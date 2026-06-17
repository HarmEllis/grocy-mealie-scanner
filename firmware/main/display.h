#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/* Brings up the ILI9341 panel + FT6336U touch and starts the LVGL port
 * task. After this returns successfully, lvgl_port_lock()/unlock() guard
 * every LVGL call from other tasks. */
esp_err_t display_init(void);

/* Backlight control (on by default after display_init). */
void display_backlight(bool on);

/* Combined sleep/wake: backlight + panel on/off.
 * sleep=true  → backlight off, then panel off (ILI9341 display off).
 * sleep=false → panel on, LVGL full-screen invalidate + forced redraw,
 *               then backlight on (so light appears only after a clean frame).
 * Call from any task; acquires LVGL port lock internally. */
void display_sleep(bool sleep);

/* Applies a per-axis linear correction derived from the averaged raw
 * coordinates at the four calibration target edges. Returns false when either
 * measured axis is too small to produce a stable correction. */
bool display_touch_set_calibration(int32_t x_left, int32_t x_right,
                                   int32_t y_top, int32_t y_bottom);

/* Restores the touch controller's raw coordinates without correction. */
void display_touch_set_identity(void);

/* While capture is enabled, raw coordinates pass through unchanged and the
 * first sample is latched until display_touch_get_sample() consumes it. */
void display_touch_capture(bool enabled);
bool display_touch_get_sample(uint16_t *x, uint16_t *y);
