// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The log lines for the faulty piece instructions a unit script ran past:
// each names the unit type, its script file, the script, the instruction and
// the piece, and says what the engine did instead.

#include "oa/app/script_faults.hpp"

#include "oa/test/check.hpp"

#include <memory>
#include <string>

namespace {

namespace vm = oa::sim::script_vm;
using oa::sim::match_runtime::ScriptFaultNote;

std::string line(const ScriptFaultNote& note) {
    return oa::app::script_fault_line("hellfire", "scripts/hellfire.COB", note);
}

} // namespace

int main() {
    auto program = std::make_shared<oa::formats::cob::CobProgram>();
    program->entry_points = {0, 13};
    program->scripts = {{"Create", 0}, {"Other", 13}};
    program->piece_names = {"Flare1", "Flare2"};
    // TAUIP's Hellfire: MOVE_NOW Flare2 on the x axis with nothing pushed.
    OA_CHECK(
        line(
            {program.get(),
             0,
             {vm::ErrorCode::stack_underflow, 0, 5, vm::opcode::move_now, 1, 0, 1, 0}}
        ) ==
        "open-annihilation: scripts/hellfire.COB (hellfire), Create: MOVE_NOW of piece Flare2 on "
        "the x axis has no value on the stack; it reads 0 and the script goes on"
    );
    OA_CHECK(
        line(
            {program.get(), 0, {vm::ErrorCode::stack_underflow, 0, 2, vm::opcode::move, 0, 2, 2, 1}}
        ) ==
        "open-annihilation: scripts/hellfire.COB (hellfire), Create: MOVE of piece Flare1 on the "
        "z axis lacks 1 of its 2 stack values; it reads 0 and the script goes on"
    );
    OA_CHECK(
        line(
            {program.get(),
             1,
             {vm::ErrorCode::stack_underflow, 0, 13, vm::opcode::spin, 1, 1, 2, 0}}
        ) ==
        "open-annihilation: scripts/hellfire.COB (hellfire), Other: SPIN of piece Flare2 on the "
        "y axis lacks 2 of its 2 stack values; they read 0 and the script goes on"
    );
    OA_CHECK(
        line(
            {program.get(), 1, {vm::ErrorCode::invalid_piece, 0, 13, vm::opcode::turn, 9, 1, 2, 0}}
        ) == "open-annihilation: scripts/hellfire.COB (hellfire), Other: TURN of piece 9, past the "
             "script's 2 pieces, is skipped and the script goes on"
    );
    OA_CHECK(
        line(
            {program.get(),
             0,
             {vm::ErrorCode::invalid_axis, 0, 2, vm::opcode::wait_turn, 1, 4, 0, 0}}
        ) ==
        "open-annihilation: scripts/hellfire.COB (hellfire), Create: WAIT_FOR_TURN of piece Flare2 "
        "on axis 4, outside x, y and z, is skipped and the script goes on"
    );
    OA_CHECK(
        line(
            {program.get(),
             0,
             {vm::ErrorCode::stack_underflow, 0, 3, vm::opcode::explode, 0, 0, 1, 0}}
        ) ==
        "open-annihilation: scripts/hellfire.COB (hellfire), Create: EXPLODE of piece Flare1 has "
        "no value on the stack; it reads 0 and the script goes on"
    );
    OA_CHECK(
        line(
            {program.get(), -1, {vm::ErrorCode::invalid_piece, 0, 40, vm::opcode::hide, 2, 0, 0, 0}}
        ) ==
        "open-annihilation: scripts/hellfire.COB (hellfire), code word 40: HIDE of piece 2, past "
        "the script's 2 pieces, is skipped and the script goes on"
    );
    return oa::test::check_exit_status();
}
