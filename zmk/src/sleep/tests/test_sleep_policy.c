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
    CHECK(!sleep_policy_should_sleep(UINT32_MAX, 0, true, false));
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

#define GRACE SLEEP_POLICY_BLE_GRACE_MS

/* The USB host: configured, also on a suspended bus, except while the
 * keyboard types over Bluetooth on a suspended bus for more than the grace
 * time. The time only counts while both hold: the adapter passes 0
 * otherwise, and any value is ignored then. */
static void test_usb_host_signal(void) {
    CHECK(!sleep_policy_usb_host(false, false, false, 0)); /* battery, or a charger only */
    CHECK(!sleep_policy_usb_host(false, true, false, 0));
    CHECK(!sleep_policy_usb_host(false, false, true, 0));
    CHECK(sleep_policy_usb_host(true, false, false, 0)); /* awake host */
    CHECK(sleep_policy_usb_host(true, false, true, 0));  /* awake host, typing over Bluetooth */
    CHECK(sleep_policy_usb_host(true, true, false, 0));  /* sleeping host, output USB: key wakes */
    CHECK(sleep_policy_usb_host(true, true, true, 0));   /* just suspended, output BLE: grace */
    /* sleeping host or cable pulled, output BLE to another device */
    CHECK(!sleep_policy_usb_host(true, true, true, UINT32_MAX));
    /* the time is ignored unless the bus is suspended and the output is BLE */
    CHECK(sleep_policy_usb_host(true, false, false, UINT32_MAX));
    CHECK(sleep_policy_usb_host(true, false, true, UINT32_MAX));
    CHECK(sleep_policy_usb_host(true, true, false, UINT32_MAX));
    CHECK(!sleep_policy_usb_host(false, false, false, UINT32_MAX));
}

/* The grace time: a Bluetooth link to the computer that suspends USB drops
 * within the supervision timeout (up to 32 s); until then a suspended bus
 * still counts. More than 60 s: the Bluetooth host is another device. */
static void test_ble_grace(void) {
    CHECK(GRACE == 60000u);
    CHECK(sleep_policy_usb_host(true, true, true, 32000));
    CHECK(sleep_policy_usb_host(true, true, true, 60000));
    CHECK(!sleep_policy_usb_host(true, true, true, 60001));
}

/* Not configured is never a host, whatever the time. */
static void test_unconfigured(void) {
    CHECK(!sleep_policy_usb_host(false, true, true, 0));
    CHECK(!sleep_policy_usb_host(false, true, true, 60000));
    CHECK(!sleep_policy_usb_host(false, true, true, 60001));
    CHECK(!sleep_policy_usb_host(false, true, true, UINT32_MAX));
}

int main(void) {
    test_never();
    test_battery();
    test_usb_host();
    test_no_overflow();
    test_usb_host_signal();
    test_ble_grace();
    test_unconfigured();
    if (failed) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("sleep_policy: all tests passed\n");
    return 0;
}
