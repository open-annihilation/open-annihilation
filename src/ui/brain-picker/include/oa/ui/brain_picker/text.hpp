// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every word the brain seat picker shows, in English. A pattern's places are
// written {name}; a translation keeps them and may move them. The texts are
// looked up whole through the Library's TextHooks before their places are
// filled, so a translator finds every one of them here. Words the Library
// already has are its own, not copies.
#pragma once

#include "oa/ui/library/text.hpp"

#include <string_view>

namespace oa::ui::brain_picker {

namespace words {

/// The window's title.
inline constexpr std::string_view title_player = "Computer player";
/// The header's context: the seat and the mod, or No Mod.
inline constexpr std::string_view context_seat = "{seat} · {mod}";
/// The header's context when no mod is being played. The Library's word.
inline constexpr std::string_view no_mod = oa::ui::library::words::no_mod;
/// The built-in player in a seat's label, before its level.
inline constexpr std::string_view built_in = "Built-in";
/// The built-in player's name. The Library's word.
inline constexpr std::string_view built_in_player = oa::ui::library::words::built_in_brain_name;
/// The built-in player's line when it is not the choice.
inline constexpr std::string_view built_into = "Built into Open Annihilation";
/// A seat's label: the brain and its level. The built-in player fills
/// `{brain}` with `built_in`.
inline constexpr std::string_view brain_level = "{brain} · {level}";
/// A seat's label in the battle room: the brain, its version and its level.
inline constexpr std::string_view brain_version_level = "{brain} {version} · {level}";
/// The footer's summary while the choice applies to every computer seat.
/// `{label}` is the seat's label.
inline constexpr std::string_view every_seat = "{label} · every computer seat";
/// Under the built-in player's levels while the Difficulty button owns them.
inline constexpr std::string_view level_from_difficulty = "Set by the Difficulty button: {level}";
/// The level row of a seat's hover card, and the details' level label.
inline constexpr std::string_view level = "Level";
/// The version row of a brain's hover card.
inline constexpr std::string_view version = "Version";
/// The control that puts this choice on every computer seat.
inline constexpr std::string_view use_every_seat = "Use for every computer seat";
/// Full awareness on a seat's hover card. The Library's fact uses its longer
/// sentence; the card uses this shorter line.
inline constexpr std::string_view sees_every_unit = "Sees every unit on the map";
/// The hint when the seat's current brain cannot play, so the picker opened
/// on the built-in player. `{name}` is the brain's name, or its id.
inline constexpr std::string_view left_out =
    "{name} cannot play this seat now, so it starts on the built-in player.";
/// The question a saved game asks when a brain it names is not installed.
inline constexpr std::string_view load_built_in_title = "Load with the built-in player?";
/// One missing brain. `{n}` is the seat, counted from 1.
inline constexpr std::string_view saved_with_missing =
    "This game was saved with {name} {version} playing seat {n}, which is not installed here.";
/// What the built-in player does on the seats a missing brain left.
inline constexpr std::string_view built_in_takes_seats =
    "The built-in player takes those seats from the units as they stand. GET the brain in the "
    "Library to load the game as it was saved.";
/// BACK. The cancel button. The app's catalogue already lists this word.
inline constexpr std::string_view back = "BACK";
/// LOAD WITH THE BUILT-IN PLAYER. The primary button.
inline constexpr std::string_view load_built_in_button = "LOAD WITH THE BUILT-IN PLAYER";

} // namespace words

} // namespace oa::ui::brain_picker
