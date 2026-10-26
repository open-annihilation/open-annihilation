// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime/script.hpp"
#include "oa/sim/unit_script.hpp"
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oa::sim::match_runtime {
uint32_t SharedRandom::bounded(uint32_t bound) noexcept {
    if (std::bit_cast<int32_t>(bound) < 2)
        return 0;
    state_ = state_ * 0x41a7u - (state_ / 0x1f31du) * 0x7fffffffu;
    if (std::bit_cast<int32_t>(state_) < 1)
        state_ += 0x7fffffffu;
    return state_ % bound;
}

namespace {
int32_t named_script(const formats::cob::CobProgram& program, std::string_view name) noexcept {
    for (std::size_t i = 0; i < program.scripts.size(); ++i)
        if (program.scripts[i].name == name)
            return static_cast<int32_t>(i);
    return -1;
}

// The program a script instance made without one runs: no code, which the VM
// refuses, so no script starts.
std::shared_ptr<const formats::cob::CobProgram>
held_program(std::shared_ptr<const formats::cob::CobProgram> program) {
    return program ? std::move(program) : std::make_shared<const formats::cob::CobProgram>();
}
} // namespace

SharedScriptProgram
prepare_type_script(const std::shared_ptr<const formats::cob::CobProgram>& program) {
    SharedScriptProgram made;
    if (!program) {
        made.vm_program = sim::script_vm::prepare_program({});
        made.function_index.fill(-1);
        return made;
    }
    made.vm_program = sim::script_vm::prepare_program(
        {program->code,
         program->entry_points,
         program->header.static_variable_count,
         program->piece_names.size()}
    );
    for (std::size_t index = 0; index < made.function_index.size(); ++index) {
        made.function_index[index] = named_script(
            *program, sim::unit_script::script_function_name(static_cast<ScriptFunction>(index))
        );
    }
    return made;
}

ScriptInstance::ScriptInstance(
    std::shared_ptr<const formats::cob::CobProgram> program,
    const SharedScriptProgram& shared,
    sim::script_vm::Host& host,
    int32_t scale,
    MatchFault* fault
)
    : program_(held_program(std::move(program))), vm_(shared.vm_program, host, scale),
      fault_(fault) {
    if (const auto& refused = vm_.program_error())
        note("script program refused", refused->message);
    function_index_ = shared.function_index;
}

void ScriptInstance::note(std::string_view what, std::string_view detail) noexcept {
    if (fault_)
        fault_->note(what, detail);
}

bool ScriptInstance::start_no_arguments(
    int32_t index, bool immediate, sim::script_vm::ReturnCallback callback
) {
    if (index < 0)
        return false;
    const auto result = vm_.start(static_cast<uint32_t>(index), {}, std::move(callback));
    if (!result.ok()) {
        if (result.error->code != sim::script_vm::ErrorCode::no_free_context)
            note("script start", result.error->message);
        return false;
    }
    if (immediate)
        tick(0);
    return true;
}

bool ScriptInstance::start_with_arguments(
    int32_t index,
    std::string_view name,
    std::span<const int32_t> args,
    bool immediate,
    sim::script_vm::ReturnCallback callback
) {
    if (args.size() > 4) {
        note("a named script call accepts at most four arguments", name);
        if (callback)
            callback(0);
        return false;
    }
    std::array<int32_t, 4> locals{};
    std::copy(args.begin(), args.end(), locals.begin());
    return start_with_locals(index, name, locals, args.size(), immediate, std::move(callback));
}

bool ScriptInstance::start_with_locals(
    int32_t index,
    std::string_view name,
    const std::array<int32_t, 4>& locals,
    std::size_t count,
    bool immediate,
    sim::script_vm::ReturnCallback callback
) {
    if (count > locals.size()) {
        note("a named script call accepts at most four arguments", name);
        if (callback)
            callback(0);
        return false;
    }
    if (index < 0) {
        if (callback)
            callback(0);
        return false;
    }
    // Keep a copy because a supplied callback is invoked with zero
    // when allocation fails. Vm::start owns the successful callback afterwards.
    const auto failed_callback = callback;
    const auto result =
        vm_.start_parameterized(static_cast<uint32_t>(index), locals, count, std::move(callback));
    if (!result.ok()) {
        if (result.error->code != sim::script_vm::ErrorCode::no_free_context)
            note("script start", result.error->message);
        if (failed_callback)
            failed_callback(0);
        return false;
    }
    if (immediate)
        tick(0);
    return true;
}

bool ScriptInstance::run_query(int32_t index, std::array<int32_t, 4>& args) {
    if (index < 0)
        return false;
    const auto started = vm_.start(static_cast<uint32_t>(index), args);
    if (!started.ok()) {
        if (started.error->code != sim::script_vm::ErrorCode::no_free_context)
            note("script query start", started.error->message);
        return false;
    }
    const auto result = vm_.tick_context(started.context, 0);
    if (!result.ok()) {
        note("script query", result.error->message);
        return false;
    }
    const auto context = vm_.context(started.context);
    for (std::size_t i = 0; i < args.size(); ++i)
        args[i] = context.slots[i];
    return true;
}

bool ScriptInstance::call_no_arguments(
    std::string_view name, bool immediate, sim::script_vm::ReturnCallback callback
) {
    return start_no_arguments(find(name), immediate, std::move(callback));
}

bool ScriptInstance::call_no_arguments(
    ScriptFunction function, bool immediate, sim::script_vm::ReturnCallback callback
) {
    return start_no_arguments(find(function), immediate, std::move(callback));
}

bool ScriptInstance::call(
    std::string_view name,
    std::span<const int32_t> args,
    bool immediate,
    sim::script_vm::ReturnCallback callback
) {
    return start_with_arguments(find(name), name, args, immediate, std::move(callback));
}

bool ScriptInstance::call(
    ScriptFunction function,
    std::span<const int32_t> args,
    bool immediate,
    sim::script_vm::ReturnCallback callback
) {
    return start_with_arguments(
        find(function),
        sim::unit_script::script_function_name(function),
        args,
        immediate,
        std::move(callback)
    );
}

bool ScriptInstance::call_with_locals(
    std::string_view name,
    const std::array<int32_t, 4>& locals,
    std::size_t count,
    bool immediate,
    sim::script_vm::ReturnCallback callback
) {
    return start_with_locals(find(name), name, locals, count, immediate, std::move(callback));
}

bool ScriptInstance::call_with_locals(
    ScriptFunction function,
    const std::array<int32_t, 4>& locals,
    std::size_t count,
    bool immediate,
    sim::script_vm::ReturnCallback callback
) {
    return start_with_locals(
        find(function),
        sim::unit_script::script_function_name(function),
        locals,
        count,
        immediate,
        std::move(callback)
    );
}

int32_t ScriptInstance::find(std::string_view name) const noexcept {
    return named_script(*program_, name);
}

int32_t ScriptInstance::find(ScriptFunction function) const noexcept {
    const auto index = static_cast<std::size_t>(function);
    return function_index_[index < function_index_.size() ? index : 0];
}

bool ScriptInstance::query(std::string_view name, std::array<int32_t, 4>& args) {
    return run_query(find(name), args);
}

bool ScriptInstance::query(ScriptFunction function, std::array<int32_t, 4>& args) {
    return run_query(find(function), args);
}

bool ScriptInstance::tick(uint32_t elapsed) {
    const auto result = vm_.tick(elapsed);
    if (!result.ok()) {
        note("script tick", result.error->message);
        return false;
    }
    return true;
}

int32_t ScriptInstance::piece_turn_speed(uint32_t piece, uint32_t axis) const noexcept {
    return vm_.turn_speed(piece, axis);
}

namespace {
// Adapts the match's UnitValueHost to the canonical GET/SET services.
struct ValueBridge {
    UnitValueHost& host;
    sim::unit_spawn::Slot& slot;
};

oa::FixedVec3 bridge_piece_world(void* context, oa::World*, oa::Unit*, uint32_t piece) {
    auto& bridge = *static_cast<ValueBridge*>(context);
    const auto at = bridge.host.piece_world_position(bridge.slot, piece);
    return {
        std::bit_cast<int32_t>(at[0]), std::bit_cast<int32_t>(at[1]), std::bit_cast<int32_t>(at[2])
    };
}

uint16_t bridge_direction(void* context, int32_t dx, int32_t dz) {
    return static_cast<ValueBridge*>(context)->host.direction_to(
        std::bit_cast<uint32_t>(dx), std::bit_cast<uint32_t>(dz)
    );
}

uint32_t bridge_distance(void* context, int32_t dx, int32_t dz) {
    return static_cast<ValueBridge*>(context)->host.distance(
        std::bit_cast<uint32_t>(dx), std::bit_cast<uint32_t>(dz)
    );
}

int32_t bridge_ground_height(void* context, oa::World*, int32_t x, int32_t z) {
    return static_cast<ValueBridge*>(context)->host.sample_terrain_height(
        std::bit_cast<uint32_t>(x), std::bit_cast<uint32_t>(z)
    );
}

void bridge_state_flag(void* context, oa::World*, oa::Unit*, uint8_t mask, bool enabled) {
    auto& bridge = *static_cast<ValueBridge*>(context);
    bridge.host.set_activation(bridge.slot, mask, enabled);
}

void bridge_yard(void* context, oa::World*, oa::Unit*, int32_t open) {
    auto& bridge = *static_cast<ValueBridge*>(context);
    bridge.host.set_yard_open(bridge.slot, open);
}

oa::sim::unit_script::UnitValueServices services(ValueBridge& bridge) {
    return {
        &bridge,
        bridge_piece_world,
        bridge_direction,
        bridge_distance,
        bridge_ground_height,
        bridge_state_flag,
        bridge_yard,
        bridge.host.value_rules,
        bridge.host.value_limits
    };
}
} // namespace

int32_t get_unit_value(
    oa::World& world,
    sim::unit_spawn::Slot& slot,
    std::span<sim::unit_spawn::Slot>,
    int32_t selector,
    int32_t second,
    int32_t third,
    UnitValueHost& host
) {
    if (!slot.unit)
        return 0;
    ValueBridge bridge{host, slot};
    return oa::sim::unit_script::unit_script_get_value(
        &world, &slot.record, selector, second, third, services(bridge)
    );
}

void set_unit_value(
    sim::unit_spawn::Slot& slot, int32_t selector, int32_t value, UnitValueHost& host
) {
    if (!slot.unit)
        return;
    ValueBridge bridge{host, slot};
    oa::sim::unit_script::unit_script_set_value(
        nullptr, &slot.record, selector, value, services(bridge)
    );
}
} // namespace oa::sim::match_runtime
