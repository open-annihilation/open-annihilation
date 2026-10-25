// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Game packet records 0x02..0x2c and OA's wide weapon records 0xe0..0xe2: one
// plain struct per type with its wire layout (offsets from the type byte),
// plus bounded encode/decode. Bytes that nothing reads are named for their
// place in the record and carried unchanged, so every record round-trips.

#include "oa/netgame/wire.hpp"
#include <bit>
#include <cstring>

namespace oa::netgame {

// 0x02, 13 bytes.
struct PingRecord {
    static constexpr RecordType type = RecordType::ping;
    uint32_t origin_tick_count{}; // the originator's millisecond clock
    uint32_t echo_tick_count{};   // the responder's millisecond clock, 0 in the request
    uint32_t origin_player_id{};
};

// 0x03, 3 bytes: a length-table row that 3.1c neither sends nor handles.
struct Unused03Record {
    static constexpr RecordType type = RecordType::unused_03;
    uint8_t payload[2]{}; // never read; carried unchanged
};

// 0x05, 65 bytes. Text is not guaranteed to be NUL terminated.
struct ChatRecord {
    static constexpr RecordType type = RecordType::chat;
    char text[64]{};
};

// 0x06, 1 byte.
struct ProbeRecord {
    static constexpr RecordType type = RecordType::probe;
};

// 0x07, 1 byte: reply to 0x06.
struct ProbeReplyRecord {
    static constexpr RecordType type = RecordType::probe_reply;
};

// 0x08, 1 byte.
struct GameStartRecord {
    static constexpr RecordType type = RecordType::game_start;
};

// 0x09, 23 bytes.
struct UnitCreatedRecord {
    static constexpr RecordType type = RecordType::unit_created;
    uint16_t unit_def_index{}; // Unit.type_index
    uint16_t unit_index{};     // Unit.id
    int32_t position[3]{};     // 16.16 x, y, z
    uint32_t bank_heading{};   // Unit.bank and Unit.heading; the receiver reads only a
                               // building's facing from the heading (units.build-rotation)
    uint16_t pitch{};          // Unit.pitch, not read by the receiver
};

// 0x0a, 7 bytes.
struct UnitLinkRecord {
    static constexpr RecordType type = RecordType::unit_link;
    uint16_t unit_index{};
    uint16_t linked_unit_index{};
    int8_t attach_piece{}; // Unit.attach_piece
    uint8_t occupancy{};   // Unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK
};

// 0x0b, 9 bytes.
struct UnitDamageRecord {
    static constexpr RecordType type = RecordType::unit_damage;
    uint16_t target_unit_index{};
    uint16_t source_unit_index{};
    uint16_t amount{};
    uint8_t direction{}; // high byte of the hit's direction word
    uint8_t kind{};      // stored in Unit.damage_kind
};

// 0x0c, 11 bytes: a unit's death, from the player whose machine simulates
// the unit to every player.
struct UnitKilledRecord {
    static constexpr RecordType type = RecordType::unit_killed;
    uint16_t unit_index{}; // Unit.id
    uint32_t
        attacker_owner_id{}; // player id of the owner of the unit that last damaged it, no_player_id for none
    uint16_t attacker_unit_index{}; // Unit.last_attacker_id, 0 = none
    int8_t killed_percent{};        // Killed's first argument; above zero a finished unit explodes
    uint8_t
        kind_and_wreck_level{}; // high nibble the death kind, low nibble the wreck level (0 none, 1 the
                                // corpse, each step further along featuredead)
};

// The two halves of UnitKilledRecord.kind_and_wreck_level.
inline constexpr uint8_t unit_killed_kind_shift = 4;
inline constexpr uint8_t unit_killed_wreck_level_mask = 0x0f;

// 0x0d, 36 bytes: a shot, from its unit's owner to every player.
// The meteor sender (whose target is its fall velocity) fills only bytes
// 0x00..0x19; the rest is unspecified.
struct WeaponFireRecord {
    static constexpr RecordType type = RecordType::weapon_fire;
    int32_t start[3]{};
    int32_t target[3]{};
    uint8_t weapon_id{};          // WeaponDef.weapon_id
    uint8_t flags{};              // bit 0 the weapon's interceptor bit, other bits unspecified
    int16_t aim_heading{};        // UnitWeapon.aim_heading
    int16_t aim_pitch{};          // UnitWeapon.aim_pitch
    uint16_t target_unit_index{}; // 0 = none
    uint16_t source_unit_index{}; // 0 = none
    uint8_t slot{};               // weapon slot, (UnitWeapon.flags >> 2) & 3
};

inline constexpr uint8_t weapon_fire_interceptor = 0x01;

// 0x0e, 14 bytes: a shot an interceptor's blast set off, named by the
// point it was aimed at and its weapon.
struct ProjectileInterceptedRecord {
    static constexpr RecordType type = RecordType::projectile_intercepted;
    int32_t target[3]{}; // Projectile.target
    uint8_t weapon_id{};
};

// 0x0f, 6 bytes.
struct FeatureEventRecord {
    static constexpr RecordType type = RecordType::feature_event;
    uint8_t action{}; // 0xfd/0xfe/0xff, else a weapon (Game.weapon_defs index)
    uint16_t x{};
    uint16_t y{};
};

inline constexpr uint8_t feature_action_queue_event = 0xfd;
inline constexpr uint8_t feature_action_tree_burn = 0xfe;
inline constexpr uint8_t feature_action_queue_event_flagged = 0xff;

/// What a wide feature record asks the receiver to do.
enum class WideFeatureAction : uint8_t {
    weapon_hit,          ///< the weapon id names the weapon that hit
    queue_event,         ///< the 0x0f queue whose action byte is 0xfd
    tree_burn,           ///< the 0x0f tree burn whose action byte is 0xfe
    queue_event_flagged, ///< the 0x0f flagged queue whose action byte is 0xff
};

// 0xe0, 37 bytes: a shot whose weapon id is 16 bits. Field order matches
// 0x0d with the id widened.
struct WideWeaponFireRecord {
    static constexpr WideRecordType type = WideRecordType::weapon_fire;
    int32_t start[3]{};
    int32_t target[3]{};
    uint16_t weapon_id{};
    uint8_t flags{};
    int16_t aim_heading{};
    int16_t aim_pitch{};
    uint16_t target_unit_index{};
    uint16_t source_unit_index{};
    uint8_t slot{};

    /// Compares every field.
    bool operator==(const WideWeaponFireRecord&) const = default;
};

// 0xe1, 15 bytes: an intercepted shot whose weapon id is 16 bits.
struct WideProjectileInterceptedRecord {
    static constexpr WideRecordType type = WideRecordType::projectile_intercepted;
    int32_t target[3]{};
    uint16_t weapon_id{};

    /// Compares every field.
    bool operator==(const WideProjectileInterceptedRecord&) const = default;
};

// 0xe2, 8 bytes: a feature event whose weapon id is 16 bits. The action is
// a WideFeatureAction. For every action other than a weapon hit the id is 0.
struct WideFeatureEventRecord {
    static constexpr WideRecordType type = WideRecordType::feature_event;
    uint8_t action{};
    uint16_t weapon_id{};
    uint16_t x{};
    uint16_t y{};

    /// Compares every field.
    bool operator==(const WideFeatureEventRecord&) const = default;
};

/// Copies a narrow shot into the wide form, widening its weapon id.
///
/// @param in The 0x0d record.
/// @return The same fields with the id stored in 16 bits.
[[nodiscard]] constexpr WideWeaponFireRecord widen(const WeaponFireRecord& in) noexcept {
    WideWeaponFireRecord out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.start[i] = in.start[i];
        out.target[i] = in.target[i];
    }
    out.weapon_id = in.weapon_id;
    out.flags = in.flags;
    out.aim_heading = in.aim_heading;
    out.aim_pitch = in.aim_pitch;
    out.target_unit_index = in.target_unit_index;
    out.source_unit_index = in.source_unit_index;
    out.slot = in.slot;
    return out;
}

/// Copies a narrow interception into the wide form, widening its weapon id.
///
/// @param in The 0x0e record.
/// @return The same target and the id stored in 16 bits.
[[nodiscard]] constexpr WideProjectileInterceptedRecord
widen(const ProjectileInterceptedRecord& in) noexcept {
    WideProjectileInterceptedRecord out{};
    for (std::size_t i = 0; i < 3; ++i)
        out.target[i] = in.target[i];
    out.weapon_id = in.weapon_id;
    return out;
}

/// Copies a narrow feature event into the wide form.
///
/// Actions 0xfd, 0xfe and 0xff become the queue, tree-burn and flagged-queue
/// actions with weapon id 0. Any other action byte becomes a weapon hit whose
/// id is that byte.
///
/// @param in The 0x0f record.
/// @return The wide record.
[[nodiscard]] constexpr WideFeatureEventRecord widen(const FeatureEventRecord& in) noexcept {
    WideFeatureEventRecord out{};
    out.x = in.x;
    out.y = in.y;
    if (in.action == feature_action_queue_event) {
        out.action = static_cast<uint8_t>(WideFeatureAction::queue_event);
    } else if (in.action == feature_action_tree_burn) {
        out.action = static_cast<uint8_t>(WideFeatureAction::tree_burn);
    } else if (in.action == feature_action_queue_event_flagged) {
        out.action = static_cast<uint8_t>(WideFeatureAction::queue_event_flagged);
    } else {
        out.action = static_cast<uint8_t>(WideFeatureAction::weapon_hit);
        out.weapon_id = in.action;
    }
    return out;
}

// 0x10, 22 bytes.
struct CobStartRecord {
    static constexpr RecordType type = RecordType::cob_start;
    uint16_t unit_index{};
    int16_t function_index{}; // COB script table index
    uint8_t argument_count{};
    uint32_t args[4]{}; // all four written as locals
};

// 0x11, 4 bytes.
struct UnitStateFlagsRecord {
    static constexpr RecordType type = RecordType::unit_state_flags;
    uint16_t unit_index{};
    uint8_t state_mask{};
};

// 0x12, 5 bytes.
struct BuilderLinkRecord {
    static constexpr RecordType type = RecordType::builder_link;
    uint16_t subject_unit_index{};
    uint16_t source_unit_index{};
};

// 0x13, 18 bytes: a sound played on one machine, for every player to hear.
struct SoundRecord {
    static constexpr RecordType type = RecordType::sound;
    // Nonzero plays the sound by its index, 0 at its position; 3.1c always
    // sends 1.
    uint8_t indexed{};
    uint32_t table_index{}; // index into Game.sounds
    int32_t position[3]{};
};

// 0x14, 24 bytes: a unit handed to a player another machine simulates (a
// capture or a gift of units), sent by the unit's owner just before the
// unit dies there as captured.
struct UnitTransferRecord {
    static constexpr RecordType type = RecordType::unit_transfer;
    uint16_t unit_index{};
    uint32_t new_owner_id{};
    // the float Unit.build_remaining truncated toward zero, and back:
    // an unfinished unit given away arrives finished (quirk, as in 3.1c)
    int32_t build_remaining{};
    int32_t health{};        // low 16 bits -> Unit.health
    uint32_t bank_heading{}; // -> Unit.bank and Unit.heading
    uint16_t pitch{};        // -> Unit.pitch
    // the three weapons' UnitWeapon.stockpile when the first weapon's
    // flags hold OA_UNIT_WEAPON_ENABLED, else 0
    uint8_t weapon_stockpiles[3]{};
};

// 0x15, 1 byte.
struct LoadedRecord {
    static constexpr RecordType type = RecordType::loaded;
};

// 0x16, 17 bytes.
struct ResourceGiveRecord {
    static constexpr RecordType type = RecordType::resource_give;
    uint32_t subtype{}; // 1, 2 or 3
    uint32_t from_id{};
    uint32_t to_id{};
    float amount{}; // unused by subtype 3
};

// 0x17, 2 bytes.
struct PlayerValueRequestRecord {
    static constexpr RecordType type = RecordType::player_value_request;
    uint8_t value{}; // PlayerSetupInfo.color
};

// 0x18, 2 bytes.
struct PlayerValueReplyRecord {
    static constexpr RecordType type = RecordType::player_value_reply;
    uint8_t value{};
};

// 0x19, 3 bytes.
struct PauseSpeedRecord {
    static constexpr RecordType type = RecordType::pause_speed;
    uint8_t kind{};  // 0 = pause flag, otherwise game speed
    uint8_t value{}; // pause bit 0, or speed 1..20
};

inline constexpr uint8_t pause_speed_kind_pause = 0;

// 0x1a, 14 bytes: unit-definition content handshake.
struct UnitDefHandshakeRecord {
    static constexpr RecordType type = RecordType::unit_def_handshake;
    uint8_t subtype{};
    uint32_t zero{};  // always written 0
    uint32_t key{};   // UnitDef.fbi_hash
    uint32_t value{}; // per subtype; UnitDef.content_checksum for subtype 2
};

enum class HandshakeSubtype : uint8_t {
    announce = 0,       // sent to a newly seen peer
    def_count = 1,      // value = unit def count - 1
    def_checksum = 2,   // key + content checksum
    verdict = 3,        // value bytes: u8 local, u8 remote, s16 limit of the unit's UnitSyncRecord
    received_count = 4, // value = count of 0x1a records received
};

inline constexpr uint8_t handshake_subtype_limit = 100; // receiver drops >= 100

// 0x1b, 6 bytes.
struct RejectRecord {
    static constexpr RecordType type = RecordType::reject;
    uint32_t player_id{};
    uint8_t reason{}; // see RejectReason
};

enum class RejectReason : uint8_t {
    rejected = 1,
    leaving = 2, // the player's machine left the battle room
    game_closed = 3,
    wrong_password = 4,
    game_full = 5,
    connection_lost = 6,
    missing_unit = 7,
    version_too_old = 8,
    watching_disallowed = 9,
    creator_left = 10,
    deathmatch = 11, // a computer or defeated player, removed when deathmatch is chosen
};

// 0x1c, 5 bytes.
struct DisconnectNoticeRecord {
    static constexpr RecordType type = RecordType::disconnect_notice;
    uint32_t player_id{};
};

// 0x1d, 9 bytes: never admitted by the pump; no sender exists.
struct ResendRequestRecord {
    static constexpr RecordType type = RecordType::resend_request;
    uint32_t first{};
    uint32_t last{};
};

// 0x1e, 2 bytes.
struct StartPositionRecord {
    static constexpr RecordType type = RecordType::start_position;
    uint8_t position{};
};

// 0x1f, 5 bytes: ack of 0x1e.
struct StartPositionAckRecord {
    static constexpr RecordType type = RecordType::start_position_ack;
    uint32_t player_id{};
};

// 0x20, 186 bytes: the 0xb9-byte player info block (PlayerSetupInfo) with the
// sender id overlaid on the block's four bytes after info_head.
struct PlayerInfoRecord {
    static constexpr RecordType type = RecordType::player_info;
    uint8_t info_head[0x90]{}; // the block before the sender id
    uint32_t player_id{};      // the sender id, overlaid on the block
    uint8_t info_tail[0x25]{}; // the block after the sender id, from player_info_tail_offset
};

inline constexpr std::size_t player_info_block_bytes = 0xb9;
inline constexpr std::size_t player_info_tail_offset = 0x94;
inline constexpr std::size_t player_info_map_hash_offset = 0xa9; // block offset, u32
inline constexpr std::size_t player_info_version_major_offset = 0xa7;
inline constexpr std::size_t player_info_version_minor_offset = 0xa8;
inline constexpr std::size_t player_info_color_offset = 0x96; // block offset, the colour slot
/// Block offset of the recorder's protocol byte: the version of the recorder
/// the sender runs (recorder_protocol_current), 0 for none. 3.1c carries the
/// byte unchanged and never reads it.
inline constexpr std::size_t player_info_recorder_protocol_offset = 0xb4;
/// Block offset of the two bytes, 'O' and 'A', that say Open Annihilation sent
/// the block. 3.1c carries them unchanged and never reads them.
inline constexpr std::size_t player_info_engine_signature_offset = 0xad;
/// The engine signature's bytes.
inline constexpr uint8_t engine_signature_first = 'O';
inline constexpr uint8_t engine_signature_second = 'A';

/// Writes the engine signature's two bytes, the one place that does.
///
/// @param[out] at where the signature's first byte goes
inline void write_engine_signature(uint8_t* at) noexcept {
    at[0] = engine_signature_first;
    at[1] = engine_signature_second;
}

/// Marks a setup block as Open Annihilation's, in the block the machine keeps,
/// so that every copy and every send of it carries the signature.
///
/// @param[in,out] block the 0xb9-byte block, as a player's PlayerSetupInfo holds it
inline void mark_engine_signature(uint8_t* block) noexcept {
    write_engine_signature(block + player_info_engine_signature_offset);
}

/// Stamps the engine signature on a setup block about to be sent (record 0x20).
///
/// @param[in,out] record the block, as record 0x20 carries it
inline void stamp_engine_signature(PlayerInfoRecord& record) noexcept {
    write_engine_signature(
        record.info_tail + (player_info_engine_signature_offset - player_info_tail_offset)
    );
}

/// Tells whether a setup block says Open Annihilation sent it.
///
/// @param block the 0xb9-byte block, as a player's PlayerSetupInfo holds it; null is not
/// @return true when the engine signature is there
[[nodiscard]] inline bool sent_by_open_annihilation(const uint8_t* block) noexcept {
    return block != nullptr &&
           block[player_info_engine_signature_offset] == engine_signature_first &&
           block[player_info_engine_signature_offset + 1] == engine_signature_second;
}

/// Returns the map hash stored at player_info_map_hash_offset of a player info record.
[[nodiscard]] inline uint32_t player_info_map_hash(const PlayerInfoRecord& r) noexcept {
    return load_u32(r.info_tail + (player_info_map_hash_offset - player_info_tail_offset));
}

// 0x21, 10 bytes, to the host: assign 1 asks a group for player_id (the
// group of same_machine_id, or a free one when that is -1); assign 0 asks
// which group the host gave it.
struct MachineGroupRequestRecord {
    static constexpr RecordType type = RecordType::machine_group_request;
    uint8_t assign{};
    uint32_t player_id{};
    uint32_t same_machine_id{};
};

// 0x22, 6 bytes: the host's answer to 0x21, broadcast.
struct MachineGroupReplyRecord {
    static constexpr RecordType type = RecordType::machine_group_reply;
    uint32_t player_id{};
    uint8_t machine_group{};
};

// 0x23, 14 bytes.
struct AllianceRecord {
    static constexpr RecordType type = RecordType::alliance;
    uint32_t player_id_a{};
    uint32_t player_id_b{};
    uint8_t value{};
    uint32_t both_sides{}; // nonzero: b also sets its own alliance with a
};

// 0x24, 6 bytes.
struct PlayerTeamRecord {
    static constexpr RecordType type = RecordType::player_team;
    uint32_t player_id{};
    uint8_t value{}; // -> Player.team
};

// 0x25, 5 bytes: a length-table row that 3.1c neither sends nor handles.
struct Unused25Record {
    static constexpr RecordType type = RecordType::unused_25;
    uint8_t payload[4]{}; // never read; carried unchanged
};

// 0x26, 41 bytes.
struct SlotTableRecord {
    static constexpr RecordType type = RecordType::slot_table;
    uint32_t slot_ids[10]{}; // 0 = free slot, 0xffffffff = state 4
};

// 0x27, 17 bytes: 3.1c has no sender for it.
struct IntegrityNoticeRecord {
    static constexpr RecordType type = RecordType::integrity_notice;
    uint32_t player_id{};
    uint8_t reserved_after_player_id[12]{}; // never read; carried unchanged
};

// 0x28, 58 bytes.
struct EconomyRecord {
    static constexpr RecordType type = RecordType::economy;
    uint8_t want_reply{};
    int32_t kills{}; // sign-extended s16 Player fields
    int32_t losses{};
    int32_t commanders_killed{};
    int32_t commanders_lost{};
    float metal{};
    float energy{};
    float metal_storage{};
    float energy_storage{};
    float energy_produced_total{}; // six Player doubles narrowed to float
    float energy_requested_total{};
    float energy_wasted_total{};
    float metal_produced_total{};
    float metal_requested_total{};
    float metal_wasted_total{};
};

// 0x29, 3 bytes.
struct EconomyReplyRecord {
    static constexpr RecordType type = RecordType::economy_reply;
    // nonzero: the receiver sets its Player.economy_requested entry for the
    // sender; 0 makes the receiver ignore the record
    uint8_t mark_requested{};
    // nonzero, with mark_requested nonzero: the receiver also sets its
    // Player.economy_answered entry for the sender
    uint8_t mark_answered{};
};

// 0x2a, 2 bytes: the sender's loading progress, the mean of its six
// load-screen category percentages (Game.load_progress), stored in the
// sender's Player.load_progress.
struct LoadProgressRecord {
    static constexpr RecordType type = RecordType::load_progress;
    uint8_t percent{};
};

// 0x2c header; the bit-packed body is decoded by unit_state.hpp. The body
// view aliases the decoded input.
struct UnitStateRecord {
    static constexpr RecordType type = RecordType::unit_state;
    uint16_t length{}; // total bytes from the type byte
    uint32_t sender_tick{};
    const uint8_t* body{}; // bytes after the 7-byte header
    uint16_t body_size{};
};

// Field visitors: one per record, listing fields in wire order.
namespace detail {

/// Visitor that reads each field little-endian from p, starting after the type byte.
struct FieldReader {
    const uint8_t* p = nullptr;
    std::size_t at = 1;

    /// Reads one byte into v and advances past it.
    void u8(uint8_t& v) { v = p[at++]; }

    /// Reads one signed byte into v and advances past it.
    void i8(int8_t& v) { v = static_cast<int8_t>(p[at++]); }

    /// Reads a 16-bit field into v and advances past it.
    void u16(uint16_t& v) {
        v = load_u16(p + at);
        at += 2;
    }

    /// Reads a signed 16-bit field into v and advances past it.
    void i16(int16_t& v) {
        v = static_cast<int16_t>(load_u16(p + at));
        at += 2;
    }

    /// Reads a 32-bit field into v and advances past it.
    void u32(uint32_t& v) {
        v = load_u32(p + at);
        at += 4;
    }

    /// Reads a signed 32-bit field into v and advances past it.
    void i32(int32_t& v) {
        v = static_cast<int32_t>(load_u32(p + at));
        at += 4;
    }

    /// Reads a 32-bit float field into v and advances past it.
    void f32(float& v) {
        v = std::bit_cast<float>(load_u32(p + at));
        at += 4;
    }

    /// Reads a byte array into v and advances past it.
    template <std::size_t N>
    void bytes(uint8_t (&v)[N]) {
        std::memcpy(v, p + at, N);
        at += N;
    }

    /// Reads a character array into v and advances past it.
    template <std::size_t N>
    void chars(char (&v)[N]) {
        std::memcpy(v, p + at, N);
        at += N;
    }
};

/// Visitor that writes each field little-endian to p, starting after the type byte.
struct FieldWriter {
    uint8_t* p = nullptr;
    std::size_t at = 1;

    /// Writes one byte from v and advances past it.
    void u8(const uint8_t& v) { p[at++] = v; }

    /// Writes one signed byte from v and advances past it.
    void i8(const int8_t& v) { p[at++] = static_cast<uint8_t>(v); }

    /// Writes a 16-bit field from v and advances past it.
    void u16(const uint16_t& v) {
        store_u16(p + at, v);
        at += 2;
    }

    /// Writes a signed 16-bit field from v and advances past it.
    void i16(const int16_t& v) {
        store_u16(p + at, static_cast<uint16_t>(v));
        at += 2;
    }

    /// Writes a 32-bit field from v and advances past it.
    void u32(const uint32_t& v) {
        store_u32(p + at, v);
        at += 4;
    }

    /// Writes a signed 32-bit field from v and advances past it.
    void i32(const int32_t& v) {
        store_u32(p + at, static_cast<uint32_t>(v));
        at += 4;
    }

    /// Writes a 32-bit float field from v and advances past it.
    void f32(const float& v) {
        store_u32(p + at, std::bit_cast<uint32_t>(v));
        at += 4;
    }

    /// Writes a byte array from v and advances past it.
    template <std::size_t N>
    void bytes(const uint8_t (&v)[N]) {
        std::memcpy(p + at, v, N);
        at += N;
    }

    /// Writes a character array from v and advances past it.
    template <std::size_t N>
    void chars(const char (&v)[N]) {
        std::memcpy(p + at, v, N);
        at += N;
    }
};

/// Visits every field of a record in wire order; one overload per record, none for records without fields.
template <class V, class R>
constexpr void fields(V&, R&) {
}

/// Visits the fields of a PingRecord in wire order.
template <class V>
constexpr void fields(V& v, PingRecord& r) {
    v.u32(r.origin_tick_count);
    v.u32(r.echo_tick_count);
    v.u32(r.origin_player_id);
}

/// Visits the fields of a Unused03Record in wire order.
template <class V>
constexpr void fields(V& v, Unused03Record& r) {
    v.bytes(r.payload);
}

/// Visits the fields of a ChatRecord in wire order.
template <class V>
constexpr void fields(V& v, ChatRecord& r) {
    v.chars(r.text);
}

/// Visits the fields of a UnitCreatedRecord in wire order.
template <class V>
constexpr void fields(V& v, UnitCreatedRecord& r) {
    v.u16(r.unit_def_index);
    v.u16(r.unit_index);
    for (auto& c : r.position)
        v.i32(c);
    v.u32(r.bank_heading);
    v.u16(r.pitch);
}

/// Visits the fields of a UnitLinkRecord in wire order.
template <class V>
constexpr void fields(V& v, UnitLinkRecord& r) {
    v.u16(r.unit_index);
    v.u16(r.linked_unit_index);
    v.i8(r.attach_piece);
    v.u8(r.occupancy);
}

/// Visits the fields of a UnitDamageRecord in wire order.
template <class V>
constexpr void fields(V& v, UnitDamageRecord& r) {
    v.u16(r.target_unit_index);
    v.u16(r.source_unit_index);
    v.u16(r.amount);
    v.u8(r.direction);
    v.u8(r.kind);
}

/// Visits the fields of a UnitKilledRecord in wire order.
template <class V>
constexpr void fields(V& v, UnitKilledRecord& r) {
    v.u16(r.unit_index);
    v.u32(r.attacker_owner_id);
    v.u16(r.attacker_unit_index);
    v.i8(r.killed_percent);
    v.u8(r.kind_and_wreck_level);
}

/// Visits the fields of a WeaponFireRecord in wire order.
template <class V>
constexpr void fields(V& v, WeaponFireRecord& r) {
    for (auto& c : r.start)
        v.i32(c);
    for (auto& c : r.target)
        v.i32(c);
    v.u8(r.weapon_id);
    v.u8(r.flags);
    v.i16(r.aim_heading);
    v.i16(r.aim_pitch);
    v.u16(r.target_unit_index);
    v.u16(r.source_unit_index);
    v.u8(r.slot);
}

/// Visits the fields of a ProjectileInterceptedRecord in wire order.
template <class V>
constexpr void fields(V& v, ProjectileInterceptedRecord& r) {
    for (auto& c : r.target)
        v.i32(c);
    v.u8(r.weapon_id);
}

/// Visits the fields of a FeatureEventRecord in wire order.
template <class V>
constexpr void fields(V& v, FeatureEventRecord& r) {
    v.u8(r.action);
    v.u16(r.x);
    v.u16(r.y);
}

/// Visits the fields of a WideWeaponFireRecord in wire order.
template <class V>
constexpr void fields(V& v, WideWeaponFireRecord& r) {
    for (auto& c : r.start)
        v.i32(c);
    for (auto& c : r.target)
        v.i32(c);
    v.u16(r.weapon_id);
    v.u8(r.flags);
    v.i16(r.aim_heading);
    v.i16(r.aim_pitch);
    v.u16(r.target_unit_index);
    v.u16(r.source_unit_index);
    v.u8(r.slot);
}

/// Visits the fields of a WideProjectileInterceptedRecord in wire order.
template <class V>
constexpr void fields(V& v, WideProjectileInterceptedRecord& r) {
    for (auto& c : r.target)
        v.i32(c);
    v.u16(r.weapon_id);
}

/// Visits the fields of a WideFeatureEventRecord in wire order.
template <class V>
constexpr void fields(V& v, WideFeatureEventRecord& r) {
    v.u8(r.action);
    v.u16(r.weapon_id);
    v.u16(r.x);
    v.u16(r.y);
}

/// Visits the fields of a CobStartRecord in wire order.
template <class V>
constexpr void fields(V& v, CobStartRecord& r) {
    v.u16(r.unit_index);
    v.i16(r.function_index);
    v.u8(r.argument_count);
    for (auto& a : r.args)
        v.u32(a);
}

/// Visits the fields of a UnitStateFlagsRecord in wire order.
template <class V>
constexpr void fields(V& v, UnitStateFlagsRecord& r) {
    v.u16(r.unit_index);
    v.u8(r.state_mask);
}

/// Visits the fields of a BuilderLinkRecord in wire order.
template <class V>
constexpr void fields(V& v, BuilderLinkRecord& r) {
    v.u16(r.subject_unit_index);
    v.u16(r.source_unit_index);
}

/// Visits the fields of a SoundRecord in wire order.
template <class V>
constexpr void fields(V& v, SoundRecord& r) {
    v.u8(r.indexed);
    v.u32(r.table_index);
    for (auto& c : r.position)
        v.i32(c);
}

/// Visits the fields of a UnitTransferRecord in wire order.
template <class V>
constexpr void fields(V& v, UnitTransferRecord& r) {
    v.u16(r.unit_index);
    v.u32(r.new_owner_id);
    v.i32(r.build_remaining);
    v.i32(r.health);
    v.u32(r.bank_heading);
    v.u16(r.pitch);
    v.bytes(r.weapon_stockpiles);
}

/// Visits the fields of a ResourceGiveRecord in wire order.
template <class V>
constexpr void fields(V& v, ResourceGiveRecord& r) {
    v.u32(r.subtype);
    v.u32(r.from_id);
    v.u32(r.to_id);
    v.f32(r.amount);
}

/// Visits the fields of a PlayerValueRequestRecord in wire order.
template <class V>
constexpr void fields(V& v, PlayerValueRequestRecord& r) {
    v.u8(r.value);
}

/// Visits the fields of a PlayerValueReplyRecord in wire order.
template <class V>
constexpr void fields(V& v, PlayerValueReplyRecord& r) {
    v.u8(r.value);
}

/// Visits the fields of a PauseSpeedRecord in wire order.
template <class V>
constexpr void fields(V& v, PauseSpeedRecord& r) {
    v.u8(r.kind);
    v.u8(r.value);
}

/// Visits the fields of a UnitDefHandshakeRecord in wire order.
template <class V>
constexpr void fields(V& v, UnitDefHandshakeRecord& r) {
    v.u8(r.subtype);
    v.u32(r.zero);
    v.u32(r.key);
    v.u32(r.value);
}

/// Visits the fields of a RejectRecord in wire order.
template <class V>
constexpr void fields(V& v, RejectRecord& r) {
    v.u32(r.player_id);
    v.u8(r.reason);
}

/// Visits the fields of a DisconnectNoticeRecord in wire order.
template <class V>
constexpr void fields(V& v, DisconnectNoticeRecord& r) {
    v.u32(r.player_id);
}

/// Visits the fields of a ResendRequestRecord in wire order.
template <class V>
constexpr void fields(V& v, ResendRequestRecord& r) {
    v.u32(r.first);
    v.u32(r.last);
}

/// Visits the fields of a StartPositionRecord in wire order.
template <class V>
constexpr void fields(V& v, StartPositionRecord& r) {
    v.u8(r.position);
}

/// Visits the fields of a StartPositionAckRecord in wire order.
template <class V>
constexpr void fields(V& v, StartPositionAckRecord& r) {
    v.u32(r.player_id);
}

/// Visits the fields of a PlayerInfoRecord in wire order.
template <class V>
constexpr void fields(V& v, PlayerInfoRecord& r) {
    v.bytes(r.info_head);
    v.u32(r.player_id);
    v.bytes(r.info_tail);
}

/// Visits the fields of a MachineGroupRequestRecord in wire order.
template <class V>
constexpr void fields(V& v, MachineGroupRequestRecord& r) {
    v.u8(r.assign);
    v.u32(r.player_id);
    v.u32(r.same_machine_id);
}

/// Visits the fields of a MachineGroupReplyRecord in wire order.
template <class V>
constexpr void fields(V& v, MachineGroupReplyRecord& r) {
    v.u32(r.player_id);
    v.u8(r.machine_group);
}

/// Visits the fields of a AllianceRecord in wire order.
template <class V>
constexpr void fields(V& v, AllianceRecord& r) {
    v.u32(r.player_id_a);
    v.u32(r.player_id_b);
    v.u8(r.value);
    v.u32(r.both_sides);
}

/// Visits the fields of a PlayerTeamRecord in wire order.
template <class V>
constexpr void fields(V& v, PlayerTeamRecord& r) {
    v.u32(r.player_id);
    v.u8(r.value);
}

/// Visits the fields of a Unused25Record in wire order.
template <class V>
constexpr void fields(V& v, Unused25Record& r) {
    v.bytes(r.payload);
}

/// Visits the fields of a SlotTableRecord in wire order.
template <class V>
constexpr void fields(V& v, SlotTableRecord& r) {
    for (auto& s : r.slot_ids)
        v.u32(s);
}

/// Visits the fields of a IntegrityNoticeRecord in wire order.
template <class V>
constexpr void fields(V& v, IntegrityNoticeRecord& r) {
    v.u32(r.player_id);
    v.bytes(r.reserved_after_player_id);
}

/// Visits the fields of a EconomyRecord in wire order.
template <class V>
constexpr void fields(V& v, EconomyRecord& r) {
    v.u8(r.want_reply);
    v.i32(r.kills);
    v.i32(r.losses);
    v.i32(r.commanders_killed);
    v.i32(r.commanders_lost);
    v.f32(r.metal);
    v.f32(r.energy);
    v.f32(r.metal_storage);
    v.f32(r.energy_storage);
    v.f32(r.energy_produced_total);
    v.f32(r.energy_requested_total);
    v.f32(r.energy_wasted_total);
    v.f32(r.metal_produced_total);
    v.f32(r.metal_requested_total);
    v.f32(r.metal_wasted_total);
}

/// Visits the fields of a EconomyReplyRecord in wire order.
template <class V>
constexpr void fields(V& v, EconomyReplyRecord& r) {
    v.u8(r.mark_requested);
    v.u8(r.mark_answered);
}

/// Visits the fields of a LoadProgressRecord in wire order.
template <class V>
constexpr void fields(V& v, LoadProgressRecord& r) {
    v.u8(r.percent);
}

} // namespace detail

/// Decodes a fixed-length record.
///
/// @tparam R Record struct; not UnitStateRecord.
/// @param bytes Record start, type byte first.
/// @param size Record size; must equal the type's table length.
/// @param[out] out Decoded record; written only on success.
/// @return ok; bad_argument for a null pointer; truncated or length_mismatch when size differs from
///         the table length; invalid_type when bytes[0] is not R's type.
template <class R>
[[nodiscard]] WireError decode_record(const uint8_t* bytes, std::size_t size, R* out) noexcept {
    constexpr auto length = record_table_length(static_cast<uint8_t>(R::type));
    static_assert(
        length != 0 && static_cast<uint8_t>(R::type) != static_cast<uint8_t>(RecordType::unit_state)
    );
    if (bytes == nullptr || out == nullptr)
        return WireError::bad_argument;
    if (size < length)
        return WireError::truncated;
    if (size != length)
        return WireError::length_mismatch;
    if (bytes[0] != static_cast<uint8_t>(R::type))
        return WireError::invalid_type;
    R record{};
    detail::FieldReader reader{bytes};
    detail::fields(reader, record);
    *out = record;
    return WireError::ok;
}

/// Encodes a fixed-length record, writing exactly its table length.
///
/// @tparam R Record struct; not UnitStateRecord.
/// @param in Record to encode.
/// @param[out] out Destination buffer.
/// @param capacity Bytes available at out.
/// @param[out] written Bytes written, when not null.
/// @return ok; bad_argument when out is null; buffer_too_small when capacity is below the table length.
template <class R>
[[nodiscard]] WireError
encode_record(const R& in, uint8_t* out, std::size_t capacity, std::size_t* written) noexcept {
    constexpr auto length = record_table_length(static_cast<uint8_t>(R::type));
    static_assert(
        length != 0 && static_cast<uint8_t>(R::type) != static_cast<uint8_t>(RecordType::unit_state)
    );
    if (out == nullptr)
        return WireError::bad_argument;
    if (capacity < length)
        return WireError::buffer_too_small;
    R record = in;
    out[0] = static_cast<uint8_t>(R::type);
    detail::FieldWriter writer{out};
    detail::fields(writer, record);
    if (written)
        *written = length;
    return WireError::ok;
}

// Every visitor must cover the whole record.
namespace detail {
/// Visitor that only advances by each field's size.
struct FieldCounter {
    std::size_t at = 1;

    /// Advances past one byte.
    constexpr void u8(uint8_t&) { at += 1; }

    /// Advances past one signed byte.
    constexpr void i8(int8_t&) { at += 1; }

    /// Advances past a 16-bit field.
    constexpr void u16(uint16_t&) { at += 2; }

    /// Advances past a signed 16-bit field.
    constexpr void i16(int16_t&) { at += 2; }

    /// Advances past a 32-bit field.
    constexpr void u32(uint32_t&) { at += 4; }

    /// Advances past a signed 32-bit field.
    constexpr void i32(int32_t&) { at += 4; }

    /// Advances past a 32-bit float field.
    constexpr void f32(float&) { at += 4; }

    /// Advances past a byte array.
    template <std::size_t N>
    constexpr void bytes(uint8_t (&)[N]) {
        at += N;
    }

    /// Advances past a character array.
    template <std::size_t N>
    constexpr void chars(char (&)[N]) {
        at += N;
    }
};

/// Returns the wire length the visitors cover for a record, type byte included.
template <class R>
constexpr std::size_t visited_length() {
    R r{};
    FieldCounter c;
    fields(c, r);
    return c.at;
}
} // namespace detail

/// Decodes the header of a 0x2c record and views its body in place.
///
/// @param bytes Record start, type byte first.
/// @param size Bytes readable from bytes; at least the record's own u16 length.
/// @param[out] out Header whose body pointer aliases bytes; written only on success.
/// @return ok; bad_argument for a null pointer; truncated below 7 bytes; invalid_type when bytes[0] is
///         not 0x2c; length_mismatch when the u16 length is below 7 or above size.
[[nodiscard]] WireError
decode_unit_state_record(const uint8_t* bytes, std::size_t size, UnitStateRecord* out) noexcept;

// Tagged holder for any record, used by generic dispatch and tests.
struct AnyRecord {
    RecordType type{};

    union {
        PingRecord ping;
        Unused03Record unused_03;
        ChatRecord chat;
        ProbeRecord probe;
        ProbeReplyRecord probe_reply;
        GameStartRecord game_start;
        UnitCreatedRecord unit_created;
        UnitLinkRecord unit_link;
        UnitDamageRecord unit_damage;
        UnitKilledRecord unit_killed;
        WeaponFireRecord weapon_fire;
        ProjectileInterceptedRecord projectile_intercepted;
        FeatureEventRecord feature_event;
        CobStartRecord cob_start;
        UnitStateFlagsRecord unit_state_flags;
        BuilderLinkRecord builder_link;
        SoundRecord sound;
        UnitTransferRecord unit_transfer;
        LoadedRecord loaded;
        ResourceGiveRecord resource_give;
        PlayerValueRequestRecord player_value_request;
        PlayerValueReplyRecord player_value_reply;
        PauseSpeedRecord pause_speed;
        UnitDefHandshakeRecord unit_def_handshake;
        RejectRecord reject;
        DisconnectNoticeRecord disconnect_notice;
        ResendRequestRecord resend_request;
        StartPositionRecord start_position;
        StartPositionAckRecord start_position_ack;
        PlayerInfoRecord player_info;
        MachineGroupRequestRecord machine_group_request;
        MachineGroupReplyRecord machine_group_reply;
        AllianceRecord alliance;
        PlayerTeamRecord player_team;
        Unused25Record unused_25;
        SlotTableRecord slot_table;
        IntegrityNoticeRecord integrity_notice;
        EconomyRecord economy;
        EconomyReplyRecord economy_reply;
        LoadProgressRecord load_progress;
        UnitStateRecord unit_state;
    };

    /// Creates an empty record of type 0 holding a zeroed ping.
    AnyRecord() noexcept : ping{} {}
};

/// Decodes one complete record of any type.
///
/// @param bytes Record start, type byte first.
/// @param size Record size; must equal its wire length.
/// @param[out] out Tagged record; a 0x2c body aliases bytes. Written only on success.
/// @return ok or the first decode error; zero_length_record for a type without a record.
[[nodiscard]] WireError
decode_any_record(const uint8_t* bytes, std::size_t size, AnyRecord* out) noexcept;
/// Encodes a record of any type.
///
/// A 0x2c writes its 7-byte header with the total length and copies body_size body bytes.
///
/// @param in Tagged record to encode.
/// @param[out] out Destination buffer.
/// @param capacity Bytes available at out.
/// @param[out] written Bytes written, when not null.
/// @return ok; bad_argument when out is null or a 0x2c is longer than 0xffff or lacks its body;
///         buffer_too_small; invalid_type for a type without a record.
[[nodiscard]] WireError encode_any_record(
    const AnyRecord& in, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept;

} // namespace oa::netgame
