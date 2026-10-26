// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// USE's question: what to do with the player's own changes, which of the
// layout's helpers to switch on, and whether the layout is for every game
// or the mod being played. Nothing here reads a file or writes a preference.
// The host applies KeyUseResult after the player presses USE.
#pragma once

#include "oa/ui/library/library.hpp"

#include <cstddef>
#include <optional>

namespace oa::ui::library {

/// Opens USE's question for a layout. GET, an install and a catalogue update
/// never call this. The choices start as the question offers them: keep is
/// marked, every helper is on, and "Only with {mod}" is marked when the
/// layout's works-with names the mod being played.
///
/// @param[in,out] library the Library
/// @param entry the layout USE was pressed on
void open_key_use(Library& library, const Entry& entry);

/// Closes the question. CANCEL and a closed Library call this. It writes
/// nothing.
///
/// @param[in,out] library the Library
void close_key_use(Library& library) noexcept;

/// Marks keep or aside.
///
/// @param[in,out] library the Library, its question open
/// @param keep true for "Keep my changes on top"
void set_key_use_keep(Library& library, bool keep) noexcept;

/// Switches one helper. Off leaves that setting as it is when USE is applied.
///
/// @param[in,out] library the Library, its question open
/// @param index the helper's place in KeyUse::helpers
/// @param on the switch
void set_key_use_helper(Library& library, std::size_t index, bool on) noexcept;

/// Marks every game, or only the mod being played.
///
/// @param[in,out] library the Library, its question open
/// @param for_mod true for "Only with {mod}"
void set_key_use_scope(Library& library, bool for_mod) noexcept;

/// What USE applies. Nothing when the question is closed.
///
/// Keep is false when the new layout already holds the player's own changes,
/// so the host does not write over that folder.
///
/// @param library the Library
/// @return the result; nothing while the question is closed
[[nodiscard]] std::optional<KeyUseResult> key_use_result(const Library& library);

} // namespace oa::ui::library
