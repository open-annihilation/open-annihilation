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
    "PLACES",
    "Program Files",
    "Select a folder or an installer.",
    "SELECTED",
    "Steam · Proton",
    "System",
    "TA demo installer",
    "The Boneyards",
    "The Boneyards needs by.ccx and the Boneyards folder.",
    "The installer of the Total Annihilation demo (1997).",
    "Total Annihilation",
    "Total Annihilation demo",
    "Total Annihilation demo (1997)",
    "Total Annihilation: Commander Pack. Recognised by its size. The game is read out of it; "
    "nothing is installed.",
    "Up",
    "Use the system picker…",
    "Use this folder",
    "Use this installer",
    "VOLUMES",
    "X",
    "Y",
};

} // namespace oa::ui::folder_chooser
