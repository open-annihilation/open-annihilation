// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The list's slots. The layout draws them and the input scrolls them, so
// both use this order: a heading, then Number keys select groups at the
// start of Groups, then that category's rows.
#pragma once

#include "oa/ui/key_editor/screen.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oa::ui::key_editor {

/// One thing the list stacks, each `list_row_height` tall.
struct Slot {
    /// What the slot is.
    enum class Kind : uint8_t {
        heading,     ///< a category's heading, not a control
        number_keys, ///< Number keys select groups
        row,         ///< an action
    };
    Kind kind{Kind::row}; ///< what it is
    std::string id{};     ///< the category, or the action id
    std::string text{};   ///< a heading's words, already looked up
};

/// The slots the list shows for the screen's category, filter and search.
///
/// While every category is shown, a category that has no row is left out,
/// except Groups, which keeps its switch. While Groups is the category, the
/// switch is the first slot. Common commands, and a search in it, are rows
/// with no headings. Menu buttons that name a screen are grouped under that
/// screen's title, in Menus and in All actions. A mod whose buttons name no
/// screen keeps the flat Menus list.
///
/// @param editor the editor
/// @param state the category, the filter and the search
/// @return the slots, top to bottom
[[nodiscard]] std::vector<Slot> list_slots(const Editor& editor, const ScreenState& state);

/// A row's height at a size class.
///
/// @param size_class the class
/// @return the height, in points
[[nodiscard]] int32_t list_row_height(kit::SizeClass size_class) noexcept;

} // namespace oa::ui::key_editor
