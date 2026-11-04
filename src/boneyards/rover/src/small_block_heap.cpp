// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rover's small-block allocator, over 32-bit addresses. The block
// placement (carving from the end of the first fitting free block), the
// merge rules and the deferred decommit of empty groups match 3.1c's Rover,
// so the same requests produce the same layout.

#include "small_block_heap.h"

#include <string.h>

namespace {

using Header = RoverSmallBlockHeader;
using Region = RoverSmallBlockRegion;
using Group = RoverSmallBlockGroup;

constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kPagesPerGroup = kRoverSmallBlockGroupSize / kPageSize;
// A committed page is one free block from here to its closing guard word.
constexpr uint32_t kPageBlock = 0xc;
constexpr uint32_t kPageBlockSize = 0xff0;
// In the third and the last word of every page (kGuardBefore, kGuardAfter);
// odd, so it reads as a block in use and no block merges across a page
// boundary.
constexpr uint32_t kPageGuard = 0xffffffff;
constexpr uint32_t kGuardBefore = 0x8;
constexpr uint32_t kGuardAfter = 0xffc;
constexpr uint32_t kInUse = 1;
// A block's size sits in its first word, before the caller's pointer, and
// again in its last.
constexpr uint32_t kSizeWord = 4;
constexpr uint32_t kBlockAlignment = 0x10;
// Offsets of a free block's links; a list head sits 4 bytes after the block
// it stands in for.
constexpr uint32_t kNextLink = 4;
constexpr uint32_t kPreviousLink = 8;
constexpr uint32_t kLastList = kRoverSmallBlockListCount - 1;
constexpr uint32_t kListsPerWord = 32;
constexpr uint32_t kTopBit = 0x80000000u;
// Initial rows of the region table, and its growth.
constexpr int32_t kHeaderRows = 16;

uint32_t& word(const RoverSmallBlockHeap* heap, uint32_t address) {
    return *reinterpret_cast<uint32_t*>(heap->space.base + address);
}

Header& header_at(const RoverSmallBlockHeap* heap, int32_t row) {
    return *reinterpret_cast<Header*>(
        heap->space.base + heap->headers + static_cast<uint32_t>(row) * sizeof(Header)
    );
}

Region& region_of(const RoverSmallBlockHeap* heap, const Header& header) {
    return *reinterpret_cast<Region*>(heap->space.base + header.region);
}

uint32_t group_address(const Header& header, uint32_t group) {
    return header.region + static_cast<uint32_t>(offsetof(Region, groups)) + group * sizeof(Group);
}

// The block a list's head stands in for.
uint32_t list_anchor(uint32_t group, uint32_t list) {
    return group + list * sizeof(RoverSmallBlockList);
}

uint32_t list_for_size(uint32_t size) {
    const uint32_t list = (size >> 4) - 1;
    return list > kLastList ? kLastList : list;
}

uint32_t list_bit(uint32_t list) {
    return kTopBit >> (list % kListsPerWord);
}

bool only_block_in_list(const RoverSmallBlockHeap* heap, uint32_t block) {
    return word(heap, block + kNextLink) == word(heap, block + kPreviousLink);
}

void unlink_block(const RoverSmallBlockHeap* heap, uint32_t block) {
    const uint32_t next = word(heap, block + kNextLink);
    const uint32_t previous = word(heap, block + kPreviousLink);
    word(heap, previous + kNextLink) = next;
    word(heap, next + kPreviousLink) = previous;
}

void push_block(const RoverSmallBlockHeap* heap, uint32_t group, uint32_t list, uint32_t block) {
    const uint32_t anchor = list_anchor(group, list);
    word(heap, block + kNextLink) = word(heap, anchor + kNextLink);
    word(heap, block + kPreviousLink) = anchor;
    word(heap, anchor + kNextLink) = block;
    word(heap, word(heap, block + kNextLink) + kPreviousLink) = block;
}

// Records that a group's list gained its first block.
void note_list_filled(Header& header, Region& region, uint32_t group, uint32_t list) {
    const bool first_in_region = region.groups_with_list[list]++ == 0;
    if (list < kListsPerWord) {
        if (first_in_region)
            header.lists_below_32 |= list_bit(list);
        region.group_lists_below_32[group] |= list_bit(list);
    } else {
        if (first_in_region)
            header.lists_from_32 |= list_bit(list);
        region.group_lists_from_32[group] |= list_bit(list);
    }
}

// Records that a group's list lost its last block.
void note_list_emptied(Header& header, Region& region, uint32_t group, uint32_t list) {
    const uint32_t keep = ~list_bit(list);
    const bool last_in_region = --region.groups_with_list[list] == 0;
    if (list < kListsPerWord) {
        region.group_lists_below_32[group] &= keep;
        if (last_in_region)
            header.lists_below_32 &= keep;
    } else {
        region.group_lists_from_32[group] &= keep;
        if (last_in_region)
            header.lists_from_32 &= keep;
    }
}

bool offers(uint32_t below_32, uint32_t from_32, uint32_t below_mask, uint32_t from_mask) {
    return (below_32 & below_mask) != 0 || (from_32 & from_mask) != 0;
}

// First row in [from, to) with a free list the masks accept, or to.
int32_t find_offering_row(
    const RoverSmallBlockHeap* heap,
    int32_t from,
    int32_t to,
    uint32_t below_mask,
    uint32_t from_mask
) {
    for (; from < to; ++from) {
        const Header& header = header_at(heap, from);
        if (offers(header.lists_below_32, header.lists_from_32, below_mask, from_mask))
            break;
    }
    return from;
}

// First row in [from, to) with an uncommitted group, or to.
int32_t find_uncommitted_row(const RoverSmallBlockHeap* heap, int32_t from, int32_t to) {
    while (from < to && header_at(heap, from).uncommitted == 0)
        ++from;
    return from;
}

/// Adds a row for a freshly reserved region with nothing committed, growing the table by sixteen
/// rows when full.
///
/// @param[in,out] heap heap whose region table grows
/// @return the new row, or -1 when the table cannot grow or the space cannot be reserved
int32_t add_region(RoverSmallBlockHeap* heap) {
    RoverSmallBlockSpace& space = heap->space;
    if (heap->header_count == heap->header_capacity) {
        const uint32_t grown = space.heap_realloc(
            space.context,
            heap->headers,
            static_cast<uint32_t>(heap->header_capacity + kHeaderRows) * sizeof(Header)
        );
        if (!grown)
            return -1;
        heap->header_capacity += kHeaderRows;
        heap->headers = grown;
    }
    Header& header = header_at(heap, heap->header_count);
    header.region = space.heap_alloc(space.context, sizeof(Region), true);
    if (!header.region)
        return -1;
    header.data = space.reserve_region(space.context);
    if (!header.data) {
        space.heap_free(space.context, header.region);
        return -1;
    }
    header.uncommitted = 0xffffffffu;
    header.lists_below_32 = 0;
    header.lists_from_32 = 0;
    region_of(heap, header).current_group = -1;
    return heap->header_count++;
}

/// Commits a region's lowest uncommitted group as eight free page-sized blocks in the last list.
///
/// Each page carries its guard words, and the group's list heads start empty.
///
/// @param[in,out] heap heap owning the region
/// @param row table row of the region
/// @return the committed group, or -1 when the commit fails
int32_t commit_group(RoverSmallBlockHeap* heap, int32_t row) {
    Header& header = header_at(heap, row);
    Region& region = region_of(heap, header);
    uint32_t group = 0;
    for (uint32_t bits = header.uncommitted; !(bits & kTopBit); bits <<= 1)
        ++group;
    const uint32_t lists = group_address(header, group);
    for (uint32_t list = 0; list < kLastList; ++list) {
        const uint32_t anchor = list_anchor(lists, list);
        word(heap, anchor + kNextLink) = anchor;
        word(heap, anchor + kPreviousLink) = anchor;
    }
    const uint32_t data = header.data + group * kRoverSmallBlockGroupSize;
    if (!heap->space.commit_group(heap->space.context, data))
        return -1;
    for (uint32_t page = data; page < data + kRoverSmallBlockGroupSize; page += kPageSize) {
        const uint32_t block = page + kPageBlock;
        word(heap, page + kGuardBefore) = kPageGuard;
        word(heap, page + kGuardAfter) = kPageGuard;
        word(heap, block) = kPageBlockSize;
        word(heap, block + kPageBlockSize - kSizeWord) = kPageBlockSize;
        word(heap, block + kNextLink) = block + kPageSize;
        word(heap, block + kPreviousLink) = block - kPageSize;
    }
    const uint32_t anchor = list_anchor(lists, kLastList);
    const uint32_t first = data + kPageBlock;
    const uint32_t last = first + (kPagesPerGroup - 1) * kPageSize;
    word(heap, anchor + kNextLink) = first;
    word(heap, first + kPreviousLink) = anchor;
    word(heap, anchor + kPreviousLink) = last;
    word(heap, last + kNextLink) = anchor;
    region.group_lists_below_32[group] = 0;
    region.group_lists_from_32[group] = list_bit(kLastList);
    if (region.groups_with_list[kLastList]++ == 0)
        header.lists_from_32 |= list_bit(kLastList);
    header.uncommitted &= ~(kTopBit >> group);
    return static_cast<int32_t>(group);
}

// The tail of rover_small_block_free: decommits the group an earlier free
// left empty and releases its region once no group of it stays committed.
// Returns the caller's row, renumbered if the table closed up below it.
int32_t decommit_deferred_group(RoverSmallBlockHeap* heap, int32_t row) {
    RoverSmallBlockSpace& space = heap->space;
    Header& header = header_at(heap, heap->deferred);
    Region& region = region_of(heap, header);
    const uint32_t group = heap->deferred_group;
    space.decommit_group(space.context, header.data + group * kRoverSmallBlockGroupSize);
    header.uncommitted |= kTopBit >> group;
    region.group_lists_from_32[group] = 0;
    if (--region.groups_with_list[kLastList] == 0)
        header.lists_from_32 &= ~list_bit(kLastList);
    if (header.uncommitted != 0xffffffffu)
        return row;
    space.release_region(space.context, header.data);
    space.heap_free(space.context, header.region);
    memmove(
        &header,
        &header_at(heap, heap->deferred + 1),
        static_cast<size_t>(heap->header_count - heap->deferred - 1) * sizeof(Header)
    );
    --heap->header_count;
    heap->scan = 0;
    return heap->deferred < row ? row - 1 : row;
}

} // namespace

bool rover_small_block_init(RoverSmallBlockHeap* heap, const RoverSmallBlockSpace* space) {
    *heap = RoverSmallBlockHeap{};
    heap->space = *space;
    heap->headers = space->heap_alloc(space->context, kHeaderRows * sizeof(Header), false);
    if (!heap->headers)
        return false;
    heap->header_capacity = kHeaderRows;
    heap->deferred = -1;
    return true;
}

uint32_t rover_small_block_alloc(RoverSmallBlockHeap* heap, uint32_t size) {
    const uint32_t block_size =
        (size + 2 * kSizeWord + kBlockAlignment - 1) & ~(kBlockAlignment - 1);
    const uint32_t wanted = (block_size >> 4) - 1;
    const uint32_t below_mask = wanted < kListsPerWord ? 0xffffffffu >> wanted : 0;
    const uint32_t from_mask =
        wanted < kListsPerWord ? 0xffffffffu : 0xffffffffu >> (wanted - kListsPerWord);

    const int32_t count = heap->header_count;
    int32_t row = find_offering_row(heap, heap->scan, count, below_mask, from_mask);
    if (row == count) {
        row = find_offering_row(heap, 0, heap->scan, below_mask, from_mask);
        if (row == heap->scan) {
            row = find_uncommitted_row(heap, heap->scan, count);
            if (row == count) {
                row = find_uncommitted_row(heap, 0, heap->scan);
                if (row == heap->scan && (row = add_region(heap)) < 0)
                    return 0;
            }
            const int32_t committed = commit_group(heap, row);
            region_of(heap, header_at(heap, row)).current_group = committed;
            if (committed < 0)
                return 0;
        }
    }

    Header& header = header_at(heap, row);
    Region& region = region_of(heap, header);
    int32_t group = region.current_group;
    if (group < 0 || !offers(
                         region.group_lists_below_32[group],
                         region.group_lists_from_32[group],
                         below_mask,
                         from_mask
                     )) {
        group = 0;
        while (!offers(
            region.group_lists_below_32[group],
            region.group_lists_from_32[group],
            below_mask,
            from_mask
        ))
            ++group;
    }
    const uint32_t lists = group_address(header, static_cast<uint32_t>(group));
    uint32_t bits = region.group_lists_below_32[group] & below_mask;
    uint32_t list = 0;
    if (!bits) {
        bits = region.group_lists_from_32[group] & from_mask;
        list = kListsPerWord;
    }
    for (; !(bits & kTopBit); bits <<= 1)
        ++list;

    const uint32_t free_block = word(heap, list_anchor(lists, list) + kNextLink);
    const uint32_t remaining = word(heap, free_block) - block_size;
    heap->scan = row;
    if (remaining == 0 || list_for_size(remaining) != list) {
        if (only_block_in_list(heap, free_block))
            note_list_emptied(header, region, static_cast<uint32_t>(group), list);
        unlink_block(heap, free_block);
        if (remaining != 0) {
            const uint32_t target = list_for_size(remaining);
            push_block(heap, lists, target, free_block);
            if (only_block_in_list(heap, free_block))
                note_list_filled(header, region, static_cast<uint32_t>(group), target);
        }
    }
    if (remaining != 0) {
        word(heap, free_block) = remaining;
        word(heap, free_block + remaining - kSizeWord) = remaining;
    }
    const uint32_t block = free_block + remaining;
    word(heap, block) = block_size + kInUse;
    word(heap, block + block_size - kSizeWord) = block_size + kInUse;

    Group& used = region.groups[group];
    if (used.used++ == 0 && heap->deferred == row &&
        heap->deferred_group == static_cast<uint32_t>(group))
        heap->deferred = -1;
    region.current_group = group;
    return block + kSizeWord;
}

int32_t rover_small_block_region(const RoverSmallBlockHeap* heap, uint32_t address) {
    for (int32_t row = 0; row < heap->header_count; ++row) {
        if (address - header_at(heap, row).data < kRoverSmallBlockRegionSize)
            return row;
    }
    return -1;
}

void rover_small_block_free(RoverSmallBlockHeap* heap, int32_t row, uint32_t address) {
    Header& header = header_at(heap, row);
    Region& region = region_of(heap, header);
    uint32_t block = address - kSizeWord;
    uint32_t size = word(heap, block) - kInUse;
    const uint32_t group = (address - header.data) / kRoverSmallBlockGroupSize;
    const uint32_t lists = group_address(header, group);
    const uint32_t size_before = word(heap, block - kSizeWord);

    const uint32_t after = block + size;
    const uint32_t size_after = word(heap, after);
    if (!(size_after & kInUse)) {
        if (only_block_in_list(heap, after))
            note_list_emptied(header, region, group, list_for_size(size_after));
        unlink_block(heap, after);
        size += size_after;
    }
    uint32_t list = list_for_size(size);
    bool file = true;
    if (!(size_before & kInUse)) {
        block -= size_before;
        const uint32_t list_before = list_for_size(size_before);
        size += size_before;
        list = list_for_size(size);
        if (list_before == list) {
            file = false;
        } else {
            if (only_block_in_list(heap, block))
                note_list_emptied(header, region, group, list_before);
            unlink_block(heap, block);
        }
    }
    if (file) {
        push_block(heap, lists, list, block);
        if (only_block_in_list(heap, block))
            note_list_filled(header, region, group, list);
    }
    word(heap, block) = size;
    word(heap, block + size - kSizeWord) = size;

    if (--region.groups[group].used != 0)
        return;
    if (heap->deferred >= 0)
        row = decommit_deferred_group(heap, row);
    heap->deferred = row;
    heap->deferred_group = group;
}
