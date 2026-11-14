# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Shared pieces of the ARM11 -> C++ static recompiler: instruction record, condition
codes, register access and the barrel shifter."""
from dataclasses import dataclass, field

COND = {
    0: "c->z", 1: "!c->z", 2: "c->c", 3: "!c->c", 4: "c->n", 5: "!c->n", 6: "c->v", 7: "!c->v",
    8: "(c->c && !c->z)", 9: "(!c->c || c->z)", 10: "(c->n == c->v)", 11: "(c->n != c->v)",
    12: "(!c->z && c->n == c->v)", 13: "(c->z || c->n != c->v)",
}
AL = 14


@dataclass
class Insn:
    addr: int
    size: int
    thumb: bool
    cond: int = AL
    # normal | branch | call | icall | ret | jump | trap
    kind: str = "normal"
    code: str = ""             # C++ statements; for ret/jump/icall, statements run before `expr`
    expr: str = ""             # ret: return address, jump: new PC, icall: call target
    target: int | None = None  # branch target, or call target key (address | thumb bit)
    link: int | None = None    # return address written to LR by calls (with thumb bit)
    literal: int | None = None # address of a literal-pool word (function-pointer discovery)
    pointer: int | None = None # code address computed by ADR (add/sub rd, pc, #imm)
    extra_targets: list = field(default_factory=list)  # jump-table targets inside the function
    switch_cmp: tuple | None = None  # (reg, max) when this is "cmp rX, #imm" (ARM switch idiom)
    asm: str = ""              # short description for the generated comment

    @property
    def next(self):
        return self.addr + self.size


def h(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


def reg(n, pc_value):
    """Read register n; r15 reads as the architectural PC value."""
    return h(pc_value) if n == 15 else f"c->r[{n}]"


def ror32(v, r):
    r &= 31
    return ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF if r else v


def shift_imm(typ, amount, rm_expr):
    """Immediate-shift operand. Returns (value_expr, carry_expr)."""
    if typ == 0:  # LSL
        if amount == 0:
            return rm_expr, "c->c"
        return f"({rm_expr} << {amount})", f"(({rm_expr} >> {32 - amount}) & 1u)"
    if typ == 1:  # LSR, 0 encodes 32
        if amount == 0:
            return "0u", f"({rm_expr} >> 31)"
        return f"({rm_expr} >> {amount})", f"(({rm_expr} >> {amount - 1}) & 1u)"
    if typ == 2:  # ASR, 0 encodes 32
        if amount == 0:
            return f"(u32)((s32){rm_expr} >> 31)", f"({rm_expr} >> 31)"
        return f"(u32)((s32){rm_expr} >> {amount})", f"(({rm_expr} >> {amount - 1}) & 1u)"
    if amount == 0:  # RRX
        return f"((c->c << 31) | ({rm_expr} >> 1))", f"({rm_expr} & 1u)"
    return f"std::rotr({rm_expr}, {amount})", f"(({rm_expr} >> {amount - 1}) & 1u)"


SHIFT_NAMES = ["lsl", "lsr", "asr", "ror"]


def trap(insn, what):
    insn.kind = "trap"
    insn.code = f'recomp::unimplemented(c, {h(insn.addr)}, "{what}");'
    insn.asm = what
    return insn
