#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Extract a *decrypted* 3DS .cia into game_data/: exheader.bin, exefs/ (code.bin
LZSS-decompressed), code.elf (code.bin at its original VAs, for Hopper), romfs/ and the
raw romfs.bin image (served to the game by the fs:USER HLE).
Pure Python, no ctrtool/3dstool needed.

usage: extract_cia.py <game.cia> [out_dir=game_data] --update <update_v1.1.cia> [--restoration <mod>]
                      [--romfs <mod> ...]

DawnOfTermina runs Majora's Mask 3D v1.1 only: the base game .cia and its official v1.1 update
(title 0004000E..., same region) are both required and checked (title IDs, region, version)
before anything is written. The update replaces exheader.bin and code.bin; its RomFS goes to
update_romfs.bin (base romfs.bin kept). The version and code.bin SHA-256 go to
out_dir/manifest.json, written last: no manifest = incomplete extraction.

--restoration applies Project Restoration (https://restoration.zora.re/) last, on top of v1.1: the
release archive (.zip, or .7z with 7z/bsdtar installed) or a folder with code.bps, exheader.bin or
exheader.bps, and optionally romfs/. The code.bps whose source CRC32 matches code.bin is picked
(the release has one per game version; its 3ds/ variant is preferred over citra/).

--romfs adds a RomFS mod (archive or folder; its romfs/ subfolder if it has one) into romfs.bin
(tools/romfs_patch.py), e.g. Project Restoration's HD HUD layout (mm3d_big_screen_layout_*.7z).
Mods are applied in order, after the update and Restoration.
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))  # run with python3 -I, which leaves the script dir out
from bps_patch import BPSError, bps_apply, bps_info  # noqa: E402
from romfs_patch import patch_romfs  # noqa: E402

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


def cia_layout(f):
    """-> (offset of content 0 = the NCCH, title ID, title version, content 0 encrypted) from
    the CIA header and its TMD (the TMD is never encrypted, so this works on any .cia)."""
    f.seek(0)
    hdr_size, _, _, cert_size, tik_size, tmd_size = struct.unpack("<IHHIII", f.read(0x14))
    f.seek(align(hdr_size, 64) + align(cert_size, 64) + align(tik_size, 64))
    tmd = f.read(tmd_size)
    sig = struct.unpack_from(">I", tmd, 0)[0]
    body = {0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}.get(sig)  # RSA-4096/2048, ECDSA
    if body is None:
        raise ValueError(f"not a CIA (unknown TMD signature type {sig:#x})")
    title_id = struct.unpack_from(">Q", tmd, body + 0x4C)[0]
    version = struct.unpack_from(">H", tmd, body + 0x9C)[0]
    content_type = struct.unpack_from(">H", tmd, body + 0xC4 + 64 * 0x24 + 6)[0]
    ncch = align(hdr_size, 64) + align(cert_size, 64) + align(tik_size, 64) + align(tmd_size, 64)
    return ncch, title_id, version, bool(content_type & 1)


def version_name(v: int) -> str:
    # TMD title version: major.minor.micro in 6.6.4 bits; 0 is the launch release, sold as 1.0.
    return "1.0" if v == 0 else f"{v >> 10}.{(v >> 4) & 0x3F}"


# Majora's Mask 3D unique IDs, by region.
MM3D_IDS = {0x00125500: "USA", 0x00125600: "EUR", 0x000D6E00: "JPN"}


def check_update(base_tid: int, update_tid: int):
    """Majora's Mask 3D and an update for it: category 0x0004000E with the base's unique ID
    (which also encodes the region)."""
    if base_tid >> 32 != 0x00040000:
        raise ValueError(f"base title {base_tid:016X} is not a game (category 00040000)")
    if base_tid & 0xFFFFFFFF not in MM3D_IDS:
        raise ValueError(f"base title {base_tid:016X} is not Majora's Mask 3D")
    if update_tid >> 32 != 0x0004000E:
        raise ValueError(f"--update: {update_tid:016X} is not an update title (category 0004000E)")
    if update_tid & 0xFFFFFFFF != base_tid & 0xFFFFFFFF:
        raise ValueError(f"--update: {update_tid:016X} belongs to another game or region "
                         f"(base is {base_tid:016X}, expected update {0x0004000E << 32 | base_tid & 0xFFFFFFFF:016X})")


def extract(cia: Path, out: Path, romfs_image="romfs.bin"):
    """Extract one decrypted CIA into out/. Run again with an update CIA on the same out/
    (romfs_image="update_romfs.bin"): it overwrites exheader.bin and exefs/code.bin (+ code.elf)
    and adds its RomFS files to romfs/."""
    with open(cia, "rb") as f:
        ncch, _, _, encrypted = cia_layout(f)
        f.seek(ncch)
        h = f.read(0x200)
        if encrypted or h[0x100:0x104] != b"NCCH":
            sys.exit(f"{cia.name}: content 0 is CIA-encrypted (title key); decrypt the .cia first")
        if not h[0x18F] & 0x4:
            sys.exit(f"{cia.name}: NCCH is encrypted (NoCrypto flag not set); decrypt the .cia first")

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
            # An update's RomFS is a separate archive (SelfNCCH path 5; MM3D v1.1: only
            # version.ctxb), not a replacement: its files go over romfs/ (for reference only),
            # its image to update_romfs.bin, and romfs.bin stays the base game's.
            n = extract_romfs(f, ncch + romfs_off, out / "romfs")
            print(f"romfs/  {n} files")
            # Raw RomFS image (from the IVFC header): what fs:USER serves to the game's SDK.
            remaining = u32(h, 0x1B4) * MEDIA_UNIT
            f.seek(ncch + romfs_off)
            with open(out / romfs_image, "wb") as o:
                while remaining:
                    chunk = f.read(min(remaining, 1 << 20))
                    if not chunk:
                        break
                    o.write(chunk)
                    remaining -= len(chunk)
            print(f"{romfs_image}  {(out / romfs_image).stat().st_size:#x}")
    return cs


def unpack_mod(src: Path, tmp: Path) -> Path:
    """Folder as is; .zip with zipfile (it drops absolute and ".." member paths); anything else
    (the official release is a .7z) with 7z or bsdtar, which refuse ".." paths too."""
    if src.is_dir():
        return src
    if not src.is_file():
        raise ValueError(f"{src} not found")
    if zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            z.extractall(tmp)
        return tmp
    for cmd in (["7z", "x", "-y", f"-o{tmp}", str(src)], ["bsdtar", "-xf", str(src), "-C", str(tmp)]):
        if shutil.which(cmd[0]):
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            return tmp
    raise ValueError(f"can't unpack {src.name} (install 7z or bsdtar, or extract it "
                     "and pass the folder)")


def find_restoration(root: Path, code: bytes) -> Path:
    """Folder of the code.bps that patches this code.bin (3ds/ before citra/: the citra variant
    keeps the mod in .bss, which our loader does not fill from code.bin)."""
    patches = sorted(root.rglob("code.bps"), key=lambda p: ("citra" in p.parts, str(p)))
    if not patches:
        raise ValueError("--restoration: no code.bps found")
    crc = zlib.crc32(code)
    for p in patches:
        src_size, _, src_crc, _ = bps_info(p.read_bytes())
        if (src_size, src_crc) == (len(code), crc):
            return p.parent
    raise ValueError(f"--restoration: no code.bps matches this code.bin (CRC32 {crc:08X}). "
                     "Project Restoration requires the game updated to v1.1: pass --update <update_v1.1.cia>")


def apply_restoration(mod: Path, out: Path, cs_before):
    """code.bps over the decompressed code.bin, exheader.bps (or the shipped exheader.bin) over
    exheader.bin, romfs/ files over romfs/. -> (new code set, [start, end) of the mod's code)."""
    code_path, exh_path = out / "exefs" / "code.bin", out / "exheader.bin"
    d = find_restoration(mod, code_path.read_bytes())
    code = bps_apply(code_path.read_bytes(), (d / "code.bps").read_bytes())
    if (d / "exheader.bps").is_file():
        exh = bps_apply(exh_path.read_bytes(), (d / "exheader.bps").read_bytes())
    elif (d / "exheader.bin").is_file():
        exh = (d / "exheader.bin").read_bytes()
    else:
        raise ValueError(f"--restoration: {d} has no exheader.bps or exheader.bin")
    cs = parse_codeset(exh)
    expect = (cs["text"]["pages"] + cs["rodata"]["pages"]) * 0x1000 + cs["data"]["size"]
    if len(exh) != exh_path.stat().st_size or len(code) < expect:
        raise ValueError("--restoration: patched exheader.bin and code.bin don't match")
    # The mod's code is appended to .data after the original .bss, which becomes zeros in
    # code.bin; the recompiler treats it as code too. Its start is the first non-zero page past
    # the original .data: the patched layout is v1.0's even from v1.1, so the pre-patch sizes
    # can't give it.
    d_addr, d_size = cs["data"]["addr"], cs["data"]["size"]
    d_off = (cs["text"]["pages"] + cs["rodata"]["pages"]) * 0x1000
    scan = cs_before["data"]["addr"] + cs_before["data"]["size"] - d_addr
    tail = code[d_off + scan : d_off + d_size].lstrip(b"\0") if 0 <= scan < d_size else b""
    if not tail:
        raise ValueError("--restoration: patched .data does not grow past the original .bss")
    start, end = (d_addr + d_size - len(tail)) & ~0xFFF, d_addr + d_size
    exh_path.write_bytes(exh)
    code_path.write_bytes(code)
    write_elf(code, cs, out / "code.elf")
    if (d / "romfs").is_dir():
        apply_romfs_mod(d / "romfs", out)
    print(f"Project Restoration applied from {d}: code.bin {len(code):#x}, mod code {start:#x}-{end:#x}")
    return cs, [start, end]


def apply_romfs_mod(mod: Path, out: Path):
    """Files of a mod's romfs/ folder into romfs.bin (what the game reads) and romfs/ (reference)."""
    n = patch_romfs(out / "romfs.bin", mod)
    for f in mod.rglob("*"):
        if f.is_file():
            dst = out / "romfs" / f.relative_to(mod)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(f, dst)
    print(f"romfs mod {mod}: {n} files added to romfs.bin")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cia", type=Path, help="decrypted base game .cia")
    ap.add_argument("out", type=Path, nargs="?", default=Path("game_data"))
    ap.add_argument("--update", type=Path, required=True, help="decrypted v1.1 update .cia (required)")
    ap.add_argument("--restoration", type=Path,
                    help="Project Restoration release (.zip/.7z) or folder; needs --update v1.1")
    ap.add_argument("--romfs", type=Path, action="append", default=[],
                    help="RomFS mod (.zip/.7z or folder) added into romfs.bin; repeatable")
    args = ap.parse_args()
    try:
        with open(args.cia, "rb") as f:
            _, base_tid, _, _ = cia_layout(f)
        with open(args.update, "rb") as f:  # validated before writing anything
            _, upd_tid, upd_ver, upd_encrypted = cia_layout(f)
        check_update(base_tid, upd_tid)
        if version_name(upd_ver) != "1.1":
            raise ValueError(f"--update: {args.update.name} is version {version_name(upd_ver)}; "
                             "DawnOfTermina needs the v1.1 update")
        if upd_encrypted:
            raise ValueError(f"{args.update.name}: content is CIA-encrypted (title key); "
                             "decrypt the update .cia first")
        if args.restoration:
            tmp = tempfile.TemporaryDirectory(prefix="dot_restoration_")
            mod = unpack_mod(args.restoration, Path(tmp.name))
            if not any(mod.rglob("code.bps")):
                raise ValueError(f"--restoration: no code.bps in {args.restoration}")
        romfs_mods = []
        for src in args.romfs:
            tmp_r = tempfile.TemporaryDirectory(prefix="dot_romfs_")
            d = unpack_mod(src, Path(tmp_r.name))
            d = next((p for p in sorted(d.rglob("romfs")) if p.is_dir()), d)
            if not any(p.is_file() for p in d.rglob("*")):
                raise ValueError(f"--romfs: {src} has no files")
            romfs_mods.append((tmp_r, d))
    except (ValueError, struct.error, subprocess.CalledProcessError, zipfile.BadZipFile) as e:
        sys.exit(f"error: {e}")

    args.out.mkdir(parents=True, exist_ok=True)
    for stale in ("update_romfs.bin", "manifest.json"):  # from an earlier run
        (args.out / stale).unlink(missing_ok=True)
    cs = extract(args.cia, args.out)
    base_sha = hashlib.sha256((args.out / "exefs" / "code.bin").read_bytes()).hexdigest()
    print(f"\napplying update {upd_tid:016X} v{version_name(upd_ver)}")
    cs = extract(args.update, args.out, "update_romfs.bin")
    version = upd_ver
    mod_code = None
    if args.restoration:
        print("\napplying Project Restoration")
        try:
            cs, mod_code = apply_restoration(mod, args.out, cs)
        except (ValueError, BPSError) as e:
            sys.exit(f"error: {e}")
    for _, d in romfs_mods:
        apply_romfs_mod(d, args.out)
    code_sha = hashlib.sha256((args.out / "exefs" / "code.bin").read_bytes()).hexdigest()
    manifest = {
        "title_id": f"{base_tid:016X}",
        "game_version": version_name(version),
        "title_version": version,
        "code_sha256": code_sha,
        "restoration_enabled": bool(args.restoration),
        "romfs_mods": [p.name for p in args.romfs],
    }
    if mod_code:
        manifest["restoration_code"] = mod_code  # read by tools/recomp (extra code range)
    manifest["base_code_sha256"] = base_sha
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"\ntitle: {cs['name']}  version {manifest['game_version']}  code compressed: {cs['compressed']}")
    for s in ("text", "rodata", "data"):
        v = cs[s]
        print(f".{s:<7} VA {v['addr']:#010x}  size {v['size']:#x}  pages {v['pages']:#x}")
    print(f".bss     size {cs['bss_size']:#x}\nstack    size {cs['stack_size']:#x}")
    print(f"entry    {cs['text']['addr']:#010x}\ncode.bin sha256 {code_sha}")


if __name__ == "__main__":
    main()
