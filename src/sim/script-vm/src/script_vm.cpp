// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/script_vm.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace oa::sim::script_vm {
namespace {
constexpr uint32_t opcode_dispatch_mask = 0x100ff000U;
// The PUSH and POP families, whose low bits name the operand's kind.
constexpr uint32_t push_family = opcode::push_constant & opcode_dispatch_mask;
constexpr uint32_t pop_family = opcode::pop_local & opcode_dispatch_mask;

int32_t as_signed(uint32_t value) noexcept {
    return std::bit_cast<int32_t>(value);
}

uint32_t as_unsigned(int32_t value) noexcept {
    return std::bit_cast<uint32_t>(value);
}

Error make_error(
    ErrorCode code, std::size_t context, uint32_t pc, uint32_t raw_opcode, std::string message
) {
    return {code, context, pc, raw_opcode, std::move(message)};
}

// What one instruction does to a context's stack: the words it takes in
// the code, the values it pops and the values it pushes.
struct StackEffect {
    uint32_t words{};
    uint32_t pops{};
    uint32_t pushes{};
};

// The stack effect of the instruction at pc, as execute_context carries it
// out; an instruction the VM does not support takes one word and nothing
// from the stack.
StackEffect stack_effect(const Program& program, uint32_t pc) {
    const auto raw = program.code[pc];
    const auto op = raw & opcode_dispatch_mask;
    auto word = [&](uint32_t offset) -> uint32_t {
        return pc + offset < program.code.size() ? program.code[pc + offset] : 0;
    };
    switch (op) {
    case opcode::ignored_piece_op:
        return {2, 2, 0};
    case opcode::dont_shadow:
    case opcode::show:
    case opcode::hide:
    case opcode::cache:
    case opcode::dont_cache:
    case opcode::shade:
    case opcode::dont_shade:
    case opcode::jump:
        return {2, 0, 0};
    case opcode::is_carrying_unit:
    case opcode::get_unit_value:
        return {1, 1, 1};
    case opcode::carrier_unit_id:
    case opcode::stack_alloc:
        return {1, 0, 1};
    case opcode::move:
    case opcode::turn:
    case opcode::spin:
        return {3, 2, 0};
    case opcode::stop_spin:
    case opcode::move_now:
    case opcode::turn_now:
        return {3, 1, 0};
    case opcode::wait_turn:
    case opcode::wait_move:
        return {3, 0, 0};
    case opcode::emit_sfx:
    case opcode::explode:
    case opcode::jump_if_false:
    case pop_family:
        return {2, 1, 0};
    case opcode::get:
        return {1, 5, 1};
    case opcode::set_unit_value:
    case opcode::random:
        return {1, 2, op == opcode::random ? 1U : 0U};
    case opcode::attach_unit:
        return {1, 3, 0};
    case opcode::drop_unit:
    case opcode::pop_stack:
    case opcode::sleep:
    case opcode::set_signal_mask:
    case opcode::signal:
        return {1, 1, 0};
    case push_family:
        return {2, 0, 1};
    case opcode::bit_not:
    case opcode::logical_not:
        return {1, 1, 1};
    case opcode::add:
    case opcode::subtract:
    case opcode::multiply:
    case opcode::divide:
    case opcode::bit_and:
    case opcode::bit_or:
    case opcode::bit_xor:
    case opcode::less:
    case opcode::less_equal:
    case opcode::greater:
    case opcode::greater_equal:
    case opcode::equal:
    case opcode::not_equal:
    case opcode::logical_and:
    case opcode::logical_or:
    case opcode::logical_xor:
        return {1, 2, 1};
    case opcode::start_script:
    case opcode::call_script:
    case opcode::discard_call:
        return {3, word(2), 0};
    case opcode::return_:
        return {1, 0, 0};
    default:
        return {1, 0, 0};
    }
}

// The stack slots the program's deepest script needs: the slots a script
// is entered with (four, or the most arguments a launch in the code gives)
// plus the deepest its own code stacks, the code walked once with the depth
// starting over at each entry point. A compiled script's stack is the same
// whichever way a branch went, so the walk's depth is the running depth. At
// least the record's 32 slots, at most limit::stack_slots.
std::size_t deepest_stack(const Program& program) {
    constexpr int64_t engine_entry_slots = 4;
    const auto scripts = program.entry_points.size();
    const auto code_words = static_cast<uint32_t>(program.code.size());
    auto sorted_entries = std::make_unique<uint32_t[]>(scripts);
    std::copy(program.entry_points.begin(), program.entry_points.end(), sorted_entries.get());
    std::sort(sorted_entries.get(), sorted_entries.get() + scripts);
    int64_t entry_slots = engine_entry_slots;
    for (uint32_t pc = 0; pc < code_words;) {
        const auto op = program.code[pc] & opcode_dispatch_mask;
        if ((op == opcode::start_script || op == opcode::call_script) && pc + 2 < code_words)
            entry_slots = std::max(
                entry_slots,
                std::min<int64_t>(program.code[pc + 2], static_cast<int64_t>(limit::stack_slots))
            );
        pc += stack_effect(program, pc).words;
    }
    int64_t deepest = static_cast<int64_t>(stack_capacity);
    int64_t depth = entry_slots;
    std::size_t next_entry = 0;
    for (uint32_t pc = 0; pc < code_words;) {
        while (next_entry < scripts && sorted_entries[next_entry] <= pc) {
            if (sorted_entries[next_entry] == pc)
                depth = entry_slots;
            ++next_entry;
        }
        const auto effect = stack_effect(program, pc);
        depth = std::max<int64_t>(0, depth - effect.pops) + effect.pushes;
        deepest = std::max(deepest, depth);
        pc += effect.words;
    }
    return static_cast<std::size_t>(
        std::min<int64_t>(deepest, static_cast<int64_t>(limit::stack_slots))
    );
}

// Validates once. A refusal clears the code so nothing runs it, and records
// the same stack a VM used to give an empty program.
void prepare_in_place(Program& program) {
    if (program.prepared)
        return;
    const char* refusal = nullptr;
    if (program.code.empty() || program.code.size() > limit::code_words ||
        program.entry_points.size() > limit::scripts || program.static_count > limit::statics ||
        program.piece_count > limit::pieces)
        refusal = "script VM program exceeds its bounded representation";
    for (const auto entry : program.entry_points)
        if (refusal == nullptr && entry >= program.code.size())
            refusal = "script VM entry point is outside code";
    if (refusal != nullptr) {
        program = {};
        program.refusal = refusal;
        program.stack_slots = stack_capacity;
        program.prepared = true;
        return;
    }
    program.stack_slots = deepest_stack(program);
    program.prepared = true;
}

std::shared_ptr<const Program> adopt_program(std::shared_ptr<const Program> program) {
    if (program && program->prepared)
        return program;
    return prepare_program(program ? Program{*program} : Program{});
}
} // namespace

std::shared_ptr<const Program> prepare_program(Program program) {
    prepare_in_place(program);
    return std::make_shared<const Program>(std::move(program));
}

Vm::Vm(std::shared_ptr<const Program> program, int32_t time_scale)
    : program_(adopt_program(std::move(program))), time_scale_(time_scale) {
    if (!program_->refusal.empty()) {
        program_error_ =
            make_error(ErrorCode::invalid_program, context_count, 0, 0, program_->refusal);
    }
    statics_.resize(program_->static_count);
    motions_.resize(program_->piece_count * piece_axis_count);
    piece_active_.resize(program_->piece_count);
    stack_slots_ = program_->stack_slots;
    stack_block_ = std::make_unique<uint32_t[]>(stack_slots_ * context_count);
    for (std::size_t index = 0; index < contexts_.size(); ++index)
        contexts_[index].slots = {stack_block_.get() + index * stack_slots_, stack_slots_};
}

Vm::Vm(Program program, int32_t time_scale) : Vm(prepare_program(std::move(program)), time_scale) {
}

Vm::Vm(std::shared_ptr<const Program> program, Host& host, int32_t time_scale)
    : Vm(std::move(program), time_scale) {
    host_ = &host;
}

Vm::Vm(Program program, Host& host, int32_t time_scale)
    : Vm(prepare_program(std::move(program)), host, time_scale) {
}

std::optional<std::size_t> Vm::allocate_context(uint32_t script_index) {
    if (script_index >= program_->entry_points.size())
        return std::nullopt;
    for (std::size_t i = 0; i < contexts_.size(); ++i) {
        auto& context = contexts_[i];
        if (context.state != ContextState::stopped)
            continue;
        context.state = ContextState::active;
        context.pc = program_->entry_points[script_index];
        context.sp = -1;
        context.signal_mask = 1;
        context.return_callback = {};
        ++active_count_;
        return i;
    }
    return std::nullopt;
}

StartResult Vm::start(
    uint32_t script_index, std::span<const int32_t> arguments, ReturnCallback return_callback
) {
    if (script_index >= program_->entry_points.size()) {
        return {
            context_count,
            make_error(
                ErrorCode::invalid_script,
                context_count,
                0,
                0,
                "script index is outside the entry-point table"
            )
        };
    }
    if (arguments.size() > stack_capacity) {
        return {
            context_count,
            make_error(
                ErrorCode::stack_overflow,
                context_count,
                0,
                0,
                "initial arguments exceed the 32-slot context stack"
            )
        };
    }
    const auto allocated = allocate_context(script_index);
    if (!allocated) {
        return {
            context_count,
            make_error(
                ErrorCode::no_free_context, context_count, 0, 0, "all eight VM contexts are active"
            )
        };
    }
    auto& context = contexts_[*allocated];
    for (std::size_t i = 0; i < arguments.size(); ++i)
        context.slots[i] = as_unsigned(arguments[i]);
    context.sp = static_cast<int32_t>(arguments.size()) - 1;
    context.return_callback = std::move(return_callback);
    return {*allocated, std::nullopt};
}

StartResult Vm::start_parameterized(
    uint32_t script_index,
    const std::array<int32_t, 4>& locals,
    std::size_t argument_count,
    ReturnCallback callback
) {
    if (argument_count > locals.size())
        return {
            context_count,
            make_error(
                ErrorCode::invalid_argument_count,
                context_count,
                0,
                0,
                "parameterized engine call accepts at most four arguments"
            )
        };
    auto result = start(script_index, locals, std::move(callback));
    if (result.ok())
        contexts_[result.context].sp = static_cast<int32_t>(argument_count) - 1;
    return result;
}

QueryResult Vm::query(uint32_t script_index, const std::array<int32_t*, 4>& arguments) {
    if (script_index >= program_->entry_points.size()) {
        return {
            false,
            make_error(
                ErrorCode::invalid_script,
                context_count,
                0,
                0,
                "script index is outside the entry-point table"
            )
        };
    }
    const auto allocated = allocate_context(script_index);
    if (!allocated) {
        return {
            false,
            make_error(
                ErrorCode::no_free_context, context_count, 0, 0, "all eight VM contexts are active"
            )
        };
    }
    auto& context = contexts_[*allocated];
    context.return_callback = {};
    for (const int32_t* argument : arguments) {
        const auto value = argument == nullptr ? 0 : *argument;
        context.slots[static_cast<std::size_t>(++context.sp)] = as_unsigned(value);
    }
    context.sp = 3;
    auto executed = tick_context(*allocated, 0);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] != nullptr)
            *arguments[i] = as_signed(context.slots[i]);
    }
    return {true, std::move(executed.error)};
}

void Vm::wake_callers(std::size_t child) {
    for (auto& context : contexts_) {
        if (context.state == ContextState::waiting_for_child && context.child == child)
            context.state = ContextState::active;
    }
}

void Vm::stop_context(std::size_t index) {
    auto& context = contexts_[index];
    if (context.state == ContextState::stopped)
        return;
    context.state = ContextState::stopped;
    --active_count_;
    wake_callers(index);
}

void Vm::advance_motions(uint32_t elapsed) {
    if (!host_ || !motions_active_ || elapsed == 0)
        return;
    bool any_active = false;
    for (std::size_t piece = 0; piece < program_->piece_count; ++piece) {
        if (piece_active_[piece] == 0)
            continue;
        piece_active_[piece] = 0;
        for (std::size_t axis = 0; axis < piece_axis_count; ++axis) {
            auto& motion = motions_[piece * piece_axis_count + axis];
            if (motion.move_speed != 0) {
                const auto current = host_->piece_position(
                    static_cast<uint32_t>(piece), static_cast<uint32_t>(axis)
                );
                auto next =
                    as_signed(as_unsigned(current) + as_unsigned(motion.move_speed) * elapsed);
                const bool complete =
                    motion.move_speed < 0 ? next <= motion.move_target : next >= motion.move_target;
                if (complete) {
                    next = motion.move_target;
                    motion.move_speed = 0;
                } else {
                    piece_active_[piece] = 1;
                }
                host_->set_piece_position(
                    static_cast<uint32_t>(piece), static_cast<uint32_t>(axis), next
                );
            }

            if (motion.spin_acceleration != 0) {
                auto next_speed = as_signed(
                    as_unsigned(motion.turn_speed) + as_unsigned(motion.spin_acceleration)
                );
                const bool reached = motion.spin_acceleration < 0
                                         ? next_speed <= motion.spin_target_speed
                                         : next_speed >= motion.spin_target_speed;
                if (reached) {
                    next_speed = motion.spin_target_speed;
                    motion.spin_acceleration = 0;
                }
                motion.turn_speed = next_speed;
            }

            if (motion.turn_speed != 0) {
                const auto current_raw =
                    host_->piece_angle(static_cast<uint32_t>(piece), static_cast<uint32_t>(axis));
                const auto delta = as_signed(as_unsigned(motion.turn_speed) * elapsed);
                auto next = (as_unsigned(current_raw) + as_unsigned(delta)) & unit::angle_mask;
                if (motion.turn_target != unit::continuous_spin_target) {
                    const auto dividend = motion.turn_speed < 0
                                              ? as_signed(
                                                    as_unsigned(current_raw) - motion.turn_target +
                                                    unit::angle_units_per_revolution
                                                )
                                              : as_signed(
                                                    motion.turn_target - as_unsigned(current_raw) +
                                                    unit::angle_units_per_revolution
                                                );
                    const auto distance =
                        dividend % static_cast<int32_t>(unit::angle_units_per_revolution);
                    const auto threshold =
                        motion.turn_speed < 0 ? as_signed(0U - as_unsigned(delta)) : delta;
                    if (distance <= threshold) {
                        next = motion.turn_target;
                        motion.turn_speed = 0;
                    } else {
                        piece_active_[piece] = 1;
                    }
                } else {
                    piece_active_[piece] = 1;
                }
                host_->set_piece_angle(
                    static_cast<uint32_t>(piece), static_cast<uint32_t>(axis), next
                );
            }
        }
        if (piece_active_[piece] != 0)
            any_active = true;
    }
    motions_active_ = any_active;
}

std::optional<Error>
Vm::execute_context(std::size_t index, std::size_t& instructions, std::size_t instruction_budget) {
    auto& context = contexts_[index];
    while (context.state == ContextState::active) {
        if (instructions >= instruction_budget) {
            return make_error(
                ErrorCode::instruction_limit, index, context.pc, 0, "instruction budget exhausted"
            );
        }
        if (context.pc >= program_->code.size()) {
            return make_error(
                ErrorCode::program_counter_out_of_range,
                index,
                context.pc,
                0,
                "program counter is outside code"
            );
        }

        const uint32_t pc = context.pc;
        const uint32_t raw = program_->code[pc];
        const uint32_t op = raw & opcode_dispatch_mask;
        auto fail = [&](ErrorCode code, std::string message) -> std::optional<Error> {
            return make_error(code, index, pc, raw, std::move(message));
        };
        auto need_words = [&](std::size_t count) { return count <= program_->code.size() - pc; };
        auto need_stack = [&](int32_t count) { return context.sp + 1 >= count; };
        auto has_push_room = [&] {
            return context.sp + 1 < static_cast<int32_t>(context.slots.size());
        };
        auto valid_slot = [&](uint32_t slot) { return slot < context.slots.size(); };
        auto valid_static = [&](uint32_t slot) { return slot < statics_.size(); };
        auto valid_piece = [&](uint32_t piece) { return piece < program_->piece_count; };
        auto valid_axis = [](uint32_t axis) { return axis < piece_axis_count; };
        auto motion_at = [&](uint32_t piece, uint32_t axis) -> AxisMotion& {
            return motions_[static_cast<std::size_t>(piece) * piece_axis_count + axis];
        };
        auto top = [&](int32_t depth = 0) -> uint32_t& {
            return context.slots[static_cast<std::size_t>(context.sp - depth)];
        };

        switch (op) {
        case opcode::ignored_piece_op: {
            if (!host_)
                return fail(ErrorCode::missing_host, "opcode 0x10009000 requires a Host");
            if (!need_words(2))
                return fail(
                    ErrorCode::truncated_instruction, "opcode 0x10009000 lacks its inline operand"
                );
            if (!need_stack(2))
                return fail(ErrorCode::stack_underflow, "opcode 0x10009000 requires two values");
            const auto second = top();
            const auto first = top(1);
            context.sp -= 2;
            host_->ignored_piece_op(program_->code[pc + 1], first, second);
            context.pc += 2;
            break;
        }
        case opcode::dont_shadow: {
            if (!host_)
                return fail(ErrorCode::missing_host, "opcode 0x1000A000 requires a Host");
            if (!need_words(2))
                return fail(
                    ErrorCode::truncated_instruction, "opcode 0x1000A000 lacks its inline operand"
                );
            host_->dont_shadow(program_->code[pc + 1]);
            context.pc += 2;
            break;
        }
        case opcode::is_carrying_unit: {
            if (!host_)
                return fail(ErrorCode::missing_host, "opcode 0x10044000 requires a Host");
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "opcode 0x10044000 requires one value");
            const auto input = top();
            --context.sp;
            const auto output = host_->is_carrying_unit(input);
            context.slots[static_cast<std::size_t>(++context.sp)] = output;
            ++context.pc;
            break;
        }
        case opcode::carrier_unit_id: {
            if (!host_)
                return fail(ErrorCode::missing_host, "opcode 0x10045000 requires a Host");
            if (!has_push_room())
                return fail(
                    ErrorCode::stack_overflow, "opcode 0x10045000 result exceeds the stack"
                );
            const auto value = host_->carrier_unit_id();
            context.slots[static_cast<std::size_t>(++context.sp)] = value;
            ++context.pc;
            break;
        }
        case opcode::move:
        case opcode::turn:
        case opcode::spin:
        case opcode::stop_spin:
        case opcode::move_now:
        case opcode::turn_now: {
            if (!host_)
                return fail(ErrorCode::missing_host, "piece transform opcode requires a Host");
            if (!need_words(3))
                return fail(
                    ErrorCode::truncated_instruction, "piece transform lacks piece/axis operands"
                );
            const auto piece = program_->code[pc + 1];
            const auto axis = program_->code[pc + 2];
            if (!valid_piece(piece))
                return fail(ErrorCode::invalid_piece, "piece index is out of range");
            if (!valid_axis(axis))
                return fail(ErrorCode::invalid_axis, "piece axis is outside 0..2");
            const int32_t pop_count =
                (op == opcode::move || op == opcode::turn || op == opcode::spin) ? 2 : 1;
            if (!need_stack(pop_count))
                return fail(ErrorCode::stack_underflow, "piece transform lacks stack operands");
            if (time_scale_ == 0 && op != opcode::move_now && op != opcode::turn_now)
                return fail(
                    ErrorCode::invalid_time_scale, "piece transform divides by a zero time scale"
                );
            auto unsafe_scale = [&](uint32_t value) {
                return time_scale_ == -1 && as_signed(value) == std::numeric_limits<int32_t>::min();
            };
            if ((op == opcode::move || op == opcode::turn) && unsafe_scale(top(1)))
                return fail(
                    ErrorCode::signed_divide_overflow, "piece speed scaling would overflow"
                );
            if (op == opcode::spin && (unsafe_scale(top()) || unsafe_scale(top(1))))
                return fail(ErrorCode::signed_divide_overflow, "spin scaling would overflow");
            if (op == opcode::stop_spin && unsafe_scale(top()))
                return fail(
                    ErrorCode::signed_divide_overflow, "spin deceleration scaling would overflow"
                );
            auto negate = [](int32_t value) { return as_signed(0U - as_unsigned(value)); };

            auto& motion = motion_at(piece, axis);
            if (op == opcode::move) {
                const auto target = as_signed(top());
                auto speed = as_signed(top(1)) / time_scale_;
                motion.move_target = target;
                motion.move_speed = speed;
                context.sp -= 2;
                const auto current = host_->piece_position(piece, axis);
                if (target < current)
                    speed = negate(speed);
                motion.move_speed = speed;
                piece_active_[piece] = 1;
                motions_active_ = true;
            } else if (op == opcode::turn) {
                const auto target = top() & unit::angle_mask;
                auto speed = as_signed(top(1)) / time_scale_;
                motion.turn_target = target;
                motion.turn_speed = speed;
                motion.spin_acceleration = 0;
                context.sp -= 2;
                const auto current = host_->piece_angle(piece, axis);
                const auto difference = as_signed(target - as_unsigned(current));
                if (difference == 0) {
                    speed = 0;
                } else {
                    const auto magnitude = difference < 0 ? negate(difference) : difference;
                    if ((magnitude > static_cast<int32_t>(unit::angle_units_per_revolution / 2U)) !=
                        (difference < 0)) {
                        speed = negate(speed);
                    }
                }
                motion.turn_speed = speed;
                piece_active_[piece] = 1;
                motions_active_ = true;
            } else if (op == opcode::spin) {
                motion.turn_target = unit::continuous_spin_target;
                motion.spin_target_speed = as_signed(top()) / time_scale_;
                motion.spin_acceleration = as_signed(top(1)) / time_scale_;
                if (motion.spin_acceleration == 0)
                    motion.turn_speed = motion.spin_target_speed;
                context.sp -= 2;
                piece_active_[piece] = 1;
                motions_active_ = true;
            } else if (op == opcode::stop_spin) {
                motion.spin_target_speed = 0;
                motion.spin_acceleration = negate(as_signed(top()) / time_scale_);
                if (motion.spin_acceleration == 0)
                    motion.turn_speed = 0;
                --context.sp;
            } else if (op == opcode::move_now) {
                const auto value = as_signed(top());
                motion.move_target = value;
                motion.move_speed = 0;
                --context.sp;
                host_->set_piece_position(piece, axis, value);
            } else {
                const auto value = top() & unit::angle_mask;
                motion.turn_target = value;
                motion.turn_speed = 0;
                motion.spin_acceleration = 0;
                --context.sp;
                host_->set_piece_angle(piece, axis, value);
            }
            context.pc += 3;
            break;
        }
        case opcode::wait_turn:
        case opcode::wait_move: {
            if (!need_words(3))
                return fail(ErrorCode::truncated_instruction, "WAIT lacks piece/axis operands");
            const auto piece = program_->code[pc + 1];
            const auto axis = program_->code[pc + 2];
            if (!valid_piece(piece))
                return fail(ErrorCode::invalid_piece, "WAIT piece index is out of range");
            if (!valid_axis(axis))
                return fail(ErrorCode::invalid_axis, "WAIT axis is outside 0..2");
            context.wait_piece = piece;
            context.wait_axis = axis;
            context.pc += 3;
            context.state = op == opcode::wait_turn ? ContextState::waiting_for_turn
                                                    : ContextState::waiting_for_move;
            break;
        }
        case opcode::show:
        case opcode::hide:
        case opcode::cache:
        case opcode::dont_cache:
        case opcode::shade:
        case opcode::dont_shade: {
            if (!host_)
                return fail(ErrorCode::missing_host, "piece flag opcode requires a Host");
            if (!need_words(2))
                return fail(
                    ErrorCode::truncated_instruction, "piece flag opcode lacks a piece operand"
                );
            const auto piece = program_->code[pc + 1];
            if (!valid_piece(piece))
                return fail(ErrorCode::invalid_piece, "piece index is out of range");
            if (op == opcode::show || op == opcode::hide)
                host_->set_piece_visible(piece, op == opcode::show);
            else if (op == opcode::cache || op == opcode::dont_cache)
                host_->set_piece_cached(piece, op == opcode::cache);
            else
                host_->set_piece_shaded(piece, op == opcode::shade);
            context.pc += 2;
            break;
        }
        case opcode::emit_sfx:
        case opcode::explode: {
            if (!host_)
                return fail(ErrorCode::missing_host, "piece effect opcode requires a Host");
            if (!need_words(2))
                return fail(ErrorCode::truncated_instruction, "piece effect lacks a piece operand");
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "piece effect requires one value");
            const auto piece = program_->code[pc + 1];
            if (!valid_piece(piece))
                return fail(ErrorCode::invalid_piece, "piece index is out of range");
            const auto value = as_signed(top());
            --context.sp;
            if (op == opcode::emit_sfx)
                host_->emit_sfx(piece, value);
            else
                host_->explode(piece, value);
            context.pc += 2;
            break;
        }
        case opcode::get_unit_value:
        case opcode::get: {
            if (!host_)
                return fail(ErrorCode::missing_host, "unit query opcode requires a Host");
            const int32_t argument_count = op == opcode::get_unit_value ? 1 : 5;
            if (!need_stack(argument_count))
                return fail(ErrorCode::stack_underflow, "unit query lacks stack arguments");
            std::array<int32_t, 5> arguments{};
            for (int32_t argument = argument_count - 1; argument >= 0; --argument)
                arguments[static_cast<std::size_t>(argument)] =
                    as_signed(top(argument_count - 1 - argument));
            context.sp -= argument_count;
            const auto value = host_->get_unit_value(
                arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]
            );
            context.slots[static_cast<std::size_t>(++context.sp)] = as_unsigned(value);
            ++context.pc;
            break;
        }
        case opcode::set_unit_value: {
            if (!host_)
                return fail(ErrorCode::missing_host, "SET_VALUE requires a Host");
            if (!need_stack(2))
                return fail(ErrorCode::stack_underflow, "SET_VALUE requires two values");
            const auto value = as_signed(top());
            const auto selector = as_signed(top(1));
            context.sp -= 2;
            host_->set_unit_value(selector, value);
            ++context.pc;
            break;
        }
        case opcode::attach_unit: {
            if (!host_)
                return fail(ErrorCode::missing_host, "ATTACH_UNIT requires a Host");
            if (!need_stack(3))
                return fail(ErrorCode::stack_underflow, "ATTACH_UNIT requires three values");
            const auto third = as_signed(top());
            const auto second = as_signed(top(1));
            const auto first = as_signed(top(2));
            context.sp -= 3;
            host_->attach_unit(first, second, third);
            ++context.pc;
            break;
        }
        case opcode::drop_unit: {
            if (!host_)
                return fail(ErrorCode::missing_host, "DROP_UNIT requires a Host");
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "DROP_UNIT requires one value");
            const auto unit = as_signed(top());
            --context.sp;
            host_->drop_unit(unit);
            ++context.pc;
            break;
        }
        case 0x10021000U: {
            if (!need_words(2))
                return fail(ErrorCode::truncated_instruction, "PUSH lacks its operand");
            if (!has_push_room())
                return fail(ErrorCode::stack_overflow, "PUSH exceeds the context's stack");
            const auto flag = raw & 7U;
            const auto operand = program_->code[pc + 1];
            uint32_t value = 0;
            if (flag == 1U)
                value = operand;
            else if (flag == 2U) {
                if (!valid_slot(operand))
                    return fail(
                        ErrorCode::invalid_local, "PUSH_LOCAL index is past the context's stack"
                    );
                value = context.slots[operand];
            } else if (flag == 4U) {
                if (!valid_static(operand))
                    return fail(ErrorCode::invalid_static, "PUSH_STATIC index is out of range");
                value = statics_[operand];
            } else {
                return fail(ErrorCode::unsupported_opcode, "unsupported PUSH flag combination");
            }
            context.slots[static_cast<std::size_t>(++context.sp)] = value;
            context.pc += 2;
            break;
        }
        case opcode::stack_alloc: {
            if (!has_push_room())
                return fail(ErrorCode::stack_overflow, "STACK_ALLOC exceeds the context's stack");
            ++context.sp;
            ++context.pc;
            break;
        }
        case 0x10023000U: {
            if (!need_words(2))
                return fail(ErrorCode::truncated_instruction, "POP lacks its destination");
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "POP requires one value");
            const auto flag = raw & 7U;
            const auto destination = program_->code[pc + 1];
            if (flag == 2U) {
                if (!valid_slot(destination))
                    return fail(
                        ErrorCode::invalid_local, "POP_LOCAL index is past the context's stack"
                    );
            } else if (flag == 4U) {
                if (!valid_static(destination))
                    return fail(ErrorCode::invalid_static, "POP_STATIC index is out of range");
            } else {
                return fail(ErrorCode::unsupported_opcode, "unsupported POP flag combination");
            }
            const auto value = top();
            if (flag == 2U)
                context.slots[destination] = value;
            else
                statics_[destination] = value;
            --context.sp;
            context.pc += 2;
            break;
        }
        case opcode::pop_stack: {
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "POP_STACK requires one value");
            --context.sp;
            ++context.pc;
            break;
        }
        case opcode::bit_not:
        case opcode::logical_not: {
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "unary operation requires one value");
            top() = op == opcode::bit_not ? ~top() : static_cast<uint32_t>(top() == 0);
            ++context.pc;
            break;
        }
        case opcode::random: {
            if (!host_)
                return fail(ErrorCode::missing_host, "RANDOM requires shared Host RNG state");
            if (!need_stack(2))
                return fail(ErrorCode::stack_underflow, "RANDOM requires lower and upper bounds");
            const auto upper = top();
            const auto lower = top(1);
            const auto bound = upper - lower + 1U;
            context.sp -= 2;
            context.slots[static_cast<std::size_t>(++context.sp)] =
                host_->random_bounded(bound) + lower;
            ++context.pc;
            break;
        }
        case opcode::add:
        case opcode::subtract:
        case opcode::multiply:
        case opcode::divide:
        case opcode::bit_and:
        case opcode::bit_or:
        case opcode::bit_xor:
        case opcode::less:
        case opcode::less_equal:
        case opcode::greater:
        case opcode::greater_equal:
        case opcode::equal:
        case opcode::not_equal:
        case opcode::logical_and:
        case opcode::logical_or:
        case opcode::logical_xor: {
            if (!need_stack(2))
                return fail(ErrorCode::stack_underflow, "binary operation requires two values");
            const auto lhs = top(1);
            const auto rhs = top();
            uint32_t result = 0;
            if (op == opcode::add)
                result = lhs + rhs;
            else if (op == opcode::subtract)
                result = lhs - rhs;
            else if (op == opcode::multiply)
                result = lhs * rhs;
            else if (op == opcode::divide) {
                if (rhs == 0)
                    return fail(ErrorCode::divide_by_zero, "signed DIV divisor is zero");
                if (as_signed(lhs) == std::numeric_limits<int32_t>::min() && as_signed(rhs) == -1)
                    return fail(ErrorCode::signed_divide_overflow, "signed DIV would overflow");
                result = as_unsigned(as_signed(lhs) / as_signed(rhs));
            } else if (op == opcode::bit_and)
                result = lhs & rhs;
            else if (op == opcode::bit_or)
                result = lhs | rhs;
            else if (op == opcode::bit_xor || op == opcode::logical_xor)
                result = lhs ^ rhs;
            else if (op == opcode::less)
                result = as_signed(lhs) < as_signed(rhs);
            else if (op == opcode::less_equal)
                result = as_signed(lhs) <= as_signed(rhs);
            else if (op == opcode::greater)
                result = as_signed(lhs) > as_signed(rhs);
            else if (op == opcode::greater_equal)
                result = as_signed(lhs) >= as_signed(rhs);
            else if (op == opcode::equal)
                result = lhs == rhs;
            else if (op == opcode::not_equal)
                result = lhs != rhs;
            else if (op == opcode::logical_and)
                result = lhs != 0 && rhs != 0;
            else if (op == opcode::logical_or)
                result = lhs != 0 || rhs != 0;
            top(1) = result;
            --context.sp;
            ++context.pc;
            break;
        }
        case opcode::sleep: {
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "SLEEP requires a duration");
            const auto product = as_unsigned(time_scale_) * top();
            context.sleep_remaining = as_signed(product) / unit::milliseconds_per_second;
            --context.sp;
            ++context.pc;
            context.state = ContextState::sleeping;
            break;
        }
        case opcode::jump: {
            if (!need_words(2))
                return fail(ErrorCode::truncated_instruction, "JUMP lacks its target");
            const auto target = program_->code[pc + 1];
            if (target >= program_->code.size())
                return fail(ErrorCode::invalid_jump, "JUMP target is outside code");
            context.pc = target;
            break;
        }
        case opcode::jump_if_false: {
            if (!need_words(2))
                return fail(ErrorCode::truncated_instruction, "JUMP_IF_FALSE lacks its target");
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "JUMP_IF_FALSE requires a condition");
            const auto condition = top();
            const auto target = program_->code[pc + 1];
            if (condition == 0 && target >= program_->code.size())
                return fail(ErrorCode::invalid_jump, "taken JUMP_IF_FALSE target is outside code");
            --context.sp;
            context.pc = condition == 0 ? target : pc + 2;
            break;
        }
        case opcode::start_script:
        case opcode::call_script: {
            if (!need_words(3))
                return fail(
                    ErrorCode::truncated_instruction, "script launch lacks index/count operands"
                );
            const auto script = program_->code[pc + 1];
            const auto argument_count = program_->code[pc + 2];
            // An invalid index or a full pool claims no context: the arguments
            // stay on the caller's stack and CALL_SCRIPT waits on no child.
            const auto child = allocate_context(script);
            if (child) {
                // The caller's stack must hold the arguments.
                if (argument_count > stack_capacity ||
                    !need_stack(static_cast<int32_t>(argument_count))) {
                    stop_context(*child);
                    return fail(
                        ErrorCode::stack_underflow, "script launch arguments are unavailable"
                    );
                }
                auto& child_context = contexts_[*child];
                for (std::size_t destination = argument_count; destination > 0; --destination) {
                    child_context.slots[destination - 1] = top();
                    --context.sp;
                }
                child_context.signal_mask = context.signal_mask;
            }
            context.pc += 3;
            if (op == opcode::call_script) {
                context.state = ContextState::waiting_for_child;
                context.child = child ? *child : no_child_context;
            }
            break;
        }
        case opcode::discard_call: {
            if (!need_words(3))
                return fail(
                    ErrorCode::truncated_instruction, "opcode 0x10063000 lacks inline operands"
                );
            const auto argument_count = program_->code[pc + 2];
            if (argument_count > 4)
                return fail(
                    ErrorCode::invalid_argument_count,
                    "opcode 0x10063000 exceeds its four-word local buffer"
                );
            if (!need_stack(static_cast<int32_t>(argument_count)))
                return fail(
                    ErrorCode::stack_underflow, "opcode 0x10063000 arguments are unavailable"
                );
            context.sp -= static_cast<int32_t>(argument_count);
            context.pc += 3;
            break;
        }
        case opcode::return_: {
            if (context.return_callback) {
                if (!need_stack(1))
                    return fail(ErrorCode::stack_underflow, "callback RETURN requires one value");
                const auto value = as_signed(top());
                --context.sp;
                context.return_callback(value);
            }
            stop_context(index);
            break;
        }
        case opcode::set_signal_mask: {
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "SET_SIGNAL_MASK requires a mask");
            context.signal_mask = top();
            --context.sp;
            ++context.pc;
            break;
        }
        case opcode::signal: {
            if (!need_stack(1))
                return fail(ErrorCode::stack_underflow, "SIGNAL requires a mask");
            const auto signal_mask = top();
            --context.sp;
            for (std::size_t target = 0; target < contexts_.size(); ++target) {
                if (contexts_[target].state != ContextState::stopped &&
                    (contexts_[target].signal_mask & signal_mask) != 0) {
                    stop_context(target);
                }
            }
            ++context.pc;
            break;
        }
        default:
            return fail(ErrorCode::unsupported_opcode, "opcode is outside the supported subset");
        }
        ++instructions;
    }
    return std::nullopt;
}

TickResult Vm::tick(uint32_t elapsed, std::size_t instruction_budget) {
    TickResult result;
    if (instruction_budget == 0) {
        result.error = make_error(
            ErrorCode::instruction_limit, context_count, 0, 0, "instruction budget is zero"
        );
        return result;
    }
    // The thread pass visits the fixed eight-context array in ascending order.
    for (std::size_t i = 0; i < contexts_.size(); ++i) {
        auto step = tick_context(i, elapsed, instruction_budget - result.instructions);
        result.instructions += step.instructions;
        if (!step.error)
            continue;
        // An instruction outside the machine's set stops its context, and
        // the tick goes on with the other contexts and the pieces' motion;
        // the tick still reports it.
        if (step.error->code == ErrorCode::unsupported_opcode) {
            stop_context(i);
            if (!result.error)
                result.error = std::move(step.error);
            continue;
        }
        result.error = std::move(step.error);
        return result;
    }
    advance_motions(elapsed);
    return result;
}

TickResult Vm::tick_context(std::size_t index, uint32_t elapsed, std::size_t instruction_budget) {
    TickResult result;
    if (index >= contexts_.size()) {
        result.error = make_error(
            ErrorCode::invalid_state,
            index,
            0,
            0,
            "context index is outside the eight-context array"
        );
        return result;
    }
    auto& context = contexts_[index];
    if (context.state == ContextState::waiting_for_turn ||
        context.state == ContextState::waiting_for_move) {
        const auto& motion = motions_
            [static_cast<std::size_t>(context.wait_piece) * piece_axis_count + context.wait_axis];
        const bool complete = context.state == ContextState::waiting_for_turn
                                  ? motion.turn_speed == 0
                                  : motion.move_speed == 0;
        if (complete)
            context.state = ContextState::active;
    }
    if (context.state == ContextState::sleeping) {
        context.sleep_remaining = as_signed(as_unsigned(context.sleep_remaining) - elapsed);
        if (context.sleep_remaining <= 0)
            context.state = ContextState::active;
    }
    if (context.state != ContextState::active)
        return result;
    result.error = execute_context(index, result.instructions, instruction_budget);
    return result;
}

std::size_t Vm::active_count() const noexcept {
    return active_count_;
}

StateResult Vm::export_state(uint32_t script_identity_token) const {
    if (program_->piece_count != 0 && host_ == nullptr) {
        return {
            std::nullopt,
            make_error(
                ErrorCode::missing_host,
                context_count,
                0,
                0,
                "exporting piece state requires a Host"
            )
        };
    }

    sim::script_state::State state;
    state.script_identity_token = script_identity_token;
    state.active_context_count = static_cast<uint32_t>(active_count_);
    for (std::size_t index = 0; index < contexts_.size(); ++index) {
        const auto& source = contexts_[index];
        auto& destination = state.contexts[index];
        destination.execution_state = static_cast<uint32_t>(source.state);
        destination.program_counter = source.pc;
        destination.stack_pointer = source.sp;
        destination.sleep_remaining = source.sleep_remaining;
        destination.wait_piece = source.wait_piece;
        destination.wait_axis = source.wait_axis;
        destination.child_context = static_cast<uint32_t>(source.child);
        destination.signal_mask = source.signal_mask;
        // The record keeps the first 32 slots; a context saved while an
        // expression holds more loses the rest.
        for (std::size_t slot = 0; slot < destination.slots.size(); ++slot) {
            destination.slots[slot] = as_signed(source.slots[slot]);
        }
    }

    state.statics.resize(statics_.size());
    for (std::size_t index = 0; index < statics_.size(); ++index) {
        state.statics[index] = as_signed(statics_[index]);
    }

    state.pieces.resize(program_->piece_count);
    for (std::size_t piece = 0; piece < state.pieces.size(); ++piece) {
        auto& destination = state.pieces[piece];
        // The three piece flags are read before the per-axis transforms;
        // hosts see the calls in that order.
        destination.visible = host_->piece_visible(static_cast<uint32_t>(piece));
        destination.cached = host_->piece_cached(static_cast<uint32_t>(piece));
        destination.shaded = host_->piece_shaded(static_cast<uint32_t>(piece));
        for (std::size_t axis = 0; axis < piece_axis_count; ++axis) {
            const auto& source = motions_[piece * piece_axis_count + axis];
            auto& destination_axis = destination.axes[axis];
            destination_axis.move_target = source.move_target;
            destination_axis.move_speed_per_tick = source.move_speed;
            destination_axis.turn_target = source.turn_target;
            destination_axis.turn_speed_per_tick = source.turn_speed;
            destination_axis.spin_target_speed_per_tick = source.spin_target_speed;
            destination_axis.spin_acceleration_per_update = source.spin_acceleration;
            destination_axis.position =
                host_->piece_position(static_cast<uint32_t>(piece), static_cast<uint32_t>(axis));
            destination_axis.angle = as_unsigned(
                host_->piece_angle(static_cast<uint32_t>(piece), static_cast<uint32_t>(axis))
            );
        }
    }
    return {std::move(state), std::nullopt};
}

std::optional<Error> Vm::import_state(const sim::script_state::State& state) {
    if (state.statics.size() != statics_.size() || state.pieces.size() != program_->piece_count) {
        return make_error(
            ErrorCode::invalid_state,
            context_count,
            0,
            0,
            "saved static or piece count does not match the loaded program"
        );
    }
    if (program_->piece_count != 0 && host_ == nullptr) {
        return make_error(
            ErrorCode::missing_host, context_count, 0, 0, "restoring piece state requires a Host"
        );
    }

    std::size_t counted_active = 0;
    for (std::size_t index = 0; index < state.contexts.size(); ++index) {
        const auto& context = state.contexts[index];
        const auto execution_state = static_cast<ContextState>(context.execution_state);
        const bool known_state = execution_state == ContextState::stopped ||
                                 execution_state == ContextState::active ||
                                 execution_state == ContextState::sleeping ||
                                 execution_state == ContextState::waiting_for_child ||
                                 execution_state == ContextState::waiting_for_turn ||
                                 execution_state == ContextState::waiting_for_move;
        if (!known_state) {
            return make_error(
                ErrorCode::invalid_state,
                index,
                context.program_counter,
                0,
                "saved context has an unknown execution-state word"
            );
        }
        if (context.stack_pointer < -1 ||
            context.stack_pointer >= static_cast<int32_t>(stack_slots_)) {
            return make_error(
                ErrorCode::invalid_state,
                index,
                context.program_counter,
                0,
                "saved context stack pointer is past the context's stack"
            );
        }
        if ((execution_state == ContextState::waiting_for_turn ||
             execution_state == ContextState::waiting_for_move) &&
            (context.wait_piece >= program_->piece_count ||
             context.wait_axis >= piece_axis_count)) {
            return make_error(
                ErrorCode::invalid_state,
                index,
                context.program_counter,
                0,
                "saved wait target is outside the loaded piece table"
            );
        }
        if (execution_state != ContextState::stopped) {
            ++counted_active;
        }
    }
    if (state.active_context_count != counted_active) {
        return make_error(
            ErrorCode::invalid_state,
            context_count,
            0,
            0,
            "saved active count does not match its context states"
        );
    }

    for (std::size_t index = 0; index < contexts_.size(); ++index) {
        const auto& source = state.contexts[index];
        auto& destination = contexts_[index];
        destination.state = static_cast<ContextState>(source.execution_state);
        destination.pc = source.program_counter;
        destination.sp = source.stack_pointer;
        destination.sleep_remaining = source.sleep_remaining;
        destination.wait_piece = source.wait_piece;
        destination.wait_axis = source.wait_axis;
        destination.child = source.child_context;
        destination.signal_mask = source.signal_mask;
        destination.return_callback = {};
        // A stack saved deeper than the record's 32 slots is zero past them.
        for (std::size_t slot = 0; slot < destination.slots.size(); ++slot) {
            destination.slots[slot] =
                slot < source.slots.size() ? as_unsigned(source.slots[slot]) : 0U;
        }
    }
    active_count_ = counted_active;
    for (std::size_t index = 0; index < statics_.size(); ++index) {
        statics_[index] = as_unsigned(state.statics[index]);
    }

    for (std::size_t piece = 0; piece < state.pieces.size(); ++piece) {
        const auto& source = state.pieces[piece];
        piece_active_[piece] = 1;
        // Flags are restored before motion arrays and transforms.
        host_->set_piece_visible(static_cast<uint32_t>(piece), source.visible);
        host_->set_piece_cached(static_cast<uint32_t>(piece), source.cached);
        host_->set_piece_shaded(static_cast<uint32_t>(piece), source.shaded);
        for (std::size_t axis = 0; axis < piece_axis_count; ++axis) {
            const auto& source_axis = source.axes[axis];
            auto& destination = motions_[piece * piece_axis_count + axis];
            destination.move_target = source_axis.move_target;
            destination.move_speed = source_axis.move_speed_per_tick;
            destination.turn_target = source_axis.turn_target;
            destination.turn_speed = source_axis.turn_speed_per_tick;
            destination.spin_target_speed = source_axis.spin_target_speed_per_tick;
            destination.spin_acceleration = source_axis.spin_acceleration_per_update;
            host_->set_piece_position(
                static_cast<uint32_t>(piece), static_cast<uint32_t>(axis), source_axis.position
            );
            host_->set_piece_angle(
                static_cast<uint32_t>(piece), static_cast<uint32_t>(axis), source_axis.angle
            );
        }
    }
    // Motion scheduling is on even when no restored motion is active.
    motions_active_ = true;
    return std::nullopt;
}

ContextSnapshot Vm::context(std::size_t index) const {
    if (index >= contexts_.size())
        return {};
    const auto& source = contexts_[index];
    ContextSnapshot result;
    result.state = source.state;
    result.pc = source.pc;
    result.stack_pointer = source.sp;
    result.sleep_remaining = source.sleep_remaining;
    result.signal_mask = source.signal_mask;
    result.child = source.child;
    result.wait_piece = source.wait_piece;
    result.wait_axis = source.wait_axis;
    for (std::size_t i = 0; i < result.slots.size(); ++i)
        result.slots[i] = as_signed(source.slots[i]);
    return result;
}

std::optional<int32_t> Vm::static_value(std::size_t index) const noexcept {
    if (index >= statics_.size())
        return std::nullopt;
    return as_signed(statics_[index]);
}

bool Vm::set_static(std::size_t index, int32_t value) noexcept {
    if (index >= statics_.size())
        return false;
    statics_[index] = as_unsigned(value);
    return true;
}

int32_t Vm::turn_speed(uint32_t piece, uint32_t axis) const noexcept {
    if (axis >= piece_axis_count || static_cast<std::size_t>(piece) >= program_->piece_count)
        return 0;
    return motions_[static_cast<std::size_t>(piece) * piece_axis_count + axis].turn_speed;
}

int32_t find_script(std::span<const char* const> script_names, const char* name) noexcept {
    if (name == nullptr)
        return -1;
    for (std::size_t index = 0; index < script_names.size(); ++index) {
        const char* entry = script_names[index];
        if (entry == nullptr)
            return -1;
        if (std::strcmp(name, entry) == 0)
            return static_cast<int32_t>(index);
    }
    return -1;
}

} // namespace oa::sim::script_vm
