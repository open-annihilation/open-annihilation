// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts Get Map passes to the interface catalogue. oa-tool joins
// this list with the other modules' lists.

#pragma once

#include <string_view>

namespace oa::ui::frontend_multiplayer {

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    "*** The launch names side {side}, which this game does not have.",
    "*** This game is Open Annihilation only, and unranked.",
    "*** You do not have {names}.",
    "*** You do not have {names}. GET MUTATORS downloads them from {registry}.",
    "*** You do not have {name}.",
    "*** You do not have {name}. GET MUTATORS downloads it from {registry}.",
    "*** {host} turned mutators off.",
    "*** {host} turned on mutators: {list}",
    "*** {list}",
    "*** {player} chose a side this game does not have.",
    "*** {player} does not have {name}.",
    "*** {player} needs a newer Open Annihilation for this mod.",
    "*** {player} plays Total Annihilation 3.1c and cannot join a game with mutators.",
    "*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than 256 weapon "
    "types.",
    "*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than five "
    "sides.",
    "*** {player} plays Total Annihilation 3.1c, which cannot play with mutators.",
    "Add {registry} to get {pack}",
    "CANCEL",
    "COMMANDER",
    "Cancel",
    "Couldn't get {pack}: {reason}",
    "Different from the host: a file does not match.",
    "Different from the host: another base game ({name}).",
    "Different from the host: another release of {name}.",
    "Different from the host: no {name}.",
    "Different from the host: {name} is on.",
    "Expansions",
    "Downloading {pack} · {done} of {size}",
    "Downloads are off.",
    "Downloads from {registry} are off (Settings › Downloads).",
    "Easy",
    "GET MAP",
    "GET MUTATORS",
    "Getting {name}: {percent}%",
    "Hard",
    "LOAD",
    "Medium",
    "Mod",
    "Mutators",
    "Mutators:",
    "OK",
    "Off",
    "Open Annihilation",
    "PLAYERS",
    "PREV",
    "Platform",
    "Previous",
    "Ready once {name} is here",
    "Rules",
    "SELECT",
    "SETTINGS",
    "SIZE",
    "The game cannot start until the content matches.",
    "The game cannot start until the host turns it on, or {player} turns it off.",
    "The game cannot start until {player} gets it, or the host turns it off.",
    "The game cannot start until {player} turns it on, or the host turns it off.",
    "The host's release of {pack} is no longer offered by {registry}.",
    "This mod needs every player on Open Annihilation. The host plays Total Annihilation 3.1c.",
    "Total Annihilation 3.1c",
    "UPDATE",
    "View",
    "VIEW…",
    "Waiting for {player}: {name}",
    "You have release {mine} of {pack}; the host plays release {theirs}.",
    "cannot check",
    "check expired",
    "install failed",
    "no downloads",
    "no space",
    "not offered",
    "not written",
    "refused",
    "unreachable",
    "wrong file",
    "wrong size",
    "{item} ✓",
    "{item}: another release here",
    "{item}: not installed",
    "{name}: not offered.",
    "{name}: {problem}",
    "{registry}: downloads off.",
    "{pack} {version} · {size} from {registry}",
};

} // namespace oa::ui::frontend_multiplayer
