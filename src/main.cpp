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
#include <filesystem>

#include "core/loader.h"
#include "hle/dsp.h"
#include "hle/fs.h"
#include "hle/hid.h"
#include "hle/srv.h"

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

    hle::srv::Initialize();
    hle::dsp::Initialize();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) || !hle::hid::Initialize()) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // Top screen 400x240 stacked over the 320x240 bottom screen, 2x scale.
    SDL_Window* window = SDL_CreateWindow("DawnOfTermina", 800, 960, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    // Until the game loop is transcribed (stage 5), just pump input and log pad changes.
    for (bool running = true; running;) {
        for (SDL_Event e; SDL_PollEvent(&e);)
            if (e.type == SDL_EVENT_QUIT) running = false;
        hle::hid::Update();
        auto pad = hle::hid::ReadPad();
        if (pad.trigger || pad.release)
            std::printf("pad hold=%08x stick=(%d,%d)\n", pad.hold, pad.stick_x, pad.stick_y);
        SDL_Delay(16);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    hle::dsp::Shutdown();
    return 0;
}
