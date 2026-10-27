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
#include <vector>

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
/// The layout's summary. Kind area: it takes no press, and its text is the
/// whole summary, of which the row shows what fits.
inline constexpr std::string_view name_summary = "key-editor.summary";
/// The Large footer's aside, or the line that asks the player to settle clashes.
inline constexpr std::string_view name_footer_note = "key-editor.footer-note";
/// The list's scroll bar. It takes no focus.
inline constexpr std::string_view name_list_bar = "key-editor.list.bar";
/// The details' scroll bar. It takes no focus.
inline constexpr std::string_view name_details_bar = "key-editor.details.bar";
/// + ADD KEY, in the details.
inline constexpr std::string_view name_detail_add = "key-editor.detail.add";
/// The capture prompt. Its text is the title, the chord and the status line.
inline constexpr std::string_view name_capture = "key-editor.capture";
/// The capture prompt's well.
inline constexpr std::string_view name_capture_well = "key-editor.capture.well";
/// Same place on every keyboard, under More options: a switch whose halves
/// are the letter (off) and the key's place (on).
inline constexpr std::string_view name_capture_means = "key-editor.capture.means";
/// The capture's More options. It shows and hides the place and the gesture.
inline constexpr std::string_view name_capture_more = "key-editor.capture.more";
/// Starts on: a press.
inline constexpr std::string_view name_capture_press = "key-editor.capture.starts.press";
/// Starts on: a double press.
inline constexpr std::string_view name_capture_double = "key-editor.capture.starts.double";
/// Starts on: a hold.
inline constexpr std::string_view name_capture_hold = "key-editor.capture.starts.hold";
/// The line under the well.
inline constexpr std::string_view name_capture_status = "key-editor.capture.status";
/// The capture prompt's CANCEL.
inline constexpr std::string_view name_capture_cancel = "key-editor.capture.cancel";
/// The capture prompt's primary button: Use {chord} for {action}, or Choose
/// what happens… for a chord in use.
inline constexpr std::string_view name_capture_ok = "key-editor.capture.ok";
/// The clash question. Its text is the question and every outcome.
inline constexpr std::string_view name_clash = "key-editor.clash";
/// The outcome Move it to {action}. Choosing it changes nothing yet.
inline constexpr std::string_view name_clash_replace = "key-editor.clash.replace";
/// The outcome Swap them. Absent when the capture is + Add a key.
inline constexpr std::string_view name_clash_swap = "key-editor.clash.swap";
/// The clash question's confirm button, named after the chosen outcome.
inline constexpr std::string_view name_clash_confirm = "key-editor.clash.confirm";
/// Choose another key: back to the capture, nothing changed.
inline constexpr std::string_view name_clash_another = "key-editor.clash.another";
/// The clash question's Cancel.
inline constexpr std::string_view name_clash_cancel = "key-editor.clash.cancel";
/// The outcome Keep it for this action, when a draft clash is settled.
inline constexpr std::string_view name_card_here = "key-editor.card.keep-here";
/// The outcome Keep it for the other action.
inline constexpr std::string_view name_card_there = "key-editor.card.keep-there";
/// Settle…, on a draft clash in the details: opens the clash question.
inline constexpr std::string_view name_card_settle = "key-editor.card.settle";
/// Find by key, beside the search.
inline constexpr std::string_view name_find = "key-editor.find";
/// Find by key's prompt. Its text is the line that says what the chord does.
inline constexpr std::string_view name_find_prompt = "key-editor.find-key";
/// Find by key's well. A press there listens for another chord.
inline constexpr std::string_view name_find_well = "key-editor.find-key.well";
/// What a Find by key row's name starts with. The row's number, from 1,
/// follows. Its text is the action and where it is read; a press chooses it.
inline constexpr std::string_view name_find_row_prefix = "key-editor.find-key.row.";
/// Find by key's Close.
inline constexpr std::string_view name_find_close = "key-editor.find-key.close";
/// Undo, in the footer.
inline constexpr std::string_view name_undo = "key-editor.undo";
/// The footer's draft line. Kind area: its text is the words shown.
inline constexpr std::string_view name_draft = "key-editor.draft";
/// The details' More options. It shows and hides the facts and the helpers.
inline constexpr std::string_view name_detail_more = "key-editor.detail.more";
/// Show as list and Show keyboard, on the keyboard view.
inline constexpr std::string_view name_show_as_list = "key-editor.show-as-list";
/// The keyboard view's list of every key. Its text is every line it shows.
inline constexpr std::string_view name_key_list = "key-editor.key-list";
/// The inspector's Change {action}'s key….
inline constexpr std::string_view name_card_change = "key-editor.key-card.change";
/// The inspector's Use {key} for {action}….
inline constexpr std::string_view name_card_use = "key-editor.key-card.use";
/// The List tab.
inline constexpr std::string_view name_view_list = "key-editor.view.list";
/// The Keyboard tab.
inline constexpr std::string_view name_view_keyboard = "key-editor.view.keyboard";
/// The Mouse tab.
inline constexpr std::string_view name_view_mouse = "key-editor.view.mouse";
/// The Nothing layer.
inline constexpr std::string_view name_layer_none = "key-editor.layer.none";
/// The Shift layer.
inline constexpr std::string_view name_layer_shift = "key-editor.layer.shift";
/// The Ctrl layer.
inline constexpr std::string_view name_layer_ctrl = "key-editor.layer.ctrl";
/// The Alt layer.
inline constexpr std::string_view name_layer_alt = "key-editor.layer.alt";
/// The Cmd layer, on macOS.
inline constexpr std::string_view name_layer_cmd = "key-editor.layer.cmd";
/// The keyboard shape drop-down. Its items are key-editor.keyboard-shape.system,
/// .ansi, .iso and .jis.
inline constexpr std::string_view name_keyboard_shape = "key-editor.keyboard-shape";
/// The keyboard picture. Its keys are key-editor.keyboard, then a dot, then the code word.
inline constexpr std::string_view name_keyboard = "key-editor.keyboard";
/// The card of the key under the pointer or the focus.
inline constexpr std::string_view name_key_card = "key-editor.key-card";
/// The mouse picture. Its buttons are key-editor.mouse.mouse-left and the rest.
inline constexpr std::string_view name_mouse = "key-editor.mouse";
/// Right-click, on the scheme strip.
inline constexpr std::string_view name_scheme_right = "key-editor.scheme.right-click";
/// Left-click, on the scheme strip.
inline constexpr std::string_view name_scheme_left = "key-editor.scheme.left-click";
/// What a mouse-table row's name starts with. The place and the row's number follow.
inline constexpr std::string_view name_mouse_table_prefix = "key-editor.mouse-table.";
/// FIND ON KEYBOARD, in the details.
inline constexpr std::string_view name_detail_find = "key-editor.detail.find";
/// IMPORT… in the footer, and an item of MORE at Compact.
inline constexpr std::string_view name_import = "key-editor.import";
/// SHARE… in the footer, and an item of MORE at Compact.
inline constexpr std::string_view name_share = "key-editor.share";
/// PASTE… in the footer, and an item of MORE at Compact.
inline constexpr std::string_view name_paste = "key-editor.paste";
/// The share prompt's name field.
inline constexpr std::string_view name_share_name = "key-editor.share.name";
/// COPY AS TEXT.
inline constexpr std::string_view name_share_copy = "key-editor.share.copy";
/// SAVE FILE….
inline constexpr std::string_view name_share_save = "key-editor.share.save-file";
/// OPEN FOLDER, after a file was written without a dialog.
inline constexpr std::string_view name_share_open = "key-editor.share.open-folder";
/// The share prompt's CLOSE.
inline constexpr std::string_view name_share_close = "key-editor.share.close";
/// The id the name folds to, so a check can read it.
inline constexpr std::string_view name_share_id = "key-editor.share.id";
/// The share prompt's text, so a check can read the text form.
inline constexpr std::string_view name_share_text = "key-editor.share.text";
/// The scroll bar of a share, paste or import text block, when that block scrolls.
inline constexpr std::string_view name_exchange_bar = "key-editor.exchange.bar";
/// The paste prompt's rows.
inline constexpr std::string_view name_paste_list = "key-editor.paste.list";
/// APPLY {n} CHANGES.
inline constexpr std::string_view name_paste_apply = "key-editor.paste.apply";
/// The paste prompt's CANCEL.
inline constexpr std::string_view name_paste_cancel = "key-editor.paste.cancel";
/// The paste prompt's summary line.
inline constexpr std::string_view name_paste_summary = "key-editor.paste.summary";
/// A mod's Ctrl+F2 keys.
inline constexpr std::string_view name_import_eye = "key-editor.import.from.eye";
/// tdraw's Settings.ini.
inline constexpr std::string_view name_import_tdraw = "key-editor.import.from.tdraw";
/// cnc-ddraw's ddraw.ini.
inline constexpr std::string_view name_import_ddraw = "key-editor.import.from.ddraw";
/// CHOOSE ANOTHER FILE….
inline constexpr std::string_view name_import_choose = "key-editor.import.choose";
/// The import prompt's rows.
inline constexpr std::string_view name_import_list = "key-editor.import.list";
/// IMPORT {n} KEYS.
inline constexpr std::string_view name_import_apply = "key-editor.import.apply";
/// The import prompt's CANCEL.
inline constexpr std::string_view name_import_cancel = "key-editor.import.cancel";
/// The import prompt's summary line.
inline constexpr std::string_view name_import_summary = "key-editor.import.summary";

/// The layout drop-down's least width at Large, in points.
inline constexpr int32_t large_layout_width = 200;
/// The search field and Find by key's least width at Large, in points.
inline constexpr int32_t large_search_width = 360;
/// The nav's width at Large, in points.
inline constexpr int32_t large_nav_width = 172;
/// The chord column's width at Large, in points.
inline constexpr int32_t large_chord_width = 240;
/// The details' width at Large, in points.
inline constexpr int32_t large_details_width = 270;
/// The layout drop-down's least width at Regular, in points.
inline constexpr int32_t regular_layout_width = 180;
/// The search field and Find by key's least width at Regular, in points.
inline constexpr int32_t regular_search_width = 300;
/// The category drop-down's least width at Regular, in points.
inline constexpr int32_t regular_category_width = 180;
/// The details' width at Regular, in points.
inline constexpr int32_t regular_details_width = 240;
/// The category drop-down's least width at Compact, in points.
inline constexpr int32_t compact_category_width = 147;
/// A row's height at Compact, in points: Kit 2's Compact control height.
inline constexpr int32_t compact_row_height = 28;
/// A row's height at Regular, in points: Kit 2's Regular control height.
inline constexpr int32_t regular_row_height = 34;
/// A row's height at Large, in points: Kit 2's Large control height.
inline constexpr int32_t large_row_height = 38;

/// Which drop-down is open. One menu is open at a time.
enum class Menu : uint8_t {
    none,     ///< no menu
    layout,   ///< LAYOUT
    category, ///< the category drop-down
    more,     ///< the footer's More
    shape,    ///< the keyboard's shape
};

/// What one input asked of the host.
enum class EditorAction : uint8_t {
    none,              ///< the screen did it itself
    cancel,            ///< close the editor
    save,              ///< write the draft
    ask_keep_changes,  ///< ask before choosing another layout; id is that layout
    ask_leave,         ///< ask before leaving unsaved changes
    ask_name,          ///< ask the layout's name; id is "duplicate" or "rename"
    copy_text,         ///< put the share text on the clipboard
    save_share,        ///< write the .oakeys package; id is the layout id
    open_share_folder, ///< show the folder the package was written to
    read_clipboard,    ///< read the clipboard and open the paste prompt
    prepare_import,    ///< fill the import offers and the date
    read_import,       ///< read one offer; id is "eye", "choose", or the offer's place
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
    find,    ///< the controller's Y: the search takes the focus
    back,    ///< the controller's B: leave the details page, else ask to leave
    save,    ///< the controller's Menu: save, when SAVE is enabled
    add_key, ///< the controller's X: + ADD KEY on the chosen row
    undo,    ///< Primary+Z: take back the last change
};

/// Which prompt covers the editor. The list is not laid out while one is open.
enum class Prompt : uint8_t {
    none,    ///< the list
    capture, ///< New key for an action
    clash,   ///< the chord is in use, or a draft clash is settled
    find,    ///< Find by key
    share,   ///< Share your keys
    paste,   ///< Paste keys
    import,  ///< Import keys from an older setup
};

/// One file IMPORT… can read, or the mod's Ctrl+F2 keys.
struct ImportOffer {
    std::string id{};      ///< "eye", or the offer's place as digits
    std::string caption{}; ///< the words on the row, already looked up
    std::string path{};    ///< the file; empty for the Ctrl+F2 keys
    std::string tool{};    ///< eye, tdraw or cnc-ddraw
    std::string file{};    ///< Ctrl+F2, or the file's name
};

/// What the editor's screen keeps between layouts, beside the model.
struct ScreenState {
    kit::Interaction interaction{};  ///< the hover, the press and the focus
    int32_t list_scroll{};           ///< how far the list is scrolled, in points
    int32_t details_scroll{};        ///< how far the details are scrolled, in points
    int32_t exchange_scroll{};       ///< how far a share, paste or import text block is scrolled
    kit::WheelCarry list_wheel{};    ///< the wheel's turn the list has not scrolled yet
    kit::WheelCarry details_wheel{}; ///< the wheel's turn the details have not scrolled yet
    /// The search field's text and caret. The rows are filtered by its text.
    kit::TextField search{};
    Menu menu{Menu::none};   ///< which drop-down is open
    int32_t menu_first{};    ///< the first item the open menu shows
    int32_t menu_marked{-1}; ///< the item the pointer or the keys mark; -1 is none
    /// Compact shows the chosen action's details on their own page.
    bool details_page{false};
    /// The category the list shows. "common" is Common commands, where the
    /// editor opens, and "all" every action. A heading is not chosen.
    std::string category{"common"};
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
    bool finger{false};            ///< a finger is down
    kit::Point finger_origin{};    ///< where the finger pressed
    kit::Point finger_at{};        ///< where the finger is
    int32_t finger_scroll{};       ///< the pane's scroll when the finger pressed
    bool finger_moved{false};      ///< the finger has moved past its reach
    bool finger_in_details{false}; ///< the finger pressed in the details
    std::string finger_row{};      ///< the row under the finger, when it pressed on one

    Prompt prompt{Prompt::none}; ///< the capture or the clash question
    Capture capture{};           ///< the capture, while a prompt is open
    /// The clash the question names. `a` is the action being bound, or the
    /// action whose draft clash is settled.
    oa::ui::key_bindings::Clash pending_clash{};
    /// The clash question settles a clash already in the draft, not a capture.
    bool settling{false};
    /// The clash question's chosen outcome: 0 the first, 1 the second.
    uint8_t outcome{0};
    bool capture_more{false}; ///< the capture's More options are open
    bool details_more{false}; ///< the details' More options are open
    /// Find by key's capture. Its action is empty; a finished chord is listed.
    Capture finding{};

    View view{View::list}; ///< which picture is showing
    /// The keyboard the picture draws. The host sets this from the system or the preference.
    kit::KeyboardShape shape{kit::KeyboardShape::ansi};
    /// The shape the system reported. Choosing From your system returns to it.
    kit::KeyboardShape system_shape{kit::KeyboardShape::ansi};
    bool shape_from_system{true}; ///< the drop-down is following the system
    /// 0 is Nothing. Otherwise one modifier bit: Shift, Ctrl, Alt or Meta.
    uint8_t layer{0};
    int32_t keyboard_scroll{0}; ///< how far the compact keyboard is scrolled, in points
    std::string card_code{};    ///< the key whose card is showing; empty shows the focus
    std::string focus_code{};   ///< the key the arrows move
    std::string outline{};      ///< the action FIND ON KEYBOARD is outlining
    std::string finger_key{};   ///< the key a finger is on
    bool finger_armed{false};   ///< the next tap on finger_key acts
    bool extra_buttons{false};  ///< buttons 6 to 8 have been pressed this run
    bool keys_as_list{false};   ///< the keyboard view shows every key as a list
    int32_t key_list_scroll{0}; ///< how far that list is scrolled, in points

    /// The share prompt's name. Typing calls set_share_name.
    kit::TextField share_field{};
    std::string share_place{}; ///< "Saved to {place}" once a file is written; empty before
    PastePreview paste{};      ///< the paste prompt's rows
    ImportPreview importing{}; ///< the import prompt's rows
    std::string import_date{}; ///< the date the host stamps on imported-from
    std::vector<ImportOffer> import_offers{}; ///< the files the host found
    int32_t import_chosen{-1};                ///< which offer is open; -1 is none
    bool import_eye{false};                   ///< the mod has Ctrl+F2 keys to offer
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
EditorResult
editor_key(Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::Key pressed);

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
EditorResult
editor_stick(Editor& editor, ScreenState& state, const kit::DisplayList& list, float notches);

/// Takes a key, button or wheel while the capture prompt is waiting, and
/// moves the focus to OK or CANCEL.
///
/// A host calls this from the screen's shortcut for a key, and from the
/// pointer for a button in the well. Once the chord is finished the prompt
/// is an ordinary dialog and this keeps the event.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param event the input
/// @return what the host is to do
EditorResult editor_capture_input(
    Editor& editor, ScreenState& state, const oa::ui::key_bindings::InputEvent& event
);

/// Finishes Escape when it has been held for a second, and moves the focus.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param now_ms the time, in milliseconds
/// @return what the host is to do
EditorResult editor_capture_tick(Editor& editor, ScreenState& state, uint64_t now_ms);

} // namespace oa::ui::key_editor
