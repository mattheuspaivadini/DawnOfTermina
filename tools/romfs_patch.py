#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Add or replace files inside a raw RomFS image (IVFC, as game_data/romfs.bin).

The game parses IVFC level 3 itself (fs serves the image from the level 3 header on), so a RomFS
mod (e.g. Project Restoration's HD HUD layout) has to live inside the image. The SDK reads the four
tables contiguously right after the level 3 header, using only their sizes (measured: it ignores
the offsets), so rebuilt tables cannot be appended: the image is rewritten as header + tables +
the old data region unchanged (file offsets are relative to the data start, so old files keep
theirs) + the new files. Hash levels are not updated (not exposed to the game).

usage: romfs_patch.py <romfs.bin> <mod romfs/ folder>
"""
import struct
import sys
from pathlib import Path

NONE = 0xFFFFFFFF


def _align(x, a):
    return (x + a - 1) & ~(a - 1)


def level3_offset(f) -> int:
    f.seek(0)
    ivfc = f.read(0x60)
    if ivfc[:4] != b"IVFC":
        raise ValueError("RomFS: missing IVFC magic (encrypted?)")
    master, log2 = struct.unpack_from("<I", ivfc, 0x8)[0], struct.unpack_from("<I", ivfc, 0x4C)[0]
    return _align(0x60 + master, 1 << log2)


def _hash(parent: int, name: str) -> int:
    h = parent ^ 123456789
    raw = name.encode("utf-16-le")
    for unit in struct.unpack(f"<{len(raw) // 2}H", raw):  # over UTF-16 code units
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF ^ unit
    return h


def _buckets(n: int) -> int:  # same sizing rule as the official tools (3dbrew / makerom)
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


class Dir:
    def __init__(self, name):
        self.name, self.dirs, self.files = name, {}, {}  # files: name -> (offset, size)


def _read_tree(f, l3):
    f.seek(l3)
    hdr = struct.unpack("<10I", f.read(0x28))
    f.seek(l3 + hdr[3])
    dirs = f.read(hdr[4])
    f.seek(l3 + hdr[7])
    files = f.read(hdr[8])

    def name(b, o, n):
        s = b[o : o + n].decode("utf-16-le")
        if "/" in s or s in (".", ".."):
            raise ValueError(f"RomFS: bad name {s!r}")
        return s

    def walk(doff, d):
        foff = struct.unpack_from("<I", dirs, doff + 0xC)[0]
        while foff != NONE:
            _, sib, off, size, _, nlen = struct.unpack_from("<IIQQII", files, foff)
            d.files[name(files, foff + 0x20, nlen)] = (off, size)
            foff = sib
        child = struct.unpack_from("<I", dirs, doff + 0x8)[0]
        while child != NONE:
            sib, nlen = struct.unpack_from("<I", dirs, child + 4)[0], struct.unpack_from("<I", dirs, child + 0x14)[0]
            sub = d.dirs[name(dirs, child + 0x18, nlen)] = Dir(None)
            sub.name = name(dirs, child + 0x18, nlen)
            walk(child, sub)
            child = sib

    root = Dir("")
    walk(0, root)
    return hdr, root


def _tables(root):
    """-> (dir hash, dir meta, file hash, file meta) bytes for the tree."""
    order = []  # breadth-first, root first, as the official tools lay them out

    def collect(d, parent):
        order.append((d, parent))

    collect(root, None)
    i = 0
    while i < len(order):
        d = order[i][0]
        for sub in sorted(d.dirs.values(), key=lambda s: s.name):
            collect(sub, d)
        i += 1
    doff, off = {}, 0
    for d, _ in order:
        doff[id(d)] = off
        off += 0x18 + _align(len(d.name.encode("utf-16-le")), 4)
    files, foff, off = [], {}, 0
    for d, _ in order:
        for n in sorted(d.files):
            files.append((d, n))
            foff[(id(d), n)] = off
            off += 0x20 + _align(len(n.encode("utf-16-le")), 4)

    dn, fn = _buckets(len(order)), _buckets(len(files))
    dhash, fhash = [NONE] * dn, [NONE] * fn
    dmeta, fmeta = bytearray(), bytearray()
    dnext, fnext = {}, {}
    for d, parent in order:
        p = doff[id(parent)] if parent else 0
        b = _hash(p, d.name) % dn
        dnext[id(d)], dhash[b] = dhash[b], doff[id(d)]
    for d, n in files:
        b = _hash(doff[id(d)], n) % fn
        fnext[(id(d), n)], fhash[b] = fhash[b], foff[(id(d), n)]
    for d, parent in order:
        subs = sorted(d.dirs.values(), key=lambda s: s.name)
        names = sorted(d.files)
        sib = NONE
        if parent:
            ps = sorted(parent.dirs.values(), key=lambda s: s.name)
            k = ps.index(d)
            sib = doff[id(ps[k + 1])] if k + 1 < len(ps) else NONE
        nm = d.name.encode("utf-16-le")
        dmeta += struct.pack("<6I", doff[id(parent)] if parent else 0, sib,
                             doff[id(subs[0])] if subs else NONE,
                             foff[(id(d), names[0])] if names else NONE, dnext[id(d)], len(nm))
        dmeta += nm.ljust(_align(len(nm), 4), b"\0")
    for d, n in files:
        names = sorted(d.files)
        k = names.index(n)
        off, size = d.files[n]
        nm = n.encode("utf-16-le")
        fmeta += struct.pack("<IIQQII", doff[id(d)], foff[(id(d), names[k + 1])] if k + 1 < len(names) else NONE,
                             off, size, fnext[(id(d), n)], len(nm))
        fmeta += nm.ljust(_align(len(nm), 4), b"\0")
    pack = lambda t: struct.pack(f"<{len(t)}I", *t)
    return pack(dhash), bytes(dmeta), pack(fhash), bytes(fmeta)


def patch_romfs(image: Path, mod: Path) -> int:
    """Puts every file under `mod` (a romfs/ folder) into `image`. -> number of files."""
    news = sorted(p for p in mod.rglob("*") if p.is_file())
    if not news:
        return 0
    tmp = image.with_name(image.name + ".tmp")
    with open(image, "rb") as f, open(tmp, "wb") as o:
        l3 = level3_offset(f)
        hdr, root = _read_tree(f, l3)
        f.seek(0, 2)
        old_data, old_end = l3 + hdr[9], f.tell()
        end = old_end - old_data  # new files go after everything, relative to the data start
        blobs = []
        for p in news:
            d = root
            for part in p.relative_to(mod).parts[:-1]:
                d = d.dirs.setdefault(part, Dir(part))
            end = _align(end, 16)
            blob = p.read_bytes()
            d.files[p.name] = (end, len(blob))
            blobs.append((end, blob))
            end += len(blob)
        tables = _tables(root)
        header, at = [0x28], 0x28
        for t in tables:
            header += [at, len(t)]
            at += len(t)
        header.append(_align(at, 16))
        f.seek(0)
        o.write(f.read(l3))
        o.write(struct.pack("<10I", *header) + b"".join(tables))
        o.write(bytes(header[9] - at))
        f.seek(old_data)
        while chunk := f.read(1 << 24):
            o.write(chunk)
        base = l3 + header[9]
        for off, blob in blobs:
            o.seek(base + off)
            o.write(blob)
    tmp.replace(image)
    return len(news)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    print(f"{patch_romfs(Path(sys.argv[1]), Path(sys.argv[2]))} files patched into {sys.argv[1]}")
