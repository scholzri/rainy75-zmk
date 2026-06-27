#!/usr/bin/env python3
"""Dump the full keymap from the Rainy 75 via VIA protocol."""

import os
import sys
import struct
import select

REPORT_SIZE = 32
VIA_DYNAMIC_KEYMAP_GET_BUFFER = 0x12
VIA_DYNAMIC_KEYMAP_GET_LAYER_COUNT = 0x11

# QMK/VIA keycodes (subset - common ones)
KEYCODES = {
    0x0000: "KC_NO",
    0x0001: "KC_TRANSPARENT",
    0x0004: "KC_A", 0x0005: "KC_B", 0x0006: "KC_C", 0x0007: "KC_D",
    0x0008: "KC_E", 0x0009: "KC_F", 0x000A: "KC_G", 0x000B: "KC_H",
    0x000C: "KC_I", 0x000D: "KC_J", 0x000E: "KC_K", 0x000F: "KC_L",
    0x0010: "KC_M", 0x0011: "KC_N", 0x0012: "KC_O", 0x0013: "KC_P",
    0x0014: "KC_Q", 0x0015: "KC_R", 0x0016: "KC_S", 0x0017: "KC_T",
    0x0018: "KC_U", 0x0019: "KC_V", 0x001A: "KC_W", 0x001B: "KC_X",
    0x001C: "KC_Y", 0x001D: "KC_Z",
    0x001E: "KC_1", 0x001F: "KC_2", 0x0020: "KC_3", 0x0021: "KC_4",
    0x0022: "KC_5", 0x0023: "KC_6", 0x0024: "KC_7", 0x0025: "KC_8",
    0x0026: "KC_9", 0x0027: "KC_0",
    0x0028: "KC_ENTER", 0x0029: "KC_ESCAPE", 0x002A: "KC_BACKSPACE",
    0x002B: "KC_TAB", 0x002C: "KC_SPACE",
    0x002D: "KC_MINUS", 0x002E: "KC_EQUAL",
    0x002F: "KC_LBRACKET", 0x0030: "KC_RBRACKET",
    0x0031: "KC_BACKSLASH", 0x0032: "KC_NONUS_HASH",
    0x0033: "KC_SEMICOLON", 0x0034: "KC_QUOTE",
    0x0035: "KC_GRAVE", 0x0036: "KC_COMMA", 0x0037: "KC_DOT",
    0x0038: "KC_SLASH",
    0x0039: "KC_CAPS_LOCK",
    0x003A: "KC_F1", 0x003B: "KC_F2", 0x003C: "KC_F3", 0x003D: "KC_F4",
    0x003E: "KC_F5", 0x003F: "KC_F6", 0x0040: "KC_F7", 0x0041: "KC_F8",
    0x0042: "KC_F9", 0x0043: "KC_F10", 0x0044: "KC_F11", 0x0045: "KC_F12",
    0x0046: "KC_PRINT_SCREEN", 0x0047: "KC_SCROLL_LOCK", 0x0048: "KC_PAUSE",
    0x0049: "KC_INSERT", 0x004A: "KC_HOME", 0x004B: "KC_PAGE_UP",
    0x004C: "KC_DELETE", 0x004D: "KC_END", 0x004E: "KC_PAGE_DOWN",
    0x004F: "KC_RIGHT", 0x0050: "KC_LEFT", 0x0051: "KC_DOWN", 0x0052: "KC_UP",
    0x0053: "KC_NUM_LOCK",
    0x0054: "KC_KP_SLASH", 0x0055: "KC_KP_ASTERISK", 0x0056: "KC_KP_MINUS",
    0x0057: "KC_KP_PLUS", 0x0058: "KC_KP_ENTER",
    0x0059: "KC_KP_1", 0x005A: "KC_KP_2", 0x005B: "KC_KP_3",
    0x005C: "KC_KP_4", 0x005D: "KC_KP_5", 0x005E: "KC_KP_6",
    0x005F: "KC_KP_7", 0x0060: "KC_KP_8", 0x0061: "KC_KP_9",
    0x0062: "KC_KP_0", 0x0063: "KC_KP_DOT",
    0x0064: "KC_NONUS_BACKSLASH",  # ISO key (< > |)
    0x0065: "KC_APPLICATION",
    0x00E0: "KC_LCTRL", 0x00E1: "KC_LSHIFT", 0x00E2: "KC_LALT",
    0x00E3: "KC_LGUI", 0x00E4: "KC_RCTRL", 0x00E5: "KC_RSHIFT",
    0x00E6: "KC_RALT", 0x00E7: "KC_RGUI",
    # Media keys (consumer page - 0x00A0+)
    0x00A5: "KC_MEDIA_PLAY_PAUSE",
    0x00A6: "KC_MEDIA_STOP",
    0x00A7: "KC_MEDIA_PREV",
    0x00A8: "KC_MEDIA_NEXT",
    0x00A9: "KC_AUDIO_MUTE",
    0x00AA: "KC_AUDIO_VOL_UP",
    0x00AB: "KC_AUDIO_VOL_DOWN",
}

# QMK modifier combos and special keycodes
def decode_keycode(kc):
    """Decode a QMK/VIA keycode to a human-readable string."""
    if kc in KEYCODES:
        return KEYCODES[kc]

    # MO(layer) = 0x5220 + layer
    if 0x5220 <= kc <= 0x522F:
        return f"MO({kc - 0x5220})"

    # TG(layer) = 0x5240 + layer
    if 0x5240 <= kc <= 0x524F:
        return f"TG({kc - 0x5240})"

    # TO(layer) = 0x5200 + layer
    if 0x5200 <= kc <= 0x520F:
        return f"TO({kc - 0x5200})"

    # LT(layer, kc) = 0x4000 + (layer << 8) + kc
    if 0x4000 <= kc <= 0x4FFF:
        layer = (kc >> 8) & 0x0F
        base_kc = kc & 0xFF
        base_name = KEYCODES.get(base_kc, f"0x{base_kc:02X}")
        return f"LT({layer},{base_name})"

    # Modifier + key combos
    # LCTL(kc) = 0x0100 + kc, LSFT(kc) = 0x0200 + kc, etc.
    mod_masks = {
        0x0100: "LCTL", 0x0200: "LSFT", 0x0400: "LALT", 0x0800: "LGUI",
        0x1100: "RCTL", 0x1200: "RSFT", 0x1400: "RALT", 0x1800: "RGUI",
    }
    for mask, name in mod_masks.items():
        if (kc & 0xFF00) == mask:
            base = kc & 0xFF
            base_name = KEYCODES.get(base, f"0x{base:02X}")
            return f"{name}({base_name})"

    # Custom keycodes (0x7E00+)
    if kc >= 0x7E00:
        custom_id = kc - 0x7E00
        custom_names = {
            # Based on VIA JSON analysis
        }
        return f"CUSTOM(0x{custom_id:04X})"

    return f"0x{kc:04X}"


def via_send_recv(fd, cmd_bytes, timeout=1.0):
    buf = bytes(cmd_bytes) + b'\x00' * (REPORT_SIZE - len(cmd_bytes))
    os.write(fd, buf)
    ready, _, _ = select.select([fd], [], [], timeout)
    if ready:
        return os.read(fd, REPORT_SIZE)
    return None


def find_via_device():
    for hr_num in range(20):
        hr_path = f"/dev/hidraw{hr_num}"
        rdesc_path = f"/sys/class/hidraw/hidraw{hr_num}/device/report_descriptor"
        try:
            with open(rdesc_path, 'rb') as f:
                desc = f.read()
            if desc[:3] == bytes([0x06, 0x60, 0xFF]):
                return hr_path
        except (OSError, FileNotFoundError):
            continue
    return None


def main():
    via_device = find_via_device()
    if not via_device:
        print("ERROR: Could not find VIA hidraw device!")
        sys.exit(1)

    print(f"VIA device: {via_device}")
    fd = os.open(via_device, os.O_RDWR | os.O_NONBLOCK)

    try:
        # Get layer count
        resp = via_send_recv(fd, [VIA_DYNAMIC_KEYMAP_GET_LAYER_COUNT])
        layer_count = resp[1] if resp else 4
        print(f"Layer count: {layer_count}")

        # Matrix dimensions from VIA JSON: 8 rows x 16 cols
        rows = 8
        cols = 16
        keys_per_layer = rows * cols  # 128 keys
        bytes_per_layer = keys_per_layer * 2  # 256 bytes (2 bytes per keycode)
        total_bytes = bytes_per_layer * layer_count

        print(f"Matrix: {rows} rows x {cols} cols = {keys_per_layer} keys/layer")
        print(f"Total keymap: {total_bytes} bytes ({layer_count} layers)")
        print()

        # Read entire keymap buffer
        keymap_data = bytearray()
        chunk_size = 28  # Max data bytes per VIA read (32 - 4 header)
        offset = 0

        while offset < total_bytes:
            read_size = min(chunk_size, total_bytes - offset)
            resp = via_send_recv(fd, [
                VIA_DYNAMIC_KEYMAP_GET_BUFFER,
                (offset >> 8) & 0xFF,
                offset & 0xFF,
                read_size
            ])
            if resp:
                keymap_data.extend(resp[4:4 + read_size])
            else:
                print(f"  ERROR: No response at offset {offset}")
                keymap_data.extend(b'\x00' * read_size)
            offset += read_size

        # Save raw keymap
        with open("keymap_dump.bin", "wb") as f:
            f.write(keymap_data)
        print(f"Raw keymap saved to keymap_dump.bin ({len(keymap_data)} bytes)")
        print()

        # Decode and display each layer
        for layer in range(layer_count):
            print(f"{'='*80}")
            print(f"  LAYER {layer}")
            print(f"{'='*80}")
            layer_offset = layer * bytes_per_layer

            for row in range(rows):
                row_keycodes = []
                for col in range(cols):
                    kc_offset = layer_offset + (row * cols + col) * 2
                    if kc_offset + 1 < len(keymap_data):
                        kc = struct.unpack('>H', keymap_data[kc_offset:kc_offset+2])[0]
                    else:
                        kc = 0
                    row_keycodes.append(kc)

                # Format row
                names = [decode_keycode(kc) for kc in row_keycodes]
                hex_codes = [f"0x{kc:04X}" for kc in row_keycodes]
                print(f"  Row {row}: {' | '.join(f'{n:>16s}' for n in names)}")

            print()

    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
