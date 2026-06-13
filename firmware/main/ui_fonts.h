#pragma once

#include "esp_err.h"
#include "lvgl.h"

extern lv_font_t gms_font_10;
extern lv_font_t gms_font_12;
extern lv_font_t gms_font_14;
extern lv_font_t gms_font_18;
extern lv_font_t gms_font_20;
extern lv_font_t gms_font_22;
extern lv_font_t gms_font_24;

/* Adds an embedded Montserrat TTF fallback to the fast LVGL bitmap fonts.
 * Normal ASCII and LVGL symbols stay bitmap-rendered; missing Latin glyphs
 * (including Dutch diacritics and accented product names) use the fallback. */
esp_err_t ui_fonts_init(void);
