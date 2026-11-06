// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards box "Exit to the system?", which a request to end the program
// (the window's close button, or the system's quit) opens while the
// Boneyards has it installed: from its entry, or its return after a battle
// it launched, until the game next sets or changes its application mode or
// a battle is launched. The Boneyards answers such a request first
// (Extension::close_requested); network play's close handlers answer the
// rest.
#pragma once

#include <cstdint>

namespace oa::app {

// Whether the Boneyards box is installed, and what it follows.
struct ExitPrompt {
    bool installed{};
    int32_t mode{};  // the application mode the game was last seen in
    bool in_match{}; // a match has started since the last seen end of one
    bool ready{};    // the Boneyards was last seen up outside a battle it launched
};

// What the box follows, seen at a frame, at a request or as a mode is set.
struct ExitPromptObservation {
    int32_t frontend_mode{}; // the frontend Game's application mode (Game.mode)
    bool boneyards_up{};     // the Boneyards is up (boneyards_status_widget_up)
    bool launched{};         // a launch is active (oa::app::netgame::extension_api::launch_active)
};

/// Follows the game into the box, as network play's close handlers follow it into theirs.
///
/// A match started (exit_prompt_match_started) puts the game in
/// mode_id::in_match until exit_prompt_match_ended; otherwise the mode is
/// the frontend Game's. A change of mode installs the box while the
/// Boneyards is up outside a battle it launched and removes it otherwise;
/// the Boneyards coming up outside such a battle (its entry, or the return
/// to it after a launched battle) installs it. Anything else keeps it, so
/// after the Boneyards' EXIT the main menu still asks "Exit to the system?"
/// until the game next sets a mode (exit_prompt_mode_set), even the one it
/// runs.
///
/// @param[in,out] prompt the box and what it last saw
/// @param now the game's state
void exit_prompt_observe(ExitPrompt& prompt, const ExitPromptObservation& now) noexcept;

/// Follows an application mode the game sets: the box stays installed only while the Boneyards is up outside a battle it launched.
///
/// Outside a match the mode set becomes the one exit_prompt_observe
/// compares with; a match keeps mode_id::in_match until
/// exit_prompt_match_ended.
///
/// @param[in,out] prompt the box and what it last saw
/// @param mode the application mode set (oa::ui::frontend_state::mode_id values)
/// @param now the game's state as the mode is set, the mode already written
void exit_prompt_mode_set(
    ExitPrompt& prompt, int32_t mode, const ExitPromptObservation& now
) noexcept;

/// Notes a match started: the game counts as in a match until exit_prompt_match_ended.
///
/// @param[in,out] prompt the box and what it last saw
void exit_prompt_match_started(ExitPrompt& prompt) noexcept;

/// Notes a match finished, left or torn down: the next observation follows the frontend Game's mode.
///
/// @param[in,out] prompt the box and what it last saw
void exit_prompt_match_ended(ExitPrompt& prompt) noexcept;

/// Answers a request to end the program with the box (Extension::close_requested).
///
/// The game is followed first (exit_prompt_observe); while the box is then
/// installed, `open_box` is asked to open it. The request falls to network
/// play's close handler when the box is not installed or cannot open (the
/// Boneyards' interface is down, as once the main menu has been left for
/// another screen); a box that cannot open stays installed for the next
/// request.
///
/// @param[in,out] prompt the box and what it last saw
/// @param now the game's state at the request
/// @param open_box opens "Exit to the system?"; returns false when it cannot open
/// @return true when the box opened
bool exit_prompt_close_requested(
    ExitPrompt& prompt, const ExitPromptObservation& now, bool (*open_box)()
);

/// Removes the box as a battle the Boneyards launches is applied, which installs network play's exit confirmation.
///
/// @param[in,out] prompt the box and what it last saw
void exit_prompt_launch_applied(ExitPrompt& prompt) noexcept;

} // namespace oa::app
