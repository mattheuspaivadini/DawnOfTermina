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

// cfg:u — system configuration blocks, answered as a USA Old 3DS set to English.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hle/ipc.h"
#include "hle/service/services.h"

namespace hle::service {

namespace {

constexpr u8 LANGUAGE_ENGLISH = 1;
constexpr u8 COUNTRY_USA = 49;
constexpr u32 REGION_USA = 1;

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// Contents of a config block, or empty if unknown.
std::vector<u8> block(u32 id) {
    switch (id) {
    case 0x00070001: return {1};                      // sound output: stereo
    case 0x000A0001: return {1, 1};                   // birthday: month, day
    case 0x000A0002: return {LANGUAGE_ENGLISH};       // language
    case 0x000B0000: return {0, 0, 2, COUNTRY_USA};   // country info: -, -, province, country
    case 0x000D0000: return {0x7F, 0x7F, 0, 0};       // EULA version accepted (max)
    case 0x000F0004: return {0, 0, 0, 0};             // system model: CTR (Old 3DS)
    case 0x00130000: return {0, 0, 0, 0};             // undocumented; zero on a retail default config
    case 0x00050005: {                                // stereo camera / 3D settings: 8 floats
        const float v[8] = {62.0f, 289.0f, 76.80f, 46.08f, 10.0f, 5.0f, 55.58f, 21.57f};
        std::vector<u8> b(sizeof v);
        std::memcpy(b.data(), v, sizeof v);
        return b;
    }
    case 0x000A0000: {  // user name: UTF-16, 0x1C bytes incl. terminator
        std::vector<u8> b(0x1C, 0);
        const char* name = "Player";
        for (int i = 0; name[i]; ++i) b[i * 2] = u8(name[i]);
        return b;
    }
    default: return {};
    }
}

void cfg_command(core::Memory& mem, u32 base) {
    ipc::Cmd c{mem, base};
    switch (c.id()) {
    case 0x0001: {  // GetConfigInfoBlk2(size, block id, buffer desc, address)
        const u32 size = c[1], id = c[2], addr = c[4];
        const std::vector<u8> data = block(id);
        if (data.empty()) throw kernel::Unimplemented("cfg: config block " + hex(id) + " (size " + std::to_string(size) + ")");
        std::memset(mem.host(addr), 0, size);
        std::memcpy(mem.host(addr), data.data(), std::min<size_t>(size, data.size()));
        c.reply(1, 2);
        c.set(2, size << 4 | 0xC), c.set(3, addr);
        return;
    }
    case 0x0002:  // SecureInfoGetRegion
        c.reply(2, 0);
        c.set(2, REGION_USA);
        return;
    case 0x0003:  // GenHashConsoleUnique(salt) -> u64
        c.reply(3, 0);
        c.set(2, 0x13579BDF), c.set(3, 0x2468ACE0);
        return;
    case 0x0004:  // GetRegionCanadaUSA
        c.reply(2, 0);
        c.set(2, 1);
        return;
    case 0x0005:  // GetSystemModel
        c.reply(2, 0);
        c.set(2, 0);  // CTR
        return;
    case 0x0006:  // GetModelNintendo2DS: 1 = not a 2DS
        c.reply(2, 0);
        c.set(2, 1);
        return;
    default:
        ipc::unimplemented("cfg:u", c);
    }
}

}  // namespace

void register_cfg() {
    for (const char* name : {"cfg:u", "cfg:s", "cfg:i"}) kernel::register_service(name, cfg_command);
}

}  // namespace hle::service
