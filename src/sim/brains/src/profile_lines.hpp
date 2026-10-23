// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Read-only AI profile tables. The lines are the engine's own profile walk.
// A later ticket applies them; this one only shows them.
#pragma once

#include <string_view>

struct lua_State;

namespace oa::sim::brains {

struct Seat;

/// Pushes a read-only profile table.
///
/// `path` and `name` are set when `path` is not empty. `name` is the file
/// name without its folder or a trailing `.txt`, as written. `text` is nil
/// and `too_large` is true when the text passes the host's string limit.
/// `lines` is one table per line `computer_profile_lines` reports. When
/// `with_report` is set, `report_path` is `report_path` or nil when that
/// view is empty. The charge is the text's bytes plus one unit per word.
///
/// `select` is one pass. The plan flag starts matched, so lines before the
/// first plan apply. The built-in player keeps that flag from one pass to
/// the next and applies a downloadable type's lines twice. This pass does
/// neither: a brain that wants those does them itself.
///
/// @param state the sandbox state
/// @param seat the seat, used to tell a unit name from a category
/// @param path the profile path, or empty for a parsed fragment
/// @param text the profile text
/// @param with_report true to set `report_path`
/// @param report_path the report's path slot, when `with_report` is set
void push_profile(
    lua_State* state,
    Seat& seat,
    std::string_view path,
    std::string_view text,
    bool with_report,
    std::string_view report_path
);

/// Installs the profile fields on `game` and `brain.strengths`.
///
/// With no match, `build_list`, `build_list_cap`, `downloadables_allowed`
/// and `brain.strengths` are nil. May raise.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_profile(lua_State* state, Seat& seat);

} // namespace oa::sim::brains
