// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "route_queue.hpp"
#include "oa/test/check.hpp"
#include <array>
#include <cstdint>
#include <limits>

namespace {
namespace movement = oa::sim::unit_movement;
namespace ground = oa::sim::ground_orders;
using oa::sim::match_runtime::same_forward_route;
constexpr int32_t pixel = 65536;

struct Queue {
    movement::Unit follower{}, leader{};
    ground::Goal goal{}, leader_goal{};
    ground::Navigation route{}, leader_route{};

    Queue() {
        follower.position = {48 * pixel, 0, 48 * pixel};
        leader.position = {80 * pixel, 0, 48 * pixel};
        follower.footprint = leader.footprint = {2, 2};
        goal.cell = leader_goal.cell = {10, 10};
        route.goal = &goal;
        leader_route.goal = &leader_goal;
        route.count = leader_route.count = 3;
        route.flags = leader_route.flags = ground::route_present_flag;
        route.points = {{{48, 48}, {160, 48}, {160, 160}}};
        leader_route.points = {{{80, 48}, {160, 48}, {160, 160}}};
    }

    bool follows() const { return same_forward_route(follower, route, leader, leader_route); }
};

void shared_native_route_requires_a_leader_ahead() {
    Queue queue;
    OA_CHECK(queue.follows());
    queue.leader.heading = 32768;
    OA_CHECK(queue.follows());
    queue.leader.position[0] = 32 * pixel;
    OA_CHECK(!queue.follows());
    queue.leader.position = queue.follower.position;
    OA_CHECK(!queue.follows());
    queue.leader.position = {80 * pixel, 0, 200 * pixel};
    OA_CHECK(!queue.follows());
    queue.leader.position = {80 * pixel, 0, 48 * pixel};
    queue.leader.footprint = {1, 2};
    OA_CHECK(!queue.follows());
    queue.leader.footprint = queue.follower.footprint;
    queue.follower.position = {0, 0, 0};
    queue.leader.position = {24 * pixel, 0, 0};
    queue.route.points[1] = queue.leader_route.points[1] = {16, 0};
    OA_CHECK(!queue.follows());
}

void physical_headings_never_replace_route_identity() {
    Queue queue;
    queue.follower.heading = queue.leader.heading = 0;
    queue.leader_route.points[1] = {0, 48};
    OA_CHECK(!queue.follows());
    queue.leader_route.points[1] = queue.route.points[1];
    queue.leader_route.points[2] = {160, 0};
    OA_CHECK(!queue.follows());
    queue.leader_route.points[2] = queue.route.points[2];
    OA_CHECK(queue.follows());
}

void different_destinations_share_only_the_current_prefix() {
    Queue queue;
    queue.leader_goal.cell[0] += 1;
    OA_CHECK(queue.follows());
    queue.leader_goal.tolerance = 1;
    OA_CHECK(queue.follows());
    queue.leader_goal.shape = ground::GoalShape::ring;
    OA_CHECK(queue.follows());
    queue.leader_goal.inner_range = 1;
    queue.leader_goal.bottom = 1;
    OA_CHECK(queue.follows());
    queue.leader_route.points[2] = {160, 0};
    OA_CHECK(!queue.follows());
    queue.leader_route.points[2] = queue.route.points[2];
    queue.leader_route.goal = nullptr;
    OA_CHECK(!queue.follows());
}

void a_leader_beyond_the_shared_corner_is_not_admitted() {
    Queue queue;
    queue.follower.position = {144 * pixel, 0, 48 * pixel};
    queue.leader.position = {160 * pixel, 0, 64 * pixel};
    queue.leader_route.points = {{{160, 48}, {160, 160}, {256, 160}}};
    OA_CHECK(queue.leader_route.points[0] == queue.route.points[1]);
    OA_CHECK(queue.leader_route.points[1] == queue.route.points[2]);
    OA_CHECK(!queue.follows());
}

void incomplete_routes_and_extreme_vectors_do_not_form_queues() {
    Queue queue;
    queue.leader_route.goal = nullptr;
    OA_CHECK(!queue.follows());
    queue.leader_route.goal = &queue.leader_goal;
    queue.leader_route.flags = ground::route_changed_flag;
    OA_CHECK(!queue.follows());
    queue.leader_route.flags = ground::route_present_flag;
    queue.leader_route.count = 1;
    OA_CHECK(!queue.follows());
    queue.leader_route.count = ground::navigation_capacity + 1;
    OA_CHECK(!queue.follows());
    queue.leader_route.count = 3;
    queue.follower.position[0] = std::numeric_limits<int32_t>::min();
    queue.leader.position[0] = std::numeric_limits<int32_t>::max();
    OA_CHECK(!queue.follows());
}

void direct_routes_repeat_their_destination() {
    Queue queue;
    queue.route.count = queue.leader_route.count = 2;
    queue.route.points[2] = {0, 0};
    queue.leader_route.points[2] = {1, 1};
    OA_CHECK(queue.follows());
    queue.route.count = 3;
    OA_CHECK(!queue.follows());
}
} // namespace

int main() {
    shared_native_route_requires_a_leader_ahead();
    physical_headings_never_replace_route_identity();
    different_destinations_share_only_the_current_prefix();
    a_leader_beyond_the_shared_corner_is_not_admitted();
    incomplete_routes_and_extreme_vectors_do_not_form_queues();
    direct_routes_repeat_their_destination();
    return oa::test::check_exit_status();
}
