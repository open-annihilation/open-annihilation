// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/formats/objects3d.hpp"
#include "oa/sim/model_runtime/instance.hpp"
#include "oa/sim/unit_movement/terrain.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace oa::sim::gameplay_input {
struct ScreenPoint {
    int32_t x{}, y{};
};

struct Camera {
    int32_t pixel_x{}, pixel_z{};
};

struct PickUnit {
    uint16_t id{};
    formats::objects3d::FixedVector3 position{};
    sim::model_runtime::RotationWords rotation{};
    const formats::objects3d::Model* model{};
};

/// Projects a model-local point of a unit onto the screen.
///
/// @param local point in the unit's model space, 16.16
/// @param world unit position, 16.16 world coordinates
/// @param camera camera position, whole pixels
/// @return screen pixel: x from world X, y from world Z less half the height, offset by
///         the battlefield origin (+128, +32)
/// @quirk Every add and subtract wraps at 32 bits before the high words are taken.
[[nodiscard]] ScreenPoint project(
    const formats::objects3d::FixedVector3& local,
    const formats::objects3d::FixedVector3& world,
    const Camera& camera
) noexcept;
/// Tests whether a point lies strictly inside every edge of a clockwise screen polygon.
///
/// @param polygon corners in clockwise screen order; fewer than three contain nothing
/// @param point screen pixel to test
/// @return true when the point is strictly inside
/// @quirk The edge products wrap at 32 bits and are compared, not subtracted.
[[nodiscard]] bool
inside_clockwise(std::span<const ScreenPoint> polygon, ScreenPoint point) noexcept;
/// Tests whether the pointer is over a unit's root-object box.
///
/// The four min-Y corners of the root object's box (formats::objects3d::object_bounds) are
/// rotated by the unit's angles, projected, and tested as a clockwise polygon.
///
/// A unit without a root model has no box and is never hit.
///
/// @param unit unit with its model, position and rotation
/// @param camera camera position, whole pixels
/// @param point pointer position, screen pixels
/// @return true when the pointer is inside the projected box base
[[nodiscard]] bool hits_root_bounds(const PickUnit& unit, const Camera& camera, ScreenPoint point);
/// Lists the units whose root-object box is under the pointer.
///
/// A unit without a root model is never hit.
///
/// @param units candidate units in pick order
/// @param camera camera position, whole pixels
/// @param point pointer position, screen pixels
/// @return ids of the units hit, in the order given
[[nodiscard]] std::vector<uint16_t>
hit_candidates(std::span<const PickUnit> units, const Camera& camera, ScreenPoint point);
/// Finds the terrain point seen at a screen-plane position.
///
/// Takes the map column and searches ground rows from eight cell rows beyond the
/// point towards it for the first whose projection (row less half the surface height,
/// never below sea level) reaches `projected_z`, then interpolates between that row and
/// the next. The inputs are the camera-adjusted pointer position, or a campaign
/// MoveUnitToRadius point.
///
/// A map with no width or height has no terrain: the result is the origin. When
/// the row found and the next project to the same screen row, there is nothing
/// to interpolate and the row found stands.
///
/// @param terrain height and sea level queries
/// @param map_x camera-adjusted map column, whole units; clamped to the map
/// @param projected_z camera-adjusted screen-plane row, whole units; clamped to the map
/// @param map_width map width, whole units
/// @param map_height map height, whole units
/// @return the terrain point, 16.16 world coordinates
/// @quirk The interpolation numerator wraps at 32 bits, as the game computes it.
[[nodiscard]] formats::objects3d::FixedVector3 terrain_intersection(
    const sim::unit_movement::Terrain& terrain,
    int32_t map_x,
    int32_t projected_z,
    int32_t map_width,
    int32_t map_height
);
/// Finds the point a drag box's corner takes at a screen-plane position.
///
/// Over the map it is the terrain point seen there (terrain_intersection).
/// A view that may show past the map's edges shows no ground there: a
/// position past them is its own point at height 0, so that the box reaches
/// as far as the view shows and takes the units drawn there. A view kept on
/// the map takes the terrain point wherever the position lies, as 3.1c does.
///
/// @param terrain height and sea level queries
/// @param map_x camera-adjusted map column, whole units
/// @param projected_z camera-adjusted screen-plane row, whole units
/// @param map_width map width, whole units
/// @param map_height map height, whole units
/// @param past_edges true when the view may show past the map's edges
/// @return the corner, 16.16 world coordinates
[[nodiscard]] formats::objects3d::FixedVector3 drag_box_corner(
    const sim::unit_movement::Terrain& terrain,
    int32_t map_x,
    int32_t projected_z,
    int32_t map_width,
    int32_t map_height,
    bool past_edges
);
/// Holds a ground point a turned view's pick found to the map the view
/// shows, as the classic pick holds its point: a point drawn within the
/// shown map stands; one drawn past it takes the terrain point seen at the
/// nearest shown map pixel (terrain_intersection). A point is drawn at its
/// column, and at its row less half its height, rounded down. So a pick
/// reaches no further past the map's edges than 3.1c's, whose view never
/// shows past them.
///
/// @param terrain height and sea level queries
/// @param point the ground point the pick found, 16.16 world coordinates
/// @param shown_width the shown map's width, whole units; 0 or less, with
///        no shown map known, holds nothing
/// @param shown_height the shown map's height, whole units; likewise
/// @param map_width map width, whole units
/// @param map_height map height, whole units
/// @return the point, 16.16 world coordinates
[[nodiscard]] formats::objects3d::FixedVector3 ground_point_on_shown_map(
    const sim::unit_movement::Terrain& terrain,
    const formats::objects3d::FixedVector3& point,
    int32_t shown_width,
    int32_t shown_height,
    int32_t map_width,
    int32_t map_height
);
/// Tells whether a building's footprint lies within the map the view shows,
/// as the local player's build cursor requires of a site.
///
/// Across, its columns lie within the shown width. Down, it is drawn raised
/// by half the height it stands at, rounded down, as the map's hidden edges
/// are measured: its first row is drawn at or below the map's top and its
/// last row within the shown height. So the cursor offers no site whose
/// building would stand over ground the view never shows, such as a
/// geothermal plant over a vent in the rows past the map's bottom edge,
/// which the simulation's own site test lets through. That test is kept as
/// it is: orders from other machines and the computer players' builds are
/// not held to this.
///
/// @param cell_x the footprint's first column; negative left of the map
/// @param cell_z the footprint's first row; negative above the map
/// @param footprint_x its columns
/// @param footprint_z its rows
/// @param height the height the building stands at, whole units
/// @param shown_width the shown map's width, whole units; 0 or less, with
///        no shown map known, holds nothing
/// @param shown_height the shown map's height, whole units; likewise
/// @return true when the footprint lies within the shown map
[[nodiscard]] bool footprint_within_shown_map(
    int32_t cell_x,
    int32_t cell_z,
    int32_t footprint_x,
    int32_t footprint_z,
    int32_t height,
    int32_t shown_width,
    int32_t shown_height
) noexcept;

// Command bytes that never take the unit under the pointer as their target. 5 is
// UNLOAD. 10 is the command that resolves to STOP. 0x0e is the build cursor a
// structure button on the build panel arms.
inline constexpr uint8_t unload_command = 5;
inline constexpr uint8_t stop_command = 10;
inline constexpr uint8_t build_command = 0x0e;

/// Tests whether an armed command takes the unit under the pointer (Game.cursor_unit_id) as its target.
///
/// @param command armed command byte
/// @return false only for unload, stop and build
[[nodiscard]] bool command_binds_cursor_unit(uint8_t command) noexcept;

} // namespace oa::sim::gameplay_input
