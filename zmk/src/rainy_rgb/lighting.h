/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Brightness policy of the effect layer, pure and host tested
 * (tests/test_lighting.c): the battery cap (setting rgb.val_battery) and the
 * idle timer (rgb.idle_s, rgb.idle_mode). The engine applies the result to
 * the effect only; the functional overlays keep their own fixed levels. Also
 * the decision whether a USB host counts as connected for both.
 */

#ifndef RAINY_RGB_LIGHTING_H
#define RAINY_RGB_LIGHTING_H
#include <stdbool.h>
#include <stdint.h>

/* rgb.idle_mode, in the order of its names ("off", "dim"). */
enum rrgb_idle_mode { RRGB_IDLE_MODE_OFF = 0, RRGB_IDLE_MODE_DIM = 1 };

/* What the idle timer asks for this frame. */
enum rrgb_idle_state { RRGB_IDLE_AWAKE = 0, RRGB_IDLE_DARK, RRGB_IDLE_DIMMED };

/* idle_s seconds (0 = never) after last_ms: DARK for mode off (and any
 * unknown mode), DIMMED for mode dim, AWAKE before. Both times are the 32-bit
 * millisecond uptime; the unsigned difference is right across its wrap.
 * last_ms must not be newer than now_ms: read it before the clock. */
enum rrgb_idle_state rrgb_idle_state(uint32_t now_ms, uint32_t last_ms, uint16_t idle_s,
                                     uint8_t mode);

/* Brightness the effect renders with: val, capped at cap while no USB host
 * is connected, then a quarter of that while DIMMED. */
uint8_t rrgb_render_val(uint8_t val, uint8_t cap, bool usb_host, enum rrgb_idle_state idle);

/* Whether a USB host counts as connected for the battery cap and the
 * low-battery pulse. hid_ready: a host configured the keyboard (ZMK
 * zmk_usb_is_hid_ready()); bus_suspended: the bus is in suspend
 * (USB_DC_SUSPEND); output_ble: the selected output is Bluetooth.
 *
 * Without VBUS detection a cable pull can arrive as a plain suspend straight
 * from the configured state, with no bus reset, and hid_ready then stays true.
 * With output USB the next keypress re-attaches and clears it; with output
 * Bluetooth nothing would, so a suspended bus counts as no host there. A PC
 * asleep with output Bluetooth also counts as no host (the cap applies while
 * it sleeps, harmless). */
bool rrgb_usb_host(bool hid_ready, bool bus_suspended, bool output_ble);

#endif
