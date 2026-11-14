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

#include <string_view>
#include <vector>

#include "core/types.h"

// nn::hid — 3DS pad state fed from SDL keyboard + first connected gamepad.
// Call Update() once per frame (after SDL event pumping), then ReadPad().
namespace hle::hid {

enum Button : u32 {
    A = 1u << 0, B = 1u << 1, SELECT = 1u << 2, START = 1u << 3,
    DRIGHT = 1u << 4, DLEFT = 1u << 5, DUP = 1u << 6, DDOWN = 1u << 7,
    R = 1u << 8, L = 1u << 9, X = 1u << 10, Y = 1u << 11,
    ZL = 1u << 14, ZR = 1u << 15,
    CPAD_RIGHT = 1u << 28, CPAD_LEFT = 1u << 29, CPAD_UP = 1u << 30, CPAD_DOWN = 1u << 31,
};

constexpr s16 CPAD_MAX = 0x9C;  // circle pad range is roughly +-156

struct PadStatus {
    u32 hold = 0, trigger = 0, release = 0;
    s16 stick_x = 0, stick_y = 0;
    bool touch = false;  // left mouse button held over the bottom screen
    u16 touch_x = 0, touch_y = 0;  // digitizer coordinates, 0..319 x 0..239, origin top-left
};

// Maps a window point to the bottom screen of the 400x480 canvas that gfx::vulkan::Context
// presents (same fit-and-centre layout). False if the point is outside the bottom screen.
bool window_to_touch(float wx, float wy, int win_w, int win_h, u16& tx, u16& ty);

// Pure state step: derives trigger/release and the CPAD_* direction bits.
PadStatus next_pad(const PadStatus& prev, u32 buttons, s16 stick_x, s16 stick_y);

// Scripted input (DOT_INPUT="<seconds>:<event>,..."), for runs without a usable keyboard or
// mouse such as automated validation. An event is a button name (A B X Y L R ZL ZR START
// SELECT UP DOWN LEFT RIGHT) or T<x>x<y> (bottom-screen touch); each is held for 150 ms,
// timed from the first Update(). Returns false on a malformed script.
struct ScriptEvent {
    double t;
    u32 buttons;
    bool touch;
    u16 x, y;
};
bool parse_input_script(std::string_view text, std::vector<ScriptEvent>& out);

bool Initialize();  // requires SDL_Init(SDL_INIT_GAMEPAD); reads DOT_INPUT
void Update();
PadStatus ReadPad();

}  // namespace hle::hid
