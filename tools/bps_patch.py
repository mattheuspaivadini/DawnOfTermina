#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""BPS (beat) patch applier, pure Python. Checks the patch, source and target CRC32s and sizes.

usage: bps_patch.py <source> <patch.bps> <target>
"""
import struct
import sys
import zlib


class BPSError(ValueError):
    pass


def _varint(p: bytes, i: int):
    data, shift = 0, 1
    while True:
        if i >= len(p):
            raise BPSError("truncated patch")
        x = p[i]
        i += 1
        data += (x & 0x7F) * shift
        if x & 0x80:
            return data, i
        shift <<= 7
        data += shift


def bps_info(patch: bytes):
    """-> (source size, target size, source CRC32, target CRC32), after checking the patch CRC."""
    if len(patch) < 4 + 3 + 12 or patch[:4] != b"BPS1":
        raise BPSError("not a BPS patch")
    src_crc, dst_crc, patch_crc = struct.unpack_from("<III", patch, len(patch) - 12)
    if zlib.crc32(patch[:-4]) != patch_crc:
        raise BPSError("patch CRC32 mismatch (corrupt .bps)")
    src_size, i = _varint(patch, 4)
    dst_size, i = _varint(patch, i)
    return src_size, dst_size, src_crc, dst_crc


def bps_apply(source: bytes, patch: bytes) -> bytes:
    src_size, dst_size, src_crc, dst_crc = bps_info(patch)
    if len(source) != src_size or zlib.crc32(source) != src_crc:
        raise BPSError(f"source does not match the patch (expected {src_size:#x} bytes, CRC32 {src_crc:08X}; "
                       f"got {len(source):#x} bytes, CRC32 {zlib.crc32(source):08X})")
    _, i = _varint(patch, 4)
    _, i = _varint(patch, i)
    meta, i = _varint(patch, i)
    i += meta
    end = len(patch) - 12
    out = bytearray(dst_size)
    o = src_rel = dst_rel = 0
    while i < end:
        data, i = _varint(patch, i)
        cmd, n = data & 3, (data >> 2) + 1
        if o + n > dst_size:
            raise BPSError("action writes past the target size")
        if cmd == 0:  # SourceRead: same offset in the source
            if o + n > len(source):
                raise BPSError("SourceRead past the source")
            out[o:o + n] = source[o:o + n]
        elif cmd == 1:  # TargetRead: literal bytes from the patch
            if i + n > end:
                raise BPSError("TargetRead past the patch")
            out[o:o + n] = patch[i:i + n]
            i += n
        else:
            d, i = _varint(patch, i)
            d = -(d >> 1) if d & 1 else d >> 1
            if cmd == 2:  # SourceCopy
                src_rel += d
                if src_rel < 0 or src_rel + n > len(source):
                    raise BPSError("SourceCopy outside the source")
                out[o:o + n] = source[src_rel:src_rel + n]
                src_rel += n
            else:  # TargetCopy: may overlap the bytes being written (RLE), so byte by byte
                dst_rel += d
                if dst_rel < 0 or dst_rel >= o:
                    raise BPSError("TargetCopy outside the written target")
                for k in range(n):
                    out[o + k] = out[dst_rel + k]
                dst_rel += n
        o += n
    if o != dst_size:
        raise BPSError(f"patch produced {o:#x} bytes, expected {dst_size:#x}")
    if zlib.crc32(out) != dst_crc:
        raise BPSError("target CRC32 mismatch")
    return bytes(out)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    src, patch, dst = sys.argv[1:]
    try:
        out = bps_apply(open(src, "rb").read(), open(patch, "rb").read())
    except BPSError as e:
        sys.exit(f"error: {e}")
    open(dst, "wb").write(out)


if __name__ == "__main__":
    main()
