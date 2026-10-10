/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * The settings table. rgb.on/effect/hue/sat/val/speed are proxied: they
 * live in the rainy_rgb state record (rainy_rgb/state, saved by the
 * engine), their defaults here must equal the engine's (engine.c, rt) for
 * a reset to restore what a fresh keyboard shows. rgb.boot_effect is stored
 * by cfg_store.c; "last" keeps the effect the user last chose.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "cfg_registry.h"
#include "cfg_table.h"
#include "rainy_rgb/effects.h"
#include "rainy_rgb/engine.h"

LOG_MODULE_REGISTER(cfg_table, CONFIG_LOG_DEFAULT_LEVEL);

static const char *boot_effect_name(uint8_t idx) {
    return idx == 0 ? "last" : rrgb_effect_name(idx - 1);
}

#define RGB_PROXY(k, t, lo, hi, d, p)                                                              \
    {                                                                                              \
        .key = k, .type = t, .flags = CFG_F_PROXY, .min = lo, .max = hi, .def = d,                 \
        .get = rrgb_param_get, .set = rrgb_param_set, .arg = p,                                    \
    }

static const struct cfg_def table[CFG_ID_COUNT] = {
    [CFG_RGB_ON] = RGB_PROXY("rgb.on", CFG_BOOL, 0, 1, 1, RRGB_P_ON),
    [CFG_RGB_EFFECT] = {.key = "rgb.effect",
                        .type = CFG_ENUM,
                        .flags = CFG_F_PROXY,
                        .def = 0,
                        .names = rrgb_effect_name,
                        .get = rrgb_param_get,
                        .set = rrgb_param_set,
                        .arg = RRGB_P_EFFECT},
    [CFG_RGB_HUE] = RGB_PROXY("rgb.hue", CFG_UINT, 0, 255, 0, RRGB_P_HUE),
    [CFG_RGB_SAT] = RGB_PROXY("rgb.sat", CFG_UINT, 0, 255, 255, RRGB_P_SAT),
    [CFG_RGB_VAL] = RGB_PROXY("rgb.val", CFG_UINT, 16, 255, 200, RRGB_P_VAL),
    [CFG_RGB_SPEED] = RGB_PROXY("rgb.speed", CFG_UINT, 1, 255, 32, RRGB_P_SPEED),
    [CFG_RGB_BOOT_EFFECT] = {.key = "rgb.boot_effect",
                             .type = CFG_ENUM,
                             .def = 0,
                             .names = boot_effect_name},
};

static uint32_t vals[CFG_ID_COUNT];

static int cfg_table_init(void) {
    int rc = cfg_init(table, CFG_ID_COUNT, vals, NULL, 0);

    if (rc != 0) {
        LOG_ERR("settings table refused: %d", rc);
    }
    return 0;
}

/* Before settings_load() (ZMK main thread), after the kernel is up. */
SYS_INIT(cfg_table_init, POST_KERNEL, 0);

void cfg_table_loaded(void) {
    uint32_t boot = cfg_u(CFG_RGB_BOOT_EFFECT);

    if (boot > 0) {
        rrgb_apply_boot_effect(boot - 1);
        cfg_note_change();
    }
}

/* rainy_rgb state changed (Fn keys, a host set): hosts notice through rev. */
void rrgb_state_changed_hook(void) { cfg_note_change(); }
