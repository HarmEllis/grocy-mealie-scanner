#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* On-board WS2812 (GPIO42) used as a scan-result indicator. The colour mirrors
 * the UI palette: green = product found, amber = not found, coral = error.
 * Pure ESP-side (RMT/led_strip) — it issues no GM67 command and does not touch
 * the scanner UART, so it is independent of the GM67 ACK/latency questions. */
typedef enum {
    STATUS_LED_GREEN,  /* product found */
    STATUS_LED_AMBER,  /* product not found */
    STATUS_LED_CORAL,  /* error */
} status_led_color_t;

esp_err_t status_led_init(void);

/* Enable/disable the result flash (settings screen toggle). Disabling also
 * clears any pixel that is currently lit. */
void status_led_set_enabled(bool enabled);

/* Briefly flash the result colour, then auto-off after a short dwell. A no-op
 * when disabled or before init. Call from the app task. */
void status_led_flash(status_led_color_t color);
