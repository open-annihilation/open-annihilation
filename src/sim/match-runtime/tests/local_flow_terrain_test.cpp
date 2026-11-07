// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Local traffic probes stop at known impassable terrain, while unseen
// terrain keeps the navigator's unknown-passable rule.
#include "combat_fixture.hpp"

#include "oa/sim/match_runtime/enhanced_counters.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;

constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint32_t water_column = 6;
constexpr uint16_t east = 49152;
constexpr uint16_t west = 16384;

/// Rebuilds the match with a ground class that cannot enter the water column.
void configure_water_barrier(Fixture& fixture, bool water, uint8_t footprint = 1) {
    fixture.match.reset();
    fixture.map.sea_level = sea_level;
    for (uint32_t index = 0; index < fixture.map.attributes.size(); ++index) {
        const uint8_t height =
            water && index % fixture.map.attribute_width == water_column ? 0 : land_height;
        fixture.map.attributes[index].height = height;
        fixture.collision_plots[index].low_height = fixture.collision_plots[index].high_height =
            height;
    }
    fixture.fields[1].movement_class = 1;
    fixture.types[1].footprint_x = fixture.types[1].footprint_z = footprint;
    fixture.loaded[1].type = fixture.types[1];
    fixture.metadata.footprint_x = fixture.metadata.footprint_z = footprint;
    fixture.metadata.max_water_depth = 0;
    fixture.metadata.min_water_depth = -10000;
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
        sea_level,
        1,
        &fixture.scenario,
        {},
        fixture.collision_plots,
        oa::sim::visibility_state::AltitudeSightData{
            8, 8, fixture.altitude_cells, fixture.altitude_patterns
        }
    };
    input.rules.setup.enhanced_mode.enabled = true;
    input.rules.setup.enhanced_mode.version = 2;
    input.unit_player_mask = 3;
    fixture.match = std::make_unique<runtime::Match>(input, fixture.services);
    fixture.match->configure_strategic_environment({0, 0.5F, 0});
    for (uint8_t player = 0; player < 2; ++player) {
        fixture.match->simulation().players[player].present = true;
        fixture.match->simulation().players[player].status = 1;
        std::array<uint8_t, OA_PLAYER_COUNT> allies{};
        allies[player] = 1;
        fixture.match->configure_player_alliances(player, allies);
        if (player == 0)
            fixture.match->configure_outcomes(0, allies, false);
    }
    CHECK(fixture.match->fault() == nullptr);
    CHECK(fixture.match->movement_map(1) != nullptr);
}

/// Distinguishes visible terrain blockage from traffic behind an unseen cell.
void traffic_probes_respect_static_terrain_and_fog() {
    for (const bool water : {true, false})
        for (const bool known_water : {true, false}) {
            Options options;
            options.sight_cells = known_water ? 9 : 1;
            Fixture fixture(options);
            configure_water_barrier(fixture, water);
            auto& blocker = fixture.spawn(0, 136, 64);
            auto& mover = fixture.spawn(0, 72, 64);
            blocker.record.heading = west;
            mover.record.heading = east;
            auto& match = *fixture.match;
            CHECK(!match.order_refused(
                match.issue_ground_move(blocker.unit_index, {24 << 16, 0, 64 << 16}, false)
            ));
            CHECK(!match.order_refused(
                match.issue_ground_move(mover.unit_index, {200 << 16, 0, 64 << 16}, false)
            ));
            fixture.run(1);

            // An unseen water column and unseen clear land produce the same
            // result. Only known impassable terrain can suppress the traffic head.
            const auto* head = match.local_flow_head(mover.unit_index);
            CHECK(head != nullptr);
            const auto counters = match.enhanced_counters();
            const bool terrain_blocked = water && known_water;
            CHECK(head->mobile_blocked == !terrain_blocked);
            CHECK(head->active == !terrain_blocked);
            CHECK(counters.moving_blockers == (terrain_blocked ? 0u : 2u));
            CHECK(counters.steering_adjustments == (terrain_blocked ? 0u : 1u));
            CHECK(match.ground_runtime(mover.unit_index)->navigation.goal != nullptr);
        }
}

struct IdleResult {
    uint64_t digest{14695981039346656037ull};
    uint32_t completion_tick{};
    uint32_t arrival_tick{};
    bool operator==(const IdleResult&) const = default;
};

/// Passes an idle mobile unit, optionally restoring the first completed local leg.
IdleResult run_idle_mobile_occupant(bool restore_completed) {
    Options options;
    options.sight_cells = 9;
    Fixture fixture(options);
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    fixture.fields[1].yard_mask = yard;
    configure_water_barrier(fixture, false, 2);
    auto& blocker = fixture.spawn(0, 136, 64);
    auto& mover = fixture.spawn(0, 72, 64);
    blocker.record.heading = west;
    mover.record.heading = east;
    auto& match = *fixture.match;
    CHECK(blocker.record.footprint_x == 2 && blocker.record.footprint_z == 2);
    CHECK(mover.record.footprint_x == 2 && mover.record.footprint_z == 2);
    CHECK(match.ground_runtime(blocker.unit_index)->navigation.goal == nullptr);
    CHECK(!match.order_refused(
        match.issue_ground_move(mover.unit_index, {200 << 16, 0, 64 << 16}, false)
    ));
    fixture.run(1);
    const auto* head = match.local_flow_head(mover.unit_index);
    CHECK(head != nullptr && head->mobile_blocked && head->active);
    CHECK(head->tick == match.state().game.tick);
    CHECK(match.enhanced_counters().moving_blockers == 1);

    IdleResult result;
    // The parked actor stays a real collision footprint throughout passing.
    const auto idle_x = blocker.record.position.x;
    const auto idle_z = blocker.record.position.z;
    for (uint32_t tick = 0; tick < 600; ++tick) {
        fixture.run(1);
        CHECK(blocker.record.position.x == idle_x && blocker.record.position.z == idle_z);
        const bool overlap =
            mover.record.cell_x < blocker.record.cell_x + blocker.record.footprint_x &&
            mover.record.cell_x + mover.record.footprint_x > blocker.record.cell_x &&
            mover.record.cell_z < blocker.record.cell_z + blocker.record.footprint_z &&
            mover.record.cell_z + mover.record.footprint_z > blocker.record.cell_z;
        CHECK(!overlap);
        head = match.local_flow_head(mover.unit_index);
        CHECK(head != nullptr);
        if (head->active)
            CHECK(head->tick == match.state().game.tick);
        if (!result.completion_tick && !head->active &&
            head->count == oa::sim::unit_movement::local_flow_completed_count &&
            head->tick == match.state().game.tick) {
            result.completion_tick = head->tick;
            CHECK(oa::sim::unit_movement::local_flow_completed_last_tick(*head, head->tick + 1));
            if (restore_completed) {
                const auto* table = runtime::find_rule_state(match.rule_state(), "local-flow");
                CHECK(table != nullptr && table->bytes != nullptr && table->restore != nullptr);
                const auto bytes = table->bytes(table->context);
                std::vector<uint8_t> saved(bytes.begin(), bytes.end());
                CHECK(
                    saved.size() ==
                    match.state().unit_slot_count * oa::sim::unit_movement::local_flow_head_bytes
                );
                const std::vector<uint8_t> empty(saved.size());
                CHECK(table->restore(table->context, empty));
                CHECK(match.local_flow_head(mover.unit_index)->count == 0);
                CHECK(table->restore(table->context, saved));
                const auto restored = table->bytes(table->context);
                CHECK(std::equal(saved.begin(), saved.end(), restored.begin(), restored.end()));
                CHECK(
                    oa::sim::unit_movement::local_flow_completed_last_tick(*head, head->tick + 1)
                );
            }
        }
        // Restoring the completion marker preserves the subsequent native
        // trajectory, motion clock and effective-head state on every tick.
        const auto& ground = *match.ground_runtime(mover.unit_index);
        for (const auto value :
             {uint32_t(mover.record.position.x),
              uint32_t(mover.record.position.z),
              uint32_t(mover.record.heading),
              uint32_t(ground.movement.speed),
              ground.movement.last_motion_tick,
              uint32_t(head->active),
              uint32_t(head->count),
              head->tick}) {
            result.digest ^= value;
            result.digest *= 1099511628211ull;
        }
        if (ground.navigation.goal == nullptr) {
            result.arrival_tick = match.state().game.tick;
            break;
        }
    }
    CHECK(match.ground_runtime(mover.unit_index)->navigation.goal == nullptr);
    CHECK(std::abs((mover.record.position.x >> 16) - 200) <= 16);
    CHECK(std::abs((mover.record.position.z >> 16) - 64) <= 16);
    return result;
}

/// Keeps parked traffic solid and preserves completed-leg continuation after restore.
void idle_mobile_occupants_remain_passable_traffic() {
    const auto original = run_idle_mobile_occupant(false);
    const auto restored = run_idle_mobile_occupant(true);
    CHECK(original.completion_tick != 0);
    CHECK(original.arrival_tick > original.completion_tick);
    CHECK(original == restored);
}
} // namespace

int main() {
    try {
        traffic_probes_respect_static_terrain_and_fog();
        idle_mobile_occupants_remain_passable_traffic();
        std::cout << "local flow terrain: ok\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
