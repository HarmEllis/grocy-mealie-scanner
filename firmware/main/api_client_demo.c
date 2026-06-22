/* Demo build of the device API (CONFIG_GMS_DEMO_MODE). Drop-in replacement for
 * api_client.c: same symbols, but no HTTP. It serves canned product fixtures
 * and keeps stock in RAM, so purchase/consume/open show realistic before/after
 * numbers and the whole UI flow can be shown without grocy-mealie-sync.
 *
 * Known demo barcodes map to fixed scenarios; any other barcode (a real GM67
 * scan) resolves to a single reusable "scanned product" slot so live scanning
 * also works end-to-end. demo_next_barcode() drives a short-BOOT-press cycle
 * through every screen the firmware can show. */

#include "api_client.h"

#include "i18n.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* --- Demo barcodes, mapped to scenarios in api_scan() ------------------- */
#define BC_MILK  "8710398526045"  /* -> found, product 101            */
#define BC_OFF   "5449000000996"  /* -> unknown, OFF name + matches   */
#define BC_PASTA "8718906875395"  /* -> found, product 103            */
#define BC_BARE  "1111111111111"  /* -> unknown, no name, no matches  */
#define BC_ERR   "9999999999999"  /* -> simulated transport error     */

#define DEMO_GENERIC_ID 199 /* reused slot for any unrecognised barcode */

/* Mutable product table. The first entries are fixtures; create/link append
 * past s_count up to the array capacity. Stock numbers are doubles to match
 * the real client and Grocy's fractional amounts. */
static api_product_t s_products[16] = {
    { .id = 101, .name = "Semi-skimmed milk", .quantity_unit = "pack",
      .stock_amount = 3, .opened_amount = 0, .min_stock_amount = 2 },
    { .id = 103, .name = "Spaghetti 500g", .quantity_unit = "pack",
      .stock_amount = 5, .opened_amount = 1, .min_stock_amount = 2 },
    { .id = 110, .name = "Cola Zero 1.5L", .quantity_unit = "bottle",
      .stock_amount = 1, .opened_amount = 0, .min_stock_amount = 1,
      .on_shopping_list = true },
    { .id = 111, .name = "Lemonade 1L", .quantity_unit = "bottle",
      .stock_amount = 0, .opened_amount = 0, .min_stock_amount = 0 },
    { .id = DEMO_GENERIC_ID, .name = "Demo product", .quantity_unit = "pcs",
      .stock_amount = 2, .opened_amount = 0, .min_stock_amount = 1 },
};
static int s_count = 5;       /* live entries in s_products            */
static int s_next_id = 200;   /* ids handed out by create             */

static void set_err(char *errbuf, const char *msg)
{
    if (errbuf != NULL) {
        strlcpy(errbuf, msg, API_ERR_LEN);
    }
}

static api_product_t *find_product(int id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_products[i].id == id) {
            return &s_products[i];
        }
    }
    return NULL;
}

/* Case-insensitive substring test, for the demo search filter. */
static bool name_contains(const char *haystack, const char *needle)
{
    if (needle[0] == '\0') {
        return true;
    }
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h != '\0'; h++) {
        size_t i = 0;
        while (i < nlen && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

void api_client_init(const app_config_t *cfg)
{
    (void)cfg; /* no base URL / token in demo mode */
}

esp_err_t api_ping(char *errbuf)
{
    (void)errbuf;
    return ESP_OK;
}

int api_server_api_version(void)
{
    return 2; /* demo server supports the full contract incl. products/{id} */
}

int api_last_http_status(void)
{
    return 0;
}

bool api_error_is_transport(esp_err_t err)
{
    return err != ESP_OK && err != ESP_FAIL;
}

esp_err_t api_scan(const char *barcode, api_scan_result_t *out, char *errbuf)
{
    memset(out, 0, sizeof(*out));

    if (strcmp(barcode, BC_ERR) == 0) {
        set_err(errbuf, tr("server_unreachable"));
        return ESP_FAIL;
    }

    if (strcmp(barcode, BC_MILK) == 0) {
        out->status = API_SCAN_FOUND;
        out->product = *find_product(101);
        return ESP_OK;
    }
    if (strcmp(barcode, BC_PASTA) == 0) {
        out->status = API_SCAN_FOUND;
        out->product = *find_product(103);
        return ESP_OK;
    }

    if (strcmp(barcode, BC_OFF) == 0) {
        out->status = API_SCAN_UNKNOWN;
        strlcpy(out->barcode, barcode, sizeof(out->barcode));
        out->has_external_name = true;
        strlcpy(out->external_name, "Coca-Cola Classic 1.5L", sizeof(out->external_name));
        out->suggestion_count = 2;
        out->suggestions[0].id = 110;
        strlcpy(out->suggestions[0].name, "Cola Zero 1.5L", sizeof(out->suggestions[0].name));
        out->suggestions[0].score = 0.82;
        out->suggestions[1].id = 111;
        strlcpy(out->suggestions[1].name, "Lemonade 1L", sizeof(out->suggestions[1].name));
        out->suggestions[1].score = 0.61;
        return ESP_OK;
    }

    if (strcmp(barcode, BC_BARE) == 0) {
        out->status = API_SCAN_UNKNOWN;
        strlcpy(out->barcode, barcode, sizeof(out->barcode));
        return ESP_OK;
    }

    /* Any other barcode (a real GM67 scan): resolve to the reusable generic
     * slot, named from the code so the screen looks live, and reset its stock
     * so actions start from a clean count. */
    api_product_t *g = find_product(DEMO_GENERIC_ID);
    size_t blen = strlen(barcode);
    const char *tail = blen > 4 ? barcode + blen - 4 : barcode;
    snprintf(g->name, sizeof(g->name), "Product %s", tail);
    strlcpy(g->quantity_unit, "pcs", sizeof(g->quantity_unit));
    g->stock_amount = 2;
    g->opened_amount = 0;
    g->min_stock_amount = 1;
    g->on_shopping_list = false;
    out->status = API_SCAN_FOUND;
    out->product = *g;
    return ESP_OK;
}

esp_err_t api_action(int product_id, api_action_t action, api_action_result_t *out,
                     char *errbuf)
{
    api_product_t *p = find_product(product_id);
    if (p == NULL) {
        set_err(errbuf, tr("demo_unknown_product"));
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    out->action = action;
    out->shopping_quantity = -1;
    strlcpy(out->product_name, p->name, sizeof(out->product_name));

    double stock_before = p->stock_amount;
    double opened_before = p->opened_amount;

    switch (action) {
    case API_ACTION_PURCHASE:
        p->stock_amount += 1;
        break;
    case API_ACTION_CONSUME:
        p->stock_amount = p->stock_amount > 0 ? p->stock_amount - 1 : 0;
        if (p->opened_amount > p->stock_amount) {
            p->opened_amount = p->stock_amount;
        }
        break;
    case API_ACTION_OPEN:
        if (p->opened_amount < p->stock_amount) {
            p->opened_amount += 1;
        }
        break;
    case API_ACTION_SHOPPING_LIST:
        p->on_shopping_list = true;
        out->shopping_quantity = p->min_stock_amount > 0 ? p->min_stock_amount : 1;
        break;
    }

    out->stock_before = stock_before;
    out->stock_after = p->stock_amount;
    out->opened_before = opened_before;
    out->opened_after = p->opened_amount;
    return ESP_OK;
}

esp_err_t api_search(const char *query, api_search_result_t *out, char *errbuf)
{
    (void)errbuf;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < s_count && out->count < API_MAX_RESULTS; i++) {
        if (s_products[i].id == DEMO_GENERIC_ID) {
            continue; /* internal scratch slot, not a real catalogue entry */
        }
        if (!name_contains(s_products[i].name, query)) {
            continue;
        }
        api_product_ref_t *ref = &out->results[out->count++];
        ref->id = s_products[i].id;
        strlcpy(ref->name, s_products[i].name, sizeof(ref->name));
        ref->stock_amount = s_products[i].stock_amount;
    }
    return ESP_OK;
}

esp_err_t api_create_product(const char *name, const char *barcode,
                             api_product_t *out, char *errbuf)
{
    (void)barcode;
    if (s_count >= (int)(sizeof(s_products) / sizeof(s_products[0]))) {
        set_err(errbuf, tr("demo_catalogue_full"));
        return ESP_FAIL;
    }
    api_product_t *p = &s_products[s_count++];
    memset(p, 0, sizeof(*p));
    p->id = s_next_id++;
    strlcpy(p->name, name, sizeof(p->name));
    strlcpy(p->quantity_unit, "pcs", sizeof(p->quantity_unit));
    *out = *p;
    return ESP_OK;
}

esp_err_t api_link_barcode(int product_id, const char *barcode,
                           api_product_t *out, char *errbuf)
{
    (void)barcode;
    api_product_t *p = find_product(product_id);
    if (p == NULL) {
        set_err(errbuf, tr("demo_unknown_product"));
        return ESP_FAIL;
    }
    *out = *p;
    return ESP_OK;
}

esp_err_t api_get_product(int product_id, api_product_t *out, char *errbuf)
{
    api_product_t *p = find_product(product_id);
    if (p == NULL) {
        set_err(errbuf, tr("demo_unknown_product"));
        return ESP_FAIL;
    }
    *out = *p;
    return ESP_OK;
}

const char *demo_next_barcode(void)
{
    static const char *const cycle[] = { BC_MILK, BC_OFF, BC_PASTA, BC_BARE, BC_ERR };
    static size_t i = 0;
    const char *code = cycle[i];
    i = (i + 1) % (sizeof(cycle) / sizeof(cycle[0]));
    return code;
}
