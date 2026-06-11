#pragma once

#include "esp_err.h"
#include "storage.h"
#include <stdbool.h>

/* Connects to the stored WiFi as a station. Blocks up to `timeout_ms`;
 * returns ESP_OK once an IP is obtained. Reconnects automatically after
 * drops for the lifetime of the app. */
esp_err_t wifi_conn_start(const app_config_t *cfg, uint32_t timeout_ms);

bool wifi_conn_is_connected(void);

/* Provisioning portal: SoftAP "scanner-XXXX" + captive DNS + a config form
 * at http://192.168.4.1. Returns the SSID actually used via `ap_ssid_out`
 * (cap >= 16). Blocks forever: the portal saves the submitted config and
 * reboots the device. `cfg` supplies the AP password and prefills the form
 * on re-provisioning. */
esp_err_t wifi_prov_run(app_config_t *cfg, char *ap_ssid_out, size_t ap_ssid_cap);
