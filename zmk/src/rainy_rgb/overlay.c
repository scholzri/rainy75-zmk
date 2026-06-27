#include <stdint.h>
#include <stdbool.h>
#include "overlay.h"
#include "color.h"
#include "led_map.h"     /* rrgb_led_for_position */

#define CAPS_POS         44   /* keymap position of CapsLock (&kp CLCK) */
#define BAT_SHOW_FRAMES  90   /* ~3s at 30fps */
#define BAT_SEG_FIRST    16   /* number row keys 1..0 = positions 16..25 */
#define BAT_SEG_COUNT    10

/* Fn-active keymap positions (non-&trans on layer 1). KEYMAP-COUPLED:
 * update this if the Fn layer changes. */
static const uint8_t fn_keys[] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,   /* top row: studio/BT/output/media/BT_CLR */
    28,   /* BKSP  -> RGB_TOG */
    43,   /* Enter -> RGB_EFF */
    56,   /* NUHS  -> RGB_HUI */
    65,   /* B     -> RGB_BAT */
    72,   /* UP    -> RGB_BRI */
    80, 81, 82,                            /* arrows -> speed/bright */
};
#define FN_KEYS_COUNT (sizeof(fn_keys) / sizeof(fn_keys[0]))

static volatile bool     s_caps;
static volatile bool     s_fn;
static volatile uint8_t  s_battery;
static volatile uint32_t s_bat_until;

void rrgb_overlay_set_caps(bool on)        { s_caps = on; }
void rrgb_overlay_set_fn(bool active)      { s_fn = active; }
void rrgb_overlay_set_battery(uint8_t pct) { s_battery = pct; }
void rrgb_overlay_battery_show(uint32_t tick) { s_bat_until = tick + BAT_SHOW_FRAMES; }

bool rrgb_overlay_active(uint32_t tick) {
    return s_caps || s_fn || (tick < s_bat_until);
}

static void set_pos(struct rrgb *px, uint16_t n, uint8_t pos, struct rrgb c) {
    int led = rrgb_led_for_position(pos);
    if (led >= 0 && led < (int)n) { px[led] = c; }
}

void rrgb_overlay_render(struct rrgb *px, uint16_t n, uint32_t tick) {
    /* 1. Fn-highlight: black out, light only Fn-active keys white. */
    if (s_fn) {
        for (uint16_t i = 0; i < n; i++) { px[i] = (struct rrgb){0, 0, 0}; }
        for (unsigned k = 0; k < FN_KEYS_COUNT; k++) {
            set_pos(px, n, fn_keys[k], (struct rrgb){255, 255, 255});
        }
    }
    /* 2. CapsLock: white on the CapsLock key. */
    if (s_caps) {
        set_pos(px, n, CAPS_POS, (struct rrgb){255, 255, 255});
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
}
