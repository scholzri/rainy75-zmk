/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot Diagnostic — .noinit SRAM buffer + optional GPIO heartbeat.
 *
 * Records boot stage codes to SRAM that can be read via SWS/BDT when
 * USB serial is not working. Also toggles a GPIO pin at each stage
 * for logic analyzer visibility (configurable, default PD7).
 *
 * How to read after flash:
 *   1. Look up buffer address:
 *      riscv32-elf-nm build/zephyr/zephyr.elf | grep boot_diag_buf
 *   2. Read via BDT:
 *      ./reverse/tools/sws_flash.sh sram 0x<addr> 36
 *   3. Decode: magic(4) count(1) last(1) rsv(2) stages(28)
 *
 * GPIO heartbeat (if CONFIG_BOOT_DIAG_GPIO=y):
 *   - Default pin: PD7 (not used by matrix, USB, or RGB)
 *   - Toggles HIGH→LOW at each stage, producing pulses visible on scope
 *   - Uses direct register access (works before GPIO driver init)
 *
 * B91 Port D GPIO registers (from SDK gpio_reg.h):
 *   reg_gpio_pd_oen  = 0x8014031A  (0=output enabled, active LOW)
 *   reg_gpio_pd_out  = 0x8014031B  (output data)
 *   reg_gpio_pd_gpio = 0x8014031E  (1=GPIO mode)
 *   reg_gpio_pd_fuc_h = 0x80140337 (function mux PD4-PD7, 2 bits/pin)
 */

#include "boot_diag.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(boot_diag, LOG_LEVEL_INF);

/* .noinit section: not zeroed by crt0, readable via SWS/BDT.
 * Non-static so symbol appears in nm output for address lookup. */
__noinit struct boot_diag_buffer boot_diag_buf;

/* Track if we've initialized this boot cycle */
static bool diag_initialized;

/* Stop PE_OUT writes once APPLICATION init completes (PE4-PE7 used as kscan columns) */
static bool pe_out_frozen;

/* Stop analog writes once BLE controller is active — the analog serial bus
 * (0x80140180-0x80140184) is shared, and the BLE blob may have in-progress
 * analog transactions when calling our callbacks. */
static bool analog_frozen;

/* ========================================================================== *
 *  GPIO Heartbeat (optional, direct register access for earliest boot)       *
 * ========================================================================== */

#ifdef CONFIG_BOOT_DIAG_GPIO

/* PD7 register addresses */
#define DIAG_GPIO_OEN   0x8014031AUL  /* output enable (0=output) */
#define DIAG_GPIO_OUT   0x8014031BUL  /* output data */
#define DIAG_GPIO_GPIO  0x8014031EUL  /* GPIO function (1=GPIO) */
#define DIAG_GPIO_FUC_H 0x80140337UL  /* function mux PD4-PD7 */
#define DIAG_GPIO_BIT   BIT(7)        /* PD7 */
#define DIAG_GPIO_MUX_MASK (BIT(6) | BIT(7)) /* PD7 mux bits in fuc_h */

static bool gpio_initialized;

static void diag_gpio_init(void)
{
	if (gpio_initialized) {
		return;
	}

	/* Clear function mux for PD7 (bits [7:6] in fuc_h → 0 = GPIO) */
	uint8_t fuc = sys_read8(DIAG_GPIO_FUC_H);

	fuc &= ~DIAG_GPIO_MUX_MASK;
	sys_write8(fuc, DIAG_GPIO_FUC_H);

	/* Set GPIO mode */
	uint8_t gpio = sys_read8(DIAG_GPIO_GPIO);

	gpio |= DIAG_GPIO_BIT;
	sys_write8(gpio, DIAG_GPIO_GPIO);

	/* Enable output (active LOW: clear bit = output enabled) */
	uint8_t oen = sys_read8(DIAG_GPIO_OEN);

	oen &= ~DIAG_GPIO_BIT;
	sys_write8(oen, DIAG_GPIO_OEN);

	/* Start HIGH */
	uint8_t out = sys_read8(DIAG_GPIO_OUT);

	out |= DIAG_GPIO_BIT;
	sys_write8(out, DIAG_GPIO_OUT);

	gpio_initialized = true;
}

static void diag_gpio_pulse(void)
{
	if (!gpio_initialized) {
		diag_gpio_init();
	}

	/* Toggle: HIGH→LOW→HIGH produces a negative pulse.
	 * Each stage gets one pulse — count pulses to identify stage. */
	uint8_t out = sys_read8(DIAG_GPIO_OUT);

	sys_write8(out & ~DIAG_GPIO_BIT, DIAG_GPIO_OUT);
	/* ~100ns at 48 MHz — visible on any logic analyzer */
	for (volatile int i = 0; i < 10; i++) {
	}
	sys_write8(out | DIAG_GPIO_BIT, DIAG_GPIO_OUT);
}

#else /* !CONFIG_BOOT_DIAG_GPIO */

static inline void diag_gpio_init(void) {}
static inline void diag_gpio_pulse(void) {}

#endif /* CONFIG_BOOT_DIAG_GPIO */

/* ========================================================================== *
 *  Core: .noinit buffer management                                           *
 * ========================================================================== */

/* PE output-enable register: SWS-readable stage marker (0x80140323).
 * Read: bdt B91 rc 0x140323 -s 1 */
#define DIAG_PE_OEN 0x80140323UL

/* PD output register: SWS-readable stage marker that is NEVER frozen.
 * PD2-PD6 are inputs (OEN=1) so writing output data has no pin effect.
 * PD7 is the diagnostic heartbeat pin (separate from kscan).
 * PD0-PD1 are unused.
 * Read: bdt B91 rc 0x14031B -s 1 */
#define DIAG_PD_OUT 0x8014031BUL

/* Analog register 0x3E: persistent stage marker.
 * Survives gpio_init(0) and kscan reconfiguration.
 * Read: bdt B91 ra 0x3E -s 1 */
#define DIAG_ANA_STAGE 0x3E

/* Write a value to analog register — inline, no dependency on blob.
 * Uses the same approach as b91_analog_write in b91_bt.c. */
static void diag_analog_write(uint8_t addr, uint8_t val)
{
	sys_write8(addr, 0x80140180UL);  /* ana_addr */
	sys_write8(val,  0x80140184UL);  /* ana_data */
	sys_write8(1,    0x80140183UL);  /* ana_len = 1 */
	sys_write8(0x60, 0x80140182UL);  /* ana_ctrl = CYC | RW(write) */
	while (sys_read8(0x80140182UL) & 0x80) { /* wait busy */
	}
}

void boot_diag_record(uint8_t stage)
{
	if (!diag_initialized) {
		boot_diag_buf.magic = BOOT_DIAG_MAGIC;
		boot_diag_buf.count = 0;
		boot_diag_buf.last = 0;
		boot_diag_buf.reserved[0] = 0;
		boot_diag_buf.reserved[1] = 0;
		diag_initialized = true;
	}

	if (boot_diag_buf.count < BOOT_DIAG_MAX_ENTRIES) {
		boot_diag_buf.stages[boot_diag_buf.count] = stage;
		boot_diag_buf.count++;
	}
	boot_diag_buf.last = stage;

	/* Write stage to PE output-enable register — readable via SWS.
	 * Frozen after APPLICATION init because PE4-PE7 are kscan columns. */
	if (!pe_out_frozen) {
		sys_write8(stage, DIAG_PE_OEN);
	}

	/* Write to analog register 0x3E — persists across GPIO reconfig.
	 * Disabled once BLE controller is active to avoid analog bus conflicts. */
	if (!analog_frozen) {
		diag_analog_write(DIAG_ANA_STAGE, stage);
	}

	/* Write to PD_OUT register — NEVER frozen, always readable via SWS.
	 * bdt B91 rc 0x14031B -s 1 → last boot_diag stage code. */
	sys_write8(stage, DIAG_PD_OUT);

	diag_gpio_pulse();
}

void boot_diag_freeze_flash(void)
{
	/* No-op: flash diagnostics removed.
	 * Kept as stub so b91_bt.c compiles without changes. */
}

void boot_diag_freeze_analog(void)
{
	analog_frozen = true;
}

/* ========================================================================== *
 *  SYS_INIT hooks at each init level                                         *
 * ========================================================================== */

static int boot_diag_early(void)
{
	diag_gpio_init();
	boot_diag_record(BOOT_DIAG_EARLY);
	return 0;
}

static int boot_diag_pre_kernel_1(void)
{
	boot_diag_record(BOOT_DIAG_PRE_KERNEL_1);
	return 0;
}

static int boot_diag_pre_kernel_2(void)
{
	boot_diag_record(BOOT_DIAG_PRE_KERNEL_2);
	return 0;
}

static int boot_diag_post_kernel(void)
{
	boot_diag_record(BOOT_DIAG_POST_KERNEL);

	/* Restore PA7 to SWS function so BDT activate works while
	 * firmware is running.  GPIO driver init (PRE_KERNEL_1) sets
	 * actas_gpio bit 7 = GPIO mode; clear it → SWS alternate fn. */
	uint8_t pa_gpio = sys_read8(0x80140306UL);
	pa_gpio &= ~BIT(7);
	sys_write8(pa_gpio, 0x80140306UL);

	return 0;
}

static int boot_diag_application(void)
{
	boot_diag_record(BOOT_DIAG_APPLICATION);
	/* Freeze PE_OUT writes — PE4-PE7 are kscan column pins */
	pe_out_frozen = true;
	LOG_INF("Boot diag: %u stages recorded, last=0x%02x",
		boot_diag_buf.count, boot_diag_buf.last);
	return 0;
}

/* Post-BLE diagnostic: runs at APPLICATION/51, right AFTER zmk_ble_init (APPLICATION/50). */
static int boot_diag_post_ble(void)
{
	boot_diag_record(BOOT_DIAG_POST_BLE);
	diag_analog_write(0x3A, BOOT_DIAG_POST_BLE);
	return 0;
}

/* Priority 0 = earliest possible at each level.
 * EARLY runs BEFORE soc_early_init_hook() (B91 clock/PLL init).
 * GPIO works here because:
 *  - PCLK runs at 24 MHz RC oscillator from boot
 *  - GPIO peripheral has no clock gate (always accessible)
 *  - SRAM is always accessible (no init needed) */
SYS_INIT(boot_diag_early, EARLY, 0);
SYS_INIT(boot_diag_pre_kernel_1, PRE_KERNEL_1, 0);
SYS_INIT(boot_diag_pre_kernel_2, PRE_KERNEL_2, 0);
SYS_INIT(boot_diag_post_kernel, POST_KERNEL, 0);
/* Priority 0 = first in APPLICATION */
SYS_INIT(boot_diag_application, APPLICATION, 0);
/* Priority 51 = right after zmk_ble_init (priority 50) */
SYS_INIT(boot_diag_post_ble, APPLICATION, 51);
