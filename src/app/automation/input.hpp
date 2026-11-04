// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The automation endpoint's input: the events of an input request become
// SDL events pushed onto SDL's event queue at the frame's pump, which the
// main loop's next poll hands to the game as it hands a device's; the
// request is answered at the next frame's pump, once they have been taken.
// The keys and buttons the events hold down are held for the game's reads
// of what is held (the automation host's hold_key and hold_buttons), and
// let go when the client leaves.
#pragma once

#include "endpoint.hpp"

#include <SDL3/SDL.h>

namespace oa::app::automation {

/// Where the game's window shows the canvas: what places a canvas point in
/// the window.
struct WindowPlacement {
    SDL_Window* window{};     ///< the game's window; null without one
    SDL_Renderer* renderer{}; ///< its renderer; null without one
    SDL_WindowID window_id{}; ///< the window's id; 0 without one
    float density{1.0F};      ///< the window's pixels to a window coordinate
};

/// Returns where the game's window shows the canvas now.
///
/// @param check the check host
/// @return the placement
[[nodiscard]] WindowPlacement window_placement(const CheckHost& check);

/// Returns where the game's window shows the canvas now.
///
/// @param endpoint the endpoint, served
/// @return the placement
[[nodiscard]] WindowPlacement window_placement(const Endpoint& endpoint);

/// Places a canvas point in the window, in window coordinates, as the
/// game's own mapping of the pointer takes it back.
///
/// @param placement where the window shows the canvas
/// @param x canvas column
/// @param y canvas row
/// @param[out] window_x the window's column, in window coordinates
/// @param[out] window_y the window's row, in window coordinates
/// @return false when the renderer cannot place it
[[nodiscard]] bool canvas_to_window(
    const WindowPlacement& placement, float x, float y, float& window_x, float& window_y
);

/// Answers input: checks the events and expect_screen, pushes the events
/// and holds the request until the game has taken them.
///
/// Refuses bad_request for events that cannot be read, screen_changed when
/// the screen is no longer the one expect_screen names, and unsupported
/// for a finger on a machine with no touch screen; nothing is pushed then.
///
/// @param[in,out] endpoint the endpoint
/// @param request the request
/// @param[in,out] answer the answer
void answer_input(Endpoint& endpoint, const Request& request, Answer& answer);

/// Answers the held input request with the frame and tick at which the
/// game took its events, at the pump of the frame after the one that
/// pushed them; and lets go of what the endpoint holds once its client
/// has left.
///
/// @param[in,out] endpoint the endpoint
/// @param stage the stage of the frame
void answer_taken_input(Endpoint& endpoint, FrameStage stage);

} // namespace oa::app::automation
