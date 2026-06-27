#!/usr/bin/env python3
"""Probe the Rainy 75 keyboard via VIA protocol over hidraw.

VIA protocol uses 32-byte HID reports (no report ID on this interface).
First byte is the command ID.
"""

import os
import sys
import struct
import time

# VIA Protocol Command IDs
VIA_GET_PROTOCOL_VERSION = 0x01
VIA_GET_KEYBOARD_VALUE = 0x02
VIA_SET_KEYBOARD_VALUE = 0x03
VIA_DYNAMIC_KEYMAP_GET_KEYCODE = 0x04
VIA_DYNAMIC_KEYMAP_SET_KEYCODE = 0x05
VIA_DYNAMIC_KEYMAP_RESET = 0x06
VIA_CUSTOM_SET_VALUE = 0x07
VIA_CUSTOM_GET_VALUE = 0x08
VIA_CUSTOM_SAVE = 0x09
VIA_EEPROM_RESET = 0x0A
VIA_BOOTLOADER_JUMP = 0x0B
VIA_DYNAMIC_KEYMAP_MACRO_GET_COUNT = 0x0C
VIA_DYNAMIC_KEYMAP_MACRO_GET_BUFFER_SIZE = 0x0D
VIA_DYNAMIC_KEYMAP_MACRO_GET_BUFFER = 0x0E
VIA_DYNAMIC_KEYMAP_MACRO_SET_BUFFER = 0x0F
VIA_DYNAMIC_KEYMAP_MACRO_RESET = 0x10
VIA_DYNAMIC_KEYMAP_GET_LAYER_COUNT = 0x11
VIA_DYNAMIC_KEYMAP_GET_BUFFER = 0x12
VIA_DYNAMIC_KEYMAP_SET_BUFFER = 0x13

# Keyboard value IDs (for VIA_GET_KEYBOARD_VALUE)
KB_VALUE_UPTIME = 0x01
KB_VALUE_LAYOUT_OPTIONS = 0x02
KB_VALUE_SWITCH_MATRIX_STATE = 0x03

REPORT_SIZE = 32


def via_send_recv(fd, cmd_bytes, timeout=1.0):
    """Send a 32-byte command and read the 32-byte response."""
    # Pad to 32 bytes
    buf = bytes(cmd_bytes) + b'\x00' * (REPORT_SIZE - len(cmd_bytes))
    os.write(fd, buf)

    # Read response with timeout
    import select
    ready, _, _ = select.select([fd], [], [], timeout)
    if ready:
        resp = os.read(fd, REPORT_SIZE)
        return resp
    return None


def main():
    # Find the VIA hidraw device by checking descriptor content
    via_device = None
    for hr_num in range(20):
        hr_path = f"/dev/hidraw{hr_num}"
        rdesc_path = f"/sys/class/hidraw/hidraw{hr_num}/device/report_descriptor"
        try:
            with open(rdesc_path, 'rb') as f:
                desc = f.read()
            # VIA usage page 0xFF60: 06 60 FF
            if desc[:3] == bytes([0x06, 0x60, 0xFF]):
                via_device = hr_path
                break
        except (OSError, FileNotFoundError):
            continue

    if not via_device:
        print("ERROR: Could not find VIA hidraw device!")
        sys.exit(1)

    print(f"Found VIA device: {via_device}")
    print(f"{'='*60}")

    # Open the device
    fd = os.open(via_device, os.O_RDWR | os.O_NONBLOCK)

    try:
        # 1. Get Protocol Version
        print("\n[1] GET_PROTOCOL_VERSION (0x01)")
        resp = via_send_recv(fd, [VIA_GET_PROTOCOL_VERSION])
        if resp:
            version = struct.unpack('>H', resp[1:3])[0]
            print(f"    Response: {resp[:8].hex(' ')}")
            print(f"    VIA Protocol Version: {version} (0x{version:04X})")
            major = version >> 8
            minor = version & 0xFF
            print(f"    Parsed: v{major}.{minor}")
        else:
            print("    No response (timeout)")

        # 2. Get Keyboard Value: Uptime
        print("\n[2] GET_KEYBOARD_VALUE: Uptime (0x02, 0x01)")
        resp = via_send_recv(fd, [VIA_GET_KEYBOARD_VALUE, KB_VALUE_UPTIME])
        if resp:
            uptime = struct.unpack('>I', resp[2:6])[0]
            print(f"    Response: {resp[:8].hex(' ')}")
            print(f"    Uptime: {uptime} ms ({uptime/1000:.1f} s)")
        else:
            print("    No response (timeout)")

        # 3. Get Keyboard Value: Layout Options
        print("\n[3] GET_KEYBOARD_VALUE: Layout Options (0x02, 0x02)")
        resp = via_send_recv(fd, [VIA_GET_KEYBOARD_VALUE, KB_VALUE_LAYOUT_OPTIONS])
        if resp:
            layout = struct.unpack('>I', resp[2:6])[0]
            print(f"    Response: {resp[:8].hex(' ')}")
            print(f"    Layout Options: 0x{layout:08X}")
        else:
            print("    No response (timeout)")

        # 4. Get Layer Count
        print("\n[4] DYNAMIC_KEYMAP_GET_LAYER_COUNT (0x11)")
        resp = via_send_recv(fd, [VIA_DYNAMIC_KEYMAP_GET_LAYER_COUNT])
        if resp:
            layer_count = resp[1]
            print(f"    Response: {resp[:8].hex(' ')}")
            print(f"    Layer Count: {layer_count}")
        else:
            print("    No response (timeout)")

        # 5. Get Macro Count
        print("\n[5] DYNAMIC_KEYMAP_MACRO_GET_COUNT (0x0C)")
        resp = via_send_recv(fd, [VIA_DYNAMIC_KEYMAP_MACRO_GET_COUNT])
        if resp:
            macro_count = resp[1]
            print(f"    Response: {resp[:8].hex(' ')}")
            print(f"    Macro Count: {macro_count}")
        else:
            print("    No response (timeout)")

        # 6. Get Macro Buffer Size
        print("\n[6] DYNAMIC_KEYMAP_MACRO_GET_BUFFER_SIZE (0x0D)")
        resp = via_send_recv(fd, [VIA_DYNAMIC_KEYMAP_MACRO_GET_BUFFER_SIZE])
        if resp:
            buf_size = struct.unpack('>H', resp[1:3])[0]
            print(f"    Response: {resp[:8].hex(' ')}")
            print(f"    Macro Buffer Size: {buf_size} bytes")
        else:
            print("    No response (timeout)")

        # 7. Get Switch Matrix State
        print("\n[7] GET_KEYBOARD_VALUE: Switch Matrix State (0x02, 0x03)")
        resp = via_send_recv(fd, [VIA_GET_KEYBOARD_VALUE, KB_VALUE_SWITCH_MATRIX_STATE])
        if resp:
            print(f"    Response: {resp.hex(' ')}")
            # Matrix state is a bitmap of pressed keys
            pressed = [i for i in range(len(resp[2:])*8) if resp[2 + i//8] & (1 << (i % 8))]
            if pressed:
                print(f"    Pressed keys (bit positions): {pressed}")
            else:
                print("    No keys currently pressed")
        else:
            print("    No response (timeout)")

        # 8. Read first keymap entry (layer 0, row 0, col 0)
        print("\n[8] DYNAMIC_KEYMAP_GET_KEYCODE: Layer 0, Row 0, Col 0 (0x04)")
        resp = via_send_recv(fd, [VIA_DYNAMIC_KEYMAP_GET_KEYCODE, 0, 0, 0])
        if resp:
            keycode = struct.unpack('>H', resp[4:6])[0]
            print(f"    Response: {resp[:8].hex(' ')}")
            print(f"    Keycode at [0,0,0]: 0x{keycode:04X}")
        else:
            print("    No response (timeout)")

        # 9. Read keymap buffer (first 28 bytes = first 14 keycodes of layer 0)
        print("\n[9] DYNAMIC_KEYMAP_GET_BUFFER: Offset 0, Size 28 (0x12)")
        resp = via_send_recv(fd, [VIA_DYNAMIC_KEYMAP_GET_BUFFER, 0x00, 0x00, 28])
        if resp:
            print(f"    Response: {resp.hex(' ')}")
            # Parse keycodes (big-endian 16-bit)
            keycodes = []
            for i in range(4, 4 + 28, 2):
                if i + 1 < len(resp):
                    kc = struct.unpack('>H', resp[i:i+2])[0]
                    keycodes.append(f"0x{kc:04X}")
            print(f"    First 14 keycodes: {' '.join(keycodes)}")
        else:
            print("    No response (timeout)")

        # 10. Try custom get value (lighting)
        print("\n[10] CUSTOM_GET_VALUE: Channel 1, ID 1 (0x08)")
        resp = via_send_recv(fd, [VIA_CUSTOM_GET_VALUE, 0x01, 0x01])
        if resp:
            print(f"    Response: {resp[:16].hex(' ')}")
        else:
            print("    No response (timeout)")

        print(f"\n{'='*60}")
        print("VIA probe complete!")

    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
