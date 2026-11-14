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

// hid:USER — input through shared memory: a ring of 8 pad states (buttons + circle pad)
// and touch states, refreshed periodically from the host input (hle::hid; the mouse is the stylus).

#include "hle/hid.h"
#include "hle/ipc.h"
#include "hle/service/services.h"
#include "hle/svc.h"

namespace hle::service {

namespace {

constexpr u32 SHARED_SIZE = 0x1000;
constexpr u32 PAD = 0x00, TOUCH = 0xA8, ACCEL = 0x108, GYRO = 0x158;  // section offsets in shared memory

core::Memory* g_mem = nullptr;
Handle g_shared = 0, g_pad0 = 0, g_pad1 = 0, g_accel = 0, g_gyro = 0, g_debug = 0;
u32 g_last_buttons = 0;
bool g_accel_on = false, g_gyro_on = false;

// Ring header {s64 reset tick, s64 previous reset tick, u32 index}: advances and returns the index.
u32 advance(u32 section, u32 entries, u64 tick) {
    const u32 index = (g_mem->read<u32>(section + 0x10) + 1) % entries;
    if (index == 0) {
        g_mem->write<u64>(section + 0x08, g_mem->read<u64>(section));
        g_mem->write<u64>(section, tick);
    }
    g_mem->write<u32>(section + 0x10, index);
    return index;
}

void write_xyz(u32 at, s16 x, s16 y, s16 z) {
    g_mem->write<s16>(at, x), g_mem->write<s16>(at + 2, y), g_mem->write<s16>(at + 4, z);
}

void ensure_objects() {
    if (g_shared) return;
    g_shared = kernel::create_shared_memory(SHARED_SIZE);
    g_pad0 = kernel::create_event(0), g_pad1 = kernel::create_event(0);
    g_accel = kernel::create_event(0), g_gyro = kernel::create_event(0), g_debug = kernel::create_event(0);
}

// Appends the current host input to the shared-memory rings and signals the sampling
// events (runs with guest execution excluded).
void update() {
    if (!g_pad0) return;
    const u32 base = g_shared ? kernel::shared_memory(g_shared).addr : 0;
    if (base) {
        const auto pad = hle::hid::ReadPad();
        const u64 tick = hle::svc::GetSystemTick();

        // Pad header: ring header, pad[2], f32 3D slider, u32 buttons, u32 raw circle pad, pad;
        // then 8 entries {buttons, pressed, released, s16 x, s16 y}.
        const u32 entry = base + PAD + 0x28 + advance(base + PAD, 8, tick) * 0x10;
        g_mem->write<u32>(entry + 0x0, pad.hold);
        g_mem->write<u32>(entry + 0x4, pad.hold & ~g_last_buttons);
        g_mem->write<u32>(entry + 0x8, g_last_buttons & ~pad.hold);
        g_mem->write<s16>(entry + 0xC, pad.stick_x);
        g_mem->write<s16>(entry + 0xE, pad.stick_y);
        g_mem->write<u32>(base + PAD + 0x1C, pad.hold);
        g_mem->write<s16>(base + PAD + 0x20, pad.stick_x);
        g_mem->write<s16>(base + PAD + 0x22, pad.stick_y);
        g_last_buttons = pad.hold;

        // Touch: ring header, raw entry, then 8 entries {u16 x, u16 y, u32 valid}.
        const u64 touch = pad.touch ? u64(pad.touch_x) | u64(pad.touch_y) << 16 | 1ull << 32 : 0;
        g_mem->write<u64>(base + TOUCH + 0x18, touch);
        g_mem->write<u64>(base + TOUCH + 0x20 + advance(base + TOUCH, 8, tick) * 8, touch);

        // Accelerometer: raw entry + 8 entries of s16 xyz; gyroscope: 32 entries. Console at rest.
        if (g_accel_on) write_xyz(base + ACCEL + 0x20 + advance(base + ACCEL, 8, tick) * 6, 0, 0, -512);
        if (g_gyro_on) write_xyz(base + GYRO + 0x18 + advance(base + GYRO, 32, tick) * 6, 0, 0, 0);
    }
    kernel::signal_event(g_pad0);
    kernel::signal_event(g_pad1);
    if (g_accel_on) kernel::signal_event(g_accel);
    if (g_gyro_on) kernel::signal_event(g_gyro);
}

void hid_command(core::Memory& mem, u32 base) {
    ipc::Cmd c{mem, base};
    switch (c.id()) {
    case 0x0A:  // GetIPCHandles -> shared memory + pad0, pad1, accelerometer, gyroscope, debug pad events
        ensure_objects();
        c.reply(1, 7);
        c.set(2, ipc::copy_handles(6)), c.set(3, g_shared);
        c.set(4, g_pad0), c.set(5, g_pad1), c.set(6, g_accel), c.set(7, g_gyro), c.set(8, g_debug);
        return;
    case 0x11:  // EnableAccelerometer
    case 0x12:  // DisableAccelerometer
        g_accel_on = c.id() == 0x11;
        c.reply(1, 0);
        return;
    case 0x13:  // EnableGyroscopeLow
    case 0x14:  // DisableGyroscopeLow
        g_gyro_on = c.id() == 0x13;
        c.reply(1, 0);
        return;
    case 0x15:  // GetGyroscopeLowRawToDpsCoefficient -> float
        c.reply(2, 0);
        c.set(2, 0x41660000);  // 14.375f
        return;
    case 0x16: {  // GetGyroscopeLowCalibrateParam -> 0x14 bytes inline: {s16 zero, +unit, -unit} x/y/z
        const s16 p[10] = {0, 6700, -6700, 0, 6700, -6700, 0, 6700, -6700, 0};
        c.reply(6, 0);
        for (u32 i = 0; i < 5; ++i) c.set(2 + i, u32(u16(p[2 * i])) | u32(u16(p[2 * i + 1])) << 16);
        return;
    }
    case 0x17:  // GetSoundVolume -> u8 (slider at max)
        c.reply(2, 0);
        c.set(2, 0x3F);
        return;
    default:
        ipc::unimplemented("hid:USER", c);
    }
}

}  // namespace

void register_hid(core::Memory& mem) {
    g_mem = &mem;
    for (const char* name : {"hid:USER", "hid:SPVR"}) kernel::register_service(name, hid_command);
    kernel::add_periodic(4'000'000, update);  // 250 Hz, like the HID module's pad sampling
}

}  // namespace hle::service
