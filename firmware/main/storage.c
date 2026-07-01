#include "storage.h"

#include "i18n.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "storage";
static const char *NS = "cfg";

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    return err;
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        dst[0] = '\0';
    }
}

static bool load_flag(nvs_handle_t h, const char *key, bool fallback)
{
    uint8_t v;
    return nvs_get_u8(h, key, &v) == ESP_OK ? v != 0 : fallback;
}

static uint32_t load_u32(nvs_handle_t h, const char *key, uint32_t fallback)
{
    uint32_t v;
    return nvs_get_u32(h, key, &v) == ESP_OK ? v : fallback;
}

esp_err_t storage_load(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->language, I18N_DEFAULT_LANGUAGE, sizeof(cfg->language));
    cfg->beep_level = 2;    /* GM67_BEEP_MEDIUM */
    cfg->light_enabled = true;
    cfg->scanner_light = 0; /* GM67_LIGHT_ON_SCAN */
    cfg->collimation = 0;   /* GM67_COLLIM_ON_SCAN */
    cfg->screen_timeout_seconds = 60; /* default: sleep after 60 s idle */
    cfg->wifi_power_save = false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot: nothing stored yet. */
    } else if (err != ESP_OK) {
        return err;
    } else {
        load_str(h, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
        load_str(h, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
        load_str(h, "api_url", cfg->api_url, sizeof(cfg->api_url));
        load_str(h, "api_token", cfg->api_token, sizeof(cfg->api_token));
        load_str(h, "ap_pass", cfg->ap_pass, sizeof(cfg->ap_pass));
        load_str(h, "lang", cfg->language, sizeof(cfg->language));
        load_str(h, "wifi_cc", cfg->wifi_country, sizeof(cfg->wifi_country));
        cfg->beep_level = (uint8_t)load_u32(h, "beep_lvl", 2);
        cfg->light_enabled = load_flag(h, "light", true);
        cfg->scanner_light = (uint8_t)load_u32(h, "scan_lgt", 0);
        cfg->collimation = (uint8_t)load_u32(h, "collim", 0);
        cfg->screen_timeout_seconds = load_u32(h, "scrn_to", 60);
        cfg->wifi_power_save = load_flag(h, "wifi_ps", false);
        cfg->api_insecure = load_flag(h, "api_insec", false);
        cfg->touch_cal_x_left = load_u32(h, "tcal_xl", 0);
        cfg->touch_cal_x_right = load_u32(h, "tcal_xr", 0);
        cfg->touch_cal_y_top = load_u32(h, "tcal_yt", 0);
        cfg->touch_cal_y_bottom = load_u32(h, "tcal_yb", 0);
        nvs_close(h);
    }
    if (!i18n_language_is_supported(cfg->language)) {
        strlcpy(cfg->language, I18N_DEFAULT_LANGUAGE, sizeof(cfg->language));
    }

    /* The SoftAP password is generated on first use and then sticks, so a
     * re-provisioning window months later still shows the password printed
     * on the sticker/QR the user saved. */
    if (cfg->ap_pass[0] == '\0') {
        static const char alphabet[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ23456789";
        for (size_t i = 0; i + 1 < sizeof(cfg->ap_pass); i++) {
            cfg->ap_pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
        }
        cfg->ap_pass[sizeof(cfg->ap_pass) - 1] = '\0';
        nvs_handle_t hw;
        if (nvs_open(NS, NVS_READWRITE, &hw) == ESP_OK) {
            nvs_set_str(hw, "ap_pass", cfg->ap_pass);
            nvs_commit(hw);
            nvs_close(hw);
        }
    }
    return ESP_OK;
}

esp_err_t storage_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_str(h, "wifi_ssid", cfg->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "wifi_pass", cfg->wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "api_url", cfg->api_url);
    if (err == ESP_OK) err = nvs_set_str(h, "api_token", cfg->api_token);
    if (err == ESP_OK) err = nvs_set_str(h, "ap_pass", cfg->ap_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "lang", cfg->language);
    if (err == ESP_OK) err = nvs_set_str(h, "wifi_cc", cfg->wifi_country);
    if (err == ESP_OK) err = nvs_set_u8(h, "wifi_ps", cfg->wifi_power_save ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "api_insec", cfg->api_insecure ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "config saved");
    } else {
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_save_settings(const app_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u32(h, "beep_lvl", cfg->beep_level);
    if (err == ESP_OK) err = nvs_set_u8(h, "light", cfg->light_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32(h, "scan_lgt", cfg->scanner_light);
    if (err == ESP_OK) err = nvs_set_u32(h, "collim", cfg->collimation);
    if (err == ESP_OK) err = nvs_set_str(h, "lang", cfg->language);
    if (err == ESP_OK) err = nvs_set_u32(h, "scrn_to", cfg->screen_timeout_seconds);
    if (err == ESP_OK) err = nvs_set_u8(h, "wifi_ps", cfg->wifi_power_save ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_save_touch_cal(const app_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u32(h, "tcal_xl", cfg->touch_cal_x_left);
    if (err == ESP_OK) err = nvs_set_u32(h, "tcal_xr", cfg->touch_cal_x_right);
    if (err == ESP_OK) err = nvs_set_u32(h, "tcal_yt", cfg->touch_cal_y_top);
    if (err == ESP_OK) err = nvs_set_u32(h, "tcal_yb", cfg->touch_cal_y_bottom);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch calibration save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_save_api_ca(const char *pem)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err;
    if (pem == NULL || pem[0] == '\0') {
        err = nvs_erase_key(h, "api_ca");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK; /* nothing stored; nothing to clear */
        }
    } else {
        err = nvs_set_str(h, "api_ca", pem);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "api ca save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_load_api_ca(char *buf, size_t cap, size_t *out_len)
{
    buf[0] = '\0';
    if (out_len != NULL) {
        *out_len = 0;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* no namespace yet: treat as no cert */
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open");
    size_t len = cap;
    err = nvs_get_str(h, "api_ca", buf, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* key absent: empty string */
    }
    if (err != ESP_OK) {
        buf[0] = '\0';
        return err;
    }
    if (out_len != NULL) {
        *out_len = strlen(buf);
    }
    return ESP_OK;
}

void storage_request_setup(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(h, "setup_req", 1) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

bool storage_take_setup_request(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    bool requested = nvs_get_u8(h, "setup_req", &v) == ESP_OK && v != 0;
    if (requested) {
        nvs_erase_key(h, "setup_req");
        nvs_commit(h);
    }
    nvs_close(h);
    return requested;
}

esp_err_t storage_erase(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
