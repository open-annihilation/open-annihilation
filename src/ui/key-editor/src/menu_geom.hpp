// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The layout, category and More menus. The layout builds them and the input
// walks the same entries, so a key and a press agree on which item is which.
#pragma once

#include "oa/ui/key_editor/screen.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oa::ui::key_editor {

/// What choosing one menu item does.
enum class MenuKind : uint8_t {
    layout_id, ///< choose that layout
    clone,     ///< ask the new layout's name and description
    describe,  ///< ask the layout's new name and description
    category,  ///< show that category
    filter,    ///< keep that filter
    restore,   ///< restore the defaults
    share,     ///< open Share your keys
    paste,     ///< read the clipboard
    import,    ///< open Import keys
    shape,     ///< the keyboard's shape
    divider,   ///< a heading, not a choice
};

/// One item of an open menu, in the menu's order. A divider has no control.
struct MenuEntry {
    MenuKind kind{MenuKind::divider}; ///< what choosing it does
    std::string id{};                 ///< the layout, the category or the filter
    std::string caption{};            ///< the words, already looked up
    std::string name{};               ///< the control's name; empty on a divider
};

/// The items of the menu that is open. Compact's category menu also holds
/// the filters, and Compact's More the layouts. No menu is open for `Menu::none`, which gives nothing.
///
/// @param editor the editor
/// @param menu which menu
/// @param state the category and the filter
/// @param compact true at Compact, where the filters live in the category menu
/// @return the items, top to bottom
[[nodiscard]] std::vector<MenuEntry>
menu_entries(const Editor& editor, Menu menu, const ScreenState& state, bool compact);

/// The item the menu shows as chosen.
///
/// @param editor the layout in use
/// @param menu which menu
/// @param state the category
/// @param entries what menu_entries gave
/// @return the item's place, from 0; -1 when none is chosen
[[nodiscard]] int32_t menu_chosen(
    const Editor& editor, Menu menu, const ScreenState& state, const std::vector<MenuEntry>& entries
);

} // namespace oa::ui::key_editor
