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
void play_context(Context* c);
u32 play();  // the Play object (GlobalContext) of the last play update, 0 before any (v1.0 layout builds)

// DekUI's single-screen interface (src/recomp/dekui.cpp): each hook replaces one instruction DekUI
// swaps for an SVC and does what DekUI's handler does. Those returning u32 return 0 (fall through)
// or the next pc (see redirect() there).
void dekui_capture_page(Context* c);
void dekui_enter_page(Context* c);
void dekui_leave_page(Context* c);
void dekui_capture_gear(Context* c);
void dekui_close_gear(Context* c);
void dekui_model_frame(Context* c);
void dekui_model_begin(Context* c);
void dekui_model_end(Context* c);
void dekui_dialogue_begin(Context* c);
void dekui_dialogue_end(Context* c);
void dekui_layout_pass(Context* c);
void dekui_layout_begin(Context* c);
void dekui_layout_end(Context* c);
u32 dekui_nav_vertical(Context* c);
u32 dekui_nav_horizontal(Context* c);
u32 dekui_nav_gear(Context* c);
u32 dekui_map_0(Context* c);
u32 dekui_map_1(Context* c);
u32 dekui_map_2(Context* c);
u32 dekui_map_3(Context* c);
u32 dekui_map_4(Context* c);
u32 dekui_map_5(Context* c);
u32 dekui_map_6(Context* c);
u32 dekui_map_7(Context* c);
u32 dekui_backdrop_0(Context* c);
u32 dekui_backdrop_1(Context* c);
u32 dekui_backdrop_2(Context* c);
u32 dekui_backdrop_4(Context* c);
u32 dekui_backdrop_5(Context* c);
u32 dekui_backdrop_6(Context* c);
u32 dekui_menu_memory_0(Context* c);
u32 dekui_menu_memory_1(Context* c);
u32 dekui_menu_memory_2(Context* c);
u32 dekui_page_switch_0(Context* c);
u32 dekui_page_switch_1(Context* c);
u32 dekui_page_switch_2(Context* c);
u32 dekui_page_switch_3(Context* c);
u32 dekui_page_switch_4(Context* c);
u32 dekui_page_switch_5(Context* c);
u32 dekui_page_switch_6(Context* c);
u32 dekui_page_switch_7(Context* c);
void dekui_menu_sound(Context* c);
u32 dekui_present_intro(Context* c);
void dekui_photo_0(Context* c);
void dekui_photo_1(Context* c);
void dekui_photo_2(Context* c);
void dekui_photo_3(Context* c);
void dekui_pause_1(Context* c);
void dekui_pause_2(Context* c);
void dekui_startup_enter(Context* c);
void dekui_startup_matrix(Context* c);
void dekui_schedule_enter(Context* c);
void dekui_schedule_stop(Context* c);
void dekui_notebook_sync(Context* c);
void dekui_notebook_rows_enter(Context* c);
void dekui_notebook_rows_leave(Context* c);
void dekui_notebook_map_input(Context* c);
void dekui_notebook_schedule_input(Context* c);
void dekui_notebook(Context* c);
void dekui_song_nav(Context* c);
void dekui_text(Context* c);
void dekui_options_draw(Context* c);
void dekui_options_select(Context* c);
void dekui_minimap_before(Context* c);
void dekui_minimap_after(Context* c);
u32 dekui_stick_pad(Context* c);
u32 dekui_stick_map(Context* c);
u32 dekui_minimap_shortcut(Context* c);
bool dekui_active();  // the single-screen modules run (HD HUD build, DOT_DEKUI not 0, code matches)
// Host input for hid:USER, before it reaches the game (DekUI's hid.cpp filters).
void dekui_filter_pad(u32& buttons, s16& x, s16& y);
bool dekui_touch_blocked();
// Applies DekUI's presentation settings: the built-in defaults, or this presentation.json.
bool dekui_load_presentation(const char* path);

}  // namespace recomp::hooks
