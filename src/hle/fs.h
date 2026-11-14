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
#include <string_view>

#include "core/types.h"

// nn::fs — RomFS served straight from the extracted game_data/romfs/ directory.
// Paths use the SDK form "rom:/actors/foo.zar" (the "rom:" prefix is optional).
namespace hle::fs {

constexpr Result ERR_NOT_FOUND = 0xC8804478;
constexpr Result ERR_INVALID_HANDLE = 0xD8E007F7;
constexpr Result ERR_INVALID_PATH = 0xE0E046BE;

bool MountRom(const std::filesystem::path& romfs_dir);

Result OpenFile(Handle* out, std::string_view path);
Result GetSize(Handle h, u64* out);
// Reads up to `size` bytes at `offset` into `dst` (a host pointer, e.g. Memory::host(va)).
Result Read(Handle h, u64 offset, void* dst, u32 size, u32* bytes_read);
Result Close(Handle h);

}  // namespace hle::fs
