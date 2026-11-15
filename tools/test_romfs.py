#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Self-check of romfs_patch.py: a synthetic image gets new and replaced files, which are then
found the way the SDK does (hash buckets from the root) and extracted back. With game_data/
present, the hash function is also checked against the real romfs.bin (read only)."""
import struct
import sys
import tempfile
from pathlib import Path

root = Path(__file__).parent
sys.path.insert(0, str(root))
from extract_cia import extract_romfs  # noqa: E402
from romfs_patch import Dir, _hash, _tables, level3_offset, patch_romfs  # noqa: E402


def lookup(image: Path, path: str) -> bytes:
    """File contents by path, through the directory and file hash tables."""
    with open(image, "rb") as f:
        l3 = level3_offset(f)
        f.seek(l3)
        h = struct.unpack("<10I", f.read(0x28))
        f.seek(l3)
        blob = f.read()
    dh, dm, fh, fm = (blob[h[1 + 2 * k] : h[1 + 2 * k] + h[2 + 2 * k]] for k in range(4))

    def find(table, meta, parent, name, name_at, next_at):
        b = struct.unpack_from("<I", table, 4 * (_hash(parent, name) % (len(table) // 4)))[0]
        while b != 0xFFFFFFFF:
            n = struct.unpack_from("<I", meta, b + name_at - 4)[0]
            if struct.unpack_from("<I", meta, b)[0] == parent and \
                    meta[b + name_at : b + name_at + n].decode("utf-16-le") == name:
                return b
            b = struct.unpack_from("<I", meta, b + next_at)[0]
        raise KeyError(path)

    *dirs, name = path.split("/")
    d = 0
    for part in dirs:
        d = find(dh, dm, d, part, 0x18, 0x10)
    e = find(fh, fm, d, name, 0x20, 0x18)
    off, size = struct.unpack_from("<QQ", fm, e + 8)
    return blob[h[9] + off : h[9] + off + size]


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    # Empty IVFC image: level 3 at 0x1000 (no master hash, 4 KiB blocks).
    ivfc = bytearray(0x60)
    ivfc[:4] = b"IVFC"
    struct.pack_into("<I", ivfc, 0x4C, 12)
    tables = _tables(Dir(""))
    hdr, body, at = [0x28], b"", 0x28
    for t in tables:
        hdr += [at, len(t)]
        body += t
        at += len(t)
    hdr.append(at)
    image = tmp / "romfs.bin"
    image.write_bytes(bytes(ivfc).ljust(0x1000, b"\0") + struct.pack("<10I", *hdr) + body + b"old data")

    mod = tmp / "mod"
    (mod / "layout" / "sub").mkdir(parents=True)
    files = {"layout/Joker.Main.New.gar": b"new" * 100, "layout/sub/x.bin": b"\1\2\3", "top.txt": b"top"}
    for p, data in files.items():
        (mod / p).write_bytes(data)
    assert patch_romfs(image, mod) == 3
    (mod / "top.txt").write_bytes(b"replaced")  # second patch: replace one file, keep the others
    files["top.txt"] = b"replaced"
    assert patch_romfs(image, mod) == 3
    for p, data in files.items():
        assert lookup(image, p) == data, p
    with open(image, "rb") as f:  # the SDK reads the tables back to back after the header
        f.seek(level3_offset(f))
        h = struct.unpack("<10I", f.read(0x28))
    assert h[1] == 0x28 and all(h[3 + 2 * k] == h[1 + 2 * k] + h[2 + 2 * k] for k in range(3)), h
    out = tmp / "out"
    with open(image, "rb") as f:
        assert extract_romfs(f, 0, out) == 3
    for p, data in files.items():
        assert (out / p).read_bytes() == data, p

real = root.parent / "game_data" / "romfs.bin"
if real.is_file():  # the official hash: a file of the base game found through its own tables
    assert lookup(real, "layout/Joker.mfpj")[:4], "real romfs.bin lookup"

print("test_romfs: OK")
