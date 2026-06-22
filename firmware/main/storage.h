#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define STORAGE_SSID_LEN   33
#define STORAGE_PASS_LEN   65
#define STORAGE_URL_LEN    128
#define STORAGE_TOKEN_LEN  96
#define STORAGE_AP_PASS_LEN 17
#define STORAGE_LANGUAGE_LEN 3
/* Max length (incl. NUL) of a custom API CA certificate (PEM). NVS string
 * values cap at ~4000 bytes; one PEM CA fits comfortably. Stored under its own
 * NVS key, not in app_config_t, to keep the struct small (it is copied by value
 * onto narrow task stacks). */
#define STORAGE_CA_CERT_MAX 4000

typedef struct {
    char wifi_ssid[STORAGE_SSID_LEN];
    char wifi_pass[STORAGE_PASS_LEN];
    char api_url[STORAGE_URL_LEN];    /* base URL incl. scheme, no trailing / */
    char api_token[STORAGE_TOKEN_LEN];
    char ap_pass[STORAGE_AP_PASS_LEN]; /* SoftAP password, generated once */
    char language[STORAGE_LANGUAGE_LEN]; /* BCP 47 code: "en" or "nl" */
    uint8_t beep_level;    /* gm67_beep_level_t; default GM67_BEEP_MEDIUM (2) */
    bool    light_enabled; /* WS2812 result flash (settings screen; default on) */
    uint8_t scanner_light; /* gm67_light_mode_t; default GM67_LIGHT_ON_SCAN (0) */
    uint8_t collimation;   /* gm67_collim_mode_t; default GM67_COLLIM_ON_SCAN (0) */
    uint32_t screen_timeout_seconds; /* backlight+panel sleep after idle; 0 = never */
    bool    wifi_power_save; /* WiFi modem sleep; default on, can be disabled for lower latency */
    bool    api_insecure;  /* trust any HTTPS cert for the API only (OTA stays strict) */
    uint32_t touch_cal_x_left;
    uint32_t touch_cal_x_right;
    uint32_t touch_cal_y_top;
    uint32_t touch_cal_y_bottom;
} app_config_t;

esp_err_t storage_init(void);

/* Loads the config; missing keys come back as empty strings. */
esp_err_t storage_load(app_config_t *cfg);

/* Persists the full config in one commit. Call rarely (provisioning only). */
esp_err_t storage_save(const app_config_t *cfg);

/* Persists only the beep/light/language settings (settings screen).
 * Cheaper and rarer than storage_save; leaves the credential keys untouched. */
esp_err_t storage_save_settings(const app_config_t *cfg);

/* Persists only the four touch-calibration samples in one NVS commit. */
esp_err_t storage_save_touch_cal(const app_config_t *cfg);

/* Custom API CA certificate (PEM). Kept out of app_config_t (see
 * STORAGE_CA_CERT_MAX). Passing an empty/NULL string to save erases the key. */
esp_err_t storage_save_api_ca(const char *pem);
/* Loads the PEM into buf (NUL-terminated). *out_len gets the string length (may
 * be NULL). Returns ESP_OK with an empty string when no cert is stored. */
esp_err_t storage_load_api_ca(char *buf, size_t cap, size_t *out_len);

/* Re-enter the provisioning portal after the next reboot, without erasing the
 * config (unlike a factory reset). Set from the settings screen / short BOOT
 * press; consumed (read + cleared) once at boot by storage_take_setup_request. */
void storage_request_setup(void);
bool storage_take_setup_request(void);

/* Wipes the config (factory reset via long BOOT press). */
esp_err_t storage_erase(void);

static inline bool storage_is_provisioned(const app_config_t *cfg)
{
    return cfg->wifi_ssid[0] != '\0' && cfg->api_url[0] != '\0';
}
