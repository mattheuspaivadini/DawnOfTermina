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

#include "gfx/pica/shbin.h"

#include <cstring>

namespace gfx::pica {

std::optional<Shbin> parse_shbin(std::span<const u8> d) {
    auto u32at = [&](size_t o) -> std::optional<u32> {
        if (o + 4 > d.size()) return std::nullopt;
        u32 v;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    };
    auto u16at = [&](size_t o) -> u16 { return u16(d[o] | d[o + 1] << 8); };
    if (d.size() < 8 || std::memcmp(d.data(), "DVLB", 4) != 0) return std::nullopt;

    const u32 n = *u32at(4);
    const size_t dvlp = 8 + size_t(n) * 4;
    if (dvlp + 0x18 > d.size() || std::memcmp(d.data() + dvlp, "DVLP", 4) != 0) return std::nullopt;
    const u32 blob_off = *u32at(dvlp + 0x8), blob_words = *u32at(dvlp + 0xC);
    const u32 desc_off = *u32at(dvlp + 0x10), desc_count = *u32at(dvlp + 0x14);
    if (dvlp + blob_off + size_t(blob_words) * 4 > d.size() || dvlp + desc_off + size_t(desc_count) * 8 > d.size())
        return std::nullopt;

    Shbin s;
    s.code.resize(blob_words);
    std::memcpy(s.code.data(), d.data() + dvlp + blob_off, size_t(blob_words) * 4);
    for (u32 i = 0; i < desc_count; ++i) s.swizzles.push_back(*u32at(dvlp + desc_off + i * 8));

    for (u32 i = 0; i < n; ++i) {
        const size_t e = *u32at(8 + i * 4);
        if (e + 0x40 > d.size() || std::memcmp(d.data() + e, "DVLE", 4) != 0) return std::nullopt;
        ShaderProgram p;
        p.type = ShaderProgram::Type(d[e + 6] & 1);
        p.main = *u32at(e + 0x8);
        p.end = *u32at(e + 0xC);
        const u32 out_off = *u32at(e + 0x28), out_count = *u32at(e + 0x2C);
        if (e + out_off + size_t(out_count) * 8 > d.size() || p.main >= blob_words) return std::nullopt;
        for (u32 k = 0; k < out_count; ++k) {
            const size_t o = e + out_off + k * 8;
            p.outputs.push_back({u16at(o), u16at(o + 2), u16at(o + 4)});
        }
        s.programs.push_back(std::move(p));
    }
    return s;
}

std::vector<u32> outmap_from_outputs(const ShaderProgram& prog) {
    std::vector<u32> map(7, 0x1F1F1F1F);  // 0x1F = unused component
    for (const auto& out : prog.outputs) {
        if (out.reg >= 7) continue;
        u32 first;
        switch (out.semantic) {
        case 0: first = 0x00; break;  // position
        case 1: first = 0x04; break;  // normal quaternion
        case 2: first = 0x08; break;  // colour
        case 3: first = 0x0C; break;  // texcoord0 uv
        case 4: first = 0x10; break;  // texcoord0 w
        case 5: first = 0x0E; break;  // texcoord1 uv
        case 6: first = 0x16; break;  // texcoord2 uv
        case 8: first = 0x12; break;  // view
        default: continue;
        }
        u32 next = first;
        for (u32 c = 0; c < 4; ++c)
            if (out.mask >> c & 1) {
                map[out.reg] = (map[out.reg] & ~(0xFFu << (c * 8))) | (next++ << (c * 8));
            }
    }
    return map;
}

}  // namespace gfx::pica
