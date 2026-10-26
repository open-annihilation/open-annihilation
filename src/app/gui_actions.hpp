// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A gadget's made gui action, and the registry action whose gadget word the
// name holds. Nothing here reads a match or a layout.
#pragma once

#include <string>
#include <string_view>

namespace oa::app {

/// Folds one piece of a gui id: ASCII lower case, with spaces and underscores
/// written as hyphens.
///
/// @param text a file stem or a gadget name
/// @return the folded piece
[[nodiscard]] std::string gui_key_piece(std::string_view text);

/// The made action id of one gadget.
///
/// `gui.` then the file stem and the gadget name, each folded by
/// gui_key_piece.
///
/// @param stem the GUI file's stem, with or without its extension's case
/// @param name the gadget's name
/// @return the action id
[[nodiscard]] std::string gui_action_id(std::string_view stem, std::string_view name);

/// The registry action whose gadget word the name holds.
///
/// The words are tried in the order the order panel tries a button's name:
/// the standing-order toggles first, because MOVEORD holds MOVE, then the
/// command words with UNLOAD before LOAD, then the ORDERS and BUILD tabs.
/// The id is the registry's, and is empty when the name holds none of those
/// words.
///
/// @param name a gadget name, with or without a side's prefix
/// @return the action id, or empty
[[nodiscard]] std::string_view order_action_id(std::string_view name);

} // namespace oa::app
