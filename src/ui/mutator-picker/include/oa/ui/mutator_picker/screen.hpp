// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's screen, built only from the UI kit's components: a
// modal window at the size class's dialog size, the filters and the search,
// the On rows numbered above the Off rows, the selected mutator's details,
// and the footer. The pointer, a finger, the keys, typed text, the wheel
// and the controller drive the model and the screen state. Nothing here
// hosts the screen. A host lays it out for its frame, draws it, passes its
// input in and acts on what the input returns. Every player word is looked
// up in the interface catalogue. Which rows show, their order, their lines
// and what is locked come from the model.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/mutator_picker/model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::mutator_picker {

/// The picker's name on the OA layer. Its controls' dialog is oa. and it.
inline constexpr std::string_view screen_name = "mutator-picker";

// The controls' automation names, which journeys press as oa. and the name.

/// Compact's filter drop-down. Its items are all, installed, downloads and
/// tag-<tag> for each tag the filter keeps.
inline constexpr std::string_view name_filter = "mutators.filter";
/// The All pill.
inline constexpr std::string_view name_filter_all = "mutators.filter.all";
/// The Installed pill.
inline constexpr std::string_view name_filter_installed = "mutators.filter.installed";
/// The Downloads pill.
inline constexpr std::string_view name_filter_downloads = "mutators.filter.downloads";
/// The three filter pills, one stop. Its parts are All, Installed and Downloads.
inline constexpr std::string_view name_filters = "mutators.filters";
/// TAGS. Its items are mutators.tags.<tag> for the tags that do not fit as pills.
inline constexpr std::string_view name_tags = "mutators.tags";
/// The tag pills, one stop. Tags, the overflow menu, stays its own stop.
inline constexpr std::string_view name_tag_pills = "mutators.tag-pills";
/// The search field.
inline constexpr std::string_view name_search = "mutators.search";
/// The list. Its rows are mutators.row.<id>.
inline constexpr std::string_view name_list = "mutators.list";
/// What a row's name starts with. The mutator's id, as a name word, follows.
inline constexpr std::string_view name_row_prefix = "mutators.row.";
/// What follows a row's name for its drag handle. A button, out of Tab's order.
inline constexpr std::string_view name_grip_suffix = ".grip";
/// What follows a row's name for its switch. A toggle, parts off and on.
inline constexpr std::string_view name_switch_suffix = ".switch";
/// What an option control's name starts with. The option's id, as a name word, follows.
inline constexpr std::string_view name_option_prefix = "mutators.option.";
/// Add or Replace, on a mutator that brings units or weapons. Parts `add` and `replace`.
inline constexpr std::string_view name_clash = "mutators.clash";
/// ▲ UP.
inline constexpr std::string_view name_up = "mutators.up";
/// ▼ DOWN.
inline constexpr std::string_view name_down = "mutators.down";
/// Compact's DETAILS.
inline constexpr std::string_view name_details = "mutators.details";
/// The details page's BACK.
inline constexpr std::string_view name_back = "mutators.back";
/// CANCEL.
inline constexpr std::string_view name_cancel = "mutators.cancel";
/// USE THESE.
inline constexpr std::string_view name_use = "mutators.use";
/// Read-only's CLOSE.
inline constexpr std::string_view name_done = "mutators.done";
/// The header's close mark.
inline constexpr std::string_view name_close = "mutators.close";
/// The hint. Kind area: it takes no press, and its text is the words shown.
inline constexpr std::string_view name_hint = "mutators.hint";
/// The footer's summary. Kind area, as the hint is.
inline constexpr std::string_view name_summary = "mutators.summary";
/// The verdict chip's words. Kind area, as the hint is.
inline constexpr std::string_view name_verdict = "mutators.verdict";
/// The list's scroll bar. It takes no focus.
inline constexpr std::string_view name_list_bar = "mutators.list.bar";
/// The details' scroll bar. It takes no focus.
inline constexpr std::string_view name_details_bar = "mutators.details.bar";

/// The list pane's width at Regular, in points.
inline constexpr int32_t regular_list_width = 400;
/// The list pane's width at Large, in points.
inline constexpr int32_t large_list_width = 440;
/// The search field's width at Regular, in points.
inline constexpr int32_t regular_search_width = 220;
/// The search field's width at Large, in points.
inline constexpr int32_t large_search_width = 280;
/// Compact's filter drop-down's width, in points.
inline constexpr int32_t compact_filter_width = 128;
/// A row's height at Compact, in points.
inline constexpr int32_t compact_row_height = 32;
/// A row's height at Regular, in points.
inline constexpr int32_t regular_row_height = 34;
/// A row's height at Large, in points.
inline constexpr int32_t large_row_height = 36;
/// The least width of the number column at Regular and Large, in points.
/// A layout widens it to the widest On number it shows.
inline constexpr int32_t number_column = 12;
/// The least width of the number column at Compact, in points.
inline constexpr int32_t compact_number_column = 9;
/// A row's badge, in points. A square.
inline constexpr int32_t row_badge_side = 20;
/// The details' badge, in points. A square.
inline constexpr int32_t details_badge_side = 40;

/// Which drop-down is open. One menu is open at a time.
enum class Menu : uint8_t {
    none,   ///< no menu
    filter, ///< Compact's filter drop-down
    tags,   ///< TAGS
    choice, ///< one option's drop-down; menu_option names it
};

/// A row being dragged, as the screen draws it. The input fills it while a
/// grip is held. The layout does not move the model.
struct DragShown {
    std::string id{};             ///< the mutator's id
    int32_t y{};                  ///< the row's top, in window points
    std::optional<Drop> target{}; ///< where it would land; none draws no line
};

/// What one input asked of the host.
enum class PickerAction : uint8_t {
    none,         ///< the screen did it itself
    cancel,       ///< close the picker
    use,          ///< use the On list
    ask_turn_off, ///< ask before switching a prerequisite off; ids names it, then its dependants
    get,          ///< mutators just switched on that are not installed, prerequisites included
    check,        ///< the mutator whose options changed
};

/// What the host does after one input.
struct ScreenResult {
    PickerAction action{PickerAction::none}; ///< what the host is to do
    /// For get, the mutators just switched on that are not installed, in the
    /// On list's order, prerequisites included. For check, that mutator's id.
    /// For ask_turn_off, the prerequisite, then the On mutators that need it.
    std::vector<std::string> ids{};
    bool redraw{}; ///< lay the screen out and draw it again
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
    find, ///< the controller's Y: the search takes the focus
    back, ///< the controller's B: leave the details page, else close
    flip, ///< the controller's X: flip the selected row's switch
    use,  ///< the controller's Menu: use the set, when USE THESE is enabled
};

/// What the picker's screen keeps between layouts, beside the model.
struct ScreenState {
    kit::Interaction interaction{};  ///< the hover, the press and the focus
    int32_t list_scroll{};           ///< how far the list is scrolled, in points
    int32_t details_scroll{};        ///< how far the details are scrolled, in points
    kit::WheelCarry list_wheel{};    ///< the wheel's turn the list has not scrolled yet
    kit::WheelCarry details_wheel{}; ///< the wheel's turn the details have not scrolled yet
    /// The search field's text and caret. The input keeps its text the same
    /// as the model's query.
    kit::TextField search{};
    Menu menu{Menu::none};     ///< which drop-down is open
    std::string menu_option{}; ///< the option, when menu is choice
    int32_t menu_first{};      ///< the first item the open menu shows
    int32_t menu_marked{-1};   ///< the item the pointer or the keys mark; -1 is none
    /// Compact shows the selected mutator's details on their own page.
    bool details_page{false};
    /// Which scroll bar a press holds.
    enum class Bar : uint8_t {
        none,    ///< none
        list,    ///< the list's
        details, ///< the details'
    };
    Bar bar{Bar::none}; ///< the held scroll bar
    int32_t bar_grab{}; ///< where on the thumb the pointer took hold, from its top

    /// A press that is not the scroll bar: a grip, a finger in a pane, or a
    /// slider's knob. Empty of kind while nothing is held.
    struct PointerHold {
        /// What the press is becoming.
        enum class Kind : uint8_t {
            none,   ///< nothing held
            grip,   ///< a row's drag handle
            finger, ///< a finger in the list or the details, which may scroll
            slider, ///< a number option's knob
        };
        Kind kind{Kind::none}; ///< what is held
        kit::Point from{};     ///< where it pressed, in window points
        int32_t offset{};      ///< how far the pane was scrolled when it pressed
        bool scrolling{};      ///< the finger moved past its reach and presses nothing
        bool in_details{};     ///< the finger pressed in the details, not the list
        kit::ControlId control{kit::no_control}; ///< the slider, while kind is slider
        int32_t grab{}; ///< where on the dragged row the pointer took hold, from its top
    };

    PointerHold hold{}; ///< the press, beside a held scroll bar
    /// Set while a row is being dragged. Empty when none is.
    std::optional<DragShown> drag{};
    /// Why USE THESE's summary waits. Empty when the summary reads the model's
    /// own line. While it is set the summary reads this. It does not disable
    /// USE THESE: a set that still has a mutator to install asks "Get mutators".
    std::string waiting{};
};

/// Tells whether USE THESE can be pressed.
///
/// True when no On mutator is unavailable and none conflicts with another
/// that is on. A mutator that is not installed yet does not disable it, and
/// neither does a download that is still running or that has stopped.
///
/// @param picker the picker
/// @return true when the button is enabled
[[nodiscard]] inline bool use_these_enabled(const Picker& picker) noexcept {
    for (const Chosen& chosen : picker.on) {
        const Mutator* mutator = nullptr;
        for (const Mutator& one : picker.inputs.mutators) {
            if (one.id == chosen.id)
                mutator = &one;
        }
        if (mutator == nullptr)
            continue;
        if (!mutator->unavailable.empty())
            return false;
        for (const std::string& other : mutator->conflicts) {
            for (const Chosen& on : picker.on) {
                if (on.id == other)
                    return false;
            }
        }
    }
    return true;
}

/// Returns the window's size for a frame: the size class's dialog size
/// (kit::metrics_of), no larger than the frame's area.
///
/// @param frame the frame the layer gives
/// @return the width and height, in points
[[nodiscard]] kit::Point window_size(const kit::Frame& frame) noexcept;

/// Lays the picker out for a frame.
///
/// The window, from its top left corner: the header titled "Mutators" with
/// the setup and the mod at its right, and a close mark; the filters and
/// the search; the list, On rows numbered above the Off rows, each with a
/// drag handle, a badge, a name, one line and a switch; the selected
/// mutator's details beside the list at Regular and Large; and the footer.
/// At Compact the details are a page of their own: while details_page is
/// set this returns what details_page_layout returns. Every control is
/// named (the names above). Tab's order is the filter pills as one stop,
/// the tag pills as one stop, TAGS, the search, the list, the details'
/// option controls, the clash mode when the mutator brings units or
/// weapons, ▲ UP, ▼ DOWN, the footer's buttons and the close mark. Left
/// and Right move inside each of those stops. The list is one stop. At
/// Compact it is the drop-down, the search, the list, DETAILS, CANCEL, USE
/// THESE and the close mark. The grips and the labels
/// take no focus, and neither does a row's switch. A drag draws the row
/// whole inside the list's view, and leaves its controls on the well.
///
/// @param picker the picker
/// @param state the screen's state
/// @param frame the frame the layer gives
/// @param fonts the fonts the texts are measured in
/// @return the display list, in the window's points
[[nodiscard]] kit::DisplayList picker_layout(
    const Picker& picker, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
);

/// Lays out Compact's details page: the header, the selected mutator's
/// details, and BACK and USE THESE.
///
/// picker_layout calls this while details_page is set at Compact. Tab's
/// order is the option controls, the clash mode when the mutator brings
/// units or weapons, ▲ UP, ▼ DOWN, BACK, USE THESE and the close mark. A
/// read-only page shows the summary and CLOSE alone.
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

/// Returns the search field while it has the focus, over which a host keeps
/// the system's text input started.
///
/// @param state the focus
/// @param list the display list
/// @return the field's rectangle, in points; none while it has not the focus
[[nodiscard]] std::optional<kit::Rect>
focused_field(const ScreenState& state, const kit::DisplayList& list) noexcept;

/// Takes a pointer's move, press or release, in the window's points.
///
/// A press on a row selects it. A press on its switch flips it, through
/// switch_on or switch_off. A press on its grip holds it: a mouse starts the
/// drag once the pointer has moved 3 points, and a finger starts it at once.
/// While the drag is held, the target is the gap nearest the pointer among
/// the On rows and the Off section, the list scrolls one row when the
/// pointer is within 16 points of the list's top or bottom edge, and drag
/// follows. The release calls drop. A release outside the list, Escape, or
/// the pointer leaving the window ends the drag and changes nothing. A
/// finger pressed on a row, not on its grip, that moves past its reach
/// scrolls the list and presses nothing; a tap selects. A changed option
/// answers check. Scroll bars are held as the Library's are. Read-only
/// selects rows and scrolls the details, and only CLOSE and the header's ×
/// answer.
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
/// selection and keep the focus, leaving the list past its ends; Page Up
/// and Page Down move by the rows shown, and Home and End go to its ends.
/// Space flips the selected row's switch. Enter opens Compact's details
/// page, or moves the focus to the details' first control at Regular and
/// Large. On the details' controls the arrows step a strip or a slider, and
/// Space or Enter flips a switch or opens a drop-down. Escape clears a
/// focused search that holds words, else ends a drag, else leaves the
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

/// Takes typed text into the search while it has the focus, and keeps the
/// model's query the same as the field.
///
/// @param[in,out] picker the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param utf8 the typed text, in UTF-8
/// @return what the host is to do
ScreenResult picker_text(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, std::string_view utf8
);

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

} // namespace oa::ui::mutator_picker
