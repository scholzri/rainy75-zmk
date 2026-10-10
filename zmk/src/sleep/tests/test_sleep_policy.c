/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the sleep decisions (sleep_policy.c), one test per rule of
 * sleep_policy.h.
 */

#include "../sleep_policy.h"
#include <stdio.h>

static int failed;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

#define MIN_MS 60000u

/* kb.sleep_min 0 never sleeps, whatever else. */
static void test_never(void) {
    CHECK(!sleep_policy_should_sleep(UINT32_MAX, 0, false, false));
    CHECK(!sleep_policy_should_sleep(UINT32_MAX, 0, false, true));
    CHECK(!sleep_policy_should_sleep(UINT32_MAX, 0, true, true));
}

/* No USB host: more than kb.sleep_min minutes, not exactly that long. */
static void test_battery(void) {
    CHECK(!sleep_policy_should_sleep(0, 15, false, false));
    CHECK(!sleep_policy_should_sleep(15 * MIN_MS, 15, false, false));
    CHECK(sleep_policy_should_sleep(15 * MIN_MS + 1, 15, false, false));
    CHECK(!sleep_policy_should_sleep(MIN_MS, 1, false, false));
    CHECK(sleep_policy_should_sleep(MIN_MS + 1, 1, false, false));
    CHECK(!sleep_policy_should_sleep(120 * MIN_MS, 120, false, false));
    CHECK(sleep_policy_should_sleep(120 * MIN_MS + 1, 120, false, false));
    CHECK(sleep_policy_should_sleep(15 * MIN_MS + 1, 15, false, true)); /* on_usb: no change here */
}

/* A USB host keeps the keyboard awake, unless kb.sleep_on_usb. */
static void test_usb_host(void) {
    CHECK(!sleep_policy_should_sleep(UINT32_MAX, 15, true, false));
    CHECK(!sleep_policy_should_sleep(UINT32_MAX, 1, true, false));
    CHECK(!sleep_policy_should_sleep(15 * MIN_MS, 15, true, true));
    CHECK(sleep_policy_should_sleep(15 * MIN_MS + 1, 15, true, true));
}

/* sleep_min * 60000 must not wrap: 71583 minutes are more than 2^32 ms. */
static void test_no_overflow(void) {
    CHECK(sleep_policy_should_sleep(UINT32_MAX, 71582, false, false)); /* 4294920000 ms */
    CHECK(!sleep_policy_should_sleep(UINT32_MAX, 71583, false, false));
    CHECK(!sleep_policy_should_sleep(UINT32_MAX, UINT32_MAX, false, false));
}

/* The USB host: configured, also on a suspended bus, except while the
 * keyboard types over Bluetooth on a suspended bus. */
static void test_usb_host_signal(void) {
    CHECK(!sleep_policy_usb_host(false, false, false)); /* battery, or a charger only */
    CHECK(!sleep_policy_usb_host(false, true, false));
    CHECK(!sleep_policy_usb_host(false, false, true));
    CHECK(sleep_policy_usb_host(true, false, false));  /* awake host */
    CHECK(sleep_policy_usb_host(true, false, true));   /* awake host, typing over Bluetooth */
    CHECK(sleep_policy_usb_host(true, true, false));   /* sleeping host, output USB: a key wakes it */
    CHECK(!sleep_policy_usb_host(true, true, true));   /* sleeping host or cable pulled, output BLE */
}

int main(void) {
    test_never();
    test_battery();
    test_usb_host();
    test_no_overflow();
    test_usb_host_signal();
    if (failed) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("sleep_policy: all tests passed\n");
    return 0;
}
