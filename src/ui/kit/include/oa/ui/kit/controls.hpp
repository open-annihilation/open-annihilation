// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The kit's list rows, chips, text and search fields, tabs, cards and their
// grid, hover cards and links, drawn through the canvas's primitives in
// either form (canvas.hpp). Their colours are kit tokens and their sizes
// compact_metrics. Texts arrive already looked up; a field holds the
// player's own text. paint draws their items.
#pragma once

#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::kit {

/// Returns a one-bit mark's size.
///
/// @param mark which mark
/// @return its columns and rows
[[nodiscard]] Point mark_size(Mark mark) noexcept;

/// Returns a text cut to fit a width: the text itself when it fits, and
/// otherwise as many whole characters as fit before "...".
///
/// @param fonts the fonts
/// @param role the font it is drawn in
/// @param text the text, in UTF-8
/// @param width the room, in points
/// @return the text to draw; "..." alone when not even one character fits
[[nodiscard]] std::string
cut_to_width(const Fonts& fonts, FontRole role, std::string_view text, int32_t width);

// ---- List rows ----

/// Returns where a list row draws its chip: at the row's right inset,
/// centred down the row, chip_height high and as wide as chip_width, or
/// state_icon_side square for a chip that shows a state icon.
///
/// A screen whose row chip takes a press adds the chip with this rectangle
/// and its own control before the row, so that a press there reaches it.
/// A row that also shows a maker's mark uses list_row_icon_rect for that
/// press: this one stays centred, so a row with no mark is unchanged.
///
/// @param fonts the fonts
/// @param row the row
/// @param chip the chip
/// @return the chip's rectangle
[[nodiscard]] Rect list_row_chip_rect(const Fonts& fonts, const Rect& row, const ChipLook& chip);

/// Returns where a list row draws its state icon.
///
/// With no maker's mark, or when the mark would not fit under the icon,
/// this is list_row_chip_rect. When the row has a mark and a state icon and
/// is at least two icons tall, the icon is the upper square of a stack of
/// two, centred down the row, with no gap between the squares. A row with
/// no chip returns an empty rectangle.
///
/// @param fonts the fonts
/// @param row the row
/// @param look the row
/// @return the icon's rectangle; empty when the row has no chip
[[nodiscard]] Rect list_row_icon_rect(const Fonts& fonts, const Rect& row, const ListRowLook& look);

/// Returns where a list row draws its maker's mark: state_icon_side square,
/// in the state-icon column.
///
/// Under a state icon, it is the lower square of the centred stack, and
/// only when the row is at least two icons tall. With no state icon, it is
/// centred down the column. Empty when the row has no mark, or the mark
/// would not fit under an icon.
///
/// @param row the row
/// @param look the row
/// @return the mark's rectangle; empty when none is drawn
[[nodiscard]] Rect list_row_maker_rect(const Rect& row, const ListRowLook& look);

/// Draws a list row.
///
/// The face is the hover colour while hovered; a selected row is tinted
/// with the accent and outlined in it. Both are rounded with the control
/// radius of a Compact button. The badge is a square of the row's
/// height less two insets, at most list_row_badge, at the left inset and
/// centred down the row, drawn by draw_badge: the picture when it is
/// drawable, else the row's tag icon on badge_colour, else the badge text
/// on its colour, else a blank dashed square. Right of it, the title in
/// the regular font with the byline after it in the small font, in
/// by_colour or the hint colour, then each
/// line in the small font one under another, each cut with "..." to its
/// room, and the tags as chips on the line after the last. The text is
/// centred down the row; in a row short_list_row_height high it lies as the
/// Mods row's does. The aside is right-aligned in the secondary text, one entry
/// a line from the title's, left of the chip, which sits at the right inset.
/// A maker's mark, when the row has one, is drawn in that same column:
/// under the state icon when the row has one and is at least two icons
/// tall, and centred in the column when it has no state icon. A mark that
/// would not fit under an icon is left out. The row's rectangle is not
/// grown. The aside's column is as wide as its widest entry by the canvas's
/// measure and by
/// the game's fonts', whichever is wider, so that no entry is cut on either
/// form of the canvas.
///
/// @param canvas where it is drawn
/// @param rect the row; its height chooses the short form, the full form or a taller one
/// @param look what the row shows
void draw_list_row(const Canvas& canvas, const Rect& rect, const ListRowLook& look);

/// Appends a list row: one item and one control of kind list_item, which
/// does not take steps, checked while selected, its text the title.
///
/// @param[in,out] list the display list
/// @param rect the row
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_list_row(
    DisplayList& list, const Rect& rect, const ListRowLook& look, ControlId id, std::string name
);

// ---- Package badges ----

/// Draws a package badge in a square: the picture, with a control_border
/// outline, when it is drawable; else the tag's icon in on_accent on the
/// face, its square 70 percent of the badge's shorter side and centred;
/// else the letters on the face, cut to the whole letters that fit the
/// square, with no ellipsis; else a dashed blank square.
///
/// @param canvas where it is drawn
/// @param rect the badge's rectangle; a square uses its side
/// @param look what the badge shows
void draw_badge(const Canvas& canvas, const Rect& rect, const BadgeLook& look);

/// Appends a package badge: one item and no control.
///
/// @param[in,out] list the display list
/// @param rect the badge
/// @param look what the badge shows
void add_badge(DisplayList& list, const Rect& rect, const BadgeLook& look);

// ---- Drag handles ----

/// Draws a drag handle: two columns of four discs, 3 points apart and 1
/// point in radius, centred in the rectangle and worked out at the canvas's
/// own pixels. Rest is quiet, hovered is the text colour, held is the accent
/// and disabled is control_hover. Disabled wins over held, and held over hovered.
///
/// @param canvas where it is drawn
/// @param rect the handle; grip_width by grip_height in the gallery
/// @param look which colour
void draw_grip(const Canvas& canvas, const Rect& rect, const GripLook& look);

/// Appends a drag handle: one item and, for a control id, one control of
/// kind button that takes no steps, its text empty, enabled unless the
/// handle is disabled. no_control adds the item alone.
///
/// @param[in,out] list the display list
/// @param rect the handle
/// @param look which colour
/// @param id the control's number, or no_control for the item alone
/// @param name the control's automation name
void add_grip(
    DisplayList& list, const Rect& rect, const GripLook& look, ControlId id, std::string name
);

// ---- Chips ----

/// Returns a chip's width: its text in the small font and chip_padding
/// either side, or state_icon_side for a chip that shows a state icon.
///
/// @param fonts the fonts
/// @param look the chip
/// @return the width, in points
[[nodiscard]] int32_t chip_width(const Fonts& fonts, const ChipLook& look);

/// Draws a chip: a pill with its text centred in the small font, in its
/// state's colours. Get draws the hovered border and the secondary text;
/// installed, update, online and problem fill their colour's tint (ok,
/// caution, information, danger) under words in that colour; playing is
/// filled with the accent under on_accent text. A filter draws the border
/// and the secondary text, the hovered border while hovered, and while
/// selected the selected surface, an accent outline and the text colour. A
/// hovered chip that has no face takes the hover face, and one with a
/// tinted face the hovered border. A disabled chip draws the control border
/// and its text in the idle colour, whatever its state.
///
/// A chip with a state icon draws only the icon (draw_state_icon), in the
/// hint for get, the accent for installed and playing, the lock colour for
/// update, the online and danger colours, and for a filter the accent while
/// on and the hint while off; while disabled the hovered border's grey
/// (control_hover), dimmer than every state's. Hovered and enabled, it
/// takes the hover face, a disc, behind the icon.
///
/// @param canvas where it is drawn
/// @param rect the chip
/// @param look its text and state
void draw_chip(const Canvas& canvas, const Rect& rect, const ChipLook& look);

/// Appends a chip: one item and, with a control number, one control of
/// kind button that does not take steps, checked while selected, enabled
/// unless disabled, its text the chip's. With no_control only the item is
/// added, as for a chip inside a row or on a card.
///
/// @param[in,out] list the display list
/// @param rect the chip
/// @param look its look
/// @param id the control's number, or no_control for the item alone
/// @param name the control's automation name
void add_chip(
    DisplayList& list, const Rect& rect, const ChipLook& look, ControlId id, std::string name
);

// ---- Pills ----

/// Returns a pill's width: its text in the small font, the semicircle at
/// each end, and chip_padding more so the word stays clear of the border,
/// and at least the pill's height so an empty pill stays a circle. A tag
/// icon at the left, and a state icon at the right, each add their square
/// and a gap.
///
/// @param fonts the fonts
/// @param look the pill
/// @return the width, in points
[[nodiscard]] int32_t pill_width(const Fonts& fonts, const PillLook& look);

/// Returns a pill's width for the fonts the canvas will draw it with.
///
/// The smooth form measures the bundled fonts, including when the game's
/// fonts are not open yet. The width is the words plus the pill's padding,
/// and at least the pill's height, so the words are not clipped to a capsule
/// that is only as wide as it is tall.
///
/// @param canvas where the pill will be drawn
/// @param look the pill
/// @return the width, in points
[[nodiscard]] int32_t pill_width_of(const Canvas& canvas, const PillLook& look);

/// Returns a pill's height: chip_height.
///
/// @return the height, in points
[[nodiscard]] constexpr int32_t pill_height() noexcept {
    return compact_metrics.chip_height;
}

/// Draws a pill: a small-font word centred on a fully rounded ground about
/// chip_height high. A tag icon sits at the left and a state icon at the
/// right, each inside the capsule, and the word is centred in the room
/// between them. A pill is a status badge. Playing and active are filled
/// with the accent under on_accent text. Neutral and inactive are the
/// selected surface under the secondary text. Installed, update, problem,
/// information and brand fill their colour's tint (ok_tint and the others)
/// under words in that colour. Hover lightens an accent ground and outlines
/// any other in the hovered border's grey; focus draws the focus ring round
/// the pill. Disabled is the hover surface under the idle colour, and
/// ignores hover and focus. The corner is stepped at scale 1 and smooth on
/// the 150% form.
///
/// @param canvas where it is drawn
/// @param rect the pill
/// @param look its text and state
void draw_pill(const Canvas& canvas, const Rect& rect, const PillLook& look);

/// Appends a pill: one item and, with a control number, one control of kind
/// button that does not take steps, checked while its state is active,
/// enabled unless disabled. With no_control only the item is added.
///
/// @param[in,out] list the display list
/// @param rect the pill
/// @param look its look
/// @param id the control's number, or no_control for the item alone
/// @param name the control's automation name
void add_pill(
    DisplayList& list, const Rect& rect, const PillLook& look, ControlId id, std::string name
);

// ---- Text and search fields ----

/// Returns the rectangle a field's text is drawn in: inside field_inset,
/// and after the magnifier when it has one.
///
/// @param rect the field
/// @param look the field's look
/// @return the text's rectangle
[[nodiscard]] Rect field_text_rect(const Rect& rect, const SearchLook& look) noexcept;

/// Returns how far a field's text is moved left so that its caret shows.
///
/// 0 while the text before the caret and the caret fit the text's room;
/// otherwise just enough that the caret stands at the room's right.
///
/// @param fonts the fonts
/// @param rect the field
/// @param look the field's text, caret and magnifier
/// @return the columns the text is moved left
[[nodiscard]] int32_t field_scroll(const Fonts& fonts, const Rect& rect, const SearchLook& look);

/// Draws a text field: the well and its border, rounded with the control
/// radius of its height (the hover border while hovered, the accent while
/// focused, with a soft ring in field_ring filling the focus_inset points
/// outside it),
/// the magnifier when the look has one,
/// and the placeholder in the hint colour while the text is empty, else the
/// text in the regular font, moved left so the caret shows. While focused,
/// a caret_width caret in the text colour stands at the caret.
///
/// @param canvas where it is drawn
/// @param rect the field
/// @param look its text, caret and state
void draw_search(const Canvas& canvas, const Rect& rect, const SearchLook& look);

/// Appends a text field: one item and one control of kind text_field that
/// takes steps, so Left and Right go to it to move its caret, its text the
/// field's.
///
/// @param[in,out] list the display list
/// @param rect the field
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_search(
    DisplayList& list, const Rect& rect, const SearchLook& look, ControlId id, std::string name
);

// ---- Tabs ----

/// Returns each tab's rectangle, left to right from the strip's left, each
/// as tall as the strip: its caption in the small font with the look's
/// tracking, or the theme's tab_tracking, that padding either side, or the
/// theme's tab_padding, and its count after the caption. A pill shows a
/// count above 0. Words show a count of 0 or more, and skip a negative one.
///
/// @param fonts the fonts
/// @param strip the strip, tab_height high
/// @param look the captions and counts
/// @return one rectangle per caption
[[nodiscard]] std::vector<Rect>
tab_rects(const Fonts& fonts, const Rect& strip, const TabsLook& look);

/// Where a strip of tabs is placed, including a sideways scroll.
struct TabStrip {
    Rect strip{};             ///< the strip asked for
    std::vector<Rect> tabs{}; ///< each tab, in window points; it may lie outside the strip
    Rect view{};              ///< where captions are drawn, between the arrows
    Rect left_arrow{};        ///< the arrow at the left; width 0 when it is hidden
    Rect right_arrow{};       ///< the arrow at the right; width 0 when it is hidden
    int32_t scroll{};         ///< how far the tabs are shifted, in points
    int32_t limit{};          ///< the furthest the strip can scroll, in points
    bool overflows{};         ///< the tabs are wider than the strip
    std::size_t selected{};   ///< the open tab's index
};

/// Places a strip of tabs. The rectangles tab_rects returns are shifted by
/// the scroll, clamped to the strip and, while the look keeps the open tab
/// in view, so the open tab lies wholly in the view. A strip
/// whose tabs fit is not shifted and has no arrows. An arrow shows at an
/// end when more tabs lie that way.
///
/// @param fonts the fonts
/// @param strip the strip, tab_height high
/// @param look the captions, the counts and the scroll asked for
/// @return the placed strip
[[nodiscard]] TabStrip place_tabs(const Fonts& fonts, const Rect& strip, const TabsLook& look);

/// Draws a strip of tabs: a hairline along the strip's foot, each caption
/// in the small font, the selected one in the text colour over a tab_rule
/// accent rule along the foot, the hovered one in the text colour and the
/// others in the secondary text. A pill draws each count above 0 in the lock
/// colour with on_accent digits. Words draw the digits beside the caption,
/// in the lock colour where the look asks and the count is above 0, and
/// otherwise in the caption's colour. When the tabs do not fit, the captions
/// are clipped to the view and a hint chevron (text colour while hovered)
/// sits at each end that has more tabs. Without fonts only the hairline is drawn.
///
/// @param canvas where it is drawn
/// @param strip the strip
/// @param look the captions, the counts and the state
void draw_tabs(const Canvas& canvas, const Rect& strip, const TabsLook& look);

/// Appends a strip of tabs: one item for the strip and one control per tab,
/// of kind tab, which does not take steps (Left and Right move the focus),
/// checked on the selected tab, its text the look's reading when one is set
/// and otherwise the caption and its count ("Updates 2"). A tab that is not
/// wholly in the view is clipped to it. The screen opens a tab when Space or
/// Enter goes to it. An arrow that shows, and whose number is set, is a
/// button that does not take the focus. The item's look keeps the clamped
/// scroll and the view.
///
/// @param[in,out] list the display list
/// @param fonts the fonts the strip is drawn in, which place the tabs
/// @param strip the strip
/// @param look the captions, the counts and the state
/// @param ids one control number per tab
/// @param names one automation name per tab
/// @param left_id the left arrow's number; no_control adds none
/// @param left_name the left arrow's automation name
/// @param right_id the right arrow's number; no_control adds none
/// @param right_name the right arrow's automation name
void add_tabs(
    DisplayList& list,
    const Fonts& fonts,
    const Rect& strip,
    const TabsLook& look,
    std::span<const ControlId> ids,
    std::span<const std::string> names,
    ControlId left_id = no_control,
    std::string left_name = {},
    ControlId right_id = no_control,
    std::string right_name = {}
);

/// Moves a placed strip by steps. A positive step reveals the next tab that
/// is not wholly in the view, toward the right, and a negative step the
/// previous one toward the left. The result is clamped to the strip, from
/// the first tab to the last, and the open tab may leave the view. The strip
/// is the list's tabs item, and the tabs are the tab controls on its row.
///
/// @param list the display list a strip was added to
/// @param steps how many tabs to move; positive reveals tabs to the right
/// @return the scroll to ask for next, in points
[[nodiscard]] int32_t tab_scroll_step(const DisplayList& list, int32_t steps);

/// Moves a placed strip by points from a scroll it had, as a drag does. The
/// result is clamped to the strip, from the first tab to the last, and the
/// open tab may leave the view. A strip whose tabs fit stays at 0.
///
/// @param list the display list a strip was added to
/// @param from the scroll the move starts from, in points
/// @param points how far to move; positive reveals tabs to the right
/// @return the scroll to ask for next, in points
[[nodiscard]] int32_t tab_scroll_by(const DisplayList& list, int32_t from, int32_t points);

/// Returns where a press can drag a placed strip sideways: the view between
/// its arrows, as tall as the strip. A strip whose tabs fit has none.
///
/// @param list the display list a strip was added to
/// @return the view; none when the list has no strip or its tabs fit
[[nodiscard]] std::optional<Rect> tab_drag_area(const DisplayList& list);

// ---- Cards ----

/// Returns a card's height for its width: the square preview's side, the
/// title's and the subtitle's lines and three insets.
///
/// @param width the card's width, in points
/// @return the height, in points
[[nodiscard]] int32_t card_height(int32_t width) noexcept;

/// Places cards in a grid, through grid.
///
/// With columns 0, as many columns as fit at card_least_width, and at least
/// one; otherwise exactly that many. Each column is as wide as the area
/// allows, card_gap apart, the spare points going to the columns on the
/// left; fewer cards than columns keep those widths. Each cell is
/// card_height of its width high, and the rows lie card_gap apart.
///
/// @param area the area, in points
/// @param count the cards
/// @param columns the columns, or 0 for as many as fit
/// @return the cells, row by row
[[nodiscard]] std::vector<Rect>
card_cells(const Rect& area, std::size_t count, std::size_t columns = 0);

/// Draws a card: its face (the hover surface, the selected surface while
/// hovered) and outline (the accent while selected, the hover border while
/// hovered), rounded with the card radius, the square preview inset from its edges holding
/// the picture, or the placeholder centred in the hint colour on the well,
/// the chips at the preview's bottom left, then the title in the regular
/// font and the subtitle in the small font, each cut with "...". A greyed
/// card is drawn under the panel blended at locked_fade over its rounded shape.
///
/// @param canvas where it is drawn
/// @param rect the card
/// @param look what it shows
void draw_card(const Canvas& canvas, const Rect& rect, const CardLook& look);

/// Appends a card: one item and one control of kind list_item, which does
/// not take steps, so the arrows move through a grid by where cards sit;
/// checked while selected, its text the title. A greyed card still takes a
/// press.
///
/// @param[in,out] list the display list
/// @param rect the card
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_card(
    DisplayList& list, const Rect& rect, const CardLook& look, ControlId id, std::string name
);

// ---- Hover cards ----

/// Returns a hover card's size, its arrow not counted: its width, or with
/// width 0 its widest line, hover_card_padding either side and a point more
/// at the right, where letters may reach past their width; its lines,
/// small_line each, and hover_card_padding over and under them.
///
/// @param fonts the fonts
/// @param look the card
/// @return the width and height, in points
[[nodiscard]] Point hover_card_size(const Fonts& fonts, const HoverCardLook& look);

/// Returns where a hover card of a size lies by what it describes: its left
/// at the anchor's left, hover_card_arrow under the anchor, or over it when
/// the frame has no room below, kept inside the frame. A card larger than
/// the frame starts at the frame's left or top.
///
/// @param anchor what the card describes
/// @param frame the room it must stay in
/// @param size the card's size, from hover_card_size
/// @return the card's rectangle
[[nodiscard]] Rect hover_card_rect(const Rect& anchor, const Rect& frame, Point size) noexcept;

/// Draws a hover card on the band inside a border of the strong edge,
/// rounded with the card radius: the
/// header (the logo when the canvas carries the icon, otherwise the small OA
/// mark in the brand gold, the title in the text colour and the aside right-aligned in the hint
/// colour), each row (its label in the hint colour
/// in a column as wide as the widest label and hover_card_padding, its value
/// in its colour, from the left when the label is empty), the block's lines
/// in the quiet colour, every text in the small font and cut with "..." to
/// the card less its padding and, at the right, the point letters may reach
/// past their width, and
/// the arrow: a triangle in the border's colour reaching hover_card_arrow
/// out of its edge, its point arrow_at along that edge.
///
/// @param canvas where it is drawn
/// @param rect the card, its arrow outside it
/// @param look what it shows
void draw_hover_card(const Canvas& canvas, const Rect& rect, const HoverCardLook& look);

/// Appends a hover card. It adds no control.
///
/// @param[in,out] list the display list
/// @param rect the card, its arrow outside it
/// @param look its look
void add_hover_card(DisplayList& list, const Rect& rect, const HoverCardLook& look);

/// How long the pointer rests on one control before its hover card shows, in milliseconds.
inline constexpr uint64_t hover_card_delay_ms = 400;

/// When the pointer settled on the control it rests on. A screen keeps one
/// in its own state.
struct HoverTimer {
    ControlId over{no_control}; ///< the control the pointer rests on, or none
    uint64_t since_ms{};        ///< when it settled there, in milliseconds
};

/// Tells whether a hover card is due, and follows the pointer.
///
/// The wait starts when the pointer settles on a control. Another control
/// starts it again, and no control ends it. The card is due once
/// hover_card_delay_ms have passed since the pointer settled.
///
/// @param[in,out] timer the control the pointer rests on and since when
/// @param hovered the control under the pointer now, or no_control
/// @param now_ms the time now, in milliseconds, from the clock that set since_ms
/// @return true when the card is due
[[nodiscard]] bool hover_card_due(HoverTimer& timer, ControlId hovered, uint64_t now_ms) noexcept;

// ---- Links ----

/// Returns a link's width: its caption in the small font.
///
/// @param fonts the fonts
/// @param look the link
/// @return the width, in points
[[nodiscard]] int32_t link_width(const Fonts& fonts, const LinkLook& look);

/// Draws a link: its caption in the small font at the rectangle's left, in
/// the accent, the light accent while hovered and the idle colour while
/// disabled.
///
/// @param canvas where it is drawn
/// @param rect the link, small_line high
/// @param look its caption and state
void draw_link(const Canvas& canvas, const Rect& rect, const LinkLook& look);

/// Appends a link: one item and one control of kind link that does not take
/// steps, enabled as the look is, its text the caption.
///
/// @param[in,out] list the display list
/// @param rect the link
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_link(
    DisplayList& list, const Rect& rect, const LinkLook& look, ControlId id, std::string name
);

} // namespace oa::ui::kit
