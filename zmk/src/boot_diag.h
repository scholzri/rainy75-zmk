/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot Diagnostic — .noinit SRAM buffer for debugging boot sequence.
 *
 * Records boot stage codes to a .noinit SRAM buffer (36 bytes).
 * Also writes to PE_OUT and PD_OUT registers for SWS readback,
 * and to analog register 0x3E (frozen before BLE blob starts).
 *
 * Buffer layout (36 bytes):
 *   [0x00..0x03] magic   = 0xB007D1A6 (little-endian)
 *   [0x04]       count   = number of stage entries written
 *   [0x05]       last    = most recent stage code (quick check)
 *   [0x06..0x07] reserved
 *   [0x08..0x23] stages  = up to 28 stage code entries
 *
 * Also restores PA7 SWS function at POST_KERNEL so BDT activate
 * works while firmware is running (GPIO driver reconfigures PA7).
 */

#ifndef BOOT_DIAG_H
#define BOOT_DIAG_H

#include <stdint.h>

/* Stage codes — monotonically increasing through boot sequence */

/* Zephyr init levels */
#define BOOT_DIAG_EARLY             0x01
#define BOOT_DIAG_PRE_KERNEL_1      0x10
#define BOOT_DIAG_PRE_KERNEL_2      0x20
#define BOOT_DIAG_POST_KERNEL       0x30
#define BOOT_DIAG_APPLICATION       0x40

/* APPLICATION sub-stages (listed in execution order) */
#define BOOT_DIAG_MCUBOOT_CONFIRM   0x48  /* APPLICATION/90: WDT + confirm */

/* USB attach substages (APPLICATION/96: called from usb_dc_attach) */
#define BOOT_DIAG_USB_CLOCK         0x50
#define BOOT_DIAG_USB_POWER         0x51
#define BOOT_DIAG_USB_PINS          0x52
#define BOOT_DIAG_USB_IRQ_MODE      0x53
#define BOOT_DIAG_USB_EP_SETUP      0x54
#define BOOT_DIAG_USB_IRQ_CONNECT   0x55
#define BOOT_DIAG_USB_DP_PULLUP     0x56
#define BOOT_DIAG_USB_ATTACHED      0x57

/* BLE substages (bt_enable → hci_b91_open → b91_bt_controller_init) */
#define BOOT_DIAG_BLE_OPEN          0x60  /* hci_b91_open entered */
#define BOOT_DIAG_BLE_RF_INIT       0x61  /* rf_drv_ble_init */
#define BOOT_DIAG_BLE_BLC_INIT      0x62  /* b91_bt_blc_init entered */
#define BOOT_DIAG_BLE_MAC           0x63  /* MAC address read */
#define BOOT_DIAG_BLE_LL_INIT       0x64  /* link layer modules init */
#define BOOT_DIAG_BLE_BUFS          0x65  /* FIFO buffer init */
#define BOOT_DIAG_BLE_HCI           0x66  /* HCI handler registration */
#define BOOT_DIAG_BLE_PM            0x67  /* power management init */
#define BOOT_DIAG_BLE_CHECK         0x68  /* buffer check passed */
#define BOOT_DIAG_BLE_THREAD        0x69  /* controller thread started */
#define BOOT_DIAG_BLE_DONE          0x6A  /* b91_bt_controller_init done */

/* APPLICATION checkpoint stages (between BLE init and USB) */
#define BOOT_DIAG_POST_BLE          0x6B  /* APPLICATION/51: zmk_ble_init done */

/* HCI communication diagnostics */
#define BOOT_DIAG_HCI_FIRST_EVT    0x6D  /* first HCI event received from controller */
#define BOOT_DIAG_HCI_FIRST_CMD    0x6E  /* first HCI command sent to controller */
#define BOOT_DIAG_CTRL_LOOP        0x70  /* controller thread: first main_loop return */
#define BOOT_DIAG_HCI_RX_CALL      0x71  /* hci_rx_handler called (FIFO has data) */
#define BOOT_DIAG_HCI_TX_CALL      0x72  /* hci_tx_handler called (FIFO has data) */

/* ZMK BLE host stack milestones */
#define BOOT_DIAG_BT_ENABLE_OK     0x80  /* bt_enable(NULL) returned 0 */
#define BOOT_DIAG_BT_ENABLE_FAIL   0x81  /* bt_enable(NULL) returned error */
#define BOOT_DIAG_SETTINGS_LOAD    0x82  /* settings_load() completed */
#define BOOT_DIAG_BLE_COMPLETE     0x83  /* zmk_ble_complete_startup() entered */
#define BOOT_DIAG_ADV_START_OK     0x84  /* bt_le_adv_start() returned 0 */
#define BOOT_DIAG_ADV_START_FAIL   0x85  /* bt_le_adv_start() failed */

/* Post-boot milestone (5s delayed work queue) */
#define BOOT_DIAG_RUNNING           0xAA

/* Error marker — next stage byte contains error code */
#define BOOT_DIAG_ERROR             0xEE

#define BOOT_DIAG_MAGIC             0xB007D1A6U
#define BOOT_DIAG_MAX_ENTRIES       28

struct boot_diag_buffer {
	uint32_t magic;
	uint8_t  count;
	uint8_t  last;
	uint8_t  reserved[2];
	uint8_t  stages[BOOT_DIAG_MAX_ENTRIES];
};

#ifdef CONFIG_BOOT_DIAG

void boot_diag_record(uint8_t stage);
void boot_diag_freeze_flash(void);
void boot_diag_freeze_analog(void);

#else /* !CONFIG_BOOT_DIAG */

static inline void boot_diag_record(uint8_t stage) { (void)stage; }
static inline void boot_diag_freeze_flash(void) {}
static inline void boot_diag_freeze_analog(void) {}

#endif /* CONFIG_BOOT_DIAG */

#endif /* BOOT_DIAG_H */
