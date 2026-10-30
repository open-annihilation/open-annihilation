// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The engine's capacities that a mod may raise: units per player, unit type
// ids, category masks, weapon ids, sides, effect emitters, the path search's
// node budget, the builders' build lists and the model composite buffer.
// Every field defaults to its 3.1c value, so a default Limits plays the game
// exactly as 3.1c does; a mod's profile fills the record before any game
// data loads.
#pragma once

#include <cstddef>
#include <cstdint>

namespace oa::data::limits {

/// The most units per player any limit may name: ten players' slots and the
/// reserved one (6553 * 10 + 1) are the most that 16-bit unit ids number.
inline constexpr uint16_t highest_units_per_player = 6553;
/// The unit type ids 3.1c's type bitsets and category masks hold.
inline constexpr uint32_t base_type_bits = 512;
/// The fewest unit type ids a type bitset or a category mask holds, and the
/// step its size goes up in.
inline constexpr uint32_t type_bits_step = 512;
/// The most unit type ids a type bitset or a category mask may hold: every
/// 16-bit type id.
inline constexpr uint32_t highest_type_bits = 65536;
/// The most emitters any effect layer may hold before it evicts.
inline constexpr uint32_t highest_effect_queue = 1'000'000;
/// The most emitters the shared effect pool may hand out.
inline constexpr uint32_t highest_effect_reserve = 10'000'000;
/// The most path nodes a game tick any budget may name.
inline constexpr int32_t highest_path_search_nodes = 10'000'000;
/// The most CANBUILD entries a builder's list may copy; also the most a
/// list keeps when it keeps every entry (BuildListOverflow::dynamic).
inline constexpr uint32_t highest_build_list_copy = 1024;
/// The smallest and largest side of the model composite buffer, in pixels.
inline constexpr int32_t lowest_composite_side = 64;
inline constexpr int32_t highest_composite_side = 8192;
/// The weapon ids 3.1c numbers: ids 0 to 255.
inline constexpr uint32_t base_weapon_types = 256;
/// The most weapon ids a mod may number: every id a 16-bit weapon id names.
inline constexpr uint32_t highest_weapon_types = 65536;
/// The SIDEDATA sides 3.1c loads: SIDE0 to SIDE4.
inline constexpr uint32_t base_sides = 5;
/// The most SIDEDATA sides a mod may load.
inline constexpr uint32_t highest_sides = 32;

/// The unit limit: how many units each player may own.
struct UnitsPerPlayer {
    /// The limit a new game offers, in units per player.
    uint16_t default_limit{250};
    /// The lowest limit a stored setting or a host may choose.
    uint16_t minimum{20};
    /// The highest limit a stored setting or a host may choose.
    uint16_t maximum{500};
    /// The limit the unit-limit screen shows when the game's settings hold
    /// none, in units per player. Kept for the profile; read by nothing yet.
    uint16_t limits_screen_fallback{101}; /// Compares every field.
    bool operator==(const UnitsPerPlayer&) const = default;
};

/// The unit type bitsets: the type ids a same-type selection holds.
struct UnitTypes {
    /// Type ids the bitsets hold; a unit whose type id is at or past this is
    /// left out of them. A multiple of type_bits_step.
    uint32_t bitset_bits{base_type_bits};
    /// Only some of a mod's bitsets are widened. Kept for the profile: the
    /// engine's one type bitset, the same-type selection, always takes
    /// bitset_bits.
    bool partial_widening{}; /// Compares every field.
    bool operator==(const UnitTypes&) const = default;
};

/// The category masks: the unit types a named category (CATEGORY,
/// badTargetCategory, noChaseCategory) can hold. A unit whose type id is at
/// or past `types` belongs to no category.
struct CategoryMasks {
    /// Type ids every mask holds, a multiple of type_bits_step.
    uint32_t types{base_type_bits}; /// Compares every field.
    bool operator==(const CategoryMasks&) const = default;
};

/// The effect emitters: smoke, wakes, nano spray, flames and the teleport
/// trail. Visual only, but every spawn draws from the rand() stream.
struct Effects {
    /// Emitters each of the ten layers holds before queueing another
    /// evicts its oldest; a layer holds one more than this at most.
    uint32_t queue{400};
    /// Emitter slots the shared pool hands out, across every layer; a spawn
    /// that finds none free creates nothing and draws no random numbers.
    uint32_t reserve{1000}; /// Compares every field.
    bool operator==(const Effects&) const = default;
};

/// The path search's budget.
struct PathSearch {
    /// Path nodes the search may visit in a game tick, all players
    /// together, before the player's own multiplier.
    int32_t nodes{1333}; /// Compares every field.
    bool operator==(const PathSearch&) const = default;
};

/// What a builder's list does with CANBUILD entries past `copy`.
enum class BuildListOverflow : uint8_t {
    truncate, ///< the list keeps the first `copy` entries
    dynamic,  ///< the list keeps every entry, up to highest_build_list_copy
};

/// Returns the number of BuildListOverflow values, for code that reads one
/// by its position, such as a mod profile's.
///
/// @return the number of values
[[nodiscard]] constexpr size_t enum_size(BuildListOverflow) noexcept {
    return 2;
}

/// The builders' build lists (gamedata/sidedata.tdf [CANBUILD]), which
/// both the build menus and the computer players read.
struct BuildLists {
    /// CANBUILD entries a builder's list keeps. The download menus may add
    /// one entry more.
    uint32_t copy{30};
    /// What happens to the entries past `copy`.
    BuildListOverflow overflow{BuildListOverflow::truncate}; /// Compares every field.
    bool operator==(const BuildLists&) const = default;
};

/// The model composite buffer, the scratch image a unit's moving pieces,
/// build effect and carried units are drawn into. Visual only.
struct ModelComposite {
    int32_t width{600};  ///< pixels across at the start
    int32_t height{600}; ///< pixels down at the start
    /// A model larger than the buffer is cut to it, instead of the buffer
    /// growing to fit the model.
    bool clamp_oversize{}; /// Compares every field.
    bool operator==(const ModelComposite&) const = default;
};

/// The weapon ids a mod may number, 0 to `types` - 1.
struct WeaponTypes {
    /// Weapon ids numbered, from 0 up to one less than this.
    uint32_t types{base_weapon_types}; /// Compares every field.
    bool operator==(const WeaponTypes&) const = default;
};

/// The SIDEDATA sides read, SIDE0 up to `count` - 1.
struct Sides {
    /// Sides loaded, SIDE0 up to one less than this.
    uint32_t count{base_sides}; /// Compares every field.
    bool operator==(const Sides&) const = default;
};

/// Returns how many CANBUILD entries a builder's list keeps.
///
/// Both the unit table's build lists and the computer players' take their
/// size from this one count.
///
/// @param lists the build-list limits
/// @return `copy` when the overflow truncates; highest_build_list_copy when
///     the list keeps every entry
[[nodiscard]] constexpr uint32_t build_list_kept(const BuildLists& lists) noexcept {
    return lists.overflow == BuildListOverflow::dynamic ? highest_build_list_copy : lists.copy;
}

/// Every capacity a mod may raise, each defaulting to its 3.1c value.
struct Limits {
    UnitsPerPlayer units_per_player{};
    UnitTypes unit_types{};
    CategoryMasks category_masks{};
    Effects effects{};
    PathSearch path_search{};
    BuildLists build_lists{};
    ModelComposite model_composite{};
    WeaponTypes weapon_types{};
    Sides sides{}; /// Compares every field.
    bool operator==(const Limits&) const = default;
};

/// Which value check_limits found out of its range.
enum class LimitsError : uint8_t {
    none,
    units_per_player,      ///< a unit limit outside 1..highest_units_per_player, or
                           ///< default_limit outside minimum..maximum
    unit_type_bits,        ///< outside type_bits_step..highest_type_bits, or not a step multiple
    category_mask_types,   ///< outside type_bits_step..highest_type_bits, or not a step multiple
    effect_queue,          ///< outside 1..highest_effect_queue
    effect_reserve,        ///< outside 1..highest_effect_reserve
    path_search_nodes,     ///< outside 1..highest_path_search_nodes
    build_list_copy,       ///< outside 1..highest_build_list_copy
    build_list_overflow,   ///< not a BuildListOverflow
    model_composite_sides, ///< a side outside lowest_composite_side..highest_composite_side
    weapon_types,          ///< outside base_weapon_types..highest_weapon_types
    sides,                 ///< outside base_sides..highest_sides
};

/// Checks every value of a Limits record against its range.
///
/// @param limits the record to check
/// @return LimitsError::none when every value is in its range, else the
///     first value out of range, in the order of LimitsError
[[nodiscard]] LimitsError check_limits(const Limits& limits) noexcept;

/// Returns the words of 32 type bits that hold a count of type ids.
///
/// @param type_bits type ids to hold
/// @return type_bits / 32, rounded up
[[nodiscard]] constexpr uint32_t type_words(uint32_t type_bits) noexcept {
    return (type_bits + 31U) / 32U;
}

} // namespace oa::data::limits
