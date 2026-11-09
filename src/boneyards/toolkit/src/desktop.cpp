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

/// The window's own name, or else the caption it shows.
///
/// A window with neither is the none stop, and is skipped. The driver's
/// control_name uses the same two sources.
bool has_control_name(const Window* window) {
    if (window->name != nullptr && window->name[0] != '\0')
        return true;
    if (window->ops == nullptr || window->ops->state_text == nullptr)
        return false;
    const char* text = window->ops->state_text(const_cast<Window*>(window), -1);
    return text != nullptr && text[0] != '\0';
}

/// A control the direction keys can land on.
///
/// Buttons, checks, radios, stage buttons, text fields, lists and sliders.
/// Disabled ones (their first state) and controls that do not take focus are
/// skipped. Scroll buttons and buttons that refuse focus are not stops.
/// Radios that share a group are one stop: the one that is chosen.
bool is_focus_stop(const Window* window) {
    if (window == nullptr || window->focusable == 0 || !window_is_shown(window))
        return false;
    // A stop with neither a name nor a caption is reported as none, and a
    // stop with no area cannot be drawn or pointed at. Both are skipped.
    if (!has_control_name(window))
        return false;
    if (window->width <= 0 || window->height <= 0)
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
    if (radio && window->radio_group != 0 && window->states.current != button_state::pressed)
        return false;
    return true;
}

/// A click may focus the window `focus_target` returned.
///
/// Direction keys land only on the control classes. A click still focuses a
/// probe, or any other shown window that has a name or a caption, and an
/// area. A disabled face does not take the keyboard. The click is delivered
/// either way.
bool can_take_click_focus(const Window* window) {
    if (window == nullptr || window->focusable == 0 || !window_is_shown(window))
        return false;
    if (!has_control_name(window))
        return false;
    if (window->width <= 0 || window->height <= 0)
        return false;
    const bool button = is_class_name(window, class_name::button);
    const bool check = is_class_name(window, class_name::check_box);
    const bool radio = is_class_name(window, class_name::radio_button);
    const bool stage = is_class_name(window, class_name::stage_button);
    const bool edit = is_class_name(window, class_name::single_edit);
    const bool slider = is_class_name(window, class_name::slider);
    if ((button || check || radio || stage || slider || edit) && window->states.current == 0)
        return false;
    return true;
}

bool shown_modal(const Window* window) {
    return window != nullptr && window_is_shown(window) &&
           (window->modal != 0 || is_class_name(window, class_name::dialog));
}

bool has_shown_modal(const Window* window) {
    for (int32_t i = 0; i < window->children.count; ++i) {
        const Window* child = window->children.items[i];
        if (child == nullptr)
            continue;
        if (shown_modal(child) || has_shown_modal(child))
            return true;
    }
    return false;
}

/// The topmost shown dialog, or window marked modal, that does not itself
/// contain another. Later windows are in front. Keys stay inside it.
void note_modal(Window* window, Window** found) {
    if (window == nullptr || !window_is_shown(window))
        return;
    if (shown_modal(window) && !has_shown_modal(window))
        *found = window;
    for (int32_t i = 0; i < window->children.count; ++i)
        note_modal(window->children.items[i], found);
}

Window* top_modal(Desktop* desktop) {
    Window* found = nullptr;
    for (int32_t i = 0; i < desktop->windows.count; ++i)
        note_modal(desktop->windows.items[i], &found);
    return found;
}

bool already_listed(Window* const* out, int32_t count, const Window* window) {
    for (int32_t i = 0; i < count; ++i) {
        if (out[i] == window)
            return true;
    }
    return false;
}

void collect_stops(Window* window, Window** out, int32_t* count) {
    if (window == nullptr || !window_is_shown(window) || *count >= k_focus_cap)
        return;
    if (is_focus_stop(window) && !already_listed(out, *count, window))
        out[(*count)++] = window;
    for (int32_t i = 0; i < window->children.count && *count < k_focus_cap; ++i)
        collect_stops(window->children.items[i], out, count);
}

int32_t collect_candidates(Desktop* desktop, Window** out) {
    int32_t count = 0;
    Window* modal = top_modal(desktop);
    if (modal != nullptr) {
        for (int32_t i = 0; i < modal->children.count && count < k_focus_cap; ++i)
            collect_stops(modal->children.items[i], out, &count);
        return count;
    }
    for (int32_t i = 0; i < desktop->windows.count && count < k_focus_cap; ++i)
        collect_stops(desktop->windows.items[i], out, &count);
    return count;
}

/// The nearest ancestor, including the window, whose region number is set.
Window* region_root_of(Window* window) {
    for (Window* cursor = window; cursor != nullptr; cursor = cursor->parent) {
        if (cursor->region != 0)
            return cursor;
    }
    return nullptr;
}

void remember_region(Desktop* desktop, Window* window) {
    Window* region = region_root_of(window);
    if (region != nullptr)
        desktop->keyboard_region = region;
}

/// Stops inside `scope`, in child order. A nested region is left for F6.
void gather_stops(
    Window* window, const Window* scope, bool skip_regions, Window** out, int32_t* count
) {
    if (window == nullptr || !window_is_shown(window) || *count >= k_focus_cap)
        return;
    if (skip_regions && window != scope && window->region != 0)
        return;
    if (window != scope && is_focus_stop(window) && !already_listed(out, *count, window))
        out[(*count)++] = window;
    for (int32_t i = 0; i < window->children.count && *count < k_focus_cap; ++i)
        gather_stops(window->children.items[i], scope, skip_regions, out, count);
}

/// True when `focus` is outside `modal`, or is the modal's own root.
bool focus_outside(const Window* modal, const Window* focus) {
    if (modal == nullptr)
        return false;
    return focus == nullptr || focus == modal || !tree_holds(modal, focus);
}

/// The answer a dialog offers first: Choose, Ok, No, Activate, Open, or Back.
///
/// The first Tab, Shift+Tab or arrow lands here while the focus is still
/// outside the dialog or on its root. A later key moves as usual.
Window* default_answer(Window* modal) {
    static const char* names[] = {"Choose", "Ok", "OK", "No", "Activate", "Open", "Back"};
    for (const char* name : names) {
        Window* found = window_find_named(modal, name, nullptr);
        if (is_focus_stop(found))
            return found;
    }
    return nullptr;
}

void land(Desktop* desktop, Window* target) {
    if (target != nullptr && target != desktop->focus)
        desktop_set_focus(desktop, target);
}

/// Tab and Shift+Tab stay in the dialog, or else in the region that has the
/// keyboard. The first one, while the focus is outside the dialog or on its
/// root, lands on the dialog's default answer.
bool move_tab(Desktop* desktop, bool backwards) {
    Window* modal = top_modal(desktop);
    Window* scope = modal;
    bool skip_regions = false;
    if (scope == nullptr && desktop->focus != nullptr)
        scope = region_root_of(desktop->focus);
    if (scope == nullptr)
        return false;
    if (scope->region != 0)
        skip_regions = true;
    Window* items[k_focus_cap];
    int32_t count = 0;
    gather_stops(scope, scope, skip_regions, items, &count);
    if (count == 0)
        return true;
    if (modal != nullptr && focus_outside(modal, desktop->focus)) {
        Window* answer = default_answer(modal);
        land(desktop, answer != nullptr ? answer : (backwards ? items[count - 1] : items[0]));
        return true;
    }
    int32_t index = -1;
    for (int32_t i = 0; i < count; ++i) {
        if (items[i] == desktop->focus)
            index = i;
    }
    int32_t next = 0;
    if (index < 0)
        next = backwards ? count - 1 : 0;
    else if (backwards)
        next = index == 0 ? count - 1 : index - 1;
    else
        next = index + 1 >= count ? 0 : index + 1;
    land(desktop, items[next]);
    return true;
}

void collect_regions(Window* window, Window** out, int32_t* count) {
    if (window == nullptr || !window_is_shown(window) || *count >= k_focus_cap)
        return;
    if (window->region != 0 && !already_listed(out, *count, window))
        out[(*count)++] = window;
    for (int32_t i = 0; i < window->children.count && *count < k_focus_cap; ++i)
        collect_regions(window->children.items[i], out, count);
}

void sort_regions(Window** regions, int32_t count) {
    for (int32_t i = 1; i < count; ++i) {
        Window* region = regions[i];
        int32_t slot = i;
        while (slot > 0 && regions[slot - 1]->region > region->region) {
            regions[slot] = regions[slot - 1];
            --slot;
        }
        regions[slot] = region;
    }
}

/// F6 and Shift+F6 step through the regions that are showing. A dialog
/// consumes the key and stays where it is. The landing spot is the first
/// stop in the region, or the region itself when it has none yet.
bool move_f6(Desktop* desktop, bool backwards) {
    if (top_modal(desktop) != nullptr)
        return true;
    Window* regions[k_focus_cap];
    int32_t count = 0;
    for (int32_t i = 0; i < desktop->windows.count && count < k_focus_cap; ++i)
        collect_regions(desktop->windows.items[i], regions, &count);
    sort_regions(regions, count);
    if (count == 0)
        return false;
    // Start from the region the focus is in. keyboard_region is the one
    // remembered when the focus last moved, and a console can be stamped
    // after that, so it is only a fallback once the focus is hidden.
    Window* current = nullptr;
    if (desktop->focus != nullptr && window_is_shown(desktop->focus))
        current = region_root_of(desktop->focus);
    if (current == nullptr)
        current = desktop->keyboard_region;
    int32_t index = -1;
    for (int32_t i = 0; i < count; ++i) {
        if (regions[i] == current)
            index = i;
    }
    int32_t cursor = index;
    for (int32_t tried = 0; tried < count; ++tried) {
        if (cursor < 0)
            cursor = backwards ? count - 1 : 0;
        else if (backwards)
            cursor = cursor == 0 ? count - 1 : cursor - 1;
        else
            cursor = cursor + 1 >= count ? 0 : cursor + 1;
        Window* region = regions[cursor];
        Window* stops[k_focus_cap];
        int32_t stop_count = 0;
        gather_stops(region, region, true, stops, &stop_count);
        Window* landing = stop_count > 0 ? stops[0] : nullptr;
        if (landing == nullptr && region->focusable != 0)
            landing = region;
        if (landing == nullptr)
            continue;
        desktop->keyboard_region = region;
        if (landing != desktop->focus)
            desktop_set_focus(desktop, landing);
        return true;
    }
    return true;
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

} // namespace

int32_t nearest_planet(
    const int32_t* xs,
    const int32_t* ys,
    const uint8_t* linked,
    int32_t count,
    int32_t from,
    int32_t direction
) {
    // from * count + planet has to stay inside a signed 32-bit index.
    constexpr int32_t k_index_limit = 46340;
    if (xs == nullptr || ys == nullptr || linked == nullptr || count <= 0 ||
        count > k_index_limit || from < 0 || from >= count)
        return -1;
    int32_t step_x = 0;
    int32_t step_y = 0;
    if (direction == key::left)
        step_x = -1;
    else if (direction == key::right)
        step_x = 1;
    else if (direction == key::up)
        step_y = -1;
    else if (direction == key::down)
        step_y = 1;
    else
        return -1;
    const int32_t origin_x = xs[from];
    const int32_t origin_y = ys[from];
    int32_t best = -1;
    int32_t best_cost = 0;
    uint8_t best_linked = 0;
    for (int32_t planet = 0; planet < count; ++planet) {
        if (planet == from)
            continue;
        const int32_t dx = xs[planet] - origin_x;
        const int32_t dy = ys[planet] - origin_y;
        const int32_t along = step_x != 0 ? step_x * dx : step_y * dy;
        const int32_t aside = step_x != 0 ? (dy < 0 ? -dy : dy) : (dx < 0 ? -dx : dx);
        if (along <= 0 || along < aside)
            continue;
        const uint8_t joined = linked[from * count + planet] != 0 ? 1 : 0;
        const int32_t cost = along + aside;
        if (best < 0 || joined > best_linked || (joined == best_linked && cost < best_cost)) {
            best = planet;
            best_cost = cost;
            best_linked = joined;
        }
    }
    return best;
}

namespace {

/// The next radio in a group, above or below, wrapping at the ends.
///
/// Left and right leave the group. A disabled radio is not a landing spot.
Window* radio_step(Window* from, FocusDirection direction) {
    if (from == nullptr || from->radio_group == 0 || from->parent == nullptr)
        return nullptr;
    if (direction != FocusDirection::up && direction != FocusDirection::down)
        return nullptr;
    const bool upward = direction == FocusDirection::up;
    Window* best = nullptr;
    Window* wrap = nullptr;
    for (int32_t i = 0; i < from->parent->children.count; ++i) {
        Window* child = from->parent->children.items[i];
        if (child == nullptr || child == from || child->radio_group != from->radio_group)
            continue;
        if (!is_class_name(child, class_name::radio_button) || !window_is_shown(child))
            continue;
        if (child->states.current == button_state::disabled)
            continue;
        const bool ahead = upward ? child->y < from->y : child->y > from->y;
        Window** slot = ahead ? &best : &wrap;
        if (*slot == nullptr)
            *slot = child;
        else if (upward ? child->y > (*slot)->y : child->y < (*slot)->y)
            *slot = child;
    }
    return best != nullptr ? best : wrap;
}

void choose_radio(Window* radio) {
    if (radio->states.current == button_state::pressed)
        return;
    Message enter{};
    enter.code = MessageCode::key_down;
    enter.key = key::enter;
    window_dispatch(radio, &enter);
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
    Window* modal = top_modal(desktop);
    if (arrow && focus_outside(modal, desktop->focus)) {
        Window* items[k_focus_cap];
        const int32_t count = collect_candidates(desktop, items);
        Window* target = default_answer(modal);
        if (target == nullptr && count > 0)
            target = items[0];
        if (target == nullptr)
            return false;
        land(desktop, target);
        return true;
    }
    if (arrow && desktop->focus != nullptr &&
        (direction == FocusDirection::up || direction == FocusDirection::down)) {
        Window* other = radio_step(desktop->focus, direction);
        if (other != nullptr) {
            choose_radio(other);
            land(desktop, other);
            return true;
        }
    }
    Window* items[k_focus_cap];
    const int32_t count = collect_candidates(desktop, items);
    if (count == 0)
        return false;
    Window* target = nullptr;
    if (!arrow || bare) {
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

/// Records the window that had the keyboard when `window` took it.
///
/// The record sits on the outermost ancestor of `window` that does not
/// already contain `old`, so moving inside a console does not replace the
/// button that opened it.
void remember_opener(Window* window, Window* old) {
    if (window == nullptr || old == nullptr || !window_is_shown(old))
        return;
    if (tree_holds(window, old))
        return;
    Window* boundary = window;
    for (Window* parent = window->parent; parent != nullptr; parent = parent->parent) {
        if (tree_holds(parent, old))
            break;
        boundary = parent;
    }
    boundary->focus_return = old;
}

/// True while a window marked as an F6 region is showing.
bool region_shown(Desktop* desktop) {
    Window* regions[k_focus_cap];
    int32_t count = 0;
    for (int32_t i = 0; i < desktop->windows.count && count < k_focus_cap; ++i)
        collect_regions(desktop->windows.items[i], regions, &count);
    return count > 0;
}

/// Tab and F6 are taken only while a dialog is up, or the focus is in a region.
bool navigation_confined(Desktop* desktop) {
    if (top_modal(desktop) != nullptr)
        return true;
    Window* focus = desktop->focus;
    return focus != nullptr && window_is_shown(focus) && region_root_of(focus) != nullptr;
}

/// A focus left on a hidden control is dropped, or handed back, before a key.
void repair_hidden_focus(Desktop* desktop) {
    Window* focus = desktop->focus;
    if (focus == nullptr || window_is_shown(focus))
        return;
    Window* back = nullptr;
    for (Window* cursor = focus; cursor != nullptr; cursor = cursor->parent) {
        // A remembered opener may have been destroyed since (a message box's
        // answer, a page that was closed). Only a window the desktop still
        // holds is read.
        Window* candidate = cursor->focus_return;
        if (desktop_holds(desktop, candidate) && window_is_shown(candidate)) {
            back = candidate;
            break;
        }
    }
    desktop_set_focus(desktop, nullptr);
    if (back != nullptr)
        desktop_set_focus(desktop, back);
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
    remember_opener(window, old);
    if (old != nullptr) {
        window_send(old, MessageCode::focus_lost);
    }
    desktop->focus = window;
    if (window != nullptr) {
        window_send(window, MessageCode::focus_gained);
        remember_region(desktop, window);
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
    // A taken Tab or F6 starts keyboard travel. Return ends it. A key-up
    // changes nothing, so the ring stays up while the key is let go.
    const auto finish = [&](bool taken) {
        if (taken && msg->code == MessageCode::key_down) {
            if (msg->key == key::tab || msg->key == key::f6)
                desktop->keyboard_travel = 1;
            else if (msg->key == key::enter)
                desktop->keyboard_travel = 0;
        }
        return taken;
    };
    if (msg->code == MessageCode::key_down || msg->code == MessageCode::key_up) {
        if (msg->code == MessageCode::key_down) {
            desktop->pointer_last = 0;
            repair_hidden_focus(desktop);
        }
        // Tab and F6 move the keyboard before a control can swallow them,
        // and only while a dialog is up or the focus is inside a region.
        // F6 also starts from nothing while a region is showing, so the war
        // map is reachable before anything has been clicked. Anywhere else
        // the focus is given the key, as it was.
        if (msg->code == MessageCode::key_down) {
            const bool confined = navigation_confined(desktop);
            if (msg->key == key::tab && confined)
                return finish(move_tab(desktop, input_key_down(key::shift)));
            if (msg->key == key::f6 &&
                (confined || (desktop->focus == nullptr && region_shown(desktop))))
                return finish(move_f6(desktop, input_key_down(key::shift)));
        }
        Window* modal = top_modal(desktop);
        const bool outside = focus_outside(modal, desktop->focus);
        if (msg->code == MessageCode::key_down && outside && msg->key == key::escape) {
            window_dispatch(modal, msg);
            return finish(true);
        }
        FocusDirection arrow = FocusDirection::down;
        if (msg->code == MessageCode::key_down && outside && direction_of(msg->key, &arrow))
            return finish(move_focus(desktop, msg));
        if (desktop->focus != nullptr && window_dispatch(desktop->focus, msg))
            return finish(true);
        if (msg->code != MessageCode::key_down) {
            FocusDirection direction = FocusDirection::down;
            if (!direction_of(msg->key, &direction))
                return false;
            Window* items[k_focus_cap];
            return collect_candidates(desktop, items) > 0;
        }
        return finish(move_focus(desktop, msg));
    }
    // Any move onto a new place ends keyboard travel, whether or not a window
    // takes it. The first place is only recorded, and a repeat stays put.
    if (is_mouse_move(msg)) {
        if (desktop->pointer_known == 0) {
            desktop->pointer_x = msg->x;
            desktop->pointer_y = msg->y;
            desktop->pointer_known = 1;
        } else if (msg->x != desktop->pointer_x || msg->y != desktop->pointer_y) {
            desktop->pointer_x = msg->x;
            desktop->pointer_y = msg->y;
            desktop->keyboard_travel = 0;
        }
    }
    if (is_pointer_button(msg))
        desktop->pointer_last = 1;
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
    // A disabled face, or a window with no name, no caption or no area, does
    // not take the keyboard. The click still reaches it; the keyboard stays put.
    if (focus != nullptr && !can_take_click_focus(focus))
        focus = nullptr;
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
