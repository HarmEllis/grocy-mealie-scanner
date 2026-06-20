/* Host unit tests for the GM67 stream demultiplexer and command tables.
 *
 * Pure C, no ESP-IDF — build and run on the host:
 *   cc -I ../main -o /tmp/t test_gm67_demux.c ../main/gm67_proto.c && /tmp/t
 * or via test/run.sh. See docs/GM67-IMPROVEMENT-PLAN.md §7. */
#include "gm67_proto.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

/* ---- demux harness ---- */

typedef struct {
    char         text[256];
    size_t       text_len;
    gm67_reply_t replies[32];
    size_t       reply_len;
} capture_t;

static void on_text(void *ctx, uint8_t b)
{
    capture_t *c = ctx;
    if (c->text_len < sizeof(c->text) - 1) {
        c->text[c->text_len++] = (char)b;
        c->text[c->text_len] = '\0';
    }
}

static void on_reply(void *ctx, gm67_reply_t kind)
{
    capture_t *c = ctx;
    if (c->reply_len < 32) {
        c->replies[c->reply_len++] = kind;
    }
}

/* Feed `data` one byte at a time (worst-case chunking) and also as one block;
 * both must produce identical output. */
static void run_both(const uint8_t *data, size_t n, capture_t *out_bytewise)
{
    gm67_demux_t d;
    capture_t block = {0};
    gm67_demux_init(&d, on_text, on_reply, &block);
    gm67_demux_feed(&d, data, n);
    gm67_demux_flush(&d);

    capture_t bw = {0};
    gm67_demux_init(&d, on_text, on_reply, &bw);
    for (size_t i = 0; i < n; i++) {
        gm67_demux_feed(&d, &data[i], 1);
    }
    gm67_demux_flush(&d);

    /* Chunking must not change the result. */
    CHECK(block.text_len == bw.text_len);
    CHECK(memcmp(block.text, bw.text, block.text_len) == 0);
    CHECK(block.reply_len == bw.reply_len);
    CHECK(memcmp(block.replies, bw.replies, block.reply_len * sizeof(gm67_reply_t)) == 0);

    *out_bytewise = bw;
}

static const uint8_t ACK[]        = {0x04,0xD0,0x04,0x00,0xFF,0x28};
static const uint8_t NAK_RESEND[] = {0x05,0xD1,0x04,0x00,0x01,0xFF,0x25};
static const uint8_t NAK_BADCTX[] = {0x05,0xD1,0x04,0x00,0x02,0xFF,0x24};
static const uint8_t NAK_DENIED[] = {0x05,0xD1,0x04,0x00,0x06,0xFF,0x20};

int main(void)
{
    capture_t c;

    /* 1. checksum/table integrity */
    CHECK(gm67_frame_valid(ACK, sizeof(ACK)));
    CHECK(gm67_frame_valid(NAK_RESEND, sizeof(NAK_RESEND)));
    CHECK(gm67_frame_valid(NAK_BADCTX, sizeof(NAK_BADCTX)));
    CHECK(gm67_frame_valid(NAK_DENIED, sizeof(NAK_DENIED)));

    /* 1b. runtime beep toggle frames (Phase 2) carry valid checksums */
    CHECK(gm67_frame_valid(gm67_cmd_beep_on.bytes, gm67_cmd_beep_on.len));
    CHECK(gm67_frame_valid(gm67_cmd_beep_off.bytes, gm67_cmd_beep_off.len));
    /* On/off differ only in the param byte (0x01 vs 0x00) and the checksum */
    CHECK(gm67_cmd_beep_on.len == gm67_cmd_beep_off.len);
    CHECK(gm67_cmd_beep_on.bytes[6] == 0x01 && gm67_cmd_beep_off.bytes[6] == 0x00);

    /* 1c. control command frames (§3 Opcode Table) carry valid checksums */
    CHECK(gm67_frame_valid(gm67_cmd_scan_enable.bytes,  gm67_cmd_scan_enable.len));
    CHECK(gm67_frame_valid(gm67_cmd_scan_disable.bytes, gm67_cmd_scan_disable.len));
    CHECK(gm67_frame_valid(gm67_cmd_start_decode.bytes, gm67_cmd_start_decode.len));
    CHECK(gm67_frame_valid(gm67_cmd_stop_decode.bytes,  gm67_cmd_stop_decode.len));
    CHECK(gm67_frame_valid(gm67_cmd_beep_cue.bytes,     gm67_cmd_beep_cue.len));
    CHECK(gm67_frame_valid(gm67_cmd_sleep.bytes,        gm67_cmd_sleep.len));
    /* Opcodes match the reference document */
    CHECK(gm67_cmd_scan_enable.bytes[1]  == 0xE9);
    CHECK(gm67_cmd_scan_disable.bytes[1] == 0xEA);
    CHECK(gm67_cmd_start_decode.bytes[1] == 0xE4);
    CHECK(gm67_cmd_stop_decode.bytes[1]  == 0xE5);
    CHECK(gm67_cmd_beep_cue.bytes[1]     == 0xE6);
    CHECK(gm67_cmd_sleep.bytes[1]        == 0xEB);

    /* 2. scan-only: a plain barcode passes through untouched, no replies */
    {
        const char *bc = "5901234123457\r\n";
        run_both((const uint8_t *)bc, strlen(bc), &c);
        CHECK(c.reply_len == 0);
        CHECK(strcmp(c.text, bc) == 0);
    }

    /* 3. reply-only: an ACK is consumed, nothing reaches the text path */
    {
        run_both(ACK, sizeof(ACK), &c);
        CHECK(c.text_len == 0);
        CHECK(c.reply_len == 1 && c.replies[0] == GM67_REPLY_ACK);
    }

    /* 4. scan+reply */
    {
        uint8_t buf[64];
        const char *bc = "12345678\r\n";
        size_t n = 0;
        memcpy(buf, bc, strlen(bc)); n += strlen(bc);
        memcpy(buf + n, ACK, sizeof(ACK)); n += sizeof(ACK);
        run_both(buf, n, &c);
        CHECK(strcmp(c.text, bc) == 0);
        CHECK(c.reply_len == 1 && c.replies[0] == GM67_REPLY_ACK);
    }

    /* 5. reply+scan */
    {
        uint8_t buf[64];
        const char *bc = "0012345678905\r\n";
        size_t n = 0;
        memcpy(buf, ACK, sizeof(ACK)); n += sizeof(ACK);
        memcpy(buf + n, bc, strlen(bc)); n += strlen(bc);
        run_both(buf, n, &c);
        CHECK(strcmp(c.text, bc) == 0);
        CHECK(c.reply_len == 1 && c.replies[0] == GM67_REPLY_ACK);
    }

    /* 6. NAK variant classification — all three must be distinguished */
    {
        run_both(NAK_RESEND, sizeof(NAK_RESEND), &c);
        CHECK(c.text_len == 0);
        CHECK(c.reply_len == 1 && c.replies[0] == GM67_REPLY_NAK_RESEND);

        run_both(NAK_BADCTX, sizeof(NAK_BADCTX), &c);
        CHECK(c.text_len == 0);
        CHECK(c.reply_len == 1 && c.replies[0] == GM67_REPLY_NAK_BAD_CONTEXT);

        run_both(NAK_DENIED, sizeof(NAK_DENIED), &c);
        CHECK(c.text_len == 0);
        CHECK(c.reply_len == 1 && c.replies[0] == GM67_REPLY_NAK_DENIED);
    }

    /* 7. partial signature that never completes → rolled back as text on flush.
     *    0x04 0xD0 is the ACK prefix; truncating it must lose nothing. */
    {
        const uint8_t buf[] = {0x04, 0xD0};
        run_both(buf, sizeof(buf), &c);
        CHECK(c.reply_len == 0);
        CHECK(c.text_len == 2 && (uint8_t)c.text[0] == 0x04 && (uint8_t)c.text[1] == 0xD0);
    }

    /* 8. prefix that breaks on a wrong byte → whole run rolls back as text.
     *    0x04 0xD0 0x04 0x99 diverges from ACK at the 4th byte. */
    {
        const uint8_t buf[] = {0x04, 0xD0, 0x04, 0x99};
        run_both(buf, sizeof(buf), &c);
        CHECK(c.reply_len == 0);
        CHECK(c.text_len == 4);
        CHECK((uint8_t)c.text[3] == 0x99);
    }

    /* 8b. correct structure but a wrong checksum byte must NOT be taken as a
     *     reply — the full run rolls back to text. ACK with FF 27 (should be 28). */
    {
        const uint8_t buf[] = {0x04, 0xD0, 0x04, 0x00, 0xFF, 0x27};
        run_both(buf, sizeof(buf), &c);
        CHECK(c.reply_len == 0);
        CHECK(c.text_len == 6);
        CHECK((uint8_t)c.text[5] == 0x27);
    }

    /* 9. two replies back to back */
    {
        uint8_t buf[32];
        size_t n = 0;
        memcpy(buf, ACK, sizeof(ACK)); n += sizeof(ACK);
        memcpy(buf + n, ACK, sizeof(ACK)); n += sizeof(ACK);
        run_both(buf, n, &c);
        CHECK(c.text_len == 0);
        CHECK(c.reply_len == 2);
    }

    /* 10. false-start inside text: a stray 0x05 that is not a NAK rolls back */
    {
        const uint8_t buf[] = {'A', 0x05, 'B', '\r', '\n'};
        run_both(buf, sizeof(buf), &c);
        CHECK(c.reply_len == 0);
        CHECK(c.text_len == 5);
    }

    if (g_failures == 0) {
        printf("OK: all gm67 demux tests passed\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
