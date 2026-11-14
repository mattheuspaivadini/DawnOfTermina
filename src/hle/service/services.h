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

#include "core/memory.h"

// Registers every HLE'd system service with hle::kernel.
namespace hle::service {

void register_all(core::Memory& mem, const std::filesystem::path& game_data);

void register_apt();
void register_misc();
void register_cfg();
void register_gsp(core::Memory& mem);
void register_hid(core::Memory& mem);
void register_dsp(core::Memory& mem);
void register_fs(const std::filesystem::path& game_data);

}  // namespace hle::service
