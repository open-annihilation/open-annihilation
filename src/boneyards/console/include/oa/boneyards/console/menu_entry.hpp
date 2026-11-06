// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How the Boneyards' way in stands on a main menu: the strip drawn across
// the menu's bottom rows where the menu's art there is the base game's, and
// the strip's own ENTER plate, at the place the mod's profile gives or the
// top right, where the art is a mod's own. The mode is chosen once from how much of the strip's
// rows the mod's FrontendX changes, unless the mod's profile names one
// (media.boneyards-entry).
#pragma once

#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/frontend_renderer/menu_buttons.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace oa::boneyards::console {

/// How the main menu shows the way into the Boneyards.
enum class MenuEntryMode : uint8_t {
    strip,  ///< the strip across the menu's bottom rows, as BY1117a draws it
    button, ///< the strip's ENTER plate alone, at its place
    none,   ///< neither; the B key and "-y" still enter
};

/// What a mod's profile asks for (media.boneyards-entry.mode).
enum class MenuEntryAsk : uint8_t {
    automatic, ///< auto: chosen from the menu's art
    strip,     ///< strip
    button,    ///< button
    none,      ///< none
};

/// Reads a profile's media.boneyards-entry.mode: auto, strip, button or none.
///
/// @param text the value, compared exactly
/// @return what it asks for; automatic for any other text
[[nodiscard]] MenuEntryAsk menu_entry_ask_named(std::string_view text) noexcept;

/// The largest share of the strip's rows, in thousandths, in which a menu's
/// FrontendX may differ from the base game's and still show the strip. The
/// menus drawn on the base game's art differ by 4% at most (a recoloured
/// copy) and the menus with art of their own by 17% or more.
inline constexpr uint32_t strip_art_limit_per_mille = 100;

/// Chooses how the main menu shows the way in.
///
/// A profile that names a mode gets that mode. Otherwise the strip shows
/// while the menu's art differs from the base game's in at most
/// strip_art_limit_per_mille of the strip's rows, and the button shows
/// above that.
///
/// @param ask what the mod's profile asks for
/// @param differing_per_mille the share of the strip's rows the art changes, 0 to 1000
/// @return the mode
[[nodiscard]] MenuEntryMode
choose_menu_entry_mode(MenuEntryAsk ask, uint32_t differing_per_mille) noexcept;

/// The strip's ENTER plate, in the strip's own screen pixels: the riveted
/// metal panel with its screws, "boneyards", "ONLINE GAMING", the red ENTER
/// and the row of eight lamps. In button mode the menu shows this part of the
/// strip, drawn by the strip itself, at the place entry_plate_place gives.
inline constexpr oa::ui::frontend_renderer::SourceRect entry_plate{253, 380, 135, 66};

/// The dark line drawn round the plate where it stands on a mod's own art,
/// in pixels on each side.
inline constexpr int32_t entry_plate_outline = 1;

/// The plate's place when the mod's profile gives none
/// (media.boneyards-entry x and y unwritten): the top-right corner, 8
/// pixels from the picture's edges.
inline constexpr oa::ui::frontend_renderer::SourceRect entry_plate_default_place{
    oa::ui::frontend_renderer::menu_picture_width - 8 - entry_plate.width,
    8,
    entry_plate.width,
    entry_plate.height
};

/// The plate's place on a main menu with art of its own: its top-left corner
/// where the mod's profile puts it (media.boneyards-entry x and y), kept so
/// the whole plate stays on the 640x480 menu. The profile checks the same
/// bounds, 0 to 505 and 0 to 414; this keeps any other caller's place on the
/// menu too.
///
/// @param x the plate's left column
/// @param y the plate's top row
/// @return the plate's place, its size the plate's
[[nodiscard]] oa::ui::frontend_renderer::SourceRect
entry_plate_place(int32_t x, int32_t y) noexcept;

/// Tells whether the plate at a place covers any of some rectangles: the
/// menu's gadgets or Open Annihilation's buttons, for the log.
///
/// @param place the plate's place, without its outline
/// @param others the rectangles
/// @return true when the plate or its outline shares a pixel with one of them
[[nodiscard]] bool entry_plate_covers(
    const oa::ui::frontend_renderer::SourceRect& place,
    std::span<const oa::ui::frontend_renderer::SourceRect> others
) noexcept;

} // namespace oa::boneyards::console
