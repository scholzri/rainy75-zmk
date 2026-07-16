#!/usr/bin/env python3
"""
Host-side per-key RGB control for the Rainy 75 Pro running the ZMK firmware
with CONFIG_RGB_MGMT=y (mcumgr group 65 over SMP serial).

Zero external dependencies — Python stdlib only (termios: Linux/macOS).
SMP/CBOR framing inherited from restore_original.py (same repo).

Library:
    from rainy75_rgb import Rainy75
    kb = Rainy75()                              # auto-detects the serial port
    kb.set_keys(["F1", "F2", "F3"], (255, 0, 0))
    kb.set_positions({0: (255, 0, 0), 14: (0, 0, 255)})
    kb.fill((0, 32, 64))
    kb.clear()                                  # back to normal effects
    kb.info()                                   # {'n': 83, 'host': True}

CLI:
    python3 rainy75_rgb.py set F1 F2 F3 --color ff0000
    python3 rainy75_rgb.py fill --color 002040
    python3 rainy75_rgb.py clear
    python3 rainy75_rgb.py info
    python3 rainy75_rgb.py demo
"""

import argparse
import base64
import glob
import os
import struct
import sys
import termios
import time

# --------------------------------------------------------------------------
# Key name -> keymap position (ISO DE, 83 keys, row-major; see rainy75.keymap)
# --------------------------------------------------------------------------

_ROWS = [
    # Row 0 (0-14)
    ["ESC", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
     "F11", "F12", "DEL", "HOME"],
    # Row 1 (15-29)
    ["GRAVE", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "MINUS",
     "EQUAL", "BSPC", "END"],
    # Row 2 (30-43)
    ["TAB", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "LBKT",
     "RBKT", "ENTER"],
    # Row 3 (44-58)
    ["CAPS", "A", "S", "D", "F", "G", "H", "J", "K", "L", "SEMI", "QUOTE",
     "HASH", "PGUP", "PGDN"],
    # Row 4 (59-72)
    ["LSHIFT", "LTGT", "Z", "X", "C", "V", "B", "N", "M", "COMMA", "DOT",
     "SLASH", "RSHIFT", "UP"],
    # Row 5 (73-82)
    ["LCTRL", "LGUI", "LALT", "SPACE", "RALT", "FN", "RCTRL", "LEFT",
     "DOWN", "RIGHT"],
]

KEY_TO_POS = {}
for _row in _ROWS:
    for _name in _row:
        KEY_TO_POS[_name] = len(KEY_TO_POS)
assert len(KEY_TO_POS) == 83

_ALIASES = {
    "BACKSPACE": "BSPC", "ENTR": "ENTER", "RETURN": "ENTER",
    "CAPSLOCK": "CAPS", "SHIFT": "LSHIFT", "CTRL": "LCTRL", "ALT": "LALT",
    "WIN": "LGUI", "CMD": "LGUI", "GUI": "LGUI", "ALTGR": "RALT",
    "PAGEUP": "PGUP", "PAGEDOWN": "PGDN", "DELETE": "DEL",
    "`": "GRAVE", "-": "MINUS", "=": "EQUAL", "[": "LBKT", "]": "RBKT",
    ";": "SEMI", "'": "QUOTE", "#": "HASH", "<": "LTGT", ",": "COMMA",
    ".": "DOT", "/": "SLASH",
}

# Named groups for convenience
GROUPS = {
    "FROW": ["ESC"] + [f"F{i}" for i in range(1, 13)] + ["DEL", "HOME"],
    "NUMROW": _ROWS[1],
    "WASD": ["W", "A", "S", "D"],
    "ARROWS": ["UP", "LEFT", "DOWN", "RIGHT"],
    "ALL": list(KEY_TO_POS.keys()),
}


def key_to_position(name):
    """Resolve a key name (or alias, case-insensitive) to keymap position."""
    n = name.upper()
    n = _ALIASES.get(n, n)
    if n in KEY_TO_POS:
        return KEY_TO_POS[n]
    raise KeyError(f"unknown key name: {name!r}")


# --------------------------------------------------------------------------
# Minimal CBOR + SMP serial framing (subset; same wire format as
# restore_original.py / mcumgr)
# --------------------------------------------------------------------------

def _cbor_uint(val):
    if val <= 23:
        return bytes([val])
    if val <= 0xFF:
        return bytes([24, val])
    if val <= 0xFFFF:
        return struct.pack(">BH", 25, val)
    return struct.pack(">BI", 26, val)


def _cbor_bstr(b):
    n = len(b)
    if n <= 23:
        return bytes([0x40 | n]) + b
    if n <= 0xFF:
        return bytes([0x58, n]) + b
    return struct.pack(">BH", 0x59, n) + b


def _cbor_tstr(s):
    b = s.encode()
    n = len(b)
    if n <= 23:
        return bytes([0x60 | n]) + b
    return bytes([0x78, n]) + b


def _cbor_map(pairs):
    out = bytes([0xA0 | len(pairs)])
    for k, v in pairs:
        out += _cbor_tstr(k) + v
    return out


def _cbor_decode(data, offset=0):
    byte = data[offset]
    major, minor = byte >> 5, byte & 0x1F
    offset += 1

    def arg(off, mn):
        if mn <= 23:
            return mn, off
        if mn == 24:
            return data[off], off + 1
        if mn == 25:
            return struct.unpack_from(">H", data, off)[0], off + 2
        if mn == 26:
            return struct.unpack_from(">I", data, off)[0], off + 4
        raise ValueError(f"cbor minor {mn}")

    if major == 0:
        return arg(offset, minor)
    if major == 1:
        v, offset = arg(offset, minor)
        return -1 - v, offset
    if major == 2 or major == 3:
        ln, offset = arg(offset, minor)
        raw = data[offset:offset + ln]
        return (raw.decode() if major == 3 else raw), offset + ln
    if major == 5:
        if minor == 31:  # indefinite
            result = {}
            while offset < len(data) and data[offset] != 0xFF:
                k, offset = _cbor_decode(data, offset)
                v, offset = _cbor_decode(data, offset)
                result[k] = v
            return result, offset + 1
        cnt, offset = arg(offset, minor)
        result = {}
        for _ in range(cnt):
            k, offset = _cbor_decode(data, offset)
            v, offset = _cbor_decode(data, offset)
            result[k] = v
        return result, offset
    if major == 7:  # simple: false/true/null
        if minor == 20:
            return False, offset
        if minor == 21:
            return True, offset
        if minor == 22:
            return None, offset
    raise ValueError(f"cbor major {major}")


def _crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


SMP_OP_READ, SMP_OP_WRITE = 0, 2
RGB_GROUP = 65
CMD_SET, CMD_FILL, CMD_CLEAR, CMD_INFO = 0, 1, 2, 3


class Rainy75:
    """SMP serial client for the rgb_mgmt group."""

    def __init__(self, port=None):
        self.port = port or self._find_port()
        self.seq = 0
        self.fd = None
        self._rxbuf = bytearray()
        self._open()

    @staticmethod
    def _find_port():
        for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*"):
            hits = sorted(glob.glob(pattern))
            if hits:
                return hits[0]
        raise RuntimeError("no serial port found (keyboard plugged in via USB?)")

    def _open(self):
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0                       # raw
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # 8N1
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 10
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    # --- SMP transport ---

    def _request(self, op, cmd, payload_map, timeout=3.0):
        cbor = _cbor_map(payload_map)
        hdr = struct.pack(">BBHHBB", op, 0, len(cbor), RGB_GROUP,
                          self.seq & 0xFF, cmd)
        self.seq += 1
        pkt = hdr + cbor
        framed = struct.pack(">H", len(pkt) + 2) + pkt + struct.pack(">H", _crc16(pkt))
        b64 = base64.b64encode(framed).decode()
        # Serial frames: 0x06 0x09 first, 0x04 0x14 continuation, max 127 B
        maxc = 124
        for i in range(0, len(b64), maxc):
            marker = b"\x06\x09" if i == 0 else b"\x04\x14"
            os.write(self.fd, marker + b64[i:i + maxc].encode() + b"\n")

        # Read response lines until a complete packet decodes
        deadline = time.monotonic() + timeout
        lines = []
        while time.monotonic() < deadline:
            chunk = os.read(self.fd, 512)
            if chunk:
                self._rxbuf += chunk
            while b"\n" in self._rxbuf:
                line, _, rest = bytes(self._rxbuf).partition(b"\n")
                self._rxbuf = bytearray(rest)
                if line[:2] in (b"\x06\x09", b"\x04\x14"):
                    lines.append(line[2:])
                    try:
                        raw = base64.b64decode(b"".join(lines))
                        plen = struct.unpack_from(">H", raw, 0)[0]
                        if len(raw) >= 2 + plen:
                            packet = raw[2:plen]      # strip len prefix + CRC
                            crc = struct.unpack_from(">H", raw, plen)[0]
                            if _crc16(packet) != crc:
                                raise ValueError("CRC mismatch")
                            body, _ = _cbor_decode(packet[8:])
                            rc = body.get("rc", 0)
                            if rc != 0:
                                raise RuntimeError(f"device rc={rc}")
                            return body
                    except (ValueError, struct.error):
                        pass  # incomplete — keep reading
        raise TimeoutError("no SMP response (is CONFIG_RGB_MGMT firmware flashed?)")

    # --- Public API ---

    def set_positions(self, mapping):
        """mapping: {keymap_position: (r, g, b)}. Starts from black when
        entering host mode; subsequent calls update incrementally."""
        quads = bytearray()
        for pos, (r, g, b) in mapping.items():
            if not 0 <= pos <= 82:
                raise ValueError(f"position {pos} out of range")
            quads += bytes([pos, r & 0xFF, g & 0xFF, b & 0xFF])
        # Chunk to stay well inside the SMP netbuf
        for i in range(0, len(quads), 48 * 4):
            self._request(SMP_OP_WRITE, CMD_SET,
                          [("px", _cbor_bstr(bytes(quads[i:i + 48 * 4])))])

    def set_keys(self, names, color):
        """names: iterable of key names or group names; color: (r, g, b)."""
        positions = {}
        for name in names:
            if name.upper() in GROUPS:
                for member in GROUPS[name.upper()]:
                    positions[key_to_position(member)] = color
            else:
                positions[key_to_position(name)] = color
        self.set_positions(positions)

    def fill(self, color):
        r, g, b = color
        self._request(SMP_OP_WRITE, CMD_FILL,
                      [("r", _cbor_uint(r)), ("g", _cbor_uint(g)),
                       ("b", _cbor_uint(b))])

    def clear(self):
        """Exit host mode — keyboard returns to its normal effect."""
        self._request(SMP_OP_WRITE, CMD_CLEAR, [])

    def info(self):
        return self._request(SMP_OP_READ, CMD_INFO, [])

    def pulse(self, color, times=1, steps=12, period=0.9):
        """Pulse the whole board: fade in/out `times`, then back to normal.
        Intended for notification hooks (agent done, awaiting input)."""
        r, g, b = color
        half = period / 2
        for _ in range(times):
            for i in range(1, steps + 1):
                f = i / steps
                self.fill((int(r * f), int(g * f), int(b * f)))
                time.sleep(half / steps)
            for i in range(steps - 1, -1, -1):
                f = i / steps
                self.fill((int(r * f), int(g * f), int(b * f)))
                time.sleep(half / steps)
        self.clear()


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _parse_color(s):
    s = s.lstrip("#")
    if len(s) != 6:
        raise argparse.ArgumentTypeError("color must be RRGGBB hex")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def _demo(kb):
    print("Demo: F-Reihe rot")
    kb.set_keys(["FROW"], (255, 0, 0))
    time.sleep(1.5)
    print("Demo: + WASD grün")
    kb.set_keys(["WASD"], (0, 255, 0))
    time.sleep(1.5)
    print("Demo: + Pfeile blau")
    kb.set_keys(["ARROWS"], (0, 64, 255))
    time.sleep(1.5)
    print("Demo: Lauflicht über die Zahlenreihe")
    for name in GROUPS["NUMROW"]:
        kb.set_keys([name], (255, 128, 0))
        time.sleep(0.08)
    time.sleep(1.0)
    print("Demo: clear — zurück zum normalen Effekt")
    kb.clear()


def main():
    ap = argparse.ArgumentParser(description="Rainy 75 per-key RGB host control")
    ap.add_argument("--port", help="serial port (default: auto-detect)")
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
    sub.add_parser("demo", help="run a short demo sequence")
    sub.add_parser("keys", help="list key names and groups")

    args = ap.parse_args()

    if args.cmd == "keys":
        for i, row in enumerate(_ROWS):
            print(f"Row {i}: {' '.join(row)}")
        print(f"Groups: {', '.join(GROUPS)}")
        return

    kb = Rainy75(port=args.port)
    try:
        if args.cmd == "set":
            kb.set_keys(args.keys, args.color)
        elif args.cmd == "fill":
            kb.fill(args.color)
        elif args.cmd == "pulse":
            kb.pulse(args.color, times=args.times)
        elif args.cmd == "clear":
            kb.clear()
        elif args.cmd == "info":
            print(kb.info())
        elif args.cmd == "demo":
            _demo(kb)
    finally:
        kb.close()


if __name__ == "__main__":
    main()
