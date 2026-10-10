/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Brightness policy, see lighting.h. Pure: no Zephyr, no ZMK.
 */

#include "lighting.h"

enum rrgb_idle_state rrgb_idle_state(uint32_t now_ms, uint32_t last_ms, uint16_t idle_s,
                                     uint8_t mode) {
    /* Unsigned difference: right across the uptime wrap. After 2^32 ms
     * (~49.7 days) without a key it wraps once and shows idle_s of light. */
    if (idle_s == 0 || (uint32_t)(now_ms - last_ms) < (uint32_t)idle_s * 1000u) {
        return RRGB_IDLE_AWAKE;
    }
    return mode == RRGB_IDLE_MODE_DIM ? RRGB_IDLE_DIMMED : RRGB_IDLE_DARK;
}

uint8_t rrgb_render_val(uint8_t val, uint8_t cap, bool usb_host, enum rrgb_idle_state idle) {
    uint8_t v = (!usb_host && val > cap) ? cap : val;

    return idle == RRGB_IDLE_DIMMED ? (uint8_t)(v / 4) : v;
}

bool rrgb_usb_host(bool hid_ready, bool bus_suspended, bool output_ble) {
    return hid_ready && !(bus_suspended && output_ble);
}
