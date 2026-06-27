# Architecture

## Single-Chip Design

The Rainy 75 uses a **single MCU** on the main PCB that handles everything:
USB, Bluetooth LE, 2.4GHz RF, key matrix scanning, RGB LEDs, and VIA protocol.

Both sides of the main PCB have been inspected — only one IC exists.
The switch side (top) has only hotswap sockets and a wireless on/off slide switch (behind CapsLock).

### Main MCU

- Chip marking: **Wob logo + "N25SEP08"** (custom Wobkey branding hiding actual part number)
- Actual chip: **Telink TLSR9511** (B91-series, RISC-V) — confirmed from firmware string
- CPU core: **Andes D25F** (not N25F — D-prefix indicates DSP/SIMD P-extension support), RV32IMACF + Andes V5 extensions
- Pipeline: **5-stage in-order**, 32-entry BTB (branch target buffer) for dynamic branch prediction, I/D caches, I/D local memories
- Max clock: **96 MHz** (datasheet); actual firmware clock TBD (likely 48 MHz or 24 MHz based on PCLK observations)
- Andes extensions in use: **XAndesPerf** (BFOZ/BFOS, LEA, FFB, BBC/BBS, BEQC/BNEC) + **XAndesCoDense** (EXEC.IT)
- Evidence: `TLSR9511` USB string descriptor at firmware offset `0x01C070`, RISC-V opcodes, `usb_ota_telink` in OTA tool path
- Package: **QFN 7x7, 56-pin** (TLSR9511B), 24 MHz crystal oscillator, blue QC dot
- Located: top-right corner of PCB (bottom side), visible row traces RT1-RT6 nearby
- Memory: **1MB embedded flash** (Puya P25Q80U), **256KB SRAM** (128KB ILM + 128KB DLM)
- GPIO count: **40** (PA0-PA7, PB0-PB7, PC0-PC7, PD0-PD7, PE0-PE7)
- USB pins: **PA5** = DM, **PA6** = DP (internal 1.5K pull-up on DP)
- Debug: **PA7** = SWS (Single Wire Slave, default function)
- SonixQMK will NOT work (wrong architecture — RISC-V, not ARM Cortex-M0)

### Memory Architecture (from datasheet)

| Region | Size | Address (CPU) | Purpose |
|--------|------|---------------|---------|
| ILM (Instruction Local Memory) | 128 KB | `0x00000000` | Code + data, lower 64KB retained in deep sleep |
| DLM (Data Local Memory) | 128 KB | `0x00080000` | Data only (no instruction fetch) |
| Flash (Puya P25Q80U) | 1 MB | via MSPI | 4KB sectors, 32/64KB blocks, 256B page write, 100K erase cycles, 20-year retention |
| Unique ID | 128 bits | in flash | Factory-programmed chip UID, readable via SDK |

- ILM can store both instructions and data; DLM can **only** store data
- Our firmware (112KB) fits within the 128KB ILM instruction limit
- Firmware base address `0x20001000` maps into ILM address space
- Flash calibration data at `0xFE000`: RF cap value (0xDD@0xC0) + ADC Vref (0x8825@0xC4) — only 5 non-FF bytes
- BLE MAC at `0xFF000`: `XX:XX:XX:XX:XX:XX` (6 bytes + 2 marker bytes `B9 16`)
- Second address at `0xFF100`: `XX:XX:XX:XX:XX:XX` (possibly 2.4G dongle pairing address)
- BLE bonding data at `0xFA000-0xFCFFF` (3 × 4KB pages)

### Working Modes (from datasheet)

| Mode | MCU | Radio | USB | Retention SRAM | Wakeup Time | Current (typ) |
|------|-----|-------|-----|----------------|-------------|---------------|
| Active | active | available | available | full | — | — |
| Idle | stall | available | available | full | 0 µs (interrupt) | — |
| Suspend | stall | off | stall/off | full | ~100 µs | 43 µA |
| Deep Sleep (w/ retention) | off | off | off | 64KB retained | ~100 µs | 1.7-2.7 µA |
| Deep Sleep (no retention) | off | off | off | off | ~1 ms | 0.7 µA |
| Shutdown | off | off | off | off | ~10 ms (RESETB only) | — |

Wakeup sources: GPIO pad, 32K timer, low-power comparator, USB resume (suspend only), RESETB

**Retention analog registers** (afe_0x38-0x3f): survive deep sleep and watchdog/software reset. Used by firmware to track boot source and state across sleep cycles. afe_0x39-0x3f also survive software reset; afe_0x38 is cleared by watchdog/software reset.

### USB Identification

| Parameter | Value |
|-----------|-------|
| VID | `0x320F` (Shenzhen Evision Semiconductor) |
| PID | `0x5055` |
| Manufacturer | **"RDR"** (unknown ODM or firmware team) |
| Product | **"Rainy 75"** |
| bcdDevice | **1.01** (firmware version) |
| bCountryCode | **13** (ISO layout) |
| Speed | Full Speed (12 Mbps) |
| Power | Bus Powered, 500mA max |
| BLE names | `Rainy 75-1`, `Rainy 75-2`, `Rainy 75-3` |

### USB HID Interfaces

| Interface | SubClass/Protocol | Endpoints | Purpose |
|-----------|-------------------|-----------|---------|
| 0 | Boot/Keyboard | EP1 IN (8B) | Standard 6KRO boot keyboard |
| 1 | None/None | EP4 IN (32B) + EP6 OUT (32B) | VIA/RAW HID (keymap config) |
| 2 | Boot/Keyboard | EP2 IN (64B) | NKRO keyboard + vendor (0xFFEF) + mouse |
| 3 | Boot/Keyboard | EP3 IN (8B) + EP5 OUT (64B) | System/consumer keys + OTA update |

**Hardware endpoint capabilities** (from datasheet, base `0x80100800`):
- 9 endpoints total: EP0 (control) + EP1-EP8 (data)
- EP1, EP2, EP3, EP4, EP7, EP8 = configurable as IN (interrupt/bulk/iso)
- EP5, EP6 = configurable as OUT (interrupt/bulk/iso)
- EP6 = only endpoint supporting ISO OUT; EP7 = only endpoint supporting ISO IN
- EP7 and EP8 exist in hardware but are unused by the Rainy 75 firmware

### HID Report Descriptors

**Interface 0 — Boot Keyboard** (79 bytes):
- Standard 6KRO: 8-bit modifier keys + 1 reserved byte + 6 keycode array
- LED output: 5 LEDs (Num/Caps/Scroll/Compose/Kana)
- 64-byte Feature report on Consumer page (settings/config channel)

**Interface 1 — VIA/RAW HID** (34 bytes):
- Usage Page: `0xFF60` (standard VIA usage page)
- 32-byte IN (Usage `0x62`) + 32-byte OUT (Usage `0x63`)

**Interface 2 — NKRO + Multi-function** (193 bytes, 5 report IDs):
- Report ID 1: NKRO keyboard — 120-bit bitmap for keys `0x04`-`0x70`
- Report ID 2: System Control — Power Down/Sleep/Wake (3 bits)
- Report ID 3: Consumer Control — 16-bit usage code (media keys)
- Report ID 5: Vendor page `0xFFEF` — 63-byte bidirectional (OTA data channel)
- Report ID 6: Mouse — 5 buttons, X/Y (16-bit), wheel, AC pan

**Interface 3 — OTA/Vendor** (37 bytes):
- Usage Page: `0xFF1C`, Usage: `0x92`
- Report ID 4: 63-byte OUT + 7-byte IN (asymmetric — firmware OTA)

### Firmware Binary

| Property | Value |
|----------|-------|
| Location | `reverse/firmware/firmware_extracted.bin` (body) / `firmware_ota.bin` (with boot header) |
| Size | 115,364 bytes body + 4,784 byte boot header = 120,148 bytes total (117.3 KB) |
| Architecture | RISC-V (RV32IMAFDC + Andes D25F extensions) |
| String descriptors | `"Rainy 75"` (0x01C040), `"TLSR9511"` (0x01C070), `"RDR"` (0x01C0A0) |
| Instructions | 34,472 valid RISC-V + 4,656 bytes Andes custom (0x5B opcode) |
| Based on QMK? | No — fully proprietary |
| Developer | "YKQ" (developer-initials string found in the firmware) |

String descriptors at firmware offsets: USB device descriptor at `0x01BF38`, config descriptor at `0x01BFC4`.
Full HID report descriptors embedded starting at `0x01A0A5` (VIA), `0x01A60D` (OTA), `0x01A686` (Vendor 0xFFEF).

**Flash layout** (verified from SWS dump, `firmware_ota.bin` == flash 0x00000-0x1D553 byte-exact):
- `0x00000-0x0001F`: Boot vector (jump + size)
- `0x00020-0x012AF`: TLNK header + startup code (4,784 bytes)
- `0x012B0-0x1D553`: Application firmware (`firmware_extracted.bin`, 115,364 bytes)
- `0x82000-0x83FFF`: VIA config (device config + RGB settings)
- `0x84000-0x87FFF`: VIA keymaps (4 layers × 4KB)
- `0x89000-0x8AFFF`: VIA macro storage
- `0xFA000-0xFCFFF`: BLE bonding data (3 pages)
- `0xFE000`: Calibration (RF + ADC, 5 bytes)
- `0xFF000`: BLE MAC + second address
- Total used: ~173 KB (12%), ~850 KB free

### RGB LEDs

| Parameter | Value |
|-----------|-------|
| LED count | 83 (WS2812-style, addressable, per-key) |
| Data pin | **PB7** (PSPI MOSI IO0) |
| Interface | PSPI (APB bus) + DMA channel 4 |
| SPI clock | ~6 MHz (24 MHz PCLK / 4) |
| Encoding | 8 SPI bits per WS2812 bit (1.33 µs/bit) |
| Buffer size | 498 DMA words (1992 bytes) per frame |
| Effects | 19 modes (OFF, WAVE, COLOUR_CLOUD, VORTEX, etc.) |

The LEDs are driven via SPI+DMA — no CPU-intensive bit-bang. The PSPI module shifts out WS2812-encoded data from SRAM (`gp+0x14d8`) through the PSPI FIFO at 0x80140048 to PB7. DMA channel 4 handles the transfer autonomously.

### Daughter Board

- **NOT a wireless module** — just a USB breakout + battery connector
- USB-C port (USB1), ESD protection (ESD1)
- Battery connectors: 2x 3-pin JST (white/black/red wires)
- FPC ribbon cable connector (CON2) to main PCB — 6-pin
- PCB code: **8101-1010-0002**, date: 2025-04-09

### Battery & Charging

- 2x **QS4541113** LiPo cells, 3.7V, 3500mAh each (7000mAh total for Pro)
- 12.95Wh per cell, CCC certified
- 3-pin connectors (B+, B-, NTC thermistor)

**Built-in charger** (TLSR9511B hardware):
- Linear charger: VBUS (USB 5V) → VBAT (Li-ion 3.6-4.2V)
- Pre-charge (trickle): below 2.9V, current = 1/10 of CC (~8 mA)
- Constant current (CC): 2.9V-4.2V, default 80 mA (configurable via analog register 0x1a, 5 mA steps)
- Constant voltage (CV): at 4.2V, current decreases
- End of charge: when current < 1/10 CC (~8 mA)
- Recharge: triggers when VBAT drops below 4.05V with VBUS connected
- Safety limits: VBUS ≤ 5.5V, VBAT ≤ 4.35V, USB total current ≤ 120 mA, die temp ≤ 125°C
- Software-assisted mode: ADC samples battery voltage via 3/4 divider, manually steps down CC current for accurate 4.2V cutoff

### Hardware Specs

| Parameter | Value |
|-----------|-------|
| Layout | 75% ISO, **83 keys** |
| Dimensions | **317.5 × 139.7 × 37.6 mm** |
| Weight | **1.8–2.0 kg** |
| Typing angle | **7°** |
| Case | **CNC 6063 Aluminum** |
| Plate | FR4 (Pro), PP (Lite/Standard) |
| Mounting | Gasket mount with silicone poles |
| Sound dampening | PORON foam, IXPE, PET sheet |
| Switch orientation | **South-facing LEDs** |
| Switches (Pro) | **Kailh Cocoa Linear** (our unit) or JWK WOB |
| Switches (Lite/Standard) | HMX Violet Linear |
| Hot-swap sockets | Kailh |
| Keycaps | **Double-shot PBT, Cherry profile** |
| Connectivity | USB-C / Bluetooth 5.0 / 2.4GHz |
| USB cable | Braided, USB-C to USB-A, ~1.5m, detachable |
| Battery | 3500mAh (Lite/Standard) / 7000mAh (Pro) |
| Wireless signal | No slit in aluminum case — goes through non-metal plate |
| PCB code | **8101-1010-0004 V1**, date: 2025-04-17 |
| 2.4G dongle | Stored in magnetic slot on keyboard body |
| Warranty | 6 months |

### Connection Modes (from manual)

| Mode | Indicator | Switch via |
|------|-----------|------------|
| Wired (USB-C) | ESC steady | Fn+Tab (cycle to ESC) |
| Bluetooth 1 | F1 blue | Fn+Tab → Fn+F1 (long press = pair) |
| Bluetooth 2 | F2 blue | Fn+Tab → Fn+F2 (long press = pair) |
| Bluetooth 3 | F3 blue | Fn+Tab → Fn+F3 (long press = pair) |
| 2.4 GHz | F4 green | Fn+Tab → Fn+F4 (long press = pair) |

- Fn+Tab cycles connection modes: ESC (wired) → F1-F3 (BT) → F4 (2.4G)
- BT pairing names: "Rainy75-1", "Rainy75-2", "Rainy75-3"
- Pairing search timeout: 1 minute, then keyboard enters sleep mode
- Long-press Fn+F1/F2/F3/F4 extends pairing time
- When power switch is ON and USB cable disconnected, auto-switches to 2.4G mode

### Polling Rates & Latency

| Mode | Polling Rate | Latency |
|------|-------------|---------|
| Wired | 1000 Hz | 2 ms |
| 2.4 GHz | 1000 Hz | 3 ms |
| Bluetooth | 500 Hz | ~8 ms |

N-Key Rollover: supported on all keys, all modes.

**Ultra-Low Latency Mode** — Toggle: **Fn+H**
- RGB version feedback: Caps Lock flashes white 3x (activate) / 1x (deactivate)
- Non-RGB version feedback: ESC-F4 flash yellow 3x / 1x

### Battery Management

- **Check level**: Long-press Fn+Space — number keys 1-0 light up proportionally (10%-100%)
  - Color: <40% red, <80% yellow, >=80% green
  - Non-RGB version: ESC–F4 indicators light up (<40% red, 40–59% yellow, 60–100% green)
- **Charging indicator** (firmware v20240121+): via Fn+Space with updated animations (replaces old ESC red flash)
- **Fully charged**: ESC flashes green 5x slowly
- **Long Battery Life Mode**: **Fn+L** toggle (added in firmware v20240121)
- **Power switch**: Must be ON for wireless mode and charging
- Battery: 2x QS4541113 LiPo, 3.7V 3500mAh each (7000mAh total for Pro)

**Advertised battery life** (from Wobkey wiki):

| Variant | RGB Off | RGB On |
|---------|---------|--------|
| Lite/Standard (3500mAh) | ~200 hours | ~40 hours |
| Pro (7000mAh) | ~900 hours | ~80 hours |

**Charging**: approx. 3–4 hours to full. Recommended charger: 5V/1A–5V/2A. Avoid fast chargers (high voltage can damage battery/circuitry).

**Sleep/Wake behavior:**
- Auto-sleep after **1 minute** of inactivity (wireless modes)
- Wake: press any key
- Long-term storage: slide battery switch to OFF to prevent drain

**MCU power consumption** (from datasheet, VBAT=4.2V DCDC):
| State | Current |
|-------|---------|
| BLE RX | 5 mA |
| BLE TX @ 0 dBm | 5.2 mA |
| EDR TX @ 0 dBm | 12.5 mA |
| Suspend | 43 µA |
| Deep sleep (64KB retention) | 2.7 µA |
| Deep sleep (no retention) | 0.7 µA |

Battery voltage sampling: via SAR ADC (10-bit, PB0-PB7 = channels 0-7, PD0-PD1 = channels 8-9). Firmware likely uses ADC with 3/4 voltage divider for battery level indication.

### System Key Combos

| Combo | Function |
|-------|----------|
| Fn+M (long press 3s) | Toggle Windows / MAC mode |
| Fn+WIN | Lock/Unlock Windows key |
| Fn+ESC (long press 3s) | Factory reset |
| Fn+L | Toggle Long Battery Life Mode (added v20240121) |

**MAC mode F-key mapping** (WIN→Option, ALT→Command):

| Key | Mac Function (direct) | Windows Function (Fn+key) |
|-----|----------------------|--------------------------|
| F1 | Screen Brightness − | Customizable via VIA |
| F2 | Screen Brightness + | Customizable via VIA |
| F3 | Mission Control | Customizable via VIA |
| F4 | Apps / Launchpad | Customizable via VIA |
| F5 | F5 | Customizable via VIA |
| F6 | F6 | Customizable via VIA |
| F7 | Previous Track | Customizable via VIA |
| F8 | Play / Pause | Customizable via VIA |
| F9 | Next Track | Customizable via VIA |
| F10 | Mute | Mute |
| F11 | Volume Down | Volume Down |
| F12 | Volume Up | Volume Up |

### Companies Involved

| Entity | Role |
|--------|------|
| Wobkey / wobkey.com | International brand (→ KeebMonkey) |
| WOB / woblab.cn | Chinese domestic brand + driver portal |
| KeebMonkey / KBM Gadgets | Parent company (founded 2019, Shenzhen) |
| **Evision Semiconductor** (Shenzhen) | **Firmware platform provider**, VID holder (`0x320F`), chip rebrander |
| Telink Semiconductor (Shanghai) | MCU manufacturer (TLSR9511 B91 RISC-V) |
| RDR | Evision's generic USB manufacturer string (hardcoded in firmware template) |
| "YKQ" | Firmware developer initials (likely an Evision engineer; zero public presence) |
| KIBU Corporation | Japan distributor |

The firmware is Evision's proprietary keyboard platform, shared across 7+ keyboards from 5+ brands. See [evision-platform.md](evision-platform.md) for full details.
