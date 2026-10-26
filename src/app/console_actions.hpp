// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match console's action ids and the key codes it runs for them.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::app {

/// Returns the key code the match console runs for an action.
///
/// Tab opens the allies menu, H the share panel and the backquote key the
/// damage bars. Backslash runs the last console line, F11 turns the debug
/// keys on and off, and Ctrl+F10 starts a film. With the debug keys on, '=',
/// ']', I and M are the refill, the kill under the pointer, the developer
/// line and the overlay. Any other id has no code. Which physical key asks
/// for the action is the player's layout, including a mod's remaps.
///
/// @param action the action id
/// @return that key's code, or nothing when the console does not run it
[[nodiscard]] std::optional<uint32_t> console_code(std::string_view action);

} // namespace oa::app
