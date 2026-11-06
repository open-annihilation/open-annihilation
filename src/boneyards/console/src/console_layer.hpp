// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The green Console layer of the Boneyards map panel. While a console screen
// is open the war map shows through it remapped to dark greens under the
// Green_Screen border; opening and closing a screen slides the green in from
// the right and back out over BYConsoleSlide pulses.
#pragma once

#include <cstdint>

#include "console_state.hpp"
#include "oa/present/surface.hpp"

namespace oa::boneyards::console {

// Remap from the war map's palette indexes to the layer's greens: 42..46
// by brightness, and orange 200 for the fixed colours 1..15 and 240..254;
// the game's own remap table.
inline constexpr uint8_t k_console_green_remap[256] = {
    46,  200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 42, 42, 43, 43,
    44,  44,  44,  45,  45,  45,  45,  46,  46,  46,  46,  46,  42,  42,  43,  43,  44, 44, 45, 45,
    45,  45,  45,  45,  46,  46,  46,  46,  42,  42,  43,  43,  44,  44,  44,  45,  45, 45, 45, 46,
    46,  46,  46,  46,  42,  42,  43,  43,  44,  44,  45,  45,  45,  46,  46,  46,  46, 46, 46, 46,
    42,  42,  42,  43,  43,  43,  43,  44,  44,  44,  45,  45,  45,  46,  46,  46,  42, 42, 43, 43,
    43,  44,  44,  45,  45,  45,  45,  45,  45,  45,  45,  45,  42,  42,  43,  43,  44, 44, 45, 45,
    45,  45,  46,  46,  46,  46,  46,  46,  42,  42,  43,  43,  43,  44,  44,  45,  45, 45, 45, 46,
    46,  46,  46,  46,  42,  42,  43,  43,  44,  44,  45,  45,  45,  45,  46,  46,  46, 46, 46, 46,
    42,  42,  43,  43,  44,  44,  44,  45,  45,  45,  45,  46,  46,  46,  46,  46,  42, 42, 43, 44,
    44,  44,  45,  45,  45,  45,  46,  46,  46,  46,  46,  46,  42,  42,  42,  42,  43, 43, 43, 44,
    44,  44,  45,  45,  45,  45,  45,  45,  42,  42,  43,  44,  44,  45,  45,  45,  42, 43, 44, 45,
    45,  46,  46,  46,  42,  43,  44,  45,  45,  45,  45,  45,  42,  43,  43,  44,  44, 45, 45, 46,
    200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 42,
};

// Frontend sounds of the slide.
namespace console_sound {
inline constexpr const char* green_slide = "GREEN_SLIDE";
inline constexpr const char* big_button = "BIGBUTTON";
} // namespace console_sound

enum class ConsoleSlide : int32_t {
    in = 0,  // the green covers the map from the right
    out = 1, // the green leaves to the right
};

// Console window. Its state image is what the map panel draws while the
// layer covers the map.
struct ConsoleLayer {
    Window base{};
    MainFrame* frame{};
    GalaxyPanel* panel{};  // the map panel, whose viewer the layer shows
    int32_t slide_steps{}; // BYConsoleSlide, 1..40
    int32_t countdown{};   // pulses left in the slide
    uint8_t sliding{};
    Rect32 map_part{};    // ConMap columns left of the slide edge
    Rect32 green_part{};  // ConGreen columns right of it
    Rect32 border_part{}; // Green_Screen columns drawn at green_part
    // Widgets.gaf Green_Screen, the border drawn over the green; map and
    // green take its size.
    Handle green_screen{};
    present::SurfaceBuffer* map{};   // "ConMap", the viewer pixels under the layer
    present::SurfaceBuffer* green{}; // "ConGreen", `map` through k_console_green_remap
    uint8_t covering{};              // the map panel draws the layer
    ConsoleSlide direction{};
};

/// Builds the hidden Console layer over the map panel's lower 313 rows.
///
/// Loads its Green_Screen border, sizes the ConMap and ConGreen buffers to
/// it and reads BYConsoleSlide from the client settings, clamped to 1..40.
///
/// @param frame Main Frame the layer reports its slides to
/// @param panel map panel whose viewer the layer shows
/// @return the layer
ConsoleLayer* console_layer_create(MainFrame* frame, GalaxyPanel* panel);

/// Shows the layer with its first snapshot of the map and starts sliding in, covering the map.
///
/// @param[in,out] layer Console layer
void console_layer_start(ConsoleLayer* layer);

/// Starts sliding the green layer back out.
///
/// @param[in,out] layer Console layer
void console_layer_reverse(ConsoleLayer* layer);

/// Shows the open console screen on top once the green covers the map.
///
/// Enables the console buttons unless the screen is the update or login
/// console. After a slide every playing sound stops first, BIGBUTTON plays
/// and, while the background sound is on, BGM drones on the alternate route.
///
/// @param[in,out] frame Main Frame
/// @param play_sound whether the slide's sounds play (after a slide)
void main_frame_present_screen(MainFrame* frame, bool play_sound);

/// Hands the map back once the green has left.
///
/// The console buttons are enabled but Release, and the map unlocks, or the
/// unlock is left to the frame update while the galaxy data loads. After a
/// slide every playing sound stops first, BIGBUTTON plays and, while the
/// background sound is on, BGM drones on the alternate route.
///
/// @param[in,out] frame Main Frame
/// @param play_sound whether the slide's sounds play (after a slide)
void main_frame_release_map(MainFrame* frame, bool play_sound);

/// Closes the open console screen and slides the green back out.
///
/// The Release button goes back to its normal face; the map is handed back
/// when the slide ends.
///
/// @param[in,out] frame Main Frame
void main_frame_close_screen(MainFrame* frame);

/// Shows the console a launched battle returns to, raised and shown without a slide.
///
/// Every playing sound stops first and, while the background sound is on,
/// BGM drones on the alternate route. With no debriefing room the battle
/// console opens again, the same one when it was already built. With a
/// debriefing room the current area becomes that room's area and the Chat
/// console opens into it.
///
/// @param[in,out] frame Main Frame
void main_frame_reopen_chat(MainFrame* frame);

} // namespace oa::boneyards::console
