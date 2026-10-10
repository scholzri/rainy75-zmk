#!/usr/bin/env python3
"""
Firmware update for the Rainy 75 over USB or Bluetooth (mcumgr SMP image management).

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
--port to name it. --ble updates over Bluetooth instead (needs bleak, and a
computer the keyboard is paired with; --address picks the keyboard if there is
more than one): about 30 to 60 s for an image, useful when USB is not at hand. A test image runs once: it stays only if it (or you)
confirms it, else the next reset goes back to the previous image.
"""

import argparse
import asyncio
import hashlib
import struct
import sys
import time

import rainy75_rgb
import restore_original as smp

IMG_MGMT_STATE = 0
IMG_MGMT_UPLOAD = 1
OS_MGMT_RESET = 5
CHUNK = 420          # image bytes per request with a 512 B SMP buffer (the
                     # firmware before 0.4.1, which cannot report its size)
OS_MGMT_PARAMS = 6
CHUNK_SPARE = 64     # SMP header and CBOR keys of the largest request


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

    def chunk_size(self):
        """Image bytes per request: from the keyboard's SMP buffer size (os
        group "mcumgr params", firmware 0.4.1 and later), else CHUNK."""
        try:
            r = self.request(smp.SMP_OP_READ_REQ, smp.SMP_GROUP_OS,
                             OS_MGMT_PARAMS, [], retries=0)
        except (TimeoutError, RuntimeError):
            return CHUNK
        buf = r.get("buf_size", 0)
        return max(CHUNK, buf - CHUNK_SPARE) if r.get("rc", 0) == 0 else CHUNK

    def images(self):
        return self.request(smp.SMP_OP_READ_REQ, smp.SMP_GROUP_IMG,
                            IMG_MGMT_STATE, []).get("images", [])

    def upload(self, data, progress=True):
        chunk = self.chunk_size()
        off, t0 = 0, time.time()
        while off < len(data):
            pairs = [("off", smp.cbor_encode_uint(off)),
                     ("data", smp.cbor_encode_bstr(data[off:off + chunk]))]
            if off == 0:
                pairs = [("image", smp.cbor_encode_uint(0)),
                         ("len", smp.cbor_encode_uint(len(data)))] + pairs
            # The first request erases slot 1 first: give it time.
            r = self.request(smp.SMP_OP_WRITE_REQ, smp.SMP_GROUP_IMG,
                             IMG_MGMT_UPLOAD, pairs,
                             timeout_s=30.0 if off == 0 else 5.0)
            if r.get("rc", 0) != 0:
                raise RuntimeError(f"upload refused at offset {off}: {r}")
            off = r.get("off", off + chunk)
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


class BleDfu(Dfu):
    """The same requests over Bluetooth (rainy75_rgb_ble's SMP client).
    BlueZ now and then drops a reply notification: a request that times out
    is sent again. That is safe for the upload too, because the keyboard
    answers a repeated chunk with the offset it already has."""

    RETRIES = 5
    REPLY_S = 5.0    # wait for an upload reply before going on from the confirmed offset
    WINDOW = 2       # upload requests in flight: 2 and 3 are equally fast (the
                     # keyboard is the limit then), 4 runs out of its SMP buffers

    def __init__(self, address=None):
        import rainy75_rgb_ble          # needs bleak, so only for --ble
        self.loop = asyncio.new_event_loop()
        self.kb = rainy75_rgb_ble.Rainy75BLE(address=address)
        # Our own notification handler (set before connect, which subscribes
        # it): it keeps every reply frame for the pipelined upload and still
        # hands it to the client's one-request path.
        self.frames = asyncio.Queue()
        self._rx = bytearray()
        self.kb._on_notify = self._on_notify
        try:
            self.loop.run_until_complete(self.kb.connect())
        except BaseException:
            self.loop.close()
            raise
        # The negotiated ATT MTU: BlueZ shows it as the SMP characteristic's
        # "MTU" property. bleak's own _acquire_mtu() is not used: it picks the
        # first writable characteristic, sometimes a HID one that BlueZ owns
        # (NotAuthorized). Without the MTU the client writes 20-byte pieces
        # and an image takes about 2.3x longer.
        try:
            char = self.kb.client.services.get_characteristic(rainy75_rgb_ble.SMP_CHAR_UUID)
            mtu = int(char.obj[1].get("MTU", 0))
            if mtu > 23:
                self.kb.client._backend._mtu_size = mtu
        except Exception:
            pass                            # keep the 20-byte pieces

    def close(self):
        try:
            self.loop.run_until_complete(self.kb.disconnect())
        except Exception:
            pass
        self.loop.close()

    def request(self, op, group, cmd, pairs, timeout_s=5.0, retries=RETRIES):
        self.kb.timeout = timeout_s
        for attempt in range(1 + retries):
            try:
                return self.loop.run_until_complete(
                    self.kb._request(op, cmd, pairs, group=group))
            except asyncio.TimeoutError:
                if attempt == retries:
                    raise TimeoutError("no answer over Bluetooth") from None

    def _on_notify(self, _char, data):
        self._rx += data
        while len(self._rx) >= 8:
            length, = struct.unpack_from(">H", self._rx, 2)
            if len(self._rx) < 8 + length:
                return
            frame = bytes(self._rx[:8 + length])
            del self._rx[:8 + length]
            self.frames.put_nowait(frame)
            self.kb._response = frame
            self.kb._resp_event.set()

    async def _send(self, op, group, cmd, pairs):
        """Write one request without waiting; returns its sequence number."""
        import rainy75_rgb_ble
        kb = self.kb
        body = rainy75_rgb._cbor_map(pairs)
        seq = kb.seq & 0xFF
        kb.seq += 1
        frame = struct.pack(">BBHHBB", op, 0, len(body), group, seq, cmd) + body
        size = max(20, (kb.client.mtu_size or 23) - 3)
        for i in range(0, len(frame), size):
            await kb.client.write_gatt_char(rainy75_rgb_ble.SMP_CHAR_UUID,
                                            frame[i:i + size], response=False)
        return seq

    def upload(self, data, progress=True):
        chunk = self.chunk_size()
        self.loop.run_until_complete(self._upload(data, progress, chunk))

    async def _upload(self, data, progress, chunk=CHUNK):
        """Pipelined upload: up to WINDOW chunks in flight. Every reply
        carries the offset the keyboard has reached (also for a chunk at the
        wrong offset, which it skips), so replies are cumulative: on a gap or
        a lost reply the upload goes on from the last offset the keyboard
        confirmed."""
        while not self.frames.empty():
            self.frames.get_nowait()
        size, t0 = len(data), time.time()
        acked = 0                   # offset the keyboard confirmed
        nxt = 0                     # next offset to send
        inflight = {}               # seq -> offset the keyboard reaches with it
        silent = 0                  # timeouts in a row
        first = True
        while acked < size:
            window = 1 if first else self.WINDOW   # the first one erases slot 1
            while len(inflight) < window and nxt < size:
                pairs = [("off", smp.cbor_encode_uint(nxt)),
                         ("data", smp.cbor_encode_bstr(data[nxt:nxt + chunk]))]
                if nxt == 0:
                    pairs = [("image", smp.cbor_encode_uint(0)),
                             ("len", smp.cbor_encode_uint(size))] + pairs
                seq = await self._send(smp.SMP_OP_WRITE_REQ, smp.SMP_GROUP_IMG,
                                       IMG_MGMT_UPLOAD, pairs)
                inflight[seq] = min(nxt + chunk, size)
                nxt = inflight[seq]
            try:
                frame = await asyncio.wait_for(self.frames.get(),
                                               30.0 if first else self.REPLY_S)
            except asyncio.TimeoutError:
                silent += 1
                if silent > self.RETRIES:
                    raise TimeoutError("no answer over Bluetooth") from None
                inflight.clear()            # resend from the confirmed offset
                nxt = acked
                continue
            silent = 0
            seq = frame[6]
            reply, _ = rainy75_rgb._cbor_decode(frame[8:])
            if reply.get("rc", 0) != 0:
                raise RuntimeError(f"upload refused at offset {acked}: {reply}")
            off = reply.get("off", acked)
            acked = max(acked, off)
            want = inflight.pop(seq, None)
            if want is None:
                continue                    # a reply from before a resend
            first = False
            if off != want:                 # the keyboard skipped a chunk
                inflight.clear()
                nxt = acked
            if not inflight:
                nxt = max(nxt, acked)
            if progress:
                print(f"\r  {100 * acked // size:3d} %  {acked}/{size} B", end="", flush=True)
        dt = time.time() - t0
        if progress:
            print(f"\r  100 %  {size} B in {dt:.1f} s ({size / dt / 1024:.1f} KiB/s)")

    def reset(self):
        try:
            self.request(smp.SMP_OP_WRITE_REQ, smp.SMP_GROUP_OS, OS_MGMT_RESET,
                         [], timeout_s=2.0, retries=0)
        except (TimeoutError, RuntimeError):
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
    ap.add_argument("--ble", action="store_true", help="over Bluetooth instead of USB")
    ap.add_argument("--address", help="with --ble: the keyboard's Bluetooth address")
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

    if a.ble:
        port = "Bluetooth"
        try:
            dfu = BleDfu(a.address)
        except Exception as e:
            sys.exit(f"No Bluetooth connection to the Rainy 75: {e}")
    else:
        port = a.port or rainy75_rgb.find_port()
        if not port:
            sys.exit("No Rainy 75 found on USB (use --port, or --ble).")
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
