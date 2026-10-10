/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deep sleep trigger (CONFIG_RAINY75_SLEEP), in place of ZMK's
 * (CONFIG_ZMK_SLEEP, app/src/activity.c): once a second,
 * sleep_policy_should_sleep() decides from the time since the last
 * activity, kb.sleep_min, the USB host state and kb.sleep_on_usb. Activity
 * is a key position or sensor event (the events activity.c counts) or a
 * settings change (the registry's rev: a set or reset from a host, the Fn
 * keys), so a host that only reads settings (the config page polls info
 * every second) does not keep the keyboard awake. Sleep is activity.c's
 * sequence: zmk_pm_suspend_devices(), on failure zmk_pm_resume_devices()
 * and a retry at the next check, then sys_poweroff(), whose platform part
 * is poweroff.c (BLE controller quiesced, LED rail and USB pull-up off, any
 * key wakes the keyboard with a cold boot). ZMK's activity states ACTIVE
 * and IDLE (battery sampling) stay as they are; ZMK_ACTIVITY_SLEEP is no
 * longer raised. After a ZMK update, compare its activity.c and pm.c with
 * this file (CLAUDE.md).
 *
 * Threads: the check runs on the system work queue, the listener in the
 * thread that raises the event (key positions: the system work queue as
 * well). last_activity_ms is one aligned word, stored and loaded whole; the
 * USB state below is used by the check only.
 *
 * Listener order: ZMK runs listeners in link order, and this one comes last
 * for key positions (activity.c, keymap, rainy_rgb, this file). If the
 * keymap listener stops an event (an error, or &trans on every layer), this
 * listener misses it, while ZMK's activity.c (the first) still sees it. See
 * docs/zmk-firmware.md, zmk-src patch 0007, on listener order.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/pm.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/endpoints.h>
#endif

#if IS_ENABLED(CONFIG_RAINY75_CONFIG)
#include "config/cfg_registry.h"
#include "config/cfg_table.h"
#endif
#include "sleep/sleep_policy.h"

LOG_MODULE_REGISTER(rainy75_sleep, CONFIG_LOG_DEFAULT_LEVEL);

BUILD_ASSERT(IS_ENABLED(CONFIG_PM_DEVICE_SYSTEM_MANAGED),
             "zmk_pm_suspend_devices() needs CONFIG_PM_DEVICE_SYSTEM_MANAGED");

#define SLEEP_CHECK_MS 1000

static volatile uint32_t last_activity_ms; /* k_uptime_get_32() of the last activity */
#if IS_ENABLED(CONFIG_RAINY75_CONFIG)
static uint32_t last_rev; /* cfg_rev() at the last check */
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
static bool ble_suspended;           /* bus suspended and output Bluetooth at the last check */
static uint32_t ble_suspended_since; /* k_uptime_get_32() when that began */
static uint8_t logged_usb = 0xff;    /* USB inputs and decision last logged, 0xff = none yet */
#endif

static int sleep_activity_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    last_activity_ms = k_uptime_get_32();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rainy75_sleep, sleep_activity_listener);
ZMK_SUBSCRIPTION(rainy75_sleep, zmk_position_state_changed);
ZMK_SUBSCRIPTION(rainy75_sleep, zmk_sensor_event);

/* kb.sleep_min and kb.sleep_on_usb; the defaults in a build without the
 * runtime settings, and if the registry refused the settings table
 * (cfg_table.c logs it): cfg_u() would give 0 then, which means never. */
static uint32_t sleep_min(void) {
#if IS_ENABLED(CONFIG_RAINY75_CONFIG)
    if (cfg_count() == CFG_ID_COUNT) {
        return cfg_u(CFG_KB_SLEEP_MIN);
    }
#endif
    return SLEEP_POLICY_MIN_DEFAULT;
}

static bool sleep_on_usb(void) {
#if IS_ENABLED(CONFIG_RAINY75_CONFIG)
    if (cfg_count() == CFG_ID_COUNT) {
        return cfg_u(CFG_KB_SLEEP_ON_USB) != 0;
    }
#endif
    return SLEEP_POLICY_ON_USB_DEFAULT;
}

/* A settings change (host set or reset, Fn keys) counts as activity. True
 * when there was one at this check. */
static bool note_settings_change(uint32_t now) {
#if IS_ENABLED(CONFIG_RAINY75_CONFIG)
    uint32_t rev = cfg_rev();

    if (rev != last_rev) {
        last_rev = rev;
        last_activity_ms = now;
        return true;
    }
#else
    ARG_UNUSED(now);
#endif
    return false;
}

/* ZMK's USB and output state, read at every check: the state that its
 * zmk_usb_conn_state_changed and endpoint events announce. The time since
 * the bus suspended with the output on Bluetooth starts when both first
 * hold and ends when either stops (now - since wraps correctly modulo 2^32
 * for times below 49 days). One log line whenever an input or the decision
 * changes, so a hardware test sees whether the B91 notices a USB suspend. */
static bool usb_host(uint32_t now) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    bool configured = zmk_usb_is_hid_ready();
    bool suspended = zmk_usb_get_status() == USB_DC_SUSPEND;
    bool output_ble = false;
    bool host;
    uint8_t state;

#if IS_ENABLED(CONFIG_ZMK_BLE)
    output_ble = zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_BLE;
#endif
    if (!(suspended && output_ble)) {
        ble_suspended = false;
    } else if (!ble_suspended) {
        ble_suspended = true;
        ble_suspended_since = now;
    }
    host = sleep_policy_usb_host(configured, suspended, output_ble,
                                 ble_suspended ? now - ble_suspended_since : 0);
    state = (uint8_t)(configured | (suspended << 1) | (output_ble << 2) | (host << 3));
    if (state != logged_usb) {
        LOG_INF("sleep: usb configured=%d suspended=%d output=%s host=%s", configured, suspended,
                output_ble ? "ble" : "usb", host ? "yes" : "no");
        logged_usb = state;
    }
    return host;
#else
    ARG_UNUSED(now);
    return false;
#endif
}

static void sleep_check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_check);

static void sleep_check(struct k_work *work) {
    /* last before now: an event on another thread between the two reads can
     * then only make the idle time a little longer. The other way round,
     * now - last would be negative and wrap to about 49 days (deep sleep in
     * the middle of typing). */
    uint32_t last = last_activity_ms;
    uint32_t now = k_uptime_get_32();
    uint32_t minutes = sleep_min();
    bool host = usb_host(now);

    ARG_UNUSED(work);
    if (note_settings_change(now)) {
        last = now;
    }
    /* now - last wraps correctly (modulo 2^32) for idle times below 49 days */
    if (sleep_policy_should_sleep(now - last, minutes, host, sleep_on_usb())) {
        LOG_INF("sleep: %u min without activity, deep sleep", minutes);
        if (zmk_pm_suspend_devices() < 0) {
            LOG_ERR("sleep: suspending the devices failed, next try in 1 s");
            zmk_pm_resume_devices();
        } else {
            sys_poweroff(); /* does not return: a key cold-boots the keyboard */
        }
    }
    k_work_reschedule(&sleep_work, K_MSEC(SLEEP_CHECK_MS));
}

static int sleep_init(void) {
    last_activity_ms = k_uptime_get_32();
#if IS_ENABLED(CONFIG_RAINY75_CONFIG)
    last_rev = cfg_rev();
#endif
    k_work_schedule(&sleep_work, K_MSEC(SLEEP_CHECK_MS));
    return 0;
}

/* Where ZMK starts its activity timer (activity_init). */
SYS_INIT(sleep_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
