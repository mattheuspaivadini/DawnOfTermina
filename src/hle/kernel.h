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

#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/memory.h"

namespace recomp {
struct Context;
}

// Horizon kernel objects, threads and IPC, at the syscall boundary used by recompiled
// code. Every guest thread runs on its own host thread, but only the thread holding the
// "baton" (the current thread) executes guest code: switches happen when it blocks or
// yields inside a syscall, choosing the highest-priority ready thread (lower = higher).
namespace hle::kernel {

struct Unimplemented : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr Result ERR_INVALID_HANDLE = 0xD8E007F7;
constexpr Result ERR_NOT_FOUND = 0xD88007FA;
constexpr Result ERR_TIMEOUT = 0x09401BFE;
constexpr Result ERR_NOT_AUTHORIZED = 0xD8E007F1;
constexpr Result ERR_OUT_OF_RANGE = 0xD8E007FD;
constexpr Handle CURRENT_PROCESS = 0xFFFF8001, CURRENT_THREAD = 0xFFFF8000;

void init(core::Memory& mem);

// ---- threads
// Starts the main guest thread at `entry` (on a new host thread) and returns immediately.
void start_main_thread(u32 entry, u32 stack_top, s32 priority);
Result create_thread(u32 entry, u32 arg, u32 stack_top, s32 priority, s32 core, Handle* out);
[[noreturn]] void exit_thread();
[[noreturn]] void exit_process();  // stops all guest threads; the host main loop then quits
void sleep_thread(s64 ns);
Result get_thread_id(Handle thread, u32* out);
Result get_thread_priority(Handle thread, s32* out);
Result set_thread_priority(Handle thread, s32 priority);
// Called at every syscall: lets a higher-priority thread that became ready run first.
void preemption_point();

// Status for the host main loop: set once the guest stops (exit or error).
struct Status {
    bool exited = false;
    std::string error;  // non-empty if the guest hit something unimplemented
};
std::optional<Status> stopped();
// Records a fatal error raised on a guest thread and stops scheduling.
void fail(const std::string& message);

// ---- synchronisation objects
Handle create_event(u32 reset_type);
Handle create_mutex(bool locked);
Handle create_semaphore(s32 initial, s32 max);
Handle create_address_arbiter();
Result signal_event(Handle h);  // also callable from the host main thread
Result clear_event(Handle h);
Result release_mutex(Handle h);
Result release_semaphore(Handle h, s32 count, s32* previous);
Result close_handle(Handle h);
Result duplicate_handle(Handle h, Handle* out);
Result wait_synchronization(Handle h, s64 timeout_ns);
Result wait_synchronization_n(const u32* handles, u32 count, bool wait_all, s64 timeout_ns, s32* index);
Result arbitrate_address(Handle arbiter, u32 addr, u32 type, s32 value, s64 timeout_ns);

// ---- resource limits (process commit accounting lives in hle::svc)
Result get_resource_limit(Handle process, Handle* out);
Result get_resource_limit_values(u32 values_va, Handle limit, u32 names_va, u32 count, bool current);

// ---- IPC
Result connect_to_port(const std::string& name, Handle* out);
Result send_sync_request(Handle session);

// A service handles one request whose command buffer is at `cmd` (guest TLS + 0x80).
using ServiceHandler = std::function<void(core::Memory& mem, u32 cmd)>;
void register_service(const std::string& name, ServiceHandler handler);
Handle open_session(const std::string& service);

// The calling guest thread's CPU context (for the svc dispatcher).
recomp::Context& current_context();

}  // namespace hle::kernel
