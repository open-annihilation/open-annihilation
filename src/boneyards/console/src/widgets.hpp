// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Window classes and helpers shared by the console screens.
#pragma once

#include <cstddef>

#include "oa/boneyards/console/images.hpp"
#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards::console {

// Push button whose draw slot paints nothing (an invisible hotspot).
extern const WindowOps k_hotspot_button_ops;
// Plain panel that draws its current state image, then its children.
extern const WindowOps k_panel_ops;

/// Returns the starting point for derived panel tables in other files.
///
/// @return a panel table freeing its record with console_window_free
WindowOps console_panel_ops();

/// Allocates a zeroed record whose first member is a Window, initialised with a dispatch table.
///
/// Released through window_destroy.
///
/// @param size record size in bytes
/// @param ops the record's dispatch table
/// @return the record's window; allocation failure aborts
Window* console_window_alloc(std::size_t size, const WindowOps* ops);

/// Copies text into a fixed field, cut to fit and terminated.
///
/// @param[out] out field receiving the text
/// @param capacity size of the field, terminator included; at least 1
/// @param text text to copy; null copies nothing
void copy_text(char* out, std::size_t capacity, const char* text);

/// Creates a plain panel that draws its current state image, then its children.
///
/// @return the panel
Window* panel_create();

/// Creates a push button whose draw slot paints nothing (an invisible hotspot).
///
/// @return the button
Window* hotspot_button_create();

/// Loads one image per state from consecutive frames of a sequence through the window's bind_images
/// slot.
///
/// @param[in,out] window window taking the images
/// @param gaf_name archive name under anims/
/// @param sequence sequence name
/// @param first frame of the first state
/// @param key transparent palette index
/// @return the bind_images slot's result
int32_t console_bind_images(
    Window* window, const char* gaf_name, const char* sequence, int32_t first, uint8_t key
);

/// Releases and frees a record made by console_window_alloc (its destroy handler).
///
/// @param self the record's window
void console_window_free(Window* self);

} // namespace oa::boneyards::console
