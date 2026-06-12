#include "wifi_conn.h"
#include "captive_dns.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "wifi";

#define CONNECTED_BIT BIT0

/* Largest provisioning POST we accept. The form's decoded maxima total ~318
 * chars; fully %-escaped that is ~980 bytes, so 1 KB covers any valid body. */
#define PORTAL_BODY_MAX 1024

static EventGroupHandle_t s_events;
static bool s_sta_started;

static void sta_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

static esp_err_t wifi_common_init(void)
{
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
        ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    }
    return ESP_OK;
}

esp_err_t wifi_conn_start(const app_config_t *cfg, uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(wifi_common_init(), TAG, "common");
    if (!s_sta_started) {
        esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       sta_event_handler, NULL),
                            TAG, "wifi handler");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       sta_event_handler, NULL),
                            TAG, "ip handler");
    }

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "config");
    if (!s_sta_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        s_sta_started = true;
    } else {
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_conn_is_connected(void)
{
    return s_events != NULL && (xEventGroupGetBits(s_events) & CONNECTED_BIT) != 0;
}

/* ------------------------------------------------------------------ */
/* Provisioning portal                                                  */
/* ------------------------------------------------------------------ */

static app_config_t *s_prov_cfg;

static const char PORTAL_FORM[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>grocy-mealie-scanner setup</title>"
    "<style>body{font-family:system-ui,sans-serif;background:#161617;color:#ededf0;"
    "max-width:420px;margin:0 auto;padding:24px}h1{font-size:20px}"
    "label{display:block;font-size:13px;color:#8b8b93;margin:14px 0 4px}"
    "input{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;"
    "border:1px solid #3a3a40;background:#1e1e22;color:#ededf0;font-size:15px}"
    "button{margin-top:20px;width:100%%;padding:12px;border-radius:10px;border:0;"
    "background:#f5c13d;color:#0b0b0c;font-size:15px;font-weight:700}</style></head>"
    "<body><h1>grocy-mealie-scanner</h1>"
    "<form method='POST' action='/save'>"
    "<label>WiFi network (SSID)</label><input name='ssid' value='%s' required maxlength='32'>"
    "<label>WiFi password</label><input name='pass' type='password' value='%s' maxlength='64'>"
    "<label>grocy-mealie-sync base URL</label>"
    "<input name='url' value='%s' placeholder='http://192.168.1.10:3000' required maxlength='127'>"
    "<label>Device token</label><input name='token' value='%s' maxlength='95'>"
    "<button type='submit'>Save &amp; reboot</button></form></body></html>";

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* URL-decode src[0..srclen) into dst, writing at most cap-1 bytes plus a NUL.
 * Decode before bounding so a value that only fits after decoding (e.g. with
 * %-escapes) is not falsely truncated, and a '%' escape near the end is never
 * split: the field boundary is srclen, never the global NUL. Returns false on
 * a malformed %XX escape or a decoded NUL byte (e.g. %00) — either would
 * silently corrupt or truncate the stored credential. */
static bool url_decode_field(char *dst, size_t cap, const char *src, size_t srclen)
{
    size_t o = 0;
    for (size_t i = 0; i < srclen && o + 1 < cap; i++) {
        char decoded;
        if (src[i] == '+') {
            decoded = ' ';
        } else if (src[i] == '%') {
            if (i + 2 >= srclen || !is_hex_digit(src[i + 1]) || !is_hex_digit(src[i + 2])) {
                dst[0] = '\0';
                return false;
            }
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            decoded = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            decoded = src[i];
        }
        if (decoded == '\0') {
            dst[0] = '\0';
            return false;
        }
        dst[o++] = decoded;
    }
    dst[o] = '\0';
    return true;
}

/* Returns false only when the field is present but malformed; an absent field
 * yields an empty value and is not an error (pass/token are optional). */
static bool form_get_field(const char *body, const char *key, char *dst, size_t cap)
{
    dst[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            return url_decode_field(dst, cap, v, len);
        }
        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }
    return true;
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    /* Heap-allocated: form + values exceed the httpd task stack comfort zone. */
    size_t cap = sizeof(PORTAL_FORM) + sizeof(app_config_t);
    char *page = malloc(cap);
    if (page == NULL) {
        return httpd_resp_send_500(req);
    }
    snprintf(page, cap, PORTAL_FORM, s_prov_cfg->wifi_ssid, s_prov_cfg->wifi_pass,
             s_prov_cfg->api_url, s_prov_cfg->api_token);
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return err;
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    /* Reject anything that would not fit rather than parsing a truncated body:
     * a partial parse would save corrupted fields and reboot while reporting
     * success. Allocate exactly the (bounded) body so a fully %-escaped but
     * otherwise valid form is still accepted. */
    if (req->content_len == 0 || req->content_len > PORTAL_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_send(req, "Form too large", HTTPD_RESP_USE_STRLEN);
    }
    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, body + total, req->content_len - total);
        if (r <= 0) {
            free(body);
            return httpd_resp_send_500(req);
        }
        total += r;
    }
    body[total] = '\0';

    bool ok = form_get_field(body, "ssid", s_prov_cfg->wifi_ssid, sizeof(s_prov_cfg->wifi_ssid));
    ok = form_get_field(body, "pass", s_prov_cfg->wifi_pass, sizeof(s_prov_cfg->wifi_pass)) && ok;
    ok = form_get_field(body, "url", s_prov_cfg->api_url, sizeof(s_prov_cfg->api_url)) && ok;
    ok = form_get_field(body, "token", s_prov_cfg->api_token, sizeof(s_prov_cfg->api_token)) && ok;
    free(body);

    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Malformed form encoding", HTTPD_RESP_USE_STRLEN);
    }

    /* Strip a trailing slash so the API client can append paths verbatim. */
    size_t ulen = strlen(s_prov_cfg->api_url);
    if (ulen > 0 && s_prov_cfg->api_url[ulen - 1] == '/') {
        s_prov_cfg->api_url[ulen - 1] = '\0';
    }

    if (s_prov_cfg->wifi_ssid[0] == '\0' || s_prov_cfg->api_url[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "SSID and base URL are required", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = storage_save(s_prov_cfg);
    if (err != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
                    "<html><body style='font-family:sans-serif;background:#161617;"
                    "color:#ededf0;text-align:center;padding-top:40px'>"
                    "<h2>Saved!</h2><p>The scanner reboots and connects to your WiFi.</p>"
                    "</body></html>",
                    HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "provisioned, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK; /* unreachable */
}

static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    /* Captive-portal probes (generate_204, hotspot-detect, ...) land here. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t wifi_prov_run(app_config_t *cfg, char *ap_ssid_out, size_t ap_ssid_cap)
{
    s_prov_cfg = cfg;
    ESP_RETURN_ON_ERROR(wifi_common_init(), TAG, "common");
    esp_netif_create_default_wifi_ap();

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "mac");
    snprintf(ap_ssid_out, ap_ssid_cap, "scanner-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ap_ssid_out, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid_out);
    strlcpy((char *)ap.ap.password, cfg->ap_pass, sizeof(ap.ap.password));
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "ap mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "ap start");
    /* 11b/g only: S3 SoftAP + AMPDU quirks make 11n undetectable for some
     * phones (see sdkconfig.defaults). */
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);

    ESP_RETURN_ON_ERROR(captive_dns_start(), TAG, "captive dns");

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &hcfg), TAG, "httpd");

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = portal_get_handler };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = portal_save_handler };
    const httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = portal_redirect_handler };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &any);

    ESP_LOGI(TAG, "provisioning portal up: SSID %s, http://192.168.4.1", ap_ssid_out);
    return ESP_OK;
}
