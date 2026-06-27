/*
 * Copyright (c) 2022 Telink Semiconductor (Shanghai) Co., Ltd.
 * Copyright (c) 2025 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE controller shim for Telink B91 (TLSR951x).
 *
 * Bridges Zephyr's BT HCI driver to the BLE controller blob
 * (liblt_9518_zephyr.a). All blob API functions are declared as
 * extern here to avoid including SDK headers that conflict with
 * Zephyr's own type definitions.
 *
 * Ported from hal_telink's b91_bt.c / b91_bt_init.c / b91_bt_buffer.c
 * with adaptations for Zephyr v4.x and our own Kconfig namespace.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/irq_multilevel.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include "b91_bt.h"

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(b91_bt);

/* -------------------------------------------------------------------------
 * Blob SDK type aliases
 * The blob uses u8/u16/u32 — these are defined in the SDK headers but
 * we avoid including those. Provide them here.
 * ----------------------------------------------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;

/* -------------------------------------------------------------------------
 * Hardware register definitions (direct access, avoiding SDK headers)
 * ----------------------------------------------------------------------- */

/* System timer — 16 MHz free-running counter */
#define B91_REG_SYSTEM_TICK     0x80140200UL
#define B91_SYSTEM_TICK_1MS     16000
#define B91_PM_SLEEP_ADJ_MS     2

/* PLIC interrupt enable register for context 0.
 * The blob manages system timer IRQ via direct register writes
 * (see SDK ext_misc.h:systimer_irq_enable). We must re-enable it
 * before yielding so BLE timing events fire during k_sleep(). */
#define B91_PLIC_IRQ_EN0        0xe4002000UL
#define B91_IRQ1_SYSTIMER       1

/* Analog register serial interface (shared with USB driver).
 * Layout from SDK analog_reg.h: addr +0, ctrl +2, len +3, data +4. */
#define B91_ANA_ADDR_REG        0x80140180UL
#define B91_ANA_CTRL_REG        0x80140182UL
#define B91_ANA_LEN_REG         0x80140183UL
#define B91_ANA_DATA_REG        0x80140184UL
#define B91_ANA_CTRL_CYC        BIT(6)  /* cycle/trigger */
#define B91_ANA_CTRL_BUSY       BIT(7)  /* busy status */

/* PM wakeup status (read from analog reg 0x64) */
#define B91_WAKEUP_STATUS_TIMER BIT(1)
#define B91_STATUS_ENTER_SUSPEND BIT(30)

/* BLE PM sleep mask bits */
#define B91_PM_SLEEP_LEG_ADV    BIT(0)
#define B91_PM_SLEEP_ACL_SLAVE  BIT(2)

/* PM function pointer types — must match blob signatures */
typedef int (*b91_cpu_pm_handler_t)(u32 sleep_mode, u32 wakeup_src,
				    u32 wakeup_tick);
typedef u32 (*b91_pm_recover_t)(u32);

/* blt_miscParam layout — must match blob's struct exactly */
typedef struct {
	u8 ext_cap_en;
	u8 pad32k_en;
	u8 pm_enter_en;
	u8 rsvd;
} b91_misc_para_t;

/* -------------------------------------------------------------------------
 * Utility functions required by the blob (swapN, swapX)
 * ----------------------------------------------------------------------- */
void swapN(unsigned char *p, int n)
{
	int i, c;

	for (i = 0; i < n / 2; i++) {
		c = p[i];
		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

void swapX(const u8 *src, u8 *dst, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		dst[len - 1 - i] = src[i];
	}
}

/* -------------------------------------------------------------------------
 * Weak stubs for blob symbols not used in BLE-peripheral-only mode
 * ----------------------------------------------------------------------- */

/* GATT push — blob references this but Zephyr host handles GATT */
int __attribute__((weak)) blc_gatt_pushHandleValueNotify(u16 connHandle,
							 u16 attHandle,
							 u8 *p, int len)
{
	return 0;
}

/* OTA callbacks — not used */
void __attribute__((weak)) host_ota_main_loop_cb(void) {}
void __attribute__((weak)) host_ota_terminate_cb(u16 connHandle, u8 reason) {}

/* USB upper tester — not used */
void __attribute__((weak)) usb_send_upper_tester_result(u8 *data, u32 len) {}

/* -------------------------------------------------------------------------
 * HCI FIFO structure — must match blob's hci_fifo_t layout exactly.
 *
 * The blob uses hci_fifo_t (hci.h:58) for bltHci_rxfifo/bltHci_txfifo,
 * NOT the generic my_fifo_t (which has u16 num instead of u8 num + u8 mask).
 *
 * Layout verified against: modules/hal/hal_telink/tlsr9/ble/stack/ble/hci/hci.h
 * ----------------------------------------------------------------------- */
typedef struct {
	u32 size;
	u8  num;
	u8  mask;
	u8  wptr;
	u8  rptr;
	u8 *p;
} hci_fifo_t;

/* -------------------------------------------------------------------------
 * Blob API extern declarations
 *
 * All signatures verified against:
 *   - modules/hal/hal_telink/tlsr9/ble/stack/ble/controller/ll/ll.h
 *   - modules/hal/hal_telink/tlsr9/ble/stack/ble/hci/hci.h
 *   - modules/hal/hal_telink/tlsr9/ble/vendor/controller/b91_bt_init.c
 *   - nm --defined-only zmk/lib/liblt_9518_zephyr.a
 * ----------------------------------------------------------------------- */

/* RF / TRNG */
extern void rf_drv_ble_init(void);
extern void trng_init(void);
extern void rf_set_power_level_index(int level);

/* Flash */
extern void flash_read_page(u32 addr, u32 len, u8 *buf);
extern void flash_write_page(u32 addr, u32 len, u8 *buf);

/* Random */
extern void generateRandomNum(int len, u8 *data);

/* Link Layer init */
extern void blc_ll_initBasicMCU(void);
extern void blc_ll_initStandby_module(u8 *mac);
extern void blc_ll_initLegacyAdvertising_module(void);
extern void blc_ll_initLegacyScanning_module(void);
extern void blc_ll_initLegacyInitiating_module(void);
extern void blc_ll_initAclConnection_module(void);
extern void blc_ll_initAclSlaveRole_module(void);

/* ACL buffer init */
extern int blc_ll_initAclConnRxFifo(u8 *buf, int size, int num);
extern int blc_ll_initAclConnSlaveTxFifo(u8 *buf, int size, int num, int connNum);
extern void blc_ll_setAclConnMaxOctetsNumber(int maxRxOct, int maxMasTxOct, int maxSlvTxOct);

/* Connection config */
extern void blc_ll_setMaxConnectionNumber(int masterNum, int slaveNum);
extern void blc_ll_setAclMasterConnectionInterval(int intervalIdx);
extern void blc_ll_setCreateConnectionTimeout(int timeoutMs);

/* PHY / CSA */
extern void blc_ll_init2MPhyCodedPhy_feature(void);
extern void blc_ll_initChannelSelectionAlgorithm_2_feature(void);

/* HCI FIFO init */
extern int blc_ll_initHciRxFifo(u8 *buf, int size, int num);
extern int blc_ll_initHciTxFifo(u8 *buf, int size, int num);
extern int blc_ll_initHciAclDataFifo(u8 *buf, int size, int num);

/* HCI handlers */
extern int blc_hci_sendACLData2Host(void);
extern int blc_hci_send_data(void);
extern void blc_hci_registerControllerDataHandler(void *handler);
extern void blc_hci_registerControllerEventHandler(void *handler);
extern void blc_hci_setEventMask_cmd(u32 mask);
extern void blc_hci_le_setEventMask_cmd(u32 mask);
extern void blc_hci_le_setEventMask_2_cmd(u32 mask);

/* Controller check */
extern u8 blc_controller_check_appBufferInitialization(void);

/* HCI register + handler */
extern void blc_register_hci_handler(void *prx, void *ptx);
extern int blc_hci_handler(u8 *p, int n);

/* Main loop + IRQ */
extern void blc_sdk_main_loop(void);
extern void blc_sdk_irq_handler(void);

/* Power management module */
extern void blc_ll_initPowerManagement_module(void);
extern void blc_pm_setSleepMask(u32 mask);
/* PM function pointers (writable — we override these) */
extern b91_cpu_pm_handler_t cpu_sleep_wakeup;
extern b91_pm_recover_t pm_tim_recover;

/* PM implementations for internal 32k RC oscillator */
extern int cpu_sleep_wakeup_32k_rc(u32 sleep_mode, u32 wakeup_src,
				   u32 wakeup_tick);
extern u32 pm_tim_recover_32k_rc(u32);

/* Misc params — controls PM entry permission */
extern b91_misc_para_t blt_miscParam __attribute__((aligned(4)));

/* 32k RC oscillator calibration */
extern void clock_cal_32k_rc(void);

/* HCI FIFOs — defined in blob, accessed by our TX/RX handlers.
 * Declared as hci_fifo_t per hci.h (NOT my_fifo_t which has u16 num). */
extern hci_fifo_t bltHci_rxfifo;
extern hci_fifo_t bltHci_txfifo;

/* -------------------------------------------------------------------------
 * Buffer sizes — from SDK ext_misc.h macros
 *
 * HCI_FIFO_SIZE(n)           = ALIGN16(n + 2 + 4)
 * CAL_LL_ACL_RX_FIFO_SIZE(n) = ALIGN16(n + 21)
 * CAL_LL_ACL_TX_FIFO_SIZE(n) = ALIGN16(n + 10)
 * ----------------------------------------------------------------------- */
#define ALIGN16(x) (((x) + 15) / 16 * 16)
#define HCI_FIFO_SIZE(n)           ALIGN16((n) + 2 + 4)
#define CAL_LL_ACL_RX_FIFO_SIZE(n) ALIGN16((n) + 21)
#define CAL_LL_ACL_TX_FIFO_SIZE(n) ALIGN16((n) + 10)

/* Use Zephyr's BT buffer config defaults:
 *   BT_BUF_ACL_TX_SIZE = 27 (default), BT_BUF_ACL_TX_COUNT = 3
 *   BT_BUF_RX_SIZE     = 76 (default), BT_BUF_RX_COUNT     = 3
 *   BT_BUF_CMD_TX_SIZE = 65, BT_BUF_CMD_TX_COUNT = 2
 *
 * BT_BUF_TX_SIZE = MAX(CMD, ACL) for HCI RX direction (host→controller)
 */
#ifndef CONFIG_BT_BUF_ACL_TX_SIZE
#define CONFIG_BT_BUF_ACL_TX_SIZE 27
#endif
#ifndef CONFIG_BT_BUF_ACL_TX_COUNT
#define CONFIG_BT_BUF_ACL_TX_COUNT 3
#endif
#ifndef CONFIG_BT_BUF_CMD_TX_SIZE
#define CONFIG_BT_BUF_CMD_TX_SIZE 65
#endif

/* Zephyr BT_BUF_RX_SIZE is not a simple config — use a safe default */
#define BT_RX_BUF_SIZE 76

/* Calculate sizes for BLE SDK buffers */
#define BT_BUF_TX_SIZE_MAX MAX(CONFIG_BT_BUF_CMD_TX_SIZE + 4, \
			       CONFIG_BT_BUF_ACL_TX_SIZE + 4)

/* ACL connection RX/TX FIFO */
#define ACL_CONN_MAX_RX_OCTETS  MIN(BT_RX_BUF_SIZE, 251)
#define ACL_SLAVE_MAX_TX_OCTETS MIN(CONFIG_BT_BUF_ACL_TX_SIZE, 251)

#define ACL_RX_FIFO_SIZE    CAL_LL_ACL_RX_FIFO_SIZE(BT_RX_BUF_SIZE)
#define ACL_RX_FIFO_NUM     8  /* power of 2 */
#define ACL_SLAVE_TX_FIFO_SIZE  CAL_LL_ACL_TX_FIFO_SIZE(CONFIG_BT_BUF_ACL_TX_SIZE)
#define ACL_SLAVE_TX_FIFO_NUM   9  /* 9, 17, or 33 per SDK */

/* HCI FIFOs */
#define HCI_RX_FIFO_SIZE    HCI_FIFO_SIZE(BT_BUF_TX_SIZE_MAX)
#define HCI_RX_FIFO_NUM     4  /* power of 2 */
#define HCI_TX_FIFO_SIZE    HCI_FIFO_SIZE(BT_RX_BUF_SIZE)
#define HCI_TX_FIFO_NUM     4  /* power of 2 */
#define HCI_RX_ACL_FIFO_SIZE ((CONFIG_BT_BUF_ACL_TX_SIZE + 4 + 3) & ~3) /* align 4 */
#define HCI_RX_ACL_FIFO_NUM 4  /* power of 2 */

/* -------------------------------------------------------------------------
 * Buffer allocations
 * ----------------------------------------------------------------------- */
static u8 app_acl_rxfifo[ACL_RX_FIFO_SIZE * ACL_RX_FIFO_NUM];
static u8 app_acl_slvTxfifo[ACL_SLAVE_TX_FIFO_SIZE * ACL_SLAVE_TX_FIFO_NUM];
static u8 app_hci_rxfifo[HCI_RX_FIFO_SIZE * HCI_RX_FIFO_NUM];
static u8 app_hci_txfifo[HCI_TX_FIFO_SIZE * HCI_TX_FIFO_NUM];
static u8 app_hci_rxAclfifo[HCI_RX_ACL_FIFO_SIZE * HCI_RX_ACL_FIFO_NUM];

/* -------------------------------------------------------------------------
 * Controller state
 * ----------------------------------------------------------------------- */
#define BLE_THREAD_PERIOD_MS  2

/* 32k RC calibration interval (seconds) */
#define RC_CAL_INTERVAL_SEC   10

/* MAC address flash offset — 1MB flash uses 0xFF000 */
#define MAC_FLASH_ADDR 0xFF000

#define BLE_SUCCESS 0

#define BYTES_TO_UINT16(n, p) do { (n) = ((u16)(p)[0] + ((u16)(p)[1] << 8)); } while (0)
#define BSTREAM_TO_UINT16(n, p) do { BYTES_TO_UINT16(n, p); (p) += 2; } while (0)

static struct {
	b91_bt_host_callback_t callbacks;
} b91_ctrl;

/* -------------------------------------------------------------------------
 * MAC address init — ported from hal_telink b91_bt_init.c
 * ----------------------------------------------------------------------- */
static void b91_bt_blc_mac_init(int flash_addr, u8 *mac_public,
				u8 *mac_random_static)
{
	if (flash_addr == 0) {
		return;
	}

	u8 mac_read[8];

	flash_read_page(flash_addr, 8, mac_read);

	u8 value_rand[5];

	generateRandomNum(5, value_rand);

	u8 ff_six_byte[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	if (memcmp(mac_read, ff_six_byte, 6)) {
		memcpy(mac_public, mac_read, 6);
	} else {
		mac_public[0] = value_rand[0];
		mac_public[1] = value_rand[1];
		mac_public[2] = value_rand[2];
		mac_public[3] = 0x38; /* company id: 0xA4C138 */
		mac_public[4] = 0xC1;
		mac_public[5] = 0xA4;

		flash_write_page(flash_addr, 6, mac_public);
	}

	mac_random_static[0] = mac_public[0];
	mac_random_static[1] = mac_public[1];
	mac_random_static[2] = mac_public[2];
	mac_random_static[5] = 0xC0; /* random static marker */

	u16 high_2_byte = (mac_read[6] | mac_read[7] << 8);

	if (high_2_byte != 0xFFFF) {
		memcpy(&mac_random_static[3], &mac_read[6], 2);
	} else {
		mac_random_static[3] = value_rand[3];
		mac_random_static[4] = value_rand[4];

		flash_write_page(flash_addr + 6, 2, &mac_random_static[3]);
	}
}

/* -------------------------------------------------------------------------
 * HCI TX handler — blob calls this to send data to host
 * ----------------------------------------------------------------------- */
static int b91_bt_hci_tx_handler(void)
{
	if (bltHci_txfifo.wptr == bltHci_txfifo.rptr) {
		return 0;
	}

	u8 *p = bltHci_txfifo.p +
		(bltHci_txfifo.rptr & bltHci_txfifo.mask) * bltHci_txfifo.size;

	if (p) {
		u32 len;

		BSTREAM_TO_UINT16(len, p);
		bltHci_txfifo.rptr++;

		if (b91_ctrl.callbacks.host_read_packet) {
			b91_ctrl.callbacks.host_read_packet(p, len);
		}
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * HCI RX handler — blob calls this to receive data from host
 * ----------------------------------------------------------------------- */
static int b91_bt_hci_rx_handler(void)
{
	if (bltHci_rxfifo.wptr == bltHci_rxfifo.rptr) {
		if (b91_ctrl.callbacks.host_send_available) {
			b91_ctrl.callbacks.host_send_available();
		}
		return 0;
	}

	u8 *p = bltHci_rxfifo.p +
		(bltHci_rxfifo.rptr & bltHci_rxfifo.mask) * bltHci_rxfifo.size;

	if (p) {
		blc_hci_handler(&p[0], 0);
		bltHci_rxfifo.rptr++;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Analog register access (serial interface, not memory-mapped)
 * ----------------------------------------------------------------------- */

static u8 b91_analog_read(u8 addr)
{
	unsigned int key = irq_lock();

	sys_write8(addr, B91_ANA_ADDR_REG);
	sys_write8(1, B91_ANA_LEN_REG);
	sys_write8(B91_ANA_CTRL_CYC, B91_ANA_CTRL_REG);
	while (sys_read8(B91_ANA_CTRL_REG) & B91_ANA_CTRL_BUSY) {
	}
	u8 val = sys_read8(B91_ANA_DATA_REG);

	irq_unlock(key);
	return val;
}

#ifdef CONFIG_POWEROFF
/* -------------------------------------------------------------------------
 * PM wakeup handler — replaces cpu_sleep_wakeup function pointer
 *
 * When the BLE blob wants to sleep between events, it calls
 * cpu_sleep_wakeup(). We override this to yield via k_sleep()
 * so other Zephyr threads can run during the wait.
 * Pattern from hal_telink b91_bt_init.c:b91_bt_zephyr_wakeup().
 * ----------------------------------------------------------------------- */

static int b91_bt_zephyr_wakeup(u32 sleep_mode, u32 wakeup_src,
				u32 wakeup_tick)
{
	ARG_UNUSED(sleep_mode);
	ARG_UNUSED(wakeup_src);

	/* Re-enable system timer IRQ (PLIC source 1) so BLE timing events fire */
	sys_write32(sys_read32(B91_PLIC_IRQ_EN0) | BIT(B91_IRQ1_SYSTIMER),
		    B91_PLIC_IRQ_EN0);

	/* Snapshot current system tick + Zephyr uptime atomically */
	k_sched_lock();
	u32 sys_tick = sys_read32(B91_REG_SYSTEM_TICK);
	int64_t ktime_ms = k_uptime_get();
	k_sched_unlock();

	/* Convert blob's wakeup_tick (16 MHz system timer) to absolute kernel time */
	ktime_ms += (s32)(wakeup_tick - sys_tick) / B91_SYSTEM_TICK_1MS
		    - B91_PM_SLEEP_ADJ_MS;

	/* Ensure MSTATUS.MIE is set so k_sleep can be woken by timer interrupt.
	 * The blob may have cleared MIE before calling us; without it, the
	 * scheduler tick interrupt never fires and k_sleep hangs forever. */
	__asm__ volatile("csrs mstatus, %0" :: "r"(1 << 3));

	/* Yield the controller thread until the BLE event time.
	 * This allows bt_rx, ZMK, and other threads to run during the wait. */
	k_sleep(K_TIMEOUT_ABS_MS(ktime_ms));

	return B91_WAKEUP_STATUS_TIMER | B91_STATUS_ENTER_SUSPEND;
}
#endif /* CONFIG_POWEROFF */

/* -------------------------------------------------------------------------
 * 32k RC oscillator calibration — periodic work item
 *
 * The internal 32k RC oscillator drifts with temperature. Periodic
 * calibration against the 16 MHz crystal maintains BLE sleep timing
 * accuracy. Called every RC_CAL_INTERVAL_SEC seconds.
 * ----------------------------------------------------------------------- */

static void b91_32k_rc_cal_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(b91_32k_rc_cal_work, b91_32k_rc_cal_handler);

static void b91_32k_rc_cal_handler(struct k_work *work)
{
	clock_cal_32k_rc();
	k_work_reschedule(&b91_32k_rc_cal_work,
			  K_SECONDS(RC_CAL_INTERVAL_SEC));
}

/* -------------------------------------------------------------------------
 * BLC init sequence — ported from hal_telink b91_bt_init.c
 *
 * Peripheral-only configuration: 0 masters, 1 slave.
 * ----------------------------------------------------------------------- */
static int b91_bt_blc_init(void *prx, void *ptx)
{
	/* TRNG init (random_generator_init = trng_init in SDK) */
	trng_init();

	/* MAC address */
	u8 mac_public[6];
	u8 mac_random_static[6];

	b91_bt_blc_mac_init(MAC_FLASH_ADDR, mac_public, mac_random_static);

	/* Core init */
	blc_ll_initBasicMCU();
	blc_ll_initStandby_module(mac_public);

	/* Advertising (mandatory for peripheral) */
	blc_ll_initLegacyAdvertising_module();

	/* Scanning + initiating — initialized unconditionally per HAL reference.
	 * Even in peripheral-only mode, the blob may reference internal state
	 * from these modules. The HAL always calls both regardless of role. */
	blc_ll_initLegacyScanning_module();
	blc_ll_initLegacyInitiating_module();

	/* Connection */
	blc_ll_initAclConnection_module();
	blc_ll_initAclSlaveRole_module();

	blc_ll_setAclConnMaxOctetsNumber(ACL_CONN_MAX_RX_OCTETS, 0,
					 ACL_SLAVE_MAX_TX_OCTETS);

	/* ACL RX FIFO (shared) */
	if (blc_ll_initAclConnRxFifo(app_acl_rxfifo, ACL_RX_FIFO_SIZE,
				     ACL_RX_FIFO_NUM) != BLE_SUCCESS) {
		return -1;
	}

	/* ACL Slave TX FIFO */
	if (blc_ll_initAclConnSlaveTxFifo(app_acl_slvTxfifo, ACL_SLAVE_TX_FIFO_SIZE,
					  ACL_SLAVE_TX_FIFO_NUM, 1) != BLE_SUCCESS) {
		return -1;
	}

	/* NOTE: blc_ll_ConfigLegacyAdvEnable_by_API_only() and
	 * blc_ll_ConfigLegacyScanEnable_by_API_only() are called in the
	 * HAL reference but not available in our blob version. Skip them;
	 * default strategy should be fine for peripheral-only mode. */

	/* Connection limits */
	blc_ll_setMaxConnectionNumber(0, 1); /* 0 master, 1 slave (blob is single-conn) */

	/* RF power */
	rf_set_power_level_index(11); /* ~0 dBm, reasonable default */

	/* PHY features */
	blc_ll_initChannelSelectionAlgorithm_2_feature();
	/* 2M PHY DISABLED: the blob's 2M PHY implementation has radio issues.
	 * After the central requests PHY update to 2M, the LL fails to maintain
	 * the connection — packets are lost, causing LL Response Timeout (0x22)
	 * exactly 40 seconds after the PHY switch. 1M PHY works fine. */

	/* HCI RX FIFO (host → controller) */
	if (blc_ll_initHciRxFifo(app_hci_rxfifo, HCI_RX_FIFO_SIZE,
				 HCI_RX_FIFO_NUM) != BLE_SUCCESS) {
		return -1;
	}

	/* HCI TX FIFO (controller → host) */
	if (blc_ll_initHciTxFifo(app_hci_txfifo, HCI_TX_FIFO_SIZE,
				 HCI_TX_FIFO_NUM) != BLE_SUCCESS) {
		return -1;
	}

	/* HCI ACL FIFO */
	if (blc_ll_initHciAclDataFifo(app_hci_rxAclfifo, HCI_RX_ACL_FIFO_SIZE,
				      HCI_RX_ACL_FIFO_NUM) != BLE_SUCCESS) {
		return -1;
	}

	/* HCI event/data handlers */
	blc_hci_registerControllerDataHandler(blc_hci_sendACLData2Host);
	blc_hci_registerControllerEventHandler(blc_hci_send_data);

	/* Event masks */
	blc_hci_setEventMask_cmd(0x10); /* HCI_EVT_MASK_DISCONNECTION_COMPLETE */
	blc_hci_le_setEventMask_cmd(0xFFFFFFFF);
	blc_hci_le_setEventMask_2_cmd(0x7FFFFFFF);

	/* Validate buffer init */
	u8 check = blc_controller_check_appBufferInitialization();

	if (check != BLE_SUCCESS) {
		LOG_ERR("BLC buffer check failed: %u", check);
		return -1;
	}

	/* Register HCI handlers */
	blc_register_hci_handler(prx, ptx);

#ifdef CONFIG_POWEROFF
	/* ---- Power management ----
	 * Enable BLE inter-event sleep: the blob calls cpu_sleep_wakeup
	 * between advertising/connection events. Our override converts
	 * this to k_sleep() so other Zephyr threads can run. */
	blc_ll_initPowerManagement_module();
	cpu_sleep_wakeup = b91_bt_zephyr_wakeup;
	pm_tim_recover = pm_tim_recover_32k_rc;
	blt_miscParam.pm_enter_en = 1;
	blc_pm_setSleepMask(B91_PM_SLEEP_LEG_ADV | B91_PM_SLEEP_ACL_SLAVE);
#endif /* CONFIG_POWEROFF */

	/* Initial 32k RC calibration */
	clock_cal_32k_rc();

	return 0;
}

/* -------------------------------------------------------------------------
 * IRQ handlers — called from Zephyr PLIC
 * ----------------------------------------------------------------------- */
static void rf_irq_handler(const void *arg)
{
	ARG_UNUSED(arg);
	blc_sdk_irq_handler();
}

static void stimer_irq_handler(const void *arg)
{
	ARG_UNUSED(arg);
	blc_sdk_irq_handler();
}

/* -------------------------------------------------------------------------
 * Controller thread
 * ----------------------------------------------------------------------- */
static void b91_bt_controller_thread(void *p1, void *p2, void *p3);

/* Use dynamic thread creation instead of K_THREAD_DEFINE.
 * K_THREAD_DEFINE with delay=-1 + k_thread_start() never executes
 * on B91 — possibly a linker section or thread init issue.
 * Dynamic creation with k_thread_create() is more reliable. */
static K_THREAD_STACK_DEFINE(ctrl_stack, CONFIG_BT_HCI_B91_RX_STACK_SIZE);
static struct k_thread ctrl_thread_data;

static void b91_bt_controller_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		blc_sdk_main_loop();
#ifndef CONFIG_PM
		k_sleep(K_MSEC(BLE_THREAD_PERIOD_MS));
#endif
	}
}

/* -------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
int b91_bt_controller_init(void)
{
	int status;

	/* IRQs — System Timer (PLIC source 1) and RF (PLIC source 15).
	 *
	 * CRITICAL: Must use multi-level encoded IRQ numbers, not raw
	 * PLIC source numbers. On B91, the PLIC is a 2nd-level interrupt
	 * controller connected via machine external interrupt 11:
	 *   encoded = IRQ_TO_L2(source) | parent_irq
	 *           = ((source + 1) << 8) | 11
	 *
	 * With raw numbers (1, 15), IRQ_CONNECT installs handlers at
	 * _sw_isr_table[1] and [15], but the PLIC driver dispatches via
	 * _sw_isr_table[CONFIG_2ND_LVL_ISR_TBL_OFFSET + source] = [13]
	 * and [27]. The handlers never get called and advertising fails
	 * because the LL scheduler can't fire RF events.
	 *
	 * The blob enables PLIC bits internally via direct register writes
	 * to 0xe4002000 during rf_drv_ble_init() and blc_ll_initBasicMCU().
	 * No separate irq_enable() call needed. */
#define BLE_STIMER_IRQ  (IRQ_TO_L2(1) | 11)    /* PLIC source 1 */
#define BLE_RF_IRQ      (IRQ_TO_L2(15) | 11)   /* PLIC source 15 */
	IRQ_CONNECT(BLE_STIMER_IRQ, 0, stimer_irq_handler, NULL, 0);
	IRQ_CONNECT(BLE_RF_IRQ, 0, rf_irq_handler, NULL, 0);

	/* RF driver init */
	rf_drv_ble_init();

	/* BLC stack init */
	status = b91_bt_blc_init(b91_bt_hci_rx_handler, b91_bt_hci_tx_handler);
	if (status != 0) {
		LOG_ERR("BLC init failed: %d", status);
		return status;
	}

	/* Start controller thread — dynamic creation */
	k_thread_create(&ctrl_thread_data, ctrl_stack,
			K_THREAD_STACK_SIZEOF(ctrl_stack),
			b91_bt_controller_thread, NULL, NULL, NULL,
			CONFIG_BT_HCI_B91_RX_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ctrl_thread_data, "ble_ctrl");

	/* Start periodic 32k RC oscillator calibration */
	k_work_schedule(&b91_32k_rc_cal_work, K_SECONDS(RC_CAL_INTERVAL_SEC));

	LOG_DBG("B91 BLE controller initialized");
	return 0;
}

void b91_bt_host_send_packet(uint8_t type, uint8_t *data, uint16_t len)
{
	u8 *p = bltHci_rxfifo.p +
		(bltHci_rxfifo.wptr & bltHci_rxfifo.mask) * bltHci_rxfifo.size;

	*p++ = type;
	memcpy(p, data, len);
	bltHci_rxfifo.wptr++;
}

/* Deep sleep entry is handled by z_sys_poweroff() in poweroff.c.
 * The old pm_state_set/pm_state_exit_post_ops stubs were removed —
 * ZMK uses sys_poweroff() (not Zephyr idle PM) for deep sleep. */

void b91_bt_host_callback_register(const b91_bt_host_callback_t *pcb)
{
	b91_ctrl.callbacks.host_read_packet = pcb->host_read_packet;
	b91_ctrl.callbacks.host_send_available = pcb->host_send_available;
}
