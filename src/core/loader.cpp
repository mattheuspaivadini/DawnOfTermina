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

#include "core/loader.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace core {

static std::vector<u8> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static u32 le32(const std::vector<u8>& b, size_t o) {
    return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | u32(b[o + 3]) << 24;
}

static u32 page_align(u32 x) { return (x + Memory::PAGE - 1) & ~(Memory::PAGE - 1); }

std::optional<CodeSet> load_code(Memory& mem, const std::filesystem::path& game_data) {
    auto exh = read_file(game_data / "exheader.bin");
    auto code = read_file(game_data / "exefs" / "code.bin");
    if (exh.size() < 0x40 || code.empty()) {
        std::fprintf(stderr, "loader: missing exheader.bin or exefs/code.bin in %s\n",
                     game_data.string().c_str());
        return std::nullopt;
    }
    auto seg = [&](size_t o) { return Segment{le32(exh, o), le32(exh, o + 4), le32(exh, o + 8)}; };
    CodeSet cs{seg(0x10), seg(0x20), seg(0x30), le32(exh, 0x3C), le32(exh, 0x1C)};

    // code.bin is the three segments back to back, each padded to whole pages.
    const u64 ro_off = u64(cs.text.pages) * Memory::PAGE;
    const u64 data_off = ro_off + u64(cs.rodata.pages) * Memory::PAGE;
    if (code.size() < data_off + cs.data.size || cs.text.size > ro_off ||
        cs.rodata.size > u64(cs.rodata.pages) * Memory::PAGE) {
        std::fprintf(stderr, "loader: code.bin (%zu bytes) does not match exheader\n", code.size());
        return std::nullopt;
    }

    const u32 data_span = page_align(cs.data.size + cs.bss_size);
    const u32 stack = page_align(cs.stack_size);
    if (!mem.map(cs.text.addr, cs.text.pages * Memory::PAGE) ||
        !mem.map(cs.rodata.addr, cs.rodata.pages * Memory::PAGE) ||
        !mem.map(cs.data.addr, data_span) || !mem.map(Memory::STACK_TOP - stack, stack)) {
        std::fprintf(stderr, "loader: could not map code segments\n");
        return std::nullopt;
    }
    std::memcpy(mem.host(cs.text.addr), code.data(), cs.text.size);
    std::memcpy(mem.host(cs.rodata.addr), code.data() + ro_off, cs.rodata.size);
    std::memcpy(mem.host(cs.data.addr), code.data() + data_off, cs.data.size);
    mem.protect(cs.text.addr, cs.text.pages * Memory::PAGE, false);
    mem.protect(cs.rodata.addr, cs.rodata.pages * Memory::PAGE, false);
    return cs;
}

}  // namespace core
