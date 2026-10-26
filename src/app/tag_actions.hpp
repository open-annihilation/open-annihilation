// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Category words a player can bind, taken from a category registry.
#pragma once

#include "oa/data/defs/categories.hpp"

#include <string>
#include <vector>

namespace oa::app {

/// One Category word and the action that selects it.
struct TagWord {
    std::string word{};   ///< the data's spelling
    std::string action{}; ///< the action id `tag_action_id` gives the word
};

/// The Category words whose masks hold a unit type, apart from ALL.
///
/// The words stay in the registry's order. A word that makes no action id,
/// and a later word that would reuse an id, is left out and named in
/// `skipped`. An empty mask, such as a name only a bad-target line adds, is
/// left out and not named.
///
/// @param registry the categories to read; not changed
/// @param skipped words left out, appended in registry order
/// @return the words that select something
[[nodiscard]] std::vector<TagWord>
tag_words(const oa::data::defs::CategoryRegistry& registry, std::vector<std::string>& skipped);

} // namespace oa::app
