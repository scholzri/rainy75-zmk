/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telink B91 PSPI + DMA Register Definitions for WS2812 LED strip driver.
 *
 * Register addresses cross-verified against:
 *   - Telink B91 SDK dma_reg.h / spi_reg.h / dma.h / spi.h
 *   - Decompiled original firmware (secondary_pipeline, hid_report_build)
 * PSPI base: 0x80140040 (APB bus peripheral SPI).
 * DMA base: 0x80100400, per-channel stride: 0x14.
 */

#ifndef B91_PSPI_H
#define B91_PSPI_H

#include <zephyr/sys/util.h>

/* -------------------------------------------------------------------------
 * PSPI Registers (base 0x80140040)
 *
 * Reference: SDK reg_include/spi_reg.h, PSPI_BASE_ADDR = 0x140040
 * ----------------------------------------------------------------------- */

#define B91_PSPI_BASE               0x80140040UL

#define B91_PSPI_MODE0              (B91_PSPI_BASE + 0x00)  /* [7]=master [6:5]=CPOL/CPHA [4]=dual [3]=LSB [2]=3line [1:0]=cs2sclk */
#define B91_PSPI_MODE1              (B91_PSPI_BASE + 0x01)  /* Clock divider: spi_clk = pclk / ((div+1)*2) */
#define B91_PSPI_MODE2              (B91_PSPI_BASE + 0x02)  /* [2]=cmd_en [1]=quad [0]=cmd_fmt [7:4]=csht */
#define B91_PSPI_TX_CNT0            (B91_PSPI_BASE + 0x03)  /* TX byte count [7:0], stores (count-1) */
#define B91_PSPI_TRANS0             (B91_PSPI_BASE + 0x05)  /* [7:4]=transmode [3:0]=dummy_cnt */
#define B91_PSPI_TRANS1             (B91_PSPI_BASE + 0x06)  /* Command register (write triggers transfer) */
#define B91_PSPI_TRANS2             (B91_PSPI_BASE + 0x07)  /* [7]=TX_DMA [6]=RX_DMA [4]=end_int [3:0]=FIFO ints */
#define B91_PSPI_DATA0              (B91_PSPI_BASE + 0x08)  /* TX FIFO data / DMA dest (0x80140048) */
#define B91_PSPI_FIFO_STATE         (B91_PSPI_BASE + 0x0D)  /* [3]=txf_clr [2]=rxf_clr [7:4]=flags */
#define B91_PSPI_TX_CNT1            (B91_PSPI_BASE + 0x12)  /* TX byte count [15:8] */
#define B91_PSPI_TX_CNT2            (B91_PSPI_BASE + 0x13)  /* TX byte count [23:16] */

/* B91_PSPI_MODE0 bits */
#define B91_PSPI_MODE0_MASTER       BIT(7)  /* 1=master, 0=slave */

/* Clock divider: spi_clk = PCLK / ((div+1)*2). PCLK=24MHz, div=1 → 6 MHz.
 * 6 MHz is the only correct value for the 0xF0/0xC0 WS2812 encoding. */
#define B91_PSPI_CLK_DIV            0x01

/* B91_PSPI_TRANS0 bits [7:4] — transfer mode */
#define B91_PSPI_TRANS0_WRITE_ONLY  (1 << 4)  /* Mode 0x1: write only */

/* B91_PSPI_TRANS2 bits */
#define B91_PSPI_TRANS2_TX_DMA_EN   BIT(7)  /* Enable TX DMA */
#define B91_PSPI_TRANS2_END_INT_EN  BIT(4)  /* Enable SPI transmit-end interrupt */

/* SPI interrupt-status register — SDK reg_spi_irq_state(i) @ +0x0E.
 * BIT(6) = FLD_SPI_END_INT: set when the last bit has been clocked out
 * (transfer fully done). Write-1-to-clear. */
#define B91_PSPI_IRQ_STATE          (B91_PSPI_BASE + 0x0E)
#define B91_PSPI_END_INT            BIT(6)

/* PLIC source 23 = IRQ23_SPI_APB (dedicated to the APB/PSPI peripheral).
 * Zephyr 2nd-level encoded IRQ: IRQ_TO_L2(23) | parent(11). */
#define B91_PSPI_IRQ_SRC            23

/* B91_PSPI_FIFO_STATE bits */
#define B91_PSPI_FIFO_CLR_TX        BIT(3)  /* Clear TX FIFO (write 1) */

/* SPI status register — SDK reg_spi_status(i) @ +0x0F
 * BIT(7) = FLD_HSPI_BUSY: 1 while SPI is clocking data, 0 when idle.
 * Used after DMA to ensure FIFO is fully drained before WS2812 reset. */
#define B91_PSPI_STATUS             (B91_PSPI_BASE + 0x0F)
#define B91_PSPI_BUSY               BIT(7)

/* -------------------------------------------------------------------------
 * DMA Registers (base 0x80100400)
 *
 * Per-channel stride: 0x14 (20 bytes). Channels 0-7.
 * Channel base = 0x80100444 + ch * 0x14.
 *
 * Reference: SDK reg_include/dma_reg.h
 *   reg_dma_ctrl(i)     = REG_ADDR32(0x100444 + i*0x14)
 *   reg_dma_src_addr(i) = REG_ADDR32(0x100448 + i*0x14)
 *   reg_dma_dst_addr(i) = REG_ADDR32(0x10044C + i*0x14)
 *   reg_dma_size(i)     = REG_ADDR32(0x100450 + i*0x14)
 * ----------------------------------------------------------------------- */

#define B91_DMA_BASE                0x80100400UL
#define B91_DMA_CH_STRIDE           0x14

/* Per-channel register offsets (from channel base) */
#define B91_DMA_CTRL                0x00    /* [0]=enable/busy [1:3]=irq_mask [4:8]=dst_req [9:13]=src_req ... */
#define B91_DMA_SRC_ADDR            0x04    /* Source address (must be 4-byte aligned) */
#define B91_DMA_DST_ADDR            0x08    /* Destination address (must be 4-byte aligned) */
#define B91_DMA_SIZE                0x0C    /* Transfer size: [21:0]=word_count [23:22]=tail_bytes */

/* Channel base address helper */
#define B91_DMA_CH_BASE(ch)         (B91_DMA_BASE + 0x44 + (ch) * B91_DMA_CH_STRIDE)

/* DMA request selection — bits [4:8] of CTRL register (dst_req_sel) */
#define B91_DMA_REQ_PSPI_TX         4       /* DMA_REQ_SPI_APB_TX */

/* -------------------------------------------------------------------------
 * GPIO Registers for PB7 pin mux (PSPI MOSI)
 * ----------------------------------------------------------------------- */

#define B91_GPIO_PB_OEN             0x8014030AUL  /* PB output enable (0=enabled) */
#define B91_GPIO_PB_FUC_H           0x80140333UL  /* PB[7:4] function mux high */
#define B91_GPIO_PB_GPIO            0x8014030EUL  /* PB GPIO mode enable */

/* -------------------------------------------------------------------------
 * GPIO Registers for PC2 (LED power enable)
 *
 * PC2 controls a MOSFET gate for LED VCC. PC2 HIGH = power ON (confirmed
 * on hardware). Not part of the keyboard matrix (matrix uses PC1).
 * Reference: decompiled secondary_pipeline() @ 0x2000efc8
 * ----------------------------------------------------------------------- */

#define B91_GPIO_PC_OEN             0x80140312UL  /* PC output enable (0=enabled) */
#define B91_GPIO_PC_OUT             0x80140313UL  /* PC output data */
#define B91_GPIO_PC_GPIO            0x80140316UL  /* PC GPIO mode enable */

/* PB7 PSPI MOSI: FUC_H bits[7:6] = 01, GPIO bit7 = 0
 * NOTE: PB5 (PSPI CLK) is a keyboard matrix column pin — must NOT be
 * reconfigured. PSPI internal clock runs without CLK routed to a pin. */
#define B91_PB7_FUC_MASK            (BIT(7) | BIT(6))
#define B91_PB7_FUC_PSPI_MOSI      BIT(6)        /* Function 1 = PSPI MOSI IO0 */

/* -------------------------------------------------------------------------
 * Analog Register Access (serial interface)
 *
 * B91 analog registers are NOT memory-mapped. They use a serial protocol
 * via the ALG (analog) register block at 0x80140180.
 *
 * Reference: SDK analog_reg.h
 *   +0x00 (0x80140180) — reg_ana_addr    : target analog register address
 *   +0x02 (0x80140182) — reg_ana_ctrl    : control + busy status
 *   +0x03 (0x80140183) — reg_ana_len     : transfer length (set to 1 for byte)
 *   +0x04 (0x80140184) — reg_ana_data    : data byte(s)
 *   +0x08 (0x80140188) — reg_ana_buf_cnt : TX/RX buffer count
 *
 * Read protocol:  addr → len=1 → ctrl=CYC → wait !BUSY → read data
 * Write protocol: addr → data → wait TX buf → ctrl=CYC|RW → wait !BUSY → ctrl=0
 * ----------------------------------------------------------------------- */

#define B91_ANALOG_ADDR_REG         0x80140180UL
#define B91_ANALOG_CTRL_REG         0x80140182UL
#define B91_ANALOG_LEN_REG          0x80140183UL
#define B91_ANALOG_DATA_REG         0x80140184UL
#define B91_ANALOG_BUF_CNT_REG      0x80140188UL

#define B91_ANALOG_CTRL_RW          BIT(5)  /* 1=write, 0=read */
#define B91_ANALOG_CTRL_CYC         BIT(6)  /* cycle/trigger */
#define B91_ANALOG_CTRL_BUSY        BIT(7)  /* busy status (read-only) */
#define B91_ANALOG_TX_BUFCNT        0xF0    /* bits 4-7: TX buffer count */

/* -------------------------------------------------------------------------
 * Analog Register Addresses for PC2 LED Power
 *
 * PC2 input enable and pull-up are in the analog register space.
 * Reference: SDK gpio_analog.h, confirmed in decompiled secondary_pipeline()
 * ----------------------------------------------------------------------- */

#define B91_ANA_PC_IE               0xBD    /* Port C input enable (analog) */
#define B91_ANA_PC_PULL             0x12    /* Port C pull-up/down config */
#define B91_ANA_PC2_PULL_MASK       0x30    /* PC2 pull bits [5:4] */
#define B91_ANA_PC2_PULL_10K        0x20    /* PC2 pull-up 10K */

/* -------------------------------------------------------------------------
 * DMA address translation
 *
 * DMA uses C-bus addresses. CPU SRAM addresses below 0xA0000 need
 * translation by adding 0xC0180000 for DMA access.
 * Reference: SDK core.h convert_ram_addr_cpu2bus()
 * ----------------------------------------------------------------------- */

#define B91_DMA_CBUS_OFFSET         0xC0180000UL

static inline uint32_t b91_dma_addr(const void *ptr)
{
	uint32_t addr = (uint32_t)ptr;
	if (addr < 0xA0000) {
		addr += B91_DMA_CBUS_OFFSET;
	}
	return addr;
}

/* -------------------------------------------------------------------------
 * WS2812 SPI encoding constants
 *
 * At 6 MHz SPI clock, each SPI bit = 166.67 ns.
 * WS2812 "1" bit: high 4 clocks + low 4 clocks = 0xF0 = 667ns/667ns
 * WS2812 "0" bit: high 2 clocks + low 6 clocks = 0xC0 = 333ns/1000ns
 * WS2812B spec: T1H=580-1000ns, T0H=220-380ns — within tolerance.
 * ----------------------------------------------------------------------- */

#define WS2812_SPI_ONE              0xF0
#define WS2812_SPI_ZERO             0xC0
#define WS2812_SPI_BYTES_PER_LED    24  /* 3 colors × 8 bits × 1 SPI byte/bit */

/* WS2812 reset: >=280us low (V5/C variant). We use k_busy_wait(500) for margin. */
#define WS2812_RESET_US             500

#endif /* B91_PSPI_H */
