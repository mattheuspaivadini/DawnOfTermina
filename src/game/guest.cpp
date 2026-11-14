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

#include "game/guest.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace game {

static core::Memory* g_mem = nullptr;

void bind(core::Memory& mem) { g_mem = &mem; }
core::Memory& mem() { return *g_mem; }

static std::string frontier_message(u32 va) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "untranscribed function at VA 0x%08X", va);
    return buf;
}

Frontier::Frontier(u32 va) : std::runtime_error(frontier_message(va)), va(va) {}

void untranscribed(u32 va) { throw Frontier(va); }

void fatal(const char* where, Result r) {
    std::fprintf(stderr, "game: fatal error in %s, result 0x%08X\n", where, r);
    std::abort();
}

}  // namespace game
