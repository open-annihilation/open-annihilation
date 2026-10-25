// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/replication/deltas.hpp"
#include "oa/netgame/replication.hpp"
#include "oa/formats/tad.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

using namespace oa;
using namespace oa::netgame;
using Bytes = std::vector<uint8_t>;

namespace {

int failures = 0;
const char* current_test = "";

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s: %s:%d: %s\n", current_test, __FILE__, __LINE__, #condition); \
        }                                                                                          \
    } while (0)

bool same_target(const AirTargetGoal& a, const AirTargetGoal& b) {
    return a.flags == b.flags && a.query_point == b.query_point && a.target_unit == b.target_unit &&
           a.arrival_radius == b.arrival_radius && a.altitude == b.altitude &&
           a.bearing == b.bearing &&
           std::equal(std::begin(a.position), std::end(a.position), std::begin(b.position));
}

Bytes encode(const auto& fill) {
    std::vector<uint8_t> storage(unit_state_writer_words * bit_stream_word_bytes);
    BitWriter writer;
    bit_writer_init(&writer, storage.data(), unit_state_writer_words);
    fill(&writer);
    storage.resize(bit_writer_byte_length(&writer));
    return storage;
}

// ---- delta layouts ----

void air_delta_layouts_round_trip() {
    AirDelta target{};
    target.goal_tag = air_goal_tag_target;
    target.target.flags = air_target_has_unit | air_target_has_arrival_radius |
                          air_target_has_altitude | air_target_has_bearing |
                          air_target_has_position | 0x02;
    target.target.query_point = -5;
    target.target.target_unit = 0x1234;
    target.target.arrival_radius = 7;
    target.target.altitude = -8;
    target.target.bearing = 9;
    target.target.position[0] = 0x01020304;
    target.target.position[1] = -1;
    target.target.position[2] = 0x7fffffff;
    target.substate = 2;
    AirDelta seek{};
    seek.goal_tag = air_goal_tag_seek;
    seek.seek.flag = true;
    seek.seek.point[1] = 11;
    seek.seek.step[2] = -12;
    seek.seek.heading = 0xbeef;
    seek.substate = 1;
    for (const auto& delta : {target, seek, AirDelta{}}) {
        const auto bytes = encode([&](BitWriter* w) { write_air_delta(w, delta); });
        BitReader reader;
        bit_reader_init(&reader, bytes.data(), bytes.size());
        AirDelta back{};
        read_air_delta(&reader, &back);
        CHECK(!bit_reader_overrun(&reader));
        CHECK(back.goal_tag == delta.goal_tag && back.substate == delta.substate);
        CHECK(same_target(back.target, delta.target));
        CHECK(back.seek.flag == delta.seek.flag && back.seek.heading == delta.seek.heading);
        CHECK(
            std::equal(
                std::begin(back.seek.point), std::end(back.seek.point), std::begin(delta.seek.point)
            )
        );
        CHECK(
            std::equal(
                std::begin(back.seek.step), std::end(back.seek.step), std::begin(delta.seek.step)
            )
        );
    }
    // Bit counts: tag 2 + substate 2, plus 8 flag bits and 16/16/16/16/16/96,
    // or 1 + 192 + 16.
    CHECK(encode([](BitWriter* w) { write_air_delta(w, AirDelta{}); }).size() == 1);
    const auto seek_bytes = encode([&](BitWriter* w) { write_air_delta(w, seek); });
    CHECK(seek_bytes.size() == (4 + 1 + 192 + 16 + 7) / 8);
    // A goal of another kind: no tag, only the substate.
    AirDelta omitted{};
    omitted.goal_tag_omitted = true;
    omitted.substate = 3;
    const auto omitted_bytes = encode([&](BitWriter* w) { write_air_delta(w, omitted); });
    CHECK(omitted_bytes.size() == 1 && omitted_bytes[0] == 3);
}

void driver_delta_bookkeeping() {
    MovementRecord ground{};
    ground.driver = MovementClass::ground;
    CHECK(!ground_driver_has_delta(ground));
    ground.flags = movement_blocked;
    CHECK(ground_driver_has_delta(ground));
    ground.ground.flags = ground_driver_path_set | ground_driver_resend;
    ground.ground.path_count = 7;
    for (int i = 0; i < 7; ++i) {
        ground.ground.path[i][0] = static_cast<int16_t>(i * 10);
        ground.ground.path[i][1] = static_cast<int16_t>(-i);
    }
    WaypointDelta taken{};
    ground_driver_take_delta(&ground, &taken);
    CHECK(taken.flag && taken.count == 3 && taken.points[2][0] == 20 && taken.points[2][1] == -2);
    CHECK(ground.ground.flags == (ground_driver_path_set | ground_driver_sent_blocked));
    CHECK(!ground_driver_has_delta(ground));

    MovementRecord air{};
    air.driver = MovementClass::air;
    air.flags = 2;
    air.air.goal_kind = air_goal_kind_seek;
    air.air.seek.flag = true;
    CHECK(!air_driver_has_delta(air));
    air_driver_note_substate(&air);
    CHECK(air_driver_has_delta(air));
    AirDelta delta{};
    air_driver_take_delta(&air, &delta);
    CHECK(delta.goal_tag == air_goal_tag_seek && delta.seek.flag && delta.substate == 2);
    CHECK(!air_driver_has_delta(air) && (air.air.flags & air_driver_sent_substate_mask) == 4);
    air_driver_note_substate(&air);
    CHECK(!air_driver_has_delta(air));
    air.air.goal_kind = 6;
    air.air.flags |= air_driver_resend;
    air_driver_take_delta(&air, &delta);
    CHECK(delta.goal_tag_omitted);
}

// ---- a World with a side table of movement records ----

struct TestWorld {
    std::unique_ptr<World> world = std::make_unique<World>();
    std::vector<Unit> units;
    std::vector<UnitDef> defs;
    std::vector<MovementRecord> movement;
    ReplicationSim sim{};
    std::size_t creates = 0;
    std::size_t links = 0;
    std::size_t kills = 0;

    // mobile: defs without a known layout that still have a movement record.
    TestWorld(
        uint16_t units_per_player,
        unsigned def_bits,
        const MovementClass* classes,
        const bool* mobile,
        std::size_t def_count
    ) {
        units.resize(std::size_t{units_per_player} * OA_PLAYER_COUNT + 1);
        movement.resize(units.size());
        defs.resize(def_count);
        for (std::size_t i = 0; i < def_count; ++i) {
            defs[i].flags = classes[i] == MovementClass::air ? OA_UNIT_DEF_FLAG_CAN_FLY : 0;
            defs[i].bm_code =
                classes[i] != MovementClass::none || (mobile != nullptr && mobile[i]) ? 1 : 0;
        }
        for (std::size_t i = 0; i < units.size(); ++i)
            units[i].id = static_cast<uint16_t>(i);
        world->units = units.data();
        world->unit_slot_count = static_cast<uint32_t>(units.size());
        world->unit_defs = defs.data();
        world->unit_def_count = static_cast<uint32_t>(defs.size());
        world->game.units_per_player = units_per_player;
        world->game.unit_def_id_bits = static_cast<int32_t>(def_bits);
        for (uint32_t p = 0; p < OA_PLAYER_COUNT; ++p) {
            auto& player = world->game.players[p];
            player.index = static_cast<uint8_t>(p);
            player.in_use = 1;
            player.player_id = 100 + p;
            player.status = OA_PLAYER_STATUS_MIRRORED;
            const auto first = p * units_per_player + 1;
            player.first_unit = oa_unit_ref_from_slot(first);
            player.last_unit = oa_unit_ref_from_slot(first + units_per_player - 1);
            player.base_unit_id = static_cast<uint16_t>(first);
            for (uint32_t u = first; u < first + units_per_player; ++u)
                units[u].owner_index = static_cast<uint8_t>(p);
        }
        sim.context = this;
        sim.movement = [](void* ctx, World* w, Unit* unit) -> MovementRecord* {
            auto* self = static_cast<TestWorld*>(ctx);
            return &self->movement[world_unit_slot(w, unit)];
        };
        sim.create_unit = [](void* ctx, World* w, uint8_t owner, const UnitCreatedRecord& r) {
            auto* self = static_cast<TestWorld*>(ctx);
            Unit* unit = world_unit_at(w, r.unit_index);
            CHECK(unit != nullptr && unit->owner_index == owner);
            if (unit == nullptr || r.unit_def_index >= self->defs.size())
                return;
            ++self->creates;
            self->spawn(unit, r.unit_def_index, true);
            unit->position = {r.position[0], r.position[1], r.position[2]};
        };
        sim.link_unit = [](void* ctx, World* w, const UnitLinkRecord& r) {
            auto* self = static_cast<TestWorld*>(ctx);
            ++self->links;
            Unit* unit = world_unit_at(w, r.unit_index);
            if (unit == nullptr)
                return;
            Unit* carrier =
                r.linked_unit_index != 0 ? world_unit_at(w, r.linked_unit_index) : nullptr;
            unit->attach_parent = world_unit_ref(w, carrier);
            unit->attach_piece = static_cast<uint8_t>(r.attach_piece);
            unit->flags = (unit->flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) |
                          (r.occupancy & OA_UNIT_FLAG_OCCUPANCY_MASK);
        };
        sim.place_unit = [](void*, World*, Unit* unit, const FixedVec3& to, uint8_t occupancy) {
            unit->position = to;
            unit->flags = (unit->flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) |
                          (occupancy & OA_UNIT_FLAG_OCCUPANCY_MASK);
        };
        sim.apply_kill = [](void* ctx, World* w, const UnitKilledRecord& r) {
            ++static_cast<TestWorld*>(ctx)->kills;
            if (Unit* unit = world_unit_at(w, r.unit_index))
                unit->flags &= ~OA_UNIT_FLAG_LIVE;
        };
    }

    Unit* spawn(Unit* unit, uint16_t def_index, bool remote) {
        const auto slot = world_unit_slot(world.get(), unit);
        unit->type_index = def_index;
        unit->def = oa_ref_from_index(def_index);
        unit->flags = OA_UNIT_FLAG_LIVE;
        unit->movement =
            defs[def_index].bm_code == unit_def_bm_code_mobile ? oa_ref_from_index(slot) : 0;
        movement[slot] = MovementRecord{};
        movement[slot].driver = movement_class_for_def(defs[def_index]);
        movement[slot].remote_driver = remote;
        return unit;
    }
};

// ---- two Worlds: one packs, the other applies ----

constexpr uint16_t small_units_per_player = 12;
constexpr unsigned small_def_bits = 4;
// def 3 is a building (no movement record); 6 and 7 are unused.
constexpr MovementClass small_classes[8] = {
    MovementClass::none,
    MovementClass::ground,
    MovementClass::air,
    MovementClass::none,
    MovementClass::ground,
    MovementClass::air,
    MovementClass::none,
    MovementClass::none,
};

Bytes pack(TestWorld& from, uint8_t player) {
    uint16_t length = 0;
    const auto bytes = encode([&](BitWriter* w) {
        CHECK(
            replication_pack_player(from.world.get(), &from.sim, player, w, &length) ==
            WireError::ok
        );
    });
    CHECK(length == bytes.size());
    return bytes;
}

// What the receiver's ground-delta and speed hooks were last handed, by unit id.
struct HookedState {
    bool delta{};
    WaypointDelta ground{};
    bool speed{};
    uint32_t speed_word{};
};

HookedState hooked[64];

void hook_ground_delta(void*, World*, Unit* unit, const WaypointDelta& delta) {
    if (unit->id < std::size(hooked)) {
        hooked[unit->id].delta = true;
        hooked[unit->id].ground = delta;
    }
}

void hook_speed(void*, World*, Unit* unit, uint32_t speed) {
    if (unit->id < std::size(hooked)) {
        hooked[unit->id].speed = true;
        hooked[unit->id].speed_word = speed;
    }
}

void pack_apply_converges() {
    TestWorld local(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );
    TestWorld remote(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );
    remote.sim.apply_ground_delta = hook_ground_delta;
    remote.sim.set_movement_speed = hook_speed;
    for (auto& state : hooked)
        state = HookedState{};
    local.world->game.players[0].status = OA_PLAYER_STATUS_LOCAL;
    Unit* units = &local.units[1];
    const uint16_t defs[] = {1, 2, 3, 4, 5, 1, 2, 4, 5, 1};
    for (std::size_t i = 0; i < std::size(defs); ++i) {
        Unit* unit = local.spawn(&units[i], defs[i], false);
        unit->health = static_cast<int16_t>(100 + 37 * i);
        unit->build_remaining = i == 2 ? 0.5f : 0.0f;
        unit->state_flags = static_cast<uint8_t>(i * 5);
        unit->flags |= i & OA_UNIT_FLAG_OCCUPANCY_MASK;
        unit->position = {
            static_cast<int32_t>((200 + 40 * i) << 16),
            12 << 16,
            static_cast<int32_t>((300 + 25 * i) << 16)
        };
        unit->heading = static_cast<uint16_t>(0x1000 * i);
        unit->pitch = static_cast<int16_t>(-3 * static_cast<int>(i));
        unit->bank = static_cast<int16_t>(i);
        MovementRecord& m = local.movement[unit->id];
        m.speed = 0xabc0000u + static_cast<uint32_t>(i);
        if (m.driver == MovementClass::ground) {
            m.ground.flags = ground_driver_path_set | ground_driver_resend;
            m.ground.path_count = static_cast<int32_t>(i % 6);
            for (std::size_t p = 0; p < ground_driver_path_capacity; ++p) {
                m.ground.path[p][0] = static_cast<int16_t>(i * 100 + p);
                m.ground.path[p][1] = static_cast<int16_t>(-static_cast<int>(p));
            }
            m.flags = i % 3 == 0 ? movement_blocked : 0;
        } else if (m.driver == MovementClass::air) {
            m.air.flags = air_driver_resend;
            m.flags = static_cast<uint8_t>(i % 3);
            m.air.goal_kind = i % 2 ? air_goal_kind_target : air_goal_kind_seek;
            m.air.target.flags = air_target_has_unit | air_target_has_position;
            m.air.target.target_unit = 3;
            m.air.target.position[0] = static_cast<int32_t>(i << 16);
            m.air.seek.flag = true;
            m.air.seek.step[2] = static_cast<int32_t>(i);
            m.air.seek.heading = static_cast<uint16_t>(i);
        }
    }
    // The last unit rides in the first one.
    units[9].attach_parent = world_unit_ref(local.world.get(), &units[0]);
    units[9].attach_piece = 2;

    const auto step = [&](uint32_t tick) {
        local.world->game.tick = tick;
        const auto bytes = pack(local, 0);
        bool handled = false;
        CHECK(
            replication_apply_record(
                remote.world.get(), &remote.sim, 0, bytes.data(), bytes.size(), &handled
            ) == WireError::ok
        );
        CHECK(handled);
    };
    uint32_t tick = 1;
    for (; tick <= small_units_per_player; ++tick)
        step(tick);
    // Change things: move, damage, re-path, re-target, change substate, kill.
    units[0].position.x += 5 << 16;
    units[1].health = 1;
    MovementRecord& ground = local.movement[units[0].id];
    ground.ground.path_count = 2;
    ground.ground.path[0][0] = 77;
    ground.ground.flags |= ground_driver_resend;
    MovementRecord& air = local.movement[units[1].id];
    air.flags = 2;
    air_driver_note_substate(&air);
    CHECK(air_driver_has_delta(air));
    MovementRecord& air_b = local.movement[units[4].id];
    air_b.air.goal_kind = air_goal_kind_none; // the goal setter re-arms the delta
    air_b.air.flags |= air_driver_resend;
    units[8] = Unit{};
    units[8].id = static_cast<uint16_t>(9);
    units[8].owner_index = 0;
    for (const uint32_t end = tick + 2 * small_units_per_player; tick < end; ++tick)
        step(tick);

    std::size_t compared = 0;
    for (std::size_t i = 0; i < std::size(defs); ++i) {
        const Unit& a = units[i];
        const Unit& b = remote.units[a.id];
        if (a.type_index == 0) {
            CHECK((b.flags & OA_UNIT_FLAG_DEATH_PENDING) != 0);
            continue;
        }
        ++compared;
        CHECK(
            b.type_index == a.type_index && b.health == a.health && b.state_flags == a.state_flags
        );
        CHECK(
            b.build_remaining == unit_state_build_fraction(unit_state_build_byte(a.build_remaining))
        );
        CHECK((b.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == (a.flags & OA_UNIT_FLAG_OCCUPANCY_MASK));
        CHECK((b.attach_parent != 0) == (a.attach_parent != 0));
        if (a.attach_parent != 0) {
            CHECK(
                world_unit(remote.world.get(), b.attach_parent)->id ==
                world_unit(local.world.get(), a.attach_parent)->id
            );
            CHECK(b.attach_piece == a.attach_piece);
            continue;
        }
        CHECK(
            b.position.x == a.position.x && b.position.y == a.position.y &&
            b.position.z == a.position.z
        );
        CHECK(b.heading == a.heading && b.pitch == a.pitch && b.bank == a.bank);
        CHECK((b.movement != 0) == (a.movement != 0));
        if (a.movement == 0)
            continue;
        const MovementRecord& ma = local.movement[a.id];
        const MovementRecord& mb = remote.movement[b.id];
        CHECK(mb.driver == ma.driver && mb.remote_driver && mb.speed == ma.speed);
        CHECK(
            a.id < std::size(hooked) && hooked[a.id].speed && hooked[a.id].speed_word == ma.speed
        );
        if (ma.driver == MovementClass::ground) {
            const auto sent = std::min<int32_t>(ma.ground.path_count, ground_delta_max_points);
            CHECK((mb.flags & movement_blocked) == (ma.flags & movement_blocked));
            CHECK(mb.ground.path_count == sent);
            for (int32_t p = 0; p < sent; ++p)
                CHECK(
                    mb.ground.path[p][0] == ma.ground.path[p][0] &&
                    mb.ground.path[p][1] == ma.ground.path[p][1]
                );
            const auto& hook = hooked[a.id].ground;
            CHECK(
                hooked[a.id].delta && hook.flag == ((ma.flags & movement_blocked) != 0) &&
                hook.count == sent
            );
            for (int32_t p = 0; p < sent; ++p)
                CHECK(
                    hook.points[p][0] == ma.ground.path[p][0] &&
                    hook.points[p][1] == ma.ground.path[p][1]
                );
        } else {
            CHECK((mb.flags & movement_substate_mask) == (ma.flags & movement_substate_mask));
            CHECK(mb.air.goal_kind == ma.air.goal_kind);
            if (ma.air.goal_kind == air_goal_kind_target)
                CHECK(same_target(mb.air.target, ma.air.target));
            if (ma.air.goal_kind == air_goal_kind_seek)
                CHECK(
                    mb.air.seek.flag == ma.air.seek.flag &&
                    mb.air.seek.step[2] == ma.air.seek.step[2] &&
                    mb.air.seek.heading == ma.air.seek.heading
                );
        }
    }
    CHECK(compared == std::size(defs) - 1);
    // Units outside the list are untouched on the remote side.
    CHECK(remote.units[small_units_per_player + 1].type_index == 0);
    CHECK(remote.world->game.players[0].last_sim_tick == static_cast<int32_t>(tick - 1));
}

// A unit a 0x2c entry names before its 0x09 has arrived, such as a
// commander announced while this machine was still loading, is created from
// the stream in the slot as it stands, at the slot's own position, as 3.1c
// creates it; it stands there until its full record places it.
void stream_created_unit_waits_for_its_full_record() {
    TestWorld local(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );
    TestWorld remote(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );
    local.world->game.players[0].status = OA_PLAYER_STATUS_LOCAL;
    Unit* commander = local.spawn(&local.units[1], 1, false);
    commander->health = 3000;
    commander->position = {200 << 16, 12 << 16, 300 << 16};
    MovementRecord& route = local.movement[commander->id];
    route.ground.flags = ground_driver_path_set | ground_driver_resend;
    route.ground.path_count = 1;
    route.ground.path[0][0] = 210;
    route.ground.path[0][1] = 300;

    const auto step = [&](uint32_t tick) {
        local.world->game.tick = tick;
        const auto bytes = pack(local, 0);
        CHECK(
            replication_apply_unit_state(
                remote.world.get(), &remote.sim, 0, bytes.data(), bytes.size()
            ) == WireError::ok
        );
    };
    // Tick 1 carries the route and the full record of the empty slot after it.
    step(1);
    const Unit& copy = remote.units[commander->id];
    CHECK(remote.creates == 1 && copy.type_index == 1);
    CHECK(copy.position.x == 0 && copy.position.y == 0 && copy.position.z == 0);
    CHECK(copy.health == 0);
    // The commander's full record rides on the tick that is a multiple of the
    // units per player, and puts the copy where the commander stands.
    for (uint32_t tick = 2; tick <= small_units_per_player; ++tick)
        step(tick);
    CHECK(remote.creates == 1);
    CHECK(
        copy.position.x == commander->position.x && copy.position.y == commander->position.y &&
        copy.position.z == commander->position.z
    );
    CHECK(copy.health == commander->health);
}

// The receiver's hook calls for one 0x2c, in order.
enum class Call : uint8_t { ground_delta, air_goal, substate, tick, sight, place };
std::vector<std::pair<Call, uint16_t>> calls;

// Each air delta sets the goal, then the substate; every live unit of the
// sender then gets its movement tick and sight update; the full record
// comes last.
void receiver_calls_follow_the_record() {
    TestWorld local(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );
    TestWorld remote(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );
    remote.sim.apply_ground_delta = [](void*, World*, Unit* unit, const WaypointDelta&) {
        calls.emplace_back(Call::ground_delta, unit->id);
    };
    remote.sim.set_air_goal = [](void*, World*, Unit* unit, const AirDelta&) {
        calls.emplace_back(Call::air_goal, unit->id);
    };
    remote.sim.set_movement_substate = [](void*, World*, Unit* unit, uint8_t) {
        calls.emplace_back(Call::substate, unit->id);
    };
    remote.sim.movement_tick = [](void*, World*, Unit* unit) {
        calls.emplace_back(Call::tick, unit->id);
    };
    remote.sim.sight_update = [](void*, World*, Unit* unit) {
        calls.emplace_back(Call::sight, unit->id);
    };
    remote.sim.place_unit = [](void*, World*, Unit* unit, const FixedVec3& to, uint8_t) {
        unit->position = to;
        calls.emplace_back(Call::place, unit->id);
    };
    local.world->game.players[0].status = OA_PLAYER_STATUS_LOCAL;
    Unit* units = &local.units[1];
    const uint16_t defs[] = {2, 1, 3, 5};
    for (std::size_t i = 0; i < std::size(defs); ++i) {
        Unit* unit = local.spawn(&units[i], defs[i], false);
        MovementRecord& m = local.movement[unit->id];
        m.ground.flags = ground_driver_resend;
        m.air.flags = air_driver_resend;
        m.air.goal_kind = air_goal_kind_seek;
    }
    local.world->game.tick = small_units_per_player;
    const auto bytes = pack(local, 0);
    calls.clear();
    bool handled = false;
    CHECK(
        replication_apply_record(
            remote.world.get(), &remote.sim, 0, bytes.data(), bytes.size(), &handled
        ) == WireError::ok
    );
    // The building (def 3) has no movement record: no delta, tick or sight.
    const uint16_t a = units[0].id, g = units[1].id, b = units[3].id;
    const std::vector<std::pair<Call, uint16_t>> expected = {
        {Call::air_goal, a},
        {Call::substate, a},
        {Call::ground_delta, g},
        {Call::air_goal, b},
        {Call::substate, b},
        {Call::tick, a},
        {Call::sight, a},
        {Call::tick, g},
        {Call::sight, g},
        {Call::tick, b},
        {Call::sight, b},
        {Call::place, a},
    };
    CHECK(handled && calls == expected);
}

// The receiver's gating for unit events follows the packet pump.
void event_records_follow_pump_rules() {
    TestWorld world(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );

    struct Seen {
        int cob = 0, transfers = 0, builders = 0, burns = 0, queued = 0, table = 0;
    } seen;

    world.sim.context = &world;
    static Seen* active = nullptr;
    active = &seen;
    world.sim.cob_start = [](void*, World*, Unit*, const CobStartRecord&) { ++active->cob; };
    world.sim.transfer_unit = [](void*, World*, Unit*, Player*, const UnitTransferRecord&) {
        ++active->transfers;
    };
    world.sim.link_builder = [](void*, World*, Unit* source, Unit* subject) {
        active->builders += source == nullptr && subject != nullptr;
    };
    world.sim.tree_burn = [](void*, World*, uint16_t, uint16_t) { ++active->burns; };
    world.sim.queue_feature_event = [](void*, World*, uint16_t, uint16_t, bool flagged) {
        active->queued += flagged ? 10 : 1;
    };
    world.sim.feature_event = [](void*, World*, uint16_t, uint16_t, uint16_t) { ++active->table; };
    Unit* live = world.spawn(&world.units[2], 1, true);
    world.units[3].type_index = 1;
    world.world->game.players[4].status = OA_PLAYER_STATUS_LOCAL;
    const auto apply = [&](const auto& record) {
        uint8_t bytes[256];
        std::size_t written = 0;
        CHECK(encode_record(record, bytes, sizeof bytes, &written) == WireError::ok);
        bool handled = false;
        CHECK(
            replication_apply_record(world.world.get(), &world.sim, 1, bytes, written, &handled) ==
            WireError::ok
        );
        return handled;
    };
    CobStartRecord cob{};
    cob.unit_index = live->id;
    CHECK(apply(cob));
    cob.unit_index = 3; // not live
    CHECK(apply(cob));
    cob.unit_index = 0;
    CHECK(apply(cob));
    CHECK(seen.cob == 1);
    UnitStateFlagsRecord flags{};
    flags.unit_index = live->id;
    flags.state_mask = 0x21;
    live->state_flags = 0x86;
    CHECK(apply(flags));
    CHECK(live->state_flags == 0x21);
    UnitTransferRecord transfer{};
    transfer.unit_index = live->id;
    transfer.new_owner_id = world.world->game.players[5].player_id; // remote slot
    CHECK(apply(transfer));
    transfer.new_owner_id = world.world->game.players[4].player_id;
    CHECK(apply(transfer));
    CHECK(seen.transfers == 1);
    BuilderLinkRecord builder{};
    builder.subject_unit_index = live->id;
    CHECK(apply(builder));
    CHECK(seen.builders == 1);
    FeatureEventRecord feature{};
    for (uint8_t action :
         {feature_action_tree_burn,
          feature_action_queue_event,
          feature_action_queue_event_flagged,
          uint8_t{4}}) {
        feature.action = action;
        CHECK(apply(feature));
    }
    CHECK(seen.burns == 1 && seen.queued == 11 && seen.table == 1);
    ChatRecord chat{};
    CHECK(!apply(chat));
    active = nullptr;
}

// Wide weapon records, and the narrow ones widened on the way in, reach the same hooks.
void wide_weapon_records_reach_the_hooks() {
    TestWorld world(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );

    struct Seen {
        WideWeaponFireRecord fire{};
        int fires = 0;
        WideProjectileInterceptedRecord hit{};
        int hits = 0;
        uint16_t feature_weapon = 0;
        int features = 0;
        int burns = 0;
        int queued = 0;
        int flagged = 0;
    } seen;

    static Seen* active = nullptr;
    active = &seen;
    world.sim.weapon_fire = [](void*, World*, Player*, const WideWeaponFireRecord& record) {
        active->fire = record;
        ++active->fires;
    };
    world.sim.detonate_projectile =
        [](void*, World*, const WideProjectileInterceptedRecord& record) {
            active->hit = record;
            ++active->hits;
        };
    world.sim.feature_event = [](void*, World*, uint16_t weapon, uint16_t, uint16_t) {
        active->feature_weapon = weapon;
        ++active->features;
    };
    world.sim.tree_burn = [](void*, World*, uint16_t, uint16_t) { ++active->burns; };
    world.sim.queue_feature_event = [](void*, World*, uint16_t, uint16_t, bool flagged) {
        ++active->queued;
        active->flagged += flagged ? 1 : 0;
    };
    const auto apply = [&](const auto& record) {
        uint8_t bytes[256];
        std::size_t written = 0;
        CHECK(encode_record(record, bytes, sizeof bytes, &written) == WireError::ok);
        bool handled = false;
        CHECK(
            replication_apply_record(world.world.get(), &world.sim, 1, bytes, written, &handled) ==
            WireError::ok
        );
        return handled;
    };

    WideWeaponFireRecord wide_fire{};
    wide_fire.start[0] = 11;
    wide_fire.target[1] = 22;
    wide_fire.weapon_id = 300;
    wide_fire.flags = 1;
    wide_fire.aim_heading = 7;
    wide_fire.aim_pitch = -2;
    wide_fire.target_unit_index = 3;
    wide_fire.source_unit_index = 4;
    wide_fire.slot = 1;
    CHECK(apply(wide_fire));
    CHECK(seen.fires == 1 && seen.fire == wide_fire);

    WeaponFireRecord narrow_fire{};
    narrow_fire.start[0] = 11;
    narrow_fire.target[1] = 22;
    narrow_fire.weapon_id = 200;
    narrow_fire.flags = 1;
    narrow_fire.aim_heading = 7;
    narrow_fire.aim_pitch = -2;
    narrow_fire.target_unit_index = 3;
    narrow_fire.source_unit_index = 4;
    narrow_fire.slot = 1;
    CHECK(apply(narrow_fire));
    CHECK(seen.fires == 2 && seen.fire.weapon_id == 200 && seen.fire.start[0] == 11);
    CHECK(seen.fire.flags == 1 && seen.fire.slot == 1 && seen.fire.aim_heading == 7);

    WideProjectileInterceptedRecord wide_hit{};
    wide_hit.target[0] = 5;
    wide_hit.target[1] = 6;
    wide_hit.target[2] = 7;
    wide_hit.weapon_id = 300;
    CHECK(apply(wide_hit));
    CHECK(seen.hits == 1 && seen.hit == wide_hit);
    ProjectileInterceptedRecord narrow_hit{};
    narrow_hit.target[0] = 8;
    narrow_hit.target[1] = 9;
    narrow_hit.target[2] = 10;
    narrow_hit.weapon_id = 9;
    CHECK(apply(narrow_hit));
    CHECK(seen.hits == 2 && seen.hit.weapon_id == 9 && seen.hit.target[0] == 8);

    WideFeatureEventRecord feature{};
    feature.x = 1;
    feature.y = 2;
    feature.action = static_cast<uint8_t>(WideFeatureAction::weapon_hit);
    feature.weapon_id = 300;
    CHECK(apply(feature) && seen.features == 1 && seen.feature_weapon == 300);
    feature.action = static_cast<uint8_t>(WideFeatureAction::queue_event);
    CHECK(apply(feature));
    feature.action = static_cast<uint8_t>(WideFeatureAction::queue_event_flagged);
    CHECK(apply(feature));
    feature.action = static_cast<uint8_t>(WideFeatureAction::tree_burn);
    CHECK(apply(feature));
    CHECK(seen.queued == 2 && seen.flagged == 1 && seen.burns == 1);
    feature.action = 4;
    CHECK(!apply(feature));
    active = nullptr;
}

struct UnitSnapshot {
    uint16_t type_index{};
    int16_t health{};
    float build_remaining{};
    uint8_t state_flags{};
    FixedVec3 position{};
    uint16_t heading{};
    uint32_t flags{};
};

UnitSnapshot snapshot(const Unit& unit) {
    return {
        unit.type_index,
        unit.health,
        unit.build_remaining,
        unit.state_flags,
        unit.position,
        unit.heading,
        unit.flags
    };
}

bool same(const UnitSnapshot& a, const UnitSnapshot& b) {
    return a.type_index == b.type_index && a.health == b.health &&
           a.build_remaining == b.build_remaining && a.state_flags == b.state_flags &&
           a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.heading == b.heading && a.flags == b.flags;
}

// A unit state cut short anywhere in its full record leaves that unit as it
// was; a listed unit outside the sender's range, or any damaged copy, is
// refused without touching another player's units.
void malformed_unit_states_change_nothing() {
    TestWorld local(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );
    TestWorld remote(
        small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
    );
    local.world->game.players[0].status = OA_PLAYER_STATUS_LOCAL;
    Unit* sent = local.spawn(&local.units[1], 1, false);
    sent->health = 250;
    sent->state_flags = 0x15;
    sent->position = {100 << 16, 3 << 16, 200 << 16};
    sent->heading = 0x4000;
    Unit* held = remote.spawn(&remote.units[1], 1, true);
    held->health = 10;
    held->position = {7 << 16, 0, 9 << 16};
    local.world->game.tick = 0; // the full record is the first unit's
    const auto bytes = pack(local, 0);
    const auto before = snapshot(*held);
    for (std::size_t n = unit_state_header_bytes; n < bytes.size(); ++n) {
        Bytes cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        cut[1] = static_cast<uint8_t>(n);
        cut[2] = static_cast<uint8_t>(n >> 8);
        const auto result = replication_apply_unit_state(
            remote.world.get(), &remote.sim, 0, cut.data(), cut.size()
        );
        CHECK(result == WireError::truncated);
        CHECK(same(snapshot(*held), before));
    }
    CHECK(
        replication_apply_unit_state(
            remote.world.get(), &remote.sim, 0, bytes.data(), bytes.size()
        ) == WireError::ok
    );
    CHECK(held->health == 250 && held->state_flags == 0x15 && held->heading == 0x4000);

    // A listed unit past the sender's range.
    for (const uint16_t index : {small_units_per_player, uint16_t{0x7fff}, uint16_t{0x8000}}) {
        const auto state = encode([&](BitWriter* w) {
            unit_state_begin(w, 1);
            unit_state_write_entry_header(w, index, 1, small_def_bits);
            write_waypoint_delta(w, WaypointDelta{});
            uint16_t length = 0;
            CHECK(unit_state_finish(w, FullUnitRecord{}, small_def_bits, &length) == WireError::ok);
        });
        CHECK(
            replication_apply_unit_state(
                remote.world.get(), &remote.sim, 0, state.data(), state.size()
            ) == WireError::bad_argument
        );
    }
    CHECK(
        replication_apply_unit_state(
            remote.world.get(), &remote.sim, OA_PLAYER_COUNT, bytes.data(), bytes.size()
        ) == WireError::bad_argument
    );

    // Damaged copies of the record: whatever they decode to, only the
    // sender's units change.
    const Unit other_before = remote.units[small_units_per_player + 1];
    uint32_t state = 0x2c2c2c2c;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int i = 0; i < 2000; ++i) {
        Bytes damaged = bytes;
        const auto changes = 1 + next() % 4;
        for (uint32_t c = 0; c < changes; ++c)
            damaged[next() % damaged.size()] ^= static_cast<uint8_t>(1u << (next() % 8));
        if (next() % 4 == 0)
            damaged.resize(1 + next() % damaged.size());
        bool handled = false;
        (void)replication_apply_record(
            remote.world.get(), &remote.sim, 0, damaged.data(), damaged.size(), &handled
        );
    }
    const Unit& other = remote.units[small_units_per_player + 1];
    CHECK(other.type_index == other_before.type_index && other.health == other_before.health);
}

// A unit state whose unit changes def: cut short anywhere in the unit's
// delta or in its full record, the speed word included, it recreates
// nothing and leaves the slot's unit; whole, it recreates the unit, then
// applies.
void short_body_recreates_nothing() {
    for (const bool routed : {true, false}) {
        TestWorld local(
            small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
        );
        TestWorld remote(
            small_units_per_player, small_def_bits, small_classes, nullptr, std::size(small_classes)
        );
        local.world->game.players[0].status = OA_PLAYER_STATUS_LOCAL;
        Unit* sent = local.spawn(&local.units[1], 1, false);
        sent->health = 250;
        if (routed) {
            // A route lists the unit among the deltas; without one only its
            // full record carries it.
            MovementRecord& route = local.movement[sent->id];
            route.ground.flags = ground_driver_path_set | ground_driver_resend;
            route.ground.path_count = 1;
            route.ground.path[0][0] = 210;
            route.ground.path[0][1] = 300;
        }
        Unit* held = remote.spawn(&remote.units[1], 4, true);
        held->health = 10;
        local.world->game.tick = 0; // the full record is the first unit's
        const auto bytes = pack(local, 0);
        // Where the unit's delta ends: a cut past it may stop later in the
        // record, after the delta has applied and recreated the unit.
        std::size_t delta_end_bits = bytes.size() * 8;
        if (routed) {
            UnitDelta deltas[4]{};
            DeltaCodecState state{};
            state.classes = {small_classes, std::size(small_classes)};
            state.deltas = deltas;
            state.capacity = std::size(deltas);
            const auto codec = delta_codec(&state);
            UnitStateDecodeOptions options{};
            options.def_index_bits = small_def_bits;
            options.full_record_object_word_present = true;
            options.delta_codec = &codec;
            UnitStateBody body{};
            CHECK(decode_unit_state(bytes.data(), bytes.size(), options, &body) == WireError::ok);
            CHECK(body.entry_count == 1);
            delta_end_bits = body.entries[0].delta_bit_offset + body.entries[0].delta_bit_count;
        }
        for (std::size_t n = unit_state_header_bytes; n < bytes.size(); ++n) {
            Bytes cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
            cut[1] = static_cast<uint8_t>(n);
            cut[2] = static_cast<uint8_t>(n >> 8);
            CHECK(
                replication_apply_unit_state(
                    remote.world.get(), &remote.sim, 0, cut.data(), cut.size()
                ) == WireError::truncated
            );
            if (n * 8 < delta_end_bits)
                CHECK(remote.creates == 0 && held->type_index == 4 && held->health == 10);
        }
        if (routed) {
            // The delta whole and the record cut later: the unit was recreated.
            CHECK(remote.creates <= 1);
            remote.creates = 0;
        }
        CHECK(
            replication_apply_unit_state(
                remote.world.get(), &remote.sim, 0, bytes.data(), bytes.size()
            ) == WireError::ok
        );
        CHECK(held->type_index == 1 && held->health == 250 && (routed || remote.creates == 1));
    }
}

} // namespace

int main() {
    struct Test {
        const char* name;
        void (*run)();
    };

    const Test tests[] = {
        {"air_delta_layouts_round_trip", air_delta_layouts_round_trip},
        {"driver_delta_bookkeeping", driver_delta_bookkeeping},
        {"pack_apply_converges", pack_apply_converges},
        {"stream_created_unit_waits_for_its_full_record",
         stream_created_unit_waits_for_its_full_record},
        {"receiver_calls_follow_the_record", receiver_calls_follow_the_record},
        {"event_records_follow_pump_rules", event_records_follow_pump_rules},
        {"wide_weapon_records_reach_the_hooks", wide_weapon_records_reach_the_hooks},
        {"malformed_unit_states_change_nothing", malformed_unit_states_change_nothing},
        {"short_body_recreates_nothing", short_body_recreates_nothing},
    };
    for (const auto& test : tests) {
        current_test = test.name;
        test.run();
    }
    std::printf("net-replication: %zu tests; %d failures\n", std::size(tests), failures);
    return failures == 0 ? 0 : 1;
}
