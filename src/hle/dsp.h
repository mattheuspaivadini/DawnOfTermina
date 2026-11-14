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

// nn::dsp — stub. Accepts the DSP component so boot proceeds; produces no audio.
// shortcut: no audio pipeline yet; implement HLE of the DSP firmware in the audio stage.
namespace hle::dsp {

Result Initialize();
Result LoadComponent(u32 size, bool* loaded);
Result Shutdown();

}  // namespace hle::dsp
