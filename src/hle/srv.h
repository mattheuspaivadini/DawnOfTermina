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

#include <string_view>

#include "core/types.h"

// nn::srv — service manager. Services are HLE'd as direct C++ calls (see
// hle/hid.h, hle/dsp.h), so a handle here only lets transcribed code keep its
// "get handle, check result" flow intact.
namespace hle::srv {

constexpr Result ERR_NOT_REGISTERED = 0xD8E06406;

Result Initialize();
Result GetServiceHandle(Handle* out, std::string_view name);

// Registers the "srv:" port with hle::kernel for recompiled code (IPC level).
void RegisterIpc();

}  // namespace hle::srv
