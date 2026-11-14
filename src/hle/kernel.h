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

#include <filesystem>
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
// Human-readable state of every guest thread and what it waits on (diagnostics).
std::string dump_threads();
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

// ---- shared memory
// A block owned by an HLE service; `addr` is 0 until the guest maps it.
struct SharedMemoryInfo {
    u32 size;
    u32 addr;
};
Handle create_shared_memory(u32 size);
SharedMemoryInfo shared_memory(Handle h);  // {0, 0} if not a memory block
Result create_memory_block(u32 addr, u32 size, Handle* out);  // svc 0x1E (guest-owned memory)
Result map_memory_block(Handle h, u32 addr);                  // svc 0x1F
Result unmap_memory_block(Handle h, u32 addr);                // svc 0x20

// ---- host-driven periodic work (VBlank, ...)
// Callbacks run with guest execution excluded: from the host main loop while no guest
// thread is running (host_tick, ~1 kHz from a host thread), or from the current guest thread
// at a syscall.
void add_periodic(s64 period_ns, std::function<void()> fn);
void host_tick();

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
// A session with its own handler (e.g. an open file), freed when its handle is closed.
Handle open_session(const std::string& name, ServiceHandler handler);

// The calling guest thread's CPU context (for the svc dispatcher).
recomp::Context& current_context();

// ---- savestates
// Host state outside guest memory registers a saver/loader pair at service init; they run in
// registration order with no guest thread executing and the kernel lock released.
using StateSaver = std::function<void(core::StateWriter&)>;
using StateLoader = std::function<void(core::StateReader&)>;
void register_state(StateSaver save, StateLoader load);
// A session whose handler has its own state (an open file): `save` writes what `kind`'s
// loader needs to rebuild the handler.
Handle open_session(const std::string& name, ServiceHandler handler, StateSaver save, const std::string& kind);
struct RestoredSession {
    ServiceHandler handler;
    StateSaver save;  // so the session can be saved again
};
void register_session_loader(const std::string& kind, std::function<RestoredSession(core::StateReader&)> load);
// Requests are carried out by the guest thread holding the CPU at its next syscall (all others
// are parked then); a load replaces every guest thread. Callable from any host thread.
void request_save(const std::filesystem::path& file);
void request_load(const std::filesystem::path& file);
void savestate_point();  // top of every svc
std::string savestate_status();  // last result, for the UI (empty = nothing yet)

}  // namespace hle::kernel
