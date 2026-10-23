// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/orders.hpp"
#include "oa/core/unit.h"
#include "oa/sim/ground_orders/movement_map.hpp"
#include "oa/base/game_math.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace oa::sim::ground_orders {
namespace {
constexpr uint8_t route_bit = route_present_flag, search_pending = search_pending_flag,
                  route_changed = route_changed_flag;
constexpr uint8_t retry_order = 0x80, announce_command = order_announce_flag;
constexpr uint32_t aircraft_type = 0x800, armed_wait_event = 0x10000, timer_event = 1;
constexpr uint32_t attack_mode_mask = 0x300000, attack_mode_seek = 0x200000;
constexpr uint32_t half_cell = 0x80000, point_reached_squared = 26, search_restart_delay = 10;
constexpr Fixed one_world_unit = 65536, lookahead_limit = 80 * one_world_unit;

Fixed bits(uint32_t v) noexcept {
    return std::bit_cast<Fixed>(v);
}

Fixed low(int64_t v) noexcept {
    return bits(static_cast<uint32_t>(v));
}

Fixed add(Fixed a, Fixed b) noexcept {
    return bits(uint32_t(a) + uint32_t(b));
}

Fixed sub(Fixed a, Fixed b) noexcept {
    return bits(uint32_t(a) - uint32_t(b));
}

Fixed mul(Fixed a, Fixed b) noexcept {
    return low(int64_t(a) * b);
}

int16_t short_bits(uint32_t v) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(v));
}

int16_t integer(Fixed v) noexcept {
    return short_bits(uint32_t(v) >> 16);
}

Fixed abs32(Fixed v) noexcept {
    return v < 0 ? sub(0, v) : v;
}

Fixed norm(Fixed x, Fixed z) noexcept {
    return low(static_cast<int64_t>(base::game_math::hypotenuse(double(x), double(z))));
}

Fixed square_high(Fixed v) noexcept {
    return low((int64_t(v) * v) >> 32);
}

// A navigator holds no more points than its capacity.
bool count_fits(const Navigation& n) noexcept {
    return n.count <= n.points.size();
}

/// Raises events on the order the goal reports to.
///
/// The bits are ORed into the order's Order::raised_events.
///
/// @param goal goal whose order is signalled; may be null
/// @param event event bits to raise
void signal(Goal* goal, uint32_t event) {
    if (goal && goal->order)
        goal->order->raised_events |= event;
}

void wait(sim::simulation_state::Order& order, uint32_t tick, uint32_t duration) {
    order.wait_events |= timer_event;
    order.wake_tick = tick + duration;
}

int16_t facing(Fixed x, Fixed z) noexcept {
    return std::bit_cast<int16_t>(base::game_math::direction(x, z));
}
} // namespace

bool SearchController::begin(SearchRecord record) noexcept {
    if (!record.unit || !record.navigation || !record.goal || !record.movement_map || !idle())
        return false;
    active_ = record;
    return true;
}

void SearchController::cancel(Navigation& navigation) noexcept {
    if (active_.navigation == &navigation)
        active_ = {};
}

void release_navigation(SearchController& controller, Navigation& navigation) noexcept {
    controller.cancel(navigation);
}

void SearchController::complete(OccupancyRectangle rectangle, uint32_t changed_tick) {
    if (active_.movement_map)
        active_.movement_map->release_unit(rectangle, changed_tick);
    active_.unit = nullptr;
    active_.movement_map = nullptr;
}

void install_goal(UnitView u, Goal* goal, uint32_t tick, Host& host) {
    auto& n = u.navigation;
    if (!count_fits(n))
        return;
    host.cancel_search(n);
    signal(n.goal, goal_replaced_event);
    n.flags &= uint8_t(~route_bit);
    n.goal = goal;
    if (!goal)
        n.flags &= uint8_t(~search_pending);
    else {
        n.flags |= search_pending;
        if (n.count > 2 &&
            goal_contains(*goal, n.points[n.count - 1][0] >> 4, n.points[n.count - 1][1] >> 4))
            n.flags = uint8_t((n.flags & ~search_pending) | route_bit);
        if ((n.flags & route_bit) == 0) {
            const auto target = goal_position(*goal, u.geometry);
            if (n.count > 2) {
                const auto& end = n.points[n.count - 1];
                const auto unit_distance = norm(
                    sub(u.geometry.position[0], target[0]), sub(u.geometry.position[2], target[2])
                );
                const auto tail_distance = norm(
                    sub(Fixed(end[0]) * one_world_unit, target[0]),
                    sub(Fixed(end[1]) * one_world_unit, target[2])
                );
                if (mul(tail_distance, 2) < unit_distance)
                    n.flags |= route_bit;
            }
            if ((n.flags & route_bit) == 0 && u.state.primary &&
                (u.state.primary->flags & retry_order) == 0) {
                n.count = 2;
                n.points[0] = {integer(u.geometry.position[0]), integer(u.geometry.position[2])};
                n.points[1] = {integer(target[0]), integer(target[2])};
                n.flags |= route_bit;
            }
        }
    }
    if (n.last_search_tick <= tick - search_restart_delay)
        n.last_search_tick = 0;
    n.flags |= route_changed;
}

bool accept_path(UnitView u, std::span<const RoutePoint> points) {
    auto& n = u.navigation;
    const bool failed = points.empty() && n.goal && !goal_contains_unit(*n.goal, u.geometry);
    if (points.empty()) {
        if (failed)
            signal(n.goal, path_failed_event);
        n.flags &= uint8_t(~route_bit);
    } else {
        n.count = static_cast<uint32_t>(std::min(points.size(), n.points.size()));
        std::copy_n(points.begin(), n.count, n.points.begin());
        n.flags |= route_bit;
    }
    n.flags = uint8_t((n.flags & ~search_pending) | route_changed);
    return failed;
}

void advance_path(Navigation& n, uint32_t count) {
    if (!count_fits(n) || count > n.count)
        return;
    if (count) {
        std::move(n.points.begin() + count, n.points.begin() + n.count, n.points.begin());
        n.count -= count;
        if (n.count < 2)
            n.flags &= uint8_t(~route_bit);
        n.flags |= route_changed;
    }
}

void tick_navigation(UnitView u, uint32_t tick, Host& host) {
    auto& n = u.navigation;
    if (!count_fits(n))
        return;
    if (n.goal && goal_contains_unit(*n.goal, u.geometry)) {
        signal(n.goal, arrived_event);
        install_goal(u, nullptr, tick, host);
    }
    if (n.count > 1) {
        const auto dx = sub(integer(u.geometry.position[0]), n.points[1][0]),
                   dz = sub(integer(u.geometry.position[2]), n.points[1][1]);
        if (add(mul(dx, dx), mul(dz, dz)) < Fixed(point_reached_squared))
            advance_path(n, 1);
    }
    if (n.goal && ((u.movement.flags & sim::unit_movement::collision_blocked) != 0 || n.count < 2))
        n.flags |= search_pending;
}

bool search_ready(Navigation& navigation, uint32_t tick) noexcept {
    if ((navigation.flags & search_pending_flag) == 0)
        return false;
    if (tick < navigation.last_search_tick + search_retry_ticks)
        return false;
    navigation.last_search_tick = tick;
    return true;
}

std::array<Point, 3> steering_points(const Navigation& n) noexcept {
    std::array<Point, 3> out{};
    if (n.count == 0 || !count_fits(n))
        return out;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& p = n.points[std::min(i, std::size_t(n.count - 1))];
        out[i] = {Fixed(p[0]) * one_world_unit, 0, Fixed(p[1]) * one_world_unit};
    }
    return out;
}

void snap_to_footprint_centre(
    Point& destination, int16_t footprint_x, int16_t footprint_z
) noexcept {
    auto snap = [](Fixed position, int16_t footprint) noexcept {
        const auto cell = short_bits(
            uint32_t(bits(uint32_t(position) - uint32_t(footprint) * half_cell + half_cell) >> 20)
        );
        return bits((uint32_t(footprint) + uint32_t(cell) * 2u) * half_cell);
    };
    destination[0] = snap(destination[0], footprint_x);
    destination[2] = snap(destination[2], footprint_z);
}

uint32_t move_ground(
    UnitView u,
    sim::simulation_state::Order& order,
    OrderState& extra,
    uint32_t events,
    uint32_t tick,
    Host& host
) {
    if (order.phase == 0) {
        if (u.attached)
            return 7;
        if (extra.command_flags & announce_command) {
            extra.command_flags &= uint8_t(~announce_command);
            host.play_sound(u.state, 5);
        }
        // Aircraft keep the air goal the VTOL move hand-off builds; the ground
        // handler only constructs the rectangular goal for mobiles.
        std::unique_ptr<Goal> next;
        if ((u.geometry.type.flags & aircraft_type) == 0)
            next = std::make_unique<Goal>(
                make_goal(order, u.geometry, extra.destination, add(extra.tolerance, 4))
            );
        if (u.state.object_present) {
            if (extra.goal) {
                install_goal(u, nullptr, tick, host);
                extra.goal.reset();
            }
            if (next) {
                order.raised_events &= ~goal_event_mask;
                install_goal(u, next.get(), tick, host);
                extra.goal = std::move(next);
            }
        }
        order.wait_events = arrived_event | path_failed_event | goal_replaced_event;
        return 1;
    }
    if (order.phase == 1) {
        if (events & arrived_event) {
            host.play_sound(u.state, 6);
            return 5;
        }
        return 9;
    }
    return 7;
}

uint32_t standby(UnitView u, sim::simulation_state::Order& order, uint32_t tick, Host& host) {
    if (order.phase == 0) {
        if (!u.state.object_present)
            return 7;
        for (uint32_t i = 0; i < 3; ++i) {
            auto& f = u.weapon_flags[i];
            if ((f & OA_UNIT_WEAPON_ENABLED) != 0 && (f & OA_UNIT_WEAPON_RETALIATE) == 0) {
                f |= OA_UNIT_WEAPON_RETALIATE;
                host.wake_weapon(u.state, i);
            }
        }
        order.wait_events |= armed_wait_event;
        wait(order, tick, 1);
        return 1;
    }
    if (order.phase == 1) {
        if ((u.state.flags & attack_mode_mask) == attack_mode_seek) {
            if (auto* target = host.find_target(u.state);
                target && host.issue_attack(u.state, *target))
                return 5;
        }
        order.wait_events |= armed_wait_event;
        wait(order, tick, host.random(30) + 30);
        return 2;
    }
    return 7;
}

namespace {
/// The speed, 16.16 pixels a tick, above which a unit of a match that applied
/// mutators is held to its route (hold_to_route).
///
/// The fastest unit of stock 3.1c, with Core Contingency and Battle Tactics,
/// is the ARMSS and CORSS scout ship at 5.8 pixels a tick, and the fastest on
/// land the ARMSH hovercraft at 3.7. Every stock unit steps less than a cell
/// (16 pixels) a tick and brakes well before its goal, so the steering always
/// lands it. Stacked speed mutators can give a commander 42 pixels a tick and
/// acceleration and braking that change its speed by all of that in one
/// tick: such a unit steps over the one cell its move must end in, brakes and
/// turns, and steps back over it, every few ticks for ever. Only a unit faster
/// than any stock unit is held, and only in a match that applied mutators
/// (FastUnitSteering::held_to_route), which 3.1c cannot join: a mod's fast
/// units in any other match steer exactly as in 3.1c.
constexpr Fixed fastest_stock_step = 6 * one_world_unit;

/// Returns the square root of a value, rounded down.
///
/// @param value the value
/// @return the largest whole number whose square is no more than `value`
uint64_t square_root_floor(uint64_t value) noexcept {
    uint64_t root = 0;
    uint64_t bit = uint64_t{1} << 62;
    while (bit > value)
        bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

/// What hold_to_route measures a unit's step against, all 16.16.
struct RouteAhead {
    Fixed distance{};           ///< to the route point steered for
    Fixed ahead_x{}, ahead_z{}; ///< offset to the steering target
    Fixed end_x{}, end_z{};     ///< offset to the third steering point
    Fixed angle{};              ///< heading change to the target, before this tick's turn
    Fixed deceleration{};       ///< the type's braking a tick
};

/// Keeps a unit faster than any stock unit from stepping past its next route
/// point, from coming on faster than it can stop, and from turning wider than
/// its route allows.
///
/// Its speed this tick is held to the distance left to the route point, so
/// the step ends on it rather than past it; to the speed from which its
/// braking still stops it at the third steering point (the point the steering
/// itself brakes for); and, while it still has to turn toward the target, to
/// the speed at which it finishes that turn within half the distance ahead
/// (the rule by which the steering brakes for a turn). A unit no faster than
/// fastest_stock_step, and every unit of a match that steers as 3.1c, is left
/// as it is.
///
/// @param u the unit, already turned this tick
/// @param[in,out] m its movement, already accelerated this tick; its speed and
///     velocity are lowered when held
/// @param ahead what the step is measured against
/// @param fast_units whether the match holds a fast unit to its route
void hold_to_route(
    const sim::unit_movement::Unit& u,
    sim::unit_movement::Movement& m,
    const RouteAhead& ahead,
    FastUnitSteering fast_units
) {
    if (fast_units != FastUnitSteering::held_to_route || u.type.maximum_speed <= fastest_stock_step)
        return;
    int64_t limit = std::max<int64_t>(ahead.distance, 0);
    if (ahead.deceleration > 0) {
        // v * v / (2 * b) is the braking distance; both sides are 16.16, so
        // the speed that brakes to a stop in d is sqrt(2 * b * d) in 16.16.
        // b and d are below 2^31, so the product fits.
        const auto end = std::max<int64_t>(norm(ahead.end_x, ahead.end_z), 0);
        limit = std::min<int64_t>(
            limit,
            static_cast<int64_t>(
                square_root_floor(uint64_t(ahead.deceleration) * uint64_t(end) * 2)
            )
        );
    }
    const auto left = sub(ahead.angle, abs32(m.turn));
    if (left > 0 && u.type.maximum_turn != 0) {
        // Turning `left` takes left / maximum_turn ticks; at speed s it covers
        // s * left / maximum_turn, kept within half the distance ahead.
        const int64_t turning =
            int64_t(norm(ahead.ahead_x, ahead.ahead_z)) * u.type.maximum_turn / (int64_t(left) * 2);
        limit = std::min<int64_t>(limit, std::max<int64_t>(turning, 0));
    }
    if (m.speed <= limit)
        return;
    m.speed = static_cast<Fixed>(limit);
    m.velocity = sim::unit_movement::aim_velocity(u.heading, 0, m.speed);
}

void steer_along(
    sim::unit_movement::Unit& u,
    sim::unit_movement::Movement& m,
    bool has_route,
    std::array<Point, 3> points,
    Fixed acceleration,
    Fixed deceleration,
    uint8_t sea_level,
    FastUnitSteering fast_units
) {
    Fixed adjustment = sub(0, deceleration);
    if (!has_route) {
        sim::unit_movement::turn(u, m, 0);
        sim::unit_movement::accelerate(u, m, adjustment, sea_level);
        return;
    }
    auto& target = points[1];
    const auto distance = norm(sub(target[0], u.position[0]), sub(target[2], u.position[2]));
    if (distance > lookahead_limit) {
        const auto dx = sub(target[0], points[0][0]), dz = sub(target[2], points[0][2]);
        const auto segment = norm(dx, dz);
        if (segment >= one_world_unit) {
            const auto amount = std::min(segment, sub(distance, lookahead_limit));
            const auto nx = low(int64_t(dx) * one_world_unit / segment),
                       nz = low(int64_t(dz) * one_world_unit / segment);
            target[0] = sub(target[0], low((int64_t(nx) * amount) >> 16));
            target[2] = sub(target[2], low((int64_t(nz) * amount) >> 16));
        }
    }
    const auto dx = sub(target[0], u.position[0]), dz = sub(target[2], u.position[2]);
    const auto heading = facing(sub(u.position[0], target[0]), sub(u.position[2], target[2]));
    const auto requested = short_bits(uint32_t(heading) - u.heading);
    const auto angle = abs32(requested);
    sim::unit_movement::turn(u, m, requested);
    const RouteAhead ahead{
        distance,
        dx,
        dz,
        sub(points[2][0], u.position[0]),
        sub(points[2][2], u.position[2]),
        angle,
        deceleration
    };
    const auto braking = sim::unit_movement::braking_distance(m.speed, deceleration);
    if (u.type.maximum_turn == 0 || !braking) {
        sim::unit_movement::accelerate(u, m, adjustment, sea_level);
        hold_to_route(u, m, ahead, fast_units);
        return;
    }
    const auto turn_distance =
        low(int64_t(m.speed) * (uint32_t(angle) & 0xffffu) / u.type.maximum_turn);
    const auto stop = low(*braking);
    if (mul(square_high(turn_distance), 4) < add(square_high(dx), square_high(dz))) {
        const auto end_x = sub(points[2][0], u.position[0]),
                   end_z = sub(points[2][2], u.position[2]);
        if (square_high(stop) < add(square_high(end_x), square_high(end_z)))
            adjustment = acceleration;
    }
    sim::unit_movement::accelerate(u, m, adjustment, sea_level);
    hold_to_route(u, m, ahead, fast_units);
}
} // namespace

void steer_ground(
    sim::unit_movement::Unit& u,
    sim::unit_movement::Movement& m,
    const Navigation& n,
    Fixed acceleration,
    Fixed deceleration,
    uint8_t sea_level,
    FastUnitSteering fast_units
) {
    const bool has_route = route_present(n) && n.count != 0 && count_fits(n);
    steer_along(
        u,
        m,
        has_route,
        has_route ? steering_points(n) : std::array<Point, 3>{},
        acceleration,
        deceleration,
        sea_level,
        fast_units
    );
}

void steer_ground(
    sim::unit_movement::Unit& u,
    sim::unit_movement::Movement& m,
    const MirroredNavigation& n,
    Fixed acceleration,
    Fixed deceleration,
    uint8_t sea_level,
    FastUnitSteering fast_units
) {
    const bool has_route = mirrored_route_present(n);
    steer_along(
        u,
        m,
        has_route,
        mirrored_steering_points(n),
        acceleration,
        deceleration,
        sea_level,
        fast_units
    );
}

} // namespace oa::sim::ground_orders
