#pragma once

#include "api_client.h"
#include "esp_err.h"
#include <stdint.h>

/* Events the UI raises toward the app task. Handlers run in the LVGL task —
 * post to a queue and return. */
typedef enum {
    UI_EVT_ACTION_TILE,      /* arg.action: tile tapped on the product screen */
    UI_EVT_LINK_SUGGESTION,  /* arg.product_id */
    UI_EVT_OPEN_PROPOSAL,    /* "create <name>" chosen on the not-found screen */
    UI_EVT_PROPOSAL_CONFIRM, /* arg.text: confirmed (possibly edited) name */
    UI_EVT_OPEN_SEARCH,
    UI_EVT_SEARCH_QUERY,     /* arg.text */
    UI_EVT_SEARCH_PICK,      /* arg.product_id */
    UI_EVT_DISMISS,          /* "scan again" / flash tapped / back to idle */
    UI_EVT_OPEN_SETTINGS,    /* gear tapped on the idle status bar */
    UI_EVT_CYCLE_BEEP,           /* scanner-beep row tapped on the settings screen */
    UI_EVT_TOGGLE_LIGHT,         /* status-light row tapped on the settings screen */
    UI_EVT_TOGGLE_LANGUAGE,      /* language row tapped on the settings screen */
    UI_EVT_CYCLE_TIMEOUT,        /* screen-timeout row tapped on the settings screen */
    UI_EVT_CYCLE_SCANNER_LIGHT,  /* scanner light row tapped on the settings screen */
    UI_EVT_CYCLE_COLLIMATION,    /* collimation row tapped on the settings screen */
    UI_EVT_OPEN_TOUCH_CAL,       /* touch-calibration row tapped */
    UI_EVT_CAL_TAP,          /* calibration overlay pressed; raw sample is ready */
    UI_EVT_CAL_RELEASE,      /* calibration overlay released; arm next sample */
    UI_EVT_SLEEP,            /* touch inactivity exceeded the configured timeout */
    UI_EVT_WAKE,             /* user touched the sleep overlay — device should wake */
    UI_EVT_OTA_ACCEPT,       /* "Update" tapped on the OTA-available screen */
    UI_EVT_OTA_SKIP,         /* "Later" tapped on the OTA-available screen */
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    api_action_t action;
    int product_id;
    char text[API_NAME_LEN];
} ui_event_t;

typedef bool (*ui_event_cb_t)(const ui_event_t *evt);

esp_err_t ui_init(ui_event_cb_t cb);

/* Status bar (every screen): connection dot + label, clock from time(). */
void ui_set_connected(bool connected);

/* Screens. Safe to call from any task (locks LVGL internally). */
void ui_show_provisioning(const char *ap_ssid, const char *ap_pass);
void ui_show_connecting(const char *message);
void ui_show_idle(void);
void ui_set_last_scan(const char *name); /* idle footer, remembers time */
void ui_show_product(const api_product_t *product);
void ui_show_saving(void);
void ui_show_flash(const api_action_result_t *result);
void ui_show_not_found(const api_scan_result_t *scan);
void ui_show_proposal(const char *initial_name);
void ui_show_search(void);
void ui_show_search_results(const api_search_result_t *results);
void ui_show_error(const char *message);
void ui_show_settings(uint8_t beep_level, bool light, const char *language,
                      uint32_t timeout_seconds, uint8_t scanner_light,
                      uint8_t collimation);
void ui_show_touch_calibration(void);
void ui_touch_calibration_set_target(uint8_t target_index);
void ui_show_touch_calibration_result(bool success, bool save_failed);

/* OTA: prompt the user that a newer firmware is available (versions are
 * human-readable, no "v" prefix), and a download/install progress screen.
 * ui_show_ota_progress() rebuilds the screen on the first call (percent
 * ignored for layout) and only updates the bar + label on later calls, so it
 * is cheap to call from the download progress callback. */
void ui_show_ota_available(const char *new_version, const char *current_version);
void ui_show_ota_progress(int percent);

/* Update the idle inactivity threshold used by the screen-sleep timer.
 * 0 = sleep disabled.  Safe to call from any task (no LVGL lock required). */
void ui_set_screen_timeout(uint32_t seconds);

/* Allow or suppress display sleep.  Call with true only from the idle screen;
 * false from every other screen so the sleep timer is inactive while the user
 * is on a product/search/proposal screen (barcode scans do not reset the LVGL
 * touch-inactivity counter, so those screens would sleep mid-read otherwise).
 * Safe to call from any task (no LVGL lock required). */
void ui_set_sleep_allowed(bool allowed);

/* Create the full-screen sleep overlay and force a synchronous redraw.
 * Call from the app task while holding lvgl_port_lock, after validating that
 * the app is still in APP_IDLE when UI_EVT_SLEEP is dequeued. */
void ui_show_sleep_visual(void);

/* Cancel a pending sleep: reset the s_asleep flag so idle_tick can re-arm.
 * Call from the app task under lvgl_port_lock when UI_EVT_SLEEP is discarded
 * because the app state changed before the event was processed. */
void ui_cancel_sleep(void);
