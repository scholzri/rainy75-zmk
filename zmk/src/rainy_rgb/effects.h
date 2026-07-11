#ifndef RAINY_RGB_EFFECTS_H
#define RAINY_RGB_EFFECTS_H
#include <stdint.h>
#include "color.h"
#include "reactive.h"

struct led_xy { uint8_t x, y; };

struct rgb_frame {
    struct rrgb *px;          /* output pixels [n] */
    uint16_t n;               /* pixel count (83) */
    uint32_t tick;            /* monotonic frame counter (age-based reactive timing) */
    uint32_t phase;           /* FPS-independent animation phase (ambient effects) */
    uint8_t hue, sat, val;    /* global params 0..255 */
    uint8_t speed;            /* 1..255 */
    const struct led_xy *xy;  /* spatial map [n] or NULL */
    uint32_t last_press_tick; /* tick of most recent keypress (for reactive) */
    const struct rrgb_ripple *ripples;  /* active ripple sources, or NULL */
    uint8_t ripple_count;               /* ripple pool size */
    const uint8_t *key_heat;            /* [n] per-LED keypress heat, or NULL */
};

typedef void (*rrgb_render_fn)(struct rgb_frame *f);

struct rrgb_effect {
    const char *name;
    rrgb_render_fn render;
};

/* Registry (defined in effects.c). */
extern const struct rrgb_effect rrgb_effects[];
extern const uint16_t rrgb_effect_count;

/* Individual effects (host-testable). */
void fx_solid(struct rgb_frame *f);
void fx_rainbow_wave(struct rgb_frame *f);
void fx_plasma(struct rgb_frame *f);
void fx_twinkle(struct rgb_frame *f);
void fx_comet(struct rgb_frame *f);
void fx_aurora(struct rgb_frame *f);
void fx_reactive_pulse(struct rgb_frame *f);
void fx_ripple(struct rgb_frame *f);
void fx_wave(struct rgb_frame *f);
void fx_rain(struct rgb_frame *f);
void fx_heatmap(struct rgb_frame *f);
void fx_speed_colour(struct rgb_frame *f);

#endif
