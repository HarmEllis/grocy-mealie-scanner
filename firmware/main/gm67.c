#include "gm67.h"
#include "board.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <string.h>

static const char *TAG = "gm67";

#define UART_RX_BUF_SIZE 512
#define READ_TIMEOUT_MS  50

static gm67_scan_cb_t s_cb;
static uint32_t s_debounce_ms;

static bool code_is_plausible(const char *code, size_t len)
{
    if (len < 4 || len >= GM67_MAX_CODE_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)code[i];
        if (!isalnum(c) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

static void reader_task(void *arg)
{
    uint8_t rx[UART_RX_BUF_SIZE];
    char line[GM67_MAX_CODE_LEN];
    size_t line_len = 0;
    char last_code[GM67_MAX_CODE_LEN] = "";
    int64_t last_code_us = 0;

    for (;;) {
        int n = uart_read_bytes(BOARD_GM67_UART_NUM, rx, sizeof(rx),
                                pdMS_TO_TICKS(READ_TIMEOUT_MS));
        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            if (c != '\r' && c != '\n') {
                if (line_len < sizeof(line) - 1) {
                    line[line_len++] = c;
                } else {
                    /* Oversized garbage; restart on the next terminator. */
                    line_len = sizeof(line) - 1;
                }
                continue;
            }
            if (line_len == 0) {
                continue;
            }
            line[line_len] = '\0';
            size_t len = line_len;
            line_len = 0;

            if (!code_is_plausible(line, len)) {
                ESP_LOGW(TAG, "dropping implausible scan payload (%u bytes)",
                         (unsigned)len);
                continue;
            }

            int64_t now = esp_timer_get_time();
            bool duplicate = strcmp(line, last_code) == 0 &&
                             (now - last_code_us) < (int64_t)s_debounce_ms * 1000;
            strlcpy(last_code, line, sizeof(last_code));
            last_code_us = now;
            if (duplicate) {
                continue;
            }

            ESP_LOGI(TAG, "scan: %s", line);
            s_cb(line);
        }
        /* A code that arrived without a trailing CR/LF is flushed once the
         * line stays idle for one read timeout. Some GM67 terminator
         * configurations send none. */
        if (n == 0 && line_len > 0) {
            line[line_len] = '\0';
            size_t len = line_len;
            line_len = 0;
            if (code_is_plausible(line, len)) {
                int64_t now = esp_timer_get_time();
                bool duplicate = strcmp(line, last_code) == 0 &&
                                 (now - last_code_us) < (int64_t)s_debounce_ms * 1000;
                strlcpy(last_code, line, sizeof(last_code));
                last_code_us = now;
                if (!duplicate) {
                    ESP_LOGI(TAG, "scan (idle flush): %s", line);
                    s_cb(line);
                }
            }
        }
    }
}

esp_err_t gm67_init(gm67_scan_cb_t cb, uint32_t debounce_ms)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "cb required");
    s_cb = cb;
    s_debounce_ms = debounce_ms;

    const uart_config_t cfg = {
        .baud_rate = BOARD_GM67_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(BOARD_GM67_UART_NUM, UART_RX_BUF_SIZE * 2,
                                            0, 0, NULL, 0),
                        TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(BOARD_GM67_UART_NUM, &cfg), TAG, "uart cfg");
    ESP_RETURN_ON_ERROR(uart_set_pin(BOARD_GM67_UART_NUM, BOARD_GM67_PIN_TX,
                                     BOARD_GM67_PIN_RX, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart pins");

    BaseType_t ok = xTaskCreate(reader_task, "gm67", 3072, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "GM67 reader on UART%d (TX=%d RX=%d, %d baud)",
             BOARD_GM67_UART_NUM, BOARD_GM67_PIN_TX, BOARD_GM67_PIN_RX, BOARD_GM67_BAUD);
    return ESP_OK;
}
