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
#include <regex>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/loader.h"
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
#include "recomp/hooks.h"
#include "ui/menu.h"

namespace recomp {
extern const FuncEntry kFunctions[];
extern const u32 kFunctionCount;
extern const FuncEntry kResumePoints[];
extern const u32 kResumePointCount;
}  // namespace recomp

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

// Shown as a dialog too: launched from a file manager there is no terminal to read.
// _Exit: guest threads may already be parked in the kernel (no static destructors).
[[noreturn]] static int fatal(const std::string& msg) {
    std::fprintf(stderr, "%s\n", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DawnOfTermina", msg.c_str(), nullptr);
    std::fflush(stdout);
    std::_Exit(1);
}

// game_data is usable when extract_cia.py finished it (manifest.json, written last) for v1.1
// and its code.bin is the one this executable was recompiled from (DOT_CODE_SHA256).
enum class GameData { Missing, Ready, OtherBuild };
static GameData check_game_data(const std::filesystem::path& d, std::string* sha = nullptr) {
    std::error_code ec;
    for (const char* f : {"exheader.bin", "exefs/code.bin", "romfs.bin", "update_romfs.bin", "manifest.json"})
        if (!std::filesystem::exists(d / f, ec)) return GameData::Missing;
    std::ifstream f(d / "manifest.json");
    const std::string m{std::istreambuf_iterator<char>(f), {}};
    std::smatch version, code;
    if (!std::regex_search(m, version, std::regex(R"re("game_version"\s*:\s*"([^"]*)")re")) || version[1] != "1.1" ||
        !std::regex_search(m, code, std::regex(R"re("code_sha256"\s*:\s*"([0-9a-f]{64})")re")))
        return GameData::Missing;
    if (sha) *sha = code[1];
    return code[1] == DOT_CODE_SHA256 ? GameData::Ready : GameData::OtherBuild;
}

[[noreturn]] static int other_build(const std::filesystem::path& data, const std::string& sha) {
    fatal("The game data at '" + data.string() + "' (code.bin " + sha.substr(0, 12) +
          "...) is not the one this executable was built from (" + std::string(DOT_CODE_SHA256).substr(0, 12) +
          "...).\nThe game code is recompiled at build time: rebuild with\n"
          "  cmake -B build -DDOT_GAME_DATA=" + data.string() + " && cmake --build build\n"
          "or start this executable with the game_data it was built for.");
}

int main(int argc, char** argv) {
    // game_data: the argument, else the working directory, else next to the executable, so
    // launching from a file manager works. The root is the folder whose tools/ the executable
    // uses: its own (a dist/ install) or its parent's (build/ in a source checkout).
    namespace fs = std::filesystem;
    const fs::path base = SDL_GetBasePath() ? fs::path(SDL_GetBasePath()) : fs::current_path();
    const fs::path root = !fs::exists(base / "tools" / "extract_cia.py") && fs::exists(base / ".." / "tools" / "extract_cia.py")
                              ? (base / "..").lexically_normal()
                              : base;
    // usage: dawnoftermina [game_data] [--base <game.cia> --update <update_v1.1.cia>]
    fs::path data;
    ui::SetupFiles setup;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "--base" || a == "--update") && i + 1 < argc) (a == "--base" ? setup.base : setup.update) = argv[++i];
        else if (a.starts_with("--")) return fatal("usage: dawnoftermina [game_data] [--base <game.cia> --update <update_v1.1.cia>]");
        else data = a;
    }
    if (data.empty()) data = check_game_data("game_data") != GameData::Missing ? fs::path("game_data") : root / "game_data";
    std::string sha;
    GameData state = check_game_data(data, &sha);
    if (state == GameData::OtherBuild) other_build(data, sha);
    if (state == GameData::Missing) {
        // No v1.1 game data: the launcher requires the base game and the v1.1 update (.cia),
        // given on the command line or picked there (offering ones found in ISO/).
        if (!SDL_Init(SDL_INIT_VIDEO)) return fatal(std::string("SDL_Init: ") + SDL_GetError());
        setup.start = !setup.base.empty() && !setup.update.empty();
        std::error_code ec;
        for (const fs::path& dir : {root / "ISO", root, fs::current_path() / "ISO"})
            for (const auto& f : fs::directory_iterator(dir, ec)) {
                if (f.path().extension() != ".cia") continue;
                std::string name = f.path().filename().string();
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                // shortcut: guessed by file name (the extractor checks the title IDs), offered only
                fs::path& slot = name.find("update") != std::string::npos ? setup.update : setup.base;
                if (slot.empty()) slot = f.path();
            }
        if (!ui::run_setup(data, root / "tools" / "extract_cia.py", setup)) return 0;
        state = check_game_data(data, &sha);
        if (state == GameData::OtherBuild) other_build(data, sha);
        if (state != GameData::Ready) return fatal("The extraction at '" + data.string() + "' is incomplete.");
    }
    gfx::g_hd_hud = fs::exists(data / "romfs" / "layout" / "Joker.Main.New.gar");  // extract_cia.py --romfs
    // config.toml lives next to game_data/ (like saves/ and cache/); DOT_* variables override it.
    const std::filesystem::path config_file = std::filesystem::absolute(data).parent_path() / "config.toml";
    core::Config config = core::load_config(config_file);
    // DekUI's single-screen layout: its defaults, or a presentation.json (DekUI's format) next to config.toml.
    const auto presentation = config_file.parent_path() / "presentation.json";
    recomp::hooks::dekui_load_presentation(std::filesystem::exists(presentation) ? presentation.c_str() : nullptr);
    core::apply_env_defaults(config);

    core::Memory mem;
    auto code = core::load_code(mem, data);
    if (!code || !hle::fs::MountRom(data / "romfs")) {
        return fatal("game_data not found or incomplete at '" + data.string() + "'. Run tools/extract_cia.py first.");
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

    // The recompiled code.bin runs on guest threads (hle::kernel); this host thread keeps
    // the window, input and presentation. Whatever the guest needs next and the runtime
    // does not provide yet stops it with a report: that is the next thing to implement.
    recomp::set_memory(mem.host(0));
    recomp::set_svc_memory(mem);
    recomp::set_function_table(recomp::kFunctions, recomp::kFunctionCount);
    recomp::set_resume_table(recomp::kResumePoints, recomp::kResumePointCount);
    hle::kernel::init(mem);
    hle::service::register_all(mem, data);
    const u8 main_priority = read_main_priority(data);
    std::printf("boot: running recompiled code (%u functions), main thread priority 0x%02X\n",
                recomp::kFunctionCount, main_priority);
    // DOT_LOAD_STATE=<file>: the main thread loads it at its first syscall.
    if (const char* st = std::getenv("DOT_LOAD_STATE")) hle::kernel::request_load(st);
    hle::kernel::start_main_thread(code->text.addr, core::Memory::STACK_TOP, main_priority);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) || !hle::hid::Initialize()) {
        return fatal(std::string("SDL_Init: ") + SDL_GetError());
    }
    // Initial window size: config.toml resolution, or DOT_WINDOW=<w>x<h>.
    int win_w = 800, win_h = 960;
    if (const char* e = std::getenv("DOT_WINDOW")) std::sscanf(e, "%dx%d", &win_w, &win_h);
    SDL_Window* window = SDL_CreateWindow("DawnOfTermina", std::max(win_w, 160), std::max(win_h, 120),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) {
        return fatal(std::string("SDL_CreateWindow: ") + SDL_GetError());
    }

    {  // scope: the Vulkan surface must be destroyed before the window
        gfx::vulkan::Context gpu;
        if (!gpu.init(window)) return fatal("Vulkan initialisation failed (see the terminal output). A GPU driver with Vulkan 1.1 is required.");
        if (!ui::init(window, gpu, config, config_file)) std::fprintf(stderr, "ui: settings menu unavailable\n");
        gfx::vulkan::Renderer renderer(gpu);
        if (!renderer.init(std::filesystem::absolute(data).parent_path() / "cache"))
            return fatal("Renderer initialisation failed (see the terminal output).");
        hle::service::gsp::set_renderer(&renderer);
        ui::set_renderer(&renderer, &mem);
        const std::vector<u32> top(gfx::vulkan::TOP_W * gfx::vulkan::SCREEN_H, 0xFF402010);
        const std::vector<u32> bottom = icon_screen(data / "exefs" / "icon.bin");
        std::vector<u32> game_top, game_bottom;

        // Until the game loop is transcribed (stage 5), pump input, log pad changes and
        // present placeholder screens.
        bool reported = false;
        // Periodic guest work (VBlank, HID sampling) is due on its own clock, not the display's:
        // an idle guest only advances through host_tick, and DOT_SPEED needs it above 60 Hz.
        std::atomic<bool> ticking{true};
        std::thread ticker([&] {
            while (ticking.load(std::memory_order_relaxed)) {
                hle::kernel::host_tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        for (bool running = true; running;) {
            for (SDL_Event e; SDL_PollEvent(&e);)
                if (e.type == SDL_EVENT_QUIT) running = false;
                else ui::handle_event(e);  // menu and hotkeys; the game polls the device state itself
            if (auto st = reported ? std::nullopt : hle::kernel::stopped()) {
                reported = true;
                if (st->exited) std::printf("guest: process exited\n");
                else std::printf("guest: stopped: %s\n", st->error.c_str());
                std::fflush(stdout);
                // Without a terminal a stopped guest looked like a frozen frame: say so on screen.
                if (!st->exited && !std::getenv("DOT_EXIT_ON_STOP")) {
                    SDL_SetWindowTitle(window, "DawnOfTermina - stopped (emulation error)");
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "DawnOfTermina: the game stopped",
                                             ("Emulation error (please report it):\n\n" + st->error).c_str(), window);
                }
                if (std::getenv("DOT_EXIT_ON_STOP")) running = false;
            }
            hle::hid::Update();
            auto pad = hle::hid::ReadPad();
            if (pad.trigger || pad.release)
                std::printf("pad hold=%08x stick=(%d,%d)\n", pad.hold, pad.stick_x, pad.stick_y);
            // N64 layout: bit 0 of 0x6A3A18 (v1.0) = the world is paused, written each frame by the
            // play loop (0x15F644) from the touch-screen menu state; set with the Items/Masks menus,
            // clear through boot, title and cutscenes (savestate diffs + trace). Same .data offset in
            // v1.0, v1.1 and Project Restoration (2 code references each).
            // DekUI's single-screen modules draw every bottom-screen menu and the minimap on the top
            // screen: no pop-up and no corner pieces (F3 still forces the pop-up).
            const bool dekui = recomp::hooks::dekui_active();
            gfx::g_bottom_modal = (mem.read<u32>(code->data.addr + 0x58A18) & 1) && !dekui;
            // Halfword at 0x68D300 (v1.0, 31 code references; same offset in the other versions):
            // 0 only while the bottom screen shows the gameplay HUD (1 through boot, title, opening
            // cutscenes and menus). The HUD pieces are hidden otherwise.
            gfx::g_hud_visible = mem.read<u16>(code->data.addr + 0x42300) == 0 && !dekui;
            ui::frame();  // before fetching the screens: a resolution change destroys their images
            // DOT_SAVE_STATE_AT=<guest seconds> (DOT_INPUT's clock): one automatic save to states/slot1.state.
            if (static const char* at = std::getenv("DOT_SAVE_STATE_AT"); at) {
                static bool saved = false;
                if (!saved && double(hle::service::gsp::stats().vblanks) / 59.83 >= std::atof(at)) {
                    saved = true;
                    hle::kernel::request_save(std::filesystem::absolute(data).parent_path() / "states" / "slot1.state");
                }
            }
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
                                "rendered %llu, skipped %llu, pipelines %llu, shaders %llu, readbacks %llu, interpolated %llu/%llu (camera draws %llu), "
                                "presents smooth %llu repeated %llu skipped %llu, uv wrapped %llu stepped %llu\n",
                                (unsigned long long)st.vblanks, (unsigned long long)st.frames, (unsigned long long)st.command_lists,
                                (unsigned long long)st.draws, (unsigned long long)st.memory_fills,
                                (unsigned long long)st.display_transfers, (unsigned long long)rs.draws,
                                (unsigned long long)rs.skipped, (unsigned long long)rs.pipelines,
                                (unsigned long long)rs.shaders,
                                (unsigned long long)rs.readbacks, (unsigned long long)rs.interpolated,
                                (unsigned long long)(rs.interpolated + rs.not_interpolated),
                                (unsigned long long)rs.camera_draws, (unsigned long long)rs.smooth,
                                (unsigned long long)rs.repeated, (unsigned long long)rs.skipped_shots,
                                (unsigned long long)rs.uv_wrapped, (unsigned long long)rs.uv_stepped);
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
                    // DOT_DUMP_INTERP: also the frame-interpolation in-between image (DOT_FPS>=60).
                    if (std::getenv("DOT_DUMP_INTERP") && hle::service::gsp::screen_dump(0, px, w, h, true))
                        write_ppm(std::filesystem::path(dir) / ("interp_" + id + ".ppm"), px, w, h);
                    if (hle::service::gsp::screen_dump(1, px, w, h))
                        write_ppm(std::filesystem::path(dir) / ("bottom_" + id + ".ppm"), px, w, h);
                    // DOT_DUMP_WINDOW: also the composed window (layout, HUD, menu), as presented.
                    if (std::getenv("DOT_DUMP_WINDOW"))
                        gpu.dump_next_frame((std::filesystem::path(dir) / ("window_" + id + ".ppm")).string());
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
            if (!config.vsync) {  // FIFO paces the loop with vsync; without it, cap at 240 Hz
                static auto next = std::chrono::steady_clock::now();
                std::this_thread::sleep_until(next);
                next = std::max(next + std::chrono::microseconds(4167), std::chrono::steady_clock::now());
            }
            if (!gpu.present({gfx::vulkan::TOP_W, gfx::vulkan::SCREEN_H, top_px},
                             {gfx::vulkan::BOTTOM_W, gfx::vulkan::SCREEN_H, bottom_px}, gpu_top_ptr, gpu_bottom_ptr))
                break;  // FIFO present paces the loop to vsync
        }
        ui::shutdown();
        ticking = false;
        ticker.join();
        hle::service::gsp::set_renderer(nullptr);
    }

    if (std::getenv("DOT_STATS")) std::printf("guest threads at exit:\n%s", hle::kernel::dump_threads().c_str());
    SDL_DestroyWindow(window);
    hle::dsp::Shutdown();
    SDL_Quit();
    std::fflush(stdout);
    // Guest threads may still be parked inside the kernel: skip static destructors.
    std::_Exit(0);
}
