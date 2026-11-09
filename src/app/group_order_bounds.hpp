// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/types.h"
#include "oa/core/unit.h"
#include <cstdint>

namespace oa::app {

namespace group_order_detail {

inline constexpr int64_t fixed_pixel = 65536;
inline constexpr int64_t cell_pixels = 16;
inline constexpr int64_t half_cell_pixels = cell_pixels / 2;
inline constexpr uint32_t ground_layer = 1;

/// Checks whether a goal origin and its footprint fit the playable map axis.
/// @param point goal centre coordinate in 16.16 pixels
/// @param footprint occupied plot count on the axis
/// @param extent playable map width or height in pixels
/// @return whether the full goal footprint is inside the playable map
inline bool axis_fits(int32_t point, int16_t footprint, int32_t extent) noexcept {
    if (footprint <= 0 || extent <= 0)
        return false;
    const int64_t last_origin = int64_t(extent) / cell_pixels - footprint;
    if (last_origin < 0)
        return false;
    const int64_t biased = int64_t(point) - int64_t(footprint - 1) * half_cell_pixels * fixed_pixel;
    return biased >= 0 && biased < (last_origin + 1) * cell_pixels * fixed_pixel;
}

} // namespace group_order_detail

/// Uses the original click when an enhanced ground formation leaves the map.
/// The playable map excludes padded terrain cells. This checks goal bounds only;
/// normal navigation still checks terrain and other units. The caller restricts
/// this policy to ground orders in the battle-flow rules preset.
/// @param formation translated formation point in 16.16 pixels
/// @param click original group command point in 16.16 pixels
/// @param actor canonical unit whose footprint receives the order
/// @param map_width playable map width in pixels
/// @param map_height playable map height in pixels
/// @param enabled whether the enhanced ground-order policy applies
/// @return the formation point, or the original valid click for an invalid point
inline oa::FixedVec3 bounded_group_order_point(
    const oa::FixedVec3& formation,
    const oa::FixedVec3& click,
    const oa::Unit& actor,
    int32_t map_width,
    int32_t map_height,
    bool enabled
) noexcept {
    if (!enabled ||
        (actor.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != group_order_detail::ground_layer ||
        actor.attach_parent != 0)
        return formation;
    const auto fits = [&](const oa::FixedVec3& point) noexcept {
        return group_order_detail::axis_fits(point.x, actor.footprint_x, map_width) &&
               group_order_detail::axis_fits(point.z, actor.footprint_z, map_height);
    };
    return !fits(formation) && fits(click) ? click : formation;
}

} // namespace oa::app
