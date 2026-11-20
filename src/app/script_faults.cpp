// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/script_faults.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oa::app {
namespace {
namespace opcode = sim::script_vm::opcode;

// The name a COB listing gives a piece instruction.
const char* piece_instruction_name(uint32_t op) noexcept {
    switch (op) {
    case opcode::move:
        return "MOVE";
    case opcode::turn:
        return "TURN";
    case opcode::spin:
        return "SPIN";
    case opcode::stop_spin:
        return "STOP_SPIN";
    case opcode::move_now:
        return "MOVE_NOW";
    case opcode::turn_now:
        return "TURN_NOW";
    case opcode::wait_turn:
        return "WAIT_FOR_TURN";
    case opcode::wait_move:
        return "WAIT_FOR_MOVE";
    case opcode::show:
        return "SHOW";
    case opcode::hide:
        return "HIDE";
    case opcode::cache:
        return "CACHE";
    case opcode::dont_cache:
        return "DONT_CACHE";
    case opcode::shade:
        return "SHADE";
    case opcode::dont_shade:
        return "DONT_SHADE";
    case opcode::emit_sfx:
        return "EMIT_SFX";
    case opcode::explode:
        return "EXPLODE";
    default:
        return "a piece instruction";
    }
}

// Whether a piece instruction names an axis after its piece.
bool names_axis(uint32_t op) noexcept {
    return op == opcode::move || op == opcode::turn || op == opcode::spin ||
           op == opcode::stop_spin || op == opcode::move_now || op == opcode::turn_now ||
           op == opcode::wait_turn || op == opcode::wait_move;
}
} // namespace

std::string script_fault_line(
    std::string_view unit_name,
    std::string_view script_path,
    const sim::match_runtime::ScriptFaultNote& note
) {
    using sim::script_vm::ErrorCode;
    const auto& program = *note.program;
    const auto& fault = note.fault;
    std::string line = "open-annihilation: ";
    line.append(script_path).append(" (").append(unit_name).append("), ");
    if (note.script >= 0 && static_cast<std::size_t>(note.script) < program.scripts.size())
        line.append(program.scripts[static_cast<std::size_t>(note.script)].name);
    else
        line.append("code word ").append(std::to_string(fault.pc));
    line.append(": ").append(piece_instruction_name(fault.opcode)).append(" of piece ");
    if (fault.code == ErrorCode::invalid_piece || fault.piece >= program.piece_names.size()) {
        line.append(std::to_string(fault.piece))
            .append(", past the script's ")
            .append(std::to_string(program.piece_names.size()))
            .append(" pieces, is skipped and the script goes on");
        return line;
    }
    line.append(program.piece_names[fault.piece]);
    if (fault.code == ErrorCode::invalid_axis || fault.axis > 2) {
        line.append(" on axis ")
            .append(std::to_string(fault.axis))
            .append(", outside x, y and z, is skipped and the script goes on");
        return line;
    }
    if (names_axis(fault.opcode)) {
        constexpr std::array<const char*, 3> axes{"x", "y", "z"};
        line.append(" on the ").append(axes[fault.axis]).append(" axis");
    }
    const auto missing = fault.takes - fault.held;
    if (fault.takes == 1)
        line.append(" has no value on the stack; it reads 0");
    else
        line.append(" lacks ")
            .append(std::to_string(missing))
            .append(" of its ")
            .append(std::to_string(fault.takes))
            .append(" stack values; ")
            .append(missing == 1 ? "it reads 0" : "they read 0");
    line.append(" and the script goes on");
    return line;
}

} // namespace oa::app
