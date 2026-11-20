// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Top-bar resource readout and side HUD layout (gamedata/SIDEDATA.TDF).
#pragma once

#include "oa/core/game_state.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oa::ui::hud {

/// Screen rectangle in 640x480 source space; width/height are pixel counts.
struct Rect {
    int32_t x{}, y{}, width{}, height{};
};

/// SIDEDATA.TDF HUD positions of one side, defaulting to the game's ARM layout.
struct SideLayout {
    uint8_t metal_color = 224;
    uint8_t energy_color = 208;
    Rect metal_bar{};
    Rect energy_bar{};
    int32_t metal_num_x = 278, metal_num_y = 18;
    int32_t metal_max_x = 341, metal_max_y = 1;
    int32_t metal_zero_x = 215, metal_zero_y = 1;
    int32_t metal_produced_x = 358, metal_produced_y = 5;
    int32_t metal_consumed_x = 358, metal_consumed_y = 17;
    int32_t energy_num_x = 529, energy_num_y = 18;
    int32_t energy_max_x = 595, energy_max_y = 1;
    int32_t energy_zero_x = 468, energy_zero_y = 1;
    int32_t energy_produced_x = 609, energy_produced_y = 5;
    int32_t energy_consumed_x = 609, energy_consumed_y = 17;
    Rect unit_name{245, 452, 0, 8};
    Rect damage_bar{200, 463, 90, 2};
    Rect unit_metal_make{350, 458, 0, 8};
    Rect unit_metal_use{350, 468, 0, 8};
    Rect unit_energy_make{400, 458, 0, 8};
    Rect unit_energy_use{400, 468, 0, 8};
    Rect logo{132, 5, 21, 21};          // the viewed player's colour, left of metal
    Rect logo2{132, 455, 21, 21};       // the cursor unit owner's logo
    Rect mission_text{385, 449, 16, 2}; // head order's status text, centred on x
    Rect unit_name2{555, 452, 1, 9};    // second unit's name, centred on x
    Rect damage_bar2{510, 463, 91, 3};  // second unit's damage or stockpile bar
    Rect name{132, 452, 11, 9};         // build button or feature line
    Rect description{132, 465, 11, 8};  // build button's unit description
};

/// Reads the SIDE<index> section of SIDEDATA.TDF text over a side layout.
///
/// Keys the file omits keep their current values, except metalcolor and
/// energycolor, which are palette index 0 without their key, as in 3.1c; a
/// bar, damage bar or logo without width and a unit name at row 0 are
/// ignored.
///
/// @param sidedata Text of gamedata/SIDEDATA.TDF.
/// @param side Side index, the digit of the section name.
/// @param[in,out] layout Layout the section's values replace.
/// @return false when the text does not parse or has no such section.
bool parse_side_layout(std::string_view sidedata, int32_t side, SideLayout& layout);

/// Columns of a resource trough spanning `left`..`right` inclusive. The fill
/// ends at `fill_right` inclusive, so an empty store still fills the left
/// column; a share level the store exceeds is marked by the three columns from
/// `marker_left`. Nothing is drawn without capacity.
struct TroughColumns {
    bool fill{};
    int32_t fill_right{};
    bool marker{};
    int32_t marker_left{};
};

/// Computes the fill and share-marker columns of a resource trough.
///
/// The fill ends at left + (right - left) * shown / capacity; the marker,
/// shown when a share level is set and the store exceeds it, starts at
/// left + (right - left) * threshold / capacity. Both truncate through a
/// 64-bit integer, keeping the low 32 bits.
///
/// @param shown Store the readout shows.
/// @param capacity Storage capacity; not above 0 draws nothing.
/// @param threshold Share level; 0 for none.
/// @param store The player's actual store, compared with the share level.
/// @param left First column of the trough in screen pixels.
/// @param right Last column of the trough (inclusive).
/// @return Whether and where the fill and marker are drawn.
[[nodiscard]] TroughColumns trough_columns(
    float shown, float capacity, float threshold, float store, int32_t left, int32_t right
) noexcept;

/// Moves a value one eighth of the way from `current` to `target`, never less than one step.
///
/// @param target Value being approached.
/// @param current Value shown now.
/// @return The next value; equals `target` once reached. Arithmetic wraps in 32 bits.
[[nodiscard]] int32_t ease_toward(int32_t target, int32_t current) noexcept;

/// Rates between resource bar refreshes, in ticks.
inline constexpr uint32_t kReadoutRefreshTicks = 30;

/// Advances the resource bar's readout for one draw of a player.
///
/// The shown stores ease toward the player's (whole units, ease_toward) and
/// never exceed the capacities, which are copied. The rates are re-read once
/// the player's display timer falls behind `tick`, and the timer then
/// advances by kReadoutRefreshTicks.
///
/// @param[in,out] readout Readout being shown.
/// @param[in,out] player Player shown; its display timer advances.
/// @param tick Current game tick.
void update_resource_readout(ResourceReadout& readout, Player& player, uint32_t tick) noexcept;

/// Readout text and the palette colour it is drawn in.
struct RateText {
    char text[16]{};
    uint8_t color{};
};

inline constexpr uint8_t kPaletteWhite = 255;

// Game.ui_colors slots of the top bar and the unit panel. With the
// colour-vision mode off, production and use take slots 10 and 12. With a
// mode on, those figures take the mode's own resource colours. Slot 15 and
// the share-level marker stay on their slots in every mode.
inline constexpr int32_t kReadoutTextColor = 15;     // stores, "0" and capacities
inline constexpr int32_t kReadoutProducedColor = 10; // production and make, while the mode is off
inline constexpr int32_t kReadoutConsumedColor =
    12; // use while the mode is off, and the share marker

/// Looks up the UI colour of a readout slot.
[[nodiscard]] inline uint8_t readout_color(const Game& game, int32_t slot) noexcept {
    return game.ui_colors[slot];
}

/// Formats the top-bar energy production or use.
///
/// Whole units, the use as a magnitude; beyond 99999 either way it shows
/// thousands with a K. With the colour-vision mode off the colour is UI slot
/// 10 for production and slot 12 for use. With a mode on, production and use
/// take that mode's resource colours, matched in the palette in use.
///
/// @param game Game block holding the UI colours.
/// @param amount Energy per second.
/// @param produced true for production, false for use.
/// @return Text and palette colour.
[[nodiscard]] RateText format_energy_rate(const Game& game, float amount, bool produced) noexcept;

/// Formats the top-bar metal production or use to one decimal, the use as a magnitude.
///
/// The colour follows the colour-vision mode the way `format_energy_rate` does.
///
/// @param game Game block holding the UI colours.
/// @param amount Metal per second.
/// @param produced true for production, false for use.
/// @return Text and palette colour.
[[nodiscard]] RateText format_metal_rate(const Game& game, float amount, bool produced) noexcept;

/// Formats the unit panel's make or use of one resource.
///
/// Signed ("+" for make, "-" for use), energy whole and metal to one decimal;
/// negative amounts show as zero. The colour follows the colour-vision mode
/// the way `format_energy_rate` does.
///
/// @param game Game block holding the UI colours.
/// @param amount Amount per second.
/// @param metal true for metal, false for energy.
/// @param produced true for make, false for use.
/// @return Text and palette colour.
[[nodiscard]] RateText
format_unit_rate(const Game& game, float amount, bool metal, bool produced) noexcept;

/// Returns how many rows of the top panel's art are drawn above the battlefield.
///
/// Under ui.interface-fixes bar-clamp, art as tall as the battlefield's top
/// edge or taller is cut to one row less than it, so that a frame of exactly
/// that height loses its last row; otherwise the art is drawn whole.
///
/// @param frame_height the art's height in pixels
/// @param battlefield_top the battlefield's top row
/// @param clamp the bar-clamp fix is in force
/// @return the rows drawn
[[nodiscard]] constexpr int32_t
top_panel_rows(int32_t frame_height, int32_t battlefield_top, bool clamp) noexcept {
    return clamp && frame_height >= battlefield_top ? battlefield_top - 1 : frame_height;
}

/// Places the aspect-fitted minimap picture inside the square radar.
///
/// @param map_width Map width (any unit, compared with the height).
/// @param map_height Map height in the same unit.
/// @param size Side of the radar square in pixels.
/// @return The picture rectangle, centred on the short axis and at least one
///         pixel thick; the whole square for an empty map.
[[nodiscard]] Rect radar_picture(int32_t map_width, int32_t map_height, int32_t size) noexcept;

} // namespace oa::ui::hud
