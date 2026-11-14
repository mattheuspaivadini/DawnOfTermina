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

#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace gfx::vulkan {

enum class ShaderStage { Vertex, Fragment, Compute };

// GLSL 450 -> SPIR-V 1.3 for Vulkan 1.1. Returns empty on failure with `log` set.
std::vector<u32> compile_glsl(std::string_view source, ShaderStage stage, std::string* log);

}  // namespace gfx::vulkan
