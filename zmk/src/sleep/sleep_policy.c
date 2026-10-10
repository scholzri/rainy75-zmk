/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deep sleep decisions, see sleep_policy.h. Pure: no Zephyr, no ZMK.
 */

#include "sleep_policy.h"

#define MS_PER_MIN 60000u

bool sleep_policy_should_sleep(uint32_t idle_ms, uint32_t sleep_min, bool usb_host,
                               bool sleep_on_usb) {
    if (sleep_min == 0) {
        return false; /* never */
    }
    if (usb_host && !sleep_on_usb) {
        return false;
    }
    /* 64 bits: no wrap for any sleep_min (the setting stops at 120) */
    return (uint64_t)idle_ms > (uint64_t)sleep_min * MS_PER_MIN;
}

bool sleep_policy_usb_host(bool hid_ready, bool bus_suspended, bool output_ble) {
    return hid_ready && !(bus_suspended && output_ble);
}
