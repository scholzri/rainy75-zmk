/*
 * Copyright (c) 2026 ecliptik
 * SPDX-License-Identifier: Apache-2.0
 *
 * CDC starvation recovery — overrides the USB DC driver's weak
 * b91_usb_cdc_starved() hook.
 *
 * When the driver detects the CDC bulk OUT endpoint dead for minutes while
 * the host holds the device configured (the wedge observed after macOS
 * sleep/dark-wake enumeration churn: CDC mute both directions, HID alive,
 * host believes everything is fine), the only clean recovery is the same one
 * a cable pull triggers: a full detach/re-attach cycle, which the host
 * answers with a fresh enumeration that rebuilds the class transfers.  HID
 * drops for the ~1-2 s the cycle takes.
 *
 * Before recovering, the diag ring is persisted via settings/NVS: with the
 * wireless switch off the board is VBUS-powered only, so a cable pull is a
 * full power cycle and .noinit SRAM does NOT survive it — flash is the only
 * medium that can carry the pre-wedge event sequence across the user's
 * instinctive replug.  Read back post-mortem with usb_diag.py --saved.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include <b91_usb_diag.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

LOG_MODULE_REGISTER(usb_cdc_recover, LOG_LEVEL_INF);

/* ZMK's USB status callback (app/src/usb.c, non-static) — re-attach must
 * re-register it or ZMK stops tracking the connection state. */
void usb_status_cb(enum usb_dc_status_code status, const uint8_t *params);

/* One re-attach per holdoff window.  The only false-positive path (a host
 * that stuffed the CDC ring while nothing on the host reads the port) would
 * otherwise re-trigger every starvation threshold. */
#define RECOVER_HOLDOFF_MS (10 * 60 * 1000)

static int64_t last_recover_at = -RECOVER_HOLDOFF_MS;
static uint32_t recover_count;

/* Saved-ring blob: u32 seq, u32 count, then count b91_usb_diag_evt records.
 * One buffer for the copy loaded from settings at boot (last wedge, possibly
 * a prior power cycle), one staging buffer for the save path. */
#define RING_BLOB_MAX (8 + 64 * sizeof(struct b91_usb_diag_evt))

static uint8_t saved_ring[RING_BLOB_MAX];
static size_t saved_ring_len;

size_t b91_usb_diag_saved(const uint8_t **blob)
{
	*blob = saved_ring;
	return saved_ring_len;
}

#if IS_ENABLED(CONFIG_SETTINGS)

static int ring_settings_set(const char *name, size_t len,
			     settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "ring") != 0 || len > sizeof(saved_ring)) {
		return -ENOENT;
	}

	ssize_t rd = read_cb(cb_arg, saved_ring, len);

	if (rd >= 0) {
		saved_ring_len = rd;
		LOG_INF("loaded saved USB diag ring (%u bytes)", (unsigned)rd);
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(b91usb, "b91usb", NULL, ring_settings_set,
			       NULL, NULL);

static void ring_persist(void)
{
	static uint8_t blob[RING_BLOB_MAX];
	struct b91_usb_diag_evt *evts = (void *)(blob + 8);
	uint32_t seq = 0;
	size_t n = b91_usb_diag_snapshot(evts, 0, 64, &seq);

	memcpy(blob, &seq, 4);
	uint32_t n32 = n;

	memcpy(blob + 4, &n32, 4);

	int rc = settings_save_one("b91usb/ring", blob,
				   8 + n * sizeof(evts[0]));

	LOG_WRN("USB diag ring persisted to flash (%u events, rc=%d)",
		(unsigned)n, rc);
}

#else /* !CONFIG_SETTINGS */

static void ring_persist(void)
{
}

#endif

/* Minimum uptime before the reboot escalation is allowed: if the wedge
 * re-formed this quickly after a boot, rebooting again would loop. */
#define RECOVER_REBOOT_MIN_UPTIME_MS (10 * 60 * 1000)

static void recover_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	ring_persist();

	/* Escalate to reboot when re-attach provably can't help:
	 *  - the USB workqueue thread is dead (every transfer completion
	 *    runs on it; hardware-confirmed 2026-07-23), or
	 *  - the previous recovery didn't stick (starved again within
	 *    15 min — a stuck transfer slot survives re-enumeration, and a
	 *    failed re-attach can even park the device unconfigured with
	 *    HID dead; observed needing a physical replug).
	 * Warm reset via the B91 PWDN register (same mechanism as
	 * flash_mgmt's trampoline) — ~2 s through MCUboot. */
	bool repeat_failure = (k_uptime_get() - last_recover_at) <
			      (15 * 60 * 1000) && recover_count > 0;

	recover_count++;
	if ((b91_usb_wq_stuck() || repeat_failure) &&
	    k_uptime_get() >= RECOVER_REBOOT_MIN_UPTIME_MS) {
		LOG_ERR("CDC starved + USB workqueue dead: rebooting");
		k_msleep(100);
		sys_write8(0x20, 0x801401ef);
		return; /* not reached */
	}

	LOG_WRN("CDC starved: cycling USB re-attach");
	usb_disable();
	k_msleep(50);
	int rc = usb_enable(usb_status_cb);

	LOG_WRN("CDC recovery re-attach done (rc=%d)", rc);
}

static K_WORK_DEFINE(recover_work, recover_work_cb);

void b91_usb_cdc_starved(void)
{
	int64_t now = k_uptime_get();

	/* Shorter holdoff (3 min) than before: the repeat-failure path in
	 * recover_work_cb needs a second chance to fire and escalate to
	 * reboot; 10 min of holdoff just prolonged a dead keyboard. */
	if (now - last_recover_at < (3 * 60 * 1000)) {
		LOG_WRN("CDC starved again within holdoff; skipping");
		return;
	}
	last_recover_at = now;
	k_work_submit(&recover_work);
}

/* ------------------------------------------------------------------------
 * On-demand re-attach stress (flash_mgmt group 64 cmd 5).
 *
 * Every captured wedge was born in the re-enumeration/re-attach burst —
 * never in plain suspend/resume (8/8 clean RTC wake cycles).  This lets a
 * host script trigger that exact burst repeatedly, with SMP traffic racing
 * the cancels, turning a once-a-day organic repro into an on-demand one.
 * Runs on the system workqueue so a killed USB workqueue can't stop it.
 * ------------------------------------------------------------------------ */

static uint32_t stress_remaining;
static uint32_t stress_gap_ms;

static void stress_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(stress_work, stress_work_cb);

static void stress_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	if (stress_remaining == 0) {
		return;
	}
	stress_remaining--;
	LOG_WRN("stress: re-attach cycle (%u left)", stress_remaining);
	usb_disable();
	k_msleep(50);
	usb_enable(usb_status_cb);

	if (stress_remaining > 0) {
		k_work_reschedule(&stress_work, K_MSEC(stress_gap_ms));
	} else {
		LOG_WRN("stress: run complete");
	}
}

void b91_usb_stress_start(uint32_t cycles, uint32_t gap_ms)
{
	stress_remaining = MIN(cycles, 200);
	stress_gap_ms = CLAMP(gap_ms, 500, 30000);
	LOG_WRN("stress: starting %u re-attach cycles, gap %u ms",
		stress_remaining, stress_gap_ms);
	/* Delay the first cycle so the SMP response reaches the host before
	 * the connection is torn down. */
	k_work_reschedule(&stress_work, K_MSEC(500));
}
