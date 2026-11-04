// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The words the in-game MCP badge shows (extension.cpp). The OA layer draws
// them. Empty means no client is connected, and the layer draws nothing.
#pragma once

#include <string_view>

namespace oa::app::mcp {

/// The badge while a client is driving the game.
///
/// "MCP · stepped" when a match is on screen and that match is held for the
/// stepped clock, or, with no match on screen, when the run was started with
/// `--mcp-clock stepped`. "MCP · realtime" otherwise. Empty until a client
/// has finished initialize and is still connected: listening, a connection
/// that has not initialized, and a client that has gone all draw nothing.
///
/// @param match_on_screen true while a match is on screen
/// @param match_stepped true when that match is held for the stepped clock
/// @return the badge text, or empty
[[nodiscard]] std::string_view connected_badge(bool match_on_screen, bool match_stepped) noexcept;

} // namespace oa::app::mcp
