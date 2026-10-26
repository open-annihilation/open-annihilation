// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's screen, built only from the UI kit's components: a modal
// window at the size class's dialog size, the layout and the search, the
// categories, the list of actions and the chosen action's details. The
// pointer, a finger, the keys, typed text, the wheel and the controller
// drive the model and the screen state. Nothing here hosts the screen.
// A host lays it out for its frame, draws it, passes its input in and acts
// on what the input returns. Every player word is looked up in the
// interface catalogue. Which rows show, and what a change does, come from
// the model.
#pragma once

#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oa::ui::key_editor {

/// The editor's name on the OA layer. Its controls' dialog is oa. and it.
inline constexpr std::string_view screen_name = "key-editor";

// The controls' automation names, which journeys press as oa. and the name.

/// The layout drop-down. Its items are key-editor.layout.<id>, and
/// key-editor.layout.duplicate and key-editor.layout.rename.
inline constexpr std::string_view name_layout = "key-editor.layout";
/// The search field.
inline constexpr std::string_view name_search = "key-editor.search";
/// The category drop-down, at Regular and Compact. Its items are
/// key-editor.category.<id>. At Compact the filter pills are items of it too,
/// named key-editor.filter.<filter>.
inline constexpr std::string_view name_category = "key-editor.category";
/// What a Large nav entry's name starts with. The category id follows.
inline constexpr std::string_view name_nav_prefix = "key-editor.nav.";
/// The All pill.
inline constexpr std::string_view name_filter_all = "key-editor.filter.all";
/// The Changed pill.
inline constexpr std::string_view name_filter_changed = "key-editor.filter.changed";
/// The Clashes pill.
inline constexpr std::string_view name_filter_clashes = "key-editor.filter.clashes";
/// The Unbound pill.
inline constexpr std::string_view name_filter_unbound = "key-editor.filter.unbound";
/// The Latchable pill.
inline constexpr std::string_view name_filter_latchable = "key-editor.filter.latchable";
/// The list. Its rows are key-editor.row.<action id>.
inline constexpr std::string_view name_list = "key-editor.list";
/// What a row's name starts with. The action id follows, when that name is sound.
inline constexpr std::string_view name_row_prefix = "key-editor.row.";
/// Number keys select groups. It heads the Groups category.
inline constexpr std::string_view name_number_keys = "key-editor.number-keys";
/// What a details chord's remove control's name starts with. The chord's
/// number, from 1, and ".remove" follow.
inline constexpr std::string_view name_chord_prefix = "key-editor.detail.chord.";
/// Latch, in the details.
inline constexpr std::string_view name_latch = "key-editor.detail.latch";
/// Hold to confirm, in the details.
inline constexpr std::string_view name_confirm = "key-editor.detail.confirm";
/// RESET TO <layout>, in the details.
inline constexpr std::string_view name_detail_reset = "key-editor.detail.reset";
/// Compact's details page's BACK.
inline constexpr std::string_view name_back = "key-editor.back";
/// Compact's MORE. Its item is key-editor.restore.
inline constexpr std::string_view name_more = "key-editor.more";
/// RESTORE DEFAULTS. At Compact it is the one item of MORE.
inline constexpr std::string_view name_restore = "key-editor.restore";
/// CANCEL.
inline constexpr std::string_view name_cancel = "key-editor.cancel";
/// SAVE.
inline constexpr std::string_view name_save = "key-editor.save";
/// The header's close mark.
inline constexpr std::string_view name_close = "key-editor.close";
/// The layout's summary. Kind area: it takes no press, and its text is the words shown.
inline constexpr std::string_view name_summary = "key-editor.summary";
/// The Large footer's aside, or the line that asks the player to settle clashes.
inline constexpr std::string_view name_footer_note = "key-editor.footer-note";
/// The list's scroll bar. It takes no focus.
inline constexpr std::string_view name_list_bar = "key-editor.list.bar";
/// The details' scroll bar. It takes no focus.
inline constexpr std::string_view name_details_bar = "key-editor.details.bar";

/// The layout drop-down's least width at Large, in points.
inline constexpr int32_t large_layout_width = 240;
/// The search field's least width at Large, in points.
inline constexpr int32_t large_search_width = 280;
/// The nav's width at Large, in points.
inline constexpr int32_t large_nav_width = 152;
/// The chord column's width at Large, in points.
inline constexpr int32_t large_chord_width = 240;
/// The details' width at Large, in points.
inline constexpr int32_t large_details_width = 270;
/// The layout drop-down's least width at Regular, in points.
inline constexpr int32_t regular_layout_width = 180;
/// The search field's least width at Regular, in points.
inline constexpr int32_t regular_search_width = 200;
/// The category drop-down's least width at Regular, in points.
inline constexpr int32_t regular_category_width = 140;
/// The details' width at Regular, in points.
inline constexpr int32_t regular_details_width = 240;
/// The category drop-down's least width at Compact, in points.
inline constexpr int32_t compact_category_width = 147;
/// A row's height at Compact, in points.
inline constexpr int32_t compact_row_height = 22;
/// A row's height at Regular, in points.
inline constexpr int32_t regular_row_height = 24;
/// A row's height at Large, in points.
inline constexpr int32_t large_row_height = 26;

/// Which drop-down is open. One menu is open at a time.
enum class Menu : uint8_t {
    none,     ///< no menu
    layout,   ///< LAYOUT
    category, ///< the category drop-down
    more,     ///< Compact's MORE
};

/// What one input asked of the host.
enum class EditorAction : uint8_t {
    none,             ///< the screen did it itself
    cancel,           ///< close the editor
    save,             ///< write the draft
    ask_keep_changes, ///< ask before choosing another layout; id is that layout
    ask_leave,        ///< ask before leaving unsaved changes
    ask_name,         ///< ask the layout's name; id is "duplicate" or "rename"
};

/// What the host does after one input.
struct EditorResult {
    EditorAction action{EditorAction::none}; ///< what the host is to do
    std::string id{};                        ///< the layout, or "duplicate" or "rename"
    bool redraw{};                           ///< lay the screen out and draw it again
};

/// What a pointer did, as the OA layer passes it to a screen.
enum class PointerKind : uint8_t {
    move, ///< it moved
    down, ///< a button or a finger pressed
    up,   ///< a button or a finger let go
};

/// The editor's own keys and controller buttons, beyond the kit's keys. A
/// host maps the platform's keys and buttons to these. The controller's
/// D-pad and left stick are the kit's arrows, its A is Space, and its right
/// stick is editor_stick. Tab stays a key: the host turns the pad's L1 and
/// R1 into it.
enum class EditorCommand : uint8_t {
    find, ///< the controller's Y: the search takes the focus
    back, ///< the controller's B: leave the details page, else ask to leave
    save, ///< the controller's Menu: save, when SAVE is enabled
};

/// What the editor's screen keeps between layouts, beside the model.
struct ScreenState {
    kit::Interaction interaction{}; ///< the hover, the press and the focus
    int32_t list_scroll{};          ///< how far the list is scrolled, in points
    int32_t details_scroll{};       ///< how far the details are scrolled, in points
    kit::WheelCarry list_wheel{};   ///< the wheel's turn the list has not scrolled yet
    kit::WheelCarry details_wheel{}; ///< the wheel's turn the details have not scrolled yet
    /// The search field's text and caret. The rows are filtered by its text.
    kit::TextField search{};
    Menu menu{Menu::none}; ///< which drop-down is open
    int32_t menu_first{};  ///< the first item the open menu shows
    int32_t menu_marked{-1}; ///< the item the pointer or the keys mark; -1 is none
    /// Compact shows the chosen action's details on their own page.
    bool details_page{false};
    /// The category the list shows. "all" is every category. A heading is not chosen.
    std::string category{"all"};
    Filter filter{Filter::all}; ///< which rows the list keeps
    std::string selected{};     ///< the chosen action; empty when none is
    /// The editor was opened over a match. The header says the game is paused.
    bool in_match{false};

    /// Which scroll bar a press holds.
    enum class Bar : uint8_t {
        none,    ///< none
        list,    ///< the list's
        details, ///< the details'
    };
    Bar bar{Bar::none}; ///< the held scroll bar
    int32_t bar_grab{}; ///< where on the thumb the pointer took hold, from its top

    /// A finger pressed in a pane, which may become a scroll.
    bool finger{false};           ///< a finger is down
    kit::Point finger_origin{};   ///< where the finger pressed
    kit::Point finger_at{};       ///< where the finger is
    int32_t finger_scroll{};      ///< the pane's scroll when the finger pressed
    bool finger_moved{false};     ///< the finger has moved past its reach
    bool finger_in_details{false}; ///< the finger pressed in the details
    std::string finger_row{};     ///< the row under the finger, when it pressed on one
};

/// The name of one list row. An action id that would make
/// `key-editor.row.<id>` unsound, or longer than 100 bytes, or a `gui.` id
/// longer than 80 bytes, is named `key-editor.row.n<index>` instead.
///
/// @param action_id the action id
/// @param index the row's place among the rows the list could show, from 0
/// @return the automation name
[[nodiscard]] std::string row_control_name(std::string_view action_id, std::size_t index);

/// Returns the editor's window size: the size class's dialog, and no larger
/// than the frame.
///
/// @param frame the frame the layer gives
/// @return the width and height, in points
[[nodiscard]] kit::Point window_size(const kit::Frame& frame) noexcept;

/// Lays the editor out for a frame.
///
/// The window, from its top left corner: the header, the layout drop-down
/// and the search, the categories (a nav at Large, a drop-down at Regular
/// and Compact) and the filter pills (inside the category drop-down at
/// Compact), the list, the chosen action's details beside the list at
/// Regular and Large, and the footer. At Compact the details are a page of
/// their own: while details_page is set this returns what details_page_layout
/// returns. Groups starts with Number keys select groups. A read-only row is
/// faded. Tab's order is the layout, the search, the categories, the pills,
/// the list, the details' controls, the footer and the close mark. The list
/// is one stop: the arrows move the chosen row. Only a row whose rectangle
/// lies in the list's view is a control.
///
/// @param editor the editor
/// @param state the screen's state
/// @param frame the frame the layer gives
/// @param fonts the fonts the texts are measured in
/// @return the display list, in the window's points
[[nodiscard]] kit::DisplayList editor_layout(
    const Editor& editor, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
);

/// Lays out Compact's details page: the header, the chosen action, and
/// BACK, RESET and SAVE.
///
/// editor_layout calls this while details_page is set at Compact. Tab's
/// order is the page's controls, BACK, RESET and SAVE.
///
/// @param editor the editor
/// @param state the screen's state
/// @param frame the frame the layer gives
/// @param fonts the fonts the texts are measured in
/// @return the display list, in the window's points
[[nodiscard]] kit::DisplayList details_page_layout(
    const Editor& editor, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
);

/// Draws the editor: the window's face, the display list, then the window's
/// edge (the kit's chrome), from the canvas's origin.
///
/// @param canvas where it is drawn
/// @param list what editor_layout or details_page_layout gave
/// @param size the window's size, as window_size gave it
void draw_editor(const kit::Canvas& canvas, const kit::DisplayList& list, kit::Point size);

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
/// A press on a row chooses it. Enter's job on a row is the key's. A press
/// on a chord's remove takes that chord off. A finger pressed in a pane that
/// moves past its reach scrolls that pane, and a tap chooses the row. Scroll
/// bars are held as the Library's are. An open menu takes the press first.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param kind a move, a press or a release
/// @param button the pointer's button, 1 for the left; 0 for a move
/// @param at the pointer, in the window's points; it may lie outside the window
/// @param finger_reach how far a finger's press may lie from a control, in
///     points; 0 for a mouse
/// @return what the host is to do
EditorResult editor_pointer(
    Editor& editor,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at,
    int32_t finger_reach
);

/// Takes a key, as the kit names it.
///
/// Tab and Shift+Tab follow the declared order. On the list, Up and Down
/// move the chosen row and keep the focus, Page Up and Page Down move by the
/// rows shown, and Home and End go to its ends. Enter on a row opens
/// Compact's details page, or moves the focus to the details' first control
/// at Regular and Large. Delete on a focused chord removes it. Escape closes
/// an open menu, else clears a search that holds words, else leaves Compact's
/// details page, else asks to leave or closes. An open menu takes the arrows,
/// Home, End, Page Up, Page Down, Enter, Space and Escape first. Typing is
/// editor_text, not a key.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param pressed the key
/// @return what the host is to do
EditorResult editor_key(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::Key pressed
);

/// Takes text typed into the focused search.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param utf8 the text
/// @return what the host is to do
EditorResult editor_text(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, std::string_view utf8
);

/// Takes a turn of the wheel. It scrolls the pane under the pointer.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param at the pointer, in the window's points
/// @param notches how far the wheel turned
/// @return what the host is to do
EditorResult editor_wheel(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::Point at, float notches
);

/// Takes one of the editor's own commands.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param command the command
/// @return what the host is to do
EditorResult editor_command(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, EditorCommand command
);

/// Takes the controller's right stick as a wheel on the focused pane.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param notches how far the stick asked to scroll
/// @return what the host is to do
EditorResult editor_stick(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, float notches
);

} // namespace oa::ui::key_editor
