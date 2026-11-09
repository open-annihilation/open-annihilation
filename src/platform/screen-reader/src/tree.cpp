// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/screen_reader.hpp"

#if !defined(OA_SCREEN_READER_COMPILED_OUT)

#include "oa/platform/screen_reader/tree.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::platform::screen_reader {

namespace {

constexpr uint64_t kFingerprintOffset = 14695981039346656037ULL;
constexpr uint64_t kFingerprintPrime = 1099511628211ULL;
constexpr int32_t kMostTaps = 64;

std::mutex tree_mutex;
std::vector<Node> snapshot;
uint64_t snapshot_focus = window_node;
uint64_t sent_fingerprint = 0;
bool have_sent = false;
uint64_t staged = 0;
std::vector<Event> queued_events;
std::atomic<bool> factory_ran{false};

/// Folds bytes into a fingerprint.
///
/// @param[in,out] hash the fingerprint so far
/// @param data the bytes
/// @param size how many
void mix_bytes(uint64_t& hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t cursor = 0; cursor < size; ++cursor) {
        hash ^= bytes[cursor];
        hash *= kFingerprintPrime;
    }
}

/// Folds one byte into a fingerprint.
///
/// @param[in,out] hash the fingerprint so far
/// @param byte the byte
void mix_byte(uint64_t& hash, unsigned char byte) {
    mix_bytes(hash, &byte, 1);
}

/// Folds a string into a fingerprint, then a separator.
///
/// @param[in,out] hash the fingerprint so far
/// @param text the string
void mix_text(uint64_t& hash, const std::string& text) {
    mix_bytes(hash, text.data(), text.size());
    mix_byte(hash, 0);
}

/// Reports whether the control is left out of the published tree.
///
/// @param node the control
/// @return true when it is hidden or decoration
bool omitted(const Node& node) {
    return node.hidden || node.role == Role::none;
}

/// The AccessKit role of a published control.
///
/// @param role the control's role
/// @return the AccessKit role; label for a role that is left out
accesskit_role accesskit_role_of(Role role) {
    switch (role) {
    case Role::button:
        return ACCESSKIT_ROLE_BUTTON;
    case Role::check_box:
        return ACCESSKIT_ROLE_CHECK_BOX;
    case Role::switch_control:
        return ACCESSKIT_ROLE_SWITCH;
    case Role::radio_group:
        return ACCESSKIT_ROLE_RADIO_GROUP;
    case Role::radio:
        return ACCESSKIT_ROLE_RADIO_BUTTON;
    case Role::tab_list:
        return ACCESSKIT_ROLE_TAB_LIST;
    case Role::tab:
        return ACCESSKIT_ROLE_TAB;
    case Role::list:
        return ACCESSKIT_ROLE_LIST;
    case Role::list_item:
        return ACCESSKIT_ROLE_LIST_ITEM;
    case Role::tree:
        return ACCESSKIT_ROLE_TREE;
    case Role::tree_item:
        return ACCESSKIT_ROLE_TREE_ITEM;
    case Role::text_field:
        return ACCESSKIT_ROLE_TEXT_INPUT;
    case Role::password:
        return ACCESSKIT_ROLE_PASSWORD_INPUT;
    case Role::search:
        return ACCESSKIT_ROLE_SEARCH_INPUT;
    case Role::slider:
        return ACCESSKIT_ROLE_SLIDER;
    case Role::log:
        return ACCESSKIT_ROLE_LOG;
    case Role::dialog:
        return ACCESSKIT_ROLE_DIALOG;
    case Role::alert_dialog:
        return ACCESSKIT_ROLE_ALERT_DIALOG;
    case Role::group:
        return ACCESSKIT_ROLE_GROUP;
    case Role::image:
        return ACCESSKIT_ROLE_IMAGE;
    case Role::link:
        return ACCESSKIT_ROLE_LINK;
    case Role::label:
        return ACCESSKIT_ROLE_LABEL;
    case Role::window:
        return ACCESSKIT_ROLE_WINDOW;
    case Role::none:
        return ACCESSKIT_ROLE_LABEL;
    }
    return ACCESSKIT_ROLE_LABEL;
}

/// Reports whether the control takes an action.
///
/// Focus is never taken. The game has no way to move its focus to a
/// control without pressing it, and a press changes a radio, a tab, a row or
/// a slider, so a reader that moves the focus as its cursor moves would
/// change settings.
///
/// @param node the control
/// @param action the action
/// @return true when the control takes it
bool supports(const Node& node, Action action) {
    if (node.disabled || omitted(node))
        return false;
    switch (node.role) {
    case Role::button:
        if (action == Action::click)
            return true;
        // Increment is the press that steps a value one way. A drop-down does
        // not step, and decrement is not that press.
        return action == Action::increment && node.steps;
    case Role::check_box:
    case Role::switch_control:
    case Role::radio:
    case Role::tab:
    case Role::list_item:
    case Role::link:
        return action == Action::click;
    case Role::tree_item:
        return action == Action::click || action == Action::expand || action == Action::collapse;
    case Role::slider:
        return action == Action::increment || action == Action::decrement ||
               action == Action::set_value;
    case Role::text_field:
    case Role::password:
    case Role::search:
        return action == Action::set_value || action == Action::click;
    case Role::image:
    case Role::list:
    case Role::tree:
    case Role::radio_group:
    case Role::tab_list:
    case Role::none:
    case Role::log:
    case Role::dialog:
    case Role::alert_dialog:
    case Role::group:
    case Role::label:
    case Role::window:
        return false;
    }
    return false;
}

/// Reports whether a role takes input, so that it can be greyed out. Words,
/// pictures and groups are never disabled.
///
/// @param role the control's role
/// @return true when the role takes a press, a key or text
bool takes_input(Role role) {
    switch (role) {
    case Role::button:
    case Role::check_box:
    case Role::switch_control:
    case Role::radio:
    case Role::tab:
    case Role::list_item:
    case Role::tree_item:
    case Role::text_field:
    case Role::password:
    case Role::search:
    case Role::slider:
    case Role::link:
        return true;
    default:
        return false;
    }
}

/// The point at the centre of a control.
///
/// @param node the control
/// @param[out] event the event that gains the point
void put_centre(const Node& node, Event& event) {
    event.x = node.x + node.width / 2.0;
    event.y = node.y + node.height / 2.0;
    event.node = node.id;
}

/// How many Unicode code points a value holds. A masked field's value is one
/// masking character per character, so this is that length.
///
/// @param text the value
/// @return the number of code points
int32_t code_points(std::string_view text) {
    int32_t count = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        std::size_t width = 1;
        if ((byte & 0xe0U) == 0xc0U)
            width = 2;
        else if ((byte & 0xf0U) == 0xe0U)
            width = 3;
        else if ((byte & 0xf8U) == 0xf0U)
            width = 4;
        if (index + width > text.size())
            width = 1;
        index += width;
        ++count;
    }
    return count;
}

/// Reports whether a press steps this control, so the reader can move it
/// without the game's focus.
///
/// @param node the control
/// @param action the action
/// @return true for increment of a button whose press steps its value one way
bool steps_on_press(const Node& node, Action action) {
    return node.role == Role::button && node.steps && action == Action::increment;
}

/// How many key taps move a slider from its value to a requested number.
///
/// @param node the slider
/// @param requested the number the reader asked for
/// @param[out] key Left when the number is lower, otherwise Right
/// @param[out] taps how many taps, from 1 to 64
/// @return false when the slider is already at that number
bool slider_taps(const Node& node, double requested, Key& key, int32_t& taps) {
    const double step = node.step > 0.0 ? node.step : 1.0;
    const double delta = requested - node.value;
    if (delta == 0.0)
        return false;
    key = delta > 0.0 ? Key::right : Key::left;
    const double raw = std::fabs(delta) / step;
    if (raw > static_cast<double>(kMostTaps)) {
        taps = kMostTaps;
        return true;
    }
    taps = static_cast<int32_t>(std::llround(raw));
    if (taps < 1)
        taps = 1;
    return true;
}

/// Copies a set-value's text out of a request.
///
/// @param data the request's data
/// @param[out] text the text, set when the data holds one
/// @param[out] number the number, set when the data holds one
void read_action_data(
    const accesskit_opt_action_data& data,
    std::optional<std::string>& text,
    std::optional<double>& number
) {
    if (!data.has_value)
        return;
    if (data.value.tag == ACCESSKIT_ACTION_DATA_VALUE) {
        if (data.value.value != nullptr)
            text = std::string(data.value.value);
        return;
    }
    if (data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE)
        number = data.value.numeric_value;
}

/// The action a request names, when the game handles it.
///
/// @param action the AccessKit action
/// @return the action, or nothing for one the game does not take
std::optional<Action> action_of(accesskit_action action) {
    switch (action) {
    case ACCESSKIT_ACTION_CLICK:
        return Action::click;
    case ACCESSKIT_ACTION_FOCUS:
        return Action::focus;
    case ACCESSKIT_ACTION_INCREMENT:
        return Action::increment;
    case ACCESSKIT_ACTION_DECREMENT:
        return Action::decrement;
    case ACCESSKIT_ACTION_SET_VALUE:
        return Action::set_value;
    case ACCESSKIT_ACTION_EXPAND:
        return Action::expand;
    case ACCESSKIT_ACTION_COLLAPSE:
        return Action::collapse;
    default:
        return std::nullopt;
    }
}

/// Adds the actions a control takes.
///
/// @param table the bound C functions
/// @param made the AccessKit node
/// @param node the control
void add_actions(const Calls& table, accesskit_node* made, const Node& node) {
    const Action offered[] = {
        Action::click,
        Action::focus,
        Action::increment,
        Action::decrement,
        Action::set_value,
        Action::expand,
        Action::collapse,
    };
    for (const Action action : offered) {
        if (!supports(node, action))
            continue;
        accesskit_action named = ACCESSKIT_ACTION_CLICK;
        switch (action) {
        case Action::click:
            named = ACCESSKIT_ACTION_CLICK;
            break;
        case Action::focus:
            named = ACCESSKIT_ACTION_FOCUS;
            break;
        case Action::increment:
            named = ACCESSKIT_ACTION_INCREMENT;
            break;
        case Action::decrement:
            named = ACCESSKIT_ACTION_DECREMENT;
            break;
        case Action::set_value:
            named = ACCESSKIT_ACTION_SET_VALUE;
            break;
        case Action::expand:
            named = ACCESSKIT_ACTION_EXPAND;
            break;
        case Action::collapse:
            named = ACCESSKIT_ACTION_COLLAPSE;
            break;
        }
        table.node_add_action(made, named);
    }
}

/// Fills one AccessKit node from a control. The node already has its role.
///
/// @param table the bound C functions
/// @param made the AccessKit node
/// @param node the control
void fill_node(const Calls& table, accesskit_node* made, const Node& node) {
    if (!node.label.empty())
        table.node_set_label(made, node.label.c_str());
    if (!node.value_text.empty())
        table.node_set_value(made, node.value_text.c_str());
    else if (node.role == Role::label && !node.label.empty())
        // AccessKit reads a label's words from its value: Windows names a
        // label from it and from nothing else.
        table.node_set_value(made, node.label.c_str());
    if (!node.description.empty())
        table.node_set_description(made, node.description.c_str());
    if (!node.shortcut.empty())
        table.node_set_keyboard_shortcut(made, node.shortcut.c_str());
    if (node.has_value)
        table.node_set_numeric_value(made, node.value);
    if (node.has_range) {
        table.node_set_min_numeric_value(made, node.least);
        table.node_set_max_numeric_value(made, node.most);
        table.node_set_numeric_value_step(made, node.step);
    }
    if (node.toggled_known) {
        table.node_set_toggled(
            made, node.toggled ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE
        );
    }
    if (node.selected_known)
        table.node_set_selected(made, node.selected);
    if (node.expanded_known)
        table.node_set_expanded(made, node.expanded);
    if (node.disabled && takes_input(node.role))
        table.node_set_disabled(made);
    if (node.modal)
        table.node_set_modal(made);
    // AccessKit counts a place in a set from 0, where the tree counts from 1.
    if (node.place > 0)
        table.node_set_position_in_set(made, static_cast<size_t>(node.place - 1));
    if (node.set_size > 0)
        table.node_set_size_of_set(made, static_cast<size_t>(node.set_size));
    if (node.live != LiveSetting::off) {
        const accesskit_live live =
            node.live == LiveSetting::assertive ? ACCESSKIT_LIVE_ASSERTIVE : ACCESSKIT_LIVE_POLITE;
        table.node_set_live(made, live);
    }
    const accesskit_rect bounds{node.x, node.y, node.x + node.width, node.y + node.height};
    table.node_set_bounds(made, bounds);
    for (const uint64_t child : node.children)
        table.node_push_child(made, child);
    add_actions(table, made, node);
}

/// Folds one published control into a fingerprint.
///
/// @param[in,out] hash the fingerprint so far
/// @param node the control
void mix_node(uint64_t& hash, const Node& node) {
    mix_bytes(hash, &node.id, sizeof node.id);
    const auto role = static_cast<uint8_t>(node.role);
    mix_byte(hash, role);
    mix_text(hash, node.label);
    mix_text(hash, node.value_text);
    mix_text(hash, node.description);
    mix_text(hash, node.shortcut);
    mix_byte(hash, static_cast<unsigned char>(node.has_value));
    mix_bytes(hash, &node.value, sizeof node.value);
    mix_byte(hash, static_cast<unsigned char>(node.has_range));
    mix_bytes(hash, &node.least, sizeof node.least);
    mix_bytes(hash, &node.most, sizeof node.most);
    mix_bytes(hash, &node.step, sizeof node.step);
    mix_byte(hash, static_cast<unsigned char>(node.toggled));
    mix_byte(hash, static_cast<unsigned char>(node.toggled_known));
    mix_byte(hash, static_cast<unsigned char>(node.selected));
    mix_byte(hash, static_cast<unsigned char>(node.selected_known));
    mix_byte(hash, static_cast<unsigned char>(node.expanded));
    mix_byte(hash, static_cast<unsigned char>(node.expanded_known));
    mix_byte(hash, static_cast<unsigned char>(node.disabled));
    mix_byte(hash, static_cast<unsigned char>(node.modal));
    mix_byte(hash, static_cast<unsigned char>(node.focused));
    mix_byte(hash, static_cast<unsigned char>(node.steps));
    mix_bytes(hash, &node.place, sizeof node.place);
    mix_bytes(hash, &node.set_size, sizeof node.set_size);
    mix_byte(hash, static_cast<unsigned char>(node.live));
    mix_bytes(hash, &node.x, sizeof node.x);
    mix_bytes(hash, &node.y, sizeof node.y);
    mix_bytes(hash, &node.width, sizeof node.width);
    mix_bytes(hash, &node.height, sizeof node.height);
    for (const uint64_t child : node.children)
        mix_bytes(hash, &child, sizeof child);
    mix_byte(hash, 0xff);
}

} // namespace

std::vector<uint64_t> kept_ids(const std::vector<Node>& nodes) {
    std::vector<uint64_t> ids;
    ids.reserve(nodes.size());
    for (const Node& node : nodes) {
        if (!omitted(node))
            ids.push_back(node.id);
    }
    return ids;
}

uint64_t fingerprint(const std::vector<Node>& nodes, uint64_t focus) {
    uint64_t hash = kFingerprintOffset;
    for (const Node& node : nodes) {
        if (!omitted(node))
            mix_node(hash, node);
    }
    mix_bytes(hash, &focus, sizeof focus);
    return hash;
}

accesskit_node* make_node(const Node& node) {
    const Calls* table = calls();
    if (table == nullptr || omitted(node))
        return nullptr;
    accesskit_node* made = table->node_new(accesskit_role_of(node.role));
    if (made == nullptr)
        return nullptr;
    fill_node(*table, made, node);
    return made;
}

accesskit_tree_update* build_update(const std::vector<Node>& nodes, uint64_t focus) {
    const Calls* table = calls();
    if (table == nullptr)
        return nullptr;
    std::vector<const Node*> kept;
    kept.reserve(nodes.size());
    bool focus_kept = false;
    bool window_kept = false;
    for (const Node& node : nodes) {
        if (omitted(node))
            continue;
        kept.push_back(&node);
        if (node.id == focus)
            focus_kept = true;
        if (node.id == window_node)
            window_kept = true;
    }
    const uint64_t focused = focus_kept ? focus : window_node;
    const size_t count = kept.size() + (window_kept ? 0 : 1);
    accesskit_tree_update* update = table->tree_update_with_capacity_and_focus(count, focused);
    if (update == nullptr)
        return nullptr;
    accesskit_tree_info* info = table->tree_info_new(window_node);
    if (info != nullptr)
        table->tree_update_set_tree_info(update, info);
    if (!window_kept) {
        Node root;
        root.id = window_node;
        root.role = Role::window;
        root.label = "Open Annihilation";
        for (const Node* node : kept) {
            bool nested = false;
            for (const Node* other : kept) {
                for (const uint64_t child : other->children) {
                    if (child == node->id)
                        nested = true;
                }
            }
            if (!nested)
                root.children.push_back(node->id);
        }
        accesskit_node* made = make_node(root);
        if (made != nullptr)
            table->tree_update_push_node(update, window_node, made);
    }
    for (const Node* node : kept) {
        accesskit_node* made = make_node(*node);
        if (made != nullptr)
            table->tree_update_push_node(update, node->id, made);
    }
    return update;
}

accesskit_tree_update* placeholder_update() {
    Node root;
    root.id = window_node;
    root.role = Role::window;
    root.label = "Open Annihilation";
    return build_update(std::vector<Node>{std::move(root)}, window_node);
}

std::optional<Event> event_for(
    const Node& node,
    Action action,
    const std::optional<std::string>& text,
    const std::optional<double>& number
) {
    if (!supports(node, action))
        return std::nullopt;
    // A key or typed text reaches the control that holds the game's focus,
    // whatever the reader named. Send it only when they are the same control.
    // A button whose press steps its value one way is the exception: that
    // press is increment, so the reader can move it without that focus.
    if (action != Action::click && !steps_on_press(node, action) && !node.focused)
        return std::nullopt;
    Event event;
    event.node = node.id;
    switch (action) {
    case Action::click:
        event.kind = EventKind::press;
        put_centre(node, event);
        // A switch's middle is the edge between its Off and On halves, which
        // is the half it already shows. Press the other half.
        if (node.role == Role::switch_control && node.toggled_known)
            event.x = node.x + node.width * (node.toggled ? 0.25 : 0.75);
        return event;
    case Action::focus:
        return std::nullopt;
    case Action::increment:
    case Action::decrement:
        if (steps_on_press(node, action)) {
            event.kind = EventKind::press;
            put_centre(node, event);
            return event;
        }
        event.kind = EventKind::key;
        event.key = action == Action::increment ? Key::right : Key::left;
        return event;
    case Action::expand:
        event.kind = EventKind::key;
        event.key = Key::right;
        return event;
    case Action::collapse:
        event.kind = EventKind::key;
        event.key = Key::left;
        return event;
    case Action::set_value:
        if (node.role == Role::slider) {
            if (!number.has_value())
                return std::nullopt;
            Key key = Key::right;
            int32_t taps = 1;
            if (!slider_taps(node, *number, key, taps))
                return std::nullopt;
            event.kind = EventKind::key;
            event.key = key;
            event.repeats = taps;
            return event;
        }
        if (!text.has_value())
            return std::nullopt;
        event.kind = EventKind::text;
        // Backspaces for the characters it holds, then the new text, so the
        // field is replaced. A password's value is one masking character each.
        event.clear = code_points(node.value_text);
        event.text = *text;
        return event;
    }
    return std::nullopt;
}

bool stage_tree(std::vector<Node> nodes, uint64_t focus) {
    const uint64_t next = fingerprint(nodes, focus);
    const std::lock_guard<std::mutex> lock(tree_mutex);
    snapshot = std::move(nodes);
    snapshot_focus = focus;
    staged = next;
    return !have_sent || next != sent_fingerprint;
}

uint64_t staged_fingerprint() {
    const std::lock_guard<std::mutex> lock(tree_mutex);
    return staged;
}

void mark_tree_sent(uint64_t marked) {
    const std::lock_guard<std::mutex> lock(tree_mutex);
    sent_fingerprint = marked;
    have_sent = true;
}

void forget_sent_tree() {
    const std::lock_guard<std::mutex> lock(tree_mutex);
    have_sent = false;
    sent_fingerprint = 0;
    snapshot.clear();
    snapshot_focus = window_node;
    staged = 0;
    queued_events.clear();
}

accesskit_tree_update* update_from_snapshot(void* /*userdata*/) {
    std::vector<Node> nodes;
    uint64_t focus = window_node;
    {
        const std::lock_guard<std::mutex> lock(tree_mutex);
        nodes = snapshot;
        focus = snapshot_focus;
    }
    factory_ran.store(true, std::memory_order_release);
    accesskit_tree_update* update = build_update(nodes, focus);
    if (update == nullptr)
        return placeholder_update();
    return update;
}

bool snapshot_factory_ran() {
    return factory_ran.load(std::memory_order_acquire);
}

void clear_snapshot_factory() {
    factory_ran.store(false, std::memory_order_release);
}

void enqueue_request(const accesskit_action_request* request) {
    if (request == nullptr)
        return;
    const std::optional<Action> action = action_of(request->action);
    if (!action.has_value())
        return;
    std::optional<std::string> text;
    std::optional<double> number;
    read_action_data(request->data, text, number);
    Node node;
    bool found = false;
    {
        const std::lock_guard<std::mutex> lock(tree_mutex);
        for (const Node& candidate : snapshot) {
            if (candidate.id == request->target_node) {
                node = candidate;
                found = true;
                break;
            }
        }
    }
    if (!found)
        return;
    const std::optional<Event> event = event_for(node, *action, text, number);
    if (!event.has_value())
        return;
    const std::lock_guard<std::mutex> lock(tree_mutex);
    queued_events.push_back(*event);
}

std::vector<Event> take_events() {
    const std::lock_guard<std::mutex> lock(tree_mutex);
    std::vector<Event> taken;
    taken.swap(queued_events);
    return taken;
}

} // namespace oa::platform::screen_reader

#endif
