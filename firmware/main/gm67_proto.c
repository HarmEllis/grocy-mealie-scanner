#include "gm67_proto.h"

#include <string.h>

/* ---- Command byte arrays ------------------------------------------------- */

/* Good-read prompt tone enable/disable */
static const uint8_t CMD_BEEP_OFF[]       = {0x07,0xC6,0x04,0x08,0x00,0x38,0x00,0xFE,0xEF};
static const uint8_t CMD_BEEP_ON[]        = {0x07,0xC6,0x04,0x08,0x00,0x38,0x01,0xFE,0xEE};
/* Good-read prompt volume (only meaningful when beep is on) */
static const uint8_t CMD_BEEP_VOL_LOW[]   = {0x07,0xC6,0x04,0x08,0x00,0x8C,0x02,0xFE,0x99};
static const uint8_t CMD_BEEP_VOL_MED[]   = {0x07,0xC6,0x04,0x08,0x00,0x8C,0x01,0xFE,0x9A};
static const uint8_t CMD_BEEP_VOL_HIGH[]  = {0x07,0xC6,0x04,0x08,0x00,0x8C,0x00,0xFE,0x9B};
/* Scanning illumination light */
static const uint8_t CMD_LIGHT_ON_SCAN[]  = {0x08,0xC6,0x04,0x08,0x00,0xF2,0x02,0x00,0xFE,0x32};
static const uint8_t CMD_LIGHT_OFF[]      = {0x08,0xC6,0x04,0x08,0x00,0xF2,0x02,0x02,0xFE,0x30};
/* Collimation/aiming light */
static const uint8_t CMD_COLLIM_ON_SCAN[] = {0x08,0xC6,0x04,0x08,0x00,0xF2,0x03,0x00,0xFE,0x31};
static const uint8_t CMD_COLLIM_OFF[]     = {0x08,0xC6,0x04,0x08,0x00,0xF2,0x03,0x02,0xFE,0x2F};

/* ---- Control commands (§3, Opcode Table) --------------------------------- *
 * These trigger immediate module actions rather than writing parameter memory.
 * Checksums verified: chk = 0x10000 - (sum of pre-checksum bytes & 0xFFFF).   */
static const uint8_t CMD_SCAN_ENABLE[]  = {0x04,0xE9,0x04,0x00,0xFF,0x0F};
static const uint8_t CMD_SCAN_DISABLE[] = {0x04,0xEA,0x04,0x00,0xFF,0x0E};
static const uint8_t CMD_START_DECODE[] = {0x04,0xE4,0x04,0x00,0xFF,0x14};
static const uint8_t CMD_STOP_DECODE[]  = {0x04,0xE5,0x04,0x00,0xFF,0x13};
static const uint8_t CMD_BEEP_CUE[]    = {0x05,0xE6,0x04,0x00,0x01,0xFF,0x10};
static const uint8_t CMD_SLEEP[]        = {0x04,0xEB,0x04,0x00,0xFF,0x0D};

#define MKMD(arr, name) { (arr), (uint8_t)sizeof(arr), (name) }

/* Runtime scanner settings */
const gm67_cmd_t gm67_cmd_beep_off      = MKMD(CMD_BEEP_OFF,      "beep=off");
const gm67_cmd_t gm67_cmd_beep_on       = MKMD(CMD_BEEP_ON,       "beep=on");
const gm67_cmd_t gm67_cmd_beep_vol_low  = MKMD(CMD_BEEP_VOL_LOW,  "beep-vol=low");
const gm67_cmd_t gm67_cmd_beep_vol_med  = MKMD(CMD_BEEP_VOL_MED,  "beep-vol=med");
const gm67_cmd_t gm67_cmd_beep_vol_high = MKMD(CMD_BEEP_VOL_HIGH, "beep-vol=high");
const gm67_cmd_t gm67_cmd_light_on_scan = MKMD(CMD_LIGHT_ON_SCAN, "light=on-scan");
const gm67_cmd_t gm67_cmd_light_off     = MKMD(CMD_LIGHT_OFF,     "light=off");
const gm67_cmd_t gm67_cmd_collim_on_scan= MKMD(CMD_COLLIM_ON_SCAN,"collim=on-scan");
const gm67_cmd_t gm67_cmd_collim_off    = MKMD(CMD_COLLIM_OFF,    "collim=off");

/* Immediate control commands */
const gm67_cmd_t gm67_cmd_scan_enable  = MKMD(CMD_SCAN_ENABLE,  "scan-enable");
const gm67_cmd_t gm67_cmd_scan_disable = MKMD(CMD_SCAN_DISABLE, "scan-disable");
const gm67_cmd_t gm67_cmd_start_decode = MKMD(CMD_START_DECODE, "start-decode");
const gm67_cmd_t gm67_cmd_stop_decode  = MKMD(CMD_STOP_DECODE,  "stop-decode");
const gm67_cmd_t gm67_cmd_beep_cue     = MKMD(CMD_BEEP_CUE,     "beep-cue");
const gm67_cmd_t gm67_cmd_sleep        = MKMD(CMD_SLEEP,        "sleep");

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
