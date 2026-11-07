// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The rules a match plays by: the match keeps the rules, unit-type and weapon
// records it is given and hands them to the systems that apply them, and the
// state a rule keeps outside the canonical records enters the state digest,
// the trace and the computer players' view only once a rule adds a table.
// A mutator digest enters the state digest and the trace with no table.
// Owner-local tables remain saveable without entering shared digests.
#include "combat_fixture.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime/match_trace.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/state_hash.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using namespace combat_fixture;
namespace match_rules = oa::data::match_rules;
namespace runtime = oa::sim::match_runtime;
namespace trace = oa::sim::trace;

/// A rule's table for the tests: four bytes of state.
struct Counter {
    std::array<uint8_t, 4> bytes{};
    uint32_t reads{};
};

/// Returns a counter's bytes.
///
/// @param context the counter
/// @return its four bytes
std::span<const uint8_t> counter_bytes(void* context) {
    auto& counter = *static_cast<Counter*>(context);
    ++counter.reads;
    return counter.bytes;
}

/// Restores a counter from four bytes.
///
/// @param context the counter
/// @param bytes the saved bytes
/// @return false unless there are exactly four
bool counter_restore(void* context, std::span<const uint8_t> bytes) {
    auto& counter = *static_cast<Counter*>(context);
    if (bytes.size() != counter.bytes.size())
        return false;
    std::copy(bytes.begin(), bytes.end(), counter.bytes.begin());
    return true;
}

/// Describes a counter as a rule-state table.
///
/// @param name the table's name
/// @param counter the counter
/// @return the table
runtime::RuleStateTable counter_table(const char* name, Counter& counter) {
    return {name, &counter, counter_bytes, counter_restore};
}

/// Digests the match state as the savegame check does.
///
/// @param match the match
/// @return the match-state digest
uint64_t state_digest(runtime::Match& match) {
    const oa::base::game_loop::Timing timing{};
    return trace::match_state_hash(match, timing, 0, 0, nullptr);
}

/// Returns the trace's total digest of the match now.
///
/// @param match the match
/// @return the total section's digest
uint64_t total_digest(const runtime::Match& match) {
    std::vector<trace::UnitSide> sides(match.state().unit_slot_count);
    return runtime::match_tick_digest(match, sides)
        .sections[static_cast<size_t>(trace::Section::total)]
        .value;
}

/// A match without rules plays by 3.1c's, and keeps none of its own.
void match_without_rules_plays_baseline() {
    Fixture f;
    CHECK(f.match->fault() == nullptr);
    CHECK(f.match->rules() == match_rules::MatchRules{});
    CHECK(!f.match->profile_active());
    const auto view = f.match->rules_view();
    CHECK(view.match == &f.match->rules());
    CHECK(view.unit_types.empty() && view.weapons.empty());
    CHECK(view.unit_type(1) == match_rules::UnitTypeRules{});
    CHECK(view.weapon(1) == match_rules::WeaponTypeRules{});
    CHECK(f.match->fold_rule_state(0x1234) == 0x1234);
}

/// The match keeps copies of the rules and the per-type and per-weapon
/// records, which its view answers with.
void match_keeps_the_rules_it_is_given() {
    Options options{};
    options.rules.repair.rate.enabled = true;
    options.rules.repair.rate.mode = match_rules::RepairRateMode::exact_remainder;
    options.unit_type_rules.resize(2);
    options.unit_type_rules[1].build_facings =
        match_rules::build_facing::south | match_rules::build_facing::west;
    options.weapon_rules.resize(256);
    options.weapon_rules[1].not_to_air = 1;
    Fixture f(options);
    CHECK(f.match->fault() == nullptr);
    // The caller's records may go; the match answers from its own copies.
    options.unit_type_rules.assign(2, {});
    options.weapon_rules.assign(256, {});
    const auto view = f.match->rules_view();
    CHECK(view.rules().repair.rate.mode == match_rules::RepairRateMode::exact_remainder);
    CHECK(
        view.unit_type(1).build_facings ==
        (match_rules::build_facing::south | match_rules::build_facing::west)
    );
    CHECK(view.weapon(1).not_to_air == 1);
    CHECK(view.weapon(2) == match_rules::WeaponTypeRules{});
    // The computer players read the same rules.
    oa::sim::ai::prepare_match_computer_players(*f.match);
    const auto* players =
        static_cast<const oa::sim::ai::ComputerPlayers*>(f.match->computer_player_state().get());
    CHECK(players != nullptr && players->rules.match == &f.match->rules());
    CHECK(players != nullptr && players->rules.weapon(1).not_to_air == 1);
}

/// Unit-type records that do not match the type table refuse the match.
void mismatched_unit_type_rules_refuse_the_match() {
    Options options{};
    options.unit_type_rules.resize(5);
    Fixture f(options);
    CHECK(f.match->fault() != nullptr);
}

/// Tables are named once, and only as many as fit.
void rule_state_tables_are_named_once() {
    runtime::RuleState state{};
    Counter counter{};
    CHECK(runtime::add_rule_state(state, counter_table("reuse", counter)));
    CHECK(runtime::digested_rule_state_count(state) == 1);
    CHECK(!runtime::add_rule_state(state, counter_table("reuse", counter)));
    CHECK(!runtime::add_rule_state(state, counter_table("", counter)));
    CHECK(!runtime::add_rule_state(state, counter_table(nullptr, counter)));
    CHECK(!runtime::add_rule_state(state, counter_table("a-name-longer-than-allowed", counter)));
    CHECK(runtime::find_rule_state(state, "reuse") != nullptr);
    CHECK(runtime::find_rule_state(state, "facing") == nullptr);
    static constexpr std::array<const char*, runtime::rule_state_capacity> names{
        "t1",
        "t2",
        "t3",
        "t4",
        "t5",
        "t6",
        "t7",
        "t8",
        "t9",
        "t10",
        "t11",
        "t12",
        "t13",
        "t14",
        "t15"
    };
    for (const char* name : names) {
        if (name != nullptr)
            CHECK(runtime::add_rule_state(state, counter_table(name, counter)));
    }
    CHECK(state.count == runtime::rule_state_capacity);
    CHECK(runtime::digested_rule_state_count(state) == runtime::rule_state_capacity);
    CHECK(!runtime::add_rule_state(state, counter_table("full", counter)));
}

/// Keeps owner-local bytes saveable while their presence and changes leave
/// the state and trace identical to a match without such a table.
void save_only_state_stays_out_of_digests() {
    Options options{};
    options.profile_sim_hash = oa::base::sha256::Digest{};
    (*options.profile_sim_hash)[0] = 1;
    Fixture f(options);
    const auto plain_state = state_digest(*f.match);
    const auto plain_total = total_digest(*f.match);
    constexpr uint64_t initial_digest = 0x1234;
    Counter cache{};
    auto table = counter_table("route-cache", cache);
    table.digest = false;
    CHECK(runtime::add_rule_state(f.match->rule_state(), table));
    CHECK(f.match->rule_state().count == 1);
    CHECK(runtime::digested_rule_state_count(f.match->rule_state()) == 0);
    CHECK(f.match->fold_rule_state(initial_digest) == initial_digest);
    CHECK(state_digest(*f.match) == plain_state);
    CHECK(total_digest(*f.match) == plain_total);
    cache.bytes = {9, 8, 7, 6};
    CHECK(state_digest(*f.match) == plain_state);
    CHECK(total_digest(*f.match) == plain_total);
    CHECK(cache.reads == 0);
    trace::SectionDigest section{};
    CHECK(!runtime::rule_state_digest(*f.match, section));

    // Saving still sees the named table and can restore the bytes it owns.
    const auto* saved = runtime::find_rule_state(f.match->rule_state(), "route-cache");
    CHECK(saved != nullptr && !saved->digest);
    CHECK(saved->bytes(saved->context).size() == cache.bytes.size());
    CHECK(cache.reads == 1);
    constexpr std::array<uint8_t, 4> restored{1, 2, 3, 4};
    CHECK(saved->restore(saved->context, restored));
    CHECK(cache.bytes == restored);
}

/// Keeps shared table bytes and trace item counts independent of interleaved
/// owner-local tables, including when only a mutator digest is shared.
void save_only_state_does_not_displace_shared_state() {
    Options options{};
    options.profile_sim_hash = oa::base::sha256::Digest{};
    (*options.profile_sim_hash)[0] = 1;
    Fixture f(options);
    Fixture g(options);
    Counter shared_a{{1, 2, 3, 4}}, shared_b{{1, 2, 3, 4}};
    Counter cache_a{{5, 6, 7, 8}}, cache_b{{8, 7, 6, 5}};
    auto local_a = counter_table("owner-a", cache_a);
    auto local_b = counter_table("owner-b", cache_b);
    local_a.digest = local_b.digest = false;
    CHECK(runtime::add_rule_state(f.match->rule_state(), local_a));
    CHECK(runtime::add_rule_state(f.match->rule_state(), counter_table("shared", shared_a)));
    CHECK(runtime::add_rule_state(g.match->rule_state(), counter_table("shared", shared_b)));
    CHECK(runtime::add_rule_state(g.match->rule_state(), local_b));
    CHECK(state_digest(*f.match) == state_digest(*g.match));
    CHECK(total_digest(*f.match) == total_digest(*g.match));
    trace::SectionDigest section{};
    CHECK(runtime::rule_state_digest(*f.match, section) && section.items == 1);
    cache_a.bytes[0] = 3;
    CHECK(state_digest(*f.match) == state_digest(*g.match));
    CHECK(total_digest(*f.match) == total_digest(*g.match));
    CHECK(cache_a.reads == 0 && cache_b.reads == 0);
    shared_a.bytes[0] = 3;
    CHECK(state_digest(*f.match) != state_digest(*g.match));
    CHECK(total_digest(*f.match) != total_digest(*g.match));

    options.mutator_digest = oa::base::sha256::Digest{};
    (*options.mutator_digest)[0] = 1;
    Fixture held(options);
    Fixture held_with_cache(options);
    CHECK(runtime::add_rule_state(held_with_cache.match->rule_state(), local_a));
    CHECK(state_digest(*held.match) == state_digest(*held_with_cache.match));
    CHECK(total_digest(*held.match) == total_digest(*held_with_cache.match));
    CHECK(runtime::rule_state_digest(*held_with_cache.match, section) && section.items == 0);
}

/// A table enters the state digest and the trace with the profile's sim
/// hash; its bytes, and the profile, move them.
void rule_state_enters_the_digests() {
    Options options{};
    options.profile_sim_hash = oa::base::sha256::Digest{};
    (*options.profile_sim_hash)[0] = 1;
    Fixture f(options);
    CHECK(f.match->profile_active());
    const uint64_t plain_state = state_digest(*f.match);
    const uint64_t plain_total = total_digest(*f.match);
    // A profile alone keeps no rule state, so nothing moves.
    Fixture without;
    CHECK(state_digest(*without.match) == plain_state);
    CHECK(total_digest(*without.match) == plain_total);

    Counter counter{};
    CHECK(runtime::add_rule_state(f.match->rule_state(), counter_table("counter", counter)));
    const uint64_t with_table = state_digest(*f.match);
    const uint64_t total_with_table = total_digest(*f.match);
    CHECK(with_table != plain_state);
    CHECK(total_with_table != plain_total);
    CHECK(state_digest(*f.match) == with_table);
    counter.bytes[2] = 7;
    CHECK(state_digest(*f.match) != with_table);
    CHECK(total_digest(*f.match) != total_with_table);
    counter.bytes[2] = 0;
    CHECK(state_digest(*f.match) == with_table);

    // The same table under another profile digests differently.
    Options other = options;
    (*other.profile_sim_hash)[0] = 2;
    Fixture g(other);
    Counter same{};
    CHECK(runtime::add_rule_state(g.match->rule_state(), counter_table("counter", same)));
    CHECK(state_digest(*g.match) != with_table);

    // A mutator digest moves the state digest and the trace with no rule table.
    Options mutated{};
    mutated.mutator_digest = oa::base::sha256::Digest{};
    (*mutated.mutator_digest)[0] = 1;
    Fixture held(mutated);
    CHECK(held.match->mutator_digest_held());
    CHECK(held.match->fold_rule_state(0x1234) != 0x1234);
    CHECK(state_digest(*held.match) != plain_state);
    CHECK(total_digest(*held.match) != plain_total);
    Options other_mutator = mutated;
    (*other_mutator.mutator_digest)[0] = 2;
    Fixture different(other_mutator);
    CHECK(state_digest(*different.match) != state_digest(*held.match));

    // A table restores from the bytes it digested.
    const auto* table = runtime::find_rule_state(f.match->rule_state(), "counter");
    CHECK(table != nullptr);
    const std::array<uint8_t, 4> saved{9, 8, 7, 6};
    CHECK(table->restore(table->context, saved));
    CHECK(counter.bytes == saved);
    CHECK(!table->restore(table->context, std::span<const uint8_t>{saved}.first(3)));
}

} // namespace

int main() {
    try {
        match_without_rules_plays_baseline();
        match_keeps_the_rules_it_is_given();
        mismatched_unit_type_rules_refuse_the_match();
        rule_state_tables_are_named_once();
        rule_state_enters_the_digests();
        save_only_state_stays_out_of_digests();
        save_only_state_does_not_displace_shared_state();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "match rules passed\n";
    return 0;
}
