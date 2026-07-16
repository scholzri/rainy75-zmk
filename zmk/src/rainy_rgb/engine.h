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
void rrgb_next_effect(void);
void rrgb_hue_step(int dir);
void rrgb_val_step(int dir);
void rrgb_speed_step(int dir);
void rrgb_on_key(uint32_t position, bool pressed);
void rrgb_battery_gauge_show(void);

void rrgb_get_persist(struct rrgb_persist *out);
void rrgb_set_persist(const struct rrgb_persist *in);
void rrgb_request_save(void); /* implemented in state.c */

/* --- Host-controlled direct pixel mode (rgb_mgmt mcumgr group) ---
 * While active, the host's pixel buffer replaces the effect layer; functional
 * overlays (CapsLock / Fn-highlight / battery) still render on top. Any
 * physical Fn+RGB control exits host mode (escape hatch). Not persisted —
 * a reboot or deep sleep returns to the normal effect. */
void rrgb_host_set_pixels(const uint8_t *quads, uint16_t count); /* [pos,r,g,b]* */
void rrgb_host_fill(uint8_t r, uint8_t g, uint8_t b);
void rrgb_host_clear(void);
bool rrgb_host_active(void);
#endif
