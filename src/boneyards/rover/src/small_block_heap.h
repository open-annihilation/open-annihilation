// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rover's small-block allocator. Requests of up to
// 0x3f8 bytes are carved from 1 MB regions of reserved address space,
// committed 32 KB at a time, instead of coming from the Win32 heap. A block
// is its size rounded up to 16 bytes, with the size plus an in-use bit in
// the word before the caller's pointer and again in its last word, so the
// memory around a buffer Rover hands its host (a string above all) is laid
// out as 3.1c's Rover laid it out, including when the host gives one back to
// its own heap.
//
// Everything is addressed with 32-bit addresses: real pointers in the 32-bit
// DLL, offsets from a host base when the allocator runs over a test arena.
#ifndef OA_ROVER_SMALL_BLOCK_HEAP_H
#define OA_ROVER_SMALL_BLOCK_HEAP_H

#include <stddef.h>
#include <stdint.h>

// Largest request the regions serve; larger ones go to the private heap.
constexpr uint32_t kRoverSmallBlockLimit = 0x3f8;
// Address space each region reserves.
constexpr uint32_t kRoverSmallBlockRegionSize = 0x100000;
// A region commits and decommits its space in groups of this size.
constexpr uint32_t kRoverSmallBlockGroupSize = 0x8000;
constexpr uint32_t kRoverSmallBlockGroupCount = 32;
// Free blocks are kept in 64 lists: list n holds blocks of (n + 1) * 16
// bytes, the last one blocks of 1024 bytes and more.
constexpr uint32_t kRoverSmallBlockListCount = 64;

// The services a region needs: address space (VirtualAlloc and VirtualFree
// in the DLL) and the private heap its bookkeeping comes from. Addresses of
// 0 mean failure.
struct RoverSmallBlockSpace {
    uintptr_t base{}; // host address of address 0; 0 in the 32-bit DLL
    void* context{};
    // Reserves kRoverSmallBlockRegionSize bytes.
    uint32_t (*reserve_region)(void* context){};
    // Commits kRoverSmallBlockGroupSize zeroed bytes at address.
    bool (*commit_group)(void* context, uint32_t address){};
    void (*decommit_group)(void* context, uint32_t address){};
    void (*release_region)(void* context, uint32_t address){};
    uint32_t (*heap_alloc)(void* context, uint32_t size, bool zeroed){};
    // Leaves the block in place when it fails.
    uint32_t (*heap_realloc)(void* context, uint32_t address, uint32_t size){};
    void (*heap_free)(void* context, uint32_t address){};
};

// One row of the region table. Bit 31 - n of a list word (n counted within
// its word) is set while some group of the region has a free block in list n.
struct RoverSmallBlockHeader {
    uint32_t lists_below_32{};
    uint32_t lists_from_32{};
    uint32_t uncommitted{}; // bit 31 - g while group g is not committed
    uint32_t data{};        // the reserved space
    uint32_t region{};      // its RoverSmallBlockRegion
};

// A list head. Free blocks link through their second word (next) and third
// (previous), so a head stands in as the block one word before it.
struct RoverSmallBlockList {
    uint32_t first{};
    uint32_t last{};
};

struct RoverSmallBlockGroup {
    int32_t used{}; // blocks allocated from the group
    RoverSmallBlockList lists[kRoverSmallBlockListCount]{};
};

// A region's bookkeeping, kept in the private heap.
struct RoverSmallBlockRegion {
    int32_t current_group{}; // group of the last allocation; -1 before any
    // Groups whose list n is not empty.
    uint8_t groups_with_list[kRoverSmallBlockListCount]{};
    uint32_t group_lists_below_32[kRoverSmallBlockGroupCount]{};
    uint32_t group_lists_from_32[kRoverSmallBlockGroupCount]{};
    RoverSmallBlockGroup groups[kRoverSmallBlockGroupCount]{};
};

static_assert(sizeof(RoverSmallBlockHeader) == 0x14, "3.1c's region table row");
static_assert(sizeof(RoverSmallBlockGroup) == 0x204, "3.1c's group");
static_assert(sizeof(RoverSmallBlockRegion) == 0x41c4, "3.1c's region bookkeeping");
static_assert(
    offsetof(RoverSmallBlockRegion, groups) == 0x144, "RoverSmallBlockRegion.groups offset"
);

struct RoverSmallBlockHeap {
    RoverSmallBlockSpace space{};
    uint32_t headers{}; // the region table
    int32_t header_count{};
    int32_t header_capacity{};
    int32_t scan{};     // row the next search starts from
    int32_t deferred{}; // row whose empty group awaits decommit; -1 for none
    uint32_t deferred_group{};
};

/// Starts an empty region table of sixteen rows.
///
/// @param[out] heap heap to set up
/// @param space address-space and runtime-heap services; copied into the heap
/// @return false when the region table cannot be allocated
bool rover_small_block_init(RoverSmallBlockHeap* heap, const RoverSmallBlockSpace* space);

/// Allocates a block of at least size bytes from the regions.
///
/// Finds the lowest free list with room in the region and group last used (searching on from the
/// last row that served), commits a group or reserves a region when none has room, and carves the
/// block from the end of that list's first free block.
///
/// @param[in,out] heap heap to allocate from
/// @param size bytes wanted, at most kRoverSmallBlockLimit
/// @return the block's 32-bit address, or 0 when no region can supply one
uint32_t rover_small_block_alloc(RoverSmallBlockHeap* heap, uint32_t size);

/// Finds the region holding an address.
///
/// @param heap heap to search
/// @param address 32-bit address of a block
/// @return the region's table row, or -1 for memory from elsewhere
int32_t rover_small_block_region(const RoverSmallBlockHeap* heap, uint32_t address);

/// Returns a block to its region, merging it with free neighbours in its page and filing the result
/// in its list.
///
/// A group left with no block in use is decommitted only once another group empties, so a group
/// that keeps filling and emptying stays; a region with no committed group left is released.
///
/// @param[in,out] heap heap owning the region
/// @param row table row of the region, as rover_small_block_region found it
/// @param address 32-bit address of the block
void rover_small_block_free(RoverSmallBlockHeap* heap, int32_t row, uint32_t address);

#endif
