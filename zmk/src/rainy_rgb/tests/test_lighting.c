/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the brightness policy (lighting.c): idle timer and battery
 * cap.
 */

#include "../lighting.h"
#include "test.h"

static void test_idle_never(void) {
    CHECK(rrgb_idle_state(10000000u, 0, 0, RRGB_IDLE_MODE_OFF) == RRGB_IDLE_AWAKE);
    CHECK(rrgb_idle_state(10000000u, 0, 0, RRGB_IDLE_MODE_DIM) == RRGB_IDLE_AWAKE);
}

static void test_idle_timeout(void) {
    /* 10 s: awake up to 9999 ms after the last key, idle from 10000 ms */
    CHECK(rrgb_idle_state(5000u + 9999u, 5000u, 10, RRGB_IDLE_MODE_OFF) == RRGB_IDLE_AWAKE);
    CHECK(rrgb_idle_state(5000u + 10000u, 5000u, 10, RRGB_IDLE_MODE_OFF) == RRGB_IDLE_DARK);
    CHECK(rrgb_idle_state(5000u + 10000u, 5000u, 10, RRGB_IDLE_MODE_DIM) == RRGB_IDLE_DIMMED);
    /* the longest timeout, 3600 s */
    CHECK(rrgb_idle_state(3599999u, 0, 3600, RRGB_IDLE_MODE_DIM) == RRGB_IDLE_AWAKE);
    CHECK(rrgb_idle_state(3600000u, 0, 3600, RRGB_IDLE_MODE_DIM) == RRGB_IDLE_DIMMED);
    /* an unknown mode counts as off */
    CHECK(rrgb_idle_state(20000u, 0, 10, 7) == RRGB_IDLE_DARK);
}

static void test_idle_wrap(void) {
    /* the uptime wraps after ~49.7 days: 0xFFFFFC18 is 1000 ms before 0 */
    CHECK(rrgb_idle_state(1000u, 0xFFFFFC18u, 1, RRGB_IDLE_MODE_OFF) == RRGB_IDLE_DARK);
    CHECK(rrgb_idle_state(500u, 0xFFFFFC18u, 2, RRGB_IDLE_MODE_OFF) == RRGB_IDLE_AWAKE);
}

static void test_cap(void) {
    CHECK(rrgb_render_val(200, 255, false, RRGB_IDLE_AWAKE) == 200); /* no cap */
    CHECK(rrgb_render_val(200, 100, false, RRGB_IDLE_AWAKE) == 100); /* capped on battery */
    CHECK(rrgb_render_val(200, 100, true, RRGB_IDLE_AWAKE) == 200);  /* USB host: no cap */
    CHECK(rrgb_render_val(80, 100, false, RRGB_IDLE_AWAKE) == 80);   /* below the cap */
}

static void test_dim(void) {
    CHECK(rrgb_render_val(200, 255, true, RRGB_IDLE_DIMMED) == 50);
    CHECK(rrgb_render_val(200, 100, false, RRGB_IDLE_DIMMED) == 25); /* cap, then a quarter */
    CHECK(rrgb_render_val(16, 255, true, RRGB_IDLE_DIMMED) == 4);
    CHECK(rrgb_render_val(200, 255, true, RRGB_IDLE_DARK) == 200); /* dark: no effect drawn */
}

int main(void) {
    test_idle_never();
    test_idle_timeout();
    test_idle_wrap();
    test_cap();
    test_dim();
    DONE();
}
