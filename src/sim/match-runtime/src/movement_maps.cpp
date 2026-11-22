// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"
#include "enhanced_state.hpp"

#include "oa/data/enhanced_mode.hpp"

#include <array>
#include <cstdint>

namespace oa::sim::match_runtime {
namespace {
sim::ground_orders::OccupancyRectangle
rectangle_of(std::array<int16_t, 2> cell, std::array<int16_t, 2> footprint) {
    const auto pack = [](std::array<int16_t, 2> pair) {
        return uint32_t{static_cast<uint16_t>(pair[0])} | uint32_t{static_cast<uint16_t>(pair[1])}
                                                              << 16;
    };
    return {pack(cell), pack(footprint)};
}

// Whether refreshing a unit's rectangle reaches only anchors whose footprint
// and one-cell rim end at least one cell before the map's far edges. The
// whole-map build and a refresh classify such anchors alike, so when moving
// units do not block planning, a moving unit's refresh there rewrites what
// the map holds. Anchors nearer the far edges keep their refreshes: the
// build pads past the edge, where a refresh reads the last row and column
// as off the map. Match::prepare_search_job tests its feed the same way.
bool refresh_stays_inside(
    const sim::ground_orders::MovementMap& map,
    std::array<int16_t, 2> cell,
    std::array<int16_t, 2> footprint
) {
    // The refresh reaches anchors up to cell + footprint; each needs its
    // footprint, its rim and one cell more inside the map.
    constexpr int64_t edge_band = 2;
    return cell[0] >= 0 && cell[1] >= 0 && footprint[0] > 0 && footprint[1] > 0 &&
           map.footprint_x() > 0 && map.footprint_z() > 0 &&
           int64_t{cell[0]} + footprint[0] + map.footprint_x() + edge_band <= map.width() &&
           int64_t{cell[1]} + footprint[1] + map.footprint_z() + edge_band <= map.height();
}
} // namespace

sim::spatial_state::CellClassifier Match::MovementClassMap::classifier() const {
    auto movement = record;
    // Following-flow planning keeps movement objects out of its cached
    // topology. Actual ground collision reads current footprint claims.
    movement.occupancy_before_tick =
        match->enhanced_features_.has(data::enhanced_mode::Feature::occupancy_free_planning)
            ? 0
            : map->projection_tick();
    return movement;
}

uint8_t Match::MovementClassMap::classify_cell(int32_t x, int32_t z) {
    return sim::spatial_state::classify_movement_cell(classifier(), x, z, match->spatial_);
}

uint8_t Match::MovementClassMap::classify_plot(int32_t x, int32_t z) {
    return sim::spatial_state::classify_movement_plot(classifier(), x, z, match->spatial_);
}

void Match::build_movement_maps() {
    // The class record comes from the first type naming the handle; every
    // type of a class carries the class's own footprint and limits.
    std::size_t count = 0;
    for (const auto& type : input_.fields) {
        if (!type.movement_class || *type.movement_class == 0 || !type.runtime_metadata ||
            movement_class_map(*type.movement_class))
            continue;
        if (count == movement_classes_.size()) {
            fault_.note("more movement classes than the class table holds");
            break;
        }
        const auto& metadata = *type.runtime_metadata;
        auto& klass = movement_classes_[count++];
        klass.match = this;
        klass.handle = *type.movement_class;
        klass.record.footprint_x = metadata.footprint_x;
        klass.record.footprint_z = metadata.footprint_z;
        klass.record.max_water_depth = metadata.max_water_depth;
        klass.record.min_water_depth = metadata.min_water_depth;
        klass.record.max_land_slope = metadata.max_slope;
        klass.record.bad_land_slope = metadata.bad_slope;
        klass.record.max_water_slope = metadata.max_water_slope;
        klass.record.bad_water_slope = metadata.bad_water_slope;
        klass.map.emplace(
            spatial_.terrain_width,
            spatial_.terrain_height,
            metadata.footprint_x,
            metadata.footprint_z,
            klass
        );
        klass.map->rebuild();
    }
}

void Match::refresh_movement_maps(std::array<int16_t, 2> cell, std::array<int16_t, 2> footprint) {
    advisory_static_change(cell, footprint);
    const auto rectangle = rectangle_of(cell, footprint);
    if (enhanced_ && enhanced_->shared_routes)
        ++enhanced_->shared_static_revision;
    for (auto& klass : movement_classes_)
        if (klass.map)
            klass.map->refresh(rectangle);
}

Match::MovementClassMap* Match::movement_class_map(sim::unit_spawn::AssetHandle movement_class) {
    for (auto& klass : movement_classes_)
        if (klass.map && klass.handle == movement_class)
            return &klass;
    return nullptr;
}

const sim::ground_orders::MovementMap*
Match::movement_map(sim::unit_spawn::AssetHandle movement_class) const {
    for (const auto& klass : movement_classes_)
        if (klass.map && klass.handle == movement_class)
            return &*klass.map;
    return nullptr;
}

void Match::MapListeners::refresh_plot_height_range(
    std::array<int16_t, 2> origin_minus_one, std::array<int16_t, 2> footprint_plus_two
) {
    match.services_.refresh_plot_height_range(origin_minus_one, footprint_plus_two);
}

void Match::MapListeners::notify_footprint_changed(
    std::array<int16_t, 2> cell, std::array<int16_t, 2> footprint
) {
    match.refresh_movement_maps(cell, footprint);
    match.services_.notify_footprint_changed(cell, footprint);
}

void Match::MapListeners::notify_object_footprint_removed(
    sim::spatial_state::Unit& unit, uint32_t old_tick
) {
    // Planning that passes through moving units reads a moving unit's
    // cells the same before and after it leaves them, so only refreshes
    // that reach the far edges are kept.
    const bool skip_inside =
        match.enhanced_features_.has(data::enhanced_mode::Feature::static_interior_refresh_skip);
    const auto rectangle = rectangle_of(unit.cell, unit.footprint);
    for (auto& klass : match.movement_classes_)
        if (klass.map) {
            // release_unit refreshes nothing for a unit whose change the
            // projection has not reached; that test is the cheaper one.
            if (skip_inside && (old_tick >= klass.map->projection_tick() ||
                                refresh_stays_inside(*klass.map, unit.cell, unit.footprint)))
                continue;
            klass.map->release_unit(rectangle, old_tick);
        }
    match.services_.notify_object_footprint_removed(unit, old_tick);
}

} // namespace oa::sim::match_runtime
