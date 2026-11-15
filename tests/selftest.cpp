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

// Assert-based self-check for the runtime core. Uses game_data/ when present
// (path in argv[1]); everything else runs on synthetic data.

#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/config.h"
#include "core/loader.h"
#include "game/crt0.h"
#include "game/guest.h"
#include "hle/fs.h"
#include "gfx/camera_midpoint.h"
#include "gfx/layout.h"
#include "hle/hid.h"
#include "hle/svc.h"
#include "mm3d/dungeon_map.h"
#include "recomp/hooks.h"

using core::Memory;
namespace stdfs = std::filesystem;

static void test_memory_and_svc(Memory& mem) {
    mem.write<u32>(Memory::VRAM_BASE, 0xDEADBEEF);
    assert(mem.read<u32>(Memory::VRAM_BASE) == 0xDEADBEEF);
    assert(!mem.map(Memory::HEAP_BASE + 1, Memory::PAGE));  // misaligned

    u32 addr = 0;
    namespace svc = hle::svc;
    assert(svc::ControlMemory(mem, &addr, Memory::HEAP_BASE, 0, 0x10000, svc::MEMOP_COMMIT, 3) == RESULT_OK);
    assert(addr == Memory::HEAP_BASE && mem.read<u32>(addr) == 0);
    mem.write<u32>(addr + 0xFFFC, 42);
    assert(svc::ControlMemory(mem, &addr, 0, 0, 0x2000, svc::MEMOP_COMMIT | svc::MEMOP_LINEAR, 3) == RESULT_OK);
    assert(addr == Memory::LINEAR_BASE);
    assert(svc::ControlMemory(mem, &addr, 0, 0, 0x1000, svc::MEMOP_COMMIT | svc::MEMOP_LINEAR, 3) == RESULT_OK);
    assert(addr == Memory::LINEAR_BASE + 0x2000);
    assert(svc::ControlMemory(mem, &addr, Memory::HEAP_BASE, 0, 0x10000, svc::MEMOP_FREE, 0) == RESULT_OK);
    assert(svc::ControlMemory(mem, &addr, Memory::HEAP_BASE, 0, 0x10000, svc::MEMOP_COMMIT, 3) == RESULT_OK);
    assert(mem.read<u32>(addr + 0xFFFC) == 0);  // freed pages come back zeroed
    assert(failed(svc::ControlMemory(mem, &addr, 0x1000, 0, 0x1000, svc::MEMOP_COMMIT, 3)));
    assert(failed(svc::ControlMemory(mem, &addr, 0, 0, Memory::FCRAM_SIZE, svc::MEMOP_COMMIT | svc::MEMOP_LINEAR, 3)));

    const u64 t0 = svc::GetSystemTick();
    svc::SleepThread(2'000'000);
    assert(svc::GetSystemTick() - t0 >= 268111856ull / 1000);  // >= 1 ms of ticks
}

static void test_fs() {
    const stdfs::path root = stdfs::temp_directory_path() / "dot_selftest_romfs";
    stdfs::create_directories(root / "actors");
    std::ofstream(root / "actors" / "a.bin", std::ios::binary) << "HELLO";
    assert(hle::fs::MountRom(root));

    Handle h = 0;
    assert(hle::fs::OpenFile(&h, "rom:/actors/a.bin") == RESULT_OK);
    u64 size = 0;
    assert(hle::fs::GetSize(h, &size) == RESULT_OK && size == 5);
    char buf[8] = {};
    u32 n = 0;
    assert(hle::fs::Read(h, 1, buf, 8, &n) == RESULT_OK && n == 4 && std::string_view(buf, 4) == "ELLO");
    assert(hle::fs::Close(h) == RESULT_OK);
    assert(hle::fs::Close(h) == hle::fs::ERR_INVALID_HANDLE);
    assert(hle::fs::OpenFile(&h, "rom:/missing") == hle::fs::ERR_NOT_FOUND);
    assert(hle::fs::OpenFile(&h, "rom:/../etc/passwd") == hle::fs::ERR_INVALID_PATH);
    stdfs::remove_all(root);
}

static void test_hid() {
    using namespace hle::hid;
    PadStatus p = next_pad({}, A | L, 0, 0);
    assert(p.hold == (A | L) && p.trigger == (A | L) && p.release == 0);
    p = next_pad(p, A, CPAD_MAX, -50);
    assert(p.hold == (A | CPAD_RIGHT | CPAD_DOWN) && p.trigger == (CPAD_RIGHT | CPAD_DOWN) && p.release == L);
    p = next_pad(p, A, 10, 0);  // inside the dead zone: no direction bits
    assert(p.hold == A && p.stick_x == 10);

    u16 tx, ty;  // dual screen, 800x960 window: scale 2, bottom screen spans x 80..719, y 480..959
    gfx::g_single_screen = false;
    assert(window_to_touch(80, 480, 800, 960, tx, ty) && tx == 0 && ty == 0);
    assert(window_to_touch(719.9f, 959.9f, 800, 960, tx, ty) && tx == 319 && ty == 239);
    assert(!window_to_touch(79, 600, 800, 960, tx, ty) && !window_to_touch(400, 479, 800, 960, tx, ty));
    assert(window_to_touch(200 + 80, 480, 1200, 960, tx, ty) && tx == 0);  // pillarboxed by 200px
    gfx::g_top_width = 240.0f * 16 / 9;  // dual + widescreen: 960x540 top at x 480, bottom at (600, 540)
    assert(window_to_touch(600, 540, 1920, 1080, tx, ty) && tx == 0 && ty == 0);
    // Single screen (16:9 top screen filling 1920x1080): bottom overlay 432x324 at (1488, 756).
    gfx::g_single_screen = true;
    assert(window_to_touch(1488, 756, 1920, 1080, tx, ty) && tx == 0 && ty == 0);
    assert(window_to_touch(1919.9f, 1079.9f, 1920, 1080, tx, ty) && tx == 319 && ty == 239);
    assert(!window_to_touch(1400, 900, 1920, 1080, tx, ty));
    gfx::g_show_bottom = false;
    assert(!window_to_touch(1700, 900, 1920, 1080, tx, ty));
    gfx::g_show_bottom = true, gfx::g_top_width = 400.0f;
    assert(window_to_touch(1488, 756, 1920, 1080, tx, ty) && tx == 0);  // 5:3 top pillarboxed, same overlay
    // N64 layout, 16:9 top filling 1920x1080 (4.5 px per top row, HUD scale 0.6 -> 2.7 px per
    // bottom pixel, margin 27): item II (53x53 from (265,185)) at (1749.9, 97.2), 143.1 wide.
    gfx::g_n64 = true, gfx::g_top_width = 240.0f * 16 / 9;
    assert(window_to_touch(1749.9f + 71.5f, 97.2f + 71.5f, 1920, 1080, tx, ty) && tx == 291 && ty == 211);
    assert(!window_to_touch(960, 540, 1920, 1080, tx, ty));  // game view: no touch
    gfx::g_hud_visible = false;  // cutscene: no HUD pieces
    assert(!window_to_touch(1749.9f + 71.5f, 97.2f + 71.5f, 1920, 1080, tx, ty));
    gfx::g_hud_visible = true;
    gfx::g_minimap = false;  // minimap (192x204 at 0.75 scale) right-bottom, hidden by Tab
    assert(!window_to_touch(1920 - 30, 1080 - 27 - 110 * 2.025f - 30, 1920, 1080, tx, ty));
    gfx::g_minimap = true;
    assert(window_to_touch(1920 - 30, 1080 - 27 - 110 * 2.025f - 30, 1920, 1080, tx, ty) && tx > 240 && ty > 220);
    gfx::g_bottom_modal = true;  // menu pop-up: 1152x864 centred at (384, 108)
    assert(window_to_touch(384, 108, 1920, 1080, tx, ty) && tx == 0 && ty == 0);
    assert(window_to_touch(1535.9f, 971.9f, 1920, 1080, tx, ty) && tx == 319 && ty == 239);
    assert(!window_to_touch(1821, 168, 1920, 1080, tx, ty));  // HUD pieces are gone
    gfx::g_bottom_modal = false, gfx::g_n64 = false, gfx::g_top_width = 400.0f;

    // config.toml round trip
    core::Config c = core::parse_config("[display]\nresolution = \"720p\" # x\nvsync = false\nfps = 30\n"
                                        "[audio]\nvolume = 150\n[keys]\nA = \"K\"\n");
    assert(c.resolution == "720p" && !c.vsync && c.fps == 30 && c.volume == 100 && c.keys["A"] == "K");
    assert(c.layout == "n64" && c.widescreen == "16:9" && c.minimap && c.hud_scale == 0.6f);  // defaults kept
    const core::Config d = core::parse_config(core::format_config(c));
    assert(d.resolution == c.resolution && d.vsync == c.vsync && d.fps == c.fps && d.keys == c.keys &&
           d.layout == c.layout && d.hud_scale == c.hud_scale && d.hud_opacity == c.hud_opacity && d.minimap == c.minimap);

    std::vector<ScriptEvent> ev;
    assert(parse_input_script("20:START,21.5:A,30:T160x120", ev) && ev.size() == 3);
    assert(ev[0].buttons == START && ev[1].t == 21.5 && ev[1].buttons == A);
    assert(ev[2].touch && ev[2].x == 160 && ev[2].y == 120 && !ev[2].buttons);
    assert(parse_input_script("40+2.5:CP-156x100,41+1:A", ev) && ev.size() == 2);
    assert(ev[0].stick && ev[0].sx == -156 && ev[0].sy == 100 && ev[0].t == 40 && ev[0].duration == 2.5);
    assert(ev[1].buttons == A && ev[1].duration == 1 && !ev[1].stick);
    assert(parse_input_script("7:CS156x0", ev) && ev[0].c_stick && !ev[0].stick && ev[0].sx == 156);
    assert(!parse_input_script("5:CP200x0", ev));
    assert(!parse_input_script("5:JUMP", ev) && !parse_input_script("5:T400x10", ev) && !parse_input_script("x", ev));
}

static void test_loader(Memory& mem, const stdfs::path& data) {
    if (!stdfs::exists(data / "exefs" / "code.bin")) {
        std::puts("selftest: game_data not present, loader test skipped");
        return;
    }
    auto cs = core::load_code(mem, data);
    assert(cs && cs->text.addr == 0x00100000);
    assert(mem.read<u32>(cs->text.addr) == 0xEB000007);  // _start: bl __clear_bss
    assert(mem.read<u32>(cs->data.addr + cs->data.size) == 0);  // .bss starts zeroed
}

// Runs the transcribed boot on a fresh address space and checks how far it gets.
// Update the expected frontier whenever the boot advances.
static void test_boot(const stdfs::path& data) {
    if (!stdfs::exists(data / "exefs" / "code.bin")) return;
    Memory mem;
    auto cs = core::load_code(mem, data);
    assert(cs);
    hle::svc::InitProcess(mem, cs->image_bytes());
    game::bind(mem);
    u32 frontier = 0;
    try {
        game::crt0::_start();
    } catch (const game::Frontier& f) {
        frontier = f.va;
    }
    namespace va = game::crt0::va;
    assert(frontier == 0x0011BECC);
    assert(game::r32(va::CLIB_LOCALE_STATE + 4) == va::CTYPE_TABLE_C + 1);
    assert(game::r32(va::CLIB_LOCALE_STATE + 0xC) == va::CASE_TABLE_C);
    assert(game::r32(va::SRV_REFCOUNT) == 1);
    const u32 heap = game::r32(va::HEAP_SIZE);
    assert(heap > (16u << 20) && heap % Memory::PAGE == 0);
    assert(heap + cs->image_bytes() + game::crt0::DEVICE_MEMORY_SIZE <= 0x04000000);
    game::w32(Memory::HEAP_BASE + heap - 4, 1);  // whole heap is mapped
    std::printf("selftest: boot frontier 0x%08X, heap %u KiB\n", frontier, heap >> 10);
}

// Interpolated camera (gfx/camera_midpoint.h): the midpoint of two yaws is the half yaw, rigid,
// translation averaged; cuts by distance and angle.
static void test_camera() {
    using namespace gfx::camera;
    auto yaw = [](double deg, double x) {
        const double r = deg * 3.14159265358979323846 / 180;
        return Mat{{{std::cos(r), 0, std::sin(r), x}, {0, 1, 0, 5}, {-std::sin(r), 0, std::cos(r), 0}}};
    };
    const Mat a = yaw(0, 0), b = yaw(40, 100), m = midpoint(a, b), want = yaw(20, 50);
    assert(rigid(a) && rigid(m));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) assert(std::abs(m[i][j] - want[i][j]) < 1e-9);
    const Mat id = mul(inverse_rigid(b), b);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) assert(std::abs(id[i][j] - (i == j)) < 1e-9);
    assert(!cut(a, b) && cut(a, yaw(50, 0)) && cut(a, yaw(0, 300)));
    Mat scaled = a;
    scaled[0][0] = 2;
    assert(!rigid(scaled));
}

// DekUI presentation settings (src/recomp/dekui_settings.cpp): the built-in presentation.json
// parses and reaches the modules; a user file overrides it; a malformed one is refused.
static void test_dekui_presentation() {
    using recomp::hooks::dekui_load_presentation;
    assert(dekui_load_presentation(nullptr));
    assert(std::abs(MM3D::DungeonMap::poses[0].x.load() - 40.2f) < 1e-3f);  // "dungeon_map_0_x": 40.2
    assert(MM3D::DungeonMap::enabled.load());                                // "dungeon_map_top": true
    const auto path = stdfs::temp_directory_path() / "dot_presentation_test.json";
    std::ofstream(path) << R"({"version": 1, "fullscreen_draws": [{"a": [1, "]"]}], "dungeon_map_0_x": -12.5,
                              "dungeon_map_top": false, "note": "x\"y"})";
    assert(dekui_load_presentation(path.c_str()));
    assert(MM3D::DungeonMap::poses[0].x.load() == -12.5f && !MM3D::DungeonMap::enabled.load());
    for (const char* bad : {R"({"version": 2})", R"({"version": 1, "x": })", "[1]", R"({"version": 1)"}) {
        std::ofstream(path) << bad;
        assert(!dekui_load_presentation(path.c_str()));
    }
    stdfs::remove(path);
    assert(dekui_load_presentation(nullptr));
}

void test_pica();                     // tests/pica_test.cpp
void test_shaders(const char* data);  // tests/shader_test.cpp
void test_recomp_runtime();            // tests/recomp_test.cpp
void test_audio();                     // tests/audio_test.cpp

int main(int argc, char** argv) {
    Memory mem;
    test_memory_and_svc(mem);
    test_fs();
    test_hid();
    test_camera();
    test_dekui_presentation();
    test_pica();
    test_recomp_runtime();
    test_audio();
    test_loader(mem, argc > 1 ? argv[1] : "game_data");
    test_boot(argc > 1 ? argv[1] : "game_data");
    test_shaders(argc > 1 ? argv[1] : "game_data");
    std::puts("selftest: OK");
    return 0;
}
