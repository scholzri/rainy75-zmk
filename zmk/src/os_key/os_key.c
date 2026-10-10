/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * OS key decisions, see os_key.h. Pure: no Zephyr, no ZMK.
 */

#include "os_key.h"

#include <string.h>

#define MODE_MAC 0x01u
#define MODE_GUI_LOCK 0x02u

/* kb.os and kb.gui_lock as one byte: written by the settings threads. */
static uint8_t mode;

/* The keys held now: position and the keycode its press sent (0 = free). */
static struct {
    uint32_t position;
    uint32_t code;
} held[OS_KEY_HELD_MAX];

void os_key_reset(void) {
    __atomic_store_n(&mode, 0, __ATOMIC_RELAXED);
    memset(held, 0, sizeof(held));
}

void os_key_set_mode(uint8_t os, bool gui_lock) {
    uint8_t m = (os == OS_KEY_OS_MAC ? MODE_MAC : 0) | (gui_lock ? MODE_GUI_LOCK : 0);

    __atomic_store_n(&mode, m, __ATOMIC_RELAXED);
}

bool os_key_param_valid(uint32_t param) { return param == OS_KEY_LGUI || param == OS_KEY_LALT; }

uint32_t os_key_map(uint32_t param, uint8_t os, bool gui_lock) {
    uint32_t code;

    if (!os_key_param_valid(param)) {
        return 0;
    }
    code = param;
    if (os == OS_KEY_OS_MAC) {
        code = (param == OS_KEY_LGUI) ? OS_KEY_LALT : OS_KEY_LGUI;
    }
    return (gui_lock && code == OS_KEY_LGUI) ? 0 : code;
}

/* The slot holding position, or -1. */
static int find(uint32_t position) {
    for (int i = 0; i < OS_KEY_HELD_MAX; i++) {
        if (held[i].code != 0 && held[i].position == position) {
            return i;
        }
    }
    return -1;
}

/* A free slot, or -1. */
static int find_free(void) {
    for (int i = 0; i < OS_KEY_HELD_MAX; i++) {
        if (held[i].code == 0) {
            return i;
        }
    }
    return -1;
}

uint32_t os_key_press(uint32_t position, uint32_t param) {
    uint8_t m = __atomic_load_n(&mode, __ATOMIC_RELAXED);
    uint32_t code;
    int slot;

    if (find(position) >= 0) {
        return 0; /* still held: its release releases the first press */
    }
    code = os_key_map(param, (m & MODE_MAC) ? OS_KEY_OS_MAC : OS_KEY_OS_WIN,
                      (m & MODE_GUI_LOCK) != 0);
    if (code == 0) {
        return 0;
    }
    slot = find_free();
    if (slot < 0) {
        return 0; /* OS_KEY_HELD_MAX keys held already */
    }
    held[slot].position = position;
    held[slot].code = code;
    return code;
}

uint32_t os_key_release(uint32_t position) {
    int i = find(position);
    uint32_t code;

    if (i < 0) {
        return 0;
    }
    code = held[i].code;
    held[i].code = 0;
    return code;
}

uint16_t os_key_count_bound(os_key_dev_at_fn dev_at, void *ctx, uint8_t n_layers, uint16_t n_pos,
                            const char *name) {
    uint16_t n = 0;

    for (uint16_t p = 0; p < n_pos; p++) {
        for (uint8_t l = 0; l < n_layers; l++) {
            const char *dev = dev_at(l, p, ctx);

            if (dev != NULL && strcmp(dev, name) == 0) {
                n++;
                break; /* a position counts once, however many layers bind it */
            }
        }
    }
    return n;
}
