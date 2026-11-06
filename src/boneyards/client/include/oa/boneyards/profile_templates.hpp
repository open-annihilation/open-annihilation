// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Profile.def: the ladder page templates, the profile category names and
// descriptions and the two player-page templates, read through Rover's
// record-file parser the first time a profile record is built.
#pragma once

#include "oa/boneyards/service.hpp"
#include "oa/rover/interface.h"

#include <cstdint>

namespace oa::boneyards {

// Where the game reads the definitions, relative to its install directory.
inline constexpr const char* kProfileDefinitionsPath = "./boneyards/Profile/Profile.def";

/// Stores a ladder template's type, subtype and sort order and reads its page.
///
/// @param[out] ladder template to fill
/// @param type ladder type from the record's bag
/// @param subtype ladder subtype from the record's bag
/// @param sort sort order of the ladder's page
/// @param file path of the page, relative to the install directory
/// @return `ladder`, whose page is null when the file cannot be read
LadderTemplate* ladder_template_init(
    LadderTemplate* ladder, int32_t type, int32_t subtype, int32_t sort, const char* file
);

/// Files one Profile.def record into the template tables.
///
/// A ladder_template whose page reads becomes a ladder, a
/// profile_category_info sets a category name and description, and the
/// byinfo and popup templates read their pages. Other commands are ignored.
/// A category id outside 0..6 drops the record.
///
/// @param command record command, which selects the table
/// @param id category index of a profile_category_info record (0..6)
/// @param name category name of a profile_category_info record
/// @param sort ladder sort order, as decimal text
/// @param category record category; not read
/// @param headline record headline; not read
/// @param file page file a template record reads
/// @param props record fields (a ladder's type and subtype, a category's description)
/// @param user the ProfileTemplates being filled
/// @return true always, so parsing goes on to the next record
bool ROVER_CALL profile_template_record(
    const char* command,
    const char* id,
    const char* name,
    const char* sort,
    const char* category,
    const char* headline,
    const char* file,
    RoverPropertyBag* props,
    void* user
);

/// Reads the template tables from Profile.def the first time they are asked for.
///
/// The tables count as loaded before the file is parsed, so a failed read is
/// not retried.
///
/// @return the tables
const ProfileTemplates* profile_templates_load();

/// Frees every template table and marks them unread.
void profile_templates_release();

} // namespace oa::boneyards
