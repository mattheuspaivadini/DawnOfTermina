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

#include "hle/kernel.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <variant>

#include "hle/svc.h"

namespace hle::kernel {

namespace {

struct Event { u32 reset_type; bool signaled = false; };
struct Mutex { bool locked; };
struct Semaphore { s32 count, max; };
struct AddressArbiter {};
struct ResourceLimit {};
struct Session { std::string service; };
struct Thread { u32 id; };
struct Process { u32 id; };
using Object = std::variant<Event, Mutex, Semaphore, AddressArbiter, ResourceLimit, Session, Thread, Process>;

core::Memory* g_mem = nullptr;
std::map<Handle, std::shared_ptr<Object>> g_handles;
std::map<std::string, ServiceHandler> g_services;
Handle g_next = 0x100;

constexpr u32 MAIN_THREAD_ID = 1, PROCESS_ID = 0x28;

Handle add(Object obj) {
    const Handle h = g_next++;
    g_handles[h] = std::make_shared<Object>(std::move(obj));
    return h;
}

Object* get(Handle h) {
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second.get();
}

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// Single-thread wait: succeeds if the object is ready now, otherwise times out, or
// reports a deadlock for infinite waits.
Result try_acquire(Handle h, bool consume) {
    Object* o = get(h);
    if (!o) return ERR_INVALID_HANDLE;
    if (auto* e = std::get_if<Event>(o)) {
        if (!e->signaled) return ERR_TIMEOUT;
        if (consume && e->reset_type == 0) e->signaled = false;  // ONESHOT
        return RESULT_OK;
    }
    if (auto* m = std::get_if<Mutex>(o)) {
        if (m->locked) return ERR_TIMEOUT;
        if (consume) m->locked = true;
        return RESULT_OK;
    }
    if (auto* s = std::get_if<Semaphore>(o)) {
        if (s->count <= 0) return ERR_TIMEOUT;
        if (consume) --s->count;
        return RESULT_OK;
    }
    throw Unimplemented("wait on unsupported object handle " + hex(h));
}

}  // namespace

void init(core::Memory& mem) {
    g_mem = &mem;
    g_handles.clear();
    g_next = 0x100;
}

Handle create_event(u32 reset_type) { return add(Event{reset_type}); }
Handle create_mutex(bool locked) { return add(Mutex{locked}); }
Handle create_semaphore(s32 initial, s32 max) { return add(Semaphore{initial, max}); }
Handle create_address_arbiter() { return add(AddressArbiter{}); }

Result signal_event(Handle h) {
    auto* e = std::get_if<Event>(get(h));
    if (!e) return ERR_INVALID_HANDLE;
    e->signaled = true;
    return RESULT_OK;
}

Result clear_event(Handle h) {
    auto* e = std::get_if<Event>(get(h));
    if (!e) return ERR_INVALID_HANDLE;
    e->signaled = false;
    return RESULT_OK;
}

Result release_mutex(Handle h) {
    auto* m = std::get_if<Mutex>(get(h));
    if (!m) return ERR_INVALID_HANDLE;
    m->locked = false;
    return RESULT_OK;
}

Result release_semaphore(Handle h, s32 count, s32* previous) {
    auto* s = std::get_if<Semaphore>(get(h));
    if (!s) return ERR_INVALID_HANDLE;
    *previous = s->count;
    s->count = std::min(s->count + count, s->max);
    return RESULT_OK;
}

Result close_handle(Handle h) { return g_handles.erase(h) ? RESULT_OK : ERR_INVALID_HANDLE; }

Result duplicate_handle(Handle h, Handle* out) {
    if (h == CURRENT_THREAD) return *out = add(Thread{MAIN_THREAD_ID}), RESULT_OK;
    if (h == CURRENT_PROCESS) return *out = add(Process{PROCESS_ID}), RESULT_OK;
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return ERR_INVALID_HANDLE;
    *out = g_next++;
    g_handles[*out] = it->second;
    return RESULT_OK;
}

Result wait_synchronization(Handle h, s64 timeout_ns) {
    const Result r = try_acquire(h, true);
    if (r != ERR_TIMEOUT) return r;
    if (timeout_ns < 0) throw Unimplemented("deadlock: infinite wait on handle " + hex(h) + " with a single thread");
    hle::svc::SleepThread(timeout_ns);
    return ERR_TIMEOUT;
}

Result wait_synchronization_n(const u32* handles, u32 count, bool wait_all, s64 timeout_ns, s32* index) {
    if (wait_all) {
        for (u32 i = 0; i < count; ++i)
            if (try_acquire(handles[i], false) != RESULT_OK) goto not_ready;
        for (u32 i = 0; i < count; ++i) try_acquire(handles[i], true);
        *index = -1;
        return RESULT_OK;
    }
    for (u32 i = 0; i < count; ++i)
        if (try_acquire(handles[i], true) == RESULT_OK) {
            *index = s32(i);
            return RESULT_OK;
        }
not_ready:
    if (timeout_ns < 0) throw Unimplemented("deadlock: infinite WaitSynchronizationN with a single thread");
    hle::svc::SleepThread(timeout_ns);
    return ERR_TIMEOUT;
}

Result arbitrate_address(Handle arbiter, u32 addr, u32 type, s32 value, s64 timeout_ns) {
    if (!std::get_if<AddressArbiter>(get(arbiter))) return ERR_INVALID_HANDLE;
    const s32 cur = s32(g_mem->read<u32>(addr));
    switch (type) {
    case 0: return RESULT_OK;  // SIGNAL: no other thread can be waiting
    case 1: case 2: case 3: case 4:  // WAIT_IF_LESS_THAN (+timeout), DECREMENT_AND_WAIT_IF_LESS_THAN (+timeout)
        if (type >= 3) g_mem->write<u32>(addr, u32(cur - 1));
        if (cur < value) {
            if (type == 2 || type == 4) {
                hle::svc::SleepThread(timeout_ns);
                return ERR_TIMEOUT;
            }
            throw Unimplemented("deadlock: ArbitrateAddress wait at " + hex(addr) + " with a single thread");
        }
        return RESULT_OK;
    default:
        throw Unimplemented("ArbitrateAddress type " + std::to_string(type));
    }
}

static bool is_main_thread(Handle h) {
    if (h == CURRENT_THREAD) return true;
    auto* t = std::get_if<Thread>(get(h));
    return t && t->id == MAIN_THREAD_ID;
}

static s32 g_main_priority = 0x30;

Result get_thread_id(Handle thread, u32* out) {
    if (!is_main_thread(thread)) return ERR_INVALID_HANDLE;
    *out = MAIN_THREAD_ID;
    return RESULT_OK;
}

Result get_thread_priority(Handle thread, s32* out) {
    if (!is_main_thread(thread)) return ERR_INVALID_HANDLE;
    *out = g_main_priority;
    return RESULT_OK;
}

Result set_thread_priority(Handle thread, s32 priority) {
    if (!is_main_thread(thread)) return ERR_INVALID_HANDLE;
    g_main_priority = priority;
    return RESULT_OK;
}

Result get_resource_limit(Handle process, Handle* out) {
    if (process != CURRENT_PROCESS) return ERR_INVALID_HANDLE;
    *out = add(ResourceLimit{});
    return RESULT_OK;
}

Result get_resource_limit_values(u32 values_va, Handle limit, u32 names_va, u32 count, bool current) {
    if (!std::get_if<ResourceLimit>(get(limit))) return ERR_INVALID_HANDLE;
    for (u32 i = 0; i < count; ++i) {
        const u32 name = g_mem->read<u32>(names_va + i * 4);
        s64 v;
        switch (name) {
        case 0: v = current ? 0 : 0x18; break;  // priority
        case 1: v = current ? hle::svc::CommittedMemory() : g_mem->read<u32>(core::Memory::CONFIG_MEM + 0x40); break;
        case 2: v = current ? 1 : 32; break;    // threads
        default: v = current ? 0 : 32; break;   // events, mutexes, semaphores, timers, shared mem, ...
        }
        g_mem->write<u64>(values_va + i * 8, u64(v));
    }
    return RESULT_OK;
}

void register_service(const std::string& name, ServiceHandler handler) { g_services[name] = std::move(handler); }

Handle open_session(const std::string& service) { return add(Session{service}); }

Result connect_to_port(const std::string& name, Handle* out) {
    if (!g_services.count(name)) return ERR_NOT_FOUND;
    *out = open_session(name);
    return RESULT_OK;
}

Result send_sync_request(Handle session, u32 tls) {
    auto* s = std::get_if<Session>(get(session));
    if (!s) return ERR_INVALID_HANDLE;
    auto it = g_services.find(s->service);
    if (it == g_services.end()) {
        throw Unimplemented("IPC to service '" + s->service + "' (no HLE implementation), command header " +
                            hex(g_mem->read<u32>(tls + 0x80)));
    }
    it->second(*g_mem, tls + 0x80);
    return RESULT_OK;
}

}  // namespace hle::kernel
