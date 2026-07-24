/*
 * Copyright (c) 2026 ecliptik
 * SPDX-License-Identifier: Apache-2.0
 *
 * B91 USB diagnostic event ring — shared between the USB DC driver (writer),
 * flash_mgmt (SMP reader, group 64 cmd 4) and the CDC recovery module.
 *
 * The ring lives in .noinit SRAM inside the driver: the MCU runs from battery,
 * so events survive cable pulls and re-attach cycles and can be read back over
 * SMP after the CDC path recovers.  See usb_dc_b91.c for the event codes.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

struct b91_usb_diag_evt {
	uint32_t ms;            /* k_uptime at event, wraps at ~49 days */
	uint8_t  code;          /* enum b91_usb_diag_code */
	uint8_t  a;
	uint16_t b;
};

/* Copies up to max_evts entries (oldest first, skipping the first `skip`)
 * into out; total event count ever written is returned via seq. */
size_t b91_usb_diag_snapshot(struct b91_usb_diag_evt *out, size_t skip,
			     size_t max_evts, uint32_t *seq);

/* Invoked by the driver's suspend poll when the CDC bulk OUT endpoint has
 * been dead (enabled-but-unarmed, or torn out of EDP_EN) past the starvation
 * threshold while the host holds the device configured.  Weak no-op in the
 * driver; the app-side recovery module overrides it to persist the ring and
 * cycle a USB re-attach. */
void b91_usb_cdc_starved(void);

/* Ring blob persisted at the last starvation event (u32 seq, u32 count,
 * then count evt records), loaded from settings at boot.  Returns the blob
 * length (0 = none).  Weak zero-length default in the driver so flash_mgmt
 * links in builds without the recovery module. */
size_t b91_usb_diag_saved(const uint8_t **blob);

/* True once the driver's liveness probe has declared the USB workqueue
 * thread dead (marker work parked >= 10 s).  A starved CDC with a dead
 * workqueue cannot be recovered by re-attach — only a reboot rebuilds the
 * thread — so the recovery module escalates accordingly. */
bool b91_usb_wq_stuck(void);

/* On-demand re-attach stress (flash_mgmt group 64 cmd 5): run `cycles`
 * usb_disable/usb_enable cycles spaced gap_ms apart, reproducing the
 * enumeration burst that births the CDC wedge.  Weak no-op in the driver
 * so builds without the recovery module link. */
void b91_usb_stress_start(uint32_t cycles, uint32_t gap_ms);

/* From the fork's zephyr patch 0006 (usb_transfer.c): copy up to max
 * transfer slots' {ep, status, k_work busy flags}.  Returns slots copied. */
size_t usb_transfer_slots_snapshot(uint8_t *eps, int8_t *statuses,
				   uint8_t *wflags, size_t max);

/* Uptime (ms) of the last HID interrupt-IN write — i.e. the user's last
 * keystroke/report.  Recovery uses it to defer disruptive re-attach/reboot
 * cycles to a typing pause. */
int64_t b91_usb_last_hid_activity(void);
