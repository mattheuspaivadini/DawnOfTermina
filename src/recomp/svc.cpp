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

// Horizon syscall ABI (arguments/results in r0-r5) -> hle::svc / hle::kernel.

#include <cstdio>
#include <string>

#include "core/memory.h"
#include "hle/kernel.h"
#include "hle/svc.h"
#include "recomp/runtime.h"

namespace recomp {

static core::Memory* g_mem = nullptr;
void set_svc_memory(core::Memory& mem) { g_mem = &mem; }

static s64 ns64(u32 lo, u32 hi) { return s64(u64(hi) << 32 | lo); }

void svc(Context* c, u32 imm) {
    namespace k = hle::kernel;
    u32* r = c->r;
    k::preemption_point();
    switch (imm) {
    case 0x01: {  // ControlMemory(op r0, addr0 r1, addr1 r2, size r3, perm r4) -> r1 addr
        u32 out = 0;
        r[0] = hle::svc::ControlMemory(*g_mem, &out, r[1], r[2], r[3], r[0], r[4]);
        r[1] = out;
        return;
    }
    case 0x02: {  // QueryMemory(addr r2) -> r1 base, r2 size, r3 perm, r4 state, r5 page flags
        const auto info = hle::svc::QueryMemory(r[2]);
        r[0] = RESULT_OK, r[1] = info.base, r[2] = info.size, r[3] = info.perm, r[4] = info.state, r[5] = 0;
        return;
    }
    case 0x03: k::exit_process();
    case 0x08: {  // CreateThread(prio r0, entry r1, arg r2, stack top r3, core r4) -> r1 handle
        Handle h = 0;
        r[0] = k::create_thread(r[1], r[2], r[3], s32(r[0]), s32(r[4]), &h);
        r[1] = h;
        return;
    }
    case 0x09: k::exit_thread();
    case 0x0A: k::sleep_thread(ns64(r[0], r[1])); return;
    case 0x0B: {  // GetThreadPriority(handle r1) -> r1
        s32 prio = 0;
        r[0] = k::get_thread_priority(r[1], &prio);
        r[1] = u32(prio);
        return;
    }
    case 0x0C: r[0] = k::set_thread_priority(r[0], s32(r[1])); return;
    case 0x13: r[1] = k::create_mutex(r[1] != 0), r[0] = RESULT_OK; return;
    case 0x14: r[0] = k::release_mutex(r[0]); return;
    case 0x15: r[1] = k::create_semaphore(s32(r[1]), s32(r[2])), r[0] = RESULT_OK; return;
    case 0x16: {
        s32 prev = 0;
        r[0] = k::release_semaphore(r[1], s32(r[2]), &prev);
        r[1] = u32(prev);
        return;
    }
    case 0x17: r[1] = k::create_event(r[1]), r[0] = RESULT_OK; return;
    case 0x18: r[0] = k::signal_event(r[0]); return;
    case 0x19: r[0] = k::clear_event(r[0]); return;
    case 0x1E: {  // CreateMemoryBlock(other perm r0, addr r1, size r2, my perm r3) -> r1 handle
        Handle h = 0;
        r[0] = k::create_memory_block(r[1], r[2], &h);
        r[1] = h;
        return;
    }
    case 0x1F: r[0] = k::map_memory_block(r[0], r[1]); return;
    case 0x20: r[0] = k::unmap_memory_block(r[0], r[1]); return;
    case 0x21: r[1] = k::create_address_arbiter(), r[0] = RESULT_OK; return;
    case 0x22: r[0] = k::arbitrate_address(r[0], r[1], r[2], s32(r[3]), ns64(r[4], r[5])); return;
    case 0x23: r[0] = k::close_handle(r[0]); return;
    case 0x24: r[0] = k::wait_synchronization(r[0], ns64(r[2], r[3])); return;
    case 0x25: {  // WaitSynchronizationN(timeout lo r0, handles r1, count r2, wait_all r3, timeout hi r4)
        s32 index = -1;
        r[0] = k::wait_synchronization_n(reinterpret_cast<const u32*>(g_mem->host(r[1])), r[2], r[3] != 0,
                                         ns64(r[0], r[4]), &index);
        r[1] = u32(index);
        return;
    }
    case 0x27: {
        Handle out = 0;
        r[0] = k::duplicate_handle(r[1], &out);
        r[1] = out;
        return;
    }
    case 0x28: {
        const u64 t = hle::svc::GetSystemTick();
        r[0] = u32(t), r[1] = u32(t >> 32);
        return;
    }
    case 0x2A:  // GetSystemInfo(type r1, param r2) -> s64 in r1:r2
        if (r[1] == 0 && r[2] == 1) {  // memory used by the APPLICATION region
            r[0] = RESULT_OK, r[1] = hle::svc::CommittedMemory(), r[2] = 0;
            return;
        }
        r[0] = RESULT_OK, r[1] = 0, r[2] = 0;
        return;
    case 0x2B:  // GetProcessInfo(handle r1, type r2) -> s64 in r1:r2
        switch (r[2]) {
        case 0: case 2: r[1] = hle::svc::CommittedMemory(); break;  // private / used memory
        case 20: r[1] = 0x20000000u - core::Memory::LINEAR_BASE; break;  // linear PA - VA offset (Old 3DS)
        default: r[1] = 0; break;
        }
        r[0] = RESULT_OK, r[2] = 0;
        return;
    case 0x2D: {  // ConnectToPort(name r1) -> r1 handle
        Handle out = 0;
        r[0] = k::connect_to_port(reinterpret_cast<const char*>(g_mem->host(r[1])), &out);
        r[1] = out;
        return;
    }
    case 0x32: r[0] = k::send_sync_request(r[0]); return;
    case 0x35: r[0] = RESULT_OK, r[1] = 0x28; return;  // GetProcessId: any stable application pid
    case 0x37: {  // GetThreadId(handle r1) -> r1
        u32 id = 0;
        r[0] = k::get_thread_id(r[1], &id);
        r[1] = id;
        return;
    }
    case 0x38: {
        Handle out = 0;
        r[0] = k::get_resource_limit(r[1], &out);
        r[1] = out;
        return;
    }
    case 0x39: r[0] = k::get_resource_limit_values(r[0], r[1], r[2], r[3], false); return;
    case 0x3A: r[0] = k::get_resource_limit_values(r[0], r[1], r[2], r[3], true); return;
    case 0x3C: hle::svc::Break(r[0]);
    case 0x3D: hle::svc::OutputDebugString(*g_mem, r[0], r[1]); return;
    default: {
        char msg[96];
        std::snprintf(msg, sizeof msg, "svc 0x%02X (r0=0x%08X r1=0x%08X) not implemented, LR=0x%08X", imm, r[0],
                      r[1], r[14]);
        throw Trap(c->r[14], msg);
    }
    }
}

}  // namespace recomp
