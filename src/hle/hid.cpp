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

#include "hle/hid.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <utility>

#include "gfx/layout.h"
#include "hle/service/gsp.h"

namespace hle::hid {

// Keyboard: K/J/I/U = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, arrows = D-pad, WASD = circle pad.
static constexpr std::pair<SDL_Scancode, u32> KEYS[] = {
    {SDL_SCANCODE_K, A},          {SDL_SCANCODE_J, B},         {SDL_SCANCODE_I, X},
    {SDL_SCANCODE_U, Y},          {SDL_SCANCODE_Q, L},         {SDL_SCANCODE_E, R},
    {SDL_SCANCODE_1, ZL},         {SDL_SCANCODE_3, ZR},        {SDL_SCANCODE_RETURN, START},
    {SDL_SCANCODE_BACKSPACE, SELECT}, {SDL_SCANCODE_UP, DUP},  {SDL_SCANCODE_DOWN, DDOWN},
    {SDL_SCANCODE_LEFT, DLEFT},   {SDL_SCANCODE_RIGHT, DRIGHT},
};

// Gamepad uses positional (Nintendo) layout: east = A, south = B.
static constexpr std::pair<SDL_GamepadButton, u32> PAD[] = {
    {SDL_GAMEPAD_BUTTON_EAST, A},          {SDL_GAMEPAD_BUTTON_SOUTH, B},
    {SDL_GAMEPAD_BUTTON_NORTH, X},         {SDL_GAMEPAD_BUTTON_WEST, Y},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R},
    {SDL_GAMEPAD_BUTTON_START, START},     {SDL_GAMEPAD_BUTTON_BACK, SELECT},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, DUP},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DDOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, DLEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DRIGHT},
};

static std::mutex state_lock;  // Update() runs on the host main thread, readers on guest threads
static PadStatus state;
static SDL_Gamepad* gamepad = nullptr;

PadStatus next_pad(const PadStatus& prev, u32 buttons, s16 x, s16 y) {
    constexpr s16 DIGITAL = 41;  // stick deflection that also reports as a direction bit
    buttons &= ~(CPAD_RIGHT | CPAD_LEFT | CPAD_UP | CPAD_DOWN);
    if (x >= DIGITAL) buttons |= CPAD_RIGHT;
    if (x <= -DIGITAL) buttons |= CPAD_LEFT;
    if (y >= DIGITAL) buttons |= CPAD_UP;
    if (y <= -DIGITAL) buttons |= CPAD_DOWN;
    return {buttons, buttons & ~prev.hold, prev.hold & ~buttons, x, y};
}

static std::vector<ScriptEvent> script;

bool parse_input_script(std::string_view text, std::vector<ScriptEvent>& out) {
    static constexpr std::pair<std::string_view, u32> NAMES[] = {
        {"A", A},   {"B", B},   {"X", X},   {"Y", Y},         {"L", L},          {"R", R},       {"ZL", ZL},
        {"ZR", ZR}, {"UP", DUP}, {"DOWN", DDOWN}, {"LEFT", DLEFT}, {"RIGHT", DRIGHT}, {"START", START}, {"SELECT", SELECT},
    };
    out.clear();
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string item(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        ScriptEvent e{};
        char name[16] = {};
        unsigned x = 0, y = 0;
        int sx = 0, sy = 0, at = 0;
        if (std::sscanf(item.c_str(), "%lf+%lf:%n", &e.t, &e.duration, &at) < 2 || !at) {
            at = 0;
            if (std::sscanf(item.c_str(), "%lf:%n", &e.t, &at) < 1 || !at) return false;
        }
        const char* ev = item.c_str() + at;
        if (std::sscanf(ev, "T%ux%u", &x, &y) == 2 && x < 320 && y < 240) {
            e.touch = true, e.x = u16(x), e.y = u16(y);
        } else if (std::sscanf(ev, "CP%dx%d", &sx, &sy) == 2 && std::abs(sx) <= int(CPAD_MAX) &&
                   std::abs(sy) <= int(CPAD_MAX)) {
            e.stick = true, e.sx = s16(sx), e.sy = s16(sy);
        } else if (std::sscanf(ev, "%15[A-Z]", name) == 1) {
            for (auto [n, bit] : NAMES)
                if (n == name) e.buttons = bit;
            if (!e.buttons) return false;
        } else {
            return false;
        }
        out.push_back(e);
    }
    return true;
}

bool Initialize() {
    state = {};
    if (const char* s = std::getenv("DOT_INPUT"); s && !parse_input_script(s, script)) {
        std::fprintf(stderr, "hid: malformed DOT_INPUT\n");
        return false;
    }
    return true;
}

bool window_to_touch(float wx, float wy, int win_w, int win_h, u16& tx, u16& ty) {
    gfx::Rect top, bottom;
    gfx::screen_layout(float(win_w), float(win_h), top, bottom);
    if (bottom.w <= 0 || bottom.h <= 0) return false;
    const float x = (wx - bottom.x) / bottom.w * 320, y = (wy - bottom.y) / bottom.h * 240;
    if (x < 0 || y < 0 || x >= 320 || y >= 240) return false;
    tx = u16(x), ty = u16(y);
    return true;
}

static s16 axis(Sint16 v) { return s16(std::clamp(v * CPAD_MAX / 32767, -int(CPAD_MAX), int(CPAD_MAX))); }

void Update() {
    if (!gamepad) {
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            if (n > 0) gamepad = SDL_OpenGamepad(ids[0]);
            SDL_free(ids);
        }
    } else if (!SDL_GamepadConnected(gamepad)) {
        SDL_CloseGamepad(gamepad);
        gamepad = nullptr;
    }

    u32 buttons = 0;
    int x = 0, y = 0;
    // A script owns the input: host devices are ignored so a run is reproducible.
    static const bool empty[SDL_SCANCODE_COUNT] = {};
    const bool* kb = script.empty() ? SDL_GetKeyboardState(nullptr) : empty;
    for (auto [key, bit] : KEYS)
        if (kb[key]) buttons |= bit;
    x += (kb[SDL_SCANCODE_D] - kb[SDL_SCANCODE_A]) * CPAD_MAX;
    y += (kb[SDL_SCANCODE_W] - kb[SDL_SCANCODE_S]) * CPAD_MAX;

    if (gamepad && script.empty()) {
        for (auto [btn, bit] : PAD)
            if (SDL_GetGamepadButton(gamepad, btn)) buttons |= bit;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) buttons |= ZL;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) buttons |= ZR;
        x += axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        y -= axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));  // SDL Y grows downward
    }
    // Touch: left mouse button over the bottom screen of the focused window. While dragged
    // off-screen the pen stays down at the last position, like a stylus slid to the bezel.
    float mx = 0, my = 0;
    const bool down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) && script.empty();
    int w = 0, h = 0;
    if (SDL_Window* win = SDL_GetMouseFocus()) SDL_GetWindowSize(win, &w, &h);

    bool script_touch = false;
    u16 sx = 0, sy = 0;
    if (!script.empty()) {
        // Guest time in VBlanks, not wall time: the script stays in step with the game when
        // frames drop or under DOT_SPEED.
        const double t = double(service::gsp::stats().vblanks) / 59.83;
        for (const auto& e : script)
            if (t >= e.t && t < e.t + e.duration) {
                buttons |= e.buttons;
                if (e.stick) x += e.sx, y += e.sy;
                if (e.touch) script_touch = true, sx = e.x, sy = e.y;
            }
    }

    std::lock_guard lk(state_lock);
    const PadStatus prev = state;
    state = next_pad(state, buttons, s16(std::clamp(x, -int(CPAD_MAX), int(CPAD_MAX))),
                     s16(std::clamp(y, -int(CPAD_MAX), int(CPAD_MAX))));
    u16 tx, ty;
    if (script_touch) state.touch = true, state.touch_x = sx, state.touch_y = sy;
    else if (down && window_to_touch(mx, my, w, h, tx, ty)) state.touch = true, state.touch_x = tx, state.touch_y = ty;
    else if (down && prev.touch) state.touch = true, state.touch_x = prev.touch_x, state.touch_y = prev.touch_y;
}

PadStatus ReadPad() {
    std::lock_guard lk(state_lock);
    return state;
}

}  // namespace hle::hid
