#!/usr/bin/env python3
"""
Telink TLSR9511 USB HID OTA Flasher for Wobkey Rainy 75 Pro ISO DE

Protocol reverse-engineered from the official .NET flasher
(KM_USB_mode_ota_pc_v5.0.2 by YKQ).

Usage:
    python3 ota_flasher.py firmware.bin          # Flash firmware
    python3 ota_flasher.py --version             # Query firmware version
    python3 ota_flasher.py --info firmware.bin   # Show firmware info without flashing
    python3 ota_flasher.py --dry-run firmware.bin # Simulate OTA without sending

OTA Protocol (via USB HID Interface 2, Report ID 5, Usage Page 0xFFEF):
  - Packets are 64 bytes (1 report ID + 63 data)
  - Data format: [cmd_type, payload_len, 0x00, payload...]
  - cmd_type 0x01: Query firmware version
  - cmd_type 0x02: OTA data transfer
  - Firmware is sent in 16-byte chunks with index + CRC16
  - Up to 3 chunks per HID write (60 bytes = 3 * 20-byte segments)
  - CRC16 polynomial: 0xA001 (reflected CRC-CCITT), init=0xFFFF

Requirements:
    Linux with hidraw support (no extra packages needed)
"""
import sys
import struct
import time
import argparse
import os
import glob

# Keyboard identifiers
VID = 0x320F
PID = 0x5055
USAGE_PAGE = 0xFFEF
INTERFACE = 2  # USB interface with 0xFFEF vendor channel
REPORT_ID = 0x05


def crc16_telink(data: bytes) -> int:
    """CRC16 with polynomial 0xA001 (reflected 0x8005), init=0xFFFF."""
    poly = [0, 0xA001]
    crc = 0xFFFF
    for byte in data:
        for bit in range(8):
            crc = (crc >> 1) ^ poly[(crc ^ byte) & 1]
            byte >>= 1
    return crc


def parse_telink_ota_firmware(data: bytes) -> dict:
    """Parse a Telink OTA firmware binary."""
    info = {}
    info['total_size'] = len(data)

    # Check TLNK magic at offset 0x20
    if len(data) > 0x24 and data[0x20:0x24] == b'\x4b\x4e\x4c\x54':
        info['has_tlnk'] = True
        info['ota_size'] = struct.unpack_from('<I', data, 0x18)[0]
        info['fw_version'] = struct.unpack_from('<H', data, 0x02)[0]
        if info['ota_size'] <= len(data):
            crc = struct.unpack_from('<I', data, info['ota_size'] - 4)[0]
            info['crc32'] = crc
    else:
        info['has_tlnk'] = False
        info['ota_size'] = len(data)

    # Validate
    if info.get('crc32') in (0, 0xFFFFFFFF):
        info['crc_valid'] = False
    else:
        info['crc_valid'] = True

    return info


def find_hidraw():
    """Find the hidraw device for the keyboard's OTA interface (interface 2).

    Searches /sys/bus/hid/devices/ for VID:PID match, then finds the
    hidraw node associated with the correct USB interface.
    """
    # HID_ID format in uevent: "0003:0000320F:00005055" (zero-padded)
    hid_id_match = f"{VID:08X}:{PID:08X}".upper()

    for hid_dev in glob.glob("/sys/bus/hid/devices/*"):
        uevent_path = os.path.join(hid_dev, "uevent")
        if not os.path.isfile(uevent_path):
            continue
        with open(uevent_path) as f:
            uevent = f.read()
        if hid_id_match not in uevent.upper():
            continue

        # Check if this HID device is on the right USB interface via HID_PHYS
        # HID_PHYS contains e.g. "usb-0000:00:14.0-1/input2" where input2 = interface 2
        if f"input{INTERFACE}" not in uevent:
            continue

        # Find the hidraw node
        hidraw_dir = os.path.join(hid_dev, "hidraw")
        if os.path.isdir(hidraw_dir):
            nodes = os.listdir(hidraw_dir)
            if nodes:
                return f"/dev/{nodes[0]}"

    return None


def open_device():
    """Open hidraw connection to the keyboard OTA interface."""
    hidraw_path = find_hidraw()
    if not hidraw_path:
        print(f"ERROR: Keyboard not found (VID={VID:04X} PID={PID:04X} Interface={INTERFACE})")
        print("Make sure the keyboard is connected via USB.")
        sys.exit(1)

    print(f"  OTA interface: {hidraw_path}")

    try:
        fd = os.open(hidraw_path, os.O_RDWR | os.O_NONBLOCK)
    except PermissionError:
        print(f"ERROR: Permission denied on {hidraw_path}")
        print("Run with sudo or add a udev rule:")
        print(f'  SUBSYSTEM=="hidraw", ATTRS{{idVendor}}=="{VID:04x}", '
              f'ATTRS{{idProduct}}=="{PID:04x}", MODE="0666"')
        sys.exit(1)

    return fd


def send_packet(fd, data: bytes):
    """Send a HID output report with report ID 5 via hidraw."""
    # hidraw expects: [report_id] + [data padded to report size]
    # Pad with 0xFF (not 0x00) to match the official .NET flasher
    packet = bytes([REPORT_ID]) + data + bytes([0xFF] * (63 - len(data)))
    os.write(fd, packet)


def read_response(fd, timeout_ms=2000) -> bytes:
    """Read a HID input report via hidraw with timeout."""
    import select
    ready, _, _ = select.select([fd], [], [], timeout_ms / 1000.0)
    if ready:
        data = os.read(fd, 64)
        return data
    return None


def get_fw_version(dev):
    """Query the keyboard's current firmware version."""
    # Command: [0x01, 0x00, 0x00]
    send_packet(dev, bytes([0x01, 0x00, 0x00]))
    resp = read_response(dev, 3000)
    if resp and len(resp) >= 12 and resp[0] == 0x05 and resp[1] == 0x01 and resp[2] == 0x08:
        version = struct.unpack_from('<I', resp, 4)[0]
        crc = struct.unpack_from('<I', resp, 8)[0]
        return version, crc
    return None, None


def flash_firmware(dev, fw_data: bytes, dry_run=False):
    """Flash firmware via OTA protocol."""
    fw_size = len(fw_data)
    total_segments = (fw_size + 15) // 16

    print(f"\nFlashing {fw_size} bytes ({total_segments} segments)...")

    # Phase 1: Send OTA start command
    # [cmd=0x02, len=0x02, 0x00, subcmd=0x01, 0xFF]
    start_cmd = bytes([0x02, 0x02, 0x00, 0x01, 0xFF])
    if not dry_run:
        send_packet(dev, start_cmd)
        # Wait for device to acknowledge and prepare
        resp = read_response(dev, 3000)
        if resp is None:
            print("  WARNING: No start acknowledgment", file=sys.stderr)

    # Phase 2: Send firmware data
    # Each HID packet contains up to 3 segments of 20 bytes each:
    #   [index_lo, index_hi, data[16], crc_lo, crc_hi]
    ota_index = 0
    start_time = time.time()
    last_seg_idx = (fw_size - 1) // 16  # last segment index (inclusive, 0-based)
    last_progress = -1

    while ota_index <= last_seg_idx:
        segments = bytearray()
        seg_count = 0

        for _ in range(3):
            if ota_index > last_seg_idx:
                break

            # Build segment: [index(2)] + [data(16)] + [crc(2)] = 20 bytes
            seg = bytearray(20)
            seg[0] = ota_index & 0xFF
            seg[1] = (ota_index >> 8) & 0xFF

            for k in range(16):
                offset = ota_index * 16 + k
                if offset < fw_size:
                    seg[k + 2] = fw_data[offset]
                else:
                    seg[k + 2] = 0xFF

            # CRC16 over [index(2) + data(16)] = 18 bytes
            crc = crc16_telink(bytes(seg[:18]))
            seg[18] = crc & 0xFF
            seg[19] = (crc >> 8) & 0xFF

            segments.extend(seg)
            seg_count += 1
            ota_index += 1

        # Build OTA data packet
        payload_len = seg_count * 20
        packet = bytearray([0x02, payload_len, 0x00]) + segments

        if not dry_run:
            send_packet(dev, bytes(packet))
            # Read response (the device sends ACK after each packet)
            resp = read_response(dev, 2000)
            if resp and resp[0] == 0x05 and resp[1] == 0x02:
                # Check for error
                if len(resp) > 6 and resp[2] == 0x03 and resp[5] == 0xFF:
                    if resp[6] != 0:
                        print(f"\n  OTA ERROR at index {ota_index}: code={resp[6]}")
                        return False

        # Progress (only print when percentage changes to avoid excessive output in pipes)
        progress = min(100, ota_index * 16 * 100 // fw_size)
        if progress != last_progress:
            last_progress = progress
            elapsed = time.time() - start_time
            if progress > 0:
                eta = elapsed / progress * (100 - progress)
            else:
                eta = 0
            print(f"\r  [{progress:3d}%] Segment {ota_index}/{total_segments} "
                  f"({elapsed:.1f}s elapsed, ~{eta:.1f}s remaining)", end='', flush=True)

    print()

    # Phase 3: Send OTA end command
    # [cmd=0x02, len=0x06, 0x00, subcmd=0x02, 0xFF, last_idx(2), ~last_idx+1(2)]
    last_idx = ota_index - 1  # ota_index is one past the last sent segment
    complement = (0xFFFF - last_idx + 1) & 0xFFFF  # two's complement (matches .NET flasher)
    end_cmd = bytes([
        0x02, 0x06, 0x00,
        0x02, 0xFF,
        last_idx & 0xFF, (last_idx >> 8) & 0xFF,
        complement & 0xFF, (complement >> 8) & 0xFF
    ])

    if not dry_run:
        send_packet(dev, end_cmd)
        print("  Sent OTA end command. Waiting for device response...")

        # Wait for completion response
        resp = read_response(dev, 10000)
        if resp:
            if (len(resp) >= 7 and resp[0] == 0x05 and resp[1] == 0x02
                    and resp[2] == 0x03 and resp[5] == 0xFF):
                if resp[6] == 0:
                    elapsed = time.time() - start_time
                    print(f"\n  OTA SUCCESS! ({elapsed:.1f}s)")
                    return True
                else:
                    print(f"\n  OTA FAILED! Error code: {resp[6]}")
                    return False
        print("  No response received (device may be rebooting)")
    else:
        elapsed = time.time() - start_time
        print(f"\n  DRY RUN complete ({elapsed:.1f}s simulated)")

    return True


def main():
    parser = argparse.ArgumentParser(description='Telink TLSR9511 OTA Flasher for Rainy 75')
    parser.add_argument('firmware', nargs='?', help='Firmware .bin file to flash')
    parser.add_argument('--version', action='store_true', help='Query current firmware version')
    parser.add_argument('--info', action='store_true', help='Show firmware file info only')
    parser.add_argument('--dry-run', action='store_true', help='Simulate OTA without sending')
    parser.add_argument('--force', action='store_true', help='Skip safety checks')
    parser.add_argument('-y', '--yes', action='store_true', help='Skip confirmation prompt')
    args = parser.parse_args()

    if args.version:
        dev = open_device()
        ver, crc = get_fw_version(dev)
        if ver is not None:
            print(f"Firmware version: 0x{ver:08X}")
            print(f"Firmware CRC: 0x{crc:08X}")
        else:
            print("Failed to get firmware version")
        os.close(dev)
        return

    if not args.firmware:
        parser.print_help()
        return

    # Load firmware
    with open(args.firmware, 'rb') as f:
        fw_data = f.read()

    info = parse_telink_ota_firmware(fw_data)
    if args.info:
        print(f"Firmware file: {args.firmware}")
        print(f"  File size: {info['total_size']} bytes")
        print(f"  TLNK header: {'Yes' if info['has_tlnk'] else 'No'}")
        print(f"  OTA size: {info['ota_size']} bytes (0x{info['ota_size']:X})")
        if info.get('fw_version') is not None:
            print(f"  FW version: 0x{info['fw_version']:04X}")
        if info.get('crc32') is not None:
            print(f"  CRC32: 0x{info['crc32']:08X}")
        if not info.get('crc_valid'):
            print("  WARNING: CRC appears invalid (0x00000000 or 0xFFFFFFFF)")
        return

    print(f"Firmware: {args.firmware} ({info['total_size']} bytes, "
          f"OTA 0x{info['ota_size']:X})")

    # Safety checks
    if not info['has_tlnk'] and not args.force:
        print("\nERROR: Firmware file does not have TLNK header.")
        print("This may not be a valid Telink OTA firmware.")
        print("Use --force to override this check.")
        sys.exit(1)

    if not info.get('crc_valid') and not args.force:
        print("\nERROR: Firmware CRC is invalid.")
        print("Use --force to override this check.")
        sys.exit(1)

    if not args.dry_run and not args.yes:
        print("\n" + "=" * 50)
        print("WARNING: You are about to flash firmware to your keyboard!")
        print("Make sure you have a backup of the original firmware.")
        print("The keyboard will reboot after flashing.")
        print("=" * 50)
        confirm = input("\nType 'YES' to proceed: ")
        if confirm != 'YES':
            print("Aborted.")
            return

    if args.dry_run:
        print("\n[DRY RUN MODE - no data will be sent]")
        flash_firmware(None, fw_data, dry_run=True)
    else:
        dev = open_device()
        ver, crc = get_fw_version(dev)
        if ver is not None:
            print(f"  Device firmware: v0x{ver:08X}")

        success = flash_firmware(dev, fw_data)
        os.close(dev)
        if not success:
            sys.exit(1)


if __name__ == '__main__':
    main()
