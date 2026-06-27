/*
 * Copyright (c) 2025 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telink B91 WS2812 LED strip driver via PSPI + DMA on PB7.
 *
 * Hardware path: RGB buffer → SPI encode → DMA ch4 → PSPI MOSI (PB7) → WS2812
 *
 * Each WS2812 bit is encoded as 1 SPI byte at 6 MHz:
 *   "0" bit = 0xC0 (2 HIGH + 6 LOW) → T0H=333ns, T0L=1000ns
 *   "1" bit = 0xF0 (4 HIGH + 4 LOW) → T1H=667ns, T1L=667ns
 * This matches the OEM firmware approach (PSPI + DMA ch4, verified waveform).
 *
 * GPIO bit-bang (previous approach) works for LEDs 2-83 but LED 1 shows
 * persistent green due to first-bit timing jitter. PSPI has zero jitter.
 */

#include "b91_pspi.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/irq_multilevel.h>

LOG_MODULE_REGISTER(led_strip_b91, LOG_LEVEL_INF);

/* Analog register access — needed for PC2 IE and pull-up config during init. */
static void analog_wait(void)
{
	while (sys_read8(B91_ANALOG_CTRL_REG) & B91_ANALOG_CTRL_BUSY) {
	}
}

static void analog_wait_txbuf(void)
{
	while (!(sys_read8(B91_ANALOG_BUF_CNT_REG) & B91_ANALOG_TX_BUFCNT)) {
	}
}

static uint8_t led_analog_read(uint8_t addr)
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

static void led_analog_write(uint8_t addr, uint8_t data)
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

#define DT_DRV_COMPAT telink_b91_spi_led_strip

/* Buffer size derived from DTS chain-length — single source of truth. */
#define NUM_LEDS DT_INST_PROP(0, chain_length)

struct b91_led_strip_config {
	uint16_t num_leds;
};

/* ========================================================================
 * PSPI + DMA WS2812 Transfer
 *
 * PSPI master mode on PB7 (MOSI), 6 MHz SPI clock.
 * Each WS2812 bit → 1 SPI byte (8 SPI bits at 6 MHz = 1333ns per WS2812 bit).
 * DMA ch4 transfers the encoded buffer to PSPI TX FIFO.
 * Total frame: 83 LEDs × 24 bits × 1333ns = 2656µs.
 * Zero CPU involvement during transfer, zero timing jitter.
 * ======================================================================== */

/* SPI-encoded DMA buffer: 1 byte per WS2812 bit, 24 bits per LED */
static uint8_t __aligned(4) spi_buf[NUM_LEDS * WS2812_SPI_BYTES_PER_LED];

/* DMA ch4 CTRL register value (configured once, reused per frame).
 * SDK dma_config_t bit layout:
 *   [0]     = dma_en (set separately per transfer)
 *   [3:1]   = irq_en (0 = poll mode)
 *   [8:4]   = dst_req_sel (4 = DMA_REQ_SPI_APB_TX)
 *   [13:9]  = src_req_sel (0 = memory)
 *   [15:14] = dst_addr_ctrl (2 = fixed, PSPI FIFO)
 *   [17:16] = src_addr_ctrl (0 = increment, buffer)
 *   [18]    = dstmode (1 = handshake for peripheral)
 *   [19]    = srcmode (0 = normal for memory)
 *   [21:20] = dstwidth (2 = 32-bit word)
 *   [23:22] = srcwidth (2 = 32-bit word)
 */
#define DMA_CH4_CTRL_CFG \
	((B91_DMA_REQ_PSPI_TX << 4) | /* dst_req = PSPI TX */ \
	 (2 << 14) |                   /* dst addr fixed (FIFO) */ \
	 (0 << 16) |                   /* src addr increment */ \
	 BIT(18) |                     /* dst handshake mode */ \
	 (2 << 20) |                   /* dst width = word */ \
	 (2 << 22))                    /* src width = word */

/* --- Interrupt-driven completion ---
 * The render thread sleeps on this semaphore during the ~2.66 ms DMA transfer
 * instead of busy-polling B91_PSPI_BUSY, freeing the CPU. The PSPI
 * End-of-Transfer interrupt (PLIC source 23, dedicated to APB SPI — no DMA-IRQ
 * sharing with the BLE blob) gives the semaphore when the last bit is clocked
 * out. A timeout-recovery path keeps LEDs working if the IRQ ever misfires. */
static K_SEM_DEFINE(xfer_done_sem, 0, 1);

/* Cycle count by which the WS2812 reset latch (>=280us low) has elapsed after
 * the previous frame. The next send waits out only the (virtually always zero)
 * remainder — the inter-frame render sleep normally covers the whole gap. */
static uint32_t reset_ready_cyc;

#define LED_PSPI_IRQ       (IRQ_TO_L2(B91_PSPI_IRQ_SRC) | 11)
#define LED_PSPI_IRQ_PRIO  2   /* below BLE RF/stimer so it can't delay them */

static void ws2812_pspi_isr(const void *arg)
{
	ARG_UNUSED(arg);
	if (sys_read8(B91_PSPI_IRQ_STATE) & B91_PSPI_END_INT) {
		sys_write8(B91_PSPI_END_INT, B91_PSPI_IRQ_STATE);  /* write-1-clear */
		k_sem_give(&xfer_done_sem);
	}
}

static void ws2812_pspi_send(const struct led_rgb *pixels, uint16_t num_pixels)
{
	uint32_t num_bytes = (uint32_t)num_pixels * WS2812_SPI_BYTES_PER_LED;
	uint8_t *p = spi_buf;

	/* Encode pixels → SPI buffer (GRB order, 1 byte per WS2812 bit) */
	for (uint16_t i = 0; i < num_pixels; i++) {
		uint8_t colors[3] = { pixels[i].g, pixels[i].r, pixels[i].b };
		for (int c = 0; c < 3; c++) {
			uint8_t byte = colors[c];
			for (int bit = 7; bit >= 0; bit--) {
				*p++ = (byte & BIT(bit)) ? WS2812_SPI_ONE
							 : WS2812_SPI_ZERO;
			}
		}
	}

	uint32_t ch4 = B91_DMA_CH_BASE(4);

	/* Set PSPI TX byte count */
	uint32_t cnt = num_bytes - 1;
	sys_write8(cnt & 0xFF, B91_PSPI_TX_CNT0);
	sys_write8((cnt >> 8) & 0xFF, B91_PSPI_TX_CNT1);
	sys_write8((cnt >> 16) & 0xFF, B91_PSPI_TX_CNT2);

	/* Clear TX FIFO */
	sys_write8(sys_read8(B91_PSPI_FIFO_STATE) | B91_PSPI_FIFO_CLR_TX,
		   B91_PSPI_FIFO_STATE);

	/* Configure DMA ch4: src = SPI buffer, dst = PSPI TX FIFO */
	sys_write32(b91_dma_addr(spi_buf), ch4 + B91_DMA_SRC_ADDR);
	sys_write32(B91_PSPI_DATA0, ch4 + B91_DMA_DST_ADDR);

	/* DMA size: [21:0] = word count, [23:22] = tail bytes */
	uint32_t words = num_bytes / 4;
	uint32_t tail = num_bytes % 4;
	sys_write32((tail << 22) | words, ch4 + B91_DMA_SIZE);

	/* Honor the WS2812 reset latch left over from the previous frame. Frames
	 * are >=11ms apart, so this remainder is virtually always already elapsed
	 * (busy-waits zero); the signed cycle delta also tolerates counter wrap. */
	int32_t reset_remain = (int32_t)(reset_ready_cyc - k_cycle_get_32());
	if (reset_remain > 0) {
		k_busy_wait(k_cyc_to_us_ceil32((uint32_t)reset_remain));
	}

	/* Clear any stale End-of-Transfer status and disarm the semaphore. */
	sys_write8(B91_PSPI_END_INT, B91_PSPI_IRQ_STATE);
	k_sem_reset(&xfer_done_sem);

	/* Enable DMA channel (set bit 0 in CTRL) */
	sys_write32(DMA_CH4_CTRL_CFG | BIT(0), ch4 + B91_DMA_CTRL);

	/* Trigger PSPI transfer: write command byte → starts clocking */
	sys_write8(0x00, B91_PSPI_TRANS1);

	/* Sleep until the End-of-Transfer IRQ fires (last bit clocked out),
	 * yielding the CPU for the whole ~2.66 ms transfer instead of spinning. */
	if (k_sem_take(&xfer_done_sem, K_MSEC(5)) != 0) {
		/* End-IRQ didn't fire this frame — skip it. The next send fully
		 * re-initializes DMA + clears stale status, so it self-heals. This
		 * timeout exists only to prevent a permanent render-thread hang. */
		LOG_WRN("LED xfer End-IRQ timeout (frame skipped)");
	}

	/* Arm the reset-latch deadline; the inter-frame render sleep covers it. */
	reset_ready_cyc = k_cycle_get_32() + k_us_to_cyc_ceil32(WS2812_RESET_US);
}

/* ========================================================================
 * Zephyr led_strip API
 * ======================================================================== */

static int b91_led_strip_update_rgb(const struct device *dev,
				    struct led_rgb *pixels,
				    size_t num_pixels)
{
	const struct b91_led_strip_config *cfg = dev->config;

	if (num_pixels > cfg->num_leds) {
		num_pixels = cfg->num_leds;
	}

	ws2812_pspi_send(pixels, num_pixels);
	return 0;
}

static int b91_led_strip_update_channels(const struct device *dev,
					 uint8_t *channels,
					 size_t num_channels)
{
	return -ENOTSUP;
}

static size_t b91_led_strip_length(const struct device *dev)
{
	const struct b91_led_strip_config *cfg = dev->config;
	return cfg->num_leds;
}

static int b91_led_strip_init(const struct device *dev)
{
	const struct b91_led_strip_config *cfg = dev->config;

#if IS_ENABLED(CONFIG_LED_STRIP_B91_SPI_PC2_POWER)
	/* PC2 controls LED VCC via MOSFET. PC2 HIGH = power ON (confirmed).
	 * Sequence matches original firmware secondary_pipeline(). */
	sys_write8(sys_read8(B91_GPIO_PC_GPIO) | BIT(2), B91_GPIO_PC_GPIO);
	sys_write8(sys_read8(B91_GPIO_PC_OEN) & ~BIT(2), B91_GPIO_PC_OEN);
	led_analog_write(B91_ANA_PC_IE,
			 led_analog_read(B91_ANA_PC_IE) & ~BIT(2));
	sys_write8(sys_read8(B91_GPIO_PC_OUT) | BIT(2), B91_GPIO_PC_OUT);
	uint8_t pc_pull = led_analog_read(B91_ANA_PC_PULL);
	led_analog_write(B91_ANA_PC_PULL,
			 (pc_pull & ~B91_ANA_PC2_PULL_MASK) | B91_ANA_PC2_PULL_10K);
	LOG_INF("PC2 = HIGH (LED power on)");
#endif

	/* --- PB7 = PSPI MOSI (function 1) ---
	 * PB5 (PSPI CLK) is a matrix column pin — do NOT reconfigure it!
	 * The PSPI internal clock runs regardless of whether CLK is routed
	 * to a physical pin. Only MOSI needs to be in SPI function mode. */

	/* PB7: PSPI MOSI — set function mux, clear GPIO mode */
	uint8_t fuc_h = sys_read8(B91_GPIO_PB_FUC_H);
	fuc_h = (fuc_h & ~B91_PB7_FUC_MASK) | B91_PB7_FUC_PSPI_MOSI;
	sys_write8(fuc_h, B91_GPIO_PB_FUC_H);
	sys_write8(sys_read8(B91_GPIO_PB_GPIO) & ~BIT(7), B91_GPIO_PB_GPIO);

	/* --- PSPI configuration ---
	 * Master mode, CPOL=0/CPHA=0, 6 MHz clock (PCLK=24MHz, div=1).
	 * Write-only mode, TX DMA enabled. */
	sys_write8(B91_PSPI_MODE0_MASTER, B91_PSPI_MODE0);
	sys_write8(B91_PSPI_CLK_DIV, B91_PSPI_MODE1);
	sys_write8(0x00, B91_PSPI_MODE2);  /* no cmd_en */
	sys_write8(B91_PSPI_TRANS0_WRITE_ONLY, B91_PSPI_TRANS0);
	sys_write8(B91_PSPI_TRANS2_TX_DMA_EN | B91_PSPI_TRANS2_END_INT_EN,
		   B91_PSPI_TRANS2);

	/* Pre-configure DMA ch4 (without enable bit — that's set per transfer) */
	sys_write32(DMA_CH4_CTRL_CFG, B91_DMA_CH_BASE(4) + B91_DMA_CTRL);

	/* Wire the PSPI End-of-Transfer interrupt so update_rgb can sleep through
	 * the DMA transfer. Clear any stale status first; irq_enable is required
	 * (nothing else enables PLIC source 23 for us — cf. the USB driver). */
	sys_write8(B91_PSPI_END_INT, B91_PSPI_IRQ_STATE);
	irq_connect_dynamic(LED_PSPI_IRQ, LED_PSPI_IRQ_PRIO, ws2812_pspi_isr, NULL, 0);
	irq_enable(LED_PSPI_IRQ);

	LOG_INF("WS2812 LED strip: %d LEDs, PSPI+DMA ch4, 6MHz, PB7 MOSI, End-IRQ",
		cfg->num_leds);
	return 0;
}

static DEVICE_API(led_strip, b91_led_strip_api) = {
	.update_rgb = b91_led_strip_update_rgb,
	.update_channels = b91_led_strip_update_channels,
	.length = b91_led_strip_length,
};

#define B91_LED_STRIP_INIT(inst)                                      \
	static const struct b91_led_strip_config b91_led_cfg_##inst = {   \
		.num_leds = DT_INST_PROP(inst, chain_length),                 \
	};                                                                \
	DEVICE_DT_INST_DEFINE(inst, b91_led_strip_init, NULL,             \
			      NULL, &b91_led_cfg_##inst,              \
			      POST_KERNEL,                            \
			      CONFIG_LED_STRIP_INIT_PRIORITY,         \
			      &b91_led_strip_api);

DT_INST_FOREACH_STATUS_OKAY(B91_LED_STRIP_INIT)
