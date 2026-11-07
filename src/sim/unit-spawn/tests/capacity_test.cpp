// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "fixture.hpp"
#include "oa/sim/unit_spawn/restore_ranges.hpp"

#include <cstring>

namespace {
constexpr uint16_t army_size = 10000;
constexpr uint8_t first_player = 3;
constexpr uint8_t second_player = 9;
constexpr uint16_t participants = (1u << first_player) | (1u << second_player);

/// Keeps the classic ten equal blocks and its original 16-bit capacity bound.
void classic_ranges() {
    CHECK(unit_pool_size(army_size) == 0);
    CHECK(unit_pool_size(6553) == 65531);
    CHECK(unit_pool_size(6554) == 0);
    constexpr uint16_t limit = 3;
    SpawnWorld world(unit_pool_size(limit), 2);
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
        world.player(player).index = player;
    CHECK(init_unit_pool(*world, limit));
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const auto first = static_cast<uint32_t>(player) * limit + 1;
        const auto& owner = world.player(player);
        CHECK(owner.base_unit_id == first);
        CHECK(owner.last_unit_id == first + limit - 1);
        uint32_t count = 0;
        CHECK(oa::world_player_units(&*world, &owner, &count) == &world.units[first]);
        CHECK(count == limit);
    }
}

/// Rejects invalid masks and ids the existing wire and weapon fields cannot carry.
void capacity_bounds() {
    CHECK(unit_pool_size(0, participants) == 0);
    CHECK(unit_pool_size(army_size, 0) == 0);
    CHECK(unit_pool_size(army_size, 1u << OA_PLAYER_COUNT) == 0);
    CHECK(unit_pool_size(army_size, participants) == 20001);
    CHECK(unit_pool_size(army_size, 7) == 30001);
    CHECK(unit_pool_size(army_size, 15) == 0);
    CHECK(unit_pool_size(32767, 1) == 32768);
    CHECK(unit_pool_size(32768, 1) == 0);
    SpawnWorld too_small(20000, 2);
    too_small.player(first_player).base_unit_id = 7;
    CHECK(!init_unit_pool(*too_small, army_size, participants));
    CHECK(too_small.player(first_player).base_unit_id == 7);
    CHECK((*too_small).game.units_per_player == 0);
}

/// Restores swapped saved ranges while retaining every physical unit id.
void restored_range_order() {
    SpawnWorld world(unit_pool_size(army_size, participants), 2);
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
        world.player(player).index = player;
    CHECK(init_unit_pool(*world, army_size, participants));
    const std::array<uint8_t, 2> order{second_player, first_player};
    CHECK(restore_unit_player_order(*world, order));
    CHECK(world.player(second_player).base_unit_id == 1);
    CHECK(world.player(second_player).last_unit_id == army_size);
    CHECK(world.player(first_player).base_unit_id == army_size + 1);
    CHECK(world.player(first_player).last_unit_id == 2 * army_size);
    for (uint32_t slot = 1; slot < world.units.size(); ++slot) {
        const auto player = slot <= army_size ? second_player : first_player;
        const auto& unit = world.units[slot];
        CHECK(unit.id == slot);
        CHECK(unit.owner_index == player);
        CHECK(oa::world_unit_owner(&*world, &unit) == &world.player(player));
        CHECK(unit.squad == -1);
    }
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
        if ((participants & (1u << player)) == 0) {
            const auto& owner = world.player(player);
            CHECK(owner.first_unit == 0 && owner.last_unit == 0);
            CHECK(owner.base_unit_id == 0 && owner.last_unit_id == 0);
        }
    CHECK(world.units[0].id == 0 && world.units[0].owner_index == 0xff);
}

/// Rejects malformed saved orders and occupied pools without changing any record.
void malformed_range_order() {
    constexpr uint16_t limit = 3;
    SpawnWorld world(unit_pool_size(limit, participants), 2);
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
        world.player(player).index = player;
    CHECK(init_unit_pool(*world, limit, participants));
    const auto unchanged = [&](std::span<const uint8_t> order) {
        const auto before_game = std::make_unique<oa::Game>((*world).game);
        const auto before_units = world.units;
        const auto before_count = (*world).unit_slot_count;
        CHECK(!restore_unit_player_order(*world, order));
        CHECK(std::memcmp(before_game.get(), &(*world).game, sizeof(oa::Game)) == 0);
        CHECK(
            std::memcmp(
                before_units.data(), world.units.data(), before_units.size() * sizeof(oa::Unit)
            ) == 0
        );
        CHECK((*world).unit_slot_count == before_count);
    };
    const std::array<uint8_t, 2> order{second_player, first_player};
    const std::array<uint8_t, 2> duplicate{first_player, first_player};
    const std::array<uint8_t, 2> invalid{first_player, OA_PLAYER_COUNT};
    const std::array<uint8_t, 1> short_order{first_player};
    unchanged({});
    unchanged(duplicate);
    unchanged(invalid);
    unchanged(short_order);
    world.units[1].flags |= OA_UNIT_FLAG_LIVE;
    unchanged(order);
    world.units[1].flags = 0;
    world.units[1].type_index = 1;
    unchanged(order);
    world.units[1].type_index = 0;
    world.units[1].id = 2;
    unchanged(order);
    world.units[1].id = 1;
    (*world).game.units_per_player = 0;
    unchanged(order);
    (*world).game.units_per_player = limit;
    --(*world).unit_slot_count;
    unchanged(order);
    ++(*world).unit_slot_count;
    (*world).game.units_per_player = 20000;
    unchanged(order);
}

/// Spawns two complete armies in sparse owner slots and rejects every overflow.
void full_armies() {
    SpawnWorld world(unit_pool_size(army_size, participants), 2);
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
        world.player(player).index = player;
    CHECK(init_unit_pool(*world, army_size, participants));
    auto& type = world.types[1];
    type.simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
    type.simulation.maximum_health = 100;
    type.footprint_x = 1;
    type.footprint_z = 1;
    type.model = 10;
    world.load_types();
    Fixture host;
    Request request;
    request.type = 1;
    request.finished = true;
    request.state = ground_occupancy_state;
    uint32_t next_slot = 1;
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        auto& owner = world.player(player);
        uint32_t count = 0;
        auto* first = oa::world_player_units(&*world, &owner, &count);
        request.player = player;
        if ((participants & (1u << player)) == 0) {
            CHECK(first == nullptr && count == 0);
            CHECK(owner.base_unit_id == 0 && owner.last_unit_id == 0);
            CHECK(create(*world, world.tables, request, host) == nullptr);
            continue;
        }
        CHECK(first == &world.units[next_slot] && count == army_size);
        CHECK(owner.base_unit_id == next_slot);
        CHECK(owner.last_unit_id == next_slot + army_size - 1);
        for (uint32_t offset = 0; offset < army_size; ++offset) {
            request.requested_slot = static_cast<uint16_t>(next_slot + offset);
            auto* unit = create(*world, world.tables, request, host);
            CHECK(unit == &world.units[next_slot + offset]);
            CHECK(unit->id == next_slot + offset);
            CHECK(unit->owner_index == player);
            CHECK(oa::world_unit_owner(&*world, unit) == &owner);
            CHECK(unit->type_index == 1);
            CHECK((unit->flags & OA_UNIT_FLAG_LIVE) != 0);
            host.calls.clear();
        }
        CHECK(owner.unit_count == army_size && owner.units_created == army_size);
        request.requested_slot = 0;
        CHECK(create(*world, world.tables, request, host) == nullptr);
        request.requested_slot = static_cast<uint16_t>(next_slot == 1 ? 10001 : 1);
        CHECK(create(*world, world.tables, request, host) == nullptr);
        request.requested_slot = 0;
        next_slot += army_size;
    }
    CHECK(next_slot == 20001);
    CHECK(world.units[0].type_index == 0 && world.units[0].owner_index == 0xff);
    CHECK((*world).game.unit_slot_count == 20001);
}
} // namespace

/// Runs the classic and Enhanced Mode unit capacity checks.
int main() {
    classic_ranges();
    capacity_bounds();
    restored_range_order();
    malformed_range_order();
    full_armies();
    std::cout << "20,000 live units in sparse participant ranges passed\n";
}
