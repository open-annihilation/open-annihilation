// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/goals.hpp"
#include "oa/base/game_math.hpp"
#include <bit>

namespace oa::sim::ground_orders {
namespace {
constexpr uint32_t half_cell = 0x80000; // half a 16-unit cell in 16.16

int32_t wrap(uint32_t v) noexcept {
    return std::bit_cast<int32_t>(v);
}

int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return wrap(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t wrap_sub(int32_t a, int32_t b) noexcept {
    return wrap(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t wrap_mul(int32_t a, int32_t b) noexcept {
    return wrap(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

// abs(): INT_MIN stays INT_MIN.
int32_t abs32(int32_t v) noexcept {
    return v < 0 ? wrap_sub(0, v) : v;
}

// Footprint-origin cell of a 16.16 world coordinate (arithmetic shift).
int16_t origin_cell(Fixed position, int16_t footprint) noexcept {
    const auto biased = wrap(
        static_cast<uint32_t>(position) - static_cast<uint32_t>(footprint) * half_cell + half_cell
    );
    return static_cast<int16_t>(biased >> 20);
}

// 16.16 centre of a footprint whose origin cell is `cell`.
Fixed cell_centre(int16_t footprint, int32_t cell) noexcept {
    return wrap_mul(wrap_add(footprint, wrap_mul(cell, 2)), static_cast<int32_t>(half_cell));
}

int32_t squared_cells(int32_t range) noexcept {
    const auto cells = range / 0x10;
    return wrap_mul(cells, cells);
}

// Octile distance with the long axis weighted 18 and the short axis 7.
int32_t octile(const Goal& g, int32_t x, int32_t z) noexcept {
    const auto dx = abs32(wrap_sub(x, g.cell[0]));
    const auto dz = abs32(wrap_sub(z, g.cell[1]));
    if (dz < dx)
        return wrap_add(wrap_mul(dz, 7), wrap_mul(dx, 0x12));
    return wrap_add(wrap_mul(dx, 7), wrap_mul(dz, 0x12));
}

int32_t squared_offset(const Goal& g, int32_t x, int32_t z) noexcept {
    const auto dx = wrap_sub(x, g.cell[0]);
    const auto dz = wrap_sub(z, g.cell[1]);
    return wrap_add(wrap_mul(dx, dx), wrap_mul(dz, dz));
}

int32_t outline_cost(const Goal& g, int32_t x, int32_t z) noexcept {
    int32_t across;
    if (x < g.left)
        across = wrap_sub(g.left, x);
    else if (g.right < x)
        across = wrap_sub(x, g.right);
    else {
        int32_t inside;
        if (z < g.top)
            inside = wrap_sub(g.top, z);
        else if (g.bottom < z)
            inside = wrap_sub(z, g.bottom);
        else {
            auto nearest_x = wrap_sub(g.right, x);
            if (wrap_sub(x, g.left) < wrap_sub(g.right, x))
                nearest_x = wrap_sub(x, g.left);
            inside = wrap_sub(g.bottom, z);
            if (wrap_sub(z, g.top) < wrap_sub(g.bottom, z))
                inside = wrap_sub(z, g.top);
            if (nearest_x < inside)
                inside = nearest_x;
        }
        return wrap_mul(inside, 0x10);
    }
    int32_t along;
    if (z < g.top)
        along = wrap_sub(g.top, z);
    else if (z <= g.bottom)
        return wrap_mul(across, 0x10);
    else
        along = wrap_sub(z, g.bottom);
    if (along < across)
        return wrap_add(wrap_mul(across, 0x10), wrap_mul(along, 6));
    return wrap_add(wrap_mul(along, 0x10), wrap_mul(across, 6));
}
} // namespace

Goal make_goal(
    sim::simulation_state::Order& order,
    const sim::unit_movement::Unit& unit,
    const Point& target,
    int32_t tolerance
) {
    Goal g;
    g.order = &order;
    g.shape = GoalShape::circle;
    g.cell = {origin_cell(target[0], unit.footprint[0]), origin_cell(target[2], unit.footprint[1])};
    g.tolerance = tolerance;
    g.radius_squared = squared_cells(tolerance);
    return g;
}

Goal make_ring_goal(
    sim::simulation_state::Order& order,
    const sim::unit_movement::Unit& unit,
    const Point& centre,
    int32_t outer,
    int32_t inner
) {
    Goal g;
    g.order = &order;
    g.shape = GoalShape::ring;
    g.cell = {origin_cell(centre[0], unit.footprint[0]), origin_cell(centre[2], unit.footprint[1])};
    g.tolerance = outer;
    g.inner_range = inner;
    g.radius_squared = squared_cells(outer);
    g.inner_radius_squared = squared_cells(inner);
    return g;
}

Goal make_outline_goal(
    sim::simulation_state::Order& order,
    const sim::unit_movement::Unit& builder,
    std::array<int16_t, 2> site,
    std::array<int16_t, 2> size
) {
    Goal g;
    g.order = &order;
    g.shape = GoalShape::outline;
    g.left = int32_t{site[0]} - builder.footprint[0];
    g.top = int32_t{site[1]} - builder.footprint[1];
    g.right = int32_t{size[0]} + site[0];
    g.bottom = int32_t{size[1]} + site[1];
    return g;
}

Goal make_build_goal(
    sim::simulation_state::Order& order,
    const sim::unit_movement::Unit& builder,
    const Point& destination,
    int16_t footprint_x,
    int16_t footprint_z
) {
    return make_outline_goal(
        order,
        builder,
        {origin_cell(destination[0], footprint_x), origin_cell(destination[2], footprint_z)},
        {footprint_x, footprint_z}
    );
}

bool goal_contains(const Goal& g, int32_t x, int32_t z) noexcept {
    switch (g.shape) {
    case GoalShape::circle:
        return squared_offset(g, x, z) <= g.radius_squared;
    case GoalShape::ring: {
        const auto d = squared_offset(g, x, z);
        return !(g.radius_squared < d || d < g.inner_radius_squared);
    }
    case GoalShape::outline:
        if ((x == g.left || x == g.right) && g.top <= z && z <= g.bottom)
            return true;
        return (z == g.top || z == g.bottom) && g.left <= x && x <= g.right;
    }
    return false;
}

bool goal_contains_unit(const Goal& g, const sim::unit_movement::Unit& unit) noexcept {
    return goal_contains(g, unit.cell[0], unit.cell[1]);
}

bool same_goal(const Goal& a, const Goal& b) noexcept {
    return a.order == b.order && a.shape == b.shape && a.cell == b.cell &&
           a.tolerance == b.tolerance && a.radius_squared == b.radius_squared &&
           a.inner_range == b.inner_range && a.inner_radius_squared == b.inner_radius_squared &&
           a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}

int32_t goal_cost(const Goal& g, int32_t x, int32_t z) noexcept {
    switch (g.shape) {
    case GoalShape::circle: {
        const auto d = octile(g, x, z);
        return d < g.tolerance ? 0 : wrap_sub(d, g.tolerance);
    }
    case GoalShape::ring: {
        const auto d = octile(g, x, z);
        if (g.tolerance < d)
            return wrap_sub(d, g.tolerance);
        if (d < g.inner_range)
            return wrap_sub(g.inner_range, d);
        return 0;
    }
    case GoalShape::outline:
        return outline_cost(g, x, z);
    }
    return 0;
}

Point goal_position(const Goal& g, const sim::unit_movement::Unit& u) noexcept {
    Point out{};
    switch (g.shape) {
    case GoalShape::circle:
        out[0] = cell_centre(u.footprint[0], g.cell[0]);
        out[2] = cell_centre(u.footprint[1], g.cell[1]);
        break;
    case GoalShape::ring: {
        out[0] = cell_centre(u.footprint[0], g.cell[0]);
        out[2] = cell_centre(u.footprint[1], g.cell[1]);
        // Offset toward the unit by the mean range along its bearing to the centre.
        const auto bearing = base::game_math::direction(
            wrap_sub(u.position[0], out[0]), wrap_sub(u.position[2], out[2])
        );
        const auto sum = wrap_add(g.tolerance, g.inner_range);
        const auto mean = wrap(static_cast<uint32_t>(sum / 2) << 16);
        out[0] = wrap_add(out[0], sim::unit_movement::sine_scaled(bearing, mean));
        out[2] = wrap_add(out[2], sim::unit_movement::cosine_scaled(bearing, mean));
        break;
    }
    case GoalShape::outline:
        out[0] = cell_centre(u.footprint[0], static_cast<int16_t>(wrap_add(g.right, g.left) / 2));
        out[2] = cell_centre(u.footprint[1], static_cast<int16_t>(g.bottom));
        break;
    }
    return out;
}
} // namespace oa::sim::ground_orders
