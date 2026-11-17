#version 450
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

// Pass-through vertex shader: PICA clip space -> Vulkan, fixed interface of shader_gen.h.

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec4 in_uv01;
layout(location = 3) in vec4 in_uv2w;
layout(location = 4) in vec4 in_quat;
layout(location = 5) in vec4 in_view;
layout(std140, set = 0, binding = 0) uniform PicaVsUniforms { vec4 c[96]; ivec4 i[4]; vec4 depth; uint b; } u;
layout(location = 0) out vec4 primary_color;
layout(location = 1) out vec2 texcoord0;
layout(location = 2) out vec2 texcoord1;
layout(location = 3) out vec2 texcoord2;
layout(location = 4) out float texcoord0_w;
layout(location = 5) out vec4 normquat;
layout(location = 6) out vec3 view;
void main() {
    // PICA window depth = z/w * scale + offset; pre-multiplied by w for Vulkan. depth.z: widescreen.
    gl_Position = vec4(in_pos.x, in_pos.y * u.depth.z, in_pos.z * u.depth.x + in_pos.w * u.depth.y, in_pos.w);
    primary_color = min(abs(in_color), vec4(1.0));
    texcoord0 = in_uv01.xy;
    texcoord1 = in_uv01.zw;
    texcoord2 = in_uv2w.xy;
    texcoord0_w = in_uv2w.z;
    normquat = in_quat;
    view = in_view.xyz;
}
