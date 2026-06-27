/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot image confirmation with watchdog safety net.
 *
 * Flow:
 * 1. PRE_KERNEL_1:  WDT driver init (disabled at boot)
 * 2. POST_KERNEL/1: Start WDT (11s timeout) — covers all remaining init
 * 3. APPLICATION/90: Feed WDT (reset countdown), schedule 5s confirm
 * 4. APPLICATION/99: ZMK/BLE/USB all initialized
 * 5. Post-init work queue (+5s): confirm image, disable WDT
 *
 * If step 5 never happens, WDT fires -> chip resets ->
 * MCUboot sees unconfirmed image -> reverts to previous firmware.
 *
 * Fatal error handler: overrides Zephyr's default (infinite spin)
 * with sys_reboot() so any crash triggers MCUboot revert of
 * unconfirmed images.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>


LOG_MODULE_REGISTER(mcuboot_confirm, LOG_LEVEL_INF);

/* Watchdog timeout — must cover POST_KERNEL through confirm.
 * Boot takes ~2s, confirm delay is 5s, so ~7s needed.
 * B91 WDT max is ~11184ms. Use 11000ms for maximum coverage. */
#define CONFIRM_WDT_TIMEOUT_MS  11000

/* Delay before confirmation — wait for USB + BLE to stabilize */
#define CONFIRM_DELAY_MS        5000

static const struct device *wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel_id = -1;

static void confirm_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(confirm_work, confirm_work_handler);

/* ========================================================================== *
 *  Fatal error handler — force reset instead of infinite spin                *
 * ========================================================================== */

void k_sys_fatal_error_handler(unsigned int reason,
			       const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	LOG_ERR("Fatal error %u — rebooting for MCUboot revert", reason);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

/* ========================================================================== *
 *  Image confirmation (delayed work queue)                                   *
 * ========================================================================== */

static void confirm_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!boot_is_img_confirmed()) {
		int rc = boot_write_img_confirmed();
		if (rc == 0) {
			LOG_INF("Image confirmed — swap is now permanent");
		} else {
			LOG_ERR("Failed to confirm image: %d", rc);
			/* Don't disable WDT — let it revert */
			return;
		}
	} else {
		LOG_INF("Image already confirmed");
	}

	/* Image is confirmed — disable watchdog */
	wdt_disable(wdt_dev);
	LOG_INF("Watchdog disabled");
}

/* ========================================================================== *
 *  Early WDT start (POST_KERNEL) — covers all remaining init                *
 * ========================================================================== */

static int mcuboot_wdt_early_start(void)
{
	int rc;
	struct wdt_timeout_cfg wdt_cfg = {
		.window = {
			.min = 0,
			.max = CONFIRM_WDT_TIMEOUT_MS,
		},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (!device_is_ready(wdt_dev)) {
		LOG_WRN("WDT device not ready — no watchdog safety net");
		return 0;
	}

	rc = wdt_install_timeout(wdt_dev, &wdt_cfg);
	if (rc < 0) {
		LOG_ERR("WDT install_timeout failed: %d", rc);
		return 0;
	}
	wdt_channel_id = rc;

	rc = wdt_setup(wdt_dev, 0);
	if (rc < 0) {
		LOG_ERR("WDT setup failed: %d", rc);
		return 0;
	}

	LOG_INF("WDT started at POST_KERNEL (%d ms)", CONFIRM_WDT_TIMEOUT_MS);
	return 0;
}

/* Priority 1: early in POST_KERNEL */
SYS_INIT(mcuboot_wdt_early_start, POST_KERNEL, 1);

/* ========================================================================== *
 *  Confirmation scheduler (APPLICATION) — feed WDT, schedule confirm         *
 * ========================================================================== */

static int mcuboot_confirm_init(void)
{
	/* Feed WDT to reset the countdown — we've made it to APPLICATION */
	if (wdt_channel_id >= 0) {
		wdt_feed(wdt_dev, wdt_channel_id);
		LOG_INF("WDT fed at APPLICATION — %d ms until revert",
			CONFIRM_WDT_TIMEOUT_MS);
	}
	LOG_INF("Rainy 75 Pro — build %s", BUILD_GIT_VERSION);
	/* Schedule confirmation after delay — gives USB + BLE time to init */
	k_work_schedule(&confirm_work, K_MSEC(CONFIRM_DELAY_MS));
	return 0;
}

/* Run at APPLICATION priority 90, before ZMK app init (which is at 99) */
SYS_INIT(mcuboot_confirm_init, APPLICATION, 90);
