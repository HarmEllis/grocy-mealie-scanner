#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Buffer size: the device API permits 64-character barcodes, so reserve
 * 64 code bytes + the NUL terminator. */
#define GM67_MAX_CODE_LEN 65

/* Called from the GM67 reader task whenever a complete barcode arrives.
 * `code` is NUL-terminated, trimmed of CR/LF, at most GM67_MAX_CODE_LEN-1
 * characters. Keep the callback short; hand off to a queue. */
typedef void (*gm67_scan_cb_t)(const char *code);

/* Starts UART1 and the GM67 owning task. The task first pushes a serial
 * configuration sequence to the module (continuous scan, Code-only payload,
 * CR LF terminator, good-read beep — see docs/GM67-IMPROVEMENT-PLAN.md), so a
 * serially reachable module is driven into a known state without setup
 * barcodes. Configuration is best-effort: a module that does not respond
 * (wrong baud, USB-KBW, older firmware) falls through to passive reading, so
 * the read path is never bricked. Requires the module on TTL serial at
 * 9600 8N1 for the configuration to be reachable. Identical codes within
 * `debounce_ms` are dropped so a product held in front fires once. */
esp_err_t gm67_init(gm67_scan_cb_t cb, uint32_t debounce_ms);

/* Toggle the module's good-read beep at runtime (Phase 2 settings screen).
 * Fire-and-forget per the single-UART-owner design: the byte sequence is sent
 * by the owning task, and the return value reports queue admission only
 * (ESP_OK = enqueued), never a module ACK. A dropped toggle is cosmetic (a
 * missed or spurious beep on the next scan). Safe to call from any task; must
 * be called after gm67_init(). */
esp_err_t gm67_set_beep(bool enabled);
