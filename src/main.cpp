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

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    const std::filesystem::path data = argc > 1 ? argv[1] : "game_data";
    if (!std::filesystem::exists(data / "romfs")) {
        std::fprintf(stderr, "game_data not found at '%s'. Run tools/extract_cia.py first.\n",
                     data.string().c_str());
        return 1;
    }
    std::puts("DawnOfTermina: game_data found. Runtime not implemented yet.");
    return 0;
}
