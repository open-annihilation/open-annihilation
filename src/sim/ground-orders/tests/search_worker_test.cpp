// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/search_worker.hpp"
#include "oa/data/limits.hpp"

#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << "failed: " #x << '\n';                                                    \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

using namespace oa::sim::ground_orders;

namespace {
struct OpenSampler : MovementMapSampler {
    uint8_t classify_cell(int32_t, int32_t) override { return 3; }
};

struct GridSampler : MovementMapSampler {
    uint32_t width{}, height{};
    std::vector<uint8_t> cells;

    uint8_t classify_cell(int32_t x, int32_t z) override {
        if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= width ||
            static_cast<uint32_t>(z) >= height)
            return 0;
        return cells[static_cast<std::size_t>(z) * width + static_cast<uint32_t>(x)];
    }
};

// Sight grids: every player has seen every cell, or none has.
const std::vector<uint16_t> all_seen(64 * 64, 0xffff);
const std::vector<uint16_t> none_seen(64 * 64, 0);

void place(oa::sim::simulation_state::Unit& unit, int16_t x, int16_t z) {
    unit.record.cell_x = x;
    unit.record.cell_z = z;
}

void fill(MovementMap& map) {
    const auto extent =
        static_cast<uint32_t>(map.width() - 1) | (static_cast<uint32_t>(map.height() - 1) << 16);
    map.refresh({0, extent});
}

SearchRecord make_record(
    oa::sim::simulation_state::Unit& unit, Navigation& navigation, Goal& goal, MovementMap& map
) {
    return {&unit, &navigation, &goal, &map};
}

/// A route the scan published, and the scans it took.
struct ScheduledRoute {
    std::vector<RoutePoint> points;
    int scans{};
    int published{};
};

/// Runs one unit's job through the scan at a fixed node credit a tick, round a
/// wall across most of a 64 by 64 map, until the scan goes idle.
///
/// @param tick_credit path nodes a tick (SearchPlayerJobState::tick_credit)
/// @return the published route and the scans it took
ScheduledRoute route_at_credit(int32_t tick_credit, uint32_t seed_step_limit = 0) {
    oa::sim::unit_spawn::LegacyWorld world(3);
    auto& mover = world.unit(1);
    mover.record.type_index = 1;
    mover.object_present = true;
    world.range(0, 1, 2);
    auto& players = world.simulation().players;
    players[0].present = true;
    players[0].status = 1;

    GridSampler sampler;
    sampler.width = 64;
    sampler.height = 64;
    sampler.cells.assign(64 * 64, 3);
    for (uint32_t z = 0; z < 60; ++z)
        sampler.cells[z * 64 + 32] = 0;
    MovementMap map(64, 64, 1, 1, sampler);
    fill(map);
    oa::sim::simulation_state::Order order;
    Goal goal;
    goal.order = &order;
    goal.cell = {60, 2};
    Navigation route;
    route.goal = &goal;
    route.flags = search_pending_flag;

    struct Context {
        oa::sim::simulation_state::Unit* mover;
        Navigation* route;
        MovementMap* map;
        ScheduledRoute result;
        uint32_t seed_step_limit;
    } context{&mover, &route, &map, {}, seed_step_limit};

    SearchUnitAccess access;
    access.context = &context;
    access.navigator = [](void* c, oa::sim::simulation_state::Unit& u) -> Navigation* {
        auto* ctx = static_cast<Context*>(c);
        return &u == ctx->mover ? ctx->route : nullptr;
    };
    access.prepare =
        [](void* c, oa::sim::simulation_state::Unit&, SearchBegin& b, MovementMap*& m) {
            place(*static_cast<Context*>(c)->mover, 2, 2);
            b.seed_step_limit = static_cast<Context*>(c)->seed_step_limit;
            m = static_cast<Context*>(c)->map;
            return true;
        };
    access.publish =
        [](void* c, oa::sim::simulation_state::Unit&, Navigation&, std::span<const RoutePoint> r) {
            auto* ctx = static_cast<Context*>(c);
            ++ctx->result.published;
            ctx->result.points.assign(r.begin(), r.end());
        };
    SearchScheduler scheduler;
    do {
        scheduler.jobs.tick_credit = tick_credit;
        (void)scan_player_jobs(
            scheduler,
            players,
            1,
            1,
            100u + static_cast<uint32_t>(context.result.scans),
            all_seen,
            access
        );
        ++context.result.scans;
    } while (!scheduler.controller.idle() && context.result.scans < 1000);
    return context.result;
}

/// Continues a capped wall detour through credited slices without accepting partial progress.
void capped_wall_detours_continue_to_the_actual_goal() {
    for (const uint32_t limit : {128u, 256u}) {
        GridSampler sampler;
        sampler.width = 32;
        sampler.height = 768;
        sampler.cells.assign(sampler.width * sampler.height, 3);
        for (uint32_t z = 1; z < sampler.height - 1; ++z)
            sampler.cells[z * sampler.width + 8] = 0;
        MovementMap map(sampler.width, sampler.height, 1, 1, sampler);
        fill(map);
        const std::vector<uint16_t> seen((sampler.width / 2) * (sampler.height / 2), 0xffff);
        oa::sim::unit_spawn::LegacyWorld world(2);
        auto& unit = world.unit(1);
        place(unit, 7, 384);
        Navigation navigation;
        oa::sim::simulation_state::Order order;
        Goal goal;
        goal.order = &order;
        goal.cell = {24, 384};
        SearchBegin begin;
        begin.seed_step_limit = limit;
        SearchWorker worker;
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, seen) ==
            SearchAdvance::in_progress
        );
        CHECK(worker.budget() == static_cast<int32_t>(limit));
        CHECK(worker.threshold() == 0);
        CHECK(worker.order_flags() == 0 && order.raised_events == 0);
        CHECK((worker.cell(7, 383).flags & search_seed_bit) != 0);
        CHECK((worker.cell(7, 385).flags & search_seed_bit) != 0);
        CHECK((worker.cell(24, 384).flags & search_goal_bit) != 0);
        CHECK(worker.heuristic(7, 383) > worker.heuristic(7, 384));
        CHECK(worker.route().empty() && !worker.exhausted());
        SearchAdvance status = SearchAdvance::in_progress;
        for (uint32_t slice = 0; slice < 600 && status == SearchAdvance::in_progress; ++slice) {
            status = worker.advance_slice();
            CHECK(worker.budget() <= search_slice_expansions);
        }
        CHECK(status == SearchAdvance::succeeded);
        CHECK(worker.finish_cell() == goal.cell);
        CHECK(!worker.route().empty());
        CHECK(worker.route().front() == (RoutePoint{120, 6152}));
        CHECK(worker.route().back() == (RoutePoint{392, 6152}));
        CHECK(worker.order_flags() == 0 && order.raised_events == 0);
    }
}

/// Allows real contact on the last credited step and keeps the uncapped route unchanged.
void seed_caps_distinguish_the_last_goal_step_from_truncation() {
    std::vector<RoutePoint> legacy;
    for (const uint32_t limit : {0u, 19u, 18u, 1u}) {
        OpenSampler sampler;
        MovementMap map(16, 16, 1, 1, sampler);
        fill(map);
        oa::sim::unit_spawn::LegacyWorld world(2);
        auto& unit = world.unit(1);
        place(unit, 1, 1);
        Navigation navigation;
        oa::sim::simulation_state::Order order;
        Goal goal;
        goal.order = &order;
        goal.cell = {12, 9};
        SearchBegin begin;
        begin.seed_step_limit = limit;
        SearchWorker worker;
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, all_seen) ==
            SearchAdvance::in_progress
        );
        const bool truncated = limit != 0 && limit < 19;
        CHECK(worker.budget() == static_cast<int32_t>(truncated ? limit : 19));
        if (truncated)
            CHECK(worker.threshold() > 0 && worker.threshold() < worker.heuristic(1, 1));
        else
            CHECK(worker.threshold() == 0);
        const auto expected = truncated ? 0u : search_goal_contact_flag;
        CHECK(worker.order_flags() == expected && order.raised_events == expected);
        CHECK((worker.cell(12, 9).flags & search_goal_bit) != 0);
        SearchAdvance status = SearchAdvance::in_progress;
        for (uint32_t slice = 0; slice < 20 && status == SearchAdvance::in_progress; ++slice)
            status = worker.advance_slice();
        CHECK(status == SearchAdvance::succeeded && !worker.route().empty());
        if (truncated) {
            const auto finish = worker.finish_cell();
            CHECK(!goal_contains(goal, finish[0], finish[1]));
            CHECK(worker.heuristic(finish[0], finish[1]) <= worker.threshold());
            CHECK(worker.heuristic(finish[0], finish[1]) < worker.heuristic(1, 1));
        } else {
            CHECK(worker.finish_cell() == goal.cell);
            CHECK(worker.route().back() == (RoutePoint{200, 152}));
        }
        if (limit == 0)
            legacy = worker.route();
        else if (!truncated)
            CHECK(worker.route() == legacy);
    }
}

/// Supplies the navigator's order callbacks and counts real goal cancellation.
struct NavigationHost : Host {
    uint32_t cancellations{};

    void cancel_search(Navigation&) override { ++cancellations; }

    void play_sound(oa::sim::simulation_state::Unit&, uint32_t) override {}

    void wake_weapon(oa::sim::simulation_state::Unit&, uint32_t) override {}

    oa::sim::simulation_state::Unit* find_target(oa::sim::simulation_state::Unit&) override {
        return nullptr;
    }

    bool issue_attack(oa::sim::simulation_state::Unit&, oa::sim::simulation_state::Unit&) override {
        return false;
    }

    uint32_t random(uint32_t) override { return 0; }
};

/// Publishes progress within one credited slice while retaining the actual goal until arrival.
void capped_progress_guides_keep_the_goal_and_requeue_until_arrival() {
    OpenSampler sampler;
    MovementMap map(256, 16, 1, 1, sampler);
    fill(map);
    const std::vector<uint16_t> seen(128 * 8, 0xffff);
    oa::sim::unit_spawn::LegacyWorld world(2);
    auto& unit = world.unit(1);
    unit.record.footprint_x = unit.record.footprint_z = 1;
    place(unit, 1, 8);
    constexpr uint16_t east_heading = 49152;
    unit.record.heading = east_heading;
    oa::sim::unit_movement::Unit geometry{};
    geometry.footprint = {1, 1};
    geometry.cell = {1, 8};
    geometry.position = {24 * 65536, 0, 136 * 65536};
    oa::sim::unit_movement::Movement movement{};
    std::array<uint8_t, 3> weapons{};
    oa::sim::simulation_state::Order order;
    Goal goal;
    goal.order = &order;
    goal.cell = {240, 8};
    Navigation navigation;
    navigation.goal = &goal;
    navigation.flags = search_pending_flag;
    UnitView view{unit, geometry, movement, navigation, false, weapons};
    NavigationHost host;
    SearchWorker worker;
    uint32_t partial_guides = 0;
    bool arrived = false;
    for (uint32_t guide = 0; guide < 16; ++guide) {
        const auto tick = 100 + guide * search_retry_ticks;
        CHECK(search_ready(navigation, tick));
        SearchBegin begin;
        begin.current_tick = tick;
        begin.seed_step_limit = 32;
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, seen) ==
            SearchAdvance::in_progress
        );
        // The initial goal is 239 cells away, beyond one 100-node slice.
        // A strict-progress guide must be published within that same slice.
        CHECK(worker.advance_slice() == SearchAdvance::succeeded);
        CHECK(worker.budget() <= search_slice_expansions);
        const auto finish = worker.finish_cell();
        const bool partial = !goal_contains(goal, finish[0], finish[1]);
        if (guide == 0)
            CHECK(partial);
        if (partial) {
            ++partial_guides;
            CHECK(worker.threshold() > 0);
            CHECK(
                worker.heuristic(finish[0], finish[1]) <
                worker.heuristic(unit.record.cell_x, unit.record.cell_z)
            );
            CHECK(worker.order_flags() == 0 && order.raised_events == 0);
        }
        CHECK(!accept_path(view, worker.route()));
        CHECK(navigation.goal == &goal && goal.order == &order);
        CHECK((navigation.flags & search_pending_flag) == 0);
        CHECK(navigation.count == 2);
        if (partial)
            CHECK(order.raised_events == 0 && host.cancellations == 0);
        geometry.cell = finish;
        geometry.position = {
            (int32_t(finish[0]) * 16 + 8) * 65536, 0, (int32_t(finish[1]) * 16 + 8) * 65536
        };
        place(unit, finish[0], finish[1]);
        tick_navigation(view, tick, host);
        if (partial) {
            CHECK(navigation.goal == &goal && goal.order == &order);
            CHECK((navigation.flags & search_pending_flag) != 0);
            CHECK(order.raised_events == 0 && host.cancellations == 0);
        } else {
            CHECK(goal_contains_unit(goal, geometry));
            CHECK(navigation.goal == nullptr);
            CHECK((order.raised_events & arrived_event) != 0);
            CHECK((order.raised_events & path_failed_event) == 0);
            CHECK(host.cancellations == 1);
            arrived = true;
            break;
        }
    }
    CHECK(arrived && partial_guides >= 2);
}

/// Records the units examined when no navigator needs a full job.
struct PickTrace {
    std::vector<uint32_t> visited;
    uint32_t preferred_calls{};
    uint32_t preferred_index{7};
    bool offers{true};
};

/// Makes eight eligible units whose ids also name their list order.
void prepare_pick_world(oa::sim::unit_spawn::LegacyWorld& world) {
    world.range(0, 1, 8);
    auto& player = world.simulation().players[0];
    player.present = true;
    player.status = 1;
    for (uint16_t slot = 1; slot <= 8; ++slot) {
        auto& unit = world.unit(slot);
        unit.record.id = slot;
        unit.record.type_index = 1;
        unit.record.movement = 1;
        unit.record.owner_index = 0;
    }
}

/// Reads a trace's offer without allocating anything in the scheduler.
SearchUnitAccess trace_access(PickTrace& trace, bool preferred) {
    SearchUnitAccess access;
    access.context = &trace;
    access.navigator = [](void* c, oa::sim::simulation_state::Unit& unit) -> Navigation* {
        static_cast<PickTrace*>(c)->visited.push_back(unit.record.id);
        return nullptr;
    };
    if (preferred)
        access.preferred_unit = [](void* c, uint8_t player, uint32_t tick, uint32_t& index) {
            auto& held = *static_cast<PickTrace*>(c);
            CHECK(player == 0 && tick >= 100);
            ++held.preferred_calls;
            index = held.preferred_index;
            return held.offers;
        };
    return access;
}

/// Compares the runtime scheduler state, excluding padding and active workers.
bool same_jobs(const SearchPlayerJobState& first, const SearchPlayerJobState& second) {
    return first.round_robin == second.round_robin && first.unit_cursor == second.unit_cursor &&
           first.credit == second.credit && first.job_picks == second.job_picks &&
           first.player_heuristic == second.player_heuristic &&
           first.refresh_tick == second.refresh_tick && first.tick_credit == second.tick_credit &&
           first.base_heuristic == second.base_heuristic && first.slice_cost == second.slice_cost &&
           first.active_heuristic == second.active_heuristic;
}

/// Offers the fourth sparse pick even when heuristic usage resets between picks.
void preferred_pick_phase_survives_heuristic_resets() {
    oa::sim::unit_spawn::LegacyWorld world(9);
    prepare_pick_world(world);
    auto& players = world.simulation().players;
    PickTrace trace;
    const auto access = trace_access(trace, true);
    SearchScheduler scheduler;
    scheduler.jobs.tick_credit = 1;
    for (uint32_t pick = 0; pick < 4; ++pick) {
        scheduler.jobs.refresh_tick = search_scale_period - 1;
        CHECK(scan_player_jobs(scheduler, players, 1, 8, 100 + pick, {}, access) == 1);
        CHECK(scheduler.jobs.job_picks[0] == 1);
        if (pick < 3)
            CHECK(trace.preferred_calls == 0);
    }
    CHECK(trace.preferred_calls == 1);
    CHECK((trace.visited == std::vector<uint32_t>{2, 3, 4, 8}));
    CHECK(scheduler.preferred_pick_phase[0] == 0);
    CHECK(scheduler.jobs.unit_cursor[0] == 3);
    scheduler.preferred_pick_phase.fill(3);
    const auto ordinary = trace_access(trace, false);
    scheduler.jobs.refresh_tick = search_scale_period - 1;
    CHECK(scan_player_jobs(scheduler, players, 1, 8, 104, {}, ordinary) == 1);
    for (const auto phase : scheduler.preferred_pick_phase)
        CHECK(phase == 3);
}

/// Borrows only a fourth credited pick while preserving the ordinary next unit.
void preferred_picks_preserve_ordinary_cursor_order() {
    oa::sim::unit_spawn::LegacyWorld world(9);
    prepare_pick_world(world);
    auto& players = world.simulation().players;
    PickTrace trace;
    const auto access = trace_access(trace, true);
    SearchScheduler scheduler;
    scheduler.jobs.tick_credit = 10;
    CHECK(scan_player_jobs(scheduler, players, 1, 8, 100, {}, access) == 10);
    CHECK((trace.visited == std::vector<uint32_t>{2, 3, 4, 8, 5, 6, 7, 8, 1, 2}));
    CHECK(trace.preferred_calls == 1);
    CHECK(scheduler.jobs.unit_cursor[0] == 1);
    CHECK(scheduler.jobs.credit[0] == 0 && scheduler.jobs.job_picks[0] == 10);
    CHECK(scheduler.controller.idle());

    // A preferred unit that is already next advances the cursor once.
    trace = PickTrace{};
    trace.preferred_index = 1;
    scheduler.jobs = SearchPlayerJobState{};
    scheduler.jobs.job_picks[0] = 3;
    scheduler.preferred_pick_phase[0] = 3;
    scheduler.jobs.tick_credit = 1;
    CHECK(scan_player_jobs(scheduler, players, 1, 8, 101, {}, access) == 1);
    CHECK((trace.visited == std::vector<uint32_t>{2}));
    CHECK(trace.preferred_calls == 1 && scheduler.jobs.unit_cursor[0] == 1);
    CHECK(scan_player_jobs(scheduler, players, 1, 8, 102, {}, access) == 1);
    CHECK((trace.visited == std::vector<uint32_t>{2, 3}));
    CHECK(trace.preferred_calls == 1 && scheduler.jobs.unit_cursor[0] == 2);
}

/// Keeps rejected offers equivalent to a null callback through resets and wrap.
void rejected_preferred_picks_keep_the_ordinary_state() {
    oa::sim::unit_spawn::LegacyWorld world(9);
    prepare_pick_world(world);
    auto& players = world.simulation().players;
    for (uint32_t mode = 0; mode < 3; ++mode) {
        PickTrace ordinary, rejected;
        rejected.offers = mode != 0;
        rejected.preferred_index = mode == 2 ? 6 : 100;
        if (mode == 2)
            world.unit(7).record.owner_index = 1;
        const auto old_access = trace_access(ordinary, false);
        const auto rejected_access = trace_access(rejected, true);
        SearchScheduler old_scan, new_scan;
        old_scan.jobs.tick_credit = new_scan.jobs.tick_credit = 9;
        old_scan.jobs.job_picks[0] = new_scan.jobs.job_picks[0] =
            std::numeric_limits<int32_t>::max() - 3;
        for (uint32_t scan = 0; scan < 151; ++scan) {
            CHECK(
                scan_player_jobs(old_scan, players, 1, 8, 100 + scan, {}, old_access) ==
                scan_player_jobs(new_scan, players, 1, 8, 100 + scan, {}, rejected_access)
            );
            CHECK(same_jobs(old_scan.jobs, new_scan.jobs));
            CHECK(ordinary.visited == rejected.visited);
        }
        CHECK(rejected.preferred_calls > 0);
        world.unit(7).record.owner_index = 0;
    }

    // Runtime counter continuation reproduces the choice. A new scheduler,
    // as after rebuilding searches, starts its ordinary phase from zero.
    PickTrace first, continued;
    const auto first_access = trace_access(first, true);
    const auto continued_access = trace_access(continued, true);
    SearchScheduler original, resumed;
    original.jobs.tick_credit = 3;
    CHECK(scan_player_jobs(original, players, 1, 8, 100, {}, first_access) == 3);
    resumed.jobs = original.jobs;
    resumed.preferred_pick_phase = original.preferred_pick_phase;
    original.jobs.tick_credit = resumed.jobs.tick_credit = 5;
    first.visited.clear();
    CHECK(scan_player_jobs(original, players, 1, 8, 101, {}, first_access) == 5);
    CHECK(scan_player_jobs(resumed, players, 1, 8, 101, {}, continued_access) == 5);
    CHECK(first.visited == continued.visited && same_jobs(original.jobs, resumed.jobs));
    PickTrace rebuilt;
    const auto rebuilt_access = trace_access(rebuilt, true);
    SearchScheduler restarted;
    restarted.jobs.tick_credit = 3;
    CHECK(scan_player_jobs(restarted, players, 1, 8, 102, {}, rebuilt_access) == 3);
    CHECK(rebuilt.preferred_calls == 0);
    CHECK((rebuilt.visited == std::vector<uint32_t>{2, 3, 4}));
}

/// Starts a preferred job through the normal readiness and work accounting.
void preferred_jobs_keep_the_active_unit_goal_and_work_debt() {
    oa::sim::unit_spawn::LegacyWorld world(9);
    prepare_pick_world(world);
    auto& players = world.simulation().players;
    auto& mover = world.unit(8);
    OpenSampler sampler;
    MovementMap map(16, 16, 1, 1, sampler);
    fill(map);
    oa::sim::simulation_state::Order order;
    Goal goal;
    goal.order = &order;
    goal.cell = {12, 9};
    Navigation route;
    route.goal = &goal;
    route.flags = search_pending_flag;

    struct Context {
        oa::sim::simulation_state::Unit* mover;
        Navigation* route;
        MovementMap* map;
        uint32_t offers{};
        uint32_t prepared{};
        uint32_t published{};
        bool prepare_ok{true};
        bool offer{true};
    } context{&mover, &route, &map};

    SearchUnitAccess access;
    access.context = &context;
    access.preferred_unit = [](void* c, uint8_t player, uint32_t, uint32_t& index) {
        auto& held = *static_cast<Context*>(c);
        CHECK(player == 0);
        ++held.offers;
        index = 7;
        return held.offer;
    };
    access.navigator = [](void* c, oa::sim::simulation_state::Unit& unit) -> Navigation* {
        auto& held = *static_cast<Context*>(c);
        return &unit == held.mover ? held.route : nullptr;
    };
    access.prepare =
        [](void* c, oa::sim::simulation_state::Unit& unit, SearchBegin&, MovementMap*& m) {
            auto& held = *static_cast<Context*>(c);
            CHECK(&unit == held.mover);
            ++held.prepared;
            place(unit, 1, 1);
            m = held.map;
            return held.prepare_ok;
        };
    access.publish = [](void* c,
                        oa::sim::simulation_state::Unit& unit,
                        Navigation& navigation,
                        std::span<const RoutePoint> points) {
        auto& held = *static_cast<Context*>(c);
        CHECK(&unit == held.mover && &navigation == held.route);
        CHECK(navigation.goal != nullptr && navigation.goal->order != nullptr);
        CHECK(!points.empty());
        ++held.published;
    };
    SearchScheduler scheduler;
    scheduler.jobs.job_picks[0] = 3;
    scheduler.preferred_pick_phase[0] = 3;
    scheduler.jobs.tick_credit = 1;
    CHECK(scan_player_jobs(scheduler, players, 1, 8, 100, all_seen, access) == 1);
    CHECK(context.offers == 1 && context.prepared == 1);
    CHECK(scheduler.jobs.unit_cursor[0] == 0);
    CHECK(route.last_search_tick == 100);
    CHECK(scheduler.worker.budget() == 19);
    CHECK(scheduler.jobs.credit[0] == 1 - 101 - 19);
    CHECK(scheduler.controller.active().unit == &mover);
    CHECK(scheduler.controller.active().navigation == &route);
    CHECK(scheduler.controller.active().goal == &goal);
    context.offer = false;
    for (uint32_t tick = 101; !scheduler.controller.idle() && tick < 150; ++tick) {
        scheduler.jobs.credit.fill(0);
        CHECK(scan_player_jobs(scheduler, players, 1, 8, tick, all_seen, access) >= 1);
    }
    CHECK(scheduler.controller.idle() && context.published == 1);
    CHECK(context.offers == 1 && context.prepared == 1);
    CHECK(scheduler.jobs.unit_cursor[0] == 0);
    scheduler.jobs.credit.fill(0);
    CHECK(scan_player_jobs(scheduler, players, 1, 8, 150, all_seen, access) == 1);
    CHECK(scheduler.jobs.unit_cursor[0] == 1);

    // Failed preparation takes its normal launch debt and retry stamp too.
    scheduler.jobs = SearchPlayerJobState{};
    scheduler.jobs.job_picks[0] = 3;
    scheduler.preferred_pick_phase[0] = 3;
    scheduler.jobs.tick_credit = 1;
    route.last_search_tick = 0;
    context.offer = true;
    context.prepare_ok = false;
    CHECK(scan_player_jobs(scheduler, players, 1, 8, 200, all_seen, access) == 1);
    CHECK(scheduler.controller.idle() && scheduler.jobs.unit_cursor[0] == 0);
    CHECK(route.last_search_tick == 200 && scheduler.jobs.credit[0] == 1 - 101);
    CHECK(context.prepared == 2 && context.published == 1);

    // Cancelling a job leaves no cursor binding to reuse when searches and
    // their order-owned Goals are rebuilt on new unit storage.
    scheduler.jobs = SearchPlayerJobState{};
    scheduler.jobs.job_picks[0] = 3;
    scheduler.preferred_pick_phase[0] = 3;
    scheduler.jobs.tick_credit = 1;
    route.last_search_tick = 0;
    context.prepare_ok = true;
    CHECK(scan_player_jobs(scheduler, players, 1, 8, 300, all_seen, access) == 1);
    CHECK(scheduler.controller.active().unit == &mover);
    scheduler.controller.cancel(route);
    CHECK(scheduler.controller.idle());
    oa::sim::unit_spawn::LegacyWorld rebuilt_world(9);
    prepare_pick_world(rebuilt_world);
    oa::sim::simulation_state::Order rebuilt_order;
    Goal rebuilt_goal;
    rebuilt_goal.order = &rebuilt_order;
    rebuilt_goal.cell = {9, 12};
    Navigation rebuilt_route;
    rebuilt_route.goal = &rebuilt_goal;
    rebuilt_route.flags = search_pending_flag;
    context.mover = &rebuilt_world.unit(8);
    context.route = &rebuilt_route;
    SearchScheduler rebuilt;
    rebuilt.jobs.tick_credit = 4;
    CHECK(
        scan_player_jobs(
            rebuilt, rebuilt_world.simulation().players, 1, 8, 400, all_seen, access
        ) == 4
    );
    CHECK(rebuilt.jobs.unit_cursor[0] == 3);
    CHECK(rebuilt.controller.active().unit == &rebuilt_world.unit(8));
    CHECK(rebuilt.controller.active().navigation == &rebuilt_route);
    CHECK(rebuilt.controller.active().goal == &rebuilt_goal);
    CHECK(rebuilt_route.last_search_tick == 400 && route.last_search_tick == 300);
}
} // namespace

int main() {
    preferred_pick_phase_survives_heuristic_resets();
    capped_wall_detours_continue_to_the_actual_goal();
    capped_progress_guides_keep_the_goal_and_requeue_until_arrival();
    seed_caps_distinguish_the_last_goal_step_from_truncation();
    preferred_picks_preserve_ordinary_cursor_order();
    rejected_preferred_picks_keep_the_ordinary_state();
    preferred_jobs_keep_the_active_unit_goal_and_work_debt();
    SearchCellGrid grid;
    grid.allocate(1, 1);
    CHECK(grid.width == 1 && grid.height == 1 && grid.count == 8 && grid.cells.size() == 8);
    CHECK(grid.cells[0].flags == 0 && grid.cells[7].predecessor == 0);
    grid.cells[0].flags = 3;
    grid.allocate(7, 1);
    CHECK(grid.count == 8 && grid.cells.size() == 8 && grid.cells[0].flags == 0);
    grid.allocate(3, 3);
    CHECK(grid.width == 3 && grid.height == 3 && grid.count == 16 && grid.cells.size() == 16);
    grid.allocate(8, 2);
    CHECK(grid.count == 16 && grid.cells.size() == 16);
    grid.allocate(0, 5);
    CHECK(grid.width == 0 && grid.height == 5 && grid.count == 0 && grid.cells.empty());
    grid.allocate(-1, 1);
    CHECK(grid.width == -1 && grid.height == 1 && grid.count == 0 && grid.cells.empty());
    grid.allocate(0x10000, 0x10000);
    CHECK(grid.width == 0x10000 && grid.height == 0x10000 && grid.count == 0 && grid.cells.empty());

    CHECK(crossed_start_goal_axis(0, 0, 4, 0, 2, 0));
    CHECK(crossed_start_goal_axis(0, 0, 4, 0, 4, 0));
    CHECK(!crossed_start_goal_axis(0, 0, 4, 0, 5, 0));
    CHECK(!crossed_start_goal_axis(0, 0, 4, 0, 0, 0));
    CHECK(crossed_start_goal_axis(0, 0, 0, 3, 0, 2));
    CHECK(!crossed_start_goal_axis(0, 0, 0, 3, 0, 0));
    CHECK(!crossed_start_goal_axis(0, 0, 4, 3, 2, 1));
    CHECK(crossed_start_goal_axis(0, 0, 4, 3, 4, 1));
    CHECK(crossed_start_goal_axis(5, 5, 1, 5, 3, 5));
    CHECK(crossed_start_goal_axis(5, 5, 5, 1, 5, 3));
    CHECK(!crossed_start_goal_axis(0, 0, 4, 0, 2, 1));

    CHECK(search_player_credit(search_tick_credit, 0) == 0);
    CHECK(search_player_credit(search_tick_credit, 1) == search_tick_credit);
    CHECK(search_player_credit(search_tick_credit, 2) == 0x29a);
    CHECK(search_rescaled_heuristic(0, 1, search_heuristic_scale) == search_heuristic_scale * 6);
    CHECK(search_rescaled_heuristic(1, 1, search_heuristic_scale) == search_heuristic_scale * 3);
    CHECK(search_rescaled_heuristic(2, 1, search_heuristic_scale) == search_heuristic_scale);
    CHECK(search_rescaled_heuristic(8, 4, search_heuristic_scale) == search_heuristic_scale);

    oa::sim::unit_spawn::LegacyWorld search_world(2);
    auto& unit = search_world.unit(1);
    Navigation navigation;
    const auto& mask = all_seen;

    {
        OpenSampler sampler;
        MovementMap map(8, 8, 1, 1, sampler);
        fill(map);
        Goal goal;
        goal.cell = {0, 0};
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 0, 0);
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, mask) ==
            SearchAdvance::failed
        );
        CHECK((worker.order_flags() & search_goal_contact_flag) != 0);
        CHECK(worker.route().empty());
    }

    {
        OpenSampler sampler;
        MovementMap map(4, 4, 1, 1, sampler);
        fill(map);
        Goal goal;
        goal.cell = {1, 0};
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 8, 0);
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, mask) ==
            SearchAdvance::failed
        );
        CHECK((worker.order_flags() & search_seed_unresolved_flag) == search_seed_unresolved_flag);
    }

    {
        GridSampler sampler;
        sampler.width = 4;
        sampler.height = 4;
        sampler.cells.assign(16, 0);
        MovementMap map(4, 4, 1, 1, sampler);
        fill(map);
        Goal goal;
        goal.cell = {3, 0};
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 0, 0);
        const auto started = worker.begin(make_record(unit, navigation, goal, map), begin, mask);
        CHECK(started == SearchAdvance::failed);
        CHECK((worker.order_flags() & search_seed_unresolved_flag) == search_seed_unresolved_flag);
    }

    {
        const auto& empty = none_seen;
        OpenSampler sampler;
        MovementMap map(4, 4, 1, 1, sampler);
        fill(map);
        Goal goal;
        goal.cell = {2, 0};
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 0, 0);
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, empty) ==
            SearchAdvance::in_progress
        );
        CHECK(worker.classify(1, 0) == 2);
        CHECK(worker.expand_best() == 0);
        CHECK(worker.fan() == search_initial_fan);
        CHECK(worker.cell(1, 0).predecessor == 6);
        CHECK((worker.cell(1, 0).flags & 3) == 1);
    }

    {
        OpenSampler sampler;
        MovementMap map(8, 8, 1, 1, sampler);
        fill(map);
        Goal goal;
        goal.cell = {3, 0};
        oa::sim::simulation_state::Order order;
        goal.order = &order;
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 0, 0);
        unit.record.heading = 0;
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, mask) ==
            SearchAdvance::in_progress
        );
        CHECK(worker.fan() == search_initial_fan);
        CHECK((worker.order_flags() & search_goal_contact_flag) != 0);
        CHECK((order.raised_events & search_goal_contact_flag) != 0);
        CHECK((worker.cell(0, 0).flags & 3) == 1);
        CHECK(worker.expand_best() == 0);
        CHECK((worker.cell(0, 0).flags & 3) == 2);
        CHECK((worker.cell(1, 0).flags & 3) == 1);
        CHECK(worker.cell(1, 0).predecessor == 6);
        auto g = std::bit_cast<int32_t>(worker.heap().payload(worker.cell(1, 0).handle)[1]);
        CHECK(g == search_cardinal_cost + search_turn_penalty[6]);
        SearchAdvance result = SearchAdvance::in_progress;
        worker = SearchWorker{};
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, mask) ==
            SearchAdvance::in_progress
        );
        for (int slice = 0; slice < 8 && result == SearchAdvance::in_progress; ++slice)
            result = worker.advance_slice();
        CHECK(result == SearchAdvance::succeeded);
        CHECK(worker.fan() == search_continue_fan);
        CHECK(!worker.route().empty());
        CHECK(worker.route().front()[0] == 8);
        CHECK(worker.route().back()[0] == 56);
    }

    {
        GridSampler sampler;
        sampler.width = 4;
        sampler.height = 3;
        sampler.cells.assign(12, 3);
        sampler.cells[1] = 0;
        MovementMap map(4, 3, 1, 1, sampler);
        fill(map);
        Goal goal;
        goal.cell = {2, 0};
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 0, 0);
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, mask) ==
            SearchAdvance::in_progress
        );
        CHECK((worker.cell(0, 1).flags & search_seed_bit) != 0);
        CHECK((worker.cell(1, 1).flags & search_seed_bit) != 0);
        CHECK(worker.threshold() < worker.heuristic(0, 0));
        CHECK(
            (worker.order_flags() & (search_goal_contact_flag | search_seed_unresolved_flag)) != 0
        );
        SearchAdvance result = SearchAdvance::in_progress;
        for (int slice = 0; slice < 8 && result == SearchAdvance::in_progress; ++slice)
            result = worker.advance_slice();
        CHECK(result == SearchAdvance::succeeded);
        CHECK(!worker.route().empty());
    }

    {
        OpenSampler sampler;
        MovementMap map(6, 6, 1, 1, sampler);
        fill(map);
        Goal first;
        first.cell = {2, 0};
        Goal second;
        second.cell = {4, 1};
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 0, 0);
        CHECK(
            worker.begin(make_record(unit, navigation, first, map), begin, mask) ==
            SearchAdvance::in_progress
        );
        while (worker.advance_slice() == SearchAdvance::in_progress) {
        }
        CHECK(
            worker.begin(make_record(unit, navigation, second, map), begin, mask) ==
            SearchAdvance::in_progress
        );
        CHECK((worker.cell(2, 0).flags & search_goal_bit) == 0);
        CHECK((worker.cell(4, 1).flags & search_goal_bit) != 0);
    }

    {
        GridSampler sampler;
        sampler.width = 4;
        sampler.height = 4;
        sampler.cells.assign(16, 1);
        MovementMap map(4, 4, 1, 1, sampler);
        fill(map);
        Goal goal;
        goal.cell = {2, 0};
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 0, 0);
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, mask) ==
            SearchAdvance::in_progress
        );
        CHECK(worker.classify(1, 0) == 1);
        CHECK(worker.expand_best() == 0);
        auto extra = std::bit_cast<int16_t>(
            static_cast<uint16_t>(worker.heap().payload(worker.cell(1, 0).handle)[3])
        );
        CHECK(extra == search_difficult_extra);
    }

    {
        OpenSampler sampler;
        MovementMap map(8, 8, 1, 1, sampler);
        fill(map);
        Goal goal;
        goal.shape = GoalShape::outline;
        goal.left = 4;
        goal.right = 5;
        goal.top = 0;
        goal.bottom = 1;
        SearchWorker worker;
        SearchBegin begin;
        place(unit, 0, 0);
        CHECK(
            worker.begin(make_record(unit, navigation, goal, map), begin, mask) ==
            SearchAdvance::in_progress
        );
        CHECK((worker.cell(4, 0).flags & search_goal_bit) != 0);
        CHECK((worker.cell(5, 1).flags & search_goal_bit) != 0);
        SearchAdvance status = SearchAdvance::in_progress;
        while (status == SearchAdvance::in_progress)
            status = worker.advance_slice();
        CHECK(status == SearchAdvance::succeeded);
        CHECK(!worker.route().empty());
    }

    {
        // With no players the scan leaves credits, cursors and the refresh counter alone.
        const SearchUnitAccess no_access{};
        SearchScheduler idle_scheduler;
        auto& idle = idle_scheduler.jobs;
        CHECK(scan_player_jobs(idle_scheduler, {}, 0, 1, 0, {}, no_access) == 0);
        CHECK(idle.refresh_tick == 0);
        CHECK(idle.credit[0] == 0);

        // Slots 1-2 belong to player 0 and slot 3 to player 1.
        oa::sim::unit_spawn::LegacyWorld jobs_world(4);
        jobs_world.unit(2).record.type_index = 1;
        jobs_world.unit(2).object_present = true;
        jobs_world.unit(3).record.type_index = 1;
        jobs_world.unit(3).object_present = true;
        jobs_world.range(0, 1, 2);
        jobs_world.range(1, 3, 1);
        auto& players = jobs_world.simulation().players;
        players[0].present = true;
        players[0].status = 1;
        players[2].present = true;
        players[2].status = 4;

        SearchScheduler scheduler;
        auto& jobs = scheduler.jobs;
        jobs.tick_credit = 1;
        // Cursor starts on the first unit. One credit examines the second slot.
        CHECK(scan_player_jobs(scheduler, players, 1, 1, 0, {}, no_access) == 1);
        CHECK(jobs.unit_cursor[0] == 1);
        CHECK(jobs.credit[0] == 0);
        CHECK(jobs.job_picks[0] == 1);
        CHECK(scheduler.controller.idle());
        CHECK(jobs.slice_cost == 1);
        CHECK(jobs.round_robin == 0);
        // Next visit wraps from the last unit back to the first.
        jobs.tick_credit = 1;
        CHECK(scan_player_jobs(scheduler, players, 1, 1, 0, {}, no_access) == 1);
        CHECK(jobs.unit_cursor[0] == 0);
        CHECK(jobs.job_picks[0] == 2);

        // Share is the tick credit / player count. The budget sums the whole active balance, then
        // the round-robin drains one slot before advancing.
        jobs = SearchPlayerJobState{};
        jobs.tick_credit = search_tick_credit;
        players[1].present = true;
        players[1].status = 1;
        const auto share = search_player_credit(search_tick_credit, 2);
        CHECK(share == 0x29a);
        CHECK(scan_player_jobs(scheduler, players, 2, 1, 0, {}, no_access) == share * 2);
        CHECK(jobs.credit[0] == 0);
        CHECK(jobs.credit[1] == 0);
        CHECK(jobs.job_picks[0] == share);
        CHECK(jobs.job_picks[1] == share);
        CHECK(jobs.round_robin == 1);
        CHECK(jobs.unit_cursor[0] == 0);
        CHECK(jobs.unit_cursor[1] == 0);
        CHECK(scheduler.controller.idle());
        // Slot 2 is present but status 4, so the slot-active test does not fund it.
        CHECK(jobs.credit[2] == 0);
        CHECK(jobs.job_picks[2] == 0);

        // An inactive slot keeps leftover credit and can spend it when the
        // round-robin lands there, even though it was left out of the budget.
        jobs = SearchPlayerJobState{};
        jobs.tick_credit = 1;
        jobs.round_robin = 1;
        jobs.credit[1] = 5;
        players[1].present = false;
        players[1].status = 0;
        CHECK(scan_player_jobs(scheduler, players, 1, 1, 0, {}, no_access) == 1);
        CHECK(jobs.round_robin == 1);
        CHECK(jobs.credit[0] == 1);
        CHECK(jobs.credit[1] == 4);
        CHECK(jobs.job_picks[1] == 1);
        CHECK(jobs.unit_cursor[1] == 0);
        CHECK(jobs.job_picks[0] == 0);

        // The refresh counter wraps at 150 and rewrites player_heuristic from job_picks.
        jobs = SearchPlayerJobState{};
        jobs.tick_credit = 0;
        jobs.base_heuristic = 10;
        for (int tick = 0; tick < 149; ++tick)
            CHECK(scan_player_jobs(scheduler, players, 1, 4, 0, {}, no_access) == 0);
        CHECK(jobs.refresh_tick == 0x95);
        CHECK(jobs.player_heuristic[0] == search_heuristic_scale);
        jobs.job_picks[0] = 8;
        CHECK(scan_player_jobs(scheduler, players, 1, 4, 0, {}, no_access) == 0);
        CHECK(jobs.refresh_tick == 0);
        CHECK(jobs.job_picks[0] == 0);
        CHECK(jobs.player_heuristic[0] == 10);
        CHECK(jobs.player_heuristic[1] == 60);
    }

    {
        // construct_search resets the open set, allocates the rounded world grid,
        // clears the touched map, and snaps cursors to the first unit.
        SearchHeap heap;
        CHECK(heap.insert({1, 2, 3, 4}) == 0);
        heap.pop();
        CHECK(heap.capacity() != 0 && heap.deferred_pop());
        SearchCellGrid search_grid;
        std::vector<uint32_t> touched{0x11111111u};
        SearchPlayerJobState jobs;
        jobs.round_robin = 3;
        jobs.unit_cursor.fill(4);
        jobs.credit.fill(9);
        jobs.job_picks.fill(7);
        jobs.player_heuristic.fill(1);
        jobs.refresh_tick = 42;
        jobs.tick_credit = 1;
        jobs.base_heuristic = 2;
        jobs.slice_cost = 8;
        jobs.active_heuristic = 6;
        construct_search(heap, search_grid, touched, jobs, 20, 13);
        CHECK(heap.empty() && heap.allocated_slots() == 0 && heap.capacity() == 0);
        CHECK(heap.free_head() == -1 && !heap.deferred_pop());
        CHECK(search_grid.width == 20 && search_grid.height == 13 && search_grid.count == 264);
        CHECK(search_grid.cells.size() == 264);
        for (const auto& cell : search_grid.cells)
            CHECK(cell.flags == 0 && cell.predecessor == 0 && cell.handle == 0);
        CHECK(touched.size() == 2 && touched[0] == 0 && touched[1] == 0);
        CHECK(jobs.tick_credit == search_tick_credit);
        CHECK(jobs.base_heuristic == search_heuristic_scale);
        CHECK(jobs.round_robin == 0 && jobs.slice_cost == 0 && jobs.active_heuristic == 0);
        CHECK(jobs.refresh_tick == 42);
        for (std::size_t player = 0; player < search_player_slots; ++player) {
            CHECK(jobs.unit_cursor[player] == 0);
            CHECK(jobs.credit[player] == 0);
            CHECK(jobs.player_heuristic[player] == search_heuristic_scale);
            CHECK(jobs.job_picks[player] == 7);
        }

        construct_search(heap, search_grid, touched, jobs, 7, 1);
        CHECK(search_grid.width == 7 && search_grid.height == 1 && search_grid.count == 8);
        CHECK(search_grid.cells.size() == 8 && touched.size() == 1 && touched[0] == 0);
        CHECK(jobs.refresh_tick == 42);

        construct_search(heap, search_grid, touched, jobs, 0, 5);
        CHECK(search_grid.count == 0 && search_grid.cells.empty() && touched.empty());
        CHECK(jobs.tick_credit == search_tick_credit && jobs.refresh_tick == 42);
    }

    {
        // A full job through the scan: the unit's navigator asks for a search,
        // the job starts (cost 101 and one for each step of its wall-follow
        // walk), later slices expand it and publish a route.
        oa::sim::unit_spawn::LegacyWorld jobs_world(3);
        auto& mover = jobs_world.unit(1);
        mover.record.type_index = 1;
        mover.object_present = true;
        jobs_world.range(0, 1, 2);
        auto& players = jobs_world.simulation().players;
        players[0].present = true;
        players[0].status = 1;

        OpenSampler sampler;
        MovementMap map(16, 16, 1, 1, sampler);
        fill(map);
        oa::sim::simulation_state::Order order;
        Goal goal;
        goal.order = &order;
        goal.cell = {12, 9};
        Navigation route;
        route.goal = &goal;
        route.flags = search_pending_flag;

        struct Context {
            oa::sim::simulation_state::Unit* mover;
            Navigation* route;
            MovementMap* map;
            int published{};
            std::vector<RoutePoint> last;
        } context{&mover, &route, &map, 0, {}};

        SearchUnitAccess access;
        access.context = &context;
        access.navigator = [](void* c, oa::sim::simulation_state::Unit& u) -> Navigation* {
            auto* ctx = static_cast<Context*>(c);
            return &u == ctx->mover ? ctx->route : nullptr;
        };
        access.prepare =
            [](void* c, oa::sim::simulation_state::Unit&, SearchBegin& b, MovementMap*& m) {
                place(*static_cast<Context*>(c)->mover, 1, 1);
                (void)b;
                m = static_cast<Context*>(c)->map;
                return true;
            };
        access.publish = [](void* c,
                            oa::sim::simulation_state::Unit&,
                            Navigation&,
                            std::span<const RoutePoint> r) {
            auto* ctx = static_cast<Context*>(c);
            ++ctx->published;
            ctx->last.assign(r.begin(), r.end());
        };

        SearchScheduler scheduler;
        scheduler.jobs.tick_credit = 2;
        // Tick 100: the second unit is examined first (no navigator), then the mover.
        CHECK(scan_player_jobs(scheduler, players, 1, 1, 100, all_seen, access) == 2);
        CHECK(!scheduler.controller.idle() && route.last_search_tick == 100);
        // The walk goes straight from (1, 1) to the goal cell (12, 9): 11 steps
        // across and 8 down, the 19th reaching the goal.
        CHECK(scheduler.worker.budget() == 19);
        CHECK(scheduler.jobs.credit[0] == 2 - 1 - 101 - 19);
        CHECK(scheduler.jobs.player_heuristic[0] == search_heuristic_scale);
        int ticks = 0;
        while (!scheduler.controller.idle() && ticks < 50) {
            scheduler.jobs.tick_credit = 0x535;
            (void)scan_player_jobs(
                scheduler, players, 1, 1, 101u + static_cast<uint32_t>(ticks), all_seen, access
            );
            ++ticks;
        }
        CHECK(scheduler.controller.idle() && context.published == 1);
        CHECK(context.last.size() >= 2);
        // Route points are cell centres in world units of the unit's footprint.
        CHECK(context.last.front()[0] == 24 && context.last.front()[1] == 24);
        CHECK(
            context.last.back()[0] == (12 * 2 + 1) * 8 && context.last.back()[1] == (9 * 2 + 1) * 8
        );
    }

    {
        // The node budget only slices a job: at 3.1c's 1,333 nodes a tick the
        // route round the wall takes several scans, at the largest budget a
        // profile may name it takes one, and the route is the same.
        const auto base = route_at_credit(search_tick_credit);
        const auto largest = route_at_credit(oa::data::limits::highest_path_search_nodes);
        CHECK(base.published == 1 && largest.published == 1);
        CHECK(base.scans > largest.scans && largest.scans <= 2);
        CHECK(!base.points.empty() && base.points == largest.points);
        const auto capped = route_at_credit(search_tick_credit, 32);
        const auto capped_largest =
            route_at_credit(oa::data::limits::highest_path_search_nodes, 32);
        CHECK(capped.published == 1 && capped_largest.published == 1);
        CHECK(!capped.points.empty() && capped.points == capped_largest.points);
        CHECK(capped.points.back() != (RoutePoint{968, 40}));
        Goal goal;
        goal.cell = {60, 2};
        const auto end = capped.points.back();
        CHECK(goal_cost(goal, (end[0] - 8) / 16, (end[1] - 8) / 16) < goal_cost(goal, 2, 2));
    }

    {
        // A cell keeps any handle the open set gives out, past 16 bits too.
        SearchMapCell cell;
        cell.handle = 70000;
        CHECK(cell.handle == 70000);
        cell.handle = SearchHeap::no_handle;
        CHECK(cell.handle == SearchHeap::no_handle);
    }
}
