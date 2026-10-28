// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The layout, category and More menus. The layout builds them and the input
// walks the same entries, so a key and a press agree on which item is which.
#pragma once

#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/kit/theme.hpp"

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
/// the filters. More holds two groups under their headings, Layouts (Import
/// New Layout…, Clone as New Layout…, Rename and Describe… on a layout other
/// than OA default, Restore Defaults) and Sharing (Share My Custom Keys…,
/// Paste Someone's Layout…); Compact's More leads with the layouts. No menu
/// is open for `Menu::none`, which gives nothing.
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
/// How many items an open menu can show in `room` points, at most the kit's
/// most_shown_choices. Every window of that many items fits, headings and rules
/// included, so scrolling never grows the menu past the room.
///
/// @param metrics the size class
/// @param entries the menu
/// @param room the points the menu may stand in, border included
/// @return at least one when the menu has an item
/// The points the shown items take, without the menu's border: each choice and
/// heading is the class's row height, a heading that is not first keeps a
/// space above it, and a rule (a divider with no words) is a space, at least
/// four points.
///
/// @param metrics the size class
/// @param entries the menu
/// @param first the first shown item, from 0
/// @param count how many items are shown
/// @return the height, in points
[[nodiscard]] int32_t menu_inner_height(
    const kit::Metrics& metrics, const std::vector<MenuEntry>& entries, int32_t first, int32_t count
) noexcept;

[[nodiscard]] int32_t menu_rows_that_fit(
    const kit::Metrics& metrics, const std::vector<MenuEntry>& entries, int32_t room
) noexcept;

/// The points one side of a menu field leaves inside the dialog, so a key and
/// the layout agree on how many rows scroll into view.
///
/// @param metrics the size class
/// @param size_class which class the dialog is
/// @return the room, in points
[[nodiscard]] int32_t menu_room(const kit::Metrics& metrics, kit::SizeClass size_class) noexcept;

[[nodiscard]] int32_t menu_chosen(
    const Editor& editor, Menu menu, const ScreenState& state, const std::vector<MenuEntry>& entries
);

} // namespace oa::ui::key_editor
