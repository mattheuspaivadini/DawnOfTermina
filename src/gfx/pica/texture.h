#pragma once

#include <span>
#include <vector>

#include "core/types.h"

namespace gfx::pica {

// PICA200 texture formats (register TEXUNITn_TYPE).
enum class TexFormat : u32 {
    RGBA8 = 0, RGB8 = 1, RGB5A1 = 2, RGB565 = 3, RGBA4 = 4, IA8 = 5, RG8 = 6,
    I8 = 7, A8 = 8, IA4 = 9, I4 = 10, A4 = 11, ETC1 = 12, ETC1A4 = 13,
};

// Bits per texel; 0 for an unknown format.
u32 bits_per_texel(TexFormat fmt);
size_t texture_size(TexFormat fmt, u32 width, u32 height);

// Decodes a tiled (8x8 Morton) PICA texture into linear RGBA8 (bytes R,G,B,A),
// rows in memory order: row 0 is the first tile row (the bottom of the image in
// PICA's GL-style convention; flip at sampling time). Width/height must be multiples
// of 8. Returns empty on bad size or unknown format.
std::vector<u32> decode_texture(std::span<const u8> src, u32 width, u32 height, TexFormat fmt);

}  // namespace gfx::pica
