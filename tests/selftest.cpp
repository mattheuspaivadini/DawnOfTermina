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
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/loader.h"
#include "game/crt0.h"
#include "game/guest.h"
#include "hle/fs.h"
#include "hle/hid.h"
#include "hle/svc.h"

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

void test_pica();                     // tests/pica_test.cpp
void test_shaders(const char* data);  // tests/shader_test.cpp

int main(int argc, char** argv) {
    Memory mem;
    test_memory_and_svc(mem);
    test_fs();
    test_hid();
    test_pica();
    test_loader(mem, argc > 1 ? argv[1] : "game_data");
    test_boot(argc > 1 ? argv[1] : "game_data");
    test_shaders(argc > 1 ? argv[1] : "game_data");
    std::puts("selftest: OK");
    return 0;
}
