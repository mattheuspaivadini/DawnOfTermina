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

#include "hle/dsp.h"

#include <cstdio>

namespace hle::dsp {

Result Initialize() { return RESULT_OK; }

Result LoadComponent(u32 size, bool* loaded) {
    std::fprintf(stderr, "dsp: LoadComponent(%u bytes) accepted (stub, no audio)\n", size);
    *loaded = true;
    return RESULT_OK;
}

Result Shutdown() { return RESULT_OK; }

}  // namespace hle::dsp
