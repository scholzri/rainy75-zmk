#ifndef RAINY_RGB_LED_MAP_H
#define RAINY_RGB_LED_MAP_H
#include <stdint.h>
#include "effects.h"

/* LED index -> normalized physical XY (uint8, uniform scale, origin top-left).
 * Points at the calibrated led_positions table (no longer NULL since Phase 2). */
extern const struct led_xy *rrgb_led_xy;

/* Physical XY of the key at the given keymap position — the origin for
 * keypress-driven spatial effects (ripple). Returns NULL if out of range. */
const struct led_xy *rrgb_xy_for_position(uint32_t position);

/* keymap position -> LED index, or -1 if out of range. */
int rrgb_led_for_position(uint32_t position);

#endif
