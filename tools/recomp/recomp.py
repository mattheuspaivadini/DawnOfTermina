#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""ARM11 (ARMv6K: ARM + Thumb-1 + VFPv2) -> C++ static recompiler for code.bin.

usage: recomp.py <game_data> <out_dir> [--chunks N] [--extra entries.txt]

Writes <out_dir>/recomp_funcs.h, recomp_table.cpp, recomp_NN.cpp (exactly N chunk files,
some possibly empty, so the build can list them up front) and stats.txt.
The output is derived from the game binary: it lives in the build tree, never in git.
"""
import argparse
import collections
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import arm  # noqa: E402
import thumb  # noqa: E402
from common import AL, COND, h  # noqa: E402

MOV_LR_PC = 0xE1A0E00F


class Image:
    def __init__(self, game_data: Path):
        exh = (game_data / "exheader.bin").read_bytes()
        self.code = (game_data / "exefs" / "code.bin").read_bytes()
        seg = lambda o: struct.unpack_from("<III", exh, o)  # addr, pages, size
        self.text, self.rodata, self.data = seg(0x10), seg(0x20), seg(0x30)
        # Code ranges: .text, plus the code a mod appended to .data (manifest.json, Project Restoration).
        self.code_ranges = [(self.text[0], self.text[2])]
        manifest = game_data / "manifest.json"
        if manifest.exists() and (r := json.loads(manifest.read_text()).get("restoration_code")):
            self.code_ranges.append((r[0], r[1] - r[0]))
        self.segs = []  # (va, size, file offset)
        off = 0
        for va, pages, size in (self.text, self.rodata, self.data):
            self.segs.append((va, size, off))
            off += pages * 0x1000

    def _off(self, a, n):
        for va, size, off in self.segs:
            if va <= a and a + n <= va + size:
                return off + (a - va)
        return None

    def in_text(self, a, n=4):
        return self.region(a, n) is not None

    def region(self, a, n=4):
        """Index of the code range holding [a, a + n), or None."""
        return next((i for i, (va, size) in enumerate(self.code_ranges) if va <= a and a + n <= va + size), None)

    def read32(self, a):
        return struct.unpack_from("<I", self.code, self._off(a, 4))[0]

    def read16(self, a):
        o = self._off(a, 2)
        return struct.unpack_from("<H", self.code, o)[0] if o is not None else 0

    def words(self, seg_index):
        va, size, off = self.segs[seg_index]
        for i in range(0, size - 3, 4):
            yield va + i, struct.unpack_from("<I", self.code, off + i)[0]


class Recompiler:
    def __init__(self, image: Image):
        self.image = image
        self.cache = {}
        self.entries = set()  # keys: address | 1 for Thumb
        self.seen = set()
        self.branch_targets = set()  # keys reached by direct B: possible tail-called functions
        self.adr_pointers = []  # ADR results, vetted after the walk like data candidates
        self.insn_hooks = {}  # instruction VA -> recomp::hooks function run in its place

    def decode(self, addr, is_thumb):
        key = (addr, is_thumb)
        insn = self.cache.get(key)
        if insn is None:
            if is_thumb:
                insn = thumb.lift(self.image, addr, self.image.read16(addr))
            else:
                insn = arm.lift(self.image, addr, self.image.read32(addr))
            self.cache[key] = insn
        return insn

    def plausible(self, key):
        addr, t = key & ~1, bool(key & 1)
        if not self.image.in_text(addr, 4) or (not t and addr & 3):
            return False
        return self.decode(addr, t).kind != "trap"

    def after_noreturn_bl(self, a, t):
        """In a mod's code range, a pointer from data to the instruction after an unconditional
        ARM `bl` is a function start: the walk assumed the call returns, but the HD HUD build's
        constructor table points at 0x7D0BDC, right after `bl` to an abort() stub."""
        return not t and self.image.region(a) not in (0, None) and (a - 4, False) in self.seen and \
            self.image.read32(a - 4) >> 24 == 0xEB

    def walk_ok(self, key):
        """plausible_walk, stricter in a mod's code range (not .text), which also holds the mod's
        data: zero words (padding, tables) decode as valid instructions there, and constants
        such as 0x7FFFFF land in it, so a walk through a zero word or a Thumb start inside a
        word already decoded as ARM is rejected."""
        a, t = key & ~1, bool(key & 1)
        if self.image.region(a) in (0, None):
            return self.plausible_walk(key)
        return not (t and (a & ~3, False) in self.seen) and self.plausible_walk(key, no_zero=True)

    def plausible_walk(self, key, limit=512, no_zero=False):
        """Stronger check for candidates found in data: following control flow from `key`
        must decode cleanly (no trap, no exit from .text) until it reaches a return/jump or
        `limit` instructions have been decoded (long functions)."""
        if not self.plausible(key):
            return False
        t = bool(key & 1)
        seen, stack, budget = set(), [key & ~1], limit
        while stack and budget:
            a = stack.pop()
            if a in seen:
                continue
            if not self.image.in_text(a, 2 if t else 4):
                return False
            seen.add(a)
            budget -= 1
            if no_zero and (self.image.read16(a) if t else self.image.read32(a)) == 0:
                return False
            insn = self.decode(a, t)
            if insn.kind == "trap":
                return False
            if insn.kind in ("ret", "jump") and insn.cond == AL:
                return True
            stack += self.successors(insn, local_only=False)
        return True  # every path ended (e.g. only tail calls) or the budget ran out cleanly

    def jump_table(self, insn):
        """Case targets of the two ARMCC switch idioms, bounded by an earlier `cmp rX, #N`:
        `addls pc, pc, rX, lsl #2` (a table of branches right after) and
        `ldrlo pc, [pc, rX, lsl #2]` (a table of absolute addresses right after)."""
        out = []
        for kind, rm, pc in insn.extra_targets:
            if insn.thumb or insn.cond not in (3, 9):  # LO: N cases, LS: N + 1 cases
                continue
            bound = None
            for back in range(1, 17):  # the compiler interleaves independent instructions
                p = insn.addr - 4 * back
                if not self.image.in_text(p):
                    break
                prev = self.decode(p, False)
                if prev.switch_cmp and prev.switch_cmp[0] == rm:
                    bound = prev.switch_cmp[1]
                    break
                # anything that changes control flow, the flags or the index ends the search
                if prev.kind != "normal" or "c->z =" in prev.code or "_flags(" in prev.code or \
                        f"c->r[{rm}] =" in prev.code:
                    break
            if bound is not None and bound < 4096:
                count = bound + 1 if insn.cond == 9 else bound
            else:  # no usable cmp (e.g. it sits before a literal pool): scan the table itself
                count = 0
                while count < 1024 and self.image.in_text(pc + 4 * count):
                    word = self.image.read32(pc + 4 * count)
                    if kind == "table" and (word & 0xFF000000) != 0xEA000000:
                        break  # branch slots: unconditional B
                    if kind == "abs_table" and (not self.image.in_text(word) or word & 3 or
                                                abs(word - pc) > 0x100000):
                        break  # absolute slots: nearby, aligned code addresses
                    count += 1
            if kind == "table":
                out += [pc + 4 * k for k in range(count)]
            else:  # abs_table: words at pc
                for k in range(count):
                    if self.image.in_text(pc + 4 * k):
                        t = self.image.read32(pc + 4 * k)
                        if self.image.in_text(t) and not t & 3:
                            out.append(t)
        return out

    def successors(self, insn, local_only):
        """Intra-function successors as (addr) list; local_only excludes tail-call branches."""
        out = []
        terminal = insn.kind in ("ret", "jump", "trap") or (insn.kind == "branch")
        if not terminal or insn.cond != AL:
            out.append(insn.next)
        if insn.kind == "branch":
            t = insn.target
            same_mode = bool(t & 1) == insn.thumb
            if same_mode and (not local_only or t not in self.entries or t == self.current):
                out.append(t & ~1)
        out += self.jump_table(insn)
        return out

    def discover(self, seeds):
        """Global walk: every call target / literal function pointer becomes an entry.
        `self.seen` keeps every decoded (address, thumb) for the data-candidate filter."""
        work = [k for k in seeds if k not in self.entries]
        self.entries.update(seeds)
        seen = self.seen
        self.current = None
        while work:
            key = work.pop()
            stack = [key & ~1]
            t = bool(key & 1)
            while stack:
                a = stack.pop()
                if (a, t) in seen or not self.image.in_text(a, 2 if t else 4):
                    continue
                seen.add((a, t))
                insn = self.decode(a, t)
                for s in self.successors(insn, local_only=False):
                    stack.append(s)
                new = []
                if insn.kind == "branch":
                    self.branch_targets.add(insn.target)
                # A branch that changes mode or code range is a tail call: a mod's hook stub returns
                # with `b` into the middle of the original function, which must not be inlined into it.
                if insn.kind == "call" or (insn.kind == "branch" and (bool(insn.target & 1) != t or
                                           self.image.region(a) != self.image.region(insn.target & ~1))):
                    new.append(insn.target)
                lit = None
                if insn.literal is not None:
                    v = self.image.read32(insn.literal)
                    if self.image.in_text(v & ~1):
                        new.append(v)
                        lit = v
                for k in new:
                    ok = self.walk_ok(k) if k == lit and self.image.region(k & ~1) else self.plausible(k)
                    if k not in self.entries and ok:
                        self.entries.add(k)
                        work.append(k)
                if insn.pointer is not None:
                    self.adr_pointers.append(insn.pointer)

    def vet(self, v):
        """Accept a code-pointer candidate that did not come from a direct call."""
        a, t = v & ~1, bool(v & 1)
        if v in self.entries or not self.image.in_text(a):
            return False
        if (a, not t) in self.seen:
            return False  # decoded in the other mode: not a function start
        if (a, t) in self.seen and v not in self.branch_targets and not self.after_noreturn_bl(a, t):
            return False  # inside code already decoded (tail-call targets excepted)
        if (a - 2, True) in self.seen and self.decode(a - 2, True).size == 4:
            return False  # second half of a Thumb BL/BLX pair
        return self.walk_ok(v)

    def pc_relative_pointers(self):
        """Code addresses computed position-independently: `ldr rX, =offset` followed
        (within a few instructions) by `add rX, pc` (Thumb) or `add rX, pc, rX` (ARM)."""
        found = []
        for a, t in list(self.seen):
            if t:
                hw = self.image.read16(a)
                if (hw & 0xFF78) != 0x4478:  # add rd, pc
                    continue
                rd, pcv, step, window = (hw & 7) | ((hw >> 4) & 8), a + 4, 2, 10
            else:
                w = self.image.read32(a)
                rd = (w >> 12) & 0xF
                if (w & 0x0FFF0FF0) != 0x008F0000 or (w & 0xF) != rd:  # add rd, pc, rd
                    continue
                pcv, step, window = a + 8, 4, 6
            for back in range(1, window):
                p = a - back * step
                if (p, t) not in self.seen:
                    break
                insn = self.decode(p, t)
                if insn.literal is not None and insn.code.startswith(f"c->r[{rd}] = "):
                    v = (self.image.read32(insn.literal) + pcv) & 0xFFFFFFFF
                    if self.image.in_text(v & ~1) and v not in self.entries and self.walk_ok(v):
                        found.append(v)
                    break
        return found

    def build(self, key):
        """Instructions of one function, sorted, plus its local labels."""
        self.current = key
        t = bool(key & 1)
        body, labels, stack = {}, {key & ~1}, [key & ~1]
        while stack:
            a = stack.pop()
            if a in body or not self.image.in_text(a, 2 if t else 4):
                continue
            insn = self.decode(a, t)
            body[a] = insn
            succ = self.successors(insn, local_only=True)
            for s in succ:
                if s != insn.next or insn.kind == "branch":  # a branch to the next insn still needs its label
                    labels.add(s)
                stack.append(s)
        return [body[a] for a in sorted(body)], labels

    # ------------------------------------------------------------------ emission

    @staticmethod
    def name(key):
        return f"func_{key:08X}"

    def resume_points(self, insns, labels):
        """Addresses a savestate can resume this function at: svc instructions and the one
        after them, call return sites and loop back-edge targets (see recomp::resume)."""
        by_addr = {i.addr: i for i in insns}
        points = set()
        for i, insn in enumerate(insns):
            if "recomp::svc(" in insn.code:
                points.update((insn.addr, insn.next))
            elif insn.kind in ("call", "icall"):
                points.add(insn.link & ~1)
            elif insn.kind == "jump" and not insn.thumb and insn.addr - 4 in by_addr and \
                    self.image.read32(insn.addr - 4) == MOV_LR_PC:
                points.add(insn.next)
            elif insn.kind == "branch" and insn.target & ~1 in by_addr and insn.target & ~1 <= insn.addr:
                points.add(insn.target & ~1)
        return {a for a in points if a in by_addr}

    def emit_function(self, key, insns, labels, out):
        t = bool(key & 1)
        resume = self.resume_points(insns, labels)
        labels |= resume
        addrs = {i.addr for i in insns}
        for a in addrs & self.insn_hooks.keys():  # an instruction hook's jump targets are local labels
            if missing := [h(x) for x in self.insn_hooks[a][1] if x not in addrs]:
                raise SystemExit(f"hooks.txt: {h(a)} jumps outside its function: {', '.join(missing)}")
            labels |= set(self.insn_hooks[a][1])
        out.append(f"u32 {self.name(key)}(recomp::Context* c) {{")
        out.append("    u32 jt = 0;")
        out.append("    u32 entry_lr = c->r[14];  // a computed jump here is a return (e.g. pop {r3}; bx r3)")
        out.append("    if (c->resume) [[unlikely]] { jt = c->resume; c->resume = 0; entry_lr = recomp::RESUMED; goto dispatch; }")
        out.append(f"    goto L_{key & ~1:08X};")
        by_addr = {i.addr: i for i in insns}
        for idx, insn in enumerate(insns):
            if insn.addr in labels:
                out.append(f"L_{insn.addr:08X}:")
            stmt = self.emit_insn(insn, by_addr)
            if insn.addr in self.insn_hooks and not insn.thumb:  # replaces the instruction (keeps its condition)
                name, targets = self.insn_hooks[insn.addr]
                stmt = (f"{{ u32 t = recomp::hooks::{name}(c); if (t) {{ jt = t; goto dispatch; }} }}" if targets
                        else f"recomp::hooks::{name}(c);")
            if "recomp::svc(" in insn.code:  # savestates resume a thread parked in a syscall here
                stmt = f"c->r[15] = {h(insn.addr | t)}; {stmt}"
            if insn.cond != AL and insn.kind != "branch" or (insn.kind == "branch" and insn.cond != AL):
                stmt = f"if ({COND[insn.cond]}) {{ {stmt} }}"
            out.append(f"    {stmt} // {insn.addr:08X} {insn.asm}")
            falls = insn.cond != AL or insn.kind in ("normal", "call", "icall")
            nxt = insns[idx + 1].addr if idx + 1 < len(insns) else None
            if falls and nxt != insn.next:
                target = insn.next
                out.append(f"    goto L_{target:08X};" if target in by_addr
                           else f"    return recomp::fell_off(c, {h(target)});")
                if target in by_addr:
                    labels.add(target)
        out.append("dispatch:")
        out.append("    switch (jt & ~1u) {")
        for a in sorted(labels):
            if a in by_addr:
                out.append(f"    case {h(a)}: goto L_{a:08X};")
        out.append("    default: return ((jt ^ entry_lr) & ~1u) && entry_lr != recomp::RESUMED ? recomp::jump(c, jt) : jt;")
        out.append("    }")
        out.append("}")
        out.append("")
        return resume

    def emit_insn(self, insn, by_addr):
        k = insn.kind
        if k == "normal":
            return f"{{ {insn.code} }}" if insn.code else ";"
        if k == "trap":
            return insn.code
        if k == "branch":
            if insn.target & ~1 in by_addr and bool(insn.target & 1) == insn.thumb and \
                    (insn.target not in self.entries or insn.target == self.current):
                if insn.target & ~1 <= insn.addr:  # loop back-edge: let pending interrupts in
                    return f"RECOMP_CHECK_INTERRUPT({h(insn.target)}); goto L_{insn.target & ~1:08X};"
                return f"goto L_{insn.target & ~1:08X};"
            if insn.target in self.entries:
                return f"return {self.name(insn.target)}(c);"
            return f"return recomp::jump(c, {h(insn.target)});"
        if k == "call":
            link = h(insn.link)
            callee = (f"{self.name(insn.target)}(c)" if insn.target in self.entries
                      else f"recomp::call(c, {h(insn.target)})")
            return (f"c->r[14] = {link}; {{ u32 rr = {callee}; "
                    f"if ((rr ^ {link}) & ~1u) {{ jt = rr; goto dispatch; }} }}")
        if k == "ret":
            return f"{{ {insn.code} return {insn.expr}; }}"
        if k == "jump":
            prev = by_addr.get(insn.addr - 4)
            if not insn.thumb and prev is not None and self.image.read32(insn.addr - 4) == MOV_LR_PC:
                link = h(insn.addr + 4)  # ARMv4 call idiom: mov lr, pc; bx/ldr pc
                return (f"{{ {insn.code} u32 t = {insn.expr}; u32 rr = recomp::call(c, t); "
                        f"if ((rr ^ {link}) & ~1u) {{ jt = rr; goto dispatch; }} }}")
            return f"{{ {insn.code} jt = {insn.expr}; goto dispatch; }}"
        if k == "icall":
            link = h(insn.link)
            return (f"{{ {insn.code} u32 t = {insn.expr}; c->r[14] = {link}; u32 rr = recomp::call(c, t); "
                    f"if ((rr ^ {link}) & ~1u) {{ jt = rr; goto dispatch; }} }}")
        raise ValueError(k)


def init_array_bounds(image, loop=0x001009E0):
    """[begin, end) of the CTR-SDK .init_array, decoded from the Thumb loop that walks it
    (same address in v1.0 and v1.1; the table moves with .rodata):
    push {r4-r6,lr}; ldr r4,[pc,#i]; add r4,pc; ldr r5,[pc,#j]; add r5,pc."""
    ins = [image.read16(loop + 2 * k) for k in range(5)]
    if ins[0] != 0xB570 or ins[1] >> 8 != 0x4C or ins[2] != 0x447C or ins[3] >> 8 != 0x4D or ins[4] != 0x447D:
        raise SystemExit(f"init_array_bounds: unexpected code at {loop:#x} (unknown game version?)")

    def pc_rel(ldr_at, add_at):  # literal at Align(pc, 4) + imm8*4, plus the add's pc
        lit = ((ldr_at + 4) & ~3) + (image.read16(ldr_at) & 0xFF) * 4
        return (image.read32(lit) + add_at + 4) & 0xFFFFFFFF

    return pc_rel(loop + 2, loop + 4), pc_rel(loop + 6, loop + 8)


def init_array_seeds(image):
    """CTR-SDK .init_array: relative offsets (each added to its own address) between the
    bounds the crt0 loop uses."""
    begin, end = init_array_bounds(image)
    seeds = []
    for a, v in image.words(1):
        t = (a + v) & 0xFFFFFFFF
        if begin <= a < end and image.in_text(t & ~1):
            seeds.append(t)
    return seeds


def select_hooks(text: str, code_sha256: str):
    """hooks.txt section whose `[<version> <sha256>]` header matches the code.bin hash ->
    (version, {entry: hook}, {instruction VA: (hook, branch targets)}); (None, {}, {}) when no
    section matches. `<VA> <hook> insn [<target>...]` hooks one instruction instead of a whole
    function; with targets, the hook returns 0 (fall through) or one of them (a jump)."""
    sections, cur = {}, None
    for line in text.splitlines():
        line = line.split("#")[0].split()
        if not line:
            continue
        if line[0].startswith("["):
            version, sha = " ".join(line).strip("[]").split()
            cur = sections[sha.lower()] = (version, {}, {})
        elif cur is None:
            raise SystemExit("hooks.txt: hook outside a [<version> <sha256>] section")
        else:
            if line[2:3] == ["insn"]:
                cur[2][int(line[0], 16)] = (line[1], tuple(int(t, 16) for t in line[3:]))
            else:
                cur[1][int(line[0], 16)] = line[1]
    return sections.get(code_sha256.lower(), (None, {}, {}))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_data", type=Path)
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--chunks", type=int, default=64)
    ap.add_argument("--extra", type=Path, default=Path(__file__).parent / "extra_entries.txt")
    ap.add_argument("--hooks", type=Path, default=Path(__file__).parent / "hooks.txt")
    args = ap.parse_args()

    image = Image(args.game_data)
    rc = Recompiler(image)
    seeds = [image.text[0]] + init_array_seeds(image)
    if args.extra.exists():
        for line in args.extra.read_text().splitlines():
            line = line.split("#")[0].strip()
            if line:
                seeds.append(int(line, 16))
    rc.discover(seeds)
    # Function pointers stored in .rodata/.data (vtables, callback tables). Integers that
    # merely fall in the .text range are rejected: a candidate must not land inside code
    # already decoded (in either mode) and must look like a well-formed function.
    data_seeds = []
    for seg in (1, 2):
        for _, v in image.words(seg):
            a, t = v & ~1, bool(v & 1)
            if rc.vet(v):
                data_seeds.append(v)
    rc.discover(data_seeds)
    while True:  # position-independent function pointers, until nothing new appears
        extra = rc.pc_relative_pointers()
        adr, rc.adr_pointers = rc.adr_pointers, []
        extra += [v for v in set(adr) if rc.vet(v)]
        if not extra:
            break
        rc.discover(extra)

    sha = hashlib.sha256(image.code).hexdigest()
    version, hooks, rc.insn_hooks = select_hooks(args.hooks.read_text(), sha)  # -> src/recomp/hooks.cpp
    print(f"code.bin {sha}: " + (f"v{version}, {len(hooks)} native hooks, {len(rc.insn_hooks)} instruction hooks"
          if version else "unknown version, building without native hooks"), file=sys.stderr)
    if missing := [h(a) for a in hooks if a not in rc.entries]:
        raise SystemExit(f"hooks.txt [{version}]: not a function entry: {', '.join(missing)}")
    if bad := [h(a) for a in rc.insn_hooks if (a, False) not in rc.seen or rc.decode(a, False).kind != "normal"]:
        raise SystemExit(f"hooks.txt [{version}]: not a decoded ARM data instruction: {', '.join(bad)}")

    funcs = {}
    for key in sorted(rc.entries):
        funcs[key] = rc.build(key)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    hdr = ["// Generated by tools/recomp/recomp.py — do not edit, do not commit.", "#pragma once",
           '#include "recomp/hooks.h"', ""]
    hdr += [f"u32 {rc.name(k)}(recomp::Context* c);" for k in funcs]
    (args.out_dir / "recomp_funcs.h").write_text("\n".join(hdr) + "\n")

    # Balance chunks by instruction count.
    total = sum(len(v[0]) for v in funcs.values())
    per_chunk = total / args.chunks + 1
    chunks = [[] for _ in range(args.chunks)]
    acc, ci = 0, 0
    resume = {}  # resume point (address | thumb) -> a function containing it
    for key, (insns, labels) in funcs.items():
        if acc > per_chunk * (ci + 1) and ci < args.chunks - 1:
            ci += 1
        rc.current = key
        if key in hooks:
            chunks[ci] += [f"u32 {rc.name(key)}(recomp::Context* c) {{ return recomp::hooks::{hooks[key]}(c); }}", ""]
        else:
            for a in rc.emit_function(key, insns, labels, chunks[ci]):
                resume.setdefault(a | (key & 1), key)
        acc += len(insns)
    for i, lines in enumerate(chunks):
        text = ["// Generated by tools/recomp/recomp.py — do not edit, do not commit.",
                '#include "recomp_funcs.h"', "", *lines]
        (args.out_dir / f"recomp_{i:02d}.cpp").write_text("\n".join(text) + "\n")

    table = ['// Generated by tools/recomp/recomp.py — do not edit, do not commit.',
             '#include "recomp_funcs.h"', "",
             "namespace recomp {",
             "extern const FuncEntry kFunctions[] = {"]
    table += [f"    {{{h(k)}, {rc.name(k)}}}," for k in funcs]
    table += ["};", f"extern const u32 kFunctionCount = {len(funcs)};",
              "extern const FuncEntry kResumePoints[] = {"]
    table += [f"    {{{h(a)}, {rc.name(resume[a])}}}," for a in sorted(resume)]
    table += ["};", f"extern const u32 kResumePointCount = {len(resume)};", "}  // namespace recomp", ""]
    (args.out_dir / "recomp_table.cpp").write_text("\n".join(table))

    traps = collections.Counter()
    for (insns, _) in funcs.values():
        for insn in insns:
            if insn.kind == "trap":
                traps[insn.asm.split(" ")[0] if " " in insn.asm else insn.asm] += 1
    thumb_funcs = sum(1 for k in funcs if k & 1)
    stats = [f"functions: {len(funcs)} ({thumb_funcs} Thumb, {len(funcs) - thumb_funcs} ARM)",
             f"instructions: {total}",
             f"trap instructions: {sum(traps.values())}",
             f"native hooks: {len(hooks)}",
             f"instruction hooks: {len(rc.insn_hooks)}",
             f"resume points: {len(resume)}"]
    stats += [f"  {n:6d}  {what}" for what, n in traps.most_common(40)]
    (args.out_dir / "stats.txt").write_text("\n".join(stats) + "\n")
    print("\n".join(stats[:3]))


if __name__ == "__main__":
    main()
