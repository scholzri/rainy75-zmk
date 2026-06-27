# WOB Driver (wobwxe.com) JavaScript Analysis

Analysis of the WOB keyboard configuration tool at `wobwxe.com`, fetched live on 2026-02-17.

## Overview

- **URL:** `https://wobwxe.com` (title: "WOB驱动" / WOB Driver)
- **Framework:** Vue.js SPA (webpack-chunked, 30 JS bundles, ~657KB app code)
- **Protocol:** WebHID API (`navigator.hid`, `sendReport`/`inputreport` events)
- **ICP Filing:** 赣ICP备2024040482号 (Jiangxi province, China)
- **Config encryption key:** present in the JS bundle (value redacted here — third-party vendor secret)
- **Localization:** Chinese (default), English, Korean, Japanese

## Device Table

The WOB driver supports **10 devices** with **two different VIDs**:

| Product | VID (hex) | PID (hex) | usagePage | usage | reportId | reportSize | Protocol |
|---------|-----------|-----------|-----------|-------|----------|------------|----------|
| **Rainy 75 (standard)** | **0x320F** | **0x5055** | **0xFF1C** | **0x92** | **4** | **0** | **Packet-based** |
| New product (TBD) | 0x36B0 | 0x3025 | 0xFF60 | 0x61 | 0 | 0 | Packet-based |
| Rainy 98 (wired) | 0x36B0 | 0x3091 | 0xFF60 | 0x61 | 0 | 256 | Command-ID |
| Rainy 98 (wireless) | 0x36B0 | 0x3098 | 0xFF60 | 0x61 | 0 | 256 | Command-ID |
| Rainy 75 RT | 0x36B0 | 0x352F | 0xFF60 | 0x61 | 0 | 1024 | Address-based |
| Rainy 75 RT KR | 0x36B0 | 0x3530 | 0xFF60 | 0x61 | 0 | 1024 | Address-based |
| Rainy 75 RT (variant 3) | 0x36B0 | 0x3531 | 0xFF60 | 0x61 | 0 | 1024 | Address-based |
| Zen 65 RT | 0x36B0 | 0x343F | 0xFF60 | 0x61 | 0 | 1024 | Address-based |
| Rainy 98 Core (DFU) | 0x36B0 | 0x3099 | 0xFF60 | 0x61 | — | — | Firmware DL |
| Rainy 98 RX (DFU) | 0x36B0 | 0x3090 | 0xFF60 | 0x61 | — | — | Firmware DL |

### Key Insight: Two VIDs

- **VID 0x320F** = Evision Semiconductor platform VID (shared with CIDOO, IQUNIX, Ajazz, EPOMAKER)
- **VID 0x36B0** (14000 decimal) = WOB's own vendor ID for their hall-effect / custom-MCU keyboards

Our **Rainy 75 Pro ISO DE** (0x320F:0x5055) is on the Evision platform. The hall-effect "RT" models use WOB's own VID.

### Key Insight: Different HID Interface

Our keyboard connects on a **different HID interface** than VIA:

| Protocol | usagePage | usage | collection | reportId |
|----------|-----------|-------|------------|----------|
| VIA | 0xFF60 | 0x61 | collections[0] | 0 |
| **WOB proprietary** | **0xFF1C** | **0x92** | **collections[2]** | **4** |

The keyboard has **both** interfaces simultaneously. VIA uses one, WOB driver uses the other.

## Protocol Architecture

The WOB driver implements **three distinct HID protocols** depending on the keyboard model:

### Common Framing

All protocols share the same packet framing:
- **Start marker:** `0xBE` (190 decimal)
- **End marker:** `0xED` (237 decimal)
- **Transport:** Output Reports via `sendReport()`, responses via `inputreport` event

### Protocol A: Address-Based (Hall-Effect Keyboards)

Used by: Rainy 75 RT (0x352F, 0x3530, 0x3531), Zen 65 RT (0x343F)

These keyboards read/write flash data using direct SRAM-mapped addresses.

**Read Command:**
```
Byte:  0     1     2     3     4     5     6     7     8     9     10
     [0xBE][0x04][len_lo][len_hi][addr0][addr1][addr2][addr3][dlen_hi][dlen_lo][0xED]
```
- `0x04` = read command
- len = always 0x000B (11 bytes)
- addr = 32-bit LE flash address (e.g., `0x80080144`)
- dlen = data length (16-bit **big-endian**)

**Read Response:**
```
Byte:  0     1     2     3     4-9     10...(N-4)   (N-3)  (N-2)  (N-1)
     [0xBE][type][len_lo][len_hi][header][  data  ][chk_lo][chk_hi][0xED]
```
- type = `0x01` or `0x04`
- Validation: `byte[0]==0xBE && (byte[1]==1 || byte[1]==4)`
- Checksum at `(len-3)` must match calculated checksum at `(len-2,len-3)`
- Data payload: bytes 10 through (len-4)

**Write Command:**
```
Byte:  0     1     2     3     4-7     8     9     10...(10+N-1)  ...  last
     [0xBE][0x03][plen_lo][plen_hi][addr LE][dlen_hi][dlen_lo][ data ]...[chk][0xED]
```
- `0x03` = write command
- plen = 13 + data_length (total packet size)
- addr = 32-bit LE flash address
- data immediately follows at offset 10

### Protocol B: Command-ID Based (Rainy 98)

Used by: Rainy 98 wired (0x3091), Rainy 98 wireless (0x3098)

Uses logical command IDs instead of raw addresses. Commands reference a parameter table:

**Command Table:**
| Command | ID | Per-Key Bytes | Description |
|---------|----|---------------|-------------|
| cmd_key_code_1 | 2 | 2 | Key mapping layer 1 |
| cmd_key_code_2 | 3 | 2 | Key mapping layer 2 |
| cmd_key_long_code_1 | 4 | 2 | Long-press key code 1 |
| cmd_key_long_code_2 | 5 | 2 | Long-press key code 2 |
| cmd_key_cancel_code | 6 | 2 | SOCD cancel codes |
| cmd_macro_stop_type_1 | 7 | 1 | Macro stop type 1 |
| cmd_macro_stop_type_2 | 8 | 1 | Macro stop type 2 |
| cmd_macro_req_count_1 | 9 | 1 | Macro repeat count 1 |
| cmd_macro_req_count_2 | 10 | 1 | Macro repeat count 2 |
| cmd_rgb_color | 48 | 3 | Per-key RGB color |
| cmd_rgb_solid_enable | 49 | 1 | Per-key solid color enable |
| cmd_rgb_config | 50 | 8 | RGB global config |
| cmd_rgb_loge_color | 51 | 3 | Logo RGB color |
| cmd_rgb_loge_solid_enable | 52 | 1 | Logo solid enable |
| cmd_rgb_loge_config | 53 | 8 | Logo RGB config |
| cmd_rgb_board_color | 54 | 3 | Board underglow color |
| cmd_rgb_board_solid_enable | 55 | 1 | Board underglow solid enable |
| cmd_rgb_board_config | 56 | 8 | Board underglow config |
| cmd_config_name | 67 | 64 | Config profile name |
| cmd_soft_version | 68 | 4 | Software version |
| cmd_cur_param_num | 69 | 1 | Current config number |
| cmd_param_enable | 70 | 1 | Config enable flags |
| cmd_sleep_config | 71 | 4 | Sleep timing config |
| cmd_hard_version | 72 | 4 | Hardware version |
| cmd_macro_enable | 72 | 10 | Macro enable flags |
| cmd_macro_length | 73 | 20 | Macro lengths |
| cmd_macro_addr | 74 | 20 | Macro addresses |
| cmd_macro_data_area | 75 | 0 | Macro raw data |

**Read format:** `[0xBE, comm_param_read, 10, cmd_id, length, offset(2), checksum(2), 0xED]`
**Write format:** `[0xBE, comm_param_set, 10+data_len, cmd_id, data_len, offset(2), ...data, checksum(2), 0xED]`

### Protocol C: Packet-Based (Rainy 75 Standard — Our Keyboard)

Used by: Rainy 75 standard (0x320F:0x5055), new product (0x3025)

Uses pre-defined packet command sequences (`AllDataCom`, `keyMapCom`, etc.) sent over reportId 4.

**Packet command arrays:**
```javascript
keyMapCom: [
  [63,0,7,56,0,0,0], [119,0,7,56,56,0,0], [175,0,7,56,112,0,0],
  [231,0,7,56,168,0,0], [31,1,7,56,224,0,0], [88,0,7,56,24,1,0],
  [136,0,7,48,80,1,0]
]

AllDataCom: [
  [1,0,1], [37,0,3,34],
  [64,0,8,56,0,0,0], [120,0,8,56,56,0,0], [176,0,8,56,112,0,0],
  [232,0,8,56,168,0,0], [32,1,8,56,224,0,0], [89,0,8,56,24,1,0],
  [137,0,8,48,80,1,0], [83,0,27,56], [139,0,27,56,56],
  [155,0,27,16,112], [2,0,2], [6,0,5,1], [44,0,5,39],
  [32,0,26,6], [76,0,20,56,0,0,0]
]

SetKeyMapCom: [
  [1,0,1], [37,0,3,34],
  [173,6,9,56,0,0,0], [92,5,9,56,56,0,0], ...
  [2,0,2]
]
```

The packet format appears to encode: `[total_len, flags, cmd_type, chunk_size, offset_lo, offset_mid, offset_hi]`. Full analysis of byte semantics pending.

Note: `[1,0,1]` and `[2,0,2]` appear to be transaction begin/end markers (handshake/save).

## Special Commands

All keyboard types share these common commands:

| Command | Bytes | Description |
|---------|-------|-------------|
| Heartbeat | `[0xBE, 0x00, 0x05, 0x00, 0xED]` | Start/keepalive |
| Save to flash | `[0xBE, 0x02, 0x05, 0x00, 0xED]` | Persist settings |
| Check FW status | `[0xBE, 0x74, 0x05, 0x00, 0xED]` | Firmware update check |
| Enter DFU mode | `[0xBE, 0xFC, 0x05, 0x00, 0xED]` | Download mode (wired) |
| Enter DFU mode | `[0xBE, 0xFC, 0x04, 0xED]` | Download mode (wireless) |
| Factory reset | `[0xBE, 0xEE, 0x05, 0x00, 0xED]` | Reset all settings |
| Start calibration | `[0xBE, 0xFE, 0x06, 0x00, 0x01, 0xED]` | Begin cal data streaming |
| Stop calibration | `[0xBE, 0xFE, 0x06, 0x00, 0x00, 0xED]` | Stop cal data streaming |

## Flash Address Map (Rainy 75 RT)

Direct SRAM-mapped flash addresses for the hall-effect keyboards:

### Key Mapping (81 keys × 2 bytes = 162 bytes per region)
| Region | Address | Length | Description |
|--------|---------|--------|-------------|
| key_map | 0x80080144 | 162 | Main layer keymap |
| key_map_fn | 0x800801E6 | 162 | Fn layer keymap |
| key_map_cover | 0x8008097E | 162 | SOCD override layer |
| key_map_cover_priority | (calculated) | 162 | SOCD priority |
| key_bind_mode | 0x80081DE0 | 81 | Bind mode per key (0=normal, 1=group, 2=DKS) |
| bind_key_setting | 0x80080D00 | 48 | DKS config per key (48 bytes × index) |

### Hall-Effect Performance (81 keys × 2 bytes = 162 bytes per region)
| Region | Address | Length | Description |
|--------|---------|--------|-------------|
| key_property | 0x80080654 | 81 | Key mode (0=normal, 1=rapid trigger, 2=special) |
| key_press | 0x80080288 | 162 | Actuation press point (0.01mm units) |
| key_return | 0x8008032A | 162 | Actuation return point |
| key_topDead | 0x800803CC | 162 | Top deadzone |
| key_bottomDead | 0x8008046E | 162 | Bottom deadzone |
| key_normalPress | 0x80080510 | 162 | Normal press threshold |
| key_normalReturn | 0x800805B2 | 162 | Normal return threshold |
| key_press_pro | 0x80080798 | 162 | Anti-chatter protection threshold |

### Calibration
| Region | Address | Length | Description |
|--------|---------|--------|-------------|
| auto_calib_enable | 0x80080B89 | 1 | Auto-calibration on/off |
| top_value | 0x80070000 | 162 | Calibration top ADC values |
| bot_value | 0x800700A2 | 162 | Calibration bottom ADC values |

### Switch Body / Profile
| Region | Address | Length | Description |
|--------|---------|--------|-------------|
| key_body | 0x80070144 | 162 | Switch type per key |
| key_body_custom1 | 0x80070168 | 256 | Custom switch profile 1 |
| key_body_custom2 | 0x80070268 | 256 | Custom switch profile 2 |
| key_body_custom3 | 0x80070468 | 256 | Custom switch profile 3 |
| key_body_custom4 | 0x80070668 | 256 | Custom switch profile 4 |

### RGB / Lighting
| Region | Address | Length | Description |
|--------|---------|--------|-------------|
| key_Rgb | 0x80080A20 | 255 | Per-key RGB data |
| key_RgbMode | 0x80080B1F | 85 | Per-key RGB mode |
| key_Rgb_public | 0x80080B80 | 9 | Global RGB settings |
| key_dynamic | 0x80080B78 | 5 | Dynamic lighting config |
| light_mode | 0x80080B78 | 1 | Current light mode |
| light_h | 0x80080B79 | 1 | Hue |
| light_s | 0x80080B7A | 1 | Saturation |
| light_v | 0x80080B7B | 1 | Value/brightness |
| light_speed | 0x80080B7C | 1 | Animation speed |
| light_sleep_time | 0x80080B84 | 4 | Light sleep timeout |
| rgb_enable | 0x80080B80 | 1 | RGB master enable |
| cap_enable | 0x80080B81 | 1 | Caps lock indicator |
| rgb_fn_enable | 0x80080B83 | 1 | Fn layer indicator |
| win_lock_state | 0x80080B88 | 1 | Win lock indicator |
| tactile_light | 0x80080B8A | 1 | Tactile/reactive lighting |

### System
| Region | Address | Length | Description |
|--------|---------|--------|-------------|
| firmwareVersion | 0x800701E8 | 4 | Firmware version (v[2].[1].[0]) |
| key_configName | 0x80070268 | 256 | Config profile name |
| currentConfig | 0x800701F0 | 12 | Current config state |
| configSwitch3 | 0x800701F4 | 1 | Config switch 3 |
| configSwitch4 | 0x800701F8 | 1 | Config switch 4 |

## Rainy 75 Standard (Our Keyboard) Config

Extracted device configuration for VID 0x320F / PID 0x5055:

```javascript
{
  productId: 20565,   // 0x5055
  vendorId: 12815,    // 0x320F
  usagePage: 65308,   // 0xFF1C
  usage: 146,         // 0x92
  reportId: 4,
  reportSize: 0,
  keyboardLength: 81,
  deviceType: 1,
  layerList: [{name: "主层", value: 0}],  // Single layer only
  navsOpenList: [true, false, true, false, true, true],
  // → [keyMapping: YES, performance: NO, light: YES, other: NO, macro: YES, settings: YES]
  charReplaceOpenList: [true, true, true, true],
  SOCDSettingType: 0,
  performanceSettingNavsOpenList: [false, false, false, false],  // No hall-effect!
  firmwareOpenStatus: [false, false, false],  // Firmware download DISABLED
  deviceUpdateMethod: 0,
  keyMappingType: 0,
  switchSettingType: 0,
  isAllowedCal: true,   // Calibration allowed (interesting!)
  RTProMode: 0,          // No Rapid Trigger
  macroDataProcessType: 0,
  isExistMacroActionsList: true,
  isNeedMouseAction: true,
  macroAllowActionsNum: 90,
  macroDelayMax: 65535,
  specialLightSetting: {
    lightListType: 0,
    tactileLight: false,
    lightingDir: false,
    winLockLight: false,
    FnLight: false,
    CapsLight: false
  }
}
```

### Key Layout (81 keys, matches 75% ISO)

Key widths confirm ISO layout: 1u, 1.5u, 1.75u, 2u, 2.25u, 6.25u, 1.25u keys.
Row 4 has the ISO Enter (2.25u) and ISO backslash key.
FnFixedKeys: positions `[15, 31, 46, 47, 60, 65, 76, 79, 83, 84, 85, 86, 87, 89, 92]`

### Default Keymap (Layer 0)

The `h` array contains the default keymap in 3-byte encoding `[flags, keycode_hi, keycode_lo]`:
```
Row 0: Esc, F1-F4, F5-F8, F9-F12, PrtSc, Del, (encoder)
Row 1: ` ~, 1-0, -, =, Backspace, Home
Row 2: Tab, Q-], Backslash, PgUp
Row 3: CapsLock, A-', Enter, PgDn
Row 4: LShift, ISO(\), Z-/, RShift, Up
Row 5: LCtrl, LWin, LAlt, Space, RAlt, Fn, Left, Down, Right
```

## Firmware Files

| Product | File | Format | Version | Path |
|---------|------|--------|---------|------|
| Rainy 75 RT | master | .uf2 | 0.0.47 | `./firmware/rainy75rt/master/0.0.47.uf2` |
| Rainy 75 RT (netbar) | netbar | .uf2 | 0.0.53 | `./firmware/rainy75rt/master/0.0.53-netbar.uf2` |
| Rainy 75 RT KR | master | .uf2 | 0.0.52 | `./firmware/rainy75rtkr/master/0.0.52.uf2` |
| Zen 65 RT | master | .uf2 | 0.0.33 | `./firmware/zen65rt/master/0.0.33.uf2` |
| Rainy 98 Core | core | .bin | 0.0.31 | `./firmware/Rainy98/core/0.0.31.bin` |
| Rainy 98 RX | rx | .bin | 0.0.13 | `./firmware/Rainy98/rx/0.0.13.bin` |

### Firmware Download Protocol

**Rainy 75 RT (.uf2 — direct USB):**
1. Enter DFU: `[0xBE, 0xFC, 0x05, 0x00, 0xED]`
2. Wait for reconnection
3. Start: `[0x81, 0x00, ...checksum(4 bytes), 0x06]` (timeout 2500ms)
4. Data chunks: `[0x80, len, ...data]` (62 bytes per chunk)
5. Address chunks: `[0x82, len, ...addr(4 bytes LE), ...data]` (56 bytes per chunk)
6. Finish: `[0x83, 0x00]`

**Rainy 98 (.bin — wireless core+rx):**
- Same download protocol but targets separate Core (PID 0x3099) and RX (PID 0x3090) devices
- checkValue: 8192 for both core and rx

## Magnetic Switch Types

The driver supports these hall-effect switch profiles:

| Index | Chinese | English | Travel | Color RGB |
|-------|---------|---------|--------|-----------|
| 0 | RGB万磁王 | RGB Magneto | 3.375mm | (189,70,70) |
| 1 | 万磁王 | Magneto | 3.375mm | (255,150,150) |
| 2 | POM万磁王 | POM Magneto | — | — |
| 3 | 天王轴电竞版 | TianWang Gaming | 3.344mm | (255,69,0) |
| 4 | 磁玉 | CiYu | — | — |
| 5 | 磁玉Pro | CiYu Pro | 3.355mm | (102,210,206) |
| 6 | 磁玉Gaming | CiYu Gaming | — | — |
| 7 | 兵王磁轴 | TTC BingWang | 3.416mm | (95,139,76) |
| 8 | 白蛇磁轴 | TTC BaiShe | 3.335mm | (219,219,219) |
| 9 | 泰山磁轴 | TaiShan | 3.338mm | (194,217,255) |
| 10 | 磁神轴 | CiShen | — | — |
| 11 | 八宝轴粉 | BaBao Pink | — | — |

Each switch type has calibration data with `calib_point` arrays (69-71 points), `height_max`, and `calib_th` (typically 7000).

### Performance Mode Presets

Each switch has default RT/performance profiles (`keyModeDate`, `keyPressDate`, `keyReturnsDate`, etc.) with per-key values:
- **keyModeDate:** 0=normal, 1=rapid trigger, 2=special
- **keyPressDate:** Actuation point (e.g., 50=0.5mm, 130=1.3mm, 400=4.0mm)
- **keyReturnsDate:** Return point (e.g., 30=0.3mm, 130=1.3mm)
- **topPressDeadDate/bottomPressDeadDate:** Dead zones (100=1.0mm, 1000=default disabled)
- **protectPressDate:** Anti-chatter threshold

## WebHID Communication Layer

```javascript
// Core send/receive (from app_5c551db8.69eed780.js)
{
  device: null,        // HIDDevice instance
  queue: [],           // command queue
  isProcessing: false,
  time: 0,             // response timeout (ms)
  reportId: 0,         // HID report ID
  reportSize: 0,       // report padding size (0 = no padding)

  setDevice(device, reportId, reportSize) { ... },

  async send(command, timeout = 100) {
    // If reportSize > 0, pad command to reportSize bytes
    // Queue command, process sequentially
    // Send via device.sendReport(this.reportId, new Uint8Array(command))
    // Wait for 'inputreport' event (one-shot listener)
    // Timeout after this.time ms → Error("响应超时")
    // Returns response as Uint8Array
  }
}
```

- Response timeout is typically 100ms, extended to 200ms for firmware status, 1000ms for handshake, 2500ms for DFU start
- `reportSize > 0`: command is zero-padded to fixed size (256 or 1024 bytes)
- `reportSize == 0`: command sent at exact length (our keyboard)

## Features by Keyboard Type

| Feature | Standard (ours) | RT (hall-effect) | Rainy 98 |
|---------|:-:|:-:|:-:|
| Key mapping | 1 layer | 2 layers (main+Fn) | 2 layers |
| SOCD | No | Yes (type 0-1) | Yes |
| DKS (Dynamic Keystroke) | No | Yes (48 bytes/key) | No |
| Rapid Trigger | No | Yes (per-key) | No |
| Performance presets | No | Yes (4 presets) | No |
| Per-key calibration | Yes* | Yes | No |
| Auto-calibration | No | Yes | No |
| Switch replacement | No | Yes | No |
| Macro | Yes (90 actions) | Yes | Yes (60 actions) |
| Per-key RGB | No | Yes | Yes |
| Logo/Board LED | No | Per-model | Yes (4+49 LEDs) |
| Firmware update | Disabled | .uf2 direct | .bin wireless |
| Sleep config | No | Yes | Yes |
| Config import/export | No | Yes | No |
| Long-press keycodes | No | No | Yes |
| Mouse actions in macro | Yes | Yes | Yes |

*Our keyboard has `isAllowedCal: true` but no performance settings UI

## Changelog Timeline

### Driver (wobwxe.com web app)
- **0.2.1** (2026-01-01): Latest as of fetch date
- **0.1.9** (2025-06-16): Driver optimization
- **0.1.7** (2025-05-17): Fix DKS precision
- **0.1.6** (2025-04-25): Add Zen 65 RT support, light speed mode, anti-disconnect, auto-calibration on power-up, switch replacement, driver URL shortcut key
- **0.1.5** (2025-03-15): Optimization
- **0.1.4** (2024-12-27): Tactile light switch, improvements
- **0.1.3** (2024-12-24): Config import/export, guide button
- **0.1.1** (2024-12-12): DKS + combo key features
- **0.1.0** (2024-11-20): Mouse keys, macro drag-select, DKS, initial trigger distance
- **0.0.13** (2024-10-31): SOCD optimization
- **0.0.8** (2024-10-12): SOCD indicator, performance display optimization
- **0.0.1** (2024-08-08): Initial driver

### Rainy 75 RT Firmware
- **0.0.47** (2025-05-19): Fix system function keys
- **0.0.46** (2025-04-29): Fix BIOS issues, auto-calibration optimization
- **0.0.43** (2024-12-27): Tactile light mode
- **0.0.38** (2024-11-20): Initial trigger distance setting
- **0.0.33** (2024-10-14): TianWang Gaming switch, offline calibration mode
- **0.0.26** (2024-09-28): Initial firmware

### Rainy 98 Firmware
- **0.0.33** (2025-12-25): Fix known keyboard abnormal states
- **0.0.31** (2025-09-25): Performance optimization
- **0.0.25** (2025-09-01): Light speed mode, SOCD, new switch adapters
- **0.0.8** (2025-03-15): Initial firmware

## Comparison with Other Protocols

| Aspect | WOB Driver | VIA (0xFF60) | GearHub (qmk.top) |
|--------|------------|--------------|---------------------|
| Transport | Output Reports | Output Reports | Feature Reports |
| usagePage | 0xFF1C (ours) / 0xFF60 (RT) | 0xFF60 | — (VID 0x3151) |
| Framing | 0xBE...0xED | Raw 32-byte | 9-byte + checksum |
| Addressing | Address / CmdID / Packet | VIA commands | Proprietary |
| Layers | 1-2 | 4 | — |
| Flash access | Direct SRAM addresses | VIA EEPROM | — |

## Analysis Files

All JS chunks saved to `/tmp/claude/wobwxe_js/`:
- `runtime.js` (5KB) — webpack runtime
- `app_5c551db8.69eed780.js` (71KB) — core: device table, HID layer, keycode maps, firmware download, Vuex store
- `app_918758ea.7850b4b2.js` (56KB) — Rainy 75 RT + Rainy 75 Standard device configs
- `app_a8ec8690.6e498107.js` (95KB) — Rainy 98 + Rainy 75 RT KR/variant3 device configs
- `app_914aebb6.6cf95d19.js` (38KB) — Rainy 75 RT KR device config
- `app_3ccc34ac.cb916b2d.js` (39KB) — Zen 65 RT device config
- `app_12245e1e.480c7eb9.js` (21KB) — New product (PID 0x3025) device config
- `app_648002f7.24a3d198.js` (64KB) — HomePage, device connection, firmware download UI
- `app_9f41190c.9092c19e.js` (105KB) — Performance, calibration, light, switch setting UIs
- `app_c3373795.1a912025.js` (44KB) — Main app shell, changelog, switch calibration data
- `app_f5ef7187.da3aebc7.js` (118KB) — i18n translations (CN, EN, KR, JP)
