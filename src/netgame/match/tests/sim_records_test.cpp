// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Records built from the simulation's shared events (the 0x11 state flags,
// the 0x0c unit-killed record byte for byte, the 0x14 unit transfer and the
// 0x0f feature records),
// and the route deltas of ground units. With arguments it answers activation queries instead: each
// stdin line "before mask enabled eligible mutate" prints the flags, the call
// order and the 0x11 record.
#include "oa/netgame/match/route_delta.hpp"
#include "oa/netgame/match/sim_records.hpp"
#include "oa/netgame/player_slots.hpp"
#include "oa/netgame/records.hpp"
#include "oa/sim/unit_activation.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::netgame::match;

namespace {
int failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

// The activation host the match binds: the owner simulating the unit here
// decides whether the change goes out, then the 0x11 record carries it.
struct Activation : sim::unit_activation::Host {
    std::vector<std::string> events;
    std::array<uint8_t, unit_flags_record_bytes> packet{};
    uint8_t* flags{};
    bool mutate = false, eligible = true;

    void script(std::string_view name) override {
        events.emplace_back(name);
        if (mutate)
            *flags = 0x80;
    }

    void sound(sim::unit_activation::Sound sound) override {
        events.push_back("sound" + std::to_string(unsigned(sound)));
    }

    void notify_attachments(uint32_t) override { events.push_back("attachments"); }

    void refresh_selected_unit() override { events.push_back("refresh"); }

    bool owner_simulates_here() override {
        events.push_back("eligible");
        return eligible;
    }

    void flags_changed(uint16_t unit, uint8_t changed) override {
        packet = encode_unit_flags_record(unit, changed);
        events.push_back("send");
    }
};

void unit_flags_record() {
    CHECK((encode_unit_flags_record(0x1234, 13) == std::array<uint8_t, 4>{0x11, 0x34, 0x12, 13}));
    uint8_t flags = 0;
    Activation host;
    host.flags = &flags;
    sim::unit_activation::change(flags, 0x1234, 13, true, host);
    CHECK((host.packet == std::array<uint8_t, 4>{17, 0x34, 0x12, 13}));
    host.mutate = true;
    sim::unit_activation::change(flags, 0x1234, 13, false, host);
    CHECK(host.packet[3] == 0x80);
}

using KilledBytes = std::array<uint8_t, unit_killed_record_bytes>;

// The joiner's solar (unit 1, player 1, id 0x1003) was last hit by the host's
// commander (unit 251, player 0, id 0x2001), as in a D-gun kill.
struct KillScene {
    World world{};
    std::vector<Unit> units = std::vector<Unit>(4);

    KillScene() {
        world.units = units.data();
        world.unit_slot_count = static_cast<uint32_t>(units.size());
        auto& host = world.game.players[0];
        host.in_use = 1;
        host.index = 0;
        host.status = OA_PLAYER_STATUS_MIRRORED;
        host.player_id = 0x2001;
        auto& joiner = world.game.players[1];
        joiner.in_use = 1;
        joiner.index = 1;
        joiner.status = OA_PLAYER_STATUS_LOCAL;
        joiner.player_id = 0x1003;
        auto& solar = units[1];
        solar.id = 1;
        solar.owner = oa_ref_from_index(1);
        solar.owner_index = 1;
        solar.last_attacker_id = 251;
        solar.last_attacker_owner = 0;
    }
};

// The 0x0c record is {0x0c, unit, the last attacker's owner as a player id,
// the last attacker, the Killed percentage, kind << 4 | wreck level},
// little-endian, and decodes back field by field.
void unit_killed_record() {
    KillScene scene;
    // A D-gunned solar: a weapon death at percentage 100, wreck level 3.
    const auto dgun = encode_unit_killed_record(scene.world, 1, 1, 100, 3);
    CHECK((dgun == KilledBytes{0x0c, 0x01, 0x00, 0x01, 0x20, 0x00, 0x00, 0xfb, 0x00, 0x64, 0x13}));
    netgame::UnitKilledRecord decoded{};
    CHECK(netgame::decode_record(dgun.data(), dgun.size(), &decoded) == netgame::WireError::ok);
    CHECK(
        decoded.unit_index == 1 && decoded.attacker_owner_id == 0x2001 &&
        decoded.attacker_unit_index == 251
    );
    CHECK(decoded.killed_percent == 100);
    CHECK(decoded.kind_and_wreck_level >> netgame::unit_killed_kind_shift == 1);
    CHECK((decoded.kind_and_wreck_level & netgame::unit_killed_wreck_level_mask) == 3);

    // Other percentages, kinds and levels pack into the last two bytes.
    CHECK(encode_unit_killed_record(scene.world, 1, 3, 64, 0)[9] == 0x40);
    CHECK(encode_unit_killed_record(scene.world, 1, 3, 64, 0)[10] == 0x30);
    CHECK(encode_unit_killed_record(scene.world, 1, 1, -56, 1)[9] == 0xc8);
    CHECK(encode_unit_killed_record(scene.world, 1, 11, 0, 1)[10] == 0xb1);
    CHECK(encode_unit_killed_record(scene.world, 1, 1, 20, 0x13)[10] == 0x13);
    CHECK(encode_unit_killed_record(scene.world, 1, 5, 0, 0x2f)[10] == 0x5f);

    // A dismissed unit with no attacker: owner no_player_id, attacker 0.
    scene.units[1].last_attacker_id = 0;
    scene.units[1].last_attacker_owner = netgame::no_player_slot;
    const auto dismissed = encode_unit_killed_record(scene.world, 1, 7, 0, 1);
    CHECK(
        (dismissed == KilledBytes{0x0c, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x71})
    );
    CHECK(
        netgame::decode_record(dismissed.data(), dismissed.size(), &decoded) ==
        netgame::WireError::ok
    );
    CHECK(decoded.attacker_owner_id == netgame::no_player_id && decoded.attacker_unit_index == 0);
    CHECK(decoded.killed_percent == 0 && decoded.kind_and_wreck_level == 0x71);

    // The attacker's owner has left: its slot is free, so no player id goes
    // out, but the attacker's unit still does.
    scene.units[1].last_attacker_id = 251;
    scene.units[1].last_attacker_owner = 0;
    scene.world.game.players[0].status = OA_PLAYER_STATUS_FREE;
    const auto departed = encode_unit_killed_record(scene.world, 1, 1, 100, 3);
    CHECK(
        (departed == KilledBytes{0x0c, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xfb, 0x00, 0x64, 0x13})
    );

    // A unit past the pool has no attacker.
    const auto past = encode_unit_killed_record(scene.world, 9, 1, 100, 3);
    CHECK((past == KilledBytes{0x0c, 0x09, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x64, 0x13}));
}

struct Sent {
    std::vector<std::vector<uint8_t>> records;
    std::vector<bool> to_host;
    std::vector<uint8_t>
        senders; // the slot each record goes out from; no_player_slot for this machine's first
    bool networked = false;
};

FeatureRecordLink link_for(World& world, Sent& sent) {
    FeatureRecordLink link{};
    link.world = &world;
    link.context = &sent;
    link.send =
        [](void* context, const uint8_t* record, std::size_t size, bool to_host, uint8_t sender) {
            auto& s = *static_cast<Sent*>(context);
            s.records.emplace_back(record, record + size);
            s.to_host.push_back(to_host);
            s.senders.push_back(sender);
        };
    link.networked = [](void* context) { return static_cast<Sent*>(context)->networked; };
    return link;
}

// A 16x16 map with a 2x2 3DO wreck (featuredead 1) at (5, 5) and a flammable
// sprite tree at (8, 8).
struct Map {
    World world{};
    std::vector<MapPlot> plots = std::vector<MapPlot>(16 * 16);
    std::vector<FeatureDef> defs = std::vector<FeatureDef>(3);
    std::vector<sim::feature_runtime::PlacedFeature> records =
        std::vector<sim::feature_runtime::PlacedFeature>(sim::feature_runtime::slot_capacity);

    Map() {
        world.game.map_width = 16;
        world.game.map_height = 16;
        world.plots = plots.data();
        for (auto& plot : plots) {
            plot.feature = sim::feature_runtime::no_feature;
            plot.height = 20;
            plot.high_height = 20;
            plot.low_height = 20;
        }
        world.feature_defs = defs.data();
        world.feature_def_count = static_cast<uint32_t>(defs.size());
        world.game.feature_def_count = static_cast<int32_t>(defs.size());
        world.placed_features = reinterpret_cast<uint8_t*>(records.data());
        world.placed_feature_count = static_cast<uint32_t>(records.size());
        for (auto& def : defs) {
            def.footprint_x = 1;
            def.footprint_z = 1;
            def.dead_feature = sim::feature_runtime::no_feature;
            def.burnt_feature = sim::feature_runtime::no_feature;
            def.reclamate_feature = sim::feature_runtime::no_feature;
            def.flags = OA_FEATURE_FLAG_SPRITE;
        }
        auto& wreck = defs[0];
        wreck.flags = 0;
        wreck.footprint_x = 2;
        wreck.footprint_z = 2;
        wreck.damage = 100;
        wreck.dead_feature = 1;
        auto& tree = defs[2];
        tree.flags = OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_FLAMABLE;
        tree.spark_time = 4; // ticks
        tree.seq_name_burn = 1;
        CHECK(sim::feature_runtime::init_feature_pool(world));
        world.game.console_flags = OA_CONSOLE_FLAG_TREE_DEATH;
    }
};

// A client hands a hit to the authority (the weapon id, to the host); the
// authority applies it and broadcasts the die sequence (0xfd); an ignition
// goes out as tree burn (0xfe) and a reclaim or a resurrection, in a network
// game, as 0xff. The reclaim and the resurrection go out from the owner of
// the unit that did them, every other record from the first player on this
// machine.
void feature_records() {
    CHECK(
        (encode_feature_record(0xfe, 8, 0x0102) == std::array<uint8_t, 6>{0x0f, 0xfe, 8, 0, 2, 1})
    );
    Map m;
    Sent sent;
    sent.networked = true;
    auto link = link_for(m.world, sent);
    sim::feature_runtime::FeatureHost host{};
    host.context = &link;
    host.feature_hit_elsewhere = feature_hit_elsewhere;
    host.feature_changed = feature_changed;
    m.world.game.viewpoint_player = 0;
    m.world.game.players[0].info = 1;
    const auto at = [](int32_t x, int32_t z) { return static_cast<std::size_t>(z * 16 + x); };
    sim::feature_runtime::place_feature(
        m.world, host, at(5, 5), 0, nullptr, nullptr, sim::feature_runtime::no_player
    );
    WeaponDef weapon{};
    weapon.damage_default = 200;
    weapon.weapon_id = 9;
    sim::feature_runtime::damage_feature(m.world, host, at(5, 5), 5, 5, weapon);
    CHECK(sent.records.size() == 1 && sent.to_host[0] && sent.records[0][1] == 9);
    CHECK(sent.senders[0] == netgame::no_player_slot);
    CHECK(m.plots[at(5, 5)].feature == 0);
    m.world.player_info[0].role = lobby_role_feature_authority;
    sim::feature_runtime::damage_feature(m.world, host, at(5, 5), 5, 5, weapon);
    CHECK(m.plots[at(5, 5)].feature == 1);
    CHECK(
        sent.records.size() == 2 && !sent.to_host[1] && sent.records[1][1] == 0xfd &&
        sent.records[1][2] == 5
    );
    CHECK(sent.senders[1] == netgame::no_player_slot);

    sim::feature_runtime::place_feature(
        m.world, host, at(8, 8), 2, nullptr, nullptr, sim::feature_runtime::no_player
    );
    sim::feature_runtime::ignite_feature(m.world, host, 8, 8, false);
    CHECK(
        sent.records.size() == 3 && sent.records[2][0] == 0x0f && sent.records[2][1] == 0xfe &&
        sent.records[2][2] == 8 && sent.records[2][4] == 8
    );
    CHECK(sent.senders[2] == netgame::no_player_slot);
    Unit reclaimer{};
    reclaimer.owner_index = 2;
    feature_changed(&link, sim::feature_runtime::FeatureChange::reclaimed, 3, 4, &reclaimer);
    CHECK(
        sent.records.size() == 4 && sent.records[3][1] == 0xff && sent.records[3][2] == 3 &&
        sent.records[3][4] == 4
    );
    CHECK(!sent.to_host[3] && sent.senders[3] == 2);
    // A wreck raised back into a unit, at its origin plot.
    Unit resurrector{};
    resurrector.owner_index = 3;
    feature_changed(
        &link, sim::feature_runtime::FeatureChange::resurrected, 0x0106, 7, &resurrector
    );
    CHECK(
        sent.records.size() == 5 &&
        (sent.records[4] == std::vector<uint8_t>{0x0f, 0xff, 6, 1, 7, 0})
    );
    CHECK(!sent.to_host[4] && sent.senders[4] == 3);

    // Outside a network game only an ignition goes out, and hits apply here.
    sent.networked = false;
    feature_changed(&link, sim::feature_runtime::FeatureChange::destroyed, 1, 1, nullptr);
    feature_changed(&link, sim::feature_runtime::FeatureChange::reclaimed, 1, 1, &reclaimer);
    feature_changed(&link, sim::feature_runtime::FeatureChange::resurrected, 1, 1, &resurrector);
    CHECK(sent.records.size() == 5);
    CHECK(!feature_hit_elsewhere(&link, 9, 1, 1) && sent.records.size() == 5);
    feature_changed(&link, sim::feature_runtime::FeatureChange::ignited, 1, 1, nullptr);
    CHECK(sent.records.size() == 6 && sent.records[5][1] == 0xfe);
}

// With the wide form on, a hit and each change go out as the 8-byte record.
void wide_feature_records() {
    const auto burn =
        encode_wide_feature_record(netgame::WideFeatureAction::tree_burn, 300, 8, 0x0102);
    CHECK(burn.size() == 8 && burn[0] == 0xe2);
    CHECK(burn[1] == static_cast<uint8_t>(netgame::WideFeatureAction::tree_burn));
    CHECK(burn[2] == 0 && burn[3] == 0 && burn[4] == 8 && burn[6] == 2 && burn[7] == 1);

    Map scene;
    Sent sent;
    sent.networked = true;
    auto link = link_for(scene.world, sent);
    link.wide = true;
    CHECK(feature_hit_elsewhere(&link, 300, 5, 6));
    CHECK(sent.records.size() == 1 && sent.records[0].size() == 8 && sent.to_host[0]);
    CHECK(sent.records[0][0] == 0xe2);
    CHECK(sent.records[0][1] == static_cast<uint8_t>(netgame::WideFeatureAction::weapon_hit));
    CHECK(sent.records[0][2] == static_cast<uint8_t>(300) && sent.records[0][3] == 1);

    feature_changed(&link, sim::feature_runtime::FeatureChange::ignited, 1, 2, nullptr);
    feature_changed(&link, sim::feature_runtime::FeatureChange::destroyed, 3, 4, nullptr);
    Unit reclaimer{};
    reclaimer.owner_index = 2;
    feature_changed(&link, sim::feature_runtime::FeatureChange::reclaimed, 5, 6, &reclaimer);
    CHECK(sent.records.size() == 4);
    CHECK(sent.records[1].size() == 8);
    CHECK(sent.records[1][1] == static_cast<uint8_t>(netgame::WideFeatureAction::tree_burn));
    CHECK(sent.records[1][2] == 0 && sent.records[1][3] == 0);
    CHECK(sent.records[2][1] == static_cast<uint8_t>(netgame::WideFeatureAction::queue_event));
    CHECK(sent.records[2][2] == 0 && sent.records[2][3] == 0);
    CHECK(
        sent.records[3][1] == static_cast<uint8_t>(netgame::WideFeatureAction::queue_event_flagged)
    );
    CHECK(sent.senders[3] == 2);
}

// Replication hooks on the local navigator, and a received delta on the
// mirrored one.
void route_deltas() {
    sim::ground_orders::Navigation sent;
    sim::unit_movement::Movement sent_motion;
    sent.flags = sim::ground_orders::route_present_flag | sim::ground_orders::route_changed_flag;
    sent.count = 5;
    for (std::size_t i = 0; i < 5; ++i)
        sent.points[i] = {static_cast<int16_t>(i), static_cast<int16_t>(-1 - int(i))};
    sent_motion.flags = sim::unit_movement::collision_blocked;
    CHECK(route_needs_send(sent, sent_motion));
    auto delta = take_route_delta(sent, sent_motion);
    CHECK(delta.blocked && delta.count == 3 && delta.points[2][0] == 2 && delta.points[2][1] == -3);
    CHECK(
        sent.flags ==
        (sim::ground_orders::route_present_flag | sim::ground_orders::route_sent_blocked_flag)
    );
    CHECK(!route_needs_send(sent, sent_motion));
    sent_motion.flags = 0;
    CHECK(route_needs_send(sent, sent_motion));
    sent.flags = 0;
    delta = take_route_delta(sent, sent_motion);
    CHECK(!delta.blocked && delta.count == 0);

    sim::ground_orders::MirroredNavigation mirrored;
    sim::unit_movement::Movement mirrored_motion;
    RouteDelta incoming;
    incoming.blocked = true;
    incoming.count = 2;
    incoming.points[0] = {8, 8};
    incoming.points[1] = {88, 8};
    apply_route_delta(mirrored, mirrored_motion, incoming);
    CHECK(
        sim::ground_orders::mirrored_route_present(mirrored) &&
        (mirrored_motion.flags & sim::unit_movement::collision_blocked) != 0
    );
    CHECK(mirrored.points[1][0] == 88 && mirrored.count == 2);
    incoming.blocked = false;
    incoming.count = 1;
    apply_route_delta(mirrored, mirrored_motion, incoming);
    CHECK(
        !sim::ground_orders::mirrored_route_present(mirrored) &&
        (mirrored_motion.flags & sim::unit_movement::collision_blocked) == 0
    );
}

// The 0x14 record a unit is handed over with: its build progress truncated
// toward zero, so any unit already started goes as finished; its health
// sign-extended; bank and heading in one word; and the three stockpiles by
// the first slot's weapon alone.
void unit_transfer_records() {
    Unit unit{};
    unit.id = 0x123;
    unit.build_remaining = 0.75F;
    unit.health = -5;
    unit.bank = -1;
    unit.heading = 0x8001;
    unit.pitch = 7;
    unit.weapons[0].stockpile = 1;
    unit.weapons[1].stockpile = 2;
    unit.weapons[1].flags = OA_UNIT_WEAPON_ENABLED;
    unit.weapons[2].stockpile = 3;
    auto record = unit_transfer_record(unit, 0x2001);
    CHECK(record.unit_index == 0x123 && record.new_owner_id == 0x2001);
    CHECK(record.build_remaining == 0 && record.health == -5);
    CHECK(record.bank_heading == 0x8001ffffu && record.pitch == 7);
    CHECK(
        record.weapon_stockpiles[0] == 0 && record.weapon_stockpiles[1] == 0 &&
        record.weapon_stockpiles[2] == 0
    );
    unit.build_remaining = 1.0F;
    unit.weapons[0].flags = OA_UNIT_WEAPON_ENABLED;
    record = unit_transfer_record(unit, 0x2001);
    CHECK(record.build_remaining == 1);
    CHECK(
        record.weapon_stockpiles[0] == 1 && record.weapon_stockpiles[1] == 2 &&
        record.weapon_stockpiles[2] == 3
    );
    uint8_t bytes[24];
    std::size_t written = 0;
    CHECK(
        netgame::encode_record(record, bytes, sizeof bytes, &written) == netgame::WireError::ok &&
        written == 24
    );
    CHECK(bytes[0] == 0x14 && bytes[7] == 1 && bytes[11] == 0xfb && bytes[14] == 0xff);
}
} // namespace

int main() {
    unit_flags_record();
    unit_killed_record();
    feature_records();
    wide_feature_records();
    unit_transfer_records();
    route_deltas();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("sim records passed");
    return 0;
}
