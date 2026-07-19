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

/* LED VCC rail (PC2) management: cut the rail only after the strip has stayed
 * dark this long — the hold-off keeps overlay flicker (caps toggling on/off)
 * from bouncing the MOSFET — and give the rail a moment to settle before the
 * first frame after re-power (WS2812 power-on reset). */
#define RRGB_RAIL_OFF_HOLD_MS 2000
#define RRGB_RAIL_OFF_TICKS   (RRGB_RAIL_OFF_HOLD_MS / RRGB_PERIOD_MS)
#define RRGB_RAIL_SETTLE_MS   5

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
    /* Activity-idle blank (CONFIG_RAINY_RGB_IDLE_BLANK); never persisted.
     * Written from the ZMK event thread, read by the render thread — volatile
     * without locking, same pattern as host_mode below. */
    volatile bool idle;
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

/* Host direct-pixel mode: written from the mcumgr (SMP) thread, read by the
 * render thread. A torn frame is a one-frame glitch at 50 FPS — harmless —
 * so a volatile flag without locking is enough. */
static struct rrgb host_px[RRGB_N];
static volatile bool host_mode;

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
    if (host_mode) {
        /* Host direct mode: the host's buffer replaces the effect layer. */
        for (uint16_t i = 0; i < RRGB_N; i++) { pixels[i] = host_px[i]; }
    } else if (rt.on) {
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
    bool rail_on = true;        /* driver init leaves PC2 HIGH */
    uint32_t dark_ticks = 0;
    for (;;) {
        /* Deadline-based pacing: render_once sleeps through the ~2.66 ms DMA
         * transfer (End-IRQ), so sleep only the remainder of the frame period
         * to hold a steady RRGB_FPS regardless of render duration. */
        int64_t deadline = k_uptime_get() + RRGB_PERIOD_MS;

        /* Render when the effect is on OR a functional overlay (caps / Fn-highlight
         * / battery gauge) needs to show — so indicators work even with RGB off.
         * With CONFIG_RAINY_RGB_IDLE_BLANK, activity-idle also blanks the strip
         * (first keypress restores it); host direct mode overrides the blank so a
         * host notification pulse still shows when the board is idle. When the
         * option is off, IS_ENABLED() folds idle_off to false — identical behaviour. */
        bool idle_off = IS_ENABLED(CONFIG_RAINY_RGB_IDLE_BLANK) && rt.idle && !host_mode;
        if (!idle_off && (rt.on || host_mode || rrgb_overlay_active(rt.tick))) {
            if (!rail_on) {
                rrgb_strip_power(true);
                rail_on = true;
                k_msleep(RRGB_RAIL_SETTLE_MS);
            }
            render_once();
            was_lit = true;
            dark_ticks = 0;
        } else {
            if (was_lit) {
                clear_strip();  /* clear once when nothing needs to show —
                                 * the black frame goes out while the rail is
                                 * still up; the rail is cut only after the
                                 * hold-off below */
                was_lit = false;
            }
            if (rail_on && ++dark_ticks >= RRGB_RAIL_OFF_TICKS) {
                rrgb_strip_power(false);
                rail_on = false;
            }
        }

        int64_t remain = deadline - k_uptime_get();
        if (remain > 0) {
            k_sleep(K_MSEC(remain));
        } else {
            k_yield();          /* render overran the budget — don't stall */
        }
    }
}

/* Physical Fn+RGB controls double as the host-mode escape hatch: the first
 * press only exits host mode (no other change), so a stray script can never
 * lock the user out of their lighting. */
static bool host_mode_escape(void) {
    if (!host_mode) { return false; }
    host_mode = false;
    LOG_INF("host mode off (Fn control)");
    return true;
}

void rrgb_toggle(void) {
    if (host_mode_escape()) { return; }
    rt.on = !rt.on;
    LOG_INF("rgb %s", rt.on ? "on" : "off");
    rrgb_request_save();
}
void rrgb_next_effect(void) {
    if (host_mode_escape()) { return; }
    rt.effect = (rt.effect + 1) % rrgb_effect_count;
    LOG_INF("effect %u (%s)", rt.effect, rrgb_effects[rt.effect].name);
    rrgb_request_save();
}
void rrgb_hue_step(int dir) {
    if (host_mode_escape()) { return; }
    rt.hue += (dir >= 0) ? 8 : (uint8_t)-8; rrgb_request_save();
}
void rrgb_val_step(int dir) {
    if (host_mode_escape()) { return; }
    int v = rt.val + (dir >= 0 ? 16 : -16);
    rt.val = (v < 16) ? 16 : (v > 255 ? 255 : v);
    rrgb_request_save();
}
void rrgb_speed_step(int dir) {
    if (host_mode_escape()) { return; }
    int s = rt.speed + (dir >= 0 ? 8 : -8);
    rt.speed = (s < 1) ? 1 : (s > 255 ? 255 : s);
    rrgb_request_save();
}

/* --- Host direct-pixel API (called from the mcumgr SMP thread) --- */

void rrgb_host_set_pixels(const uint8_t *quads, uint16_t count) {
    if (!host_mode) {
        /* Entering host mode: start from black so a partial set lights
         * exactly the requested keys and nothing else. */
        for (uint16_t i = 0; i < RRGB_N; i++) { host_px[i] = (struct rrgb){0, 0, 0}; }
    }
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *q = &quads[i * 4];
        int led = rrgb_led_for_position(q[0]);
        if (led < 0) { continue; }
        host_px[led] = (struct rrgb){q[1], q[2], q[3]};
    }
    host_mode = true;
}

void rrgb_host_fill(uint8_t r, uint8_t g, uint8_t b) {
    for (uint16_t i = 0; i < RRGB_N; i++) { host_px[i] = (struct rrgb){r, g, b}; }
    host_mode = true;
}

void rrgb_host_clear(void) {
    host_mode = false;
}

bool rrgb_host_active(void) {
    return host_mode;
}
/* Activity-idle hook (CONFIG_RAINY_RGB_IDLE_BLANK). Fed by the ZMK
 * activity_state_changed event; the render loop blanks while idle. */
void rrgb_set_idle(bool idle) {
    if (rt.idle != idle) {
        rt.idle = idle;
        LOG_INF("rgb %s (activity)", idle ? "idle-off" : "resume");
    }
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
