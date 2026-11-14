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

#include <filesystem>
#include <optional>

#include "core/memory.h"

namespace core {

struct Segment {
    u32 addr, pages, size;
};

// ExHeader SCI CodeSetInfo.
struct CodeSet {
    Segment text, rodata, data;
    u32 bss_size, stack_size;
};

// Reads game_data/exheader.bin + exefs/code.bin, maps .text/.rodata (read-only),
// .data+.bss and the main stack at their original VAs.
std::optional<CodeSet> load_code(Memory& mem, const std::filesystem::path& game_data);

}  // namespace core
