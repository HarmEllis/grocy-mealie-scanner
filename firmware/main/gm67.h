#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Product barcodes remain limited to 64 characters. Setup QR payloads use
 * the larger reader buffer and are validated separately. */
#define GM67_MAX_BARCODE_LEN 65
#define GM67_MAX_CODE_LEN    512

/* Called from the GM67 reader task whenever a complete barcode arrives.
 * `code` is NUL-terminated, trimmed of CR/LF, at most GM67_MAX_CODE_LEN-1
 * characters. Keep the callback short; hand off to a queue. */
typedef void (*gm67_scan_cb_t)(const char *code);

/* GM67 beep level.  Values map to NVS storage (do not reorder). */
typedef enum {
    GM67_BEEP_OFF    = 0,
    GM67_BEEP_LOW    = 1,
    GM67_BEEP_MEDIUM = 2,
    GM67_BEEP_HIGH   = 3,
} gm67_beep_level_t;

/* GM67 scanning illumination light mode. */
typedef enum {
    GM67_LIGHT_ON_SCAN    = 0,
    GM67_LIGHT_ALWAYS_OFF = 1,
} gm67_light_mode_t;

/* GM67 collimation/aiming light mode. */
typedef enum {
    GM67_COLLIM_ON_SCAN    = 0,
    GM67_COLLIM_ALWAYS_OFF = 1,
} gm67_collim_mode_t;

/* Starts UART1 and the GM67 owning task.  The scanner must already be in TTL
 * serial mode (scan the UART QR code from the GM67 manual once; the setting
 * persists in the scanner's own NVS).  Identical codes within `debounce_ms`
 * are dropped so a product held in front fires once. */
esp_err_t gm67_init(gm67_scan_cb_t cb, uint32_t debounce_ms);

/* Set the good-read beep level.  Fire-and-forget: enqueues one or two
 * PARAM_SEND commands to the owning task; returns ESP_OK on successful
 * enqueue, ESP_ERR_NO_MEM if the queue is full.  The setting persists in
 * the scanner's NVS. */
esp_err_t gm67_set_beep_level(gm67_beep_level_t level);

/* Set the scanning illumination light mode.  Fire-and-forget. */
esp_err_t gm67_set_scanner_light(gm67_light_mode_t mode);

/* Set the collimation/aiming light mode.  Fire-and-forget. */
esp_err_t gm67_set_collimation(gm67_collim_mode_t mode);

/* Enable or disable the scanning gate (screen sleep feature).
 * enabled=false → software gate closes immediately (authoritative guard), then
 *   SCAN_DISABLE is queued best-effort; always returns ESP_OK.
 * enabled=true  → SCAN_ENABLE is queued first; the software gate opens only
 *   on success.  Returns ESP_ERR_NO_MEM if the command queue is full (gate
 *   stays closed so the caller can retry), ESP_ERR_INVALID_STATE if called
 *   before gm67_init().
 * Safe to call from any task after gm67_init(). */
esp_err_t gm67_set_scanning(bool enabled);
