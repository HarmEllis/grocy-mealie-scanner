#pragma once

#include "esp_err.h"

/* Buffer size: the device API permits 64-character barcodes, so reserve
 * 64 code bytes + the NUL terminator. */
#define GM67_MAX_CODE_LEN 65

/* Called from the GM67 reader task whenever a complete barcode arrives.
 * `code` is NUL-terminated, trimmed of CR/LF, at most GM67_MAX_CODE_LEN-1
 * characters. Keep the callback short; hand off to a queue. */
typedef void (*gm67_scan_cb_t)(const char *code);

/* Starts UART1 + the reader task. The GM67 must be configured for
 * continuous (auto-sense) scanning with serial/TTL output, 9600 8N1 —
 * see BOARD_NOTES.md. Identical codes within `debounce_ms` are dropped so
 * a product held in front of the scanner fires once. */
esp_err_t gm67_init(gm67_scan_cb_t cb, uint32_t debounce_ms);
