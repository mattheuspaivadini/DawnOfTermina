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
#include <span>
#include <string>

#include "core/types.h"
#include "gfx/pica/command_processor.h"

// PICA200 -> GLSL 450 (Vulkan) shader generation.
//
// Varying interface shared by both stages (fixed locations):
//   0 vec4 primary_color, 1 vec2 texcoord0, 2 vec2 texcoord1, 3 vec2 texcoord2,
//   4 float texcoord0_w, 5 vec4 normquat, 6 vec3 view
// VS uniforms: set 0 binding 0, std140 { vec4 c[96]; ivec4 i[4]; vec4 depth (scale, offset); uint b; }
// FS textures: set 1 bindings 0..2 (sampler2D tex0..tex2).
// FS push constants (values that change per draw, kept out of the shader so they do not
// create variants): { uint tev_const[6]; uint tev_buffer_color; uint alpha_ref; uint fog_color; },
// the colours as raw RGBA8 registers (see fragment_push_constants).
// FS lighting and fog (declared only when enabled): set 0 binding 1, std430 storage buffer
//   { uint regs[0x90] (registers 0x140..0x1C0); uint lut[24 * 256] (ShaderState::light_lut);
//     uint fog_lut[128] (ShaderState::fog_lut) }.
namespace gfx::pica {

struct VsSource {
    std::span<const u32> code;      // program words (VS_PROGRAM_DATA)
    std::span<const u32> swizzles;  // operand descriptors (VS_SWIZZLE_DATA)
    u32 entry;                      // main offset in words
    std::span<const u32> outmap;    // VS_OUTMAP registers 0x50..0x56 (7 values)
    u16 bools = 0;                  // b0..b15 and i0..i3 at draw time: baked into the shader
    std::array<u32, 4> ints{};
};

// Returns GLSL specialised for `bools`/`ints` as structured code (no dispatcher loop, which
// Mesa takes minutes to compile), or an empty string with `error` set for programs it cannot
// structure (backward jumps, BREAK outside a loop, geometry-only opcodes, LITP); the caller
// then runs the program on the CPU (shader_interp.h).
std::string generate_vertex_shader(const VsSource& src, std::string* error);

// Fragment shader for the TEV combiner / alpha-test state in the register file.
// shortcut: gas (fog mode 7), procedural textures, shadow maps and non-2D texture types are
// not generated yet; add when a scene needs them.
std::string generate_fragment_shader(const std::array<u32, reg::COUNT>& regs);
constexpr u32 FRAGMENT_PUSH_WORDS = 9;
std::array<u32, FRAGMENT_PUSH_WORDS> fragment_push_constants(const std::array<u32, reg::COUNT>& regs);

}  // namespace gfx::pica
