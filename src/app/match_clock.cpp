// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "match_clock.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/ui/console/game_fields.hpp"

namespace oa::app {

bool match_clock_runs(bool shared_match, bool menu_open, bool finished) noexcept {
    if (shared_match)
        return true;
    return !menu_open && !finished;
}

uint16_t clock_flags_with_pause(uint16_t clock_flags, uint16_t sim_run_flags) noexcept {
    constexpr uint16_t paused = oa::ui::console::kSimRunPaused;
    return static_cast<uint16_t>((clock_flags & ~paused) | (sim_run_flags & paused));
}

bool clock_reading_behind(uint32_t reading, uint32_t previous_clock) noexcept {
    return oa::base::game_loop::scaled_clock_before(reading, previous_clock);
}

AwayAction away_action(const AwayFacts& facts) noexcept {
    if (!facts.match_shown || facts.finished || facts.network || facts.replay || facts.scripted)
        return AwayAction::none;
    return facts.pause_chosen ? AwayAction::pause : AwayAction::keep_playing;
}

} // namespace oa::app
