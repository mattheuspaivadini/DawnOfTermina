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
// Widescreen layout only: the bottom screen overlay is shown (toggled with Tab).
inline std::atomic<bool> g_show_bottom{true};

// Classic: top screen above, bottom screen centred below, scaled to fit. Widescreen (top
// wider than 5:3): the top screen fills the window and the bottom screen is an overlay in
// the bottom-right corner, 30% of the window height (empty rect when hidden).
inline void screen_layout(float win_w, float win_h, Rect& top, Rect& bottom) {
    const float tw = g_top_width.load();
    if (tw > 400.5f) {
        const float s = std::min(win_w / tw, win_h / 240.0f);
        top = {(win_w - tw * s) / 2, (win_h - 240 * s) / 2, tw * s, 240 * s};
        const float bh = g_show_bottom ? win_h * 0.3f : 0.0f, bw = bh * 320 / 240;
        bottom = {win_w - bw, win_h - bh, bw, bh};
        return;
    }
    const float s = std::min(win_w / 400.0f, win_h / 480.0f);
    const float ox = (win_w - 400 * s) / 2, oy = (win_h - 480 * s) / 2;
    top = {ox, oy, 400 * s, 240 * s};
    bottom = {ox + 40 * s, oy + 240 * s, 320 * s, 240 * s};
}

}  // namespace gfx
