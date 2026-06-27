# GPIO Pin Mapping & Matrix Scan

## Telink B91 GPIO Register Layout

Verified from SDK + boot assembly + datasheet (DS-TLSR9511B-E3 v1.1.0):

- Base: `0x80140300`, 8-byte stride per port (PA=0x300, PB=0x308, PC=0x310, PD=0x318, PE=0x320)
- Per-port: +0=input, +1=IE (input enable, high active), +2=OEN (output enable, **low** active: 0=output, 1=high-Z), +3=output, +4=polarity, +5=DS (drive strength), +6=act-as-GPIO, +7=IRQ
- Function mux: PA=0x140330-0x140331, PB=0x140332-0x140333, PC=0x140334-0x140335, PD=0x140336-0x140337, PE=0x140350-0x140351
- Total GPIOs: **40** (PA0-PA7, PB0-PB7, PC0-PC7, PD0-PD7, PE0-PE7) — PF0-PF5 are MSPI flash pins (not available as GPIO)

## Column Pins (16 output pins — active-LOW during scan)

| Col | GPIO | Port.Bit | SDK Pin |
|-----|------|----------|---------|
| 0 | PE4 | PE.4 | 0x410 |
| 1 | PE5 | PE.5 | 0x420 |
| 2 | PE6 | PE.6 | 0x440 |
| 3 | PE7 | PE.7 | 0x480 |
| 4 | PA0 | PA.0 | 0x001 |
| 5 | PA1 | PA.1 | 0x002 |
| 6 | PA2 | PA.2 | 0x004 |
| 7 | PA3 | PA.3 | 0x008 |
| 8 | PA4 | PA.4 | 0x010 |
| 9 | PB1 | PB.1 | 0x102 |
| 10 | PB2 | PB.2 | 0x104 |
| 11 | PB3 | PB.3 | 0x108 |
| 12 | PB4 | PB.4 | 0x110 |
| 13 | PB5 | PB.5 | 0x120 |
| 14 | PB6 | PB.6 | 0x140 |
| 15 | PC1 | PC.1 | 0x202 |

All 16 column pins get pull-up type `11` = **10K pull-up** (via analog registers afe_0x0e-0x17) during init.
During idle, all columns driven LOW. During scan, one column HIGH at a time.

## Row Pins (7 input pins — active-LOW with pull-ups)

| Row | GPIO | Port.Bit | SDK Pin | VIA Row Content |
|-----|------|----------|---------|-----------------|
| 0 | PE0 | PE.0 | 0x401 | ESC, F1-F12, DEL, HOME |
| 1 | PD2 | PD.2 | 0x304 | \`, 1-0, -, =, BKSP, END |
| 2 | PD3 | PD.3 | 0x308 | TAB, Q-], ENTER |
| 3 | PD4 | PD.4 | 0x310 | CAPS, A-#, PGUP, PGDN |
| 4 | PD5 | PD.5 | 0x320 | LSFT, <>, Z-/, RSFT, UP |
| 5 | PD6 | PD.6 | 0x340 | LCTL, LGUI, LALT, SPC, RALT, MO(1), RCTL, arrows |
| 6 | PE3 | PE.3 | 0x408 | (empty — unused matrix positions) |

Row 7 (Vol+, Mute) handled separately — not through main matrix scan.
The function at `0x2000F584` (previously misnamed `encoder_input_read`) is actually `consumer_report_send` — it sends 3-byte consumer control HID reports to USB endpoint 2 with 2ms delays. It doesn't read GPIO; the actual encoder/switch GPIO polling happens in the main loop, which populates the report buffer before calling this sender.
On this model (Rainy 75 Pro ISO DE) there is no rotary encoder — Row 7 keycodes (0xA9=Mute, 0xAA=Vol+) are read from dedicated GPIO (PC7/PC0 candidates).
Wireless switch under CapsLock does NOT map to Row 7 (confirmed: VIA cmd 0x14 shows no change when switch is toggled).
All row pins get pull-up type `01` = **1M pull-up** (via analog registers afe_0x0e-0x17). PD2-PD6 OEN bits set to 1 (high-Z/input) during scan.

### Pull-up/Pull-down Resistor Encoding (from datasheet)

Controlled via analog registers afe_0x0e through afe_0x17, 2 bits per pin:

| Value | Resistor |
|-------|----------|
| `00` | None (floating) |
| `01` | **1M pull-up** (weak) — used for row pins |
| `10` | **100K pull-down** |
| `11` | **10K pull-up** (strong) — used for column pins |

### Drive Strength (from datasheet)

| Pins | DS=1 (default) | DS=0 |
|------|----------------|------|
| PA5-PA7, PE0-PE1, PE4-PE7 | **8 mA** | 4 mA |
| All other GPIOs | **4 mA** | 2 mA |

Default is maximum drive strength. Column/row matrix pins (PA0-PA4, PB1-PB6, PC1, PD2-PD6, PE0, PE3-PE7) use default max drive.

## Additional GPIO Pins (non-matrix)

| Pin | Direction | Purpose | Evidence |
|-----|-----------|---------|----------|
| **PB7** | **Output** | **RGB LED SPI MOSI (PSPI IO0)** | func_mux=1 @ 0x80140333[7:6], GPIO disabled @ 0x8014030e bit 7 |
| PC2 | Output | USB/SPI bus control signal (NOT LED data) | Toggled in `hid_report_send()`, configured as GPIO in `secondary_pipeline()` |
| PC7 | Input | Rotary encoder channel (or wireless switch) | |
| PC0 | Input | Rotary encoder channel (or wireless switch) | |
| PD0 | Input | RF calibration pulse (PWM) | Used by `pwm_gpio_test_pulse()` → `rf_frequency_calibration()` |
| PD7 | Input | Unknown (configured but not in matrix scan) | |
| PA7 | Input | Unknown (configured in init, possibly special key) | |
| PB0 | Input | Unknown (configured in init) | |

Note: PE4-PE7 default to JTAG functions (PE4=TDI, PE5=TDO, PE6=TMS, PE7=TCK per datasheet Table 1-4). Firmware reconfigures them as GPIO for matrix columns. JTAG is NOT available.

**USB pins:** PA5=DM (USB D-), PA6=DP (USB D+) — neither used in matrix. Internal 1.5K pull-up on DP.
**SWS pin:** PA7 defaults to SWS (Single Wire Slave debug interface) — not in matrix, accessible via test pad near MCU.

## RGB LED Output (SPI+DMA)

The 83 WS2812-style per-key RGB LEDs are driven via **PSPI MOSI on PB7** using DMA, not GPIO bit-bang.

| Parameter | Value |
|-----------|-------|
| Data pin | PB7 (PSPI MOSI IO0) |
| SPI module | PSPI (APB bus, base 0x80140040) |
| SPI clock | ~6 MHz (PCLK 24 MHz, divider 1) |
| WS2812 encoding | 8 SPI bits per WS2812 bit → 1.33 µs/bit |
| DMA channel | 4 (request = DMA_REQ_SPI_APB_TX) |
| Buffer | `gp+0x14d8`, 1992 bytes (498 DMA words) |
| LED count | 83 (per-key, matches matrix key count) |
| Transfer dest | PSPI data FIFO (0x80140048) |

**Pin mux setup** in `secondary_pipeline()` @ 0x2000efc8:
- `reg_gpio_pb_fuc_h` (0x80140333) bits [7:6] = `01` → PSPI function for PB7
- `reg_gpio_pb_gpio` (0x8014030e) bit 7 cleared → switches PB7 from GPIO to peripheral mode
- Matches SDK `pspi_set_pin_mux(PSPI_MOSI_IO0_PB7)` exactly

## Scan Method

Column-to-row with output preconditioning:

1. Set all 16 columns HIGH (deactivate)
2. Momentarily enable row pins PD2-PD6 + PE0 as output, drive to known state
3. Drive selected column LOW
4. Release row pins to high-Z (input) — pull-ups pull HIGH
5. Read PE.input (bits 0,3 -> rows 0,6) and PD.input (bits 2-6 -> rows 1-5)
6. Key pressed when row reads LOW (shorted to active-low column)
7. Reset column HIGH, advance to next

Key firmware functions:
- `matrix_scan_core` at `0x20001EF4` — main matrix scan loop
- `matrix_scan_sleep` at `0x2000DA88` — sleep/wake version with full re-init
- `gpio_pin_init` at `0x2000272C` — configures all func_mux, OEN, pull-ups
- `consumer_report_send` at `0x2000F584` — sends 3-byte consumer control HID reports to USB EP2

## Matrix Scan Timing (logic analyzer validated)

| Parameter | Value |
|-----------|-------|
| Scan rate | ~500 Hz (~2ms full cycle) |
| Scan burst duration | ~375 us (all 16 columns sequentially) |
| Per-column active time | ~10.75 us LOW pulse |
| Per-column slot | ~21.2 us |
| Idle between bursts | ~1.63 ms (all columns LOW, MCU doing other work) |

## Column Scan Order Validation

Probed specific keys at known matrix positions:

| Key | Matrix Position | Pulse offset | Expected |
|-----|----------------|-------------|----------|
| ESC | row 0, col 0 | 10.9 us | 10.9 us (reference) |
| HOME | row 0, col 14 | 307.75 us | 307.7 us |

Calculation: 296.85 us across 14 columns = **21.2 us per column slot**.
Column scan order matches firmware pin table exactly — no remapping needed.

## Matrix Layout

- Matrix: **8 rows x 16 columns**
- ISO layout with L-shaped Enter key

**Layer 0 (Default ISO DE 75%):**
```
Row 0: ESC  F1  F2  F3  F4  F5  F6  F7  F8  F9  F10  F11  F12  DEL  HOME  --
Row 1:  `    1   2   3   4   5   6   7   8   9   0    -    =   BKSP  END  --
Row 2: TAB   Q   W   E   R   T   Y   U   I   O   P    [    ]   ENT   --   --
Row 3: CAPS  A   S   D   F   G   H   J   K   L   ;    '    #    --  PGUP PGDN
Row 4: LSFT  <>  Z   X   C   V   B   N   M   ,   .    /  0x87 RSFT  UP   --
Row 5: LCTL LGUI LALT --  --  --  --  -- SPC RALT MO(1) RCTL -- LEFT DOWN RGHT
Row 6: (empty row)
Row 7: VOL+ MUTE (rest empty)
```

ISO key confirmations:
- `KC_NONUS_BACKSLASH` (0x0064) = ISO `<>|` key
- `KC_NONUS_HASH` (0x0032) = ISO `#'` key
- `MO(1)` = Fn key (momentary layer 1 toggle)
- Custom keycodes: `0x7820`-`0x782A` (RGB), `0x7E01xx` (wireless/system)

VIA JSON files: `reverse/reference/via_iso_usb/`, `reverse/reference/via_iso_2.4g/`

RGB effects: 19 modes (OFF, WAVE, COLOUR_CLOUD, VORTEX, MIX_COLOUR, BREATHE, LIGHT, SLOWLY_OFF, STONE, LASER, STARRY, FLOWERS_OPEN, TRAVERSE, WAVE_BAR, METEOR, RAIN, SCAN, TRIGGER_COLOUR, CENTER_SPREAD)

**Fn Layer Key Combos (stock Evision firmware, from the manual — Windows mode):**

> These are the **stock firmware's** combos, documented here as a reverse-engineering
> reference. They do **not** apply to this ZMK firmware — for the ZMK keymap (Bluetooth
> profiles, reset, ZMK Studio, RGB) see **[usage.md](usage.md)**.

| Combo | Function | Category |
|-------|----------|----------|
| Fn+ESC (3s) | Factory reset | System |
| Fn+F1/F2/F3 | BT device 1/2/3 (press=connect, long=pair) | Connection |
| Fn+F4 | 2.4G (long press=pair) | Connection |
| Fn+F5 | Previous Track | Media |
| Fn+F6 | Next Track | Media |
| Fn+F7 | Mute | Media |
| Fn+F8 | Volume Down | Media |
| Fn+F9 | Volume Up | Media |
| Fn+F10 | Play/Pause | Media |
| Fn+F11 | Brightness Down | Media (display) |
| Fn+F12 | Brightness Up | Media (display) |
| Fn+Tab | Cycle connection mode (ESC→F1-F3→F4) | Connection |
| Fn+M (3s) | Toggle Windows/MAC mode | System |
| Fn+H | Toggle ultra-low latency mode | System |
| Fn+WIN | Lock/Unlock Windows key | System |
| Fn+Space (hold) | Battery level indicator | System |
| Fn+L | Toggle Long Battery Life Mode (v20240121+) | System |
| Fn+Backspace | ON/OFF RGB backlight | RGB |
| Fn+Enter | Cycle RGB effect mode | RGB |
| Fn+\\| | Switch RGB color | RGB |
| Fn+↑ | RGB brightness up | RGB |
| Fn+↓ | RGB brightness down | RGB |
| Fn+← | RGB speed down | RGB |
| Fn+→ | RGB speed up | RGB |
