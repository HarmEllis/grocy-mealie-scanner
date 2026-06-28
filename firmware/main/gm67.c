#include "gm67.h"
#include "gm67_proto.h"
#include "board.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "gm67";

#define UART_RX_BUF_SIZE     512
#define READ_TIMEOUT_MS      50
/* If gm67_task has not signalled ready within this window the scanner is
 * stuck; gm67_init() surfaces the error so the caller can abort/reboot. */
#define GM67_READY_TIMEOUT_MS 3000

/* Runtime command queue.  Holds fire-and-forget PARAM_SEND frames the
 * owning task drains and writes between reads, so the UART keeps a single
 * owner.  GM67_CMD_MAX_BYTES covers the longest PARAM_SEND frame (10 bytes)
 * with headroom. */
#define GM67_CMD_QUEUE_LEN 8
#define GM67_CMD_MAX_BYTES 16

typedef struct {
    uint8_t bytes[GM67_CMD_MAX_BYTES];
    uint8_t len;
    char    name[16];
} gm67_cmd_msg_t;

static gm67_scan_cb_t    s_cb;
static uint32_t          s_debounce_ms;
static gm67_demux_t      s_demux;
static QueueHandle_t     s_cmd_queue;
static SemaphoreHandle_t s_ready_sem;
/* False until the task sends SCAN_ENABLE at startup and is ready. */
static volatile bool     s_ready;
/* Software scanning gate: closed while the display is sleeping. */
static _Atomic bool      s_scanning_enabled = true;

typedef struct {
    char    line[GM67_MAX_CODE_LEN];
    size_t  line_len;
    bool    overflow;
    char    last_code[GM67_MAX_CODE_LEN];
    int64_t last_code_us;
} gm67_runtime_t;

static bool code_is_plausible(const char *code, size_t len)
{
    if (len < 4 || len >= GM67_MAX_CODE_LEN) {
        return false;
    }
    bool setup_qr = len >= 5 && memcmp(code, "GMS1_", 5) == 0;
    if (!setup_qr && len >= GM67_MAX_BARCODE_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)code[i];
        if (!isalnum(c) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

static void submit_code(gm67_runtime_t *rt, const char *code, size_t len, const char *how)
{
    if (!atomic_load(&s_scanning_enabled)) {
        return;
    }
    if (!code_is_plausible(code, len)) {
        ESP_LOGD(TAG, "dropping implausible scan payload (%u bytes)", (unsigned)len);
        return;
    }
    int64_t now = esp_timer_get_time();
    bool duplicate = strcmp(code, rt->last_code) == 0 &&
                     (now - rt->last_code_us) < (int64_t)s_debounce_ms * 1000;
    strlcpy(rt->last_code, code, sizeof(rt->last_code));
    rt->last_code_us = now;
    if (duplicate) {
        return;
    }
    ESP_LOGI(TAG, "scan%s: %s", how, code);
    s_cb(code);
}

static void on_text(void *ctx, uint8_t byte)
{
    gm67_runtime_t *rt = ctx;
    if (!s_ready) {
        return;
    }
    char c = (char)byte;
    if (c != '\r' && c != '\n') {
        if (rt->line_len < sizeof(rt->line) - 1) {
            rt->line[rt->line_len++] = c;
        } else {
            rt->overflow = true;
        }
        return;
    }
    if (rt->line_len == 0 && !rt->overflow) {
        return;
    }
    if (rt->overflow) {
        ESP_LOGW(TAG, "dropping over-length scan payload (>%u chars)",
                 (unsigned)(sizeof(rt->line) - 1));
        rt->line_len = 0;
        rt->overflow = false;
        return;
    }
    rt->line[rt->line_len] = '\0';
    size_t len = rt->line_len;
    rt->line_len = 0;
    submit_code(rt, rt->line, len, "");
}

static void idle_flush(gm67_runtime_t *rt)
{
    if (rt->overflow) {
        rt->line_len = 0;
        rt->overflow = false;
        return;
    }
    if (rt->line_len == 0) {
        return;
    }
    rt->line[rt->line_len] = '\0';
    size_t len = rt->line_len;
    rt->line_len = 0;
    submit_code(rt, rt->line, len, " (idle flush)");
}

static void gm67_task(void *arg)
{
    (void)arg;
    static gm67_runtime_t rt;
    memset(&rt, 0, sizeof(rt));
    gm67_demux_init(&s_demux, on_text, NULL, &rt);

    /* Drive the scan engine to match the software gate.  SCAN_ENABLE/DISABLE
     * persists in the scanner's NVS, so we must assert the desired state on every
     * boot: without it the LED state from the last session (e.g. screen asleep)
     * carries over.  s_scanning_enabled is normally true, but a caller may close
     * the gate before gm67_init() (boot landing on the connection-error/setup
     * screen) — honour that so the scanner stays dark until we are online. */
    if (atomic_load(&s_scanning_enabled)) {
        uart_write_bytes(BOARD_GM67_UART_NUM, gm67_cmd_scan_enable.bytes,
                         gm67_cmd_scan_enable.len);
    } else {
        uart_write_bytes(BOARD_GM67_UART_NUM, gm67_cmd_scan_disable.bytes,
                         gm67_cmd_scan_disable.len);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    uart_flush_input(BOARD_GM67_UART_NUM);
    s_ready = true;
    xSemaphoreGive(s_ready_sem);
    ESP_LOGI(TAG, "GM67 reader running");

    uint8_t rx[UART_RX_BUF_SIZE];
    for (;;) {
        /* Drain runtime commands first: this task is the single UART owner, so
         * fire-and-forget frames are written here rather than by the caller. */
        gm67_cmd_msg_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            uart_write_bytes(BOARD_GM67_UART_NUM, cmd.bytes, cmd.len);
            ESP_LOGI(TAG, "runtime command sent: %s (%u bytes)", cmd.name, (unsigned)cmd.len);
        }

        int n = uart_read_bytes(BOARD_GM67_UART_NUM, rx, sizeof(rx),
                                pdMS_TO_TICKS(READ_TIMEOUT_MS));
        if (n > 0) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx, (size_t)n, ESP_LOG_DEBUG);
            gm67_demux_feed(&s_demux, rx, (size_t)n);
        } else {
            gm67_demux_flush(&s_demux);
            idle_flush(&rt);
        }
    }
}

esp_err_t gm67_init(gm67_scan_cb_t cb, uint32_t debounce_ms)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "cb required");
    s_cb = cb;
    s_debounce_ms = debounce_ms;

    s_ready_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_ready_sem != NULL, ESP_ERR_NO_MEM, TAG, "ready sem");

    s_cmd_queue = xQueueCreate(GM67_CMD_QUEUE_LEN, sizeof(gm67_cmd_msg_t));
    ESP_RETURN_ON_FALSE(s_cmd_queue != NULL, ESP_ERR_NO_MEM, TAG, "cmd queue");

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

    BaseType_t ok = xTaskCreate(gm67_task, "gm67", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "GM67 reader on UART%d (TX=%d RX=%d, %d baud)",
             BOARD_GM67_UART_NUM, BOARD_GM67_PIN_TX, BOARD_GM67_PIN_RX, BOARD_GM67_BAUD);

    if (xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(GM67_READY_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "GM67 task not ready within %d ms", GM67_READY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(s_ready_sem);
    s_ready_sem = NULL;
    return ESP_OK;
}

/* Enqueue a single fire-and-forget command. */
static esp_err_t enqueue_cmd(const gm67_cmd_t *cmd)
{
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cmd->len > GM67_CMD_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    gm67_cmd_msg_t msg;
    memcpy(msg.bytes, cmd->bytes, cmd->len);
    msg.len = cmd->len;
    strlcpy(msg.name, cmd->name, sizeof(msg.name));
    return xQueueSend(s_cmd_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t gm67_set_beep_level(gm67_beep_level_t level)
{
    if (level == GM67_BEEP_OFF) {
        return enqueue_cmd(&gm67_cmd_beep_off);
    }
    esp_err_t err = enqueue_cmd(&gm67_cmd_beep_on);
    if (err != ESP_OK) {
        return err;
    }
    const gm67_cmd_t *vol;
    switch (level) {
    case GM67_BEEP_LOW:    vol = &gm67_cmd_beep_vol_low;  break;
    case GM67_BEEP_HIGH:   vol = &gm67_cmd_beep_vol_high; break;
    default: /* MEDIUM */  vol = &gm67_cmd_beep_vol_med;  break;
    }
    return enqueue_cmd(vol);
}

esp_err_t gm67_set_scanner_light(gm67_light_mode_t mode)
{
    return enqueue_cmd(mode == GM67_LIGHT_ALWAYS_OFF
                       ? &gm67_cmd_light_off
                       : &gm67_cmd_light_on_scan);
}

esp_err_t gm67_set_collimation(gm67_collim_mode_t mode)
{
    return enqueue_cmd(mode == GM67_COLLIM_ALWAYS_OFF
                       ? &gm67_cmd_collim_off
                       : &gm67_cmd_collim_on_scan);
}

esp_err_t gm67_set_scanning(bool enabled)
{
    if (!enabled) {
        atomic_store(&s_scanning_enabled, false);
        if (s_cmd_queue != NULL) {
            gm67_cmd_msg_t msg;
            if (gm67_cmd_scan_disable.len <= sizeof(msg.bytes)) {
                memcpy(msg.bytes, gm67_cmd_scan_disable.bytes,
                       gm67_cmd_scan_disable.len);
                msg.len = gm67_cmd_scan_disable.len;
                strlcpy(msg.name, gm67_cmd_scan_disable.name, sizeof(msg.name));
                xQueueSend(s_cmd_queue, &msg, 0);
            }
        }
        return ESP_OK;
    }

    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    gm67_cmd_msg_t msg;
    if (gm67_cmd_scan_enable.len > sizeof(msg.bytes)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(msg.bytes, gm67_cmd_scan_enable.bytes, gm67_cmd_scan_enable.len);
    msg.len = gm67_cmd_scan_enable.len;
    strlcpy(msg.name, gm67_cmd_scan_enable.name, sizeof(msg.name));
    if (xQueueSend(s_cmd_queue, &msg, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_scanning_enabled, true);
    return ESP_OK;
}
