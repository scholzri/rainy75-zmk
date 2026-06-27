# Firmware Analysis

## Ghidra Setup

- **Binary:** `reverse/firmware/firmware_extracted.bin` (115,364 bytes — firmware body at flash 0x012B0, without 4,784-byte boot header)
- **Language:** `RISCV:LE:32:AndeStar_v5` (Ghidra 12.0.1 has built-in support)
- **Base address:** `0x20001000`
- **Project:** `reverse/ghidra/rainy75_andesv5`

The built-in AndeStar V5 SLEIGH module (`andestar_v5.instr.sinc`, 608 lines) covers all Andes instructions found in firmware: BFOZ/BFOS, BBC/BBS, BEQC/BNEC, LEA, FFB/FFZMISM, EXEC.IT/EX9.IT, GP-relative loads/stores.

Headless scripting note: Ghidra 12.0.1 headless Java scripts have an OSGi bundle error. Use **PyGhidra** standalone API instead (`pip install pyghidra`, then `pyghidra.start(install_dir="/opt/ghidra")`).

## Analysis Results

- **211 functions** total (27% more than the original RV32GC import which found 166)
- **211 named functions** (100% coverage) via combined analysis methods:
  - Round 1: Byte-level SDK matching — 2 functions (LTO blocks most matches)
  - Round 2: Register-constant identification — 39 functions
  - Round 3: Call graph propagation + deep disassembly — 93 functions
  - Round 4: BSim semantic matching — 107 functions (+16 new, 2 renames)
  - Round 5: Domain-based naming from call graph neighbors — 100 more functions named
  - Round 6: Manual analysis of final 4 — `analog_config_writeback`, `ble_analog_irq_config`, `ble_bond_flash_process`, `ble_ll_pdu_fragment`
- **495 register labels** imported from SDK headers (REG_* at memory-mapped addresses)
- **876 equates** imported for bitfield masks (FLD_*)

## Named Functions by Category

| Category | Functions | Key Addresses |
|----------|-----------|---------------|
| **Key Pipeline** | key_pipeline_main, key_process_stage1/2, matrix_scan, matrix_scan_core, matrix_scan_sleep | `0x2000e9f8`, `0x20002ebc`, `0x20001EF4`, `0x2000DA88` |
| **GPIO/Matrix** | gpio_key_init, gpio_pin_init, key_state_update, gpio_set_up_down_res | `0x2000ba10`, `0x2000272C`, `0x2000bb04`, `0x20019624` |
| **Main Loop** | main_loop_body, main_or_user_init, init_config | `0x20011474`, `0x20012b9c`, `0x20011394` |
| **RF/BLE** | rf_init, rf_irq_handler, rf_event_handler, rf_txrx_config, ble_task_handler, etc. | `0x20002ba0`, `0x20013db8`, `0x200056c0`, etc. |
| **Flash** | flash_send_addr, flash_send_cmd, flash_wait_done, flash_mspi_read_ram, flash_4line_dis | `0x2000103c`, `0x20001080`, `0x20001234`, `0x20001324` |
| **System** | sys_clock_init, clock_set_32k_tick, plic_init, analog_write/read_reg8, timer_start | `0x200060c4`, `0x20001574`, `0x20006a14`, `0x200133a0` |
| **Crypto** | hmac_sha224, aes_get_result | `0x20001a7c`, `0x20013a84` |
| **USB** | usb_irq_handler, hid_report_send, usbkb_report_frame | `0x2000c728`, `0x2000c634`, `0x20015354` |
| **Utility** | ring_buffer_push_byte, strnlen, queue_remove, __riscv_save_0, __riscv_restore_1 | `0x20016df0`, `0x20013518`, `0x200135d4` |

## Function Naming Confidence

- **95-100% (~22 functions):** SDK byte matches, BSim sim>=0.9, manually traced, ABI helpers
- **75-94% (~30 functions):** BSim 0.7-0.9, strong register patterns
- **60-74% (~30 functions):** BSim medium, domain propagation
- **50-59% (~17 functions):** Weak domain signals + BSim low
- **<50% (~8 functions):** Gap region functions with minimal evidence

Zero debug symbols exist — every name is inferred.

## SDK Function Matching (BSim)

Byte-level matching is fundamentally incompatible with LTO-compiled binaries (SDK's `.a` files contain GIMPLE IR, not machine code).

**BSim approach (completed):**
1. Built `acl_peripheral_demo` from SDK v4.0.4.5 with Andes GCC 14.2.0 (no LTO)
2. Generated BSim signatures via PyGhidra (211 firmware vs 1949 reference functions)
3. 168 matches found: 21 high (>=0.7), 59 medium (0.5-0.7), 88 low (0.3-0.5)
4. 16 new names applied after filtering attractor false positives
5. 2 existing names corrected to SDK standard

BSim attractor functions (filtered as false positives):
- `tlkapi_debug_handler`: matched 45 functions
- `blc_initMacAddress`: matched 18 functions

### Potential Further Approaches

- String reference cross-mapping (LUI+ADDI pairs -> .rodata strings)
- Constant-tuple fingerprinting (ordered tuple of all constants a function loads)
- Ghidra Version Tracking (structural correlators)
- BinDiff/Diaphora (CFG topology matching)
- Incremental build isolation (progressively complex reference apps)

## Key Pipeline Architecture

```
main_loop_body (1060B, called in infinite loop)
  +-- init_config (204B)
  +-- key_pipeline_main (792B) — called TWICE per loop iteration
  |    +-- hid_report_send (176B)        <-- sends PREVIOUS cycle's report
  |    |    +-- reg_access_util x 4      <-- copies report to BLE/USB
  |    |    +-- ble_gatt_notify (278B)
  |    |    +-- hid_report_build (188B)  <-- builds NEXT report
  |    +-- gpio_set_up_down_res x 22     <-- pull-ups on all matrix pins
  |    +-- key_process_stage1 x 4        <-- GPIO direction config
  |    +-- key_debounce_check (86B)
  |    +-- key_process_stage2 x 2        <-- post-scan processing
  |    +-- gpio_key_reg_write x 7        <-- reads row inputs
  +-- secondary_pipeline (436B)          — wireless/2.4G report path
  +-- gpio_key_init + key_state_update
  +-- hid_report_send
  +-- ble_task_handler (448B)
  +-- main_periodic_task / main_ble_check / main_task_check
```

Firmware architecture: bare-metal cooperative polling loop (NO RTOS).
Main loop: `main() -> sys_init() -> user_init_normal() -> while(1) { main_loop(); }`

## Peripheral Access Map

| Peripheral | Functions | Register Range | Datasheet Section |
|------------|-----------|----------------|-------------------|
| RF_BB (Baseband) | 22 | `0x80140800`-`0x80140BFF` | Ch 5 |
| STIMER (16MHz fixed) | 21 | `0x80140200`-`0x8014021F` | 7.3 |
| DMA | 15 | `0x80100400`-`0x801005FF` | — |
| GPIO | 11 | `0x80140300`-`0x8014035F` | 9.1 |
| PLIC (IRQ) | 10 | `0xE4000000`-`0xE4200000` | Ch 8 |
| MSPI (Flash) | 7 | `0x80140100`-`0x8014013F` | 9.5 |
| CORE_CTRL | 7 | `0x80140000`-`0x8014003F` | — |
| RF_RADIO | 6 | `0x80140E00`-`0x80140FFF` | Ch 5 |
| RF_MODEM | 5 | `0x80140C00`-`0x80140DFF` | Ch 5 |
| TIMER | 5 | `0x80140140`-`0x8014017F` | 7.1 |
| SYS_CTRL | 4 | `0x801401C0`-`0x801401FF` | 4.4, Ch 6 |
| ANALOG | 2 | `0x80140180`-`0x8014018F` | — |
| USB | 1 | `0x80100800`-`0x801008FF` | 9.10 |
| PWM | 1 | `0x80140400`-`0x801404FF` | Ch 10 |
| PSPI | — | `0x80140040`-`0x8014007F` | 9.7 |
| I2C | — | `0x80140280`-`0x801402BF` | 9.3 |
| UART0 | — | `0x80140080`-`0x801400BF` | 9.9 |
| UART1 | — | `0x801400C0`-`0x801400FF` | 9.9 |
| SWIRE | — | `0x80100C00`-`0x80100C0F` | 9.2 |
| SAR ADC | — | analog registers | Ch 11 |
| TRNG | — | — | Ch 16 |
| AES | — | — | Ch 14 |
| PKE | — | — | Ch 15 |

### Software Reset Registers (from datasheet, base `0x801401C0`)

| Offset | Bits | Modules (write 1 = reset) |
|--------|------|---------------------------|
| 0x20 | [0] HSPI, [1] I2C, [2] UART0, [3] USB, [4] PWM0, [6] UART1 | |
| 0x21 | [1] STIMER, [2] DMA, [3] ALGM, [4] PKE, **[6] PSPI**, [7] SPI_SLV | |
| 0x22 | [0] TIMER, [2] TRNG, [3] MCU reset disable, [4] MCU reset enable, [5] LM | |
| 0x23 | [0] ZB, [1] ZB_MSTCLK, [2] ZB_LPCLK, [3] ZB_CRYPT, [4] MSPI, [6] SARADC, [7] ALG | |
| 0x2f | [0] suspend enable, [5] reset all (acts as watchdog reset), [7] stall MCU | |

### Retention Analog Registers (from datasheet, Section 4.3)

These registers survive deep sleep and can be used to track boot source:

| Address | Survives | Reset Value | Purpose |
|---------|----------|-------------|---------|
| afe_0x38 | deep sleep only (cleared by WDT/SW reset) | 0xFF | Watchdog tracking flag |
| afe_0x39-0x3f | deep sleep + WDT + SW reset (cleared only by POR) | 0x00 (0x3f=0x0F) | Program state across sleep cycles |

Firmware likely uses these to distinguish POR vs. deep sleep wakeup vs. watchdog reset.

## Key State Buffer Layout (SRAM)

All keyboard state is stored relative to the `gp` (global pointer) register.

### Key Scan Buffers

| GP Offset | Size | Purpose |
|-----------|------|---------|
| `gp+0x32C` | 8 bytes | Current key state (8 rows x 1 byte bitmask) |
| `gp+0x338` | 8 bytes | Previous key state (for delta detection) |
| `gp+0x1204` | 16 bytes | Extended key state or NKRO bitmap |
| `gp+0x2F4` | 3 bytes | Modifier key state |
| `gp+0x36C` | 3 bytes | Modifier state copy/shadow |
| `gp+0x348` | 2 bytes | Debounce counter |
| `gp+0xF4` | 16 bytes | LED color config (R, G, B, checksum) — **corrected:** not debounce state |

### HID Report Buffers

| GP Offset | Size | Purpose |
|-----------|------|---------|
| `gp+0x14d8` | 1992 bytes (498 DMA words) | RGB LED SPI buffer (83 LEDs × 24 encoded bytes, WS2812 DMA source) |
| `gp+0x13DC` | 249 bytes | Main HID report buffer |
| `gp+0x1314` | ~83 bytes | RGB LED index mapping (key-to-LED, per-key effect) |
| `gp+0x1294` | ~83 bytes | Previous/shadow report |
| `gp+0x1214` | 23B x 4 | Layer keymap buffers |
| `gp+0x34C` | 8 bytes | Consumer keys report |
| `gp+0x318` | 8 bytes | System keys report |
| `gp+0x2E8` | 8 bytes | Mouse report |

### Control Flags

| GP Offset | Size | Purpose |
|-----------|------|---------|
| `gp+0x1A2` | 1 byte | Busy flag (spin-wait for 0) |
| `gp+0x334` | 1 byte | Report dirty flag |
| `gp+0x1E5` | 1 byte | Set to 1 at start of key_pipeline_main |
| `gp+0x153` | 1 byte | Connection mode (0=BLE, 1=dongle/2.4G, 2=USB) |
| `gp+0x22A` | 1 byte | Key direction flag (0=release, nonzero=press) |
| `gp+0x259` | 1 byte | Connection mode alt (0=USB, else=BLE/wireless) |
| `gp+0x365` | 1 byte | DMA channel index (= 4, PSPI TX) |
| `gp+0x128` | 1 byte | SPI transfer config (lower nibble → reg 0x80140045) |
| `gp+0x129` | 1 byte | SPI GPIO setup flag (bit 2 of reg 0x80140042) |
| `gp+0x124` | 4 bytes | SPI mode selector (0-3, wireless mode variants) |

### Report Dirty Flags — `key_state_flags` (gp+0x300, 1 byte)

Producer-consumer dirty-flag architecture for HID report delivery. 55 total references across 12 functions.

| Bit | Mask | Name | Set By | Cleared By |
|-----|------|------|--------|------------|
| 0 | 0x01 | SCAN_COMPLETE | `matrix_scan` (end of 16-col cycle) | — (indicator only) |
| 1 | 0x02 | KEY_STATE_DIRTY | `ble_stack_init`, `ble_bond_flash_process` | `wireless_main_loop`, `main_loop_body`, `init_bss_section` |
| 2 | 0x04 | NKRO_DIRTY | `ble_init_sub` | `wireless_main_loop`, `main_loop_body`, `init_bss_section` |
| 3 | 0x08 | CONSUMER_DIRTY | `consumer_key_to_hid_translate` | `wireless_main_loop`, `main_loop_body`, `init_bss_section` |
| 4 | 0x10 | MODIFIER_DIRTY | — (bulk via 0x1E) | `wireless_main_loop`, `main_loop_body`, `init_bss_section` |
| 5 | 0x20 | LED_UPDATE | `via_layer_mode_set` (key press) | `via_layer_mode_set` (key release) |
| 6 | 0x40 | ALL_CLEAR | BLE stack/interrupt (not in main code) | Main loops after zeroing all buffers |
| 7 | 0x80 | NKRO_EXTENDED_PENDING | `ble_stack_init`, `ble_bond_flash_process` | Same, after buffer slot found/removed |

`key_buffers_clear` sets bits 1-4 simultaneously (`|= 0x1E`) after zeroing all key state buffers.

**ALL_CLEAR (bit 6) mechanism:** Gates `ble_stack_init`, `ble_init_sub`, `consumer_key_to_hid_translate`, and `ble_bond_flash_process` (all bail out early). Main loops zero all buffers before processing dirty bits. Ensures clean key release during mode transitions/disconnects.

### Report Mode Flags — `report_mode_flags` (gp+0x301, 1 byte)

28 total references across 14 functions.

| Bit | Mask | Name | Set By | Cleared By |
|-----|------|------|--------|------------|
| 0 | 0x01 | BLE_CONNECTED | `ble_pairing_init`, `flash_4line_dis`, `via_layer_mode_set` | — |
| 1 | 0x02 | LED_OVERRIDE | VIA cmd 0x26 (raw byte write) | `report_buffers_clear` |
| 2 | 0x04 | FLASH_LOG_MODE | — | — |
| 5 | 0x20 | KEYMAP_RELOAD | `via_protocol_reset`, `via_device_reset`, VIA cmd 0x27 | `via_layer_mode_set` (key release) |
| 6 | 0x40 | SPECIAL_REPORT_DIRTY | `special_key_dispatch`, `ble_report_set_axes`, `key_buffers_clear` | `wireless_main_loop`, `main_loop_body`, `init_bss_section` |

**LED_OVERRIDE (bit 1):** Normal mode loads RGB from `gp+0x7A/7B/7C` with brightness from `gp+0x97`. Override mode forces fixed RGB(0xA2, 0xA2, 0xA2) (bright white — BLE activity indicator).

**KEYMAP_RELOAD (bit 5):** Set by VIA on protocol/device reset. Consumed by `via_layer_mode_set` on key release, which clears it and sets BLE_CONNECTED, triggering BLE stack reinit if needed.

**Connection mode dispatch:** `gp+0x153` selects the main loop:
- Mode 0 (BLE): `wireless_main_loop` — sends via BLE ATT writes
- Mode 1 (Dongle/2.4GHz): `wireless_main_loop` — sends via flash_util + ble_task_sub
- Mode 2 (USB): `main_loop_body` + `init_bss_section` — sends via `usb_endpoint_send`

All three paths use identical bit-check/clear logic: check ALL_CLEAR → zero buffers if set → process bits 1-4 → process SPECIAL_REPORT_DIRTY.

### hid_report_send Flow

1. Spin-wait on `gp+0x1A2` == 0
2. Set PD output enable (signals activity)
3. `reg_access_util(gp+0x13DC, 0, 0xF9)` — copy 249-byte main report
4. `ble_gatt_notify()` — push via BLE GATT
5. Copy consumer keys, system keys, mouse reports
6. Clear dirty flag, set busy flag
7. Call `hid_report_build()` — DMA setup for next transfer
8. Spin-wait again, clear PD, delay 300us

## SRAM Variable Clusters — Deep Trace Results

Three major SRAM regions traced to near-complete coverage. GP base = `0x00080800`.

### Clock/PM Subsystem (gp+0x0158–0x0169)

Two consecutive Telink SDK structs plus the system clock struct:

**`pm_early_wakeup_time_us_s`** at gp+0x0158 (4 × u16, 8 bytes):

| GP Offset | Type | Name | Description | Confidence |
|-----------|------|------|-------------|------------|
| 0x0158 | u16 | `pm_suspend_early_wakeup_us` | Sleep timing lead time, non-32M PLL path | High |
| 0x015A | u16 | `pm_deep_ret_early_wakeup_us` | Sleep timing lead time, fractional-PLL path | High |
| 0x015C | u16 | `pm_deep_early_wakeup_us` | Sleep timing lead time, 32M crystal path | High |
| 0x015E | u16 | `pm_sleep_min_time_us` | Minimum sleep duration threshold (skip if shorter) | High |

**`pm_r_delay_cycle_s`** at gp+0x0160 (2 × u16, 4 bytes):

| GP Offset | Type | Name | Description | Confidence |
|-----------|------|------|-------------|------------|
| 0x0160 | u16 | `pm_deep_r_delay_cycle` | Analog reg 0x40 restore, 32M path | High |
| 0x0162 | u16 | `pm_suspend_ret_r_delay_cycle` | Analog reg 0x40 restore, standard PLL path | High |

**`sys_clk_t`** at gp+0x0164 (u16 + 4 × u8, 6 bytes):

| GP Offset | Type | Name | Value | Description | Confidence |
|-----------|------|------|-------|-------------|------------|
| 0x0164 | u16 | `sys_clk.pll_clk` | 0xC0 (192) | PLL frequency MHz | High |
| 0x0166 | u8 | `sys_clk.cclk` | 48 | CPU clock MHz | High |
| 0x0167 | u8 | `sys_clk.hclk` | 48 | AHB bus clock MHz | High |
| 0x0168 | u8 | `sys_clk.pclk` | 24 | APB peripheral clock MHz | High |
| 0x0169 | u8 | `sys_clk.mspi_clk` | 48 | MSPI flash clock MHz | High |

All three structs are written by ROM/`sys_init()` at boot; `sys_clk_t` is refined by `sys_post_clock_init`. Clock config: PLL=192MHz, CCLK=48MHz, HCLK=48MHz, PCLK=24MHz, MSPI=48MHz — matches SDK's `CCLK_48M_HCLK_48M_PCLK_24M`.

### VIA / BLE Runtime State (gp+0x0170–0x019C)

Heterogeneous mix of VIA protocol state, macro engine, flash wear-leveling pointers, and BLE connection state.

**VIA layer & dirty flag:**

| GP Offset | Type | Name | Description | Confidence |
|-----------|------|------|-------------|------------|
| 0x0173 | u8 | `via_rx_buf_dirty` | Set on USB EP6/BLE ATT receive, cleared after flash commit | High |
| 0x0174 | i16 | `via_active_layer` | Current VIA layer index (0–3), masked `& 0xF` | High |

**Flash wear-leveling pointer cluster** (5 × i16, gp+0x017A–0x018A):

| GP Offset | Type | Name | Flash Sector | Page Size | Confidence |
|-----------|------|------|-------------|-----------|------------|
| 0x017A | i16 | `via_layer0_flash_page_ptr` | 0x84000 (layer 0) | 0x100 | High |
| 0x017C | i16 | `via_layer1_flash_page_ptr` | 0x85000 (layer 1) | 0x100 | High |
| 0x017E | i16 | `via_layer2_flash_page_ptr` | 0x86000 (layer 2) | 0x100 | High |
| 0x0180 | i16 | `via_layer3_flash_page_ptr` | 0x87000 (layer 3) | 0x100 | High |
| 0x018A | i16 | `via_macro_flash_page_ptr` | 0x89000 (macros) | 0x200 | High |

All 5 pointers initialized to 0 at factory reset; overflow at 0xE00 triggers sector erase + reset. **Correction:** gp+0x0180 is NOT `chip_id` — it is `via_layer3_flash_page_ptr` (the existing SRAM map entry at absolute address 0x00000180 is a different variable).

**Macro engine state:**

| GP Offset | Type | Name | Description | Confidence |
|-----------|------|------|-------------|------------|
| 0x0176 | u8 | `macro_playback_active` | Macro replay in-progress flag | High |
| 0x0178 | u8 | `macro_current_key_byte` | Current macro key byte being replayed | High |
| 0x0182 | u16 | `macro_buf_read_pos` | Byte cursor into gp+0x3C0 macro buffer | High |
| 0x0186 | u8 | `macro_count` | Number of macros loaded from flash | High |
| 0x0187 | u8 | `macro_key_held_flag` | 0xFF=key held, 0x00=advance | High |
| 0x0188 | u16 | `macro_data_remaining` | Macro data available / not-done gate | Medium |

**BLE connection state:**

| GP Offset | Type | Name | Description | Confidence |
|-----------|------|------|-------------|------------|
| 0x018C | u8 | `ble_handshake_phase` | 3-state enum: 0=complete, 1=ACK wait, 2=fresh frame | Medium |
| 0x0194 | u8 | `ble_notify_suppressed` | Gate for BLE ATT HID notifications | Medium |
| 0x0198 | u32 | `ble_conn_start_tick` | System tick at BLE connection start (`\| 1` armed) | High |
| 0x019C | u32 | `ble_conn_elapsed` | Connection duration accumulator (reset to 0) | Low |

### RF DMA & BLE Notification Buffers

**`rf_rx_dma_buf`** at gp+0x0C74 (1024 bytes):

RF RX DMA ring buffer — 4 slots × 256 bytes. Programmed as DMA CH1 destination in `rf_init`:
```c
_REG_DMA_DST_ADDR_CH1 = gp + 0xc74;
REG_RF_RX_WPTR_MASK = 3;    // 4 slots (mask 0..3)
REG_RF_BB_RX_SIZE = 8;      // 8 × 32 = 256 bytes/slot
```
CPU never writes to this buffer — hardware DMA fills it. Occupies SRAM 0x00081474–0x00081873.

**`flash_verify_scratch`** at gp+0x0DC0 (256 bytes):

Flash write-verify scratch buffer used by `key_state_process`:
```c
(*_flash_write_func_ptr)(offset + base, size, src);
(*_flash_read_func_ptr)(offset + base, size, gp + 0xdc0);  // read back
iVar1 = ble_data_util(gp + 0xdc0, src, size);              // memcmp
```
**Overlaps** with tail of RF RX ring (intentional: Telink pattern of reusing RF buffer as scratch during flash writes when RF is off).

**`ble_notify_fifo`** at gp+0x0EC0 (288 bytes = 16 slots × 18 bytes):

BLE ATT notification outbound FIFO. Each slot: 1 type byte + 1 padding + 16 payload bytes.

| Tag | ASCII | ATT Handle | Payload | Report Type |
|-----|-------|------------|---------|-------------|
| 0x23 | `#` | 0x33 | 7 bytes | Keyboard (standard) |
| 0x24 | `$` | 0x29 | 8 bytes | Keyboard (full) |
| 0x25 | `%` | 0x21 | 2 bytes | Consumer/Media |
| 0x26 | `&` | 0x25 | 1 byte | System control |
| 0x27 | `'` | 0x37 | 15 bytes | NKRO |

FIFO state: `gp+0xFE0` = write index (byte), `gp+0xFE1` = read index (byte). Both reset to 0 on connection abort. Max depth: 16 entries (`wireless_main_loop` sends when BLE LL TX FIFO has room: `rom_ble_ll_get_fifo_count < 8`).

### Heterogeneous Range (gp+0x0028–0x01A8) — Key Findings

This range was initially expected to be a BLE LL buffer cluster — **it is not**. It's a heterogeneous mix of unrelated subsystems:

**RGB LED pipeline:**

| GP Offset | Type | Name | Description | Confidence |
|-----------|------|------|-------------|------------|
| 0x0075 | u8 | `rgb_effect_index` | Active RGB effect mode | High |
| 0x007A | u8 | `rgb_default_r` | Stored red default → gp+0x261 | High |
| 0x007B | u8 | `rgb_default_g` | Stored green default → gp+0x21C | High |
| 0x007C | u8 | `rgb_default_b` | Stored blue default → gp+0x1A4 | High |
| 0x0097 | u8 | `rgb_floor_mask` | OR'd into all RGB channels as minimum floor | High |
| 0x01A4 | u8 | `rgb_live_b` | Live blue channel after brightness scaling | High |

Color pipeline: defaults (gp+0x7A/7B/7C) → live values (gp+0x261/21C/1A4) via `ble_ll_util(brightness)`, with `gp+0x97` as minimum floor mask. LED_OVERRIDE mode (gp+0x301 bit 1) forces fixed RGB(0xA2, 0xA2, 0xA2).

**LED color config** (gp+0x00F4, 16 bytes): R, G, B bytes + checksum at offset 3. Written to flash at 0x8A000. **Corrected:** previously labeled `debounce_state` — actual usage is LED color configuration, as confirmed by BLE ATT notification payload pattern (R, G, B, sum).

**SPI/PSPI config:**

| GP Offset | Type | Name | Description | Confidence |
|-----------|------|------|-------------|------------|
| 0x0124 | u32 | `pspi_mode_sel` | SPI mode selector (0–3 for WS2812 modes) | High |
| 0x0128 | u8 | `pspi_bits_per_xfer` | Bits per SPI transfer (→ REG_PSPI_TRANS0) | High |
| 0x0129 | u8 | `pspi_cpha` | Clock phase select (→ REG_PSPI_MODE2 bit 2) | High |

**RF/BLE control:**

| GP Offset | Type | Name | Description | Confidence |
|-----------|------|------|-------------|------------|
| 0x0088 | u8 | `ble_conn_ready_flag` | BLE connection established gate | Medium-High |
| 0x0096 | u8 | `matrix_scan_phase` | Two-pass matrix scan mode flag | High |
| 0x0108 | u32 | `ble_conn_interval_us` | BLE connection interval in µs | High |
| 0x0146 | u8 | `usb_hid_idle_rate` | USB HID Set Idle / Get Idle value | Very High |
| 0x0148 | u32 | `rf_tx_pkt_ptr` | RF TX DMA source buffer pointer (DMA CH0) | Very High |
| 0x0152 | u8 | `ble_link_connected` | BLE connection state (0=disconnected) | High |
| 0x01A1 | u8 | `rf_analog_reg3b_shadow` | Shadow copy of analog reg 0x3B (RF power/sleep) | High |

## Firmware Binary Composition (~112KB)

| Component | Size | Percentage |
|-----------|------|-----------|
| BLE stack (libB91_ble_lib.a) | ~40-50KB | 35-45% |
| Hardware drivers | ~10-15KB | 10-13% |
| 2.4GHz proprietary wireless | ~5-10KB | 5-9% |
| Boot + utilities | ~3-5KB | 3-5% |
| Application code (matrix, layers, VIA, RGB, macros) | ~27-47KB | 25-40% |

## Last 4 Unnamed Functions — Analysis

Disassembled from firmware binary using Andes GCC objdump (standard capstone can't decode Andes V5 extensions).

### FUN_ram_2000ca2c (40 bytes) — `analog_config_writeback`

- **Callers:** 0 (orphan — likely called via function pointer or interrupt)
- **Callees:** 0 (leaf)
- **GP access:** reads `gp+0x0153` (analog_config flag)
- **BSim:** tlkapi_debug_handler (0.447) — false positive (attractor)
- **Behavior:** Splits 16-bit value in a4 into two bytes (high=a1, low=a4), writes both bytes to 6 different memory addresses (t6+180, t6+181, t3+0, a5+0, t1+0, a2+0). Conditional on a1==t5 (early exit). Pattern matches analog register pair writes — scatters a config value to multiple hardware registers.
- **Proposed name:** `analog_config_writeback` — writes analog config value to multiple register copies

### FUN_ram_2000f6bc (54 bytes) — `ble_analog_irq_config`

- **Callers:** 1 (ble_bonding_manager @ 0x20012a5c)
- **Callees:** 1 (jumps to 0x2000e1cc)
- **BSim:** tlkapi_debug_handler (0.430) — false positive (attractor)
- **Behavior:** Classic interrupt-critical register write: `csrr a5, mstatus` → Andes custom ops → `csrrci a5, mstatus, 8` (disable interrupts) → load address 0x3D090 (analog register space) → bitfield operations (andi 63, andi -2) → write → restore. Pattern matches SDK's `analog_write_reg8` with BLE-specific parameters.
- **Proposed name:** `ble_analog_irq_config` — analog register write in interrupt-critical section, called from BLE bonding

### FUN_ram_20011ae8 (648 bytes) — `ble_bond_flash_process`

- **Callers:** 4 (ble_pairing_init x2, ble_bonding_manager x2)
- **Callees:** 4+ (ble_init_sub, mode_dispatch_loop, ble_stack_init, function @ 0x20010f64)
- **BSim:** tlkapi_debug_isBusy (0.449) — false positive
- **Behavior:** Contains multiple sub-functions (ret at 0x20011b8a, new prologue at 0x20011b9c):
  - **Sub 1 (0x20011ae8-0x20011b9a):** Byte-by-byte comparison loop between two buffers (s3, s2). Decrements counter in s4. On mismatch writes `2` as status. On counter exhaustion calls 0x20010f64. This is a **bonding key/nonce comparison** function.
  - **Sub 2 (0x20011b9c+):** Flash operations — reads from 0x89000 (512 bytes), processes data in a complex byte-level loop with multiple state variables, writes results to callback pointers. Manages flash-backed bonding records.
- **Proposed name:** `ble_bond_flash_process` — BLE bonding data validation + flash record management (hub function)

### FUN_ram_20017a08 (264 bytes) — `ble_ll_pdu_fragment`

- **Callers:** 0 (orphan — continuation block, reached via jump from larger function)
- **Callees:** 1 (flash_util @ 0x2001a448), also calls 0x20012750, 0x200125bc, 0x20012330
- **BSim:** flash_change_app_lock_block (0.313) — weak match
- **Behavior:** Many backward jumps to addresses 0x20016xxx-0x20017xxx (outside this function), confirming it's a **tail fragment** of a much larger BLE link layer function. Works with data structures using large offsets (s1+1946, s3-1856). Operations include: calling BLE send (args 9,0,0), checking connection state, managing PDU length fields (loads byte, adds 5, stores to offset+7), address tracking with 2-byte packing.
- **Proposed name:** `ble_ll_pdu_fragment` — tail section of BLE link layer PDU processing, not a standalone function

## VIA Command Handler Switch Table

**Function:** `via_command_handler` @ 0x2000c12c (1074 bytes)
**Switch table:** `switchdataD_ram_2001a91c` — 0xD4 entries (212 cases)

The handler receives an encoded parameter, subtracts 0x5C00, and dispatches through a jump table. Cases map to VIA protocol commands:

| Internal Case | VIA Function | Flash Address | Description |
|---------------|-------------|---------------|-------------|
| 0x21 | `via_get_protocol_version()` | — | Returns version 4 (VIA V3) |
| 0x22 | `via_protocol_reset()` | — | Sets gp+0x301 flag, ORs 0x20 |
| 0x23 | `via_get_layer_count()` | — | Returns 9 (internal layer count) |
| 0x24 | `via_device_reset()` | — | Clears report buffers |
| 0x25 | `ble_stack_init(0)` | — | BLE reset |
| 0x26-0x27 | Register state ops | — | Direct gp+0x301 manipulation |
| 0x2b | GET_BUFFER (flash read) | `0x84F00 + offset` | Keymap/config read (256B chunks) |
| 0x38 | SET_BUFFER (layered write) | Layer routing below | Keymap write with bounds check |
| 0x30 | Keymap pair write | — | Dispatches to layer-specific writers |
| 0x31 | Macro response | — | `via_macro_response_build()` |
| 0x32 | Layer keymap write | — | `via_keymap_pair_write_layer1/3()` |
| 0x2a | Buffer copy + flash | `0x85000` | Data accumulation + periodic flush |
| 0x2e | Macro flash write | `0x89000` | 512B macro buffer write |
| 0x35-0x37 | Flash read variants | `0x840E0+` | Address calculation + 2-byte reads |
| 0x3c | Data copy + flash | `0x84000` | Copy loop + periodic flush |
| 0x19 | Endpoint ready wait | — | USB EP poll loop (max 501 iterations) |

**Dispatch mechanism:** `param_1 - 0x5C00` (masked to 16-bit), bounds-checked against 0xD4 (212). Each entry is a 4-byte absolute jump target.

**4 table zones:**

| Zone | Cases | Target Region | Purpose |
|------|-------|---------------|---------|
| VIA Core | 0x00-0x3E | `0x2000C12C`-`0x2000D300` | VIA protocol, flash R/W, keymap, macros |
| ROM Functions | 0x3F-0x55 | `0x0000xxxx` (ROM) | BLE stack ROM utilities (crypto, pairing) |
| Wireless Handlers | 0x56-0x61 | `0x2000F9A0`-`0x2000FDA4` | 12 unique wireless-mode command handlers |
| BLE Handlers | 0x62-0xD3 | `0x20011F54`-`0x20011FDC` | 13 active + 101 default/fallthrough |

**Table statistics:** 212 entries, 86 unique jump targets, ~123 default/fallthrough, ~9 `halt_baddata` traps.

**SET_BUFFER (case 0x38) layer routing — confirmed flash layout:**

| Offset Range | Flash Address | Content |
|-------------|---------------|---------|
| `0x000-0x0FF` | `0x84000` | Layer 0 keymap (gp-0x600 buffer) |
| `0x100-0x1FF` | `0x85000` | Layer 1 keymap (gp-0x500 buffer) |
| `0x200-0x2FF` | `0x86000` | Layer 2 keymap (gp-0x400 buffer) |
| `0x300-0x3FF` | `0x89000` | Macro buffer (gp-0x300, via `via_macro_buffer_append`) |
| `>= 0x400` | — | **Rejected** (silently dropped) |

Each layer accumulates 256-byte chunks in SRAM, flushed to flash via `key_event_dispatch()` + `flash_write_chunked()` when the buffer exceeds 0xE00 bytes.

After dispatch, the handler sends response via `usb_endpoint_send(4, gp+0x380, 0x20)` (USB mode) or sets `gp+0x2DA = 2` (BLE mode).

**Internal layer count = 9** (via_get_layer_count returns 9), but SET_BUFFER only accepts offsets < 0x400 (4 layers). The extra 5 layers may be firmware-internal (mode-specific overlays for WIN/MAC/wireless).

**Note:** `DAT_ram_2001b0c4` (initially thought to be VIA-related) is actually a **BLE timing/interval lookup table** used by `ble_init_or_reconnect` @ 0x200127E8. Contains a non-linear curve with increasing step sizes (1,1,...,2,2,...,3,3,...) — characteristic of BLE connection interval mapping. Ghidra misinterprets the Andes V5 custom instruction at 0x20012824 as switch-case indexing.

## Gap Function Analysis

7 gap regions where Ghidra didn't create function boundaries. Disassembled with Andes objdump.

| Address Range | Size | Call Sites | Analysis |
|--------------|------|------------|----------|
| `0x2000A4E0`-`0x2000B548` | ~4.2KB | 9 | **RGB effect computation blocks** — continuation of `led_assign_from_matrix` (0x20009534). Loads RGB triplets from per-key buffers (s4, s3, s2), stores to computed LED addresses with bounds checking. Byte clamping (0-191, 0-127 ranges). Multiple backward jumps to 0x20009xxx (parent function). Not standalone functions. |
| `0x2000C37C`-`0x2000C48C` | ~276B | 4 | VIA command handler case blocks (continuation of `via_command_handler`) |
| `0x2000CDF8`-`0x2000D1EC` | ~1KB | 8 | VIA keymap/macro write handlers (flash accumulation + flush logic) |
| `0x2000F8DA`-`0x20010398` | ~2.8KB | 20 | **Peripheral/driver initialization** — stores function pointers to callback tables (addresses like 0x200170a0, 0x200190bc), configures GPIO registers (0x20014068), sets clock values (0x30D400 ≈ 3.2MHz), repeated calls to ROM init routine. Sets up interrupt handlers and peripheral drivers at boot. |
| `0x20015888`-`0x200158D4` | ~80B | 3 | BLE sub-functions (small utility blocks) |
| `0x20017D08`-`0x20017FC4` | ~704B | 3 | BLE/RF state machine blocks |
| `0x200185B0`-`0x20018A48` | ~1.2KB | 3 | Late BLE initialization blocks |

Most gaps are **not standalone functions** but continuation blocks of larger functions that Ghidra failed to follow through Andes custom instructions (EXEC.IT, BFOZ). Creating Ghidra functions at these addresses would force decompilation but the output will be partial (missing the parent function context).

## 9 Internal Layers — Mode-Specific Overlay System

`via_get_layer_count()` @ 0x2000c2fc returns **9**, but VIA's SET_BUFFER only accepts offsets < 0x400 (4 layers). Analysis of `via_layer_mode_set()` @ 0x20011dc4 (412B) reveals the full layer architecture:

### Layer Map

| Layers | Storage | Access | Purpose |
|--------|---------|--------|---------|
| 0-3 | Flash (0x84000-0x87000) | VIA read/write | Standard user-editable layers |
| 4-8 | Runtime only (SRAM) | Firmware internal | Mode-specific overlays |

### SRAM Variables

| GP Offset | Size | Purpose |
|-----------|------|---------|
| `gp+0x174` | 2 bytes (short) | Active layer index (masked to 0xF) |
| `gp+0x177` | 1 byte | Pending layer for mode-specific switch |
| `gp+0x171` | 1 byte | Mode change pending flag (set to 1) |
| `gp+0x22a` | 1 byte | Connection mode: `0x00`=wireless, nonzero=USB/BLE |
| `gp+0x153` | 1 byte | Connection type: `0`=wireless, `1`=USB, `2`=BLE |

### Layer Selection Logic (via_layer_mode_set)

FC00-FF00 range keycodes trigger layer changes via a multi-path dispatch:

1. **Keycodes 0x5010-0x5017 and 0x5300-0x5307**: Mode-specific layer select
   - If `gp+0x22a == 0` (wireless mode): **silently ignored** — returns without action
   - Otherwise: `gp+0x177 = keycode & 0xF` — sets pending layer
2. **Keycodes 0x5400-0x5407**: Extended mode layer with state change
   - Same wireless guard
   - Sets `gp+0x177 = keycode & 0xFF` and `gp+0x171 = 1` (pending flag)
3. **Keycodes 0x412C-0x432B**: Standard layer switching
   - Wireless mode: clears status flags (0x300, 0x304), checks 0x308 for reconnection
   - USB/BLE mode: sets `gp+0x300 |= 0x20`
   - Computes layer: `(keycode - 0x412C) >> 8 + 1`, clamped to 0-3
4. **Keycodes 0x5F10-0x5F11**: Simple increment — `gp+0x174 = keycode + 1`

### LED Effect Layer Handling

`led_effect_apply()` @ 0x200090f4 handles RGB differently per layer:
```
layer = *(short *)(gp + 0x174) & 0xF;
if (layer < 4) {
    keymap_ptr = lookup_table[layer];     // DAT_ram_2001b158 + layer*4
} else {
    keymap_ptr = gp - 0x500;              // Fallback buffer for layers 4-8
}
```

Layers 4-8 share a single fallback RGB buffer — they don't have per-layer LED configurations.

### Conclusion

The 5 extra layers are **WIN/MAC/wireless mode overlays**:
- Not stored in flash — generated at runtime per connection mode
- Disabled in wireless mode (firmware silently ignores layer 4+ selection)
- Share a single RGB fallback buffer
- Likely remap modifier keys (WIN↔CMD), media keys, or Fn combinations per mode
- The `mode_dispatch_loop()` @ 0x200119f0 routes execution to different main loops per connection type (`gp+0x153`: 0=wireless_main_loop, 1=main_loop_body/USB, 2=init_bss_section/BLE)

## RGB LED Pin Investigation

### PC2 — Not the LED Data Pin

Initial analysis suggested PC2 (bit 2 of `REG_GPIO_PC_OUT` = 0x80140313) as the LED data pin. **This is incorrect.** PC2 is a control/enable signal:

- **`hid_report_send()` @ 0x2000c634**: Sets PC2 HIGH before sending USB HID reports, clears LOW after + 300µs delay. This is a **USB activity signal**, not LED data.
- **`secondary_pipeline()` @ 0x2000efc8**: Configures PC2 as GPIO output (0x80140316 |= 4, 0x80140312 &= ~4), drives LOW, then configures SPI registers (0x80140040-0x80140047). This is **SPI bus initialization** for wireless communication.
- **GPIO init code**: Sets PC2 alongside matrix column pins — PC2 is part of the I/O control, not dedicated LED output.

### pwm_gpio_test_pulse — RF Calibration, Not LED

`pwm_gpio_test_pulse()` @ 0x20013afc writes to 0x8014031E = **`REG_GPIO_PD_GPIO`** (PD function select), NOT PC2. This function:
- Is called exclusively from `rf_frequency_calibration()` @ 0x20013b64
- Configures PWM registers to generate a precise test pulse
- Uses PD0 (0xFE = all pins except bit 0 as GPIO) for RF calibration timing
- Has nothing to do with RGB LEDs

### LED Color Buffer Pipeline

The LED color buffer flows through:
1. `led_effect_apply()` @ 0x200090f4 — applies RGB effect mode, fills per-key color values
2. `led_assign_from_matrix()` @ 0x20009534 — maps key presses to LED indices (112 max, buffer at `gp+0x1314`)
3. Gap region 0x2000A4E0-0x2000B548 (4.2KB) — RGB triplet processing with bounds clamping (0-191 brightness, 0-127 color)
4. `hid_report_build()` @ 0x2001441c — DMA transfer of 498 bytes from `gp+0x14d8` to **PSPI MOSI (PB7)** via DMA channel 4 at ~6 MHz SPI clock (WS2812 encoding: 8 SPI bits per LED bit, 1.33 µs/bit)

### Confirmed: PSPI MOSI on PB7 via DMA Channel 4

The RGB LED data output pin is **PB7**, configured as **PSPI MOSI (IO0)** with DMA channel 4.

**Firmware evidence** — `secondary_pipeline()` @ 0x2000efc8, lines 6088-6089 of `decompiled_all.c`:
```c
REG_GPIO_PB_FUC_H = REG_GPIO_PB_FUC_H & 0x3f | 0x40;  // 0x80140333 bits[7:6] = 01 → PSPI func
REG_GPIO_PB_GPIO = REG_GPIO_PB_GPIO & 0x7f;             // 0x8014030e bit 7 cleared → disable GPIO
```

**Decoding the func_mux write:**
- `reg_gpio_pb_fuc_h` (0x80140333) covers PB4-PB7: bits [1:0]=PB4, [3:2]=PB5, [5:4]=PB6, [7:6]=PB7
- `& 0x3f` clears bits 7:6 (PB7 field), `| 0x40` sets them to `01` = function value 1
- SDK enum: `PSPI_MOSI_IO0_PB7 = GPIO_PB7` with func_mux value 1 (from `pspi_set_pin_mux()` in SDK `spi.c`)
- Clearing bit 7 of `reg_gpio_pb_gpio` = `gpio_function_dis(PB7)` — switches PB7 from GPIO to peripheral mode

**PB7 is free from the matrix** — matrix columns use PB1-PB6 (`PB_OUT |= 0x7e`), not PB7.

**SPI register configuration** — `secondary_pipeline()` lines 6058-6082:

| Register | Address | Value | Purpose |
|----------|---------|-------|---------|
| `reg_spi_ctrl` | 0x80140047 | `\|= 0x10` | SPI module enable |
| `reg_spi_mode1` | 0x80140041 | `= 1` | Clock divider = 1 |
| `reg_spi_mode0` | 0x80140040 | mode-dependent | SPI mode (4 wireless modes) |
| `reg_spi_mode2+3` | 0x80140045 | lower nibble from `gp+0x128` | Transfer config |
| `reg_spi_mode2` | 0x80140042 | bit 2 from `gp+0x129` | GPIO setup flag |

**SPI clock:** PSPI runs on PCLK (APB bus). Formula: `spi_clock = PCLK / ((div + 1) * 2)`.
With PCLK=24 MHz and div=1: **6 MHz**. Using 8 SPI bits per WS2812 bit → 1.33 µs/bit (within WS2812 spec of 1.25 µs ±600ns).

**DMA configuration** — `secondary_pipeline()` lines 6083-6085:
```c
*(byte *)(gp + 0x365) = 4;                                            // DMA channel = 4
_DAT_ram_80100494 = DAT_ram_2001cb88 << 4 | _DAT_ram_80100494 & 0xf;  // ch4 req = PSPI TX
_DAT_ram_801004a8 = DAT_ram_2001cb84 << 4 | _DAT_ram_801004a8 & 0xf;  // ch5 req = PSPI RX
```
- DMA registers at base 0x80100444 with 0x14 (20-byte) stride per channel
- Channel 4 control: 0x80100494 = 0x80100444 + 4×0x14
- Upper nibble = `dma_req_sel_e`: `DMA_REQ_SPI_APB_TX = 4` (PSPI TX)

**DMA transfer** — `hid_report_build()` @ 0x2001441c, lines 8991-8996:
```c
iVar2 = (uint)*(byte *)(gp + 0x365) * 0x14;                          // ch4 offset = 0x50
*(uint *)(iVar2 + 0x100448U | 0x80000000) = uVar1;                    // src = gp+0x14d8 (RGB buf)
*(undefined4 *)(iVar2 + 0x10044cU | 0x80000000) = 0x80140048;         // dst = PSPI data FIFO
*(undefined4 *)(iVar2 + 0x100450U | 0x80000000) = 0x1f2;              // len = 498 bytes
*pbVar3 = *pbVar3 | 1;                                                // enable DMA channel
```
- Source: `gp+0x14d8` — RGB LED buffer (112 LEDs × ~4.5 bytes WS2812 encoding = 498 bytes)
- Destination: 0x80140048 — PSPI data FIFO
- Transfer size: 0x1F2 = 498 bytes

### All SPI MOSI Candidates (SDK Reference)

| SPI Module | Pin | func_mux Register | Bits | Value | Available? |
|-----------|-----|-------------------|------|-------|------------|
| **PSPI** | **PB7** | **0x80140333** | **[7:6]** | **1** | **YES — confirmed** |
| PSPI | PC7 | 0x80140335 | [7:6] | 0 | No — encoder/switch input |
| PSPI | PD3 | 0x80140336 | [7:6] | 1 | No — matrix row 2 |
| HSPI | PB3 | 0x80140332 | [7:6] | 0 | No — matrix col 11 |
| HSPI | PA4 | 0x80140331 | [1:0] | 2 | No — matrix col 8 |
