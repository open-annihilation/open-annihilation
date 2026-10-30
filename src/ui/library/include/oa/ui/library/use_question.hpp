// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// USE's question, scope first: whether the layout is for every game or the
// mod being played, what to do with the player's own changes, what moves,
// and which of the layout's helpers to switch on. Nothing here reads a file
// or writes a preference. The host applies KeyUseResult after the player
// presses USE, and never writes over a layout's own changes.
#pragma once

#include "oa/ui/library/library.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oa::ui::library {

/// Opens USE's question for a layout. GET, an install and a catalogue update
/// never call this. The choices start as the question offers them: keep is
/// marked, every helper is on, and "Only with {mod}" is marked when the
/// layout's works-with names the mod being played. What USE would move is
/// asked of Library::key_hooks, from the layout in use to this one, once.
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

/// Tells whether USE would put the player's own changes on top: they are
/// shown, keep is marked, and the new layout has none of its own yet. A
/// layout that already has its own changes keeps them, and nothing is
/// written over them.
///
/// @param use the question
/// @return true when the own changes go on top
[[nodiscard]] bool key_use_keeps(const KeyUse& use) noexcept;

/// The commands USE would move for the answers marked now: KeyMoves::kept
/// while key_use_keeps, else KeyMoves::clean.
///
/// @param use the question
/// @return the list, at most most_key_moves; empty when the host said nothing
[[nodiscard]] const std::vector<KeyMove>& key_use_moves(const KeyUse& use) noexcept;

/// How many commands USE would move for the answers marked now, all of them.
///
/// @param use the question
/// @return the count
[[nodiscard]] uint32_t key_use_moves_total(const KeyUse& use) noexcept;

/// USE's button, which names the answer to Where: "Use for {mod}" while only
/// the mod being played is marked, else "Use for every game".
///
/// @param library the Library, its question open
/// @return the caption
[[nodiscard]] std::string key_use_button(const Library& library);

/// What USE applies. Nothing when the question is closed.
///
/// Keep is false when the new layout already holds the player's own changes,
/// so the host does not write over that folder.
///
/// @param library the Library
/// @return the result; nothing while the question is closed
[[nodiscard]] std::optional<KeyUseResult> key_use_result(const Library& library);

} // namespace oa::ui::library
