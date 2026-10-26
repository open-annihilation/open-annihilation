// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The numpad pointer's synthetic mouse. It never warps the system's cursor:
// the events carry pointer_keys_mouse_id, as the pad's carry pad_mouse_id.

#include "oa/app/runtime.hpp"

#include "pad_state.hpp"

#include "oa/ui/key_bindings/helpers.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <ostream>

namespace oa::app {
namespace {

/// The button mask bit for a pointer-key button, or 0.
///
/// @param button the button number
/// @return the mask bit
uint32_t button_mask(uint8_t button) {
    if (button == SDL_BUTTON_LEFT)
        return SDL_BUTTON_LMASK;
    if (button == SDL_BUTTON_RIGHT)
        return SDL_BUTTON_RMASK;
    return 0;
}

/// Prints one synthetic event when input is being traced.
///
/// @param kind motion or button
/// @param x the pointer's column
/// @param y the pointer's row
/// @param button the button, for a button event
/// @param down the button went down
/// @param trace true when --trace-input is on
void trace_pointer_keys(const char* kind, float x, float y, uint8_t button, bool down, bool trace) {
    if (!trace)
        return;
    std::cerr << "input pointer-keys which=0x" << std::hex
              << static_cast<unsigned>(pointer_keys_mouse_id) << std::dec << ' ' << kind
              << " x=" << x << " y=" << y;
    if (kind[0] == 'b')
        std::cerr << " button=" << static_cast<int>(button) << " down=" << (down ? 1 : 0);
    std::cerr << '\n';
}

} // namespace

void Runtime::emit_pointer_keys_step(
    oa::ui::key_bindings::PointerStep step, float& x, float& y, bool& known, uint32_t& buttons
) {
    const auto size = PadAccess::canvas(*this);
    const float scale_x = size.x / static_cast<float>(kCanvasWidth);
    const float scale_y = size.y / static_cast<float>(kCanvasHeight);
    if (!known) {
        if (pointer_x_ == 0.0F && pointer_y_ == 0.0F) {
            x = size.x / 2.0F;
            y = size.y / 2.0F;
        } else {
            x = pointer_x_;
            y = pointer_y_;
        }
        known = true;
    } else if (std::fabs(pointer_x_ - x) > 0.5F || std::fabs(pointer_y_ - y) > 0.5F) {
        // A real mouse, or another pointer, moved. Continue from where it is.
        x = pointer_x_;
        y = pointer_y_;
    }
    x = std::clamp(x + step.dx * scale_x, 0.0F, std::max(0.0F, size.x - 1.0F));
    y = std::clamp(y + step.dy * scale_y, 0.0F, std::max(0.0F, size.y - 1.0F));

    const auto send = [&](SDL_Event& event) {
        bool running = true;
        dispatch_event(event, running);
        if (!running)
            exit_requested_ = true;
    };
    const bool place = std::fabs(pointer_x_ - x) > 0.5F || std::fabs(pointer_y_ - y) > 0.5F;
    if (place) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.timestamp = SDL_GetTicksNS();
        event.motion.windowID = 0;
        event.motion.which = pointer_keys_mouse_id;
        event.motion.state = buttons;
        event.motion.x = x;
        event.motion.y = y;
        trace_pointer_keys("motion", x, y, 0, false, options_.trace_input);
        send(event);
    }
    if (step.down) {
        const uint8_t button = *step.down;
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.timestamp = SDL_GetTicksNS();
        event.button.windowID = 0;
        event.button.which = pointer_keys_mouse_id;
        event.button.button = button;
        event.button.down = true;
        event.button.clicks = step.clicks == 0 ? uint8_t{1} : step.clicks;
        event.button.x = x;
        event.button.y = y;
        buttons |= button_mask(button);
        trace_pointer_keys("button", x, y, button, true, options_.trace_input);
        send(event);
    }
    if (step.up) {
        const uint8_t button = *step.up;
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.timestamp = SDL_GetTicksNS();
        event.button.windowID = 0;
        event.button.which = pointer_keys_mouse_id;
        event.button.button = button;
        event.button.down = false;
        event.button.clicks = 1;
        event.button.x = x;
        event.button.y = y;
        buttons &= ~button_mask(button);
        trace_pointer_keys("button", x, y, button, false, options_.trace_input);
        send(event);
    }
}

} // namespace oa::app
