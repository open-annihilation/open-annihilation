// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's reading order, and where More maps… sits on map selection.
//
// The columns are the same walk the original screens use (left edges within
// 16 share a column, a horizontal run whose centres sit within 8 and whose
// next control starts within 40 is one row). Library, Settings and the
// update pill are appended after those columns, in the order they are given.
// A gadget that lies wholly inside an extension window's press rectangle is
// left out: the extension's own button is the one a player sees.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// One control that may join the main menu's order.
struct MenuCandidate {
    std::string name;      ///< the control's name
    int32_t x = 0;         ///< its left column, on the canvas
    int32_t y = 0;         ///< its top row, on the canvas
    int32_t width = 0;     ///< its width; 0 or less has no area
    int32_t height = 0;    ///< its height; 0 or less has no area
    bool visible = true;   ///< it is shown
    bool enabled = true;   ///< it takes a press
    bool hidden = false;   ///< it is covered and stays out of the order
    bool button = false;   ///< it is a push button
    bool gadget = false;   ///< it is one of the original screen's controls
    bool appended = false; ///< it follows the columns, in the order given
    /// A gadget placed after the last extension button that joined the columns.
    /// With no such button it is not a stop, so the menu's credits stays out of
    /// the order until an extension button has joined it.
    bool follows_extension = false;
    std::string shortcut; ///< a quick key, when the button has one
};

/// The rectangle where an extension window's art takes the press.
struct MenuPressRect {
    int32_t x = 0;      ///< left column
    int32_t y = 0;      ///< top row
    int32_t width = 0;  ///< width; 0 or less covers nothing
    int32_t height = 0; ///< height; 0 or less covers nothing
};

/// One stop of the order, in canvas pixels.
struct MenuReadingStop {
    std::size_t index = 0; ///< its place in the candidate list
    int32_t x = 0;         ///< its left column
    int32_t y = 0;         ///< its top row
    int32_t width = 0;     ///< its width
    int32_t height = 0;    ///< its height
};

/// Where an arrow moves once the ring is showing.
enum class MenuReadingWay : uint8_t {
    left,  ///< the previous control by place
    right, ///< the next control by place
    up,    ///< the nearest control above
    down   ///< the nearest control below
};

/// Tells whether a gadget lies wholly inside a press rectangle.
///
/// Edges count as inside. A rectangle with no area covers nothing. A control
/// that is not a gadget is never covered: the extension's own button stays.
///
/// @param candidate the control
/// @param press the rectangle
/// @return true when the gadget is covered
[[nodiscard]] bool gadget_inside_press(const MenuCandidate& candidate, const MenuPressRect& press);

/// The main menu's reading order.
///
/// Column stops come first: the original screen's controls that a press
/// rectangle does not cover, then an extension's buttons that are shown,
/// enabled, not hidden and have area. A button with no shortcut joins too,
/// so Credits follows Enter. A gadget marked to follow an extension's
/// buttons is not read by its place: it is the stop after the last of those
/// buttons, and with none of them it stays out. Library, Settings and the
/// update pill follow, in the order the candidates give them, while each is
/// shown, enabled, not hidden and has area.
///
/// @param candidates the controls, gadgets and appended icons together
/// @param presses the rectangles where extension windows take the press
/// @return the stops, in reading order
[[nodiscard]] std::vector<MenuReadingStop> main_menu_reading_order(
    std::span<const MenuCandidate> candidates, std::span<const MenuPressRect> presses
);

/// The stop an arrow lands on.
///
/// Left and right move by place, as the original screens' previous and next
/// do: the score is the top row times 5000 plus the left column, and the move
/// wraps. Up and down use that score with the column in place of the row.
/// Controls whose left edges sit within 10 share a column. An empty list
/// returns 0.
///
/// @param stops the order, each with its place
/// @param from the stop the focus is on
/// @param way the arrow
/// @return the stop the arrow lands on
[[nodiscard]] std::size_t
menu_reading_step(std::span<const MenuReadingStop> stops, std::size_t from, MenuReadingWay way);

/// Map selection's reading order.
///
/// `more-maps` is inserted immediately after the map list. With no map list
/// it is appended. Without the button the names are returned as they were
/// given.
///
/// @param names the screen's own order, each control's name
/// @param more_maps true while More maps… shows
/// @return the names, in reading order
[[nodiscard]] std::vector<std::string>
map_picker_reading_order(std::span<const std::string> names, bool more_maps);

} // namespace oa::app
