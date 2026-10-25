// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Game-packet layer shared definitions: record types, the per-type length
// table and little-endian field access. 3.1c's table covers 0x02..0x2c and
// record_wire_length stays 3.1c's. OA records 0xe0..0xe2 travel only between
// OA machines when a profile raises weapon-types past 256. Everything here is
// bounded and reports failures through WireError; nothing allocates or throws.

#include "oa/base/bytes.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::netgame {

enum class WireError : uint8_t {
    ok = 0,
    truncated,                // input ends inside a field or record
    invalid_type,             // byte cannot head a record (<= 0x01 or >= 0x2d)
    zero_length_record,       // type with no table length (0x04, 0x2b) or a 0x2c of length 0
    length_mismatch,          // record size differs from the table / declared length
    buffer_too_small,         // caller output storage cannot hold the result
    overflow,                 // bounded internal storage exhausted
    unsupported_delta_layout, // 0x2c per-class unit delta without a supplied codec
    bad_argument,             // argument outside the documented domain
    unknown_peer,             // no peer slot for the sender and none reclaimable
};

/// Returns a short lower-case description of an error for logs.
///
/// @param error Error to describe.
/// @return A static string; "unknown" for a value outside the enum.
[[nodiscard]] const char* wire_error_name(WireError error) noexcept;

// Record type byte (first byte of every game packet).
enum class RecordType : uint8_t {
    ping = 0x02,
    unused_03 = 0x03, // in the length table; 3.1c neither sends nor handles it
    chat = 0x05,
    probe = 0x06,
    probe_reply = 0x07,
    game_start = 0x08,
    unit_created = 0x09,
    unit_link = 0x0a,
    unit_damage = 0x0b,
    unit_killed = 0x0c,
    weapon_fire = 0x0d,
    projectile_intercepted = 0x0e,
    feature_event = 0x0f,
    cob_start = 0x10,
    unit_state_flags = 0x11,
    builder_link = 0x12,
    sound = 0x13,
    unit_transfer = 0x14,
    loaded = 0x15,
    resource_give = 0x16,
    player_value_request = 0x17,
    player_value_reply = 0x18,
    pause_speed = 0x19,
    unit_def_handshake = 0x1a,
    reject = 0x1b,
    disconnect_notice = 0x1c,
    resend_request = 0x1d,
    start_position = 0x1e,
    start_position_ack = 0x1f,
    player_info = 0x20,
    machine_group_request = 0x21,
    machine_group_reply = 0x22,
    alliance = 0x23,
    player_team = 0x24,
    unused_25 = 0x25, // in the length table; 3.1c neither sends nor handles it
    slot_table = 0x26,
    integrity_notice = 0x27,
    economy = 0x28,
    economy_reply = 0x29,
    load_progress = 0x2a,
    unit_state = 0x2c,
};

inline constexpr std::size_t record_type_count = 0x2d;
inline constexpr uint8_t first_record_type = 0x02;
inline constexpr uint8_t last_record_type = 0x2c;

// Per-type record length including the type byte; 0 = no table entry.
// 0x2c lists only its 3-byte prefix: the real length is the u16 at +1.
inline constexpr uint16_t record_length_table[record_type_count] = {
    0, 0, 13, 3,  0, 65, 1, 1, 1, 23,  7,  9, 11, 36, 14, 6,  22, 4,  5, 18, 24, 1, 17,
    2, 2, 3,  14, 6, 5,  9, 2, 5, 186, 10, 6, 14, 6,  5,  41, 17, 58, 3, 2,  0,  3,
};
static_assert(record_length_table[0x20] == 186 && record_length_table[0x28] == 58);

// Phase admission bits per record type, tested by the packet pump:
// 1 = lobby/other states, 2 = loading, 4 = in game.
inline constexpr uint8_t record_phase_table[record_type_count] = {
    0, 0, 7, 7, 0, 7, 7, 7, 7, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 7, 4, 6, 4,
    7, 7, 7, 1, 7, 7, 0, 6, 6, 7, 1, 1, 7, 7, 1, 7, 7, 7, 7, 7, 0, 4,
};

inline constexpr uint8_t unit_state_prefix_bytes = 3; // type + u16 length
inline constexpr uint8_t unit_state_header_bytes = 7; // + u32 sender tick

/// OA weapon records. These bytes are not 3.1c record types and never enter
/// record_length_table or record_wire_length.
enum class WideRecordType : uint8_t {
    weapon_fire = 0xe0,
    projectile_intercepted = 0xe1,
    feature_event = 0xe2,
};

inline constexpr uint8_t first_wide_record_type = 0xe0;
inline constexpr uint8_t last_wide_record_type = 0xe2;
inline constexpr std::size_t wide_record_type_count = 3;

/// Length of each wide record, including the type byte, in type order.
inline constexpr uint16_t wide_record_length_table[wide_record_type_count] = {37, 15, 8};

/// Tells whether a byte is in the record type range 0x02..0x2c.
[[nodiscard]] constexpr bool is_record_type(uint8_t type) noexcept {
    return type >= first_record_type && type <= last_record_type;
}

/// Tells whether a byte is one of the OA wide weapon records 0xe0..0xe2.
[[nodiscard]] constexpr bool is_wide_record_type(uint8_t type) noexcept {
    return type >= first_wide_record_type && type <= last_wide_record_type;
}

/// Returns the fixed table length of a 3.1c or wide record type, or 0.
///
/// Wide types take wide_record_length_table. Types 0x02..0x2c take
/// record_length_table, including a 0x2c prefix of 3. Anything else is 0.
///
/// @param type Record type byte.
/// @return Table length including the type byte, or 0 when the byte has none.
[[nodiscard]] constexpr uint16_t record_table_length(uint8_t type) noexcept {
    if (is_wide_record_type(type))
        return wide_record_length_table[static_cast<std::size_t>(type - first_wide_record_type)];
    if (is_record_type(type))
        return record_length_table[type];
    return 0;
}

/// Returns the wire length of the record that starts at bytes[0], as the receive splitter reads it.
///
/// The 0x2c length comes from its own u16 at +1; every other type takes its
/// record_length_table entry. A zero length never advances past the record,
/// so callers must stop on zero_length_record.
///
/// @param bytes Record start.
/// @param available Bytes readable from bytes.
/// @param[out] length Record length including the type byte; written even when it is zero.
/// @return ok; bad_argument for a null pointer; truncated when the type byte or the 0x2c prefix is
///         missing; invalid_type; zero_length_record for a type with no length (0x04, 0x2b, or a 0x2c of
///         length 0).
[[nodiscard]] WireError
record_wire_length(const uint8_t* bytes, std::size_t available, uint16_t* length) noexcept;

/// Returns the wire length of a wide weapon record that starts at bytes[0].
///
/// Only 0xe0, 0xe1 and 0xe2 are valid. The length always comes from
/// wide_record_length_table. record_wire_length still refuses these bytes.
///
/// @param bytes Record start.
/// @param available Bytes readable from bytes.
/// @param[out] length Record length including the type byte.
/// @return ok; bad_argument for a null pointer; truncated when the type byte is
///         missing or the buffer is shorter than the table length; invalid_type
///         for any byte outside 0xe0..0xe2.
[[nodiscard]] WireError
wide_record_wire_length(const uint8_t* bytes, std::size_t available, uint16_t* length) noexcept;

// Explicit little-endian field access.

/// Reads a little-endian u16 at p.
[[nodiscard]] constexpr uint16_t load_u16(const uint8_t* p) noexcept {
    return base::bytes::load_le16(p);
}

/// Reads a little-endian u32 at p.
[[nodiscard]] constexpr uint32_t load_u32(const uint8_t* p) noexcept {
    return base::bytes::load_le32(p);
}

/// Writes v as a little-endian u16 at p.
constexpr void store_u16(uint8_t* p, uint16_t v) noexcept {
    base::bytes::store_le16(p, v);
}

/// Writes v as a little-endian u32 at p.
constexpr void store_u32(uint8_t* p, uint32_t v) noexcept {
    base::bytes::store_le32(p, v);
}

} // namespace oa::netgame
