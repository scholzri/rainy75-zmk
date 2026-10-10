/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host test of the real settings table: cfg_table.c compiled against the
 * Zephyr stubs in stub/, its owners' hooks stubbed here. The effect names
 * come from the real effects.c (12 effects, 13 with the walker diagnostic,
 * CONFIG_RAINY_RGB_WALKER) or, with STUB_EFFECTS=n, from n stub names. The
 * list setting rgb.cycle may have at most CFG_LIST_MAX (16) names, so a 17th
 * effect makes cfg_init() refuse the whole table: the build with 17 stub
 * names checks that, and the builds with the real effects.c fail as soon as
 * the effect table grows that far. With CONFIG_RAINY75_OS_KEY the real
 * os_key.c receives kb.os and kb.gui_lock, and kb.os_keys reads the count
 * stubbed here (bound); without it kb.os_keys reads 0. run_host_tests.sh
 * builds every variant.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../cfg_registry.h"
#include "../cfg_table.h"
#include "os_key/os_key.h"
#include "rainy_rgb/ble_status.h"
#include "rainy_rgb/effects.h"
#include "rainy_rgb/engine.h"
#include "rainy_rgb/lighting.h"
#include "rainy_rgb/overlay.h"

static int failed;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

/* stub/zephyr/init.h: cfg_table.c's SYS_INIT function. */
extern int (*const test_sys_init)(void);

/* stub/zephyr/logging/log.h: every LOG_ERR / LOG_WRN, the last one kept. */
static int logs;
static char last_log[96];

void test_log(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    logs++;
}

#ifdef STUB_EFFECTS
static const char *const stub_names[] = {"e0", "e1", "e2",  "e3",  "e4",  "e5",  "e6",  "e7", "e8",
                                         "e9", "e10", "e11", "e12", "e13", "e14", "e15", "e16"};
_Static_assert(STUB_EFFECTS <= sizeof(stub_names) / sizeof(stub_names[0]), "too few stub names");
const uint16_t rrgb_effect_count = STUB_EFFECTS;

const char *rrgb_effect_name(uint8_t idx) { return idx < STUB_EFFECTS ? stub_names[idx] : NULL; }
#endif

/* The owners: the engine (proxied state record and lighting settings), the
 * overlay and ble_status, as cfg_table.c calls them. */
static uint32_t param[RRGB_P_SPEED + 1];
static int boot_effect = -1;
static uint8_t val_battery;
static uint16_t idle_s;
static uint8_t idle_mode = 0xFF;
static int activity;
static uint8_t caps_style = 0xFF;
static uint32_t caps_rgb;
static bool fn_highlight;
static uint8_t bat_low = 0xFF;
static bool passkey_guide;

uint32_t rrgb_param_get(uint8_t p) { return p <= RRGB_P_SPEED ? param[p] : 0; }

void rrgb_param_set(uint8_t p, uint32_t v) {
    if (p <= RRGB_P_SPEED) {
        param[p] = v;
    }
}

void rrgb_apply_boot_effect(uint8_t effect) { boot_effect = effect; }
void rrgb_set_val_battery(uint8_t cap) { val_battery = cap; }

void rrgb_set_idle_timeout(uint16_t seconds, uint8_t mode) {
    idle_s = seconds;
    idle_mode = mode;
}

void rrgb_note_activity(void) { activity++; }

void rrgb_overlay_set_caps_style(uint8_t style, uint32_t rgb) {
    caps_style = style;
    caps_rgb = rgb;
}

void rrgb_overlay_set_fn_highlight(bool on) { fn_highlight = on; }
void rrgb_overlay_set_bat_low(uint8_t pct) { bat_low = pct; }
void rrgb_ble_set_passkey_guide(bool on) { passkey_guide = on; }

#ifdef CONFIG_RAINY75_OS_KEY
/* The &os_key behavior's count over the live keymap (behavior_os_key.c). */
static uint16_t bound = 2;

uint16_t os_key_bound_count(void) { return bound; }
#endif

static struct cfg_value U(uint32_t u) {
    struct cfg_value v = {.u = u};
    return v;
}

/* Every setting of the table, by key, at its enum index. */
static const char *const keys[CFG_ID_COUNT] = {
    [CFG_RGB_ON] = "rgb.on",
    [CFG_RGB_EFFECT] = "rgb.effect",
    [CFG_RGB_HUE] = "rgb.hue",
    [CFG_RGB_SAT] = "rgb.sat",
    [CFG_RGB_VAL] = "rgb.val",
    [CFG_RGB_SPEED] = "rgb.speed",
    [CFG_RGB_BOOT_EFFECT] = "rgb.boot_effect",
    [CFG_RGB_CYCLE] = "rgb.cycle",
    [CFG_RGB_VAL_BATTERY] = "rgb.val_battery",
    [CFG_RGB_IDLE_S] = "rgb.idle_s",
    [CFG_RGB_IDLE_MODE] = "rgb.idle_mode",
    [CFG_IND_CAPS_STYLE] = "ind.caps_style",
    [CFG_IND_CAPS_COLOR] = "ind.caps_color",
    [CFG_IND_FN_HIGHLIGHT] = "ind.fn_highlight",
    [CFG_IND_PASSKEY_GUIDE] = "ind.passkey_guide",
    [CFG_IND_BAT_LOW] = "ind.bat_low",
    [CFG_KB_OS] = "kb.os",
    [CFG_KB_GUI_LOCK] = "kb.gui_lock",
    [CFG_KB_OS_KEYS] = "kb.os_keys",
};

static void test_accepted(void) {
    struct cfg_value v;

    CHECK(logs == 0); /* no "settings table refused" */
    CHECK(cfg_count() == CFG_ID_COUNT);
    for (uint8_t i = 0; i < CFG_ID_COUNT; i++) {
        CHECK(keys[i] != NULL && cfg_find(keys[i], strlen(keys[i])) == i);
    }
    CHECK(cfg_name_count(cfg_def(CFG_RGB_EFFECT)) == rrgb_effect_count);
    CHECK(cfg_name_count(cfg_def(CFG_RGB_BOOT_EFFECT)) == rrgb_effect_count + 1); /* + last */
    cfg_get(CFG_RGB_CYCLE, &v);
    CHECK(v.len == rrgb_effect_count); /* default: all effects in table order */
    CHECK(v.idx[0] == 0 && v.idx[v.len - 1] == rrgb_effect_count - 1);
}

/* The table's name lists are in the order of the owners' enums: a name maps
 * to the enum value its owner switches on, not merely to some index. */
static void test_name_order(void) {
    const struct cfg_def *caps = cfg_def(CFG_IND_CAPS_STYLE), *idle = cfg_def(CFG_RGB_IDLE_MODE);
    const struct cfg_def *os = cfg_def(CFG_KB_OS);

    CHECK(cfg_name_count(caps) == 3 && cfg_name_count(idle) == 2 && cfg_name_count(os) == 2);
    CHECK(cfg_name_find(caps, "key", 3) == RRGB_CAPS_KEY);
    CHECK(cfg_name_find(caps, "tint", 4) == RRGB_CAPS_TINT);
    CHECK(cfg_name_find(caps, "off", 3) == RRGB_CAPS_OFF);
    CHECK(cfg_name_find(idle, "off", 3) == RRGB_IDLE_MODE_OFF);
    CHECK(cfg_name_find(idle, "dim", 3) == RRGB_IDLE_MODE_DIM);
    CHECK(cfg_name_find(os, "win", 3) == OS_KEY_OS_WIN);
    CHECK(cfg_name_find(os, "mac", 3) == OS_KEY_OS_MAC);
}

/* cfg_init() pushed every default to its owner (cfg_def.notify = push). */
static void test_defaults_pushed(void) {
    CHECK(val_battery == 255);
    CHECK(idle_s == 0 && idle_mode == RRGB_IDLE_MODE_OFF);
    CHECK(caps_style == RRGB_CAPS_KEY && caps_rgb == 0xFFFFFF);
    CHECK(fn_highlight && passkey_guide);
    CHECK(bat_low == 0);
}

/* A set reaches its owner through push(), with the lighting idle restart. */
static void test_set_reaches_owner(void) {
    struct cfg_value v = U(15);
    int before = activity;

    CHECK(cfg_set(CFG_RGB_VAL_BATTERY, &v, NULL) == -EINVAL && val_battery == 255); /* 16..255 */
    v = U(100);
    CHECK(cfg_set(CFG_RGB_VAL_BATTERY, &v, NULL) == 0 && val_battery == 100);
    v = U(RRGB_CAPS_TINT);
    CHECK(cfg_set(CFG_IND_CAPS_STYLE, &v, NULL) == 0 && caps_style == RRGB_CAPS_TINT);
    CHECK(caps_rgb == 0xFFFFFF); /* the colour is pushed with the style */
    v = U(0);
    CHECK(cfg_set(CFG_IND_PASSKEY_GUIDE, &v, NULL) == 0 && !passkey_guide);
    CHECK(activity == before + 3);
    v = U(77);
    CHECK(cfg_set(CFG_RGB_HUE, &v, NULL) == 0 && param[RRGB_P_HUE] == 77); /* proxied */
    CHECK(cfg_reset(CFG_RGB_VAL_BATTERY) == 0 && val_battery == 255);
}

static void test_boot_effect(void) {
    struct cfg_value v = U(2); /* names: last, effect 0, effect 1, ... */

    CHECK(cfg_set(CFG_RGB_BOOT_EFFECT, &v, NULL) == 0);
    cfg_table_loaded();
    CHECK(boot_effect == 1);
}

/* kb.os and kb.gui_lock reach the &os_key behavior; kb.os_keys is
 * read-only, counted on every read. */
static void test_kb(void) {
    const struct cfg_def *os = cfg_def(CFG_KB_OS), *keys_def = cfg_def(CFG_KB_OS_KEYS);
    struct cfg_value v = U(OS_KEY_OS_MAC);
    int before = activity;

    CHECK(cfg_name_count(os) == 2);
    CHECK(strcmp(os->names(0), "win") == 0 && strcmp(os->names(1), "mac") == 0);
    CHECK(cfg_u(CFG_KB_OS) == OS_KEY_OS_WIN && cfg_u(CFG_KB_GUI_LOCK) == 0);
    CHECK(keys_def->type == CFG_UINT && keys_def->flags == (CFG_F_RO | CFG_F_PROXY));
    CHECK(keys_def->min == 0 && keys_def->max == 83);
#ifdef CONFIG_RAINY75_OS_KEY
    CHECK(cfg_u(CFG_KB_OS_KEYS) == 2);
    bound = 0;
    CHECK(cfg_u(CFG_KB_OS_KEYS) == 0); /* a keymap saved without &os_key */
    CHECK(os_key_press(74, OS_KEY_LGUI) == OS_KEY_LGUI && os_key_release(74) == OS_KEY_LGUI);
    CHECK(cfg_set(CFG_KB_OS, &v, NULL) == 0);
    CHECK(os_key_press(74, OS_KEY_LGUI) == OS_KEY_LALT && os_key_release(74) == OS_KEY_LALT);
    v = U(1);
    CHECK(cfg_set(CFG_KB_GUI_LOCK, &v, NULL) == 0);
    CHECK(os_key_press(75, OS_KEY_LALT) == 0); /* Command, locked */
    CHECK(cfg_reset(CFG_KB_OS) == 0 && cfg_reset(CFG_KB_GUI_LOCK) == 0);
    CHECK(os_key_press(74, OS_KEY_LGUI) == OS_KEY_LGUI && os_key_release(74) == OS_KEY_LGUI);
#else
    CHECK(cfg_u(CFG_KB_OS_KEYS) == 0); /* no &os_key behavior in this build */
#endif
    CHECK(activity == before); /* not lighting settings: no idle restart */
    v = U(5);
    CHECK(cfg_set(CFG_KB_OS_KEYS, &v, NULL) == -EACCES && cfg_reset(CFG_KB_OS_KEYS) == -EACCES);
    v = U(2);
    CHECK(cfg_set(CFG_KB_OS, &v, NULL) == -EINVAL); /* win and mac only */
}

/* Only the build with more stub names than a list setting may have expects
 * the table to be refused; the real effect table must always be accepted. */
#if defined(STUB_EFFECTS) && STUB_EFFECTS > CFG_LIST_MAX
#define EXPECT_REFUSED 1
#else
#define EXPECT_REFUSED 0
#endif

int main(void) {
    CHECK(test_sys_init() == 0);
    if (EXPECT_REFUSED) {
        /* no table at all (there was none before), and the error logged */
        CHECK(cfg_count() == 0 && cfg_def(0) == NULL);
        CHECK(logs == 1 && strcmp(last_log, "settings table refused: -22") == 0);
    } else if (cfg_count() != CFG_ID_COUNT) {
        printf("FAIL the real table was refused (%s): %u effects, a list setting takes at most "
               "%d names\n",
               last_log, (unsigned)rrgb_effect_count, CFG_LIST_MAX);
        return 1;
    } else {
        test_accepted();
        test_name_order();
        test_defaults_pushed();
        test_set_reaches_owner();
        test_boot_effect();
        test_kb();
    }
    if (failed) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("cfg_table (%u effects%s): all tests passed\n", (unsigned)rrgb_effect_count,
           EXPECT_REFUSED ? ", refused" : "");
    return 0;
}
