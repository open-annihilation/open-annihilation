// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ai.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/match_rules/difficulty_names.hpp"
#include "oa/sim/combat_state.hpp"
#include "oa/sim/unit_movement/movement.hpp"
#include "oa/test/computer_fake.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/platform/files.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace oa::sim::ai;
using namespace oa::test::computer_fake;

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/// Checks the computer players' build lists against the build-list limits: a list holds
/// one entry more than the CANBUILD entries a builder keeps (31 in 3.1c), for 3.1c's
/// copy, a raised copy, the largest copy, and every entry.
void test_build_list_limits() {
    // ARMCOM's list holds 1100 entries, cycling through the catalog's types.
    constexpr uint32_t listed = 1100;
    const uint16_t ids[] = {ARMMEX, ARMSOLAR, ARMLAB, ARMPW, ARMCK};
    std::vector<uint16_t> lists = {0, listed};
    for (uint32_t index = 0; index < listed; ++index)
        lists.push_back(ids[index % 5]);

    struct Case {
        oa::data::limits::BuildLists lists;
        uint32_t expected{};
    };

    const Case cases[] = {
        {{}, 31},
        {{36, oa::data::limits::BuildListOverflow::truncate}, 37},
        {{oa::data::limits::highest_build_list_copy, oa::data::limits::BuildListOverflow::truncate},
         oa::data::limits::highest_build_list_copy + 1},
        {{30, oa::data::limits::BuildListOverflow::dynamic},
         oa::data::limits::highest_build_list_copy + 1},
    };
    for (const auto& c : cases) {
        Fake fake;
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
        ComputerPlayers state{};
        describe_catalog(state);
        state.build_lists = c.lists;
        check(computer_players_configure(&state, "", lists), "long list: configure");
        check(computer_players_initialize(&state, host_for(fake)), "long list: initialize");
        const auto& com = state.types[ARMCOM];
        check(com.build_count == c.expected, "long list: entries kept");
        bool in_order = com.build_ids != nullptr;
        for (uint32_t index = 0; in_order && index < com.build_count; ++index)
            in_order = com.build_ids[index] == ids[index % 5];
        check(in_order, "long list: the first entries, in order");
        check(state.types[ARMLAB].build_count == 0, "long list: other builders keep theirs");
        computer_players_release(&state);
    }
}

/// A builder's list is its type's whole build list, the download menus' entries
/// included: ids in order with repeats kept, an id outside the table skipped, none for a
/// type that builds nothing, and empty lists for types the lists do not reach.
void test_whole_build_lists() {
    // ARMCOM lists a repeated entry; ARMMEX, no builder, lists one it never gets; the
    // lists end before ARMCK.
    const std::vector<uint16_t> lists = {
        0, 4, ARMSOLAR, ARMLLT, ARMLLT, TYPE_COUNT + 3, 1, ARMPW, 0, 2, ARMPW, ARMLLT
    };
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", lists), "whole lists: configure");
    check(computer_players_initialize(&state, host_for(fake)), "whole lists: initialize");
    const auto& com = state.types[ARMCOM];
    check(
        com.build_count == 3 && com.build_ids[0] == ARMSOLAR && com.build_ids[1] == ARMLLT &&
            com.build_ids[2] == ARMLLT,
        "whole lists: in order, repeats kept, an id outside the table skipped"
    );
    check(state.types[ARMMEX].build_count == 0, "whole lists: a type that builds nothing");
    const auto& lab = state.types[ARMLAB];
    check(
        lab.build_count == 2 && lab.build_ids[0] == ARMPW && lab.build_ids[1] == ARMLLT,
        "whole lists: the lab's list"
    );
    check(
        state.types[ARMCK].has_build_list == 1 && state.types[ARMCK].build_count == 0,
        "whole lists: a builder the lists do not reach has an empty list"
    );
    computer_players_release(&state);
}

void test_sort_squads() {
    ComputerPlayers state{};
    describe_catalog(state);
    oa::Unit unit{};
    unit.flags = OA_UNIT_FLAG_BUILDING;
    check(computer_sort_squad(unit, state.types[ARMSOLAR]) == Squad::structures, "structure");
    unit.flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    check(computer_sort_squad(unit, state.types[ARMLLT]) == Squad::armed_structures, "defence");
    unit.flags = OA_UNIT_FLAG_HAS_WEAPONS;
    check(computer_sort_squad(unit, state.types[ARMPW]) == Squad::land_army, "army");
    check(computer_sort_squad(unit, state.types[ARMCK]) == Squad::builders, "builder");
    check(computer_sort_squad(unit, state.types[ARMFIG]) == Squad::aircraft, "aircraft");
    check(computer_sort_squad(unit, state.types[ARMPT]) == Squad::navy, "navy");
    unit.flags = 0;
    check(computer_sort_squad(unit, state.types[ARMPW]) == Squad::none, "unarmed mobile");
    computer_players_release(&state);
}

void test_profile() {
    for (const auto difficulty : {OA_DIFFICULTY_EASY, OA_DIFFICULTY_HARD}) {
        Fake fake;
        fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 5);
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
        auto host = host_for(fake);
        host.difficulty = difficulty;
        ComputerPlayers state{};
        describe_catalog(state);
        check(
            computer_players_configure(
                &state,
                R"(// sample
plan easy
Weight ARM 0.2
Weight ARMLAB 0.5
Weight PLANT 2
Limit ARMMEX 8
limit level1 3
plan hard
limit ARMMEX 1
weight ARMSOLAR 0.1 # trailing comment
)",
                kBuildLists
            ),
            "configure"
        );
        check(computer_players_initialize(&state, host), "initialize");
        check(!state.players[0].present && state.players[1].present, "computer controller only");
        const auto& k = state.players[1].knowledge;
        if (difficulty == OA_DIFFICULTY_EASY) {
            check(k.weight_percent[ARMPW] == 20, "category weight scales");
            check(k.weight_percent[ARMLAB] == 10, "exact weight locks after category");
            check(k.weight_percent[CORMEX] == 100, "other side untouched");
            check(k.limits[ARMMEX] == 8 && k.limit_locked[ARMMEX] == 1, "exact limit locks");
            check(k.limits[ARMSOLAR] == 3, "category limit");
        } else {
            check(k.weight_percent[ARMPW] == 100, "easy plan skipped on hard");
            check(k.limits[ARMMEX] == 1, "hard limit");
            check(k.weight_percent[ARMSOLAR] == 10, "comment stripped");
        }
        const auto& com = state.types[ARMCOM];
        check(
            com.build_count == 4 && com.build_ids[0] == ARMSOLAR && com.build_ids[2] == ARMLAB &&
                com.build_ids[3] == CORMEX,
            "side build list"
        );
        check(
            state.types[ARMCK].has_build_list && state.types[ARMCK].build_count == 0,
            "builder without list"
        );
        check(
            k.base_weights[ARMLAB] == 60 && k.base_weights[ARMSOLAR] == 40 &&
                k.base_weights[ARMCOM] == 20 && k.base_weights[ARMPW] == 0,
            "base weights"
        );
        computer_players_release(&state);
    }
}

// +AI hands a human to the computer after the start. That player gets a
// controller and one pass of the profile; the computer already playing is
// not scaled again.
void test_adopt_handed_player() {
    Fake fake;
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 5);
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    check(
        computer_players_configure(&state, "plan any\nweight ARM 0.5\n", kBuildLists), "configure"
    );
    check(computer_players_initialize(&state, host), "initialize");
    check(!state.players[0].present && state.players[1].present, "only the computer starts");
    const auto kept = state.players[1].knowledge.weight_percent[ARMPW];
    fake.player(0, OA_PLAYER_STATUS_COMPUTER, 1, 5);
    computer_players_adopt(&state, host, 0);
    check(state.players[0].present != 0, "a handed player gets a controller");
    check(state.players[0].knowledge.weight_percent[ARMPW] == kept, "the profile applies once");
    check(state.players[1].knowledge.weight_percent[ARMPW] == kept, "the other computer is kept");
    computer_players_adopt(&state, host, 0);
    check(state.players[0].present != 0, "a second adopt leaves the controller");
    fake.player(2, OA_PLAYER_STATUS_LOCAL, 40, 50);
    computer_players_adopt(&state, host, 2);
    check(state.players[2].present == 0, "a human is not adopted");
    computer_players_release(&state);
}

// The downloadable weight and limit passes run once per computer player, each applying the
// downloadable types' directives to every controller in type order. With two
// computer players a category weight compounds over four passes (100 -> 50
// -> 25 -> 12 -> 6); an exact name locks the weight after its first use; the
// limit pass skips a type whose limit the profile locked, so ARMCOM's LEVEL1
// weight is applied only by the two weight passes.
void test_downloadable_directives() {
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    fake.player(2, OA_PLAYER_STATUS_COMPUTER, 31, 50);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    state.types[ARMFIG].flags |= OA_UNIT_DEF_FLAG_DOWNLOADABLE;
    std::snprintf(
        state.types[ARMFIG].ai_directives,
        sizeof state.types[ARMFIG].ai_directives,
        "weight VTOL 0.5\nweight ARMPT 0.5\n"
    );
    state.types[ARMCOM].flags |= OA_UNIT_DEF_FLAG_DOWNLOADABLE;
    std::snprintf(
        state.types[ARMCOM].ai_directives,
        sizeof state.types[ARMCOM].ai_directives,
        "weight SHIP 0.5\nweight LEVEL1 0.5\n"
    );
    check(
        computer_players_configure(&state, "plan any\nlimit ARMCOM 2\n", kBuildLists), "configure"
    );
    check(computer_players_initialize(&state, host), "initialize");
    for (const int player : {1, 2}) {
        const auto& k = state.players[player].knowledge;
        check(k.weight_percent[ARMFIG] == 6, "category weight compounds once per pass and player");
        check(k.limit_locked[ARMCOM] == 1, "profile limit locks ARMCOM");
        check(k.weight_percent[ARMMEX] == 25, "limit-locked type skipped by the limit passes");
        // SHIP halves ARMPT first, then ARMFIG's exact directive halves and locks it.
        check(k.weight_percent[ARMPT] == 25 && k.weight_locked[ARMPT] == 1, "exact weight locks");
    }
    computer_players_release(&state);
}

std::string read_all(std::FILE* file) {
    std::string text;
    std::rewind(file);
    char buffer[256];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) != 0)
        text.append(buffer, n);
    return text;
}

// Layout of the weight report: time from 30 Hz ticks, controller from Player.status,
// null paths as "(null)", then per type the limit ("n/a " when unlimited),
// signed strength triple, weight percentage, unit name and name.
void test_weight_report() {
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    std::snprintf(fake.world.game.players[1].name, sizeof fake.world.game.players[1].name, "Robot");
    fake.world.game.tick = 108000 + 2 * 1800 + 3 * 30 + 7;
    fake.world.unit_def_count = ARMSOLAR + 1;
    std::snprintf(fake.defs[ARMMEX].unit_name, sizeof fake.defs[ARMMEX].unit_name, "ARMMEX");
    std::snprintf(fake.defs[ARMMEX].name, sizeof fake.defs[ARMMEX].name, "Metal Extractor");
    fake.strength[ARMMEX][1] = 0xfb;
    auto host = host_for(fake);
    host.difficulty = OA_DIFFICULTY_HARD;
    ComputerPlayers state{};
    describe_catalog(state);
    check(
        computer_players_configure(
            &state, "plan any\nlimit ARMMEX 8\nweight ARMSOLAR 0.5\n", kBuildLists
        ),
        "configure"
    );
    check(computer_players_initialize(&state, host), "initialize");
    const auto scratch = oa::test::make_scratch_directory("oa-ai-computer-report");
    std::FILE* out = oa::platform::open_file(scratch / "weights.txt", "w+b");
    computer_write_report(&state, host, 1, {"Maps/Test.TNT", nullptr}, out);
    const std::string expected = "Match clock: 01:02:03\r\n"
                                 "Name: 'Robot' in player slot 1\r\n"
                                 "Played by: AI\r\n"
                                 "Map file: 'Maps/Test.TNT'\r\n"
                                 "AI settings file: '(null)'\r\n"
                                 "Challenge level: 'HARD'\r\n"
                                 "================================================\r\n"
                                 "Columns: build limit - priority : metal value : energy value "
                                 "= weight percent before economy adjustment - short name : "
                                 "full name\r\n"
                                 "n/a  -  50 :  50 :  50 = 100 - '\t\t:'\r\n"
                                 "   8 -  50 :  -5 :  50 = 100 - 'ARMMEX\t\t:Metal Extractor'\r\n"
                                 "n/a  -  50 :  50 :  50 =  50 - '\t\t:'\r\n";
    check(read_all(out) == expected, "weight report text");
    std::fclose(out);
    out = oa::platform::open_file(scratch / "header.txt", "w+b");
    fake.world.game.players[0].status = OA_PLAYER_STATUS_MIRRORED;
    computer_write_report(&state, host, 0, {}, out);
    const auto header = read_all(out);
    check(
        header.find("Played by: INVALID\r\n") != std::string::npos &&
            header.find("Map file: '(null)'") != std::string::npos &&
            header.ends_with("short name : full name\r\n"),
        "report header only without a controller"
    );
    std::fclose(out);
    std::error_code removal;
    std::filesystem::remove_all(scratch, removal);
    computer_players_release(&state);
}

void test_score_and_pick() {
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", kBuildLists), "configure");
    check(computer_players_initialize(&state, host), "initialize");
    auto& p = fake.world.game.players[1];
    // Full stores and an income of at least 200 energy and 5 metal: all
    // weight on the base priority.
    p.energy_produced = 200.0F;
    fake.strength[ARMSOLAR][0] = 80;
    fake.strength[ARMSOLAR][1] = 10;
    fake.strength[ARMSOLAR][2] = 30;
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 80, "base priority score");
    // The build score grades the income, not the stores:
    // energy under 50 adds 100 to the energy need, under 200 adds 10; metal
    // under 3 adds 100 to the metal need, under 5 adds 20.
    p.energy_produced = 49.0F;
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 30, "low energy income");
    p.energy_produced = 199.0F;
    check(
        computer_build_score(&state, host, 1, ARMSOLAR) == (30 * 10 + 80 * 90) / 100,
        "modest energy income"
    );
    p.energy_produced = 200.0F;
    p.metal_produced = 2.0F;
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 10, "low metal income");
    p.metal_produced = 4.0F;
    check(
        computer_build_score(&state, host, 1, ARMSOLAR) == (10 * 20 + 80 * 80) / 100,
        "modest metal income"
    );
    p.metal_produced = 5.0F;
    // Metal short: metal need 100 takes every share; score is strength[1].
    p.metal = 30.0F;
    p.metal_storage = 500.0F;
    p.metal_produced = 0.0F;
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 10, "metal need score");
    // Energy need: (1000 - 600) * 0.125 = 50; metal need 0 -> e 50, base 50.
    p.metal = 500.0F;
    p.metal_produced = 5.0F;
    p.energy = 600.0F;
    check(
        computer_build_score(&state, host, 1, ARMSOLAR) == (30 * 50 + 80 * 50) / 100,
        "energy split score"
    );
    p.energy = 40.0F;
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 0, "low energy rejects");
    p.energy = 1000.0F;
    state.players[1].knowledge.limits[ARMSOLAR] = 0;
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 0, "limit rejects");
    state.players[1].knowledge.limits[ARMSOLAR] = unlimited;
    state.players[1].knowledge.weight_percent[ARMSOLAR] = 50;
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 40, "weight scales score");

    // Pick: running total 40 (ARMSOLAR), 90, 140; rolls keep the later ones.
    fake.spawn(10, 1, ARMCOM, 100, 100, 0);
    fake.rolls = {0, 10, 139, 200};
    const auto pick = computer_pick_build(&state, host, 1, 10);
    check(pick == ARMMEX || pick == ARMSOLAR || pick == ARMLAB || pick == 0, "pick in list");
    fake.roll = 0;
    fake.rolls = {0, 0, 0, 0};
    check(computer_pick_build(&state, host, 1, 10) == 0, "other-side pick rejected");
    computer_players_release(&state);
}

// An unscripted lab (CC14's ARMLAB, AC10's CORLAB) under a computer whose
// metal income is below 3. The strategic refresh values a type's metal as
// its metal cost x -0.02, +100 for an extractor and +25 for a maker, and its
// energy as its energy cost x -0.0025 less five times its energy rate, each
// clamped to 0..100, so a kbot keeps only its base priority. The build score then
// gives the metal value the whole share (metal need >= 100), and the pick
// has nothing to pick until the income reaches 3.
void test_lab_idle_without_metal_income() {
    namespace combat = oa::sim::combat_state;
    std::vector<combat::StrategicType> strategic(TYPE_COUNT);
    for (auto& type : strategic)
        type.min_water_depth = -10000;
    auto& pw = strategic[ARMPW]; // ARMPW.FBI
    pw.abilities = combat::ability_can_attack;
    pw.build_cost_energy = 697.0F;
    pw.build_cost_metal = 53.0F;
    pw.energy_use = 0.3F;
    pw.energy_make = 0.3F;
    auto& ck = strategic[ARMCK]; // ARMCK.FBI
    ck.flags = combat::def_flag_builder;
    ck.build_cost_energy = 2410.0F;
    ck.build_cost_metal = 120.0F;
    ck.energy_use = 0.1F;
    ck.energy_make = 8.0F;
    auto& mex = strategic[ARMMEX]; // ARMMEX.FBI
    mex.build_cost_energy = 521.0F;
    mex.build_cost_metal = 50.0F;
    mex.energy_use = 3.0F;
    mex.extracts_metal = 0.001F;
    auto& solar = strategic[ARMSOLAR]; // ARMSOLAR.FBI
    solar.build_cost_energy = 760.0F;
    solar.build_cost_metal = 145.0F;
    solar.energy_use = -20.0F;
    combat::StrategicRefreshState refreshed;
    combat::strategic_refresh(refreshed, strategic, {});
    using Strength = std::array<uint8_t, 3>; // priority, metal value, energy value
    check(refreshed.strengths[ARMPW] == Strength{84, 0, 0}, "kbot has no metal or energy value");
    check(refreshed.strengths[ARMCK] == Strength{100, 0, 0}, "constructor has no metal value");
    check(refreshed.strengths[ARMMEX] == Strength{100, 99, 0}, "extractor metal value");
    check(refreshed.strengths[ARMSOLAR] == Strength{100, 0, 98}, "solar energy value");

    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    for (uint16_t type = 1; type < TYPE_COUNT; ++type)
        std::memcpy(fake.strength[type], refreshed.strengths[type].data(), 3);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", kBuildLists), "configure");
    check(computer_players_initialize(&state, host), "initialize");
    fake.spawn(10, 1, ARMLAB, 100, 100, OA_UNIT_FLAG_BUILDING);
    auto& p = fake.world.game.players[1];
    p.energy_produced = 200.0F;
    p.metal_produced = 2.9F;
    check(computer_build_score(&state, host, 1, ARMPW) == 0, "kbot scores 0 under 3 metal");
    check(computer_build_score(&state, host, 1, ARMCK) == 0, "constructor scores 0 under 3 metal");
    check(computer_build_score(&state, host, 1, ARMMEX) == 99, "extractor keeps its metal value");
    fake.rolls = {0, 0};
    check(computer_pick_build(&state, host, 1, 10) == 0, "lab idle under 3 metal");
    // Energy income under 50 with no surplus makes the energy need 120; the 20
    // above the metal share goes to the energy value, which kbots lack too.
    p.energy_produced = 10.0F;
    p.energy_requested = 10.0F;
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 98 * 20 / 100, "energy share");
    check(computer_pick_build(&state, host, 1, 10) == 0, "lab idle on low energy too");
    // From 3 metal the need is 20, leaving 80 on the base priority.
    p.energy_produced = 200.0F;
    p.energy_requested = 0.0F;
    p.metal_produced = 3.0F;
    check(
        computer_build_score(&state, host, 1, ARMPW) == 84 * 80 / 100, "kbot priority from 3 metal"
    );
    // From 5 metal with a surplus the base priority takes the whole share:
    // running totals 84 then 184; the second roll (150) keeps ARMPW.
    p.metal_produced = 5.0F;
    p.metal_requested = 1.0F;
    check(computer_build_score(&state, host, 1, ARMCK) == 100, "constructor priority from 5 metal");
    fake.roll = 0;
    fake.rolls = {0, 150};
    check(computer_pick_build(&state, host, 1, 10) == ARMPW, "lab picks from 5 metal");
    computer_players_release(&state);
}

// The match's knowledge refresh as it reaches a computer player: every 30
// ticks the walk reports the player's live, finished own units.
void refresh_knowledge(ComputerPlayers& state, Fake& fake, uint8_t index) {
    constexpr uint32_t refresh_period = 30;
    auto* knowledge = computer_player_knowledge(&state, index);
    const auto tick = fake.world.game.tick;
    if (knowledge == nullptr || fake.knowledge_tick[index] + refresh_period > tick)
        return;
    computer_knowledge_clear(&state, *knowledge);
    KnowledgeTally tally;
    for (const auto& unit : fake.units)
        if ((unit.flags & OA_UNIT_FLAG_LIVE) != 0 && unit.owner_index == index &&
            unit.build_remaining == 0.0F)
            computer_knowledge_count(&state, *knowledge, unit, tally);
    computer_knowledge_settle(*knowledge, tally);
    fake.knowledge_tick[index] = tick;
}

// One match tick of the computer players: each player's controller order
// tick when it is a computer, then its knowledge refresh.
void tick_players(ComputerPlayers& state, Fake& fake, const ComputerHost& host) {
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const auto& player = host.world->game.players[index];
        if (player.in_use == 0)
            continue;
        if (player_has_controller(player) && player.status == OA_PLAYER_STATUS_COMPUTER)
            computer_player_tick_orders(&state, host, index);
        refresh_knowledge(state, fake, index);
    }
}

uint32_t standing_move(const oa::Unit& unit) {
    return (unit.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
}

uint32_t standing_fire(const oa::Unit& unit) {
    return (unit.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
}

void tick_until(
    ComputerPlayers& state, Fake& fake, const ComputerHost& host, uint32_t first, uint32_t last
) {
    for (uint32_t tick = first; tick <= last; ++tick) {
        fake.world.game.tick = tick;
        tick_players(state, fake, host);
    }
}

// The squad sort takes only units with OA_UNIT_FLAG_SELECTABLE. A unit an InitialMission script
// holds (which clears the bit) keeps its squad and its scripted standing
// orders until its MakeSelectable order runs.
void test_scripted_units_stay_unsorted() {
    Fake fake;
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", kBuildLists), "scripted: configure");
    constexpr uint32_t hold_fire = 0, hold_position = 0;
    auto& free_tank = fake.spawn(12, 1, ARMPW, 300, 300, OA_UNIT_FLAG_HAS_WEAPONS);
    auto& scripted = fake.spawn(13, 1, ARMPW, 320, 300, OA_UNIT_FLAG_HAS_WEAPONS);
    scripted.flags = (scripted.flags & ~OA_UNIT_FLAG_SELECTABLE) | OA_UNIT_FLAG_NOT_SELECTABLE;
    auto& tower =
        fake.spawn(14, 1, ARMLLT, 340, 300, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
    tower.flags &= ~OA_UNIT_FLAG_SELECTABLE;
    fake.rolls.assign(4096, 7);
    check(computer_players_initialize(&state, host), "scripted: initialize");
    tick_until(state, fake, host, 1, 30);
    check(free_tank.squad == static_cast<int32_t>(Squad::land_army), "unscripted tank sorted");
    check(
        standing_move(free_tank) == 2 && standing_fire(free_tank) == 2,
        "unscripted tank roams and fires at will"
    );
    check(scripted.squad == 0 && tower.squad == 0, "scripted units keep no squad");
    check(
        standing_move(scripted) == hold_position && standing_fire(scripted) == hold_fire,
        "scripted standing orders untouched"
    );
    // MakeSelectable: clear OA_UNIT_FLAG_NOT_SELECTABLE, set OA_UNIT_FLAG_SELECTABLE; the
    // next sort takes the unit.
    scripted.flags = (scripted.flags & ~OA_UNIT_FLAG_NOT_SELECTABLE) | OA_UNIT_FLAG_SELECTABLE;
    tick_until(state, fake, host, 31, 60);
    check(scripted.squad == static_cast<int32_t>(Squad::land_army), "selectable tank sorted");
    check(
        standing_move(scripted) == 2 && standing_fire(scripted) == 2,
        "selectable tank takes the standing orders"
    );
    check(tower.squad == 0, "tower still held by its script");
    computer_players_release(&state);
}

// The structures task queues a pick only for a factory whose primary order list is
// empty: a sorted factory still busy with a build or a Wait gets none until
// its orders run out.
void test_busy_factory_gets_no_pick() {
    Fake fake;
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", kBuildLists), "busy factory: configure");
    fake.spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING);
    fake.spawn(12, 1, ARMLAB, 300, 200, OA_UNIT_FLAG_BUILDING);
    fake.holds_order[12] = true;
    fake.rolls.assign(4096, 7);
    check(computer_players_initialize(&state, host), "busy factory: initialize");
    const auto picks = [&fake](uint16_t slot) {
        uint32_t count = 0;
        for (const auto& order : fake.orders)
            count += order.kind == "factory" && order.unit == slot ? 1u : 0u;
        return count;
    };
    tick_until(state, fake, host, 1, 120);
    check(
        fake.units[11].squad == static_cast<int32_t>(Squad::structures) &&
            fake.units[12].squad == static_cast<int32_t>(Squad::structures),
        "both factories sorted into the structures squad"
    );
    check(picks(11) != 0, "the idle factory queues a pick");
    check(picks(12) == 0, "a factory holding an order gets no pick");
    fake.holds_order[12] = false;
    tick_until(state, fake, host, 121, 180);
    check(picks(12) != 0, "the factory queues a pick once its orders run out");
    computer_players_release(&state);
}

void test_controller_presence() {
    oa::Player player{};
    check(!player_has_controller(player), "a free slot has no controller");
    player.status = OA_PLAYER_STATUS_LOCAL;
    check(player_has_controller(player), "a closed local slot still gets one");
    player.in_use = 1;
    check(player_has_controller(player), "local player");
    player.status = OA_PLAYER_STATUS_COMPUTER;
    check(player_has_controller(player), "computer player");
    player.status = OA_PLAYER_STATUS_MIRRORED;
    check(
        !player_has_controller(player), "a mirrored player runs its controller on its own machine"
    );
    player.in_use = 0;
    check(
        player_has_controller(player),
        "the mirrored-player test reads the record's in-use word first"
    );
}

void test_tick() {
    Fake fake;
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", kBuildLists), "configure");
    fake.strength[CORMEX][0] = fake.strength[CORMEX][1] = fake.strength[CORMEX][2] = 0;
    fake.spawn(1, 0, ARMLLT, 900, 900, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
    fake.spawn(10, 1, ARMCOM, 200, 200, 0);
    fake.spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING);
    for (uint32_t slot = 12; slot < 16; ++slot)
        fake.spawn(slot, 1, ARMPW, 300 + static_cast<int32_t>(slot), 300, OA_UNIT_FLAG_HAS_WEAPONS);
    fake.rolls.assign(4096, 7);
    check(computer_players_initialize(&state, host), "initialize");
    // The first squad sort comes after the 30-tick countdown.
    for (uint32_t tick = 1; tick <= 29; ++tick) {
        fake.world.game.tick = tick;
        tick_players(state, fake, host);
    }
    check(fake.units[10].squad == 0, "no sort before the countdown");
    fake.world.game.tick = 30;
    tick_players(state, fake, host);
    // Tasks already ran on empty squads; the next due runs see the members.
    for (uint32_t tick = 31; tick <= 120; ++tick) {
        fake.world.game.tick = tick;
        tick_players(state, fake, host);
    }
    check(
        fake.units[10].squad == 4 && fake.units[11].squad == 1 && fake.units[12].squad == 3,
        "sorted into squads"
    );
    check(
        ((fake.units[12].flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT) ==
                2 &&
            ((fake.units[10].flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >>
             OA_UNIT_FLAG_MOVE_ORDER_SHIFT) == 1,
        "standing move orders"
    );
    bool factory = false, build = false, attack = false;
    for (const auto& order : fake.orders) {
        factory |= order.kind == "factory" && order.unit == 11 &&
                   (order.arg == ARMPW || order.arg == ARMCK);
        build |= order.kind == "build" && order.unit == 10;
    }
    check(factory, "idle factory queues a unit");
    check(build, "commander places a build");
    // Four army units exceed the strike minimum: the strike squad recruits
    // them and, with no home squad having members, attacks the nearest enemy.
    fake.orders.clear();
    fake.set_squad(11, 0);
    fake.units[11].flags = 0;
    fake.set_squad(10, 0);
    fake.units[10].flags = 0;
    fake.world.game.tick = 400;
    tick_players(state, fake, host);
    std::size_t strike = fake.squads[{1, 2}].size();
    for (const auto& order : fake.orders)
        attack |= order.kind == "attack" && order.arg == 1;
    check(strike == 4, "strike squad recruited the army");
    check(attack, "strike squad attacks the nearest enemy");
    computer_players_release(&state);
}

// The sighted weight near a point: the signed base weights of the units on the
// player's seen list within the radius on the ground plane, a unit's distance
// taken from the whole parts of its squared 16.16 offsets.
void test_sighted_weight() {
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", kBuildLists), "configure");
    check(computer_players_initialize(&state, host), "initialize");
    auto* knowledge = computer_player_knowledge(&state, 1);
    check(knowledge != nullptr && knowledge->base_weights[ARMLLT] == 40, "structure weight");
    if (knowledge == nullptr) {
        computer_players_release(&state);
        return;
    }
    knowledge->base_weights[ARMPW] = -5;
    constexpr int32_t x = 500, z = 700;
    const oa::FixedVec3 at{x << 16, 0, z << 16};
    fake.spawn(40, 0, ARMLLT, x + 96, z + 128, OA_UNIT_FLAG_BUILDING); // exactly 160 away
    fake.spawn(41, 0, ARMLLT, x + 161, z, OA_UNIT_FLAG_BUILDING);      // just outside
    fake.spawn(42, 0, ARMPW, x, z - 10, 0).flags = 0;                  // dead, still counts
    fake.spawn(43, 0, ARMLLT, x - 160, z, OA_UNIT_FLAG_BUILDING);
    // 200/65536 of a unit past 160: the square's whole part is still 25600.
    fake.units[43].position.x -= 200;
    fake.spawn(44, 0, ARMLLT, x, z, OA_UNIT_FLAG_BUILDING);
    fake.units[44].type_index = TYPE_COUNT + 3; // outside the type table: adds nothing
    fake.spawn(45, 0, ARMSOLAR, x + 20, z + 20, OA_UNIT_FLAG_BUILDING); // not sighted
    std::array<uint16_t, 6> seen{40, 41, 42, 43, 44, 63};
    oa::sim::detection::Sightings sightings{};
    sightings.seen = seen.data();
    sightings.capacity = static_cast<uint32_t>(seen.size());
    sightings.seen_count = static_cast<uint32_t>(seen.size());
    // 40 + 40 - 5 for units 40, 43 and 42; slot 63 is an empty record of type 0.
    check(
        computer_sighted_weight(&state, *knowledge, sightings, fake.world, at, 160) == 75,
        "sighted weight within 160"
    );
    check(
        computer_sighted_weight(&state, *knowledge, sightings, fake.world, at, 159) == -5,
        "sighted weight within 159"
    );
    check(
        computer_sighted_weight(&state, *knowledge, sightings, fake.world, at, 161) == 115,
        "sighted weight within 161"
    );
    sightings.seen_count = 0;
    check(
        computer_sighted_weight(&state, *knowledge, sightings, fake.world, at, 160) == 0,
        "no sightings"
    );
    computer_players_release(&state);
}

// What the siege test's host answers beyond the fake match.
struct SiegeView {
    std::array<uint16_t, 4> seen{};
    oa::sim::detection::Sightings sightings{};
    bool reach[64]{};
    int32_t visible_below_z = 0; // player 1 sees points with z under this, world units
};

SiegeView siege_view;

// The siege task: with members in squad 9 it turns its search now and then,
// steps it, weighs a point the player sees by what it has sighted around it,
// keeps the better draw as its target and orders its members to attack there.
void test_siege() {
    Fake fake;
    auto& siege = siege_view;
    siege = {};
    fake.world.game.map_width_world = 1024;
    fake.world.game.map_height_world = 512;
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    auto host = host_for(fake);
    host.point_visible = [](void*, uint8_t player, const oa::FixedVec3* at) {
        return player == 1 && (at->z >> 16) < siege_view.visible_below_z;
    };
    host.sightings = [](void*, uint8_t player) -> const oa::sim::detection::Sightings* {
        return player == 1 ? &siege_view.sightings : nullptr;
    };
    host.weapon_reaches = [](void*, uint16_t unit, const oa::FixedVec3*) {
        return unit < 64 && siege_view.reach[unit];
    };
    host.order_attack_point = [](void* c, uint16_t unit, const oa::FixedVec3* at) {
        fake_of(c)->orders.push_back({"attack point", unit, 0, *at, false});
        return true;
    };
    fake.defs[ARMPW].abilities = OA_UNIT_DEF_ABILITY_CAN_ATTACK;
    fake.defs[ARMLLT].abilities = OA_UNIT_DEF_ABILITY_CAN_ATTACK;
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", kBuildLists), "configure");
    check(computer_players_initialize(&state, host), "initialize");
    auto& ai = state.players[1];
    auto& task = ai.tasks[static_cast<uint32_t>(Squad::siege)];
    check(task.kind == TaskKind::siege, "siege task");
    const oa::FixedVec3 centre{512 << 16, 0, 256 << 16};
    const auto same = [](const oa::FixedVec3& a, const oa::FixedVec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    check(
        same(task.siege_target, centre) && same(task.siege_probe, centre) &&
            same(task.siege_step, centre) && task.siege_weight == 0,
        "the siege starts at the map's centre"
    );
    // Only the siege runs: every other task and the sort wait.
    for (auto& other : ai.tasks)
        if (&other != &task)
            other.next_tick = 1000000;
    ai.sort_countdown = 1000000;
    fake.world.game.tick = 100;
    // An empty squad takes only the reschedule's draw.
    fake.rolls = {7};
    fake.roll = 0;
    computer_player_tick_orders(&state, host, 1);
    check(fake.roll == 1 && task.next_tick == 100 + 7 + 30, "empty siege squad reschedules");
    check(same(task.siege_probe, centre) && fake.orders.empty(), "empty siege squad stays");

    // Members: two armed walkers, two armed structures (one in reach) and a
    // builder that cannot attack.
    fake.spawn(20, 1, ARMPW, 100, 100, OA_UNIT_FLAG_HAS_WEAPONS).movement = 1;
    fake.spawn(21, 1, ARMLLT, 120, 100, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
    fake.spawn(22, 1, ARMLLT, 140, 100, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
    fake.spawn(23, 1, ARMCK, 160, 100, 0).movement = 1;
    fake.spawn(24, 1, ARMPW, 180, 100, OA_UNIT_FLAG_HAS_WEAPONS).movement = 1;
    siege.reach[22] = true;
    for (const uint16_t slot : std::array<uint16_t, 5>{20, 21, 22, 23, 24}) {
        auto& unit = fake.units[slot];
        unit.def = oa::world_unit_def_ref(&fake.world, &fake.defs[unit.type_index]);
        fake.set_squad(slot, static_cast<int>(Squad::siege));
    }
    // Sighted around the first step: two structures of weight 40 in reach, one
    // out of it.
    fake.spawn(2, 0, ARMLLT, 512 + 96, 576 + 128, OA_UNIT_FLAG_BUILDING);
    fake.spawn(3, 0, ARMLLT, 512, 576 - 30, OA_UNIT_FLAG_BUILDING);
    fake.spawn(4, 0, ARMLLT, 512 + 170, 576, OA_UNIT_FLAG_BUILDING);
    siege.seen = {2, 3, 4, 0};
    siege.sightings.seen = siege.seen.data();
    siege.sightings.capacity = 4;
    siege.sightings.seen_count = 3;
    siege.visible_below_z = 800;
    // Reschedule 9; the search turns (0 of 10) to half a turn, so it steps 320
    // world units toward +z; the point weighs 80 and its draw 12 beats the
    // target's, which draws nothing below 2.
    fake.world.game.tick = task.next_tick;
    fake.rolls = {9, 0, 0x8000, 12};
    fake.roll = 0;
    computer_player_tick_orders(&state, host, 1);
    const oa::FixedVec3 step{
        -oa::sim::unit_movement::sine_scaled(0x8000, 0x1400000),
        0,
        -oa::sim::unit_movement::cosine_scaled(0x8000, 0x1400000)
    };
    check(step.x == 0 && step.z == 0x1400000, "half a turn steps along +z");
    const oa::FixedVec3 first{512 << 16, 0, 576 << 16};
    check(fake.roll == 4, "four draws");
    check(same(task.siege_step, step) && same(task.siege_probe, first), "the search stepped");
    check(
        same(task.siege_target, first) && task.siege_weight == 80, "the richer point is the target"
    );
    std::vector<uint16_t> ordered;
    for (const auto& order : fake.orders) {
        check(order.kind == "attack point" && same(order.at, first), "attack at the target");
        ordered.push_back(order.unit);
    }
    check(
        ordered == std::vector<uint16_t>{20, 22, 24},
        "walkers, and structures in reach, attack; the builder does not"
    );
    // The next run keeps the heading, steps past what the player sees and
    // leaves the target; one after that sees a poorer point, whose draw loses.
    fake.orders.clear();
    fake.world.game.tick = task.next_tick;
    siege.visible_below_z = 800;
    fake.rolls = {1, 3};
    fake.roll = 0;
    computer_player_tick_orders(&state, host, 1);
    const oa::FixedVec3 second{512 << 16, 0, 896 << 16};
    check(fake.roll == 2 && same(task.siege_probe, second), "unseen step draws nothing more");
    check(same(task.siege_target, first) && task.siege_weight == 80, "the target stays");
    check(fake.orders.size() == 3, "the members attack the target again");
    fake.world.game.tick = task.next_tick;
    siege.visible_below_z = 2000;
    siege.sightings.seen_count = 1; // unit 2 alone, far from the third point
    fake.rolls = {1, 3, 5};
    fake.roll = 0;
    computer_player_tick_orders(&state, host, 1);
    check(fake.roll == 3, "a seen point with nothing near draws only against the target");
    check(same(task.siege_target, first) && task.siege_weight == 80, "a poorer point loses");
    computer_players_release(&state);
}

// The build score is nothing for a DOWNLOADABLE type (OA_UNIT_DEF_FLAG_DOWNLOADABLE)
// while the session object's kind is 1, a campaign.
void test_campaign_downloadables() {
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    state.types[ARMLLT].flags |= OA_UNIT_DEF_FLAG_DOWNLOADABLE;
    check(computer_players_configure(&state, "", kBuildLists), "configure");
    check(computer_players_initialize(&state, host), "initialize");
    // Full stores and positive income: the score is the base priority.
    check(computer_build_score(&state, host, 1, ARMLLT) == 50, "downloadable outside a campaign");
    state.downloadables_restricted = 1;
    check(computer_build_score(&state, host, 1, ARMLLT) == 0, "campaign skips downloadable");
    check(computer_build_score(&state, host, 1, ARMSOLAR) == 50, "campaign keeps other types");
    computer_players_release(&state);
}

// ReloadAIProfiles resets the computer players' tables, then runs the profile
// again. The plan flag is the one setting the directives share: it starts set,
// and the downloadable passes leave it set, so a line before the first plan
// applies on the first load and on a reload.
void test_profile_reload() {
    constexpr const char* profile = "weight ARMSOLAR 0.5\nplan easy\nlimit ARMMEX 4\n";
    Fake fake;
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 5);
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    fake.rolls.assign(64, 5);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    check(
        computer_players_reload_profile(&state, host, profile) && !state.initialized,
        "a reload before the first initialisation only stores the profile"
    );
    check(computer_players_configure(&state, profile, kBuildLists), "configure");
    check(computer_players_initialize(&state, host), "initialize");
    auto& k = state.players[1].knowledge;
    check(
        k.weight_percent[ARMSOLAR] == 50 && k.weight_locked[ARMSOLAR] == 1,
        "the leading line applies on the first load"
    );
    check(k.limits[ARMMEX] == 4 && k.limit_locked[ARMMEX] == 1, "easy plan limit");
    check(state.plan_matches, "the downloadable passes leave the plan flag set");
    computer_apply_weight(&state, 1, "ARMPW", 0.25F);
    computer_apply_limit(&state, 1, "ARMLLT", 2);
    check(k.weight_percent[ARMPW] == 25 && k.limits[ARMLLT] == 2, "console directives");
    const auto grid = k.land_grid;
    const auto rolls = fake.roll;
    check(computer_players_reload_profile(&state, host, profile), "reload");
    check(k.weight_percent[ARMPW] == 100 && k.weight_locked[ARMPW] == 0, "weights reset");
    check(k.limits[ARMLLT] == unlimited && k.limit_locked[ARMLLT] == 0, "limits reset");
    check(
        k.weight_percent[ARMSOLAR] == 50 && k.weight_locked[ARMSOLAR] == 1,
        "the leading line applies from the kept plan flag"
    );
    check(k.limits[ARMMEX] == 4 && k.limit_locked[ARMMEX] == 1, "easy plan limit again");
    check(
        fake.roll == rolls && k.land_grid.step_x == grid.step_x &&
            k.land_grid.phase_z == grid.phase_z,
        "the build grids are kept and nothing is drawn"
    );
    check(state.players[1].present && !state.players[0].present, "controllers kept");
    computer_players_release(&state);
}

/// Collects the numbers of the profile lines that give no number.
///
/// @param context the line numbers, a std::vector<uint32_t>
/// @param line a line of the profile
void collect_lines_without_number(void* context, const ProfileLine& line) noexcept {
    if (computer_profile_line_lacks_number(line))
        static_cast<std::vector<uint32_t>*>(context)->push_back(line.number);
}

/// Returns the numbers of a profile's weight and limit lines that give no number.
///
/// @param text the profile
/// @return their line numbers, from 1
std::vector<uint32_t> lines_without_number(std::string_view text) {
    std::vector<uint32_t> lines;
    computer_profile_lines(text, collect_lines_without_number, &lines);
    return lines;
}

/// The lines `lines_skipped` gathers, and the match whose unit types name the units.
struct SkippedLines {
    const oa::World* world{};      ///< the match
    std::vector<uint32_t> lines{}; ///< the skipped lines' numbers
};

/// Collects the numbers of the profile lines the computer players skip.
///
/// @param context the SkippedLines being filled
/// @param line a line of the profile
void collect_skipped_lines(void* context, const ProfileLine& line) noexcept {
    auto& skipped = *static_cast<SkippedLines*>(context);
    if (computer_profile_line_skipped(line, *skipped.world))
        skipped.lines.push_back(line.number);
}

/// Returns the numbers of a profile's lines the computer players skip.
///
/// @param text the profile
/// @param world the match whose unit types name the units
/// @return their line numbers, from 1
std::vector<uint32_t> lines_skipped(std::string_view text, const oa::World& world) {
    SkippedLines skipped;
    skipped.world = &world;
    computer_profile_lines(text, collect_skipped_lines, &skipped);
    return skipped.lines;
}

/// Reads a text file of the installed game.
///
/// @param assets the installed game's store
/// @param resource '/'-separated path
/// @return the text, or empty when nothing provides the file
std::string read_text(const oa::AssetStore& assets, const char* resource) {
    const auto bytes = oa::test::read_game_file(assets, resource);
    return {bytes.begin(), bytes.end()};
}

// The installed game's computer-player profile and side build lists.
void test_installed_data(const oa::AssetStore& assets) {
    const auto profile = read_text(assets, "ai/default.txt");
    check(!profile.empty(), "the install holds ai/default.txt");
    if (profile.empty())
        return;
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    auto host = host_for(fake);
    host.difficulty = OA_DIFFICULTY_MEDIUM;
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, profile, kBuildLists), "configure from the install");
    check(computer_players_initialize(&state, host), "initialize from the install");
    const auto& k = state.players[1].knowledge;
    check(k.limits[ARMMEX] >= 0, "the installed limit applied");
    check(k.weight_percent[ARMPW] < 100, "the installed ARM weight applied");
    computer_players_release(&state);
    // Core Contingency's ai/Krogoth.txt, when installed, writes ARMDECOM as "ARM DECOM"
    // in each of its three plans.
    const auto krogoth = read_text(assets, "ai/krogoth.txt");
    if (!krogoth.empty()) {
        check(lines_without_number(krogoth).size() >= 3, "ai/krogoth.txt lines told");
        // Those three lines are skipped; its "Limit CORFORT O" lines read as 0.
        const oa::World world{};
        check(
            lines_skipped(krogoth, world) == std::vector<uint32_t>{74, 317, 560},
            "ai/krogoth.txt: its three split names are skipped"
        );
    }
}

// --- ai.* rules -------------------------------------------------------------

using oa::data::match_rules::MatchRules;

// Counts the orders of one kind given to one unit.
uint32_t orders_of(const Fake& fake, const char* kind, uint16_t unit) {
    uint32_t count = 0;
    for (const auto& order : fake.orders)
        count += order.kind == kind && order.unit == unit ? 1u : 0u;
    return count;
}

// The plan flag starts set: the lines a profile puts before its first plan
// line apply to every computer player as the game starts, to one playing a
// difficulty of its own too, and each plan line then selects the lines after it.
void test_profile_leading_lines() {
    constexpr const char* profile = "// every difficulty\n"
                                    "weight ARM 0.2\n"
                                    "limit ARM 0\n"
                                    "weight ARMLAB 4\n"
                                    "limit ARMLAB 2\n"
                                    "plan easy\n"
                                    "limit ARMMEX 6\n"
                                    "plan hard\n"
                                    "limit ARMMEX 9\n";
    Fake fake;
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 5);
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    fake.player(2, OA_PLAYER_STATUS_COMPUTER, 31, 40);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    computer_players_set_difficulty(&state, 2, OA_DIFFICULTY_HARD);
    check(computer_players_configure(&state, profile, kBuildLists), "leading: configure");
    check(computer_players_initialize(&state, host), "leading: initialize");
    for (const uint8_t player : {uint8_t{1}, uint8_t{2}}) {
        const auto& k = state.players[player].knowledge;
        check(
            k.weight_percent[ARMPW] == 20 && k.limits[ARMPW] == 0,
            "leading: the category lines apply"
        );
        check(
            k.weight_percent[ARMLAB] == 80 && k.limits[ARMLAB] == 2 && k.limit_locked[ARMLAB] == 1,
            "leading: the unit lines apply"
        );
        check(
            k.weight_percent[CORMEX] == 100 && k.limits[CORMEX] == unlimited,
            "leading: the other side is untouched"
        );
    }
    check(state.players[1].knowledge.limits[ARMMEX] == 6, "leading: the game's difficulty plan");
    check(state.players[2].knowledge.limits[ARMMEX] == 9, "leading: the player's own plan");
    computer_players_release(&state);
}

// A weight or limit line with a name and no number is told apart; one with a
// number, a line of the command alone and every other line are not.
void test_profile_lines_without_number() {
    const auto lines = lines_without_number(
        "Weight ARM 0.2\n"
        "Limit ARM DECOM 4\n"
        "Limit CORFORT O\n"
        "Limit ARMSABO\n"
        "weight ARMLLT x2\n"
        "limit ARMLAB 4\n"
        "Limit\n"
        "weight ARMSOLAR .5\n"
        "limit CORMEX -1\n"
        "plan hard\n"
        "// Limit ARM DECOM\n"
    );
    check(lines == std::vector<uint32_t>{2, 3, 4, 5}, "lines without a number");
}

// A weight or limit line whose name holds a space, and whose first part names
// no unit, is told apart from one whose first part is a unit's name, and from
// lines with no later number.
void test_profile_lines_skipped() {
    const char* text = "Limit ARM DECOM 4\n"
                       "Weight ARM DECOM 0.5\n"
                       "Weight ARMCK MP 0\n"
                       "Limit ARMCK MP 0\n"
                       "Limit CORFORT O\n"
                       "Limit ARM DE COM 4\n"
                       "Limit ARM DECOM four\n"
                       "Limit ARMSABO\n"
                       "Weight NORTON COMMANDO .5\n"
                       "Weight limit armbrawl 400 // 0.1\n"
                       "Limit ARM 4 DECOM\n";
    Fake fake;
    std::snprintf(fake.defs[ARMCK].unit_name, sizeof fake.defs[ARMCK].unit_name, "%s", "armck");
    check(
        lines_skipped(text, fake.world) == std::vector<uint32_t>{1, 2, 6, 9, 10},
        "skipped: split names whose first part names no unit"
    );
    check(
        lines_without_number(text) == std::vector<uint32_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
        "skipped: every line without a number is still told"
    );
}

// "Limit ARM DECOM 4", ARMDECOM written with a space as in Core Contingency's
// ai/Krogoth.txt, is skipped: 3.1c limits the category ARM to 0 there, every Arm
// type no line names on its own, and with no metal income the commander picks
// nothing. Skipped, the line leaves the extractor unlimited and the commander
// picks it, as with the same profile without that line.
void test_split_name_is_skipped() {
    for (const bool split : {false, true}) {
        Fake fake;
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
        auto& p = fake.world.game.players[1];
        p.metal_produced = 0.0F;
        p.energy_produced = 200.0F;
        using Strength = std::array<uint8_t, 3>; // priority, metal value, energy value
        const auto value = [&fake](uint16_t type, Strength strength) {
            std::memcpy(fake.strength[type], strength.data(), strength.size());
        };
        value(ARMMEX, {100, 99, 0});
        value(ARMSOLAR, {100, 0, 98});
        value(ARMLAB, {100, 0, 0});
        value(CORMEX, {100, 0, 0});
        auto host = host_for(fake);
        ComputerPlayers state{};
        describe_catalog(state);
        const char* profile = split ? "Limit ARM DECOM 4\nWeight ARM DECOM 0.5\nLimit ARMLAB 4\n"
                                    : "Limit ARMLAB 4\n";
        check(computer_players_configure(&state, profile, kBuildLists), "split: configure");
        check(computer_players_initialize(&state, host), "split: initialize");
        const auto& k = state.players[1].knowledge;
        fake.spawn(10, 1, ARMCOM, 100, 100, 0);
        check(
            k.limits[ARMMEX] == unlimited && k.limits[ARMSOLAR] == unlimited &&
                k.limits[ARMCOM] == unlimited,
            "split: the Arm types no line names stay unlimited"
        );
        check(k.weight_percent[ARMPW] == 100, "split: the split weight is skipped too");
        check(k.limits[ARMLAB] == 4, "split: a type named on its own keeps its limit");
        check(k.limits[CORMEX] == unlimited, "split: the other side is untouched");
        check(computer_pick_build(&state, host, 1, 10) == ARMMEX, "split: extractor picked");
        computer_players_release(&state);
    }
}

// "Weight ARMCA MP 0", as in TA 3.3a's ai/issapequa.txt, names a unit before its
// space, so it keeps 3.1c's reading: the weight is multiplied by 0 and locked, and
// a later "Weight ARMCA 2.46" is ignored. ARMCK stands in for ARMCA. The limit line
// of the same shape limits to 0 and locks.
void test_split_unit_name_reads_zero() {
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    auto host = host_for(fake);
    ComputerPlayers state{};
    describe_catalog(state);
    const char* profile = "Weight ARMCK MP 0\n"
                          "Weight ARMCK 2.46\n"
                          "Limit ARMLAB MP 0\n"
                          "Limit ARMLAB 4\n";
    check(computer_players_configure(&state, profile, kBuildLists), "unit split: configure");
    check(computer_players_initialize(&state, host), "unit split: initialize");
    const auto& k = state.players[1].knowledge;
    check(
        k.weight_percent[ARMCK] == 0 && k.weight_locked[ARMCK] == 1,
        "unit split: the weight reads 0 and locks"
    );
    check(
        k.limits[ARMLAB] == 0 && k.limit_locked[ARMLAB] == 1,
        "unit split: the limit reads 0 and locks"
    );
    check(k.weight_percent[ARMPW] == 100, "unit split: the side is untouched");
    computer_players_release(&state);
}

// ai.difficulty-names: the plan keyword, the report label and the menu label a
// difficulty carries follow the names; the baseline names keep 3.1c's.
void test_difficulty_names() {
    using Names = oa::data::match_rules::AiDifficultyNamesNames;
    oa::data::match_rules::AiDifficultyNames swapped{};
    swapped.enabled = true;
    swapped.names = {Names::hard, Names::medium, Names::easy};
    check(oa::data::match_rules::difficulty_name_index({}, 0) == 0, "3.1c: difficulty 0 is easy");
    check(oa::data::match_rules::difficulty_name_index({}, 2) == 2, "3.1c: difficulty 2 is hard");
    check(oa::data::match_rules::difficulty_name_index(swapped, 0) == 2, "swapped: 0 is hard");
    check(oa::data::match_rules::difficulty_name_index(swapped, 1) == 1, "swapped: 1 is medium");
    check(oa::data::match_rules::difficulty_name_index(swapped, 2) == 0, "swapped: 2 is easy");
    check(
        oa::data::match_rules::difficulty_name_index(swapped, 3) == 3, "past the names unchanged"
    );
    check(oa::data::match_rules::difficulty_name_index(swapped, -1) == -1, "negative unchanged");
    oa::data::match_rules::AiDifficultyNames rotated{};
    rotated.names = {Names::medium, Names::hard, Names::easy};
    check(oa::data::match_rules::difficulty_name_index(rotated, 0) == 1, "any order: 0 is medium");

    const char* profile = "plan easy\nlimit ARMMEX 8\nplan hard\nlimit ARMMEX 1\n"
                          "plan medium\nlimit ARMSOLAR 4\n";

    struct Case {
        oa::data::match_rules::AiDifficultyNames names;
        int32_t difficulty;
        int32_t mex_limit;
        int32_t solar_limit;
        const char* label;
    };

    oa::data::match_rules::AiDifficultyNames baseline_on{};
    baseline_on.enabled = true;
    const Case cases[] = {
        {{}, OA_DIFFICULTY_EASY, 8, -1, "'EASY'"},
        {baseline_on, OA_DIFFICULTY_EASY, 8, -1, "'EASY'"},
        {swapped, 0, 1, -1, "'HARD'"},
        {swapped, 1, -1, 4, "'MEDIUM'"},
        {swapped, 2, 8, -1, "'EASY'"},
    };
    const auto scratch = oa::test::make_scratch_directory("oa-ai-difficulty-names");
    for (const auto& c : cases) {
        Fake fake;
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
        auto host = host_for(fake);
        host.difficulty = c.difficulty;
        MatchRules rules{};
        rules.ai.difficulty_names = c.names;
        ComputerPlayers state{};
        state.rules.match = &rules;
        describe_catalog(state);
        check(computer_players_configure(&state, profile, kBuildLists), "names: configure");
        check(computer_players_initialize(&state, host), "names: initialize");
        const auto& k = state.players[1].knowledge;
        check(k.limits[ARMMEX] == c.mex_limit, "names: the plan the difficulty's keyword selects");
        check(k.limits[ARMSOLAR] == c.solar_limit, "names: medium keeps its plan");
        std::FILE* out = oa::platform::open_file(scratch / "weights.txt", "w+b");
        computer_write_report(&state, host, 1, {}, out);
        const auto text = read_all(out);
        std::fclose(out);
        check(
            text.find(std::string("Challenge level: ") + c.label) != std::string::npos,
            "names: the report's label"
        );
        computer_players_release(&state);
    }
    std::error_code removal;
    std::filesystem::remove_all(scratch, removal);
}

// ai.squad5-factory-tick: squad 5 (armed structures) runs the structures task, so an
// armed building with a build list queues picks; in 3.1c it never does.
void test_squad5_factory_tick() {
    for (const bool on : {false, true}) {
        Fake fake;
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        auto host = host_for(fake);
        MatchRules rules{};
        rules.ai.squad5_factory_tick.enabled = on;
        ComputerPlayers state{};
        state.rules.match = &rules;
        describe_catalog(state);
        check(computer_players_configure(&state, "", kBuildLists), "squad 5: configure");
        // An armed factory, as a silo that builds its own stockpile is.
        fake.spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
        fake.rolls.assign(4096, 7);
        check(computer_players_initialize(&state, host), "squad 5: initialize");
        check(
            state.players[1].tasks[5].kind == (on ? TaskKind::structures : TaskKind::idle),
            "squad 5: the task"
        );
        tick_until(state, fake, host, 1, 120);
        check(fake.units[11].squad == static_cast<int32_t>(Squad::armed_structures), "squad 5");
        check((orders_of(fake, "factory", 11) != 0) == on, "squad 5: the armed factory's picks");
        computer_players_release(&state);
    }
}

// ai.factory-tick-filter: skip-busy-background-queue leaves a building whose secondary
// queue holds an order alone; power-toggle energy-use-32 switches buildings by EnergyUse
// (its float's top byte as a signed number, at least 0x42) instead of MakesMetal.
void test_factory_tick_filter() {
    using Toggle = oa::data::match_rules::AiFactoryTickFilterPowerToggle;
    for (const bool skip : {false, true}) {
        Fake fake;
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        auto host = host_for(fake);
        MatchRules rules{};
        rules.ai.factory_tick_filter.enabled = true;
        rules.ai.factory_tick_filter.skip_busy_background_queue = skip;
        ComputerPlayers state{};
        state.rules.match = &rules;
        describe_catalog(state);
        check(computer_players_configure(&state, "", kBuildLists), "filter: configure");
        fake.spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING);
        fake.spawn(12, 1, ARMLAB, 300, 200, OA_UNIT_FLAG_BUILDING);
        fake.holds_secondary[12] = true;
        fake.rolls.assign(4096, 7);
        check(computer_players_initialize(&state, host), "filter: initialize");
        tick_until(state, fake, host, 1, 120);
        check(orders_of(fake, "factory", 11) != 0, "filter: the idle factory picks");
        check(
            (orders_of(fake, "factory", 12) != 0) == !skip,
            "filter: a factory with a secondary order picks only without the filter"
        );
        computer_players_release(&state);
    }

    struct Building {
        uint16_t type;
        int8_t makes_metal;
        float energy_use;
        bool toggles_by_makes_metal;
        bool toggles_by_energy_use;
    };

    // The structures task's toggle switches these buildings off: the player's energy
    // (1000) is not above twice its metal (500).
    const Building buildings[] = {
        {ARMSOLAR, 1, 0.0F, true, false}, // a metal maker using no energy
        {ARMMEX, 0, 32.0F, false, true},
        {ARMLLT, 0, 31.99F, false, false},
        {CORMEX, 0, -40.0F, false, false}, // a negative EnergyUse fails the signed test
    };
    for (const auto toggle : {Toggle::makes_metal, Toggle::energy_use_32}) {
        Fake fake;
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        auto host = host_for(fake);
        MatchRules rules{};
        rules.ai.factory_tick_filter.enabled = true;
        rules.ai.factory_tick_filter.power_toggle = toggle;
        ComputerPlayers state{};
        state.rules.match = &rules;
        describe_catalog(state);
        check(computer_players_configure(&state, "", kBuildLists), "toggle: configure");
        uint16_t slot = 11;
        for (const auto& building : buildings) {
            state.types[building.type].makes_metal = building.makes_metal;
            state.types[building.type].energy_use = building.energy_use;
            fake.spawn(slot, 1, building.type, 200 + slot * 20, 200, OA_UNIT_FLAG_BUILDING);
            ++slot;
        }
        fake.rolls.assign(4096, 7);
        check(computer_players_initialize(&state, host), "toggle: initialize");
        tick_until(state, fake, host, 1, 60);
        slot = 11;
        for (const auto& building : buildings) {
            const bool expected = toggle == Toggle::makes_metal ? building.toggles_by_makes_metal
                                                                : building.toggles_by_energy_use;
            check(
                (orders_of(fake, "off", slot) != 0) == expected, "toggle: which buildings switch"
            );
            ++slot;
        }
        computer_players_release(&state);
    }
}

// ai.builder-withhold-threshold: a capturing builder takes build jobs only while the player
// has fewer build-capable units than the threshold; once idle it still circles the base
// from five.
void test_builder_withhold_threshold() {
    for (const int32_t threshold : {5, 10}) {
        Fake fake;
        fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        auto host = host_for(fake);
        MatchRules rules{};
        rules.ai.builder_withhold_threshold.enabled = threshold != 5;
        rules.ai.builder_withhold_threshold.builders = threshold;
        ComputerPlayers state{};
        state.rules.match = &rules;
        describe_catalog(state);
        check(computer_players_configure(&state, "", kBuildLists), "withhold: configure");
        // The commander's list holds another side's type; it never picks it.
        fake.strength[CORMEX][0] = fake.strength[CORMEX][1] = fake.strength[CORMEX][2] = 0;
        fake.spawn(10, 1, ARMCOM, 200, 200, 0);
        // Six more build-capable units: seven in all.
        for (uint32_t slot = 11; slot < 17; ++slot)
            fake.spawn(slot, 1, ARMCK, 260 + static_cast<int32_t>(slot), 260, 0);
        fake.rolls.assign(4096, 7);
        check(computer_players_initialize(&state, host), "withhold: initialize");
        tick_until(state, fake, host, 1, 200);
        check(state.players[1].knowledge.builder_count == 7, "withhold: seven builders");
        check(
            (orders_of(fake, "build", 10) != 0) == (threshold == 10),
            "withhold: the commander builds only under the threshold"
        );
        check(orders_of(fake, "move", 10) != 0, "withhold: an idle commander circles from five");
        computer_players_release(&state);
    }
}

// ai.attack-wave-size: both strike tasks launch at the wave size; the may-attack floor
// stays 3.
void test_attack_wave_size() {
    for (const int32_t wave : {6, 10}) {
        Fake fake;
        fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        auto host = host_for(fake);
        MatchRules rules{};
        rules.ai.attack_wave_size.enabled = wave != 6;
        rules.ai.attack_wave_size.units = wave;
        ComputerPlayers state{};
        state.rules.match = &rules;
        describe_catalog(state);
        check(computer_players_configure(&state, "", kBuildLists), "wave: configure");
        fake.spawn(1, 0, ARMLLT, 900, 900, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
        fake.spawn(10, 1, ARMSOLAR, 200, 200, OA_UNIT_FLAG_BUILDING);
        for (uint32_t slot = 12; slot < 19; ++slot)
            fake.spawn(
                slot, 1, ARMPW, 300 + static_cast<int32_t>(slot), 300, OA_UNIT_FLAG_HAS_WEAPONS
            );
        fake.rolls.assign(4096, 7);
        check(computer_players_initialize(&state, host), "wave: initialize");
        const auto& tasks = state.players[1].tasks;
        check(
            tasks[2].launch_size == wave && tasks[6].launch_size == wave,
            "wave: both strike tasks launch at the wave size"
        );
        check(tasks[2].min_size == 3 && tasks[6].min_size == 3, "wave: the floor stays 3");
        tick_until(state, fake, host, 1, 301);
        check(fake.squads[{1, 2}].size() == 7, "wave: seven recruits");
        const bool attacked = orders_of(fake, "attack", 12) != 0;
        check(attacked == (wave == 6), "wave: seven attack at 6 and rally home at 10");
        computer_players_release(&state);
    }
}

/// Extends the fake's canonical unit storage without its fixed 64-slot order arrays.
struct RecruitFake : Fake {
    std::vector<std::string> running;
    std::vector<uint16_t> joined;
    std::vector<uint16_t> owned_targets;
    std::vector<uint16_t> prior_reads;
    bool prior_read_after_callback{};

    /// Rebinds the canonical world after growing its owned unit vector.
    explicit RecruitFake(uint32_t count) : running(count, "idle"), owned_targets(count) {
        units.resize(count);
        world.units = units.data();
        world.unit_slot_count = count;
    }

    /// Returns the extended fake from the original host's base-class context.
    static RecruitFake& self(void* context) { return *static_cast<RecruitFake*>(fake_of(context)); }

    /// Logs native callback order and keeps large-slot order state inside bounds.
    ComputerHost host() {
        auto result = host_for(*this);
        result.primary_order = [](void* context, uint16_t unit, uint8_t* preserve, uint8_t* flags) {
            if (preserve != nullptr)
                *preserve = 0;
            if (flags != nullptr)
                *flags = 0;
            return self(context).running[unit] != "idle";
        };
        result.secondary_order = [](void*, uint16_t) { return false; };
        result.set_squad = [](void* context, uint16_t unit, Squad squad) {
            auto& fake = self(context);
            fake.joined.push_back(unit);
            fake.set_squad(unit, static_cast<int>(squad));
        };
        result.order_attack = [](void* context, uint16_t unit, uint16_t target) {
            auto& fake = self(context);
            fake.orders.push_back({"attack", unit, target, {}, false});
            fake.running[unit] = "attack";
            fake.owned_targets[unit] = target;
            return true;
        };
        result.order_move =
            [](void* context, uint16_t unit, const oa::FixedVec3* point, bool queue) {
                auto& fake = self(context);
                fake.orders.push_back({"move", unit, 0, *point, queue});
                fake.running[unit] = "move";
                fake.owned_targets[unit] = 0;
                return true;
            };
        result.order_patrol =
            [](void* context, uint16_t unit, const oa::FixedVec3* point, bool queue) {
                auto& fake = self(context);
                fake.orders.push_back({"patrol", unit, 0, *point, queue});
                fake.running[unit] = "patrol";
                fake.owned_targets[unit] = 0;
                return true;
            };
        return result;
    }
};

/// Holds a real built-in strike tick over hundreds of waiting canonical units.
struct RecruitRun {
    static constexpr uint32_t distributed_version = 7;
    static constexpr uint8_t player = 1;
    static constexpr uint16_t first = 10;
    static constexpr uint32_t refresh = 300;
    RecruitFake fake;
    MatchRules rules{};
    ComputerPlayers state{};
    ComputerHost host{};
    Squad destination{}, source{};

    /// Clumps every candidate at the existing squad's centre, within safe integer ranges.
    RecruitRun(uint32_t version, uint32_t sources, uint32_t existing, bool naval = false)
        : fake(first + sources + existing + 2),
          destination(naval ? Squad::naval_strike : Squad::land_strike),
          source(naval ? Squad::navy : Squad::land_army) {
        fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, first - 1);
        fake.player(player, OA_PLAYER_STATUS_COMPUTER, first, first + sources + existing);
        oa::data::enhanced_mode::apply_alias(
            rules.setup.enhanced_mode, static_cast<uint32_t>(version)
        );
        state.rules.match = &rules;
        state.features = oa::data::enhanced_mode::version_features(version);
        describe_catalog(state);
        host = fake.host();
        check(computer_players_configure(&state, "", kBuildLists), "recruit budget: configure");
        auto& enemy =
            fake.spawn(1, 0, ARMLLT, 900, 900, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
        enemy.health = 1000;
        for (uint32_t index = 0; index < sources + existing; ++index) {
            const auto slot = static_cast<uint16_t>(first + index);
            auto& unit =
                fake.spawn(slot, player, naval ? ARMPT : ARMPW, 300, 300, OA_UNIT_FLAG_HAS_WEAPONS);
            unit.id = slot;
            unit.health = 1000;
            fake.set_squad(slot, static_cast<int>(index < existing ? destination : source));
            fake.running[slot] = index < existing ? "attack" : index % 2 == 0 ? "patrol" : "idle";
        }
        check(computer_players_initialize(&state, host), "recruit budget: initialize");
        auto& controller = state.players[player];
        controller.sort_countdown = 100000;
        for (auto& task : controller.tasks)
            task.kind = TaskKind::none;
        controller.tasks[static_cast<uint32_t>(destination)] = {
            TaskKind::strike, destination, refresh, 0, 1, 20000, source, 1
        };
        fake.orders.clear();
        fake.joined.clear();
    }

    /// Frees the catalog and controller tables after their host has finished.
    ~RecruitRun() { computer_players_release(&state); }

    /// Returns the waiting source in the native host's current iteration order.
    std::vector<uint16_t> waiting() { return fake.squads[{player, static_cast<int>(source)}]; }

    /// Returns current strike members without changing their native vector order.
    std::vector<uint16_t> members() { return fake.squads[{player, static_cast<int>(destination)}]; }

    /// Runs the actual ordinary scheduled task dispatch.
    void tick(uint32_t at) {
        fake.world.game.tick = at;
        computer_player_tick_orders(&state, host, player);
    }

    /// Checks stable admissions, unjoined order state and slot-ordered attack callbacks.
    void dispatch(uint32_t at, std::size_t admitted) {
        const auto candidates = waiting();
        const auto previous_members = members();
        const auto previous_orders = fake.running;
        const auto random_before = fake.roll;
        fake.orders.clear();
        fake.joined.clear();
        tick(at);
        check(fake.joined.size() == admitted, "recruit budget: exact admission count");
        check(admitted <= candidates.size(), "recruit budget: enough live candidates");
        if (admitted <= candidates.size()) {
            std::vector<uint16_t> expected;
            if (previous_members.empty() && admitted != 0) {
                // Seeding takes the first source member. The native host's
                // swap/pop puts its last member first for the following scan.
                expected.push_back(candidates.front());
                if (admitted > 1)
                    expected.push_back(candidates.back());
                for (std::size_t index = 1; expected.size() < admitted; ++index)
                    expected.push_back(candidates[index]);
            } else {
                expected.assign(candidates.begin(), candidates.begin() + admitted);
            }
            check(fake.joined == expected, "recruit budget: source iteration order preserved");
        }
        const auto current_members = members();
        check(
            current_members.size() == previous_members.size() + admitted,
            "recruit budget: every existing strike member remains"
        );
        for (const auto slot : previous_members)
            check(
                std::find(current_members.begin(), current_members.end(), slot) !=
                    current_members.end(),
                "recruit budget: an existing member was not removed"
            );
        for (const auto slot : waiting())
            check(
                fake.running[slot] == previous_orders[slot],
                "recruit budget: unrecruited patrol and idle orders are unchanged"
            );
        std::vector<uint16_t> attacks;
        for (const auto& order : fake.orders) {
            check(
                order.kind == "attack" && order.arg == 1 && !order.queue,
                "recruit budget: only normal attacks are dispatched"
            );
            attacks.push_back(order.unit);
        }
        auto expected_attacks = current_members;
        std::sort(expected_attacks.begin(), expected_attacks.end());
        check(
            attacks == expected_attacks, "recruit budget: attack callbacks retain unit-slot order"
        );
        check(fake.roll == random_before, "recruit budget: no scheduling random draw added");
        check(
            state.players[player].tasks[static_cast<uint32_t>(destination)].next_tick ==
                at + refresh,
            "recruit budget: native strike deadline unchanged"
        );
    }
};

/// Places eight compact strike groups opposite their own nearby enemy.
struct CohortRun : RecruitRun {
    static constexpr uint32_t cohort_version = 10;
    static constexpr uint32_t groups = 8;
    static constexpr uint32_t members_per_group = 4;
    static constexpr int32_t fixed_one = 65536;

    /// Keeps the whole existing force while isolating target assignment from recruitment.
    explicit CohortRun(uint32_t version = cohort_version, bool long_x = false)
        : RecruitRun(version, 0, groups * members_per_group) {
        host.map_cells_x = host.map_cells_z = 1024;
        state.players[player].tasks[static_cast<uint32_t>(destination)].merge_radius = 1000000;
        constexpr std::array<int32_t, members_per_group> offset{-3, -1, 1, 3};
        for (uint32_t group = 0; group < groups; ++group) {
            const int32_t across = group % 2 == 0 ? 1000 : 3000;
            const int32_t along = 1000 + static_cast<int32_t>(group / 2) * 2000;
            enemy(
                static_cast<uint16_t>(group + 1),
                long_x ? along : across + 400,
                long_x ? across + 400 : along
            );
            for (uint32_t member = 0; member < members_per_group; ++member) {
                const auto slot = first + group * members_per_group + member;
                auto& unit = fake.units[slot];
                const int32_t x = (long_x ? along : across) + offset[member];
                const int32_t z =
                    (long_x ? across : along) + offset[members_per_group - 1 - member];
                unit.position = {x * fixed_one, 0, z * fixed_one};
            }
        }
    }

    /// Replaces a canonical enemy slot and gives its current record positive health.
    void enemy(uint16_t slot, int32_t x, int32_t z) {
        auto& unit =
            fake.spawn(slot, 0, ARMLLT, x, z, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
        unit.id = slot;
        unit.health = 1000;
    }

    /// Runs one ordinary scheduled dispatch and returns its slot-ordered attack assignments.
    std::vector<std::pair<uint16_t, uint16_t>> assign(uint32_t at) {
        fake.orders.clear();
        fake.joined.clear();
        const auto random_before = fake.roll;
        tick(at);
        check(fake.roll == random_before, "cohort targets: no random draws");
        check(fake.joined.empty(), "cohort targets: the existing force recruits no waiting units");
        std::vector<std::pair<uint16_t, uint16_t>> result;
        for (const auto& order : fake.orders) {
            check(
                order.kind == "attack" && !order.queue,
                "cohort targets: ordinary unqueued attack callbacks"
            );
            check(
                order.unit >= first && order.unit < first + groups * members_per_group,
                "cohort targets: only current members receive attack callbacks"
            );
            result.emplace_back(order.unit, order.arg);
        }
        check(
            std::is_sorted(result.begin(), result.end()),
            "cohort targets: callbacks stay in canonical unit-slot order"
        );
        check(
            std::adjacent_find(
                result.begin(),
                result.end(),
                [](const auto& left, const auto& right) { return left.first == right.first; }
            ) == result.end(),
            "cohort targets: each member receives at most one attack callback"
        );
        return result;
    }
};

/// Preserves each earlier preset's single nearest-enemy assignment.
void test_earlier_strikes_keep_one_target() {
    for (uint32_t version = 0; version <= 8; ++version) {
        CohortRun run(version);
        const auto orders = run.assign(300);
        check(orders.size() == 32, "cohort targets: earlier presets order the whole strike");
        for (const auto& [unit, target] : orders) {
            (void)unit;
            // Slots 3 and 5 are equally close to the whole-force centroid;
            // the native first strict minimum is slot 3.
            check(target == 3, "cohort targets: earlier presets keep their centroid target");
        }
    }
    CohortRun disabled;
    disabled.rules.setup.enhanced_mode.enabled = false;
    disabled.state.features = {};
    const auto disabled_orders = disabled.assign(300);
    check(disabled_orders.size() == 32, "cohort targets: disabled mode orders the whole strike");
    for (const auto& [unit, target] : disabled_orders) {
        (void)unit;
        check(target == 3, "cohort targets: disabled Enhanced Mode keeps the native target");
    }
}

/// Keeps nearby members together while different fronts receive their nearby enemies.
void test_spatial_strikes_use_local_targets() {
    for (const bool long_x : {false, true}) {
        CohortRun run(CohortRun::cohort_version, long_x);
        const auto orders = run.assign(300);
        check(orders.size() == 32, "cohort targets: every strike member is assigned");
        for (const auto& [unit, target] : orders) {
            const auto group = (unit - CohortRun::first) / CohortRun::members_per_group;
            check(target == group + 1, "cohort targets: each front chooses its nearby enemy");
        }
        check(
            run.assign(600) == orders,
            "cohort targets: unchanged positions keep the same target assignments"
        );
    }
}

/// Shares a nearby enemy cluster instead of broadcasting its first unit to every group.
void test_cohort_target_pressure_spreads_a_cluster() {
    for (const int32_t enemy_x : {9000, 30000}) {
        CohortRun run;
        run.host.map_cells_x = 2048;
        for (uint16_t slot = 1; slot <= CohortRun::groups; ++slot)
            run.enemy(slot, enemy_x + slot * 4, 4000);
        const auto orders = run.assign(300);
        std::map<uint16_t, uint32_t> counts;
        for (const auto& [unit, target] : orders) {
            (void)unit;
            ++counts[target];
        }
        check(orders.size() == 32, "cohort pressure: the whole force receives orders");
        check(counts.size() == CohortRun::groups, "cohort pressure: nearby alternatives are used");
        for (const auto& [target, count] : counts) {
            check(
                target >= 1 && target <= CohortRun::groups, "cohort pressure: a live enemy target"
            );
            check(
                count == CohortRun::members_per_group,
                "cohort pressure: one nearby group per target"
            );
        }
        if (orders.size() == CohortRun::groups * CohortRun::members_per_group)
            for (uint32_t group = 0; group < CohortRun::groups; ++group) {
                const auto first = group * CohortRun::members_per_group;
                for (uint32_t member = 1; member < CohortRun::members_per_group; ++member)
                    check(
                        orders[first + member].second == orders[first].second,
                        "cohort pressure: nearby members keep one shared group target"
                    );
            }
    }
}

/// Keeps pressure costs beyond 32 bits from favouring an already crowded target.
void test_cohort_pressure_keeps_wide_costs() {
    CohortRun run;
    run.host.map_cells_x = run.host.map_cells_z = 2048;
    for (uint16_t slot = 1; slot <= CohortRun::groups; ++slot)
        run.fake.units[slot].flags = 0;
    run.enemy(1, 32700, 32700);
    run.enemy(2, 32704, 32704);
    const auto orders = run.assign(300);
    std::map<uint16_t, uint32_t> counts;
    for (const auto& [unit, target] : orders) {
        (void)unit;
        ++counts[target];
    }
    check(orders.size() == 32, "cohort wide costs: every member receives a target");
    check(
        counts.size() == 2 && counts[1] == 16 && counts[2] == 16,
        "cohort wide costs: equal nearby alternatives each receive half the force"
    );
}

/// Keeps the existing widened and submerged filters in land and naval cohort searches.
void test_cohort_targets_keep_water_filter() {
    for (const bool naval : {false, true}) {
        RecruitRun run(CohortRun::cohort_version, 0, 8, naval);
        run.rules.ai.nearest_enemy_filter.enabled = true;
        run.rules.ai.nearest_enemy_filter.rules =
            oa::data::match_rules::AiNearestEnemyFilterRules::skip_submerged;
        auto& sub = run.fake.spawn(1, 0, ARMPT, 400, 300, OA_UNIT_FLAG_HAS_WEAPONS);
        sub.health = 1000;
        constexpr uint32_t submerged = 3;
        std::memcpy(sub.last_occupy_code, &submerged, sizeof submerged);
        constexpr uint32_t move_rate_tier_two = 0x8u;
        run.fake.spawn(2, 0, ARMPW, 500, 300, OA_UNIT_FLAG_HAS_WEAPONS | move_rate_tier_two)
            .health = 1000;
        run.fake.spawn(3, 0, ARMLLT, 600, 300, OA_UNIT_FLAG_HAS_WEAPONS).health = 1000;
        const auto random_before = run.fake.roll;
        run.tick(300);
        check(run.fake.roll == random_before, "cohort water: no random draws");
        check(run.fake.orders.size() == 8, "cohort water: every member receives an attack");
        for (const auto& order : run.fake.orders)
            check(
                order.kind == "attack" && order.arg == (naval ? 1 : 3),
                "cohort water: land skips submerged targets, naval strike retains them"
            );
    }
}

/// Uses the only valid enemy for every group, and emits no attacks when none remain.
void test_cohort_target_filtering_and_empty_search() {
    CohortRun run;
    run.fake.units.resize(64);
    run.fake.running.resize(64);
    run.fake.world.units = run.fake.units.data();
    run.fake.world.unit_slot_count = static_cast<uint32_t>(run.fake.units.size());
    run.fake.player(2, OA_PLAYER_STATUS_LOCAL, 50, 51);
    run.fake.allies[CohortRun::player][2] = true;
    auto& ally = run.fake.spawn(50, 2, ARMLLT, 2000, 4000, OA_UNIT_FLAG_HAS_WEAPONS);
    ally.health = 1000;
    for (uint16_t slot = 1; slot <= CohortRun::groups; ++slot)
        run.enemy(slot, 2000, 4000);
    run.fake.units[1].health = 0;
    run.fake.units[2].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    run.fake.units[3].state_flags |= OA_UNIT_STATE_CLOAKED;
    run.fake.units[4].flags |= OA_UNIT_FLAG_NOT_SELECTABLE;
    constexpr uint32_t occupied_state_two = 2;
    run.fake.units[5].flags |= occupied_state_two;
    run.fake.units[6].flags &= ~OA_UNIT_FLAG_LIVE;
    run.fake.units[8].health = -1;
    const auto orders = run.assign(300);
    check(orders.size() == 32, "cohort filters: every member uses the remaining enemy");
    for (const auto& [unit, target] : orders) {
        (void)unit;
        check(
            target == 7, "cohort filters: dying, cloaked, unavailable and allied units are skipped"
        );
    }
    run.fake.units[7].health = 0;
    check(run.assign(600).empty(), "cohort filters: no attack order without a valid enemy");
    RecruitRun empty(CohortRun::cohort_version, 0, 0);
    const auto random_before = empty.fake.roll;
    empty.tick(300);
    check(empty.fake.orders.empty(), "cohort filters: an empty strike issues no orders");
    check(
        empty.fake.roll == random_before, "cohort filters: an empty strike draws no random value"
    );
}

/// Rechecks death, cloak and replacement records instead of retaining candidate slots.
void test_cohort_targets_reread_live_slots() {
    CohortRun run;
    for (uint16_t slot = 2; slot <= CohortRun::groups; ++slot)
        run.fake.units[slot].flags = 0;
    const auto first = run.assign(300);
    check(first.size() == 32, "cohort live slots: every member receives the first live target");
    for (const auto& [unit, target] : first) {
        (void)unit;
        check(target == 1, "cohort live slots: the first live enemy is selected");
    }
    run.fake.units[1].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    run.enemy(2, 3000, 4000);
    const auto replacement = run.assign(600);
    check(replacement.size() == 32, "cohort live slots: the replacement receives every group");
    for (const auto& [unit, target] : replacement) {
        (void)unit;
        check(target == 2, "cohort live slots: the dying former target is dropped");
    }
    run.enemy(1, 2000, 4000);
    run.fake.units[1].id = 1234;
    run.fake.units[1].state_flags |= OA_UNIT_STATE_CLOAKED;
    const auto cloaked = run.assign(900);
    check(cloaked.size() == 32, "cohort live slots: the uncloaked target remains usable");
    for (const auto& [unit, target] : cloaked) {
        (void)unit;
        check(target == 2, "cohort live slots: a cloaked reused slot is not a cached candidate");
    }
    run.fake.units[1].state_flags &= ~OA_UNIT_STATE_CLOAKED;
    run.fake.units[2].health = 0;
    const auto returned = run.assign(1200);
    check(returned.size() == 32, "cohort live slots: the fresh record becomes eligible");
    for (const auto& [unit, target] : returned) {
        (void)unit;
        check(target == 1, "cohort live slots: the current record is selected after replacement");
    }
}

/// Keeps assignments independent of the squad list's swap-and-pop order.
void test_cohort_targets_keep_canonical_callback_order() {
    CohortRun normal;
    CohortRun shuffled;
    auto& members =
        shuffled.fake.squads[{CohortRun::player, static_cast<int>(shuffled.destination)}];
    std::reverse(members.begin(), members.end());
    check(
        normal.assign(300) == shuffled.assign(300),
        "cohort target order: membership vector order does not change commands"
    );
}

/// Resolves equal costs by canonical slot rather than the enemy player's iteration order.
void test_cohort_targets_break_ties_by_slot() {
    CohortRun run;
    for (uint16_t slot = 1; slot <= CohortRun::groups; ++slot)
        run.fake.units[slot].flags = 0;
    run.fake.player(0, OA_PLAYER_STATUS_LOCAL, 2, 3);
    run.fake.player(2, OA_PLAYER_STATUS_LOCAL, 1, 1);
    auto& later = run.fake.spawn(1, 2, ARMLLT, 2000, 4000, OA_UNIT_FLAG_HAS_WEAPONS);
    later.health = 1000;
    run.enemy(2, 2000, 4000);
    const auto orders = run.assign(300);
    check(orders.size() == 32, "cohort ties: all members receive a live equal-cost target");
    if (!orders.empty())
        check(orders.front().second == 1, "cohort ties: the first cohort takes the lower slot");
}

/// Refreshes only selected members without disturbing ordinary task clocks or recruitment.
void test_masked_cohort_refresh_keeps_task_state() {
    for (const uint32_t version : {uint32_t{8}, CohortRun::cohort_version}) {
        CohortRun run(version);
        run.fake.world.game.tick = 100;
        const auto members = run.members();
        const auto random_before = run.fake.roll;
        const auto& task =
            run.state.players[CohortRun::player].tasks[static_cast<uint32_t>(run.destination)];
        const auto deadline = task.next_tick;
        const auto attacking = task.attacking;
        std::array<uint64_t, strike_refresh_mask_words> sources{};
        const auto refresh = [&](std::span<const uint64_t> selected) {
            return computer_player_refresh_strike_targets(
                &run.state, run.host, CohortRun::player, run.destination, selected
            );
        };
        check(!refresh(sources), "cohort refresh: an empty selection is a no-op");
        check(!refresh({}), "cohort refresh: an absent mask is not a whole-squad refresh");
        constexpr uint16_t first = CohortRun::first;
        constexpr uint16_t last = first + CohortRun::groups * CohortRun::members_per_group - 1;
        for (const auto slot : {uint16_t{1}, first, last})
            sources[slot / strike_refresh_word_bits] |= uint64_t{1}
                                                        << (slot % strike_refresh_word_bits);
        check(
            !refresh(std::span<const uint64_t>(sources).first(1)),
            "cohort refresh: a truncated mask is refused"
        );
        check(
            refresh(sources) == (version == CohortRun::cohort_version),
            "cohort refresh: only the cohort preset accepts early attack callbacks"
        );
        if (version == CohortRun::cohort_version) {
            check(run.fake.orders.size() == 2, "cohort refresh: only two selected members ordered");
            if (run.fake.orders.size() == 2)
                check(
                    run.fake.orders[0].unit == first && run.fake.orders[0].arg == 1 &&
                        run.fake.orders[1].unit == last && run.fake.orders[1].arg == 8,
                    "cohort refresh: selected fronts keep their ordinary spatial targets"
                );
        } else {
            check(run.fake.orders.empty(), "cohort refresh: earlier presets issue no orders");
        }
        check(
            task.next_tick == deadline && task.attacking == attacking && run.members() == members &&
                run.fake.joined.empty() && run.fake.roll == random_before,
            "cohort refresh: task deadline, attack state, membership and random stream unchanged"
        );
    }
}

/// Places one member near the edge of a broad group with two selected enemy fronts.
struct FrontRun : RecruitRun {
    static constexpr uint32_t members_count = 7;
    static constexpr uint16_t boundary = first + 3;
    static constexpr int32_t fixed_one = 65536;

    /// Keeps the nearby anchors in one group and distant anchors in separate groups.
    explicit FrontRun(uint32_t version = CohortRun::cohort_version)
        : RecruitRun(version, 0, members_count) {
        host.map_cells_x = host.map_cells_z = 1024;
        state.players[player].tasks[static_cast<uint32_t>(destination)].merge_radius = 1000000;
        for (uint16_t slot = first; slot < boundary; ++slot)
            place(slot, 1004, 1000);
        place(boundary, 1000, 1000);
        place(first + 4, 1000, 601);
        place(first + 5, 1000, 1399);
        place(first + 6, 1100, 1399);
        enemy(1, 1001, 1002);
        enemy(2, 1000, 1002);
    }

    /// Moves one canonical record to whole-world coordinates.
    void place(uint16_t slot, int32_t x, int32_t z) {
        fake.units[slot].position = {x * fixed_one, 0, z * fixed_one};
    }

    /// Replaces one live canonical enemy record without retaining a target handle.
    void enemy(uint16_t slot, int32_t x, int32_t z) {
        auto& unit =
            fake.spawn(slot, 0, ARMLLT, x, z, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
        unit.id = slot;
        unit.health = 1000;
    }

    /// Dispatches the actual task and checks its callback order and random stream.
    std::vector<std::pair<uint16_t, uint16_t>> assign(uint32_t at) {
        fake.orders.clear();
        const auto random_before = fake.roll;
        tick(at);
        check(fake.roll == random_before, "selected fronts: no random draws");
        check(fake.joined.empty(), "selected fronts: no waiting recruits");
        std::vector<std::pair<uint16_t, uint16_t>> result;
        for (const auto& order : fake.orders) {
            check(
                order.kind == "attack" && !order.queue, "selected fronts: ordinary attack callbacks"
            );
            result.emplace_back(order.unit, order.arg);
        }
        check(result.size() == members_count, "selected fronts: every existing member ordered");
        for (std::size_t index = 0; index < result.size(); ++index)
            check(
                result[index].first == first + index,
                "selected fronts: callbacks stay in canonical unit-slot order"
            );
        return result;
    }

    /// Returns one actor's dispatched target, or zero when its callback is absent.
    static uint16_t
    target(const std::vector<std::pair<uint16_t, uint16_t>>& orders, uint16_t slot) {
        const auto found = std::find_if(orders.begin(), orders.end(), [=](const auto& order) {
            return order.first == slot;
        });
        return found == orders.end() ? 0 : found->second;
    }
};

/// Redirects a broad group's boundary member only to an already selected closer front.
void test_selected_front_fallback_crosses_coarse_group_boundary() {
    FrontRun run;
    for (uint16_t slot = FrontRun::first; slot < FrontRun::boundary; ++slot)
        run.place(slot, 1000, 1000);
    run.place(FrontRun::boundary, 1000, 1099);
    for (uint16_t slot = FrontRun::first + 4; slot < FrontRun::first + FrontRun::members_count;
         ++slot)
        run.place(slot, 1000, 1399);
    run.enemy(1, 1000, 1000);
    run.enemy(2, 1000, 1100);
    run.enemy(3, 1000, 1099);
    const auto orders = run.assign(300);
    // The inclusive 1000..1399 extent puts 1099 in the first band's
    // centroid at 1024, whose own target is 1. Only targets 1 and 2
    // are selected by the occupied groups; the exact-position enemy 3 is not.
    check(
        FrontRun::target(orders, FrontRun::boundary) == 2,
        "selected fronts: the boundary member uses the nearer selected front"
    );
    check(
        FrontRun::target(orders, FrontRun::first) == 1 &&
            FrontRun::target(orders, FrontRun::first + 4) == 2,
        "selected fronts: near and distant anchors retain their selected fronts"
    );
    check(
        std::none_of(
            orders.begin(), orders.end(), [](const auto& order) { return order.second == 3; }
        ),
        "selected fronts: an unselected enemy is not a per-member candidate"
    );
}

/// Preserves group coherence at equality and changes only materially longer assignments.
void test_selected_front_fallback_keeps_exact_distance_threshold() {
    struct Case {
        int32_t own_x, other_x, target_z;
        uint16_t own_slot, expected;
    };

    // At the boundary actor (1000,1000), the squared distances are
    // 10:9, 5:4 and 13:10. Equality at 4*5 == 5*4 keeps the group's
    // target, including when the nearer alternate has a lower slot.
    constexpr Case cases[] = {
        {1001, 1000, 1003, 1, 1},
        {1001, 1000, 1002, 1, 1},
        {1001, 1000, 1002, 2, 2},
        {1002, 1001, 1003, 1, 2},
    };
    for (const auto& item : cases) {
        FrontRun run;
        run.enemy(item.own_slot, item.own_x, item.target_z);
        run.enemy(3 - item.own_slot, item.other_x, item.target_z);
        const auto orders = run.assign(300);
        check(
            FrontRun::target(orders, FrontRun::boundary) == item.expected,
            "selected fronts: the strict squared-distance threshold is preserved"
        );
        check(
            FrontRun::target(orders, FrontRun::first) == item.own_slot,
            "selected fronts: nearby anchors retain their cohort target"
        );
    }
}

/// Resolves equally close alternate fronts by slot while excluding an ineligible enemy.
void test_selected_front_fallback_keeps_candidate_filter_and_tie() {
    FrontRun run;
    run.enemy(1, 1002, 1000);
    run.enemy(2, 1000, 1001);
    run.enemy(3, 1000, 999);
    run.enemy(4, 1000, 1000);
    run.fake.units[4].health = 0;
    const auto orders = run.assign(300);
    // The southern, middle and northern groups choose 3, 1 and 2.
    // The boundary actor is equally close to 2 and 3, although 3 is selected first.
    check(
        FrontRun::target(orders, FrontRun::boundary) == 2,
        "selected fronts: equally close alternatives use the lower canonical slot"
    );
    check(
        FrontRun::target(orders, FrontRun::first + 4) == 3 &&
            FrontRun::target(orders, FrontRun::first + 5) == 2,
        "selected fronts: both alternate fronts are selected by their nearby groups"
    );
    check(
        std::none_of(
            orders.begin(), orders.end(), [](const auto& order) { return order.second == 4; }
        ),
        "selected fronts: an ineligible exact-position enemy stays excluded"
    );
}

/// Leaves earlier presets on their single whole-strike target and random stream.
void test_selected_front_fallback_preserves_earlier_presets() {
    for (uint32_t version = 0; version <= 8; ++version) {
        FrontRun run(version);
        run.enemy(1, 1002, 1002);
        const auto orders = run.assign(300);
        for (const auto& [unit, target] : orders) {
            (void)unit;
            check(target == 1, "selected fronts: earlier presets keep one centroid target");
        }
    }
}

/// Keeps existing source-owned targets while exercising the actual cohort policy.
struct RetentionRun : RecruitRun {
    static constexpr uint32_t retention_version = 11;
    static constexpr int32_t fixed_one = 65536;

    /// Starts coincident members with a fresh optimum and a comparable prior front.
    explicit RetentionRun(
        uint32_t version = retention_version,
        uint32_t existing = 8,
        uint32_t sources = 0,
        bool naval = false
    )
        : RecruitRun(version, sources, existing, naval) {
        host.map_cells_x = host.map_cells_z = 2048;
        state.players[player].tasks[static_cast<uint32_t>(destination)].merge_radius = 1000000;
        for (uint32_t index = 0; index < existing + sources; ++index)
            place(static_cast<uint16_t>(first + index), 1000, 1000);
        enemy(1, 1002, 1002);
        enemy(2, 1003, 1000);
        host.owned_attack_target = read_owned_target;
    }

    /// Reports existing source-owned intent without changing any fake order state.
    static uint16_t read_owned_target(void* context, uint8_t owner, uint16_t slot) {
        auto& fake = RecruitFake::self(context);
        fake.prior_reads.push_back(slot);
        fake.prior_read_after_callback |= !fake.orders.empty();
        if (slot >= fake.units.size() || fake.units[slot].owner_index != owner ||
            fake.running[slot] != "attack")
            return 0;
        return fake.owned_targets[slot];
    }

    /// Moves a canonical member without changing its squad or existing order.
    void place(uint16_t slot, int32_t x, int32_t z) {
        fake.units[slot].position = {x * fixed_one, 0, z * fixed_one};
    }

    /// Supplies an eligible current enemy record to the ordinary enemy walk.
    void enemy(uint16_t slot, int32_t x, int32_t z) {
        auto& unit =
            fake.spawn(slot, 0, ARMLLT, x, z, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
        unit.id = slot;
        unit.health = 1000;
    }

    /// Supplies one compatible target, or no prior, for each existing member.
    void prior(uint16_t slot, uint16_t target) { fake.owned_targets[slot] = target; }

    /// Runs a periodic dispatch while checking timing, reads, order and randomness.
    std::vector<std::pair<uint16_t, uint16_t>>
    assign(uint32_t at = refresh, uint32_t admitted = 0, bool targets_available = true) {
        fake.orders.clear();
        fake.joined.clear();
        fake.prior_reads.clear();
        fake.prior_read_after_callback = false;
        const auto random_before = fake.roll;
        tick(at);
        check(fake.roll == random_before, "cohort retention: no random draw");
        check(fake.joined.size() == admitted, "cohort retention: ordinary recruitment only");
        check(
            !fake.prior_read_after_callback,
            "cohort retention: every prior is read before any attack callback"
        );
        check(
            fake.prior_reads.size() <= 2 * members().size(),
            "cohort retention: at most two head reads per current member"
        );
        check(
            state.players[player].tasks[static_cast<uint32_t>(destination)].next_tick ==
                at + refresh,
            "cohort retention: native deadline advances by the ordinary interval"
        );
        std::vector<std::pair<uint16_t, uint16_t>> result;
        for (const auto& order : fake.orders) {
            check(
                order.kind == "attack" && !order.queue, "cohort retention: normal attack callback"
            );
            result.emplace_back(order.unit, order.arg);
        }
        auto expected = members();
        std::sort(expected.begin(), expected.end());
        if (!targets_available)
            expected.clear();
        check(
            result.size() == expected.size(), "cohort retention: one callback per dispatched source"
        );
        for (std::size_t index = 0; index < std::min(result.size(), expected.size()); ++index)
            check(
                result[index].first == expected[index], "cohort retention: canonical callback order"
            );
        return result;
    }
};

/// Preserves a true majority rather than the nearest minority or a weak plurality.
void test_cohort_retention_requires_a_verified_majority() {
    for (const uint32_t prior_count : {uint32_t{4}, uint32_t{5}, uint32_t{6}}) {
        RetentionRun run;
        for (uint32_t index = 0; index < 8; ++index)
            run.prior(RetentionRun::first + index, index < prior_count ? 2 : index == 7 ? 0 : 1);
        const auto orders = run.assign();
        for (const auto& [source, target] : orders) {
            (void)source;
            check(
                target == (prior_count > 4 ? 2 : 1),
                "cohort retention: more than half of all members must support the prior"
            );
        }
        check(
            run.fake.prior_reads.size() == 16, "cohort retention: candidate and confirming walks"
        );
    }
    RetentionRun busy;
    for (uint32_t index = 0; index < 8; ++index)
        busy.prior(RetentionRun::first + index, 2);
    for (uint32_t index = 0; index < 4; ++index)
        busy.fake.running[RetentionRun::first + index] = "busy";
    for (const auto& [source, target] : busy.assign()) {
        (void)source;
        check(target == 1, "cohort retention: unsupported heads still count in the denominator");
    }
}

/// Newly admitted members do not inherit a small established subgroup's prior.
void test_cohort_retention_counts_new_recruits_without_priors() {
    RetentionRun run(RetentionRun::retention_version, 5, 3);
    for (uint32_t index = 0; index < 4; ++index)
        run.prior(RetentionRun::first + index, 2);
    run.prior(RetentionRun::first + 4, 1);
    for (const auto& [source, target] : run.assign(300, 3)) {
        (void)source;
        check(
            target == 1, "cohort retention: four prior votes among eight members are insufficient"
        );
    }
}

/// Uses all current head targets without truncating their distinct-target history.
void test_cohort_retention_handles_more_than_eight_prior_targets() {
    RetentionRun run(RetentionRun::retention_version, 20);
    for (uint16_t target = 1; target <= 9; ++target)
        run.enemy(target, 1100 + target * 8, 1200);
    run.enemy(1, 1002, 1002);
    run.enemy(8, 1003, 1000);
    for (uint32_t index = 0; index < 12; ++index)
        run.prior(RetentionRun::first + index, 8);
    uint16_t actor = RetentionRun::first + 12;
    for (uint16_t target = 1; target <= 9; ++target)
        if (target != 8)
            run.prior(actor++, target);
    auto& membership = run.fake.squads[{RetentionRun::player, static_cast<int>(run.destination)}];
    std::reverse(membership.begin(), membership.end());
    for (const auto& [source, target] : run.assign()) {
        (void)source;
        check(
            target == 8, "cohort retention: verified majority survives nine distinct prior slots"
        );
    }
}

/// Keeps the exact 12.5% weighted-cost boundary and charges retained earlier choices.
void test_cohort_retention_keeps_exact_weighted_margin_and_pressure() {
    for (const bool just_over : {false, true}) {
        RetentionRun run;
        constexpr std::array<std::array<int32_t, 2>, 8> positions{
            {{-100, -300},
             {0, -300},
             {-100, -200},
             {0, -200},
             {-100, -100},
             {0, -100},
             {-100, 0},
             {0, 0}}
        };
        for (uint16_t index = 0; index < positions.size(); ++index)
            run.place(
                RetentionRun::first + index, 1000 + positions[index][0], 1000 + positions[index][1]
            );
        if (just_over)
            run.place(RetentionRun::first + 6, 900, 930);
        run.enemy(1, just_over ? 1001 : 1003, just_over ? 1003 : 1000);
        run.enemy(2, just_over ? 1003 : 1002, just_over ? 1000 : 1002);
        run.prior(RetentionRun::first + 7, 2);
        const auto orders = run.assign();
        check(
            FrontRun::target(orders, RetentionRun::first + 7) == (just_over ? 1 : 2),
            "cohort retention: costs45/40 retain at equality while50/44 replace"
        );
    }
    RetentionRun loaded(RetentionRun::retention_version, 3);
    loaded.place(RetentionRun::first, 900, 980);
    loaded.place(RetentionRun::first + 1, 1000, 1000);
    loaded.place(RetentionRun::first + 2, 1000, 1000);
    loaded.enemy(1, 1003, 1000);
    loaded.enemy(2, 1002, 1002);
    loaded.prior(RetentionRun::first + 1, 2);
    loaded.prior(RetentionRun::first + 2, 2);
    const auto orders = loaded.assign();
    check(
        FrontRun::target(orders, RetentionRun::first + 1) == 1,
        "cohort retention: earlier finalized pressure can reject a nearer prior"
    );
    RetentionRun finalized(RetentionRun::retention_version, 2);
    finalized.place(RetentionRun::first + 1, 1001, 1000);
    finalized.prior(RetentionRun::first, 2);
    const auto charged = finalized.assign();
    check(
        FrontRun::target(charged, RetentionRun::first) == 2 &&
            FrontRun::target(charged, RetentionRun::first + 1) == 1,
        "cohort retention: later cohorts charge the retained choice, not its fresh optimum"
    );
}

/// Applies ordinary enemy eligibility to a prior inside the same fresh search.
void test_cohort_retention_rechecks_prior_eligibility() {
    for (uint32_t condition = 0; condition < 6; ++condition) {
        RetentionRun run;
        for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source)
            run.prior(source, 2);
        auto& prior = run.fake.units[2];
        switch (condition) {
        case 0:
            prior.health = 0;
            break;
        case 1:
            prior.flags |= OA_UNIT_FLAG_DEATH_PENDING;
            break;
        case 2:
            prior.state_flags |= OA_UNIT_STATE_CLOAKED;
            break;
        case 3:
            prior.flags |= OA_UNIT_FLAG_NOT_SELECTABLE;
            break;
        case 4:
            prior.flags = (prior.flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) | 2u;
            break;
        default:
            for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source)
                run.prior(source, RetentionRun::first);
            break;
        }
        for (const auto& [source, target] : run.assign()) {
            (void)source;
            check(target == 1, "cohort retention: an ineligible majority prior is not retained");
        }
    }
    RetentionRun empty;
    empty.fake.units[1].health = empty.fake.units[2].health = 0;
    for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source)
        empty.prior(source, 2);
    check(
        empty.assign(300, 0, false).empty(),
        "cohort retention: no command without a live eligible enemy"
    );
}

/// Keeps the ordinary land and naval prior filters and wide integer costs.
void test_cohort_retention_keeps_water_rules_and_wide_costs() {
    for (const bool naval : {false, true}) {
        RetentionRun run(RetentionRun::retention_version, 8, 0, naval);
        run.rules.ai.nearest_enemy_filter.rules =
            oa::data::match_rules::AiNearestEnemyFilterRules::skip_submerged;
        const uint32_t submerged = 3;
        std::memcpy(run.fake.units[2].last_occupy_code, &submerged, sizeof submerged);
        for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source)
            run.prior(source, 2);
        for (const auto& [source, target] : run.assign()) {
            (void)source;
            check(
                target == (naval ? 2 : 1),
                "cohort retention: only naval strikes retain submerged priors"
            );
        }
    }
    for (const bool comparable : {false, true}) {
        RetentionRun run;
        for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source) {
            run.place(source, -32768, -32768);
            run.prior(source, 2);
        }
        run.enemy(1, comparable ? 32766 : 0, comparable ? 32767 : 0);
        run.enemy(2, 32767, 32767);
        for (const auto& [source, target] : run.assign()) {
            (void)source;
            check(
                target == (comparable ? 2 : 1),
                "cohort retention: weighted margin uses widened integer products"
            );
        }
    }
}

/// Retains a comparable higher-slot prior before applying the unchanged member fallback.
void test_cohort_retention_precedes_member_front_fallback() {
    FrontRun run(RetentionRun::retention_version);
    run.host.owned_attack_target = RetentionRun::read_owned_target;
    for (uint16_t source = FrontRun::first; source < FrontRun::boundary; ++source)
        run.place(source, 1000, 1000);
    run.place(FrontRun::boundary, 1000, 1099);
    for (uint16_t source = FrontRun::first + 4; source < FrontRun::first + FrontRun::members_count;
         ++source)
        run.place(source, 1000, 1399);
    run.enemy(1, 1000, 1000);
    run.enemy(2, 1000, 1100);
    run.enemy(3, 1000, 1048);
    for (uint16_t source = FrontRun::first; source <= FrontRun::boundary; ++source)
        run.fake.owned_targets[source] = 3;
    const auto orders = run.assign(300);
    check(
        FrontRun::target(orders, FrontRun::first) == 3,
        "cohort retention: exact equal fresh cost keeps the higher-slot majority prior"
    );
    check(
        FrontRun::target(orders, FrontRun::boundary) == 2,
        "cohort retention: the boundary member still uses the materially closer selected front"
    );
    check(
        std::none_of(
            orders.begin(), orders.end(), [](const auto& order) { return order.second == 1; }
        ),
        "cohort retention: discarded fresh optimum is not a member fallback candidate"
    );
}

/// Canonical records, rather than allocation addresses or membership-vector order, decide targets.
void test_cohort_retention_is_repeatable() {
    RetentionRun first;
    RetentionRun second;
    for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source) {
        first.prior(source, 2);
        second.prior(source, 2);
    }
    auto& membership =
        second.fake.squads[{RetentionRun::player, static_cast<int>(second.destination)}];
    std::reverse(membership.begin(), membership.end());
    const auto left = first.assign();
    const auto right = second.assign();
    check(
        left == right,
        "cohort retention: different allocations and vector orders give the same targets"
    );
    check(
        first.assign(600) == left && second.assign(600) == right,
        "cohort retention: unchanged canonical current heads retain the same assignments"
    );
    check(
        first.fake.roll == second.fake.roll,
        "cohort retention: repeatable policy keeps matching RNG state"
    );
}

/// Keeps legacy behavior and nullable callback fallback independent of current heads.
void test_cohort_retention_preserves_earlier_presets_and_null_reader() {
    for (const uint32_t version :
         {uint32_t{0},
          uint32_t{1},
          uint32_t{2},
          uint32_t{3},
          uint32_t{4},
          uint32_t{5},
          uint32_t{6},
          uint32_t{7},
          uint32_t{8},
          uint32_t{10}}) {
        RetentionRun run(version);
        for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source)
            run.prior(source, 2);
        for (const auto& [source, target] : run.assign()) {
            (void)source;
            check(target == 1, "cohort retention: versions0–10 keep their previous fresh target");
        }
        check(run.fake.prior_reads.empty(), "cohort retention: versions0–10 never query priors");
    }
    RetentionRun missing;
    missing.host.owned_attack_target = nullptr;
    for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source)
        missing.prior(source, 2);
    for (const auto& [source, target] : missing.assign()) {
        (void)source;
        check(target == 1, "cohort retention: no reader means ordinary fresh selection");
    }
    RetentionRun disabled;
    disabled.rules.setup.enhanced_mode.enabled = false;
    disabled.state.features = {};
    for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 8; ++source)
        disabled.prior(source, 2);
    for (const auto& [source, target] : disabled.assign()) {
        (void)source;
        check(
            target == 1, "cohort retention: disabled Enhanced Mode keeps native target selection"
        );
    }
    check(
        disabled.fake.prior_reads.empty(), "cohort retention: disabled mode never queries priors"
    );
}

/// Keeps all healthy current heads outside an early source mask untouched.
void test_cohort_retention_respects_mask_and_native_task_state() {
    RetentionRun run;
    for (uint16_t source = RetentionRun::first; source < RetentionRun::first + 7; ++source)
        run.prior(source, 2);
    const auto selected = static_cast<uint16_t>(RetentionRun::first + 7);
    const auto previous = run.fake.owned_targets;
    const auto task =
        run.state.players[RetentionRun::player].tasks[static_cast<uint32_t>(run.destination)];
    const auto random_before = run.fake.roll;
    std::array<uint64_t, strike_refresh_mask_words> sources{};
    sources[selected / strike_refresh_word_bits] |= uint64_t{1}
                                                    << (selected % strike_refresh_word_bits);
    run.fake.world.game.tick = 55;
    check(
        computer_player_refresh_strike_targets(
            &run.state, run.host, RetentionRun::player, run.destination, sources
        ),
        "cohort retention: an empty selected source receives a majority front"
    );
    check(
        run.fake.orders.size() == 1 && run.fake.orders.front().unit == selected &&
            run.fake.orders.front().arg == 2,
        "cohort retention: only selected source receives retained front"
    );
    for (uint16_t source = RetentionRun::first; source < selected; ++source)
        check(
            run.fake.owned_targets[source] == previous[source],
            "cohort retention: healthy unmasked target state stays unchanged"
        );
    const auto& after =
        run.state.players[RetentionRun::player].tasks[static_cast<uint32_t>(run.destination)];
    check(
        after.next_tick == task.next_tick && after.attacking == task.attacking &&
            run.fake.joined.empty() && run.fake.roll == random_before,
        "cohort retention: early mask preserves deadline, attack state, membership and RNG"
    );
}

/// Compares public dispatch with a full enemy search for eight known spatial groups.
struct FullEnemyRun : RetentionRun {
    static constexpr uint16_t enemies = 9;
    static constexpr uint16_t groups = 8;
    static constexpr uint32_t excluded_movement_flags = 0xau;
    static constexpr uint32_t move_rate_upper_tiers = 0x8u;
    using Orders = std::vector<std::pair<uint16_t, uint16_t>>;

    struct Reference {
        std::array<uint16_t, groups> fronts{};
        Orders orders;
    };

    /// Gives each group one source, with nine enemies sharing an exactly tied position.
    explicit FullEnemyRun(uint32_t version, bool naval = false)
        : RetentionRun(version, groups, 0, naval) {
        for (uint16_t group = 0; group < groups; ++group)
            place(first + group, 1000 + 200 * (group % 2), 1000 + 200 * (group / 2));
        for (uint16_t slot = 1; slot <= enemies; ++slot)
            enemy(slot, 1300, 1300);
    }

    /// Reads signed whole coordinates without depending on the policy's local helpers.
    static int32_t whole(oa::oa_fixed value) { return value / fixed_one - (value % fixed_one < 0); }

    /// Ranks current eligible enemies, without limiting their number or querying the host.
    bool eligible(uint16_t slot) const {
        const auto& unit = fake.units[slot];
        const auto& owner = fake.world.game.players[unit.owner_index];
        if (!owner.in_use || owner.status < 1 || owner.status > 3 ||
            owner.index == OA_PLAYER_COUNT || fake.allies[player][owner.index] ||
            (unit.flags & OA_UNIT_FLAG_LIVE) == 0 ||
            (unit.flags & OA_UNIT_FLAG_DEATH_PENDING) != 0 || unit.health <= 0 ||
            (unit.state_flags & OA_UNIT_STATE_CLOAKED) != 0 ||
            (unit.flags & OA_UNIT_FLAG_NOT_SELECTABLE) != 0)
            return false;
        const bool widened = rules.ai.nearest_enemy_filter.rules ==
                             oa::data::match_rules::AiNearestEnemyFilterRules::skip_submerged;
        if (widened ? (unit.flags & excluded_movement_flags) != 0
                    : (unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == 2)
            return false;
        uint32_t occupy{};
        std::memcpy(&occupy, unit.last_occupy_code, sizeof occupy);
        return !widened || source == Squad::navy || occupy != 3;
    }

    /// Measures complete signed coordinate differences in a wide integer.
    uint64_t distance(uint16_t actor, uint16_t target) const {
        const int64_t dx = static_cast<int64_t>(whole(fake.units[actor].position.x)) -
                           whole(fake.units[target].position.x);
        const int64_t dz = static_cast<int64_t>(whole(fake.units[actor].position.z)) -
                           whole(fake.units[target].position.z);
        return static_cast<uint64_t>(dx * dx + dz * dz);
    }

    /// Exhaustively selects all fronts before applying the independent member fallback.
    Reference reference(uint8_t selected = 0xffu) const {
        Reference result;
        std::array<uint32_t, enemies + 1> assignments{};
        for (uint16_t group = 0; group < groups; ++group) {
            const auto actor = static_cast<uint16_t>(first + group);
            uint16_t best{};
            uint64_t best_cost{};
            for (uint16_t target = 1; target <= enemies; ++target) {
                if (!eligible(target))
                    continue;
                const auto cost = (distance(actor, target) + 1) * (assignments[target] + 1);
                if (best == 0 || std::pair{cost, target} < std::pair{best_cost, best}) {
                    best = target;
                    best_cost = cost;
                }
            }
            const uint16_t prior =
                fake.running[actor] == "attack" ? fake.owned_targets[actor] : uint16_t{0};
            if (state.features.has(oa::data::enhanced_mode::Feature::cohort_retention) &&
                prior != 0 && prior <= enemies && eligible(prior)) {
                const auto prior_cost = (distance(actor, prior) + 1) * (assignments[prior] + 1);
                if (8 * prior_cost <= 9 * best_cost)
                    best = prior;
            }
            result.fronts[group] = best;
            if (best != 0)
                ++assignments[best];
        }
        for (uint16_t group = 0; group < groups; ++group) {
            if ((selected & (1u << group)) == 0 || result.fronts[group] == 0)
                continue;
            const auto actor = static_cast<uint16_t>(first + group);
            const auto own = result.fronts[group];
            auto nearest = own;
            for (const auto front : result.fronts)
                if (front != 0 && std::pair{distance(actor, front), front} <
                                      std::pair{distance(actor, nearest), nearest})
                    nearest = front;
            const auto target =
                4 * distance(actor, own) > 5 * distance(actor, nearest) ? nearest : own;
            result.orders.emplace_back(actor, target);
        }
        return result;
    }

    /// Checks periodic or masked public commands against the unbounded reference search.
    void compare(bool periodic = true, uint8_t selected = 0xffu, uint32_t at = 300) {
        const auto expected = reference(selected);
        if (periodic) {
            check(
                assign(at, 0, !expected.orders.empty()) == expected.orders,
                "full enemy reference: periodic target arguments agree"
            );
            return;
        }
        const auto before = state.players[player].tasks[static_cast<uint32_t>(destination)];
        const auto random_before = fake.roll;
        const auto membership = members();
        fake.orders.clear();
        fake.prior_reads.clear();
        fake.prior_read_after_callback = false;
        fake.world.game.tick = at;
        std::array<uint64_t, strike_refresh_mask_words> mask{};
        for (uint16_t group = 0; group < groups; ++group)
            if ((selected & (1u << group)) != 0)
                mask[(first + group) / strike_refresh_word_bits] |=
                    uint64_t{1} << ((first + group) % strike_refresh_word_bits);
        check(
            computer_player_refresh_strike_targets(&state, host, player, destination, mask) ==
                !expected.orders.empty(),
            "full enemy reference: refresh acceptance agrees"
        );
        Orders actual;
        for (const auto& order : fake.orders) {
            check(order.kind == "attack" && !order.queue, "full enemy reference: normal callback");
            actual.emplace_back(order.unit, order.arg);
        }
        check(actual == expected.orders, "full enemy reference: masked target arguments agree");
        const auto& after = state.players[player].tasks[static_cast<uint32_t>(destination)];
        check(
            after.next_tick == before.next_tick && after.attacking == before.attacking &&
                members() == membership && fake.joined.empty() && fake.roll == random_before &&
                !fake.prior_read_after_callback && fake.prior_reads.size() <= 2 * groups,
            "full enemy reference: refresh preserves clocks, membership, RNG and read order"
        );
    }
};

/// Exercises every three-position layout of four enemies against the full scan.
void test_full_enemy_reference_exhausts_small_layouts() {
    constexpr std::array<std::array<int32_t, 2>, 3> points{
        {{1100, 1100}, {1200, 1300}, {1300, 1100}}
    };
    constexpr uint32_t layouts = 3 * 3 * 3 * 3;
    for (const uint32_t version : {10u, 11u})
        for (const bool with_priors : {false, true})
            for (uint32_t layout = 0; layout < layouts; ++layout) {
                FullEnemyRun run(version);
                auto digits = layout;
                for (uint16_t target = 1; target <= 4; ++target) {
                    const auto& point = points[digits % points.size()];
                    run.enemy(target, point[0], point[1]);
                    digits /= points.size();
                }
                if (with_priors)
                    for (uint16_t group = 0; group < FullEnemyRun::groups; ++group)
                        run.prior(run.first + group, group % 3 == 0 ? 9 : group % 2 == 0 ? 1 : 0);
                run.compare();
            }
}

/// Requires the eighth fresh candidate and a separately eligible ninth majority prior.
void test_full_enemy_reference_keeps_eighth_and_outside_prior() {
    for (const uint32_t version : {10u, 11u}) {
        FullEnemyRun run(version);
        run.prior(run.first + 7, 9);
        check(
            run.reference().fronts[7] == (version == 11 ? 9 : 8),
            "full enemy reference: seven penalties expose eighth fresh or ninth retained front"
        );
        run.compare();
        for (uint16_t group = 0; group < 7; ++group)
            check(
                run.fake.owned_targets[run.first + group] == group + 1,
                "full enemy reference: every earlier tied front receives its finalized load"
            );
        check(
            run.fake.owned_targets[run.first + 7] == (version == 11 ? 9 : 8),
            "full enemy reference: public dispatch preserves eighth-rank and outside-prior cases"
        );
    }
}

/// Checks complete eligibility masks, water policy and player-range tie ordering.
void test_full_enemy_reference_keeps_filtered_candidate_domain() {
    for (const uint32_t version : {10u, 11u})
        for (const bool naval : {false, true})
            for (uint32_t unavailable = 0; unavailable < 16; ++unavailable) {
                FullEnemyRun run(version, naval);
                run.rules.ai.nearest_enemy_filter.rules =
                    oa::data::match_rules::AiNearestEnemyFilterRules::skip_submerged;
                for (uint16_t target = 1; target <= 4; ++target)
                    if ((unavailable & (1u << (target - 1))) != 0)
                        run.fake.units[target].health = 0;
                run.fake.units[5].flags |= OA_UNIT_FLAG_DEATH_PENDING;
                run.fake.units[6].state_flags |= OA_UNIT_STATE_CLOAKED;
                run.fake.units[7].flags |= OA_UNIT_FLAG_NOT_SELECTABLE;
                run.fake.units[8].flags |= FullEnemyRun::move_rate_upper_tiers;
                const uint32_t submerged = 3;
                std::memcpy(run.fake.units[9].last_occupy_code, &submerged, sizeof submerged);
                run.prior(run.first + 7, 9);
                run.compare();
            }
    for (const uint32_t version : {10u, 11u}) {
        FullEnemyRun run(version);
        run.fake.player(0, OA_PLAYER_STATUS_LOCAL, 4, 9);
        run.fake.player(2, OA_PLAYER_STATUS_LOCAL, 1, 3);
        for (uint16_t target = 1; target <= 3; ++target) {
            run.fake.units[target].owner_index = 2;
            run.fake.units[target].owner = oa::oa_ref_from_index(2);
        }
        run.prior(run.first + 7, 1);
        run.compare();
        run.fake.allies[run.player][2] = true;
        run.compare(true, 0xffu, 600);
        run.fake.allies[run.player][2] = false;
        run.fake.world.game.players[2].in_use = 0;
        run.compare(true, 0xffu, 900);
    }
}

/// Covers signed extrema, partial refresh masks and repeatable canonical decisions.
void test_full_enemy_reference_keeps_wide_masked_decisions() {
    for (const uint32_t version : {10u, 11u}) {
        FullEnemyRun run(version);
        constexpr std::array<int32_t, 4> z{-32768, -10000, 10000, 32767};
        for (uint16_t group = 0; group < run.groups; ++group) {
            // Z spans one more whole unit than X, retaining the known four Z bands.
            run.place(run.first + group, group % 2 == 0 ? -32768 : 32766, z[group / 2]);
            run.prior(run.first + group, group % 3 == 0 ? 9 : 0);
        }
        run.enemy(1, 32767, 32767);
        run.enemy(2, 32766, 32767);
        run.compare(false, 0xa5u, 55);
        auto& membership = run.fake.squads[{run.player, static_cast<int>(run.destination)}];
        std::reverse(membership.begin(), membership.end());
        run.compare(false, 0x5au, 61);
        run.compare(false, 0xffu, 67);
    }
}

/// Applies the closer-front fallback only to selected refresh sources without task changes.
void test_selected_front_fallback_respects_refresh_source_mask() {
    FrontRun run;
    run.enemy(1, 1002, 1002);
    run.fake.world.game.tick = 100;
    run.fake.running[FrontRun::first] = "patrol";
    const auto running = run.fake.running;
    const auto members = run.members();
    const auto random_before = run.fake.roll;
    const auto& task =
        run.state.players[FrontRun::player].tasks[static_cast<uint32_t>(run.destination)];
    const auto deadline = task.next_tick;
    const auto attacking = task.attacking;
    std::array<uint64_t, strike_refresh_mask_words> sources{};
    sources[FrontRun::boundary / strike_refresh_word_bits] |=
        uint64_t{1} << (FrontRun::boundary % strike_refresh_word_bits);
    check(
        computer_player_refresh_strike_targets(
            &run.state, run.host, FrontRun::player, run.destination, sources
        ),
        "selected front refresh: a selected boundary member receives an order"
    );
    check(
        run.fake.orders.size() == 1 && run.fake.orders.front().unit == FrontRun::boundary &&
            run.fake.orders.front().arg == 2,
        "selected front refresh: only the selected boundary member changes front"
    );
    for (uint16_t slot = FrontRun::first; slot < FrontRun::first + FrontRun::members_count; ++slot)
        if (slot != FrontRun::boundary)
            check(
                run.fake.running[slot] == running[slot],
                "selected front refresh: healthy unselected orders remain unchanged"
            );
    check(
        task.next_tick == deadline && task.attacking == attacking && run.members() == members &&
            run.fake.joined.empty() && run.fake.roll == random_before,
        "selected front refresh: task state, membership and random stream unchanged"
    );
}

/// Admits at most 256 new members per ordinary land or naval dispatch.
void test_distributed_strike_recruitment_budget() {
    constexpr uint32_t version = RecruitRun::distributed_version;
    for (const bool naval : {false, true}) {
        RecruitRun run(version, 600, 4, naval);
        run.dispatch(300, 256);
        const auto waiting = run.waiting();
        const auto members = run.members();
        run.fake.orders.clear();
        run.fake.joined.clear();
        run.tick(599);
        check(
            run.fake.joined.empty() && run.fake.orders.empty(),
            "recruit budget: nothing joins before the next native deadline"
        );
        check(
            run.waiting() == waiting && run.members() == members,
            "recruit budget: membership waits for the next dispatch"
        );
        run.dispatch(600, 256);
        run.dispatch(900, 88);
        check(
            run.waiting().empty() && run.members().size() == 604,
            "recruit budget: all waiting members eventually join"
        );
    }
    RecruitRun empty(version, 600, 0);
    empty.dispatch(300, 256);
    empty.dispatch(600, 256);
    empty.dispatch(900, 88);
    RecruitRun existing(version, 0, 600);
    existing.dispatch(300, 0);
    check(
        existing.fake.orders.size() == 600,
        "recruit budget: the admission cap does not truncate existing strike orders"
    );
}

/// Leaves the old 5000 recruits plus the separate empty-squad seed unchanged.
void test_earlier_recruitment_keeps_its_seed_and_limit() {
    for (uint32_t version = 0; version < RecruitRun::distributed_version; ++version) {
        RecruitRun run(version, 5002, 0);
        run.dispatch(300, 5001);
        check(
            run.waiting().size() == 1 && run.members().size() == 5001,
            "recruit budget: earlier presets retain the separate seed and 5000 joins"
        );
    }
}

/// Reads the current source membership after a waiting slot becomes a builder.
void test_recruitment_rereads_reused_source_slots() {
    RecruitRun run(RecruitRun::distributed_version, 600, 4);
    run.dispatch(300, 256);
    const auto reused = run.waiting().front();
    run.fake.set_squad(reused, -1);
    auto& builder = run.fake.spawn(reused, RecruitRun::player, ARMCK, 300, 300, 0);
    builder.id = reused;
    builder.health = 1000;
    run.fake.set_squad(reused, static_cast<int>(Squad::builders));
    run.fake.running[reused] = "idle";
    run.dispatch(600, 256);
    check(
        std::find(run.fake.joined.begin(), run.fake.joined.end(), reused) == run.fake.joined.end(),
        "recruit budget: the replacement builder is not admitted from stale membership"
    );
    check(
        run.fake.running[reused] == "idle" && run.fake.units[reused].type_index == ARMCK,
        "recruit budget: the replacement's idle order remains unchanged"
    );
    run.dispatch(900, 87);
    check(
        run.waiting().empty() && run.members().size() == 603,
        "recruit budget: all remaining live source members join without the reused slot"
    );
}

/// Leaves another task's whole-squad callback dispatch independent of recruitment.
void test_recruitment_budget_does_not_limit_rally_callbacks() {
    RecruitRun run(RecruitRun::distributed_version, 600, 1);
    auto& tasks = run.state.players[RecruitRun::player].tasks;
    tasks[static_cast<uint32_t>(Squad::land_strike)].kind = TaskKind::none;
    tasks[static_cast<uint32_t>(Squad::land_army)] = {
        TaskKind::rally, Squad::land_army, 300, 0, 0, 0, Squad::land_strike, 0
    };
    auto expected = run.waiting();
    std::sort(expected.begin(), expected.end());
    const auto random_before = run.fake.roll;
    run.tick(300);
    std::vector<uint16_t> moved;
    for (const auto& order : run.fake.orders) {
        check(
            order.kind == "move" && !order.queue,
            "recruit budget: a rally still gives ordinary unqueued moves"
        );
        moved.push_back(order.unit);
    }
    check(
        moved == expected && moved.size() == 600,
        "recruit budget: another task still dispatches every waiting member"
    );
    check(
        run.fake.joined.empty() && run.waiting().size() == 600,
        "recruit budget: the rally changes no source membership"
    );
    check(run.fake.roll == random_before, "recruit budget: the rally's random stream is unchanged");
    check(
        tasks[static_cast<uint32_t>(Squad::land_army)].next_tick == 450,
        "recruit budget: the rally keeps its separate 150-tick deadline"
    );
}

// The aircraft task's run, with the wing and base given.
struct AirRun {
    Fake fake;
    ComputerPlayers state{};
    MatchRules rules{};

    AirRun(uint32_t wing, bool with_base) {
        fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        fake.spawn(1, 0, ARMLLT, 500, 600, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
        for (uint32_t i = 0; i < wing; ++i) {
            fake.spawn(11 + i, 1, ARMFIG, 300 + static_cast<int32_t>(i), 300, 0);
            fake.set_squad(static_cast<uint16_t>(11 + i), static_cast<int>(Squad::aircraft));
        }
        state.rules.match = &rules;
        describe_catalog(state);
        check(computer_players_configure(&state, "", kBuildLists), "air: configure");
        fake.rolls.assign(4096, 3);
        check(computer_players_initialize(&state, host_for(fake)), "air: initialize");
        if (with_base)
            state.players[1].knowledge.base_position = {200 << 16, 0, 200 << 16};
    }

    ~AirRun() { computer_players_release(&state); }

    // Runs the order tick once at tick 1, when only the aircraft task (and the others,
    // on empty squads) is due.
    void run() {
        fake.world.game.tick = 1;
        fake.roll = 0;
        auto host = host_for(fake);
        computer_player_tick_orders(&state, host, 1);
    }
};

// ai.patrol-group-size: a wing patrols to a map edge from the group size; below it, it
// scouts around the base (a move, then queued patrols).
void test_patrol_group_size() {
    for (const int32_t size : {5, 15}) {
        AirRun air(6, true);
        air.rules.ai.patrol_group_size.enabled = size != 5;
        air.rules.ai.patrol_group_size.units = size;
        air.run();
        const bool scouts = orders_of(air.fake, "move", 11) != 0;
        check(scouts == (size == 15), "patrol size: six scout below 15 and raid from 5");
        bool edge = false;
        for (const auto& order : air.fake.orders)
            edge |= order.kind == "patrol" && order.unit == 11 && !order.queue;
        check(edge == (size == 5), "patrol size: the raid is one patrol in place of orders");
    }
}

// ai.patrol-null-enemy-skip: without a base a small wing patrols to a random map edge,
// drawing as a large wing does, in place of a queued patrol to the nearest enemy.
void test_patrol_null_enemy_skip() {
    for (const bool on : {false, true}) {
        AirRun air(2, false);
        air.rules.ai.patrol_null_enemy_skip.enabled = on;
        air.run();
        bool to_enemy = false, to_edge = false;
        for (const auto& order : air.fake.orders) {
            if (order.unit != 11 || order.kind != "patrol")
                continue;
            to_enemy |= order.queue && order.at.x == (500 << 16) && order.at.z == (600 << 16);
            // The draws all give 3: an odd random(2) picks a random x on the z = 0 edge.
            to_edge |= !order.queue && order.at.x == (3 << 16) && order.at.z == 0;
        }
        check(to_enemy == !on, "null enemy: 3.1c patrols to the nearest enemy");
        check(to_edge == on, "null enemy: the rule patrols to a map edge");
        // The aircraft task's rescheduling draw, the edge's three draws, then the siege
        // task's rescheduling draw.
        check(air.fake.roll == (on ? 5u : 2u), "null enemy: the edge's draws");
    }
}

// ai.squad-assignment role-squads: the sort table, and standing orders keyed on the
// commander ability.
void test_squad_assignment() {
    using SquadRules = oa::data::match_rules::AiSquadAssignmentRules;
    constexpr auto role = SquadRules::role_squads;
    ComputerType type{};
    oa::Unit building{};
    building.flags = OA_UNIT_FLAG_BUILDING;
    type.flags = OA_UNIT_DEF_FLAG_BUILDER;
    check(computer_sort_squad(building, type, role) == Squad::structures, "role: a factory");
    type.flags = 0;
    type.energy_use = 58.5F;
    check(computer_sort_squad(building, type, role) == Squad::structures, "role: 58.5 energy");
    type.energy_use = 58.49F; // high word 0x4269
    check(computer_sort_squad(building, type, role) == Squad::armed_structures, "role: 58.49");
    type.energy_use = -200.0F;
    check(computer_sort_squad(building, type, role) == Squad::armed_structures, "role: producer");
    building.flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    type.energy_use = 1000.0F;
    check(computer_sort_squad(building, type, role) == Squad::structures, "role: armed user");
    check(
        computer_sort_squad(building, type, SquadRules::base) == Squad::armed_structures,
        "base: an armed building is a defence"
    );
    oa::Unit mobile{};
    type = {};
    type.flags = OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_CAN_FLY;
    check(computer_sort_squad(mobile, type, role) == Squad::builders, "role: builder first");
    type.flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    check(computer_sort_squad(mobile, type, role) == Squad::aircraft, "role: aircraft");
    type.flags = 0;
    type.min_water_depth = 1;
    check(computer_sort_squad(mobile, type, role) == Squad::navy, "role: needs water");
    type.min_water_depth = -10;
    type.max_water_depth = 128;
    check(computer_sort_squad(mobile, type, role) == Squad::navy, "role: dives 128");
    check(
        computer_sort_squad(mobile, type, SquadRules::base) == Squad::none, "base: unarmed diver"
    );
    type.max_water_depth = 127;
    check(computer_sort_squad(mobile, type, role) == Squad::land_army, "role: an unarmed tank");
    type.flags = OA_UNIT_DEF_FLAG_AMPHIBIOUS;
    check(computer_sort_squad(mobile, type, role) == Squad::navy, "role: amphibious");
    check(
        computer_sort_squad(mobile, type, SquadRules::base) == Squad::none,
        "base: unarmed amphibian"
    );

    for (const auto rules_value : {SquadRules::base, SquadRules::role_squads}) {
        Fake fake;
        fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
        auto host = host_for(fake);
        MatchRules rules{};
        rules.ai.squad_assignment.enabled = rules_value != SquadRules::base;
        rules.ai.squad_assignment.rules = rules_value;
        ComputerPlayers state{};
        state.rules.match = &rules;
        describe_catalog(state);
        // ARMCOM captures; ARMCK is a commander that does not.
        state.types[ARMCK].abilities = OA_UNIT_DEF_ABILITY_COMMANDER;
        check(computer_players_configure(&state, "", kBuildLists), "orders: configure");
        auto& com = fake.spawn(10, 1, ARMCOM, 200, 200, 0);
        auto& ck = fake.spawn(11, 1, ARMCK, 220, 200, 0);
        auto& tank = fake.spawn(12, 1, ARMPW, 240, 200, 0);
        fake.rolls.assign(4096, 7);
        check(computer_players_initialize(&state, host), "orders: initialize");
        tick_until(state, fake, host, 1, 30);
        const bool role_rules = rules_value == SquadRules::role_squads;
        check(standing_move(com) == (role_rules ? 2u : 1u), "orders: the capturer");
        check(standing_move(ck) == (role_rules ? 1u : 2u), "orders: the commander");
        check(standing_fire(com) == 2 && standing_fire(ck) == 2, "orders: fire at will");
        check(
            tank.squad == (role_rules ? static_cast<int32_t>(Squad::land_army) : 0),
            "orders: an unarmed tank joins the army only by the role rules"
        );
        computer_players_release(&state);
    }
}

// ai.nearest-enemy-filter skip-submerged: the land strike passes over a fully submerged
// enemy and over move-rate tiers 2 and 3; the naval strike still finds the submerged one.
void test_nearest_enemy_filter() {
    using Filter = oa::data::match_rules::AiNearestEnemyFilterRules;
    for (const auto filter : {Filter::base, Filter::skip_submerged}) {
        for (const bool naval : {false, true}) {
            Fake fake;
            fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
            fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
            auto host = host_for(fake);
            MatchRules rules{};
            rules.ai.nearest_enemy_filter.enabled = filter != Filter::base;
            rules.ai.nearest_enemy_filter.rules = filter;
            ComputerPlayers state{};
            state.rules.match = &rules;
            describe_catalog(state);
            check(computer_players_configure(&state, "", kBuildLists), "nearest: configure");
            // Nearest first: a submerged sub, a fast mover, then a tower.
            auto& sub = fake.spawn(1, 0, ARMPT, 400, 300, OA_UNIT_FLAG_HAS_WEAPONS);
            const uint32_t submerged = 3;
            std::memcpy(sub.last_occupy_code, &submerged, sizeof submerged);
            constexpr uint32_t move_rate_tier_2 = 0x8u;
            fake.spawn(2, 0, ARMPW, 500, 300, OA_UNIT_FLAG_HAS_WEAPONS | move_rate_tier_2);
            fake.spawn(3, 0, ARMLLT, 600, 300, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
            const auto type = naval ? ARMPT : ARMPW;
            for (uint32_t slot = 12; slot < 19; ++slot)
                fake.spawn(
                    slot, 1, type, 300 + static_cast<int32_t>(slot), 300, OA_UNIT_FLAG_HAS_WEAPONS
                );
            fake.rolls.assign(4096, 7);
            check(computer_players_initialize(&state, host), "nearest: initialize");
            tick_until(state, fake, host, 1, 301);
            uint16_t target = 0;
            for (const auto& order : fake.orders)
                if (order.kind == "attack" && order.unit == 12)
                    target = order.arg;
            const uint16_t expected = filter == Filter::base ? 1 : naval ? 1 : 3;
            check(target == expected, "nearest: the strike's target");
            computer_players_release(&state);
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        const auto assets = oa::test::require_game_assets("the installed computer-player data");
        test_installed_data(assets);
        failures += oa::test::computer_fake::fixture_failures;
        if (failures != 0) {
            std::fprintf(stderr, "%d failure(s)\n", failures);
            return 1;
        }
        std::printf("computer player data tests passed\n");
        return 0;
    }
    test_sort_squads();
    test_build_list_limits();
    test_whole_build_lists();
    test_profile();
    test_adopt_handed_player();
    test_downloadable_directives();
    test_weight_report();
    test_score_and_pick();
    test_lab_idle_without_metal_income();
    test_controller_presence();
    test_tick();
    test_scripted_units_stay_unsorted();
    test_busy_factory_gets_no_pick();
    test_campaign_downloadables();
    test_profile_reload();
    test_profile_leading_lines();
    test_profile_lines_without_number();
    test_profile_lines_skipped();
    test_split_name_is_skipped();
    test_split_unit_name_reads_zero();
    test_sighted_weight();
    test_siege();
    test_difficulty_names();
    test_squad5_factory_tick();
    test_factory_tick_filter();
    test_builder_withhold_threshold();
    test_attack_wave_size();
    test_earlier_strikes_keep_one_target();
    test_spatial_strikes_use_local_targets();
    test_cohort_target_pressure_spreads_a_cluster();
    test_cohort_pressure_keeps_wide_costs();
    test_cohort_targets_keep_water_filter();
    test_cohort_target_filtering_and_empty_search();
    test_cohort_targets_reread_live_slots();
    test_cohort_targets_keep_canonical_callback_order();
    test_cohort_targets_break_ties_by_slot();
    test_masked_cohort_refresh_keeps_task_state();
    test_selected_front_fallback_crosses_coarse_group_boundary();
    test_selected_front_fallback_keeps_exact_distance_threshold();
    test_selected_front_fallback_keeps_candidate_filter_and_tie();
    test_selected_front_fallback_preserves_earlier_presets();
    test_selected_front_fallback_respects_refresh_source_mask();
    test_cohort_retention_requires_a_verified_majority();
    test_cohort_retention_counts_new_recruits_without_priors();
    test_cohort_retention_handles_more_than_eight_prior_targets();
    test_cohort_retention_keeps_exact_weighted_margin_and_pressure();
    test_cohort_retention_rechecks_prior_eligibility();
    test_cohort_retention_keeps_water_rules_and_wide_costs();
    test_cohort_retention_precedes_member_front_fallback();
    test_cohort_retention_is_repeatable();
    test_cohort_retention_preserves_earlier_presets_and_null_reader();
    test_cohort_retention_respects_mask_and_native_task_state();
    test_full_enemy_reference_exhausts_small_layouts();
    test_full_enemy_reference_keeps_eighth_and_outside_prior();
    test_full_enemy_reference_keeps_filtered_candidate_domain();
    test_full_enemy_reference_keeps_wide_masked_decisions();
    test_distributed_strike_recruitment_budget();
    test_earlier_recruitment_keeps_its_seed_and_limit();
    test_recruitment_rereads_reused_source_slots();
    test_recruitment_budget_does_not_limit_rally_callbacks();
    test_patrol_group_size();
    test_patrol_null_enemy_skip();
    test_squad_assignment();
    test_nearest_enemy_filter();
    failures += oa::test::computer_fake::fixture_failures;
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("computer player tests passed\n");
    return 0;
}
