// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match report the Boneyards keeps of a network game, behind the report
// hooks of network play's API (oa/app/netgame/extension_api.hpp): network
// play says when a report starts, what happens in the match, its chat, its
// frames and its end, and the Boneyards' reporter (oa/boneyards/reporter.hpp)
// assembles the report and uploads it to the Boneyards while a launch is
// active on data the Boneyards reports (boneyards_report_hooks).
#pragma once

#include "oa/app/netgame/extension_api.hpp"
#include "oa/boneyards/reporter.hpp"

namespace oa::app {

/// Returns the reporter's hooks as a report uses them.
///
/// The launch's options are the launch block's (player limit, time limit,
/// pfs, provider label, tournament and address) while a launch is active
/// (oa::app::netgame::extension_api::launch_active), and none otherwise;
/// the uploads are online while a launch is active on data the Boneyards
/// reports (boneyards_report_simulation_allowed), and go to the Boneyards
/// (boneyards_report_hooks). A simulation that may not be reported answers
/// no (reporter::Hooks::simulation_permitted). No reporting library is opened.
///
/// @return the hooks
[[nodiscard]] oa::boneyards::reporter::Hooks boneyards_reporter_hooks() noexcept;

/// Returns the report the Boneyards keeps; one for the whole game.
///
/// @return the report
[[nodiscard]] const oa::boneyards::reporter::Reporter& boneyards_report() noexcept;

/// Sets the report hooks of network play's API to the Boneyards' report.
///
/// report_start starts the report for the match's Game and options
/// (reporter::init_game_reporter); report_event, report_chat and
/// report_step report to it, to every destination; report_close shuts it
/// down; report_release shuts it down when the reporting library is
/// loaded. The other hooks are left as they are.
///
/// @param[in,out] hooks network play's hooks
void fill_boneyards_report_hooks(oa::app::netgame::extension_api::Hooks& hooks) noexcept;

} // namespace oa::app
