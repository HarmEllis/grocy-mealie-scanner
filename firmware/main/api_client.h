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
/* Fits a 36-char Mealie label UUID + NUL, and the reserved "unlabeled"
 * sentinel. */
#define API_LABEL_ID_LEN       40
/* 19 real labels (server-side cap, see docs/DEVICE-API.md) + the always-
 * appended "unlabeled" bucket. */
#define API_MAX_LABELS         20
/* A-Z + the '#' bucket for names that don't start with a letter. */
#define API_MAX_LETTERS        27
/* First-page cap for a browse product list; matches the server's browse
 * limit default/max (see docs/DEVICE-API.md). v1 firmware shows only the
 * first page (see api_browse_result_t.total/has_more). */
#define API_MAX_BROWSE_RESULTS 20

typedef struct {
    int  id;
    char name[API_NAME_LEN];
    char quantity_unit[API_UNIT_LEN];
    double stock_amount;
    double opened_amount;
    double min_stock_amount;
    bool on_shopping_list;
    double shopping_list_amount;
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

/* Browse (by Mealie label / A-Z index). See docs/DEVICE-API.md
 * for the /labels, /letters, and /products?labelId=/letter= contract. */

typedef struct {
    char id[API_LABEL_ID_LEN];   /* real Mealie label id, or the reserved
                                  * "unlabeled" sentinel */
    char name[API_NAME_LEN];
    int  count;
} api_label_t;

typedef struct {
    int count;
    api_label_t items[API_MAX_LABELS];
} api_label_list_t;

typedef struct {
    char letter; /* 'A'-'Z' or '#' */
    int  count;
} api_letter_bucket_t;

typedef struct {
    int count; /* always API_MAX_LETTERS (27) on success */
    api_letter_bucket_t items[API_MAX_LETTERS];
} api_letter_list_t;

typedef struct {
    int  count;                 /* items actually filled in `results` */
    int  total;                 /* total matches server-side, for the "+N more" hint */
    bool has_more;
    api_product_ref_t results[API_MAX_BROWSE_RESULTS];
} api_browse_result_t;

/* Stores base URL + token; no I/O. */
void api_client_init(const app_config_t *cfg);

/* Each call blocks (8 s timeout) — run from the app task, never the LVGL
 * task. On HTTP/transport failure they return an esp_err_t != ESP_OK and
 * fill `errbuf` (cap API_ERR_LEN) with a short user-facing message when it
 * is non-NULL. A 409 on action/create returns ESP_ERR_INVALID_STATE with
 * the server's error message in errbuf. */
esp_err_t api_ping(char *errbuf);
esp_err_t api_scan(const char *barcode, api_scan_result_t *out, char *errbuf);
int api_last_http_status(void);
bool api_error_is_transport(esp_err_t err);
/* `amount` is the quantity for the action (>= 1; the picker yields 1-9). */
esp_err_t api_action(int product_id, api_action_t action, int amount,
                     api_action_result_t *out, char *errbuf);
esp_err_t api_search(const char *query, api_search_result_t *out, char *errbuf);
/* Browse: full first page only (no offset), see api_browse_result_t. */
esp_err_t api_list_labels(api_label_list_t *out, char *errbuf);
esp_err_t api_list_letters(api_letter_list_t *out, char *errbuf);
esp_err_t api_list_products_by_label(const char *label_id, api_browse_result_t *out, char *errbuf);
esp_err_t api_list_products_by_letter(char letter, api_browse_result_t *out, char *errbuf);
esp_err_t api_create_product(const char *name, const char *barcode,
                             api_product_t *out, char *errbuf);
esp_err_t api_link_barcode(int product_id, const char *barcode,
                           api_product_t *out, char *errbuf);
/* Fetch a full product by id (home-screen search -> pick, shown as if scanned).
 * The server response root is the product, same shape as a scan "found". */
esp_err_t api_get_product(int product_id, api_product_t *out, char *errbuf);

/* Device API capability version last advertised by the server on /ping
 * (defaults to 1 until a successful ping reports a newer value). The idle
 * screen gates the product-search affordance on this so an out-of-date server
 * never shows a feature that would 404. */
int api_server_api_version(void);

#if CONFIG_GMS_DEMO_MODE
/* Demo build only (api_client_demo.c): returns the next synthetic barcode in
 * a fixed scenario cycle (product -> rich not-found -> product -> bare
 * not-found -> simulated error -> ...). main.c feeds it to the scan path on a
 * short BOOT press so the whole flow can be shown without the GM67 or a
 * printed code. The returned pointer is to static storage; copy if retained. */
const char *demo_next_barcode(void);
#endif
