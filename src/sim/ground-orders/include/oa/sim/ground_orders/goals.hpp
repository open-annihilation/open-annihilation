// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/unit_spawn/legacy_views.hpp"
#include "oa/sim/unit_movement/movement.hpp"
#include <array>
#include <cstdint>

namespace oa::sim::ground_orders {
using Fixed = sim::unit_movement::Fixed;
using Point = std::array<Fixed, 3>;

// Order events a goal raises on its order (Order::raised_events): the
// navigator's, then the path search's.
inline constexpr uint32_t arrived_event = 0x20, path_failed_event = 0x40,
                          goal_replaced_event = 0x80;
inline constexpr uint32_t search_goal_contact_flag = 0x100; // order event: start inside goal
// Order event: the start is off the grid, or the wall-follow seed missed the goal.
inline constexpr uint32_t search_seed_unresolved_flag = 0x200;
// Every event a goal raises on its order. Installing a new goal on an order
// clears them, so the order only hears from the goal it now follows.
inline constexpr uint32_t goal_event_mask = arrived_event | path_failed_event |
                                            goal_replaced_event | search_goal_contact_flag |
                                            search_seed_unresolved_flag;

// Ground movement goals. Each shape is one of the game's goal kinds and its
// value the kind number the goal reports (the air goals are kinds 2 and 3).
// The navigator and path search only use goals through the functions below.
enum class GoalShape : uint8_t {
    /// Arrive within tolerance of a cell.
    circle = 4,
    /// Stay between an inner and outer range (attack).
    ring = 5,
    /// Stand on the border around a build site.
    outline = 6,
};

struct Goal {
    sim::simulation_state::Order* order{}; // order events are raised on it
    GoalShape shape{GoalShape::circle};
    std::array<int16_t, 2> cell{};  // circle/ring centre (unit footprint origin)
    int32_t tolerance{};            // circle: arrival radius; ring: outer range (world units)
    int32_t radius_squared{};       // circle radius; ring outer radius; squared (cells^2)
    int32_t inner_range{};          // ring inner range (world units)
    int32_t inner_radius_squared{}; // ring inner radius, squared (cells^2)
    // Outline border cells, all inclusive.
    int32_t left{}, right{}, top{}, bottom{};
};

/// Builds a circular goal around a target for a unit's footprint.
///
/// @param order order whose events the goal raises
/// @param unit moving unit, for its footprint
/// @param target signed 16.16 world position; converted to the footprint-origin cell
/// @param tolerance arrival radius in world units
/// @return the goal
Goal make_goal(
    sim::simulation_state::Order& order,
    const sim::unit_movement::Unit& unit,
    const Point& target,
    int32_t tolerance
);
/// Builds a ring goal between an inner and outer range of a centre.
///
/// @param order order whose events the goal raises
/// @param unit moving unit, for its footprint
/// @param centre signed 16.16 world position; converted to the footprint-origin cell
/// @param outer outer range in world units
/// @param inner inner range in world units
/// @return the goal
Goal make_ring_goal(
    sim::simulation_state::Order& order,
    const sim::unit_movement::Unit& unit,
    const Point& centre,
    int32_t outer,
    int32_t inner
);
/// Builds the border goal around a build site.
///
/// The builder's own footprint is subtracted from the near edges.
///
/// @param order order whose events the goal raises
/// @param builder building unit, for its footprint
/// @param site top-left cell of the site
/// @param size site size in cells
/// @return the goal
Goal make_outline_goal(
    sim::simulation_state::Order& order,
    const sim::unit_movement::Unit& builder,
    std::array<int16_t, 2> site,
    std::array<int16_t, 2> size
);
/// Builds the outline goal for a snapped build destination.
///
/// Converts the 16.16 site centre to its top-left cell the same way as the
/// build-site checks.
///
/// @param order order whose events the goal raises
/// @param builder building unit, for its footprint
/// @param destination signed 16.16 site centre
/// @param footprint_x site width in cells
/// @param footprint_z site depth in cells
/// @return the goal
Goal make_build_goal(
    sim::simulation_state::Order& order,
    const sim::unit_movement::Unit& builder,
    const Point& destination,
    int16_t footprint_x,
    int16_t footprint_z
);

/// Tests whether a footprint-origin cell satisfies the goal.
///
/// A circle holds cells within its squared radius, a ring cells between its
/// squared radii, an outline the cells on its border.
///
/// @param goal goal to test
/// @param cell_x cell column
/// @param cell_z cell row
/// @return true when the cell satisfies the goal
bool goal_contains(const Goal& goal, int32_t cell_x, int32_t cell_z) noexcept;
/// Tests whether the unit's current cell satisfies the goal.
///
/// Every goal shape tests the unit's cell with goal_contains.
///
/// @param goal goal to test
/// @param unit unit whose cell is tested
/// @return true when the unit's cell satisfies the goal
bool goal_contains_unit(const Goal& goal, const sim::unit_movement::Unit& unit) noexcept;
/// Tests whether two goals report to the same order and have the same shape,
/// cells and ranges.
///
/// @param a one goal
/// @param b the other goal
/// @return true when every field is the same
[[nodiscard]] bool same_goal(const Goal& a, const Goal& b) noexcept;
/// Returns the admissible search cost from a cell to the goal.
///
/// Circles and rings use the octile distance with weights 18 and 7 less the
/// range; outlines weight 16 and 6.
///
/// @param goal goal to reach
/// @param cell_x cell column
/// @param cell_z cell row
/// @return the cost, zero inside the goal
int32_t goal_cost(const Goal& goal, int32_t cell_x, int32_t cell_z) noexcept;
/// Returns the world position the navigator steers toward.
///
/// A circle's centre cell; a ring's centre moved toward the unit by the mean
/// range; an outline's bottom-middle cell. Positions are the unit footprint's
/// centre at that cell.
///
/// @param goal goal to steer toward
/// @param unit steering unit, for its footprint and position
/// @return signed 16.16 position; Y is left at zero
Point goal_position(const Goal& goal, const sim::unit_movement::Unit& unit) noexcept;

/// Visits the cells the path search marks as goal cells.
///
/// The centre of a circle, one cell at the mean range south of a ring, and every
/// border cell of an outline (top and bottom rows first, then the side columns).
///
/// @param g goal whose cells are visited
/// @param fn called with (cell_x, cell_z) for each goal cell
template <typename Fn>
void for_each_goal_cell(const Goal& g, Fn&& fn) {
    switch (g.shape) {
    case GoalShape::circle:
        fn(int32_t{g.cell[0]}, int32_t{g.cell[1]});
        return;
    case GoalShape::ring: {
        const auto sum = static_cast<int32_t>(
            static_cast<uint32_t>(g.tolerance) + static_cast<uint32_t>(g.inner_range)
        );
        const auto offset = static_cast<int16_t>(sum / 0x20);
        fn(int32_t{g.cell[0]}, int32_t{static_cast<int16_t>(g.cell[1] + offset)});
        return;
    }
    case GoalShape::outline:
        for (auto x = g.left; x <= g.right; ++x) {
            fn(int32_t{static_cast<int16_t>(x)}, int32_t{static_cast<int16_t>(g.top)});
            fn(int32_t{static_cast<int16_t>(x)}, int32_t{static_cast<int16_t>(g.bottom)});
        }
        for (auto z = g.top + 1; z <= g.bottom - 1; ++z) {
            fn(int32_t{static_cast<int16_t>(g.left)}, int32_t{static_cast<int16_t>(z)});
            fn(int32_t{static_cast<int16_t>(g.right)}, int32_t{static_cast<int16_t>(z)});
        }
        return;
    }
}
} // namespace oa::sim::ground_orders
