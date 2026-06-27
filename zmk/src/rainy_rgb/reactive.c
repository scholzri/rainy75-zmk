#include <stdint.h>
#include <stdbool.h>
#include "reactive.h"
#include "effects.h"     /* struct led_xy */
#include "led_map.h"     /* rrgb_led_xy, rrgb_led_for_position */

#define RRGB_N        83
#define PRESS_Q_SIZE  16            /* power of two */
#define PRESS_Q_MASK  (PRESS_Q_SIZE - 1)
#define HEAT_BUMP     200
#define HEAT_DECAY    4            /* per-frame cool, FPS-compensated for 50 fps */

struct press_evt { uint8_t led; uint32_t tick; };

static struct press_evt press_q[PRESS_Q_SIZE];
static volatile uint32_t q_head;     /* producer */
static uint32_t q_tail;              /* consumer */

static struct rrgb_ripple ripples[RRGB_RIPPLE_POOL];
static uint8_t ripple_next;
static uint8_t key_heat[RRGB_N];

void rrgb_reactive_on_press(uint32_t position, uint32_t tick) {
    int led = rrgb_led_for_position(position);
    if (led < 0) {
        return;
    }
    uint32_t h = q_head;
    press_q[h & PRESS_Q_MASK].led = (uint8_t)led;
    press_q[h & PRESS_Q_MASK].tick = tick;
    q_head = h + 1;                  /* publish after writing the slot */
}

void rrgb_reactive_tick(uint32_t tick) {
    (void)tick;
    uint32_t head = q_head;          /* snapshot once */
    while (q_tail != head) {
        struct press_evt e = press_q[q_tail & PRESS_Q_MASK];
        q_tail++;
        const struct led_xy *xy = &rrgb_led_xy[e.led];
        ripples[ripple_next].x = xy->x;
        ripples[ripple_next].y = xy->y;
        ripples[ripple_next].start_tick = e.tick;
        ripples[ripple_next].active = true;
        ripple_next = (uint8_t)((ripple_next + 1) % RRGB_RIPPLE_POOL);
        key_heat[e.led] = (key_heat[e.led] > 255 - HEAT_BUMP)
                              ? 255 : (uint8_t)(key_heat[e.led] + HEAT_BUMP);
    }
    for (int i = 0; i < RRGB_N; i++) {
        key_heat[i] = (key_heat[i] > HEAT_DECAY) ? (uint8_t)(key_heat[i] - HEAT_DECAY) : 0;
    }
}

const struct rrgb_ripple *rrgb_ripples(void) { return ripples; }
uint8_t rrgb_ripple_pool_size(void) { return RRGB_RIPPLE_POOL; }
const uint8_t *rrgb_key_heat(void) { return key_heat; }
