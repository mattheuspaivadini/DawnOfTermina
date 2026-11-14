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

#include <array>
#include <memory>

#include "core/memory.h"
#include "gfx/pica/command_processor.h"

namespace gfx::vulkan {

class Context;

// Executes PICA200 draws with Vulkan. The render target mirrors the guest's colour buffer
// in VRAM: it is loaded from guest memory when first drawn to and written back (tiled, in
// the guest's pixel format) by flush(), which GSP calls before any GX command that could
// read or overwrite VRAM. One submission per draw.
// shortcut: no stencil, logic ops, fragment lighting, fog, geometry shaders; depth is kept
// on the GPU only (cleared by MemoryFill on the depth buffer, never written back).
class Renderer {
public:
    explicit Renderer(Context& ctx);
    ~Renderer();
    bool init();

    void draw(core::Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs,
              bool indexed);
    // Writes the render target back to guest memory and drops it.
    void flush(core::Memory& mem);
    // The guest filled [start, end) through MemoryFill: a depth buffer there is cleared.
    void memory_fill(u32 start_va, u32 end_va, u32 value, u32 width_mode);

    struct Stats {
        u64 draws, skipped, pipelines, flushes;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gfx::vulkan
