// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {

namespace {
// Yard map cell bits the building-site test reads (the FBI yardmap letters
// combine them).
constexpr uint8_t yard_claims_plot = 0x01; // placed, marks plot_claimed
constexpr uint8_t yard_refuses_units = 0x06;
constexpr uint8_t yard_level = 0x08;     // bounds slope and water depth
constexpr uint8_t yard_submerged = 0x10; // plot stays below the building
constexpr uint8_t yard_refuses_features = 0x20;
constexpr uint8_t yard_refuses_indestructible = 0x40;
constexpr uint8_t yard_geothermal = 0x80;
// Stands in, as an 'o' cell, where a yard map is shorter than the footprint.
constexpr uint8_t yard_default_cell = 0x2f;
} // namespace

std::optional<uint8_t> Match::building_site(
    uint16_t type,
    int32_t cell_x,
    int32_t cell_z,
    uint16_t skip_unit,
    uint8_t placing_player,
    const BuildSiteOptions& options
) const {
    if (type == 0 || type >= world_.types.size())
        return std::nullopt;
    // Facing east or west swaps the footprint's width and depth.
    const auto facing = build_facing(type, options.facing);
    const bool turned = (facing & 1U) != 0;
    const auto fx = static_cast<int32_t>(
        turned ? world_.types[type].footprint_z : world_.types[type].footprint_x
    );
    const auto fz = static_cast<int32_t>(
        turned ? world_.types[type].footprint_x : world_.types[type].footprint_z
    );
    const auto width = static_cast<int32_t>(spatial_.terrain_width);
    const auto height = static_cast<int32_t>(spatial_.terrain_height);
    // Packed cell_x < 1, cell_z < 1, and a one-cell far border.
    if (cell_x < 1 || cell_z < 1 || width <= cell_x + fx || height <= cell_z + fz)
        return std::nullopt;
    bool in_sight = true;
    // orders.build-site-kickout place-over-own-units: the placing player's
    // own units with a movement object leave the build cursor's site open.
    const bool own_units_pass = placing_player != no_placing_player &&
                                rules().orders.build_site_kickout.place_over_own_units;
    const auto placer_moves_off = [&](uint16_t slot) {
        const Unit* unit = world_unit_at(&state(), slot);
        return unit != nullptr && unit->owner_index == placing_player &&
               ground_runtime(slot) != nullptr;
    };
    if (placing_player != no_placing_player) {
        const auto corner = static_cast<std::size_t>(cell_z) * input_.map.attribute_width +
                            static_cast<std::size_t>(cell_x);
        const uint32_t corner_height =
            corner < input_.map.attributes.size() ? input_.map.attributes[corner].height : 0u;
        const std::array<uint32_t, 3> centre{
            static_cast<uint32_t>((cell_x * 2 + fx) * 0x80000),
            corner_height << 16,
            static_cast<uint32_t>((cell_z * 2 + fz) * 0x80000)
        };
        if (!point_mapped(centre))
            return std::nullopt;
        in_sight = point_visible(placing_player, centre);
    }
    std::span<const uint8_t> yard;
    uint8_t max_slope = 255;
    int16_t max_water_depth = 10000, min_water_depth = -10000;
    int8_t waterline = 0;
    if (type < input_.fields.size()) {
        yard = build_yard(type, facing);
        if (const auto* meta = input_.fields[type].runtime_metadata) {
            max_slope = meta->max_slope;
            max_water_depth = meta->max_water_depth;
            min_water_depth = meta->min_water_depth;
        }
        if (const auto* definition = input_.fields[type].definition)
            waterline = definition->waterline;
    }
    uint8_t min_low = 0xff, max_high = 0, submerged_high = 0;
    bool needs_geothermal = false, found_geothermal = false;
    for (int32_t row = 0; row < fz; ++row) {
        for (int32_t column = 0; column < fx; ++column) {
            const auto& plot =
                spatial_.plots
                    [static_cast<std::size_t>(cell_z + row) * spatial_.terrain_width +
                     static_cast<std::size_t>(cell_x + column)];
            const auto cell = static_cast<std::size_t>(row) * static_cast<std::size_t>(fx) +
                              static_cast<std::size_t>(column);
            const uint8_t mask = cell < yard.size() ? yard[cell] : yard_default_cell;
            if ((mask & yard_level) != 0) {
                min_low = std::min(min_low, plot.low_height);
                max_high = std::max(max_high, plot.high_height);
            }
            if ((mask & yard_submerged) != 0)
                submerged_high = std::max(submerged_high, plot.high_height);
            if ((mask & yard_claims_plot) != 0 &&
                (plot.flags & sim::spatial_state::plot_claimed) != 0 && in_sight)
                return std::nullopt;
            // The placer's own units that move off let the site through and
            // report it as over them, whatever slot they hold and whether the
            // placer sees them; any other unit refuses it unless it holds
            // skip_unit's slot or stands out of the placer's sight.
            if ((mask & yard_refuses_units) != 0 && plot.ground != sim::spatial_state::no_unit &&
                !own_mobile_unit(plot.ground, options.own_units_player)) {
                if (own_units_pass && placer_moves_off(plot.ground)) {
                    if (options.over_own_units != nullptr)
                        *options.over_own_units = true;
                } else if (plot.ground != skip_unit && in_sight) {
                    return std::nullopt;
                }
            }
            if ((mask & yard_refuses_features) != 0 && plot.blocking_feature)
                return std::nullopt;
            if ((mask & yard_refuses_indestructible) != 0 && plot.indestructible_feature)
                return std::nullopt;
            if ((mask & yard_geothermal) != 0) {
                needs_geothermal = true;
                if (plot.geo_feature)
                    found_geothermal = true;
            }
        }
    }
    if (needs_geothermal && !found_geothermal)
        return std::nullopt;
    const auto sea = static_cast<int32_t>(spatial_.sea_level);
    uint8_t base = min_low;
    if (min_low <= max_high) {
        if (static_cast<int32_t>(max_slope) < max_high - min_low)
            return std::nullopt;
    } else {
        max_high = 0;
        base = static_cast<uint8_t>(sea - waterline);
    }
    if (base < submerged_high)
        return std::nullopt;
    if (static_cast<int32_t>(min_low) < sea - static_cast<int32_t>(max_water_depth))
        return std::nullopt;
    max_high = std::max(max_high, submerged_high);
    if (sea - static_cast<int32_t>(min_water_depth) < static_cast<int32_t>(max_high))
        return std::nullopt;
    return base;
}

uint8_t
Match::footprint_height(uint16_t type, int32_t cell_x, int32_t cell_z, uint8_t facing) const {
    if (type == 0 || type >= world_.types.size())
        return 0;
    std::span<const uint8_t> yard;
    int8_t waterline = 0;
    const auto turn = build_facing(type, facing);
    if (type < input_.fields.size()) {
        yard = build_yard(type, turn);
        if (const auto* definition = input_.fields[type].definition)
            waterline = definition->waterline;
    }
    const bool turned = (turn & 1U) != 0;
    return sim::spatial_state::footprint_build_height(
        turned ? world_.types[type].footprint_z : world_.types[type].footprint_x,
        turned ? world_.types[type].footprint_x : world_.types[type].footprint_z,
        yard,
        waterline,
        cell_x,
        cell_z,
        spatial_
    );
}

void Match::snap_to_build_grid(uint16_t type, FixedVec3& position) const {
    const auto& world = state();
    if (type == 0 || type >= world.unit_def_count || world.unit_defs[type].bm_code != 0)
        return;
    const auto& def = world.unit_defs[type];
    const auto cell_x = footprint_origin_cell(position.x, def.footprint_x);
    const auto cell_z = footprint_origin_cell(position.z, def.footprint_z);
    sim::ground_orders::Point site{position.x, position.y, position.z};
    snap_build_position(site, def.footprint_x, def.footprint_z);
    position.x = site[0];
    position.y =
        static_cast<int32_t>(static_cast<uint32_t>(footprint_height(type, cell_x, cell_z)) << 16);
    position.z = site[2];
}

uint16_t Match::feature_word_under(
    const sim::ground_orders::Point& point, int16_t* origin_x, int16_t* origin_z
) const {
    const auto& plots = spatial_.plots;
    const auto plot_index = [&](int32_t cell_x, int32_t cell_z) -> std::optional<std::size_t> {
        if (cell_x < 0 || cell_z < 0 || static_cast<uint32_t>(cell_x) >= spatial_.terrain_width ||
            static_cast<uint32_t>(cell_z) >= spatial_.terrain_height)
            return std::nullopt;
        const auto index = static_cast<std::size_t>(cell_z) * spatial_.terrain_width +
                           static_cast<std::size_t>(cell_x);
        if (index >= plots.size())
            return std::nullopt;
        return index;
    };
    auto cell_x = static_cast<int16_t>(point[0] >> 20);
    auto cell_z = static_cast<int16_t>(point[2] >> 20);
    auto index = plot_index(cell_x, cell_z);
    if (!index)
        return sim::spatial_state::no_feature;
    if (plots[*index].feature_word == sim::spatial_state::feature_continuation) {
        cell_x = static_cast<int16_t>(cell_x - plots[*index].feature_back_x);
        cell_z = static_cast<int16_t>(cell_z - plots[*index].feature_back_z);
        index = plot_index(cell_x, cell_z);
        if (!index)
            return sim::spatial_state::no_feature;
    }
    const auto word = plots[*index].feature_word;
    if (word >= sim::spatial_state::first_reserved_feature)
        return sim::spatial_state::no_feature;
    if (origin_x != nullptr)
        *origin_x = cell_x;
    if (origin_z != nullptr)
        *origin_z = cell_z;
    return word;
}

bool Match::site_clear_for(
    uint16_t type,
    int32_t cell_x,
    int32_t cell_z,
    uint16_t skip_unit,
    uint8_t occupancy_kind,
    uint8_t facing
) const {
    const auto oob = occupancy_kind == 2;
    if (type == 0 || type >= world_.types.size())
        return oob;
    // A building facing east or west has its footprint's width and depth swapped.
    const bool turned = (build_facing(type, facing) & 1U) != 0;
    const auto fx = static_cast<int32_t>(
        turned ? world_.types[type].footprint_z : world_.types[type].footprint_x
    );
    const auto fz = static_cast<int32_t>(
        turned ? world_.types[type].footprint_x : world_.types[type].footprint_z
    );
    const auto width = static_cast<int32_t>(spatial_.terrain_width);
    const auto height = static_cast<int32_t>(spatial_.terrain_height);
    // Packed cell_x/cell_z >= 0 and footprint inside the far border.
    if (cell_x < 0 || cell_z < 0 || width <= cell_x + fx || height <= cell_z + fz)
        return oob;
    if (world_.types[type].bm_code == 0)
        return building_site_clear(type, cell_x, cell_z, 0, {facing});
    if (occupancy_kind != 1)
        return true;
    uint8_t max_slope = 255, max_water_slope = 255;
    int16_t max_water_depth = 10000, min_water_depth = -10000;
    if (type < input_.fields.size()) {
        if (const auto* meta = input_.fields[type].runtime_metadata) {
            max_slope = meta->max_slope;
            max_water_slope = meta->max_water_slope;
            max_water_depth = meta->max_water_depth;
            min_water_depth = meta->min_water_depth;
        }
    }
    const auto sea = static_cast<int32_t>(spatial_.sea_level);
    for (int32_t row = 0; row < fz; ++row) {
        for (int32_t column = 0; column < fx; ++column) {
            const auto& plot =
                spatial_.plots
                    [static_cast<std::size_t>(cell_z + row) * spatial_.terrain_width +
                     static_cast<std::size_t>(cell_x + column)];
            if (plot.blocking_feature)
                return false;
            if (plot.ground != sim::spatial_state::no_unit && plot.ground != skip_unit)
                return false;
            const auto low = static_cast<int32_t>(plot.low_height);
            const auto high = static_cast<int32_t>(plot.high_height);
            if (low < sea - static_cast<int32_t>(max_water_depth))
                return false;
            if (sea - static_cast<int32_t>(min_water_depth) < high)
                return false;
            const auto slope = high - low;
            if (static_cast<int32_t>(max_slope) < slope) {
                if (sea <= low)
                    return false;
                if (static_cast<int32_t>(max_water_slope) < slope)
                    return false;
            }
        }
    }
    return true;
}

} // namespace oa::sim::match_runtime
