// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match-level computer build cases, once by the built-in player and once
// with Classic seated. After every tick the random stream and the world
// digest match. The case's own checks are read on the Classic run.
//
// A campaign run caps ARMRELAY in the profile both sides read. The built-in
// player's limit call writes only its own knowledge, which Classic does not
// read, so the cap has to be in the profile before the first order. The
// limit call still runs after the first tick, as the case does.

#include "classic_harness.hpp"

#include "match_fixture.hpp"

#include "oa/core/player.h"
#include "oa/data/brain_facts.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/state_hash.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;
namespace facts = oa::data::brain_facts;
namespace played = oa::data::mod_profile;

using PackageBytes = oa::test::classic_harness::PackageBytes;

// The relay cap, alone. The campaign case's profile is empty and the cap is
// applied to the built-in knowledge after one tick; Classic takes the cap
// from this line instead, and both runs here read it.
constexpr const char* relay_cap = "limit ARMRELAY 0\n";

struct Spend {
    uint64_t largest_tick{0};
    uint64_t lowest_bank{UINT64_MAX};
    bool saw_tick{false};
};

struct StopNote {
    bool stopped{false};
    uint32_t tick{0};
    std::string reason;
};

void note_stopped(void* context, uint8_t, uint32_t tick, const host::ScriptError& error) {
    auto* note = static_cast<StopNote*>(context);
    if (note == nullptr || note->stopped)
        return;
    note->stopped = true;
    note->tick = tick;
    try {
        note->reason = host::describe(error);
    } catch (const std::exception&) {
        note->reason = "the seat stopped";
    }
}

void print_spend(const Spend& spend) {
    std::cout << "classic match: largest tick " << spend.largest_tick
              << " instructions, lowest bank " << spend.lowest_bank << '\n';
}

facts::GameFacts facts_of(const data::match_rules::MatchRules& rules, std::string_view text) {
    played::ModProfile mod;
    mod.rules = rules;
    facts::GameFacts game;
    game.difficulty = OA_DIFFICULTY_MEDIUM;
    game.mod.profile = std::move(mod);
    if (!text.empty()) {
        game.ai_profile.path = "ai/test.txt";
        game.ai_profile.text = std::string(text);
    }
    return game;
}

void require_seated(const brains::SeatOutcome& outcome) {
    if (outcome.result == brains::SeatResult::seated)
        return;
    const char* reason = outcome.reason[0] != '\0' ? outcome.reason.data() : "not seated";
    throw std::runtime_error(std::string("seat: ") + reason);
}

void prepare(
    Fixture& fixture,
    std::string_view profile_text,
    bool campaign,
    const data::match_rules::MatchRules& rules,
    const PackageBytes* package,
    StopNote* note,
    std::span<const uint16_t> lists = build_lists
) {
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile_text, lists, campaign));
    brains::set_match_game_facts(*fixture.match, facts_of(rules, profile_text));
    if (package == nullptr)
        return;
    if (note != nullptr) {
        brains::ReportHooks hooks;
        hooks.context = note;
        hooks.stopped = &note_stopped;
        brains::set_report_hooks(*fixture.match, hooks);
    }
    require_seated(
        brains::seat_brain(*fixture.match, oa::test::classic_harness::classic_spec(*package))
    );
    CHECK(brains::brain_seated(*fixture.match, computer));
}

void step(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

uint64_t digest_of(Fixture& fixture) {
    return oa::sim::trace::match_state_hash(
        *fixture.match, oa::base::game_loop::Timing{}, 0, 0, nullptr
    );
}

void require_same(Fixture& plain, Fixture& with_brain, const char* name, uint32_t tick) {
    if (plain.match->fault() != nullptr || with_brain.match->fault() != nullptr)
        throw std::runtime_error(
            std::string(name) + ": match fault at tick " + std::to_string(tick)
        );
    if (plain.match->random_state() != with_brain.match->random_state())
        throw std::runtime_error(
            std::string(name) + ": random state at tick " + std::to_string(tick)
        );
    if (digest_of(plain) != digest_of(with_brain))
        throw std::runtime_error(
            std::string(name) + ": world digest at tick " + std::to_string(tick)
        );
}

void note_tick(
    Fixture& with_brain,
    StopNote& note,
    Spend& spend,
    uint64_t& before,
    const char* name,
    uint32_t tick
) {
    if (note.stopped || !brains::brain_seated(*with_brain.match, computer)) {
        const std::string why = note.reason.empty() ? "Classic left the seat" : note.reason;
        throw std::runtime_error(
            std::string(name) + ": Classic stopped at tick " + std::to_string(tick) + ": " + why
        );
    }
    const uint64_t after = brains::seat_instructions(*with_brain.match, computer);
    if (after < before)
        throw std::runtime_error(std::string(name) + ": instruction total went backwards");
    const uint64_t spent = after - before;
    before = after;
    if (spent > spend.largest_tick)
        spend.largest_tick = spent;
    host::Sandbox* sandbox = brains::seat_sandbox(*with_brain.match, computer);
    if (sandbox == nullptr)
        throw std::runtime_error(
            std::string(name) + ": no sandbox at tick " + std::to_string(tick)
        );
    const uint64_t balance = sandbox->bank_balance();
    if (balance < spend.lowest_bank)
        spend.lowest_bank = balance;
    spend.saw_tick = true;
}

bool play(
    Fixture& plain,
    Fixture& with_brain,
    StopNote& note,
    Spend& spend,
    uint32_t ticks,
    const char* name,
    const std::function<bool()>& done
) {
    require_same(plain, with_brain, name, 0);
    uint64_t before = brains::seat_instructions(*with_brain.match, computer);
    for (uint32_t tick = 1; tick <= ticks; ++tick) {
        step(plain);
        step(with_brain);
        require_same(plain, with_brain, name, tick);
        note_tick(with_brain, note, spend, before, name, tick);
        if (done && done())
            return true;
    }
    return false;
}

void spawn_two(Fixture& fixture) {
    fixture.spawn(0, vehicle, 8, 8);
    fixture.spawn(computer, vehicle, 40, 40);
}

std::vector<int64_t> end_state(const Fixture& fixture) {
    std::vector<int64_t> state;
    for (const auto& slot : fixture.match->world().slots) {
        if (!slot.unit)
            continue;
        state.push_back(slot.record.type_index);
        state.push_back(slot.record.position.x);
        state.push_back(slot.record.position.z);
        state.push_back(slot.record.squad);
    }
    state.push_back(static_cast<int64_t>(fixture.count(computer, collector)));
    return state;
}

void open_pair(
    Fixture& plain,
    Fixture& with_brain,
    std::string_view profile_text,
    bool campaign,
    const data::match_rules::MatchRules& rules,
    const PackageBytes& package,
    StopNote& note,
    const std::function<void(Fixture&)>& spawn
) {
    spawn(plain);
    spawn(with_brain);
    prepare(plain, profile_text, campaign, rules, nullptr, nullptr);
    prepare(with_brain, profile_text, campaign, rules, &package, &note);
}

void test_builds(const PackageBytes& package, Spend& spend) {
    Fixture plain;
    Fixture with_brain;
    StopNote note;
    open_pair(plain, with_brain, profile, false, {}, package, note, spawn_two);
    uint32_t ticks = 0;
    const bool built = play(plain, with_brain, note, spend, run_ticks, "builds", [&] {
        ++ticks;
        return with_brain.count(computer, collector) != 0;
    });
    const auto& strengths = with_brain.match->strategic_state(computer).strengths;
    CHECK(strengths.size() == type_count);
    CHECK(strengths[collector][0] == 84);
    CHECK(strengths[collector][1] == 0 && strengths[collector][2] == 0);
    const auto* knowledge = sim::ai::computer_player_knowledge(
        sim::ai::match_computer_players(*with_brain.match), computer
    );
    CHECK(knowledge != nullptr);
    CHECK(knowledge->weight_percent[collector] == 10);
    CHECK(knowledge->weight_percent[relay] == 50 && knowledge->limits[relay] == 0);
    CHECK(sim::ai::match_computer_players(*with_brain.match)->types[vehicle].build_count == 2);
    CHECK(built);
    const auto& income = with_brain.match->state().game.players[computer];
    CHECK(oa::player_energy_produced(&income) == 210.0F);
    CHECK(oa::player_metal_produced(&income) == 7.0F);
    CHECK(ticks > 30);
    CHECK(with_brain.count(computer, relay) == 0);
    CHECK(with_brain.count(0, collector) == 0);
    if (ticks < run_ticks)
        play(plain, with_brain, note, spend, run_ticks - ticks, "builds rest", {});
}

void test_no_income(const PackageBytes& package, Spend& spend) {
    Fixture plain(0, false);
    Fixture with_brain(0, false);
    StopNote note;
    open_pair(plain, with_brain, profile, false, {}, package, note, [](Fixture& fixture) {
        fixture.spawn(computer, vehicle, 40, 40);
    });
    const bool built = play(plain, with_brain, note, spend, run_ticks, "no income", [&] {
        return with_brain.count(computer, collector) != 0;
    });
    CHECK(!built);
}

void test_campaign(const PackageBytes& package, Spend& spend) {
    for (const bool campaign : {true, false}) {
        Fixture plain(collector);
        Fixture with_brain(collector);
        StopNote note;
        const char* name = campaign ? "campaign" : "skirmish downloadables";
        open_pair(plain, with_brain, relay_cap, campaign, {}, package, note, [](Fixture& fixture) {
            fixture.spawn(computer, vehicle, 40, 40);
        });
        CHECK(
            sim::ai::match_computer_players(*with_brain.match)->downloadables_restricted ==
            (campaign ? 1 : 0)
        );
        play(plain, with_brain, note, spend, 1, name, {});
        sim::ai::computer_apply_limit(
            sim::ai::match_computer_players(*plain.match), computer, "ARMRELAY", 0
        );
        sim::ai::computer_apply_limit(
            sim::ai::match_computer_players(*with_brain.match), computer, "ARMRELAY", 0
        );
        const bool built = play(plain, with_brain, note, spend, run_ticks, name, [&] {
            return with_brain.count(computer, collector) != 0;
        });
        CHECK(built == !campaign);
    }
}

void test_silo(const PackageBytes& package, Spend& spend) {
    data::match_rules::MatchRules rules{};
    rules.ai.squad5_factory_tick.enabled = true;
    Fixture plain(0, true, rules);
    Fixture with_brain(0, true, rules);
    StopNote note;
    plain.spawn(computer, silo, 40, 40);
    auto& launcher = with_brain.spawn(computer, silo, 40, 40);
    prepare(plain, "", false, rules, nullptr, nullptr);
    prepare(with_brain, "", false, rules, &package, &note);
    bool missile_built = false;
    const bool stockpiled = play(plain, with_brain, note, spend, run_ticks, "silo", [&] {
        missile_built = missile_built || with_brain.count(computer, missile) != 0 ||
                        with_brain.match->queued_build_count(launcher.unit_index, missile) != 0;
        return launcher.record.weapons[0].stockpile != 0;
    });
    CHECK(!missile_built);
    CHECK(stockpiled);
    CHECK(with_brain.match->fault() == nullptr);
}

void test_builder_rounds(const PackageBytes& package, Spend& spend) {
    Fixture plain(0, true, {}, 0, 1, {}, 2, true);
    Fixture with_brain(0, true, {}, 0, 1, {}, 2, true);
    StopNote note;
    plain.spawn(computer, vehicle, 40, 40);
    auto& defence = with_brain.spawn(computer, vehicle, 40, 40);
    prepare(plain, "", false, {}, nullptr, nullptr, builder_round_lists);
    prepare(with_brain, "", false, {}, &package, &note, builder_round_lists);
    bool missile_built = false;
    const bool stockpiled = play(plain, with_brain, note, spend, run_ticks, "builder rounds", [&] {
        missile_built = missile_built || with_brain.count(computer, missile) != 0 ||
                        with_brain.match->queued_build_count(defence.unit_index, missile) != 0;
        return defence.record.weapons[0].stockpile != 0;
    });
    CHECK(!missile_built);
    CHECK(stockpiled);
    CHECK(with_brain.match->fault() == nullptr);
}

data::match_rules::MatchRules baseline_rules() {
    data::match_rules::MatchRules rules{};
    rules.ai.difficulty_names.enabled = true;
    rules.ai.factory_tick_filter.enabled = true;
    rules.ai.builder_withhold_threshold.enabled = true;
    rules.ai.attack_wave_size.enabled = true;
    rules.ai.patrol_group_size.enabled = true;
    rules.ai.squad_assignment.enabled = true;
    rules.ai.nearest_enemy_filter.enabled = true;
    return rules;
}

data::match_rules::MatchRules modded_rules() {
    namespace rules = data::match_rules;
    rules::MatchRules modded{};
    auto& ai = modded.ai;
    ai.difficulty_names.enabled = true;
    ai.difficulty_names.names = {
        rules::AiDifficultyNamesNames::hard,
        rules::AiDifficultyNamesNames::medium,
        rules::AiDifficultyNamesNames::easy
    };
    ai.squad5_factory_tick.enabled = true;
    ai.factory_tick_filter = {true, true, rules::AiFactoryTickFilterPowerToggle::energy_use_32};
    ai.builder_withhold_threshold = {true, 10};
    ai.commander_keeps_orders_when_damaged.enabled = true;
    ai.attack_wave_size = {true, 10};
    ai.patrol_group_size = {true, 15};
    ai.patrol_null_enemy_skip.enabled = true;
    ai.squad_assignment = {true, rules::AiSquadAssignmentRules::role_squads};
    ai.nearest_enemy_filter = {true, rules::AiNearestEnemyFilterRules::skip_submerged};
    return modded;
}

// The pinned modded set leaves income multipliers off. A mod that turns the
// hack on takes the registry's on-default, 0.5, 1.0 and 4.0, for production
// and for reclaim. The alone case uses that.
data::match_rules::MatchRules income_rules() {
    data::match_rules::MatchRules rules{};
    rules.ai.income_multipliers.enabled = true;
    rules.ai.income_multipliers.production = {0.5, 1.0, 4.0};
    rules.ai.income_multipliers.reclaim = {0.5, 1.0, 4.0};
    return rules;
}

data::match_rules::MatchRules difficulty_name_rules() {
    namespace rules = data::match_rules;
    rules::MatchRules named{};
    named.ai.difficulty_names.enabled = true;
    named.ai.difficulty_names.names = {
        rules::AiDifficultyNamesNames::hard,
        rules::AiDifficultyNamesNames::medium,
        rules::AiDifficultyNamesNames::easy
    };
    return named;
}

data::match_rules::MatchRules squad5_rules() {
    data::match_rules::MatchRules rules{};
    rules.ai.squad5_factory_tick.enabled = true;
    return rules;
}

data::match_rules::MatchRules factory_filter_rules() {
    namespace rules = data::match_rules;
    rules::MatchRules filter{};
    filter.ai.factory_tick_filter = {
        true, true, rules::AiFactoryTickFilterPowerToggle::energy_use_32
    };
    return filter;
}

data::match_rules::MatchRules withhold_rules() {
    data::match_rules::MatchRules rules{};
    rules.ai.builder_withhold_threshold = {true, 10};
    return rules;
}

data::match_rules::MatchRules commander_rules() {
    data::match_rules::MatchRules rules{};
    rules.ai.commander_keeps_orders_when_damaged.enabled = true;
    return rules;
}

data::match_rules::MatchRules wave_rules() {
    data::match_rules::MatchRules rules{};
    rules.ai.attack_wave_size = {true, 10};
    return rules;
}

data::match_rules::MatchRules patrol_size_rules() {
    data::match_rules::MatchRules rules{};
    rules.ai.patrol_group_size = {true, 15};
    return rules;
}

data::match_rules::MatchRules patrol_skip_rules() {
    data::match_rules::MatchRules rules{};
    rules.ai.patrol_null_enemy_skip.enabled = true;
    return rules;
}

data::match_rules::MatchRules squad_rules() {
    namespace rules = data::match_rules;
    rules::MatchRules assigned{};
    assigned.ai.squad_assignment = {true, rules::AiSquadAssignmentRules::role_squads};
    return assigned;
}

data::match_rules::MatchRules nearest_rules() {
    namespace rules = data::match_rules;
    rules::MatchRules nearest{};
    nearest.ai.nearest_enemy_filter = {true, rules::AiNearestEnemyFilterRules::skip_submerged};
    return nearest;
}

std::vector<int64_t> play_rules(
    const PackageBytes& package,
    Spend& spend,
    const data::match_rules::MatchRules& rules,
    const char* name
) {
    Fixture plain(0, true, rules);
    Fixture with_brain(0, true, rules);
    StopNote note;
    open_pair(plain, with_brain, profile, false, rules, package, note, spawn_two);
    play(plain, with_brain, note, spend, run_ticks, name, {});
    return end_state(with_brain);
}

void test_rule_sets(const PackageBytes& package, Spend& spend) {
    const auto base = play_rules(package, spend, {}, "rules 3.1c");
    CHECK(base.back() != 0);
    CHECK(play_rules(package, spend, baseline_rules(), "rules baseline") == base);
    const auto modded = play_rules(package, spend, modded_rules(), "rules modded");
    CHECK(modded.back() != 0);
    CHECK(play_rules(package, spend, modded_rules(), "rules modded again") == modded);
}

void test_hacks_alone(const PackageBytes& package, Spend& spend) {
    struct Alone {
        const char* name;
        data::match_rules::MatchRules (*make)();
    };

    const Alone hacks[] = {
        {"income-multipliers", income_rules},
        {"difficulty-names", difficulty_name_rules},
        {"squad5-factory-tick", squad5_rules},
        {"factory-tick-filter", factory_filter_rules},
        {"builder-withhold-threshold", withhold_rules},
        {"commander-keeps-orders-when-damaged", commander_rules},
        {"attack-wave-size", wave_rules},
        {"patrol-group-size", patrol_size_rules},
        {"patrol-null-enemy-skip", patrol_skip_rules},
        {"squad-assignment", squad_rules},
        {"nearest-enemy-filter", nearest_rules},
    };
    for (const Alone& alone : hacks)
        play_rules(package, spend, alone.make(), alone.name);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: oa-sim-brains-classic-match-test <brains/classic>\n";
        return 2;
    }
    Spend spend;
    try {
        PackageBytes package;
        package.load(argv[1]);
        test_builds(package, spend);
        test_no_income(package, spend);
        test_campaign(package, spend);
        test_silo(package, spend);
        test_builder_rounds(package, spend);
        test_rule_sets(package, spend);
        test_hacks_alone(package, spend);
        CHECK(spend.saw_tick);
    } catch (const std::exception& error) {
        if (spend.saw_tick)
            print_spend(spend);
        std::cerr << "classic match test failed: " << error.what() << '\n';
        return 1;
    }
    print_spend(spend);
    std::cout << "classic match tests passed\n";
    return 0;
}
