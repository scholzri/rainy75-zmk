#!/usr/bin/env python3
"""
Read and change the Rainy 75's runtime settings (mcumgr group 67) over USB
or Bluetooth: the protocol of docs/config-protocol.md, the same the config
page uses.

Usage:
    rainy75_cfg.py info                 # protocol version, effects, change counter
    rainy75_cfg.py list                 # every setting with its type and range
    rainy75_cfg.py get [KEY ...]        # all values, or the named ones
    rainy75_cfg.py set KEY VALUE        # e.g. set rgb.effect plasma
    rainy75_cfg.py reset [KEY ...]      # back to the defaults (all, or the named ones)
    rainy75_cfg.py --ble get            # over Bluetooth (needs bleak; see rainy75_rgb_ble.py)

Values: on/off for switches, numbers, names for choices, colours as
#RRGGBB, lists as comma-separated names (set rgb.cycle solid,plasma,wave).
"""

import argparse
import asyncio
import sys

import rainy75_rgb
import restore_original as smp

GROUP = 67
CMD_INFO, CMD_LIST, CMD_GET, CMD_SET, CMD_RESET = range(5)
READ, WRITE = smp.SMP_OP_READ_REQ, smp.SMP_OP_WRITE_REQ

RC_TEXT = {3: "invalid value", 5: "unknown setting", 7: "reply too large",
           11: "read-only setting"}


class DeviceError(Exception):
    def __init__(self, rc):
        super().__init__(f"keyboard refused: {RC_TEXT.get(rc, 'error')} (rc {rc})")
        self.rc = rc


def cbor(v):
    """CBOR for a request field: bool, unsigned int, text, or a list of those."""
    if isinstance(v, bool):
        return b"\xf5" if v else b"\xf4"
    if isinstance(v, int):
        return smp.cbor_encode_uint(v)
    if isinstance(v, str):
        return smp.cbor_encode_tstr(v)
    if isinstance(v, (list, tuple)):
        if len(v) > 23:
            raise ValueError("list too long")
        return bytes([0x80 | len(v)]) + b"".join(cbor(x) for x in v)
    raise TypeError(f"no CBOR for {type(v).__name__}")


def parse_value(entry, text):
    """Command-line text -> value for the setting described by a list entry
    [key, type, a, b, flags]; ValueError if it does not fit."""
    key, typ, a, b, _ = entry
    if typ == "b":
        t = text.lower()
        if t in ("on", "true", "1", "yes"):
            return True
        if t in ("off", "false", "0", "no"):
            return False
        raise ValueError(f"{key}: expected on or off, got {text!r}")
    if typ == "u":
        n = int(text, 0)
        if not a <= n <= b:
            raise ValueError(f"{key}: {n} is outside {a}..{b}")
        return n
    if typ == "c":
        n = int(text[1:] if text.startswith("#") else text, 16)
        if not 0 <= n <= 0xFFFFFF:
            raise ValueError(f"{key}: {text!r} is not a colour #RRGGBB")
        return n
    if typ == "e":
        if text not in a:
            raise ValueError(f"{key}: {text!r} is not one of {', '.join(a)}")
        return text
    if typ == "l":
        names = [x for x in text.split(",") if x]
        bad = [x for x in names if x not in a]
        if bad:
            raise ValueError(f"{key}: unknown {', '.join(bad)} (known: {', '.join(a)})")
        return names
    raise ValueError(f"{key}: unknown type {typ!r}")


def format_value(entry, v):
    typ = entry[1]
    if typ == "b":
        return "on" if v else "off"
    if typ == "c":
        return f"#{v:06X}"
    if typ == "l":
        return ",".join(v)
    return str(v)


def describe(entry):
    _, typ, a, b, flags = entry
    text = {"b": "on/off", "c": "colour #RRGGBB"}.get(typ)
    if typ == "u":
        text = f"{a}..{b}"
    elif typ == "e":
        text = "one of: " + ", ".join(a)
    elif typ == "l":
        text = "list of: " + ", ".join(a)
    return text + ("  (read-only)" if flags & 1 else "")


def fetch_list(link):
    entries, i = [], 0
    while True:
        r = link.request(READ, CMD_LIST, [("i", i)])
        entries += r["s"]
        if "next" not in r:
            return entries
        i = r["next"]


def fetch_values(link, keys=None):
    """All values, or the named ones. Either way the keyboard may answer in
    pages: a reply with "next" is followed by a request for the rest (an index
    into the settings, or into the key list when keys are given)."""
    values, fields = {}, ([("k", list(keys))] if keys else [("i", 0)])
    while True:
        r = link.request(READ, CMD_GET, fields)
        values.update(r["v"])
        if "next" not in r:
            return values
        fields = [("k", list(keys))] if keys else []
        fields.append(("i", r["next"]))


class SerialLink:
    def __init__(self, port=None):
        import rainy75_dfu
        port = port or rainy75_rgb.find_port()
        if not port:
            raise RuntimeError("no Rainy 75 found on USB (use --port)")
        self.dfu = rainy75_dfu.Dfu(port)

    def request(self, op, cmd, fields):
        r = self.dfu.request(op, GROUP, cmd, [(k, cbor(v)) for k, v in fields])
        if r.get("rc", 0):
            raise DeviceError(r["rc"])
        return r

    def close(self):
        self.dfu.close()


class BleLink:
    def __init__(self, address=None):
        import rainy75_rgb_ble
        self.loop = asyncio.new_event_loop()
        self.kb = rainy75_rgb_ble.Rainy75BLE(address=address)
        self.loop.run_until_complete(self.kb.connect())

    def request(self, op, cmd, fields):
        pairs = [(k, cbor(v)) for k, v in fields]
        try:
            return self.loop.run_until_complete(
                self.kb._request(op, cmd, pairs, group=GROUP))
        except RuntimeError as e:  # "device rc=N" from rainy75_rgb_ble
            rc = str(e).rpartition("=")[2]
            if rc.isdigit():
                raise DeviceError(int(rc)) from None
            raise

    def close(self):
        self.loop.run_until_complete(self.kb.disconnect())
        self.loop.close()


def run(link, a):
    if a.cmd == "info":
        r = link.request(READ, CMD_INFO, [])
        print(f"protocol {r['v']}, {r['n']} settings, change counter {r['rev']}")
        print("effects: " + ", ".join(r["fx"]))
    elif a.cmd == "list":
        for e in fetch_list(link):
            print(f"{e[0]:20s} {describe(e)}")
    elif a.cmd == "get":
        entries = {e[0]: e for e in fetch_list(link)}
        for key, v in fetch_values(link, a.keys).items():
            shown = format_value(entries[key], v) if key in entries else v
            print(f"{key:20s} {shown}")
    elif a.cmd == "set":
        entries = {e[0]: e for e in fetch_list(link)}
        if a.key not in entries:
            raise ValueError(f"unknown setting {a.key!r} (see: rainy75_cfg.py list)")
        v = parse_value(entries[a.key], a.value)
        r = link.request(WRITE, CMD_SET, [("k", a.key), ("v", v)])
        print(f"{a.key:20s} {format_value(entries[a.key], r['v'])}")
    elif a.cmd == "reset":
        link.request(WRITE, CMD_RESET, [("k", a.keys)] if a.keys else [])
        print("reset " + (", ".join(a.keys) if a.keys else "all settings"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--port", help="serial port (default: found by USB name)")
    ap.add_argument("--ble", action="store_true", help="use Bluetooth instead of USB")
    ap.add_argument("--address", help="Bluetooth address (default: by name)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info", help="protocol version, effects, change counter")
    sub.add_parser("list", help="every setting with its type and range")
    g = sub.add_parser("get", help="values (all, or the named keys)")
    g.add_argument("keys", nargs="*")
    s = sub.add_parser("set", help="change one setting")
    s.add_argument("key")
    s.add_argument("value")
    r = sub.add_parser("reset", help="defaults (all, or the named keys)")
    r.add_argument("keys", nargs="*")
    a = ap.parse_args(argv)

    link = BleLink(a.address) if a.ble else SerialLink(a.port)
    try:
        run(link, a)
    except (DeviceError, ValueError) as e:
        sys.exit(str(e))
    finally:
        link.close()


if __name__ == "__main__":
    main()
