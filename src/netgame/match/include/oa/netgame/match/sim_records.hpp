// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Records built from the simulation's shared events: the unit state-flags
// record (0x11) for a unit_flags_changed event, the unit-killed record (0x0c)
// for a unit_killed event, the unit-transfer record (0x14) for a
// unit_transferred event, and the feature record (0x0f, or 0xe2 when the
// profile raises weapon-types) for the feature code's hit and change hooks.

#include "oa/core/world.h"
#include "oa/netgame/records.hpp"
#include "oa/sim/feature_runtime.hpp"

#include <array>
#include <cstdint>

namespace oa::netgame::match {

inline constexpr std::size_t unit_flags_record_bytes = 4;
inline constexpr std::size_t unit_killed_record_bytes = 11;
inline constexpr std::size_t feature_record_bytes = 6;

// Lobby role bit of the player that applies feature damage (? authority).
inline constexpr uint8_t lobby_role_feature_authority = 0x01;

/// Encodes a 0x11 unit state-flags record: {0x11, unit low, unit high, flags}.
///
/// @param unit Unit index.
/// @param flags New state flags.
/// @return The 4-byte record.
[[nodiscard]] std::array<uint8_t, unit_flags_record_bytes>
encode_unit_flags_record(uint16_t unit, uint8_t flags) noexcept;

/// Encodes the 0x0c record a unit simulated here dies with.
///
/// The record is {0x0c, unit, the player id of its last attacker's owner,
/// its last attacker, the Killed percentage, kind << 4 | wreck level},
/// little-endian. The owner's id is no_player_id when the unit has no last
/// attacker or that player's slot is free; the last attacker is 0 for none,
/// and a unit past the pool has none.
///
/// @param world Match world; the unit is still live, with its last attacker.
/// @param unit Unit slot.
/// @param kind Death kind; its low four bits are sent.
/// @param killed_percent Killed percentage the unit dies with.
/// @param wreck_level Wreck level, 0 for none; its low four bits are sent.
/// @return The 11-byte record.
[[nodiscard]] std::array<uint8_t, unit_killed_record_bytes> encode_unit_killed_record(
    const World& world, uint16_t unit, uint8_t kind, int8_t killed_percent, uint8_t wreck_level
) noexcept;

/// Builds the 0x14 record that hands a unit simulated here to a player another machine simulates.
///
/// It carries the unit's slot, the new owner's player id, Unit.build_remaining
/// truncated toward zero to an integer, Unit.health sign-extended, Unit.bank
/// in the low and Unit.heading in the high half of one word, Unit.pitch,
/// and the three weapon slots' stockpiles when the first slot's weapon is
/// enabled (OA_UNIT_WEAPON_ENABLED), else three zeros.
///
/// @param unit The unit handed over, still live.
/// @param new_owner_id Player id of the receiving player.
/// @return The record.
/// @quirk Build progress runs from 1.0 (not started) down to 0.0 (finished),
///        so the truncation sends 0 for any unit already started: an
///        unfinished unit given away arrives finished, as in 3.1c.
[[nodiscard]] UnitTransferRecord
unit_transfer_record(const Unit& unit, uint32_t new_owner_id) noexcept;

/// Encodes a 0x0f feature record: {0x0f, action, x low, x high, z low, z high}.
///
/// @param action Feature action (0xfd..0xff) or, for a hit handed to the authority, the weapon id.
/// @param cell_x Plot column, truncated to 16 bits.
/// @param cell_z Plot row, truncated to 16 bits.
/// @return The 6-byte record.
[[nodiscard]] std::array<uint8_t, feature_record_bytes>
encode_feature_record(uint8_t action, int32_t cell_x, int32_t cell_z) noexcept;

/// Bytes of a wide feature record, the type byte included.
inline constexpr std::size_t wide_feature_record_bytes =
    record_table_length(static_cast<uint8_t>(WideRecordType::feature_event));
static_assert(wide_feature_record_bytes == 8);

/// Encodes a 0xe2 feature record.
///
/// The weapon id is sent only for a weapon hit. Every other action goes out
/// with id 0.
///
/// @param action What the receiver does.
/// @param weapon Weapon id of a hit; ignored for every other action.
/// @param cell_x Plot column, truncated to 16 bits.
/// @param cell_z Plot row, truncated to 16 bits.
/// @return The 8-byte record.
[[nodiscard]] std::array<uint8_t, wide_feature_record_bytes> encode_wide_feature_record(
    WideFeatureAction action, uint16_t weapon, int32_t cell_x, int32_t cell_z
) noexcept;

// Where the feature hooks below send their records. match_binding_install
// installs them through MultiplayerHooks' feature entries, which
// Match::feature_host passes to the feature code.
struct FeatureRecordLink {
    World* world{};
    void* context{};
    // to_host sends the record to the authority only, otherwise it goes to
    // every player; it goes out from the player in slot sender, or from the
    // first player on this machine when sender is no_player_slot.
    void (*send)(
        void* context, const uint8_t* record, std::size_t size, bool to_host, uint8_t sender
    ){};
    // Whether the session is a network game (object state 3).
    bool (*networked)(void* context){};
    /// Feature records go out as 0xe2. False keeps the 6-byte 0x0f.
    bool wide{};
};

/// Implements FeatureHost::feature_hit_elsewhere over a FeatureRecordLink.
///
/// In a network game a viewpoint without the authority role hands the hit's
/// weapon to the authority and applies nothing.
///
/// @param link The FeatureRecordLink.
/// @param weapon_id Weapon that hit the feature.
/// @param cell_x Plot column.
/// @param cell_z Plot row.
/// @return True when the hit was handed to the authority and must not be applied here.
bool feature_hit_elsewhere(void* link, uint16_t weapon_id, int32_t cell_x, int32_t cell_z);

/// Implements FeatureHost::feature_changed over a FeatureRecordLink.
///
/// An ignition always goes out as tree burn (0xfe); a destruction (the
/// authority's die sequence, 0xfd), a reclaim and a resurrection (both the
/// reclaim sequence, 0xff, which takes the wreck away on every machine) go
/// out only in a network game. Every record goes to all players: a reclaim
/// or resurrection from the owner of the unit that did it, the others from
/// the first player on this machine.
///
/// @param link The FeatureRecordLink.
/// @param change What happened to the feature.
/// @param cell_x Plot column; a resurrected wreck's origin column.
/// @param cell_z Plot row; a resurrected wreck's origin row.
/// @param reclaimer The unit that finished reclaiming or resurrecting the feature; null for a fire or a
///        destruction.
void feature_changed(
    void* link,
    sim::feature_runtime::FeatureChange change,
    int32_t cell_x,
    int32_t cell_z,
    const Unit* reclaimer
);

} // namespace oa::netgame::match
