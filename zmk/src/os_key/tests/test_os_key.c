/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the OS key decisions (os_key.c), one test per rule of
 * os_key.h.
 */

#include "../os_key.h"
#include <stdio.h>

static int failed;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

#define GUI OS_KEY_LGUI
#define ALT OS_KEY_LALT
#define WIN OS_KEY_OS_WIN
#define MAC OS_KEY_OS_MAC
#define POS_GUI 74 /* the default keymap's left GUI and left Alt positions */
#define POS_ALT 75

static void test_params(void) {
    CHECK(os_key_param_valid(GUI) && os_key_param_valid(ALT));
    CHECK(!os_key_param_valid(0x000700E0u)); /* LCTRL */
    CHECK(!os_key_param_valid(0x000700E7u)); /* RGUI: the board has none */
    CHECK(!os_key_param_valid(0));
}

/* The whole truth table: Mac swaps, then the GUI lock drops GUI. */
static void test_map(void) {
    CHECK(os_key_map(GUI, WIN, false) == GUI);
    CHECK(os_key_map(ALT, WIN, false) == ALT);
    CHECK(os_key_map(GUI, MAC, false) == ALT); /* Win key position: Option */
    CHECK(os_key_map(ALT, MAC, false) == GUI); /* Alt position: Command */
    CHECK(os_key_map(GUI, WIN, true) == 0);
    CHECK(os_key_map(ALT, WIN, true) == ALT);
    CHECK(os_key_map(GUI, MAC, true) == ALT);
    CHECK(os_key_map(ALT, MAC, true) == 0); /* it would send Command */
    CHECK(os_key_map(0x000700E0u, WIN, false) == 0);
    CHECK(os_key_map(GUI, 2, false) == GUI); /* an os past mac counts as win */
}

static void test_press_release(void) {
    os_key_reset();
    CHECK(os_key_press(POS_GUI, GUI) == GUI);
    CHECK(os_key_release(POS_GUI) == GUI);
    CHECK(os_key_release(POS_GUI) == 0); /* released already */
    CHECK(os_key_release(POS_ALT) == 0); /* never pressed */
}

static void test_mac(void) {
    os_key_reset();
    os_key_set_mode(MAC, false);
    CHECK(os_key_press(POS_GUI, GUI) == ALT);
    CHECK(os_key_press(POS_ALT, ALT) == GUI); /* both held */
    CHECK(os_key_release(POS_ALT) == GUI);
    CHECK(os_key_release(POS_GUI) == ALT);
}

static void test_gui_lock(void) {
    os_key_reset();
    os_key_set_mode(WIN, true);
    CHECK(os_key_press(POS_GUI, GUI) == 0);
    CHECK(os_key_release(POS_GUI) == 0);
    CHECK(os_key_press(POS_ALT, ALT) == ALT);
    CHECK(os_key_release(POS_ALT) == ALT);
    os_key_set_mode(MAC, true);
    CHECK(os_key_press(POS_ALT, ALT) == 0); /* Command locked */
    CHECK(os_key_press(POS_GUI, GUI) == ALT);
    CHECK(os_key_release(POS_ALT) == 0);
    CHECK(os_key_release(POS_GUI) == ALT);
}

/* The release sends what the press sent, whatever the settings are now. */
static void test_setting_changed_while_held(void) {
    os_key_reset();
    CHECK(os_key_press(POS_GUI, GUI) == GUI);
    os_key_set_mode(MAC, false);
    CHECK(os_key_release(POS_GUI) == GUI); /* not Option */
    os_key_set_mode(WIN, false);
    CHECK(os_key_press(POS_GUI, GUI) == GUI);
    os_key_set_mode(WIN, true);
    CHECK(os_key_release(POS_GUI) == GUI); /* the lock does not hold GUI down */
    CHECK(os_key_press(POS_GUI, GUI) == 0);
    os_key_set_mode(WIN, false);
    CHECK(os_key_release(POS_GUI) == 0); /* the press sent nothing */
}

/* Two positions sending the same modifier: one release each (ZMK counts
 * modifier presses, so both are needed). */
static void test_same_key_twice(void) {
    os_key_reset();
    CHECK(os_key_press(POS_GUI, GUI) == GUI);
    CHECK(os_key_press(10, GUI) == GUI);
    CHECK(os_key_release(POS_GUI) == GUI);
    CHECK(os_key_release(10) == GUI);
}

static void test_position_pressed_again(void) {
    os_key_reset();
    CHECK(os_key_press(POS_GUI, GUI) == GUI);
    CHECK(os_key_press(POS_GUI, GUI) == 0); /* still held: nothing more */
    CHECK(os_key_release(POS_GUI) == GUI);
    CHECK(os_key_release(POS_GUI) == 0);
}

static void test_held_limit(void) {
    os_key_reset();
    for (uint32_t p = 0; p < OS_KEY_HELD_MAX; p++) {
        CHECK(os_key_press(p, ALT) == ALT);
    }
    CHECK(os_key_press(100, GUI) == 0); /* no room: nothing sent */
    CHECK(os_key_release(100) == 0);
    CHECK(os_key_release(1) == ALT);
    CHECK(os_key_press(100, GUI) == GUI); /* room again */
    CHECK(os_key_release(100) == GUI);
    CHECK(os_key_release(0) == ALT && os_key_release(2) == ALT && os_key_release(3) == ALT);
}

/* A press that found the table full sent nothing and is not remembered: when
 * another key's release frees a slot, the overflowed key's own release must
 * still send nothing (it would release a modifier it never pressed, for ZMK
 * the same one another held key sent). */
static void test_overflow_then_free(void) {
    os_key_reset();
    for (uint32_t p = 0; p < OS_KEY_HELD_MAX; p++) {
        CHECK(os_key_press(p, ALT) == ALT);
    }
    CHECK(os_key_press(100, GUI) == 0);    /* overflow: nothing sent */
    CHECK(os_key_release(1) == ALT);       /* another key frees a slot */
    CHECK(os_key_release(100) == 0);       /* X's release: nothing, X was never held */
    CHECK(os_key_release(100) == 0);       /* and still nothing */
    CHECK(os_key_press(100, GUI) == GUI);  /* a fresh press of X works, in the freed slot */
    CHECK(os_key_release(100) == GUI);
    CHECK(os_key_release(0) == ALT && os_key_release(2) == ALT && os_key_release(3) == ALT);
}

static void test_invalid_param(void) {
    os_key_reset();
    CHECK(os_key_press(POS_GUI, 0x000700E0u) == 0);
    CHECK(os_key_release(POS_GUI) == 0);
}

static void test_reset(void) {
    os_key_reset(); /* not the state the previous test left */
    os_key_set_mode(MAC, true);
    CHECK(os_key_press(POS_ALT, GUI) == ALT);
    os_key_reset();
    CHECK(os_key_release(POS_ALT) == 0); /* forgotten */
    CHECK(os_key_press(POS_GUI, GUI) == GUI); /* win, no lock */
    CHECK(os_key_release(POS_GUI) == GUI);
}

/* A keymap of 2 layers and 6 positions, passed as ctx. */
static const char *const fake_keymap[2][6] = {
    {"key_press", "os_key", "os_key", "key_press", NULL, "key_press"},
    {"transparent", "transparent", "os_key", "transparent", "transparent", "os_key"},
};

static const char *fake_dev_at(uint8_t layer, uint16_t pos, void *ctx) {
    const char *const(*km)[6] = ctx;

    return layer < 2 && pos < 6 ? km[layer][pos] : NULL; /* layer 2: not in use */
}

static void test_count_bound(void) {
    char name[] = "os_key"; /* a copy: names compare as strings, not pointers */
    void *km = (void *)fake_keymap;

    CHECK(os_key_count_bound(fake_dev_at, km, 3, 6, name) == 3); /* positions 1, 2 and 5 */
    CHECK(os_key_count_bound(fake_dev_at, km, 1, 6, name) == 2); /* layer 0 only */
    CHECK(os_key_count_bound(fake_dev_at, km, 3, 2, name) == 1); /* positions 0 and 1 */
    CHECK(os_key_count_bound(fake_dev_at, km, 3, 6, "mo") == 0);
    CHECK(os_key_count_bound(fake_dev_at, km, 0, 6, name) == 0);
}

int main(void) {
    test_params();
    test_map();
    test_press_release();
    test_mac();
    test_gui_lock();
    test_setting_changed_while_held();
    test_same_key_twice();
    test_position_pressed_again();
    test_held_limit();
    test_overflow_then_free();
    test_invalid_param();
    test_reset();
    test_count_bound();
    if (failed) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("os_key: all tests passed\n");
    return 0;
}
