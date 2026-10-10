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
 * press can wake it. One exception: while it types over Bluetooth to
 * another device and the computer has had USB asleep for about a minute
 * (more than SLEEP_POLICY_BLE_GRACE_MS with the bus suspended and the
 * output on Bluetooth, without a break). A key press then goes to the
 * Bluetooth device and cannot wake the computer anyway, and a keyboard
 * pulled from a sleeping computer (the B91 cannot see the pull) still
 * sleeps. The minute is for a Bluetooth link to the computer that suspends
 * USB: when that computer sleeps, the link drops within the BLE supervision
 * timeout (up to 32 s), ZMK falls back to USB output, and the keyboard
 * must stay awake so that a key press can wake the computer over USB.
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
/* The exception of sleep_policy_usb_host() starts after this long with the
 * bus suspended and the output on Bluetooth: more than the longest BLE
 * supervision timeout (32 s). */
#define SLEEP_POLICY_BLE_GRACE_MS 60000u

/* True when the keyboard should sleep now: idle_ms since the last activity,
 * sleep_min = kb.sleep_min, usb_host = sleep_policy_usb_host(), sleep_on_usb
 * = kb.sleep_on_usb. */
bool sleep_policy_should_sleep(uint32_t idle_ms, uint32_t sleep_min, bool usb_host,
                               bool sleep_on_usb);

/* The usb_host of sleep_policy_should_sleep(): hid_ready = a host configured
 * the keyboard (ZMK's zmk_usb_is_hid_ready(), true also while the bus is
 * suspended), bus_suspended = the bus is suspended now, output_ble = the
 * keyboard types over Bluetooth now, ble_suspended_ms = how long both
 * bus_suspended and output_ble have held without a break (ignored unless
 * both hold now). False only without hid_ready, or with both for more than
 * SLEEP_POLICY_BLE_GRACE_MS. */
bool sleep_policy_usb_host(bool hid_ready, bool bus_suspended, bool output_ble,
                           uint32_t ble_suspended_ms);

#endif /* RAINY75_SLEEP_POLICY_H */
