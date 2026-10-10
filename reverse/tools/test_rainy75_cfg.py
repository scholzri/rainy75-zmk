"""Unit tests for rainy75_cfg.py (no hardware needed)."""
import argparse
import asyncio
import contextlib
import io
import unittest
from unittest import mock

import rainy75_cfg as c

EFFECTS = ["solid", "rainbow", "plasma"]
ENTRIES = {
    "rgb.on": ["rgb.on", "b", None, None, 0],
    "rgb.val": ["rgb.val", "u", 16, 255, 0],
    "rgb.effect": ["rgb.effect", "e", EFFECTS, None, 0],
    "ind.caps_color": ["ind.caps_color", "c", None, None, 0],
    "rgb.cycle": ["rgb.cycle", "l", EFFECTS, None, 0],
    "kb.os_keys": ["kb.os_keys", "u", 0, 83, 1],
}


class Cbor(unittest.TestCase):
    def test_scalars(self):
        self.assertEqual(c.cbor(True), b"\xf5")
        self.assertEqual(c.cbor(False), b"\xf4")
        self.assertEqual(c.cbor(5), b"\x05")
        self.assertEqual(c.cbor(300), b"\x19\x01\x2c")
        self.assertEqual(c.cbor("ab"), b"\x62ab")

    def test_list(self):
        self.assertEqual(c.cbor(["a", "b"]), b"\x82\x61a\x61b")
        self.assertEqual(c.cbor([]), b"\x80")


class ParseValue(unittest.TestCase):
    def test_bool(self):
        self.assertIs(c.parse_value(ENTRIES["rgb.on"], "on"), True)
        self.assertIs(c.parse_value(ENTRIES["rgb.on"], "0"), False)
        with self.assertRaises(ValueError):
            c.parse_value(ENTRIES["rgb.on"], "maybe")

    def test_uint_range(self):
        self.assertEqual(c.parse_value(ENTRIES["rgb.val"], "120"), 120)
        with self.assertRaises(ValueError):
            c.parse_value(ENTRIES["rgb.val"], "300")

    def test_uint_not_a_number_names_key(self):
        with self.assertRaises(ValueError) as cm:
            c.parse_value(ENTRIES["rgb.val"], "abc")
        self.assertIn("rgb.val", str(cm.exception))
        self.assertIn("not a number", str(cm.exception))

    def test_enum(self):
        self.assertEqual(c.parse_value(ENTRIES["rgb.effect"], "plasma"), "plasma")
        with self.assertRaises(ValueError):
            c.parse_value(ENTRIES["rgb.effect"], "fire")

    def test_colour(self):
        self.assertEqual(c.parse_value(ENTRIES["ind.caps_color"], "#FF8000"), 0xFF8000)
        self.assertEqual(c.parse_value(ENTRIES["ind.caps_color"], "0x0000ff"), 0xFF)
        with self.assertRaises(ValueError):
            c.parse_value(ENTRIES["ind.caps_color"], "#1000000")

    def test_list(self):
        self.assertEqual(c.parse_value(ENTRIES["rgb.cycle"], "plasma,solid"),
                         ["plasma", "solid"])
        self.assertEqual(c.parse_value(ENTRIES["rgb.cycle"], ""), [])
        with self.assertRaises(ValueError):
            c.parse_value(ENTRIES["rgb.cycle"], "solid,fire")


class Format(unittest.TestCase):
    def test_values(self):
        self.assertEqual(c.format_value(ENTRIES["rgb.on"], True), "on")
        self.assertEqual(c.format_value(ENTRIES["ind.caps_color"], 0xFF8000), "#FF8000")
        self.assertEqual(c.format_value(ENTRIES["rgb.cycle"], ["a", "b"]), "a,b")
        self.assertEqual(c.format_value(ENTRIES["rgb.val"], 120), "120")

    def test_describe(self):
        self.assertEqual(c.describe(ENTRIES["rgb.val"]), "16..255")
        self.assertEqual(c.describe(ENTRIES["rgb.on"]), "on/off")
        self.assertIn("plasma", c.describe(ENTRIES["rgb.effect"]))
        self.assertTrue(c.describe(ENTRIES["kb.os_keys"]).endswith("(read-only)"))

    def test_describe_unknown_type_does_not_raise(self):
        text = c.describe(["x.new", "x", None, None, 0])
        self.assertIsInstance(text, str)
        self.assertIn("unknown", text)
        self.assertTrue(c.describe(["x.new", "x", None, None, 1]).endswith("(read-only)"))


class FakeLink:
    """Replies by (op, cmd, i); records every request."""

    def __init__(self, replies):
        self.replies = replies
        self.sent = []

    def request(self, op, cmd, fields):
        self.sent.append((op, cmd, fields))
        key = (op, cmd, dict(fields).get("i", 0))
        return self.replies[key]


class Paging(unittest.TestCase):
    def test_list_follows_next(self):
        link = FakeLink({
            (c.READ, c.CMD_LIST, 0): {"rc": 0, "s": [ENTRIES["rgb.on"]], "next": 1},
            (c.READ, c.CMD_LIST, 1): {"rc": 0, "s": [ENTRIES["rgb.val"]]},
        })
        self.assertEqual([e[0] for e in c.fetch_list(link)], ["rgb.on", "rgb.val"])

    def test_get_all_follows_next(self):
        link = FakeLink({
            (c.READ, c.CMD_GET, 0): {"rc": 0, "v": {"rgb.on": True}, "next": 1},
            (c.READ, c.CMD_GET, 1): {"rc": 0, "v": {"rgb.val": 120}},
        })
        self.assertEqual(c.fetch_values(link), {"rgb.on": True, "rgb.val": 120})

    def test_get_by_key_single_request(self):
        link = FakeLink({(c.READ, c.CMD_GET, 0): {"rc": 0, "v": {"rgb.val": 120}}})
        self.assertEqual(c.fetch_values(link, ["rgb.val"]), {"rgb.val": 120})
        self.assertEqual(link.sent, [(c.READ, c.CMD_GET, [("k", ["rgb.val"])])])

    def test_get_by_key_follows_next(self):
        link = FakeLink({
            (c.READ, c.CMD_GET, 0): {"rc": 0, "v": {"rgb.on": True}, "next": 1},
            (c.READ, c.CMD_GET, 1): {"rc": 0, "v": {"rgb.val": 120}},
        })
        self.assertEqual(c.fetch_values(link, ["rgb.on", "rgb.val"]),
                         {"rgb.on": True, "rgb.val": 120})
        self.assertEqual(link.sent, [
            (c.READ, c.CMD_GET, [("k", ["rgb.on", "rgb.val"])]),
            (c.READ, c.CMD_GET, [("k", ["rgb.on", "rgb.val"]), ("i", 1)]),
        ])


class Run(unittest.TestCase):
    def test_set_sends_typed_value(self):
        link = FakeLink({
            (c.READ, c.CMD_LIST, 0): {"rc": 0, "s": list(ENTRIES.values())},
            (c.WRITE, c.CMD_SET, 0): {"rc": 0, "v": "plasma"},
        })
        c.run(link, argparse.Namespace(cmd="set", key="rgb.effect", value="plasma"))
        self.assertEqual(link.sent[-1], (c.WRITE, c.CMD_SET,
                                         [("k", "rgb.effect"), ("v", "plasma")]))

    def test_set_unknown_key_refused_locally(self):
        link = FakeLink({(c.READ, c.CMD_LIST, 0): {"rc": 0, "s": list(ENTRIES.values())}})
        with self.assertRaises(ValueError):
            c.run(link, argparse.Namespace(cmd="set", key="rgb.nope", value="1"))

    def test_reset_all_sends_empty_request(self):
        link = FakeLink({(c.WRITE, c.CMD_RESET, 0): {"rc": 0}})
        c.run(link, argparse.Namespace(cmd="reset", keys=[]))
        self.assertEqual(link.sent, [(c.WRITE, c.CMD_RESET, [])])

    def test_device_error_names_rc(self):
        self.assertIn("unknown setting", str(c.DeviceError(5)))
        self.assertIn("read-only", str(c.DeviceError(11)))

    def test_device_error_enotsup_says_old_firmware(self):
        self.assertIn("not supported", str(c.DeviceError(8)))
        self.assertIn("update", str(c.DeviceError(8)))

    def test_list_shows_unknown_type_and_keeps_others(self):
        link = FakeLink({(c.READ, c.CMD_LIST, 0): {
            "rc": 0, "s": [ENTRIES["rgb.on"], ["x.new", "x", None, None, 0],
                           ENTRIES["rgb.val"]]}})
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            c.run(link, argparse.Namespace(cmd="list"))
        lines = out.getvalue().splitlines()
        self.assertEqual(len(lines), 3)
        self.assertIn("unknown", lines[1])
        self.assertIn("16..255", lines[2])


class FakeBleakError(Exception):
    """Stands in for bleak.exc.BleakError (bleak need not be installed)."""


class FakeKb:
    def __init__(self, exc=None):
        self.exc = exc

    async def _request(self, op, cmd, pairs, group):
        raise self.exc

    async def disconnect(self):
        raise self.exc


class BleErrors(unittest.TestCase):
    def link(self, exc):
        link = object.__new__(c.BleLink)
        link.loop = asyncio.new_event_loop()
        link._bleak_error = FakeBleakError
        link.kb = FakeKb(exc)
        self.addCleanup(link.loop.close)
        return link

    def test_bleak_error_becomes_one_line_runtime_error(self):
        with self.assertRaises(RuntimeError) as cm:
            self.link(FakeBleakError("Not connected")).request(c.READ, c.CMD_INFO, [])
        self.assertEqual(str(cm.exception), "Not connected")
        with self.assertRaises(RuntimeError) as cm:
            self.link(FakeBleakError()).request(c.READ, c.CMD_INFO, [])
        self.assertEqual(str(cm.exception), "Bluetooth error")

    def test_bleak_error_ending_in_equals_digit_is_not_a_device_rc(self):
        with self.assertRaises(RuntimeError) as cm:
            self.link(FakeBleakError("handle=5")).request(c.READ, c.CMD_INFO, [])
        self.assertNotIsInstance(cm.exception, c.DeviceError)

    def test_device_rc_still_a_device_error(self):
        with self.assertRaises(c.DeviceError) as cm:
            self.link(RuntimeError("device rc=8")).request(c.READ, c.CMD_INFO, [])
        self.assertEqual(cm.exception.rc, 8)

    def test_empty_timeout_gets_a_message(self):
        with self.assertRaises(TimeoutError) as cm:
            self.link(asyncio.TimeoutError()).request(c.READ, c.CMD_INFO, [])
        self.assertIn("no response", str(cm.exception))

    def test_close_converts_and_still_closes_the_loop(self):
        link = self.link(FakeBleakError("gone"))
        with self.assertRaises(RuntimeError):
            link.close()
        self.assertTrue(link.loop.is_closed())


class OddReply(unittest.TestCase):
    def test_wrong_shape_is_one_line(self):
        class Link(FakeLink):
            def close(self):
                pass
        with mock.patch.object(c, "SerialLink", lambda port: Link({
                (c.READ, c.CMD_LIST, 0): {"rc": 0, "x": 1}})):
            with self.assertRaises(SystemExit) as cm:
                c.main(["get"])
        self.assertIn("unexpected reply", str(cm.exception.code))


if __name__ == "__main__":
    unittest.main()
