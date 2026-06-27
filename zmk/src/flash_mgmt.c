/*
 * Custom mcumgr group for raw flash operations.
 *
 * Exposes erase/write/read/commit commands for arbitrary flash offsets,
 * enabling firmware restore over USB without the SWS hardware debugger.
 *
 * Group ID 64 (MGMT_GROUP_ID_PERUSER), 4 commands:
 *   0: erase   {"off": uint, "len": uint}        -> {"rc": int}
 *   1: write   {"off": uint, "data": bstr}        -> {"rc": int}
 *   2: read    {"off": uint, "len": uint}          -> {"rc": int, "data": bstr}
 *   3: commit  {"stg": uint, "len": uint}          -> {"rc": int} (then erase+copy+reset)
 *
 * Safety: refuses operations touching >= 0xFE000 (calibration/MAC region).
 *
 * The commit command uses a RAM-resident trampoline to safely erase/write
 * flash while the firmware runs from XIP flash. All threads and interrupts
 * are disabled before the destructive work begins.
 *
 * Copyright (c) 2025 scholzri
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(flash_mgmt, LOG_LEVEL_INF);

#define FLASH_MGMT_ID_ERASE  0
#define FLASH_MGMT_ID_WRITE  1
#define FLASH_MGMT_ID_READ   2
#define FLASH_MGMT_ID_COMMIT 3

/* Protected region: RF/ADC calibration (0xFE000) and BLE MAC (0xFF000) */
#define FLASH_PROTECTED_START  0xFE000

/* Max data per read/write operation (one flash page) */
#define FLASH_MGMT_MAX_DATA    256

/* Flash erase sector size */
#define FLASH_SECTOR_SIZE      4096

static const struct device *flash_dev;

/* -----------------------------------------------------------------------
 * B91 HAL RAM-resident functions (in .ram_code section, execute from ILM).
 * These are the ONLY functions safe to call after erasing XIP flash.
 * ----------------------------------------------------------------------- */
extern void flash_erase_sector_ram(unsigned long addr);
extern void flash_write_page_ram(unsigned long addr, unsigned long len,
				 unsigned char *buf);
extern void flash_read_page_ram(unsigned long addr, unsigned long len,
				unsigned char *buf);
extern void analog_write_reg8(unsigned char addr, unsigned char data);
extern void flash_unlock(void);
extern void flash_unlock_ram(unsigned long mid_type);

/* B91 timer base — WDT enable is in TMR_CTRL2 (base+2), bit 7 */
#define REG_TMR_BASE    0x80140140
#define REG_TMR_CTRL2   (*(volatile unsigned char *)(REG_TMR_BASE + 0x02))
#define TMR_WD_EN_BIT   0x80

/* Disable all interrupts — inline CSR manipulation, always safe from RAM.
 * Zeroes entire MIE register and clears MSTATUS.MIE (global interrupt enable). */
static inline void _flash_mgmt_irq_disable(void)
{
	__asm__ volatile("csrw 0x304, zero");                     /* MIE = 0 */
	__asm__ volatile("csrc 0x300, %0" :: "r"((1UL << 3)));   /* MSTATUS.MIE = 0 */
}

/* -----------------------------------------------------------------------
 * RAM-resident trampoline: erase flash + copy staging -> 0x0 + reset.
 *
 * This function executes ENTIRELY from ILM (SRAM). It never references
 * XIP flash — all called functions are also in .ram_code. After this
 * function starts, the flash can be freely erased.
 *
 * Flow: disable IRQs -> erase 0x0..erase_end -> copy staging -> reset
 * ----------------------------------------------------------------------- */

/* Parameters passed via global (in DLM/SRAM, survives flash erase) */
static volatile uint32_t commit_stg_off;
static volatile uint32_t commit_fw_len;

/* Progress marker visible via SWS: bdt B91 ra 0x3E -s 1 */
__attribute__((section(".ram_code"))) __attribute__((noinline))
static void trampoline_marker(uint8_t val)
{
	analog_write_reg8(0x3E, val);
}

__attribute__((section(".ram_code"))) __attribute__((noinline))
static void flash_restore_trampoline(void)
{
	uint32_t stg = commit_stg_off;
	uint32_t fw_len = commit_fw_len;

	/* Disable all interrupts — no XIP access from any ISR or thread */
	_flash_mgmt_irq_disable();

	/* Disable hardware watchdog — it can't be fed during the trampoline */
	REG_TMR_CTRL2 &= ~TMR_WD_EN_BIT;

	/* Disable Branch Target Buffer (Andes D25F mmisc_ctl CSR 0x7D0 bit 3).
	 * CRITICAL: flash_erase_sector_ram() does NOT disable BTB — the SDK's
	 * wrapper flash_erase_sector() does it via DISABLE_BTB/ENABLE_BTB, but
	 * we call the _ram version directly.  With BTB enabled, the branch
	 * predictor speculatively fetches from cached XIP addresses while
	 * mspi_stop_xip() has the MSPI bus in manual SPI mode, hanging the bus
	 * after 2-3 erase iterations. */
	__asm__ volatile("csrci 0x7D0, 8");   /* DISABLE_BTB */
	__asm__ volatile("fence.i");           /* flush instruction pipeline */

	/* Clear flash write protection from RAM (safe after IRQ disable).
	 * flash_unlock_ram(0) sends WREN + Write Status Register 0x0000,
	 * clearing all BP bits and WPS. Belt-and-suspenders: flash_unlock()
	 * was already called from the commit handler, but re-clearing from
	 * RAM ensures protection is off even if something re-locked it. */
	flash_unlock_ram(0);

	trampoline_marker(0x11); /* entering Phase 1 */

	/* Phase 1: Erase 0x0 to staging start (preserves staging data) */
	for (uint32_t pos = 0; pos < stg; pos += FLASH_SECTOR_SIZE) {
		flash_erase_sector_ram(pos);
	}

	trampoline_marker(0x22); /* Phase 1 complete */

	/* Brief settling delay after bulk erase (~500k cycles ≈ 10ms at 48MHz) */
	for (volatile uint32_t i = 0; i < 500000; i++) {
	}

	trampoline_marker(0x33); /* entering Phase 2 */

	/* Phase 2: Copy firmware from staging area to 0x0 */
	uint8_t buf[256] __attribute__((aligned(4)));
	for (uint32_t pos = 0; pos < fw_len; pos += 256) {
		uint32_t chunk = fw_len - pos;
		if (chunk > 256) {
			chunk = 256;
		}
		flash_read_page_ram(stg + pos, chunk, buf);
		flash_write_page_ram(pos, chunk, buf);
	}

	trampoline_marker(0x44); /* Phase 2 complete */

	/* Phase 3: Erase staging through 0x80000 to clean up:
	 *   - The staging copy (no longer needed)
	 *   - OTA secondary bank remnants (0x40000-0x7FFFF)
	 * The stock firmware's dual-bank boot checks 0x40000 for a TLNK
	 * header; leftover data there prevents boot. */
	for (uint32_t pos = stg; pos < 0x80000; pos += FLASH_SECTOR_SIZE) {
		flash_erase_sector_ram(pos);
	}

	trampoline_marker(0x55); /* Phase 3 complete, about to reset */

	/* Reset MCU: write BIT(5) to PWDN_CTRL register.
	 * B91 peripheral registers require 0x80000000 base (REG_RW_BASE_ADDR).
	 * SDK: write_reg8(0x1401ef, 0x20) = *(volatile u8 *)(0x80000000|0x1401ef) */
	*(volatile unsigned char *)0x801401ef = 0x20;

	/* Never reached */
	while (1) {
	}
}

/* Delayed work: called ~500ms after commit response is sent */
static void commit_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(commit_work, commit_work_handler);

static void commit_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	/* Point of no return — jump to RAM trampoline */
	flash_restore_trampoline();
}

/* -----------------------------------------------------------------------
 * SMP handlers
 * ----------------------------------------------------------------------- */

static int flash_mgmt_erase(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	uint32_t off = 0, len = 0;
	size_t decoded;
	int rc = 0;

	struct zcbor_map_decode_key_val decode_map[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &len),
	};

	if (zcbor_map_decode_bulk(zsd, decode_map, ARRAY_SIZE(decode_map),
				  &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	/* Bounds check: must not touch protected region */
	if ((uint64_t)off + len > FLASH_PROTECTED_START) {
		LOG_ERR("erase: would touch protected region (off=0x%x len=0x%x)", off, len);
		return MGMT_ERR_EACCESSDENIED;
	}

	/* Alignment check: must be sector-aligned */
	if ((off % FLASH_SECTOR_SIZE) != 0 || (len % FLASH_SECTOR_SIZE) != 0 || len == 0) {
		LOG_ERR("erase: bad alignment (off=0x%x len=0x%x)", off, len);
		return MGMT_ERR_EINVAL;
	}

	/*
	 * Erase sector by sector with k_msleep() between each.
	 * Safe ONLY for regions outside the running XIP code (0x10000-0x5C000).
	 * For erasing the app image area, use the commit command instead.
	 */
	for (uint32_t pos = off; pos < off + len; pos += FLASH_SECTOR_SIZE) {
		rc = flash_erase(flash_dev, pos, FLASH_SECTOR_SIZE);
		if (rc < 0) {
			LOG_ERR("erase: failed at 0x%x: %d", pos, rc);
			break;
		}
		k_msleep(10);
	}

	bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, rc);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int flash_mgmt_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	uint32_t off = 0;
	struct zcbor_string data = {0};
	size_t decoded;
	int rc;

	struct zcbor_map_decode_key_val decode_map[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode, &data),
	};

	if (zcbor_map_decode_bulk(zsd, decode_map, ARRAY_SIZE(decode_map),
				  &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	if (data.len == 0 || data.len > FLASH_MGMT_MAX_DATA) {
		LOG_ERR("write: bad data len %zu", data.len);
		return MGMT_ERR_EINVAL;
	}

	/* Bounds check */
	if ((uint64_t)off + data.len > FLASH_PROTECTED_START) {
		LOG_ERR("write: would touch protected region (off=0x%x len=%zu)", off, data.len);
		return MGMT_ERR_EACCESSDENIED;
	}

	rc = flash_write(flash_dev, off, data.value, data.len);
	if (rc < 0) {
		LOG_ERR("write: flash_write failed at 0x%x: %d", off, rc);
	}

	bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, rc);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int flash_mgmt_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	uint32_t off = 0, len = 0;
	size_t decoded;
	int rc;
	uint8_t buf[FLASH_MGMT_MAX_DATA];

	struct zcbor_map_decode_key_val decode_map[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &len),
	};

	if (zcbor_map_decode_bulk(zsd, decode_map, ARRAY_SIZE(decode_map),
				  &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	if (len == 0 || len > FLASH_MGMT_MAX_DATA) {
		return MGMT_ERR_EINVAL;
	}

	/* Bounds check */
	if ((uint64_t)off + len > FLASH_PROTECTED_START) {
		return MGMT_ERR_EACCESSDENIED;
	}

	rc = flash_read(flash_dev, off, buf, len);
	if (rc < 0) {
		LOG_ERR("read: flash_read failed at 0x%x: %d", off, rc);
		bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, rc);
		return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	bool ok = zcbor_tstr_put_lit(zse, "rc") &&
		  zcbor_int32_put(zse, 0) &&
		  zcbor_tstr_put_lit(zse, "data") &&
		  zcbor_bstr_encode_ptr(zse, buf, len);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/*
 * Commit: schedule RAM trampoline to erase+copy+reset.
 *
 * The host must first stage the firmware in a safe flash area (e.g., slot 1
 * at 0x5C000) using the erase+write commands. Then send commit with the
 * staging offset and firmware length.
 *
 * The handler sends the SMP response immediately, then after 500ms the RAM
 * trampoline runs: disables all interrupts, erases the target area, copies
 * firmware from staging to 0x0, and resets the MCU.
 *
 * Request:  {"stg": uint, "len": uint}
 * Response: {"rc": 0}
 */
static int flash_mgmt_commit(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	uint32_t stg = 0, len = 0;
	size_t decoded;

	struct zcbor_map_decode_key_val decode_map[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("stg", zcbor_uint32_decode, &stg),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &len),
	};

	if (zcbor_map_decode_bulk(zsd, decode_map, ARRAY_SIZE(decode_map),
				  &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	/* Validate staging area */
	if (len == 0 || (uint64_t)stg + len > FLASH_PROTECTED_START) {
		return MGMT_ERR_EINVAL;
	}

	/* Firmware must fit before protected region */
	uint32_t erase_end = (len + (FLASH_SECTOR_SIZE - 1))
			     & ~(FLASH_SECTOR_SIZE - 1);
	if (erase_end > FLASH_PROTECTED_START) {
		return MGMT_ERR_EINVAL;
	}

	/* Staging area must not overlap with target area (0x0..erase_end) */
	if (stg < erase_end) {
		return MGMT_ERR_EINVAL;
	}

	/* Store parameters for trampoline (in DLM, survives flash erase) */
	commit_stg_off = stg;
	commit_fw_len = len;

	LOG_INF("commit: stg=0x%x len=0x%x — scheduling restore", stg, len);

	/* Clear flash write protection (BP bits in status register).
	 * The flash chip has hardware write protection on 0x0-0x7FFFF.
	 * The raw flash_erase_sector_ram() used by the trampoline does not
	 * clear this — erase silently fails without it. */
	flash_unlock();

	/* Schedule trampoline 500ms from now (allows SMP response to be sent) */
	k_work_schedule(&commit_work, K_MSEC(500));

	bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static const struct mgmt_handler flash_mgmt_handlers[] = {
	[FLASH_MGMT_ID_ERASE]  = { .mh_read = NULL, .mh_write = flash_mgmt_erase },
	[FLASH_MGMT_ID_WRITE]  = { .mh_read = NULL, .mh_write = flash_mgmt_write },
	[FLASH_MGMT_ID_READ]   = { .mh_read = flash_mgmt_read, .mh_write = NULL },
	[FLASH_MGMT_ID_COMMIT] = { .mh_read = NULL, .mh_write = flash_mgmt_commit },
};

static struct mgmt_group flash_mgmt_group = {
	.mg_handlers = flash_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(flash_mgmt_handlers),
	.mg_group_id = MGMT_GROUP_ID_PERUSER,
};

static void flash_mgmt_register(void)
{
	flash_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
	if (!device_is_ready(flash_dev)) {
		LOG_ERR("flash device not ready");
		return;
	}
	mgmt_register_group(&flash_mgmt_group);
	LOG_INF("flash_mgmt registered (group %d)", MGMT_GROUP_ID_PERUSER);
}

MCUMGR_HANDLER_DEFINE(flash_mgmt, flash_mgmt_register);
