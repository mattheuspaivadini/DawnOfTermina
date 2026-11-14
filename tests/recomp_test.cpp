// DawnOfTermina — native static recompilation of Majora's Mask 3D.
// Copyright (C) 2026 DawnOfTermina contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#undef NDEBUG
#include <cassert>

#include "recomp/hooks.h"
#include "recomp/runtime.h"

using namespace recomp;

void test_recomp_runtime() {
    Context c{};
    // ADDS: unsigned carry and signed overflow
    assert(add_flags(&c, 0xFFFFFFFF, 1) == 0 && c.z && c.c && !c.v && !c.n);
    assert(add_flags(&c, 0x7FFFFFFF, 1) == 0x80000000 && c.n && c.v && !c.c);
    // SUBS / CMP: carry = no borrow
    assert(sub_flags(&c, 5, 3) == 2 && c.c && !c.v);
    assert(sub_flags(&c, 3, 5) == 0xFFFFFFFE && !c.c && c.n);
    assert(sub_flags(&c, 0x80000000, 1) == 0x7FFFFFFF && c.v);
    // ADC / SBC use the incoming carry
    c.c = 1;
    assert(adc_flags(&c, 1, 1) == 3);
    c.c = 0;
    assert(sbc_flags(&c, 5, 3) == 1 && c.c);  // 5 - 3 - 1

    // Register shifts, including amounts >= 32
    u32 co = 0;
    assert(shift_reg(0, 0x80000001, 1, 0, &co) == 2 && co == 1);
    assert(shift_reg(0, 1, 32, 0, &co) == 0 && co == 1);
    assert(shift_reg(1, 0x80000000, 32, 0, &co) == 0 && co == 1);
    assert(shift_reg(2, 0x80000000, 40, 0, &co) == 0xFFFFFFFF && co == 1);
    assert(shift_reg(3, 0x1, 1, 0, &co) == 0x80000000 && co == 1);
    assert(shift_reg(0, 7, 0, 1, &co) == 7 && co == 1);  // amount 0 keeps carry

    // SIMD: UADD8 sets GE per byte on carry, SEL picks by GE
    c = {};
    assert(parallel(&c, 5, 4, 0xFF010203, 0x01010101) == 0x00020304 && c.ge == 0x8);
    assert(sel(&c, 0xAABBCCDD, 0x11223344) == 0xAA223344);
    assert(parallel(&c, 6, 4, 0xFF010203, 0x01010101) == 0xFF020304);  // UQADD8 saturates
    assert(parallel(&c, 1, 3, 0x00010005, 0x00020003) == 0xFFFF0002);  // SSUB16

    // Saturation sets Q
    c = {};
    assert(ssat(&c, 200, 8) == 127 && c.q);
    assert(usat(&c, u32(-5), 8) == 0);
    assert(qaddsub(&c, 0, 0x7FFFFFFF, 1) == 0x7FFFFFFF);

    // VFP compare flags and float->int conversion
    c = {};
    vfp_cmp(&c, 1.0, 2.0);
    fmstat(&c);
    assert(c.n && !c.z && !c.c);
    vfp_cmp(&c, 2.0, 2.0);
    fmstat(&c);
    assert(c.z && c.c);
    assert(vfp_to_int(&c, -1.7, true, true) == u32(-1));
    assert(vfp_to_int(&c, 2.5, true, false) == 2);  // round to nearest even
    assert(vfp_to_int(&c, 1e20, false, true) == 0xFFFFFFFF);
    set_fd(&c, 3, 1.5);
    assert(fd(&c, 3) == 1.5 && c.s[7] == 0x3FF80000);

    // Native hooks (src/recomp/hooks.cpp): guest VA = offset into a host buffer here.
    u8* const saved_base = g_base;
    alignas(4) static u8 mem[64];
    set_memory(mem);
    c = {};
    c.r[14] = 0x00123457;
    std::memcpy(mem, "abcdefgh", 9);
    c.r[0] = 16, c.r[1] = 0, c.r[2] = 9;
    assert(hooks::aeabi_memcpy(&c) == 0x00123457 && std::strcmp((char*)mem + 16, "abcdefgh") == 0);
    c.r[0] = 32, c.r[1] = 3, c.r[2] = 0x1AB;  // __aeabi_memset(dst, n, c): value is the low byte of r2
    assert(hooks::aeabi_memset(&c) == 0x00123457 && mem[32] == 0xAB && mem[34] == 0xAB && mem[35] == 0);
    c.r[0] = 32, c.r[1] = 2;
    hooks::aeabi_memclr(&c);
    assert(mem[33] == 0 && mem[34] == 0xAB);
    c.r[0] = 16, c.r[1] = 0, c.r[2] = 4;  // swapped arguments (src, dst): copy 16 -> 0
    std::memcpy(mem + 16, "wxyz", 4);
    hooks::memcpy_src_dst(&c);
    assert(std::memcmp(mem, "wxyzefgh", 8) == 0);
    c.r[0] = 16;
    hooks::strlen(&c);
    assert(c.r[0] == 8);
    c.r[0] = 0, c.r[1] = 16;
    hooks::strcmp(&c);
    assert(c.r[0] == 0);
    mem[3] = 'a';
    c.r[0] = 0, c.r[1] = 16;
    hooks::strcmp(&c);
    assert(s32(c.r[0]) < 0);
    set_fs(&c, 0, 1.0f), set_fs(&c, 1, 1.0f);
    hooks::atan2f(&c);
    assert(std::fabs(fs(&c, 0) - 0.7853982f) < 1e-6f);
    set_fs(&c, 0, 0.0f);
    hooks::cosf(&c);
    assert(fs(&c, 0) == 1.0f);
    set_memory(saved_base);
}
