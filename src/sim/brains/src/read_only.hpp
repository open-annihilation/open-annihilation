// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A read-only view of a Lua table. The proxy stays empty. Its metatable
// reads, walks and measures the hidden table, and refuses every write with
// the caller's sentence. A04 reuses it for `me`.
#pragma once

struct lua_State;

namespace oa::sim::brains {

/// Turns the table at `proxy` into a read-only view of the table at `hidden`.
///
/// `__index` is the hidden table, `__len` and `__pairs` walk it, and
/// `__metatable` is false. `__newindex` raises `denial` with no location
/// prefix. Both indexes may be relative.
///
/// @param state the sandbox state
/// @param proxy the table scripts already hold
/// @param hidden the table that stores the values
/// @param denial the sentence a write raises
void make_read_only(lua_State* state, int proxy, int hidden, const char* denial);

/// Pops a table and pushes a new read-only view of it.
///
/// @param state the sandbox state
/// @param denial the sentence a write raises
void push_read_only(lua_State* state, const char* denial);

} // namespace oa::sim::brains
