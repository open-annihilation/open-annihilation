// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "runtime_full.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/visibility_state.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::app::renderer_live_sight {

/// Separates undiscovered ground from mapped ground outside current sight.
enum class Fog : uint8_t { seen, grey, black };

/// Reads mapping before line of sight, so unexplored cells stay black.
/// @param sight the current mapping, player coverage and allied sight policy
/// @param x the east-west fog-sampling coordinate, in world pixels
/// @param z the north-south fog-sampling coordinate, in world pixels
/// @return seen, grey or black under the policy; cells absent from enabled mapping stay black
inline Fog fog_at(const full::SightView& sight, int32_t x, int32_t z) {
    if (!sight.mapping && !sight.line_of_sight)
        return Fog::seen;
    constexpr int32_t fog_edge = present::world_renderer::fog_cell_pixels;
    const int32_t column = x / fog_edge;
    const int32_t row = z / fog_edge;
    if (x < 0 || z < 0 || column >= sight.width || row >= sight.height)
        return sight.mapping ? Fog::black : Fog::seen;
    const std::size_t index = static_cast<std::size_t>(row) * sight.width + column;
    if (sight.mapping &&
        (index >= sight.player_bits.size() || (sight.player_bits[index] & sight.mapped_bits) == 0))
        return Fog::black;
    const present::world_renderer::FogOptions options{
        sight.line_of_sight, sight.mapping, sight.shared_bits
    };
    const bool visible = present::world_renderer::cell_shown_in_sight(
        index < sight.coverage.size() ? sight.coverage[index] : uint16_t{0},
        index < sight.player_bits.size() ? sight.player_bits[index] : uint16_t{0},
        static_cast<uint16_t>(sight.mapped_bits & ~sight.shared_bits),
        options
    );
    return present::world_renderer::view_greys(options) && !visible ? Fog::grey : Fog::seen;
}

} // namespace oa::app::renderer_live_sight
