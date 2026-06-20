/* OTA firmware update via GitHub Releases.
 *
 * Checks the GitHub API for a newer tagged release, compares semver with the
 * running image, and downloads the signed binary via the ESP-IDF Advanced OTA
 * API so progress can be reported.  The existing rollback + app-signing
 * infrastructure (RSA-3072 Secure Boot V2, no eFuse burning) is leveraged
 * automatically by the bootloader.
 *
 * All functions block and must run from the app task (same rule as api_client). */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define OTA_VERSION_LEN 24

typedef struct {
    bool available;
    char current_version[OTA_VERSION_LEN];
    char new_version[OTA_VERSION_LEN]; /* human-readable, no "v" prefix */
    char tag[OTA_VERSION_LEN];         /* full Git tag, e.g. "v0.0.2"  */
} ota_check_result_t;

/* Progress callback: receives percentage 0–100 during download. */
typedef void (*ota_progress_cb_t)(int percent);

/* Check GitHub Releases for a newer firmware version.
 *
 * Requires WiFi and a synchronised system clock (SNTP); returns
 * ESP_ERR_INVALID_STATE immediately if the clock is still at epoch so TLS
 * certificate validation is not attempted with a bogus date.
 *
 * Returns ESP_OK when the check itself succeeded (regardless of whether an
 * update is available); inspect result->available.  Returns an error when the
 * check could not be performed (network, parse, clock). */
esp_err_t ota_check_for_update(ota_check_result_t *result);

/* Download and install the firmware for the given version tag (e.g. "v0.0.2").
 *
 * Uses the Advanced OTA API (esp_https_ota_begin / perform / finish) so the
 * caller can display a progress bar.  GitHub asset downloads redirect (302) to
 * objects.githubusercontent.com; the Mozilla CA bundle covers both hosts.
 *
 * On success the caller should reboot (esp_restart()).  On failure the OTA is
 * aborted and the running firmware is unaffected. */
esp_err_t ota_perform_update(const char *tag, ota_progress_cb_t progress_cb);
