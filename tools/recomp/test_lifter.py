#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Self-check of the ARM/Thumb lifters on hand-picked encodings (no game data needed)."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import arm  # noqa: E402
import thumb  # noqa: E402


class FakeImage:
    """Flat little-endian memory at 0x100000 for the lifters."""
    def __init__(self, words):
        self.mem = b"".join(struct.pack("<I", w) for w in words)

    def in_text(self, a, n=4):
        return 0x100000 <= a and a + n <= 0x100000 + len(self.mem)

    def read32(self, a):
        return struct.unpack_from("<I", self.mem, a - 0x100000)[0]

    def read16(self, a):
        return struct.unpack_from("<H", self.mem, a - 0x100000)[0]


img = FakeImage([0] * 16)

# B / BL / BLX imm targets
i = arm.lift(img, 0x100000, 0xEB000007)
assert (i.kind, i.target, i.link) == ("call", 0x100024, 0x100004), i
i = arm.lift(img, 0x103ED8, 0xFAFFF05A)
assert (i.kind, i.target) == ("call", 0x100049), hex(i.target)  # BLX to Thumb sets bit 0

# ADR producing a Thumb pointer (ARM->Thumb veneer)
i = arm.lift(img, 0x1001A0, 0xE28FC07F)
assert i.pointer == 0x100227 and i.kind == "normal"

# Returns
assert arm.lift(img, 0x100000, 0xE12FFF1E).kind == "ret"            # bx lr
assert arm.lift(img, 0x100000, 0xE8BD8010).kind == "ret"            # pop {r4, pc}
assert arm.lift(img, 0x100000, 0xE49DF004).kind == "ret"            # ldr pc, [sp], #4
assert arm.lift(img, 0x100000, 0xE1A0F00E).kind == "ret"            # mov pc, lr

# Exclusives and media
assert "excl = 1" in arm.lift(img, 0x100000, 0xE1940F9F).code      # ldrex r0, [r4]
assert "c->r[1] = 0" in arm.lift(img, 0x100000, 0xE1841F92).code   # strex r1, r2, [r4]
assert "usat16" in arm.lift(img, 0x100000, 0xE6E8AF3A).code
assert "__builtin_bswap32" in arm.lift(img, 0x100000, 0xE6BF0F31).code  # rev r0, r1

# Barrel shifter carry: movs r0, r1, lsr #32 takes bit 31 as carry
i = arm.lift(img, 0x100000, 0xE1B00021)
assert "(m >> 31)" in i.code and "c->c = sc" in i.code, i.code

# Literal pool folding
lit = FakeImage([0xE59F0000, 0, 0x12345678])  # ldr r0, [pc, #0] -> word at 0x100008
i = arm.lift(lit, 0x100000, 0xE59F0000)
assert i.code == "c->r[0] = 0x12345678u;" and i.literal == 0x100008

# Thumb: BL pair, push/pop, bx lr, conditional branch, ldr literal
pair = FakeImage([0xF8E7F1FF])  # halfwords f1ff, f8e7
i = thumb.lift(pair, 0x100000, 0xF1FF)
assert i.size == 4 and i.kind == "call" and i.target & 1
assert thumb.lift(img, 0x100000, 0xBD10).kind == "ret"              # pop {r4, pc}
assert thumb.lift(img, 0x100000, 0x4770).kind == "ret"              # bx lr
i = thumb.lift(img, 0x100000, 0xD072)                                # beq +0xe4
assert (i.cond, i.target) == (0, (0x100004 + 0xE4) | 1)
assert thumb.lift(img, 0x100000, 0x4718).kind == "jump"             # bx r3 (return via register)

print("test_lifter: OK")
