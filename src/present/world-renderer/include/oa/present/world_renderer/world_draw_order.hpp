// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The order the battlefield draws its features and units in. The map is
// drawn far to near in rows one plot deep. Features lower than
// standing_feature_min_height (rubble, metal heaps) lie under every unit and
// draw first. Then each row draws its ground units followed by the standing
// features (trees, rocks, wrecks) whose plot is in that row, so a tree covers
// the units in its own row and the rows behind it, and the units in the rows
// in front of it cover the tree. A ground unit standing on a building's
// footprint, as a unit leaving a factory stands on its platform, draws
// straight after that building wherever its row would draw it first, so the
// building never covers it. Aircraft and units off the ground draw last,
// after the projectiles and explosions, over everything on the ground.

#include <cstdint>
#include <span>
#include <vector>

namespace oa::present::world_renderer {

/// Lowest FeatureDef.height of a feature that stands among the ground units.
inline constexpr uint8_t standing_feature_min_height = 10;

/// Map pixels of depth in one draw row: one plot.
inline constexpr int32_t draw_row_depth = 16;

/// Map pixels across and down one footprint cell.
inline constexpr int32_t footprint_cell_pixels = 16;

/// A feature to place in the draw order, by the plot that holds it.
struct FeatureDrawSite {
    int32_t cell_x{}; ///< plot column of the feature's origin plot
    int32_t cell_z{}; ///< plot row of the feature's origin plot
    int8_t height{};  ///< FeatureDef.height
};

/// A unit to place in the draw order.
struct UnitDrawSite {
    int32_t position_z{};  ///< Unit.position.z, 16.16 map pixels
    uint32_t flags{};      ///< Unit.flags; the occupancy bits and the building bit are read
    int32_t position_x{};  ///< Unit.position.x, 16.16 map pixels
    int16_t footprint_x{}; ///< Unit.footprint_x, cells across; read for a building
    int16_t footprint_z{}; ///< Unit.footprint_z, cells down; read for a building
};

/// What a ground-pass draw is.
enum class DrawnThing : uint8_t {
    feature,
    unit,
};

/// One draw of the ground pass.
struct BattlefieldDraw {
    DrawnThing kind{};
    uint32_t index{}; ///< position in the features or the units the plan was made from
};

/// The battlefield's feature and unit draws, in the order each pass makes them.
struct BattlefieldDrawPlan {
    /// Features under every unit, row by row, each row left to right.
    std::vector<uint32_t> lying_features;
    /// Ground units and standing features, row by row: a row's units in the
    /// order given, then its features left to right.
    std::vector<BattlefieldDraw> ground;
    /// Units that are not on the ground, row by row, in the order given.
    std::vector<uint32_t> raised_units;
};

/// Returns whether a feature stands among the ground units rather than lying under them.
///
/// @param height FeatureDef.height
/// @return true when the height, read as an unsigned byte, is at least
///     standing_feature_min_height
/// @quirk A negative height reads as a large one, so such a feature stands.
[[nodiscard]] bool feature_stands(int8_t height) noexcept;

/// Returns the plot row a unit draws with.
///
/// @param position_z Unit.position.z, 16.16 map pixels
/// @param camera_y Game.camera_y, map pixels at the top of the view; below 0
///     above the map
/// @return the camera's plot row, rounded toward negative infinity for a
///     camera above the map, plus the whole rows, truncated toward zero,
///     between the camera and the unit's whole-pixel depth
/// @quirk Rows are counted from the camera rather than from the map edge, so
///     while the view is part-way into a plot a unit up to 15 pixels into the
///     next plot row still draws with the row before it.
[[nodiscard]] int32_t unit_draw_row(int32_t position_z, int32_t camera_y) noexcept;

/// Returns whether a unit's place lies on a building's footprint.
///
/// @param unit the unit
/// @param building the building, whose place is its footprint's centre
/// @return true when the unit's whole-pixel place lies within the
///     footprint's cells, its left and top edges included
[[nodiscard]] bool
stands_on_footprint(const UnitDrawSite& unit, const UnitDrawSite& building) noexcept;

/// Orders a frame's features and units for the battlefield's draw passes.
///
/// A feature is placed by the plot row and column that hold it, a unit by
/// unit_draw_row. Units whose occupancy is ground go to the ground pass, the
/// others (in the air, or carried) to the raised pass. A ground unit that is
/// not a building and stands on the footprint of a ground building
/// (stands_on_footprint) draws straight after that building when its row and
/// its place in the list would draw it before; units that follow one
/// building keep the order given among themselves.
///
/// @param features the features to draw
/// @param units the units to draw, in the order a row draws them
/// @param camera_y Game.camera_y, map pixels at the top of the view
/// @param[out] plan the three passes; its lists are replaced
void plan_battlefield_draws(
    std::span<const FeatureDrawSite> features,
    std::span<const UnitDrawSite> units,
    int32_t camera_y,
    BattlefieldDrawPlan& plan
);

} // namespace oa::present::world_renderer
