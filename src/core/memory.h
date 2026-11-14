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

#pragma once

#include <cstring>

#include "core/types.h"

namespace core {

// The whole 32-bit guest address space is reserved as one host block, so a guest VA
// translates to a host pointer with a single add. Unmapped guest pages stay PROT_NONE
// and fault on access, which surfaces bad transcriptions immediately.
class Memory {
public:
    static constexpr u32 PAGE = 0x1000;
    static constexpr u32 HEAP_BASE = 0x08000000;
    static constexpr u32 STACK_TOP = 0x10000000;
    static constexpr u32 LINEAR_BASE = 0x14000000;  // Old 3DS linear heap
    static constexpr u32 VRAM_BASE = 0x1F000000;
    static constexpr u32 DSP_BASE = 0x1FF00000;
    static constexpr u32 CONFIG_MEM = 0x1FF80000;
    static constexpr u32 SHARED_PAGE = 0x1FF81000;
    static constexpr u32 TLS_BASE = 0x1FF82000;

    static constexpr u32 FCRAM_SIZE = 128u << 20;
    static constexpr u32 VRAM_SIZE = 6u << 20;
    static constexpr u32 DSP_SIZE = 512u << 10;
    static constexpr u32 TLS_SIZE = 0x200;  // per thread; IPC command buffer at +0x80
    static constexpr u32 TLS_PAGES = 4;     // shortcut: room for 32 threads, grow when threads are HLE'd

    Memory();
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    // addr and size must be page aligned. New pages are zero-filled.
    bool map(u32 addr, u32 size);
    bool unmap(u32 addr, u32 size);
    bool protect(u32 addr, u32 size, bool writable);

    u8* host(u32 addr) const { return base_ + addr; }

    template <class T> T read(u32 addr) const {
        T v;
        std::memcpy(&v, base_ + addr, sizeof v);
        return v;
    }
    template <class T> void write(u32 addr, T v) { std::memcpy(base_ + addr, &v, sizeof v); }

    static constexpr u32 tls(u32 thread_index) { return TLS_BASE + thread_index * TLS_SIZE; }

private:
    u8* base_ = nullptr;
};

}  // namespace core
