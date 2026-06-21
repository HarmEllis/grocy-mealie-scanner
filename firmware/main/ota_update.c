/* OTA firmware update via GitHub Releases — implementation.
 *
 * Review points addressed:
 * 1. SNTP guard: clock_is_synced() prevents TLS handshake failures when the
 *    system clock is still at the UNIX epoch after an async SNTP start.
 * 2. GitHub API: User-Agent header avoids 403; Accept header requests JSON.
 * 3. Progress: uses Advanced OTA API (begin/perform loop/finish) so the UI
 *    can show a live progress bar via the callback.
 * 4. Memory: 16 KB response buffer for the /releases/latest payload; fine
 *    given 8 MB PSRAM on the ESP32-S3R8. */

#include "ota_update.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ota";

/* GitHub API endpoint for the latest release. */
#define GITHUB_API_URL \
    "https://api.github.com/repos/HarmEllis/grocy-mealie-scanner/releases/latest"

/* Download URL template.  %s is the full tag, e.g. "v0.0.2". */
#define GITHUB_DL_FMT \
    "https://github.com/HarmEllis/grocy-mealie-scanner/releases/download/%s/" \
    "grocy-mealie-scanner.bin"

#define API_TIMEOUT_MS   10000
#define DL_TIMEOUT_MS    30000
#define RESPONSE_BUF_MAX 16384   /* GitHub /releases/latest can be 5–20 KB */
#define USER_AGENT       "grocy-mealie-scanner"
#define MAX_REDIRECTS    5

/* ------------------------------------------------------------------ */
/* Semver helpers                                                      */
/* ------------------------------------------------------------------ */

typedef struct { int major, minor, patch; } semver_t;

/* Parse "v1.2.3" or "1.2.3" (ignoring any -suffix like -rc1 or -5-gabcdef)
 * into a semver_t.  Returns true on success. */
static bool parse_semver(const char *s, semver_t *out)
{
    if (s == NULL) {
        return false;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    return sscanf(s, "%d.%d.%d", &out->major, &out->minor, &out->patch) == 3;
}

/* Returns >0 if a > b, <0 if a < b, 0 if equal. */
static int semver_cmp(const semver_t *a, const semver_t *b)
{
    if (a->major != b->major) {
        return a->major - b->major;
    }
    if (a->minor != b->minor) {
        return a->minor - b->minor;
    }
    return a->patch - b->patch;
}

/* ------------------------------------------------------------------ */
/* Clock guard (review point 1)                                        */
/* ------------------------------------------------------------------ */

/* Returns true when the system clock looks synchronised (year > 2024).
 * SNTP starts asynchronously; if we fire an HTTPS request while the clock is
 * still at the UNIX epoch the TLS handshake rejects the server certificate
 * because its "not before" date is in the future from epoch's perspective. */
static bool clock_is_synced(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year > (2024 - 1900);
}

/* ------------------------------------------------------------------ */
/* HTTP response accumulator (same pattern as api_client.c)            */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    int   len;
    int   cap;
} resp_acc_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_acc_t *acc = (resp_acc_t *)evt->user_data;
        int space = acc->cap - 1 - acc->len;
        int n = evt->data_len < space ? evt->data_len : space;
        if (n > 0) {
            memcpy(acc->buf + acc->len, evt->data, n);
            acc->len += n;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Cert bundle helper                                                  */
/* ------------------------------------------------------------------ */

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
extern esp_err_t esp_crt_bundle_attach(void *conf);
#define CRT_BUNDLE_ATTACH esp_crt_bundle_attach
#else
#define CRT_BUNDLE_ATTACH NULL
#endif

/* ------------------------------------------------------------------ */
/* Public: check for update                                            */
/* ------------------------------------------------------------------ */

esp_err_t ota_check_for_update(ota_check_result_t *result)
{
    memset(result, 0, sizeof(*result));

    /* Fill in the running version from the app descriptor embedded at build
     * time (set by PROJECT_VER / git describe). */
    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(result->current_version, app->version,
            sizeof(result->current_version));

    /* Guard: TLS certificate validation will fail if the clock is at epoch. */
    if (!clock_is_synced()) {
        ESP_LOGW(TAG, "clock not synced, skipping OTA check");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate response buffer. */
    char *resp = calloc(1, RESPONSE_BUF_MAX);
    if (resp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    resp_acc_t acc = { .buf = resp, .len = 0, .cap = RESPONSE_BUF_MAX };

    esp_http_client_config_t cfg = {
        .url               = GITHUB_API_URL,
        .timeout_ms        = API_TIMEOUT_MS,
        .event_handler     = http_event_handler,
        .user_data         = &acc,
        .crt_bundle_attach = CRT_BUNDLE_ATTACH,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(resp);
        return ESP_FAIL;
    }

    /* Review point 2: GitHub API requires User-Agent; without it → 403.
     * Also specify the modern JSON Accept header. */
    esp_http_client_set_header(client, "User-Agent", USER_AGENT);
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GitHub API request failed: %s", esp_err_to_name(err));
        free(resp);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "GitHub API HTTP %d", status);
        free(resp);
        return ESP_FAIL;
    }

    resp[acc.len] = '\0';
    cJSON *json = cJSON_Parse(resp);
    free(resp);
    if (json == NULL) {
        ESP_LOGW(TAG, "failed to parse GitHub API response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Extract tag_name, e.g. "v0.0.2".  The download URL is constructed from
     * this rather than iterating the assets array — saves parsing effort. */
    const cJSON *tag_item = cJSON_GetObjectItemCaseSensitive(json, "tag_name");
    if (!cJSON_IsString(tag_item) || tag_item->valuestring == NULL) {
        cJSON_Delete(json);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *remote_tag = tag_item->valuestring;
    semver_t local_ver, remote_ver;
    if (!parse_semver(result->current_version, &local_ver) ||
        !parse_semver(remote_tag, &remote_ver)) {
        ESP_LOGW(TAG, "cannot parse versions: local=%s remote=%s",
                 result->current_version, remote_tag);
        cJSON_Delete(json);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(result->tag, remote_tag, sizeof(result->tag));
    /* Strip the "v" prefix for the human-readable version. */
    const char *v = remote_tag;
    if (*v == 'v' || *v == 'V') {
        v++;
    }
    strlcpy(result->new_version, v, sizeof(result->new_version));

    result->available = semver_cmp(&remote_ver, &local_ver) > 0;

    ESP_LOGI(TAG, "local=%s remote=%s -> %s", result->current_version,
             remote_tag, result->available ? "UPDATE AVAILABLE" : "up to date");

    cJSON_Delete(json);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public: perform update (review point 3 — Advanced OTA API)          */
/* ------------------------------------------------------------------ */

/* Callback set via esp_https_ota_config_t.http_client_init_cb so the
 * internally created HTTP client carries our User-Agent header. */
static esp_err_t ota_http_init_cb(esp_http_client_handle_t client)
{
    return esp_http_client_set_header(client, "User-Agent", USER_AGENT);
}

esp_err_t ota_perform_update(const char *tag, ota_progress_cb_t progress_cb)
{
    if (!clock_is_synced()) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    int n = snprintf(url, sizeof(url), GITHUB_DL_FMT, tag);
    if (n < 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "starting OTA download: %s", url);

    esp_http_client_config_t http_cfg = {
        .url                   = url,
        .timeout_ms            = DL_TIMEOUT_MS,
        .max_redirection_count = MAX_REDIRECTS,
        .crt_bundle_attach     = CRT_BUNDLE_ATTACH,
        .keep_alive_enable     = true,
        /* GitHub redirects the release asset to objects.githubusercontent.com
         * with a long signed query string.  The default 512-byte buffers are
         * too small to hold the request line + headers for that redirected
         * URL, which surfaces as "HTTP_CLIENT: Out of buffer" and a failed
         * connection open.  Enlarge both RX and TX buffers. */
        .buffer_size           = 4096,
        .buffer_size_tx        = 4096,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config         = &http_cfg,
        .http_client_init_cb = ota_http_init_cb,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }

    int total = esp_https_ota_get_image_size(handle);
    int last_pct = -1;

    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        /* Report progress to the UI via callback (review point 3). */
        if (progress_cb != NULL && total > 0) {
            int read = esp_https_ota_get_image_len_read(handle);
            int pct = read * 100 / total;
            if (pct != last_pct) {
                last_pct = pct;
                progress_cb(pct);
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA data incomplete");
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update installed — reboot to activate");
    return ESP_OK;
}
