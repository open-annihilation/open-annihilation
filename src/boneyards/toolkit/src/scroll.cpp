// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slider (scroll bar with "decbutton", "incbutton" and "sbutton" thumb
// children) and ListBox (vertical item list driven by a child "slider").
#include <cstdlib>
#include <cstring>

#include "list.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "ops_defaults.hpp"

namespace oa::boneyards {
namespace {

constexpr int32_t k_slider_default_range = 100;
constexpr int32_t k_slider_default_index = 50;
constexpr int32_t k_slider_repeat_delay = 15; // pulses before auto-repeat
constexpr int32_t k_list_default_capacity = 500;
const char* const k_dec_button = "decbutton";
const char* const k_inc_button = "incbutton";
const char* const k_thumb = "sbutton";
const char* const k_list_slider = "slider";

void destroy_record(Window* self) {
    window_release(self);
    std::free(self);
}

bool horizontal(const Window* w) {
    return w->height < w->width;
}

// Background image or colour, focus frame, then children.
bool draw_body(Window* self, bool focus_frame) {
    const int32_t state = self->states.current;
    const Handle image =
        state >= 0 && state < self->states.count ? self->states.images[state] : k_no_handle;
    if (image < 0) {
        if (self->color >= 0) {
            color_fill(self->x, self->y, self->width, self->height, self->color);
        }
    } else {
        image_draw(self->x, self->y, self->width, self->height, image);
    }
    if (focus_frame && self->focused != 0) {
        draw_focus_frame(self->x, self->y, self->width, self->height);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Slider

Slider* as_slider(Window* w) {
    return reinterpret_cast<Slider*>(w);
}

/// Tests whether the slider counts backwards: its "incbutton" lies before its
/// "decbutton" (left of it when horizontal, above it when vertical).
///
/// @param self slider to test
/// @return true when reversed; false without both arrow buttons
bool slider_reversed(Window* self) {
    Window* inc = window_find_named(self, k_inc_button, nullptr);
    Window* dec = window_find_named(self, k_dec_button, nullptr);
    if (inc == nullptr || dec == nullptr) {
        return false;
    }
    return horizontal(self) ? inc->x < dec->x : inc->y < dec->y;
}

/// Computes the pixel range the thumb's centre moves along.
///
/// The track runs between the inner edges of the two arrow buttons, or along
/// the whole slider without both of them, inset at each end by half the
/// "sbutton" thumb's length. A slider wider than tall is horizontal.
///
/// @param self slider to measure
/// @param[out] low first thumb-centre coordinate (x when horizontal, else y)
/// @param[out] high last thumb-centre coordinate
void slider_track(Window* self, int32_t* low, int32_t* high) {
    Window* inc = window_find_named(self, k_inc_button, nullptr);
    Window* dec = window_find_named(self, k_dec_button, nullptr);
    Window* thumb = window_find_named(self, k_thumb, nullptr);
    const bool across = horizontal(self);
    int32_t half = 0;
    const int32_t thumb_size = thumb == nullptr ? 0 : (across ? thumb->width : thumb->height);
    if (thumb_size != 0) {
        half = thumb_size / 2;
    }
    const int32_t origin = across ? self->x : self->y;
    const int32_t length = across ? self->width : self->height;
    if (inc == nullptr || dec == nullptr) {
        *low = half + origin;
        *high = (length - half) + origin;
        return;
    }
    const int32_t inc_pos = across ? inc->x : inc->y;
    const int32_t dec_pos = across ? dec->x : dec->y;
    const int32_t inc_size = across ? inc->width : inc->height;
    const int32_t dec_size = across ? dec->width : dec->height;
    if (dec_pos <= inc_pos) {
        *low = dec_size + dec_pos + half;
        *high = inc_pos - half;
        return;
    }
    *low = inc_size + inc_pos + half;
    *high = dec_pos - half;
}

/// Converts a slider index to the track coordinate of the thumb's centre.
///
/// @param self slider (a Slider record) to measure
/// @param index slider position; mirrored when the slider is reversed
/// @return the rounded coordinate along the track (x when horizontal, else
///     y); the track start when the range is below 2
int32_t slider_position(Window* self, int32_t index) {
    Slider* s = as_slider(self);
    if (slider_reversed(self)) {
        index = (s->range - index) - 1;
    }
    int32_t low = 0;
    int32_t high = 0;
    slider_track(self, &low, &high);
    if (s->range < 2) {
        return low;
    }
    const int32_t steps = s->range - 1;
    return (steps / 2 + (high - low) * index) / steps + low;
}

} // namespace

void slider_enable(Window* self) {
    window_set_state(self, button_state::unpressed);
    for (int32_t i = 0; i < self->children.count; ++i) {
        window_set_state(self->children.items[i], button_state::unpressed);
    }
}

void slider_disable(Window* self) {
    window_set_state(self, button_state::disabled);
    for (int32_t i = 0; i < self->children.count; ++i) {
        window_set_state(self->children.items[i], button_state::disabled);
    }
}

namespace {

/// Pages the slider by its page step so the thumb moves towards the last
/// track press.
///
/// Nothing happens while the press lies within the thumb or no further than
/// the neighbouring position.
///
/// @param[in,out] self slider (a Slider record) to page
void slider_page_towards_press(Window* self) {
    Slider* s = as_slider(self);
    Window* thumb = window_find_named(self, k_thumb, nullptr);
    int32_t half = 0;
    int32_t pressed = 0;
    if (horizontal(self)) {
        half = thumb != nullptr ? thumb->width / 2 : 0;
        pressed = s->press_x;
    } else {
        half = thumb != nullptr ? thumb->height / 2 : 0;
        pressed = s->press_y;
    }
    if (pressed < slider_position(self, s->index) - half &&
        pressed < slider_position(self, s->index - 1)) {
        if (!slider_reversed(self)) {
            slider_set_value(s, slider_index(s) - s->page_step);
            return;
        }
    } else {
        if (pressed <= slider_position(self, s->index) + half) {
            return;
        }
        if (pressed <= slider_position(self, s->index + 1)) {
            return;
        }
        if (slider_reversed(self)) {
            slider_set_value(s, slider_index(s) - s->page_step);
            return;
        }
    }
    slider_set_value(s, slider_index(s) + s->page_step);
}

// Steps from an arrow button, with auto-repeat after the delay.
void slider_arrow(Slider* s, int32_t step) {
    if (s->repeat_delay < s->repeat_count) {
        slider_set_value(s, slider_index(s) + step);
        return;
    }
    if (s->repeat_count == 0) {
        slider_set_value(s, slider_index(s) + step);
    }
    ++s->repeat_count;
}

/// Handles events from the slider's own buttons.
///
/// An activated "decbutton" or "incbutton" steps the index by -1 or +1: once
/// at the press, then again on every activation after the auto-repeat delay.
/// A dragged "sbutton" sets the index from the pointer's place along the
/// track. A released scroll button resets the auto-repeat count. Every change
/// is reported to the parent. A disabled slider ignores all of it.
///
/// @param[in,out] self slider (a Slider record) receiving the event
/// @param notify event from a child; null is ignored
void slider_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr || self->states.current == button_state::disabled) {
        return;
    }
    Slider* s = as_slider(self);
    if (notify->code == NotifyCode::scroll_released) {
        s->repeat_count = 0;
    }
    if (window_is_named(notify->source, k_dec_button)) {
        if (notify->code == NotifyCode::activated) {
            slider_arrow(s, -1);
        }
        return;
    }
    if (window_is_named(notify->source, k_inc_button)) {
        if (notify->code == NotifyCode::activated) {
            slider_arrow(s, 1);
        }
        return;
    }
    if (!window_is_named(notify->source, k_thumb) || notify->code != NotifyCode::dragged ||
        notify->message == nullptr) {
        return;
    }
    int32_t low = 0;
    int32_t high = 0;
    slider_track(self, &low, &high);
    if (high - low == 0) {
        return;
    }
    const int32_t pos = horizontal(self) ? notify->message->x : notify->message->y;
    const int32_t span = (high - low) - 1;
    int32_t index = ((pos - low) * (s->range - 1) + span / 2) / span;
    if (slider_reversed(self)) {
        index = (s->range - index) - 1;
    }
    slider_set_value(s, index);
}

/// Draws a slider: its state image or background colour, the focus frame
/// while focused, then its buttons.
///
/// A hidden slider draws nothing.
///
/// @param self slider to draw
/// @return true (handled)
bool slider_draw(Window* self) {
    if (window_is_shown(self)) {
        draw_body(self, true);
        window_draw_children(self);
    }
    return true;
}

/// Moves the slider from the keyboard: Page Up and Page Down by the page
/// step, Up and Right by +1, Left and Down by -1; each change is reported to
/// the parent.
///
/// Other keys take the default key handling.
///
/// @param[in,out] self slider (a Slider record) receiving the key
/// @param msg key message; null, a code other than key_down, or a disabled
///     slider is not handled
/// @return true for the keys above, otherwise the default key_down result
bool slider_key_down(Window* self, const Message* msg) {
    if (msg == nullptr || self->states.current == button_state::disabled ||
        msg->code != MessageCode::key_down) {
        return false;
    }
    Slider* s = as_slider(self);
    switch (msg->key) {
    case key::page_up:
        slider_set_value(s, slider_index(s) + s->page_step);
        return true;
    case key::page_down:
        slider_set_value(s, slider_index(s) - s->page_step);
        return true;
    case key::left:
    case key::down:
        slider_set_value(s, slider_index(s) - 1);
        return true;
    case key::up:
    case key::right:
        slider_set_value(s, slider_index(s) + 1);
        return true;
    default:
        return window_default_key_down(self, msg);
    }
}

/// Presses the track: records the press point and, unless the track is
/// already Pressed, captures the pointer, shows Pressed and pages once
/// towards the press.
///
/// Holding the press pages again through slider_pulse.
///
/// @param[in,out] self slider (a Slider record) being pressed
/// @param msg left_down message with the pointer position; null is not
///     handled
/// @return true, or false for a null message or a disabled slider
bool slider_left_down(Window* self, const Message* msg) {
    if (msg == nullptr || self->states.current == button_state::disabled) {
        return false;
    }
    Slider* s = as_slider(self);
    s->repeat_count = 0;
    s->press_x = msg->x;
    s->press_y = msg->y;
    if (!detail::window_is_pressed(self)) {
        services_desktop_capture(self);
        window_set_state(self, button_state::pressed);
        slider_page_towards_press(self);
    }
    return true;
}

/// Releases the track: drops the pointer capture, shows Unpressed and resets
/// the auto-repeat count.
///
/// @param[in,out] self slider (a Slider record) being released
/// @param msg pointer message (unused)
/// @return true, or false for a disabled slider
bool slider_left_up(Window* self, const Message* /*msg*/) {
    if (self->states.current == button_state::disabled) {
        return false;
    }
    services_desktop_release_capture();
    window_set_state(self, button_state::unpressed);
    as_slider(self)->repeat_count = 0;
    return true;
}

/// Tracks a held pointer the slider has captured: over the slider it records
/// the new press point and shows Pressed, elsewhere it shows Unpressed.
///
/// @param[in,out] self slider (a Slider record) receiving the move
/// @param msg mouse_move message with the pointer position and held buttons;
///     null is not handled
/// @return true, or false for a null message
bool slider_mouse_move(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (self->states.current != button_state::disabled && (msg->buttons & button_any) != 0 &&
        services_desktop_captured() == self) {
        if (self->ops->contains(self, msg->x, msg->y)) {
            as_slider(self)->press_x = msg->x;
            as_slider(self)->press_y = msg->y;
            window_set_state(self, button_state::pressed);
            return true;
        }
        window_set_state(self, button_state::unpressed);
    }
    return true;
}

/// Repeats track paging while the track stays Pressed, once the auto-repeat
/// delay has passed.
///
/// @param[in,out] self slider (a Slider record) being pulsed; its repeat
///     count advances
void slider_pulse(Window* self) {
    Slider* s = as_slider(self);
    if (detail::window_is_pressed(self)) {
        if (s->repeat_delay < s->repeat_count) {
            slider_page_towards_press(self);
            return;
        }
        ++s->repeat_count;
    }
}

/// Tests whether a class name is "Slider".
///
/// @param name class name to test; may be null
/// @return true when `name` is "Slider"
bool slider_is_class(const char* name) {
    return names_equal(name, class_name::slider);
}

int32_t zero_type_code() {
    return 0;
}

constexpr WindowOps make_slider_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::slider;
    ops.destroy = destroy_record;
    ops.is_class = slider_is_class;
    ops.type_code = zero_type_code;
    ops.pulse = slider_pulse;
    ops.on_notify = slider_on_notify;
    ops.draw = slider_draw;
    ops.key_down = slider_key_down;
    ops.left_down = slider_left_down;
    ops.left_up = slider_left_up;
    ops.mouse_move = slider_mouse_move;
    return ops;
}

// ---------------------------------------------------------------------------
// ListBox

ListBox* as_list(Window* w) {
    return reinterpret_cast<ListBox*>(w);
}

Slider* list_slider(Window* self) {
    Window* found = window_find_named(self, k_list_slider, class_name::slider);
    return found != nullptr ? as_slider(found) : nullptr;
}

/// Lays the list out from its top item and updates its slider.
///
/// Items stack downwards from the list's top-left corner from the top item
/// on; items before it, and items that no longer fit the list's height, are
/// hidden. While items are hidden and the item before the top would still
/// fit, the top moves back one and the layout repeats. With bottom_align the
/// shown items move down against the bottom edge. The slider's range becomes
/// the hidden count + 1 and its index follows the top; with nothing hidden
/// the slider is hidden (keep_slider) or disabled. The page size and the
/// slider's page step become the number of items from the top to the last
/// shown item, or 0 when those are the same item. Nothing is placed without
/// a child named "slider".
///
/// @param[in,out] self list (a ListBox record) to lay out
void list_layout(Window* self) {
    ListBox* list = as_list(self);
    Slider* slider = nullptr;
    int32_t hidden = 0;
    for (;;) {
        int32_t top_height = self->height;
        int32_t offset = 0;
        hidden = 0;
        slider = list_slider(self);
        for (int32_t i = 0; i < self->children.count; ++i) {
            Window* item = self->children.items[i];
            if (i == 0 || slider == nullptr || item == nullptr) {
                continue;
            }
            window_move_to(item, self->x, self->y + offset);
            if (i == list->top) {
                top_height = item->height;
            }
            if (i < list->top + 1 || self->height <= offset + item->height) {
                item->visible = 0;
                ++hidden;
            } else {
                item->visible = 1;
                list->last = i - 1;
                offset += item->height;
            }
        }
        if (list->bottom_align != 0) {
            const int32_t shift = self->height - offset;
            for (int32_t i = 0; i < self->children.count; ++i) {
                Window* item = self->children.items[i];
                if (!window_is_named(item, k_list_slider) && window_is_shown(item)) {
                    window_move_to(item, item->x, item->y + shift);
                }
            }
        }
        if (hidden == 0 || list->top < 1 || self->height <= top_height + offset) {
            break;
        }
        --list->top;
    }
    Slider* bar = list_slider(self);
    if (bar != nullptr) {
        slider_set_range(bar, hidden + 1, false);
        slider_set_index(bar, hidden - list->top);
        if (hidden == 0) {
            if (list->keep_slider != 0) {
                bar->base.visible = 0;
            } else {
                slider_disable(&bar->base);
            }
        } else {
            if (bar->base.states.current == button_state::disabled) {
                slider_enable(&bar->base);
            }
            bar->base.visible = 1;
        }
    }
    list->page = list->last != list->top ? (list->last - list->top) + 1 : 0;
    if (slider != nullptr) {
        slider->page_step = list->page;
    }
}

/// Scrolls the selected item into view and lays the list out again.
///
/// Does nothing in log mode.
///
/// @param[in,out] self list (a ListBox record) to scroll
void list_reveal_selection(Window* self) {
    ListBox* list = as_list(self);
    if (list->log_mode != 0) {
        return;
    }
    const int32_t selected = list->selected;
    if (selected < list->top) {
        list->top = selected;
    } else if (list->last < selected) {
        list->top = (list->top - list->last) + selected;
    }
    list_layout(self);
}

/// Handles events from the list's slider and items.
///
/// An activated "slider" sets the top item from its index. An activated or
/// changed item becomes the selection when it is visible (not in log mode);
/// a changed item is also reported to the list's parent as
/// NotifyCode::changed.
///
/// @param[in,out] self list (a ListBox record) receiving the event
/// @param notify event from a child; null is ignored
void list_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr) {
        return;
    }
    ListBox* list = as_list(self);
    if (window_is_named(notify->source, k_list_slider)) {
        if (notify->code == NotifyCode::activated) {
            const Slider* bar = as_slider(notify->source);
            const int32_t top = (slider_range(bar) - slider_index(bar)) - 1;
            if (list->top != top) {
                list->top = top;
                list_layout(self);
            }
        }
        return;
    }
    if ((notify->code != NotifyCode::activated && notify->code != NotifyCode::changed) ||
        self->children.count == 0) {
        return;
    }
    if (list->log_mode == 0) {
        for (int32_t i = 1; i < self->children.count; ++i) {
            const int32_t item = i - 1;
            if (notify->source == self->children.items[i] && list->top <= item &&
                item <= list->last) {
                list_box_select(list, item);
                break;
            }
        }
    }
    if (notify->code == NotifyCode::changed) {
        window_notify_parent(self, NotifyCode::changed, 0);
    }
}

/// Draws a list: its state image or background colour, the selection colour
/// behind the selected item (the focused colour while the list has the
/// focus), then its children.
///
/// No highlight is drawn with no_highlight set, while the selected item is
/// out of view, or when the unfocused colour is unset. A hidden list draws
/// nothing.
///
/// @param self list (a ListBox record) to draw
/// @return true (handled)
bool list_draw(Window* self) {
    if (!window_is_shown(self)) {
        return true;
    }
    ListBox* list = as_list(self);
    draw_body(self, false);
    if (list->no_highlight == 0) {
        const int32_t selected = list_box_selected(list);
        if (selected + 1 < self->children.count && list->top <= list->selected &&
            list->selected <= list->last) {
            Window* item = self->children.items[selected + 1];
            if (item != nullptr && list->highlight >= 0) {
                const Handle color = self->focused != 0 ? list->focused_highlight : list->highlight;
                color_fill(item->x, item->y, item->width, item->height, color);
            }
        }
    }
    window_draw_children(self);
    return true;
}

/// Lays the list out again after a geometry change (the refresh slot),
/// keeping the selection in view outside log mode.
///
/// @param[in,out] self list (a ListBox record) to lay out
/// @return true (handled)
bool list_refresh(Window* self) {
    if (as_list(self)->log_mode != 0) {
        list_layout(self);
    } else {
        list_reveal_selection(self);
    }
    return true;
}

/// Finds the first child, in child order, whose child_at slot reports a hit.
///
/// With no child hit the result is null, never the list itself.
///
/// @param self list to search
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
/// @return the child's hit, or null when the list is hidden, missed or no
///     child is hit
Window* list_child_at(Window* self, int32_t x, int32_t y) {
    if (!self->ops->contains(self, x, y) || !window_is_shown(self)) {
        return nullptr;
    }
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child != nullptr) {
            Window* hit = child->ops->child_at(child, x, y);
            if (hit != nullptr) {
                return hit;
            }
        }
    }
    return nullptr;
}

/// Moves the selection from the keyboard, or scrolls in log mode.
///
/// Home and End go to the first and last item, Page Up and Page Down move by
/// a page, and Up and Down by one item. Other keys take the default key
/// handling.
///
/// @param[in,out] self list (a ListBox record) receiving the key
/// @param msg key message; null is not handled
/// @return true for the keys above (also when nothing moves), otherwise the
///     default key_down result
bool list_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    ListBox* list = as_list(self);
    const bool log = list->log_mode != 0;
    if (msg->code == MessageCode::key_down) {
        switch (msg->key) {
        case key::home:
            if (log) {
                if (list->top != 0) {
                    list_box_scroll(list, -list->top);
                }
            } else if (list->selected != 0) {
                list_box_select(list, 0);
            }
            return true;
        case key::end:
            if (log) {
                list_box_scroll_to_end(list);
            } else if (list->selected != list->count - 1) {
                list_box_select(list, list->count - 1);
            }
            return true;
        case key::page_up:
            if (log) {
                if (list->top > 0) {
                    list_box_scroll(list, -list->page);
                }
            } else if (list->selected > 0) {
                list_box_select(list, list->selected - list->page);
            }
            return true;
        case key::page_down:
            if (log) {
                if (list->last < list->count - 1) {
                    list_box_scroll(list, list->page);
                }
            } else if (list->selected < list->count - 1) {
                list_box_select(list, list->page + list->selected);
            }
            return true;
        case key::up:
            if (log) {
                if (list->top > 0) {
                    list_box_scroll(list, -1);
                }
            } else if (list->selected > 0) {
                list_box_select(list, list->selected - 1);
            }
            return true;
        case key::down:
            if (log) {
                if (list->last < list->count - 1) {
                    list_box_scroll(list, 1);
                }
            } else if (list->selected < list->count - 1) {
                list_box_select(list, list->selected + 1);
            }
            return true;
        default:
            break;
        }
    }
    return window_default_key_down(self, msg);
}

/// Frees a ListBox: releases the selection colours that are set, runs the
/// base teardown and frees the record.
///
/// The Boneyards console list's destroy handler adds nothing of its own.
///
/// @param self list (a ListBox record) to free
void list_destroy(Window* self) {
    ListBox* list = as_list(self);
    if (list->highlight > k_no_handle)
        color_release(list->highlight);
    if (list->focused_highlight > k_no_handle)
        color_release(list->focused_highlight);
    destroy_record(self);
}

/// Tests whether a class name is "ListBox".
///
/// @param name class name to test; may be null
/// @return true when `name` is "ListBox"
bool list_is_class(const char* name) {
    return names_equal(name, class_name::list_box);
}

constexpr WindowOps make_list_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::list_box;
    ops.destroy = list_destroy;
    ops.is_class = list_is_class;
    ops.type_code = zero_type_code;
    ops.child_at = list_child_at;
    ops.on_notify = list_on_notify;
    ops.draw = list_draw;
    ops.refresh = list_refresh;
    ops.key_down = list_key_down;
    return ops;
}

} // namespace

constinit const WindowOps k_slider_ops = make_slider_ops();
constinit const WindowOps k_list_box_ops = make_list_ops();

void slider_init(Slider* slider, const WindowOps* ops) {
    window_init(&slider->base, ops);
    slider->range = k_slider_default_range;
    slider->index = k_slider_default_index;
    slider->page_step = 1;
    slider->repeat_delay = k_slider_repeat_delay;
    window_set_state_count(&slider->base, 3);
    window_set_state(&slider->base, button_state::unpressed);
}

Slider* slider_create() {
    auto* s = static_cast<Slider*>(std::calloc(1, sizeof(Slider)));
    if (s == nullptr) {
        std::abort();
    }
    slider_init(s, &k_slider_ops);
    return s;
}

int32_t slider_range(const Slider* slider) {
    return slider->range;
}

int32_t slider_index(const Slider* slider) {
    return slider->index;
}

void slider_set_range(Slider* slider, int32_t range, bool keep_position) {
    if (range < 1) {
        range = 1;
    }
    const int32_t old_steps = slider->range - 1;
    slider->range = range;
    if (!keep_position) {
        return;
    }
    if (old_steps == 0) {
        slider_set_index(slider, slider_reversed(&slider->base) ? range - 1 : 0);
        return;
    }
    slider_set_index(slider, (old_steps / 2 + slider->index * (range - 1)) / old_steps);
}

void slider_set_index(Slider* slider, int32_t index) {
    if (index < 0) {
        slider->index = 0;
    } else if (index < slider->range) {
        slider->index = index;
    } else {
        slider->index = slider->range - 1;
    }
    Window* self = &slider->base;
    Window* thumb = window_find_named(self, k_thumb, nullptr);
    if (thumb == nullptr) {
        return;
    }
    if (horizontal(self)) {
        thumb->x = slider_position(self, slider->index) - thumb->width / 2;
        return;
    }
    thumb->y = slider_position(self, slider->index) - thumb->height / 2;
}

void slider_set_value(Slider* slider, int32_t index) {
    slider_set_index(slider, index);
    window_notify_parent(&slider->base, NotifyCode::activated, 0);
}

void list_box_init(ListBox* list, const WindowOps* ops) {
    window_init(&list->base, ops);
    list->capacity = k_list_default_capacity;
    list->highlight = k_no_handle;
    list->focused_highlight = k_no_handle;
    list_box_set_highlight(list, 0x9E, 0x6E, 0x2D, 0x4B);
    list_box_set_focused_highlight(list, 0x6F, 0x00, 0xCE, 0x64);
}

void list_box_set_highlight(ListBox* list, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (list->highlight > k_no_handle) {
        color_release(list->highlight);
    }
    list->highlight = color_create(r, g, b, a);
}

void list_box_set_focused_highlight(ListBox* list, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (list->focused_highlight > k_no_handle) {
        color_release(list->focused_highlight);
    }
    list->focused_highlight = color_create(r, g, b, a);
}

ListBox* list_box_create() {
    auto* list = static_cast<ListBox*>(std::calloc(1, sizeof(ListBox)));
    if (list == nullptr) {
        std::abort();
    }
    list_box_init(list, &k_list_box_ops);
    return list;
}

void list_box_add(ListBox* list, Window* item) {
    list_box_insert(list, item, -1);
}

void list_box_insert(ListBox* list, Window* item, int32_t position) {
    if (item == nullptr) {
        return;
    }
    Window* self = &list->base;
    window_move_to(item, self->x, self->y);
    item->visible = 0;
    window_set_parent(item, self, position);
    const int32_t count = list->count + 1;
    if (list->capacity < count) {
        if (self->children.count > 0) {
            const int32_t first = list_slider(self) != nullptr ? 1 : 0;
            if (first < self->children.count && self->children.items[first] != nullptr) {
                window_destroy(self->children.items[first]);
            }
        }
    } else {
        list->count = count;
    }
    list_layout(self);
}

void list_box_remove(ListBox* list, Window* item) {
    Window* self = &list->base;
    int32_t index = 0;
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child->ops->is_class(class_name::slider)) {
            continue;
        }
        if (child == item) {
            --list->count;
            if (index <= list->selected && list->selected != 0) {
                --list->selected;
            }
            if (index <= list->top && list->top != 0) {
                --list->top;
                --list->last;
            }
            list_remove_at(&self->children, i);
            item->parent = nullptr;
            list_layout(self);
            return;
        }
        ++index;
    }
}

void list_box_clear(ListBox* list) {
    Window* self = &list->base;
    for (;;) {
        int32_t first = 0;
        if (first < self->children.count &&
            self->children.items[first]->ops->is_class(class_name::slider)) {
            ++first;
        }
        if (first >= self->children.count) {
            break;
        }
        window_destroy(self->children.items[first]);
    }
    list->count = 0;
    list->top = 0;
    list->selected = 0;
    list->last = 0;
    list->page = 0;
    list_layout(self);
}

void list_box_select(ListBox* list, int32_t index) {
    if (list->log_mode != 0) {
        return;
    }
    if (index < 0) {
        index = 0;
    }
    if (list->count <= index) {
        index = list->count - 1;
    }
    if (list->selected == index) {
        return;
    }
    list->selected = index;
    if (index <= list->top) {
        list->top = index;
    }
    if (list->last <= index) {
        list->top += index - list->last;
    }
    list_layout(&list->base);
    window_notify_parent(&list->base, NotifyCode::activated, 0);
}

int32_t list_box_selected(const ListBox* list) {
    return list->log_mode != 0 ? 0 : list->selected;
}

Window* list_box_item(ListBox* list, int32_t index) {
    const int32_t child = index + 1;
    if (child < 0 || child >= list->base.children.count) {
        return nullptr;
    }
    return list->base.children.items[child];
}

int32_t list_box_index_of(ListBox* list, const Window* item) {
    Window* self = &list->base;
    const int32_t first = list_slider(self) != nullptr ? 1 : 0;
    for (int32_t i = first; i < self->children.count; ++i) {
        if (self->children.items[i] == item) {
            return i - first;
        }
    }
    return -1;
}

Window* list_box_find_text(ListBox* list, const char* text) {
    Window* self = &list->base;
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child->ops->is_class(class_name::slider)) {
            continue;
        }
        const char* caption = child->ops->state_text(child, -1);
        if (caption != nullptr && text != nullptr && std::strcmp(text, caption) == 0) {
            return child;
        }
    }
    return nullptr;
}

void list_box_scroll(ListBox* list, int32_t delta) {
    if (delta == 0) {
        return;
    }
    list->top += delta;
    if (list->top < 0) {
        list->top = 0;
    }
    if (list->count < list->top + list->page) {
        list->top = list->count - list->page;
    }
    list_layout(&list->base);
}

void list_box_scroll_to_end(ListBox* list) {
    if (list->last != list->count - 1) {
        list_box_scroll(list, (list->count - list->page) - list->top);
    }
}

} // namespace oa::boneyards
