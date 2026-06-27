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
#endif
