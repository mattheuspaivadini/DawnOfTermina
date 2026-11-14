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
#include <filesystem>
#include <memory>
#include <vector>

#include "core/memory.h"
#include "gfx/pica/command_processor.h"

namespace gfx::vulkan {

class Context;

struct GpuScreen;

// Executes PICA200 draws with Vulkan. Colour buffers live in a GPU-resident surface cache keyed
// by their guest address, rendered at DOT_RES_SCALE times the guest resolution; guest memory
// is only written back when a CPU-side GX operation (DMA, TextureCopy, a DisplayTransfer the
// GPU path does not handle) touches a surface, or when a texture reads part of one. CPU
// writes to a surface's memory are detected by hashing it when the surface is next used.
// DisplayTransfers from a cached surface go straight to a screen-oriented image that
// presentation reads (see display()).
// shortcut: plain CPU loads of rendered pixels are not detected (no read faults on guest
// memory); DOT_READBACK=1 restores a write-back after every frame. No logic ops, gas, shadow
// maps, geometry shaders or MSAA; depth stays on the GPU.
class Renderer {
public:
    explicit Renderer(Context& ctx);
    ~Renderer();
    // `cache_dir` holds the on-disk shader/pipeline caches (created on first save).
    bool init(const std::filesystem::path& cache_dir);

    void draw(core::Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs,
              bool indexed);
    // Brings guest memory in [start, end) up to date with the GPU before a CPU-side GX
    // operation reads or writes it. With `overwrite`, surfaces entirely inside the range
    // are skipped (they are about to be replaced).
    void sync(core::Memory& mem, u32 start_va, u32 end_va, bool overwrite = false);
    // The guest filled [start, end) through MemoryFill (already applied to guest memory):
    // clears a colour surface or a depth buffer starting there.
    void memory_fill(core::Memory& mem, u32 start_va, u32 end_va, u32 value, u32 width_mode);
    // DisplayTransfer from a cached colour surface into a screen image on the GPU; false when
    // the transfer is not one the GPU path handles (the caller then runs it on the CPU).
    bool display_transfer(core::Memory& mem, u32 in, u32 out, u32 in_dim, u32 out_dim, u32 flags);
    // The screen image last transferred to `va`, if guest memory there is unchanged since.
    bool display(core::Memory& mem, u32 va, GpuScreen& out);
    // Reads a screen image back (DOT_DUMP_FRAMES), RGBA8 rows top first.
    bool read_display(u32 va, std::vector<u32>& rgba, u32& width, u32& height);
    u32 scale() const;

    struct Stats {
        u64 draws, skipped, pipelines, readbacks, shaders;  // shaders = GLSL compiled (not from the disk cache)
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gfx::vulkan
