// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rover's small-block allocator over a test arena: block placement,
// merging, the deferred decommit of empty groups, region release and table
// growth, with the whole structure walked and checked along the way.

#include "../src/small_block_heap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Arena addresses: runtime-heap blocks below kRegionBase (address 0 never
// handed out), then one slot per region.
constexpr uint32_t kHeapBase = 0x10000;
constexpr uint32_t kRegionBase = 0x200000;
constexpr uint32_t kRegionSlots = 20;
constexpr uint32_t kArenaSize = kRegionBase + kRegionSlots * kRoverSmallBlockRegionSize;
constexpr uint32_t kGroupsPerRegion = kRoverSmallBlockGroupCount;
constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kMaxHeapBlocks = 64;
constexpr uint8_t kDecommittedFill = 0xdd;

struct HeapBlock {
    uint32_t address{};
    uint32_t size{};
};

struct Arena {
    uint8_t* memory{};
    uint32_t heap_top{};
    HeapBlock heap_blocks[kMaxHeapBlocks]{};
    bool reserved[kRegionSlots]{};
    bool committed[kRegionSlots][kGroupsPerRegion]{};
    uint32_t region_limit{}; // reservations refused beyond this many
    uint32_t reserves{};
    uint32_t releases{};
    uint32_t decommits{};
    uint32_t last_decommit{};
};

Arena* arena_of(void* context) {
    return static_cast<Arena*>(context);
}

uint32_t slot_of(uint32_t address) {
    return (address - kRegionBase) / kRoverSmallBlockRegionSize;
}

uint32_t group_of(uint32_t address) {
    return (address - kRegionBase) % kRoverSmallBlockRegionSize / kRoverSmallBlockGroupSize;
}

uint32_t reserve_region(void* context) {
    Arena* arena = arena_of(context);
    uint32_t in_use = 0;
    for (bool reserved : arena->reserved)
        in_use += reserved;
    if (in_use >= arena->region_limit)
        return 0;
    for (uint32_t slot = 0; slot < kRegionSlots; ++slot) {
        if (!arena->reserved[slot]) {
            arena->reserved[slot] = true;
            ++arena->reserves;
            return kRegionBase + slot * kRoverSmallBlockRegionSize;
        }
    }
    return 0;
}

bool commit_group(void* context, uint32_t address) {
    Arena* arena = arena_of(context);
    check(arena->reserved[slot_of(address)], "a group is committed only inside a reserved region");
    check(!arena->committed[slot_of(address)][group_of(address)], "a group is committed once");
    arena->committed[slot_of(address)][group_of(address)] = true;
    memset(arena->memory + address, 0, kRoverSmallBlockGroupSize);
    return true;
}

void decommit_group(void* context, uint32_t address) {
    Arena* arena = arena_of(context);
    check(
        arena->committed[slot_of(address)][group_of(address)],
        "only a committed group is decommitted"
    );
    arena->committed[slot_of(address)][group_of(address)] = false;
    memset(arena->memory + address, kDecommittedFill, kRoverSmallBlockGroupSize);
    ++arena->decommits;
    arena->last_decommit = address;
}

void release_region(void* context, uint32_t address) {
    Arena* arena = arena_of(context);
    for (bool committed : arena->committed[slot_of(address)])
        check(!committed, "a region is released only with nothing committed");
    arena->reserved[slot_of(address)] = false;
    ++arena->releases;
}

uint32_t heap_alloc(void* context, uint32_t size, bool zeroed) {
    Arena* arena = arena_of(context);
    for (HeapBlock& block : arena->heap_blocks) {
        if (block.address == 0) {
            if (arena->heap_top + size > kRegionBase)
                return 0;
            block.address = arena->heap_top;
            block.size = size;
            arena->heap_top += (size + 15) & ~15u;
            memset(arena->memory + block.address, zeroed ? 0 : 0xcd, size);
            return block.address;
        }
    }
    return 0;
}

HeapBlock* heap_block(Arena* arena, uint32_t address) {
    for (HeapBlock& block : arena->heap_blocks) {
        if (block.address == address)
            return &block;
    }
    return nullptr;
}

void heap_free(void* context, uint32_t address) {
    HeapBlock* block = heap_block(arena_of(context), address);
    check(block != nullptr, "the private heap frees only its own blocks");
    if (block)
        *block = HeapBlock{};
}

uint32_t heap_realloc(void* context, uint32_t address, uint32_t size) {
    Arena* arena = arena_of(context);
    const HeapBlock* old = heap_block(arena, address);
    if (!old)
        return 0;
    const uint32_t old_size = old->size;
    const uint32_t grown = heap_alloc(context, size, false);
    if (!grown)
        return 0;
    memcpy(arena->memory + grown, arena->memory + address, old_size < size ? old_size : size);
    heap_free(context, address);
    return grown;
}

struct Fixture {
    Arena arena{};
    RoverSmallBlockHeap heap{};
};

bool start(Fixture* fixture, uint32_t region_limit = kRegionSlots) {
    *fixture = Fixture{};
    fixture->arena.memory = static_cast<uint8_t*>(malloc(kArenaSize));
    memset(fixture->arena.memory, kDecommittedFill, kArenaSize);
    fixture->arena.heap_top = kHeapBase;
    fixture->arena.region_limit = region_limit;
    RoverSmallBlockSpace space{};
    space.base = reinterpret_cast<uintptr_t>(fixture->arena.memory);
    space.context = &fixture->arena;
    space.reserve_region = reserve_region;
    space.commit_group = commit_group;
    space.decommit_group = decommit_group;
    space.release_region = release_region;
    space.heap_alloc = heap_alloc;
    space.heap_realloc = heap_realloc;
    space.heap_free = heap_free;
    return rover_small_block_init(&fixture->heap, &space);
}

void finish(Fixture* fixture) {
    free(fixture->arena.memory);
}

uint32_t& word(Fixture* fixture, uint32_t address) {
    return *reinterpret_cast<uint32_t*>(fixture->arena.memory + address);
}

const RoverSmallBlockHeader& row(Fixture* fixture, int32_t index) {
    return *reinterpret_cast<const RoverSmallBlockHeader*>(
        fixture->arena.memory + fixture->heap.headers +
        static_cast<uint32_t>(index) * sizeof(RoverSmallBlockHeader)
    );
}

const RoverSmallBlockRegion& region(Fixture* fixture, int32_t index) {
    return *reinterpret_cast<const RoverSmallBlockRegion*>(
        fixture->arena.memory + row(fixture, index).region
    );
}

uint32_t list_of(uint32_t size) {
    const uint32_t list = (size >> 4) - 1;
    return list > 63 ? 63 : list;
}

bool list_bit_set(uint32_t below_32, uint32_t from_32, uint32_t list) {
    const uint32_t bit = 0x80000000u >> (list % 32);
    return ((list < 32 ? below_32 : from_32) & bit) != 0;
}

uint32_t list_anchor(uint32_t region_address, uint32_t group, uint32_t list) {
    return region_address + static_cast<uint32_t>(offsetof(RoverSmallBlockRegion, groups)) +
           group * sizeof(RoverSmallBlockGroup) + list * sizeof(RoverSmallBlockList);
}

// Walks every committed page, list and bit of the structure; false on the
// first inconsistency.
bool walk(Fixture* fixture, const char* when) {
    bool ok = true;
    auto fail = [&](const char* what, uint32_t address) {
        if (ok)
            fprintf(stderr, "FAIL: %s: %s at %#x\n", when, what, address);
        ok = false;
        ++failures;
    };
    for (int32_t index = 0; index < fixture->heap.header_count; ++index) {
        const RoverSmallBlockHeader& header = row(fixture, index);
        const RoverSmallBlockRegion& bookkeeping = region(fixture, index);
        uint32_t groups_with_list[64]{};
        for (uint32_t group = 0; group < kGroupsPerRegion; ++group) {
            const bool committed = !(header.uncommitted & (0x80000000u >> group));
            if (committed != fixture->arena.committed[slot_of(header.data)][group])
                fail("commit bit disagrees with the space", header.data);
            if (!committed) {
                if (bookkeeping.group_lists_below_32[group] ||
                    bookkeeping.group_lists_from_32[group])
                    fail("an uncommitted group offers a list", header.data);
                continue;
            }
            const uint32_t data = header.data + group * kRoverSmallBlockGroupSize;
            int32_t used = 0;
            uint32_t free_in_list[64]{};
            for (uint32_t page = data; page < data + kRoverSmallBlockGroupSize; page += kPageSize) {
                if (word(fixture, page + 8) != 0xffffffffu ||
                    word(fixture, page + 0xffc) != 0xffffffffu)
                    fail("page guard", page);
                bool previous_free = false;
                uint32_t block = page + 0xc;
                while (block < page + 0xffc) {
                    const uint32_t head = word(fixture, block);
                    const uint32_t size = head & ~1u;
                    if (size < 0x10 || size % 0x10 || block + size > page + 0xffc) {
                        fail("block size", block);
                        return false;
                    }
                    if (word(fixture, block + size - 4) != head)
                        fail("trailing size", block);
                    if (head & 1) {
                        ++used;
                        previous_free = false;
                    } else {
                        if (previous_free)
                            fail("two free neighbours", block);
                        previous_free = true;
                        ++free_in_list[list_of(size)];
                    }
                    block += size;
                }
                if (block != page + 0xffc)
                    fail("page does not tile", page);
            }
            if (used != bookkeeping.groups[group].used)
                fail("group use count", data);
            for (uint32_t list = 0; list < 64; ++list) {
                const uint32_t anchor = list_anchor(header.region, group, list);
                uint32_t members = 0;
                uint32_t previous = anchor;
                for (uint32_t entry = word(fixture, anchor + 4); entry != anchor;
                     entry = word(fixture, entry + 4)) {
                    if (word(fixture, entry + 8) != previous)
                        fail("back link", entry);
                    if (entry < data || entry >= data + kRoverSmallBlockGroupSize ||
                        (word(fixture, entry) & 1) || list_of(word(fixture, entry)) != list) {
                        fail("list member", entry);
                        return false;
                    }
                    previous = entry;
                    if (++members > 0x1000) {
                        fail("list loops", anchor);
                        return false;
                    }
                }
                if (word(fixture, anchor + 8) != previous)
                    fail("list tail", anchor);
                if (members != free_in_list[list])
                    fail("free block missing from its list", anchor);
                const bool offered = list_bit_set(
                    bookkeeping.group_lists_below_32[group],
                    bookkeeping.group_lists_from_32[group],
                    list
                );
                if (offered != (members != 0))
                    fail("group list bit", anchor);
                groups_with_list[list] += members != 0;
            }
        }
        for (uint32_t list = 0; list < 64; ++list) {
            if (bookkeeping.groups_with_list[list] != groups_with_list[list])
                fail("region list count", header.data);
            if (list_bit_set(header.lists_below_32, header.lists_from_32, list) !=
                (groups_with_list[list] != 0))
                fail("region list bit", header.data);
        }
    }
    return ok;
}

uint32_t alloc(Fixture* fixture, uint32_t size) {
    return rover_small_block_alloc(&fixture->heap, size);
}

void release(Fixture* fixture, uint32_t address) {
    const int32_t index = rover_small_block_region(&fixture->heap, address);
    check(index >= 0, "a block is found in its region");
    if (index >= 0)
        rover_small_block_free(&fixture->heap, index, address);
}

void check_first_blocks() {
    Fixture fixture{};
    check(start(&fixture), "the table is allocated");
    check(
        fixture.heap.header_capacity == 16 && fixture.heap.deferred == -1,
        "an empty table of 16 rows"
    );

    const uint32_t first = alloc(&fixture, 33);
    const uint32_t page = kRegionBase;
    check(first == page + 0xfd0, "the first block is carved from the end of the first page");
    check(word(&fixture, first - 4) == 0x31, "a 33-byte request takes 48 bytes, marked in use");
    check(word(&fixture, page + 0xff8) == 0x31, "and repeats its size in its last word");
    check(word(&fixture, page + 0xc) == 0xfc0, "the page's free block shrinks in place");
    check(word(&fixture, page + 0xfc8) == 0xfc0, "with its trailing size");
    check(fixture.arena.reserves == 1 && fixture.arena.committed[0][0], "one region, group 0");

    const uint32_t second = alloc(&fixture, 1);
    check(
        second == page + 0xfc0 && word(&fixture, second - 4) == 0x11,
        "the smallest block is 16 bytes"
    );
    const uint32_t empty = alloc(&fixture, 0);
    check(
        empty == page + 0xfb0 && word(&fixture, empty - 4) == 0x11, "a zero request is 16 bytes too"
    );
    const uint32_t largest = alloc(&fixture, kRoverSmallBlockLimit);
    check(largest == page + 0xbb0 && word(&fixture, largest - 4) == 0x401, "0x3f8 bytes take 1024");
    check(first % 16 == 0 && second % 16 == 0 && largest % 16 == 0, "blocks are 16-byte aligned");
    walk(&fixture, "four blocks");

    check(
        rover_small_block_region(&fixture.heap, kHeapBase) == -1 &&
            rover_small_block_region(&fixture.heap, kRegionBase + kRoverSmallBlockRegionSize) == -1,
        "memory outside the regions is not theirs"
    );

    release(&fixture, second);
    walk(&fixture, "a middle block freed");
    check(word(&fixture, second - 4) == 0x10, "a freed block is marked free");
    release(&fixture, empty);
    walk(&fixture, "two neighbours freed");
    check(word(&fixture, empty - 4) == 0x20, "free neighbours merge");
    release(&fixture, largest);
    release(&fixture, first);
    walk(&fixture, "all freed");
    check(word(&fixture, page + 0xc) == 0xff0, "an emptied page is one free block again");
    check(
        fixture.heap.deferred == 0 && fixture.heap.deferred_group == 0 &&
            fixture.arena.decommits == 0,
        "an emptied group stays committed, awaiting decommit"
    );
    const uint32_t again = alloc(&fixture, 33);
    check(again == first && fixture.heap.deferred == -1, "reusing the group cancels its decommit");
    finish(&fixture);
}

void check_page_filling() {
    Fixture fixture{};
    start(&fixture);
    uint32_t blocks[3]{};
    for (uint32_t& block : blocks)
        block = alloc(&fixture, kRoverSmallBlockLimit);
    check(
        blocks[0] == kRegionBase + 0xc00 && blocks[1] == kRegionBase + 0x800 &&
            blocks[2] == kRegionBase + 0x400,
        "a page holds three 1024-byte blocks, taken from its end"
    );
    check(word(&fixture, kRegionBase + 0xc) == 0x3f0, "leaving a 1008-byte block in list 62");
    const uint32_t fourth = alloc(&fixture, kRoverSmallBlockLimit);
    check(fourth == kRegionBase + 0x1c00, "the next 1024 bytes come from the next page");
    const uint32_t small = alloc(&fixture, 0x100);
    check(
        small == kRegionBase + 0x10 + 0x3f0 - 0x110, "a smaller block takes the lowest fitting list"
    );
    walk(&fixture, "filled pages");
    finish(&fixture);
}

void check_block_sizes() {
    Fixture fixture{};
    start(&fixture);
    bool sized = true;
    bool fits = true;
    for (uint32_t size = 0; size <= kRoverSmallBlockLimit; ++size) {
        const uint32_t block = alloc(&fixture, size);
        const uint32_t expected = (size + 0x17) & ~0xfu;
        sized = sized && word(&fixture, block - 4) == (expected | 1) &&
                word(&fixture, block - 4 + expected - 4) == (expected | 1);
        fits = fits && size + 8 <= expected;
    }
    check(sized, "a request takes its size plus two size words, in whole 16-byte units");
    check(fits, "the caller's bytes never reach the trailing size word");
    walk(&fixture, "every size");
    finish(&fixture);
}

// Fills a whole group with 24 blocks of 1024 bytes.
void fill_group(Fixture* fixture, uint32_t* blocks) {
    for (uint32_t index = 0; index < 24; ++index)
        blocks[index] = alloc(fixture, kRoverSmallBlockLimit);
}

void empty_group(Fixture* fixture, const uint32_t* blocks) {
    for (uint32_t index = 0; index < 24; ++index)
        release(fixture, blocks[index]);
}

void check_deferred_decommit() {
    Fixture fixture{};
    start(&fixture);
    uint32_t group_0[24]{};
    uint32_t group_1[24]{};
    fill_group(&fixture, group_0);
    check(!fixture.arena.committed[0][1], "a full group does not commit the next early");
    fill_group(&fixture, group_1);
    check(
        fixture.arena.committed[0][1] && group_1[0] == kRegionBase + 0x8c00,
        "the next group is committed when the first is full"
    );
    walk(&fixture, "two full groups");

    empty_group(&fixture, group_1);
    check(
        fixture.arena.decommits == 0 && fixture.heap.deferred == 0 &&
            fixture.heap.deferred_group == 1,
        "the first group to empty only waits"
    );
    empty_group(&fixture, group_0);
    check(
        fixture.arena.decommits == 1 && fixture.arena.last_decommit == kRegionBase + 0x8000,
        "the next group to empty decommits the waiting one"
    );
    check(
        fixture.heap.deferred == 0 && fixture.heap.deferred_group == 0 &&
            row(&fixture, 0).uncommitted == 0x7fffffffu,
        "and waits in its place"
    );
    walk(&fixture, "one group decommitted");

    fill_group(&fixture, group_0);
    fill_group(&fixture, group_1);
    check(
        fixture.arena.committed[0][1] && group_1[0] == kRegionBase + 0x8c00,
        "a decommitted group is committed afresh"
    );
    walk(&fixture, "recommitted");
    finish(&fixture);
}

void check_region_release() {
    Fixture fixture{};
    start(&fixture);
    uint32_t blocks[33][24]{};
    for (auto& group : blocks)
        fill_group(&fixture, group);
    check(
        fixture.heap.header_count == 2 && fixture.arena.reserves == 2,
        "a full region reserves another"
    );
    check(
        blocks[32][0] == kRegionBase + kRoverSmallBlockRegionSize + 0xc00,
        "the second region's first block"
    );
    walk(&fixture, "two regions");

    for (uint32_t group = 1; group < 32; ++group)
        empty_group(&fixture, blocks[group]);
    empty_group(&fixture, blocks[0]);
    check(
        row(&fixture, 0).uncommitted == 0x7fffffffu && fixture.heap.deferred == 0 &&
            fixture.heap.deferred_group == 0,
        "the first region keeps only its waiting group"
    );
    walk(&fixture, "first region emptied");

    empty_group(&fixture, blocks[32]);
    check(
        fixture.arena.releases == 1 && fixture.heap.header_count == 1,
        "a region with nothing committed is released"
    );
    check(
        row(&fixture, 0).data == kRegionBase + kRoverSmallBlockRegionSize &&
            fixture.heap.deferred == 0 && fixture.heap.scan == 0,
        "the table closes up and the waiting group's row is renumbered"
    );
    walk(&fixture, "first region released");
    finish(&fixture);
}

void check_table_growth_and_exhaustion() {
    Fixture fixture{};
    start(&fixture, 17);
    static uint32_t blocks[17 * 32][24];
    for (auto& group : blocks)
        fill_group(&fixture, group);
    check(
        fixture.heap.header_count == 17 && fixture.heap.header_capacity == 32,
        "the table grows by 16 rows"
    );
    walk(&fixture, "seventeen regions");
    const uint32_t refused = alloc(&fixture, kRoverSmallBlockLimit);
    check(refused == 0, "no block when no region can be reserved");
    const uint32_t smaller = alloc(&fixture, 0x3e0);
    check(smaller != 0, "a request that fits a leftover block still succeeds");
    release(&fixture, smaller);
    for (auto& group : blocks)
        empty_group(&fixture, group);
    check(fixture.heap.header_count == 1, "every emptied region but the waiting one is released");
    walk(&fixture, "all regions emptied");
    finish(&fixture);
}

uint32_t next_random(uint32_t* state) {
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}

void check_random_use() {
    Fixture fixture{};
    start(&fixture);
    constexpr uint32_t kLive = 3000;
    static uint32_t address[kLive];
    static uint32_t size[kLive];
    memset(address, 0, sizeof address);
    uint32_t state = 0x1234567;
    for (uint32_t step = 0; step < 60000; ++step) {
        const uint32_t slot = next_random(&state) % kLive;
        if (address[slot]) {
            const uint8_t fill = static_cast<uint8_t>(slot);
            for (uint32_t offset = 0; offset < size[slot]; ++offset) {
                if (fixture.arena.memory[address[slot] + offset] != fill) {
                    check(false, "a live block keeps its contents");
                    break;
                }
            }
            release(&fixture, address[slot]);
            address[slot] = 0;
        } else {
            const uint32_t roll = next_random(&state);
            size[slot] = roll % 4 == 0 ? roll % (kRoverSmallBlockLimit + 1) : roll % 64;
            address[slot] = alloc(&fixture, size[slot]);
            check(address[slot] != 0, "a random request is served");
            memset(fixture.arena.memory + address[slot], static_cast<uint8_t>(slot), size[slot]);
        }
        if (step % 5000 == 0 && !walk(&fixture, "random use"))
            break;
    }
    for (uint32_t slot = 0; slot < kLive; ++slot) {
        if (address[slot])
            release(&fixture, address[slot]);
    }
    walk(&fixture, "random use freed");
    check(fixture.heap.header_count == 1, "everything freed leaves one region");
    finish(&fixture);
}

} // namespace

int main() {
    check_first_blocks();
    check_page_filling();
    check_block_sizes();
    check_deferred_decommit();
    check_region_release();
    check_table_growth_and_exhaustion();
    check_random_use();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("rover small-block heap: all checks passed\n");
    return 0;
}
