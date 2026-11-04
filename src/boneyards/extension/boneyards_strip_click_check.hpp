// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-strip-buttons: pointer clicks on the main menu's strip, compiled
// only when the game's self-checks are built.
#pragma once

namespace oa::app {

struct CheckHost;

/// Clicks the strip's SINGLE, MULTI and EXIT with pointer events, and the
/// old MULTI column the strip does not cover.
///
/// SINGLE opens single player and its back button returns. MULTI, and the
/// cross-play click on the old column, open SELECT CONNECTION and its back
/// button returns. EXIT ends the run. Nothing is activated by name.
///
/// @param host the running game's check host
/// @return 0 when every click did that
int check_strip_buttons(const CheckHost& host);

} // namespace oa::app
