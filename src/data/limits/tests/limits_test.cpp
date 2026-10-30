// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Limits record: its defaults are 3.1c's values, check_limits accepts
// every value from the lowest to the highest a profile may name and refuses
// each one just outside.

#include "oa/data/limits.hpp"
#include "oa/test/check.hpp"

namespace {

using namespace oa::data::limits;

/// Returns a record with every capacity at the highest value a profile may name.
///
/// @return the record
Limits highest_limits() {
    Limits limits;
    limits.units_per_player = {
        highest_units_per_player,
        highest_units_per_player,
        highest_units_per_player,
        highest_units_per_player
    };
    limits.unit_types.bitset_bits = highest_type_bits;
    limits.category_masks.types = highest_type_bits;
    limits.effects = {highest_effect_queue, highest_effect_reserve};
    limits.path_search.nodes = highest_path_search_nodes;
    limits.build_lists = {highest_build_list_copy, BuildListOverflow::dynamic};
    limits.model_composite = {highest_composite_side, highest_composite_side, true};
    limits.weapon_types.types = highest_weapon_types;
    limits.sides.count = highest_sides;
    return limits;
}

void test_defaults_are_the_base_game() {
    const Limits limits;
    OA_CHECK(limits.units_per_player.default_limit == 250);
    OA_CHECK(limits.units_per_player.minimum == 20);
    OA_CHECK(limits.units_per_player.maximum == 500);
    OA_CHECK(limits.units_per_player.limits_screen_fallback == 101);
    OA_CHECK(limits.unit_types.bitset_bits == 512 && !limits.unit_types.partial_widening);
    OA_CHECK(limits.category_masks.types == 512);
    OA_CHECK(limits.effects.queue == 400 && limits.effects.reserve == 1000);
    OA_CHECK(limits.path_search.nodes == 1333);
    OA_CHECK(limits.build_lists.copy == 30);
    OA_CHECK(limits.build_lists.overflow == BuildListOverflow::truncate);
    OA_CHECK(limits.model_composite.width == 600 && limits.model_composite.height == 600);
    OA_CHECK(!limits.model_composite.clamp_oversize);
    OA_CHECK(limits.weapon_types.types == 256);
    OA_CHECK(limits.sides.count == 5);
    OA_CHECK(check_limits(limits) == LimitsError::none);
}

void test_highest_values_pass() {
    OA_CHECK(check_limits(highest_limits()) == LimitsError::none);
    // Ten players' slots and the reserved one still fit 16-bit unit ids.
    OA_CHECK(uint32_t{highest_units_per_player} * 10 + 1 <= 0xffffU);
    OA_CHECK(type_words(highest_type_bits) == 2048 && type_words(512) == 16);
}

void test_each_value_just_outside_fails() {
    Limits limits = highest_limits();
    limits.units_per_player.maximum = highest_units_per_player + 1;
    OA_CHECK(check_limits(limits) == LimitsError::units_per_player);
    limits = {};
    limits.units_per_player.minimum = 251;
    OA_CHECK(check_limits(limits) == LimitsError::units_per_player);
    limits = {};
    limits.units_per_player.minimum = 0;
    OA_CHECK(check_limits(limits) == LimitsError::units_per_player);

    limits = {};
    limits.unit_types.bitset_bits = highest_type_bits + type_bits_step;
    OA_CHECK(check_limits(limits) == LimitsError::unit_type_bits);
    limits.unit_types.bitset_bits = 1000;
    OA_CHECK(check_limits(limits) == LimitsError::unit_type_bits);

    limits = {};
    limits.category_masks.types = 256;
    OA_CHECK(check_limits(limits) == LimitsError::category_mask_types);

    limits = {};
    limits.effects.queue = highest_effect_queue + 1;
    OA_CHECK(check_limits(limits) == LimitsError::effect_queue);
    limits = {};
    limits.effects.reserve = 0;
    OA_CHECK(check_limits(limits) == LimitsError::effect_reserve);

    limits = {};
    limits.path_search.nodes = highest_path_search_nodes + 1;
    OA_CHECK(check_limits(limits) == LimitsError::path_search_nodes);

    limits = {};
    limits.build_lists.copy = highest_build_list_copy + 1;
    OA_CHECK(check_limits(limits) == LimitsError::build_list_copy);
    limits = {};
    limits.build_lists.overflow = static_cast<BuildListOverflow>(2);
    OA_CHECK(check_limits(limits) == LimitsError::build_list_overflow);

    limits = {};
    limits.model_composite.height = lowest_composite_side - 1;
    OA_CHECK(check_limits(limits) == LimitsError::model_composite_sides);

    limits = {};
    limits.weapon_types.types = 255;
    OA_CHECK(check_limits(limits) == LimitsError::weapon_types);
    limits = {};
    limits.weapon_types.types = 65537;
    OA_CHECK(check_limits(limits) == LimitsError::weapon_types);

    limits = {};
    limits.sides.count = 4;
    OA_CHECK(check_limits(limits) == LimitsError::sides);
    limits = {};
    limits.sides.count = 33;
    OA_CHECK(check_limits(limits) == LimitsError::sides);

    limits = {};
    limits.weapon_types.types = 255;
    limits.sides.count = 4;
    OA_CHECK(check_limits(limits) == LimitsError::weapon_types);
}

} // namespace

int main() {
    test_defaults_are_the_base_game();
    test_highest_values_pass();
    test_each_value_just_outside_fails();
    return oa::test::check_exit_status();
}
