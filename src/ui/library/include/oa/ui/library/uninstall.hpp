// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// UNINSTALL in an installed package's details: when the button is off and
// why, what pressing it leads to (the question, or a notice that the
// package is in use), and the kit question and notices the app shows over
// the Library. The removal itself is the app's. Its words are in text.hpp.
#pragma once

#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/library.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::ui::library {

/// The Uninstall question's CANCEL button's name to automation, which
/// removes nothing. It is the first button and the one the keys mark at first.
inline constexpr std::string_view uninstall_cancel_button_id = "cancel";
/// The Uninstall question's UNINSTALL button's name to automation, which
/// removes the package. It has the primary look.
inline constexpr std::string_view uninstall_button_id = "uninstall";
/// The name to automation of the OK that closes an uninstall's notice.
inline constexpr std::string_view uninstall_ok_button_id = "ok";

/// What pressing an entry's UNINSTALL leads to.
enum class UninstallStep : uint8_t {
    off,              ///< nothing: it is not installed, or uninstall_off_reason says why
    ask,              ///< the Uninstall question, whose UNINSTALL removes it
    switch_off_first, ///< the mod being played: it is switched off first, or a notice says so
    language_first,   ///< the language pack in use: a notice to choose another language first
};

/// Why an uninstall removed nothing, as its notice says it.
enum class UninstallProblem : uint8_t {
    busy,      ///< the folder is being changed, here or by another copy of the game
    changed,   ///< the folder no longer holds what the Library listed
    link,      ///< the folder is a link, which is never followed
    outside,   ///< the folder is not one of the player's own Mods, Maps or Languages folders
    not_moved, ///< the folder could not be moved aside
};

/// Writes why an entry's UNINSTALL is off: a map pack's while a game is
/// played, and any package's while it is in the download queue, until it
/// fails or is installed (CANCEL comes first).
///
/// @param library the Library
/// @param entry one of its entries
/// @return the reason; empty when UNINSTALL is on, or the entry is not installed
[[nodiscard]] std::string uninstall_off_reason(const Library& library, const Entry& entry);

/// Tells what pressing an entry's UNINSTALL leads to: nothing while it is
/// off, a notice for the mod being played (Installed::playing) and for a
/// language pack in use (Installed::in_use), which are never removed, and
/// the question for every other installed package.
///
/// @param library the Library
/// @param entry one of its entries
/// @return the step
[[nodiscard]] UninstallStep uninstall_step(const Library& library, const Entry& entry);

/// Makes the Uninstall question: "Uninstall {name}", asking "Uninstall
/// {name}? Its folder, {folder}, is removed.", adding "…, and with it the
/// copy ROLL BACK keeps." when one is kept, then "GET installs it again."
/// for a package a registry lists, or that the Library cannot get the
/// player's own again; then CANCEL (`cancel`) and UNINSTALL (`uninstall`).
/// CANCEL is marked at first and answers Escape, B and N; UNINSTALL has the
/// primary look, and Y answers it. Its word is the kit's default, so
/// automation names its buttons prompt.cancel and prompt.uninstall.
///
/// @param entry the entry, installed
/// @param folder its folder as the player knows it, such as "Mods/ridge"
/// @param text how texts are looked up
/// @return the question
[[nodiscard]] oa::ui::kit::Question
uninstall_question(const Entry& entry, std::string_view folder, const TextHooks& text = {});

/// The Uninstall question, and, when other installed mutators need this one,
/// a paragraph before the last saying they stay installed and cannot be
/// switched on until it is back. The buttons and the removal are unchanged.
///
/// @param library the Library, whose installed mutators are the dependants
/// @param entry the entry, installed
/// @param folder its folder as the player knows it, such as "Mutators/gatehouse"
/// @param text how texts are looked up
/// @return the question
[[nodiscard]] oa::ui::kit::Question uninstall_question(
    const Library& library, const Entry& entry, std::string_view folder, const TextHooks& text = {}
);

/// The Uninstall question for the mod being played, whose UNINSTALL switches
/// it off before it removes its folder: "You are playing {name}. UNINSTALL
/// switches it off first, and the game starts again with no mod." before the
/// question's own paragraphs. The buttons are the question's.
///
/// @param library the Library
/// @param entry the entry, installed and played
/// @param folder its folder as the player knows it, such as "Mods/ridge"
/// @param text how texts are looked up
/// @return the question
[[nodiscard]] oa::ui::kit::Question uninstall_playing_question(
    const Library& library, const Entry& entry, std::string_view folder, const TextHooks& text = {}
);

/// Makes the notice UNINSTALL shows instead of its question for a package
/// in use, which removes nothing: for the mod being played, that SWITCH OFF
/// comes first; for the language pack in use, that another language is
/// chosen in Settings › Language first. Its one button is OK (`ok`), which
/// Enter, Escape and Y answer.
///
/// @param entry the entry, installed
/// @param step switch_off_first or language_first; any other is taken as
///     switch_off_first
/// @param text how texts are looked up
/// @return the notice, as a kit question of one button
[[nodiscard]] oa::ui::kit::Question
uninstall_notice(const Entry& entry, UninstallStep step, const TextHooks& text = {});

/// Makes the notice that an uninstall removed nothing, and why, with OK (`ok`).
///
/// @param name the package's name as the player sees it
/// @param problem why
/// @param text how texts are looked up
/// @return the notice, as a kit question of one button
[[nodiscard]] oa::ui::kit::Question uninstall_failed_notice(
    std::string_view name, UninstallProblem problem, const TextHooks& text = {}
);

/// Writes the note under the selected entry's buttons: playing_note's,
/// then, while the entry's UNINSTALL is off, why (uninstall_off_reason),
/// then, for a mutator whose GET brings prerequisites, that they download
/// first and sit above it. A blocked plan adds its reason when the status
/// line would not already show it.
///
/// @param library the Library
/// @return the note; empty when it has none
[[nodiscard]] std::string details_note(const Library& library);

} // namespace oa::ui::library
