// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/limits.hpp"

namespace oa::data::limits {
namespace {

/// Tests a unit limit against the range every limit keeps.
///
/// @param limit units per player
/// @return true for 1..highest_units_per_player
[[nodiscard]] bool unit_limit_in_range(uint16_t limit) noexcept {
    return limit >= 1 && limit <= highest_units_per_player;
}

/// Tests a type-bit count against the range of type bitsets and category masks.
///
/// @param type_bits type ids held
/// @return true for a multiple of type_bits_step from type_bits_step to highest_type_bits
[[nodiscard]] bool type_bits_in_range(uint32_t type_bits) noexcept {
    return type_bits >= type_bits_step && type_bits <= highest_type_bits &&
           type_bits % type_bits_step == 0;
}

/// Tests a composite side against its range.
///
/// @param side pixels
/// @return true for lowest_composite_side..highest_composite_side
[[nodiscard]] bool composite_side_in_range(int32_t side) noexcept {
    return side >= lowest_composite_side && side <= highest_composite_side;
}

} // namespace

LimitsError check_limits(const Limits& limits) noexcept {
    const UnitsPerPlayer& units = limits.units_per_player;
    if (!unit_limit_in_range(units.default_limit) || !unit_limit_in_range(units.minimum) ||
        !unit_limit_in_range(units.maximum) || !unit_limit_in_range(units.limits_screen_fallback) ||
        units.minimum > units.default_limit || units.default_limit > units.maximum)
        return LimitsError::units_per_player;
    if (!type_bits_in_range(limits.unit_types.bitset_bits))
        return LimitsError::unit_type_bits;
    if (!type_bits_in_range(limits.category_masks.types))
        return LimitsError::category_mask_types;
    if (limits.effects.queue < 1 || limits.effects.queue > highest_effect_queue)
        return LimitsError::effect_queue;
    if (limits.effects.reserve < 1 || limits.effects.reserve > highest_effect_reserve)
        return LimitsError::effect_reserve;
    if (limits.path_search.nodes < 1 || limits.path_search.nodes > highest_path_search_nodes)
        return LimitsError::path_search_nodes;
    if (limits.build_lists.copy < 1 || limits.build_lists.copy > highest_build_list_copy)
        return LimitsError::build_list_copy;
    if (limits.build_lists.overflow != BuildListOverflow::truncate &&
        limits.build_lists.overflow != BuildListOverflow::dynamic)
        return LimitsError::build_list_overflow;
    if (!composite_side_in_range(limits.model_composite.width) ||
        !composite_side_in_range(limits.model_composite.height))
        return LimitsError::model_composite_sides;
    if (limits.weapon_types.types < base_weapon_types ||
        limits.weapon_types.types > highest_weapon_types)
        return LimitsError::weapon_types;
    if (limits.sides.count < base_sides || limits.sides.count > highest_sides)
        return LimitsError::sides;
    return LimitsError::none;
}

} // namespace oa::data::limits
