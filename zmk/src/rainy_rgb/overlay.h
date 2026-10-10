#ifndef RAINY_RGB_OVERLAY_H
#define RAINY_RGB_OVERLAY_H
#include <stdint.h>
#include <stdbool.h>
#include "color.h"   /* struct rrgb */

/* Resolves the keymap-coupled BLE status key table and resets ble_status.
 * ble = false (build without BLE): ble_status owns no keys and is ignored.
 * Call once at boot, before the first BLE event. The indicator settings
 * below are not reset. */
void rrgb_overlay_init(bool ble);

/* Neutral state, set by the adapter from the ZMK event thread. */
void rrgb_overlay_set_caps(bool on);
void rrgb_overlay_set_fn(bool active);
void rrgb_overlay_set_battery(uint8_t pct);
void rrgb_overlay_battery_show(uint32_t tick);   /* start the ~3s gauge window */

/* --- Indicator settings (config/cfg_table.c pushes them from the settings
 * registry; the defaults named here also hold without CONFIG_RAINY75_CONFIG) */
enum rrgb_caps_style { RRGB_CAPS_KEY = 0, RRGB_CAPS_TINT = 1, RRGB_CAPS_OFF = 2 };
/* ind.caps_style + ind.caps_color 0xRRGGBB (default KEY, white): with
 * CapsLock on, KEY lights the CapsLock key in the colour, TINT mixes every
 * LED 50/50 with the colour at the effect's brightness (tint_v of
 * rrgb_overlay_render; not over the Fn overview), OFF shows nothing. */
void rrgb_overlay_set_caps_style(uint8_t style, uint32_t rgb);
/* ind.fn_highlight (default on). Off: holding Fn changes no key here (the
 * BLE status still paints F1..F4). */
void rrgb_overlay_set_fn_highlight(bool on);
/* ind.bat_low: battery percent below which Esc pulses red, 0 = off (default). */
void rrgb_overlay_set_bat_low(uint8_t pct);

/* Fn-highlight keys: bit p (word p / 32, bit p % 32) = keymap position p
 * lights white while the Fn layer is held, the rest goes dark. Empty until
 * set; zmk_adapter.c builds it from the live keymap. */
#define RRGB_FN_MASK_WORDS 3   /* positions 0..95 */
void rrgb_overlay_set_fn_keys(const uint32_t mask[RRGB_FN_MASK_WORDS]);
/* Behavior device name bound at keymap position pos, NULL if none. */
typedef const char *(*rrgb_dev_at_fn)(uint16_t pos, void *ctx);
/* Pure: the mask for positions 0..n-1 (at most 96): a key is lit when it has
 * a binding whose behavior is not trans_name (NULL: none is transparent). */
void rrgb_fn_mask_build(rrgb_dev_at_fn dev_at, void *ctx, uint16_t n, const char *trans_name,
                        uint32_t mask[RRGB_FN_MASK_WORDS]);

/* Low-battery pulse (ind.bat_low): red strength 0..255 on Esc this frame.
 * 0 when off (threshold 0), with a USB host, at level 0 (no reading yet: ZMK
 * reports 0 before its first sample) or at a level >= threshold; else a 2 s
 * triangle, full at tick 0 (tick = render frames at 50 FPS). Pure. */
#define RRGB_BAT_LOW_PERIOD 100
uint8_t rrgb_bat_low_alpha(uint8_t level, uint8_t threshold, bool usb_host, uint32_t tick);
/* The engine calls this right after the effect layer, only while the effect
 * is drawn: blends Esc toward red by rrgb_bat_low_alpha() of the level given
 * to rrgb_overlay_set_battery(). Not part of rrgb_overlay_active(): the pulse
 * never keeps the frame loop or the LED rail on by itself. */
void rrgb_overlay_bat_low_render(struct rrgb *px, uint16_t n, uint32_t tick, bool usb_host);

/* Applied AFTER the effect, BEFORE strip_show. Order: Fn-highlight,
 * CapsLock, battery gauge, BLE status (last, so it owns F1..F4 and wins
 * the number row while the passkey is typed). tint_v: the brightness
 * 0..255 the effect layer was drawn at this frame (its rendered value
 * times the effect gain), 0 when it was not drawn (RGB off, idle off,
 * faded out for BLE, host pixel mode); the CapsLock tint colour is scaled
 * by it, so 0 shows no tint. */
void rrgb_overlay_render(struct rrgb *px, uint16_t n, uint32_t tick, uint8_t tint_v);

/* True if any functional overlay needs to show this frame (caps on with the
 * key style, Fn held with the highlight on, battery gauge window open, or a
 * BLE status indication), so the engine renders indicators even when the
 * decorative RGB is toggled off or idle. The CapsLock tint shows only on
 * top of a drawn effect and does not count. */
bool rrgb_overlay_active(uint32_t tick);

/* True while ble_status shows an automatic BLE animation (connecting,
 * switching, pairing, see rrgb_ble_suppress_effect): the engine renders the
 * effect layer black (fading), the overlays still render on top. Always
 * false in a build without BLE. Implies rrgb_overlay_active(tick), so the
 * LED rail stays on with RGB off. */
bool rrgb_overlay_suppress_effect(uint32_t tick);

/* Effect layer gain 0..255 for the next frame: down to 0 over
 * RRGB_EFFECT_FADE_OUT_FRAMES while suppressed, back to 255 over
 * RRGB_EFFECT_FADE_IN_FRAMES after. Pure, the engine keeps the value. */
#define RRGB_EFFECT_FADE_OUT_FRAMES  5    /* 0.1 s at 50 FPS */
#define RRGB_EFFECT_FADE_IN_FRAMES  25    /* 0.5 s */
uint8_t rrgb_effect_gain_next(uint8_t gain, bool suppress);
/* Per frame: rrgb_effect_gain_next() while the effect layer is drawn
 * (drawn = RGB on and not idle off); otherwise the gain is invisible and
 * snaps to its target (0 while suppressed, else 255), so it never freezes mid
 * fade while RGB is off or the render loop is stopped. */
uint8_t rrgb_effect_gain_frame(uint8_t gain, bool suppress, bool drawn);

/* False while the Fn layer is held: Fn combinations are commands (BT slots,
 * output, media, RGB controls), not typing, so they leave no reactive
 * afterglow or ripple; the indicators own those keys. Also false while the
 * effect is suppressed (tick = current render frame): the passkey digits
 * typed during pairing would otherwise leave invisible reactive state
 * (heat, ripples, the walker's step) that pops when the effect returns. */
bool rrgb_overlay_key_reactive(uint32_t position, uint32_t tick);

#endif
