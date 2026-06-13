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
    /* Feedback toggles default on; a missing NVS key (first boot) keeps them on. */
    strlcpy(cfg->language, I18N_DEFAULT_LANGUAGE, sizeof(cfg->language));
    cfg->beep_enabled = true;
    cfg->light_enabled = true;
    cfg->screen_timeout_seconds = 60; /* default: sleep after 60 s idle */
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
        cfg->beep_enabled = load_flag(h, "beep", true);
        cfg->light_enabled = load_flag(h, "light", true);
        cfg->screen_timeout_seconds = load_u32(h, "scrn_to", 60);
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
    esp_err_t err = nvs_set_u8(h, "beep", cfg->beep_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "light", cfg->light_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, "lang", cfg->language);
    if (err == ESP_OK) err = nvs_set_u32(h, "scrn_to", cfg->screen_timeout_seconds);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
    }
    return err;
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
