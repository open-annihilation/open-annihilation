// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Helpers the brain picker's model and its texts share. Not a public header.
#pragma once

#include "oa/ui/brain_picker/model.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::brain_picker {
namespace detail {

/// Finds the brain with `id` among the inputs.
///
/// An empty id is the built-in player, which is not an entry, so this
/// returns nothing for it. A brain with an empty id in the inputs is
/// skipped. The first matching id is kept.
///
/// @param picker the picker
/// @param id the brain's id
/// @return the brain, or nothing
const Brain* find_brain(const Picker& picker, std::string_view id);

/// Tells whether `id` is a row the list shows.
///
/// Empty is the built-in player, which is always listed. Any other id is
/// listed when the inputs hold it.
///
/// @param picker the picker
/// @param id the row's id
/// @return true when the row is listed
bool listed(const Picker& picker, std::string_view id);

/// Tells whether the brain can play this seat.
///
/// @param brain the brain
/// @return true when `unfit` is empty
bool can_play(const Brain& brain);

/// Tells whether `level` is one of the built-in player's three levels.
///
/// @param level the level id
/// @return true for `easy`, `medium` or `hard`
bool built_in_level(std::string_view level);

/// The id of a difficulty, `easy`, `medium` or `hard`.
///
/// @param difficulty the difficulty
/// @return the id
std::string_view difficulty_id(data::brain_library::Difficulty difficulty);

/// The built-in player's level when a choice names one that is not its own.
///
/// `setup_difficulty` while the Difficulty button owns the level, or when
/// `current` is not `easy`, `medium` or `hard`. Otherwise `current`.
///
/// @param fixed true when the Difficulty button owns the level
/// @param setup the setup's difficulty
/// @param current the level the seat remembered
/// @return the level id
std::string
opened_built_in_level(bool fixed, data::brain_library::Difficulty setup, std::string_view current);

/// The name the built-in player shows for `level`.
///
/// The three names are the inputs' Easy, Medium and Hard. Any other id is
/// returned as written.
///
/// @param inputs the host's inputs
/// @param level the level id
/// @return the name
std::string built_in_level_name(const Inputs& inputs, std::string_view level);

/// The index of `level` in the brain's levels.
///
/// @param brain the brain
/// @param level the level id
/// @return the index, or the level count when the brain has no such level
std::size_t level_index(const Brain& brain, std::string_view level);

/// The level id `default_level` picks for the setup's difficulty.
///
/// @param brain the brain
/// @param wanted the difficulty asked for
/// @return the level id; empty when the brain has no levels
std::string default_level_id(const Brain& brain, data::brain_library::Difficulty wanted);

/// The name a brain shows for `level`.
///
/// The level's name, or its id when the name is empty. `level` itself when
/// the brain has no such level.
///
/// @param brain the brain
/// @param level the level id
/// @return the name
std::string level_name(const Brain& brain, std::string_view level);

/// The text an option keeps for `value`, when the option allows it.
///
/// A choice or a toggle keeps one of its values, compared as written. A
/// number keeps one of `number_values`, compared as that canonical text.
///
/// @param spec the option
/// @param value the value as given
/// @return the text to keep, or nothing when the value is refused
std::optional<std::string>
kept_option(const mutator_picker::OptionSpec& spec, std::string_view value);

/// The default an option starts from, one the option allows when it has one.
///
/// A number whose fallback is one of `number_values` uses it. Otherwise the
/// first value that list offers. A choice or a toggle whose fallback is one
/// of its values uses it, and otherwise the first value.
///
/// @param spec the option
/// @return the default
std::string default_option(const mutator_picker::OptionSpec& spec);

/// The options a brain holds, in its order.
///
/// An unknown id is dropped. A value that is not allowed, and an option the
/// choice does not name, take the default.
///
/// @param brain the brain
/// @param given the values to keep, when they are allowed
/// @return the options
std::vector<std::pair<std::string, std::string>> normalize_options(
    const Brain& brain, const std::vector<std::pair<std::string, std::string>>& given
);

} // namespace detail
} // namespace oa::ui::brain_picker
