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

// APT (applet manager): application start-up handshake and library applets (software keyboard).

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "core/savestate.h"
#include "hle/ipc.h"
#include "hle/service/services.h"

namespace hle::service {

namespace {

constexpr u32 APPID_HOMEMENU = 0x101;
constexpr u32 APPID_SWKBD = 0x401;
constexpr u32 SIGNAL_WAKEUP = 1, SIGNAL_REQUEST = 2, SIGNAL_RESPONSE = 3, SIGNAL_WAKEUP_BY_EXIT = 10;

struct Parameter {
    u32 sender, signal;
    std::vector<u8> buffer;
    Handle object = 0;
};

// Software keyboard: "types" a fixed name and confirms with the rightmost button, like a
// player accepting it at once. Layout of the 0x400-byte config: SoftwareKeyboardConfig as
// documented by 3dbrew/Citra (buttons at +0x04, results from +0x138).
// shortcut: fixed text, no on-screen keyboard; add a host text prompt when names matter.
void software_keyboard(core::Memory& mem, std::vector<u8>& config, Handle shared) {
    static constexpr char16_t NAME[] = u"Link";
    constexpr u32 LEN = 4;
    const auto put32 = [&](u32 off, u32 v) { std::memcpy(&config[off], &v, 4); };
    u16 max_len = 0;
    std::memcpy(&max_len, &config[0x20], 2);
    const u32 len = max_len ? std::min<u32>(LEN, max_len) : LEN;
    if (const u32 addr = kernel::shared_memory(shared).addr) {
        for (u32 i = 0; i < len; ++i) mem.write<u16>(addr + i * 2, NAME[i]);
        mem.write<u16>(addr + len * 2, 0);
    }
    u32 buttons_m1 = 0;
    std::memcpy(&buttons_m1, &config[0x04], 4);
    put32(0x138, buttons_m1 == 0 ? 0 : buttons_m1 == 1 ? 2 : 5);  // D0_CLICK / D1_CLICK1 / D2_CLICK2
    put32(0x144, 0);                                             // text offset in shared memory
    const u16 n = u16(len);
    std::memcpy(&config[0x148], &n, 2);  // text length
}

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
        g_parameters.push_back({APPID_HOMEMENU, SIGNAL_WAKEUP, {}});
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
        const u32 size = std::min<u32>(c[2], u32(p.buffer.size())), dst = c.static_buffer_addr(0);
        for (u32 i = 0; i < size; ++i) mem.write<u8>(dst + i, p.buffer[i]);
        c.reply(4, 4);
        c.set(2, p.sender), c.set(3, p.signal), c.set(4, size);  // sender, signal, buffer size
        c.set(5, ipc::move_handles(1)), c.set(6, p.object);
        c.set(7, ipc::static_buffer(size, 0)), c.set(8, dst);
        return;
    }
    case 0x0C:  // SendParameter(src, dst, signal, size, handle, buffer)
        if (c[2] != APPID_SWKBD) ipc::unimplemented(name, c);
        // A library applet answers the start-up Request with a Response carrying the memory
        // for its screen capture (size = first word of the CaptureBufferInfo sent along).
        if (c[3] == SIGNAL_REQUEST) {
            const u32 capture = c[4] >= 4 ? mem.read<u32>(c[8]) : 0;
            g_parameters.push_back({APPID_SWKBD, SIGNAL_RESPONSE, {}, kernel::create_shared_memory(std::max(capture, 0x1000u))});
            kernel::signal_event(g_parameter_event);
        }
        c.reply(1, 0);
        return;
    case 0x18:  // PrepareToStartLibraryApplet(applet id)
        if (c[1] != APPID_SWKBD) ipc::unimplemented(name, c);
        c.reply(1, 0);
        return;
    case 0x1E: {  // StartLibraryApplet(applet id, size, handle, buffer): runs to completion at once
        if (c[1] != APPID_SWKBD || c[2] < 0x14C) ipc::unimplemented(name, c);
        std::vector<u8> config(c[2]);
        for (u32 i = 0; i < c[2]; ++i) config[i] = mem.read<u8>(c[6] + i);
        software_keyboard(mem, config, c[4]);
        g_parameters.push_back({APPID_SWKBD, SIGNAL_WAKEUP_BY_EXIT, std::move(config)});
        kernel::signal_event(g_parameter_event);
        c.reply(1, 0);
        return;
    }
    case 0x40:  // SendCaptureBufferInfo(size, buffer): the HLE keyboard draws nothing
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
    kernel::register_state(
        [](core::StateWriter& w) {
            std::lock_guard lk(g_apt_lock);
            w.section("APT ");
            w.put(g_lock_handle), w.put(g_notification_event), w.put(g_parameter_event), w.put(g_attributes);
            w.put(u32(g_parameters.size()));
            for (auto& p : g_parameters) w.put(p.sender), w.put(p.signal), w.vec(p.buffer), w.put(p.object);
        },
        [](core::StateReader& r) {
            std::lock_guard lk(g_apt_lock);
            r.section("APT ");
            r.get(g_lock_handle), r.get(g_notification_event), r.get(g_parameter_event), r.get(g_attributes);
            g_parameters.resize(r.get<u32>());
            for (auto& p : g_parameters) r.get(p.sender), r.get(p.signal), p.buffer = r.vec<u8>(), r.get(p.object);
        });
}

}  // namespace hle::service
