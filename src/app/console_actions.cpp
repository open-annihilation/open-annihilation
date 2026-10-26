// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Maps a console action id to the key code the match console runs.
#include "console_actions.hpp"

#include "oa/ui/console/hotkeys.hpp"

namespace oa::app {

namespace hotkey = oa::ui::console::hotkey;

std::optional<uint32_t> console_code(std::string_view action) {
    if (action == "game.team-menu")
        return hotkey::tab;
    if (action == "game.share-panel")
        return static_cast<uint32_t>('h');
    if (action == "game.health-bars")
        return static_cast<uint32_t>('`');
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
    return std::nullopt;
}

} // namespace oa::app
