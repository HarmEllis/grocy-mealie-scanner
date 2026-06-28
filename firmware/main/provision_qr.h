#pragma once
#include <stdbool.h>
#include <stddef.h>
#define PROVISION_QR_PREFIX "GMS1_"
#define PROVISION_QR_SSID_LEN 33
#define PROVISION_QR_PASS_LEN 65
#define PROVISION_QR_URL_LEN 128
#define PROVISION_QR_TOKEN_LEN 96
#define PROVISION_QR_COUNTRY_LEN 4
#define PROVISION_QR_LANGUAGE_LEN 3
#define PROVISION_QR_MAX_TEXT_LEN 446
typedef struct {
    char wifi_ssid[PROVISION_QR_SSID_LEN];
    char wifi_pass[PROVISION_QR_PASS_LEN];
    char api_url[PROVISION_QR_URL_LEN];
    char api_token[PROVISION_QR_TOKEN_LEN];
    char wifi_country[PROVISION_QR_COUNTRY_LEN];
    char language[PROVISION_QR_LANGUAGE_LEN];
    bool api_insecure;
} provision_qr_config_t;
bool provision_qr_is_payload(const char *text);
bool provision_qr_parse(const char *text, provision_qr_config_t *out);
