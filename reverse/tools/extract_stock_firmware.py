#!/usr/bin/env python3
"""
Extract the stock Rainy 75 OTA firmware image from the official Wobkey updater (.exe).

The updater is a .NET application that embeds the firmware as a managed resource named
`code_2M` (a 2 MB flash image: a short wrapper, then the Telink OTA payload, then 0xFF
padding). This script pulls that resource, locates the Telink boot header by its `TLNK`
magic, reads the image size from the header, and writes out just the OTA payload —
`firmware_ota.bin`, the image `restore_stock.sh` flashes to return to stock.

The firmware itself is proprietary (Telink / Wobkey / Evision) and is NOT shipped in this
repository. Download the official updater yourself (see INSTALL.md) and run this on it.

Requires:  pip install dnfile

Usage:
    python3 extract_stock_firmware.py "Rainy 75 ISO firmware.exe"
    python3 extract_stock_firmware.py updater.exe -o reverse/firmware/firmware_ota.bin
"""
import argparse
import hashlib
import struct
import sys

RESOURCE = "code_2M"
TLNK = b"KNLT"  # 'TLNK' little-endian; sits at OTA payload offset 0x20


def find_code_resource(exe_path):
    try:
        import dnfile
    except ImportError:
        sys.exit("Missing dependency. Install it with:  pip install dnfile")

    pe = dnfile.dnPE(exe_path)
    for res in pe.net.resources:
        for entry in getattr(res.data, "entries", None) or []:
            if entry.name == RESOURCE and isinstance(entry.data, (bytes, bytearray)):
                return bytes(entry.data)
    sys.exit(f"Resource {RESOURCE!r} not found — is this the Rainy 75 firmware updater?")


def main():
    ap = argparse.ArgumentParser(
        description="Extract firmware_ota.bin from the official Rainy 75 updater .exe")
    ap.add_argument("exe", help="the official Wobkey firmware updater (.exe)")
    ap.add_argument("-o", "--output", default="firmware_ota.bin",
                    help="output path (default: firmware_ota.bin)")
    args = ap.parse_args()

    code = find_code_resource(args.exe)

    pos = code.find(TLNK)
    if pos < 0x20:
        sys.exit("Telink boot header (TLNK magic) not found in code_2M — unexpected format.")
    start = pos - 0x20
    size = struct.unpack_from("<I", code, start + 0x18)[0]
    if not 0 < size <= len(code) - start:
        sys.exit(f"Implausible OTA size 0x{size:X} — aborting.")

    payload = code[start:start + size]
    with open(args.output, "wb") as f:
        f.write(payload)

    print(f"Wrote {args.output} — {len(payload)} bytes "
          f"(sha256 {hashlib.sha256(payload).hexdigest()[:16]}…)")
    print("This is your stock restore image; restore_stock.sh flashes it.")


if __name__ == "__main__":
    main()
