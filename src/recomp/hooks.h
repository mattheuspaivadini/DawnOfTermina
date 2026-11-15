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

#include <atomic>

#include "recomp/runtime.h"

// Native replacements for hot C library routines of the game (listed with their VAs in
// tools/recomp/hooks.txt). The recompiler emits the hooked func_XXXXXXXX as a call to
// these, so direct calls, tail calls and the indirect-call table all reach them.
namespace recomp::hooks {

u32 aeabi_memcpy(Context* c);
u32 memcpy_src_dst(Context* c);
u32 aeabi_memcpy4(Context* c);
u32 aeabi_memmove4(Context* c);
u32 aeabi_memset(Context* c);
u32 aeabi_memclr(Context* c);
u32 aeabi_memclr4(Context* c);
u32 strlen(Context* c);
u32 strcmp(Context* c);
u32 sinf(Context* c);
u32 cosf(Context* c);
u32 atan2f(Context* c);

// Whether [va, va + bytes) is mapped guest memory (pointer chains followed by hooks).
bool readable(u32 va, u32 bytes);

// Instruction hooks (`insn` in hooks.txt): run in place of one instruction; those declared with
// jump targets return 0 (fall through) or the target.
// Camera FOV (port of DekUI's CameraLive): replaces `vldr s0, [r4, #0x18]` where the game builds a
// camera projection from its vertical FOV. 45° is the original gameplay FOV; a larger setting
// widens every camera by the same factor in projection space. Settings in degrees, 45..70
// (45 = original); cutscene_fov applies while the live top-screen camera is scripted.
extern std::atomic<float> fov, cutscene_fov;
void fov_projection(Context* c);

void pause_capture_sync(Context* c);

// DekUI single-screen inventory (src/recomp/dekui.cpp): Items/Masks/Gear on the top screen.
void dekui_capture_page(Context* c);
void dekui_capture_gear(Context* c);
void dekui_close_gear(Context* c);
void dekui_layout_pass(Context* c);
void dekui_layout_begin(Context* c);
void dekui_layout_end(Context* c);
u32 dekui_nav_vertical(Context* c);
u32 dekui_nav_horizontal(Context* c);
u32 dekui_nav_gear(Context* c);
bool dekui_inventory_routed();  // this frame's inventory page was moved to the top screen

}  // namespace recomp::hooks
