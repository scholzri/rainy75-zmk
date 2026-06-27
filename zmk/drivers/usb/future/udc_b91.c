/*
 * Copyright (c) 2025 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telink B91 USB Device Controller Driver
 *
 * Implements the Zephyr UDC API for the B91 (TLSR9518/9511) SoC's built-in
 * USB 2.0 full-speed device controller. The controller has 9 endpoints:
 *   - EP0: bidirectional control, 8-byte FIFO (hardware limit)
 *   - EP1-4, EP7-8: IN capable (interrupt/bulk)
 *   - EP5-6: OUT capable
 *
 * Key hardware characteristics:
 *   - No DMA: all data transfers are byte-by-byte via FIFO registers
 *   - Software DATA0/DATA1 toggle management for data endpoints
 *   - 7 separate PLIC interrupt vectors for USB events
 *   - Analog register interface for DP pullup control
 *
 * v1 scope: EP0 control transfers + EP IN interrupt transfers.
 * Deferred: suspend/resume, remote wakeup, ISO, bulk, EP OUT beyond EP0.
 */

#include "udc_b91.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/util.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

/* The UDC common header provides helper functions like udc_ctrl_alloc(),
 * udc_submit_event(), udc_ctrl_update_stage(), etc. */
#include "udc_common.h"

LOG_MODULE_REGISTER(udc_b91, CONFIG_UDC_DRIVER_LOG_LEVEL);

#define DT_DRV_COMPAT telink_b91_udc

/* ========================================================================== *
 *  Driver Data Structures                                                     *
 * ========================================================================== */

struct udc_b91_config {
	uintptr_t base;                    /* USB register base from DT */
	size_t num_of_eps;                 /* From DT num-bidir-endpoints */
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
	void (*irq_connect)(void);         /* Connects all 5 PLIC IRQs */
	k_thread_stack_t *thread_stk;
	size_t thread_stk_sz;
};

struct udc_b91_data {
	struct k_thread thread_data;
	struct k_sem irq_sem;              /* ISR → thread signaling */
	volatile uint32_t irq_pending;     /* Bitmask of pending IRQ events */
	uint8_t ep_data_toggle[9];         /* DATA0/DATA1 per EP (indices 0-8) */
	uint8_t setup_pkt[8];             /* SETUP packet captured in ISR */
};

/* ========================================================================== *
 *  Register Access Helpers                                                    *
 * ========================================================================== */

static inline uint8_t b91_read8(const struct udc_b91_config *cfg, uint16_t off)
{
	return sys_read8(cfg->base + off);
}

static inline void b91_write8(const struct udc_b91_config *cfg,
			      uint16_t off, uint8_t val)
{
	sys_write8(val, cfg->base + off);
}

/* ========================================================================== *
 *  Analog Register Access                                                     *
 *                                                                             *
 *  B91 analog registers are accessed via a serial interface, not directly     *
 *  memory-mapped. Protocol:                                                   *
 *    1. Wait for not busy                                                     *
 *    2. Write target address to analog_addr register                          *
 *    3. For write: write data to analog_data, trigger write                   *
 *    4. For read: trigger read, wait, read data from analog_data              *
 * ========================================================================== */

static void b91_analog_wait(void)
{
	while (sys_read8(B91_ANALOG_STATUS_REG) & B91_ANALOG_STATUS_BUSY) {
		/* Busy-wait; analog ops complete in ~10 cycles */
	}
}

static uint8_t b91_analog_read(uint8_t addr)
{
	b91_analog_wait();
	sys_write8(addr, B91_ANALOG_ADDR_REG);
	sys_write8(B91_ANALOG_CTRL_READ, B91_ANALOG_CTRL_REG);
	b91_analog_wait();
	return sys_read8(B91_ANALOG_DATA_REG);
}

static void b91_analog_write(uint8_t addr, uint8_t data)
{
	b91_analog_wait();
	sys_write8(addr, B91_ANALOG_ADDR_REG);
	sys_write8(data, B91_ANALOG_DATA_REG);
	sys_write8(B91_ANALOG_CTRL_WRITE, B91_ANALOG_CTRL_REG);
	b91_analog_wait();
}

/* ========================================================================== *
 *  USB Pin and Power Control                                                  *
 * ========================================================================== */

/** Enable/disable the 1.5K pull-up on DP (signals full-speed device to host) */
static void b91_dp_pullup_en(bool enable)
{
	uint8_t val = b91_analog_read(B91_ANALOG_REG_USB_DP);
	if (enable) {
		val |= B91_ANALOG_DP_PULLUP;
	} else {
		val &= ~B91_ANALOG_DP_PULLUP;
	}
	b91_analog_write(B91_ANALOG_REG_USB_DP, val);
}

/** Power on/off the USB module via analog register */
static void b91_usb_power_on(bool enable)
{
	uint8_t val = b91_analog_read(B91_ANALOG_REG_USB_PWR);
	if (enable) {
		val &= ~B91_ANALOG_USB_PWR_DN;
	} else {
		val |= B91_ANALOG_USB_PWR_DN;
	}
	b91_analog_write(B91_ANALOG_REG_USB_PWR, val);
}

/** Configure PA5 (DM) and PA6 (DP) for USB function */
static void b91_usb_pin_setup(void)
{
	/* Clear GPIO function mux bits for PA5 and PA6 (set to peripheral mode) */
	uint8_t mux = sys_read8(B91_GPIO_PA5_FUNC_MUX);
	mux &= ~(B91_GPIO_PA5_MUX_MASK | B91_GPIO_PA6_MUX_MASK);
	sys_write8(mux, B91_GPIO_PA5_FUNC_MUX);

	/* Disable GPIO function for PA5 and PA6 (0 = peripheral mode) */
	uint8_t func = sys_read8(B91_GPIO_PA_FUNC_EN);
	func &= ~(B91_GPIO_PA5_BIT | B91_GPIO_PA6_BIT);
	sys_write8(func, B91_GPIO_PA_FUNC_EN);

	/* Enable input on PA5 and PA6 (required for USB differential signaling) */
	uint8_t input = sys_read8(B91_GPIO_PA_INPUT_EN);
	input |= (B91_GPIO_PA5_BIT | B91_GPIO_PA6_BIT);
	sys_write8(input, B91_GPIO_PA_INPUT_EN);

	/* Disable SWire/USB coexistence (not needed for normal USB operation) */
	uint8_t swire = sys_read8(B91_REG_SWIRE_USB);
	swire &= ~B91_SWIRE_USB_EN;
	sys_write8(swire, B91_REG_SWIRE_USB);
}

/** Enable USB clock and release USB from reset */
static void b91_usb_clock_enable(void)
{
	/* Release USB from reset */
	uint8_t rst = sys_read8(B91_REG_RST0);
	rst &= ~B91_FLD_RST0_USB;
	sys_write8(rst, B91_REG_RST0);

	/* Enable USB clock */
	uint8_t clk = sys_read8(B91_REG_CLK_EN0);
	clk |= B91_FLD_CLK0_USB_EN;
	sys_write8(clk, B91_REG_CLK_EN0);
}

/* ========================================================================== *
 *  EP0 FIFO Operations                                                        *
 * ========================================================================== */

/** Read bytes from the EP0 control endpoint FIFO */
static void b91_ctrl_ep_read(const struct udc_b91_config *cfg,
			     uint8_t *buf, uint8_t len)
{
	b91_write8(cfg, B91_USB_CTRL_EP_PTR, 0);
	for (uint8_t i = 0; i < len; i++) {
		buf[i] = b91_read8(cfg, B91_USB_CTRL_EP_DAT);
	}
}

/** Write bytes to the EP0 control endpoint FIFO */
static void b91_ctrl_ep_write(const struct udc_b91_config *cfg,
			      const uint8_t *buf, uint8_t len)
{
	b91_write8(cfg, B91_USB_CTRL_EP_PTR, 0);
	for (uint8_t i = 0; i < len; i++) {
		b91_write8(cfg, B91_USB_CTRL_EP_DAT, buf[i]);
	}
}

/* ========================================================================== *
 *  Data EP Operations                                                         *
 * ========================================================================== */

/** Write data to a data endpoint FIFO and trigger transmission */
static void b91_ep_write_fifo(const struct udc_b91_config *cfg,
			      uint8_t ep_num, const uint8_t *buf, uint16_t len,
			      uint8_t data_toggle)
{
	b91_write8(cfg, B91_USB_EP_PTR(ep_num), 0);
	for (uint16_t i = 0; i < len; i++) {
		b91_write8(cfg, B91_USB_EP_DAT(ep_num), buf[i]);
	}

	/* Set BUSY to trigger transmission, with correct DATA toggle PID */
	uint8_t ctrl = B91_USB_EP_BUSY;
	ctrl |= data_toggle ? B91_USB_EP_DAT1 : B91_USB_EP_DAT0;
	b91_write8(cfg, B91_USB_EP_CTRL(ep_num), ctrl);
}

/** Start an IN transfer on a data endpoint from a queued buffer */
static void b91_start_ep_in(const struct device *dev,
			    struct udc_ep_config *ep_cfg,
			    struct net_buf *buf)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);
	uint8_t ep_num = USB_EP_GET_IDX(ep_cfg->addr);
	uint16_t len = MIN(buf->len, ep_cfg->mps);

	b91_ep_write_fifo(cfg, ep_num, buf->data, len,
			  priv->ep_data_toggle[ep_num]);
	net_buf_pull(buf, len);
}

/* ========================================================================== *
 *  ISR Handlers                                                               *
 *                                                                             *
 *  All ISRs follow the same pattern:                                          *
 *    1. Read/clear the hardware IRQ flag                                      *
 *    2. Set a bit in priv->irq_pending                                        *
 *    3. Signal the thread via k_sem_give()                                    *
 *                                                                             *
 *  NO UDC framework calls happen in ISR context (they require mutexes).       *
 *                                                                             *
 *  Special case: SETUP ISR must read the 8-byte SETUP packet from EP0 FIFO    *
 *  immediately, before hardware overwrites it with the next transaction.      *
 * ========================================================================== */

static void b91_isr_setup(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);

	/* Read the 8-byte SETUP packet while it's still in the FIFO */
	b91_ctrl_ep_read(cfg, priv->setup_pkt, 8);

	/* Clear the SETUP IRQ (write-1-to-clear) */
	b91_write8(cfg, B91_USB_CTRL_EP_IRQ_STA, B91_CTRL_EP_IRQ_SETUP);

	priv->irq_pending |= B91_IRQ_EP0_SETUP;
	k_sem_give(&priv->irq_sem);
}

static void b91_isr_data(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);

	b91_write8(cfg, B91_USB_CTRL_EP_IRQ_STA, B91_CTRL_EP_IRQ_DATA);

	priv->irq_pending |= B91_IRQ_EP0_DATA;
	k_sem_give(&priv->irq_sem);
}

static void b91_isr_status(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);

	b91_write8(cfg, B91_USB_CTRL_EP_IRQ_STA, B91_CTRL_EP_IRQ_STA);

	priv->irq_pending |= B91_IRQ_EP0_STATUS;
	k_sem_give(&priv->irq_sem);
}

static void b91_isr_endpoint(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);

	uint8_t ep_irq = b91_read8(cfg, B91_USB_EP_IRQ_STATUS);

	/* Check each data endpoint for pending IRQs */
	for (uint8_t ep = 1; ep <= 8; ep++) {
		uint8_t bit = b91_ep_bit(ep);
		if (ep_irq & bit) {
			/* Clear this EP's IRQ (write-1-to-clear) */
			b91_write8(cfg, B91_USB_EP_IRQ_STATUS, bit);
			priv->irq_pending |= B91_IRQ_EP_COMPLETE(ep);
		}
	}

	k_sem_give(&priv->irq_sem);
}

static void b91_isr_reset(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);

	/* Clear reset status — this register uses |= for status bits (bits 5-7) */
	uint8_t mask = b91_read8(cfg, B91_USB_IRQ_MASK);
	b91_write8(cfg, B91_USB_IRQ_MASK, mask | B91_USB_IRQ_RESET_O);

	priv->irq_pending |= B91_IRQ_USB_RESET;
	k_sem_give(&priv->irq_sem);
}

/* ========================================================================== *
 *  Thread Event Handlers                                                      *
 * ========================================================================== */

/** Handle USB bus reset */
static void b91_handle_reset(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);

	LOG_DBG("USB bus reset");

	/* Clear all EP control registers and reset data toggles
	 * (matches SDK usb_handle_irq() reset handler) */
	for (int i = 0; i < 8; i++) {
		b91_write8(cfg, B91_USB_EP_CTRL(i), 0);
		priv->ep_data_toggle[i] = 0;
	}

	/* Notify the USB device stack */
	udc_submit_event(dev, UDC_EVT_RESET, 0);
}

/** Handle EP0 SETUP packet */
static void b91_handle_ep0_setup(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);
	struct net_buf *buf;

	/* Write timing calibration (required per SDK pattern) */
	b91_write8(cfg, B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);

	/* Drop any pending control transfer buffers from previous transaction */
	struct udc_ep_config *ep_out = udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT);
	struct udc_ep_config *ep_in = udc_get_ep_cfg(dev, USB_CONTROL_EP_IN);

	buf = udc_buf_get_all(ep_out);
	if (buf) {
		net_buf_unref(buf);
	}
	buf = udc_buf_get_all(ep_in);
	if (buf) {
		net_buf_unref(buf);
	}

	/* Allocate a buffer for the 8-byte SETUP packet */
	buf = udc_ctrl_alloc(dev, USB_CONTROL_EP_OUT, 8);
	if (buf == NULL) {
		LOG_ERR("Failed to allocate SETUP buffer");
		b91_write8(cfg, B91_USB_CTRL_EP_CTRL, B91_EP_DAT_STALL);
		return;
	}

	/* Copy the SETUP packet (already read in ISR) into the net_buf */
	net_buf_add_mem(buf, priv->setup_pkt, 8);
	udc_ep_buf_set_setup(buf);

	/* Advance the control transfer state machine.
	 * This submits the SETUP event to the USB stack, which will parse it
	 * and call back into our ep_enqueue with response data. */
	udc_ctrl_update_stage(dev, buf);

	if (udc_ctrl_stage_is_data_out(dev)) {
		/*
		 * Host will send data to us (e.g., SET_REPORT).
		 * Allocate an OUT buffer to receive it.
		 */
		uint16_t len = udc_data_stage_length(buf);

		LOG_DBG("SETUP: data OUT stage, len=%u", len);
		struct net_buf *dbuf = udc_ctrl_alloc(dev,
						      USB_CONTROL_EP_OUT, len);
		if (dbuf == NULL) {
			LOG_ERR("Failed to allocate DATA OUT buffer");
			b91_write8(cfg, B91_USB_CTRL_EP_CTRL, B91_EP_DAT_STALL);
			return;
		}
		udc_buf_put(ep_out, dbuf);
	} else if (udc_ctrl_stage_is_data_in(dev)) {
		/*
		 * Host wants to read data from us (e.g., GET_DESCRIPTOR).
		 * The stack will enqueue IN data via ep_enqueue.
		 */
		LOG_DBG("SETUP: data IN stage");
		udc_ctrl_submit_s_in_status(dev);
	} else {
		/*
		 * No data stage (e.g., SET_ADDRESS, SET_CONFIGURATION).
		 * Go directly to status.
		 */
		LOG_DBG("SETUP: no-data, status");
		udc_ctrl_submit_s_status(dev);
	}

	/* ACK the data phase so the host can proceed */
	b91_write8(cfg, B91_USB_CTRL_EP_CTRL, B91_EP_DAT_ACK);
}

/** Handle EP0 DATA phase IRQ (fires for each 8-byte packet) */
static void b91_handle_ep0_data(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;

	b91_write8(cfg, B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);

	if (udc_ctrl_stage_is_data_in(dev)) {
		/*
		 * Host is reading data from us (IN direction).
		 * The previous DATA IN packet was sent successfully.
		 * Write the next chunk (up to 8 bytes) from the queued buffer.
		 */
		struct udc_ep_config *ep_in =
			udc_get_ep_cfg(dev, USB_CONTROL_EP_IN);
		struct net_buf *buf = udc_buf_peek(ep_in);

		if (buf == NULL) {
			/* No more data to send — this shouldn't happen during
			 * a valid DATA IN stage. Might be a ZLP or status. */
			LOG_DBG("EP0 DATA IN: no buffer");
			b91_write8(cfg, B91_USB_CTRL_EP_CTRL, B91_EP_DAT_ACK);
			return;
		}

		if (buf->len > 0) {
			/* Write next chunk to EP0 FIFO */
			uint16_t chunk = MIN(buf->len, B91_EP0_MPS);
			b91_ctrl_ep_write(cfg, buf->data, chunk);
			net_buf_pull(buf, chunk);
		}

		if (buf->len == 0) {
			/* All data sent — dequeue and advance state machine.
			 * Check for ZLP requirement first. */
			if (udc_ep_buf_has_zlp(buf)) {
				udc_ep_buf_clear_zlp(buf);
				/* Send ZLP by ACK'ing with empty FIFO */
				LOG_DBG("EP0 DATA IN: sending ZLP");
			} else {
				buf = udc_buf_get(ep_in);
				if (buf) {
					udc_ctrl_update_stage(dev, buf);
				}
			}
		}
	} else if (udc_ctrl_stage_is_data_out(dev)) {
		/*
		 * Host is sending data to us (OUT direction).
		 * Read up to 8 bytes from the EP0 FIFO.
		 */
		struct udc_ep_config *ep_out =
			udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT);
		struct net_buf *buf = udc_buf_peek(ep_out);

		if (buf == NULL) {
			LOG_WRN("EP0 DATA OUT: no buffer prepared");
			b91_write8(cfg, B91_USB_CTRL_EP_CTRL, B91_EP_DAT_ACK);
			return;
		}

		/* Read available bytes from EP0 FIFO */
		uint8_t tmp[B91_EP0_MPS];
		b91_ctrl_ep_read(cfg, tmp, B91_EP0_MPS);

		uint16_t remain = net_buf_tailroom(buf);
		uint16_t copy = MIN(remain, B91_EP0_MPS);
		net_buf_add_mem(buf, tmp, copy);

		/* Check if we've received all expected data */
		if (net_buf_tailroom(buf) == 0 || copy < B91_EP0_MPS) {
			/* Transfer complete — submit for status phase */
			buf = udc_buf_get(ep_out);
			if (buf) {
				udc_ctrl_submit_s_out_status(dev, buf);
			}
		}
	} else if (udc_ctrl_stage_is_status_in(dev)) {
		/*
		 * This DATA IRQ in status_in stage means the status IN
		 * (ZLP from device) was sent. Dequeue and finalize.
		 */
		struct udc_ep_config *ep_in =
			udc_get_ep_cfg(dev, USB_CONTROL_EP_IN);
		struct net_buf *buf = udc_buf_get(ep_in);

		if (buf) {
			udc_ctrl_submit_status(dev, buf);
		}
	} else if (udc_ctrl_stage_is_status_out(dev)) {
		/*
		 * Status OUT: host sent ZLP to acknowledge our data.
		 * Read it (ignore contents) and finalize.
		 */
		struct udc_ep_config *ep_out =
			udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT);
		struct net_buf *buf = udc_buf_get(ep_out);

		if (buf) {
			udc_ctrl_submit_status(dev, buf);
		}
	}

	b91_write8(cfg, B91_USB_CTRL_EP_CTRL, B91_EP_DAT_ACK);
}

/** Handle EP0 STATUS phase IRQ */
static void b91_handle_ep0_status(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;

	b91_write8(cfg, B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);

	/* ACK the status phase */
	b91_write8(cfg, B91_USB_CTRL_EP_CTRL, B91_EP_STA_ACK);

	LOG_DBG("EP0 STATUS phase complete");
}

/** Handle data EP IN transfer completion */
static void b91_handle_ep_in_complete(const struct device *dev,
				      uint8_t ep_num)
{
	struct udc_b91_data *priv = udc_get_private(dev);
	uint8_t ep_addr = USB_EP_DIR_IN | ep_num;
	struct udc_ep_config *ep_cfg = udc_get_ep_cfg(dev, ep_addr);

	if (ep_cfg == NULL) {
		LOG_WRN("IRQ for unconfigured EP%u IN", ep_num);
		return;
	}

	/* Toggle DATA0/DATA1 for next transfer */
	priv->ep_data_toggle[ep_num] ^= 1;

	/* Dequeue the completed buffer and signal the stack */
	struct net_buf *buf = udc_buf_get(ep_cfg);
	if (buf) {
		udc_submit_ep_event(dev, buf, 0);
	}

	/* Start next queued transfer immediately if available */
	buf = udc_buf_peek(ep_cfg);
	if (buf && !ep_cfg->stat.halted) {
		b91_start_ep_in(dev, ep_cfg, buf);
	}
}

/* ========================================================================== *
 *  Thread Handler                                                             *
 *                                                                             *
 *  All UDC framework calls happen here (thread context, mutexes OK).          *
 *  ISRs set bits in irq_pending and signal the semaphore.                     *
 * ========================================================================== */

static void b91_thread_handler(void *arg1, void *arg2, void *arg3)
{
	const struct device *dev = (const struct device *)arg1;
	struct udc_b91_data *priv = udc_get_private(dev);

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		k_sem_take(&priv->irq_sem, K_FOREVER);

		/* Atomically snapshot and clear pending flags */
		unsigned int key = irq_lock();
		uint32_t pending = priv->irq_pending;
		priv->irq_pending = 0;
		irq_unlock(key);

		/* Process USB reset first (highest priority) */
		if (pending & B91_IRQ_USB_RESET) {
			b91_handle_reset(dev);
		}

		/* Process EP0 control transfer phases in order */
		if (pending & B91_IRQ_EP0_SETUP) {
			b91_handle_ep0_setup(dev);
		}
		if (pending & B91_IRQ_EP0_DATA) {
			b91_handle_ep0_data(dev);
		}
		if (pending & B91_IRQ_EP0_STATUS) {
			b91_handle_ep0_status(dev);
		}

		/* Process data EP completions (EP1-EP4 for v1) */
		for (uint8_t ep = 1; ep <= 8; ep++) {
			if (pending & B91_IRQ_EP_COMPLETE(ep)) {
				b91_handle_ep_in_complete(dev, ep);
			}
		}
	}
}

/* ========================================================================== *
 *  UDC API Callbacks                                                          *
 * ========================================================================== */

static enum udc_bus_speed udc_b91_device_speed(const struct device *dev)
{
	ARG_UNUSED(dev);
	return UDC_BUS_SPEED_FS;
}

static int udc_b91_ep_enqueue(const struct device *dev,
			      struct udc_ep_config *const ep_cfg,
			      struct net_buf *const buf)
{
	struct udc_b91_data *priv = udc_get_private(dev);

	LOG_DBG("Enqueue EP 0x%02x, len=%u", ep_cfg->addr, buf->len);
	udc_buf_put(ep_cfg, buf);

	if (ep_cfg->stat.halted) {
		LOG_DBG("EP 0x%02x halted, buffer queued", ep_cfg->addr);
		return 0;
	}

	if (USB_EP_GET_IDX(ep_cfg->addr) == 0) {
		/*
		 * EP0: the DATA IRQ handler will pick up this buffer.
		 * For DATA IN, we need to prime the first chunk if there's
		 * a pending DATA phase. Wake the thread to check.
		 */
		if (USB_EP_DIR_IS_IN(ep_cfg->addr) &&
		    udc_ctrl_stage_is_data_in(dev)) {
			/*
			 * Write first 8 bytes to EP0 FIFO immediately.
			 * The host will send an IN token and the hardware
			 * will respond with this data.
			 */
			const struct udc_b91_config *cfg = dev->config;
			struct net_buf *pbuf = udc_buf_peek(ep_cfg);

			if (pbuf && pbuf->len > 0) {
				uint16_t chunk = MIN(pbuf->len, B91_EP0_MPS);
				b91_ctrl_ep_write(cfg, pbuf->data, chunk);
				net_buf_pull(pbuf, chunk);
			}
		}
		return 0;
	}

	/* Data EP IN: start transfer if EP is not busy */
	if (USB_EP_DIR_IS_IN(ep_cfg->addr)) {
		const struct udc_b91_config *cfg = dev->config;
		uint8_t ep_num = USB_EP_GET_IDX(ep_cfg->addr);
		uint8_t ctrl = b91_read8(cfg, B91_USB_EP_CTRL(ep_num));

		if (!(ctrl & B91_USB_EP_BUSY)) {
			b91_start_ep_in(dev, ep_cfg, buf);
		}
		/* If busy, the completion ISR will start this transfer */
	}

	/* EP OUT (beyond EP0) is deferred for v1 */

	return 0;
}

static int udc_b91_ep_dequeue(const struct device *dev,
			      struct udc_ep_config *const ep_cfg)
{
	struct net_buf *buf;

	unsigned int key = irq_lock();
	buf = udc_buf_get_all(ep_cfg);
	irq_unlock(key);

	while (buf) {
		struct net_buf *next = net_buf_frag_del(NULL, buf);
		udc_submit_ep_event(dev, buf, -ECONNABORTED);
		buf = next;
	}

	return 0;
}

static int udc_b91_ep_set_halt(const struct device *dev,
			       struct udc_ep_config *const ep_cfg)
{
	const struct udc_b91_config *cfg = dev->config;
	uint8_t ep_num = USB_EP_GET_IDX(ep_cfg->addr);

	LOG_DBG("HALT EP 0x%02x", ep_cfg->addr);

	if (ep_num == 0) {
		/* Protocol stall on EP0 */
		b91_write8(cfg, B91_USB_CTRL_EP_CTRL, B91_EP_DAT_STALL);
	} else {
		b91_write8(cfg, B91_USB_EP_CTRL(ep_num), B91_USB_EP_STALL);
	}

	ep_cfg->stat.halted = true;
	return 0;
}

static int udc_b91_ep_clear_halt(const struct device *dev,
				 struct udc_ep_config *const ep_cfg)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);
	uint8_t ep_num = USB_EP_GET_IDX(ep_cfg->addr);

	LOG_DBG("Clear HALT EP 0x%02x", ep_cfg->addr);

	/* Clear STALL and reset data toggle to DATA0 */
	b91_write8(cfg, B91_USB_EP_CTRL(ep_num), 0);
	priv->ep_data_toggle[ep_num] = 0;
	ep_cfg->stat.halted = false;

	/* Retrigger any pending transfers */
	if (USB_EP_DIR_IS_IN(ep_cfg->addr)) {
		struct net_buf *buf = udc_buf_peek(ep_cfg);
		if (buf) {
			b91_start_ep_in(dev, ep_cfg, buf);
		}
	}

	return 0;
}

static int udc_b91_ep_try_config(const struct device *dev,
				 struct udc_ep_config *const ep_cfg)
{
	uint8_t ep_num = USB_EP_GET_IDX(ep_cfg->addr);

	ARG_UNUSED(dev);

	/* Validate endpoint direction matches hardware capabilities.
	 * B91: EP1-4, EP7-8 = IN only; EP5-6 = OUT only; EP0 = bidirectional */
	if (ep_num == 0) {
		return 0;
	}

	if (USB_EP_DIR_IS_IN(ep_cfg->addr)) {
		if (ep_num == 5 || ep_num == 6) {
			return -EINVAL;
		}
	} else {
		if (ep_num != 5 && ep_num != 6) {
			return -EINVAL;
		}
	}

	if (ep_cfg->mps > 64) {
		return -EINVAL;
	}

	return 0;
}

static int udc_b91_ep_enable(const struct device *dev,
			     struct udc_ep_config *const ep_cfg)
{
	const struct udc_b91_config *cfg = dev->config;
	uint8_t ep_num = USB_EP_GET_IDX(ep_cfg->addr);

	if (ep_num == 0) {
		/* EP0 is always enabled in hardware */
		return 0;
	}

	LOG_DBG("Enable EP 0x%02x", ep_cfg->addr);

	/* Enable this endpoint in hardware */
	uint8_t en = b91_read8(cfg, B91_USB_EDP_EN);
	en |= b91_ep_bit(ep_num);
	b91_write8(cfg, B91_USB_EDP_EN, en);

	/* Enable IRQ for this endpoint */
	uint8_t irq_mask = b91_read8(cfg, B91_USB_EP_IRQ_MASK);
	irq_mask |= b91_ep_bit(ep_num);
	b91_write8(cfg, B91_USB_EP_IRQ_MASK, irq_mask);

	/* Set max packet size register (global, applies to all EPs, value = MPS/8) */
	b91_write8(cfg, B91_USB_EP_MAX_SIZE, ep_cfg->mps >> 3);

	return 0;
}

static int udc_b91_ep_disable(const struct device *dev,
			      struct udc_ep_config *const ep_cfg)
{
	const struct udc_b91_config *cfg = dev->config;
	uint8_t ep_num = USB_EP_GET_IDX(ep_cfg->addr);

	if (ep_num == 0) {
		return 0;
	}

	LOG_DBG("Disable EP 0x%02x", ep_cfg->addr);

	/* Disable endpoint */
	uint8_t en = b91_read8(cfg, B91_USB_EDP_EN);
	en &= ~b91_ep_bit(ep_num);
	b91_write8(cfg, B91_USB_EDP_EN, en);

	/* Disable EP IRQ */
	uint8_t irq_mask = b91_read8(cfg, B91_USB_EP_IRQ_MASK);
	irq_mask &= ~b91_ep_bit(ep_num);
	b91_write8(cfg, B91_USB_EP_IRQ_MASK, irq_mask);

	return 0;
}

static int udc_b91_set_address(const struct device *dev, const uint8_t addr)
{
	/*
	 * The B91 hardware auto-handles SET_ADDRESS when AUTO_ADDR is enabled
	 * in reg_ctrl_ep_irq_mode. The hardware automatically applies the
	 * address from the SETUP packet's wValue after the STATUS phase.
	 *
	 * We keep AUTO_ADDR enabled specifically for this purpose, so this
	 * callback is a no-op. The framework still processes SET_ADDRESS
	 * at the protocol level.
	 */
	LOG_DBG("SET_ADDRESS %u (hardware auto-handles)", addr);
	return 0;
}

static int udc_b91_host_wakeup(const struct device *dev)
{
	ARG_UNUSED(dev);
	LOG_WRN("Remote wakeup not implemented (v1)");
	return -ENOTSUP;
}

static int udc_b91_enable(const struct device *dev)
{
	/* Enable control endpoints via the UDC framework */
	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT,
				   USB_EP_TYPE_CONTROL, B91_EP0_MPS, 0)) {
		LOG_ERR("Failed to enable EP0 OUT");
		return -EIO;
	}
	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_IN,
				   USB_EP_TYPE_CONTROL, B91_EP0_MPS, 0)) {
		LOG_ERR("Failed to enable EP0 IN");
		return -EIO;
	}

	/* Enable DP 1.5K pull-up — this makes the device visible to the host */
	b91_dp_pullup_en(true);

	LOG_INF("B91 UDC enabled, device connected to bus");
	return 0;
}

static int udc_b91_disable(const struct device *dev)
{
	/* Disconnect from bus */
	b91_dp_pullup_en(false);

	udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT);
	udc_ep_disable_internal(dev, USB_CONTROL_EP_IN);

	LOG_INF("B91 UDC disabled, device disconnected");
	return 0;
}

static int udc_b91_init(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;

	/* Step 1: Enable USB clock and release from reset */
	b91_usb_clock_enable();

	/* Step 2: Power on USB module */
	b91_usb_power_on(true);

	/* Step 3: Configure USB pins (PA5=DM, PA6=DP) */
	b91_usb_pin_setup();

	/* Step 4: Enable manual interrupt mode for all request types
	 * EXCEPT SET_ADDRESS (keep AUTO_ADDR so hardware handles it).
	 *
	 * Clear bits = manual mode; Set bits = auto mode.
	 * We write only AUTO_ADDR, clearing all other auto bits. */
	b91_write8(cfg, B91_USB_CTRL_EP_IRQ_MODE, B91_CTRL_EP_AUTO_ADDR);

	/* Step 5: Enable USB-level IRQ masks for RESET and SUSPEND */
	b91_write8(cfg, B91_USB_IRQ_MASK,
		   B91_USB_IRQ_RESET_MASK | B91_USB_IRQ_SUSPEND_MASK);

	/* Step 6: Set max EP packet size (64 bytes → register value 64/8 = 8) */
	b91_write8(cfg, B91_USB_EP_MAX_SIZE, 64 >> 3);

	/* Step 7: Write initial timing calibration */
	b91_write8(cfg, B91_USB_SUPS_CYC_CALI, B91_USB_TIMING_CALIB);

	/* Step 8: Connect and enable all 5 PLIC interrupts */
	cfg->irq_connect();

	LOG_INF("B91 UDC initialized (EP0 MPS=%u)", B91_EP0_MPS);
	return 0;
}

static int udc_b91_shutdown(const struct device *dev)
{
	b91_dp_pullup_en(false);
	b91_usb_power_on(false);

	udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT);
	udc_ep_disable_internal(dev, USB_CONTROL_EP_IN);

	LOG_INF("B91 UDC shut down");
	return 0;
}

static void udc_b91_lock(const struct device *dev)
{
	udc_lock_internal(dev, K_FOREVER);
}

static void udc_b91_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
}

/* ========================================================================== *
 *  UDC API Table                                                              *
 * ========================================================================== */

static const struct udc_api udc_b91_api = {
	.lock         = udc_b91_lock,
	.unlock       = udc_b91_unlock,
	.device_speed = udc_b91_device_speed,
	.init         = udc_b91_init,
	.enable       = udc_b91_enable,
	.disable      = udc_b91_disable,
	.shutdown     = udc_b91_shutdown,
	.set_address  = udc_b91_set_address,
	.host_wakeup  = udc_b91_host_wakeup,
	.ep_enable    = udc_b91_ep_enable,
	.ep_disable   = udc_b91_ep_disable,
	.ep_set_halt  = udc_b91_ep_set_halt,
	.ep_clear_halt = udc_b91_ep_clear_halt,
	.ep_enqueue   = udc_b91_ep_enqueue,
	.ep_dequeue   = udc_b91_ep_dequeue,
	.ep_try_config = udc_b91_ep_try_config,
};

/* ========================================================================== *
 *  Device Preinit                                                             *
 * ========================================================================== */

static int udc_b91_driver_preinit(const struct device *dev)
{
	const struct udc_b91_config *cfg = dev->config;
	struct udc_b91_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;
	int err;

	k_mutex_init(&data->mutex);
	k_sem_init(&priv->irq_sem, 0, K_SEM_MAX_LIMIT);
	memset(priv->ep_data_toggle, 0, sizeof(priv->ep_data_toggle));
	priv->irq_pending = 0;

	/* Set device capabilities */
	data->caps.hs = false;               /* Full-speed only */
	data->caps.rwup = false;             /* No remote wakeup (v1) */
	data->caps.mps0 = UDC_MPS0_8;       /* EP0 = 8 bytes (HW limit!) */
	data->caps.out_ack = false;
	data->caps.addr_before_status = false; /* Address applied after status */

	/* Register EP0 OUT (control, 8B MPS) */
	cfg->ep_cfg_out[0].caps.out = 1;
	cfg->ep_cfg_out[0].caps.control = 1;
	cfg->ep_cfg_out[0].caps.mps = B91_EP0_MPS;
	cfg->ep_cfg_out[0].addr = USB_EP_DIR_OUT | 0;
	err = udc_register_ep(dev, &cfg->ep_cfg_out[0]);
	if (err) {
		LOG_ERR("Failed to register EP0 OUT");
		return err;
	}

	/* Register EP0 IN (control, 8B MPS) */
	cfg->ep_cfg_in[0].caps.in = 1;
	cfg->ep_cfg_in[0].caps.control = 1;
	cfg->ep_cfg_in[0].caps.mps = B91_EP0_MPS;
	cfg->ep_cfg_in[0].addr = USB_EP_DIR_IN | 0;
	err = udc_register_ep(dev, &cfg->ep_cfg_in[0]);
	if (err) {
		LOG_ERR("Failed to register EP0 IN");
		return err;
	}

	/* Register data IN endpoints (EP1-EP4 for v1)
	 * EP1-4 are IN-capable with interrupt and bulk support, 64B MPS */
	for (size_t i = 1; i < cfg->num_of_eps && i <= 4; i++) {
		cfg->ep_cfg_in[i].caps.in = 1;
		cfg->ep_cfg_in[i].caps.interrupt = 1;
		cfg->ep_cfg_in[i].caps.bulk = 1;
		cfg->ep_cfg_in[i].caps.mps = 64;
		cfg->ep_cfg_in[i].addr = USB_EP_DIR_IN | i;
		err = udc_register_ep(dev, &cfg->ep_cfg_in[i]);
		if (err) {
			LOG_ERR("Failed to register EP%zu IN", i);
			return err;
		}
	}

	/* Create the driver thread */
	k_thread_create(&priv->thread_data,
			cfg->thread_stk, cfg->thread_stk_sz,
			b91_thread_handler,
			(void *)dev, NULL, NULL,
			K_PRIO_COOP(CONFIG_UDC_B91_THREAD_PRIORITY),
			K_ESSENTIAL, K_NO_WAIT);
	k_thread_name_set(&priv->thread_data, dev->name);

	LOG_INF("B91 UDC preinit complete (EP0 MPS=8, %zu EPs registered)",
		cfg->num_of_eps);
	return 0;
}

/* ========================================================================== *
 *  Device Instantiation                                                       *
 * ========================================================================== */

#define UDC_B91_DEVICE_DEFINE(n)                                              \
                                                                              \
	K_THREAD_STACK_DEFINE(udc_b91_stack_##n, CONFIG_UDC_B91_STACK_SIZE);  \
                                                                              \
	static struct udc_ep_config                                           \
		ep_cfg_out_##n[DT_INST_PROP(n, num_bidir_endpoints)];         \
	static struct udc_ep_config                                           \
		ep_cfg_in_##n[DT_INST_PROP(n, num_bidir_endpoints)];          \
                                                                              \
	static void udc_b91_irq_connect_##n(void)                             \
	{                                                                     \
		/* IRQ 0: SETUP (from DT interrupts property) */              \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(n, 0, irq),                   \
			    DT_INST_IRQ_BY_IDX(n, 0, priority),               \
			    b91_isr_setup, DEVICE_DT_INST_GET(n), 0);         \
		irq_enable(DT_INST_IRQ_BY_IDX(n, 0, irq));                   \
                                                                              \
		/* IRQ 1: DATA */                                             \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(n, 1, irq),                   \
			    DT_INST_IRQ_BY_IDX(n, 1, priority),               \
			    b91_isr_data, DEVICE_DT_INST_GET(n), 0);          \
		irq_enable(DT_INST_IRQ_BY_IDX(n, 1, irq));                   \
                                                                              \
		/* IRQ 2: STATUS */                                           \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(n, 2, irq),                   \
			    DT_INST_IRQ_BY_IDX(n, 2, priority),               \
			    b91_isr_status, DEVICE_DT_INST_GET(n), 0);        \
		irq_enable(DT_INST_IRQ_BY_IDX(n, 2, irq));                   \
                                                                              \
		/* IRQ 3: ENDPOINT (data EP completion) */                    \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(n, 3, irq),                   \
			    DT_INST_IRQ_BY_IDX(n, 3, priority),               \
			    b91_isr_endpoint, DEVICE_DT_INST_GET(n), 0);      \
		irq_enable(DT_INST_IRQ_BY_IDX(n, 3, irq));                   \
                                                                              \
		/* IRQ 4: RESET */                                            \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(n, 4, irq),                   \
			    DT_INST_IRQ_BY_IDX(n, 4, priority),               \
			    b91_isr_reset, DEVICE_DT_INST_GET(n), 0);         \
		irq_enable(DT_INST_IRQ_BY_IDX(n, 4, irq));                   \
	}                                                                     \
                                                                              \
	static const struct udc_b91_config udc_b91_config_##n = {            \
		.base = DT_INST_REG_ADDR(n),                                  \
		.num_of_eps = DT_INST_PROP(n, num_bidir_endpoints),           \
		.ep_cfg_in = ep_cfg_in_##n,                                   \
		.ep_cfg_out = ep_cfg_out_##n,                                 \
		.irq_connect = udc_b91_irq_connect_##n,                      \
		.thread_stk = udc_b91_stack_##n,                              \
		.thread_stk_sz = K_THREAD_STACK_SIZEOF(udc_b91_stack_##n),    \
	};                                                                    \
                                                                              \
	static struct udc_b91_data udc_b91_priv_##n;                         \
                                                                              \
	static struct udc_data udc_data_##n = {                               \
		.mutex = Z_MUTEX_INITIALIZER(udc_data_##n.mutex),             \
		.priv = &udc_b91_priv_##n,                                   \
	};                                                                    \
                                                                              \
	DEVICE_DT_INST_DEFINE(n, udc_b91_driver_preinit, NULL,               \
			      &udc_data_##n, &udc_b91_config_##n,            \
			      POST_KERNEL,                                    \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,             \
			      &udc_b91_api);

DT_INST_FOREACH_STATUS_OKAY(UDC_B91_DEVICE_DEFINE)
