#ifndef RAINY_RGB_REACTIVE_H
#define RAINY_RGB_REACTIVE_H
#include <stdint.h>
#include <stdbool.h>

#define RRGB_RIPPLE_POOL 8

struct rrgb_ripple { uint8_t x, y; uint32_t start_tick; bool active; };

/* Producer (ZMK event thread): record a keypress origin. Safe to call from
 * the event callback — only appends to an SPSC queue. */
void rrgb_reactive_on_press(uint32_t position, uint32_t tick);

/* Consumer (render thread, once per frame): drain the queue -> activate
 * ripples + bump key_heat, then decay key_heat. */
void rrgb_reactive_tick(uint32_t tick);

const struct rrgb_ripple *rrgb_ripples(void);
uint8_t rrgb_ripple_pool_size(void);
const uint8_t *rrgb_key_heat(void);

#endif
