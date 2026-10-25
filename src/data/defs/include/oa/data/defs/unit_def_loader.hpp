// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The FBI unit-definition loader: one UNITINFO section into a UnitDef record.
#pragma once

#include "oa/core/unit_def.h"
#include "oa/core/weapon_def.h"
#include "oa/data/defs/categories.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/move_classes.hpp"
#include "oa/data/defs/sound_categories.hpp"
#include "oa/data/defs/unit_texts.hpp"
#include "oa/data/match_rules.hpp"
#include "oa/formats/tdf.hpp"

#include <cstdint>
#include <span>

namespace oa::data::defs {

inline constexpr int16_t min_cloak_distance_default = 80; // a cloaker's when unset
inline constexpr uint32_t self_destruct_countdown_default = 5;
inline constexpr uint32_t standing_order_default = 2;
inline constexpr int32_t fixed_unit_scale = 0x10000; // bankscale/damagemodifier default
inline constexpr const char* bad_target_category_default = "none";

// Blocks a UnitDef refers to: the yard map (UnitDef.yard_map) and the build
// list (UnitDef.build_ids). A record stores block index + 1; 0 means none.
struct UnitDefBlock {
    uint8_t* bytes{};
    uint32_t size{};
};

struct UnitDefBlocks {
    UnitDefBlock* blocks{};
    uint32_t count{};
    uint32_t capacity{};
};

/// Empties a block store without freeing anything.
///
/// @param[out] blocks store to reset; its previous storage is not released
void unit_def_blocks_init(UnitDefBlocks* blocks) noexcept;

/// Allocates a zero-filled block.
///
/// The store grows from 256 blocks by doubling, up to 65536.
///
/// @param[in,out] blocks store to extend
/// @param size block size in bytes; 0 allocates an empty block
/// @return block index + 1, or 0 when the store is full or an allocation fails
oa_ref32 unit_def_blocks_alloc(UnitDefBlocks* blocks, uint32_t size) noexcept;

/// Resolves a block reference to its bytes.
///
/// @param blocks store that issued the reference
/// @param ref block index + 1 from unit_def_blocks_alloc
/// @return the bytes, or null for 0 and out-of-range references
[[nodiscard]] uint8_t* unit_def_block(const UnitDefBlocks* blocks, oa_ref32 ref) noexcept;

/// Returns the size of a referenced block.
///
/// @param blocks store that issued the reference
/// @param ref block index + 1 from unit_def_blocks_alloc
/// @return the size in bytes, or 0 for 0 and out-of-range references
[[nodiscard]] uint32_t unit_def_block_size(const UnitDefBlocks* blocks, oa_ref32 ref) noexcept;

// Lookups the loader makes outside the definition tables.
struct UnitDefLoadHost {
    void* context{};
    // FeatureDef index of the named corpse, loading the definition when the
    // table lacks it.
    int16_t (*corpse)(void* context, const char* feature_name) = nullptr;
};

// Which units get a yard map (UnitDef.yard_map) under a mod's unit rules;
// the defaults are 3.1c's: every building, and nothing else.
struct YardMapRules {
    // Mobile units (bmcode not 0) get one too (units.mobile-unit-yardmap).
    bool mobile_units{};
    // A unit whose file has no YardMap key, or whose footprint is 0 cells
    // wide or deep, gets none (units.skip-empty-yardmap).
    bool skip_without_key{};
};

/// Returns which units get a yard map under a mod's unit rules.
///
/// @param units the rules' units area
/// @return the yard map rules: units.mobile-unit-yardmap and
///         units.skip-empty-yardmap as each is on or off
[[nodiscard]] constexpr YardMapRules yard_map_rules(const match_rules::UnitsRules& units) noexcept {
    return {units.mobile_unit_yardmap.enabled, units.skip_empty_yardmap.enabled};
}

// Tables a unit definition resolves names against. All are required except host.
struct UnitDefSources {
    const char* language{}; // localized-key prefix ("" for none)
    const MoveClassTable* move_classes{};
    std::span<const WeaponDef> weapon_defs{}; // the whole weapon table
    const SoundCategoryTable* sound_categories{};
    CategoryRegistry* categories{};
    UnitDefBlocks* blocks{};
    const UnitDefLoadHost* host{};
    YardMapRules yard_maps{}; // which units get a yard map; 3.1c's by default
    // Receives the unit's names and descriptions in other languages, for
    // what players see; null reads none (oa/data/defs/unit_texts.hpp).
    const UnitTextSink* texts{};
};

/// Finds a weapon by its TDF section name.
///
/// The lowest slot wins when several share a name.
///
/// @param weapon_defs the whole weapon table
/// @param name section name, matched case-insensitively; may be null
/// @return index of the first match, or -1 for a null or empty name or no match
[[nodiscard]] int
find_weapon_by_name(std::span<const WeaponDef> weapon_defs, const char* name) noexcept;

/// Reads a unit file's UNITINFO section over its record.
///
/// Keys are read in the game's order: names, the targeting category masks,
/// costs and 16.16 motion values (moverate1/2 default to twice maxvelocity),
/// the packed flags and abilities words, category registration,
/// sound category (by name, else the text as a number), corpse, movement
/// class (or the file's own class keys when it names none), the five weapons
/// (unknown names give weapon 0), the yard map of a building (bmcode 0), or of
/// the units UnitDefSources::yard_maps names, and
/// the footprint bounds and sizes in 16.16. A cloaker without
/// mincloakdistance gets min_cloak_distance_default.
///
/// @param files file boundary
/// @param path FBI file path
/// @param[in,out] unit record to fill; its type_id must already be set
///     (categories register it) and its model_height feeds size_y; untouched
///     on failure
/// @param sources name tables the keys resolve against
/// @return false when the file is missing, malformed or has no UNITINFO section
/// @quirk Footprint bounds come from 32-bit products that wrap and are halved
///     toward zero, as in 3.1c.
bool load_unit_def(
    const Files* files, const char* path, UnitDef& unit, const UnitDefSources& sources
) noexcept;

} // namespace oa::data::defs
