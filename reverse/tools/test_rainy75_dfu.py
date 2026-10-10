"""Unit tests for rainy75_dfu.py (no hardware needed)."""
import hashlib
import struct
import unittest
from unittest import mock

import rainy75_dfu as dfu


class ImageHash(unittest.TestCase):
    def test_header_and_body_without_tlvs(self):
        hdr = bytearray(32)
        struct.pack_into("<I", hdr, 0, 0x96f3b83d)   # IMAGE_MAGIC
        struct.pack_into("<H", hdr, 8, 32)           # header size
        struct.pack_into("<I", hdr, 12, 5)           # image size
        data = bytes(hdr) + b"BODY!" + b"TLV-AREA"
        self.assertEqual(dfu.image_hash(data),
                         hashlib.sha256(bytes(hdr) + b"BODY!").digest())


class FakeClient:
    """Serves queued raw SMP replies; records nothing else."""

    def __init__(self, replies, seq=7):
        self.seq, self.fd, self._rxbuf = seq, -1, bytearray()
        self.replies = list(replies)

    def _read_response(self, timeout_s=5.0):
        if not self.replies:
            raise TimeoutError("no SMP response received")
        return self.replies.pop(0)


def _reply(seq, cbor=b"\xa1\x62rc\x00"):
    return struct.pack(">BBHHBB", 1, 0, len(cbor), 67, seq, 0) + cbor


class SeqMatch(unittest.TestCase):
    def request(self, replies):
        d = dfu.Dfu.__new__(dfu.Dfu)
        d.client = FakeClient(replies)
        with mock.patch.object(dfu.smp.os, "write"), \
                mock.patch.object(dfu.smp.termios, "tcflush"):
            return d.request(0, 67, 0, [], retries=0)

    def test_stale_reply_is_skipped(self):
        stale = _reply(3, b"\xa1\x61x\x01")
        self.assertEqual(self.request([stale, _reply(7)]), {"rc": 0})

    def test_only_stale_reply_times_out(self):
        with self.assertRaises(TimeoutError):
            self.request([_reply(3)])


if __name__ == "__main__":
    unittest.main()
