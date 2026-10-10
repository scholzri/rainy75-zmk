#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_USB_DC_B91)
#include <b91_usb_diag.h>
#endif
#include "engine.h"
#include "effects.h"
#include "led_map.h"
#include "reactive.h"
#include "zmk_adapter.h"
#include "overlay.h"
#include "lighting.h"

LOG_MODULE_REGISTER(rrgb_engine, CONFIG_LOG_DEFAULT_LEVEL);

#define RRGB_N         83
#define RRGB_FPS       50
#define RRGB_PERIOD_MS (1000 / RRGB_FPS)   /* 20 ms target frame period (exact) */
/* 1024 was too tight: a render frame nests render_once() -> an effect's render()
 * -> rrgb_overlay_render() -> rrgb_strip_show(), a LOG_WRN on that path adds a
 * logging frame on top, and every interrupt pushes its exception frame onto
 * this stack before switching to the ISR stack. A static walk of the worst
 * path came to roughly 770 bytes, so 1 KB left almost no margin.
 *
 * An overflow here is not a crash you can find later. The B91 has no PMP stack
 * guard, so nothing faults: the excess frames land in the neighbouring thread
 * stack (the BLE RX stack sits directly below this one in .noinit) and that
 * thread's own writes corrupt this loop's saved locals in turn, e.g. the frame
 * deadline, leaving the loop asleep for a very long time. The board keeps
 * typing, USB and SMP keep answering, host-mode frames are still ACCEPTED, and
 * the strip is simply dark with nothing in the log. That is the dark-strip
 * episode. (A real fault would NOT look like this: mcuboot_confirm.c overrides
 * k_sys_fatal_error_handler to cold-reboot, so a faulting thread reboots the
 * whole board.) CONFIG_STACK_SENTINEL in app.conf turns a future overflow into
 * exactly that logged reboot instead of a silent hang, and CONFIG_INIT_STACKS
 * lets rrgb_stack_unused() report the real headroom. */
#define RRGB_STACK     2048
#define RRGB_PRIO      10   /* preemptible, below BLE */

/* LED VCC rail (PC2) management: cut the rail only after the strip has stayed
 * dark this long — the hold-off keeps overlay flicker (caps toggling on/off)
 * from bouncing the MOSFET — and give the rail a moment to settle before the
 * first frame after re-power (WS2812 power-on reset). */
#define RRGB_RAIL_OFF_HOLD_MS 2000
#define RRGB_RAIL_OFF_TICKS   (RRGB_RAIL_OFF_HOLD_MS / RRGB_PERIOD_MS)
#define RRGB_RAIL_SETTLE_MS   5

/* --- Black-box instrumentation (B91_DIAG_RGB_STATE) ---------------------
 * Chasing a dark-strip episode where the firmware was healthy, SMP answered,
 * host-mode frames were accepted (which overrides idle off) and the DMA
 * never reported a timeout — yet nothing lit, and only a USB resume restored
 * it.  That combination points at the LED rail being low while the render loop
 * believed it high: `rail_on` is loop-local state, so anything that drops PC2
 * behind its back is invisible to it and never re-powered.
 *
 * Sample what the loop believes AND what the pin actually reads, and record
 * every change into the USB diagnostic ring (survives replug on battery,
 * readable with reverse/tools/usb_diag.py).
 */
/* Keepalive cadence is bounded by the ring, not by curiosity: 64 entries
 * shared with the USB events, so a 5 min tick emits ~96 entries overnight and
 * wraps away the very divergence the trap exists to catch.  30 min fits a full
 * night (~16 entries) with room for transitions. */
#define RRGB_DIAG_KEEPALIVE_TICKS (30 * 60 * RRGB_FPS)  /* ~30 min */

/* Sample the rail and record it, returning the raw pin state so the caller can
 * act on it without reading the registers a second time.  Note the frame count
 * is private: rt.tick only advances inside render_once(), so it freezes in
 * exactly the blanked state this trap has to keep watching. */
static uint8_t rrgb_diag_state(bool rail_believed, bool idle, bool host, bool on)
{
	uint8_t rail = rrgb_strip_rail_state();

#if IS_ENABLED(CONFIG_USB_DC_B91) && IS_ENABLED(CONFIG_LED_STRIP_B91_SPI_PC2_POWER)
	static uint8_t last_bits = 0xFF;
	static uint32_t last_tick;
	static uint32_t ticks;

	uint8_t bits = (rail_believed ? BIT(0) : 0) | (idle ? BIT(1) : 0) |
		       (host ? BIT(2) : 0) | (on ? BIT(3) : 0) |
		       ((rail & RRGB_RAIL_HIGH) ? BIT(4) : 0) |
		       ((rail & RRGB_RAIL_OUT_EN) ? BIT(5) : 0) |
		       ((rail & RRGB_RAIL_GPIO_MODE) ? BIT(6) : 0);

	ticks++;
	if (bits != last_bits ||
	    (ticks - last_tick) >= RRGB_DIAG_KEEPALIVE_TICKS) {
		last_bits = bits;
		last_tick = ticks;
		b91_usb_diag_note(32 /* B91_DIAG_RGB_STATE */, bits,
				  (uint16_t)(ticks >> 4));
	}
#else
	ARG_UNUSED(rail_believed); ARG_UNUSED(idle);
	ARG_UNUSED(host); ARG_UNUSED(on);
#endif
	return rail;
}

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
/* Effect layer gain (render thread only): faded to 0 while ble_status shows
 * an automatic BLE animation (connecting / switching / pairing, see
 * rrgb_overlay_suppress_effect), back to 255 after. */
static uint8_t effect_gain = 255;

/* Lighting settings (rrgb_set_*; config/cfg_table.c pushes them, these are
 * the defaults). Written from the mcumgr work queue, the system work queue
 * or the ZMK main thread, read by the render thread: single bytes and
 * aligned words, volatile without locking like host_mode below (a pair
 * changed together can be torn for one frame). */
static volatile uint8_t val_battery = 255;  /* rgb.val_battery: cap without a USB host */
static volatile uint16_t idle_s;            /* rgb.idle_s, 0 = never */
static volatile uint8_t idle_mode;          /* rgb.idle_mode, enum rrgb_idle_mode */
static volatile bool usb_host;              /* zmk_usb_is_hid_ready(), zmk_adapter.c */
/* Idle timer origin: k_uptime_get_32() of the last key position event or
 * settings change. */
static volatile uint32_t last_activity_ms;

/* Render-loop liveness beat (see rrgb_heartbeat). Written by the render thread,
 * read from the mcumgr (SMP) thread; a 32-bit aligned load is atomic on this
 * core, so volatile without locking is enough — and a torn read would only
 * misreport one sample of a counter the caller compares across a second. */
static volatile uint32_t loop_beat;

/* Host direct-pixel mode: written from the mcumgr (SMP) thread, read by the
 * render thread. A torn frame is a one-frame glitch at 50 FPS — harmless —
 * so a volatile flag without locking is enough. */
static struct rrgb host_px[RRGB_N];
static volatile bool host_mode;

/* Millisecond stamp of the last host frame, for the host-mode watchdog
 * (CONFIG_RGB_MGMT_HOST_TIMEOUT_S). Deliberately the 32-bit uptime: a 64-bit
 * load is not atomic on this core, and the comparison below uses a signed
 * delta, which tolerates the ~49-day wrap the same way the strip driver's
 * reset-latch deadline does. */
static volatile uint32_t host_last_ms;

/* Enter or refresh host mode. The stamp is written BEFORE the flag so the
 * render thread can never observe host_mode set against a stale timestamp and
 * expire the frame it was just handed. */
static void host_touch(void) {
    host_last_ms = k_uptime_get_32();
    host_mode = true;
}

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

static void render_once(enum rrgb_idle_state idle) {
    /* idle "off": the effect layer is off, the overlays still show */
    bool effect_on = rt.on && idle != RRGB_IDLE_DARK;
    /* CapsLock tint strength: the brightness the effect is drawn at (its
     * rendered value times the gain), 0 while it is not drawn */
    uint8_t tint_v = 0;

    rrgb_reactive_tick(rt.tick);
    anim_phase_q8 += rrgb_speed_increment(rt.speed);
    struct rgb_frame f = {
        .px = pixels, .n = RRGB_N, .tick = rt.tick, .phase = anim_phase_q8 >> 8,
        .hue = rt.hue, .sat = rt.sat,
        /* rgb.val, capped without a USB host (rgb.val_battery), a quarter of
         * that while idle "dim" */
        .val = rrgb_render_val(rt.val, val_battery, usb_host, idle),
        /* the same limits for the reactive flash: 255 at the defaults */
        .val_max = rrgb_render_val(255, val_battery, usb_host, idle),
        .speed = rt.speed,
        .xy = rrgb_led_xy, .last_press_tick = rt.last_press_tick,
        .ripples = rrgb_ripples(),
        .ripple_count = rrgb_ripple_pool_size(),
        .key_heat = rrgb_key_heat(),
    };
    /* BLE connecting / switching / pairing: the effect is off, the board
     * shows only the BLE status and the other overlays. Host direct mode is
     * not affected: an explicit host frame, not the normal effect. */
    effect_gain = rrgb_effect_gain_frame(effect_gain,
                                         rrgb_overlay_suppress_effect(rt.tick), effect_on);
    if (host_mode) {
        /* Host direct mode: the host's buffer replaces the effect layer. */
        for (uint16_t i = 0; i < RRGB_N; i++) { pixels[i] = host_px[i]; }
    } else if (effect_on && effect_gain > 0) {
        rrgb_effects[rt.effect].render(&f);
        tint_v = scale8(f.val, effect_gain);
        if (effect_gain < 255) {
            for (uint16_t i = 0; i < RRGB_N; i++) {
                pixels[i].r = scale8(pixels[i].r, effect_gain);
                pixels[i].g = scale8(pixels[i].g, effect_gain);
                pixels[i].b = scale8(pixels[i].b, effect_gain);
            }
        }
        /* ind.bat_low: Esc pulses red on top of the effect, only while the
         * effect is drawn */
        rrgb_overlay_bat_low_render(pixels, RRGB_N, rt.tick, usb_host);
    } else {
        /* RGB toggled off, idle off or effect suppressed: black base so
         * functional overlays still show. */
        for (uint16_t i = 0; i < RRGB_N; i++) { pixels[i] = (struct rrgb){0, 0, 0}; }
    }
    rrgb_overlay_render(pixels, RRGB_N, rt.tick, tint_v);
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
        /* Liveness beat — deliberately NOT rt.tick, which only advances inside
         * render_once() and so freezes whenever the strip is legitimately dark
         * (idle off, RGB off). This counter advances once per iteration, so a
         * caller can tell "thread is gone" from "thread is fine, nothing to
         * draw" — the two states the dark-strip bug made indistinguishable. */
        loop_beat++;

        /* Host-mode watchdog. Host mode is normally released by an explicit
         * clear, so a host that dies without sending one strands the board on
         * its last frame indefinitely; idle off cannot rescue it (host mode
         * overrides it by design) and on battery nothing else will.
         * Undocking mid-animation is the case that bites: the link dies before
         * the host can retract the frame, and afterwards there is no host left
         * to send anything at all. Host animations refresh continuously, so a
         * silence this long means the host is genuinely gone. */
#if defined(CONFIG_RGB_MGMT) && CONFIG_RGB_MGMT_HOST_TIMEOUT_S > 0
        /* Both sides of the compare must stay SIGNED. Zephyr's MSEC_PER_SEC is
         * 1000U, and an unsigned right-hand side would drag the int32_t delta
         * unsigned with it under the usual arithmetic conversions — turning
         * every negative delta into a huge positive and expiring host mode the
         * instant one appeared. That is reachable: the clock is sampled before
         * host_last_ms is read, so a frame landing between the two reads makes
         * the stamp newer than the sample. Hence the literal 1000, not
         * MSEC_PER_SEC.
         *
         * Known benign race, left unlocked deliberately: the test and the
         * `host_mode = false` write are not atomic, so a frame arriving in that
         * window is dropped and the board shows effects for one frame. The next
         * frame re-enters host mode (<=0.5 s even for the slowest host
         * animation), and it needs a host to resume at the exact instant a 30 s
         * silence expires. Locking the render path at 50 FPS costs more than
         * the glitch. */
        if (host_mode &&
            (int32_t)(k_uptime_get_32() - host_last_ms) >
                    (int32_t)CONFIG_RGB_MGMT_HOST_TIMEOUT_S * 1000) {
            host_mode = false;
            LOG_WRN("host mode expired after %d s of silence; back to effects",
                    CONFIG_RGB_MGMT_HOST_TIMEOUT_S);
        }
#endif

        /* Deadline-based pacing: render_once sleeps through the ~2.66 ms DMA
         * transfer (End-IRQ), so sleep only the remainder of the frame period
         * to hold a steady RRGB_FPS regardless of render duration. */
        int64_t deadline = k_uptime_get() + RRGB_PERIOD_MS;

        /* Idle timer (rgb.idle_s / rgb.idle_mode). The writers (settings
         * path, key events) run above this thread, so they only come in
         * between two of these reads. The settings are read BEFORE
         * last_activity_ms: a change (timeout, then a new timestamp) is then
         * never seen as the new timeout with the old timestamp. The
         * timestamp is read BEFORE the clock, so a key stamped in between is
         * never newer than now. Either would read as idle and turn the
         * effect off for one frame. */
        uint16_t timeout_s = idle_s;
        uint8_t mode = idle_mode;
        uint32_t last = last_activity_ms;
        enum rrgb_idle_state idle = rrgb_idle_state(k_uptime_get_32(), last, timeout_s, mode);
        /* Render when the effect shows OR a functional overlay (caps / Fn-highlight
         * / battery gauge / BLE status) needs to show, so indicators work with RGB
         * off and while idle. Idle "off" turns only the effect off (the first
         * keypress restores it); host direct mode overrides it so a host
         * notification pulse still shows when the board is idle. */
        bool idle_off = idle == RRGB_IDLE_DARK && !host_mode;

        /* Black box: what we believe vs what the pin says (see above). */
        uint8_t rail = rrgb_diag_state(rail_on, idle_off, host_mode, rt.on);

        if ((rt.on && !idle_off) || host_mode || rrgb_overlay_active(rt.tick)) {
            if (!rail_on) {
                rrgb_strip_power(true);
                rail_on = true;
                k_msleep(RRGB_RAIL_SETTLE_MS);
            } else if (IS_ENABLED(CONFIG_LED_STRIP_B91_SPI_PC2_POWER) &&
                       (rail & (RRGB_RAIL_HIGH | RRGB_RAIL_OUT_EN |
                                RRGB_RAIL_GPIO_MODE)) !=
                           (RRGB_RAIL_HIGH | RRGB_RAIL_OUT_EN |
                            RRGB_RAIL_GPIO_MODE)) {
                /* We think the rail is up but the pin disagrees, so every frame
                 * we render lands on an unpowered strip and nothing lights —
                 * exactly the dark-strip episode this instrumentation chases.
                 * Any of the three can do it: the level dropped, the output
                 * driver was disabled, or the pin left GPIO mode. The diag
                 * event above has already recorded which; re-assert the rail
                 * (rrgb_strip_power replays all three) so the board recovers
                 * on its own. */
                LOG_WRN("LED rail diverged while believed on (state 0x%02x); "
                        "re-asserting", rail);
#if IS_ENABLED(CONFIG_USB_DC_B91)
                /* Durability: the ring wraps in ~a day of keepalives, and a
                 * cold boot wipes it, so get this divergence into NVS before
                 * the self-heal erases the only symptom. The diag event was
                 * recorded earlier this frame, so the snapshot contains it. */
                b91_usb_diag_persist_async();
#endif
                rrgb_strip_power(true);
                k_msleep(RRGB_RAIL_SETTLE_MS);
            }
            render_once(idle);
            was_lit = true;
            dark_ticks = 0;
        } else {
            if (was_lit) {
                clear_strip();  /* clear once when nothing needs to show —
                                 * the black frame goes out while the rail is
                                 * still up; the rail is cut only after the
                                 * hold-off below */
                was_lit = false;
                /* render_once() stops stepping the gain: snap it to its
                 * target so the effect does not resume mid fade */
                effect_gain = rrgb_effect_gain_frame(
                        effect_gain, rrgb_overlay_suppress_effect(rt.tick), false);
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

__weak void rrgb_state_changed_hook(void) {}

__weak uint8_t rrgb_cycle_next_hook(uint8_t cur) {
    return (uint8_t)((cur + 1) % rrgb_effect_count);
}

/* Every change of the persisted state: save it and tell the settings. */
static void state_changed(void) {
    rrgb_request_save();
    rrgb_state_changed_hook();
}

void rrgb_toggle(void) {
    if (host_mode_escape()) { return; }
    rt.on = !rt.on;
    LOG_INF("rgb %s", rt.on ? "on" : "off");
    state_changed();
}
void rrgb_next_effect(void) {
    if (host_mode_escape()) { return; }
    uint8_t next = rrgb_cycle_next_hook(rt.effect);   /* rgb.cycle */

    rt.effect = (next < rrgb_effect_count) ? next : 0;
    LOG_INF("effect %u (%s)", rt.effect, rrgb_effects[rt.effect].name);
    state_changed();
}
void rrgb_hue_step(int dir) {
    if (host_mode_escape()) { return; }
    rt.hue += (dir >= 0) ? 8 : (uint8_t)-8; state_changed();
}
void rrgb_val_step(int dir) {
    if (host_mode_escape()) { return; }
    int v = rt.val + (dir >= 0 ? 16 : -16);
    rt.val = (v < 16) ? 16 : (v > 255 ? 255 : v);
    state_changed();
}
void rrgb_speed_step(int dir) {
    if (host_mode_escape()) { return; }
    int s = rt.speed + (dir >= 0 ? 8 : -8);
    rt.speed = (s < 1) ? 1 : (s > 255 ? 255 : s);
    state_changed();
}

uint32_t rrgb_param_get(uint8_t p) {
    switch (p) {
    case RRGB_P_ON: return rt.on;
    case RRGB_P_EFFECT: return rt.effect;
    case RRGB_P_HUE: return rt.hue;
    case RRGB_P_SAT: return rt.sat;
    case RRGB_P_VAL: return rt.val;
    case RRGB_P_SPEED: return rt.speed;
    default: return 0;
    }
}

void rrgb_param_set(uint8_t p, uint32_t v) {
    switch (p) {
    case RRGB_P_ON: rt.on = (v != 0); break;
    case RRGB_P_EFFECT:
        if (v >= rrgb_effect_count) { return; }
        rt.effect = v;
        break;
    case RRGB_P_HUE: rt.hue = (uint8_t)v; break;
    case RRGB_P_SAT: rt.sat = (uint8_t)v; break;
    case RRGB_P_VAL: rt.val = (v < 16) ? 16 : (v > 255 ? 255 : v); break;
    case RRGB_P_SPEED: rt.speed = (v < 1) ? 1 : (v > 255 ? 255 : v); break;
    default: return;
    }
    host_mode = false;   /* a setting from a host shows the effect again */
    rrgb_note_activity(); /* also on an idle board */
    state_changed();
}

void rrgb_apply_boot_effect(uint8_t effect) {
    if (effect < rrgb_effect_count) { rt.effect = effect; }
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
    host_touch();
}

void rrgb_host_fill(uint8_t r, uint8_t g, uint8_t b) {
    for (uint16_t i = 0; i < RRGB_N; i++) { host_px[i] = (struct rrgb){r, g, b}; }
    host_touch();
}

void rrgb_host_clear(void) {
    host_mode = false;
}

bool rrgb_host_active(void) {
    return host_mode;
}

/* Render-loop heartbeat: advances once per loop iteration whether or not a
 * frame is drawn, so a caller that samples it twice can tell a live loop from a
 * dead one — the distinction that cost this bug weeks, because every other
 * signal (keys, USB, SMP, the accepted host frame) stays healthy when only this
 * thread dies. Exposed over SMP by rgb_mgmt's info command, so two
 * `rainy75_rgb.py info` calls answer it outright. */
uint32_t rrgb_heartbeat(void) {
    return loop_beat;
}

/* Bytes still untouched on the render thread's stack (0 if unavailable).
 * The 1 KB that killed this thread was a guess, and so is the 2 KB replacing
 * it — this turns the next answer into a measurement, readable from the host
 * without a shell or a debugger. Needs CONFIG_INIT_STACKS +
 * CONFIG_THREAD_STACK_INFO to paint and walk the stack. */
uint32_t rrgb_stack_unused(void) {
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
    size_t unused = 0;

    if (k_thread_stack_space_get(&rrgb_thread, &unused) != 0) {
        return 0;
    }
    return (uint32_t)unused;
#else
    return 0;
#endif
}

void rrgb_set_val_battery(uint8_t cap) { val_battery = cap; }

void rrgb_set_idle_timeout(uint16_t seconds, uint8_t mode) {
    idle_s = seconds;
    idle_mode = mode;
}

void rrgb_note_activity(void) { last_activity_ms = k_uptime_get_32(); }

void rrgb_set_usb_host(bool host) {
    if (usb_host != host) {
        usb_host = host;
        LOG_INF("usb host %s", host ? "connected" : "gone");
    }
}

void rrgb_on_key(uint32_t position, bool pressed) {
    rrgb_note_activity();   /* every key position event restarts the idle timer */
    /* Fn-layer presses (BT slot, output, media, RGB controls) and presses
     * while the effect is suppressed for BLE (passkey digits) leave no
     * reactive trace: see rrgb_overlay_key_reactive(). */
    if (pressed && rrgb_overlay_key_reactive(position, rt.tick)) {
        rt.last_press_tick = rt.tick;
        rrgb_reactive_on_press(position, rt.tick);
    }
}

void rrgb_battery_gauge_show(void) { rrgb_overlay_battery_show(rt.tick); }

uint32_t rrgb_now(void) { return rt.tick; }

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
