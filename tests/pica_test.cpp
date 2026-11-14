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
#include <cstring>
#include <vector>

#include "gfx/pica/command_processor.h"
#include "gfx/pica/texture.h"

using namespace gfx::pica;

static u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }

static void test_tiling() {
    // RGBA8 is stored A,B,G,R; put the Morton index in R so we can see where each texel lands.
    std::vector<u8> src(16 * 8 * 4);
    for (u32 i = 0; i < 128; ++i) src[i * 4 + 0] = 255, src[i * 4 + 3] = u8(i);
    auto out = decode_texture(src, 16, 8, TexFormat::RGBA8);
    assert(out.size() == 128);
    assert(out[0] == rgba(0, 0, 0, 255));
    assert(out[1] == rgba(1, 0, 0, 255));       // (1,0) -> index 1
    assert(out[16] == rgba(2, 0, 0, 255));      // (0,1) -> index 2
    assert(out[2] == rgba(4, 0, 0, 255));       // (2,0) -> index 4
    assert(out[7 * 16 + 7] == rgba(63, 0, 0, 255));
    assert(out[8] == rgba(64, 0, 0, 255));      // second tile starts at x = 8
    assert(decode_texture(src, 12, 8, TexFormat::RGBA8).empty());  // not a multiple of 8
}

static void test_formats() {
    std::vector<u8> t(8 * 8 * 2, 0);
    t[0] = 0x00, t[1] = 0xF8;  // RGB565 pure red
    assert(decode_texture(t, 8, 8, TexFormat::RGB565)[0] == rgba(255, 0, 0, 255));
    t[0] = 0x3F, t[1] = 0x00;  // RGB5A1: blue = 31, alpha = 1
    assert(decode_texture(t, 8, 8, TexFormat::RGB5A1)[0] == rgba(0, 0, 255, 255));
    t[0] = 0x80, t[1] = 0x40;  // IA8: alpha byte first, then intensity
    assert(decode_texture(t, 8, 8, TexFormat::IA8)[0] == rgba(0x40, 0x40, 0x40, 0x80));
    t[0] = 0x21;  // I4: low nibble is the even texel
    auto i4 = decode_texture(t, 8, 8, TexFormat::I4);
    assert(i4[0] == rgba(17, 17, 17, 255) && i4[1] == rgba(34, 34, 34, 255));
}

static void test_etc1() {
    std::vector<u8> src(32, 0);  // one 8x8 tile = four 4x4 blocks
    // Block 0: individual mode, all base colours 8*17 = 136, table 0, indices 0 -> +2.
    // Texel (0,0) uses index 3 (msb + lsb) -> -8.
    u64 b0 = u64(0x88888800) << 32 | (1u << 16 | 1u);
    // Block 1 (x = 4): differential mode, base 31 -> 255, +2 clamps at 255.
    u64 b1 = u64(31u << 27 | 31u << 19 | 31u << 11 | 2u) << 32;
    std::memcpy(&src[0], &b0, 8);
    std::memcpy(&src[8], &b1, 8);
    auto out = decode_texture(src, 8, 8, TexFormat::ETC1);
    assert(out[0] == rgba(128, 128, 128, 255));
    assert(out[1] == rgba(138, 138, 138, 255));
    assert(out[4] == rgba(255, 255, 255, 255));

    std::vector<u8> a4(64, 0);
    u64 alpha = 0x5;  // texel (0,0) alpha nibble = 5
    std::memcpy(&a4[0], &alpha, 8);
    std::memcpy(&a4[8], &b0, 8);
    assert(decode_texture(a4, 8, 8, TexFormat::ETC1A4)[0] == rgba(128, 128, 128, 85));
}

static u32 hdr(u32 id, u32 mask, u32 extra, bool consecutive = false) {
    return id | mask << 16 | extra << 20 | u32(consecutive) << 31;
}

static void test_command_processor() {
    assert(f24_to_f32(0x3F0000) == 1.0f && f24_to_f32(0xC00000) == -2.0f && f24_to_f32(0) == 0.0f);

    CommandProcessor cp;
    u32 draws = 0;
    cp.on_draw = [&](u32 id) { draws += id == reg::DRAW_ARRAYS; };

    const u32 one = 0x3F800000, two = 0x40000000, three = 0x40400000, four = 0x40800000;
    // f24 uniform packed w,z,y,x: x = 1.0, y = -2.0, z = 0, w = 1.0
    const u32 fx = 0x3F0000, fy = 0xC00000, fz = 0, fw = 0x3F0000;
    std::vector<u32> list = {
        0x11223344, hdr(0x100, 0xF, 0),
        0xAABBCCDD, hdr(0x100, 0x1, 0),                          // low byte only
        1, hdr(0x200, 0xF, 2, true), 2, 3,                       // consecutive 0x200..0x202
        0x80000005, hdr(reg::VS_FLOAT_UNIFORM_SETUP, 0xF, 0),    // f32 uniform 5
        four, hdr(reg::VS_FLOAT_UNIFORM_DATA, 0xF, 3), three, two, one, 0,  // w,z,y,x + pad
        0, hdr(reg::VS_FLOAT_UNIFORM_SETUP, 0xF, 0),             // f24 uniform 0
        fw << 8 | fz >> 16, hdr(reg::VS_FLOAT_UNIFORM_DATA, 0xF, 2),
        (fz & 0xFFFF) << 16 | fy >> 8, (fy & 0xFF) << 24 | fx,
        0, hdr(reg::VS_PROGRAM_OFFSET, 0xF, 0),
        0xAAAA, hdr(reg::VS_PROGRAM_DATA, 0xF, 1), 0xBBBB, 0,    // two words, padded
        1, hdr(reg::DRAW_ARRAYS, 0xF, 0),
    };
    assert(cp.process(list));
    assert(cp.regs()[0x100] == 0x112233DD);
    assert(cp.regs()[0x200] == 1 && cp.regs()[0x201] == 2 && cp.regs()[0x202] == 3);
    const Vec4 u5 = cp.vs().float_uniforms[5];
    assert(u5.x == 1.0f && u5.y == 2.0f && u5.z == 3.0f && u5.w == 4.0f);
    const Vec4 u0 = cp.vs().float_uniforms[0];
    assert(u0.x == 1.0f && u0.y == -2.0f && u0.z == 0.0f && u0.w == 1.0f);
    assert((cp.regs()[reg::VS_FLOAT_UNIFORM_SETUP] & 0xFF) == 1);  // index auto-incremented
    assert(cp.vs().code[0] == 0xAAAA && cp.vs().code[1] == 0xBBBB);
    assert(draws == 1);

    const u32 truncated[] = {0, hdr(0x100, 0xF, 4), 1};
    assert(!cp.process(truncated));
}

void test_pica() {
    test_tiling();
    test_formats();
    test_etc1();
    test_command_processor();
}
