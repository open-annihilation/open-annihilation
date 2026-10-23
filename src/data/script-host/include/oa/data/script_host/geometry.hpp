// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The engine's heading and length functions for scripts.
#pragma once

struct lua_State;

namespace oa::data::script_host {

/// Opens the geometry library and sets the global `geometry`.
///
/// The value is a plain table, so later code may add fields to it. A
/// profile's `install` calls this. A sandbox that does not call it has no
/// `geometry`. May raise.
///
/// @param state sandbox state
void open_geometry_library(lua_State* state);

} // namespace oa::data::script_host
