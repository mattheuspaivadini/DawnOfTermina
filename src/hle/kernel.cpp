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
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "hle/svc.h"
#include "recomp/runtime.h"

namespace hle::kernel {

struct ThreadExit {};  // unwinds a guest thread's host stack on svcExitThread/ExitProcess

namespace {

using Clock = std::chrono::steady_clock;
struct Thread;

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// ---------------------------------------------------------------- objects

struct Object {
    virtual ~Object() = default;
    virtual std::string describe() const { return "object"; }
    virtual bool waitable() const { return false; }
    virtual bool ready(const Thread*) const { return false; }
    virtual void acquire(Thread*) {}
};

struct Event : Object {
    u32 reset_type;  // 0 ONESHOT, 1 STICKY, 2 PULSE
    bool signaled = false;
    explicit Event(u32 r) : reset_type(r) {}
    bool waitable() const override { return true; }
    bool ready(const Thread*) const override { return signaled; }
    std::string describe() const override { return std::string("event") + (signaled ? "(set)" : ""); }
    void acquire(Thread*) override {
        if (reset_type == 0) signaled = false;
    }
};

struct Mutex : Object {
    Thread* owner = nullptr;
    u32 count = 0;
    bool waitable() const override { return true; }
    bool ready(const Thread* t) const override { return !owner || owner == t; }
    std::string describe() const override { return owner ? "mutex(locked)" : "mutex"; }
    void acquire(Thread* t) override { owner = t, ++count; }
};

struct Semaphore : Object {
    s32 count, max;
    Semaphore(s32 c, s32 m) : count(c), max(m) {}
    bool waitable() const override { return true; }
    bool ready(const Thread*) const override { return count > 0; }
    void acquire(Thread*) override { --count; }
    std::string describe() const override { return "semaphore(" + std::to_string(count) + ")"; }
};

struct AddressArbiter : Object {};
struct ResourceLimit : Object {};
struct Session : Object {
    std::string describe() const override { return "session(" + service + ")"; }
    std::string service;
    ServiceHandler handler;  // per-session handler; empty = look the service up by name
    explicit Session(std::string s, ServiceHandler h = {}) : service(std::move(s)), handler(std::move(h)) {}
};
struct Process : Object {};
struct MemoryBlock : Object {
    u32 size, addr;
    bool guest_owned;
    MemoryBlock(u32 s, u32 a, bool g) : size(s), addr(a), guest_owned(g) {}
};

enum class State { Ready, Running, Waiting, Dead };

struct Thread : Object {
    u32 id;
    s32 priority;
    State state = State::Ready;
    recomp::Context ctx{};
    std::condition_variable cv;
    std::thread host;
    u64 last_run = 0;  // round robin among equal priorities
    const char* host_where = "starting";  // diagnostics: what the host thread is doing

    // wait state
    std::vector<std::shared_ptr<Object>> wait_objects;
    bool wait_all = false;
    bool has_deadline = false;
    Clock::time_point deadline;
    u32 arbiter_addr = 0;  // non-zero while waiting on an address arbiter
    Result wait_result = RESULT_OK;
    s32 wait_index = -1;

    bool waitable() const override { return true; }
    bool ready(const Thread*) const override { return state == State::Dead; }
    std::string describe() const override { return "thread " + std::to_string(id); }
};

// ---------------------------------------------------------------- state

core::Memory* g_mem = nullptr;
std::mutex g_lock;  // guards everything below
std::map<Handle, std::shared_ptr<Object>> g_handles;
std::vector<std::shared_ptr<Thread>> g_threads;
std::map<std::string, ServiceHandler> g_services;
Thread* g_current = nullptr;
bool g_preempt = false;
u64 g_run_counter = 0;
Handle g_next_handle = 0x100;
u32 g_next_thread_id = 1;
std::optional<Status> g_status;
thread_local Thread* t_self = nullptr;

struct Periodic {
    s64 period_ns;
    Clock::time_point next;
    std::function<void()> fn;
};
std::vector<Periodic> g_periodic;
bool g_in_periodic = false;
// Occupies the CPU while the host runs periodic callbacks so no guest thread starts.
Thread* host_marker();

constexpr u32 MAX_THREADS = core::Memory::TLS_PAGES * core::Memory::PAGE / core::Memory::TLS_SIZE;

Handle add(std::shared_ptr<Object> obj) {
    const Handle h = g_next_handle++;
    static const bool trace = std::getenv("DOT_TRACE_HANDLES");
    if (trace)
        std::fprintf(stderr, "kernel: handle %s = %s (thread %u, lr=%s)\n", hex(h).c_str(), obj->describe().c_str(),
                     t_self ? t_self->id : 0, t_self ? hex(t_self->ctx.r[14]).c_str() : "-");
    g_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<Object> get(Handle h) {
    if (h == CURRENT_THREAD && t_self) {
        for (auto& t : g_threads)
            if (t.get() == t_self) return t;
    }
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

template <class T> T* get_as(Handle h) { return dynamic_cast<T*>(get(h).get()); }

// Picks the next thread to run (lock held) and wakes it.
void pick_next() {
    Thread* best = nullptr;
    for (auto& t : g_threads)
        if (t->state == State::Ready &&
            (!best || t->priority < best->priority || (t->priority == best->priority && t->last_run < best->last_run)))
            best = t.get();
    g_current = best;
    g_preempt = false;
    if (best) best->cv.notify_one();
}

void make_ready(Thread* t) {
    t->state = State::Ready;
    if (!g_current) pick_next();
    else if (t->priority < g_current->priority) {
        g_preempt = true;
        recomp::g_interrupt.store(1, std::memory_order_relaxed);  // reach a preemption point soon
    }
}

// Completes a satisfiable wait (lock held). Returns true if the thread was woken.
bool try_complete_wait(Thread* t) {
    if (t->state != State::Waiting || t->arbiter_addr || t->wait_objects.empty()) return false;
    if (t->wait_all) {
        for (auto& o : t->wait_objects)
            if (!o->ready(t)) return false;
        for (auto& o : t->wait_objects) o->acquire(t);
        t->wait_index = -1;
    } else {
        s32 i = 0;
        for (; i < s32(t->wait_objects.size()); ++i)
            if (t->wait_objects[i]->ready(t)) break;
        if (i == s32(t->wait_objects.size())) return false;
        t->wait_objects[i]->acquire(t);
        t->wait_index = i;
    }
    t->wait_objects.clear();
    t->wait_result = RESULT_OK;
    make_ready(t);
    return true;
}

// Re-evaluates every waiting thread after some object changed (lock held).
void wake_waiters() {
    std::vector<Thread*> waiting;
    for (auto& t : g_threads)
        if (t->state == State::Waiting && !t->arbiter_addr && !t->wait_objects.empty()) waiting.push_back(t.get());
    std::sort(waiting.begin(), waiting.end(), [](Thread* a, Thread* b) { return a->priority < b->priority; });
    for (Thread* t : waiting) try_complete_wait(t);
}

// The current thread gives up the CPU (its state is already set) and waits until it
// is scheduled again. Runs with the lock held.
void switch_away(std::unique_lock<std::mutex>& lk, Thread* self) {
    self->last_run = ++g_run_counter;
    if (g_current == self) pick_next();
    if (self->state == State::Dead) return;
    self->host_where = "waiting for the baton";
    while (g_current != self) {
        if (g_status) {  // guest stopped: park this thread forever
            self->cv.wait(lk);
            continue;
        }
        if (self->state == State::Waiting && self->has_deadline) {
            if (self->cv.wait_until(lk, self->deadline) == std::cv_status::timeout && self->state == State::Waiting) {
                self->wait_objects.clear();
                self->arbiter_addr = 0;
                self->wait_result = ERR_TIMEOUT;
                make_ready(self);
            }
        } else {
            self->cv.wait(lk);
        }
        if (!g_current && self->state == State::Ready) pick_next();
    }
    self->state = State::Running;
    self->host_where = "guest code";
}

// Blocks the current thread with its wait fields set (lock held).
Result block(std::unique_lock<std::mutex>& lk, s64 timeout_ns) {
    Thread* self = t_self;
    self->state = State::Waiting;
    self->has_deadline = timeout_ns >= 0;
    if (self->has_deadline) self->deadline = Clock::now() + std::chrono::nanoseconds(timeout_ns);
    switch_away(lk, self);
    return self->wait_result;
}

[[noreturn]] void park_forever() {
    std::unique_lock lk(g_lock);
    t_self->host_where = "parked (thread ended)";
    for (;;) t_self->cv.wait(lk);
}

void finish_thread() {
    Thread* self = t_self;
    self->state = State::Dead;
    wake_waiters();  // threads waiting on this thread's handle
    if (g_current == self) pick_next();
}

void thread_main(Thread* self, u32 entry) {
    t_self = self;
    {
        std::unique_lock lk(g_lock);
        while (g_current != self) self->cv.wait(lk);
        self->state = State::Running;
        self->host_where = "guest code";
    }
    try {
        recomp::call(&self->ctx, entry);
        exit_thread();  // returning from the entry point ends the thread
    } catch (const recomp::Trap& t) {
        char where[96];
        std::snprintf(where, sizeof where, " [thread %u, pc=0x%08X lr=0x%08X sp=0x%08X]", self->id, t.pc,
                      self->ctx.r[14], self->ctx.r[13]);
        fail(std::string(t.what()) + where);
    } catch (const Unimplemented& u) {
        char where[64];
        std::snprintf(where, sizeof where, " [thread %u, lr=0x%08X]", self->id, self->ctx.r[14]);
        fail(std::string("HLE: ") + u.what() + where);
    } catch (const ThreadExit&) {
    }
    park_forever();
}

std::shared_ptr<Thread> new_thread(u32 entry, u32 arg, u32 stack_top, s32 priority) {
    auto t = std::make_shared<Thread>();
    t->id = g_next_thread_id++;
    t->priority = priority;
    t->ctx.r[0] = arg;
    t->ctx.r[13] = stack_top;
    t->ctx.tls = core::Memory::tls(t->id - 1);
    t->ctx.fpscr = 0x03C00000;  // RunFast: flush-to-zero + default NaN, round to nearest
    g_mem->write<u32>(t->ctx.tls, 0);
    g_threads.push_back(t);
    t->host = std::thread(thread_main, t.get(), entry);
    t->host.detach();
    return t;
}

Thread* host_marker() {
    static Thread marker;
    marker.priority = 0x40;  // below every guest thread: any wake-up requests preemption
    return &marker;
}

// Runs due periodic callbacks. Caller guarantees no guest thread is executing; called
// without the kernel lock (callbacks use kernel functions).
void run_due_periodic() {
    std::vector<std::function<void()>> due;
    {
        std::lock_guard lk(g_lock);
        if (g_in_periodic) return;
        const auto now = Clock::now();
        for (auto& p : g_periodic)
            if (now >= p.next) {
                due.push_back(p.fn);
                p.next += std::chrono::nanoseconds(p.period_ns);
                if (p.next < now) p.next = now + std::chrono::nanoseconds(p.period_ns);  // fell behind
            }
        if (due.empty()) return;
        g_in_periodic = true;
    }
    for (auto& fn : due) fn();
    std::lock_guard lk(g_lock);
    g_in_periodic = false;
}

}  // namespace

void init(core::Memory& mem) {
    recomp::set_interrupt_handler(preemption_point);
    std::lock_guard lk(g_lock);
    g_mem = &mem;
    g_handles.clear();
    g_next_handle = 0x100;
}

recomp::Context& current_context() { return t_self->ctx; }

// ---------------------------------------------------------------- threads

void start_main_thread(u32 entry, u32 stack_top, s32 priority) {
    std::lock_guard lk(g_lock);
    new_thread(entry, 0, stack_top, priority);
    pick_next();
}

Result create_thread(u32 entry, u32 arg, u32 stack_top, s32 priority, s32, Handle* out) {
    std::unique_lock lk(g_lock);
    if (priority < 0 || priority > 0x3F) return ERR_OUT_OF_RANGE;
    if (g_threads.size() >= MAX_THREADS) throw Unimplemented("more than " + std::to_string(MAX_THREADS) + " guest threads");
    auto t = new_thread(entry, arg, stack_top, priority);
    *out = add(t);
    if (priority < t_self->priority) {  // a higher-priority thread runs immediately
        t_self->state = State::Ready;
        switch_away(lk, t_self);
    }
    return RESULT_OK;
}

void exit_thread() {
    {
        std::lock_guard lk(g_lock);
        finish_thread();
    }
    throw ThreadExit{};
}

void exit_process() {
    {
        std::lock_guard lk(g_lock);
        if (!g_status) g_status = Status{true, {}};
        g_current = nullptr;
    }
    throw ThreadExit{};
}

void sleep_thread(s64 ns) {
    std::unique_lock lk(g_lock);
    if (ns <= 0) {  // yield
        t_self->state = State::Ready;
        switch_away(lk, t_self);
        return;
    }
    t_self->wait_objects.clear();
    block(lk, ns);
}

void preemption_point() {
    run_due_periodic();
    std::unique_lock lk(g_lock);
    if (g_preempt && t_self && g_current == t_self) {
        t_self->state = State::Ready;
        switch_away(lk, t_self);
    }
}

Result get_thread_id(Handle h, u32* out) {
    std::lock_guard lk(g_lock);
    auto* t = get_as<Thread>(h);
    if (!t) return ERR_INVALID_HANDLE;
    *out = t->id;
    return RESULT_OK;
}

Result get_thread_priority(Handle h, s32* out) {
    std::lock_guard lk(g_lock);
    auto* t = get_as<Thread>(h);
    if (!t) return ERR_INVALID_HANDLE;
    *out = t->priority;
    return RESULT_OK;
}

Result set_thread_priority(Handle h, s32 priority) {
    std::lock_guard lk(g_lock);
    auto* t = get_as<Thread>(h);
    if (!t) return ERR_INVALID_HANDLE;
    if (priority < 0 || priority > 0x3F) return ERR_OUT_OF_RANGE;
    t->priority = priority;
    for (auto& o : g_threads)
        if (o->state == State::Ready && o->priority < t_self->priority) g_preempt = true;
    return RESULT_OK;
}

std::string dump_threads() {
    std::lock_guard lk(g_lock);
    std::string out = "  baton: ";
    out += !g_current ? "nobody" : g_current == host_marker() ? "host" : "thread " + std::to_string(g_current->id);
    out += g_preempt ? " (preempt pending)\n" : "\n";
    for (auto& t : g_threads) {
        static const char* names[] = {"ready", "running", "waiting", "dead"};
        char line[160];
        std::snprintf(line, sizeof line, "  thread %u prio 0x%02X %-7s [host: %s] lr=0x%08X sp=0x%08X", t->id,
                      t->priority, names[int(t->state)], t->host_where, t->ctx.r[14], t->ctx.r[13]);
        out += line;
        if (t->state == State::Waiting) {
            if (t->arbiter_addr) out += " on arbiter address " + hex(t->arbiter_addr);
            for (auto& o : t->wait_objects) {
                std::string handle = "?";
                for (auto& [h, obj] : g_handles)
                    if (obj == o) handle = hex(h);
                out += " " + o->describe() + "[" + handle + "]";
            }
            if (t->wait_objects.empty() && !t->arbiter_addr) out += " sleeping";
            if (t->wait_all) out += " (all)";
        }
        out += "\n";
    }
    return out;
}

std::optional<Status> stopped() {
    std::lock_guard lk(g_lock);
    return g_status;
}

void fail(const std::string& message) {
    std::lock_guard lk(g_lock);
    if (!g_status) g_status = Status{false, message};
    g_current = nullptr;
}

// ---------------------------------------------------------------- objects

Handle create_event(u32 reset_type) {
    std::lock_guard lk(g_lock);
    return add(std::make_shared<Event>(reset_type));
}

Handle create_mutex(bool locked) {
    std::lock_guard lk(g_lock);
    auto m = std::make_shared<Mutex>();
    if (locked) m->owner = t_self, m->count = 1;
    return add(m);
}

Handle create_semaphore(s32 initial, s32 max) {
    std::lock_guard lk(g_lock);
    return add(std::make_shared<Semaphore>(initial, max));
}

Handle create_address_arbiter() {
    std::lock_guard lk(g_lock);
    return add(std::make_shared<AddressArbiter>());
}

Result signal_event(Handle h) {
    std::lock_guard lk(g_lock);
    auto* e = get_as<Event>(h);
    if (!e) return ERR_INVALID_HANDLE;
    e->signaled = true;
    wake_waiters();
    if (e->reset_type == 2) e->signaled = false;  // PULSE: wakes current waiters only
    return RESULT_OK;
}

Result clear_event(Handle h) {
    std::lock_guard lk(g_lock);
    auto* e = get_as<Event>(h);
    if (!e) return ERR_INVALID_HANDLE;
    e->signaled = false;
    return RESULT_OK;
}

Result release_mutex(Handle h) {
    std::lock_guard lk(g_lock);
    auto* m = get_as<Mutex>(h);
    if (!m) return ERR_INVALID_HANDLE;
    if (m->owner != t_self) return ERR_NOT_AUTHORIZED;
    if (--m->count == 0) {
        m->owner = nullptr;
        wake_waiters();
    }
    return RESULT_OK;
}

Result release_semaphore(Handle h, s32 count, s32* previous) {
    std::lock_guard lk(g_lock);
    auto* s = get_as<Semaphore>(h);
    if (!s) return ERR_INVALID_HANDLE;
    if (count < 0 || s->count + count > s->max) return ERR_OUT_OF_RANGE;
    *previous = s->count;
    s->count += count;
    wake_waiters();
    return RESULT_OK;
}

Result close_handle(Handle h) {
    std::lock_guard lk(g_lock);
    return g_handles.erase(h) ? RESULT_OK : ERR_INVALID_HANDLE;
}

Result duplicate_handle(Handle h, Handle* out) {
    std::lock_guard lk(g_lock);
    auto obj = h == CURRENT_PROCESS ? std::make_shared<Process>() : get(h);
    if (!obj) return ERR_INVALID_HANDLE;
    *out = add(obj);
    return RESULT_OK;
}

Result wait_synchronization_n(const u32* handles, u32 count, bool wait_all, s64 timeout_ns, s32* index) {
    std::unique_lock lk(g_lock);
    Thread* self = t_self;
    self->wait_objects.clear();
    for (u32 i = 0; i < count; ++i) {
        auto o = get(handles[i]);
        if (!o) return ERR_INVALID_HANDLE;
        if (!o->waitable()) throw Unimplemented("wait on a non-waitable object, handle " + hex(handles[i]));
        self->wait_objects.push_back(o);
    }
    self->wait_all = wait_all;
    self->arbiter_addr = 0;
    self->state = State::Waiting;
    if (count && try_complete_wait(self)) {  // ready right away: keep running
        self->state = State::Running;
        if (g_current != self) g_current = self;  // make_ready may have rescheduled; we still hold the CPU
        *index = self->wait_index;
        return RESULT_OK;
    }
    if (timeout_ns == 0) {
        self->state = State::Running;
        self->wait_objects.clear();
        return ERR_TIMEOUT;
    }
    const Result r = block(lk, timeout_ns);
    *index = self->wait_index;
    return r;
}

Result wait_synchronization(Handle h, s64 timeout_ns) {
    s32 index;
    const u32 handles[1] = {h};
    return wait_synchronization_n(handles, 1, false, timeout_ns, &index);
}

Result arbitrate_address(Handle arbiter, u32 addr, u32 type, s32 value, s64 timeout_ns) {
    std::unique_lock lk(g_lock);
    if (!get_as<AddressArbiter>(arbiter)) return ERR_INVALID_HANDLE;
    if (type == 0) {  // SIGNAL: wake up to `value` waiters (all if negative), by priority
        std::vector<Thread*> waiters;
        for (auto& t : g_threads)
            if (t->state == State::Waiting && t->arbiter_addr == addr) waiters.push_back(t.get());
        std::sort(waiters.begin(), waiters.end(), [](Thread* a, Thread* b) { return a->priority < b->priority; });
        const size_t n = value < 0 ? waiters.size() : std::min<size_t>(value, waiters.size());
        for (size_t i = 0; i < n; ++i) {
            waiters[i]->arbiter_addr = 0;
            waiters[i]->wait_result = RESULT_OK;
            make_ready(waiters[i]);
        }
        return RESULT_OK;
    }
    if (type > 4) throw Unimplemented("ArbitrateAddress type " + std::to_string(type));
    // 1 WAIT_IF_LESS_THAN, 2 DECREMENT_AND_WAIT_IF_LESS_THAN, 3/4 the same with timeout
    const s32 cur = s32(g_mem->read<u32>(addr));
    if (cur >= value) return RESULT_OK;
    if (type == 2 || type == 4) g_mem->write<u32>(addr, u32(cur - 1));
    t_self->wait_objects.clear();
    t_self->arbiter_addr = addr;
    return block(lk, type >= 3 ? timeout_ns : -1);
}

// ---------------------------------------------------------------- shared memory

Handle create_shared_memory(u32 size) {
    std::lock_guard lk(g_lock);
    return add(std::make_shared<MemoryBlock>(size, 0, false));
}

SharedMemoryInfo shared_memory(Handle h) {
    std::lock_guard lk(g_lock);
    auto* b = get_as<MemoryBlock>(h);
    return b ? SharedMemoryInfo{b->size, b->addr} : SharedMemoryInfo{0, 0};
}

Result create_memory_block(u32 addr, u32 size, Handle* out) {
    std::lock_guard lk(g_lock);
    if (addr == 0) throw Unimplemented("svcCreateMemoryBlock with address 0 (kernel-allocated block)");
    *out = add(std::make_shared<MemoryBlock>(size, addr, true));
    return RESULT_OK;
}

Result map_memory_block(Handle h, u32 addr) {
    std::lock_guard lk(g_lock);
    if (std::getenv("DOT_TRACE_HANDLES"))
        std::fprintf(stderr, "kernel: map memory block %s at %s\n", hex(h).c_str(), hex(addr).c_str());
    auto* b = get_as<MemoryBlock>(h);
    if (!b) return ERR_INVALID_HANDLE;
    if (b->guest_owned) return RESULT_OK;  // the guest already has it at b->addr
    const u32 size = (b->size + core::Memory::PAGE - 1) & ~(core::Memory::PAGE - 1);
    if (!g_mem->map(addr, size)) throw Unimplemented("svcMapMemoryBlock at " + hex(addr) + " failed");
    b->addr = addr;
    return RESULT_OK;
}

Result unmap_memory_block(Handle h, u32 addr) {
    std::lock_guard lk(g_lock);
    auto* b = get_as<MemoryBlock>(h);
    if (!b) return ERR_INVALID_HANDLE;
    if (!b->guest_owned && b->addr == addr) b->addr = 0;  // pages stay mapped: the guest may still touch them
    return RESULT_OK;
}

// ---------------------------------------------------------------- periodic work

void add_periodic(s64 period_ns, std::function<void()> fn) {
    std::lock_guard lk(g_lock);
    g_periodic.push_back({period_ns, Clock::now() + std::chrono::nanoseconds(period_ns), std::move(fn)});
}

void host_tick() {
    {
        std::lock_guard lk(g_lock);
        if (g_status) return;
        if (g_current) {  // a guest thread is busy: have it stop at its next loop back-edge
            const auto now = Clock::now();
            for (auto& p : g_periodic)
                if (now >= p.next) recomp::g_interrupt.store(1, std::memory_order_relaxed);
            return;
        }
        g_current = host_marker();
    }
    run_due_periodic();
    std::lock_guard lk(g_lock);
    if (g_current == host_marker()) pick_next();
}

// ---------------------------------------------------------------- resource limits

Result get_resource_limit(Handle process, Handle* out) {
    std::lock_guard lk(g_lock);
    if (process != CURRENT_PROCESS) return ERR_INVALID_HANDLE;
    *out = add(std::make_shared<ResourceLimit>());
    return RESULT_OK;
}

Result get_resource_limit_values(u32 values_va, Handle limit, u32 names_va, u32 count, bool current) {
    std::lock_guard lk(g_lock);
    if (!get_as<ResourceLimit>(limit)) return ERR_INVALID_HANDLE;
    for (u32 i = 0; i < count; ++i) {
        const u32 name = g_mem->read<u32>(names_va + i * 4);
        s64 v;
        switch (name) {
        case 0: v = current ? 0 : 0x18; break;  // priority
        case 1: v = current ? hle::svc::CommittedMemory() : g_mem->read<u32>(core::Memory::CONFIG_MEM + 0x40); break;
        case 2: v = current ? s64(g_threads.size()) : MAX_THREADS; break;
        default: v = current ? 0 : 64; break;  // events, mutexes, semaphores, timers, shared memory, ...
        }
        g_mem->write<u64>(values_va + i * 8, u64(v));
    }
    return RESULT_OK;
}

// ---------------------------------------------------------------- IPC

void register_service(const std::string& name, ServiceHandler handler) {
    std::lock_guard lk(g_lock);
    g_services[name] = std::move(handler);
}

Handle open_session(const std::string& service) {
    std::lock_guard lk(g_lock);
    return add(std::make_shared<Session>(service));
}

Handle open_session(const std::string& name, ServiceHandler handler) {
    std::lock_guard lk(g_lock);
    return add(std::make_shared<Session>(name, std::move(handler)));
}

Result connect_to_port(const std::string& name, Handle* out) {
    {
        std::lock_guard lk(g_lock);
        if (!g_services.count(name)) return ERR_NOT_FOUND;
    }
    *out = open_session(name);
    return RESULT_OK;
}

Result send_sync_request(Handle session) {
    ServiceHandler handler;
    std::string name;
    {
        std::lock_guard lk(g_lock);
        auto* s = get_as<Session>(session);
        if (!s) return ERR_INVALID_HANDLE;
        name = s->service;
        if (s->handler) {
            handler = s->handler;
        } else {
            auto it = g_services.find(name);
            if (it != g_services.end()) handler = it->second;
        }
    }
    const u32 cmd = t_self->ctx.tls + 0x80;
    if (!handler)
        throw Unimplemented("IPC to service '" + name + "' (no HLE implementation), command header " +
                            hex(g_mem->read<u32>(cmd)));
    handler(*g_mem, cmd);  // runs without the kernel lock: handlers call back into the kernel

    // Copied handles: the client gets its own handle to the object (closing it must not
    // invalidate the service's), as the real kernel's reply translation does.
    const u32 header = g_mem->read<u32>(cmd), normal = header >> 6 & 0x3F, translate = header & 0x3F;
    for (u32 i = 1 + normal, end = i + translate; i < end;) {
        const u32 desc = g_mem->read<u32>(cmd + i * 4);
        const u32 words = (desc & 0xF) == 0 ? (desc >> 26) + 1 : 1;
        if ((desc & 0x3F) == 0)  // copy-handle descriptor (not move, not calling PID)
            for (u32 k = 1; k <= words && i + k < end; ++k) {
                Handle copy = 0;
                if (const Handle h = g_mem->read<u32>(cmd + (i + k) * 4); h && duplicate_handle(h, &copy) == RESULT_OK)
                    g_mem->write<u32>(cmd + (i + k) * 4, copy);
            }
        i += 1 + words;
    }
    return RESULT_OK;
}

}  // namespace hle::kernel
