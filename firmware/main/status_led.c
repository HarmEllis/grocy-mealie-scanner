/* Scan-result indicator on the board WS2812 (GPIO42). A flash sets the pixel
 * and an esp_timer one-shot clears it after a short dwell, so the app task
 * never blocks waiting for the LED to turn off. See docs/GM67-IMPROVEMENT-PLAN
 * §4.2 (Phase 2). */
#include "status_led.h"
#include "board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include <stddef.h>

static const char *TAG = "status_led";

#define STATUS_LED_FLASH_MS   220
#define STATUS_LED_BRIGHTNESS  64  /* 0..255 dimming applied to the palette colour */

static led_strip_handle_t s_strip;
static esp_timer_handle_t s_off_timer;
static bool s_enabled = true;

/* Mirrors the UI colours from the design export. */
static const struct { uint8_t r, g, b; } COLORS[] = {
    [STATUS_LED_GREEN] = {0x35, 0xc9, 0x8c},
    [STATUS_LED_AMBER] = {0xf5, 0xc1, 0x3d},
    [STATUS_LED_CORAL] = {0xe8, 0x67, 0x4a},
};

static inline uint8_t scale(uint8_t c)
{
    return (uint8_t)((c * STATUS_LED_BRIGHTNESS) / 255);
}

static void off_cb(void *arg)
{
    (void)arg;
    if (s_strip != NULL) {
        led_strip_clear(s_strip);
    }
}

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_PIN_RGB_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);

    const esp_timer_create_args_t off_args = {
        .callback = off_cb,
        .name = "led_off",
    };
    err = esp_timer_create(&off_args, &s_off_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "off timer create failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "status LED on GPIO%d", BOARD_PIN_RGB_LED);
    return ESP_OK;
}

void status_led_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled && s_strip != NULL) {
        esp_timer_stop(s_off_timer);
        led_strip_clear(s_strip);
    }
}

void status_led_flash(status_led_color_t color)
{
    if (!s_enabled || s_strip == NULL ||
        (size_t)color >= sizeof(COLORS) / sizeof(COLORS[0])) {
        return;
    }
    /* Restart the dwell if a previous flash is still lit. esp_timer_stop does
     * not preempt an off_cb already mid-run, so refresh here could briefly race
     * a clear on the one RMT channel; worst case is a dropped update on a
     * cosmetic LED, never a crash, so it is left unsynchronised on purpose. */
    esp_timer_stop(s_off_timer);
    led_strip_set_pixel(s_strip, 0, scale(COLORS[color].r), scale(COLORS[color].g),
                        scale(COLORS[color].b));
    led_strip_refresh(s_strip);
    esp_timer_start_once(s_off_timer, (uint64_t)STATUS_LED_FLASH_MS * 1000);
}
