// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards box "Exit to the system?" as the game keeps it installed
// (boneyards_exit_prompt.hpp).
#include "boneyards_exit_prompt.hpp"

#include "oa/ui/frontend_state/dispatcher.hpp"

namespace oa::app {

namespace {

/// Tells whether the Boneyards is up outside a battle it launched.
///
/// @param now the game's state
/// @return true while it is
bool ready(const ExitPromptObservation& now) noexcept {
    return now.boneyards_up && !now.launched;
}

} // namespace

void exit_prompt_observe(ExitPrompt& prompt, const ExitPromptObservation& now) noexcept {
    const int32_t mode =
        prompt.in_match ? oa::ui::frontend_state::mode_id::in_match : now.frontend_mode;
    const bool up = ready(now);
    if (mode != prompt.mode) {
        prompt.mode = mode;
        prompt.installed = up;
    }
    if (up && !prompt.ready)
        prompt.installed = true;
    prompt.ready = up;
}

void exit_prompt_mode_set(
    ExitPrompt& prompt, int32_t mode, const ExitPromptObservation& now
) noexcept {
    exit_prompt_observe(prompt, now);
    if (!prompt.in_match)
        prompt.mode = mode;
    prompt.installed = ready(now);
}

bool exit_prompt_close_requested(
    ExitPrompt& prompt, const ExitPromptObservation& now, bool (*open_box)()
) {
    exit_prompt_observe(prompt, now);
    return prompt.installed && open_box();
}

void exit_prompt_match_started(ExitPrompt& prompt) noexcept {
    prompt.in_match = true;
}

void exit_prompt_match_ended(ExitPrompt& prompt) noexcept {
    prompt.in_match = false;
}

void exit_prompt_launch_applied(ExitPrompt& prompt) noexcept {
    prompt.installed = false;
}

} // namespace oa::app
