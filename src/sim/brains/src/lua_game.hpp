// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The `game` table. install_game fills the empty table B01 created.
// set_game_field is how a later ticket adds one field of its own.
#pragma once

#include "internal.hpp"

struct lua_State;

namespace oa::sim::brains {

/// Fills `game` from the seat's facts and keeps the table scripts already hold.
///
/// Nested tables are built once, so `game.mod == game.mod`. `has_expansion`
/// and `hack` live in the hidden table: a write to either name raises, and
/// `pairs(game)` yields both names. The build is charged to the phase that
/// opens the sandbox, which is when this runs.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_game(lua_State* state, Seat& seat);

/// Pops a value and stores it in `game`'s hidden table under `key`.
///
/// A missing table or a null key drops the value. This does not charge the
/// build; the caller counts the value it pushed.
///
/// @param state the sandbox state
/// @param seat the seat whose `game` is open
/// @param key the field name
void set_game_field(lua_State* state, Seat& seat, const char* key);

/// Replaces `game.ai_profile` with the match's reloaded profile text.
///
/// The path and the report path stay the ones the seat was given. Nothing
/// is written when the match has no profile text yet. The caller counts
/// the work: this pushes the table and does not open a phase of its own.
///
/// @param state the sandbox state
/// @param seat the seated brain
void refresh_ai_profile(lua_State* state, Seat& seat);

} // namespace oa::sim::brains
