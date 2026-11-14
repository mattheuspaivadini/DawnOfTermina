#include "gfx/pica/texture.h"

#include <algorithm>
#include <cstring>

namespace gfx::pica {

static u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }
static u32 expand5(u32 c) { return c << 3 | c >> 2; }
static u32 expand6(u32 c) { return c << 2 | c >> 4; }

u32 bits_per_texel(TexFormat fmt) {
    switch (fmt) {
    case TexFormat::RGBA8: return 32;
    case TexFormat::RGB8: return 24;
    case TexFormat::RGB5A1: case TexFormat::RGB565: case TexFormat::RGBA4:
    case TexFormat::IA8: case TexFormat::RG8: return 16;
    case TexFormat::I8: case TexFormat::A8: case TexFormat::IA4: case TexFormat::ETC1A4: return 8;
    case TexFormat::I4: case TexFormat::A4: case TexFormat::ETC1: return 4;
    }
    return 0;
}

size_t texture_size(TexFormat fmt, u32 width, u32 height) {
    return size_t(width) * height * bits_per_texel(fmt) / 8;
}

static u32 decode_texel(const u8* t, TexFormat fmt, u32 index) {
    auto u16le = [](const u8* p) { return u32(p[0] | p[1] << 8); };
    switch (fmt) {
    case TexFormat::RGBA8: return rgba(t[3], t[2], t[1], t[0]);
    case TexFormat::RGB8: return rgba(t[2], t[1], t[0], 255);
    case TexFormat::RGB5A1: {
        u32 v = u16le(t);
        return rgba(expand5(v >> 11), expand5(v >> 6 & 31), expand5(v >> 1 & 31), (v & 1) * 255);
    }
    case TexFormat::RGB565: {
        u32 v = u16le(t);
        return rgba(expand5(v >> 11), expand6(v >> 5 & 63), expand5(v & 31), 255);
    }
    case TexFormat::RGBA4: {
        u32 v = u16le(t);
        return rgba((v >> 12) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17);
    }
    case TexFormat::IA8: return rgba(t[1], t[1], t[1], t[0]);
    case TexFormat::RG8: return rgba(t[1], t[0], 0, 255);
    case TexFormat::I8: return rgba(t[0], t[0], t[0], 255);
    case TexFormat::A8: return rgba(0, 0, 0, t[0]);
    case TexFormat::IA4: {
        u32 i = (t[0] >> 4) * 17;
        return rgba(i, i, i, (t[0] & 15) * 17);
    }
    case TexFormat::I4: case TexFormat::A4: {
        u32 v = (index & 1 ? t[0] >> 4 : t[0] & 15) * 17;
        return fmt == TexFormat::I4 ? rgba(v, v, v, 255) : rgba(0, 0, 0, v);
    }
    default: return 0;
    }
}

// One 4x4 ETC1 block. 3DS stores the standard 64-bit block as a little-endian u64.
static void decode_etc1_block(u64 block, u64 alpha, bool has_alpha, u32* out, u32 stride) {
    static constexpr int MOD[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42},
                                      {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    const u32 hi = u32(block >> 32), lo = u32(block);
    const bool flip = hi & 1, diff = hi & 2;
    const u32 table[2] = {hi >> 5 & 7, hi >> 2 & 7};
    int base[2][3];
    for (int c = 0; c < 3; ++c) {
        const int shift = 27 - c * 8;  // R, G, B fields
        if (diff) {
            int c1 = hi >> shift & 31;
            int d = int(hi >> (shift - 3) & 7);
            d = d >= 4 ? d - 8 : d;
            base[0][c] = int(expand5(c1));
            base[1][c] = int(expand5((c1 + d) & 31));
        } else {
            base[0][c] = int(hi >> (shift + 1) & 15) * 17;
            base[1][c] = int(hi >> (shift - 3) & 15) * 17;
        }
    }
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) {
            const u32 i = x * 4 + y;
            const u32 sub = flip ? y >= 2 : x >= 2;
            const u32 msb = lo >> (16 + i) & 1, lsb = lo >> i & 1;
            const int m = MOD[table[sub]][lsb] * (msb ? -1 : 1);  // index msb = sign, lsb = magnitude
            int c[3];
            for (int k = 0; k < 3; ++k) c[k] = std::clamp(base[sub][k] + m, 0, 255);
            const u32 a = has_alpha ? u32(alpha >> (i * 4) & 15) * 17 : 255;
            out[y * stride + x] = rgba(u32(c[0]), u32(c[1]), u32(c[2]), a);
        }
}

std::vector<u32> decode_texture(std::span<const u8> src, u32 width, u32 height, TexFormat fmt) {
    const u32 bpp = bits_per_texel(fmt);
    if (!bpp || width % 8 || height % 8 || src.size() < texture_size(fmt, width, height)) return {};
    std::vector<u32> out(size_t(width) * height);
    const u8* p = src.data();

    for (u32 ty = 0; ty < height; ty += 8)
        for (u32 tx = 0; tx < width; tx += 8) {
            if (fmt == TexFormat::ETC1 || fmt == TexFormat::ETC1A4) {
                // An 8x8 tile holds four 4x4 blocks in Z order.
                const bool a4 = fmt == TexFormat::ETC1A4;
                for (u32 b = 0; b < 4; ++b) {
                    u64 alpha = 0, block;
                    if (a4) std::memcpy(&alpha, p, 8), p += 8;
                    std::memcpy(&block, p, 8), p += 8;
                    u32* dst = &out[size_t(ty + (b >> 1) * 4) * width + tx + (b & 1) * 4];
                    decode_etc1_block(block, alpha, a4, dst, width);
                }
                continue;
            }
            for (u32 i = 0; i < 64; ++i) {  // i = Morton index: x bits 0,2,4; y bits 1,3,5
                const u32 x = (i & 1) | (i >> 1 & 2) | (i >> 2 & 4);
                const u32 y = (i >> 1 & 1) | (i >> 2 & 2) | (i >> 3 & 4);
                const u8* t = p + (bpp >= 8 ? i * (bpp / 8) : i / 2);
                out[size_t(ty + y) * width + tx + x] = decode_texel(t, fmt, i);
            }
            p += 64 * bpp / 8;
        }
    return out;
}

}  // namespace gfx::pica
