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

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>

namespace core {

static constexpr u64 SPACE = 1ull << 32;

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
    if (!map(VRAM_BASE, VRAM_SIZE) || !map(DSP_BASE, DSP_SIZE) || !map(CONFIG_MEM, PAGE) ||
        !map(SHARED_PAGE, PAGE) || !map(TLS_BASE, TLS_PAGES * PAGE)) {
        std::perror("mapping fixed 3DS regions");
        std::abort();
    }
}

Memory::~Memory() { munmap(base_, SPACE); }

bool Memory::map(u32 addr, u32 size) {
    return aligned(addr, size) && mprotect(base_ + addr, size, PROT_READ | PROT_WRITE) == 0;
}

bool Memory::unmap(u32 addr, u32 size) {
    // MADV_DONTNEED drops the pages so a later map() sees zeroes again.
    return aligned(addr, size) && madvise(base_ + addr, size, MADV_DONTNEED) == 0 &&
           mprotect(base_ + addr, size, PROT_NONE) == 0;
}

bool Memory::protect(u32 addr, u32 size, bool writable) {
    return aligned(addr, size) &&
           mprotect(base_ + addr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0;
}

}  // namespace core
