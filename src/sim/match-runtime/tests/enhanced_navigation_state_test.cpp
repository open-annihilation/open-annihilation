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
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace routes = oa::sim::ground_orders;
namespace flow = oa::sim::unit_movement;

constexpr std::array<const char*, 4> navigation_tables{
    "shared-routes", "local-flow", "flow-map-version", "flow-progress"
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
    // Both matches discard that old field on the next advance, rather than
    // silently adopting its older revision as the current map version.
    source.run(1);
    loaded.run(1);
    CHECK(source.match->enhanced_counters().shared_route_invalidations == 1);
    CHECK(loaded.match->enhanced_counters().shared_route_invalidations == 1);
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
}

/// Rebuilds the fixture with one real, permissive ground movement class.
void configure_movement_class(Fixture& fixture) {
    fixture.match.reset();
    fixture.fields[1].movement_class = 1;
    fixture.metadata.footprint_x = fixture.metadata.footprint_z = 1;
    fixture.metadata.max_water_depth = 255;
    fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
    fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
    runtime::OfflineInputs input{
        fixture.map,
        fixture.loaded,
        fixture.types,
        fixture.fields,
        fixture.weapons,
        fixture.terrain_values,
        fixture.masks,
        8,
        8,
        4,
        2,
        0,
        30,
        1,
        &fixture.scenario,
        {},
        fixture.collision_plots,
        oa::sim::visibility_state::AltitudeSightData{
            8, 8, fixture.altitude_cells, fixture.altitude_patterns
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

} // namespace

int main() {
    try {
        version_one_and_off_keep_no_flow_state();
        navigation_state_survives_restore();
        malformed_state_is_rejected_atomically();
        sustained_blockage_bypasses_shared_routes();
        std::cout << "Enhanced navigation state: restore and pending invalidation match\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
