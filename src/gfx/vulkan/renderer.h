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
#include <atomic>
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
    // Creates every pipeline recorded in cache/pipeline_manifest.bin (all earlier runs) on several
    // threads; call before the guest starts. `done`/`total` report progress to another thread.
    void warm(std::atomic<u32>& done, std::atomic<u32>& total);
    // The .shbin code blobs of the romfs: a draw's vertex program is recorded in the seed by blob.
    void load_shader_blobs(const std::filesystem::path& romfs);
    // Launcher, after extraction: compiles and creates every pipeline of a pipeline_seed.bin (from
    // the repository: generator inputs only) against the romfs blobs; saves the caches for the
    // game. False if the seed is missing or empty.
    bool precompile(const std::filesystem::path& seed, std::atomic<u32>& done, std::atomic<u32>& total);

    // A new command list starts: the guest may have rewritten any texture since the last one.
    void begin_list();
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
    // With frame interpolation (DOT_FPS=60/90/120), a presenting call (`consume`) for the top
    // screen returns the real or in-between image due now on the presentation timeline.
    bool display(core::Memory& mem, u32 va, GpuScreen& out, bool consume = true);
    // Guest VBlank (GSP): the clock the presentation timeline runs on.
    void vblank();
    // Reads a screen image back (DOT_DUMP_FRAMES), RGBA8 rows top first.
    bool read_display(u32 va, std::vector<u32>& rgba, u32& width, u32& height, bool interp = false);
    u32 scale() const;
    // Runtime settings (menu): top screen aspect (0 = off, < 0 = the window's), frame rate
    // (30 = off, 60/90/120 = 1/2/3 interpolated frames per game frame) and the resolution scale (1-8). A new scale writes GPU-only surfaces
    // back to guest memory and drops every cached render target; the next draws recreate them.
    // Call it from the presenting thread before it fetches the screens (their images die here).
    void set_scale(core::Memory& mem, u32 scale);
    // Savestates: write every GPU-only surface back to guest memory / forget every cached
    // render target (guest memory was replaced). Screen images are kept until replaced.
    void flush(core::Memory& mem);
    void drop_targets();
    void set_widescreen(float aspect);
    void set_frame_rate(u32 fps);

    struct Stats {
        u64 draws, skipped, pipelines, readbacks, shaders;  // shaders = GLSL compiled (not from the disk cache)
        u64 warmed;                                         // pipelines created by warm(); `pipelines` = on demand
        u64 interpolated, not_interpolated;                 // DOT_FPS>=60: game frames interpolated / not (cuts)
        u64 camera_draws;                                   // in-between draws moved along the rigid camera path
        u64 uv_wrapped, uv_stepped;                         // in-between UV translations: shortest way round / cell switch
        u64 smooth, repeated, skipped_shots;                // top-screen presents: next image / same again / one jumped
        // DOT_STATS: time inside draw() (real draws, vertex fetch included) and inside frame replays, vertices uploaded.
        u64 draw_ns, replay_ns, vertices;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void drop_targets_locked(bool displays);
};

}  // namespace gfx::vulkan
