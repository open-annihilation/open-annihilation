// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "oa/core/unit_def.h"
#include "oa/core/weapon_def.h"
#include "oa/data/defs/categories.hpp"
#include "oa/data/defs/move_classes.hpp"
#include "oa/data/defs/sound_categories.hpp"
#include "oa/data/defs/unit_def_loader.hpp"

namespace oa::data::unit_definitions {

enum class ErrorCode { none, io, malformed, missing_unitinfo, invalid_number };

struct Error {
    ErrorCode code = ErrorCode::none;
    std::size_t offset = 0;
    std::string message;
};

template <typename T>
struct Result {
    T value{};
    Error error{};

    /// Returns whether no error was reported.
    [[nodiscard]] explicit operator bool() const noexcept { return error.code == ErrorCode::none; }
};

// A unit type's typed fields, filled from the UnitDef record its FBI loaded
// into (unit_definition_from). The *_fixed members are the record's signed
// 16.16 values.
struct UnitDefinition {
    std::string unit_name, object_name, display_name, description, side;
    std::string movement_class, sound_category;
    std::string weapon1, weapon2, weapon3, explode_as, self_destruct_as;
    std::vector<std::string> categories;

    int32_t build_cost_energy = 0, build_cost_metal = 0, build_time = 0;
    int32_t max_damage = 0;
    int32_t max_velocity_fixed = 0, brake_rate_fixed = 0, acceleration_fixed = 0;
    int32_t bank_scale_fixed = 65536, pitch_scale_fixed = 0;
    int32_t damage_modifier_fixed = 65536, move_rate1_fixed = 0, move_rate2_fixed = 0;
    int16_t turn_rate = 0, worker_time = 0, heal_time = 0;
    int16_t sight_distance = 0, radar_distance = 0, sonar_distance = 0;
    int16_t radar_distance_jam = 0, sonar_distance_jam = 0;
    int16_t min_cloak_distance = 0, build_angle = 0, build_distance = 0;
    int16_t sort_bias = 0, cruise_altitude = 0, maneuver_leash_length = 0;
    int16_t attack_run_length = 0, kamikaze_distance = 0;
    // The movement class's footprint, depths and slopes when the type names
    // one, as UnitDef holds them.
    int16_t footprint_x = 0, footprint_z = 0;
    int16_t max_water_depth = 10000, min_water_depth = -10000;
    uint8_t max_slope = 255, max_water_slope = 255;
    int8_t waterline = 0, transport_size = 0, transport_capacity = 0, bm_code = 0;
    int8_t makes_metal = 0;
    float energy_make = 0, energy_use = 0, metal_make = 0, extracts_metal = 0;
    float wind_generator = 0, tidal_generator = 0, energy_storage = 0, metal_storage = 0;
    float cloak_cost = 0, cloak_cost_moving = 0;
    uint8_t standing_move_order = 2, standing_fire_order = 2;
    uint8_t self_destruct_countdown = 5;

    bool init_cloaked = false, downloadable = false, builder = false, stealth = false;
    bool can_cloak = false; // cloak_cost > 0
    bool z_buffer = false, is_airbase = false, targeting_upgrade = false, teleporter = false;
    bool hide_damage = false, shoot_me = false, armored_state = false, activate_when_built = false;
    bool can_fly = false, can_hover = false, upright = false, floater = false, amphibious = false;
    bool is_feature = false, no_shadow = false, immune_to_paralyzer = false, hover_attack = false;
    bool anti_weapons = false, digger = false, on_offable = false;
    bool mobile_stand_orders = false, fire_stand_orders = false, can_stop = false;
    bool can_attack = false, can_guard = false, can_patrol = false, can_move = false;
    bool can_load = false, can_reclamate = false, can_resurrect = false, can_capture = false;
    bool can_dgun = false, kamikaze = false, no_restrict = false, show_player_name = false;
    bool commander = false, cant_be_transported = false;
};

// Bits of UnitDef.flags that loading an FBI sets.
enum class UnitFlag : uint32_t {
    init_cloaked = 1U << 4,
    downloadable = 1U << 5,
    builder = 1U << 6,
    z_buffer = 1U << 7,
    stealth = 1U << 8,
    is_airbase = 1U << 9,
    targeting_upgrade = 1U << 10,
    can_fly = 1U << 11,
    can_hover = 1U << 12,
    teleporter = 1U << 13,
    hide_damage = 1U << 14,
    shoot_me = 1U << 15,
    armored_state = 1U << 17,
    activate_when_built = 1U << 18,
    floater = 1U << 19,
    upright = 1U << 20,
    amphibious = 1U << 21,
    is_feature = 1U << 24,
    no_shadow = 1U << 25,
    immune_to_paralyzer = 1U << 26,
    hover_attack = 1U << 27,
    kamikaze = 1U << 28,
    anti_weapons = 1U << 29,
    digger = 1U << 30,
};

/// Returns the bit of a unit flag.
///
/// @param flag unit flag
/// @return its mask in UnitDef.flags
[[nodiscard]] constexpr uint32_t flag_mask(UnitFlag flag) noexcept {
    return static_cast<uint32_t>(flag);
}

/// Packs the UnitDef.flags word.
///
/// @param definition the unit type's fields
/// @return standing move orders in bits 0..1, fire orders in bits 2..3, and
///         each UnitFlag that is set
[[nodiscard]] uint32_t pack_unit_flags(const UnitDefinition& definition) noexcept;
/// Packs the UnitDef.abilities word.
///
/// Bit 13 is cloakcost > 0 and bits 20..22 are the self-destruct countdown.
///
/// @param definition the unit type's fields
/// @return the packed word
/// @quirk Canreclamate also sets bit 9, as in 3.1c.
[[nodiscard]] uint32_t pack_unit_abilities(const UnitDefinition& definition) noexcept;

// The UnitDef fields spawning reads, under the same names; unit_flags is
// UnitDef.flags.
struct SpawnDefinitionFields {
    uint32_t unit_flags = 0;
    int16_t footprint_x = 0; // after movement-class resolution
    int16_t footprint_z = 0; // after movement-class resolution
    int32_t max_damage = 0;
    int16_t heal_time = 0;
    int16_t build_angle = 0;
    int8_t makes_metal = 0; // as the FBI loads it
    int8_t bm_code = 0;
};

/// Projects the scalar fields spawning reads directly from a unit type.
///
/// Runtime-owned model, COB, player-limit and availability state is not
/// manufactured here.
///
/// @param definition the unit type's fields
/// @return the flag word and the spawn fields
[[nodiscard]] SpawnDefinitionFields project_for_spawn(const UnitDefinition& definition) noexcept;

struct RuntimeDefinitionMetadata {
    std::optional<uint8_t> movement_class_handle;
    int16_t footprint_x = 0, footprint_z = 0;
    int16_t max_water_depth = 10000, min_water_depth = -10000;
    uint8_t max_slope = 255, bad_slope = 127;
    uint8_t max_water_slope = 255, bad_water_slope = 127;
    int32_t slope_speed_step_fixed = 0; // UnitDef.slope_speed_step
    std::vector<uint8_t> yard_cells;    // UnitDef.yard_map, row-major Z then X
    int16_t sight_distance = 0, radar_distance = 0, sonar_distance = 0;
    int16_t radar_distance_jam = 0, sonar_distance_jam = 0;
};

// The tables a loaded UnitDef's references point into.
struct UnitDefinitionSources {
    const defs::MoveClassTable* move_classes{};         // UnitDef.move_class
    std::span<const WeaponDef> weapon_defs{};           // UnitDef.weapon1..3, explode_as, ...
    const defs::SoundCategoryTable* sound_categories{}; // UnitDef.sound_category
    const defs::CategoryRegistry* categories{};         // the categories that hold the type
};

/// Fills the typed fields of a unit type from the record its FBI loaded into.
///
/// Names come from the tables the record refers to: the movement class (empty
/// for none), the sound category, each weapon's section name (empty for
/// weapon 0, the stand-in for a missing or unknown name) and every category
/// whose mask holds the type except ALL. Footprint, water depths and slopes
/// are the record's, the movement class's when it names one.
///
/// @param unit record data::defs::load_unit_def filled
/// @param sources tables the record's references point into
/// @return the typed fields
[[nodiscard]] UnitDefinition
unit_definition_from(const UnitDef& unit, const UnitDefinitionSources& sources);

/// Resolves a loaded unit type's movement class, limits, yard and sensors.
///
/// The record already holds its movement class's footprint, depth and
/// maximum slopes; the class adds its bad slopes, and a type without one
/// takes half of each maximum. A building's yard is the yard map block the
/// FBI loader compiled; so is a mobile unit's when the loader gave it one
/// (units.mobile-unit-yardmap). A building the loader gave none because its
/// file has no YardMap key (units.skip-empty-yardmap) has a yard of empty
/// cells, which claim and refuse nothing, as a building whose YardMap text
/// has no cell letters.
///
/// @param unit record data::defs::load_unit_def filled
/// @param movement_classes the classes UnitDef.move_class refers to
/// @param blocks the blocks UnitDef.yard_map refers to
/// @param yard_maps which units the loader gave a yard map
/// @return the metadata, or an error for a unit whose yard map did not load
[[nodiscard]] Result<RuntimeDefinitionMetadata> resolve_runtime_metadata(
    const UnitDef& unit,
    const defs::MoveClassTable& movement_classes,
    const defs::UnitDefBlocks& blocks,
    const defs::YardMapRules& yard_maps = defs::YardMapRules{}
);

// 3.1c merges loose files and ordered archives before it lists units/*.FBI.
// Implementations must therefore return one winning logical path per
// case-insensitive name from list_effective().
class CatalogAssetReader {
  public:

    virtual ~CatalogAssetReader() = default;
    /// Lists the effective resources of a directory.
    ///
    /// @param directory resource directory, e.g. "units"
    /// @param extension required suffix, e.g. ".FBI"
    /// @return one winning logical path per case-insensitive name
    [[nodiscard]] virtual Result<std::vector<std::string>>
    list_effective(std::string_view directory, std::string_view extension) const = 0;
    /// Reads a resource's text.
    ///
    /// @param logical_path path from list_effective
    /// @return the content, or an error
    [[nodiscard]] virtual Result<std::string> read(std::string_view logical_path) const = 0;
};

/// A copy of one category mask: bit n of the words is unit type id n. It
/// holds as many words as the category registry's masks (16 for 3.1c's
/// 512 type ids), or none for a category the type does not name.
struct UnitCategoryMask {
    std::vector<uint32_t> words;
    /// Tests whether a unit type is in the category.
    ///
    /// @param type_id unit type id
    /// @return true when its bit is set; false past the mask's words
    [[nodiscard]] bool contains(uint16_t type_id) const noexcept;
};

struct UnitTargetCategoryMasks {
    UnitCategoryMask primary_bad;   // resolved from UnitDef.primary_bad_target_category
    UnitCategoryMask secondary_bad; // resolved from UnitDef.secondary_bad_target_category
    UnitCategoryMask special_bad;   // resolved from UnitDef.special_bad_target_category
    UnitCategoryMask no_chase;      // resolved from UnitDef.no_chase_category
};

/// Copies a loaded unit type's target-category masks out of the category registry.
///
/// @param unit record data::defs::load_unit_def filled
/// @param categories the registry its category references point into, once
///     every type is registered
/// @return the type's three weapon bad-target masks and its no-chase mask
[[nodiscard]] UnitTargetCategoryMasks
target_category_masks(const UnitDef& unit, const defs::CategoryRegistry& categories);

} // namespace oa::data::unit_definitions
