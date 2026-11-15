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

// gsp::Gpu — GPU access: shared memory with the interrupt relay queue, framebuffer info
// and the GX command queue; VBlank interrupts at 60 Hz; framebuffer presentation.

#include "hle/service/gsp.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "gfx/pica/command_processor.h"
#include "gfx/pica/pixel.h"
#include "gfx/vulkan/context.h"
#include "gfx/vulkan/renderer.h"
#include "core/savestate.h"
#include "hle/ipc.h"
#include "hle/service/services.h"

namespace hle::service {

namespace {

using core::Memory;

enum Interrupt : u8 { PSC0 = 0, PSC1 = 1, PDC0 = 2, PDC1 = 3, PPF = 4, P3D = 5, DMA = 6 };

constexpr u32 SHARED_SIZE = 0x1000;
constexpr Result RESULT_FIRST_INITIALIZATION = 0x2A07;

struct Framebuffer {
    u32 va = 0, stride = 0, format = 0;
};

core::Memory* g_mem = nullptr;
std::mutex g_fb_lock;  // guards g_fb (read by the host main thread)
Framebuffer g_fb[2];
Handle g_shared = 0, g_event = 0;
gfx::pica::CommandProcessor g_pica;
std::atomic<gfx::vulkan::Renderer*> g_renderer{nullptr};
std::atomic<u64> g_vblanks{0}, g_lists{0}, g_draws{0}, g_fills{0}, g_transfers{0}, g_frames{0};

u32 shared_base() { return kernel::shared_memory(g_shared).addr; }

void signal_interrupt(Interrupt id) {
    const u32 base = shared_base();
    if (!base || !g_event) return;
    const u32 q = base;  // relay queue of thread index 0
    const u8 index = g_mem->read<u8>(q), count = g_mem->read<u8>(q + 1);
    if (count < 0x34) {
        g_mem->write<u8>(q + 0xC + (index + count) % 0x34, id);
        g_mem->write<u8>(q + 1, u8(count + 1));
    }
    g_mem->write<u8>(q + 2, 0);  // no error
    kernel::signal_event(g_event);
}

void set_framebuffer(int screen, u32 info) {
    // FrameBufferInfo: active fb, left addr (PA), right addr, stride, format, shown fb, -
    if (std::getenv("DOT_TRACE_FB"))
        std::fprintf(stderr, "gsp: screen %d framebuffer pa %08X stride %u format %u\n", screen,
                     g_mem->read<u32>(info + 4), g_mem->read<u32>(info + 12), g_mem->read<u32>(info + 16));
    std::lock_guard lk(g_fb_lock);
    // The guest passes virtual addresses here (the GSP module converts them for the LCD).
    const u32 addr = g_mem->read<u32>(info + 4);
    const u32 va = Memory::va_to_pa(addr) ? addr : Memory::pa_to_va(addr);
    g_fb[screen] = {va, g_mem->read<u32>(info + 12), g_mem->read<u32>(info + 16) & 7};
}

// Latches framebuffer updates the guest queued in shared memory, then interrupts.
void vblank() {
    const u32 base = shared_base();
    if (!base) return;
    for (int screen = 0; screen < 2; ++screen) {
        const u32 upd = base + 0x200 + screen * 0x40;  // thread index 0
        if (g_mem->read<u8>(upd + 1)) {
            const u8 index = g_mem->read<u8>(upd) & 1;
            set_framebuffer(screen, upd + 4 + index * 0x1C);
            g_mem->write<u8>(upd + 1, 0);
            if (screen == 0) ++g_frames;
        }
    }
    ++g_vblanks;
    if (auto* r = g_renderer.load()) r->vblank();
    signal_interrupt(PDC0);
    signal_interrupt(PDC1);
}

using namespace gfx::pica::pixel;

// ---- GX commands
void memory_fill(u32 start, u32 value, u32 end, u16 control) {
    if (!start || end <= start) return;
    const u32 width = control >> 8 & 3;  // 0: 16-bit, 1: 24-bit, 2/3: 32-bit
    u8* p = g_mem->host(start);
    const u32 size = end - start;
    if (width >= 2) {
        for (u32 i = 0; i + 4 <= size; i += 4) std::memcpy(p + i, &value, 4);
    } else if (width == 1) {
        for (u32 i = 0; i + 3 <= size; i += 3) p[i] = u8(value), p[i + 1] = u8(value >> 8), p[i + 2] = u8(value >> 16);
    } else {
        const u16 v = u16(value);
        for (u32 i = 0; i + 2 <= size; i += 2) std::memcpy(p + i, &v, 2);
    }
}

void texture_copy(u32 in, u32 out, u32 size, u32 in_dim, u32 out_dim) {
    const u32 in_w = (in_dim & 0xFFFF) * 16, in_gap = (in_dim >> 16) * 16;
    const u32 out_w = (out_dim & 0xFFFF) * 16, out_gap = (out_dim >> 16) * 16;
    size &= ~0xFu;
    if (!in_w || !out_w) {
        std::memmove(g_mem->host(out), g_mem->host(in), size);
        return;
    }
    u32 ip = 0, op = 0, in_line = 0, out_line = 0;
    while (size) {
        const u32 n = std::min({in_w - in_line, out_w - out_line, size});
        std::memmove(g_mem->host(out + op), g_mem->host(in + ip), n);
        ip += n, op += n, in_line += n, out_line += n, size -= n;
        if (in_line == in_w) ip += in_gap, in_line = 0;
        if (out_line == out_w) op += out_gap, out_line = 0;
    }
}

void display_transfer(u32 in, u32 out, u32 in_dim, u32 out_dim, u32 flags) {
    const bool flip = flags & 1, input_linear = flags >> 1 & 1, dont_swizzle = flags >> 5 & 1;
    const u32 in_fmt = flags >> 8 & 7, out_fmt = flags >> 12 & 7, scaling = flags >> 24 & 3;
    const u32 in_w = in_dim & 0xFFFF;
    const u32 hs = scaling ? 1 : 0, vs = scaling == 2 ? 1 : 0;
    const u32 out_w = (out_dim & 0xFFFF) >> hs, out_h = (out_dim >> 16) >> vs;
    const u32 sb = bytes_per_pixel(in_fmt), db = bytes_per_pixel(out_fmt);
    const u8* src = g_mem->host(in);
    u8* dst = g_mem->host(out);
    for (u32 y = 0; y < out_h; ++y)
        for (u32 x = 0; x < out_w; ++x) {
            const u32 ix = x << hs, iy = y << vs, oy = flip ? out_h - y - 1 : y;
            u32 so, d;
            if (input_linear && !dont_swizzle) {  // linear -> tiled
                so = (ix + iy * in_w) * sb;
                d = morton_offset(x, oy, db) + (oy & ~7u) * out_w * db;
            } else if (input_linear) {  // linear -> linear
                so = (ix + iy * in_w) * sb;
                d = (x + oy * out_w) * db;
            } else if (!dont_swizzle) {  // tiled -> linear (the usual colour buffer -> framebuffer)
                so = morton_offset(ix, iy, sb) + (iy & ~7u) * in_w * sb;
                d = (x + oy * out_w) * db;
            } else {  // tiled -> tiled
                so = morton_offset(ix, iy, sb) + (iy & ~7u) * in_w * sb;
                d = morton_offset(x, oy, db) + (oy & ~7u) * out_w * db;
            }
            Rgba c = decode(in_fmt, src + so);
            if (scaling) {  // average the 2 (X) or 4 (XY) neighbours, consecutive in Morton order
                const u32 n = scaling == 2 ? 4 : 2;
                for (u32 k = 1; k < n; ++k) {
                    const Rgba e = decode(in_fmt, src + so + k * sb);
                    c.r += e.r, c.g += e.g, c.b += e.b, c.a += e.a;
                }
                c = {c.r / n, c.g / n, c.b / n, c.a / n};
            }
            encode(out_fmt, c, dst + d);
        }
}

// DisplayTransfers the renderer ran on the GPU: their output exists only as a screen image
// until something on the CPU side needs those bytes, then the transfer is replayed here.
struct Transfer {
    u32 in, out, in_dim, out_dim, flags;
    u32 in_end() const { return in + (in_dim & 0xFFFF) * (in_dim >> 16) * bytes_per_pixel(flags >> 8 & 7); }
    u32 out_end() const {
        const u32 scaling = flags >> 24 & 3, hs = scaling ? 1 : 0, vs = scaling == 2 ? 1 : 0;
        return out + ((out_dim & 0xFFFF) >> hs) * ((out_dim >> 16) >> vs) * bytes_per_pixel(flags >> 12 & 7);
    }
};
std::map<u32, Transfer> g_gpu_transfers;  // by output VA

// Guest memory in [start, end) is about to be read or written by the CPU side.
void settle(u32 start, u32 end, bool overwrite = false) {
    auto* r = g_renderer.load();
    if (!r) return;
    for (auto it = g_gpu_transfers.begin(); it != g_gpu_transfers.end();) {
        const Transfer t = it->second;
        if (t.out >= end || t.out_end() <= start) {
            ++it;
            continue;
        }
        it = g_gpu_transfers.erase(it);
        r->sync(*g_mem, t.in, t.in_end());
        display_transfer(t.in, t.out, t.in_dim, t.out_dim, t.flags);
    }
    r->sync(*g_mem, start, end, overwrite);
}

void run_command(u32 cmd) {
    auto w = [&](u32 i) { return g_mem->read<u32>(cmd + i * 4); };
    auto* r = g_renderer.load();
    switch (w(0) & 0xFF) {
    case 0x00:  // RequestDma(src, dst, size)
        settle(w(1), w(1) + w(3));
        settle(w(2), w(2) + w(3), true);
        std::memmove(g_mem->host(w(2)), g_mem->host(w(1)), w(3));
        signal_interrupt(DMA);
        return;
    case 0x01: {  // SubmitGpuCommandList(address, size, flags)
        const u32 addr = w(1), size = w(2);
        if (std::getenv("DOT_TRACE_FB")) std::fprintf(stderr, "gsp: command list %08X size %u\n", addr, size);
        g_pica.process({reinterpret_cast<const u32*>(g_mem->host(addr)), size / 4});
        ++g_lists;
        signal_interrupt(P3D);
        return;
    }
    case 0x02: {  // MemoryFill(start0, value0, end0, start1, value1, end1, control0 | control1 << 16)
        if (std::getenv("DOT_TRACE_FB"))
            std::fprintf(stderr, "gsp: memory fill %08X-%08X = %08X, %08X-%08X = %08X\n", w(1), w(3), w(2), w(4),
                         w(6), w(5));
        for (u32 k = 0; k < 2; ++k) {
            const u32 start = w(1 + k * 3), value = w(2 + k * 3), end = w(3 + k * 3);
            if (!start) continue;
            settle(start, end, true);
            memory_fill(start, value, end, u16(w(7) >> (k * 16)));
            if (r) r->memory_fill(*g_mem, start, end, value, w(7) >> (8 + k * 16) & 3);
        }
        ++g_fills;
        if (w(1)) signal_interrupt(PSC0);
        if (w(4)) signal_interrupt(PSC1);
        return;
    }
    case 0x03:  // DisplayTransfer(in, out, in dims, out dims, flags)
        if (std::getenv("DOT_TRACE_FB"))
            std::fprintf(stderr, "gsp: display transfer %08X -> %08X in %08X out %08X flags %08X\n", w(1), w(2), w(3),
                         w(4), w(5));
        if (r && r->display_transfer(*g_mem, w(1), w(2), w(3), w(4), w(5))) {
            // Tiled outputs became cached surfaces (synced like any other); linear ones are screens.
            if (!(w(5) >> 5 & 1)) g_gpu_transfers[w(2)] = {w(1), w(2), w(3), w(4), w(5)};
        } else if (w(5) >> 3 & 1) {
            const u32 size = (w(3) & 0xFFFF) * (w(3) >> 16) * 4;
            settle(w(1), w(1) + size);
            settle(w(2), w(2) + size, true);
            texture_copy(w(1), w(2), size, 0, 0);
        } else {
            const Transfer t{w(1), w(2), w(3), w(4), w(5)};
            settle(t.in, t.in_end());
            settle(t.out, t.out_end(), true);
            display_transfer(t.in, t.out, t.in_dim, t.out_dim, t.flags);
        }
        if (std::getenv("DOT_TRACE_FB")) {
            const u32 n = (w(4) & 0xFFFF) * (w(4) >> 16) * bytes_per_pixel(w(5) >> 12 & 7);
            u32 in_nz = 0, out_nz = 0;
            for (u32 i = 0; i < n; ++i) out_nz += g_mem->read<u8>(w(2) + i) != 0;
            for (u32 i = 0; i < (w(3) & 0xFFFF) * (w(3) >> 16) * bytes_per_pixel(w(5) >> 8 & 7); ++i)
                in_nz += g_mem->read<u8>(w(1) + i) != 0;
            std::fprintf(stderr, "gsp:   input %u non-zero bytes, output %u non-zero bytes\n", in_nz, out_nz);
        }
        ++g_transfers;
        signal_interrupt(PPF);
        return;
    case 0x04: {  // TextureCopy(in, out, size, in width|gap, out width|gap, flags)
        // shortcut: settles generous ranges (size plus gaps) rather than exact strided spans.
        const u32 in_gap = (w(4) >> 16) * 16, out_gap = (w(5) >> 16) * 16, in_w = (w(4) & 0xFFFF) * 16, out_w = (w(5) & 0xFFFF) * 16;
        const u32 in_span = in_w ? w(3) + w(3) / in_w * in_gap : w(3), out_span = out_w ? w(3) + w(3) / out_w * out_gap : w(3);
        settle(w(1), w(1) + in_span);
        settle(w(2), w(2) + out_span);
        texture_copy(w(1), w(2), w(3), w(4), w(5));
        signal_interrupt(PPF);
        return;
    }
    case 0x05:  // FlushCacheRegions
        return;
    default: {
        char msg[64];
        std::snprintf(msg, sizeof msg, "gsp: GX command 0x%02X", w(0) & 0xFF);
        throw kernel::Unimplemented(msg);
    }
    }
}

void trigger_queue() {
    const u32 q = shared_base() + 0x800;  // thread index 0
    if (q == 0x800) return;
    while (g_mem->read<u8>(q + 1)) {
        const u8 index = g_mem->read<u8>(q);
        run_command(q + 0x20 + index * 0x20);
        g_mem->write<u8>(q + 1, u8(g_mem->read<u8>(q + 1) - 1));
        g_mem->write<u8>(q, u8((index + 1) % 15));
    }
}

void gsp_command(core::Memory& mem, u32 base) {
    ipc::Cmd c{mem, base};
    switch (c.id()) {
    case 0x01:  // WriteHWRegs(offset, size, buffer)
    case 0x02:  // WriteHWRegsWithMask
    case 0x08:  // FlushDataCache
    case 0x09:  // InvalidateDataCache
    case 0x0B:  // SetLcdForceBlack
    case 0x10:  // SetAxiConfigQoSMode
    case 0x14:  // UnregisterInterruptRelayQueue
    case 0x16:  // AcquireRight
    case 0x17:  // ReleaseRight
    case 0x19:  // SaveVramSysArea
    case 0x1A:  // RestoreVramSysArea
    case 0x1E:  // SetInternalPriorities
    case 0x1F:  // StoreDataCache
        c.reply(1, 0);
        return;
    case 0x04: {  // ReadHWRegs(offset, size) -> static buffer
        const u32 size = c[2], addr = c.static_buffer_addr(0);
        std::memset(mem.host(addr), 0, size);
        c.reply(1, 2);
        c.set(2, ipc::static_buffer(size, 0)), c.set(3, addr);
        return;
    }
    case 0x18: {  // ImportDisplayCaptureInfo -> {left, right, format, stride} per screen
        std::lock_guard lk(g_fb_lock);
        for (const Framebuffer& f : g_fb)  // the caller is about to read them
            if (f.va) settle(f.va, f.va + f.stride * (&f == &g_fb[0] ? 400 : 320));
        c.reply(9, 0);
        for (u32 i = 0; i < 2; ++i) {
            const Framebuffer& f = g_fb[i];
            c.set(2 + i * 4, f.va), c.set(3 + i * 4, f.va), c.set(4 + i * 4, f.format), c.set(5 + i * 4, f.stride);
        }
        return;
    }
    case 0x05:  // SetBufferSwap(screen, FrameBufferInfo[7])
        set_framebuffer(c[1] & 1, base + 8);
        c.reply(1, 0);
        return;
    case 0x0C:  // TriggerCmdReqQueue
        trigger_queue();
        c.reply(1, 0);
        return;
    case 0x13:  // RegisterInterruptRelayQueue(flags, copy desc, event) -> thread index, shared memory
        g_event = c[3];
        if (!g_shared) g_shared = kernel::create_shared_memory(SHARED_SIZE);
        c.reply(2, 2, RESULT_FIRST_INITIALIZATION);
        c.set(2, 0);  // thread index
        c.set(3, ipc::copy_handles(1)), c.set(4, g_shared);
        return;
    default:
        ipc::unimplemented("gsp::Gpu", c);
    }
}

}  // namespace

void register_gsp(core::Memory& mem) {
    g_mem = &mem;
    g_pica.fetch = [](u32 pa, u32 bytes) -> std::span<const u32> {
        const u32 va = Memory::pa_to_va(pa);
        if (!va) return {};
        return {reinterpret_cast<const u32*>(g_mem->host(va)), bytes / 4};
    };
    g_pica.on_draw = [](u32 trigger) {
        ++g_draws;
        if (auto* r = g_renderer.load())
            r->draw(*g_mem, g_pica.regs(), g_pica.vs(), trigger == gfx::pica::reg::DRAW_ELEMENTS);
    };
    kernel::register_service("gsp::Gpu", gsp_command);
    kernel::add_periodic(16'715'000, vblank);  // ~59.83 Hz
    kernel::register_state(
        [](core::StateWriter& w) {
            settle(0, ~0u);  // GPU-side transfers and surfaces back into guest memory
            if (auto* r = g_renderer.load()) r->flush(*g_mem);
            w.section("GSP ");
            {
                std::lock_guard lk(g_fb_lock);
                w.put(g_fb);
            }
            w.put(g_shared), w.put(g_event), w.put(g_vblanks.load());
            g_pica.save_state(w);
        },
        [](core::StateReader& r) {
            r.section("GSP ");
            {
                std::lock_guard lk(g_fb_lock);
                r.get(g_fb);
            }
            r.get(g_shared), r.get(g_event);
            g_vblanks = r.get<u64>();  // DOT_INPUT times stay those of the original run
            g_pica.load_state(r);
            g_gpu_transfers.clear();
            if (auto* rr = g_renderer.load()) rr->drop_targets();
        });
}

namespace gsp {

void set_renderer(gfx::vulkan::Renderer* renderer) { g_renderer = renderer; }

bool screen_image(int screen, std::vector<u32>& rgba) {
    Framebuffer fb;
    {
        std::lock_guard lk(g_fb_lock);
        fb = g_fb[screen];
    }
    if (!fb.va) return false;
    const u32 w = screen == 0 ? 400 : 320, h = 240, bpp = bytes_per_pixel(fb.format);
    rgba.resize(w * h);
    // Framebuffers are stored rotated: each memory row is one screen column, bottom to top.
    for (u32 x = 0; x < w; ++x) {
        const u8* col = g_mem->host(fb.va + x * fb.stride);
        for (u32 y = 0; y < h; ++y) {
            const Rgba c = decode(fb.format, col + (h - 1 - y) * bpp);
            rgba[y * w + x] = c.r | c.g << 8 | c.b << 16 | 0xFFu << 24;
        }
    }
    return true;
}

bool gpu_screen(int screen, gfx::vulkan::GpuScreen& out) {
    auto* r = g_renderer.load();
    u32 va;
    {
        std::lock_guard lk(g_fb_lock);
        va = g_fb[screen].va;
    }
    return r && va && r->display(*g_mem, va, out);
}

bool screen_dump(int screen, std::vector<u32>& rgba, u32& width, u32& height, bool interp) {
    auto* r = g_renderer.load();
    u32 va;
    {
        std::lock_guard lk(g_fb_lock);
        va = g_fb[screen].va;
    }
    gfx::vulkan::GpuScreen g;
    if (r && va && r->display(*g_mem, va, g, false)) return r->read_display(va, rgba, width, height, interp);
    if (interp) return false;
    width = screen == 0 ? 400 : 320, height = 240;
    return screen_image(screen, rgba);
}

Stats stats() { return {g_vblanks, g_lists, g_draws, g_fills, g_transfers, g_frames}; }

void cpu_read(u32 start, u32 end) { settle(start, end); }

}  // namespace gsp

}  // namespace hle::service
