// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether the system's quit leaves at once (runtime_events.cpp). The game
// draws nothing differently: a window the player can see still asks.
#pragma once

#include <string_view>

namespace oa::app::mcp {

/// Tells whether the player can see a window.
///
/// A missing window, a headless run, a hidden window and the dummy video
/// driver are not a window the player can answer. A minimized window is
/// still shown.
///
/// @param has_window the run has a window
/// @param headless the run is headless
/// @param hidden the window is hidden
/// @param video_driver SDL's video driver; empty when none is current
/// @return true when the player can see the window
[[nodiscard]] inline bool window_shown_to_player(
    bool has_window, bool headless, bool hidden, std::string_view video_driver
) noexcept {
    return has_window && !headless && !hidden && video_driver != "dummy";
}

/// Tells whether the system's quit leaves as game.quit does.
///
/// No window the player can see, or a run that serves --mcp-http, leaves
/// at once. A shown window without --mcp-http keeps the exit question.
///
/// @param window_shown window_shown_to_player
/// @param mcp_http --mcp-http is on
/// @return true when the run leaves without asking
[[nodiscard]] inline bool system_quit_leaves(bool window_shown, bool mcp_http) noexcept {
    return !window_shown || mcp_http;
}

/// Tells whether this run serves --mcp-http.
///
/// @return true after check_options accepted --mcp-http
[[nodiscard]] bool http_serving() noexcept;

} // namespace oa::app::mcp
