// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Field values from the loaded records. Nothing here calls into Lua, and
// nothing here keeps a name index: that table lives in the script state.
#include "definitions.hpp"

#include "oa/data/mission_types.hpp"

#include <cstring>

namespace oa::sim::brains {
namespace {

constexpr std::size_t key_room = 64;
constexpr uint16_t no_feature = 0xffff;

struct Omitted {
    const char* key;
    const char* reason;
};

// Schema keys the loaded record does not keep. The reason is what a reviewer
// reads; the script sees nil for each of them.
constexpr Omitted omitted_units[] = {
    {"copyright", "the loader checks the notice and does not store it"},
    {"version", "the number is reduced to an availability bit and is not stored"},
    {"yardmap", "the text is compiled into yard cells and is not stored"},
    {"corpse", "the record keeps a feature index, not the feature's name"},
    {"badslope", "a movement class keeps it; the unit record keeps only the maximum slopes"},
    {"badwaterslope", "a movement class keeps it; the unit record keeps only the maximum slopes"},
    {"wpri_badtargetcategory", "the record keeps a category mask, not the token text"},
    {"wsec_badtargetcategory", "the record keeps a category mask, not the token text"},
    {"wspe_badtargetcategory", "the record keeps a category mask, not the token text"},
    {"nochasecategory", "the record keeps a category mask, not the token text"},
};

// Sound and art names live on the weapon's asset side. The model name is
// empty when the slot shares another weapon's model, so it is not read either.
constexpr Omitted omitted_weapons[] = {
    {"explosiongaf", "the record keeps an explosion handle, not the archive name"},
    {"explosionart", "the record keeps an explosion handle, not the sequence name"},
    {"lavaexplosiongaf", "the record keeps an explosion handle, not the archive name"},
    {"lavaexplosionart", "the record keeps an explosion handle, not the sequence name"},
    {"waterexplosiongaf", "the record keeps an explosion handle, not the archive name"},
    {"waterexplosionart", "the record keeps an explosion handle, not the sequence name"},
    {"soundstart", "the record keeps a sound id, not the sound name"},
    {"soundhit", "the record keeps a sound id, not the sound name"},
    {"soundwater", "the record keeps a sound id, not the sound name"},
    {"model", "the model name is empty when the slot shares its model"},
};

constexpr std::string_view feature_keys[] = {
    "description", "footprintx",   "footprintz",     "height",          "damage",
    "metal",       "energy",       "reclaimable",    "autoreclaimable", "blocking",
    "flamable",    "geothermal",   "indestructible", "nodisplayinfo",   "animating",
    "animtrans",   "shadtrans",    "featuredead",    "featureburnt",    "featurereclamate",
    "sparktime",   "spreadchance", "reproduce",      "reproducearea",
};

/// Folds ASCII letters into `dest`. A key that does not fit is refused.
///
/// @param dest buffer, including room for a NUL
/// @param room bytes available in `dest`
/// @param key the name to fold
/// @return false when `key` does not fit
bool fold_key(char* dest, std::size_t room, std::string_view key) noexcept {
    if (dest == nullptr || key.size() + 1 > room)
        return false;
    for (std::size_t i = 0; i < key.size(); ++i) {
        const auto letter = static_cast<unsigned char>(key[i]);
        const auto lower = letter >= 'A' && letter <= 'Z' ? letter - 'A' + 'a' : letter;
        dest[i] = static_cast<char>(lower);
    }
    dest[key.size()] = '\0';
    return true;
}

/// The reason for a folded key, or null.
///
/// @param rows the omitted table
/// @param count rows in the table
/// @param folded lower-case key
/// @return the reason, or null
const char* omitted_reason(const Omitted* rows, std::size_t count, const char* folded) noexcept {
    if (folded == nullptr)
        return nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(folded, rows[i].key) == 0)
            return rows[i].reason;
    }
    return nullptr;
}

/// Characters of a fixed buffer, up to its first NUL.
///
/// @param text the buffer
/// @param room bytes `text` may hold
/// @return the text; empty when `text` is null
std::string_view chars_of(const char* text, std::size_t room) noexcept {
    std::size_t length = 0;
    if (text != nullptr) {
        while (length < room && text[length] != '\0')
            ++length;
    }
    return std::string_view(text == nullptr ? "" : text, length);
}

bool put_integer(FieldValue& out, int64_t value) noexcept {
    out = {};
    out.kind = FieldValue::Kind::integer;
    out.integer = value;
    return true;
}

bool put_fixed(FieldValue& out, int32_t count) noexcept {
    out = {};
    out.kind = FieldValue::Kind::fixed;
    out.integer = count;
    return true;
}

bool put_real(FieldValue& out, float value) noexcept {
    out = {};
    out.kind = FieldValue::Kind::real;
    out.real = value;
    return true;
}

bool put_bool(FieldValue& out, bool value) noexcept {
    out = {};
    out.kind = FieldValue::Kind::boolean;
    out.boolean = value;
    return true;
}

bool put_text(FieldValue& out, std::string_view text) noexcept {
    out = {};
    out.kind = FieldValue::Kind::text;
    out.text = text;
    return true;
}

/// A name the typed definition keeps. Null definition reads as empty text.
///
/// @param definition the typed definition, or null
/// @param text the name on that definition; ignored when `definition` is null
/// @param out receives the text
/// @return true
bool put_defined(
    FieldValue& out, const data::unit_definitions::UnitDefinition* definition, std::string_view text
) noexcept {
    if (definition == nullptr)
        return put_text(out, {});
    return put_text(out, text);
}

bool flag_on(uint32_t bits, uint32_t mask) noexcept {
    return (bits & mask) != 0;
}

/// Whole part of a stored float, toward zero. The loader wrote an integer there.
///
/// @param value the stored float
/// @return the truncated integer
int64_t whole_of(float value) noexcept {
    return static_cast<int64_t>(static_cast<int32_t>(value));
}

bool same(const char* folded, const char* key) noexcept {
    return std::strcmp(folded, key) == 0;
}

bool read_unit_key(
    const oa::UnitDef& unit,
    const data::unit_definitions::UnitDefinition* definition,
    const char* folded,
    FieldValue& out
) noexcept {
    if (same(folded, "acceleration"))
        return put_fixed(out, unit.acceleration);
    if (same(folded, "activatewhenbuilt"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_ACTIVATE_WHEN_BUILT));
    if (same(folded, "ai_limit"))
        return put_text(out, chars_of(unit.ai_limit, sizeof unit.ai_limit));
    if (same(folded, "ai_weight"))
        return put_text(out, chars_of(unit.ai_weight, sizeof unit.ai_weight));
    if (same(folded, "amphibious"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_AMPHIBIOUS));
    if (same(folded, "antiweapons"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_ANTI_WEAPONS));
    if (same(folded, "armoredstate"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_ARMORED_STATE));
    if (same(folded, "attackrunlength"))
        return put_integer(out, unit.attack_run_length);
    if (same(folded, "bankscale"))
        return put_fixed(out, unit.bank_scale);
    if (same(folded, "bmcode"))
        return put_integer(out, unit.bm_code);
    if (same(folded, "brakerate"))
        return put_fixed(out, unit.brake_rate);
    if (same(folded, "buildangle"))
        return put_integer(out, static_cast<uint16_t>(unit.build_angle));
    if (same(folded, "buildcostenergy"))
        return put_integer(out, whole_of(unit.build_cost_energy));
    if (same(folded, "buildcostmetal"))
        return put_integer(out, whole_of(unit.build_cost_metal));
    if (same(folded, "builddistance"))
        return put_integer(out, unit.build_distance);
    if (same(folded, "builder"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_BUILDER));
    if (same(folded, "buildtime"))
        return put_integer(out, unit.build_time);
    if (same(folded, "canattack"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_ATTACK));
    if (same(folded, "cancapture"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_CAPTURE));
    if (same(folded, "candgun"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_DGUN));
    if (same(folded, "canfly"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_CAN_FLY));
    if (same(folded, "canguard"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_GUARD));
    if (same(folded, "canhover"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_CAN_HOVER));
    if (same(folded, "canload"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_LOAD));
    if (same(folded, "canmove"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_MOVE));
    if (same(folded, "canpatrol"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_PATROL));
    if (same(folded, "canreclamate"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_RECLAMATE));
    if (same(folded, "canresurrect"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_RESURRECT));
    if (same(folded, "canstop"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CAN_STOP));
    if (same(folded, "cantbetransported"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED));
    if (same(folded, "category")) {
        out = {};
        out.kind = FieldValue::Kind::list;
        out.listed = definition;
        return true;
    }
    if (same(folded, "cloakcost"))
        return put_integer(out, whole_of(unit.cloak_cost));
    if (same(folded, "cloakcostmoving"))
        return put_integer(out, whole_of(unit.cloak_cost_moving));
    if (same(folded, "commander"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_COMMANDER));
    if (same(folded, "cruisealt"))
        return put_integer(out, unit.cruise_alt);
    if (same(folded, "damagemodifier"))
        return put_fixed(out, unit.damage_modifier);
    if (same(folded, "defaultmissiontype")) {
        const auto names = data::mission_types::registered_names();
        const int mission = unit.default_mission_type;
        std::string_view spelling{};
        if (mission >= 0 && static_cast<std::size_t>(mission) < names.size())
            spelling = names[static_cast<std::size_t>(mission)];
        return put_text(out, spelling);
    }
    if (same(folded, "description"))
        return put_text(out, chars_of(unit.description, sizeof unit.description));
    if (same(folded, "digger"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_DIGGER));
    if (same(folded, "downloadable"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_DOWNLOADABLE));
    if (same(folded, "energymake"))
        return put_real(out, unit.energy_make);
    if (same(folded, "energystorage"))
        return put_real(out, unit.energy_storage);
    if (same(folded, "energyuse"))
        return put_real(out, unit.energy_use);
    if (same(folded, "explodeas"))
        return put_defined(
            out,
            definition,
            definition == nullptr
                ? std::string_view{}
                : std::string_view(definition->explode_as.data(), definition->explode_as.size())
        );
    if (same(folded, "extractsmetal"))
        return put_real(out, unit.extracts_metal);
    if (same(folded, "firestandorders"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_FIRE_STAND_ORDERS));
    if (same(folded, "floater"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_FLOATER));
    if (same(folded, "footprintx"))
        return put_integer(out, unit.footprint_x);
    if (same(folded, "footprintz"))
        return put_integer(out, unit.footprint_z);
    if (same(folded, "healtime"))
        return put_integer(out, unit.heal_time);
    if (same(folded, "hidedamage"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_HIDE_DAMAGE));
    if (same(folded, "hoverattack"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_HOVER_ATTACK));
    if (same(folded, "immunetoparalyzer"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_IMMUNE_TO_PARALYZER));
    if (same(folded, "init_cloaked"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_INIT_CLOAKED));
    if (same(folded, "isairbase"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_IS_AIRBASE));
    if (same(folded, "isfeature"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_IS_FEATURE));
    if (same(folded, "istargetingupgrade"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_TARGETING_UPGRADE));
    if (same(folded, "kamikaze"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_KAMIKAZE));
    if (same(folded, "kamikazedistance"))
        return put_integer(out, unit.kamikaze_distance);
    if (same(folded, "makesmetal"))
        return put_integer(out, unit.makes_metal);
    if (same(folded, "maneuverleashlength"))
        return put_integer(out, unit.maneuver_leash_length);
    if (same(folded, "maxdamage"))
        return put_integer(out, unit.max_damage);
    if (same(folded, "maxslope"))
        return put_integer(out, unit.max_slope);
    if (same(folded, "maxvelocity"))
        return put_fixed(out, unit.max_velocity);
    if (same(folded, "maxwaterdepth"))
        return put_integer(out, unit.max_water_depth);
    if (same(folded, "maxwaterslope"))
        return put_integer(out, unit.max_water_slope);
    if (same(folded, "metalmake"))
        return put_real(out, unit.metal_make);
    if (same(folded, "metalstorage"))
        return put_real(out, unit.metal_storage);
    if (same(folded, "mincloakdistance"))
        return put_integer(out, unit.min_cloak_distance);
    if (same(folded, "minwaterdepth"))
        return put_integer(out, unit.min_water_depth);
    if (same(folded, "mobilestandorders"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_MOBILE_STAND_ORDERS));
    if (same(folded, "movementclass"))
        return put_defined(
            out,
            definition,
            definition == nullptr
                ? std::string_view{}
                : std::string_view(
                      definition->movement_class.data(), definition->movement_class.size()
                  )
        );
    if (same(folded, "moverate1"))
        return put_fixed(out, unit.move_rate1);
    if (same(folded, "moverate2"))
        return put_fixed(out, unit.move_rate2);
    if (same(folded, "name"))
        return put_text(out, chars_of(unit.name, sizeof unit.name));
    if (same(folded, "norestrict"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_NO_RESTRICT));
    if (same(folded, "noshadow"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_NO_SHADOW));
    if (same(folded, "objectname"))
        return put_text(out, chars_of(unit.object_name, sizeof unit.object_name));
    if (same(folded, "onoffable"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_ON_OFFABLE));
    if (same(folded, "pitchscale"))
        return put_fixed(out, unit.pitch_scale);
    if (same(folded, "radardistance"))
        return put_integer(out, unit.radar_distance);
    if (same(folded, "radardistancejam"))
        return put_integer(out, unit.radar_distance_jam);
    if (same(folded, "selfdestructas"))
        return put_defined(
            out,
            definition,
            definition == nullptr
                ? std::string_view{}
                : std::string_view(
                      definition->self_destruct_as.data(), definition->self_destruct_as.size()
                  )
        );
    if (same(folded, "selfdestructcountdown")) {
        const uint32_t countdown = (unit.abilities & OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_MASK) >>
                                   OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT;
        return put_integer(out, countdown);
    }
    if (same(folded, "shootme"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_SHOOT_ME));
    if (same(folded, "showplayername"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_SHOW_PLAYER_NAME));
    if (same(folded, "side")) {
        if (definition != nullptr)
            return put_text(
                out, std::string_view(definition->side.data(), definition->side.size())
            );
        return put_text(out, chars_of(unit.side, sizeof unit.side));
    }
    if (same(folded, "sightdistance"))
        return put_integer(out, unit.sight_distance);
    if (same(folded, "sonardistance"))
        return put_integer(out, unit.sonar_distance);
    if (same(folded, "sonardistancejam"))
        return put_integer(out, unit.sonar_distance_jam);
    if (same(folded, "sortbias"))
        return put_integer(out, unit.sort_bias);
    if (same(folded, "soundcategory"))
        return put_defined(
            out,
            definition,
            definition == nullptr
                ? std::string_view{}
                : std::string_view(
                      definition->sound_category.data(), definition->sound_category.size()
                  )
        );
    if (same(folded, "standingfireorder")) {
        const uint32_t order =
            (unit.flags & OA_UNIT_DEF_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_DEF_FLAG_FIRE_ORDER_SHIFT;
        return put_integer(out, order);
    }
    if (same(folded, "standingmoveorder"))
        return put_integer(out, unit.flags & OA_UNIT_DEF_FLAG_MOVE_ORDER_MASK);
    if (same(folded, "stealth"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_STEALTH));
    if (same(folded, "teleporter"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_TELEPORTER));
    if (same(folded, "tidalgenerator"))
        return put_real(out, unit.tidal_generator);
    if (same(folded, "transportcapacity"))
        return put_integer(out, unit.transport_capacity);
    if (same(folded, "transportsize"))
        return put_integer(out, unit.transport_size);
    if (same(folded, "turnrate"))
        return put_integer(out, unit.turn_rate);
    if (same(folded, "unitname"))
        return put_text(out, chars_of(unit.unit_name, sizeof unit.unit_name));
    if (same(folded, "upright"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_UPRIGHT));
    if (same(folded, "wacky"))
        return put_bool(out, flag_on(unit.abilities, OA_UNIT_DEF_ABILITY_WACKY));
    if (same(folded, "waterline"))
        return put_integer(out, unit.water_line);
    if (same(folded, "weapon1"))
        return put_defined(
            out,
            definition,
            definition == nullptr
                ? std::string_view{}
                : std::string_view(definition->weapon1.data(), definition->weapon1.size())
        );
    if (same(folded, "weapon2"))
        return put_defined(
            out,
            definition,
            definition == nullptr
                ? std::string_view{}
                : std::string_view(definition->weapon2.data(), definition->weapon2.size())
        );
    if (same(folded, "weapon3"))
        return put_defined(
            out,
            definition,
            definition == nullptr
                ? std::string_view{}
                : std::string_view(definition->weapon3.data(), definition->weapon3.size())
        );
    if (same(folded, "windgenerator"))
        return put_real(out, unit.wind_generator);
    if (same(folded, "workertime"))
        return put_integer(out, unit.worker_time);
    if (same(folded, "zbuffer"))
        return put_bool(out, flag_on(unit.flags, OA_UNIT_DEF_FLAG_Z_BUFFER));
    return false;
}

bool read_weapon_key(const oa::WeaponDef& weapon, const char* folded, FieldValue& out) noexcept {
    if (same(folded, "accuracy"))
        return put_integer(out, weapon.accuracy);
    if (same(folded, "areaofeffect"))
        return put_integer(out, weapon.area_of_effect);
    if (same(folded, "ballistic"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_BALLISTIC));
    if (same(folded, "beamweapon"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_BEAM_WEAPON));
    if (same(folded, "burnblow"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_BURN_BLOW));
    if (same(folded, "burst"))
        return put_integer(out, weapon.burst);
    if (same(folded, "burstrate"))
        return put_integer(out, weapon.burst_rate);
    if (same(folded, "color"))
        return put_integer(out, static_cast<uint8_t>(weapon.color));
    if (same(folded, "color2"))
        return put_integer(out, static_cast<uint8_t>(weapon.color2));
    if (same(folded, "commandfire"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_COMMAND_FIRE));
    if (same(folded, "coverage"))
        return put_integer(out, weapon.coverage);
    if (same(folded, "cruise"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_CRUISE));
    if (same(folded, "dropped"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_DROPPED));
    if (same(folded, "duration"))
        return put_integer(out, weapon.duration);
    if (same(folded, "edgeeffectiveness"))
        return put_real(out, weapon.edge_effectiveness);
    if (same(folded, "endsmoke"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_END_SMOKE));
    if (same(folded, "energypershot"))
        return put_real(out, weapon.energy_per_shot);
    if (same(folded, "firestarter"))
        return put_integer(out, weapon.fire_starter);
    if (same(folded, "flighttime"))
        return put_integer(out, weapon.flight_time);
    if (same(folded, "groundbounce"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_GROUND_BOUNCE));
    if (same(folded, "guidance"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_GUIDANCE));
    if (same(folded, "holdtime"))
        return put_integer(out, weapon.hold_time);
    if (same(folded, "id"))
        return put_integer(out, weapon.weapon_id);
    if (same(folded, "interceptor"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_INTERCEPTOR));
    if (same(folded, "lineofsight"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_LINE_OF_SIGHT));
    if (same(folded, "metalpershot"))
        return put_real(out, weapon.metal_per_shot);
    if (same(folded, "meteor"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_METEOR));
    if (same(folded, "minbarrelangle"))
        return put_real(out, weapon.min_barrel_angle);
    if (same(folded, "name"))
        return put_text(out, chars_of(weapon.name, sizeof weapon.name));
    if (same(folded, "noautorange"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_NO_AUTO_RANGE));
    if (same(folded, "noexplode"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_NO_EXPLODE));
    if (same(folded, "noradar"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_NO_RADAR));
    if (same(folded, "paralyzer"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_PARALYZER));
    if (same(folded, "pitchtolerance"))
        return put_integer(out, static_cast<uint16_t>(weapon.pitch_tolerance));
    if (same(folded, "propeller"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_PROPELLER));
    if (same(folded, "randomdecay"))
        return put_integer(out, weapon.random_decay);
    if (same(folded, "range"))
        return put_integer(out, weapon.range);
    if (same(folded, "reloadtime"))
        return put_integer(out, weapon.reload_time);
    if (same(folded, "rendertype"))
        return put_integer(out, weapon.render_type);
    if (same(folded, "selfprop"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_SELF_PROP));
    if (same(folded, "shakeduration"))
        return put_integer(out, weapon.shake_duration);
    if (same(folded, "shakemagnitude"))
        return put_integer(out, weapon.shake_magnitude);
    if (same(folded, "shellweapon"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_SHELL_WEAPON));
    if (same(folded, "smokedelay"))
        return put_integer(out, weapon.smoke_delay);
    if (same(folded, "smoketrail"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_SMOKE_TRAIL));
    if (same(folded, "soundtrigger"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_SOUND_TRIGGER));
    if (same(folded, "sprayangle"))
        return put_integer(out, weapon.spray_angle);
    if (same(folded, "startsmoke"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_START_SMOKE));
    if (same(folded, "startvelocity"))
        return put_fixed(out, weapon.start_velocity);
    if (same(folded, "stockpile"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_STOCKPILE));
    if (same(folded, "targetable"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_TARGETABLE));
    if (same(folded, "toairweapon"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_TO_AIR_WEAPON));
    if (same(folded, "tolerance"))
        return put_integer(out, static_cast<uint16_t>(weapon.tolerance));
    if (same(folded, "tracks"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_TRACKS));
    if (same(folded, "turnrate"))
        return put_integer(out, weapon.turn_rate);
    if (same(folded, "turret"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_TURRET));
    if (same(folded, "twophase"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_TWO_PHASE));
    if (same(folded, "unitsonly"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_UNITS_ONLY));
    if (same(folded, "vlaunch"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_VLAUNCH));
    if (same(folded, "waterweapon"))
        return put_bool(out, flag_on(weapon.flags, OA_WEAPON_FLAG_WATER_WEAPON));
    if (same(folded, "weaponacceleration"))
        return put_fixed(out, weapon.weapon_acceleration);
    if (same(folded, "weapontimer"))
        return put_integer(out, weapon.weapon_timer);
    if (same(folded, "weaponvelocity"))
        return put_fixed(out, weapon.weapon_velocity);
    return false;
}

/// A feature link's name, or nil when the link is empty or outside the table.
bool put_feature_link(
    std::span<const oa::FeatureDef> table, uint16_t link, FieldValue& out
) noexcept {
    out = {};
    if (link == no_feature || static_cast<std::size_t>(link) >= table.size()) {
        out.kind = FieldValue::Kind::nil;
        return true;
    }
    return put_text(out, chars_of(table[link].name, sizeof table[link].name));
}

bool read_feature_key(
    const oa::FeatureDef& feature,
    std::span<const oa::FeatureDef> table,
    const char* folded,
    FieldValue& out
) noexcept {
    if (same(folded, "description"))
        return put_text(out, chars_of(feature.description, sizeof feature.description));
    if (same(folded, "footprintx"))
        return put_integer(out, feature.footprint_x);
    if (same(folded, "footprintz"))
        return put_integer(out, feature.footprint_z);
    if (same(folded, "height"))
        return put_integer(out, feature.height);
    if (same(folded, "damage"))
        return put_integer(out, feature.damage);
    if (same(folded, "metal"))
        return put_real(out, feature.metal);
    if (same(folded, "energy"))
        return put_real(out, feature.energy);
    if (same(folded, "reclaimable"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_RECLAIMABLE));
    if (same(folded, "autoreclaimable"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_AUTO_RECLAIMABLE));
    if (same(folded, "blocking"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_BLOCKING));
    if (same(folded, "flamable"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_FLAMABLE));
    if (same(folded, "geothermal"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_GEOTHERMAL));
    if (same(folded, "indestructible"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_INDESTRUCTIBLE));
    if (same(folded, "nodisplayinfo"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_NO_DISPLAY_INFO));
    if (same(folded, "animating"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_ANIMATING));
    if (same(folded, "animtrans"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_ANIM_TRANS));
    if (same(folded, "shadtrans"))
        return put_bool(out, flag_on(feature.flags, OA_FEATURE_FLAG_SHAD_TRANS));
    if (same(folded, "featuredead"))
        return put_feature_link(table, feature.dead_feature, out);
    if (same(folded, "featureburnt"))
        return put_feature_link(table, feature.burnt_feature, out);
    if (same(folded, "featurereclamate"))
        return put_feature_link(table, feature.reclamate_feature, out);
    if (same(folded, "sparktime"))
        return put_integer(out, feature.spark_time);
    if (same(folded, "spreadchance"))
        return put_integer(out, feature.spread_chance);
    if (same(folded, "reproduce"))
        return put_integer(out, feature.reproduce);
    if (same(folded, "reproducearea"))
        return put_integer(out, feature.reproduce_area);
    return false;
}

} // namespace

bool read_unit_field(
    const oa::UnitDef& unit,
    const data::unit_definitions::UnitDefinition* definition,
    std::string_view key,
    FieldValue& out
) noexcept {
    char folded[key_room];
    if (!fold_key(folded, sizeof folded, key))
        return false;
    if (omitted_reason(omitted_units, sizeof omitted_units / sizeof omitted_units[0], folded) !=
        nullptr)
        return false;
    return read_unit_key(unit, definition, folded, out);
}

bool read_weapon_field(
    const oa::WeaponDef& weapon, std::string_view key, FieldValue& out
) noexcept {
    char folded[key_room];
    if (!fold_key(folded, sizeof folded, key))
        return false;
    if (omitted_reason(
            omitted_weapons, sizeof omitted_weapons / sizeof omitted_weapons[0], folded
        ) != nullptr)
        return false;
    return read_weapon_key(weapon, folded, out);
}

bool read_feature_field(
    const oa::FeatureDef& feature,
    std::span<const oa::FeatureDef> table,
    std::string_view key,
    FieldValue& out
) noexcept {
    char folded[key_room];
    if (!fold_key(folded, sizeof folded, key))
        return false;
    return read_feature_key(feature, table, folded, out);
}

const char* omitted_unit_reason(std::string_view key) noexcept {
    char folded[key_room];
    if (!fold_key(folded, sizeof folded, key))
        return nullptr;
    return omitted_reason(omitted_units, sizeof omitted_units / sizeof omitted_units[0], folded);
}

const char* omitted_weapon_reason(std::string_view key) noexcept {
    char folded[key_room];
    if (!fold_key(folded, sizeof folded, key))
        return nullptr;
    return omitted_reason(
        omitted_weapons, sizeof omitted_weapons / sizeof omitted_weapons[0], folded
    );
}

std::span<const std::string_view> feature_field_keys() noexcept {
    return feature_keys;
}

} // namespace oa::sim::brains
