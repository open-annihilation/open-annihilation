// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The control tree the screen reader hears, and the events its actions become.
// A build that leaves the bridge out cannot include this header: it names
// AccessKit. Callers include it only when OA_SCREEN_READER_COMPILED_OUT is
// not set. screen_reader.hpp is the header that build can include.
#pragma once

#include "oa/platform/screen_reader/api.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oa::platform::screen_reader {

/// What a control is to the screen reader. Decoration is `none` and is left
/// out of the tree AccessKit is given.
enum class Role : uint8_t {
    none,           ///< decoration, left out
    button,         ///< a push button, including a colour cell
    check_box,      ///< a box that is on or off
    switch_control, ///< a switch; `switch` is a keyword
    radio_group,    ///< a strip or a row, with one part chosen
    radio,          ///< one part of a strip or a row
    tab_list,       ///< a row of tabs
    tab,            ///< one tab
    list,           ///< a list of rows
    list_item,      ///< one row
    tree,           ///< a tree of rows
    tree_item,      ///< one row of a tree
    text_field,     ///< a field that takes typed text
    password,       ///< a masked field; its value is a length, never the text
    search,         ///< a search field
    slider,         ///< a slider, a scroll bar or a stepper
    log,            ///< a log of lines, announced politely
    dialog,         ///< a dialog
    alert_dialog,   ///< a message box
    group,          ///< a group of controls, such as a console
    image,          ///< a picture with a meaning
    link,           ///< a link
    label,          ///< words that take no pointer
    window,         ///< the game's window, the root of the tree
};

/// How a change of a control is announced.
enum class LiveSetting : uint8_t {
    off,       ///< a change is not announced
    polite,    ///< a change waits for a pause
    assertive, ///< a change is announced at once
};

/// What a screen reader asks the game to do.
enum class Action : uint8_t {
    click,     ///< press, the default action
    focus,     ///< move the focus
    increment, ///< the next value
    decrement, ///< the previous value
    set_value, ///< replace the value
    expand,    ///< open a row
    collapse,  ///< close a row
};

/// How an action reaches the game. The runtime pushes the same SDL events
/// `ui.click` and `ui.key` push.
enum class EventKind : uint8_t {
    press,  ///< a click at a point: the pointer moves, the button goes down and up
    motion, ///< the pointer moves, and nothing is pressed
    key,    ///< a key goes down and up, `repeats` times
    text,   ///< typed text, with no click
};

/// A key an action becomes.
enum class Key : uint8_t {
    left,  ///< the previous value, or a collapsed row
    right, ///< the next value, or an expanded row
};

/// One control in the tree the screen reader hears.
///
/// Bounds are window points, the same space as `SDL_GetWindowSize`, so a
/// point here is a point a pointer event already uses.
struct Node {
    uint64_t id{}; ///< stable id; never 0, and never `window_node` except the root
    Role role{Role::none};
    std::string label;       ///< the name a player hears
    std::string value_text;  ///< the value as words, such as "Gamma 1.0"
    std::string description; ///< more words; empty when there are none
    std::string shortcut;    ///< the quick key, as the player reads it
    bool has_value{};        ///< `value` is set
    double value{};          ///< a slider's position, or another control's number
    bool has_range{};        ///< `least`, `most` and `step` are set
    double least{};          ///< the least value
    double most{};           ///< the greatest value
    double step{};           ///< how far one step moves the value
    bool toggled{};          ///< a check box or a switch is on
    bool toggled_known{};    ///< `toggled` was set
    bool selected{};         ///< this is the chosen one of a group
    bool selected_known{};   ///< `selected` was set
    bool expanded{};         ///< a tree row is open
    bool expanded_known{};   ///< `expanded` was set
    bool disabled{};         ///< it is shown and grayed out; it stays in the tree
    bool modal{};            ///< a dialog covers what is behind it
    bool hidden{};           ///< covered or not shown; left out of AccessKit's tree
    bool focused{};          ///< it holds the keyboard focus, whether or not the ring is drawn
    /// A press steps the value one way. Increment is that press. A drop-down
    /// leaves this false.
    bool steps{};
    /// Its place in the screen's Tab order, from 1. 0 when that order is not known.
    int32_t tab_index{};
    int32_t place{};    ///< its place in a set, from 1; 0 when it has none
    int32_t set_size{}; ///< how many share the set; 0 when it has none
    LiveSetting live{LiveSetting::off};
    double x{};      ///< left edge, in window points
    double y{};      ///< top edge, in window points
    double width{};  ///< width, in window points
    double height{}; ///< height, in window points
    std::vector<uint64_t> children;
};

/// One action turned into the event the game already handles.
struct Event {
    EventKind kind{EventKind::press};
    Key key{Key::right};
    std::string text; ///< typed text, for `EventKind::text`
    double x{};       ///< the point, in window points
    double y{};
    uint64_t node{};    ///< the control the action named
    int32_t repeats{1}; ///< how many times a key is tapped; at most 64
    /// Backspaces sent before typed text, so a field's text is replaced.
    /// A password counts its masking characters, one per character it holds.
    int32_t clear{};
};

/// The ids AccessKit is given. Hidden controls and decoration are left out.
///
/// @param nodes the controls, including ones that are left out
/// @return the ids that are published, in the same order
[[nodiscard]] std::vector<uint64_t> kept_ids(const std::vector<Node>& nodes);

/// A fingerprint of the published tree and the focused id.
///
/// Two calls with the same published controls and the same focus return the
/// same value. A change of a published field, or of the focus, returns a
/// different one. Hidden controls and decoration do not change it.
///
/// @param nodes the controls
/// @param focus the focused id
/// @return the fingerprint
[[nodiscard]] uint64_t fingerprint(const std::vector<Node>& nodes, uint64_t focus);

/// Builds one AccessKit node. The caller frees it with `node_free` unless a
/// tree update has taken it.
///
/// @param node the control
/// @return the node, or null when the bridge is not ready or the role is left out
[[nodiscard]] accesskit_node* make_node(const Node& node);

/// Builds a tree update from the controls that are published.
///
/// The window node is the root. A focused id that is not published focuses
/// the window. The caller frees the update.
///
/// @param nodes the controls
/// @param focus the focused id
/// @return the update, or null when it cannot be made
[[nodiscard]] accesskit_tree_update* build_update(const std::vector<Node>& nodes, uint64_t focus);

/// The window AccessKit is shown before the game has published a tree.
///
/// @return the update, or null when it cannot be made
[[nodiscard]] accesskit_tree_update* placeholder_update();

/// Turns an action into the event the game already handles.
///
/// A disabled control, a hidden control and decoration do nothing. Click is a
/// press at the control's centre; a switch is pressed on the half it does not
/// show. Focus does nothing: the game cannot move its focus without a press.
/// A key or typed text reaches the game's focused control, so those are made
/// only when `node` is that control. Increment of a button whose press steps
/// its value one way is that press, and it does not need the focus. Decrement
/// is not that press. Increment and decrement of a slider are Right and
/// Left. Expand and collapse are Right and Left. Set value on a field
/// replaces its text: `clear` backspaces, then the new text. Set value on a
/// slider is as many Left or Right taps as the step needs, at most 64; a
/// step of 0 or less counts as 1, and a request for the value it already
/// holds does nothing.
///
/// @param node the control the action names
/// @param action what the reader asked
/// @param text the text of a set-value, when the reader sent one
/// @param number the number of a set-value, when the reader sent one
/// @return the event, or nothing when the control does not take that action
[[nodiscard]] std::optional<Event> event_for(
    const Node& node,
    Action action,
    const std::optional<std::string>& text,
    const std::optional<double>& number
);

/// Keeps the tree a reader action is matched against, and reports whether it
/// differs from the tree last sent.
///
/// The snapshot is replaced either way, so an action uses the latest tree.
/// The sent fingerprint changes only in `mark_tree_sent`.
///
/// @param nodes the controls
/// @param focus the focused id
/// @return true when this tree is not the one last marked sent
[[nodiscard]] bool stage_tree(std::vector<Node> nodes, uint64_t focus);

/// The fingerprint `stage_tree` last stored and has not yet marked sent.
///
/// @return that fingerprint; 0 when nothing has been staged
[[nodiscard]] uint64_t staged_fingerprint();

/// Records that the staged tree was given to AccessKit.
///
/// @param staged the fingerprint `stage_tree` returned alongside the change
void mark_tree_sent(uint64_t staged);

/// Forgets the tree last sent, so the next one is sent again.
void forget_sent_tree();

/// Builds the staged tree. The factory AccessKit calls.
///
/// @param userdata unused
/// @return the update; a placeholder when the staged tree cannot be made
[[nodiscard]] accesskit_tree_update* update_from_snapshot(void* userdata);

/// Reports whether `update_from_snapshot` has run since the flag was cleared.
///
/// @return true after the factory has built a tree
[[nodiscard]] bool snapshot_factory_ran();

/// Clears the factory flag, before an update that may or may not call it.
void clear_snapshot_factory();

/// Queues the event an action request becomes. The request stays the caller's
/// to free. The text is copied first.
///
/// @param request the request AccessKit handed over; null does nothing
void enqueue_request(const accesskit_action_request* request);

/// Takes the queued events. The queue is empty afterwards.
///
/// @return the events, in the order the actions arrived
[[nodiscard]] std::vector<Event> take_events();

/// Stores the tree and, when a reader has asked and the tree changed, asks
/// the adapter to send it. An unchanged tree sends nothing.
///
/// @param nodes the controls
/// @param focus the focused id
void submit_tree(std::vector<Node> nodes, uint64_t focus);

/// Marks the bridge as asked for by a reader, so a test can reach `submit_tree`'s
/// guard. The game does not call this. A test clears it before it returns.
///
/// @param on true while the test pretends a reader has asked for the tree
void testing_set_reader_active(bool on);

/// A stand-in for the adapter's update, so a test can see whether `submit_tree`
/// pushed. Null uses the adapter. The game does not call this. A test clears
/// it before it returns.
///
/// @param hook returns whether the update was sent; null restores the adapter
void testing_set_update_hook(bool (*hook)());

} // namespace oa::platform::screen_reader
