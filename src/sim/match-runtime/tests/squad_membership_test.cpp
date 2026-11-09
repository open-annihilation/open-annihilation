// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced squad lookup keeps native vector order through transfers, deaths,
// reused slots and the application's canonical saved-squad assignment seam.
// The load cases cover membership reconstruction, not a complete AI save bank.
#include "combat_fixture.hpp"

#include "oa/data/persist/save_sections.hpp"
#include "oa/sim/match_runtime/match_trace.hpp"
#include "oa/sim/trace.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace trace = oa::sim::trace;
namespace persist = oa::data::persist;

constexpr uint32_t legacy_version = 4;
constexpr uint32_t indexed_version = 6;
constexpr uint32_t no_squad = 0xffffffffu;
constexpr uint32_t fixed_one = 65536;
constexpr std::array<uint32_t, 8> groups = {0, 1, 2, 3, 4, 6, 9, 100};

/// Keeps the fixture's units alive and prevents autonomous attacks.
Options options_for(uint32_t version) {
    Options options;
    options.rules.setup.enhanced_mode.enabled = version != 0;
    options.rules.setup.enhanced_mode.version = version;
    options.per_player_limit = 16;
    // Off retains all ten banks; match that capacity in the enabled fixtures.
    options.unit_player_mask = static_cast<uint16_t>((uint32_t{1} << OA_PLAYER_COUNT) - 1);
    options.maximum_health = 30000;
    return options;
}

/// Runs the legacy and indexed membership paths over identical native units.
struct Pair {
    Fixture legacy;
    Fixture indexed;

    /// Selects the historical path while the other match uses the indexed preset.
    explicit Pair(uint32_t baseline = legacy_version)
        : legacy(options_for(baseline)), indexed(options_for(indexed_version)) {}

    /// Creates corresponding units with the same global native slot.
    uint16_t spawn(uint8_t owner, uint32_t x, uint32_t z, uint16_t requested = 0) {
        const auto create = [&](Fixture& fixture) {
            auto* slot = fixture.match->create(
                {owner, 1, {x * fixed_one, 32u * fixed_one, z * fixed_one}, true, 1, requested}
            );
            CHECK(slot != nullptr && slot->unit != nullptr);
            slot->record.flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
            return slot->unit_index;
        };
        const auto first = create(legacy);
        const auto second = create(indexed);
        CHECK(first == second);
        compare();
        return first;
    }

    /// Applies the public membership operation in both actual matches.
    void assign(uint16_t unit, uint32_t group) {
        legacy.match->set_unit_squad(unit, group);
        indexed.match->set_unit_squad(unit, group);
        compare();
    }

    /// Compares vector order and canonical trace state without the differing rule blobs.
    void compare() {
        for (uint8_t owner = 0; owner < 2; ++owner)
            for (const auto group : groups) {
                const auto left = legacy.match->squad_members(owner, group);
                const auto right = indexed.match->squad_members(owner, group);
                CHECK(std::equal(left.begin(), left.end(), right.begin(), right.end()));
            }
        const auto count = legacy.match->state().unit_slot_count;
        CHECK(count == indexed.match->state().unit_slot_count);
        for (uint32_t slot = 0; slot < count; ++slot) {
            const auto& left = legacy.match->state().units[slot];
            const auto& right = indexed.match->state().units[slot];
            CHECK(left.squad == right.squad);
            CHECK(left.type_index == right.type_index && left.owner_index == right.owner_index);
        }
        const auto digest = [](Fixture& fixture) {
            std::vector<trace::UnitSide> sides(fixture.match->state().unit_slot_count);
            runtime::fill_trace_sides(*fixture.match, sides);
            return trace::tick_digest(
                fixture.match->state(),
                sides.data(),
                {fixture.match->random_state(), fixture.match->lcg_state()},
                nullptr
            );
        };
        const auto left = digest(legacy);
        const auto right = digest(indexed);
        CHECK(left.tick == right.tick);
        for (size_t section = 0; section < trace::section_count; ++section) {
            CHECK(left.sections[section].value == right.sections[section].value);
            CHECK(left.sections[section].items == right.sections[section].items);
        }
    }

    /// Checks a hand-worked vector in both versions, including its exact order.
    void expect(uint8_t owner, uint32_t group, std::initializer_list<uint16_t> expected) {
        for (auto* fixture : {&legacy, &indexed}) {
            const auto actual = fixture->match->squad_members(owner, group);
            CHECK(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
        }
    }

    /// Runs real unit ticks and compares their canonical continuation each tick.
    void run(uint32_t ticks) {
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            legacy.run(1);
            indexed.run(1);
            compare();
        }
    }

    /// Removes a unit through the real death and squad teardown hooks.
    void kill(uint16_t victim, uint16_t killer) {
        for (auto* fixture : {&legacy, &indexed})
            combat_fixture::kill(
                *fixture, fixture->match->spawn_slot(victim), fixture->match->spawn_slot(killer)
            );
        compare();
    }
};

/// Preserves native swap/pop/append order, including assigning the same squad.
void check_same_group_and_cross_group_moves(uint32_t baseline) {
    Pair pair(baseline);
    const auto a = pair.spawn(0, 40, 40);
    const auto b = pair.spawn(0, 80, 40);
    const auto c = pair.spawn(0, 120, 40);
    for (const auto unit : {a, b, c})
        pair.assign(unit, 1);
    pair.expect(0, 1, {a, b, c});
    pair.assign(a, 1);
    pair.expect(0, 1, {c, b, a});
    pair.assign(b, 1);
    pair.expect(0, 1, {c, a, b});
    pair.assign(c, 2);
    pair.expect(0, 1, {b, a});
    pair.expect(0, 2, {c});
    pair.assign(a, 2);
    pair.expect(0, 1, {b});
    pair.expect(0, 2, {c, a});
    pair.assign(c, no_squad);
    pair.expect(0, 2, {a});
    pair.assign(c, no_squad);
    pair.assign(c, 1);
    pair.expect(0, 1, {b, c});
    pair.run(20);
}

/// Keeps the same native vector and continuation behavior for Off and presets one to five.
void same_group_and_cross_group_moves_keep_vector_order() {
    for (uint32_t version = 0; version < indexed_version; ++version)
        check_same_group_and_cross_group_moves(version);
}

/// Exercises many current-position changes without changing native operation order.
void long_membership_sequences_match_the_legacy_path() {
    Pair pair;
    std::array<uint16_t, 12> units{};
    for (size_t index = 0; index < units.size(); ++index)
        units[index] = pair.spawn(0, 40 + uint32_t(index % 4) * 40, 40 + uint32_t(index / 4) * 40);
    for (uint32_t step = 0; step < 600; ++step) {
        const auto unit = units[(step * 7 + step / 11) % units.size()];
        const auto group =
            step % 13 == 0 ? no_squad : groups[(step * 5 + step / 17) % groups.size()];
        pair.assign(unit, group);
        if (step % 37 == 0)
            pair.run(1);
    }
    pair.run(20);
}

/// Clears dead membership before the same native slot starts another lifetime.
void death_and_exact_slot_reuse_keep_membership_exact() {
    Pair pair;
    const auto a = pair.spawn(0, 40, 40);
    const auto b = pair.spawn(0, 80, 40);
    const auto c = pair.spawn(0, 120, 40);
    const auto enemy = pair.spawn(1, 200, 200);
    for (const auto unit : {a, b, c})
        pair.assign(unit, 1);
    pair.kill(b, enemy);
    pair.expect(0, 1, {a, c});
    CHECK(pair.spawn(0, 80, 40, b) == b);
    pair.assign(b, 1);
    pair.expect(0, 1, {a, c, b});
    pair.assign(c, 1);
    pair.expect(0, 1, {a, b, c});
    pair.kill(a, enemy);
    pair.expect(0, 1, {c, b});
    pair.run(20);
}

/// Reads the saved squad bytes that the application assigns before set_unit_squad.
uint32_t saved_squad(Fixture& fixture, uint16_t unit) {
    persist::SaveContext save{&fixture.match->state(), nullptr, nullptr, nullptr, nullptr, nullptr};
    std::array<uint8_t, persist::unit_record_bytes> bytes{};
    persist::save_encode_unit_record(
        &save, &fixture.match->spawn_slot(unit).record, 0, true, bytes.data()
    );
    const size_t at = persist::unit_record::squad;
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
           uint32_t(bytes[at + 3]) << 24;
}

/// Preserves legacy duplicate vector entries left by saved canonical squad assignment.
void restored_assigned_and_unassigned_squads_keep_first_occurrence_semantics() {
    for (const auto loaded_group : {2u, no_squad}) {
        Pair saving;
        const auto saved = saving.spawn(0, 40, 40);
        saving.assign(saved, loaded_group);
        const uint32_t encoded_group = saved_squad(saving.legacy, saved);
        CHECK(encoded_group == loaded_group);

        Pair loaded;
        const auto a = loaded.spawn(0, 40, 40);
        const auto b = loaded.spawn(0, 80, 40);
        const auto c = loaded.spawn(0, 120, 40);
        // Runtime::restore_saved_unit performs this canonical write before its
        // membership callback. The original spawn-time group remains observable.
        for (auto* fixture : {&loaded.legacy, &loaded.indexed}) {
            fixture->match->spawn_slot(a).record.squad = static_cast<int32_t>(encoded_group);
            fixture->match->set_unit_squad(a, encoded_group);
        }
        loaded.compare();
        loaded.expect(0, 0, {a, b, c});
        loaded.assign(a, 0);
        loaded.expect(0, 0, {a, b, c, a});
        loaded.assign(b, 3);
        loaded.expect(0, 0, {a, a, c});
        loaded.assign(a, 4);
        loaded.expect(0, 0, {c, a});
        loaded.expect(0, 4, {a});
        // Further changes must keep the conservative fallback for this match.
        for (uint32_t step = 0; step < 40; ++step) {
            loaded.assign(step % 2 == 0 ? b : c, groups[step % groups.size()]);
            loaded.assign(a, step % 3 == 0 ? no_squad : 0);
        }
        loaded.run(20);
    }
}

/// Preserves membership through the supported gift and capture lifetime changes.
void owner_transfer_and_capture_match_the_legacy_vectors() {
    Pair pair;
    const auto gifted = pair.spawn(0, 40, 40);
    const auto captured = pair.spawn(0, 80, 40);
    const auto capturer = pair.spawn(1, 200, 200);
    pair.assign(gifted, 4);
    pair.assign(captured, 3);
    pair.assign(capturer, 9);
    for (auto* fixture : {&pair.legacy, &pair.indexed})
        fixture->match->transfer_unit(gifted, 1);
    pair.compare();
    pair.run(2);
    for (auto* fixture : {&pair.legacy, &pair.indexed})
        fixture->match->capture_unit(
            fixture->match->spawn_slot(captured), fixture->match->spawn_slot(capturer)
        );
    pair.compare();
    pair.run(2);
    uint32_t owned = 0;
    for (uint32_t slot = 1; slot < pair.indexed.match->state().unit_slot_count; ++slot) {
        const auto& unit = pair.indexed.match->state().units[slot];
        if (unit.type_index != 0 && unit.owner_index == 1) {
            ++owned;
            pair.assign(static_cast<uint16_t>(slot), 6);
        }
    }
    CHECK(owned == 3);
    pair.run(20);
}

/// Rebuilds the lightweight asset fixture with room for ten thousand distinct footprints.
void prepare_large_membership_match(Fixture& fixture, uint32_t version) {
    constexpr uint32_t terrain_width = 128;
    constexpr uint32_t terrain_cells = terrain_width * terrain_width;
    constexpr uint32_t sight_width = terrain_width / 2;
    constexpr uint16_t per_player_limit = 10000;
    fixture.match.reset();
    fixture.map.attribute_width = fixture.map.attribute_height = terrain_width;
    fixture.map.attributes.assign(terrain_cells, {});
    fixture.terrain_values.assign(terrain_cells, {});
    fixture.collision_plots.assign(terrain_cells, {});
    fixture.altitude_cells.assign(sight_width * sight_width, {});
    runtime::OfflineInputs input{
        fixture.map,
        fixture.loaded,
        fixture.types,
        fixture.fields,
        fixture.weapons,
        fixture.terrain_values,
        fixture.masks,
        sight_width,
        sight_width,
        per_player_limit,
        2,
        0,
        30,
        1,
        &fixture.scenario,
        {},
        fixture.collision_plots,
        oa::sim::visibility_state::AltitudeSightData{
            sight_width, sight_width, fixture.altitude_cells, fixture.altitude_patterns
        }
    };
    input.rules = options_for(version).rules;
    input.unit_player_mask = 3;
    fixture.match = std::make_unique<runtime::Match>(input, fixture.services);
    fixture.match->configure_strategic_environment({0, 0.5F, 0});
    for (uint8_t owner = 0; owner < 2; ++owner) {
        fixture.match->simulation().players[owner].present = true;
        fixture.match->simulation().players[owner].status = 1;
        std::array<uint8_t, OA_PLAYER_COUNT> allies{};
        allies[owner] = 1;
        fixture.match->configure_player_alliances(owner, allies);
        if (owner == 0)
            fixture.match->configure_outcomes(owner, allies, false);
    }
    CHECK(fixture.match->fault() == nullptr);
}

/// Preserves the exact two vectors when five thousand of ten thousand members transfer.
void ten_thousand_members_keep_exact_batch_transfer_order() {
    Pair pair;
    prepare_large_membership_match(pair.legacy, legacy_version);
    prepare_large_membership_match(pair.indexed, indexed_version);
    constexpr uint16_t members = 10000;
    constexpr uint16_t transferred = 5000;
    constexpr uint32_t row_members = 100;
    for (auto* fixture : {&pair.legacy, &pair.indexed})
        for (uint16_t index = 0; index < members; ++index) {
            const uint16_t slot = index + 1;
            const uint32_t x = (uint32_t(index) % row_members + 1) * 16 + 8;
            const uint32_t z = (uint32_t(index) / row_members + 1) * 16 + 8;
            auto* created = fixture->match->create(
                {0, 1, {x * fixed_one, 32u * fixed_one, z * fixed_one}, true, 1, slot}
            );
            CHECK(created != nullptr && created->unit != nullptr && created->unit_index == slot);
            created->record.flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
        }
    CHECK(pair.legacy.match->state().unit_slot_count == 20001);
    pair.compare();
    for (auto* fixture : {&pair.legacy, &pair.indexed})
        for (uint16_t slot = 1; slot <= transferred; ++slot)
            fixture->match->set_unit_squad(slot, 2);
    pair.compare();
    for (auto* fixture : {&pair.legacy, &pair.indexed}) {
        const auto source = fixture->match->squad_members(0, 0);
        const auto destination = fixture->match->squad_members(0, 2);
        CHECK(source.size() == members - transferred && destination.size() == transferred);
        for (uint16_t index = 0; index < transferred; ++index) {
            CHECK(source[index] == members - index);
            CHECK(destination[index] == index + 1);
            CHECK(fixture->match->spawn_slot(index + 1).record.squad == 2);
            CHECK(fixture->match->spawn_slot(members - index).record.squad == 0);
        }
    }
}

/// Reports every independent failure without skipping the remaining cases.
template <typename Function>
int run_case(std::string_view name, Function function) {
    try {
        function();
        std::cout << "PASS " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        return 1;
    }
}
} // namespace

/// Runs actual-match vector, lifecycle, load-seam and canonical continuation checks.
int main() {
    int failures = 0;
    failures +=
        run_case("same and cross squad", same_group_and_cross_group_moves_keep_vector_order);
    failures +=
        run_case("long membership sequence", long_membership_sequences_match_the_legacy_path);
    failures += run_case("death and reused slot", death_and_exact_slot_reuse_keep_membership_exact);
    failures += run_case(
        "saved membership seam",
        restored_assigned_and_unassigned_squads_keep_first_occurrence_semantics
    );
    failures +=
        run_case("owner transfer and capture", owner_transfer_and_capture_match_the_legacy_vectors);
    failures +=
        run_case("ten thousand members", ten_thousand_members_keep_exact_batch_transfer_order);
    return failures == 0 ? 0 : 1;
}
