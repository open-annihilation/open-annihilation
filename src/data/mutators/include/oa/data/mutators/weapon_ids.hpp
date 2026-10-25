// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Weapon ids for the weapons a mutator run brought. The game's and the mod's
// weapons keep the ids they already have. A mutator weapon keeps a requested
// id when it is free, and otherwise takes the lowest free id from 1 to
// `weapon_types` - 1.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutation_rules/documents.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace oa::data::mutators {

/// One weapon a mutator brought, by a record file or by `weapons:clone`.
///
/// `assigned_id` is 0 until `assign_weapon_ids` succeeds. The name is the
/// weapon section's name, in the spelling the mutator wrote.
struct BroughtWeapon {
    /// Section name.
    std::string name;
    /// Index of the mutator that brought it, in the player's list.
    uint16_t mutator{};
    /// Document path the section lives in.
    std::string path;
    /// The id written onto the section when assignment succeeds. 0 before that.
    int assigned_id{};
};

/// Why weapon ids could not be assigned, or that they were.
struct WeaponIdResult {
    /// True when every brought weapon received an id.
    bool ok{true};
    /// Index of the mutator whose weapon could not be numbered.
    uint16_t mutator{};
    /// Name of the weapon that could not be numbered.
    std::string weapon;
};

/// Gives every brought weapon an id and writes it into the weapon section.
///
/// A live weapon section whose name is not in `brought` keeps its id and
/// occupies that slot when the id is from 1 to `weapon_types` - 1. A missing
/// id, an id of 0, an id past `weapon_types` - 1 and a value that does not
/// start with a number occupy nothing. The brought weapons are taken one
/// mutator at a time, in list order, and within a mutator by name without
/// ASCII case, then by the raw name. That mutator's requested ids, from 1
/// to `weapon_types` - 1, are kept when they are still free. Its other
/// weapons then take the lowest free id from 1 to `weapon_types` - 1. A
/// later mutator's request for an id an earlier weapon already holds takes
/// another free id. Slot 0 is never given out. Two brought weapons never
/// share an id.
///
/// Every id is chosen before any section is written. When one weapon has no
/// free id, nothing is written, `result.ok` is false, and `result` names
/// that weapon and its mutator. The first such weapon in the order above is
/// the one named, so the mutator that overflows is the one refused. On
/// success each section's `ID=` is the assigned id, as a decimal, and
/// `assigned_id` is set.
///
/// @param documents the run's documents; brought sections are edited on success
/// @param layout the mod's directory names
/// @param brought the weapons mutators added; updated with the assigned ids
/// @param result set to the refusal, or left ok on success
/// @param weapon_types how many weapon ids the mod numbers. Requests and free
///     ids run from 1 to `weapon_types` - 1. 256 unless the caller read the
///     profile's `limits.weapon-types`.
/// @return false when a brought weapon could not be numbered
[[nodiscard]] bool assign_weapon_ids(
    oa::data::mutation_rules::DocumentSet& documents,
    const oa::data::defs::DataLayout& layout,
    std::span<BroughtWeapon> brought,
    WeaponIdResult& result,
    uint32_t weapon_types = 256
);

} // namespace oa::data::mutators
