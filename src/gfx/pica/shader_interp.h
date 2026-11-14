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

#include "gfx/pica/command_processor.h"

namespace gfx::pica {

// Runs a PICA200 vertex shader on the CPU: inputs v0..v15 -> outputs o0..o15.
// Same semantics as generate_vertex_shader (PICA multiply, CALL/IF/LOOP frames, relative
// uniform addressing); geometry-shader opcodes and LITP end the program.
void run_vertex_shader(const ShaderState& vs, u32 entry, const Vec4 in[16], Vec4 out[16]);

}  // namespace gfx::pica
