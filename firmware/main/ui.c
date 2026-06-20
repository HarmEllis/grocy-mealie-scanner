/* LVGL screens following design/Grocy-Mealie-Scanner_variant-a.html
 * (variant A: grid tiles + colour flash, 240x320 portrait, 1:1 values).
 * Deviations are documented in BOARD_NOTES.md ("Design → LVGL notes"). */
#include "ui.h"

#include "i18n.h"
#include "touch_calibration.h"
#include "ui_fonts.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui";

/* Palette (hex values straight from the design export) */
#define COL_DEVICE   lv_color_hex(0x0b0b0c)
#define COL_CARD     lv_color_hex(0x1e1e22)
#define COL_TEXT     lv_color_hex(0xededf0)
#define COL_DIM      lv_color_hex(0x8b8b93)
#define COL_DIM2     lv_color_hex(0x5e5e66)
#define COL_AMBER    lv_color_hex(0xf5c13d)
#define COL_GREEN    lv_color_hex(0x35c98c)
#define COL_GOLD     lv_color_hex(0xf0a93a)
#define COL_CORAL    lv_color_hex(0xe8674a)
#define COL_BLUE     lv_color_hex(0x5aa0ef)
#define COL_BORDER   lv_color_hex(0x232328)  /* rgba(255,255,255,.08) on device bg */
#define COL_BORDER2  lv_color_hex(0x303036)  /* rgba(255,255,255,.14) */

#define STATUS_BAR_H 26

static ui_event_cb_t s_cb;
static lv_obj_t *s_screen;        /* single root screen, rebuilt per state */
static lv_obj_t *s_status_bar;    /* standard status bar, or NULL on bare screens */
static lv_obj_t *s_status_dot;
static lv_obj_t *s_status_label;
static lv_obj_t *s_status_clock;
static bool s_connected;
static bool s_status_shows_conn; /* true only while the status bar shows Connected/Offline (idle) */
static char s_last_scan_name[API_NAME_LEN];
static char s_last_scan_time[8];
static lv_timer_t *s_clock_timer;

/* Screen sleep state.
 * s_screen_timeout_ms: written by app task (ui_set_screen_timeout), read by
 *   LVGL task (idle_tick) — use _Atomic to satisfy the C11 data-race rule.
 * s_sleep_allowed: written by app task (ui_set_sleep_allowed), read by LVGL
 *   task (idle_tick) — single-word aligned access, safe on ESP32 Xtensa.
 * s_asleep, s_sleep_overlay: LVGL task only. */
static _Atomic uint32_t s_screen_timeout_ms = 0; /* 0 = sleep disabled */
static _Atomic bool s_sleep_allowed = false;
static bool s_asleep = false;
static lv_obj_t *s_sleep_overlay = NULL;

/* Pending-state carried between screens */
static char s_pending_barcode[API_BARCODE_LEN];
static lv_obj_t *s_search_results_box;
static lv_obj_t *s_touch_cal_target;

static bool emit(ui_event_type_t type, api_action_t action, int product_id,
                 const char *text)
{
    ui_event_t evt = {
        .type = type,
        .action = action,
        .product_id = product_id,
    };
    if (text != NULL) {
        strlcpy(evt.text, text, sizeof(evt.text));
    }
    return s_cb(&evt);
}

void ui_set_screen_timeout(uint32_t seconds)
{
    atomic_store(&s_screen_timeout_ms, seconds * 1000u);
}

void ui_set_sleep_allowed(bool allowed)
{
    atomic_store(&s_sleep_allowed, allowed);
}

/* Taps on the sleep overlay wake the display.  The overlay sits on
 * lv_layer_top() and consumes the touch so nothing underneath fires. */
static void sleep_overlay_cb(lv_event_t *e)
{
    (void)e;
    if (!s_asleep) {
        return;
    }
    s_asleep = false;
    lv_display_trigger_activity(NULL);
    if (!emit(UI_EVT_WAKE, 0, 0, NULL)) {
        /* Queue full — restore s_asleep so the overlay remains tappable */
        s_asleep = true;
        return;
    }
    if (s_sleep_overlay != NULL) {
        /* lv_obj_del_async: defer the free to the next lv_timer_handler so we
         * don't free the event target while the indev dispatcher still holds a
         * pointer to it (use-after-free if lv_obj_del were used here). */
        lv_obj_del_async(s_sleep_overlay);
        s_sleep_overlay = NULL;
    }
}

/* Called from the LVGL task every 1 s (alongside clock_tick).  Checks
 * LVGL's built-in touch-inactivity counter and triggers sleep when the
 * configured threshold is exceeded.  Only fires while on the idle screen
 * (s_sleep_allowed=true) — non-idle screens (product, search, proposal…)
 * must not sleep because barcode scans don't reset the touch-inactivity
 * counter, so a freshly-scanned product screen would time out mid-read. */
static void idle_tick(lv_timer_t *t)
{
    (void)t;
    uint32_t timeout_ms = atomic_load(&s_screen_timeout_ms);
    if (s_asleep || timeout_ms == 0 || !atomic_load(&s_sleep_allowed)) {
        return;
    }
    if (lv_display_get_inactive_time(NULL) >= timeout_ms) {
        /* Mark as asleep to prevent re-emit; overlay creation happens in the
         * app task (ui_show_sleep_visual) after state validation so a scan
         * arriving just before this event is processed cannot leave an orphan
         * overlay on a live product screen. */
        s_asleep = true;
        if (!emit(UI_EVT_SLEEP, 0, 0, NULL)) {
            s_asleep = false; /* queue full — allow retry on next tick */
        }
    }
}

/* Create the full-screen sleep overlay and force a synchronous redraw.
 * Must be called from the app task while holding lvgl_port_lock. */
void ui_show_sleep_visual(void)
{
    lv_obj_t *top = lv_layer_top();
    s_sleep_overlay = lv_obj_create(top);
    lv_obj_remove_style_all(s_sleep_overlay);
    lv_obj_set_size(s_sleep_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_sleep_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_sleep_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sleep_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* LV_EVENT_PRESSED fires on the first touch contact — LV_EVENT_CLICKED
     * requires a full press+release without movement, which can miss wakes
     * on a resistive panel or when the user holds the screen on. */
    lv_obj_add_event_cb(s_sleep_overlay, sleep_overlay_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(top);
    lv_refr_now(NULL);
}

/* Reset the s_asleep flag when the app task rejects a UI_EVT_SLEEP event
 * (state changed before the event was processed).  Must be called under
 * lvgl_port_lock so idle_tick cannot race the write. */
void ui_cancel_sleep(void)
{
    s_asleep = false;
}

/* Defined further down with the other not-found/search callbacks. */
static void dismiss_cb(lv_event_t *e);

static void open_settings_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_OPEN_SETTINGS, 0, 0, NULL);
}

/* A tappable text/symbol affordance on the status bar (gear, back chevron). */
static lv_obj_t *add_bar_icon(const char *symbol, lv_align_t align, int dx,
                              lv_event_cb_t cb)
{
    lv_obj_t *icon = lv_label_create(s_status_bar);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &gms_font_12, 0);
    lv_obj_set_style_text_color(icon, COL_DIM, 0);
    lv_obj_align(icon, align, dx, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(icon, 8);
    lv_obj_add_event_cb(icon, cb, LV_EVENT_CLICKED, NULL);
    return icon;
}

/* ------------------------------------------------------------------ */
/* Shared scaffolding                                                  */
/* ------------------------------------------------------------------ */

static void clock_tick(lv_timer_t *t)
{
    (void)t;
    if (s_status_clock == NULL) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 100) { /* only once SNTP has set the clock */
        lv_label_set_text_fmt(s_status_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
}

static void fmt_amount(char *dst, size_t cap, double v)
{
    if (v == (long)v) {
        snprintf(dst, cap, "%ld", (long)v);
    } else {
        snprintf(dst, cap, "%.1f", v);
    }
}

/* Wipes the screen and rebuilds the root + status bar. Returns the content
 * container that fills the rest of the 240x320 canvas. */
static lv_obj_t *screen_reset(const char *status_text, lv_color_t dot_color,
                              bool with_status_bar)
{
    lv_obj_clean(s_screen);
    s_status_bar = NULL;
    s_status_dot = NULL;
    s_status_label = NULL;
    s_status_clock = NULL;
    s_search_results_box = NULL;
    s_touch_cal_target = NULL;
    s_status_shows_conn = false; /* only ui_show_idle re-enables this */

    lv_obj_set_style_bg_color(s_screen, COL_DEVICE, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    if (with_status_bar) {
        lv_obj_t *bar = lv_obj_create(s_screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 240, STATUS_BAR_H);
        lv_obj_set_pos(bar, 0, 0);
        lv_obj_set_style_border_color(bar, COL_BORDER, 0);
        lv_obj_set_style_border_width(bar, 1, 0);
        lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_hor(bar, 12, 0);
        s_status_bar = bar; /* screens may attach a gear/back affordance */

        s_status_dot = lv_obj_create(bar);
        lv_obj_remove_style_all(s_status_dot);
        lv_obj_set_size(s_status_dot, 6, 6);
        lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_status_dot, dot_color, 0);
        lv_obj_align(s_status_dot, LV_ALIGN_LEFT_MID, 0, 0);

        s_status_label = lv_label_create(bar);
        lv_label_set_text(s_status_label, status_text);
        lv_obj_set_style_text_font(s_status_label, &gms_font_10, 0);
        lv_obj_set_style_text_color(s_status_label, COL_DIM, 0);
        lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 12, 0);

        s_status_clock = lv_label_create(bar);
        lv_label_set_text(s_status_clock, "--:--");
        lv_obj_set_style_text_font(s_status_clock, &gms_font_10, 0);
        lv_obj_set_style_text_color(s_status_clock, COL_DIM, 0);
        lv_obj_align(s_status_clock, LV_ALIGN_RIGHT_MID, 0, 0);
        clock_tick(NULL);
    }

    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(content);
    int top = with_status_bar ? STATUS_BAR_H : 0;
    lv_obj_set_size(content, 240, 320 - top);
    lv_obj_set_pos(content, 0, top);
    return content;
}

/* ------------------------------------------------------------------ */
/* Idle                                                                */
/* ------------------------------------------------------------------ */

static void scanline_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

void ui_show_idle(void)
{
    lvgl_port_lock(0);
    /* Reset touch-inactivity so the idle screen always gets a full timeout,
     * not an inherited one from whatever screen the user was on before. */
    lv_display_trigger_activity(NULL);
    lv_obj_t *content = screen_reset(s_connected ? tr("connected") : tr("offline"),
                                     s_connected ? COL_GREEN : COL_CORAL, true);
    s_status_shows_conn = true; /* this status bar tracks live connection state */

    /* Settings gear; nudge the clock left so the two don't collide. */
    lv_obj_align(s_status_clock, LV_ALIGN_RIGHT_MID, -22, 0);
    add_bar_icon(LV_SYMBOL_SETTINGS, LV_ALIGN_RIGHT_MID, 0, open_settings_cb);

    /* Scan frame: 128x96, rounded, amber corner brackets + animated line */
    lv_obj_t *frame = lv_obj_create(content);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, 128, 96);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_radius(frame, 14, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x121214), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);

    /* Corner brackets: four small L-shapes built from 2 bars each */
    static const struct { lv_align_t align; int dx, dy; bool top, left; } corners[] = {
        { LV_ALIGN_TOP_LEFT, 8, 8, true, true },
        { LV_ALIGN_TOP_RIGHT, -8, 8, true, false },
        { LV_ALIGN_BOTTOM_LEFT, 8, -8, false, true },
        { LV_ALIGN_BOTTOM_RIGHT, -8, -8, false, false },
    };
    for (size_t i = 0; i < 4; i++) {
        lv_obj_t *h = lv_obj_create(frame);
        lv_obj_remove_style_all(h);
        lv_obj_set_size(h, 18, 2);
        lv_obj_set_style_bg_color(h, COL_AMBER, 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        lv_obj_align(h, corners[i].align, corners[i].dx, corners[i].dy);
        lv_obj_t *v = lv_obj_create(frame);
        lv_obj_remove_style_all(v);
        lv_obj_set_size(v, 2, 18);
        lv_obj_set_style_bg_color(v, COL_AMBER, 0);
        lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
        lv_obj_align(v, corners[i].align, corners[i].dx, corners[i].dy);
    }

    /* Stylised barcode bars */
    static const uint8_t widths[] = { 3, 2, 4, 2, 3, 5, 2, 3, 2, 4 };
    static const uint8_t shades[] = { 2, 1, 2, 0, 2, 2, 1, 2, 0, 2 };
    int x = (128 - 36 - 9 * 3) / 2;
    for (size_t i = 0; i < sizeof(widths); i++) {
        lv_obj_t *bar = lv_obj_create(frame);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, widths[i], 46);
        lv_obj_set_pos(bar, x, (96 - 46) / 2);
        lv_color_t c = shades[i] == 2 ? COL_TEXT : (shades[i] == 1 ? COL_DIM : COL_DIM2);
        lv_obj_set_style_bg_color(bar, c, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        x += widths[i] + 3;
    }

    /* Animated scan line */
    lv_obj_t *line = lv_obj_create(frame);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 108, 2);
    lv_obj_set_x(line, 10);
    lv_obj_set_style_bg_color(line, COL_AMBER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, line);
    lv_anim_set_exec_cb(&a, scanline_anim_cb);
    lv_anim_set_values(&a, 6, 88);
    lv_anim_set_duration(&a, 1900);
    lv_anim_set_playback_duration(&a, 1900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, tr("ready_to_scan"));
    lv_obj_set_style_text_font(title, &gms_font_20, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 172);

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, tr("point_scanner"));
    lv_obj_set_style_text_font(hint, &gms_font_12, 0);
    lv_obj_set_style_text_color(hint, COL_DIM, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 200);

    /* Footer: last scan */
    if (s_last_scan_name[0] != '\0') {
        lv_obj_t *footer = lv_obj_create(content);
        lv_obj_remove_style_all(footer);
        lv_obj_set_size(footer, 240, 32);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_border_color(footer, COL_BORDER, 0);
        lv_obj_set_style_border_width(footer, 1, 0);
        lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_pad_hor(footer, 12, 0);

        lv_obj_t *tag = lv_label_create(footer);
        lv_label_set_text(tag, tr("last"));
        lv_obj_set_style_text_font(tag, &gms_font_10, 0);
        lv_obj_set_style_text_color(tag, COL_DIM2, 0);
        lv_obj_align(tag, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *name = lv_label_create(footer);
        lv_label_set_text(name, s_last_scan_name);
        lv_obj_set_style_text_font(name, &gms_font_12, 0);
        lv_obj_set_style_text_color(name, COL_DIM, 0);
        lv_obj_set_width(name, 150);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 36, 0);

        lv_obj_t *when = lv_label_create(footer);
        lv_label_set_text(when, s_last_scan_time);
        lv_obj_set_style_text_font(when, &gms_font_10, 0);
        lv_obj_set_style_text_color(when, COL_DIM2, 0);
        lv_obj_align(when, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lvgl_port_unlock();
}

void ui_set_last_scan(const char *name)
{
    strlcpy(s_last_scan_name, name, sizeof(s_last_scan_name));
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 100) {
        snprintf(s_last_scan_time, sizeof(s_last_scan_time), "%02d:%02d",
                 tm.tm_hour, tm.tm_min);
    } else {
        s_last_scan_time[0] = '\0';
    }
}

void ui_set_connected(bool connected)
{
    s_connected = connected;
    lvgl_port_lock(0);
    /* The status dot/label only reflect connectivity on the idle screen.
     * Other screens reuse the same widgets for their own status (Saving,
     * Scanned, ...), so leave those untouched. */
    if (s_status_shows_conn) {
        if (s_status_dot != NULL) {
            lv_obj_set_style_bg_color(s_status_dot, connected ? COL_GREEN : COL_CORAL, 0);
        }
        if (s_status_label != NULL) {
            lv_label_set_text(s_status_label, connected ? tr("connected") : tr("offline"));
        }
    }
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Product found                                                       */
/* ------------------------------------------------------------------ */

static void tile_event_cb(lv_event_t *e)
{
    api_action_t action = (api_action_t)(uintptr_t)lv_event_get_user_data(e);
    emit(UI_EVT_ACTION_TILE, action, 0, NULL);
}

static lv_obj_t *make_stat_card(lv_obj_t *parent, int x, const char *label,
                                double value, lv_color_t value_color)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 70, 46);
    lv_obj_set_pos(card, x, 0);
    lv_obj_set_style_radius(card, 9, 0);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 7, 0);

    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &gms_font_10, 0);
    lv_obj_set_style_text_color(l, COL_DIM2, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);

    char buf[16];
    fmt_amount(buf, sizeof(buf), value);
    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, buf);
    lv_obj_set_style_text_font(v, &gms_font_20, 0);
    lv_obj_set_style_text_color(v, value_color, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    return card;
}

typedef struct {
    const char *label;
    const char *symbol;
    lv_color_t color;
    api_action_t action;
} tile_def_t;

void ui_show_product(const api_product_t *product)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(tr("scanned"), COL_GREEN, true);
    lv_obj_set_style_pad_all(content, 12, 0);

    /* Back chevron → idle; shift the dot + label right to make room. */
    lv_obj_align(s_status_dot, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 30, 0);
    add_bar_icon(LV_SYMBOL_LEFT, LV_ALIGN_LEFT_MID, 0, dismiss_cb);

    lv_obj_t *name = lv_label_create(content);
    lv_label_set_text(name, product->name);
    lv_obj_set_style_text_font(name, &gms_font_18, 0);
    lv_obj_set_style_text_color(name, COL_TEXT, 0);
    lv_obj_set_width(name, 216);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name, 0, 0);

    lv_obj_t *sub = lv_label_create(content);
    if (product->quantity_unit[0] != '\0') {
        lv_label_set_text(sub, product->quantity_unit);
    } else {
        lv_label_set_text(sub, "");
    }
    lv_obj_set_style_text_font(sub, &gms_font_12, 0);
    lv_obj_set_style_text_color(sub, COL_DIM, 0);
    lv_obj_set_pos(sub, 0, 26);

    /* Stat cards: In stock / Min / Opened (70px wide, 7px gaps) */
    lv_obj_t *cards = lv_obj_create(content);
    lv_obj_remove_style_all(cards);
    lv_obj_set_size(cards, 216, 46);
    lv_obj_set_pos(cards, 0, 48);
    lv_color_t stock_color = product->stock_amount <= product->min_stock_amount
                                 ? COL_CORAL
                                 : COL_AMBER;
    make_stat_card(cards, 0, tr("in_stock"), product->stock_amount, stock_color);
    make_stat_card(cards, 73, tr("minimum"), product->min_stock_amount, COL_DIM);
    make_stat_card(cards, 146, tr("opened_stat"), product->opened_amount, COL_GREEN);

    /* 2x2 action grid (tiles 104x82, 8px gap) */
    static const tile_def_t tiles[] = {
        { "action_bought", LV_SYMBOL_PLUS, { 0 }, API_ACTION_PURCHASE },
        { "action_opened", LV_SYMBOL_EJECT, { 0 }, API_ACTION_OPEN },
        { "action_consumed", LV_SYMBOL_OK, { 0 }, API_ACTION_CONSUME },
        { "action_shopping", LV_SYMBOL_LIST, { 0 }, API_ACTION_SHOPPING_LIST },
    };
    const lv_color_t tile_colors[] = { COL_GREEN, COL_GOLD, COL_CORAL, COL_BLUE };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *tile = lv_obj_create(content);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, 104, 82);
        lv_obj_set_pos(tile, (i % 2) * 112, 106 + (i / 2) * 90);
        lv_obj_set_style_radius(tile, 11, 0);
        lv_obj_set_style_bg_color(tile, tile_colors[i], 0);
        lv_obj_set_style_bg_opa(tile, 33, 0); /* ≈ .13 fill from the design */
        lv_obj_set_style_border_color(tile, tile_colors[i], 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_opa(tile, 87, 0); /* ≈ .34 */
        lv_obj_set_style_pad_all(tile, 10, 0);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(tile, 61, LV_STATE_PRESSED); /* ≈ .24 */
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)tiles[i].action);

        lv_obj_t *icon = lv_label_create(tile);
        lv_label_set_text(icon, tiles[i].symbol);
        lv_obj_set_style_text_font(icon, &gms_font_20, 0);
        lv_obj_set_style_text_color(icon, tile_colors[i], 0);
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_text(label, tr(tiles[i].label));
        lv_obj_set_style_text_font(label, &gms_font_14, 0);
        lv_obj_set_style_text_color(label, COL_TEXT, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Saving / flash / error                                              */
/* ------------------------------------------------------------------ */

void ui_show_saving(void)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(tr("saving_status"), COL_AMBER, true);

    lv_obj_t *spinner = lv_spinner_create(content);
    lv_obj_set_size(spinner, 56, 56);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -24);
    lv_obj_set_style_arc_color(spinner, COL_CARD, 0);
    lv_obj_set_style_arc_color(spinner, COL_AMBER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 5, 0);
    lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);

    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, tr("saving"));
    lv_obj_set_style_text_font(label, &gms_font_14, 0);
    lv_obj_set_style_text_color(label, COL_DIM, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 28);
    lvgl_port_unlock();
}

static void flash_dismiss_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_DISMISS, 0, 0, NULL);
}

void ui_show_flash(const api_action_result_t *result)
{
    static const char *labels[] = {
        [API_ACTION_PURCHASE] = "action_bought",
        [API_ACTION_OPEN] = "action_opened",
        [API_ACTION_CONSUME] = "action_consumed",
        [API_ACTION_SHOPPING_LIST] = "added_to_list",
    };
    const lv_color_t colors[] = { COL_GREEN, COL_GOLD, COL_CORAL, COL_BLUE };
    lv_color_t color = colors[result->action];

    char sub[48];
    char b[16], a[16];
    switch (result->action) {
    case API_ACTION_PURCHASE:
    case API_ACTION_CONSUME:
        fmt_amount(b, sizeof(b), result->stock_before);
        fmt_amount(a, sizeof(a), result->stock_after);
        snprintf(sub, sizeof(sub), tr("stock_change_fmt"), b, a);
        break;
    case API_ACTION_OPEN:
        fmt_amount(b, sizeof(b), result->opened_before);
        fmt_amount(a, sizeof(a), result->opened_after);
        snprintf(sub, sizeof(sub), tr("open_change_fmt"), b, a);
        break;
    case API_ACTION_SHOPPING_LIST:
    default:
        strlcpy(sub, tr("on_shopping_list"), sizeof(sub));
        break;
    }

    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(NULL, color, false);
    lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(content, flash_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *badge = lv_obj_create(content);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 104, 104);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, color, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);

    lv_obj_t *check = lv_label_create(badge);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(check, &gms_font_24, 0);
    lv_obj_set_style_text_color(check, COL_DEVICE, 0);
    lv_obj_center(check);

    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, tr(labels[result->action]));
    lv_obj_set_style_text_font(label, &gms_font_24, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 212);

    lv_obj_t *name = lv_label_create(content);
    lv_label_set_text(name, result->product_name);
    lv_obj_set_style_text_font(name, &gms_font_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xd2d2d6), 0);
    lv_obj_set_width(name, 216);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 244);

    lv_obj_t *pill = lv_obj_create(content);
    lv_obj_remove_style_all(pill);
    lv_obj_set_height(pill, 26);
    lv_obj_set_style_radius(pill, 13, 0);
    lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pill, 33, 0);
    lv_obj_set_style_pad_hor(pill, 13, 0);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);

    lv_obj_t *subl = lv_label_create(pill);
    lv_label_set_text(subl, sub);
    lv_obj_set_style_text_font(subl, &gms_font_12, 0);
    lv_obj_set_style_text_color(subl, lv_color_white(), 0);
    lv_obj_center(subl);
    lvgl_port_unlock();
}

static void error_dismiss_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_DISMISS, 0, 0, NULL);
}

void ui_show_error(const char *message)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(tr("error"), COL_CORAL, true);
    lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(content, error_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *circle = lv_obj_create(content);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 54, 54);
    lv_obj_align(circle, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, COL_CORAL, 0);
    lv_obj_set_style_bg_opa(circle, 36, 0);
    lv_obj_set_style_border_color(circle, COL_CORAL, 0);
    lv_obj_set_style_border_width(circle, 1, 0);
    lv_obj_set_style_border_opa(circle, 97, 0);

    lv_obj_t *icon = lv_label_create(circle);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &gms_font_22, 0);
    lv_obj_set_style_text_color(icon, COL_CORAL, 0);
    lv_obj_center(icon);

    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, message);
    lv_obj_set_style_text_font(label, &gms_font_14, 0);
    lv_obj_set_style_text_color(label, COL_TEXT, 0);
    lv_obj_set_width(label, 200);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 122);

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, tr("tap_to_dismiss"));
    lv_obj_set_style_text_font(hint, &gms_font_12, 0);
    lv_obj_set_style_text_color(hint, COL_DIM, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Not found / link / proposal / search                                */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, bool primary,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 212, primary ? 42 : 40);
    lv_obj_set_style_radius(btn, 10, 0);
    if (primary) {
        lv_obj_set_style_bg_color(btn, COL_AMBER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(btn, COL_BORDER2, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2f), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    }
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &gms_font_14, 0);
    lv_obj_set_style_text_color(label, primary ? COL_DEVICE : COL_TEXT, 0);
    lv_obj_set_width(label, 196);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return btn;
}

static int s_suggestion_id;

static void link_suggestion_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_LINK_SUGGESTION, 0, s_suggestion_id, NULL);
}

static void open_proposal_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_OPEN_PROPOSAL, 0, 0, NULL);
}

static void open_search_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_OPEN_SEARCH, 0, 0, NULL);
}

static void dismiss_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_DISMISS, 0, 0, NULL);
}

void ui_show_not_found(const api_scan_result_t *scan)
{
    strlcpy(s_pending_barcode, scan->barcode, sizeof(s_pending_barcode));

    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(tr("unknown"), COL_AMBER, true);
    lv_obj_set_style_pad_hor(content, 14, 0);

    lv_obj_t *circle = lv_obj_create(content);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 44, 44);
    lv_obj_align(circle, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, COL_AMBER, 0);
    lv_obj_set_style_bg_opa(circle, 36, 0);
    lv_obj_set_style_border_color(circle, COL_AMBER, 0);
    lv_obj_set_style_border_width(circle, 1, 0);
    lv_obj_set_style_border_opa(circle, 97, 0);

    lv_obj_t *icon = lv_label_create(circle);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &gms_font_18, 0);
    lv_obj_set_style_text_color(icon, COL_AMBER, 0);
    lv_obj_center(icon);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, tr("product_not_found"));
    lv_obj_set_style_text_font(title, &gms_font_18, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    /* Barcode chip */
    lv_obj_t *chip = lv_obj_create(content);
    lv_obj_remove_style_all(chip);
    lv_obj_set_height(chip, 26);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(chip, 8, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(0x151517), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chip, COL_BORDER, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_pad_hor(chip, 12, 0);
    lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 86);

    lv_obj_t *code = lv_label_create(chip);
    lv_label_set_text(code, scan->barcode);
    lv_obj_set_style_text_font(code, &gms_font_12, 0);
    lv_obj_set_style_text_color(code, COL_TEXT, 0);
    lv_obj_center(code);

    /* Action stack (per the agreed unknown-barcode flow) */
    int y = 124;
    if (scan->suggestion_count > 0) {
        s_suggestion_id = scan->suggestions[0].id;
        char text[96];
        snprintf(text, sizeof(text), tr("link_to_fmt"), scan->suggestions[0].name);
        lv_obj_t *btn = make_button(content, text, true, link_suggestion_cb, NULL);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
        y += 49;
    }
    if (scan->has_external_name) {
        char text[96];
        snprintf(text, sizeof(text), tr("create_quoted_fmt"), scan->external_name);
        lv_obj_t *btn = make_button(content, text, scan->suggestion_count == 0,
                                    open_proposal_cb, NULL);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
        y += scan->suggestion_count == 0 ? 49 : 47;
    }
    lv_obj_t *search_btn = make_button(content, tr("search_products"), false,
                                       open_search_cb, NULL);
    lv_obj_align(search_btn, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *again = lv_label_create(content);
    lv_label_set_text(again, tr("scan_again"));
    lv_obj_set_style_text_font(again, &gms_font_12, 0);
    lv_obj_set_style_text_color(again, COL_DIM, 0);
    lv_obj_align(again, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(again, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(again, 12);
    lv_obj_add_event_cb(again, dismiss_cb, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
}

/* --- proposal (create with editable name + on-screen keyboard) ------- */

static lv_obj_t *s_proposal_ta;

static void proposal_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_proposal_ta != NULL) {
        emit(UI_EVT_PROPOSAL_CONFIRM, 0, 0, lv_textarea_get_text(s_proposal_ta));
    }
}

void ui_show_proposal(const char *initial_name)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(tr("new_product"), COL_AMBER, true);
    lv_obj_set_style_pad_hor(content, 14, 0);

    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, tr("create_in_grocy_as"));
    lv_obj_set_style_text_font(label, &gms_font_12, 0);
    lv_obj_set_style_text_color(label, COL_DIM, 0);
    lv_obj_set_pos(label, 0, 6);

    s_proposal_ta = lv_textarea_create(content);
    lv_textarea_set_one_line(s_proposal_ta, true);
    lv_textarea_set_max_length(s_proposal_ta, API_NAME_LEN - 1);
    lv_textarea_set_text(s_proposal_ta, initial_name);
    lv_obj_set_size(s_proposal_ta, 212, 40);
    lv_obj_set_pos(s_proposal_ta, 0, 26);
    lv_obj_set_style_bg_color(s_proposal_ta, COL_CARD, 0);
    lv_obj_set_style_text_color(s_proposal_ta, COL_TEXT, 0);
    lv_obj_set_style_border_color(s_proposal_ta, COL_BORDER2, 0);
    lv_obj_set_style_text_font(s_proposal_ta, &gms_font_14, 0);

    lv_obj_t *btn = make_button(content, tr("create_product"), true, proposal_confirm_cb, NULL);
    lv_obj_set_pos(btn, 0, 76);

    /* On-screen keyboard opens with the name field focused (user request) */
    lv_obj_t *kb = lv_keyboard_create(content);
    lv_obj_set_size(kb, 212, 140);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_proposal_ta);
    lv_obj_add_state(s_proposal_ta, LV_STATE_FOCUSED);
    lvgl_port_unlock();
}

/* --- search ----------------------------------------------------------- */

static lv_obj_t *s_search_ta;

static void search_submit_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) { /* keyboard checkmark */
        emit(UI_EVT_SEARCH_QUERY, 0, 0, lv_textarea_get_text(s_search_ta));
    }
}

static void search_pick_cb(lv_event_t *e)
{
    int id = (int)(uintptr_t)lv_event_get_user_data(e);
    emit(UI_EVT_SEARCH_PICK, 0, id, NULL);
}

void ui_show_search(void)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(tr("search"), COL_BLUE, true);
    lv_obj_set_style_pad_hor(content, 14, 0);

    s_search_ta = lv_textarea_create(content);
    lv_textarea_set_one_line(s_search_ta, true);
    lv_textarea_set_placeholder_text(s_search_ta, tr("product_name_placeholder"));
    lv_textarea_set_max_length(s_search_ta, API_NAME_LEN - 1);
    lv_obj_set_size(s_search_ta, 212, 40);
    lv_obj_set_pos(s_search_ta, 0, 8);
    lv_obj_set_style_bg_color(s_search_ta, COL_CARD, 0);
    lv_obj_set_style_text_color(s_search_ta, COL_TEXT, 0);
    lv_obj_set_style_border_color(s_search_ta, COL_BORDER2, 0);
    lv_obj_set_style_text_font(s_search_ta, &gms_font_14, 0);

    s_search_results_box = lv_obj_create(content);
    lv_obj_remove_style_all(s_search_results_box);
    lv_obj_set_size(s_search_results_box, 212, 86);
    lv_obj_set_pos(s_search_results_box, 0, 56);
    lv_obj_set_flex_flow(s_search_results_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_search_results_box, 6, 0);
    lv_obj_set_scroll_dir(s_search_results_box, LV_DIR_VER);

    lv_obj_t *kb = lv_keyboard_create(content);
    lv_obj_set_size(kb, 212, 140);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_search_ta);
    lv_obj_add_event_cb(kb, search_submit_cb, LV_EVENT_READY, NULL);
    lvgl_port_unlock();
}

void ui_show_search_results(const api_search_result_t *results)
{
    lvgl_port_lock(0);
    if (s_search_results_box == NULL) {
        lvgl_port_unlock();
        return;
    }
    lv_obj_clean(s_search_results_box);
    if (results->count == 0) {
        lv_obj_t *none = lv_label_create(s_search_results_box);
        lv_label_set_text(none, tr("no_matches"));
        lv_obj_set_style_text_font(none, &gms_font_12, 0);
        lv_obj_set_style_text_color(none, COL_DIM, 0);
    }
    for (int i = 0; i < results->count; i++) {
        const api_product_ref_t *ref = &results->results[i];
        lv_obj_t *row = lv_obj_create(s_search_results_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 212, 34);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, COL_BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a2f), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, search_pick_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)ref->id);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, ref->name);
        lv_obj_set_style_text_font(name, &gms_font_12, 0);
        lv_obj_set_style_text_color(name, COL_TEXT, 0);
        lv_obj_set_width(name, 150);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        char stock[28];
        char amount[12];
        fmt_amount(amount, sizeof(amount), ref->stock_amount);
        snprintf(stock, sizeof(stock), tr("in_stock_fmt"), amount);
        lv_obj_t *st = lv_label_create(row);
        lv_label_set_text(st, stock);
        lv_obj_set_style_text_font(st, &gms_font_10, 0);
        lv_obj_set_style_text_color(st, COL_DIM2, 0);
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

static void cycle_beep_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_CYCLE_BEEP, 0, 0, NULL);
}

static void toggle_light_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_TOGGLE_LIGHT, 0, 0, NULL);
}

static void cycle_scanner_light_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_CYCLE_SCANNER_LIGHT, 0, 0, NULL);
}

static void cycle_collimation_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_CYCLE_COLLIMATION, 0, 0, NULL);
}

static void toggle_language_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_TOGGLE_LANGUAGE, 0, 0, NULL);
}

static void cycle_timeout_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_CYCLE_TIMEOUT, 0, 0, NULL);
}

static void open_touch_cal_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_OPEN_TOUCH_CAL, 0, 0, NULL);
}

/* One toggle card: title + subtitle + a state-only switch (the whole row is the
 * tap target, so the switch itself does not handle clicks). */
static void make_settings_row(lv_obj_t *parent, int y, const char *title,
                              const char *subtitle, bool on, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 212, 56);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_radius(row, 11, 0);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x26262b), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &gms_font_14, 0);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, -9);

    lv_obj_t *s = lv_label_create(row);
    lv_label_set_text(s, subtitle);
    lv_obj_set_style_text_font(s, &gms_font_10, 0);
    lv_obj_set_style_text_color(s, COL_DIM, 0);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 9);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 24);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE); /* the row owns the tap */
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2a2a2f), 0);
    lv_obj_set_style_bg_color(sw, COL_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
}

static const char *beep_level_str(uint8_t level)
{
    switch (level) {
    case 0: return tr("beep_off");
    case 1: return tr("beep_low");
    case 3: return tr("beep_high");
    default: return tr("beep_med");
    }
}

static const char *light_mode_str(uint8_t mode)
{
    return mode == 0 ? tr("on_scan") : tr("always_off");
}

static void make_cycle_row(lv_obj_t *parent, int y, const char *title,
                           const char *value, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 212, 48);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_radius(row, 11, 0);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x26262b), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &gms_font_14, 0);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, &gms_font_12, 0);
    lv_obj_set_style_text_color(v, COL_BLUE, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void make_language_row(lv_obj_t *parent, int y, const char *language)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 212, 48);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_radius(row, 11, 0);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x26262b), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, toggle_language_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, tr("language"));
    lv_obj_set_style_text_font(title, &gms_font_14, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, tr(strcmp(language, "nl") == 0 ? "dutch" : "english"));
    lv_obj_set_style_text_font(value, &gms_font_12, 0);
    lv_obj_set_style_text_color(value, COL_BLUE, 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void make_timeout_row(lv_obj_t *parent, int y, uint32_t timeout_seconds)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 212, 48);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_radius(row, 11, 0);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x26262b), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, cycle_timeout_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, tr("screen_timeout"));
    lv_obj_set_style_text_font(title, &gms_font_14, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *value = lv_label_create(row);
    if (timeout_seconds == 0) {
        lv_label_set_text(value, tr("screen_timeout_never"));
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), tr("seconds_fmt"), (int)timeout_seconds);
        lv_label_set_text(value, buf);
    }
    lv_obj_set_style_text_font(value, &gms_font_12, 0);
    lv_obj_set_style_text_color(value, COL_BLUE, 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void make_navigation_row(lv_obj_t *parent, int y, const char *title,
                                lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 212, 48);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_radius(row, 11, 0);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x26262b), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &gms_font_14, 0);
    lv_obj_set_style_text_color(label, COL_TEXT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, &gms_font_12, 0);
    lv_obj_set_style_text_color(chevron, COL_BLUE, 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
}

void ui_show_settings(uint8_t beep_level, bool light, const char *language,
                      uint32_t timeout_seconds, uint8_t scanner_light,
                      uint8_t collimation)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(NULL, COL_TEXT, false);

    lv_obj_t *bar = lv_obj_create(content);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, STATUS_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);

    lv_obj_t *back = lv_label_create(bar);
    char back_text[32];
    snprintf(back_text, sizeof(back_text), LV_SYMBOL_LEFT " %s", tr("back"));
    lv_label_set_text(back, back_text);
    lv_obj_set_style_text_font(back, &gms_font_10, 0);
    lv_obj_set_style_text_color(back, COL_DIM, 0);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 10);
    lv_obj_add_event_cb(back, dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, tr("settings"));
    lv_obj_set_style_text_font(title, &gms_font_12, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_center(title);

    lv_obj_t *body = lv_obj_create(content);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 240, 320 - STATUS_BAR_H);
    lv_obj_set_pos(body, 0, STATUS_BAR_H);
    lv_obj_set_style_pad_hor(body, 14, 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    /* Section 1: Scan feedback */
    lv_obj_t *section1 = lv_label_create(body);
    lv_label_set_text(section1, tr("scan_feedback"));
    lv_obj_set_style_text_font(section1, &gms_font_10, 0);
    lv_obj_set_style_text_color(section1, COL_DIM2, 0);
    lv_obj_set_pos(section1, 2, 14);

    make_cycle_row(body, 34, tr("scanner_beep"), beep_level_str(beep_level), cycle_beep_cb);
    make_settings_row(body, 90, tr("status_light"), tr("result_flash"),
                      light, toggle_light_cb);

    /* Section 2: Scanner hardware */
    lv_obj_t *section2 = lv_label_create(body);
    lv_label_set_text(section2, tr("scanner"));
    lv_obj_set_style_text_font(section2, &gms_font_10, 0);
    lv_obj_set_style_text_color(section2, COL_DIM2, 0);
    lv_obj_set_pos(section2, 2, 160);

    make_cycle_row(body, 178, tr("scanner_light"),
                   light_mode_str(scanner_light), cycle_scanner_light_cb);
    make_cycle_row(body, 234, tr("collimation"),
                   light_mode_str(collimation), cycle_collimation_cb);

    make_language_row(body, 298, language);
    make_timeout_row(body, 354, timeout_seconds);
    make_navigation_row(body, 410, tr("touch_calibrate"), open_touch_cal_cb);

    lv_obj_t *note = lv_label_create(body);
    lv_label_set_text(note, tr("changes_next_scan"));
    lv_obj_set_style_text_font(note, &gms_font_10, 0);
    lv_obj_set_style_text_color(note, COL_DIM2, 0);
    lv_obj_set_width(note, 212);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(note, 0, 466);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Touch calibration                                                   */
/* ------------------------------------------------------------------ */

static void touch_cal_tap_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_CAL_TAP, 0, 0, NULL);
}

static void touch_cal_release_cb(lv_event_t *e)
{
    (void)e;
    emit(UI_EVT_CAL_RELEASE, 0, 0, NULL);
}

static void position_touch_cal_target(uint8_t target_index)
{
    static const lv_point_t targets[TOUCH_CAL_TARGET_COUNT] = {
        { TOUCH_CAL_TARGET_LEFT, TOUCH_CAL_TARGET_TOP },
        { TOUCH_CAL_TARGET_RIGHT, TOUCH_CAL_TARGET_TOP },
        { TOUCH_CAL_TARGET_LEFT, TOUCH_CAL_TARGET_BOTTOM },
        { TOUCH_CAL_TARGET_RIGHT, TOUCH_CAL_TARGET_BOTTOM },
    };
    if (s_touch_cal_target == NULL || target_index >= TOUCH_CAL_TARGET_COUNT) {
        return;
    }
    lv_obj_set_pos(s_touch_cal_target, targets[target_index].x - 16,
                   targets[target_index].y - 16);
}

void ui_show_touch_calibration(void)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(NULL, COL_BLUE, false);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, tr("touch_calibrate"));
    lv_obj_set_style_text_font(title, &gms_font_18, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *instruction = lv_label_create(content);
    lv_label_set_text(instruction, tr("touch_cal_instr"));
    lv_obj_set_style_text_font(instruction, &gms_font_10, 0);
    lv_obj_set_style_text_color(instruction, COL_DIM, 0);
    lv_obj_align(instruction, LV_ALIGN_CENTER, 0, 12);

    s_touch_cal_target = lv_obj_create(content);
    lv_obj_remove_style_all(s_touch_cal_target);
    lv_obj_set_size(s_touch_cal_target, 32, 32);
    lv_obj_set_style_radius(s_touch_cal_target, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_touch_cal_target, COL_BLUE, 0);
    lv_obj_set_style_border_width(s_touch_cal_target, 2, 0);
    lv_obj_set_style_bg_color(s_touch_cal_target, COL_BLUE, 0);
    lv_obj_set_style_bg_opa(s_touch_cal_target, 24, 0);

    lv_obj_t *horizontal = lv_obj_create(s_touch_cal_target);
    lv_obj_remove_style_all(horizontal);
    lv_obj_set_size(horizontal, 20, 2);
    lv_obj_set_style_bg_color(horizontal, COL_BLUE, 0);
    lv_obj_set_style_bg_opa(horizontal, LV_OPA_COVER, 0);
    lv_obj_center(horizontal);

    lv_obj_t *vertical = lv_obj_create(s_touch_cal_target);
    lv_obj_remove_style_all(vertical);
    lv_obj_set_size(vertical, 2, 20);
    lv_obj_set_style_bg_color(vertical, COL_BLUE, 0);
    lv_obj_set_style_bg_opa(vertical, LV_OPA_COVER, 0);
    lv_obj_center(vertical);
    position_touch_cal_target(0);

    lv_obj_t *tap_layer = lv_obj_create(content);
    lv_obj_remove_style_all(tap_layer);
    lv_obj_set_size(tap_layer, 240, 320);
    lv_obj_set_pos(tap_layer, 0, 0);
    lv_obj_add_flag(tap_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tap_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tap_layer, touch_cal_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(tap_layer, touch_cal_release_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *back = lv_obj_create(content);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 112, 30);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x26262b), LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back);
    char back_text[32];
    snprintf(back_text, sizeof(back_text), LV_SYMBOL_LEFT " %s", tr("back"));
    lv_label_set_text(back_label, back_text);
    lv_obj_set_style_text_font(back_label, &gms_font_10, 0);
    lv_obj_set_style_text_color(back_label, COL_DIM, 0);
    lv_obj_center(back_label);
    lvgl_port_unlock();
}

void ui_touch_calibration_set_target(uint8_t target_index)
{
    lvgl_port_lock(0);
    position_touch_cal_target(target_index);
    lvgl_port_unlock();
}

void ui_show_touch_calibration_result(bool success, bool save_failed)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(NULL, success ? COL_GREEN : COL_CORAL, false);

    lv_obj_t *circle = lv_obj_create(content);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 64, 64);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, -28);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, success ? COL_GREEN : COL_CORAL, 0);
    lv_obj_set_style_bg_opa(circle, 36, 0);
    lv_obj_set_style_border_color(circle, success ? COL_GREEN : COL_CORAL, 0);
    lv_obj_set_style_border_width(circle, 2, 0);

    lv_obj_t *icon = lv_label_create(circle);
    lv_label_set_text(icon, success ? LV_SYMBOL_OK : LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &gms_font_22, 0);
    lv_obj_set_style_text_color(icon, success ? COL_GREEN : COL_CORAL, 0);
    lv_obj_center(icon);

    lv_obj_t *message = lv_label_create(content);
    lv_label_set_text(message, tr(success ? "touch_cal_done" :
                                  (save_failed ? "touch_cal_save_failed" :
                                                 "touch_cal_invalid")));
    lv_obj_set_style_text_font(message, &gms_font_14, 0);
    lv_obj_set_style_text_color(message, COL_TEXT, 0);
    lv_obj_set_width(message, 208);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, 42);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Provisioning / connecting                                           */
/* ------------------------------------------------------------------ */

void ui_show_provisioning(const char *ap_ssid, const char *ap_pass)
{
    char wifi_qr[96];
    snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_pass);

    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(NULL, COL_AMBER, false);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, tr("setup"));
    lv_obj_set_style_text_font(title, &gms_font_20, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *qr = lv_qrcode_create(content);
    lv_qrcode_set_size(qr, 140);
    lv_qrcode_set_dark_color(qr, COL_DEVICE);
    lv_qrcode_set_light_color(qr, COL_TEXT);
    lv_qrcode_update(qr, wifi_qr, strlen(wifi_qr));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_border_color(qr, COL_TEXT, 0);
    lv_obj_set_style_border_width(qr, 8, 0);

    lv_obj_t *info = lv_label_create(content);
    lv_label_set_text_fmt(info, tr("setup_info_fmt"), ap_ssid, ap_pass);
    lv_obj_set_style_text_font(info, &gms_font_12, 0);
    lv_obj_set_style_text_color(info, COL_DIM, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 216);
    lvgl_port_unlock();
}

void ui_show_connecting(const char *message)
{
    lvgl_port_lock(0);
    lv_obj_t *content = screen_reset(NULL, COL_AMBER, false);

    lv_obj_t *spinner = lv_spinner_create(content);
    lv_obj_set_size(spinner, 56, 56);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -24);
    lv_obj_set_style_arc_color(spinner, COL_CARD, 0);
    lv_obj_set_style_arc_color(spinner, COL_AMBER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 5, 0);
    lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);

    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, message);
    lv_obj_set_style_text_font(label, &gms_font_14, 0);
    lv_obj_set_style_text_color(label, COL_DIM, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, 200);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 32);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */

esp_err_t ui_init(ui_event_cb_t cb)
{
    s_cb = cb;
    lvgl_port_lock(0);
    s_screen = lv_screen_active();
    lv_obj_set_style_bg_color(s_screen, COL_DEVICE, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    s_clock_timer = lv_timer_create(clock_tick, 1000, NULL);
    lv_timer_create(idle_tick, 1000, NULL); /* screen-sleep inactivity check */
    lvgl_port_unlock();
    ESP_LOGI(TAG, "ui ready");
    return ESP_OK;
}
