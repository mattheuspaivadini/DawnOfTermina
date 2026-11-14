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
#include <stdexcept>

#include "core/memory.h"

// Shared helpers for transcribed game code. Game data stays at its original guest
// addresses (.data/.bss/heap), so transcribed functions take and return guest
// pointers as u32 and touch memory through these accessors.
namespace game {

void bind(core::Memory& mem);
core::Memory& mem();

inline u8 r8(u32 va) { return mem().read<u8>(va); }
inline u16 r16(u32 va) { return mem().read<u16>(va); }
inline u32 r32(u32 va) { return mem().read<u32>(va); }
inline void w8(u32 va, u8 v) { mem().write<u8>(va, v); }
inline void w16(u32 va, u16 v) { mem().write<u16>(va, v); }
inline void w32(u32 va, u32 v) { mem().write<u32>(va, v); }
inline const char* cstr(u32 va) { return reinterpret_cast<const char*>(mem().host(va)); }

// Thrown when execution reaches a function that has not been transcribed or
// HLE-replaced yet. The boot runs until the first one: that address is the next
// thing to work on.
struct Frontier : std::runtime_error {
    u32 va;
    explicit Frontier(u32 va);
};
[[noreturn]] void untranscribed(u32 va);

// SDK fatal-error paths (nn::err / panic): report and abort.
[[noreturn]] void fatal(const char* where, Result r);

}  // namespace game
