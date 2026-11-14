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

#include <span>
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
    float c_x = 0, c_y = 0;  // C-stick (Circle Pad Pro / New 3DS), -1..1, +y = up
};

// Keyboard bindings (SDL scancodes), editable at runtime and saved in config.toml [keys].
// Pad actions set `buttons`; CP_* move the circle pad; touch actions tap the bottom screen
// at (x, y) while held, a shortcut for its touch-only buttons.
struct KeyBinding {
    const char* action;  // config name
    const char* label;   // menu text
    int key;             // SDL_Scancode
    u32 buttons = 0;
    s16 cp_x = 0, cp_y = 0;
    u16 x = 0, y = 0;    // touch point when `touch`
    bool touch = false;
    int pad_button = -1;  // SDL_GamepadButton also triggering a touch action (-1 = none)
};
std::span<KeyBinding> key_bindings();

// Mouse camera: relative motion, in pixels, accumulated by the host loop since the last Update().
void add_mouse_motion(float dx, float dy, float sensitivity);
// While false (settings menu open), keyboard, mouse and gamepad are ignored.
void set_host_input(bool enabled);

// Maps a window point to the bottom screen as gfx::screen_layout places it (the layout
// Context::present uses). False if the point is outside the bottom screen.
bool window_to_touch(float wx, float wy, int win_w, int win_h, u16& tx, u16& ty);

// Pure state step: derives trigger/release and the CPAD_* direction bits.
PadStatus next_pad(const PadStatus& prev, u32 buttons, s16 stick_x, s16 stick_y);

// Scripted input (DOT_INPUT="<seconds>[+<duration>]:<event>,..."), for runs without a usable
// keyboard or mouse such as automated validation. An event is a button name (A B X Y L R ZL
// ZR START SELECT UP DOWN LEFT RIGHT), T<x>x<y> (bottom-screen touch) or CP<x>x<y> (circle
// pad, -156..156, +y = up) or CS<x>x<y> (C-stick, same range); each is held for <duration> seconds (default 0.15) of guest time,
// counted in VBlanks since boot. While a script is set, keyboard, mouse and gamepad are
// ignored. Returns false on a malformed script.
struct ScriptEvent {
    double t = 0, duration = 0.15;
    u32 buttons = 0;
    bool touch = false, stick = false, c_stick = false;
    u16 x = 0, y = 0;      // touch
    s16 sx = 0, sy = 0;    // circle pad, or C-stick (CS<x>x<y>, same range)
};
bool parse_input_script(std::string_view text, std::vector<ScriptEvent>& out);

bool Initialize();  // requires SDL_Init(SDL_INIT_GAMEPAD); reads DOT_INPUT
void Update();
PadStatus ReadPad();

}  // namespace hle::hid
