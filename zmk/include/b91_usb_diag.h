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
