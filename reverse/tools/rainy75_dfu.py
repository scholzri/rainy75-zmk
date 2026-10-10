#!/usr/bin/env python3
"""
Firmware update for the Rainy 75 over USB (mcumgr SMP image management).

About 7x faster than the mcumgr CLI over USB (313 KB in about 12 s instead of
85 s): the CLI pauses about 20 ms after every 127-byte serial line, a pacing
meant for real UARTs. Over USB CDC ACM the bus has its own flow control, so
the lines go out back to back. Same protocol, same firmware; zero
dependencies beyond the Python stdlib.

Usage:
    rainy75_dfu.py upload build/zephyr/zmk.signed.bin            # upload to slot 1
    rainy75_dfu.py upload build/zephyr/zmk.signed.bin --test --reset
                                       # upload, mark for test, reset (the usual flow)
    rainy75_dfu.py list                # images and their flags
    rainy75_dfu.py test [HASH]         # mark slot 1 (or HASH) for a test boot
    rainy75_dfu.py confirm [HASH]      # make the running image (or HASH) permanent
    rainy75_dfu.py reset

The port is found by USB product name (the console port, interface 0); use
--port to name it. A test image runs once: it stays only if it (or you)
confirms it, else the next reset goes back to the previous image.
"""

import argparse
import hashlib
import struct
import sys
import time

import rainy75_rgb
import restore_original as smp

IMG_MGMT_STATE = 0
IMG_MGMT_UPLOAD = 1
OS_MGMT_RESET = 5
CHUNK = 420          # image bytes per request; ~26 KiB/s, larger gains nothing
                     # (the firmware's receive path is CPU bound per byte, see
                     # docs/zmk-firmware.md) and must fit the 512 B SMP buffer


def image_hash(data):
    """The hash MCUboot reports for a signed image: SHA-256 over header +
    image body, without the trailing TLVs."""
    hdr_size, = struct.unpack_from("<H", data, 8)
    img_size, = struct.unpack_from("<I", data, 12)
    return hashlib.sha256(data[:hdr_size + img_size]).digest()


class Dfu:
    def __init__(self, port):
        self.client = smp.SMPClient(port)
        self.client.open()

    def close(self):
        self.client.close()

    def request(self, op, group, cmd, pairs, timeout_s=5.0, retries=2):
        """One SMP request; returns the decoded response map."""
        c = self.client
        payload = smp.cbor_encode_map(pairs)
        seq = c.seq
        hdr = smp.smp_build_header(op, 0, len(payload), group, seq, cmd)
        c.seq = (c.seq + 1) & 0xFF
        frames = smp.smp_serial_encode(hdr + payload)
        for attempt in range(1 + retries):
            c._rxbuf.clear()
            smp.termios.tcflush(c.fd, smp.termios.TCIFLUSH)
            for frame in frames:            # back to back: USB flow control
                smp.os.write(c.fd, frame)
            try:
                # Skip replies to someone else's request (a stale one left
                # in the port by a program that had it open): match the seq.
                end = time.monotonic() + timeout_s
                while True:
                    raw = c._read_response(
                        timeout_s=max(0.1, end - time.monotonic()))
                    if len(raw) > 6 and raw[6] == seq:
                        break
                break
            except TimeoutError:
                if attempt == retries:
                    raise
        _, _, length, _, _, _ = struct.unpack_from(">BBHHBB", raw, 0)
        body = raw[smp.SMP_HDR_SIZE:smp.SMP_HDR_SIZE + length]
        return rainy75_rgb._cbor_decode(body)[0] if body else {}

    def images(self):
        return self.request(smp.SMP_OP_READ_REQ, smp.SMP_GROUP_IMG,
                            IMG_MGMT_STATE, []).get("images", [])

    def upload(self, data, progress=True):
        off, t0 = 0, time.time()
        while off < len(data):
            pairs = [("off", smp.cbor_encode_uint(off)),
                     ("data", smp.cbor_encode_bstr(data[off:off + CHUNK]))]
            if off == 0:
                pairs = [("image", smp.cbor_encode_uint(0)),
                         ("len", smp.cbor_encode_uint(len(data)))] + pairs
            # The first request erases slot 1 first: give it time.
            r = self.request(smp.SMP_OP_WRITE_REQ, smp.SMP_GROUP_IMG,
                             IMG_MGMT_UPLOAD, pairs,
                             timeout_s=30.0 if off == 0 else 5.0)
            if r.get("rc", 0) != 0:
                raise RuntimeError(f"upload refused at offset {off}: {r}")
            off = r.get("off", off + CHUNK)
            if progress:
                pct = 100 * off // len(data)
                print(f"\r  {pct:3d} %  {off}/{len(data)} B", end="", flush=True)
        dt = time.time() - t0
        if progress:
            print(f"\r  100 %  {len(data)} B in {dt:.1f} s "
                  f"({len(data) / dt / 1024:.1f} KiB/s)")

    def set_state(self, hash_bytes, confirm):
        pairs = [("confirm", b"\xf5" if confirm else b"\xf4")]
        if hash_bytes is not None:
            pairs.insert(0, ("hash", smp.cbor_encode_bstr(hash_bytes)))
        r = self.request(smp.SMP_OP_WRITE_REQ, smp.SMP_GROUP_IMG,
                         IMG_MGMT_STATE, pairs)
        if r.get("rc", 0) != 0:
            raise RuntimeError(f"image state refused: {r}")
        return r.get("images", [])

    def reset(self):
        try:
            self.request(smp.SMP_OP_WRITE_REQ, smp.SMP_GROUP_OS, OS_MGMT_RESET,
                         [], timeout_s=2.0, retries=0)
        except TimeoutError:
            pass                            # the reset may beat the response


def print_images(images):
    for i in images:
        flags = [f for f in ("active", "confirmed", "pending", "permanent")
                 if i.get(f)]
        print(f"  slot {i.get('slot')}: {i.get('version', '?'):10s} "
              f"{i.get('hash', b'').hex()[:16]}  {' '.join(flags)}")


def parse_hash(text):
    return bytes.fromhex(text) if text else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--port", help="serial port (default: found by USB name)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    up = sub.add_parser("upload", help="upload a signed image to slot 1")
    up.add_argument("file")
    up.add_argument("--test", action="store_true", help="mark it for a test boot")
    up.add_argument("--reset", action="store_true", help="reset afterwards")
    sub.add_parser("list", help="show the images")
    t = sub.add_parser("test", help="mark an image for a test boot")
    t.add_argument("hash", nargs="?", help="default: the image in slot 1")
    c = sub.add_parser("confirm", help="make an image permanent")
    c.add_argument("hash", nargs="?", help="default: the running image")
    sub.add_parser("reset", help="reset the keyboard")
    a = ap.parse_args()

    port = a.port or rainy75_rgb.find_port()
    if not port:
        sys.exit("No Rainy 75 found on USB (use --port).")
    dfu = Dfu(port)
    try:
        if a.cmd == "upload":
            data = open(a.file, "rb").read()
            want = image_hash(data)
            print(f"Uploading {a.file} to slot 1 over {port}")
            dfu.upload(data)
            slot1 = [i for i in dfu.images() if i.get("slot") == 1]
            if not slot1 or slot1[0].get("hash") != want:
                sys.exit("Slot 1 does not hold the uploaded image (hash mismatch).")
            print(f"  slot 1 holds {want.hex()[:16]}")
            if a.test:
                dfu.set_state(want, confirm=False)
                print("  marked for a test boot")
            if a.reset:
                dfu.reset()
                print("  reset")
        elif a.cmd == "list":
            print_images(dfu.images())
        elif a.cmd == "test":
            h = parse_hash(a.hash)
            if h is None:
                slot1 = [i for i in dfu.images() if i.get("slot") == 1]
                if not slot1:
                    sys.exit("No image in slot 1.")
                h = slot1[0]["hash"]
            print_images(dfu.set_state(h, confirm=False))
        elif a.cmd == "confirm":
            print_images(dfu.set_state(parse_hash(a.hash), confirm=True))
        elif a.cmd == "reset":
            dfu.reset()
    finally:
        dfu.close()


if __name__ == "__main__":
    main()
