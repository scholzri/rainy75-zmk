#include <stdint.h>
#include "effects.h"

static uint8_t hash8(uint32_t x) {            /* cheap deterministic noise */
    x = (x ^ 61u) ^ (x >> 16);
    x *= 9u; x ^= x >> 4; x *= 0x27d4eb2du; x ^= x >> 15;
    return (uint8_t)x;
}

void fx_solid(struct rgb_frame *f) {
    struct rrgb c = hsv2rgb(f->hue, f->sat, f->val);
    for (uint16_t i = 0; i < f->n; i++) { f->px[i] = c; }
}

void fx_rainbow_wave(struct rgb_frame *f) {
    uint8_t step = (uint8_t)f->phase;
    for (uint16_t i = 0; i < f->n; i++) {
        uint8_t h = f->hue + (uint8_t)(i * 256 / f->n) + step;
        f->px[i] = hsv2rgb(h, f->sat, f->val);
    }
}

/* Plasma: two summed sines drive the hue field. */
void fx_plasma(struct rgb_frame *f) {
    uint8_t t = (uint8_t)f->phase;
    for (uint16_t i = 0; i < f->n; i++) {
        uint8_t a = sin8((uint8_t)(i * 6 + t));
        uint8_t b = sin8((uint8_t)(i * 3 - t * 2));
        f->px[i] = hsv2rgb((uint8_t)(f->hue + a / 2 + b / 2), f->sat, f->val);
    }
}

/* Twinkle: each LED has a hash-phased triangle brightness. */
void fx_twinkle(struct rgb_frame *f) {
    uint8_t t = (uint8_t)f->phase;
    for (uint16_t i = 0; i < f->n; i++) {
        uint8_t phase = (uint8_t)(hash8(i) + t);
        uint8_t b = sin8(phase);
        b = (b > 128) ? scale8((b - 128) * 2, f->val) : 0;   /* sparse sparkle */
        f->px[i] = hsv2rgb((uint8_t)(f->hue + hash8(i + 99)), f->sat, b);
    }
}

/* Comet: bright head + fading tail running along the chain. */
void fx_comet(struct rgb_frame *f) {
    uint16_t head = (uint16_t)(f->phase % f->n);
    for (uint16_t i = 0; i < f->n; i++) {
        uint16_t dist = (head >= i) ? (head - i) : (f->n - i + head);
        uint8_t b = (dist < 12) ? scale8(f->val, (uint8_t)(255 - dist * 21)) : 0;
        f->px[i] = hsv2rgb(f->hue, f->sat, b);
    }
}

/* Aurora: slow drift between hue and hue+64 across the chain. */
void fx_aurora(struct rgb_frame *f) {
    uint8_t t = (uint8_t)f->phase;
    for (uint16_t i = 0; i < f->n; i++) {
        uint8_t mix = sin8((uint8_t)(i * 4 + t));
        uint8_t h = (uint8_t)(f->hue + scale8(64, mix));
        f->px[i] = hsv2rgb(h, f->sat, f->val);
    }
}

/* Reactive: brightness flashes on each keypress (global), decays over ticks. */
void fx_reactive_pulse(struct rgb_frame *f) {
    uint32_t age = f->tick - f->last_press_tick;   /* frames since last press */
    uint8_t boost = (age < 32) ? (uint8_t)(255 - age * 8) : 0;   /* ~0.6 s @ 50 fps */
    uint8_t base = f->val / 5;
    uint8_t v = (base + boost > 255) ? 255 : base + boost;
    struct rrgb c = hsv2rgb(f->hue, f->sat, v);
    for (uint16_t i = 0; i < f->n; i++) { f->px[i] = c; }
}

#define RIPPLE_RING   22      /* ring half-thickness (XY units) */
#define RIPPLE_FADER  255     /* radius at which a ring has fully faded */

void fx_ripple(struct rgb_frame *f) {
    for (uint16_t i = 0; i < f->n; i++) { f->px[i] = (struct rrgb){0, 0, 0}; }
    if (!f->xy || !f->ripples) { return; }
    /* Expansion velocity in .4 fixed point, FPS-compensated (×~0.6 vs the old
     * 30 fps integer rate) so rings keep their feel at 50 fps; still speed-scaled. */
    int vel_q4 = (f->speed / 16 + 1) * 10;
    for (uint8_t r = 0; r < f->ripple_count; r++) {
        const struct rrgb_ripple *rp = &f->ripples[r];
        if (!rp->active) { continue; }
        int radius = ((int)(f->tick - rp->start_tick) * vel_q4) >> 4;
        if (radius >= RIPPLE_FADER) { continue; }     /* faded out */
        uint8_t fade = (uint8_t)(255 - radius);       /* older/bigger = dimmer */
        for (uint16_t i = 0; i < f->n; i++) {
            uint8_t dist = hypot8((int)f->xy[i].x - rp->x, (int)f->xy[i].y - rp->y);
            int d = (int)dist - radius;
            if (d < 0) { d = -d; }
            if (d < RIPPLE_RING) {
                uint8_t edge = (uint8_t)((RIPPLE_RING - d) * (255 / RIPPLE_RING));
                uint8_t b = scale8(scale8(edge, fade), f->val);
                struct rrgb c = hsv2rgb((uint8_t)(f->hue + radius * 2), f->sat, b);
                if (c.r > f->px[i].r) { f->px[i].r = c.r; }
                if (c.g > f->px[i].g) { f->px[i].g = c.g; }
                if (c.b > f->px[i].b) { f->px[i].b = c.b; }
            }
        }
    }
}

/* Wave: diagonal traveling sine wave across 2D XY space. */
void fx_wave(struct rgb_frame *f) {
    if (!f->xy) { for (uint16_t i=0;i<f->n;i++) f->px[i]=(struct rrgb){0,0,0}; return; }
    uint8_t t = (uint8_t)f->phase;
    for (uint16_t i = 0; i < f->n; i++) {
        uint8_t phase = (uint8_t)(f->xy[i].x + f->xy[i].y - t);   /* diagonal travel */
        uint8_t b = sin8(phase);
        f->px[i] = hsv2rgb((uint8_t)(f->hue + b / 2), f->sat, scale8(b, f->val));
    }
}

#define RAIN_DROPS    6
#define RAIN_COL_W    14      /* column half-width (XY units) */
#define RAIN_TAIL     34      /* tail length above the head (XY units) */

/* Rain: drops fall top→bottom, bright head + fading tail. */
void fx_rain(struct rgb_frame *f) {
    static int16_t drop_y[RAIN_DROPS];
    static uint8_t drop_x[RAIN_DROPS];
    static bool rain_init;
    if (!rain_init) {
        for (int d = 0; d < RAIN_DROPS; d++) {
            drop_y[d] = -(int16_t)(hash8((uint32_t)d * 40u) % 120);
            drop_x[d] = hash8((uint32_t)d * 7u);
        }
        rain_init = true;
    }
    uint8_t spd = (uint8_t)(f->speed / 32 + 1);   /* FPS-compensated fall velocity */
    for (int d = 0; d < RAIN_DROPS; d++) {
        drop_y[d] += spd;
        if (drop_y[d] > 110) {                       /* fell off bottom -> respawn at top */
            drop_y[d] = -(int16_t)(hash8(((uint32_t)d * 40u) ^ f->tick) % 120);
            drop_x[d] = hash8(((uint32_t)d * 7u) ^ f->tick);
        }
    }
    for (uint16_t i = 0; i < f->n; i++) { f->px[i] = (struct rrgb){0, 0, 0}; }
    if (!f->xy) { return; }
    for (uint16_t i = 0; i < f->n; i++) {
        for (int d = 0; d < RAIN_DROPS; d++) {
            int ddx = (int)f->xy[i].x - drop_x[d];
            if (ddx < 0) { ddx = -ddx; }
            if (ddx > RAIN_COL_W) { continue; }
            int ddy = drop_y[d] - (int)f->xy[i].y;    /* head at drop_y, tail above */
            if (ddy < 0 || ddy >= RAIN_TAIL) { continue; }
            uint8_t b = scale8((uint8_t)(255 - ddy * (255 / RAIN_TAIL)), f->val);
            struct rrgb c = hsv2rgb(f->hue, f->sat, b);
            if (c.r > f->px[i].r) { f->px[i].r = c.r; }
            if (c.g > f->px[i].g) { f->px[i].g = c.g; }
            if (c.b > f->px[i].b) { f->px[i].b = c.b; }
        }
    }
}

/* Heatmap: keys glow on press (black->red->yellow->white) and cool over time. */
void fx_heatmap(struct rgb_frame *f) {
    if (!f->key_heat) { for (uint16_t i=0;i<f->n;i++) f->px[i]=(struct rrgb){0,0,0}; return; }
    for (uint16_t i = 0; i < f->n; i++) {
        uint8_t h = f->key_heat[i];
        uint8_t hue = scale8(h, 40);                 /* red -> toward yellow as it heats */
        uint8_t sat = (uint8_t)(255 - scale8(h, 90)); /* white-hot at peak */
        f->px[i] = hsv2rgb(hue, sat, scale8(h, f->val));
    }
}


/* Speed-colour (after the stock firmware's "Trigger Colour" mode): the whole
 * board wears one colour whose depth tracks typing speed. The decaying per-LED
 * key_heat[] summed over the board is the speed signal — idle settles to a
 * pale, dimmer tint; fast typing drives saturation and brightness toward the
 * full vivid colour. Hue/sat/val knobs still apply (hue picks the colour,
 * sat/val cap the deep end). */
void fx_speed_colour(struct rgb_frame *f) {
    uint32_t total = 0;
    if (f->key_heat) {
        for (uint16_t i = 0; i < f->n; i++) { total += f->key_heat[i]; }
    }
    /* ~6 keys' worth of fresh heat reaches full depth (steady fast typing) */
    uint8_t depth = (total >= 1530) ? 255 : (uint8_t)(total / 6);
    uint8_t sat = scale8(f->sat, (uint8_t)(80 + scale8(175, depth)));
    uint8_t val = scale8(f->val, (uint8_t)(96 + scale8(159, depth)));
    struct rrgb col = hsv2rgb(f->hue, sat, val);
    for (uint16_t i = 0; i < f->n; i++) { f->px[i] = col; }
}

const struct rrgb_effect rrgb_effects[] = {
    { "solid",      fx_solid },
    { "rainbow",    fx_rainbow_wave },
    { "plasma",     fx_plasma },
    { "twinkle",    fx_twinkle },
    { "comet",      fx_comet },
    { "aurora",     fx_aurora },
    { "reactive",   fx_reactive_pulse },
    { "ripple",     fx_ripple },
    { "wave",       fx_wave },
    { "rain",       fx_rain },
    { "heatmap",    fx_heatmap },
    { "speedcolour", fx_speed_colour },
};
const uint16_t rrgb_effect_count = sizeof(rrgb_effects) / sizeof(rrgb_effects[0]);
