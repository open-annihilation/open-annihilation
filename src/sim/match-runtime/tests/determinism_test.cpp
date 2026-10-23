// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whole-match determinism: a synthetic three-a-side skirmish of fights,
// moves and patrols runs 360 ticks, in which the two duellists destroy each
// other, and its match-state digest (the one the savegame check compares)
// and its trace digests must equal the values pinned below on every
// platform, and two runs must agree tick for tick.
// A change that moves a pinned value changes what the simulation computes;
// update the constant only with the reason in the commit message.
//
// The same skirmish played under a mod profile that turns every limit and
// hack on at its baseline preset (baseline_profile_text) must give the same
// pinned values: a rule at its 3.1c baseline plays exactly as 3.1c.
#include "combat_fixture.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/sim/match_runtime/match_trace.hpp"
#include "oa/sim/state_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace combat_fixture;
namespace trace = oa::sim::trace;

constexpr uint32_t match_ticks = 360;
constexpr uint32_t halfway_tick = 180;

struct Pinned {
    uint64_t halfway_state{}; // match_state_hash after halfway_tick
    uint64_t final_state{};   // match_state_hash after match_ticks
    uint64_t final_total{};   // the last tick's total section digest
    uint64_t every_total{};   // digest_fold of every tick's total, low word then high
    uint32_t live_units{};    // live units after match_ticks
};

constexpr Pinned pinned{
    .halfway_state = 0x60f37308a7eb5e14ull,
    .final_state = 0xf7ca3a754b0db9c8ull,
    .final_total = 0x0e75170de962eee6ull,
    .every_total = 0x553c0fdf106aa43aull,
    .live_units = 4,
};

struct Run {
    std::vector<uint64_t> totals; // each tick's total section digest
    uint64_t halfway_state{};
    uint64_t final_state{};
    uint32_t live_units{};
    bool duellists_destroyed{};
};

/// Tells whether a unit slot holds a live unit.
///
/// @param world canonical state
/// @param slot unit slot
/// @return true when the slot has a type and the live flag
bool live(const World& world, uint32_t slot) {
    return world.units[slot].type_index != 0 && (world.units[slot].flags & OA_UNIT_FLAG_LIVE) != 0;
}

/// Returns the total section digest of the match's current state, its rule
/// state included.
///
/// @param match match to sample
/// @param[in,out] sides scratch table of one entry per unit slot
/// @return the total section's 64-bit digest
uint64_t total_digest(const sim::match_runtime::Match& match, std::vector<trace::UnitSide>& sides) {
    sides.assign(match.state().unit_slot_count, {});
    const auto digest = sim::match_runtime::match_tick_digest(match, sides);
    return digest.sections[static_cast<size_t>(trace::Section::total)].value;
}

/// Digests the match state as the savegame check does, with a default clock,
/// the camera at the origin and no meteor storm.
///
/// @param match match to digest
/// @return the match-state digest
uint64_t state_digest(sim::match_runtime::Match& match) {
    const base::game_loop::Timing timing{};
    return trace::match_state_hash(match, timing, 0, 0, nullptr);
}

/// Plays the skirmish: player 0's first two units attack player 1's first,
/// which attacks player 0's first back; player 1's second unit marches on
/// player 0's side; each player's third unit patrols across the map.
///
/// @param options the rules the match plays by; 3.1c's by default
/// @param keep_contacts keep players 0 and 1's radar pictures from the start
/// @return each tick's total digest and the values pinned above
Run play(const Options& options = {}, bool keep_contacts = false) {
    Fixture f(options);
    if (keep_contacts) {
        f.match->keep_player_contacts(0, true);
        f.match->keep_player_contacts(1, true);
    }
    auto& gunner = f.spawn(0, 48, 48);
    auto& second = f.spawn(0, 48, 112);
    auto& scout = f.spawn(0, 64, 208);
    auto& target = f.spawn(1, 176, 64);
    auto& marcher = f.spawn(1, 208, 128);
    auto& patroller = f.spawn(1, 200, 208);
    const uint16_t gunner_slot = gunner.unit_index;
    const uint16_t target_slot = target.unit_index;
    CHECK(f.match->issue_attack(gunner_slot, target_slot, true));
    CHECK(f.match->issue_attack(second.unit_index, target_slot, true));
    CHECK(f.match->issue_attack(target_slot, gunner_slot, true));
    f.match->issue_ground_move(marcher.unit_index, {96 << 16, 0, 128 << 16}, false);
    f.match->issue_patrol(scout.unit_index, {200 << 16, 0, 40 << 16}, false);
    f.match->issue_patrol(patroller.unit_index, {40 << 16, 0, 176 << 16}, false);
    Run run;
    std::vector<trace::UnitSide> sides;
    for (uint32_t tick = 1; tick <= match_ticks; ++tick) {
        f.run(1);
        run.totals.push_back(total_digest(*f.match, sides));
        if (tick == halfway_tick)
            run.halfway_state = state_digest(*f.match);
    }
    run.final_state = state_digest(*f.match);
    const auto& world = f.match->state();
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot)
        run.live_units += live(world, slot) ? 1u : 0u;
    run.duellists_destroyed = !live(world, gunner_slot) && !live(world, target_slot);
    return run;
}

/// Folds every tick's total digest into one value, low word then high word.
///
/// @param totals each tick's total digest
/// @return the fold, from trace::digest_basis
uint64_t fold_totals(const std::vector<uint64_t>& totals) {
    uint64_t fold = trace::digest_basis;
    for (const uint64_t total : totals) {
        fold = trace::digest_fold(fold, static_cast<uint32_t>(total));
        fold = trace::digest_fold(fold, static_cast<uint32_t>(total >> 32));
    }
    return fold;
}

/// Checks a pinned value, printing the value found when it differs.
///
/// @param what name printed
/// @param found value the run produced
/// @param expected value pinned in this file
/// @return whether they are equal
bool matches_pinned(const char* what, uint64_t found, uint64_t expected) {
    if (found != expected)
        std::cerr << what << ": found 0x" << std::hex << found << ", pinned 0x" << expected
                  << std::dec << '\n';
    return found == expected;
}

/// Plays the skirmish twice; the runs must agree with each other and with the pinned values.
///
/// @param options the rules the match plays by
/// @param keep_contacts keep players 0 and 1's radar pictures from the start
void runs_repeat_and_match_pins(const Options& options = {}, bool keep_contacts = false) {
    const Run first = play(options, keep_contacts);
    const Run second = play(options, keep_contacts);
    CHECK(first.totals.size() == match_ticks);
    CHECK(first.duellists_destroyed);
    CHECK(first.totals == second.totals);
    CHECK(first.halfway_state == second.halfway_state && first.final_state == second.final_state);
    CHECK(first.halfway_state != first.final_state);
    // Every pinned value is compared, so a failure prints all that moved.
    bool held = matches_pinned("halfway state digest", first.halfway_state, pinned.halfway_state);
    held = matches_pinned("final state digest", first.final_state, pinned.final_state) && held;
    held = matches_pinned("final total digest", first.totals.back(), pinned.final_total) && held;
    held = matches_pinned(
               "every tick's total digest", fold_totals(first.totals), pinned.every_total
           ) &&
           held;
    held = matches_pinned("live units", first.live_units, pinned.live_units) && held;
    CHECK(held);
}

/// Resolves the profile that turns every limit and hack on at its baseline
/// preset, accepting hacks not implemented yet.
///
/// @return the match options its rules, limits and sim hash give
Options baseline_profile_options() {
    const std::string text = oa::data::mod_profile::baseline_profile_text();
    oa::data::mod_profile::ResolveOptions resolve{};
    resolve.accept_unimplemented_hacks = true;
    const auto result = oa::data::mod_profile::resolve_profile(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()},
        "baseline-rules",
        resolve
    );
    for (const auto& error : result.errors)
        std::cerr << oa::data::mod_profile::format_diagnostic(error) << '\n';
    CHECK(result.resolution.has_value());
    const auto& profile = result.resolution->profile;
    Options options{};
    options.rules = profile.rules;
    options.limits = profile.limits;
    options.profile_sim_hash = profile.sim_hash;
    return options;
}

/// Plays the skirmish under the profile of every rule at its baseline; it
/// must keep no rule state and match the pins.
void baseline_profile_matches_pins() {
    const Options options = baseline_profile_options();
    CHECK(!(options.rules == oa::data::match_rules::MatchRules{}));
    {
        const Fixture f(options);
        CHECK(f.match->profile_active());
        CHECK(f.match->rules() == options.rules);
        CHECK(f.match->rule_state().count == 0);
    }
    runs_repeat_and_match_pins(options);
}

/// Plays the skirmish with players 0 and 1's radar pictures kept from the start.
/// The pinned digests must still hold: a picture is not part of the simulation.
void kept_contacts_match_pins() {
    runs_repeat_and_match_pins({}, true);
}

} // namespace

int main() {
    try {
        runs_repeat_and_match_pins();
        baseline_profile_matches_pins();
        kept_contacts_match_pins();
        std::cout << "match determinism passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
