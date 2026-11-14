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

#include "core/memory.h"

// Horizon kernel syscalls, called directly by transcribed code in place of the
// original `svc #N` wrappers. Only what the boot path needs so far.
namespace hle::svc {

enum MemOp : u32 {
    MEMOP_FREE = 1,
    MEMOP_COMMIT = 3,
    MEMOP_PROTECT = 6,
    MEMOP_OP_MASK = 0xFF,
    MEMOP_LINEAR = 0x10000,
};
enum MemPerm : u32 { PERM_R = 1, PERM_W = 2, PERM_X = 4 };

constexpr Result ERR_OUT_OF_MEMORY = 0xD86007F3;
constexpr Result ERR_INVALID_COMBINATION = 0xE0E01BEE;
constexpr Result ERR_INVALID_ADDRESS = 0xE0E01BF5;

// Kernel-side process setup: fills config memory (memory-region sizes for the Old 3DS
// 64 MB application mode) and accounts the loaded image (code + data + bss + stack)
// as already-committed memory.
void InitProcess(core::Memory& mem, u32 image_bytes);
// What svcGetResourceLimitCurrentValues(COMMIT) reports for the process.
u32 CommittedMemory();

// svc 0x01
Result ControlMemory(core::Memory& mem, u32* out_addr, u32 addr0, u32 addr1, u32 size, u32 op,
                     u32 perm);
// svc 0x03
[[noreturn]] void ExitProcess();
// svc 0x0A
void SleepThread(s64 ns);
// svc 0x28 — ARM11 ticks (268,111,856 Hz) since boot.
u64 GetSystemTick();
// svc 0x3C
[[noreturn]] void Break(u32 reason);
// svc 0x3D
void OutputDebugString(const core::Memory& mem, u32 addr, u32 len);

}  // namespace hle::svc
