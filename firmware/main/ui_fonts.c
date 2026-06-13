#include "ui_fonts.h"

#include "esp_log.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "ui_fonts";

extern const uint8_t s_montserrat_ttf_start[]
    asm("_binary_Montserrat_Medium_ttf_start");
extern const uint8_t s_montserrat_ttf_end[]
    asm("_binary_Montserrat_Medium_ttf_end");

lv_font_t gms_font_10;
lv_font_t gms_font_12;
lv_font_t gms_font_14;
lv_font_t gms_font_18;
lv_font_t gms_font_20;
lv_font_t gms_font_22;
lv_font_t gms_font_24;

static lv_font_t *s_fallbacks[7];

static bool has_glyph(const lv_font_t *font, uint32_t codepoint)
{
    lv_font_glyph_dsc_t glyph;
    return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) &&
           glyph.resolved_font != NULL;
}

static esp_err_t init_font(lv_font_t *font, const lv_font_t *bitmap,
                           lv_font_t **fallback, int size)
{
    size_t data_size = (size_t)(s_montserrat_ttf_end - s_montserrat_ttf_start);
    *fallback = lv_tiny_ttf_create_data_ex(s_montserrat_ttf_start, data_size, size,
                                           LV_FONT_KERNING_NONE, 4);
    if (*fallback == NULL) {
        ESP_LOGE(TAG, "failed to create %d px fallback", size);
        return ESP_ERR_NO_MEM;
    }
    *font = *bitmap;
    font->fallback = *fallback;
    return ESP_OK;
}

esp_err_t ui_fonts_init(void)
{
    struct {
        lv_font_t *font;
        const lv_font_t *bitmap;
        int size;
    } fonts[] = {
        { &gms_font_10, &lv_font_montserrat_10, 10 },
        { &gms_font_12, &lv_font_montserrat_12, 12 },
        { &gms_font_14, &lv_font_montserrat_14, 14 },
        { &gms_font_18, &lv_font_montserrat_18, 18 },
        { &gms_font_20, &lv_font_montserrat_20, 20 },
        { &gms_font_22, &lv_font_montserrat_22, 22 },
        { &gms_font_24, &lv_font_montserrat_24, 24 },
    };

    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        esp_err_t err = init_font(fonts[i].font, fonts[i].bitmap,
                                  &s_fallbacks[i], fonts[i].size);
        if (err != ESP_OK) {
            return err;
        }
    }

    static const uint32_t required_glyphs[] = {
        0x00E9, /* e acute */
        0x00EB, /* e diaeresis */
        0x00EF, /* i diaeresis */
    };
    for (size_t i = 0; i < sizeof(required_glyphs) / sizeof(required_glyphs[0]); i++) {
        if (!has_glyph(&gms_font_14, required_glyphs[i])) {
            ESP_LOGE(TAG, "missing required glyph U+%04" PRIX32, required_glyphs[i]);
            return ESP_ERR_NOT_FOUND;
        }
    }

    ESP_LOGI(TAG, "Latin glyph fallback ready (%u-byte TTF)",
             (unsigned)(s_montserrat_ttf_end - s_montserrat_ttf_start));
    return ESP_OK;
}
