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

#include <optional>
#include <span>
#include <vector>

#include "core/types.h"

namespace gfx::pica {

// One program (DVLE) inside a SHBIN/DVLB container.
struct ShaderProgram {
    enum Type : u8 { VERTEX = 0, GEOMETRY = 1 } type;
    u32 main, end;  // word offsets into the shared code blob
    struct Output {
        u16 semantic;  // 0 pos, 1 normquat, 2 color, 3 texcoord0, 4 texcoord0w, 5 texcoord1, 6 texcoord2, 8 view
        u16 reg;       // o0..o15
        u16 mask;      // component mask, bit 0 = x
    };
    std::vector<Output> outputs;
};

// DVLB: code + operand descriptors shared by all programs (DVLP), plus the programs.
struct Shbin {
    std::vector<u32> code;
    std::vector<u32> swizzles;  // operand descriptors (first word of each 8-byte entry)
    std::vector<ShaderProgram> programs;
};

std::optional<Shbin> parse_shbin(std::span<const u8> data);

// VS_OUTMAP register values (0x50..0x56) implied by a program's output table.
std::vector<u32> outmap_from_outputs(const ShaderProgram& prog);

}  // namespace gfx::pica
