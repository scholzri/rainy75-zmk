# B91 TLSR9511 GPIO Pinout — Rainy 75 Pro ISO DE

Complete GPIO pin assignment map. 40 GPIO pins (PA0-PA7, PB0-PB7, PC0-PC7, PD0-PD7, PE0-PE7) plus 6 flash pins (PF0-PF5).

## GPIO Register Dump (from diagnostic scan)

```
PA: in=c0 oen=e0 out=00 ie=00 pol=ff gpio=1f irq=00
PB: in=00 oen=81 out=00 ie=00 pol=ff gpio=7f irq=00
PC: in=00 oen=f9 out=04 ie=00 pol=ff gpio=ff irq=00
PD: in=fe oen=7f out=d7 ie=fc pol=ff gpio=ff irq=7c
PE: in=0d oen=0f out=00 ie=01 pol=ff gpio=ff irq=00

Analog pull: PA=0x00 PB=0x00 PC=0x00 PD=0x00 PE=0x20
Analog IE:   PC=0x3f PD=0x03
```

Register key: `in`=input read, `oen`=output enable (0=output, 1=hi-z), `out`=output value,
`ie`=digital input enable, `pol`=polarity, `gpio`=GPIO mode (vs alternate function), `irq`=interrupt enable.

## ADC Scan Results (batteries fully charged ~4.2V)

| Channel | Pin | Voltage | Notes |
|---------|-----|---------|-------|
| 0x1 | PB0 | 960–1005 mV | Unknown external circuit, ~1V through low impedance |
| 0x2-0x8 | PB1-PB7 | 0 mV | Matrix columns (PB1-PB6) + RGB MOSI (PB7) |
| 0x9 | PD0 | 956–975 mV | RF calibration PWM circuit |
| 0xA | **PD1** | **2154–2177 mV** | **Battery ADC** — 1/2 divider → ~4308–4354 mV battery |
| VBAT/3 | internal | 1108–1110 mV | MCU supply rail = 3324–3330 mV (3.3V regulator) |

## Port A (PA0–PA7)

| Pin | Function | Dir | Evidence | Confidence |
|-----|----------|-----|----------|------------|
| PA0 | Matrix column 4 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PA1 | Matrix column 5 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PA2 | Matrix column 6 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PA3 | Matrix column 7 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PA4 | Matrix column 8 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PA5 | USB D- (DM) | Alt | gpio=0 (alternate function), oen=1 | **Confirmed** |
| PA6 | USB D+ (DP) | Alt | gpio=0, oen=1, in=1 (idle high) | **Confirmed** |
| PA7 | SWS debug | Alt | gpio=0 (restored by boot_diag POST_KERNEL) | **Confirmed** |

## Port B (PB0–PB7)

| Pin | Function | Dir | Evidence | Confidence |
|-----|----------|-----|----------|------------|
| **PB0** | **Unknown** — external circuit at ~1V | In | gpio=1, oen=1, ie=0, ADC=960–1005mV, probe: driven LOW | **Unknown** |
| PB1 | Matrix column 9 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PB2 | Matrix column 10 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PB3 | Matrix column 11 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PB4 | Matrix column 12 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PB5 | Matrix column 13 | Out | DTS col-gpios. MUST NOT be PSPI CLK | **Confirmed** |
| PB6 | Matrix column 14 | Out | DTS col-gpios, gpio=1, oen=0 | **Confirmed** |
| PB7 | RGB PSPI MOSI (IO0) | Alt | gpio=0 (SPI function), DMA ch4 to 0x80140048 | **Confirmed** |

## Port C (PC0–PC7)

| Pin | Function | Dir | Evidence | Confidence |
|-----|----------|-----|----------|------------|
| **PC0** | **Unused** — driven LOW externally | In | oen=1, ie=0, PC_IE_ana bit 0=1, probe: driven LOW | **Resolved** |
| PC1 | Matrix column 15 | Out | DTS col-gpios, oen=0 | **Confirmed** |
| PC2 | LED power MOSFET gate | Out | oen=0, out=1 (HIGH=on), active-HIGH | **Confirmed** |
| **PC3** | **Unused** — driven LOW externally | In | oen=1, PC_IE_ana bit 3=1, probe: driven LOW | **Resolved** |
| **PC4** | **Unused** — driven LOW externally | In | oen=1, PC_IE_ana bit 4=1, probe: driven LOW | **Resolved** |
| **PC5** | **Unused** — driven LOW externally | In | oen=1, PC_IE_ana bit 5=1, probe: driven LOW | **Resolved** |
| **PC6** | **Unused** — driven LOW externally | In | oen=1, probe: driven LOW | **Resolved** |
| **PC7** | **Unused** — driven LOW externally | In | oen=1, ie=0, probe: driven LOW | **Resolved** |

Note: PC_IE_ana=0x3F means PC0-PC5 have analog input enable — likely POR default, not functional. None of these pins are referenced in the original firmware (0/211 functions). All read LOW with 10K internal pull-up unable to overcome external path.

## Port D (PD0–PD7)

| Pin | Function | Dir | Evidence | Confidence |
|-----|----------|-----|----------|------------|
| PD0 | RF calibration PWM | In | PD_IE_ana bit 0=1, ADC=956mV, decompiled `pwm_gpio_test_pulse()` | **Confirmed** |
| PD1 | Battery ADC (1/2 divider) | In | PD_IE_ana bit 1=1, ADC=2154mV, channel 0x0A | **Confirmed** |
| PD2 | Matrix row 1 | In | DTS row-gpios, ie=1, irq=1, in=1 (pull-up idle) | **Confirmed** |
| PD3 | Matrix row 2 | In | DTS row-gpios, ie=1, irq=1, in=1 | **Confirmed** |
| PD4 | Matrix row 3 | In | DTS row-gpios, ie=1, irq=1, in=1 | **Confirmed** |
| PD5 | Matrix row 4 | In | DTS row-gpios, ie=1, irq=1, in=1 | **Confirmed** |
| PD6 | Matrix row 5 | In | DTS row-gpios, ie=1, irq=1, in=1 | **Confirmed** |
| PD7 | Boot diagnostic heartbeat | Out | oen=0, out=1, used by boot_diag.c | **Confirmed** |

## Port E (PE0–PE7)

| Pin | Function | Dir | Evidence | Confidence |
|-----|----------|-----|----------|------------|
| PE0 | Matrix row 0 | In | DTS row-gpios, ie=1, in=1 (pull-up idle) | **Confirmed** |
| **PE1** | **Unused** — driven LOW externally | In | oen=1, ie=0, probe: driven LOW | **Resolved** |
| **PE2** | **Unknown** — reads HIGH, weak driver | In | oen=1, ie=0, probe: float=1 up=0 down=1 | **Unknown** |
| **PE3** | **Permanently HIGH** — not wireless switch | In | oen=1, ie=0, in=1, SWS polling: no change on switch toggle | **Resolved** |
| PE4 | Matrix column 0 (JTAG TDI) | Out | DTS col-gpios, oen=0 | **Confirmed** |
| PE5 | Matrix column 1 (JTAG TDO) | Out | DTS col-gpios, oen=0, PE pull bit 5=1 | **Confirmed** |
| PE6 | Matrix column 2 (JTAG TMS) | Out | DTS col-gpios, oen=0 | **Confirmed** |
| PE7 | Matrix column 3 (JTAG TCK) | Out | DTS col-gpios, oen=0 | **Confirmed** |

## Port F (PF0–PF5) — Flash SPI

| Pin | Function |
|-----|----------|
| PF0-PF5 | MSPI flash (Puya P25Q80U 1MB) — not available as GPIO |

## Summary

### By function

| Function | Pins | Count |
|----------|------|-------|
| Matrix columns | PE4-PE7, PA0-PA4, PB1-PB6, PC1 | 16 |
| Matrix rows | PE0, PD2-PD6 | 6 |
| USB | PA5 (DM), PA6 (DP) | 2 |
| SWS debug | PA7 | 1 |
| RGB LED data | PB7 (PSPI MOSI) | 1 |
| LED power | PC2 (MOSFET gate) | 1 |
| Battery ADC | PD1 (channel 0x0A) | 1 |
| RF calibration | PD0 (PWM) | 1 |
| Boot diagnostic | PD7 (heartbeat) | 1 |
| Flash SPI | PF0-PF5 | 6 |
| Unused (resolved) | PC0, PC3-PC7, PE1, PE3 | 8 |
| **Unknown** | **PB0, PE2** | **2** |
| **Total** | | **46** |

### Pin probe results (v2 diagnostic — IE-enabled tri-state reads)

Each pin probed: float (no pull), pull-up (10K internal), pull-down (internal).

| Pin | Float | Pull-up | Pull-down | Classification |
|-----|-------|---------|-----------|----------------|
| PB0 | 0 | 0 | 1 | Driven LOW (~1V, below VIH 2.3V). 10K pull-up can't overcome. |
| PC0 | 0 | 0 | 1 | Driven LOW. External low-impedance path. |
| PC3-PC7 | 0 | 0 | 1 | All driven LOW. External low-impedance paths. |
| PE1 | 0 | 0 | 1 | Driven LOW. |
| PE2 | 1 | 0 | 1 | Weak HIGH — floats HIGH but 10K pull-up reads LOW (anomalous). |
| PE3 | 1 | 1 | 1 | Strongly driven HIGH. No response to wireless switch toggle. |

### Remaining unknown pins

| Pin | ADC | Evidence | Notes |
|-----|-----|----------|-------|
| PB0 | 960–1005 mV | Driven LOW (below VIH), not used by original FW | Connected to external circuit. No ADC code exists in original FW (0/211 functions). Possibly charge IC status or unused test point. |
| PE2 | N/A | Floats HIGH but anomalous pull response | Weak external connection. Not referenced by original FW. |

### Resolved unused pins (8 pins)

PC0, PC3, PC4, PC5, PC6, PC7, PE1, PE3 — none are referenced in the original firmware's 211 functions. All (except PE3) read LOW through external low-impedance paths. PE3 is permanently HIGH and does not respond to the wireless switch.

**PE3 in original firmware**: Checked via `REG_GPIO_PE_SETTING1 & 8` in `key_state_update()` and `gpio_matrix_init()`, mapped to key position 0x408. This is part of the shared Evision platform code — likely used on other keyboards (CIDOO, IQUNIX, etc.) but non-functional on the Rainy 75 Pro where PE3 is tied HIGH.

### Wireless switch (under CapsLock) — RESOLVED

The slide switch is a **hardware battery power cutoff** — not connected to any GPIO.

- **OFF + no USB** → keyboard completely dead (no power)
- **OFF + USB** → runs from VBUS only (LEDs work, BLE works, battery disconnected)
- **ON** → battery powers keyboard (and charges via USB if connected)

Tested: toggling the switch caused zero change in any GPIO input register or ADC reading.
This is consistent with the switch being upstream of the voltage regulator, physically
disconnecting the battery from the power path.

### Original firmware analysis (relevant findings)

- **Zero ADC code** in all 211 functions — the original firmware never reads any ADC channel
- Battery level likely handled internally by BLE blob or estimated from rail voltage
- Only PE0 and PE3 from Port E are referenced (PE0=matrix row, PE3=key position 0x408)
- PB0, PC0, PC3-PC7, PE1, PE2 are completely unreferenced

### Remaining diagnostic tests

1. **PB0 USB unplug test**: Read PB0 ADC via flash dump on battery-only (EVK VCC powers MCU, USB disconnected). If PB0 drops, it's VBUS-derived.
2. **PE2 investigation**: Weak HIGH with anomalous pull response — may be connected to an unpopulated component or test point.
