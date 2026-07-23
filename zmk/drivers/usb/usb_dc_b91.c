/*
 * Copyright (c) 2025 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telink B91 USB Device Controller — Legacy usb_dc.h API
 *
 * Implements the Zephyr legacy USB device controller API for the B91
 * (TLSR9518/9511) SoC's built-in USB 2.0 full-speed controller.
 */

#include "udc_b91.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/usb/usb_dc.h>
#include <zephyr/sys/util.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <b91_usb_diag.h>


LOG_MODULE_REGISTER(usb_dc_b91, CONFIG_USB_DRIVER_LOG_LEVEL);

/* USB register base address */
#define USB_BASE  0x80100800UL

/* Max endpoints: EP0 + EP1-EP8 */
#define NUM_EP    9

/* ========================================================================== *
 *  Driver State                                                               *
 * ========================================================================== */

struct usb_dc_b91_ep {
	uint16_t mps;
	uint8_t  type;
	bool     enabled;
	bool     stalled;
	uint8_t  buf[64];       /* OUT data staging buffer */
	uint16_t buf_len;       /* bytes received */
	uint16_t buf_offset;    /* read cursor for read_wait */
	uint8_t  sram_addr;     /* USB SRAM buffer offset owned by this EP */
	uint8_t  sram_size;     /* size of that buffer; 0 = none allocated */
};

struct usb_dc_b91_state {
	usb_dc_status_callback status_cb;
	usb_dc_ep_callback     ep_cb[2][NUM_EP]; /* [0]=OUT [1]=IN */
	struct usb_dc_b91_ep   ep[NUM_EP];
	bool                   attached;
	bool                   suspended;        /* bus idle observed by suspend poll */
	volatile uint32_t      isr_events;       /* ISR entry counter (bus activity) */
	bool                   ep0_in_transfer;  /* current EP0 direction from SETUP */
	uint8_t                ep_in_toggle[NUM_EP]; /* DATA0/1 toggle: PID the host expects next */
	bool                   ep_in_pending[NUM_EP]; /* IN transfer armed, awaiting host ACK */
};

static struct usb_dc_b91_state state;

/* ========================================================================== *
 *  USB SRAM Buffer Allocator                                                  *
 *                                                                             *
 *  B91 USB SRAM is 256 bytes (EP1-EP7 shared), plus a separate 8-byte FIFO   *
 *  for EP0.  EP buffer addresses are 8-bit registers in 1-byte units.         *
 *  Each EP needs at least max_packet_size bytes in SRAM.                      *
 *  OUT EP buffers MUST be >= global ep_max_size (64B) because the host        *
 *  controls packet size.  IN EP buffers can be smaller since firmware          *
 *  controls the write count.                                                  *
 * ========================================================================== */

#define USB_SRAM_SIZE 256

static uint8_t usb_sram_next;  /* next free byte in USB SRAM (bump allocator) */

/* ========================================================================== *
 *  USB Diagnostic Event Ring                                                  *
 *                                                                             *
 *  The CDC ACM console is this driver's own transport, so when the CDC path   *
 *  dies there is no log channel left to explain why.  This ring records the   *
 *  USB-level event sequence (status deliveries, SET_CONFIGURATIONs, EP        *
 *  configure/enable, CDC OUT arm-state transitions) in .noinit SRAM: the MCU  *
 *  runs from battery, so the ring survives cable pulls and re-attach cycles   *
 *  and can be read back over SMP (flash_mgmt group 64, cmd 4) after the CDC   *
 *  path recovers.  Lost only on actual power loss or cold boot.               *
 * ========================================================================== */

#define B91_USB_DIAG_MAGIC   0x55424447u /* "UBDG" */
#define B91_USB_DIAG_ENTRIES 64

enum b91_usb_diag_code {
	B91_DIAG_STATUS   = 1,  /* a = usb_dc_status_code delivered to stack */
	B91_DIAG_SETUP    = 2,  /* a = bRequest (5/9/11 only), b = wValue */
	B91_DIAG_EP_CFG   = 3,  /* a = ep_addr, b = SRAM offset (0xFFFF = ENOMEM) */
	B91_DIAG_EP_EN    = 4,  /* a = ep_addr */
	B91_DIAG_EP_DIS   = 5,  /* a = ep_addr */
	B91_DIAG_ARM      = 6,  /* a = armed?1:0, b = ticks spent in prior state */
	B91_DIAG_STARVED  = 7,  /* a = ep, b = un-armed ticks at trigger */
	B91_DIAG_DETACH   = 8,
	B91_DIAG_WQ_STUCK = 9,  /* b = poll ticks the USB workqueue sat idle */
	B91_DIAG_WQ_STATE = 10, /* a = thread_state byte, b = pended_on low 16 */
	B91_DIAG_WQ_PEND2 = 11, /* b = pended_on high 16 — resolve via zmk.elf map */
};

static __noinit struct {
	uint32_t magic;
	uint32_t seq;           /* total events ever written */
	struct b91_usb_diag_evt evt[B91_USB_DIAG_ENTRIES];
} usb_diag;

static void diag_ev(uint8_t code, uint8_t a, uint16_t b)
{
	unsigned int key = irq_lock();

	if (usb_diag.magic != B91_USB_DIAG_MAGIC) {
		memset(&usb_diag, 0, sizeof(usb_diag));
		usb_diag.magic = B91_USB_DIAG_MAGIC;
	}

	struct b91_usb_diag_evt *e =
		&usb_diag.evt[usb_diag.seq % B91_USB_DIAG_ENTRIES];

	e->ms = (uint32_t)k_uptime_get();
	e->code = code;
	e->a = a;
	e->b = b;
	usb_diag.seq++;

	irq_unlock(key);
}

/* Snapshot for the SMP reader: copies up to max_evts entries ending at the
 * newest, oldest first.  Returns the number of entries copied. */
size_t b91_usb_diag_snapshot(struct b91_usb_diag_evt *out, size_t skip,
			     size_t max_evts, uint32_t *seq)
{
	unsigned int key = irq_lock();

	if (usb_diag.magic != B91_USB_DIAG_MAGIC) {
		irq_unlock(key);
		*seq = 0;
		return 0;
	}

	uint32_t avail = MIN(usb_diag.seq, (uint32_t)B91_USB_DIAG_ENTRIES);
	uint32_t first = usb_diag.seq - avail;
	size_t n = 0;

	for (uint32_t i = first + skip; i < usb_diag.seq && n < max_evts; i++) {
		out[n++] = usb_diag.evt[i % B91_USB_DIAG_ENTRIES];
	}
	*seq = usb_diag.seq;

	irq_unlock(key);
	return n;
}

/* Hook invoked when the CDC bulk OUT endpoint has been starved (enabled but
 * un-armed while the host holds us configured) past the threshold.  The app
 * layer overrides this to cycle a USB re-attach; the weak default keeps
 * MCUboot/bridge builds linking without the recovery module. */
__weak void b91_usb_cdc_starved(void)
{
}

__weak size_t b91_usb_diag_saved(const uint8_t **blob)
{
	*blob = NULL;
	return 0;
}

/* ========================================================================== *
 *  Register Access Helpers                                                    *
 * ========================================================================== */

static inline uint8_t usb_read8(uint16_t off)
{
	return sys_read8(USB_BASE + off);
}

static inline void usb_write8(uint16_t off, uint8_t val)
{
	sys_write8(val, USB_BASE + off);
}

/* ========================================================================== *
 *  Analog Register Access (serial interface, not memory-mapped)               *
 * ========================================================================== */

static void analog_wait(void)
{
	while (sys_read8(B91_ANALOG_CTRL_REG) & B91_ANALOG_CTRL_BUSY) {
	}
}

static void analog_wait_txbuf(void)
{
	/*
	 * The B91 analog interface needs the TX buffer to be non-empty before a
	 * write is triggered; without this, a rapid write can emit stale FIFO data
	 * because the hardware starts the write cycle immediately.
	 */
	while (!(sys_read8(B91_ANALOG_BUF_CNT_REG) & B91_ANALOG_TX_BUFCNT)) {
	}
}

static uint8_t analog_read(uint8_t addr)
{
	unsigned int key = irq_lock();

	sys_write8(addr, B91_ANALOG_ADDR_REG);
	sys_write8(1, B91_ANALOG_LEN_REG);
	sys_write8(B91_ANALOG_CTRL_CYC, B91_ANALOG_CTRL_REG);
	analog_wait();
	uint8_t val = sys_read8(B91_ANALOG_DATA_REG);

	irq_unlock(key);
	return val;
}

static void analog_write(uint8_t addr, uint8_t data)
{
	unsigned int key = irq_lock();

	sys_write8(addr, B91_ANALOG_ADDR_REG);
	sys_write8(data, B91_ANALOG_DATA_REG);
	analog_wait_txbuf();
	sys_write8(B91_ANALOG_CTRL_CYC | B91_ANALOG_CTRL_RW, B91_ANALOG_CTRL_REG);
	analog_wait();
	sys_write8(0, B91_ANALOG_CTRL_REG);

	irq_unlock(key);
}

/* ========================================================================== *
 *  USB Pin and Power Control                                                  *
 * ========================================================================== */

static void dp_pullup_en(bool enable)
{
	uint8_t val = analog_read(B91_ANALOG_REG_USB_DP);

	if (enable) {
		val |= B91_ANALOG_DP_PULLUP;
	} else {
		val &= ~B91_ANALOG_DP_PULLUP;
	}
	analog_write(B91_ANALOG_REG_USB_DP, val);
}

static void usb_power_on(bool enable)
{
	uint8_t val = analog_read(B91_ANALOG_REG_USB_PWR);

	if (enable) {
		val &= ~B91_ANALOG_USB_PWR_DN;
	} else {
		val |= B91_ANALOG_USB_PWR_DN;
	}
	analog_write(B91_ANALOG_REG_USB_PWR, val);
}

static void usb_pin_setup(void)
{
	/* Clear function mux for PA5 (DM) and PA6 (DP) → USB function (0) */
	uint8_t mux = sys_read8(B91_GPIO_PA_FUC_H);

	mux &= ~(B91_GPIO_PA5_MUX_MASK | B91_GPIO_PA6_MUX_MASK);
	sys_write8(mux, B91_GPIO_PA_FUC_H);

	/* Disable GPIO mode (0 = peripheral mode) */
	uint8_t gpio = sys_read8(B91_GPIO_PA_GPIO_EN);

	gpio &= ~(B91_GPIO_PA5_BIT | B91_GPIO_PA6_BIT);
	sys_write8(gpio, B91_GPIO_PA_GPIO_EN);

	/* Enable input (required for USB differential signaling) */
	uint8_t input = sys_read8(B91_GPIO_PA_INPUT_EN);
	input |= (B91_GPIO_PA5_BIT | B91_GPIO_PA6_BIT);
	sys_write8(input, B91_GPIO_PA_INPUT_EN);

	/* Disable SWire/USB coexistence */
	uint8_t swire = sys_read8(B91_REG_SWIRE_USB);

	swire &= ~B91_SWIRE_USB_EN;
	sys_write8(swire, B91_REG_SWIRE_USB);
}

static void usb_clock_enable(void)
{
	uint8_t clk = sys_read8(B91_REG_CLK_EN0);

	/* Enable USB clock (BIT=1 means enabled) */
	clk |= B91_FLD_CLK0_USB_EN;
	sys_write8(clk, B91_REG_CLK_EN0);

	/* B91 reset register polarity: BIT=1 means RELEASED, BIT=0 means IN RESET.
	 * sys_init() already sets reg_rst0 = 0xFF (all released).
	 * Reset-toggle: clear bit (assert reset) → set bit (release). */
	uint8_t rst = sys_read8(B91_REG_RST0);

	sys_write8(rst & ~B91_FLD_RST0_USB, B91_REG_RST0); /* assert reset */
	sys_write8(rst | B91_FLD_RST0_USB, B91_REG_RST0);  /* release reset */
}

/* EP address helpers */
static inline uint8_t ep_num(uint8_t ep_addr)
{
	return ep_addr & 0x7F;
}

static inline bool ep_is_in(uint8_t ep_addr)
{
	return (ep_addr & 0x80) != 0;
}

static inline uint8_t ep_dir_idx(uint8_t ep_addr)
{
	return ep_is_in(ep_addr) ? 1 : 0;
}

/* ========================================================================== *
 *  ISR Forward Declaration and IRQ Setup                                      *
 * ========================================================================== */

static void usb_dc_b91_isr(const void *arg);

/* PLIC source numbers for B91 USB (from hal_telink plic.h) */
#define B91_PLIC_USB_SETUP    7   /* IRQ7_USB_CTRL_EP_SETUP */
#define B91_PLIC_USB_DATA     8   /* IRQ8_USB_CTRL_EP_DATA */
#define B91_PLIC_USB_STATUS   9   /* IRQ9_USB_CTRL_EP_STATUS */
#define B91_PLIC_USB_ENDPOINT 11  /* IRQ11_USB_ENDPOINT */
#define B91_PLIC_USB_RESET    35  /* IRQ35_USB_RESET (not 34 = IRQ34_USB_250US) */
#define B91_USB_IRQ_PRIO      2

/*
 * With CONFIG_MULTI_LEVEL_INTERRUPTS=y, raw PLIC source numbers must be
 * encoded as 2nd-level IRQs.  The PLIC's parent is the RISC-V machine
 * external interrupt (11).  Encoding: ((source + 1) << 8) | 11.
 */
#define PLIC_PARENT_IRQ 11 /* machine external interrupt */
#define PLIC_TO_ZEPHYR_IRQ(src) (((unsigned int)(src) + 1) << 8 | PLIC_PARENT_IRQ)

static const unsigned int usb_irqs[] = {
	PLIC_TO_ZEPHYR_IRQ(B91_PLIC_USB_SETUP),
	PLIC_TO_ZEPHYR_IRQ(B91_PLIC_USB_DATA),
	PLIC_TO_ZEPHYR_IRQ(B91_PLIC_USB_STATUS),
	PLIC_TO_ZEPHYR_IRQ(B91_PLIC_USB_ENDPOINT),
	PLIC_TO_ZEPHYR_IRQ(B91_PLIC_USB_RESET),
};

static void irq_connect_all(void)
{
	for (int i = 0; i < ARRAY_SIZE(usb_irqs); i++) {
		irq_connect_dynamic(usb_irqs[i], B91_USB_IRQ_PRIO,
				    usb_dc_b91_isr, NULL, 0);
		irq_enable(usb_irqs[i]);
	}
}

/* ========================================================================== *
 *  Suspend Detection (polled)                                                  *
 *                                                                             *
 *  The suspend condition (bus idle > 3 ms) is routed to PLIC source 24        *
 *  (IRQ24_USB_PWDN), which this driver intentionally does NOT connect: the    *
 *  condition is level-like while the bus stays idle and would re-fire         *
 *  continuously.  Instead a delayable work polls the w1c SUSPEND status bit.  *
 *  USB_DC_SUSPEND is delivered once per active->idle edge; delivering it      *
 *  repeatedly would let consumers that k_work_reschedule() on the event       *
 *  (e.g. ZMK's suspend re-attach watchdog) push their deadline forever.      *
 *  USB_DC_RESUME is delivered from the ISR on the first bus activity after    *
 *  a suspend (any connected USB IRQ implies traffic).                         *
 * ========================================================================== */

#define B91_USB_SUSPEND_POLL_MS 500

/* CDC bulk OUT starvation detection.  In a healthy configured device the
 * bulk OUT EP is re-armed (BUSY) within milliseconds of every packet; the
 * only long-lived un-armed states are a wedged RX chain (transfer-layer
 * teardown race — observed twice on hardware after macOS dark-wake
 * enumeration churn) or a host that filled the CDC ring with nobody
 * reading the port (rx_paused).  A 2-minute threshold plus the recovery
 * module's own holdoff makes the false-positive cost one benign re-attach. */
#define B91_USB_STARVE_TICKS (240) /* x 500 ms = 2 min */

static void suspend_poll_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(suspend_poll_work, suspend_poll_cb);

/* Poll-local trackers (poll runs only on the system workqueue). */
static uint32_t poll_last_isr_events;
static uint8_t poll_last_bits;
static uint8_t diag_bulk_out_ep;   /* CDC bulk OUT EP number (5 or 6), 0 = none */
static uint16_t out_unarmed_ticks;
static bool out_starve_fired;

/* USB workqueue liveness probe.  All usb_transfer completions run on the
 * dedicated z_usb_work_q thread; if that thread dies (stack overflow, fault)
 * CDC wedges bidirectionally with no other symptom.  Each poll tick submits
 * a marker work item to it and checks the previous one ran. */
#ifdef CONFIG_USB_WORKQUEUE
extern struct k_work_q z_usb_work_q;

static volatile bool wq_marker_ran;
static uint16_t wq_stuck_ticks;
static bool wq_stuck_logged;

static void wq_marker_cb(struct k_work *work)
{
	ARG_UNUSED(work);
	wq_marker_ran = true;
}

static K_WORK_DEFINE(wq_marker_work, wq_marker_cb);

static void wq_liveness_tick(void)
{
	if (wq_marker_ran) {
		wq_marker_ran = false;
		wq_stuck_ticks = 0;
		wq_stuck_logged = false;
		k_work_submit_to_queue(&z_usb_work_q, &wq_marker_work);
		return;
	}

	if (wq_stuck_ticks == 0) {
		/* First tick: marker may simply not have been submitted yet */
		k_work_submit_to_queue(&z_usb_work_q, &wq_marker_work);
	}
	if (wq_stuck_ticks < UINT16_MAX) {
		wq_stuck_ticks++;
	}
	/* 10 s of a parked 500 ms marker = the workqueue thread is gone */
	if (wq_stuck_ticks >= 20 && !wq_stuck_logged) {
		wq_stuck_logged = true;
		diag_ev(B91_DIAG_WQ_STUCK, 0, wq_stuck_ticks);

		/* Forensics: is the thread dead or blocked, and on WHAT?
		 * pended_on is the wait object's address — resolve it against
		 * zmk.elf to name the semaphore/FIFO the thread hangs on.
		 * The workqueue's own idle wait shows as its k_work_q queue;
		 * anything else is the deadlock culprit. */
		uintptr_t pend = (uintptr_t)z_usb_work_q.thread.base.pended_on;

		diag_ev(B91_DIAG_WQ_STATE,
			z_usb_work_q.thread.base.thread_state,
			(uint16_t)(pend & 0xFFFF));
		diag_ev(B91_DIAG_WQ_PEND2, 0, (uint16_t)(pend >> 16));
		LOG_ERR("USB workqueue not running (%u ticks, state=0x%02x, "
			"pended_on=%p)", wq_stuck_ticks,
			z_usb_work_q.thread.base.thread_state, (void *)pend);
	}
}

bool b91_usb_wq_stuck(void)
{
	return wq_stuck_logged;
}
#else
static void wq_liveness_tick(void)
{
}

bool b91_usb_wq_stuck(void)
{
	return false;
}
#endif

static void suspend_poll_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!state.attached) {
		return;
	}

	uint8_t irq_mask_reg = usb_read8(B91_USB_IRQ_MASK);
	uint8_t bits = irq_mask_reg &
		       (B91_USB_IRQ_SUSPEND_O | B91_USB_IRQ_250US_O);

	/* "quiet" = no USB interrupt (traffic/reset) since the previous tick.
	 * A latched SUSPEND_O bit observed after traffic is stale — it latched
	 * during the idle gap that precedes every enumeration (attach ->
	 * debounce -> reset) — and must NOT be reported: a false SUSPEND mutes
	 * HID (zmk_usb_hid_send_report drops reports while suspended) and lets
	 * the ZMK re-attach watchdog tear down a healthy connection. */
	uint32_t ev = state.isr_events;
	bool quiet = (ev == poll_last_isr_events);

	poll_last_isr_events = ev;

	if (bits != poll_last_bits) {
		/* Diagnostic: trace status-bit behaviour (latch/re-assert
		 * semantics are undocumented; this log is how we verify them
		 * on hardware). */
		LOG_DBG("poll: sups=%d 250us=%d lvl=%d quiet=%d ev=%u susp=%d",
			!!(bits & B91_USB_IRQ_SUSPEND_O),
			!!(bits & B91_USB_IRQ_250US_O),
			!!(irq_mask_reg & B91_USB_IRQ_250US_LVL), quiet, ev,
			state.suspended);
		poll_last_bits = bits;
	}

	if (bits) {
		/* w1c: clear observed status bits so the next tick reflects a
		 * fresh hardware assertion. Preserve mask bits (0-2). */
		usb_write8(B91_USB_IRQ_MASK, (irq_mask_reg & 0x07) | bits);
	}

	if ((bits & B91_USB_IRQ_SUSPEND_O) && quiet && !state.suspended) {
		state.suspended = true;
		LOG_DBG("USB bus idle; reporting SUSPEND");
		diag_ev(B91_DIAG_STATUS, USB_DC_SUSPEND, 0);
		if (state.status_cb) {
			state.status_cb(USB_DC_SUSPEND, NULL);
		}
	}

	wq_liveness_tick();

	/* --- CDC bulk OUT starvation watchdog ---
	 *
	 * Two dead phenotypes, both while the hardware says the host still
	 * holds us configured:
	 *   - enabled but never re-armed (transfer chain died), or
	 *   - removed from EDP_EN entirely (stack tore the EP down and no
	 *     re-enumeration followed) — the 2026-07-23 overnight wedge
	 *     escaped the enabled-only check.
	 * Gated on !suspended: while the host sleeps the bus is idle by
	 * design, and a re-attach cycle would dark-wake macOS. */
	if (diag_bulk_out_ep != 0 && !state.suspended &&
	    (usb_read8(B91_USB_HOST_CONN) & B91_USB_HOST_CONN_CONFIGURED)) {
		bool enabled = usb_read8(B91_USB_EDP_EN) &
			       b91_ep_bit(diag_bulk_out_ep);
		bool armed = enabled &&
			     (usb_read8(B91_USB_EP_CTRL(diag_bulk_out_ep)) &
			      B91_USB_EP_BUSY);

		if (armed) {
			if (out_unarmed_ticks > 1) {
				diag_ev(B91_DIAG_ARM, 1, out_unarmed_ticks);
			}
			out_unarmed_ticks = 0;
			out_starve_fired = false;
		} else {
			if (out_unarmed_ticks == 0) {
				diag_ev(B91_DIAG_ARM, enabled ? 0 : 2, 0);
			}
			if (out_unarmed_ticks < UINT16_MAX) {
				out_unarmed_ticks++;
			}
			if (out_unarmed_ticks >= B91_USB_STARVE_TICKS &&
			    !out_starve_fired) {
				out_starve_fired = true;
				diag_ev(B91_DIAG_STARVED,
					diag_bulk_out_ep | (enabled ? 0 : 0x80),
					out_unarmed_ticks);
				LOG_WRN("CDC bulk OUT EP%u dead %u ticks (%s)",
					diag_bulk_out_ep, out_unarmed_ticks,
					enabled ? "un-armed" : "disabled");
				b91_usb_cdc_starved();
			}
		}
	} else {
		out_unarmed_ticks = 0;
		out_starve_fired = false;
	}

	k_work_reschedule(&suspend_poll_work, K_MSEC(B91_USB_SUSPEND_POLL_MS));
}

/* ========================================================================== *
 *  Lifecycle Functions                                                         *
 * ========================================================================== */

int usb_dc_attach(void)
{
	if (state.attached) {
		return 0;
	}

	/*
	 * Ensure DP pullup is disabled at the start. After an SWS soft reset,
	 * the analog pullup register retains its previous state. Without an
	 * explicit disconnect, the host won't detect a new device. Hold DP
	 * low for 50ms so the host completes disconnect debounce.
	 */
	dp_pullup_en(false);
	k_msleep(50);

	/*
	 * Do NOT memset the entire state — the Zephyr USB stack registers
	 * status_cb via usb_dc_set_status_callback() BEFORE calling attach.
	 * A memset here would wipe that callback and break USB enumeration.
	 * Only reset EP-specific hardware state, preserving all callbacks.
	 */
	for (int i = 0; i < NUM_EP; i++) {
		state.ep[i].mps = 0;
		state.ep[i].type = 0;
		state.ep[i].enabled = false;
		state.ep[i].stalled = false;
		state.ep[i].buf_len = 0;
		state.ep[i].buf_offset = 0;
		state.ep[i].sram_addr = 0;
		state.ep[i].sram_size = 0;
	}

	/* 1. Enable USB clock and release from reset */

	usb_clock_enable();

	/* 2. Power on USB module */
	usb_power_on(true);

	/* 3. Configure USB pins (PA5=DM, PA6=DP) */
	usb_pin_setup();

	/* 4. Manual interrupt mode, keep AUTO_ADDR for hardware SET_ADDRESS */
	usb_write8(B91_USB_CTRL_EP_IRQ_MODE, B91_CTRL_EP_AUTO_ADDR);

	/* 5. Enable USB-level IRQ masks: RESET + SUSPEND */
	usb_write8(B91_USB_IRQ_MASK,
		   B91_USB_IRQ_RESET_MASK | B91_USB_IRQ_SUSPEND_MASK);

	/* 6. Set global EP max packet size: 64 bytes = register value 8 */
	usb_write8(B91_USB_EP_MAX_SIZE, 64 >> 3);

	/* 6b. Initialize USB SRAM bump allocator.
	 * B91 USB SRAM is only 256 bytes for EP1-EP7 (EP0 has its own
	 * 8-byte FIFO).  Buffer addresses are assigned dynamically in
	 * usb_dc_ep_configure() based on each EP's actual MPS.
	 * All EP buf_addr registers default to 0; unused EPs stay there. */
	usb_sram_next = 0;
	for (int i = 0; i < 8; i++) {
		usb_write8(B91_USB_EP_BUF_ADDR(i), 0);
	}

	/* 7. Timing calibration (required per SDK/original firmware) */
	usb_write8(B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);

	/* 8. Connect PLIC interrupts */
	irq_connect_all();

	/* 9. Enable DP pullup — device becomes visible to host */
	dp_pullup_en(true);

	/* Read back analog 0x0B to verify pullup bit (BIT(7)) is set.
	 * Also provides settling time after analog write. */
	{
		uint8_t pullup_check = analog_read(0x0B);

		LOG_DBG("DP pullup readback: 0x%02x (expect BIT(7) set)", pullup_check);
	}

	state.attached = true;
	state.suspended = false;
	poll_last_isr_events = state.isr_events;
	poll_last_bits = 0;
	diag_bulk_out_ep = 0;
	out_unarmed_ticks = 0;
	out_starve_fired = false;
	k_work_reschedule(&suspend_poll_work, K_MSEC(B91_USB_SUSPEND_POLL_MS));
	LOG_INF("B91 USB attached");

	/* Notify stack that physical connection is established.
	 * Host will see DP pullup and send bus reset next. */
	diag_ev(B91_DIAG_STATUS, USB_DC_CONNECTED, 0);
	if (state.status_cb) {
		state.status_cb(USB_DC_CONNECTED, NULL);
	}

	return 0;
}

int usb_dc_detach(void)
{
	k_work_cancel_delayable(&suspend_poll_work);

	dp_pullup_en(false);
	usb_power_on(false);

	for (int i = 0; i < ARRAY_SIZE(usb_irqs); i++) {
		irq_disable(usb_irqs[i]);
	}

	state.attached = false;
	state.suspended = false;
	diag_ev(B91_DIAG_DETACH, 0, 0);
	LOG_INF("B91 USB detached");
	return 0;
}

int usb_dc_reset(void)
{
	/* Clear all data endpoint control and buffer address registers */
	for (int i = 0; i < 8; i++) {
		usb_write8(B91_USB_EP_CTRL(i), 0);
		usb_write8(B91_USB_EP_BUF_ADDR(i), 0);
	}

	/* Reset USB SRAM allocator — EPs will be reconfigured */
	usb_sram_next = 0;

	/* Reset driver state for all EPs (keep callbacks and MPS config) */
	for (int i = 0; i < NUM_EP; i++) {
		state.ep[i].stalled = false;
		state.ep[i].buf_len = 0;
		state.ep[i].buf_offset = 0;
		state.ep[i].sram_addr = 0;
		state.ep[i].sram_size = 0;
	}

	/* Reset DATA toggle to DATA0 for all endpoints (USB spec) */
	memset(state.ep_in_toggle, 0, sizeof(state.ep_in_toggle));
	memset(state.ep_in_pending, 0, sizeof(state.ep_in_pending));

	return 0;
}

int usb_dc_set_address(const uint8_t addr)
{
	/*
	 * Hardware auto-handles SET_ADDRESS via AUTO_ADDR bit in
	 * reg_ctrl_ep_irq_mode. The address is applied automatically
	 * after the STATUS phase completes. No software action needed.
	 */
	LOG_DBG("SET_ADDRESS %u (hardware auto)", addr);
	return 0;
}

void usb_dc_set_status_callback(const usb_dc_status_callback cb)
{
	state.status_cb = cb;
}

int usb_dc_wakeup_request(void)
{
	LOG_WRN("Remote wakeup not implemented");
	return -ENOTSUP;
}

/* ========================================================================== *
 *  ISR                                                                        *
 * ========================================================================== */

static void usb_dc_b91_isr(const void *arg)
{
	ARG_UNUSED(arg);

	uint8_t irq_mask_reg = usb_read8(B91_USB_IRQ_MASK);
	uint8_t ctrl_ep_irq = usb_read8(B91_USB_CTRL_EP_IRQ_STA);
	uint8_t ep_irq = usb_read8(B91_USB_EP_IRQ_STATUS);

	/* --- Resume detection --- */
	/* This ISR only runs on the connected traffic/reset IRQs, so any entry
	 * means the bus is active again. Report RESUME first so a RESET in the
	 * same pass is the final status the stack observes. */
	state.isr_events++;
	if (state.suspended) {
		state.suspended = false;
		LOG_DBG("USB bus active again; reporting RESUME");
		diag_ev(B91_DIAG_STATUS, USB_DC_RESUME, 0);
		if (state.status_cb) {
			state.status_cb(USB_DC_RESUME, NULL);
		}
	}

	/* --- Bus Reset (highest priority) --- */
	if (irq_mask_reg & B91_USB_IRQ_RESET_O) {
		/* Clear ONLY RESET_O status bit (w1c). Preserve mask bits (0-2),
		 * don't accidentally clear other pending status bits (6-7). */
		usb_write8(B91_USB_IRQ_MASK,
			   (irq_mask_reg & 0x07) | B91_USB_IRQ_RESET_O);

		/* Clear all EP control registers (matches original firmware) */
		for (int i = 0; i < 8; i++) {
			usb_write8(B91_USB_EP_CTRL(i), 0);
		}

		/*
		 * Clear ALL pending EP IRQs. During bus reset, partially
		 * received packets may leave stale pending IRQs. If these
		 * are processed after the reset handler cancels transfers,
		 * the ISR would read 0 bytes from empty FIFOs and the
		 * transfer layer would interpret it as a ZLP completion,
		 * permanently losing the transfer slot.
		 */
		usb_write8(B91_USB_EP_IRQ_STATUS, 0xFF);
		ep_irq = 0;

		/*
		 * B91 hardware quirk: OUT endpoints must be armed (BUSY)
		 * immediately after reset, otherwise the hardware permanently
		 * NAKs OUT tokens and out_irq is never generated.
		 */
		usb_write8(B91_USB_EP_CTRL(5), B91_USB_EP_BUSY);
		usb_write8(B91_USB_EP_CTRL(6), B91_USB_EP_BUSY);

		/* Reset EP state */
		for (int i = 0; i < NUM_EP; i++) {
			state.ep[i].stalled = false;
			state.ep[i].buf_len = 0;
			state.ep[i].buf_offset = 0;
		}

		memset(state.ep_in_toggle, 0, sizeof(state.ep_in_toggle));
		memset(state.ep_in_pending, 0, sizeof(state.ep_in_pending));

		diag_ev(B91_DIAG_STATUS, USB_DC_RESET, 0);
		if (state.status_cb) {
			state.status_cb(USB_DC_RESET, NULL);
		}
	}

	/* NOTE: bus suspend is NOT handled here. The SUSPEND status routes to
	 * PLIC source 24 (IRQ24_USB_PWDN), which is never connected, so this
	 * ISR could only ever observe it opportunistically while other traffic
	 * fired — i.e. exactly when the bus is NOT suspended. Suspend delivery
	 * lives in suspend_poll_cb() above. */

	/* --- EP0 SETUP --- */
	if (ctrl_ep_irq & B91_CTRL_EP_IRQ_SETUP) {
		usb_write8(B91_USB_CTRL_EP_IRQ_STA, B91_CTRL_EP_IRQ_SETUP);

		/* Timing calibration (mandatory per SDK) */
		usb_write8(B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);

		/* Clear stale stall from any previous transfer */
		state.ep[0].stalled = false;

		/* Read 8-byte SETUP packet from EP0 FIFO into staging buffer */
		usb_write8(B91_USB_CTRL_EP_PTR, 0);
		for (int i = 0; i < 8; i++) {
			state.ep[0].buf[i] = usb_read8(B91_USB_CTRL_EP_DAT);
		}
		state.ep[0].buf_len = 8;
		state.ep[0].buf_offset = 0;

		/* Track transfer direction for DATA phase handling */
		state.ep0_in_transfer = (state.ep[0].buf[0] & 0x80) != 0;

		LOG_DBG("SETUP: bmRT=0x%02x bReq=0x%02x wVal=0x%04x wLen=%u",
			state.ep[0].buf[0], state.ep[0].buf[1],
			(uint16_t)((state.ep[0].buf[3] << 8) | state.ep[0].buf[2]),
			(uint16_t)((state.ep[0].buf[7] << 8) | state.ep[0].buf[6]));

		/* Trace enumeration-shaping STANDARD requests only (SET_ADDRESS,
		 * SET_CONFIGURATION, SET_INTERFACE) — GET_DESCRIPTOR bursts would
		 * flood the 64-entry ring, and class requests share bRequest
		 * values (HID SET_REPORT is also 9). */
		if ((state.ep[0].buf[0] & 0x60) == 0 &&
		    (state.ep[0].buf[1] == 5 || state.ep[0].buf[1] == 9 ||
		     state.ep[0].buf[1] == 11)) {
			diag_ev(B91_DIAG_SETUP, state.ep[0].buf[1],
				(uint16_t)((state.ep[0].buf[3] << 8) |
					   state.ep[0].buf[2]));
		}

		if (state.ep_cb[0][0]) {
			state.ep_cb[0][0](0x00, USB_DC_EP_SETUP);
		}

		/*
		 * Always ACK or STALL after SETUP callback, matching the
		 * original firmware pattern.  For IN transfers, ep_write()
		 * already set DAT_ACK; writing it again is harmless.  For
		 * OUT transfers this arms data-phase reception.  For
		 * zero-length OUT (SET_ADDRESS, etc.) the hardware needs
		 * DAT_ACK to transition to the STATUS phase.
		 */
		if (state.ep[0].stalled) {
			usb_write8(B91_USB_CTRL_EP_CTRL, B91_EP_DAT_STALL);
		} else {
			usb_write8(B91_USB_CTRL_EP_CTRL, B91_EP_DAT_ACK);
		}
	}

	/* --- EP0 DATA phase --- */
	if (ctrl_ep_irq & B91_CTRL_EP_IRQ_DATA) {
		usb_write8(B91_USB_CTRL_EP_IRQ_STA, B91_CTRL_EP_IRQ_DATA);
		usb_write8(B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);

		LOG_DBG("EP0 DATA %s", state.ep0_in_transfer ? "IN" : "OUT");

		if (!state.ep0_in_transfer) {
			/*
			 * OUT transfer: read received data from EP0 FIFO.
			 * The pointer holds the number of bytes received.
			 */
			uint8_t count = usb_read8(B91_USB_CTRL_EP_PTR);
			uint8_t len = MIN(count, B91_EP0_MPS);

			usb_write8(B91_USB_CTRL_EP_PTR, 0);
			for (uint8_t i = 0; i < len; i++) {
				state.ep[0].buf[i] =
					usb_read8(B91_USB_CTRL_EP_DAT);
			}
			state.ep[0].buf_len = len;
			state.ep[0].buf_offset = 0;

			if (state.ep_cb[0][0]) {
				state.ep_cb[0][0](0x00, USB_DC_EP_DATA_OUT);
			}

			/* Re-ACK for multi-packet OUT transfers.
			 * Skip if callback stalled EP0. */
			if (!state.ep[0].stalled) {
				usb_write8(B91_USB_CTRL_EP_CTRL,
					   B91_EP_DAT_ACK);
			}
		} else {
			/*
			 * IN transfer: previous ep_write data was sent to host.
			 * Do NOT read from FIFO — it contains echo of sent data.
			 * Fire DATA_IN so the stack can queue the next chunk.
			 */
			if (state.ep_cb[1][0]) {
				state.ep_cb[1][0](0x80, USB_DC_EP_DATA_IN);
			}
		}
	}

	/* --- EP0 STATUS phase --- */
	if (ctrl_ep_irq & B91_CTRL_EP_IRQ_STA) {
		usb_write8(B91_USB_CTRL_EP_IRQ_STA, B91_CTRL_EP_IRQ_STA);
		usb_write8(B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);

		/* ACK the status phase */
		usb_write8(B91_USB_CTRL_EP_CTRL, B91_EP_STA_ACK);

		LOG_DBG("EP0 STATUS complete");

		/* Fire DATA_IN to signal status phase completion */
		if (state.ep_cb[1][0]) {
			state.ep_cb[1][0](0x80, USB_DC_EP_DATA_IN);
		}
	}

	/* --- Data EP IRQs --- */
	for (uint8_t i = 1; i <= 8; i++) {
		uint8_t bit = b91_ep_bit(i);

		if (!(ep_irq & bit)) {
			continue;
		}

		/* Clear this EP's IRQ */
		usb_write8(B91_USB_EP_IRQ_STATUS, bit);

		if (state.ep_cb[0][i]) {
			/* OUT endpoint: read data from FIFO into staging buf */
			uint8_t ptr_val = usb_read8(B91_USB_EP_PTR(i));

			usb_write8(B91_USB_EP_PTR(i), 0);

			uint16_t len = MIN(ptr_val, sizeof(state.ep[i].buf));

			if (len == 0) {
				/*
				 * Spurious 0-byte IRQ: B91 hardware fires an
				 * extra EP IRQ when BUSY is re-armed by
				 * read_continue. Just re-arm and ignore.
				 */
				usb_write8(B91_USB_EP_CTRL(i),
					   B91_USB_EP_BUSY);
				continue;
			}

			for (uint16_t j = 0; j < len; j++) {
				state.ep[i].buf[j] =
					usb_read8(B91_USB_EP_DAT(i));
			}
			state.ep[i].buf_len = len;
			state.ep[i].buf_offset = 0;
			LOG_DBG("EP%u OUT: %u bytes", i, len);

			state.ep_cb[0][i](i, USB_DC_EP_DATA_OUT);
		} else {
			/* IN endpoint: transfer complete.
			 *
			 * B91 hardware quirk: when an IN EP is enabled but
			 * NOT BUSY, the hardware responds to host IN tokens
			 * with empty DATA packets instead of NAK.  The host
			 * (xHCI) advances its DATA toggle on every completed
			 * transaction regardless of success/error status.
			 *
			 * Fix: the ISR tracks the host's toggle by toggling
			 * ep_in_toggle on EVERY completion (false or real).
			 * For false completions, we also update EP_CTRL with
			 * the correct PID so subsequent false responses match
			 * the host's expected toggle (eliminating EPROTO).
			 */
			state.ep_in_toggle[i] ^= 1;

			if (!state.ep_in_pending[i]) {
				/* False completion: update PID for next
				 * false response to match host's toggle */
				usb_write8(B91_USB_EP_CTRL(i),
					   state.ep_in_toggle[i]
					   ? B91_USB_EP_DAT1
					   : B91_USB_EP_DAT0);
				continue;
			}
			state.ep_in_pending[i] = false;

			/* Set PID for idle period false responses */
			usb_write8(B91_USB_EP_CTRL(i),
				   state.ep_in_toggle[i]
				   ? B91_USB_EP_DAT1
				   : B91_USB_EP_DAT0);

			if (state.ep_cb[1][i]) {
				state.ep_cb[1][i](0x80 | i,
						 USB_DC_EP_DATA_IN);
			}
		}
	}
}

/* ========================================================================== *
 *  Endpoint Management                                                        *
 * ========================================================================== */

int usb_dc_ep_check_cap(const struct usb_dc_ep_cfg_data *const cfg)
{
	uint8_t n = ep_num(cfg->ep_addr);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	/* EP0 is always control, bidirectional */
	if (n == 0) {
		if (cfg->ep_type != USB_DC_EP_CONTROL) {
			return -EINVAL;
		}
		if (cfg->ep_mps > B91_EP0_MPS) {
			return -EINVAL;
		}
		return 0;
	}

	/* Data EPs: validate direction matches hardware */
	if (ep_is_in(cfg->ep_addr)) {
		/* IN: EP1-4, EP7-8 only */
		if (n == 5 || n == 6) {
			return -EINVAL;
		}
	} else {
		/* OUT: EP5-6 only */
		if (n != 5 && n != 6) {
			return -EINVAL;
		}
	}

	if (cfg->ep_mps > 64) {
		return -EINVAL;
	}

	return 0;
}

int usb_dc_ep_configure(const struct usb_dc_ep_cfg_data *const cfg)
{
	uint8_t n = ep_num(cfg->ep_addr);
	int ret;

	ret = usb_dc_ep_check_cap(cfg);
	if (ret) {
		return ret;
	}

	state.ep[n].mps = cfg->ep_mps;
	state.ep[n].type = cfg->ep_type;

	/*
	 * Note: EP_MAX_SIZE is a GLOBAL register (applies to all data EPs).
	 * It is set once to 64 bytes in usb_dc_attach() and must not be
	 * overwritten here — a later ep_configure with a smaller MPS would
	 * shrink the limit for every endpoint (e.g. HID interrupt EP after
	 * CDC ACM bulk EP would break bulk transfers).
	 */

	/*
	 * Allocate USB SRAM buffer for this EP.
	 * B91 USB SRAM = 256 bytes (EP1-7 shared).
	 *
	 * OUT EPs: must allocate 64B (global ep_max_size) because the host
	 * controls packet size and hardware writes up to max_size bytes.
	 *
	 * IN EPs: firmware controls write count, so we can allocate just
	 * enough for the EP's actual MPS.  Round up to 8-byte alignment
	 * for hardware FIFO alignment safety.
	 */
	if (n >= 1 && n <= 7) {
		bool is_out = !(cfg->ep_addr & 0x80); /* bit 7 = IN */
		uint8_t buf_size = is_out ? 64 : ROUND_UP(cfg->ep_mps, 8);

		if (is_out && cfg->ep_type == USB_DC_EP_BULK) {
			diag_bulk_out_ep = n;
		}

		/*
		 * Allocation must be idempotent: the stack re-runs
		 * usb_dc_ep_configure() for every EP on EVERY SET_CONFIGURATION,
		 * and a host can re-enumerate without a detach/attach cycle
		 * (macOS does on sleep/wake).  usb_sram_next is only reset in
		 * attach, so bump-allocating again here would leak the pool:
		 * the second enumeration starts where the first ended and the
		 * 64B bulk EPs no longer fit.  set_endpoint() failures are
		 * non-fatal to SET_CONFIGURATION (a later EP's success
		 * overwrites `found`), so the host sees a configured device
		 * with silently dead endpoints — CDC ACM mute, HID alive.
		 * Reuse the EP's existing buffer instead.
		 */
		if (state.ep[n].sram_size >= buf_size) {
			usb_write8(B91_USB_EP_BUF_ADDR(n), state.ep[n].sram_addr);
			diag_ev(B91_DIAG_EP_CFG, cfg->ep_addr,
				state.ep[n].sram_addr);
			LOG_DBG("EP 0x%02x: USB SRAM [%u..%u) reused",
				cfg->ep_addr, state.ep[n].sram_addr,
				(uint16_t)state.ep[n].sram_addr + buf_size);
		} else {
			uint8_t alloc_end = usb_sram_next + buf_size;

			if (alloc_end > USB_SRAM_SIZE) {
				LOG_ERR("EP 0x%02x: USB SRAM exhausted! need %u, "
					"have %u of %u free",
					cfg->ep_addr, buf_size,
					USB_SRAM_SIZE - usb_sram_next, USB_SRAM_SIZE);
				diag_ev(B91_DIAG_EP_CFG, cfg->ep_addr, 0xFFFF);
				return -ENOMEM;
			}

			usb_write8(B91_USB_EP_BUF_ADDR(n), usb_sram_next);
			LOG_INF("EP 0x%02x: USB SRAM [%u..%u) (%uB %s)",
				cfg->ep_addr, usb_sram_next,
				(uint16_t)usb_sram_next + buf_size, buf_size,
				is_out ? "OUT" : "IN");
			diag_ev(B91_DIAG_EP_CFG, cfg->ep_addr, usb_sram_next);
			state.ep[n].sram_addr = usb_sram_next;
			state.ep[n].sram_size = buf_size;
			usb_sram_next = alloc_end;
		}
	}

	LOG_INF("EP 0x%02x configured: MPS=%u type=%u",
		cfg->ep_addr, cfg->ep_mps, cfg->ep_type);
	return 0;
}

int usb_dc_ep_enable(const uint8_t ep)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	state.ep[n].enabled = true;

	/* EP0 is always enabled in hardware */
	if (n == 0) {
		return 0;
	}

	/* Enable endpoint in hardware */
	uint8_t en = usb_read8(B91_USB_EDP_EN);

	en |= b91_ep_bit(n);
	usb_write8(B91_USB_EDP_EN, en);

	/* Clear any stale pending IRQ before enabling the mask.
	 * Without this, a leftover pending IRQ fires immediately
	 * on mask enable, causing a spurious 0-byte callback that
	 * completes the transfer slot before real data arrives. */
	usb_write8(B91_USB_EP_IRQ_STATUS, b91_ep_bit(n));

	/* Enable IRQ for this endpoint */
	uint8_t irq_mask = usb_read8(B91_USB_EP_IRQ_MASK);

	irq_mask |= b91_ep_bit(n);
	usb_write8(B91_USB_EP_IRQ_MASK, irq_mask);

	/* Arm OUT endpoints for reception (set BUSY).
	 * B91 requires this before the endpoint can accept data. */
	if (!ep_is_in(ep)) {
		usb_write8(B91_USB_EP_CTRL(n), B91_USB_EP_BUSY);
	}

	diag_ev(B91_DIAG_EP_EN, ep, 0);
	LOG_INF("EP 0x%02x enabled (edp_en=0x%02x irq_mask=0x%02x)",
		ep, usb_read8(B91_USB_EDP_EN), usb_read8(B91_USB_EP_IRQ_MASK));
	return 0;
}

int usb_dc_ep_disable(const uint8_t ep)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	state.ep[n].enabled = false;

	if (n == 0) {
		return 0;
	}

	uint8_t en = usb_read8(B91_USB_EDP_EN);

	en &= ~b91_ep_bit(n);
	usb_write8(B91_USB_EDP_EN, en);

	uint8_t irq_mask = usb_read8(B91_USB_EP_IRQ_MASK);

	irq_mask &= ~b91_ep_bit(n);
	usb_write8(B91_USB_EP_IRQ_MASK, irq_mask);

	diag_ev(B91_DIAG_EP_DIS, ep, 0);
	LOG_DBG("EP 0x%02x disabled", ep);
	return 0;
}

int usb_dc_ep_set_callback(const uint8_t ep, const usb_dc_ep_callback cb)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	state.ep_cb[ep_dir_idx(ep)][n] = cb;
	LOG_DBG("EP 0x%02x set_callback: dir=%d cb=%p", ep, ep_dir_idx(ep), cb);
	return 0;
}

int usb_dc_ep_set_stall(const uint8_t ep)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	if (n == 0) {
		usb_write8(B91_USB_CTRL_EP_CTRL, B91_EP_DAT_STALL);
	} else {
		usb_write8(B91_USB_EP_CTRL(n), B91_USB_EP_STALL);
	}

	state.ep[n].stalled = true;
	LOG_DBG("EP 0x%02x stalled", ep);
	return 0;
}

int usb_dc_ep_clear_stall(const uint8_t ep)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	if (n > 0) {
		usb_write8(B91_USB_EP_CTRL(n), 0);
	}

	state.ep[n].stalled = false;
	state.ep_in_toggle[n] = 0; /* USB spec: toggle resets after CLEAR_FEATURE */
	LOG_DBG("EP 0x%02x stall cleared", ep);
	return 0;
}

int usb_dc_ep_is_stalled(const uint8_t ep, uint8_t *const stalled)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP || !stalled) {
		return -EINVAL;
	}

	*stalled = state.ep[n].stalled ? 1 : 0;
	return 0;
}

int usb_dc_ep_halt(const uint8_t ep)
{
	return usb_dc_ep_set_stall(ep);
}

int usb_dc_ep_flush(const uint8_t ep)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	if (n == 0) {
		usb_write8(B91_USB_CTRL_EP_PTR, 0);
	} else {
		usb_write8(B91_USB_EP_PTR(n), 0);
	}

	state.ep[n].buf_len = 0;
	state.ep[n].buf_offset = 0;
	return 0;
}

int usb_dc_ep_mps(uint8_t ep)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return 0;
	}

	return state.ep[n].mps;
}

/* ========================================================================== *
 *  Data Transfer                                                              *
 * ========================================================================== */

int usb_dc_ep_write(const uint8_t ep, const uint8_t *const data,
		    const uint32_t data_len, uint32_t *const ret_bytes)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP || !ep_is_in(ep)) {
		return -EINVAL;
	}

	if (n == 0) {
		/* EP0 IN: write to control endpoint FIFO */
		uint32_t len = MIN(data_len, B91_EP0_MPS);

		usb_write8(B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);
		usb_write8(B91_USB_CTRL_EP_PTR, 0);
		for (uint32_t i = 0; i < len; i++) {
			usb_write8(B91_USB_CTRL_EP_DAT, data[i]);
		}
		/* ACK the data phase */
		usb_write8(B91_USB_CTRL_EP_CTRL, B91_EP_DAT_ACK);

		if (ret_bytes) {
			*ret_bytes = len;
		}
		return 0;
	}

	/* Check if USB host has configured the device before writing data EPs.
	 * SDK usbhw_get_host_conn_status() checks BIT(7) of reg_usb_host_conn:
	 * "Set Configuration will set BIT(7) to 1." */
	if (!(usb_read8(B91_USB_HOST_CONN) & B91_USB_HOST_CONN_CONFIGURED)) {
		return -ENODEV;
	}

	/* Data EP IN: wait for endpoint to become free.
	 *
	 * Zephyr's usb_write() retries on -EAGAIN with k_yield() between
	 * attempts (CONFIG_USB_NUMOF_EP_WRITE_RETRIES=3 by default).
	 * k_yield() may return almost immediately if no other thread is
	 * ready, so 3 retries can complete in < 300µs — not enough time
	 * for the host to ACK a USB IN packet (~1ms at full-speed USB).
	 *
	 * Instead, busy-wait here for up to 2ms with 10µs steps.  USB IRQs
	 * remain enabled during k_busy_wait(), so the IN-complete IRQ fires
	 * and clears BUSY within ~1ms; the loop then exits immediately.
	 */
	for (int _retry = 0; _retry < 200; _retry++) {
		if (!(usb_read8(B91_USB_EP_CTRL(n)) & B91_USB_EP_BUSY)) {
			break;
		}
		k_busy_wait(10);
	}

	if (usb_read8(B91_USB_EP_CTRL(n)) & B91_USB_EP_BUSY) {
		LOG_DBG("EP%u IN write: BUSY timeout", n);
		return -EAGAIN;
	}

	uint32_t len = MIN(data_len, state.ep[n].mps);


	/* Arm IN transfer with correct DATA toggle.
	 *
	 * B91 hardware quirk: when an IN EP is enabled but not BUSY,
	 * the hardware responds to host IN tokens with stale empty
	 * DATA packets.  The ISR tracks the host's toggle by toggling
	 * ep_in_toggle on every completion (false or real), and updates
	 * EP_CTRL with the correct PID so false responses match.
	 *
	 * Here under irq_lock we:
	 *  1. Drain any false completions the ISR hasn't serviced yet
	 *  2. Write data and arm EP with the synced toggle
	 *  3. Post-arm check catches a false completion during FIFO
	 *     write (~3µs window) and corrects the toggle
	 */
	{
		unsigned int key = irq_lock();
		uint8_t bit = b91_ep_bit(n);

		/* 1. Drain pending false completions */
		while (usb_read8(B91_USB_EP_IRQ_STATUS) & bit) {
			usb_write8(B91_USB_EP_IRQ_STATUS, bit);
			state.ep_in_toggle[n] ^= 1;
		}

		uint8_t toggle = state.ep_in_toggle[n];

		/* 2. Write data to FIFO and arm */
		usb_write8(B91_USB_EP_PTR(n), 0);
		for (uint32_t i = 0; i < len; i++) {
			usb_write8(B91_USB_EP_DAT(n), data[i]);
		}

		state.ep_in_pending[n] = true;
		usb_write8(B91_USB_EP_CTRL(n), B91_USB_EP_BUSY |
			   (toggle ? B91_USB_EP_DAT1 : B91_USB_EP_DAT0));

		/* 3. Post-arm check: catch false completion during FIFO
		 *    write.  EP is now BUSY → no new false completions.
		 *    Any set IRQ bit is from before the arm. */
		if (usb_read8(B91_USB_EP_IRQ_STATUS) & bit) {
			state.ep_in_toggle[n] ^= 1;
			toggle = state.ep_in_toggle[n];
			usb_write8(B91_USB_EP_CTRL(n), B91_USB_EP_BUSY |
				   (toggle ? B91_USB_EP_DAT1
					   : B91_USB_EP_DAT0));
		}
		usb_write8(B91_USB_EP_IRQ_STATUS, bit);

		irq_unlock(key);
	}

	if (ret_bytes) {
		*ret_bytes = len;
	}

	return 0;
}

static int ep_read_ex(uint8_t ep, uint8_t *data, uint32_t max_data_len,
		      uint32_t *read_bytes, bool wait)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	struct usb_dc_b91_ep *e = &state.ep[n];

	/* If data is NULL and max_data_len is 0, return available bytes */
	if (!data && !max_data_len) {
		if (read_bytes) {
			*read_bytes = e->buf_len - e->buf_offset;
		}
		return 0;
	}

	uint32_t avail = e->buf_len - e->buf_offset;
	uint32_t len = MIN(avail, max_data_len);

	if (data && len > 0) {
		memcpy(data, e->buf + e->buf_offset, len);
		e->buf_offset += len;
	}

	if (read_bytes) {
		*read_bytes = len;
	}

	/* If all data consumed and not in wait mode, re-arm the endpoint */
	if (!wait && e->buf_offset >= e->buf_len) {
		e->buf_len = 0;
		e->buf_offset = 0;

		if (n > 0) {
			/* Re-arm OUT endpoint for next packet */
			usb_write8(B91_USB_EP_CTRL(n), B91_USB_EP_BUSY);
		}
		/*
		 * EP0: Do NOT auto-ACK here. After reading SETUP data,
		 * a premature DAT_ACK would arm the hardware to respond
		 * to the host's IN token with stale FIFO data before
		 * ep_write has written the actual response (~10µs race).
		 *
		 * Instead, EP0 ACK is handled explicitly:
		 *  - IN transfers: usb_dc_ep_write() sets DAT_ACK
		 *  - OUT transfers: ISR SETUP handler sets DAT_ACK
		 *    after checking direction bit in bmRequestType
		 *  - Multi-packet OUT: ISR DATA handler re-ACKs
		 */
	}

	return 0;
}

int usb_dc_ep_read(const uint8_t ep, uint8_t *const data,
		   const uint32_t max_data_len, uint32_t *const read_bytes)
{
	return ep_read_ex(ep, data, max_data_len, read_bytes, false);
}

int usb_dc_ep_read_wait(uint8_t ep, uint8_t *data, uint32_t max_data_len,
			uint32_t *read_bytes)
{
	return ep_read_ex(ep, data, max_data_len, read_bytes, true);
}

int usb_dc_ep_read_continue(uint8_t ep)
{
	uint8_t n = ep_num(ep);

	if (n >= NUM_EP) {
		return -EINVAL;
	}

	state.ep[n].buf_len = 0;
	state.ep[n].buf_offset = 0;

	if (n == 0) {
		usb_write8(B91_USB_CTRL_EP_CTRL, B91_EP_DAT_ACK);
	} else {
		/* Re-arm OUT endpoint */
		usb_write8(B91_USB_EP_CTRL(n), B91_USB_EP_BUSY);
	}

	return 0;
}
