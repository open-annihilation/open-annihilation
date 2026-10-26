// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/script_vm.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace vm = oa::sim::script_vm;

enum class PieceWrite {
    visible,
    cached,
    shaded,
    position_x,
    angle_x,
    position_y,
    angle_y,
    position_z,
    angle_z,
};

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

vm::Vm make_vm(
    std::vector<uint32_t> code,
    std::vector<uint32_t> entries = {0},
    std::size_t statics = 1,
    int32_t time_scale = 1000
) {
    return vm::Vm({std::move(code), std::move(entries), statics}, time_scale);
}

struct MockHost final : vm::Host {
    std::array<std::array<int32_t, 3>, 2> positions{};
    std::array<std::array<int32_t, 3>, 2> angles{};
    std::array<uint32_t, 2> visible_values{};
    std::array<uint32_t, 2> cached_values{};
    std::array<uint32_t, 2> shaded_values{};
    std::vector<std::pair<uint32_t, bool>> visibility, caching, shading;
    std::vector<std::pair<uint32_t, int32_t>> effects, explosions;
    std::array<int32_t, 5> query_arguments{};
    std::vector<std::pair<int32_t, int32_t>> set_values;
    std::vector<std::array<int32_t, 3>> attachments;
    std::vector<int32_t> drops;
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> ignored_piece_op_calls;
    std::vector<uint32_t> dont_shadow_calls;
    std::vector<uint32_t> carrying_queries;
    std::size_t carrier_queries = 0;
    std::vector<uint32_t> random_bounds;
    std::vector<PieceWrite> piece_writes;

    int32_t piece_position(uint32_t piece, uint32_t axis) const override {
        return positions[piece][axis];
    }

    int32_t piece_angle(uint32_t piece, uint32_t axis) const override {
        return angles[piece][axis];
    }

    uint32_t piece_visible(uint32_t piece) const override { return visible_values[piece]; }

    uint32_t piece_cached(uint32_t piece) const override { return cached_values[piece]; }

    uint32_t piece_shaded(uint32_t piece) const override { return shaded_values[piece]; }

    void set_piece_position(uint32_t piece, uint32_t axis, int32_t value) override {
        positions[piece][axis] = value;
        constexpr PieceWrite events[] = {
            PieceWrite::position_x, PieceWrite::position_y, PieceWrite::position_z
        };
        piece_writes.push_back(events[axis]);
    }

    void set_piece_angle(uint32_t piece, uint32_t axis, uint32_t value) override {
        angles[piece][axis] = static_cast<int32_t>(value);
        constexpr PieceWrite events[] = {
            PieceWrite::angle_x, PieceWrite::angle_y, PieceWrite::angle_z
        };
        piece_writes.push_back(events[axis]);
    }

    void set_piece_visible(uint32_t piece, uint32_t value) override {
        visible_values[piece] = value;
        visibility.emplace_back(piece, value != 0);
        piece_writes.push_back(PieceWrite::visible);
    }

    void set_piece_cached(uint32_t piece, uint32_t value) override {
        cached_values[piece] = value;
        caching.emplace_back(piece, value != 0);
        piece_writes.push_back(PieceWrite::cached);
    }

    void set_piece_shaded(uint32_t piece, uint32_t value) override {
        shaded_values[piece] = value;
        shading.emplace_back(piece, value != 0);
        piece_writes.push_back(PieceWrite::shaded);
    }

    void emit_sfx(uint32_t piece, int32_t value) override { effects.emplace_back(piece, value); }

    void explode(uint32_t piece, int32_t value) override { explosions.emplace_back(piece, value); }

    int32_t get_unit_value(
        int32_t first, int32_t second, int32_t third, int32_t fourth, int32_t fifth
    ) override {
        query_arguments = {first, second, third, fourth, fifth};
        return first + second + third + fourth + fifth;
    }

    void set_unit_value(int32_t selector, int32_t value) override {
        set_values.emplace_back(selector, value);
    }

    void attach_unit(int32_t first, int32_t second, int32_t third) override {
        attachments.push_back({first, second, third});
    }

    void drop_unit(int32_t unit) override { drops.push_back(unit); }

    void ignored_piece_op(uint32_t inline_value, uint32_t first, uint32_t second) override {
        ignored_piece_op_calls.emplace_back(inline_value, first, second);
    }

    void dont_shadow(uint32_t inline_value) override { dont_shadow_calls.push_back(inline_value); }

    uint32_t is_carrying_unit(uint32_t value) override {
        carrying_queries.push_back(value);
        return value + 100;
    }

    uint32_t carrier_unit_id() override {
        ++carrier_queries;
        return 77;
    }

    uint32_t random_bounded(uint32_t bound) override {
        random_bounds.push_back(bound);
        return 2;
    }
};

void arithmetic_locals_and_statics() {
    auto machine = make_vm({
        vm::opcode::stack_alloc,
        vm::opcode::push_constant,
        40,
        vm::opcode::pop_local,
        0,
        vm::opcode::push_local,
        0,
        vm::opcode::push_constant,
        2,
        vm::opcode::add,
        vm::opcode::pop_static,
        0,
        vm::opcode::return_,
    });
    require(machine.start(0).ok(), "arithmetic script did not start");
    const auto run = machine.tick(0);
    require(run.ok(), "arithmetic script faulted");
    require(machine.static_value(0) == 42, "local/add/static instruction chain did not produce 42");
    require(machine.active_count() == 0, "RETURN did not stop the context");
}

void signed_branching_and_logic() {
    auto machine = make_vm({
        vm::opcode::push_constant,
        static_cast<uint32_t>(-3),
        vm::opcode::push_constant,
        5,
        vm::opcode::less,
        vm::opcode::jump_if_false,
        15,
        vm::opcode::push_constant,
        7,
        vm::opcode::push_constant,
        7,
        vm::opcode::equal,
        vm::opcode::pop_static,
        0,
        vm::opcode::return_,
        vm::opcode::push_constant,
        0,
        vm::opcode::pop_static,
        0,
        vm::opcode::return_,
    });
    require(machine.start(0).ok(), "branch script did not start");
    require(machine.tick(0).ok(), "branch script faulted");
    require(machine.static_value(0) == 1, "signed comparison or conditional jump disagreed");

    auto false_branch = make_vm({
        vm::opcode::push_constant,
        8,
        vm::opcode::push_constant,
        3,
        vm::opcode::less,
        vm::opcode::jump_if_false,
        11,
        vm::opcode::push_constant,
        99,
        vm::opcode::jump,
        13,
        vm::opcode::push_constant,
        17,
        vm::opcode::pop_static,
        0,
        vm::opcode::return_,
    });
    require(
        false_branch.start(0).ok() && false_branch.tick(0).ok(), "taken conditional branch faulted"
    );
    require(false_branch.static_value(0) == 17, "false condition did not take its absolute target");
}

int32_t run_binary(uint32_t operation, int32_t lhs, int32_t rhs) {
    auto machine = make_vm({
        vm::opcode::push_constant,
        static_cast<uint32_t>(lhs),
        vm::opcode::push_constant,
        static_cast<uint32_t>(rhs),
        operation,
        vm::opcode::pop_static,
        0,
        vm::opcode::return_,
    });
    require(machine.start(0).ok() && machine.tick(0).ok(), "binary instruction faulted");
    return *machine.static_value(0);
}

void integer_instruction_matrix() {
    require(
        run_binary(vm::opcode::add, std::numeric_limits<int32_t>::max(), 1) ==
            std::numeric_limits<int32_t>::min(),
        "ADD did not wrap at 32 bits"
    );
    require(run_binary(vm::opcode::subtract, 3, 8) == -5, "SUB operand order disagreed");
    require(run_binary(vm::opcode::multiply, -7, 6) == -42, "MUL signed bits disagreed");
    require(
        run_binary(vm::opcode::divide, -43, 5) == -8, "DIV was not signed/truncated toward zero"
    );
    require(run_binary(vm::opcode::bit_and, 0x55, 0x0f) == 5, "AND disagreed");
    require(run_binary(vm::opcode::bit_or, 0x50, 0x0f) == 0x5f, "OR disagreed");
    require(run_binary(vm::opcode::less_equal, -1, -1) == 1, "signed <= disagreed");
    require(run_binary(vm::opcode::greater, -1, 0) == 0, "signed > disagreed");
    require(run_binary(vm::opcode::greater_equal, 4, 3) == 1, "signed >= disagreed");
    require(run_binary(vm::opcode::not_equal, 4, 4) == 0, "!= disagreed");
    require(run_binary(vm::opcode::logical_and, -3, 9) == 1, "logical AND did not normalize");
    require(run_binary(vm::opcode::logical_or, 0, -1) == 1, "logical OR did not normalize");
    require(
        run_binary(vm::opcode::logical_xor, 6, 3) == 5,
        "0x10059000 did not keep the game's bitwise XOR behavior"
    );
}

void xor_then_not_opcodes() {
    auto machine = make_vm({
        vm::opcode::push_constant,
        0x55aa00ffU,
        vm::opcode::push_constant,
        0x0f0f0f0fU,
        vm::opcode::bit_xor,
        vm::opcode::bit_not,
        vm::opcode::pop_static,
        0,
        vm::opcode::return_,
    });
    require(machine.start(0).ok(), "bitwise script did not start");
    require(machine.tick(0).ok(), "bitwise script faulted");
    const auto expected = static_cast<int32_t>(~(0x55aa00ffU ^ 0x0f0f0f0fU));
    require(
        machine.static_value(0) == expected, "0x10037000 XOR / 0x10038000 NOT semantics disagreed"
    );
}

void sleep_scheduling() {
    auto machine = make_vm(
        {
            vm::opcode::push_constant,
            2000,
            vm::opcode::sleep,
            vm::opcode::push_constant,
            9,
            vm::opcode::pop_static,
            0,
            vm::opcode::return_,
        },
        {0},
        1,
        10
    );
    const auto started = machine.start(0);
    require(started.ok(), "sleep script did not start");
    require(machine.tick(0).ok(), "sleep opcode faulted");
    require(
        machine.context(started.context).state == vm::ContextState::sleeping,
        "SLEEP did not yield its context"
    );
    require(
        machine.context(started.context).sleep_remaining == 20, "SLEEP scale calculation disagreed"
    );
    require(machine.tick(19).ok(), "sleep countdown faulted");
    require(
        machine.context(started.context).state == vm::ContextState::sleeping,
        "sleep resumed one tick early"
    );
    require(machine.tick(1).ok(), "sleep resume faulted");
    require(
        machine.static_value(0) == 9 && machine.active_count() == 0,
        "sleep did not resume and finish at zero"
    );
}

void call_waits_and_preserves_argument_order() {
    // Context 0 calls context 1. The ascending coordinator visits child 1 in the
    // same tick, but returns to the already-visited parent only on the next tick.
    auto machine = make_vm(
        {
            vm::opcode::push_constant,
            41,
            vm::opcode::call_script,
            1,
            1,
            vm::opcode::return_,
            vm::opcode::stack_alloc,
            vm::opcode::push_local,
            0,
            vm::opcode::push_constant,
            1,
            vm::opcode::add,
            vm::opcode::pop_static,
            0,
            vm::opcode::return_,
        },
        {0, 6}
    );
    const auto parent = machine.start(0);
    require(parent.ok(), "call parent did not start");
    require(machine.tick(0).ok(), "call/child tick faulted");
    require(machine.static_value(0) == 42, "CALL_SCRIPT did not preserve its argument");
    require(
        machine.context(parent.context).state == vm::ContextState::active,
        "child RETURN did not wake its caller"
    );
    require(machine.active_count() == 1, "child did not terminate independently");
    require(
        machine.tick(0).ok() && machine.active_count() == 0,
        "woken caller did not finish on its next coordinator visit"
    );
}

void signal_stops_matching_contexts() {
    auto machine = make_vm(
        {
            vm::opcode::push_constant,
            2,
            vm::opcode::signal,
            vm::opcode::return_,
            vm::opcode::push_constant,
            2,
            vm::opcode::set_signal_mask,
            vm::opcode::push_constant,
            100,
            vm::opcode::sleep,
            vm::opcode::return_,
        },
        {0, 4}
    );
    const auto target = machine.start(1);
    const auto sender = machine.start(0);
    require(target.ok() && sender.ok(), "signal test contexts did not start");
    require(machine.tick(0).ok(), "SIGNAL execution faulted");
    require(
        machine.context(target.context).state == vm::ContextState::stopped,
        "SIGNAL did not stop the context with an intersecting mask"
    );
    require(machine.active_count() == 0, "signal sender did not subsequently return");
}

// Context allocation fails once all eight contexts are busy. START_SCRIPT
// (0x10061000) then leaves its arguments on the caller's stack, advances
// three words and runs on; CALL_SCRIPT (0x10062000) waits on child -1, which
// no RETURN wakes, until a SIGNAL stops it.
void launch_without_free_context() {
    constexpr uint32_t filler = 1;
    constexpr uint32_t short_sleep = 10;
    const std::vector<uint32_t> code{
        // 0: start-script filler(7); the unconsumed 7 is popped into static 0.
        vm::opcode::push_constant,
        7,
        vm::opcode::start_script,
        filler,
        1,
        vm::opcode::pop_static,
        0,
        vm::opcode::return_,
        // 8: filler sleeps, then returns.
        vm::opcode::push_constant,
        short_sleep,
        vm::opcode::sleep,
        vm::opcode::return_,
        // 12: call-script filler(5), then static 1 = 9.
        vm::opcode::push_constant,
        5,
        vm::opcode::call_script,
        filler,
        1,
        vm::opcode::push_constant,
        9,
        vm::opcode::pop_static,
        1,
        vm::opcode::return_,
        // 22: clears its own mask, then signals mask 1.
        vm::opcode::push_constant,
        0,
        vm::opcode::set_signal_mask,
        vm::opcode::push_constant,
        1,
        vm::opcode::signal,
        vm::opcode::return_,
    };
    const std::vector<uint32_t> entries{0, 8, 12, 22};

    auto starter = make_vm(code, entries, 2);
    const auto launcher = starter.start(0);
    require(launcher.ok() && launcher.context == 0, "START_SCRIPT launcher did not start");
    while (starter.active_count() < vm::context_count)
        require(starter.start(filler).ok(), "filling VM contexts failed");
    require(starter.tick(0).ok(), "START_SCRIPT with a full pool faulted");
    require(
        starter.static_value(0) == 7 && starter.context(0).state == vm::ContextState::stopped,
        "START_SCRIPT with a full pool did not leave its argument and run on"
    );
    require(starter.active_count() == vm::context_count - 1, "START_SCRIPT claimed a context");

    auto caller = make_vm(code, entries, 2);
    require(caller.start(2).ok(), "CALL_SCRIPT launcher did not start");
    while (caller.active_count() < vm::context_count)
        require(caller.start(filler).ok(), "filling VM contexts failed");
    require(caller.tick(0).ok(), "CALL_SCRIPT with a full pool faulted");
    const auto waiting = caller.context(0);
    require(
        waiting.state == vm::ContextState::waiting_for_child &&
            waiting.child == vm::no_child_context && waiting.pc == 17 &&
            waiting.stack_pointer == 0 && waiting.slots[0] == 5,
        "CALL_SCRIPT with a full pool did not wait on child -1 with its argument left"
    );
    require(caller.tick(short_sleep).ok() && caller.active_count() == 1, "fillers did not return");
    require(
        caller.context(0).state == vm::ContextState::waiting_for_child &&
            caller.static_value(1) == 0,
        "a RETURN woke the caller waiting on child -1"
    );
    const auto exported = caller.export_state(0);
    require(
        exported.ok() && exported.state->contexts[0].child_context == 0xffffffffU,
        "the no-child word was not saved as 0xFFFFFFFF"
    );
    require(caller.start(3).ok() && caller.tick(0).ok(), "signal sender faulted");
    require(
        caller.context(0).state == vm::ContextState::stopped && caller.active_count() == 0,
        "SIGNAL did not stop the caller waiting on child -1"
    );
}

// The reserve check covers the script index too: a launch of an index outside
// the table claims nothing and runs on. A claimed launch whose arguments are
// not on the stack faults and gives its context back.
void launch_of_missing_script_or_arguments() {
    auto missing = make_vm({
        vm::opcode::push_constant,
        3,
        vm::opcode::start_script,
        9,
        1,
        vm::opcode::pop_static,
        0,
        vm::opcode::return_,
    });
    require(missing.start(0).ok() && missing.tick(0).ok(), "launch of a missing script faulted");
    require(
        missing.static_value(0) == 3 && missing.active_count() == 0,
        "launch of a missing script consumed its argument or claimed a context"
    );

    auto short_stack =
        make_vm({vm::opcode::start_script, 1, 2, vm::opcode::return_, vm::opcode::return_}, {0, 4});
    require(short_stack.start(0).ok(), "short-stack launcher did not start");
    const auto faulted = short_stack.tick(0);
    require(
        !faulted.ok() && faulted.error->code == vm::ErrorCode::stack_underflow &&
            short_stack.active_count() == 1 &&
            short_stack.context(1).state == vm::ContextState::stopped &&
            short_stack.context(0).pc == 0,
        "launch without its arguments kept the claimed context or advanced"
    );
}

void piece_motion_waits_and_callbacks() {
    MockHost host;
    vm::Program program{
        {
            vm::opcode::push_constant,
            2000,
            vm::opcode::push_constant,
            10,
            vm::opcode::move,
            0,
            0,
            vm::opcode::wait_move,
            0,
            0,
            vm::opcode::push_constant,
            1000,
            vm::opcode::push_constant,
            90,
            vm::opcode::turn,
            0,
            1,
            vm::opcode::wait_turn,
            0,
            1,
            vm::opcode::show,
            0,
            vm::opcode::cache,
            0,
            vm::opcode::shade,
            0,
            vm::opcode::push_constant,
            5,
            vm::opcode::emit_sfx,
            0,
            vm::opcode::push_constant,
            6,
            vm::opcode::explode,
            0,
            vm::opcode::return_,
        },
        {0},
        0,
        1
    };
    vm::Vm machine(std::move(program), host, 1000);
    const auto started = machine.start(0);
    require(started.ok() && machine.tick(0).ok(), "piece motion script did not reach WAIT_MOVE");
    require(
        machine.context(started.context).state == vm::ContextState::waiting_for_move,
        "WAIT_MOVE did not yield"
    );
    require(
        machine.tick(5).ok() && host.positions[0][0] == 10,
        "MOVE did not advance and clamp to its target"
    );
    require(
        machine.context(started.context).state == vm::ContextState::waiting_for_move,
        "WAIT_MOVE resumed before the following coordinator visit"
    );
    require(
        machine.tick(0).ok() &&
            machine.context(started.context).state == vm::ContextState::waiting_for_turn,
        "completed MOVE did not resume into TURN/WAIT_TURN"
    );
    require(
        machine.tick(90).ok() && host.angles[0][1] == 90,
        "TURN did not advance and clamp to its target"
    );
    require(
        machine.tick(0).ok() && machine.active_count() == 0,
        "completed TURN did not resume the script"
    );
    require(
        host.visibility == std::vector<std::pair<uint32_t, bool>>{{0, true}} &&
            host.caching == std::vector<std::pair<uint32_t, bool>>{{0, true}} &&
            host.shading == std::vector<std::pair<uint32_t, bool>>{{0, true}},
        "piece flag callbacks disagreed"
    );
    require(
        host.effects == std::vector<std::pair<uint32_t, int32_t>>{{0, 5}} &&
            host.explosions == std::vector<std::pair<uint32_t, int32_t>>{{0, 6}},
        "piece effect callback operand order disagreed"
    );
}

void spin_acceleration_and_stop_wait() {
    MockHost host;
    vm::Vm machine(
        {{
             vm::opcode::push_constant,
             1000,
             vm::opcode::push_constant,
             3000,
             vm::opcode::spin,
             0,
             2,
             vm::opcode::return_,
             vm::opcode::push_constant,
             1000,
             vm::opcode::stop_spin,
             0,
             2,
             vm::opcode::wait_turn,
             0,
             2,
             vm::opcode::return_,
         },
         {0, 8},
         0,
         1},
        host,
        1000
    );
    require(machine.start(0).ok() && machine.tick(1).ok(), "SPIN script faulted");
    require(host.angles[0][2] == 1, "SPIN acceleration did not affect the first motion update");
    const auto stopper = machine.start(1);
    require(stopper.ok() && machine.tick(1).ok(), "STOP_SPIN script faulted");
    require(
        machine.context(stopper.context).state == vm::ContextState::waiting_for_turn,
        "WAIT_TURN did not wait during deceleration"
    );
    require(
        machine.tick(0).ok() && machine.context(stopper.context).state == vm::ContextState::stopped,
        "WAIT_TURN did not resume after spin speed reached zero"
    );
}

void spin_reversal_preserves_per_piece_activity() {
    MockHost host;
    vm::Vm machine(
        {{
             vm::opcode::push_constant,
             0,
             vm::opcode::push_constant,
             static_cast<uint32_t>(-2000),
             vm::opcode::spin,
             0,
             0,
             vm::opcode::return_,
             vm::opcode::push_constant,
             1000,
             vm::opcode::push_constant,
             2000,
             vm::opcode::spin,
             0,
             0,
             vm::opcode::return_,
             vm::opcode::push_constant,
             0,
             vm::opcode::push_constant,
             1000,
             vm::opcode::spin,
             1,
             0,
             vm::opcode::return_,
         },
         {0, 8, 16},
         0,
         2},
        host,
        1000
    );
    require(machine.start(0).ok() && machine.tick(1).ok(), "negative SPIN setup faulted");
    require(host.angles[0][0] == 65534, "negative SPIN did not wrap its angle");
    require(machine.start(1).ok() && machine.tick(1).ok(), "SPIN reversal faulted");
    require(host.angles[0][0] == 65533, "SPIN reversal first acceleration step disagreed");
    require(machine.tick(1).ok(), "SPIN reversal zero-crossing tick faulted");
    const auto stalled_angle = host.angles[0][0];
    require(
        machine.start(2).ok() && machine.tick(1).ok() && machine.tick(5).ok(),
        "separate-piece SPIN faulted"
    );
    require(
        host.angles[0][0] == stalled_angle && host.angles[1][0] != 0,
        "acceleration-only axis incorrectly stayed active through zero"
    );
}

void turn_completion_uses_signed_wrapped_step() {
    MockHost host;
    vm::Vm machine(
        {{
             vm::opcode::push_constant,
             0x7fffffffU,
             vm::opcode::push_constant,
             10,
             vm::opcode::turn,
             0,
             0,
             vm::opcode::wait_turn,
             0,
             0,
             vm::opcode::return_,
         },
         {0},
         0,
         1},
        host,
        1
    );
    const auto started = machine.start(0);
    require(started.ok() && machine.tick(2).ok(), "wrapped-step TURN faulted");
    require(
        host.angles[0][0] == 65534 &&
            machine.context(started.context).state == vm::ContextState::waiting_for_turn,
        "TURN completion used unsigned magnitude instead of the signed wrapped delta"
    );
}

void turn_direction_preserves_wrapped_abs_edge() {
    MockHost host;
    host.angles[0][0] = std::numeric_limits<int32_t>::min();
    vm::Vm machine(
        {{
             vm::opcode::push_constant,
             5,
             vm::opcode::push_constant,
             0,
             vm::opcode::turn,
             0,
             0,
             vm::opcode::wait_turn,
             0,
             0,
             vm::opcode::return_,
         },
         {0},
         0,
         1},
        host,
        1
    );
    require(machine.start(0).ok() && machine.tick(0).ok(), "TURN abs edge setup faulted");
    host.angles[0][0] = 100;
    require(
        machine.tick(1).ok() && host.angles[0][0] == 95,
        "TURN replaced the signed wrapped abs edge with unsigned magnitude"
    );
}

void unit_callback_stack_order() {
    MockHost host;
    vm::Vm machine(
        {{
             vm::opcode::push_constant,
             11,
             vm::opcode::get_unit_value,
             vm::opcode::pop_static,
             0,
             vm::opcode::push_constant,
             1,
             vm::opcode::push_constant,
             2,
             vm::opcode::push_constant,
             3,
             vm::opcode::push_constant,
             4,
             vm::opcode::push_constant,
             5,
             vm::opcode::get,
             vm::opcode::pop_static,
             1,
             vm::opcode::push_constant,
             9,
             vm::opcode::push_constant,
             10,
             vm::opcode::set_unit_value,
             vm::opcode::push_constant,
             20,
             vm::opcode::push_constant,
             21,
             vm::opcode::push_constant,
             22,
             vm::opcode::attach_unit,
             vm::opcode::push_constant,
             23,
             vm::opcode::drop_unit,
             vm::opcode::return_,
         },
         {0},
         2,
         0},
        host
    );
    require(machine.start(0).ok() && machine.tick(0).ok(), "unit callback script faulted");
    require(
        machine.static_value(0) == 11 && machine.static_value(1) == 15,
        "unit query return values were not pushed"
    );
    require(
        host.query_arguments == std::array<int32_t, 5>{1, 2, 3, 4, 5},
        "five-argument GET stack order disagreed"
    );
    require(
        host.set_values == std::vector<std::pair<int32_t, int32_t>>{{9, 10}},
        "SET_VALUE stack order disagreed"
    );
    require(
        host.attachments == std::vector<std::array<int32_t, 3>>{{{20, 21, 22}}} &&
            host.drops == std::vector<int32_t>{23},
        "attach/drop callback stack order disagreed"
    );
}

void host_opcode_rejects_before_consuming_stack() {
    vm::Vm machine(
        {{
             vm::opcode::push_constant,
             1000,
             vm::opcode::push_constant,
             5,
             vm::opcode::move,
             0,
             0,
         },
         {0},
         0,
         1}
    );
    const auto started = machine.start(0);
    require(started.ok(), "missing-host script did not start");
    const auto result = machine.tick(0);
    const auto state = machine.context(started.context);
    require(
        !result.ok() && result.error->code == vm::ErrorCode::missing_host && state.pc == 4 &&
            state.stack_pointer == 1 && state.slots[0] == 1000 && state.slots[1] == 5,
        "missing Host rejection consumed MOVE operands"
    );
}

void constructor_checks_bounds_before_allocation() {
    vm::Vm machine({{vm::opcode::return_}, {0}, std::numeric_limits<std::size_t>::max(), 0});
    require(
        machine.program_error() &&
            machine.program_error()->code == vm::ErrorCode::invalid_program &&
            !machine.start(0).ok(),
        "oversized static table was not rejected before allocation"
    );
    vm::Vm outside({{vm::opcode::return_}, {1}, 0, 0});
    require(
        outside.program_error() && !outside.start(0).ok(),
        "entry point outside code was not rejected"
    );
    require(
        outside.context(vm::context_count).state == vm::ContextState::stopped,
        "a context index past the eight contexts reads as stopped"
    );
}

void context_reuse_preserves_unreset_words() {
    auto machine = make_vm(
        {
            vm::opcode::push_constant,
            2000,
            vm::opcode::sleep,
            vm::opcode::return_,
            vm::opcode::push_constant,
            1,
            vm::opcode::signal,
            vm::opcode::return_,
        },
        {0, 4},
        0,
        10
    );
    const auto sleeper = machine.start(0);
    require(
        sleeper.ok() && machine.tick(0).ok() &&
            machine.context(sleeper.context).sleep_remaining == 20,
        "reuse setup did not retain a sleep value"
    );
    require(
        machine.start(1).ok() && machine.tick(0).ok() && machine.active_count() == 0,
        "signal did not stop the sleeping context"
    );
    const auto reused = machine.start(0);
    require(
        reused.ok() && reused.context == sleeper.context &&
            machine.context(reused.context).sleep_remaining == 20,
        "allocator cleared a context word the game leaves stale"
    );
}

void piece_carrier_and_random_opcodes() {
    MockHost host;
    vm::Vm machine(
        {{
             vm::opcode::push_constant,
             20,
             vm::opcode::push_constant,
             30,
             vm::opcode::ignored_piece_op,
             7,
             vm::opcode::dont_shadow,
             8,
             vm::opcode::push_constant,
             9,
             vm::opcode::is_carrying_unit,
             vm::opcode::pop_static,
             0,
             vm::opcode::carrier_unit_id,
             vm::opcode::pop_static,
             1,
             vm::opcode::push_constant,
             5,
             vm::opcode::push_constant,
             9,
             vm::opcode::random,
             vm::opcode::pop_static,
             2,
             vm::opcode::push_constant,
             1,
             vm::opcode::push_constant,
             2,
             vm::opcode::discard_call,
             0xdeadbeefU,
             2,
             vm::opcode::return_,
         },
         {0},
         3,
         0},
        host
    );
    require(
        machine.start(0).ok() && machine.tick(0).ok(),
        "piece, carrier and random opcode script faulted"
    );
    require(
        host.ignored_piece_op_calls ==
                std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>{{7, 20, 30}} &&
            host.dont_shadow_calls == std::vector<uint32_t>{8},
        "0x10009000/0x1000A000 callback arguments disagreed"
    );
    require(
        host.carrying_queries == std::vector<uint32_t>{9} && host.carrier_queries == 1 &&
            machine.static_value(0) == 109 && machine.static_value(1) == 77,
        "is-carrying and carrier-id callbacks disagreed"
    );
    require(
        host.random_bounds == std::vector<uint32_t>{5} && machine.static_value(2) == 7,
        "RANDOM bounds/result stack behavior disagreed"
    );
    require(
        machine.context(0).stack_pointer == -1,
        "0x10063000 did not consume its encoded argument count"
    );
}

void return_callback_order_and_value() {
    vm::Vm machine(
        {{
             vm::opcode::push_constant,
             42,
             vm::opcode::return_,
         },
         {0},
         0,
         0}
    );
    bool invoked = false;
    bool observed_active = false;
    int32_t observed_sp = 99;
    int32_t returned = 0;
    const auto started = machine.start(0, {}, [&](int32_t value) {
        invoked = true;
        returned = value;
        const auto during = machine.context(0);
        observed_active = during.state == vm::ContextState::active;
        observed_sp = during.stack_pointer;
    });
    require(started.ok() && machine.tick(0).ok(), "callback RETURN faulted");
    require(
        invoked && returned == 42 && observed_active && observed_sp == -1 &&
            machine.context(0).state == vm::ContextState::stopped,
        "RETURN did not pop, invoke, then stop in the game's order"
    );
    invoked = false;
    const auto no_callback = machine.start(0);
    require(
        no_callback.ok() && machine.tick(0).ok() && !invoked &&
            machine.context(no_callback.context).stack_pointer == 0,
        "allocator did not clear callback or null-callback RETURN popped a value"
    );

    vm::Vm missing_value({{vm::opcode::return_}, {0}, 0, 0});
    require(missing_value.start(0, {}, [](int32_t) {}).ok(), "empty callback RETURN did not start");
    const auto error = missing_value.tick(0);
    require(
        !error.ok() && error.error->code == vm::ErrorCode::stack_underflow &&
            missing_value.context(0).state == vm::ContextState::active,
        "callback RETURN underflow changed context state"
    );
}

void discard_call_bounds_before_pop() {
    auto machine = make_vm({
        vm::opcode::push_constant,
        1,
        vm::opcode::discard_call,
        0,
        5,
    });
    const auto started = machine.start(0);
    require(started.ok(), "bounded 0x10063000 test did not start");
    const auto result = machine.tick(0);
    const auto state = machine.context(started.context);
    require(
        !result.ok() && result.error->code == vm::ErrorCode::invalid_argument_count &&
            state.pc == 2 && state.stack_pointer == 0 && state.slots[0] == 1,
        "oversized 0x10063000 changed state before rejection"
    );
}

vm::Program state_bridge_program() {
    return {
        {
            vm::opcode::stack_alloc,
            vm::opcode::push_constant,
            41,
            vm::opcode::pop_local,
            0,
            vm::opcode::push_constant,
            123,
            vm::opcode::pop_static,
            0,
            vm::opcode::show,
            0,
            vm::opcode::cache,
            0,
            vm::opcode::shade,
            0,
            vm::opcode::push_constant,
            1000,
            vm::opcode::push_constant,
            10,
            vm::opcode::move,
            0,
            0,
            vm::opcode::push_constant,
            30,
            vm::opcode::sleep,
            vm::opcode::push_local,
            0,
            vm::opcode::push_constant,
            1,
            vm::opcode::add,
            vm::opcode::pop_static,
            1,
            vm::opcode::wait_move,
            0,
            0,
            vm::opcode::return_,
            vm::opcode::push_constant,
            99,
            vm::opcode::return_,
        },
        {0, 36},
        2,
        1
    };
}

void state_bridge_round_trip_and_continuation() {
    constexpr uint32_t identity_token = 0x4829137aU;
    MockHost source_host;
    vm::Vm source(state_bridge_program(), source_host, 1000);
    const auto sleeper = source.start(0);
    require(
        sleeper.ok() && source.tick(0).ok() && source.tick(2).ok(),
        "state bridge did not reach a partially advanced sleep/motion state"
    );
    const std::array<int32_t, 1> running_arguments{7};
    const auto runner = source.start(1, running_arguments);
    require(runner.ok(), "state bridge second running context did not start");

    const auto exported = source.export_state(identity_token);
    require(exported.ok(), "running VM state did not export");
    const auto encoded = oa::sim::script_state::encode(*exported.state);
    require(
        exported.state->contexts[sleeper.context].execution_state ==
                static_cast<uint32_t>(vm::ContextState::sleeping) &&
            exported.state->contexts[sleeper.context].sleep_remaining == 28 &&
            exported.state->contexts[sleeper.context].slots[0] == 41,
        "sleep state, scheduler ticks, or local slot was not exported"
    );
    require(
        exported.state->contexts[runner.context].execution_state ==
                static_cast<uint32_t>(vm::ContextState::active) &&
            exported.state->contexts[runner.context].slots[0] == 7 &&
            exported.state->statics == std::vector<int32_t>({123, 0}),
        "running context stack or static words were not exported"
    );
    require(
        exported.state->pieces[0].axes[0].move_target == 10 &&
            exported.state->pieces[0].axes[0].move_speed_per_tick == 1 &&
            exported.state->pieces[0].axes[0].position == 2 &&
            exported.state->pieces[0].visible == 1 && exported.state->pieces[0].cached == 1 &&
            exported.state->pieces[0].shaded == 1,
        "motion, transform, or flag fields were not exported"
    );

    const auto decoded = oa::sim::script_state::decode(encoded, identity_token, 2, 1);
    require(decoded.ok(), "VM-produced state payload did not decode");
    MockHost restored_host;
    vm::Vm restored(state_bridge_program(), restored_host, 1000);
    require(
        !restored.import_state(*decoded.state).has_value(),
        "decoded state did not import into a matching VM"
    );
    require(
        restored_host.piece_writes == std::vector<PieceWrite>(
                                          {PieceWrite::visible,
                                           PieceWrite::cached,
                                           PieceWrite::shaded,
                                           PieceWrite::position_x,
                                           PieceWrite::angle_x,
                                           PieceWrite::position_y,
                                           PieceWrite::angle_y,
                                           PieceWrite::position_z,
                                           PieceWrite::angle_z}
                                      ),
        "restore host side effects did not follow the flag/axis order"
    );
    const auto reexported = restored.export_state(identity_token);
    require(
        reexported.ok() && oa::sim::script_state::encode(*reexported.state) == encoded,
        "restore did not reproduce the exact serialized VM/host fields"
    );

    const auto continue_and_compare = [&](uint32_t elapsed) {
        require(
            source.tick(elapsed).ok() && restored.tick(elapsed).ok(),
            "source or restored VM faulted after state transfer"
        );
        const auto source_after = source.export_state(identity_token);
        const auto restored_after = restored.export_state(identity_token);
        require(
            source_after.ok() && restored_after.ok() &&
                oa::sim::script_state::encode(*source_after.state) ==
                    oa::sim::script_state::encode(*restored_after.state),
            "restored VM continuation diverged from the uninterrupted VM"
        );
    };
    continue_and_compare(8);
    continue_and_compare(20);
    continue_and_compare(0);
    require(
        source.active_count() == 0 && restored.active_count() == 0 &&
            source.static_value(1) == 42 && restored.static_value(1) == 42,
        "restored sleeping/local script did not complete identically"
    );
}

void state_restore_clears_return_callback() {
    bool callback_invoked = false;
    vm::Vm source(
        {{
             vm::opcode::push_constant,
             42,
             vm::opcode::return_,
         },
         {0},
         0,
         0}
    );
    require(
        source.start(0, {}, [&](int32_t) { callback_invoked = true; }).ok(),
        "callback-clearing source did not start"
    );
    const auto saved = source.export_state(7);
    require(saved.ok(), "callback-clearing source did not export");

    vm::Vm restored(
        {{
             vm::opcode::push_constant,
             42,
             vm::opcode::return_,
         },
         {0},
         0,
         0}
    );
    require(
        restored.start(0, {}, [&](int32_t) { callback_invoked = true; }).ok(),
        "callback-clearing destination did not start with an installed callback"
    );
    require(
        !restored.import_state(*saved.state).has_value() && restored.tick(0).ok(),
        "callback-clearing restore did not execute"
    );
    require(
        !callback_invoked && restored.context(0).state == vm::ContextState::stopped &&
            restored.context(0).stack_pointer == 0,
        "restore kept the return callback installed before it"
    );
}

void state_import_rejects_invalid_context_before_mutation() {
    vm::Vm machine({{vm::opcode::return_}, {0}, 0, 0});
    const auto before = machine.context(0);
    auto invalid = machine.export_state(1);
    require(invalid.ok(), "invalid-state fixture did not export");
    invalid.state->contexts[0].execution_state = 0x12345678U;
    invalid.state->contexts[0].stack_pointer = 99;
    invalid.state->active_context_count = 1;
    const auto error = machine.import_state(*invalid.state);
    require(
        error.has_value() && error->code == vm::ErrorCode::invalid_state &&
            machine.context(0).state == before.state &&
            machine.context(0).stack_pointer == before.stack_pointer,
        "invalid context was not rejected before VM mutation"
    );
}

void unsupported_is_side_effect_free_at_fault() {
    constexpr uint32_t unhandled_extension_opcode = 0x10039000U;
    auto machine = make_vm({
        vm::opcode::push_constant,
        7,
        unhandled_extension_opcode,
        vm::opcode::return_,
    });
    const auto started = machine.start(0);
    require(started.ok(), "unsupported-op script did not start");
    const auto first = machine.tick(0);
    require(
        !first.ok() && first.error->code == vm::ErrorCode::unsupported_opcode,
        "unsupported engine opcode was not reported"
    );
    const auto before = machine.context(started.context);
    require(
        before.pc == 2 && before.stack_pointer == 0 && before.slots[0] == 7,
        "state before unsupported opcode is unexpected"
    );
    const auto second = machine.tick(0);
    const auto after = machine.context(started.context);
    require(
        second.ok() && after.pc == before.pc && after.stack_pointer == before.stack_pointer &&
            after.slots == before.slots && machine.active_count() == 0,
        "an unsupported opcode did not stop its context, or changed its state"
    );
}

// An unknown instruction in one context stops that context alone: a later
// context runs in the same tick.
void unsupported_stops_only_its_context() {
    constexpr uint32_t unhandled_extension_opcode = 0x10039000U;
    auto machine = make_vm(
        {
            unhandled_extension_opcode,
            vm::opcode::push_constant,
            5,
            vm::opcode::pop_static,
            0,
            vm::opcode::return_,
        },
        {0, 1}
    );
    require(machine.start(0).ok() && machine.start(1).ok(), "both scripts did not start");
    const auto run = machine.tick(0);
    require(
        !run.ok() && run.error->code == vm::ErrorCode::unsupported_opcode,
        "the unknown instruction was not reported"
    );
    require(machine.static_value(0) == 5, "the later context did not run in the same tick");
    require(machine.active_count() == 0, "a context stayed active");
    require(machine.tick(0).ok(), "the stopped context faulted again");
}

void faults_do_not_consume_operands() {
    auto machine = make_vm({
        vm::opcode::push_constant,
        static_cast<uint32_t>(std::numeric_limits<int32_t>::min()),
        vm::opcode::push_constant,
        static_cast<uint32_t>(-1),
        vm::opcode::divide,
    });
    const auto started = machine.start(0);
    require(started.ok(), "divide script did not start");
    const auto run = machine.tick(0);
    require(
        !run.ok() && run.error->code == vm::ErrorCode::signed_divide_overflow,
        "signed division overflow was not trapped"
    );
    const auto state = machine.context(started.context);
    require(
        state.pc == 4 && state.stack_pointer == 1 &&
            state.slots[0] == std::numeric_limits<int32_t>::min() && state.slots[1] == -1,
        "faulting DIV consumed or changed operands"
    );
}

void synchronous_local_query() {
    auto machine = make_vm(
        {
            vm::opcode::push_constant,
            99,
            vm::opcode::pop_local,
            0,
            vm::opcode::push_constant,
            77,
            vm::opcode::pop_local,
            3,
            vm::opcode::push_constant,
            111,
            vm::opcode::return_,
            vm::opcode::jump,
            11,
        },
        {0, 11}
    );
    require(machine.start(1).ok(), "unrelated query sibling did not start");
    int32_t first = 10;
    int32_t second = 20;
    int32_t third = 30;
    int32_t fourth = 40;
    const auto queried = machine.query(0, {&first, nullptr, &third, &fourth});
    const auto queried_context = machine.context(1);
    require(
        queried.ok() && first == 99 && second == 20 && third == 30 && fourth == 77,
        "query did not read back only the non-null local slots"
    );
    require(
        queried_context.state == vm::ContextState::stopped && queried_context.stack_pointer == 4 &&
            queried_context.slots[0] == 99 && queried_context.slots[1] == 0 &&
            queried_context.slots[2] == 30 && queried_context.slots[3] == 77 &&
            queried_context.slots[4] == 111,
        "query consumed the RETURN operand or invoked a callback"
    );
    require(
        machine.active_count() == 1 && machine.context(0).state == vm::ContextState::active &&
            machine.context(0).pc == 11,
        "query advanced a context other than the queried script"
    );

    int32_t unchanged = 5;
    const auto missing = machine.query(2, {&unchanged, &unchanged, &unchanged, &unchanged});
    require(
        !missing.accepted && missing.error &&
            missing.error->code == vm::ErrorCode::invalid_script && unchanged == 5,
        "invalid query query modified arguments"
    );
    while (machine.active_count() < vm::context_count)
        require(machine.start(1).ok(), "filling VM contexts failed");
    const auto exhausted = machine.query(0, {&unchanged, nullptr, nullptr, nullptr});
    require(
        !exhausted.accepted && exhausted.error &&
            exhausted.error->code == vm::ErrorCode::no_free_context && unchanged == 5,
        "exhausted query query modified arguments"
    );

    auto sleeper = make_vm({
        vm::opcode::push_constant,
        1000,
        vm::opcode::sleep,
    });
    int32_t sleep_slots[] = {1, 2, 3, 4};
    const auto sleeping =
        sleeper.query(0, {&sleep_slots[0], &sleep_slots[1], &sleep_slots[2], &sleep_slots[3]});
    require(
        sleeping.ok() && sleep_slots[0] == 1 && sleep_slots[1] == 2 && sleep_slots[2] == 3 &&
            sleep_slots[3] == 4 && sleeper.active_count() == 1 &&
            sleeper.context(0).state == vm::ContextState::sleeping,
        "sleeping query query did not stay scheduled with its locals intact"
    );
}

void script_name_lookup() {
    const char* names[] = {"Create", "Query", "AimPrimary", "Query"};
    require(vm::find_script({}, "Create") == -1, "find_script searched an empty name table");
    require(vm::find_script({}, nullptr) == -1, "find_script called strcmp on a null empty query");
    require(
        vm::find_script(std::span<const char* const>(names, 4), nullptr) == -1,
        "find_script called strcmp on a null query"
    );
    require(
        vm::find_script(names, "Query") == 1,
        "find_script did not return the first exact strcmp match"
    );
    require(vm::find_script(names, "query") == -1, "find_script folded script-name case");
    require(vm::find_script(names, "Aim") == -1, "find_script treated a prefix as a script name");
    require(vm::find_script(names, "AimPrimary") == 2, "find_script missed the last searched name");
    require(
        vm::find_script(std::span<const char* const>(names, 2), "AimPrimary") == -1,
        "find_script searched past the COB script count"
    );
    require(
        vm::find_script(std::span<const char* const>(names, 2), "Query") == 1,
        "find_script ignored a name inside the script count"
    );

    const char* hole[] = {"Create", nullptr, "AimPrimary"};
    require(
        vm::find_script(hole, "Create") == 0, "find_script rejected a match before a null entry"
    );
    require(
        vm::find_script(hole, "AimPrimary") == -1, "find_script skipped a null name-table entry"
    );

    const char* empty_names[] = {"", "Create"};
    require(vm::find_script(empty_names, "") == 0, "find_script missed an empty script name");
    require(
        vm::find_script(empty_names, "Create") == 1,
        "find_script did not continue after an empty name"
    );
}

void instruction_budget_stops_infinite_loop() {
    auto machine = make_vm({vm::opcode::jump, 0});
    require(machine.start(0).ok(), "loop script did not start");
    const auto run = machine.tick(0, 17);
    require(
        !run.ok() && run.error->code == vm::ErrorCode::instruction_limit && run.instructions == 17,
        "instruction budget did not bound an infinite JUMP"
    );
}

// A chain of comparisons whose partial results all wait on the stack (the
// Cavedog compiler's "a || b || c" leaves every term in place and ORs them
// at the end; Devolution's beacon keeps 78) runs past the record's 32 slots:
// the VM sizes every context's stack for its program's deepest script. The
// saved state keeps the first 32 slots of a context, and a context's stack
// stops at limit::stack_slots.
void shared_program_keeps_separate_machines() {
    const auto shared = vm::prepare_program(
        {{
             vm::opcode::push_constant,
             5,
             vm::opcode::pop_static,
             0,
             vm::opcode::push_constant,
             2000,
             vm::opcode::sleep,
             vm::opcode::return_,
         },
         {0},
         1}
    );
    vm::Vm first(shared, 10);
    vm::Vm second(shared, 10);
    require(
        first.program().code.data() == second.program().code.data() &&
            !first.program().code.empty(),
        "two VMs built from one program do not share its code"
    );
    require(first.start(0).ok() && first.tick(0).ok(), "first shared VM did not sleep");
    require(
        first.static_value(0) == 5 && first.active_count() == 1 &&
            first.context(0).state == vm::ContextState::sleeping &&
            first.context(0).stack_pointer == -1,
        "first shared VM did not keep its own static and context"
    );
    require(
        second.static_value(0) == 0 && second.active_count() == 0 &&
            second.context(0).state == vm::ContextState::stopped,
        "the second VM saw the first VM's statics or thread"
    );
    const std::array<int32_t, 1> argument{11};
    require(second.start(0, argument).ok(), "second shared VM did not start");
    require(
        second.context(0).stack_pointer == 0 && second.context(0).slots[0] == 11 &&
            first.context(0).stack_pointer == -1 && first.context(0).slots[0] == 2000,
        "the two VMs did not keep separate stacks"
    );
}

void deep_expression_fits_the_stack() {
    constexpr uint32_t terms = 78;
    std::vector<uint32_t> code;
    // Every term but the last is false; the chain's value is the last one's.
    for (uint32_t term = 0; term < terms; ++term)
        code.insert(code.end(), {vm::opcode::push_constant, term + 1 == terms ? 1U : 0U});
    for (uint32_t term = 1; term < terms; ++term)
        code.push_back(vm::opcode::logical_or);
    code.insert(
        code.end(), {vm::opcode::pop_static, 0, vm::opcode::push_constant, 10, vm::opcode::sleep}
    );
    code.insert(code.end(), {vm::opcode::push_constant, 7, vm::opcode::pop_static, 0});
    code.push_back(vm::opcode::return_);
    auto machine = make_vm(code, {0}, 1);
    // The script is entered with four slots and stacks 78 more.
    require(machine.stack_slots() == terms + 4, "the stack was not sized for the chain");
    require(
        make_vm({vm::opcode::return_}).stack_slots() == vm::stack_capacity,
        "a shallow script has the record's 32 slots"
    );
    require(machine.start(0).ok(), "deep expression script did not start");
    const auto run = machine.tick(0);
    require(run.ok(), "a 78-slot expression faulted: " + (run.ok() ? "" : run.error->message));
    require(machine.static_value(0) == 1, "the OR chain's value was lost past slot 32");
    require(
        machine.context(0).stack_pointer == -1 && machine.active_count() == 1,
        "the expression left its slots behind"
    );
    // The stack a save keeps is the record's 32 slots; a round trip restores
    // the sleeping context and its empty stack.
    const auto exported = machine.export_state(0);
    require(exported.ok(), "export after a deep expression failed");
    require(
        exported.state->contexts[0].slots.size() == vm::stack_capacity &&
            exported.state->contexts[0].stack_pointer == -1,
        "the exported context is not the 32-slot record"
    );
    auto restored = make_vm(code, {0}, 1);
    require(!restored.import_state(*exported.state), "import after a deep expression failed");
    require(
        restored.tick(10).ok() && restored.static_value(0) == 7,
        "the restored context did not run on"
    );

    // The stack is bounded: one push past limit::stack_slots faults with
    // stack_overflow, leaving the context's slots as they were.
    std::vector<uint32_t> overflow;
    for (std::size_t slot = 0; slot <= vm::limit::stack_slots; ++slot)
        overflow.insert(overflow.end(), {vm::opcode::push_constant, 3});
    auto bounded = make_vm(overflow, {0}, 1);
    require(bounded.stack_slots() == vm::limit::stack_slots, "the stack was not capped");
    require(bounded.start(0).ok(), "overflow script did not start");
    const auto faulted = bounded.tick(0);
    require(
        !faulted.ok() && faulted.error->code == vm::ErrorCode::stack_overflow &&
            bounded.context(0).stack_pointer == static_cast<int32_t>(vm::limit::stack_slots) - 1,
        "the working stack did not stop at its limit"
    );
}

int main() {
    try {
        shared_program_keeps_separate_machines();
        deep_expression_fits_the_stack();
        arithmetic_locals_and_statics();
        signed_branching_and_logic();
        integer_instruction_matrix();
        xor_then_not_opcodes();
        sleep_scheduling();
        call_waits_and_preserves_argument_order();
        signal_stops_matching_contexts();
        launch_without_free_context();
        launch_of_missing_script_or_arguments();
        piece_motion_waits_and_callbacks();
        spin_acceleration_and_stop_wait();
        spin_reversal_preserves_per_piece_activity();
        turn_completion_uses_signed_wrapped_step();
        turn_direction_preserves_wrapped_abs_edge();
        unit_callback_stack_order();
        host_opcode_rejects_before_consuming_stack();
        constructor_checks_bounds_before_allocation();
        context_reuse_preserves_unreset_words();
        piece_carrier_and_random_opcodes();
        return_callback_order_and_value();
        discard_call_bounds_before_pop();
        state_bridge_round_trip_and_continuation();
        state_restore_clears_return_callback();
        state_import_rejects_invalid_context_before_mutation();
        unsupported_is_side_effect_free_at_fault();
        unsupported_stops_only_its_context();
        faults_do_not_consume_operands();
        synchronous_local_query();
        script_name_lookup();
        instruction_budget_stops_infinite_loop();
        std::cout << "script VM instruction tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
