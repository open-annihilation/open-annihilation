// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The line icons the settings dialog draws: one before each section's entry
// in the list, and the key layout card's tile. Each is drawn once as a
// picture large enough for every scale, which draw_picture filters down to
// its place.
#pragma once

#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"

#include <cstdint>

namespace oa::ui::engine_settings::icons {

/// The colour an icon is drawn in.
enum class Tone : uint8_t {
    quiet, ///< an entry at rest: the list's text colour
    lit,   ///< the open entry, or one under the pointer: the text colour
};

/// The side of every icon's picture, in pixels: three pixels to a unit of
/// the 24-unit square the icons are designed on, so that an icon stays
/// sharp up to 3x.
inline constexpr uint32_t picture_side = 72;

/// Returns a section's icon: a few lines on a 24-unit square, two units
/// wide with round ends, as the design's icons are. Touch is a hand,
/// Controller a gamepad, Updates a turning arrow, Game Content a disc,
/// System a box, Engine Limits three sliders and Developer two angle
/// brackets; a mod's options and a registered page take the icons nearest
/// their meaning.
///
/// @param page the section
/// @param tone the colour
/// @return the picture, picture_side square; it lives as long as the program
[[nodiscard]] const oa::ui::frontend_renderer::RgbaPicture& section_icon(Page page, Tone tone);

/// Returns the key layout card's tile: a keyboard in the accent on a rounded
/// square of the hover surface.
///
/// @return the picture, picture_side square; it lives as long as the program
[[nodiscard]] const oa::ui::frontend_renderer::RgbaPicture& key_layout_tile();

} // namespace oa::ui::engine_settings::icons
