// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "fixture.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)

/// An order the scheduler removes because its handler ended it is reported
/// before it is destroyed, and a result that keeps the order is not.
void ended_orders_are_reported() {
    TestWorld tables(2, 2);
    auto& world = *tables;
    Fixture listening;
    Fixture quiet;
    auto& unit = tables.units[1];
    unit.def = oa::oa_ref_from_index(1);
    auto& player = tables.player(0);
    player.in_use = 1;
    player.status = 1;
    unit.owner = oa::world_player_ref_of(&world, &player);
    auto& queue = tables.orders[1];
    world.game.tick = 100;
    listening.order_ends.context = &listening;
    listening.order_ends.ended = [](void* context, const Unit&, const Order& ended, bool done) {
        static_cast<Fixture*>(context)->calls.push_back(
            "ended:" + std::to_string(ended.kind) + ":" + (done ? "1" : "0")
        );
    };

    struct Outcome {
        StepFault fault{};
        bool empty{};
        uint8_t head_kind{};
        bool waiting{};
        bool retry{};
        std::vector<std::string> calls;
    };

    // A budget of eight steps is enough for one removal and the order that
    // follows it. A result that keeps its order spends the budget.
    const auto drive = [&](Fixture& host, uint32_t result, int count, uint8_t kind) {
        Order orders[3]{};
        for (int index = 0; index < count; ++index)
            orders[index].kind = static_cast<uint8_t>(kind + index);
        queue.primary = &orders[0];
        for (int index = 1; index < count; ++index)
            orders[index - 1].next = &orders[index];
        host.calls.clear();
        host.random_value = 4;
        host.handler = [result](Unit&, Order&) { return result; };
        const auto fault = primary_orders(world, queue, unit, host, 8);
        const Outcome outcome{
            fault,
            queue.primary == nullptr,
            queue.primary != nullptr ? queue.primary->kind : uint8_t{0},
            queue.primary != nullptr && queue.primary->wait_events != 0,
            queue.primary != nullptr && (queue.primary->flags & 0x80) != 0,
            host.calls
        };
        queue.primary = nullptr;
        host.handler = {};
        return outcome;
    };
    const auto ended_lines = [](const std::vector<std::string>& calls) {
        std::vector<std::string> lines;
        for (const auto& call : calls)
            if (call.starts_with("ended:"))
                lines.push_back(call);
        return lines;
    };
    const auto without_ended = [](const std::vector<std::string>& calls) {
        std::vector<std::string> lines;
        for (const auto& call : calls)
            if (!call.starts_with("ended:"))
                lines.push_back(call);
        return lines;
    };
    const auto same_queue = [](const Outcome& left, const Outcome& right) {
        return left.fault == right.fault && left.empty == right.empty &&
               left.head_kind == right.head_kind && left.waiting == right.waiting &&
               left.retry == right.retry;
    };
    const auto ended_before_destroy = [](const std::vector<std::string>& calls, const char* line) {
        const auto ended_at = std::find(calls.begin(), calls.end(), line);
        const auto destroy_at = std::find(calls.begin(), calls.end(), "destroy");
        return ended_at != calls.end() && destroy_at != calls.end() && ended_at < destroy_at;
    };

    constexpr uint8_t kind = 11;
    for (const uint32_t result : {0u, 1u, 2u, 3u, 4u, 6u}) {
        const auto heard = drive(listening, result, 1, kind);
        const auto silent = drive(quiet, result, 1, kind);
        CHECK(ended_lines(heard.calls).empty());
        CHECK(!heard.empty);
        CHECK(same_queue(heard, silent));
        CHECK(without_ended(heard.calls) == silent.calls);
    }
    {
        const auto heard = drive(listening, 5, 1, kind);
        const auto silent = drive(quiet, 5, 1, kind);
        CHECK(heard.empty);
        CHECK((heard.calls == std::vector<std::string>{"dispatch", "ended:11:1", "destroy"}));
        CHECK(ended_before_destroy(heard.calls, "ended:11:1"));
        CHECK(same_queue(heard, silent));
        CHECK(without_ended(heard.calls) == silent.calls);
    }
    {
        const auto heard = drive(listening, 8, 1, kind);
        const auto silent = drive(quiet, 8, 1, kind);
        CHECK(heard.empty);
        CHECK((heard.calls == std::vector<std::string>{"dispatch", "ended:11:0", "destroy"}));
        CHECK(ended_before_destroy(heard.calls, "ended:11:0"));
        CHECK(same_queue(heard, silent));
        CHECK(without_ended(heard.calls) == silent.calls);
    }
    {
        const auto heard = drive(listening, 9, 2, kind);
        const auto silent = drive(quiet, 9, 2, kind);
        CHECK(!heard.empty && heard.head_kind == static_cast<uint8_t>(kind + 1) && heard.waiting);
        CHECK((ended_lines(heard.calls) == std::vector<std::string>{"ended:11:0"}));
        CHECK(ended_before_destroy(heard.calls, "ended:11:0"));
        CHECK(same_queue(heard, silent));
        CHECK(without_ended(heard.calls) == silent.calls);
    }
    {
        const auto heard = drive(listening, 9, 1, kind);
        const auto silent = drive(quiet, 9, 1, kind);
        CHECK(ended_lines(heard.calls).empty());
        CHECK(!heard.empty && heard.waiting && heard.retry && heard.head_kind == kind);
        CHECK(same_queue(heard, silent));
        CHECK(without_ended(heard.calls) == silent.calls);
    }
    {
        const auto heard = drive(listening, 7, 3, kind);
        const auto silent = drive(quiet, 7, 3, kind);
        CHECK(heard.empty);
        CHECK(
            (heard.calls ==
             std::vector<std::string>{"dispatch", "ended:11:0", "destroy", "destroy", "destroy"})
        );
        CHECK(ended_before_destroy(heard.calls, "ended:11:0"));
        CHECK(std::count(heard.calls.begin(), heard.calls.end(), "ended:11:0") == 1);
        CHECK(same_queue(heard, silent));
        CHECK(without_ended(heard.calls) == silent.calls);
    }
}

int main() {
    Player inactive{};
    CHECK(!player_active(inactive));
    Player human{};
    human.in_use = 1;
    human.status = 1;
    human.index = 0;
    std::array<int32_t, 3> centroid{7, 7, 7};
    CHECK(!selection_centroid({}, centroid));
    CHECK(centroid[0] == 7);
    const std::array<std::array<uint32_t, 3>, 2> positions{{
        {0x00020000u, 0xffff0000u, 0x00010000u},
        {0x00040000u, 0x00010000u, 0x00030000u},
    }};
    CHECK(selection_centroid(positions, centroid));
    CHECK(centroid[0] == 0x00030000 && centroid[1] == 0 && centroid[2] == 0x00020000);
    std::array<int32_t, 3> picked{9, 9, 9};
    CHECK(!priority_selection_centroid({}, {}, {}, picked));
    CHECK(picked[0] == 9 && picked[1] == 9 && picked[2] == 9);
    const std::array<std::array<uint32_t, 3>, 1> third_choice{{
        {0x00080000u, 0x00050000u, 0x00060000u},
    }};
    CHECK(priority_selection_centroid({}, {}, third_choice, picked));
    CHECK(picked[0] == 0x00080000 && picked[1] == 0x00050000 && picked[2] == 0x00060000);
    CHECK(priority_selection_centroid({}, positions, third_choice, picked));
    CHECK(picked[0] == 0x00030000 && picked[1] == 0 && picked[2] == 0x00020000);
    const std::array<std::array<uint32_t, 3>, 1> first_choice{{
        {0x00010000u, 0xfffe0000u, 0x00070000u},
    }};
    CHECK(priority_selection_centroid(first_choice, positions, third_choice, picked));
    CHECK(picked[0] == 0x00010000 && picked[1] == (-2 << 16) && picked[2] == 0x00070000);
    const std::array<uint16_t, 6> sight{{0, 0x0001, 0, 0x0004, 0, 0}};
    const std::array<uint32_t, 3> cell_10{{0x00200000u, 0, 0}};
    CHECK(viewpoint_sees_point(2, 3, sight, 0, cell_10));
    CHECK(!viewpoint_sees_point(2, 3, sight, 1, cell_10));
    const std::array<uint32_t, 3> cell_11{{0x00200000u, 0, 0x00200000u}};
    CHECK(viewpoint_sees_point(2, 3, sight, 2, cell_11));
    const std::array<uint32_t, 3> raised{{0x00200000u, 0x00400000u, 0x00200000u}};
    CHECK(viewpoint_sees_point(2, 3, sight, 0, raised));
    CHECK(!viewpoint_sees_point(2, 3, sight, 2, raised));
    const std::array<uint32_t, 3> dropped{{0x00200000u, 0xffc00000u, 0}};
    CHECK(viewpoint_sees_point(2, 3, sight, 2, dropped));
    const std::array<uint32_t, 3> outside{{0x00400000u, 0, 0}};
    CHECK(!viewpoint_sees_point(2, 3, sight, 0, outside));
    const std::array<uint32_t, 3> negative{{0xffe00000u, 0, 0}};
    CHECK(!viewpoint_sees_point(2, 3, sight, 0, negative));
    const std::array<uint8_t, 6> coverage{{0, 5, 0, 0, 0, 0x80}};
    CHECK(point_visible(0, 2, 3, {}, sight, 0, cell_10));
    CHECK(point_visible(1, 2, 3, {}, sight, 0, cell_10));
    CHECK(!point_visible(4, 2, 3, {}, sight, 1, cell_10));
    CHECK(point_visible(0, 2, 3, coverage, sight, 2, cell_11));
    CHECK(point_visible(2, 2, 3, coverage, {}, 0, cell_10));
    CHECK(point_visible(3, 2, 3, coverage, {}, 0, raised));
    const std::array<uint32_t, 3> coverage_high{{0x00200000u, 0, 0x00400000u}};
    CHECK(point_visible(6, 2, 3, coverage, {}, 0, coverage_high));
    CHECK(!point_visible(2, 2, 3, coverage, sight, 2, dropped));
    CHECK(point_visible(0, 2, 3, coverage, sight, 2, dropped));
    CHECK(!point_visible(2, 2, 3, coverage, sight, 2, cell_11));
    CHECK(!point_visible(2, 2, 3, coverage, {}, 0, outside));
    CHECK(!point_visible(2, 2, 3, {}, sight, 0, outside));
    CHECK(!point_visible(2, 2, 3, coverage, {}, 0, negative));
    Order primary_move;
    Order primary_stop;
    Order secondary_weapon;
    primary_move.kind = 26;
    primary_stop.kind = 45;
    secondary_weapon.kind = 13;
    primary_move.next = &primary_stop;
    const OrderQueue seeker{&primary_move, &secondary_weapon};
    CHECK(find_order(seeker, 45) == &primary_stop);
    CHECK(find_order(seeker, 13) == &secondary_weapon);
    CHECK(find_order(seeker, 38) == nullptr);
    TestWorld marks(1, 1);
    auto* marked = (*marks).game.players;
    marked[0].in_use = 1;
    marked[0].status = 1;
    marked[0].machine_group = 1;
    marked[1].in_use = 1;
    marked[1].status = 2;
    marked[1].machine_group = 2;
    CHECK(lowest_unused_player_mark(*marks) == 3);
    for (std::size_t i = 0; i < 10; ++i) {
        marked[i].in_use = 1;
        marked[i].status = 1;
    }
    for (int32_t mark = 1; mark <= 10; ++mark)
        marked[static_cast<std::size_t>(mark - 1)].machine_group = static_cast<uint32_t>(mark);
    CHECK(lowest_unused_player_mark(*marks) == 0);
    CHECK(player_slot_active(0, human));
    CHECK(!player_slot_active(10, human));
    human.index = 10;
    CHECK(!player_active(human));
    // Slot 1 ghost, 2-3 player1, 4 ally, 5-8 rejected, 9 defeated, 10 closer.
    TestWorld near(11, 1);
    auto& units = near.units;
    auto& ghost = units[1];
    ghost.flags = OA_UNIT_FLAG_LIVE;
    units[2].flags = 0x10000003;
    units[2].position = {3 << 16, -1, 0};
    units[3].flags = 0x10000003;
    units[3].position = {2 << 16, 0, 0};
    auto& ally = units[4];
    ally.flags = OA_UNIT_FLAG_LIVE;
    units[6].flags = 0x10000002;
    units[7].flags = 0x10008000;
    units[8].flags = OA_UNIT_FLAG_LIVE;
    units[8].state_flags = 4;
    units[9].flags = OA_UNIT_FLAG_LIVE;
    auto& closer = units[10];
    closer.flags = 0x10000001;
    closer.position = {1 << 16, 0x7fffffff, 0};
    near.player(0).status = 1;
    near.range(0, 1, 1);
    near.player(1).in_use = 1;
    near.player(1).status = 1;
    near.player(1).index = 4;
    near.range(1, 2, 2);
    near.player(2).in_use = 1;
    near.player(2).status = 2;
    near.player(2).index = 2;
    near.range(2, 4, 1);
    near.player(3).in_use = 1;
    near.player(3).status = 3;
    near.player(3).index = 3;
    near.range(3, 5, 4);
    near.player(4).in_use = 1;
    near.player(4).status = 1;
    near.player(4).index = 10;
    near.range(4, 9, 1);
    near.player(5).in_use = 1;
    near.player(5).status = 1;
    near.range(5, 10, 1);
    std::array<uint8_t, 11> relation{};
    relation[1] = 1;
    relation[2] = 1;
    relation[5] = 1;
    CHECK(nearest_candidate_unit(*near, relation, 0, 0) == &closer);
    relation[0] = 1;
    CHECK(nearest_candidate_unit(*near, relation, 0, 0) == &units[3]);
    relation[4] = 9;
    TestWorld empty(1, 1);
    CHECK(nearest_candidate_unit(*empty, relation, 0, 0) == nullptr);
    CHECK(nearest_candidate_unit(*near, relation, 0, 0) == nullptr);
    const std::array<uint8_t, 1> open{0};
    TestWorld one(3, 1);
    one.player(0).in_use = 1;
    one.player(0).status = 1;
    one.range(0, 1, 2);
    one.units[1].flags = OA_UNIT_FLAG_LIVE;
    one.units[1].position = {0x00018000, 0, 0};
    one.units[2].flags = OA_UNIT_FLAG_LIVE;
    one.units[2].position = {0x00010000, 0, 0};
    CHECK(nearest_candidate_unit(*one, open, 0, 0) == &one.units[2]);
    one.units[1].position = {2 << 16, 0, 0};
    one.units[2].position = {0, 0, -(2 << 16)};
    CHECK(nearest_candidate_unit(*one, open, 0, 0) == &one.units[1]);
    one.units[1].position = {0, 0, 0};
    one.units[2].position = {1 << 16, 0, 0};
    const auto edge = static_cast<int32_t>(0x80000000u);
    CHECK(nearest_candidate_unit(*one, open, edge, edge) == &one.units[1]);
    {
        // The candidate filter: widened flags pass over occupancy states 2 and 3 and
        // move-rate tiers 2 and 3; skip_submerged passes over water state 3.
        TestWorld filtered(4, 1);
        filtered.player(0).in_use = 1;
        filtered.player(0).status = 1;
        filtered.range(0, 1, 3);
        for (uint32_t i = 1; i <= 3; ++i) {
            filtered.units[i].flags = OA_UNIT_FLAG_LIVE;
            filtered.units[i].position = {static_cast<int32_t>(i) << 16, 0, 0};
        }
        filtered.units[1].flags |= OA_UNIT_FLAG_OCCUPANCY_MASK; // occupancy state 3
        const uint32_t submerged = 3;
        std::memcpy(filtered.units[2].last_occupy_code, &submerged, sizeof submerged);
        const auto nearest = [&](bool widened, bool skip_submerged) {
            CandidateFilter filter{};
            filter.widened_flags = widened;
            filter.skip_submerged = skip_submerged;
            return nearest_candidate_unit(*filtered, open, 0, 0, filter);
        };
        CHECK(nearest(false, false) == &filtered.units[1]);
        CHECK(nearest(false, true) == &filtered.units[1]);
        CHECK(nearest(true, false) == &filtered.units[2]);
        CHECK(nearest(true, true) == &filtered.units[3]);
        constexpr uint32_t move_rate_tier_1 = 0x4u, move_rate_tier_2 = 0x8u;
        filtered.units[3].flags |= move_rate_tier_2;
        CHECK(nearest(true, true) == nullptr);
        CHECK(nearest(false, true) == &filtered.units[1]);
        filtered.units[1].flags = OA_UNIT_FLAG_LIVE | move_rate_tier_1;
        CHECK(nearest(true, true) == &filtered.units[1]);
        filtered.units[1].flags = OA_UNIT_FLAG_LIVE | OA_UNIT_FLAG_NOT_SELECTABLE;
        CHECK(nearest(true, false) == &filtered.units[2]);
    }
    TestWorld tw(2, 2);
    auto& w = *tw;
    Fixture h;
    auto& t = tw.defs[1];
    t.max_damage = 100;
    auto& u = tw.units[1];
    u.def = oa::oa_ref_from_index(1);
    auto& p = tw.player(0);
    p.in_use = 1;
    p.status = 1;
    u.owner = oa::world_player_ref_of(&w, &p);
    auto& q = tw.orders[1];
    Order a, b, c;
    q.primary = &a;
    a.next = &b;
    b.next = &c;
    rotate_primary(q, b);
    CHECK(q.primary == &a && a.next == &c && c.next == &b && !b.next);
    remove_order(q, u, c, h);
    CHECK(a.next == &b && (c.flags & 1));
    a.preserve_flags = 4;
    clear_orders(q, u, false, h);
    CHECK(q.primary == &a && !a.next);
    clear_orders(q, u, true, h);
    CHECK(!q.primary);
    a = {};
    q.primary = &a;
    w.game.tick = 100;
    a.wake_tick = 200;
    a.wait_events = 2;
    u.events = 4;
    h.calls.clear();
    primary_orders(w, q, u, h);
    CHECK(h.calls.empty() && u.events == 4);
    a.wait_events = 0x10002;
    a.raised_events = 0x10000;
    u.events = 6;
    h.random_value = 4;
    primary_orders(w, q, u, h);
    CHECK(a.wait_events == 1 && a.wake_tick == 134 && u.events == 4);
    CHECK(h.events.back() == 0x10002);
    CHECK(
        h.calls ==
        std::vector<std::string>({"weapon0", "weapon1", "weapon2", "dispatch", "random15"})
    );
    h.calls.clear();
    w.game.tick = 134;
    primary_orders(w, q, u, h);
    CHECK(h.events.back() == 1 && a.wake_tick == 168);
    a = {};
    b = {};
    a.flags = 4;
    b.flags = 4;
    a.next = &b;
    q.secondary = &a;
    a.wait_events = 1;
    a.wake_tick = 500;
    b.wait_events = 1;
    b.wake_tick = 100;
    h.calls.clear();
    secondary_orders(w, q, u, h);
    CHECK(a.wake_tick == 500 && b.wake_tick == 168);
    h.handler = [](Unit&, Order&) { return 7; };
    w.game.tick = 500;
    secondary_orders(w, q, u, h);
    CHECK(q.secondary == &b);
    h.handler = {};
    q.primary = nullptr;
    q.secondary = nullptr;
    w.game.tick = 240;
    u.health = 25;
    u.health_percent = 33;
    u.damage_countdown = 1;
    u.capture_cooldown = 2;
    u.script = 1;
    u.flags = 0x10;
    h.calls.clear();
    update_unit(w, q, u, h);
    CHECK(
        u.health_percent == 25 && u.previous_health_percent == 33 && !u.damage_countdown &&
        u.capture_cooldown == 1 && !u.flags
    );
    CHECK(h.calls == std::vector<std::string>({"script"}));
    // The signed health * 100 is divided as an unsigned word, and the clamp comes after
    // the division.
    u.health = -1;
    update_unit(w, q, u, h);
    CHECK(u.health_percent == 100);
    u.movement = 1;
    u.flags = 0x10001;
    t.flags = 0x101000;
    t.water_line = 5;
    w.game.sea_level = 20;
    h.terrain = 30;
    h.calls.clear();
    update_height(w, u, h);
    CHECK(static_cast<uint32_t>(u.position.y) == 30u * 65536 && u.flags == 1);
    CHECK(h.calls == std::vector<std::string>({"terrain", "terrain"}));
    t.flags = 0x80000;
    u.flags = 0x10001;
    update_height(w, u, h);
    CHECK(static_cast<uint32_t>(u.position.y) == 15u * 65536);
    q.primary = &a;
    a = {};
    h.handler = [](Unit&, Order&) { return 2; };
    CHECK(primary_orders(w, q, u, h, 4) == StepFault::order_budget_spent);
    q.primary = nullptr;
    h.handler = {};
    u.owner = 0;
    u.type_index = 1;
    u.movement = 0;
    u.script = 0;
    u.flags = 0;
    p.in_use = 1;
    p.status = 3;
    tw.range(0, 1, 1);
    w.game.periodic_flags |= 2;
    w.game.periodic_countdown = 1;
    h.calls.clear();
    update_units(w, tw.orders, h);
    CHECK(w.game.active_unit_count == 1 && w.game.periodic_countdown == 90);
    CHECK(h.calls == std::vector<std::string>({"pre", "key", "periodic1", "periodic2"}));
    PlacementGrids grids;
    grids.step_x = 1;
    grids.base = 99;
    grids.alt_base = 99;
    h.calls.clear();
    h.random_values = {4, 1, 7, 2, 9, 0, 5, 3};
    seed_placement_grids(grids, h);
    CHECK(grids.base == 3 && grids.alt_base == 6);
    CHECK(grids.step_x == 15 && grids.step_y == 12);
    CHECK(grids.phase_x == 0 && grids.phase_y == -4);
    CHECK(grids.alt_step_x == 23 && grids.alt_step_y == 14);
    CHECK(grids.alt_phase_x == -6 && grids.alt_phase_y == -4);
    CHECK(
        h.calls == std::vector<std::string>(
                       {"random10",
                        "random3",
                        "random15",
                        "random12",
                        "random20",
                        "random3",
                        "random23",
                        "random14"}
                   )
    );
    // Steps keep the low 16 bits of roll+base+8. A negative step divides by 2
    // toward zero. The phase subtract happens in 32 bits before the low-word store.
    h.calls.clear();
    h.random_cursor = 0;
    h.random_values = {0x0001fffeu, 0, 1, 0, 0xfffcu, 0, 0x10005u, 0};
    seed_placement_grids(grids, h);
    CHECK(grids.step_x == 9 && grids.step_y == 11);
    CHECK(grids.phase_x == static_cast<int16_t>(1 - 9 / 2));
    CHECK(grids.phase_y == static_cast<int16_t>(0 - 11 / 2));
    CHECK(grids.alt_step_x == 10 && grids.alt_step_y == 14);
    CHECK(grids.alt_phase_x == 0);
    CHECK(grids.alt_phase_y == static_cast<int16_t>(0 - 14 / 2));
    h.calls.clear();
    h.random_cursor = 0;
    h.random_values = {0xfff0u, 0, 1, 0, 0, 0, 0, 0};
    seed_placement_grids(grids, h);
    CHECK(grids.step_x == -5);
    CHECK(grids.phase_x == 3);
    CHECK(h.calls[2] == "random4294967291");

    // On a map whose water does damage, every 30 ticks each unit the local
    // machine simulates at or under the sea takes the map's water damage as
    // kind 11: a submarine on the floor and a ship on the water do, and a
    // hovercraft does not.
    {
        constexpr uint32_t water_damage_kind = 11;
        TestWorld sea(4, 4);
        auto& world = *sea;
        world.game.sea_level = 20;
        world.environment_enabled = 1;
        world.environment_damage = 7;
        auto& owner = sea.player(0);
        owner.in_use = 1;
        owner.status = 1;
        sea.range(0, 1, 3);
        std::array<OrderQueue, 4> queues{};
        const std::array<std::pair<uint32_t, int32_t>, 3> kinds{
            {{OA_UNIT_DEF_FLAG_UPRIGHT, 5},
             {OA_UNIT_DEF_FLAG_FLOATER, 17},
             {OA_UNIT_DEF_FLAG_CAN_HOVER, 20}}
        };
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            auto& def = sea.defs[index + 1];
            def.flags = kinds[index].first;
            def.max_damage = 100;
            auto& unit = sea.units[index + 1];
            unit.type_index = static_cast<int16_t>(index + 1);
            unit.def = oa::oa_ref_from_index(static_cast<uint32_t>(index + 1));
            unit.owner = oa::oa_ref_from_index(0);
            unit.health = 100;
            unit.position.y = kinds[index].second * 65536;
        }
        Fixture host;
        for (const uint32_t tick : {59u, 60u, 61u, 90u}) {
            world.game.tick = tick;
            for (std::size_t slot = 1; slot < 4; ++slot)
                update_unit(world, queues[slot], sea.units[slot], host);
        }
        CHECK(host.damaged.size() == 4);
        for (const auto& hit : host.damaged)
            CHECK(
                (hit.unit == 1 || hit.unit == 2) && hit.amount == 7 && hit.kind == water_damage_kind
            );
    }

    // At the 30-tick reading a unit whose type has no maximum health reads 0%
    // with no fault. A unit whose update faults (here one with no type) ends
    // only its own update: the units after it are still updated, and the pass
    // returns the first fault.
    {
        TestWorld pass(5, 3);
        auto& world = *pass;
        world.game.tick = 60;
        auto& owner = pass.player(0);
        owner.in_use = 1;
        owner.status = 1;
        pass.range(0, 1, 4);
        pass.defs[1].max_damage = 0;
        pass.defs[2].max_damage = 200;
        // Slot 1 has no maximum health, slot 2 no type, slots 3 and 4 200.
        const std::array<uint32_t, 4> types{1, 2, 2, 2};
        for (std::size_t slot = 1; slot < 5; ++slot) {
            auto& unit = pass.units[slot];
            unit.type_index = static_cast<int16_t>(types[slot - 1]);
            unit.def = slot == 2 ? oa::oa_ref32{0} : oa::oa_ref_from_index(types[slot - 1]);
            unit.owner = oa::oa_ref_from_index(0);
            unit.health = slot == 1 ? 0 : 100;
            unit.health_percent = 80;
        }
        Fixture host;
        CHECK(update_units(world, pass.orders, host) == StepFault::untyped_unit);
        CHECK(world.game.active_unit_count == 4);
        CHECK(pass.units[1].previous_health_percent == 80 && pass.units[1].health_percent == 0);
        CHECK(pass.units[2].health_percent == 80);
        for (std::size_t slot = 3; slot < 5; ++slot)
            CHECK(
                pass.units[slot].previous_health_percent == 80 &&
                pass.units[slot].health_percent == 50
            );
    }
    ended_orders_are_reported();
    std::cout << "simulation state tests passed\n";
}
