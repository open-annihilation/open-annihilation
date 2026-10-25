// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A two-player match on a small flat map with one armed mobile type, shared
// by the damage-path and commander-rule tests.
#pragma once

#include "oa/sim/match_runtime.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "oa/test/match_services.hpp"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__));    \
    } while (false)

namespace combat_fixture {

using namespace oa;

inline constexpr uint8_t weapon_hit = 1;
inline constexpr uint8_t paralyzer_hit = 2;
inline constexpr uint8_t paralyze_order = 27;
inline constexpr uint8_t attack_chase_order = 6;
inline constexpr uint32_t attack_notice = 2;
inline constexpr const char* unit_name = "TESTUNIT";

// One EXPLODE a unit's script ran: the unit, the piece and the flags, and
// whether the unit still hung on a carrier or carried a unit when it ran.
struct PieceExplosion {
    uint16_t unit{};
    uint32_t piece{};
    int32_t flags{};
    bool carried{};
    bool carrying{};
};

struct Services : oa::test::QuietServices {
    uint32_t notices{};
    // Values of the attachment notifications raised (0x10000 on cloaking).
    std::vector<uint32_t> attachment_notices;
    // Records the EXPLODEs of Options::killed_script; without it one is
    // unexpected.
    bool record_explosions{};
    std::vector<PieceExplosion> explosions;

    /// Counts the attack notices among the command sounds.
    ///
    /// @param category speech category
    void command_sound(sim::unit_spawn::Slot&, uint32_t category) override {
        if (category == attack_notice)
            ++notices;
    }

    /// Records an attachment notification.
    ///
    /// @param value the notified value
    void attachment_notification(sim::unit_spawn::Slot&, uint32_t value) override {
        attachment_notices.push_back(value);
    }

    /// Records an EXPLODE of Options::killed_script; any other throws.
    ///
    /// @param slot Unit whose script ran it.
    /// @param piece Exploding piece.
    /// @param flags The EXPLODE's flags.
    void explode_piece(sim::unit_spawn::Slot& slot, uint32_t piece, int32_t flags) override {
        if (!record_explosions)
            throw std::runtime_error("unexpected explode_piece");
        explosions.push_back(
            {slot.unit_index,
             piece,
             flags,
             slot.record.attach_parent != 0,
             slot.record.attach_first_child != 0}
        );
    }

    /// Fails the test: no fixture script attaches a unit.
    void attach_unit(sim::unit_spawn::Slot&, int32_t, int32_t, int32_t) override {
        oa::test::unexpected_call("attach_unit");
    }

    /// Fails the test: no fixture script drops a unit.
    void drop_unit(sim::unit_spawn::Slot&, int32_t) override {
        oa::test::unexpected_call("drop_unit");
    }
};

struct Scenario : sim::scenario::DefinitionHost {
    int32_t lava_world{};
    int32_t water_does_damage{};
    int32_t water_damage{};
    std::optional<std::string> unit_type_killed;

    /// Reads the GlobalHeader integers the fixture sets: lavaworld, waterdoesdamage
    /// and waterdamage.
    ///
    /// @param key key name
    /// @param fallback value for any other key
    /// @return the value
    int32_t integer(std::string_view key, int32_t fallback) override {
        if (key == "waterdoesdamage")
            return water_does_damage;
        if (key == "waterdamage")
            return water_damage;
        return key == "lavaworld" ? lava_world : fallback;
    }

    /// Reads the GlobalHeader text the fixture sets: UnitTypeKilled.
    ///
    /// @param key key name
    /// @return the text, or nothing for any other key
    std::optional<std::string> text(std::string_view key) override {
        return key == "UnitTypeKilled" ? unit_type_killed : std::nullopt;
    }
};

struct Options {
    bool defeat_allowed{};
    bool multiplayer{};
    int32_t lava_world{};
    // Plot height of the cells from this column east, over sea level 30; the
    // rest of the map stays at height 0.
    std::optional<uint32_t> high_ground_from{};
    // Edge of the square sight mask in 32-unit sight cells, centred on the
    // unit; 1 covers only the unit's own cell.
    uint32_t sight_cells = 1;
    // Explosion art by (archive, entry); none leaves the blasts without sprites.
    std::function<const formats::gaf::Sequence*(std::string_view, std::string_view)>
        effect_sequence{};
    // The FeatureDef table, the loader's resolved plot words by plot index,
    // and the frames of the table's sequences.
    std::vector<FeatureDef> features{};
    std::vector<std::pair<std::size_t, uint16_t>> feature_words{};
    std::function<bool(oa_ref32, uint16_t, sim::feature_runtime::FeatureSequenceFrame&)>
        feature_sequence_frame{};
    // The mission schema's feature placements, over the same table.
    std::vector<sim::feature_runtime::FeaturePlacement> mission_features{};
    // A savegame resumes: the loader's features and the placements are left
    // for its Features section.
    bool resuming_saved_game{};
    // GlobalHeader waterdoesdamage / waterdamage.
    int32_t water_does_damage{};
    int32_t water_damage{};
    // GlobalHeader DefeatCondition_UnitTypeKilled text ("NAME,COUNT").
    std::optional<std::string> unit_type_killed{};
    // Arms the type's third slot with a 120-unit commandfire turret gun of
    // 400 energy a shot (registry index 2); AimTertiary accepts at once and
    // FireTertiary counts shots in static 2.
    bool dgun{};
    // TDF reloadtime of the turret gun, seconds.
    std::string gun_reload_time = "0.1";
    // Gives the type a Killed(severity, corpsetype) script with a solar
    // collector's thresholds: corpsetype 1 up to severity 25, 2 up to 50 and
    // 3 above. It explodes the root piece once, with the corpsetype as the
    // flags, and the services record each EXPLODE.
    bool killed_script{};
    // The rules and capacities the match plays by (OfflineInputs::rules,
    // limits, unit_type_rules, weapon_rules, profile_sim_hash, mutator_digest);
    // 3.1c's and no profile by default.
    data::match_rules::MatchRules rules{};
    data::limits::Limits limits{};
    std::vector<data::match_rules::UnitTypeRules> unit_type_rules{};
    std::vector<data::match_rules::WeaponTypeRules> weapon_rules{};
    std::optional<base::sha256::Digest> profile_sim_hash{};
    std::optional<base::sha256::Digest> mutator_digest{};
    // The map's wind limits (OfflineInputs::minimum_wind, maximum_wind).
    int32_t minimum_wind{};
    int32_t maximum_wind{};
    // Unit slots of each player (OfflineInputs::per_player_limit).
    uint16_t per_player_limit = 4;
    // The type's maximum health (MaxDamage); 0 makes a type with none.
    uint32_t maximum_health = 1000;
    // Weapon slots the registry accepts, from 0. 256 is 3.1c's table.
    uint32_t weapon_types = 256;
    // Id of the fixture's TESTGUN. 1 is today's gun.
    uint16_t gun_id = 1;
};

/// Returns the fixture's TESTGUN section (slot 1): a 400-range turret gun
/// doing 10 damage.
///
/// @param reload_time its reloadtime text, in seconds
/// @param extra further keys, each with its ';', placed before the [DAMAGE] block
/// @param damage its [DAMAGE] default
/// @param id its weapon id
/// @return the weapon TDF text
inline std::string test_gun_tdf(
    std::string_view reload_time = "0.1",
    std::string_view extra = {},
    int damage = 10,
    uint16_t id = 1
) {
    return "[TESTGUN]{id=" + std::to_string(id) + "; reloadtime=" + std::string(reload_time) +
           "; range=400; lineofsight=1; weaponvelocity=100; turret=1; " + std::string(extra) +
           " [DAMAGE]{default=" + std::to_string(damage) + ";}}";
}

/// Returns the corpsetype Options::killed_script picks for a severity, which
/// is also the flags it explodes the root piece with.
///
/// @param severity Killed percentage.
/// @return 1 up to 25, 2 up to 50, 3 above.
inline int32_t killed_corpsetype(int32_t severity) {
    return severity <= 25 ? 1 : severity <= 50 ? 2 : 3;
}

// A 16x16-cell map (256 world units a side) with one mobile type carrying a
// 400-unit line-of-sight turret gun.
struct Fixture {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values =
        std::vector<sim::visibility_state::TerrainCell>(256);
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::vector<sim::visibility_state::AltitudeCell> altitude_cells =
        std::vector<sim::visibility_state::AltitudeCell>(64);
    std::vector<sim::visibility_state::AltitudeSightPattern> altitude_patterns =
        std::vector<sim::visibility_state::AltitudeSightPattern>(6);
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    std::array<sim::unit_spawn::LoadedType, 2> loaded;
    std::array<sim::unit_spawn::Type, 2> types;
    data::unit_definitions::UnitDefinition def;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::vector<sim::spatial_state::Plot> collision_plots =
        std::vector<sim::spatial_state::Plot>(256);
    std::array<sim::match_runtime::RuntimeTypeFields, 2> fields{};
    std::array<uint8_t, 1> yard{4};
    sim::combat_state::WeaponRegistry weapons;
    std::vector<FeatureDef> features;
    std::vector<sim::feature_runtime::FeaturePlacement> mission_features;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    /// Builds a 16 by 16 map, one armed unit type and a match over them, with
    /// players 0 and 1 each allied only with itself and player 0's outcomes
    /// configured.
    ///
    /// @param options the map, weapons, features and outcome settings
    explicit Fixture(const Options& options = {}) {
        map.attribute_width = map.attribute_height = 16;
        map.attributes.resize(256);
        if (options.high_ground_from) {
            constexpr uint8_t high = 60;
            map.sea_level = 30;
            for (uint32_t cell = 0; cell < 256; ++cell) {
                if (cell % 16 < *options.high_ground_from)
                    continue;
                map.attributes[cell].height = high;
                collision_plots[cell].low_height = collision_plots[cell].high_height = high;
            }
        }
        masks[0].width = masks[0].height = static_cast<uint16_t>(options.sight_cells);
        masks[0].offset_x = masks[0].offset_z = static_cast<int16_t>(options.sight_cells / 2);
        masks[0].pixels.assign(std::size_t{options.sight_cells} * options.sight_cells, 1);
        model->objects.resize(1);
        model->objects[0].name = "root";
        // Model top (UnitDef.model_height). The level shot leaves the root at the
        // shooter's height, and the projectile contact hits a ground occupant only below
        // its top, so a flat model lets every shot fly through its target.
        model->objects[0].vertices = {{0, 0, 0}, {0, 20 << 16, 0}};
        using namespace sim::script_vm;
        // AimPrimary accepts at once; FirePrimary counts shots in static 1.
        script->code = {
            opcode::return_,
            opcode::push_constant,
            1,
            opcode::pop_static,
            0,
            opcode::push_constant,
            1,
            opcode::return_,
            opcode::push_static,
            1,
            opcode::push_constant,
            1,
            opcode::add,
            opcode::pop_static,
            1,
            opcode::return_,
            opcode::return_,
        };
        script->scripts = {{"Create", 0}, {"AimPrimary", 1}, {"FirePrimary", 8}, {"RockUnit", 16}};
        script->entry_points = {0, 1, 8, 16};
        script->header.static_variable_count = 2;
        if (options.dgun) {
            script->code.insert(
                script->code.end(),
                {opcode::push_constant,
                 1,
                 opcode::return_,
                 opcode::push_static,
                 2,
                 opcode::push_constant,
                 1,
                 opcode::add,
                 opcode::pop_static,
                 2,
                 opcode::return_}
            );
            script->scripts.push_back({"AimTertiary", 17});
            script->scripts.push_back({"FireTertiary", 20});
            script->entry_points.insert(script->entry_points.end(), {17, 20});
            script->header.static_variable_count = 3;
        }
        if (options.killed_script) {
            // Local 1 takes the corpsetype for the severity in local 0, then
            // EXPLODE root with it as the flags.
            const auto killed = static_cast<uint32_t>(script->code.size());
            const uint32_t over_25 = killed + 11;
            const uint32_t over_50 = killed + 22;
            const uint32_t chosen = killed + 24;
            constexpr uint32_t root_piece = 0;
            script->code.insert(
                script->code.end(),
                {opcode::push_local,
                 0,
                 opcode::push_constant,
                 25,
                 opcode::less_equal,
                 opcode::jump_if_false,
                 over_25,
                 opcode::push_constant,
                 1,
                 opcode::jump,
                 chosen,
                 opcode::push_local,
                 0,
                 opcode::push_constant,
                 50,
                 opcode::less_equal,
                 opcode::jump_if_false,
                 over_50,
                 opcode::push_constant,
                 2,
                 opcode::jump,
                 chosen,
                 opcode::push_constant,
                 3,
                 opcode::pop_local,
                 1,
                 opcode::push_local,
                 1,
                 opcode::explode,
                 root_piece,
                 opcode::return_}
            );
            script->scripts.push_back({"Killed", killed});
            script->entry_points.push_back(killed);
            services.record_explosions = true;
        }
        script->piece_names = {"root"};
        types[1].simulation.flags = 0x800000 | OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        types[1].simulation.maximum_health = options.maximum_health;
        types[1].footprint_x = types[1].footprint_z = 1;
        types[1].bm_code = 1;
        types[1].model = reinterpret_cast<uintptr_t>(model.get());
        types[1].cob = reinterpret_cast<uintptr_t>(script.get());
        loaded[1].model = model;
        loaded[1].script = script;
        loaded[1].type = types[1];
        loaded[1].unit_name = unit_name;
        def.sight_distance = 160;
        def.weapon1 = "TESTGUN";
        def.can_move = def.can_attack = def.can_guard = def.can_patrol = true;
        def.acceleration_fixed = 65536;
        def.brake_rate_fixed = 65536;
        def.max_velocity_fixed = 2 * 65536;
        def.turn_rate = 1024;
        def.energy_storage = 1000.0F;
        def.metal_storage = 1000.0F;
        fields[1].definition = &def;
        fields[1].yard_mask = yard;
        fields[1].runtime_metadata = &metadata;
        fields[1].target_masks = &target_masks;
        fields[1].movement_class = 0;
        (void)sim::combat_state::install_weapon_text(
            weapons,
            test_gun_tdf(options.gun_reload_time, {}, 10, options.gun_id),
            false,
            options.weapon_types
        );
        if (options.dgun) {
            def.weapon3 = "TESTDGUN";
            def.can_dgun = true;
            (void)sim::combat_state::install_weapon_text(
                weapons,
                "[TESTDGUN]{id=2; reloadtime=1.2; range=120; lineofsight=1; weaponvelocity=200; "
                "turret=1; energypershot=400; commandfire=1; [DAMAGE]{default=10;}}"
            );
        }
        scenario.lava_world = options.lava_world;
        scenario.water_does_damage = options.water_does_damage;
        scenario.water_damage = options.water_damage;
        scenario.unit_type_killed = options.unit_type_killed;
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
            options.per_player_limit,
            2,
            0,
            30,
            1,
            &scenario,
            {},
            collision_plots,
            sim::visibility_state::AltitudeSightData{8, 8, altitude_cells, altitude_patterns}
        };
        input.effect_sequence = options.effect_sequence;
        features = options.features;
        for (const auto& [plot, word] : options.feature_words)
            collision_plots[plot].feature_word = word;
        input.feature_defs = features;
        input.feature_sequence_frame = options.feature_sequence_frame;
        mission_features = options.mission_features;
        input.mission_features = mission_features;
        input.resuming_saved_game = options.resuming_saved_game;
        input.limits = options.limits;
        input.rules = options.rules;
        input.unit_type_rules = options.unit_type_rules;
        input.weapon_rules = options.weapon_rules;
        input.profile_sim_hash = options.profile_sim_hash;
        input.mutator_digest = options.mutator_digest;
        input.minimum_wind = options.minimum_wind;
        input.maximum_wind = options.maximum_wind;
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        match->configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t player = 0; player < 2; ++player) {
            match->simulation().players[player].present = true;
            match->simulation().players[player].status = 1;
        }
        for (uint8_t player = 0; player < 2; ++player) {
            std::array<uint8_t, 10> allies{};
            allies[player] = 1;
            match->configure_player_alliances(player, allies);
            if (player == 0)
                match->configure_outcomes(
                    0, allies, options.defeat_allowed, false, options.multiplayer
                );
        }
    }

    /// Creates a finished unit that fires at will and may chase.
    ///
    /// @param player owning player
    /// @param x whole world units
    /// @param z whole world units
    /// @return the unit's slot
    sim::unit_spawn::Slot& spawn(uint8_t player, uint32_t x, uint32_t z) {
        auto* slot = match->create({player, 1, {x << 16, 32u << 16, z << 16}, true, 1, 0});
        CHECK(slot && slot->unit);
        slot->unit->flags =
            (slot->unit->flags & ~(OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK)) |
            (1u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT) | (2u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
        return *slot;
    }

    /// Runs full match ticks, checking after each that no fault was noted.
    ///
    /// @param ticks ticks to run
    void run(uint32_t ticks) {
        for (uint32_t i = 0; i < ticks; ++i) {
            ++match->simulation().tick;
            match->tick();
            CHECK(match->fault() == nullptr);
        }
    }

    /// Returns how many times a unit's script ran FirePrimary.
    ///
    /// @param slot the unit
    /// @return its script's static 1, or -1 when unset
    int32_t shots_from(const sim::unit_spawn::Slot& slot) {
        auto* instance = match->instance(slot.unit_index);
        CHECK(instance && instance->script());
        return instance->script()->vm().static_value(1).value_or(-1);
    }
};

/// Tells whether a unit's first primary order is of a kind.
///
/// @param slot the unit
/// @param kind mission kind
/// @return true when the queue's head has the kind
inline bool head_is(const sim::unit_spawn::Slot& slot, uint8_t kind) {
    return slot.unit->primary != nullptr && slot.unit->primary->kind == kind;
}

/// Kills a unit with a weapon death credited to the killer's owner, as the
/// kill handler counts it.
///
/// @param f the fixture
/// @param victim the dying unit
/// @param killer the unit credited
inline void kill(Fixture& f, sim::unit_spawn::Slot& victim, sim::unit_spawn::Slot& killer) {
    victim.unit->record.damage_kind = static_cast<uint8_t>(sim::match_runtime::DeathKind::weapon);
    victim.record.last_attacker_id = killer.unit_index;
    victim.record.last_attacker_owner = killer.record.owner_index;
    f.match->teardown_dead_unit(victim);
}

} // namespace combat_fixture
