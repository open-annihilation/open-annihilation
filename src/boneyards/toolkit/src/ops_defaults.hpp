// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Default dispatch-table slots of the plain Window class, shared so derived
// tables can be built as constant expressions.
#pragma once

#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards::detail {

/// Frees a heap window record: runs window_release, then frees the record
/// (the plain Window's destroy slot).
///
/// @param self window record to free
void window_plain_destroy(Window* self);
/// Returns the text of one of the window's states.
///
/// @param self window whose state texts are read
/// @param state state index, or -1 for the current state
/// @return the state's text; "" when the state is out of range or has no text
const char* window_plain_state_text(Window* self, int32_t state);
/// Looks up a base state name ("Disabled", "Normal").
///
/// Only the first min(states.count, 2) names are searched.
///
/// @param self window whose state count bounds the search
/// @param state_name state name to find, compared case-sensitively; may be null
/// @return the state index, or -1 for a null or unknown name
int32_t window_plain_state_index(Window* self, const char* state_name);
/// Returns the base name of a state.
///
/// @param self window whose state count bounds the lookup
/// @param state state index
/// @return "Disabled" or "Normal", or null when the state is outside both the
///     window's states and the two base names
const char* window_plain_state_name(Window* self, int32_t state);
/// Tests whether a class name is the plain window class, "Window".
///
/// @param name class name to test; may be null
/// @return true when `name` is "Window"
bool window_plain_is_class(const char* name);
/// Loads consecutive frames of a sequence as the images of every state.
///
/// State i gets frame `first + i`; each previous image is released first. A
/// null or empty `file` only releases the images and leaves every state
/// without one; a null `sequence` changes nothing.
///
/// @param[in,out] self window whose state images are replaced
/// @param file GAF file holding the frames; null or "" clears the images
/// @param sequence sequence name within `file` ("" = first sequence)
/// @param first frame index given to state 0
/// @param key transparency key stored with each loaded image (9 = the
///     transparent index)
/// @return the frame index after the last one loaded; `first` when nothing
///     was loaded
int32_t window_plain_bind_images(
    Window* self, const char* file, const char* sequence, int32_t first, int32_t key
);
/// Returns the plain Window class's type code, which nothing in the engine
/// reads.
///
/// @return the class's constant
int32_t window_plain_type_code();
/// Does nothing (the default for slots with no result).
///
/// @param self window receiving the call (unused)
void noop_window(Window* self);
/// Ignores an event from a child (the default on_notify slot).
///
/// @param self window receiving the event (unused)
/// @param notify event (unused)
void noop_notify(Window* self, const Notify* notify);
/// Does nothing and reports the call unhandled (the default for refresh,
/// tick and the reserved_2 and reserved_3 messages).
///
/// @param self window receiving the call (unused)
/// @return false
bool false_window(Window* self);
/// Finishes a draw without drawing anything; derived draw slots end with it.
///
/// @param self window being drawn (unused)
/// @return true (handled)
bool window_plain_draw(Window* self);
/// Leaves a message unhandled (the default for most pointer slots).
///
/// @param self window receiving the message (unused)
/// @param msg message (unused)
/// @return false
bool false_message(Window* self, const Message* msg);
/// Ignores a caption (the plain Window's set_caption slot).
///
/// @param self window receiving the caption (unused)
/// @param text caption (unused)
/// @param style text_style bits (unused)
void noop_caption(Window* self, const char* text, int32_t style);
/// Raises a gadget by raising its parent; the gadget keeps its place among
/// its siblings.
///
/// A top-level gadget is raised on the installed desktop instead.
///
/// @param self gadget to raise
void gadget_raise(Window* self);
/// Marks the window focused and raises it through its raise slot.
///
/// @param[in,out] self window gaining the keyboard focus
/// @return true (handled)
bool window_plain_focus_gained(Window* self);
/// Clears the window's focused flag.
///
/// @param[in,out] self window losing the keyboard focus
/// @return true (handled)
bool window_plain_focus_lost(Window* self);

/// Handles a message through the window's own left_down slot (used for right,
/// middle and double presses that act like a left press).
///
/// @param self window receiving the message
/// @param msg pointer message, passed on unchanged
/// @return the left_down slot's result
bool forward_left_down(Window* self, const Message* msg);
/// Handles a message through the window's own left_up slot (used for right
/// and middle releases that act like a left release).
///
/// @param self window receiving the message
/// @param msg pointer message, passed on unchanged
/// @return the left_up slot's result
bool forward_left_up(Window* self, const Message* msg);
/// Handles a message through the window's own right_down slot (a check box's
/// right double click counts as a right press).
///
/// @param self window receiving the message
/// @param msg pointer message, passed on unchanged
/// @return the right_down slot's result
bool forward_right_down(Window* self, const Message* msg);

/// Tests whether the window shows its Pressed state.
///
/// The Slider's own pressed-state test; the stage button calls it
/// directly.
///
/// @param self window to test
/// @return true while the current state is button_state::pressed
bool window_is_pressed(const Window* self);

/// Builds the plain Window class's dispatch table as a constant expression.
///
/// @return the table every derived class starts from
constexpr WindowOps plain_window_ops() {
    return WindowOps{
        class_name::window,
        window_plain_destroy,
        noop_caption,
        window_plain_state_text,
        window_plain_state_index,
        window_plain_state_name,
        window_plain_is_class,
        window_plain_bind_images,
        window_plain_type_code,
        window_default_focus_target,
        window_default_raise,
        window_default_contains,
        window_default_child_at,
        noop_window,
        noop_window,
        {noop_window},
        noop_notify,
        window_plain_draw,
        false_window,
        false_window,
        false_window,
        false_window,
        window_plain_focus_gained,
        window_plain_focus_lost,
        noop_window,
        noop_window,
        window_default_key_down,
        window_default_key_up,
        false_message,
        false_message,
        false_message,
        false_message,
        false_message,
        false_message,
        false_message,
        false_message,
        false_message,
        window_default_mouse_move,
        false_message,
    };
}

} // namespace oa::boneyards::detail
