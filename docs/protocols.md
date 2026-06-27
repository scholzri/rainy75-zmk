# Communication Protocols

## USB HID OTA Protocol

Reverse-engineered from the official .NET flasher (`KM_USB_mode_ota_pc_v5.0.2`) via ILSpy decompilation.
Protocol is standard Telink OTA over USB HID — **no encryption, no signatures**.

### Device Discovery

| Parameter | Value |
|-----------|-------|
| VID | `0x320F` |
| PID | `0x5055` |
| USB Interface | 2 (NKRO + multi-function) |
| HID Usage Page | `0xFFEF` |
| Report ID | `0x05` |
| hidraw (Linux) | `/dev/hidraw3` (varies) |

### Packet Format

All packets sent via HID output report (SET_REPORT on EP0), Report ID 0x05:

```
[report_id=0x05] [cmd_type] [payload_len] [0x00] [payload...] [0xFF padding to 64 bytes]
```

**Critical:** Unused bytes MUST be padded with `0xFF` (not `0x00`). The .NET flasher fills the entire 64-byte buffer with `0xFF` before writing command data. Using `0x00` padding causes OTA END to fail with error `0x0B`.

Interface 2 has only EP2 IN (no OUT endpoint), so writes use SET_REPORT control transfers on EP0 — not interrupt OUT.

### Commands

| cmd_type | Purpose | Payload |
|----------|---------|---------|
| 0x01 | Get FW Version | `[0x00, 0x00]` — response: version(4) + CRC(4) |
| 0x02 | OTA Start | `[0x02, 0x00, 0x01, 0xFF]` |
| 0x02 | OTA Data | `[len, 0x00, segments...]` — up to 3 segments per packet |
| 0x02 | OTA End | `[0x06, 0x00, 0x02, 0xFF, last_idx(2), complement(2)]` |

**OTA End complement:** Two's complement of last segment index: `(0xFFFF - last_idx + 1) & 0xFFFF`. This differs from the Telink SDK which uses one's complement (`last_idx ^ 0xFFFF`). Confirmed by decompiling the .NET flasher: `(ushort)(65535 - num3 + 1)`.

### OTA Data Segment (20 bytes each, 3 per packet = 60 bytes max)

```
[index_lo, index_hi] [firmware_data x 16] [crc16_lo, crc16_hi]
```

- `index`: LE u16, sequential segment counter (each = 16 bytes of firmware)
- `firmware_data`: 16 bytes from `firmware[index * 16]`, padded with 0xFF at end
- `crc16`: CRC16 over [index(2) + data(16)] = 18 bytes

**CRC16:** Polynomial 0xA001 (reflected CRC-CCITT), initial value 0xFFFF.

### Firmware Size Constraints

- **256KB hard limit**: OTA handler writes to the secondary flash bank (0x40000–0x7FFFF). Firmware must be ≤256KB.
- **16-byte alignment required**: The firmware body (everything before the 4-byte CRC32 trailer) must be a multiple of 16 bytes. Unaligned images cause OTA END to fail with error `0x0B` (OTA_FW_SIZE_ERR). Pad with 0xFF to align.
  - Example: original firmware body = 120,144 bytes = 7509 × 16 (aligned)
  - Unaligned body → error 0x0B; pad to next multiple of 16 → success

### Responses

| Response bytes | Meaning |
|----------------|---------|
| `[05, 01, 08, 00, ver(4), crc(4)]` | Firmware version query |
| `[05, 02, 03, 00, 06, FF, 00]` | OTA success |
| `[05, 02, 03, 00, 06, FF, N]` | OTA failure (N = error code) |

### OTA Error Codes (from Telink SDK `ota.h`)

| Code | Name | Meaning |
|------|------|---------|
| 0x00 | SUCCESS | OTA completed successfully |
| 0x01 | FW_VERSION_ERR | Firmware version mismatch |
| 0x02 | FW_CHECK_ERR | Firmware check/validation failed |
| 0x03 | FW_CRC_ERR | CRC error during data transfer |
| 0x04 | FW_WRITE_ERR | Flash write failed |
| 0x05 | DATA_INCOMPLETE | Data transfer incomplete |
| 0x07 | FW_CHECK_ERR2 | Secondary firmware check fail |
| 0x0B | FW_SIZE_ERR | Firmware size error (too small/large, or not 16-byte aligned) |
| 0x0D | TIMEOUT | OTA operation timed out |

### OTA Flow

Start → send all segments (device ACKs each packet) → End → device validates & reboots.

Live test: `--version` query returned FW version 0x00000000, CRC 0x3BF0F8E5.

### Verified OTA → ZMK Installation Path (EVK-free)

Complete path from stock Evision firmware to ZMK, no SWS hardware required:

1. **Stock firmware** (VID 320F:5055) running on keyboard
2. `python3 reverse/tools/prepare_ota.py build-bridge/combined.bin` → creates aligned OTA image
3. `python3 reverse/tools/ota_flasher.py build-bridge/combined_ota.bin` → OTA flash (~24s)
4. Keyboard reboots as "Rainy 75 Bridge" (VID 1D50:615E) with USB CDC ACM + mcumgr
5. `mcumgr image upload build/zephyr/zmk.signed.bin` → uploads full ZMK app (~82s)
6. `mcumgr image test <hash>` → marks for swap
7. `mcumgr reset` → MCUboot swaps images, keyboard boots as "Rainy 75 Pro"

The bridge firmware is a minimal ZMK build (MCUboot + USB + mcumgr, no BLE/RGB) that fits within the 256KB OTA limit. The full ZMK app (with BLE, RGB, battery, etc.) is then uploaded via mcumgr DFU over USB serial.

### Firmware Packaging (inside .exe resource `code_2M`)

- 256-byte wrapper header: 48 bytes 0x56 padding + 4-byte code size (big-endian) + 0xFF padding
- OTA payload starts at wrapper offset 256
- OTA payload has standard Telink format: boot vector at 0x00, `TLNK` magic at 0x20, size at 0x18
- Total OTA size: 120,148 bytes (flash 0x00000-0x1D553)
- Boot header: 4,784 bytes (0x0000-0x12AF) — boot vector + TLNK header + startup code
- `firmware_extracted.bin` = OTA payload starting at offset 0x12B0 (after Telink boot header), 115,364 bytes
- `firmware_ota.bin` = complete OTA payload (boot header + firmware body), byte-exact match with flash dump
- Dual-bank flash: primary `0x0`, secondary `0x40000`, boot flag at offset `0x20`

### `param_128K` Resource (configuration)

| Offset | Content |
|--------|---------|
| 0-47 | 0x55 padding |
| 48-63 | Encryption key: `112233445566778899aabbccddeeff00` (placeholder, NOT used) |
| 64-67 | VID: `"320F"` |
| 68-71 | PID: `"5055"` |
| 72-73 | Report ID: `"05"` |
| 80+ | Usage Page: `"FFEF"` |
| 256+ | Window title: `"Mouse_USB_OTA"` |

Linux OTA flasher: `reverse/tools/ota_flasher.py` — uses hidraw directly, no dependencies.

### Official Firmware Update Procedure (from Wobkey wiki)

1. Turn **OFF** the keyboard (battery switch under CapsLock)
2. Connect via USB-C cable
3. Switch to **Wired Mode** (Fn+Tab until ESC indicator)
4. Open the downloaded `.exe` firmware file
5. Press **Start**
6. Wait for "Pass" / "Success" — do NOT unplug until 100%

**Firmware v20240121 changelog:**
- Added Long Battery Life Mode (Fn+L toggle)
- Removed ESC charging indicator (red flash)
- Moved charging indicator to Fn+Space with updated animations

**Official recommendation:** do NOT update firmware if keyboard is working correctly.

### SDK vs Firmware OTA

The Telink B91 SDK's OTA implementation targets **BLE GATT**, not USB HID. The Rainy 75 uses a **custom USB HID OTA handler** not found in the public SDK.

---

## VIA Protocol

Probed via hidraw (Usage Page `0xFF60`), all commands responded correctly.

| Parameter | Value |
|-----------|-------|
| VIA Protocol Version | 11 (0x0B) — VIA V3 |
| Layer Count | 4 |
| Macro Count | 16 |
| Macro Buffer Size | 512 bytes |

**VIA JSON configuration files** — 4 variants exist (RGB/Non-RGB × Wired/2.4G):

| Model | Connection | Local Copy |
|-------|-----------|------------|
| RGB (Standard / Pro) | Wired | `reverse/reference/via_iso_usb/` |
| RGB (Standard / Pro) | 2.4G | `reverse/reference/via_iso_2.4g/` |
| Non-RGB (Lite) | Wired | (not obtained) |
| Non-RGB (Lite) | 2.4G | (not obtained) |

Load via [usevia.app](https://usevia.app) → Settings → enable "Show Design Tab" → Design tab → load JSON file.

### Command Responses

| VIA cmd | Response | Meaning |
|---------|----------|---------|
| `0x01` | `01 00 0b` | Protocol version = 11 |
| `0x02` | `02 03` | get_keyboard_value |
| `0x05` | `05 00` | keymap_reset ack |
| `0x0C` | `0c 10` | macro_get_count = 16 |
| `0x0D` | `0d 02 00` | macro_buffer_size = 512 |
| `0x11` | `11 04` | layer_count = 4 |
| `0x16`+ | `01 00 09` | Unrecognized (fallback) |

All 256 VIA commands tested with param=0x00. Re-tested with param=0x01 — see Extended Probe below.

### VIA Command Handler — Firmware Internals

The VIA dispatch function `via_command_handler` @ 0x2000c12c (1074 bytes) uses a 212-entry switch table at 0x2001a91c. Key internal details:

- `via_get_protocol_version()` returns version **4** internally (reported as VIA V3, protocol 11 over HID)
- `via_get_layer_count()` returns **9** internally (but only 4 exposed via SET_BUFFER bounds check)
- SET_BUFFER flash layout: Layer 0 → `0x84000`, Layer 1 → `0x85000`, Layer 2 → `0x86000`, Macros → `0x89000`
- GET_BUFFER reads from `0x84F00 + offset` (VIA data region)
- Writes accumulate in SRAM (256B chunks), flushed via `flash_write_chunked()` when buffer > 0xE00
- Response sent via `usb_endpoint_send(4, gp+0x380, 32)` (USB) or flag `gp+0x2DA = 2` (BLE)
- 5 "extra" internal layers (4-8) likely represent mode overlays (WIN/MAC/wireless variants)

See [firmware-analysis.md](firmware-analysis.md) for full switch table mapping.

### Extended VIA Probe Results (2025-02)

Deep probe of all 256 commands with param=0x01, plus targeted parameter sweeps.

#### Undocumented Commands

| VIA cmd | Response | Behavior |
|---------|----------|----------|
| `0x14` | `14 pp pp pp 00 00` | Recognized handler (echoes cmd byte) but returns zeros — deprecated stub |
| `0x15` | `15 pp 00 00 ...` | Recognized handler (echoes cmd byte) but returns zeros — deprecated stub |
| `0x16`-`0xFF` | `01 01 09 01 ...` | All return identical default fallthrough — not real handlers |

**cmd 0x14 / 0x15 analysis:**
- Both echo back their command byte, confirming they are recognized handlers (unrecognized cmds return `01 01 09 01`)
- In isolation, both consistently return all zeros regardless of parameters
- During heavy probing sessions (rapid sequential commands), byte[5] occasionally shows stale buffer values (`0xA9`/`0xAA`) leaked from prior GET_KEYCODE/GET_BUFFER responses — the firmware reuses the response buffer without fully clearing it
- Likely deprecated **lighting_set_value / lighting_get_value** stubs from an older VIA protocol revision (superseded by CUSTOM_SET/GET_VALUE at 0x09/0x08)
- Wireless switch toggle has no effect on responses — confirmed by controlled test (20 samples each, ON vs OFF, all zeros)
- Not useful for reading hardware state or keymap data

#### CUSTOM_GET_VALUE (0x08) — RGB Lighting Config

Only channel 3 responds. Exhaustive scan of all 8 channels x 64 IDs:

| Channel | ID | Value | Meaning |
|---------|-----|-------|---------|
| 3 | 1 | `0x09` | RGB brightness (0-9, maps to VIA slider `id_qmk_rgb_matrix_brightness`) |
| 3 | 2 | `0x11` (17) | RGB effect mode (0-18, see effect list below) |
| 3 | 3 | `0x04` | RGB animation speed |
| 3 | 4 | `0x0E, 0xFF` | RGB hue (14) + saturation (255) — 2-byte value |

No other channels or IDs return data. Channels 0-2 and 4-7 are completely empty.

**RGB config is read-only via VIA.** CUSTOM_SET_VALUE (0x09) is acknowledged (echoes cmd byte `09`) but silently discards the value — read-back shows no change. CUSTOM_SAVE (0x0A) is not implemented (falls through to default handler, returns `01 03 09`). RGB settings can only be changed via physical Fn+key combos.

Additionally, a **master RGB enable/disable toggle** (Fn+Backspace) exists as a separate SRAM flag not exposed via any CUSTOM_GET_VALUE ID — the 4 visible config values remain unchanged when toggled.

**RGB physical key combos (from manual):**

| Combo | Function |
|-------|----------|
| Fn+Backspace | ON/OFF backlight (master toggle) |
| Fn+Enter | Cycle backlight mode (MODE 1→18→OFF) |
| Fn+\\| | Switch color |
| Fn+↑ | Increase brightness |
| Fn+↓ | Decrease brightness |
| Fn+← | Slow down lighting speed |
| Fn+→ | Speed up lighting speed |

RGB effect modes (firmware ID = VIA ch3/id2 value):

**Fn+Enter cycle is sequential: OFF(0) → WAVE(1) → ... → CENTER_SPREAD(18) → OFF(0).** Confirmed by reading ch3/id2 via VIA while cycling with physical Fn+Enter. The manual's "MODE 1-18" printed numbering uses a different display order and does NOT correspond to firmware IDs.

| FW ID | FW Name | Description (from manual) |
|-------|---------|---------------------------|
| 0 | OFF | Lights off |
| 1 | WAVE | Wave lighting effect |
| 2 | COLOUR_CLOUD | Single color gradually transitioning |
| 3 | VORTEX | Lighting rotates around center of keyboard |
| 4 | MIX_COLOUR | Lighting moving horizontally in cloud-like shape |
| 5 | BREATHE | Color changing in breathing rhythm |
| 6 | LIGHT | Full keyboard backlight ON (solid, static) |
| 7 | SLOWLY_OFF | Keypress illuminates then gradually dims |
| 8 | STONE | Multicolored or monochromatic blinking |
| 9 | LASER | Colorful gradual lighting transitions |
| 10 | STARRY | Randomly generated lighting combinations |
| 11 | FLOWERS_OPEN | Wings effect, swinging up and down |
| 12 | TRAVERSE | Interweaving row by row left to right |
| 13 | WAVE_BAR | (not described in manual) |
| 14 | METEOR | Raindrop/meteor effect |
| 15 | RAIN | (not described in manual) |
| 16 | SCAN | Sways left-right, speed tracks keystroke speed |
| 17 | TRIGGER_COLOUR | Keypress generates spreading light effect |
| 18 | CENTER_SPREAD | Expands/contracts from center, speed tracks keystroke speed |

#### Hidden Layer Scan

GET_BUFFER reads from `flash_base + 0x84F00 + offset` with no bounds checking on the read path.
Scanned 12 layers (3072 bytes, offsets 0x0000-0x0C00):

| Region | Flash Address | Content |
|--------|--------------|---------|
| Layers 0-3 (0x0000-0x0400) | `0x84F00-0x85300` | Valid keymap data (4 standard VIA layers) |
| Layer 4 (0x0400-0x0500) | `0x85300-0x85400` | 6x `0xFFFF` at R0C0-R0C5, rest zeroed (partial/vestigial) |
| Layers 5-11 (0x0500-0x0C00) | `0x85400-0x85B00` | All zeros |
| Extended (0x0C00-0x5000) | `0x85B00-0x89F00` | All zeros (GET_BUFFER samples) |

SET_BUFFER is bounded to offset < 0x400 (firmware case 0x38). Writes to offset >= 0x400 are silently rejected.
**No writable hidden layers exist.**

#### GET_KEYCODE Extended Layer Scan

`dynamic_keymap_get_keycode` (0x11) tested for layers 0-8. All layers return `0x0000` for position [0,0],
which is correct (R0C0 = KC_NO / unused on this layout). No hidden keycode data on layers 4-8.

#### Macro Buffer

Macro buffer (512 bytes reported) is completely empty — no macros configured.
Reading beyond the 512-byte boundary returns zeros — no hidden macro data.

#### Special Keycodes in Keymap

| Keycode | USB HID | Meaning | Location |
|---------|---------|---------|----------|
| `0x00A9` | Consumer Mute | KC_AUDIO_MUTE | R7C1 (all layers) |
| `0x00AA` | Consumer Vol+ | KC_AUDIO_VOL_UP | R7C0 (all layers) |
| `0x0087` | KB International 1 | KC_INT1 / KC_RO — ghost key for JIS layout (no physical key on ISO) | R4C12 (all layers, under 2u RSFT) |
| `0x7820-0x782A` | N/A | RGB control keycodes | Layer 1 (Fn layer) |
| `0xC1xx` | N/A | Consumer page keycodes (media) | Layer 1 |
| `0xFFFF` | N/A | Transparent (inherit from lower layer) | Fn layer unused positions |

---

## HID Command Probe Results

All keyboard HID interfaces probed for hidden/undocumented commands.

**Interface 2 (0xFFEF) — Vendor channel:**
- cmd `0x01`: returns FW version — known
- cmd `0x00`, `0x03`-`0x1F`: echo or timeout — no hidden functionality
- **No memory read, flash dump, or debug commands exposed**

**Interface 3 (0xFF1C) — WOB proprietary channel:**
- Completely silent on Report ID 4 — no response to any protocol variant
- The wobwxe.com (WOB驱动) web driver lists our keyboard (0x320F:0x5055) on this interface with `usagePage=0xFF1C, usage=0x92, reportId=4`
- WOB driver uses **three distinct protocols** depending on keyboard model (see [wob-driver-analysis.md](wob-driver-analysis.md)):
  - **Protocol A (Address-based):** Direct SRAM/flash read/write with 0xBE framing — for RT hall-effect keyboards
  - **Protocol B (Command-ID):** Logical command IDs 2-75 — for Rainy 98
  - **Protocol C (Packet-based):** Pre-defined packet sequences (AllDataCom, keyMapCom) — for our standard Rainy 75
- **All three protocols tested, all silent:**
  - 0xBE-framed: heartbeat `[BE 00 05 00 ED]`, FW status `[BE 74 05 00 ED]`, ReadRam, ReadFlash — no response
  - Protocol C: begin `[01 00 01]`, keyMapCom reads `[3F 00 07 38 00 00 00]` — no response
  - Both padded (64B) and exact-length packets — no response
- Same 0xBE packets sent on Interface 2 (Report ID 5) are **echoed back verbatim** — firmware receives but doesn't process them
- Tools: `reverse/tools/mem_reader.py` (Protocol A), `reverse/tools/wob_probe.py` (Protocol C)

**0xBE Memory Protocol (RT variant only):**
- Discovered in wobwxe.com JS (live fetch 2026-02-17, also in Wayback Machine 2025-07-19)
- Packet: `[0xBE, cmd, len_lo, len_hi, addr LE 4B, datalen_hi, datalen_lo, 0xED]`
- Commands: `0x01`=ReadFlash, `0x02`=Save, `0x03`=SetRam, `0x04`=ReadRam, `0xFC`=Download, `0xFF`=Reset
- CRC-16/MODBUS checksum before end marker (for write commands)
- Addresses are direct Telink B91 SRAM (`0x8007xxxx`–`0x8008xxxx`) or flash (`0x0`)

**Conclusion:** USB HID firmware dump is **not possible on the standard Rainy 75 firmware** — OTA is write-only. The WOB driver's device table includes our keyboard for Protocol C, but the firmware has no handler for Interface 3 input reports. Likely the Protocol C handler was planned but not shipped in the current firmware (version 0x00000000). SWS via Burning EVK remains the only path for reading flash contents on the standard model.
