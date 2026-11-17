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

#include <array>
#include <vector>

#include "core/memory.h"
#include "gfx/pica/command_processor.h"

namespace gfx::pica {

// One vertex as the vertex shader sees it: input registers v0..v15.
struct VertexInput {
    Vec4 v[16];
};

// Fetches the vertices of the current draw (DRAW_ARRAYS / DRAW_ELEMENTS) from guest memory
// following the attribute-buffer registers. `indices` empty: draw `out` in order (one vertex per
// index for indexed draws). Else `out` is the referenced vertex range and `indices` point into it.
// Returns false (with no vertices) for configurations not supported yet.
bool load_vertices(const core::Memory& mem, const std::array<u32, reg::COUNT>& regs, const ShaderState& vs,
                   bool indexed, std::vector<VertexInput>& out, std::vector<u16>& indices);

}  // namespace gfx::pica
