#!/usr/bin/env python3
"""
Write firmware to Wobkey Rainy 75 Pro via mcumgr SMP serial (flash_mgmt).

Stages firmware to a safe flash area, verifies it, then issues a commit
command that triggers the RAM trampoline to erase+copy+reset.

Uses a custom mcumgr group (ID 64) with raw flash erase/write/read/commit.
Requires firmware built with CONFIG_FLASH_MGMT=y.

Zero external dependencies — uses only Python stdlib.

Usage:
    python3 restore_original.py firmware.bin                  # Write firmware
    python3 restore_original.py firmware.bin --no-verify      # Skip read-back verify
    python3 restore_original.py firmware.bin --port /dev/ttyACM1
    python3 restore_original.py --read 0x0 256                # Read flash region
    python3 restore_original.py --info firmware.bin           # Show firmware info

Requirements:
    Linux with CDC ACM support (/dev/ttyACM0)
    Firmware with CONFIG_FLASH_MGMT=y running on keyboard
"""

import sys
import struct
import os
import argparse
import time
import base64
import termios

# --------------------------------------------------------------------------
# Minimal CBOR encoder/decoder (RFC 8949 subset: uint, bstr, tstr, map)
# --------------------------------------------------------------------------

def cbor_encode_uint(val):
    """Encode CBOR unsigned integer (major type 0)."""
    if val <= 23:
        return bytes([val])
    elif val <= 0xFF:
        return bytes([24, val])
    elif val <= 0xFFFF:
        return struct.pack(">BH", 25, val)
    else:
        return struct.pack(">BI", 26, val)


def cbor_encode_nint(val):
    """Encode CBOR negative integer (major type 1). val must be < 0."""
    n = -1 - val
    if n <= 23:
        return bytes([0x20 | n])
    elif n <= 0xFF:
        return bytes([0x38, n])
    elif n <= 0xFFFF:
        return struct.pack(">BH", 0x39, n)
    else:
        return struct.pack(">Bi", 0x3A, n)


def cbor_encode_int(val):
    """Encode CBOR integer (signed)."""
    if val >= 0:
        return cbor_encode_uint(val)
    return cbor_encode_nint(val)


def cbor_encode_bstr(b):
    """Encode CBOR byte string (major type 2)."""
    n = len(b)
    if n <= 23:
        return bytes([0x40 | n]) + b
    elif n <= 0xFF:
        return bytes([0x58, n]) + b
    elif n <= 0xFFFF:
        return struct.pack(">BH", 0x59, n) + b
    else:
        return struct.pack(">BI", 0x5A, n) + b


def cbor_encode_tstr(s):
    """Encode CBOR text string (major type 3)."""
    b = s.encode('utf-8')
    n = len(b)
    if n <= 23:
        return bytes([0x60 | n]) + b
    elif n <= 0xFF:
        return bytes([0x78, n]) + b
    else:
        return struct.pack(">BH", 0x79, n) + b


def cbor_encode_map(pairs):
    """Encode CBOR map from list of (key_string, encoded_value_bytes) tuples."""
    n = len(pairs)
    if n <= 23:
        result = bytes([0xA0 | n])
    else:
        result = bytes([0xB8, n])
    for key, val in pairs:
        result += cbor_encode_tstr(key) + val
    return result


def _cbor_decode_arg(data, offset, minor):
    """Decode CBOR argument value from minor field."""
    if minor <= 23:
        return minor, offset
    elif minor == 24:
        return data[offset], offset + 1
    elif minor == 25:
        return struct.unpack_from(">H", data, offset)[0], offset + 2
    elif minor == 26:
        return struct.unpack_from(">I", data, offset)[0], offset + 4
    elif minor == 27:
        return struct.unpack_from(">Q", data, offset)[0], offset + 8
    raise ValueError(f"unsupported CBOR minor value {minor}")


def cbor_decode(data, offset=0):
    """Decode one CBOR item. Returns (value, new_offset)."""
    byte = data[offset]
    major = byte >> 5
    minor = byte & 0x1F
    offset += 1

    if major == 0:  # unsigned int
        return _cbor_decode_arg(data, offset, minor)
    elif major == 1:  # negative int
        val, offset = _cbor_decode_arg(data, offset, minor)
        return -1 - val, offset
    elif major == 2:  # byte string
        length, offset = _cbor_decode_arg(data, offset, minor)
        return data[offset:offset + length], offset + length
    elif major == 3:  # text string
        length, offset = _cbor_decode_arg(data, offset, minor)
        return data[offset:offset + length].decode('utf-8'), offset + length
    elif major == 5:  # map
        if minor == 31:
            # Indefinite-length map — read until break (0xFF)
            result = {}
            while offset < len(data) and data[offset] != 0xFF:
                key, offset = cbor_decode(data, offset)
                val, offset = cbor_decode(data, offset)
                result[key] = val
            if offset < len(data):
                offset += 1  # skip break byte
            return result, offset
        count, offset = _cbor_decode_arg(data, offset, minor)
        result = {}
        for _ in range(count):
            key, offset = cbor_decode(data, offset)
            val, offset = cbor_decode(data, offset)
            result[key] = val
        return result, offset
    else:
        raise ValueError(f"unsupported CBOR major type {major}")


# --------------------------------------------------------------------------
# CRC16-ITU-T (polynomial 0x1021, init 0x0000) — used by SMP serial framing
# --------------------------------------------------------------------------

def crc16_itu_t(data, init=0x0000):
    crc = init
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF
    return crc


# --------------------------------------------------------------------------
# SMP (Simple Management Protocol) serial transport
# --------------------------------------------------------------------------

SMP_HDR_SIZE = 8
SMP_FRAME_FIRST = b'\x06\x09'
SMP_FRAME_CONT = b'\x04\x14'
SMP_FRAME_MAX = 127  # max bytes per serial frame (including header)

# SMP operations
SMP_OP_READ_REQ = 0
SMP_OP_READ_RSP = 1
SMP_OP_WRITE_REQ = 2
SMP_OP_WRITE_RSP = 3

# Group IDs
SMP_GROUP_OS = 0
SMP_GROUP_IMG = 1
SMP_GROUP_FLASH = 64  # MGMT_GROUP_ID_PERUSER

# OS group commands
OS_MGMT_ID_RESET = 5

# Flash mgmt commands
FLASH_MGMT_ID_ERASE = 0
FLASH_MGMT_ID_WRITE = 1
FLASH_MGMT_ID_READ = 2
FLASH_MGMT_ID_COMMIT = 3


def smp_build_header(op, flags, cbor_len, group, seq, cmd_id):
    """Build 8-byte SMP header (big-endian)."""
    return struct.pack(">BBHHBB", op, flags, cbor_len, group, seq, cmd_id)


def smp_serial_encode(raw_packet):
    """Encode raw SMP packet into serial frames (base64 + CRC16)."""
    # Format: 2-byte length (packet + CRC) | packet | 2-byte CRC (over packet only)
    crc = crc16_itu_t(raw_packet)
    pkt_len = len(raw_packet) + 2  # length includes CRC
    full = struct.pack(">H", pkt_len) + raw_packet + struct.pack(">H", crc)

    # Base64 encode
    encoded = base64.b64encode(full).decode('ascii')

    # Split into frames (max payload per frame after header = SMP_FRAME_MAX - 2 - 1)
    max_b64_per_frame = SMP_FRAME_MAX - 3  # 2 header + 1 newline
    frames = []
    pos = 0
    while pos < len(encoded):
        chunk = encoded[pos:pos + max_b64_per_frame]
        if pos == 0:
            frame = SMP_FRAME_FIRST + chunk.encode('ascii') + b'\n'
        else:
            frame = SMP_FRAME_CONT + chunk.encode('ascii') + b'\n'
        frames.append(frame)
        pos += max_b64_per_frame

    return frames


def smp_serial_decode(data):
    """Decode base64+CRC framed SMP response. Returns raw SMP packet."""
    # Strip frame headers and concatenate base64 payload
    b64_parts = []
    for line in data:
        if line.startswith(SMP_FRAME_FIRST):
            b64_parts.append(line[2:].rstrip(b'\n\r'))
        elif line.startswith(SMP_FRAME_CONT):
            b64_parts.append(line[2:].rstrip(b'\n\r'))

    b64_data = b''.join(b64_parts)
    decoded = base64.b64decode(b64_data)

    # Format: 2-byte length (packet + CRC) | packet | 2-byte CRC (over packet only)
    if len(decoded) < 4:
        raise ValueError("SMP response too short")
    pkt_len = struct.unpack_from(">H", decoded, 0)[0]  # includes CRC
    packet = decoded[2:2 + pkt_len - 2]  # strip length prefix and CRC
    crc_recv = struct.unpack_from(">H", decoded, 2 + pkt_len - 2)[0]
    crc_calc = crc16_itu_t(packet)
    if crc_recv != crc_calc:
        raise ValueError(f"SMP CRC mismatch: recv=0x{crc_recv:04X} calc=0x{crc_calc:04X}")
    return packet


# --------------------------------------------------------------------------
# SMP Serial Client
# --------------------------------------------------------------------------

class SMPClient:
    def __init__(self, port='/dev/ttyACM0'):
        self.port = port
        self.seq = 0
        self.fd = None
        self._rxbuf = bytearray()

    def open(self):
        """Open serial port for SMP communication."""
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY)
        # Configure raw mode
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0   # iflag: no special processing
        attrs[1] = 0   # oflag: no special processing
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag: 8N1
        attrs[3] = 0   # lflag: raw mode
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 10  # 1 second timeout (0.1s units)
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self._rxbuf.clear()

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def _read_line(self, timeout_s=5.0):
        """Read one line (ending with \\n) from serial, preserving leftover."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            # Check buffer for a complete line
            if b'\n' in self._rxbuf:
                idx = self._rxbuf.index(b'\n')
                line = bytes(self._rxbuf[:idx + 1])
                del self._rxbuf[:idx + 1]
                return line
            # Read more data
            try:
                chunk = os.read(self.fd, 1024)
            except OSError:
                chunk = b''
            if chunk:
                self._rxbuf.extend(chunk)
            else:
                time.sleep(0.01)
        return None

    def _read_response(self, timeout_s=5.0):
        """Read a complete SMP response (one or more serial frames)."""
        frames = []
        deadline = time.monotonic() + timeout_s

        while time.monotonic() < deadline:
            line = self._read_line(timeout_s=max(0.1, deadline - time.monotonic()))
            if line is None:
                break

            if line[:2] == SMP_FRAME_FIRST:
                frames = [line]
            elif line[:2] == SMP_FRAME_CONT and frames:
                frames.append(line)
            else:
                continue  # skip log lines

            # Try to decode accumulated frames
            try:
                b64_parts = []
                for f in frames:
                    b64_parts.append(f[2:].rstrip(b'\n\r'))
                b64_data = b''.join(b64_parts)
                decoded = base64.b64decode(b64_data)
                if len(decoded) < 4:
                    continue
                pkt_len = struct.unpack_from(">H", decoded, 0)[0]
                # pkt_len includes packet + CRC; total needed = 2 + pkt_len
                if len(decoded) >= 2 + pkt_len:
                    return smp_serial_decode(frames)
            except Exception:
                pass  # need more frames

        if frames:
            try:
                return smp_serial_decode(frames)
            except Exception as e:
                raise TimeoutError(f"incomplete SMP response ({len(frames)} frames): {e}")
        raise TimeoutError("no SMP response received")

    def send(self, op, group, cmd_id, cbor_payload=None, retries=2, timeout_s=10.0):
        """Send SMP command, receive and decode response. Returns CBOR dict."""
        if cbor_payload is None:
            cbor_payload = cbor_encode_map([])  # empty map

        header = smp_build_header(op, 0, len(cbor_payload), group, self.seq, cmd_id)
        self.seq = (self.seq + 1) & 0xFF

        raw_packet = header + cbor_payload
        frames = smp_serial_encode(raw_packet)

        for attempt in range(1 + retries):
            # Flush pending rx data (log output)
            self._rxbuf.clear()
            termios.tcflush(self.fd, termios.TCIFLUSH)

            for frame in frames:
                os.write(self.fd, frame)

            try:
                resp_raw = self._read_response(timeout_s=timeout_s)
                break
            except TimeoutError:
                if attempt < retries:
                    time.sleep(0.1)
                    continue
                raise

        # Parse SMP header
        if len(resp_raw) < SMP_HDR_SIZE:
            raise ValueError(f"SMP response too short: {len(resp_raw)} bytes")
        resp_op, resp_flags, resp_len, resp_group, resp_seq, resp_id = \
            struct.unpack_from(">BBHHBB", resp_raw, 0)

        # Decode CBOR payload
        cbor_data = resp_raw[SMP_HDR_SIZE:SMP_HDR_SIZE + resp_len]
        if cbor_data:
            result, _ = cbor_decode(cbor_data)
        else:
            result = {}
        return result

    def flash_erase(self, off, length):
        """Erase flash region. Returns rc.

        Firmware erases sector-by-sector with k_msleep(10) between each.
        For a full 1016KB erase (254 sectors): ~18s. Timeout set accordingly.
        """
        timeout = max(30.0, (length // SECTOR_SIZE) * 0.15)
        payload = cbor_encode_map([
            ("off", cbor_encode_uint(off)),
            ("len", cbor_encode_uint(length)),
        ])
        resp = self.send(SMP_OP_WRITE_REQ, SMP_GROUP_FLASH, FLASH_MGMT_ID_ERASE,
                         payload, timeout_s=timeout)
        return resp.get("rc", -1)

    def flash_write(self, off, data):
        """Write data to flash. Returns rc."""
        payload = cbor_encode_map([
            ("off", cbor_encode_uint(off)),
            ("data", cbor_encode_bstr(data)),
        ])
        resp = self.send(SMP_OP_WRITE_REQ, SMP_GROUP_FLASH, FLASH_MGMT_ID_WRITE, payload)
        return resp.get("rc", -1)

    def flash_read(self, off, length):
        """Read from flash. Returns (rc, data_bytes)."""
        payload = cbor_encode_map([
            ("off", cbor_encode_uint(off)),
            ("len", cbor_encode_uint(length)),
        ])
        resp = self.send(SMP_OP_READ_REQ, SMP_GROUP_FLASH, FLASH_MGMT_ID_READ, payload)
        return resp.get("rc", -1), resp.get("data", b'')

    def flash_commit(self, stg_off, fw_len):
        """Commit staged firmware: erase+copy+reset from RAM trampoline.

        Firmware sends response immediately, then after 500ms the RAM
        trampoline runs and the device resets. Returns rc from the response.
        """
        payload = cbor_encode_map([
            ("stg", cbor_encode_uint(stg_off)),
            ("len", cbor_encode_uint(fw_len)),
        ])
        resp = self.send(SMP_OP_WRITE_REQ, SMP_GROUP_FLASH, FLASH_MGMT_ID_COMMIT,
                         payload, timeout_s=5.0)
        return resp.get("rc", -1)

    def os_reset(self):
        """Send OS reset command (standard mcumgr reset)."""
        payload = cbor_encode_map([])
        frames = smp_serial_encode(
            smp_build_header(SMP_OP_WRITE_REQ, 0, len(payload),
                             SMP_GROUP_OS, self.seq, OS_MGMT_ID_RESET)
            + payload
        )
        self.seq = (self.seq + 1) & 0xFF
        for frame in frames:
            os.write(self.fd, frame)
        # Don't wait for response — device reboots immediately


# --------------------------------------------------------------------------
# Firmware validation
# --------------------------------------------------------------------------

TLNK_MAGIC = b'\x4b\x4e\x4c\x54'
PROTECTED_START = 0xFE000
SECTOR_SIZE = 4096
WRITE_CHUNK = 256
# Minimum staging offset: must be above the running app's XIP region.
# Slot1 starts at 0x80000, which is guaranteed outside the active image.
# Staging below the running XIP code (0x10000-0x5C000+) would crash the
# firmware when flash_erase destroys code the CPU fetches via XIP.
MIN_STAGING_OFFSET = 0x80000


def validate_firmware(data, filename):
    """Validate and display firmware info. Returns True if OK."""
    print(f"Firmware: {filename}")
    print(f"  Size: {len(data)} bytes (0x{len(data):X})")

    if len(data) < 0x24:
        print("  ERROR: File too small")
        return False

    if len(data) >= PROTECTED_START:
        print(f"  ERROR: File too large (>= 0x{PROTECTED_START:X})")
        return False

    has_tlnk = data[0x20:0x24] == TLNK_MAGIC
    print(f"  TLNK magic at 0x20: {'OK' if has_tlnk else 'MISSING'}")

    if has_tlnk:
        ota_size = struct.unpack_from('<I', data, 0x18)[0]
        fw_ver = struct.unpack_from('<H', data, 0x02)[0]
        print(f"  OTA size: {ota_size} bytes")
        print(f"  FW version: 0x{fw_ver:04X}")

    boot_word = struct.unpack_from('<I', data, 0)[0]
    print(f"  Boot vector: 0x{boot_word:08X}")

    return True


# --------------------------------------------------------------------------
# Restore workflow
# --------------------------------------------------------------------------

def restore_firmware(client, fw_data, verify=True):
    """Stage firmware, verify, then commit (erase+copy+reset).

    The B91 runs code from XIP flash, so we cannot erase the app image
    area while the firmware is running. Instead:
      1. Calculate staging offset (must be >= erase_end so commit's
         erase phase doesn't destroy staged data)
      2. Stage the firmware at the calculated offset
      3. Verify the staged data
      4. Commit: firmware's RAM trampoline erases 0x0, copies staging, resets
    """
    fw_size = len(fw_data)
    erase_end = ((fw_size + SECTOR_SIZE - 1) // SECTOR_SIZE) * SECTOR_SIZE

    # Staging offset must be:
    #   1. >= erase_end (commit erases 0x0..erase_end, must not destroy staged data)
    #   2. >= MIN_STAGING_OFFSET (above the running app's XIP code region)
    stg_offset = max(erase_end, MIN_STAGING_OFFSET)
    stg_erase_size = ((fw_size + SECTOR_SIZE - 1) // SECTOR_SIZE) * SECTOR_SIZE

    # Check staging area fits before protected region
    if stg_offset + fw_size > PROTECTED_START:
        print(f"ERROR: Firmware too large for staging area "
              f"(0x{stg_offset:X} + 0x{fw_size:X} > 0x{PROTECTED_START:X})")
        return False

    print(f"\n  Firmware size: {fw_size} bytes (0x{fw_size:X})")
    print(f"  Erase end:    0x{erase_end:X}")
    print(f"  Staging at:   0x{stg_offset:X}")

    # Phase 1: Erase staging area
    stg_sectors = stg_erase_size // SECTOR_SIZE
    print(f"\nErasing staging area 0x{stg_offset:X} - "
          f"0x{stg_offset + stg_erase_size:X} ({stg_erase_size // 1024}KB)...")
    rc = client.flash_erase(stg_offset, stg_erase_size)
    if rc != 0:
        print(f"  ERASE FAILED: rc={rc}")
        return False
    print(f"  Staging area erased ({stg_sectors} sectors)")

    # Phase 2: Write firmware to staging area
    total_chunks = (fw_size + WRITE_CHUNK - 1) // WRITE_CHUNK
    print(f"Writing {fw_size} bytes to staging area ({total_chunks} chunks)...")
    start_time = time.time()
    for i in range(total_chunks):
        off = i * WRITE_CHUNK
        chunk = fw_data[off:off + WRITE_CHUNK]
        rc = client.flash_write(stg_offset + off, chunk)
        if rc != 0:
            print(f"\n  WRITE FAILED at staging 0x{stg_offset + off:X}: rc={rc}")
            return False
        progress = (i + 1) * 100 // total_chunks
        elapsed = time.time() - start_time
        rate = (off + len(chunk)) / elapsed if elapsed > 0 else 0
        print(f"\r  [{progress:3d}%] Chunk {i + 1}/{total_chunks} "
              f"({elapsed:.1f}s, {rate / 1024:.1f} KB/s)", end='', flush=True)
    elapsed = time.time() - start_time
    print(f"\n  Write complete ({elapsed:.1f}s)")

    # Phase 3: Verify staging area
    if verify:
        print(f"Verifying staged firmware...")
        start_time = time.time()
        for i in range(total_chunks):
            off = i * WRITE_CHUNK
            expected = fw_data[off:off + WRITE_CHUNK]
            rc, actual = client.flash_read(stg_offset + off, len(expected))
            if rc != 0:
                print(f"\n  READ FAILED at staging 0x{stg_offset + off:X}: rc={rc}")
                return False
            if actual != expected:
                for j in range(len(expected)):
                    if j < len(actual) and actual[j] != expected[j]:
                        print(f"\n  VERIFY MISMATCH at staging 0x{stg_offset + off + j:X}: "
                              f"expected 0x{expected[j]:02X}, got 0x{actual[j]:02X}")
                        return False
                print(f"\n  VERIFY MISMATCH at staging 0x{stg_offset + off:X}: "
                      f"length mismatch (expected {len(expected)}, got {len(actual)})")
                return False
            progress = (i + 1) * 100 // total_chunks
            print(f"\r  [{progress:3d}%] Verified chunk {i + 1}/{total_chunks}", end='', flush=True)
        elapsed = time.time() - start_time
        print(f"\n  Verify OK ({elapsed:.1f}s)")
    else:
        print("Skipping verification (--no-verify)")

    # Phase 4: Commit — RAM trampoline erases + copies + resets
    print(f"\nCommitting: erase 0x0..0x{erase_end:X}, "
          f"copy 0x{stg_offset:X} -> 0x0, reset...")
    rc = client.flash_commit(stg_offset, fw_size)
    if rc != 0:
        print(f"  COMMIT FAILED: rc={rc}")
        return False
    print("  Commit accepted. Device will reset in ~500ms.")

    return True


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description='Write firmware via mcumgr SMP serial (flash_mgmt group 64)')
    parser.add_argument('firmware', nargs='?', help='Firmware .bin file to write')
    parser.add_argument('--port', default='/dev/ttyACM0',
                        help='Serial port (default: /dev/ttyACM0)')
    parser.add_argument('--no-verify', action='store_true',
                        help='Skip read-back verification')
    parser.add_argument('--info', action='store_true',
                        help='Show firmware info only')
    parser.add_argument('--read', nargs=2, metavar=('OFFSET', 'LENGTH'),
                        help='Read flash region (hex offset, decimal length)')
    parser.add_argument('-y', '--yes', action='store_true',
                        help='Skip confirmation prompt')
    args = parser.parse_args()

    # Read-only flash read mode
    if args.read:
        off = int(args.read[0], 0)
        length = int(args.read[1], 0)
        client = SMPClient(args.port)
        client.open()
        try:
            rc, data = client.flash_read(off, min(length, WRITE_CHUNK))
            if rc != 0:
                print(f"Read failed: rc={rc}")
                sys.exit(1)
            print(f"Flash @ 0x{off:06X} ({len(data)} bytes):")
            # Hex dump
            for i in range(0, len(data), 16):
                hex_part = ' '.join(f'{b:02x}' for b in data[i:i + 16])
                ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[i:i + 16])
                print(f"  {off + i:06X}: {hex_part:<48s}  {ascii_part}")
        finally:
            client.close()
        return

    if not args.firmware:
        parser.print_help()
        return

    # Load firmware
    if not os.path.isfile(args.firmware):
        print(f"ERROR: File not found: {args.firmware}")
        sys.exit(1)

    with open(args.firmware, 'rb') as f:
        fw_data = f.read()

    if not validate_firmware(fw_data, args.firmware):
        sys.exit(1)

    if args.info:
        return

    # Confirmation
    if not args.yes:
        print("\n" + "=" * 60)
        print("WARNING: This will erase current firmware and write the")
        print("provided image directly to flash. The keyboard will reboot.")
        print("")
        print("If interrupted, the keyboard may not boot and will require")
        print("the SWS hardware debugger (Burning EVK) to recover.")
        print("=" * 60)
        confirm = input("\nType 'YES' to proceed: ")
        if confirm != 'YES':
            print("Aborted.")
            return

    # Connect and restore
    client = SMPClient(args.port)
    print(f"\nOpening {args.port}...")
    client.open()

    try:
        # Quick connectivity test: read first 4 bytes of flash
        print("Testing connection...")
        rc, data = client.flash_read(0, 4)
        if rc != 0:
            print(f"ERROR: Flash read test failed (rc={rc})")
            print("Is the keyboard running ZMK with CONFIG_FLASH_MGMT=y?")
            sys.exit(1)
        print(f"  Connected. Flash[0:4] = {data.hex()}")

        success = restore_firmware(client, fw_data, verify=not args.no_verify)

        if success:
            print("\nWaiting for device to reset...")
            time.sleep(2)
            print("Flash complete. Device is resetting.")
        else:
            print("\nFlash FAILED. Staging area may contain partial data.")
            print("The running firmware is NOT affected — safe to retry.")
            sys.exit(1)
    finally:
        client.close()


if __name__ == '__main__':
    main()
