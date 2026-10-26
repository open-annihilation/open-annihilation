// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch and pen hints the touch controls read input with.
#pragma once

#include <cstdint>

namespace oa::app {

/// Sets the touch and pen hints the touch controls read input with: no mouse events made
/// from fingers or touch events made from the mouse, the pen as a mouse, no touch events
/// made from the pen.
///
/// Called before SDL starts video when touch controls are on from the start, and when the
/// first finger switches them on (SDL reads these hints live). A hint that an environment
/// variable already holds (SDL refuses a normal-priority hint then) is reported once on
/// std::clog and left as the variable says; any other refusal throws std::runtime_error.
/// Safe to call more than once.
void set_input_hints();

/// Sets how far apart two clicks may be and still be a double click.
///
/// A time of zero clears the hint, so the system's own time is used. A hint
/// an environment variable already holds is left as the variable says. Any
/// other refusal is left in place and not thrown: a double click still uses
/// whatever time is already set.
///
/// @param milliseconds the time, in milliseconds; zero uses the system's
void set_mouse_double_click_time(uint32_t milliseconds);

} // namespace oa::app
