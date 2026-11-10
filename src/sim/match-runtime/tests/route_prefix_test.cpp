// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "route_prefix.hpp"
#include "preferred_search_candidate.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>

namespace {
using oa::sim::ground_orders::RoutePoint;
using oa::sim::match_runtime::is_route_prefix;
using oa::sim::match_runtime::preferred_fallback_search;
using oa::sim::match_runtime::preferred_search_unit_is_live;
using oa::sim::match_runtime::same_preferred_search_goal;

void check(
    bool expected,
    std::initializer_list<RoutePoint> existing,
    std::initializer_list<RoutePoint> proposed
) {
    OA_CHECK(
        is_route_prefix(
            std::span(existing.begin(), existing.size()),
            std::span(proposed.begin(), proposed.size())
        ) == expected
    );
}

void straight_prefixes_and_changed_origins() {
    check(true, {{0, 0}, {10, 0}}, {{0, 0}, {10, 0}});
    check(true, {{-20, -20}, {10, 0}, {10, 10}}, {{4, 0}, {7, 0}});
    check(true, {{-20, -20}, {10, 0}, {10, 10}}, {{4, 3}, {10, 0}});
    check(true, {{-20, -20}, {10, 0}, {10, 10}}, {{4, 3}, {10, 0}, {10, 5}});
    check(false, {{-20, -20}, {10, 0}, {10, 10}}, {{4, 3}, {10, 5}});
    check(true, {{0, 0}, {10, 10}, {20, 10}}, {{3, 3}, {7, 7}});
    check(false, {{0, 0}, {10, 10}, {20, 10}}, {{3, 3}, {7, 8}});
    check(false, {{0, 0}, {10, 0}}, {{0, 0}, {11, 0}});
    check(false, {{0, 0}, {10, 0}}, {{0, 0}, {-1, 0}});
    check(false, {{0, 0}, {10, 0}}, {{0, 0}, {7, 0}, {6, 0}});
}

void subdivisions_duplicates_and_bends() {
    check(true, {{0, 0}, {4, 0}, {8, 0}, {8, 8}}, {{0, 0}, {6, 0}});
    check(true, {{0, 0}, {4, 0}, {8, 0}, {8, 8}}, {{0, 0}, {8, 0}, {8, 4}});
    check(true, {{0, 0}, {8, 0}, {8, 8}}, {{0, 0}, {2, 0}, {6, 0}, {8, 0}, {8, 4}});
    check(true, {{0, 0}, {0, 0}, {4, 0}, {4, 0}, {8, 0}}, {{0, 0}, {0, 0}, {6, 0}, {6, 0}});
    check(true, {{0, 0}, {0, 0}, {0, 0}}, {{0, 0}, {0, 0}});
    check(true, {{0, 0}, {8, 0}, {8, 8}}, {{0, 0}, {0, 0}});
    check(true, {{0, 0}, {8, 0}, {8, 8}}, {{8, 0}, {8, 4}});
    check(true, {{0, 0}, {8, 0}}, {{0, 0}, {8, 0}, {8, 0}});
    check(false, {{0, 0}, {8, 0}}, {{0, 0}, {8, 0}, {9, 0}});
    check(false, {{0, 0}, {8, 0}, {8, 8}}, {{0, 0}, {8, 4}});
    check(false, {{0, 0}, {8, 0}, {8, 8}}, {{0, 0}, {8, 8}});
    check(false, {{0, 0}, {8, 0}, {4, 0}, {12, 0}}, {{0, 0}, {12, 0}});
    check(true, {{0, 0}, {8, 0}, {4, 0}, {12, 0}}, {{0, 0}, {8, 0}, {6, 0}});
    check(false, {{0, 0}, {8, 0}, {4, 0}, {12, 0}}, {{0, 0}, {8, 0}, {9, 0}});
}

void malformed_counts_and_coordinate_bounds() {
    const std::array<RoutePoint, 2> route{{{0, 0}, {8, 0}}};
    OA_CHECK(!is_route_prefix({}, route));
    OA_CHECK(!is_route_prefix(route, {}));
    OA_CHECK(!is_route_prefix(std::span(route).first(1), route));
    OA_CHECK(!is_route_prefix(route, std::span(route).first(1)));
    std::array<RoutePoint, 21> oversized{};
    OA_CHECK(!is_route_prefix(oversized, route));
    OA_CHECK(!is_route_prefix(route, oversized));
    std::array<RoutePoint, 20> full{};
    for (size_t i = 0; i < full.size(); ++i)
        full[i] = {static_cast<int16_t>(i), 0};
    OA_CHECK(is_route_prefix(full, full));
    OA_CHECK(is_route_prefix(full, std::array<RoutePoint, 2>{{{0, 0}, {19, 0}}}));
    constexpr auto low = std::numeric_limits<int16_t>::min();
    constexpr auto high = std::numeric_limits<int16_t>::max();
    check(true, {{low, low}, {high, high}}, {{low, low}, {0, 0}, {high, high}});
    check(false, {{low, low}, {high, high}}, {{low, low}, {high, high - 1}});
    check(true, {{low, high}, {high, low}}, {{low, high}, {-1, 0}});
    check(false, {{low, high}, {high, low}}, {{low, high}, {0, 0}});
    check(true, {{low, low}, {high, low}, {high, high}}, {{low, low}, {0, low}});
    check(false, {{low, low}, {high, low}, {high, high}}, {{low, low}, {high, high}});
}

void saved_battle_routes_keep_their_existing_backbone() {
    // Fixed-goal Core Prime battle, tick1800: mature blocked actors with active
    // passing heads. A shorter guide reaches the same first corner; its changed
    // point count must not make it supersede that temporary passing movement.
    constexpr std::array<RoutePoint, 7> first{
        {{1655, 4246},
         {1680, 3216},
         {1808, 3088},
         {6032, 3088},
         {6096, 3152},
         {7696, 3152},
         {8640, 4208}}
    };
    constexpr std::array<RoutePoint, 2> first_prefix{{{1653, 4235}, {1680, 3216}}};
    OA_CHECK(is_route_prefix(first, first_prefix));
    OA_CHECK(!is_route_prefix(first, std::array<RoutePoint, 2>{{{1653, 4235}, {1808, 3088}}}));
    constexpr std::array<RoutePoint, 3> second{{{5548, 1625}, {4560, 1616}, {3568, 2192}}};
    constexpr std::array<RoutePoint, 2> second_prefix{{{5544, 1631}, {4560, 1616}}};
    OA_CHECK(is_route_prefix(second, second_prefix));
    auto after_save = first;
    after_save[0] = {0, 0};
    OA_CHECK(is_route_prefix(after_save, first_prefix));
}

void captured_stationary_fallbacks_keep_their_native_search_eligibility() {
    oa::Unit actor{};
    actor.type_index = 1;
    actor.movement = 1;
    actor.health = 1;
    actor.flags = OA_UNIT_FLAG_LIVE;
    OA_CHECK(preferred_search_unit_is_live(actor));
    actor.flags |= OA_UNIT_FLAG_DEATH_PENDING;
    OA_CHECK(!preferred_search_unit_is_live(actor));
    actor.flags = OA_UNIT_FLAG_LIVE;
    actor.health = 0;
    OA_CHECK(!preferred_search_unit_is_live(actor));
    actor.health = 1;
    actor.flags = 0;
    OA_CHECK(!preferred_search_unit_is_live(actor));
    actor.flags = OA_UNIT_FLAG_LIVE;
    actor.movement = 0;
    OA_CHECK(!preferred_search_unit_is_live(actor));
    actor.movement = 1;
    actor.type_index = 0;
    OA_CHECK(!preferred_search_unit_is_live(actor));
    oa::sim::simulation_state::Order order;
    oa::sim::ground_orders::Goal goal;
    goal.order = &order;
    goal.cell = {312, 336};
    goal.tolerance = 180;
    goal.radius_squared = 121;
    oa::sim::ground_orders::Navigation navigation;
    navigation.goal = &goal;
    navigation.flags = oa::sim::ground_orders::search_pending_flag;
    oa::sim::unit_movement::Movement movement;
    oa::sim::unit_movement::LocalFlowHead traffic;

    struct Captured {
        uint32_t last_motion, last_search, count;
        int32_t desired_speed;
    };

    // Retained 20,000-unit tick9000 cases14797/15141/15639. The final
    // actor's desired speed remains nonzero while its position is blocked.
    for (const auto captured : {
             Captured{7216, 6814, 1, 0},
             Captured{7291, 7270, 1, 0},
             Captured{8375, 8227, 2, 58982},
         }) {
        movement.last_motion_tick = captured.last_motion;
        movement.speed = captured.desired_speed;
        navigation.last_search_tick = captured.last_search;
        navigation.count = captured.count;
        OA_CHECK(preferred_fallback_search(navigation, movement, traffic, 9000));
    }
    const auto retained = goal;
    OA_CHECK(same_preferred_search_goal(goal, retained));
    goal.cell[0] += 1;
    OA_CHECK(!same_preferred_search_goal(goal, retained));
    goal = retained;
    goal.tolerance += 16;
    OA_CHECK(!same_preferred_search_goal(goal, retained));
    goal = retained;
    oa::sim::simulation_state::Order replacement;
    goal.order = &replacement;
    OA_CHECK(!same_preferred_search_goal(goal, retained));
    goal = retained;
    traffic.mobile_blocked = true;
    OA_CHECK(!preferred_fallback_search(navigation, movement, traffic, 9000));
    traffic.mobile_blocked = false;
    movement.last_motion_tick = 8999;
    OA_CHECK(!preferred_fallback_search(navigation, movement, traffic, 9000));
    movement.last_motion_tick = 8375;
    navigation.last_search_tick = 8999;
    OA_CHECK(!preferred_fallback_search(navigation, movement, traffic, 9000));
    navigation.last_search_tick = 8227;
    navigation.flags = 0;
    OA_CHECK(!preferred_fallback_search(navigation, movement, traffic, 9000));
    navigation.flags = oa::sim::ground_orders::search_pending_flag;
    goal.order = nullptr;
    OA_CHECK(!preferred_fallback_search(navigation, movement, traffic, 9000));
    goal.order = &order;
    navigation.goal = nullptr;
    OA_CHECK(!preferred_fallback_search(navigation, movement, traffic, 9000));
    navigation.goal = &goal;
    movement.last_motion_tick = std::numeric_limits<uint32_t>::max() - 100;
    navigation.last_search_tick = std::numeric_limits<uint32_t>::max() - 200;
    OA_CHECK(preferred_fallback_search(navigation, movement, traffic, 20));
}
} // namespace

int main() {
    straight_prefixes_and_changed_origins();
    subdivisions_duplicates_and_bends();
    malformed_counts_and_coordinate_bounds();
    saved_battle_routes_keep_their_existing_backbone();
    captured_stationary_fallbacks_keep_their_native_search_eligibility();
    return oa::test::check_exit_status();
}
