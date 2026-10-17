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
    // in the list or a feature's column.
    struct Keyed {
        int32_t row{};
        uint8_t rank{};
        int32_t order{};
        BattlefieldDraw draw{};
    };

    // Room for every draw at once, so that a frame's lists do not grow as
    // they fill.
    std::vector<Keyed> ground;
    std::vector<Keyed> lying;
    std::vector<Keyed> raised;
    ground.reserve(features.size() + units.size());
    lying.reserve(features.size());
    raised.reserve(units.size());
    for (uint32_t index = 0; index < features.size(); ++index) {
        const auto& feature = features[index];
        const Keyed keyed{feature.cell_z, 1, feature.cell_x, {DrawnThing::feature, index}};
        (feature_stands(feature.height) ? ground : lying).push_back(keyed);
    }
    for (uint32_t index = 0; index < units.size(); ++index) {
        const auto& unit = units[index];
        const Keyed keyed{
            unit_draw_row(unit.position_z, camera_y),
            0,
            static_cast<int32_t>(index),
            {DrawnThing::unit, index}
        };
        const bool on_ground = (unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == ground_occupancy;
        (on_ground ? ground : raised).push_back(keyed);
    }
    const auto before = [](const Keyed& a, const Keyed& b) {
        return std::tie(a.row, a.rank, a.order) < std::tie(b.row, b.rank, b.order);
    };
    std::stable_sort(lying.begin(), lying.end(), before);
    std::stable_sort(ground.begin(), ground.end(), before);
    std::stable_sort(raised.begin(), raised.end(), before);
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
