// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-strip-buttons: pointer clicks on the main menu's strip, and
// --check-boneyards-menu-modes: the way in a mod's main menu shows, compiled
// only when the game's self-checks are built.
#pragma once

namespace oa::app {

struct CheckHost;

/// Clicks the strip's SINGLE, MULTI, credits spot, INTRO and EXIT with
/// pointer events, and the old MULTI column the strip does not cover.
///
/// SINGLE opens single player and its back button returns. MULTI, and the
/// cross-play click on the old column, open SELECT CONNECTION and its back
/// button returns. Credits and INTRO play their movies, and the main menu
/// comes back as it was: set up once again, the strip shown, the menu's
/// buttons in place and the frame the same but for the menu's sparks. EXIT
/// ends the run. Nothing is activated by name.
///
/// @param host the running game's check host
/// @return 0 when every click did that
int check_strip_buttons(const CheckHost& host);

/// Checks the way in a mod's main menu shows, and what clicks on it do.
///
/// On a menu drawn on the base game's art the strip shows, and a click on
/// its art over a button of the mod's menu hidden under it, and one on its
/// plain art, leave the menu as it is. On a menu with art of its own the
/// strip's rows are not shown: the menu keeps the base game's SINGLE at
/// 139,393, which opens single player, and a click on ENTER on the strip's
/// plate, wherever it stands, starts the entry, which opens
/// the Boneyards (frontend state 7). Before that, the menu's Credits plays the
/// credits, and the main menu comes back as it was, the plate at its place.
///
/// @param host the running game's check host
/// @param button true for a menu with art of its own; false for one on the base game's art
/// @return 0 when every step did that
int check_boneyards_menu_modes(const CheckHost& host, bool button);

} // namespace oa::app
