// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Enhanced Mode features that only skip work whose answer is known play
// exactly the battle their set plays without them: two columns meet head on
// in a narrow passage, every unit's script sleeps and wakes, and every tick's
// world, traffic heads and script threads are the same with the feature as
// without it.
#include "combat_fixture.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/match_runtime/enhanced_counters.hpp"
#include "oa/sim/script_vm.hpp"
#include "oa/sim/state_hash.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace enhanced = oa::data::enhanced_mode;
namespace movement = oa::sim::unit_movement;
using enhanced::Feature;
using enhanced::FeatureSet;

constexpr int32_t pixel = 65536;
// Map cells across and down: a room at each end joined by a passage five
// cells wide, two two-by-two units abreast.
constexpr uint32_t width = 96, height = 32;
constexpr uint32_t column_size = 8, unit_count = column_size * 2;
constexpr uint32_t march_ticks = 1200;
constexpr uint8_t land_height = 60, sea_level = 30;
// The passage's first and last columns, and the row both columns march along.
constexpr int16_t passage_start = 40, passage_end = 56, lane = 8;
// How long each unit's script sleeps between wakes, in milliseconds.
constexpr uint32_t script_sleep_ms = 200;

/// The features that only skip work, each judged against the set without it.
constexpr std::array skips{Feature::idle_goal_skip};

/// Returns a two-by-two unit's centre on a cell, in whole world units.
///
/// @param cell the unit's first cell
/// @return the centre
constexpr int16_t centre(int16_t cell) {
    return int16_t(cell * 16 + 16);
}

/// Tells whether a cell is dry land: the two rooms, and the passage between them.
///
/// @param x cell column
/// @param z cell row
/// @return true for land
bool land(uint32_t x, uint32_t z) {
    const bool room =
        (x >= 1 && x < uint32_t(passage_start)) || (x > uint32_t(passage_end) && x < width - 1);
    return room ? z >= 3 && z <= 23
                : x >= uint32_t(passage_start) && x <= uint32_t(passage_end) && z >= 8 && z <= 12;
}

/// The candidate set the salvage tickets judge against (C9).
///
/// @return its features
FeatureSet candidate_features() {
    FeatureSet features = enhanced::version_features(13);
    for (const Feature cut :
         {Feature::moving_corner_advance,
          Feature::indexed_searches,
          Feature::patrol_goal_retention,
          Feature::bounded_strike_recruits,
          Feature::squad_position_index,
          Feature::swept_ground_steps,
          Feature::group_destination_repair,
          Feature::distributed_arrivals,
          Feature::local_throat_guide,
          Feature::cohort_targets,
          Feature::local_following,
          Feature::route_queue,
          Feature::cardinal_queue_escape})
        features = features.without(cut);
    for (const Feature added :
         {Feature::exact_indexed_searches,
          Feature::wide_strike_recruits,
          Feature::paced_cohort_targets,
          Feature::dispatch_cohort_retention,
          Feature::counterflow_queue_escape})
        features = features.with(added);
    return features;
}

/// What one march saw, tick by tick.
struct March {
    // One fold of the world, the units' traffic heads and their script
    // threads after each tick.
    std::vector<uint64_t> ticks;
    // Script wakes: ticks on which the first unit's script counted a wake.
    uint32_t script_wakes{};
    // Idle heads: units that began a tick with an inactive traffic head long
    // blocked and no recovery due, summed over the ticks.
    uint32_t idle_heads{};
    // Local traffic probes over the march.
    uint64_t probes{};
};

/// Folds one value into an FNV-1a digest.
///
/// @param digest the digest so far
/// @param value the value
void fold(uint64_t& digest, uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        digest ^= uint8_t(value >> (byte * 8));
        digest *= 1099511628211ull;
    }
}

/// Builds the passage and the two columns, orders each across, and plays
/// march_ticks ticks with a feature set.
///
/// @param features the set the match plays
/// @return what the march saw
March march(FeatureSet features) {
    Options options;
    options.sight_cells = width + 1;
    options.per_player_limit = column_size;
    Fixture fixture(options);
    fixture.match.reset();
    fixture.map.attribute_width = width;
    fixture.map.attribute_height = height;
    fixture.map.sea_level = sea_level;
    fixture.map.attributes.resize(size_t(width) * height);
    fixture.terrain_values.resize(size_t(width) * height);
    fixture.collision_plots.resize(size_t(width) * height);
    fixture.altitude_cells.resize(size_t(width / 2) * (height / 2));
    for (uint32_t z = 0; z < height; ++z)
        for (uint32_t x = 0; x < width; ++x) {
            const bool high = land(x, z) || (x && land(x - 1, z)) || (z && land(x, z - 1)) ||
                              (x && z && land(x - 1, z - 1));
            fixture.map.attributes[size_t(z) * width + x].height = high ? land_height : 0;
        }
    for (uint32_t z = 0; z < height; ++z)
        for (uint32_t x = 0; x < width; ++x) {
            auto& plot = fixture.collision_plots[size_t(z) * width + x];
            plot.low_height = plot.high_height =
                fixture.map.attributes[size_t(z) * width + x].height;
            for (uint32_t dz = 0; dz < 2 && z + dz < height; ++dz)
                for (uint32_t dx = 0; dx < 2 && x + dx < width; ++dx) {
                    const auto h = fixture.map.attributes[size_t(z + dz) * width + x + dx].height;
                    plot.low_height = std::min(plot.low_height, h);
                    plot.high_height = std::max(plot.high_height, h);
                }
        }
    // Create loops for ever: it counts a wake in static 0, then sleeps.
    {
        using namespace oa::sim::script_vm;
        fixture.script->code = {
            opcode::push_static,
            0,
            opcode::push_constant,
            1,
            opcode::add,
            opcode::pop_static,
            0,
            opcode::push_constant,
            script_sleep_ms,
            opcode::sleep,
            opcode::jump,
            0,
        };
        fixture.script->scripts = {{"Create", 0}};
        fixture.script->entry_points = {0};
        fixture.script->header.static_variable_count = 1;
    }
    fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
    fixture.types[1].simulation.flags &= ~OA_UNIT_DEF_FLAG_HAS_WEAPONS;
    fixture.loaded[1].type = fixture.types[1];
    fixture.fields[1].movement_class = 1;
    fixture.metadata.footprint_x = fixture.metadata.footprint_z = 2;
    fixture.metadata.min_water_depth = -10000;
    fixture.metadata.max_water_depth = 0;
    fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
    fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
    fixture.def.weapon1.clear();
    fixture.def.can_attack = false;
    fixture.def.max_velocity_fixed = 117964;
    fixture.def.acceleration_fixed = fixture.def.brake_rate_fixed = 6553;
    fixture.def.turn_rate = 1120;
    runtime::OfflineInputs input{
        fixture.map,
        fixture.loaded,
        fixture.types,
        fixture.fields,
        fixture.weapons,
        fixture.terrain_values,
        fixture.masks,
        int32_t(width / 2),
        int32_t(height / 2),
        uint16_t(column_size),
        2,
        0,
        sea_level,
        1,
        &fixture.scenario,
        {},
        fixture.collision_plots,
        oa::sim::visibility_state::AltitudeSightData{
            int32_t(width / 2),
            int32_t(height / 2),
            fixture.altitude_cells,
            fixture.altitude_patterns
        }
    };
    enhanced::apply_alias(input.rules.setup.enhanced_mode, 13);
    input.enhanced_features_override = features;
    input.unit_player_mask = 3;
    fixture.match = std::make_unique<runtime::Match>(input, fixture.services);
    auto& match = *fixture.match;
    CHECK(match.fault() == nullptr);
    match.configure_strategic_environment({0, 0.5F, 0});
    for (uint8_t owner = 0; owner < 2; ++owner) {
        match.simulation().players[owner].present = true;
        match.simulation().players[owner].status = 1;
        std::array<uint8_t, OA_PLAYER_COUNT> allies{};
        allies[0] = allies[1] = 1;
        match.configure_player_alliances(owner, allies);
        if (!owner)
            match.configure_outcomes(owner, allies, false);
    }
    std::array<uint16_t, unit_count> ids{};
    std::array<int16_t, unit_count> destinations{};
    for (uint32_t i = 0; i < unit_count; ++i) {
        const bool eastward = i < column_size;
        const auto rank = int16_t(i % column_size);
        const auto x = int16_t(eastward ? 36 - rank * 3 : 60 + rank * 3);
        destinations[i] = int16_t(eastward ? 90 - rank * 3 : 3 + rank * 3);
        auto* slot = match.create(
            {uint8_t(eastward ? 0 : 1),
             1,
             {uint32_t(centre(x) * pixel),
              uint32_t(land_height * pixel),
              uint32_t(centre(lane) * pixel)},
             true,
             1,
             0}
        );
        CHECK(slot && slot->unit);
        slot->unit->flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
        slot->record.heading = eastward ? 49152 : 16384;
        match.place_unit_at(
            slot->unit_index, centre(x) * pixel, land_height * pixel, centre(lane) * pixel, 1
        );
        ids[i] = slot->unit_index;
        CHECK(match.instance(slot->unit_index) && match.instance(slot->unit_index)->script());
    }
    fixture.run(60);
    // The whole map is known, so traffic probes classify every cell.
    std::fill(
        match.sight_mutable().player_bits.begin(),
        match.sight_mutable().player_bits.end(),
        uint16_t{3}
    );
    for (uint32_t i = 0; i < unit_count; ++i)
        CHECK(!match.order_refused(match.issue_ground_move(
            ids[i], {centre(destinations[i]) * pixel, 0, centre(lane) * pixel}, false
        )));
    March result;
    int32_t wakes = 0;
    for (uint32_t tick = 0; tick < march_ticks; ++tick) {
        const auto next_tick = match.simulation().tick + 1;
        for (const auto id : ids) {
            const auto* head = match.local_flow_head(id);
            if (head && !head->active && head->blocked_ticks >= movement::local_flow_retry_ticks &&
                !movement::local_flow_recovery_due(*head, next_tick, id) &&
                match.ground_runtime(id)->navigation.goal != nullptr)
                ++result.idle_heads;
        }
        fixture.run(1);
        uint64_t digest = 14695981039346656037ull;
        fold(
            digest,
            oa::sim::trace::match_state_hash(match, oa::base::game_loop::Timing{}, 0, 0, nullptr)
        );
        for (const auto id : ids) {
            if (const auto* head = match.local_flow_head(id)) {
                for (const auto& point : head->points) {
                    fold(digest, uint16_t(point[0]));
                    fold(digest, uint16_t(point[1]));
                }
                fold(digest, head->goal_key);
                fold(digest, head->tick);
                fold(digest, head->count);
                fold(digest, head->active);
                fold(digest, head->mobile_blocked);
                fold(digest, head->blocked_ticks);
            }
            auto& vm = match.instance(id)->script()->vm();
            fold(digest, uint32_t(vm.static_value(0).value_or(-1)));
            fold(digest, vm.active_count());
            for (std::size_t context = 0; context < oa::sim::script_vm::context_count; ++context) {
                const auto snapshot = vm.context(context);
                fold(digest, uint32_t(snapshot.state));
                fold(digest, uint32_t(snapshot.sleep_remaining));
                fold(digest, snapshot.pc);
            }
        }
        result.ticks.push_back(digest);
        const auto counted = match.instance(ids[0])->script()->vm().static_value(0).value_or(0);
        if (counted != wakes)
            ++result.script_wakes;
        wakes = counted;
    }
    result.probes = match.enhanced_counters().local_flow_probes;
    return result;
}

/// Plays a set with and without each skip and compares every tick.
///
/// @param base the set without the skips
void skips_play_the_same_battle(FeatureSet base) {
    CHECK(enhanced::feature_set_problem(base) == nullptr);
    const auto plain = march(base);
    // The march exercises what the skips skip: sleeping scripts that wake
    // between quiet ticks, local traffic choices, and long-blocked heads
    // waiting for recovery.
    CHECK(plain.script_wakes > 2 && plain.script_wakes < march_ticks / 2);
    CHECK(plain.probes > 0);
    CHECK(plain.idle_heads > 0);
    for (const Feature skip : skips) {
        CHECK(!base.has(skip));
        const auto features = base.with(skip);
        CHECK(enhanced::feature_set_problem(features) == nullptr);
        const auto skipped = march(features);
        CHECK(skipped.ticks.size() == plain.ticks.size());
        for (std::size_t tick = 0; tick < plain.ticks.size(); ++tick)
            if (skipped.ticks[tick] != plain.ticks[tick]) {
                std::fprintf(
                    stderr,
                    "%s parts from its set at march tick %zu\n",
                    std::string(enhanced::feature_name(skip)).c_str(),
                    tick
                );
                CHECK(false);
                break;
            }
        CHECK(skipped.script_wakes == plain.script_wakes && skipped.probes == plain.probes);
        CHECK(skipped.idle_heads == plain.idle_heads);
    }
}
} // namespace

int main() {
    try {
        skips_play_the_same_battle(enhanced::version_features(13));
        skips_play_the_same_battle(candidate_features());
        std::cout << "enhanced skips play the same battle\n";
    } catch (const std::exception& error) {
        std::cerr << "enhanced skips: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
