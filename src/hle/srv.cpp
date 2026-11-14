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

#include "hle/srv.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace hle::srv {

static constexpr std::array<std::string_view, 10> KNOWN = {
    "fs:USER", "hid:USER", "dsp::DSP", "gsp::Gpu", "cfg:u", "ptm:u", "apt:U", "ndm:u", "ac:u", "ir:rst",
};

Result Initialize() { return RESULT_OK; }

Result GetServiceHandle(Handle* out, std::string_view name) {
    auto it = std::find(KNOWN.begin(), KNOWN.end(), name);
    if (it == KNOWN.end()) {
        std::fprintf(stderr, "srv: unknown service '%.*s'\n", int(name.size()), name.data());
        return ERR_NOT_REGISTERED;
    }
    *out = 0x100 + Handle(it - KNOWN.begin());  // stable fake handle per service
    return RESULT_OK;
}

}  // namespace hle::srv
