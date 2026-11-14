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

// ir:USER with a Circle Pad Pro always attached: the New 3DS's built-in C-stick (and ZL/ZR)
// reaches games written for the Circle Pad Pro this way. Protocol from 3dbrew ("IRUSER
// Shared Memory", "Circle Pad Pro") as implemented by Citra's ExtraHID.
//
// The game hands over a shared block (InitializeIrNopShared): a 0x10-byte status header,
// then the receive ring {begin, end, count, pad} + `count` packet infos {offset, size} + data.
// Packets the "device" sends are {0xA5, network id, size (1 or 2 bytes), payload, CRC-8}.
// Requests: 1 = start polling every N ms, 2 = read calibration data. Each poll answers with
// 6 bytes: 0x10 | x << 8 | y << 20 (12-bit stick, centre 0x800), then battery and the
// active-low ZL/ZR/R bits.

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "hle/hid.h"
#include "hle/ipc.h"
#include "hle/service/services.h"
#include "hle/svc.h"

namespace hle::service {

namespace {

core::Memory* g_mem = nullptr;
std::mutex g_lock;  // IPC handlers run on guest threads, the poll from the periodic tick
Handle g_events[3] = {};  // receive, send, connection status
u32 g_shared = 0, g_recv_size = 0, g_packets = 0;  // guest block; receive ring size and slots
u32 g_begin = 0, g_end = 0, g_count = 0;
bool g_connected = false;
u32 g_period_ms = 0;  // HID polling requested by the game
u64 g_next_poll = 0;  // system tick of the next sample

// Header fields (byte offsets).
constexpr u32 CONNECTION_STATUS = 8, TRYING_STATUS = 9, ROLE = 10, CONNECTED = 12, NETWORK_ID = 13, INITIALIZED = 14;
constexpr u32 RECV_INFO = 0x10, RECV_BUF = 0x20;

// Calibration block read from a New 3DS (Citra).
constexpr u8 CALIBRATION[0x40] = {
    0x00, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0xF5,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
};

u8 crc8(const std::vector<u8>& d) {  // polynomial 0x07, initial value 0
    u8 crc = 0;
    for (u8 b : d) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) crc = u8(crc & 0x80 ? crc << 1 ^ 0x07 : crc << 1);
    }
    return crc;
}

// Appends a packet from the device to the receive ring (Citra's BufferManager::Put).
void send(const std::vector<u8>& payload) {
    if (!g_shared || !g_packets) return;
    std::vector<u8> p = {0xA5, g_mem->read<u8>(g_shared + NETWORK_ID)};
    if (payload.size() < 0x40) p.push_back(u8(payload.size()));
    else p.push_back(u8(payload.size() >> 8 | 0x40)), p.push_back(u8(payload.size()));
    p.insert(p.end(), payload.begin(), payload.end());
    p.push_back(crc8(p));

    const u32 infos = g_shared + RECV_BUF, data = infos + 8 * g_packets;
    const u32 data_size = g_recv_size - 8 * g_packets;
    if (g_count == g_packets) return;  // full: the game has not released old packets
    u32 at = 0;
    if (g_count) {
        const u32 last = infos + 8 * ((g_end + g_packets - 1) % g_packets);
        at = (g_mem->read<u32>(last) + g_mem->read<u32>(last + 4)) % data_size;
        const u32 first = g_mem->read<u32>(infos + 8 * g_begin);
        if (p.size() > (first + data_size - at) % data_size) return;
    } else if (p.size() > data_size) {
        return;
    }
    g_mem->write<u32>(infos + 8 * g_end, at), g_mem->write<u32>(infos + 8 * g_end + 4, u32(p.size()));
    for (size_t i = 0; i < p.size(); ++i) g_mem->write<u8>(data + (at + i) % data_size, p[i]);
    g_end = (g_end + 1) % g_packets, ++g_count;
    if (std::getenv("DOT_TRACE_IR")) std::fprintf(stderr, "ir:USER -> %zu bytes at %u, %u queued\n", p.size(), at, g_count);
    g_mem->write<u32>(g_shared + RECV_INFO, g_begin), g_mem->write<u32>(g_shared + RECV_INFO + 4, g_end);
    g_mem->write<u32>(g_shared + RECV_INFO + 8, g_count);
    if (g_events[0]) kernel::signal_event(g_events[0]);
}

// One C-stick sample (runs from the periodic tick, guest execution excluded).
void poll() {
    std::lock_guard lk(g_lock);
    const u64 now = hle::svc::GetSystemTick();
    if (!g_connected || !g_period_ms || now < g_next_poll) return;
    g_next_poll = now + u64(g_period_ms) * 268'112;  // 268.111856 MHz system tick;
    const auto pad = hle::hid::ReadPad();
    const u32 x = u32(0x800 + 0x7FF * pad.c_x) & 0xFFF, y = u32(0x800 + 0x7FF * pad.c_y) & 0xFFF;
    const u32 stick = 0x10 | x << 8 | y << 20;
    const u8 buttons = 0x1F | !(pad.hold & hle::hid::ZL) << 5 | !(pad.hold & hle::hid::ZR) << 6 | 1 << 7;
    send({u8(stick), u8(stick >> 8), u8(stick >> 16), u8(stick >> 24), buttons, 0});
}

// A request from the game to the device (SendIrNop payload).
void receive(const std::vector<u8>& r) {
    if (r.empty()) return;
    if (r[0] == 1 && r.size() == 3) {  // ConfigureHIDPolling(period ms, ...)
        // The game repeats this request until samples arrive: keep the schedule, first one now.
        if (!g_period_ms && r[1]) std::fprintf(stderr, "ir:USER: Circle Pad Pro polled every %u ms\n", r[1]);
        if (!g_period_ms) g_next_poll = 0;
        g_period_ms = r[1];
    } else if (r[0] == 2 && r.size() == 6) {  // ReadCalibrationData(response time, u16 offset, u16 size)
        const u32 offset = (r[2] | r[3] << 8) & ~15u, size = (r[4] | r[5] << 8) & ~15u;
        if (offset + size > sizeof CALIBRATION) return;
        std::vector<u8> out = {0x11, r[2], r[3], r[4], r[5]};
        out.insert(out.end(), CALIBRATION + offset, CALIBRATION + offset + size);
        send(out);
    } else {
        std::fprintf(stderr, "ir:USER: unknown Circle Pad Pro request %02X (%zu bytes)\n", r[0], r.size());
    }
}

void signal(int i) {
    if (g_events[i]) kernel::signal_event(g_events[i]);
}

void ir_command(core::Memory& mem, u32 base) {
    ipc::Cmd c{mem, base};
    std::lock_guard lk(g_lock);
    static const bool trace = std::getenv("DOT_TRACE_IR");
    if (trace) {
        std::fprintf(stderr, "ir:USER %08X", c.header());
        for (u32 i = 1; i <= (c.header() >> 6 & 63) + (c.header() & 63) && i < 9; ++i) std::fprintf(stderr, " %08X", c[i]);
        if (c.id() == 0x0D)
            for (u32 i = 0; i < c[1] && i < 16; ++i) std::fprintf(stderr, i ? " %02X" : " | %02X", mem.read<u8>(c[3] + i));
        std::fprintf(stderr, " (recv %u/%u)\n", g_count, g_packets);
    }
    switch (c.id()) {
    case 0x0A: case 0x0B: case 0x0C: {  // Get{Receive,Send,ConnectionStatus}Event
        Handle& e = g_events[c.id() - 0x0A];
        if (!e) e = kernel::create_event(0);
        c.reply(1, 2);
        c.set(2, ipc::copy_handles(1)), c.set(3, e);
        return;
    }
    case 0x18: {  // InitializeIrNopShared(shared size, recv size, recv count, send size, send count, baud, handle)
        g_shared = kernel::shared_memory(c[8]).addr;
        g_recv_size = c[2], g_packets = c[3];
        g_begin = g_end = g_count = 0, g_connected = false, g_period_ms = 0;
        if (g_shared) {
            for (u32 i = 0; i < 0x20; i += 4) mem.write<u32>(g_shared + i, 0);
            mem.write<u8>(g_shared + INITIALIZED, 1);
        }
        c.reply(1, 0);
        return;
    }
    case 0x06:  // RequireConnection(device id): 1 = Circle Pad Pro
    case 0x07:  // AutoConnection
        if (g_shared) {
            mem.write<u8>(g_shared + CONNECTION_STATUS, 2), mem.write<u8>(g_shared + ROLE, 2);
            mem.write<u8>(g_shared + CONNECTED, 1), mem.write<u8>(g_shared + TRYING_STATUS, 0);
            if (!g_connected) std::fprintf(stderr, "ir:USER: Circle Pad Pro connected\n");
            g_connected = true;
            signal(2);
        }
        c.reply(1, 0);
        return;
    case 0x09:  // Disconnect
        if (g_connected) signal(2);
        g_connected = false, g_period_ms = 0;
        if (g_shared) mem.write<u8>(g_shared + CONNECTION_STATUS, 0), mem.write<u8>(g_shared + CONNECTED, 0);
        c.reply(1, 0);
        return;
    case 0x0D: {  // SendIrNop(size, static buffer)
        if (!g_connected) {
            c.reply(1, 0, 0xC8A10C0D);  // invalid state: not connected
            return;
        }
        const u32 size = c[1], addr = c[3];
        std::vector<u8> r(size);
        for (u32 i = 0; i < size; ++i) r[i] = mem.read<u8>(addr + i);
        receive(r);
        signal(1);
        c.reply(1, 0);
        return;
    }
    case 0x19:  // ReleaseReceivedData(count)
        if (c[1] > g_count) {
            c.reply(1, 0, 0xD8A10C0D);  // not found: no data
            return;
        }
        g_count -= c[1], g_begin = (g_begin + c[1]) % (g_packets ? g_packets : 1);
        if (g_shared)
            mem.write<u32>(g_shared + RECV_INFO, g_begin), mem.write<u32>(g_shared + RECV_INFO + 8, g_count);
        c.reply(1, 0);
        return;
    case 0x02:  // FinalizeIrNop
        g_shared = 0, g_connected = false, g_period_ms = 0;
        c.reply(1, 0);
        return;
    default: {  // the rest (receive/send buffer clears, machine id...) need no state
        static bool seen[0x20] = {};
        if (c.id() < 0x20 && !seen[c.id()]) {
            seen[c.id()] = true;
            std::fprintf(stderr, "hle: ir:USER 0x%08X (stub: success)\n", c.header());
        }
        c.reply(1, 0);
    }
    }
}

}  // namespace

void register_ir(core::Memory& mem) {
    g_mem = &mem;
    kernel::register_service("ir:USER", ir_command);
    kernel::add_periodic(1'000'000, poll);  // 1 ms granularity; the game picks the period (16 ms)
}

}  // namespace hle::service
