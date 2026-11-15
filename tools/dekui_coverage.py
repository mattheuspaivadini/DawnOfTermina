#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Checklist of the DekUI port: which instructions DekUI substitutes in a code.bin and which of them
have an `insn` hook in tools/recomp/hooks.txt.

DekUI swaps instructions for SVCs in Azahar's MemoryReadCode (src/core/arm/dynarmic/arm_dynarmic.cpp)
through a chain of MM3D::<Module>::*Instruction(pc, word) calls. This script builds a probe from that
same chain (with the DekUI headers), runs it over every word of the code.bin and compares the result
with the hooks.txt section of that code.bin. Usage:

    tools/dekui_coverage.py <dekui source dir> <game_data> [--all]

Modules outside the single-screen port (gameplay extras) are listed only with --all."""
import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "recomp"))
from recomp import select_hooks  # noqa: E402

# DekUI modules that are not part of the single-screen interface (gameplay/visual extras).
EXTRAS = {"ActorDrawDistance", "BottleHealing", "CameraLive", "ClassicDoubleTime", "ClassicSaving",
          "DeathPenalties", "Difficulty", "FastTime", "FormCamera", "Music", "OwlIntro", "OwlSword",
          "Prices", "PropDrawDistance", "VegetationDrawDistance", "VisibilityDepth", "SongEffect",
          "TatlReminders", "BomberApproach", "ScreenShrink"}

# Headers that need Azahar/dynarmic and contribute no instruction substitution.
SKIP_HEADERS = ("camera_instruction_probe", "interpolation_performance", "diagnostics.h",
                "effect_capture", "link_pose_capture", "notebook_inspector")


def probe_source(dynarmic: str) -> str:
    includes = [l for l in dynarmic.splitlines()[:120]
                if l.startswith('#include "mm3d/') and not any(s in l for s in SKIP_HEADERS)]
    body = dynarmic[dynarmic.index("MemoryReadCode"):dynarmic.index("MemoryRead8")]
    funcs = list(dict.fromkeys(re.findall(r"MM3D::\w+::\w*Instruction|SongList::NavPC", body)))  # chain order
    checks = ['    if(va==MM3D::SongList::NavPC&&w==MM3D::SongList::NavWord) '
              'std::printf("%06x %08x %08x MM3D::SongList::Nav\\n",va,w,0xef000000u|MM3D::SongList::NavSVC);'
              if f.startswith("SongList") else
              f'    if({f}(va,w)!=w) std::printf("%06x %08x %08x {f}\\n",va,w,{f}(va,w));' for f in funcs]
    return "\n".join(includes + [
        "#include <cstdio>", "#include <cstring>", "#include <vector>",
        "int main(int, char** argv) {",
        "  std::FILE* f = std::fopen(argv[1], \"rb\"); std::vector<unsigned char> b(32 << 20);",
        "  const size_t n = std::fread(b.data(), 1, b.size(), f);",
        "  for (size_t o = 0; o + 4 <= n; o += 4) {",
        "    unsigned va = 0x100000 + unsigned(o), w; std::memcpy(&w, &b[o], 4);",
        *checks, "  }", "}"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dekui", type=Path, help="DekUI source checkout (github.com/tgacquin/mm3d-dekui)")
    ap.add_argument("game_data", type=Path)
    ap.add_argument("--all", action="store_true", help="also list the gameplay extras")
    args = ap.parse_args()
    code = args.game_data / "exefs" / "code.bin"
    dynarmic = (args.dekui / "src/core/arm/dynarmic/arm_dynarmic.cpp").read_text()
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "probe.cpp", Path(tmp) / "probe"
        src.write_text(probe_source(dynarmic))
        subprocess.run(["c++", "-std=c++20", "-O1", "-w", f"-I{args.dekui / 'src'}",
                        f"-I{args.dekui / 'externals'}", str(src), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe), str(code)], check=True, capture_output=True, text=True).stdout

    hooks_txt = (Path(__file__).parent / "recomp" / "hooks.txt").read_text()
    version, _, insns, patches = select_hooks(hooks_txt, hashlib.sha256(code.read_bytes()).hexdigest())
    print(f"code.bin section: {version or 'none'}")
    per_module = {}
    for line in out.splitlines():
        va, word, new, func = line.split()
        module, va = func.split("::")[1], int(va, 16)
        if any(va == v for sites in per_module.values() for v, *_ in sites):
            continue  # an earlier module in DekUI's chain owns this instruction
        per_module.setdefault(module, []).append((va, word, new, func.split("::")[2]))
    done = total = 0
    for module in sorted(per_module):
        if module in EXTRAS and not args.all:
            continue
        sites = per_module[module]
        hit = sum(va in insns or va in patches for va, *_ in sites)
        if module not in EXTRAS:
            done, total = done + hit, total + len(sites)
        print(f"{'ok ' if hit == len(sites) else '   '} {module:24} {hit}/{len(sites)}"
              f"{'  (extra)' if module in EXTRAS else ''}")
        for va, word, new, fn in sites:
            if va in patches and patches[va] != int(new, 16):
                print(f"      WRONG   0x{va:08x} patch {patches[va]:08x}, DekUI writes {new}")
            elif va not in insns and va not in patches:
                kind = "svc" if (int(new, 16) >> 24) & 0xF == 0xF else f"patch 0x{new}"
                print(f"      missing 0x{va:08x} {word} {fn}: {kind}")
    print(f"single-screen coverage: {done}/{total}")


if __name__ == "__main__":
    main()
