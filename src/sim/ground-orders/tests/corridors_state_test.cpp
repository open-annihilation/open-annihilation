// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Canonical corridor state, exact interrupted construction and transactional validation.
#include "oa/sim/ground_orders/corridors.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace {
bool watch_allocations{};
size_t allocation_count{};
} // namespace

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(size_t size) {
    if (watch_allocations)
        ++allocation_count;
    if (void* result = std::malloc(size == 0 ? 1 : size))
        return result;
    throw std::bad_alloc{};
}

void* operator new[](size_t size) {
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {
namespace orders = oa::sim::ground_orders;
using Cell = orders::CorridorOrigin;
using Catalog = orders::CorridorCatalog;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t width = 64, height = 64, corridor_capacity = 4, queue_capacity = 4;
constexpr uint32_t bucket_columns = 8, bucket_rows = 8, bucket_count = 64;
constexpr Cell footprint{2, 2};
constexpr orders::CorridorLimits limits{corridor_capacity, queue_capacity, width * height};
constexpr size_t header_size = 128, builder_size = 160, geometry_size = 64, seed_size = 8;
constexpr size_t builder_offset = header_size;
constexpr size_t geometry_offset = header_size + builder_size;
constexpr size_t queue_offset = geometry_offset + geometry_size * corridor_capacity;
constexpr size_t bucket_offset = queue_offset + seed_size * queue_capacity;
constexpr size_t canonical_size = bucket_offset + 8 * bucket_count;
constexpr uint32_t active_flag = 1, clearing_flag = 2;
constexpr uint8_t upper_mouth_phase = 4, index_phase = 5;
constexpr uint64_t revision = 0x0102030405060708ull;

template <class Function>
auto no_allocation(Function&& function) {
    allocation_count = 0;
    watch_allocations = true;
    const auto result = function();
    watch_allocations = false;
    OA_CHECK(allocation_count == 0);
    return result;
}

uint64_t read_le(std::span<const uint8_t> bytes, size_t offset, size_t count) {
    uint64_t value{};
    for (size_t i = 0; i < count; ++i)
        value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}

void write_le(Bytes& bytes, size_t offset, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i)
        bytes[offset + i] = uint8_t(value >> (8 * i));
}

struct Sample {
    Cell origin{}, footprint{};
    bool operator==(const Sample&) const = default;
};

struct Grid {
    std::array<uint8_t, width * height> plots{};
    std::array<Sample, 4096> trace{};
    size_t calls{};

    Grid() {
        for (const int16_t low : {int16_t(12), int16_t(36)}) {
            rectangle(2, low - 8, 11, low + 11);
            rectangle(12, low, 29, low + 3);
            rectangle(30, low - 8, 43, low + 11);
        }
    }

    void rectangle(int16_t left, int16_t top, int16_t right, int16_t bottom) {
        for (int32_t z = top; z <= bottom; ++z)
            for (int32_t x = left; x <= right; ++x)
                plots[size_t(z) * width + size_t(x)] = 1;
    }

    orders::CorridorSampler sampler() {
        return {this, [](void* context, Cell origin, Cell size) {
                    auto& grid = *static_cast<Grid*>(context);
                    OA_CHECK(grid.calls < grid.trace.size());
                    if (grid.calls < grid.trace.size())
                        grid.trace[grid.calls] = {origin, size};
                    ++grid.calls;
                    if (origin[0] < 0 || origin[1] < 0 ||
                        uint32_t(origin[0]) + uint32_t(size[0]) > width ||
                        uint32_t(origin[1]) + uint32_t(size[1]) > height)
                        return orders::CorridorCell::blocked;
                    for (int32_t z = 0; z < size[1]; ++z)
                        for (int32_t x = 0; x < size[0]; ++x)
                            if (!grid.plots[size_t(origin[1] + z) * width + size_t(origin[0] + x)])
                                return orders::CorridorCell::blocked;
                    return orders::CorridorCell::clear;
                }};
    }
};

Bytes save(const Catalog& catalog) {
    Bytes bytes(no_allocation([&] { return catalog.saved_size(); }));
    OA_CHECK(no_allocation([&] { return catalog.write_state(bytes); }));
    return bytes;
}

void reset(Catalog& catalog, uint64_t next_revision) {
    no_allocation([&] {
        catalog.reset(next_revision);
        return true;
    });
}

void request(Catalog& catalog, Cell seed) {
    OA_CHECK(no_allocation([&] {
                 return catalog.request(seed, orders::CorridorAxis::x);
             }) == orders::CorridorRequest::queued);
}

orders::CorridorStatus status(const Catalog& catalog) {
    return no_allocation([&] { return catalog.status(); });
}

uint32_t advance(Catalog& catalog, Grid& grid, uint32_t credit) {
    orders::CorridorBudget budget{credit, 7};
    no_allocation([&] {
        catalog.advance(budget, grid.sampler());
        return true;
    });
    OA_CHECK(budget.remaining + budget.used == credit + 7);
    OA_CHECK(budget.used >= 7 && budget.used - 7 <= credit);
    return budget.used - 7;
}

bool idle(const Catalog& catalog) {
    const auto current = status(catalog);
    return !current.clearing && !current.building && current.pending_seeds == 0;
}

void finish(Catalog& catalog, Grid& grid, uint32_t quantum) {
    for (uint32_t pass = 0; pass < 100'000 && !idle(catalog); ++pass)
        advance(catalog, grid, quantum);
    OA_CHECK(idle(catalog));
}

bool same_status(const orders::CorridorStatus& a, const orders::CorridorStatus& b) {
    return a.corridors == b.corridors && a.pending_seeds == b.pending_seeds &&
           a.revision == b.revision && a.work_units == b.work_units && a.samples == b.samples &&
           a.rejected_seeds == b.rejected_seeds && a.clearing == b.clearing &&
           a.building == b.building && a.enabled == b.enabled;
}

void query_parity(const Catalog& first, const Catalog& second) {
    for (const auto cell : {Cell{0, 0}, Cell{9, 12}, Cell{20, 12}, Cell{20, 36}, Cell{31, 36}}) {
        std::array<orders::CorridorContact, 8> a{}, b{};
        orders::CorridorBudget qa{31, 0}, qb{31, 0};
        const auto ra = no_allocation([&] { return first.at_origin(cell, a, qa); });
        const auto rb = no_allocation([&] { return second.at_origin(cell, b, qb); });
        OA_CHECK(ra.count == rb.count && ra.complete == rb.complete);
        OA_CHECK(qa.remaining == qb.remaining && qa.used == qb.used);
        for (uint32_t i = 0; i < std::min<uint32_t>(ra.count, a.size()); ++i)
            OA_CHECK(a[i].id == b[i].id && a[i].region == b[i].region);
    }
    oa::sim::unit_movement::Unit unit{};
    unit.cell = {6, 12};
    unit.footprint = footprint;
    unit.position = {(6 * 16 + 16) * 65536, 0, (12 * 16 + 16) * 65536};
    orders::Goal goal{};
    orders::Navigation navigation{};
    navigation.goal = &goal;
    navigation.count = 2;
    navigation.flags = orders::route_present_flag;
    navigation.points[0] = {800, 800};
    navigation.points[1] = {40 * 16 + 16, 12 * 16 + 16};
    std::array<orders::CorridorApproach, 8> a{}, b{};
    orders::CorridorBudget qa{512, 0}, qb{512, 0};
    const auto ra = no_allocation([&] { return first.upcoming(unit, navigation, a, qa); });
    const auto rb = no_allocation([&] { return second.upcoming(unit, navigation, b, qb); });
    OA_CHECK(ra.count == rb.count && ra.complete == rb.complete);
    OA_CHECK(qa.used == qb.used && qa.remaining == qb.remaining);
    for (uint32_t i = 0; i < std::min<uint32_t>(ra.count, a.size()); ++i)
        OA_CHECK(
            a[i].id == b[i].id && a[i].direction == b[i].direction &&
            a[i].entry_segment == b[i].entry_segment && a[i].exit_segment == b[i].exit_segment &&
            a[i].already_inside == b[i].already_inside &&
            a[i].exits_other_end == b[i].exits_other_end
        );
}

struct Snapshots {
    std::vector<Bytes> construction;
    Bytes partial_index, ready, clearing, pending, active_pending, wrapped, duplicate_result;
};

Snapshots snapshots() {
    Grid grid;
    Catalog catalog(width, height, footprint, limits);
    reset(catalog, revision);
    request(catalog, {20, 12});
    request(catalog, {20, 36});
    Snapshots result;
    result.pending = save(catalog);
    std::array<bool, 6> phases{};
    std::array<bool, 24> phase_sections{};
    for (uint32_t pass = 0; pass < 100'000 && !idle(catalog); ++pass) {
        advance(catalog, grid, 1);
        auto bytes = save(catalog);
        const auto flags = read_le(bytes, 52, 4);
        if (!(flags & active_flag))
            continue;
        const auto phase = bytes[builder_offset + 24];
        const auto section = bytes[builder_offset + 16];
        OA_CHECK(phase < phases.size() && section < 4);
        if (phase >= phases.size() || section >= 4)
            continue;
        phases[phase] = true;
        if (!phase_sections[size_t(phase) * 4 + section]) {
            result.construction.push_back(bytes);
            phase_sections[size_t(phase) * 4 + section] = true;
        }
        if (result.active_pending.empty() && read_le(bytes, 44, 4) != 0)
            result.active_pending = bytes;
        if (result.partial_index.empty() && phase == index_phase && read_le(bytes, 36, 4) == 0) {
            for (size_t bucket = 0; bucket < bucket_count; ++bucket)
                if (read_le(bytes, bucket_offset + 8 * bucket, 8) != 0) {
                    result.partial_index = bytes;
                    break;
                }
        }
    }
    OA_CHECK(idle(catalog));
    OA_CHECK(std::all_of(phases.begin(), phases.end(), [](bool value) { return value; }));
    OA_CHECK(!result.partial_index.empty() && !result.active_pending.empty());
    result.ready = save(catalog);
    OA_CHECK(read_le(result.ready, 36, 4) == 2);
    request(catalog, {50, 50});
    request(catalog, {51, 50});
    request(catalog, {52, 50});
    result.wrapped = save(catalog);
    OA_CHECK(read_le(result.wrapped, 40, 4) + read_le(result.wrapped, 44, 4) > queue_capacity);
    reset(catalog, revision + 1);
    request(catalog, {20, 12});
    request(catalog, {20, 36});
    advance(catalog, grid, 3);
    result.clearing = save(catalog);
    OA_CHECK(read_le(result.clearing, 48, 4) == 3);
    OA_CHECK(read_le(result.clearing, 52, 4) == clearing_flag);
    Catalog duplicate(width, height, footprint, limits);
    request(duplicate, {20, 12});
    request(duplicate, {21, 13});
    finish(duplicate, grid, 13);
    result.duplicate_result = save(duplicate);
    OA_CHECK(read_le(result.duplicate_result, 36, 4) == 1);
    OA_CHECK(read_le(result.duplicate_result, builder_offset + 48, 2) == 1);
    OA_CHECK(
        read_le(result.duplicate_result, builder_offset + 48 + 4, 2) ==
        static_cast<uint64_t>(footprint[0])
    );
    return result;
}

void replay_live_prefix(Catalog& catalog, const Bytes& saved) {
    Grid grid;
    const auto saved_revision = read_le(saved, 56, 8);
    if (saved_revision == 0) {
        request(catalog, {20, 12});
        request(catalog, {21, 13});
    } else {
        reset(catalog, revision);
        request(catalog, {20, 12});
        request(catalog, {20, 36});
        if (saved_revision == revision + 1 || read_le(saved, 44, 4) == 3) {
            finish(catalog, grid, 1);
            request(catalog, {50, 50});
            request(catalog, {51, 50});
            request(catalog, {52, 50});
            if (saved_revision == revision + 1) {
                reset(catalog, revision + 1);
                request(catalog, {20, 12});
                request(catalog, {20, 36});
            }
        }
    }
    const auto target_work = read_le(saved, 64, 8);
    for (uint32_t pass = 0; pass < 100'000 && status(catalog).work_units < target_work; ++pass)
        advance(catalog, grid, 1);
    OA_CHECK(status(catalog).work_units == target_work);
    OA_CHECK(save(catalog) == saved);
}

void resume_exactly(const Bytes& saved) {
    Catalog first(width, height, footprint, limits), second(width, height, footprint, limits);
    replay_live_prefix(first, saved);
    OA_CHECK(no_allocation([&] { return second.can_restore(saved); }));
    OA_CHECK(no_allocation([&] { return second.restore(saved); }));
    OA_CHECK(save(first) == saved && save(second) == saved);
    const auto sample_start = status(first).samples;
    Grid a, b;
    for (const uint32_t credit : {0u, 1u, 7u, 31u, 113u}) {
        const auto used_a = advance(first, a, credit);
        uint32_t used_b{}, remaining = credit;
        while (remaining) {
            const auto part = std::min<uint32_t>(remaining, 3);
            used_b += advance(second, b, part);
            remaining -= part;
        }
        OA_CHECK(used_a == used_b);
        OA_CHECK(same_status(status(first), status(second)));
        OA_CHECK(save(first) == save(second));
        query_parity(first, second);
    }
    finish(first, a, 1);
    finish(second, b, 47);
    OA_CHECK(save(first) == save(second));
    OA_CHECK(a.calls == b.calls && a.calls <= a.trace.size());
    OA_CHECK(status(first).samples - sample_start == a.calls);
    OA_CHECK(
        std::equal(
            a.trace.begin(), a.trace.begin() + std::min(a.calls, a.trace.size()), b.trace.begin()
        )
    );
    query_parity(first, second);
    const auto cached = no_allocation([&] { return first.state_bytes(); });
    OA_CHECK(no_allocation([&] { return first.can_restore(cached); }));
    OA_CHECK(no_allocation([&] { return first.restore(cached); }));
    OA_CHECK(save(first) == save(second));
}

void canonical_layout_and_buffers(const Bytes& ready, const Bytes& different_valid_state) {
    Catalog catalog(width, height, footprint, limits);
    OA_CHECK(no_allocation([&] { return catalog.saved_size(); }) == canonical_size);
    OA_CHECK(ready.size() == canonical_size);
    constexpr std::array<uint8_t, 4> magic{'O', 'C', 'O', 'R'};
    OA_CHECK(std::equal(magic.begin(), magic.end(), ready.begin()));
    OA_CHECK(read_le(ready, 4, 4) == 1);
    OA_CHECK(read_le(ready, 8, 4) == width && read_le(ready, 12, 4) == height);
    OA_CHECK(read_le(ready, 16, 2) == 2 && read_le(ready, 18, 2) == 2);
    OA_CHECK(read_le(ready, 20, 4) == corridor_capacity);
    OA_CHECK(read_le(ready, 24, 4) == queue_capacity);
    OA_CHECK(read_le(ready, 28, 4) == bucket_columns && read_le(ready, 32, 4) == bucket_rows);
    OA_CHECK(read_le(ready, 56, 8) == revision && ready[56] == 8 && ready[63] == 1);
    OA_CHECK(read_le(ready, 88, 8) == canonical_size);
    OA_CHECK(std::all_of(ready.begin() + 96, ready.begin() + 128, [](uint8_t byte) {
        return byte == 0;
    }));
    Bytes short_output(canonical_size - 1, 0xa5), long_output(canonical_size + 1, 0xa5);
    OA_CHECK(!no_allocation([&] { return catalog.write_state(short_output); }));
    OA_CHECK(!no_allocation([&] { return catalog.write_state(long_output); }));
    OA_CHECK(std::all_of(short_output.begin(), short_output.end(), [](uint8_t byte) {
        return byte == 0xa5;
    }));
    OA_CHECK(std::all_of(long_output.begin(), long_output.end(), [](uint8_t byte) {
        return byte == 0xa5;
    }));
    OA_CHECK(no_allocation([&] { return catalog.restore(ready); }));
    const auto before = save(catalog);
    const auto* borrowed = no_allocation([&] { return catalog.geometry(1); });
    const auto old_status = status(catalog);
    const Catalog& logical_const = catalog;
    OA_CHECK(no_allocation([&] { return logical_const.can_restore(ready); }));
    OA_CHECK(save(catalog) == before && same_status(status(catalog), old_status));
    OA_CHECK(no_allocation([&] { return catalog.geometry(1); }) == borrowed);
    OA_CHECK(no_allocation([&] { return logical_const.can_restore(different_valid_state); }));
    OA_CHECK(save(catalog) == before && same_status(status(catalog), old_status));
    OA_CHECK(no_allocation([&] { return catalog.geometry(1); }) == borrowed);
    const auto view = no_allocation([&] { return catalog.state_bytes(); });
    OA_CHECK(std::equal(before.begin(), before.end(), view.begin()));
    OA_CHECK(no_allocation([&] { return logical_const.can_restore(view); }));
    OA_CHECK(std::equal(before.begin(), before.end(), view.begin()));
    Catalog disabled(0, height, footprint, limits);
    OA_CHECK(no_allocation([&] { return disabled.saved_size(); }) == 0);
    OA_CHECK(no_allocation([&] { return disabled.state_bytes(); }).empty());
    OA_CHECK(!no_allocation([&] { return disabled.write_state({}); }));
    OA_CHECK(!no_allocation([&] { return disabled.can_restore({}); }));
    OA_CHECK(!no_allocation([&] { return disabled.restore({}); }));
}

void rejected_without_changes(
    Catalog& target,
    std::span<const uint8_t> input,
    size_t mutation_offset = std::numeric_limits<size_t>::max()
) {
    const auto before = save(target);
    Bytes after(before.size());
    const auto old_status = status(target);
    const auto* borrowed = no_allocation([&] { return target.geometry(1); });
    const auto accepted = no_allocation([&] { return target.can_restore(input); });
    if (accepted)
        std::fprintf(
            stderr,
            "accepted malformed state: size=%zu mutation=%zu\n",
            input.size(),
            mutation_offset
        );
    OA_CHECK(!accepted);
    OA_CHECK(no_allocation([&] { return target.write_state(after); }) && after == before);
    OA_CHECK(same_status(status(target), old_status));
    OA_CHECK(no_allocation([&] { return target.geometry(1); }) == borrowed);
    OA_CHECK(!no_allocation([&] { return target.restore(input); }));
    OA_CHECK(no_allocation([&] { return target.write_state(after); }) && after == before);
    OA_CHECK(same_status(status(target), old_status));
    OA_CHECK(no_allocation([&] { return target.geometry(1); }) == borrowed);
}

void malformed_headers_and_records_are_atomic(const Snapshots& states) {
    Catalog target(width, height, footprint, limits);
    OA_CHECK(no_allocation([&] { return target.restore(states.ready); }));
    const auto changed = [&](const Bytes& original, size_t offset, uint64_t value, size_t size) {
        auto broken = original;
        write_le(broken, offset, value, size);
        rejected_without_changes(target, broken, offset);
    };
    for (const auto offset :
         {size_t(0),
          size_t(4),
          size_t(8),
          size_t(12),
          size_t(20),
          size_t(24),
          size_t(28),
          size_t(32),
          size_t(88)})
        changed(
            states.ready,
            offset,
            read_le(states.ready, offset, offset == 88 ? 8 : 4) + 1,
            offset == 88 ? 8 : 4
        );
    changed(states.ready, 16, 1, 2);
    changed(states.ready, 18, 1, 2);
    changed(states.ready, 36, corridor_capacity + 1, 4);
    changed(states.ready, 40, queue_capacity, 4);
    changed(states.ready, 44, queue_capacity + 1, 4);
    changed(states.ready, 48, bucket_count + 1, 4);
    changed(states.ready, 52, 4, 4);
    changed(states.ready, 52, active_flag | clearing_flag, 4);
    for (size_t i = 96; i < header_size; ++i)
        changed(states.ready, i, 1, 1);
    for (size_t i = 0; i < canonical_size; ++i)
        rejected_without_changes(target, std::span<const uint8_t>(states.ready).first(i));
    auto oversized = states.ready;
    oversized.push_back(0);
    rejected_without_changes(target, oversized);
    for (const auto offset : {size_t(2), size_t(3)})
        changed(states.ready, geometry_offset + offset, 255, 1);
    changed(states.ready, geometry_offset, 2, 2);
    changed(states.ready, geometry_offset + 4, 1, 2);
    changed(states.ready, geometry_offset + 8, uint16_t(-1), 2);
    changed(states.ready, geometry_offset + 16 + 4, width, 2);
    changed(states.ready, geometry_offset + 24, width, 2);
    changed(states.ready, geometry_offset + 40, width, 2);
    changed(states.ready, geometry_offset + 44, revision + 1, 8);
    for (size_t slot = 0; slot < corridor_capacity; ++slot)
        for (size_t i = 52; i < geometry_size; ++i)
            changed(states.ready, geometry_offset + slot * geometry_size + i, 1, 1);
    for (size_t slot = 0; slot < queue_capacity; ++slot) {
        changed(states.ready, queue_offset + slot * seed_size, uint16_t(-1), 2);
        changed(states.ready, queue_offset + slot * seed_size + 4, 255, 1);
        for (size_t i = 5; i < seed_size; ++i)
            changed(states.ready, queue_offset + slot * seed_size + i, 1, 1);
    }
    for (const auto range :
         {std::array<size_t, 2>{5, 8}, {19, 24}, {25, 26}, {39, 48}, {100, 112}, {132, 160}})
        for (size_t i = range[0]; i < range[1]; ++i)
            changed(states.ready, builder_offset + i, 1, 1);
    const auto& active = states.active_pending;
    changed(active, builder_offset + 4, 255, 1);
    changed(active, builder_offset, uint16_t(-1), 2);
    changed(active, builder_offset + 16, 4, 1);
    changed(active, builder_offset + 17, 2, 1);
    changed(active, builder_offset + 18, 2, 1);
    changed(active, builder_offset + 24, 6, 1);
    changed(active, builder_offset + 38, 2, 1);
    changed(active, builder_offset + 8, std::numeric_limits<int16_t>::max(), 2);
    changed(states.partial_index, builder_offset + 112, bucket_columns, 4);
    changed(states.partial_index, builder_offset + 128, bucket_rows, 4);
}

void duplicate_and_index_corruption_is_rejected(const Snapshots& states) {
    Catalog target(width, height, footprint, limits);
    OA_CHECK(no_allocation([&] { return target.restore(states.ready); }));
    auto pending = states.pending;
    std::copy_n(
        pending.begin() + queue_offset, seed_size, pending.begin() + queue_offset + seed_size
    );
    rejected_without_changes(target, pending);
    auto active = states.active_pending;
    const auto head = read_le(active, 40, 4);
    std::copy_n(
        active.begin() + builder_offset, seed_size, active.begin() + queue_offset + seed_size * head
    );
    rejected_without_changes(target, active);
    auto duplicate = states.ready;
    std::copy_n(
        duplicate.begin() + geometry_offset,
        geometry_size,
        duplicate.begin() + geometry_offset + geometry_size
    );
    write_le(duplicate, geometry_offset + geometry_size, 2, 2);
    for (size_t i = 0; i < bucket_count; ++i) {
        const auto old = read_le(duplicate, bucket_offset + 8 * i, 8);
        write_le(duplicate, bucket_offset + 8 * i, (old & ~uint64_t(2)) | ((old & 1) ? 2 : 0), 8);
    }
    rejected_without_changes(target, duplicate);
    for (const auto* saved : {&states.ready, &states.partial_index}) {
        auto broken = *saved;
        bool changed{};
        for (size_t i = 0; i < bucket_count; ++i) {
            const auto old = read_le(broken, bucket_offset + 8 * i, 8);
            if (old) {
                write_le(broken, bucket_offset + 8 * i, old & (old - 1), 8);
                changed = true;
                break;
            }
        }
        OA_CHECK(changed);
        rejected_without_changes(target, broken);
    }
    auto outside = states.ready;
    write_le(outside, bucket_offset, uint64_t(1) << 63, 8);
    rejected_without_changes(target, outside);
    auto clearing = states.clearing;
    write_le(clearing, bucket_offset, 1, 8);
    rejected_without_changes(target, clearing);
    clearing = states.clearing;
    write_le(clearing, bucket_offset + 8 * (bucket_count - 1), uint64_t(1) << 63, 8);
    rejected_without_changes(target, clearing);
    clearing = states.clearing;
    write_le(clearing, bucket_offset + 8 * (bucket_count - 1), uint64_t(1) << 3, 8);
    OA_CHECK(no_allocation([&] { return target.can_restore(clearing); }));
    OA_CHECK(no_allocation([&] { return target.restore(clearing); }));
    OA_CHECK(save(target) == clearing);
    // A bounded stale builder result need not be a published slot.
    auto inactive = states.ready;
    write_le(inactive, builder_offset + 48, 0, 2);
    write_le(inactive, builder_offset + 48 + 44, 0, 8);
    rejected_without_changes(target, inactive);
    inactive[builder_offset + 24] = upper_mouth_phase;
    for (size_t offset = 112; offset < 132; offset += 4)
        write_le(inactive, builder_offset + offset, 0, 4);
    OA_CHECK(no_allocation([&] { return target.can_restore(inactive); }));
    OA_CHECK(no_allocation([&] { return target.restore(inactive); }));
    OA_CHECK(save(target) == inactive);
}

} // namespace

int main() {
    const auto states = snapshots();
    canonical_layout_and_buffers(states.ready, states.active_pending);
    for (const auto& saved : states.construction)
        resume_exactly(saved);
    for (const auto* saved :
         {&states.partial_index,
          &states.ready,
          &states.clearing,
          &states.pending,
          &states.active_pending,
          &states.wrapped,
          &states.duplicate_result})
        resume_exactly(*saved);
    malformed_headers_and_records_are_atomic(states);
    duplicate_and_index_corruption_is_rejected(states);
    return oa::test::check_exit_status();
}
