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

    // ptm:u (power/pedometer): plugged in, battery full, no steps walked.
    kernel::register_service("ptm:u", [](core::Memory& mem, u32 base) {
        ipc::Cmd c{mem, base};
        switch (c.id()) {
        case 0x05: case 0x07: case 0x08: case 0x09: case 0x0C: {  // adapter, battery level/charge, pedometer, total steps
            static constexpr u32 VALUE[] = {0, 0, 0, 0, 0, 1, 0, 5, 1, 0, 0, 0, 0};
            c.reply(2, 0);
            c.set(2, VALUE[c.id()]);
            return;
        }
        case 0x0B: {  // GetStepHistory(hours, start time, mapped buffer of u16 per hour)
            const u32 desc = c[4], addr = c[5];
            for (u32 i = 0; i < c[1] && i * 2 < (desc >> 4); ++i) mem.write<u16>(addr + i * 2, 0);
            c.reply(1, 2);
            c.set(2, desc), c.set(3, addr);
            return;
        }
        default:
            log_once("ptm:u", c.header());
            c.reply(1, 0);
        }
    });

    // ir:USER (Circle Pad Pro / IR): no device ever connects. The event getters hand out
    // events that are never signalled; every other command succeeds.
    kernel::register_service("ir:USER", [](core::Memory& mem, u32 base) {
        ipc::Cmd c{mem, base};
        static Handle events[3] = {};  // receive, send, connection status
        if (const u32 id = c.id(); id >= 0x0A && id <= 0x0C) {
            Handle& e = events[id - 0x0A];
            if (!e) e = kernel::create_event(0);
            c.reply(1, 2);
            c.set(2, ipc::copy_handles(1)), c.set(3, e);
            return;
        }
        log_once("ir:USER", c.header());
        c.reply(1, 0);
    });
}

}  // namespace hle::service
