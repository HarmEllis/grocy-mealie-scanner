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
#define GM67_ACK_TIMEOUT_MS  150  /* per-command ACK window (§3.2) */
#define GM67_CFG_RESEND_MAX  3    /* resends on CMD_NAK RESEND before giving up */
/* Upper bound on boot configuration: well above the worst case (wake + 8
 * commands, each up to GM67_ACK_TIMEOUT_MS with retries, or an early abort). If
 * the task has not signalled readiness by then it is genuinely stuck, which
 * should fail gm67_init() so the image is not marked OTA-valid. */
#define GM67_READY_TIMEOUT_MS 10000

/* Runtime command queue (Phase 2). Holds fire-and-forget PARAM_SEND frames the
 * owning task drains and writes between reads, so the UART keeps a single
 * owner. GM67_CMD_MAX_BYTES covers the longest such frame (the 9-byte beep
 * toggle) with headroom. */
#define GM67_CMD_QUEUE_LEN 4
#define GM67_CMD_MAX_BYTES 16

typedef struct {
    uint8_t bytes[GM67_CMD_MAX_BYTES];
    uint8_t len;
} gm67_cmd_msg_t;

static gm67_scan_cb_t    s_cb;
static uint32_t          s_debounce_ms;
static gm67_demux_t      s_demux;
static QueueHandle_t     s_cmd_queue;  /* runtime fire-and-forget commands */
static SemaphoreHandle_t s_ready_sem; /* given once the task is ready */
/* False until boot configuration completes; while false the line assembler
 * discards bytes so a pre-config scan is never surfaced. */
static volatile bool     s_ready;
/* Software scanning gate: set false by gm67_set_scanning(false) while the
 * display is asleep; submit_code() drops codes until re-enabled. */
static _Atomic bool      s_scanning_enabled = true;

/* All reader/transaction state lives in the owning task and is reached from the
 * demux callbacks via its ctx pointer. */
typedef struct {
    char         line[GM67_MAX_CODE_LEN];
    size_t       line_len;
    bool         overflow;       /* current code exceeded the buffer; drop it whole */
    char         last_code[GM67_MAX_CODE_LEN];
    int64_t      last_code_us;
    bool         got_reply;      /* a reply frame arrived (config transactions) */
    gm67_reply_t last_reply;
} gm67_runtime_t;

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

static void submit_code(gm67_runtime_t *rt, const char *code, size_t len, const char *how)
{
    if (!atomic_load(&s_scanning_enabled)) {
        return;
    }
    if (!code_is_plausible(code, len)) {
        ESP_LOGW(TAG, "dropping implausible scan payload (%u bytes)", (unsigned)len);
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

/* Demux text callback: the original line assembler, now fed one byte at a time
 * from behind the reply demultiplexer. CR/LF terminate a code; a code that
 * overruns the buffer is dropped whole so we never submit a truncated value. */
static void on_text(void *ctx, uint8_t byte)
{
    gm67_runtime_t *rt = ctx;
    if (!s_ready) {
        return; /* discard pre-config scan bytes */
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

/* Demux reply callback: records the frame for a pending config transaction. At
 * runtime no transaction is pending and this is harmless (Phase 1 sends no
 * runtime commands, so no reply frames occur after config). */
static void on_reply(void *ctx, gm67_reply_t kind)
{
    gm67_runtime_t *rt = ctx;
    rt->got_reply = true;
    rt->last_reply = kind;
}

/* A code that arrived without a trailing CR/LF is flushed once the line stays
 * idle for one read timeout — some GM67 terminator configs send none, and a
 * config abort leaves the module in whatever mode it shipped in. */
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

/* Send one config command and wait for its ACK. Destructive input flush here is
 * safe: it runs only at startup, before any scan is surfaced (§3.2 step 1). */
static esp_err_t send_cmd_wait_ack(gm67_runtime_t *rt, const gm67_cmd_t *cmd)
{
    int retries = 0;
    for (;;) {
        uart_flush_input(BOARD_GM67_UART_NUM);
        rt->got_reply = false;
        uart_write_bytes(BOARD_GM67_UART_NUM, cmd->bytes, cmd->len);

        int64_t deadline = esp_timer_get_time() + (int64_t)GM67_ACK_TIMEOUT_MS * 1000;
        while (!rt->got_reply) {
            uint8_t rx[64];
            int n = uart_read_bytes(BOARD_GM67_UART_NUM, rx, sizeof(rx),
                                    pdMS_TO_TICKS(10));
            if (n > 0) {
                gm67_demux_feed(&s_demux, rx, (size_t)n);
            }
            if (!rt->got_reply && esp_timer_get_time() >= deadline) {
                return ESP_ERR_TIMEOUT;
            }
        }
        if (rt->last_reply == GM67_REPLY_ACK) {
            return ESP_OK;
        }
        if (rt->last_reply == GM67_REPLY_NAK_RESEND && retries < GM67_CFG_RESEND_MAX) {
            retries++;
            continue;
        }
        return ESP_FAIL; /* BAD_CONTEXT / DENIED / resend cap reached */
    }
}

/* Push the Phase 1 configuration. Best-effort and fail-open.
 *
 * Abort policy (§3.2 step 4): only a *timeout* is ambiguous — without a verified
 * max response latency, a late untagged ACK could satisfy a later command, so we
 * stop the whole sequence and fall through to the passive reader. A NAK, by
 * contrast, is definitive and correlated to the current command, so we log it
 * and continue with the remaining settings.
 *
 * Persistence note (§6.1, unresolved): these are parameter writes applied on
 * every boot. That is the accepted Phase 1 behavior on the assumption writes are
 * volatile; if a bench test shows they persist to nonvolatile storage, this must
 * become an idempotent read/compare/write before shipping. Do not merge to a
 * release without resolving §6.1. */
static void gm67_configure(gm67_runtime_t *rt)
{
    /* Any serial byte wakes a module asleep in induction mode. */
    const uint8_t wake = 0x00;
    uart_write_bytes(BOARD_GM67_UART_NUM, &wake, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    for (size_t i = 0; i < gm67_config_seq_len; i++) {
        esp_err_t err = send_cmd_wait_ack(rt, &gm67_config_seq[i]);
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "config timeout at '%s'; aborting rest, using passive read",
                     gm67_config_seq[i].name);
            return;
        }
        if (err != ESP_OK) {
            /* Definitive NAK: this setting did not take, but it is correlated to
             * this command, so the rest of the sequence is still safe to send. */
            ESP_LOGW(TAG, "config '%s' rejected; continuing", gm67_config_seq[i].name);
            continue;
        }
        ESP_LOGI(TAG, "config: %s OK", gm67_config_seq[i].name);
    }
    ESP_LOGI(TAG, "GM67 configuration sequence complete");
}

static void gm67_task(void *arg)
{
    (void)arg;
    static gm67_runtime_t rt;
    memset(&rt, 0, sizeof(rt));
    gm67_demux_init(&s_demux, on_text, on_reply, &rt);

    gm67_configure(&rt);
    /* Discard any partial reply prefix the module left mid-frame (e.g. an
     * aborted config sequence): with s_ready still false, on_text drops these,
     * so they cannot prepend to the first real scan. */
    gm67_demux_flush(&s_demux);
    s_ready = true;
    xSemaphoreGive(s_ready_sem); /* unblock gm67_init() */
    ESP_LOGI(TAG, "GM67 reader running");

    uint8_t rx[UART_RX_BUF_SIZE];
    for (;;) {
        /* Drain runtime commands first: this task is the single UART owner, so
         * fire-and-forget frames (the beep toggle) are written here rather than
         * by the caller. No ACK is awaited — the module's reply is consumed by
         * the demux's always-strip path (no transaction pending → dropped). */
        gm67_cmd_msg_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            uart_write_bytes(BOARD_GM67_UART_NUM, cmd.bytes, cmd.len);
            ESP_LOGI(TAG, "runtime command sent (%u bytes)", (unsigned)cmd.len);
        }

        int n = uart_read_bytes(BOARD_GM67_UART_NUM, rx, sizeof(rx),
                                pdMS_TO_TICKS(READ_TIMEOUT_MS));
        if (n > 0) {
            gm67_demux_feed(&s_demux, rx, (size_t)n);
        } else {
            /* Phase 2 note: a runtime command's ACK can now arrive split across
             * reads, leaving a partial reply prefix in the demux when the line
             * goes idle. gm67_demux_flush then rolls those bytes to on_text.
             * This is benign: the ACK/NAK bytes are non-printable, so
             * code_is_plausible rejects the assembled "line", and a toggle is
             * only ever sent from the settings screen — never while a real code
             * is mid-flight — so it cannot corrupt a live scan. */
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

    /* Create the command queue before the task starts so a gm67_set_beep() call
     * made right after gm67_init() returns is never dropped for want of a queue. */
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

    /* Block until the task finishes boot configuration and is reading, so the
     * caller can safely mark the OTA image valid (§3.4). A timeout means the
     * task is stuck; surface it rather than mark a broken image good. */
    if (xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(GM67_READY_TIMEOUT_MS)) != pdTRUE) {
        /* Stuck task; the caller aborts on this error and reboots, so leave the
         * semaphore in place — the task may still give it and a delete here
         * would race that. */
        ESP_LOGE(TAG, "GM67 task not ready within %d ms", GM67_READY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    /* Handshake done (the task gives it exactly once); reclaim it. */
    vSemaphoreDelete(s_ready_sem);
    s_ready_sem = NULL;
    return ESP_OK;
}

esp_err_t gm67_set_beep(bool enabled)
{
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE; /* called before gm67_init() */
    }
    const gm67_cmd_t *cmd = enabled ? &gm67_cmd_beep_on : &gm67_cmd_beep_off;
    gm67_cmd_msg_t msg;
    if (cmd->len > sizeof(msg.bytes)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(msg.bytes, cmd->bytes, cmd->len);
    msg.len = cmd->len;
    return xQueueSend(s_cmd_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t gm67_set_scanning(bool enabled)
{
    if (!enabled) {
        /* Disable: set the software gate first so codes racing through before
         * SCAN_DISABLE reaches hardware are still blocked.  The hardware
         * command is best-effort — a queue-full means the module keeps
         * scanning, but the software gate is the authoritative guard. */
        atomic_store(&s_scanning_enabled, false);
        if (s_cmd_queue != NULL) {
            gm67_cmd_msg_t msg;
            if (gm67_cmd_scan_disable.len <= sizeof(msg.bytes)) {
                memcpy(msg.bytes, gm67_cmd_scan_disable.bytes,
                       gm67_cmd_scan_disable.len);
                msg.len = gm67_cmd_scan_disable.len;
                xQueueSend(s_cmd_queue, &msg, 0); /* best-effort */
            }
        }
        return ESP_OK;
    }

    /* Enable (wake-up): queue SCAN_ENABLE *before* opening the software gate.
     * If the hardware command fails to queue, the module stays idle — opening
     * the gate would make the caller believe scanning is active when it is not.
     * Leave the gate closed and report the error so the caller can retry. */
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    gm67_cmd_msg_t msg;
    if (gm67_cmd_scan_enable.len > sizeof(msg.bytes)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(msg.bytes, gm67_cmd_scan_enable.bytes, gm67_cmd_scan_enable.len);
    msg.len = gm67_cmd_scan_enable.len;
    if (xQueueSend(s_cmd_queue, &msg, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM; /* gate stays closed; caller should retry */
    }
    atomic_store(&s_scanning_enabled, true);
    return ESP_OK;
}
