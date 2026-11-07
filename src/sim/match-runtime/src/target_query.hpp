// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/world.h"
#include "oa/sim/spatial_state/spatial.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::match_runtime {

/// Holds ascending sightings retained by the spatial target query.
struct NearbyTargetSightings {
    std::span<const uint16_t> seen;
    std::span<const uint16_t> radar;
    size_t candidates{};
    bool complete{};
};

/// Retains sightings in nearby buckets without resolving distant units.
///
/// Reads canonical bucket links and includes the outside bucket and every
/// supplied unbucketed unit. The caller keeps those extra slots current.
/// Sightings must be ascending and unique, as the knowledge refresh writes
/// them. Scratch holds two entries per world slot. Failure requests the full
/// sightings walk, including for ranges whose signed square wraps or queries
/// too broad to benefit from sorting a bounded set of nearby slots.
///
/// @param world the canonical unit pool
/// @param spatial the current bucket heads
/// @param extras live carried or otherwise unbucketed slots
/// @param seen ascending visible slots
/// @param radar ascending radar slots
/// @param center query position in signed 16.16 world coordinates
/// @param radius query radius in whole world units
/// @param[out] scratch storage for the retained seen and radar slots
/// @return the retained sightings, or an incomplete result requiring a full walk
/// @quirk Fractional distances are rounded down by the sightings walk, so
///        the bucket rectangle extends one whole world unit beyond the range.
inline NearbyTargetSightings nearby_target_sightings(
    const oa::World& world,
    const sim::spatial_state::World& spatial,
    std::span<const uint16_t> extras,
    std::span<const uint16_t> seen,
    std::span<const uint16_t> radar,
    const std::array<uint32_t, 3>& center,
    int32_t radius,
    std::span<uint16_t> scratch
) noexcept {
    constexpr int32_t largest_signed_square_root = 46340;
    constexpr int32_t fixed_to_bucket_shift = 23;
    constexpr int64_t fixed_one = int64_t{1} << 16;
    constexpr size_t minimum_candidate_limit = 64;
    constexpr size_t maximum_candidate_limit = 1024;
    const size_t capacity = world.unit_slot_count;
    const int32_t center_x = std::bit_cast<int32_t>(center[0]);
    const int32_t center_z = std::bit_cast<int32_t>(center[2]);
    if (radius < 0 || radius > largest_signed_square_root || center_x < 0 || center_z < 0 ||
        capacity == 0 || scratch.size() / 2 < capacity || spatial.bucket_width == 0 ||
        spatial.bucket_height == 0 ||
        static_cast<uint64_t>(spatial.bucket_width) * spatial.bucket_height !=
            spatial.buckets.size())
        return {};

    const size_t known_count = std::max(seen.size(), radar.size());
    if (known_count == 0)
        return {{}, {}, 0, true};
    // Local buckets include allies too. Limit sorting when their population
    // approaches the sightings count, and keep a fixed cap for dense battles.
    const size_t candidate_limit =
        std::clamp(known_count / 4, minimum_candidate_limit, maximum_candidate_limit);
    size_t candidate_count = 0;
    const auto append = [&](uint32_t slot) {
        if (slot == 0 || slot >= capacity || candidate_count == capacity ||
            candidate_count == candidate_limit)
            return false;
        scratch[candidate_count++] = static_cast<uint16_t>(slot);
        return true;
    };
    const auto append_chain = [&](uint32_t slot) {
        size_t remaining = capacity;
        while (slot != 0) {
            if (remaining-- == 0 || !append(slot))
                return false;
            slot = oa::oa_unit_slot_from_ref(world.units[slot].attach_next);
        }
        return true;
    };
    const int64_t reach = (static_cast<int64_t>(radius) + 1) * fixed_one;
    const auto bucket_edge = [](int64_t coordinate, uint32_t count) {
        const int64_t bucket = coordinate >> fixed_to_bucket_shift;
        return static_cast<uint32_t>(std::clamp(bucket, int64_t{0}, int64_t{count - 1}));
    };
    const uint32_t first_x =
        bucket_edge(static_cast<int64_t>(center_x) - reach, spatial.bucket_width);
    const uint32_t last_x =
        bucket_edge(static_cast<int64_t>(center_x) + reach, spatial.bucket_width);
    const uint32_t first_z =
        bucket_edge(static_cast<int64_t>(center_z) - reach, spatial.bucket_height);
    const uint32_t last_z =
        bucket_edge(static_cast<int64_t>(center_z) + reach, spatial.bucket_height);
    const uint64_t query_buckets =
        static_cast<uint64_t>(last_x - first_x + 1) * (last_z - first_z + 1);
    const uint64_t all_buckets = spatial.buckets.size();
    if (query_buckets >= all_buckets - all_buckets / 4)
        return {};
    for (uint32_t z = first_z; z <= last_z; ++z)
        for (uint32_t x = first_x; x <= last_x; ++x)
            if (!append_chain(
                    spatial.buckets[static_cast<size_t>(z) * spatial.bucket_width + x].head
                ))
                return {};
    if (!append_chain(spatial.outside_bucket.head))
        return {};
    for (const uint16_t slot : extras)
        if (!append(slot))
            return {};

    auto candidates = scratch.first(candidate_count);
    std::sort(candidates.begin(), candidates.end());
    candidate_count =
        static_cast<size_t>(std::unique(candidates.begin(), candidates.end()) - candidates.begin());
    size_t seen_count = 0;
    size_t radar_count = 0;
    for (size_t index = 0; index < candidate_count; ++index) {
        const uint16_t slot = scratch[index];
        if (std::binary_search(radar.begin(), radar.end(), slot))
            scratch[capacity + radar_count++] = slot;
        if (std::binary_search(seen.begin(), seen.end(), slot))
            scratch[seen_count++] = slot;
    }
    return {
        scratch.first(seen_count), scratch.subspan(capacity, radar_count), candidate_count, true
    };
}

} // namespace oa::sim::match_runtime
