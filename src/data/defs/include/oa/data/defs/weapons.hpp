// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Weapon definitions from WEAPONS\*.TDF. The table holds 256 slots unless the
// caller raises WeaponLoadOptions::weapon_types.
#pragma once

#include "oa/core/weapon_def.h"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/rule_keys.hpp"
#include "oa/formats/tdf.hpp"

#include <cstdint>
#include <vector>

namespace oa::data::defs {

// Bits of WeaponDef.flags, one per boolean TDF key (bit 31 is never written).
enum WeaponFlag : uint32_t {
    weapon_flag_line_of_sight = 1u << 0,
    weapon_flag_ballistic = 1u << 1,
    weapon_flag_shell_weapon = 1u << 2,
    weapon_flag_beam_weapon = 1u << 3,
    weapon_flag_vlaunch = 1u << 4,
    weapon_flag_meteor = 1u << 5,
    weapon_flag_no_radar = 1u << 6,
    weapon_flag_paralyzer = 1u << 7,
    weapon_flag_dropped = 1u << 8,
    weapon_flag_start_smoke = 1u << 9,
    weapon_flag_end_smoke = 1u << 10,
    weapon_flag_sound_trigger = 1u << 11,
    weapon_flag_guidance = 1u << 12,
    weapon_flag_tracks = 1u << 13,
    weapon_flag_units_only = 1u << 14,
    weapon_flag_ground_bounce = 1u << 15,
    weapon_flag_water_weapon = 1u << 16,
    weapon_flag_to_air_weapon = 1u << 17,
    weapon_flag_smoke_trail = 1u << 18,
    weapon_flag_turret = 1u << 19,
    weapon_flag_self_prop = 1u << 20,
    weapon_flag_propeller = 1u << 21,
    weapon_flag_no_explode = 1u << 22,
    weapon_flag_burn_blow = 1u << 23,
    weapon_flag_two_phase = 1u << 24,
    weapon_flag_cruise = 1u << 25,
    weapon_flag_command_fire = 1u << 26,
    weapon_flag_no_auto_range = 1u << 27,
    weapon_flag_stockpile = 1u << 28,
    weapon_flag_targetable = 1u << 29,
    weapon_flag_interceptor = 1u << 30,
};

// TDF time and speed units converted to game ticks (30 per second) and 16.16 fixed point.
inline constexpr double weapon_ticks_per_second = 30.0;
inline constexpr double weapon_velocity_scale = 65536.0 / 30.0;      // pixels/s -> fixed/tick
inline constexpr double weapon_acceleration_scale = 65536.0 / 900.0; // pixels/s^2 -> fixed/tick^2
inline constexpr double weapon_turn_rate_scale = 1.0 / 30.0;         // angle/s -> angle/tick
inline constexpr double weapon_default_min_barrel_angle = -11.25;    // degrees
// 3.1c's constant is a few ulps below pi/180.
inline constexpr double weapon_degrees_to_radians = 0x1.1df46a2529d34p-6;
inline constexpr int32_t weapon_default_range = 0x7fff;
inline constexpr int16_t weapon_no_sound = -1;

inline constexpr uint32_t weapon_damage_name_capacity = 32;
inline constexpr uint32_t weapon_asset_name_capacity = 64;

struct WeaponDamage {
    char unit[weapon_damage_name_capacity]{};
    int32_t damage{};
};

// Per-unit DAMAGE overrides of one weapon, sorted by case-insensitive name.
struct WeaponDamageTable {
    WeaponDamage* entries{};
    uint32_t count{};
    uint32_t capacity{};
};

// Asset lookups made while loading; any callback may be null (result 0 / -1).
struct WeaponResolver {
    void* context{};
    oa_ref32 (*model)(void* context, const char* name) = nullptr;
    oa_ref32 (*animation)(void* context, const char* gaf, const char* sequence) = nullptr;
    int16_t (*sound)(void* context, const char* name) = nullptr;
};

// The asset names one weapon section's keys gave, kept whatever the resolver
// made of them; empty for a missing key. Longer names are cut at 63
// characters.
struct WeaponAssetNames {
    char model[weapon_asset_name_capacity]{};         // model, even when the slot shares it
    char explosion_gaf[weapon_asset_name_capacity]{}; // explosiongaf, an archive under anims/
    char explosion_art[weapon_asset_name_capacity]{}; // explosionart, a sequence in it
    // waterexplosiongaf and waterexplosionart, or on a lava world
    // lavaexplosiongaf and lavaexplosionart (WeaponDef.water_explosion_art).
    char water_explosion_gaf[weapon_asset_name_capacity]{};
    char water_explosion_art[weapon_asset_name_capacity]{};
    char sound_start[weapon_asset_name_capacity]{}; // soundstart, a sound played when it fires
    char sound_hit[weapon_asset_name_capacity]{};   // soundhit, where it goes off on land
    char sound_water[weapon_asset_name_capacity]{}; // soundwater, where it goes off in water
};

struct WeaponTable {
    // One entry per slot. The length is max(OA_WEAPON_DEF_COUNT, highest accepted ID + 1).
    std::vector<WeaponDef> defs;
    // DAMAGE overrides per slot; WeaponDef.damage_overrides holds index + 1 when set.
    std::vector<WeaponDamageTable> damage;
    std::vector<WeaponAssetNames> assets; // the names each slot's section gave
    // What each slot's section gave for the rule keys a profile binds
    // (WeaponLoadOptions::data_keys); all false without a binding.
    std::vector<match_rules::WeaponTypeRules> rule_data;
    uint32_t rejected_ids{}; // sections left out: past the limit, negative or missing ID
    // Sections left out because their ID is at or past WeaponLoadOptions::weapon_types.
    uint32_t past_limit{};
    int32_t highest_past_limit{-1}; // the highest such ID, -1 for none
};

struct WeaponLoadOptions {
    const WeaponResolver* resolver{};
    const char* variant{};
    bool lava_world{};   // use lavaexplosion* instead of waterexplosion* keys
    bool archive_only{}; // ignore loose weapon files, as when archive scanning is on
    // The rule keys a mod profile binds, read into WeaponTable::rule_data;
    // null reads none, as 3.1c does.
    const WeaponDataKeys* data_keys{};
    // IDs from 0 to weapon_types - 1 take a slot. A value outside 256..65536
    // is held to that range.
    uint32_t weapon_types{OA_WEAPON_DEF_COUNT};
};

/// Returns the weapon id a record carries, across both id bytes.
///
/// @param weapon weapon record from a WeaponTable
/// @return the id weapon_table_init or weapon_load wrote
[[nodiscard]] uint16_t weapon_id(const WeaponDef* weapon) noexcept;

/// Returns the model name a weapon slot loaded and owns (WeaponDef.model_name).
///
/// @param weapon weapon record from a WeaponTable
/// @return the name; empty when the slot shares a lower slot's model or has none
[[nodiscard]] const char* weapon_model_name(const WeaponDef* weapon) noexcept;

/// Returns the DAMAGE override handle a weapon record carries in WeaponDef.damage_overrides.
///
/// @param weapon weapon record from a WeaponTable
/// @return slot index + 1 of its WeaponTable.damage map, or 0 when it has none
[[nodiscard]] uint32_t weapon_damage_handle(const WeaponDef* weapon) noexcept;

/// Returns a weapon's damage against a unit.
///
/// @param table table holding the weapon's DAMAGE overrides
/// @param weapon weapon record from `table`
/// @param unit_name unit name, matched case-insensitively
/// @return the DAMAGE override for the unit, or the weapon's default damage
[[nodiscard]] int32_t weapon_damage_for(
    const WeaponTable* table, const WeaponDef* weapon, const char* unit_name
) noexcept;

/// Sizes the table to OA_WEAPON_DEF_COUNT zeroed slots and numbers each one.
///
/// @param[out] table table to reset; any DAMAGE maps it held are not freed
void weapon_table_init(WeaponTable* table) noexcept;

/// Frees every DAMAGE map and forgets owned model names.
///
/// Slots that loaded a model get model 0; the models themselves belong to
/// the resolver. Other fields are left as they were.
///
/// @param[in,out] table table to release
void weapon_table_free(WeaponTable* table) noexcept;

/// Parses one weapon section into the slot its ID key names.
///
/// Every numeric field is written: times become ticks (30 per second),
/// velocities and accelerations 16.16 fixed point per tick, turnrate angle
/// units per tick and minbarrelangle radians (default -11.25 degrees). Flag
/// bits are replaced one by one, so bit 31 survives. The model is shared with
/// a lower slot that loaded the same name; explosion art uses the lava keys
/// on a lava world. The asset names are also kept in the slot's
/// WeaponTable.assets entry. A DAMAGE block sets damage_default and adds each
/// other key to the slot's override map, which a later section with the same
/// ID extends. An ID from 256 to weapon_types - 1 grows every vector to
/// ID + 1 and numbers the new slots. An ID at or past the limit, or a
/// negative or missing ID, is left out and counted in rejected_ids; only an
/// ID at or past the limit is counted in past_limit. With bound rule keys
/// (WeaponLoadOptions::data_keys), the section also replaces the slot's
/// WeaponTable.rule_data record.
///
/// Numbers are read as the game reads them: a key that is present takes its
/// text's value even when the text is empty or not a number (0), and only a
/// missing key takes the default. `range=;` therefore gives a range of 0, not
/// weapon_default_range.
///
/// @param[in,out] table weapon table
/// @param section weapon section of a WEAPONS\*.TDF file
/// @param options asset resolver and world options; may be null
/// @return false when the ID is outside 0..weapon_types-1 (counted in
///     rejected_ids) or a DAMAGE override cannot be stored
bool weapon_load(
    WeaponTable* table, const formats::tdf::Block* section, const WeaponLoadOptions* options
) noexcept;

/// Loads every section of one weapon TDF text into the table, in text order.
///
/// The table is not reset first, so the text adds to what it already holds.
///
/// @param[in,out] table weapon table, set up by weapon_table_init
/// @param text TDF text; need not be NUL-terminated
/// @param length bytes of text
/// @param options asset resolver and world options; may be null
/// @return false when the text does not parse or a section is rejected (see weapon_load)
bool load_weapon_text(
    WeaponTable* table, const char* text, uint32_t length, const WeaponLoadOptions* options
) noexcept;

/// Resets the table and loads every section of every WEAPONS\*.TDF in VFS order.
///
/// Each file is read through the variant directory when it has it; loose
/// files are skipped under archive_only.
///
/// @param files file boundary
/// @param[out] table table to fill
/// @param options asset resolver, variant and loading options; may be null
/// @return the number of files parsed
uint32_t
load_weapon_defs(const Files* files, WeaponTable* table, const WeaponLoadOptions* options) noexcept;

} // namespace oa::data::defs
