// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Desktop: top-level window list, focus/capture/hover tracking and input
// routing.
#include "list.hpp"
#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards {
namespace {

/// Tests whether a message is a pointer move.
///
/// @param msg message to test; may be null
/// @return true for MessageCode::mouse_move
bool is_mouse_move(const Message* msg) {
    return msg != nullptr && msg->code == MessageCode::mouse_move;
}

/// Tests whether a message is a button message, one of the codes between the
/// move and the wheel (0x201-0x209).
///
/// @param msg message to test; may be null
/// @return true for a press, release or double click of any button
bool is_pointer_button(const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    const auto value = static_cast<int32_t>(msg->code);
    return value > static_cast<int32_t>(MessageCode::mouse_move) &&
           value < static_cast<int32_t>(MessageCode::wheel);
}

/// Finds the deepest shown window under a point across all top-level
/// windows; later (higher) windows win.
///
/// @param desktop desktop to search
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
/// @return the window found by window_hit_test, or null
Window* deepest_window_at(Desktop* desktop, int32_t x, int32_t y) {
    Window* hit = nullptr;
    for (int32_t i = 0; i < desktop->windows.count; ++i) {
        Window* window = desktop->windows.items[i];
        if (window == nullptr) {
            continue;
        }
        Window* found = window_hit_test(window, x, y);
        if (found != nullptr) {
            hit = found;
        }
    }
    return hit;
}

/// Destroys the owned descendants of a window, depth first.
///
/// Owned children are destroyed one at a time, rescanning after each; the
/// remaining children are then searched in turn.
///
/// @param[in,out] window window whose subtree is cleaned up
void destroy_owned_children(Window* window) {
    bool removed = true;
    while (removed) {
        removed = false;
        for (int32_t i = 0; i < window->children.count; ++i) {
            Window* child = window->children.items[i];
            if (child != nullptr && child->owned != 0) {
                window_destroy(child);
                removed = true;
                break;
            }
        }
    }
    for (int32_t i = 0; i < window->children.count; ++i) {
        if (window->children.items[i] != nullptr) {
            destroy_owned_children(window->children.items[i]);
        }
    }
}

bool tree_holds(const Window* root, const Window* window) {
    if (root == window)
        return true;
    for (int32_t i = 0; i < root->children.count; ++i) {
        const Window* child = root->children.items[i];
        if (child != nullptr && tree_holds(child, window))
            return true;
    }
    return false;
}

// Direction keys move between controls. Reading order is top to bottom, then
// left to right. A vertical move stays in a column: controls whose left edges
// are within ten pixels share one.
constexpr int32_t k_focus_stride = 5000;
constexpr int32_t k_focus_wrap = 25000000;
constexpr int32_t k_column_slack = 10;
constexpr int32_t k_focus_cap = 1024;

enum class FocusDirection : int32_t { left, right, up, down };

bool is_class_name(const Window* window, const char* name) {
    return window != nullptr && window->ops != nullptr && window->ops->is_class != nullptr &&
           window->ops->is_class(name);
}

/// A control the direction keys can land on.
///
/// Buttons, checks, radios, stage buttons, text fields, lists and sliders.
/// Disabled ones (their first state) and controls that do not take focus are
/// skipped. Scroll buttons and buttons that refuse focus are not stops.
bool is_focus_stop(const Window* window) {
    if (window == nullptr || window->focusable == 0 || !window_is_shown(window))
        return false;
    const bool button = is_class_name(window, class_name::button);
    const bool check = is_class_name(window, class_name::check_box);
    const bool radio = is_class_name(window, class_name::radio_button);
    const bool stage = is_class_name(window, class_name::stage_button);
    const bool edit = is_class_name(window, class_name::single_edit);
    const bool box = is_class_name(window, class_name::edit_box);
    const bool list = is_class_name(window, class_name::list_box);
    const bool slider = is_class_name(window, class_name::slider);
    if (!button && !check && !radio && !stage && !edit && !box && !list && !slider)
        return false;
    // State 0 is the disabled face of buttons, checks, radios, stage buttons,
    // sliders and single-line edits. Lists and edit boxes have no such face.
    if ((button || check || radio || stage || slider || edit) && window->states.current == 0)
        return false;
    return true;
}

bool shown_dialog(const Window* window) {
    return window != nullptr && window_is_shown(window) &&
           is_class_name(window, class_name::dialog);
}

bool has_shown_dialog(const Window* window) {
    for (int32_t i = 0; i < window->children.count; ++i) {
        const Window* child = window->children.items[i];
        if (child == nullptr)
            continue;
        if (shown_dialog(child) || has_shown_dialog(child))
            return true;
    }
    return false;
}

/// The topmost shown dialog that does not itself contain a shown dialog.
///
/// Later windows are in front. The direction keys stay inside it, so a
/// message box keeps the pad on its own buttons.
void note_dialog(Window* window, Window** found) {
    if (window == nullptr || !window_is_shown(window))
        return;
    if (shown_dialog(window) && !has_shown_dialog(window))
        *found = window;
    for (int32_t i = 0; i < window->children.count; ++i)
        note_dialog(window->children.items[i], found);
}

Window* top_dialog(Desktop* desktop) {
    Window* found = nullptr;
    for (int32_t i = 0; i < desktop->windows.count; ++i)
        note_dialog(desktop->windows.items[i], &found);
    return found;
}

void collect_stops(Window* window, Window** out, int32_t* count) {
    if (window == nullptr || !window_is_shown(window) || *count >= k_focus_cap)
        return;
    if (is_focus_stop(window))
        out[(*count)++] = window;
    for (int32_t i = 0; i < window->children.count && *count < k_focus_cap; ++i)
        collect_stops(window->children.items[i], out, count);
}

int32_t collect_candidates(Desktop* desktop, Window** out) {
    int32_t count = 0;
    Window* dialog = top_dialog(desktop);
    if (dialog != nullptr) {
        for (int32_t i = 0; i < dialog->children.count && count < k_focus_cap; ++i)
            collect_stops(dialog->children.items[i], out, &count);
        return count;
    }
    for (int32_t i = 0; i < desktop->windows.count && count < k_focus_cap; ++i)
        collect_stops(desktop->windows.items[i], out, &count);
    return count;
}

bool direction_of(int32_t key, FocusDirection* direction) {
    switch (key) {
    case key::left:
        *direction = FocusDirection::left;
        return true;
    case key::right:
        *direction = FocusDirection::right;
        return true;
    case key::up:
        *direction = FocusDirection::up;
        return true;
    case key::down:
        *direction = FocusDirection::down;
        return true;
    default:
        return false;
    }
}

int32_t reading_score(const Window* window) {
    return window->y * k_focus_stride + window->x;
}

Window* first_in_reading_order(Window** items, int32_t count) {
    Window* best = items[0];
    int32_t best_score = reading_score(best);
    for (int32_t i = 1; i < count; ++i) {
        const int32_t score = reading_score(items[i]);
        if (score < best_score) {
            best = items[i];
            best_score = score;
        }
    }
    return best;
}

// Columns are grouped from left to right, so raising a control (which
// reorders the window tree) does not move it into a different column.
void snap_columns(Window** items, int32_t count, int32_t* columns) {
    int32_t order[k_focus_cap];
    for (int32_t i = 0; i < count; ++i)
        order[i] = i;
    for (int32_t i = 1; i < count; ++i) {
        const int32_t key = order[i];
        int32_t slot = i;
        while (slot > 0) {
            const Window* earlier = items[order[slot - 1]];
            const Window* current = items[key];
            const bool in_place =
                earlier->x < current->x || (earlier->x == current->x && earlier->y <= current->y);
            if (in_place)
                break;
            order[slot] = order[slot - 1];
            --slot;
        }
        order[slot] = key;
    }
    for (int32_t n = 0; n < count; ++n) {
        const int32_t index = order[n];
        const int32_t x = items[index]->x;
        int32_t snapped = x;
        for (int32_t prior = 0; prior < n; ++prior) {
            const int32_t delta = columns[order[prior]] - x;
            if (delta < k_column_slack && delta > -k_column_slack) {
                snapped = columns[order[prior]];
                break;
            }
        }
        columns[index] = snapped;
    }
}

int32_t column_of(const Window* from, Window** items, const int32_t* columns, int32_t count) {
    for (int32_t i = 0; i < count; ++i) {
        if (items[i] == from)
            return columns[i];
    }
    for (int32_t i = 0; i < count; ++i) {
        const int32_t delta = columns[i] - from->x;
        if (delta < k_column_slack && delta > -k_column_slack)
            return columns[i];
    }
    return from->x;
}

/// The next control in `direction`, wrapping past the last back to the first.
///
/// Left and right order controls by their place on the screen. Up and down
/// order a column. The focused control stays put when it is the only one.
Window* nearest_stop(
    Window** items,
    const int32_t* columns,
    int32_t count,
    const Window* from,
    FocusDirection direction
) {
    const bool horizontal = direction == FocusDirection::left || direction == FocusDirection::right;
    const bool backward = direction == FocusDirection::left || direction == FocusDirection::up;
    const int32_t origin = horizontal
                               ? reading_score(from)
                               : column_of(from, items, columns, count) * k_focus_stride + from->y;
    int32_t best = backward ? origin - k_focus_wrap : origin + k_focus_wrap;
    Window* chosen = nullptr;
    for (int32_t i = 0; i < count; ++i) {
        int32_t score =
            horizontal ? reading_score(items[i]) : columns[i] * k_focus_stride + items[i]->y;
        if (backward) {
            if (score >= origin)
                score -= k_focus_wrap;
            if (chosen == nullptr || score > best) {
                chosen = items[i];
                best = score;
            }
        } else if (score <= origin) {
            score += k_focus_wrap;
            if (chosen == nullptr || score < best) {
                chosen = items[i];
                best = score;
            }
        } else if (chosen == nullptr || score < best) {
            chosen = items[i];
            best = score;
        }
    }
    return chosen;
}

void notify_focus(Desktop* desktop, Window* window) {
    if (desktop->on_focus == nullptr || desktop->focus_notify != 0)
        return;
    desktop->focus_notify = 1;
    desktop->on_focus(desktop, window, desktop->focus_user);
    desktop->focus_notify = 0;
}

/// Moves the focus for a direction key the focused control did not use.
///
/// With no focus, a direction key or Enter or Space lands on the first
/// control and does not press it. Returns false when nothing can take the
/// focus, so the key can pass on.
bool move_focus(Desktop* desktop, const Message* msg) {
    FocusDirection direction = FocusDirection::down;
    const bool arrow = direction_of(msg->key, &direction);
    const bool bare = desktop->focus == nullptr;
    const bool activate = msg->key == key::enter || msg->key == ' ';
    if (!arrow && !(bare && activate))
        return false;
    Window* items[k_focus_cap];
    const int32_t count = collect_candidates(desktop, items);
    if (count == 0)
        return false;
    Window* target = nullptr;
    if (!arrow) {
        target = first_in_reading_order(items, count);
    } else if (bare) {
        target = first_in_reading_order(items, count);
    } else {
        int32_t columns[k_focus_cap];
        snap_columns(items, count, columns);
        target = nearest_stop(items, columns, count, desktop->focus, direction);
    }
    if (target == nullptr)
        return false;
    if (target != desktop->focus)
        desktop_set_focus(desktop, target);
    return true;
}

} // namespace

void desktop_init(Desktop* desktop, int32_t x, int32_t y, int32_t width, int32_t height) {
    *desktop = Desktop{};
    desktop->x = x;
    desktop->y = y;
    desktop->width = width;
    desktop->height = height;
}

void desktop_destroy_owned(Desktop* desktop) {
    bool removed = true;
    while (removed) {
        removed = false;
        for (int32_t i = 0; i < desktop->windows.count; ++i) {
            Window* window = desktop->windows.items[i];
            if (window != nullptr && window->owned != 0) {
                window_destroy(window);
                removed = true;
                break;
            }
        }
    }
    for (int32_t i = 0; i < desktop->windows.count; ++i) {
        if (desktop->windows.items[i] != nullptr) {
            destroy_owned_children(desktop->windows.items[i]);
        }
    }
}

bool desktop_holds(const Desktop* desktop, const Window* window) {
    if (desktop == nullptr || window == nullptr)
        return false;
    for (int32_t i = 0; i < desktop->windows.count; ++i) {
        const Window* top = desktop->windows.items[i];
        if (top != nullptr && tree_holds(top, window))
            return true;
    }
    return false;
}

void desktop_release(Desktop* desktop) {
    if (desktop->focus != nullptr) {
        desktop->focus = nullptr;
        notify_focus(desktop, nullptr);
    }
    list_free(&desktop->windows);
    desktop->capture = nullptr;
    desktop->hover = nullptr;
}

void desktop_add(Desktop* desktop, Window* window) {
    list_add_unique(&desktop->windows, window);
}

void desktop_remove(Desktop* desktop, Window* window) {
    list_remove(&desktop->windows, window);
}

void desktop_raise(Desktop* desktop, Window* window) {
    if (list_remove(&desktop->windows, window)) {
        list_add_unique(&desktop->windows, window);
    }
}

void desktop_set_focus(Desktop* desktop, Window* window) {
    Window* old = desktop->focus;
    if (window == old) {
        return;
    }
    if (old != nullptr) {
        window_send(old, MessageCode::focus_lost);
    }
    desktop->focus = window;
    if (window != nullptr) {
        window_send(window, MessageCode::focus_gained);
    }
    notify_focus(desktop, window);
}

void desktop_set_capture(Desktop* desktop, Window* window) {
    desktop->capture = window;
}

bool desktop_route(Desktop* desktop, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (msg->code == MessageCode::key_down || msg->code == MessageCode::key_up) {
        if (desktop->focus != nullptr && window_dispatch(desktop->focus, msg))
            return true;
        if (msg->code != MessageCode::key_down) {
            FocusDirection direction = FocusDirection::down;
            if (!direction_of(msg->key, &direction))
                return false;
            Window* items[k_focus_cap];
            return collect_candidates(desktop, items) > 0;
        }
        return move_focus(desktop, msg);
    }
    if (desktop->capture != nullptr && window_dispatch(desktop->capture, msg)) {
        return true;
    }
    if (is_mouse_move(msg)) {
        Window* target = deepest_window_at(desktop, msg->x, msg->y);
        Window* hover = desktop->hover;
        if (hover != target && hover != nullptr) {
            hover->ops->mouse_leave(hover);
            desktop->hover = nullptr;
        }
        if (target != nullptr && window_dispatch(target, msg)) {
            if (desktop->hover != target) {
                target->ops->mouse_enter(target);
                desktop->hover = target;
            }
            return true;
        }
        return false;
    }
    if (!is_pointer_button(msg)) {
        return false;
    }
    Window* target = desktop_window_at(desktop, msg->x, msg->y);
    if (target == nullptr) {
        return false;
    }
    if (target == desktop->focus) {
        return window_dispatch(target, msg);
    }
    Window* focus = target->ops->focus_target(target, nullptr, false);
    if (focus != nullptr && focus != desktop->focus) {
        desktop_set_focus(desktop, focus);
        if (focus == target) {
            return window_dispatch(focus, msg);
        }
    }
    return window_dispatch(target, msg);
}

bool desktop_has_windows(const Desktop* desktop) {
    return desktop->windows.count != 0;
}

void desktop_draw(Desktop* desktop) {
    Message msg{};
    msg.code = MessageCode::draw;
    for (int32_t i = 0; i < desktop->windows.count; ++i) {
        if (desktop->windows.items[i] != nullptr) {
            window_dispatch(desktop->windows.items[i], &msg);
        }
    }
}

void desktop_pulse(Desktop* desktop) {
    for (int32_t i = 0; i < desktop->windows.count; ++i) {
        if (desktop->windows.items[i] != nullptr) {
            window_pulse_tree(desktop->windows.items[i]);
        }
    }
}

void desktop_tick(Desktop* desktop) {
    for (int32_t i = 0; i < desktop->windows.count; ++i) {
        Window* window = desktop->windows.items[i];
        if (window != nullptr) {
            window->ops->tick(window);
        }
    }
}

int32_t services_desktop_x() {
    const Desktop* desktop = services().desktop;
    return desktop != nullptr ? desktop->x : 0;
}

int32_t services_desktop_y() {
    const Desktop* desktop = services().desktop;
    return desktop != nullptr ? desktop->y : 0;
}

int32_t services_desktop_width() {
    const Desktop* desktop = services().desktop;
    return desktop != nullptr ? desktop->width : 0;
}

int32_t services_desktop_height() {
    const Desktop* desktop = services().desktop;
    return desktop != nullptr ? desktop->height : 0;
}

void services_desktop_add(Window* window) {
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_add(desktop, window);
}

void services_desktop_remove(Window* window) {
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_remove(desktop, window);
}

void services_desktop_raise(Window* window) {
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_raise(desktop, window);
}

void services_desktop_destroy_owned() {
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_destroy_owned(desktop);
}

void services_desktop_focus(Window* window) {
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_set_focus(desktop, window);
}

void services_desktop_capture(Window* window) {
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_set_capture(desktop, window);
}

void services_desktop_release_capture() {
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop->capture = nullptr;
}

Window* services_desktop_captured() {
    const Desktop* desktop = services().desktop;
    return desktop != nullptr ? desktop->capture : nullptr;
}

Window* services_desktop_hover() {
    const Desktop* desktop = services().desktop;
    return desktop != nullptr ? desktop->hover : nullptr;
}

void services_desktop_set_hover(Window* window) {
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop->hover = window;
}

Window* desktop_window_at(Desktop* desktop, int32_t x, int32_t y) {
    Window* hit = nullptr;
    for (int32_t i = 0; i < desktop->windows.count; ++i) {
        Window* window = desktop->windows.items[i];
        if (window == nullptr) {
            continue;
        }
        Window* found = window->ops->child_at(window, x, y);
        if (found != nullptr) {
            hit = found;
        }
    }
    return hit;
}

} // namespace oa::boneyards
