// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How dark the battlefield's shadows are drawn as the view zooms out, and the
// alpha table the processor draws a faded shadow through. At the game's view,
// zoom 1, and closer, shadows are drawn exactly as the game draws them. Zoomed
// out they lighten, easing smoothly, until, once they would be a sixteenth of
// the game's darkness, a little before four times as far out as the game's
// view, no shadow is drawn at all, and none costs a frame anything.
// Presentation only: nothing the simulation reads changes.
#pragma once

#include "oa/present/model/model_library.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace oa::present::model {

/// Steps between no shadow and the game's own darkness a shadow is drawn at:
/// a level of shadow_full_level draws shadows as the game does, 0 draws none.
inline constexpr uint32_t shadow_full_level = 64;

/// The zoom, in window pixels per map pixel, at which the shadows' strength
/// reaches 0: four times as far out as the game's view.
inline constexpr float shadowless_zoom = 0.25F;

/// The faintest level a shadow is drawn at: a sixteenth of the game's
/// darkness, under which a shadow would change what is under it by a few
/// levels of a channel at most, which nobody sees, for the whole cost of
/// drawing it.
inline constexpr uint32_t shadow_least_level = shadow_full_level / 16;

/// Returns how dark shadows are drawn at a zoom, from 0 (none) to 1 (as the
/// game draws them).
///
/// 1 at zoom 1 and closer, 0 at shadowless_zoom and farther out; between, 1
/// less the smoothstep of how far out the view lies, in halvings of the zoom
/// over the two halvings from zoom 1 to shadowless_zoom, so that each step
/// of the wheel lightens the shadows by about as much and they ease in and
/// out at both ends: 0.5 at zoom 0.5 (twice as far out).
///
/// @param zoom window pixels per map pixel; 0 or less counts as the farthest out
/// @return the strength, 0 to 1
[[nodiscard]] float shadow_strength(float zoom) noexcept;

/// Returns the level shadows are drawn at, at a zoom: shadow_strength in
/// steps of 1 / shadow_full_level, rounded to the nearest step, and 0 below
/// shadow_least_level.
///
/// shadow_full_level exactly where the strength is 1 (zoom 1 and closer, and
/// a zoom a float's rounding away from 1), 0 where it rounds to fewer than
/// shadow_least_level steps, and between shadow_least_level and
/// shadow_full_level - 1 wherever it lies between.
///
/// @param zoom window pixels per map pixel
/// @return the level, 0 to shadow_full_level
[[nodiscard]] uint32_t shadow_level(float zoom) noexcept;

/// Returns the strength a level draws shadows at.
///
/// @param level 0 to shadow_full_level; more counts as shadow_full_level
/// @return level / shadow_full_level, 0 to 1
[[nodiscard]] constexpr float shadow_level_strength(uint32_t level) noexcept {
    return level >= shadow_full_level
               ? 1.0F
               : static_cast<float>(level) / static_cast<float>(shadow_full_level);
}

/// Mixes a colour channel toward the colour a shadow makes of it, by a
/// level: the shadowed channel at shadow_full_level, the channel itself at
/// 0, rounded to the nearest value between.
///
/// @param under the channel under the shadow
/// @param shadowed the channel the game's own shadow makes of it
/// @param level 0 to shadow_full_level
/// @return the faded channel
[[nodiscard]] constexpr uint8_t
fade_shadow_channel(uint8_t under, uint8_t shadowed, uint32_t level) noexcept {
    if (level >= shadow_full_level)
        return shadowed;
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(under) * (shadow_full_level - level) +
         static_cast<uint32_t>(shadowed) * level + shadow_full_level / 2) /
        shadow_full_level
    );
}

/// The alpha table the processor draws shadows through at a level below the
/// game's own: the display's alpha table, its rows for the colours shadows
/// are drawn in each faded. A faded row maps each colour under the shadow to
/// the palette's colour nearest the colour under it mixed toward the
/// display's blend by the level (fade_shadow_channel), and keeps the colour
/// under it where that is the nearest. Every other row is the display's own.
///
/// The table is made again only when the level, the colours, the display's
/// alpha table or its palette change; drawing reads it and changes nothing,
/// so the bands of a frame share it.
class ShadowTable {
  public:

    /// Readies the table for a level and returns what shadows blend through.
    ///
    /// @param display the models' display: its alpha table and palette
    /// @param level 0 to shadow_full_level
    /// @param colours set for each colour the shadows are drawn in, whose
    ///     rows are faded
    /// @return null at shadow_full_level, or at 0, or where the display has
    ///     no alpha table: shadows then blend through the display's own, or
    ///     are not drawn; else the table, 256 rows of 256 colours as the
    ///     display's, valid until the next call
    const uint8_t*
    prepare(const ModelDisplay& display, uint32_t level, const std::array<bool, 256>& colours);

    /// Returns how many times the table has been made, for the tests.
    ///
    /// @return the count
    [[nodiscard]] uint64_t builds() const noexcept { return builds_; }

  private:

    std::vector<uint8_t> table_;         ///< the faded table, 256 by 256
    std::vector<uint8_t> source_;        ///< the display's alpha table it was made from
    Palette palette_{};                  ///< the palette it was made with
    std::array<bool, 256> faded_rows_{}; ///< the colours whose rows are faded
    uint32_t level_{shadow_full_level};  ///< the level it was made at
    uint64_t builds_{};                  ///< times the table was made
};

/// Collects the colours a sprite's pixels are drawn in, other than its key,
/// for ShadowTable::prepare.
///
/// A raw sprite's pixels are read; any other sprite adds colour 0 alone,
/// the colour the models' silhouettes are drawn in.
///
/// @param sprite the sprite; null or one with no data adds nothing
/// @param[in,out] colours set for each colour found
void note_shadow_colours(const Sprite* sprite, std::array<bool, 256>& colours) noexcept;

} // namespace oa::present::model
