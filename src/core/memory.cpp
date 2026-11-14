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

#include "core/memory.h"

#include <algorithm>

#include "core/savestate.h"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace core {

static constexpr u64 SPACE = 1ull << 32;
static u8* g_fault_base = nullptr;

// A fault inside the guest address space means recompiled code touched unmapped guest
// memory: report the guest VA (async-signal-safe output only) and stop.
static void on_fault(int sig, siginfo_t* info, void*) {
    const u8* a = static_cast<const u8*>(info->si_addr);
    if (g_fault_base && a >= g_fault_base && a < g_fault_base + SPACE) {
        char msg[] = "guest: memory fault at guest VA 0x00000000 (unmapped page)\n";
        u32 va = u32(a - g_fault_base);
        for (int i = 0; i < 8; ++i) msg[41 - i] = "0123456789ABCDEF"[(va >> (i * 4)) & 0xF];
        (void)!write(2, msg, sizeof msg - 1);
    }
    signal(sig, SIG_DFL);  // re-raise for a core dump / debugger
    raise(sig);
}

static bool aligned(u32 addr, u32 size) {
    return addr % Memory::PAGE == 0 && size % Memory::PAGE == 0 && u64(addr) + size <= SPACE;
}

// shortcut: POSIX mmap only; add a VirtualAlloc path when Windows is targeted.
Memory::Memory() {
    void* p = mmap(nullptr, SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        std::perror("reserving 4 GiB guest address space");
        std::abort();
    }
    base_ = static_cast<u8*>(p);
    if (!g_fault_base) {
        g_fault_base = base_;
        struct sigaction sa = {};
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, nullptr);
    }
    if (!map(VRAM_BASE, VRAM_SIZE) || !map(DSP_BASE, DSP_SIZE) || !map(CONFIG_MEM, PAGE) ||
        !map(SHARED_PAGE, PAGE) || !map(TLS_BASE, TLS_PAGES * PAGE)) {
        std::perror("mapping fixed 3DS regions");
        std::abort();
    }
}

Memory::~Memory() { munmap(base_, SPACE); }

void Memory::mark(u32 addr, u32 size, u8 state) {
    if (pages_.empty()) pages_.assign(SPACE / PAGE, 0);
    std::fill_n(pages_.begin() + addr / PAGE, size / PAGE, state);
}

bool Memory::map(u32 addr, u32 size) {
    if (!aligned(addr, size) || mprotect(base_ + addr, size, PROT_READ | PROT_WRITE) != 0) return false;
    mark(addr, size, 2);
    return true;
}

bool Memory::unmap(u32 addr, u32 size) {
    // MADV_DONTNEED drops the pages so a later map() sees zeroes again.
    if (!aligned(addr, size) || madvise(base_ + addr, size, MADV_DONTNEED) != 0 ||
        mprotect(base_ + addr, size, PROT_NONE) != 0)
        return false;
    mark(addr, size, 0);
    return true;
}

bool Memory::protect(u32 addr, u32 size, bool writable) {
    if (!aligned(addr, size) || mprotect(base_ + addr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ) != 0)
        return false;
    mark(addr, size, writable ? 2 : 1);
    return true;
}

// Read-only pages (code, rodata) come from the loader and are the same on every boot.
void Memory::save_state(StateWriter& w) const {
    w.section("MEM ");
    w.vec(pages_);
    static const u8 zero[PAGE] = {};
    for (u32 p = 0; p < pages_.size(); ++p)
        if (pages_[p] == 2 && std::memcmp(base_ + u64(p) * PAGE, zero, PAGE)) {
            w.put(p);
            w.bytes(base_ + u64(p) * PAGE, PAGE);
        }
    w.put(~0u);
}

void Memory::load_state(StateReader& r) {
    r.section("MEM ");
    const std::vector<u8> want = r.vec<u8>();
    if (want.size() != SPACE / PAGE) throw StateError("savestate: bad page map");
    for (u32 p = 0; p < want.size();) {  // runs of pages whose state changes
        u32 q = p;
        while (q < want.size() && want[q] == want[p] && pages_[q] != want[q]) ++q;
        if (q == p) {
            ++p;
            continue;
        }
        const u32 addr = p * PAGE, size = (q - p) * PAGE;
        if (want[p] == 0 ? !unmap(addr, size) : want[p] == 1 ? !(map(addr, size) && protect(addr, size, false))
                                                           : !map(addr, size))
            throw StateError("savestate: cannot map guest pages");
        p = q;
    }
    for (u32 p = 0; p < pages_.size(); ++p)
        if (pages_[p] == 2) std::memset(base_ + u64(p) * PAGE, 0, PAGE);
    for (u32 p; (p = r.get<u32>()) != ~0u;) {
        if (p >= pages_.size() || pages_[p] != 2) throw StateError("savestate: page data for an unmapped page");
        r.bytes(base_ + u64(p) * PAGE, PAGE);
    }
}

}  // namespace core
