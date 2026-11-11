// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Scroll bars (type-4 gadgets) and the text lists they scroll, as the gadget
// engine keeps and drives them: a bar bound to its SLIDERS art between two
// arrows, its knob moved by the pointer, by its arrows and by its list, and a
// list's first row kept in step with its bar. Each function works on one bar
// or one list; a screen keeps them with its other records and brings the
// rest of a group (the records that share an association) in line itself.

#include "oa/ui/gui_layout.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"

#include <cstdint>
#include <vector>

namespace oa::ui::gui_input {

/// Where a scroll bar's SLIDERS art comes from once bind_scroll_bar has bound it.
enum class ScrollArt : uint8_t {
    unbound, ///< not bound yet: as the GUI authors it, and it takes no pointer
    none,    ///< bound with no SLIDERS art: it has no arrows and is not drawn
    panel,   ///< the panel's own GAF
    shared,  ///< the shared COMMONGUI.GAF
};

/// First frame of a horizontal bar's art in the shared SLIDERS sequence; a vertical bar's is 0.
inline constexpr uint8_t kScrollHorizontalArt = 10;
// Frames of a bar's art, counted from its first: the track's start, middle
// and end, the knob's start, middle and end, and the back and forward
// arrows, each followed by its held face.
inline constexpr int32_t kScrollTrackStart = 0;
inline constexpr int32_t kScrollTrack = 1;
inline constexpr int32_t kScrollTrackEnd = 2;
inline constexpr int32_t kScrollKnobStart = 3;
inline constexpr int32_t kScrollKnobMiddle = 4;
inline constexpr int32_t kScrollKnobEnd = 5;
inline constexpr int32_t kScrollBackArrow = 6;
inline constexpr int32_t kScrollForwardArrow = 8;
/// Ticks a held arrow waits after its first step before it repeats once a tick.
inline constexpr int32_t kScrollRepeatDelay = 15;
/// The id a ScrollHold holds when it holds no bar.
inline constexpr int32_t kNoScrollBar = -1;

/// The size of one frame of a SLIDERS sequence.
struct ScrollFrame {
    int16_t width{};
    int16_t height{};
};

/// The SLIDERS art a panel's bars can be bound with: each sequence's frame sizes, in order.
struct ScrollArtFrames {
    std::vector<ScrollFrame> panel;  ///< the panel's own GAF; empty when it has none
    std::vector<ScrollFrame> shared; ///< the shared COMMONGUI.GAF; empty when it has none
};

/// A rectangle in panel coordinates.
struct ScrollRect {
    int16_t x{};
    int16_t y{};
    int16_t width{};
    int16_t height{};
};

/// One scroll bar and its two arrows.
///
/// The knob stands at a position from 0 to range - 1 along the bar. A bar
/// whose attributes have attribute::horizontal moves its knob across, any
/// other down; its shape (wider than high) chooses its art and where its
/// arrows go.
struct ScrollBar {
    ScrollRect rect;          ///< the bar; once bound, the part between its arrows
    ScrollRect back_arrow;    ///< the arrow that steps the knob back; empty when it has none
    ScrollRect forward_arrow; ///< the arrow that steps the knob forward; empty when it has none
    uint32_t attributes{};    ///< the GUI's attribs
    uint8_t group{};          ///< the GUI's association: a bar and the lists it scrolls share one
    int16_t range{};          ///< knob positions
    int32_t maximum{};   ///< the value the last position stands for; the GUI's thickness until set
    int16_t knob{};      ///< knob position
    int16_t knob_size{}; ///< knob length along the bar, in pixels
    ScrollArt art{};     ///< where the bar's art comes from
    uint8_t art_base{};  ///< first frame of the bar's art in its SLIDERS sequence
    bool active{};       ///< shown; a hidden bar and its arrows take no pointer
    bool grayed{};       ///< grayed out: drawn shaded and it takes no pointer
};

/// A part of a scroll bar the pointer can be on.
enum class ScrollPart : uint8_t {
    none,          ///< off the bar and its arrows
    bar,           ///< the bar, knob included
    back_arrow,    ///< the arrow that steps the knob back
    forward_arrow, ///< the arrow that steps the knob forward
};

/// A scroll bar, or one of its arrows, held down by the pointer.
struct ScrollHold {
    int32_t bar{kNoScrollBar}; ///< the id the holder gave the bar held; kNoScrollBar when none is
    ScrollPart part{};         ///< the part held
    bool dragging{};           ///< the knob was pressed and follows the pointer
    int32_t drag_origin{};     ///< pointer position along the bar when the knob was pressed
    int16_t drag_knob{};       ///< knob position when it was pressed
    int32_t pointer_x{};       ///< last pointer column, in panel coordinates
    int32_t pointer_y{};       ///< last pointer row, in panel coordinates
    int32_t repeat_delay{};    ///< ticks a held arrow still waits before it repeats
    uint32_t repeat_tick{};    ///< tick an arrow's repeat was last checked in; kept between holds
    uint32_t step_tick{};      ///< tick a held bar last stepped its knob in
};

/// A text list's rows as a scroll bar scrolls them.
struct ScrollList {
    int16_t y{};      ///< top row, in panel coordinates
    int16_t height{}; ///< height in pixels
    int16_t
        item_height{}; ///< row pitch: the GUI's itemheight, raised by scroll_list_fill; 0 for none
    int16_t count{};   ///< rows
    int16_t first{};   ///< first row shown
    int16_t selection{}; ///< selected row
    int16_t
        last_first{}; ///< first row of the last full page, found by scroll_list_fill; -1 when empty
    uint8_t group{};  ///< the GUI's association
    bool active{};    ///< shown
};

/// What a press on a list did.
enum class ScrollListPress : uint8_t {
    missed,  ///< the list is empty or the press fell outside its rows
    same,    ///< the press fell on the selected row
    changed, ///< the press moved the selection
};

// ---- Scroll bars ----

/// Returns a scroll bar gadget as the gadget engine loads it, before it is bound.
///
/// The rectangle, attributes, association, activity, range, knob and knob
/// size are the GUI's; the maximum is the GUI's thickness, as in 3.1c.
///
/// @param gadget The type-4 gadget.
/// @return The bar, unbound.
[[nodiscard]] ScrollBar scroll_bar_from_gadget(const ui::gui_layout::Gadget& gadget) noexcept;

/// Binds a bar to its art and arrows, as the first draw of its panel does in 3.1c.
///
/// The bar takes the panel's SLIDERS art from its first frame, or else the
/// shared art from frame 10 for a bar wider than high and frame 0 for any
/// other, and then the shared art's first frame sets its thickness. Each
/// arrow is the size of its frame: the back arrow at the bar's start and the
/// forward arrow at its end, and the bar shrinks to lie between them. A bar
/// wider than high then takes the width of frame 5 as its knob and its width
/// less the knob and 4 as its range; any other bar keeps its range. Without
/// art a bar has no arrows and its range is its longer side less 6. The knob
/// goes to position 0.
///
/// @param[in,out] bar Bar to bind.
/// @param art SLIDERS frames the bar's panel can use.
void bind_scroll_bar(ScrollBar& bar, const ScrollArtFrames& art) noexcept;

/// Tells which part of a shown bar lies under a point.
///
/// An arrow covers its rectangle, right and bottom edges excluded, and wins
/// over the bar; the bar covers its rectangle with the column after its
/// right edge and the row after its bottom edge, as in 3.1c.
///
/// @param bar Bar tested.
/// @param x Column in panel coordinates.
/// @param y Row in panel coordinates.
/// @return The part under the point; none for a hidden or unbound bar.
[[nodiscard]] ScrollPart scroll_bar_part(const ScrollBar& bar, int32_t x, int32_t y) noexcept;

/// Returns the rectangle a press grabs a bar's knob in, edges included.
///
/// Across the bar it starts a pixel in and is the bar's thickness less 2
/// wide; along it, it starts 1 pixel past the knob's position across a
/// horizontal bar and 2 pixels past it down any other, and runs the knob's
/// size and a pixel.
///
/// @param bar Bar measured.
/// @return The rectangle.
[[nodiscard]] ui::gui_layout::GadgetRect scroll_knob_rect(const ScrollBar& bar) noexcept;

/// Tells whether a bar takes the pointer: bound, shown and not grayed.
///
/// A bar whose attributes have attribute::text_list is drawn grayed and
/// takes no pointer either, as in 3.1c.
///
/// @param bar Bar tested.
/// @return True when the pointer can move its knob.
[[nodiscard]] bool scroll_takes_input(const ScrollBar& bar) noexcept;

/// Moves a bar's knob to the position of a value.
///
/// The position is value / maximum * (range - 1) in double precision, with
/// the value first lowered to the maximum; a whole position is kept, and any
/// other has 1 added before it is truncated toward zero.
///
/// @param[in,out] bar Bar to move; without a maximum or with fewer than two positions the knob goes to 0.
/// @param value Value; one above the maximum counts as the maximum.
/// @quirk A negative value is not raised to 0, so it puts the knob before
///        the bar's start, as in 3.1c.
void scroll_set_value(ScrollBar& bar, int32_t value) noexcept;

/// Returns the value a bar's knob stands for: knob / (range - 1) * maximum, truncated.
///
/// @param bar Bar read.
/// @return The value; 0 with fewer than two positions.
[[nodiscard]] int32_t scroll_value(const ScrollBar& bar) noexcept;

/// Steps a bar's knob once as its arrow does: one position toward the bar's start or end, unless it is there.
///
/// @param[in,out] bar Bar to step; the knob stays within 0..range - 1.
/// @param forward True to step toward the bar's end.
void scroll_step(ScrollBar& bar, bool forward) noexcept;

/// How many knob positions Page Up and Page Down move a scroll bar.
///
/// A bar with fewer than two positions moves one. Any other moves a tenth of
/// the travel from its start to its end, and at least one position.
///
/// @param range the bar's range, the count of knob positions
/// @return the page step, at least 1
[[nodiscard]] int16_t scroll_page_span(int16_t range) noexcept;

/// Presses the pointer on a bar, as 3.1c takes a press on a scroll bar or its arrow.
///
/// Whatever the pointer is on, the press ends the hold before it. An arrow of
/// a bar that takes input steps the knob one position toward its end at
/// once, unless the knob is there already, and starts its repeat delay. A
/// press on such a bar holds it and, on the knob, starts dragging it; it
/// does not move the knob. A press anywhere else is not held.
///
/// @param[in,out] bar Bar under the pointer.
/// @param[in,out] hold Held state; replaced by the press.
/// @param id The holder's id for the bar, kept in the hold.
/// @param x Pointer column in panel coordinates.
/// @param y Pointer row in panel coordinates.
/// @param tick Current 30 Hz tick.
/// @return True when the bar's handler must run, which every arrow press
///         gives even when the knob stays.
bool scroll_press(
    ScrollBar& bar, ScrollHold& hold, int32_t id, int32_t x, int32_t y, uint32_t tick
) noexcept;

/// Moves the pointer; a dragged knob follows it at the next scroll_hold_tick.
///
/// @param[in,out] hold Held state; the pointer position is kept.
/// @param x Pointer column in panel coordinates.
/// @param y Pointer row in panel coordinates.
void scroll_move(ScrollHold& hold, int32_t x, int32_t y) noexcept;

/// Releases the pointer, ending any hold.
///
/// A held bar stops dragging and then moves its knob one position toward the
/// pointer when the pointer lies before or after the knob, as in 3.1c; a
/// quick click on the bar so moves it one position.
///
/// @param[in,out] bar The bar the hold holds; ignored when the hold holds an arrow.
/// @param[in,out] hold Held state; cleared, its arrow repeat's last tick kept.
/// @param x Pointer column in panel coordinates.
/// @param y Pointer row in panel coordinates.
/// @return True when the knob moved.
bool scroll_release(ScrollBar& bar, ScrollHold& hold, int32_t x, int32_t y) noexcept;

/// Ends a hold without moving anything, keeping its arrow repeat's last tick.
///
/// @param[in,out] hold Held state.
void scroll_let_go(ScrollHold& hold) noexcept;

/// Runs a held bar or arrow for one update of its screen.
///
/// A dragged knob moves by as many positions as the pointer has moved pixels
/// along the bar since the press, kept within 0..range - 1, in every update,
/// so the bar's handler runs once for all the pointer's moves since the last
/// update, as in 3.1c. A held bar that is not dragged moves its knob one
/// position toward the last pointer position in each new tick. A held arrow
/// counts its repeat delay down, one in each new tick, and once the delay is
/// spent steps its bar once a tick. A bar that stops taking input ends the
/// hold.
///
/// @param[in,out] bar The bar the hold holds.
/// @param[in,out] hold Held state.
/// @param tick Current 30 Hz tick.
/// @return True when the bar's handler must run, which every arrow step
///         gives even when the knob stays.
bool scroll_hold_tick(ScrollBar& bar, ScrollHold& hold, uint32_t tick) noexcept;

// ---- Lists ----

/// Returns a list gadget as the gadget engine loads it.
///
/// @param gadget The type-2 gadget.
/// @return The list with no rows.
[[nodiscard]] ScrollList scroll_list_from_gadget(const ui::gui_layout::Gadget& gadget) noexcept;

/// Readies a shown list as 3.1c's first draw of its panel does.
///
/// The list starts on its first row with that row selected, and loses the
/// height past its last whole line height + 2 pixels: 193 pixels become 192
/// with a 14-pixel line height.
///
/// @param[in,out] list List readied; a hidden one is left as it is.
/// @param line_height The panel font's line height; 0 keeps the height.
void scroll_list_trim(ScrollList& list, int32_t line_height) noexcept;

/// Returns a list's row pitch: its own, or the line height + 1 without one, and at least 1.
///
/// @param list List read.
/// @param line_height The panel font's line height.
/// @return The pitch in pixels.
[[nodiscard]] int32_t scroll_list_pitch(const ScrollList& list, int32_t line_height) noexcept;

/// Returns the rows a list's page holds when the keys and a screen move its
/// selection: its height less 2 over the line height + 1.
///
/// @param list List read.
/// @param line_height The panel font's line height.
/// @return The rows.
[[nodiscard]] int32_t scroll_list_page_rows(const ScrollList& list, int32_t line_height) noexcept;

/// Fills a list with rows as 3.1c fills a text list.
///
/// The pitch becomes at least the line height + 1, the first row and the
/// selection go to 0, and the first row of the last full page is found by
/// fitting rows from the end into the list's height.
///
/// @param[in,out] list List filled.
/// @param count Rows; lowered to 32767.
/// @param line_height The panel font's line height.
/// @return True when the rows do not all fit: its scroll bar is to be shown.
bool scroll_list_fill(ScrollList& list, int32_t count, int32_t line_height) noexcept;

/// Sizes a bar's knob and range from the text list it scrolls, as 3.1c does.
///
/// A page holds (height - 2) / pitch whole rows, the pitch being at least
/// the line height + 1. The knob is rows / count * (bar height - 3),
/// truncated, and at least 10 pixels; the range is the bar's height less the
/// knob and 3, or 0 when every row fits.
///
/// @param[in,out] bar Bar sized.
/// @param list The list it scrolls.
/// @param line_height The panel font's line height.
void scroll_bar_fit_list(ScrollBar& bar, const ScrollList& list, int32_t line_height) noexcept;

/// Moves a list's first row to where its bar's knob stands, as 3.1c does after the knob moves.
///
/// The first row becomes (count - height / pitch) * knob / (range - 1),
/// truncated, or 0 with fewer than two positions. The selection stays.
///
/// @param[in,out] list List that follows.
/// @param bar The bar that moved.
/// @return False for a list with no pitch, which is left as it is.
bool scroll_list_follow_bar(ScrollList& list, const ScrollBar& bar) noexcept;

/// Moves a bar's knob to where its list's first row stands, as 3.1c does after the list scrolls.
///
/// The knob goes to first * range / last page's first row, truncated, or 0
/// when the last page starts on the first row.
///
/// @param[in,out] bar Bar that follows; left as it is for a list of fewer than two rows.
/// @param list The list that scrolled.
/// @quirk A list on its last page puts the knob at range, one position past
///        the last, as in 3.1c; the bar is drawn with the knob at its end.
void scroll_bar_follow_list(ScrollBar& bar, const ScrollList& list) noexcept;

/// Selects a row of a list and brings it into view, as 3.1c does when a screen picks a row.
///
/// The selection is always stored. A row off the page becomes the first row
/// shown, no further than the last page's first row, and the bar, when there
/// is one and the list has more than a page, moves its knob to
/// range * first / last page's first row, truncated.
///
/// @param[in,out] list List whose row is picked.
/// @param[in,out] bar Its scroll bar, or null.
/// @param line_height The panel font's line height.
/// @param row Row to select.
void scroll_list_select(
    ScrollList& list, ScrollBar* bar, int32_t line_height, int32_t row
) noexcept;

/// Moves a list's selection one row, as the Up and Down keys do in 3.1c.
///
/// A selection on the page moves one row toward the list's start or end and
/// the list scrolls a row when it crosses the page's edge; it stops at row 0,
/// at the last row, and at the last page's first row + rows - 1. A selection
/// off the page is brought into view instead, as scroll_list_select does.
///
/// @param[in,out] list List stepped.
/// @param[in,out] bar Its scroll bar, or null; it moves only when the selection was off the page.
/// @param line_height The panel font's line height.
/// @param forward True to move toward the list's end.
/// @return True when the selection moved on the page: the rest of the list's group is to follow it.
bool scroll_list_step(ScrollList& list, ScrollBar* bar, int32_t line_height, bool forward) noexcept;

/// Finds the row a press on a list picks, as 3.1c does.
///
/// Rows start 2 pixels below the list's top and end 4 pixels above its
/// bottom, one pitch apart; a press outside them is missed. The row is kept
/// on the page and within the rows.
///
/// @param list List pressed.
/// @param line_height The panel font's line height.
/// @param y Pointer row in panel coordinates.
/// @param[out] row The row picked; left as it is when the press is missed.
/// @return What the press does to the selection.
ScrollListPress
scroll_list_press(const ScrollList& list, int32_t line_height, int32_t y, int32_t& row) noexcept;

/// Scrolls a list with a pitch by whole rows within its first row and its last page's first row.
///
/// This is the mouse wheel's own scroll, which 3.1c does not have.
///
/// @param[in,out] list List scrolled.
/// @param rows Rows to scroll, negative toward the start.
/// @return True when the first row changed.
bool scroll_list_scroll(ScrollList& list, int32_t rows) noexcept;

} // namespace oa::ui::gui_input
