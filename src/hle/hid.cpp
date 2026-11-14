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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <utility>

#include "gfx/layout.h"
#include "hle/service/gsp.h"

namespace hle::hid {

// Bottom-screen touch buttons of the field HUD (digitizer coordinates), measured on a dump
// of the Lost Woods HUD: item I top-right, item II bottom-right, the Ocarina slot
// bottom-left (still empty there, so its action is not yet confirmed), the Map tab.
constexpr u16 TOUCH_ITEM_I_X = 293, TOUCH_ITEM_I_Y = 27, TOUCH_ITEM_II_X = 293, TOUCH_ITEM_II_Y = 213;
constexpr u16 TOUCH_OCARINA_X = 27, TOUCH_OCARINA_Y = 213, TOUCH_MAP_X = 160, TOUCH_MAP_Y = 220;

// Keyboard: K/J/I/U = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, arrows = D-pad, WASD = circle pad,
// F/G = item buttons I/II, O = Ocarina, M = map (touch-screen buttons of the bottom screen).
// shortcut: fixed touch points; a screen that moves these buttons needs its own.
static KeyBinding KEYS[] = {
    {"A", "A", SDL_SCANCODE_K, A},
    {"B", "B", SDL_SCANCODE_J, B},
    {"X", "X", SDL_SCANCODE_I, X},
    {"Y", "Y", SDL_SCANCODE_U, Y},
    {"L", "L", SDL_SCANCODE_Q, L},
    {"R", "R", SDL_SCANCODE_E, R},
    {"ZL", "ZL", SDL_SCANCODE_1, ZL},
    {"ZR", "ZR", SDL_SCANCODE_3, ZR},
    {"START", "Start", SDL_SCANCODE_RETURN, START},
    {"SELECT", "Select", SDL_SCANCODE_BACKSPACE, SELECT},
    {"UP", "D-pad up", SDL_SCANCODE_UP, DUP},
    {"DOWN", "D-pad down", SDL_SCANCODE_DOWN, DDOWN},
    {"LEFT", "D-pad left", SDL_SCANCODE_LEFT, DLEFT},
    {"RIGHT", "D-pad right", SDL_SCANCODE_RIGHT, DRIGHT},
    {"CP_UP", "Circle pad up", SDL_SCANCODE_W, 0, 0, CPAD_MAX},
    {"CP_DOWN", "Circle pad down", SDL_SCANCODE_S, 0, 0, -CPAD_MAX},
    {"CP_LEFT", "Circle pad left", SDL_SCANCODE_A, 0, -CPAD_MAX, 0},
    {"CP_RIGHT", "Circle pad right", SDL_SCANCODE_D, 0, CPAD_MAX, 0},
    {"ITEM_I", "Item I (touch)", SDL_SCANCODE_F, 0, 0, 0, TOUCH_ITEM_I_X, TOUCH_ITEM_I_Y, true, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"ITEM_II", "Item II (touch)", SDL_SCANCODE_G, 0, 0, 0, TOUCH_ITEM_II_X, TOUCH_ITEM_II_Y, true, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"OCARINA", "Ocarina (touch)", SDL_SCANCODE_O, 0, 0, 0, TOUCH_OCARINA_X, TOUCH_OCARINA_Y, true, SDL_GAMEPAD_BUTTON_MISC1},
    {"MAP", "Map (touch)", SDL_SCANCODE_M, 0, 0, 0, TOUCH_MAP_X, TOUCH_MAP_Y, true, SDL_GAMEPAD_BUTTON_TOUCHPAD},
};

std::span<KeyBinding> key_bindings() { return KEYS; }

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
static bool host_input = true;
static float mouse_x = 0, mouse_y = 0;  // mouse camera deflection pending for the next Update()

void set_host_input(bool enabled) { host_input = enabled; }

void add_mouse_motion(float dx, float dy, float sensitivity) {
    // ~40 px of motion per frame = full deflection at sensitivity 1.
    mouse_x += dx * sensitivity / 40.0f, mouse_y -= dy * sensitivity / 40.0f;
}

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
        } else if ((std::sscanf(ev, "CP%dx%d", &sx, &sy) == 2 || std::sscanf(ev, "CS%dx%d", &sx, &sy) == 2) &&
                   std::abs(sx) <= int(CPAD_MAX) && std::abs(sy) <= int(CPAD_MAX)) {
            e.stick = ev[1] == 'P', e.c_stick = ev[1] == 'S', e.sx = s16(sx), e.sy = s16(sy);
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
    const bool host = script.empty() && host_input;
    float cx = host ? std::clamp(mouse_x, -1.0f, 1.0f) : 0, cy = host ? std::clamp(mouse_y, -1.0f, 1.0f) : 0;
    mouse_x = mouse_y = 0;
    static const bool empty[SDL_SCANCODE_COUNT] = {};
    const bool* kb = host ? SDL_GetKeyboardState(nullptr) : empty;
    const KeyBinding* touch_key = nullptr;
    for (const KeyBinding& k : KEYS) {
        const bool held = (k.key > 0 && k.key < SDL_SCANCODE_COUNT && kb[k.key]) ||
                          (host && gamepad && k.pad_button >= 0 && SDL_GetGamepadButton(gamepad, SDL_GamepadButton(k.pad_button)));
        if (!held) continue;
        buttons |= k.buttons, x += k.cp_x, y += k.cp_y;
        if (k.touch) touch_key = &k;
    }

    if (gamepad && host) {
        for (auto [btn, bit] : PAD)
            if (SDL_GetGamepadButton(gamepad, btn)) buttons |= bit;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) buttons |= ZL;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) buttons |= ZR;
        x += axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        y -= axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));  // SDL Y grows downward
        // Right stick = C-stick (camera), with a small dead zone.
        const float rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        const float ry = -SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
        if (std::abs(rx) > 0.15f || std::abs(ry) > 0.15f)
            cx = std::clamp(cx + rx, -1.0f, 1.0f), cy = std::clamp(cy + ry, -1.0f, 1.0f);
    }
    // Touch: left mouse button over the bottom screen of the focused window. While dragged
    // off-screen the pen stays down at the last position, like a stylus slid to the bezel.
    float mx = 0, my = 0;
    SDL_Window* focus = SDL_GetMouseFocus();
    const bool down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) && host &&
                      !(focus && SDL_GetWindowRelativeMouseMode(focus));  // captured = camera, not stylus
    int w = 0, h = 0;
    if (focus) SDL_GetWindowSize(focus, &w, &h);

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
                if (e.c_stick) cx = float(e.sx) / CPAD_MAX, cy = float(e.sy) / CPAD_MAX;
                if (e.touch) script_touch = true, sx = e.x, sy = e.y;
            }
    }

    std::lock_guard lk(state_lock);
    const PadStatus prev = state;
    state = next_pad(state, buttons, s16(std::clamp(x, -int(CPAD_MAX), int(CPAD_MAX))),
                     s16(std::clamp(y, -int(CPAD_MAX), int(CPAD_MAX))));
    state.c_x = cx, state.c_y = cy;
    u16 tx, ty;
    if (script_touch) state.touch = true, state.touch_x = sx, state.touch_y = sy;
    else if (touch_key) state.touch = true, state.touch_x = touch_key->x, state.touch_y = touch_key->y;
    else if (down && window_to_touch(mx, my, w, h, tx, ty)) state.touch = true, state.touch_x = tx, state.touch_y = ty;
    else if (down && prev.touch) state.touch = true, state.touch_x = prev.touch_x, state.touch_y = prev.touch_y;
}

PadStatus ReadPad() {
    std::lock_guard lk(state_lock);
    return state;
}

}  // namespace hle::hid
