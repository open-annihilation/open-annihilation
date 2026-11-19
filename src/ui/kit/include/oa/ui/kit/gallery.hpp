// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The UI kit gallery: one screen that shows every component of the kit, each
// once working on the gallery's own model and once in each fixed state, at
// the scale and size class of its frame. Its model, its layout, its drawing
// and what an event does to it. It is made of the kit's components alone and
// draws nothing of its own; its texts are fixed samples, never a setting, a
// real mod or the build's version.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace oa::ui::kit {

/// The version the window chrome's sample shows. Never the build's own, so
/// that the gallery's pictures stay the same from one release to the next.
inline constexpr std::string_view gallery_version = "v1.2.3";

/// The languages the working drop-down offers, each named in itself: the
/// game's fonts draw every one.
inline constexpr std::array<std::string_view, 4> gallery_languages{
    "English",
    "Deutsch",
    "Español",
    "Français",
};

/// The stops of the working slider: 0 to 10, a tenth each.
inline constexpr int32_t gallery_slider_stops = 11;

/// The gallery's model: where it is scrolled, the pointer and the focus, and
/// the values of its working samples. Nothing in it is a setting, and
/// nothing the gallery does reads or changes one.
struct GalleryModel {
    int32_t scroll{};          ///< how far the sections are scrolled up, in points
    int32_t list_scroll{};     ///< how far the scroll list's rows are scrolled up, in points
    Interaction interaction{}; ///< the control under the pointer, the one held and the focus
    WheelCarry wheel{};        ///< what turns of the wheel have not yet scrolled of the sections
    WheelCarry list_wheel{};   ///< what they have not yet scrolled of the scroll list
    std::size_t tab{};         ///< the working tabs' open tab, from 0
    int32_t presses{};         ///< how often the working button, link and OA button were pressed
    bool toggle_on{};          ///< the working switch is On
    std::size_t level{1};      ///< the working strip's chosen level, from 0
    std::size_t language{};    ///< the working drop-down's choice, in gallery_languages
    bool menu_open{};          ///< the working drop-down's menu is open
    int32_t menu_marked{};     ///< the item its menu marks, from 0
    bool menu_held{};          ///< a press that began on the open menu is held
    int32_t slider_stop{8};    ///< the working slider's stop, from 0 to gallery_slider_stops - 1
    TextField search{};        ///< the working search field's text and caret
    std::array<bool, 3> chips{true, false, false}; ///< which working filter chips are on
    std::size_t nav{};                             ///< the working nav list's chosen entry
    std::size_t row{};                             ///< the chosen working list row
    std::size_t list_row{};                        ///< the chosen row of the scroll list
    std::size_t card{};                            ///< the chosen working card
    int32_t carousel_index{2};                     ///< the working carousel's centre card, from 0
    std::size_t choice_index{};                    ///< the working choice cards' chosen row, from 0
};

/// A component the gallery draws from an origin of its own: the window
/// chrome's sample, the notice and the question, whose header and footer the
/// kit draws from a canvas's origin. It takes no input.
struct GalleryPanel {
    Point origin{}; ///< where its origin lies in the gallery, in points
    Rect clip{};  ///< the part of the gallery it draws in: its own place within the sections' view
    Rect place{}; ///< its place in the gallery, in points, scrolled with the sections
    DisplayList list{}; ///< its parts, in points from its origin; no controls
};

/// The gallery laid out for a frame, in points from its top left corner.
struct GalleryLayout {
    /// The gallery's own parts and its controls: its window, the sections'
    /// headings and samples, and the working samples' controls in the
    /// declared Tab order.
    DisplayList list{};
    /// Drawn over list's parts, in order, each clipped to its place.
    std::vector<GalleryPanel> panels{};
    /// Where the sections scroll: the body between the header and the
    /// footer, its scroll bar, the content's height and the offset.
    ScrollArea sections{};
    /// Where the scroll list's rows scroll, in the gallery's points as the
    /// sections are scrolled.
    ScrollArea rows{};
    /// The working drop-down's open menu, as drawn; empty while it is closed.
    Rect menu{};
    /// What the open menu shows.
    ChoiceMenuLook menu_look{};
    /// The columns the sections flow in: 1 at Compact, 2 at Regular and 3 at
    /// Large.
    std::size_t columns{};
};

/// Lays the gallery out for a frame.
///
/// The window covers the frame's whole area: the face, the header (the OA
/// mark, "Open Annihilation", "UI kit gallery" and the size class and scale
/// in the lock colour), the footer band with CLOSE at its right, and the
/// edge. Between the header and the footer the sections flow, each under
/// the column that is shortest so far, in one column at Compact, two at
/// Regular and three at Large: First run (the carousel turned, language
/// cards, setup chrome, choice cards, a coach mark and its spotlight, a
/// picture well, the call to action and a step indicator), then Window
/// chrome, Tabs, Buttons, Toggle, Level
/// strip, Drop-down, Slider, Search field, Chips, Pills, Progress, Nav list,
/// List rows, Scroll list, Card grid, Hover card, Section, label and hint,
/// Lock, Notice, Question and OA mark, each under its heading. Where they
/// are taller than the view between the header and the footer they scroll
/// in it, with a scroll bar in the right margin, every part and control
/// clipped to the view. Each interactive component shows once working on
/// the model, named gallery.<section>.<n> from 1, and once in each fixed
/// state (idle, hovered, pressed, focused, disabled, and locked where it
/// has a lock), which takes no input. The carousel's cards are
/// gallery.carousel.card.<tag> and its arrows gallery.carousel.previous and
/// gallery.carousel.next; the setup buttons are gallery.setup.skip,
/// gallery.setup.back and gallery.setup.next. CLOSE is gallery.close and
/// last in the Tab order.
///
/// @param model the gallery's model; its offsets are held to what scrolls
/// @param frame the frame: its area's size, its size class and its scale
/// @param fonts the game's fonts the texts are measured in
/// @return the layout
[[nodiscard]] GalleryLayout
gallery_layout(const GalleryModel& model, const Frame& frame, const Fonts& fonts);

/// Draws a laid-out gallery from the canvas's origin: its list's parts with
/// the kit's paint, then each panel's parts with paint on the canvas
/// clipped to the panel and placed at its origin.
///
/// @param canvas where it is drawn, in either form
/// @param layout the gallery as gallery_layout laid it out
void draw_gallery(const Canvas& canvas, const GalleryLayout& layout);

/// What happened to the gallery.
enum class GalleryEventKind : uint8_t {
    pointer_move, ///< the pointer moved
    pointer_down, ///< the pointer's button, or a finger, went down
    pointer_up,   ///< it came up
    key,          ///< a key was pressed
    text,         ///< text was typed
    wheel,        ///< the wheel turned
};

/// One event the gallery takes.
struct GalleryEvent {
    GalleryEventKind kind{}; ///< what happened
    Point at{};              ///< the pointer, in the gallery's points
    int32_t reach{};         ///< a finger's reach, in points; 0 for a mouse
    Key key{};               ///< the key pressed
    std::string_view text{}; ///< the text typed, in UTF-8
    float notches{};         ///< the wheel's turn; away from the player is positive
};

/// What an event asks of the gallery's host.
enum class GalleryResult : uint8_t {
    none,   ///< nothing it draws changed
    redraw, ///< what it draws changed: draw it again
    close,  ///< CLOSE or Escape: take the gallery off
};

/// Takes one event on the gallery and changes its model, and nothing else.
///
/// The pointer and a finger hover, hold and press the working samples
/// (kit::pointer_move and the like); a release over the control it held
/// presses it. A press opens a tab, flips the switch wherever it lands on
/// it, chooses the level under it, opens the drop-down's menu, moves the
/// slider (a held press drags it), gives the search field the focus,
/// turns a filter chip on or off, chooses a nav entry, a row or a card,
/// counts a press of the button, the link or the OA button, and CLOSE
/// closes. While the menu is open a press on one of its items chooses it
/// on release, and any other press only closes it. The keys follow
/// kit::key: Tab in the declared order, the arrows by where controls sit
/// with the sections laid out whole (a control scrolled out of the view is
/// judged where it lies in them, and the footer's CLOSE under them all),
/// Left and Right step the switch, the strip, the slider and the
/// drop-down, Space and Enter press, and the focus moved scrolls the
/// sections, and the scroll list, until the focused control shows. The
/// open menu takes Up, Down, Home, End, Page Up, Page Down, Enter, Space
/// and Escape. Text typed and the editing keys go to the focused search
/// field. The wheel scrolls the scroll list under the pointer, else the
/// sections; Page Up, Page Down, Home and End scroll the sections. Escape
/// closes the open menu, else the gallery.
///
/// @param[in,out] model the gallery's model
/// @param frame the frame it is laid out for
/// @param fonts the game's fonts it is laid out in
/// @param event the event
/// @return what the host does
GalleryResult
gallery_act(GalleryModel& model, const Frame& frame, const Fonts& fonts, const GalleryEvent& event);

/// Returns the focused text field: the working search field while it has
/// the focus, over which a host keeps the system's text input started.
///
/// @param model the gallery's model
/// @param layout the gallery laid out from it
/// @return the field's rectangle, in the gallery's points; none while it lacks the focus
[[nodiscard]] std::optional<Rect>
gallery_text_field(const GalleryModel& model, const GalleryLayout& layout);

} // namespace oa::ui::kit
