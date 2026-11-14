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

#pragma once

#include "core/types.h"

// PICA200 colour-buffer / framebuffer pixel formats: 0 RGBA8, 1 RGB8, 2 RGB565, 3 RGB5A1,
// 4 RGBA4 (as stored in memory), and the 8x8 Morton tiling of colour buffers.
namespace gfx::pica::pixel {

// 0 RGBA8, 1 RGB8, 2 RGB565, 3 RGB5A1, 4 RGBA4
inline u32 bytes_per_pixel(u32 fmt) { return fmt == 0 ? 4 : fmt == 1 ? 3 : 2; }

struct Rgba {
    u32 r, g, b, a;
};

inline Rgba decode(u32 fmt, const u8* p) {
    const u32 v = p[0] | p[1] << 8;
    switch (fmt) {
    case 0: return {p[3], p[2], p[1], p[0]};
    case 1: return {p[2], p[1], p[0], 255};
    case 2: return {(v >> 11) * 255 / 31, (v >> 5 & 63) * 255 / 63, (v & 31) * 255 / 31, 255};
    case 3: return {(v >> 11) * 255 / 31, (v >> 6 & 31) * 255 / 31, (v >> 1 & 31) * 255 / 31, (v & 1) * 255};
    default: return {(v >> 12) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17};
    }
}

inline void encode(u32 fmt, Rgba c, u8* p) {
    u32 v;
    switch (fmt) {
    case 0: p[0] = u8(c.a), p[1] = u8(c.b), p[2] = u8(c.g), p[3] = u8(c.r); return;
    case 1: p[0] = u8(c.b), p[1] = u8(c.g), p[2] = u8(c.r); return;
    case 2: v = (c.r * 31 / 255) << 11 | (c.g * 63 / 255) << 5 | c.b * 31 / 255; break;
    case 3: v = (c.r * 31 / 255) << 11 | (c.g * 31 / 255) << 6 | (c.b * 31 / 255) << 1 | (c.a > 127); break;
    default: v = (c.r / 17) << 12 | (c.g / 17) << 8 | (c.b / 17) << 4 | c.a / 17; break;
    }
    p[0] = u8(v), p[1] = u8(v >> 8);
}

inline u32 morton_offset(u32 x, u32 y, u32 bpp) {  // offset inside a row of 8x8 tiles
    const u32 i = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
    return (i + (x & ~7u) * 8) * bpp;
}

}  // namespace gfx::pica::pixel
