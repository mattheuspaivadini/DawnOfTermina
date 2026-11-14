#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Extract a *decrypted* 3DS .cia into game_data/: exheader.bin, exefs/ (code.bin
LZSS-decompressed), code.elf (code.bin at its original VAs, for Hopper) and romfs/.
Pure Python, no ctrtool/3dstool needed.

usage: extract_cia.py <game.cia> [out_dir=game_data]
"""
import struct
import sys
from pathlib import Path

MEDIA_UNIT = 0x200


def align(x, a):
    return (x + a - 1) & ~(a - 1)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def lzss_decompress(comp: bytes) -> bytes:
    """3DS backward LZSS ("BLZ"), as used by ExeFS .code. Port of ctrtool's lzss_decompress."""
    top_bottom = u32(comp, len(comp) - 8)
    out_size = len(comp) + u32(comp, len(comp) - 4)
    out = bytearray(out_size)
    out[: len(comp)] = comp
    index = len(comp) - (top_bottom >> 24)
    stop = len(comp) - (top_bottom & 0xFFFFFF)
    pos = out_size
    while index > stop:
        index -= 1
        control = comp[index]
        for _ in range(8):
            if index <= stop or index <= 0 or pos <= 0:
                break
            if control & 0x80:
                index -= 2
                seg = comp[index] | (comp[index + 1] << 8)
                size = ((seg >> 12) & 0xF) + 3
                disp = (seg & 0xFFF) + 2
                if pos < size:
                    raise ValueError("corrupt LZSS stream")
                for _ in range(size):
                    if pos + disp >= out_size:
                        raise ValueError("corrupt LZSS stream")
                    b = out[pos + disp]
                    pos -= 1
                    out[pos] = b
            else:
                pos -= 1
                index -= 1
                out[pos] = comp[index]
            control = (control << 1) & 0xFF
    return bytes(out)


def parse_codeset(exh: bytes):
    """ExHeader SCI CodeSetInfo -> dict of segments (addr, pages, size)."""
    seg = lambda o: dict(addr=u32(exh, o), pages=u32(exh, o + 4), size=u32(exh, o + 8))
    return dict(
        name=exh[0:8].rstrip(b"\0").decode("ascii", "replace"),
        compressed=bool(exh[0xD] & 1),
        text=seg(0x10),
        stack_size=u32(exh, 0x1C),
        rodata=seg(0x20),
        data=seg(0x30),
        bss_size=u32(exh, 0x3C),
    )


def write_elf(code: bytes, cs, path: Path):
    """Wrap the flat code.bin into an ELF32 ARM image with .text/.rodata/.data/.bss at their
    original VAs, so Hopper (which rejects headerless binaries) maps it 1:1."""
    t, r, d = cs["text"], cs["rodata"], cs["data"]
    base = 0x1000  # file offset of code.bin byte 0
    ro_off = t["pages"] * 0x1000
    data_off = ro_off + r["pages"] * 0x1000
    bss_addr = d["addr"] + d["size"]
    shstr = b"\0.text\0.rodata\0.data\0.bss\0.shstrtab\0"
    shstr_off = base + len(code)
    sh_off = align(shstr_off + len(shstr), 4)
    phdrs = [  # (type, offset, vaddr, filesz, memsz, flags)
        (1, base, t["addr"], t["size"], t["size"], 5),
        (1, base + ro_off, r["addr"], r["size"], r["size"], 4),
        (1, base + data_off, d["addr"], d["size"], d["size"] + cs["bss_size"], 6),
    ]
    shdrs = [  # (name, type, flags, addr, offset, size)
        (0, 0, 0, 0, 0, 0),
        (1, 1, 0x6, t["addr"], base, t["size"]),
        (7, 1, 0x2, r["addr"], base + ro_off, r["size"]),
        (15, 1, 0x3, d["addr"], base + data_off, d["size"]),
        (21, 8, 0x3, bss_addr, base + data_off + d["size"], cs["bss_size"]),
        (26, 3, 0, 0, shstr_off, len(shstr)),
    ]
    ehdr = struct.pack(
        "<4sBBBB8sHHIIIIIHHHHHH", b"\x7fELF", 1, 1, 1, 0, b"", 2, 40, 1, t["addr"],
        52, sh_off, 0x05000000, 52, 32, len(phdrs), 40, len(shdrs), len(shdrs) - 1,
    )
    img = bytearray(ehdr)
    for p in phdrs:
        img += struct.pack("<IIIIIIII", p[0], p[1], p[2], p[2], p[3], p[4], p[5], 0x1000)
    img += bytes(base - len(img)) + code + shstr
    img += bytes(sh_off - len(img))
    for s in shdrs:
        img += struct.pack("<IIIIIIIIII", *s, 0, 0, 4 if s[1] else 0, 0)
    path.write_bytes(img)


def utf16_name(b, o, n):
    return b[o : o + n].decode("utf-16-le")


def extract_romfs(f, base, out: Path) -> int:
    f.seek(base)
    ivfc = f.read(0x60)
    if ivfc[:4] != b"IVFC":
        raise ValueError("RomFS: missing IVFC magic (encrypted?)")
    master_hash_size = u32(ivfc, 0x8)
    lvl3_block = 1 << u32(ivfc, 0x4C)
    lvl3 = base + align(0x60 + master_hash_size, lvl3_block)
    f.seek(lvl3)
    hdr = f.read(0x28)
    dir_meta_off, dir_meta_size = u32(hdr, 0x0C), u32(hdr, 0x10)
    file_meta_off, file_meta_size = u32(hdr, 0x1C), u32(hdr, 0x20)
    data_off = lvl3 + u32(hdr, 0x24)
    f.seek(lvl3 + dir_meta_off)
    dirs = f.read(dir_meta_size)
    f.seek(lvl3 + file_meta_off)
    files = f.read(file_meta_size)
    NONE = 0xFFFFFFFF
    count = 0

    def walk_dir(doff, path: Path):
        nonlocal count
        path.mkdir(parents=True, exist_ok=True)
        foff = u32(dirs, doff + 0xC)
        while foff != NONE:
            name = utf16_name(files, foff + 0x20, u32(files, foff + 0x1C))
            assert "/" not in name and name not in ("", ".", ".."), name  # untrusted image
            off, size = u64(files, foff + 0x8), u64(files, foff + 0x10)
            f.seek(data_off + off)
            with open(path / name, "wb") as o:
                remaining = size
                while remaining:
                    chunk = f.read(min(remaining, 1 << 20))
                    o.write(chunk)
                    remaining -= len(chunk)
            count += 1
            foff = u32(files, foff + 0x4)
        child = u32(dirs, doff + 0x8)
        while child != NONE:
            name = utf16_name(dirs, child + 0x18, u32(dirs, child + 0x14))
            assert "/" not in name and name not in ("", ".", ".."), name
            walk_dir(child, path / name)
            child = u32(dirs, child + 0x4)

    walk_dir(0, out)
    return count


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cia = Path(sys.argv[1])
    out = Path(sys.argv[2] if len(sys.argv) > 2 else "game_data")
    out.mkdir(parents=True, exist_ok=True)
    with open(cia, "rb") as f:
        hdr_size, _, _, cert_size, tik_size, tmd_size = struct.unpack("<IHHIII", f.read(0x14))
        ncch = align(hdr_size, 64) + align(cert_size, 64) + align(tik_size, 64) + align(tmd_size, 64)
        f.seek(ncch)
        h = f.read(0x200)
        if h[0x100:0x104] != b"NCCH":
            sys.exit("content 0 is not a plain NCCH: the .cia is CIA-encrypted; decrypt it first")
        if not h[0x18F] & 0x4:
            sys.exit("NCCH is encrypted (NoCrypto flag not set); decrypt the .cia first")

        exh_size = u32(h, 0x180)
        f.seek(ncch + 0x200)
        exh = f.read(exh_size + 0x400)  # ExHeader + AccessDesc
        (out / "exheader.bin").write_bytes(exh)
        cs = parse_codeset(exh)

        exefs_off = ncch + u32(h, 0x1A0) * MEDIA_UNIT
        f.seek(exefs_off)
        eh = f.read(0x200)
        (out / "exefs").mkdir(exist_ok=True)
        for i in range(10):
            name = eh[i * 16 : i * 16 + 8].rstrip(b"\0").decode("ascii")
            if not name:
                continue
            off, size = u32(eh, i * 16 + 8), u32(eh, i * 16 + 12)
            f.seek(exefs_off + 0x200 + off)
            blob = f.read(size)
            if name == ".code":
                if cs["compressed"]:
                    blob = lzss_decompress(blob)
                expect = (cs["text"]["pages"] + cs["rodata"]["pages"]) * 0x1000 + cs["data"]["size"]
                assert len(blob) >= expect, f"code.bin {len(blob):#x} < expected {expect:#x}"
                name = "code"
                write_elf(blob, cs, out / "code.elf")
                print("code.elf (for Hopper)")
            (out / "exefs" / f"{name}.bin").write_bytes(blob)
            print(f"exefs/{name}.bin  {len(blob):#x}")

        romfs_off = u32(h, 0x1B0) * MEDIA_UNIT
        if romfs_off:
            n = extract_romfs(f, ncch + romfs_off, out / "romfs")
            print(f"romfs/  {n} files")

    print(f"\ntitle: {cs['name']}  code compressed: {cs['compressed']}")
    for s in ("text", "rodata", "data"):
        v = cs[s]
        print(f".{s:<7} VA {v['addr']:#010x}  size {v['size']:#x}  pages {v['pages']:#x}")
    print(f".bss     size {cs['bss_size']:#x}\nstack    size {cs['stack_size']:#x}")
    print(f"entry    {cs['text']['addr']:#010x}")


if __name__ == "__main__":
    main()
