// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Region labels over a movement map. The eight neighbour steps are the path
// search's neighbour order. This file does not run that search.
#include "reach.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace oa::sim::brains {
namespace {

constexpr int8_t step_x[8] = {0, -1, -1, -1, 0, 1, 1, 1};
constexpr int8_t step_z[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
constexpr uint32_t shift_half = 524288u;

uint32_t arithmetic_shift20(uint32_t value) noexcept {
    const uint32_t high = value >> 20;
    return (value & 0x80000000u) != 0u ? high | 0xfffff000u : high;
}

bool map_pass(void* context, uint32_t x, uint32_t z) {
    const auto* grid = static_cast<const sim::ground_orders::MovementMap*>(context);
    return grid != nullptr && grid->cell(x, z) >= 1;
}

const ClassRegions* find_slot(const RegionCache& cache, uint32_t handle) noexcept {
    if (handle == 0)
        return nullptr;
    for (std::size_t slot = 0; slot < region_slots; ++slot) {
        if (cache.slot[slot].handle == handle)
            return &cache.slot[slot];
    }
    return nullptr;
}

ClassRegions* find_slot(RegionCache& cache, uint32_t handle) noexcept {
    return const_cast<ClassRegions*>(find_slot(static_cast<const RegionCache&>(cache), handle));
}

bool same_grid(const ClassRegions& regions, uint32_t width, uint32_t height) noexcept {
    return regions.width == width && regions.height == height && regions.labels != nullptr;
}

void fill_regions(
    ClassRegions& regions,
    uint32_t* queue,
    const RegionBuild& build,
    uint32_t width,
    uint32_t height
) {
    const uint32_t cell_count = width * height;
    for (uint32_t cell = 0; cell < cell_count; ++cell)
        regions.labels[cell] = 0;
    CellPass pass = build.pass != nullptr ? build.pass : map_pass;
    void* context = build.pass != nullptr ? build.context
                                          : const_cast<sim::ground_orders::MovementMap*>(build.map);
    uint32_t next_label = 1;
    uint32_t head = 0;
    uint32_t tail = 0;
    for (uint32_t z = 0; z < height; ++z) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t start = z * width + x;
            if (regions.labels[start] != 0 || !pass(context, x, z))
                continue;
            const uint32_t label = next_label;
            ++next_label;
            regions.labels[start] = label;
            queue[tail] = start;
            ++tail;
            while (head < tail) {
                const uint32_t packed = queue[head];
                ++head;
                const uint32_t column = packed % width;
                const uint32_t row = packed / width;
                for (int direction = 0; direction < 8; ++direction) {
                    const int32_t next_x = static_cast<int32_t>(column) + step_x[direction];
                    const int32_t next_z = static_cast<int32_t>(row) + step_z[direction];
                    if (next_x < 0 || next_z < 0 || static_cast<uint32_t>(next_x) >= width ||
                        static_cast<uint32_t>(next_z) >= height)
                        continue;
                    const auto neighbour =
                        static_cast<uint32_t>(next_z) * width + static_cast<uint32_t>(next_x);
                    if (regions.labels[neighbour] != 0 ||
                        !pass(
                            context, static_cast<uint32_t>(next_x), static_cast<uint32_t>(next_z)
                        ))
                        continue;
                    regions.labels[neighbour] = label;
                    queue[tail] = neighbour;
                    ++tail;
                }
            }
        }
    }
}

bool ensure_queue(RegionCache& cache, uint32_t cell_count) {
    if (cache.queue_cells >= cell_count && cache.queue != nullptr)
        return true;
    auto* memory = new (std::nothrow) uint32_t[cell_count]();
    if (memory == nullptr)
        return false;
    cache.queue.reset(memory);
    cache.queue_cells = cell_count;
    return true;
}

} // namespace

uint64_t ensure_regions(RegionCache& cache, const RegionBuild& build) {
    if (build.map == nullptr || build.handle == 0)
        return 0;
    const uint32_t width = build.map->width();
    const uint32_t height = build.map->height();
    const uint64_t cells = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (width == 0 || height == 0 || cells > static_cast<uint64_t>(UINT32_MAX))
        return 0;
    const auto cell_count = static_cast<uint32_t>(cells);

    ClassRegions* regions = find_slot(cache, build.handle);
    if (regions != nullptr && regions->tick == build.tick && same_grid(*regions, width, height))
        return 0;
    if (regions == nullptr) {
        for (std::size_t slot = 0; slot < region_slots; ++slot) {
            if (cache.slot[slot].handle == 0) {
                regions = &cache.slot[slot];
                break;
            }
        }
    }
    if (regions == nullptr) {
        std::size_t victim = 0;
        for (std::size_t slot = 1; slot < region_slots; ++slot) {
            const ClassRegions& older = cache.slot[victim];
            const ClassRegions& candidate = cache.slot[slot];
            if (candidate.tick < older.tick ||
                (candidate.tick == older.tick && candidate.handle < older.handle))
                victim = slot;
        }
        regions = &cache.slot[victim];
        regions->handle = 0;
        regions->labels.reset();
    }
    if (!same_grid(*regions, width, height)) {
        auto* labels = new (std::nothrow) uint32_t[cell_count]();
        if (labels == nullptr)
            return 0;
        regions->labels.reset(labels);
        regions->width = width;
        regions->height = height;
    }
    if (!ensure_queue(cache, cell_count) || regions->labels == nullptr)
        return 0;
    regions->handle = build.handle;
    regions->tick = build.tick;
    fill_regions(*regions, cache.queue.get(), build, width, height);
    return (cells + 15u) / 16u;
}

bool regions_meet(
    const RegionCache& cache, uint32_t handle, int32_t ax, int32_t az, int32_t bx, int32_t bz
) noexcept {
    const ClassRegions* regions = find_slot(cache, handle);
    if (regions == nullptr || regions->labels == nullptr || regions->width == 0 ||
        regions->height == 0)
        return false;
    uint32_t left[9]{};
    uint32_t left_count = 0;
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            const int32_t x = ax + dx;
            const int32_t z = az + dz;
            if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= regions->width ||
                static_cast<uint32_t>(z) >= regions->height)
                continue;
            const uint32_t label =
                regions
                    ->labels[static_cast<uint32_t>(z) * regions->width + static_cast<uint32_t>(x)];
            if (label == 0 || left_count >= 9)
                continue;
            left[left_count] = label;
            ++left_count;
        }
    }
    if (left_count == 0)
        return false;
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            const int32_t x = bx + dx;
            const int32_t z = bz + dz;
            if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= regions->width ||
                static_cast<uint32_t>(z) >= regions->height)
                continue;
            const uint32_t label =
                regions
                    ->labels[static_cast<uint32_t>(z) * regions->width + static_cast<uint32_t>(x)];
            if (label == 0)
                continue;
            for (uint32_t held = 0; held < left_count; ++held) {
                if (left[held] == label)
                    return true;
            }
        }
    }
    return false;
}

const ClassRegions* regions_for(const RegionCache& cache, uint32_t handle) noexcept {
    return find_slot(cache, handle);
}

uint32_t region_label(const RegionCache& cache, uint32_t handle, uint32_t x, uint32_t z) noexcept {
    const ClassRegions* regions = find_slot(cache, handle);
    if (regions == nullptr || regions->labels == nullptr || x >= regions->width ||
        z >= regions->height)
        return 0;
    return regions->labels[z * regions->width + x];
}

uint32_t region_queue_cells(const RegionCache& cache) noexcept {
    return cache.queue_cells;
}

void anchor_cell(
    int32_t x, int32_t z, int16_t footprint_x, int16_t footprint_z, int32_t* cx, int32_t* cz
) noexcept {
    if (cx == nullptr || cz == nullptr)
        return;
    const auto shifted_x = arithmetic_shift20(
        static_cast<uint32_t>(x) -
        static_cast<uint32_t>(static_cast<int32_t>(footprint_x)) * shift_half + shift_half
    );
    const auto shifted_z = arithmetic_shift20(
        static_cast<uint32_t>(z) -
        static_cast<uint32_t>(static_cast<int32_t>(footprint_z)) * shift_half + shift_half
    );
    *cx = static_cast<int32_t>(shifted_x);
    *cz = static_cast<int32_t>(shifted_z);
}

} // namespace oa::sim::brains
