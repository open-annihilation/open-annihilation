// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where Open Annihilation's buttons sit on the main menu's 640x480 picture:
// the Library button and, a quarter of a button's side to its right, the
// Settings button, as a pair in one corner, and the place a wider button
// beside them takes. The engine draws the pair; an overlay that adds a
// button to the group places it with the same numbers.
#pragma once

#include "oa/ui/frontend_renderer/artless.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::ui::frontend_renderer {

/// The side of each square button of the main menu's group, in source pixels.
inline constexpr int32_t menu_button_side = 48;

/// The gap between the group's buttons: a quarter of a button's side.
inline constexpr int32_t menu_button_gap = menu_button_side / 4;

/// The gap between the group and the picture's edges.
inline constexpr int32_t menu_button_inset = 12;

/// The width of the picture the group sits on, in source pixels.
inline constexpr int32_t menu_picture_width = 640;

/// The height of the picture the group sits on, in source pixels.
inline constexpr int32_t menu_picture_height = 480;

/// The width of a button as wide as the pair: two sides and the gap.
inline constexpr int32_t menu_pair_width = 2 * menu_button_side + menu_button_gap;

/// A corner of the main menu's picture.
enum class MenuCorner : uint8_t { top_left, top_right, bottom_left, bottom_right };

/// Reads a corner's name: top-left, top-right, bottom-left or bottom-right.
///
/// @param name the name, compared exactly
/// @return the corner, or nullopt for any other text
[[nodiscard]] constexpr std::optional<MenuCorner>
menu_corner_named(std::string_view name) noexcept {
    if (name == "top-left")
        return MenuCorner::top_left;
    if (name == "top-right")
        return MenuCorner::top_right;
    if (name == "bottom-left")
        return MenuCorner::bottom_left;
    if (name == "bottom-right")
        return MenuCorner::bottom_right;
    return std::nullopt;
}

/// Tells whether a corner is on the picture's top edge.
///
/// @param corner the corner
/// @return true for top-left and top-right
[[nodiscard]] constexpr bool menu_corner_top(MenuCorner corner) noexcept {
    return corner == MenuCorner::top_left || corner == MenuCorner::top_right;
}

/// Tells whether a corner is on the picture's left edge.
///
/// @param corner the corner
/// @return true for top-left and bottom-left
[[nodiscard]] constexpr bool menu_corner_left(MenuCorner corner) noexcept {
    return corner == MenuCorner::top_left || corner == MenuCorner::bottom_left;
}

/// Returns the corner the group takes while an overlay covers the menu's
/// bottom rows: the top corner on the same side, so the group stays clear of
/// it; any other time, the corner asked for.
///
/// @param asked the corner asked for
/// @param overlay_drawn an overlay with buttons of its own covers the bottom rows
/// @return the corner the group sits in
[[nodiscard]] constexpr MenuCorner
menu_group_corner(MenuCorner asked, bool overlay_drawn) noexcept {
    if (!overlay_drawn)
        return asked;
    return menu_corner_left(asked) ? MenuCorner::top_left : MenuCorner::top_right;
}

/// The pair's two buttons.
struct MenuPairPlaces {
    SourceRect library{};  ///< the Library button
    SourceRect settings{}; ///< the Settings button, a gap to the Library's right
};

/// Places the pair in a corner, menu_button_inset in from both its edges:
/// the Library button, then the Settings button a menu_button_gap to its
/// right. In a right-hand corner the Settings button keeps to the edge; in a
/// left-hand one, the Library button does.
///
/// @param corner the corner the group sits in (menu_group_corner)
/// @return the two squares, in source pixels
[[nodiscard]] constexpr MenuPairPlaces menu_pair_places(MenuCorner corner) noexcept {
    const int32_t x = menu_corner_left(corner)
                          ? menu_button_inset
                          : menu_picture_width - menu_button_inset - menu_pair_width;
    const int32_t y = menu_corner_top(corner)
                          ? menu_button_inset
                          : menu_picture_height - menu_button_inset - menu_button_side;
    return {
        {x, y, menu_button_side, menu_button_side},
        {x + menu_button_side + menu_button_gap, y, menu_button_side, menu_button_side},
    };
}

} // namespace oa::ui::frontend_renderer
