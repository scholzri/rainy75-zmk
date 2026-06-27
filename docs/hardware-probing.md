# Hardware Probing

## Logic Analyzer Setup

AZDelivery Logic Analyzer (Cypress CY7C68013A FX2LP, Saleae clone).
- Driver: `fx2lafw` (open-source firmware, uploaded on plug-in)
- Software: `sigrok-cli` 0.7.2 + `libsigrok` 0.5.2 (Arch Linux)
- 8 channels, max 24 MHz sample rate, 5.25V max input, VIH=1.4V / VIL=0.8V
- USB ID: `0925:3881`
- Scan: `/usr/bin/sigrok-cli --scan` (detects as "Saleae Logic")
- Unconnected channels float HIGH (FX2 internal pull-ups)

## Test Pads Near MCU (3 copper pads, bottom side of main PCB)

| Pad | Signal | Evidence |
|-----|--------|----------|
| 1 | **GND** | Constant LOW regardless of power source |
| 2 | **VCC (3.3V)** | Constant HIGH regardless of power source |
| 3 | **SWS** | Sleep/wake pattern on battery; constant HIGH on USB |

### SWS Identification

- On battery power (no USB): alternating ~80ms LOW (sleeping) / ~275ms HIGH (active) at ~2.8 Hz
- On USB power (steady state): constant HIGH (MCU always awake, no sleep cycling)
- On USB cold boot (plug-in): 2 brief deep-sleep cycles before USB enumeration settles (see below)
- This sleep/wake behavior is characteristic of Telink SWS (Single Wire Slave) debug interface

### SWS Boot Capture (USB cold boot, 24 MHz, 2026-02-19)

Captured via `sigrok-cli --driver fx2lafw --config samplerate=24M --time 5s` during USB plug-in (no battery).

| Time (ms) | SWS (D0) | Duration | Phase |
|-----------|----------|----------|-------|
| 0 | HIGH | 799 ms | Boot ROM + firmware init |
| 799 | LOW | 76 ms | 1st deep-sleep cycle |
| 875 | HIGH | 241 ms | Active (scanning/init) |
| 1116 | LOW | 51 ms | 2nd deep-sleep cycle |
| 1167 | HIGH | permanent | USB enumerated, always awake |

VCC (D1) was 100% HIGH throughout — no glitches or dropouts on the power rail.

The firmware does 2 sleep/wake cycles during early init even on USB-only power, before USB enumeration locks it into always-active mode. After ~1.2s from power-on, SWS stays permanently HIGH.

**SWS specs from datasheet (Section 9.2):**
- Pin: **PA7** (default SWS function, func_mux register 0x140331[7:6])
- Max data rate: **2 Mbps**
- Base register address: `0x80100c00`
- **NOT available in deep sleep or suspend mode** — explains why SWS goes LOW during sleep phases (MCU powers down the SWS interface, PA7 floats to its pull-up/pull-down state)
- SWS registers: data (0x00), control (0x01), clock divider (0x02), ID (0x03)

## Test Pads Near CON1 (6 pads, FPC connector area)

| Contact | Signal | Evidence |
|---------|--------|----------|
| 1 | **GND** | Constant LOW |
| 2 | **VBUS (5V)** | Constant HIGH |
| 3 | **USB D+** | ~1ms period (USB SOF), fast data bursts |
| 4 | **USB D-** | ~1ms period (USB SOF), complementary to D+ |
| 5 | **VCC** | Constant HIGH |
| 6 | **VCC** | Constant HIGH |

FPC passthrough signals (USB + power from daughter board). Not useful for firmware work.
12 MHz LA is insufficient to decode USB Full Speed (12 Mbps) — would need 48+ MHz.

## Other Test Points

- **MK2** (labeled pad): constant HIGH — factory test point
- **Corner pads** (copper circle + ring, bottom-left and top-right): constant HIGH — likely antenna or power test points
- **No rotary encoder** on this model (Rainy 75 Pro ISO DE) — Row 7 VIA entries may be Fn-layer combos

## MCU Sleep/Wake Behavior (battery power)

| State | Duration | SWS Level | Notes |
|-------|----------|-----------|-------|
| Active | ~275 ms | HIGH | Scanning matrix, handling BLE |
| Sleep | ~80 ms | LOW | Deep sleep, SWS released |
| Cycle | ~355 ms | — | ~2.8 Hz wake rate |

On USB (steady state): no sleep cycling, MCU continuously active.
On USB (cold boot): 2 brief sleep cycles (~76ms + ~51ms) in first ~1.2s, then always active.

## SWS Programming (Firmware Dump)

### Required Hardware: Telink Burning EVK (TLSRGSOCBK56B)

- Only confirmed SWS programmer for B91 series — no open-source alternative exists
- USB ID: `248a:826a` ("Telink Web Debugger"), contains TLSR8266 bridge chip
- Wiring: SWM -> SWS pad (pad 3), GND -> GND pad (pad 1), 3.3V -> VCC pad (pad 2) — 3 wires only
- Connection verified via logic analyzer capture (2026-02-19): clean signals, no contact noise
- ~22 EUR from Mouser (mouser.de) — only reliable Western distributor
- Also on AliExpress (search "Telink Burning Board TLSRGSOCBK56B") — volatile stock
- Alternative: B91 Starter Kit (TLSR9518ADK80D-KIT, ~100-150 EUR)

### Software: Telink BDT (Burning and Debugging Tool) for Linux

- Latest: v2.2.1, native x64 CLI
- Docs: https://wiki.telink-semi.cn/wiki/IDE-and-Tools/BDT_for_TLSR9_Series_in_Linux/
- Also available as WEB BDT (Chrome/Chromium WebUSB)
- Requires EVK firmware V4.7+ for BDT v2.2.0+
- May need udev rules for USB ID `248a:826a`

```bash
sudo ./bdt 9518 ac                          # Activate chip (wake from deep sleep)
sudo ./bdt 9518 rf 0 0x100000 -o dump.bin   # Read 1MB flash
sudo ./bdt 9518 wf 0 -i firmware.bin        # Write flash
```

### Gotchas

- Deep sleep blocks SWS — use BDT `ac` command to wake chip first
- Keyboard wireless switch should be OFF during programming
- TLSR9511 has 1MB embedded flash, 256KB SRAM
- Flash read protection unlikely on consumer keyboard but possible (AES-128)
- Try both `9518` and `9511` chip selectors if one doesn't work
- No open-source SWS tools for B91 (pvvx/TLSRPGM, telinkdebugger, TlsrComProg are all TLSR82xx)
- FTDI FT232H/FT2232H cannot bit-bang SWS protocol
- RPi Pico `telinkdebugger` only supports TLSR8232

### Important

- Preserve calibration data at 0xFE000 (only 5 non-FF bytes: RF cap=0xDD@0xC0, ADC Vref=0x8825@0xC4) and MAC at 0xFF000 (`XX:XX:XX:XX:XX:XX`). Backup: `reverse/firmware/calibration_0xFE000.bin` (8KB)
- Second address at 0xFF100 (`XX:XX:XX:XX:XX:XX`) — possibly 2.4G dongle pairing address, also preserve
- BLE bonding data at 0xFA000-0xFCFFF (3 pages with crypto keys + connection params)
- Bricking risk near-zero with SWS access — works even with completely erased flash
- JTAG is NOT available (PE4-PE7 reused as matrix columns)
