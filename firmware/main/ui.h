#pragma once

#include "api_client.h"
#include "esp_err.h"

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
    UI_EVT_DISMISS,          /* "scan again" / flash tapped / back */
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    api_action_t action;
    int product_id;
    char text[API_NAME_LEN];
} ui_event_t;

typedef void (*ui_event_cb_t)(const ui_event_t *evt);

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
