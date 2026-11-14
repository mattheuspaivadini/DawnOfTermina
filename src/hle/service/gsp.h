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

#include <vector>

#include "core/types.h"

// Host side of gsp::Gpu: the framebuffers the guest last swapped in.
namespace gfx::vulkan {
class Renderer;
struct GpuScreen;
}

namespace hle::service::gsp {

// Draws go to `renderer` once set (before that they are only counted).
void set_renderer(gfx::vulkan::Renderer* renderer);

// Fills `rgba` (bytes R,G,B,A; row 0 = top) with the current framebuffer of screen
// 0 (top, 400x240) or 1 (bottom, 320x240). Returns false until the guest set one.
bool screen_image(int screen, std::vector<u32>& rgba);

// The screen's framebuffer as a GPU image (the renderer's DisplayTransfer output, at the
// render scale), if that is its newest copy.
bool gpu_screen(int screen, gfx::vulkan::GpuScreen& out);

// The screen as pixels for DOT_DUMP_FRAMES: read back from the GPU (scaled) or from memory.
bool screen_dump(int screen, std::vector<u32>& rgba, u32& width, u32& height, bool interp = false);

// Counters for diagnostics.
struct Stats {
    u64 vblanks, command_lists, draws, memory_fills, display_transfers;
    u64 frames;  // top-screen buffer swaps latched: the game's own frame count
};
Stats stats();

}  // namespace hle::service::gsp
