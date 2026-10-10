/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * The settings table. rgb.on/effect/hue/sat/val/speed are proxied: they
 * live in the rainy_rgb state record (rainy_rgb/state, saved by the
 * engine), their defaults here must equal the engine's (engine.c, rt) for
 * a reset to restore what a fresh keyboard shows. The other settings are
 * stored by cfg_store.c: rgb.boot_effect is read once after the load
 * (cfg_table_loaded), rgb.cycle when Fn+Enter is pressed
 * (rrgb_cycle_next_hook), and the rest is pushed to its owner by push(),
 * the per-owner change callback (cfg_def.notify), after init and after
 * every set, reset and load: the lighting and indicator settings to the
 * rainy_rgb engine, overlay and ble_status, kb.os and kb.gui_lock to the
 * &os_key behavior (os_key/os_key.h). The owners start with the same
 * defaults, so a build without CONFIG_RAINY75_CONFIG behaves like a fresh
 * keyboard. kb.os_keys is read-only: it counts the key positions bound to
 * &os_key in the live keymap on every read and is not stored. kb.sleep_min
 * and kb.sleep_on_usb are stored and read by the sleep trigger
 * (sleep/sleep_adapter.c) at every check, once a second; builds without the
 * trigger (test images, ZMK's own sleep) keep them without effect.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include "cfg_registry.h"
#include "cfg_table.h"
#include "os_key/os_key.h"
#include "rainy_rgb/ble_status.h"
#include "rainy_rgb/effects.h"
#include "rainy_rgb/engine.h"
#include "rainy_rgb/lighting.h"
#include "rainy_rgb/overlay.h"
#include "sleep/sleep_policy.h"

LOG_MODULE_REGISTER(cfg_table, CONFIG_LOG_DEFAULT_LEVEL);

BUILD_ASSERT(CFG_ID_COUNT <= CFG_MAX, "more settings than the registry holds");

static const char *boot_effect_name(uint8_t idx) {
    return idx == 0 ? "last" : rrgb_effect_name(idx - 1);
}

/* In the order of enum rrgb_idle_mode (lighting.h). */
static const char *idle_mode_name(uint8_t idx) {
    static const char *const names[] = {"off", "dim"};

    return idx < ARRAY_SIZE(names) ? names[idx] : NULL;
}

/* In the order of enum rrgb_caps_style (overlay.h). */
static const char *caps_style_name(uint8_t idx) {
    static const char *const names[] = {"key", "tint", "off"};

    return idx < ARRAY_SIZE(names) ? names[idx] : NULL;
}

/* In the order of enum os_key_os (os_key/os_key.h). */
static const char *os_name(uint8_t idx) {
    static const char *const names[] = {"win", "mac"};

    return idx < ARRAY_SIZE(names) ? names[idx] : NULL;
}

/* kb.os_keys: counted on every read, so it needs no RAM and follows every
 * keymap change; 0 in a build without the &os_key behavior. */
static uint32_t os_keys_get(uint8_t arg) {
    ARG_UNUSED(arg);
#ifdef CONFIG_RAINY75_OS_KEY
    return os_key_bound_count();
#else
    return 0;
#endif
}

static void push(uint8_t i);

#define RGB_PROXY(k, t, lo, hi, d, p)                                                              \
    {                                                                                              \
        .key = k, .type = t, .flags = CFG_F_PROXY, .min = lo, .max = hi, .def = d,                 \
        .get = rrgb_param_get, .set = rrgb_param_set, .arg = p,                                    \
    }

/* A setting stored here and pushed to its owner. */
#define OWNED(k, t, lo, hi, d, n)                                                                  \
    {                                                                                              \
        .key = k, .type = t, .min = lo, .max = hi, .def = d, .names = n, .notify = push,           \
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
    [CFG_RGB_CYCLE] = {.key = "rgb.cycle", .type = CFG_LIST, .names = rrgb_effect_name},
    [CFG_RGB_VAL_BATTERY] = OWNED("rgb.val_battery", CFG_UINT, 16, 255, 255, NULL),
    [CFG_RGB_IDLE_S] = OWNED("rgb.idle_s", CFG_UINT, 0, 3600, 0, NULL),
    [CFG_RGB_IDLE_MODE] =
        OWNED("rgb.idle_mode", CFG_ENUM, 0, 0, RRGB_IDLE_MODE_OFF, idle_mode_name),
    [CFG_IND_CAPS_STYLE] = OWNED("ind.caps_style", CFG_ENUM, 0, 0, RRGB_CAPS_KEY, caps_style_name),
    [CFG_IND_CAPS_COLOR] = OWNED("ind.caps_color", CFG_COLOR, 0, 0, 0xFFFFFF, NULL),
    [CFG_IND_FN_HIGHLIGHT] = OWNED("ind.fn_highlight", CFG_BOOL, 0, 1, 1, NULL),
    [CFG_IND_PASSKEY_GUIDE] = OWNED("ind.passkey_guide", CFG_BOOL, 0, 1, 1, NULL),
    [CFG_IND_BAT_LOW] = OWNED("ind.bat_low", CFG_UINT, 0, 50, 0, NULL),
    [CFG_KB_OS] = OWNED("kb.os", CFG_ENUM, 0, 0, OS_KEY_OS_WIN, os_name),
    [CFG_KB_GUI_LOCK] = OWNED("kb.gui_lock", CFG_BOOL, 0, 1, 0, NULL),
    [CFG_KB_OS_KEYS] = {.key = "kb.os_keys",
                        .type = CFG_UINT,
                        .flags = CFG_F_RO | CFG_F_PROXY,
                        .max = OS_KEY_COUNT_MAX,
                        .get = os_keys_get},
    /* Read by the sleep trigger at every check: no notify. */
    [CFG_KB_SLEEP_MIN] = {.key = "kb.sleep_min",
                          .type = CFG_UINT,
                          .max = SLEEP_POLICY_MIN_MAX,
                          .def = SLEEP_POLICY_MIN_DEFAULT},
    [CFG_KB_SLEEP_ON_USB] = {.key = "kb.sleep_on_usb",
                             .type = CFG_BOOL,
                             .def = SLEEP_POLICY_ON_USB_DEFAULT},
};

static uint32_t vals[CFG_ID_COUNT];
static struct cfg_list_slot lists[1]; /* rgb.cycle */
static struct k_spinlock list_lock;

static uint32_t list_lock_take(void) { return (uint32_t)k_spin_lock(&list_lock).key; }

static void list_lock_give(uint32_t key) {
    k_spinlock_key_t k = {.key = (int)key};

    k_spin_unlock(&list_lock, k);
}

/* The per-owner change callback: hand the new value to its owner. */
static void push(uint8_t i) {
    switch (i) {
    case CFG_RGB_VAL_BATTERY:
        rrgb_set_val_battery((uint8_t)cfg_u(i));
        break;
    case CFG_RGB_IDLE_S:
    case CFG_RGB_IDLE_MODE:
        rrgb_set_idle_timeout((uint16_t)cfg_u(CFG_RGB_IDLE_S), (uint8_t)cfg_u(CFG_RGB_IDLE_MODE));
        break;
    case CFG_IND_CAPS_STYLE:
    case CFG_IND_CAPS_COLOR:
        rrgb_overlay_set_caps_style((uint8_t)cfg_u(CFG_IND_CAPS_STYLE), cfg_u(CFG_IND_CAPS_COLOR));
        break;
    case CFG_IND_FN_HIGHLIGHT:
        rrgb_overlay_set_fn_highlight(cfg_u(i) != 0);
        break;
    case CFG_IND_PASSKEY_GUIDE:
        rrgb_ble_set_passkey_guide(cfg_u(i) != 0);
        break;
    case CFG_IND_BAT_LOW:
        rrgb_overlay_set_bat_low((uint8_t)cfg_u(i));
        break;
    case CFG_KB_OS:
    case CFG_KB_GUI_LOCK:
#ifdef CONFIG_RAINY75_OS_KEY
        os_key_set_mode((uint8_t)cfg_u(CFG_KB_OS), cfg_u(CFG_KB_GUI_LOCK) != 0);
#endif
        return; /* not a lighting setting: the idle timer keeps running */
    default:
        return;
    }
    rrgb_note_activity(); /* a changed setting shows also on an idle board */
}

static int cfg_table_init(void) {
    int rc;

    cfg_set_lock(list_lock_take, list_lock_give);
    rc = cfg_init(table, CFG_ID_COUNT, vals, lists, ARRAY_SIZE(lists));
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

/* Fn+Enter: the next effect of rgb.cycle (engine.c has the weak default). */
uint8_t rrgb_cycle_next_hook(uint8_t cur) {
    int next = cfg_cycle_next(CFG_RGB_CYCLE, cur);

    return next >= 0 ? (uint8_t)next : (uint8_t)((cur + 1) % rrgb_effect_count);
}

/* rainy_rgb state changed (Fn keys, a host set): hosts notice through rev. */
void rrgb_state_changed_hook(void) { cfg_note_change(); }
