# Evision Keyboard Firmware Platform

## Discovery

The Wobkey Rainy 75 Pro runs **Evision Semiconductor's proprietary keyboard firmware platform** — a closed-source, NDA-protected SDK deployed across multiple brands and chip architectures. No public source code exists anywhere. This was confirmed through deep research (Feb 2026) and cross-validated by analyzing VIA JSON configurations from sibling keyboards.

## Evision Semiconductor (Shenzhen)

**Shenzhen Evision Semiconductor Technology Co., Ltd.** (深圳维盛半导体科技有限公司)
- Website: eevision.com
- Founded: 2004, ~80 employees (40+ engineering)
- USB VID: `0x320F`
- Business model: Turnkey keyboard/mouse/peripheral solution design house
- Rebrands silicon from multiple vendors under VS-series part numbers
- No public SDK, no developer documentation, no firmware downloads

### Known Evision Part Numbers

| Evision Part | Actual Silicon | Architecture |
|---|---|---|
| VS11K09A-1 | Sonix SN32F248B | ARM Cortex-M0 |
| VS11K16A/17A/20A | Sonix SN32F268 | ARM Cortex-M0 |
| VS11K28A | WCH CH555L | Intel 8051 |
| VS11K34A | Telink TLSR8270 | Telink TC32 |
| *(unmarked)* | **Telink TLSR9511** | **RISC-V Andes D25F** (Rainy 75) |

Source: SonixQMK Mechanical Keyboard Database

## Sibling Keyboards (Confirmed Shared Codebase)

All share VID `0x320F`, the same 19 RGB effect names, and identical custom VIA keycodes:

| Keyboard | Brand | PID (USB) | PID (2.4G) | Form Factor |
|---|---|---|---|---|
| **Rainy 75 / Rainy 75 Pro** | Wobkey | `0x5055` | `0x5088` | 75% ISO/ANSI |
| **ABM066** | CIDOO | `0x5055` | `0x5088` | 65% Alice |
| **STELLAR (ABM081)** | CIDOO | `0x5055` | `0x5088` | 75% |
| **AK068 / AKS068 Pro** | Ajazz / Attack Shark / MambaSnake | `0x5055` | `0x5088` | 65% |
| **MQ80** | IQUNIX | `0x5055` | — | 75% |
| **MG65** | IQUNIX | `0x5055` | — | 65% |
| **EK21** | EPOMAKER | `0x5055` | — | Numpad |

**The PID `0x5055` is shared across at least 6 different keyboards from 5 different brands.** VIA cannot auto-detect which keyboard is connected — users must manually load the correct JSON definition. PID `0x5088` identifies the 2.4G wireless dongle mode (also shared).

## Platform Fingerprint

### Identical Across All Products (Platform Constants)

1. **VID `0x320F`** — Evision vendor ID
2. **PID `0x5055` (USB) / `0x5088` (2.4G)** — connection-mode PIDs, NOT product IDs
3. **Matrix: 8 rows x 16 columns** — platform maximum, regardless of physical key count
4. **USB manufacturer string: `"RDR"`** — hardcoded in firmware template
5. **19 RGB effect names** in exact order (see below)
6. **5 core wireless keycodes**: `2.4G MODE`, `Bluetooth 1/2/3`, `LOCK WIN KEY`
7. **Chinglish phrasing** in keycode titles ("Bluetooth working connection 1", etc.)

### Per-Product Customization

1. Product name (`"Rainy 75"`, `"AKS068"`, `"Alice 68"`, etc.)
2. Additional custom keycodes (product-specific features like LOGO LED, OLED, battery display)
3. Physical key layout (positions, count, sizes — different per PCB)
4. VIA JSON format (V2 `lighting.extends` vs V3 `menus` — depends on when JSON was authored)
5. Which matrix rows/columns are actually wired

### RGB Effect Names (Evision Platform Enum)

These 19 effect names are the **strongest fingerprint** — identical in name, order, and numbering across all products:

| ID | Name | Description |
|----|------|-------------|
| 0 | `OFF_MODE` | LEDs off |
| 1 | `WAVE_MODE` | Color wave |
| 2 | `COLOUR_CLOUD_MODE` | Color cloud |
| 3 | `VORTEX_MODE` | Spiral vortex |
| 4 | `MIX_COLOUR_MODE` | Mixed colors |
| 5 | `BREATHE_MODE` | Breathing |
| 6 | `LIGHT_MODE` | Solid static |
| 7 | `SLOWLY_OFF_MODE` | Slow fade out |
| 8 | `STONE_MODE` | Stone effect |
| 9 | `LASER_MODE` | Laser sweep |
| 10 | `STARRY_MODE` | Starry sky |
| 11 | `FLOWERS_OPEN_MODE` | Blooming flowers |
| 12 | `TRAVERSE_MODE` | Traverse |
| 13 | `WAVE_BAR_MODE` | Wave bar |
| 14 | `METEOR_MODE` | Meteor shower |
| 15 | `RAIN_MODE` | Rain drops |
| 16 | `SCAN_MODE` | Scanning |
| 17 | `TRIGGER_COLOUR_MODE` | Reactive typing |
| 18 | `CENTER_SPREAD_MODE` | Center spread |

## Protocol Branches

The Evision ecosystem has at least **two distinct protocol branches**:

### VIA V3 Branch (VID `0x320F`)
- Used by: Rainy 75, CIDOO, IQUNIX, EPOMAKER
- Protocol: Standard VIA V3 (usage page `0xFF60`)
- Configuration: VIA app with per-product JSON definitions
- 4 user-accessible layers, 16 macros, 512B macro buffer

### Proprietary Branch (VID `0x3151` — RongYuan)
- Used by: Attack Shark / Ajazz via GearHub (qmk.top)
- Protocol: Proprietary HID Feature Reports
- Transport: 9-byte commands with checksum, 64-byte OTA packets
- Configuration: GearHub web app (qmk.top)

**Note:** Some products (e.g., AKS068) appear in BOTH branches — with VIA JSON definitions AND GearHub support. The Attack Shark AKS068 has VIA JSONs on GitHub but is also listed in GearHub under VID `0x3151`, suggesting some keyboards ship with dual-protocol firmware or different firmware variants per market.

## GearHub Protocol (qmk.top) — Extracted from JavaScript

The GearHub web app is a 2.4MB Vite/Vue.js SPA with the main bundle at `js/index.1c916957.js`. Device-specific protocol methods are in ~700+ lazy-loaded Webpack chunks.

### Transport Layer

| Aspect | Detail |
|--------|--------|
| Transport | HID Feature Reports (USB), prefixed with `0x55` for BT |
| Packet size | 9 bytes (commands), 64 bytes (OTA) |
| Checksum | `byte[7] = 255 - (sum(bytes[0..6]) & 0xFF)` |

### Command Reference

| Byte | Name | Purpose |
|------|------|---------|
| `0x8F` | getNormalID | Device ID (uint32 at bytes[1-4]) |
| `0xF1` | getDongleID | Dongle ID (uint16) |
| `0xF7` | check24GStatus | Dongle status (battery, online, device type) |
| `0xF6` | select24GDevice | Select device (kb=10, mouse=5, both=13) |
| `0x77` | checkStatus | BT heartbeat |
| `0xBA` | firmware upgrade | Sub: `0xC0`=start, `0xC2`=verify |
| `0x7F` | boot_usb | `[0x7F,0x55,0xAA,0x55,0xAA,0,0]` — enter USB OTA bootloader |
| `0xF8` | boot_rf | `[0xF8,0x55,0xAA,0x55,0xAA,0,0]` — enter RF OTA bootloader |

### Matrix Keycode Encoding (4-byte)

| byte[0] | Type | Example |
|---------|------|---------|
| 0 | HID keycode / combo | Standard keys |
| 1 | Mouse button | `[1,0,0xF0,0]` = left click |
| 3 | Consumer/media | `[3,0,0xCD,0]` = play/pause |
| 9 | Macro | byte[1]=type, byte[2]=index |
| 10 | Fn key | `[10,1,0,0]` = Fn |
| 11 | Fire key | `[11,0,0,0]` |
| 21 | Gamepad | |
| 22 | Snap/recoil control | byte[1]: 4=normal, 6=keep, 7=toggle |
| 24 | Mod-tap | byte[1]=key1, byte[2]=key2, byte[3]=time/10 |

## VIA Bug: Incorrect Response on DYNAMIC_KEYMAP_SET_BUFFER

Confirmed on EPOMAKER EK21 (GitHub issue `the-via/keyboards#2237`): the Evision firmware returns `0x01` as the first response byte instead of echoing the command ID (`0x13` for DYNAMIC_KEYMAP_SET_BUFFER). This is a platform-wide bug affecting saved keymap loading in VIA.

```
Command:  19 0 0 28 0 41 95 16 0 43 ...
Response:  1 0 9 28 0 41 95 16 0 43 ...
           ^-- should be 19 (0x13)
```

This affects all Evision VIA keyboards when loading saved keymap JSON files. Manual key changes work because they use different VIA commands.

## Supply Chain

```
Telink Semiconductor (Shanghai)
  └─ TLSR9511 B91 RISC-V SoC + BLE SDK (open source base layer)
      │
Evision Semiconductor (Shenzhen)
  └─ Custom-brands chip, layers proprietary keyboard firmware
  └─ Provides turnkey solution: firmware + driver software + MCU
      │
KeebMonkey / KBM Gadgets (Shenzhen, founded 2019)
  ├─ WOB brand (China domestic: Tmall, JD, Douyin) → woblab.cn
  ├─ WOBKEY brand (international) → wobkey.com
  └─ Web config: wobwxe.com ("WOB驱动")
      │
Firmware hosting:
  ├─ China: driveall.oss-cn-hangzhou.aliyuncs.com (Alibaba Cloud)
  └─ International: drivers.sfo3.digitaloceanspaces.com (DigitalOcean)
```

## Developer initials: "YKQ"

- A repeated 16-byte developer-initials string at firmware offset `0x1aed3`
- Likely an **AES-128 key or device identifier**, not a debug string
- **Zero public presence** — no GitHub, Gitee, CSDN, LinkedIn, Bilibili profiles
- Almost certainly an internal Evision engineer

## Paths to Source Code

Since no public source exists:

1. **Web config tool JavaScript** (wobwxe.com) — contains full protocol implementation in client-side JS. Now reachable directly (previously had connectivity issues from Europe). Full analysis completed — see [wob-driver-analysis.md](wob-driver-analysis.md).

2. **Telink B91 BLE SDK** (open source) — provides the entire base layer. Combined with our 211 fully-named decompiled functions, reconstructing functional source is feasible.

3. **Binary diffing** — fetch OTA packages from sibling keyboards and diff against Rainy 75 to isolate platform vs. per-product code.

4. **SonixQMK firmware dumps** — the SonixQMK community can extract firmware from Evision ARM-based keyboards. Cross-comparing with our RISC-V binary reveals shared algorithms.

5. **Direct engagement with Evision** — B2B inquiry for SDK access under appropriate terms.

## Related Resources

| Resource | URL |
|----------|-----|
| AKS068 VIA JSON (xero) | github.com/xero/aks068-via |
| AKS068 VIA JSON (vzhny) | github.com/vzhny/aks068-via |
| VIA JSON collection (since19861019) | github.com/since19861019/via-json |
| EK21 VIA bug report | github.com/the-via/keyboards/issues/2237 |
| CIDOO ABM066 VIA JSON | epomaker.com/blogs/via-json/cidoo-abm066-usb-via-json-file |
| GearHub (Attack Shark) | qmk.top |
| WOB Driver (Wobkey) | wobwxe.com |
| SonixQMK KB Database | github.com/SonixQMK/Mechanical-Keyboard-Database |
| Evision VID lookup | the-sz.com/products/usbid/index.php?v=0x320F |
