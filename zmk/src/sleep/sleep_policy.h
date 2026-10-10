/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deep sleep trigger: the decisions, pure and ZMK-free (host tested in
 * tests/). sleep_adapter.c asks them once a second with the time since the
 * last activity and the USB state, and powers the keyboard off when they
 * say so. The settings come from config/cfg_table.c: kb.sleep_min (minutes
 * without activity, 0 = never) and kb.sleep_on_usb (also sleep while a USB
 * host is connected).
 *
 *  - kb.sleep_min 0: never.
 *  - A USB host connected and kb.sleep_on_usb off: never.
 *  - Else: once more than kb.sleep_min minutes passed without activity.
 *
 * "USB host connected" (sleep_policy_usb_host): a host has configured the
 * keyboard, also while it suspends the bus (the computer sleeps), so a key
 * press can wake it. Not while the bus is suspended and the keyboard types
 * over Bluetooth: a key press then goes to the Bluetooth host and cannot
 * wake the computer anyway, and a keyboard pulled from a sleeping computer
 * (the B91 cannot see the pull) still sleeps.
 */

#ifndef RAINY75_SLEEP_POLICY_H
#define RAINY75_SLEEP_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* kb.sleep_min: default and range max, in minutes (0 = never). */
#define SLEEP_POLICY_MIN_DEFAULT 15
#define SLEEP_POLICY_MIN_MAX 120
/* kb.sleep_on_usb: default (off). */
#define SLEEP_POLICY_ON_USB_DEFAULT 0

/* True when the keyboard should sleep now: idle_ms since the last activity,
 * sleep_min = kb.sleep_min, usb_host = sleep_policy_usb_host(), sleep_on_usb
 * = kb.sleep_on_usb. */
bool sleep_policy_should_sleep(uint32_t idle_ms, uint32_t sleep_min, bool usb_host,
                               bool sleep_on_usb);

/* The usb_host of sleep_policy_should_sleep(): hid_ready = a host configured
 * the keyboard (ZMK's zmk_usb_is_hid_ready(), true also while the bus is
 * suspended), bus_suspended = the bus is suspended now, output_ble = the
 * keyboard types over Bluetooth now. */
bool sleep_policy_usb_host(bool hid_ready, bool bus_suspended, bool output_ble);

#endif /* RAINY75_SLEEP_POLICY_H */
