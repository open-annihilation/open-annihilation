// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boneyards message box ("BYMessageBox"): a 216x136 top-level dialog painted
// with Main_Console.gaf "MessageBox", a wrapped text area and Ok, Ok/Cancel or
// Yes/No buttons. Every Boneyards screen reports errors and confirmations
// through it. A message whose glyphs are taller than the font's line
// lengthens the box. The picture grows with it: the top edge, the bar behind
// the buttons and the bottom edge stay, and the panel between them repeats.
#pragma once

#include <cstdint>

#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards {

// Button sets (may be combined; later sets overlap earlier ones).
namespace message_box_buttons {
inline constexpr uint32_t ok = 0x1;
inline constexpr uint32_t ok_cancel = 0x2;
inline constexpr uint32_t yes_no = 0x4;
} // namespace message_box_buttons

// Button and child names; also the captions.
namespace message_box_name {
inline constexpr const char* box = "BYMessageBox";
inline constexpr const char* text = "Text";
inline constexpr const char* ok = "Ok";
inline constexpr const char* cancel = "Cancel";
inline constexpr const char* yes = "Yes";
inline constexpr const char* no = "No";
} // namespace message_box_name

namespace message_box_layout {
inline constexpr int32_t x = 0x8E, y = 0xAC, width = 0xD8, height = 0x88;
inline constexpr int32_t text_dx = 0xD, text_dy = 6, text_width = 0xBE, text_height = 0x60;
inline constexpr int32_t button_width = 0x39, button_height = 0x15, button_dy = 0x6A;
inline constexpr int32_t single_button_dx = 0x50, left_button_dx = 0x1E, right_button_dx = 0x82;
} // namespace message_box_layout

namespace message_box_resource {
inline constexpr const char* gaf = "main_console.gaf";
inline constexpr const char* face = "MessageBox";
inline constexpr const char* button_face = "BlankButton";
inline constexpr const char* font = "anims\\Hattfont12.gaf";
inline constexpr int32_t key = 9;
} // namespace message_box_resource

struct MessageBox;
using MessageBoxClosed = void (*)(void* user, MessageBox* box);

struct MessageBox {
    Window base;
    uint8_t reserved_after_base; // cleared by the constructor; nothing reads it
    // Not set by the constructor (zero here); the destructor copies it to the
    // draw object's flag.
    uint8_t draw_flag_on_close;
    bool* accepted;       // set by Ok/Yes (cleared on creation)
    bool* declined;       // set by Cancel/No (cleared on creation)
    Window* focus_return; // receives focus when a button closes the box
    // Called once when a button or key closes the box, besides the flags
    // `accepted` and `declined` a caller may poll.
    MessageBoxClosed on_closed;
    void* user;
};

extern const WindowOps k_message_box_ops;
extern const WindowOps k_blank_button_ops;

/// Builds a message box as a raised top-level window of the installed desktop.
///
/// The flags are cleared now and set when a button or key closes the box.
/// The box keeps the keyboard. Enter presses Ok or Yes until an answer has
/// the focus; Return and Space then press that answer. A letter presses the
/// button it starts, and Escape presses Cancel or No. The first arrow lands
/// on the default answer, and later arrows move between the answers.
///
/// @param text message; literal "\n" sequences become line breaks; null shows nothing
/// @param buttons message_box_buttons set (may be combined; later sets overlap earlier ones)
/// @param[out] accepted set by Ok or Yes; may be null
/// @param[out] declined set by Cancel or No; may be null
/// @param focus_return window given the focus when the box closes; null gives
///        it back to the window that had it when the box opened
/// @return the box; allocation failure aborts
MessageBox* message_box_create(
    const char* text, uint32_t buttons, bool* accepted, bool* declined, Window* focus_return
);

/// Hears that a message box opened. The text is the one the box was given.
using MessageBoxOpened = void (*)(const char* text);

/// The listener `message_box_create` calls once the box is shown. Null reports nothing.
///
/// @param opened the listener, or null
void message_box_set_opened(MessageBoxOpened opened);

/// Builds a four-state Main_Console.gaf "BlankButton" with a Hattfont12 caption.
///
/// @param[in,out] parent window the button is placed in
/// @param dx left edge relative to the parent, in pixels
/// @param dy top edge relative to the parent, in pixels
/// @param caption caption shown in every state
/// @return the button
Window* blank_button_create(Window* parent, int32_t dx, int32_t dy, const char* caption);

/// Rewrites every literal backslash-n pair as CR LF, in place.
///
/// @param[in,out] text NUL-terminated text to rewrite
void convert_escaped_newlines(char* text);

/// Finds the open (not yet closed) message box among the desktop's top-level windows.
///
/// @param desktop desktop to search; may be null
/// @return the topmost open box, or null
MessageBox* message_box_find_open(Desktop* desktop);

/// Brings every open message box in front of the other top-level windows.
///
/// A console that opens over a box raises its window above the box. The
/// boxes are put back in front, oldest first, so the one that was on top
/// stays on top. The keyboard focus is left where it is. A closed box is
/// left where it is.
///
/// @param desktop desktop whose boxes move; null does nothing
void message_box_keep_in_front(Desktop* desktop);

} // namespace oa::boneyards
