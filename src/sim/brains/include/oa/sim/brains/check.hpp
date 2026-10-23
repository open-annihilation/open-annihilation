// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Loads a brain and runs fits with no match, the check a library uses
// before a game starts.
#pragma once

#include "oa/sim/brains/seat.hpp"

namespace oa::sim::brains {

/// Loads the brain as seat_brain does, with no match.
///
/// The spec's player is not checked. The network check, the sandbox, the
/// entry and its requires run, then fits(game). start is not called, no
/// hooks are installed and the sandbox is closed before this returns. The
/// result is seated when the brain fits, does_not_fit with fits's text, or
/// refused or failed as seat_brain gives them. Safe on any thread, since it
/// touches no match. `facts` need only last for the call.
///
/// @param spec the package, the level and the options
/// @param facts the game the brain reads
/// @return seated, refused, failed or does not fit
[[nodiscard]] SeatOutcome
check_brain(const SeatSpec& spec, const oa::data::brain_facts::GameFacts& facts);

} // namespace oa::sim::brains
