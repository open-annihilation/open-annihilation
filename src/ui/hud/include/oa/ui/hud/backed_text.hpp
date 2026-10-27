// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Text laid over backings in the match's bars and over the battlefield: a
// line's background box laid apart from its letters, the groups of lines
// whose letters are held until every backing of the group is laid, so that
// no line's box covers another line of its group, and what a check finds of
// lines laid on pixels. Nothing here changes the simulation, a saved game or
// what a shared game sends.
#pragma once

#include "oa/present/game_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace oa::ui::hud {

/// Lays the background box of a line alone, as lay_text lays it under the
/// line: over every column of the line and its rows from background_top to
/// background_bottom, in text_shade_color reduced to the canvas's palette.
/// The line's letters, outline and shadow are left to without_backing.
///
/// @param[in,out] canvas the pixels; an 8-bit canvas without a palette is
///        left as it is, as lay_text leaves it
/// @param layers the line; one without a background box lays nothing
/// @param x the column of the pen
/// @param baseline_y the row just below the capitals
void lay_text_backing(
    present::TextCanvas& canvas, const present::TextLayers& layers, int32_t x, int32_t baseline_y
);

/// Gives a line without its background box: laid over the box
/// lay_text_backing laid, it makes the pixels lay_text makes of the whole
/// line, where the palette holds black.
///
/// @param layers the line
/// @return the line with no box; background_top and background_bottom
///         still give the rows its box took
[[nodiscard]] present::TextLayers without_backing(present::TextLayers layers);

/// The groups of lines the match draws over backings, and the lines they
/// hold.
///
/// While a group holds, the drawing lays each line's backing where the line
/// is drawn and hands the line itself to hold; when the outermost group
/// closes, the held lines are laid in the order they were drawn, over every
/// backing of the group. A group opened inside another joins it. A group
/// that does not hold lays each line where it is drawn, backing and all, as
/// if there were no group.
class TextHold {
  public:

    /// Stops a TextHold holding while it lives, for a painter that reads
    /// back what its lines made as it paints them.
    class Pause {
      public:

        /// Stops holding.
        ///
        /// @param[in,out] hold the groups
        explicit Pause(TextHold& hold) noexcept;
        /// Holds again, if a group holds.
        ~Pause();
        Pause(const Pause&) = delete;
        Pause& operator=(const Pause&) = delete;

      private:

        TextHold* hold_{};
    };

    /// Opens a group, or joins the group already open.
    ///
    /// @param holds whether a group opened now holds its lines; a group
    ///        joined keeps the choice of the group it joins
    void open(bool holds);

    /// Closes a group. Closing the outermost lays the lines it held, in the
    /// order they were held, while nothing holds; a close with no group
    /// open does nothing.
    void close();

    /// Lets go every held line unlaid and closes every group, as when a
    /// frame stops part way.
    void drop() noexcept;

    /// Tells whether a line drawn now is held.
    ///
    /// @return true while a group that holds is open and no Pause lives
    [[nodiscard]] bool holding() const noexcept;

    /// Holds a line until the outermost group closes; while nothing holds,
    /// lays it at once.
    ///
    /// @param lay lays the line, once, while nothing holds
    void hold(std::function<void()> lay);

    /// Gives the number of lines held now.
    ///
    /// @return the lines waiting for their group to close
    [[nodiscard]] std::size_t held() const noexcept;

  private:

    std::vector<std::function<void()>> lines_{};
    int32_t depth_{};  ///< groups open
    int32_t paused_{}; ///< Pause objects alive
    bool holds_{};     ///< the outermost open group holds its lines
};

/// A line laid on pixels, as a check looks for it.
struct LaidLine {
    present::TextLayers layers{};
    int32_t x{};          ///< the column of the pen
    int32_t baseline_y{}; ///< the row just below the capitals
    std::array<uint8_t, 3> color{};
    /// Its background box was laid, over the rows its layers' background
    /// rows give.
    bool backed{};
};

/// What a check finds of lines laid on RGB pixels, in the order they were
/// laid.
struct LaidLinesFound {
    /// Pixels a line's letters cover whole.
    std::size_t letter_pixels{};
    /// Of those, the pixels inside the backing of a line laid after it: the
    /// places where a line's box lies over another line.
    std::size_t letters_under_later_backings{};
    /// Pixels a line's letters cover whole that do not show its colour,
    /// where no line laid after it has its letters, outline or shadow.
    std::size_t covered_letters{};
    /// Pixels of the lines' backings where no line has its letters, outline
    /// or shadow.
    std::size_t backing_pixels{};
    /// Of those, the pixels that show the backing's colour.
    std::size_t backing_pixels_shown{};
    /// Lines laid with a backing that shows on fewer than half of its
    /// pixels where no line has its letters, outline or shadow.
    std::size_t bare_backings{};
};

/// Finds what became of lines laid on RGB pixels: whether every line's
/// letters show whole and every backing is there.
///
/// A pixel a line's letters cover whole shows the line's colour reduced to
/// the palette, and a pixel of a backing where no line has its letters,
/// outline or shadow shows text_shade_color reduced to it.
///
/// @param rgb width * height * 3 bytes, top row first
/// @param width columns
/// @param height rows
/// @param palette the palette the colours were reduced to, entries of red,
///        green, blue and one more byte; empty for colours kept
/// @param lines the lines, in the order they were laid
/// @return what was found
[[nodiscard]] LaidLinesFound find_laid_lines(
    std::span<const uint8_t> rgb,
    int32_t width,
    int32_t height,
    std::span<const uint8_t> palette,
    std::span<const LaidLine> lines
);

} // namespace oa::ui::hud
