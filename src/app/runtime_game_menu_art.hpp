// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reads the game's own art the Settings and Library screens are drawn with
// over the game's own main menu (game_menu_art.hpp) from the game's files:
// the bare picture the game's menus stand on, FRONTBG; a title in the
// game's font; the frame round the score board, OUTCOME0, its corners and a
// stretch of each side joined, and a stretch of its top tube; the pieces of
// commongui.gaf; and a plate of the Options screen, STARTOPT.GUI over the
// picture its setup asks for, with no button on it.
#pragma once

#include "game_menu_art.hpp"

#include "oa/formats/hpi.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <optional>
#include <string_view>

namespace oa::app {

/// The game's Options screen: STARTOPT.GUI over the picture its setup asks
/// for, with its palette and art.
///
/// @param assets the game's files
/// @return the screen
/// @throws std::exception when a file is missing or cannot be read
[[nodiscard]] oa::ui::frontend_renderer::ScreenResources read_options_screen(oa::AssetStore& assets);

/// Reads the bare picture the game's menus stand on, FRONTBG, in the
/// language shown where the game has it so.
///
/// @param assets the game's files
/// @return the picture
/// @throws std::exception when it is missing or cannot be read
[[nodiscard]] oa::Image read_bare_picture(oa::AssetStore& assets);

/// Cuts a rectangle out of a drawn screen as an opaque picture.
///
/// @param drawn the screen
/// @param x the rectangle's first column
/// @param y its first row
/// @param width its columns
/// @param height its rows
/// @return the picture; empty when the rectangle does not lie on the screen
[[nodiscard]] WindowIcon cut_out(
    const oa::ui::frontend_renderer::Surface& drawn, int32_t x, int32_t y, int32_t width, int32_t height
);

/// Makes clear what of a picture cut out of the Options screen is the bare
/// picture's ground: every pixel reached from its edges through pixels like
/// the bare picture's under them, so that the plate alone stays.
///
/// @param[in,out] picture the cut-out picture
/// @param bare the bare picture
/// @param x the picture's first column on the bare picture
/// @param y its first row
void clear_bare_ground(WindowIcon& picture, const oa::Image& bare, int32_t x, int32_t y);

/// How far a button's plate on the Options screen's picture reaches past the
/// button, in the picture's pixels: from 10 columns left of it to 9 right of
/// it, and from 7 rows above it to 6 below, as the main menu's plates.
inline constexpr int32_t options_plate_left = 10;
inline constexpr int32_t options_plate_right = 9;
inline constexpr int32_t options_plate_top = 7;
inline constexpr int32_t options_plate_bottom = 6;

/// Reads the game's art a screen is drawn with over the game's own main
/// menu.
///
/// @param assets the game's files
/// @param title the screen's title, in English, as the interface words it
/// @param screen the screen's name, before the reason on stderr
/// @return the art; none when a file is missing or a piece does not lie on
///         its picture, the reason on stderr
[[nodiscard]] std::optional<GameMenuArt>
read_game_menu_art(oa::AssetStore& assets, std::string_view title, std::string_view screen);

} // namespace oa::app
