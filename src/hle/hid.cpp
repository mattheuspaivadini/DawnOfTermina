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
#include <utility>

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

bool Initialize() {
    state = {};
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
    const bool* kb = SDL_GetKeyboardState(nullptr);
    for (auto [key, bit] : KEYS)
        if (kb[key]) buttons |= bit;
    x += (kb[SDL_SCANCODE_D] - kb[SDL_SCANCODE_A]) * CPAD_MAX;
    y += (kb[SDL_SCANCODE_W] - kb[SDL_SCANCODE_S]) * CPAD_MAX;

    if (gamepad) {
        for (auto [btn, bit] : PAD)
            if (SDL_GetGamepadButton(gamepad, btn)) buttons |= bit;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) buttons |= ZL;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) buttons |= ZR;
        x += axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        y -= axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));  // SDL Y grows downward
    }
    state = next_pad(state, buttons, s16(std::clamp(x, -int(CPAD_MAX), int(CPAD_MAX))),
                     s16(std::clamp(y, -int(CPAD_MAX), int(CPAD_MAX))));
}

PadStatus ReadPad() { return state; }

}  // namespace hle::hid
