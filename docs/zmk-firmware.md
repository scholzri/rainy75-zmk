# ZMK Firmware — Implementation

Custom ZMK firmware for the Wobkey Rainy 75 Pro ISO DE keyboard, targeting the Telink TLSR9511B (B91) SoC. BLE + USB HID.

## Status

| Component | Status | Notes |
|-----------|--------|-------|
| Build infrastructure | **Done** | west workspace, Zephyr module, CMake/Kconfig |
| Board definition | **Done** | HWMv2 format, DTS, keymap, defconfig |
| BLE HCI driver | **Done** | Real blob linked, Zephyr v4.1 device-model API |
| USB DC driver | **Done** | Legacy `usb_dc.h` API, linked and enabled |
| RGB LED strip | **Done** | WS2812 via PSPI + DMA ch4, PB7 MOSI, 83 per-key LEDs, ZMK underglow enabled |
| Battery ADC sensor | **Done** | SAR ADC driver, PD1 channel 0x0A, 1/2 divider, BLE battery service |
| Deep sleep | **Done** | `sys_poweroff` → DEEPSLEEP_MODE (cold boot), 15min idle timeout |
| ZMK Studio | **Done** | Runtime keymap editing over BLE GATT (WebBluetooth) |
| MCUboot DFU | **Done** | mcumgr USB UART (primary) + BLE SMP (backup), swap-using-move, WDT crash revert |
| Watchdog | **Done** | B91 HW WDT driver, MCUboot image confirmation |

**Build output (all stages enabled):**

| Region | Used | Total | Usage |
|--------|------|-------|-------|
| ROM | 304 KB | 448 KB | 68% |
| RAM (DLM) | 71 KB | 128 KB | 56% |
| RAM (ILM) | 39 KB | 128 KB | 31% |

**MCUboot bootloader (separate build):**

| Region | Used | Total | Usage |
|--------|------|-------|-------|
| ROM | 49 KB | 64 KB | 77% |
| RAM (DLM) | 35 KB | 128 KB | 28% |
| RAM (ILM) | 5 KB | 128 KB | 4% |

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         ZMK Firmware                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────────┐   │
│  │  Keymap   │  │ BLE Host │  │ USB HID  │  │  RGB Underglow │   │
│  │ (2 layers)│  │ (Zephyr) │  │ (Zephyr) │  │   (ZMK)       │   │
│  └─────┬─────┘  └────┬─────┘  └────┬─────┘  └──────┬────────┘   │
│        │              │             │               │            │
│  ┌─────┴─────┐  ┌────┴─────┐  ┌────┴──────┐  ┌─────┴────────┐  │
│  │  kscan    │  │ hci_b91  │  │usb_dc_b91 │  │led_strip_b91 │  │
│  │  matrix   │  │ (our own)│  │(legacy API)│  │(PSPI+DMA ch4)│  │
│  └─────┬─────┘  └────┬─────┘  └────┬──────┘  └──────┬───────┘  │
│        │         ┌────┴─────┐       │               │           │
│        │         │ b91_bt.c │  ┌────┴──────┐  ┌─────┴───────┐  │
│        │         │  (shim)  │  │  CDC ACM  │  │  PSPI MOSI  │  │
│        │         │  + PM    │  │ (mcumgr)  │  │  PB7 → LEDs │  │
│  ┌─────┴─────┐  ┌────┴──────────┐  └───────┘  └─────────────┘  │
│  │ GPIO 16×6 │  │liblt_9518_zeph│  BLE blob                    │
│  │ col2row   │  │   (2.8 MB)    │                               │
└──┴───────────┴──┴───────────────┴───────────────────────────────┘
        Battery ADC (PD1)    Deep sleep (sys_poweroff)
```

The BLE controller blob (`liblt_9518_zephyr.a`) is a precompiled binary from Telink that implements the BLE link layer, HCI, and RF control. It is **proprietary** (confidential / non-transferable / NDA — see [NOTICE](../NOTICE)) and is **not committed** to this repo; `fetch_ble_blob.sh` downloads it (pinned commit + SHA-256 verified) from Telink's public repository at build time. Our shim (`b91_bt.c`) bridges it to Zephyr without including the SDK's conflicting headers. Deep sleep uses `z_sys_poweroff()` (cold boot via DEEPSLEEP_MODE 0x30) in `zmk/src/poweroff.c`.

## Workspace Layout

```
rainy75/                            # workspace root
├── zmk/                            # our Zephyr module (manifest repo)
│   ├── west.yml                    # manifest: fetches ZMK + hal_telink + mcuboot
│   ├── zephyr/module.yml           # registers as Zephyr module
│   ├── CMakeLists.txt              # top-level: includes drivers, links blob
│   ├── Kconfig                     # top-level: rsource driver Kconfigs
│   ├── lib/
│   │   └── liblt_9518_zephyr.a     # BLE controller blob (2.8 MB)
│   ├── src/
│   │   └── mcuboot_confirm.c       # MCUboot image confirmation + WDT safety net
│   ├── boards/rainy75/             # HWMv2 board definition
│   │   ├── board.yml
│   │   ├── Kconfig.rainy75
│   │   ├── Kconfig.defconfig
│   │   ├── rainy75_defconfig        # hardware-only (shared by MCUboot + app)
│   │   ├── rainy75.dts
│   │   ├── rainy75.keymap
│   │   ├── rainy75.zmk.yml
│   │   └── board.cmake
│   ├── dts/bindings/
│   │   ├── bluetooth/telink,b91-bt-hci.yaml
│   │   ├── usb/telink,b91-usbd.yaml
│   │   ├── led-strip/telink,b91-spi-led-strip.yaml
│   │   ├── sensor/telink,b91-battery-adc.yaml
│   │   └── watchdog/telink,b91-watchdog.yaml
│   └── drivers/
│       ├── bluetooth/              # BLE HCI driver + deep sleep PM
│       │   ├── b91_bt.h            # shim API header
│       │   ├── b91_bt.c            # shim: blob bridge + init + thread + PM hooks
│       │   ├── hci_b91.c           # Zephyr HCI device-model driver
│       │   ├── Kconfig
│       │   └── CMakeLists.txt
│       ├── usb/                    # USB DC driver (legacy usb_dc.h API)
│       │   ├── usb_dc_b91.c        # All 21 usb_dc_* functions + ISR
│       │   ├── udc_b91.h           # Register definitions
│       │   ├── Kconfig
│       │   └── CMakeLists.txt
│       ├── led_strip/              # WS2812 LED strip driver
│       │   ├── led_strip_b91_spi.c  # PSPI + DMA ch4, Zephyr led_strip API
│       │   ├── b91_pspi.h           # PSPI + DMA register definitions
│       │   ├── Kconfig
│       │   └── CMakeLists.txt
│       ├── sensor/                 # Battery ADC driver
│       │   ├── battery_b91_adc.c    # SAR ADC sensor + channel scanner
│       │   ├── Kconfig
│       │   └── CMakeLists.txt
│       └── watchdog/               # B91 hardware watchdog driver
│           ├── wdt_b91.c            # Zephyr wdt API, register-direct
│           ├── Kconfig
│           └── CMakeLists.txt
├── conf/                           # build configuration overlays
│   ├── app.conf                    # ZMK app config (BLE, USB, mcumgr, RGB, Studio, sleep)
│   ├── mcuboot.conf                # MCUboot bootloader config
│   ├── mcuboot.overlay             # MCUboot DTS overlay (disables peripherals, adds CDC ACM)
│   └── mcumgr.overlay              # App DTS overlay (CDC ACM for mcumgr SMP transport)
├── zmk-src/                        # ZMK upstream (fetched by west)
│   └── app/                        # ZMK application
├── zephyr/                         # Zephyr upstream (fetched by west)
├── bootloader/
│   └── mcuboot/                    # MCUboot v2.2.0 (fetched by west)
├── modules/
│   └── hal/hal_telink/             # Telink HAL (fetched by west)
├── build/                          # app build output
│   └── zephyr/zmk.elf
└── build-mcuboot/                  # MCUboot build output
    └── zephyr/zephyr.elf
```

## Build Instructions

### Prerequisites

- **Zephyr SDK 0.17.0** — must match Zephyr v4.1.0 (`zephyr/SDK_VERSION`)
  - SDK 0.17.4 causes `conflicting types for __lock___libc_recursive_mutex` in picolibc
  - Installed at: `toolchain/zephyr-sdk-0.17.0/` (project-local, in `.gitignore`)
  - To reinstall: download [minimal SDK](https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v0.17.0), extract to `toolchain/`, run `setup.sh -t riscv64-zephyr-elf`
- **Python venv** with `west` installed
- **CMake** 3.20+, **Ninja**

### First-time setup

```bash
cd path/to/rainy75-zmk
python -m venv .venv
source .venv/bin/activate
pip install west

# Initialize west workspace from our manifest
west init -l zmk
west update
```

### Build (recommended)

```bash
# App builds need a physical-layout flag — --iso or --ansi (no default).
./build.sh -a --iso    # MCUboot + app + combined image (first build or after west update)
./build.sh --iso       # app only (incremental)
./build.sh -p --iso    # app pristine (after Kconfig/DTS changes)
./build.sh -pa --ansi  # full pristine rebuild, ANSI layout
```

`build.sh` handles venv activation, SDK path, patch application, DTS overlays, and combined image creation.
The layout flag passes `-DDTS_EXTRA_CPPFLAGS=-DRAINY75_ANSI` for ANSI; `mcuboot`/`bridge`-only builds need no flag.

### Build (manual)

```bash
source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=$(pwd)/toolchain/zephyr-sdk-0.17.0

# Application
west build -b rainy75 zmk-src/app -- \
  -DZMK_CONFIG="$(pwd)/zmk/boards/rainy75" \
  -DZMK_EXTRA_MODULES="$(pwd)/zmk" \
  -DEXTRA_CONF_FILE="$(pwd)/conf/app.conf" \
  -DEXTRA_DTC_OVERLAY_FILE="$(pwd)/zmk/boards/rainy75/rainy75.keymap;$(pwd)/conf/mcumgr.overlay"

# MCUboot
west build -b rainy75 -d build-mcuboot bootloader/mcuboot/boot/zephyr -- \
  -DEXTRA_CONF_FILE="$(pwd)/conf/mcuboot.conf" \
  -DEXTRA_DTC_OVERLAY_FILE="$(pwd)/conf/mcuboot.overlay" \
  "-DDTS_ROOT=$(pwd)/zmk;$(pwd)/zmk-src/app" \
  "-DZMK_EXTRA_MODULES=$(pwd)/zmk;$(pwd)/zmk-src/app"
```

### Output

- Application signed binary: `build/zephyr/zmk.signed.bin` (with MCUboot image header)
- MCUboot binary: `build-mcuboot/zephyr/zephyr.bin`
- Combined flash image: `build/combined.bin` (MCUboot @ 0x0 + app @ 0x10000)

### Config split

Board defconfig (`rainy75_defconfig`) contains only hardware-essential configs (GPIO, flash, heap) shared by both MCUboot and the application. Application-specific configs (BLE, USB, ZMK, mcumgr, WDT, RGB) are in `conf/app.conf`, passed via `EXTRA_CONF_FILE`. This allows MCUboot to build cleanly against the same board definition.

## Hardware Bring-Up Checklist

`conf/app.conf` is structured in 5 stages. Start with Stage 0 (firmware backup and SWS validation), then Stage 1 (minimal USB + DFU), then uncomment stages one at a time. Each stage uploads via mcumgr DFU so MCUboot can revert if it fails. **Do not proceed to the next stage until all verification steps pass.**

### Stage 0: Original Firmware Backup & SWS Validation

**Before touching anything, dump the original firmware and prove that the SWS toolchain works end-to-end.**

#### Prerequisites: EVK Setup

Tools downloaded to `reverse/tools/bdt/`:
- **BDT v2.2.1** (Telink Burning & Debugging Tool, Linux x64 CLI)
- **BDT User Guide** (PDF, 13MB — full command reference)
- **Burning EVK User Guide** (PDF — hardware wiring)
- **EVK firmware v4.7** (required for BDT v2.2.0+, in `release/fw/`)
- **udev rules** (`99-telink-evk.rules` — USB ID `248a:826a/826b`, non-root access)

Helper script: `reverse/tools/sws_flash.sh` — wraps BDT for all Stage 0-1+ operations.

```bash
# One-time setup (installs udev rules, checks EVK connection)
./reverse/tools/sws_flash.sh setup

# If EVK firmware is below v4.7:
./reverse/tools/sws_flash.sh evk-version
./reverse/tools/sws_flash.sh evk-upgrade
```

**Wiring (3 wires):**

| EVK Pin | Keyboard Pad | Signal |
|---------|-------------|--------|
| SWM | Pad 3 (bottom side, near MCU) | SWS (PA7) |
| GND | Pad 1 | Ground |
| 3.3V | Pad 2 | VCC |

**Checklist before connecting:**
- Wireless switch OFF (under CapsLock keycap)
- USB cable connected (powers the keyboard, keeps MCU awake — avoids deep sleep blocking SWS)
- BDT chip selector: `B91` (default), try `CHIP=9518` if B91 fails

**BDT command reference (for manual use):**
```bash
BDT=reverse/tools/bdt/release/bdt
# IMPORTANT: B91 requires rst→ac before any flash operation!
# rst restores PA7 to SWS mode (firmware reconfigures it as GPIO on boot)
# ac initializes the flash controller for rf/wf commands
$BDT B91 rst                           # Reset chip (restore PA7 to SWS mode)
$BDT B91 ac                            # Activate chip (init flash controller)
$BDT B91 rf 0x00 -s 1024k -o dump.bin  # Read full 1MB flash
$BDT B91 wf 0x00 -i firmware.bin -e    # Erase + write flash
$BDT B91 rf 0xFE000 -s 16              # Read calibration (prints to stdout)
$BDT B91 rc 0x00020000 -s 256          # Read SRAM (256 bytes at 0x20000)
$BDT 8266 up -ev                       # Query EVK firmware version
# Once rst→ac is done, SWS stays active across multiple BDT commands
```

#### 0a. Dump original firmware ~~(3 independent reads)~~ DONE

```bash
./reverse/tools/sws_flash.sh dump
```

This reads full 1MB flash three times, compares checksums, and saves:
- `reverse/firmware/original_dump_{1,2,3}.bin` (three independent reads)
- `reverse/firmware/original_full_flash.bin` (verified copy)

- [x] All three SHA-256 checksums are identical (5 dumps, all `32479b18...`)
- [x] File size is exactly 1,048,576 bytes (1 MB)
- [x] 2MB read confirmed second 1MB is exact mirror — chip has exactly 1MB flash

#### 0b. Analyze the dump — DONE

Analysis performed manually (all checks pass):

- [x] TLNK header present at 0x00020 (`4B 4E 4C 54` = "KNLT" little-endian)
- [x] VIA keymaps at 0x84000-0x87FFF contain data (all 4 layers populated)
- [x] BLE MAC address at 0xFF000 is valid: `XX:XX:XX:XX:XX:XX`
- [x] Calibration at 0xFE000 is not blank: RF cap=0xDD, ADC Vref=0x8825 (5 non-FF bytes at 0xFE0C0)
- [x] OTA image (`firmware_ota.bin`) == flash 0x00000-0x1D553 **exact byte match**
- [x] `firmware_extracted.bin` == flash 0x012B0-0x1D553 (firmware body without boot header)
- [x] Calibration+MAC saved to `reverse/firmware/calibration_0xFE000.bin` (8KB, SHA-256: `5be2c186...`)

**Verified flash memory map:**
```
0x000000  120,148B  Boot vector + TLNK header + application firmware
0x01E000  409,600B  [unused, 0xFF]
0x082000    8,192B  VIA device config (0xAA55 magic + RGB/feature settings)
0x084000   16,384B  VIA keymaps (4 layers × 4KB)
0x088000    4,096B  [unused, 0xFF]
0x089000    8,192B  VIA macro storage (empty, zeros)
0x08B000  454,656B  [unused, 0xFF]
0x0FA000   12,288B  BLE bonding data (3 pages: crypto keys + conn params)
0x0FD000    4,096B  [unused, 0xFF]
0x0FE000    4,096B  Calibration: RF cap (0xDD) + ADC Vref (0x8825)
0x0FF000    4,096B  BLE MAC + 2nd address (XX:XX:XX:XX:XX:XX) + trailer (0x66)
```

Key findings:
- Flash is only 12% used (120KB firmware + 57KB data out of 1MB)
- BLE bonding pages at 0xFA000/0xFC000 are identical (redundant copy)
- 0xFB000 contains BLE connection parameters with repeating `96 9c 33 9c` pattern
- Second address at 0xFF100 (`XX:XX:XX:XX:XX:XX`) may be 2.4G dongle pairing address
- Calibration is minimal (5 bytes only) — chip has factory-default RF/ADC cal

#### 0c. Test SWS reflash round-trip — DONE

**Prove that writing and reading back via SWS produces identical data before risking custom firmware.**

```bash
./reverse/tools/sws_flash.sh roundtrip
```

This writes the original dump back to flash, reads it back, and verifies:

- [x] Readback checksum matches the original dump (`32479b18...` — identical)
- [x] Keyboard still works normally after the reflash (USB HID, keys, RGB, BLE)

Note: Flash has write protection (BP status 0x0030, protecting 0x00000-0x7FFFF). BDT `-f` flag auto-unlocks before writing. Write took ~43s, readback ~23s for 1MB.

### Stage 1: Initial SWS Flash (USB + DFU)

**Keep the Burning EVK connected throughout this stage.** Only disconnect after DFU is confirmed working.

Prerequisites:
- Stage 0 complete (original firmware backed up, SWS round-trip verified)
- `conf/app.conf` has only Stage 1 uncommented (default)

```bash
# 1. Build MCUboot bootloader (only needed once, unless conf/mcuboot.conf changes)
ZEPHYR_SDK_INSTALL_DIR=$(pwd)/toolchain/zephyr-sdk-0.17.0 \
  west build -b rainy75 -d build-mcuboot \
    bootloader/mcuboot/boot/zephyr --pristine \
    -- -DEXTRA_CONF_FILE=$(pwd)/conf/mcuboot.conf \
       -DEXTRA_DTC_OVERLAY_FILE=$(pwd)/conf/mcuboot.overlay \
       -DDTS_ROOT=$(pwd)/zmk-src/app

# 2. Build application (or just: ./build.sh -p)
./build.sh -p   # pristine rebuild, sets up env + EXTRA_CONF_FILE automatically

# 3. Create combined flash image (MCUboot at 0x0, app at 0x10000)
#    CRITICAL: must use zmk.signed.bin (has MCUboot image header), NOT zmk.bin
#    Or just: ./build.sh -c
python3 -c "
mcuboot = open('build-mcuboot/zephyr/zephyr.bin','rb').read()
app = open('build/zephyr/zmk.signed.bin','rb').read()
pad = 0x10000 - len(mcuboot)  # 64KB boot partition
combined = mcuboot + (b'\xff' * pad) + app
open('build/combined.bin','wb').write(combined)
print(f'MCUboot={len(mcuboot)} App={len(app)} Combined={len(combined)}')
"

# 4. Flash via SWS (Burning EVK)
#    Only writes up to end of firmware — calibration (0xFE000) and MAC (0xFF000) untouched
./reverse/tools/sws_flash.sh flash build/combined.bin <<< "y"
```

#### 1a. Verify USB enumeration and serial console — PASSED

- [x] `lsusb` shows `1d50:615e OpenMoko, Inc. Rainy 75 Pro` (3 interfaces: CDC ACM + HID)
- [x] `/dev/ttyACM0` appears (CDC ACM serial)
- [x] `cat /dev/ttyACM0` shows full Zephyr boot logs (zero dropped messages with tuned config)
- [x] Boot log shows LED strip init, boot_diag, WDT, USB attach/configure, ZMK welcome
- [x] Boot log shows "Image already confirmed" and "Watchdog disabled" at ~4.7s

**If USB doesn't enumerate within 5 seconds:** the custom USB DC driver is the single point of failure. Debugging options while SWS is connected:

- **Read boot_diag buffer via SWS** — the `CONFIG_BOOT_DIAG=y` module writes stage codes to a `.noinit` SRAM buffer at every init level and every substep of `usb_dc_attach()`. Read it via BDT:
  ```bash
  # 1. Find buffer address from ELF
  addr=$(riscv32-elf-nm build/zephyr/zephyr.elf | grep boot_diag_buf | cut -d' ' -f1)

  # 2. Read 36 bytes via SWS
  ./reverse/tools/sws_flash.sh sram 0x$addr 36

  # 3. Decode:
  #   Bytes 0-3: magic = A6 D1 07 B0 (LE 0xB007D1A6) — buffer is valid
  #   Byte 4:    count (number of stages recorded)
  #   Byte 5:    last stage code (quick check — where boot stopped)
  #   Bytes 8+:  stage history
  #
  #   Stage codes (in execution order):
  #     0x01 = EARLY            (before clock/PLL init)
  #     0x10 = PRE_KERNEL_1     (interrupt stack, no kernel)
  #     0x20 = PRE_KERNEL_2
  #     0x30 = POST_KERNEL      (kernel alive)
  #     0x40 = APPLICATION      (before USB/MCUboot)
  #     0x48 = MCUBOOT_CONFIRM  (WDT started)
  #     0x50 = USB_CLOCK        (USB clock + reset)
  #     0x51 = USB_POWER        (analog power-on)
  #     0x52 = USB_PINS         (PA5/PA6 pin mux)
  #     0x53 = USB_IRQ_MODE     (EP0 manual mode)
  #     0x54 = USB_EP_SETUP     (EP max size + timing)
  #     0x55 = USB_IRQ_CONNECT  (PLIC IRQ 11 dynamic reg)
  #     0x56 = USB_DP_PULLUP    (DP pullup — host sees device)
  #     0x57 = USB_ATTACHED     (attach complete)
  #     0xAA = RUNNING          (5s post-boot, image confirmed)
  ```
  **Common failure signatures:**
  - `last=0x01`: Crashed in PRE_KERNEL_1 — SoC or clock init problem
  - `last=0x40`: APPLICATION reached but USB didn't start — check ZMK USB Kconfig
  - `last=0x51`: Hung in USB power-on — analog register protocol issue
  - `last=0x55`: Hung at IRQ connect — PLIC IRQ 11 dynamic registration failed
  - `last=0x56`: Hung at DP pullup — analog register hang
  - `last=0x57`: USB attached but no enumeration — host-side issue or descriptor problem

- **GPIO heartbeat on PD7** — if `CONFIG_BOOT_DIAG_GPIO=y` (default when BOOT_DIAG is enabled), PD7 pulses LOW once per stage. Count pulses on a logic analyzer to identify the last successful stage without BDT.

- **Read back flash** via SWS to confirm the image was written correctly
- **Check MCUboot output** — if MCUboot itself fails, the chip won't even reach the app. Read the MCUboot region back and compare with the build output
- **Measure kscan GPIO pins** — if the kscan driver initialized, column pins should be toggling at ~500 Hz (visible on a logic analyzer). This confirms the MCU is running and Zephyr is scheduling

If all else fails: reflash the original firmware via SWS (verified in Stage 0c), debug the USB driver code, rebuild, try again.

#### 1a½. USB driver hardening checks (first boot only)

These items were flagged during code review and need hardware validation:

- [x] **EP0 DATA OUT byte count**: Validated — mcumgr uploaded 89KB over EP5 OUT without CRC errors or data corruption. If the pointer register reported `count + 1`, the base64/CRC16 framing would have caught it. No off-by-one observed.
- [~] **ISR/thread shared state**: No issues at Stage 1 load levels (mcumgr 3 KB/s sustained, USB HID). Monitor when BLE adds interrupt pressure (Stage 2). If intermittent stalls appear, add `irq_lock()`/`irq_unlock()` around `state.ep[n]` accesses in thread-context functions.

#### 1b. Verify key matrix — PASSED

- [x] Key presses register as USB HID input (ZMK logs show position decode → keymap → HID report)
- [ ] Test multiple keys across different rows/columns to verify full matrix
- [ ] Fn layer works (hold Fn + F-keys for media controls)
- [ ] No ghost keys or stuck keys

#### 1c. Verify mcumgr DFU — PASSED

- [x] `mcumgr echo hello` → `hello`
- [x] `mcumgr image list` → slot0, version 0.3.0, active confirmed, bootable
- [x] Upload image: `mcumgr image upload build/zephyr/zmk.signed.bin` — 89.5 KB in 29s (~3 KB/s)
- [x] `mcumgr image list` shows images in both slot0 and slot1 (different hashes)
- [x] `mcumgr image test <hash>` marks slot1 as `pending`
- [x] `mcumgr reset` triggers MCUboot swap (~3.6s), keyboard boots with new image
- [x] Boot log shows "Image confirmed — swap is now permanent" (first-time confirmation)
- [x] `mcumgr image list` shows new hash in slot0, old hash in slot1

**Note:** `mcumgr image upload` alone does NOT trigger a swap. Must use `mcumgr image test <hash>` to mark the slot1 image as pending, then `mcumgr reset`. First mcumgr command after fresh boot may timeout (CDC ACM not immediately ready) — retry after 1-2s.

#### 1d. Test MCUboot revert (WDT crash recovery) — PASSED

1. Added `while(1) { k_msleep(100); }` AFTER WDT setup in `mcuboot_confirm_init()` — WDT starts (10s), but image confirmation never happens
2. Uploaded via mcumgr, marked pending, reset

Results (total recovery: 24 seconds):
- [x] Broken image boots — USB never enumerates (while(1) blocks APPLICATION/90, USB init at APPLICATION/99 never runs)
- [x] After ~10 seconds, WDT fires → full SoC reset
- [x] MCUboot sees unconfirmed image → swaps back to previous working image (~3.6s)
- [x] Keyboard boots normally with `[SWAP TEST]` marker in logs
- [x] `mcumgr image list` shows working image back in slot0 (hash `2ffb...`), broken image in slot1 (hash `9afe...`)

3. Removed `while(1)` — `mcuboot_confirm.c` restored to normal

**Stage 1 complete.** The WDT revert safety net is proven. All future stages can be tested via mcumgr DFU with automatic rollback on failure.

#### Recovery Options (before needing SWS)

Three recovery paths exist, in order of preference:

1. **WDT auto-revert** (automatic, no user action needed)
   - Covers: app crashes, hangs, USB driver failures during first boot of a new image
   - How: MCUboot marks new image as "test". If the app doesn't call `boot_write_img_confirmed()` within 10s (WDT timeout), the SoC resets and MCUboot swaps back to the previous image
   - Limitation: only works for *unconfirmed* images (first boot after upload). Once the app confirms the image (~5s after boot), WDT revert is no longer possible

2. **MCUboot serial recovery** (manual, requires `NO_APPLICATION` build)
   - Covers: confirmed-but-broken images (WDT can't revert), mcumgr not responding in the app
   - How: build MCUboot with `CONFIG_BOOT_SERIAL_NO_APPLICATION=y`, flash via SWS. MCUboot enters serial recovery unconditionally — upload a new image directly to slot0
   - Command: `mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" image upload build/zephyr/zmk.signed.bin`
   - Speed: ~3.6 KiB/s (SMP serial framing, 127-byte frames + base64 overhead)
   - Then: reflash normal MCUboot (without `NO_APPLICATION`) + combined image via SWS

3. **mcumgr DFU** (normal path, no timing constraints)
   - Covers: routine firmware updates when the app is running normally
   - How: upload new image to slot1, mark pending, reset → MCUboot swaps
   - Command: `mcumgr image upload build/zephyr/zmk.signed.bin` then `mcumgr image test <hash>` then `mcumgr reset`

**SWS (Burning EVK) is only needed if both above fail** — which requires: (a) the app confirmed a broken image, AND (b) MCUboot itself is broken. Since MCUboot is rarely reflashed, this should not happen in normal development.

After all Stage 1 verification passes, the Burning EVK can be disconnected. All future updates use mcumgr (path 3) with WDT safety (path 1) and serial recovery (path 2) as fallbacks.

### Stage 2: BLE (via mcumgr DFU)

```bash
# 1. Uncomment Stage 2 block in conf/app.conf
# 2. Build (pristine required after Kconfig changes)
./build.sh -p

# 3. Upload via mcumgr (~30s for ~90KB)
~/go/bin/mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" \
    image upload build/zephyr/zmk.signed.bin

# 4. Mark new image for test boot and reset
HASH=$(~/go/bin/mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" \
    image list 2>&1 | grep -A1 "slot=1" | grep hash | awk '{print $2}')
~/go/bin/mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" \
    image test "$HASH"
~/go/bin/mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" reset
# Wait ~20s for swap + boot. If crash: WDT reverts in ~24s total.
```

Verify:
- [x] Serial console shows BLE controller init (blob init, MAC address, advertising start)
- [x] USB still works — serial console, USB HID key input (regression check)
- [x] `mcumgr ... image list` still works (DFU regression check)
- [x] BLE advertises "Rainy 75 Pro" (check with `bluetoothctl scan on` or phone BLE scanner)
- [x] BLE pairing succeeds — Passkey Entry (6-digit code), Security Level 4 (SC + authenticated)
- [x] BLE HID input works — key presses arrive over BLE
- [x] Switching between USB and BLE works — Fn+F4 (`&out OUT_TOG`), or plug/unplug USB
- [x] BLE connection is stable over 5+ minutes of use
- [x] BLE reconnects after keyboard power cycle

**Blob bugs found:**
- **2M PHY disabled** — `blc_ll_init2MPhyCodedPhy_feature()` must NOT be called. After the central requests PHY update to 2M, the blob's radio loses packets, causing LL Response Timeout (0x22) exactly 40 seconds later. 1M PHY works perfectly.
- **BT_PRIVACY incompatible** — `CONFIG_BT_PRIVACY=y` hangs during `bt_enable()`. Blob doesn't support `LE_Set_Random_Address` HCI command.
- **First connection 0x3E** — first BLE connection after boot fails with "Connection Failed to be Established" in ~30ms. Benign — automatic retry succeeds within 300ms.

### Stage 3: RGB Underglow (via mcumgr DFU) — COMPLETE

Uncomment Stage 3 block in `conf/app.conf`. Build with `--pristine`, upload, reset.

Verify:
- [x] 83 WS2812 per-key LEDs light up with ZMK default underglow effect on boot
- [x] ZMK RGB controls work (brightness up/down, effect cycle, hue/saturation)
- [x] RGB off command works (LEDs turn off completely)
- [x] RGB state persists across power cycles (stored in NVS)
- [x] USB HID still works (regression check)
- [x] BLE still works — connection, pairing, HID input (regression check)
- [x] No visible flicker or color artifacts — PSPI+DMA has zero timing jitter
- [x] Serial logs show no DMA or SPI errors

**Key findings:**
- **GPIO bit-bang failed for LED 1** — first-bit timing jitter (cache miss + branch overhead before first GPIO HIGH) caused LED 1 (first in WS2812 chain) to persistently show green regardless of intended color. LEDs 2-83 worked fine with bit-bang.
- **PSPI+DMA solved it** — hardware-timed SPI transfers have zero jitter. Each WS2812 bit encoded as 1 SPI byte at 6 MHz: `0xF0` for "1" (667ns high), `0xC0` for "0" (333ns high). Matches OEM firmware approach exactly.
- **PB5 is a matrix column** — PB5 is PSPI CLK pin BUT also keyboard matrix column 13 (`gpiob 5` in DTS). Configuring PB5 as PSPI CLK broke that column (most keys dead). Fix: only configure PB7 as PSPI MOSI — PSPI internal clock runs without CLK pin routed to physical pin.
- **PC2 HIGH = LED power** — PC2 controls a MOSFET gating LED VCC. PC2 HIGH = power ON (active-high, confirmed by testing both polarities).
- **NVS brightness gotcha** — ZMK stores RGB brightness in NVS. The BREATHE effect overrides brightness with its own animation cycle, masking a stored brightness=0. Other effects (SOLID, SPECTRUM, SWIRL) use stored brightness directly, so they appear "off" until brightness is increased.

**Fn-layer RGB bindings:**

| Key | Binding | Function |
|-----|---------|----------|
| Fn+Backspace | `&rgb_ug RGB_TOG` | Toggle RGB on/off |
| Fn+Enter | `&rgb_ug RGB_EFF` | Cycle effect |
| Fn+NUHS | `&rgb_ug RGB_HUI` | Cycle hue |
| Fn+↑ | `&rgb_ug RGB_BRI` | Brightness up |
| Fn+↓ | `&rgb_ug RGB_BRD` | Brightness down |
| Fn+← | `&rgb_ug RGB_SPD` | Speed down |
| Fn+→ | `&rgb_ug RGB_SPI` | Speed up |

### Stage 4: Battery ADC (COMPLETE)

Battery voltage sensed via PD1 (ADC channel 0x0A) through a 1/2 resistor divider.
`CONFIG_BATTERY_B91_ADC=y` + `CONFIG_ZMK_BATTERY_REPORTING=y` in `conf/app.conf`.

Verify:
- [x] PD1 ADC reads ~2150-2180 mV (= ~4300-4360 mV battery, fully charged)
- [x] Battery percentage reported over BLE (phone shows battery level)
- [ ] Voltage reading changes when charging vs discharging
- [x] All other features still work (regression check)

### Stage 5: Deep Sleep PM (via mcumgr DFU)

**Implementation**: Deep retention 64K sleep (`DEEPSLEEP_MODE_RET_SRAM_LOW64K`, ~2.7µA).
Bottom 64KB of ILM SRAM retained — BLE controller state survives sleep.

**Files**:
- `zmk/src/poweroff.c` — `z_sys_poweroff()`: turns off RGB/USB, configures analog pull-downs on columns (100K) and pull-ups on rows (1M), configures row wakeup, enters `DEEPSLEEP_MODE` (cold boot on wakeup)
- `patches/zephyr/0004-*` — adds `HAS_POWEROFF` to tlsr951x Kconfig

**How it works**:
1. ZMK activity.c detects 15min idle → calls `sys_poweroff()`
2. `z_sys_poweroff()` shuts down peripherals, enters deep sleep with GPIO pad wakeup
3. Any keypress pulls a row LOW → MCU cold-boots through MCUboot (~1-2s)

**Note**: Retention mode (`DEEPSLEEP_MODE_RET_SRAM_LOW64K`, 0x03) is incompatible with MCUboot.
The boot ROM reloads MCUboot into ILM on any reset, overwriting retained app code.
Using `DEEPSLEEP_MODE` (0x30, cold boot) instead.

**Config** (`conf/app.conf`):
```
CONFIG_ZMK_SLEEP=y
CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=900000  # 15 minutes
```
`CONFIG_POWEROFF=y` is auto-selected by `HAS_POWEROFF` in SoC Kconfig.

Build with `./build.sh -pa`, upload via mcumgr, reset.

Verify:
- [ ] Keyboard enters deep sleep after idle timeout
- [ ] Keypress wakes it up promptly
- [ ] USB re-enumerates after wake (if connected)
- [ ] BLE reconnects after wake (retention recovery)
- [ ] RGB resumes correct state after wake
- [ ] No data loss or stuck keys after wake
- [ ] Multiple sleep/wake cycles work reliably

## BLE HCI Driver

### Why a custom driver

Zephyr had a B91 BLE HCI driver from v3.2 to v3.6. It was removed in v3.7 (PR #73289, May 2024) because:
- The BLE controller blob was unmaintained
- The hal_telink BLE shim code uses `<zephyr.h>` (pre-v3.1 header, doesn't exist in v4.x)
- Nobody was testing or updating it

We revive this functionality with a clean implementation:

1. **Our own Kconfig namespace** (`BT_HCI_B91`) to avoid colliding with hal_telink's `BT_B91` guards
2. **Our own shim** (`b91_bt.c`) that declares blob functions as `extern` with standard C types — no SDK headers needed
3. **Zephyr v4.1 device-model API** (`DEVICE_API(bt_hci, ...)`) instead of the removed legacy API

### Driver architecture

| File | Role | Lines |
|------|------|-------|
| `hci_b91.c` | Zephyr HCI device driver — `open`/`send`/`close` + HCI packet parsing | 250 |
| `b91_bt.c` | Shim — blob init, controller thread, IRQ handlers, FIFO management | 320 |
| `b91_bt.h` | Public API — `controller_init`, `send_packet`, `callback_register` | 25 |

**Data flow (host → controller):**
```
Zephyr BT Host → hci_b91_send() → b91_bt_host_send_packet()
    → write to bltHci_rxfifo → blob's blc_hci_handler()
```

**Data flow (controller → host):**
```
blob main loop → bltHci_txfifo → b91_bt_hci_tx_handler()
    → host_read_packet callback → hci_b91_host_recv()
    → bt_buf_get_evt()/bt_buf_get_rx() → data->recv(dev, buf)
```

### Blob details

| Property | Value |
|----------|-------|
| Source | `telink-semi/zephyr_hal_telink_b91_ble_lib` (GitHub) |
| File | `liblt_9518_zephyr.a` (2.8 MB) |
| Compiled with | GCC + LTO, Zephyr-compatible relocations |
| API version | Older "Slave" naming (e.g., `blc_ll_initAclSlaveRole_module`) |
| Symbols exported | 1438 (verified via `nm --defined-only`) |
| Symbols needed from us | `swapN`, `swapX` (byte-swap utilities) |

The blob also defines `sys_init` (from its LTO'd `ext_pm.c.o`), which conflicts with hal_telink's `drivers/B91/sys.c`. We patch hal_telink's `CMakeLists.txt` to skip `sys.c` when `CONFIG_BT_HCI_B91=y`, same as it already does for `CONFIG_BT_B91`.

### BLC init sequence

Ported from hal_telink's `b91_bt_init.c`, peripheral-only (0 masters, 1 slave):

1. `trng_init()` — hardware random number generator
2. MAC address init from flash at `0xFF000` — reads 8 bytes, generates random if blank
3. `blc_ll_initBasicMCU()` → `blc_ll_initStandby_module(mac)`
4. `blc_ll_initLegacyAdvertising_module()`
5. `blc_ll_initAclConnection_module()` + `blc_ll_initAclSlaveRole_module()`
6. Buffer init — ACL RX/TX FIFOs + HCI RX/TX/ACL FIFOs
7. `blc_ll_setMaxConnectionNumber(0, 1)` — peripheral only
8. `blc_ll_initChannelSelectionAlgorithm_2_feature()` (2M PHY **disabled** — blob radio bug, see below)
9. HCI handler registration + event masks
10. `blc_controller_check_appBufferInitialization()` — blob self-validates

### IRQ and threading

- **IRQ 1** (SYSTIMER) and **IRQ 15** (RF/ZB_RT) — both call `blc_sdk_irq_handler()`
- Controller thread: runs `blc_sdk_main_loop()` every 2 ms
- Thread stack: `CONFIG_BT_HCI_B91_RX_STACK_SIZE` (set to 2048; default 1024 is tight per hal_telink references)
- Thread priority: `CONFIG_BT_HCI_B91_RX_PRIO` (default 7)

### Weak stubs

The blob references symbols that aren't used in BLE peripheral mode. We provide weak empty stubs:

| Symbol | Purpose | Why stubbed |
|--------|---------|-------------|
| `blc_gatt_pushHandleValueNotify` | GATT notify | Zephyr host handles GATT |
| `host_ota_main_loop_cb` | OTA callback | No OTA support yet |
| `host_ota_terminate_cb` | OTA callback | No OTA support yet |
| `usb_send_upper_tester_result` | USB test | Not applicable |

## Board Definition

### Devicetree

Based on `telink_b91.dtsi` from hal_telink. Key nodes:

| Node | Configuration |
|------|---------------|
| CPU | 48 MHz, RV32IMACF |
| ILM | 128 KB at `0x00000000` |
| DLM | 128 KB at `0x00080000` |
| Flash | 1 MB at `0x20000000` |
| Partitions | boot (64K) + slot0 (448K) + slot1 (448K) + storage (56K) |
| GPIO | Ports A-E enabled |
| kscan | 16 cols × 6 rows, `col2row`, `GPIO_ACTIVE_LOW` |
| BLE HCI | `telink,b91-bt-hci`, status "okay" |
| USB controller | `telink,b91-usbd` at `0x80100800`, PLIC IRQ 11 |
| CDC ACM UART | Defined in `conf/mcumgr.overlay` (app) and `conf/mcuboot.overlay` (bootloader) |
| LED strip | `telink,b91-spi-led-strip`, 83 per-key LEDs, GRB, `zmk,underglow` |
| Watchdog | `telink,b91-watchdog` at `0x80140140`, `watchdog0` alias, MCUboot crash revert |
| Battery ADC | `telink,b91-battery-adc`, PD1 channel 0x0A, 1/2 divider, `zmk,battery` |
| Flash controller | `telink,b91-flash-controller` at `0x80140100`, `zephyr,flash-controller` |
| pinctrl | `pad-mul-sel = <1>` |

**GPIO matrix (from [gpio-matrix.md](gpio-matrix.md)):**

| Columns (16) | Rows (6) |
|--------------|----------|
| PE4, PE5, PE6, PE7 | PE0 |
| PA0, PA1, PA2, PA3, PA4 | PD2, PD3, PD4, PD5, PD6 |
| PB1, PB2, PB3, PB4, PB5, PB6 | |
| PC1 | |

All GPIO_ACTIVE_LOW. Rows have GPIO_PULL_UP. Diode direction: col2row.

### Flash Layout

```
0x00000 ┌──────────────────────┐
        │ Boot (MCUboot) 64K   │
0x10000 ├──────────────────────┤
        │ Slot 0 (active) 448K │
0x80000 ├──────────────────────┤
        │ Slot 1 (backup) 448K │
0xF0000 ├──────────────────────┤
        │ NVS Storage 56K      │
0xFE000 ├──────────────────────┤
        │ Calibration 4K (RO)  │  ← RF/ADC cal data, Telink SDK
0xFF000 ├──────────────────────┤
        │ MAC address 4K (RO)  │  ← BLE MAC at 0xFF000-0xFF005
0x100000└──────────────────────┘
```

**Reserved regions (must not be overwritten):**
- `0xFE000-0xFEFFF`: RF/ADC calibration data written by Telink SDK at factory
- `0xFF000-0xFF005`: BLE MAC address (read by blob's `blc_ll_initStandby_module`)

The Telink reference board (`tlsr9518adk80d`) uses 64K boot + 448K slots + 16K scratch + 44K storage, ending at `0xFDFFF` to preserve the same reserved regions. Our layout uses 64K boot (same as Telink reference) and no scratch partition (`swap-using-move` mode), giving more room for NVS storage.

**Stock firmware regions (not applicable to ZMK, for reference):**
- `0x84000-0x89000`: VIA keymap layers + macros (stock Evision firmware only)

### Keymap

2-layer ISO DE 75% layout:

- **Layer 0** — default: ESC, F1-F12, full alphanumeric, ISO hash, NUBS (`<>` key)
- **Layer 1** — Fn: Studio unlock (ESC), BT profile select (F1-F3), output toggle (F4), media keys (F5-F12), RGB controls

### Defconfig

**Board defconfig** (`rainy75_defconfig`) — hardware-only, shared by app + MCUboot:

```
CONFIG_GPIO=y / CONFIG_PINCTRL=y
CONFIG_HEAP_MEM_POOL_SIZE=4096
CONFIG_FLASH=y / CONFIG_FLASH_MAP=y / CONFIG_FLASH_PAGE_LAYOUT=y
```

**App config** (`conf/app.conf`) — applied via `EXTRA_CONF_FILE`. Key sections:

- **USB**: `USB_DC_B91`, `ZMK_USB`, CDC ACM for mcumgr (via `conf/mcumgr.overlay`)
- **Logging**: ring buffer only (no UART backend), `ZMK_LOGGING_MINIMAL`
- **BLE**: `BT_HCI_B91`, Passkey Entry, 1M PHY only, `ZMK_BLE_PASSKEY_ENTRY`
- **DFU**: mcumgr USB UART (primary) + BLE SMP (backup), swap-using-move
- **RGB**: `ZMK_RGB_UNDERGLOW`, PSPI+DMA, PC2 power MOSFET
- **Battery**: `BATTERY_B91_ADC`, PD1 channel 0x0A, BLE battery service
- **Sleep**: `ZMK_SLEEP`, 15min idle → `sys_poweroff` (DEEPSLEEP_MODE, cold boot)
- **Studio**: `ZMK_STUDIO`, BLE GATT transport (WebBluetooth), unlock via Fn+ESC

## USB DC Driver

The USB device controller driver (`zmk/drivers/usb/usb_dc_b91.c`) implements Zephyr's legacy `usb_dc.h` API — all 21 `usb_dc_*` functions: lifecycle management, endpoint configuration, data transfer, and ISR handling. It uses `irq_connect_dynamic()` for USB PLIC IRQs (avoids PLIC source 11 collision with machine external interrupt 11).

On top of the DC layer:
- **CDC ACM UART** provides mcumgr SMP transport (`zephyr,uart-mcumgr`, via `conf/mcumgr.overlay`)
- **USB HID** provides keyboard/consumer/system HID interfaces for ZMK

A new-API UDC driver (`udc_b91.c`) is also present for future use when ZMK migrates to `USB_DEVICE_STACK_NEXT`. The legacy `usb_dc.h` API is deprecated in Zephyr, with removal targeted for Zephyr 4.5 (~Oct 2026). ZMK upstream is working on migration.

## RGB LED Strip Driver

WS2812 LED strip driver via Telink B91 PSPI + DMA. All register sequences from decompiled original firmware (`secondary_pipeline`, `hid_report_build`).

| Property | Value |
|----------|-------|
| SPI peripheral | PSPI (Peripheral SPI) at `0x80140040` |
| Data pin | PB7 = PSPI MOSI IO0 (only MOSI configured — CLK pin NOT routed) |
| SPI clock | 6 MHz (PCLK 24MHz / ((1+1)*2), divider=1) |
| DMA channel | ch4 (TX only) |
| LED count | 83 per-key (per DTS `chain-length`, confirmed from firmware analysis) |
| Color order | GRB (WS2812 standard) |
| Encoding | 1-bit = `0xF0` (667ns high), 0-bit = `0xC0` (333ns high) |
| SPI buffer | 83 × 24 = 1992 bytes (static, 4-byte aligned, in BSS) |
| Reset pulse | 500 µs busy-wait after DMA completes (V5/C variant spec) |
| LED power | PC2 HIGH via MOSFET (`CONFIG_LED_STRIP_B91_SPI_PC2_POWER=y`) |

Implements Zephyr `led_strip` API: `update_rgb`, `length`. ZMK underglow enabled via `CONFIG_ZMK_RGB_UNDERGLOW=y`.

Register definitions in `b91_pspi.h` cover PSPI mode/control/FIFO registers, DMA channel registers (base + stride), GPIO PB7 pin mux, and DMA C-bus address translation.

**Critical: PB5 is a matrix column, NOT PSPI CLK.** PB1-PB6 are all keyboard matrix column pins in the DTS. PB5 would be PSPI CLK (function 1), but configuring it as SPI breaks column 13 (most keys dead). The PSPI internal clock runs regardless of whether CLK is routed to a physical pin — only MOSI (PB7) needs SPI function mode.

**Why not GPIO bit-bang:** An earlier GPIO bit-bang approach worked for LEDs 2-83 but LED 1 (first in chain) always showed persistent green due to first-bit timing jitter. The WS2812 protocol is extremely sensitive to the first rising edge — any extra latency from cache misses or branch overhead before the first GPIO HIGH pulse causes LED 1 to latch the wrong bit. PSPI+DMA has zero CPU involvement during the transfer, zero timing jitter.

## Battery ADC Driver

SAR ADC battery voltage sensor (`CONFIG_BATTERY_B91_ADC`):

- Battery voltage on PD1 (ADC channel 0x0A) via 1/2 resistor divider
- Zephyr `sensor` API: `sample_fetch` + `channel_get`
- Channels: `GAUGE_VOLTAGE` (millivolts) + `GAUGE_STATE_OF_CHARGE` (percentage)
- DT-configured: ADC channel mux, voltage divider ratio, full/empty thresholds
- Linear SoC calculation between empty and full voltage

| ADC Property | Value |
|-------------|-------|
| Clock | 4 MHz (24 MHz / 6) |
| Resolution | 14-bit |
| Vref | 1.2V (calibrated 1175 mV) |
| Prescale | 1/4 |
| Analog access | Serial interface at `0x80140180` |

## Deep Sleep

Implemented in `zmk/src/poweroff.c` as `z_sys_poweroff()`, triggered by ZMK after 15 minutes idle (`CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=900000`).

**Sequence:** RGB off (PC2 LOW) → USB DP pullup off → configure analog pull-downs on columns (100K) + pull-ups on rows (1M) → configure GPIO pad wakeup on all 6 row pins (LOW-level trigger) → enter `DEEPSLEEP_MODE` (0x30, cold boot).

**Wakeup:** Any keypress pulls a row LOW → MCU cold-boots through MCUboot (~1-2s). No state is retained.

**Why not retention mode:** `DEEPSLEEP_MODE_RET_SRAM_LOW64K` (0x03) retains 64KB of ILM SRAM, but MCUboot's boot ROM reloads its own code into ILM on any reset, overwriting the retained app code. Cold boot (0x30) is used instead.

**GPIO wakeup config:** 6 row pins (PD2-PD6, PE0) via `pm_set_gpio_wakeup()`. SDK register layout: 0x41-0x45 = polarity (SET = LOW-level), 0x46-0x4A = enable. Wakeup status register 0x64 guards entry — all rows must be HIGH (no key pressed) to enter sleep.

## MCUboot DFU

USB-based firmware updates via mcumgr, with watchdog-based crash revert.

### Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Boot (64KB)   │  Slot 0 (448KB)  │  Slot 1 (448KB)        │
│  MCUboot       │  Active image    │  Upload target          │
│  swap-using-   │  (ZMK firmware)  │  (new image via mcumgr) │
│  move, no sig  │                  │                         │
└──────────────────────────────────────────────────────────────┘
```

**Swap mode:** `swap-using-move` — MCUboot copies slot0→slot1 sector-by-sector, then copies new image to slot0. On failure/crash, reverts by swapping back.

**Image signing:** Disabled (`CONFIG_BOOT_SIGNATURE_TYPE_NONE`). Development mode — no cryptographic verification.

### Reflash workflow

```bash
# 1. Build new firmware
ZEPHYR_SDK_INSTALL_DIR=$(pwd)/toolchain/zephyr-sdk-0.17.0 \
  west build -b rainy75 -s zmk-src/app --pristine \
  -- -DEXTRA_CONF_FILE=$(pwd)/conf/app.conf

# 2. Upload via mcumgr (over USB serial)
mcumgr --conntype serial --connstring /dev/ttyACM0,baud=115200 \
    image upload build/zephyr/zmk.signed.bin

# 3. List images (verify upload)
mcumgr --conntype serial --connstring /dev/ttyACM0 image list

# 4. Reset (triggers swap)
mcumgr --conntype serial --connstring /dev/ttyACM0 reset
```

### Image confirmation flow

1. **MCUboot boots** — checks slot0 for pending swap, executes swap if needed, boots app
3. **Application starts** — `mcuboot_confirm_init` at APPLICATION/90 installs 10s WDT timeout
4. **USB + BLE init** — standard ZMK initialization at APPLICATION/99
5. **5s delayed work** — calls `boot_write_img_confirmed()` to make swap permanent, disables WDT
6. **If crash occurs** — WDT fires after 10s, chip resets, MCUboot sees unconfirmed image → reverts

**Future:** MCUboot v2.3.0+ supports starting WDT in the bootloader itself (`BOOT_WATCHDOG_SETUP_AT_BOOT`), covering the gap between MCUboot boot and app WDT init. This requires Zephyr 4.3+ (MCUboot v2.3.0 is incompatible with Zephyr 4.1 on RISC-V). When ZMK upgrades, we can simplify: MCUboot starts WDT → driver preserves it → app feeds/confirms/disables. The DTS `watchdog0` alias is already in place for this.

### Watchdog driver

B91 hardware watchdog (`wdt_b91.c`), register-direct Zephyr `wdt` API:

| Property | Value |
|----------|-------|
| Register base | `0x80140140` (timer block) |
| Clock | PCLK (24 MHz) |
| Max timeout | ~11,184 ms |
| Reset type | Full SoC reset (no interrupt/callback) |
| Channels | 1 (channel 0) |

### MCUboot build notes

MCUboot builds as a separate application against the same board DTS. The overlay (`conf/mcuboot.overlay`) disables all peripherals (BLE, USB, kscan, RGB, WDT, battery). The `-DDTS_ROOT=$(pwd)/zmk-src/app` flag is needed so the DTS preprocessor can find ZMK's `dt-bindings/zmk/matrix_transform.h` header.

**MCUboot version:** Pinned to `v2.2.0` (June 2025) in `west.yml`. Upgrade from previous pin (commit `346f7374`, between v2.1.0 and v2.2.0) brings +102 commits: swap_move max-size fixes, watchdog feeding during erase, SHA init crash fix. MCUboot v2.3.0+ is incompatible with Zephyr 4.1 on RISC-V (`IS_BOOTLOADER` Kconfig requires `XIP && ARM`).

**Sector calculation:** `CONFIG_BOOT_MAX_IMG_SECTORS=112` (448KB slot / 4KB erase sector = 112). Set explicitly because `BOOT_MAX_IMG_SECTORS_AUTO` fails silently on Telink B91 — DTS has no `erase-block-size` property, so CMake warns "Unable to determine erase size" and falls back to 128. The correct value must match the actual partition/erase geometry.

**Serial recovery:** `CONFIG_BOOT_SERIAL_NO_APPLICATION=y` (uncomment in `conf/mcuboot.conf` when needed). MCUboot enters serial recovery unconditionally — no timing window, no VID switch. Flash this MCUboot variant via SWS, upload a working app via mcumgr, then reflash the normal MCUboot.

MCUboot ROM usage: ~49KB (77% of 64KB boot partition). Includes USB device stack, CDC ACM, serial recovery (mcumgr), and multithreading kernel. ~15KB headroom for future additions (Ed25519 signing would add ~4KB).

## Upstream Patches

All upstream modifications are tracked as `git format-patch` files in `patches/` and auto-applied by `build.sh` before each build. After `west update`, patches are re-applied automatically.

```
patches/
  zephyr/
    0001-gpio-b91-fix-WRITE_BIT-double-BIT.patch
    0002-gpio-b91-fix-interrupt-support.patch
    0003-flash-b91-report-erase-sectors.patch
    0004-soc-tlsr951x-select-HAS_POWEROFF-for-deep-sleep-supp.patch
  mcuboot/
    0001-b91-riscv-boot-fixes.patch
  hal_telink/
    0001-exclude-sys-for-BT_HCI_B91.patch
```

### Zephyr (4 files, 4 patches)

**`drivers/gpio/gpio_b91.c`** — WRITE_BIT double-BIT fix **[VERIFIED]**

```diff
-WRITE_BIT(gpio->actas_gpio, BIT(pin), 1);
+WRITE_BIT(gpio->actas_gpio, pin, 1);
```

`WRITE_BIT` internally applies `BIT()`, so `BIT(BIT(pin))` overflows `uint8_t` for pins >= 3. Pins 0-2 work by coincidence. Affects all GPIO pin_configure calls.

**`drivers/gpio/gpio_b91.c`** — GPIO interrupt support with multi-level PLIC **[VERIFIED]**

Four bugs that make GPIO interrupts completely non-functional on B91:

1. `DT_INST_IRQN()` returns multi-level encoded values (e.g. `0x1A00` for PLIC source 25), but `irq_set()` stores in `uint8_t` → truncated to 0. All comparisons against `IRQ_GPIO` (25) fail.
2. `riscv_plic_irq_enable()` called with truncated value → enables PLIC source 0 (nothing).
3. No `irq_enable()` in init path — PLIC source never unmasked.
4. RISC0/RISC1 per-pin enable registers have non-zero POR defaults → ISR storm on PLIC unmask without clearing first.

Fix: split `irq_num` into raw PLIC source (register config) and encoded form (`irq_enable` API), guard config with `IS_INST_IRQ_EN`, add `irq_enable()` in init with per-pin clear. Requires board DTS to override port interrupt from 3 IRQs to 1 (e.g. `interrupts = <25 1>` for IRQ_GPIO).

*Nobody noticed because SoC DTS defines 3 IRQs per port → `IS_INST_IRQ_EN` always false → `IRQ_CONNECT` never runs → all B91 users use polling.*

**`drivers/flash/soc_flash_b91.c`** — Flash page layout: 4KB erase sectors **[VERIFIED]**

```diff
-	.pages_count = FLASH_SIZE / PAGE_SIZE,
-	.pages_size = PAGE_SIZE,
+	.pages_count = FLASH_SIZE / SECTOR_SIZE,
+	.pages_size = SECTOR_SIZE,
```

Upstream reports 256B programming pages. MCUboot enumerates these as swap sectors — 456KB slot / 256B = 1,824 sectors, overflowing `BOOT_MAX_IMG_SECTORS` (128). With 4KB erase sectors: 114 entries, fits.

*Empirical test*: Reverted → MCUboot enters serial recovery instead of booting app.

**`soc/telink/tlsr/tlsr951x/Kconfig`** — Enable HAS_POWEROFF for deep sleep

```diff
+	select HAS_POWEROFF
```

TLSR951x supports `sys_poweroff()` via deep retention sleep, but upstream never declared `HAS_POWEROFF`. Without it, `CONFIG_POWEROFF` (and thus `CONFIG_ZMK_SLEEP`) cannot be enabled.

### MCUboot (1 file, 1 patch)

**`boot/zephyr/main.c`** — B91 RISC-V boot fixes **[VERIFIED]**

- **do_boot XIP fix**: upstream groups RISC-V with Xtensa, copying image to SRAM `0xBE030000`. B91 boots from flash (XIP), `0xBE030000` is invalid. Without fix: no USB enumeration. *Note: upstreamed in MCUboot `main` (commit `2750a58c`, March 2026) but not yet in any release.*
- **boot_console_init early**: moved before DFU wait guard — USB init side effects (clock/DMA) needed by flash controller for image hash verification.
- **fence.i**: flush instruction cache before jumping to app (generic RISC-V, guarded by `CONFIG_RISCV`).

### hal_telink (1 file, 1 patch)

**`tlsr9/CMakeLists.txt`** — BT_HCI_B91 guard **[DEFERRED: Stage 2 BLE]**

```diff
-if (NOT CONFIG_PM AND NOT CONFIG_BT_B91)
+if (NOT CONFIG_PM AND NOT CONFIG_BT_B91 AND NOT CONFIG_BT_HCI_B91)
```

BLE controller blob defines `sys_init()`, collides with hal_telink's `sys.c`. Required when `CONFIG_BT_HCI_B91=y`.

### zmk-src (1 file, 1 patch)

**`app/Kconfig` + `app/src/activity.c`** — `ZMK_USB_NO_VBUS_DETECT` for boards without VBUS sensing

B91 has no USB VBUS detection pin. Without this patch, `is_usb_power_present()` returns true (USB status stays at SUSPEND after unplug), preventing deep sleep from ever triggering. When `CONFIG_ZMK_USB_NO_VBUS_DETECT=y`, `is_usb_power_present()` always returns false, allowing the idle sleep timeout to work.

### Reverted fixes (proven unnecessary)

- **`serial_adapter.c` k_yield**: Serial recovery works without it — USB IRQs handle CDC ACM receive independently.
- **`serial_adapter.c` uart_mcumgr chosen node**: `DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart)` resolves identically.
- **`start.S` BIN_SIZE field**: `.org 0x18 / .word 0` is a no-op — gap-fill already zeros these bytes.

## Key Decisions and Workarounds

### BT_HCI_B91 vs BT_B91

hal_telink's `CMakeLists.txt` uses `CONFIG_BT_B91` to gate its own BLE code (broken `<zephyr.h>` includes) and blob linking. Using `BT_B91` triggers compilation of hal_telink's BLE shim which fails on Zephyr 4.1.

We use `BT_HCI_B91` to:
- Avoid triggering hal_telink's broken BLE code path
- Compile our own clean shim instead
- Link the blob ourselves from `zmk/CMakeLists.txt`

We still patch hal_telink's `CMakeLists.txt` line 15 to add `NOT CONFIG_BT_HCI_B91` so that `sys.c` is excluded when our driver links the blob (which also defines `sys_init`).

### SDK 0.17.0 requirement

Zephyr v4.1.0 expects SDK 0.17.0 (per `zephyr/SDK_VERSION`). SDK 0.17.4's picolibc headers are incompatible, causing `conflicting types for __lock___libc_recursive_mutex`. Always set `ZEPHYR_SDK_INSTALL_DIR=$(pwd)/toolchain/zephyr-sdk-0.17.0` when building.

### Blob symbol naming

The blob uses older SDK naming conventions:

| Our shim calls | Not available (newer SDK) |
|----------------|--------------------------|
| `blc_ll_initAclSlaveRole_module` | `blc_ll_initAclPeriphrRole_module` |
| `blc_ll_initAclConnSlaveTxFifo` | `blc_ll_initAclPeriphrTxFifo` |
| `blc_controller_check_appBufferInitialization` | `blc_contr_checkControllerInitialization` |

These were renamed "Slave" → "Peripheral" in newer SDK versions. The blob predates this rename.

### Extern declarations instead of SDK headers

The Telink BLE SDK headers (`tl_common.h`, `ble.h`, etc.) redefine `uint8_t`, `bool`, `ARRAY_SIZE`, and other types that conflict with Zephyr. Our shim declares all ~25 `blc_*` functions as `extern` with standard C types, avoiding the SDK headers entirely.

## Remaining Work

### Stage 4: Battery ADC (only incomplete stage)

Driver exists (`battery_b91_adc.c`), config enabled, BLE battery service registered. PD1 channel 0x0A via 1/2 resistor divider, calibration data at 0xFE0C0. **Voltage scaling needs hardware validation** — measure actual battery voltage vs. ADC reading to confirm the divider ratio and Vref. Linear SoC model (3300–4200 mV) is adequate for a keyboard.

### Upstream patches

GPIO patches 0001+0002 fix real bugs in Zephyr's B91 GPIO driver (`WRITE_BIT` double-BIT, multi-level IRQ truncation). These affect every B91 GPIO user and are good candidates for upstreaming. The `ZMK_USB_NO_VBUS_DETECT` zmk-src patch is a clean feature flag, also reasonable to propose upstream.

## Known Limitations

| Limitation | Root Cause | Impact |
|---|---|---|
| No BLE Privacy | Blob doesn't support `LE_Set_Random_Address` — hangs `bt_enable()` | Public MAC address exposed during advertising |
| 1M PHY only | Blob's 2M PHY loses packets → LL Response Timeout 0x22 after 40s | Slightly lower throughput (irrelevant for HID) |
| Single BLE connection | Blob is single-conn (`blc_ll_setMaxConnectionNumber(0, 1)`) | No multi-profile BLE |
| USB SRAM = 256 bytes | B91 hardware, 8-bit addressing only | Max 1 CDC ACM + HID |
| Cold boot wakeup (~1–2s) | Retention mode incompatible with MCUboot (boot ROM overwrites ILM) | Slower wake from deep sleep |
| No 2.4 GHz wireless | Would need dongle firmware + proprietary RF protocol | Original has 3 modes; we have USB + BLE |
| First BLE conn fails (0x3E) | Blob boot timing issue — second attempt always succeeds | Benign, 300ms delay on first connect |
| No image signing | MCUboot validates SHA-256 only, no cryptographic signature | Acceptable for consumer keyboard (no secrets on-device) |
| `west.yml` floats on `main` | ZMK and hal_telink not pinned | Future `west update` could introduce breaking changes |
