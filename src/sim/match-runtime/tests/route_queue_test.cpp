// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// An actual native route queue waits briefly at a right-angle mouth, then
// crosses on real movement after an ordinary unit leaves the passage.
#include "combat_fixture.hpp"
#include "../src/route_queue.hpp"

#include "oa/sim/ground_orders/steering_target.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/ground_orders/shared_routes.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;

constexpr int32_t pixel = 65536;
constexpr uint32_t battle_version = oa::data::enhanced_mode::battle_flow_version;
constexpr uint32_t distributed_version = oa::data::enhanced_mode::distributed_battle_flow_version;
constexpr uint32_t legacy_version = 4;
constexpr uint32_t following_version = oa::data::enhanced_mode::following_flow_version;
constexpr uint32_t map_height = 24;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint8_t ground_layer = 1;
constexpr uint16_t east = 49152;
constexpr int32_t maximum_speed = 117964;
constexpr int32_t acceleration = 6553;
constexpr uint16_t maximum_turn = 1120;

Options options() {
    Options result;
    result.sight_cells = 49;
    result.per_player_limit = 16;
    return result;
}

struct Scene {
    Fixture fixture{options()};
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    uint32_t width{};
    bool wide{}, flat{}, double_turn{};
    uint16_t gate{};
    std::vector<uint16_t> queue;

    explicit Scene(uint32_t version, bool wide = false, bool flat = false, bool double_turn = false)
        : wide(wide), flat(flat), double_turn(double_turn) {
        fixture.match.reset();
        width = wide ? 48 : 24;
        fixture.map.attribute_width = width;
        fixture.map.attribute_height = map_height;
        fixture.map.sea_level = sea_level;
        fixture.map.attributes.resize(size_t(width) * map_height);
        fixture.terrain_values.resize(size_t(width) * map_height);
        fixture.collision_plots.resize(size_t(width) * map_height);
        fixture.altitude_cells.resize(size_t(width / 2) * (map_height / 2));
        for (uint32_t z = 0; z < map_height; ++z)
            for (uint32_t x = 0; x < width; ++x) {
                const bool high = land(x, z) || (x && land(x - 1, z)) || (z && land(x, z - 1)) ||
                                  (x && z && land(x - 1, z - 1));
                fixture.map.attributes[size_t(z) * width + x].height = high ? land_height : 0;
            }
        for (uint32_t z = 0; z < map_height; ++z)
            for (uint32_t x = 0; x < width; ++x) {
                auto& plot = fixture.collision_plots[size_t(z) * width + x];
                plot.low_height = plot.high_height =
                    fixture.map.attributes[size_t(z) * width + x].height;
                for (uint32_t dz = 0; dz < 2 && z + dz < map_height; ++dz)
                    for (uint32_t dx = 0; dx < 2 && x + dx < width; ++dx) {
                        const auto height =
                            fixture.map.attributes[size_t(z + dz) * width + x + dx].height;
                        plot.low_height = std::min(plot.low_height, height);
                        plot.high_height = std::max(plot.high_height, height);
                    }
            }
        fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
        fixture.loaded[1].type = fixture.types[1];
        fixture.fields[1].movement_class = 1;
        fixture.fields[1].yard_mask = yard;
        fixture.metadata.footprint_x = fixture.metadata.footprint_z = 2;
        fixture.metadata.min_water_depth = -10000;
        fixture.metadata.max_water_depth = 0;
        fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
        fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
        fixture.def.max_velocity_fixed = maximum_speed;
        fixture.def.acceleration_fixed = fixture.def.brake_rate_fixed = acceleration;
        fixture.def.turn_rate = maximum_turn;
        runtime::OfflineInputs inputs{
            fixture.map,
            fixture.loaded,
            fixture.types,
            fixture.fields,
            fixture.weapons,
            fixture.terrain_values,
            fixture.masks,
            static_cast<int32_t>(width / 2),
            map_height / 2,
            16,
            2,
            0,
            sea_level,
            1,
            &fixture.scenario,
            {},
            fixture.collision_plots,
            oa::sim::visibility_state::AltitudeSightData{
                static_cast<int32_t>(width / 2),
                map_height / 2,
                fixture.altitude_cells,
                fixture.altitude_patterns
            }
        };
        inputs.rules.setup.enhanced_mode.enabled = version != 0;
        inputs.rules.setup.enhanced_mode.version = version;
        inputs.unit_player_mask = 3;
        fixture.match = std::make_unique<runtime::Match>(inputs, fixture.services);
        match().configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t owner = 0; owner < 2; ++owner) {
            match().simulation().players[owner].present = true;
            match().simulation().players[owner].status = 1;
            std::array<uint8_t, OA_PLAYER_COUNT> allies{};
            allies[owner] = 1;
            match().configure_player_alliances(owner, allies);
            if (owner == 0)
                match().configure_outcomes(owner, allies, false);
        }
        gate = spawn(gate_x() * pixel);
        const uint32_t count = wide ? 8 : 2;
        for (uint32_t index = 0; index < count; ++index)
            queue.push_back(spawn((gate_x() - 16 - int32_t(index) * 32) * pixel - 1));
        CHECK(match().fault() == nullptr);
    }

    bool land(uint32_t x, uint32_t z) const {
        if (flat)
            return true;
        const auto mouth = wide ? 31u : 7u;
        const auto turn = wide ? 34u : 10u;
        const auto end = wide ? 44u : 19u;
        return (x >= 2 && x < mouth && z >= 6 && z <= 11) ||
               (x >= mouth && x <= end && z >= 8 && z <= 9) ||
               (x >= turn && x <= turn + 4 && z >= (double_turn ? 4u : 2u) && z <= 9) ||
               (double_turn && x >= 10 && x <= 22 && z >= 4 && z <= 5) ||
               (wide && x >= 30 && x <= 43 && z >= 2 && z <= 3);
    }

    int32_t gate_x() const { return wide ? 520 : 136; }

    int32_t turn_x() const { return wide ? 592 : 208; }

    int32_t parking_x() const { return wide ? 672 : double_turn ? 288 : 224; }

    runtime::Match& match() { return *fixture.match; }

    ground::GroundRuntime& ground(uint16_t id) {
        auto* result = match().ground_runtime(id);
        CHECK(result != nullptr);
        return *result;
    }

    uint16_t spawn(int32_t x) {
        auto& slot = fixture.spawn(0, uint32_t(x / pixel), 144);
        match().place_unit_at(slot.unit_index, x, land_height * pixel, 144 * pixel, ground_layer);
        slot.record.heading = east;
        return slot.unit_index;
    }

    void begin() {
        // These are ordinary idle ticks, allowing a newly issued route to
        // reach the native retry cadence before its first movement wave.
        run(60);
        for (size_t index = 0; index < queue.size(); ++index) {
            const auto id = queue[index];
            const ground::RoutePoint destination =
                double_turn ? ground::RoutePoint{static_cast<int16_t>(index == 0 ? 336 : 304), 80}
                            : ground::RoutePoint{static_cast<int16_t>(turn_x()), 48};
            CHECK(!match().order_refused(
                match().issue_ground_move(
                    id, {destination[0] * pixel, 0, destination[1] * pixel}, false
                )
            ));
            if (double_turn)
                audit_destination(id, destination, {static_cast<int16_t>(index == 0 ? 20 : 18), 4});
        }
        if (wide) {
            // Declare every egress order before the wave. Normal mission
            // handling advances the prequeued orders.
            constexpr std::array<ground::RoutePoint, 8> parking{
                {{688, 48},
                 {656, 48},
                 {624, 48},
                 {496, 48},
                 {528, 48},
                 {560, 48},
                 {560, 80},
                 {592, 80}}
            };
            constexpr std::array<std::array<int16_t, 2>, 8> parking_cells{
                {{42, 2}, {40, 2}, {38, 2}, {30, 2}, {32, 2}, {34, 2}, {34, 4}, {36, 4}}
            };
            for (size_t index = 0; index < queue.size(); ++index) {
                audit_destination(queue[index], parking[index], parking_cells[index]);
                CHECK(!match().order_refused(
                    match().issue_ground_move(
                        queue[index],
                        {parking[index][0] * pixel, 0, parking[index][1] * pixel},
                        true
                    )
                ));
            }
        }
    }

    void audit_destination(uint16_t id, ground::RoutePoint point, std::array<int16_t, 2> expected) {
        const auto& body = ground(id).geometry;
        auto* primary = match().spawn_slot(id).unit->primary;
        CHECK(primary != nullptr);
        const auto goal =
            ground::make_goal(*primary, body, {point[0] * pixel, 0, point[1] * pixel}, 4);
        CHECK(goal.cell == expected);
        const oa::sim::spatial_state::CellClassifier classifier{
            body.footprint[0],
            body.footprint[1],
            static_cast<int16_t>(fixture.metadata.max_water_depth),
            static_cast<int16_t>(fixture.metadata.min_water_depth),
            fixture.metadata.max_slope,
            fixture.metadata.bad_slope,
            fixture.metadata.max_water_slope,
            fixture.metadata.bad_water_slope,
            0
        };
        CHECK(
            ground::static_flow_passable(classifier, goal.cell[0], goal.cell[1], match().spatial())
        );
    }

    void release_gate() {
        CHECK(!match().order_refused(
            match().issue_ground_move(gate, {parking_x() * pixel, 0, 144 * pixel}, false)
        ));
        if (double_turn)
            audit_destination(gate, {288, 144}, {17, 8});
    }

    void run(uint32_t ticks = 1) {
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            fixture.run(1);
            check_claims();
        }
    }

    void check_claims() {
        std::vector<uint16_t> claims(size_t(width) * map_height);
        for (const auto id : all_units()) {
            const auto& record = match().spawn_slot(id).record;
            if (record.type_index == 0)
                continue;
            const auto& state = ground(id);
            CHECK(state.geometry.cell == (std::array<int16_t, 2>{record.cell_x, record.cell_z}));
            for (int16_t z = 0; z < 2; ++z)
                for (int16_t x = 0; x < 2; ++x) {
                    const auto px = state.geometry.cell[0] + x;
                    const auto pz = state.geometry.cell[1] + z;
                    CHECK(px >= 0 && pz >= 0 && uint32_t(px) < width && uint32_t(pz) < map_height);
                    const auto at = size_t(pz) * width + size_t(px);
                    const auto& plot = match().spatial().plots[at];
                    CHECK(!plot.blocking_feature);
                    CHECK(plot.low_height == land_height && plot.high_height == land_height);
                    CHECK(claims[at] == 0);
                    claims[at] = id;
                    CHECK(plot.ground == id);
                }
        }
    }

    std::vector<uint16_t> all_units() const {
        auto result = queue;
        result.push_back(gate);
        return result;
    }
};

bool same_goal(const ground::Goal& left, const ground::Goal& right) {
    return left.shape == right.shape && left.cell == right.cell &&
           left.tolerance == right.tolerance && left.radius_squared == right.radius_squared &&
           left.inner_range == right.inner_range &&
           left.inner_radius_squared == right.inner_radius_squared && left.left == right.left &&
           left.right == right.right && left.top == right.top && left.bottom == right.bottom;
}

bool genuine_contact(Scene& scene, bool distinct_goals = false) {
    auto& leader = scene.ground(scene.queue[0]);
    auto& follower = scene.ground(scene.queue[1]);
    const auto& a = leader.navigation;
    const auto& b = follower.navigation;
    if (!a.goal || !b.goal || a.count < 3 || b.count < 3 ||
        same_goal(*a.goal, *b.goal) == distinct_goals || a.points[1] != b.points[1] ||
        a.points[2] != b.points[2])
        return false;
    if (a.goal->order != scene.match().spawn_slot(scene.queue[0]).unit->primary ||
        b.goal->order != scene.match().spawn_slot(scene.queue[1]).unit->primary)
        return false;
    const auto* flow = scene.match().local_flow_head(scene.queue[1]);
    if (!flow || !flow->mobile_blocked || flow->blocked_ticks >= movement::local_flow_retry_ticks ||
        (leader.movement.flags & movement::collision_blocked) == 0)
        return false;
    const auto leader_gap =
        std::abs(int64_t(a.points[1][0]) * pixel - leader.geometry.position[0]) +
        std::abs(int64_t(a.points[1][1]) * pixel - leader.geometry.position[2]);
    const auto follower_gap =
        std::abs(int64_t(b.points[1][0]) * pixel - follower.geometry.position[0]) +
        std::abs(int64_t(b.points[1][1]) * pixel - follower.geometry.position[2]);
    return leader_gap < follower_gap;
}

struct WaveResult {
    bool contact{}, bypass{}, crossed{}, arrived{};
    uint32_t contact_tick{}, crossing_tick{};
};

void observe_advanced_prefix(Scene& scene, bool& observed) {
    if (observed || scene.wide || scene.double_turn)
        return;
    const auto& leader = scene.ground(scene.queue[0]);
    const auto& follower = scene.ground(scene.queue[1]);
    const auto& a = leader.navigation;
    const auto& b = follower.navigation;
    if (a.count < 2 || b.count < 3 || a.points[0] != b.points[1] || a.points[1] != b.points[2])
        return;
    observed = true;
    const auto* front = scene.match().local_flow_head(scene.queue[0]);
    const auto* back = scene.match().local_flow_head(scene.queue[1]);
    std::cout << "Observed native prefix candidate: version="
              << scene.match().rules().setup.enhanced_mode.version
              << ", tick=" << scene.match().simulation().tick << ", leader=" << scene.queue[0]
              << ", follower=" << scene.queue[1] << ", leader raw=" << leader.geometry.position[0]
              << ',' << leader.geometry.position[2]
              << ", follower raw=" << follower.geometry.position[0] << ','
              << follower.geometry.position[2] << ", headings=" << leader.geometry.heading << '/'
              << follower.geometry.heading << ", leader P0=" << a.points[0][0] << ','
              << a.points[0][1] << ", leader P1=" << a.points[1][0] << ',' << a.points[1][1]
              << ", follower P1=" << b.points[1][0] << ',' << b.points[1][1]
              << ", follower P2=" << b.points[2][0] << ',' << b.points[2][1]
              << ", goal owns primary="
              << (a.goal && a.goal->order == scene.match().spawn_slot(scene.queue[0]).unit->primary)
              << '/'
              << (b.goal && b.goal->order == scene.match().spawn_slot(scene.queue[1]).unit->primary)
              << ", search stamps=" << a.last_search_tick << '/' << b.last_search_tick
              << ", local head active=" << (front && front->active) << '/' << (back && back->active)
              << ", head targets=" << (front ? front->points[1][0] : 0) << ','
              << (front ? front->points[1][1] : 0) << '/' << (back ? back->points[1][0] : 0) << ','
              << (back ? back->points[1][1] : 0) << '\n';
}

WaveResult native_wave(uint32_t version, bool wide = false, bool distinct_goals = false) {
    Scene scene(version, wide, false, distinct_goals);
    scene.begin();
    WaveResult result;
    bool advanced_observed{};
    for (uint32_t tick = 0; tick < 600 && !genuine_contact(scene, distinct_goals); ++tick) {
        scene.run();
        observe_advanced_prefix(scene, advanced_observed);
    }
    CHECK(genuine_contact(scene, distinct_goals));
    result.contact = true;
    result.contact_tick = scene.match().simulation().tick;
    const auto& navigation = scene.ground(scene.queue[1]).navigation;
    std::cout << "Native queue contact: version=" << version << ", size=" << scene.queue.size()
              << ", tick=" << result.contact_tick << ", leader=" << scene.queue[0]
              << ", follower=" << scene.queue[1] << ", Goal=" << navigation.goal->cell[0] << ','
              << navigation.goal->cell[1] << ", P1=" << navigation.points[1][0] << ','
              << navigation.points[1][1] << ", P2=" << navigation.points[2][0] << ','
              << navigation.points[2][1] << '\n';
    std::vector<ground::Goal> goals;
    if (distinct_goals) {
        for (const auto id : scene.queue)
            goals.push_back(*scene.ground(id).navigation.goal);
        CHECK(goals[0].cell == (std::array<int16_t, 2>{20, 4}));
        CHECK(goals[1].cell == (std::array<int16_t, 2>{18, 4}));
        if (version == distributed_version || version == following_version) {
            CHECK(navigation.points[1] == (ground::RoutePoint{176, 144}));
            CHECK(navigation.points[2] == (ground::RoutePoint{208, 112}));
        } else {
            CHECK(navigation.points[1] == (ground::RoutePoint{208, 144}));
            CHECK(navigation.points[2] == (ground::RoutePoint{208, 80}));
        }
        std::cout << "Distinct native goals: leader=" << goals[0].cell[0] << ',' << goals[0].cell[1]
                  << ", follower=" << goals[1].cell[0] << ',' << goals[1].cell[1] << '\n';
    }
    for (uint32_t tick = 0; tick < 10; ++tick) {
        const auto* head = scene.match().local_flow_head(scene.queue[1]);
        CHECK(head != nullptr);
        if (head->blocked_ticks < movement::local_flow_retry_ticks)
            result.bypass |= head->active;
        scene.run();
        observe_advanced_prefix(scene, advanced_observed);
    }
    scene.release_gate();
    std::vector<bool> crossed(scene.queue.size());
    std::vector<bool> arrived(scene.queue.size());
    for (uint32_t tick = 0; tick < 900; ++tick) {
        scene.run();
        observe_advanced_prefix(scene, advanced_observed);
        for (size_t index = 0; index < scene.queue.size(); ++index) {
            const auto& cell = scene.ground(scene.queue[index]).geometry.cell;
            crossed[index] = crossed[index] || (cell[0] >= (wide ? 34 : 10) && cell[1] < 6);
            if (distinct_goals)
                arrived[index] =
                    arrived[index] || ground::goal_contains_unit(
                                          goals[index], scene.ground(scene.queue[index]).geometry
                                      );
        }
        result.crossed =
            std::all_of(crossed.begin(), crossed.end(), [](bool value) { return value; });
        result.arrived =
            std::all_of(arrived.begin(), arrived.end(), [](bool value) { return value; });
        if (result.crossed && (!distinct_goals || result.arrived)) {
            result.crossing_tick = scene.match().simulation().tick;
            break;
        }
    }
    if (wide && !result.crossed) {
        const auto show = [&](uint16_t id, int crossed_once) {
            const auto& state = scene.ground(id);
            const auto& slot = scene.match().spawn_slot(id);
            const auto* head = scene.match().local_flow_head(id);
            const auto* goal = state.navigation.goal;
            std::cerr << "Native queue final: uid=" << id << ", ever crossed=" << crossed_once
                      << ", tick=" << scene.match().simulation().tick
                      << ", x raw=" << state.geometry.position[0]
                      << ", z raw=" << state.geometry.position[2]
                      << ", cell=" << state.geometry.cell[0] << ',' << state.geometry.cell[1]
                      << ", order=" << (slot.unit->primary ? unsigned(slot.unit->primary->kind) : 0)
                      << ", goal=" << (goal ? goal->cell[0] : -1) << ','
                      << (goal ? goal->cell[1] : -1)
                      << ", goal owns head=" << (goal && goal->order == slot.unit->primary)
                      << ", nav count=" << state.navigation.count
                      << ", nav flags=" << unsigned(state.navigation.flags)
                      << ", P1=" << state.navigation.points[1][0] << ','
                      << state.navigation.points[1][1] << ", P2=" << state.navigation.points[2][0]
                      << ',' << state.navigation.points[2][1]
                      << ", blocked=" << ((state.movement.flags & movement::collision_blocked) != 0)
                      << ", speed=" << state.movement.speed << ", motion age="
                      << uint32_t(scene.match().simulation().tick - state.movement.last_motion_tick)
                      << ", flow active=" << (head && head->active)
                      << ", flow mobile=" << (head && head->mobile_blocked)
                      << ", flow blocked=" << (head ? unsigned(head->blocked_ticks) : 0) << '\n';
        };
        for (size_t index = 0; index < scene.queue.size(); ++index)
            show(scene.queue[index], crossed[index]);
        show(scene.gate, -1);
    }
    if (version == battle_version || version == distributed_version) {
        CHECK(!result.bypass);
        CHECK(result.crossed);
        if (distinct_goals)
            CHECK(result.arrived);
    }
    std::cout << "Native queue release: version=" << version << ", bypass=" << result.bypass
              << ", crossed=" << result.crossed
              << ", arrived=" << (distinct_goals ? (result.arrived ? "1" : "0") : "NOT_CHECKED")
              << ", tick=" << result.crossing_tick << '\n';
    return result;
}

void a_real_native_wave_waits_then_crosses_the_mouth(uint32_t version) {
    const auto legacy = native_wave(legacy_version);
    const auto enhanced = native_wave(version);
    CHECK(legacy.contact && legacy.bypass);
    CHECK(enhanced.contact && !enhanced.bypass && enhanced.crossed);
}

void eight_real_units_cross_after_the_ordinary_gate_leaves(uint32_t version) {
    const auto result = native_wave(version, true);
    CHECK(result.contact && !result.bypass && result.crossed);
}

void distinct_native_goals_share_the_two_corner_queue(uint32_t version) {
    const auto legacy = native_wave(legacy_version, false, true);
    const auto enhanced = native_wave(version, false, true);
    CHECK(legacy.contact && legacy.bypass);
    CHECK(enhanced.contact && !enhanced.bypass && enhanced.crossed && enhanced.arrived);
}

void a_permanent_native_gate_releases_mature_recovery(uint32_t version) {
    Scene scene(version);
    scene.begin();
    for (uint32_t tick = 0; tick < 600 && !genuine_contact(scene); ++tick)
        scene.run();
    CHECK(genuine_contact(scene));
    bool mature_recovery{};
    const uint32_t contact_tick = scene.match().simulation().tick;
    for (uint32_t tick = 0; tick < 100; ++tick) {
        const auto* before = scene.match().local_flow_head(scene.queue[1]);
        CHECK(before != nullptr);
        const auto blocked = before->blocked_ticks;
        scene.run();
        const auto* after = scene.match().local_flow_head(scene.queue[1]);
        CHECK(after != nullptr);
        if (after->active) {
            CHECK(blocked >= movement::local_flow_retry_ticks);
            CHECK(
                scene.match().simulation().tick - contact_tick >=
                movement::local_flow_retry_ticks - 1
            );
            mature_recovery = true;
            break;
        }
    }
    CHECK(mature_recovery);
    CHECK(scene.match().spawn_slot(scene.gate).record.position.x == scene.gate_x() * pixel);
    std::cout << "Native permanent queue: contact=" << contact_tick
              << ", recovery=" << scene.match().simulation().tick << '\n';
}

enum class Pipeline {
    matched,
    different_goal,
    opposing_leg,
    different_next_leg,
    idle_leader,
    cancelled_leader,
    retargeted_leader,
    all_blockers_matched,
    incompatible_blocker,
    dying_leader,
    nonpositive_leader,
    dying_follower,
    nonpositive_follower
};

struct PipelineResult {
    std::array<movement::Fixed, 3> position{}, velocity{};
    std::array<int16_t, 2> cell{};
    std::array<ground::RoutePoint, 3> points{};
    movement::Fixed speed{};
    uint16_t heading{};
    int16_t turn{};
    uint8_t flags{};
    bool active{}, mobile{}, leader_live{}, follower_live{}, backward_leader{};
    bool operator==(const PipelineResult&) const = default;
};

void prepare_route(Scene& scene, uint16_t id, int32_t x, int32_t z, uint32_t age) {
    auto& state = scene.ground(id);
    auto& record = scene.match().spawn_slot(id).record;
    scene.match().place_unit_at(id, x, land_height * pixel, z * pixel, ground_layer);
    record.heading = east;
    state.project_slot();
    state.movement.speed = maximum_speed;
    state.movement.velocity = movement::aim_velocity(east, 0, maximum_speed);
    state.movement.turn = 0;
    state.movement.flags = ground_layer | movement::collision_blocked;
    state.movement.last_motion_tick = 57 - age;
    state.navigation.points = {};
    state.navigation.points[0] = {static_cast<int16_t>(x / pixel), static_cast<int16_t>(z)};
    state.navigation.points[1] = {208, 144};
    state.navigation.points[2] = {208, 48};
    state.navigation.count = 3;
    state.navigation.flags = ground::route_present_flag;
    state.navigation.last_search_tick = 0;
}

PipelineResult pipeline(uint32_t version, Pipeline control, uint8_t blocked = 2) {
    Scene scene(version, false, true);
    auto& match = scene.match();
    const auto leader = scene.queue[0], follower = scene.queue[1];
    const bool overlap =
        control == Pipeline::all_blockers_matched || control == Pipeline::incompatible_blocker;
    CHECK(
        !match.order_refused(match.issue_ground_move(follower, {208 * pixel, 0, 48 * pixel}, false))
    );
    if (control != Pipeline::idle_leader)
        CHECK(!match.order_refused(
            match.issue_ground_move(leader, {208 * pixel, 0, 48 * pixel}, false)
        ));
    if (overlap)
        CHECK(!match.order_refused(
            match.issue_ground_move(scene.gate, {208 * pixel, 0, 48 * pixel}, false)
        ));
    scene.run();
    match.simulation().tick = 56;
    prepare_route(scene, leader, overlap ? 112 * pixel : 120 * pixel - 1, overlap ? 128 : 144, 10);
    prepare_route(scene, follower, 88 * pixel - 1, 144, blocked >= 30 ? 30 : 2);
    if (overlap)
        prepare_route(scene, scene.gate, 112 * pixel, 160, 10);
    else {
        auto& gate = scene.ground(scene.gate);
        gate.movement.speed = 0;
        gate.movement.velocity = {};
    }
    auto& front = scene.ground(leader);
    switch (control) {
    case Pipeline::different_goal:
        CHECK(front.navigation.goal != nullptr);
        ++front.navigation.goal->cell[0];
        break;
    case Pipeline::opposing_leg:
        front.navigation.points[1] = {48, 144};
        break;
    case Pipeline::different_next_leg:
        front.navigation.points[2] = {224, 48};
        break;
    case Pipeline::idle_leader:
        front.navigation = {};
        front.movement.speed = 0;
        front.movement.velocity = {};
        break;
    case Pipeline::cancelled_leader:
        match.stop_orders(leader);
        front.movement.speed = 0;
        front.movement.velocity = {};
        break;
    case Pipeline::retargeted_leader:
        CHECK(!match.order_refused(
            match.issue_ground_move(leader, {32 * pixel, 0, 144 * pixel}, false)
        ));
        break;
    case Pipeline::incompatible_blocker:
        scene.ground(scene.gate).navigation.points[2] = {224, 48};
        break;
    case Pipeline::dying_leader:
        match.spawn_slot(leader).record.flags |= OA_UNIT_FLAG_DEATH_PENDING;
        break;
    case Pipeline::nonpositive_leader:
        match.spawn_slot(leader).record.health = 0;
        break;
    case Pipeline::dying_follower:
        match.spawn_slot(follower).record.flags |= OA_UNIT_FLAG_DEATH_PENDING;
        break;
    case Pipeline::nonpositive_follower:
        match.spawn_slot(follower).record.health = 0;
        break;
    default:
        break;
    }
    if (version >= 2) {
        std::vector<movement::LocalFlowHead> heads(match.state().unit_slot_count);
        heads[follower].mobile_blocked = true;
        heads[follower].blocked_ticks = blocked;
        std::vector<uint8_t> bytes(heads.size() * movement::local_flow_head_bytes);
        CHECK(movement::write_local_flow_heads(heads, bytes));
        const auto* table = runtime::find_rule_state(match.rule_state(), "local-flow");
        CHECK(table && table->restore && table->restore(table->context, bytes));
    }
    scene.run();
    PipelineResult result;
    result.leader_live = oa::unit_is_live_target(match.spawn_slot(leader).record.flags) &&
                         match.spawn_slot(leader).record.health > 0;
    result.follower_live = oa::unit_is_live_target(match.spawn_slot(follower).record.flags) &&
                           match.spawn_slot(follower).record.health > 0;
    if (result.leader_live) {
        const auto* actual = match.ground_runtime(leader);
        const auto* head = match.local_flow_head(leader);
        if (actual && actual->navigation.goal && actual->navigation.count >= 2 &&
            actual->navigation.goal->order == match.spawn_slot(leader).unit->primary && head &&
            head->active && head->count == 3 && head->tick == 57) {
            const int64_t dx = int64_t(head->points[1][0]) * pixel - actual->geometry.position[0];
            const int64_t dz = int64_t(head->points[1][1]) * pixel - actual->geometry.position[2];
            const int64_t fx =
                int64_t(actual->navigation.points[1][0]) * pixel - actual->geometry.position[0];
            const int64_t fz =
                int64_t(actual->navigation.points[1][1]) * pixel - actual->geometry.position[2];
            result.backward_leader = dx * fx + dz * fz <= 0;
        }
    }
    const auto* survivor = match.ground_runtime(follower);
    if (!survivor) {
        CHECK(!result.follower_live);
        CHECK(control == Pipeline::dying_follower);
        return result;
    }
    const auto& state = *survivor;
    result.position = state.geometry.position;
    result.velocity = state.movement.velocity;
    result.cell = state.geometry.cell;
    result.speed = state.movement.speed;
    result.heading = state.geometry.heading;
    result.turn = state.movement.turn;
    result.flags = state.movement.flags;
    if (const auto* head = match.local_flow_head(follower)) {
        result.active = head->active;
        result.mobile = head->mobile_blocked;
        result.points = head->points;
    }
    CHECK(match.simulation().tick == 57);
    CHECK(state.geometry.heading == static_cast<uint16_t>(east + state.movement.turn));
    CHECK(std::abs(int(state.movement.turn)) <= maximum_turn);
    CHECK(
        state.movement.velocity ==
        movement::aim_velocity(state.geometry.heading, 0, state.movement.speed)
    );
    return result;
}

void actual_pipeline_rejects_incompatible_or_unowned_routes(uint32_t version) {
    // The idle gate makes the leader apply a non-forward local leg. Eight
    // no longer treats that applied leg as a forward queue certificate.
    const bool follows_applied = version == following_version;
    const auto matched = pipeline(version, Pipeline::matched);
    CHECK(matched.backward_leader);
    CHECK(matched.mobile && matched.active == follows_applied);
    const auto different_goal = pipeline(version, Pipeline::different_goal);
    CHECK(different_goal.backward_leader);
    CHECK(different_goal.mobile && different_goal.active == follows_applied);
    CHECK(pipeline(legacy_version, Pipeline::matched).active);
    for (const auto control :
         {Pipeline::opposing_leg,
          Pipeline::different_next_leg,
          Pipeline::idle_leader,
          Pipeline::cancelled_leader,
          Pipeline::retargeted_leader,
          Pipeline::incompatible_blocker}) {
        const auto result = pipeline(version, control);
        CHECK(result.mobile && result.active);
    }
    const auto all_match = pipeline(version, Pipeline::all_blockers_matched);
    CHECK(all_match.mobile && !all_match.active);
    const auto mature = pipeline(version, Pipeline::matched, 31);
    CHECK(mature.mobile && mature.active);
}

void dying_or_nonpositive_units_do_not_hold_the_queue(uint32_t version) {
    for (const auto control :
         {Pipeline::dying_leader,
          Pipeline::nonpositive_leader,
          Pipeline::dying_follower,
          Pipeline::nonpositive_follower}) {
        const auto result = pipeline(version, control);
        if (control == Pipeline::dying_leader || control == Pipeline::nonpositive_leader) {
            CHECK(!result.leader_live);
            CHECK(result.active);
        } else
            CHECK(!result.follower_live);
    }
    // DEATH_PENDING actors follow the real teardown before later units move.
    // The nonpositive-health cases retain their runtime and exercise the gate.
}

void historical_profiles_do_not_read_route_coherence() {
    for (uint32_t version = 0; version < battle_version; ++version)
        CHECK(pipeline(version, Pipeline::matched) == pipeline(version, Pipeline::different_goal));
}

uint64_t applied_goal_key(const ground::Goal& goal) {
    uint64_t key = 14695981039346656037ull;
    const auto add = [&key](uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            key ^= static_cast<uint8_t>(value >> (byte * 8));
            key *= 1099511628211ull;
        }
    };
    add(static_cast<uint32_t>(goal.shape));
    add(static_cast<uint16_t>(goal.cell[0]));
    add(static_cast<uint16_t>(goal.cell[1]));
    add(static_cast<uint32_t>(goal.tolerance));
    add(static_cast<uint32_t>(goal.radius_squared));
    add(static_cast<uint32_t>(goal.inner_range));
    add(static_cast<uint32_t>(goal.inner_radius_squared));
    add(static_cast<uint32_t>(goal.left));
    add(static_cast<uint32_t>(goal.right));
    add(static_cast<uint32_t>(goal.top));
    add(static_cast<uint32_t>(goal.bottom));
    return key;
}

void applied_head_certificate_is_explicit_and_current() {
    movement::Unit follower{}, leader{};
    follower.position = {88 * pixel, land_height * pixel, 144 * pixel};
    leader.position = {120 * pixel, land_height * pixel, 144 * pixel};
    follower.footprint = leader.footprint = {2, 2};
    follower.id = 3;
    leader.id = 2;
    ground::Goal goal{};
    ground::Navigation route{};
    route.goal = &goal;
    route.flags = ground::route_present_flag;
    route.count = 3;
    route.points[1] = {208, 144};
    route.points[2] = {208, 48};
    auto leader_route = route;
    movement::LocalFlowHead head{};
    head.points = {{{120, 144}, {104, 144}, {208, 48}}};
    head.active = true;
    head.count = 3;
    head.tick = 57;
    head.goal_key = 9;
    const auto check = [&](const movement::LocalFlowHead* candidate,
                           bool enabled = true,
                           uint64_t key = 9,
                           uint32_t tick = 57) {
        return runtime::same_forward_route_with_applied_head(
            follower, route, leader, leader_route, candidate, key, tick, enabled
        );
    };
    CHECK(runtime::same_forward_route(follower, route, leader, leader_route));
    CHECK(
        runtime::same_forward_route_with_applied_head(
            follower, route, leader, leader_route, &head, 9, 57
        )
    );
    CHECK(!check(&head));
    head.points[1] = {120, 160};
    CHECK(!check(&head));
    head.points[1] = {136, 144};
    CHECK(check(&head));
    head.points[1] = {104, 144};
    CHECK(check(nullptr));
    CHECK(check(&head, false));
    CHECK(check(&head, true, 0));
    CHECK(check(&head, true, 8));
    CHECK(check(&head, true, 9, 56));
    CHECK(check(&head, true, 9, 58));
    for (const uint8_t count : {uint8_t{0}, uint8_t{2}, uint8_t{4}}) {
        head.count = count;
        CHECK(check(&head));
    }
    head.count = 3;
    head.active = false;
    CHECK(check(&head));
    head.active = true;
    head.points[1] = {123, 144};
    CHECK(movement::local_flow_head_reached(leader, head));
    CHECK(check(&head));
    head.points[1] = {104, 144};
    head.blocked_ticks = movement::local_flow_retry_ticks;
    head.tick = 88;
    CHECK(movement::local_flow_recovery_due(head, 88, 2));
    CHECK(check(&head, true, 9, 88));
    head.blocked_ticks = 0;
    head.tick = 57;
    leader_route.points[1] = {32, 144};
    CHECK(!check(&head));
}

std::vector<movement::LocalFlowHead> saved_heads(runtime::Match& match) {
    const auto* table = runtime::find_rule_state(match.rule_state(), "local-flow");
    CHECK(table && table->bytes && table->restore);
    std::vector<movement::LocalFlowHead> heads(match.state().unit_slot_count);
    CHECK(movement::read_local_flow_heads(heads, table->bytes(table->context)));
    return heads;
}

void restore_heads(runtime::Match& match, const std::vector<movement::LocalFlowHead>& heads) {
    const auto* table = runtime::find_rule_state(match.rule_state(), "local-flow");
    CHECK(table && table->restore);
    std::vector<uint8_t> bytes(heads.size() * movement::local_flow_head_bytes);
    CHECK(movement::write_local_flow_heads(heads, bytes));
    CHECK(table->restore(table->context, bytes));
}

/// Read-only occupancy host for the explicit test adapter and copied first step.
/// Subsequent movement runs through the actual Match, with its real host.
struct AppliedProbe final : movement::Host {
    Scene& scene;
    bool enabled{};
    uint32_t tick{};
    std::vector<movement::LocalFlowHead> heads;
    std::vector<uint32_t> progress;

    AppliedProbe(Scene& scene, bool enabled)
        : scene(scene), enabled(enabled), tick(scene.match().simulation().tick),
          heads(saved_heads(scene.match())), progress(scene.match().state().unit_slot_count) {
        for (const auto id : scene.all_units())
            progress[id] = scene.ground(id).movement.last_motion_tick;
    }

    movement::LocalFlowBlocker
    at(const movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy) {
        using Obstacle = movement::LocalFlowObstacle;
        if (occupancy != ground_layer || cell[0] < 0 || cell[1] < 0 ||
            uint32_t(cell[0]) + 2 >= scene.width || uint32_t(cell[1]) + 2 >= map_height)
            return {Obstacle::stationary};
        auto& match = scene.match();
        movement::LocalFlowBlocker result{};
        bool coherent = true;
        const auto follower = static_cast<uint16_t>(unit.id);
        for (int16_t z = 0; z < 2; ++z)
            for (int16_t x = 0; x < 2; ++x) {
                const auto& plot =
                    match.spatial().plots[size_t(cell[1] + z) * scene.width + size_t(cell[0] + x)];
                if (plot.blocking_feature || plot.low_height != land_height ||
                    plot.high_height != land_height)
                    return {Obstacle::stationary};
                const auto id = plot.ground;
                if (id == oa::sim::spatial_state::no_unit || id == follower)
                    continue;
                const auto* other = match.ground_runtime(id);
                if (!other || !other->navigation.goal)
                    return {Obstacle::stationary};
                const auto& slot = match.spawn_slot(id);
                const auto* goal = other->navigation.goal;
                const auto& own = scene.ground(follower).navigation;
                coherent =
                    coherent &&
                    slot.record.owner_index == match.spawn_slot(follower).record.owner_index &&
                    slot.unit && slot.unit->primary == goal->order && own.goal &&
                    own.goal->order == match.spawn_slot(follower).unit->primary &&
                    runtime::same_forward_route_with_applied_head(
                        unit,
                        own,
                        other->geometry,
                        other->navigation,
                        &heads[id],
                        applied_goal_key(*goal),
                        tick,
                        enabled
                    );
                const auto waiting = tick - progress[id];
                if (result.obstacle != Obstacle::moving || waiting > result.waiting_ticks ||
                    (waiting == result.waiting_ticks && id < result.unit_id))
                    result = {Obstacle::moving, id, other->geometry.heading, waiting};
            }
        result.same_route_queue = result.obstacle == Obstacle::moving && coherent;
        return result;
    }

    bool can_occupy(
        const movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ) override {
        return at(unit, cell, occupancy).obstacle == movement::LocalFlowObstacle::clear;
    }

    void remove_occupancy(movement::Unit&) override {}

    void insert_occupancy(movement::Unit&) override {}

    void update_spatial_membership(movement::Unit&) override {}

    movement::LocalFlowDecision choose(uint16_t id, movement::LocalFlowCounters& counters) {
        auto& body = scene.ground(id);
        movement::LocalFlowProbe probe{};
        probe.context = this;
        probe.at = [](void* context,
                      const movement::Unit& unit,
                      std::array<int16_t, 2> cell,
                      uint8_t occupancy) {
            return static_cast<AppliedProbe*>(context)->at(unit, cell, occupancy);
        };
        probe.allow_reverse = false;
        probe.progress_at_tick_start = progress;
        const auto points = ground::steering_points(body.navigation);
        return movement::choose_local_flow(
            body.geometry,
            body.movement,
            points[1],
            ground::steering_target(body.geometry, points).point,
            tick,
            *this,
            probe,
            counters
        );
    }
};

struct AppliedOutcome {
    bool adjusted{}, old_certificate{}, applied_certificate{}, first_fresh_veto{};
    bool vacated_before_recovery{}, both_arrived{}, both_rested{};
    uint32_t finish_tick{};
    std::array<int16_t, 2> target{};
};

void prepare_applied_leader_checkpoint(Scene& scene) {
    auto& match = scene.match();
    const auto leader = scene.queue[0], follower = scene.queue[1];
    scene.run(60);
    for (const auto id : scene.queue) {
        const int32_t goal_x = id == leader ? 336 : 304;
        CHECK(!match.order_refused(
            match.issue_ground_move(id, {goal_x * pixel, 0, 128 * pixel}, false)
        ));
    }
    scene.run();
    match.simulation().tick = 56;
    match.place_unit_at(scene.gate, 80 * pixel, land_height * pixel, 224 * pixel, ground_layer);
    prepare_route(scene, leader, 176 * pixel, 176, 2);
    prepare_route(scene, follower, 152 * pixel - 1, 173, 2);
    for (const auto id : scene.queue) {
        auto& body = scene.ground(id);
        body.navigation.points[1] = {240, 128};
        body.navigation.points[2] = {272, 128};
        body.navigation.points[3] = {static_cast<int16_t>(id == leader ? 336 : 304), 128};
        body.navigation.count = 4;
        body.navigation.last_search_tick = 56;
        body.movement.flags = ground_layer;
        const uint16_t heading = id == leader ? uint16_t{16384} : uint16_t{0};
        match.spawn_slot(id).record.heading = heading;
        body.project_slot();
        body.movement.velocity = movement::aim_velocity(heading, 0, body.movement.speed);
        CHECK(
            body.navigation.goal &&
            body.navigation.goal->order == match.spawn_slot(id).unit->primary
        );
    }
    // Placement starts a new physical oracle. Discard traffic heads made
    // while the ordinary owned Goals were installed at the spawn positions.
    auto heads = saved_heads(match);
    heads[follower] = {};
    auto& front = heads[leader];
    front.points = {{{176, 176}, {160, 176}, {272, 128}}};
    front.goal_key = applied_goal_key(*scene.ground(leader).navigation.goal);
    front.tick = 56;
    front.count = 3;
    front.active = true;
    restore_heads(match, heads);
    scene.check_claims();
}

AppliedOutcome applied_leader_wave(bool enabled) {
    Scene scene(distributed_version, false, true);
    prepare_applied_leader_checkpoint(scene);
    auto& match = scene.match();
    const auto leader = scene.queue[0], follower = scene.queue[1];
    // First let the real Match apply the leader's retained backward leg. The
    // opt-in adapter may only inspect that actual current-tick publication.
    scene.run();
    const uint32_t admission_tick = match.simulation().tick;
    const auto start_cell = scene.ground(follower).geometry.cell;
    AppliedProbe probe(scene, enabled);
    auto& back = scene.ground(follower);
    auto& lead = scene.ground(leader);
    CHECK(probe.heads[leader].active && probe.heads[leader].tick == admission_tick);
    CHECK(!movement::local_flow_head_reached(lead.geometry, probe.heads[leader]));
    std::cout << "Applied admission: tick=" << admission_tick
              << ", front=" << lead.geometry.position[0] << ',' << lead.geometry.position[2]
              << ", back=" << back.geometry.position[0] << ',' << back.geometry.position[2]
              << ", frontP1=" << lead.navigation.points[1][0] << ',' << lead.navigation.points[1][1]
              << ", backP1=" << back.navigation.points[1][0] << ',' << back.navigation.points[1][1]
              << ", frontP2=" << lead.navigation.points[2][0] << ',' << lead.navigation.points[2][1]
              << ", backP2=" << back.navigation.points[2][0] << ',' << back.navigation.points[2][1]
              << ", frontHead=" << probe.heads[leader].active << ':'
              << probe.heads[leader].points[1][0] << ',' << probe.heads[leader].points[1][1]
              << ", backHead=" << probe.heads[follower].active << ':'
              << probe.heads[follower].points[1][0] << ',' << probe.heads[follower].points[1][1]
              << ", oldcert="
              << runtime::same_forward_route(
                     back.geometry, back.navigation, lead.geometry, lead.navigation
                 )
              << '\n';
    CHECK(!probe.heads[follower].active);
    CHECK(probe.heads[follower].count == 0);
    CHECK(probe.heads[follower].blocked_ticks < movement::local_flow_retry_ticks);
    CHECK(!movement::local_flow_recovery_due(probe.heads[follower], admission_tick, follower));
    AppliedOutcome result;
    result.old_certificate =
        runtime::same_forward_route(back.geometry, back.navigation, lead.geometry, lead.navigation);
    result.applied_certificate = runtime::same_forward_route_with_applied_head(
        back.geometry,
        back.navigation,
        lead.geometry,
        lead.navigation,
        &probe.heads[leader],
        applied_goal_key(*lead.navigation.goal),
        admission_tick,
        true
    );
    CHECK(result.old_certificate && !result.applied_certificate);
    const auto native_before = back.navigation;
    const auto goal_before = *back.navigation.goal;
    const auto* own_goal = back.navigation.goal;
    const auto* own_order = own_goal->order;
    const auto leader_goal = *lead.navigation.goal;
    // Ownership remains an outer certificate gate in both adapters. Merely
    // observing the same foreign body never acquires a following proof.
    auto& leader_record = match.spawn_slot(leader).record;
    const uint8_t owner = leader_record.owner_index;
    leader_record.owner_index = 1;
    const auto foreign = probe.at(back.geometry, lead.geometry.cell, ground_layer);
    probe.enabled = !enabled;
    const auto foreign_other = probe.at(back.geometry, lead.geometry.cell, ground_layer);
    probe.enabled = enabled;
    leader_record.owner_index = owner;
    CHECK(foreign.obstacle == movement::LocalFlowObstacle::moving);
    CHECK(foreign_other.obstacle == foreign.obstacle && foreign_other.unit_id == foreign.unit_id);
    CHECK(!foreign.same_route_queue && !foreign_other.same_route_queue);
    movement::LocalFlowCounters counters{};
    const auto decision = probe.choose(follower, counters);
    result.adjusted = decision.adjusted;
    CHECK(decision.mobile_blocked);
    CHECK(back.navigation.goal == own_goal && own_goal->order == own_order);
    CHECK(
        back.navigation.points == native_before.points &&
        back.navigation.count == native_before.count
    );
    auto effective = back.navigation;
    if (decision.adjusted) {
        auto& head = probe.heads[follower];
        head.points = {
            {{static_cast<int16_t>(back.geometry.position[0] >> 16),
              static_cast<int16_t>(back.geometry.position[2] >> 16)},
             {static_cast<int16_t>(decision.target[0] >> 16),
              static_cast<int16_t>(decision.target[2] >> 16)},
             native_before.points[2]}
        };
        head.count = 3;
        head.active = true;
        head.mobile_blocked = true;
        head.goal_key = applied_goal_key(*own_goal);
        head.tick = admission_tick;
        std::copy(head.points.begin(), head.points.end(), effective.points.begin());
        effective.count = 3;
        result.target = head.points[1];
        restore_heads(match, probe.heads);
    }
    // This is an actual native steering+sweep on copies against the live full
    // footprint claims. It distinguishes a fresh refusal from latched flags.
    auto step_unit = back.geometry;
    auto step_movement = back.movement;
    ground::steer_ground(
        step_unit, step_movement, effective, back.acceleration, back.deceleration, sea_level
    );
    const auto sweep = movement::integrate_swept_local_flow(
        step_unit, step_movement, admission_tick + 1, true, probe, counters
    );
    result.first_fresh_veto = sweep.status == movement::GroundSweepStatus::blocked;
    CHECK(sweep.status != movement::GroundSweepStatus::unsupported);
    std::array<bool, 2> arrived{};
    for (uint32_t elapsed = 0; elapsed < 900; ++elapsed) {
        scene.run();
        const auto& state = scene.ground(follower);
        if (elapsed == 0) {
            CHECK(state.geometry.position == step_unit.position);
            CHECK(state.geometry.cell == step_unit.cell);
            CHECK(state.geometry.heading == step_unit.heading);
            CHECK(state.movement.speed == step_movement.speed);
            CHECK(state.movement.velocity == step_movement.velocity);
        }
        if (elapsed + 1 < movement::local_flow_retry_ticks && state.geometry.cell != start_cell)
            result.vacated_before_recovery = true;
        for (size_t index = 0; index < scene.queue.size(); ++index) {
            const auto& current = scene.ground(scene.queue[index]);
            const auto& goal = index == 0 ? leader_goal : goal_before;
            arrived[index] = arrived[index] || ground::goal_contains_unit(goal, current.geometry);
            if (current.navigation.goal) {
                CHECK(
                    current.navigation.goal->order ==
                    match.spawn_slot(scene.queue[index]).unit->primary
                );
                CHECK(same_goal(*current.navigation.goal, goal));
            }
        }
        result.both_arrived = arrived[0] && arrived[1];
        result.both_rested = result.both_arrived && scene.ground(leader).movement.speed == 0 &&
                             scene.ground(follower).movement.speed == 0 &&
                             !scene.ground(leader).navigation.goal &&
                             !scene.ground(follower).navigation.goal;
        if (result.both_rested) {
            result.finish_tick = elapsed + 1;
            break;
        }
    }
    std::cout << "Applied leader oracle: adapter=" << enabled << ", adjusted=" << result.adjusted
              << ", target=" << result.target[0] << ',' << result.target[1]
              << ", fresh_veto=" << result.first_fresh_veto
              << ", vacated_before_recovery=" << result.vacated_before_recovery
              << ", arrived=" << result.both_arrived << ", rested=" << result.both_rested
              << ", finish=" << result.finish_tick << '\n';
    return result;
}

void applied_backward_leader_releases_an_existing_passing_leg() {
    const auto old = applied_leader_wave(false);
    const auto adapted = applied_leader_wave(true);
    CHECK(!old.adjusted && old.first_fresh_veto && !old.vacated_before_recovery);
    CHECK(adapted.adjusted && adapted.target[1] < 173);
    CHECK(!adapted.first_fresh_veto && adapted.vacated_before_recovery);
    CHECK(adapted.both_arrived && adapted.both_rested);
}

AppliedOutcome actual_following_caller_wave(uint32_t version) {
    Scene scene(version, false, true);
    prepare_applied_leader_checkpoint(scene);
    auto& match = scene.match();
    const auto leader = scene.queue[0], follower = scene.queue[1];
    const auto initial_heads = saved_heads(match);
    CHECK(initial_heads[leader].active);
    CHECK(!initial_heads[follower].active && initial_heads[follower].count == 0);
    const auto native_before = scene.ground(follower).navigation;
    const auto goals =
        std::array{*scene.ground(leader).navigation.goal, *scene.ground(follower).navigation.goal};
    const auto* follower_goal = scene.ground(follower).navigation.goal;
    const auto* follower_order = follower_goal->order;
    auto step_unit = scene.ground(follower).geometry;
    auto step_movement = scene.ground(follower).movement;
    const auto start_cell = step_unit.cell;
    const auto leader_cell = scene.ground(leader).geometry.cell;
    // No adapter chooses or restores a follower head. This tick executes the
    // real TickHost certificate, local choice and ordinary retained head.
    scene.run();
    const uint32_t admission_tick = match.simulation().tick;
    const auto* applied = match.local_flow_head(follower);
    const auto* front = match.local_flow_head(leader);
    CHECK(applied && front && front->active && front->tick == admission_tick);
    CHECK(!movement::local_flow_head_reached(scene.ground(leader).geometry, *front));
    CHECK(!movement::local_flow_recovery_due(*front, admission_tick, leader));
    const auto& actual = scene.ground(follower);
    CHECK(actual.navigation.goal == follower_goal && follower_goal->order == follower_order);
    CHECK(
        actual.navigation.points == native_before.points &&
        actual.navigation.count == native_before.count
    );
    CHECK(scene.ground(leader).geometry.cell == leader_cell);
    CHECK(actual.geometry.cell == start_cell);
    AppliedOutcome result;
    result.adjusted = applied->active;
    auto effective = native_before;
    if (applied->active) {
        CHECK(applied->count == 3 && applied->goal_key == applied_goal_key(*follower_goal));
        CHECK(applied->tick == admission_tick);
        CHECK(applied->blocked_ticks < movement::local_flow_retry_ticks);
        result.target = applied->points[1];
        std::copy(applied->points.begin(), applied->points.end(), effective.points.begin());
        effective.count = 3;
    }
    // Only the read-only collision host is reused here. It never runs its
    // decision adapter. Native copied steering+sweep must equal the first
    // actual TickHost step, identifying fresh refusal rather than old flags.
    AppliedProbe physical(scene, false);
    movement::LocalFlowCounters counters{};
    const auto& body = scene.ground(follower);
    ground::steer_ground(
        step_unit, step_movement, effective, body.acceleration, body.deceleration, sea_level
    );
    const auto sweep = movement::integrate_swept_local_flow(
        step_unit, step_movement, admission_tick, true, physical, counters
    );
    CHECK(sweep.status != movement::GroundSweepStatus::unsupported);
    result.first_fresh_veto = sweep.status == movement::GroundSweepStatus::blocked;
    CHECK(actual.geometry.position == step_unit.position);
    CHECK(actual.geometry.cell == step_unit.cell);
    CHECK(actual.geometry.heading == step_unit.heading);
    CHECK(actual.movement.speed == step_movement.speed);
    CHECK(actual.movement.velocity == step_movement.velocity);
    std::array<bool, 2> arrived{};
    for (uint32_t elapsed = 0; elapsed < 900; ++elapsed) {
        if (elapsed)
            scene.run();
        if (elapsed + 1 < movement::local_flow_retry_ticks &&
            scene.ground(follower).geometry.cell != start_cell)
            result.vacated_before_recovery = true;
        for (size_t index = 0; index < scene.queue.size(); ++index) {
            const auto& current = scene.ground(scene.queue[index]);
            arrived[index] =
                arrived[index] || ground::goal_contains_unit(goals[index], current.geometry);
            if (current.navigation.goal) {
                CHECK(
                    current.navigation.goal->order ==
                    match.spawn_slot(scene.queue[index]).unit->primary
                );
                CHECK(same_goal(*current.navigation.goal, goals[index]));
            }
        }
        result.both_arrived = arrived[0] && arrived[1];
        result.both_rested = result.both_arrived && scene.ground(leader).movement.speed == 0 &&
                             scene.ground(follower).movement.speed == 0 &&
                             !scene.ground(leader).navigation.goal &&
                             !scene.ground(follower).navigation.goal;
        if (result.both_rested) {
            result.finish_tick = elapsed + 1;
            break;
        }
    }
    std::cout << "Native applied-follow caller: version=" << version
              << ", active=" << result.adjusted << ", target=" << result.target[0] << ','
              << result.target[1] << ", fresh_veto=" << result.first_fresh_veto
              << ", vacated_before_recovery=" << result.vacated_before_recovery
              << ", arrived=" << result.both_arrived << ", rested=" << result.both_rested
              << ", finish=" << result.finish_tick << '\n';
    return result;
}

void actual_match_uses_the_following_certificate_only_in_eight() {
    const auto old = actual_following_caller_wave(distributed_version);
    const auto following = actual_following_caller_wave(following_version);
    CHECK(!old.adjusted && old.first_fresh_veto && !old.vacated_before_recovery);
    CHECK(following.adjusted && following.target[1] < 173);
    CHECK(!following.first_fresh_veto && following.vacated_before_recovery);
    CHECK(old.both_arrived && old.both_rested);
    CHECK(following.both_arrived && following.both_rested);
}

void print_legacy_trace() {
    for (uint32_t version = 0; version < battle_version; ++version)
        for (const auto control :
             {Pipeline::matched,
              Pipeline::different_goal,
              Pipeline::opposing_leg,
              Pipeline::idle_leader,
              Pipeline::cancelled_leader,
              Pipeline::retargeted_leader}) {
            const auto result = pipeline(version, control);
            std::cout << version << ',' << static_cast<unsigned>(control);
            for (const auto value : result.position)
                std::cout << ',' << value;
            for (const auto value : result.velocity)
                std::cout << ',' << value;
            std::cout << ',' << result.heading << ',' << result.turn << ',' << result.speed << ','
                      << unsigned(result.flags) << ',' << result.active << ',' << result.mobile;
            for (const auto& point : result.points)
                std::cout << ',' << point[0] << ',' << point[1];
            std::cout << '\n';
        }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--legacy-trace") == 0) {
        print_legacy_trace();
        return EXIT_SUCCESS;
    }
    uint32_t failures{};
    const auto run = [&failures](const char* name, const auto& test) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    };
    for (const auto version : {battle_version, distributed_version, following_version}) {
        const auto run_version = [&](const char* name, void (*test)(uint32_t)) {
            run(name, [&] { test(version); });
        };
        run_version("native two-unit queue", a_real_native_wave_waits_then_crosses_the_mouth);
        run_version(
            "native eight-unit queue", eight_real_units_cross_after_the_ordinary_gate_leaves
        );
        run_version(
            "distinct native goals on shared corners",
            distinct_native_goals_share_the_two_corner_queue
        );
        run_version("permanent native gate", a_permanent_native_gate_releases_mature_recovery);
        run_version(
            "actual pipeline coherence", actual_pipeline_rejects_incompatible_or_unowned_routes
        );
        run_version(
            "dying or nonpositive queue actors", dying_or_nonpositive_units_do_not_hold_the_queue
        );
    }
    run("historical profile coherence", historical_profiles_do_not_read_route_coherence);
    run("applied head certificate gates", applied_head_certificate_is_explicit_and_current);
    run("applied backward leader", applied_backward_leader_releases_an_existing_passing_leg);
    run("actual following caller", actual_match_uses_the_following_certificate_only_in_eight);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
