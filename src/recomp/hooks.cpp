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

#include "recomp/hooks.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "hle/service/gsp.h"
#include "hle/svc.h"

// Only what callers can observe under AAPCS is reproduced: the result register and the
// memory written. Scratch registers (r1-r3, r12, flags) are left as they were.
// shortcut: errno (sinf/cosf of ±inf, underflow) is not set, and results may differ from the
// game's polynomials in the last ulp; transcribe the originals if a caller ever depends on that.
namespace recomp::hooks {

static u8* p(u32 va) { return g_base + va; }

// The original copies forward; memmove matches it for every non-overlapping or dst < src
// call. shortcut: an overlapping dst > src memcpy (undefined in C) is not replicated.
u32 aeabi_memcpy(Context* c) { std::memmove(p(c->r[0]), p(c->r[1]), c->r[2]); return c->r[14]; }
u32 memcpy_src_dst(Context* c) { std::memmove(p(c->r[1]), p(c->r[0]), c->r[2]); return c->r[14]; }
u32 aeabi_memcpy4(Context* c) { return aeabi_memcpy(c); }
u32 aeabi_memmove4(Context* c) { return aeabi_memcpy(c); }
u32 aeabi_memset(Context* c) { std::memset(p(c->r[0]), u8(c->r[2]), c->r[1]); return c->r[14]; }
u32 aeabi_memclr(Context* c) { std::memset(p(c->r[0]), 0, c->r[1]); return c->r[14]; }
u32 aeabi_memclr4(Context* c) { return aeabi_memclr(c); }

u32 strlen(Context* c) {
    c->r[0] = u32(std::strlen(reinterpret_cast<const char*>(p(c->r[0]))));
    return c->r[14];
}

// Same result as the game's: difference of the first differing bytes (only the sign is used).
u32 strcmp(Context* c) {
    const u8* a = p(c->r[0]);
    const u8* b = p(c->r[1]);
    while (*a && *a == *b) ++a, ++b;
    c->r[0] = u32(int(*a) - int(*b));
    return c->r[14];
}

u32 sinf(Context* c) { set_fs(c, 0, std::sin(fs(c, 0))); return c->r[14]; }
u32 cosf(Context* c) { set_fs(c, 0, std::cos(fs(c, 0))); return c->r[14]; }
u32 atan2f(Context* c) { set_fs(c, 0, std::atan2(fs(c, 0), fs(c, 1))); return c->r[14]; }

// ---- camera FOV, ported from DekUI (src/mm3d/camera_live.h, GPL-2.0-or-later) ----------------
// Addresses are the v1.0 layout (also Project Restoration's), where hooks.txt installs the hook.

std::atomic<float> fov{45.f}, cutscene_fov{45.f};

bool readable(u32 va, u32 bytes) {
    if (va < 0x100000 || va > 0x40000000 - bytes) return false;
    const hle::svc::MemoryInfo m = hle::svc::QueryMemory(va);
    return m.state != 0 && va + bytes <= u64(m.base) + m.size;
}

// Play's camera array: four 0x198-byte cameras at +0x408, active index at +0xA7A, Camera::setting
// at +0x156; settings 9/10 (DEMO0/FREE0) are scripted cutscene cameras.
static bool scripted_camera() {
    if (!readable(0x72F5C0, 4)) return false;  // runs during the title screen too
    const u32 context = r32(0x72F5C0);
    if (!readable(context + 0x16C, 4)) return false;
    const u32 game = r32(context + 0x16C);
    if (!readable(game, 0xA7C) || (r32(game + 0x13C) >> 8 & 255) != 4) return false;
    const u32 index = r32(game + 0xA78) >> 16;
    if (index > 3) return false;
    const u32 camera = game + 0x408 + index * 0x198;
    const unsigned setting = r32(camera + 0x154) >> 16;
    return r32(camera + 0xA0) == game && (setting == 9 || setting == 10);
}

// mov r0, r4 at 0x14F70C (play update loop, right before bl 0x15F644): the Play object lives on the
// heap with no global pointer, so it is captured here for the scene tour (DOT_TOUR in main.cpp).
std::atomic<u32> g_play{0};
void play_context(Context* c) {
    c->r[0] = c->r[4];
    g_play.store(c->r[4], std::memory_order_relaxed);
}
u32 play() { return g_play.load(std::memory_order_relaxed); }

void fov_projection(Context* c) {
    const u32 projection = c->r[4];
    u32 bits = r32(projection + 0x18);
    float want = fov.load(std::memory_order_relaxed);
    // Only the live top-screen projection (0x656B5C, a 400x240 viewport) can be a cutscene camera.
    if (r32(0x656B5C) == projection && r32(projection + 4) == 0 && r32(projection + 8) == 400 &&
        r32(projection + 12) == 0 && r32(projection + 16) == 240 && scripted_camera())
        want = cutscene_fov.load(std::memory_order_relaxed);
    const float native = std::bit_cast<float>(bits);
    if (std::isfinite(native) && native > 0 && native < 170 && want > 45 && want <= 70) {
        constexpr float half_rad = 3.14159265358979f / 360.f;
        // 45° becomes the setting; other FOVs (animated cameras) widen by the same tangent ratio.
        bits = std::bit_cast<u32>(std::atan(std::tan(native * half_rad) * std::tan(want * half_rad) /
                                            std::tan(45.f * half_rad)) / half_rad);
    }
    c->s[0] = bits;
}

// add r0, r7, r8, lsl #2 where the pause menu starts copying the top screen into its backdrop
// texture (0x120648 v1.0/Restoration, 0x120748 v1.1; DekUI's PauseCapture PC): the CPU reads the
// linear framebuffer (RGB8, 400 rows of 240 * 3 bytes) at owner + 0xC + index * 4 (+8 for the
// right eye) directly, which the renderer cannot see, so the GPU-side frame is written back first.
// Without it the paused world behind the menus was black.
void pause_capture_sync(Context* c) {
    const u32 entry = c->r[7] + (c->r[8] << 2), fb = r32(entry + (c->r[4] ? 0x14 : 0xC));
    if (fb) hle::service::gsp::cpu_read(fb, fb + 400 * 240 * 3);
    c->r[0] = entry;
}

}  // namespace recomp::hooks
