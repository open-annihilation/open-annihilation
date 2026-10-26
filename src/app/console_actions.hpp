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
/// damage bars. Shift+1, Shift+3, Shift+8 and Shift+backquote are the other
/// four damage-bar keys. Ctrl+F5 to Ctrl+F8 store a view and F5 to F8 return
/// to it. Ctrl+B, Ctrl+F and Ctrl+V select the units tagged CTRL_B, CTRL_F
/// and CTRL_V, and Ctrl+letter selects the units tagged CTRL_<letter>.
/// Backslash runs the last console line, F11 turns the debug keys on and
/// off, and Ctrl+F10 starts a film. With the debug keys on, '=', ']', I and
/// M are the refill, the kill under the pointer, the developer line and the
/// overlay. Any other id has no code. Which physical key asks for the action
/// is the player's layout, including a mod's remaps.
///
/// @param action the action id
/// @return that key's code, or nothing when the console does not run it
[[nodiscard]] std::optional<uint32_t> console_code(std::string_view action);

} // namespace oa::app
