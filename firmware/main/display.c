#include "display.h"
#include "board.h"
#include "touch_calibration.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>

static const char *TAG = "display";

#define LVGL_BUFFER_LINES 60

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static portMUX_TYPE s_touch_cal_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_touch_scale_x = 1.0f;
static float s_touch_offset_x;
static float s_touch_scale_y = 1.0f;
static float s_touch_offset_y;
static bool s_touch_capture_enabled;
static bool s_touch_sample_valid;
static uint16_t s_touch_sample_x;
static uint16_t s_touch_sample_y;

static uint16_t clamp_coordinate(float value, uint16_t maximum)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= maximum) {
        return maximum;
    }
    return (uint16_t)(value + 0.5f);
}

static void touch_process_coords(esp_lcd_touch_handle_t tp, uint16_t *x,
                                 uint16_t *y, uint16_t *strength,
                                 uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;

    float scale_x;
    float offset_x;
    float scale_y;
    float offset_y;

    portENTER_CRITICAL(&s_touch_cal_lock);
    if (s_touch_capture_enabled) {
        if (*point_num > 0 && !s_touch_sample_valid) {
            s_touch_sample_x = x[0];
            s_touch_sample_y = y[0];
            s_touch_sample_valid = true;
        }
        portEXIT_CRITICAL(&s_touch_cal_lock);
        return;
    }
    scale_x = s_touch_scale_x;
    offset_x = s_touch_offset_x;
    scale_y = s_touch_scale_y;
    offset_y = s_touch_offset_y;
    portEXIT_CRITICAL(&s_touch_cal_lock);

    for (uint8_t i = 0; i < *point_num; i++) {
        x[i] = clamp_coordinate(scale_x * x[i] + offset_x,
                                BOARD_LCD_H_RES - 1);
        y[i] = clamp_coordinate(scale_y * y[i] + offset_y,
                                BOARD_LCD_V_RES - 1);
    }
}

static esp_err_t lcd_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = BOARD_LCD_PIN_MISO,
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * LVGL_BUFFER_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = BOARD_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                                 &io_cfg, &s_panel_io),
                        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, /* tied to board RESET */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(s_panel_io, &panel_cfg, &s_panel),
                        TAG, "ili9341");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    /* Panel quirk (see BOARD_NOTES.md): colours are inverted. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");
    return ESP_OK;
}

static esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t i2c_bus;
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = BOARD_TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_PIN_SDA,
        .scl_io_num = BOARD_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus), TAG, "i2c bus");

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io),
                        TAG, "touch io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_PIN_RST,
        .int_gpio_num = BOARD_TOUCH_PIN_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .process_coordinates = touch_process_coords,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch), TAG, "ft5x06");
    return ESP_OK;
}

esp_err_t display_init(void)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "backlight gpio");
    gpio_set_level(BOARD_LCD_PIN_BL, 0); /* keep dark until first frame */

    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "lcd");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * LVGL_BUFFER_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = s_touch,
    };
    if (lvgl_port_add_touch(&touch_cfg) == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return ESP_FAIL;
    }

    display_backlight(true);
    ESP_LOGI(TAG, "display + touch + LVGL ready (%dx%d portrait)",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

void display_backlight(bool on)
{
    gpio_set_level(BOARD_LCD_PIN_BL, on ? 1 : 0);
}

void display_sleep(bool sleep)
{
    if (sleep) {
        /* Backlight first so the user sees black immediately, then tell the
         * panel to stop driving — avoids a white flash on some ILI9341 lots. */
        lvgl_port_lock(0);
        display_backlight(false);
        esp_lcd_panel_disp_on_off(s_panel, false);
        lvgl_port_unlock();
        ESP_LOGI(TAG, "display sleeping");
    } else {
        /* Wake panel, force a full redraw while LVGL is locked so the SPI
         * transfer completes before we light the backlight. */
        lvgl_port_lock(0);
        esp_lcd_panel_disp_on_off(s_panel, true);
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
        lv_refr_now(NULL);
        lvgl_port_unlock();
        display_backlight(true);
        ESP_LOGI(TAG, "display awake");
    }
}

bool display_touch_set_calibration(int32_t x_left, int32_t x_right,
                                   int32_t y_top, int32_t y_bottom)
{
    int32_t x_span = x_right - x_left;
    int32_t y_span = y_bottom - y_top;
    if (abs(x_span) <= TOUCH_CAL_MIN_SPAN ||
        abs(y_span) <= TOUCH_CAL_MIN_SPAN) {
        return false;
    }

    float scale_x = (float)(TOUCH_CAL_TARGET_RIGHT - TOUCH_CAL_TARGET_LEFT) /
                    (float)x_span;
    float scale_y = (float)(TOUCH_CAL_TARGET_BOTTOM - TOUCH_CAL_TARGET_TOP) /
                    (float)y_span;
    float offset_x = TOUCH_CAL_TARGET_LEFT - scale_x * x_left;
    float offset_y = TOUCH_CAL_TARGET_TOP - scale_y * y_top;

    portENTER_CRITICAL(&s_touch_cal_lock);
    s_touch_scale_x = scale_x;
    s_touch_offset_x = offset_x;
    s_touch_scale_y = scale_y;
    s_touch_offset_y = offset_y;
    portEXIT_CRITICAL(&s_touch_cal_lock);
    return true;
}

void display_touch_set_identity(void)
{
    portENTER_CRITICAL(&s_touch_cal_lock);
    s_touch_scale_x = 1.0f;
    s_touch_offset_x = 0.0f;
    s_touch_scale_y = 1.0f;
    s_touch_offset_y = 0.0f;
    portEXIT_CRITICAL(&s_touch_cal_lock);
}

void display_touch_capture(bool enabled)
{
    portENTER_CRITICAL(&s_touch_cal_lock);
    s_touch_capture_enabled = enabled;
    if (enabled) {
        s_touch_sample_valid = false;
    }
    portEXIT_CRITICAL(&s_touch_cal_lock);
}

bool display_touch_get_sample(uint16_t *x, uint16_t *y)
{
    if (x == NULL || y == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_touch_cal_lock);
    bool valid = s_touch_sample_valid;
    if (valid) {
        *x = s_touch_sample_x;
        *y = s_touch_sample_y;
        s_touch_sample_valid = false;
    }
    portEXIT_CRITICAL(&s_touch_cal_lock);
    return valid;
}
