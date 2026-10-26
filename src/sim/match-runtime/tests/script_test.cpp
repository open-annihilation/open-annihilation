// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime/script.hpp"
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
using namespace oa::sim::match_runtime;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)

struct UnusedVmHost : oa::sim::script_vm::Host {
#define RET(type, name, args)                                                                      \
    type name args override {                                                                      \
        throw std::runtime_error("unexpected fixture host call");                                  \
    }
    RET(int32_t, piece_position, (uint32_t, uint32_t) const)
    RET(int32_t, piece_angle, (uint32_t, uint32_t) const)
    RET(uint32_t, piece_visible, (uint32_t) const)
    RET(uint32_t, piece_cached, (uint32_t) const)
    RET(uint32_t, piece_shaded, (uint32_t) const)
    RET(void, set_piece_position, (uint32_t, uint32_t, int32_t))
    RET(void, set_piece_angle, (uint32_t, uint32_t, uint32_t))
    RET(void, set_piece_visible, (uint32_t, uint32_t))
    RET(void, set_piece_cached, (uint32_t, uint32_t))
    RET(void, set_piece_shaded, (uint32_t, uint32_t))
    RET(void, emit_sfx, (uint32_t, int32_t))
    RET(void, explode, (uint32_t, int32_t))
    RET(int32_t, get_unit_value, (int32_t, int32_t, int32_t, int32_t, int32_t))
    RET(void, set_unit_value, (int32_t, int32_t))
    RET(void, attach_unit, (int32_t, int32_t, int32_t))
    RET(void, drop_unit, (int32_t))
    RET(void, ignored_piece_op, (uint32_t, uint32_t, uint32_t))
    RET(void, dont_shadow, (uint32_t))
    RET(uint32_t, is_carrying_unit, (uint32_t))
    RET(uint32_t, carrier_unit_id, ())
    RET(uint32_t, random_bounded, (uint32_t))
#undef RET
};

struct Values : UnitValueHost {
    std::array<uint32_t, 3> piece_world_position(oa::sim::unit_spawn::Slot&, uint32_t) override {
        return {0x120000, 0x40000, 0xfffe0000};
    }

    uint16_t direction_to(uint32_t, uint32_t) override { return 10; }

    uint32_t distance(uint32_t, uint32_t) override { return 20; }

    int32_t sample_terrain_height(uint32_t x, uint32_t z) override {
        CHECK(x == 0x130000 && z == 0xfffe0000);
        return 42;
    }

    void set_activation(oa::sim::unit_spawn::Slot& slot, uint8_t mask, bool active) override {
        CHECK(mask == 1 && active);
        slot.record.state_flags |= mask;
    }

    void set_yard_open(oa::sim::unit_spawn::Slot&, int32_t) override {
        throw std::runtime_error("unexpected fixture yard call");
    }
};

int main() {
    SharedRandom random(0);
    const auto initial = random.state();
    CHECK(initial == 0x66e29573u);
    CHECK(SharedRandom(0x66e29572u).state() == 1u && SharedRandom(1u).state() == 0x66e29573u);
    CHECK(!random.bounded(0) && random.state() == initial);
    CHECK(!random.bounded(0xffffffff) && random.state() == initial);
    random.restore(1);
    CHECK(random.bounded(100000) == 16807 && random.state() == 16807);
    auto cob = std::make_shared<oa::formats::cob::CobProgram>();
    cob->code = {
        oa::sim::script_vm::opcode::push_constant, 42, oa::sim::script_vm::opcode::return_
    };
    cob->entry_points = {0};
    cob->scripts = {{"Create", 0}};
    cob->header.script_count = 1;
    UnusedVmHost vmhost;
    ScriptInstance script(cob, vmhost, 30);
    int32_t returned = -1;
    CHECK(script.call("Create", {}, true, [&](int32_t n) { returned = n; }));
    CHECK(returned == 42 && script.vm().active_count() == 0);
    returned = -1;
    CHECK(!script.call("create", {}, true, [&](int32_t n) { returned = n; }));
    CHECK(returned == 0);
    // Query writes locals back, rather than treating RETURN as the result. It
    // executes only its new context; an already-active deferred call stays idle.
    auto queries = std::make_shared<oa::formats::cob::CobProgram>();
    using namespace oa::sim::script_vm;
    queries->code = {
        opcode::push_constant,
        99,
        opcode::pop_static,
        0,
        opcode::return_,
        opcode::push_constant,
        17,
        opcode::pop_local,
        0,
        opcode::push_constant,
        800,
        opcode::return_
    };
    queries->entry_points = {0, 5};
    queries->scripts = {{"Deferred", 0}, {"QueryPrimary", 5}};
    queries->header.static_variable_count = 1;
    ScriptInstance query_script(queries, vmhost, 30);
    CHECK(query_script.call("Deferred", {}, false));
    std::array<int32_t, 4> query_args{0, 2, 3, 4};
    CHECK(query_script.query("QueryPrimary", query_args));
    CHECK(query_args[0] == 17 && query_args[1] == 2 && query_args[2] == 3 && query_args[3] == 4);
    CHECK(query_script.vm().static_value(0) == 0 && query_script.vm().active_count() == 1);
    CHECK(!query_script.query("queryprimary", query_args) && query_args[0] == 17);
    query_script.tick(0);
    CHECK(query_script.vm().static_value(0) == 99);
    // A second unit of this type shares the code and keeps its own statics,
    // stack and running script.
    ScriptInstance other_query(queries, vmhost, 30);
    CHECK(query_script.vm().program().code.data() == other_query.vm().program().code.data());
    CHECK(!query_script.vm().program().code.empty());
    CHECK(other_query.vm().static_value(0) == 0 && other_query.vm().active_count() == 0);
    CHECK(
        other_query.find(ScriptFunction::query_primary) ==
        query_script.find(ScriptFunction::query_primary)
    );
    const std::array<int32_t, 1> only{4};
    CHECK(other_query.call("Deferred", only, false));
    CHECK(other_query.vm().active_count() == 1 && other_query.vm().context(0).slots[0] == 4);
    CHECK(query_script.vm().static_value(0) == 99 && query_script.vm().active_count() == 0);
    auto parameter_cob = std::make_shared<oa::formats::cob::CobProgram>();
    parameter_cob->code = {opcode::push_local, 3, opcode::pop_static, 0, opcode::return_};
    parameter_cob->entry_points = {0};
    parameter_cob->scripts = {{"ReadFourth", 0}};
    parameter_cob->header.static_variable_count = 1;
    ScriptInstance parameter_script(parameter_cob, vmhost, 30);
    const std::array<int32_t, 4> four{1, 2, 3, 77};
    CHECK(parameter_script.call("ReadFourth", four, true));
    CHECK(parameter_script.vm().static_value(0) == 77);
    CHECK(parameter_script.call_no_arguments("ReadFourth", true));
    CHECK(parameter_script.vm().static_value(0) == 77);
    const std::array<int32_t, 1> one{5};
    CHECK(parameter_script.call("ReadFourth", one, true));
    CHECK(parameter_script.vm().static_value(0) == 0);
    bool failed_callback = false;
    CHECK(!parameter_script.call_no_arguments("Missing", true, [&](int32_t) {
        failed_callback = true;
    }));
    CHECK(!failed_callback);
    CHECK(!parameter_script.call_no_arguments(ScriptFunction::create, true, [&](int32_t) {
        failed_callback = true;
    }));
    CHECK(!failed_callback);
    // A fixed engine function starts the first script with its exact name, as
    // the same call by name does; a name differing only in case is not it.
    auto fixed_cob = std::make_shared<oa::formats::cob::CobProgram>();
    fixed_cob->code = {
        opcode::push_constant,
        1,
        opcode::return_,
        opcode::push_constant,
        2,
        opcode::return_,
        opcode::push_constant,
        3,
        opcode::return_,
        opcode::push_local,
        0,
        opcode::pop_static,
        0,
        opcode::push_constant,
        31,
        opcode::pop_local,
        0,
        opcode::return_
    };
    fixed_cob->entry_points = {0, 3, 6, 9};
    fixed_cob->scripts = {{"rockunit", 0}, {"RockUnit", 3}, {"RockUnit", 6}, {"SweetSpot", 9}};
    fixed_cob->header.static_variable_count = 1;
    ScriptInstance fixed(fixed_cob, vmhost, 30);
    CHECK(fixed.find(ScriptFunction::rock_unit) == 1 && fixed.find("RockUnit") == 1);
    CHECK(fixed.find(ScriptFunction::sweet_spot) == 3);
    CHECK(fixed.find(ScriptFunction::create) == -1 && fixed.find(ScriptFunction::killed) == -1);
    CHECK(fixed.find(static_cast<ScriptFunction>(0xff)) == fixed.find(ScriptFunction::create));
    const std::array<int32_t, 2> rock{-4, 6};
    returned = -1;
    CHECK(fixed.call(ScriptFunction::rock_unit, rock, true, [&](int32_t n) { returned = n; }));
    CHECK(returned == 2);
    returned = -1;
    CHECK(!fixed.call(ScriptFunction::hit_by_weapon, rock, true, [&](int32_t n) { returned = n; }));
    CHECK(returned == 0);
    returned = -1;
    const std::array<int32_t, 5> too_many{};
    CHECK(!fixed.call(ScriptFunction::rock_unit, too_many, true, [&](int32_t n) { returned = n; }));
    CHECK(returned == 0);
    returned = -1;
    CHECK(fixed.call_with_locals(ScriptFunction::rock_unit, four, 1, true, [&](int32_t n) {
        returned = n;
    }));
    CHECK(returned == 2);
    std::array<int32_t, 4> spot{9, 0, 0, 0};
    CHECK(fixed.query(ScriptFunction::sweet_spot, spot));
    CHECK(spot[0] == 31 && fixed.vm().static_value(0) == 9);
    std::array<int32_t, 4> missing{5, 6, 7, 8};
    CHECK(!fixed.query(ScriptFunction::query_primary, missing));
    CHECK(missing == (std::array<int32_t, 4>{5, 6, 7, 8}));
    // An instance without a program finds no fixed function.
    ScriptInstance empty(nullptr, vmhost, 30);
    CHECK(empty.find(ScriptFunction::create) == -1);
    oa::sim::unit_spawn::LegacyWorld world(2);
    world.types()[1].simulation.maximum_health = 100;
    world.load_types();
    auto& unit = world.unit(0);
    unit.type = &world.types()[1].simulation;
    unit.health = 25;
    auto slots = world.views().slots();
    auto& state = world.state();
    Values values;
    CHECK(get_unit_value(state, slots[0], slots, 4, 0, 0, values) == 25);
    slots[0].record.build_remaining = 1.0f;
    CHECK(get_unit_value(state, slots[0], slots, 17, 0, 0, values) == 100);
    slots[0].record.build_remaining = 0.5f;
    CHECK(get_unit_value(state, slots[0], slots, 17, 0, 0, values) == 50);
    CHECK(get_unit_value(state, slots[0], slots, 7, 0, 0, values) == 0x11fffe);
    CHECK(get_unit_value(state, slots[0], slots, 16, 0x12fffe, 0, values) == 42 * 65536);
    // An index outside 1..20 reads the extension the profile mounts there.
    CHECK(get_unit_value(state, slots[0], slots, 71, 0, 0, values) == 0);
    oa::data::match_rules::ScriptExtensionTable extensions{};
    extensions.mounts[extensions.count++] = {
        71, oa::data::match_rules::ScriptExtension::unit_my_id
    };
    extensions.mounts[extensions.count++] = {
        69, oa::data::match_rules::ScriptExtension::unit_min_id
    };
    oa::data::match_rules::MatchRules rules{};
    rules.script_get = extensions;
    values.value_rules.match = &rules;
    CHECK(get_unit_value(state, slots[0], slots, 71, 0, 0, values) == slots[0].record.id);
    CHECK(get_unit_value(state, slots[0], slots, 69, 0, 0, values) == 1);
    CHECK(get_unit_value(state, slots[0], slots, 70, 0, 0, values) == 0);
    values.value_rules = {};
    set_unit_value(slots[0], 6, 3, values);
    CHECK(slots[0].record.build_flags == 2 && unit.events == 4);
    set_unit_value(slots[0], 1, 3, values);
    CHECK(slots[0].record.state_flags == 1);
    set_unit_value(slots[0], 99, 3, values);
    CHECK(unit.events == 4);
    std::cout << "match runtime tests passed\n";
}
