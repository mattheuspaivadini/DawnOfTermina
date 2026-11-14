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

#include "recomp/runtime.h"

// Native replacements for hot C library routines of the game (listed with their VAs in
// tools/recomp/hooks.txt). The recompiler emits the hooked func_XXXXXXXX as a call to
// these, so direct calls, tail calls and the indirect-call table all reach them.
namespace recomp::hooks {

u32 aeabi_memcpy(Context* c);
u32 memcpy_src_dst(Context* c);
u32 aeabi_memcpy4(Context* c);
u32 aeabi_memmove4(Context* c);
u32 aeabi_memset(Context* c);
u32 aeabi_memclr(Context* c);
u32 aeabi_memclr4(Context* c);
u32 strlen(Context* c);
u32 strcmp(Context* c);
u32 sinf(Context* c);
u32 cosf(Context* c);
u32 atan2f(Context* c);

}  // namespace recomp::hooks
