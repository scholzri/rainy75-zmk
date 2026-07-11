# Wobkey Rainy 75 Pro ISO DE — Reverse Engineering

## Project Goal

Fully reverse engineer the firmware and hardware of the Wobkey Rainy 75 Pro ISO DE keyboard (Telink TLSR9511, RISC-V, Andes D25F core).

## Key Facts

- **MCU:** Telink TLSR9511 (B91), QFN56, 1MB flash, 256KB SRAM
- **Architecture:** RISC-V RV32IMACF + Andes V5 extensions (XAndesPerf + XAndesCoDense)
- **USB:** VID `0x320F`, PID `0x5055`, 4 HID interfaces
- **Layout:** 75% ISO, 83 keys, Kailh Cocoa Linear switches (Pro), Cherry profile double-shot PBT keycaps
- **Matrix:** 8x16, column-to-row scan, ~500 Hz, 16 col pins + 7 row pins
- **Connectivity:** USB-C (1000Hz/2ms) / BLE 5.0 (500Hz/~8ms) / 2.4GHz (1000Hz/3ms), NKRO all modes
- **Battery:** 7000mAh (2×3500mAh LiPo), ~900h RGB off / ~80h RGB on, charge ~3-4h
- **Firmware:** Evision Semiconductor proprietary platform (120KB with boot header, 115KB body), NOT QMK-based, developer "YKQ"
- **Platform:** Shared across 7+ keyboards (CIDOO, IQUNIX, Ajazz, EPOMAKER) — all VID `0x320F` PID `0x5055`
- **VIA:** V3 (protocol 11), 4 layers, 16 macros, 512B macro buffer
- **RGB:** 83 WS2812 per-key LEDs (no separate underglow) via PSPI MOSI on PB7, DMA ch4, ~6 MHz SPI clock. ZMK firmware drives them with the custom out-of-tree **rainy_rgb** engine (replaces ZMK underglow; 12 effects + spatial/reactive + functional indicators) — see [docs/rainy-rgb.md](docs/rainy-rgb.md)
- **Ghidra:** 211 functions, 211 named (100%), project uses `RISCV:LE:32:AndeStar_v5`

## Documentation

All technical findings are in `docs/`:
- [docs/zmk-firmware.md](docs/zmk-firmware.md) — ZMK firmware build, BLE HCI driver, board definition, workspace layout
- [docs/rainy-rgb.md](docs/rainy-rgb.md) — rainy_rgb out-of-tree lighting engine: 12 effects, XY calibration, functional indicators (CapsLock/Fn-highlight/battery), controls, build/flash
- [docs/architecture.md](docs/architecture.md) — MCU, USB, HID interfaces, RGB, battery, connection modes
- [docs/gpio-matrix.md](docs/gpio-matrix.md) — GPIO pins, matrix scan, timing, keymap, Fn combos
- [docs/firmware-analysis.md](docs/firmware-analysis.md) — Ghidra, 211 functions, key pipeline, SRAM buffers, decompilation
- [docs/protocols.md](docs/protocols.md) — OTA protocol, VIA protocol, HID probe results
- [docs/hardware-probing.md](docs/hardware-probing.md) — test pads, SWS, logic analyzer, Burning EVK
- [docs/resources.md](docs/resources.md) — downloads, SDK links, reference projects
- [docs/evision-platform.md](docs/evision-platform.md) — Evision firmware platform, sibling keyboards, GearHub protocol
- [docs/wob-driver-analysis.md](docs/wob-driver-analysis.md) — wobwxe.com WOB driver JS analysis, 3 HID protocols, flash address map

## Local Files

```
zmk/                             # Zephyr module — our custom firmware code
  west.yml                       # west manifest (fetches ZMK + hal_telink + mcuboot)
  boards/rainy75/                # HWMv2 board definition (DTS, keymap, defconfig)
  drivers/bluetooth/             # BLE HCI driver (b91_bt.c shim) + deep sleep PM hooks
  drivers/usb/                   # USB DC driver (legacy usb_dc.h API, linked and enabled)
  drivers/led_strip/             # WS2812 LED strip driver (PSPI + DMA, b91_pspi.h registers)
  drivers/sensor/                # Battery ADC driver (SAR ADC sensor + channel scanner)
  drivers/watchdog/              # B91 hardware WDT driver (register-direct, Zephyr wdt API)
  src/mcuboot_confirm.c          # MCUboot image confirmation + WDT safety net
  src/boot_diag.c + .h           # Boot diagnostic: .noinit SRAM buffer + PD7 GPIO heartbeat + PA7 SWS restore
  src/poweroff.c                 # Deep sleep: z_sys_poweroff() — deep retention 64K with GPIO wakeup
  src/flash_mgmt.c               # Custom mcumgr group 64: raw flash erase/write/read/commit + RAM trampoline
  src/rainy_rgb/                 # rainy_rgb lighting engine (color/effects/engine/reactive/overlay/led_map/state/zmk_adapter) — see docs/rainy-rgb.md
  src/behaviors/behavior_rainy_rgb.c  # &rgb keymap behavior (toggle/effect/hue/bright/speed/battery)
  dts/bindings/                  # DTS bindings: b91-usbd / b91-spi-led-strip / b91-battery-adc / b91-watchdog
  lib/liblt_9518_zephyr.a        # BLE controller blob (2.8 MB) — proprietary/NDA, fetched by fetch_ble_blob.sh, gitignored (NOT committed)
conf/                            # build configuration overlays
  app.conf                       # ZMK app config (BLE, USB, mcumgr, WDT, RGB)
  ota-bridge.conf                # OTA bridge config (monolithic, USB+mcumgr+flash_mgmt)
  mcuboot.conf                   # MCUboot bootloader config
  mcuboot.overlay                # MCUboot DTS overlay (disables peripherals)
zmk-src/                         # ZMK upstream (fetched by west)
zephyr/                          # Zephyr upstream (fetched by west)
modules/hal/hal_telink/          # Telink HAL (fetched by west, patched for BT_HCI_B91)
bootloader/mcuboot/              # MCUboot v2.2.0 (fetched by west)
install_zmk.sh                   # stock → ZMK one-command installer (OTA bridge + flash_mgmt)
restore_stock.sh                 # ZMK → stock one-command restorer (flash_mgmt + reset)
build/zephyr/zmk.elf             # app build output (~304 KB ROM, all stages)
build-mcuboot/zephyr/zephyr.elf  # MCUboot build output (~51 KB ROM, 64KB boot partition)
build-bridge/zephyr/zmk.bin      # OTA bridge output (~83 KB, monolithic)
reverse/
  firmware/                      # firmware_extracted.bin, firmware_ota.bin, HID descriptors, keymap dump
  scripts/                       # ghidra_reexport.py, apply_names.py, common.py, call_graph.py, constant_fingerprint.py, map_gp_data.py, import_registers.py
    obsolete/                    # archived one-time/superseded scripts (20 scripts)
  tools/                         # ota_flasher.py, prepare_ota.py, restore_original.py, via_probe.py, via_dump_keymap.py, probe_commands.py, mem_reader.py, wob_probe.py
    sws_flash.sh                 # Stage 0-1+ helper: dump/analyze/roundtrip/flash/restore via BDT+EVK
    bdt/                         # Telink BDT v2.2.1 (Linux x64), EVK firmware v4.7, udev rules, docs
  ghidra/                        # rainy75_andesv5 project + scripts (ApplyAllNames.java, BSim, export)
  analysis/                      # ghidra_export_functions.txt, bsim_matches.txt, decompiled_all.c, decompiled_stubborn.c, etc.
  photos/                        # PCB teardown (motherboard/ + daughterboard/)
  reference/                     # Official firmware exe, manual, VIA JSON, TLSR9511B datasheet (PDF + markdown transcript)
  bsim_work/                     # BSim DB, signatures, bsim_reference.elf
  sdk/                           # Telink B91 BLE SDK clone
  toolchain/                     # Andes GCC 14.2.0 (riscv32-elf-gcc -mcpu=d25f)
  resource_param_128K.bin        # OTA config (VID/PID/key)
  .venv/                         # Python venv (capstone, hidapi, pefile, dnfile)
```

> **Public-repo note:** entries derived from the proprietary firmware are **gitignored /
> local-only** and are NOT in the published repository: `reverse/ghidra/`,
> `reverse/analysis/`, `reverse/scripts/` (the Ghidra pipeline), `reverse/reference/`,
> `reverse/firmware/*.bin`, `reverse/sdk/`, the BLE blob (`zmk/lib/*.a`, fetched at build
> time), and the development-process artifacts `docs/plans/` + `docs/superpowers/`. The
> published repo carries the firmware code, the standalone `reverse/tools/`, and the
> polished `docs/`.

## TODO

### Done

**Hardware & GPIO:**
- USB enumeration and chip identification (TLSR9511 confirmed)
- GPIO pin mapping (16 col + 7 row) and matrix scan validation via logic analyzer (500 Hz)
- SWS test pad identified (3 pads near MCU: GND, VCC, SWS)
- RGB LED data pin confirmed: PB7 = PSPI MOSI IO0, DMA ch4, WS2812 8-bit encoding
- Solved: 0x0087 = KC_INT1 ghost key, encoder_input_read = consumer_report_send

**Firmware Analysis (Ghidra):**
- 211/211 functions named (100%) via register-constant ID, call graph, BSim, manual analysis
- All 211 functions decompile (0 failures) — output: `decompiled_all.c` (296KB, 11542 lines)
  - Fix for Andes V5 computed jumps: JumpTable overrides + FlowOverride.RETURN + FlowOverride.CALL_RETURN (integrated into `ghidra_reexport.py`)
- 495 register labels + 876 equates imported via PyGhidra
- Post-processing pipeline (Steps 6-9 in `ghidra_reexport.py`): register labels (558 defs), ROM function names (38 ROM functions), SRAM variables (14 high-confidence), cross-reference comments
- Key pipeline traced: main_loop_body → key_pipeline_main → matrix_scan → hid_report_send
- VIA command handler: 212-case switch table, flash layout 0x84000–0x89000
- 9 internal layers: 0-3 VIA-exposed (flash-backed), 4-8 runtime mode overlays
- All 7 gap regions analyzed (RGB effect blocks, peripheral init, BLE continuations)

**Protocols & Probing:**
- OTA protocol reverse-engineered (Linux flasher built and tested): `ota_flasher.py` + `prepare_ota.py`, 0xFF padding fix, 16-byte alignment, two's complement END command
- EVK-free installation path verified: stock → OTA bridge (24s) → mcumgr DFU full app (82s), no hardware debugger required
- HID command probe — all interfaces, no hidden debug commands
- Extended VIA probe: hidden layers, CUSTOM_GET_VALUE, RGB read-only, full cmd scan
- Manual incorporated (key combos, connection modes, battery, latency, MAC mode)

**Evision Platform:**
- Evision Semiconductor identified as firmware provider — NDA-protected, no public source
- 7+ sibling keyboards confirmed (CIDOO ABM066, IQUNIX MQ80/MG65, Ajazz AKS068, EPOMAKER EK21)
- GearHub (qmk.top) analyzed: proprietary protocol branch (VID `0x3151`) vs VIA V3 branch

**WOB Driver (wobwxe.com):**
- Complete JS analysis: 10 devices, two VIDs (`0x320F`, `0x36B0`), 3 distinct HID protocols
- Protocol A (address-based, RT hall-effect), Protocol B (command-ID, Rainy 98), Protocol C (packet-based, our keyboard)
- Our keyboard uses Interface 3: usagePage `0xFF1C`, usage `0x92`, reportId 4 (NOT VIA's `0xFF60`)
- All three WOB protocols tested on standard Rainy 75 — **no response** (firmware has no handler)
- Flash address map, DFU protocol, 12 magnetic switch profiles, config encryption key documented
- Tools built: `mem_reader.py` (RT variant), `wob_probe.py` (Protocol C probe)

**ZMK Firmware:**
- ZMK build infrastructure: west workspace, Zephyr module, CMake/Kconfig wiring
- HWMv2 board definition: `rainy75` targeting `tlsr9518` SoC, DTS with GPIO matrix (16×6, col2row), 2-layer ISO DE keymap
- BLE HCI driver: own shim (`b91_bt.c`) around Telink BLE blob (`liblt_9518_zephyr.a` from `telink-semi/zephyr_hal_telink_b91_ble_lib`), Zephyr v4.1 DEVICE_API, peripheral-only (0 masters, 1 slave)
- Blob integration: `BT_HCI_B91` config namespace (avoids hal_telink's broken `BT_B91` code path), hal_telink `CMakeLists.txt` patched for `sys_init` conflict, `swapN`/`swapX` utilities provided, weak stubs for unused symbols
- USB DC driver: legacy `usb_dc.h` API implementation (`usb_dc_b91.c`), replaces old UDC-API driver (backed up to `future/udc_b91.c`). All 21 `usb_dc_*` functions implemented: lifecycle, EP management, data transfer, ISR. Uses `irq_connect_dynamic()` for USB PLIC IRQs (avoids PLIC source 11 collision with machine external interrupt 11).
- USB HID enabled: `CONFIG_USB_DC_B91=y` + `CONFIG_ZMK_USB=y` in defconfig, Zephyr USB device stack linked and working
- USB CDC ACM console: `telink,b91-usbd` DTS binding + `usbd@80100800` node with `cdc_acm_uart` child, `zephyr,console` chosen, `CONFIG_USB_CDC_ACM=y` + `CONFIG_LOG_BACKEND_UART=y` — Zephyr logs visible over USB serial
- RGB LED strip driver: `led_strip_b91_spi.c` — WS2812 via PSPI + DMA ch4, PB7 MOSI, 6 MHz SPI clock, 83 per-key LEDs, GRB encoding, Zephyr `led_strip` API, ZMK underglow enabled (`CONFIG_ZMK_RGB_UNDERGLOW=y`). Audit fixes: DMA register offsets corrected (SRC=+0x04, DST=+0x08, SIZE=+0x0C per SDK dma_reg.h), SIZE register now in 32-bit words, added SPI TX_CNT programming, buffer 4-byte aligned
- Battery ADC driver: `battery_b91_adc.c` — SAR ADC sensor (4 MHz, 14-bit, Vref 1.2V, prescale 1/4) + boot-time channel scanner, Zephyr `sensor` API with GAUGE_VOLTAGE + GAUGE_STATE_OF_CHARGE channels (disabled pending pin identification via scan mode)
- Deep sleep: `zmk/src/poweroff.c` — `z_sys_poweroff()` implementation: turns off RGB (PC2 LOW), disables USB DP pull-up, configures analog pull-downs on columns (100K) and pull-ups on rows (1M), configures row GPIO wakeup via `pm_set_gpio_wakeup()`, enters `DEEPSLEEP_MODE` (0x30, cold boot). Retention mode (0x03) doesn't work with MCUboot — boot ROM reloads MCUboot into ILM on any reset, overwriting retained app code. Zephyr patches: 0004 (HAS_POWEROFF Kconfig) + 0005 (start.S retention wakeup skip, currently dead code). `CONFIG_ZMK_SLEEP=y` with 15min idle timeout. Wakeup on any keypress → MCU cold-boots through MCUboot (~1-2s).
- MCUboot DFU: mcumgr UART transport over CDC ACM, swap-using-move, unsigned images, WDT crash revert
- MCUboot bootloader: v2.2.0 (June 2025), builds to ~51KB (fits 64KB boot partition), swap-using-move, no signing, serial recovery via NO_APPLICATION, `BOOT_MAX_IMG_SECTORS` auto-calculated
- B91 watchdog driver: `wdt_b91.c` — Zephyr `wdt` API, register-direct, PCLK-based, max ~11s timeout, full SoC reset
- MCUboot image confirmation: `mcuboot_confirm.c` — SYS_INIT at APPLICATION/90, 10s WDT safety net, 5s delayed confirmation
- Config split: `rainy75_defconfig` hardware-only (shared MCUboot+app), `conf/app.conf` app-specific (BLE, USB, ZMK, mcumgr, WDT)
- Successful compile: zmk.elf (~92 KB ROM Stage 1) + MCUboot (~51 KB ROM), SDK 0.17.0 required
- BLE controller thread stack bumped to 2048 (default 1024 was tight per hal_telink references)
- Flash partition layout verified: storage reduced from 76K→72K to avoid overwriting RF/ADC calibration at 0xFE000
- `.gitignore` added for build artifacts, fetched modules, toolchain, SDK
- Boot diagnostic: `boot_diag.c` — `.noinit` SRAM buffer (36 bytes, 28 stage entries) readable via SWS/BDT, PD7 GPIO heartbeat, PA7 SWS restore at POST_KERNEL (keeps SWS debug working while firmware runs), SYS_INIT at all 5 levels + 8 USB attach substages + MCUboot confirm + running marker, `CONFIG_BOOT_DIAG=y` in app.conf
- `build.sh` — MCUboot + app + combined + OTA + bridge build: `-p` pristine, `-m` MCUboot, `-c` combined, `-b` bridge, `-a` all
- USB serial logging tuned: `ZMK_LOGGING_MINIMAL=y` (INFO level, ZMK defaults DBG which floods buffer), `LOG_BUFFER_SIZE=4096`, `CDC_ACM_RINGBUF_SIZE=4096` (default 1024 blocks log thread), `LOG_PROCESS_THREAD_SLEEP_MS=100`. Full boot log visible with zero dropped messages.
- BLE HCI driver IRQ fix: `IRQ_CONNECT()` must use multi-level encoded IRQ numbers (`IRQ_TO_L2(source) | 11`), not raw PLIC source numbers. stimer(1)→0x020B, RF(15)→0x100B. Raw numbers install handlers at wrong `_sw_isr_table` indices → LL scheduling never fires → no advertising.
- BLE advertising: "Rainy 75 Pro" visible in scans, Just Works pairing, HID over GATT (6KRO + consumer), battery service
- BLE HID input: confirmed working over BLE, endpoint toggle via Fn+F4 (`&out OUT_TOG`)
- BLE 2M PHY: DISABLED — blob's 2M PHY causes LL Response Timeout (0x22) after 40s. 1M PHY stable. Only real blob bug found.
- BLE reconnection: stable with 1M PHY. Blob sends Encryption Change natively (injection workaround removed — was only needed due to 2M PHY instability).
- BLE controller thread: `blc_sdk_main_loop()` + `k_sleep(K_MSEC(2))` when !CONFIG_PM, stack 2048
- Boot diagnostic analog freeze: `boot_diag_freeze_analog()` called before BLE thread start — prevents analog bus (0x80140180-0x80140184) conflicts with blob's DEEP_ANA_REG usage (0x38-0x3F)
- RGB LED strip driver: `led_strip_b91_spi.c` — WS2812 via PSPI + DMA ch4 on PB7 MOSI, 6 MHz SPI clock, 83 per-key LEDs, GRB encoding, Zephyr `led_strip` API, ZMK underglow enabled (`CONFIG_ZMK_RGB_UNDERGLOW=y`). PC2 HIGH controls LED VCC via MOSFET. PB5 (PSPI CLK) is a matrix column — must NOT be reconfigured. GPIO bit-bang failed for LED 1 (first-bit timing jitter) — PSPI+DMA has zero jitter.
- RGB Fn-layer bindings: toggle (Fn+Backspace), effect cycle (Fn+Enter), hue (Fn+NUHS), brightness up/down (Fn+↑/↓), speed up/down (Fn+→/←)
- Flash management: `flash_mgmt.c` — custom mcumgr group 64 (erase/write/read/commit), RAM trampoline for in-place firmware replacement, ~10ms settling delay after bulk erase, diagnostic markers via analog reg 0x3E
- OTA bridge: monolithic ZMK build (`conf/ota-bridge.conf`), USB+mcumgr+flash_mgmt only (~83KB), for stock→ZMK transition via OTA protocol
- Installation scripts: `install_zmk.sh` (stock→ZMK, two-stage OTA+flash_mgmt) and `restore_stock.sh` (ZMK→stock, flash_mgmt+trampoline)
- `restore_original.py` — zero-dependency Python SMP serial client for flash_mgmt: CBOR codec, CRC16-ITU-T, SMP framing, staging+verify+commit workflow, dynamic staging offset
- Flash_mgmt trampoline: confirmed working from both bridge and ZMK builds. Settling delay (~10ms busy-wait after 93+ sector bulk erase) prevents write-phase crash. Flash write protection fix: `flash_unlock()` called in commit handler before trampoline — clears BP bits on 0x0-0x7FFFF so raw `flash_erase_sector_ram()` works.
- Full round-trip tested: stock→ZMK (`install_zmk.sh -y`) and ZMK→stock (`restore_stock.sh -y`) — two complete cycles verified, no hardware debugger needed.

**SPI Pin Tracing (Completed):**
- PSPI fully traced: PB7=MOSI (RGB data), PB5=CLK, PB6=MISO — configured in `secondary_pipeline()` at 0x2000efc8
- SPI clock = PCLK/4 = 6 MHz (divider=1), master mode, write-only for RGB
- DMA ch4 (TX) and ch5 (RX) configured; RGB uses ch4 only
- RGB transfer: 498 DMA words (1992 bytes = 83 LEDs × 24 encoded bytes) from gp+0x14d8 → PSPI FIFO at 0x80140048, triggered by `hid_report_build()` at 0x2001441c. DMA SIZE register uses 32-bit word units (SDK: `dma_set_size(chn, len, DMA_WORD_WIDTH)` = len/4)
- HSPI not used in this firmware
- Gap region (0x2000F8DA–0x20010398) is not a single function but multiple init blocks broken by Andes V5 computed jumps

**SRAM Variable Analysis (Deep Trace — 310 named, 75% coverage):**
- 241 GP-relative variables + 65 SRAM absolute addresses mapped across all firmware domains
- 1788 total named replacements in `decompiled_all.c` (SRAM:587, GP:897, REG:129, ROM:151, FLASH:24)
- Remaining unnamed: ~216 gp refs (mostly Tier 4 annotated), 7 `DAT_ram_` refs
- Clock/PM subsystem (gp+0x0158–0x0169): 3 SDK structs — `pm_early_wakeup_time_us_s`, `pm_r_delay_cycle_s`, `sys_clk_t`
- BLE Link Layer: 46 variables traced (`ll_state`, `ll_conn_substate`, `ll_event_counter`, `ll_ctrl_pending_flags`, etc.)
- USB subsystem: setup packet struct (gp+0x01DC–0x01E2), descriptor transfer, HID protocol mode
- Wireless/RF: connection state machine, report pointers, channel management, retry logic
- Key/HID: report buffers, dirty flags, macro engine FSM, layer control, consumer/system/media reports
- VIA protocol: command buffer (32B shared TX/RX), keymap dirty flags, protocol version
- Corrections: gp+0xF4 = `led_color_cfg` (not `debounce_state`), gp+0x0180 = `via_layer3_flash_page_ptr` (not `chip_id`), gp+0x018C = `ll_more_data_flag` (not `ble_handshake_phase`), gp+0x0338 = `report_special` (not `key_state_previous`)

**Flash Data Table Naming (ghidra_reexport.py Step 10):**
- 20 flash data tables identified and named in `build_flash_data_map()`: jump tables (6), LED/keymap data (4), USB descriptors (5), BLE data (1), DMA constants (4)
- Applied to `decompiled_all.c`: 19 replacements across 15 unique tables

**GP Variable Naming (ghidra_reexport.py Step 11):**
- ~95 GP-relative variables mapped across 5 SRAM clusters (Clock/PM, RGB, RF/BLE, VIA, HID) + negative offsets (.data section)
- Four-tier expression-aware replacement: deref→name, address→&name, array→comment, bare→annotation
- Handles both positive (gp+0xNNN) and negative (gp-0xNNN) offsets
- Applied to `decompiled_all.c`: 522 GP replacements across 96 unique offsets + 163 Tier-4 annotations
- New entries: `flash_staging_buf`(10), `via_layer[01]_page_buf`(14), `ble_bond_*`(12), `macro_data_buf`(3), `ll_ctrl_pdu_buf`(7), `layer_keymaps_l[1-3]`(9), `keymap_default_*`(2)
- `map_gp_data.py` KNOWN_BUFFERS: 160 entries (was ~150), negative GP offsets for VIA/BLE/keymap

**Report Dirty Flags (gp+0x0300/0x0301 — Fully Traced):**
- `key_state_flags` (gp+0x300): 8-bit producer-consumer dirty flags, 55 references across 12 functions
  - Bits 0-4: SCAN_COMPLETE, KEY_STATE_DIRTY, NKRO_DIRTY, CONSUMER_DIRTY, MODIFIER_DIRTY
  - Bit 5: LED_UPDATE (layer change), Bit 6: ALL_CLEAR (bulk release), Bit 7: NKRO_EXTENDED_PENDING
  - ALL_CLEAR (bit 6) gates key registration functions and triggers buffer zeroing in all 3 main loops
- `report_mode_flags` (gp+0x301): 28 references across 14 functions
  - Bit 0: BLE_CONNECTED, Bit 1: LED_OVERRIDE (fixed 0xA2 RGB), Bit 2: FLASH_LOG_MODE
  - Bit 5: KEYMAP_RELOAD (VIA reset), Bit 6: SPECIAL_REPORT_DIRTY (mouse/consumer)
- Three parallel main loops dispatch on `gp+0x153`: mode 0 (BLE), mode 1 (dongle/2.4G), mode 2 (USB) — all use identical dirty-bit processing

**VIA Dispatcher Table (Fully Decoded):**
- Actual table at `switchdataD_ram_2001a91c` (NOT `DAT_ram_2001b0c4` which is a BLE timing curve)
- 212 entries, 86 unique jump targets, 4 zones: VIA core (0x00-0x3E), ROM functions (0x3F-0x55), wireless handlers (0x56-0x61), BLE handlers (0x62-0xD3)
- SET_BUFFER flash layout confirmed: layer 0→0x84000, layer 1→0x85000, layer 2→0x86000, macros→0x89000
- `DAT_ram_2001b0c4` is actually a BLE connection interval lookup (non-linear timing curve), misidentified due to Andes V5 custom instruction at 0x20012824

**Sibling OTA Diff (Blocked — No Local Files):**
- No sibling keyboard OTA packages found locally
- CDN endpoints identified: `drivers.sfo3.digitaloceanspaces.com` (international) and `driveall.oss-cn-hangzhou.aliyuncs.com` (China)
- Best candidates: CIDOO ABM066 (same VIA branch, VID 0x320F) and Ajazz AKS068 (appears in both VIA and GearHub branches)
- Need to download firmware .exe files from manufacturer support pages and extract with existing `ota_flasher.py`

### Next — Hardware Bring-Up (staged, see [docs/zmk-firmware.md](docs/zmk-firmware.md) "Hardware Bring-Up Checklist")
- **Stage 0 (COMPLETE)**: Full 1MB flash dumped (5 reads, all SHA-256: `32479b18`), analyzed, calibration+MAC extracted, round-trip write verified. OTA firmware matches flash byte-for-byte. Flash map: firmware 120KB at 0x0, VIA data 57KB at 0x82000, BLE bonding 12KB at 0xFA000, cal+MAC 8KB at 0xFE000 (850KB free). Calibration backup: `reverse/firmware/calibration_0xFE000.bin`. Flash has write protection (BP 0x0030, 0x00000-0x7FFFF) — BDT `-f` auto-unlocks.
- **Stage 1 (COMPLETE)**: MCUboot (51KB, 64KB partition) + USB app (~92KB) flashed via SWS. USB enumeration (VID 1d50:615e, CDC ACM + HID), key matrix, serial console, mcumgr DFU (echo + image list + upload), MCUboot WDT+confirm — all working.
  - **SWS debug**: PA7 SWS restore in boot_diag.c POST_KERNEL → `bdt B91 ac` works while firmware running (no rst needed)
  - **Serial recovery**: `CONFIG_BOOT_SERIAL_NO_APPLICATION=y` only (GPIO entrance abandoned — matrix diodes + WRITE_BIT bug)
  - **Build**: `./build.sh -a` (all: MCUboot + app + combined), `./build.sh -p` (app pristine). Flash: `sws_flash.sh flash build/combined.bin <<< "y"`
  - **mcumgr**: `~/go/bin/mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" image upload build/zephyr/zmk.signed.bin`
- **Stage 2 (COMPLETE)**: BLE advertising, pairing (Passkey Entry, Security Level 4), HID over GATT, BLE HID input, reconnection — all working. "Rainy 75 Pro" at XX:XX:XX:XX:XX:XX, Fn+F4 endpoint toggle (`&out OUT_TOG`). Blob bugs found: (1) IRQ_CONNECT multi-level encoding (stimer→0x020B, RF→0x100B), (2) 2M PHY causes LL Response Timeout 0x22 after 40s — disabled, 1M works fine, (3) BT_PRIVACY hangs bt_enable() — blob doesn't support LE_Set_Random_Address. First connection after boot fails with 0x3E (benign, retries succeed). SWS reads interfere with BLE — don't activate SWS while BLE running.
- **Stage 3 (COMPLETE)**: RGB underglow — 83 WS2812 per-key LEDs via PSPI+DMA, all effects working (solid, breathe, spectrum, swirl), Fn-layer RGB controls, PC2 HIGH LED power, NVS persistence. GPIO bit-bang failed for LED 1 (first-bit jitter) → PSPI+DMA fixed it. PB5 is matrix column 13, not PSPI CLK.
- **Stage 4**: ADC channel scan → battery sensor (via mcumgr DFU)
- **Stage 5 (COMPLETE)**: Deep sleep — `DEEPSLEEP_MODE` (0x30, cold boot on wakeup), GPIO keypress wakeup. `z_sys_poweroff()` in `zmk/src/poweroff.c`: RGB off, USB detach, analog pull-downs on columns (100K) + pull-ups on rows (1M), `pm_set_gpio_wakeup()` on all 6 row pins. Wakeup on any keypress → cold boot through MCUboot (~1-2s). Retention mode (0x03) incompatible with MCUboot (boot ROM overwrites retained ILM). Patches: 0004 (HAS_POWEROFF Kconfig) + 0005 (start.S retention skip, dead code). `CONFIG_ZMK_SLEEP=y`, 15min idle timeout.

### Next — Other
- Continue SRAM variable analysis (~216 gp refs remain as Tier-4 annotations, 7 `DAT_ram_` unnamed — 75% coverage)

## Notes

- The Rainy 75 Pro ISO DE uses a dedicated ISO firmware (dated 2025-05-30)
- The wireless on/off switch is under the CapsLock keycap
- Flashing wrong firmware can brick the keyboard — always dump originals first
- OTA protocol is write-only — SWS via Burning EVK is the only way to read flash
- For bigger research tasks, the user has an external deep-research LLM available — provide a prompt and the user will return the research results
- Ghidra headless Java scripts are broken in 12.0.1 (OSGi error) — use PyGhidra instead
- **ZMK build**: requires Zephyr SDK 0.17.0 (not 0.17.4), set `ZEPHYR_SDK_INSTALL_DIR=$(pwd)/toolchain/zephyr-sdk-0.17.0`, pass `-DEXTRA_CONF_FILE=$(pwd)/conf/app.conf` — see [docs/zmk-firmware.md](docs/zmk-firmware.md)
- **Environment**: Arch Linux distrobox, use `pacman` not `dnf`
