# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""VFPv2 lifter (cp10 = single, cp11 = double) for the ARM decoder.
Note: generated code must be compiled with -ffp-contract=off (VFP MAC is not fused)."""
from common import h, reg, trap


def _regs(w, dbl):
    vd, vn, vm = (w >> 12) & 0xF, (w >> 16) & 0xF, w & 0xF
    D, N, M = (w >> 22) & 1, (w >> 7) & 1, (w >> 5) & 1
    if dbl:
        return vd, vn, vm
    return vd << 1 | D, vn << 1 | N, vm << 1 | M


def _get(dbl, i):
    return f"recomp::fd(c, {i})" if dbl else f"recomp::fs(c, {i})"


def _set(dbl, i, v):
    return f"recomp::set_fd(c, {i}, {v});" if dbl else f"recomp::set_fs(c, {i}, (float)({v}));"


def lift(image, insn, w, pc):
    if w & 0x10:
        return lift_transfer(insn, w, pc)
    dbl = (w >> 8) & 1
    d, n, m = _regs(w, dbl)
    D, N, M = _get(dbl, d), _get(dbl, n), _get(dbl, m)
    pqrs = ((w >> 23) & 1) << 3 | ((w >> 21) & 1) << 2 | ((w >> 20) & 1) << 1 | ((w >> 6) & 1)
    T = "double" if dbl else "float"
    ops = {
        0b0000: f"{D} + {T}({N} * {M})", 0b0001: f"{D} - {T}({N} * {M})",
        0b0010: f"-{D} + {T}({N} * {M})", 0b0011: f"-{D} - {T}({N} * {M})",
        0b0100: f"{N} * {M}", 0b0101: f"-({N} * {M})", 0b0110: f"{N} + {M}",
        0b0111: f"{N} - {M}", 0b1000: f"{N} / {M}",
    }
    insn.asm = "vfp"
    if pqrs in ops:
        insn.code = _set(dbl, d, ops[pqrs])
        return insn
    if pqrs != 0b1111:
        return trap(insn, f"vfp dp {w:08x}")
    ext = ((w >> 16) & 0xF) << 1 | (w >> 7) & 1
    vd, vm, Dbit, Mbit = (w >> 12) & 0xF, w & 0xF, (w >> 22) & 1, (w >> 5) & 1
    sd, sm = vd << 1 | Dbit, vm << 1 | Mbit  # single-precision numbering of Vd/Vm
    if ext == 0b00000:
        insn.code = _set(dbl, d, M)
    elif ext == 0b00001:
        insn.code = _set(dbl, d, f"std::fabs({M})")
    elif ext == 0b00010:
        insn.code = _set(dbl, d, f"-{M}")
    elif ext == 0b00011:
        insn.code = _set(dbl, d, f"std::sqrt({M})")
    elif ext in (0b01000, 0b01001):
        insn.code = f"recomp::vfp_cmp(c, {D}, {M});"
    elif ext in (0b01010, 0b01011):
        insn.code = f"recomp::vfp_cmp(c, {D}, 0.0);"
    elif ext == 0b01111:  # precision conversion
        insn.code = (f"recomp::set_fs(c, {sd}, (float)recomp::fd(c, {vm}));" if dbl
                     else f"recomp::set_fd(c, {vd}, (double)recomp::fs(c, {sm}));")
    elif ext in (0b10000, 0b10001):  # int (in Sm) -> float/double
        src = f"(s32)c->s[{sm}]" if ext & 1 else f"c->s[{sm}]"
        insn.code = _set(dbl, d, f"({T}){src}")
    elif ext in (0b11000, 0b11001, 0b11010, 0b11011):  # float/double -> int (into Sd)
        signed = "true" if ext & 0b10 else "false"
        toward_zero = "true" if ext & 1 else "false"
        insn.code = f"c->s[{sd}] = recomp::vfp_to_int(c, {M}, {signed}, {toward_zero});"
    else:
        return trap(insn, f"vfp ext {w:08x}")
    return insn


def lift_transfer(insn, w, pc):
    opc, load = (w >> 21) & 7, (w >> 20) & 1
    vn, rt, N = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 7) & 1
    insn.asm = "vmov"
    if not (w >> 8) & 1:  # cp10
        if opc == 0:
            sn = vn << 1 | N
            insn.code = f"c->r[{rt}] = c->s[{sn}];" if load else f"c->s[{sn}] = {reg(rt, pc)};"
            return insn
        if opc == 7:
            if vn == 1:  # FPSCR
                if load and rt == 15:
                    insn.code, insn.asm = "recomp::fmstat(c);", "vmrs apsr_nzcv"
                elif load:
                    insn.code = f"c->r[{rt}] = c->fpscr;"
                else:
                    insn.code = f"c->fpscr = {reg(rt, pc)};"
                return insn
            if vn in (0, 8) and load:  # FPSID / FPEXC
                insn.code = f"c->r[{rt}] = {'0x41000000u' if vn == 0 else '0x40000000u'};"
                return insn
            if vn == 8 and not load:
                return insn  # FPEXC write: VFP stays enabled
        return trap(insn, f"vfp xfer {w:08x}")
    if opc in (0, 1):  # FMDLR/FMRDL (low), FMDHR/FMRDH (high)
        si = vn * 2 + opc
        insn.code = f"c->r[{rt}] = c->s[{si}];" if load else f"c->s[{si}] = {reg(rt, pc)};"
        return insn
    return trap(insn, f"vfp xfer {w:08x}")


def lift_ldst(image, insn, w, pc):
    p, u, wb, load = (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1, (w >> 20) & 1
    rn = (w >> 16) & 0xF
    dbl = (w >> 8) & 1
    imm8 = w & 0xFF
    insn.asm = "vldr/vstr"
    if (w & 0x0FE00000) == 0x0C400000:  # 64-bit transfer core<->VFP
        rt, rt2, vm, M = (w >> 12) & 0xF, (w >> 16) & 0xF, w & 0xF, (w >> 5) & 1
        lo = vm * 2 if dbl else (vm << 1 | M)
        if load:
            insn.code = f"c->r[{rt}] = c->s[{lo}]; c->r[{rt2}] = c->s[{lo + 1}];"
        else:
            insn.code = f"c->s[{lo}] = {reg(rt, pc)}; c->s[{lo + 1}] = {reg(rt2, pc)};"
        return insn
    vd, D = (w >> 12) & 0xF, (w >> 22) & 1
    first = vd * 2 if dbl else (vd << 1 | D)  # index into c->s
    base = h(pc & ~3) if rn == 15 else f"c->r[{rn}]"
    if p and not wb:  # single register, immediate offset
        off = imm8 * 4
        if rn == 15 and load:
            a = ((pc & ~3) + (off if u else -off)) & 0xFFFFFFFF
            if image.in_text(a, 8 if dbl else 4):
                insn.code = " ".join(f"c->s[{first + k}] = {h(image.read32(a + 4 * k))};"
                                     for k in range(2 if dbl else 1))
                return insn
        words = 2 if dbl else 1
        ops = []
        for k in range(words):
            ops.append(f"c->s[{first + k}] = recomp::r32(a + {4 * k}u);" if load
                       else f"recomp::w32(a + {4 * k}u, c->s[{first + k}]);")
        insn.code = f"{{ u32 a = {base} {'+' if u else '-'} {off}u; {' '.join(ops)} }}"
        return insn
    # load/store multiple: IA (P=0,U=1) or DB (P=1,U=0,W=1)
    if not ((p == 0 and u == 1) or (p == 1 and u == 0 and wb)):
        return trap(insn, f"vldm mode {w:08x}")
    words = imm8 & ~1 if dbl else imm8  # FLDMX/FSTMX (odd imm8) transfer imm8 - 1 words
    start = "b" if u else f"b - {4 * imm8}u"
    parts = [f"u32 b = {base}; u32 a = {start};"]
    for k in range(words):
        parts.append(f"c->s[{first + k}] = recomp::r32(a + {4 * k}u);" if load
                     else f"recomp::w32(a + {4 * k}u, c->s[{first + k}]);")
    if wb:
        parts.append(f"c->r[{rn}] = b {'+' if u else '-'} {4 * imm8}u;")
    insn.code = "{ " + " ".join(parts) + " }"
    return insn
