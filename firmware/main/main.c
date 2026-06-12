/* App task: boot flow + the single state machine that ties GM67 scans,
 * UI events and API calls together. The GM67 reader task and the LVGL
 * task only post into s_queue; every API call happens here so the UI
 * never blocks on HTTP. */

#include "api_client.h"
#include "board.h"
#include "display.h"
#include "gm67.h"
#include "storage.h"
#include "ui.h"
#include "wifi_conn.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "main";

#define SCAN_DEBOUNCE_MS      2500
#define WIFI_TIMEOUT_MS       20000
#define FLASH_DWELL_MS        2200   /* matches the design's auto-reset */
#define SCREEN_TIMEOUT_MS     45000  /* any non-idle screen falls back to idle */
#define FACTORY_RESET_HOLD_MS 5000

typedef enum {
    APP_EVT_SCAN,
    APP_EVT_UI,
    APP_EVT_TIMEOUT,
} app_event_kind_t;

typedef struct {
    app_event_kind_t kind;
    union {
        char barcode[GM67_MAX_CODE_LEN];
        ui_event_t ui;
        uint32_t generation; /* APP_EVT_TIMEOUT: arm-time generation */
    };
} app_event_t;

typedef enum {
    APP_IDLE,
    APP_PRODUCT,    /* s_product valid */
    APP_FLASH,      /* action confirmed, waiting for dwell timeout */
    APP_NOT_FOUND,  /* s_scan valid (status UNKNOWN) */
    APP_PROPOSAL,   /* create-product proposal, s_scan valid */
    APP_SEARCH,     /* product search, s_scan valid */
    APP_ERROR,      /* error screen, tap dismisses */
} app_state_t;

static QueueHandle_t s_queue;
static esp_timer_handle_t s_timeout_timer;
static uint32_t s_generation; /* bumped on every state change; stale timeouts are dropped */
static uint32_t s_timer_generation; /* generation captured when the timeout timer was armed */

static app_state_t s_state = APP_IDLE;
static api_product_t s_product;
static api_scan_result_t s_scan;

/* ------------------------------------------------------------------ */
/* Producers (other tasks)                                             */
/* ------------------------------------------------------------------ */

static void on_scan(const char *code)
{
    app_event_t evt = { .kind = APP_EVT_SCAN };
    strlcpy(evt.barcode, code, sizeof(evt.barcode));
    xQueueSend(s_queue, &evt, 0);
}

static void on_ui_event(const ui_event_t *ui_evt)
{
    app_event_t evt = { .kind = APP_EVT_UI, .ui = *ui_evt };
    xQueueSend(s_queue, &evt, 0);
}

static void on_timeout(void *arg)
{
    (void)arg;
    /* Report the generation captured when this timer was armed, not the live
     * one: a transition may have bumped s_generation just before this fires. */
    app_event_t evt = { .kind = APP_EVT_TIMEOUT, .generation = s_timer_generation };
    xQueueSend(s_queue, &evt, 0);
}

/* ------------------------------------------------------------------ */
/* State transitions                                                   */
/* ------------------------------------------------------------------ */

static void enter_state(app_state_t state, uint32_t timeout_ms)
{
    s_state = state;
    s_generation++;
    esp_timer_stop(s_timeout_timer);
    if (timeout_ms > 0) {
        s_timer_generation = s_generation;
        esp_timer_start_once(s_timeout_timer, (uint64_t)timeout_ms * 1000);
    }
}

static void go_idle(void)
{
    ui_show_idle();
    enter_state(APP_IDLE, 0);
}

static void show_error(const char *message)
{
    ui_show_error(message);
    enter_state(APP_ERROR, SCREEN_TIMEOUT_MS);
}

static void show_product(const api_product_t *product)
{
    s_product = *product;
    ui_set_last_scan(product->name);
    ui_show_product(product);
    enter_state(APP_PRODUCT, SCREEN_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* Event handlers (app task)                                           */
/* ------------------------------------------------------------------ */

static void handle_scan(const char *barcode)
{
    /* A fresh scan takes over from any screen except an open keyboard:
     * losing typed input to an accidental re-scan would be worse. */
    if (s_state == APP_PROPOSAL || s_state == APP_SEARCH) {
        return;
    }

    ESP_LOGI(TAG, "scan: %s", barcode);
    ui_show_connecting("Looking up...");

    char err[API_ERR_LEN];
    esp_err_t ret = api_scan(barcode, &s_scan, err);
    if (ret != ESP_OK) {
        show_error(err);
        return;
    }
    if (s_scan.status == API_SCAN_FOUND) {
        show_product(&s_scan.product);
    } else {
        ui_show_not_found(&s_scan);
        enter_state(APP_NOT_FOUND, SCREEN_TIMEOUT_MS);
    }
}

static void handle_action(api_action_t action)
{
    ui_show_saving();

    char err[API_ERR_LEN];
    api_action_result_t result;
    esp_err_t ret = api_action(s_product.id, action, &result, err);
    if (ret != ESP_OK) {
        show_error(err); /* incl. 409 insufficient stock with server text */
        return;
    }
    ui_set_last_scan(result.product_name);
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

static void handle_ui(const ui_event_t *evt)
{
    switch (evt->type) {
    case UI_EVT_ACTION_TILE:
        if (s_state == APP_PRODUCT) {
            handle_action(evt->action);
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
        ui_show_search();
        enter_state(APP_SEARCH, 2 * SCREEN_TIMEOUT_MS);
        break;
    case UI_EVT_SEARCH_QUERY:
        handle_search_query(evt->text);
        break;
    case UI_EVT_SEARCH_PICK:
        handle_link(evt->product_id);
        break;
    case UI_EVT_DISMISS:
        go_idle();
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Housekeeping tasks/timers                                           */
/* ------------------------------------------------------------------ */

static void status_tick(void *arg)
{
    (void)arg;
    ui_set_connected(wifi_conn_is_connected());
}

/* Long-press BOOT (5 s) = factory reset. Polling at 100 ms is plenty. */
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
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void start_sntp(void)
{
    /* Status-bar clock. Europe/Amsterdam with DST rules. */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    esp_netif_sntp_init(&cfg);
}

/* ------------------------------------------------------------------ */
/* Boot                                                                */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_ERROR_CHECK(storage_init());

    app_config_t cfg;
    ESP_ERROR_CHECK(storage_load(&cfg));

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init(on_ui_event));

    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_BOOT_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_cfg);
    xTaskCreate(reset_button_task, "reset_btn", 2048, NULL, 2, NULL);

    if (!storage_is_provisioned(&cfg)) {
        char ap_ssid[16];
        ui_show_connecting("Starting setup...");
        ESP_ERROR_CHECK(wifi_prov_run(&cfg, ap_ssid, sizeof(ap_ssid)));
        ui_show_provisioning(ap_ssid, cfg.ap_pass);
        /* The portal's POST handler saves the config and reboots. */
        vTaskDelay(portMAX_DELAY);
    }

    ui_show_connecting("Connecting to WiFi...");
    esp_err_t ret = wifi_conn_start(&cfg, WIFI_TIMEOUT_MS);
    if (ret != ESP_OK) {
        /* Auto-reconnect keeps trying in the background; tell the user and
         * fall through to idle so a later reconnect just works. */
        ESP_LOGW(TAG, "wifi connect timed out, continuing");
    }
    ui_set_connected(wifi_conn_is_connected());
    start_sntp();

    api_client_init(&cfg);

    s_queue = xQueueCreate(8, sizeof(app_event_t));
    configASSERT(s_queue != NULL);

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

    char err[API_ERR_LEN];
    if (wifi_conn_is_connected() && api_ping(err) != ESP_OK) {
        ESP_LOGW(TAG, "api ping failed: %s", err);
        show_error(err);
    } else {
        go_idle();
    }

    ESP_ERROR_CHECK(gm67_init(on_scan, SCAN_DEBOUNCE_MS));

    /* Display up, WiFi attempted, scanner task running: this image works.
     * Without this an OTA-installed build rolls back on next reboot. */
    esp_ota_mark_app_valid_cancel_rollback();

    app_event_t evt;
    while (true) {
        if (xQueueReceive(s_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt.kind) {
        case APP_EVT_SCAN:
            handle_scan(evt.barcode);
            break;
        case APP_EVT_UI:
            handle_ui(&evt.ui);
            break;
        case APP_EVT_TIMEOUT:
            if (evt.generation == s_generation && s_state != APP_IDLE) {
                go_idle();
            }
            break;
        }
    }
}
