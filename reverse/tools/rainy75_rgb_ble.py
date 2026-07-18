#!/usr/bin/env python3
"""
BLE transport for the Rainy 75 rgb_mgmt host control (mcumgr group 65).

Same commands as rainy75_rgb.py, but over the SMP GATT service — the BLE
mcumgr transport the firmware already ships for DFU backup — instead of USB
CDC-ACM serial. Key names, groups, and CBOR are imported from rainy75_rgb.py;
the only extra dependency is `bleak` (pip install bleak).

SMP over BLE GATT is simpler than the serial console framing: raw SMP frames
(8-byte header + CBOR payload) written to the SMP characteristic, chunked to
ATT MTU; responses arrive as notifications. No base64, no CRC16. Requests
larger than one MTU rely on CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY=y (set in
conf/app.conf). The link must be encrypted (Zephyr's SMP BT transport
requires it) — a keyboard bonded for BLE HID already satisfies that.

Usage:
    python3 rainy75_rgb_ble.py info
    python3 rainy75_rgb_ble.py set F1 F2 WASD --color ff0000
    python3 rainy75_rgb_ble.py fill --color 002040
    python3 rainy75_rgb_ble.py pulse --color ff5f00
    python3 rainy75_rgb_ble.py clear
    python3 rainy75_rgb_ble.py --address XX:XX:XX:XX:XX:XX info
"""

import argparse
import asyncio
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rainy75_rgb import (  # noqa: E402
    GROUPS, _ROWS, key_to_position,
    _cbor_map, _cbor_uint, _cbor_bstr, _cbor_decode,
    RGB_GROUP, SMP_OP_READ, SMP_OP_WRITE,
    CMD_SET, CMD_FILL, CMD_CLEAR, CMD_INFO,
)

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.device import BLEDevice
except ImportError:
    sys.exit("bleak is required for BLE control: pip install bleak")

SMP_SERVICE_UUID = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
SMP_CHAR_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48"
DEVICE_NAME = "Rainy 75 Pro"


async def _find_known_device(address=None, name=None):
    """Resolve the device from BlueZ's registry instead of a scan.

    The keyboard's normal state is *connected* (it is a keyboard), and a
    connected peripheral does not advertise — so bleak's scan-based
    find_device_by_address can never see it. BlueZ already has the device
    object; hand its path straight to bleak."""
    from dbus_fast.aio import MessageBus
    from dbus_fast import BusType, Message

    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    try:
        reply = await bus.call(Message(
            destination="org.bluez", path="/",
            interface="org.freedesktop.DBus.ObjectManager",
            member="GetManagedObjects"))
        for path, ifaces in reply.body[0].items():
            dev = ifaces.get("org.bluez.Device1")
            if not dev:
                continue
            addr = dev["Address"].value
            dname = dev["Name"].value if "Name" in dev else None
            if (address and addr.upper() == address.upper()) or \
               (address is None and name and dname == name):
                props = {k: v.value for k, v in dev.items()}
                return BLEDevice(addr, dname, {"path": path, "props": props})
    finally:
        bus.disconnect()
    return None


class Rainy75BLE:
    """SMP-over-BLE client for the rgb_mgmt group (async)."""

    def __init__(self, address=None, name=DEVICE_NAME, timeout=10.0):
        self.address = address
        self.name = name
        self.timeout = timeout
        self.client = None
        self.seq = 0
        self._rxbuf = bytearray()
        self._response = None
        self._resp_event = asyncio.Event()
        self._we_connected = False  # did WE bring the link up?

    async def connect(self):
        # Registry first (connected keyboards don't advertise), scan fallback.
        target = await _find_known_device(self.address, self.name)
        already_up = (target is not None and
                      target.details["props"].get("Connected", False))
        if target is None:
            if self.address is not None:
                target = await BleakScanner.find_device_by_address(
                    self.address, timeout=self.timeout)
            else:
                target = await BleakScanner.find_device_by_name(
                    self.name, timeout=self.timeout)
        if target is None:
            raise RuntimeError(
                f"{self.address or self.name!r} not found "
                "(keyboard paired and BLE switch on?)")
        self.client = BleakClient(target, timeout=self.timeout)
        await self.client.connect()
        self._we_connected = not already_up
        await self.client.start_notify(SMP_CHAR_UUID, self._on_notify)

    async def disconnect(self):
        if self.client is None:
            return
        try:
            await self.client.stop_notify(SMP_CHAR_UUID)
        except Exception:
            pass
        if self._we_connected:
            # We established the link, so we take it down again.
            await self.client.disconnect()
        else:
            # The keyboard was already connected (live BLE HID session!).
            # bleak's disconnect() calls BlueZ Device1.Disconnect, which
            # tears down the WHOLE ACL link — kicking the user's keyboard
            # input off the air for the seconds it takes to reconnect. Only
            # close our private D-Bus session; BlueZ drops our notify
            # subscription with it and the HID connection stays untouched.
            bus = getattr(self.client._backend, "_bus", None)
            if bus is not None:
                try:
                    bus.disconnect()
                except Exception:
                    pass
        self.client = None

    def _on_notify(self, _char, data):
        # Responses may span several notifications; reassemble on the SMP
        # header's length field (bytes 2-3, big-endian).
        self._rxbuf += data
        if len(self._rxbuf) < 8:
            return
        (length,) = struct.unpack_from(">H", self._rxbuf, 2)
        if len(self._rxbuf) < 8 + length:
            return
        frame = bytes(self._rxbuf[:8 + length])
        del self._rxbuf[:8 + length]
        self._response = frame
        self._resp_event.set()

    async def _request(self, op, cmd, payload_map):
        cbor = _cbor_map(payload_map)
        hdr = struct.pack(">BBHHBB", op, 0, len(cbor), RGB_GROUP,
                          self.seq & 0xFF, cmd)
        self.seq += 1
        frame = hdr + cbor

        self._rxbuf.clear()
        self._response = None
        self._resp_event.clear()

        # Chunk to ATT MTU (mtu_size includes the 3-byte ATT header). On
        # BlueZ the negotiated MTU isn't exposed without an AcquireWrite, so
        # this typically falls back to 20 B chunks — the firmware's SMP
        # reassembly (CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY) handles that,
        # and at these payload sizes the difference is imperceptible.
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            maxc = max(20, (self.client.mtu_size or 23) - 3)
        for i in range(0, len(frame), maxc):
            await self.client.write_gatt_char(
                SMP_CHAR_UUID, frame[i:i + maxc], response=False)

        await asyncio.wait_for(self._resp_event.wait(), self.timeout)
        body, _ = _cbor_decode(self._response[8:])
        rc = body.get("rc", 0)
        if rc != 0:
            raise RuntimeError(f"device rc={rc}")
        return body

    # --- Public API (mirrors rainy75_rgb.Rainy75) ---

    async def set_positions(self, mapping):
        quads = bytearray()
        for pos, (r, g, b) in mapping.items():
            if not 0 <= pos <= 82:
                raise ValueError(f"position {pos} out of range")
            quads += bytes([pos, r & 0xFF, g & 0xFF, b & 0xFF])
        for i in range(0, len(quads), 48 * 4):
            await self._request(SMP_OP_WRITE, CMD_SET,
                                [("px", _cbor_bstr(bytes(quads[i:i + 48 * 4])))])

    async def set_keys(self, names, color):
        positions = {}
        for name in names:
            if name.upper() in GROUPS:
                for member in GROUPS[name.upper()]:
                    positions[key_to_position(member)] = color
            else:
                positions[key_to_position(name)] = color
        await self.set_positions(positions)

    async def fill(self, color):
        r, g, b = color
        await self._request(SMP_OP_WRITE, CMD_FILL,
                            [("r", _cbor_uint(r)), ("g", _cbor_uint(g)),
                             ("b", _cbor_uint(b))])

    async def clear(self):
        await self._request(SMP_OP_WRITE, CMD_CLEAR, [])

    async def info(self):
        return await self._request(SMP_OP_READ, CMD_INFO, [])

    async def pulse(self, color, times=1, steps=12, period=0.9):
        r, g, b = color
        half = period / 2
        for _ in range(times):
            for i in list(range(1, steps + 1)) + list(range(steps - 1, -1, -1)):
                f = i / steps
                await self.fill((int(r * f), int(g * f), int(b * f)))
                await asyncio.sleep(half / steps)
        await self.clear()


# --------------------------------------------------------------------------
# CLI (same subcommands as rainy75_rgb.py)
# --------------------------------------------------------------------------

def _parse_color(s):
    s = s.lstrip("#")
    if len(s) != 6:
        raise argparse.ArgumentTypeError("color must be RRGGBB hex")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


async def _run(args):
    kb = Rainy75BLE(address=args.address)
    await kb.connect()
    try:
        if args.cmd == "set":
            await kb.set_keys(args.keys, args.color)
        elif args.cmd == "fill":
            await kb.fill(args.color)
        elif args.cmd == "pulse":
            await kb.pulse(args.color, times=args.times)
        elif args.cmd == "clear":
            await kb.clear()
        elif args.cmd == "info":
            print(await kb.info())
    finally:
        await kb.disconnect()


def main():
    ap = argparse.ArgumentParser(
        description="Rainy 75 per-key RGB host control over BLE")
    ap.add_argument("--address", help="BLE address (default: scan by name)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_set = sub.add_parser("set", help="light specific keys/groups")
    p_set.add_argument("keys", nargs="+",
                       help=f"key names or groups ({', '.join(GROUPS)})")
    p_set.add_argument("--color", type=_parse_color, default=(255, 0, 0),
                       help="RRGGBB hex (default ff0000)")

    p_fill = sub.add_parser("fill", help="all keys one color")
    p_fill.add_argument("--color", type=_parse_color, required=True)

    p_pulse = sub.add_parser("pulse", help="pulse whole board, then clear")
    p_pulse.add_argument("--color", type=_parse_color, required=True)
    p_pulse.add_argument("--times", type=int, default=1)

    sub.add_parser("clear", help="back to normal effects")
    sub.add_parser("info", help="query state")

    p_keys = sub.add_parser("keys", help="list key names and groups")

    args = ap.parse_args()
    if args.cmd == "keys":
        for i, row in enumerate(_ROWS):
            print(f"Row {i}: {' '.join(row)}")
        print(f"Groups: {', '.join(GROUPS)}")
        return

    asyncio.run(_run(args))


if __name__ == "__main__":
    main()
