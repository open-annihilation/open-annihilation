// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/projectile.h"
#include "oa/core/weapon_def.h"
#include "oa/data/match_rules.hpp"
#include "oa/sim/ballistics.hpp"

#include <cstdint>

// What the weapons.* hacks and a weapon's own data keys change in the reach,
// aim and impact rules. A match without them passes 3.1c's records, and each
// function then gives what 3.1c does.
namespace oa::sim::weapon_execution {

/// Returns the launch solver's inputs for a weapon under the match's rules.
///
/// @param projectile_velocity WeaponDef.weapon_velocity
/// @param minimum_barrel_angle_radians WeaponDef.min_barrel_angle
/// @param simulation_gravity Game.gravity
/// @param rules the match's rules; weapons.high-arc-ballistic lets the solver lob
/// @return the solver's parameters
[[nodiscard]] inline sim::ballistics::BallisticParameters ballistic_parameters(
    int32_t projectile_velocity,
    float minimum_barrel_angle_radians,
    int32_t simulation_gravity,
    const data::match_rules::MatchRules& rules
) noexcept {
    return {
        projectile_velocity,
        minimum_barrel_angle_radians,
        simulation_gravity,
        rules.weapons.high_arc_ballistic.enabled
    };
}

/// Returns what a weapon's data keys change in the reach tests.
///
/// @param weapon the weapon's own rules
/// @return its not-to-air, surface-fire and not-to-underwater switches
[[nodiscard]] inline sim::ballistics::ReachKeys
reach_keys(const data::match_rules::WeaponTypeRules& weapon) noexcept {
    return {weapon.not_to_air, weapon.surface_fire, weapon.not_to_underwater};
}

/// Returns a weapon's reach-test inputs under the match's rules.
///
/// @param weapon the weapon definition; its whole id selects its own rules
/// @param simulation_gravity Game.gravity
/// @param rules the match's rules
/// @return flags, range, launch solver inputs and data-key switches
[[nodiscard]] inline sim::ballistics::WeaponReachParameters reach_parameters(
    const WeaponDef& weapon,
    int32_t simulation_gravity,
    const data::match_rules::MatchRulesView& rules
) noexcept {
    return {
        weapon.flags,
        weapon.range,
        ballistic_parameters(
            weapon.weapon_velocity, weapon.min_barrel_angle, simulation_gravity, rules.rules()
        ),
        reach_keys(rules.weapon(oa_weapon_def_id(&weapon)))
    };
}

/// Tests whether a projectile's impact skips its area blast and its minimap
/// mark (weapons.no-map-alert).
///
/// It does when the weapon has the key, the shot has no source unit and the
/// weapon's [DAMAGE] default is 0.
///
/// @param shot the projectile
/// @param weapon its weapon definition
/// @param keys the weapon's own rules
/// @return true when the blast and the mark are skipped
[[nodiscard]] inline bool silent_ownerless_shot(
    const Projectile& shot, const WeaponDef& weapon, const data::match_rules::WeaponTypeRules& keys
) noexcept {
    return keys.no_map_alert && shot.source == 0 && weapon.damage_default == 0;
}

} // namespace oa::sim::weapon_execution
