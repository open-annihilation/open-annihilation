// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Route batch records: in an Enhanced Mode match between Open Annihilation
// machines, every route, traffic-head and blocked-bit change, and every
// aircraft goal change, of one player's tick travels as one complete batch
// of parts in place of that tick's 0x2c, and each machine acknowledges the
// batches it holds so that a lost part is sent again. These bytes are not
// 3.1c record types: a 3.1c machine, or a match with Enhanced Mode off,
// never sends or reads them.
//
//   part (0xe4)
//     +0  u8  type
//     +1  u16 length, the type byte included
//     +3  u32 tick: the sender's tick whose changes the batch holds
//     +7  u32 previous tick: the tick of the sender's batch before it, 0 for its first
//     +11 u16 part index, from 0
//     +13 u8  flags: route_batch_part_last on the batch's last part
//     +14 bit stream, as the 0x2c list: for each change a u16 unit index,
//         the def index and the unit's delta; the u16 terminator 0xffff;
//         in the last part only, a flag bit and, when set, the full unit
//         record of the unit that rides on the tick. Padded to a byte.
//
//   acknowledgement (0xe5), from each player of the machine that holds them
//     +0  u8  type
//     +1  u16 length, the type byte included
//     +3  u32 owner: the player id whose batches it acknowledges
//     +7  u32 applied through: the tick of the last batch applied, 0 for none
//     +11 u32 highest held: the highest tick of which any part is here
//     +15 u8  entry count, at most route_batch_ack_max_entries
//     +16 entries, oldest first, of batches held but not yet applied:
//           u32 tick, u16 parts mapped, u8 flags (route_batch_ack_last_held
//           when the last part is among them), then a map of
//           ceil(parts / 8) bytes, bit i of byte i / 8 set for each part held

#include "oa/netgame/bitstream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oa::netgame {

/// The route batch records' type bytes.
enum class RouteBatchRecordType : uint8_t {
    part = 0xe4,            ///< one part of a player's tick of route changes
    acknowledgement = 0xe5, ///< which of a player's batches a machine holds
};

/// The first route batch record type.
inline constexpr uint8_t first_route_batch_record_type = 0xe4;
/// The last route batch record type.
inline constexpr uint8_t last_route_batch_record_type = 0xe5;

/// Bytes of a part's header, before its bit stream.
inline constexpr std::size_t route_batch_part_header_bytes = 14;
/// Bytes of an acknowledgement's header, before its entries.
inline constexpr std::size_t route_batch_ack_header_bytes = 16;
/// Bytes of an acknowledgement entry before its map.
inline constexpr std::size_t route_batch_ack_entry_header_bytes = 7;
/// The longest route batch record: one send buffer, so a record always fits a packet.
inline constexpr std::size_t route_batch_record_max_bytes = 0x42a;
/// The packer closes a part and starts the next once the part reaches this many bytes.
inline constexpr std::size_t route_batch_part_soft_limit_bytes = 0x380;
/// The most parts one batch may have; part indices run below it.
inline constexpr uint16_t route_batch_max_parts = 1024;
/// The most batches one acknowledgement lists.
inline constexpr uint8_t route_batch_ack_max_entries = 8;
/// Bytes of the largest map an acknowledgement entry carries.
inline constexpr std::size_t route_batch_ack_map_bytes = route_batch_max_parts / 8;

/// Flag bit of a part: it is the last part of its batch.
inline constexpr uint8_t route_batch_part_last = 0x01;
/// Flag bit of an acknowledgement entry: the batch's last part is held, so its part count is final.
inline constexpr uint8_t route_batch_ack_last_held = 0x01;

/// Tells whether a byte is one of the route batch records 0xe4..0xe5.
///
/// @param type the byte
/// @return true for a part or an acknowledgement
[[nodiscard]] constexpr bool is_route_batch_record_type(uint8_t type) noexcept {
    return type >= first_route_batch_record_type && type <= last_route_batch_record_type;
}

/// The header of one part.
struct RouteBatchPartHeader {
    uint16_t length{};        ///< whole record, type byte included
    uint32_t tick{};          ///< the sender's tick whose changes the batch holds
    uint32_t previous_tick{}; ///< the tick of the sender's batch before it, 0 for its first
    uint16_t part_index{};    ///< from 0
    bool last{};              ///< the batch's last part
};

/// One batch an acknowledgement says is held but not yet applied.
struct RouteBatchAckEntry {
    uint32_t tick{};
    uint16_t parts{}; ///< parts the map covers: the batch's count when last_held
    bool last_held{}; ///< the batch's last part is held
    std::array<uint8_t, route_batch_ack_map_bytes> held{}; ///< bit i set for each part i held
};

/// What one machine holds of one player's batches.
struct RouteBatchAck {
    uint32_t owner_id{};        ///< the player id whose batches these are
    uint32_t applied_through{}; ///< the tick of the last batch applied there, 0 for none
    uint32_t highest_held{};    ///< the highest tick of which any part is there
    uint8_t entry_count{};      ///< at most route_batch_ack_max_entries
    std::array<RouteBatchAckEntry, route_batch_ack_max_entries> entries{};
};

/// Returns the wire length of the route batch record at bytes[0].
///
/// @param bytes record start
/// @param available bytes readable from bytes
/// @param[out] length the record's length, type byte included
/// @return ok; bad_argument for a null pointer; invalid_type for any other byte; truncated when the
///         length field is missing or the record runs past available; length_mismatch for a length
///         shorter than the type's header or longer than route_batch_record_max_bytes
[[nodiscard]] WireError
route_batch_record_length(const uint8_t* bytes, std::size_t available, uint16_t* length) noexcept;

/// Decodes and checks a part's header.
///
/// @param bytes record start
/// @param size the record's size; must equal its length field
/// @param[out] out the header; written only on success
/// @return ok; the errors of route_batch_record_length; invalid_type for an acknowledgement;
///         length_mismatch when size differs from the length; bad_argument for a tick of 0, a previous
///         tick not before the tick, a part index of route_batch_max_parts or more, or an unknown flag
[[nodiscard]] WireError decode_route_batch_part_header(
    const uint8_t* bytes, std::size_t size, RouteBatchPartHeader* out
) noexcept;

/// Starts a part: its type, a zero length and flags to patch later, its tick, previous tick and index.
///
/// @param[in,out] writer freshly initialised writer
/// @param tick the sender's tick
/// @param previous_tick the tick of the sender's batch before it, 0 for its first
/// @param part_index the part's index
void route_batch_part_begin(
    BitWriter* writer, uint32_t tick, uint32_t previous_tick, uint16_t part_index
) noexcept;

/// Ends a part: pads its bit stream to a byte and patches its length and flags.
///
/// @param[in,out] writer writer holding the part
/// @param last the part is its batch's last
/// @param[out] length the record's length, when not null
/// @return ok, the writer's error, or overflow beyond route_batch_record_max_bytes
[[nodiscard]] WireError
route_batch_part_finish(BitWriter* writer, bool last, uint16_t* length) noexcept;

/// Returns the bytes an acknowledgement takes.
///
/// @param ack the acknowledgement
/// @return its length, type byte included
[[nodiscard]] std::size_t route_batch_ack_bytes(const RouteBatchAck& ack) noexcept;

/// Encodes an acknowledgement.
///
/// @param ack the acknowledgement; its entries' maps are written ceil(parts / 8) bytes long
/// @param[out] out storage for the record
/// @param capacity bytes at out
/// @param[out] written the record's length, when not null
/// @return ok; bad_argument for a null out, more than route_batch_ack_max_entries entries or an
///         entry of more than route_batch_max_parts parts; buffer_too_small; overflow beyond
///         route_batch_record_max_bytes
[[nodiscard]] WireError encode_route_batch_ack(
    const RouteBatchAck& ack, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept;

/// Decodes an acknowledgement.
///
/// @param bytes record start
/// @param size the record's size; must equal its length field
/// @param[out] out the acknowledgement; written only on success
/// @return ok; the errors of route_batch_record_length; invalid_type for a part; length_mismatch when
///         size differs from the length or the entries do not fill it exactly; bad_argument for more
///         than route_batch_ack_max_entries entries or an entry of more than route_batch_max_parts
///         parts or an unknown flag
[[nodiscard]] WireError
decode_route_batch_ack(const uint8_t* bytes, std::size_t size, RouteBatchAck* out) noexcept;

} // namespace oa::netgame
