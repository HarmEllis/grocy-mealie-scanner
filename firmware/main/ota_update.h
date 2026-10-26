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

/* Holds any tag semver_best_consider keeps (SEMVER_TAG_LEN) and the 31-character
 * esp_app_desc_t version. */
#define OTA_VERSION_LEN 48

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
 * `include_prereleases` also considers GitHub pre-releases and offers the
 * highest version by semver precedence (a pre-release sorts below its release).
 * Without it only the latest stable release is offered. The caller sets it
 * when the connected gm-sync runs a pre-release, so a scanner follows a
 * pre-release server onto the matching pre-release firmware, and returns to
 * stable releases otherwise.
 *
 * Returns ESP_OK when the check itself succeeded (regardless of whether an
 * update is available); inspect result->available.  Returns an error when the
 * check could not be performed (network, parse, clock). */
esp_err_t ota_check_for_update(bool include_prereleases, ota_check_result_t *result);

/* Download and install the firmware for the given version tag (e.g. "v0.0.2").
 *
 * Uses the Advanced OTA API (esp_https_ota_begin / perform / finish) so the
 * caller can display a progress bar.  GitHub asset downloads redirect (302) to
 * objects.githubusercontent.com; the Mozilla CA bundle covers both hosts.
 *
 * On success the caller should reboot (esp_restart()).  On failure the OTA is
 * aborted and the running firmware is unaffected. */
esp_err_t ota_perform_update(const char *tag, ota_progress_cb_t progress_cb);
