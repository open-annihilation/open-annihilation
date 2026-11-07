// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced navigation keeps pending route work, temporary steering and actual
// progress clocks across a save, including a static invalidation still pending.
#include "combat_fixture.hpp"

#include "oa/sim/ground_orders/shared_routes.hpp"
#include "oa/sim/match_runtime/enhanced_counters.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace routes = oa::sim::ground_orders;
namespace flow = oa::sim::unit_movement;

constexpr std::array<const char*, 5> navigation_tables{
    "shared-routes", "local-flow", "flow-map-version", "flow-progress", "shared-route-scan"
};

Options options_for(uint32_t version) {
    Options options;
    options.rules.setup.enhanced_mode.enabled = version != 0;
    options.rules.setup.enhanced_mode.version = version;
    return options;
}

const runtime::RuleStateTable& table(runtime::Match& match, const char* name) {
    const auto* found = runtime::find_rule_state(match.rule_state(), name);
    CHECK(found != nullptr);
    CHECK(found->bytes != nullptr);
    CHECK(found->restore != nullptr);
    CHECK(!found->digest);
    return *found;
}

std::vector<uint8_t> bytes_of(const runtime::RuleStateTable& state) {
    const auto bytes = state.bytes(state.context);
    return {bytes.begin(), bytes.end()};
}

void version_one_and_off_keep_no_flow_state() {
    for (const uint32_t version : {0u, 1u}) {
        Fixture fixture(options_for(version));
        CHECK(fixture.match->fault() == nullptr);
        for (const auto* name : navigation_tables)
            CHECK(runtime::find_rule_state(fixture.match->rule_state(), name) == nullptr);
    }
}

/// Installs an unfinished field whose revision is one behind the current map.
void install_pending_field(runtime::Match& match) {
    routes::FlowCache cache(16, 16);
    const routes::FlowSampler sampler{
        nullptr, [](void*, uint32_t, uint32_t, int32_t x, int32_t z) -> uint8_t {
            return x >= 0 && z >= 0 && x < 16 && z < 16 ? 1 : 0;
        }
    };
    constexpr uint32_t field_revision = 7;
    cache.advance(match.state().game.tick, field_revision, sampler);
    oa::Unit unit{};
    unit.cell_x = unit.cell_z = 1;
    unit.position.x = unit.position.z = 24 << 16;
    routes::Goal goal{};
    goal.cell = {14, 14};
    const routes::FlowContext context{0, 0, field_revision, 0, 1, 1};
    std::array<routes::RoutePoint, routes::navigation_capacity> points{};
    uint32_t count{};
    CHECK(cache.route(context, unit, goal, points, count, sampler) == routes::FlowResult::fallback);
    CHECK(cache.route(context, unit, goal, points, count, sampler) == routes::FlowResult::pending);
    CHECK(cache.counters().fields_started == 1);
    const auto& shared = table(match, "shared-routes");
    CHECK(shared.restore(shared.context, cache.state_bytes()));

    const auto& revision = table(match, "flow-map-version");
    constexpr std::array<uint8_t, 4> pending_revision{8, 0, 0, 0};
    CHECK(revision.restore(revision.context, pending_revision));
    CHECK(
        bytes_of(revision) == std::vector<uint8_t>(pending_revision.begin(), pending_revision.end())
    );
}

/// Restores a live steering head and a nonzero actual-progress clock together.
void install_steering_state(runtime::Match& match, uint16_t unit) {
    std::vector<flow::LocalFlowHead> heads(match.state().unit_slot_count);
    heads[unit].points = {{{64, 64}, {80, 80}, {160, 64}}};
    heads[unit].goal_key = 0x123456789abcdef0ull;
    heads[unit].tick = match.state().game.tick;
    heads[unit].count = 3;
    heads[unit].active = true;
    heads[unit].mobile_blocked = true;
    std::vector<uint8_t> bytes(heads.size() * flow::local_flow_head_bytes);
    CHECK(flow::write_local_flow_heads(heads, bytes));
    const auto& steering = table(match, "local-flow");
    CHECK(steering.restore(steering.context, bytes));
    CHECK(bytes_of(steering) == bytes);
    match.ground_runtime(unit)->movement.last_motion_tick = match.state().game.tick - 9;
}

void navigation_state_survives_restore() {
    Fixture source(options_for(2));
    Fixture loaded(options_for(2));
    auto& first = source.spawn(0, 64, 64);
    auto& second = loaded.spawn(0, 64, 64);
    CHECK(first.unit_index == second.unit_index);
    source.match->simulation().tick = loaded.match->simulation().tick = 123;
    install_pending_field(*source.match);
    install_steering_state(*source.match, first.unit_index);
    const auto& scan = table(*source.match, "shared-route-scan");
    constexpr std::array<uint8_t, 5> scan_clock{1, 123, 0, 0, 0};
    CHECK(scan.restore(scan.context, scan_clock));
    const auto expected_progress =
        source.match->ground_runtime(first.unit_index)->movement.last_motion_tick;
    CHECK(expected_progress == 114);
    CHECK(loaded.match->ground_runtime(second.unit_index)->movement.last_motion_tick == 0);

    std::array<std::vector<uint8_t>, navigation_tables.size()> saved;
    for (size_t at = 0; at < navigation_tables.size(); ++at) {
        saved[at] = bytes_of(table(*source.match, navigation_tables[at]));
        const auto& destination = table(*loaded.match, navigation_tables[at]);
        CHECK(destination.restore(destination.context, saved[at]));
        CHECK(bytes_of(destination) == saved[at]);
    }
    CHECK(
        loaded.match->ground_runtime(second.unit_index)->movement.last_motion_tick ==
        expected_progress
    );
    const auto* head = loaded.match->local_flow_head(second.unit_index);
    CHECK(head != nullptr && head->active && head->mobile_blocked);
    CHECK(head->points[1] == (std::array<int16_t, 2>{80, 80}));

    // A map change after the last field advance must remain pending after load.
    // Both matches retain the frontier and mark its routes for current-map
    // validation, rather than discarding useful work or reverting the map.
    source.run(1);
    loaded.run(1);
    CHECK(source.match->enhanced_counters().shared_route_invalidations == 0);
    CHECK(loaded.match->enhanced_counters().shared_route_invalidations == 0);
    CHECK(!source.match->local_flow_head(first.unit_index)->active);
    CHECK(!loaded.match->local_flow_head(second.unit_index)->active);
    for (size_t at = 0; at < navigation_tables.size(); ++at)
        CHECK(
            bytes_of(table(*source.match, navigation_tables[at])) ==
            bytes_of(table(*loaded.match, navigation_tables[at]))
        );
    CHECK(first.record.position.x == second.record.position.x);
    CHECK(first.record.position.z == second.record.position.z);
    CHECK(
        source.match->ground_runtime(first.unit_index)->movement.last_motion_tick ==
        loaded.match->ground_runtime(second.unit_index)->movement.last_motion_tick
    );
}

void malformed_state_is_rejected_atomically() {
    Fixture fixture(options_for(2));
    auto& unit = fixture.spawn(0, 64, 64);
    fixture.match->simulation().tick = 123;
    install_pending_field(*fixture.match);
    install_steering_state(*fixture.match, unit.unit_index);
    for (const auto* name : navigation_tables) {
        const auto& state = table(*fixture.match, name);
        const auto valid = bytes_of(state);
        CHECK(!valid.empty());
        auto short_bytes = valid;
        short_bytes.pop_back();
        CHECK(!state.restore(state.context, short_bytes));
        CHECK(bytes_of(state) == valid);
    }
    const auto& steering = table(*fixture.match, "local-flow");
    const auto valid = bytes_of(steering);
    auto malformed = valid;
    malformed[unit.unit_index * flow::local_flow_head_bytes + 24] = 1;
    CHECK(!steering.restore(steering.context, malformed));
    CHECK(bytes_of(steering) == valid);

    const auto& scan = table(*fixture.match, "shared-route-scan");
    constexpr std::array<uint8_t, 5> scanned{1, 0x78, 0x56, 0x34, 0x12};
    CHECK(scan.restore(scan.context, scanned));
    const auto saved_scan = bytes_of(scan);
    CHECK(saved_scan == std::vector<uint8_t>(scanned.begin(), scanned.end()));
    auto malformed_scan = scanned;
    malformed_scan[0] = 2;
    CHECK(!scan.restore(scan.context, malformed_scan));
    CHECK(bytes_of(scan) == saved_scan);
    malformed_scan[0] = 0;
    CHECK(!scan.restore(scan.context, malformed_scan));
    CHECK(bytes_of(scan) == saved_scan);
    constexpr std::array<uint8_t, 6> oversized_scan{1, 0, 0, 0, 0, 0};
    CHECK(!scan.restore(scan.context, oversized_scan));
    CHECK(bytes_of(scan) == saved_scan);
    constexpr std::array<uint8_t, 5> unscanned{};
    CHECK(scan.restore(scan.context, unscanned));
    CHECK(bytes_of(scan) == std::vector<uint8_t>(unscanned.begin(), unscanned.end()));
}

/// Rebuilds the fixture with one real, permissive ground movement class.
void configure_movement_class(Fixture& fixture, uint16_t per_player_limit = 4) {
    fixture.match.reset();
    fixture.fields[1].movement_class = 1;
    fixture.metadata.footprint_x = fixture.metadata.footprint_z = 1;
    fixture.metadata.max_water_depth = 255;
    fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
    fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
    const auto sight_width = static_cast<int32_t>(fixture.map.attribute_width / 2);
    const auto sight_height = static_cast<int32_t>(fixture.map.attribute_height / 2);
    runtime::OfflineInputs input{
        fixture.map,
        fixture.loaded,
        fixture.types,
        fixture.fields,
        fixture.weapons,
        fixture.terrain_values,
        fixture.masks,
        sight_width,
        sight_height,
        per_player_limit,
        2,
        0,
        30,
        1,
        &fixture.scenario,
        {},
        fixture.collision_plots,
        oa::sim::visibility_state::AltitudeSightData{
            sight_width, sight_height, fixture.altitude_cells, fixture.altitude_patterns
        }
    };
    input.rules = options_for(2).rules;
    input.unit_player_mask = 3;
    fixture.match = std::make_unique<runtime::Match>(input, fixture.services);
    fixture.match->configure_strategic_environment({0, 0.5F, 0});
    for (uint8_t player = 0; player < 2; ++player) {
        fixture.match->simulation().players[player].present = true;
        fixture.match->simulation().players[player].status = 1;
        std::array<uint8_t, 10> allies{};
        allies[player] = 1;
        fixture.match->configure_player_alliances(player, allies);
        if (player == 0)
            fixture.match->configure_outcomes(0, allies, false);
    }
    CHECK(fixture.match->fault() == nullptr);
    CHECK(fixture.match->movement_map(1) != nullptr);
}

/// Installs an admitted field before or after its first expansion tick.
void install_poll_field(runtime::Match& match, uint16_t unit, uint32_t tick, bool pending) {
    routes::FlowCache cache(16, 16);
    const routes::FlowSampler sampler{
        nullptr, [](void*, uint32_t, uint32_t, int32_t x, int32_t z) -> uint8_t {
            return x >= 0 && z >= 0 && x < 16 && z < 16 ? 1 : 0;
        }
    };
    const auto* ground = match.ground_runtime(unit);
    CHECK(ground != nullptr && ground->navigation.goal != nullptr);
    const routes::FlowContext context{0, 0, 0, 0, 1, 1};
    std::array<routes::RoutePoint, routes::navigation_capacity> points{};
    uint32_t count{};
    cache.advance(pending ? tick : tick - 2, 0, sampler);
    CHECK(
        cache.route(
            context, match.state().units[unit], *ground->navigation.goal, points, count, sampler
        ) == routes::FlowResult::fallback
    );
    CHECK(
        cache.route(
            context, match.state().units[unit], *ground->navigation.goal, points, count, sampler
        ) == routes::FlowResult::pending
    );
    if (!pending) {
        cache.advance(tick - 1, 0, sampler);
        CHECK(
            cache.route(
                context, match.state().units[unit], *ground->navigation.goal, points, count, sampler
            ) == routes::FlowResult::ready
        );
    }
    const auto& shared = table(match, "shared-routes");
    CHECK(shared.restore(shared.context, cache.state_bytes()));
    const auto& revision = table(match, "flow-map-version");
    constexpr std::array<uint8_t, 4> current_revision{};
    CHECK(revision.restore(revision.context, current_revision));
}

/// Serves shared routes without waiting for a full-search cursor or its budget.
void shared_poll_progresses_without_legacy_credit() {
    constexpr uint16_t player_units = 1024;
    constexpr uint32_t request_tick = 120;
    for (const bool pending : {false, true}) {
        auto options = options_for(2);
        options.per_player_limit = player_units;
        Fixture fixture(options);
        configure_movement_class(fixture, player_units);
        auto& match = *fixture.match;
        auto& unit = fixture.spawn(0, 64, 64);
        CHECK(!match.order_refused(
            match.issue_ground_move(unit.unit_index, {200 << 16, 0, 64 << 16}, false)
        ));
        fixture.run(1);
        CHECK(match.simulation().players[0].units.size() == player_units);
        CHECK(match.simulation().players[1].units.size() == player_units);
        auto* ground = match.ground_runtime(unit.unit_index);
        CHECK(ground != nullptr && ground->navigation.goal != nullptr);
        install_poll_field(match, unit.unit_index, request_tick, pending);
        match.simulation().tick = request_tick;
        auto& jobs = match.path_search_jobs();
        jobs.tick_credit = 0;
        jobs.credit.fill(0);
        constexpr uint32_t distant_cursor = 900;
        jobs.unit_cursor[0] = jobs.unit_cursor[1] = distant_cursor;
        ground->navigation.count = 0;
        ground->navigation.flags = routes::search_pending_flag;
        ground->navigation.last_search_tick = 0;

        const auto before = match.enhanced_counters();
        match.advance_path_search();
        CHECK(match.fault() == nullptr);
        const auto first = match.enhanced_counters();
        CHECK(first.shared_route_requests == before.shared_route_requests + 1);
        CHECK(first.full_route_searches == before.full_route_searches);
        CHECK(jobs.unit_cursor[0] == distant_cursor && jobs.unit_cursor[1] == distant_cursor);
        if (pending) {
            CHECK(first.shared_route_waits == before.shared_route_waits + 1);
            CHECK(ground->navigation.count == 0);
            CHECK(ground->navigation.last_search_tick == 0);
        } else {
            CHECK(first.shared_route_hits == before.shared_route_hits + 1);
            CHECK(ground->navigation.count >= 2);
            CHECK(ground->navigation.last_search_tick == request_tick);
        }
        const auto& scan = table(match, "shared-route-scan");
        constexpr std::array<uint8_t, 5> scanned{1, request_tick, 0, 0, 0};
        CHECK(bytes_of(scan) == std::vector<uint8_t>(scanned.begin(), scanned.end()));

        // Re-arm the request after a ready result too: this call must be
        // stopped by the saved per-tick scan clock, rather than by its route.
        ground->navigation.count = 0;
        ground->navigation.flags = routes::search_pending_flag;
        ground->navigation.last_search_tick = 0;
        match.advance_path_search();
        CHECK(match.enhanced_counters().shared_route_requests == first.shared_route_requests);
        CHECK(ground->navigation.count == 0);

        // With two active players the bounded scan visits 512 slots per
        // player. This next tick covers the other half of each 1024-slot pool.
        match.simulation().tick = request_tick + 1;
        match.advance_path_search();
        CHECK(match.enhanced_counters().shared_route_requests == first.shared_route_requests);
        CHECK(ground->navigation.count == 0);

        match.simulation().tick = request_tick + 2;
        match.advance_path_search();
        const auto delivered = match.enhanced_counters();
        CHECK(delivered.shared_route_requests == first.shared_route_requests + 1);
        CHECK(delivered.shared_route_hits == first.shared_route_hits + 1);
        CHECK(delivered.full_route_searches == before.full_route_searches);
        CHECK(ground->navigation.count >= 2);
        CHECK(ground->navigation.last_search_tick == request_tick + 2);
        CHECK(jobs.unit_cursor[0] == distant_cursor && jobs.unit_cursor[1] == distant_cursor);
    }
}

/// Leaves an active full search authoritative while its field becomes ready.
void shared_poll_preserves_an_active_full_search() {
    constexpr uint16_t player_units = 1024;
    constexpr uint32_t search_tick = 120;
    auto options = options_for(2);
    options.per_player_limit = player_units;
    Fixture fixture(options);
    configure_movement_class(fixture, player_units);
    auto& match = *fixture.match;
    auto& unit = fixture.spawn(0, 64, 64);
    CHECK(!match.order_refused(
        match.issue_ground_move(unit.unit_index, {200 << 16, 0, 64 << 16}, false)
    ));
    fixture.run(1);
    auto* ground = match.ground_runtime(unit.unit_index);
    CHECK(ground != nullptr && ground->navigation.goal != nullptr);
    const auto* goal = ground->navigation.goal;
    auto& steering = table(match, "local-flow");
    std::vector<flow::LocalFlowHead> heads(match.state().unit_slot_count);
    CHECK(flow::read_local_flow_heads(heads, bytes_of(steering)));
    heads[unit.unit_index].blocked_ticks = flow::local_flow_retry_ticks + 1;
    heads[unit.unit_index].mobile_blocked = true;
    std::vector<uint8_t> bytes(heads.size() * flow::local_flow_head_bytes);
    CHECK(flow::write_local_flow_heads(heads, bytes));
    CHECK(steering.restore(steering.context, bytes));
    ground->navigation.flags |= routes::search_pending_flag;
    ground->navigation.last_search_tick = 0;
    match.simulation().tick = search_tick;
    auto& jobs = match.path_search_jobs();
    // Give each active player one node credit. Starting this real job costs
    // more, leaving its open frontier held for a later simulation tick.
    jobs.tick_credit = match.participant_count();
    jobs.credit.fill(0);
    jobs.round_robin = 0;
    jobs.unit_cursor[0] = player_units - 1;
    CHECK(match.path_search_worker().exhausted());
    const auto before = match.enhanced_counters();
    match.advance_path_search();
    CHECK(match.fault() == nullptr);
    CHECK(match.enhanced_counters().full_route_searches == before.full_route_searches + 1);
    CHECK(ground->navigation.last_search_tick == search_tick);
    CHECK(ground->navigation.needs_search());
    CHECK(!match.path_search_worker().exhausted());
    const auto held_budget = match.path_search_worker().budget();
    const auto held_start = match.path_search_worker().start_cell();

    jobs.tick_credit = 0;
    jobs.credit.fill(0);
    CHECK(flow::read_local_flow_heads(heads, bytes_of(steering)));
    heads[unit.unit_index].blocked_ticks = flow::local_flow_retry_ticks - 1;
    CHECK(flow::write_local_flow_heads(heads, bytes));
    CHECK(steering.restore(steering.context, bytes));
    constexpr uint32_t ready_tick = search_tick + routes::search_retry_ticks;
    install_poll_field(match, unit.unit_index, ready_tick, false);
    match.simulation().tick = ready_tick;
    CHECK(ground->navigation.goal == goal);
    const auto ready = match.enhanced_counters();
    match.advance_path_search();
    CHECK(match.fault() == nullptr);
    const auto after = match.enhanced_counters();
    CHECK(after.shared_route_requests == ready.shared_route_requests);
    CHECK(after.shared_route_hits == ready.shared_route_hits);
    CHECK(after.full_route_searches == ready.full_route_searches);
    CHECK(ground->navigation.goal == goal);
    CHECK(ground->navigation.needs_search());
    CHECK(ground->navigation.last_search_tick == search_tick);
    CHECK(!match.path_search_worker().exhausted());
    CHECK(match.path_search_worker().budget() == held_budget);
    CHECK(match.path_search_worker().start_cell() == held_start);
}

/// A long mobile blockage must escape a ready field that ignores traffic.
void sustained_blockage_bypasses_shared_routes() {
    for (const uint8_t blocked_ticks :
         {uint8_t(flow::local_flow_retry_ticks - 1), flow::local_flow_retry_ticks}) {
        Fixture fixture(options_for(2));
        configure_movement_class(fixture);
        auto& unit = fixture.spawn(0, 64, 64);
        auto& match = *fixture.match;
        CHECK(!match.order_refused(
            match.issue_ground_move(unit.unit_index, {200 << 16, 0, 200 << 16}, false)
        ));
        fixture.run(1);
        auto* ground = match.ground_runtime(unit.unit_index);
        CHECK(ground != nullptr && ground->movement_class == 1);
        CHECK(ground->navigation.goal != nullptr);
        match.simulation().tick = 123;

        // This field is already complete and usable. An empty cache would
        // let its first request fall through to A* even without the bypass.
        routes::FlowCache cache(16, 16);
        const routes::FlowSampler sampler{
            nullptr, [](void*, uint32_t, uint32_t, int32_t x, int32_t z) -> uint8_t {
                return x >= 0 && z >= 0 && x < 16 && z < 16 ? 1 : 0;
            }
        };
        cache.advance(121, 0, sampler);
        const routes::FlowContext context{0, 0, 0, 0, 1, 1};
        std::array<routes::RoutePoint, routes::navigation_capacity> points{};
        uint32_t count{};
        CHECK(
            cache.route(context, unit.record, *ground->navigation.goal, points, count, sampler) ==
            routes::FlowResult::fallback
        );
        CHECK(
            cache.route(context, unit.record, *ground->navigation.goal, points, count, sampler) ==
            routes::FlowResult::pending
        );
        cache.advance(122, 0, sampler);
        CHECK(
            cache.route(context, unit.record, *ground->navigation.goal, points, count, sampler) ==
            routes::FlowResult::ready
        );
        const auto& shared = table(match, "shared-routes");
        CHECK(shared.restore(shared.context, cache.state_bytes()));
        const auto& revision = table(match, "flow-map-version");
        constexpr std::array<uint8_t, 4> current_revision{};
        CHECK(revision.restore(revision.context, current_revision));

        std::vector<flow::LocalFlowHead> heads(match.state().unit_slot_count);
        heads[unit.unit_index].blocked_ticks = blocked_ticks;
        std::vector<uint8_t> bytes(heads.size() * flow::local_flow_head_bytes);
        CHECK(flow::write_local_flow_heads(heads, bytes));
        const auto& steering = table(match, "local-flow");
        CHECK(steering.restore(steering.context, bytes));
        ground->navigation.flags |= routes::search_pending_flag;
        ground->navigation.last_search_tick = 0;

        const auto before = match.enhanced_counters();
        match.advance_path_search();
        CHECK(match.fault() == nullptr);
        const auto after = match.enhanced_counters();
        if (blocked_ticks < flow::local_flow_retry_ticks) {
            CHECK(after.shared_route_requests == before.shared_route_requests + 1);
            CHECK(after.shared_route_hits == before.shared_route_hits + 1);
            CHECK(after.full_route_searches == before.full_route_searches);
        } else {
            CHECK(after.shared_route_requests == before.shared_route_requests);
            CHECK(after.full_route_searches == before.full_route_searches + 1);
            CHECK(ground->navigation.last_search_tick == match.simulation().tick);
        }
    }
}

/// A crowded static edge must still reach a shared route through its bridge.
void static_barrier_keeps_shared_route_access() {
    Fixture fixture(options_for(2));
    // Leave both bridges above the hidden 128-world-unit bottom edge.
    fixture.map.attribute_height = 32;
    fixture.map.attributes.resize(16 * 32);
    fixture.terrain_values.resize(16 * 32);
    fixture.collision_plots.resize(16 * 32);
    fixture.altitude_cells.resize(8 * 16);
    configure_movement_class(fixture);
    auto& match = *fixture.match;
    constexpr int32_t width = 16;
    constexpr int32_t height = 32;
    constexpr int32_t wall_x = 8;
    constexpr std::array<int32_t, 2> bridge_rows{4, 12};
    for (int32_t z = 0; z < height; ++z)
        if (std::none_of(bridge_rows.begin(), bridge_rows.end(), [z](int32_t row) {
                return z >= row && z < row + 4;
            }))
            match.spatial().plots[size_t(z) * width + wall_x].blocking_feature = true;
    auto& unit = fixture.spawn(0, 72, 136);
    CHECK(!match.order_refused(
        match.issue_ground_move(unit.unit_index, {200 << 16, 0, 136 << 16}, false)
    ));
    fixture.run(1);
    auto* ground = match.ground_runtime(unit.unit_index);
    CHECK(ground != nullptr && ground->navigation.goal != nullptr);
    CHECK(ground->navigation.count == 2);
    // This fixture tests known static terrain rather than discovery through fog.
    auto& sight = match.sight_mutable().player_bits;
    std::fill(sight.begin(), sight.end(), uint16_t{1});
    match.simulation().tick = 123;

    routes::FlowCache cache(width, height);
    const routes::FlowSampler sampler{
        &match, [](void* context, uint32_t, uint32_t, int32_t x, int32_t z) -> uint8_t {
            auto& current = *static_cast<runtime::Match*>(context);
            if (x < 0 || z < 0 || x >= width || z >= height)
                return 0;
            return current.spatial().plots[size_t(z) * width + size_t(x)].blocking_feature ? 0 : 1;
        }
    };
    const routes::FlowContext context{0, 0, 0, 0, 1, 1};
    std::array<routes::RoutePoint, routes::navigation_capacity> points{};
    uint32_t count{};
    cache.advance(121, 0, sampler);
    CHECK(
        cache.route(context, unit.record, *ground->navigation.goal, points, count, sampler) ==
        routes::FlowResult::fallback
    );
    CHECK(
        cache.route(context, unit.record, *ground->navigation.goal, points, count, sampler) ==
        routes::FlowResult::pending
    );
    cache.advance(122, 0, sampler);
    CHECK(
        cache.route(context, unit.record, *ground->navigation.goal, points, count, sampler) ==
        routes::FlowResult::ready
    );
    CHECK(count >= 3);
    const auto& shared = table(match, "shared-routes");
    CHECK(shared.restore(shared.context, cache.state_bytes()));
    const auto& revision = table(match, "flow-map-version");
    constexpr std::array<uint8_t, 4> current_revision{};
    CHECK(revision.restore(revision.context, current_revision));

    // A nearby crowd can report mobile blockage while the original route
    // still points through a static wall. Its retry deadline must not prevent
    // the shared static field from supplying the missing bridge detour.
    std::vector<flow::LocalFlowHead> heads(match.state().unit_slot_count);
    heads[unit.unit_index].blocked_ticks = flow::local_flow_retry_ticks + 1;
    heads[unit.unit_index].mobile_blocked = true;
    std::vector<uint8_t> bytes(heads.size() * flow::local_flow_head_bytes);
    CHECK(flow::write_local_flow_heads(heads, bytes));
    const auto& steering = table(match, "local-flow");
    CHECK(steering.restore(steering.context, bytes));
    ground->movement.flags |= flow::collision_blocked;
    ground->navigation.flags |= routes::search_pending_flag;
    ground->navigation.last_search_tick = 0;

    const auto before = match.enhanced_counters();
    match.advance_path_search();
    CHECK(match.fault() == nullptr);
    const auto after = match.enhanced_counters();
    CHECK(after.shared_route_requests == before.shared_route_requests + 1);
    CHECK(after.shared_route_hits == before.shared_route_hits + 1);
    CHECK(after.full_route_searches == before.full_route_searches);
    CHECK(ground->navigation.count >= 3);
    CHECK(
        std::any_of(
            ground->navigation.points.begin(),
            ground->navigation.points.begin() + ground->navigation.count,
            [](const routes::RoutePoint& point) { return point[1] == 72 || point[1] == 200; }
        )
    );
    constexpr uint32_t crossing_tick_limit = 240;
    bool crossed = false;
    for (uint32_t tick = 0; tick < crossing_tick_limit && ground->navigation.goal != nullptr;
         ++tick) {
        const auto before_x = unit.record.cell_x;
        fixture.run(1);
        if (before_x < wall_x && unit.record.cell_x >= wall_x) {
            crossed = true;
            CHECK(std::any_of(bridge_rows.begin(), bridge_rows.end(), [&](int32_t row) {
                return unit.record.cell_z >= row && unit.record.cell_z < row + 4;
            }));
        }
    }
    CHECK(crossed);
    CHECK(ground->navigation.goal == nullptr);
    CHECK(std::abs((unit.record.position.x >> 16) - 200) <= 16);
    CHECK(std::abs((unit.record.position.z >> 16) - 136) <= 16);
}

/// Finds a genuine temporary traffic head while two units pass each other.
uint16_t create_passing_head(Fixture& fixture) {
    configure_movement_class(fixture);
    auto& first = fixture.spawn(0, 72, 48);
    auto& second = fixture.spawn(0, 72, 96);
    const std::array<uint16_t, 2> units{first.unit_index, second.unit_index};
    auto& match = *fixture.match;
    (void)match.issue_ground_move(units[0], {72 << 16, 0, 112 << 16}, false);
    (void)match.issue_ground_move(units[1], {72 << 16, 0, 24 << 16}, false);
    for (uint32_t tick = 0; tick < 120; ++tick) {
        fixture.run(1);
        if (tick == 2)
            for (size_t side = 0; side < units.size(); ++side) {
                auto& navigation = match.ground_runtime(units[side])->navigation;
                navigation.points[0] = {72, static_cast<int16_t>(side == 0 ? 48 : 96)};
                navigation.points[1] =
                    navigation.points[2] = {72, static_cast<int16_t>(side == 0 ? 112 : 24)};
                navigation.count = 3;
                navigation.flags = routes::route_present_flag | routes::route_changed_flag;
            }
        for (const auto unit : units)
            if (match.local_flow_head(unit)->active)
                return unit;
    }
    CHECK(false);
    return 0;
}

/// Keeps a delivered route and its avoidance head through the retry interval.
void delivered_shared_routes_keep_their_retry_interval() {
    Fixture fixture(options_for(2));
    const auto unit = create_passing_head(fixture);
    auto& match = *fixture.match;
    auto* ground = match.ground_runtime(unit);
    CHECK(ground != nullptr && ground->navigation.goal != nullptr);
    CHECK(match.local_flow_head(unit)->active);
    const auto* goal = ground->navigation.goal;
    const auto& steering = table(match, "local-flow");
    const auto genuine_head = bytes_of(steering);
    constexpr uint32_t request_tick = 120;
    constexpr uint32_t retry_tick = request_tick + routes::search_retry_ticks;
    for (const auto& slot : match.world().slots)
        if (auto* other = match.ground_runtime(slot.unit_index); other != nullptr) {
            other->navigation.flags &= uint8_t(~routes::search_pending_flag);
            other->navigation.last_search_tick = retry_tick;
        }
    auto& jobs = match.path_search_jobs();
    jobs.tick_credit = 0;
    jobs.credit.fill(0);
    match.simulation().tick = request_tick;
    install_poll_field(match, unit, request_tick, false);
    ground->navigation.count = 0;
    ground->navigation.flags = routes::search_pending_flag;
    ground->navigation.last_search_tick = 0;
    const auto before = match.enhanced_counters();
    match.advance_path_search();
    CHECK(match.fault() == nullptr);
    const auto delivered = match.enhanced_counters();
    CHECK(delivered.shared_route_requests == before.shared_route_requests + 1);
    CHECK(delivered.shared_route_hits == before.shared_route_hits + 1);
    CHECK(delivered.full_route_searches == before.full_route_searches);
    CHECK(ground->navigation.last_search_tick == request_tick);
    CHECK(ground->navigation.count >= 2);
    CHECK(!match.local_flow_head(unit)->active);
    const auto delivered_points = ground->navigation.points;
    const auto delivered_count = ground->navigation.count;

    // This head was produced by actual passing movement, and belongs to the
    // same goal. A collision request must not repeatedly replace it with the
    // same cached route before that route's retry deadline.
    CHECK(steering.restore(steering.context, genuine_head));
    CHECK(match.local_flow_head(unit)->active);
    ground->navigation.flags |= routes::search_pending_flag;
    for (const uint32_t tick : {request_tick, request_tick + 1, retry_tick - 1}) {
        match.simulation().tick = tick;
        match.advance_path_search();
        CHECK(match.fault() == nullptr);
        CHECK(match.enhanced_counters().shared_route_requests == delivered.shared_route_requests);
        CHECK(match.enhanced_counters().shared_route_hits == delivered.shared_route_hits);
        CHECK(ground->navigation.points == delivered_points);
        CHECK(ground->navigation.count == delivered_count);
        CHECK(ground->navigation.goal == goal);
        CHECK(ground->navigation.last_search_tick == request_tick);
        CHECK(ground->navigation.needs_search());
        CHECK(match.local_flow_head(unit)->active);
    }

    match.simulation().tick = retry_tick;
    match.advance_path_search();
    CHECK(match.fault() == nullptr);
    CHECK(match.enhanced_counters().shared_route_requests == delivered.shared_route_requests + 1);
    CHECK(match.enhanced_counters().shared_route_hits == delivered.shared_route_hits + 1);
    CHECK(match.enhanced_counters().full_route_searches == before.full_route_searches);
    CHECK(ground->navigation.goal == goal);
    CHECK(ground->navigation.last_search_tick == retry_tick);
    CHECK(!ground->navigation.needs_search());
    CHECK(!match.local_flow_head(unit)->active);
}

/// New searches must replace temporary steering from an older route of the same goal.
void accepted_routes_release_old_traffic_heads() {
    for (const bool shared_path : {false, true}) {
        Fixture fixture(options_for(2));
        const auto unit = create_passing_head(fixture);
        auto& match = *fixture.match;
        auto* ground = match.ground_runtime(unit);
        CHECK(ground != nullptr && ground->navigation.goal != nullptr);
        CHECK(match.local_flow_head(unit)->active);
        match.simulation().tick = 123;

        // Isolate this unit's request from the other passing unit's retry.
        for (const auto& slot : match.world().slots)
            if (auto* other = match.ground_runtime(slot.unit_index); other != nullptr) {
                other->navigation.flags &= uint8_t(~routes::search_pending_flag);
                other->navigation.last_search_tick = match.simulation().tick;
            }

        auto& steering = table(match, "local-flow");
        std::vector<flow::LocalFlowHead> heads(match.state().unit_slot_count);
        CHECK(flow::read_local_flow_heads(heads, bytes_of(steering)));
        const uint8_t collision_ticks = shared_path ? 7 : flow::local_flow_retry_ticks;
        heads[unit].blocked_ticks = collision_ticks;
        std::vector<uint8_t> bytes(heads.size() * flow::local_flow_head_bytes);
        CHECK(flow::write_local_flow_heads(heads, bytes));
        CHECK(steering.restore(steering.context, bytes));

        if (shared_path) {
            routes::FlowCache cache(16, 16);
            const routes::FlowSampler sampler{
                nullptr, [](void*, uint32_t, uint32_t, int32_t x, int32_t z) -> uint8_t {
                    return x >= 0 && z >= 0 && x < 16 && z < 16 ? 1 : 0;
                }
            };
            const routes::FlowContext context{0, 0, 0, 0, 1, 1};
            std::array<routes::RoutePoint, routes::navigation_capacity> points{};
            uint32_t count{};
            cache.advance(121, 0, sampler);
            CHECK(
                cache.route(
                    context,
                    match.state().units[unit],
                    *ground->navigation.goal,
                    points,
                    count,
                    sampler
                ) == routes::FlowResult::fallback
            );
            CHECK(
                cache.route(
                    context,
                    match.state().units[unit],
                    *ground->navigation.goal,
                    points,
                    count,
                    sampler
                ) == routes::FlowResult::pending
            );
            cache.advance(122, 0, sampler);
            CHECK(
                cache.route(
                    context,
                    match.state().units[unit],
                    *ground->navigation.goal,
                    points,
                    count,
                    sampler
                ) == routes::FlowResult::ready
            );
            const auto& shared = table(match, "shared-routes");
            CHECK(shared.restore(shared.context, cache.state_bytes()));
            const auto& revision = table(match, "flow-map-version");
            constexpr std::array<uint8_t, 4> current_revision{};
            CHECK(revision.restore(revision.context, current_revision));
        }

        ground->navigation.flags |= routes::search_pending_flag;
        ground->navigation.last_search_tick = 0;
        const auto before = match.enhanced_counters();
        match.advance_path_search();
        CHECK(match.fault() == nullptr);
        const auto after = match.enhanced_counters();
        if (shared_path) {
            CHECK(after.shared_route_hits == before.shared_route_hits + 1);
            CHECK(after.full_route_searches == before.full_route_searches);
        } else {
            CHECK(after.full_route_searches == before.full_route_searches + 1);
        }
        CHECK(routes::route_present(ground->navigation));
        CHECK(ground->navigation.count >= 2);
        CHECK(!match.local_flow_head(unit)->active);
        CHECK(match.local_flow_head(unit)->blocked_ticks == collision_ticks);
    }
}

/// Opposing units must complete their moves naturally and remain at rest.
void passing_units_rejoin_and_arrive() {
    Fixture fixture(options_for(2));
    (void)create_passing_head(fixture);
    auto& match = *fixture.match;
    std::vector<uint16_t> units;
    for (const auto& slot : match.world().slots)
        if (slot.unit != nullptr && slot.record.type_index != 0)
            units.push_back(slot.unit_index);
    CHECK(units.size() == 2);
    fixture.run(240);
    const std::array<int32_t, 2> goal_z{112, 24};
    std::array<oa::FixedVec3, 2> arrivals{};
    for (size_t side = 0; side < units.size(); ++side) {
        const auto& unit = match.state().units[units[side]];
        const int64_t dx = (unit.position.x >> 16) - 72;
        const int64_t dz = (unit.position.z >> 16) - goal_z[side];
        CHECK(dx * dx + dz * dz <= 16 * 16);
        CHECK(match.ground_runtime(units[side])->navigation.goal == nullptr);
        CHECK(!match.local_flow_head(units[side])->active);
        arrivals[side] = unit.position;
    }
    fixture.run(60);
    for (size_t side = 0; side < units.size(); ++side) {
        const auto& unit = match.state().units[units[side]];
        CHECK(unit.position.x == arrivals[side].x);
        CHECK(unit.position.z == arrivals[side].z);
    }
}

} // namespace

int main() {
    try {
        version_one_and_off_keep_no_flow_state();
        navigation_state_survives_restore();
        malformed_state_is_rejected_atomically();
        shared_poll_progresses_without_legacy_credit();
        shared_poll_preserves_an_active_full_search();
        sustained_blockage_bypasses_shared_routes();
        static_barrier_keeps_shared_route_access();
        delivered_shared_routes_keep_their_retry_interval();
        accepted_routes_release_old_traffic_heads();
        passing_units_rejoin_and_arrive();
        std::cout << "Enhanced navigation state: restore and pending invalidation match\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
