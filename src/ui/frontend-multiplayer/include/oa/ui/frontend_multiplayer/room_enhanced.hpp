// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room requires every machine to agree on Enhanced Mode before START.
#pragma once

namespace oa::ui::frontend_multiplayer {

struct Lobby;

/// The first reason Enhanced Mode holds START, or none.
///
/// An enhanced room requires the host's version and simulation hash on every
/// seated machine, watchers included. A room with Enhanced Mode off keeps the
/// usual 3.1c rules unless another machine advertises Enhanced Mode.
///
/// @param lobby Lobby state.
/// @return The English refusal line, or null. Allocates nothing.
[[nodiscard]] const char* lobby_enhanced_start_problem(Lobby& lobby) noexcept;

/// Tells whether the room agrees on Enhanced Mode as far as START is concerned.
///
/// @param lobby Lobby state.
/// @return True when nothing about Enhanced Mode holds START. Allocates nothing.
[[nodiscard]] bool lobby_enhanced_allow_start(Lobby& lobby) noexcept;

/// Posts the current Enhanced Mode refusal on this machine and writes it to the log.
///
/// The line is not sent. A room with no refusal posts nothing.
///
/// @param lobby Lobby state.
void lobby_note_enhanced_refused_start(Lobby& lobby) noexcept;

} // namespace oa::ui::frontend_multiplayer
