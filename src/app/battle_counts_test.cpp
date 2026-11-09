// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Live army counts exclude lethal and unused records, include positive-health
// construction, follow births and ownership changes, and leave the world untouched.
#include "battle_counts.hpp"

#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/test/check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

namespace battle = oa::app::battle_counts;

constexpr size_t unit_slots = 12;
constexpr uint16_t unit_type = 1;
constexpr int16_t initial_health = 100;

struct Fixture {
    std::array<oa::Unit, unit_slots> units{};
    oa::World world{};

    /// Binds the canonical records to a small fixed unit table.
    Fixture() {
        world.units = units.data();
        world.unit_slot_count = static_cast<uint32_t>(units.size());
    }
};

/// Makes a typed, live unit with the given owner and health.
///
/// @param owner player slot
/// @param health current health
/// @return the unit record
oa::Unit live_unit(uint8_t owner, int16_t health = initial_health) {
    oa::Unit unit{};
    unit.type_index = unit_type;
    unit.owner_index = owner;
    unit.flags = OA_UNIT_FLAG_LIVE;
    unit.health = health;
    return unit;
}

/// Excludes pending deaths, nonlive records, lethal health and unused types.
void test_only_living_typed_units_count() {
    Fixture fixture;
    fixture.units[1] = live_unit(0);
    fixture.units[2] = live_unit(1);
    fixture.units[2].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    fixture.units[3] = live_unit(1);
    fixture.units[3].flags = 0;
    fixture.units[4] = live_unit(1, 0);
    fixture.units[5] = live_unit(1, -1);
    fixture.units[6] = live_unit(1);
    fixture.units[6].type_index = 0;
    fixture.units[7] = live_unit(0);
    fixture.units[7].flags |= OA_UNIT_FLAG_SELECTED;

    battle::Counts expected{};
    expected[0] = 2;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);
}

/// Keeps the last valid player and rejects owner bytes outside the player table.
void test_owner_boundaries() {
    Fixture fixture;
    constexpr auto last_player = static_cast<uint8_t>(OA_PLAYER_COUNT - 1);
    fixture.units[1] = live_unit(last_player, 1);
    fixture.units[2] = live_unit(OA_PLAYER_COUNT);
    fixture.units[3] = live_unit(std::numeric_limits<uint8_t>::max());
    fixture.units[4] = live_unit(0);

    battle::Counts expected{};
    expected[0] = 1;
    expected[last_player] = 1;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);
}

/// Ignores reserved slot zero and records outside the declared table extent.
void test_reserved_slot_and_table_bounds() {
    Fixture fixture;
    fixture.units[0] = live_unit(0);
    fixture.units[1] = live_unit(1);
    fixture.units[2] = live_unit(1);
    fixture.units[3] = live_unit(0);
    fixture.world.unit_slot_count = 3;

    battle::Counts expected{};
    expected[1] = 2;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);

    fixture.world.unit_slot_count = 0;
    OA_CHECK(battle::count_live_units(fixture.world) == battle::Counts{});
    fixture.world.unit_slot_count = 1;
    OA_CHECK(battle::count_live_units(fixture.world) == battle::Counts{});
    fixture.world.units = nullptr;
    fixture.world.unit_slot_count = static_cast<uint32_t>(fixture.units.size());
    OA_CHECK(battle::count_live_units(fixture.world) == battle::Counts{});
}

/// Counts unfinished positive-health units without counting zero-health nanoframes.
void test_construction_health_and_player_counter_independence() {
    Fixture fixture;
    fixture.units[1] = live_unit(0, 0);
    fixture.units[1].build_remaining = 1.0F;
    fixture.units[2] = live_unit(0, 1);
    fixture.units[2].build_remaining = 0.75F;
    fixture.units[3] = live_unit(1);
    fixture.world.game.players[0].unit_count = std::numeric_limits<uint16_t>::max();
    fixture.world.game.players[1].unit_count = 0;

    battle::Counts expected{};
    expected[0] = 1;
    expected[1] = 1;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);

    fixture.units[1].health = 1;
    fixture.units[1].build_remaining = 0.99F;
    expected[0] = 2;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);
}

/// Recounts death, a reused slot and a new owner without waiting for another tick.
void test_death_birth_and_owner_changes_are_fresh() {
    Fixture fixture;
    fixture.units[1] = live_unit(0);
    fixture.world.game.tick = 30;
    battle::Counts expected{};
    expected[0] = 1;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);

    fixture.units[1].health = 0;
    fixture.units[1].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    OA_CHECK(battle::count_live_units(fixture.world) == battle::Counts{});

    fixture.units[1] = live_unit(1);
    expected[0] = 0;
    expected[1] = 1;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);

    fixture.units[1].owner_index = 0;
    expected[0] = 1;
    expected[1] = 0;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);
    OA_CHECK(fixture.world.game.tick == 30);
}

/// Preserves every byte of the canonical world and the unit records it reads.
void test_counting_is_read_only() {
    Fixture fixture;
    fixture.units[1] = live_unit(0);
    fixture.units[2] = live_unit(1, 0);
    fixture.units[2].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    fixture.world.game.tick = 97;
    fixture.world.game.players[0].unit_count = 7;

    std::array<std::byte, sizeof(fixture.world)> world_before{};
    std::array<std::byte, sizeof(fixture.units)> units_before{};
    std::memcpy(world_before.data(), &fixture.world, world_before.size());
    std::memcpy(units_before.data(), fixture.units.data(), units_before.size());

    battle::Counts expected{};
    expected[0] = 1;
    OA_CHECK(battle::count_live_units(fixture.world) == expected);
    OA_CHECK(std::memcmp(world_before.data(), &fixture.world, world_before.size()) == 0);
    OA_CHECK(std::memcmp(units_before.data(), fixture.units.data(), units_before.size()) == 0);
}

} // namespace

/// Runs the live army census regressions.
int main() {
    test_only_living_typed_units_count();
    test_owner_boundaries();
    test_reserved_slot_and_table_bounds();
    test_construction_health_and_player_counter_independence();
    test_death_birth_and_owner_changes_are_fresh();
    test_counting_is_read_only();
    return oa::test::check_exit_status();
}
