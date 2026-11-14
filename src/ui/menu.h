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

#include <SDL3/SDL.h>

#include <filesystem>

#include "core/config.h"

namespace gfx::vulkan {
class Context;
class Renderer;
}  // namespace gfx::vulkan

// Settings menu (Dear ImGui over the final Vulkan pass, F1 or Esc) and the PC-side hotkeys:
// Tab = bottom screen overlay, F11 = fullscreen, F2 = mouse camera (captured cursor drives
// the C-stick). Settings are applied live and saved to config.toml on every change.
namespace ui {

bool init(SDL_Window* window, gfx::vulkan::Context& gpu, core::Config& config, const std::filesystem::path& file);
void set_renderer(gfx::vulkan::Renderer* renderer);  // null while there is none
void shutdown();
// Returns false when the event belongs to the menu (or a hotkey) and the game must not see it.
bool handle_event(const SDL_Event& e);
// Builds this frame's menu (once per presented frame, before Context::present).
void frame();
bool visible();

}  // namespace ui
