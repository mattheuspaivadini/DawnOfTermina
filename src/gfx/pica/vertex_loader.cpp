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

#include "gfx/pica/vertex_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gfx::pica {

namespace {

constexpr u32 ATTRIB_BASE = 0x200, ATTRIB_FORMAT_LO = 0x201, ATTRIB_FORMAT_HI = 0x202, LOADERS = 0x203;
constexpr u32 INDEX_BUFFER = 0x227, VERTEX_COUNT = 0x228, FIRST_VERTEX = 0x22A;
constexpr u32 VS_ATTR_PERM_LO = 0x2BB, VS_ATTR_PERM_HI = 0x2BC;

u32 element_size(u32 type) { return type == 3 ? 4 : type == 2 ? 2 : 1; }  // float, s16, u8/s8

}  // namespace

bool load_vertices(const core::Memory& mem, const std::array<u32, reg::COUNT>& regs, const ShaderState& vs,
                   bool indexed, std::vector<VertexInput>& out) {
    out.clear();
    // Offsets are physical: the base is often the start of VRAM with offsets reaching FCRAM.
    const u32 base_pa = regs[ATTRIB_BASE] << 3;
    const u32 base = core::Memory::pa_to_va(base_pa);
    const u64 formats = u64(regs[ATTRIB_FORMAT_LO]) | u64(regs[ATTRIB_FORMAT_HI] & 0xFFFF) << 32;
    const u32 fixed_mask = regs[ATTRIB_FORMAT_HI] >> 16 & 0xFFF;
    const u32 attrib_count = (regs[ATTRIB_FORMAT_HI] >> 28) + 1;
    const u64 perm = u64(regs[VS_ATTR_PERM_LO]) | u64(regs[VS_ATTR_PERM_HI]) << 32;

    // Where each attribute lives: loader data offset + aligned position inside the vertex.
    u32 source[12] = {}, stride[12] = {};
    bool loaded[12] = {};
    for (u32 l = 0; l < 12; ++l) {
        const u32 offset = regs[LOADERS + l * 3];
        const u64 comps = u64(regs[LOADERS + l * 3 + 1]) | u64(regs[LOADERS + l * 3 + 2] & 0xFFFF) << 32;
        const u32 bytes = regs[LOADERS + l * 3 + 2] >> 16 & 0xFF;
        const u32 count = regs[LOADERS + l * 3 + 2] >> 28;
        u32 pos = 0;
        for (u32 k = 0; k < count; ++k) {
            const u32 a = comps >> (k * 4) & 0xF;
            if (a < 12) {
                const u32 esize = element_size(formats >> (a * 4) & 3);
                pos = (pos + esize - 1) & ~(esize - 1);
                source[a] = offset + pos, stride[a] = bytes, loaded[a] = true;
                pos += esize * ((formats >> (a * 4 + 2) & 3) + 1);
            } else {  // 12..15: 4..16 bytes of padding
                pos = (pos + 3) & ~3u;
                pos += (a - 11) * 4;
            }
        }
    }
    u32 source_va[12] = {};
    for (u32 a = 0; a < 12; ++a)
        if (loaded[a] && !(source_va[a] = core::Memory::pa_to_va(base_pa + source[a]))) loaded[a] = false;

    const u32 count = regs[VERTEX_COUNT];
    const u32 index_info = regs[INDEX_BUFFER];
    const u32 index_va = core::Memory::pa_to_va(base_pa + (index_info & 0x0FFFFFFF));
    if (indexed && !index_va) return false;
    const bool index16 = index_info >> 31;
    static const bool trace = std::getenv("DOT_TRACE_DRAWS") != nullptr;
    if (trace)
        std::fprintf(stderr, "draw: base %08X (va %08X) fmt %08X %08X index %08X count %u first %u indexed %d\n",
                     base_pa, base, regs[ATTRIB_FORMAT_LO], regs[ATTRIB_FORMAT_HI], index_info, count,
                     regs[FIRST_VERTEX], indexed);
    out.resize(count);
    for (u32 i = 0; i < count; ++i) {
        u32 vertex;
        if (indexed) vertex = index16 ? mem.read<u16>(index_va + i * 2) : mem.read<u8>(index_va + i);
        else vertex = regs[FIRST_VERTEX] + i;
        VertexInput& vin = out[i];
        for (auto& r : vin.v) r = {0, 0, 0, 1};
        for (u32 a = 0; a < attrib_count && a < 12; ++a) {
            const u32 reg = perm >> (a * 4) & 0xF;
            Vec4 value;
            if (fixed_mask >> a & 1 || !loaded[a]) {
                value = vs.fixed_attribs[a];
            } else {
                value = {0, 0, 0, 1};
                const u32 type = formats >> (a * 4) & 3, n = (formats >> (a * 4 + 2) & 3) + 1;
                const u32 at = source_va[a] + vertex * stride[a];
                float* dst = &value.x;
                for (u32 k = 0; k < n; ++k) {
                    switch (type) {
                    case 0: dst[k] = float(s8(mem.read<u8>(at + k))); break;
                    case 1: dst[k] = float(mem.read<u8>(at + k)); break;
                    case 2: dst[k] = float(s16(mem.read<u16>(at + k * 2))); break;
                    default: dst[k] = mem.read<float>(at + k * 4); break;
                    }
                }
            }
            vin.v[reg] = value;
        }
    }
    return true;
}

}  // namespace gfx::pica
