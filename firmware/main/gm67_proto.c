#include "gm67_proto.h"

#include <string.h>

/* ---- Command tables (literal byte strings from the manual, Appendix 6) ---- */

/* Comm interface → Serial */
static const uint8_t CMD_IFACE_SERIAL[]   = {0x08,0xC6,0x04,0x08,0x00,0xF2,0x01,0x00,0xFE,0x33};
/* Trigger mode → Continuous */
static const uint8_t CMD_TRIGGER_CONT[]   = {0x07,0xC6,0x04,0x08,0x00,0x8A,0x04,0xFE,0x99};
/* Scan data send format → Code only */
static const uint8_t CMD_SEND_CODE_ONLY[] = {0x07,0xC6,0x04,0x08,0x00,0xEB,0x00,0xFE,0x3C};
/* Terminator → CR LF */
static const uint8_t CMD_TERM_CRLF[]      = {0x08,0xC6,0x04,0x08,0x00,0xF2,0x05,0x01,0xFE,0x2E};
/* STX/ETX → Forbid */
static const uint8_t CMD_STXETX_OFF[]     = {0x08,0xC6,0x04,0x08,0x00,0xF2,0xB7,0x00,0xFD,0x7D};
/* Decoded packet format → raw (not packet) */
static const uint8_t CMD_PACKET_RAW[]     = {0x07,0xC6,0x04,0x08,0x00,0xEE,0x00,0xFE,0x39};
/* Good-read prompt tone → On / Off. The same PARAM_SEND frames serve the boot
 * sequence (On) and the runtime toggle (gm67_set_beep, exported below). */
static const uint8_t CMD_BEEP_ON[]        = {0x07,0xC6,0x04,0x08,0x00,0x38,0x01,0xFE,0xEE};
static const uint8_t CMD_BEEP_OFF[]       = {0x07,0xC6,0x04,0x08,0x00,0x38,0x00,0xFE,0xEF};
/* Single scan time → 3s */
static const uint8_t CMD_SCANTIME_3S[]    = {0x08,0xC6,0x04,0x08,0x00,0xF2,0xFA,0x03,0xFD,0x37};

#define CMD(arr, name) { (arr), (uint8_t)sizeof(arr), (name) }

const gm67_cmd_t gm67_config_seq[] = {
    CMD(CMD_IFACE_SERIAL,   "iface=serial"),
    CMD(CMD_TRIGGER_CONT,   "trigger=continuous"),
    CMD(CMD_SEND_CODE_ONLY, "send=code-only"),
    CMD(CMD_TERM_CRLF,      "term=crlf"),
    CMD(CMD_STXETX_OFF,     "stx/etx=off"),
    CMD(CMD_PACKET_RAW,     "packet=raw"),
    CMD(CMD_BEEP_ON,        "beep=on"),
    CMD(CMD_SCANTIME_3S,    "scantime=3s"),
};
const size_t gm67_config_seq_len = sizeof(gm67_config_seq) / sizeof(gm67_config_seq[0]);

/* Runtime good-read beep toggle (Phase 2). Reuses the byte arrays above, so the
 * checksums are asserted once by the host tests that cover the config seq. */
const gm67_cmd_t gm67_cmd_beep_on  = CMD(CMD_BEEP_ON,  "beep=on");
const gm67_cmd_t gm67_cmd_beep_off = CMD(CMD_BEEP_OFF, "beep=off");

/* ---- Reply frames -------------------------------------------------------- */

static const uint8_t FRAME_ACK[]        = {0x04,0xD0,0x04,0x00,0xFF,0x28};
static const uint8_t FRAME_NAK_RESEND[] = {0x05,0xD1,0x04,0x00,0x01,0xFF,0x25};
static const uint8_t FRAME_NAK_BADCTX[] = {0x05,0xD1,0x04,0x00,0x02,0xFF,0x24};
static const uint8_t FRAME_NAK_DENIED[] = {0x05,0xD1,0x04,0x00,0x06,0xFF,0x20};

typedef struct {
    const uint8_t *bytes;
    uint8_t        len;
    gm67_reply_t   kind;
} known_frame_t;

static const known_frame_t KNOWN[] = {
    { FRAME_ACK,        (uint8_t)sizeof(FRAME_ACK),        GM67_REPLY_ACK },
    { FRAME_NAK_RESEND, (uint8_t)sizeof(FRAME_NAK_RESEND), GM67_REPLY_NAK_RESEND },
    { FRAME_NAK_BADCTX, (uint8_t)sizeof(FRAME_NAK_BADCTX), GM67_REPLY_NAK_BAD_CONTEXT },
    { FRAME_NAK_DENIED, (uint8_t)sizeof(FRAME_NAK_DENIED), GM67_REPLY_NAK_DENIED },
};
#define KNOWN_COUNT (sizeof(KNOWN) / sizeof(KNOWN[0]))

bool gm67_frame_valid(const uint8_t *frame, size_t len)
{
    if (len < 3) {
        return false;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < len - 2; i++) {
        sum += frame[i];
    }
    uint16_t chk = (uint16_t)((0x10000u - (sum & 0xFFFFu)) & 0xFFFFu);
    uint16_t want = (uint16_t)((frame[len - 2] << 8) | frame[len - 1]);
    return chk == want;
}

/* ---- Demultiplexer ------------------------------------------------------- */

/* Does pend[0..pend_len) match the start of a known frame?
 *   returns true if it is a prefix of at least one known frame;
 *   sets *exact + *kind if it equals one exactly. The known set has no frame
 *   that is a prefix of another, so an exact match is unambiguous. */
static bool pend_is_frame_prefix(const uint8_t *pend, uint8_t pend_len,
                                 bool *exact, gm67_reply_t *kind)
{
    *exact = false;
    bool any = false;
    for (size_t i = 0; i < KNOWN_COUNT; i++) {
        if (pend_len <= KNOWN[i].len &&
            memcmp(pend, KNOWN[i].bytes, pend_len) == 0) {
            any = true;
            if (pend_len == KNOWN[i].len) {
                *exact = true;
                *kind = KNOWN[i].kind;
            }
        }
    }
    return any;
}

void gm67_demux_init(gm67_demux_t *d, gm67_text_fn on_text,
                     gm67_reply_fn on_reply, void *ctx)
{
    memset(d, 0, sizeof(*d));
    d->on_text = on_text;
    d->on_reply = on_reply;
    d->ctx = ctx;
}

/* Reduce the pending buffer: emit a completed frame, keep waiting on a live
 * prefix, or roll the leading byte out as text and retry. */
static void demux_reduce(gm67_demux_t *d)
{
    for (;;) {
        if (d->pend_len == 0) {
            return;
        }
        bool exact;
        gm67_reply_t kind;
        if (pend_is_frame_prefix(d->pend, d->pend_len, &exact, &kind)) {
            if (exact) {
                if (d->on_reply) {
                    d->on_reply(d->ctx, kind);
                }
                d->pend_len = 0;
                return;
            }
            return; /* live prefix — wait for more bytes */
        }
        /* pend[0] cannot start a known frame: it is text. */
        if (d->on_text) {
            d->on_text(d->ctx, d->pend[0]);
        }
        memmove(d->pend, d->pend + 1, --d->pend_len);
    }
}

void gm67_demux_feed(gm67_demux_t *d, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        /* pend never holds a full frame (reduce fires at exact length) and the
         * longest known frame is < GM67_MAX_FRAME_LEN, so this cannot overflow. */
        d->pend[d->pend_len++] = data[i];
        demux_reduce(d);
    }
}

void gm67_demux_flush(gm67_demux_t *d)
{
    for (uint8_t i = 0; i < d->pend_len; i++) {
        if (d->on_text) {
            d->on_text(d->ctx, d->pend[i]);
        }
    }
    d->pend_len = 0;
}
