// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pins each schema default to the value the unit, weapon and movement loaders store.
#include "oa/data/field_schema.hpp"

#include "oa/core/unit_def.h"
#include "oa/core/weapon_def.h"
#include "oa/data/defs/categories.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/move_classes.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/defs/weapons.hpp"
#include "oa/test/check.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using oa::MoveClass;
using oa::UnitDef;
using oa::WeaponDef;
using oa::data::field_schema::RecordKind;
using oa::data::field_schema::Value;
using oa::data::field_schema::default_value;
using oa::data::field_schema::fields;

struct MemoryFile {
    std::string path;
    std::string text;
};

struct MemoryFiles {
    std::vector<MemoryFile> files;

    const MemoryFile* find(const char* path) const {
        std::string key = path;
        for (char& character : key) {
            if (character == '/')
                character = '\\';
            else if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character + 32);
        }
        for (const MemoryFile& file : files) {
            std::string folded = file.path;
            for (char& character : folded) {
                if (character == '/')
                    character = '\\';
                else if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character + 32);
            }
            if (folded == key)
                return &file;
        }
        return nullptr;
    }

    oa::data::defs::Files services() {
        return oa::data::defs::Files{
            this,
            [](void* context, const char* path, uint8_t** data, uint32_t* size, bool* archived) {
                const MemoryFile* file = static_cast<MemoryFiles*>(context)->find(path);
                if (file == nullptr)
                    return false;
                *data = static_cast<uint8_t*>(std::malloc(file->text.size() + 1));
                std::memcpy(*data, file->text.data(), file->text.size());
                (*data)[file->text.size()] = 0;
                *size = static_cast<uint32_t>(file->text.size());
                *archived = false;
                return true;
            },
            [](void*, uint8_t* data) { std::free(data); },
            [](void* context, const char* path) {
                return static_cast<MemoryFiles*>(context)->find(path) != nullptr;
            },
            [](void*, const char*, const char*, void (*)(void*, const char*), void*) {},
        };
    }
};

struct Notes {
    const char* key{};
    const char* reason{};
};

// Text the loader turns into an index, or a number it does not store on the record.
constexpr Notes k_unmapped_unit[] = {
    {"category", "registered as type bits, not stored on the unit"},
    {"defaultmissiontype", "stored as a mission-type index, not the text"},
    {"soundcategory", "stored as a sound-category index, not the text"},
    {"version",
     "kept as the catalog availability bit against the running build, not a stored number"},
};

bool empty_default(const Value& def) {
    switch (def.kind) {
    case Value::Kind::integer:
        return def.integer == 0;
    case Value::Kind::number:
        return def.number == 0.0;
    case Value::Kind::boolean:
        return !def.boolean;
    case Value::Kind::text:
        return def.text.empty();
    case Value::Kind::absent:
        return false;
    }
    return false;
}

void expect_int(int64_t member, const Value& def) {
    OA_CHECK(def.kind == Value::Kind::integer);
    OA_CHECK(member == def.integer);
}

void expect_fixed(int32_t member, const Value& def) {
    OA_CHECK(def.kind == Value::Kind::number);
    OA_CHECK(static_cast<double>(member) / 65536.0 == def.number);
}

void expect_real(float member, const Value& def) {
    OA_CHECK(def.kind == Value::Kind::number);
    OA_CHECK(static_cast<double>(member) == def.number);
}

void expect_whole_float(float member, const Value& def) {
    OA_CHECK(def.kind == Value::Kind::integer);
    OA_CHECK(static_cast<double>(member) == static_cast<double>(def.integer));
}

void expect_flag(uint32_t word, uint32_t bit, const Value& def) {
    OA_CHECK(def.kind == Value::Kind::boolean);
    OA_CHECK(((word & bit) != 0) == def.boolean);
}

void expect_text(const char* member, const Value& def) {
    OA_CHECK(def.kind == Value::Kind::text);
    OA_CHECK(std::strcmp(member, def.text.c_str()) == 0);
}

void expect_zero_count(int64_t member, const Value& def) {
    OA_CHECK(def.kind == Value::Kind::number);
    OA_CHECK(def.number == 0.0);
    OA_CHECK(member == 0);
}

bool unmapped_unit(std::string_view key, const Value& def) {
    for (const Notes& note : k_unmapped_unit) {
        if (key != note.key)
            continue;
        OA_CHECK(empty_default(def));
        OA_CHECK(note.reason[0] != '\0');
        return true;
    }
    return false;
}

void compare_unit(
    std::string_view key,
    const Value& def,
    const UnitDef& unit,
    const MoveClass& move,
    oa::data::defs::CategoryRegistry* categories
) {
    namespace defs = oa::data::defs;
    if (unmapped_unit(key, def))
        return;
    if (key == "acceleration")
        expect_fixed(unit.acceleration, def);
    else if (key == "activatewhenbuilt")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_ACTIVATE_WHEN_BUILT, def);
    else if (key == "ai_limit")
        expect_text(unit.ai_limit, def);
    else if (key == "ai_weight")
        expect_text(unit.ai_weight, def);
    else if (key == "amphibious")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_AMPHIBIOUS, def);
    else if (key == "antiweapons")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_ANTI_WEAPONS, def);
    else if (key == "armoredstate")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_ARMORED_STATE, def);
    else if (key == "attackrunlength")
        expect_int(unit.attack_run_length, def);
    else if (key == "badwaterslope")
        expect_int(move.bad_water_slope, def);
    else if (key == "badslope")
        expect_int(move.bad_slope, def);
    else if (key == "bankscale")
        expect_fixed(unit.bank_scale, def);
    else if (key == "bmcode")
        expect_int(unit.bm_code, def);
    else if (key == "brakerate")
        expect_fixed(unit.brake_rate, def);
    else if (key == "buildangle")
        // The loader stores int16_t; the game reads the bits back unsigned (runtime.cpp:90).
        expect_int(std::bit_cast<uint16_t>(unit.build_angle), def);
    else if (key == "buildcostenergy")
        expect_whole_float(unit.build_cost_energy, def);
    else if (key == "buildcostmetal")
        expect_whole_float(unit.build_cost_metal, def);
    else if (key == "builddistance")
        expect_int(unit.build_distance, def);
    else if (key == "builder")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_BUILDER, def);
    else if (key == "buildtime")
        expect_int(unit.build_time, def);
    else if (key == "canattack")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_ATTACK, def);
    else if (key == "cancapture")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_CAPTURE, def);
    else if (key == "candgun")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_DGUN, def);
    else if (key == "canfly")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_CAN_FLY, def);
    else if (key == "canguard")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_GUARD, def);
    else if (key == "canhover")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_CAN_HOVER, def);
    else if (key == "canload")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_LOAD, def);
    else if (key == "canmove")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_MOVE, def);
    else if (key == "canpatrol")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_PATROL, def);
    else if (key == "canreclamate")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_RECLAMATE, def);
    else if (key == "canresurrect")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_RESURRECT, def);
    else if (key == "canstop")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_STOP, def);
    else if (key == "cantbetransported")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED, def);
    else if (key == "cloakcost")
        expect_whole_float(unit.cloak_cost, def);
    else if (key == "cloakcostmoving")
        expect_whole_float(unit.cloak_cost_moving, def);
    else if (key == "commander")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_COMMANDER, def);
    else if (key == "copyright") {
        OA_CHECK(def.kind == Value::Kind::text);
        OA_CHECK(def.text == defs::unit_copyright_default);
    } else if (key == "corpse") {
        OA_CHECK(def.text.empty());
        OA_CHECK(unit.corpse == -1);
    } else if (key == "cruisealt")
        expect_int(unit.cruise_alt, def);
    else if (key == "damagemodifier")
        expect_fixed(unit.damage_modifier, def);
    else if (key == "description")
        expect_text(unit.description, def);
    else if (key == "digger")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_DIGGER, def);
    else if (key == "downloadable")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_DOWNLOADABLE, def);
    else if (key == "energymake")
        expect_real(unit.energy_make, def);
    else if (key == "energystorage")
        expect_real(unit.energy_storage, def);
    else if (key == "energyuse")
        expect_real(unit.energy_use, def);
    else if (key == "explodeas") {
        OA_CHECK(def.text.empty());
        OA_CHECK(unit.explode_as == 1);
    } else if (key == "extractsmetal")
        expect_real(unit.extracts_metal, def);
    else if (key == "firestandorders")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_FIRE_STAND_ORDERS, def);
    else if (key == "floater")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_FLOATER, def);
    else if (key == "footprintx")
        expect_int(unit.footprint_x, def);
    else if (key == "footprintz")
        expect_int(unit.footprint_z, def);
    else if (key == "healtime")
        expect_int(unit.heal_time, def);
    else if (key == "hidedamage")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_HIDE_DAMAGE, def);
    else if (key == "hoverattack")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_HOVER_ATTACK, def);
    else if (key == "immunetoparalyzer")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_IMMUNE_TO_PARALYZER, def);
    else if (key == "init_cloaked")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_INIT_CLOAKED, def);
    else if (key == "isairbase")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_IS_AIRBASE, def);
    else if (key == "isfeature")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_IS_FEATURE, def);
    else if (key == "istargetingupgrade")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_TARGETING_UPGRADE, def);
    else if (key == "kamikaze")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_KAMIKAZE, def);
    else if (key == "kamikazedistance")
        expect_int(unit.kamikaze_distance, def);
    else if (key == "makesmetal")
        expect_int(unit.makes_metal, def);
    else if (key == "maneuverleashlength")
        expect_int(unit.maneuver_leash_length, def);
    else if (key == "maxdamage")
        expect_int(unit.max_damage, def);
    else if (key == "maxslope")
        expect_int(unit.max_slope, def);
    else if (key == "maxvelocity")
        expect_fixed(unit.max_velocity, def);
    else if (key == "maxwaterdepth")
        expect_int(unit.max_water_depth, def);
    else if (key == "maxwaterslope")
        expect_int(unit.max_water_slope, def);
    else if (key == "metalmake")
        expect_real(unit.metal_make, def);
    else if (key == "metalstorage")
        expect_real(unit.metal_storage, def);
    else if (key == "mincloakdistance")
        expect_int(unit.min_cloak_distance, def);
    else if (key == "minwaterdepth")
        expect_int(unit.min_water_depth, def);
    else if (key == "mobilestandorders")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_MOBILE_STAND_ORDERS, def);
    else if (key == "movementclass") {
        OA_CHECK(def.text.empty());
        OA_CHECK(unit.move_class == 0);
    } else if (key == "moverate1")
        expect_fixed(unit.move_rate1, def);
    else if (key == "moverate2")
        expect_fixed(unit.move_rate2, def);
    else if (key == "name")
        expect_text(unit.name, def);
    else if (key == "nochasecategory") {
        OA_CHECK(def.text == "none");
        OA_CHECK(unit.no_chase_category == defs::category_registry_ref(categories, "none"));
    } else if (key == "norestrict")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_NO_RESTRICT, def);
    else if (key == "noshadow")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_NO_SHADOW, def);
    else if (key == "objectname")
        expect_text(unit.object_name, def);
    else if (key == "onoffable")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_ON_OFFABLE, def);
    else if (key == "pitchscale")
        expect_fixed(unit.pitch_scale, def);
    else if (key == "radardistance")
        expect_int(unit.radar_distance, def);
    else if (key == "radardistancejam")
        expect_int(unit.radar_distance_jam, def);
    else if (key == "selfdestructas") {
        OA_CHECK(def.text.empty());
        OA_CHECK(unit.self_destruct_as == 1);
    } else if (key == "selfdestructcountdown") {
        const auto bits = (unit.abilities & OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_MASK) >>
                          OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT;
        OA_CHECK(def.kind == Value::Kind::integer);
        OA_CHECK(bits == (static_cast<uint32_t>(def.integer) & 7u));
    } else if (key == "shootme")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_SHOOT_ME, def);
    else if (key == "showplayername")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_SHOW_PLAYER_NAME, def);
    else if (key == "side")
        expect_text(unit.side, def);
    else if (key == "sightdistance")
        expect_int(unit.sight_distance, def);
    else if (key == "sonardistance")
        expect_int(unit.sonar_distance, def);
    else if (key == "sonardistancejam")
        expect_int(unit.sonar_distance_jam, def);
    else if (key == "sortbias")
        expect_int(unit.sort_bias, def);
    else if (key == "standingfireorder") {
        const auto bits =
            (unit.flags & OA_UNIT_DEF_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_DEF_FLAG_FIRE_ORDER_SHIFT;
        OA_CHECK(def.kind == Value::Kind::integer);
        OA_CHECK(bits == (static_cast<uint32_t>(def.integer) & 3u));
    } else if (key == "standingmoveorder") {
        const auto bits = unit.flags & OA_UNIT_DEF_FLAG_MOVE_ORDER_MASK;
        OA_CHECK(def.kind == Value::Kind::integer);
        OA_CHECK(bits == (static_cast<uint32_t>(def.integer) & 3u));
    } else if (key == "stealth")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_STEALTH, def);
    else if (key == "teleporter")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_TELEPORTER, def);
    else if (key == "tidalgenerator")
        expect_real(unit.tidal_generator, def);
    else if (key == "transportcapacity")
        expect_int(unit.transport_capacity, def);
    else if (key == "transportsize")
        expect_int(unit.transport_size, def);
    else if (key == "turnrate")
        expect_int(unit.turn_rate, def);
    else if (key == "unitname")
        expect_text(unit.unit_name, def);
    else if (key == "upright")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_UPRIGHT, def);
    else if (key == "wacky")
        expect_flag(unit.abilities, OA_UNIT_DEF_ABILITY_WACKY, def);
    else if (key == "waterline")
        expect_int(unit.water_line, def);
    else if (key == "weapon1") {
        OA_CHECK(def.text.empty());
        OA_CHECK(unit.weapon1 == 1);
    } else if (key == "weapon2") {
        OA_CHECK(def.text.empty());
        OA_CHECK(unit.weapon2 == 1);
    } else if (key == "weapon3") {
        OA_CHECK(def.text.empty());
        OA_CHECK(unit.weapon3 == 1);
    } else if (key == "windgenerator")
        expect_real(unit.wind_generator, def);
    else if (key == "workertime")
        expect_int(unit.worker_time, def);
    else if (key == "wpri_badtargetcategory") {
        OA_CHECK(def.text == "none");
        OA_CHECK(
            unit.primary_bad_target_category == defs::category_registry_ref(categories, "none")
        );
    } else if (key == "wsec_badtargetcategory") {
        OA_CHECK(def.text == "none");
        OA_CHECK(
            unit.secondary_bad_target_category == defs::category_registry_ref(categories, "none")
        );
    } else if (key == "wspe_badtargetcategory") {
        OA_CHECK(def.text == "none");
        OA_CHECK(
            unit.special_bad_target_category == defs::category_registry_ref(categories, "none")
        );
    } else if (key == "yardmap") {
        OA_CHECK(def.text.empty());
        OA_CHECK(unit.yard_map == 0);
    } else if (key == "zbuffer")
        expect_flag(unit.flags, OA_UNIT_DEF_FLAG_Z_BUFFER, def);
    else
        OA_CHECK(false);
}

void compare_weapon_flag(std::string_view key, const Value& def, uint32_t word) {
    namespace defs = oa::data::defs;
    if (key == "ballistic")
        expect_flag(word, defs::weapon_flag_ballistic, def);
    else if (key == "beamweapon")
        expect_flag(word, defs::weapon_flag_beam_weapon, def);
    else if (key == "burnblow")
        expect_flag(word, defs::weapon_flag_burn_blow, def);
    else if (key == "commandfire")
        expect_flag(word, defs::weapon_flag_command_fire, def);
    else if (key == "cruise")
        expect_flag(word, defs::weapon_flag_cruise, def);
    else if (key == "dropped")
        expect_flag(word, defs::weapon_flag_dropped, def);
    else if (key == "endsmoke")
        expect_flag(word, defs::weapon_flag_end_smoke, def);
    else if (key == "groundbounce")
        expect_flag(word, defs::weapon_flag_ground_bounce, def);
    else if (key == "guidance")
        expect_flag(word, defs::weapon_flag_guidance, def);
    else if (key == "interceptor")
        expect_flag(word, defs::weapon_flag_interceptor, def);
    else if (key == "lineofsight")
        expect_flag(word, defs::weapon_flag_line_of_sight, def);
    else if (key == "meteor")
        expect_flag(word, defs::weapon_flag_meteor, def);
    else if (key == "noautorange")
        expect_flag(word, defs::weapon_flag_no_auto_range, def);
    else if (key == "noexplode")
        expect_flag(word, defs::weapon_flag_no_explode, def);
    else if (key == "noradar")
        expect_flag(word, defs::weapon_flag_no_radar, def);
    else if (key == "paralyzer")
        expect_flag(word, defs::weapon_flag_paralyzer, def);
    else if (key == "propeller")
        expect_flag(word, defs::weapon_flag_propeller, def);
    else if (key == "selfprop")
        expect_flag(word, defs::weapon_flag_self_prop, def);
    else if (key == "shellweapon")
        expect_flag(word, defs::weapon_flag_shell_weapon, def);
    else if (key == "smoketrail")
        expect_flag(word, defs::weapon_flag_smoke_trail, def);
    else if (key == "soundtrigger")
        expect_flag(word, defs::weapon_flag_sound_trigger, def);
    else if (key == "startsmoke")
        expect_flag(word, defs::weapon_flag_start_smoke, def);
    else if (key == "stockpile")
        expect_flag(word, defs::weapon_flag_stockpile, def);
    else if (key == "targetable")
        expect_flag(word, defs::weapon_flag_targetable, def);
    else if (key == "toairweapon")
        expect_flag(word, defs::weapon_flag_to_air_weapon, def);
    else if (key == "tracks")
        expect_flag(word, defs::weapon_flag_tracks, def);
    else if (key == "turret")
        expect_flag(word, defs::weapon_flag_turret, def);
    else if (key == "twophase")
        expect_flag(word, defs::weapon_flag_two_phase, def);
    else if (key == "unitsonly")
        expect_flag(word, defs::weapon_flag_units_only, def);
    else if (key == "vlaunch")
        expect_flag(word, defs::weapon_flag_vlaunch, def);
    else if (key == "waterweapon")
        expect_flag(word, defs::weapon_flag_water_weapon, def);
    else
        OA_CHECK(false);
}

bool weapon_flag_key(std::string_view key) {
    return key == "ballistic" || key == "beamweapon" || key == "burnblow" || key == "commandfire" ||
           key == "cruise" || key == "dropped" || key == "endsmoke" || key == "groundbounce" ||
           key == "guidance" || key == "interceptor" || key == "lineofsight" || key == "meteor" ||
           key == "noautorange" || key == "noexplode" || key == "noradar" || key == "paralyzer" ||
           key == "propeller" || key == "selfprop" || key == "shellweapon" || key == "smoketrail" ||
           key == "soundtrigger" || key == "startsmoke" || key == "stockpile" ||
           key == "targetable" || key == "toairweapon" || key == "tracks" || key == "turret" ||
           key == "twophase" || key == "unitsonly" || key == "vlaunch" || key == "waterweapon";
}

void compare_weapon(
    std::string_view key,
    const Value& def,
    const WeaponDef& weapon,
    const oa::data::defs::WeaponAssetNames& assets,
    const WeaponDef& lava,
    const oa::data::defs::WeaponAssetNames& lava_assets
) {
    if (key == "id") {
        // The file id selects the slot. Left out, the loader drops the section;
        // weapon_id is the slot index written when the table is reset.
        OA_CHECK(def.kind == Value::Kind::integer);
        OA_CHECK(def.integer == -1);
        return;
    }
    if (weapon_flag_key(key)) {
        compare_weapon_flag(key, def, weapon.flags);
        return;
    }
    if (key == "accuracy")
        expect_int(weapon.accuracy, def);
    else if (key == "areaofeffect")
        expect_int(weapon.area_of_effect, def);
    else if (key == "burst")
        expect_int(weapon.burst, def);
    else if (key == "burstrate")
        expect_zero_count(weapon.burst_rate, def);
    else if (key == "color")
        // The loader stores int8_t; the game reads it back unsigned (combat_state.cpp:108).
        expect_int(static_cast<uint8_t>(weapon.color), def);
    else if (key == "color2")
        // The loader stores int8_t; the game reads it back unsigned (combat_state.cpp:109).
        expect_int(static_cast<uint8_t>(weapon.color2), def);
    else if (key == "coverage")
        expect_int(weapon.coverage, def);
    else if (key == "duration")
        expect_zero_count(weapon.duration, def);
    else if (key == "edgeeffectiveness")
        expect_real(weapon.edge_effectiveness, def);
    else if (key == "energypershot")
        expect_real(weapon.energy_per_shot, def);
    else if (key == "explosionart") {
        OA_CHECK(def.text.empty());
        OA_CHECK(weapon.explosion_art == 0);
        OA_CHECK(assets.explosion_art[0] == '\0');
    } else if (key == "explosiongaf") {
        OA_CHECK(def.text.empty());
        OA_CHECK(assets.explosion_gaf[0] == '\0');
    } else if (key == "firestarter")
        expect_int(weapon.fire_starter, def);
    else if (key == "flighttime")
        expect_zero_count(weapon.flight_time, def);
    else if (key == "holdtime")
        expect_zero_count(weapon.hold_time, def);
    else if (key == "lavaexplosionart") {
        OA_CHECK(def.text.empty());
        OA_CHECK(lava.water_explosion_art == 0);
        OA_CHECK(lava_assets.water_explosion_art[0] == '\0');
    } else if (key == "lavaexplosiongaf") {
        OA_CHECK(def.text.empty());
        OA_CHECK(lava_assets.water_explosion_gaf[0] == '\0');
    } else if (key == "metalpershot")
        expect_real(weapon.metal_per_shot, def);
    else if (key == "minbarrelangle") {
        OA_CHECK(def.kind == Value::Kind::number);
        OA_CHECK(def.number == -11.25);
        OA_CHECK(
            weapon.min_barrel_angle ==
            static_cast<float>(def.number * oa::data::defs::weapon_degrees_to_radians)
        );
    } else if (key == "model") {
        OA_CHECK(def.text.empty());
        OA_CHECK(weapon.model == 0);
        OA_CHECK(assets.model[0] == '\0');
    } else if (key == "name")
        expect_text(weapon.name, def);
    else if (key == "pitchtolerance")
        expect_int(static_cast<uint16_t>(weapon.pitch_tolerance), def);
    else if (key == "randomdecay")
        expect_zero_count(weapon.random_decay, def);
    else if (key == "range")
        expect_int(weapon.range, def);
    else if (key == "reloadtime")
        expect_zero_count(weapon.reload_time, def);
    else if (key == "rendertype")
        expect_int(weapon.render_type, def);
    else if (key == "shakeduration")
        expect_zero_count(weapon.shake_duration, def);
    else if (key == "shakemagnitude")
        expect_int(weapon.shake_magnitude, def);
    else if (key == "smokedelay")
        expect_zero_count(weapon.smoke_delay, def);
    else if (key == "soundhit") {
        OA_CHECK(def.text.empty());
        OA_CHECK(weapon.sound_hit == oa::data::defs::weapon_no_sound);
        OA_CHECK(assets.sound_hit[0] == '\0');
    } else if (key == "soundstart") {
        OA_CHECK(def.text.empty());
        OA_CHECK(weapon.sound_start == oa::data::defs::weapon_no_sound);
        OA_CHECK(assets.sound_start[0] == '\0');
    } else if (key == "soundwater") {
        OA_CHECK(def.text.empty());
        OA_CHECK(weapon.sound_water == oa::data::defs::weapon_no_sound);
        OA_CHECK(assets.sound_water[0] == '\0');
    } else if (key == "sprayangle")
        expect_int(weapon.spray_angle, def);
    else if (key == "startvelocity")
        expect_zero_count(weapon.start_velocity, def);
    else if (key == "tolerance")
        // The loader stores int16_t; the game reads it back unsigned (combat_state.cpp:100).
        expect_int(static_cast<uint16_t>(weapon.tolerance), def);
    else if (key == "turnrate")
        expect_zero_count(weapon.turn_rate, def);
    else if (key == "waterexplosionart") {
        OA_CHECK(def.text.empty());
        OA_CHECK(weapon.water_explosion_art == 0);
        OA_CHECK(assets.water_explosion_art[0] == '\0');
    } else if (key == "waterexplosiongaf") {
        OA_CHECK(def.text.empty());
        OA_CHECK(assets.water_explosion_gaf[0] == '\0');
    } else if (key == "weaponacceleration")
        expect_zero_count(weapon.weapon_acceleration, def);
    else if (key == "weapontimer")
        expect_zero_count(weapon.weapon_timer, def);
    else if (key == "weaponvelocity")
        expect_zero_count(weapon.weapon_velocity, def);
    else
        OA_CHECK(false);
}

void compare_move(std::string_view key, const Value& def, const MoveClass& move, const char* name) {
    if (key == "badwaterslope")
        expect_int(move.bad_water_slope, def);
    else if (key == "badslope")
        expect_int(move.bad_slope, def);
    else if (key == "footprintx")
        expect_int(move.footprint_x, def);
    else if (key == "footprintz")
        expect_int(move.footprint_z, def);
    else if (key == "maxslope")
        expect_int(move.max_slope, def);
    else if (key == "maxwaterdepth")
        expect_int(move.max_water_depth, def);
    else if (key == "maxwaterslope")
        expect_int(move.max_water_slope, def);
    else if (key == "minwaterdepth")
        expect_int(move.min_water_depth, def);
    else if (key == "name")
        expect_text(name, def);
    else
        OA_CHECK(false);
}

const oa::formats::tdf::Block*
parse_section(oa::formats::tdf::Document* document, const char* text, const char* name) {
    oa::formats::tdf::document_init(document);
    OA_CHECK(
        oa::formats::tdf::parse_text(
            document, text, static_cast<uint32_t>(std::strlen(text)), false, nullptr
        )
    );
    OA_CHECK(oa::formats::tdf::select_section(document, name));
    return oa::formats::tdf::cursor(document);
}

struct Hold {
    oa::data::defs::MoveClassTable moves{};
    std::vector<WeaponDef> weapon_defs = std::vector<WeaponDef>(OA_WEAPON_DEF_COUNT);
    oa::data::defs::SoundCategoryTable sounds{};
    oa::data::defs::CategoryRegistry categories{};
    oa::data::defs::UnitDefBlocks blocks{};
    oa::data::defs::WeaponTdfSet weapon_files{};

    Hold() {
        oa::data::defs::move_class_table_init(&moves);
        oa::data::defs::category_registry_init(&categories);
        oa::data::defs::unit_def_blocks_init(&blocks);
        oa::data::defs::weapon_tdf_set_init(&weapon_files);
    }

    ~Hold() {
        oa::data::defs::category_registry_clear(&categories);
        for (uint32_t index = 0; index < blocks.count; ++index)
            std::free(blocks.blocks[index].bytes);
        std::free(blocks.blocks);
        oa::data::defs::weapon_tdf_set_free(&weapon_files);
    }

    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;

    oa::data::defs::UnitDefSources sources() {
        return {"", &moves, weapon_defs, &sounds, &categories, &blocks, nullptr};
    }

    oa::data::defs::UnitHeaderSources header_sources() {
        return {"", &weapon_files, 3, 1, false, false, nullptr};
    }
};

void check_unit(Hold& hold, const char* path, const char* text) {
    MemoryFiles memory;
    memory.files.push_back({path, text});
    const auto files = memory.services();
    UnitDef unit{};
    unit.type_id = 1;
    bool refused = false;
    OA_CHECK(oa::data::defs::load_unit_header(&files, path, unit, hold.header_sources(), &refused));
    OA_CHECK(refused);
    OA_CHECK(oa::data::defs::load_unit_def(&files, path, unit, hold.sources()));

    oa::formats::tdf::Document document;
    const auto* block = parse_section(&document, text, "UNITINFO");
    MoveClass own{};
    oa::data::defs::move_class_init_defaults(&own);
    oa::data::defs::move_class_load(&own, block);
    for (const auto& spec : fields(RecordKind::unit)) {
        if (oa::formats::tdf::find_value(block, std::string(spec.key).c_str()) != nullptr)
            continue;
        compare_unit(
            spec.key, default_value(RecordKind::unit, spec.key, block), unit, own, &hold.categories
        );
    }
    oa::formats::tdf::document_free(&document);
}

struct WeaponHold {
    std::unique_ptr<oa::data::defs::WeaponTable> table =
        std::make_unique<oa::data::defs::WeaponTable>();

    WeaponHold() { oa::data::defs::weapon_table_init(table.get()); }

    ~WeaponHold() { oa::data::defs::weapon_table_free(table.get()); }

    WeaponHold(const WeaponHold&) = delete;
    WeaponHold& operator=(const WeaponHold&) = delete;
};

const oa::formats::tdf::Block*
only_section(oa::formats::tdf::Document* document, const char* text) {
    oa::formats::tdf::document_init(document);
    OA_CHECK(
        oa::formats::tdf::parse_text(
            document, text, static_cast<uint32_t>(std::strlen(text)), false, nullptr
        )
    );
    OA_CHECK(document->root != nullptr && document->root->child_count == 1);
    return document->root->children[0];
}

void check_weapons() {
    oa::formats::tdf::Document refused_document;
    const auto* refused_section = only_section(&refused_document, "[RIDGE_BARE]{name=bare;}");
    WeaponHold refused_table;
    OA_CHECK(!oa::data::defs::weapon_load(refused_table.table.get(), refused_section, nullptr));
    OA_CHECK(refused_table.table->rejected_ids == 1);
    const Value ident = default_value(RecordKind::weapon, "id", refused_section);
    OA_CHECK(ident.kind == Value::Kind::integer && ident.integer == -1);
    oa::formats::tdf::document_free(&refused_document);

    oa::formats::tdf::Document plain_document;
    const auto* plain_section = only_section(&plain_document, "[RIDGE_BARE]{ID=7;}");
    WeaponHold plain;
    OA_CHECK(oa::data::defs::weapon_load(plain.table.get(), plain_section, nullptr));
    const WeaponDef plain_weapon = plain.table->defs[7];
    const oa::data::defs::WeaponAssetNames plain_assets = plain.table->assets[7];
    OA_CHECK(plain_weapon.damage_default == 0);
    OA_CHECK(default_value(RecordKind::weapon_damage, "default", nullptr).integer == 0);
    const Value named = default_value(RecordKind::weapon_damage, "ridgeunit", nullptr);
    OA_CHECK(named.kind == Value::Kind::integer && named.integer == 0);

    oa::data::defs::WeaponLoadOptions lava_options{};
    lava_options.lava_world = true;
    WeaponHold lava;
    OA_CHECK(oa::data::defs::weapon_load(lava.table.get(), plain_section, &lava_options));
    const WeaponDef lava_weapon = lava.table->defs[7];
    const oa::data::defs::WeaponAssetNames lava_assets = lava.table->assets[7];

    for (const auto& spec : fields(RecordKind::weapon)) {
        if (oa::formats::tdf::find_value(plain_section, std::string(spec.key).c_str()) != nullptr)
            continue;
        compare_weapon(
            spec.key,
            default_value(RecordKind::weapon, spec.key, plain_section),
            plain_weapon,
            plain_assets,
            lava_weapon,
            lava_assets
        );
    }
    oa::formats::tdf::document_free(&plain_document);
}

void check_move(const char* text) {
    oa::formats::tdf::Document document;
    OA_CHECK(
        oa::formats::tdf::parse_text(
            &document, text, static_cast<uint32_t>(std::strlen(text)), false, nullptr
        )
    );
    oa::data::defs::MoveClassTable table{};
    oa::data::defs::move_class_table_init(&table);
    oa::data::defs::move_class_table_load(&table, &document);
    oa::formats::tdf::reset_cursor(&document);
    OA_CHECK(oa::formats::tdf::select_section(&document, "CLASS0"));
    const auto* block = oa::formats::tdf::cursor(&document);
    for (const auto& spec : fields(RecordKind::move_class)) {
        if (oa::formats::tdf::find_value(block, std::string(spec.key).c_str()) != nullptr)
            continue;
        compare_move(
            spec.key,
            default_value(RecordKind::move_class, spec.key, block),
            table.classes[0],
            table.names[0]
        );
    }
    oa::formats::tdf::document_free(&document);
}

} // namespace

int main() {
    Hold hold;
    check_unit(hold, "units\\RIDGEBARE.fbi", "[UNITINFO]{UnitName=RIDGEBARE;}");
    check_unit(
        hold,
        "units\\RIDGEFAST.fbi",
        "[UNITINFO]{MaxVelocity=1.5;CloakCost=40;MaxSlope=30;MaxWaterSlope=50;}"
    );
    check_weapons();
    check_move("[CLASS0]{}");
    check_move("[CLASS0]{maxslope=30;maxwaterslope=50;}");
    return oa::test::check_exit_status();
}
