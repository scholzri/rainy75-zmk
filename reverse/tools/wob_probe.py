#!/usr/bin/env python3
"""
Wobkey Rainy 75 Pro — WOB Driver Protocol C Probe

Probes the WOB proprietary HID interface (usagePage 0xFF1C, usage 0x92,
reportId 4, Interface 3) using the Packet-Based protocol discovered in
the wobwxe.com JavaScript driver.

The WOB driver lists our keyboard (0x320F:0x5055) as using "Protocol C"
with pre-defined packet command sequences (AllDataCom, keyMapCom).
This is DIFFERENT from the Address-Based protocol used by RT hall-effect
keyboards — that protocol was already tested (no response).

Usage:
    python3 wob_probe.py --probe           # Test heartbeat + basic reads
    python3 wob_probe.py --read-keymap     # Read keymap via keyMapCom
    python3 wob_probe.py --read-all        # Read all data via AllDataCom
    python3 wob_probe.py --fw-status       # Check firmware update status
    python3 wob_probe.py --raw CMD_BYTES   # Send raw bytes (hex, comma-sep)

Protocol C packet format (from JS analysis):
    [byte0, byte1, cmd_type, chunk_size, offset_lo, offset_mid, offset_hi]

    cmd_type 1 = begin transaction
    cmd_type 2 = end/save transaction
    cmd_type 3 = read config (34 bytes)
    cmd_type 5 = read data block
    cmd_type 7 = read keymap
    cmd_type 8 = read all data chunk
    cmd_type 9 = write keymap chunk
    cmd_type 20 = read (unknown)
    cmd_type 26 = read (unknown)
    cmd_type 27 = read (unknown)

Special commands (0xBE-framed, shared across all WOB keyboards):
    Heartbeat:  [0xBE, 0x00, 0x05, 0x00, 0xED]
    FW status:  [0xBE, 0x74, 0x05, 0x00, 0xED]
    Save:       [0xBE, 0x02, 0x05, 0x00, 0xED]  (NOT sent by this tool)
    DFU:        [0xBE, 0xFC, 0x05, 0x00, 0xED]  (NOT sent by this tool)
    Reset:      [0xBE, 0xEE, 0x05, 0x00, 0xED]  (NOT sent by this tool)

Requirements:
    Linux with hidraw support. Run as root or add udev rule.
"""
import sys
import time
import argparse
import os
import glob
import select

# Keyboard identifiers
VID = 0x320F
PID = 0x5055

# HID interface for WOB proprietary protocol
REPORT_ID = 0x04
TARGET_INTERFACE = 3  # usagePage 0xFF1C, usage 0x92

# Protocol C pre-defined command sequences (from wobwxe.com JS)
# Each sub-array is one HID output report payload

KEYMAP_COM = [
    [63, 0, 7, 56, 0, 0, 0],
    [119, 0, 7, 56, 56, 0, 0],
    [175, 0, 7, 56, 112, 0, 0],
    [231, 0, 7, 56, 168, 0, 0],
    [31, 1, 7, 56, 224, 0, 0],
    [88, 0, 7, 56, 24, 1, 0],
    [136, 0, 7, 48, 80, 1, 0],
]

# AllDataCom — the full read sequence the WOB driver sends to read everything
# Includes begin [1,0,1], multiple read commands, end [2,0,2], and more reads
ALL_DATA_COM = [
    [1, 0, 1],              # begin transaction
    [37, 0, 3, 34],         # cmd 3: read config (34 bytes)
    [64, 0, 8, 56, 0, 0, 0],      # cmd 8: read chunk at offset 0
    [120, 0, 8, 56, 56, 0, 0],    # cmd 8: read chunk at offset 56
    [176, 0, 8, 56, 112, 0, 0],   # cmd 8: read chunk at offset 112
    [232, 0, 8, 56, 168, 0, 0],   # cmd 8: read chunk at offset 168
    [32, 1, 8, 56, 224, 0, 0],    # cmd 8: read chunk at offset 224
    [89, 0, 8, 56, 24, 1, 0],     # cmd 8: read chunk at offset 280
    [137, 0, 8, 48, 80, 1, 0],    # cmd 8: read chunk at offset 336 (48 bytes)
    [83, 0, 27, 56],               # cmd 27: read (unknown type)
    [139, 0, 27, 56, 56],          # cmd 27: read
    [155, 0, 27, 16, 112],         # cmd 27: read
    [2, 0, 2],              # end/save transaction
    [6, 0, 5, 1],           # cmd 5: read data block
    [44, 0, 5, 39],         # cmd 5: read data block
    [32, 0, 26, 6],         # cmd 26: read
    [76, 0, 20, 56, 0, 0, 0],     # cmd 20: read
]

# SetKeyMapCom — write sequence (for reference only, NOT used)
# SET_KEYMAP_COM = [
#     [1, 0, 1],  # begin
#     [37, 0, 3, 34],
#     [173, 6, 9, 56, 0, 0, 0],  # cmd 9: write keymap chunk
#     ...
#     [2, 0, 2],  # end/save
# ]


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
    """Open hidraw for Interface 3 (WOB protocol). Also listen on Interface 2."""
    send_path = find_hidraw_for_interface(TARGET_INTERFACE)
    if not send_path:
        print(f"ERROR: Interface {TARGET_INTERFACE} not found "
              f"(VID={VID:04X} PID={PID:04X})")
        print("Make sure the keyboard is connected via USB.")
        sys.exit(1)

    print(f"WOB interface: {send_path} (Interface {TARGET_INTERFACE}, "
          f"Report ID {REPORT_ID})")

    recv_paths = []
    for iface in (2, 3):
        path = find_hidraw_for_interface(iface)
        if path:
            recv_paths.append((iface, path))
            print(f"  Listen: {path} (Interface {iface})")

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


def send_raw(fd, data: bytes, pad_to=64):
    """Send HID output report via hidraw.

    For Protocol C with reportSize=0, we try sending at exact length first.
    If that fails, fall back to padded 64-byte report.
    """
    packet = bytes([REPORT_ID]) + data
    if pad_to and len(packet) < pad_to:
        packet = packet + bytes(pad_to - len(packet))
    os.write(fd, packet)


def read_responses(recv_fds, timeout_ms=500, max_responses=10) -> list:
    """Read responses from all monitored interfaces."""
    results = []
    fds = [fd for _, fd in recv_fds]
    if not fds:
        return results
    deadline = time.time() + timeout_ms / 1000.0

    while time.time() < deadline and len(results) < max_responses:
        remaining = max(0.01, deadline - time.time())
        ready, _, _ = select.select(fds, [], [], remaining)
        if not ready:
            break
        for fd in ready:
            try:
                data = os.read(fd, 1024)
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
                os.read(fd, 1024)
        except OSError:
            pass


def hexdump(data: bytes, prefix="  ", start_offset=0):
    """Print a hex dump of data."""
    for i in range(0, len(data), 16):
        hex_part = ' '.join(f'{b:02X}' for b in data[i:i + 16])
        ascii_part = ''.join(
            chr(b) if 32 <= b < 127 else '.' for b in data[i:i + 16])
        print(f"{prefix}{start_offset + i:04X}: {hex_part:<48s} {ascii_part}")


def send_and_show(send_fd, recv_fds, data: bytes, label: str,
                  timeout_ms=500, pad_to=64):
    """Send a command and display all responses."""
    drain_input(recv_fds)

    data_hex = ' '.join(f'{b:02X}' for b in data)
    print(f"\n  TX [{len(data)}B]: {data_hex}")
    print(f"      ({label})")

    send_raw(send_fd, data, pad_to=pad_to)
    responses = read_responses(recv_fds, timeout_ms=timeout_ms)

    if responses:
        for iface, resp in responses:
            resp_hex = ' '.join(f'{b:02X}' for b in resp[:64])
            print(f"  RX iface{iface} [{len(resp)}B]: {resp_hex}")
            if len(resp) > 64:
                print(f"      ... +{len(resp) - 64} more bytes")
    else:
        print(f"  RX: (no response)")

    return responses


def probe(send_fd, recv_fds):
    """Test if the keyboard responds to WOB Protocol C."""
    print("\n=== WOB Protocol C Probe ===")
    print("Testing on Interface 3 (usagePage 0xFF1C, reportId 4)")
    got_any = False

    # Test 1: Heartbeat (0xBE-framed, common to all WOB keyboards)
    print("\n--- Test 1: Heartbeat ---")
    responses = send_and_show(
        send_fd, recv_fds,
        bytes([0xBE, 0x00, 0x05, 0x00, 0xED]),
        "Heartbeat (0xBE framed)",
        timeout_ms=1000)
    if responses:
        got_any = True

    time.sleep(0.1)

    # Test 2: FW status check (0xBE-framed, safe read-only)
    print("\n--- Test 2: FW Status ---")
    responses = send_and_show(
        send_fd, recv_fds,
        bytes([0xBE, 0x74, 0x05, 0x00, 0xED]),
        "FW status check (0xBE framed)",
        timeout_ms=1000)
    if responses:
        got_any = True

    time.sleep(0.1)

    # Test 3: Protocol C begin transaction
    print("\n--- Test 3: Begin Transaction ---")
    responses = send_and_show(
        send_fd, recv_fds,
        bytes([1, 0, 1]),
        "Protocol C: begin [1,0,1]",
        timeout_ms=1000)
    if responses:
        got_any = True

    time.sleep(0.1)

    # Test 4: Single keyMapCom read (cmd 7, safe read-only)
    print("\n--- Test 4: KeyMap Read (1st chunk) ---")
    responses = send_and_show(
        send_fd, recv_fds,
        bytes([63, 0, 7, 56, 0, 0, 0]),
        "Protocol C: keyMapCom[0] — read 56B keymap at offset 0",
        timeout_ms=1000)
    if responses:
        got_any = True

    time.sleep(0.1)

    # Test 5: Try sending exact-length packets (no padding)
    # In case the firmware expects the exact WebHID behavior
    print("\n--- Test 5: Heartbeat (exact length, no padding) ---")
    try:
        responses = send_and_show(
            send_fd, recv_fds,
            bytes([0xBE, 0x00, 0x05, 0x00, 0xED]),
            "Heartbeat (exact 5 bytes, no padding)",
            timeout_ms=1000,
            pad_to=0)
        if responses:
            got_any = True
    except OSError as e:
        print(f"  Send failed: {e} (exact-length not supported by hidraw)")

    time.sleep(0.1)

    # Test 6: keyMapCom without padding
    print("\n--- Test 6: KeyMap Read (exact length, no padding) ---")
    try:
        responses = send_and_show(
            send_fd, recv_fds,
            bytes([63, 0, 7, 56, 0, 0, 0]),
            "Protocol C: keyMapCom[0] (exact 7 bytes, no padding)",
            timeout_ms=1000,
            pad_to=0)
        if responses:
            got_any = True
    except OSError as e:
        print(f"  Send failed: {e} (exact-length not supported by hidraw)")

    print("\n" + "=" * 50)
    if got_any:
        print("SUCCESS: Got responses! Protocol C appears to work.")
    else:
        print("No responses received from WOB protocol.")
        print("Possible reasons:")
        print("  - Standard firmware may not implement Protocol C either")
        print("  - WOB driver JS might have Protocol C but firmware lacks handler")
        print("  - Report size mismatch (hidraw vs WebHID)")
    print("=" * 50)

    return got_any


def read_keymap(send_fd, recv_fds):
    """Read the full keymap using keyMapCom sequence."""
    print("\n=== Read Keymap (Protocol C) ===")
    print(f"Sending {len(KEYMAP_COM)} keyMapCom packets...")

    all_data = bytearray()

    for i, cmd in enumerate(KEYMAP_COM):
        cmd_bytes = bytes(cmd)
        chunk_size = cmd[3]
        offset = 0
        if len(cmd) >= 7:
            offset = cmd[4] + (cmd[5] << 8) + (cmd[6] << 16)

        label = (f"keyMapCom[{i}]: cmd={cmd[2]}, "
                 f"size={chunk_size}, offset={offset}")
        responses = send_and_show(
            send_fd, recv_fds, cmd_bytes, label, timeout_ms=500)

        for iface, resp in responses:
            # Check if it looks like real data (not echo-back)
            if resp != cmd_bytes and len(resp) > 0:
                all_data.extend(resp)

        time.sleep(0.05)

    if all_data:
        print(f"\n--- Combined Response Data ({len(all_data)} bytes) ---")
        hexdump(all_data)
    else:
        print("\nNo data received from keymap read.")


def read_all_data(send_fd, recv_fds):
    """Read all data using AllDataCom sequence (read-only commands only)."""
    print("\n=== Read All Data (Protocol C) ===")

    # Filter out the save command [2,0,2] for safety
    safe_commands = []
    for cmd in ALL_DATA_COM:
        if cmd == [2, 0, 2]:
            safe_commands.append(("SKIP", cmd, "end/save — SKIPPED for safety"))
        else:
            safe_commands.append(("SEND", cmd, None))

    print(f"AllDataCom has {len(ALL_DATA_COM)} commands, "
          f"sending {sum(1 for t, _, _ in safe_commands if t == 'SEND')} "
          f"(skipping save)")

    all_data = bytearray()

    for i, (action, cmd, reason) in enumerate(safe_commands):
        if action == "SKIP":
            print(f"\n  SKIP [{len(cmd)}B]: "
                  f"{' '.join(f'{b:02X}' for b in cmd)}")
            print(f"      ({reason})")
            continue

        cmd_bytes = bytes(cmd)
        cmd_type = cmd[2] if len(cmd) > 2 else '?'
        chunk_size = cmd[3] if len(cmd) > 3 else 0
        offset = 0
        if len(cmd) >= 7:
            offset = cmd[4] + (cmd[5] << 8) + (cmd[6] << 16)

        label = f"AllDataCom[{i}]: cmd={cmd_type}, size={chunk_size}"
        if offset:
            label += f", offset={offset}"

        responses = send_and_show(
            send_fd, recv_fds, cmd_bytes, label, timeout_ms=500)

        for iface, resp in responses:
            if resp != cmd_bytes and len(resp) > 0:
                all_data.extend(resp)

        time.sleep(0.05)

    if all_data:
        print(f"\n--- Combined Response Data ({len(all_data)} bytes) ---")
        hexdump(all_data)
    else:
        print("\nNo data received from AllDataCom read.")


def fw_status(send_fd, recv_fds):
    """Check firmware update status."""
    print("\n=== Firmware Status Check ===")
    send_and_show(
        send_fd, recv_fds,
        bytes([0xBE, 0x74, 0x05, 0x00, 0xED]),
        "FW status check (0xBE framed)",
        timeout_ms=1000)


def send_raw_cmd(send_fd, recv_fds, hex_str):
    """Send raw bytes specified as hex."""
    # Parse hex string: "BE 00 05 00 ED" or "BE,00,05,00,ED" or "BE00050ED"
    hex_str = hex_str.replace(',', ' ').replace('0x', '').replace('0X', '')
    parts = hex_str.split()
    if len(parts) == 1 and len(parts[0]) > 2:
        # Maybe no spaces: "BE0005ED" -> split every 2 chars
        raw = parts[0]
        parts = [raw[i:i + 2] for i in range(0, len(raw), 2)]

    try:
        data = bytes(int(x, 16) for x in parts)
    except ValueError:
        print(f"ERROR: Invalid hex: {hex_str}")
        sys.exit(1)

    # Safety check: refuse to send dangerous commands
    DANGEROUS = {
        0x02: "Save to flash",
        0xFC: "Enter DFU mode",
        0xEE: "Factory reset",
        0xFE: "Start calibration",
    }
    if len(data) >= 2 and data[0] == 0xBE:
        cmd = data[1]
        if cmd in DANGEROUS:
            print(f"REFUSED: command 0x{cmd:02X} = {DANGEROUS[cmd]}")
            print("This command could modify the keyboard state.")
            print("Remove this safety check if you know what you're doing.")
            sys.exit(1)

    print("\n=== Raw Command ===")
    send_and_show(send_fd, recv_fds, data, "user-specified raw bytes",
                  timeout_ms=1000)


def main():
    parser = argparse.ArgumentParser(
        description='Wobkey Rainy 75 — WOB Protocol C Probe')
    parser.add_argument('--probe', action='store_true',
                        help='Test heartbeat + basic Protocol C commands')
    parser.add_argument('--read-keymap', action='store_true',
                        help='Read keymap via keyMapCom sequence')
    parser.add_argument('--read-all', action='store_true',
                        help='Read all data via AllDataCom (safe: skips save)')
    parser.add_argument('--fw-status', action='store_true',
                        help='Check firmware update status')
    parser.add_argument('--raw', metavar='HEX',
                        help='Send raw hex bytes (e.g., "BE 00 05 00 ED")')
    args = parser.parse_args()

    if not any([args.probe, args.read_keymap, args.read_all,
                args.fw_status, args.raw]):
        parser.print_help()
        return

    send_fd, recv_fds = open_device()

    try:
        if args.probe:
            probe(send_fd, recv_fds)
        elif args.read_keymap:
            read_keymap(send_fd, recv_fds)
        elif args.read_all:
            read_all_data(send_fd, recv_fds)
        elif args.fw_status:
            fw_status(send_fd, recv_fds)
        elif args.raw:
            send_raw_cmd(send_fd, recv_fds, args.raw)
    finally:
        os.close(send_fd)
        for _, fd in recv_fds:
            os.close(fd)


if __name__ == '__main__':
    main()
