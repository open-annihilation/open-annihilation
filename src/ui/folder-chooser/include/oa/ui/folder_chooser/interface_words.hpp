// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts the folder chooser passes to the interface catalogue. oa-tool joins
// this list with the other modules' lists.

#pragma once

#include <string_view>

namespace oa::ui::folder_chooser {

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    "A",
    "A folder that holds totala1.hpi, or a Commander Pack or the 1997 demo installer.",
    "Applications",
    "archive",
    "B",
    "Back",
    "Battle Tactics",
    "Bottles",
    "Campaign",
    "Cancel",
    "Cavedog tilesets",
    "Cavedog units",
    "Choose a folder or an installer",
    "Core Contingency",
    "Desktop",
    "Documents",
    "Downloads",
    "file",
    "Filter",
    "Filter names",
    "folder",
    "FOUND",
    "GAMES",
    "GAMES ON THIS DECK",
    "GOG installer (macOS)",
    "GOG installer (older Macs)",
    "GOG installer (Windows)",
    "GOG installer for macOS",
    "GOG installer for older Macs",
    "GOG installer for Windows",
    "Heroic",
    "Holds {bundles}.",
    "Home",
    "Lutris",
    "macOS asks once before an app reads Desktop, Documents or Downloads.",
    "microSD card",
    "Music",
    "No folders here.",
    "No names match.",
    "Open",
    "Open Annihilation could not check for updates.",
    "Open Annihilation {version} is out. The Boneyards needs the latest version.",
    "PLACES",
    "Program Files",
    "Select a folder or an installer.",
    "SELECTED",
    "Start Open Annihilation without the mod on its command line to enter the "
    "Boneyards.",
    "Steam · Proton",
    "Switch off and enter",
    "Switch on",
    "Switch {mod} back on?",
    "System",
    "TA demo installer",
    "The Boneyards",
    "The Boneyards is installed. Start Open Annihilation again to enter the Boneyards.",
    "The Boneyards launches and reports games only on 3.1c, Core Contingency and Battle Tactics. "
    "Switch your mod off to play in the Boneyards.",
    "The Boneyards needs Open Annihilation's online features: update checks and the content "
    "catalogue. Turn them on and continue?",
    "The Boneyards needs by.ccx and the Boneyards folder.",
    "The Boneyards plays Total Annihilation without mods. "
    "Switch {mod} off and enter the Boneyards?",
    "The installer of the Total Annihilation demo (1997).",
    "Total Annihilation",
    "Total Annihilation demo",
    "Total Annihilation demo (1997)",
    "Total Annihilation: Commander Pack. Recognised by its size. The game is read out of it; "
    "nothing is installed.",
    "Turn on",
    "Up",
    "Use the system picker…",
    "Use this folder",
    "Use this installer",
    "VOLUMES",
    "X",
    "Y",
};

} // namespace oa::ui::folder_chooser
