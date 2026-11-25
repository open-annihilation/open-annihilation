// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oa/core/world.h"
#include "unit_mesh_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace oa::app::unit_detail_transition {

/// The strategic square's side in framebuffer pixels.
inline constexpr float dot_pixels = 5.0F;
/// Models become useful before they are much larger than the strategic square.
inline constexpr float model_to_dot_ratio = 1.2F;
/// The short interval around the boundary that keeps easing from flickering.
inline constexpr float boundary_hysteresis = 0.025F;
/// The earlier zoom choice used when no owned model supplies a size.
inline constexpr float unknown_size_zoom = 1.0F / 6.0F;

/// Screen x and y's coefficients over world x, height and z at zoom one.
using Projection = std::array<float, 6>;

/// Population-weighted projected model diameter, before applying zoom.
struct Average {
    double pixels{};  ///< the larger projected width or height, in pixels at zoom one
    uint32_t units{}; ///< live owned units that supplied a model size
};

/// The current match's last size sample and dot/model boundary decision.
struct State {
    const oa::World* world{}; ///< the sample's canonical records
    uint32_t tick{};          ///< simulation tick sampled
    uint8_t viewpoint{};      ///< player whose known army supplies the sample
    Projection projection{};  ///< camera orientation used by the sample
    Average average{};        ///< the owned army's representative model size
    bool sampled{};           ///< the tick and camera identify a completed sample
    bool decided{};           ///< dots holds an earlier boundary decision
    bool dots{};              ///< the prior automatic representation
};

/// Sine and cosine for a unit's heading, used only for presentation sizing.
struct Heading {
    float sine{};
    float cosine{1};
};

/// Heading words in one whole turn.
inline constexpr double heading_words_per_turn = 65536.0;

/// Returns the screen-sizing coefficients for one unit heading word.
///
/// They are worked out on each call rather than kept for every heading word:
/// a kept table would hold 512 KiB for the rest of the run from the first
/// drawn match on, on every machine.
///
/// @param word unit heading, 65536 per turn
/// @return its sine and cosine, each taken in double and rounded to float;
///         no simulation calculation uses them
[[nodiscard]] inline Heading heading(uint16_t word) noexcept {
    constexpr double radians_per_word = 2 * std::numbers::pi / heading_words_per_turn;
    const double angle = word * radians_per_word;
    return {static_cast<float>(std::sin(angle)), static_cast<float>(std::cos(angle))};
}

/// Returns one type's containing model diameter projected at zoom one.
///
/// Rest-pose drawable mesh bounds supply this size. Canonical UnitDef X/Z
/// bounds describe the collision footprint and must not supply it.
///
/// @param geometry containing dimensions derived from loaded drawable polygons
/// @param projection current camera's screen coefficients at zoom one
/// @param turn unit's heading words about its height axis
/// @return projected pixels; zero for invalid bounds or projection
[[nodiscard]] inline double
model_pixels(const Geometry& geometry, const Projection& projection, uint16_t turn = 0) noexcept {
    const auto& dimensions = geometry.dimensions;
    if (std::any_of(
            dimensions.begin(),
            dimensions.end(),
            [](double value) { return !std::isfinite(value) || value < 0; }
        ) ||
        std::all_of(dimensions.begin(), dimensions.end(), [](double value) { return value == 0; }))
        return 0;
    double pixels{};
    const Heading direction = heading(turn);
    for (std::size_t row = 0; row < 2; ++row) {
        const double x = projection[row * dimensions.size()];
        const double y = projection[row * dimensions.size() + 1];
        const double z = projection[row * dimensions.size() + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return 0;
        // Model z faces opposite world z. Apply the unit's turn before
        // finding its extent along the camera's screen axes.
        const double across = x * direction.cosine - z * direction.sine;
        const double along = -x * direction.sine - z * direction.cosine;
        const double extent = std::abs(across) * dimensions[0] + std::abs(y) * dimensions[1] +
                              std::abs(along) * dimensions[2];
        pixels = std::max(pixels, extent);
    }
    return std::isfinite(pixels) ? pixels : 0;
}

/// Finds a representative physical size without exposing hidden enemy types.
///
/// Each owned ordinary mobile unit contributes once, so a rare large type
/// does not carry the same weight as an army of small ones. Buildings and
/// commanders supply a fallback only when no ordinary mobile unit is present.
///
/// @param world canonical units; their type bounds are not used for geometry
/// @param projection current camera's screen coefficients at zoom one
/// @param geometry drawable rest-mesh dimensions, indexed by unit type index
/// @return the larger projected width or height averaged over eligible units
[[nodiscard]] inline Average average_model_pixels(
    const oa::World& world, const Projection& projection, std::span<const Geometry> geometry
) noexcept {
    Average ordinary{}, fallback{};
    if (world.units == nullptr || world.unit_defs == nullptr)
        return {};
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        if (!oa::unit_is_live_target(unit.flags) || unit.health <= 0 ||
            unit.owner_index != world.game.viewpoint_player || unit.build_remaining != 0)
            continue;
        const auto* type = oa::world_unit_def_of(&world, &unit);
        if (type == nullptr || unit.type_index >= geometry.size())
            continue;
        const double pixels = model_pixels(geometry[unit.type_index], projection, unit.heading);
        if (!(pixels > 0))
            continue;
        fallback.pixels += pixels;
        ++fallback.units;
        if ((unit.flags & OA_UNIT_FLAG_BUILDING) != 0 ||
            (type->abilities & OA_UNIT_DEF_ABILITY_COMMANDER) != 0)
            continue;
        ordinary.pixels += pixels;
        ++ordinary.units;
    }
    auto result = ordinary.units != 0 ? ordinary : fallback;
    if (result.units != 0)
        result.pixels /= result.units;
    return result;
}

/// Chooses dots below the physical model's useful screen size.
///
/// The initial boundary is a model 20% larger than its five-pixel dot. A
/// 2.5% interval on each side retains the prior choice during a small zoom
/// reversal or population change. Fixed zoom choices do not use this state.
///
/// @param[in,out] state the match's previous automatic choice and cached sample
/// @param world canonical records, read without changing them
/// @param tick current simulation tick
/// @param projection camera coefficients at zoom one
/// @param zoom current framebuffer pixels per map pixel
/// @param geometry_provider supplies mesh dimensions only when the sample changes
/// @return true for dots; false for physical units
template <class GeometryProvider>
[[nodiscard]] inline bool as_dots_with_geometry(
    State& state,
    const oa::World& world,
    uint32_t tick,
    const Projection& projection,
    float zoom,
    GeometryProvider&& geometry_provider
) {
    if (!std::isfinite(zoom) || !(zoom > 0))
        return false;
    const bool same_world = state.world == &world;
    if (!same_world || !state.sampled || state.tick != tick || state.projection != projection ||
        state.viewpoint != world.game.viewpoint_player) {
        if (!same_world)
            state.decided = false;
        state.world = &world;
        state.tick = tick;
        state.viewpoint = world.game.viewpoint_player;
        state.projection = projection;
        state.average = average_model_pixels(world, projection, geometry_provider());
        state.sampled = true;
    }
    if (state.average.units == 0) {
        state.decided = false;
        return zoom < unknown_size_zoom;
    }
    const double pixels = state.average.pixels * zoom;
    double boundary = dot_pixels * model_to_dot_ratio;
    if (state.decided)
        boundary *= state.dots ? 1 + boundary_hysteresis : 1 - boundary_hysteresis;
    state.dots = pixels < boundary;
    state.decided = true;
    return state.dots;
}

/// Applies the automatic boundary using an already available geometry span.
/// @param[in,out] state previous representation and size sample
/// @param world canonical records, read without changing them
/// @param tick current simulation tick
/// @param projection camera coefficients at zoom one
/// @param zoom current framebuffer pixels per map pixel
/// @param geometry drawable rest-mesh dimensions, indexed by type
/// @return true for dots; false for physical units
[[nodiscard]] inline bool as_dots(
    State& state,
    const oa::World& world,
    uint32_t tick,
    const Projection& projection,
    float zoom,
    std::span<const Geometry> geometry
) noexcept {
    return as_dots_with_geometry(state, world, tick, projection, zoom, [geometry] {
        return geometry;
    });
}

} // namespace oa::app::unit_detail_transition
