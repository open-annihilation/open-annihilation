// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// match-determinism's synthetic skirmish, played with a primary event table
// and any listeners. The digests are the pins match-determinism keeps, so an
// event check can prove a run with hooks reaches the same match.
#pragma once

#include "combat_fixture.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/sim/match_runtime/match_trace.hpp"
#include "oa/sim/state_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace event_skirmish {

using namespace combat_fixture;
namespace trace = oa::sim::trace;
using sim::match_runtime::EventHooks;

inline constexpr uint32_t match_ticks = 360;
inline constexpr uint32_t halfway_tick = 180;

// match-determinism's pinned digests (tests/determinism_test.cpp); the run
// with hooks must reach the same.
inline constexpr uint64_t pinned_halfway_state = 0x60f37308a7eb5e14ull;
inline constexpr uint64_t pinned_final_state = 0xf7ca3a754b0db9c8ull;
inline constexpr uint64_t pinned_final_total = 0x0e75170de962eee6ull;

/// Tells whether a unit slot holds a live unit.
///
/// @param world canonical state
/// @param slot unit slot
/// @return true when the slot has a type and the live flag
inline bool live(const World& world, uint32_t slot) {
    return world.units[slot].type_index != 0 && (world.units[slot].flags & OA_UNIT_FLAG_LIVE) != 0;
}

/// Returns the total section digest of the match's current state.
///
/// @param match match to sample
/// @param[in,out] sides scratch table of one entry per unit slot
/// @return the total section's 64-bit digest
inline uint64_t
total_digest(const sim::match_runtime::Match& match, std::vector<trace::UnitSide>& sides) {
    const auto& world = match.state();
    sides.assign(world.unit_slot_count, {});
    sim::match_runtime::fill_trace_sides(match, sides);
    const trace::RandomState random{match.random_state(), match.lcg_state()};
    const auto digest = trace::tick_digest(world, sides.data(), random);
    return digest.sections[static_cast<size_t>(trace::Section::total)].value;
}

/// Digests the match state as the savegame check does, with a default clock,
/// the camera at the origin and no meteor storm.
///
/// @param match match to digest
/// @return the match-state digest
inline uint64_t state_digest(sim::match_runtime::Match& match) {
    const base::game_loop::Timing timing{};
    return trace::match_state_hash(match, timing, 0, 0, nullptr);
}

struct Run {
    std::vector<uint64_t> totals; // each tick's total section digest
    uint64_t halfway_state{};
    uint64_t final_state{};
    std::vector<uint16_t> units; // the six spawned slots, in spawn order
    std::vector<uint8_t> owners; // their owners
    uint16_t gunner{};
    uint16_t target{};
};

/// Plays match-determinism's skirmish: player 0's first two units attack
/// player 1's first, which attacks player 0's first back; player 1's second
/// unit marches on player 0's side; each player's third unit patrols across
/// the map.
///
/// @param hooks event hooks set before the first unit is created
/// @param listeners listeners added after the primary table, before the first
///        unit is created
/// @return each tick's total digest, both state digests and the units
inline Run play(const EventHooks& hooks, std::span<const EventHooks> listeners = {}) {
    Fixture f;
    f.match->event_hooks = hooks;
    for (const EventHooks& listener : listeners)
        CHECK(f.match->add_event_listener(listener) != 0);
    auto& gunner = f.spawn(0, 48, 48);
    auto& second = f.spawn(0, 48, 112);
    auto& scout = f.spawn(0, 64, 208);
    auto& target = f.spawn(1, 176, 64);
    auto& marcher = f.spawn(1, 208, 128);
    auto& patroller = f.spawn(1, 200, 208);
    Run run;
    for (const auto* slot : {&gunner, &second, &scout, &target, &marcher, &patroller}) {
        run.units.push_back(slot->unit_index);
        run.owners.push_back(slot->record.owner_index);
    }
    run.gunner = gunner.unit_index;
    run.target = target.unit_index;
    CHECK(f.match->issue_attack(run.gunner, run.target, true));
    CHECK(f.match->issue_attack(second.unit_index, run.target, true));
    CHECK(f.match->issue_attack(run.target, run.gunner, true));
    f.match->issue_ground_move(marcher.unit_index, {96 << 16, 0, 128 << 16}, false);
    f.match->issue_patrol(scout.unit_index, {200 << 16, 0, 40 << 16}, false);
    f.match->issue_patrol(patroller.unit_index, {40 << 16, 0, 176 << 16}, false);
    std::vector<trace::UnitSide> sides;
    for (uint32_t tick = 1; tick <= match_ticks; ++tick) {
        f.run(1);
        run.totals.push_back(total_digest(*f.match, sides));
        if (tick == halfway_tick)
            run.halfway_state = state_digest(*f.match);
    }
    run.final_state = state_digest(*f.match);
    CHECK(!live(f.match->state(), run.gunner) && !live(f.match->state(), run.target));
    return run;
}

} // namespace event_skirmish
