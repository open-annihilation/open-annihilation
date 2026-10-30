// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/world_renderer/world_draw_order.hpp"

#include "oa/core/unit.h"

#include <algorithm>
#include <tuple>

namespace oa::present::world_renderer {
namespace {

/// Unit.flags occupancy of a unit on the ground or the water.
constexpr uint32_t ground_occupancy = 1;

/// Returns the whole map pixel of a 16.16 coordinate.
int32_t whole_pixel(int32_t position) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(position) >> 16);
}

/// Returns whether a unit stands on the ground or the water.
bool on_ground(const UnitDrawSite& unit) noexcept {
    return (unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == ground_occupancy;
}

/// Returns whether a unit is a building.
bool is_building(const UnitDrawSite& unit) noexcept {
    return (unit.flags & OA_UNIT_FLAG_BUILDING) != 0;
}

} // namespace

bool feature_stands(int8_t height) noexcept {
    return static_cast<uint8_t>(height) >= standing_feature_min_height;
}

int32_t unit_draw_row(int32_t position_z, int32_t camera_y) noexcept {
    const int32_t whole = static_cast<int16_t>(static_cast<uint32_t>(position_z) >> 16);
    // A camera above the map counts its row toward negative infinity, so
    // that its rows run on from the map's as a camera's on the map do.
    const int32_t camera_row = camera_y >= 0 ? camera_y / draw_row_depth
                                             : -((draw_row_depth - 1 - camera_y) / draw_row_depth);
    return camera_row + (whole - camera_y) / draw_row_depth;
}

bool stands_on_footprint(const UnitDrawSite& unit, const UnitDrawSite& building) noexcept {
    // A building stands at its footprint's centre, so its cells reach half
    // the footprint either way of it.
    const int32_t left =
        whole_pixel(building.position_x) - building.footprint_x * footprint_cell_pixels / 2;
    const int32_t top =
        whole_pixel(building.position_z) - building.footprint_z * footprint_cell_pixels / 2;
    const int32_t x = whole_pixel(unit.position_x);
    const int32_t z = whole_pixel(unit.position_z);
    return x >= left && x < left + building.footprint_x * footprint_cell_pixels && z >= top &&
           z < top + building.footprint_z * footprint_cell_pixels;
}

void plan_battlefield_draws(
    std::span<const FeatureDrawSite> features,
    std::span<const UnitDrawSite> units,
    int32_t camera_y,
    BattlefieldDrawPlan& plan
) {
    plan.lying_features.clear();
    plan.ground.clear();
    plan.raised_units.clear();

    // Sort keys: the row, then units before features, then a unit's place
    // in the list or a feature's column, then whether a unit follows the
    // building of that place, then a feature's or a following unit's place in
    // the list.
    struct Keyed {
        int32_t row{};
        uint8_t rank{};
        int32_t order{};
        uint8_t follows{};
        BattlefieldDraw draw{};
    };

    // Kept from frame to frame with room for every draw at once, so that a
    // frame's lists neither take memory nor grow as they fill.
    thread_local std::vector<Keyed> ground;
    thread_local std::vector<Keyed> lying;
    thread_local std::vector<Keyed> raised;
    ground.clear();
    lying.clear();
    raised.clear();
    ground.reserve(features.size() + units.size());
    lying.reserve(features.size());
    raised.reserve(units.size());
    for (uint32_t index = 0; index < features.size(); ++index) {
        const auto& feature = features[index];
        const Keyed keyed{feature.cell_z, 1, feature.cell_x, 0, {DrawnThing::feature, index}};
        (feature_stands(feature.height) ? ground : lying).push_back(keyed);
    }
    // The ground pass's buildings, by their place in it.
    thread_local std::vector<uint32_t> buildings;
    buildings.clear();
    buildings.reserve(units.size());
    for (uint32_t index = 0; index < units.size(); ++index) {
        const auto& unit = units[index];
        const Keyed keyed{
            unit_draw_row(unit.position_z, camera_y),
            0,
            static_cast<int32_t>(index),
            0,
            {DrawnThing::unit, index}
        };
        if (on_ground(unit)) {
            if (is_building(unit))
                buildings.push_back(static_cast<uint32_t>(ground.size()));
            ground.push_back(keyed);
        } else {
            raised.push_back(keyed);
        }
    }
    // Every draw of a list has keys of its own: a unit's place is its own,
    // and a feature in the same cell as another, or a unit following the same
    // building as another, follows it in the list.
    const auto before = [](const Keyed& a, const Keyed& b) {
        return std::tie(a.row, a.rank, a.order, a.follows, a.draw.index) <
               std::tie(b.row, b.rank, b.order, b.follows, b.draw.index);
    };
    // A ground unit on a building's footprint, which its row and place would
    // draw before the building, takes the building's keys and follows it.
    if (!buildings.empty())
        for (auto& keyed : ground) {
            if (keyed.draw.kind != DrawnThing::unit || is_building(units[keyed.draw.index]))
                continue;
            for (const uint32_t at : buildings) {
                const Keyed& building = ground[at];
                if (stands_on_footprint(units[keyed.draw.index], units[building.draw.index]) &&
                    before(keyed, building)) {
                    keyed.row = building.row;
                    keyed.order = building.order;
                    keyed.follows = 1;
                    break;
                }
            }
        }
    std::sort(lying.begin(), lying.end(), before);
    std::sort(ground.begin(), ground.end(), before);
    std::sort(raised.begin(), raised.end(), before);
    plan.lying_features.reserve(lying.size());
    plan.ground.reserve(ground.size());
    plan.raised_units.reserve(raised.size());
    for (const auto& keyed : lying)
        plan.lying_features.push_back(keyed.draw.index);
    for (const auto& keyed : ground)
        plan.ground.push_back(keyed.draw);
    for (const auto& keyed : raised)
        plan.raised_units.push_back(keyed.draw.index);
}

} // namespace oa::present::world_renderer
