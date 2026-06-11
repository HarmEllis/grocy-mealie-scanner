#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Brings up the ILI9341 panel + FT6336U touch and starts the LVGL port
 * task. After this returns successfully, lvgl_port_lock()/unlock() guard
 * every LVGL call from other tasks. */
esp_err_t display_init(void);

/* Backlight control (on by default after display_init). */
void display_backlight(bool on);
