// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts the map browser passes to the interface catalogue. oa-tool joins
// this list with the other modules' lists.

#pragma once

#include <string_view>

namespace oa::ui::map_browser {

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    "1 player",
    "2",
    "3",
    "4",
    "5–10",
    "All",
    "Any",
    "Available",
    "Battle room · No Mod",
    "Battle room · playing {mod}",
    "Cancel",
    "Choose a map",
    "Close",
    "Doesn't fit",
    "Doesn't fit {game}",
    "Download and select",
    "Downloaded",
    "Downloading…",
    "Downloads",
    "Fit with {game} is checked when it's installed",
    "Fits {game} ✓",
    "Game",
    "L",
    "Loading preview",
    "M",
    "MAPS",
    "Mod",
    "No Mod",
    "No map matches these filters",
    "No preview",
    "OPEN ANNIHILATION",
    "PLAYERS",
    "S",
    "SIZE",
    "Search maps",
    "Select",
    "Skirmish · No Mod",
    "Skirmish · playing {mod}",
    "TA 3.1c",
    "{count} players",
    "{filter} ({count})",
    "{players} · {size}",
    "{size} KB",
    "{size} MB",
};

} // namespace oa::ui::map_browser
