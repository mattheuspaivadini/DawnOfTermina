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

#include "game/crt0.h"

#include <cstring>

#include "game/guest.h"
#include "hle/srv.h"
#include "hle/svc.h"

namespace game::crt0 {

using core::Memory;

// ---- not yet transcribed: boot stops here ---------------------------------------
static void CallStaticInitializers() { untranscribed(0x001009E0); }  // .init_array, 144 ctors
static void CallStaticInitializers2() { untranscribed(0x001042C0); }  // empty range today
static void nninitSystem() { untranscribed(0x00104348); }
static void nnMain() { untranscribed(0x00103EC8); }
static void InitAllocatorPool(u32) { untranscribed(0x00105B40); }
static void InitThreadSystem() { untranscribed(0x0010589C); }
static void InitHeapBlock(u32, u32) { untranscribed(0x0010F5FC); }
static void CreateExpHeap(u32, u32, u32) { untranscribed(0x00106538); }
static Result SetDeviceMemorySize(u32) { untranscribed(0x0010598C); }
static void InitHeapRegionObject(u32, u32, u32) { untranscribed(0x0011BECC); }

// ---- HLE replacements of SDK functions -------------------------------------------

// Original VA: 0x001521D4 — nn::srv::Initialize. The original connects to the "srv:"
// port (retrying every 500 us while it is not ready) and sends RegisterClient. Services
// are HLE'd as direct calls, so only the SDK's bookkeeping is reproduced.
// shortcut: the SDK critical section at 0x007C4398 is not initialised; transcribe it
// if any later code locks it.
static Result nn_srv_Initialize() {
    const u32 refs = r32(va::SRV_REFCOUNT);
    if (s32(refs) > 0) {
        w32(va::SRV_REFCOUNT, refs + 1);
        return 0x082067F9;  // already initialised (info result, not a failure)
    }
    hle::srv::Initialize();
    w32(va::SRV_HANDLE, 0x00000F00);  // non-zero fake session handle
    w32(va::SRV_REFCOUNT, refs + 1);
    return RESULT_OK;
}

// Original VA: 0x00105938 — svcGetResourceLimit(CurrentProcess) +
// svcGetResourceLimitCurrentValues(COMMIT) + svcCloseHandle.
static u32 GetUsedMemory() { return hle::svc::CommittedMemory(); }

// Original VA: 0x0017AB24 — nn::err fatal handler.
[[noreturn]] static void ThrowFatal(Result r) { fatal("nn::err (0x0017AB24)", r); }
// Original VA: 0x00162434 — panic.
[[noreturn]] static void Panic(Result r) { fatal("panic (0x00162434)", r); }

// ---- transcribed -----------------------------------------------------------------

// Original VA: 0x00100024
void ClearBss() {
    std::memset(mem().host(va::BSS_START), 0, va::BSS_END - va::BSS_START);
}

// Original VA: 0x00300C70
static u32 GetCLibLocaleState() { return va::CLIB_LOCALE_STATE; }

// Original VA: 0x00137AC4 — ctype table for a locale name; only "C" exists.
static u32 GetCtypeTable(u32 /*category*/, u32 name) {
    if (name && r8(name) && std::strcmp(cstr(va::LOCALE_NAME_C), cstr(name)) != 0) return 0;
    return va::CTYPE_TABLE_C;
}

// Original VA: 0x001066A4 — case-mapping table for a locale name; only "C" exists.
static u32 GetCaseTable(u32 /*category*/, u32 name) {
    if (name && r8(name) && std::strcmp(cstr(va::LOCALE_NAME_C2), cstr(name)) != 0) return 0;
    return va::CASE_TABLE_C;
}

// Original VA: 0x001042F4
void InitCLocale() {
    const u32 st = GetCLibLocaleState();
    w32(st + 0x4, GetCtypeTable(0, 0) + 1);  // +1 so that table[-1] is the EOF entry
    w32(st + 0xC, GetCaseTable(0, 0));
}

// Original VA: 0x00105928
static u32 GetAppMemorySize() { return r32(Memory::CONFIG_MEM + 0x40); }  // APPMEMALLOC

// Original VA: 0x001082A8
static void InitHeapRegion(u32 base, u32 size) {
    if (!r8(va::HEAP_REGION_READY)) {
        w8(va::HEAP_REGION_READY, 1);
        InitHeapRegionObject(va::HEAP_REGION_OBJ, base, size);
    }
}

// Original VA: 0x00105A6C — grow or shrink the 0x08000000 heap to `size`.
Result SetHeapSize(u32 size) {
    Result r;
    u32 out = 0;
    if (size & 0xFFF) {
        r = 0xE0E01BF2;  // misaligned size
    } else {
        const u32 cur = r32(va::HEAP_SIZE);
        if (size > cur)
            r = hle::svc::ControlMemory(mem(), &out, Memory::HEAP_BASE + cur, 0, size - cur,
                                        hle::svc::MEMOP_COMMIT, 3);
        else
            r = hle::svc::ControlMemory(mem(), &out, Memory::HEAP_BASE + size, 0, cur - size,
                                        hle::svc::MEMOP_FREE, 0);
        if (!failed(r)) w32(va::HEAP_SIZE, size);
    }
    if (failed(r)) ThrowFatal(r);
    InitHeapRegion(Memory::HEAP_BASE, r32(va::HEAP_SIZE));
    w32(va::DEFAULT_ALLOCATOR_PTR, va::DEFAULT_ALLOCATOR);
    return r;
}

// Original VA: 0x00104368 — split application memory into the 0x08000000 heap and
// device (linear) memory, then build the SDK allocators on top.
void nninitStartUp() {
    w8(va::STARTUP_DONE, 0);
    const u32 app = GetAppMemorySize();
    const u32 used = GetUsedMemory();
    const u32 heap = (app - used - DEVICE_MEMORY_SIZE) & ~0xFFFu;
    const u32 main_heap = heap - 0x20000;
    SetHeapSize(heap);
    const Result r = SetDeviceMemorySize(DEVICE_MEMORY_SIZE);
    if (failed(r)) Panic(r);
    InitAllocatorPool(0x10000);
    InitThreadSystem();
    InitHeapBlock(0x0072F8B0, main_heap);
    CreateExpHeap(0x0072F7F0, r32(0x0072F8B8), r32(0x0072F8BC));
}

// Original VA: 0x00100000
void _start() {
    ClearBss();
    InitCLocale();
    nn_srv_Initialize();
    nninitStartUp();
    CallStaticInitializers();
    CallStaticInitializers2();
    nninitSystem();
    nnMain();
    hle::svc::ExitProcess();
}

}  // namespace game::crt0
