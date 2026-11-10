// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace oa::app::renderer_live_scene {

/// Exact inputs of indices over one retained, possibly foundation-cut ground.
struct GroundIndexKey {
    uint64_t revision{};
    std::size_t vertices{};
    std::array<uint64_t, 4> card_axes{};
    std::array<uint64_t, 3> float_control{};
    bool card{};

    /// Compares every scalar source, including floating-point bit patterns.
    /// @param other the other index's source
    /// @return whether both indices are built from the same retained geometry
    [[nodiscard]] bool operator==(const GroundIndexKey& other) const = default;
};

/// Reasons a requested ground index cannot reuse the retained index.
enum GroundIndexMiss : uint32_t {
    index_invalid = 1,
    index_uncached = 2,
    index_revision = 4,
    index_vertices = 8,
    index_card = 16,
    index_axes = 32,
    index_float_control = 64
};

/// One build attempt, recorded only while benchmark timers are enabled.
struct GroundIndexBuild {
    GroundIndexKey before{}, requested{};
    uint64_t previous_attempts{};
    uint32_t reasons{};
    bool was_valid{}, completed{};
    uint64_t bounds_nanoseconds{}, world_nanoseconds{}, card_nanoseconds{}, total_nanoseconds{};
};

/// Bounded build records; excess attempts are counted rather than allocated.
struct GroundIndexBuilds {
    static constexpr std::size_t capacity = 8;
    std::array<GroundIndexBuild, capacity> records{};
    std::size_t size{};
    uint64_t dropped{};
};

} // namespace oa::app::renderer_live_scene
