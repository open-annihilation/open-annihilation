// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts the first-run setup passes to the interface catalogue. oa-tool joins
// this list with the other modules' lists.

#pragma once

#include <string_view>

namespace oa::ui::first_run {

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    "A random 128-bit number, made the first time you download from a registry, not when the game "
    "starts. Each registry that asks has its own. Core Prime asks for one. It is sent only when "
    "asking that registry for a download. Turning it off forgets it, and downloads from that "
    "registry stop. Built-in languages and files you open keep working.",
    "A random install ID, one per registry, is sent only when downloading from that registry.",
    "Active",
    "Back",
    "Built in",
    "Cancel",
    "Cancel download",
    "Check for content updates is set to Never, and Core Prime's install ID is turned off.",
    "Check for content updates stays at Automatically, and Core Prime's install ID stays on.",
    "Checking…",
    "Choose your language.",
    "Close",
    "Couldn't download",
    "Download from GOG",
    "Download from GOG…",
    "Downloading",
    "Downloading the Total Annihilation: Commander Pack installer.",
    "Downloads are off, so language packs stay offline.",
    "Downloads, updates and a number only you hold",
    "Get",
    "Get language packs, mods and map packs from Core Prime.",
    "Get, update and switch mods, map packs and languages. The number counts every package you "
    "have added: mods, map packs, language packs and expansions.",
    "Installed",
    "Library",
    "Lists of what is new are checked as the game starts and when the Library opens.",
    "Next",
    "Not now",
    "ONLINE",
    "ONLINE FEATURES",
    "Offline",
    "Open Annihilation",
    "Open Annihilation then downloads the Total Annihilation: Commander Pack installer from your "
    "account and copies the game out of it. Your sign-in is used for this download only and is not "
    "kept.",
    "Open Annihilation's own settings: controls, graphics, the interface's size, downloads and "
    "your files.",
    "Open GOG again",
    "Other languages download from Core Prime. The first download makes a random install ID, which "
    "a later step explains.",
    "Paste address",
    "Previous",
    "Retry",
    "Settings",
    "Sign in to GOG in the window that opened. The download starts as soon as you have signed "
    "in.",
    "Sign in to GOG on the page your web browser opened. GOG then shows an empty white page: copy "
    "its whole address from the browser's address bar. The download starts by itself; Paste "
    "address starts it too.",
    "Signing in to GOG…",
    "Skip",
    "Start playing",
    "The game could not be downloaded from GOG.",
    "THE MAIN MENU",
    "The Library is where you get mods, map packs and languages. Settings is where you change how "
    "Open Annihilation looks and plays. Everything else on this menu is the game's own.",
    "The catalogue could not be read, so language packs stay offline.",
    "Try again",
    "Turn on online features",
    "Two buttons for everything Open Annihilation adds",
    "WELCOME",
    "Welcome",
    "A continues. B goes back.",
    "COPYING",
    "Check again",
    "Continue",
    "Copies go to Documents › Open Annihilation",
    "Copy again from…",
    "Copy the game",
    "Copying Total Annihilation",
    "From {place}…",
    "GAME CONTENT",
    "Game Content",
    "If OA stops, the copy carries on from here next time.",
    "Locate Total Annihilation Game Files",
    "Locate another…",
    "Nothing is installed and your files are not changed.",
    "OA copies the game once. The installation is left as it is.",
    "OA plays from its own copy in Documents ›\u00a0Open Annihilation. The installation is not "
    "used or "
    "changed.",
    "Open folder",
    "Pause",
    "Quit",
    "Setup",
    "THE GAME",
    "To Documents › Open Annihilation › Games and Expansions",
    "Welcome to Open Annihilation",
    "Your install ID",
    "about {seconds} seconds left",
    "not added",
    "of",
    "{done} of {total} · {percent}%",
    "{step} · {index} OF {count}",
    "And {n} more; the log has them all.",
    "Choose another place",
    "Open Annihilation needs a fresh Total Annihilation install, without mods, to set up its copy. "
    "Nothing has been copied. Choose another place to copy from.",
    "Some of its files are not the size they are in Total Annihilation 3.1c and its expansions. A "
    "mod or another program may have changed them.",
    "Your Total Annihilation has been changed",
    "{file}, from {bundle}, is {size} bytes. It should be {expected}.",
};

} // namespace oa::ui::first_run
