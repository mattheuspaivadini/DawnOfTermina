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

// dsp::DSP — control protocol of the audio DSP: the component "loads", the audio pipe hands
// out the shared-structure addresses on Initialize, and every audio frame (160 samples at
// 32728 Hz) runs the HLE firmware (hle/dsp.cpp) and raises the pipe interrupt.

#include <algorithm>
#include <deque>
#include <mutex>

#include "hle/dsp.h"
#include "hle/ipc.h"
#include "hle/service/services.h"

namespace hle::service {

namespace {

constexpr u32 PIPE_AUDIO = 2;

core::Memory* g_mem = nullptr;
std::mutex g_dsp_lock;
std::deque<u8> g_pipes[8];
Handle g_semaphore_event = 0, g_pipe_event[8] = {};
bool g_running = false;

u32 dsp_to_va(u32 dsp_addr) { return dsp::DSP_DRAM_VA + dsp_addr * 2; }

void audio_frame() {
    Handle ev;
    s16 samples[dsp::FRAME_SAMPLES * 2];
    {
        std::lock_guard lk(g_dsp_lock);
        if (!g_running) return;
        dsp::process_frame(*g_mem, samples);
        ev = g_pipe_event[PIPE_AUDIO];
    }
    dsp::output(samples, dsp::FRAME_SAMPLES);
    if (ev) kernel::signal_event(ev);
}

void dsp_command(core::Memory& mem, u32 base) {
    ipc::Cmd c{mem, base};
    std::unique_lock lk(g_dsp_lock);
    switch (c.id()) {
    case 0x01:  // RecvData(register) -> 1: DSP running
        c.reply(2, 0);
        c.set(2, 1);
        return;
    case 0x02:  // RecvDataIsReady(register)
        c.reply(2, 0);
        c.set(2, 1);
        return;
    case 0x07:  // SetSemaphore
    case 0x12:  // UnloadComponent
    case 0x13:  // FlushDataCache
    case 0x14:  // InvalidateDataCache
    case 0x17:  // SetSemaphoreMask
    case 0x20:  // ForceHeadphoneOut
        c.reply(1, 0);
        return;
    case 0x0C: {  // ConvertProcessAddressFromDspDram(dsp address); read before reply() overwrites it
        const u32 va = dsp_to_va(c[1]);
        c.reply(2, 0);
        c.set(2, va);
        return;
    }
    case 0x0D: {  // WriteProcessPipe(channel, size, static desc, address)
        const u32 channel = c[1] & 7, size = c[2], addr = c[4];
        if (channel == PIPE_AUDIO && size >= 4) {
            const u8 state = mem.read<u8>(addr);  // StateChange is the first byte
            if (state == 0 || state == 2) {  // Initialize / Wakeup: publish the 15 structures
                auto& p = g_pipes[PIPE_AUDIO];
                p.clear();
                auto push16 = [&](u16 v) { p.push_back(u8(v)), p.push_back(u8(v >> 8)); };
                push16(dsp::NUM_STRUCTS);
                for (u16 off : dsp::STRUCT_OFFSET) push16(u16(dsp::REGION0_WORD + off / 2));
                if (state == 0) dsp::reset();
                g_running = true;
            } else if (state == 1 || state == 3) {  // Shutdown / Sleep
                g_running = false;
            }
        }
        const Handle ev = g_pipe_event[channel];
        c.reply(1, 0);
        lk.unlock();
        if (ev) kernel::signal_event(ev);
        return;
    }
    case 0x0E:    // ReadPipe(channel, peer, size)
    case 0x10: {  // ReadPipeIfPossible(channel, peer, size) -> u16 read, static buffer
        auto& p = g_pipes[c[1] & 7];
        const u32 n = std::min<u32>(c[3] & 0xFFFF, u32(p.size()));
        const u32 addr = c.static_buffer_addr(0);
        for (u32 i = 0; i < n; ++i) mem.write<u8>(addr + i, p[i]);
        p.erase(p.begin(), p.begin() + n);
        if (c.id() == 0x0E) {
            c.reply(1, 2);
            c.set(2, ipc::static_buffer(n, 0)), c.set(3, addr);
        } else {
            c.reply(2, 2);
            c.set(2, n), c.set(3, ipc::static_buffer(n, 0)), c.set(4, addr);
        }
        return;
    }
    case 0x0F:  // GetPipeReadableSize(channel, peer)
        c.reply(2, 0);
        c.set(2, u32(g_pipes[c[1] & 7].size()));
        return;
    case 0x11: {  // LoadComponent(size, prog mask, data mask, mapped buffer desc, address) -> loaded
        const u32 desc = c[4], addr = c[5];
        c.reply(2, 2);
        c.set(2, 1), c.set(3, desc), c.set(4, addr);
        return;
    }
    case 0x15: {  // RegisterInterruptEvents(interrupt, channel, copy desc, event)
        const u32 channel = c[2] & 7;
        g_pipe_event[channel] = c[4];
        c.reply(1, 0);
        return;
    }
    case 0x16:  // GetSemaphoreEventHandle
        if (!g_semaphore_event) {
            lk.unlock();
            const Handle h = kernel::create_event(0);
            lk.lock();
            g_semaphore_event = h;
        }
        c.reply(1, 2);
        c.set(2, ipc::copy_handles(1)), c.set(3, g_semaphore_event);
        return;
    case 0x1F:  // GetHeadphoneStatus
    case 0x21:  // GetIsDspOccupied
        c.reply(2, 0);
        c.set(2, 0);
        return;
    default:
        ipc::unimplemented("dsp::DSP", c);
    }
}

}  // namespace

void register_dsp(core::Memory& mem) {
    g_mem = &mem;
    kernel::register_service("dsp::DSP", dsp_command);
    kernel::add_periodic(4'888'000, audio_frame);  // 160 samples at 32728 Hz
}

}  // namespace hle::service
