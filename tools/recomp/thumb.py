# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Thumb-1 (ARMv6K, no Thumb-2) lifter: one halfword (or a BL/BLX pair) -> Insn."""
from arm import emit_dp, sext
from common import Insn, h, reg, shift_imm, trap

# Format 4 ALU op -> (ARM data-processing opcode or special)
ALU = {0: 0, 1: 1, 5: 5, 6: 6, 8: 8, 10: 10, 11: 11, 12: 12, 14: 14, 15: 15}


def lo(r):
    return f"c->r[{r}]"


def lift(image, addr, hw):
    insn = Insn(addr, 2, True)
    pc = addr + 4
    top5 = hw >> 11

    if top5 < 3:  # LSL/LSR/ASR imm
        rd, rm, imm = hw & 7, (hw >> 3) & 7, (hw >> 6) & 0x1F
        val, carry = shift_imm(top5, imm, "m")
        insn.code = emit_dp(insn, 13, 1, rd, "", f"u32 m = {lo(rm)}; u32 b = {val};", carry)
        insn.asm = ["lsls", "lsrs", "asrs"][top5]
        return insn
    if top5 == 3:  # ADD/SUB reg or imm3
        rd, rn, x = hw & 7, (hw >> 3) & 7, (hw >> 6) & 7
        b = f"u32 b = {x}u;" if hw & 0x400 else f"u32 b = {lo(x)};"
        insn.code = emit_dp(insn, 2 if hw & 0x200 else 4, 1, rd, lo(rn), b, "c->c")
        insn.asm = "subs" if hw & 0x200 else "adds"
        return insn
    if top5 < 8:  # MOV/CMP/ADD/SUB imm8
        op, rd, imm = (hw >> 11) & 3, (hw >> 8) & 7, hw & 0xFF
        arm_op = [13, 10, 4, 2][op]
        insn.code = emit_dp(insn, arm_op, 1, rd, lo(rd), f"u32 b = {imm}u;", "c->c")
        insn.asm = ["movs", "cmp", "adds", "subs"][op] + f" r{rd}, #{imm}"
        return insn
    if (hw >> 10) == 0x10:  # format 4 ALU
        op, rm, rd = (hw >> 6) & 0xF, (hw >> 3) & 7, hw & 7
        insn.asm = f"alu{op}"
        if op in ALU:
            insn.code = emit_dp(insn, ALU[op], 1, rd, lo(rd), f"u32 b = {lo(rm)};", "c->c")
        elif op in (2, 3, 4, 7):  # LSL/LSR/ASR/ROR by register
            typ = {2: 0, 3: 1, 4: 2, 7: 3}[op]
            insn.code = emit_dp(insn, 13, 1, rd, "",
                                f"u32 sc; u32 b = recomp::shift_reg({typ}, {lo(rd)}, {lo(rm)} & 0xFFu, c->c, &sc);", "sc")
        elif op == 9:  # NEG = RSBS rd, rm, #0
            insn.code = emit_dp(insn, 3, 1, rd, lo(rm), "u32 b = 0u;", "c->c")
        else:  # MUL
            insn.code = f"{{ u32 r = {lo(rd)} * {lo(rm)}; c->r[{rd}] = r; c->n = r >> 31; c->z = r == 0; }}"
        return insn
    if (hw >> 10) == 0x11:  # hi-register ops / BX / BLX
        op = (hw >> 8) & 3
        rd = ((hw >> 4) & 8) | (hw & 7)
        rm = (hw >> 3) & 0xF
        if op == 3:
            if hw & 0x80:
                insn.kind, insn.expr, insn.link, insn.asm = "icall", reg(rm, pc), (addr + 2) | 1, f"blx r{rm}"
            elif rm == 14:
                insn.kind, insn.expr, insn.asm = "ret", "c->r[14]", "bx lr"
            elif rm == 15:  # bx pc: switch to ARM at the next word
                insn.kind, insn.target, insn.asm = "branch", (pc & ~3), "bx pc"
            else:
                insn.kind, insn.expr, insn.asm = "jump", reg(rm, pc), f"bx r{rm}"
            return insn
        if op == 1:
            insn.code = emit_dp(insn, 10, 1, rd, reg(rd, pc), f"u32 b = {reg(rm, pc)};", "c->c")
            insn.asm = "cmp hi"
            return insn
        value = f"{reg(rd, pc)} + {reg(rm, pc)}" if op == 0 else reg(rm, pc)
        insn.asm = "add hi" if op == 0 else f"mov r{rd}, r{rm}"
        if rd == 15:
            if op == 2 and rm == 14:
                insn.kind, insn.expr = "ret", "c->r[14]"
            else:
                insn.kind, insn.expr = "jump", f"(({value}) | 1u)"
            return insn
        insn.code = f"c->r[{rd}] = {value};"
        return insn
    if top5 == 9:  # LDR literal
        rd = (hw >> 8) & 7
        a = (pc & ~3) + (hw & 0xFF) * 4
        if image.in_text(a, 4):
            v = image.read32(a)
            insn.literal = a
            insn.code, insn.asm = f"c->r[{rd}] = {h(v)};", f"ldr r{rd}, ={v:#x}"
        else:
            insn.code, insn.asm = f"c->r[{rd}] = recomp::r32({h(a)});", f"ldr r{rd}, [pc]"
        return insn
    if (hw >> 12) == 5:  # load/store register offset
        op, rm, rn, rd = (hw >> 9) & 7, (hw >> 6) & 7, (hw >> 3) & 7, hw & 7
        a = f"u32 a = {lo(rn)} + {lo(rm)};"
        insn.asm = ["str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh"][op]
        insn.code = "{ " + a + " " + {
            0: f"recomp::w32(a, {lo(rd)});", 1: f"recomp::w16(a, (u16){lo(rd)});",
            2: f"recomp::w8(a, (u8){lo(rd)});", 3: f"c->r[{rd}] = (u32)(s32)(s8)recomp::r8(a);",
            4: f"c->r[{rd}] = recomp::r32(a);", 5: f"c->r[{rd}] = recomp::r16(a);",
            6: f"c->r[{rd}] = recomp::r8(a);", 7: f"c->r[{rd}] = (u32)(s32)(s16)recomp::r16(a);"}[op] + " }"
        return insn
    if (hw >> 13) == 3 or (hw >> 12) == 8:  # LDR/STR (B) imm5, LDRH/STRH imm5
        load = (hw >> 11) & 1
        imm, rn, rd = (hw >> 6) & 0x1F, (hw >> 3) & 7, hw & 7
        if (hw >> 12) == 8:
            width, scale = "16", 2
        else:
            width, scale = ("8", 1) if hw & 0x1000 else ("32", 4)
        a = f"u32 a = {lo(rn)} + {imm * scale}u;"
        acc = f"c->r[{rd}] = recomp::r{width}(a);" if load else f"recomp::w{width}(a, (u{width}){lo(rd)});"
        insn.code, insn.asm = "{ " + a + " " + acc + " }", f"{'ldr' if load else 'str'}{width}"
        return insn
    if (hw >> 12) == 9:  # SP-relative
        load, rd, imm = (hw >> 11) & 1, (hw >> 8) & 7, (hw & 0xFF) * 4
        a = f"u32 a = c->r[13] + {imm}u;"
        acc = f"c->r[{rd}] = recomp::r32(a);" if load else f"recomp::w32(a, {lo(rd)});"
        insn.code, insn.asm = "{ " + a + " " + acc + " }", "ldr/str sp"
        return insn
    if (hw >> 12) == 0xA:  # ADD rd, pc/sp, imm
        rd, imm = (hw >> 8) & 7, (hw & 0xFF) * 4
        insn.code = f"c->r[{rd}] = c->r[13] + {imm}u;" if hw & 0x800 else f"c->r[{rd}] = {h((pc & ~3) + imm)};"
        insn.asm = "add sp/pc"
        return insn
    if (hw >> 12) == 0xB:
        return lift_misc(insn, hw, pc)
    if (hw >> 12) == 0xC:  # LDMIA/STMIA rn!
        load, rn = (hw >> 11) & 1, (hw >> 8) & 7
        regs = [i for i in range(8) if hw >> i & 1]
        if not regs:
            return trap(insn, "ldm/stm empty list")
        parts = [f"u32 a = c->r[{rn}];"]
        for i, r in enumerate(regs):
            parts.append(f"c->r[{r}] = recomp::r32(a + {4 * i}u);" if load else f"recomp::w32(a + {4 * i}u, c->r[{r}]);")
        if not (load and rn in regs):
            parts.insert(1, f"c->r[{rn}] = a + {4 * len(regs)}u;") if load else parts.append(f"c->r[{rn}] = a + {4 * len(regs)}u;")
        insn.code, insn.asm = "{ " + " ".join(parts) + " }", "ldmia/stmia"
        return insn
    if (hw >> 12) == 0xD:  # conditional branch / SVC
        cond = (hw >> 8) & 0xF
        if cond == 0xF:
            insn.code, insn.asm = f"recomp::svc(c, {hw & 0xFF});", f"svc {hw & 0xFF:#x}"
            return insn
        if cond == 0xE:
            return trap(insn, f"udf {hw:04x}")
        target = (pc + (sext(hw & 0xFF, 8) << 1)) & 0xFFFFFFFF
        insn.cond, insn.kind, insn.target, insn.asm = cond, "branch", target | 1, f"b{cond} {target:#x}"
        return insn
    if top5 == 0x1C:  # B
        target = (pc + (sext(hw & 0x7FF, 11) << 1)) & 0xFFFFFFFF
        insn.kind, insn.target, insn.asm = "branch", target | 1, f"b {target:#x}"
        return insn
    if top5 == 0x1E:  # BL/BLX prefix: pair with the next halfword
        hw2 = image.read16(addr + 2)
        if hw2 >> 11 not in (0x1F, 0x1D):
            return trap(insn, f"lone bl prefix {hw:04x}")
        target = (pc + (sext(hw & 0x7FF, 11) << 12) + ((hw2 & 0x7FF) << 1)) & 0xFFFFFFFF
        insn.size, insn.kind, insn.link = 4, "call", (addr + 4) | 1
        if hw2 >> 11 == 0x1F:
            insn.target, insn.asm = target | 1, f"bl {target:#x}"
        else:
            insn.target, insn.asm = target & ~3, f"blx {target & ~3:#x}"
        return insn
    return trap(insn, f"thumb {hw:04x}")


def lift_misc(insn, hw, pc):
    sub = (hw >> 8) & 0xF
    if sub == 0:  # ADD/SUB sp, #imm7*4
        imm = (hw & 0x7F) * 4
        insn.code, insn.asm = f"c->r[13] {'-' if hw & 0x80 else '+'}= {imm}u;", "add/sub sp"
        return insn
    if sub == 2:  # SXTH SXTB UXTH UXTB
        op, rm, rd = (hw >> 6) & 3, (hw >> 3) & 7, hw & 7
        m = lo(rm)
        insn.code = f"c->r[{rd}] = " + [f"(u32)(s32)(s16){m};", f"(u32)(s32)(s8){m};",
                                         f"{m} & 0xFFFFu;", f"{m} & 0xFFu;"][op]
        insn.asm = ["sxth", "sxtb", "uxth", "uxtb"][op]
        return insn
    if sub in (4, 5):  # PUSH {list[, lr]}
        regs = [i for i in range(8) if hw >> i & 1] + ([14] if hw & 0x100 else [])
        n = len(regs)
        parts = [f"u32 a = c->r[13] - {4 * n}u;"]
        parts += [f"recomp::w32(a + {4 * i}u, c->r[{r}]);" for i, r in enumerate(regs)]
        parts.append("c->r[13] = a;")
        insn.code, insn.asm = "{ " + " ".join(parts) + " }", "push"
        return insn
    if sub in (0xC, 0xD):  # POP {list[, pc]}
        regs = [i for i in range(8) if hw >> i & 1]
        has_pc = bool(hw & 0x100)
        n = len(regs) + has_pc
        parts = ["u32 a = c->r[13];"]
        parts += [f"c->r[{r}] = recomp::r32(a + {4 * i}u);" for i, r in enumerate(regs)]
        parts.append(f"c->r[13] = a + {4 * n}u;")
        insn.asm = "pop"
        if has_pc:
            parts.append(f"u32 v = recomp::r32(a + {4 * len(regs)}u);")
            insn.kind, insn.code, insn.expr = "ret", " ".join(parts), "v"
            return insn
        insn.code = "{ " + " ".join(parts) + " }"
        return insn
    if sub == 0xA:
        op, rm, rd = (hw >> 6) & 3, (hw >> 3) & 7, hw & 7
        m = lo(rm)
        code = {0: f"__builtin_bswap32({m})", 1: f"((({m}) & 0x00FF00FFu) << 8) | ((({m}) >> 8) & 0x00FF00FFu)",
                3: f"(u32)(s32)(s16)__builtin_bswap16((u16){m})"}.get(op)
        if code is None:
            return trap(insn, f"thumb rev {hw:04x}")
        insn.code, insn.asm = f"c->r[{rd}] = {code};", "rev"
        return insn
    if sub == 0xE:
        return trap(insn, f"bkpt {hw & 0xFF:#x}")
    return trap(insn, f"thumb misc {hw:04x}")
