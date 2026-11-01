// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Gadget model of the loaded multiplayer panels.
//
// The multiplayer screen handlers address controls by name and touch a handful of
// gadget-record fields. A Panel holds those fields for one loaded GUI; the
// screen layer copies them back into the layout it renders.
#pragma once

#include "oa/ui/gui_input/scroll_bar.hpp"
#include "oa/ui/gui_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_multiplayer {

inline constexpr std::size_t kPanelControls = 192;
inline constexpr std::size_t kControlNameBytes = 32;
inline constexpr std::size_t kControlTextBytes = 0x80; // Control.text
inline constexpr int32_t kNoControl = -1;

enum class ControlType : uint8_t {
    panel = 0,
    button = 1,
    list_box = 2,
    text_box = 3,
    slider = 4,
    label = 5,
    hot_surface = 6,
    image = 12,
};

// Bits of Control.attributes that the lobby code sets.
inline constexpr uint32_t kAttributeLabelText = 0x10;   // button converted to a label
inline constexpr uint32_t kAttributeRemoteRow = 0x8000; // set on remote rows

// Label colour index written to Control.color for warnings.
inline constexpr int32_t kWarningColor = 0xc;

struct Panel;
// Called with the owning panel when a slider moves.
using SliderHandler = void (*)(Panel& panel, void* user);

struct Control {
    std::array<char, kControlNameBytes> name{};
    ControlType type = ControlType::panel;
    int16_t x = 0, y = 0, width = 0, height = 0;
    uint32_t attributes = 0;
    int32_t color = 0;
    uint8_t active = 0;
    uint8_t stage = 0;   // element state
    uint8_t stages = 0;  // staged-button caption count
    int16_t value = 0;   // group value
    bool grayed = false; // per-type grayed state
    /// A button's quick key: the GUI's quickkey, then the letter its caption
    /// takes when panel_set_text sets one; 0 for none.
    int8_t quick_key{};
    /// A hot surface takes clicks only while this is set; loaded from the GUI's hotornot.
    bool hot{};
    /// Drawn as the GUI draws it, but takes no pointer and no press: the
    /// battle room's RES, which shows a screen size the settings choose.
    bool display_only{};
    /// Light-table level a button's art is drawn at, 1..31; 0 draws it unlit.
    uint8_t light_level{};
    int16_t list_selection = 0;
    int16_t list_first = 0;
    /// A list's row pitch in pixels: the GUI's itemheight, raised by
    /// panel_fill_list to the panel's line height and 1. Only a list with a
    /// pitch follows its group's scroll bar.
    int16_t list_item_height{};
    /// The first row of a list's last full page, which panel_fill_list finds; -1 for an empty list.
    int16_t list_last_first{};
    /// A slider's bar as the engine's gadget layer keeps it: its positions,
    /// maximum, knob, art and, once panel_bind_sliders binds it, its rectangle
    /// between its two arrows, which x, y, width and height then follow. The
    /// bar's own activity and gray state follow the control's.
    oa::ui::gui_input::ScrollBar scroll{};
    /// The GUI's association: a slider and a list it scrolls share one.
    uint8_t group{};
    SliderHandler on_change = nullptr;
    /// A slider without on_change whose knob moves reach the panel's handler as a click on it.
    bool notifies_panel{};
    int16_t source = -1; // layout gadget this control renders as
    std::array<char, kControlTextBytes> text{};
    std::array<char, kControlNameBytes> link{}; // name a click reports
    std::vector<std::string> items;             // list rows (the list's text buffer)
    /// A list's names, one per `items` entry, when the shown text is not the
    /// name a choice uses. Empty for every other list, and for a list whose
    /// rows were replaced without names.
    std::vector<std::string> item_keys;
    /// One byte per `items` entry: non-zero greys that row. Empty greys none.
    std::vector<uint8_t> item_grey;
    /// The picture a hot surface without art shows, as palette indices in
    /// rows of picture_width; empty when it has none, and the surface then
    /// shows a blank. MAPPIC holds the selected map's minimap fitted to its size.
    std::vector<uint8_t> picture;
    int32_t picture_width = 0, picture_height = 0;
};

// Controls 1..count-1 follow the root record 0.
struct Panel {
    std::array<char, kControlNameBytes> name{};
    std::array<Control, kPanelControls> controls{};
    int32_t count = 0;
    int32_t selected = kNoControl; // control being activated
    int32_t focus = kNoControl;    // control receiving keys: a text box, or a list for Up and Down
    uint8_t button = 1;            // mouse button of the activation, 2 = right
    int16_t root_x = 0, root_y = 0;
    /// The GUI font's line height: its 'I' glyph's height and 2. A list's rows follow it.
    int16_t line_height{};
    bool dirty = false;
};

/// Builds a panel from a parsed layout; record 0 is the layout root.
///
/// Copies each gadget's name, type, rectangle, attributes, activity and
/// association, and the button, text box, label, list, scroll bar and hot
/// surface fields; at most 192 gadgets. A scroll bar's maximum is its GUI
/// thickness, as in 3.1c; panel_bind_sliders then binds the sliders. The
/// GUI's defaultfocus control takes the focus, as in 3.1c.
///
/// @param[out] panel Panel to build; marked dirty.
/// @param name Panel name, such as the GUI file's.
/// @param layout Parsed GUI layout.
void panel_load(Panel& panel, std::string_view name, const ui::gui_layout::Layout& layout);

/// Finds a control by name, then by link, ignoring ASCII case; the root record is never matched.
///
/// @param panel Panel to search.
/// @param name Control name or link.
/// @return The control index, or kNoControl.
[[nodiscard]] int32_t panel_find(const Panel& panel, std::string_view name) noexcept;

/// Finds a control by name or link as panel_find does.
///
/// @param panel Panel to search.
/// @param name Control name or link.
/// @return The control, or null.
[[nodiscard]] Control* panel_control(Panel& panel, std::string_view name) noexcept;

/// Finds a control by name or link as panel_find does.
///
/// @param panel Panel to search.
/// @param name Control name or link.
/// @return The control, or null.
[[nodiscard]] const Control* panel_control(const Panel& panel, std::string_view name) noexcept;

/// Returns a control's name.
///
/// @param control Control to read.
/// @return A view of the name up to its terminator.
[[nodiscard]] std::string_view control_name(const Control& control) noexcept;

/// Returns a control's text.
///
/// @param control Control to read.
/// @return A view of the text up to its terminator.
[[nodiscard]] std::string_view control_text(const Control& control) noexcept;

/// Replaces a control's name, cut to 31 characters.
///
/// @param[out] control Control to rename.
/// @param name New name.
void set_control_name(Control& control, std::string_view name) noexcept;

/// Replaces a control's text, cut to 127 characters.
///
/// @param[out] control Control to change.
/// @param text New text.
void set_control_text(Control& control, std::string_view text) noexcept;

/// Replaces the name a click on the control reports, cut to 31 characters.
///
/// @param[out] control Control to change.
/// @param link New link name.
void set_control_link(Control& control, std::string_view link) noexcept;

/// Returns the name a click on the control reports.
///
/// @param control Control to read.
/// @return A view of the link up to its terminator; empty when none was set.
[[nodiscard]] std::string_view control_link(const Control& control) noexcept;

/// Tells whether the control being activated has a name or link, ignoring ASCII case.
///
/// @param panel Panel whose selected control is tested.
/// @param name Name or link to compare.
/// @return True when the selected control matches.
[[nodiscard]] bool panel_selected_is(const Panel& panel, std::string_view name) noexcept;

/// Sets the stage of a named staged button and marks the panel dirty.
///
/// @param[in,out] panel Panel holding the control; a missing control is ignored.
/// @param name Control name or link.
/// @param stage New stage, stored as a byte.
void panel_set_stage(Panel& panel, std::string_view name, int32_t stage) noexcept;

/// Sets the group value of a named control and marks the panel dirty.
///
/// @param[in,out] panel Panel holding the control; a missing control is ignored.
/// @param name Control name or link.
/// @param value New value, stored as a 16-bit integer.
void panel_set_value(Panel& panel, std::string_view name, int32_t value) noexcept;

/// Shows or hides a named control and marks the panel dirty.
///
/// @param[in,out] panel Panel holding the control; a missing control is ignored.
/// @param name Control name or link.
/// @param active True to show the control.
void panel_set_active(Panel& panel, std::string_view name, bool active) noexcept;

/// Grays out or enables a named control and marks the panel dirty.
///
/// @param[in,out] panel Panel holding the control; a missing control is ignored.
/// @param name Control name or link.
/// @param grayed True to gray the control out.
void panel_set_grayed(Panel& panel, std::string_view name, bool grayed) noexcept;

/// Replaces the text of a named control and marks the panel dirty.
///
/// A button then takes the quick key its new caption gives it, as the gadget
/// engine assigns one (ui::gui_input::caption_quick_key): none for a button
/// with stages, the key it had with the no_quick_key attribute or an empty
/// caption, else the first caption letter no other button's key takes
/// (ui::gui_input::free_quick_key).
///
/// @param[in,out] panel Panel holding the control; a missing control is ignored.
/// @param name Control name or link.
/// @param text New text, cut to 127 characters.
void panel_set_text(Panel& panel, std::string_view name, std::string_view text) noexcept;

/// Returns the text of a named control.
///
/// @param panel Panel holding the control.
/// @param name Control name or link.
/// @return The text, or empty when the control is missing.
[[nodiscard]] std::string_view panel_text(const Panel& panel, std::string_view name) noexcept;

/// Replaces the rows of a named list and marks the panel dirty.
///
/// @param[in,out] panel Panel holding the list; a missing control is ignored.
/// @param name List name or link.
/// @param items New rows; a selection past the end moves to row 0.
void panel_set_items(Panel& panel, std::string_view name, std::vector<std::string> items);

/// Finds the first control of a type that shares a control's group, the control itself left out.
///
/// @param panel Panel to search.
/// @param index Control whose group is searched.
/// @param type Type of the control wanted.
/// @return The control index, or kNoControl.
[[nodiscard]] int32_t
panel_group_member(const Panel& panel, int32_t index, ControlType type) noexcept;

/// Readies the panel's lists as 3.1c's first draw of a panel does (gui_input::scroll_list_trim).
///
/// Every shown list starts on its first row with that row selected, and
/// loses the height past its last whole line height + 2 pixels.
///
/// @param[in,out] panel Panel whose lists are readied; without a line height every height is kept.
void panel_bind_lists(Panel& panel) noexcept;

/// Fills a list with rows as 3.1c fills a text list, and shows or hides its scroll bar.
///
/// The list is filled as gui_input::scroll_list_fill fills it. When the list
/// is shown, the first scroll bar in its group is shown exactly when the rows
/// do not all fit, and is then fitted (gui_input::scroll_bar_fit_list) to the
/// group's first list, as in 3.1c.
///
/// @param[in,out] panel Panel holding the list; a missing list is ignored.
/// @param name List name or link.
/// @param rows New rows.
/// @quirk The knob keeps its position, as in 3.1c, so after a refill it can
///        stand away from the first row the list now shows until it moves.
void panel_fill_list(Panel& panel, std::string_view name, std::vector<std::string> rows) noexcept;

/// Sets the names beside a list's rows, one per row.
///
/// A count other than the list's row count clears the names. A missing list
/// is ignored. Replacing the rows clears the names.
///
/// @param[in,out] panel Panel holding the list.
/// @param name List name or link.
/// @param keys One name per row.
void panel_set_item_keys(Panel& panel, std::string_view name, std::vector<std::string> keys);

/// Marks the greyed rows of a list, one byte per row.
///
/// A non-zero byte greys that row. A count other than the list's row count
/// clears the marks. A missing list is ignored. Replacing the rows clears
/// the marks.
///
/// @param[in,out] panel Panel holding the list.
/// @param name List name or link.
/// @param grey One byte per row.
void panel_set_item_grey(Panel& panel, std::string_view name, std::vector<uint8_t> grey);

/// Selects a row of a list and brings it into view, as 3.1c does when a screen picks a row.
///
/// The list and its group's scroll bar move as gui_input::scroll_list_select moves them.
///
/// @param[in,out] panel Panel holding the list; a missing list is ignored.
/// @param name List name or link.
/// @param row Row to select.
void panel_select_list_row(Panel& panel, std::string_view name, int32_t row) noexcept;

/// Brings the rest of a control's group in line with it, as 3.1c does after a list or knob moves.
///
/// A scroll bar puts each list of its group that has a pitch where its knob
/// stands (gui_input::scroll_list_follow_bar). A list gives the other lists
/// of its group its first row and selection, and puts the group's bars'
/// knobs where its first row stands (gui_input::scroll_bar_follow_list).
///
/// @param[in,out] panel Panel holding the group.
/// @param index The list or scroll bar that moved.
void panel_sync_group(Panel& panel, int32_t index) noexcept;

/// Moves a list's selection one row, as the Up and Down keys do in 3.1c (gui_input::scroll_list_step).
///
/// A selection moved on the page brings the list's group in line with it.
///
/// @param[in,out] panel Panel holding the list.
/// @param index The list.
/// @param forward True to move toward the list's end.
void panel_step_list(Panel& panel, int32_t index, bool forward) noexcept;

/// What a press on a list did.
using ListPress = oa::ui::gui_input::ScrollListPress;

/// Presses the pointer on a list as 3.1c does: selects the row under it and focuses the list.
///
/// The row is picked as gui_input::scroll_list_press picks it, and every
/// list in the group, this one included, takes it as its selection, lowered
/// to its own last row.
///
/// @param[in,out] panel Panel holding the list; the list takes the focus unless the press is missed.
/// @param index The list.
/// @param y Pointer row in panel coordinates.
/// @return What the press did.
ListPress panel_press_list(Panel& panel, int32_t index, int32_t y) noexcept;

/// The rows a list shows at once: its height over its row pitch, at least one.
///
/// The pitch is the list's own, or `line_height` when the list has none.
/// This is the page the automation endpoint reports and moves.
///
/// @param list the list
/// @param line_height the panel's line height, used when the list has no pitch
/// @return the rows; 1 when the list has no height
[[nodiscard]] int32_t list_shown_rows(const Control& list, int32_t line_height) noexcept;

/// Scrolls a list with a pitch by whole rows within its first row and its
/// last page's first row, then brings its group in line with it.
///
/// This is the mouse wheel's own scroll, which 3.1c does not have.
///
/// @param[in,out] panel Panel holding the list.
/// @param index The list.
/// @param rows Rows to scroll, negative toward the start.
/// @return True when the list's first row changed.
bool panel_scroll_list(Panel& panel, int32_t index, int32_t rows) noexcept;

/// Copies a control to a new record appended after the loaded ones.
///
/// @param[in,out] panel Panel to extend.
/// @param index Control to copy; the root record cannot be copied.
/// @param suffix Character that replaces the last character of the copy's name.
/// @param dy Vertical offset of the copy in pixels.
/// @return The new index, or kNoControl for a bad index or a full panel.
int32_t panel_clone(Panel& panel, int32_t index, char suffix, int16_t dy) noexcept;

/// Applies the gadget-engine side of a click before the panel handler runs.
///
/// Staged buttons advance their stage and checkbox buttons flip their value.
///
/// @param[in,out] panel Panel clicked; its selected control and mouse button are set.
/// @param index Control clicked.
/// @param button Mouse button, 1 left or 2 right.
/// @return False for an unknown, hidden, grayed or display-only control, or a hot surface
///     that is not hot.
bool panel_press(Panel& panel, int32_t index, uint8_t button = 1) noexcept;

/// Applies the gadget-engine side of a click on a named control, as panel_press by index does.
///
/// @param[in,out] panel Panel clicked.
/// @param name Control name or link.
/// @param button Mouse button, 1 left or 2 right.
/// @return False for an unknown, hidden, grayed or display-only control, or a hot surface
///     that is not hot.
bool panel_press(Panel& panel, std::string_view name, uint8_t button = 1) noexcept;

inline constexpr uint32_t kAttributeCheckbox = 0x80;

/// Finds the topmost shown control under a point.
///
/// Labels, panels, display-only controls and hot surfaces that are not hot
/// are passed over. A bound slider is under the point on its bar and its
/// arrows (gui_input::scroll_bar_part).
///
/// @param panel Panel to search.
/// @param x Column in panel coordinates.
/// @param y Row in panel coordinates.
/// @return The control index, or kNoControl.
[[nodiscard]] int32_t panel_hit(const Panel& panel, int32_t x, int32_t y) noexcept;

/// Binds the panel's sliders to their art and arrows, as 3.1c's first draw does.
///
/// Each slider's bar is bound (gui_input::bind_scroll_bar) and the control
/// takes the bar's rectangle between its arrows; a slider that shares its
/// group with a list is hidden until panel_fill_list shows it.
///
/// @param[in,out] panel Panel whose sliders are bound.
/// @param art SLIDERS frames the panel can use.
void panel_bind_sliders(Panel& panel, const oa::ui::gui_input::ScrollArtFrames& art) noexcept;

/// Returns a slider's bar with the control's activity and gray state.
///
/// @param[in,out] slider Slider read.
/// @return Its bar.
oa::ui::gui_input::ScrollBar& slider_bar(Control& slider) noexcept;

/// Presses the pointer on a control, taking a press on a slider or one of its arrows as in 3.1c.
///
/// A press on a slider is gui_input::scroll_press's, with the control's index
/// as the bar's id; any other press ends the hold.
///
/// @param[in,out] panel Panel pressed.
/// @param[in,out] hold Held state; replaced by the press.
/// @param index Control under the pointer, or kNoControl.
/// @param x Pointer column in panel coordinates.
/// @param y Pointer row in panel coordinates.
/// @param tick Current 30 Hz tick.
/// @return The slider whose handler must run, which every arrow press
///         gives even when the knob stays, or kNoControl.
int32_t slider_press(
    Panel& panel,
    oa::ui::gui_input::ScrollHold& hold,
    int32_t index,
    int32_t x,
    int32_t y,
    uint32_t tick
) noexcept;

/// Releases the pointer, ending any hold, as gui_input::scroll_release does.
///
/// @param[in,out] panel Panel the pointer is over.
/// @param[in,out] hold Held state; cleared.
/// @param x Pointer column in panel coordinates.
/// @param y Pointer row in panel coordinates.
/// @return The slider whose knob moved, or kNoControl.
int32_t
slider_release(Panel& panel, oa::ui::gui_input::ScrollHold& hold, int32_t x, int32_t y) noexcept;

/// Runs the held slider or arrow for one update of the screen, as gui_input::scroll_hold_tick does.
///
/// @param[in,out] panel Panel held.
/// @param[in,out] hold Held state.
/// @param tick Current 30 Hz tick.
/// @return The slider whose handler must run, which every arrow step gives
///         even when the knob stays, or kNoControl.
int32_t slider_hold_tick(Panel& panel, oa::ui::gui_input::ScrollHold& hold, uint32_t tick) noexcept;

} // namespace oa::ui::frontend_multiplayer
