// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "unit_script_fixture.hpp"

#include <cstring>
#include <iostream>
#include <utility>

namespace us = oa::sim::unit_script;
namespace op = oa::sim::script_vm::opcode;
using us::test::require;

namespace {

struct Function {
    const char* name{};
    std::vector<uint32_t> code;
};

oa::formats::cob::CobProgram assemble(std::vector<Function> functions, uint32_t statics = 8) {
    oa::formats::cob::CobProgram cob;
    for (auto& function : functions) {
        cob.scripts.push_back({function.name, static_cast<uint32_t>(cob.code.size())});
        cob.entry_points.push_back(static_cast<uint32_t>(cob.code.size()));
        cob.code.insert(cob.code.end(), function.code.begin(), function.code.end());
    }
    cob.header.static_variable_count = statics;
    cob.header.script_count = static_cast<uint32_t>(cob.scripts.size());
    cob.piece_names = {"base", "turret"};
    return cob;
}

// Copies local `slot` into static `index`.
std::vector<uint32_t> local_to_static(uint32_t slot, uint32_t index) {
    return {op::push_local, slot, op::pop_static, index};
}

std::vector<uint32_t> join(std::vector<std::vector<uint32_t>> parts) {
    std::vector<uint32_t> code;
    for (auto& part : parts)
        code.insert(code.end(), part.begin(), part.end());
    return code;
}

const std::vector<uint32_t> finish{op::push_constant, 0, op::return_};

struct Bound {
    explicit Bound(oa::formats::cob::CobProgram program)
        : cob(std::move(program)), host(cob.piece_names.size()), shared(us::test::vm_program(cob)),
          vm(shared, host) {
        require(us::unit_script_attach(fixture.world, fixture.unit(1), &vm, cob), "attach");
    }

    ~Bound() { us::unit_script_detach(fixture.world, fixture.unit(1)); }

    int32_t stat(std::size_t index) const { return *vm.static_value(index); }

    us::test::WorldFixture fixture;
    oa::formats::cob::CobProgram cob;
    us::test::RecordingHost host;
    std::shared_ptr<const oa::sim::script_vm::Program> shared;
    oa::sim::script_vm::Vm vm;
};

void engine_names_are_exact() {
    require(
        std::strcmp(us::script_function_name(us::ScriptFunction::set_sfx_occupy), "setSFXoccupy") ==
            0,
        "setSFXoccupy keeps its lower-case s"
    );
    require(
        std::strcmp(
            us::script_function_name(us::ScriptFunction::query_landing_pad), "QueryLandingPad"
        ) == 0,
        "QueryLandingPad name"
    );
    require(us::weapon_aim_function(1) == us::ScriptFunction::aim_secondary, "aim slot table");
    require(us::weapon_fire_function(2) == us::ScriptFunction::fire_tertiary, "fire slot table");
    require(us::weapon_query_function(0) == us::ScriptFunction::query_primary, "query slot table");
    require(
        us::weapon_aim_from_function(2) == us::ScriptFunction::aim_from_tertiary, "aim-from table"
    );
    require(us::move_rate_function(0) == us::ScriptFunction::stop_moving, "rate 0 stops");
    require(us::move_rate_function(3) == us::ScriptFunction::move_rate3, "rate 3");
    for (std::size_t i = 0; i < us::script_function_count; ++i) {
        const auto function = static_cast<us::ScriptFunction>(i);
        require(us::engine_call(function).function == function, "engine table order");
        require(us::engine_call(function).argument_count <= us::max_arguments, "argument count");
    }
}

void attach_resolves_names_and_detach_releases() {
    Bound bound(assemble({{"Create", finish}, {"AimPrimary", finish}}));
    auto* script = us::unit_script_of(bound.fixture.world, bound.fixture.unit(1));
    require(script != nullptr && script->vm == &bound.vm, "attached script is found");
    require(
        script->function_index[static_cast<std::size_t>(us::ScriptFunction::create)] == 0,
        "Create index"
    );
    require(
        script->function_index[static_cast<std::size_t>(us::ScriptFunction::aim_primary)] == 1,
        "Aim index"
    );
    require(
        script->function_index[static_cast<std::size_t>(us::ScriptFunction::killed)] == -1,
        "absent Killed"
    );
    require(
        us::unit_script_of(bound.fixture.world, bound.fixture.unit(2)) == nullptr,
        "other slot unbound"
    );
    require(
        us::unit_script_call(
            bound.fixture.world, bound.fixture.unit(2), us::ScriptFunction::create, {}
        ) == us::ScriptCallStatus::no_script,
        "unbound unit reports no script"
    );
    us::test::RecordingHost other_host(2);
    oa::sim::script_vm::Vm replaced(bound.shared, other_host);
    require(
        replaced.program().code.data() == bound.vm.program().code.data(),
        "two interpreters of one script do not share its code"
    );
    us::unit_script_detach(bound.fixture.world, bound.fixture.unit(1), &replaced);
    require(
        us::unit_script_of(bound.fixture.world, bound.fixture.unit(1)) != nullptr,
        "detaching another interpreter keeps the attached one"
    );
    us::unit_script_detach(bound.fixture.world, bound.fixture.unit(1), &bound.vm);
    require(
        us::unit_script_of(bound.fixture.world, bound.fixture.unit(1)) == nullptr, "detach clears"
    );
    require(
        us::unit_script_attach(bound.fixture.world, bound.fixture.unit(1), &bound.vm, bound.cob),
        "reattach"
    );
    require(
        !us::unit_script_attach(bound.fixture.world, bound.fixture.unit(0), &bound.vm, bound.cob),
        "reserved slot 0 is refused"
    );
}

void argument_call_writes_four_locals_and_sets_stack_pointer() {
    // Pops the top of stack (local 0 when one argument is passed) and copies local 1.
    Bound bound(assemble({
        {"HitByWeapon", join({local_to_static(0, 0), local_to_static(1, 1), finish})},
        {"TransportDrop", join({{op::pop_static, 2}, local_to_static(1, 3), finish})},
    }));
    auto* world = bound.fixture.world;
    auto* unit = bound.fixture.unit(1);
    require(
        us::unit_script_call(
            world, unit, us::ScriptFunction::hit_by_weapon, us::script_args(-40, 25)
        ) == us::ScriptCallStatus::started,
        "HitByWeapon starts"
    );
    require(bound.vm.context(0).stack_pointer == 1, "two arguments leave sp at 1");
    require(bound.stat(0) == 0, "deferred call does not run before the tick");
    require(bound.vm.tick(0).ok(), "tick");
    require(bound.stat(0) == -40 && bound.stat(1) == 25, "locals carry the arguments");

    us::ScriptArgs drop{{7, 0x00120034, 0, 0}, 1};
    require(
        us::unit_script_call(
            world, unit, us::ScriptFunction::transport_drop, drop, us::ScriptRun::immediate
        ) == us::ScriptCallStatus::started,
        "TransportDrop starts immediately"
    );
    require(bound.stat(2) == 7, "stack holds only the first argument");
    require(bound.stat(3) == 0x00120034, "local 1 still receives the second value");
}

void bare_call_leaves_stack_empty() {
    Bound bound(
        assemble({{"Activate", join({{op::push_constant, 9, op::pop_static, 0}, finish})}})
    );
    auto* world = bound.fixture.world;
    auto* unit = bound.fixture.unit(1);
    require(
        us::unit_script_call_bare(world, unit, us::ScriptFunction::activate) ==
            us::ScriptCallStatus::started,
        "bare call starts"
    );
    require(bound.vm.context(0).stack_pointer == -1, "bare call pushes nothing");
    require(bound.stat(0) == 0, "deferred bare call waits for the tick");
    require(us::unit_script_tick(world, unit, 0) == us::ScriptCallStatus::started, "tick");
    require(bound.stat(0) == 9, "bare call ran on the tick");
}

void query_reads_back_stack_slots() {
    Bound bound(assemble({
        {"QueryPrimary", join({{op::push_constant, 1, op::pop_local, 0}, finish})},
        {"Killed",
         join({{op::push_local, 0, op::push_constant, 1, op::add, op::pop_local, 1}, finish})},
    }));
    auto* world = bound.fixture.world;
    auto* unit = bound.fixture.unit(1);
    int32_t piece = 0;
    require(
        us::unit_script_query(world, unit, us::ScriptFunction::query_primary, &piece) ==
            us::ScriptCallStatus::started,
        "QueryPrimary runs"
    );
    require(piece == 1, "query result comes from local 0");
    int32_t severity = 55;
    int32_t corpse = -1;
    require(
        us::unit_script_query(world, unit, us::ScriptFunction::killed, &severity, &corpse) ==
            us::ScriptCallStatus::started,
        "Killed runs"
    );
    require(severity == 55 && corpse == 56, "Killed receives severity and returns corpse type");
    require(bound.vm.active_count() == 0, "queries finish synchronously");
    int32_t untouched = 12;
    require(
        us::unit_script_query(world, unit, us::ScriptFunction::sweet_spot, &untouched) ==
            us::ScriptCallStatus::no_function,
        "missing query"
    );
    require(untouched == 12, "missing query leaves the value");
}

void failures_follow_the_game_callback_rule() {
    const std::vector<uint32_t> sleeper{
        op::push_constant, 1000, op::sleep, op::push_constant, 0, op::return_
    };
    Bound bound(
        assemble({{"AimPrimary", join({{op::push_local, 0}, {op::return_}})}, {"Create", sleeper}})
    );
    auto* world = bound.fixture.world;
    auto* unit = bound.fixture.unit(1);
    std::vector<int32_t> returned;
    auto record = [&](int32_t value) { returned.push_back(value); };

    require(
        us::unit_script_call(
            world,
            unit,
            us::ScriptFunction::aim_primary,
            us::script_args(0x2000, 0x100),
            us::ScriptRun::immediate,
            record
        ) == us::ScriptCallStatus::started,
        "aim call"
    );
    require(returned.size() == 1 && returned[0] == 0x2000, "RETURN value reaches the aim callback");

    require(
        us::unit_script_call(
            world,
            unit,
            us::ScriptFunction::aim_secondary,
            us::script_args(1, 2),
            us::ScriptRun::deferred,
            record
        ) == us::ScriptCallStatus::no_function,
        "missing aim function"
    );
    require(returned.size() == 2 && returned[1] == 0, "a failed argument call reports 0");
    require(
        us::unit_script_call_bare(
            world, unit, us::ScriptFunction::aim_secondary, us::ScriptRun::deferred, record
        ) == us::ScriptCallStatus::no_function,
        "missing bare function"
    );
    require(returned.size() == 2, "a failed bare call does not report");

    for (int i = 0; i < 8; ++i)
        require(
            us::unit_script_call_bare(
                world, unit, us::ScriptFunction::create, us::ScriptRun::immediate
            ) == us::ScriptCallStatus::started,
            "sleeping context"
        );
    require(bound.vm.active_count() == 8, "all contexts busy");
    require(
        us::unit_script_call(
            world,
            unit,
            us::ScriptFunction::aim_primary,
            us::script_args(3, 4),
            us::ScriptRun::deferred,
            record
        ) == us::ScriptCallStatus::no_free_context,
        "no free context"
    );
    require(returned.size() == 3 && returned[2] == 0, "a full machine reports 0");
    require(
        us::unit_script_call_index(world, unit, 0, us::script_args(), us::ScriptRun::deferred) ==
            us::ScriptCallStatus::no_free_context,
        "index path shares the context limit"
    );
}

uint32_t hypot_services = 0;

oa::FixedVec3 piece_at(void*, oa::World*, oa::Unit*, uint32_t piece) {
    return {static_cast<int32_t>(0x00050000 + piece), 0x00070000, static_cast<int32_t>(0xfffe8000)};
}

uint16_t direction_of(void*, int32_t dx, int32_t dz) {
    return static_cast<uint16_t>((dx >> 16) * 16 + (dz >> 16));
}

uint32_t distance_of(void*, int32_t dx, int32_t dz) {
    ++hypot_services;
    return static_cast<uint32_t>(dx + dz);
}

int32_t height_at(void*, oa::World*, int32_t x, int32_t z) {
    return (x >> 16) + (z >> 16);
}

void set_flag(void* context, oa::World*, oa::Unit* unit, uint8_t mask, bool enabled) {
    ++*static_cast<int*>(context);
    unit->state_flags =
        static_cast<uint8_t>(enabled ? unit->state_flags | mask : unit->state_flags & ~mask);
}

void set_yard(void* context, oa::World*, oa::Unit* unit, int32_t open) {
    ++*static_cast<int*>(context);
    unit->build_flags =
        static_cast<uint8_t>((unit->build_flags & ~OA_UNIT_BUILD_YARD_OPEN) | ((open & 1) << 2));
}

void unit_values_map_to_canonical_fields() {
    us::test::WorldFixture fixture;
    auto* world = fixture.world;
    auto* unit = fixture.unit(1);
    int service_calls = 0;
    const us::UnitValueServices services{
        &service_calls, piece_at, direction_of, distance_of, height_at, set_flag, set_yard
    };
    auto get = [&](us::UnitValue selector, int32_t first = 0, int32_t second = 0) {
        return us::unit_script_get_value(
            world, unit, static_cast<int32_t>(selector), first, second, services
        );
    };
    auto set = [&](us::UnitValue selector, int32_t value) {
        us::unit_script_set_value(world, unit, static_cast<int32_t>(selector), value, services);
    };

    unit->health = 100;
    require(get(us::UnitValue::health) == 25, "health percent of max_damage 400");
    unit->health = -4;
    require(
        get(us::UnitValue::health) == static_cast<int32_t>(0xfffffe70U / 400U),
        "negative health divides unsigned as the game does"
    );
    unit->flags |= (2U << OA_UNIT_FLAG_MOVE_ORDER_SHIFT) | (1U << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
    require(get(us::UnitValue::standing_move_orders) == 2, "move orders");
    require(get(us::UnitValue::standing_fire_orders) == 1, "fire orders");

    set(us::UnitValue::activation, 5);
    require(
        get(us::UnitValue::activation) == 1 && service_calls == 1,
        "activation goes through the service"
    );
    require((unit->events & us::event_script_state_changed) != 0, "SET raises the event bit");
    set(us::UnitValue::armored, 1);
    require(get(us::UnitValue::armored) == 1, "armored reads state flag bit 1");
    set(us::UnitValue::in_build_stance, 3);
    set(us::UnitValue::busy, 1);
    set(us::UnitValue::bugger_off, 1);
    require(unit->build_flags == 0x0b, "stance, busy and bugger-off write build flag bits 0, 1, 3");
    set(us::UnitValue::busy, 0);
    require(
        get(us::UnitValue::busy) == 0 && get(us::UnitValue::in_build_stance) == 1,
        "busy clears alone"
    );
    set(us::UnitValue::yard_open, 1);
    require(
        get(us::UnitValue::yard_open) == 1 && service_calls == 3, "yard goes through the service"
    );
    unit->events = 0;
    set(static_cast<us::UnitValue>(77), 1);
    require(unit->events == us::event_script_state_changed, "unknown SET still raises the event");

    require(
        get(us::UnitValue::piece_xz, 1) == 0x0004fffe,
        "piece xz adds the signed z high word to the x high word"
    );
    require(get(us::UnitValue::piece_y, 1) == 0x00070000, "piece y");

    auto* other = fixture.unit(3);
    other->position = {0x00100000, 0x00020000, 0x00300000};
    other->def = 1;
    fixture.world->unit_defs[0].model_height = 0x000a0000;
    require(get(us::UnitValue::unit_xz, 3) == 0, "a dead unit reads zero");
    other->flags = OA_UNIT_FLAG_LIVE;
    require(get(us::UnitValue::unit_xz, 0x70003) == 0x00100030, "unit xz uses the low id word");
    require(get(us::UnitValue::unit_y, 3) == 0x00020000, "unit y");
    require(get(us::UnitValue::unit_height, 3) == 0x000a0000, "unit height");
    require(get(us::UnitValue::unit_y, 0) == 0, "id 0 is no unit");

    unit->heading = 0x0010;
    require(get(us::UnitValue::xz_atan, 0x00020003) == 0x13, "xz atan is relative to heading");
    require(get(us::UnitValue::xz_atan, 0x0002ffff) == 0x1f, "negative z rounds x up");
    require(get(us::UnitValue::atan, 0x00010000, 0x00020000) == 0x12, "atan of explicit vector");
    require(
        get(us::UnitValue::hypot, 3, 4) == 7 &&
            get(us::UnitValue::xz_hypot, 0x00010002) == 0x00030000 && hypot_services == 2,
        "hypot services"
    );
    require(
        get(us::UnitValue::ground_height, 0x00040005) == (9 << 16),
        "ground height is shifted to 16.16"
    );

    unit->build_remaining = 0.0F;
    require(get(us::UnitValue::build_percent_left) == 0, "finished unit");
    unit->build_remaining = 1.0F;
    require(get(us::UnitValue::build_percent_left) == 100, "unstarted unit");
    unit->build_remaining = 0.5F;
    require(get(us::UnitValue::build_percent_left) == 50, "1 - trunc(0.5 * -99)");
    require(get(static_cast<us::UnitValue>(99)) == 0, "unknown GET");
}

void carried_units_follow_attachment_links() {
    us::test::WorldFixture fixture;
    auto* world = fixture.world;
    auto* carrier = fixture.unit(1);
    carrier->attach_first_child = oa::oa_unit_ref_from_slot(4);
    fixture.unit(4)->attach_next = oa::oa_unit_ref_from_slot(6);
    fixture.unit(4)->attach_parent = oa::oa_unit_ref_from_slot(1);
    fixture.unit(6)->attach_parent = oa::oa_unit_ref_from_slot(1);
    require(us::unit_script_is_carrying(world, carrier, 6) == 1, "second child found");
    require(us::unit_script_is_carrying(world, carrier, 5) == 0, "non-child");
    require(us::unit_script_carrier_id(world, fixture.unit(6)) == 1, "carrier id");
    require(us::unit_script_carrier_id(world, carrier) == 0, "uncarried");
    fixture.unit(6)->attach_next = oa::oa_unit_ref_from_slot(4);
    require(us::unit_script_is_carrying(world, carrier, 7) == 0, "a cyclic chain terminates");
}

} // namespace

int main() {
    try {
        engine_names_are_exact();
        attach_resolves_names_and_detach_releases();
        argument_call_writes_four_locals_and_sets_stack_pointer();
        bare_call_leaves_stack_empty();
        query_reads_back_stack_slots();
        failures_follow_the_game_callback_rule();
        unit_values_map_to_canonical_fields();
        carried_units_follow_attachment_links();
    } catch (const std::exception& error) {
        std::cerr << "unit-script test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "unit-script tests passed\n";
    return 0;
}
