#pragma once

#include "esp_err.h"
#include "storage.h"
#include <stdbool.h>

/* C-side mirror of docs/DEVICE-API.md (v1). All strings NUL-terminated. */

#define API_NAME_LEN     64
#define API_UNIT_LEN     24
#define API_BARCODE_LEN  65
#define API_ERR_LEN      96
#define API_MAX_MATCHES  5
#define API_MAX_RESULTS  8

typedef struct {
    int  id;
    char name[API_NAME_LEN];
    char quantity_unit[API_UNIT_LEN];
    double stock_amount;
    double opened_amount;
    double min_stock_amount;
    bool on_shopping_list;
} api_product_t;

typedef struct {
    int  id;
    char name[API_NAME_LEN];
    double stock_amount; /* only filled by product search */
    double score;        /* only filled by scan suggestions */
} api_product_ref_t;

typedef enum {
    API_SCAN_FOUND,
    API_SCAN_UNKNOWN,
} api_scan_status_t;

typedef struct {
    api_scan_status_t status;
    api_product_t product;              /* status == FOUND */
    char barcode[API_BARCODE_LEN];      /* status == UNKNOWN */
    bool has_external_name;
    char external_name[API_NAME_LEN];   /* OFF "<brand> <name>" when present */
    int suggestion_count;
    api_product_ref_t suggestions[API_MAX_MATCHES];
} api_scan_result_t;

typedef enum {
    API_ACTION_PURCHASE,
    API_ACTION_OPEN,
    API_ACTION_CONSUME,
    API_ACTION_SHOPPING_LIST,
} api_action_t;

typedef struct {
    api_action_t action;
    char product_name[API_NAME_LEN];
    double stock_before, stock_after;
    double opened_before, opened_after;
    double shopping_quantity; /* < 0 when not a shopping-list action */
} api_action_result_t;

typedef struct {
    int count;
    api_product_ref_t results[API_MAX_RESULTS];
} api_search_result_t;

/* Stores base URL + token; no I/O. */
void api_client_init(const app_config_t *cfg);

/* Each call blocks (8 s timeout) — run from the app task, never the LVGL
 * task. On HTTP/transport failure they return an esp_err_t != ESP_OK and
 * fill `errbuf` (cap API_ERR_LEN) with a short user-facing message when it
 * is non-NULL. A 409 on action/create returns ESP_ERR_INVALID_STATE with
 * the server's error message in errbuf. */
esp_err_t api_ping(char *errbuf);
esp_err_t api_scan(const char *barcode, api_scan_result_t *out, char *errbuf);
esp_err_t api_action(int product_id, api_action_t action, api_action_result_t *out,
                     char *errbuf);
esp_err_t api_search(const char *query, api_search_result_t *out, char *errbuf);
esp_err_t api_create_product(const char *name, const char *barcode,
                             api_product_t *out, char *errbuf);
esp_err_t api_link_barcode(int product_id, const char *barcode,
                           api_product_t *out, char *errbuf);
