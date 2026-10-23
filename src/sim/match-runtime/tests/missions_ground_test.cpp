// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "match_tick_access.hpp"
#include "oa/base/text.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "oa/test/match_services.hpp"

using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x);   \
    } while (false)

namespace {
constexpr uint8_t kamikaze_kind = 7;
constexpr uint8_t guard_no_move_kind = 22;
constexpr uint8_t park_kind = 28;
constexpr uint8_t reclaim_kind = 32;
constexpr uint8_t repair_patrol_kind = 34;
constexpr uint8_t repair_unit_kind = 35;
constexpr uint8_t standby_mine_kind = 42;
constexpr int32_t map_cells = 32;

struct Services : oa::test::QuietServices {
    std::vector<uint32_t> speech;
    uint32_t footprint_changes{};
    uint32_t refreshes{};

    void command_sound(sim::unit_spawn::Slot&, uint32_t category) override {
        speech.push_back(category);
        caption.clear();
    }

    /// The match's speech hook: a speech with its order's caption.
    static void speak(void* context, sim::unit_spawn::Slot&, uint32_t category, const char* text) {
        auto& services = *static_cast<Services*>(context);
        services.speech.push_back(category);
        services.caption = text;
    }

    std::string caption; // the last speech's caption; empty for none

    void refresh_selected_unit(sim::unit_spawn::Slot&) override { ++refreshes; }

    void notify_footprint_changed(std::array<int16_t, 2>, std::array<int16_t, 2>) override {
        ++footprint_changes;
    }
};

using Scenario = oa::test::EmptyScenario;

// Dispatches the order once and applies the sweep's phase rule for results 0 and 1.
uint32_t step(
    sim::match_runtime::Match& match,
    sim::unit_spawn::Slot& unit,
    sim::simulation_state::Order& order,
    uint32_t events = 0
) {
    sim::match_runtime::MatchTickAccess host(match);
    order.wait_events = 0;
    const auto result = host.dispatch_mission(match.state(), unit.record, order, events);
    if (result == 0)
        order.phase = 0;
    else if (result == 1)
        ++order.phase;
    return result;
}

size_t plot(int32_t x, int32_t z) {
    return static_cast<size_t>(z * map_cells + x);
}

// A 2x2 feature at (8, 8) and a 1x1 wreck at (20, 20); the tree (2) is
// placed later.
void place_features(std::vector<sim::spatial_state::Plot>& plots) {
    for (int32_t z = 8; z < 10; ++z)
        for (int32_t x = 8; x < 10; ++x) {
            auto& cell = plots[plot(x, z)];
            cell.feature_word = (x == 8 && z == 8) ? 0 : sim::spatial_state::feature_continuation;
            cell.feature_back_x = static_cast<uint8_t>(x - 8);
            cell.feature_back_z = static_cast<uint8_t>(z - 8);
            cell.feature_footprint_x = cell.feature_footprint_z = 2;
            cell.blocking_feature = true;
        }
    plots[plot(20, 20)].feature_word = 1;
}

std::vector<FeatureDef> feature_table() {
    std::vector<FeatureDef> table(3);
    oa::base::text::copy_terminated(table[0].name, "ROCK01");
    table[0].footprint_x = table[0].footprint_z = 2;
    table[0].height = 10;
    table[0].energy = 10.0F;
    table[0].metal = 50.0F;
    table[0].flags = OA_FEATURE_FLAG_RECLAIMABLE | OA_FEATURE_FLAG_AUTO_RECLAIMABLE;
    oa::base::text::copy_terminated(table[1].name, "ARMPW_DEAD");
    table[1].footprint_x = table[1].footprint_z = 1;
    table[1].metal = 20.0F;
    table[1].flags = OA_FEATURE_FLAG_RECLAIMABLE;
    // Tree1 of the green world: energy only.
    oa::base::text::copy_terminated(table[2].name, "TREE1");
    table[2].footprint_x = table[2].footprint_z = 1;
    table[2].height = 40;
    table[2].energy = 250.0F;
    table[2].flags = OA_FEATURE_FLAG_RECLAIMABLE | OA_FEATURE_FLAG_AUTO_RECLAIMABLE;
    // No featuredead, featureburnt or featurereclamate remnants.
    for (auto& def : table) {
        def.dead_feature = sim::feature_runtime::no_feature;
        def.burnt_feature = sim::feature_runtime::no_feature;
        def.reclamate_feature = sim::feature_runtime::no_feature;
    }
    return table;
}

sim::ground_orders::Point cell_point(int32_t x, int32_t z) {
    return {(x * 16 + 8) << 16, 0, (z * 16 + 8) << 16};
}

struct PathFailure {
    uint16_t unit{};
    uint8_t kind{};
};

// Buildings stand on every cell around the unit, so a route search for a goal
// outside that ring finds no way through. The move keeps trying; the listener
// hears the failure once per order.
void unreachable_goals_are_reported() {
    constexpr int32_t cells = 32;
    constexpr uint8_t ground_height = 40;
    constexpr uint16_t mover_type = 1;
    constexpr uint16_t building_type = 2;
    constexpr uint16_t type_count = 3;
    constexpr sim::unit_spawn::AssetHandle mover_class = 1;
    constexpr int32_t home_x = 8;
    constexpr int32_t home_z = 8;
    constexpr int32_t blocked_x = 20;
    constexpr int32_t blocked_z = 8;
    constexpr int32_t open_home_x = 16;
    constexpr int32_t open_home_z = 16;
    constexpr int32_t open_x = 18;
    constexpr int32_t open_z = 16;
    constexpr int32_t stay_x = 16;
    constexpr int32_t stay_z = 22;

    formats::tnt::Map map;
    map.attribute_width = map.attribute_height = cells;
    map.sea_level = 20;
    map.attributes.resize(static_cast<std::size_t>(cells * cells));
    for (auto& attribute : map.attributes)
        attribute.height = ground_height;
    std::vector<sim::visibility_state::TerrainCell> terrain(
        static_cast<std::size_t>(cells * cells)
    );
    std::vector<sim::spatial_state::Plot> plots(static_cast<std::size_t>(cells * cells));
    for (auto& cell : plots)
        cell.low_height = cell.high_height = ground_height;
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.pixels = {1};
    const std::array masks{mask};
    auto model = std::make_shared<formats::objects3d::Model>();
    model->objects.resize(1);
    model->objects[0].name = "root";
    model->objects[0].vertices = {{0, 0, 0}, {0, 20 << 16, 0}};
    auto script = std::make_shared<formats::cob::CobProgram>();
    script->code = {sim::script_vm::opcode::return_};
    script->scripts = {{"Create", 0}};
    script->entry_points = {0};
    script->piece_names = {"root"};

    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    std::array<data::unit_definitions::RuntimeDefinitionMetadata, type_count> metadata;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    const std::array<uint8_t, 1> yard{0x2f};
    for (uint16_t i = 1; i < type_count; ++i) {
        types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
        types[i].simulation.maximum_health = 100;
        types[i].footprint_x = types[i].footprint_z = 1;
        types[i].bm_code = i == building_type ? 0 : 1;
        types[i].model = reinterpret_cast<uintptr_t>(model.get());
        types[i].cob = reinterpret_cast<uintptr_t>(script.get());
        loaded[i].model = model;
        loaded[i].script = script;
        loaded[i].type = types[i];
        loaded[i].unit_name = i == building_type ? "WALL" : "MOVER";
        defs[i].sight_distance = 64;
        defs[i].acceleration_fixed = 65536;
        defs[i].brake_rate_fixed = 65536;
        defs[i].max_velocity_fixed = 2 * 65536;
        defs[i].turn_rate = 1024;
        metadata[i].footprint_x = metadata[i].footprint_z = 1;
        fields[i].definition = &defs[i];
        fields[i].runtime_metadata = &metadata[i];
        fields[i].target_masks = &target_masks;
        fields[i].movement_class = sim::unit_spawn::AssetHandle{0};
    }
    fields[mover_type].movement_class = mover_class;
    fields[building_type].yard_mask = yard;
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    sim::match_runtime::OfflineInputs input{map,   loaded,    types,     fields, weapons, terrain,
                                            masks, cells / 2, cells / 2, 16,     2,       0,
                                            30,    1,         &scenario, {},     plots,   {},
                                            0,     0,         0.0F};
    sim::match_runtime::Match match(input, services);
    match.configure_strategic_environment({0, 0.5F, 0});
    match.simulation().players[0].present = true;
    match.simulation().players[0].status = 1;
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    match.configure_player_alliances(0, allies);
    match.configure_outcomes(0, allies, false);
    match.state().unit_defs[mover_type].abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE;
    CHECK(match.movement_map(mover_class) != nullptr);

    const auto centre = [](int32_t cell) { return static_cast<uint32_t>(cell * 16 + 8) << 16; };
    for (int32_t dz = -1; dz <= 1; ++dz)
        for (int32_t dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0)
                continue;
            auto* wall = match.create(
                {0, building_type, {centre(home_x + dx), 0, centre(home_z + dz)}, true, 1, 0}
            );
            CHECK(wall != nullptr);
            CHECK(wall->record.cell_x == home_x + dx && wall->record.cell_z == home_z + dz);
        }
    auto* mover = match.create({0, mover_type, {centre(home_x), 0, centre(home_z)}, true, 1, 0});
    CHECK(mover && mover->record.movement && match.ground_runtime(mover->unit_index));
    CHECK(mover->record.cell_x == home_x && mover->record.cell_z == home_z);

    std::vector<PathFailure> heard;
    sim::match_runtime::EventHooks hooks;
    hooks.context = &heard;
    hooks.path_failed = [](void* context, const World&, uint16_t unit, uint8_t kind) {
        static_cast<std::vector<PathFailure>*>(context)->push_back({unit, kind});
    };
    CHECK(match.add_event_listener(hooks) != 0);

    auto see_all = [&] {
        auto& bits = match.sight_mutable().player_bits;
        std::fill(bits.begin(), bits.end(), uint16_t{0xffff});
    };
    auto advance = [&](int count) {
        for (int step_index = 0; step_index < count; ++step_index) {
            see_all();
            ++match.simulation().tick;
            match.tick();
        }
    };

    match.simulation().tick = 100;
    const auto mover_index = mover->unit_index;
    auto& issued =
        match.issue_ground_move(mover_index, {centre(blocked_x), 0, centre(blocked_z)}, false);
    CHECK(!match.order_refused(issued));
    advance(120);
    CHECK(heard.size() == 1);
    CHECK(heard[0].unit == mover_index);
    CHECK(heard[0].kind == sim::ground_orders::move_ground_kind);

    (void)match.issue_ground_move(mover_index, {centre(blocked_x), 0, centre(blocked_z)}, false);
    advance(120);
    CHECK(heard.size() == 2);
    CHECK(heard[1].unit == mover_index);
    CHECK(heard[1].kind == sim::ground_orders::move_ground_kind);

    auto* wanderer =
        match.create({0, mover_type, {centre(open_home_x), 0, centre(open_home_z)}, true, 1, 0});
    CHECK(wanderer && wanderer->record.cell_x == open_home_x);
    (void)match.issue_ground_move(wanderer->unit_index, {centre(open_x), 0, centre(open_z)}, false);
    advance(120);
    CHECK(heard.size() == 2);

    auto* stayer = match.create({0, mover_type, {centre(stay_x), 0, centre(stay_z)}, true, 1, 0});
    CHECK(stayer && stayer->record.cell_x == stay_x && stayer->record.cell_z == stay_z);
    (void)match.issue_ground_move(stayer->unit_index, {centre(stay_x), 0, centre(stay_z)}, false);
    advance(40);
    CHECK(heard.size() == 2);
    std::cout << "unreachable goals are reported passed\n";
}
} // namespace

int main() {
    formats::tnt::Map map;
    map.attribute_width = map.attribute_height = map_cells;
    map.attributes.resize(map_cells * map_cells);
    std::vector<sim::visibility_state::TerrainCell> terrain_values(map_cells * map_cells);
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.transparent = 0;
    mask.pixels = {1};
    const std::array masks{mask};
    auto model = std::make_shared<formats::objects3d::Model>();
    model->objects.resize(1);
    model->objects[0].name = "root";
    auto script = std::make_shared<formats::cob::CobProgram>();
    script->code = {sim::script_vm::opcode::return_};
    script->scripts = {{"Create", 0}, {"StopBuilding", 0}};
    script->entry_points = {0, 0};
    script->piece_names = {"root"};

    // 1 builder (mobile), 2 mine (building), 3 ARMPW (mobile, the wreck's type).
    constexpr size_t type_count = 4;
    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    for (size_t i = 1; i < type_count; ++i) {
        loaded[i].model = model;
        loaded[i].script = script;
        types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
        types[i].simulation.maximum_health = 100;
        types[i].footprint_x = types[i].footprint_z = 1;
        types[i].bm_code = i == 2 ? 0 : 1;
        types[i].model = reinterpret_cast<uintptr_t>(model.get());
        types[i].cob = reinterpret_cast<uintptr_t>(script.get());
        defs[i].sight_distance = 200;
        defs[i].acceleration_fixed = 65536;
        defs[i].brake_rate_fixed = 65536;
        defs[i].max_velocity_fixed = 2 * 65536;
        defs[i].turn_rate = 1024;
        defs[i].worker_time = 60;
        defs[i].build_distance = 64;
        defs[i].build_time = 100;
        defs[i].energy_storage = 1000.0F;
        defs[i].metal_storage = 1000.0F;
    }
    defs[1].weapon1 = "NUKE";
    loaded[3].unit_name = "ARMPW";
    for (size_t i = 1; i < type_count; ++i)
        loaded[i].type = types[i];
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    const std::array<uint8_t, 1> yard{4};
    for (size_t i = 1; i < type_count; ++i) {
        fields[i].definition = &defs[i];
        fields[i].yard_mask = yard;
        fields[i].runtime_metadata = &metadata;
        fields[i].target_masks = &target_masks;
        if (i != 2)
            fields[i].movement_class = 0;
    }
    sim::combat_state::WeaponRegistry weapons;
    (void)sim::combat_state::install_weapon_text(
        weapons, "[NUKE]{id=1; reloadtime=0.4; energypershot=120; metalpershot=60;}"
    );
    std::vector<sim::spatial_state::Plot> collision_plots(map_cells * map_cells);
    place_features(collision_plots);
    const auto features = feature_table();
    Services services;
    Scenario scenario;
    sim::match_runtime::OfflineInputs input{
        map, loaded, types, fields,    weapons, terrain_values,  masks, 16, 16, 5,    2,
        0,   30,     1,     &scenario, {},      collision_plots, {},    0,  0,  0.0F, features
    };
    sim::match_runtime::Match match(input, services);
    match.set_speech_hooks({&services, &Services::speak});
    match.state().unit_defs[1].abilities =
        OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_REPAIR |
        OA_UNIT_DEF_ABILITY_CAN_RECLAMATE | OA_UNIT_DEF_ABILITY_CAN_RESURRECT;
    match.configure_strategic_environment({0, 0.5f, 0});
    for (uint8_t p = 0; p < 2; ++p) {
        match.simulation().players[p].present = true;
        match.simulation().players[p].status = p == 0 ? 1 : 2;
        std::array<uint8_t, 10> allies{};
        allies[p] = 1;
        match.configure_player_alliances(p, allies);
    }
    auto& game = match.state().game;
    game.tick = 100;

    auto* builder = match.create({0, 1, {(120u << 16), 0, (120u << 16)}, true, 1, 0});
    CHECK(builder && builder->record.movement && match.ground_runtime(builder->unit_index));
    const auto builder_id = builder->unit_index;

    // Reclaim: walk to the 2x2 rock, stand in the build stance, count down
    // (metal + energy) / 2 + 15 ticks in steps of two, then collect and clear it.
    {
        auto& order = match.issue_feature_reclaim(builder_id, cell_point(9, 9), false);
        CHECK(order.kind == reclaim_kind);
        CHECK(step(match, *builder, order) == 1 && order.wait_events == 0xe0);
        CHECK(step(match, *builder, order, sim::ground_orders::path_failed_event) == 8);
        order.phase = 1;
        CHECK(step(match, *builder, order) == 1);
        CHECK(
            (order.flags & sim::match_runtime::MatchTickAccess::building_flag) &&
            (builder->record.build_flags & 1)
        );
        CHECK(step(match, *builder, order) == 1 && order.phase == 3);
        services.speech.clear();
        CHECK(step(match, *builder, order) == 2);
        CHECK(services.speech == std::vector<uint32_t>{0x0b});
        CHECK(
            order.wake_tick == game.tick + 2 &&
            builder->record.decloak_until_tick == game.tick + 300
        );
        uint32_t steps = 0;
        uint32_t result = 2;
        while (result == 2 && steps < 100) {
            result = step(match, *builder, order);
            ++steps;
        }
        // 45 ticks of work in steps of two; phase 3 repeats until they run out.
        CHECK(result == 1 && steps == 22 && order.phase == 4);
        CHECK(step(match, *builder, order) == 1 && order.phase == 5);
        const auto changes = services.footprint_changes;
        CHECK(step(match, *builder, order) == 5);
        CHECK(builder->record.economy.energy.produced == 10.0F);
        CHECK(builder->record.economy.metal.produced == 50.0F);
        for (int32_t z = 8; z < 10; ++z)
            for (int32_t x = 8; x < 10; ++x)
                CHECK(
                    match.spatial().plots[plot(x, z)].feature_word == sim::spatial_state::no_feature
                );
        CHECK(services.footprint_changes == changes + 1);
        // The rock is gone: the order reports the failure and ends.
        services.speech.clear();
        CHECK(step(match, *builder, order) == 8 && services.speech == std::vector<uint32_t>{7});
        CHECK(services.caption == "Reclamation failed");
        match.stop_orders(builder_id);
    }

    // Resurrect: the ARMPW wreck is raised as a finished ARMPW at one hit
    // point after build_time * 0.3 / (workertime / 30) steps, then repaired.
    {
        builder->record.build_flags = 0;
        auto& order = match.insert_ground_order(builder_id, 37, cell_point(20, 20));
        CHECK(step(match, *builder, order) == 1 && order.wait_events == 0xe0);
        CHECK(step(match, *builder, order) == 1);
        CHECK(step(match, *builder, order) == 1);
        services.speech.clear();
        CHECK(step(match, *builder, order) == 1 && services.speech == std::vector<uint32_t>{0x0b});
        uint32_t steps = 0;
        while (step(match, *builder, order) == 2 && steps < 100)
            ++steps;
        CHECK(steps == 15 && order.phase == 5);
        CHECK(step(match, *builder, order) == 1);
        CHECK(match.spatial().plots[plot(20, 20)].feature_word == sim::spatial_state::no_feature);
        sim::unit_spawn::Slot* raised = nullptr;
        for (auto& slot : match.world().slots)
            if (slot.unit && slot.record.type_index == 3)
                raised = &slot;
        CHECK(raised && raised->record.health == 1 && raised->record.build_remaining == 0.0F);
        CHECK(raised->record.position.x == cell_point(20, 20)[0]);
        services.speech.clear();
        CHECK(step(match, *builder, order) == 5 && services.speech == std::vector<uint32_t>{8});
        CHECK(services.caption == "Resurrection complete");
        auto* head = match.orders(builder_id).primary;
        CHECK(head && head->kind == repair_unit_kind);
        match.stop_orders(builder_id);
    }

    // RepairPatrol: phase 0 queues the looping copy; an arrival rotates; with
    // energy in store the damaged ally nearby gets a repair and a walk back.
    auto* damaged = match.create({0, 3, {(140u << 16), 0, (120u << 16)}, true, 1, 0});
    CHECK(damaged);
    damaged->record.health = 40;
    {
        auto& order = match.issue_repair_patrol(builder_id, cell_point(2, 2), false);
        CHECK(order.kind == repair_patrol_kind);
        CHECK(step(match, *builder, order) == 1);
        std::vector<uint8_t> kinds;
        match.visit_primary_queue(builder_id, [&](const auto& queued) {
            kinds.push_back(queued.kind);
        });
        CHECK((kinds == std::vector<uint8_t>{repair_patrol_kind, repair_patrol_kind}));
        CHECK(step(match, *builder, order, sim::ground_orders::arrived_event) == 6);
        auto& player = match.state().game.players[0];
        player.energy = 900.0F;
        player.energy_storage = 1000.0F;
        player.metal = 900.0F;
        player.metal_storage = 1000.0F;
        builder->record.flags &= ~OA_UNIT_FLAG_MOVE_ORDER_MASK;
        CHECK(step(match, *builder, order) == 6);
        auto* head = match.orders(builder_id).primary;
        CHECK(head && head->kind == repair_unit_kind);
        CHECK(head->next && head->next->kind == sim::ground_orders::move_ground_kind);
        match.stop_orders(builder_id);
    }
    damaged->record.health = 100;

    // RepairPatrol with metal low reclaims an auto-reclaimable feature in sight.
    {
        (void)sim::feature_runtime::place_feature(
            match.state(),
            match.feature_host(),
            plot(7, 7),
            0,
            nullptr,
            nullptr,
            sim::feature_runtime::no_player
        );
        CHECK(
            match.spatial().plots[plot(8, 8)].feature_word ==
            sim::spatial_state::feature_continuation
        );
        auto& player = match.state().game.players[0];
        player.metal = 10.0F;
        auto& order = match.issue_repair_patrol(builder_id, cell_point(2, 2), false);
        CHECK(step(match, *builder, order) == 1);
        CHECK(step(match, *builder, order) == 3 && order.wait_events == 0);
        auto* head = match.orders(builder_id).primary;
        CHECK(head && head->kind == reclaim_kind && head != &order);
        match.stop_orders(builder_id);
    }

    // BuildWeapon: 12-tick reload paid in five-tick shares of 120 energy and
    // 60 metal, then one round is stockpiled and the count drops.
    {
        auto& order = match.issue_build_weapon(builder_id, 0, 1);
        CHECK(match.orders(builder_id).secondary == &order);
        // queued_build_count counts the secondary list too: BuildWeapon's descriptor
        // (0xC0140) has bit 0x100 and its first parameter is the slot.
        CHECK(match.queued_build_count(builder_id, 0) == 1);
        CHECK(match.queued_build_count(builder_id, 1) == 0);
        CHECK(step(match, *builder, order) == 1);
        auto& economy = builder->record.economy;
        economy.energy.requested = economy.energy.accepted = economy.energy.gate = 0.0F;
        economy.metal.requested = economy.metal.accepted = economy.metal.gate = 0.0F;
        CHECK(step(match, *builder, order) == 2 && order.wake_tick == game.tick + 5);
        CHECK(economy.energy.requested == 50.0F && economy.metal.requested == 25.0F);
        CHECK(step(match, *builder, order) == 2);
        CHECK(step(match, *builder, order) == 1);
        CHECK(economy.energy.requested == 120.0F && economy.metal.requested == 60.0F);
        CHECK(builder->record.weapons[0].stockpile == 0);
        CHECK(step(match, *builder, order) == 0 && order.phase == 0);
        CHECK(builder->record.weapons[0].stockpile == 1);
        CHECK(match.queued_build_count(builder_id, 0) == 0);
        CHECK(step(match, *builder, order) == 5);
        match.stop_orders(builder_id);
    }

    // Park: a ground unit walks to the border of a clearing around itself.
    {
        auto& order = match.insert_ground_order(builder_id, park_kind);
        CHECK(step(match, *builder, order) == 1 && order.wait_events == 0xe0);
        CHECK(step(match, *builder, order) == 0 && order.wake_tick == game.tick + 0x1e);
        order.phase = 1;
        CHECK(step(match, *builder, order, sim::ground_orders::arrived_event) == 5);
        match.stop_orders(builder_id);
    }

    // Guard_NoMove: a lost target switches to the search phase; with nothing
    // aimed at, the guard keeps waiting; an empty search restarts.
    {
        auto& order = match.insert_ground_order(builder_id, guard_no_move_kind);
        CHECK(step(match, *builder, order, 8) == 2 && order.phase == 3);
        CHECK(step(match, *builder, order) == 0);
        CHECK(step(match, *builder, order) == 1 && order.wake_tick == game.tick + 0x1e);
        CHECK(step(match, *builder, order) == 2 && order.phase == 1);
        match.stop_orders(builder_id);
    }

    // Attack_Kamikaze: arrival self-destructs through a queued SelfDestruct.
    {
        auto& order = match.insert_ground_order(builder_id, kamikaze_kind, cell_point(12, 7));
        CHECK(step(match, *builder, order) == 1);
        CHECK(order.wait_events == (0xe0u | 1u) && order.wake_tick == game.tick + 0x3c);
        services.speech.clear();
        CHECK(step(match, *builder, order, sim::ground_orders::arrived_event) == 5);
        CHECK(services.speech == std::vector<uint32_t>{6});
        const auto& queues = match.orders(builder_id);
        CHECK(
            (queues.primary && queues.primary->kind == sim::match_runtime::self_destruct_kind) ||
            (queues.secondary && queues.secondary->kind == sim::match_runtime::self_destruct_kind)
        );
        match.stop_orders(builder_id);
    }

    // Standby_Mine is a building's mission.
    {
        auto& order = match.insert_ground_order(builder_id, standby_mine_kind);
        CHECK(step(match, *builder, order) == 7);
        match.stop_orders(builder_id);
        auto* mine = match.create({0, 2, {(40u << 16), 0, (200u << 16)}, true, 1, 0});
        CHECK(mine);
        auto& armed = match.insert_ground_order(mine->unit_index, standby_mine_kind);
        CHECK(step(match, *mine, armed) == 1);
        CHECK(armed.wait_events == 0x10001u && armed.wake_tick == game.tick + 1);
        match.stop_orders(mine->unit_index);
    }

    // Wait with no search radius: the plain timed wait.
    {
        auto& order =
            match.insert_ground_order(builder_id, sim::match_runtime::wait_kind, std::nullopt, 40);
        CHECK(step(match, *builder, order) == 1 && order.wake_tick == game.tick + 40);
        CHECK(step(match, *builder, order) == 5);
        match.stop_orders(builder_id);
    }

    // stop_building: the order flags' bit 0x40 gates StopBuilding and is
    // cleared. share_script_start shares its COB index (1) with no arguments.
    {
        struct Forwarded {
            uint32_t count{};
            uint16_t unit{};
            int16_t function{};
            uint8_t arguments{};
            std::array<uint32_t, 4> locals{};
        } forwarded;

        match.multiplayer.context = &forwarded;
        match.multiplayer.script_started = [](void* context,
                                              uint16_t unit,
                                              int16_t function,
                                              uint8_t arguments,
                                              const std::array<uint32_t, 4>& locals) {
            auto& f = *static_cast<Forwarded*>(context);
            ++f.count;
            f.unit = unit;
            f.function = function;
            f.arguments = arguments;
            f.locals = locals;
        };
        auto& order =
            match.insert_ground_order(builder_id, sim::match_runtime::wait_kind, std::nullopt, 40);
        sim::match_runtime::MatchTickAccess host(match);
        order.flags |= sim::match_runtime::MatchTickAccess::building_flag;
        host.stop_building(*builder, order);
        CHECK((order.flags & sim::match_runtime::MatchTickAccess::building_flag) == 0);
        CHECK(forwarded.count == 1 && forwarded.unit == builder_id && forwarded.function == 1);
        CHECK(forwarded.arguments == 0 && forwarded.locals == (std::array<uint32_t, 4>{}));
        host.stop_building(*builder, order);
        CHECK((order.flags & sim::match_runtime::MatchTickAccess::building_flag) == 0);
        CHECK(forwarded.count == 1);
        match.multiplayer = {};
        match.stop_orders(builder_id);
    }

    // movement_rate: Movement.flags bit 2, a carrier or no speed and no turn give 0;
    // otherwise signed compares of the speed with moverate1 and moverate2.
    {
        sim::unit_movement::Movement movement;
        oa::Unit unit{};
        oa::UnitDef def{};
        def.move_rate1 = 2 << 16;
        def.move_rate2 = 4 << 16;
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 0);
        movement.turn = 1;
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 1);
        movement.speed = 2 << 16;
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 1);
        movement.speed = (2 << 16) + 1;
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 2);
        movement.speed = 4 << 16;
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 2);
        movement.speed = (4 << 16) + 1;
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 3);
        movement.speed = -(8 << 16);
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 1);
        movement.flags = 0x04;
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 0);
        movement.flags = 0;
        unit.attach_parent = 1;
        CHECK(sim::match_runtime::MatchTickAccess::movement_rate(movement, unit, def) == 0);
    }

    // PATROL given to a type with the repair bit is RepairPatrol (command 9). With energy in store the builder repairs the raised ARMPW
    // (one hit point) beside its route and walks back to it. With the metal
    // store under a fifth of its capacity it looks for features to reclaim,
    // but a tree (energy only) is taken only while the energy store is under
    // a fifth of its capacity or has room for it: past the trees with 900 of
    // 1000 it takes neither; with 500 it stops for each, takes its 250
    // energy and patrols on.
    {
        std::array<uint8_t, 10> allies{};
        allies[0] = 1;
        match.configure_outcomes(0, allies, false);
        match.state().unit_defs[1].flags |= OA_UNIT_DEF_FLAG_BUILDER;
        sim::unit_spawn::Slot* raised = nullptr;
        for (auto& slot : match.world().slots)
            if (slot.unit && slot.record.type_index == 3 && slot.record.health == 1)
                raised = &slot;
        CHECK(raised);
        auto& player = match.state().game.players[0];
        player.metal = 10.0F;
        player.energy = 900.0F;
        // The feature search samples every third cell around the unit, so these rows
        // are ones the grid lands on from the route.
        const std::array<int32_t, 2> tree_rows{10, 19};
        for (const auto z : tree_rows)
            (void)sim::feature_runtime::place_feature(
                match.state(),
                match.feature_host(),
                plot(23, z),
                2,
                nullptr,
                nullptr,
                sim::feature_runtime::no_player
            );
        const auto trees_left = [&] {
            uint32_t left = 0;
            for (const auto z : tree_rows)
                left += match.spatial().plots[plot(23, z)].feature_word == 2 ? 1 : 0;
            return left;
        };
        auto* patroller = match.create({0, 1, {(424u << 16), 0, (56u << 16)}, true, 1, 0});
        CHECK(patroller && match.ground_runtime(patroller->unit_index));
        const auto id = patroller->unit_index;
        auto& patrol = match.issue_patrol(id, cell_point(26, 21), false);
        CHECK(patrol.kind == repair_patrol_kind);
        // Runs the match, recording each order that takes the lead from the
        // patrol, which must stay queued behind it.
        std::vector<uint8_t> detours;
        uint8_t leading = repair_patrol_kind;
        const auto patrol_for = [&](uint32_t ticks, bool until_trees_gone) {
            for (uint32_t step = 0; step < ticks; ++step) {
                if (until_trees_gone && trees_left() == 0)
                    break;
                match.simulation().tick = game.tick + 1;
                match.tick();
                const auto* head = match.orders(id).primary;
                CHECK(head);
                if (head->kind != leading && head->kind != repair_patrol_kind &&
                    head->kind != sim::ground_orders::move_ground_kind) {
                    detours.push_back(head->kind);
                    bool behind = false;
                    for (const auto* queued = head->next; queued; queued = queued->next)
                        behind = behind || queued->kind == repair_patrol_kind;
                    CHECK(behind);
                }
                leading = head->kind;
            }
        };
        auto& credit = patroller->record.economy;
        patrol_for(900, false);
        CHECK((detours == std::vector<uint8_t>{repair_unit_kind}));
        CHECK(raised->record.health == 100);
        CHECK(trees_left() == 2 && credit.energy.produced == 0.0F);
        player.energy = 500.0F;
        patrol_for(3000, true);
        CHECK(trees_left() == 0);
        CHECK((detours == std::vector<uint8_t>{repair_unit_kind, reclaim_kind, reclaim_kind}));
        CHECK(credit.energy.produced == 500.0F && credit.metal.produced == 0.0F);
        const auto* head = match.orders(id).primary;
        CHECK(head && head->kind == repair_patrol_kind);
        match.stop_orders(id);
    }

    // Display rules that say "working" once: stage 3 speaks and moves to
    // stage 4, which reclaims silently and reaches stage 5 one step sooner.
    {
        std::vector<sim::spatial_state::Plot> quiet_plots(map_cells * map_cells);
        place_features(quiet_plots);
        input.collision_plots = quiet_plots;
        input.display.reclaim_voice_once = true;
        Services quiet_services;
        sim::match_runtime::Match quiet(input, quiet_services);
        quiet.set_speech_hooks({&quiet_services, &Services::speak});
        quiet.state().unit_defs[1].abilities =
            OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_RECLAMATE;
        quiet.configure_strategic_environment({0, 0.5f, 0});
        for (uint8_t p = 0; p < 2; ++p) {
            quiet.simulation().players[p].present = true;
            quiet.simulation().players[p].status = p == 0 ? 1 : 2;
        }
        quiet.state().game.tick = 100;
        auto* worker = quiet.create({0, 1, {(120u << 16), 0, (120u << 16)}, true, 1, 0});
        CHECK(worker != nullptr);
        auto& order = quiet.issue_feature_reclaim(worker->unit_index, cell_point(9, 9), false);
        CHECK(step(quiet, *worker, order) == 1);
        CHECK(step(quiet, *worker, order) == 1);
        CHECK(step(quiet, *worker, order) == 1 && order.phase == 3);
        quiet_services.speech.clear();
        CHECK(step(quiet, *worker, order) == 2 && order.phase == 4);
        CHECK(quiet_services.speech == std::vector<uint32_t>{0x0b});
        uint32_t steps = 0;
        uint32_t result = 2;
        while (result == 2 && steps < 100) {
            result = step(quiet, *worker, order);
            ++steps;
        }
        // The same 45 ticks of work, with no further voice and no extra step.
        CHECK(result == 1 && steps == 22 && order.phase == 5);
        CHECK(quiet_services.speech == std::vector<uint32_t>{0x0b});
        CHECK(step(quiet, *worker, order) == 5);
        CHECK(worker->record.economy.metal.produced == 50.0F);
        quiet.stop_orders(worker->unit_index);
    }

    unreachable_goals_are_reported();
    std::cout << "ground missions passed\n";
    return 0;
}
