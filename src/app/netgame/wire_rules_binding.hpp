// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The network rules a mod profile gives network play: its identity's
// version bytes and every network.* and recorder.* hack, as the plain
// WireRules the battle room, the connection and the match read.

#include "oa/data/mod_profile.hpp"
#include "oa/netgame/wire_rules.hpp"

namespace oa::app::netgame {

/// Returns the network rules of a profile.
///
/// No profile gives 3.1c's rules. A network version other than 3.1 makes
/// joining need the same major version, with no launch bias, as the clients
/// of such a game compare it. The game speed range, and whether the host
/// may lock it with .syncon, come from console.game-speed-range. With
/// Enhanced Mode on (setup.enhanced-mode) route changes travel as complete
/// route batches.
///
/// @param profile the resolved profile, or null
/// @return the rules
[[nodiscard]] oa::netgame::WireRules
wire_rules_of(const oa::data::mod_profile::ModProfile* profile) noexcept;

} // namespace oa::app::netgame
