#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define STORAGE_SSID_LEN   33
#define STORAGE_PASS_LEN   65
#define STORAGE_URL_LEN    128
#define STORAGE_TOKEN_LEN  96
#define STORAGE_AP_PASS_LEN 17

typedef struct {
    char wifi_ssid[STORAGE_SSID_LEN];
    char wifi_pass[STORAGE_PASS_LEN];
    char api_url[STORAGE_URL_LEN];    /* base URL incl. scheme, no trailing / */
    char api_token[STORAGE_TOKEN_LEN];
    char ap_pass[STORAGE_AP_PASS_LEN]; /* SoftAP password, generated once */
} app_config_t;

esp_err_t storage_init(void);

/* Loads the config; missing keys come back as empty strings. */
esp_err_t storage_load(app_config_t *cfg);

/* Persists the full config in one commit. Call rarely (provisioning only). */
esp_err_t storage_save(const app_config_t *cfg);

/* Wipes the config (factory reset via long BOOT press). */
esp_err_t storage_erase(void);

static inline bool storage_is_provisioned(const app_config_t *cfg)
{
    return cfg->wifi_ssid[0] != '\0' && cfg->api_url[0] != '\0';
}
