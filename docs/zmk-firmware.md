# ZMK Firmware — Implementation

Custom ZMK firmware for the Wobkey Rainy 75 Pro ISO DE keyboard, targeting the Telink TLSR9511B (B91) SoC. BLE + USB HID.

## Status

| Component | Status | Notes |
|-----------|--------|-------|
| Build infrastructure | **Done** | west workspace, Zephyr module, CMake/Kconfig |
| Board definition | **Done** | HWMv2 format, DTS, keymap, defconfig |
| BLE HCI driver | **Done** | Zephyr v4.1 device-model API; open link layer by default (no blob), Telink blob opt-in (`--blob`) |
| USB DC driver | **Done** | Legacy `usb_dc.h` API; polled suspend + ISR resume, dead-bus reconnect recovery (`CONFIG_ZMK_USB_SUSPEND_REATTACH`) |
| RGB LED strip | **Done** | WS2812 via PSPI + DMA ch4, PB7 MOSI, 83 per-key LEDs, ZMK underglow enabled |
| Battery ADC sensor | **Done** | SAR ADC driver, PD1 channel 0x0A, 1/2 divider, BLE battery service |
| Deep sleep | **Done** | `sys_poweroff` → DEEPSLEEP_MODE (cold boot), 15min idle timeout |
| ZMK Studio | **Done** | Runtime keymap editing over BLE GATT (WebBluetooth) |
| MCUboot DFU | **Done** | mcumgr USB UART (primary) + BLE SMP (backup), swap-using-move, WDT crash revert |
| Watchdog | **Done** | B91 HW WDT driver, MCUboot image confirmation |

**Build output (all stages enabled, ISO, `./build.sh -p --iso`):**

| Region | Open controller (default) | Blob (`--blob`) | Total |
|--------|------|------|-------|
| ROM | 303474 B (66%) | 328120 B (72%) | 448 KB |
| RAM (DLM) | 105764 B (81%) | 85436 B (65%) | 128 KB |
| RAM (ILM) | 7726 B (6%) | 40288 B (31%) | 128 KB |

The open build's extra DLM is mostly its 251-octet queues for 3 links (about 5 KB per link) and the larger host ACL/ATT buffers.

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

The BLE controller is our open link layer (`zmk/drivers/bluetooth/openll/`, see [open-ble-controller.md](open-ble-controller.md)) by default. The alternative, opt-in with `./build.sh --blob`, is the Telink controller blob (`liblt_9518_zephyr.a`), a precompiled binary that implements the BLE link layer, HCI, and RF control. It is **proprietary** (confidential / non-transferable / NDA, see [NOTICE](../NOTICE)) and is **not committed** to this repo; `fetch_ble_blob.sh` downloads it (pinned commit + SHA-256 verified) from Telink's public repository at build time, only for `--blob` builds. Our shim (`b91_bt.c`) bridges it to Zephyr without including the SDK's conflicting headers. Deep sleep uses `z_sys_poweroff()` (cold boot via DEEPSLEEP_MODE 0x30) in `zmk/src/poweroff.c`.

## Workspace Layout

```
rainy75/                            # workspace root
├── zmk/                            # our Zephyr module (manifest repo)
│   ├── west.yml                    # manifest: fetches ZMK + hal_telink + mcuboot
│   ├── zephyr/module.yml           # registers as Zephyr module
│   ├── CMakeLists.txt              # top-level: includes drivers, links the blob (--blob only)
│   ├── Kconfig                     # top-level: rsource driver Kconfigs
│   ├── lib/
│   │   └── liblt_9518_zephyr.a     # BLE controller blob (2.8 MB, fetched for --blob, gitignored)
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
│       │   ├── b91_mac.c / .h      # MAC from flash, shared by blob and open controller
│       │   ├── openll/             # open link layer, blob-free (the default controller)
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
│   ├── openll.conf                 # open controller (default): counters, 251-octet buffers, CCC at boot
│   ├── blob.conf                   # Telink blob controller (./build.sh --blob)
│   ├── privacy.conf                # BT_PRIVACY, open controller only (./build.sh --privacy)
│   ├── ota-bridge.conf             # OTA bridge (./build.sh -b)
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
./build.sh -b          # OTA bridge only (build-bridge/, for install_zmk.sh)
```

BLE controller options (app builds):

| Option | Controller | Extra config |
|---|---|---|
| (default) | open link layer, no blob fetched or linked | `conf/openll.conf` |
| `--blob` | Telink blob, `fetch_ble_blob.sh` runs first | `conf/blob.conf` (no openll.conf) |
| `--privacy` | open link layer with a resolvable private address; every host must be paired again; refused with `--blob` | `conf/openll.conf` + `conf/privacy.conf` |
| `--openll` | accepted no-op alias (prints a note) | |

`grep -c liblt build/zephyr/zmk.map` gives 0 for the open controller and 48 for the blob.

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
  -DEXTRA_CONF_FILE="$(pwd)/conf/app.conf;$(pwd)/conf/openll.conf" \
  -DEXTRA_DTC_OVERLAY_FILE="$(pwd)/zmk/boards/rainy75/rainy75.keymap;$(pwd)/conf/mcumgr.overlay"
# (blob instead: conf/app.conf;conf/blob.conf, after ./fetch_ble_blob.sh)

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

Board defconfig (`rainy75_defconfig`) contains only hardware-essential configs (GPIO, flash, heap) shared by both MCUboot and the application. Application-specific configs (BLE, USB, ZMK, mcumgr, WDT, RGB) are in `conf/app.conf`, passed via `EXTRA_CONF_FILE` together with the controller overlay (`conf/openll.conf` or `conf/blob.conf`, plus `conf/privacy.conf` for `--privacy`). This allows MCUboot to build cleanly against the same board definition.

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

**Note:** `mcumgr image upload` alone does NOT trigger a swap. Must use `mcumgr image test <hash>` to mark the slot1 image as pending, then `mcumgr reset`. The first mcumgr command after a fresh boot used to time out (its response was dropped behind the boot log in the full CDC TX ring); fixed by zephyr patch 0011, see "Upstream Patches".

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
- **BT_PRIVACY hang (most likely not the blob):** `CONFIG_BT_PRIVACY=y` hung during startup, which was blamed on the blob lacking LE Set Random Address. The same hang later showed up with the open controller and turned out to be a ZMK deadlock (advertising started inside the settings commit while the host's IRK store waited for the settings lock), fixed by `patches/zmk-src/0005`. The blob was not retested with the patch, so whether it supports LE Set Random Address is unknown; `--privacy` stays limited to the open controller.
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
- `zmk/boards/rainy75/Kconfig.rainy75`: the board selects `HAS_POWEROFF` (no Zephyr patch needed)

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
`CONFIG_POWEROFF=y` is available because the board selects `HAS_POWEROFF`.

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

### Open controller (default)

The open-source link layer (`zmk/drivers/bluetooth/openll/`) replaces the blob behind the same `b91_bt.h` seam and is the default controller (`CONFIG_BT_HCI_B91_CTLR_OPEN=y`, `conf/openll.conf`). It does not link `liblt_9518_zephyr.a`. Features: legacy advertising (also while connected), up to 3 peripheral links (`CONFIG_BT_HCI_B91_OPENLL_MAX_CONN`), CSA #1 and #2, peripheral latency, responder LLCP incl. encryption, Data Length Extension to 251 octets, LE Ping, Connection Parameters Request (responder), 1M PHY only, opt-in privacy, deep sleep coordination and power counters over mcumgr group 66. Only a Linux/BlueZ central is tested so far. Architecture, measurements, build and flash steps, hardware findings and open items: [open-ble-controller.md](open-ble-controller.md). The sections below describe the blob shim (`./build.sh --blob`).

### BLE profiles with the open controller

The keymap binds three profiles (Fn+F1/F2/F3 = `&bt_sel_ble 0..2`, which also switches the output from USB to BLE, see [BLE policy module](#ble-policy-module); Fn+Del clears the active profile's bond). ZMK's host side keeps up to 5 connections and pairings; the open controller keeps up to 3 links, one per profile:

- A profile switch does not disconnect the old host. ZMK starts connectable advertising whenever the active profile is open or not connected, also while other profiles are connected; the controller advertises in the gaps between connection events (with the blob, too, but it kept only one connection).
- With the other hosts still connected, switching back is instant (no reconnect); HID reports go only to the active profile's host.
- With all 3 links taken, connectable advertising is refused (0x09, Connection Limit Exceeded).
- After a deep-sleep wake (cold boot) ZMK reconnects the active profile's host; the other hosts reconnect when their profile is selected (upstream ZMK behaviour).

Status: advertising while connected and the multilink controller are device-tested with one real central; the switch between several real hosts is not tested yet (open item, see [open-ble-controller.md](open-ble-controller.md#known-limitations-and-open-items)).

### ZMK Studio over USB and BLE

Studio runs over both transports, independent of the selected output (zmk-src patch 0008; upstream ZMK serves only the transport of the selected output). A request from another transport than the last one locks Studio first.

**USB:** a second CDC ACM port (`conf/studio-usb.overlay`, added to the app build by `build.sh`, chosen as `zmk,studio-rpc-uart`), USB interface 3, next to the console/mcumgr port on interface 0. Budget in the 256 B USB SRAM for EP1-7: per port 64 B bulk OUT (the hardware maximum packet size register applies to every OUT EP) + 32 B bulk IN + 16 B notify, two ports + 16 B HID = 240 B. The board `Kconfig.defconfig` sets `CDC_ACM_BULK_EP_MPS` to 32 when the Studio port exists (the symbol has no prompt); builds without it (MCUboot, bridge) keep 64. The second port uses EP6 (OUT, the only other OUT EP) and EP7 (IN). Those two reset into isochronous mode (USB register `0x38` resets to `0xC0`, meant for audio): without handshakes the first packets got through, then the port went quiet. The driver now writes the ISO mode register from the endpoint types (bulk and interrupt EPs non-isochronous), from a shadow cleared at attach. Measured: 300 of 300 Studio requests at about 300 per second (median 2.5 ms), with mcumgr echoes on port 0 at the same time; 20 of 20 clean enumerations. `99-rainy75-zmk.rules` names the ports "Rainy 75 Pro Console" / "Rainy 75 Pro Studio" for Chrome's Web Serial picker on Linux (`ID_MODEL_ENC` per interface).

**BLE:** With the open controller the ATT MTU is 247 (`CONFIG_BT_L2CAP_TX_MTU=247`, 251-octet ACL buffers, Data Length Extension), so a 244-byte RPC frame fits one PDU: 22.6 Studio RPCs per s instead of 10.3 with 27-octet PDUs and ATT MTU 65 (open controller before Data Length Extension; the blob build was not measured). "zmk_studio: Failed to select a transport!" at boot is normal: the BLE RPC transport is selected only once the BLE endpoint is ("Endpoint changed: BLE:0"); while the endpoint is USB, Studio over BLE pauses.

### Driver architecture

| File | Role | Lines |
|------|------|-------|
| `hci_b91.c` | Zephyr HCI device driver: `open`/`send`/`close` + HCI packet parsing | 270 |
| `b91_bt.c` | Shim: blob init, controller thread, IRQ handlers, FIFO management | 680 |
| `b91_mac.c` | MAC address from flash (`0xFF000`) with random static fallback, shared by both controllers | 60 |
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

The blob also defines `sys_init` (from its LTO'd `ext_pm.c.o`), which conflicts with hal_telink's `drivers/B91/sys.c`. We patch hal_telink's `CMakeLists.txt` to skip `sys.c` when the blob is selected (`CONFIG_BT_HCI_B91_CTLR_BLOB=y`), same as it already does for `CONFIG_BT_B91`. The open controller needs the HAL `sys.c`.

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
- Thread priority: `CONFIG_BT_HCI_B91_RX_PRIO` (default 2)

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

On layer 0 of both layouts, left GUI and left Alt are `&os_key LGUI` and `&os_key LALT` (positions 74 and 75; see OS key behavior below); right Alt stays `&kp RALT`.

### OS key behavior

`&os_key LGUI` / `&os_key LALT` (`zmk/src/behaviors/behavior_os_key.c`, binding `rainy,behavior-os-key`, node `os_key` with display name "OS Key" in `zmk/dts/rainy75_os_key.dtsi`, built with `CONFIG_RAINY75_OS_KEY`, which is on whenever the keymap includes that node) sends GUI or Alt as the runtime settings `kb.os` and `kb.gui_lock` say ([config-protocol.md](config-protocol.md)): with `mac` the two swap (Option and Command in the Mac order), then with the GUI lock a key that would send GUI sends nothing. The decisions are pure and host tested (`zmk/src/os_key/`, `tests/run_host_tests.sh`); the behavior raises the keycode they return with `raise_zmk_keycode_state_changed_from_encoded()`, as `&kp` does. The key sent on press is kept per key position (4 slots, 32 B) and released on release, so changing a setting while a key is held leaves nothing stuck. `config/cfg_table.c` pushes the settings with `os_key_set_mode()`; without `CONFIG_RAINY75_CONFIG` (OTA bridge) the keys are plain GUI and Alt. Studio metadata: one parameter with the values LGUI ("Win (Option on a Mac)") and LALT ("Alt (Command on a Mac)"); other parameters are refused with `-ENOTSUP`.

`kb.os_keys` (read-only) counts the key positions bound to `&os_key` on any layer in use, from ZMK's live keymap (`zmk_keymap_layer_index_to_id()`, `zmk_keymap_get_layer_binding_at_idx()`), on every read from the mcumgr thread, so it costs no RAM and follows unsaved Studio edits. It follows every keymap edit (a Studio set, save or discard, Restore Stock Settings) without a `rev` change, the one exception to "`rev` changes on every change" ([config-protocol.md](config-protocol.md)), so clients read `kb.os_keys` explicitly (on connect, when they show the Keyboard section or the warning, or periodically). Studio stores only the positions a user changed (`keymap/l/<layer>/<position>`), so a keymap saved before this behavior existed still gets `&os_key` on the two keys unless they were rebound; the count shows when they were.

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

### USB Remote Wakeup

The B91 can drive real resume signaling, so a keypress wakes a sleeping host without re-enumerating. `usb_dc_wakeup_request()` pulses the `WAKEUPEN` system register (`0x801401ee`, SC_BASE + 0x2e): the USB resume bit, then the USB-suspend wakeup source bit. This is the recipe Telink's public SDK uses in `usb_hardware_remote_wakeup()` (`tl_ble_sdk`, `drivers/B91/usbhw.c`). The datasheet lists "support remote wakeup" as a suspend-mode feature but documents no bit for driving it, and the MDEV wakeup-feature bit is read-only, so the SDK is the reference.

Enabled with `CONFIG_USB_DEVICE_REMOTE_WAKEUP=y` (`conf/app.conf`). Zephyr then sets the descriptor's remote-wakeup attribute, tracks the host's `SET_FEATURE(DEVICE_REMOTE_WAKEUP)`, and refuses `usb_wakeup_request()` with `-EACCES` until the host has armed it. ZMK asks for a wakeup from the HID send path and falls back to re-presenting the device only if the request is refused or a wakeup already driven did not resume the bus (`patches/zmk-src/0004`).

Verified against a sleeping Linux host, keypress as the only input:

```
SETUP SET_FEATURE wValue=1                 host arms remote wakeup as it sleeps
WAKE_REQ -> RESUME PULSE DRIVEN            keypress
STATUS SUSPEND
WAKE_REQ (susp-flag, susp-level) -> RESUME PULSE DRIVEN
STATUS RESUME                              host resumed the bus
SETUP CLEAR_FEATURE wValue=1               host disarms after waking
```

No `DETACH`, no `SET_CONFIGURATION`, and the kernel's device number is unchanged across the cycle — a true resume, not a re-presentation. Read this back with `reverse/tools/usb_diag.py` (ring event codes 18 `WAKE_REQ` and 19 `SUSP_POLL`).

**Do not gate the pulse on suspend detection.** The first `WAKE_REQ` above fired with neither the latched `SUSPEND_O` bit nor the MDEV suspend level set, about 400 ms before the driver's poll recognised the suspend; two other armed sleeps produced no suspend indication at all. The driver therefore pulses whenever it is attached and leaves the policy gate to Zephyr's `SET_FEATURE` tracking. Suspend *detection* was separately improved to consult the live MDEV suspend level (gated on CONFIGURED) instead of only the latched status bit, which is racy.

**Host-side arming (Linux).** All of these are required, and nothing wakes the machine if any is missing. The per-device setting resets on every replug and every DFU cycle:

```bash
echo enabled > /sys/bus/usb/devices/usb3/power/wakeup   # root hub (often disabled by default)
echo enabled > /sys/bus/usb/devices/3-1/power/wakeup    # the keyboard
grep XHCI /proc/acpi/wakeup                             # must be *enabled
```

`99-rainy75-zmk.rules` makes both persistent, so install it into `/etc/udev/rules.d/` rather than setting them by hand:

```
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="1d50", ATTR{idProduct}=="615e", ATTR{power/wakeup}="enabled"
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="1d6b", ATTR{power/wakeup}="enabled"
```

The second line covers the root hub (`1d6b` is the Linux Foundation root-hub ID). Arming a hub wakes nothing by itself: only a device that is itself armed can.

Runtime autosuspend (`power/control=auto`) is not a usable test vehicle here: the host never idles the port, most likely because of the CDC console's own traffic. Use a full system suspend. Note also that the keypress only reaches the USB path when the active ZMK endpoint is USB (`Fn+F4` toggles); over BLE no wakeup is requested.

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

With the open BLE controller (the default build), `z_sys_poweroff()` first calls `b91_bt_controller_poweroff()`, which quiesces the link layer scheduler and the radio (both interrupt sources off). Deep sleep and the 15 minute timeout work unchanged, including wake and automatic reconnect to the bonded host. See [open-ble-controller.md](open-ble-controller.md#power-management).

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

**Fast USB upload:** `reverse/tools/rainy75_dfu.py` speaks the same SMP image management as the mcumgr CLI, but writes the 127-byte serial lines back to back. The CLI waits about 20 ms after every line (pacing for real UARTs; its debug log shows the gaps), so each 295-byte request took about 100 ms: 313 KB in 85 s (3.6 KiB/s). Without the pauses, with 420 bytes per request: 12.7 s (24 KiB/s), the same with 295 to 460 bytes per request. The host CRC16 in C (`binascii.crc_hqx`, same CRC as the bitwise Python loop, which cost about 1.3 ms per request on a mostly idle host CPU) brings it to 11.7 s (26 KiB/s). The flash write is not the limit (page program and progressive sector erase are about 3.2 s of the upload): the firmware receive path is CPU bound at about 15 us per received byte, because the code runs in place from flash through an 8 KB I-cache. No firmware change (`UART_MCUMGR_RX_BUF_COUNT=2` keeps up; USB CDC ACM flow control holds the host back). Verified: slot 1 hash matches, test boot and revert, 8 of 8 uploads. `python3 reverse/tools/rainy75_dfu.py upload FILE --test --reset` is the one-shot update.

```bash
# 1. Build new firmware
./build.sh -p --iso

# 2. Upload via mcumgr (over USB serial; rainy75_dfu.py is about 6x faster)
mcumgr --conntype serial --connstring /dev/ttyACM0,baud=115200 \
    image upload build/zephyr/zmk.signed.bin

# 3. List images (verify upload)
mcumgr --conntype serial --connstring /dev/ttyACM0 image list

# 4. Reset (triggers swap)
mcumgr --conntype serial --connstring /dev/ttyACM0 reset
```

**One reader on the CDC port.** The log console and mcumgr share the one CDC ACM port. Close every other reader first (`cat`, a serial logger, a stuck script): a second reader takes SMP responses away from mcumgr, which then fails with NMP timeouts or stalled uploads. During the open controller work a leftover logger looked exactly like "mcumgr over USB is unreliable" until it was killed. Extra log traffic (e.g. `CONFIG_BT_HCI_B91_OPENLL_STATS_LOG`) also slows uploads; zephyr patches 0010 and 0011 make mcumgr robust against the log itself. Measured on the default image with a free port: 10 of 10 USB uploads OK, about 80 s each (3.6 KiB/s), `image list` right after each. Device reboots in the middle of a USB upload were seen two or three times before those patches (possibly misread stalls followed by a reset) and were not reproduced in about 45 uploads afterwards; the cause is unknown.

### Image confirmation flow

1. **MCUboot boots** — checks slot0 for pending swap, executes swap if needed, boots app
3. **Application starts** — `mcuboot_confirm_init` at APPLICATION/90 installs 10s WDT timeout
4. **USB + BLE init** — standard ZMK initialization at APPLICATION/99
5. **5s delayed work** — calls `boot_write_img_confirmed()` to make swap permanent, disables WDT
6. **If crash occurs** — WDT fires after 10s, chip resets, MCUboot sees unconfirmed image → reverts

**Risky test images:** `./build.sh ... --test-image` (adds `conf/test-image.conf` = `CONFIG_RAINY75_MCUBOOT_MANUAL_CONFIRM=y`, never in a release) skips step 5's confirmation and only disables the WDT; it also turns deep sleep off (`CONFIG_ZMK_SLEEP=n`), because waking from deep sleep is a cold boot that would revert the unconfirmed image. The image then runs as long as needed, and any reset or power cycle goes back to the previous image, even if USB and BLE no longer work (with USB unplugged, the wireless switch under CapsLock cuts the power). Confirm a good test image by hand with `mcumgr image confirm <hash>`. Do not upload another image while a test image runs unconfirmed: slot 1 holds the fallback; reset back to it first. Used for the USB Studio port bring-up.

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

All upstream modifications are tracked as `git format-patch` files in `patches/` and listed in `zmk/zephyr/patches.yml` for Zephyr's `west patch`. `build.sh` applies the missing ones before each build, so after `west update` they are re-applied automatically.

### Applying the patches

`zmk/zephyr/patches.yml` lists every patch with its sha256, target tree (`module`: the path relative to the workspace, e.g. `zephyr`, `bootloader/mcuboot`, `modules/hal/hal_telink`, `zmk-src`; not the west project name, because the ZMK project is named `zmk` like our module directory), author, date, an `upstreamable` flag and a comment. It sits where `west patch` looks by default (the manifest repository is `zmk/`); the patch files stay in `patches/`, so every call passes the patch base relative to `zmk/`:

```
west update && west patch -b ../patches apply    # manual flow: fresh trees, then all patches
west patch -b ../patches list
west patch -b ../patches clean                   # back to manifest-rev, see below
```

The apply-command is `git am --3way` with a neutral committer, so each patch becomes a commit with its subject. `west patch apply` applies every listed patch and is not idempotent: run it on trees fresh from `west update` (`git am --3way` happens to skip a patch whose change is already in the tree, but a stack that rewrites the same lines conflicts). `build.sh` therefore decides per tree itself:

- A patch counts as applied when a commit above `manifest-rev` has its subject (the old, known-good rule; `git apply --reverse --check` fails for an earlier patch once a later one rewrites its lines), and that commit must match the file (`git patch-id --stable`): a patch rewritten in place under the same subject is reported as outdated.
- All applied: nothing to do. Missing patches that are the tail of the series (none applied after `west update`, or new patches appended) go to `west patch apply` through a temporary copy of `patches.yml` with just those entries, so the sha256 check and the apply-command still come from `west patch`.
- A gap (a later patch is in the tree, an earlier one is missing or outdated) stops the build with the missing files and the fix: `git -C <tree> checkout --detach manifest-rev` (or `west update`), then build again.
- A failed apply runs `git am --abort` and stops the build. After `west patch apply` every handed-over patch is checked again, because `west patch` skips a patch whose module path does not resolve and `git am --3way` skips one whose change is already there, both without an error.
- Commits beyond `manifest-rev` that are not in `patches.yml` stop the build: a patch dropped from the series stays in a tree that has it. `RAINY75_ALLOW_EXTRA_COMMITS=1` turns this into a NOTE for own work in a tree.
- A patch file in `patches/` that `patches.yml` does not list stops the build.

`build.sh` never resets or cleans a tree, so uncommitted work there is never lost; a conflicting change makes `git am` fail, and the build stops. `west patch clean` runs `git checkout --detach manifest-rev` in each patched tree (`checkout-command` in `patches.yml`, `clean-command` empty): it drops the patch commits, leaves own commits behind (reflog) and refuses to overwrite conflicting uncommitted changes. The upstream defaults (`git checkout .`, `git clean -d -f -x`) would discard uncommitted work and keep the `git am` commits.

Adding or changing a patch: commit in the tree, `git format-patch -N` into `patches/<repo>/`, then add or update the entry in `patches.yml` (`sha256sum patches/<repo>/<file>`).

```
patches/
  zephyr/
    0001-gpio-b91-fix-WRITE_BIT-double-BIT.patch
    0002-gpio-b91-fix-interrupt-support.patch
    0003-flash-b91-report-erase-sectors.patch
    0004-usb-device-fix-transfer-slot-leak-on-cancel-resubmit.patch
    0005-usb-cdc_acm-retry-RX-transfer-restart-instead-of-dro.patch
    0006-usb-device-add-transfer-slot-diagnostic-snapshot.patch
    0007-usb-device-reclaim-transfer-slots-whose-completion-n.patch
    0008-usb-device-expose-a-transfer-slot-s-work-item-for-di.patch
    0009-usb-device-do-not-re-init-transfer-slots-on-every-us.patch
    0010-mgmt-uart_mcumgr-keep-log-output-out-of-SMP-frames.patch
    0011-mgmt-uart_mcumgr-optionally-wait-for-TX-room-instead.patch
    0012-usb-device-cdc_acm-set-the-call-management-data-inte.patch
  mcuboot/
    0001-b91-riscv-boot-fixes.patch
  hal_telink/
    0001-build-sys.c-unless-the-BLE-controller-blob-is-select.patch
  zmk-src/
    0001-zmk-usb-no-vbus-detect.patch
    0002-zmk-recover-a-dead-USB-bus-while-suspended-no-VBUS-d.patch
    0003-zmk-don-t-re-attach-USB-during-the-host-s-HID-bind-w.patch
    0004-zmk-drive-USB-remote-wakeup-from-the-HID-send-path.patch
    0005-zmk-start-BLE-advertising-from-the-workqueue-after-s.patch
    0006-zmk-raise-BLE-auth-state-events.patch
    0007-zmk-keep-passkey-entry-keys-out-of-the-HID-reports.patch
    0008-zmk-studio-serve-RPC-on-every-transport.patch
```

`zmk-src/0005` is needed for `--privacy`: ZMK started advertising inside the
settings commit (settings lock held), while the host had queued storing the
newly generated IRK on the system workqueue, which also transmits the HCI
commands. The two waited on each other until the HCI command timeout
asserted; the MCUboot test image then reverted, and the previous image went
on advertising the public address. This is most likely also what the old
"BT_PRIVACY hangs bt_enable() with the blob" note was (not retested with the
blob).

### Zephyr (12 patches)

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

**`drivers/console/uart_mcumgr.c` + `subsys/logging/backends/log_backend_uart.c`** (0010): log output never lands inside an SMP frame **[VERIFIED]**

The log console and mcumgr share the one CDC ACM port, and both write it with
`uart_poll_out()` byte by byte (log thread vs SMP work queue). A log message
written while a response frame was going out ended up inside the frame, the
host dropped the frame, and the request timed out. With the opt-in openll
stats log (7 lines every 2 s) that was one lost response every 4 s: 17..27
retries per image upload, 110..180 s instead of 80 s with the mcumgr CLI
(17..34 CLI request timeouts), and 2 of 11 CLI uploads stopped making progress
at a fixed offset until killed (a new mcumgr on the same port answered at once,
so the device side was alive). The patch writes a whole SMP frame under a mutex
that the UART log backend also takes around each message (not in panic or ISR
context). Measured on the stats-log image with the patch: 0..1 lost responses
per upload (the first request, which waits for the slot erase), CLI uploads
81..84 s, a minimal one-request-at-a-time SMP client 15 s instead of 85 s.
The patch assumes deferred logging (`CONFIG_LOG_MODE_DEFERRED`, as in this
build): in immediate mode the backend runs in the caller's context, possibly
with interrupts locked, where the mutex must not be taken. Upstreaming it
would need a skip of the lock in that case.

**`drivers/console/uart_mcumgr.c` + `Kconfig`** (0011): the first mcumgr command after boot is answered **[VERIFIED]**

`uart_poll_out()` on CDC ACM discards bytes while the 4 KB TX ring is full,
and the log fills it whenever no host reads the port (the boot log alone is
more than 4 KB). The first SMP response after the port was opened was
therefore dropped: the first `mcumgr` command after a boot (or after a quiet
period with log output and the port closed) failed with `NMP timeout`, the
next one worked. Measured: after a reset and 30 s, a fresh open read exactly
the 4096 backlog bytes and no response. `CONFIG_UART_MCUMGR_TX_WAIT_MS=500`
(conf/app.conf) writes responses with `uart_fifo_fill()` and waits up to
500 ms per frame for the host to drain the ring; each frame also starts with a
newline, because the backlog before it ends mid-line. Measured: the response
follows the 4096 backlog bytes on a line of its own, 3 of 3; the first
`mcumgr image list` after a swap boot answers. The ota-bridge build
(conf/ota-bridge.conf) does not set the option.

The two patches work together: 0011's frame write runs under 0010's mutex, so
a waiting response cannot be interleaved with a log message either. Neither
helps against a second program reading the port (see "MCUboot DFU").

**`subsys/usb/device/class/cdc_acm.c`** (0012): the Call Management descriptor of a second CDC ACM port points at its own data interface. The legacy class renumbers a CDC ACM function that is not first in the configuration (interface numbers, union descriptor, IAD) but left `bDataInterface` at its static 1, so the Studio port (interfaces 3/4) pointed at data interface 1. Linux uses the union descriptor and did not notice; hosts that read Call Management would get the wrong interface. Upstreamable.

### MCUboot (1 file, 1 patch)

**`boot/zephyr/main.c`** — B91 RISC-V boot fixes **[VERIFIED]**

- **do_boot XIP fix**: upstream groups RISC-V with Xtensa, copying image to SRAM `0xBE030000`. B91 boots from flash (XIP), `0xBE030000` is invalid. Without fix: no USB enumeration. *Note: upstreamed in MCUboot `main` (commit `2750a58c`, March 2026) but not yet in any release.*
- **boot_console_init early**: moved before DFU wait guard — USB init side effects (clock/DMA) needed by flash controller for image hash verification.
- **fence.i**: flush instruction cache before jumping to app (generic RISC-V, guarded by `CONFIG_RISCV`).

### hal_telink (1 file, 2 patches)

**`tlsr9/CMakeLists.txt`** (0001): build `sys.c` unless the BLE controller blob is selected

```diff
-if (NOT CONFIG_PM AND NOT CONFIG_BT_B91)
+if (NOT CONFIG_PM AND NOT CONFIG_BT_B91 AND NOT CONFIG_BT_HCI_B91_CTLR_BLOB)
```

The blob defines its own `sys_init()`, which collides with hal_telink's `sys.c`; the open controller needs the HAL's `sys.c`. hal_telink is pinned in `zmk/west.yml` to the commit this patch is made against.

### zmk-src (14 files, 8 patches)

**0001 — `app/Kconfig` + `app/src/activity.c`** — `ZMK_USB_NO_VBUS_DETECT` for boards without VBUS sensing

B91 has no USB VBUS detection pin. Without this patch, `is_usb_power_present()` returns true (USB status stays at SUSPEND after unplug), preventing deep sleep from ever triggering. When `CONFIG_ZMK_USB_NO_VBUS_DETECT=y`, `is_usb_power_present()` always returns false, allowing the idle sleep timeout to work.

**0002 — `app/src/usb.c`** — dead-bus heartbeat: re-present the device when the bus is provably dead while suspended (no VBUS detect means cable removal is invisible, so a returning host's bus reset can be missed).

**0003 — `app/src/usb.c`** — bind-window grace: failed HID sends within 5 s of a completed enumeration no longer count toward the starvation verdict, so an ordinary wake does not trigger a second, needless re-attach. From PR #20.

**0004, `app/src/usb_hid.c` + `app/src/usb.c`:** ask for USB remote wakeup from the HID send path, falling back to re-presenting only when the request is refused or a driven wakeup did not resume the bus. See [USB Remote Wakeup](#usb-remote-wakeup). The patch changed `zmk_usb_user_activity_while_suspended()` to return `int`; its `#else` stub (used when `CONFIG_ZMK_USB_SUSPEND_REATTACH` is off, i.e. the OTA bridge and therefore `./build.sh -a` / `-b`) still returned `void` and broke those builds until it was fixed to return `-ENOTSUP` (hotfix PR #35 on main).

**0005, `app/src/ble.c`:** `zmk_ble_ready()` submits `update_advertising_work` instead of starting advertising inside the settings commit, so it runs after the host's pending IRK/identity stores. Fixes the `CONFIG_BT_PRIVACY` startup deadlock described above.

**0006, `app/src/ble.c` + new `app/include/zmk/events/ble_auth_state_changed.h`, `app/src/events/ble_auth_state_changed.c`, `app/CMakeLists.txt`:** a new event `zmk_ble_auth_state_changed { profile, state, digits }` for pairing indicators (rainy_rgb BLE slot LEDs). States: `PASSKEY_REQ` (`auth_passkey_entry`), `PASSKEY_DIGITS` (each digit typed, `digits` = count so far, 1..6; `PASSKEY_REQ` means 0; the profile is the one stored at `PASSKEY_REQ`), `PASSKEY_SUBMITTED` (Enter, queued before the passkey goes to the stack so the result always follows it), `PAIRED_OK` (`auth_pairing_complete`), `FAILED` (`auth_cancel`, `security_changed` with an error, `pairing_failed`, or a pairing completed on a taken profile) and `CLEARED` (`zmk_ble_clear_bonds()`, and every profile in `zmk_ble_clear_all_bonds()`). `profile` is the bonded profile of the peer, else the active profile, where new pairings happen. The BT callbacks run in the BT RX thread, so every event goes through a small message queue (8 entries) drained by a work item on the system workqueue; the thread-context sources take the same path so the order is kept. One failed pairing usually produces two or three `FAILED` events within the same RX callback chain (Zephyr calls `security_changed`, `pairing_failed` and sometimes `cancel`); consumers treat them as one. After `BT_CLR` the old host usually still tries to reconnect with its stale keys, which gives `CLEARED` and then `FAILED` on the same slot (a red flash); this is expected. Only peripheral-role connections raise events. The patch only reports: it changes no ZMK behaviour. The policy built on it (output switch on profile select, open profile timeout) lives in our module, see [BLE policy module](#ble-policy-module).

**0007, `app/src/hid_listener.c` + `app/src/ble.c` + `app/include/zmk/ble.h`:** keys typed for a passkey no longer reach a host. ZMK event listeners run in link order (the `.event_subscription` linker section is not sorted), and `hid_listener.c` is linked before `ble.c`, so upstream ZMK reported every passkey key to the current endpoint before the passkey listener consumed it. On the device the digits appeared on the USB host, because the endpoint falls back to USB while the new BLE profile is not connected yet. `zmk_ble_passkey_entry_active()` (true while `auth_passkey_entry_conn` or `auth_pairing_keys_conn` is set) is checked at the top of the HID listener: while a passkey is entered it drops presses and drops releases of keys that are not in the report; releases of keys held from before the request still go out, so nothing gets stuck. The check does not depend on listener order. The ownership lasts beyond the Enter release: `auth_passkey_entry_conn` is cleared there, but the host still checks the passkey, and a second Enter typed meanwhile reached the PC. A separate reference (`auth_pairing_keys_conn`, atomic) is taken at the passkey request and released on `pairing_complete`, `pairing_failed`, `security_changed` with an error, `cancel` or the disconnect of that connection. `auth_passkey_entry_conn` is atomic too and is cleared on all the same paths: Zephyr calls the `cancel` callback only for a remote Pairing Failed, while Esc (a local `bt_conn_auth_cancel()`), the SMP timeout and a disconnect before Enter only reach `pairing_failed`/`security_changed`. Upstream left it set on those paths; with the check above that kept every key away from the hosts until the next pairing. Enter and Esc take it with an atomic exchange, so it is never dropped twice. A disconnect that ends a pairing nothing else ended raises `FAILED`. A digit raises `PASSKEY_DIGITS` only while the entry is still open, so a digit typed while the RX thread ends the pairing does not restart the guidance after its `FAILED`. Corner case kept: a usage held from before the request and pressed again on another key during the entry is released early by that key's release.

**0008, `app/src/studio/rpc.c` + `uart_rpc_transport.c` + `gatt_rpc_transport.c` + `app/include/zmk/studio/rpc.h`:** ZMK Studio on every transport. Upstream served Studio only on the transport of the selected output (USB output: USB, BLE output: BLE) and locked it on every output change, so with the cable plugged in a Bluetooth client got no answer. Every transport now listens all the time. A transport claims the shared RX buffer (`zmk_rpc_rx_claim()`) before writing request bytes into it, so two clients never interleave; another transport can take over once the owner has been quiet for 200 ms (`RX_CLAIM_IDLE_MS`), and a request that arrives inside that window is dropped (the client retries). Responses and notifications go out over the transport of the last request; the USB transport only drains the shared TX buffer while it is selected. With `CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT` a change of transport locks Studio first, so a client on another transport never inherits an unlock. Measured: BLE 234/234, USB 49/50 right after (the first request inside the 200 ms window), BLE 113/113, output untouched. Upstreamable.

**Updating an older zmk-src tree.** Until October 2026 the series had 8 patches: 0006 also switched the output in `zmk_ble_prof_select()` and 0008 carried the open profile timeout. A tree that still has those commits lacks the new 0006 subject while 0007 is applied, so `build.sh` stops with the gap message (see [Applying the patches](#applying-the-patches)). Move the tree back to the manifest revision (`git -C zmk-src checkout --detach manifest-rev`, or `west update`) and run `./build.sh` again, which applies the series.

### BLE policy module

Policy that used to live in zmk-src patches 0006 (output switch) and 0008 (open profile timeout) is part of our module and uses only public ZMK and Zephyr interfaces: ZMK events (`zmk_ble_active_profile_changed`, `zmk_ble_auth_state_changed` from patch 0006), the `zmk_ble_*` functions in `zmk/ble.h`, `zmk/endpoints.h`, and `BT_CONN_CB_DEFINE`. Built with `CONFIG_ZMK_BLE` (`zmk/CMakeLists.txt`, library `rainy75_ble`).

**`&bt_sel_ble N`** (`zmk/src/behaviors/behavior_bt_sel_ble.c`, binding `rainy,behavior-bt-sel-ble`, node in `zmk/dts/rainy75_ble.dtsi`): `&bt BT_SEL N` plus "type over BLE". It calls `zmk_ble_prof_select(N)` first and then sets the preferred transport to BLE if it was USB, so the output never flips to the old profile; when N is already active the key only switches the output. The keymap uses it on Fn+F1..F3; `&bt BT_CLR` and the other `&bt` commands are unchanged and do not touch the output. Connection events and the open profile timeout's return call `zmk_ble_prof_select()` directly and keep the output as it is. Studio metadata: one parameter, profile 0..`ZMK_BLE_PROFILE_COUNT - 1`.

**Open profile timeout** (`zmk/src/ble_open_profile/`: `open_profile.c/.h` pure decisions, host tests in `tests/run_host_tests.sh`; `open_profile_timeout.c` Zephyr adapter), `CONFIG_RAINY75_BLE_OPEN_PROFILE_TIMEOUT` (seconds, default 30, range 0..3600, 0 compiles it out; was `CONFIG_ZMK_BLE_OPEN_PROFILE_TIMEOUT` in patch 0008). When a profile change makes an open profile active and no host pairs within the timeout, the keyboard selects the previously active profile again if it is connected, else the most recently connected profile (RAM only: set on a bonded host's connect and on a completed pairing, forgotten when its bond is cleared), else it stays. It is only armed when there is a profile to return to. A pairing in progress pauses the timer; when it fails the full timeout starts again. A plain connection does not pause it, so a host that stays connected after a failed pairing, or one with stale keys that keeps reconnecting, cannot hold the open profile forever. A completed pairing, selecting a bonded profile, `BT_CLR` (the user wants to pair on the active slot) and `BT_CLR_ALL` disarm it. The timer is RAM-only: it is not re-armed after a reboot or deep sleep wake, and selecting the already active open profile again does not restart it (ZMK raises no event for it). The return keeps the output transport.

Signals, all from public interfaces:

| Rule | Signal |
|---|---|
| explicit select of a profile | `zmk_ble_active_profile_changed` with a new index (raised synchronously inside `zmk_ble_prof_select()`); events with the known index come from connection changes and are ignored |
| the timeout's own return | the same event, to a bonded profile, which disarms like any other selection; no marker needed |
| previously active profile | the module's last seen index (seeded from `zmk_ble_active_profile_index()` at the settings commit) |
| most recently connected | own `BT_CONN_CB_DEFINE` `connected` (bonded peer, peripheral role) and `PAIRED_OK` |
| pairing start (pause) | `ZMK_BLE_AUTH_PASSKEY_REQ` |
| pairing failed (restart) | `ZMK_BLE_AUTH_FAILED` while paused (pairing failed, cancel, security failure, pairing on a taken profile, disconnect during the pairing per patch 0007) |
| pairing complete (disarm) | `ZMK_BLE_AUTH_PAIRED_OK` |
| bonds cleared (disarm) | `ZMK_BLE_AUTH_CLEARED` |

Pairing start: Zephyr has no "pairing started" callback other than `pairing_accept`, which belongs to the single `bt_conn_auth_cb` ZMK registers, so the pause starts at the passkey request. ZMK selects `BT_SMP_SC_PAIR_ONLY`, Zephyr's `BT_SMP_ENFORCE_MITM` (default y) sets MITM in our response, and with `CONFIG_ZMK_BLE_PASSKEY_ENTRY` the keyboard is KeyboardOnly, so every host with a display or keyboard gets Passkey Entry. Differences to patch 0008, which paused from the accepted pairing request: the feature exchange and key generation before the passkey request (well under a second) are not paused, and a NoInputNoOutput host, or a build without `CONFIG_ZMK_BLE_PASSKEY_ENTRY`, pairs Just Works without a pause (about a second; if the timeout hits it, ZMK drops the new bond as a pairing on a taken profile and raises `FAILED`, as for any pairing that completes after the user left the open profile). The pairing end comes from the same ordered auth event queue as its start; Zephyr's auth info callbacks would run in the BT RX thread, and an end seen before its queued start would leave the timer paused for good. A pause ends only through these events (`FAILED` of any connection); 0008 also ended it on the disconnect of any host.

Before switching back, the module raises its own event `rainy75_ble_open_profile_timeout { profile, target }` (`zmk/include/rainy75/events/ble_open_profile_timeout.h`), synchronously before the profile change; rainy_rgb shows it like `FAILED` on the open slot (was the auth state `ZMK_BLE_AUTH_PAIRING_TIMEOUT` in patch 0008).

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

### Stage 4: Battery ADC (done)

Complete, see the Stage 4 checklist above. Linear SoC model (3300 to 4200 mV) is adequate for a keyboard. The open controller's power counters (mcumgr group 66) also report the battery millivolts.

### Upstream patches

GPIO patches 0001+0002 fix real bugs in Zephyr's B91 GPIO driver (`WRITE_BIT` double-BIT, multi-level IRQ truncation). These affect every B91 GPIO user and are good candidates for upstreaming. The `ZMK_USB_NO_VBUS_DETECT` zmk-src patch is a clean feature flag, also reasonable to propose upstream.

## Known Limitations

| Limitation | Root Cause | Impact |
|---|---|---|
| BLE privacy opt-in only | Default builds keep the public identity address (bonds survive); `--privacy` (open controller) needs every host to pair again | Public MAC address visible during advertising by default |
| 1M PHY only | Open controller: no open 2M register source exists. Blob: its 2M PHY loses packets, LL Response Timeout 0x22 after 40 s | Slightly lower throughput (irrelevant for HID) |
| BLE links | Open controller: up to 3 links (one per profile). Blob: single connection (`blc_ll_setMaxConnectionNumber(0, 1)`) | Blob: only one profile connected at a time |
| USB SRAM = 256 bytes | B91 hardware, 8-bit addressing only | Two CDC ACM ports + HID only with 32 B bulk IN endpoints (240 B used) |
| Cold boot wakeup (~1–2s) | Retention mode incompatible with MCUboot (boot ROM overwrites ILM) | Slower wake from deep sleep |
| No 2.4 GHz wireless | Would need dongle firmware + proprietary RF protocol | Original has 3 modes; we have USB + BLE |
| First BLE conn fails (0x3E) | Blob only: boot timing issue, the second attempt always succeeds | Benign, 300ms delay on first connect |
| No image signing | MCUboot validates SHA-256 only, no cryptographic signature | Acceptable for consumer keyboard (no secrets on-device) |
| `west.yml` floats on `main` | ZMK and hal_telink not pinned | Future `west update` could introduce breaking changes |
