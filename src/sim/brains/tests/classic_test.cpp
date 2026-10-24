// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Classic beside the built-in computer player on the fake match the AI tests
// use. test_controller_presence has no Classic version: it only asks which
// player records have a controller, and Classic does not decide that.
//
// A 400-tick run keeps the test_tick units and does not clear them for a
// strike. Placeholder task bodies do not recruit and do not draw a
// construction pick, so that run's checks are the section markers, the
// assigns made inside the sort section, and the standing bits. Orders, the
// draw counts and the full squad map are printed at the first difference
// and are not assertions. Strike recruitment is tick 301 on the built-in
// player; the sort itself still agrees.

#include "classic_harness.hpp"

#include "oa/sim/brains/classic.hpp"
#include "oa/test/game_data.hpp"

#include "match_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using oa::test::classic_harness::Assign;
using oa::test::classic_harness::PackageBytes;
using oa::test::classic_harness::PathRead;
using oa::test::classic_harness::SortAsk;
using oa::test::classic_harness::TickLog;
using oa::test::classic_harness::capture_random;
using oa::test::classic_harness::classic_spec;
using oa::test::classic_harness::copy_catalog;
using oa::test::classic_harness::note_builtin_section;
using oa::test::classic_harness::open_classic;
using oa::test::classic_harness::play_classic;
using oa::test::classic_harness::read_path;
using oa::test::classic_harness::recording_setup;
using oa::test::classic_harness::refresh_knowledge;
using oa::test::classic_harness::set_squad_builtin;
using oa::test::classic_harness::set_squad_classic;
using oa::test::classic_harness::setup_draw;
using oa::test::classic_harness::shared_random_kind;
using oa::test::classic_harness::sort_squad;
using oa::test::classic_harness::stamp_ids;
using oa::test::classic_harness::standing_bits;
using oa::test::classic_harness::standing_fire;
using oa::test::classic_harness::standing_move;
using oa::test::classic_harness::tick_players;
using oa::test::classic_harness::unwatch;
using oa::test::classic_harness::watch;
using oa::test::computer_fake::ARMCK;
using oa::test::computer_fake::ARMCOM;
using oa::test::computer_fake::ARMFIG;
using oa::test::computer_fake::ARMLAB;
using oa::test::computer_fake::ARMLLT;
using oa::test::computer_fake::ARMPT;
using oa::test::computer_fake::ARMPW;
using oa::test::computer_fake::CORMEX;
using oa::test::computer_fake::Fake;
using oa::test::computer_fake::describe_catalog;
using oa::test::computer_fake::host_for;
using oa::test::computer_fake::kBuildLists;

namespace {

using oa::sim::ai::ComputerHost;
using oa::sim::ai::ComputerPlayers;
using oa::sim::ai::Squad;

struct HeldPlayers {
    ComputerPlayers state{};

    HeldPlayers() = default;
    HeldPlayers(const HeldPlayers&) = delete;
    HeldPlayers& operator=(const HeldPlayers&) = delete;

    ~HeldPlayers() { oa::sim::ai::computer_players_release(&state); }
};

struct Noted {
    bool orders{};
    bool squads{};
    bool draws{};
};

struct Twin {
    Fake builtin;
    Fake classic_world;
    oa::data::match_rules::MatchRules rules{};
    oa::data::brain_facts::GameFacts facts{};
    std::vector<oa::data::unit_definitions::UnitDefinition> definitions;
    oa::sim::match_runtime::SetUpRecord setup{};
    HeldPlayers builtin_players;
    HeldPlayers classic_players;
    ComputerHost builtin_host{};
    ComputerHost classic_host{};
    TickLog builtin_log;
    TickLog classic_log;
    Noted noted;
    std::optional<oa::sim::brains::ToolSeat> seat;
};

void arm_hosts(Twin& twin) {
    twin.builtin_host = host_for(twin.builtin);
    twin.classic_host = host_for(twin.classic_world);
    twin.builtin_host.set_squad = set_squad_builtin;
    twin.classic_host.set_squad = set_squad_classic;
}

void fill_types(Twin& twin) {
    describe_catalog(twin.builtin_players.state);
    describe_catalog(twin.classic_players.state);
    CHECK((oa::sim::ai::computer_players_configure(&twin.builtin_players.state, "", kBuildLists)));
    CHECK((oa::sim::ai::computer_players_configure(&twin.classic_players.state, "", kBuildLists)));
}

void copy_both(Twin& twin) {
    std::vector<oa::data::unit_definitions::UnitDefinition> unused;
    copy_catalog(twin.builtin, twin.builtin_players.state, unused);
    copy_catalog(twin.classic_world, twin.classic_players.state, twin.definitions);
}

void seat_pair(Twin& twin, const PackageBytes& package) {
    const auto saved = twin.builtin_host.random;
    recording_setup = &twin.setup;
    twin.builtin_host.random = capture_random;
    CHECK(
        (oa::sim::ai::computer_players_initialize(&twin.builtin_players.state, twin.builtin_host))
    );
    recording_setup = nullptr;
    twin.builtin_host.random = saved;
    CHECK(
        (oa::sim::ai::computer_players_initialize(&twin.classic_players.state, twin.classic_host))
    );
    twin.builtin_host.note_section = note_builtin_section;
    twin.seat = open_classic(package, twin.classic_host, twin.facts, twin.definitions, &twin.setup);
}

bool same_assigns(const TickLog& left, const TickLog& right) {
    if (left.assigns.size() != right.assigns.size())
        return false;
    for (std::size_t index = 0; index < left.assigns.size(); ++index) {
        const Assign& a = left.assigns[index];
        const Assign& b = right.assigns[index];
        if (a.slot != b.slot || a.squad != b.squad)
            return false;
    }
    return true;
}

bool same_standing(const Fake& left, const Fake& right) {
    if (left.units.size() != right.units.size())
        return false;
    for (std::size_t index = 0; index < left.units.size(); ++index) {
        if (standing_bits(left.units[index]) != standing_bits(right.units[index]))
            return false;
    }
    return true;
}

void note_orders(Twin& twin, uint32_t turn, std::size_t builtin_from, std::size_t classic_from) {
    if (twin.noted.orders)
        return;
    const auto& left = twin.builtin.orders;
    const auto& right = twin.classic_world.orders;
    const std::size_t left_new = left.size() - builtin_from;
    const std::size_t right_new = right.size() - classic_from;
    bool differ = left_new != right_new;
    const std::size_t count = left_new < right_new ? left_new : right_new;
    for (std::size_t index = 0; index < count && !differ; ++index) {
        const auto& a = left[builtin_from + index];
        const auto& b = right[classic_from + index];
        differ = a.kind != b.kind || a.unit != b.unit || a.arg != b.arg;
    }
    if (!differ)
        return;
    std::fprintf(stderr, "orders differ at tick %u\n", turn);
    twin.noted.orders = true;
}

void note_squads(Twin& twin, uint32_t turn) {
    if (twin.noted.squads)
        return;
    const std::size_t count = twin.builtin.units.size();
    for (std::size_t index = 0; index < count; ++index) {
        if (twin.builtin.units[index].squad == twin.classic_world.units[index].squad)
            continue;
        std::fprintf(
            stderr, "squads differ at tick %u slot %u\n", turn, static_cast<unsigned>(index)
        );
        twin.noted.squads = true;
        return;
    }
}

void note_draws(Twin& twin, uint32_t turn, std::size_t builtin_roll, std::size_t classic_roll) {
    if (twin.noted.draws)
        return;
    const std::size_t builtin_used = twin.builtin.roll - builtin_roll;
    const std::size_t classic_used = twin.classic_world.roll - classic_roll;
    if (builtin_used == classic_used)
        return;
    std::fprintf(stderr, "draws differ at tick %u\n", turn);
    twin.noted.draws = true;
}

void step(Twin& twin, uint32_t turn) {
    watch(twin.builtin_log, twin.classic_log);
    const std::size_t builtin_orders = twin.builtin.orders.size();
    const std::size_t classic_orders = twin.classic_world.orders.size();
    const std::size_t builtin_roll = twin.builtin.roll;
    const std::size_t classic_roll = twin.classic_world.roll;
    twin.builtin.world.game.tick = turn;
    twin.classic_world.world.game.tick = turn;
    tick_players(twin.builtin_players.state, twin.builtin, twin.builtin_host);
    play_classic(*twin.seat, turn);
    refresh_knowledge(twin.classic_players.state, twin.classic_world, 1);
    unwatch();
    if (twin.builtin_log.markers != twin.classic_log.markers) {
        std::fprintf(stderr, "markers differ at tick %u\n", turn);
        CHECK(false);
    }
    if (!same_assigns(twin.builtin_log, twin.classic_log)) {
        std::fprintf(stderr, "sort assigns differ at tick %u\n", turn);
        CHECK(false);
    }
    if (!same_standing(twin.builtin, twin.classic_world)) {
        std::fprintf(stderr, "standing bits differ at tick %u\n", turn);
        CHECK(false);
    }
    note_orders(twin, turn, builtin_orders, classic_orders);
    note_squads(twin, turn);
    note_draws(twin, turn, builtin_roll, classic_roll);
}

void test_sort_squads(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    twin.classic_world.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    stamp_ids(twin.classic_world);
    twin.seat = open_classic(package, twin.classic_host, twin.facts, twin.definitions, nullptr);

    SortAsk solar;
    solar.building = true;
    solar.rules = "base";
    CHECK(sort_squad(*twin.seat, solar) == static_cast<lua_Integer>(Squad::structures));

    SortAsk defence = solar;
    defence.armed = true;
    CHECK(sort_squad(*twin.seat, defence) == static_cast<lua_Integer>(Squad::armed_structures));

    SortAsk army;
    army.armed = true;
    army.rules = "base";
    CHECK(sort_squad(*twin.seat, army) == static_cast<lua_Integer>(Squad::land_army));
    SortAsk builder = army;
    builder.flags = OA_UNIT_DEF_FLAG_BUILDER;
    CHECK(sort_squad(*twin.seat, builder) == static_cast<lua_Integer>(Squad::builders));
    SortAsk aircraft = army;
    aircraft.flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    CHECK(sort_squad(*twin.seat, aircraft) == static_cast<lua_Integer>(Squad::aircraft));
    SortAsk navy = army;
    navy.min_water = 12;
    CHECK(sort_squad(*twin.seat, navy) == static_cast<lua_Integer>(Squad::navy));
    SortAsk unarmed;
    unarmed.rules = "base";
    CHECK(sort_squad(*twin.seat, unarmed) == static_cast<lua_Integer>(Squad::none));

    SortAsk factory;
    factory.building = true;
    factory.flags = OA_UNIT_DEF_FLAG_BUILDER;
    factory.rules = "role-squads";
    CHECK(sort_squad(*twin.seat, factory) == static_cast<lua_Integer>(Squad::structures));

    SortAsk spends = factory;
    spends.flags = 0;
    spends.has_energy = true;
    spends.energy = 58.5F;
    CHECK(sort_squad(*twin.seat, spends) == static_cast<lua_Integer>(Squad::structures));
    spends.energy = 58.49F;
    CHECK(sort_squad(*twin.seat, spends) == static_cast<lua_Integer>(Squad::armed_structures));
    spends.energy = -200.0F;
    CHECK(sort_squad(*twin.seat, spends) == static_cast<lua_Integer>(Squad::armed_structures));
    spends.energy = 1000.0F;
    spends.armed = true;
    CHECK(sort_squad(*twin.seat, spends) == static_cast<lua_Integer>(Squad::structures));
    SortAsk base_defence = spends;
    base_defence.rules = "base";
    base_defence.has_energy = false;
    CHECK(
        sort_squad(*twin.seat, base_defence) == static_cast<lua_Integer>(Squad::armed_structures)
    );

    SortAsk builder_first;
    builder_first.flags = OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_CAN_FLY;
    builder_first.rules = "role-squads";
    CHECK(sort_squad(*twin.seat, builder_first) == static_cast<lua_Integer>(Squad::builders));
    SortAsk wing = builder_first;
    wing.flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    CHECK(sort_squad(*twin.seat, wing) == static_cast<lua_Integer>(Squad::aircraft));
    SortAsk wet;
    wet.rules = "role-squads";
    wet.min_water = 1;
    CHECK(sort_squad(*twin.seat, wet) == static_cast<lua_Integer>(Squad::navy));
    wet.min_water = -10;
    wet.max_water = 128;
    CHECK(sort_squad(*twin.seat, wet) == static_cast<lua_Integer>(Squad::navy));
    SortAsk base_diver = wet;
    base_diver.rules = "base";
    CHECK(sort_squad(*twin.seat, base_diver) == static_cast<lua_Integer>(Squad::none));
    wet.max_water = 127;
    CHECK(sort_squad(*twin.seat, wet) == static_cast<lua_Integer>(Squad::land_army));
    wet.flags = OA_UNIT_DEF_FLAG_AMPHIBIOUS;
    wet.max_water = 0;
    CHECK(sort_squad(*twin.seat, wet) == static_cast<lua_Integer>(Squad::navy));
    SortAsk base_amphibian = wet;
    base_amphibian.rules = "base";
    CHECK(sort_squad(*twin.seat, base_amphibian) == static_cast<lua_Integer>(Squad::none));
}

void run_assignment(const PackageBytes& package, bool role_rules) {
    Twin twin;
    arm_hosts(twin);
    twin.builtin.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    twin.classic_world.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    fill_types(twin);
    using SquadRules = oa::data::match_rules::AiSquadAssignmentRules;
    const SquadRules rules_value = role_rules ? SquadRules::role_squads : SquadRules::base;
    twin.rules.ai.squad_assignment.enabled = rules_value != SquadRules::base;
    twin.rules.ai.squad_assignment.rules = rules_value;
    twin.builtin_players.state.rules.match = &twin.rules;
    twin.classic_players.state.rules.match = &twin.rules;
    if (role_rules) {
        twin.facts.mod.profile = oa::data::mod_profile::ModProfile{};
        twin.facts.mod.profile->rules = twin.rules;
    }
    twin.builtin_players.state.types[ARMCK].abilities = OA_UNIT_DEF_ABILITY_COMMANDER;
    twin.classic_players.state.types[ARMCK].abilities = OA_UNIT_DEF_ABILITY_COMMANDER;
    copy_both(twin);
    auto& com = twin.builtin.spawn(10, 1, ARMCOM, 200, 200, 0);
    auto& ck = twin.builtin.spawn(11, 1, ARMCK, 220, 200, 0);
    auto& tank = twin.builtin.spawn(12, 1, ARMPW, 240, 200, 0);
    auto& classic_com = twin.classic_world.spawn(10, 1, ARMCOM, 200, 200, 0);
    auto& classic_ck = twin.classic_world.spawn(11, 1, ARMCK, 220, 200, 0);
    auto& classic_tank = twin.classic_world.spawn(12, 1, ARMPW, 240, 200, 0);
    twin.builtin.rolls.assign(4096, 7);
    twin.classic_world.rolls.assign(4096, 7);
    stamp_ids(twin.builtin);
    stamp_ids(twin.classic_world);
    seat_pair(twin, package);
    for (uint32_t turn = 1; turn <= 120; ++turn)
        step(twin, turn);
    CHECK(standing_move(com) == (role_rules ? 2u : 1u));
    CHECK(standing_move(ck) == (role_rules ? 1u : 2u));
    CHECK(standing_fire(com) == 2u && standing_fire(ck) == 2u);
    CHECK(tank.squad == (role_rules ? static_cast<int32_t>(Squad::land_army) : 0));
    CHECK(standing_move(classic_com) == standing_move(com));
    CHECK(standing_move(classic_ck) == standing_move(ck));
    CHECK(standing_fire(classic_com) == 2u && standing_fire(classic_ck) == 2u);
    CHECK(classic_tank.squad == tank.squad);
}

void test_squad_assignment(const PackageBytes& package) {
    run_assignment(package, false);
    run_assignment(package, true);
}

void test_scripted_units_stay_unsorted(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    twin.builtin.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
    twin.builtin.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    twin.classic_world.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
    twin.classic_world.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    fill_types(twin);
    copy_both(twin);
    auto& free_tank = twin.builtin.spawn(12, 1, ARMPW, 300, 300, OA_UNIT_FLAG_HAS_WEAPONS);
    auto& scripted = twin.builtin.spawn(13, 1, ARMPW, 320, 300, OA_UNIT_FLAG_HAS_WEAPONS);
    scripted.flags = (scripted.flags & ~OA_UNIT_FLAG_SELECTABLE) | OA_UNIT_FLAG_NOT_SELECTABLE;
    auto& tower = twin.builtin.spawn(
        14, 1, ARMLLT, 340, 300, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS
    );
    tower.flags &= ~OA_UNIT_FLAG_SELECTABLE;
    auto& classic_free = twin.classic_world.spawn(12, 1, ARMPW, 300, 300, OA_UNIT_FLAG_HAS_WEAPONS);
    auto& classic_scripted =
        twin.classic_world.spawn(13, 1, ARMPW, 320, 300, OA_UNIT_FLAG_HAS_WEAPONS);
    classic_scripted.flags =
        (classic_scripted.flags & ~OA_UNIT_FLAG_SELECTABLE) | OA_UNIT_FLAG_NOT_SELECTABLE;
    auto& classic_tower = twin.classic_world.spawn(
        14, 1, ARMLLT, 340, 300, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS
    );
    classic_tower.flags &= ~OA_UNIT_FLAG_SELECTABLE;
    twin.builtin.rolls.assign(4096, 7);
    twin.classic_world.rolls.assign(4096, 7);
    stamp_ids(twin.builtin);
    stamp_ids(twin.classic_world);
    seat_pair(twin, package);
    for (uint32_t turn = 1; turn <= 30; ++turn)
        step(twin, turn);
    CHECK(free_tank.squad == static_cast<int32_t>(Squad::land_army));
    CHECK(standing_move(free_tank) == 2u && standing_fire(free_tank) == 2u);
    CHECK(scripted.squad == 0 && tower.squad == 0);
    CHECK(standing_move(scripted) == 0u && standing_fire(scripted) == 0u);
    CHECK(classic_free.squad == free_tank.squad);
    CHECK(classic_scripted.squad == 0 && classic_tower.squad == 0);
    scripted.flags = (scripted.flags & ~OA_UNIT_FLAG_NOT_SELECTABLE) | OA_UNIT_FLAG_SELECTABLE;
    classic_scripted.flags =
        (classic_scripted.flags & ~OA_UNIT_FLAG_NOT_SELECTABLE) | OA_UNIT_FLAG_SELECTABLE;
    for (uint32_t turn = 31; turn <= 60; ++turn)
        step(twin, turn);
    CHECK(scripted.squad == static_cast<int32_t>(Squad::land_army));
    CHECK(standing_move(scripted) == 2u && standing_fire(scripted) == 2u);
    CHECK(tower.squad == 0);
    CHECK(classic_scripted.squad == scripted.squad);
    CHECK(classic_tower.squad == 0);
}

void expect_whole(const PathRead& read, lua_Integer expect) {
    CHECK(read.ok && read.whole && read.number == expect);
}

void test_tick(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
        fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        fake->strength[CORMEX][0] = 0;
        fake->strength[CORMEX][1] = 0;
        fake->strength[CORMEX][2] = 0;
        fake->spawn(1, 0, ARMLLT, 900, 900, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
        fake->spawn(10, 1, ARMCOM, 200, 200, 0);
        fake->spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING);
        for (uint32_t slot = 12; slot < 16; ++slot) {
            fake->spawn(
                slot, 1, ARMPW, 300 + static_cast<int32_t>(slot), 300, OA_UNIT_FLAG_HAS_WEAPONS
            );
        }
        fake->rolls.assign(4096, 7);
        stamp_ids(*fake);
    }
    fill_types(twin);
    copy_both(twin);
    seat_pair(twin, package);
    step(twin, 1);
    expect_whole(read_path(*twin.seat, "sort_countdown"), 29);
    expect_whole(read_path(*twin.seat, "tasks[1].next"), 31);
    expect_whole(read_path(*twin.seat, "tasks[2].next"), 301);
    expect_whole(read_path(*twin.seat, "tasks[3].next"), 151);
    expect_whole(read_path(*twin.seat, "tasks[4].next"), 91);
    expect_whole(read_path(*twin.seat, "tasks[5].next"), 0);
    const PathRead idle = read_path(*twin.seat, "tasks[5].kind");
    CHECK(idle.ok && idle.text && std::strcmp(idle.words, "idle") == 0);
    expect_whole(read_path(*twin.seat, "tasks[6].next"), 301);
    expect_whole(read_path(*twin.seat, "tasks[7].next"), 151);
    expect_whole(read_path(*twin.seat, "tasks[8].next"), 38);
    expect_whole(read_path(*twin.seat, "tasks[9].next"), 38);
    for (uint32_t turn = 2; turn <= 29; ++turn)
        step(twin, turn);
    CHECK(twin.builtin.units[10].squad == 0);
    CHECK(twin.classic_world.units[10].squad == 0);
    for (uint32_t turn = 30; turn <= 120; ++turn)
        step(twin, turn);
    CHECK(twin.builtin.units[10].squad == 4);
    CHECK(twin.builtin.units[11].squad == 1);
    CHECK(twin.builtin.units[12].squad == 3);
    CHECK(standing_move(twin.builtin.units[12]) == 2u);
    CHECK(standing_move(twin.builtin.units[10]) == 1u);
    CHECK(twin.classic_world.units[10].squad == 4);
    CHECK(twin.classic_world.units[11].squad == 1);
    CHECK(twin.classic_world.units[12].squad == 3);
    CHECK(standing_move(twin.classic_world.units[12]) == 2u);
    CHECK(standing_move(twin.classic_world.units[10]) == 1u);
    for (uint32_t turn = 121; turn <= 400; ++turn)
        step(twin, turn);
}

void test_siege_centre(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    twin.classic_world.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    twin.classic_world.world.game.map_width_world = 1024;
    twin.classic_world.world.game.map_height_world = 512;
    stamp_ids(twin.classic_world);
    twin.seat = open_classic(package, twin.classic_host, twin.facts, twin.definitions, nullptr);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_target.x"), 33554432);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_target.y"), 0);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_target.z"), 16777216);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_probe.x"), 33554432);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_step.z"), 16777216);
}

void test_setup_random(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    twin.builtin.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    twin.classic_world.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    fill_types(twin);
    copy_both(twin);
    twin.builtin.rolls.assign(4096, 7);
    twin.classic_world.rolls.assign(4096, 7);
    stamp_ids(twin.builtin);
    stamp_ids(twin.classic_world);
    seat_pair(twin, package);
    CHECK(twin.setup.draw_count > 0);
    for (uint8_t index = 0; index < twin.setup.draw_count; ++index) {
        const auto& draw = twin.setup.draws[index];
        const lua_Integer got = setup_draw(*twin.seat, static_cast<lua_Integer>(draw.bound));
        CHECK(got == static_cast<lua_Integer>(draw.value));
    }
}

void flip_quirk(PackageBytes& package) {
    bool flipped = false;
    for (std::size_t index = 0; index < package.paths.size(); ++index) {
        if (package.paths[index] != "brain.lua")
            continue;
        std::vector<uint8_t>& bytes = package.bytes[index];
        const char mark[] = "@quirk";
        for (std::size_t at = 0; at + sizeof mark - 1 <= bytes.size(); ++at) {
            bool found = true;
            for (std::size_t step = 0; step + 1 < sizeof mark; ++step) {
                if (bytes[at + step] != static_cast<uint8_t>(mark[step]))
                    found = false;
            }
            if (!found)
                continue;
            bytes[at + 1] = static_cast<uint8_t>('Q');
            flipped = true;
            break;
        }
    }
    CHECK(flipped);
    package.bind();
}

void test_fingerprint(const char* root) {
    PackageBytes package;
    package.load(root);
    Fixture fixture;
    const oa::sim::brains::SeatOutcome seated =
        oa::sim::brains::seat_brain(*fixture.match, classic_spec(package));
    CHECK(seated.result == oa::sim::brains::SeatResult::seated);
    CHECK(oa::sim::brains::seat_is_classic(*fixture.match, computer));
    oa::data::script_host::Sandbox* sandbox =
        oa::sim::brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    CHECK(shared_random_kind(*sandbox) == LUA_TFUNCTION);

    PackageBytes changed;
    changed.load(root);
    flip_quirk(changed);
    Fixture again;
    const oa::sim::brains::SeatOutcome reseated =
        oa::sim::brains::seat_brain(*again.match, classic_spec(changed));
    CHECK(reseated.result == oa::sim::brains::SeatResult::seated);
    CHECK(!oa::sim::brains::seat_is_classic(*again.match, computer));
    oa::data::script_host::Sandbox* plain = oa::sim::brains::seat_sandbox(*again.match, computer);
    CHECK(plain != nullptr);
    CHECK(shared_random_kind(*plain) == LUA_TNIL);
}

} // namespace

void test_build_list_limits(const PackageBytes& package);
void test_whole_build_lists(const PackageBytes& package);
void test_profile(const PackageBytes& package);
void test_adopt_handed_player(const PackageBytes& package);
void test_downloadable_directives(const PackageBytes& package);
void test_profile_reload(const PackageBytes& package);
void test_profile_leading_lines(const PackageBytes& package);
void test_difficulty_names(const PackageBytes& package);
void test_profile_twins(const PackageBytes& package);
void test_knowledge_twins(const PackageBytes& package);
void test_grid_and_metal_twins(const PackageBytes& package);
void test_installed_data(const char* package_root);

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: oa-sim-brains-classic-test --data <brains/classic>\n");
            return 2;
        }
        try {
            test_installed_data(argv[2]);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            return 1;
        }
        std::printf("sim-brains-classic-data: ok\n");
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: oa-sim-brains-classic-test <brains/classic>\n");
        return 2;
    }
    try {
        PackageBytes package;
        package.load(argv[1]);
        test_sort_squads(package);
        test_squad_assignment(package);
        test_scripted_units_stay_unsorted(package);
        test_tick(package);
        test_siege_centre(package);
        test_setup_random(package);
        test_fingerprint(argv[1]);
        test_build_list_limits(package);
        test_whole_build_lists(package);
        test_profile(package);
        test_adopt_handed_player(package);
        test_downloadable_directives(package);
        test_profile_reload(package);
        test_profile_leading_lines(package);
        test_difficulty_names(package);
        test_profile_twins(package);
        test_knowledge_twins(package);
        test_grid_and_metal_twins(package);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::printf("sim-brains-classic: ok\n");
    return 0;
}
