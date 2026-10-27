// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The OA UI kit's layout: points, the Auto scale and the three size classes,
// frames, arrangements and scroll areas, and one display list. Nothing here
// draws. Hit testing reads the list's controls. Drawing reads its items.
#pragma once

#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdint.h>
#include <string>
#include <vector>

namespace oa::ui::kit {

/// A place in a layout, in points.
struct Point {
    int32_t x{}; ///< column
    int32_t y{}; ///< row
};

/// The points kept clear on each side of a rectangle.
struct Insets {
    int32_t left{};   ///< columns clear on the left
    int32_t top{};    ///< rows clear on the top
    int32_t right{};  ///< columns clear on the right
    int32_t bottom{}; ///< rows clear on the bottom
};

/// Tells whether a point lies in a rectangle.
///
/// The rectangle's right and bottom edges are outside it. An empty rectangle
/// holds nothing.
///
/// @param rect the rectangle, in points
/// @param point the point, in points
/// @return true when the point lies inside the rectangle
[[nodiscard]] constexpr bool contains(const Rect& rect, Point point) noexcept {
    return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.width &&
           point.y < rect.y + rect.height;
}

/// Returns the rectangle two rectangles share.
///
/// @param a a rectangle, in points
/// @param b another rectangle, in points
/// @return the shared rectangle; zero wide or high when they do not meet
[[nodiscard]] constexpr Rect intersect(const Rect& a, const Rect& b) noexcept {
    const int32_t left = std::max(a.x, b.x);
    const int32_t top = std::max(a.y, b.y);
    const int32_t right = std::min(a.x + a.width, b.x + b.width);
    const int32_t bottom = std::min(a.y + a.height, b.y + b.height);
    return {left, top, std::max(right - left, int32_t{0}), std::max(bottom - top, int32_t{0})};
}

/// Tells whether a rectangle lies wholly in another.
///
/// @param inner the rectangle
/// @param outer the rectangle it must lie in
/// @return true when no part of the inner rectangle lies outside the outer
[[nodiscard]] constexpr bool wholly_in(const Rect& inner, const Rect& outer) noexcept {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Returns a rectangle grown by the same number of points on every side.
///
/// A negative growth shrinks it. The result may be empty.
///
/// @param rect the rectangle, in points
/// @param by the points added on each side
/// @return the grown rectangle
[[nodiscard]] constexpr Rect grown(const Rect& rect, int32_t by) noexcept {
    return {rect.x - by, rect.y - by, rect.width + 2 * by, rect.height + 2 * by};
}

/// Returns a rectangle brought in by insets.
///
/// @param rect the rectangle, in points
/// @param insets the points taken off each side
/// @return the inset rectangle, empty when the insets use it up
[[nodiscard]] constexpr Rect inset(const Rect& rect, const Insets& insets) noexcept {
    return {
        rect.x + insets.left,
        rect.y + insets.top,
        rect.width - insets.left - insets.right,
        rect.height - insets.top - insets.bottom,
    };
}

/// Returns the rectangle's point nearest another point.
///
/// Inside the rectangle the point is unchanged. Outside, it is the nearest
/// point of the rectangle's own area, its right-most column and bottom-most
/// row included. An empty rectangle returns its origin.
///
/// @param rect the rectangle, in points
/// @param point the point, in points
/// @return the nearest point of the rectangle
[[nodiscard]] constexpr Point nearest_point(const Rect& rect, Point point) noexcept {
    if (rect.width <= 0 || rect.height <= 0)
        return {rect.x, rect.y};
    return {
        std::clamp(point.x, rect.x, rect.x + rect.width - 1),
        std::clamp(point.y, rect.y, rect.y + rect.height - 1),
    };
}

/// The window height, in canvas pixels, that the Auto scale counts as one step.
inline constexpr int32_t auto_scale_height = 720;

/// Returns the Auto scale of a window from its height, as a percentage.
///
/// The canvas height over 720, and never below the display's density, so a
/// point is never smaller than a window point. The height is the canvas, in
/// pixels, not the window's points. layer_viewport may then take one step
/// down from it, where that gives the room a larger size class.
///
/// While offer_fractional is false the scale is the nearest whole step, a
/// half away from zero, and at least 1: 1x below 1080 lines, 2x from 1080,
/// 3x from 1800. While it is true, 150% is among the steps, the nearest of
/// 1x, 150%, 2x, 3x, 4x and above, and a tie takes the larger: 1x below 900
/// lines, 150% from 900, 2x from 1260 and 3x from 1800. The density floor is
/// still a whole step, so a density of 2 or more is never 150%.
///
/// @param canvas_height the canvas height, in pixels
/// @param density the canvas pixels per window point
/// @param offer_fractional whether 150% is among the steps Auto may choose
/// @return the scale, as a percentage: 100, 150, 200, 300 and so on
[[nodiscard]] int32_t
auto_scale(int32_t canvas_height, float density, bool offer_fractional = false) noexcept;

/// How much room a layout has, from the points left after the scale: the
/// largest class whose settings dialog fits with class_margin points round it.
enum class SizeClass : uint8_t {
    compact, ///< below 768 by 534 points: Regular's dialog does not fit
    regular, ///< from 768 by 534 points: Regular's 720 by 486 dialog fits
    large,   ///< from 1008 by 648 points: Large's 960 by 600 dialog fits
};

/// The points kept free round a size class's settings dialog, on each side,
/// for a room to take that class.
inline constexpr int32_t class_margin = 24;

/// The points at which a layout becomes Regular, 768 by 534: Regular's
/// dialog with class_margin round it. Both sides must reach them.
inline constexpr Point regular_from{
    regular_metrics.dialog_width + 2 * class_margin,
    regular_metrics.dialog_height + 2 * class_margin,
};
/// The points at which a layout becomes Large, 1008 by 648: Large's dialog
/// with class_margin round it. Both sides must reach them.
inline constexpr Point large_from{
    large_metrics.dialog_width + 2 * class_margin,
    large_metrics.dialog_height + 2 * class_margin,
};

/// Returns a size class's metrics: compact_metrics, regular_metrics or
/// large_metrics. A screen takes its window's sizes from them.
///
/// @param size_class the size class
/// @return its metrics
[[nodiscard]] constexpr const Metrics& metrics_of(SizeClass size_class) noexcept {
    switch (size_class) {
    case SizeClass::regular:
        return regular_metrics;
    case SizeClass::large:
        return large_metrics;
    case SizeClass::compact:
        break;
    }
    return compact_metrics;
}

/// Returns the size class of a layout's room: the largest class whose
/// dialog fits it with class_margin points round it.
///
/// Large from large_from, Regular from regular_from, and Compact below that.
/// One side reaching a class does not: both the width and the height must.
///
/// @param width_points the room's width, in points
/// @param height_points the room's height, in points
/// @return the size class
[[nodiscard]] SizeClass size_class_of(int32_t width_points, int32_t height_points) noexcept;

/// Returns a chosen size class held to a room: the chosen class where its
/// dialog fits the room, the margin not counted, else the largest smaller
/// class whose dialog does, and Compact where none does.
///
/// A player who picks a class gets it wherever its dialog can show whole,
/// even where size_class_of would pick a smaller one.
///
/// @param chosen the class chosen
/// @param width_points the room's width, in points
/// @param height_points the room's height, in points
/// @return the class the room is laid out at
[[nodiscard]] SizeClass
size_class_within(SizeClass chosen, int32_t width_points, int32_t height_points) noexcept;

/// The canvas a screen is laid out on.
struct Viewport {
    int32_t width{};            ///< canvas pixels
    int32_t height{};           ///< canvas pixels
    float density{1.0F};        ///< canvas pixels per window point
    int32_t scale_percent{100}; ///< the scale, as a percentage; 150 is one and a half
    Insets safe{};              ///< canvas pixels kept clear on each side
};

/// The room a screen lays out into, in points, and the scale it is drawn at.
struct Frame {
    Rect area{};                              ///< the canvas less its safe insets, in points
    SizeClass size_class{SizeClass::compact}; ///< the class of area
    int32_t scale_percent{100};               ///< the viewport's scale, as a percentage
    /// Reduced motion, from a later Settings row. The kit does not read
    /// preferences: a screen copies the row into the frame. Off, the
    /// default, turns a carousel and pulses a call to action. On, the
    /// carousel slides flat and the call to action holds a steady ring.
    bool reduced_motion{false};
};

/// Returns the frame of a viewport.
///
/// The area is the canvas less its safe insets, divided by the scale and
/// rounded down, placed at the insets divided the same way. Its class comes
/// from that size. A scale of 0 or less counts as 1% while dividing and is
/// kept as given on the frame.
///
/// @param viewport the canvas, its density, its scale and its insets
/// @return the frame
[[nodiscard]] Frame frame_of(const Viewport& viewport) noexcept;

/// The one fractional scale an Interface size offers, in percent: one and a
/// half, drawn on the canvas's smooth form in the modern fonts.
inline constexpr int32_t fractional_scale_percent = 150;

/// Chosen percent that means Auto with 150% among its steps (auto_scale with
/// offer_fractional). Any other percent of 0 or less is Auto on whole steps
/// alone. A chosen size, 150% included, is never this value.
inline constexpr int32_t auto_with_fraction_percent = -1;

/// Returns the viewport OA's own screens are laid out on, at the scale an
/// Interface size chooses for a canvas.
///
/// The chosen step is the chosen percent's whole steps, or for Auto (0 or
/// less, other than auto_with_fraction_percent) the canvas's Auto scale on
/// whole steps. auto_with_fraction_percent is Auto with 150% among the steps.
/// A chosen step replaces Auto's rule, the density floor included, on a
/// touch canvas as on a desktop one.
///
/// The scale serves the class: where Auto's scale leaves the room's points
/// (size_class_of) below the wanted class and one step down gives the room
/// that class, Auto takes the step down. The steps are the whole ones, and
/// 150% between 1x and 2x while Auto offers it; the step down is never below
/// the display's density floor, nor below 1x. So 1920 by 1080 is Large at 1x
/// rather than Regular at 2x, and Large at 150% where 150% is offered;
/// 1440 by 900 with 150% offered is Large at 1x rather than Regular at 150%.
/// A chosen percent is never stepped down.
///
/// The scale is then the largest whole step, no larger than that one, at
/// which Compact's dialog (compact_metrics' dialog_width by dialog_height
/// points) fits the room, the canvas less its safe insets, and at least 1.
/// 150%, whether chosen or what Auto picked, is held the same way: it is
/// used where Compact's dialog fits the room at one and a half, and
/// otherwise 1. frame_of of the result gives the size class of the room's
/// points at that scale.
///
/// @param canvas the canvas: its pixels, density and safe insets; its
///     scale_percent is not read
/// @param chosen_percent the Interface size in percent, 100 a step: 100,
///     150, 200, 300 or 400; 0 for Auto on whole steps;
///     auto_with_fraction_percent for Auto with 150% among the steps. Any
///     other percent between two steps takes the step below it.
/// @param wanted the class Auto's step down serves: Large, unless the player
///     chose a window size, whose class it is then
/// @return the canvas with scale_percent set to that scale: 150, or a
///     whole step times 100
[[nodiscard]] Viewport layer_viewport(
    const Viewport& canvas, int32_t chosen_percent, SizeClass wanted = SizeClass::large
) noexcept;

/// Returns a rectangle of points as canvas pixels.
///
/// Each edge is that edge in points times the percentage, divided by 100 and
/// rounded to the nearest pixel, a half away from zero (lround of the point
/// times the scale), each edge on its own, so two rectangles that share an
/// edge in points share it in pixels, with no gap and no overlap, and every
/// edge falls on a whole pixel. A scale of 0 or less counts as 1%.
///
/// @param points the rectangle, in points
/// @param scale_percent the scale, as a percentage
/// @return the rectangle, in canvas pixels
[[nodiscard]] Rect to_canvas(const Rect& points, int32_t scale_percent) noexcept;

/// Returns a canvas pixel as a point.
///
/// Each coordinate is the pixel times 100, divided by the percentage and
/// rounded down. A scale of 0 or less counts as 1%.
///
/// @param canvas the pixel
/// @param scale_percent the scale, as a percentage
/// @return the point
[[nodiscard]] Point to_points(Point canvas, int32_t scale_percent) noexcept;

/// Places rectangles down an area, each as wide as the area.
///
/// The heights are as given. The gap lies between them, not after the last.
/// The column starts at the area's top and may run past its bottom.
///
/// @param area the area, in points
/// @param heights each rectangle's height, in points, top to bottom
/// @param gap the points between two rectangles
/// @return one rectangle per height
[[nodiscard]] std::vector<Rect>
column(const Rect& area, std::span<const int32_t> heights, int32_t gap);

/// Places rectangles across an area, each as tall as the area.
///
/// The widths and the gaps between them are one group, set to the left, the
/// centre or the right of the area. At the centre, spare points go to the
/// right of the group. The group may be wider than the area.
///
/// @param area the area, in points
/// @param widths each rectangle's width, in points, left to right
/// @param gap the points between two rectangles
/// @param align where the group sits in the area
/// @return one rectangle per width
[[nodiscard]] std::vector<Rect>
row(const Rect& area, std::span<const int32_t> widths, int32_t gap, Align align);

/// Places cells in rows across an area.
///
/// As many columns as fit at the least width, and at least one when there is
/// a cell to place. The cells of a full row widen to fill the row; spare
/// points go to the cells on the left, one each. A short last row keeps those
/// widths and starts at the left. A least width of 0 or less is one column.
///
/// @param area the area, in points
/// @param least_cell_width the narrowest a cell may be and still add a column, in points
/// @param cell_height each cell's height, in points
/// @param gap the points between cells
/// @param count the cells
/// @return the cells, row by row
[[nodiscard]] std::vector<Rect> grid(
    const Rect& area, int32_t least_cell_width, int32_t cell_height, int32_t gap, std::size_t count
);

/// Two rectangles a split makes of one area.
struct Split {
    Rect first{};  ///< the first part
    Rect second{}; ///< the rest, after the gap
};

/// Splits an area across, the first part a given width.
///
/// The second starts the gap after the first and ends at the area's right
/// edge. When that leaves it nothing, its width is 0.
///
/// @param area the area, in points
/// @param first_width the first part's width, in points
/// @param gap the points between the parts
/// @return the two parts, each as tall as the area
[[nodiscard]] Split split_across(const Rect& area, int32_t first_width, int32_t gap);

/// Splits an area down, the first part a given height.
///
/// The second starts the gap under the first and ends at the area's bottom
/// edge. When that leaves it nothing, its height is 0.
///
/// @param area the area, in points
/// @param first_height the first part's height, in points
/// @param gap the points between the parts
/// @return the two parts, each as wide as the area
[[nodiscard]] Split split_down(const Rect& area, int32_t first_height, int32_t gap);

/// Where a list scrolls: the view, the bar, and how far the content has moved.
///
/// The limit is the caller's. It is at least the content's height less the
/// view's, and more while the content scrolls on so a line is not cut by the
/// view's edge. It is not derived again here.
struct ScrollArea {
    Rect view{};              ///< what the content is seen through
    Rect well{};              ///< the scroll bar's well, as high as the view
    Rect hit{};               ///< where a press holds the scroll bar
    int32_t page_step{};      ///< how far a page step moves the offset, in points
    int32_t content_height{}; ///< the content's height, in points
    int32_t limit{};          ///< the greatest offset, in points
    int32_t offset{};         ///< how far the content is moved up, in points

    /// Returns the greatest offset that shows the end of a content in a view.
    ///
    /// @param content_height the content's height, in points
    /// @param view_height the view's height, in points
    /// @return the content's height less the view's, or 0 when the content fits
    [[nodiscard]] static int32_t limit_for(int32_t content_height, int32_t view_height) noexcept;

    /// Returns the offset nearest the open one that shows an item whole.
    ///
    /// The item's top, before scrolling, may come up to the view's first row.
    /// The row under the item, or the limit when the item is the last, may
    /// come down to the view's last row. An item taller than the view shows
    /// its top.
    ///
    /// @param content_top the item's top before scrolling, in points
    /// @param height the item's height, in points
    /// @param last true when the item is the last
    /// @return the offset, from 0 to the limit
    [[nodiscard]] int32_t
    offset_showing(int32_t content_top, int32_t height, bool last) const noexcept;

    /// Returns the scroll bar's thumb.
    ///
    /// The thumb sits inside the well's border. It is as tall as the view's
    /// share of the content, and never under the least height, and as far
    /// down its travel as the offset is down the limit, to the nearest row.
    ///
    /// @param least_height the least height of the thumb, in points
    /// @return the thumb, in points
    [[nodiscard]] Rect thumb(int32_t least_height) const noexcept;

    /// Returns the offset that puts the thumb's top on a row.
    ///
    /// @param thumb_top the thumb's top row, clamped to its travel
    /// @param least_height the least height of the thumb, in points
    /// @return the offset, from 0 to the limit
    [[nodiscard]] int32_t offset_at(int32_t thumb_top, int32_t least_height) const noexcept;

    /// Returns a content rectangle moved up by the offset.
    ///
    /// An empty rectangle is returned as it was given.
    ///
    /// @param content the rectangle before scrolling, in points
    /// @return the rectangle as placed, in points
    [[nodiscard]] Rect placed(const Rect& content) const noexcept;
};

/// A control's number. no_control is none.
using ControlId = int32_t;

/// No control.
inline constexpr ControlId no_control = -1;

/// What an item of a display list is. Later components append roles, each
/// to all_roles and canvas_draws as well.
enum class Role : uint8_t {
    fill,        ///< a flat rectangle
    blend,       ///< a rectangle blended by the item's opacity
    outline,     ///< a one-pixel outline
    bevel,       ///< a raised edge
    rule,        ///< a hairline
    text,        ///< a text
    picture,     ///< a picture: the item's PictureLook's, else the canvas's icon
    mark,        ///< a one-bit mark; its picture and colour are the item's MarkLook
    button,      ///< a button
    toggle,      ///< an Off/On switch
    levels,      ///< a strip of levels
    slider,      ///< a slider
    choice,      ///< a drop-down's field
    choice_menu, ///< a drop-down's open menu
    lock,        ///< a padlock and its text
    scroll_bar,  ///< a scroll bar
    focus_ring,  ///< the keyboard focus outline
    oa_button,   ///< the OA button
    oa_mark,     ///< the OA mark alone
    header,      ///< the window's header; its words are the item's HeaderLook
    footer_band, ///< the footer's rule and band; the item's FooterBandLook places them
    nav,         ///< a nav list; the item's NavLook is what is drawn
    heading,     ///< a section heading; the words are the item's HeadingLook
    row_frame,   ///< a row's rule, label and hints; the item's RowFrame
    locked_fade, ///< a locked row's fade; the rectangle is the item's
    list_row,    ///< a list row; the item's ListRowLook is what is drawn
    chip,        ///< a chip; the item's ChipLook
    field,       ///< a text field or a search field; the item's SearchLook
    tabs,        ///< a strip of tabs; the item's TabsLook, the rectangle the strip
    card,        ///< a card of a grid; the item's CardLook
    hover_card, ///< a hover card; the item's HoverCardLook, the rectangle the card without its arrow
    link,       ///< a link; the item's LinkLook
    progress,   ///< a progress bar; the item's ProgressLook says how far it has come
    // The parts of a screen drawn in the modern fonts, the Game files screen
    // and the folder chooser. Its own painter draws them from the item's
    // rectangle, lines, glyph, colour and state, with small rounded corners
    // and hairlines; the crisp canvas draws nothing for them. Such a screen
    // also draws its text as Role::text, its plain buttons as Role::button and
    // its OFF/ON switches as Role::toggle, from the item's lines rather than a
    // look.
    header_bar,     ///< a header bar: a band darker than the screen, a hairline under it
    badge,          ///< the OA badge: the Open Annihilation icon filling the rectangle, no text
    header_text,    ///< the words of a header bar
    version,        ///< a version's words
    title,          ///< a title's lines
    panel,          ///< a panel with an outline: a card, the rows' panel, an option's row
    icon,           ///< a glyph in the middle of the rectangle; on: in an outlined frame
    banner,         ///< a banner: a warm band, a bar of the item's colour at its left
    row,            ///< a row's flat ground in a panel
    divider,        ///< a line between two rows, in the item's colour
    button_main,    ///< the main button, filled with the accent
    button_danger,  ///< a button that removes something, outlined and lettered in red
    progress_track, ///< a progress bar's track
    progress_fill,  ///< a progress bar's fill, its fraction of the track
    progress_busy,  ///< a busy bar: the track striped, with no fraction
    backdrop,       ///< the dimming behind a sheet; no press reaches a part under it
    sheet,          ///< a sheet over a backdrop: a panel with a shadow
    pill,           ///< a pill; the item's PillLook
    language_card,  ///< a language card; the item's LanguageCardLook
    carousel,       ///< cards turned in a ring; the item's CarouselLook
    step_indicator, ///< a step's name and segments; the item's StepLook
    setup_chrome,   ///< a setup window's header and footer; the item's SetupChromeLook
    choice_card,    ///< a radio row; the item's ChoiceCardLook
    coach_mark,     ///< a numbered coach mark; the item's CoachMarkLook
    spotlight,      ///< a dimmed layer with a cut-out; the item's SpotlightLook
    picture_well,   ///< a bordered well and a caption; the item's PictureWellLook
    call_to_action, ///< an accent button that pulses; the item's CallToActionLook
    package_badge,  ///< a package badge; the item's BadgeLook
    grip,           ///< a drag handle; the item's GripLook
    keycap,         ///< a keycap; the item's KeycapLook
    chord_chip,     ///< a chord chip; the item's ChordChipLook
    keyboard,       ///< a keyboard picture; the item's KeyboardLook
    mouse_picture,  ///< a mouse picture; the item's MouseLook
    hold_ring,      ///< a hold ring; the item's HoldRingLook
};

/// Every role, in the order Role declares them. A role appended to Role is
/// appended here too, and canvas_draws says whether the kit's canvas draws
/// it: the static_assert under canvas_draws holds the list to the
/// enumeration, and the compiler holds canvas_draws to every role. The
/// gallery's test reads this list, so a role the gallery shows no part of
/// fails it.
inline constexpr std::array all_roles{
    Role::fill,          Role::blend,
    Role::outline,       Role::bevel,
    Role::rule,          Role::text,
    Role::picture,       Role::mark,
    Role::button,        Role::toggle,
    Role::levels,        Role::slider,
    Role::choice,        Role::choice_menu,
    Role::lock,          Role::scroll_bar,
    Role::focus_ring,    Role::oa_button,
    Role::oa_mark,       Role::header,
    Role::footer_band,   Role::nav,
    Role::heading,       Role::row_frame,
    Role::locked_fade,   Role::list_row,
    Role::chip,          Role::field,
    Role::tabs,          Role::card,
    Role::hover_card,    Role::link,
    Role::progress,      Role::header_bar,
    Role::badge,         Role::header_text,
    Role::version,       Role::title,
    Role::panel,         Role::icon,
    Role::banner,        Role::row,
    Role::divider,       Role::button_main,
    Role::button_danger, Role::progress_track,
    Role::progress_fill, Role::progress_busy,
    Role::backdrop,      Role::sheet,
    Role::pill,          Role::language_card,
    Role::carousel,      Role::step_indicator,
    Role::setup_chrome,  Role::choice_card,
    Role::coach_mark,    Role::spotlight,
    Role::picture_well,  Role::call_to_action,
    Role::package_badge, Role::grip,
    Role::keycap,        Role::chord_chip,
    Role::keyboard,      Role::mouse_picture,
    Role::hold_ring,
};

/// Tells whether the kit's canvas draws a role (paint, in either form): it
/// draws every role but the parts of a screen drawn in the modern fonts,
/// header_bar to sheet, which that screen's own painter draws. A role added
/// to Role must be named here, or the switch does not compile with the
/// warnings the engine builds with.
///
/// @param role the role
/// @return true when paint draws it
[[nodiscard]] constexpr bool canvas_draws(Role role) noexcept {
    switch (role) {
    case Role::fill:
    case Role::blend:
    case Role::outline:
    case Role::bevel:
    case Role::rule:
    case Role::text:
    case Role::picture:
    case Role::mark:
    case Role::button:
    case Role::toggle:
    case Role::levels:
    case Role::slider:
    case Role::choice:
    case Role::choice_menu:
    case Role::lock:
    case Role::scroll_bar:
    case Role::focus_ring:
    case Role::oa_button:
    case Role::oa_mark:
    case Role::header:
    case Role::footer_band:
    case Role::nav:
    case Role::heading:
    case Role::row_frame:
    case Role::locked_fade:
    case Role::list_row:
    case Role::chip:
    case Role::field:
    case Role::tabs:
    case Role::card:
    case Role::hover_card:
    case Role::link:
    case Role::progress:
    case Role::pill:
    case Role::language_card:
    case Role::carousel:
    case Role::step_indicator:
    case Role::setup_chrome:
    case Role::choice_card:
    case Role::coach_mark:
    case Role::spotlight:
    case Role::picture_well:
    case Role::call_to_action:
    case Role::package_badge:
    case Role::grip:
    case Role::keycap:
    case Role::chord_chip:
    case Role::keyboard:
    case Role::mouse_picture:
    case Role::hold_ring:
        return true;
    case Role::header_bar:
    case Role::badge:
    case Role::header_text:
    case Role::version:
    case Role::title:
    case Role::panel:
    case Role::icon:
    case Role::banner:
    case Role::row:
    case Role::divider:
    case Role::button_main:
    case Role::button_danger:
    case Role::progress_track:
    case Role::progress_fill:
    case Role::progress_busy:
    case Role::backdrop:
    case Role::sheet:
        return false;
    }
    return false;
}

/// Tells whether all_roles holds its roles in Role's order: each entry's
/// value is its place.
///
/// @return true when it does
[[nodiscard]] constexpr bool all_roles_in_order() noexcept {
    for (std::size_t place = 0; place < all_roles.size(); ++place)
        if (static_cast<std::size_t>(all_roles[place]) != place)
            return false;
    return true;
}

// The last role is named here: a role appended to Role is appended to
// all_roles and named here in hold_ring's place.
static_assert(
    all_roles_in_order() && all_roles.size() == static_cast<std::size_t>(Role::hold_ring) + 1,
    "all_roles names every role of Role once, in its order"
);

/// A mark a screen drawn in the modern fonts places in a part: an icon, a
/// banner's or a button's. Its own painter draws it, as strokes on a square;
/// the crisp canvas draws none.
enum class Glyph : uint8_t {
    none,      ///< no mark
    folder,    ///< a folder
    device,    ///< the device
    disk,      ///< a disk
    clock,     ///< a clock
    warning,   ///< a warning triangle
    check,     ///< a check mark
    cross,     ///< a cross
    dash,      ///< a dash
    info,      ///< an information mark
    stop,      ///< a stop square
    play,      ///< a play triangle
    trash,     ///< a bin
    refresh,   ///< a circular arrow
    plus,      ///< a plus
    file,      ///< a file
    oa,        ///< the Open Annihilation icon, or the OA mark without it
    magnifier, ///< a magnifying glass, before a search field's text
};

/// The part a text plays on a screen drawn in the modern fonts. The painter
/// draws a text at its pixel size and weight; the style says what the text is.
enum class TextStyle : uint8_t {
    header,     ///< the header bar
    title,      ///< a step's title
    subtitle,   ///< under the title
    lead,       ///< a card's or banner's lead line
    body,       ///< ordinary text
    small,      ///< footers and hints
    row_title,  ///< a row's name
    row_detail, ///< a row's detail
    button,     ///< a button's label
    mark_text,  ///< text beside a mark
};

/// How a control looks, for the item that draws it.
struct State {
    bool hovered{};  ///< the pointer is over it
    bool pressed{};  ///< it is held
    bool focused{};  ///< the keyboard focus is on it
    bool selected{}; ///< it is the chosen one of its group
    bool on{};       ///< a switch is on
    bool locked{};   ///< it shows and takes no change
    bool disabled{}; ///< it takes nothing
    friend constexpr bool operator==(State, State) = default;
};

/// One thing a screen draws, in the order it is drawn.
///
/// A screen drawn in the modern fonts (the Game files screen and the folder
/// chooser) lays its parts out in canvas pixels, where the kit's other
/// screens use points: its rectangles and clips are canvas pixels, and its
/// texts are its lines at a pixel size, drawn by its own painter.
struct Item {
    Role role{};                   ///< what is drawn
    Rect rect{};                   ///< where, in points
    Rect clip{};                   ///< what drawing may touch; empty means none
    std::string text{};            ///< a text item's words
    FontRole font{};               ///< which font a text is drawn in
    int32_t tracking{};            ///< extra columns after each glyph but the last
    Align align{};                 ///< where a text sits in its rectangle
    Colour colour{};               ///< the colour
    uint32_t opacity{};            ///< a blend's share of a pixel, in 256ths; 256 is opaque
    ControlId control{no_control}; ///< the control this draws, or none
    State state{};                 ///< how that control looks
    /// How a component is drawn. Empty for a generic role. A component's look
    /// owns its texts, so the list outlives the words it was built from.
    Look look{};
    /// A modern-font text, already broken into lines, top to bottom; empty
    /// for none. A button's label is its one line, a switch's are OFF and ON.
    std::vector<std::string> lines{};
    int32_t pixel_size{};             ///< the lines' size, in canvas pixels
    bool bold{};                      ///< the lines are bold
    TextStyle style{TextStyle::body}; ///< the part the lines play
    Glyph glyph{Glyph::none};         ///< the mark of an icon, a banner or a button
    int32_t glyph_size{};             ///< a button's mark's side, in canvas pixels; 0: pixel_size
    float fraction{};                 ///< a progress fill's share of its track, 0 to 1
};

/// What a control is. A screen maps these when it lists controls to automation.
enum class ControlKind : uint8_t {
    button,     ///< a button
    toggle,     ///< a switch
    choice,     ///< a drop-down
    slider,     ///< a slider
    levels,     ///< a strip of levels
    buttons,    ///< a row of buttons
    list,       ///< a list
    list_item,  ///< a row of a list
    text_field, ///< a field the player types in
    scroll_bar, ///< a scroll bar
    tab,        ///< a tab
    link,       ///< a link
    area,       ///< a place that takes a press and is not one of the others
};

/// A control a press can reach. The rectangle is where it lies, scrolled out
/// of view or not. What a press reaches is the rectangle clipped, when a clip
/// is set. Its name is how automation finds it.
struct Control {
    ControlId id{no_control}; ///< its number
    Rect rect{};              ///< where it lies, in points
    Rect clip{};              ///< the part a press can reach; empty means the whole rectangle
    bool enabled{true};       ///< false while a press does not reach it
    /// Its automation name: words of a-z, 0-9 and hyphens, joined by dots, at
    /// most 100 bytes, and unique on the screen. The automation endpoint adds
    /// its own prefix; the name here has none.
    std::string name{};
    ControlKind kind{};   ///< what it is
    bool focusable{true}; ///< the arrows and Tab can stop on it
    /// Left and Right act on it, as a switch or a slider takes them, instead
    /// of moving the focus.
    bool steps{};
    int32_t group{-1};  ///< its scroll area's number; -1 when it is in none
    bool checked{};     ///< a switch that is on, or a chosen row, for automation
    std::string text{}; ///< its caption or its value, for automation
    /// The words automation names its parts by, in order, each a word of
    /// a-z, 0-9 and hyphens: a strip's levels, a row of buttons' buttons, a
    /// drop-down's items. A part without one is named by its place from 1;
    /// a switch's halves are always off and on (automation_parts).
    std::vector<std::string> parts{};
};

/// What a screen drew, and where its controls are.
///
/// Items are in drawing order. Controls are in the order a point is tested,
/// and the two lists are not one list: drawing never reads a control, and a
/// hit never reads an item. Tab follows tab_order.
struct DisplayList {
    std::vector<Item> items{};       ///< what is drawn, first to last
    std::vector<Control> controls{}; ///< what can be pressed, first tested first
    /// The declared order Tab follows. Only these controls take the focus.
    std::vector<ControlId> tab_order{};
};

/// Returns the first enabled control a point lies in.
///
/// Controls are tried in list order. A control with a clip is reached only
/// in the part its rectangle shares with the clip; with no clip, in its
/// rectangle. A disabled control is skipped.
///
/// @param list the display list
/// @param point the point, in the same points as the controls
/// @return that control's number, or no_control when none holds the point
[[nodiscard]] ControlId hit(const DisplayList& list, Point point) noexcept;

/// What a disabled control does to a press inside it.
enum class DisabledControl : uint8_t {
    passes, ///< the press passes on to the controls under it, as if it were not there
    blocks, ///< the press stops there and reaches no control
};

/// Returns the control a point reaches, by a rule for disabled controls.
///
/// Controls are tried in list order, each where hit tries it. The first
/// enabled one the point lies in is the answer. A disabled one it lies in is
/// skipped with DisabledControl::passes, as hit does; with
/// DisabledControl::blocks it ends the search, and the point reaches no
/// control. The folder chooser blocks; the Game files screen passes.
///
/// @param list the display list
/// @param point the point, in the same points as the controls
/// @param disabled what a disabled control the point lies in does
/// @return the control's number, or no_control when the point reaches none
[[nodiscard]] ControlId
hit(const DisplayList& list, Point point, DisabledControl disabled) noexcept;

/// Returns a list's control of a number.
///
/// @param list the display list
/// @param id the control's number
/// @return the control, or null when the list has none of that number; the first when it has several
[[nodiscard]] const Control* control_of(const DisplayList& list, ControlId id) noexcept;

/// A temporary list would be freed before its control is read: name the list.
const Control* control_of(DisplayList&& list, ControlId id) = delete;

// Parts of a screen drawn in the modern fonts, laid out in canvas pixels at a
// scale that may be a fraction of a pixel a point (the Game files screen and
// the folder chooser).

/// How a screen drawn in the modern fonts turns points into canvas pixels and
/// measures its text.
struct Typesetter {
    TextMeasureHooks measure{}; ///< the bundled fonts' measure
    float px_per_point{1.0F};   ///< canvas pixels per point
};

/// Returns a length in points as whole canvas pixels: the length times the
/// pixels a point, in single precision, rounded to the nearest pixel, a half
/// away from zero.
///
/// @param type the scale
/// @param points the length, in points
/// @return the length, in canvas pixels
[[nodiscard]] int32_t canvas_pixels(const Typesetter& type, float points) noexcept;

/// Returns a text's size in points as its pixel size: canvas_pixels, and at
/// least 1.
///
/// @param type the scale
/// @param points the size, in points
/// @return the pixel size
[[nodiscard]] int32_t canvas_font(const Typesetter& type, float points) noexcept;

/// A modern-font text's look.
struct Lettering {
    float points{};                     ///< its size, in points
    bool bold{};                        ///< bold
    Colour colour{screen_colour::text}; ///< its colour
    TextStyle style{TextStyle::body};   ///< the part it plays
};

/// How a text part's rectangle sits in the room it is given.
enum class TextFit : uint8_t {
    column,      ///< as wide as the room, at its left
    tight_left,  ///< as wide as its widest line, at the room's left
    tight_right, ///< as wide as its widest line, at the room's right
};

/// Adds a text broken into lines no wider than a room, as one part.
///
/// The text breaks at its spaces, every new line starts a paragraph, and a
/// word wider than the room breaks between its characters, or where
/// break_word says when one is given (path_break breaks a folder's path after
/// a separator). Without break_word, a text with Chinese, Japanese or Korean
/// characters breaks as wrap's rules break it (WrapRules::wide_scripts):
/// between Chinese or Japanese characters too, keeping a Korean word whole;
/// a path, with break_word, breaks as other text does. An empty line is
/// dropped. When there are more lines than most_lines, the last
/// one kept takes the rest of the text and ends with an ellipsis, shortened
/// to the room (fit). The part is a line high (measured_line) for each line,
/// and as wide as `sits` says.
///
/// @param[in,out] parts the parts it is added to
/// @param type the scale and the measure
/// @param role the part's role: text, title, header_text or version
/// @param at the room's left and the text's top, in canvas pixels
/// @param room the room's width, in canvas pixels
/// @param words the text, in UTF-8
/// @param look its size, weight, colour and style
/// @param sits how the part sits in the room
/// @param most_lines the most lines; 0 for no limit
/// @param break_word where a word wider than the room breaks; null breaks it between characters
/// @return the part's height, in canvas pixels; 0 when it was not added (no text, no room)
int32_t text_part(
    std::vector<Item>& parts,
    const Typesetter& type,
    Role role,
    Point at,
    int32_t room,
    std::string_view words,
    const Lettering& look,
    TextFit sits = TextFit::column,
    std::size_t most_lines = 0,
    WordBreak break_word = nullptr
);

/// Adds one line of text, shortened with an ellipsis to a room (fit), as one
/// part as wide as the line and a line high, centred on a row.
///
/// @param[in,out] parts the parts it is added to
/// @param type the scale and the measure
/// @param x the line's left, or its right with TextFit::tight_right, in canvas pixels
/// @param middle the row the line is centred on, in canvas pixels
/// @param room the most width it may take, in canvas pixels
/// @param words the line, in UTF-8
/// @param look its size, weight, colour and style
/// @param sits tight_right for a line that ends at x; any other starts at x
/// @param role the part's role
/// @return the width it takes, in canvas pixels; 0 when it was not added
int32_t line_part(
    std::vector<Item>& parts,
    const Typesetter& type,
    int32_t x,
    int32_t middle,
    int32_t room,
    std::string_view words,
    const Lettering& look,
    TextFit sits = TextFit::tight_left,
    Role role = Role::text
);

/// The share of a button's label size between its mark and the label.
inline constexpr float button_mark_gap_em = 0.5F;

/// A modern-font button: its label, its look, its mark and its control.
struct ButtonSpec {
    std::string label{};           ///< the words, already looked up
    Role role{Role::button};       ///< button, button_main or button_danger
    Glyph glyph{Glyph::none};      ///< the mark before the label
    float glyph_points{};          ///< the mark's side, in points; 0: the label's pixel size
    ControlId control{no_control}; ///< the control it is
    bool enabled{true};            ///< a press reaches it
};

/// Returns the width a button takes: its label in bold, its mark and the gap
/// after it (button_mark_gap_em of the label's size), and the padding on each
/// side, and at least a least width.
///
/// @param type the scale and the measure
/// @param button the button
/// @param text_points the label's size, in points
/// @param pad_points the padding on each side, in points
/// @param least_points the least width, in points
/// @return the width, in canvas pixels
[[nodiscard]] int32_t button_span(
    const Typesetter& type,
    const ButtonSpec& button,
    float text_points,
    float pad_points,
    float least_points
);

/// Adds a button as one part. Its label, bold, is shortened (fit) to what its
/// mark and 4 points on each side leave of the box. A main button's label is
/// the ink on the accent, a danger button's red, a plain one's the button
/// text; the main button's state is on, a button that takes no press is
/// disabled.
///
/// @param[in,out] parts the parts it is added to
/// @param type the scale and the measure
/// @param box the button, in canvas pixels
/// @param button the button
/// @param text_points the label's size, in points
void button_part(
    std::vector<Item>& parts,
    const Typesetter& type,
    const Rect& box,
    const ButtonSpec& button,
    float text_points
);

/// The sizes of a row of buttons, in points.
struct ButtonRowSizes {
    float text{};       ///< a label's size
    float pad{};        ///< the padding on each side of a label
    float least_main{}; ///< the least width of the main button
    float gap{};        ///< between two buttons; twice it between the two groups
    float height{};     ///< every button's height
};

/// Adds a row of buttons, each as wide as button_span gives and at most the
/// room: the left group from the room's left and the right group ending at
/// its right when both fit on one line with twice the gap between them; else
/// every button from the left, left group first, a button that would pass the
/// room's right starting a new line.
///
/// @param[in,out] parts the parts they are added to
/// @param type the scale and the measure
/// @param at the room's left and the row's top, in canvas pixels
/// @param room the room's width, in canvas pixels
/// @param left the buttons at the left, left to right
/// @param right the buttons at the right, left to right
/// @param sizes the buttons' sizes
/// @return the height the row takes, in canvas pixels; 0 with no button
int32_t button_row(
    std::vector<Item>& parts,
    const Typesetter& type,
    Point at,
    int32_t room,
    std::span<const ButtonSpec> left,
    std::span<const ButtonSpec> right,
    const ButtonRowSizes& sizes
);

/// Adds a glyph in a box as an icon part, its state on when it is framed.
/// Nothing is added for no glyph and no frame.
///
/// @param[in,out] parts the parts it is added to
/// @param box the box, in canvas pixels
/// @param glyph the mark
/// @param colour the mark's colour
/// @param framed true to draw the box's outline too
void icon_part(std::vector<Item>& parts, const Rect& box, Glyph glyph, Colour colour, bool framed);

/// Adds a part with no text and no control: a header bar, a panel, a row, a
/// banner, a divider, a sheet, a backdrop or a bar.
///
/// @param[in,out] parts the parts it is added to
/// @param role what it draws
/// @param box where, in canvas pixels
/// @param colour its colour: a banner's bar, a divider's line
void plain_part(std::vector<Item>& parts, Role role, const Rect& box, Colour colour);

/// Moves parts down and appends them to a list, in their order, leaving the
/// list they came from empty.
///
/// @param[in,out] to the list they are appended to
/// @param[in,out] from the parts; empty afterwards
/// @param down how far they move down, in canvas pixels
void move_parts(std::vector<Item>& to, std::vector<Item>& from, int32_t down);

/// Moves parts down, gives each a clip, and appends them to a list, in their
/// order, leaving the list they came from empty.
///
/// @param[in,out] to the list they are appended to
/// @param[in,out] from the parts; empty afterwards
/// @param down how far they move down, in canvas pixels
/// @param clip each part's clip; empty for none
void move_parts(std::vector<Item>& to, std::vector<Item>& from, int32_t down, const Rect& clip);

/// Where the three blocks of a column lie in an area: a fixed top, a body,
/// and a fixed bottom a gap under the body.
struct ScrollColumn {
    int32_t top{};      ///< the top block's first row, scrolled
    int32_t body{};     ///< the body's first row, scrolled
    int32_t bottom{};   ///< the bottom block's first row, scrolled
    Rect top_clip{};    ///< the top block's clip; empty for none
    Rect body_clip{};   ///< the body's clip; empty for none
    Rect bottom_clip{}; ///< the bottom block's clip; empty for none
    /// What scrolls: the view, empty when nothing does; the content's height;
    /// the limit and the offset, clamped to it.
    ScrollArea scroll{};
};

/// Places a column's blocks in an area.
///
/// The gap lies between the body and the bottom when both have height. When
/// the blocks fit the area's height they lie one after another from its top,
/// and nothing scrolls. Else, when the body has height and the area less the
/// top, the gap and the bottom leaves it least_body or more, the body scrolls
/// in that view between the fixed top and bottom, clipped to it. Else the
/// whole column scrolls in the area, every block clipped to it. The limit is
/// what does not fit, and the offset the wanted one, at most the limit and at
/// least 0.
///
/// @param area the area, in canvas pixels
/// @param top_height the top block's height
/// @param body_height the body's height
/// @param bottom_height the bottom block's height
/// @param gap the gap between the body and the bottom
/// @param least_body the least view the body scrolls in alone
/// @param offset how far the column is wanted scrolled
/// @return where the blocks lie
[[nodiscard]] ScrollColumn scroll_column(
    const Rect& area,
    int32_t top_height,
    int32_t body_height,
    int32_t bottom_height,
    int32_t gap,
    int32_t least_body,
    int32_t offset
) noexcept;

/// Returns the index of the first part a press can reach: the one after the
/// last backdrop, or 0 with none.
///
/// @param list the display list
/// @return the index
[[nodiscard]] std::size_t first_live_part(const DisplayList& list) noexcept;

/// Lists a display list's controls from its parts, for a screen whose parts
/// carry their controls. Every part from first_live_part that draws a control
/// becomes one, the last drawn first, so a point is tested against the top
/// part first: its rectangle and clip are the part's, it is enabled unless
/// the part is disabled, a toggle for a switch and a button otherwise. Each
/// enabled control joins the Tab order once, in drawing order. A part under a
/// backdrop takes no press and no focus.
///
/// @param[in,out] list the display list; its controls and Tab order are replaced
void list_part_controls(DisplayList& list);

} // namespace oa::ui::kit
