# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""ARM (A32, ARMv6K) instruction lifter: one 32-bit word -> Insn with C++ semantics."""
from common import AL, Insn, h, reg, ror32, shift_imm, trap, SHIFT_NAMES
import vfp

DP_NAMES = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
            "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"]
LOGICAL = {0, 1, 8, 9, 12, 13, 14, 15}


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def dp_result(op, a, b):
    return {0: f"{a} & {b}", 1: f"{a} ^ {b}", 2: f"{a} - {b}", 3: f"{b} - {a}", 4: f"{a} + {b}",
            5: f"{a} + {b} + c->c", 6: f"{a} + ~{b} + c->c", 7: f"{b} + ~{a} + c->c",
            12: f"{a} | {b}", 13: b, 14: f"{a} & ~{b}", 15: f"~{b}"}[op]


def dp_flags(op, a, b):
    """Flag-setting arithmetic: returns the expression computing result + NZCV."""
    return {2: f"recomp::sub_flags(c, {a}, {b})", 3: f"recomp::sub_flags(c, {b}, {a})",
            4: f"recomp::add_flags(c, {a}, {b})", 5: f"recomp::adc_flags(c, {a}, {b})",
            6: f"recomp::sbc_flags(c, {a}, {b})", 7: f"recomp::sbc_flags(c, {b}, {a})",
            10: f"recomp::sub_flags(c, {a}, {b})", 11: f"recomp::add_flags(c, {a}, {b})"}[op]


def emit_dp(insn, op, s, rd, a_expr, b_stmt, carry_expr):
    """Shared by ARM and Thumb. b_stmt declares `u32 b` (and `u32 sc` if needed)."""
    parts = [f"u32 a = {a_expr};" if op not in (13, 15) else "", b_stmt]
    logical = op in LOGICAL
    if s and logical and carry_expr not in ("sc", "c->c"):
        parts.append(f"u32 sc = {carry_expr};")
    if op in (8, 9, 10, 11):  # compare/test: flags only
        if op == 8:
            parts.append("u32 r = a & b;")
        elif op == 9:
            parts.append("u32 r = a ^ b;")
        else:
            parts.append(f"{dp_flags(op, 'a', 'b')};")
            return " ".join(p for p in parts if p)
        parts.append("c->n = r >> 31; c->z = r == 0;")
        if carry_expr != "c->c":
            parts.append("c->c = sc;")
        return " ".join(p for p in parts if p)
    if s and not logical:
        parts.append(f"u32 r = {dp_flags(op, 'a', 'b')};")
    else:
        parts.append(f"u32 r = {dp_result(op, 'a', 'b')};")
        if s:
            parts.append("c->n = r >> 31; c->z = r == 0;")
            if carry_expr != "c->c":
                parts.append("c->c = sc;")
    if rd == 15:
        return " ".join(p for p in parts if p)  # caller turns r into a jump/return
    parts.append(f"c->r[{rd}] = r;")
    return " ".join(p for p in parts if p)


def lift(image, addr, w):
    insn = Insn(addr, 4, False, cond=w >> 28)
    pc = addr + 8
    cond = w >> 28
    if cond == 0xF:
        return lift_unconditional(image, insn, w, pc)
    op3 = (w >> 25) & 7

    if op3 in (0, 1):
        if op3 == 0 and (w & 0x90) == 0x90:
            return lift_mul_extra(image, insn, w, pc)
        if op3 == 0 and (w & 0x01900000) == 0x01000000:  # misc space (opcode 10xx, S=0)
            return lift_misc(image, insn, w, pc)
        return lift_dp(image, insn, w, pc)
    if op3 in (2, 3):
        if op3 == 3 and w & 0x10:
            return lift_media(image, insn, w, pc)
        return lift_ldst(image, insn, w, pc)
    if op3 == 4:
        return lift_ldm(image, insn, w, pc)
    if op3 == 5:
        off = sext(w & 0xFFFFFF, 24) << 2
        target = (pc + off) & 0xFFFFFFFF
        if w & (1 << 24):
            insn.kind, insn.target, insn.link = "call", target, addr + 4
            insn.asm = f"bl {target:#x}"
        else:
            insn.kind, insn.target = "branch", target
            insn.asm = f"b {target:#x}"
        return insn
    if op3 == 6:
        if ((w >> 8) & 0xE) == 0xA:
            return vfp.lift_ldst(image, insn, w, pc)
        return trap(insn, f"coprocessor load/store {w:08x}")
    # op3 == 7
    if w & (1 << 24):
        insn.code = f"recomp::svc(c, {w & 0xFF});"
        insn.asm = f"svc {w & 0xFFFFFF:#x}"
        return insn
    if ((w >> 8) & 0xE) == 0xA:
        return vfp.lift(image, insn, w, pc)
    return lift_coproc(insn, w)


def lift_unconditional(image, insn, w, pc):
    insn.cond = AL
    if (w & 0x0E000000) == 0x0A000000:  # BLX imm
        off = (sext(w & 0xFFFFFF, 24) << 2) | ((w >> 23) & 2)
        target = (pc + off) & 0xFFFFFFFF
        insn.kind, insn.target, insn.link = "call", target | 1, insn.addr + 4
        insn.asm = f"blx {target:#x}"
        return insn
    if (w & 0x0D70F000) == 0x0550F000:  # PLD
        insn.asm = "pld"
        return insn
    if w == 0xF57FF01F:  # CLREX
        insn.code = "c->excl = 0;"
        insn.asm = "clrex"
        return insn
    return trap(insn, f"unconditional {w:08x}")


def operand2(w, pc):
    """Data-processing operand 2. Returns (stmt declaring u32 b [and sc], carry_expr)."""
    if w & (1 << 25):
        rot = ((w >> 8) & 0xF) * 2
        imm = ror32(w & 0xFF, rot)
        return f"u32 b = {h(imm)};", ("c->c" if rot == 0 else str(imm >> 31) + "u")
    rm = reg(w & 0xF, pc)
    typ = (w >> 5) & 3
    if w & 0x10:  # register-specified shift
        rs = reg((w >> 8) & 0xF, pc)
        return (f"u32 sc; u32 b = recomp::shift_reg({typ}, {rm}, {rs} & 0xFFu, c->c, &sc);", "sc")
    val, carry = shift_imm(typ, (w >> 7) & 0x1F, "m")
    return f"u32 m = {rm}; u32 b = {val};", carry


def lift_dp(image, insn, w, pc):
    op = (w >> 21) & 0xF
    s = (w >> 20) & 1
    rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
    if op in (8, 9, 10, 11) and not s:  # immediate form of the misc space
        if op in (9, 11):
            return lift_msr(insn, w, pc)
        return trap(insn, f"undefined dp {w:08x}")
    b_stmt, carry = operand2(w, pc)
    insn.code = emit_dp(insn, op, s, rd, reg(rn, pc), b_stmt, carry)
    insn.asm = f"{DP_NAMES[op]}{'s' if s else ''} r{rd}, r{rn}"
    if op in (2, 4) and rn == 15 and w & (1 << 25):  # ADR
        imm = ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)
        insn.pointer = (pc + imm if op == 4 else pc - imm) & 0xFFFFFFFF
    if op == 10 and w & (1 << 25):
        insn.switch_cmp = (rn, ror32(w & 0xFF, ((w >> 8) & 0xF) * 2))
    if rd == 15 and op not in (8, 9, 10, 11):
        if op == 13 and not (w & (1 << 25)) and (w & 0xFF0) == 0 and (w & 0xF) == 14:
            insn.kind, insn.code, insn.expr = "ret", "", "c->r[14]"  # mov pc, lr
        else:
            insn.kind, insn.expr = "jump", "r"
            # add pc, pc, rX, lsl #2 jump table: the function builder adds the targets
            if op == 4 and rn == 15 and not (w & (1 << 25)) and (w & 0xFF0) == 0x100:
                insn.extra_targets = [("table", w & 0xF, pc)]
    return insn


def lift_msr(insn, w, pc):
    if w & (1 << 25):
        val = h(ror32(w & 0xFF, ((w >> 8) & 0xF) * 2))
    else:
        val = reg(w & 0xF, pc)
    if (w >> 22) & 1:
        return trap(insn, "msr spsr")
    mask = (w >> 16) & 0xF
    if mask == 0:  # hint space: NOP/YIELD/WFE/WFI/SEV
        insn.asm = "hint"
        return insn
    insn.code = f"recomp::write_cpsr(c, {val}, {mask});"
    insn.asm = "msr cpsr"
    return insn


def lift_misc(image, insn, w, pc):
    op2 = (w >> 4) & 0xF
    op = (w >> 21) & 3
    rd, rm = (w >> 12) & 0xF, w & 0xF
    if op2 == 0:
        if op & 1:
            return lift_msr(insn, w, pc)
        if (w >> 22) & 1:
            return trap(insn, "mrs spsr")
        insn.code = f"c->r[{rd}] = recomp::read_cpsr(c);"
        insn.asm = "mrs"
        return insn
    if op2 == 1 and op == 1:  # BX
        if rm == 14:
            insn.kind, insn.expr, insn.asm = "ret", "c->r[14]", "bx lr"
        else:
            insn.kind, insn.expr, insn.asm = "jump", reg(rm, pc), f"bx r{rm}"
        return insn
    if op2 == 1 and op == 3:  # CLZ
        insn.code = f"c->r[{rd}] = std::countl_zero({reg(rm, pc)});"
        insn.asm = "clz"
        return insn
    if op2 == 3 and op == 1:  # BLX reg
        insn.kind, insn.expr, insn.link, insn.asm = "icall", reg(rm, pc), insn.addr + 4, f"blx r{rm}"
        return insn
    if op2 == 5:  # QADD/QSUB/QDADD/QDSUB
        rn = (w >> 16) & 0xF
        insn.code = f"c->r[{rd}] = recomp::qaddsub(c, {op}, {reg(rm, pc)}, {reg(rn, pc)});"
        insn.asm = "qadd/qsub"
        return insn
    if op2 == 7:
        return trap(insn, "bkpt")
    if op2 & 0x9 == 0x8:  # signed halfword multiplies
        x, y = (w >> 5) & 1, (w >> 6) & 1
        rdh, rn_acc, rs = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF
        m, s_ = reg(rm, pc), reg(rs, pc)
        hx = f"(s32)(s16)({m} >> {16 * x})"
        hy = f"(s32)(s16)({s_} >> {16 * y})"
        if op == 0:  # SMLAxy
            insn.code = f"c->r[{rdh}] = recomp::add_q(c, (u32)({hx} * {hy}), {reg(rn_acc, pc)});"
        elif op == 1:
            prod = f"(u32)(((s64)(s32){m} * (s64)(s16)({s_} >> {16 * y})) >> 16)"
            if x == 0:  # SMLAWy
                insn.code = f"c->r[{rdh}] = recomp::add_q(c, {prod}, {reg(rn_acc, pc)});"
            else:  # SMULWy
                insn.code = f"c->r[{rdh}] = {prod};"
        elif op == 2:  # SMLALxy: RdHi = rdh, RdLo = rn_acc
            insn.code = (f"{{ s64 acc = (s64)(((u64)c->r[{rdh}] << 32) | c->r[{rn_acc}]) + (s64)({hx} * {hy}); "
                         f"c->r[{rn_acc}] = (u32)acc; c->r[{rdh}] = (u32)((u64)acc >> 32); }}")
        else:  # SMULxy
            insn.code = f"c->r[{rdh}] = (u32)({hx} * {hy});"
        insn.asm = "smul/smla xy"
        return insn
    return trap(insn, f"misc {w:08x}")


def lift_mul_extra(image, insn, w, pc):
    sh = (w >> 5) & 3
    if sh == 0:
        if (w >> 24) & 1:  # SWP/SWPB and exclusives
            return lift_sync(insn, w, pc)
        op = (w >> 21) & 7
        s = (w >> 20) & 1
        rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        m, sv = reg(rm, pc), reg(rs, pc)
        if op in (0, 1):
            expr = f"{m} * {sv}" + (f" + {reg(rn, pc)}" if op == 1 else "")
            insn.code = f"{{ u32 r = {expr}; c->r[{rd}] = r;" + (" c->n = r >> 31; c->z = r == 0;" if s else "") + " }"
            insn.asm = "mul" if op == 0 else "mla"
            return insn
        if op == 2:  # UMAAL
            insn.code = (f"{{ u64 p = (u64){m} * {sv} + c->r[{rn}] + c->r[{rd}]; "
                         f"c->r[{rn}] = (u32)p; c->r[{rd}] = (u32)(p >> 32); }}")
            insn.asm = "umaal"
            return insn
        if op >= 4:  # UMULL UMLAL SMULL SMLAL: RdHi = rd, RdLo = rn
            signed, acc = op >= 6, op & 1
            prod = f"(u64)((s64)(s32){m} * (s64)(s32){sv})" if signed else f"(u64){m} * (u64){sv}"
            if acc:
                prod += f" + (((u64)c->r[{rd}] << 32) | c->r[{rn}])"
            insn.code = (f"{{ u64 p = {prod}; c->r[{rn}] = (u32)p; c->r[{rd}] = (u32)(p >> 32);"
                         + (" c->n = (u32)(p >> 63); c->z = p == 0;" if s else "") + " }")
            insn.asm = ["umull", "umlal", "smull", "smlal"][op - 4]
            return insn
        return trap(insn, f"mul {w:08x}")
    return lift_ldst_extra(image, insn, w, pc)


def lift_sync(insn, w, pc):
    rn, rd, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
    op = (w >> 20) & 0xF
    if (w & 0x0FB00FF0) == 0x01000090:  # SWP / SWPB
        b = (w >> 22) & 1
        t = "8" if b else "32"
        insn.code = (f"{{ u32 a = {reg(rn, pc)}; u32 v = recomp::r{t}(a); recomp::w{t}(a, ({'u8' if b else 'u32'}){reg(rm, pc)}); "
                     f"c->r[{rd}] = v; }}")
        insn.asm = "swp"
        return insn
    if (w & 0xFFF) == 0xF9F:  # LDREX{B,H,D}
        width = {0x9: "32", 0xD: "8", 0xF: "16", 0xB: "64"}.get(op)
        if width == "64":
            insn.code = (f"{{ u32 a = {reg(rn, pc)}; c->excl = 1; c->r[{rd}] = recomp::r32(a); "
                         f"c->r[{rd + 1}] = recomp::r32(a + 4); }}")
        elif width:
            insn.code = f"{{ u32 a = {reg(rn, pc)}; c->excl = 1; c->r[{rd}] = recomp::r{width}(a); }}"
        else:
            return trap(insn, f"ldrex {w:08x}")
        insn.asm = "ldrex"
        return insn
    if (w & 0xFF0) == 0xF90:  # STREX{B,H,D}: rd = status, rm = value
        width = {0x8: "32", 0xC: "8", 0xE: "16", 0xA: "64"}.get(op)
        # shortcut: one guest thread runs at a time, so a store-exclusive always succeeds
        # after its load-exclusive; revisit when guest threads run truly in parallel.
        if width == "64":
            store = f"recomp::w32(a, c->r[{rm}]); recomp::w32(a + 4, c->r[{rm + 1}]);"
        elif width:
            store = f"recomp::w{width}(a, (u{width}){reg(rm, pc)});"
        else:
            return trap(insn, f"strex {w:08x}")
        insn.code = (f"{{ u32 a = {reg(rn, pc)}; if (c->excl) {{ {store} c->excl = 0; c->r[{rd}] = 0; }} "
                     f"else c->r[{rd}] = 1; }}")
        insn.asm = "strex"
        return insn
    return trap(insn, f"sync {w:08x}")


def address_mode(w, pc, offset_expr):
    """Pre/post indexing. Returns (addr_stmt, writeback_stmt)."""
    rn = (w >> 16) & 0xF
    p, u, wb = (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1
    sign = "+" if u else "-"
    base = reg(rn, pc)
    if p:
        addr = f"u32 a = {base} {sign} {offset_expr};" if offset_expr != "0u" else f"u32 a = {base};"
        wbs = f"c->r[{rn}] = a;" if wb else ""
    else:
        addr = f"u32 a = {base};"
        wbs = f"c->r[{rn}] = a {sign} {offset_expr};"
    return addr, wbs


def finish_load(image, insn, w, pc, rd, load_expr, addr_stmt, wb_stmt):
    rn = (w >> 16) & 0xF
    if rd == 15:
        pre = f"{addr_stmt} u32 v = {load_expr}; {wb_stmt}"
        is_pop = rn == 13 and not ((w >> 24) & 1) and ((w >> 23) & 1)
        insn.kind = "ret" if is_pop else "jump"
        insn.code, insn.expr = pre, "v"
        return insn
    insn.code = f"{{ {addr_stmt} u32 v = {load_expr}; {wb_stmt} c->r[{rd}] = v; }}"
    return insn


def lift_ldst(image, insn, w, pc):
    load, byte = (w >> 20) & 1, (w >> 22) & 1
    rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
    if w & (1 << 25):
        val, _ = shift_imm((w >> 5) & 3, (w >> 7) & 0x1F, reg(w & 0xF, pc))
        off = val
    else:
        off = h(w & 0xFFF)
    # Literal pool load: fold the constant (code is read-only).
    if load and rn == 15 and not (w & (1 << 25)) and (w >> 24) & 1 and not (w >> 21) & 1:
        a = pc + (w & 0xFFF) if (w >> 23) & 1 else pc - (w & 0xFFF)
        if image.in_text(a, 4) and not byte:
            v = image.read32(a)
            insn.literal = a
            insn.asm = f"ldr r{rd}, ={v:#x}"
            if rd == 15:
                insn.kind, insn.expr = "jump", h(v)
            else:
                insn.code = f"c->r[{rd}] = {h(v)};"
            return insn
    addr_stmt, wb_stmt = address_mode(w, pc, off)
    width = "8" if byte else "32"
    insn.asm = f"{'ldr' if load else 'str'}{'b' if byte else ''} r{rd}, [r{rn}]"
    if load:
        return finish_load(image, insn, w, pc, rd, f"recomp::r{width}(a)", addr_stmt, wb_stmt)
    value = h(pc) if rd == 15 else f"c->r[{rd}]"  # ARM11 stores PC + 8
    cast = "(u8)" if byte else ""
    insn.code = f"{{ u32 v = {value}; {addr_stmt} recomp::w{width}(a, {cast}v); {wb_stmt} }}"
    return insn


def lift_ldst_extra(image, insn, w, pc):
    load, sh = (w >> 20) & 1, (w >> 5) & 3
    rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
    off = h(((w >> 4) & 0xF0) | (w & 0xF)) if (w >> 22) & 1 else reg(w & 0xF, pc)
    addr_stmt, wb_stmt = address_mode(w, pc, off)
    insn.asm = f"ldst-extra r{rd}, [r{rn}]"
    if load:
        expr = {1: "recomp::r16(a)", 2: "(u32)(s32)(s8)recomp::r8(a)", 3: "(u32)(s32)(s16)recomp::r16(a)"}[sh]
        return finish_load(image, insn, w, pc, rd, expr, addr_stmt, wb_stmt)
    if sh == 1:  # STRH
        insn.code = f"{{ u32 v = c->r[{rd}]; {addr_stmt} recomp::w16(a, (u16)v); {wb_stmt} }}"
    elif sh == 2:  # LDRD
        insn.code = (f"{{ {addr_stmt} u32 v0 = recomp::r32(a), v1 = recomp::r32(a + 4); {wb_stmt} "
                     f"c->r[{rd}] = v0; c->r[{rd + 1}] = v1; }}")
    else:  # STRD
        insn.code = (f"{{ u32 v0 = c->r[{rd}], v1 = c->r[{rd + 1}]; {addr_stmt} "
                     f"recomp::w32(a, v0); recomp::w32(a + 4, v1); {wb_stmt} }}")
    return insn


def lift_ldm(image, insn, w, pc):
    rn = (w >> 16) & 0xF
    p, u, wb, load = (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1, (w >> 20) & 1
    regs = [i for i in range(16) if w >> i & 1]
    n = len(regs)
    if n == 0:
        return trap(insn, "ldm/stm with empty list")
    start = {(0, 1): "b", (1, 1): "b + 4u", (0, 0): f"b - {4 * n - 4}u", (1, 0): f"b - {4 * n}u"}[(p, u)]
    new_base = f"b + {4 * n}u" if u else f"b - {4 * n}u"
    parts = [f"u32 b = c->r[{rn}]; u32 a = {start};"]
    insn.asm = f"{'ldm' if load else 'stm'} r{rn}{'!' if wb else ''}, {{{','.join(f'r{r}' for r in regs)}}}"
    if load:
        if wb:
            parts.append(f"c->r[{rn}] = {new_base};")
        for i, r in enumerate(regs):
            if r == 15:
                parts.append(f"u32 v = recomp::r32(a + {4 * i}u);")
            else:
                parts.append(f"c->r[{r}] = recomp::r32(a + {4 * i}u);")
        if 15 in regs:
            insn.kind = "ret" if rn == 13 and u and not p else "jump"
            insn.code, insn.expr = " ".join(parts), "v"
            return insn
        insn.code = "{ " + " ".join(parts) + " }"
        return insn
    for i, r in enumerate(regs):
        val = h(pc) if r == 15 else f"c->r[{r}]"  # ARM11 stores PC + 8
        parts.append(f"recomp::w32(a + {4 * i}u, {val});")
    if wb:
        parts.append(f"c->r[{rn}] = {new_base};")
    insn.code = "{ " + " ".join(parts) + " }"
    return insn


def lift_media(image, insn, w, pc):
    op1 = (w >> 20) & 0x1F
    op2 = (w >> 5) & 7
    rn, rd, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
    m, n = reg(rm, pc), reg(rn, pc)
    if op1 < 8:  # parallel add/subtract
        insn.code = f"c->r[{rd}] = recomp::parallel(c, {op1}, {op2}, {n}, {m});"
        insn.asm = "parallel add/sub"
        return insn
    if op1 == 8 and (op2 & 1) == 0:  # PKHBT / PKHTB
        tb = (w >> 6) & 1
        val, _ = shift_imm(2 if tb else 0, (w >> 7) & 0x1F, m)
        insn.code = (f"c->r[{rd}] = ({n} & 0xFFFF0000u) | ({val} & 0xFFFFu);" if tb
                     else f"c->r[{rd}] = ({n} & 0xFFFFu) | ({val} & 0xFFFF0000u);")
        insn.asm = "pkh"
        return insn
    if op1 == 8 and op2 == 5:  # SEL
        insn.code = f"c->r[{rd}] = recomp::sel(c, {n}, {m});"
        insn.asm = "sel"
        return insn
    if (op1 & 0x1A) == 0x0A and (op2 & 1) == 0:  # SSAT / USAT
        sat = (w >> 16) & 0x1F
        val, _ = shift_imm(2 if (w >> 6) & 1 else 0, (w >> 7) & 0x1F, m)
        fn = "usat" if (w >> 22) & 1 else "ssat"
        bits = sat if fn == "usat" else sat + 1
        insn.code = f"c->r[{rd}] = recomp::{fn}(c, {val}, {bits});"
        insn.asm = fn
        return insn
    if op2 == 1 and op1 in (0x0A, 0x0E) and rs == 0xF:  # SSAT16 / USAT16
        sat = (w >> 16) & 0xF
        fn = "usat16" if op1 == 0x0E else "ssat16"
        insn.code = f"c->r[{rd}] = recomp::{fn}(c, {m}, {sat if fn == 'usat16' else sat + 1});"
        insn.asm = fn
        return insn
    if op2 == 3 and op1 in (0x0A, 0x0B, 0x0C, 0x0E, 0x0F, 0x08):  # extend (+add)
        rot = ((w >> 10) & 3) * 8
        src = f"std::rotr({m}, {rot})" if rot else m
        kind = op1 & 7
        ext = {2: f"(u32)(s32)(s8){src}", 3: f"(u32)(s32)(s16){src}", 6: f"({src} & 0xFFu)",
               7: f"({src} & 0xFFFFu)", 0: f"recomp::xtb16({src}, true)", 4: f"recomp::xtb16({src}, false)"}.get(kind)
        if ext is None:
            return trap(insn, f"extend {w:08x}")
        if kind in (0, 4):
            insn.code = f"c->r[{rd}] = " + (f"recomp::add16({n}, {ext});" if rn != 15 else f"{ext};")
        else:
            insn.code = f"c->r[{rd}] = " + (f"{n} + {ext};" if rn != 15 else f"{ext};")
        insn.asm = "xt"
        return insn
    if op1 == 0x0B and op2 == 1:
        insn.code, insn.asm = f"c->r[{rd}] = __builtin_bswap32({m});", "rev"
        return insn
    if op1 == 0x0B and op2 == 5:
        insn.code, insn.asm = f"c->r[{rd}] = ((({m}) & 0x00FF00FFu) << 8) | ((({m}) >> 8) & 0x00FF00FFu);", "rev16"
        return insn
    if op1 == 0x0F and op2 == 5:
        insn.code, insn.asm = f"c->r[{rd}] = (u32)(s32)(s16)__builtin_bswap16((u16){m});", "revsh"
        return insn
    if op1 == 0x18 and op2 == 0 and rd == 15:  # USAD8 (rd field is Ra=15)
        insn.code = f"c->r[{rn}] = recomp::usad8({m}, {reg(rs, pc)});"
        insn.asm = "usad8"
        return insn
    if op1 in (0x10, 0x14, 0x15):  # SMLAD/SMUAD/SMLSD/SMUSD, SMLALD/SMLSLD, SMMLA/SMMUL/SMMLS
        rdd, ra = (w >> 16) & 0xF, (w >> 12) & 0xF
        swap = (w >> 5) & 1
        mm, ss = reg(rm, pc), reg(rs, pc)
        if op1 == 0x15:
            rnd = "0x80000000ll" if swap else "0"
            prod = f"(s64)(s32){mm} * (s64)(s32){ss}"
            if op2 >> 1 == 0:
                acc = f"((s64)(s32)c->r[{ra}] << 32) + " if ra != 15 else ""
                insn.code = f"c->r[{rdd}] = (u32)(({acc}{prod} + {rnd}) >> 32);"
            else:
                insn.code = f"c->r[{rdd}] = (u32)((((s64)(s32)c->r[{ra}] << 32) - {prod} + {rnd}) >> 32);"
            insn.asm = "smmul"
            return insn
        sub = (w >> 6) & 1
        insn.code = (f"recomp::dual_mul(c, {op1 == 0x14}, {bool(sub)}, {bool(swap)}, {rdd}, {ra}, {mm}, {ss});"
                     .replace("True", "true").replace("False", "false"))
        insn.asm = "smlad"
        return insn
    return trap(insn, f"media {w:08x}")


def lift_coproc(insn, w):
    cp = (w >> 8) & 0xF
    rt = (w >> 12) & 0xF
    crn, crm = (w >> 16) & 0xF, w & 0xF
    opc1, opc2 = (w >> 21) & 7, (w >> 5) & 7
    is_mrc = (w >> 20) & 1
    if cp == 15 and w & 0x10:
        if is_mrc and crn == 13 and crm == 0 and opc1 == 0 and opc2 == 3:
            insn.code, insn.asm = f"c->r[{rt}] = c->tls;", "mrc tls"
            return insn
        if not is_mrc and crn == 7 and crm in (5, 10) and opc1 == 0:
            insn.asm = "barrier"  # DMB/DSB/flush: single-threaded host, no-op
            return insn
    return trap(insn, f"coprocessor {w:08x}")
