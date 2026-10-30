// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unit-script extensions: a GET index a mod profile mounts reads the
// extension mounted there, at whatever index; an index nothing is mounted at
// reads 0; exact fidelity reads free slots as they stand and safe fidelity
// reads them as 0; every SET still wakes the unit; and a script that walks
// every unit id, as mod scripts do, sees the values through the interpreter.

#include "oa/test/check.hpp"
#include "unit_script_fixture.hpp"

#include <bit>
#include <cstdint>
#include <vector>

namespace us = oa::sim::unit_script;
namespace mr = oa::data::match_rules;
namespace op = oa::sim::script_vm::opcode;

namespace {

/// The community numbering of the eight extensions.
mr::ScriptExtensionTable community_table() {
    mr::ScriptExtensionTable table{};
    const mr::ScriptMount mounts[] = {
        {32, mr::ScriptExtension::unit_kills_x100},
        {69, mr::ScriptExtension::unit_min_id},
        {70, mr::ScriptExtension::unit_max_id},
        {71, mr::ScriptExtension::unit_my_id},
        {72, mr::ScriptExtension::unit_owner_of},
        {73, mr::ScriptExtension::unit_build_percent_left_of},
        {74, mr::ScriptExtension::unit_allied_with},
        {75, mr::ScriptExtension::unit_is_local},
    };
    for (const mr::ScriptMount& mount : mounts)
        table.mounts[table.count++] = mount;
    return table;
}

/// A world of two players' unit ranges: limit 2, so slots 1..2 are player
/// 0's, 3..4 player 1's and so on up to slot 20, plus reserved slot 0.
struct Ranges {
    us::test::WorldFixture fixture{21};
    oa::World* world = fixture.world;

    Ranges() {
        world->game.units_per_player = 2;
        world->game.max_units_setting = 2;
        world->units[0].owner_index = 0xff;
        for (uint32_t slot = 1; slot < 21; ++slot) {
            world->units[slot].owner_index = static_cast<uint8_t>((slot - 1) / 2);
            world->units[slot].flags = 0;
        }
        for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
            world->game.players[player].index = player;
            world->game.players[player].alliance[player] = 1; // every player allies itself
        }
        live(1);
        live(3);
        world->game.players[0].status = OA_PLAYER_STATUS_LOCAL;
        world->game.players[1].status = OA_PLAYER_STATUS_COMPUTER;
        world->game.players[2].status = OA_PLAYER_STATUS_MIRRORED;
    }

    void live(uint32_t slot) { world->units[slot].flags = OA_UNIT_FLAG_LIVE; }
};

int32_t
get(Ranges& ranges,
    uint32_t caller,
    int32_t selector,
    int32_t argument,
    const mr::ScriptExtensionTable* table,
    mr::ScriptFidelity fidelity = mr::ScriptFidelity::exact) {
    mr::MatchRules rules{};
    if (table != nullptr)
        rules.script_get = *table;
    rules.script_fidelity = fidelity;
    us::UnitValueServices services{};
    services.rules.match = &rules;
    return us::unit_script_get_value(
        ranges.world, &ranges.world->units[caller], selector, argument, 0, services
    );
}

void unmounted_indices_read_zero() {
    Ranges ranges;
    const mr::ScriptExtensionTable table = community_table();
    for (const int32_t selector : {21, 31, 33, 68, 76, 1000, 65535, 65536, -1, 0})
        OA_CHECK(get(ranges, 1, selector, 1, &table) == 0);
    // Without a profile nothing is mounted: the community indices read 0.
    for (int32_t selector = 21; selector < 100; ++selector)
        OA_CHECK(get(ranges, 1, selector, 1, nullptr) == 0);
    const mr::ScriptExtensionTable empty{};
    OA_CHECK(get(ranges, 1, 71, 1, &empty) == 0);
}

void extensions_mount_at_any_index() {
    Ranges ranges;
    mr::ScriptExtensionTable table{};
    table.mounts[table.count++] = {33, mr::ScriptExtension::unit_my_id};
    table.mounts[table.count++] = {45, mr::ScriptExtension::unit_my_id};
    table.mounts[table.count++] = {60000, mr::ScriptExtension::unit_min_id};
    OA_CHECK(get(ranges, 3, 33, 0, &table) == 3);
    OA_CHECK(get(ranges, 3, 45, 0, &table) == 3);
    OA_CHECK(get(ranges, 3, 60000, 0, &table) == 1);
    OA_CHECK(get(ranges, 3, 71, 0, &table) == 0);
    // The base selectors keep their meaning, whatever the table holds.
    ranges.world->units[3].build_remaining = 0.5F;
    OA_CHECK(
        get(ranges, 3, static_cast<int32_t>(us::UnitValue::build_percent_left), 0, &table) == 50
    );
}

void caller_values() {
    Ranges ranges;
    const mr::ScriptExtensionTable table = community_table();
    ranges.world->units[3].veteran_level = 7;
    OA_CHECK(get(ranges, 3, 32, 0, &table) == 700);
    ranges.world->units[3].veteran_level = 65535;
    OA_CHECK(get(ranges, 3, 32, 0, &table) == 6553500);
    OA_CHECK(get(ranges, 3, 69, 0, &table) == 1);
    OA_CHECK(get(ranges, 3, 71, 0, &table) == 3);
    OA_CHECK(get(ranges, 1, 71, 0, &table) == 1);
    // The highest id follows the configured limit, not the game's.
    OA_CHECK(get(ranges, 3, 70, 0, &table) == 20);
    ranges.world->game.max_units_setting = 1500;
    OA_CHECK(get(ranges, 3, 70, 0, &table) == 15000);
    ranges.world->game.max_units_setting = 0;
    OA_CHECK(get(ranges, 3, 70, 0, &table) == 20);
}

void owner_of_reads_the_slot() {
    Ranges ranges;
    const mr::ScriptExtensionTable table = community_table();
    OA_CHECK(get(ranges, 1, 72, 3, &table) == 1);
    OA_CHECK(get(ranges, 1, 72, 0x10003, &table) == 1); // the low 16 bits name the slot
    OA_CHECK(get(ranges, 1, 72, 20, &table) == 9);
    // A free slot still holds its range's owner; slot 0 holds 0xff.
    OA_CHECK(get(ranges, 1, 72, 6, &table) == 2);
    OA_CHECK(get(ranges, 1, 72, 0, &table) == 0xff);
    // Past the table, and past the highest id, read 0.
    OA_CHECK(get(ranges, 1, 72, 21, &table) == 0);
    ranges.world->game.max_units_setting = 1;
    OA_CHECK(get(ranges, 1, 72, 11, &table) == 0);
    OA_CHECK(get(ranges, 1, 72, 10, &table) == 4);
    // Safe fidelity reads only live units.
    OA_CHECK(get(ranges, 1, 72, 6, &table, mr::ScriptFidelity::safe) == 0);
    OA_CHECK(get(ranges, 1, 72, 3, &table, mr::ScriptFidelity::safe) == 1);
    OA_CHECK(get(ranges, 1, 72, 0, &table, mr::ScriptFidelity::safe) == 0);
}

void build_percent_left_of_reads_another_unit() {
    Ranges ranges;
    const mr::ScriptExtensionTable table = community_table();
    ranges.world->units[3].build_remaining = 0.25F;
    ranges.world->units[6].build_remaining = 1.0F; // a free slot's stale value
    ranges.world->units[19].build_remaining = 0.5F;
    ranges.world->units[20].build_remaining = 0.5F;
    OA_CHECK(get(ranges, 1, 73, 3, &table) == 25);
    OA_CHECK(get(ranges, 1, 73, 1, &table) == 0);
    OA_CHECK(get(ranges, 1, 73, 6, &table) == 100);
    OA_CHECK(get(ranges, 1, 73, 19, &table) == 50);
    // The table's last slot and anything past it read 0.
    OA_CHECK(get(ranges, 1, 73, 20, &table) == 0);
    OA_CHECK(get(ranges, 1, 73, 21, &table) == 0);
    OA_CHECK(get(ranges, 1, 73, -1, &table) == 0);
    // The whole argument counts, and the record offset wraps at 32 bits.
    OA_CHECK(get(ranges, 1, 73, 0x10003, &table) == 0);
    OA_CHECK(get(ranges, 1, 73, 3 + (1 << 29), &table) == 25);
    // Safe fidelity takes the argument as the slot and needs a live unit.
    OA_CHECK(get(ranges, 1, 73, 3, &table, mr::ScriptFidelity::safe) == 25);
    OA_CHECK(get(ranges, 1, 73, 6, &table, mr::ScriptFidelity::safe) == 0);
    OA_CHECK(get(ranges, 1, 73, 3 + (1 << 29), &table, mr::ScriptFidelity::safe) == 0);
}

void allied_with_is_one_way() {
    Ranges ranges;
    const mr::ScriptExtensionTable table = community_table();
    ranges.world->game.players[0].alliance[1] = 1;
    OA_CHECK(get(ranges, 1, 74, 3, &table) == 1); // player 0 allies player 1
    OA_CHECK(get(ranges, 3, 74, 1, &table) == 0); // player 1 does not ally player 0
    OA_CHECK(get(ranges, 1, 74, 1, &table) == 1); // own units
    OA_CHECK(get(ranges, 1, 74, 2, &table) == 1); // a free slot of the own range
    OA_CHECK(get(ranges, 1, 74, 5, &table) == 0);
    OA_CHECK(get(ranges, 1, 74, 0, &table) == 0);  // owner 0xff
    OA_CHECK(get(ranges, 1, 74, 21, &table) == 0); // past the table
    OA_CHECK(get(ranges, 1, 74, 2, &table, mr::ScriptFidelity::safe) == 0);
    OA_CHECK(get(ranges, 1, 74, 3, &table, mr::ScriptFidelity::safe) == 1);
}

void is_local_reads_the_owner_status() {
    Ranges ranges;
    const mr::ScriptExtensionTable table = community_table();
    OA_CHECK(get(ranges, 1, 75, 1, &table) == 1); // local human
    OA_CHECK(get(ranges, 1, 75, 4, &table) == 1); // computer, free slot
    OA_CHECK(get(ranges, 1, 75, 5, &table) == 0); // another machine's player
    OA_CHECK(get(ranges, 1, 75, 7, &table) == 0); // a free player slot
    OA_CHECK(get(ranges, 1, 75, 0, &table) == 0);
    OA_CHECK(get(ranges, 1, 75, 21, &table) == 0);
    // Under safe fidelity only a live unit answers: the computer's live unit
    // does, and the free place beside it does not.
    OA_CHECK(get(ranges, 1, 75, 3, &table, mr::ScriptFidelity::safe) == 1);
    OA_CHECK(get(ranges, 1, 75, 4, &table, mr::ScriptFidelity::safe) == 0);
}

void health_ignores_its_argument() {
    Ranges ranges;
    const mr::ScriptExtensionTable table = community_table();
    ranges.world->units[1].def = 1;
    ranges.world->units[1].health = 200;
    ranges.world->units[3].def = 1;
    ranges.world->units[3].health = 100;
    OA_CHECK(get(ranges, 1, static_cast<int32_t>(us::UnitValue::health), 3, &table) == 50);
}

void every_set_wakes_the_unit() {
    Ranges ranges;
    const mr::ScriptExtensionTable table = community_table();
    mr::MatchRules rules{};
    rules.script_get = table;
    us::UnitValueServices services{};
    services.rules.match = &rules;
    for (const int32_t selector : {71, 74, 21, 1000}) {
        oa::Unit& unit = ranges.world->units[1];
        unit.events = 0;
        const oa::Unit before = unit;
        us::unit_script_set_value(ranges.world, &unit, selector, 1, services);
        OA_CHECK(unit.events == us::event_script_state_changed);
        OA_CHECK(unit.state_flags == before.state_flags && unit.build_flags == before.build_flags);
    }
}

// A host whose GET goes to the engine's handler, as a match's does.
struct ValueHost : us::test::RecordingHost {
    ValueHost(oa::World* world, oa::Unit* unit, const us::UnitValueServices& services)
        : RecordingHost(2), world(world), unit(unit), services(services) {}

    int32_t
    get_unit_value(int32_t selector, int32_t first, int32_t second, int32_t, int32_t) override {
        ++reads;
        return us::unit_script_get_value(world, unit, selector, first, second, services);
    }

    oa::World* world;
    oa::Unit* unit;
    us::UnitValueServices services;
    std::size_t reads = 0;
};

// The loop mod scripts run: for id = MIN_ID .. MAX_ID, count the ids whose
// owner the caller allies, and the ones on this machine.
std::vector<uint32_t> scan_code() {
    const auto get_one = [](uint32_t selector, std::vector<uint32_t> argument) {
        std::vector<uint32_t> code{op::push_constant, selector};
        code.insert(code.end(), argument.begin(), argument.end());
        const std::vector<uint32_t> rest{
            op::push_constant, 0, op::push_constant, 0, op::push_constant, 0, op::get
        };
        code.insert(code.end(), rest.begin(), rest.end());
        return code;
    };
    std::vector<uint32_t> code{
        op::stack_alloc,
        op::push_constant,
        69,
        op::get_unit_value,
        op::pop_local,
        0, // id = MIN_ID
    };
    const uint32_t loop = static_cast<uint32_t>(code.size());
    const std::vector<uint32_t> test{
        op::push_local,
        0,
        op::push_constant,
        70,
        op::get_unit_value,
        op::less_equal,
        op::jump_if_false,
        0, // patched to the end
    };
    code.insert(code.end(), test.begin(), test.end());
    const std::size_t exit_operand = code.size() - 1;
    for (const auto& part : {
             get_one(74, {op::push_local, 0}),
             std::vector<uint32_t>{op::push_static, 0, op::add, op::pop_static, 0},
             get_one(75, {op::push_local, 0}),
             std::vector<uint32_t>{op::push_static, 1, op::add, op::pop_static, 1},
             std::vector<uint32_t>{
                 op::push_local, 0, op::push_constant, 1, op::add, op::pop_local, 0, op::jump, loop
             },
         })
        code.insert(code.end(), part.begin(), part.end());
    code[exit_operand] = static_cast<uint32_t>(code.size());
    const std::vector<uint32_t> end{op::push_constant, 0, op::return_};
    code.insert(code.end(), end.begin(), end.end());
    return code;
}

void a_script_walks_every_unit_id() {
    Ranges ranges;
    ranges.world->game.players[0].alliance[1] = 1;
    const mr::ScriptExtensionTable table = community_table();
    mr::MatchRules rules{};
    rules.script_get = table;
    us::UnitValueServices services{};
    services.rules.match = &rules;
    oa::formats::cob::CobProgram cob;
    cob.code = scan_code();
    cob.scripts.push_back({"Scan", 0});
    cob.entry_points.push_back(0);
    cob.header.static_variable_count = 2;
    cob.header.script_count = 1;
    cob.piece_names = {"base", "turret"};
    ValueHost host(ranges.world, &ranges.world->units[1], services);
    const auto shared = us::test::vm_program(cob);
    oa::sim::script_vm::Vm vm(shared, host);
    OA_CHECK(vm.start(0, {}).ok());
    const auto run = vm.tick(0);
    OA_CHECK(run.ok());
    // Ids 1..20: player 0's two slots and player 1's two are allied; players 0
    // and 1 are on this machine.
    OA_CHECK(*vm.static_value(0) == 4);
    OA_CHECK(*vm.static_value(1) == 4);
    OA_CHECK(host.reads == 2 + 20 * 3);

    // Without the extensions the loop never starts: MAX_ID reads 0.
    ValueHost bare(ranges.world, &ranges.world->units[1], us::UnitValueServices{});
    oa::sim::script_vm::Vm plain(shared, bare);
    OA_CHECK(plain.program().code.data() == vm.program().code.data());
    OA_CHECK(plain.start(0, {}).ok());
    OA_CHECK(plain.tick(0).ok());
    OA_CHECK(*plain.static_value(0) == 0);
}

void a_full_table_walk_fits_one_tick() {
    // The largest table a 16-bit id names, walked in one tick without a sleep.
    us::test::WorldFixture fixture{65531};
    oa::World* world = fixture.world;
    world->game.units_per_player = 6553;
    world->game.max_units_setting = 6553;
    for (uint32_t slot = 1; slot < 65531; ++slot)
        world->units[slot].owner_index = static_cast<uint8_t>((slot - 1) / 6553);
    world->game.players[0].alliance[0] = 1;
    world->game.players[0].status = OA_PLAYER_STATUS_LOCAL;
    const mr::ScriptExtensionTable table = community_table();
    mr::MatchRules rules{};
    rules.script_get = table;
    us::UnitValueServices services{};
    services.rules.match = &rules;
    oa::formats::cob::CobProgram cob;
    cob.code = scan_code();
    cob.scripts.push_back({"Scan", 0});
    cob.entry_points.push_back(0);
    cob.header.static_variable_count = 2;
    cob.header.script_count = 1;
    cob.piece_names = {"base", "turret"};
    ValueHost host(world, &world->units[1], services);
    oa::sim::script_vm::Vm vm(us::test::vm_program(cob), host);
    OA_CHECK(vm.start(0, {}).ok());
    OA_CHECK(vm.tick(0).ok());
    OA_CHECK(*vm.static_value(0) == 6553);
    OA_CHECK(*vm.static_value(1) == 6553);
}

} // namespace

int main() {
    unmounted_indices_read_zero();
    extensions_mount_at_any_index();
    caller_values();
    owner_of_reads_the_slot();
    build_percent_left_of_reads_another_unit();
    allied_with_is_one_way();
    is_local_reads_the_owner_status();
    health_ignores_its_argument();
    every_set_wakes_the_unit();
    a_script_walks_every_unit_id();
    a_full_table_walk_fits_one_tick();
    return oa::test::check_exit_status();
}
