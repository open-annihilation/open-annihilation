// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain seat picker's screen, built only from the UI kit's components:
// a modal window at the size class's dialog size, the list of brains, the
// selected brain's details, and the footer. The pointer, a finger, the keys,
// the wheel and the controller drive the model and the screen state. Nothing
// here hosts the screen. A host lays it out for its frame, draws it, passes
// its input in and acts on what the input returns. Every player word is
// looked up in the interface catalogue. Which rows show, their order, their
// lines and what is locked come from the model.
#pragma once

#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::ui::brain_picker {

/// The picker's name on the OA layer. Its controls' dialog is oa. and it.
inline constexpr std::string_view screen_name = "brain-picker";

// The controls' automation names, which journeys press as oa. and the name.

/// The list. Its rows are brains.row.<id>, and the built-in player's is
/// brains.row.built-in. Kind list.
inline constexpr std::string_view name_list = "brains.list";
/// What a row's name starts with. The brain's id, as a name word, follows.
/// The built-in player's name is brains.row.built-in. Kind list_item. Its
/// text is the name.
inline constexpr std::string_view name_row_prefix = "brains.row.";
/// The level strip. Its parts are the level ids, in the brain's order.
inline constexpr std::string_view name_level = "brains.level";
/// What an option control's name starts with. The option's id, as a name
/// word, follows.
inline constexpr std::string_view name_option_prefix = "brains.option.";
/// "Use for every computer seat". A toggle, parts off and on.
inline constexpr std::string_view name_every_seat = "brains.every-seat";
/// Compact's DETAILS.
inline constexpr std::string_view name_details = "brains.details";
/// The details page's BACK.
inline constexpr std::string_view name_back = "brains.back";
/// CANCEL.
inline constexpr std::string_view name_cancel = "brains.cancel";
/// USE THIS.
inline constexpr std::string_view name_use = "brains.use";
/// The header's close mark.
inline constexpr std::string_view name_close = "brains.close";
/// The hint under the header. Kind area: it takes no press, and its text is
/// the words shown.
inline constexpr std::string_view name_hint = "brains.hint";
/// The footer's summary. Kind area, as the hint is.
inline constexpr std::string_view name_summary = "brains.summary";
/// The details' name. Kind area, so a text-only details pane still has a
/// clip the scroll can read. It takes no press and no focus.
inline constexpr std::string_view name_brain = "brains.name";
/// The list's scroll bar. It takes no focus.
inline constexpr std::string_view name_list_bar = "brains.list.bar";
/// The details' scroll bar. It takes no focus.
inline constexpr std::string_view name_details_bar = "brains.details.bar";

/// The list pane's width at Regular, in points.
inline constexpr int32_t regular_list_width = 300;
/// The list pane's width at Large, in points.
inline constexpr int32_t large_list_width = 360;
/// A row's height at Compact, in points.
inline constexpr int32_t compact_row_height = 32;
/// A row's height at Regular, in points.
inline constexpr int32_t regular_row_height = 34;
/// A row's height at Large, in points.
inline constexpr int32_t large_row_height = 36;
/// A row's badge, in points. A square.
inline constexpr int32_t row_badge_side = 20;
/// The details' badge, in points. A square.
inline constexpr int32_t details_badge_side = 40;

/// Which drop-down is open. One menu is open at a time, and only an
/// option's drop-down opens.
enum class Menu : uint8_t {
    none,   ///< no menu
    choice, ///< one option's drop-down; menu_option names it
};

/// What one input asked of the host.
enum class PickerAction : uint8_t {
    none,   ///< the screen did it itself
    cancel, ///< close the picker
    use,    ///< use the choice
};

/// What the host does after one input.
struct ScreenResult {
    PickerAction action{PickerAction::none}; ///< what the host is to do
    bool redraw{};                           ///< lay the screen out and draw it again
};

/// What a pointer did, as the OA layer passes it to a screen.
enum class PointerKind : uint8_t {
    move, ///< it moved
    down, ///< a button or a finger pressed
    up,   ///< a button or a finger let go
};

/// The picker's own keys and controller buttons, beyond the kit's keys. A
/// host maps the platform's keys and buttons to these. The controller's
/// D-pad and left stick are the kit's arrows, its A is Space, and its right
/// stick is picker_stick.
enum class ScreenCommand : uint8_t {
    back, ///< the controller's B: close a menu, else leave the details page, else close
    use,  ///< the controller's Menu: use the choice
};

/// What the picker's screen keeps between layouts, beside the model.
struct ScreenState {
    kit::Interaction interaction{};  ///< the hover, the press and the focus
    int32_t list_scroll{};           ///< how far the list is scrolled, in points
    int32_t details_scroll{};        ///< how far the details are scrolled, in points
    kit::WheelCarry list_wheel{};    ///< the wheel's turn the list has not scrolled yet
    kit::WheelCarry details_wheel{}; ///< the wheel's turn the details have not scrolled yet
    Menu menu{Menu::none};           ///< which drop-down is open
    std::string menu_option{};       ///< the option, when menu is choice
    int32_t menu_first{};            ///< the first item the open menu shows
    int32_t menu_marked{-1};         ///< the item the pointer or the keys mark; -1 is none
    /// Compact shows the selected brain's details on their own page.
    bool details_page{false};
    /// Which scroll bar a press holds.
    enum class Bar : uint8_t {
        none,    ///< none
        list,    ///< the list's
        details, ///< the details'
    };
    Bar bar{Bar::none}; ///< the held scroll bar
    int32_t bar_grab{}; ///< where on the thumb the pointer took hold, from its top

    /// A press that is not the scroll bar: a finger in a pane, or a slider's
    /// knob. Empty of kind while nothing is held.
    struct PointerHold {
        /// What the press is becoming.
        enum class Kind : uint8_t {
            none,   ///< nothing held
            finger, ///< a finger in the list or the details, which may scroll
            slider, ///< a number option's knob
        };
        Kind kind{Kind::none}; ///< what is held
        kit::Point from{};     ///< where it pressed, in window points
        int32_t offset{};      ///< how far the pane was scrolled when it pressed
        bool scrolling{};      ///< the finger moved past its reach and presses nothing
        bool in_details{};     ///< the finger pressed in the details, not the list
        kit::ControlId control{kit::no_control}; ///< the slider, while kind is slider
    };

    PointerHold hold{}; ///< the press, beside a held scroll bar
};

/// Returns the window's size for a frame: the size class's dialog size
/// (kit::metrics_of), no larger than the frame's area.
///
/// @param frame the frame the layer gives
/// @return the width and height, in points
[[nodiscard]] kit::Point window_size(const kit::Frame& frame) noexcept;

/// Lays the picker out for a frame.
///
/// The window, from its top left corner: the header titled "Computer player"
/// with the seat and the mod at its right, and a close mark; the hint, when
/// the model gives one; the list, the built-in player first, each row with a
/// badge, a name, one line, the Full awareness chip at Regular and Large, and
/// the check mark on the chosen row; the selected brain's details beside the
/// list at Regular and Large; and the footer. At Compact the details are a
/// page of their own: while details_page is set this returns what
/// details_page_layout returns. Every control is named (the names above).
/// Tab's order is the list, the level strip, the option controls, the
/// every-seat switch, CANCEL, USE THIS and the close mark. At Compact it is
/// the list, DETAILS, CANCEL, USE THIS and the close mark. The labels take
/// no focus, and neither does a scroll bar.
///
/// @param picker the picker
/// @param state the screen's state
/// @param frame the frame the layer gives
/// @param fonts the fonts the texts are measured in
/// @return the display list, in the window's points
[[nodiscard]] kit::DisplayList picker_layout(
    const Picker& picker, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
);

/// Lays out Compact's details page: the header, the selected brain's
/// details, and BACK and USE THIS.
///
/// picker_layout calls this while details_page is set at Compact. Tab's
/// order is the level strip, the option controls, the every-seat switch,
/// BACK, USE THIS and the close mark.
///
/// @param picker the picker
/// @param state the screen's state
/// @param frame the frame the layer gives
/// @param fonts the fonts the texts are measured in
/// @return the display list, in the window's points
[[nodiscard]] kit::DisplayList details_page_layout(
    const Picker& picker, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
);

/// Draws the picker: the window's face, the display list, then the window's
/// edge (the kit's chrome), from the canvas's origin.
///
/// @param canvas where it is drawn
/// @param list what picker_layout or details_page_layout gave
/// @param size the window's size, as window_size gave it
void draw_picker(const kit::Canvas& canvas, const kit::DisplayList& list, kit::Point size);

/// Takes a pointer's move, press or release, in the window's points.
///
/// A press on a row selects it, through select. A press on the level strip,
/// an option control or the every-seat switch changes that control: a
/// slider's knob is held and dragged, and a drop-down is opened and an item
/// chosen. DETAILS, BACK, CANCEL, USE THIS and the header's × do what they
/// say. A finger that moves past its reach in the list or the details scrolls
/// that pane and presses nothing on lift. A tap selects. A press outside an
/// open drop-down only closes it. Scroll bars are held as the Library's are.
///
/// @param[in,out] picker the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param kind a move, a press or a release
/// @param button the pointer's button, 1 for the left; 0 for a move
/// @param at the pointer, in the window's points; it may lie outside the window
/// @param finger_reach how far a finger's press may lie from a control, in
///     points; 0 for a mouse
/// @return what the host is to do
ScreenResult picker_pointer(
    Picker& picker,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at,
    int32_t finger_reach
);

/// Takes a key, as the kit names it.
///
/// Tab and Shift+Tab follow the declared order, and a details control that
/// is clipped out of view is scrolled into view so the keys can reach it.
/// The arrows move by where controls sit. On the list, Up and Down move the
/// selection and keep the focus, staying on the list at its ends. Page Up
/// and Page Down move by the rows shown, and Home and End go to its ends.
/// Left and Right step a level strip, a choice strip or a slider. Space flips
/// the every-seat switch or a toggle. Enter opens Compact's details page, or
/// moves the focus to the details' first control at Regular and Large, and
/// on USE THIS answers use. Escape closes an open drop-down, else leaves the
/// details page, else answers cancel. An open drop-down takes the arrows,
/// Home, End, Page Up, Page Down, Enter, Space and Escape first.
///
/// @param[in,out] picker the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param pressed the key
/// @return what the host is to do
ScreenResult
picker_key(Picker& picker, ScreenState& state, const kit::DisplayList& list, kit::Key pressed);

/// Takes a turn of the wheel: it scrolls the pane under the pointer, or the
/// open drop-down's items.
///
/// @param[in,out] picker the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param at the pointer, in the window's points
/// @param notches the turn; positive away from the player, towards the top
/// @return what the host is to do
ScreenResult picker_wheel(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, kit::Point at, float notches
);

/// Takes one of the picker's own keys or controller buttons.
///
/// `back` does what Escape does. `use` answers use.
///
/// @param[in,out] picker the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param command what was pressed
/// @return what the host is to do
ScreenResult picker_command(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, ScreenCommand command
);

/// Takes the controller's right stick: it scrolls the pane that holds the
/// focus, the details while the focus is on one of their controls, and the
/// list otherwise.
///
/// @param[in,out] picker the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param notches the stick's turn, as notches of the wheel; positive towards the top
/// @return what the host is to do
ScreenResult
picker_stick(Picker& picker, ScreenState& state, const kit::DisplayList& list, float notches);

} // namespace oa::ui::brain_picker
