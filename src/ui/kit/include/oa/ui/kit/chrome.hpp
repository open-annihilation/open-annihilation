// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The window's chrome: its face and edge, its header and footer band, the
// nav list, and a section's heading, rows, rules and locked fade. Texts
// arrive already looked up. The arithmetic is the settings dialog's.
#pragma once

#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/tag_icon.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::ui::kit {

/// A rectangle in points. One point is one pixel of the 640 by 480 picture.
using Rect = oa::ui::frontend_renderer::SourceRect;

struct Canvas;
struct DisplayList;

/// A control's number. The same number layout.hpp names.
using ControlId = int32_t;

/// What the header shows. Empty strings draw nothing. The widths are the
/// columns kept for each word, and the caller passes them.
struct HeaderLook {
    int32_t width{};         ///< the window's width, in points
    std::string title{};     ///< the title, already looked up
    int32_t title_width{};   ///< the columns kept for the title
    int32_t tracking{};      ///< extra columns after each glyph of the title and the second word
    std::string second{};    ///< the word after the title, already looked up
    int32_t second_width{};  ///< the columns kept for that word
    std::string version{};   ///< the version, already looked up
    int32_t version_width{}; ///< the columns kept for the version
    std::string note{};      ///< the note before the version, already looked up
    /// Columns kept clear at the right, inside the padding, for a control
    /// the header does not draw. 0 puts the version against the padding.
    int32_t right_inset{};
    /// The mark at the left. Off, the title, or the second word when there
    /// is no title, starts at the padding.
    bool mark{true};
    /// The band's height under the top edge, in points: Kit 2's window header
    /// (Metrics::window_header_height), say. The mark, the title, the second
    /// word, the version and the note sit in its middle, and its rule under
    /// it. 0 is Compact's header_height, with the mark at mark_top.
    int32_t height{};
};

/// One entry of a nav list.
struct NavEntry {
    Rect rect{};           ///< the entry
    std::string caption{}; ///< its words, already looked up
    bool selected{};       ///< it is the open section
    bool hovered{};        ///< the pointer is over it, or a press on it is held
    bool focused{};        ///< the keyboard focus outlines it
    /// A tag icon at the caption's left start. None leaves the caption where it was.
    std::optional<TagIcon> tag{};
};

/// A nav list: its face, the rule at its right, an optional divider and its entries.
struct NavLook {
    Rect area{};                     ///< the list's face
    int32_t rule_column{};           ///< the column of the rule at the face's right
    std::optional<Rect> divider{};   ///< a hairline among the entries; none when unset
    std::vector<NavEntry> entries{}; ///< the entries, top to bottom
};

/// The rule, label and hint lines of one section row. A hint clip with no
/// area draws that line in the canvas's own clip.
struct RowFrame {
    int32_t top{};                         ///< the row's rule
    int32_t left{};                        ///< the rule's left column
    int32_t width{};                       ///< the rule's width
    Rect label{};                          ///< the label's box
    std::string label_text{};              ///< the label, already looked up
    std::vector<Rect> hints{};             ///< each hint line's box
    std::vector<std::string> hint_texts{}; ///< each hint line, already looked up
    std::vector<bool> hint_notices{};      ///< true draws that line in the lock colour
    std::vector<Rect> hint_clips{};        ///< a further clip for that line; empty means none
};

/// The footer's rule and the band under it.
struct FooterBandLook {
    int32_t width{};    ///< the window's width, in points
    int32_t rule_row{}; ///< the row of the line over the band
};

/// A section heading's words, already looked up. The box is the item's rectangle.
struct HeadingLook {
    std::string text{}; ///< the heading
};

/// Fills the window with the panel colour.
///
/// @param canvas where it is drawn
/// @param whole the window
void draw_window_face(const Canvas& canvas, const Rect& whole);

/// Draws the window's raised edge, the strong edge over dark. A screen
/// draws it last, over the face.
///
/// @param canvas where it is drawn
/// @param whole the window
void draw_window_edge(const Canvas& canvas, const Rect& whole);

/// Draws the header over the window's full inner width: the band and its
/// rule, the header mark, the title, the second word, the version at the
/// right and the note before it. The band is HeaderLook::height high, or
/// Compact's header_height when that is 0.
///
/// The modern fonts, when they stand in for the game's font wholly, draw
/// the title at its measured width when that is wider than title_width, and
/// the second word follows. A smooth canvas does the same from the width
/// the modern face actually draws, game glyphs or not, so the second word
/// keeps a gap. An empty string draws nothing.
///
/// @param canvas where it is drawn; its icon is the header mark's
/// @param look the words and the columns kept for them
void draw_header(const Canvas& canvas, const HeaderLook& look);

/// Draws the footer's rule and the band under it, the footer's height tall.
///
/// @param canvas where it is drawn
/// @param width the window's width, in points
/// @param rule_row the row of the line over the band
void draw_footer_band(const Canvas& canvas, int32_t width, int32_t rule_row);

/// Draws a nav list: its face, the rule at its right, its divider, and each
/// entry with its selection marker, its hover and its focus outline.
///
/// @param canvas where it is drawn
/// @param look the list
void draw_nav(const Canvas& canvas, const NavLook& look);

/// Appends a nav list: one item and one control for each entry. The control's
/// kind is a tab, and Left and Right do not act on it.
///
/// @param[in,out] list the display list
/// @param look the list
/// @param ids one control number per entry
/// @param names one automation name per entry
void add_nav(
    DisplayList& list,
    const NavLook& look,
    std::span<const ControlId> ids,
    std::span<const std::string> names
);

/// Draws a section heading in the small font, in the quiet colour, with the
/// heading's tracking.
///
/// @param canvas where it is drawn
/// @param rect the heading's box
/// @param text the heading, already looked up
void draw_heading(const Canvas& canvas, const Rect& rect, const std::string& text);

/// Draws a row's rule, its label and its hint lines. A notice hint is drawn
/// in the lock colour. A hint clip keeps that line inside the clip.
///
/// @param canvas where it is drawn
/// @param row the row's rule, label and hints
void draw_row_frame(const Canvas& canvas, const RowFrame& row);

/// Fades a rectangle into the panel, as a locked row is faded.
///
/// @param canvas where it is drawn
/// @param rect the rectangle
void draw_locked_fade(const Canvas& canvas, const Rect& rect);

/// Draws a hairline rule.
///
/// @param canvas where it is drawn
/// @param left the rule's left column
/// @param row the rule's row
/// @param width the rule's width
void draw_rule(const Canvas& canvas, int32_t left, int32_t row, int32_t width);

} // namespace oa::ui::kit
