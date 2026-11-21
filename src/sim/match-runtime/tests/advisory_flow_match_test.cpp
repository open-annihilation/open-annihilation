// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Advisory lanes enroll live routes after startup, preserve native movement,
// and resume pending discovery and an entered crossing through saved rule state.
#include "oa/data/enhanced_mode.hpp"
#include "combat_fixture.hpp"
#include "support/match_tick_access.hpp"

#include "oa/netgame/match/match_binding.hpp"
#include "oa/netgame/unit_state.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime/match_trace.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;
namespace net = oa::netgame;
namespace binding = oa::netgame::match;

constexpr int32_t pixel = 65536;
// Both banks and the crossing stay inside the native hidden map margins.
constexpr uint32_t map_width = 24;
constexpr uint32_t sight_width = map_width / 2;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint8_t ground_occupancy = 1;
constexpr uint8_t ground_footprint = 2;
constexpr uint16_t east = 49152;
constexpr uint32_t idle_ticks = 19;
constexpr uint32_t crossing_limit = 600;
constexpr ground::RoutePoint start{48, 128};
constexpr ground::RoutePoint destination{224, 128};
constexpr uint8_t approach_phase = 1;
constexpr uint8_t crossing_phase = 2;
constexpr int16_t exit_plot = 11;
constexpr uint32_t ordinary_budget = 4096;

enum class Driver { owned, remote, mirrored, carried, aircraft };

struct Setup {
    uint32_t version = 5;
    bool known = true;
    bool supported_class = true;
    Driver driver = Driver::owned;
};

/// Returns the two wide banks and four-plot-wide horizontal crossing.
bool clear_plot(uint32_t x, uint32_t z) {
    const bool bank = (x >= 1 && x <= 4) || (x >= 11 && x <= 14);
    const bool crossing = x >= 5 && x <= 10 && z >= 6 && z <= 9;
    return (bank && z >= 2 && z <= 13) || crossing;
}

/// A land plot needs all four of its TNT height vertices above the water.
bool land_vertex(uint32_t x, uint32_t z) {
    return clear_plot(x, z) || (x != 0 && clear_plot(x - 1, z)) ||
           (z != 0 && clear_plot(x, z - 1)) || (x != 0 && z != 0 && clear_plot(x - 1, z - 1));
}

Options options_for(const Setup& setup) {
    Options options;
    options.sight_cells = setup.known ? map_width + 1 : 1;
    return options;
}

// The real Match owns all navigation, occupancy, fog and movement. The fixture
// supplies only terrain and a mobile type with a full physical footprint.
struct Crossing {
    ground::Goal excluded_goal{};
    Fixture fixture;
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    Driver driver{};
    uint16_t mover{};

    explicit Crossing(const Setup& setup = {}) : fixture(options_for(setup)), driver(setup.driver) {
        fixture.match.reset();
        fixture.map.attribute_width = fixture.map.attribute_height = map_width;
        fixture.map.attributes.resize(map_width * map_width);
        fixture.terrain_values.resize(map_width * map_width);
        fixture.collision_plots.resize(map_width * map_width);
        fixture.altitude_cells.resize(sight_width * sight_width);
        fixture.map.sea_level = sea_level;
        for (uint32_t index = 0; index < fixture.map.attributes.size(); ++index) {
            fixture.map.attributes[index].height =
                land_vertex(index % map_width, index / map_width) ? land_height : 0;
        }
        for (uint32_t z = 0; z < map_width; ++z)
            for (uint32_t x = 0; x < map_width; ++x) {
                auto& plot = fixture.collision_plots[z * map_width + x];
                plot.low_height = plot.high_height =
                    fixture.map.attributes[z * map_width + x].height;
                for (uint32_t dz = 0; dz < 2 && z + dz < map_width; ++dz)
                    for (uint32_t dx = 0; dx < 2 && x + dx < map_width; ++dx) {
                        const auto height =
                            fixture.map.attributes[(z + dz) * map_width + x + dx].height;
                        plot.low_height = std::min(plot.low_height, height);
                        plot.high_height = std::max(plot.high_height, height);
                    }
            }
        fixture.types[1].footprint_x = fixture.types[1].footprint_z = ground_footprint;
        if (setup.driver == Driver::aircraft) {
            fixture.types[1].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
            fixture.def.can_fly = true;
            fixture.def.cruise_altitude = 80;
        }
        fixture.loaded[1].type = fixture.types[1];
        fixture.fields[1].movement_class = setup.supported_class ? 1 : 0;
        fixture.fields[1].yard_mask = yard;
        fixture.metadata.footprint_x = fixture.metadata.footprint_z = ground_footprint;
        fixture.metadata.min_water_depth = -10000;
        fixture.metadata.max_water_depth = 0;
        fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
        fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
        fixture.def.max_velocity_fixed = 117964;
        fixture.def.acceleration_fixed = fixture.def.brake_rate_fixed = 6553;
        fixture.def.turn_rate = 1120;
        runtime::OfflineInputs inputs{
            fixture.map,
            fixture.loaded,
            fixture.types,
            fixture.fields,
            fixture.weapons,
            fixture.terrain_values,
            fixture.masks,
            sight_width,
            sight_width,
            4,
            2,
            0,
            sea_level,
            1,
            &fixture.scenario,
            {},
            fixture.collision_plots,
            oa::sim::visibility_state::AltitudeSightData{
                sight_width, sight_width, fixture.altitude_cells, fixture.altitude_patterns
            }
        };
        oa::data::enhanced_mode::apply_alias(
            inputs.rules.setup.enhanced_mode, static_cast<uint32_t>(setup.version)
        );
        inputs.unit_player_mask = 3;
        fixture.match = std::make_unique<runtime::Match>(inputs, fixture.services);
        auto& match = *fixture.match;
        match.configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t owner = 0; owner < 2; ++owner) {
            match.simulation().players[owner].present = true;
            match.simulation().players[owner].status = 1;
            std::array<uint8_t, OA_PLAYER_COUNT> allies{};
            allies[owner] = 1;
            match.configure_player_alliances(owner, allies);
            if (owner == 0)
                match.configure_outcomes(owner, allies, false);
        }
        CHECK(match.fault() == nullptr);
        CHECK((match.movement_map(1) != nullptr) == setup.supported_class);
        auto& unit = fixture.spawn(0, start[0], start[1]);
        mover = unit.unit_index;
        match.place_unit_at(
            mover, start[0] * pixel, land_height * pixel, start[1] * pixel, ground_occupancy
        );
        unit.record.heading = east;
    }

    runtime::Match& match() { return *fixture.match; }

    ground::GroundRuntime& movement() {
        auto* state = match().ground_runtime(mover);
        CHECK(state != nullptr);
        return *state;
    }

    void move() {
        CHECK(!match().order_refused(
            match().issue_ground_move(
                mover, {destination[0] * pixel, 0, destination[1] * pixel}, false
            )
        ));
    }

    void run(uint32_t ticks = 1) { fixture.run(ticks); }

    /// Publishes a clear canonical route after the ordinary order creates its Goal.
    void route() {
        move();
        run();
        auto& state = movement();
        if (driver == Driver::aircraft && !state.navigation.goal) {
            excluded_goal.cell = {14, 8};
            excluded_goal.order = match().spawn_slot(mover).unit->primary;
            state.navigation.goal = &excluded_goal;
        }
        CHECK(state.navigation.goal != nullptr);
        state.navigation.points = {};
        state.navigation.points[0] = {
            static_cast<int16_t>(state.geometry.position[0] >> 16),
            static_cast<int16_t>(state.geometry.position[2] >> 16)
        };
        state.navigation.points[1] = {112, 128};
        state.navigation.points[2] = {160, 128};
        state.navigation.points[3] = destination;
        state.navigation.count = 4;
        state.navigation.flags = ground::route_present_flag;
        state.navigation.last_search_tick = match().simulation().tick;
    }
};

/// Copies the exact owner-local navigation state through its ordinary save seam.
std::vector<uint8_t> rule_bytes(runtime::Match& match, const char* name) {
    const auto* state = runtime::find_rule_state(match.rule_state(), name);
    CHECK(state != nullptr && state->bytes != nullptr && state->restore != nullptr);
    const auto bytes = state->bytes(state->context);
    return {bytes.begin(), bytes.end()};
}

void restore_rule(runtime::Match& match, const char* name, std::span<const uint8_t> bytes) {
    const auto* state = runtime::find_rule_state(match.rule_state(), name);
    CHECK(state != nullptr && state->restore != nullptr);
    CHECK(state->restore(state->context, bytes));
    CHECK(rule_bytes(match, name) == std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

struct NativeState {
    std::array<int32_t, 3> position{}, velocity{}, previous_vector{};
    std::array<int16_t, 2> cell{};
    std::array<ground::RoutePoint, ground::navigation_capacity> route{};
    int32_t speed{};
    uint32_t count{}, flags{}, search_tick{}, motion_tick{}, occupancy_tick{};
    uint16_t heading{};
    int16_t turn{};
    uint8_t movement_flags{};
    bool goal{}, order{};
    bool operator==(const NativeState&) const = default;
};

NativeState native_state(Crossing& crossing, uint16_t unit = 0) {
    const auto* ground_state = crossing.match().ground_runtime(unit == 0 ? crossing.mover : unit);
    CHECK(ground_state != nullptr);
    const auto& state = *ground_state;
    return {
        state.geometry.position,
        state.movement.velocity,
        state.previous_vector,
        state.geometry.cell,
        state.navigation.points,
        state.movement.speed,
        state.navigation.count,
        state.navigation.flags,
        state.navigation.last_search_tick,
        state.movement.last_motion_tick,
        state.occupancy_changed_tick,
        state.geometry.heading,
        state.movement.turn,
        state.movement.flags,
        state.navigation.goal != nullptr,
        state.navigation.goal && state.navigation.goal->order
    };
}

uint64_t match_digest(Crossing& crossing) {
    namespace trace = oa::sim::trace;
    std::vector<trace::UnitSide> sides(crossing.match().state().unit_slot_count);
    const auto digest = runtime::match_tick_digest(crossing.match(), sides);
    return digest.sections[static_cast<size_t>(trace::Section::total)].value;
}

void bounded_work(Crossing& crossing) {
    const auto budget = runtime::MatchTickAccess::advisory_budget(crossing.match());
    CHECK(budget.used <= budget.limit);
    CHECK(budget.remaining + budget.used == budget.limit);
}

/// Every full footprint stays on the authoritative land plots after physical movement.
void clear_footprint(Crossing& crossing) {
    const auto& unit = crossing.movement().geometry;
    for (int16_t z = 0; z < unit.footprint[1]; ++z)
        for (int16_t x = 0; x < unit.footprint[0]; ++x) {
            CHECK(unit.cell[0] + x >= 0 && unit.cell[1] + z >= 0);
            CHECK(clear_plot(uint32_t(unit.cell[0] + x), uint32_t(unit.cell[1] + z)));
        }
}

void describe(const char* label, Crossing& crossing) {
    const auto& native = crossing.movement();
    const auto lane = runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
    const auto catalog =
        runtime::MatchTickAccess::advisory_catalog(crossing.match(), crossing.mover);
    std::cerr << label << " tick=" << crossing.match().simulation().tick
              << " pose=" << native.geometry.position[0] << ',' << native.geometry.position[2]
              << " cell=" << native.geometry.cell[0] << ',' << native.geometry.cell[1]
              << " nav=" << native.navigation.count << '/' << unsigned(native.navigation.flags)
              << " goal=" << (native.navigation.goal != nullptr) << " guide=" << lane.active << '/'
              << lane.entered << '/' << unsigned(lane.phase) << " corridor=" << lane.corridor
              << " catalog=" << catalog.corridors << '/' << catalog.pending_seeds << '/'
              << catalog.building << " work=" << catalog.work_units
              << " samples=" << catalog.samples << " rejected=" << catalog.rejected_seeds << '\n';
}

/// Discovery and enrollment are recurring work, including a unit's first late order.
void first_route_after_startup_receives_advice() {
    Crossing crossing;
    crossing.run(idle_ticks);
    CHECK(crossing.match().simulation().tick > 2);
    CHECK(!runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover).active);
    crossing.route();
    const auto* goal = crossing.movement().navigation.goal;
    const auto* order = goal->order;
    bool advised{}, entered{}, exited{};
    for (uint32_t tick = 0; tick < crossing_limit && crossing.movement().navigation.goal; ++tick) {
        crossing.run();
        bounded_work(crossing);
        clear_footprint(crossing);
        const auto lane = runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
        if (lane.active && !advised)
            CHECK(runtime::MatchTickAccess::advisory_budget(crossing.match()).active_used == 0);
        advised |= lane.active;
        entered |= lane.entered;
        if (lane.active) {
            CHECK(lane.supported && lane.corridor != 0);
            CHECK(crossing.movement().navigation.goal == goal);
            CHECK(goal->order == order);
        }
        exited |= crossing.movement().geometry.cell[0] > exit_plot;
    }
    if (!(advised && entered && exited)) {
        std::cerr << "late observed advised/entered/exited=" << advised << '/' << entered << '/'
                  << exited << '\n';
        describe("late-failure", crossing);
    }
    CHECK(advised && entered && exited);
    CHECK(crossing.movement().navigation.goal == nullptr);
    CHECK(crossing.movement().geometry.position[0] >= 208 * pixel);
}

/// Retained approaches use reserved work after new-route queries are exhausted.
void retained_approach_survives_exhausted_new_queries() {
    Crossing crossing;
    crossing.run(idle_ticks);
    crossing.route();
    runtime::MatchTickAccess::AdvisoryLaneInspection retained{};
    bool approaching{};
    for (uint32_t tick = 0; tick < crossing_limit; ++tick) {
        crossing.run();
        retained = runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
        if (retained.active && !retained.entered && retained.phase == approach_phase) {
            approaching = true;
            break;
        }
    }
    if (!approaching)
        describe("reserved approach setup", crossing);
    CHECK(approaching);
    const uint32_t before_tick = crossing.match().simulation().tick;
    const auto before_position = crossing.movement().geometry.position;
    const auto reserved = runtime::MatchTickAccess::advisory_budget(crossing.match());
    CHECK(reserved.active_used == 0 && reserved.active_remaining != 0);
    runtime::MatchTickAccess::tick_without_advisory_queries(crossing.match());
    CHECK(crossing.match().fault() == nullptr);
    CHECK(crossing.match().simulation().tick == before_tick + 1);
    const auto& native = crossing.movement();
    CHECK(native.geometry.position != before_position);
    for (const size_t axis : {size_t{0}, size_t{2}})
        CHECK(
            native.geometry.position[axis] == before_position[axis] + native.movement.velocity[axis]
        );
    const auto next = runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
    if (!next.active)
        describe("reserved approach withdrawal", crossing);
    CHECK(next.active && next.corridor == retained.corridor);
    CHECK(next.lane == retained.lane && next.direction == retained.direction);
    CHECK(next.generation == retained.generation);
    CHECK(next.phase == approach_phase || next.phase == crossing_phase);
    if (next.entered)
        CHECK(
            native.geometry.cell[0] <= 11 &&
            native.geometry.cell[0] + native.geometry.footprint[0] - 1 >= 4 &&
            native.geometry.cell[1] <= 9 &&
            native.geometry.cell[1] + native.geometry.footprint[1] - 1 >= 6
        );
    const auto budget = runtime::MatchTickAccess::advisory_budget(crossing.match());
    CHECK(budget.query_remaining == 0);
    CHECK(budget.active_used != 0);
    CHECK(budget.active_remaining + budget.active_used == reserved.active_remaining);
    bounded_work(crossing);
    clear_footprint(crossing);
}

/// A replacement same-Goal route keeps its crossing lane, then requests ordinary work.
void same_goal_reroute_cannot_revive_an_obsolete_waypoint() {
    Crossing crossing;
    crossing.run(idle_ticks);
    crossing.route();
    bool entered{};
    runtime::MatchTickAccess::AdvisoryLaneInspection admitted{};
    for (uint32_t tick = 0; tick < crossing_limit; ++tick) {
        crossing.run();
        admitted = runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
        if (admitted.active && admitted.entered && admitted.phase == crossing_phase &&
            crossing.movement().geometry.cell[0] >= 6) {
            entered = true;
            break;
        }
    }
    if (!entered)
        describe("reroute entry", crossing);
    CHECK(entered);
    auto& navigation = crossing.movement().navigation;
    const auto* goal = navigation.goal;
    CHECK(goal != nullptr);
    const auto* order = goal->order;
    constexpr ground::RoutePoint obsolete{48, 128};
    navigation.points = {};
    navigation.points[0] = {
        static_cast<int16_t>(crossing.movement().geometry.position[0] >> 16),
        static_cast<int16_t>(crossing.movement().geometry.position[2] >> 16)
    };
    navigation.points[1] = obsolete;
    navigation.points[2] = {112, 128};
    navigation.points[3] = destination;
    navigation.count = 4;
    navigation.flags = ground::route_present_flag;
    navigation.last_search_tick = crossing.match().simulation().tick;
    bool released{};
    int32_t egress_x{};
    for (uint32_t tick = 0; tick < crossing_limit && navigation.goal; ++tick) {
        crossing.run();
        bounded_work(crossing);
        clear_footprint(crossing);
        const auto lane = runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
        if (!released && lane.entered) {
            CHECK(lane.corridor == admitted.corridor);
            CHECK(lane.lane == admitted.lane);
            CHECK(lane.direction == admitted.direction);
            CHECK(navigation.goal == goal && goal->order == order);
        }
        if (!released && crossing.movement().geometry.cell[0] > exit_plot) {
            const uint32_t egress_tick = crossing.match().simulation().tick;
            egress_x = crossing.movement().geometry.position[0];
            // Advice observes the pre-motor pose. The next callback must
            // withdraw the crossing before it can steer along the old P1.
            crossing.run();
            CHECK(crossing.match().simulation().tick == egress_tick + 1);
            bounded_work(crossing);
            clear_footprint(crossing);
            const auto next =
                runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
            if (next.active)
                describe("egress next callback", crossing);
            CHECK(!next.active);
            CHECK(navigation.count < 2 || navigation.points[1] != obsolete);
            CHECK(crossing.movement().geometry.cell[0] > exit_plot);
            released = true;
        }
        if (released)
            CHECK(crossing.movement().geometry.position[0] >= egress_x);
    }
    CHECK(released);
    CHECK(navigation.goal == nullptr);
}

void restore_rules(Crossing& first, Crossing& restored) {
    CHECK(native_state(first) == native_state(restored));
    const auto& source = first.match().rule_state();
    CHECK(source.count == restored.match().rule_state().count);
    for (uint8_t index = 0; index < source.count; ++index) {
        const auto& table = source.tables[index];
        CHECK(table.name != nullptr);
        restore_rule(restored.match(), table.name, rule_bytes(first.match(), table.name));
    }
    CHECK(match_digest(first) == match_digest(restored));
}

/// The save seam reproduces the exact next native ticks and all owner-local rule bytes.
void compare_continuation(Crossing& first, Crossing& restored) {
    CHECK(native_state(first) == native_state(restored));
    CHECK(match_digest(first) == match_digest(restored));
    const auto& source = first.match().rule_state();
    for (uint32_t tick = 0; tick < crossing_limit && first.movement().navigation.goal; ++tick) {
        first.run();
        restored.run();
        CHECK(native_state(first) == native_state(restored));
        CHECK(match_digest(first) == match_digest(restored));
        for (uint8_t index = 0; index < source.count; ++index) {
            const auto* name = source.tables[index].name;
            CHECK(rule_bytes(first.match(), name) == rule_bytes(restored.match(), name));
        }
    }
    if (first.movement().navigation.goal)
        describe("saved continuation", first);
    CHECK(first.movement().navigation.goal == nullptr);
    CHECK(restored.movement().navigation.goal == nullptr);
}

void pending_discovery_survives_restore() {
    Crossing first;
    Crossing restored;
    const auto empty = rule_bytes(restored.match(), "advisory-flow");
    first.run(idle_ticks);
    restored.run(idle_ticks);
    runtime::MatchTickAccess::set_advisory_budget(first.match(), 8);
    runtime::MatchTickAccess::set_advisory_budget(restored.match(), 8);
    first.route();
    restored.route();
    bool pending{};
    for (uint32_t tick = 0; tick < crossing_limit; ++tick) {
        const auto status = runtime::MatchTickAccess::advisory_catalog(first.match(), first.mover);
        if (status.pending_seeds != 0 || status.building) {
            pending = true;
            break;
        }
        first.run();
        restored.run();
    }
    CHECK(pending);
    const auto saved = rule_bytes(first.match(), "advisory-flow");
    CHECK(saved != empty);
    restore_rule(restored.match(), "advisory-flow", empty);
    CHECK(rule_bytes(restored.match(), "advisory-flow") != saved);
    runtime::MatchTickAccess::set_advisory_budget(first.match(), ordinary_budget);
    runtime::MatchTickAccess::set_advisory_budget(restored.match(), ordinary_budget);
    restore_rules(first, restored);
    compare_continuation(first, restored);
}

void entered_crossing_survives_restore() {
    Crossing first;
    Crossing restored;
    const auto empty = rule_bytes(restored.match(), "advisory-flow");
    first.run(idle_ticks);
    restored.run(idle_ticks);
    first.route();
    restored.route();
    bool entered{};
    for (uint32_t tick = 0; tick < crossing_limit; ++tick) {
        const auto lane = runtime::MatchTickAccess::advisory_lane(first.match(), first.mover);
        if (lane.active && lane.entered) {
            entered = true;
            break;
        }
        first.run();
        restored.run();
    }
    if (!entered)
        describe("saved entry", first);
    CHECK(entered);
    restore_rule(restored.match(), "advisory-flow", empty);
    CHECK(!runtime::MatchTickAccess::advisory_lane(restored.match(), restored.mover).active);
    restore_rules(first, restored);
    compare_continuation(first, restored);
}

/// Replacing the current order withdraws a loaded guide before its pointers are rebound.
void replaced_order_withdraws_pending_saved_rebind() {
    Crossing first;
    Crossing restored;
    first.run(idle_ticks);
    restored.run(idle_ticks);
    first.route();
    restored.route();
    bool entered{};
    for (uint32_t tick = 0; tick < crossing_limit; ++tick) {
        const auto lane = runtime::MatchTickAccess::advisory_lane(first.match(), first.mover);
        if (lane.active && lane.entered) {
            entered = true;
            break;
        }
        first.run();
        restored.run();
    }
    if (!entered)
        describe("replacement entry", first);
    CHECK(entered);
    restore_rules(first, restored);
    const auto before = runtime::MatchTickAccess::advisory_lane(first.match(), first.mover);
    CHECK(before.active);
    CHECK(runtime::MatchTickAccess::advisory_lane(restored.match(), restored.mover).active);
    CHECK(first.match().simulation().tick == restored.match().simulation().tick);
    first.move();
    restored.move();
    const auto original = runtime::MatchTickAccess::advisory_lane(first.match(), first.mover);
    const auto loaded = runtime::MatchTickAccess::advisory_lane(restored.match(), restored.mover);
    CHECK(!original.active && !loaded.active && original.phase == 0 && loaded.phase == 0);
    CHECK(original.generation > before.generation && original.generation == loaded.generation);
    CHECK(
        rule_bytes(first.match(), "advisory-flow") == rule_bytes(restored.match(), "advisory-flow")
    );
    first.run();
    restored.run();
    CHECK(first.movement().navigation.goal != nullptr);
    compare_continuation(first, restored);
}

uint64_t saved_integer(std::span<const uint8_t> bytes, size_t offset, size_t width) {
    CHECK(width <= sizeof(uint64_t));
    CHECK(offset <= bytes.size() && width <= bytes.size() - offset);
    uint64_t value{};
    for (size_t byte = 0; byte < width; ++byte)
        value |= uint64_t(bytes[offset + byte]) << (byte * 8);
    return value;
}

/// A malformed late record cannot leave earlier catalogs or guides partly restored.
void malformed_advisory_state_restores_nothing() {
    constexpr size_t header_bytes = 96;
    Crossing crossing;
    crossing.run(idle_ticks);
    crossing.route();
    crossing.run(8);
    auto& match = crossing.match();
    const auto& state = match.rule_state();
    const auto* advisory = runtime::find_rule_state(state, "advisory-flow");
    CHECK(advisory != nullptr && advisory->restore != nullptr && !advisory->digest);
    std::array<std::vector<uint8_t>, runtime::rule_state_capacity> before;
    for (uint8_t index = 0; index < state.count; ++index)
        before[index] = rule_bytes(match, state.tables[index].name);
    const auto valid = rule_bytes(match, "advisory-flow");
    CHECK(valid.size() >= header_bytes);
    const auto rejected = [&](const std::vector<uint8_t>& bytes) {
        CHECK(!advisory->restore(advisory->context, bytes));
        for (uint8_t index = 0; index < state.count; ++index)
            CHECK(rule_bytes(match, state.tables[index].name) == before[index]);
    };
    auto malformed = valid;
    malformed.pop_back();
    rejected(malformed);
    malformed = valid;
    malformed.push_back(0);
    rejected(malformed);
    constexpr size_t reserved_header_byte = 95;
    malformed = valid;
    malformed[reserved_header_byte] ^= 1;
    rejected(malformed);
    constexpr size_t active_cursor_offset = 84;
    malformed = valid;
    std::fill_n(malformed.begin() + active_cursor_offset, sizeof(uint32_t), uint8_t{0});
    rejected(malformed);

    constexpr size_t catalog_metadata_bytes = 320;
    constexpr size_t catalog_reserved_offset = 8;
    constexpr size_t catalog_size_offset = 16;
    constexpr size_t identity_bytes = 16;
    constexpr size_t identity_owner_offset = 14;
    constexpr size_t guide_bytes = 176;
    constexpr size_t unit_count_offset = 16;
    constexpr size_t catalog_count_offset = 20;
    const auto unit_count = saved_integer(valid, unit_count_offset, sizeof(uint32_t));
    const auto catalog_count = saved_integer(valid, catalog_count_offset, sizeof(uint32_t));
    CHECK(unit_count == match.state().unit_slot_count && unit_count > 1);
    CHECK(catalog_count >= 2);
    size_t at = header_bytes;
    for (uint64_t index = 0; index < catalog_count; ++index) {
        CHECK(at <= valid.size() && catalog_metadata_bytes <= valid.size() - at);
        const auto size = saved_integer(valid, at + catalog_size_offset, sizeof(uint64_t));
        const auto blob = at + catalog_metadata_bytes;
        CHECK(size != 0 && size <= valid.size() - blob);
        if (index + 1 == catalog_count) {
            malformed = valid;
            malformed[blob] ^= 1;
            rejected(malformed);
            malformed = valid;
            malformed[at + catalog_reserved_offset] ^= 1;
            rejected(malformed);
        }
        at = blob + static_cast<size_t>(size);
    }
    CHECK(
        valid.size() - at ==
        unit_count * (identity_bytes + guide_bytes + movement::local_flow_head_bytes)
    );
    malformed = valid;
    malformed[at + (unit_count - 1) * identity_bytes + identity_owner_offset] = 255;
    rejected(malformed);
    constexpr size_t head_count_offset = 24;
    malformed = valid;
    malformed[valid.size() - movement::local_flow_head_bytes + head_count_offset] = 1;
    rejected(malformed);
}

/// Existing head and waypoint codecs carry actual advisory steering to a mirrored motor.
void advisory_head_reaches_the_existing_remote_movement_seam() {
    Crossing owner;
    owner.run(idle_ticks);
    owner.route();
    runtime::MatchTickAccess::AdvisoryLaneInspection lane{};
    bool entered{};
    for (uint32_t tick = 0; tick < crossing_limit; ++tick) {
        owner.run();
        lane = runtime::MatchTickAccess::advisory_lane(owner.match(), owner.mover);
        if (lane.active && lane.entered && lane.phase == crossing_phase) {
            entered = true;
            break;
        }
    }
    if (!entered)
        describe("transport entry", owner);
    CHECK(entered);
    std::vector<movement::LocalFlowHead> avoidance(owner.match().state().unit_slot_count);
    CHECK(movement::read_local_flow_heads(avoidance, rule_bytes(owner.match(), "local-flow")));
    CHECK(!avoidance[owner.mover].active);
    const auto* effective = owner.match().local_flow_head(owner.mover);
    CHECK(effective != nullptr && effective->active && effective->count == 3);
    CHECK(effective->tick == owner.match().simulation().tick);
    const ground::RoutePoint target{208, static_cast<int16_t>(lane.lane * 16 + 16)};
    CHECK(effective->points[1] == target && effective->points[2] == target);
    CHECK(owner.movement().navigation.points[1] != effective->points[1]);
    std::array<uint8_t, movement::local_flow_head_bytes> saved{};
    CHECK(movement::write_local_flow_heads({effective, 1}, saved));
    std::array<movement::LocalFlowHead, 1> decoded{};
    CHECK(movement::read_local_flow_heads(decoded, saved));
    CHECK(decoded[0].active && decoded[0].count == 3 && decoded[0].points == effective->points);
    std::array<uint8_t, movement::local_flow_head_bytes> encoded_again{};
    CHECK(movement::write_local_flow_heads(decoded, encoded_again));
    CHECK(encoded_again == saved);

    net::WaypointDelta delta{};
    delta.flag = (owner.movement().movement.flags & movement::collision_blocked) != 0;
    delta.count = decoded[0].count;
    for (uint8_t point = 0; point < delta.count; ++point)
        for (size_t axis = 0; axis < 2; ++axis)
            delta.points[point][axis] = decoded[0].points[point][axis];
    std::array<uint8_t, net::unit_state_writer_words * net::bit_stream_word_bytes> wire{};
    net::BitWriter writer{};
    net::bit_writer_init(&writer, wire.data(), net::unit_state_writer_words);
    net::write_waypoint_delta(&writer, delta);
    net::BitReader reader{};
    net::bit_reader_init(&reader, wire.data(), net::bit_writer_byte_length(&writer));
    net::WaypointDelta received{};
    net::read_waypoint_delta(&reader, &received);
    CHECK(!net::bit_reader_overrun(&reader));
    CHECK(received.flag == delta.flag && received.count == delta.count);

    Crossing remote;
    auto& state = remote.movement();
    auto& record = remote.match().spawn_slot(remote.mover).record;
    const auto& pose = owner.movement().geometry;
    remote.match().place_unit_at(
        remote.mover, pose.position[0], pose.position[1], pose.position[2], ground_occupancy
    );
    record.heading = pose.heading;
    record.pitch = pose.pitch;
    state.project_slot();
    state.movement = owner.movement().movement;
    state.previous_vector = owner.movement().previous_vector;
    state.mirrored_driver = true;
    remote.match().simulation().players[0].status = ground::mirrored_player_status;
    remote.match().simulation().tick = owner.match().simulation().tick + 1;
    binding::MatchBinding receiver{};
    receiver.match = &remote.match();
    const auto callbacks = binding::match_binding_sim(&receiver);
    auto& world = remote.match().state();
    callbacks.apply_ground_delta(callbacks.context, &world, &record, received);
    CHECK(state.mirrored_navigation.count == effective->count);
    for (uint8_t point = 0; point < received.count; ++point)
        CHECK(state.mirrored_navigation.points[point] == effective->points[point]);
    const auto distance = [&state, target] {
        const int64_t dx = int64_t(target[0]) * pixel - state.geometry.position[0];
        const int64_t dz = int64_t(target[1]) * pixel - state.geometry.position[2];
        return dx * dx + dz * dz;
    };
    const auto before = distance();
    callbacks.movement_tick(callbacks.context, &world, &record);
    callbacks.sight_update(callbacks.context, &world, &record);
    CHECK(remote.match().fault() == nullptr);
    CHECK(distance() < before);
    CHECK(!runtime::MatchTickAccess::advisory_lane(remote.match(), remote.mover).active);
    clear_footprint(remote);

    owner.movement().navigation.flags &= uint8_t(~ground::route_changed_flag);
    owner.run();
    const auto* continuing = owner.match().local_flow_head(owner.mover);
    CHECK(continuing != nullptr && continuing->active && continuing->count == 3);
    CHECK(continuing->points[1] == decoded[0].points[1]);
    CHECK(continuing->points[0] == decoded[0].points[0]);
    CHECK((owner.movement().navigation.flags & ground::route_changed_flag) == 0);
}

void configure_excluded_driver(Crossing& crossing, Driver driver) {
    auto& state = crossing.movement();
    if (driver == Driver::remote)
        crossing.match().simulation().players[0].status = ground::mirrored_player_status;
    if (driver == Driver::mirrored) {
        state.mirrored_driver = true;
        state.mirrored_navigation.count =
            std::min<uint32_t>(state.navigation.count, state.mirrored_navigation.points.size());
        std::copy_n(
            state.navigation.points.begin(),
            state.mirrored_navigation.count,
            state.mirrored_navigation.points.begin()
        );
    }
    if (driver == Driver::carried) {
        auto& carrier = crossing.fixture.spawn(0, 48, 64);
        crossing.match().place_unit_at(
            carrier.unit_index, 48 * pixel, land_height * pixel, 64 * pixel, ground_occupancy
        );
        auto& record = crossing.match().spawn_slot(crossing.mover).record;
        record.attach_parent = oa::oa_unit_ref_from_slot(carrier.unit_index);
        record.attach_piece = 0xff;
    }
}

/// Ineligible advice leaves the actual previous-version native trajectory untouched.
void ordinary_fallback(Setup setup, bool exhausted) {
    setup.version = 4;
    Crossing previous(setup);
    setup.version = 5;
    Crossing current(setup);
    runtime::MatchTickAccess::set_advisory_budget(current.match(), 0);
    previous.run(idle_ticks);
    current.run(idle_ticks);
    previous.route();
    current.route();
    configure_excluded_driver(previous, setup.driver);
    configure_excluded_driver(current, setup.driver);
    runtime::MatchTickAccess::set_advisory_budget(current.match(), exhausted ? 0 : ordinary_budget);
    CHECK(native_state(previous) == native_state(current));
    const auto initial_position = current.movement().geometry.position;
    constexpr uint32_t fallback_ticks = 24;
    for (uint32_t tick = 0; tick < fallback_ticks; ++tick) {
        previous.run();
        current.run();
        if (setup.driver == Driver::remote) {
            previous.match().step_mirrored_movement(previous.mover);
            current.match().step_mirrored_movement(current.mover);
        }
        CHECK(native_state(previous) == native_state(current));
        CHECK(!runtime::MatchTickAccess::advisory_lane(current.match(), current.mover).active);
        bounded_work(current);
    }
    if (setup.driver != Driver::carried)
        CHECK(current.movement().geometry.position != initial_position);
}

void unsupported_fog_and_budget_use_ordinary_navigation() {
    Setup unsupported;
    unsupported.supported_class = false;
    ordinary_fallback(unsupported, false);
    Setup unseen;
    unseen.known = false;
    ordinary_fallback(unseen, false);
    ordinary_fallback({}, true);
}

/// Discovery and steering for several units consume one finite per-tick allowance.
void all_units_share_the_finite_work_budget() {
    Crossing crossing;
    crossing.run(idle_ticks);
    crossing.route();
    std::array<uint16_t, 3> units{crossing.mover};
    size_t next = 1;
    for (const uint32_t z : {64u, 192u}) {
        auto& unit = crossing.fixture.spawn(0, 48, z);
        units[next++] = unit.unit_index;
        crossing.match().place_unit_at(
            unit.unit_index, 48 * pixel, land_height * pixel, z * pixel, ground_occupancy
        );
        unit.record.heading = east;
        CHECK(!crossing.match().order_refused(crossing.match().issue_ground_move(
            unit.unit_index, {destination[0] * pixel, 0, int32_t(z) * pixel}, false
        )));
    }
    runtime::MatchTickAccess::set_advisory_budget(crossing.match(), 128);
    uint32_t shared_limit{};
    bool used{}, advised{};
    constexpr uint32_t budget_ticks = 240;
    for (uint32_t tick = 0; tick < budget_ticks; ++tick) {
        crossing.run();
        bounded_work(crossing);
        const auto budget = runtime::MatchTickAccess::advisory_budget(crossing.match());
        if (tick == 0)
            shared_limit = budget.limit;
        CHECK(budget.limit == shared_limit);
        used |= budget.used != 0;
        for (const auto unit : units)
            advised |= runtime::MatchTickAccess::advisory_lane(crossing.match(), unit).active;
    }
    CHECK(shared_limit != 0 && used && advised);
}

constexpr size_t retained_group_size = 4;

std::array<uint16_t, retained_group_size> retained_group(Crossing& crossing) {
    crossing.run(idle_ticks);
    crossing.route();
    std::array<uint16_t, retained_group_size> units{crossing.mover};
    size_t at = 1;
    for (const uint32_t z : {64u, 176u, 208u}) {
        auto& unit = crossing.fixture.spawn(0, 48, z);
        units[at++] = unit.unit_index;
        crossing.match().place_unit_at(
            unit.unit_index, 48 * pixel, land_height * pixel, int32_t(z) * pixel, ground_occupancy
        );
        unit.record.heading = east;
        CHECK(!crossing.match().order_refused(crossing.match().issue_ground_move(
            unit.unit_index, {destination[0] * pixel, 0, destination[1] * pixel}, false
        )));
    }
    crossing.run();
    for (size_t index = 1; index < units.size(); ++index) {
        auto* state = crossing.match().ground_runtime(units[index]);
        CHECK(state != nullptr && state->navigation.goal != nullptr);
        auto& navigation = state->navigation;
        navigation.points = {};
        navigation.points[0] = {
            static_cast<int16_t>(state->geometry.position[0] >> 16),
            static_cast<int16_t>(state->geometry.position[2] >> 16)
        };
        navigation.points[1] = start;
        navigation.points[2] = {112, 128};
        navigation.points[3] = {160, 128};
        navigation.points[4] = destination;
        navigation.count = 5;
        navigation.flags = ground::route_present_flag;
        navigation.last_search_tick = crossing.match().simulation().tick;
    }
    constexpr uint32_t enrollment_ticks = 64;
    for (uint32_t tick = 0; tick < enrollment_ticks; ++tick) {
        bool all_active = true;
        for (const auto unit : units) {
            const auto guide = runtime::MatchTickAccess::advisory_lane(crossing.match(), unit);
            all_active &= guide.active && guide.phase != 0;
        }
        if (all_active)
            return units;
        crossing.run();
    }
    CHECK(false);
    return units;
}

/// Partial active work defers retained guides fairly and resumes the saved cursor exactly.
void retained_active_work_is_fair_and_survives_restore() {
    Crossing first;
    Crossing restored;
    const auto units = retained_group(first);
    CHECK(retained_group(restored) == units);
    runtime::MatchTickAccess::set_advisory_budget(first.match(), 128);
    runtime::MatchTickAccess::set_advisory_budget(restored.match(), 128);
    std::array<bool, retained_group_size> served{};
    bool deferred{}, stopped{}, saved_cursor{};
    constexpr uint32_t fairness_ticks = 8;
    for (uint32_t tick = 0; tick < fairness_ticks; ++tick) {
        std::array<runtime::MatchTickAccess::AdvisoryLaneInspection, retained_group_size> before{};
        std::array<const ground::Goal*, retained_group_size> goals{};
        for (size_t index = 0; index < units.size(); ++index) {
            before[index] = runtime::MatchTickAccess::advisory_lane(first.match(), units[index]);
            goals[index] = first.match().ground_runtime(units[index])->navigation.goal;
            CHECK(before[index].phase != 0 && goals[index] != nullptr);
        }
        first.run();
        restored.run();
        bounded_work(first);
        const auto budget = runtime::MatchTickAccess::advisory_budget(first.match());
        CHECK(budget.active_used <= 128 && budget.active_remaining + budget.active_used == 128);
        stopped |= budget.active_stopped;
        std::vector<movement::LocalFlowHead> published(first.match().state().unit_slot_count);
        const auto state_bytes = rule_bytes(first.match(), "advisory-flow");
        const auto head_bytes = published.size() * movement::local_flow_head_bytes;
        CHECK(
            movement::read_local_flow_heads(
                published, std::span<const uint8_t>(state_bytes).last(head_bytes)
            )
        );
        for (size_t index = 0; index < units.size(); ++index) {
            const auto unit = units[index];
            const auto guide = runtime::MatchTickAccess::advisory_lane(first.match(), unit);
            CHECK(first.match().ground_runtime(unit)->navigation.goal == goals[index]);
            CHECK(guide.phase != 0 && guide.generation == before[index].generation);
            CHECK(guide.corridor == before[index].corridor && guide.lane == before[index].lane);
            CHECK(guide.direction == before[index].direction);
            CHECK(guide.route_history == before[index].route_history);
            served[index] |= guide.active;
            if (!guide.active) {
                deferred = true;
                CHECK(guide.phase == before[index].phase);
                CHECK(!published[unit].active);
            }
        }
        if (!saved_cursor && budget.active_cursor != units.front()) {
            constexpr size_t active_cursor_offset = 84;
            auto altered = rule_bytes(restored.match(), "advisory-flow");
            const auto original = altered;
            const uint32_t alternate = units.front();
            const auto before_digest = match_digest(restored);
            for (size_t byte = 0; byte < sizeof(uint32_t); ++byte)
                altered[active_cursor_offset + byte] = uint8_t(alternate >> (byte * 8));
            restore_rule(restored.match(), "advisory-flow", altered);
            CHECK(
                runtime::MatchTickAccess::advisory_budget(restored.match()).active_cursor ==
                alternate
            );
            CHECK(match_digest(restored) == before_digest);
            restore_rule(restored.match(), "advisory-flow", original);
            CHECK(
                runtime::MatchTickAccess::advisory_budget(restored.match()).active_cursor ==
                budget.active_cursor
            );
            saved_cursor = true;
        }
        for (const auto unit : units)
            CHECK(native_state(first, unit) == native_state(restored, unit));
        CHECK(match_digest(first) == match_digest(restored));
        const auto& tables = first.match().rule_state();
        for (uint8_t index = 0; index < tables.count; ++index) {
            const auto* name = tables.tables[index].name;
            CHECK(rule_bytes(first.match(), name) == rule_bytes(restored.match(), name));
        }
    }
    CHECK(deferred && stopped && saved_cursor);
    CHECK(served.back());
    CHECK(std::all_of(served.begin(), served.end(), [](bool active) { return active; }));
}

void foreign_carried_and_air_drivers_receive_no_advice() {
    for (const auto driver :
         {Driver::remote, Driver::mirrored, Driver::carried, Driver::aircraft}) {
        Setup setup;
        setup.driver = driver;
        ordinary_fallback(setup, false);
    }
}

/// Mobile clearance and unrelated static changes keep the local geometry proof.
void only_overlapping_static_changes_invalidate_the_crossing() {
    Crossing crossing;
    crossing.run(idle_ticks);
    crossing.route();
    runtime::MatchTickAccess::AdvisoryLaneInspection admitted{};
    bool active{};
    for (uint32_t tick = 0; tick < crossing_limit; ++tick) {
        crossing.run();
        admitted = runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
        if (admitted.active && admitted.entered && crossing.movement().geometry.cell[0] >= 6) {
            active = true;
            break;
        }
    }
    if (!active)
        describe("static entry", crossing);
    CHECK(active);
    const auto catalog =
        runtime::MatchTickAccess::advisory_catalog(crossing.match(), crossing.mover);
    CHECK(catalog.corridors != 0);
    auto& mobile = crossing.fixture.spawn(0, 48, 48);
    crossing.match().place_unit_at(
        mobile.unit_index, 48 * pixel, land_height * pixel, 48 * pixel, ground_occupancy
    );
    CHECK(!crossing.match().order_refused(
        crossing.match().issue_ground_move(mobile.unit_index, {48 * pixel, 0, 80 * pixel}, false)
    ));
    const auto old_cell = std::array{mobile.record.cell_x, mobile.record.cell_z};
    bool vacated{};
    for (uint32_t tick = 0; tick < 20; ++tick) {
        crossing.run();
        vacated |= std::array{mobile.record.cell_x, mobile.record.cell_z} != old_cell;
        const auto lane = runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover);
        CHECK(lane.active && lane.corridor == admitted.corridor);
        CHECK(
            runtime::MatchTickAccess::advisory_catalog(crossing.match(), crossing.mover).revision ==
            catalog.revision
        );
    }
    CHECK(vacated);
    crossing.match().spatial().plots[2 * map_width + 2].blocking_feature = true;
    crossing.match().refresh_movement_maps({2, 2}, {1, 1});
    const auto unrelated =
        runtime::MatchTickAccess::advisory_catalog(crossing.match(), crossing.mover);
    CHECK(unrelated.revision == catalog.revision);
    CHECK(unrelated.corridors == catalog.corridors);
    crossing.run();
    CHECK(runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover).active);
    CHECK(
        runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover).corridor ==
        admitted.corridor
    );
    crossing.match().spatial().plots[8 * map_width + 10].blocking_feature = true;
    crossing.match().refresh_movement_maps({10, 8}, {1, 1});
    const auto invalidated =
        runtime::MatchTickAccess::advisory_catalog(crossing.match(), crossing.mover);
    CHECK(invalidated.revision != catalog.revision);
    CHECK(invalidated.corridors == 0 && invalidated.clearing);
    crossing.run();
    CHECK(!runtime::MatchTickAccess::advisory_lane(crossing.match(), crossing.mover).active);
    bounded_work(crossing);
}

} // namespace

int main() {
    uint32_t failures{};
    const auto run = [&failures](const char* name, void (*test)()) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    };
    run("late enrollment", first_route_after_startup_receives_advice);
    run("retained approach reserve", retained_approach_survives_exhausted_new_queries);
    run("same Goal reroute", same_goal_reroute_cannot_revive_an_obsolete_waypoint);
    run("pending restore", pending_discovery_survives_restore);
    run("entered restore", entered_crossing_survives_restore);
    run("replacement before rebind", replaced_order_withdraws_pending_saved_rebind);
    run("malformed restore", malformed_advisory_state_restores_nothing);
    run("remote movement", advisory_head_reaches_the_existing_remote_movement_seam);
    run("ordinary fallback", unsupported_fog_and_budget_use_ordinary_navigation);
    run("shared budget", all_units_share_the_finite_work_budget);
    run("retained active fairness", retained_active_work_is_fair_and_survives_restore);
    run("excluded drivers", foreign_carried_and_air_drivers_receive_no_advice);
    run("static invalidation", only_overlapping_static_changes_invalidate_the_crossing);
    if (failures == 0)
        std::cout << "Advisory lanes: live Match routes and saved continuation agree\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
