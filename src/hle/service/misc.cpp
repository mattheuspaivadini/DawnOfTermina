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

// Small services whose commands need no state.

#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "hle/ipc.h"
#include "hle/service/services.h"

namespace hle::service {

namespace {

// Logs the first use of each command so stubbed behaviour stays visible.
void log_once(const char* service, u32 header) {
    static std::mutex lock;
    static std::set<std::string> seen;
    char key[64];
    std::snprintf(key, sizeof key, "%s 0x%08X", service, header);
    std::lock_guard lk(lock);
    if (seen.insert(key).second) std::fprintf(stderr, "hle: %s (stub: success)\n", key);
}

}  // namespace

void register_misc() {
    // ndm:u (network daemon manager): every command only controls background daemons.
    kernel::register_service("ndm:u", [](core::Memory& mem, u32 base) {
        ipc::Cmd c{mem, base};
        log_once("ndm:u", c.header());
        c.reply(1, 0);
    });
}

}  // namespace hle::service
