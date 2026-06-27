#ifndef RAINY_RGB_OVERLAY_H
#define RAINY_RGB_OVERLAY_H
#include <stdint.h>
#include <stdbool.h>
#include "color.h"   /* struct rrgb */

/* Neutral state — set by the adapter from the ZMK event thread. */
void rrgb_overlay_set_caps(bool on);
void rrgb_overlay_set_fn(bool active);
void rrgb_overlay_set_battery(uint8_t pct);
void rrgb_overlay_battery_show(uint32_t tick);   /* start the ~3s gauge window */

/* Applied AFTER the effect, BEFORE strip_show. */
void rrgb_overlay_render(struct rrgb *px, uint16_t n, uint32_t tick);

/* True if any functional overlay needs to show this frame (caps on, Fn held,
 * or battery gauge window open) — lets the engine render indicators even when
 * the decorative RGB is toggled off. */
bool rrgb_overlay_active(uint32_t tick);

#endif
