// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The log line for a faulty piece instruction a unit script ran past
// (sim::script_vm::PieceFault): a piece or axis outside the script, or
// values missing from the stack. The script goes on as in 3.1c, and the
// match keeps the fault once per unit type and script
// (sim::match_runtime::ScriptFaultLog); the line names the unit type, its
// script file, the script, the instruction and the piece, so that a mod's
// author can find it.
#pragma once

#include "oa/sim/match_runtime/script.hpp"

#include <string>
#include <string_view>

namespace oa::app {

/// Words a faulty piece instruction as a line for the log.
///
/// @param unit_name the unit type's name
/// @param script_path the unit type's script file
/// @param note the fault, its program and its script
/// @return the line, without a newline
[[nodiscard]] std::string script_fault_line(
    std::string_view unit_name,
    std::string_view script_path,
    const sim::match_runtime::ScriptFaultNote& note
);

} // namespace oa::app
