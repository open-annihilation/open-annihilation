// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Classic beside the built-in computer player on the fake match the AI tests
// use. test_controller_presence has no Classic version: it only asks which
// player records have a controller, and Classic does not decide that.
// test_weight_report belongs to the report that follows this ticket.
//
// test_tick follows the whole run: ticks 1..120, then one tick at 400 after
// the home units are cleared. That tick recruits the strike squad and
// attacks. Section markers, sort assigns and standing bits are checked on
// every tick. With the guard up, every task section's draws and orders are
// checked too.

#include "classic_harness.hpp"

#include "oa/base/game_math.hpp"
#include "oa/core/unit_def.h"
#include "oa/core/world.h"
#include "oa/sim/ai.hpp"
#include "oa/sim/brains/classic.hpp"
#include "oa/sim/detection.hpp"
#include "oa/sim/unit_movement/movement.hpp"
#include "oa/test/game_data.hpp"

#include "match_fixture.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using oa::test::classic_harness::Assign;
using oa::test::classic_harness::ClassicBind;
using oa::test::classic_harness::PackageBytes;
using oa::test::classic_harness::RollSync;
using oa::test::classic_harness::SectionCut;
using oa::test::classic_harness::active_sync;
using oa::test::classic_harness::builds_held;
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
using oa::test::computer_fake::ARMMEX;
using oa::test::computer_fake::ARMPT;
using oa::test::computer_fake::ARMPW;
using oa::test::computer_fake::ARMSOLAR;
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
    bool check_squads{};
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
        if (twin.check_squads)
            CHECK(false);
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

std::size_t later_mark(const SectionCut* cuts, int task, std::size_t end_roll, bool orders) {
    for (int index = task + 1; index < 10; ++index) {
        if (!cuts[index].marked)
            continue;
        return orders ? cuts[index].order_index : cuts[index].arrived_roll;
    }
    return end_roll;
}

bool same_order(
    const oa::test::computer_fake::Order& left, const oa::test::computer_fake::Order& right
) {
    return left.kind == right.kind && left.unit == right.unit && left.arg == right.arg &&
           left.at.x == right.at.x && left.at.y == right.at.y && left.at.z == right.at.z &&
           left.queue == right.queue;
}

void compare_synced_sections(const Twin& twin, const RollSync& sync, uint32_t turn) {
    for (int task = 1; task <= 9; ++task) {
        const SectionCut& left = sync.builtin_cuts[task];
        const SectionCut& right = sync.classic_cuts[task];
        if (left.marked != right.marked) {
            std::fprintf(stderr, "task %d markers differ at tick %u\n", task, turn);
            CHECK(false);
        }
        if (!left.marked)
            continue;
        const std::size_t left_draws =
            later_mark(sync.builtin_cuts, task, twin.builtin.roll, false) - left.body_roll;
        const std::size_t right_draws =
            later_mark(sync.classic_cuts, task, twin.classic_world.roll, false) - right.body_roll;
        if (left_draws != right_draws) {
            std::fprintf(
                stderr,
                "task %d draws differ at tick %u: %lu and %lu\n",
                task,
                turn,
                static_cast<unsigned long>(left_draws),
                static_cast<unsigned long>(right_draws)
            );
            CHECK(false);
        }
        const std::size_t left_from = left.order_index;
        const std::size_t right_from = right.order_index;
        const std::size_t left_to =
            later_mark(sync.builtin_cuts, task, twin.builtin.orders.size(), true);
        const std::size_t right_to =
            later_mark(sync.classic_cuts, task, twin.classic_world.orders.size(), true);
        if (left_to - left_from != right_to - right_from) {
            std::fprintf(stderr, "task %d orders differ at tick %u\n", task, turn);
            CHECK(false);
        }
        const std::size_t count = left_to - left_from;
        for (std::size_t index = 0; index < count; ++index) {
            if (same_order(
                    twin.builtin.orders[left_from + index],
                    twin.classic_world.orders[right_from + index]
                ))
                continue;
            std::fprintf(
                stderr,
                "task %d order %lu differs at tick %u\n",
                task,
                static_cast<unsigned long>(index),
                turn
            );
            CHECK(false);
        }
    }
}

struct SyncGuard {
    explicit SyncGuard(Twin& twin) {
        sync.builtin = &twin.builtin;
        sync.classic = &twin.classic_world;
        active_sync = &sync;
    }

    ~SyncGuard() { active_sync = nullptr; }

    SyncGuard(const SyncGuard&) = delete;
    SyncGuard& operator=(const SyncGuard&) = delete;

    RollSync sync{};
};

void step(Twin& twin, uint32_t turn) {
    RollSync* sync = active_sync;
    if (sync != nullptr) {
        for (int index = 0; index < 10; ++index) {
            sync->builtin_cuts[index] = {};
            sync->classic_cuts[index] = {};
        }
    }
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
    if (sync != nullptr)
        compare_synced_sections(twin, *sync, turn);
    else {
        note_orders(twin, turn, builtin_orders, classic_orders);
        note_draws(twin, turn, builtin_roll, classic_roll);
    }
    note_squads(twin, turn);
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

void open_playing(Twin& twin, const PackageBytes& package, bool hold);

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
    open_playing(twin, package, false);
    twin.check_squads = true;
    SyncGuard guard(twin);
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
    bool builtin_factory = false;
    bool classic_factory = false;
    bool builtin_build = false;
    bool classic_build = false;
    for (const auto& order : twin.builtin.orders) {
        builtin_factory = builtin_factory || (order.kind == "factory" && order.unit == 11 &&
                                              (order.arg == ARMPW || order.arg == ARMCK));
        builtin_build = builtin_build || (order.kind == "build" && order.unit == 10);
    }
    for (const auto& order : twin.classic_world.orders) {
        classic_factory = classic_factory || (order.kind == "factory" && order.unit == 11 &&
                                              (order.arg == ARMPW || order.arg == ARMCK));
        classic_build = classic_build || (order.kind == "build" && order.unit == 10);
    }
    CHECK(builtin_factory);
    CHECK(classic_factory);
    CHECK(builtin_build);
    CHECK(classic_build);
    // The home squads are cleared, then one tick at 400 recruits and attacks.
    // Ticks 121..399 are not played, so the strike's next tick is still 301.
    twin.builtin.orders.clear();
    twin.classic_world.orders.clear();
    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        fake->set_squad(11, 0);
        fake->units[11].flags = 0;
        fake->set_squad(10, 0);
        fake->units[10].flags = 0;
    }
    step(twin, 400);
    CHECK((twin.builtin.squads[{1, 2}].size() == 4));
    CHECK((twin.classic_world.squads[{1, 2}].size() == 4));
    bool builtin_attack = false;
    bool classic_attack = false;
    for (const auto& order : twin.builtin.orders)
        builtin_attack = builtin_attack || (order.kind == "attack" && order.arg == 1);
    for (const auto& order : twin.classic_world.orders)
        classic_attack = classic_attack || (order.kind == "attack" && order.arg == 1);
    CHECK(builtin_attack);
    CHECK(classic_attack);
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

int32_t site_max_x = 60;
int32_t site_max_z = 60;
uint8_t metal_at[64][64]{};
bool feature_at[64][64]{};

uint8_t test_cell_metal(void*, int32_t x, int32_t z) {
    if (x < 0 || z < 0 || x >= 64 || z >= 64)
        return 0;
    return metal_at[z][x];
}

bool test_metal_feature(void*, int32_t x, int32_t z) {
    if (x < 0 || z < 0 || x >= 64 || z >= 64)
        return false;
    return feature_at[z][x];
}

bool test_site(void*, uint16_t, int32_t x, int32_t z) {
    return x >= 0 && z >= 0 && x < site_max_x && z < site_max_z;
}

void clear_ground() {
    std::memset(metal_at, 0, sizeof metal_at);
    std::memset(feature_at, 0, sizeof feature_at);
    site_max_x = 60;
    site_max_z = 60;
}

void use_ground(Twin& twin) {
    for (ComputerHost* host : {&twin.builtin_host, &twin.classic_host}) {
        host->cell_metal = test_cell_metal;
        host->metal_feature = test_metal_feature;
        host->site_clear = test_site;
        host->building_site = test_site;
    }
}

void use_map(Twin& twin, int32_t cells_x, int32_t cells_z, int32_t surface) {
    for (ComputerHost* host : {&twin.builtin_host, &twin.classic_host}) {
        host->map_cells_x = cells_x;
        host->map_cells_z = cells_z;
        host->surface_metal = surface;
    }
}

void publish_rules(Twin& twin) {
    twin.builtin_players.state.rules.match = &twin.rules;
    twin.classic_players.state.rules.match = &twin.rules;
    twin.facts.mod.profile = oa::data::mod_profile::ModProfile{};
    twin.facts.mod.profile->rules = twin.rules;
}

void open_playing(Twin& twin, const PackageBytes& package, bool hold) {
    builds_held.clear();
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
    ClassicBind bind;
    bind.build_runs = kBuildLists;
    bind.build_run_length = static_cast<uint32_t>(std::size(kBuildLists));
    bind.knowledge_fake = &twin.classic_world;
    bind.knowledge_players = &twin.classic_players.state;
    bind.install_builds_held = hold;
    if (hold)
        builds_held[&twin.classic_world] = 0;
    twin.seat =
        open_classic(package, twin.classic_host, twin.facts, twin.definitions, &twin.setup, &bind);
}

void play_for(Twin& twin, uint32_t until) {
    SyncGuard guard(twin);
    for (uint32_t turn = 1; turn <= until; ++turn)
        step(twin, turn);
}

uint32_t orders_of(const Fake& fake, const char* kind, uint16_t unit) {
    uint32_t count = 0;
    for (const auto& order : fake.orders)
        count += order.kind == kind && order.unit == unit ? 1u : 0u;
    return count;
}

uint32_t orders_named(const Fake& fake, const char* kind) {
    uint32_t count = 0;
    for (const auto& order : fake.orders)
        count += order.kind == kind ? 1u : 0u;
    return count;
}

uint32_t orders_arg(const Fake& fake, const char* kind, uint16_t arg) {
    uint32_t count = 0;
    for (const auto& order : fake.orders)
        count += order.kind == kind && order.arg == arg ? 1u : 0u;
    return count;
}

void copy_message(char* dest, std::size_t room, const char* text) {
    if (room == 0)
        return;
    std::snprintf(dest, room, "%s", text != nullptr ? text : "");
}

struct LuaCall {
    bool failed{};
    bool ran{};
    bool pick{};
    bool set_limit{};
    bool set_weight{};
    char message[256]{};
    int type_index{};
    int limit{};
    int weight{};
    int builder_type{};
    int score{};
    int picked{};
    int count{};
    int got{};
    int64_t distances[64]{};
    int32_t order[64]{};
};

void call_build_work(void* context, lua_State* state) {
    auto* ask = static_cast<LuaCall*>(context);
    const int seat = lua_gettop(state);
    if (lua_type(state, seat) != LUA_TTABLE) {
        ask->failed = true;
        copy_message(ask->message, sizeof ask->message, "the seat table is missing");
        return;
    }
    if (ask->set_limit) {
        lua_getfield(state, seat, "limits");
        if (lua_type(state, -1) == LUA_TTABLE) {
            lua_pushinteger(state, ask->limit);
            lua_rawseti(state, -2, ask->type_index);
        }
        lua_pop(state, 1);
    }
    if (ask->set_weight) {
        lua_getfield(state, seat, "weight_percent");
        if (lua_type(state, -1) == LUA_TTABLE) {
            lua_pushinteger(state, ask->weight);
            lua_rawseti(state, -2, ask->type_index);
        }
        lua_pop(state, 1);
    }
    lua_getglobal(state, "require");
    lua_pushstring(state, "build");
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        ask->failed = true;
        copy_message(ask->message, sizeof ask->message, lua_tostring(state, -1));
        return;
    }
    lua_getfield(state, -1, ask->pick ? "pick" : "score");
    lua_pushvalue(state, seat);
    if (ask->pick) {
        lua_newtable(state);
        lua_pushinteger(state, ask->builder_type);
        lua_setfield(state, -2, "type_index");
    } else {
        lua_pushinteger(state, ask->type_index);
    }
    if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
        ask->failed = true;
        copy_message(ask->message, sizeof ask->message, lua_tostring(state, -1));
        return;
    }
    if (lua_isinteger(state, -1) == 0) {
        ask->failed = true;
        copy_message(ask->message, sizeof ask->message, "build did not return an integer");
        return;
    }
    const int value = static_cast<int>(lua_tointeger(state, -1));
    if (ask->pick)
        ask->picked = value;
    else
        ask->score = value;
    ask->ran = true;
}

void call_heap_work(void* context, lua_State* state) {
    auto* ask = static_cast<LuaCall*>(context);
    lua_getglobal(state, "brain");
    lua_getfield(state, -1, "float_heap_order");
    lua_createtable(state, ask->count, 0);
    for (int index = 0; index < ask->count; ++index) {
        lua_pushinteger(state, static_cast<lua_Integer>(ask->distances[index]));
        lua_rawseti(state, -2, index + 1);
    }
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        ask->failed = true;
        copy_message(ask->message, sizeof ask->message, lua_tostring(state, -1));
        return;
    }
    if (lua_type(state, -1) != LUA_TTABLE) {
        ask->failed = true;
        copy_message(ask->message, sizeof ask->message, "float_heap_order did not return a table");
        return;
    }
    const int length = static_cast<int>(luaL_len(state, -1));
    if (length < 0 || length > 64) {
        ask->failed = true;
        copy_message(ask->message, sizeof ask->message, "float_heap_order returned a long table");
        return;
    }
    ask->got = length;
    for (int index = 1; index <= length; ++index) {
        lua_rawgeti(state, -1, index);
        if (lua_isinteger(state, -1) == 0) {
            ask->failed = true;
            copy_message(
                ask->message, sizeof ask->message, "float_heap_order returned a non-integer"
            );
            return;
        }
        ask->order[index - 1] = static_cast<int32_t>(lua_tointeger(state, -1));
        lua_pop(state, 1);
    }
    ask->ran = true;
}

struct ScoreRig {
    Fake fake;
    HeldPlayers players;
    ComputerHost host{};
    std::vector<oa::data::unit_definitions::UnitDefinition> definitions;
    oa::data::brain_facts::GameFacts facts{};
    std::optional<oa::sim::brains::ToolSeat> seat;

    void open(const PackageBytes& package) {
        host = host_for(fake);
        ClassicBind bind;
        bind.build_runs = kBuildLists;
        bind.build_run_length = static_cast<uint32_t>(std::size(kBuildLists));
        bind.knowledge_fake = &fake;
        bind.knowledge_players = &players.state;
        seat = open_classic(package, host, facts, definitions, nullptr, &bind);
    }
};

void run_lua(oa::sim::brains::ToolSeat& seat, LuaCall& ask, void (*work)(void*, lua_State*)) {
    oa::data::script_host::ScriptError error;
    if (!seat.with_seat(&ask, work, error))
        throw std::runtime_error(oa::data::script_host::describe(error));
    if (ask.failed)
        throw std::runtime_error(ask.message[0] != '\0' ? ask.message : "the Lua call failed");
    CHECK(ask.ran);
}

void expect_score(
    ScoreRig& rig,
    uint16_t type,
    int32_t expect,
    bool set_limit,
    int limit,
    bool set_weight,
    int weight
) {
    auto& knowledge = rig.players.state.players[1].knowledge;
    if (set_limit)
        knowledge.limits[type] = limit;
    if (set_weight)
        knowledge.weight_percent[type] = static_cast<uint8_t>(weight);
    const int32_t cpp = oa::sim::ai::computer_build_score(&rig.players.state, rig.host, 1, type);
    LuaCall ask;
    ask.type_index = static_cast<int>(type);
    ask.set_limit = set_limit;
    ask.limit = limit;
    ask.set_weight = set_weight;
    ask.weight = weight;
    run_lua(*rig.seat, ask, call_build_work);
    CHECK(cpp == expect);
    CHECK(ask.score == expect);
}

void expect_pick(
    ScoreRig& rig,
    uint16_t slot,
    uint16_t builder_type,
    const std::vector<uint32_t>& rolls,
    uint16_t expect
) {
    rig.fake.rolls = rolls;
    rig.fake.roll = 0;
    const auto cpp = oa::sim::ai::computer_pick_build(&rig.players.state, rig.host, 1, slot);
    rig.fake.roll = 0;
    LuaCall ask;
    ask.pick = true;
    ask.builder_type = static_cast<int>(builder_type);
    run_lua(*rig.seat, ask, call_build_work);
    CHECK(cpp == expect);
    CHECK(static_cast<uint16_t>(ask.picked) == expect);
}

void prepare_score(ScoreRig& rig, const PackageBytes&) {
    rig.fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    describe_catalog(rig.players.state);
    CHECK((oa::sim::ai::computer_players_configure(&rig.players.state, "", kBuildLists)));
    copy_catalog(rig.fake, rig.players.state, rig.definitions);
    stamp_ids(rig.fake);
    CHECK((oa::sim::ai::computer_players_initialize(&rig.players.state, host_for(rig.fake))));
}

void test_score_and_pick(const PackageBytes& package) {
    ScoreRig rig;
    prepare_score(rig, package);
    rig.open(package);
    auto& player = rig.fake.world.game.players[1];
    player.energy_produced = 200.0F;
    rig.fake.strength[ARMSOLAR][0] = 80;
    rig.fake.strength[ARMSOLAR][1] = 10;
    rig.fake.strength[ARMSOLAR][2] = 30;
    expect_score(rig, ARMSOLAR, 80, false, 0, false, 0);
    player.energy_produced = 49.0F;
    expect_score(rig, ARMSOLAR, 30, false, 0, false, 0);
    player.energy_produced = 199.0F;
    expect_score(rig, ARMSOLAR, (30 * 10 + 80 * 90) / 100, false, 0, false, 0);
    player.energy_produced = 200.0F;
    player.metal_produced = 2.0F;
    expect_score(rig, ARMSOLAR, 10, false, 0, false, 0);
    player.metal_produced = 4.0F;
    expect_score(rig, ARMSOLAR, (10 * 20 + 80 * 80) / 100, false, 0, false, 0);
    player.metal_produced = 5.0F;
    player.metal = 30.0F;
    player.metal_storage = 500.0F;
    player.metal_produced = 0.0F;
    expect_score(rig, ARMSOLAR, 10, false, 0, false, 0);
    player.metal = 500.0F;
    player.metal_produced = 5.0F;
    player.energy = 600.0F;
    expect_score(rig, ARMSOLAR, (30 * 50 + 80 * 50) / 100, false, 0, false, 0);
    player.energy = 40.0F;
    expect_score(rig, ARMSOLAR, 0, false, 0, false, 0);
    player.energy = 1000.0F;
    expect_score(rig, ARMSOLAR, 0, true, 0, false, 0);
    expect_score(rig, ARMSOLAR, 40, true, oa::sim::ai::unlimited, true, 50);
    rig.fake.spawn(10, 1, ARMCOM, 100, 100, 0);
    expect_pick(rig, 10, ARMCOM, {0, 10, 139, 200}, 0);
    expect_pick(rig, 10, ARMCOM, {0, 0, 0, 0}, 0);
}

void test_lab_idle_without_metal_income(const PackageBytes& package) {
    ScoreRig rig;
    prepare_score(rig, package);
    const uint8_t strengths[][3] = {
        {84, 0, 0},
        {100, 0, 0},
        {100, 99, 0},
        {100, 0, 98},
    };
    const uint16_t types[] = {ARMPW, ARMCK, ARMMEX, ARMSOLAR};
    for (int index = 0; index < 4; ++index)
        std::memcpy(rig.fake.strength[types[index]], strengths[index], 3);
    rig.fake.spawn(10, 1, ARMLAB, 100, 100, OA_UNIT_FLAG_BUILDING);
    rig.open(package);
    auto& player = rig.fake.world.game.players[1];
    player.energy_produced = 200.0F;
    player.metal_produced = 2.9F;
    expect_score(rig, ARMPW, 0, false, 0, false, 0);
    expect_score(rig, ARMCK, 0, false, 0, false, 0);
    expect_score(rig, ARMMEX, 99, false, 0, false, 0);
    expect_pick(rig, 10, ARMLAB, {0, 0}, 0);
    player.energy_produced = 10.0F;
    player.energy_requested = 10.0F;
    expect_score(rig, ARMSOLAR, 98 * 20 / 100, false, 0, false, 0);
    expect_pick(rig, 10, ARMLAB, {0, 0}, 0);
    player.energy_produced = 200.0F;
    player.energy_requested = 0.0F;
    player.metal_produced = 3.0F;
    expect_score(rig, ARMPW, 84 * 80 / 100, false, 0, false, 0);
    player.metal_produced = 5.0F;
    player.metal_requested = 1.0F;
    expect_score(rig, ARMCK, 100, false, 0, false, 0);
    expect_pick(rig, 10, ARMLAB, {0, 150}, ARMPW);
}

void test_campaign_downloadables(const PackageBytes& package) {
    ScoreRig rig;
    rig.fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    describe_catalog(rig.players.state);
    rig.players.state.types[ARMLLT].flags |= OA_UNIT_DEF_FLAG_DOWNLOADABLE;
    CHECK((oa::sim::ai::computer_players_configure(&rig.players.state, "", kBuildLists)));
    copy_catalog(rig.fake, rig.players.state, rig.definitions);
    stamp_ids(rig.fake);
    CHECK((oa::sim::ai::computer_players_initialize(&rig.players.state, host_for(rig.fake))));
    rig.open(package);
    expect_score(rig, ARMLLT, 50, false, 0, false, 0);
    expect_score(rig, ARMSOLAR, 50, false, 0, false, 0);
    rig.seat.reset();
    rig.players.state.downloadables_restricted = 1;
    rig.facts.mode = oa::data::brain_facts::GameMode::campaign;
    rig.open(package);
    expect_score(rig, ARMLLT, 0, false, 0, false, 0);
    expect_score(rig, ARMSOLAR, 50, false, 0, false, 0);
}

void both_players(Twin& twin) {
    for (Fake* fake : {&twin.builtin, &twin.classic_world})
        fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
}

std::vector<uint32_t> filled_rolls(uint32_t value) {
    return std::vector<uint32_t>(4096, value);
}

std::vector<uint32_t> patterned_rolls() {
    const uint32_t pattern[] = {0, 50, 150, 200, 7};
    std::vector<uint32_t> rolls(4096);
    for (std::size_t index = 0; index < rolls.size(); ++index)
        rolls[index] = pattern[index % 5];
    return rolls;
}

void zero_strength(Fake& fake, uint16_t type) {
    fake.strength[type][0] = 0;
    fake.strength[type][1] = 0;
    fake.strength[type][2] = 0;
}

void set_strength(Fake& fake, uint16_t type, uint8_t priority, uint8_t metal, uint8_t energy) {
    fake.strength[type][0] = priority;
    fake.strength[type][1] = metal;
    fake.strength[type][2] = energy;
}

void paint_metal(int32_t x, int32_t z, uint8_t metal) {
    feature_at[z][x] = true;
    for (int32_t row = 0; row < 2; ++row)
        for (int32_t column = 0; column < 2; ++column)
            metal_at[z + row][x + column] = metal;
}

void test_busy_factory_gets_no_pick(const PackageBytes& package) {
    for (const bool skip : {false, true}) {
        Twin twin;
        arm_hosts(twin);
        both_players(twin);
        fill_types(twin);
        copy_both(twin);
        twin.rules.ai.factory_tick_filter.enabled = true;
        twin.rules.ai.factory_tick_filter.skip_busy_background_queue = skip;
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING);
            fake->spawn(12, 1, ARMLAB, 300, 200, OA_UNIT_FLAG_BUILDING);
            fake->holds_secondary[12] = true;
            fake->rolls = filled_rolls(7);
            stamp_ids(*fake);
        }
        use_map(twin, 64, 64, 0);
        use_ground(twin);
        open_playing(twin, package, false);
        play_for(twin, 120);
        CHECK(orders_of(twin.builtin, "factory", 11) != 0);
        CHECK((orders_of(twin.builtin, "factory", 12) != 0) == !skip);
        CHECK(
            orders_of(twin.classic_world, "factory", 11) == orders_of(twin.builtin, "factory", 11)
        );
        CHECK(
            orders_of(twin.classic_world, "factory", 12) == orders_of(twin.builtin, "factory", 12)
        );
    }
}

void test_squad5_factory_tick(const PackageBytes& package) {
    for (const bool on : {false, true}) {
        Twin twin;
        arm_hosts(twin);
        both_players(twin);
        fill_types(twin);
        copy_both(twin);
        twin.rules.ai.squad5_factory_tick.enabled = on;
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
            fake->rolls = filled_rolls(7);
            stamp_ids(*fake);
        }
        use_map(twin, 64, 64, 0);
        use_ground(twin);
        open_playing(twin, package, false);
        play_for(twin, 120);
        CHECK(twin.builtin.units[11].squad == static_cast<int32_t>(Squad::armed_structures));
        CHECK(twin.classic_world.units[11].squad == twin.builtin.units[11].squad);
        CHECK((orders_of(twin.builtin, "factory", 11) != 0) == on);
        CHECK(
            orders_of(twin.classic_world, "factory", 11) == orders_of(twin.builtin, "factory", 11)
        );
    }
}

void test_factory_tick_filter(const PackageBytes& package) {
    using Toggle = oa::data::match_rules::AiFactoryTickFilterPowerToggle;

    struct Building {
        uint16_t type;
        int8_t makes_metal;
        float energy_use;
        bool toggles_by_makes_metal;
        bool toggles_by_energy_use;
    };

    const Building buildings[] = {
        {ARMSOLAR, 1, 0.0F, true, false},
        {ARMMEX, 0, 32.0F, false, true},
        {ARMLLT, 0, 31.99F, false, false},
        {CORMEX, 0, -40.0F, false, false},
    };
    for (const auto toggle : {Toggle::makes_metal, Toggle::energy_use_32}) {
        Twin twin;
        arm_hosts(twin);
        both_players(twin);
        fill_types(twin);
        for (const auto& building : buildings) {
            twin.builtin_players.state.types[building.type].makes_metal = building.makes_metal;
            twin.classic_players.state.types[building.type].makes_metal = building.makes_metal;
            twin.builtin_players.state.types[building.type].energy_use = building.energy_use;
            twin.classic_players.state.types[building.type].energy_use = building.energy_use;
        }
        copy_both(twin);
        twin.rules.ai.factory_tick_filter.enabled = true;
        twin.rules.ai.factory_tick_filter.power_toggle = toggle;
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            uint16_t slot = 11;
            for (const auto& building : buildings) {
                fake->spawn(slot, 1, building.type, 200 + slot * 20, 200, OA_UNIT_FLAG_BUILDING);
                ++slot;
            }
            fake->rolls = filled_rolls(7);
            stamp_ids(*fake);
        }
        use_map(twin, 64, 64, 0);
        use_ground(twin);
        open_playing(twin, package, false);
        play_for(twin, 60);
        uint16_t slot = 11;
        for (const auto& building : buildings) {
            const bool expected = toggle == Toggle::makes_metal ? building.toggles_by_makes_metal
                                                                : building.toggles_by_energy_use;
            CHECK((orders_of(twin.builtin, "off", slot) != 0) == expected);
            CHECK(
                orders_of(twin.classic_world, "off", slot) == orders_of(twin.builtin, "off", slot)
            );
            ++slot;
        }
    }
}

void test_builder_withhold_threshold(const PackageBytes& package) {
    for (const int32_t threshold : {5, 10}) {
        Twin twin;
        arm_hosts(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
            fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        }
        fill_types(twin);
        copy_both(twin);
        twin.rules.ai.builder_withhold_threshold.enabled = threshold != 5;
        twin.rules.ai.builder_withhold_threshold.builders = threshold;
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            zero_strength(*fake, CORMEX);
            fake->spawn(10, 1, ARMCOM, 200, 200, 0);
            for (uint32_t slot = 11; slot < 17; ++slot)
                fake->spawn(slot, 1, ARMCK, 260 + static_cast<int32_t>(slot), 260, 0);
            fake->rolls = filled_rolls(7);
            stamp_ids(*fake);
        }
        use_map(twin, 64, 64, 0);
        use_ground(twin);
        open_playing(twin, package, false);
        play_for(twin, 200);
        CHECK(twin.builtin_players.state.players[1].knowledge.builder_count == 7);
        CHECK(twin.classic_players.state.players[1].knowledge.builder_count == 7);
        CHECK((orders_of(twin.builtin, "build", 10) != 0) == (threshold == 10));
        CHECK(orders_of(twin.builtin, "move", 10) != 0);
        CHECK(orders_of(twin.classic_world, "build", 10) == orders_of(twin.builtin, "build", 10));
        CHECK(orders_of(twin.classic_world, "move", 10) == orders_of(twin.builtin, "move", 10));
    }
}

void test_extractor_metal_twins(const PackageBytes& package) {
    for (const int32_t surface : {0, 100, 255}) {
        Twin twin;
        arm_hosts(twin);
        both_players(twin);
        fill_types(twin);
        twin.builtin_players.state.types[ARMMEX].extracts_metal = 0.001F;
        twin.classic_players.state.types[ARMMEX].extracts_metal = 0.001F;
        copy_both(twin);
        publish_rules(twin);
        clear_ground();
        paint_metal(12, 11, 0);
        paint_metal(17, 12, 10);
        paint_metal(12, 17, 10);
        paint_metal(12, 40, 50);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            zero_strength(*fake, ARMSOLAR);
            zero_strength(*fake, ARMLAB);
            zero_strength(*fake, CORMEX);
            fake->spawn(10, 1, ARMCOM, 200, 200, 0);
            fake->rolls = patterned_rolls();
            stamp_ids(*fake);
        }
        use_map(twin, 64, 64, surface);
        use_ground(twin);
        open_playing(twin, package, false);
        play_for(twin, 600);
        CHECK(orders_arg(twin.builtin, "build", ARMMEX) != 0);
        CHECK(
            orders_arg(twin.classic_world, "build", ARMMEX) ==
            orders_arg(twin.builtin, "build", ARMMEX)
        );
    }
}

void test_grid_half_map_twin(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    both_players(twin);
    fill_types(twin);
    copy_both(twin);
    publish_rules(twin);
    clear_ground();
    site_max_x = 30;
    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        zero_strength(*fake, CORMEX);
        fake->spawn(10, 1, ARMCOM, 200, 200, 0);
        fake->rolls = filled_rolls(7);
        stamp_ids(*fake);
    }
    use_map(twin, 64, 64, 0);
    use_ground(twin);
    open_playing(twin, package, false);
    play_for(twin, 600);
    CHECK(orders_of(twin.builtin, "build", 10) != 0);
    CHECK(orders_of(twin.classic_world, "build", 10) == orders_of(twin.builtin, "build", 10));
}

void test_commander_reach_twin(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    both_players(twin);
    fill_types(twin);
    copy_both(twin);
    publish_rules(twin);
    clear_ground();
    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        zero_strength(*fake, CORMEX);
        fake->spawn(10, 1, ARMCOM, 200, 200, 0);
        fake->rolls = filled_rolls(7);
        stamp_ids(*fake);
    }
    use_map(twin, 2, 2, 0);
    use_ground(twin);
    open_playing(twin, package, false);
    play_for(twin, 600);
    CHECK(orders_of(twin.builtin, "build", 10) == 0);
    CHECK(orders_of(twin.classic_world, "build", 10) == 0);
}

void apply_economy(Fake& fake, uint32_t turn) {
    auto& player = fake.world.game.players[1];
    player.energy = 1000.0F;
    player.metal = 500.0F;
    player.energy_storage = 1000.0F;
    player.metal_storage = 500.0F;
    player.energy_requested = 0.0F;
    player.metal_requested = 0.0F;
    player.energy_produced = 200.0F;
    player.metal_produced = 5.0F;
    switch (turn % 11) {
    case 0:
        player.energy_produced = 0.0F;
        break;
    case 1:
        player.energy_produced = 49.0F;
        break;
    case 2:
        player.energy_produced = 50.0F;
        break;
    case 3:
        player.energy_produced = 199.0F;
        break;
    case 4:
        player.energy_produced = 200.0F;
        break;
    case 5:
        player.metal_produced = 0.0F;
        break;
    case 6:
        player.metal_produced = 2.0F;
        break;
    case 7:
        player.metal_produced = 3.0F;
        break;
    case 8:
        player.metal_produced = 4.0F;
        break;
    case 9:
        player.metal_produced = 5.0F;
        break;
    default:
        player.energy = 600.0F;
        player.metal = 30.0F;
        player.energy_produced = 10.0F;
        player.energy_requested = 10.0F;
        break;
    }
    if (turn % 50 == 0)
        player.energy = 40.0F;
}

void test_economy_sweep_twin(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    both_players(twin);
    fill_types(twin);
    copy_both(twin);
    publish_rules(twin);
    clear_ground();
    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        set_strength(*fake, ARMSOLAR, 10, 10, 80);
        set_strength(*fake, ARMMEX, 10, 80, 10);
        set_strength(*fake, ARMLAB, 80, 10, 10);
        zero_strength(*fake, CORMEX);
        set_strength(*fake, ARMPW, 80, 0, 10);
        set_strength(*fake, ARMCK, 10, 0, 80);
        fake->spawn(10, 1, ARMCOM, 200, 200, 0);
        fake->spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING);
        fake->rolls = filled_rolls(7);
        stamp_ids(*fake);
    }
    use_map(twin, 64, 64, 0);
    use_ground(twin);
    open_playing(twin, package, false);
    {
        SyncGuard guard(twin);
        for (uint32_t turn = 1; turn <= 600; ++turn) {
            apply_economy(twin.builtin, turn);
            apply_economy(twin.classic_world, turn);
            step(twin, turn);
        }
    }
    CHECK(orders_named(twin.builtin, "factory") != 0);
    CHECK(orders_named(twin.builtin, "build") != 0);
    CHECK(orders_named(twin.classic_world, "factory") == orders_named(twin.builtin, "factory"));
    CHECK(orders_named(twin.classic_world, "build") == orders_named(twin.builtin, "build"));
}

void test_capturer_hold_twin(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    both_players(twin);
    fill_types(twin);
    copy_both(twin);
    publish_rules(twin);
    clear_ground();
    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        zero_strength(*fake, CORMEX);
        fake->spawn(10, 1, ARMCOM, 200, 200, 0);
        fake->rolls = filled_rolls(7);
        stamp_ids(*fake);
    }
    use_map(twin, 64, 64, 0);
    use_ground(twin);
    open_playing(twin, package, true);
    uint32_t before_hold = 0;
    uint32_t added_at_361 = 0;
    uint32_t added_at_451 = 0;
    {
        SyncGuard guard(twin);
        for (uint32_t turn = 1; turn <= 600; ++turn) {
            if (turn == 300) {
                twin.builtin_players.state.players[1].commander_build_tick = 450;
                builds_held[&twin.classic_world] = 450;
                before_hold = orders_of(twin.builtin, "build", 10);
            }
            const uint32_t before = orders_of(twin.builtin, "build", 10);
            step(twin, turn);
            const uint32_t added = orders_of(twin.builtin, "build", 10) - before;
            if (turn == 361)
                added_at_361 = added;
            if (turn == 451)
                added_at_451 = added;
        }
    }
    CHECK(before_hold != 0);
    CHECK(added_at_361 == 0);
    CHECK(added_at_451 != 0);
    CHECK(orders_of(twin.builtin, "move", 10) == 0);
    CHECK(orders_of(twin.classic_world, "build", 10) == orders_of(twin.builtin, "build", 10));
    CHECK(orders_of(twin.classic_world, "move", 10) == 0);
}

void reference_heap(const int64_t* distances, int count, int32_t* order, int* got) {
    *got = count;
    if (count <= 0)
        return;
    std::vector<oa::base::game_math::FloatHeapPair> heap(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        auto& pair = heap[static_cast<std::size_t>(index)];
        pair.value = static_cast<uint32_t>(index + 1);
        pair.key = static_cast<float>(-static_cast<double>(distances[index]));
    }
    if (count >= 2)
        oa::base::game_math::float_heap_make(heap.data(), count);
    int32_t size = count;
    int written = 0;
    while (size > 0) {
        order[written] = static_cast<int32_t>(heap[0].value);
        ++written;
        oa::base::game_math::float_heap_pop(heap.data(), size);
        --size;
    }
    *got = written;
}

void test_float_heap_order(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    twin.classic_world.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    stamp_ids(twin.classic_world);
    twin.seat = open_classic(package, twin.classic_host, twin.facts, twin.definitions, nullptr);
    uint32_t rng = 0xC04C04u;
    const auto next = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    for (int list = 0; list < 1000; ++list) {
        LuaCall ask;
        if (list == 0) {
            ask.count = 0;
        } else if (list == 1) {
            ask.count = 1;
            ask.distances[0] = 0;
        } else if (list == 2) {
            ask.count = 8;
            for (int index = 0; index < 8; ++index)
                ask.distances[index] = 25;
        } else if (list == 3) {
            const int64_t special[] = {
                INT64_MIN,
                INT64_MAX,
                static_cast<int64_t>(1) << 53,
                -(static_cast<int64_t>(1) << 53),
                0,
                7,
                7,
                -1
            };
            ask.count = 8;
            for (int index = 0; index < 8; ++index)
                ask.distances[index] = special[index];
        } else {
            ask.count = static_cast<int>(2 + (next() % 20));
            int64_t last = 1;
            for (int index = 0; index < ask.count; ++index) {
                const uint32_t roll = next();
                if (roll % 4 == 0)
                    ask.distances[index] = static_cast<int64_t>(roll) << 20;
                else if (roll % 3 == 0)
                    ask.distances[index] = last;
                else
                    ask.distances[index] = static_cast<int64_t>(roll % 7) - 3;
                if ((roll & 1u) != 0)
                    ask.distances[index] = -ask.distances[index];
                last = ask.distances[index];
            }
        }
        int32_t expect[64]{};
        int expect_count = 0;
        reference_heap(ask.distances, ask.count, expect, &expect_count);
        run_lua(*twin.seat, ask, call_heap_work);
        CHECK(ask.got == expect_count);
        for (int index = 0; index < expect_count; ++index)
            CHECK(ask.order[index] == expect[index]);
    }
}

struct WeightAsk {
    int32_t x{};
    int32_t z{};
    int32_t radius{};
    bool ok{};
    lua_Integer weight{};
    char message[256]{};
};

struct WeightWrite {
    lua_Integer key{};
    lua_Integer value{};
};

void write_weight_work(void* context, lua_State* state) {
    auto* write = static_cast<const WeightWrite*>(context);
    if (write == nullptr || lua_type(state, -1) != LUA_TTABLE)
        return;
    lua_getfield(state, -1, "base_weights");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return;
    }
    lua_pushinteger(state, write->value);
    lua_rawseti(state, -2, write->key);
    lua_pop(state, 1);
}

void call_weight_work(void* context, lua_State* state) {
    auto* ask = static_cast<WeightAsk*>(context);
    if (ask == nullptr || lua_type(state, -1) != LUA_TTABLE)
        return;
    const int seat_index = lua_gettop(state);
    lua_getglobal(state, "require");
    lua_pushliteral(state, "squads");
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        copy_message(ask->message, sizeof ask->message, lua_tostring(state, -1));
        return;
    }
    lua_getfield(state, -1, "sighted_weight");
    lua_pushvalue(state, seat_index);
    lua_newtable(state);
    lua_pushinteger(state, static_cast<lua_Integer>(ask->x));
    lua_setfield(state, -2, "x");
    lua_pushinteger(state, 0);
    lua_setfield(state, -2, "y");
    lua_pushinteger(state, static_cast<lua_Integer>(ask->z));
    lua_setfield(state, -2, "z");
    lua_pushinteger(state, static_cast<lua_Integer>(ask->radius));
    if (lua_pcall(state, 3, 1, 0) != LUA_OK) {
        copy_message(ask->message, sizeof ask->message, lua_tostring(state, -1));
        return;
    }
    if (lua_isinteger(state, -1) == 0) {
        copy_message(ask->message, sizeof ask->message, "sighted weight was not an integer");
        return;
    }
    ask->ok = true;
    ask->weight = lua_tointeger(state, -1);
}

void run_seat(oa::sim::brains::ToolSeat& seat, void* context, void (*work)(void*, lua_State*)) {
    oa::data::script_host::ScriptError error;
    if (!seat.with_seat(context, work, error))
        throw std::runtime_error(oa::data::script_host::describe(error));
}

struct WeightView {
    std::array<uint16_t, 6> seen{40, 41, 42, 43, 44, 63};
    oa::sim::detection::Sightings sightings{};
};

WeightView weight_view;

const oa::sim::detection::Sightings* weight_sightings(void*, uint8_t player) {
    return player == 1 ? &weight_view.sightings : nullptr;
}

void test_sighted_weight(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    twin.builtin.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    twin.classic_world.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    weight_view = {};
    weight_view.seen = {40, 41, 42, 43, 44, 63};
    weight_view.sightings.seen = weight_view.seen.data();
    weight_view.sightings.capacity = static_cast<uint32_t>(weight_view.seen.size());
    weight_view.sightings.seen_count = static_cast<uint32_t>(weight_view.seen.size());
    twin.classic_host.sightings = weight_sightings;
    fill_types(twin);
    copy_both(twin);
    stamp_ids(twin.builtin);
    stamp_ids(twin.classic_world);
    seat_pair(twin, package);
    auto* knowledge = oa::sim::ai::computer_player_knowledge(&twin.classic_players.state, 1);
    CHECK(knowledge != nullptr);
    CHECK(knowledge->base_weights != nullptr);
    CHECK(knowledge->base_weights[ARMLLT] == 40);
    knowledge->base_weights[ARMPW] = -5;
    WeightWrite write;
    write.key = ARMPW;
    write.value = -5;
    run_seat(*twin.seat, &write, write_weight_work);
    constexpr int32_t x = 500;
    constexpr int32_t z = 700;
    const oa::FixedVec3 at{x << 16, 0, z << 16};
    auto& world = twin.classic_world;
    world.spawn(40, 0, ARMLLT, x + 96, z + 128, OA_UNIT_FLAG_BUILDING);
    world.spawn(41, 0, ARMLLT, x + 161, z, OA_UNIT_FLAG_BUILDING);
    world.spawn(42, 0, ARMPW, x, z - 10, 0).flags = 0;
    world.spawn(43, 0, ARMLLT, x - 160, z, OA_UNIT_FLAG_BUILDING);
    world.units[43].position.x -= 200;
    world.spawn(44, 0, ARMLLT, x, z, OA_UNIT_FLAG_BUILDING);
    world.units[44].type_index = static_cast<uint16_t>(oa::test::computer_fake::TYPE_COUNT + 3);
    world.spawn(45, 0, ARMSOLAR, x + 20, z + 20, OA_UNIT_FLAG_BUILDING);
    const int32_t expect_160 = oa::sim::ai::computer_sighted_weight(
        &twin.classic_players.state, *knowledge, weight_view.sightings, world.world, at, 160
    );
    const int32_t expect_159 = oa::sim::ai::computer_sighted_weight(
        &twin.classic_players.state, *knowledge, weight_view.sightings, world.world, at, 159
    );
    const int32_t expect_161 = oa::sim::ai::computer_sighted_weight(
        &twin.classic_players.state, *knowledge, weight_view.sightings, world.world, at, 161
    );
    CHECK(expect_160 == 75);
    CHECK(expect_159 == -5);
    CHECK(expect_161 == 115);
    auto lua_weight = [&](int32_t radius) {
        WeightAsk ask;
        ask.x = at.x;
        ask.z = at.z;
        ask.radius = radius;
        run_seat(*twin.seat, &ask, call_weight_work);
        if (!ask.ok)
            std::fprintf(stderr, "sighted weight: %s\n", ask.message);
        CHECK(ask.ok);
        return ask.weight;
    };
    CHECK(lua_weight(160) == 75);
    CHECK(lua_weight(159) == -5);
    CHECK(lua_weight(161) == 115);
    weight_view.sightings.seen_count = 0;
    CHECK(
        oa::sim::ai::computer_sighted_weight(
            &twin.classic_players.state, *knowledge, weight_view.sightings, world.world, at, 160
        ) == 0
    );
    CHECK(lua_weight(160) == 0);
}

struct SiegeView {
    std::array<uint16_t, 4> seen{};
    oa::sim::detection::Sightings sightings{};
    bool reach[64]{};
    int32_t visible_below_z = 0;
};

SiegeView siege_view;

bool siege_point_visible(void*, uint8_t player, const oa::FixedVec3* at) {
    return player == 1 && at != nullptr && (at->z >> 16) < siege_view.visible_below_z;
}

const oa::sim::detection::Sightings* siege_sightings(void*, uint8_t player) {
    return player == 1 ? &siege_view.sightings : nullptr;
}

bool siege_weapon_reaches(void*, uint16_t unit, const oa::FixedVec3*) {
    return unit < 64 && siege_view.reach[unit];
}

bool siege_order_attack_point(void* context, uint16_t unit, const oa::FixedVec3* at) {
    if (at == nullptr)
        return false;
    oa::test::computer_fake::fake_of(context)->orders.push_back(
        {"attack point", unit, 0, *at, false}
    );
    return true;
}

void install_siege(ComputerHost& host) {
    host.point_visible = siege_point_visible;
    host.sightings = siege_sightings;
    host.weapon_reaches = siege_weapon_reaches;
    host.order_attack_point = siege_order_attack_point;
}

void quiet_siege_work(void*, lua_State* state) {
    if (lua_type(state, -1) != LUA_TTABLE)
        return;
    lua_pushinteger(state, 1000000);
    lua_setfield(state, -2, "sort_countdown");
    lua_getfield(state, -1, "tasks");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return;
    }
    for (int task = 0; task <= 8; ++task) {
        lua_rawgeti(state, -1, task);
        if (lua_type(state, -1) == LUA_TTABLE) {
            lua_pushinteger(state, 1000000);
            lua_setfield(state, -2, "next");
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

void quiet_but_siege(Twin& twin) {
    auto& ai = twin.builtin_players.state.players[1];
    ai.sort_countdown = 1000000;
    for (auto& task : ai.tasks) {
        if (task.kind != oa::sim::ai::TaskKind::siege)
            task.next_tick = 1000000;
    }
    run_seat(*twin.seat, nullptr, quiet_siege_work);
}

void script_rolls(Twin& twin, const uint32_t* values, std::size_t count) {
    twin.builtin.rolls.assign(values, values + count);
    twin.classic_world.rolls.assign(values, values + count);
    twin.builtin.roll = 0;
    twin.classic_world.roll = 0;
    twin.builtin.orders.clear();
    twin.classic_world.orders.clear();
}

bool same_point(const oa::FixedVec3& point, int32_t x, int32_t y, int32_t z) {
    return point.x == x && point.y == y && point.z == z;
}

void expect_attack_points(
    const Fake& fake, const uint16_t* units, std::size_t count, int32_t x, int32_t z
) {
    CHECK(fake.orders.size() == count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& order = fake.orders[index];
        CHECK(order.kind == "attack point");
        CHECK(order.unit == units[index]);
        CHECK(same_point(order.at, x, 0, z));
    }
}

void add_siege_member(
    Fake& fake,
    uint16_t slot,
    uint16_t type,
    int32_t x,
    int32_t z,
    uint32_t flags,
    uint32_t movement
) {
    auto& unit = fake.spawn(slot, 1, type, x, z, flags);
    unit.movement = movement;
    unit.def = oa::world_unit_def_ref(&fake.world, &fake.defs[type]);
    fake.set_squad(slot, static_cast<int>(Squad::siege));
}

void test_siege(const PackageBytes& package) {
    Twin twin;
    arm_hosts(twin);
    siege_view = {};
    install_siege(twin.builtin_host);
    install_siege(twin.classic_host);
    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        fake->world.game.map_width_world = 1024;
        fake->world.game.map_height_world = 512;
        fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
        fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        stamp_ids(*fake);
    }
    fill_types(twin);
    copy_both(twin);
    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        fake->defs[ARMPW].abilities |= OA_UNIT_DEF_ABILITY_CAN_ATTACK;
        fake->defs[ARMLLT].abilities |= OA_UNIT_DEF_ABILITY_CAN_ATTACK;
    }
    open_playing(twin, package, false);
    const int32_t centre_x = 512 << 16;
    const int32_t centre_z = 256 << 16;
    auto& task = twin.builtin_players.state.players[1].tasks[static_cast<uint32_t>(Squad::siege)];
    CHECK(task.kind == oa::sim::ai::TaskKind::siege);
    CHECK(same_point(task.siege_target, centre_x, 0, centre_z));
    CHECK(same_point(task.siege_probe, centre_x, 0, centre_z));
    CHECK(same_point(task.siege_step, centre_x, 0, centre_z));
    CHECK(task.siege_weight == 0);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_target.x"), centre_x);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_target.z"), centre_z);
    quiet_but_siege(twin);
    twin.check_squads = true;
    SyncGuard guard(twin);
    const uint32_t empty_rolls[] = {7};
    script_rolls(twin, empty_rolls, 1);
    step(twin, 100);
    CHECK(twin.builtin.roll == 1u);
    CHECK(twin.classic_world.roll == 1u);
    CHECK(task.next_tick == 100u + 7u + 30u);
    expect_whole(read_path(*twin.seat, "tasks[9].next"), static_cast<lua_Integer>(task.next_tick));
    CHECK(same_point(task.siege_probe, centre_x, 0, centre_z));
    expect_whole(read_path(*twin.seat, "tasks[9].siege_probe.x"), centre_x);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_probe.z"), centre_z);
    CHECK(twin.builtin.orders.empty());
    CHECK(twin.classic_world.orders.empty());

    for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
        add_siege_member(*fake, 20, ARMPW, 100, 100, OA_UNIT_FLAG_HAS_WEAPONS, 1);
        add_siege_member(
            *fake, 21, ARMLLT, 120, 100, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS, 0
        );
        add_siege_member(
            *fake, 22, ARMLLT, 140, 100, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS, 0
        );
        add_siege_member(*fake, 23, ARMCK, 160, 100, 0, 1);
        add_siege_member(*fake, 24, ARMPW, 180, 100, OA_UNIT_FLAG_HAS_WEAPONS, 1);
        fake->spawn(2, 0, ARMLLT, 512 + 96, 576 + 128, OA_UNIT_FLAG_BUILDING);
        fake->spawn(3, 0, ARMLLT, 512, 576 - 30, OA_UNIT_FLAG_BUILDING);
        fake->spawn(4, 0, ARMLLT, 512 + 170, 576, OA_UNIT_FLAG_BUILDING);
    }
    siege_view.reach[22] = true;
    siege_view.seen = {2, 3, 4, 0};
    siege_view.sightings.seen = siege_view.seen.data();
    siege_view.sightings.capacity = 4;
    siege_view.sightings.seen_count = 3;
    siege_view.visible_below_z = 800;
    const uint32_t turn_rolls[] = {9, 0, 0x8000u, 12};
    script_rolls(twin, turn_rolls, 4);
    const uint32_t armed = task.next_tick;
    step(twin, armed);
    const oa::FixedVec3 turned{
        -oa::sim::unit_movement::sine_scaled(0x8000, 0x1400000),
        0,
        -oa::sim::unit_movement::cosine_scaled(0x8000, 0x1400000)
    };
    CHECK(turned.x == 0 && turned.z == 0x1400000);
    const int32_t first_z = 576 << 16;
    CHECK(twin.builtin.roll == 4u);
    CHECK(twin.classic_world.roll == 4u);
    CHECK(same_point(task.siege_step, turned.x, 0, turned.z));
    CHECK(same_point(task.siege_probe, centre_x, 0, first_z));
    CHECK(same_point(task.siege_target, centre_x, 0, first_z));
    CHECK(task.siege_weight == 80);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_step.x"), turned.x);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_step.z"), turned.z);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_probe.z"), first_z);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_target.z"), first_z);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_weight"), 80);
    const uint16_t attackers[] = {20, 22, 24};
    expect_attack_points(twin.builtin, attackers, 3, centre_x, first_z);
    expect_attack_points(twin.classic_world, attackers, 3, centre_x, first_z);

    const uint32_t hold_rolls[] = {1, 3};
    script_rolls(twin, hold_rolls, 2);
    const uint32_t unseen = task.next_tick;
    step(twin, unseen);
    const int32_t second_z = 896 << 16;
    CHECK(twin.builtin.roll == 2u);
    CHECK(twin.classic_world.roll == 2u);
    CHECK(same_point(task.siege_probe, centre_x, 0, second_z));
    expect_whole(read_path(*twin.seat, "tasks[9].siege_probe.z"), second_z);
    CHECK(same_point(task.siege_target, centre_x, 0, first_z));
    CHECK(task.siege_weight == 80);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_weight"), 80);
    CHECK(twin.builtin.orders.size() == 3u);
    CHECK(twin.classic_world.orders.size() == 3u);

    siege_view.visible_below_z = 2000;
    siege_view.sightings.seen_count = 1;
    const uint32_t poor_rolls[] = {1, 3, 5};
    script_rolls(twin, poor_rolls, 3);
    step(twin, task.next_tick);
    CHECK(twin.builtin.roll == 3u);
    CHECK(twin.classic_world.roll == 3u);
    CHECK(same_point(task.siege_target, centre_x, 0, first_z));
    CHECK(task.siege_weight == 80);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_target.z"), first_z);
    expect_whole(read_path(*twin.seat, "tasks[9].siege_weight"), 80);
}

void test_attack_wave_size(const PackageBytes& package) {
    for (const int32_t wave : {6, 10}) {
        Twin twin;
        arm_hosts(twin);
        twin.rules.ai.attack_wave_size.enabled = wave != 6;
        twin.rules.ai.attack_wave_size.units = wave;
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
            fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
            fake->spawn(1, 0, ARMLLT, 900, 900, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
            fake->spawn(10, 1, ARMSOLAR, 200, 200, OA_UNIT_FLAG_BUILDING);
            for (uint32_t slot = 12; slot < 19; ++slot) {
                fake->spawn(
                    slot, 1, ARMPW, 300 + static_cast<int32_t>(slot), 300, OA_UNIT_FLAG_HAS_WEAPONS
                );
            }
            fake->rolls = filled_rolls(7);
            stamp_ids(*fake);
        }
        fill_types(twin);
        copy_both(twin);
        open_playing(twin, package, false);
        expect_whole(read_path(*twin.seat, "tasks[2].launch"), wave);
        expect_whole(read_path(*twin.seat, "tasks[6].launch"), wave);
        expect_whole(read_path(*twin.seat, "tasks[2].min_size"), 3);
        expect_whole(read_path(*twin.seat, "tasks[6].min_size"), 3);
        const auto& tasks = twin.builtin_players.state.players[1].tasks;
        CHECK(tasks[2].launch_size == wave && tasks[6].launch_size == wave);
        CHECK(tasks[2].min_size == 3 && tasks[6].min_size == 3);
        twin.check_squads = true;
        {
            SyncGuard guard(twin);
            for (uint32_t turn = 1; turn <= 301; ++turn)
                step(twin, turn);
        }
        CHECK((twin.builtin.squads[{1, 2}].size() == 7u));
        CHECK((twin.classic_world.squads[{1, 2}].size() == 7u));
        const bool expect_attack = wave == 6;
        CHECK((orders_of(twin.builtin, "attack", 12) != 0) == expect_attack);
        CHECK((orders_of(twin.classic_world, "attack", 12) != 0) == expect_attack);
    }
}

bool unqueued_patrol(const Fake& fake, uint16_t unit) {
    for (const auto& order : fake.orders) {
        if (order.kind == "patrol" && order.unit == unit && !order.queue)
            return true;
    }
    return false;
}

void set_base(Twin& twin, int32_t x, int32_t z) {
    const oa::FixedVec3 base{x << 16, 0, z << 16};
    twin.builtin_players.state.players[1].knowledge.base_position = base;
    twin.classic_players.state.players[1].knowledge.base_position = base;
}

void test_patrol_group_size(const PackageBytes& package) {
    for (const int32_t size : {5, 15}) {
        Twin twin;
        arm_hosts(twin);
        twin.rules.ai.patrol_group_size.enabled = size != 5;
        twin.rules.ai.patrol_group_size.units = size;
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
            fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
            fake->spawn(1, 0, ARMLLT, 500, 600, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
            for (uint32_t wing = 0; wing < 6; ++wing) {
                const auto slot = static_cast<uint16_t>(11 + wing);
                fake->spawn(slot, 1, ARMFIG, 300 + static_cast<int32_t>(wing), 300, 0);
                fake->set_squad(slot, static_cast<int>(Squad::aircraft));
            }
            fake->rolls = filled_rolls(3);
            stamp_ids(*fake);
        }
        fill_types(twin);
        copy_both(twin);
        open_playing(twin, package, false);
        set_base(twin, 200, 200);
        twin.builtin.roll = 0;
        twin.classic_world.roll = 0;
        twin.check_squads = true;
        {
            SyncGuard guard(twin);
            step(twin, 1);
        }
        const bool expect_scout = size == 15;
        CHECK((orders_of(twin.builtin, "move", 11) != 0) == expect_scout);
        CHECK((orders_of(twin.classic_world, "move", 11) != 0) == expect_scout);
        CHECK(unqueued_patrol(twin.builtin, 11) == !expect_scout);
        CHECK(unqueued_patrol(twin.classic_world, 11) == !expect_scout);
    }
}

void test_patrol_null_enemy_skip(const PackageBytes& package) {
    for (const bool on : {false, true}) {
        Twin twin;
        arm_hosts(twin);
        twin.rules.ai.patrol_null_enemy_skip.enabled = on;
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
            fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
            fake->spawn(1, 0, ARMLLT, 500, 600, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
            for (uint32_t wing = 0; wing < 2; ++wing) {
                const auto slot = static_cast<uint16_t>(11 + wing);
                fake->spawn(slot, 1, ARMFIG, 300 + static_cast<int32_t>(wing), 300, 0);
                fake->set_squad(slot, static_cast<int>(Squad::aircraft));
            }
            fake->rolls = filled_rolls(3);
            stamp_ids(*fake);
        }
        fill_types(twin);
        copy_both(twin);
        open_playing(twin, package, false);
        twin.builtin.roll = 0;
        twin.classic_world.roll = 0;
        twin.check_squads = true;
        {
            SyncGuard guard(twin);
            step(twin, 1);
        }
        auto queued_enemy = [](const Fake& fake) {
            for (const auto& order : fake.orders) {
                if (order.unit == 11 && order.kind == "patrol" && order.queue &&
                    order.at.x == (500 << 16) && order.at.z == (600 << 16))
                    return true;
            }
            return false;
        };
        auto edge = [](const Fake& fake) {
            for (const auto& order : fake.orders) {
                if (order.unit == 11 && order.kind == "patrol" && !order.queue &&
                    order.at.x == (3 << 16) && order.at.z == 0)
                    return true;
            }
            return false;
        };
        CHECK(queued_enemy(twin.builtin) == !on);
        CHECK(queued_enemy(twin.classic_world) == !on);
        CHECK(edge(twin.builtin) == on);
        CHECK(edge(twin.classic_world) == on);
        const std::size_t draws = on ? 5u : 2u;
        CHECK(twin.builtin.roll == draws);
        CHECK(twin.classic_world.roll == draws);
    }
}

uint16_t attack_target(const Fake& fake, uint16_t unit) {
    uint16_t target = 0;
    for (const auto& order : fake.orders) {
        if (order.kind == "attack" && order.unit == unit)
            target = order.arg;
    }
    return target;
}

void test_nearest_enemy_filter(const PackageBytes& package) {
    using Filter = oa::data::match_rules::AiNearestEnemyFilterRules;
    for (const auto filter : {Filter::base, Filter::skip_submerged}) {
        for (const bool naval : {false, true}) {
            Twin twin;
            arm_hosts(twin);
            twin.rules.ai.nearest_enemy_filter.enabled = filter != Filter::base;
            twin.rules.ai.nearest_enemy_filter.rules = filter;
            publish_rules(twin);
            for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
                fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
                fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
                auto& sub = fake->spawn(1, 0, ARMPT, 400, 300, OA_UNIT_FLAG_HAS_WEAPONS);
                const uint32_t submerged = 3;
                std::memcpy(sub.last_occupy_code, &submerged, sizeof submerged);
                constexpr uint32_t move_rate_tier_2 = 0x8u;
                fake->spawn(2, 0, ARMPW, 500, 300, OA_UNIT_FLAG_HAS_WEAPONS | move_rate_tier_2);
                fake->spawn(
                    3, 0, ARMLLT, 600, 300, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS
                );
                const auto type = naval ? ARMPT : ARMPW;
                for (uint32_t slot = 12; slot < 19; ++slot) {
                    fake->spawn(
                        slot,
                        1,
                        type,
                        300 + static_cast<int32_t>(slot),
                        300,
                        OA_UNIT_FLAG_HAS_WEAPONS
                    );
                }
                fake->rolls = filled_rolls(7);
                stamp_ids(*fake);
            }
            fill_types(twin);
            copy_both(twin);
            open_playing(twin, package, false);
            twin.check_squads = true;
            {
                SyncGuard guard(twin);
                for (uint32_t turn = 1; turn <= 301; ++turn)
                    step(twin, turn);
            }
            const uint16_t expected = filter == Filter::base ? 1 : naval ? 1 : 3;
            CHECK(attack_target(twin.builtin, 12) == expected);
            CHECK(attack_target(twin.classic_world, 12) == expected);
        }
    }
}

void apply_ridge(oa::data::match_rules::MatchRules& match_rules) {
    using oa::data::match_rules::AiDifficultyNamesNames;
    using oa::data::match_rules::AiFactoryTickFilterPowerToggle;
    using oa::data::match_rules::AiNearestEnemyFilterRules;
    using oa::data::match_rules::AiSquadAssignmentRules;
    auto& ai = match_rules.ai;
    ai.difficulty_names.enabled = true;
    ai.difficulty_names.names = {
        AiDifficultyNamesNames::hard,
        AiDifficultyNamesNames::medium,
        AiDifficultyNamesNames::easy,
    };
    ai.squad5_factory_tick.enabled = true;
    ai.factory_tick_filter.enabled = true;
    ai.factory_tick_filter.skip_busy_background_queue = true;
    ai.factory_tick_filter.power_toggle = AiFactoryTickFilterPowerToggle::energy_use_32;
    ai.builder_withhold_threshold.enabled = true;
    ai.builder_withhold_threshold.builders = 10;
    ai.commander_keeps_orders_when_damaged.enabled = true;
    ai.attack_wave_size.enabled = true;
    ai.attack_wave_size.units = 10;
    ai.patrol_group_size.enabled = true;
    ai.patrol_group_size.units = 15;
    ai.patrol_null_enemy_skip.enabled = true;
    ai.squad_assignment.enabled = true;
    ai.squad_assignment.rules = AiSquadAssignmentRules::role_squads;
    ai.nearest_enemy_filter.enabled = true;
    ai.nearest_enemy_filter.rules = AiNearestEnemyFilterRules::skip_submerged;
}

void play_long(Twin& twin, uint32_t until) {
    twin.check_squads = true;
    SyncGuard guard(twin);
    for (uint32_t turn = 1; turn <= until; ++turn)
        step(twin, turn);
    CHECK(!twin.noted.squads);
    CHECK(twin.builtin.orders.size() == twin.classic_world.orders.size());
}

void test_land_army_twin(const PackageBytes& package) {
    for (const bool ridge : {false, true}) {
        Twin twin;
        arm_hosts(twin);
        if (ridge)
            apply_ridge(twin.rules);
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
            fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 50);
            fake->spawn(1, 0, ARMLLT, 9000, 9000, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
            fake->spawn(10, 1, ARMSOLAR, 200, 200, OA_UNIT_FLAG_BUILDING);
            for (uint32_t slot = 0; slot < 40; ++slot) {
                const auto unit = static_cast<uint16_t>(11 + slot);
                int32_t x = 1000;
                int32_t z = 1000;
                if (slot < 8)
                    x = 1000 + static_cast<int32_t>(slot) * 10;
                else if (slot < 16) {
                    x = 1000 + static_cast<int32_t>(slot - 8) * 10;
                    z = 1020;
                } else
                    x = 20000 + static_cast<int32_t>(slot) * 3000;
                fake->spawn(unit, 1, ARMPW, x, z, OA_UNIT_FLAG_HAS_WEAPONS);
                const int squad = slot < 8 || slot >= 16 ? 2 : 3;
                fake->set_squad(unit, squad);
            }
            fake->rolls = filled_rolls(7);
            stamp_ids(*fake);
        }
        fill_types(twin);
        copy_both(twin);
        open_playing(twin, package, false);
        play_long(twin, 1500);
        CHECK((twin.builtin.squads[{1, 2}].size() == 16u));
        CHECK((twin.classic_world.squads[{1, 2}].size() == 16u));
    }
}

void test_navy_submerged_twin(const PackageBytes& package) {
    for (const bool ridge : {false, true}) {
        Twin twin;
        arm_hosts(twin);
        if (ridge)
            apply_ridge(twin.rules);
        publish_rules(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
            fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
            auto& sub = fake->spawn(1, 0, ARMPT, 250, 100, OA_UNIT_FLAG_HAS_WEAPONS);
            const uint32_t submerged = 3;
            std::memcpy(sub.last_occupy_code, &submerged, sizeof submerged);
            fake->spawn(2, 0, ARMPW, 800, 100, OA_UNIT_FLAG_HAS_WEAPONS | 0x8u);
            fake->spawn(3, 0, ARMLLT, 1400, 100, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
            for (uint32_t slot = 0; slot < 9; ++slot) {
                const auto unit = static_cast<uint16_t>(12 + slot);
                const int32_t x = slot < 8 ? 100 + static_cast<int32_t>(slot) * 8 : 8000;
                fake->spawn(unit, 1, ARMPT, x, 100, OA_UNIT_FLAG_HAS_WEAPONS);
                fake->set_squad(unit, static_cast<int>(Squad::navy));
            }
            fake->rolls = filled_rolls(7);
            stamp_ids(*fake);
        }
        fill_types(twin);
        copy_both(twin);
        open_playing(twin, package, false);
        play_long(twin, 1500);
        CHECK((twin.builtin.squads[{1, static_cast<int>(Squad::naval_strike)}].size() == 8u));
        CHECK((twin.classic_world.squads[{1, static_cast<int>(Squad::naval_strike)}].size() == 8u));
        CHECK(attack_target(twin.builtin, 12) == 1u);
        CHECK(attack_target(twin.classic_world, 12) == 1u);
    }
}

void test_air_wing_twin(const PackageBytes& package) {
    for (const bool ridge : {false, true}) {
        for (const bool with_base : {false, true}) {
            for (uint32_t wing = 1; wing <= 16; ++wing) {
                Twin twin;
                arm_hosts(twin);
                if (ridge)
                    apply_ridge(twin.rules);
                publish_rules(twin);
                for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
                    fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
                    fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
                    fake->spawn(
                        1, 0, ARMLLT, 500, 600, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS
                    );
                    if (with_base)
                        fake->spawn(10, 1, ARMSOLAR, 200, 200, OA_UNIT_FLAG_BUILDING);
                    for (uint32_t member = 0; member < wing; ++member) {
                        const auto slot = static_cast<uint16_t>(11 + member);
                        fake->spawn(slot, 1, ARMFIG, 300 + static_cast<int32_t>(member), 300, 0);
                        fake->set_squad(slot, static_cast<int>(Squad::aircraft));
                    }
                    fake->rolls = filled_rolls(3);
                    stamp_ids(*fake);
                }
                fill_types(twin);
                copy_both(twin);
                open_playing(twin, package, false);
                if (with_base)
                    set_base(twin, 200, 200);
                play_long(twin, 1500);
                CHECK(!twin.builtin.orders.empty());
            }
        }
    }
}

void test_siege_seen_twin(const PackageBytes& package) {
    for (const bool ridge : {false, true}) {
        Twin twin;
        arm_hosts(twin);
        if (ridge)
            apply_ridge(twin.rules);
        publish_rules(twin);
        siege_view = {};
        install_siege(twin.builtin_host);
        install_siege(twin.classic_host);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->world.game.map_width_world = 1024;
            fake->world.game.map_height_world = 512;
            fake->player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
            fake->player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
            stamp_ids(*fake);
        }
        fill_types(twin);
        copy_both(twin);
        for (Fake* fake : {&twin.builtin, &twin.classic_world}) {
            fake->defs[ARMPW].abilities |= OA_UNIT_DEF_ABILITY_CAN_ATTACK;
            fake->defs[ARMLLT].abilities |= OA_UNIT_DEF_ABILITY_CAN_ATTACK;
            add_siege_member(*fake, 20, ARMPW, 100, 100, OA_UNIT_FLAG_HAS_WEAPONS, 1);
            add_siege_member(
                *fake, 21, ARMLLT, 120, 100, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS, 0
            );
            add_siege_member(
                *fake, 22, ARMLLT, 140, 100, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS, 0
            );
            add_siege_member(*fake, 23, ARMCK, 160, 100, 0, 1);
            add_siege_member(*fake, 24, ARMPW, 180, 100, OA_UNIT_FLAG_HAS_WEAPONS, 1);
            fake->spawn(2, 0, ARMLLT, 400, 300, OA_UNIT_FLAG_BUILDING);
            fake->spawn(3, 0, ARMLLT, 420, 300, OA_UNIT_FLAG_BUILDING);
            fake->rolls = filled_rolls(7);
        }
        siege_view.reach[22] = true;
        siege_view.seen = {2, 3, 0, 0};
        siege_view.sightings.seen = siege_view.seen.data();
        siege_view.sightings.capacity = 4;
        siege_view.sightings.seen_count = 2;
        siege_view.visible_below_z = 4000;
        open_playing(twin, package, false);
        play_long(twin, 1500);
        CHECK(orders_named(twin.builtin, "attack point") > 0u);
        CHECK(
            orders_named(twin.classic_world, "attack point") ==
            orders_named(twin.builtin, "attack point")
        );
    }
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
        test_sighted_weight(package);
        test_siege(package);
        test_attack_wave_size(package);
        test_patrol_group_size(package);
        test_patrol_null_enemy_skip(package);
        test_nearest_enemy_filter(package);
        test_land_army_twin(package);
        test_navy_submerged_twin(package);
        test_air_wing_twin(package);
        test_siege_seen_twin(package);
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
        test_score_and_pick(package);
        test_lab_idle_without_metal_income(package);
        test_campaign_downloadables(package);
        test_busy_factory_gets_no_pick(package);
        test_squad5_factory_tick(package);
        test_factory_tick_filter(package);
        test_builder_withhold_threshold(package);
        test_extractor_metal_twins(package);
        test_grid_half_map_twin(package);
        test_commander_reach_twin(package);
        test_economy_sweep_twin(package);
        test_capturer_hold_twin(package);
        test_float_heap_order(package);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::printf("sim-brains-classic: ok\n");
    return 0;
}
