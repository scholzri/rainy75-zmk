#!/usr/bin/env python3
"""
Wobkey Rainy 75 Pro — SRAM/Flash Memory Reader

Protocol reverse-engineered from wobwxe.com (WOB磁轴驱动) JavaScript bundle,
retrieved via Wayback Machine (2025-07-19 snapshot).

The WOB driver uses a 0xBE-framed protocol over HID Report ID 4 (Interface 3,
Usage Page 0xFF1C, Usage 0x92) to directly read/write Telink B91 SRAM and flash.

Usage:
    python3 mem_reader.py --probe              # Check if protocol is supported
    python3 mem_reader.py --read-ram ADDR LEN  # Read SRAM (hex addr, decimal len)
    python3 mem_reader.py --read-flash ADDR LEN # Read flash (hex addr, decimal len)
    python3 mem_reader.py --dump-sram FILE     # Dump full 256KB SRAM to file
    python3 mem_reader.py --dump-flash FILE    # Dump 1MB flash to file
    python3 mem_reader.py --version            # Read firmware version from SRAM

Protocol (0xBE framed):
    Send: [0xBE] [cmd] [len_lo] [len_hi] [addr LE 4B] [datalen_hi] [datalen_lo] [0xED]
    Recv: [0xBE] [ack] [len_lo] [len_hi] [reserved 6B] [payload...] [crc_lo] [crc_hi] [0xED]

Commands:
    0x01 = ReadFlash    0x04 = ReadRam
    0x02 = Save         0xFF = Reset
    0x03 = SetRam       0xFC = Download (enter OTA bootloader)

Requirements:
    Linux with hidraw support. Run as root or add udev rule.
"""
import sys
import struct
import time
import argparse
import os
import glob
import select

# Keyboard identifiers
VID = 0x320F
PID = 0x5055

# Protocol constants
MAGIC_START = 0xBE
MAGIC_END = 0xED
CMD_READ_FLASH = 0x01
CMD_SAVE = 0x02
CMD_SET_RAM = 0x03
CMD_READ_RAM = 0x04
CMD_DOWNLOAD = 0xFC
CMD_RESET = 0xFF

# Report ID 4 on Interface 3 (0xFF1C)
REPORT_ID = 0x04
TARGET_INTERFACE = 3

# CRC-16/MODBUS lookup table (from wobwxe.com JS)
CRC_TABLE = [
    0, 49345, 49537, 320, 49921, 960, 640, 49729,
    50689, 1728, 1920, 51009, 1280, 50625, 50305, 1088,
    52225, 3264, 3456, 52545, 3840, 53185, 52865, 3648,
    2560, 51905, 52097, 2880, 51457, 2496, 2176, 51265,
    55297, 6336, 6528, 55617, 6912, 56257, 55937, 6720,
    7680, 57025, 57217, 8000, 56577, 7616, 7296, 56385,
    5120, 54465, 54657, 5440, 55041, 6080, 5760, 54849,
    53761, 4800, 4992, 54081, 4352, 53697, 53377, 4160,
    61441, 12480, 12672, 61761, 13056, 62401, 62081, 12864,
    13824, 63169, 63361, 14144, 62721, 13760, 13440, 62529,
    15360, 64705, 64897, 15680, 65281, 16320, 16000, 65089,
    64001, 15040, 15232, 64321, 14592, 63937, 63617, 14400,
    10240, 59585, 59777, 10560, 60161, 11200, 10880, 59969,
    60929, 11968, 12160, 61249, 11520, 60865, 60545, 11328,
    58369, 9408, 9600, 58689, 9984, 59329, 59009, 9792,
    8704, 58049, 58241, 9024, 57601, 8640, 8320, 57409,
    40961, 24768, 24960, 41281, 25344, 41921, 41601, 25152,
    26112, 42689, 42881, 26432, 42241, 26048, 25728, 42049,
    27648, 44225, 44417, 27968, 44801, 28608, 28288, 44609,
    43521, 27328, 27520, 43841, 26880, 43457, 43137, 26688,
    30720, 47297, 47489, 31040, 47873, 31680, 31360, 47681,
    48641, 32448, 32640, 48961, 32000, 48577, 48257, 31808,
    46081, 29888, 30080, 46401, 30464, 47041, 46721, 30272,
    29184, 45761, 45953, 29504, 45313, 29120, 28800, 45121,
    20480, 37057, 37249, 20800, 37633, 21440, 21120, 37441,
    38401, 22208, 22400, 38721, 21760, 38337, 38017, 21568,
    39937, 23744, 23936, 40257, 24320, 40897, 40577, 24128,
    23040, 39617, 39809, 23360, 39169, 22976, 22656, 38977,
    34817, 18624, 18816, 35137, 19200, 35777, 35457, 19008,
    19968, 36545, 36737, 20288, 36097, 19904, 19584, 35905,
    17408, 33985, 34177, 17728, 34561, 18368, 18048, 34369,
    33281, 17088, 17280, 33601, 16640, 33217, 32897, 16448,
]


def crc16_modbus(data: bytes, length: int) -> int:
    """CRC-16/MODBUS matching wobwxe.com JS implementation."""
    crc = 0xFFFF
    for i in range(length):
        idx = (crc & 0xFF) ^ data[i]
        crc = (crc >> 8) ^ CRC_TABLE[idx]
    return crc & 0xFFFF


def build_read_packet(cmd: int, address: int, length: int) -> bytes:
    """Build a 0xBE-framed read command packet.

    Format: [0xBE, cmd, total_len_lo, total_len_hi, addr0-3 LE, datalen_hi, datalen_lo, 0xED]
    Total length = 11 bytes (fixed for read commands)
    """
    total_len = 11
    pkt = bytearray([
        MAGIC_START,
        cmd,
        total_len & 0xFF,
        (total_len >> 8) & 0xFF,
        address & 0xFF,
        (address >> 8) & 0xFF,
        (address >> 16) & 0xFF,
        (address >> 24) & 0xFF,
        (length >> 8) & 0xFF,   # datalen high byte
        length & 0xFF,           # datalen low byte
        MAGIC_END,
    ])
    return bytes(pkt)


def parse_response(data: bytes) -> bytes:
    """Parse a 0xBE-framed response, validate CRC, return payload.

    Response: [0xBE, ack_type, len_lo, len_hi, ...reserved..., payload, crc_lo, crc_hi, 0xED]
    """
    if not data or len(data) < 5:
        return None

    if data[0] != MAGIC_START:
        return None

    ack_type = data[1]
    if ack_type not in (1, 4):
        return None

    total_len = data[2] + (data[3] << 8)
    if total_len > len(data):
        return None

    if data[total_len - 1] != MAGIC_END:
        return None

    # CRC check
    expected_crc = crc16_modbus(data, total_len - 3)
    actual_crc = data[total_len - 3] + (data[total_len - 2] << 8)
    if expected_crc != actual_crc:
        print(f"  CRC mismatch: expected 0x{expected_crc:04X}, got 0x{actual_crc:04X}")
        return None

    # Payload starts at byte 10, ends before CRC
    payload = bytes(data[10:total_len - 3])
    return payload


def find_hidraw_for_interface(interface: int):
    """Find the hidraw device for a specific USB interface."""
    hid_id_match = f"{VID:08X}:{PID:08X}".upper()

    for hid_dev in glob.glob("/sys/bus/hid/devices/*"):
        uevent_path = os.path.join(hid_dev, "uevent")
        if not os.path.isfile(uevent_path):
            continue
        with open(uevent_path) as f:
            uevent = f.read()
        if hid_id_match not in uevent.upper():
            continue
        if f"input{interface}" not in uevent:
            continue

        hidraw_dir = os.path.join(hid_dev, "hidraw")
        if os.path.isdir(hidraw_dir):
            nodes = os.listdir(hidraw_dir)
            if nodes:
                return f"/dev/{nodes[0]}"
    return None


def open_device():
    """Open hidraw connections. Returns (send_fd, recv_fds) tuple.

    Sends on Interface 3 (Report ID 4).
    Listens on Interface 2 AND Interface 3 for responses.
    """
    send_path = find_hidraw_for_interface(TARGET_INTERFACE)
    if not send_path:
        print(f"ERROR: Interface {TARGET_INTERFACE} not found "
              f"(VID={VID:04X} PID={PID:04X})")
        print("Make sure the keyboard is connected via USB.")
        sys.exit(1)

    print(f"Send interface: {send_path} (Interface {TARGET_INTERFACE}, Report ID {REPORT_ID})")

    recv_paths = []
    for iface in (2, 3):
        path = find_hidraw_for_interface(iface)
        if path:
            recv_paths.append((iface, path))
            print(f"Listen interface: {path} (Interface {iface})")

    try:
        send_fd = os.open(send_path, os.O_RDWR | os.O_NONBLOCK)
    except PermissionError:
        print(f"ERROR: Permission denied on {send_path}")
        print("Run with sudo or add a udev rule:")
        print(f'  SUBSYSTEM=="hidraw", ATTRS{{idVendor}}=="{VID:04x}", '
              f'ATTRS{{idProduct}}=="{PID:04x}", MODE="0666"')
        sys.exit(1)

    recv_fds = []
    for iface, path in recv_paths:
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            recv_fds.append((iface, fd))
        except PermissionError:
            print(f"  Warning: cannot open {path} for reading")

    return send_fd, recv_fds


def send_packet(fd, data: bytes):
    """Send a HID output report with Report ID 4 via hidraw."""
    # Pad to 63 bytes (report ID + 63 = 64 byte packet)
    padded = data + bytes(63 - len(data))
    packet = bytes([REPORT_ID]) + padded
    os.write(fd, packet)


def read_responses(recv_fds, timeout_ms=500) -> list:
    """Read responses from all monitored interfaces."""
    results = []
    fds = [fd for _, fd in recv_fds]
    deadline = time.time() + timeout_ms / 1000.0

    while time.time() < deadline:
        remaining = max(0.01, deadline - time.time())
        ready, _, _ = select.select(fds, [], [], remaining)
        if not ready:
            break
        for fd in ready:
            try:
                data = os.read(fd, 256)
                if data:
                    iface = next(i for i, f in recv_fds if f == fd)
                    results.append((iface, data))
            except OSError:
                pass

    return results


def drain_input(recv_fds):
    """Drain any pending input data."""
    for _, fd in recv_fds:
        try:
            while True:
                ready, _, _ = select.select([fd], [], [], 0.01)
                if not ready:
                    break
                os.read(fd, 256)
        except OSError:
            pass


def read_memory(send_fd, recv_fds, cmd, address, length, retries=3):
    """Read memory (SRAM or flash) at the given address."""
    packet = build_read_packet(cmd, address, length)

    for attempt in range(retries):
        drain_input(recv_fds)
        send_packet(send_fd, packet)
        responses = read_responses(recv_fds, timeout_ms=1000)

        for iface, data in responses:
            # Skip keyboard input reports (keypresses etc)
            if len(data) > 0 and data[0] == MAGIC_START:
                payload = parse_response(data)
                if payload is not None:
                    return payload

            # Also check if response starts at byte 1 (report ID stripped)
            if len(data) > 1 and data[0] in (0x04, 0x05) and data[1] == MAGIC_START:
                payload = parse_response(data[1:])
                if payload is not None:
                    return payload

        if attempt < retries - 1:
            time.sleep(0.1)

    return None


def probe_protocol(send_fd, recv_fds):
    """Test if the keyboard responds to the 0xBE protocol."""
    print("\n=== Protocol Probe ===")
    print("Sending ReadRam for firmware version (0x800701E8, 4 bytes)...")

    # Try the known soft_version address from the magnetic switch firmware
    result = read_memory(send_fd, recv_fds, CMD_READ_RAM, 0x800701E8, 4)
    if result:
        print(f"  Response: {result.hex()}")
        print("  Protocol is SUPPORTED!")
        return True

    print("  No valid response from ReadRam.")
    print()
    print("Trying ReadFlash at offset 0 (4 bytes)...")
    result = read_memory(send_fd, recv_fds, CMD_READ_FLASH, 0, 4)
    if result:
        print(f"  Response: {result.hex()}")
        print("  ReadFlash is SUPPORTED!")
        return True

    print("  No valid response from ReadFlash.")
    print()

    # Show all raw responses we got
    print("Sending one more ReadRam and showing ALL raw responses...")
    drain_input(recv_fds)
    packet = build_read_packet(CMD_READ_RAM, 0x80000000, 4)
    send_packet(send_fd, packet)
    time.sleep(0.5)

    responses = read_responses(recv_fds, timeout_ms=2000)
    if responses:
        for iface, data in responses:
            print(f"  Interface {iface}: [{len(data)}B] {data[:32].hex()}")
    else:
        print("  No responses received on any interface.")
        print()
        print("The standard Rainy 75 firmware may not implement the 0xBE protocol.")
        print("This protocol might be exclusive to the magnetic switch (RT) variant.")

    return False


def dump_memory(send_fd, recv_fds, cmd, start_addr, total_size, output_file,
                chunk_size=48):
    """Dump a range of memory to a file."""
    cmd_name = "flash" if cmd == CMD_READ_FLASH else "SRAM"
    print(f"\nDumping {total_size} bytes of {cmd_name} "
          f"from 0x{start_addr:08X} to {output_file}...")

    data = bytearray()
    offset = 0
    start_time = time.time()
    errors = 0

    with open(output_file, 'wb') as f:
        while offset < total_size:
            remaining = min(chunk_size, total_size - offset)
            addr = start_addr + offset

            result = read_memory(send_fd, recv_fds, cmd, addr, remaining)
            if result is None:
                errors += 1
                if errors > 10:
                    print(f"\n  Too many errors ({errors}). Aborting.")
                    print(f"  Dumped {offset} of {total_size} bytes.")
                    return False
                # Write zeros for failed reads
                f.write(bytes(remaining))
                print(f"\n  Read failed at 0x{addr:08X}, wrote zeros")
            else:
                f.write(result[:remaining])
                errors = 0

            offset += remaining

            # Progress
            elapsed = time.time() - start_time
            progress = offset * 100 // total_size
            if elapsed > 0:
                speed = offset / elapsed
                eta = (total_size - offset) / speed if speed > 0 else 0
            else:
                eta = 0
            print(f"\r  [{progress:3d}%] 0x{addr:08X}  "
                  f"{offset}/{total_size}  "
                  f"{elapsed:.1f}s elapsed, ~{eta:.1f}s remaining  ",
                  end='', flush=True)

    elapsed = time.time() - start_time
    print(f"\n  Done! {total_size} bytes in {elapsed:.1f}s "
          f"({total_size/elapsed:.0f} B/s)")
    return True


def main():
    parser = argparse.ArgumentParser(
        description='Wobkey Rainy 75 — SRAM/Flash Memory Reader (0xBE protocol)')
    parser.add_argument('--probe', action='store_true',
                        help='Test if the keyboard supports the 0xBE protocol')
    parser.add_argument('--read-ram', nargs=2, metavar=('ADDR', 'LEN'),
                        help='Read SRAM at hex ADDR for LEN bytes (e.g., 0x80080144 162)')
    parser.add_argument('--read-flash', nargs=2, metavar=('ADDR', 'LEN'),
                        help='Read flash at hex ADDR for LEN bytes (e.g., 0x0 256)')
    parser.add_argument('--dump-sram', metavar='FILE',
                        help='Dump full 256KB SRAM (0x80000000-0x8003FFFF) to file')
    parser.add_argument('--dump-flash', metavar='FILE',
                        help='Dump 1MB flash (0x00000000-0x000FFFFF) to file')
    parser.add_argument('--version', action='store_true',
                        help='Read firmware version from known SRAM address')
    args = parser.parse_args()

    if not any([args.probe, args.read_ram, args.read_flash,
                args.dump_sram, args.dump_flash, args.version]):
        parser.print_help()
        return

    send_fd, recv_fds = open_device()

    try:
        if args.probe:
            probe_protocol(send_fd, recv_fds)

        elif args.read_ram:
            addr = int(args.read_ram[0], 0)
            length = int(args.read_ram[1])
            print(f"\nReadRam 0x{addr:08X}, {length} bytes...")
            result = read_memory(send_fd, recv_fds, CMD_READ_RAM, addr, length)
            if result:
                print(f"  Response ({len(result)} bytes):")
                # Hex dump
                for i in range(0, len(result), 16):
                    hex_part = ' '.join(f'{b:02X}' for b in result[i:i+16])
                    ascii_part = ''.join(
                        chr(b) if 32 <= b < 127 else '.' for b in result[i:i+16])
                    print(f"  {addr+i:08X}: {hex_part:<48s} {ascii_part}")
            else:
                print("  No response.")

        elif args.read_flash:
            addr = int(args.read_flash[0], 0)
            length = int(args.read_flash[1])
            print(f"\nReadFlash 0x{addr:08X}, {length} bytes...")
            result = read_memory(send_fd, recv_fds, CMD_READ_FLASH, addr, length)
            if result:
                print(f"  Response ({len(result)} bytes):")
                for i in range(0, len(result), 16):
                    hex_part = ' '.join(f'{b:02X}' for b in result[i:i+16])
                    ascii_part = ''.join(
                        chr(b) if 32 <= b < 127 else '.' for b in result[i:i+16])
                    print(f"  {addr+i:08X}: {hex_part:<48s} {ascii_part}")
            else:
                print("  No response.")

        elif args.dump_sram:
            # Telink B91 SRAM: 256KB at 0x80000000
            dump_memory(send_fd, recv_fds, CMD_READ_RAM,
                        0x80000000, 256 * 1024, args.dump_sram)

        elif args.dump_flash:
            # Telink B91 flash: 1MB at 0x00000000
            dump_memory(send_fd, recv_fds, CMD_READ_FLASH,
                        0x00000000, 1024 * 1024, args.dump_flash)

        elif args.version:
            # Try the magnetic switch firmware's version address
            print("\nReading firmware version from known addresses...")
            for name, addr, length in [
                ("soft_version (RT)", 0x800701E8, 4),
                ("SRAM base", 0x80000000, 16),
                ("Flash base (boot vector)", 0x00000000, 32),
            ]:
                print(f"\n  {name} @ 0x{addr:08X} ({length}B):")
                cmd = CMD_READ_FLASH if addr < 0x80000000 else CMD_READ_RAM
                result = read_memory(send_fd, recv_fds, cmd, addr, length)
                if result:
                    print(f"    {result.hex()}")
                else:
                    print(f"    No response")

    finally:
        os.close(send_fd)
        for _, fd in recv_fds:
            os.close(fd)


if __name__ == '__main__':
    main()
