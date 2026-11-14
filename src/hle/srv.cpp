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
#include <string>

#include "hle/kernel.h"

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

void RegisterIpc() {
    hle::kernel::register_service("srv:", [](core::Memory& mem, u32 cmd) {
        auto rd = [&](u32 i) { return mem.read<u32>(cmd + i * 4); };
        auto wr = [&](u32 i, u32 v) { mem.write<u32>(cmd + i * 4, v); };
        const u32 header = rd(0);
        switch (header) {
        case 0x00010002:  // RegisterClient
            wr(0, 0x00010040), wr(1, RESULT_OK);
            return;
        case 0x00020000:  // EnableNotification -> semaphore handle
            wr(0, 0x00020042), wr(1, RESULT_OK), wr(2, 0x0), wr(3, hle::kernel::create_semaphore(0, 64));
            return;
        case 0x00050100: {  // GetServiceHandle(name[8], length, flags)
            const u32 len = std::min(rd(3), 8u);
            const std::string name(reinterpret_cast<const char*>(mem.host(cmd + 4)), len);
            // Always hand out a session: the first request to a service without an HLE
            // implementation then reports the service name and command.
            wr(0, 0x00050042), wr(1, RESULT_OK), wr(2, 0x10), wr(3, hle::kernel::open_session(name));
            return;
        }
        default: {
            char msg[64];
            std::snprintf(msg, sizeof msg, "srv: command 0x%08X", header);
            throw hle::kernel::Unimplemented(msg);
        }
        }
    });
}

}  // namespace hle::srv
