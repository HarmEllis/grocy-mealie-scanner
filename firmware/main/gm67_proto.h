/* GM67 serial command protocol — pure, host-testable layer.
 *
 * Deliberately free of ESP-IDF / FreeRTOS / UART dependencies so the stream
 * demultiplexer (the riskiest new code) can be unit-tested on the host. See
 * docs/GM67-IMPROVEMENT-PLAN.md §3.4 and §7.
 *
 * Frame format (Appendix 6 of the GM67 manual):
 *   [LEN] [OPCODE] 04 08 00 [params...] [CHK_HI] [CHK_LO]   (host → module)
 *   [LEN] [OPCODE] 04 00 [data...]       [CHK_HI] [CHK_LO]   (module → host)
 * LEN counts every pre-checksum byte including LEN itself. The checksum is the
 * 16-bit two's complement of the sum of all bytes except the two checksum bytes.
 * Every command we send is a literal byte string from the manual, so frames are
 * hardcoded rather than synthesized; gm67_frame_valid() exists only to let the
 * tests assert the tables are transcribed correctly. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The longest reply frame we recognise is a 7-byte CMD_NAK. */
#define GM67_MAX_FRAME_LEN 8

/* A reply frame the module can send us. */
typedef enum {
    GM67_REPLY_ACK,
    GM67_REPLY_NAK_RESEND,
    GM67_REPLY_NAK_BAD_CONTEXT,
    GM67_REPLY_NAK_DENIED,
} gm67_reply_t;

/* A host → module command, with its name for logging and a length so callers
 * never rely on a terminator. */
typedef struct {
    const uint8_t *bytes;
    uint8_t        len;
    const char    *name;
} gm67_cmd_t;

/* Immediate control commands (§3, Opcode Table).  All are fire-and-forget
 * frames queued via the owning task, never awaited for ACK. */
extern const gm67_cmd_t gm67_cmd_scan_enable;   /* 0xE9: resume scan engine   */
extern const gm67_cmd_t gm67_cmd_scan_disable;  /* 0xEA: pause scan engine    */
extern const gm67_cmd_t gm67_cmd_start_decode;  /* 0xE4: one-shot (Host mode) */
extern const gm67_cmd_t gm67_cmd_stop_decode;   /* 0xE5: abort active scan    */
extern const gm67_cmd_t gm67_cmd_beep_cue;      /* 0xE6: immediate single beep */
extern const gm67_cmd_t gm67_cmd_sleep;         /* 0xEB: enter low-power sleep */

/* Runtime scanner settings — sent fire-and-forget when the user changes a
 * setting on the settings screen.  Beep level requires two commands for
 * non-off values: first gm67_cmd_beep_on, then the volume command. */
extern const gm67_cmd_t gm67_cmd_beep_off;
extern const gm67_cmd_t gm67_cmd_beep_on;
extern const gm67_cmd_t gm67_cmd_beep_vol_low;
extern const gm67_cmd_t gm67_cmd_beep_vol_med;
extern const gm67_cmd_t gm67_cmd_beep_vol_high;
extern const gm67_cmd_t gm67_cmd_light_on_scan;
extern const gm67_cmd_t gm67_cmd_light_off;
extern const gm67_cmd_t gm67_cmd_collim_on_scan;
extern const gm67_cmd_t gm67_cmd_collim_off;

/* Validate a complete frame's trailing 16-bit checksum. Used by tests. */
bool gm67_frame_valid(const uint8_t *frame, size_t len);

/* ---- Stream demultiplexer ------------------------------------------------ *
 * Separates the single RX byte stream into:
 *   - reply frames  (exact, checksum-correct ACK/NAK signatures), and
 *   - everything else (raw barcode text), byte for byte.
 *
 * It ALWAYS strips a recognised reply signature, whether or not a transaction
 * is pending, so a fire-and-forget command's ACK never leaks into the text
 * path. A run of bytes that starts to look like a frame prefix but fails to
 * complete (or would mismatch) is rolled back in full to the text callback, so
 * concatenated scan+reply / reply+scan input loses nothing. Barcode payloads in
 * the Phase 1 symbology set are printable ASCII and never collide with a reply
 * signature (which starts with 0x04/0x05). */
typedef void (*gm67_text_fn)(void *ctx, uint8_t byte);
typedef void (*gm67_reply_fn)(void *ctx, gm67_reply_t kind);

typedef struct {
    uint8_t       pend[GM67_MAX_FRAME_LEN]; /* possible in-progress frame prefix */
    uint8_t       pend_len;
    gm67_text_fn  on_text;
    gm67_reply_fn on_reply;
    void         *ctx;
} gm67_demux_t;

void gm67_demux_init(gm67_demux_t *d, gm67_text_fn on_text,
                     gm67_reply_fn on_reply, void *ctx);

/* Feed `n` received bytes. Emits text bytes and/or reply frames via the
 * callbacks, in stream order. */
void gm67_demux_feed(gm67_demux_t *d, const uint8_t *data, size_t n);

/* Flush any buffered partial-frame bytes to the text callback. Call when the
 * line has gone idle so a lone control byte that never became a frame is not
 * held forever. */
void gm67_demux_flush(gm67_demux_t *d);
