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

// APT (applet manager): application start-up handshake.

#include <deque>
#include <mutex>

#include "hle/ipc.h"
#include "hle/service/services.h"

namespace hle::service {

namespace {

constexpr u32 APPID_HOMEMENU = 0x101;
constexpr u32 SIGNAL_WAKEUP = 1;

struct Parameter {
    u32 sender, signal;
};

std::mutex g_apt_lock;
Handle g_lock_handle = 0, g_notification_event = 0, g_parameter_event = 0;
u32 g_attributes = 0;
std::deque<Parameter> g_parameters;

void handle(core::Memory& mem, u32 base, const char* name) {
    ipc::Cmd c{mem, base};
    std::lock_guard lk(g_apt_lock);
    switch (c.id()) {
    case 0x01:  // GetLockHandle(attributes)
        if (!g_lock_handle) g_lock_handle = kernel::create_mutex(false);
        c.reply(3, 2);
        c.set(2, g_attributes), c.set(3, 0);  // attributes, power button state
        c.set(4, ipc::copy_handles(1)), c.set(5, g_lock_handle);
        return;
    case 0x02:  // Initialize(app id, attributes) -> notification + parameter events
        g_attributes = c[2];
        if (!g_notification_event) g_notification_event = kernel::create_event(0);
        if (!g_parameter_event) g_parameter_event = kernel::create_event(0);
        c.reply(1, 3);
        c.set(2, ipc::copy_handles(2)), c.set(3, g_notification_event), c.set(4, g_parameter_event);
        return;
    case 0x03:  // Enable(attributes): the HOME Menu wakes the application up
        g_parameters.push_back({APPID_HOMEMENU, SIGNAL_WAKEUP});
        kernel::signal_event(g_parameter_event);
        c.reply(1, 0);
        return;
    case 0x09:  // IsRegistered(app id)
        c.reply(2, 0);
        c.set(2, 1);
        return;
    case 0x0B:  // InquireNotification(app id) -> notification type (none)
        c.reply(2, 0);
        c.set(2, 0);
        return;
    case 0x0D:    // ReceiveParameter(app id, buffer size)
    case 0x0E: {  // GlanceParameter
        if (g_parameters.empty()) {
            c.reply(1, 0, 0xC8A0CFEF);  // no parameter pending
            return;
        }
        const Parameter p = g_parameters.front();
        if (c.id() == 0x0D) g_parameters.pop_front();
        c.reply(4, 4);
        c.set(2, p.sender), c.set(3, p.signal), c.set(4, 0);  // sender, signal, buffer size
        c.set(5, ipc::move_handles(1)), c.set(6, 0);
        c.set(7, ipc::static_buffer(0, 0)), c.set(8, c.static_buffer_addr(0));
        return;
    }
    case 0x3E:  // ReplySleepQuery
    case 0x55:  // SetScreenCapPostPermission
    case 0x43:  // NotifyToWait
    case 0x4F:  // SetApplicationCpuTimeLimit
        c.reply(1, 0);
        return;
    case 0x4B:  // AppletUtility(id, input size, output size, ...)
        c.reply(2, 2);
        c.set(2, 0);  // applet result
        c.set(3, ipc::static_buffer(0, 0)), c.set(4, c.static_buffer_addr(0));
        return;
    case 0x50:  // GetApplicationCpuTimeLimit
        c.reply(2, 0);
        c.set(2, 0);
        return;
    case 0x101:  // CheckNew3DSApp
    case 0x102:  // CheckNew3DS
        c.reply(2, 0);
        c.set(2, 0);  // Old 3DS
        return;
    default:
        ipc::unimplemented(name, c);
    }
}

}  // namespace

void register_apt() {
    for (const char* name : {"APT:U", "APT:A", "APT:S"})
        kernel::register_service(name, [name](core::Memory& mem, u32 cmd) { handle(mem, cmd, name); });
}

}  // namespace hle::service
