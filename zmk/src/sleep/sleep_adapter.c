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
 * well). last_activity_ms is one aligned word, stored and loaded whole.
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
static int8_t logged_host = -1; /* USB host state last logged, -1 = none yet */

static int sleep_activity_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    last_activity_ms = k_uptime_get_32();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rainy75_sleep, sleep_activity_listener);
ZMK_SUBSCRIPTION(rainy75_sleep, zmk_position_state_changed);
ZMK_SUBSCRIPTION(rainy75_sleep, zmk_sensor_event);

/* kb.sleep_min and kb.sleep_on_usb; the defaults in a build without the
 * runtime settings. */
static uint32_t sleep_min(void) {
#if IS_ENABLED(CONFIG_RAINY75_CONFIG)
    return cfg_u(CFG_KB_SLEEP_MIN);
#else
    return SLEEP_POLICY_MIN_DEFAULT;
#endif
}

static bool sleep_on_usb(void) {
#if IS_ENABLED(CONFIG_RAINY75_CONFIG)
    return cfg_u(CFG_KB_SLEEP_ON_USB) != 0;
#else
    return SLEEP_POLICY_ON_USB_DEFAULT;
#endif
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
 * zmk_usb_conn_state_changed and endpoint events announce. */
static bool usb_host(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    bool output_ble = false;

#if IS_ENABLED(CONFIG_ZMK_BLE)
    output_ble = zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_BLE;
#endif
    return sleep_policy_usb_host(zmk_usb_is_hid_ready(), zmk_usb_get_status() == USB_DC_SUSPEND,
                                 output_ble);
#else
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
    bool host = usb_host();

    ARG_UNUSED(work);
    if (note_settings_change(now)) {
        last = now;
    }
    if (logged_host != (int8_t)host) {
        LOG_INF("sleep: usb host %s", host ? "yes" : "no");
        logged_host = (int8_t)host;
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
