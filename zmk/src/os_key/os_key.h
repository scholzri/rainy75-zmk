/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * &os_key: the left GUI and Alt keys as the operating system wants them.
 * The decisions, pure and ZMK-free (host tested in tests/);
 * behaviors/behavior_os_key.c hands ZMK's key events to them and raises the
 * keycode they return, config/cfg_table.c pushes the settings kb.os and
 * kb.gui_lock with os_key_set_mode().
 *
 *  - The binding's parameter is LGUI or LALT, nothing else.
 *  - kb.os mac swaps them: the Win key position sends Option (LALT) and the
 *    Alt position Command (LGUI), the order of a Mac keyboard.
 *  - Then, with kb.gui_lock on, a key that would send GUI sends nothing.
 *  - The key sent on press is remembered per key position and released on
 *    release, whatever the settings say by then: changing them while a key
 *    is held cannot leave a modifier stuck. At most OS_KEY_HELD_MAX &os_key
 *    keys are held at once; a press beyond that, or a second press of a
 *    position that is still held, sends nothing (and so releases nothing).
 *
 * Threads: press and release run in ZMK's key event path (system work
 * queue), os_key_set_mode() in the settings threads (mcumgr work queue,
 * ZMK main thread at load). The mode is one byte, stored and loaded
 * atomically; the held keys belong to the key event path alone.
 */

#ifndef RAINY75_OS_KEY_H
#define RAINY75_OS_KEY_H

#include <stdbool.h>
#include <stdint.h>

/* LGUI and LALT as dt-bindings/zmk/keys.h encodes them (usage page 7 in bits
 * 16..23, the usage id below); behavior_os_key.c checks they match. */
#define OS_KEY_LGUI 0x000700E3u
#define OS_KEY_LALT 0x000700E2u

#define OS_KEY_HELD_MAX 4    /* &os_key keys held at the same time */
#define OS_KEY_COUNT_MAX 83  /* key positions of the board: the range of kb.os_keys */

/* kb.os, in the order of its names ("win", "mac"). */
enum os_key_os {
    OS_KEY_OS_WIN = 0,
    OS_KEY_OS_MAC = 1,
};

/* Back to the start: win, no GUI lock, nothing held. For tests only: never
 * call it while an &os_key key is held, its release would then send nothing
 * and leave the modifier down. */
void os_key_reset(void);
/* kb.os (enum os_key_os; any value but OS_KEY_OS_MAC counts as win) and
 * kb.gui_lock, used from the next press on. */
void os_key_set_mode(uint8_t os, bool gui_lock);

/* True for the parameters &os_key takes: OS_KEY_LGUI and OS_KEY_LALT. */
bool os_key_param_valid(uint32_t param);
/* The keycode to send for param under os (as for os_key_set_mode) and
 * gui_lock; 0 = nothing (GUI lock, or a parameter that is not valid). */
uint32_t os_key_map(uint32_t param, uint8_t os, bool gui_lock);

/* Key position pressed with the binding's parameter: the keycode to press
 * now, 0 = nothing. */
uint32_t os_key_press(uint32_t position, uint32_t param);
/* Key position released: the keycode its press sent, to release now; 0 =
 * nothing (the press sent nothing). */
uint32_t os_key_release(uint32_t position);

/* The behavior bound at (layer index, key position), as its device name;
 * NULL for none (also for a layer index that is not in use). */
typedef const char *(*os_key_dev_at_fn)(uint8_t layer, uint16_t pos, void *ctx);
/* kb.os_keys: how many of the key positions 0..n_pos-1 are bound to the
 * behavior named name on at least one of the layers 0..n_layers-1. */
uint16_t os_key_count_bound(os_key_dev_at_fn dev_at, void *ctx, uint8_t n_layers, uint16_t n_pos,
                            const char *name);

/* Zephyr glue (behaviors/behavior_os_key.c, built with CONFIG_RAINY75_OS_KEY):
 * os_key_count_bound() over ZMK's live keymap. */
uint16_t os_key_bound_count(void);

#endif /* RAINY75_OS_KEY_H */
