// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The OA UI kit's input: a pointer, a finger's reach, the keys, Tab in a
// declared order, the arrows by where controls sit, and the wheel. Every
// control carries a name, so a screen can be driven without a pointer.
#pragma once

#include "oa/ui/kit/layout.hpp"

#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::kit {

/// The keys a kit screen answers to. A host gives the platform's keys these
/// meanings. The names and their order up to no are the settings dialog's
/// keys, so a host can pass one through as the other; the editing keys and
/// the information key after it are the kit's own. Enter also goes to a
/// focused tab, and Home and End to a focused text field, as key says.
enum class Key : uint8_t {
    enter,     ///< the focused button, link or list row; otherwise the screen accepts
    escape,    ///< the screen cancels
    up,        ///< the focus to the control above
    down,      ///< the focus to the control below
    left,      ///< a stepping control, or the focus to the control on the left
    right,     ///< a stepping control, or the focus to the control on the right
    space,     ///< the focused control
    tab,       ///< the focus to the next control in the declared order
    back_tab,  ///< the focus to the previous control in the declared order
    page_up,   ///< scrolls
    page_down, ///< scrolls
    home,      ///< scrolls to the top
    end,       ///< scrolls to the end
    yes,       ///< nothing on a kit screen
    no,        ///< nothing on a kit screen
    /// A focused text field deletes the character before its caret.
    backspace,
    /// A focused text field deletes the character after its caret.
    delete_forward,
    /// The information key: a screen that has more to tell about what the
    /// pointer or the focus is on shows it, such as a card. The OA layer
    /// gives it F1 outside a match; nothing on a kit screen otherwise.
    info,
};

/// One of the four ways the arrows move the focus.
enum class Direction : uint8_t {
    up,    ///< towards the top
    down,  ///< towards the bottom
    left,  ///< towards the left
    right, ///< towards the right
};

/// Where a finger's press lands.
struct Reached {
    ControlId control{no_control}; ///< the control, or none
    Point at{};                    ///< the point the press takes, in points
};

/// Returns where a finger's press lands.
///
/// On a control, the finger's own point. Otherwise the nearest point of the
/// nearest enabled control whose press area lies within reach, the first of
/// equal distances, and a point another control covers is passed over. With
/// nothing within reach, the finger's own point and no control. A reach of 0
/// or less does not search.
///
/// @param list the display list
/// @param finger the finger, in the same points as the controls
/// @param reach how far a control may lie from the finger, in points
/// @return the control and the point the press takes
[[nodiscard]] Reached reach(const DisplayList& list, Point finger, int32_t reach) noexcept;

/// Returns where a finger's press lands, with a reach that may be a fraction
/// of a point, as a screen at a fractional scale has it.
///
/// As the reach above: on a control, the finger's own point; otherwise the
/// nearest point of the nearest enabled control whose press area lies no
/// farther from the finger than the reach, the first in list order of equal
/// distances; with nothing within reach, the finger's own point and no
/// control. A reach of 0 or less does not search.
///
/// @param list the display list
/// @param finger the finger, in the same points as the controls
/// @param within how far a control may lie from the finger, in points
/// @return the control and the point the press takes
[[nodiscard]] Reached reach(const DisplayList& list, Point finger, float within) noexcept;

/// Returns where a finger's press lands, by a rule for disabled controls.
///
/// As the reach above, but a disabled control the finger is on does as the
/// rule says: with DisabledControl::passes it is passed over, as above; with
/// DisabledControl::blocks the first control whose press area holds the
/// finger, when it is disabled, takes the press to no control, and nothing
/// within reach is searched. The search itself passes over disabled controls
/// either way.
///
/// @param list the display list
/// @param finger the finger, in the same points as the controls
/// @param reach how far a control may lie from the finger, in points
/// @param disabled what a disabled control the finger is on does
/// @return the control and the point the press takes
[[nodiscard]] Reached
reach(const DisplayList& list, Point finger, int32_t reach, DisabledControl disabled) noexcept;

/// What a pointer and the keys have done on a screen, between events.
struct Interaction {
    ControlId hovered{no_control}; ///< the control under the pointer
    ControlId pressed{no_control}; ///< the control a held press is on
    ControlId focused{
        no_control
    }; ///< the control the keys act on, whether or not the ring is drawn
    /// The keys act on the focused control. A screen opens with this false
    /// (focus_opened) unless it asks for the ring, and the focused control is
    /// set either way. A pointer move does not clear it. Enter does, except on
    /// a text field, and the hidden-ring key table then applies again, as at
    /// open. The ring itself is drawn only while focus_ring_visible.
    bool focus_shown{};
    /// The pointer has hidden the ring. The focused control still takes the
    /// keys. Only the drawing reads this.
    bool pointer_hid_ring{};
    /// How far a finger's held press was moved to reach the control it took.
    /// Its later moves and its release are moved by the same amount.
    Point finger_shift{};
};

/// Tells whether the focus ring is drawn.
///
/// The keys read focus_shown. The drawing reads this: a pointer move hides
/// the ring without taking the keys off the focused control.
///
/// @param interaction the focus
/// @return true when the ring is drawn
[[nodiscard]] inline bool focus_ring_visible(const Interaction& interaction) noexcept {
    return interaction.focus_shown && !interaction.pointer_hid_ring;
}

/// A key shows the ring again on the control that already has the focus.
///
/// An opening focus (focus_shown false) is left alone, so the hidden-ring
/// key table still applies. A text field's ring was not hidden.
///
/// @param[in,out] interaction the focus
inline void show_ring_for_key(Interaction& interaction) noexcept {
    if (interaction.focus_shown && interaction.focused != no_control)
        interaction.pointer_hid_ring = false;
}

/// What a pointer event asks the screen to do.
enum class PointerResult : uint8_t {
    none,      ///< nothing changed
    redraw,    ///< only the look changed: a hover, a press or a release off the control
    activated, ///< the press was released over the control it held
};

/// What one pointer event did.
struct PointerOutcome {
    PointerResult result{};        ///< what the screen does
    ControlId control{no_control}; ///< the control it names, or none
    Point at{};                    ///< the point the event used, after a finger's shift
};

/// Moves the hover to the control under the pointer, and hides the focus ring.
///
/// While a press is held, the point is moved by the finger's shift first.
/// A move hides the ring, except on a text field, which keeps its ring, its
/// caret and its typing. The focused control stays, and the next key still
/// acts on it. An opening focus is not hidden this way, so its key table
/// still holds. Only the drawing reads the hide.
///
/// @param[in,out] interaction the pointer's place; its hover changes and the ring hides
/// @param list the display list
/// @param point the pointer, in points
/// @return redraw when the hover moves or the ring's visibility changes, otherwise none
[[nodiscard]] PointerOutcome
pointer_move(Interaction& interaction, const DisplayList& list, Point point);

/// Holds the control under the pointer.
///
/// A press on nothing holds nothing. Once a key has shown the focus, a press
/// on a control that takes the focus moves the focus to it.
///
/// @param[in,out] interaction the pointer's place; its press, hover and shift change
/// @param list the display list
/// @param point the pointer, in points
/// @return redraw when a control is held, otherwise none
[[nodiscard]] PointerOutcome
pointer_down(Interaction& interaction, const DisplayList& list, Point point);

/// Holds the control a finger reaches.
///
/// The press, and the shift that later moves and the release keep, are reach's.
///
/// @param[in,out] interaction the pointer's place
/// @param list the display list
/// @param finger the finger, in points
/// @param reach how far a control may lie from the finger, in points
/// @return what the press did, as pointer_down reports it
[[nodiscard]] PointerOutcome
finger_down(Interaction& interaction, const DisplayList& list, Point finger, int32_t reach);

/// Releases the held control.
///
/// A release over that control activates it, at the point after the finger's
/// shift. A release elsewhere only redraws. A release that held nothing does
/// nothing. The next press starts with no shift.
///
/// @param[in,out] interaction the pointer's place; the press and the shift end
/// @param list the display list
/// @param point the pointer, in points
/// @return activated, redraw or none
[[nodiscard]] PointerOutcome
pointer_up(Interaction& interaction, const DisplayList& list, Point point);

/// Shows an interaction on the parts that draw its controls: a part is
/// focused when it draws the focused control and the ring is drawn
/// (focus_ring_visible), and pressed when it draws the held control. A part
/// that draws no control is neither.
///
/// @param[in,out] list the display list; its items' focused and pressed states change
/// @param interaction the focus and the held control
void mark_states(DisplayList& list, const Interaction& interaction) noexcept;

/// Returns the next control in the declared order.
///
/// The order wraps around. From a control that is not in it, going forward
/// gives the first and going back the last. An empty order gives no control.
///
/// @param list the display list
/// @param from the control to move on from
/// @param forward true for Tab, false for Shift+Tab
/// @return the control, or no_control when the order is empty
[[nodiscard]] ControlId
next_in_tab_order(const DisplayList& list, ControlId from, bool forward) noexcept;

/// Returns the control the focus moves to in a direction.
///
/// Candidates are the focusable, enabled controls of the declared order,
/// other than the one the focus is on, that lie beyond it: their rectangles
/// do not overlap and the candidate is wholly past the facing edge, touching
/// counting as past; or one rectangle contains the other and the candidate's
/// centre lies strictly past the other's in the direction. The focused
/// control's own scroll area is searched first, including controls it shows
/// none of. Only when that finds nothing are controls outside it searched,
/// and a control whose own scroll area shows none of it is left out. A
/// control in no scroll area (group -1) has none of its own: every control
/// is outside it, those in no scroll area among them. Of the
/// candidates, those in line with the focused control win; among them the
/// smallest gap in the direction, then the smallest distance between centres
/// across it, then the earlier in the declared order. With none in line, the
/// smallest gap plus twice the gap across, a gap across of 0 when the spans
/// meet, then the same ties. With no candidate the focus stays.
///
/// @param list the display list
/// @param from the control the focus is on
/// @param direction the way it moves
/// @return the control, or no_control when the focus stays
[[nodiscard]] ControlId
focus_toward(const DisplayList& list, ControlId from, Direction direction) noexcept;

/// Returns the control the focus moves to in a direction, judging where the
/// controls lie with their scroll areas laid out whole.
///
/// As focus_toward above, with each control first put where it lies with
/// every scroll area at rest and as tall as its content: a control in an
/// area, its group an index into `areas`, moves down by that area's offset,
/// and a control in no area whose top lies at or below an area's view moves
/// down by that area's limit. A row scrolled out of view then lies between
/// the fixed controls above and below its area, as it does in the content,
/// and every control is a candidate, whether its area shows it or not. The
/// folder chooser's arrows move this way, from its buttons into its rows.
///
/// @param list the display list
/// @param from the control the focus is on
/// @param direction the way it moves
/// @param areas the scroll areas, by group; their views, offsets and limits are read
/// @return the control, or no_control when the focus stays
[[nodiscard]] ControlId focus_toward(
    const DisplayList& list, ControlId from, Direction direction, std::span<const ScrollArea> areas
) noexcept;

/// What a key asks the screen to do.
enum class /* one key */ KeyResult : uint8_t {
    none,       ///< the key does nothing
    redraw,     ///< the focus moved, or was shown
    to_control, ///< the focused control takes the key
    scroll,     ///< the screen scrolls
    accept,     ///< the screen accepts
    cancel,     ///< the screen cancels
};

/// What one key did.
struct KeyOutcome {
    KeyResult result{};            ///< what the screen does
    ControlId control{no_control}; ///< the control it names, or none
    Key key{};                     ///< the key
};

/// Places the focus a screen opens with.
///
/// The first control of the declared order is focused, and the ring is
/// drawn only when `show` is true. An empty order leaves the focus unset.
/// A focus already set is left where it is, so a later layout does not
/// move it. `show` is the preference that asks every screen to open with
/// the ring; the kit does not read that preference.
///
/// @param[in,out] interaction the focus
/// @param list the display list, for its declared order
/// @param show true to draw the ring on the first control
void focus_opened(Interaction& interaction, const DisplayList& list, bool show) noexcept;

/// Applies one key to a kit screen.
///
/// A screen opens with the first control of its declared order focused and
/// the ring hidden (focus_opened), unless it asks for the ring.
///
/// While the ring is hidden, Tab moves to the control after the focused one
/// and shows the ring there. From none, or from a control that is not in
/// the order, that is the first. Shift+Tab moves to the control before it
/// and shows the ring; from the first, or from none, that is the last. Up
/// shows the ring on the last control. Down, Left, Right and Space show it
/// on the first. Enter does not show the ring: the screen accepts. Escape
/// cancels. The other keys do what they do with nothing focused.
///
/// While the ring shows, Tab and Shift+Tab move in the declared order. Up
/// and Down move by where controls sit. Left and Right go to a focused
/// control that takes steps, and otherwise move by where controls sit.
/// Space goes to the focused control. Enter goes to it when it is a button,
/// a link, a list row or a tab, and otherwise the screen accepts, and then
/// the ring is hidden, except on a text field, which keeps its ring, its
/// caret and its typing. The focused control stays. Escape cancels. Page Up
/// and Page Down scroll. Home and End go to a focused text field, which
/// moves its caret, and otherwise scroll. Backspace and Delete go to a
/// focused text field and otherwise do nothing. Yes, No and Info do
/// nothing: a screen that answers to Info does so before it calls key.
///
/// @param[in,out] interaction the focus; a move shows the ring and sets the control
/// @param list the display list
/// @param key the key
/// @return what the screen does
[[nodiscard]] KeyOutcome key(Interaction& interaction, const DisplayList& list, Key key);

/// Inserts typed text at a field's caret and moves the caret past it.
///
/// The text is refused whole when it is not well-formed UTF-8 (an overlong
/// form, a surrogate, a character past U+10FFFF or a cut sequence) or holds
/// a control character (U+0000 to U+001F, U+007F or U+0080 to U+009F). A
/// caret past the text's end, or inside a character, is first moved back to
/// the boundary before it.
///
/// @param[in,out] field the text and its caret
/// @param utf8 the typed text, in UTF-8
/// @return true when the text was inserted; false when it was refused or empty
bool insert_text(TextField& field, std::string_view utf8);

/// Applies an editing key to a field.
///
/// Backspace deletes the whole character before the caret, and Delete the
/// one after it. Left and Right move the caret one character, Home to the
/// text's start and End to its end. Other keys change nothing. A caret past
/// the text's end, or inside a character, is first moved back to the
/// boundary before it.
///
/// @param[in,out] field the text and its caret
/// @param pressed the key
/// @return true when the text or the caret changed
bool edit_text(TextField& field, Key pressed);

/// The fraction of a row a turn of the wheel has not yet scrolled.
struct WheelCarry {
    float rows{}; ///< rows still to scroll; a fraction, or 0 at an end
};

/// Returns the offset a turn of the wheel scrolls to.
///
/// A notch scrolls step rows. The fraction carries to the next turn. A turn
/// larger than the scrolled area reaches its end in that direction, and a
/// fraction carried towards an end already reached is dropped. A turn that is
/// not a finite number changes nothing. An area that does not scroll drops
/// whatever was carried.
///
/// @param[in,out] carry the fraction still to scroll
/// @param notches the turn, in notches; away from the player is positive and
/// scrolls towards the top
/// @param step how many rows one notch scrolls
/// @param offset how far the area is scrolled, in points
/// @param limit the greatest offset, in points
/// @return the offset after the turn
[[nodiscard]] int32_t wheel_offset(
    WheelCarry& carry, float notches, int32_t step, int32_t offset, int32_t limit
) noexcept;

/// What a kit control is to a screen reader, beside ControlKind.
///
/// `unset` means the reader has not chosen one. `switch_control` is the role
/// written "switch".
enum class AccessRole : uint8_t {
    unset,          ///< not chosen
    none,           ///< decoration
    button,         ///< a push button
    check_box,      ///< a box that is on or off
    switch_control, ///< a switch
    radio_group,    ///< a strip or a row of buttons
    radio,          ///< one part of a strip or a row
    tab_list,       ///< a row of tabs
    tab,            ///< one tab
    list,           ///< a list of rows
    list_item,      ///< one row, or one item of an open menu
    text_field,     ///< a field that takes typed text
    search,         ///< a search field
    slider,         ///< a slider or a scroll bar
    link,           ///< a link
    label,          ///< words that take no pointer
    group,          ///< a group of controls
};

/// One named control, or one part of it, as automation reads it.
struct AutomationEntry {
    std::string name;   ///< its name
    ControlKind kind{}; ///< what it is; a part's is its control's
    Rect rect{};        ///< where it lies, in points
    bool shown{};       ///< some of its press area is in view
    bool enabled{};     ///< a press can reach it
    bool focused{};     ///< it holds the keyboard focus, whether or not the ring is drawn
    bool checked{};     ///< a switch that is on, or a chosen row
    std::string text;   ///< its caption or its value
    bool focusable{};   ///< the arrows and Tab can stop on it; a part never
    /// It is a part of the control before it that takes a press of its own:
    /// a switch's half, a strip's level, a row of buttons' button or an open
    /// drop-down's item (automation_parts).
    bool part{};
    AccessRole role{AccessRole::unset}; ///< what a screen reader calls it
    std::string label;                  ///< the caption; empty when the control has none
    std::string value_text;             ///< its value as words
    bool has_value{};                   ///< `value` is set
    int32_t value{};                    ///< a slider's stop
    bool has_range{};                   ///< `least`, `most` and `step` are set
    int32_t least{};                    ///< the least value
    int32_t most{};                     ///< the greatest value
    int32_t step{};                     ///< how far one step moves the value
    int32_t place{};                    ///< its place in a set, from 1; 0 when it has none
    int32_t set_size{};                 ///< how many share the set; 0 when it has none
    bool chosen{};                      ///< it is the chosen part
    bool pressed{};                     ///< a switch is on
};

/// Returns the named controls, in list order.
///
/// A control with no name is left out. One is shown when any of its press
/// area is in view.
///
/// @param list the display list
/// @param interaction the focus, for which control is focused
/// @return one entry per named control
[[nodiscard]] std::vector<AutomationEntry>
automation_entries(const DisplayList& list, const Interaction& interaction);

/// Returns the named controls as automation presses them: each control, in
/// list order, followed by the parts of it a press reaches on their own.
///
/// A control's rectangle is its press area while any of it shows, so that a
/// press at its centre reaches it; otherwise where it lies. Its parts take
/// their places from the components' own geometry, so a part is pressed
/// where it is drawn:
/// - a control a switch item draws (Role::toggle): its halves, named
///   <name>.off and <name>.on, each half the switch, the On half's the
///   wider by the odd column; checked on the half the switch shows;
/// - a strip of levels (Role::levels): each level where level_at finds it,
///   checked on the chosen level and enabled only where it is offered;
/// - a row of buttons (ControlKind::buttons): each button the list draws for it;
/// - a drop-down whose menu is open (Role::choice, ChoiceLook::open): each
///   item the menu (Role::choice_menu) shows, at the rectangle the menu draws
///   it in, checked on the chosen item. A divider is not listed. An item
///   whose row gives an id is named by that id; another by the control's
///   parts word for it, else its place from 1. A row that is not choosable
///   is listed and is not enabled. A control of the list that bears such an
///   item's name is that item, and is listed once, as the part.
///
/// A part is named <name>.<word>, its word the control's parts word for it,
/// else its place from 1; a switch's are off and on. A part's kind is its
/// control's; it is never focusable or focused, and is enabled while its
/// control is. A part inside its control shows where the control's press
/// area holds it, and its rectangle is that share while it has room.
///
/// @param list the display list
/// @param interaction the focus, for which control is focused
/// @return one entry per named control and per part
[[nodiscard]] std::vector<AutomationEntry>
automation_parts(const DisplayList& list, const Interaction& interaction);

/// Returns the control of a name.
///
/// The first in list order when several share it.
///
/// @param list the display list
/// @param name the name
/// @return that control's number, or no_control when none has it
[[nodiscard]] ControlId control_named(const DisplayList& list, std::string_view name) noexcept;

/// Returns the first fault in the controls' names.
///
/// A name is words of a-z, 0-9 and hyphens joined by dots, at most 100 bytes,
/// and no two controls share one. Names are lower case, so two that differ
/// here differ ignoring case as well.
///
/// @param list the display list
/// @return an empty string when every name is sound, otherwise a sentence naming the first fault
[[nodiscard]] std::string name_problem(const DisplayList& list);

} // namespace oa::ui::kit
