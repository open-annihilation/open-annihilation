// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether the documents a pipeline pass is about to serve are a game the
// engine can load. A problem the mod already had is not a problem a mutator
// caused. A problem that cannot hurt the match is a warning.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/journal.hpp"
#include "oa/data/mutation_rules/documents.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mutators {

/// How many unit files a shared game can keep in step.
///
/// The battle room's unit-content table holds the same number. The app pins
/// the two with a static assert; this module does not include that header.
inline constexpr uint32_t unit_sync_limit = 1024;

/// The limits one integrity check uses. The pipeline input carries them.
struct IntegrityLimits {
    /// Unit types the category masks hold. Later type ids silently lose
    /// their categories. 512 unless the caller read another width.
    uint32_t category_mask_types{512};
    /// Weapon IDs 0 to `weapon_types` - 1 take a slot. Free ids are 1 to
    /// `weapon_types` - 1. 256 unless the caller read the profile's
    /// `limits.weapon-types`.
    uint32_t weapon_types{256};
    /// True when the match is a shared game. Past `unit_sync_limit` then
    /// fails the mutator; otherwise it is a warning.
    bool shared{false};
};

/// One way the documents are not a game the engine can load.
enum class Problem : uint8_t {
    menu_names_missing_unit,     ///< a download entry names a unit that is not there
    gui_edit_names_missing_page, ///< a build page a mutator changed is not a file the game holds
    weapon_missing,            ///< weapon1, weapon2 or weapon3 names no weapon that holds its slot
    corpse_missing,            ///< corpse names no feature
    explosion_missing,         ///< explodeas or selfdestructas names no weapon that holds its slot
    movement_class_missing,    ///< movementclass names no class the game reads
    model_missing,             ///< the unit's model file is not in the store
    script_missing,            ///< the unit has no script; it can still play
    side_without_commander,    ///< a side the game reads has no commander, or not a live unit
    too_many_sides,            ///< a side section past the five the game reads
    too_many_movement_classes, ///< a class section past the 32 the game reads
    weapon_id_out_of_range, ///< a weapon's ID is outside 0 to weapon_types - 1, so it takes no slot
    too_many_unit_types,    ///< more unit files than the type table can hold
    past_category_masks,    ///< more unit files than the category masks hold
    past_unit_sync,         ///< more unit files than a shared game can keep in step
    file_name_differs,      ///< a unit file's name is not its UnitName
    unit_name_twice,        ///< two unit files share one UnitName
    too_many_download_files, ///< more download files than the game reads, including ones the menus added
};

/// One problem in one state of the documents.
///
/// `severity` is `warning` when the match can still start, and `failure`
/// when the pipeline must leave a mutator out. `record`, `field` and `target`
/// are the comparison key, together with `problem`. A count problem leaves
/// `record`, `field` and `target` empty, so a mod already past that limit
/// matches a result that is further past it. `mutator` is empty until
/// `new_issues` fills it. `message` is one English sentence.
struct IntegrityIssue {
    /// Which problem.
    Problem problem{};
    /// A warning the report keeps, or a failure that leaves the mutator out.
    Severity severity{};
    /// The record that holds the reference: a unit name, a side, a class or a menu section.
    std::string record;
    /// The field, lower case. Empty when the problem is not one field.
    std::string field;
    /// What the reference names, or the other file when two units share a name.
    std::string target;
    /// The document, or the model or script path.
    std::string path;
    /// The mutator that caused the problem, once `new_issues` has blamed one.
    std::optional<uint16_t> mutator;
    /// One English sentence a player can read.
    std::string message;
};

/// Every problem in one state of the documents.
///
/// `store_holds` is the store's view of one path: true when that file is
/// present and not empty. The base check calls it with no mutator layer
/// mounted. The result check calls it with the files layer mounted and
/// with the files the pass is about to serve. Directory names come from
/// `layout`, the same layout the pipeline parsed with.
///
/// @param documents the parsed documents, removed ones included
/// @param layout the pipeline's data layout
/// @param limits category width, weapon types and whether the match is shared
/// @param store_holds true when the store holds that path
/// @return the problems, in the order the check walks the documents
[[nodiscard]] std::vector<IntegrityIssue> integrity_issues(
    const oa::data::mutation_rules::DocumentSet& documents,
    const oa::data::defs::DataLayout& layout,
    const IntegrityLimits& limits,
    const std::function<bool(std::string_view)>& store_holds
);

/// The problems in `result` that `base` does not already have.
///
/// Two issues are the same when `problem`, `record`, `field` and `target`
/// match with ASCII case folded. A reference problem (a weapon, explosion,
/// corpse, movement class or menu unit that nothing holds) is the base
/// data's own error when the base already has that problem, field and
/// target on any record, so it is not new. Each kept issue has `mutator`
/// set from the journal and the list order: the mutator that last wrote the field
/// (or added the menu entry); else the one that removed, renamed or
/// re-numbered what the reference names; else the last in list order that
/// wrote the record. A count problem names the last mutator that added a
/// unit or a download file, or the last mutator the journal names. The
/// journal is ordered here, so the caller's order does not change the blame.
///
/// @param base problems before any mutator ran
/// @param result problems after the last hook
/// @param journal the pass's changes; it need not be ordered
/// @param layout the pipeline's data layout, used to recognise a download file
/// @return the new problems, with `mutator` filled when the journal names one
[[nodiscard]] std::vector<IntegrityIssue> new_issues(
    std::span<const IntegrityIssue> base,
    std::span<const IntegrityIssue> result,
    const Journal& journal,
    const oa::data::defs::DataLayout& layout
);

} // namespace oa::data::mutators
