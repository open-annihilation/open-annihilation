// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch and pen hints the touch controls read input with
// (input_hints.hpp): fingers reach the game only as fingers, the mouse only
// as a mouse, and the pen as a mouse that hovers.
#include "oa/app/input_hints.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace oa::app {
namespace {

/// One hint the touch controls set, and whether the environment variable
/// that holds it in its place has been reported this run.
struct InputHint {
    const char* name{};  ///< SDL's hint, which is also its environment variable's name
    const char* value{}; ///< the value the touch controls read input with
    bool reported{};     ///< the environment variable holding it was reported
};

/// The number of hints the touch controls set.
constexpr std::size_t kInputHintCount = 4;

/// Returns the hints of the run, with what has been reported of them.
///
/// The touch controls make their own clicks from fingers (no mouse events
/// from fingers), read the mouse as a mouse (no touch events from the
/// mouse) and keep the pen a mouse that hovers (mouse events from the pen,
/// no touch events from it).
///
/// @return the hints, kept for the run
std::array<InputHint, kInputHintCount>& input_hints() noexcept {
    static std::array<InputHint, kInputHintCount> hints{{
        {SDL_HINT_TOUCH_MOUSE_EVENTS, "0", false},
        {SDL_HINT_MOUSE_TOUCH_EVENTS, "0", false},
        {SDL_HINT_PEN_MOUSE_EVENTS, "1", false},
        {SDL_HINT_PEN_TOUCH_EVENTS, "0", false},
    }};
    return hints;
}

} // namespace

void set_input_hints() {
    for (InputHint& hint : input_hints()) {
        if (SDL_SetHint(hint.name, hint.value))
            continue;
        // SDL refuses a hint at normal priority while an environment
        // variable of its name is set; the variable is left to say how
        // input is read.
        if (const char* held = SDL_getenv(hint.name); held != nullptr) {
            if (!hint.reported) {
                hint.reported = true;
                std::clog << "open-annihilation: the environment sets " << hint.name << '=' << held
                          << ", which the touch controls keep in place of " << hint.value << '\n';
            }
            continue;
        }
        throw std::runtime_error(
            std::string("SDL refused the touch controls' input hint ") + hint.name + ": " +
            SDL_GetError()
        );
    }
}

void set_mouse_double_click_time(uint32_t milliseconds) {
    if (milliseconds == 0) {
        (void)SDL_ResetHint(SDL_HINT_MOUSE_DOUBLE_CLICK_TIME);
        return;
    }
    const std::string text = std::to_string(milliseconds);
    if (SDL_SetHint(SDL_HINT_MOUSE_DOUBLE_CLICK_TIME, text.c_str()))
        return;
    // An environment variable of the hint's name keeps the system's time.
    if (SDL_getenv(SDL_HINT_MOUSE_DOUBLE_CLICK_TIME) != nullptr)
        return;
}

} // namespace oa::app
