// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's picture set against the base game's: the band at the
// bottom of the menu that an overlay with buttons of its own covers, the
// FrontendX the base game itself holds, how much of a band a mod's own
// FrontendX changes, and whether a main-menu file comes from a mod's folder.
// A caller uses these to choose how an overlay stands on a mod's menu, and
// to leave out a base-game picture that does not belong on a mod's art.
#pragma once

#include "oa/formats/hpi.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/gui_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace oa::ui::frontend_renderer {

/// The main menu's picture, FrontendX, as the game's files name it.
inline constexpr std::string_view main_menu_background_path = "bitmaps/frontendx.pcx";

/// The main menu's own sprites, MAINMENU.GAF, as the game's files name them.
inline constexpr std::string_view main_menu_sprites_path = "anims/mainmenu.gaf";

/// The first row of the band at the bottom of the main menu that an overlay
/// with buttons of its own covers (MainMenuLayout::with_overlay).
inline constexpr int32_t main_menu_band_top = 378;

/// The band's height in rows: the menu's last 102.
inline constexpr int32_t main_menu_band_rows = 102;

/// The band's width in columns: the whole 640-pixel picture.
inline constexpr int32_t main_menu_band_width = 640;

/// The largest difference on any one channel at which two pixels still count
/// as the same colour, so that a copy saved through another palette, whose
/// colours moved by a few steps, still matches.
inline constexpr uint8_t menu_art_tolerance = 24;

/// The pixels of a band two pictures share, and how many of them differ.
struct MenuArtDifference {
    uint32_t differing{}; ///< pixels whose colour differs by more than menu_art_tolerance
    uint32_t compared{};  ///< pixels compared; 0 when the pictures could not be compared

    /// Returns the share of compared pixels that differ, in thousandths,
    /// rounded down. Pictures that could not be compared count as wholly
    /// different.
    ///
    /// @return 0 to 1000
    [[nodiscard]] constexpr uint32_t per_mille() const noexcept {
        if (compared == 0)
            return 1000;
        return static_cast<uint32_t>(static_cast<uint64_t>(differing) * 1000U / compared);
    }
};

/// Compares two pictures over a band of rows, pixel by pixel.
///
/// A pixel differs when any of its red, green and blue values differs by
/// more than menu_art_tolerance. Pictures of different sizes, or without
/// their RGB pixels, are not compared.
///
/// @param shown the picture the menu shows
/// @param base the picture it is set against
/// @param top the band's first row
/// @param rows the band's height; rows past the pictures' last are left out
/// @return the pixels compared and those that differ
[[nodiscard]] MenuArtDifference
compare_menu_art(const Image& shown, const Image& base, int32_t top, int32_t rows) noexcept;

/// Compares a button's picture at rest, as the screen draws it, with the
/// pixels of a picture under it.
///
/// The button is the first record of the screen's layout with the name,
/// compared ignoring ASCII case. Its sprite is found as the screen finds it
/// and its resting frame is drawn through the screen's active palette; each
/// pixel the frame covers is compared with the picture's pixel there, as
/// compare_menu_art compares two pixels. A button with no sprite, or a
/// picture of another size than the screen's background, compares nothing.
///
/// @param resources the screen's resources
/// @param name the button's name
/// @param picture a picture the size of the screen's background
/// @return the pixels compared and those that differ
[[nodiscard]] MenuArtDifference
compare_button_art(const ScreenResources& resources, std::string_view name, const Image& picture);

/// Tells whether a gadget lies wholly inside the band at the bottom of the
/// main menu (main_menu_band_top, main_menu_band_rows, main_menu_band_width).
///
/// A gadget with no area lies in no band.
///
/// @param gadget a record of the main menu's layout, at its screen place
/// @return true when every pixel of it is in the band
[[nodiscard]] bool in_main_menu_band(const ui::gui_layout::Gadget& gadget) noexcept;

/// Turns off, for the keyboard, the records of a main menu's layout that an
/// overlay covers: each record wholly in the band (in_main_menu_band) takes
/// no keyboard focus (gui_layout::attribute::no_focus) and, when it is a
/// button, no quick key, unless its name is one of `kept`, compared ignoring
/// ASCII case. A record only partly in the band, and every record outside
/// it, keeps both. Nothing is drawn differently.
///
/// @param[in,out] layout the main menu's layout, at its screen places; the
///     root record, the first, is left as it is
/// @param kept the names of records whose jobs the overlay's own buttons show
/// @return how many records were turned off
std::size_t cover_main_menu_band(
    ui::gui_layout::Layout& layout, std::span<const std::string_view> kept
) noexcept;

/// Finds the FrontendX the base game holds: the first copy, in lookup order,
/// in an archive that does not lie in the played mod's folder, which is the
/// copy the main menu shows when no mod is played.
///
/// A loose copy is not read: a mod's folder is loose too.
///
/// @param assets the asset store
/// @param mod_folder the played mod's folder; empty when no mod is played
/// @return the file, or nullopt when only the mod's folder holds one
[[nodiscard]] std::optional<AssetData>
base_game_main_menu_background(const AssetStore& assets, const std::filesystem::path& mod_folder);

/// Tells whether a file the store provides comes from the played mod's
/// folder: a loose file under that folder, or a file of an archive that lies
/// directly in it. An expansion's folder, which the store also layers over
/// the game folder, is the game's own.
///
/// @param assets the asset store
/// @param path the file's path
/// @param mod_folder the played mod's folder; empty when no mod is played
/// @return true for the mod's file; false for the game's, for any file when
///     no mod is played, and for a path nothing provides
[[nodiscard]] bool from_mod_folder(
    const AssetStore& assets, std::string_view path, const std::filesystem::path& mod_folder
);

} // namespace oa::ui::frontend_renderer
