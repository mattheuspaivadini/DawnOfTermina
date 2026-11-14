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

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include "core/loader.h"
#include "game/crt0.h"
#include "game/guest.h"
#include "gfx/pica/texture.h"
#include "gfx/vulkan/context.h"
#include "gfx/vulkan/renderer.h"
#include "gfx/layout.h"
#include "hle/dsp.h"
#include "hle/fs.h"
#include "hle/hid.h"
#include "hle/srv.h"
#include "hle/kernel.h"
#include "hle/service/gsp.h"
#include "hle/service/services.h"
#include "hle/svc.h"
#include "recomp/runtime.h"

#ifdef DOT_HAVE_RECOMP
namespace recomp {
extern const FuncEntry kFunctions[];
extern const u32 kFunctionCount;
}  // namespace recomp
#endif

// ExHeader ARM11 local capabilities: main thread priority at 0x20F.
static u8 read_main_priority(const std::filesystem::path& data) {
    std::ifstream f(data / "exheader.bin", std::ios::binary);
    f.seekg(0x20F);
    char p = 0x30;
    f.read(&p, 1);
    return u8(p);
}

// Writes an RGBA8 screen as a binary PPM (debugging aid for DOT_DUMP_FRAMES).
static void write_ppm(const std::filesystem::path& path, const std::vector<u32>& px, u32 w, u32 h) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (u32 p : px) f.put(char(p)).put(char(p >> 8)).put(char(p >> 16));
}

// Bottom screen placeholder until the game renders: the title's own SMDH icon
// (48x48 tiled RGB565 at 0x24C0), scaled 3x and centred. Exercises the PICA texture
// decoder and the Vulkan present path with real data.
static std::vector<u32> icon_screen(const std::filesystem::path& smdh_path) {
    using namespace gfx::vulkan;
    std::vector<u32> screen(BOTTOM_W * SCREEN_H, 0xFF201010);
    std::ifstream f(smdh_path, std::ios::binary);
    std::vector<u8> smdh{std::istreambuf_iterator<char>(f), {}};
    if (smdh.size() < 0x36C0 || std::string_view(reinterpret_cast<char*>(smdh.data()), 4) != "SMDH")
        return screen;
    auto icon = gfx::pica::decode_texture(std::span(smdh).subspan(0x24C0), 48, 48,
                                          gfx::pica::TexFormat::RGB565);
    const u32 ox = (BOTTOM_W - 144) / 2, oy = (SCREEN_H - 144) / 2;
    for (u32 y = 0; y < 144; ++y)
        for (u32 x = 0; x < 144; ++x) screen[(oy + y) * BOTTOM_W + ox + x] = icon[(y / 3) * 48 + x / 3];
    return screen;
}

int main(int argc, char** argv) {
    const std::filesystem::path data = argc > 1 ? argv[1] : "game_data";

    core::Memory mem;
    auto code = core::load_code(mem, data);
    if (!code || !hle::fs::MountRom(data / "romfs")) {
        std::fprintf(stderr, "game_data not found or incomplete at '%s'. Run tools/extract_cia.py first.\n",
                     data.string().c_str());
        return 1;
    }
    std::printf("loaded .text %#x+%#x .rodata %#x+%#x .data %#x+%#x .bss %#x, entry %#x\n",
                code->text.addr, code->text.size, code->rodata.addr, code->rodata.size,
                code->data.addr, code->data.size, code->bss_size, code->text.addr);

    hle::svc::InitProcess(mem, code->image_bytes());
    hle::svc::AddRegion(code->text.addr, code->text.pages * core::Memory::PAGE, 5, 4);  // r-x code
    hle::svc::AddRegion(code->rodata.addr, code->rodata.pages * core::Memory::PAGE, 1, 4);
    hle::svc::AddRegion(code->data.addr, (code->data.size + code->bss_size + 0xFFF) & ~0xFFFu, 3, 4);
    const u32 stack = (code->stack_size + 0xFFF) & ~0xFFFu;
    hle::svc::AddRegion(core::Memory::STACK_TOP - stack, stack, 3, 5);
    hle::dsp::Initialize();

#ifdef DOT_HAVE_RECOMP
    // The recompiled code.bin runs on guest threads (hle::kernel); this host thread keeps
    // the window, input and presentation. Whatever the guest needs next and the runtime
    // does not provide yet stops it with a report: that is the next thing to implement.
    recomp::set_memory(mem.host(0));
    recomp::set_svc_memory(mem);
    recomp::set_function_table(recomp::kFunctions, recomp::kFunctionCount);
    hle::kernel::init(mem);
    hle::service::register_all(mem, data);
    const u8 main_priority = read_main_priority(data);
    std::printf("boot: running recompiled code (%u functions), main thread priority 0x%02X\n",
                recomp::kFunctionCount, main_priority);
    hle::kernel::start_main_thread(code->text.addr, core::Memory::STACK_TOP, main_priority);
#else
    // No recompiled code (game_data absent at configure time): run the hand-transcribed
    // boot until it reaches code that is not transcribed yet.
    game::bind(mem);
    try {
        game::crt0::_start();
    } catch (const game::Frontier& f) {
        std::printf("boot: reached the transcription frontier: %s\n", f.what());
    }
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) || !hle::hid::Initialize()) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // Top screen 400x240 stacked over the 320x240 bottom screen, 2x scale.
    // DOT_WINDOW=<w>x<h> picks the initial window size (e.g. 1920x1080 with DOT_WIDESCREEN=window).
    int win_w = 800, win_h = 960;
    if (const char* e = std::getenv("DOT_WINDOW")) std::sscanf(e, "%dx%d", &win_w, &win_h);
    SDL_Window* window = SDL_CreateWindow("DawnOfTermina", std::max(win_w, 160), std::max(win_h, 120),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    {  // scope: the Vulkan surface must be destroyed before the window
        gfx::vulkan::Context gpu;
        if (!gpu.init(window)) return 1;
#ifdef DOT_HAVE_RECOMP
        gfx::vulkan::Renderer renderer(gpu);
        if (!renderer.init(std::filesystem::absolute(data).parent_path() / "cache")) return 1;
        hle::service::gsp::set_renderer(&renderer);
#endif
        const std::vector<u32> top(gfx::vulkan::TOP_W * gfx::vulkan::SCREEN_H, 0xFF402010);
        const std::vector<u32> bottom = icon_screen(data / "exefs" / "icon.bin");
        std::vector<u32> game_top, game_bottom;

        // Until the game loop is transcribed (stage 5), pump input, log pad changes and
        // present placeholder screens.
        bool reported = false;
#ifdef DOT_HAVE_RECOMP
        // Periodic guest work (VBlank, HID sampling) is due on its own clock, not the display's:
        // an idle guest only advances through host_tick, and DOT_SPEED needs it above 60 Hz.
        std::atomic<bool> ticking{true};
        std::thread ticker([&] {
            while (ticking.load(std::memory_order_relaxed)) {
                hle::kernel::host_tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
#endif
        for (bool running = true; running;) {
            for (SDL_Event e; SDL_PollEvent(&e);)
                if (e.type == SDL_EVENT_QUIT) running = false;
                else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_TAB && !e.key.repeat)
                    gfx::g_show_bottom = !gfx::g_show_bottom;  // widescreen layout: bottom screen overlay
                else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11 && !e.key.repeat)
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
#ifdef DOT_HAVE_RECOMP
            if (auto st = reported ? std::nullopt : hle::kernel::stopped()) {
                reported = true;
                if (st->exited) std::printf("guest: process exited\n");
                else std::printf("guest: stopped: %s\n", st->error.c_str());
                std::fflush(stdout);
                if (std::getenv("DOT_EXIT_ON_STOP")) running = false;
            }
#endif
            hle::hid::Update();
            auto pad = hle::hid::ReadPad();
            if (pad.trigger || pad.release)
                std::printf("pad hold=%08x stick=(%d,%d)\n", pad.hold, pad.stick_x, pad.stick_y);
#ifdef DOT_HAVE_RECOMP
            if (std::getenv("DOT_STATS")) {  // once per second: what the guest GPU path is doing
                static u64 frame = 0;
                if (++frame % 60 == 0) {
                    const auto st = hle::service::gsp::stats();
                    static u64 last_vblanks = ~0ull;
                    static int still = 0;
                    if (st.vblanks == last_vblanks && ++still == 3)
                        std::printf("guest is not progressing:\n%s", hle::kernel::dump_threads().c_str());
                    if (st.vblanks != last_vblanks) still = 0;
                    last_vblanks = st.vblanks;
                    const auto rs = renderer.stats();
                    std::printf("gsp: vblanks %llu, frames %llu, command lists %llu, draws %llu, fills %llu, transfers %llu | "
                                "rendered %llu, skipped %llu, pipelines %llu, shaders %llu, readbacks %llu\n",
                                (unsigned long long)st.vblanks, (unsigned long long)st.frames, (unsigned long long)st.command_lists,
                                (unsigned long long)st.draws, (unsigned long long)st.memory_fills,
                                (unsigned long long)st.display_transfers, (unsigned long long)rs.draws,
                                (unsigned long long)rs.skipped, (unsigned long long)rs.pipelines,
                                (unsigned long long)rs.shaders,
                                (unsigned long long)rs.readbacks);
                }
            }
            // Screens the renderer produced on the GPU are presented from there; otherwise from memory.
            gfx::vulkan::GpuScreen gpu_screens[2];
            const bool gpu_top = hle::service::gsp::gpu_screen(0, gpu_screens[0]);
            const bool gpu_bottom = hle::service::gsp::gpu_screen(1, gpu_screens[1]);
            const bool have_top = !gpu_top && hle::service::gsp::screen_image(0, game_top);
            const bool have_bottom = !gpu_bottom && hle::service::gsp::screen_image(1, game_bottom);
            if (const char* dir = std::getenv("DOT_DUMP_FRAMES"); dir && (have_top || gpu_top)) {
                static u32 n = 0;
                static const u32 every = std::max(1, std::atoi(std::getenv("DOT_DUMP_EVERY") ? std::getenv("DOT_DUMP_EVERY") : "120"));
                if (++n % every == 0) {
                    const std::string id = std::to_string(n / every);
                    std::vector<u32> px;
                    u32 w = 0, h = 0;
                    if (hle::service::gsp::screen_dump(0, px, w, h))
                        write_ppm(std::filesystem::path(dir) / ("top_" + id + ".ppm"), px, w, h);
                    if (hle::service::gsp::screen_dump(1, px, w, h))
                        write_ppm(std::filesystem::path(dir) / ("bottom_" + id + ".ppm"), px, w, h);
                    // Guest time of each dump, in the units of DOT_INPUT, to line scripts up with frames.
                    if (std::FILE* f = std::fopen((std::filesystem::path(dir) / "frames.txt").c_str(), "a")) {
                        std::fprintf(f, "%s %.1f\n", id.c_str(), hle::service::gsp::stats().vblanks / 59.83);
                        std::fclose(f);
                    }
                }
            }
            const u32* top_px = have_top ? game_top.data() : top.data();
            const u32* bottom_px = have_bottom ? game_bottom.data() : bottom.data();
            const gfx::vulkan::GpuScreen* gpu_top_ptr = gpu_top ? &gpu_screens[0] : nullptr;
            const gfx::vulkan::GpuScreen* gpu_bottom_ptr = gpu_bottom ? &gpu_screens[1] : nullptr;
#else
            const u32* top_px = top.data();
            const u32* bottom_px = bottom.data();
            const gfx::vulkan::GpuScreen* gpu_top_ptr = nullptr;
            const gfx::vulkan::GpuScreen* gpu_bottom_ptr = nullptr;
#endif
            if (!gpu.present({gfx::vulkan::TOP_W, gfx::vulkan::SCREEN_H, top_px},
                             {gfx::vulkan::BOTTOM_W, gfx::vulkan::SCREEN_H, bottom_px}, gpu_top_ptr, gpu_bottom_ptr))
                break;  // FIFO present paces the loop to vsync
        }
#ifdef DOT_HAVE_RECOMP
        ticking = false;
        ticker.join();
        hle::service::gsp::set_renderer(nullptr);
#endif
    }

#ifdef DOT_HAVE_RECOMP
    if (std::getenv("DOT_STATS")) std::printf("guest threads at exit:\n%s", hle::kernel::dump_threads().c_str());
#endif
    SDL_DestroyWindow(window);
    hle::dsp::Shutdown();
    SDL_Quit();
    std::fflush(stdout);
    // Guest threads may still be parked inside the kernel: skip static destructors.
    std::_Exit(0);
}
