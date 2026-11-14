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

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <fstream>
#include <iterator>
#include <vector>

#include "core/loader.h"
#include "game/crt0.h"
#include "game/guest.h"
#include "gfx/pica/texture.h"
#include "gfx/vulkan/context.h"
#include "hle/dsp.h"
#include "hle/fs.h"
#include "hle/hid.h"
#include "hle/srv.h"
#include "hle/kernel.h"
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
    hle::service::register_all(data);
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
    SDL_Window* window = SDL_CreateWindow("DawnOfTermina", 800, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    {  // scope: the Vulkan surface must be destroyed before the window
        gfx::vulkan::Context gpu;
        if (!gpu.init(window)) return 1;
        const std::vector<u32> top(gfx::vulkan::TOP_W * gfx::vulkan::SCREEN_H, 0xFF402010);
        const std::vector<u32> bottom = icon_screen(data / "exefs" / "icon.bin");

        // Until the game loop is transcribed (stage 5), pump input, log pad changes and
        // present placeholder screens.
        bool reported = false;
        for (bool running = true; running;) {
            for (SDL_Event e; SDL_PollEvent(&e);)
                if (e.type == SDL_EVENT_QUIT) running = false;
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
            if (!gpu.present({gfx::vulkan::TOP_W, gfx::vulkan::SCREEN_H, top.data()},
                             {gfx::vulkan::BOTTOM_W, gfx::vulkan::SCREEN_H, bottom.data()}))
                break;  // FIFO present paces the loop to vsync
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    hle::dsp::Shutdown();
    return 0;
}
