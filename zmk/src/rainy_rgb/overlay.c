#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "overlay.h"
#include "color.h"
#include "led_map.h"     /* rrgb_led_for_position */
#include "ble_status.h"

#define CAPS_POS         44   /* keymap position of CapsLock (&kp CLCK) */
#define BAT_LOW_POS       0   /* keymap position of Esc: the low-battery pulse */
#define BAT_SHOW_FRAMES  90   /* ~3s at 30fps */
#define BAT_SEG_FIRST    16   /* number row keys 1..0 = positions 16..25 */
#define BAT_SEG_COUNT    10

/* BLE status keys (ble_status.h), keymap positions. KEYMAP-COUPLED:
 * F1..F3 = &bt_sel_ble 0..2, F4 = &out OUT_TOG on the Fn layer; the number
 * row 1..0 takes the passkey digits; Enter submits it. */
#define BLE_POS_SLOT0    1    /* F1..F3 = positions 1..3 */
#define BLE_POS_OUTPUT   4    /* F4 */
#define BLE_POS_NUMROW  16    /* 1..0 = positions 16..25 */
#ifdef CONFIG_RAINY_RGB_ANSI_LEDMAP
#define BLE_POS_ENTER   56    /* ANSI wide Enter (the ISO #~ slot) */
#else
#define BLE_POS_ENTER   43    /* ISO Enter */
#endif

static volatile bool     s_caps;
static volatile bool     s_fn;
static volatile uint8_t  s_battery;
static volatile uint32_t s_bat_until;
static bool              s_ble;   /* ble_status owns its keys (BLE build) */

/* Indicator settings (rrgb_overlay_set_*): written by the settings path
 * (mcumgr work queue, ZMK main thread at boot), read by the render thread;
 * single bytes and words, a pair set together can be torn for one frame.
 * The initial values are the defaults. */
static volatile uint8_t  s_caps_style = RRGB_CAPS_KEY;
static volatile uint32_t s_caps_rgb = 0xFFFFFF;
static volatile bool     s_fn_highlight = true;
static volatile uint8_t  s_bat_low;   /* percent, 0 = off */

/* Fn-highlight keys, bit p = keymap position p. Written by the layer
 * listener (system workqueue) when layer 1 becomes active, read by the
 * render thread: a torn read mixes the old and the new set for one frame. */
static uint32_t s_fn_mask[RRGB_FN_MASK_WORDS];

static uint8_t ble_led(uint8_t pos) {
    int led = rrgb_led_for_position(pos);
    return (led < 0) ? RRGB_BLE_NONE : (uint8_t)led;
}

void rrgb_overlay_init(bool ble) {
    struct rrgb_ble_keys k;
    for (uint8_t s = 0; s < RRGB_BLE_SLOTS; s++) {
        k.slot[s] = ble ? ble_led((uint8_t)(BLE_POS_SLOT0 + s)) : RRGB_BLE_NONE;
    }
    k.output = ble ? ble_led(BLE_POS_OUTPUT) : RRGB_BLE_NONE;
    for (uint8_t d = 0; d < 10; d++) {
        k.numrow[d] = ble ? ble_led((uint8_t)(BLE_POS_NUMROW + d)) : RRGB_BLE_NONE;
    }
    k.enter = ble ? ble_led(BLE_POS_ENTER) : RRGB_BLE_NONE;
    rrgb_ble_init(&k);
    rrgb_ble_set_fn(s_fn);
    s_ble = ble;
}

void rrgb_overlay_set_caps(bool on)        { s_caps = on; }
void rrgb_overlay_set_fn(bool active)      { s_fn = active; rrgb_ble_set_fn(active); }
void rrgb_overlay_set_battery(uint8_t pct) { s_battery = pct; }
void rrgb_overlay_battery_show(uint32_t tick) { s_bat_until = tick + BAT_SHOW_FRAMES; }

void rrgb_overlay_set_caps_style(uint8_t style, uint32_t rgb) {
    s_caps_rgb = rgb & 0xFFFFFFu;
    s_caps_style = style;
}
void rrgb_overlay_set_fn_highlight(bool on) { s_fn_highlight = on; }
void rrgb_overlay_set_bat_low(uint8_t pct)  { s_bat_low = pct; }

void rrgb_overlay_set_fn_keys(const uint32_t mask[RRGB_FN_MASK_WORDS]) {
    for (uint8_t w = 0; w < RRGB_FN_MASK_WORDS; w++) { s_fn_mask[w] = mask[w]; }
}

void rrgb_fn_mask_build(rrgb_dev_at_fn dev_at, void *ctx, uint16_t n, const char *trans_name,
                        uint32_t mask[RRGB_FN_MASK_WORDS]) {
    memset(mask, 0, RRGB_FN_MASK_WORDS * sizeof(mask[0]));
    for (uint16_t p = 0; p < n && p < RRGB_FN_MASK_WORDS * 32; p++) {
        const char *dev = dev_at(p, ctx);
        if (dev != NULL && (trans_name == NULL || strcmp(dev, trans_name) != 0)) {
            mask[p / 32] |= 1u << (p % 32);
        }
    }
}

static bool fn_key(uint16_t pos) { return ((s_fn_mask[pos / 32] >> (pos % 32)) & 1u) != 0; }

/* CapsLock needs a frame of its own: on with the key style. The tint shows
 * only on top of a drawn effect (tint_v), so it never keeps the frame loop
 * or the LED rail on by itself. */
static bool caps_key_shown(void) {
    return s_caps && s_caps_style == RRGB_CAPS_KEY;
}

/* The Fn overview (ind.fn_highlight) replaces the lighting this frame. */
static bool fn_overview(void) {
    return s_fn && s_fn_highlight;
}

static struct rrgb rgb_of(uint32_t c) {
    return (struct rrgb){(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
}

/* 50/50 mix of a pixel with c (the CapsLock tint). */
static struct rrgb mix50(struct rrgb a, struct rrgb c) {
    return (struct rrgb){(uint8_t)((a.r + c.r) / 2), (uint8_t)((a.g + c.g) / 2),
                         (uint8_t)((a.b + c.b) / 2)};
}

/* a toward b by t/255. */
static uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
    return (uint8_t)(a + ((int)b - (int)a) * t / 255);
}

uint8_t rrgb_bat_low_alpha(uint8_t level, uint8_t threshold, bool usb_host, uint32_t tick) {
    uint32_t h = RRGB_BAT_LOW_PERIOD / 2, p;

    if (threshold == 0 || usb_host || level == 0 || level >= threshold) {
        return 0;
    }
    p = tick % RRGB_BAT_LOW_PERIOD;
    return (uint8_t)(255u * (p < h ? h - p : p - h) / h);
}

void rrgb_overlay_bat_low_render(struct rrgb *px, uint16_t n, uint32_t tick, bool usb_host) {
    uint8_t a = rrgb_bat_low_alpha(s_battery, s_bat_low, usb_host, tick);
    int led = rrgb_led_for_position(BAT_LOW_POS);

    if (a == 0 || led < 0 || led >= (int)n) { return; }
    px[led] = (struct rrgb){lerp8(px[led].r, 255, a), lerp8(px[led].g, 0, a),
                            lerp8(px[led].b, 0, a)};
}

bool rrgb_overlay_suppress_effect(uint32_t tick) {
    return s_ble && rrgb_ble_suppress_effect(tick);
}

#define GAIN_OUT_STEP ((255 + RRGB_EFFECT_FADE_OUT_FRAMES - 1) / RRGB_EFFECT_FADE_OUT_FRAMES)
#define GAIN_IN_STEP  ((255 + RRGB_EFFECT_FADE_IN_FRAMES - 1) / RRGB_EFFECT_FADE_IN_FRAMES)

uint8_t rrgb_effect_gain_next(uint8_t gain, bool suppress) {
    if (suppress) {
        return gain > GAIN_OUT_STEP ? (uint8_t)(gain - GAIN_OUT_STEP) : 0;
    }
    return gain < 255 - GAIN_IN_STEP ? (uint8_t)(gain + GAIN_IN_STEP) : 255;
}

uint8_t rrgb_effect_gain_frame(uint8_t gain, bool suppress, bool drawn) {
    if (!drawn) {
        return suppress ? 0 : 255;
    }
    return rrgb_effect_gain_next(gain, suppress);
}

/* Called from the key event thread with the render thread's tick: a
 * deliberately unlocked cross-thread read (a 32-bit load, at worst one frame
 * stale, which moves the suppression edge by one frame). */
bool rrgb_overlay_key_reactive(uint32_t position, uint32_t tick) {
    (void)position;   /* every Fn-layer press is a command, not only F1..F4 */
    return !s_fn && !rrgb_overlay_suppress_effect(tick);
}

bool rrgb_overlay_active(uint32_t tick) {
    return caps_key_shown() || fn_overview() || (tick < s_bat_until) ||
           (s_ble && rrgb_ble_active(tick));
}

static void set_pos(struct rrgb *px, uint16_t n, uint8_t pos, struct rrgb c) {
    int led = rrgb_led_for_position(pos);
    if (led >= 0 && led < (int)n) { px[led] = c; }
}

void rrgb_overlay_render(struct rrgb *px, uint16_t n, uint32_t tick, uint8_t tint_v) {
    bool overview = fn_overview();
    uint8_t style = s_caps_style;

    /* 1. Fn-highlight (ind.fn_highlight): black out, light the keys whose
     *    layer-1 binding is not transparent (F1..F4 are repainted by the BLE
     *    status in step 4). */
    if (overview) {
        for (uint16_t i = 0; i < n; i++) { px[i] = (struct rrgb){0, 0, 0}; }
        for (uint16_t p = 0; p < RRGB_FN_MASK_WORDS * 32; p++) {
            if (fn_key(p)) { set_pos(px, n, (uint8_t)p, (struct rrgb){255, 255, 255}); }
        }
    }
    /* 2. CapsLock (ind.caps_style, ind.caps_color): the key in the colour,
     *    or every LED mixed 50/50 with the colour scaled by tint_v (the
     *    effect's brightness); no tint over the Fn overview or without the
     *    effect (tint_v 0). */
    if (s_caps && style == RRGB_CAPS_KEY) {
        set_pos(px, n, CAPS_POS, rgb_of(s_caps_rgb));
    } else if (s_caps && style == RRGB_CAPS_TINT && tint_v > 0 && !overview) {
        struct rrgb c = rgb_of(s_caps_rgb);

        c = (struct rrgb){scale8(c.r, tint_v), scale8(c.g, tint_v), scale8(c.b, tint_v)};
        for (uint16_t i = 0; i < n; i++) { px[i] = mix50(px[i], c); }
    }
    /* 3. Battery gauge: 10-segment bar on the number row, ~3s window. */
    if (tick < s_bat_until) {
        uint8_t lit = (uint8_t)((s_battery * BAT_SEG_COUNT + 50) / 100);  /* 0..10 */
        uint8_t hue = (uint8_t)(85 * (uint16_t)s_battery / 100);          /* 0%=red,100%=green */
        for (uint8_t s = 0; s < BAT_SEG_COUNT; s++) {
            struct rrgb c = (s < lit) ? hsv2rgb(hue, 255, 255)
                                      : (struct rrgb){8, 8, 8};
            set_pos(px, n, (uint8_t)(BAT_SEG_FIRST + s), c);
        }
    }
    /* 4. BLE status, LAST: F1..F4 replace the Fn white with the slot/output
     * colours, and the passkey guidance wins over the battery gauge on the
     * number row. It paints only the keys it owns. */
    if (s_ble) {
        (void)rrgb_ble_render(px, n, tick);
    }
}
