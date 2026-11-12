// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/spatial_state/cell_classifier.hpp"
#include "oa/sim/spatial_state/movement_footprint.hpp"
#include <bit>
#include <limits>

namespace oa::sim::spatial_state {
namespace {

int32_t wrapped_add(int32_t left, int32_t right) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(left) + std::bit_cast<uint32_t>(right));
}

/// Classifies a plot rectangle for a movement class.
///
/// @param movement movement class: water depths, slope bands and occupancy tick
/// @param x rectangle origin column
/// @param z rectangle origin row
/// @param width rectangle width in plots
/// @param height rectangle depth in plots
/// @param world world holding the plots, units and sea level
/// @return 0 when the rectangle leaves the map or any plot is blocked, held by a settled ground occupant, outside the water depths or past the maximum slope; 1 when a plot passes only the maximum slope; otherwise 3
uint8_t classify_rectangle(
    const CellClassifier& movement,
    int32_t x,
    int32_t z,
    int32_t width,
    int32_t height,
    const World& world
) {
    if (x < 0 || z < 0 || wrapped_add(x, width) >= static_cast<int32_t>(world.terrain_width) ||
        wrapped_add(z, height) >= static_cast<int32_t>(world.terrain_height))
        return 0;
    uint8_t result = 3;
    for (int32_t row = 0; row < height; ++row)
        for (int32_t column = 0; column < width; ++column) {
            const auto& plot = world.plots
                                   [static_cast<std::size_t>(z + row) * world.terrain_width +
                                    static_cast<std::size_t>(x + column)];
            if (plot.blocking_feature)
                return 0;
            if (plot.ground != no_unit) {
                if (plot.ground >= world.units.size())
                    return 0;
                const auto& occupant = world.units[plot.ground];
                if (!occupant.object_present ||
                    occupant.object_tick < movement.occupancy_before_tick)
                    return 0;
            }
            if (static_cast<int32_t>(plot.low_height) <
                    static_cast<int32_t>(world.sea_level) - movement.max_water_depth ||
                static_cast<int32_t>(plot.high_height) >
                    static_cast<int32_t>(world.sea_level) - movement.min_water_depth)
                return 0;
            const auto slope = static_cast<uint8_t>(plot.high_height - plot.low_height);
            if (plot.low_height < world.sea_level) {
                if (slope > movement.bad_water_slope) {
                    if (slope > movement.max_water_slope)
                        return 0;
                    result = 1;
                }
            } else if (slope > movement.bad_land_slope) {
                if (slope > movement.max_land_slope)
                    return 0;
                result = 1;
            }
        }
    return result;
}

} // namespace

uint8_t classify_movement_footprint(
    const CellClassifier& movement, int32_t x, int32_t z, const World& world
) noexcept {
    if (x < 0 || z < 0 || movement.footprint_x <= 0 || movement.footprint_z <= 0 ||
        world.terrain_width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        world.terrain_height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        uint64_t{static_cast<uint32_t>(x)} + static_cast<uint32_t>(movement.footprint_x) >=
            world.terrain_width ||
        uint64_t{static_cast<uint32_t>(z)} + static_cast<uint32_t>(movement.footprint_z) >=
            world.terrain_height ||
        uint64_t{world.terrain_width} * world.terrain_height != world.plots.size())
        return 0;
    return classify_rectangle(movement, x, z, movement.footprint_x, movement.footprint_z, world);
}

uint8_t
classify_movement_cell(const CellClassifier& movement, int32_t x, int32_t z, const World& world) {
    if (static_cast<uint64_t>(world.terrain_width) * world.terrain_height != world.plots.size())
        return 0;
    auto result =
        classify_rectangle(movement, x, z, movement.footprint_x, movement.footprint_z, world);
    if (result <= 1)
        return result;
    if (classify_rectangle(
            movement,
            wrapped_add(x, -1),
            wrapped_add(z, -1),
            wrapped_add(movement.footprint_x, 1),
            1,
            world
        ) != 3)
        return 1;
    if (classify_rectangle(
            movement,
            wrapped_add(x, movement.footprint_x),
            wrapped_add(z, -1),
            1,
            wrapped_add(movement.footprint_z, 1),
            world
        ) != 3)
        return 1;
    if (classify_rectangle(
            movement,
            x,
            wrapped_add(z, movement.footprint_z),
            wrapped_add(movement.footprint_x, 1),
            1,
            world
        ) != 3)
        return 1;
    return classify_rectangle(
               movement, wrapped_add(x, -1), z, 1, wrapped_add(movement.footprint_z, 1), world
           ) == 3
               ? 3
               : 1;
}

uint8_t
classify_movement_plot(const CellClassifier& movement, int32_t x, int32_t z, const World& world) {
    if (x < 0 || z < 0 || x >= static_cast<int32_t>(world.terrain_width) ||
        z >= static_cast<int32_t>(world.terrain_height) ||
        static_cast<uint64_t>(world.terrain_width) * world.terrain_height != world.plots.size())
        return 0;
    const auto& plot =
        world
            .plots[static_cast<std::size_t>(z) * world.terrain_width + static_cast<std::size_t>(x)];
    if (plot.blocking_feature)
        return 0;
    if (plot.ground != no_unit) {
        if (plot.ground >= world.units.size())
            return 0;
        const auto& occupant = world.units[plot.ground];
        if (!occupant.object_present || occupant.object_tick < movement.occupancy_before_tick)
            return 0;
    }
    const auto sea = static_cast<int32_t>(world.sea_level);
    if (static_cast<int32_t>(plot.low_height) < sea - movement.max_water_depth ||
        static_cast<int32_t>(plot.high_height) > sea - movement.min_water_depth)
        return 0;
    const auto slope = static_cast<uint8_t>(plot.high_height - plot.low_height);
    if (plot.low_height < world.sea_level) {
        if (slope <= movement.bad_water_slope)
            return 3;
        return slope <= movement.max_water_slope ? 1 : 0;
    }
    if (slope <= movement.bad_land_slope)
        return 3;
    return slope <= movement.max_land_slope ? 1 : 0;
}

} // namespace oa::sim::spatial_state
