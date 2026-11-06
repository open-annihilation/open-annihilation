// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' part of --check-multiplayer-menu, which drives the running
// game through the engine's check host (oa/app/check_host.hpp) after network
// play's part.
#pragma once

namespace oa::app {

struct CheckHost;

/// Checks the Boneyards on the main menu for --check-multiplayer-menu.
///
/// The main menu shows the Boneyards strip. Enter Boneyards reaches frontend
/// state 7 and the login console opens; a request to close the window asks
/// "Exit to the system?", whose NO keeps the game. REGISTER in the login
/// console opens the registration console, and BACK TO LOGIN leaves it.
/// EXIT then returns to the main menu with no message box left behind. On
/// the main menu after EXIT a request to close the window still asks
/// "Exit to the system?", since the Boneyards' entry installed the question
/// and no application mode has been set since; and MULTI still leads to the
/// provider list (oa::app::netgame::menu_check::multiplayer_round).
/// Every click goes through the SDL presenter as a player's does. A
/// failure, or a run without the game's window, throws std::runtime_error.
///
/// @param host the running game's check host
void check_boneyards_menu(const CheckHost& host);

} // namespace oa::app
