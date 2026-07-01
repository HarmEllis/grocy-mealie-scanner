/* App task: boot flow + the single state machine that ties GM67 scans,
 * UI events and API calls together. The GM67 reader task and the LVGL
 * task only post into s_queue; every API call happens here so the UI
 * never blocks on HTTP. */

#include "api_client.h"
#include "board.h"
#include "display.h"
#include "gm67.h"
#include "i18n.h"
#if !CONFIG_GMS_DEMO_MODE
#include "ota_update.h"
#endif
#include "provision_qr.h"
#include "status_led.h"
#include "storage.h"
#include "touch_calibration.h"
#include "ui.h"
#include "ui_fonts.h"
#include "wifi_conn.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_phy_init.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "main";

#define SCAN_DEBOUNCE_MS      2500
#define WIFI_TIMEOUT_MS       20000
#define FLASH_DWELL_MS        2200   /* matches the design's auto-reset */
#define SCREEN_TIMEOUT_MS     45000  /* any non-idle screen falls back to idle */
#define CONN_RETRY_MS         5000   /* connection-error screen auto-retry cadence */
#define FACTORY_RESET_HOLD_MS 5000
#define TOUCH_CAL_RESULT_MS   1500
#define SCAN_FAST_RETRY_MS    2500
#define SCAN_RETRY_DELAY_1_MS 300
#define SCAN_RETRY_DELAY_2_MS 800

typedef enum {
    APP_EVT_SCAN,
    APP_EVT_UI,
    APP_EVT_TIMEOUT,
    APP_EVT_OTA_CHECK,  /* boot/periodic timer asks for a GitHub release check */
} app_event_kind_t;

typedef struct {
    app_event_kind_t kind;
    union {
        char barcode[GM67_MAX_CODE_LEN];
        ui_event_t ui;
    };
} app_event_t;

typedef enum {
    APP_IDLE,
    APP_PRODUCT,    /* s_product valid */
    APP_ACTION_CONFIRM, /* quantity picker for a tapped action; s_product valid */
    APP_FLASH,      /* action confirmed, waiting for dwell timeout */
    APP_NOT_FOUND,  /* s_scan valid (status UNKNOWN) */
    APP_PROPOSAL,   /* create-product proposal, s_scan valid */
    APP_SEARCH,     /* product search, s_scan valid */
    APP_ERROR,      /* error screen, tap dismisses */
    APP_CONN_ERROR, /* not connected to the API; tap/timer retries, never idles */
    APP_SETTINGS,   /* on-device settings (feedback and language) */
    APP_TOUCH_CAL,  /* four-point touch calibration or its result message */
    APP_SLEEP,      /* display sleeping; only UI_EVT_WAKE transitions out */
    APP_OTA_PROMPT, /* update-available prompt; accept/skip or times out */
    APP_OTA_PROGRESS, /* download/install in progress (app task blocks here) */
} app_state_t;

static QueueHandle_t s_queue;
static esp_timer_handle_t s_timeout_timer;
/* Absolute deadline (esp_timer_get_time, us) of the current screen's timeout,
 * or UINT64_MAX when none. Written and read only in the app task, so a stale
 * timer callback that races a state change cannot dismiss the new screen:
 * it just re-checks the deadline, which the new screen has pushed forward. */
static uint64_t s_timeout_deadline = UINT64_MAX;

static app_state_t s_state = APP_IDLE;
static api_product_t s_product;
static api_scan_result_t s_scan;
/* How the active product-search session was opened. true = from the idle screen
 * (no barcode in flight): a pick fetches the product and shows it as if scanned.
 * false = from the unknown-barcode flow: a pick links s_scan.barcode instead. */
static bool s_search_from_idle;
/* Loaded once at boot; the settings screen mutates feedback and language and
 * persists them, so it lives at file scope rather than on app_main's stack. */
static app_config_t s_cfg;
static touch_cal_sample_t s_touch_cal_samples[TOUCH_CAL_TARGET_COUNT];
static uint8_t s_touch_cal_index;
static bool s_touch_cal_result_visible;
/* Set true while the display is sleeping; on_scan checks this to drop codes
 * while the screen is off.  Written by the app task, read by the GM67 task. */
static _Atomic bool s_display_asleep = false;

#if !CONFIG_GMS_DEMO_MODE
/* Result of the most recent GitHub release check; the tag is reused when the
 * user accepts the update.  App task only. */
static ota_check_result_t s_ota_result;
#endif

/* ------------------------------------------------------------------ */
/* Producers (other tasks)                                             */
/* ------------------------------------------------------------------ */

static void on_scan(const char *code)
{
    /* Drop scans while the display is sleeping.  gm67_set_scanning() gates at
     * submit_code(); this is a second guard in the app task for defence in depth. */
    if (atomic_load(&s_display_asleep)) {
        return;
    }
    app_event_t evt = { .kind = APP_EVT_SCAN };
    strlcpy(evt.barcode, code, sizeof(evt.barcode));
    xQueueSend(s_queue, &evt, 0);
}

static bool normalize_qr_country(char *cc)
{
    size_t n = strlen(cc);
    if (n == 0) return true;
    if (n < 2 || n > 3) return false;
    for (size_t i = 0; i < n; i++) {
        if (cc[i] >= 'a' && cc[i] <= 'z') cc[i] -= 'a' - 'A';
        else if (!((cc[i] >= 'A' && cc[i] <= 'Z') ||
                   (cc[i] >= '0' && cc[i] <= '9'))) return false;
    }
    return true;
}

static bool apply_provision_qr(const char *code)
{
    if (!provision_qr_is_payload(code)) return false;
    provision_qr_config_t qr;
    if (!provision_qr_parse(code, &qr) || !qr.wifi_ssid[0] || !qr.api_url[0] ||
        !i18n_language_is_supported(qr.language) ||
        !normalize_qr_country(qr.wifi_country) ||
        (strncmp(qr.api_url, "http://", 7) != 0 &&
         strncmp(qr.api_url, "https://", 8) != 0)) {
        ESP_LOGW(TAG, "invalid setup QR rejected");
        ui_show_connecting(tr("setup_qr_invalid"));
        return true;
    }
    size_t n = strlen(qr.api_url);
    while (n > 0 && qr.api_url[n - 1] == '/') qr.api_url[--n] = '\0';
    app_config_t cfg = s_cfg;
    strlcpy(cfg.wifi_ssid, qr.wifi_ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, qr.wifi_pass, sizeof(cfg.wifi_pass));
    strlcpy(cfg.api_url, qr.api_url, sizeof(cfg.api_url));
    strlcpy(cfg.api_token, qr.api_token, sizeof(cfg.api_token));
    strlcpy(cfg.wifi_country, qr.wifi_country, sizeof(cfg.wifi_country));
    strlcpy(cfg.language, qr.language, sizeof(cfg.language));
    cfg.api_insecure = qr.api_insecure;
    if (storage_save(&cfg) != ESP_OK) {
        ui_show_connecting(tr("setup_qr_save_failed"));
        return true;
    }
    s_cfg = cfg;
    ESP_LOGI(TAG, "setup QR applied; rebooting");
    ui_show_connecting(i18n_tr_for("saved", cfg.language));
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return true;
}

static void on_setup_scan(const char *code)
{
    if (!apply_provision_qr(code))
        ESP_LOGI(TAG, "ignoring product barcode during setup");
}

static bool on_ui_event(const ui_event_t *ui_evt)
{
    app_event_t evt = { .kind = APP_EVT_UI, .ui = *ui_evt };
    return xQueueSend(s_queue, &evt, 0) == pdTRUE;
}

static void on_timeout(void *arg)
{
    (void)arg;
    /* Just nudge the app task; it validates against s_timeout_deadline so a
     * stale fire that races a state change is harmless. */
    app_event_t evt = { .kind = APP_EVT_TIMEOUT };
    xQueueSend(s_queue, &evt, 0);
}

#if !CONFIG_GMS_DEMO_MODE
/* Boot + periodic OTA timers post here; the check itself runs in the app task
 * (handle_ota_check) so it never races the state machine or blocks a timer. */
static void on_ota_check_timer(void *arg)
{
    (void)arg;
    app_event_t evt = { .kind = APP_EVT_OTA_CHECK };
    xQueueSend(s_queue, &evt, 0);
}
#endif

/* ------------------------------------------------------------------ */
/* State transitions                                                   */
/* ------------------------------------------------------------------ */

static void enter_state(app_state_t state, uint32_t timeout_ms)
{
    s_state = state;
    /* Allow display sleep only while idle; every other screen blocks sleep
     * because scans arrive via UART and do not reset LVGL touch-inactivity. */
    ui_set_sleep_allowed(state == APP_IDLE);
    esp_timer_stop(s_timeout_timer);
    if (timeout_ms > 0) {
        s_timeout_deadline = esp_timer_get_time() + (uint64_t)timeout_ms * 1000;
        esp_timer_start_once(s_timeout_timer, (uint64_t)timeout_ms * 1000);
    } else {
        s_timeout_deadline = UINT64_MAX;
    }
}

static void go_idle(void)
{
    /* Only offer on-device product search when the connected server advertises
     * the products/{id} capability (apiVersion >= 2); otherwise the affordance
     * would lead to a 404. Refreshed from the last ping on every return to idle. */
    ui_set_search_available(api_server_api_version() >= 2);
    ui_show_idle();
    enter_state(APP_IDLE, 0);
}

static void show_settings(void)
{
    ui_show_settings(s_cfg.beep_level, s_cfg.light_enabled, s_cfg.language,
                     s_cfg.screen_timeout_seconds, s_cfg.scanner_light,
                     s_cfg.collimation, s_cfg.wifi_power_save);
    enter_state(APP_SETTINGS, SCREEN_TIMEOUT_MS);
}

static void show_error(const char *message)
{
    status_led_flash(STATUS_LED_CORAL);
    ui_show_error(message);
    enter_state(APP_ERROR, SCREEN_TIMEOUT_MS);
}

static void show_connection_error(const char *message)
{
    status_led_flash(STATUS_LED_CORAL);
    /* Park the scanner while we are not connected: a scan is useless without the
     * API, and a lit aiming light on the error screen is misleading. The gate
     * reopens in try_connect() once the API answers. (At boot this runs before
     * gm67_init(), which then honours the closed gate; see gm67_task.) */
    gm67_set_scanning(false);
    ui_show_connection_error(message);
    /* Re-arm the screen timer as an auto-retry tick rather than a fall-to-idle. */
    enter_state(APP_CONN_ERROR, CONN_RETRY_MS);
}

/* Gate to the idle screen: only reach idle once WiFi is up and the API answers.
 * Re-run on boot, on a tap, and on the auto-retry tick so the user never sees a
 * fake "connected" idle screen. api_ping() blocks up to HTTP_TIMEOUT_MS, so show
 * a spinner first (the LVGL task keeps drawing while this app task blocks). */
static void try_connect(void)
{
    char err[API_ERR_LEN];
    if (!wifi_conn_is_connected()) {
        strlcpy(err, tr("wifi_disconnected"), sizeof(err));
        show_connection_error(err);
        return;
    }
    ui_show_connecting(tr("reconnecting"));
    if (api_ping(err) == ESP_OK) {
        /* Online again: reopen the scanner gate that show_connection_error()
         * closed before returning to idle. */
        gm67_set_scanning(true);
        go_idle();
    } else {
        show_connection_error(err);
    }
}

static void show_product(const api_product_t *product)
{
    status_led_flash(STATUS_LED_GREEN);
    s_product = *product;
    ui_set_last_scan(product->name, product->id);
    ui_show_product(product);
    enter_state(APP_PRODUCT, SCREEN_TIMEOUT_MS);
}

#if !CONFIG_GMS_DEMO_MODE
/* Runs in the app task while ota_perform_update blocks; just forwards the
 * download percentage to the progress screen (which updates in place). */
static void ota_progress_cb(int percent)
{
    ui_show_ota_progress(percent);
}

/* Query GitHub for a newer release.  Only surfaces the prompt when idle and
 * online; any error (network, clock-not-synced, parse) is silent and retried
 * on the next periodic tick. */
static void handle_ota_check(void)
{
    if (s_state != APP_IDLE || !wifi_conn_is_connected()) {
        return;
    }
    if (ota_check_for_update(&s_ota_result) != ESP_OK || !s_ota_result.available) {
        return;
    }
    ESP_LOGI(TAG, "OTA available: %s -> %s", s_ota_result.current_version,
             s_ota_result.new_version);
    ui_show_ota_available(s_ota_result.new_version, s_ota_result.current_version);
    /* Give the user a generous window before falling back to idle. */
    enter_state(APP_OTA_PROMPT, 2 * SCREEN_TIMEOUT_MS);
}

/* User accepted: download + install on the inactive slot, then reboot into it.
 * Blocks the app task for the duration; the progress screen is driven by the
 * callback.  On failure the running image is untouched. */
static void handle_ota_accept(void)
{
    ui_show_ota_progress(0);
    enter_state(APP_OTA_PROGRESS, 0); /* no timeout: the download owns the task */

    esp_err_t err = ota_perform_update(s_ota_result.tag, ota_progress_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
        show_error(tr("ota_failed"));
        return;
    }
    ui_show_connecting(tr("ota_restarting"));
    vTaskDelay(pdMS_TO_TICKS(800)); /* let the message render before reboot */
    esp_restart();
}

/* Manual "check for updates" from the settings screen.  Unlike the silent
 * periodic handle_ota_check, every outcome is surfaced so the user gets
 * feedback: the update prompt, an "up to date" screen, or an error. */
static void handle_ota_check_manual(void)
{
    if (!wifi_conn_is_connected()) {
        show_error(tr("ota_check_failed"));
        return;
    }
    ui_show_connecting(tr("ota_checking")); /* check blocks the app task ~seconds */
    if (ota_check_for_update(&s_ota_result) != ESP_OK) {
        show_error(tr("ota_check_failed"));
        return;
    }
    if (s_ota_result.available) {
        ui_show_ota_available(s_ota_result.new_version, s_ota_result.current_version);
        enter_state(APP_OTA_PROMPT, 2 * SCREEN_TIMEOUT_MS);
    } else {
        ui_show_ota_up_to_date(s_ota_result.current_version);
        enter_state(APP_ERROR, SCREEN_TIMEOUT_MS); /* tap/timeout returns to idle */
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Event handlers (app task)                                           */
/* ------------------------------------------------------------------ */

static void handle_scan(const char *barcode)
{
    if (apply_provision_qr(barcode)) return;
    /* A fresh scan takes over from any screen except an open keyboard or the
     * quantity picker: losing typed input / a half-chosen amount to an
     * accidental re-scan would be worse. */
    if (s_state == APP_PROPOSAL || s_state == APP_SEARCH ||
        s_state == APP_TOUCH_CAL || s_state == APP_OTA_PROMPT ||
        s_state == APP_OTA_PROGRESS || s_state == APP_CONN_ERROR ||
        s_state == APP_ACTION_CONFIRM) {
        return;
    }

    ESP_LOGI(TAG, "scan: %s", barcode);
#if !CONFIG_GMS_DEMO_MODE
    if (!wifi_conn_is_connected()) {
        show_connection_error(tr("wifi_disconnected"));
        return;
    }
#endif
    ui_show_connecting(tr("looking_up"));

    char err[API_ERR_LEN];
    esp_err_t ret = ESP_FAIL;
    bool last_transport_error = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        int64_t started = esp_timer_get_time();
        bool wifi_connected = wifi_conn_is_connected();
        ret = api_scan(barcode, &s_scan, err);
        int elapsed_ms = (int)((esp_timer_get_time() - started) / 1000);
        int status = api_last_http_status();
        last_transport_error = api_error_is_transport(ret);
        ESP_LOGI(TAG, "scan attempt %d: ret=%s status=%d elapsed=%dms wifi=%s",
                 attempt, esp_err_to_name(ret), status, elapsed_ms,
                 wifi_connected ? "connected" : "disconnected");

        if (ret == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "scan succeeded on attempt %d", attempt);
            }
            break;
        }
        if (!last_transport_error || elapsed_ms >= SCAN_FAST_RETRY_MS || attempt == 3) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(attempt == 1 ? SCAN_RETRY_DELAY_1_MS
                                              : SCAN_RETRY_DELAY_2_MS));
    }
    if (ret != ESP_OK) {
        if (last_transport_error) {
            show_connection_error(err);
        } else {
            show_error(err);
        }
        return;
    }
    if (s_scan.status == API_SCAN_FOUND) {
        show_product(&s_scan.product);
    } else {
        status_led_flash(STATUS_LED_AMBER);
        ui_show_not_found(&s_scan);
        enter_state(APP_NOT_FOUND, SCREEN_TIMEOUT_MS);
    }
}

static void handle_action(api_action_t action, int amount)
{
    ui_show_saving();

    char err[API_ERR_LEN];
    api_action_result_t result;
    esp_err_t ret = api_action(s_product.id, action, amount, &result, err);
    if (ret != ESP_OK) {
        show_error(err); /* incl. 409 insufficient stock with server text */
        return;
    }
    ui_set_last_scan(result.product_name, s_product.id);
    ui_show_flash(&result);
    enter_state(APP_FLASH, FLASH_DWELL_MS);
}

static void handle_link(int product_id)
{
    ui_show_saving();

    char err[API_ERR_LEN];
    api_product_t product;
    esp_err_t ret = api_link_barcode(product_id, s_scan.barcode, &product, err);
    if (ret != ESP_OK) {
        show_error(err);
        return;
    }
    show_product(&product);
}

/* Home-screen search pick: no barcode to link, just fetch the full product and
 * show it exactly as a scan would. */
static void handle_pick_product(int product_id)
{
    ui_show_loading();

    char err[API_ERR_LEN];
    api_product_t product;
    esp_err_t ret = api_get_product(product_id, &product, err);
    if (ret != ESP_OK) {
        show_error(err);
        return;
    }
    show_product(&product);
}

static void handle_create(const char *name)
{
    if (name[0] == '\0') {
        return; /* keyboard confirmed an empty name; stay on the proposal */
    }
    ui_show_saving();

    char err[API_ERR_LEN];
    api_product_t product;
    esp_err_t ret = api_create_product(name, s_scan.barcode, &product, err);
    if (ret != ESP_OK) {
        show_error(err); /* incl. 409 duplicate name */
        return;
    }
    show_product(&product);
}

static void handle_search_query(const char *query)
{
    char err[API_ERR_LEN];
    api_search_result_t results;
    esp_err_t ret = api_search(query, &results, err);
    if (ret != ESP_OK) {
        show_error(err);
        return;
    }
    ui_show_search_results(&results);
    enter_state(APP_SEARCH, SCREEN_TIMEOUT_MS);
}

static void finish_touch_calibration(void)
{
    int32_t x_left = (s_touch_cal_samples[0].x + s_touch_cal_samples[2].x) / 2;
    int32_t x_right = (s_touch_cal_samples[1].x + s_touch_cal_samples[3].x) / 2;
    int32_t y_top = (s_touch_cal_samples[0].y + s_touch_cal_samples[1].y) / 2;
    int32_t y_bottom = (s_touch_cal_samples[2].y + s_touch_cal_samples[3].y) / 2;

    display_touch_capture(false);
    s_touch_cal_result_visible = true;
    if (abs(x_right - x_left) <= TOUCH_CAL_MIN_SPAN ||
        abs(y_bottom - y_top) <= TOUCH_CAL_MIN_SPAN) {
        ui_show_touch_calibration_result(false, false);
        enter_state(APP_TOUCH_CAL, TOUCH_CAL_RESULT_MS);
        return;
    }

    app_config_t updated = s_cfg;
    updated.touch_cal_x_left = (uint32_t)x_left;
    updated.touch_cal_x_right = (uint32_t)x_right;
    updated.touch_cal_y_top = (uint32_t)y_top;
    updated.touch_cal_y_bottom = (uint32_t)y_bottom;
    if (storage_save_touch_cal(&updated) != ESP_OK) {
        ui_show_touch_calibration_result(false, true);
        enter_state(APP_TOUCH_CAL, TOUCH_CAL_RESULT_MS);
        return;
    }

    s_cfg = updated;
    bool applied = display_touch_set_calibration(x_left, x_right, y_top, y_bottom);
    configASSERT(applied);
    ui_show_touch_calibration_result(true, false);
    enter_state(APP_TOUCH_CAL, TOUCH_CAL_RESULT_MS);
}

static void handle_ui(const ui_event_t *evt)
{
    switch (evt->type) {
    case UI_EVT_ACTION_TILE:
        if (s_state == APP_PRODUCT) {
            ui_show_action_confirm(evt->action, &s_product);
            enter_state(APP_ACTION_CONFIRM, SCREEN_TIMEOUT_MS);
        }
        break;
    case UI_EVT_ACTION_CONFIRM:
        if (s_state == APP_ACTION_CONFIRM) {
            handle_action(evt->action, evt->amount);
        }
        break;
    case UI_EVT_LINK_SUGGESTION:
        handle_link(evt->product_id);
        break;
    case UI_EVT_OPEN_PROPOSAL:
        ui_show_proposal(s_scan.has_external_name ? s_scan.external_name : "");
        enter_state(APP_PROPOSAL, 2 * SCREEN_TIMEOUT_MS); /* typing takes time */
        break;
    case UI_EVT_PROPOSAL_CONFIRM:
        handle_create(evt->text);
        break;
    case UI_EVT_OPEN_SEARCH:
        /* Remember whether search was opened from idle (show on pick) or from the
         * unknown-barcode flow (link s_scan.barcode on pick). */
        s_search_from_idle = (s_state == APP_IDLE);
        ui_show_search();
        enter_state(APP_SEARCH, 2 * SCREEN_TIMEOUT_MS);
        break;
    case UI_EVT_SEARCH_QUERY:
        handle_search_query(evt->text);
        break;
    case UI_EVT_SEARCH_PICK:
        if (s_search_from_idle) {
            handle_pick_product(evt->product_id);
        } else {
            handle_link(evt->product_id);
        }
        break;
    case UI_EVT_LAST_SCAN_TAP:
        if (s_state == APP_IDLE) {
            handle_pick_product(evt->product_id);
        }
        break;
    case UI_EVT_OPEN_SETTINGS:
        show_settings();
        break;
    case UI_EVT_CYCLE_BEEP:
        s_cfg.beep_level = (s_cfg.beep_level + 1) % 4;
        storage_save_settings(&s_cfg);
        gm67_set_beep_level((gm67_beep_level_t)s_cfg.beep_level);
        show_settings();
        break;
    case UI_EVT_TOGGLE_LIGHT:
        s_cfg.light_enabled = !s_cfg.light_enabled;
        storage_save_settings(&s_cfg);
        status_led_set_enabled(s_cfg.light_enabled);
        show_settings();
        break;
    case UI_EVT_TOGGLE_WIFI_PS:
        s_cfg.wifi_power_save = !s_cfg.wifi_power_save;
        storage_save_settings(&s_cfg);
        wifi_conn_set_power_save(s_cfg.wifi_power_save);
        show_settings();
        break;
    case UI_EVT_TOGGLE_LANGUAGE:
        strlcpy(s_cfg.language, strcmp(s_cfg.language, "nl") == 0 ? "en" : "nl",
                sizeof(s_cfg.language));
        storage_save_settings(&s_cfg);
        lvgl_port_lock(0);
        i18n_set_language(s_cfg.language);
        lvgl_port_unlock();
        show_settings();
        break;
    case UI_EVT_CYCLE_TIMEOUT: {
        /* Cycle presets: 30 → 60 → 120 → 300 → 0 (Never) → 30 → … */
        static const uint32_t presets[] = { 30, 60, 120, 300, 0 };
        static const size_t n_presets = sizeof(presets) / sizeof(presets[0]);
        size_t idx = 0;
        for (size_t i = 0; i < n_presets; i++) {
            if (presets[i] == s_cfg.screen_timeout_seconds) {
                idx = (i + 1) % n_presets;
                break;
            }
        }
        s_cfg.screen_timeout_seconds = presets[idx];
        storage_save_settings(&s_cfg);
        ui_set_screen_timeout(s_cfg.screen_timeout_seconds); /* immediate effect */
        show_settings();
        break;
    }
    case UI_EVT_CYCLE_SCANNER_LIGHT:
        s_cfg.scanner_light = (s_cfg.scanner_light + 1) % 2;
        storage_save_settings(&s_cfg);
        gm67_set_scanner_light((gm67_light_mode_t)s_cfg.scanner_light);
        show_settings();
        break;
    case UI_EVT_CYCLE_COLLIMATION:
        s_cfg.collimation = (s_cfg.collimation + 1) % 2;
        storage_save_settings(&s_cfg);
        gm67_set_collimation((gm67_collim_mode_t)s_cfg.collimation);
        show_settings();
        break;
    case UI_EVT_OPEN_TOUCH_CAL:
        if (s_state == APP_SETTINGS) {
            s_touch_cal_index = 0;
            s_touch_cal_result_visible = false;
            display_touch_capture(true);
            ui_show_touch_calibration();
            enter_state(APP_TOUCH_CAL, 2 * SCREEN_TIMEOUT_MS);
        }
        break;
    case UI_EVT_CAL_TAP:
        if (s_state == APP_TOUCH_CAL && !s_touch_cal_result_visible &&
            s_touch_cal_index < TOUCH_CAL_TARGET_COUNT) {
            display_touch_capture(false);
            touch_cal_sample_t *sample = &s_touch_cal_samples[s_touch_cal_index];
            if (!display_touch_get_sample(&sample->x, &sample->y)) {
                break;
            }
            s_touch_cal_index++;
            if (s_touch_cal_index == TOUCH_CAL_TARGET_COUNT) {
                finish_touch_calibration();
            } else {
                ui_touch_calibration_set_target(s_touch_cal_index);
            }
        }
        break;
    case UI_EVT_CAL_RELEASE:
        if (s_state == APP_TOUCH_CAL && !s_touch_cal_result_visible &&
            s_touch_cal_index < TOUCH_CAL_TARGET_COUNT) {
            display_touch_capture(true);
        }
        break;
    case UI_EVT_SLEEP:
        if (s_state != APP_IDLE) {
            /* Race: a scan arrived and changed state before we dequeued the
             * sleep event.  Discard the sleep and reset idle_tick so it can
             * re-arm the next time we return to APP_IDLE. */
            lvgl_port_lock(0);
            ui_cancel_sleep();
            lvgl_port_unlock();
            break;
        }
        gm67_set_scanning(false);
        atomic_store(&s_display_asleep, true);
        enter_state(APP_SLEEP, 0);
        lvgl_port_lock(0);
        ui_show_sleep_visual();
        lvgl_port_unlock();
        display_sleep(true);
        break;
    case UI_EVT_WAKE:
        display_sleep(false);
        if (gm67_set_scanning(true) != ESP_OK) {
            ESP_LOGW(TAG, "wake: SCAN_ENABLE not queued; scanner may stay idle");
        }
        atomic_store(&s_display_asleep, false);
        go_idle();
        break;
    case UI_EVT_DISMISS:
        if (s_state == APP_TOUCH_CAL) {
            display_touch_capture(false);
            s_touch_cal_result_visible = false;
            show_settings();
        } else if (s_state == APP_CONN_ERROR) {
            try_connect(); /* tap = retry now; stays on error if still down */
        } else if (s_state == APP_ACTION_CONFIRM) {
            show_product(&s_product); /* back chevron: return to the product */
        } else {
            go_idle();
        }
        break;
    case UI_EVT_OPEN_SETUP:
#if !CONFIG_GMS_DEMO_MODE
        /* Re-enter the provisioning portal after a reboot, keeping the rest of
         * the config (touch cal, etc.). The portal pre-fills current values. */
        ui_show_connecting(tr("opening_setup"));
        storage_request_setup();
        vTaskDelay(pdMS_TO_TICKS(600)); /* let the message render */
        esp_restart();
#endif
        break;
    case UI_EVT_RECALIBRATE_WIFI:
        /* Wipe the stored WiFi PHY calibration and reboot. The calibration blob
         * (NVS namespace "phy") is only read during PHY init at boot, so a full,
         * fresh recalibration runs on the next start. Recovers a device whose
         * stored calibration drifted into an unusable state — the TX-side
         * signature we saw: scans find APs but auth never completes and the
         * SoftAP is invisible — without a web flasher or partition tooling. */
        ui_show_connecting(tr("recalibrating_wifi"));
        esp_phy_erase_cal_data_in_nvs();
        vTaskDelay(pdMS_TO_TICKS(600)); /* let the message render */
        esp_restart();
        break;
    case UI_EVT_OTA_ACCEPT:
#if !CONFIG_GMS_DEMO_MODE
        if (s_state == APP_OTA_PROMPT) {
            handle_ota_accept();
        }
#endif
        break;
    case UI_EVT_OTA_SKIP:
#if !CONFIG_GMS_DEMO_MODE
        if (s_state == APP_OTA_PROMPT) {
            go_idle();
        }
#endif
        break;
    case UI_EVT_CHECK_UPDATE:
#if !CONFIG_GMS_DEMO_MODE
        if (s_state == APP_SETTINGS) {
            handle_ota_check_manual();
        }
#endif
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Housekeeping tasks/timers                                           */
/* ------------------------------------------------------------------ */

static void status_tick(void *arg)
{
    (void)arg;
#if CONFIG_GMS_DEMO_MODE
    ui_set_connected(true); /* no WiFi to poll; keep the bar "online" */
#else
    ui_set_connected(wifi_conn_is_connected());
#endif
}

/* BOOT button. Long-press (5 s) = factory reset on every build. A short press
 * (released before the 5 s threshold) re-enters the setup portal in a normal
 * build (to change WiFi/API without wiping everything), or injects the next
 * scenario barcode in a demo build. Polling at 100 ms is plenty. */
static void reset_button_task(void *arg)
{
    (void)arg;
    int held_ms = 0;
    while (true) {
        if (gpio_get_level(BOARD_PIN_BOOT_KEY) == 0) {
            held_ms += 100;
            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "factory reset: erasing config");
                storage_erase();
                esp_restart();
            }
        } else {
            if (held_ms > 0 && held_ms < FACTORY_RESET_HOLD_MS) {
#if CONFIG_GMS_DEMO_MODE
                on_scan(demo_next_barcode());
#else
                ESP_LOGI(TAG, "BOOT short press: entering setup");
                storage_request_setup();
                esp_restart();
#endif
            }
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

#if !CONFIG_GMS_DEMO_MODE
/* SNTP first-sync notification: fires the boot OTA check the instant the clock
 * becomes valid. Reaching this callback guarantees WiFi is up (SNTP only gets a
 * reply over a working link) and that time(NULL) is now real, so the TLS
 * handshake to GitHub sees a valid certificate window — more robust than a
 * fixed boot delay that a slow DNS/NTP response could outlast. Runs in the SNTP
 * task; only the first sync triggers a check (SNTP re-syncs roughly hourly and
 * the periodic timer, not this, owns the ongoing cadence). */
static void on_time_synced(struct timeval *tv)
{
    (void)tv;
    static atomic_bool s_boot_check_fired = false;
    if (atomic_exchange(&s_boot_check_fired, true)) {
        return;
    }
    if (s_queue != NULL) {
        app_event_t evt = { .kind = APP_EVT_OTA_CHECK };
        xQueueSend(s_queue, &evt, 0);
    }
}

static void start_sntp(void)
{
    /* Status-bar clock. Europe/Amsterdam with DST rules. */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    cfg.sync_cb = on_time_synced; /* kicks the boot OTA check once time lands */
    esp_netif_sntp_init(&cfg);
}
#endif

/* ------------------------------------------------------------------ */
/* Boot                                                                */
/* ------------------------------------------------------------------ */

static void apply_stored_touch_calibration(void)
{
    bool present = s_cfg.touch_cal_x_left != 0 ||
                   s_cfg.touch_cal_x_right != 0 ||
                   s_cfg.touch_cal_y_top != 0 ||
                   s_cfg.touch_cal_y_bottom != 0;
    bool in_range = s_cfg.touch_cal_x_left <= UINT16_MAX &&
                    s_cfg.touch_cal_x_right <= UINT16_MAX &&
                    s_cfg.touch_cal_y_top <= UINT16_MAX &&
                    s_cfg.touch_cal_y_bottom <= UINT16_MAX;
    if (!present || !in_range ||
        !display_touch_set_calibration((int32_t)s_cfg.touch_cal_x_left,
                                       (int32_t)s_cfg.touch_cal_x_right,
                                       (int32_t)s_cfg.touch_cal_y_top,
                                       (int32_t)s_cfg.touch_cal_y_bottom)) {
        display_touch_set_identity();
        if (present) {
            ESP_LOGW(TAG, "stored touch calibration is invalid; using identity");
        }
    }
}

void app_main(void)
{
    /* Enable GM67 debug logs (hex dumps for ACK diagnosis) without raising the
     * global log level, which would flood the console with SPI-master noise. */
    esp_log_level_set("gm67", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(storage_init());

    ESP_ERROR_CHECK(storage_load(&s_cfg));

    ESP_ERROR_CHECK(display_init());
    apply_stored_touch_calibration();
    lvgl_port_lock(0);
    esp_err_t ui_err = ui_fonts_init();
    if (ui_err == ESP_OK) {
        ui_err = i18n_init(s_cfg.language);
    }
    lvgl_port_unlock();
    ESP_ERROR_CHECK(ui_err);
    ESP_ERROR_CHECK(ui_init(on_ui_event));
    ui_set_screen_timeout(s_cfg.screen_timeout_seconds);

    /* Scan-result LED; the persisted toggle decides whether flashes show. A
     * failure here is non-fatal — the device works without the indicator. */
    if (status_led_init() == ESP_OK) {
        status_led_set_enabled(s_cfg.light_enabled);
    }

    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_BOOT_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_cfg);
    xTaskCreate(reset_button_task, "reset_btn", 2048, NULL, 2, NULL);

    /* Create the event queue before WiFi/SNTP start: the SNTP time-sync
     * callback (start_sntp) posts APP_EVT_OTA_CHECK here the moment the clock
     * is set, which can land before the rest of app_main finishes. */
    s_queue = xQueueCreate(8, sizeof(app_event_t));
    configASSERT(s_queue != NULL);

#if CONFIG_GMS_DEMO_MODE
    /* Demo image: no provisioning, no WiFi, no API ping. Show the bar as
     * connected and seed a plausible wall clock so the idle screen looks
     * live, then fall straight through to idle below. */
    ESP_LOGW(TAG, "DEMO MODE: skipping provisioning, WiFi and API");
    ui_set_connected(true);
    struct timeval demo_now = { .tv_sec = 1750000000 }; /* 2025-06-15 ~14:46 UTC */
    settimeofday(&demo_now, NULL);
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
#else
    /* Enter the portal when unprovisioned, or when the user asked to reconfigure
     * (settings row / short BOOT press set a flag and rebooted). Consume the flag
     * here so a power-cycle out of the portal returns to normal operation. */
    bool setup_requested = storage_take_setup_request();
    if (!storage_is_provisioned(&s_cfg) || setup_requested) {
        char ap_ssid[16];
        ui_show_connecting(tr("starting_setup"));
        /* During setup, accept versioned configuration QR payloads only. */
        ESP_ERROR_CHECK(gm67_init(on_setup_scan, SCAN_DEBOUNCE_MS));
        ESP_ERROR_CHECK(wifi_prov_run(&s_cfg, ap_ssid, sizeof(ap_ssid)));
        ui_show_provisioning(ap_ssid, s_cfg.ap_pass);
        /* The portal's POST handler saves the config and reboots. */
        vTaskDelay(portMAX_DELAY);
    }

    ui_show_connecting(tr("connecting_wifi"));
    esp_err_t ret = wifi_conn_start(&s_cfg, WIFI_TIMEOUT_MS);
    if (ret != ESP_OK) {
        /* Auto-reconnect keeps trying in the background; tell the user and
         * fall through to idle so a later reconnect just works. */
        ESP_LOGW(TAG, "wifi connect timed out, continuing");
    }
    ui_set_connected(wifi_conn_is_connected());
    start_sntp();
#endif

    api_client_init(&s_cfg);

    const esp_timer_create_args_t timeout_args = {
        .callback = on_timeout,
        .name = "screen_to",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timeout_args, &s_timeout_timer));

    const esp_timer_create_args_t status_args = {
        .callback = status_tick,
        .name = "status",
    };
    esp_timer_handle_t status_timer;
    ESP_ERROR_CHECK(esp_timer_create(&status_args, &status_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer, 5 * 1000 * 1000));

#if CONFIG_GMS_DEMO_MODE
    go_idle();
#else
    /* Gate the idle screen on a real connection: if WiFi or the API is down we
     * stay on a sticky connection-error screen that retries, rather than showing
     * a misleading "connected" idle screen. */
    try_connect();
#endif

    ESP_ERROR_CHECK(gm67_init(on_scan, SCAN_DEBOUNCE_MS));

    /* Display up, WiFi attempted, scanner task running: this image works.
     * Without this an OTA-installed build rolls back on next reboot. */
    esp_ota_mark_app_valid_cancel_rollback();

#if !CONFIG_GMS_DEMO_MODE
    /* OTA update checks. The boot check is driven by the SNTP time-sync
     * callback (on_time_synced) rather than a fixed delay, so it only runs once
     * WiFi is up and the clock is valid — exactly what the TLS handshake to
     * GitHub needs. This periodic timer then polls every
     * CONFIG_GMS_OTA_CHECK_INTERVAL_HOURS hours thereafter. */
    const esp_timer_create_args_t ota_periodic_args = {
        .callback = on_ota_check_timer,
        .name = "ota_periodic",
    };
    esp_timer_handle_t ota_periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&ota_periodic_args, &ota_periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(
        ota_periodic_timer,
        (uint64_t)CONFIG_GMS_OTA_CHECK_INTERVAL_HOURS * 3600ULL * 1000000ULL));
#endif

    app_event_t evt;
    while (true) {
        if (xQueueReceive(s_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* While sleeping, drop everything except UI_EVT_WAKE; events queued
         * in the window between idle_tick emitting and APP_SLEEP committing
         * are stale and must not drive API calls behind the dark panel. */
        if (s_state == APP_SLEEP) {
            if (evt.kind != APP_EVT_UI || evt.ui.type != UI_EVT_WAKE) {
                continue;
            }
        }
        switch (evt.kind) {
        case APP_EVT_SCAN:
            handle_scan(evt.barcode);
            break;
        case APP_EVT_UI:
            handle_ui(&evt.ui);
            break;
        case APP_EVT_TIMEOUT:
            if (s_state != APP_IDLE && esp_timer_get_time() >= s_timeout_deadline) {
                if (s_state == APP_TOUCH_CAL) {
                    display_touch_capture(false);
                    s_touch_cal_result_visible = false;
                    show_settings();
                } else if (s_state == APP_CONN_ERROR) {
                    try_connect(); /* auto-retry tick: advances to idle once online */
                } else {
                    go_idle();
                }
            }
            break;
        case APP_EVT_OTA_CHECK:
#if !CONFIG_GMS_DEMO_MODE
            handle_ota_check();
#endif
            break;
        }
    }
}
