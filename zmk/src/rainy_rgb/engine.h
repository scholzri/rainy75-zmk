#ifndef RAINY_RGB_ENGINE_H
#define RAINY_RGB_ENGINE_H
#include <stdint.h>
#include <stdbool.h>

struct rrgb_persist {
    uint8_t version;
    bool on;
    uint8_t effect, hue, sat, val, speed;
};

void rrgb_engine_init(void);
void rrgb_toggle(void);
void rrgb_next_effect(void);   /* Fn+Enter: the effect rrgb_cycle_next_hook() picks */
void rrgb_hue_step(int dir);
void rrgb_val_step(int dir);
void rrgb_speed_step(int dir);
void rrgb_on_key(uint32_t position, bool pressed);   /* every key position event */
void rrgb_battery_gauge_show(void);
/* Current render frame (the overlay clock). It only advances while frames
 * are drawn, so an event stamped with it shows from its start even when the
 * strip was dark. */
uint32_t rrgb_now(void);

void rrgb_get_persist(struct rrgb_persist *out);
void rrgb_set_persist(const struct rrgb_persist *in);
void rrgb_request_save(void); /* implemented in state.c */

/* --- Single parameters (runtime settings, config/cfg_table.c) ---
 * The persisted state as single values. A set clamps like a load (except the
 * effect: out of range is ignored, a load resets it to 0), leaves host mode
 * (the new value should be visible), restarts the idle timer and saves like a
 * Fn key. */
enum rrgb_param {
    RRGB_P_ON,
    RRGB_P_EFFECT,
    RRGB_P_HUE,
    RRGB_P_SAT,
    RRGB_P_VAL,
    RRGB_P_SPEED,
};
uint32_t rrgb_param_get(uint8_t p);
void rrgb_param_set(uint8_t p, uint32_t v);
/* Effect after boot (setting rgb.boot_effect). It is not saved by itself, but
 * the next save of the state record (any later Fn change or settings set)
 * stores the effect then showing: "last" means the effect showing at the last
 * save. */
void rrgb_apply_boot_effect(uint8_t effect);
/* Called after every state change (Fn keys, rrgb_param_set). The default
 * does nothing; config/cfg_table.c overrides it to bump the settings change
 * counter. */
void rrgb_state_changed_hook(void);

/* --- Lighting settings (config/cfg_table.c pushes them from the settings
 * registry; the defaults named here also hold without CONFIG_RAINY75_CONFIG).
 * rgb.val_battery: brightness cap of the effect while no USB host is
 * connected, 16..255; 255 = no cap (default). */
void rrgb_set_val_battery(uint8_t cap);
/* rgb.idle_s / rgb.idle_mode: seconds without a key position event (0 =
 * never, default) after which the effect turns off or dims to a quarter
 * (enum rrgb_idle_mode in lighting.h); the overlays keep showing. */
void rrgb_set_idle_timeout(uint16_t seconds, uint8_t mode);
/* Restart the idle timer: every key position event (rrgb_on_key) and every
 * change of a lighting or indicator setting (the six state-record settings
 * through rrgb_param_set(); rgb.val_battery, rgb.idle_* and ind.* through
 * config/cfg_table.c), so a change made from a host shows on an idle board.
 * rgb.cycle and rgb.boot_effect do not restart it. */
void rrgb_note_activity(void);
/* A USB host has configured the keyboard, also while it suspends the bus
 * (zmk_adapter.c: zmk_usb_is_hid_ready()). Default: no host. */
void rrgb_set_usb_host(bool host);
/* Fn+Enter: the effect after cur (setting rgb.cycle). The default is the next
 * effect in table order; config/cfg_table.c overrides it. A result past the
 * effect table selects effect 0. */
uint8_t rrgb_cycle_next_hook(uint8_t cur);

/* --- Host-controlled direct pixel mode (rgb_mgmt mcumgr group) ---
 * While active, the host's pixel buffer replaces the effect layer; functional
 * overlays (CapsLock / Fn-highlight / battery) still render on top. Any
 * physical Fn+RGB control exits host mode (escape hatch). Not persisted: a
 * reboot or deep sleep returns to the normal effect. */
void rrgb_host_set_pixels(const uint8_t *quads, uint16_t count); /* [pos,r,g,b]* */
void rrgb_host_fill(uint8_t r, uint8_t g, uint8_t b);
void rrgb_host_clear(void);
bool rrgb_host_active(void);

/* Render-loop heartbeat: advances once per loop iteration, drawn or not, so a
 * stalled value means the thread is gone rather than merely idle (see engine.c). */
uint32_t rrgb_heartbeat(void);

/* Untouched bytes left on the render thread's stack; 0 if unmeasurable. */
uint32_t rrgb_stack_unused(void);
#endif
