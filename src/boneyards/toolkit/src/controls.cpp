// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Standard controls: Gadget, Button, NoFocusBtn, SButton, Static, CheckBox,
// RadioButton, StageBtn and Progress.
#include <cstdlib>
#include <cstring>

#include "oa/boneyards/toolkit.hpp"
#include "ops_defaults.hpp"

namespace oa::boneyards {
namespace detail {

bool forward_left_down(Window* self, const Message* msg) {
    return self->ops->left_down(self, msg);
}

bool forward_left_up(Window* self, const Message* msg) {
    return self->ops->left_up(self, msg);
}

bool forward_right_down(Window* self, const Message* msg) {
    return self->ops->right_down(self, msg);
}

bool window_is_pressed(const Window* self) {
    return self->states.current == button_state::pressed;
}

} // namespace detail

namespace {

using detail::forward_left_down;
using detail::forward_left_up;
using detail::forward_right_down;

constexpr int32_t k_button_states = button_state::count;
constexpr int32_t k_check_box_states = 5;
constexpr int32_t k_check_box_label_gap = 6;
constexpr int32_t k_stage_first = 2; // first cycling stage after Disabled/Pressed
constexpr int32_t k_stage_text_inset = 5;
constexpr int32_t k_key_enter = key::enter;
constexpr int32_t k_key_space = ' ';

const char* const k_button_state_names[] = {"Disabled", "Pressed", "Unpressed"};
const char* const k_check_state_names[] = {
    "Disabled", "Unchecked Down", "Checked Down", "Unchecked Up", "Checked Up"
};
const char* const k_stage_state_names[] = {
    "Disabled",
    "Pressed",
    "First",
    "Stage2",
    "Stage3",
    "Stage4",
    "Stage5",
    "Stage6",
    "Stage7",
    "Stage8",
    "Stage9"
};
const char* const k_progress_state_names[] = {"Disabled", "Left Side", "Center", "Right Side"};

template <size_t N>
int32_t lookup_state(const Window* self, const char* const (&names)[N], const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int32_t i = 0; i < self->states.count && i < static_cast<int32_t>(N); ++i) {
        if (std::strcmp(names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

template <size_t N>
const char* state_label(const Window* self, const char* const (&names)[N], int32_t state) {
    if (state < 0 || state >= self->states.count || state >= static_cast<int32_t>(N)) {
        return nullptr;
    }
    return names[state];
}

bool is_activate_key(const Message* msg) {
    return msg->key == k_key_enter || msg->key == k_key_space;
}

Handle current_state_handle(const Handle* handles, const Window* self) {
    const int32_t state = self->states.current;
    return state >= 0 && state < self->states.count ? handles[state] : k_no_handle;
}

// Image of the current state, or the background colour.
void draw_face(Window* self) {
    const Handle image = current_state_handle(self->states.images, self);
    if (image < 0) {
        color_fill(self->x, self->y, self->width, self->height, self->color);
    } else {
        image_draw(self->x, self->y, self->width, self->height, image);
    }
}

void destroy_record(Window* self) {
    window_release(self);
    std::free(self);
}

template <typename T>
T* allocate(const WindowOps* ops) {
    auto* record = static_cast<T*>(std::calloc(1, sizeof(T)));
    if (record == nullptr) {
        std::abort();
    }
    window_init(reinterpret_cast<Window*>(record), ops);
    return record;
}

bool ignore_message(Window*, const Message*) {
    return false;
}

// ---------------------------------------------------------------------------
// Gadget

/// Tests whether a class name is "Gadget".
///
/// @param name class name to test; may be null
/// @return true when `name` is "Gadget"
bool gadget_is_class(const char* name) {
    return names_equal(name, class_name::gadget);
}

int32_t gadget_type_code() {
    return 0;
}

constexpr WindowOps make_gadget_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::gadget;
    ops.destroy = destroy_record;
    ops.is_class = gadget_is_class;
    ops.type_code = gadget_type_code;
    ops.raise = detail::gadget_raise;
    ops.draw = window_draw_background;
    return ops;
}

// ---------------------------------------------------------------------------
// Button

/// Looks up a button state name ("Disabled", "Pressed", "Unpressed").
///
/// @param self button whose state count bounds the search
/// @param name state name, compared case-sensitively; may be null
/// @return the state index, or -1 for a null or unknown name
int32_t button_state_index(Window* self, const char* name) {
    return lookup_state(self, k_button_state_names, name);
}

/// Returns the name of a button state.
///
/// @param self button whose state count bounds the lookup
/// @param state state index
/// @return "Disabled", "Pressed" or "Unpressed", or null outside the button's
///     states
const char* button_state_name(Window* self, int32_t state) {
    return state_label(self, k_button_state_names, state);
}

/// Tests whether a class name is "Button".
///
/// @param name class name to test; may be null
/// @return true when `name` is "Button"
bool button_is_class(const char* name) {
    return names_equal(name, class_name::button);
}

int32_t zero_type_code() {
    return 0;
}

/// Sets the caption shown in the Unpressed and Pressed states.
///
/// @param[in,out] self button whose captions change
/// @param text caption; null or "" clears it
/// @param style text_style bits for the caption
void button_set_caption(Window* self, const char* text, int32_t style) {
    window_set_state_text(self, button_state::unpressed, text, style);
    window_set_state_text(self, button_state::pressed, text, style);
}

/// Plays the activate sound and reports NotifyCode::activated to the parent.
///
/// @param self button that was used
void button_activate(Window* self) {
    window_play_sound(self, 2);
    window_notify_parent(self, NotifyCode::activated, 0);
}

/// Draws a button: its face, its caption and, while focused, the focus frame.
///
/// The face is the current state's image, or else the background colour. The
/// caption moves one pixel right and down while Pressed. A hidden button draws
/// nothing; children are not drawn.
///
/// @param self button to draw
/// @return true (handled)
bool button_draw(Window* self) {
    if (window_is_shown(self)) {
        draw_face(self);
        const Handle text = current_state_handle(self->states.texts, self);
        if (text >= 0) {
            const int32_t shift = self->states.current == button_state::pressed ? 1 : 0;
            text_draw(
                self->font, self->x + shift, self->y + shift, self->width, self->height, text
            );
        }
        if (self->focused != 0) {
            draw_focus_frame(self->x, self->y, self->width, self->height);
        }
    }
    return detail::window_plain_draw(self);
}

/// Activates an enabled button on Enter or Space; other keys, and any key on
/// a disabled button, take the default key handling.
///
/// @param self button receiving the key
/// @param msg key_down message; null is not handled
/// @return true when the button was activated, else the default key_down
///     result
bool button_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (self->states.current != button_state::disabled && is_activate_key(msg)) {
        button_activate(self);
        return true;
    }
    return window_default_key_down(self, msg);
}

/// Presses the button: captures the pointer, shows Pressed and plays the press
/// sound.
///
/// A disabled button only plays its disabled click; a button already Pressed
/// is left alone.
///
/// @param[in,out] self button being pressed
/// @param msg pointer message (unused)
/// @return true (handled)
bool button_left_down(Window* self, const Message* /*msg*/) {
    if (self->states.current == button_state::pressed) {
        return true;
    }
    if (self->states.current == button_state::disabled) {
        window_play_sound(self, 0);
        return true;
    }
    services_desktop_capture(self);
    window_set_state(self, button_state::pressed);
    window_play_sound(self, 1);
    return true;
}

/// Releases the pointer capture; a Pressed button returns to Unpressed and
/// activates when the pointer is still over it.
///
/// @param[in,out] self button being released
/// @param msg left_up message carrying the pointer position; null is not
///     handled
/// @return true (handled), or false for a null message
bool button_left_up(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    services_desktop_release_capture();
    if (self->states.current == button_state::pressed) {
        window_set_state(self, button_state::unpressed);
        if (self->ops->contains(self, msg->x, msg->y)) {
            button_activate(self);
        }
    }
    return true;
}

/// Tracks a held pointer the button has captured: Pressed while over the
/// button, Unpressed elsewhere; a disabled button drops the capture.
///
/// The default mouse_move handling runs afterwards in every case.
///
/// @param[in,out] self button receiving the move
/// @param msg mouse_move message with the pointer position and held buttons;
///     null is not handled
/// @return the default mouse_move result
bool button_mouse_move(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if ((msg->buttons & button_any) != 0 && services_desktop_captured() == self) {
        if (self->states.current == button_state::disabled) {
            services_desktop_release_capture();
            return window_default_mouse_move(self, msg);
        }
        if (self->ops->contains(self, msg->x, msg->y)) {
            window_set_state(self, button_state::pressed);
            return window_default_mouse_move(self, msg);
        }
        window_set_state(self, button_state::unpressed);
    }
    return window_default_mouse_move(self, msg);
}

constexpr WindowOps make_button_ops() {
    WindowOps ops = make_gadget_ops();
    ops.class_name = class_name::button;
    ops.set_caption = button_set_caption;
    ops.state_index = button_state_index;
    ops.state_name = button_state_name;
    ops.is_class = button_is_class;
    ops.type_code = zero_type_code;
    ops.draw = button_draw;
    ops.key_down = button_key_down;
    ops.left_down = button_left_down;
    ops.left_up = button_left_up;
    ops.left_double = forward_left_down;
    ops.right_down = forward_left_down;
    ops.right_up = forward_left_up;
    ops.right_double = forward_left_down;
    ops.mouse_move = button_mouse_move;
    return ops;
}

// ---------------------------------------------------------------------------
// NoFocusBtn and SButton

/// Tests whether a class name is "NoFocusBtn".
///
/// @param name class name to test; may be null
/// @return true when `name` is "NoFocusBtn"
bool no_focus_button_is_class(const char* name) {
    return names_equal(name, class_name::no_focus_button);
}

/// Passes a focus request on to the parent; the button never takes the focus
/// itself.
///
/// @param self button asked for a focus target
/// @param from window the focus is moving away from; a request that came from
///     the parent is not passed back
/// @param backwards true for Shift+Tab
/// @return the parent's focus target, or null
Window* no_focus_button_focus_target(Window* self, Window* from, bool backwards) {
    Window* parent = self->parent;
    if (parent != nullptr && parent != from) {
        return parent->ops->focus_target(parent, self, backwards);
    }
    return nullptr;
}

constexpr WindowOps make_no_focus_button_ops() {
    WindowOps ops = make_button_ops();
    ops.class_name = class_name::no_focus_button;
    ops.is_class = no_focus_button_is_class;
    ops.focus_target = no_focus_button_focus_target;
    return ops;
}

/// Tests whether a class name is "SButton".
///
/// @param name class name to test; may be null
/// @return true when `name` is "SButton"
bool scroll_button_is_class(const char* name) {
    return names_equal(name, class_name::scroll_button);
}

/// Repeats the activation on every pulse while the button is held Pressed.
///
/// @param self scroll button being pulsed
void scroll_button_pulse(Window* self) {
    if (self->states.current == button_state::pressed) {
        button_activate(self);
    }
}

/// Presses the scroll button like a button press and activates it at once.
///
/// A disabled button only plays its disabled click; a button already Pressed
/// is left alone.
///
/// @param[in,out] self scroll button being pressed
/// @param msg pointer message (unused)
/// @return true (handled)
bool scroll_button_left_down(Window* self, const Message* /*msg*/) {
    if (self->states.current == button_state::pressed) {
        return true;
    }
    if (self->states.current == button_state::disabled) {
        window_play_sound(self, 0);
        return true;
    }
    services_desktop_capture(self);
    window_set_state(self, button_state::pressed);
    window_play_sound(self, 1);
    button_activate(self);
    return true;
}

/// Releases the pointer capture, reports NotifyCode::scroll_released to the
/// parent and returns a Pressed button to Unpressed.
///
/// @param[in,out] self scroll button being released
/// @param msg left_up message; null is not handled
/// @return true (handled), or false for a null message
bool scroll_button_left_up(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    services_desktop_release_capture();
    window_notify_parent(self, NotifyCode::scroll_released, 0);
    if (self->states.current == button_state::pressed) {
        window_set_state(self, button_state::unpressed);
    }
    return true;
}

/// Tracks a held pointer the button has captured and reports each move to the
/// parent as a drag.
///
/// The button shows Pressed while the pointer is over it and Unpressed
/// elsewhere, and the parent's on_notify receives NotifyCode::dragged with
/// the message. A disabled button drops the capture and takes the default
/// mouse_move handling instead.
///
/// @param[in,out] self scroll button receiving the move
/// @param msg mouse_move message with the pointer position and held buttons;
///     null is not handled
/// @return true; for a disabled button the default mouse_move result; false
///     for a null message
bool scroll_button_mouse_move(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if ((msg->buttons & button_any) != 0 && services_desktop_captured() == self) {
        if (self->states.current == button_state::disabled) {
            services_desktop_release_capture();
            return window_default_mouse_move(self, msg);
        }
        const bool inside = self->ops->contains(self, msg->x, msg->y);
        window_set_state(self, inside ? button_state::pressed : button_state::unpressed);
        if (self->parent != nullptr) {
            Notify notify{NotifyCode::dragged, self, 0, msg};
            self->parent->ops->on_notify(self->parent, &notify);
        }
    }
    return true;
}

constexpr WindowOps make_scroll_button_ops() {
    WindowOps ops = make_no_focus_button_ops();
    ops.class_name = class_name::scroll_button;
    ops.is_class = scroll_button_is_class;
    ops.pulse = scroll_button_pulse;
    ops.left_down = scroll_button_left_down;
    ops.left_up = scroll_button_left_up;
    ops.mouse_move = scroll_button_mouse_move;
    return ops;
}

// ---------------------------------------------------------------------------
// Static

/// Tests whether a class name is "Static".
///
/// @param name class name to test; may be null
/// @return true when `name` is "Static"
bool static_is_class(const char* name) {
    return names_equal(name, class_name::static_text);
}

/// Sets the caption of the Disabled and Normal states.
///
/// @param[in,out] self label whose captions change
/// @param text caption; null or "" clears it
/// @param style text_style bits for the caption
void static_set_caption(Window* self, const char* text, int32_t style) {
    window_set_state_text(self, 0, text, style);
    window_set_state_text(self, 1, text, style);
}

/// Draws a label: the current state's image and caption, with no background
/// fill and no focus frame.
///
/// A hidden label draws nothing; children are not drawn.
///
/// @param self label to draw
/// @return true (handled)
bool static_draw(Window* self) {
    if (window_is_shown(self)) {
        const Handle image = current_state_handle(self->states.images, self);
        const Handle text = current_state_handle(self->states.texts, self);
        if (image >= 0) {
            image_draw(self->x, self->y, self->width, self->height, image);
        }
        if (text >= 0 && !modern_chat_paint_window(self, text_get(text))) {
            text_draw(self->font, self->x, self->y, self->width, self->height, text);
        }
    }
    return true;
}

/// Reports a press on the label to the parent as NotifyCode::activated.
///
/// @param self label pressed
/// @param msg pointer message (unused)
/// @return true (handled)
bool static_left_down(Window* self, const Message* /*msg*/) {
    window_notify_parent(self, NotifyCode::activated, 0);
    return true;
}

/// Reports a double click on the label to the parent as NotifyCode::changed.
///
/// @param self label double-clicked
/// @param msg pointer message (unused)
/// @return true (handled)
bool static_left_double(Window* self, const Message* /*msg*/) {
    window_notify_parent(self, NotifyCode::changed, 0);
    return true;
}

constexpr WindowOps make_static_ops() {
    WindowOps ops = make_gadget_ops();
    ops.class_name = class_name::static_text;
    ops.set_caption = static_set_caption;
    ops.is_class = static_is_class;
    ops.type_code = zero_type_code;
    ops.draw = static_draw;
    ops.left_down = static_left_down;
    ops.left_double = static_left_double;
    ops.right_down = forward_left_down;
    ops.right_double = ignore_message;
    return ops;
}

// ---------------------------------------------------------------------------
// CheckBox

/// Tests whether a class name is "CheckBox".
///
/// @param name class name to test; may be null
/// @return true when `name` is "CheckBox"
bool check_box_is_class(const char* name) {
    return names_equal(name, class_name::check_box);
}

/// Looks up a check box state name ("Disabled", "Unchecked Down", "Checked
/// Down", "Unchecked Up", "Checked Up").
///
/// @param self check box whose state count bounds the search
/// @param name state name, compared case-sensitively; may be null
/// @return the state index, or -1 for a null or unknown name
int32_t check_box_state_index(Window* self, const char* name) {
    return lookup_state(self, k_check_state_names, name);
}

/// Returns the name of a check box state.
///
/// @param self check box whose state count bounds the lookup
/// @param state state index
/// @return the state's name, or null outside the box's states
const char* check_box_state_name(Window* self, int32_t state) {
    return state_label(self, k_check_state_names, state);
}

/// Sets the caption of all five check box states.
///
/// @param[in,out] self check box whose captions change
/// @param text caption; null or "" clears it
/// @param style text_style bits for the caption
void check_box_set_caption(Window* self, const char* text, int32_t style) {
    for (int32_t state = 0; state < k_check_box_states; ++state) {
        window_set_state_text(self, state, text, style);
    }
}

/// Reports a toggle to the parent as NotifyCode::activated, with value 1 when
/// the box is now Checked Up and 0 otherwise.
///
/// @param self check box that was toggled
void check_box_notify(Window* self) {
    window_notify_parent(
        self, NotifyCode::activated, self->states.current == check_state::checked_up ? 1 : 0
    );
}

/// Draws a check box: the box image vertically centred at the left edge, the
/// caption `label_gap` pixels after it and, while focused, the focus frame.
///
/// A hidden check box draws nothing; children are not drawn.
///
/// @param self check box (a CheckBox record) to draw
/// @return true (handled)
bool check_box_draw(Window* self) {
    if (window_is_shown(self)) {
        auto* box = reinterpret_cast<CheckBox*>(self);
        const Handle image = current_state_handle(self->states.images, self);
        const Handle text = current_state_handle(self->states.texts, self);
        int32_t image_w = 0;
        int32_t image_h = 0;
        if (image >= 0) {
            image_size(image, &image_w, &image_h);
            image_draw(self->x, (self->height - image_h) / 2 + self->y, image_w, image_h, image);
        }
        if (text >= 0) {
            text_draw(
                self->font,
                box->label_gap + image_w + self->x,
                self->y,
                (self->width - box->label_gap) - image_w,
                self->height,
                text
            );
        }
        if (self->focused != 0) {
            draw_focus_frame(self->x, self->y, self->width, self->height);
        }
    }
    return true;
}

/// Toggles the box on Enter or Space and reports the toggle; other keys take
/// the default key handling.
///
/// A checked box (up or down) becomes Unchecked Up and an unchecked one
/// becomes Checked Up. A disabled box keeps its state but still reports.
///
/// @param[in,out] self check box receiving the key
/// @param msg key_down message; null is not handled
/// @return true for Enter or Space, else the default key_down result
bool check_box_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (!is_activate_key(msg)) {
        return window_default_key_down(self, msg);
    }
    const int32_t state = self->states.current;
    if (state == check_state::checked_up || state == check_state::checked_down) {
        window_set_state(self, check_state::unchecked_up);
    } else if (state == check_state::unchecked_up || state == check_state::unchecked_down) {
        window_set_state(self, check_state::checked_up);
    }
    check_box_notify(self);
    return true;
}

/// Presses the box: captures the pointer and shows Checked Down when the box
/// was Checked Up, Unchecked Down otherwise.
///
/// A disabled box only plays its disabled click.
///
/// @param[in,out] self check box being pressed
/// @param msg pointer message (unused)
/// @return true (handled)
bool check_box_left_down(Window* self, const Message* /*msg*/) {
    if (self->states.current == check_state::disabled) {
        window_play_sound(self, 0);
        return true;
    }
    const bool checked = self->states.current == check_state::checked_up;
    services_desktop_capture(self);
    window_set_state(self, checked ? check_state::checked_down : check_state::unchecked_down);
    return true;
}

/// Releases the pointer capture and toggles the box when released over it.
///
/// Over the box, Unchecked Down becomes Checked Up and Checked Down becomes
/// Unchecked Up, and the toggle is reported. Elsewhere a down state returns
/// to its up state unchanged. A disabled box only drops the capture.
///
/// @param[in,out] self check box being released
/// @param msg left_up message carrying the pointer position; null is not
///     handled
/// @return true (handled), or false for a null message
bool check_box_left_up(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    const int32_t old = self->states.current;
    services_desktop_release_capture();
    if (self->states.current == check_state::disabled) {
        return true;
    }
    if (self->ops->contains(self, msg->x, msg->y)) {
        if (old == check_state::unchecked_down) {
            window_set_state(self, check_state::checked_up);
        } else if (old == check_state::checked_down) {
            window_set_state(self, check_state::unchecked_up);
        }
        check_box_notify(self);
        return true;
    }
    if (old == check_state::unchecked_down) {
        window_set_state(self, check_state::unchecked_up);
    } else if (old == check_state::checked_down) {
        window_set_state(self, check_state::checked_up);
    }
    return true;
}

/// Tracks a held pointer the box has captured: the down state of the current
/// check while over the box, the up state elsewhere.
///
/// The default mouse_move handling runs afterwards in every case.
///
/// @param[in,out] self check box receiving the move
/// @param msg mouse_move message with the pointer position and held buttons;
///     null is not handled
/// @return the default mouse_move result
bool check_box_mouse_move(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    int32_t state = self->states.current;
    if ((msg->buttons & button_any) != 0 && services_desktop_captured() == self) {
        if (self->ops->contains(self, msg->x, msg->y)) {
            if (state == check_state::unchecked_up) {
                state = check_state::unchecked_down;
            } else if (state == check_state::checked_up) {
                state = check_state::checked_down;
            }
        } else if (state == check_state::unchecked_down) {
            state = check_state::unchecked_up;
        } else if (state == check_state::checked_down) {
            state = check_state::checked_up;
        }
        window_set_state(self, state);
    }
    return window_default_mouse_move(self, msg);
}

void check_box_destroy(Window* self) {
    destroy_record(self);
}

constexpr WindowOps make_check_box_ops() {
    WindowOps ops = make_gadget_ops();
    ops.class_name = class_name::check_box;
    ops.destroy = check_box_destroy;
    ops.set_caption = check_box_set_caption;
    ops.state_index = check_box_state_index;
    ops.state_name = check_box_state_name;
    ops.is_class = check_box_is_class;
    ops.type_code = zero_type_code;
    ops.draw = check_box_draw;
    ops.key_down = check_box_key_down;
    ops.left_down = check_box_left_down;
    ops.left_up = check_box_left_up;
    ops.left_double = forward_left_down;
    ops.right_down = forward_left_down;
    ops.right_up = forward_left_up;
    ops.right_double = forward_right_down;
    ops.mouse_move = check_box_mouse_move;
    return ops;
}

// ---------------------------------------------------------------------------
// RadioButton

/// Tests whether a class name is "RadioButton".
///
/// @param name class name to test; may be null
/// @return true when `name` is "RadioButton"
bool radio_is_class(const char* name) {
    return names_equal(name, class_name::radio_button);
}

/// Looks up a radio button state name (the button names "Disabled",
/// "Pressed", "Unpressed").
///
/// @param self radio button whose state count bounds the search
/// @param name state name, compared case-sensitively; may be null
/// @return the state index, or -1 for a null or unknown name
int32_t radio_state_index(Window* self, const char* name) {
    return lookup_state(self, k_button_state_names, name);
}

/// Returns the name of a radio button state.
///
/// @param self radio button whose state count bounds the lookup
/// @param state state index
/// @return "Disabled", "Pressed" or "Unpressed", or null outside the radio's
///     states
const char* radio_state_name(Window* self, int32_t state) {
    return state_label(self, k_button_state_names, state);
}

/// Reports the radio button's selection to its group, then plays the
/// activate sound.
///
/// The parent's on_notify receives NotifyCode::activated and every sibling's
/// on_notify NotifyCode::radio_selected. The radio's own state is left to the
/// caller.
///
/// @param self radio button being selected
void radio_select(Window* self) {
    Window* parent = self->parent;
    if (parent != nullptr) {
        Notify notify{NotifyCode::activated, self, 0, nullptr};
        parent->ops->on_notify(parent, &notify);
        notify.code = NotifyCode::radio_selected;
        for (int32_t i = 0; i < parent->children.count; ++i) {
            Window* sibling = parent->children.items[i];
            if (sibling != nullptr && sibling != self) {
                sibling->ops->on_notify(sibling, &notify);
            }
        }
    }
    window_play_sound(self, 2);
}

/// Deselects the radio (Pressed to Unpressed) when another radio of its group
/// reports NotifyCode::radio_selected.
///
/// @param[in,out] self radio button receiving the event
/// @param notify event from a sibling; null or any other event is ignored
void radio_on_notify(Window* self, const Notify* notify) {
    if (notify != nullptr && notify->code == NotifyCode::radio_selected && notify->source != self &&
        self->states.current == button_state::pressed) {
        window_set_state(self, button_state::unpressed);
    }
}

/// Selects an enabled, unselected radio button on Enter or Space; other keys
/// take the default key handling.
///
/// @param[in,out] self radio button receiving the key
/// @param msg key_down message; null is not handled
/// @return true when the radio was selected, else the default key_down result
bool radio_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (self->states.current != button_state::disabled && is_activate_key(msg) &&
        self->states.current != button_state::pressed) {
        window_set_state(self, button_state::pressed);
        radio_select(self);
        return true;
    }
    return window_default_key_down(self, msg);
}

/// Selects the radio when released over it while it holds the pointer
/// capture, then releases the capture.
///
/// A disabled radio does nothing.
///
/// @param self radio button being released
/// @param msg left_up message carrying the pointer position; null is not
///     handled
/// @return true (handled), or false for a null message
bool radio_left_up(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (self->states.current != button_state::disabled) {
        if (self->ops->contains(self, msg->x, msg->y) && services_desktop_captured() == self) {
            radio_select(self);
        }
        services_desktop_release_capture();
    }
    return true;
}

constexpr WindowOps make_radio_ops() {
    WindowOps ops = make_gadget_ops();
    ops.class_name = class_name::radio_button;
    ops.set_caption = button_set_caption;
    ops.state_index = radio_state_index;
    ops.state_name = radio_state_name;
    ops.is_class = radio_is_class;
    ops.type_code = zero_type_code;
    ops.on_notify = radio_on_notify;
    ops.draw = button_draw;
    ops.key_down = radio_key_down;
    ops.left_down = button_left_down;
    ops.left_up = radio_left_up;
    ops.right_down = forward_left_down;
    ops.right_up = forward_left_up;
    ops.mouse_move = button_mouse_move;
    return ops;
}

// ---------------------------------------------------------------------------
// StageBtn

bool stage_is_class(const char* name) {
    return names_equal(name, class_name::stage_button);
}

int32_t stage_type_code() {
    return 0;
}

int32_t stage_state_index(Window* self, const char* name) {
    return lookup_state(self, k_stage_state_names, name);
}

const char* stage_state_name(Window* self, int32_t state) {
    return state_label(self, k_stage_state_names, state);
}

/// Moves the stage button to the stage after `from`, reports
/// NotifyCode::activated and plays the activate sound.
///
/// Past the last state the button wraps to First (state 2).
///
/// @param[in,out] self stage button to advance
/// @param from stage index to advance from
void stage_advance(Window* self, int32_t from) {
    int32_t next = from + 1;
    if (self->states.count - 1 < next) {
        next = k_stage_first;
    }
    window_set_state(self, next);
    window_notify_parent(self, NotifyCode::activated, 0);
    window_play_sound(self, 2);
}

/// Draws a stage button: its face, its caption inset 5 pixels and, while
/// focused, the focus frame.
///
/// When the current state has no caption, the caption saved at the press
/// (the stage being left) is drawn. The caption moves one pixel right and
/// down while Pressed. A hidden button draws nothing; children are not drawn.
///
/// @param self stage button (a StageButton record) to draw
/// @return true (handled)
bool stage_draw(Window* self) {
    if (window_is_shown(self)) {
        auto* stage = reinterpret_cast<StageButton*>(self);
        draw_face(self);
        Handle text = current_state_handle(self->states.texts, self);
        bool skip = false;
        if (text == -1) {
            text = stage->saved_text;
            skip = text == -1;
        }
        if (!skip && text > -2) {
            const bool pressed = detail::window_is_pressed(self);
            text_draw(
                self->font,
                self->x + k_stage_text_inset + (pressed ? 1 : 0),
                self->y + (pressed ? 1 : 0),
                self->width,
                self->height,
                text
            );
        }
        if (self->focused != 0) {
            draw_focus_frame(self->x, self->y, self->width, self->height);
        }
    }
    return true;
}

/// Advances the stage on Enter or Space; other keys take the default key
/// handling.
///
/// @param[in,out] self stage button receiving the key
/// @param msg key message; null is not handled
/// @return true when the stage advanced, else the default key_down result
bool stage_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (msg->code == MessageCode::key_down && is_activate_key(msg)) {
        stage_advance(self, self->states.current);
        return true;
    }
    return window_default_key_down(self, msg);
}

/// Presses the stage button: captures the pointer, saves the current stage
/// and its caption, shows Pressed and plays the press sound.
///
/// A disabled button only plays its disabled click; a button already Pressed
/// is left alone.
///
/// @param[in,out] self stage button (a StageButton record) being pressed
/// @param msg pointer message (unused)
/// @return true (handled)
bool stage_left_down(Window* self, const Message* /*msg*/) {
    if (detail::window_is_pressed(self)) {
        return true;
    }
    if (self->states.current == button_state::disabled) {
        window_play_sound(self, 0);
        return true;
    }
    auto* stage = reinterpret_cast<StageButton*>(self);
    services_desktop_capture(self);
    stage->saved_text = current_state_handle(self->states.texts, self);
    stage->saved_state = self->states.current;
    window_set_state(self, button_state::pressed);
    window_play_sound(self, 1);
    return true;
}

/// Releases the pointer capture and restores the saved stage, advancing from
/// it when released over the button.
///
/// A disabled button only drops the capture.
///
/// @param[in,out] self stage button (a StageButton record) being released
/// @param msg left_up message carrying the pointer position; null is not
///     handled
/// @return true (handled), or false for a null message
bool stage_left_up(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    auto* stage = reinterpret_cast<StageButton*>(self);
    const int32_t saved = stage->saved_state;
    services_desktop_release_capture();
    if (self->states.current != button_state::disabled) {
        window_set_state(self, saved);
        if (self->ops->contains(self, msg->x, msg->y)) {
            stage_advance(self, saved);
        }
    }
    return true;
}

/// Tracks a held pointer the button has captured: Pressed while over the
/// button, the saved stage elsewhere.
///
/// The default mouse_move handling runs afterwards in every case.
///
/// @param[in,out] self stage button (a StageButton record) receiving the move
/// @param msg mouse_move message with the pointer position and held buttons;
///     null is not handled
/// @return the default mouse_move result
bool stage_mouse_move(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if ((msg->buttons & button_any) != 0 && services_desktop_captured() == self) {
        if (self->ops->contains(self, msg->x, msg->y)) {
            window_set_state(self, button_state::pressed);
        } else {
            window_set_state(self, reinterpret_cast<StageButton*>(self)->saved_state);
        }
    }
    return window_default_mouse_move(self, msg);
}

constexpr WindowOps make_stage_ops() {
    WindowOps ops = make_gadget_ops();
    ops.class_name = class_name::stage_button;
    ops.state_index = stage_state_index;
    ops.state_name = stage_state_name;
    ops.is_class = stage_is_class;
    ops.type_code = stage_type_code;
    ops.draw = stage_draw;
    ops.key_down = stage_key_down;
    ops.left_down = stage_left_down;
    ops.left_up = stage_left_up;
    ops.left_double = forward_left_down;
    ops.right_down = forward_left_down;
    ops.right_up = forward_left_up;
    ops.right_double = forward_left_down;
    ops.mouse_move = stage_mouse_move;
    return ops;
}

// ---------------------------------------------------------------------------
// Progress

bool progress_is_class(const char* name) {
    return names_equal(name, class_name::progress);
}

int32_t progress_type_code() {
    return 0;
}

int32_t progress_state_index(Window* self, const char* name) {
    return lookup_state(self, k_progress_state_names, name);
}

const char* progress_state_name(Window* self, int32_t state) {
    return state_label(self, k_progress_state_names, state);
}

/// Fills value / range of the bar's long axis with the background colour.
///
/// A bar wider than tall fills from the left edge (the right edge when
/// `reverse` is set); otherwise it fills from the bottom edge. The fill is
/// clamped to the bar's length. Nothing is drawn while the bar is hidden or
/// its value or range is zero.
///
/// @param self progress bar (a Progress record) to draw
/// @return true (handled)
/// @quirk A reversed vertical bar is drawn one fill length higher, so its band
///     ends `fill` pixels above the bottom edge instead of starting at the
///     top edge.
bool progress_draw(Window* self) {
    auto* bar = reinterpret_cast<Progress*>(self);
    if (!window_is_shown(self) || bar->value == 0 || bar->range == 0) {
        return true;
    }
    const int32_t w = self->width;
    const int32_t h = self->height;
    int32_t x = self->x;
    int32_t y = self->y;
    if (h < w) {
        int32_t fill = static_cast<int32_t>(static_cast<double>(bar->value * w) / bar->range);
        if (w < fill) {
            fill = w;
        }
        if (bar->reverse != 0) {
            x = (x - fill) + w;
        }
        color_fill(x, y, fill, h, self->color);
        return true;
    }
    int32_t fill = static_cast<int32_t>(static_cast<double>(bar->value * h) / bar->range);
    if (h < fill) {
        fill = h;
    }
    if (bar->reverse != 0) {
        y -= fill;
    }
    color_fill(x, (y + h) - fill, w, fill, self->color);
    return true;
}

constexpr WindowOps make_progress_ops() {
    WindowOps ops = make_gadget_ops();
    ops.class_name = class_name::progress;
    ops.state_index = progress_state_index;
    ops.state_name = progress_state_name;
    ops.is_class = progress_is_class;
    ops.type_code = progress_type_code;
    ops.draw = progress_draw;
    return ops;
}

} // namespace

constinit const WindowOps k_gadget_ops = make_gadget_ops();
constinit const WindowOps k_button_ops = make_button_ops();
constinit const WindowOps k_no_focus_button_ops = make_no_focus_button_ops();
constinit const WindowOps k_scroll_button_ops = make_scroll_button_ops();
constinit const WindowOps k_static_ops = make_static_ops();
constinit const WindowOps k_check_box_ops = make_check_box_ops();
constinit const WindowOps k_radio_button_ops = make_radio_ops();
constinit const WindowOps k_stage_button_ops = make_stage_ops();
constinit const WindowOps k_progress_ops = make_progress_ops();

Window* gadget_create() {
    return allocate<Window>(&k_gadget_ops);
}

Window* button_create() {
    Window* w = allocate<Window>(&k_button_ops);
    window_set_state_count(w, k_button_states);
    window_set_state(w, button_state::unpressed);
    return w;
}

Window* no_focus_button_create() {
    Window* w = button_create();
    w->ops = &k_no_focus_button_ops;
    return w;
}

Window* scroll_button_create() {
    Window* w = button_create();
    w->ops = &k_scroll_button_ops;
    return w;
}

Window* static_create() {
    Window* w = allocate<Window>(&k_static_ops);
    w->focusable = 0;
    return w;
}

CheckBox* check_box_create() {
    auto* box = allocate<CheckBox>(&k_check_box_ops);
    window_set_state_count(&box->base, k_check_box_states);
    window_set_state(&box->base, check_state::unchecked_up);
    box->label_gap = k_check_box_label_gap;
    return box;
}

Window* radio_button_create() {
    Window* w = allocate<Window>(&k_radio_button_ops);
    window_set_state_count(w, k_button_states);
    window_set_state(w, button_state::unpressed);
    return w;
}

StageButton* stage_button_create() {
    auto* stage = allocate<StageButton>(&k_stage_button_ops);
    stage->saved_text = k_no_handle;
    return stage;
}

Progress* progress_create() {
    return allocate<Progress>(&k_progress_ops);
}

bool check_box_checked(const CheckBox* box) {
    const int32_t state = box->base.states.current;
    return state == check_state::checked_up || state == check_state::checked_down;
}

void progress_set(Progress* progress, int32_t value, int32_t range) {
    progress->value = value;
    progress->range = range;
}

} // namespace oa::boneyards
