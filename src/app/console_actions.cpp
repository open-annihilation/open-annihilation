// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Maps a console action id to the key code the match console runs.
#include "console_actions.hpp"

#include "oa/ui/console/hotkeys.hpp"

namespace oa::app {

namespace hotkey = oa::ui::console::hotkey;

namespace {

/// The console code of Ctrl+A plus a letter, or nothing when `letter` is not a letter.
///
/// @param letter the letter, upper or lower case
/// @return Ctrl+that letter, or nothing
[[nodiscard]] std::optional<uint32_t> control_letter(char letter) {
    if (letter >= 'a' && letter <= 'z')
        return hotkey::control_a + static_cast<uint32_t>(letter - 'a');
    if (letter >= 'A' && letter <= 'Z')
        return hotkey::control_a + static_cast<uint32_t>(letter - 'A');
    return std::nullopt;
}

/// The bookmark slot at the end of an action id, 1 to 4, or 0.
///
/// @param action the action id
/// @param prefix `camera.bookmark-set-` or `camera.bookmark-go-`
/// @return the slot, or 0 when the id is not that bookmark
[[nodiscard]] int bookmark_slot(std::string_view action, std::string_view prefix) {
    if (!action.starts_with(prefix) || action.size() != prefix.size() + 1)
        return 0;
    const char digit = action.back();
    if (digit < '1' || digit > '4')
        return 0;
    return digit - '0';
}

} // namespace

std::optional<uint32_t> console_code(std::string_view action) {
    if (action == "game.team-menu")
        return hotkey::tab;
    if (action == "game.share-panel")
        return static_cast<uint32_t>('h');
    if (action == "game.health-bars")
        return static_cast<uint32_t>('`');
    if (action == "game.health-bars-mode-1")
        return static_cast<uint32_t>('!');
    if (action == "game.health-bars-mode-2")
        return static_cast<uint32_t>('#');
    if (action == "game.health-bars-mode-3")
        return static_cast<uint32_t>('*');
    if (action == "game.health-bars-mode-4")
        return static_cast<uint32_t>('~');
    if (action == "game.repeat-command")
        return hotkey::repeat_command;
    if (action == "debug.keys")
        return hotkey::f11;
    if (action == "debug.movie-capture")
        return hotkey::control_f10;
    if (action == "debug.refill")
        return static_cast<uint32_t>('=');
    if (action == "debug.kill-under-pointer")
        return static_cast<uint32_t>(']');
    if (action == "debug.i")
        return static_cast<uint32_t>('i');
    if (action == "debug.overlay")
        return static_cast<uint32_t>('m');
    if (const int slot = bookmark_slot(action, "camera.bookmark-set-"); slot != 0)
        return hotkey::control_f5 + static_cast<uint32_t>(slot - 1);
    if (const int slot = bookmark_slot(action, "camera.bookmark-go-"); slot != 0)
        return hotkey::f5 + static_cast<uint32_t>(slot - 1);
    if (action == "select.builders")
        return control_letter('B');
    if (action == "select.factories")
        return control_letter('F');
    if (action == "select.aircraft")
        return control_letter('V');
    constexpr std::string_view category = "select.category-";
    if (action.starts_with(category) && action.size() == category.size() + 1)
        return control_letter(action.back());
    return std::nullopt;
}

} // namespace oa::app
