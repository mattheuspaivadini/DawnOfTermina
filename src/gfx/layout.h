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

#include <algorithm>
#include <atomic>

// Where the two screens go in the window; shared by presentation and mouse touch input.
namespace gfx {

struct Rect {
    float x, y, w, h;
};

// Width of the top screen in 240-row units: 400 normally, more when the renderer widened it
// (set by presentation from the top screen image it shows).
inline std::atomic<float> g_top_width{400.0f};
// Single-screen layout: the bottom screen overlay is shown (toggled with Tab).
inline std::atomic<bool> g_show_bottom{true};
// Single screen (default): the top screen fills the window, the bottom screen is an overlay.
// Dual: both screens stacked, the classic 3DS view.
inline std::atomic<bool> g_single_screen{true};

// N64 layout (single screen with no bottom overlay): pieces of the bottom screen (hearts,
// rupees, item slots, minimap) are drawn in the corners of the top screen, and the whole
// bottom screen pops up in the middle, over a dimmed top screen, while the game shows a
// menu there (g_bottom_modal, read from game memory) or F3 forces it (g_force_modal).
inline std::atomic<bool> g_n64{false};
inline std::atomic<bool> g_bottom_modal{false}, g_force_modal{false};
inline std::atomic<bool> g_minimap{true};  // Tab in the N64 layout
inline std::atomic<bool> g_hud_visible{true};  // the bottom screen shows the gameplay HUD (game memory)
inline std::atomic<float> g_hud_scale{0.6f}, g_hud_opacity{0.9f};
inline bool bottom_modal() { return g_n64 && (g_bottom_modal || g_force_modal); }

// A rectangle of the bottom screen (320x240 units) placed in a corner of the top screen.
// x/y: distance from that corner, in bottom-screen pixels at the piece's scale.
struct HudPiece {
    Rect src;
    bool right, bottom;
    float x, y, scale;
    bool minimap;
};
// Measured on the MM3D gameplay bottom screen (Lost Woods): right column I / X / Y / II as
// a C-button cluster, hearts top left, Ocarina slot + Gear + rupees bottom left, map with
// its Masks/Map/Items tabs bottom right (raised above the top screen's A-button prompt).
inline constexpr HudPiece HUD_PIECES[] = {
    {{265, 2, 53, 53}, true, false, 109, 26, 1, false},    // item I
    {{266, 68, 52, 52}, true, false, 55, 0, 1, false},     // X
    {{258, 118, 50, 50}, true, false, 56, 54, 1, false},   // Y
    {{265, 185, 53, 53}, true, false, 0, 26, 1, false},    // item II
    {{64, 0, 192, 36}, false, false, 0, 0, 1, false},      // hearts
    {{2, 185, 53, 53}, false, true, 0, 0, 1, false},       // Ocarina slot
    {{0, 100, 56, 40}, false, true, 57, 6, 1, false},      // Gear
    {{0, 64, 56, 32}, false, true, 0, 57, 1, false},       // rupees
    {{64, 36, 192, 204}, true, true, 0, 110, 0.75f, true},  // minimap + tabs
};

// Where a HUD piece goes on the top screen rect; empty when hidden.
inline Rect hud_rect(const Rect& top, const HudPiece& p) {
    if (!g_hud_visible || (p.minimap && !g_minimap)) return {0, 0, 0, 0};
    const float u = top.h / 240, s = u * g_hud_scale * p.scale, margin = 6 * u;
    const float w = p.src.w * s, h = p.src.h * s, ox = p.x * s + margin, oy = p.y * s + margin;
    return {p.right ? top.x + top.w - ox - w : top.x + ox, p.bottom ? top.y + top.h - oy - h : top.y + oy, w, h};
}

// Render scale for a w x h window (DOT_RES_SCALE=native and the config presets): the top
// screen fills the window height in widescreen, else both screens stacked fit in it.
inline unsigned native_scale(unsigned w, unsigned h, bool widescreen) {
    return std::clamp(widescreen ? h / 240 : std::min(w / 400, h / 480), 1u, 8u);
}

// Single: the top screen (any width) fills the window keeping its aspect; the bottom screen
// is an overlay in the bottom-right corner, 30% of the window height (empty rect when
// hidden). Dual: top above, bottom centred below, scaled to fit.
inline void screen_layout(float win_w, float win_h, Rect& top, Rect& bottom) {
    const float tw = g_top_width.load();
    if (g_single_screen) {
        const float s = std::min(win_w / tw, win_h / 240.0f);
        top = {(win_w - tw * s) / 2, (win_h - 240 * s) / 2, tw * s, 240 * s};
        const float bh = g_show_bottom ? win_h * 0.3f : 0.0f, bw = bh * 320 / 240;
        bottom = {win_w - bw, win_h - bh, bw, bh};
        if (g_n64) {  // no overlay; the bottom screen only shows as the menu pop-up
            const float mw = bottom_modal() ? std::min(win_h * 0.8f * 320 / 240, win_w * 0.95f) : 0.0f;
            const float mh = mw * 240 / 320;
            bottom = {(win_w - mw) / 2, (win_h - mh) / 2, mw, mh};
        }
        return;
    }
    const float s = std::min(win_w / tw, win_h / 480.0f);
    const float ox = (win_w - tw * s) / 2, oy = (win_h - 480 * s) / 2;
    top = {ox, oy, tw * s, 240 * s};
    bottom = {ox + (tw - 320) / 2 * s, oy + 240 * s, 320 * s, 240 * s};
}

}  // namespace gfx
