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

#include "recomp/runtime.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <vector>

namespace recomp {

u8* g_base = nullptr;
std::atomic<u32> g_interrupt{0};
static void (*g_interrupt_handler)() = nullptr;

void set_interrupt_handler(void (*handler)()) { g_interrupt_handler = handler; }

void service_interrupt() {
    g_interrupt.store(0, std::memory_order_relaxed);
    if (g_interrupt_handler) g_interrupt_handler();
}
static const FuncEntry* g_table = nullptr;
// Direct index: slot (key - g_lo) / 2 holds 1 + the entry's position in g_table (0 = none).
// Entries are at least halfword aligned, so ARM and Thumb keys never share a slot.
static std::vector<u32> g_slots;
static u32 g_lo = 0;

void set_memory(u8* guest_base) { g_base = guest_base; }
void set_function_table(const FuncEntry* table, u32 count) {
    g_table = table;
    g_slots.clear();
    if (!count) return;
    g_lo = table[0].key & ~1u;  // sorted by key
    g_slots.assign(((table[count - 1].key - g_lo) >> 1) + 1, 0);
    for (u32 i = 0; i < count; ++i) g_slots[(table[i].key - g_lo) >> 1] = i + 1;
}

static std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

u32 call(Context* c, u32 target) {
    const u32 slot = (target - g_lo) >> 1;  // wraps to huge below g_lo
    const u32 i = slot < g_slots.size() ? g_slots[slot] : 0;
    // The key check rejects an ARM/Thumb mode mismatch at a real entry address.
    if (!i || g_table[i - 1].key != target)
        throw Trap(target, "indirect call/jump to " + hex(target) + " which is not a recompiled function entry "
                           "(add it to tools/recomp/extra_entries.txt if it is real code); LR=" + hex(c->r[14]));
    return g_table[i - 1].fn(c);
}

void unimplemented(Context*, u32 pc, const char* what) {
    throw Trap(pc, std::string("unimplemented instruction at ") + hex(pc) + ": " + what);
}

u32 fell_off(Context*, u32 pc) { throw Trap(pc, "execution fell off a recompiled function at " + hex(pc)); }

u32 read_cpsr(const Context* c) {
    return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->q << 27 | c->ge << 16 | 0x10;  // user mode
}

void write_cpsr(Context* c, u32 v, u32 mask) {
    if (mask & 8) c->n = v >> 31, c->z = v >> 30 & 1, c->c = v >> 29 & 1, c->v = v >> 28 & 1, c->q = v >> 27 & 1;
    if (mask & 4) c->ge = v >> 16 & 0xF;
}

static s32 sat32(Context* c, s64 v) {
    if (v > std::numeric_limits<s32>::max()) return c->q = 1, std::numeric_limits<s32>::max();
    if (v < std::numeric_limits<s32>::min()) return c->q = 1, std::numeric_limits<s32>::min();
    return s32(v);
}

u32 qaddsub(Context* c, u32 op, u32 m, u32 n) {
    s64 rn = s32(n);
    if (op & 2) rn = sat32(c, rn * 2);  // QDADD / QDSUB
    return u32(sat32(c, op & 1 ? s64(s32(m)) - rn : s64(s32(m)) + rn));
}

u32 add_q(Context* c, u32 a, u32 b) {
    const s64 r = s64(s32(a)) + s32(b);
    if (r != s32(r)) c->q = 1;
    return u32(r);
}

u32 parallel(Context* c, u32 prefix, u32 op, u32 n, u32 m) {
    const bool is_signed = prefix < 4;
    const u32 kind = prefix & 3;  // 1 plain (sets GE), 2 saturating, 3 halving
    const bool bytes = op >= 4;
    const int lanes = bytes ? 4 : 2, width = bytes ? 8 : 16;
    const u32 mask = (1u << width) - 1;
    auto lane = [&](u32 v, int i) -> s64 {
        const u32 x = v >> (i * width) & mask;
        return is_signed ? s64(x) - (x >> (width - 1) ? s64(1) << width : 0) : s64(x);
    };
    u32 result = 0, ge = 0;
    for (int i = 0; i < lanes; ++i) {
        s64 a = lane(n, i), b, r;
        bool add;
        if (bytes) {
            b = lane(m, i), add = op == 4;
        } else if (op == 1 || op == 2) {  // ASX / SAX: cross halves
            b = lane(m, 1 - i);
            add = (op == 1) == (i == 1);
        } else {
            b = lane(m, i), add = op == 0;
        }
        r = add ? a + b : a - b;
        if (kind == 1) {
            const bool g = is_signed ? r >= 0 : (add ? r >= (s64(1) << width) : r >= 0);
            if (g) ge |= (bytes ? 1u : 3u) << (bytes ? i : 2 * i);
        } else if (kind == 2) {
            const s64 lo = is_signed ? -(s64(1) << (width - 1)) : 0;
            const s64 hi = is_signed ? (s64(1) << (width - 1)) - 1 : (s64(1) << width) - 1;
            r = std::clamp(r, lo, hi);
        } else if (kind == 3) {
            r >>= 1;
        }
        result |= (u32(r) & mask) << (i * width);
    }
    if (kind == 1) c->ge = ge;
    return result;
}

u32 sel(const Context* c, u32 n, u32 m) {
    u32 r = 0;
    for (int i = 0; i < 4; ++i) r |= ((c->ge >> i & 1) ? n : m) & (0xFFu << (8 * i));
    return r;
}

u32 ssat(Context* c, u32 v, u32 bits) {
    const s64 hi = (s64(1) << (bits - 1)) - 1, lo = -(s64(1) << (bits - 1));
    const s64 x = s32(v);
    if (x > hi) return c->q = 1, u32(hi);
    if (x < lo) return c->q = 1, u32(lo);
    return v;
}

u32 usat(Context* c, u32 v, u32 bits) {
    const s64 hi = (s64(1) << bits) - 1;
    const s64 x = s32(v);
    if (x > hi) return c->q = 1, u32(hi);
    if (x < 0) return c->q = 1, 0;
    return v;
}

u32 ssat16(Context* c, u32 v, u32 bits) {
    return (ssat(c, u32(s32(s16(v))), bits) & 0xFFFF) | ssat(c, u32(s32(s16(v >> 16))), bits) << 16;
}

u32 usat16(Context* c, u32 v, u32 bits) {
    return (usat(c, u32(s32(s16(v))), bits) & 0xFFFF) | usat(c, u32(s32(s16(v >> 16))), bits) << 16;
}

u32 xtb16(u32 v, bool sign) {
    const u32 lo = sign ? u32(s32(s8(v))) : v & 0xFF;
    const u32 hi = sign ? u32(s32(s8(v >> 16))) : (v >> 16) & 0xFF;
    return (lo & 0xFFFF) | hi << 16;
}

u32 add16(u32 a, u32 b) { return ((a + b) & 0xFFFF) | (((a >> 16) + (b >> 16)) << 16); }

u32 usad8(u32 a, u32 b) {
    u32 sum = 0;
    for (int i = 0; i < 4; ++i) {
        const int x = int(a >> (8 * i) & 0xFF), y = int(b >> (8 * i) & 0xFF);
        sum += u32(x > y ? x - y : y - x);
    }
    return sum;
}

void dual_mul(Context* c, bool longacc, bool subtract, bool swap, u32 rd, u32 ra, u32 n, u32 m) {
    if (swap) m = std::rotr(m, 16);
    const s64 p1 = s64(s16(n)) * s16(m), p2 = s64(s16(n >> 16)) * s16(m >> 16);
    const s64 sum = subtract ? p1 - p2 : p1 + p2;
    if (longacc) {  // SMLALD / SMLSLD: rd = RdHi, ra = RdLo
        const s64 acc = s64(u64(c->r[rd]) << 32 | c->r[ra]) + sum;
        c->r[ra] = u32(acc), c->r[rd] = u32(u64(acc) >> 32);
        return;
    }
    s64 r = sum + (ra != 15 ? s64(s32(c->r[ra])) : 0);
    if (r != s32(r)) c->q = 1;
    c->r[rd] = u32(r);
}

void vfp_cmp(Context* c, double a, double b) {
    u32 nzcv;
    if (std::isnan(a) || std::isnan(b)) nzcv = 0x3;
    else if (a == b) nzcv = 0x6;
    else if (a < b) nzcv = 0x8;
    else nzcv = 0x2;
    c->fpscr = (c->fpscr & 0x0FFFFFFF) | nzcv << 28;
}

u32 vfp_to_int(Context* c, double v, bool is_signed, bool toward_zero) {
    if (std::isnan(v)) return 0;
    // FPSCR.RMode (bits 23:22): 0 nearest, 1 +inf, 2 -inf, 3 zero.
    const u32 mode = toward_zero ? 3 : (c->fpscr >> 22 & 3);
    v = mode == 0 ? std::nearbyint(v) : mode == 1 ? std::ceil(v) : mode == 2 ? std::floor(v) : std::trunc(v);
    if (is_signed) return u32(s32(std::clamp(v, -2147483648.0, 2147483647.0)));
    return u32(std::clamp(v, 0.0, 4294967295.0));
}

}  // namespace recomp
