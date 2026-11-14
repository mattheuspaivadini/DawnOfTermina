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

#include <cmath>
#include <cstring>

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

}  // namespace recomp::hooks
