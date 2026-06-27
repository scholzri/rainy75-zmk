/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telink B91 USB Device Controller — Register Definitions
 *
 * Register map derived from Telink BLE SDK usb_reg.h (Apache 2.0).
 * All offsets are relative to the USB register base at 0x80100800.
 * Access via sys_read8(base + offset) / sys_write8(val, base + offset).
 */

#ifndef UDC_B91_H
#define UDC_B91_H

#include <zephyr/sys/util.h>

/* ---------------------------------------------------------------------------
 * Control Endpoint (EP0) Registers
 * EP0 has a fixed 8-byte FIFO — this is a hardware limitation.
 * Read/write via pointer (reset to 0) + data register (auto-increments).
 * --------------------------------------------------------------------------- */

#define B91_USB_CTRL_EP_PTR        0x00  /* EP0 FIFO pointer (write 0 to reset) */
#define B91_USB_CTRL_EP_DAT        0x01  /* EP0 data byte (R/W, auto-increment) */
#define B91_USB_CTRL_EP_CTRL       0x02  /* EP0 control (ACK/STALL) */
#define B91_USB_CTRL_EP_IRQ_STA    0x03  /* EP0 IRQ status (write-1-to-clear) */
#define B91_USB_CTRL_EP_IRQ_MODE   0x04  /* EP0 auto/manual mode control */

/* B91_USB_CTRL_EP_CTRL bits */
#define B91_EP_DAT_ACK             BIT(0)  /* ACK the data phase */
#define B91_EP_DAT_STALL           BIT(1)  /* STALL the data phase */
#define B91_EP_STA_ACK             BIT(2)  /* ACK the status phase */
#define B91_EP_STA_STALL           BIT(3)  /* STALL the status phase */

/* B91_USB_CTRL_EP_IRQ_STA bits (write-1-to-clear) */
#define B91_CTRL_EP_IRQ_TRANS      0x0F    /* Bits 0-3: transaction status */
#define B91_CTRL_EP_IRQ_SETUP      BIT(4)  /* SETUP packet received */
#define B91_CTRL_EP_IRQ_DATA       BIT(5)  /* DATA phase complete */
#define B91_CTRL_EP_IRQ_STA        BIT(6)  /* STATUS phase complete */
#define B91_CTRL_EP_IRQ_INTF       BIT(7)  /* SET_INTERFACE received */

/* B91_USB_CTRL_EP_IRQ_MODE bits
 * SET a bit = hardware auto-handles that request type
 * CLEAR a bit = software (manual/interrupt) handles it */
#define B91_CTRL_EP_AUTO_ADDR      BIT(0)  /* Auto SET_ADDRESS */
#define B91_CTRL_EP_AUTO_CFG       BIT(1)  /* Auto SET_CONFIGURATION */
#define B91_CTRL_EP_AUTO_INTF      BIT(2)  /* Auto SET_INTERFACE */
#define B91_CTRL_EP_AUTO_STA       BIT(3)  /* Auto status phase */
#define B91_CTRL_EP_AUTO_SYN       BIT(4)  /* Auto sync */
#define B91_CTRL_EP_AUTO_DESC      BIT(5)  /* Auto GET_DESCRIPTOR */
#define B91_CTRL_EP_AUTO_FEAT      BIT(6)  /* Auto SET/CLEAR_FEATURE */
#define B91_CTRL_EP_AUTO_STD       BIT(7)  /* Auto standard requests */

/* All auto bits OR'd together for convenience */
#define B91_CTRL_EP_AUTO_ALL       0xFF

/* ---------------------------------------------------------------------------
 * USB Global Control Registers
 * --------------------------------------------------------------------------- */

#define B91_USB_CTRL               0x05  /* USB control register */
#define B91_USB_CYCL_CALI          0x06  /* SOF cycle calibration (16-bit) */
#define B91_USB_MDEV               0x0A  /* Device mode/features */
#define B91_USB_HOST_CONN          0x0B  /* Host connection status */
#define B91_USB_HOST_CONN_CONFIGURED BIT(7) /* Set by HW on SET_CONFIGURATION (SDK) */
#define B91_USB_SUPS_CYC_CALI      0x0C  /* Suspend cycle calibration */
#define B91_USB_INTF_ALT           0x0D  /* Interface alternate setting */
#define B91_USB_EDP_EN             0x0E  /* Endpoint enable register */
#define B91_USB_IRQ_MASK           0x0F  /* USB IRQ mask + status (shared) */

/* B91_USB_CTRL bits */
#define B91_USB_CTRL_AUTO_CLK      BIT(0)
#define B91_USB_CTRL_LOW_SPD       BIT(1)
#define B91_USB_CTRL_LOW_JITT      BIT(2)
#define B91_USB_CTRL_TST_MODE      BIT(3)

/* B91_USB_MDEV bits */
#define B91_USB_MDEV_SELF_PWR      BIT(0)
#define B91_USB_MDEV_SUSP_STA      BIT(1)
#define B91_USB_MDEV_WAKE_FEA      BIT(2)
#define B91_USB_MDEV_VEND_CMD      BIT(3)
#define B91_USB_MDEV_VEND_DIS      BIT(4)

/* B91_USB_EDP_EN bits — UNUSUAL ordering: bit 0 = EP8, bit 1 = EP1, etc. */
#define B91_USB_EDP8_EN            BIT(0)
#define B91_USB_EDP1_EN            BIT(1)
#define B91_USB_EDP2_EN            BIT(2)
#define B91_USB_EDP3_EN            BIT(3)
#define B91_USB_EDP4_EN            BIT(4)
#define B91_USB_EDP5_EN            BIT(5)
#define B91_USB_EDP6_EN            BIT(6)
#define B91_USB_EDP7_EN            BIT(7)

/* B91_USB_IRQ_MASK bits — mask (bits 0-2) and status (bits 5-7) share byte.
 * Status bits are write-1-to-clear via |= (not direct assignment). */
#define B91_USB_IRQ_RESET_MASK     BIT(0)
#define B91_USB_IRQ_250US_MASK     BIT(1)
#define B91_USB_IRQ_SUSPEND_MASK   BIT(2)
#define B91_USB_IRQ_RESET_LVL      BIT(3)  /* Read-only: reset level */
#define B91_USB_IRQ_250US_LVL      BIT(4)  /* Read-only: 250us level */
#define B91_USB_IRQ_RESET_O        BIT(5)  /* Status: reset occurred (w1c) */
#define B91_USB_IRQ_250US_O        BIT(6)  /* Status: 250us tick (w1c) */
#define B91_USB_IRQ_SUSPEND_O      BIT(7)  /* Status: suspend occurred (w1c) */

/* ---------------------------------------------------------------------------
 * Data Endpoint Registers (EP1-EP8)
 * Indexed by (ep_num & 0x07). EP8 maps to index 0 in the register bank.
 * --------------------------------------------------------------------------- */

#define B91_USB_EP_PTR(ep)         (0x10 + ((ep) & 0x07))  /* FIFO pointer */
#define B91_USB_EP_DAT(ep)         (0x18 + ((ep) & 0x07))  /* Data byte R/W */
#define B91_USB_EP_CTRL(ep)        (0x20 + ((ep) & 0x07))  /* Control */
#define B91_USB_EP_BUF_ADDR(ep)    (0x28 + ((ep) & 0x07))  /* Buffer addr (8B units) */

/* B91_USB_EP_CTRL bits */
#define B91_USB_EP_BUSY            BIT(0)  /* Set by SW to trigger TX; cleared by HW on completion */
#define B91_USB_EP_STALL           BIT(1)  /* Respond with STALL handshake */
#define B91_USB_EP_DAT0            BIT(2)  /* DATA0 toggle */
#define B91_USB_EP_DAT1            BIT(3)  /* DATA1 toggle */
#define B91_USB_EP_MONO            BIT(6)  /* Monophonic mode (audio) */
#define B91_USB_EP_EOF_ISO         BIT(7)  /* EOF for isochronous */

/* ---------------------------------------------------------------------------
 * Global Endpoint Registers
 * --------------------------------------------------------------------------- */

#define B91_USB_RAM_CTRL           0x30  /* USB SRAM power control */
#define B91_USB_ISO_MODE           0x38  /* Isochronous mode config */
#define B91_USB_EP_IRQ_STATUS      0x39  /* EP IRQ status (bit per EP, w1c) */
#define B91_USB_EP_IRQ_MASK        0x3A  /* EP IRQ mask (bit per EP) */
#define B91_USB_EP8_SEND_MAX       0x3B  /* EP8 max send size */
#define B91_USB_EP8_SEND_THRES     0x3C  /* EP8 send threshold */
#define B91_USB_EP8_FIFO_MODE      0x3D  /* EP8 FIFO mode control */
#define B91_USB_EP_MAX_SIZE        0x3E  /* Max packet size for all EPs (value = MPS/8) */

/* B91_USB_EP_IRQ bits — same unusual ordering as EDP_EN */
#define B91_USB_EDP8_IRQ           BIT(0)
#define B91_USB_EDP1_IRQ           BIT(1)
#define B91_USB_EDP2_IRQ           BIT(2)
#define B91_USB_EDP3_IRQ           BIT(3)
#define B91_USB_EDP4_IRQ           BIT(4)
#define B91_USB_EDP5_IRQ           BIT(5)
#define B91_USB_EDP6_IRQ           BIT(6)
#define B91_USB_EDP7_IRQ           BIT(7)

/* B91_USB_RAM_CTRL bits */
#define B91_USB_CEN_PWR_DN         BIT(0)
#define B91_USB_CLK_PWR_DN         BIT(1)
#define B91_USB_WEN_PWR_DN         BIT(3)
#define B91_USB_CEN_FUNC           BIT(4)

/* ---------------------------------------------------------------------------
 * Clock and Reset Registers (outside USB register block)
 * Absolute addresses in SoC address space.
 * --------------------------------------------------------------------------- */

#define B91_REG_RST0               0x801401E0UL  /* Peripheral reset control */
#define B91_REG_CLK_EN0            0x801401E4UL  /* Peripheral clock enable */
#define B91_FLD_RST0_USB           BIT(3)        /* USB reset bit */
#define B91_FLD_CLK0_USB_EN        BIT(3)        /* USB clock enable bit */

/* Analog register for DP pullup: analog reg 0x0B, bit 7 */
#define B91_ANALOG_REG_USB_DP      0x0B
#define B91_ANALOG_DP_PULLUP       BIT(7)

/* Analog register for USB power: analog reg 0x7D, bit 1 */
#define B91_ANALOG_REG_USB_PWR     0x7D
#define B91_ANALOG_USB_PWR_DN      BIT(1)

/* SWire/USB coexistence register */
#define B91_REG_SWIRE_USB          0x80100C01UL
#define B91_SWIRE_USB_EN           BIT(7)

/* ---------------------------------------------------------------------------
 * Analog Register Access (serial interface)
 *
 * B91 analog registers are NOT memory-mapped. They use a serial protocol
 * via the ALG (analog) register block at 0x80140180.
 *
 * Register layout (from SDK analog_reg.h):
 *   +0x00 (0x80140180) — reg_ana_addr  : target analog register address
 *   +0x02 (0x80140182) — reg_ana_ctrl  : control + busy status
 *   +0x03 (0x80140183) — reg_ana_len   : transfer length (set to 1 for byte)
 *   +0x04 (0x80140184) — reg_ana_data  : data byte(s)
 *   +0x08 (0x80140188) — reg_ana_buf_cnt : TX/RX buffer count
 *
 * Read protocol:  addr → len=1 → ctrl=CYC → wait !BUSY → read data
 * Write protocol: addr → data → wait TX buf → ctrl=CYC|RW → wait !BUSY → ctrl=0
 * --------------------------------------------------------------------------- */

#define B91_ANALOG_ADDR_REG        0x80140180UL
#define B91_ANALOG_CTRL_REG        0x80140182UL
#define B91_ANALOG_LEN_REG         0x80140183UL
#define B91_ANALOG_DATA_REG        0x80140184UL
#define B91_ANALOG_BUF_CNT_REG     0x80140188UL

/* reg_ana_ctrl bits (all in the same register at +0x02) */
#define B91_ANALOG_CTRL_RW         BIT(5)  /* 1=write, 0=read */
#define B91_ANALOG_CTRL_CYC        BIT(6)  /* cycle/trigger */
#define B91_ANALOG_CTRL_BUSY       BIT(7)  /* busy status (read-only) */

/* reg_ana_buf_cnt bits */
#define B91_ANALOG_TX_BUFCNT       0xF0    /* bits 4-7: TX buffer count */

/* ---------------------------------------------------------------------------
 * GPIO Registers for USB Pin Setup (PA5=DM, PA6=DP)
 *
 * Register layout (from SDK gpio_reg.h):
 *   0x80140301  reg_gpio_pa_ie    — input enable
 *   0x80140306  reg_gpio_pa_gpio  — GPIO mode (1=GPIO, 0=peripheral)
 *   0x80140331  reg_gpio_pa_fuc_h — function mux for PA4-PA7 (2 bits/pin)
 *
 * PA5 mux bits [2:3] in fuc_h, PA6 mux bits [4:5] in fuc_h.
 * USB function = 0 (clear mux bits).
 * --------------------------------------------------------------------------- */

/* Function mux register for PA4-PA7 (2 bits per pin) */
#define B91_GPIO_PA_FUC_H          0x80140331UL
#define B91_GPIO_PA5_MUX_MASK      (BIT(2) | BIT(3))
#define B91_GPIO_PA6_MUX_MASK      (BIT(4) | BIT(5))

/* GPIO mode register (1=GPIO mode, 0=peripheral mode) */
#define B91_GPIO_PA_GPIO_EN        0x80140306UL
/* Input enable register */
#define B91_GPIO_PA_INPUT_EN       0x80140301UL
#define B91_GPIO_PA5_BIT           BIT(5)
#define B91_GPIO_PA6_BIT           BIT(6)

/* ---------------------------------------------------------------------------
 * IRQ Event Bitmask (for ISR → thread communication)
 * --------------------------------------------------------------------------- */

#define B91_IRQ_EP0_SETUP          BIT(0)
#define B91_IRQ_EP0_DATA           BIT(1)
#define B91_IRQ_EP0_STATUS         BIT(2)
#define B91_IRQ_USB_RESET          BIT(3)
/* Bits 4-12: EP1-EP8 completion (bit 4+ep_num) */
#define B91_IRQ_EP_COMPLETE(n)     BIT(4 + (n))

/* ---------------------------------------------------------------------------
 * EP0 Max Packet Size
 * --------------------------------------------------------------------------- */

#define B91_EP0_MPS                8  /* Fixed by hardware */

/* ---------------------------------------------------------------------------
 * Helper: Map EP number to enable/IRQ register bit
 *
 * The EDP_EN and EP_IRQ registers have unusual bit ordering:
 *   BIT(0) = EP8, BIT(1) = EP1, BIT(2) = EP2, ..., BIT(7) = EP7
 * --------------------------------------------------------------------------- */

static inline uint8_t b91_ep_bit(uint8_t ep_num)
{
	if (ep_num == 8) {
		return BIT(0);
	}
	if (ep_num >= 1 && ep_num <= 7) {
		return BIT(ep_num);
	}
	return 0;  /* EP0 has no enable bit */
}

/* ---------------------------------------------------------------------------
 * Timing Calibration
 *
 * The SDK writes 0x38 to reg_usb_sups_cyc_cali before every EP0 phase.
 * This is required for correct USB timing; the exact reason is undocumented.
 * --------------------------------------------------------------------------- */

#define B91_USB_TIMING_CALIB       0x38

#endif /* UDC_B91_H */
