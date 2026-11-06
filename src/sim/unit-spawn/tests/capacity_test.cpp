// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "fixture.hpp"

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
    full_armies();
    std::cout << "20,000 live units in sparse participant ranges passed\n";
}
