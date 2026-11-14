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

#include "hle/svc.h"

#include <chrono>
#include <iterator>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace hle::svc {

using core::Memory;

// shortcut: linear heap is a bump allocator (freed ranges are not reused);
// replace with a free list if the game frees and re-commits linear memory.
static u32 linear_next = Memory::LINEAR_BASE;
static u32 committed = 0;
static std::map<u32, MemoryInfo> regions;  // by base

void AddRegion(u32 base, u32 size, u32 perm, u32 state) {
    // Merge with a directly preceding region of the same kind (heap grows in steps).
    auto it = regions.lower_bound(base);
    if (it != regions.begin()) {
        auto prev = std::prev(it);
        MemoryInfo& p = prev->second;
        if (p.base + p.size == base && p.perm == perm && p.state == state) {
            p.size += size;
            return;
        }
    }
    regions[base] = {base, size, perm, state};
}

static void RemoveRange(u32 base, u32 size) {
    for (auto it = regions.begin(); it != regions.end();) {
        MemoryInfo r = it->second;
        const u64 end = u64(r.base) + r.size, cut_end = u64(base) + size;
        if (end <= base || r.base >= cut_end) {
            ++it;
            continue;
        }
        it = regions.erase(it);
        if (r.base < base) regions[r.base] = {r.base, base - r.base, r.perm, r.state};
        if (end > cut_end) regions[u32(cut_end)] = {u32(cut_end), u32(end - cut_end), r.perm, r.state};
    }
}

MemoryInfo QueryMemory(u32 addr) {
    auto it = regions.upper_bound(addr);
    u32 gap_start = 0;
    if (it != regions.begin()) {
        const MemoryInfo& r = std::prev(it)->second;
        if (addr < u64(r.base) + r.size) return r;
        gap_start = r.base + r.size;
    }
    const u32 gap_end = it == regions.end() ? 0xFFFFF000u : it->first;
    return {gap_start, gap_end - gap_start, 0, 0};
}

void InitProcess(Memory& mem, u32 image_bytes) {
    mem.write<u32>(Memory::CONFIG_MEM + 0x40, 0x04000000);  // APPMEMALLOC
    mem.write<u32>(Memory::CONFIG_MEM + 0x44, 0x02C00000);  // SYSMEMALLOC
    mem.write<u32>(Memory::CONFIG_MEM + 0x48, 0x01400000);  // BASEMEMALLOC
    committed = image_bytes;
    linear_next = Memory::LINEAR_BASE;
    regions.clear();
}

u32 CommittedMemory() { return committed; }

Result ControlMemory(Memory& mem, u32* out_addr, u32 addr0, u32 /*addr1*/, u32 size, u32 op,
                     u32 perm) {
    if (size % Memory::PAGE) return ERR_INVALID_ADDRESS;
    const bool linear = op & MEMOP_LINEAR;
    switch (op & MEMOP_OP_MASK) {
    case MEMOP_COMMIT: {
        if (committed + u64(size) > Memory::FCRAM_SIZE) return ERR_OUT_OF_MEMORY;
        u32 addr = addr0;
        if (linear) {
            addr = linear_next;
            linear_next += size;
        } else if (addr0 < Memory::HEAP_BASE || u64(addr0) + size > Memory::STACK_TOP - (4u << 20)) {
            return ERR_INVALID_ADDRESS;
        }
        if (!mem.map(addr, size)) return ERR_INVALID_ADDRESS;
        AddRegion(addr, size, perm & 7, linear ? 7 : 5);
        committed += size;
        *out_addr = addr;
        return RESULT_OK;
    }
    case MEMOP_FREE:
        if (!mem.unmap(addr0, size)) return ERR_INVALID_ADDRESS;
        RemoveRange(addr0, size);
        committed -= size < committed ? size : committed;
        *out_addr = addr0;
        return RESULT_OK;
    case MEMOP_PROTECT:
        return mem.protect(addr0, size, perm & PERM_W) ? RESULT_OK : ERR_INVALID_ADDRESS;
    default:
        return ERR_INVALID_COMBINATION;
    }
}

void ExitProcess() { std::exit(0); }

void SleepThread(s64 ns) {
    if (ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    else std::this_thread::yield();
}

u64 GetSystemTick() {
    using namespace std::chrono;
    static const auto boot = steady_clock::now();
    const auto ns = duration_cast<nanoseconds>(steady_clock::now() - boot).count();
    constexpr u64 HZ = 268111856, NS = 1000000000;
    return u64(ns) / NS * HZ + u64(ns) % NS * HZ / NS;  // split to avoid u64 overflow
}

void Break(u32 reason) {
    std::fprintf(stderr, "svcBreak(%u)\n", reason);
    std::abort();
}

void OutputDebugString(const Memory& mem, u32 addr, u32 len) {
    std::fprintf(stderr, "%.*s", int(len), reinterpret_cast<const char*>(mem.host(addr)));
}

}  // namespace hle::svc
