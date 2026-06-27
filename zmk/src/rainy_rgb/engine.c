#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include "engine.h"
#include "effects.h"
#include "led_map.h"
#include "reactive.h"
#include "zmk_adapter.h"
#include "overlay.h"

LOG_MODULE_REGISTER(rrgb_engine, CONFIG_LOG_DEFAULT_LEVEL);

#define RRGB_N         83
#define RRGB_FPS       50
#define RRGB_PERIOD_MS (1000 / RRGB_FPS)   /* 20 ms target frame period (exact) */
#define RRGB_STACK     1024
#define RRGB_PRIO      10   /* preemptible, below BLE */

/* --- Animation speed model (FPS-independent) ---
 * Ambient effects advance off a shared phase accumulator, NOT the raw frame
 * counter, so animation speed is decoupled from RRGB_FPS (raising the frame
 * rate makes motion smoother, not faster) and is identical across all effects.
 * The speed knob (1..255) maps to phase-units/second; the per-frame increment
 * is that / RRGB_FPS in .8 fixed point — sub-unit, so the slowest step truly
 * crawls (integer tick*factor could never go below 1 unit/frame). Tune feel
 * with these two constants: */
#define RRGB_SPEED_MIN_UPS_Q8   1792   /* slowest: ~7 u/s -> one color cycle / ~34 s */
#define RRGB_SPEED_SLOPE_UPS_Q8  144   /* per speed step; speed=255 -> ~150 u/s (~1.7 s/cycle) */

static inline uint32_t rrgb_speed_increment(uint8_t speed) {
    uint32_t ups_q8 = RRGB_SPEED_MIN_UPS_Q8 + (uint32_t)speed * RRGB_SPEED_SLOPE_UPS_Q8;
    return ups_q8 / RRGB_FPS;   /* per-frame phase increment, .8 fixed point */
}

struct rrgb_runtime {
    bool on;
    uint8_t effect;
    uint8_t hue, sat, val, speed;
    uint32_t tick;
    uint32_t last_press_tick;
};

static struct rrgb_runtime rt = {
    .on = true, .effect = 0, .hue = 0, .sat = 255, .val = 200, .speed = 32,
};
static struct rrgb pixels[RRGB_N];
static uint32_t anim_phase_q8;   /* .8 fixed-point animation phase accumulator */

#define RRGB_PERSIST_VERSION 1

void rrgb_get_persist(struct rrgb_persist *out) {
    out->version = RRGB_PERSIST_VERSION;
    out->on = rt.on; out->effect = rt.effect; out->hue = rt.hue;
    out->sat = rt.sat; out->val = rt.val; out->speed = rt.speed;
}
void rrgb_set_persist(const struct rrgb_persist *in) {
    if (in->version != RRGB_PERSIST_VERSION) { return; }
    rt.on = in->on;
    rt.effect = (in->effect < rrgb_effect_count) ? in->effect : 0;
    rt.hue = in->hue; rt.sat = in->sat;
    rt.val = (in->val < 16) ? 16 : in->val;
    rt.speed = (in->speed < 1) ? 1 : in->speed;
}

K_THREAD_STACK_DEFINE(rrgb_stack, RRGB_STACK);
static struct k_thread rrgb_thread;

static void render_once(void) {
    rrgb_reactive_tick(rt.tick);
    anim_phase_q8 += rrgb_speed_increment(rt.speed);
    struct rgb_frame f = {
        .px = pixels, .n = RRGB_N, .tick = rt.tick, .phase = anim_phase_q8 >> 8,
        .hue = rt.hue, .sat = rt.sat, .val = rt.val, .speed = rt.speed,
        .xy = rrgb_led_xy, .last_press_tick = rt.last_press_tick,
        .ripples = rrgb_ripples(),
        .ripple_count = rrgb_ripple_pool_size(),
        .key_heat = rrgb_key_heat(),
    };
    if (rt.on) {
        rrgb_effects[rt.effect].render(&f);
    } else {
        /* RGB toggled off: black base so functional overlays still show. */
        for (uint16_t i = 0; i < RRGB_N; i++) { pixels[i] = (struct rrgb){0, 0, 0}; }
    }
    rrgb_overlay_render(pixels, RRGB_N, rt.tick);
    rrgb_strip_show(pixels, RRGB_N);
    rt.tick++;
}

static void clear_strip(void) {
    for (int i = 0; i < RRGB_N; i++) { pixels[i] = (struct rrgb){0, 0, 0}; }
    rrgb_strip_show(pixels, RRGB_N);
}

static void rrgb_loop(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    bool was_lit = false;
    for (;;) {
        /* Deadline-based pacing: render_once sleeps through the ~2.66 ms DMA
         * transfer (End-IRQ), so sleep only the remainder of the frame period
         * to hold a steady RRGB_FPS regardless of render duration. */
        int64_t deadline = k_uptime_get() + RRGB_PERIOD_MS;

        /* Render when the effect is on OR a functional overlay (caps / Fn-highlight
         * / battery gauge) needs to show — so indicators work even with RGB off. */
        if (rt.on || rrgb_overlay_active(rt.tick)) {
            render_once();
            was_lit = true;
        } else if (was_lit) {
            clear_strip();      /* clear once when nothing needs to show */
            was_lit = false;
        }

        int64_t remain = deadline - k_uptime_get();
        if (remain > 0) {
            k_sleep(K_MSEC(remain));
        } else {
            k_yield();          /* render overran the budget — don't stall */
        }
    }
}

void rrgb_toggle(void) {
    rt.on = !rt.on;
    LOG_INF("rgb %s", rt.on ? "on" : "off");
    rrgb_request_save();
}
void rrgb_next_effect(void) {
    rt.effect = (rt.effect + 1) % rrgb_effect_count;
    LOG_INF("effect %u (%s)", rt.effect, rrgb_effects[rt.effect].name);
    rrgb_request_save();
}
void rrgb_hue_step(int dir)   { rt.hue += (dir >= 0) ? 8 : (uint8_t)-8; rrgb_request_save(); }
void rrgb_val_step(int dir) {
    int v = rt.val + (dir >= 0 ? 16 : -16);
    rt.val = (v < 16) ? 16 : (v > 255 ? 255 : v);
    rrgb_request_save();
}
void rrgb_speed_step(int dir) {
    int s = rt.speed + (dir >= 0 ? 8 : -8);
    rt.speed = (s < 1) ? 1 : (s > 255 ? 255 : s);
    rrgb_request_save();
}
void rrgb_on_key(uint32_t position, bool pressed) {
    if (pressed) {
        rt.last_press_tick = rt.tick;
        rrgb_reactive_on_press(position, rt.tick);
    }
}

void rrgb_battery_gauge_show(void) { rrgb_overlay_battery_show(rt.tick); }

void rrgb_engine_init(void) {
    if (rrgb_strip_init() != 0) { return; }
    /* Persisted state is restored by state.c's SETTINGS_STATIC_HANDLER when ZMK
     * runs settings_load() in main() (after this SYS_INIT). No load here. */
    k_thread_create(&rrgb_thread, rrgb_stack, RRGB_STACK,
                    rrgb_loop, NULL, NULL, NULL, RRGB_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&rrgb_thread, "rainy_rgb");
    LOG_INF("rainy_rgb engine started (%d LEDs @ %d fps)", RRGB_N, RRGB_FPS);
}

static int rrgb_sys_init(void) { rrgb_engine_init(); return 0; }
SYS_INIT(rrgb_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
