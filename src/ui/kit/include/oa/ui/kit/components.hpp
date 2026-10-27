// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The kit's controls, drawn through the canvas's primitives (canvas.hpp).
// One button, five looks. Captions come already looked up. paint draws a
// display list's items in order.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace oa::ui::kit {

/// Draws a one-bit mark.
///
/// @param canvas where it is drawn
/// @param mark which mark
/// @param at the mark's top left, in points
/// @param ink the colour of its set pixels
void draw_mark(const Canvas& canvas, Mark mark, Point at, Colour ink);

/// Draws a state icon, filling a square of the rectangle's shorter side
/// centred in it.
///
/// A circled check, a circled up-arrow, a download arrow and a key are one
/// colour, `ink`. The key has no ring. They are worked out from rings and
/// strokes at the canvas's own pixels (covered_pixels), each pixel's share
/// counted from 16 samples in integers, and drawn through blend_coverage,
/// so they are as crisp at 2x, 3x and 150% as at 1x. The design is a square
/// of 24 units: the ring is 2 units wide with its outer edge a unit in, and
/// every stroke is 2.5 units wide with round ends.
///
/// The OA badge and the bone ignore `ink`. When an app icon has been given
/// (`set_open_annihilation_mark`), the badge is that picture, drawn through
/// `draw_picture` from the copy kept for the mark's size, so a small mark
/// is filtered down and not taken from the nearest pixel of the full icon.
/// With no picture, the badge is a rounded plate (red 60, green 64, blue 56)
/// with the letters in the lock colour, sampled so a 2x drawing is finer
/// than a doubled 1x drawing and 150% keeps a partial edge. The bone is
/// always that shape: off-white (red 244, green 239, blue 224) with a dark
/// outline (red 12, green 10, blue 8).
///
/// Pixels an icon does not cover are left as they are.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points; state_icon_side square in a chip
/// @param icon which icon
/// @param ink the colour of a one-colour icon; ignored for the badge and the bone
void draw_state_icon(const Canvas& canvas, const Rect& rect, StateIcon icon, Colour ink);

/// Draws a tag icon, filling a square of the rectangle's shorter side,
/// centred in it.
///
/// The eight catalogue icons are strokes on a design of 24 units, each
/// stroke 2.5 units wide with round ends, the same width as a state icon.
/// They are worked out at the canvas's own pixels and drawn through
/// blend_coverage, so a 2x drawing is not a doubled 1x drawing. A rectangle
/// more than 1024 pixels on a side draws nothing, and so does an undrawable
/// canvas. Pixels the icon does not cover are left as they are.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param icon which icon
/// @param ink the colour of the strokes
void draw_tag_icon(const Canvas& canvas, const Rect& rect, TagIcon icon, Colour ink);

/// The app icon drawn as the small maker's mark on a Library row or a
/// registry card. The spans point at pixels kept elsewhere for as long as
/// the mark is set. An empty full picture leaves the drawn plate.
struct OpenAnnihilationMark {
    oa::ui::frontend_renderer::RgbaPicture full{};     ///< the icon's visible part
    oa::ui::frontend_renderer::RgbaPicture prepared{}; ///< that icon filtered down
};

/// Gives the kit the app icon it draws as the maker's mark. An empty mark
/// puts the drawn plate back.
///
/// @param mark the pictures; their pixels must outlive the mark
void set_open_annihilation_mark(OpenAnnihilationMark mark);

/// Returns the mark set_open_annihilation_mark last gave, or an empty one.
///
/// @return the mark
[[nodiscard]] const OpenAnnihilationMark& open_annihilation_mark() noexcept;

/// Draws the keyboard focus ring, the one ring every control shows: a ring
/// focus_ring points thick in the light accent, its corners rounded as a
/// control of the control's height is (control_radius_for). When around is
/// set its outside lies focus_inset points out from the control, the room
/// the screens keep round a control, so a point clear of the control stays
/// between them, and its corners are that much rounder; otherwise it lies
/// on the inside of the control's own edge.
///
/// @param canvas where it is drawn
/// @param control the control
/// @param around true draws it outside the control; false on the control's edge
void draw_focus_ring(const Canvas& canvas, const Rect& control, bool around);

/// Where a button that wears the OA badge draws the badge and its caption.
struct ButtonMarkParts {
    Rect mark{};    ///< the badge's square, at the left
    Rect caption{}; ///< the caption's box, after the badge
};

/// Returns the corner radius of a filled button: a quarter of its shorter
/// side, at least 2 points, so it grows with the button.
///
/// @param area the button
/// @return the radius, in points
[[nodiscard]] int32_t filled_radius(const Rect& area) noexcept;

/// Places the OA badge and the caption in a button that wears the badge.
///
/// The padding is a fifth of the button's height, at least 2 points. The
/// badge is a square as tall as the button less the padding above and below,
/// the padding in from the left edge. The caption's box starts the padding
/// after the badge and ends the padding before the right edge, as tall as
/// the button, so the left padding, the gap and the right padding are even.
///
/// @param area the button
/// @return the badge and the caption's box; empty boxes when the button is too small
[[nodiscard]] ButtonMarkParts button_mark_parts(const Rect& area) noexcept;

/// Draws a button in one of its looks, with rounded corners: the control
/// radius of its height (control_radius_for), and a filled button's
/// filled_radius. Its edge and face are drawn as two rounded shapes. A
/// quiet button at rest draws its caption alone. A button that is not
/// enabled draws no face, the control border and the idle caption,
/// whatever the pointer is doing. The caption is centred in the look's
/// font, small unless it says regular. A look that wears the OA badge draws
/// the badge at the left and the caption after it, keeping to its left edge
/// (button_mark_parts); the caller fits the caption to that box. A refresh
/// mark draws a circular arrow in place of the caption: the quiet look's
/// face while the pointer is over it, no face while it cannot be pressed,
/// and the arrow in the text colour while it is turning. When the look's
/// check has been showing, the arrow eases into a check in the accent, then
/// the arrow returns.
/// A close mark draws an × the same way, in the text colour, on that same face.
/// A caption with a refresh arrow (ButtonLook::refresh_icon) draws the arrow
/// and the caption side by side, centred together, in the caption's colour.
///
/// @param canvas where it is drawn
/// @param area the button
/// @param look its caption, style and state
void draw_button(const Canvas& canvas, const Rect& area, const ButtonLook& look);

/// Returns the points a refresh arrow adds before a button's caption
/// (ButtonLook::refresh_icon): the arrow, as tall as the button less two
/// points above and below and at most a state icon's side, and the gap
/// after it. A layout widens the button by it.
///
/// @param height the button's height, in points
/// @return the points; 0 for a button too short to hold an arrow
[[nodiscard]] int32_t refresh_icon_room(int32_t height) noexcept;

/// How long one full turn of a refresh mark, or of the OA button's spinner,
/// takes, in milliseconds. The place on the turn comes from this time, so it
/// is the same at every frame rate.
inline constexpr uint32_t refresh_revolution_ms = 1600;

/// How long a refresh mark shows its check after a refresh that read a list,
/// in milliseconds, and then the arrow again.
inline constexpr uint32_t refresh_check_ms = 800;

/// How long the arrow takes to ease into that check, in milliseconds, counted
/// inside refresh_check_ms.
inline constexpr uint32_t refresh_ease_ms = 160;

/// Returns the eighth of a turn a refresh mark or the OA button's spinner
/// shows after this long turning. Eight eighths are one refresh_revolution_ms.
/// The same time is the same eighth however many frames drew it.
///
/// @param elapsed_ms how long it has been turning, in milliseconds
/// @return the eighth, 0 to 7
[[nodiscard]] constexpr uint8_t refresh_eighth(uint32_t elapsed_ms) noexcept {
    constexpr uint32_t place_ms = refresh_revolution_ms / 8u;
    return static_cast<uint8_t>((elapsed_ms / place_ms) % 8u);
}

/// Returns how far a refresh mark's check has eased in: 0 is still the arrow
/// and 255 is the check. The ease takes a few steps across refresh_ease_ms,
/// then holds. Past refresh_check_ms, or at zero, the mark is not showing it.
///
/// @param shown_ms how long the check has been showing, in milliseconds
/// @return 0, or a step up to 255
[[nodiscard]] constexpr uint8_t refresh_check_ease(uint32_t shown_ms) noexcept {
    if (shown_ms == 0 || shown_ms >= refresh_check_ms)
        return 0;
    if (shown_ms >= refresh_ease_ms)
        return 255;
    constexpr uint32_t steps = 4;
    const uint32_t step = shown_ms * steps / refresh_ease_ms;
    if (step == 0)
        return 0;
    return static_cast<uint8_t>(step * 255u / (steps - 1u));
}

/// Returns where a switch's track lies in its area: switch_track_width by
/// switch_height points, each held to the area's, at the area's right and
/// centred down it.
///
/// @param area the switch's area
/// @return the track
[[nodiscard]] Rect switch_track(const Rect& area) noexcept;

/// Draws an Off/On switch: a pill track with a round knob two points in,
/// lime with the knob at the right in the dark ink while On, the selected
/// surface with the knob at the left while Off. A locked On track is the
/// hovered border's grey. The captions name the switch's halves to
/// automation; the switch does not draw them.
///
/// @param canvas where it is drawn
/// @param area the switch
/// @param look its state and its two captions
void draw_switch(const Canvas& canvas, const Rect& area, const SwitchLook& look);

/// Where a radio's parts lie in its area.
struct RadioParts {
    Rect mark{};    ///< the ring's square
    Rect caption{}; ///< a radio row's caption's box; empty for the mark alone
};

/// Returns where a radio's parts lie. The mark alone is the largest square
/// centred in the area. A radio row's mark is half the row's height
/// (14, 17 and 19 points at Kit 2's control heights), at least 8 points,
/// centred down the row and as far in from its left as from its top; the
/// caption's box runs from that far past the mark to that far short of the
/// row's right edge, the row's height. Callers fit the caption to that box.
///
/// @param area the radio's area
/// @param row true for a radio row, false for the mark alone
/// @return the mark and the caption's box; an empty caption box when the row has no room
[[nodiscard]] RadioParts radio_parts(const Rect& area, bool row) noexcept;

/// Draws a radio. The mark is a one-point ring: the idle switch grey while
/// not chosen, the accent while chosen, lighter while hovered and deeper
/// while held, with the accent's dot half the ring across in it while
/// chosen. A disabled radio's ring and dot are the hovered border's grey. A
/// radio row is a box with the control radius of its height round them:
/// the selected surface with the accent's edge while chosen, the hover face
/// with the hovered border while hovered, else no face and the control
/// border; its caption in the regular font, in the text colour, the idle
/// colour while disabled.
///
/// @param canvas where it is drawn
/// @param area the radio's area: the mark's square, or the whole row
/// @param look its state and, for a row, its caption
void draw_radio(const Canvas& canvas, const Rect& area, const RadioLook& look);

/// Draws a strip of levels. Levels from offered on are faded into the panel.
///
/// @param canvas where it is drawn
/// @param area the strip
/// @param look its captions, the chosen level and its state
void draw_levels(const Canvas& canvas, const Rect& area, const LevelsLook& look);

/// Returns the level of a strip under a column.
///
/// @param area the strip
/// @param levels how many levels it holds
/// @param level_width one level's columns, inside the border
/// @param column the column, in points
/// @return the level's index, from 0; the nearest end for a column outside it
[[nodiscard]] std::size_t
level_at(const Rect& area, std::size_t levels, int32_t level_width, int32_t column) noexcept;

/// Draws a slider: its track filled up to the knob, its stops and its knob.
///
/// @param canvas where it is drawn
/// @param track_area the slider's area, the track within it
/// @param look the stop, the stops and its state
void draw_slider(const Canvas& canvas, const Rect& track_area, const SliderLook& look);

/// Returns the column a slider's knob is centred on.
///
/// @param track the slider's area
/// @param stop the stop
/// @param stops the slider's stops
/// @return the column, in points
[[nodiscard]] int32_t knob_column(const Rect& track, int32_t stop, int32_t stops) noexcept;

/// Returns the stop nearest a column on a slider.
///
/// @param track the slider's area
/// @param column the column, in points
/// @param stops the slider's stops
/// @return the stop, 0 to stops - 1
[[nodiscard]] int32_t stop_at(const Rect& track, int32_t column, int32_t stops) noexcept;

/// Draws a drop-down's field: the choice at its left and the arrow at its right.
///
/// @param canvas where it is drawn
/// @param field the field
/// @param look the choice it shows, and whether it is open
void draw_choice(const Canvas& canvas, const Rect& field, const ChoiceLook& look);

/// Draws a drop-down's open menu over what is under it.
///
/// @param canvas where it is drawn
/// @param menu the menu
/// @param look the items on screen, the chosen one and the marked one
void draw_choice_menu(const Canvas& canvas, const Rect& menu, const ChoiceMenuLook& look);

/// Returns how many items an open menu shows at once.
///
/// @param choices the choices it offers
/// @return at most the compact most_shown_choices
[[nodiscard]] int32_t shown_choices(std::size_t choices) noexcept;

/// The settings dialog's footer line at Compact: the row choice_menu keeps
/// a menu above unless it is given another.
inline constexpr int32_t compact_footer_line =
    compact_metrics.dialog_height - compact_metrics.edge - compact_metrics.footer_height - 1;

/// Returns where a drop-down's open menu lies: under its field, as wide as
/// the field and as tall as the items it shows, and over the field when that
/// would pass the screen's footer line.
///
/// @param field the drop-down's field
/// @param choices the choices it offers
/// @param footer_line the row of the screen's footer line: the settings
///     dialog's at its size class, Compact's when not given
/// @return the menu's rectangle
[[nodiscard]] Rect choice_menu(
    const Rect& field, std::size_t choices, int32_t footer_line = compact_footer_line
) noexcept;

/// Returns one shown item's rectangle in an open menu.
///
/// @param menu the menu
/// @param place the item's place among those shown, from 0
/// @return the item's rectangle, inside the menu's border
[[nodiscard]] Rect choice_item(const Rect& menu, int32_t place) noexcept;

/// Returns the shown item under a point.
///
/// @param menu the menu
/// @param x the point's column, in points
/// @param y the point's row, in points
/// @return the item's place among those shown, from 0; -1 when the point is
///     on the border or outside the menu
[[nodiscard]] int32_t choice_item_at(const Rect& menu, int32_t x, int32_t y) noexcept;

/// Moves a marked item by a number of choices, skipping a divider it would
/// land on, one further in the same direction.
///
/// Home and End are a move to the ends, which are not dividers. A step of 0
/// stays. A landing that cannot leave a divider stays where it was.
///
/// @param look the menu, whose items name the dividers
/// @param marked the item marked now, from 0
/// @param step how many choices to move, negative towards the first
/// @return the item to mark
[[nodiscard]] int32_t
choice_mark_toward(const ChoiceMenuLook& look, int32_t marked, int32_t step) noexcept;

/// Draws the padlock and a lock's text, ending at the area's right.
///
/// @param canvas where it is drawn
/// @param area the lock's area
/// @param look the text
void draw_lock(const Canvas& canvas, const Rect& area, const LockLook& look);

/// Draws a scroll bar: a well, and its thumb in the well's inner columns.
///
/// @param canvas where it is drawn
/// @param well the well
/// @param thumb the thumb
/// @param hot the pointer is over the bar, or a press on it is held
void draw_scroll_bar(const Canvas& canvas, const Rect& well, const Rect& thumb, bool hot);

/// The side of the attention dot the OA button wears, ring included, in points.
inline constexpr int32_t oa_button_dot_side = 7;

/// The dot's gap to the OA button's top and right edges, in points.
inline constexpr int32_t oa_button_dot_inset = 2;

/// Returns where the OA button's attention dot lies: oa_button_dot_side
/// points square, oa_button_dot_inset points in from the button's top and
/// right edges.
///
/// @param side the button's side, in points
/// @return the dot's square, from the button's top left corner, in points
[[nodiscard]] constexpr Rect oa_button_dot(int32_t side) noexcept {
    return {
        side - oa_button_dot_inset - oa_button_dot_side,
        oa_button_dot_inset,
        oa_button_dot_side,
        oa_button_dot_side
    };
}

/// The badge's gap to the OA button's bottom and right edges, in points.
inline constexpr int32_t oa_button_badge_inset = 2;

/// The badge's height, in points: the small font's line.
inline constexpr int32_t oa_button_badge_height = 11;

/// The badge's gap between its outline and the number, on each side, in points.
inline constexpr int32_t oa_button_badge_pad = 2;

/// How many places the badge's spinner has, one full turn. One turn takes
/// refresh_revolution_ms, the same as a refresh mark, measured in time.
inline constexpr int32_t oa_button_spinner_places = 8;

/// The largest number the badge writes as itself. Above it, the badge says 999+.
inline constexpr std::size_t oa_button_count_limit = 999;

/// Returns the number the OA button's badge writes: the count, or "999+"
/// above oa_button_count_limit.
///
/// @param count the number
/// @return the text
[[nodiscard]] inline std::string oa_button_count_text(std::size_t count) {
    if (count > oa_button_count_limit)
        return "999+";
    return std::to_string(count);
}

/// Returns where the OA button's bottom-right badge lies. A count uses the
/// measured width of its text plus oa_button_badge_pad on each side, and at
/// least a square of oa_button_badge_height; a spinner passes 0 and is that
/// square. The badge is oa_button_badge_inset points in from the button's
/// bottom and right edges. A button too small to hold it, or a negative
/// width, returns an empty rectangle and nothing is drawn.
///
/// @param side the button's side, in points
/// @param text_columns the number's width, in points; 0 for a spinner or a count
///     not yet measured
/// @return the badge, from the button's top left corner, in points
[[nodiscard]] constexpr Rect oa_button_badge(int32_t side, int32_t text_columns) noexcept {
    if (side < oa_button_badge_inset * 2 + oa_button_badge_height || text_columns < 0)
        return {};
    int32_t width = text_columns + 2 * oa_button_badge_pad;
    if (width < oa_button_badge_height)
        width = oa_button_badge_height;
    const int32_t room = side - 2 * oa_button_badge_inset;
    if (width > room)
        width = room;
    return {
        side - oa_button_badge_inset - width,
        side - oa_button_badge_inset - oa_button_badge_height,
        width,
        oa_button_badge_height,
    };
}

/// Draws the OA button at the canvas's origin: a bevelled square, the icon
/// inset when the canvas has one, and otherwise the OA mark. With the dot,
/// the kit's attention dot is drawn over its top right corner
/// (oa_button_dot): a disc oa_button_dot_side points across in the warning
/// token (colour::lock) whose outer point is a ring in the outline token
/// (colour::control_border). A corner mark draws the badge in the bottom
/// right (oa_button_badge): the count, or the spinner in its place. Without
/// either, nothing else is drawn.
///
/// @param canvas where it is drawn; its icon is the button's
/// @param side the square's side, in points
/// @param state how it looks
/// @param dot it wears the attention dot, as the main menu's does while
///     updates are waiting
void draw_oa_button(const Canvas& canvas, int32_t side, OaButtonState state, bool dot = false);

/// Draws the OA button as draw_oa_button does, with the dot and the
/// bottom-right badge the mark names. The spinner's `turn` is which of
/// oa_button_spinner_places is lit, and the same turn draws the same pixels
/// on every machine. The badge does not move when the button is pressed.
///
/// @param canvas where it is drawn; its icon is the button's
/// @param side the square's side, in points
/// @param state how it looks
/// @param mark the dot and the bottom-right badge
void draw_oa_button(
    const Canvas& canvas, int32_t side, OaButtonState state, const OaButtonMark& mark
);

/// Draws the spinner the OA button's badge uses: eight dots on a circle,
/// three of them lit, the brightest the place `turn` names and the two
/// behind it dimmer. `turn` is which of oa_button_spinner_places is lit
/// (refresh_eighth of the time it has been turning). The same turn draws
/// the same pixels on every machine. The OA button's badge draws through
/// this, so its spinner does not change.
///
/// @param canvas where it is drawn
/// @param area the square the dots sit in
/// @param turn which place is brightest, wrapped over the eight
void draw_spinner(const Canvas& canvas, const Rect& area, uint32_t turn);

/// The shipped icons' clear corners follow a quarter circle of this radius,
/// in 256ths of the icon's side.
inline constexpr int32_t menu_icon_corner_256 = 56;

/// Draws an OA button as the icon itself, on the main menu's picture and in
/// the window. There is no panel and no bevel: the icon fills the square,
/// filtered from its own pixels, and its clear corners stay clear. Hover
/// and keyboard focus draw a ring in the accent that follows that corner;
/// a press draws the ring in the held accent and shifts the icon down and
/// right by one point. The dot and the badge are drawn as draw_oa_button
/// draws them, and the badge does not move when pressed. Nothing is drawn
/// when the side is 0 or less, or when the canvas has neither a painter
/// nor a picture.
///
/// @param canvas where it is drawn; its icon is the button's
/// @param side the square's side, in points
/// @param state how it looks
/// @param focused a ring is drawn at rest, as for keyboard focus
/// @param mark the dot and the bottom-right badge
void draw_menu_icon_button(
    const Canvas& canvas, int32_t side, OaButtonState state, bool focused, const OaButtonMark& mark
);

/// Draws the OA mark alone at the canvas's origin: the icon filling the
/// square, or without one the mark the OA button shows at rest. A side of
/// 0 or less draws nothing.
///
/// @param canvas where it is drawn; its icon is the mark's
/// @param side the square's side, in points
void draw_oa_mark(const Canvas& canvas, int32_t side);

/// Draws the header's mark: the icon filling the place, or without one the
/// small OA mark in its outlined square.
///
/// @param canvas where it is drawn; its icon is the mark's
/// @param place the place the icon fills
void draw_header_mark(const Canvas& canvas, const Rect& place);

/// Appends a button: one item and one control.
///
/// @param[in,out] list the display list
/// @param rect the button
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_button(
    DisplayList& list, const Rect& rect, const ButtonLook& look, ControlId id, std::string name
);

/// Appends a switch: one item and one control. steps is set, and checked is
/// the switch's state.
///
/// @param[in,out] list the display list
/// @param rect the switch
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_switch(
    DisplayList& list, const Rect& rect, const SwitchLook& look, ControlId id, std::string name
);

/// Appends a radio: one item, and with a control number one control, a
/// button whose checked is the radio's state and whose text is its caption.
/// With no_control it adds the item alone, for a mark inside a control the
/// caller adds itself, such as a card.
///
/// @param[in,out] list the display list
/// @param rect the radio: the mark's square, or the whole row
/// @param look its look
/// @param id the control's number, or no_control
/// @param name the control's automation name
void add_radio(
    DisplayList& list, const Rect& rect, const RadioLook& look, ControlId id, std::string name
);

/// Appends a level strip: one item and one control. steps is set.
///
/// @param[in,out] list the display list
/// @param rect the strip
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_levels(
    DisplayList& list, const Rect& rect, const LevelsLook& look, ControlId id, std::string name
);

/// Appends a slider: one item and one control. steps is set.
///
/// @param[in,out] list the display list
/// @param rect the slider's area
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_slider(
    DisplayList& list, const Rect& rect, const SliderLook& look, ControlId id, std::string name
);

/// Appends a drop-down field: one item and one control. steps is set.
///
/// @param[in,out] list the display list
/// @param rect the field
/// @param look its look
/// @param id the control's number
/// @param name the control's automation name
void add_choice(
    DisplayList& list, const Rect& rect, const ChoiceLook& look, ControlId id, std::string name
);

/// Appends a focus ring. It adds no control.
///
/// @param[in,out] list the display list
/// @param control the control the ring outlines
/// @param around true draws it outside the control
void add_focus_ring(DisplayList& list, const Rect& control, bool around);

/// Appends a lock. It adds no control.
///
/// @param[in,out] list the display list
/// @param area the lock's area
/// @param look its text
void add_lock(DisplayList& list, const Rect& area, const LockLook& look);

/// Appends a mark. It adds no control.
///
/// @param[in,out] list the display list
/// @param at the mark's top left
/// @param mark which mark
/// @param ink the colour of its set pixels
void add_mark(DisplayList& list, Point at, Mark mark, Colour ink);

/// Draws a display list's items in list order. An item with a clip draws
/// only inside it. A generic role is drawn by the canvas's primitive of its
/// name; a component by its draw function.
///
/// @param canvas where the list is drawn
/// @param list the display list
void paint(const Canvas& canvas, const DisplayList& list);

} // namespace oa::ui::kit
