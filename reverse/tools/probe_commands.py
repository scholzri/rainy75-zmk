#!/usr/bin/env python3
"""
Probe all HID interfaces of the Rainy 75 for hidden commands.
Safe: skips cmd=0x02 on interface 2 (OTA trigger).
"""
import os
import sys
import glob
import select
import struct
import time

VID = 0x320F
PID = 0x5055


def find_hidraw_for_interface(iface_num):
    hid_id = f"{VID:08X}:{PID:08X}".upper()
    for hid_dev in glob.glob("/sys/bus/hid/devices/*"):
        uevent_path = os.path.join(hid_dev, "uevent")
        if not os.path.isfile(uevent_path):
            continue
        with open(uevent_path) as f:
            uevent = f.read()
        if hid_id not in uevent.upper():
            continue
        if f"input{iface_num}" not in uevent:
            continue
        hidraw_dir = os.path.join(hid_dev, "hidraw")
        if os.path.isdir(hidraw_dir):
            nodes = os.listdir(hidraw_dir)
            if nodes:
                return f"/dev/{nodes[0]}"
    return None


def send_and_recv(fd, packet, timeout_ms=500):
    try:
        os.write(fd, packet)
    except OSError as e:
        return [f"WRITE_ERROR: {e}"]
    responses = []
    deadline = time.time() + timeout_ms / 1000.0
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        ready, _, _ = select.select([fd], [], [], remaining)
        if ready:
            try:
                data = os.read(fd, 256)
                responses.append(data)
            except OSError as e:
                responses.append(f"READ_ERROR: {e}")
                break
        else:
            break
    return responses


def hex_line(data, max_bytes=48):
    if isinstance(data, str):
        return f"    {data}"
    hex_str = ' '.join(f'{b:02x}' for b in data[:max_bytes])
    extra = f" (+{len(data)-max_bytes})" if len(data) > max_bytes else ""
    return f"    {hex_str}{extra}"


def drain(fd, timeout_ms=100):
    while True:
        ready, _, _ = select.select([fd], [], [], timeout_ms / 1000.0)
        if ready:
            try:
                os.read(fd, 256)
            except OSError:
                break
        else:
            break


def is_interesting(resp):
    """Check if response has any non-zero data beyond the first 2 bytes (report_id + cmd echo)."""
    if isinstance(resp, str):
        return True
    return any(b != 0 for b in resp[2:])


def probe_iface2():
    """Probe Interface 2 (0xFFEF) — skip cmd 0x02 (OTA)."""
    path = find_hidraw_for_interface(2)
    if not path:
        print("  Interface 2: NOT FOUND")
        return
    print(f"\n{'='*70}")
    print(f"Interface 2: Vendor 0xFFEF — {path}")
    print(f"{'='*70}")
    fd = os.open(path, os.O_RDWR)
    drain(fd)

    # Basic command scan — SKIP 0x02 (OTA trigger!)
    print("\n  cmd scan (report_id=0x05, skip 0x02):")
    for cmd in list(range(0x00, 0x02)) + list(range(0x03, 0x20)):
        packet = bytes([0x05, cmd, 0x00, 0x00]) + bytes(60)
        responses = send_and_recv(fd, packet, timeout_ms=300)
        for resp in responses:
            if is_interesting(resp):
                print(f"  cmd=0x{cmd:02X} INTERESTING:")
                print(hex_line(resp))
            else:
                pass  # just echoed cmd back
        time.sleep(0.02)

    drain(fd)

    # cmd=0x01 subcmd variations
    print("\n  cmd=0x01 subcmd scan:")
    for sub in range(0x20):
        packet = bytes([0x05, 0x01, sub, 0x00]) + bytes(60)
        responses = send_and_recv(fd, packet, timeout_ms=300)
        for resp in responses:
            if is_interesting(resp):
                print(f"  cmd=0x01 sub=0x{sub:02X}:")
                print(hex_line(resp))
        time.sleep(0.02)

    drain(fd)

    # Try different payload structures for each non-OTA cmd
    print("\n  cmd scan with payload byte[3]=0x01:")
    for cmd in [0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A]:
        packet = bytes([0x05, cmd, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00]) + bytes(56)
        responses = send_and_recv(fd, packet, timeout_ms=300)
        for resp in responses:
            if is_interesting(resp):
                print(f"  cmd=0x{cmd:02X} payload=[01,00,00,00]:")
                print(hex_line(resp))
        time.sleep(0.02)

    os.close(fd)


def probe_iface3():
    """Probe Interface 3 (0xFF1C) — the other vendor channel."""
    path = find_hidraw_for_interface(3)
    if not path:
        print("  Interface 3: NOT FOUND")
        return
    print(f"\n{'='*70}")
    print(f"Interface 3: Vendor 0xFF1C — {path}")
    print(f"{'='*70}")
    fd = os.open(path, os.O_RDWR)
    drain(fd)

    # This interface has Report ID 4, asymmetric: 63-byte OUT, 7-byte IN
    # Try all cmd bytes with report ID 4
    print("\n  cmd scan (report_id=0x04):")
    for cmd in range(0x20):
        packet = bytes([0x04, cmd, 0x00, 0x00]) + bytes(60)
        responses = send_and_recv(fd, packet, timeout_ms=300)
        for resp in responses:
            print(f"  cmd=0x{cmd:02X} -> {len(resp) if isinstance(resp, bytes) else resp}:")
            print(hex_line(resp))
        time.sleep(0.02)

    drain(fd)

    # Try without report ID prefix (some hidraw implementations add it automatically)
    print("\n  cmd scan (no explicit report_id):")
    for cmd in range(0x10):
        packet = bytes([cmd, 0x00, 0x00]) + bytes(61)
        responses = send_and_recv(fd, packet, timeout_ms=300)
        for resp in responses:
            print(f"  byte0=0x{cmd:02X}:")
            print(hex_line(resp))
        time.sleep(0.02)

    os.close(fd)


def probe_via():
    """Probe Interface 1 (VIA 0xFF60) for undocumented commands."""
    path = find_hidraw_for_interface(1)
    if not path:
        print("  Interface 1: NOT FOUND")
        return
    print(f"\n{'='*70}")
    print(f"Interface 1: VIA 0xFF60 — {path}")
    print(f"{'='*70}")
    fd = os.open(path, os.O_RDWR)
    drain(fd)

    # VIA protocol: 32-byte packets, first byte = command
    # Known: 0x01=get_protocol_version, 0x05=dynamic_keymap_macro_get_count, etc.
    # Scan full range
    print("\n  VIA command scan (0x00-0xFF):")
    for cmd in range(0x100):
        packet = bytes([cmd]) + bytes(31)
        responses = send_and_recv(fd, packet, timeout_ms=200)
        for resp in responses:
            if isinstance(resp, bytes) and any(b != 0 and b != 0xFF for b in resp):
                # Non-trivial response
                print(f"  via=0x{cmd:02X}:")
                print(hex_line(resp))
        time.sleep(0.01)

    os.close(fd)


def main():
    print("Rainy 75 HID Command Probe")
    print("=" * 70)
    probe_iface2()
    probe_iface3()
    probe_via()
    print(f"\n{'='*70}")
    print("Done.")


if __name__ == '__main__':
    main()
