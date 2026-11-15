#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Self-check of the BPS patcher on hand-built patches covering the four actions (no game data)."""
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bps_patch import BPSError, _varint, bps_apply, bps_info  # noqa: E402


def varint(n):
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if not n:
            return bytes(out + bytes([x | 0x80]))
        out.append(x)
        n -= 1


def action(cmd, n, extra=b""):
    return varint((n - 1) << 2 | cmd) + extra


def rel(d):  # signed relative offset of SourceCopy/TargetCopy
    return varint(abs(d) << 1 | (d < 0))


def make(src, dst, actions, meta=b""):
    body = b"BPS1" + varint(len(src)) + varint(len(dst)) + varint(len(meta)) + meta + b"".join(actions)
    body += struct.pack("<II", zlib.crc32(src), zlib.crc32(dst))
    return body + struct.pack("<I", zlib.crc32(body))


src = b"Hello, Termina! The moon is falling."
dst = b"Hello, Clock Town! ab" + b"ab" * 6 + b"moon is falling."
patch = make(src, dst, [
    action(0, 7),                              # SourceRead "Hello, "
    action(1, 12, b"Clock Town! "),            # TargetRead
    action(1, 2, b"ab"),
    action(3, 12, rel(19)),                    # TargetCopy, overlapping (RLE of "ab")
    action(2, 16, rel(20)),                    # SourceCopy "moon is falling."
], meta=b"<test/>")
assert bps_apply(src, patch) == dst
assert bps_info(patch) == (len(src), len(dst), zlib.crc32(src), zlib.crc32(dst))
for n in (0, 1, 0x7F, 0x80, 0x407F, 0x4080, 0x204080, 1 << 32):  # beat's varint carries +1 per byte
    assert _varint(varint(n), 0) == (n, len(varint(n))), n
assert varint(0x80) == b"\x00\x80"

for bad_src, bad_patch, why in (
        (src[:-1], patch, "source size"),
        (src.replace(b"H", b"J"), patch, "source CRC"),
        (src, patch[:-5] + bytes([patch[-5] ^ 1]) + patch[-4:], "patch CRC"),
        (src, b"UPS1" + patch[4:], "magic"),
        (src, make(src, dst[:-1] + b"!", [action(0, 7), action(1, len(dst) - 7, dst[7:])]), "target CRC"),
        (src, make(src, dst, [action(2, 4, rel(-1))]), "SourceCopy before 0"),
        (src, make(src, dst, [action(0, 7)]), "short target")):
    try:
        bps_apply(bad_src, bad_patch)
    except BPSError:
        continue
    raise AssertionError(f"accepted a patch with a bad {why}")

print("test_bps: OK")
