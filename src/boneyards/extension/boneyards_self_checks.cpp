// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' --check-* runs, called from the extension's run_mode hook.
// Compiled only when the game's self-checks are built. The checks' header
// arrives through the link to oa-boneyards-checks. A release build leaves
// this file out and does not name that module's directory.
#include "boneyards_check.hpp"
#include "boneyards_options.hpp"
#include "boneyards_strip_click_check.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"

namespace oa::app {

/// Runs the Boneyards checks at the points Runtime::run offers them.
///
/// At start --check-boneyards-login, or --check-boneyards-unit-limit, which
/// applies that limit from the Profile and reads it back from the engine's
/// unit limit preference. Headless, --check-strip-buttons clicks the strip;
/// --enter-boneyards follows, and writes preferences.
///
/// @param runtime The running app.
/// @param phase Point of Runtime::run offering the run.
/// @param[out] exit_code Receives the check's exit code when one ran.
/// @return True when a check ran, which ends Runtime::run.
bool run_boneyards_self_checks(Runtime& runtime, RunPhase phase, int& exit_code) {
    const auto& options = boneyards_options();
    switch (phase) {
    case RunPhase::start:
        if (options.check_boneyards_unit_limit) {
            exit_code = check_profile_unit_limit(runtime, *options.check_boneyards_unit_limit);
            return true;
        }
        if (!options.check_boneyards_login)
            return false;
        exit_code = check_boneyards_login(check_host(runtime));
        return true;
    case RunPhase::headless_first:
        return false;
    case RunPhase::headless: {
        if (options.check_strip_buttons) {
            exit_code = check_strip_buttons(check_host(runtime));
            return true;
        }
        if (!options.enter_boneyards_ticks)
            return false;
        const CheckHost host = check_host(runtime);
        exit_code =
            check_boneyards_entry(host, runtime_options(runtime), *options.enter_boneyards_ticks);
        host.write_preferences(host.context);
        return true;
    }
    }
    return false;
}

} // namespace oa::app
