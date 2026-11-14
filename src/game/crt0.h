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

#include "core/types.h"

// CTR-SDK C runtime start-up (crt0) and nninit, as linked into code.bin.
namespace game::crt0 {

// Original VA: 0x00100000 — process entry point. Never returns (ends in svcExitProcess).
[[noreturn]] void _start();

void ClearBss();          // 0x00100024
void InitCLocale();       // 0x001042F4
void nninitStartUp();     // 0x00104368
Result SetHeapSize(u32 size);  // 0x00105A6C

// Guest globals touched by the boot path.
namespace va {
constexpr u32 BSS_START = 0x006B0B64, BSS_END = 0x007CE49C;
constexpr u32 CLIB_LOCALE_STATE = 0x007C934C;  // C library locale block
constexpr u32 CTYPE_TABLE_C = 0x0064A260;      // "C" ctype table, entry 0 = EOF
constexpr u32 CASE_TABLE_C = 0x0064A370;
constexpr u32 LOCALE_NAME_C = 0x0064A258, LOCALE_NAME_C2 = 0x0064A368;  // both "C"
constexpr u32 SRV_REFCOUNT = 0x006B08C4, SRV_HANDLE = 0x006B08C8;
constexpr u32 STARTUP_DONE = 0x00656B64;   // cleared by nninitStartUp, set by nnMain
constexpr u32 HEAP_SIZE = 0x006A4044;      // current size of the 0x08000000 heap
constexpr u32 DEVICE_MEM_BASE = 0x006A403C, DEVICE_MEM_SIZE = 0x006A4040;
constexpr u32 HEAP_REGION_READY = 0x006A4048;
constexpr u32 HEAP_REGION_OBJ = 0x007B2E14;
constexpr u32 DEFAULT_ALLOCATOR_PTR = 0x006A404C, DEFAULT_ALLOCATOR = 0x007C14D0;
}  // namespace va

constexpr u32 DEVICE_MEMORY_SIZE = 0x01F12000;  // linear memory for GPU/DSP

}  // namespace game::crt0
