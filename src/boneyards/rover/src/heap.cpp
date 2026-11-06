// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rover's allocator: in the drop-in DLL, a private Win32 heap with the
// small-block regions in front of it, as 3.1c's Rover allocates; malloc
// elsewhere.

#include "heap.h"

#if defined(_WIN32) && !defined(_WIN64)

#include "small_block_heap.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// Initial commit of the private heap: one page, growable.
constexpr SIZE_T kInitialHeapSize = 0x1000;
// Largest request the heap accepts.
constexpr size_t kMaxRequest = 0xffffffe0;
// The heap rounds requests to paragraphs.
constexpr size_t kParagraph = 0x10;

// Guards the small-block regions. The Win32 heap itself is serialized.
// A critical section is the lock Windows XP provides. It is made on first
// use, before any block is taken, so nothing at load time has to run first
// and the order libraries load in does not matter.
CRITICAL_SECTION g_lock{};
// 0 while the section is uninitialised, 1 while one thread makes it, 2 once it is ready.
volatile LONG g_lock_state = 0;
HANDLE g_heap = nullptr;
RoverSmallBlockHeap g_blocks{};

constexpr LONG kLockUninitialised = 0;
constexpr LONG kLockInitialising = 1;
constexpr LONG kLockReady = 2;

/// Reads the lock's state without changing it.
LONG lock_state() {
    return InterlockedCompareExchange(&g_lock_state, kLockUninitialised, kLockUninitialised);
}

/// Makes the critical section on the first call, then waits until it exists.
///
/// One thread initialises; any other that arrives during that spins until the
/// section is ready. The section is ready before the first block is taken.
void lock_make_ready() {
    if (lock_state() == kLockReady)
        return;
    if (InterlockedCompareExchange(&g_lock_state, kLockInitialising, kLockUninitialised) ==
        kLockUninitialised) {
        InitializeCriticalSection(&g_lock);
        InterlockedExchange(&g_lock_state, kLockReady);
        return;
    }
    while (lock_state() != kLockReady)
        Sleep(0);
}

void lock_enter() {
    lock_make_ready();
    EnterCriticalSection(&g_lock);
}

void lock_leave() {
    LeaveCriticalSection(&g_lock);
}

uint32_t address_of(void* pointer) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
}

void* pointer_to(uint32_t address) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(address));
}

uint32_t reserve_region(void*) {
    return address_of(
        VirtualAlloc(nullptr, kRoverSmallBlockRegionSize, MEM_RESERVE, PAGE_READWRITE)
    );
}

bool commit_group(void*, uint32_t address) {
    return VirtualAlloc(
               pointer_to(address), kRoverSmallBlockGroupSize, MEM_COMMIT, PAGE_READWRITE
           ) != nullptr;
}

void decommit_group(void*, uint32_t address) {
    VirtualFree(pointer_to(address), kRoverSmallBlockGroupSize, MEM_DECOMMIT);
}

void release_region(void*, uint32_t address) {
    VirtualFree(pointer_to(address), 0, MEM_RELEASE);
}

uint32_t heap_alloc(void*, uint32_t size, bool zeroed) {
    return address_of(HeapAlloc(g_heap, zeroed ? HEAP_ZERO_MEMORY : 0, size));
}

uint32_t heap_realloc(void*, uint32_t address, uint32_t size) {
    return address_of(HeapReAlloc(g_heap, 0, pointer_to(address), size));
}

void heap_free(void*, uint32_t address) {
    HeapFree(g_heap, 0, pointer_to(address));
}

/// Creates the private heap and the small-block table on first use.
///
/// Called under the lock.
///
/// @return true when both exist
bool heap_ready() {
    if (g_heap)
        return true;
    HANDLE heap = HeapCreate(0, kInitialHeapSize, 0);
    if (!heap)
        return false;
    g_heap = heap;
    RoverSmallBlockSpace space{};
    space.reserve_region = reserve_region;
    space.commit_group = commit_group;
    space.decommit_group = decommit_group;
    space.release_region = release_region;
    space.heap_alloc = heap_alloc;
    space.heap_realloc = heap_realloc;
    space.heap_free = heap_free;
    if (rover_small_block_init(&g_blocks, &space))
        return true;
    HeapDestroy(heap);
    g_heap = nullptr;
    return false;
}

} // namespace

void* rover_heap_alloc(size_t size) {
    if (size > kMaxRequest)
        return nullptr;
    lock_enter();
    const bool ready = heap_ready();
    uint32_t block = 0;
    if (ready && size <= kRoverSmallBlockLimit)
        block = rover_small_block_alloc(&g_blocks, static_cast<uint32_t>(size));
    lock_leave();
    if (block)
        return pointer_to(block);
    if (!ready)
        return nullptr;
    if (size == 0)
        size = 1;
    return HeapAlloc(g_heap, 0, (size + kParagraph - 1) & ~(kParagraph - 1));
}

void rover_heap_free(void* block) {
    if (!block)
        return;
    lock_enter();
    const bool ready = heap_ready();
    const int32_t region = ready ? rover_small_block_region(&g_blocks, address_of(block)) : -1;
    if (region >= 0)
        rover_small_block_free(&g_blocks, region, address_of(block));
    lock_leave();
    if (ready && region < 0)
        HeapFree(g_heap, 0, block);
}

#else

#include <stdlib.h>

void* rover_heap_alloc(size_t size) {
    return malloc(size);
}

void rover_heap_free(void* block) {
    free(block);
}

#endif
