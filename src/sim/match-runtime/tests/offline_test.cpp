// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime.hpp"
#include "oa/base/text.hpp"
#include <iostream>
#include <bit>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <cstdint>
#include "oa/test/match_services.hpp"

// Raw words of a unit's economy block (energy then metal accumulators).
std::array<uint32_t, 12>& economy_words(oa::Unit& unit) {
    return *reinterpret_cast<std::array<uint32_t, 12>*>(&unit.economy);
}

using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)

// A UnitWeapon.flags bit the sound callback sets, to show mission dispatch keeps it.
constexpr uint8_t weapon_bit_set_by_sound = 0x40;

struct Services : oa::test::QuietServices {
    std::vector<std::string> calls;
    uint16_t expected_id{1};
    bool strict_activation{true};

    void activation_sound(sim::unit_spawn::Slot& slot, sim::unit_activation::Sound sound) override {
        if (strict_activation)
            CHECK(slot.unit_index == expected_id && sound == sim::unit_activation::Sound::activate);
        calls.push_back("sound");
    }

    std::function<void(sim::unit_spawn::Slot&, uint32_t)> command;

    void command_sound(sim::unit_spawn::Slot& slot, uint32_t category) override {
        if (!command)
            throw std::runtime_error("unexpected command sound");
        command(slot, category);
    }

    void attachment_notification(sim::unit_spawn::Slot&, uint32_t) override {
        oa::test::unexpected_call("attachment_notification");
    }

    void refresh_selected_unit(sim::unit_spawn::Slot& slot) override {
        if (strict_activation)
            CHECK(slot.unit_index == expected_id);
        calls.push_back("refresh");
    }

    void emit_sfx(sim::unit_spawn::Slot&, uint32_t, int32_t) override {
        oa::test::unexpected_call("emit_sfx");
    }

    void explode_piece(sim::unit_spawn::Slot&, uint32_t, int32_t) override {
        oa::test::unexpected_call("explode_piece");
    }

    void attach_unit(sim::unit_spawn::Slot&, int32_t, int32_t, int32_t) override {
        oa::test::unexpected_call("attach_unit");
    }

    void drop_unit(sim::unit_spawn::Slot&, int32_t) override {
        oa::test::unexpected_call("drop_unit");
    }

    void refresh_plot_height_range(std::array<int16_t, 2>, std::array<int16_t, 2>) override {
        calls.push_back("masked");
    }

    void notify_object_footprint_removed(oa::sim::spatial_state::Unit&, uint32_t) override {
        calls.push_back("removed");
    }

    void notify_footprint_changed(std::array<int16_t, 2>, std::array<int16_t, 2>) override {
        calls.push_back("footprint");
    }
};

using Scenario = oa::test::EmptyScenario;

struct Start : sim::unit_spawn::StartHost {
    uint16_t commander_type_for_side(uint8_t side) override {
        CHECK(side == 1);
        return 1;
    }

    void report_missing_start_position(int32_t) override {
        throw std::runtime_error("unexpected missing start");
    }

    void set_camera_position(int32_t x, int32_t z, uint32_t flags) override {
        CHECK(x == -32 && z == 32 && flags == 0);
    }
};

// Records the FBI loader filled become Game.unit_defs whole; only the target
// category handles are renumbered into this match's mask table.
void loader_records_become_the_world_table() {
    formats::tnt::Map map;
    map.attribute_width = map.attribute_height = 16;
    map.attributes.resize(256);
    std::vector<sim::visibility_state::TerrainCell> terrain_values(256);
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.pixels = {1};
    const std::array masks{mask};
    std::array<sim::unit_spawn::LoadedType, 2> loaded;
    std::array<sim::unit_spawn::Type, 2> types;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    std::array<sim::match_runtime::RuntimeTypeFields, 2> fields{};
    fields[1].target_masks = &target_masks;
    std::array<UnitDef, 2> records{};
    oa::base::text::copy_terminated(records[0].unit_name, "None");
    oa::base::text::copy_terminated(records[1].unit_name, "ARMSOLAR");
    records[1].type_id = 1;
    records[1].flags = OA_UNIT_DEF_FLAG_AVAILABLE | OA_UNIT_DEF_FLAG_Z_BUFFER;
    records[1].footprint_x = records[1].footprint_z = 5;
    records[1].weapon1 = 1;
    records[1].yard_map = 7;
    records[1].build_ids = 3;
    records[1].gui_page_count = 2;
    records[1].damage_modifier = 21845;
    records[1].primary_bad_target_category = 99;
    std::vector<sim::spatial_state::Plot> collision_plots(256);
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    sim::match_runtime::OfflineInputs input{
        map,
        loaded,
        types,
        fields,
        weapons,
        terrain_values,
        masks,
        8,
        8,
        2,
        2,
        0,
        30,
        1,
        &scenario,
        {},
        collision_plots,
        {}
    };
    input.unit_defs = records;
    // The profile's rules and each type's rule keys reach the match.
    input.rules.script_get.mounts[input.rules.script_get.count++] = {
        71, oa::data::match_rules::ScriptExtension::unit_my_id
    };
    input.rules.script_fidelity = oa::data::match_rules::ScriptFidelity::safe;
    std::array<oa::data::match_rules::UnitTypeRules, 2> unit_type_rules{};
    unit_type_rules[1].veterancy_thresholds = {7};
    input.unit_type_rules = unit_type_rules;
    sim::match_runtime::Match match(input, services);
    CHECK(match.rules() == input.rules);
    CHECK(match.rules_view().unit_type(1) == unit_type_rules[1]);
    CHECK(match.rules_view().unit_type(0) == oa::data::match_rules::UnitTypeRules{});
    CHECK(match.rules_view().unit_type(5) == oa::data::match_rules::UnitTypeRules{});
    const UnitDef& def = match.state().unit_defs[1];
    CHECK(std::strcmp(match.state().unit_defs[0].unit_name, "None") == 0);
    CHECK(
        std::strcmp(def.unit_name, "ARMSOLAR") == 0 && def.type_id == 1 &&
        def.flags == records[1].flags
    );
    CHECK(def.weapon1 == 1 && def.yard_map == 7 && def.build_ids == 3 && def.gui_page_count == 2);
    CHECK(def.damage_modifier == 21845 && def.footprint_x == 5);
    CHECK(def.primary_bad_target_category == 1 && def.no_chase_category == 4);
    UnitDef reloaded = records[1];
    reloaded.damage_modifier = 0x10000;
    reloaded.primary_bad_target_category = 77;
    match.replace_unit_def(1, reloaded);
    CHECK(def.damage_modifier == 0x10000 && def.primary_bad_target_category == 1);
    std::array<UnitDef, 1> short_table{};
    input.unit_defs = short_table;
    CHECK(sim::match_runtime::Match::input_error(input) != nullptr);
    sim::match_runtime::Match mismatched(input, services);
    CHECK(mismatched.fault() != nullptr);
}

/// Builds a match over `weapons` and returns it. The registry must outlive the match.
std::unique_ptr<sim::match_runtime::Match>
match_over(const sim::combat_state::WeaponRegistry& weapons) {
    formats::tnt::Map map;
    map.attribute_width = map.attribute_height = 16;
    map.attributes.resize(256);
    std::vector<sim::visibility_state::TerrainCell> terrain_values(256);
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.pixels = {1};
    const std::array masks{mask};
    std::array<sim::unit_spawn::LoadedType, 2> loaded;
    std::array<sim::unit_spawn::Type, 2> types;
    std::array<sim::match_runtime::RuntimeTypeFields, 2> fields{};
    std::array<UnitDef, 2> records{};
    oa::base::text::copy_terminated(records[1].unit_name, "ARMSOLAR");
    records[1].type_id = 1;
    std::vector<sim::spatial_state::Plot> collision_plots(256);
    Services services;
    Scenario scenario;
    sim::match_runtime::OfflineInputs input{
        map,
        loaded,
        types,
        fields,
        weapons,
        terrain_values,
        masks,
        8,
        8,
        2,
        2,
        0,
        30,
        1,
        &scenario,
        {},
        collision_plots,
        {}
    };
    input.unit_defs = records;
    return std::make_unique<sim::match_runtime::Match>(input, services);
}

// Slots from 256 live beside Game.weapon_defs. A registry of 256 leaves that
// table null. W300 at a raised limit is slot 300, reference 301.
void weapon_records_past_255_fill_the_extra_table() {
    sim::combat_state::WeaponRegistry plain;
    const auto stock = match_over(plain);
    CHECK(stock->fault() == nullptr);
    CHECK(stock->state().extra_weapon_defs == nullptr);
    CHECK(stock->state().extra_weapon_def_count == 0);
    CHECK(oa::world_weapon_def_count(&stock->state()) == OA_WEAPON_DEF_COUNT);

    sim::combat_state::WeaponRegistry wide;
    CHECK(sim::combat_state::install_weapon_text(wide, "[W300]{id=300;}", false, 1024) == 1);
    const auto match = match_over(wide);
    CHECK(match->fault() == nullptr);
    const oa::World& state = match->state();
    CHECK(state.extra_weapon_def_count == 45);
    CHECK(
        std::memcmp(
            state.game.weapon_defs,
            wide.records().data(),
            sizeof(oa::WeaponDef) * OA_WEAPON_DEF_COUNT
        ) == 0
    );
    const oa::WeaponDef* weapon = oa::world_weapon_def(&state, 301);
    CHECK(weapon != nullptr && std::strcmp(weapon->key, "W300") == 0);
    CHECK(oa::oa_weapon_def_id(weapon) == 300);
    CHECK(oa::world_weapon_ref(&state, weapon) == 301);
}

// The capacities the match sizes from OfflineInputs::limits: the effect layers
// and pool, the path search's credit and, from the per-player limit, the unit
// slots. 3.1c's by default; a mod's limits raise them, up to the largest a
// profile may name.
void limits_size_the_match() {
    formats::tnt::Map map;
    map.attribute_width = map.attribute_height = 16;
    map.attributes.resize(256);
    std::vector<sim::visibility_state::TerrainCell> terrain_values(256);
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.pixels = {1};
    const std::array masks{mask};
    std::array<sim::unit_spawn::LoadedType, 2> loaded;
    std::array<sim::unit_spawn::Type, 2> types;
    std::array<sim::match_runtime::RuntimeTypeFields, 2> fields{};
    std::vector<sim::spatial_state::Plot> collision_plots(256);
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    const auto built = [&](uint16_t per_player_limit, const data::limits::Limits& limits) {
        sim::match_runtime::OfflineInputs input{
            map,
            loaded,
            types,
            fields,
            weapons,
            terrain_values,
            masks,
            8,
            8,
            per_player_limit,
            2,
            0,
            30,
            1,
            &scenario,
            {},
            collision_plots,
            {}
        };
        input.limits = limits;
        return std::make_unique<sim::match_runtime::Match>(input, services);
    };

    const auto base = built(250, {});
    CHECK(base->fault() == nullptr);
    CHECK(base->effects().evict_above == 400 && base->effects().pool_capacity == 1000);
    CHECK(base->effects().layer_slots == 401);
    CHECK(base->path_search_jobs().tick_credit == 1333);
    CHECK(base->state().game.units_per_player == 250);
    CHECK(base->state().unit_slot_count == 2501);

    data::limits::Limits raised;
    raised.effects = {20480, 204800};
    raised.path_search.nodes = 66650;
    raised.units_per_player = {1500, 20, 1500, 1500};
    const auto mod = built(1500, raised);
    CHECK(mod->fault() == nullptr);
    CHECK(mod->effects().evict_above == 20480 && mod->effects().pool_capacity == 204800);
    CHECK(mod->effects().layer_slots == 20481);
    CHECK(mod->path_search_jobs().tick_credit == 66650);
    CHECK(mod->state().unit_slot_count == 15001);
    CHECK(mod->limits().path_search.nodes == 66650);

    // The largest values a profile may name; the pool is kept small here, so
    // that the rings, which never hold more than it, stay small too.
    data::limits::Limits largest;
    largest.effects = {data::limits::highest_effect_queue, 4096};
    largest.path_search.nodes = data::limits::highest_path_search_nodes;
    const auto widest = built(data::limits::highest_units_per_player, largest);
    CHECK(widest->fault() == nullptr);
    CHECK(widest->effects().layer_slots == 4097);
    CHECK(widest->path_search_jobs().tick_credit == data::limits::highest_path_search_nodes);
    CHECK(widest->state().unit_slot_count == 65531);
}

int main() {
    loader_records_become_the_world_table();
    weapon_records_past_255_fill_the_extra_table();
    limits_size_the_match();
    formats::tnt::Map map;
    map.attribute_width = map.attribute_height = 16;
    map.attributes.resize(256);
    std::vector<sim::visibility_state::TerrainCell> terrain_values(256);
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.transparent = 0;
    mask.pixels = {1};
    const std::array masks{mask};
    auto model = std::make_shared<formats::objects3d::Model>();
    model->objects.resize(1);
    model->objects[0].name = "root";
    auto script = std::make_shared<formats::cob::CobProgram>();
    using namespace sim::script_vm;
    script->code = {
        opcode::hide,
        0,
        opcode::push_constant,
        1000,
        opcode::sleep,
        opcode::show,
        0,
        opcode::return_,
        opcode::push_constant,
        1,
        opcode::pop_static,
        0,
        opcode::return_,
        opcode::push_constant,
        1,
        opcode::push_constant,
        1,
        opcode::set_unit_value,
        opcode::return_
    };
    script->scripts = {{"Create", 0}, {"Activate", 8}, {"Enable", 13}};
    script->entry_points = {0, 8, 13};
    script->header.static_variable_count = 1;
    script->piece_names = {"root"};
    std::array<sim::unit_spawn::LoadedType, 2> loaded;
    loaded[1].model = model;
    loaded[1].script = script;
    std::array<sim::unit_spawn::Type, 2> types;
    types[1].simulation.flags = 0x800000;
    types[1].simulation.maximum_health = 100;
    types[1].footprint_x = types[1].footprint_z = 1;
    types[1].model = reinterpret_cast<uintptr_t>(model.get());
    types[1].cob = reinterpret_cast<uintptr_t>(script.get());
    loaded[1].type = types[1];
    data::unit_definitions::UnitDefinition def;
    def.sight_distance = 160;
    def.acceleration_fixed = 65536;
    def.brake_rate_fixed = 65536;
    def.max_velocity_fixed = 2 * 65536;
    def.turn_rate = 1024;
    // The economy tick recomputes the player capacities from finished-unit storage
    // every tick; the commander's 1000/1000 keeps the stores bounded here.
    def.energy_storage = 1000.0F;
    def.metal_storage = 1000.0F;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::vector<sim::spatial_state::Plot> collision_plots(256);
    std::array<sim::match_runtime::RuntimeTypeFields, 2> fields{};
    fields[1].definition = &def;
    const std::array<uint8_t, 1> yard{4};
    fields[1].yard_mask = yard;
    fields[1].runtime_metadata = &metadata;
    fields[1].target_masks = &target_masks;
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    sim::match_runtime::OfflineInputs input{
        map,
        loaded,
        types,
        fields,
        weapons,
        terrain_values,
        masks,
        8,
        8,
        2,
        2,
        0,
        30,
        1,
        &scenario,
        {},
        collision_plots,
        {}
    };
    sim::match_runtime::Match match(input, services);
    match.configure_strategic_environment({0, 0.5f, 0});
    match.simulation().players[0].present = true;
    match.simulation().players[0].status = 1;
    auto* slot = match.create({0, 1, {64u << 16, 0, 64u << 16}, true, 1, 0});
    CHECK(slot && slot->unit_index == 1 && slot->unit->health == 100);
    CHECK((services.calls == std::vector<std::string>{"masked", "footprint"}));
    auto* instance = match.instance(1);
    CHECK(instance && !(instance->model().pieces()[0].flags & 1));
    CHECK(instance->piece_world(0xffffffffu) == slot->unit->position);
    CHECK(instance->piece_world(1) == slot->unit->position);
    std::size_t occupied = 0;
    for (const auto& plot : match.spatial().plots)
        occupied += plot.ground == 1;
    CHECK(occupied == 1);
    CHECK(
        match.world().players[0].current_count == 1 && match.world().players[0].total_created == 1
    );
    match.tick_scripts(30);
    CHECK(instance->model().pieces()[0].flags & 1);
    CHECK(instance->script()->vm().active_count() == 0);
    CHECK(instance->script()->call_no_arguments("Enable", true));
    CHECK((slot->record.state_flags & 1) && instance->script()->vm().static_value(0) == 1);
    CHECK(services.calls[2] == "sound" && services.calls[3] == "refresh");
    types[1].bm_code = 1;
    fields[1].movement_class = 0;
    match.reload_unit_defs();
    services.expected_id = 2;
    const std::array<sim::unit_spawn::StartMarker, 1> markers{{{1, 0, 96, 96}}};
    Start start;
    const auto started = match.start_player(0, {1, 0, 0, 100}, markers, 0, 256, 128, start);
    CHECK(
        started.position_found && match.state().game.players[0].shared_energy_storage == 200 &&
        match.state().game.players[0].shared_metal_storage == 200
    );
    // The start then seeds the stores from the same slot, unfloored.
    match.state().game.players[0].energy = 100.0F;
    match.state().game.players[0].metal = 0.0F;
    auto* moving = started.unit ? &match.world().slots[started.unit->id] : nullptr;
    CHECK(
        moving && moving->unit_index == 2 && moving->movement_object && moving->unit->object_present
    );
    const auto* ground =
        reinterpret_cast<const sim::ground_orders::GroundRuntime*>(moving->movement_object);
    CHECK(
        ground->movement.speed == 0 && ground->movement.flags == 1 && ground->navigation.flags == 8
    );
    CHECK(ground->geometry.position[0] == 96 * 65536);
    moving->unit->flags |= OA_UNIT_FLAG_SELECTABLE;
    const auto saved_unfinished = moving->record.build_remaining;
    moving->record.build_remaining = std::bit_cast<float>(uint32_t{0x7fc00000});
    CHECK(match.selectable(2));
    moving->record.build_remaining = saved_unfinished;
    // Only the mobile unit participates in this bounded tick fixture.
    slot->unit->record.type_index = 0;
    types[1].simulation.default_mission_type = sim::ground_orders::standby_kind;
    match.reload_unit_defs();
    moving->unit->flags = (moving->unit->flags & ~OA_UNIT_FLAG_FIRE_ORDER_MASK) |
                          (2u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    match.configure_outcomes(0, allies, true);
    const std::array<uint32_t, 3> position_before_ticks = moving->unit->position;
    for (uint32_t tick = 0; tick < 35; ++tick) {
        match.simulation().tick = tick;
        match.tick();
    }
    CHECK(match.simulation().active_units == 1);

    CHECK(moving->unit->health_percent == 100 && moving->unit->position == position_before_ticks);
    CHECK(moving->unit->primary && moving->unit->primary->kind == sim::ground_orders::standby_kind);
    CHECK(match.instance(2)->script()->vm().active_count() == 0);
    CHECK(match.instance(2)->model().pieces()[0].flags & 1);
    // A sound callback may mutate live unit and weapon state. Mission dispatch
    // must not write its earlier geometry/weapon projection over those changes.
    services.command = [&](sim::unit_spawn::Slot& subject, uint32_t category) {
        CHECK(category == 6);
        subject.unit->flags |= OA_UNIT_FLAG_ATTACHED_WITHOUT_PIECE;
        subject.record.weapons[0].flags |= weapon_bit_set_by_sound;
    };
    auto& completed = match.insert_ground_order(2, sim::ground_orders::move_ground_kind);
    completed.phase = 1;
    completed.wait_events = sim::ground_orders::arrived_event;
    completed.raised_events = sim::ground_orders::arrived_event;
    match.simulation().tick = 35;
    match.tick();
    CHECK(
        (moving->unit->flags & OA_UNIT_FLAG_ATTACHED_WITHOUT_PIECE) &&
        (match.state().units[2].weapons[0].flags & weapon_bit_set_by_sound)
    );
    moving->unit->flags |= OA_UNIT_FLAG_ATTACHED_WITHOUT_PIECE;
    match.state().units[2].attach_first_child = oa::oa_unit_ref_from_slot(1);
    match.prepare_spatial_state();
    match.synchronize_spatial_state();
    CHECK(
        (moving->unit->flags & OA_UNIT_FLAG_ATTACHED_WITHOUT_PIECE) &&
        match.spatial().units[2].first_attachment == 1
    );
    // The local slot's one deadline (Player.next_economy_tick) carries both
    // the outcome checks and the economy: due at 0 and 30, it now waits for 60.
    CHECK(
        match.outcome_state().countdown == 3 &&
        match.state().game.players[0].next_economy_tick == 60
    );
    // A live unallied opponent interrupts victory without resetting countdown.
    match.world().players[1].current_count = 1;
    match.simulation().tick = 60;
    match.tick();
    CHECK(match.outcome_state().countdown == 3);
    match.world().players[1].current_count = 0;
    for (auto tick : {90u, 120u, 150u, 180u}) {
        match.simulation().tick = tick;
        match.tick();
    }
    CHECK(match.outcome() == sim::scenario::Outcome::victory);
    // A destination already inside the current footprint installs and completes
    // the plain navigation goal without fabricating a search result.
    const sim::ground_orders::Point here{96 * 65536, 0, 96 * 65536};
    auto* reached = &match.insert_ground_order(2, sim::ground_orders::move_ground_kind, here, 0);
    match.simulation().tick = 181;
    match.tick();
    CHECK(ground->navigation.goal == nullptr);
    match.simulation().tick = 182;
    match.tick();
    CHECK(moving->unit->primary != reached);
    const std::array<uint32_t, 3> before_move = moving->unit->position;
    const sim::ground_orders::Point destination{96 * 65536, 0, 160 * 65536};
    (void)match.insert_ground_order(2, sim::ground_orders::move_ground_kind, destination, 0);
    for (uint32_t tick = 183; tick < 243; ++tick) {
        match.simulation().tick = tick;
        match.tick();
    }
    CHECK(moving->unit->position != before_move);
    CHECK(match.ground_runtime(2)->geometry.cell[1] != 5);
    CHECK(match.spatial().units[2].cell == match.ground_runtime(2)->geometry.cell);

    CHECK((match.outcome_state().flags & sim::scenario::outcome_flag::finished) != 0);
    // Natural recovery consumes the debit accumulator in Unit.economy and only
    // changes health when the resource gate accepts the request.
    def.build_time = 100;
    def.build_cost_energy = 1000;
    types[1].simulation.heal_time = 30;
    match.reload_unit_defs();
    moving->unit->health = 90;
    // The game is decided, so the tick settles no economy; the
    // settlement follows the tick's debit directly.
    auto& owner = match.state().game.players[match.state().units[2].owner_index];
    owner.next_economy_tick = 248;
    match.simulation().tick = 248;
    match.tick();
    match.update_player_economy(owner.index);
    CHECK(moving->unit->health == 91);
    auto& economy = economy_words(match.state().units[2]);
    // The block scaling runs after the debit: requested moves to word 5 and the live
    // requested/accepted words are cleared. A covered player store clears gate.
    CHECK(std::bit_cast<float>(economy[1]) == 0.0F && std::bit_cast<float>(economy[2]) == 0.0F);
    CHECK(std::bit_cast<float>(economy[5]) == 1.0F && std::bit_cast<float>(economy[3]) == 0.0F);
    economy[3] = std::bit_cast<uint32_t>(1.0F);
    owner.next_economy_tick = 256;
    match.simulation().tick = 256;
    match.tick();
    match.update_player_economy(owner.index);
    CHECK(moving->unit->health == 91);
    CHECK(std::bit_cast<float>(economy[1]) == 0.0F && std::bit_cast<float>(economy[3]) == 0.0F);
    // Ordinary click replaces non-preserved orders; shift appends after the
    // queue-tail marker, while preserved primary orders survive.
    auto& first = match.issue_ground_move(2, here, false);
    CHECK(moving->unit->primary == &first && first.next == nullptr);
    auto& queued = match.issue_ground_move(2, destination, true);
    CHECK(moving->unit->primary == &first && first.next == &queued && queued.next == nullptr);
    auto& queued2 = match.issue_ground_move(2, here, true);
    CHECK(
        moving->unit->primary == &first && first.next == &queued && queued.next == &queued2 &&
        queued2.next == nullptr
    );
    queued2.next = &first;
    std::size_t walked = 0;
    match.visit_primary_queue(2, [&](const auto&) { ++walked; });
    CHECK(walked > 0 && walked <= 256);
    auto& queued3 = match.issue_ground_move(2, destination, true);
    walked = 0;
    match.visit_primary_queue(2, [&](const auto&) { ++walked; });
    CHECK(walked > 0 && walked <= 256 && queued3.next != &queued3);
    CHECK(queued3.next == nullptr);
    const sim::ground_orders::Point site_a{80 * 65536, 0, 80 * 65536};
    const sim::ground_orders::Point site_b{112 * 65536, 0, 112 * 65536};
    def.builder = 1;
    def.build_distance = 128;
    auto& build1 = match.issue_mobile_build(2, 1, site_a, false);
    auto& build2 = match.issue_mobile_build(2, 1, site_b, true);
    CHECK(moving->unit->primary == &build1 && build1.next == &build2 && build2.next == nullptr);
    for (uint32_t tick = 257; tick < 280; ++tick) {
        match.simulation().tick = tick;
        match.tick();
    }
    CHECK(moving->unit->primary != nullptr);
    auto& preserved = match.insert_ground_order(2, sim::ground_orders::standby_kind);
    preserved.preserve_flags |= 4;
    auto& replacement = match.issue_ground_move(2, here, false);
    CHECK(
        moving->unit->primary == &preserved && preserved.next == &replacement &&
        replacement.next == nullptr
    );
    CHECK(replacement.preserve_flags == 3);
    const auto local_coverage = match.sight().coverage;
    match.simulation().players[1].present = true;
    match.simulation().players[1].status = 2;
    services.expected_id = 3;
    auto* opponent = match.create({1, 1, {192u << 16, 0, 192u << 16}, true, 1, 0});
    CHECK(opponent && opponent->unit_index == 3);
    CHECK(match.sight().coverage == local_coverage);
    bool enemy_has_coverage = false;
    for (auto count : match.player_coverage(1))
        enemy_has_coverage |= count != 0;
    CHECK(enemy_has_coverage);
    CHECK(match.point_visible(1, opponent->unit->position));
    CHECK(!match.point_visible(0, opponent->unit->position));
    CHECK(match.unit_visible(1, 3));
    CHECK(!match.unit_visible(0, 3));
    opponent->unit->position = moving->unit->position;
    CHECK(match.unit_visible(0, 3));
    opponent->record.state_flags |= 4;
    CHECK(match.unit_visible(1, 3));
    CHECK(!match.unit_visible(0, 3));
    CHECK(!match.point_visible(1, {0xffff0000u, 0, 0}));
    // Attack order ownership: actual resolver, target observer insertion and
    // ordinary replacement destruction must compose without dangling links.
    def.can_attack = true;
    def.can_move = true;
    moving->unit->flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    opponent->unit->flags |= OA_UNIT_FLAG_LIVE;
    CHECK(match.issue_attack(2, 3, true));
    CHECK(moving->unit->primary->kind == 6);
    auto* attacked = moving->unit->primary;
    auto& after_attack = match.issue_ground_move(2, here, false);
    CHECK(moving->unit->primary != attacked);
    CHECK(after_attack.kind == sim::ground_orders::move_ground_kind);
    // Capture: a copy for the capturer at the captured unit's pose with its
    // health, build_remaining and heading, then 30000 of kind 4 on the
    // captured unit from no source.
    match.simulation().players[0].present = true;
    match.simulation().players[0].status = 1;
    match.simulation().players[1].present = true;
    match.simulation().players[1].status = 1;
    opponent->record.state_flags = 0;
    opponent->unit->health = 77;
    opponent->record.build_remaining = std::bit_cast<float>(uint32_t{0x3f000000});
    opponent->record.bank = 0x11;
    opponent->yaw = 0x1234;
    opponent->record.pitch = 0x22;
    const uint16_t count0 = match.world().players[0].current_count;
    services.command = [](sim::unit_spawn::Slot&, uint32_t) {};
    services.strict_activation = false;
    match.capture_unit(*opponent, *moving);
    sim::unit_spawn::Slot* copy = nullptr;
    for (auto& candidate : match.world().slots) {
        if (candidate.unit_index != 0 && candidate.unit && candidate.unit->record.type_index &&
            candidate.record.owner_index == 0 && candidate.unit_index != moving->unit_index &&
            candidate.unit_index != opponent->unit_index)
            copy = &candidate;
    }
    CHECK(copy && copy->unit);
    CHECK(copy->unit->health == 77);
    CHECK(std::bit_cast<uint32_t>(copy->record.build_remaining) == 0x3f000000);
    CHECK(copy->record.bank == 0x11 && copy->yaw == 0x1234 && copy->record.pitch == 0x22);
    CHECK(copy->record.owner_index == 0);
    CHECK((opponent->unit->flags & OA_UNIT_FLAG_DEATH_PENDING) != 0);
    CHECK(match.world().players[0].current_count == count0 + 1);
    services.strict_activation = true;
    services.command = {};
    // MakesMetal through the metal credit. An active unfinished building
    // credits the unsigned byte into the metal produced word when the energy
    // gate is open; a closed gate, negative EnergyUse, a clear activate bit,
    // or a missing building flag credits nothing. Easy computer (difficulty 0)
    // scales the credit to 0.5.
    {
        const auto saved_makes = def.makes_metal;
        const auto saved_use = def.energy_use;
        const auto saved_extracts = def.extracts_metal;
        const auto saved_flags = opponent->unit->flags;
        const auto saved_state_flags = opponent->record.state_flags;
        const auto saved_build_remaining = opponent->record.build_remaining;
        const auto saved_status = match.simulation().players[1].status;
        const auto saved_difficulty = match.difficulty();
        auto& words = economy_words(opponent->record);
        const auto saved_words = words;
        def.makes_metal = 30;
        def.energy_use = 0;
        def.extracts_metal = 0;
        opponent->unit->flags = saved_flags | OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_LIVE;
        opponent->record.state_flags = static_cast<uint8_t>(saved_state_flags | 1u);
        opponent->record.build_remaining = std::bit_cast<float>(uint32_t{0x3f800000});
        match.simulation().players[1].status = 1;
        match.set_difficulty(1);
        words.fill(0);
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 30.0F);
        CHECK(std::bit_cast<float>(words[10]) == 30.0F && std::bit_cast<float>(words[6]) == 0.0F);
        words.fill(0);
        words[3] = std::bit_cast<uint32_t>(1.0F);
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 0.0F);
        CHECK(std::bit_cast<float>(words[10]) == 0.0F && std::bit_cast<float>(words[3]) == 1.0F);
        words.fill(0);
        def.energy_use = -20.0F;
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 0.0F);
        CHECK(std::bit_cast<float>(words[4]) == 20.0F);
        def.energy_use = 0;
        words.fill(0);
        opponent->record.state_flags = static_cast<uint8_t>(opponent->record.state_flags & ~1u);
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 0.0F);
        CHECK(std::bit_cast<float>(words[10]) == 0.0F);
        opponent->record.state_flags = static_cast<uint8_t>(opponent->record.state_flags | 1u);
        words.fill(0);
        opponent->unit->flags &= ~OA_UNIT_FLAG_BUILDING;
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 0.0F);
        CHECK(std::bit_cast<float>(words[10]) == 0.0F);
        opponent->unit->flags |= OA_UNIT_FLAG_BUILDING;
        match.simulation().players[1].status = 2;
        match.set_difficulty(0);
        words.fill(0);
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 15.0F);
        CHECK(std::bit_cast<float>(words[10]) == 15.0F);
        def.makes_metal = saved_makes;
        def.energy_use = saved_use;
        def.extracts_metal = saved_extracts;
        opponent->unit->flags = saved_flags;
        opponent->record.state_flags = saved_state_flags;
        opponent->record.build_remaining = saved_build_remaining;
        match.simulation().players[1].status = saved_status;
        match.set_difficulty(saved_difficulty);
        match.reload_unit_defs();
        words = saved_words;
    }
    // ExtractsMetal through the metal credit, before the Unit.build_remaining
    // finished test. The extraction rate credits the metal produced word while
    // the energy gate is open, even while the unit is unfinished.
    {
        const auto saved_extracts = def.extracts_metal;
        const auto saved_use = def.energy_use;
        const auto saved_flags = opponent->unit->flags;
        const auto saved_state_flags = opponent->record.state_flags;
        const auto saved_build_remaining = opponent->record.build_remaining;
        const auto saved_status = match.simulation().players[1].status;
        const auto saved_difficulty = match.difficulty();
        const auto saved_speed = opponent->record.extracted_metal;
        auto& words = economy_words(opponent->record);
        const auto saved_words = words;
        def.extracts_metal = 1.0F;
        def.energy_use = 0;
        opponent->unit->flags = saved_flags | OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_LIVE;
        opponent->record.state_flags = static_cast<uint8_t>(saved_state_flags | 1u);
        opponent->record.build_remaining = std::bit_cast<float>(uint32_t{0});
        opponent->record.extracted_metal = 30.0F;
        match.simulation().players[1].status = 1;
        match.set_difficulty(1);
        words.fill(0);
        words[3] = std::bit_cast<uint32_t>(1.0F);
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 0.0F);
        words.fill(0);
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 30.0F);
        CHECK(std::bit_cast<float>(words[10]) == 30.0F);
        words.fill(0);
        def.energy_use = -20.0F;
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 0.0F);
        def.energy_use = 0;
        words.fill(0);
        opponent->record.build_remaining = 1.0F;
        match.reload_unit_defs();

        match.update_player_economy(1);
        CHECK(match.world().players[1].metal_produced == 30.0F);
        def.extracts_metal = saved_extracts;
        def.energy_use = saved_use;
        opponent->unit->flags = saved_flags;
        opponent->record.state_flags = saved_state_flags;
        opponent->record.build_remaining = saved_build_remaining;
        match.simulation().players[1].status = saved_status;
        match.set_difficulty(saved_difficulty);
        match.reload_unit_defs();
        opponent->record.extracted_metal = saved_speed;
        words = saved_words;
    }
    match.simulation().players[1].present = false;
    for (uint32_t tick = 257; tick < 1500; ++tick) {
        match.simulation().tick = tick;
        match.tick();
    }
    CHECK(match.strategic_state(0).classifications.size() == types.size());
    CHECK(match.strategic_state(0).owned_counts[1] == 1);
    std::cout << "offline spawn/model/script/spatial composition passed\n";
}
