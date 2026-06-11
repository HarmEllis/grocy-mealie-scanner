#include "api_client.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "api";

#define HTTP_TIMEOUT_MS 8000
#define MAX_RESPONSE    4096
#define MAX_URL         (STORAGE_URL_LEN + 128)

static char s_base_url[STORAGE_URL_LEN];
static char s_auth_header[STORAGE_TOKEN_LEN + 8];

void api_client_init(const app_config_t *cfg)
{
    strlcpy(s_base_url, cfg->api_url, sizeof(s_base_url));
    snprintf(s_auth_header, sizeof(s_auth_header), "Bearer %s", cfg->api_token);
}

static void set_err(char *errbuf, const char *msg)
{
    if (errbuf != NULL) {
        strlcpy(errbuf, msg, API_ERR_LEN);
    }
}

typedef struct {
    char *buf;
    int len;
} response_acc_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        response_acc_t *acc = (response_acc_t *)evt->user_data;
        int space = MAX_RESPONSE - 1 - acc->len;
        int n = evt->data_len < space ? evt->data_len : space;
        if (n > 0) {
            memcpy(acc->buf + acc->len, evt->data, n);
            acc->len += n;
        }
    }
    return ESP_OK;
}

/* Performs the request and parses the JSON body. Returns ESP_OK for any
 * HTTP response with a parsable body (status reported via *status_out);
 * transport errors return the esp_http_client error. Caller owns *json_out. */
static esp_err_t request_json(const char *method, const char *path, const char *body,
                              int *status_out, cJSON **json_out, char *errbuf)
{
    char url[MAX_URL];
    snprintf(url, sizeof(url), "%s%s", s_base_url, path);

    char *resp = calloc(1, MAX_RESPONSE);
    if (resp == NULL) {
        set_err(errbuf, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    response_acc_t acc = { .buf = resp, .len = 0 };

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = http_event_cb,
        .user_data = &acc,
        .crt_bundle_attach = NULL, /* set below only for https */
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    extern esp_err_t esp_crt_bundle_attach(void *conf);
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(resp);
        set_err(errbuf, "HTTP init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client,
                               strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", s_auth_header);
    esp_http_client_set_header(client, "Accept", "application/json");
    if (body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: %s", method, path, esp_err_to_name(err));
        free(resp);
        set_err(errbuf, "Server unreachable");
        return err;
    }

    resp[acc.len] = '\0';
    cJSON *json = cJSON_Parse(resp);
    free(resp);
    if (json == NULL) {
        set_err(errbuf, "Invalid server response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    *status_out = status;
    *json_out = json;
    return ESP_OK;
}

static void json_get_str(const cJSON *obj, const char *key, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        strlcpy(dst, v->valuestring, cap);
    } else {
        dst[0] = '\0';
    }
}

static double json_get_num(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static void parse_product(const cJSON *p, api_product_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = (int)json_get_num(p, "id", 0);
    json_get_str(p, "name", out->name, sizeof(out->name));
    json_get_str(p, "quantityUnit", out->quantity_unit, sizeof(out->quantity_unit));
    out->stock_amount = json_get_num(p, "stockAmount", 0);
    out->opened_amount = json_get_num(p, "openedAmount", 0);
    out->min_stock_amount = json_get_num(p, "minStockAmount", 0);
    out->on_shopping_list = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "onShoppingList"));
}

/* Copies the server's {"error": "..."} into errbuf, with a fallback. */
static void take_server_error(const cJSON *json, char *errbuf, const char *fallback)
{
    char msg[API_ERR_LEN] = "";
    json_get_str(json, "error", msg, sizeof(msg));
    set_err(errbuf, msg[0] != '\0' ? msg : fallback);
}

esp_err_t api_ping(char *errbuf)
{
    int status = 0;
    cJSON *json = NULL;
    ESP_RETURN_ON_ERROR(request_json("GET", "/api/device/v1/ping", NULL,
                                     &status, &json, errbuf),
                        TAG, "ping");
    bool ok = status == 200 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok"));
    if (!ok) {
        take_server_error(json, errbuf, status == 401 ? "Invalid device token" : "Ping failed");
    }
    cJSON_Delete(json);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t api_scan(const char *barcode, api_scan_result_t *out, char *errbuf)
{
    char path[160];
    snprintf(path, sizeof(path), "/api/device/v1/scan/%s", barcode);

    int status = 0;
    cJSON *json = NULL;
    ESP_RETURN_ON_ERROR(request_json("GET", path, NULL, &status, &json, errbuf), TAG, "scan");
    if (status != 200) {
        take_server_error(json, errbuf, "Lookup failed");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    char status_str[16];
    json_get_str(json, "status", status_str, sizeof(status_str));

    if (strcmp(status_str, "found") == 0) {
        out->status = API_SCAN_FOUND;
        parse_product(cJSON_GetObjectItemCaseSensitive(json, "product"), &out->product);
    } else {
        out->status = API_SCAN_UNKNOWN;
        json_get_str(json, "barcode", out->barcode, sizeof(out->barcode));

        const cJSON *ext = cJSON_GetObjectItemCaseSensitive(json, "externalLookup");
        if (cJSON_IsObject(ext)) {
            char name[API_NAME_LEN] = "";
            char brand[API_NAME_LEN] = "";
            json_get_str(ext, "name", name, sizeof(name));
            json_get_str(ext, "brand", brand, sizeof(brand));
            if (name[0] != '\0') {
                out->has_external_name = true;
                if (brand[0] != '\0') {
                    strlcpy(out->external_name, brand, sizeof(out->external_name));
                    strlcat(out->external_name, " ", sizeof(out->external_name));
                    strlcat(out->external_name, name, sizeof(out->external_name));
                } else {
                    strlcpy(out->external_name, name, sizeof(out->external_name));
                }
            }
        }

        const cJSON *matches = cJSON_GetObjectItemCaseSensitive(json, "suggestedMatches");
        const cJSON *m;
        cJSON_ArrayForEach(m, matches) {
            if (out->suggestion_count >= API_MAX_MATCHES) {
                break;
            }
            api_product_ref_t *ref = &out->suggestions[out->suggestion_count++];
            ref->id = (int)json_get_num(m, "id", 0);
            json_get_str(m, "name", ref->name, sizeof(ref->name));
            ref->score = json_get_num(m, "score", 0);
        }
    }
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t api_action(int product_id, api_action_t action, api_action_result_t *out,
                     char *errbuf)
{
    static const char *action_names[] = {
        [API_ACTION_PURCHASE] = "purchase",
        [API_ACTION_OPEN] = "open",
        [API_ACTION_CONSUME] = "consume",
        [API_ACTION_SHOPPING_LIST] = "add_to_shopping_list",
    };

    char path[80];
    snprintf(path, sizeof(path), "/api/device/v1/products/%d/action", product_id);
    char body[64];
    snprintf(body, sizeof(body), "{\"action\":\"%s\",\"amount\":1}", action_names[action]);

    int status = 0;
    cJSON *json = NULL;
    ESP_RETURN_ON_ERROR(request_json("POST", path, body, &status, &json, errbuf),
                        TAG, "action");
    if (status != 200) {
        take_server_error(json, errbuf, "Action failed");
        cJSON_Delete(json);
        return status == 409 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    out->action = action;
    out->shopping_quantity = -1;
    const cJSON *product = cJSON_GetObjectItemCaseSensitive(json, "product");
    json_get_str(product, "name", out->product_name, sizeof(out->product_name));
    const cJSON *stock = cJSON_GetObjectItemCaseSensitive(json, "stock");
    out->stock_before = json_get_num(stock, "before", 0);
    out->stock_after = json_get_num(stock, "after", 0);
    const cJSON *opened = cJSON_GetObjectItemCaseSensitive(json, "opened");
    out->opened_before = json_get_num(opened, "before", 0);
    out->opened_after = json_get_num(opened, "after", 0);
    const cJSON *shopping = cJSON_GetObjectItemCaseSensitive(json, "shoppingList");
    if (cJSON_IsObject(shopping)) {
        out->shopping_quantity = json_get_num(shopping, "quantity", 0);
    }
    cJSON_Delete(json);
    return ESP_OK;
}

static int url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 4 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xf];
        }
    }
    dst[o] = '\0';
    return (int)o;
}

esp_err_t api_search(const char *query, api_search_result_t *out, char *errbuf)
{
    char encoded[3 * API_NAME_LEN];
    url_encode(query, encoded, sizeof(encoded));
    char path[256];
    snprintf(path, sizeof(path), "/api/device/v1/products?query=%s&limit=%d",
             encoded, API_MAX_RESULTS);

    int status = 0;
    cJSON *json = NULL;
    ESP_RETURN_ON_ERROR(request_json("GET", path, NULL, &status, &json, errbuf),
                        TAG, "search");
    if (status != 200) {
        take_server_error(json, errbuf, "Search failed");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
    const cJSON *r;
    cJSON_ArrayForEach(r, results) {
        if (out->count >= API_MAX_RESULTS) {
            break;
        }
        api_product_ref_t *ref = &out->results[out->count++];
        ref->id = (int)json_get_num(r, "id", 0);
        json_get_str(r, "name", ref->name, sizeof(ref->name));
        ref->stock_amount = json_get_num(r, "stockAmount", 0);
    }
    cJSON_Delete(json);
    return ESP_OK;
}

/* POST helper shared by create + link: both answer with a full product. */
static esp_err_t post_for_product(const char *path, const char *body,
                                  api_product_t *out, char *errbuf, const char *fallback)
{
    int status = 0;
    cJSON *json = NULL;
    ESP_RETURN_ON_ERROR(request_json("POST", path, body, &status, &json, errbuf),
                        TAG, "post");
    if (status != 200 && status != 201) {
        take_server_error(json, errbuf, fallback);
        cJSON_Delete(json);
        return status == 409 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    parse_product(cJSON_GetObjectItemCaseSensitive(json, "product"), out);
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t api_create_product(const char *name, const char *barcode,
                             api_product_t *out, char *errbuf)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    cJSON_AddStringToObject(body, "barcode", barcode);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_str == NULL) {
        set_err(errbuf, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = post_for_product("/api/device/v1/products", body_str, out, errbuf,
                                     "Create failed");
    free(body_str);
    return err;
}

esp_err_t api_link_barcode(int product_id, const char *barcode,
                           api_product_t *out, char *errbuf)
{
    char path[80];
    snprintf(path, sizeof(path), "/api/device/v1/products/%d/barcodes", product_id);
    char body[96];
    snprintf(body, sizeof(body), "{\"barcode\":\"%s\"}", barcode);
    return post_for_product(path, body, out, errbuf, "Link failed");
}
