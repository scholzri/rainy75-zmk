#include "led_map.h"

#define RRGB_N 83

/* Calibrated on hardware 2026-06-27 via the built-in calibration mode.
 * The WS2812 chain order mostly matches keymap position order (identity),
 * EXCEPT the nav cluster: after ISO-Enter the chain routes through PGUP
 * (pos 57) before row 3 (CapsLock..NUHS = pos 44..56), then PGDN (pos 58).
 * led_to_pos[led] = keymap position under that LED (kept here as a comment
 * for provenance; runtime only needs the inverse + the XY table):
 *   identity 0..43, then led44->57, led45..57->44..56, identity 58..82.
 */

/* keymap position -> led index (inverse of the calibration). */
static const uint8_t pos_to_led[RRGB_N] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
    36, 37, 38, 39, 40, 41, 42, 43, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 44, 58, 59,
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82,
};

/* led index -> normalized physical XY (uniform scale by max x-center=1575,
 * so a circular ripple stays circular; y-centers span 0..91). */
static const struct led_xy led_positions[RRGB_N] = {
    {8,8}, {29,8}, {45,8}, {62,8}, {78,8}, {99,8},
    {115,8}, {131,8}, {147,8}, {167,8}, {183,8}, {199,8},
    {215,8}, {239,8}, {255,8}, {8,26}, {24,26}, {40,26},
    {57,26}, {73,26}, {89,26}, {105,26}, {121,26}, {138,26},
    {154,26}, {170,26}, {186,26}, {202,26}, {229,26}, {255,26},
    {13,42}, {35,42}, {51,42}, {67,42}, {83,42}, {100,42},
    {116,42}, {132,42}, {148,42}, {164,42}, {181,42}, {197,42},
    {213,42}, {234,50}, {255,42}, {15,58}, {38,58}, {54,58},
    {70,58}, {87,58}, {103,58}, {119,58}, {135,58}, {151,58},
    {168,58}, {184,58}, {200,58}, {216,58}, {255,58}, {10,74},
    {28,74}, {45,74}, {61,74}, {77,74}, {93,74}, {109,74},
    {125,74}, {142,74}, {158,74}, {174,74}, {190,74}, {215,74},
    {239,74}, {11,91}, {32,91}, {53,91}, {113,91}, {172,91},
    {188,91}, {204,91}, {223,91}, {239,91}, {255,91},
};

const struct led_xy *rrgb_led_xy = led_positions;

const struct led_xy *rrgb_xy_for_position(uint32_t position) {
    if (position >= RRGB_N) {
        return (const struct led_xy *)0;
    }
    return &led_positions[pos_to_led[position]];
}

int rrgb_led_for_position(uint32_t position) {
    if (position >= RRGB_N) {
        return -1;
    }
    return pos_to_led[position];
}
