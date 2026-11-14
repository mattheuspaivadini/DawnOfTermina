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

#include <cstdio>
#include <string>

#include "core/memory.h"
#include "hle/kernel.h"

// Horizon IPC command buffers (TLS + 0x80): word 0 is the header
// [command id:16][normal params:6][translate params:6].
namespace hle::ipc {

struct Cmd {
    core::Memory& mem;
    u32 base;  // guest address of the command buffer

    u32 operator[](u32 i) const { return mem.read<u32>(base + i * 4); }
    void set(u32 i, u32 v) { mem.write<u32>(base + i * 4, v); }
    u32 header() const { return (*this)[0]; }
    u32 id() const { return header() >> 16; }

    // Starts a reply: header with `normal` words (including the result) and `translate` words.
    void reply(u32 normal, u32 translate, Result r = RESULT_OK) {
        set(0, id() << 16 | normal << 6 | translate);
        set(1, r);
    }
    // Receive buffer the client registered for static buffer `index` (TLS + 0x180).
    u32 static_buffer_addr(u32 index) const { return mem.read<u32>(base + 0x100 + index * 8 + 4); }
};

// Translate descriptors.
constexpr u32 copy_handles(u32 n) { return (n - 1) << 26; }
constexpr u32 move_handles(u32 n) { return (n - 1) << 26 | 0x10; }
constexpr u32 static_buffer(u32 size, u32 index) { return size << 14 | index << 10 | 0x2; }

[[noreturn]] inline void unimplemented(const char* service, const Cmd& c) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "%s: command 0x%08X", service, c.header());
    throw hle::kernel::Unimplemented(msg);
}

}  // namespace hle::ipc
