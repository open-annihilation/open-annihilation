// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace oa::ui::frontend_state {

/// A rectangle in points. Edges that only meet do not overlap.
struct PlaceRect {
    int32_t x{};      ///< the left edge
    int32_t y{};      ///< the top edge
    int32_t width{};  ///< the width; the right edge is x + width
    int32_t height{}; ///< the height; the bottom edge is y + height
};

/// The sizes of the button and the lines under it, in points.
struct BlockSizes {
    int32_t button_width{};     ///< the button's width
    int32_t button_height{};    ///< the button's height
    int32_t line_height{};      ///< one summary line's height
    int32_t line_width{};       ///< a line's width before it is shortened
    int32_t least_line_width{}; ///< a line shorter than this is left out
    int32_t gap{};              ///< the gap between the block and a rectangle it would meet
    std::size_t lines{};        ///< how many lines the block asks for, under the button
    /// Width to the right of the button, in points, that must miss every
    /// other rectangle and stay in the room: a chip and the gap before it.
    /// Zero when the block has nothing beside the button.
    int32_t beside{};
    /// Widths of the buttons on one row, left to right. Empty means one
    /// button of `button_width`, so a block that names one width is unchanged.
    /// One width is that button. Several stand `button_gap` apart, and the
    /// row's right edge lies on the anchor's right edge.
    std::vector<int32_t> button_widths{};
    /// The gap between two buttons of that row, in points. Unused for one button.
    int32_t button_gap{};
};

/// Where the button and its lines were placed. The lines' right edge, and the
/// rightmost button's, is the anchor's.
struct BlockPlace {
    PlaceRect button{};               ///< the rightmost button, or the only one
    std::vector<PlaceRect> buttons{}; ///< every button, left to right
    std::vector<PlaceRect> lines{};   ///< the lines that were kept, top to bottom
};

/// Places a button, or a row of buttons, and the lines under it against an anchor.
///
/// The lines sit under the row, one point apart. The lines' right edge, and
/// the rightmost button's, lie on the anchor's right edge. `button_widths`
/// empty, or one width, is one button of that width: the place is the one a
/// single `button_width` gives. Several buttons stand left to right,
/// `button_gap` apart. The place just under the anchor, a gap below it, is
/// tried first. When that place meets another rectangle or leaves the room,
/// the place just over the anchor is tried, and the block moves up past
/// every rectangle it meets, leaving the gap as it passes. A line spans
/// line_width to the left of the right edge, shortens from the left so it
/// ends before any rectangle it would meet, and is left out when less than
/// least_line_width remains. A strip `beside` wide, as tall as the button
/// and starting at the rightmost button's right edge, is part of the same
/// meeting test: a rectangle it would meet moves the block, as one a button
/// would meet does. Lines drop from the last while the block cannot be
/// placed whole. A place that leaves the room is refused. When no place
/// holds the row, the place under the anchor is used.
///
/// @param anchor the gadget the button is anchored to
/// @param others every other rectangle the block must miss
/// @param room the frame the block must stay inside
/// @param sizes the button, the lines and the gap
/// @return the place
[[nodiscard]] BlockPlace place_block(
    const PlaceRect& anchor,
    std::span<const PlaceRect> others,
    const PlaceRect& room,
    const BlockSizes& sizes
);

/// Returns the nearest rectangle under the anchor that shares some of its columns.
///
/// @param anchor the anchor
/// @param others the rectangles
/// @return the rectangle whose top is nearest under the anchor's bottom, the
///     leftmost of a tie; nothing when none is under it
[[nodiscard]] std::optional<PlaceRect>
next_below(const PlaceRect& anchor, std::span<const PlaceRect> others) noexcept;

/// Places a button the size of the gadget under the anchor, in the anchor's column.
///
/// The gadget is next_below's. The button is as wide and as tall as it,
/// shares its left edge, and is centred down the space between the anchor's
/// bottom and that gadget's top, so the gaps above and below are even.
///
/// @param anchor the gadget the button stands under
/// @param others every other rectangle, that gadget among them
/// @param room the frame the button must stay inside
/// @param gap the least gap above and below
/// @return the button, or nothing when no gadget is under the anchor, the
///     space does not hold the button with the gap, or the button meets a
///     rectangle or leaves the room
[[nodiscard]] std::optional<PlaceRect> place_in_column(
    const PlaceRect& anchor, std::span<const PlaceRect> others, const PlaceRect& room, int32_t gap
);

/// Returns the middle of a bar of two parts: the space between the left
/// part's right edge and the right part's left edge, set in from each by
/// the bolt between a part and the middle, nine twentieths of the left
/// part's height, as tall as the left part.
///
/// @param left the bar's left part
/// @param right the bar's right part
/// @return the middle; empty when the parts leave no room
[[nodiscard]] PlaceRect bar_middle(const PlaceRect& left, const PlaceRect& right) noexcept;

/// Places a button the size of a bar's middle under it, the left edges together.
///
/// The button is bar_middle's rectangle moved down by its height and a gap
/// as wide as the bolt between a part and the middle, so the gap under the
/// bar is even with the bar's own.
///
/// @param left the bar's left part
/// @param right the bar's right part
/// @param others every rectangle the button must miss
/// @param room the frame the button must stay inside
/// @return the button, or nothing when it meets a rectangle or leaves the room
[[nodiscard]] std::optional<PlaceRect> place_under_middle(
    const PlaceRect& left,
    const PlaceRect& right,
    std::span<const PlaceRect> others,
    const PlaceRect& room
);

/// Places a button the size of the anchor over it, the left edges together.
///
/// The button is centred down the space between the bottom of the last
/// rectangle over the anchor that shares its columns, or the room's top,
/// and the anchor's top, so the gaps above and below are even.
///
/// @param anchor the gadget the button stands over
/// @param others every other rectangle
/// @param room the frame the button must stay inside
/// @param gap the least gap above and below
/// @return the button, or nothing when the space does not hold it with the
///     gap, or it meets a rectangle or leaves the room
[[nodiscard]] std::optional<PlaceRect> place_over(
    const PlaceRect& anchor, std::span<const PlaceRect> others, const PlaceRect& room, int32_t gap
);

/// Places a button to the right of an anchor, centred on it.
///
/// The button's left edge is `gap` past the anchor's right edge. It is
/// `height` high and centred down the anchor, so a shorter button sits
/// inside the anchor's rows and a taller one sticks out evenly. It is as
/// wide as `most_width`, and shorter when a rectangle to its right shares
/// any of its rows: it then ends `gap` before the first such rectangle.
/// A rectangle with no area is skipped. Nothing is returned when the width
/// left is under `least_width`, or the button would leave the room, or it
/// would meet a rectangle.
///
/// @param anchor the gadget the button stands beside
/// @param others every other rectangle the button must miss
/// @param room the frame the button must stay inside
/// @param gap the gap after the anchor, and before a rectangle that limits it
/// @param height the button's height
/// @param least_width a button narrower than this is left out
/// @param most_width the button's width when nothing limits it
/// @return the button, or nothing when it does not fit
[[nodiscard]] std::optional<PlaceRect> place_beside(
    const PlaceRect& anchor,
    std::span<const PlaceRect> others,
    const PlaceRect& room,
    int32_t gap,
    int32_t height,
    int32_t least_width,
    int32_t most_width
);

/// Returns the button's place in Tab's order.
///
/// Records are ordered by y * 5000 + x of the top left, as the gadget
/// engine orders them. The count is how many of `focusable` come at or
/// before the button, so the button follows a record that shares its key.
///
/// @param focusable the records that take the focus, in any order
/// @param button the button
/// @return how many records come before the button
[[nodiscard]] std::size_t
focus_place(std::span<const PlaceRect> focusable, const PlaceRect& button);

/// Returns the squared distance from a point to a rectangle, in points.
///
/// A point inside the rectangle is no distance. The right and bottom edges
/// are outside, as a control's press area is. A rectangle with no area is
/// as far as a distance can be, so it never wins a comparison.
///
/// @param x the point's column
/// @param y the point's row
/// @param rect the rectangle
/// @return the squared distance
[[nodiscard]] int64_t point_distance_squared(int32_t x, int32_t y, const PlaceRect& rect) noexcept;

/// Tells whether some gadget is nearer the point than the button.
///
/// A point inside a gadget belongs to that gadget, even when it is also
/// inside the button. Otherwise the button keeps the point unless a
/// gadget's squared distance is strictly less. A tie belongs to the button.
/// A gadget with no area is skipped.
///
/// @param x the point's column, in the same points as the rectangles
/// @param y the point's row
/// @param button the button
/// @param gadgets every gadget of the screen under the button
/// @return true when a gadget is nearer, or the point lies inside one
[[nodiscard]] bool nearer_gadget(
    int32_t x, int32_t y, const PlaceRect& button, std::span<const PlaceRect> gadgets
) noexcept;

} // namespace oa::ui::frontend_state
