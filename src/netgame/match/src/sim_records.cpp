// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/match/sim_records.hpp"

#include "oa/netgame/player_slots.hpp"
#include "oa/netgame/records.hpp"

#include <iterator>

namespace oa::netgame::match {
namespace {

FeatureRecordLink& link_of(void* link) {
    return *static_cast<FeatureRecordLink*>(link);
}

bool networked(const FeatureRecordLink& link) {
    return link.networked != nullptr && link.networked(link.context);
}

void send_feature_record(
    const FeatureRecordLink& link,
    uint8_t action,
    int32_t x,
    int32_t z,
    bool to_host,
    uint8_t sender
) {
    if (link.send == nullptr)
        return;
    const auto record = encode_feature_record(action, x, z);
    link.send(link.context, record.data(), to_host, sender);
}

} // namespace

std::array<uint8_t, unit_flags_record_bytes>
encode_unit_flags_record(uint16_t unit, uint8_t flags) noexcept {
    UnitStateFlagsRecord record{};
    record.unit_index = unit;
    record.state_mask = flags;
    std::array<uint8_t, unit_flags_record_bytes> bytes{};
    (void)encode_record(record, bytes.data(), bytes.size(), nullptr);
    return bytes;
}

std::array<uint8_t, unit_killed_record_bytes> encode_unit_killed_record(
    const World& world, uint16_t unit, uint8_t kind, int8_t killed_percent, uint8_t wreck_level
) noexcept {
    UnitKilledRecord record{};
    record.unit_index = unit;
    record.attacker_owner_id = no_player_id;
    if (const auto* u = world_unit_at(&world, unit)) {
        record.attacker_owner_id = player_slot_id(world.game, u->last_attacker_owner);
        record.attacker_unit_index = static_cast<uint16_t>(u->last_attacker_id);
    }
    record.killed_percent = killed_percent;
    record.kind_and_wreck_level = static_cast<uint8_t>(
        (kind << unit_killed_kind_shift) | (wreck_level & unit_killed_wreck_level_mask)
    );
    std::array<uint8_t, unit_killed_record_bytes> bytes{};
    (void)encode_record(record, bytes.data(), bytes.size(), nullptr);
    return bytes;
}

UnitTransferRecord unit_transfer_record(const Unit& unit, uint32_t new_owner_id) noexcept {
    UnitTransferRecord record{};
    record.unit_index = unit.id;
    record.new_owner_id = new_owner_id;
    record.build_remaining = static_cast<int32_t>(unit.build_remaining);
    record.health = unit.health;
    record.bank_heading = static_cast<uint16_t>(unit.bank) |
                          (static_cast<uint32_t>(static_cast<uint16_t>(unit.heading)) << 16);
    record.pitch = static_cast<uint16_t>(unit.pitch);
    // Every slot's stockpile goes by the first slot's weapon, as in 3.1c.
    if ((unit.weapons[0].flags & OA_UNIT_WEAPON_ENABLED) != 0)
        for (std::size_t slot = 0; slot < std::size(record.weapon_stockpiles); ++slot)
            record.weapon_stockpiles[slot] = unit.weapons[slot].stockpile;
    return record;
}

std::array<uint8_t, feature_record_bytes>
encode_feature_record(uint8_t action, int32_t cell_x, int32_t cell_z) noexcept {
    FeatureEventRecord record{};
    record.action = action;
    record.x = static_cast<uint16_t>(cell_x);
    record.y = static_cast<uint16_t>(cell_z);
    std::array<uint8_t, feature_record_bytes> bytes{};
    (void)encode_record(record, bytes.data(), bytes.size(), nullptr);
    return bytes;
}

bool feature_hit_elsewhere(void* context, uint16_t weapon_id, int32_t cell_x, int32_t cell_z) {
    auto& link = link_of(context);
    if (!networked(link))
        return false;
    auto* world = link.world;
    const auto* viewpoint = world_player(world, world->game.viewpoint_player);
    const auto* info = viewpoint != nullptr ? world_player_info(world, viewpoint) : nullptr;
    if (info != nullptr && (info->role & lobby_role_feature_authority) != 0)
        return false;
    // 3.1c's record holds one byte, so a weapon past 255 goes out as its low byte.
    send_feature_record(
        link, static_cast<uint8_t>(weapon_id), cell_x, cell_z, true, no_player_slot
    );
    return true;
}

void feature_changed(
    void* context,
    sim::feature_runtime::FeatureChange change,
    int32_t cell_x,
    int32_t cell_z,
    const Unit* reclaimer
) {
    auto& link = link_of(context);
    switch (change) {
    case sim::feature_runtime::FeatureChange::ignited:
        send_feature_record(link, feature_action_tree_burn, cell_x, cell_z, false, no_player_slot);
        break;
    case sim::feature_runtime::FeatureChange::destroyed:
        if (networked(link))
            send_feature_record(
                link, feature_action_queue_event, cell_x, cell_z, false, no_player_slot
            );
        break;
    case sim::feature_runtime::FeatureChange::reclaimed:
    case sim::feature_runtime::FeatureChange::resurrected:
        if (networked(link))
            send_feature_record(
                link,
                feature_action_queue_event_flagged,
                cell_x,
                cell_z,
                false,
                reclaimer != nullptr ? reclaimer->owner_index : no_player_slot
            );
        break;
    }
}

} // namespace oa::netgame::match
