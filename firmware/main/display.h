#pragma once

#include "esp_err.h"
#include "lvgl.h"

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
