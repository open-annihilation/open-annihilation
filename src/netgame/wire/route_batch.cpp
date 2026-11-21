// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/route_batch.hpp"

#include <cstdint>
#include <cstring>

namespace oa::netgame {
namespace {

constexpr std::size_t length_offset = 1;
constexpr std::size_t length_field_end = 3;
constexpr std::size_t part_tick_offset = 3;
constexpr std::size_t part_previous_offset = 7;
constexpr std::size_t part_index_offset = 11;
constexpr std::size_t part_flags_offset = 13;
constexpr std::size_t ack_owner_offset = 3;
constexpr std::size_t ack_applied_offset = 7;
constexpr std::size_t ack_highest_offset = 11;
constexpr std::size_t ack_count_offset = 15;
constexpr uint8_t part_known_flags = route_batch_part_last;
constexpr uint8_t ack_known_flags = route_batch_ack_last_held;

/// Returns the header a route batch record type cannot be shorter than.
///
/// @param type a route batch record type
/// @return its header's bytes
std::size_t header_bytes_of(uint8_t type) noexcept {
    return type == static_cast<uint8_t>(RouteBatchRecordType::part) ? route_batch_part_header_bytes
                                                                    : route_batch_ack_header_bytes;
}

/// Returns the bytes of an entry's map.
///
/// @param parts the parts the map covers
/// @return ceil(parts / 8)
std::size_t map_bytes(uint16_t parts) noexcept {
    return (static_cast<std::size_t>(parts) + 7u) / 8u;
}

} // namespace

WireError
route_batch_record_length(const uint8_t* bytes, std::size_t available, uint16_t* length) noexcept {
    if (bytes == nullptr || length == nullptr)
        return WireError::bad_argument;
    if (available == 0)
        return WireError::truncated;
    if (!is_route_batch_record_type(bytes[0]))
        return WireError::invalid_type;
    if (available < length_field_end)
        return WireError::truncated;
    const uint16_t declared = load_u16(bytes + length_offset);
    *length = declared;
    if (declared < header_bytes_of(bytes[0]) || declared > route_batch_record_max_bytes)
        return WireError::length_mismatch;
    if (declared > available)
        return WireError::truncated;
    return WireError::ok;
}

WireError decode_route_batch_part_header(
    const uint8_t* bytes, std::size_t size, RouteBatchPartHeader* out
) noexcept {
    if (out == nullptr)
        return WireError::bad_argument;
    uint16_t length = 0;
    const auto measured = route_batch_record_length(bytes, size, &length);
    if (measured != WireError::ok)
        return measured;
    if (bytes[0] != static_cast<uint8_t>(RouteBatchRecordType::part))
        return WireError::invalid_type;
    if (length != size)
        return WireError::length_mismatch;
    RouteBatchPartHeader header{};
    header.length = length;
    header.tick = load_u32(bytes + part_tick_offset);
    header.previous_tick = load_u32(bytes + part_previous_offset);
    header.part_index = load_u16(bytes + part_index_offset);
    const uint8_t flags = bytes[part_flags_offset];
    header.last = (flags & route_batch_part_last) != 0;
    if (header.tick == 0 || header.previous_tick >= header.tick ||
        header.part_index >= route_batch_max_parts || (flags & ~part_known_flags) != 0)
        return WireError::bad_argument;
    *out = header;
    return WireError::ok;
}

void route_batch_part_begin(
    BitWriter* writer, uint32_t tick, uint32_t previous_tick, uint16_t part_index
) noexcept {
    bit_writer_write(writer, static_cast<uint8_t>(RouteBatchRecordType::part), 8);
    bit_writer_write(writer, 0, 16);
    bit_writer_write(writer, tick, 32);
    bit_writer_write(writer, previous_tick, 32);
    bit_writer_write(writer, part_index, 16);
    bit_writer_write(writer, 0, 8);
}

WireError route_batch_part_finish(BitWriter* writer, bool last, uint16_t* length) noexcept {
    if (writer == nullptr)
        return WireError::bad_argument;
    if (writer->error != WireError::ok)
        return writer->error;
    const auto bytes = bit_writer_byte_length(writer);
    if (bytes > route_batch_record_max_bytes)
        return WireError::overflow;
    bit_writer_patch_byte(writer, length_offset, static_cast<uint8_t>(bytes));
    bit_writer_patch_byte(writer, length_offset + 1, static_cast<uint8_t>(bytes >> 8));
    bit_writer_patch_byte(writer, part_flags_offset, last ? route_batch_part_last : uint8_t{0});
    if (length != nullptr)
        *length = static_cast<uint16_t>(bytes);
    return writer->error;
}

std::size_t route_batch_ack_bytes(const RouteBatchAck& ack) noexcept {
    std::size_t bytes = route_batch_ack_header_bytes;
    const std::size_t entries = ack.entry_count < route_batch_ack_max_entries
                                    ? ack.entry_count
                                    : route_batch_ack_max_entries;
    for (std::size_t i = 0; i < entries; ++i)
        bytes += route_batch_ack_entry_header_bytes + map_bytes(ack.entries[i].parts);
    return bytes;
}

WireError encode_route_batch_ack(
    const RouteBatchAck& ack, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept {
    if (out == nullptr || ack.entry_count > route_batch_ack_max_entries)
        return WireError::bad_argument;
    for (std::size_t i = 0; i < ack.entry_count; ++i)
        if (ack.entries[i].parts > route_batch_max_parts)
            return WireError::bad_argument;
    const std::size_t bytes = route_batch_ack_bytes(ack);
    if (bytes > route_batch_record_max_bytes)
        return WireError::overflow;
    if (bytes > capacity)
        return WireError::buffer_too_small;
    out[0] = static_cast<uint8_t>(RouteBatchRecordType::acknowledgement);
    store_u16(out + length_offset, static_cast<uint16_t>(bytes));
    store_u32(out + ack_owner_offset, ack.owner_id);
    store_u32(out + ack_applied_offset, ack.applied_through);
    store_u32(out + ack_highest_offset, ack.highest_held);
    out[ack_count_offset] = ack.entry_count;
    std::size_t at = route_batch_ack_header_bytes;
    for (std::size_t i = 0; i < ack.entry_count; ++i) {
        const auto& entry = ack.entries[i];
        store_u32(out + at, entry.tick);
        store_u16(out + at + 4, entry.parts);
        out[at + 6] = entry.last_held ? route_batch_ack_last_held : uint8_t{0};
        at += route_batch_ack_entry_header_bytes;
        const auto held_bytes = map_bytes(entry.parts);
        std::memcpy(out + at, entry.held.data(), held_bytes);
        // Bits past the parts mapped are written clear.
        if ((entry.parts % 8u) != 0)
            out[at + held_bytes - 1] =
                static_cast<uint8_t>(out[at + held_bytes - 1] & ((1u << (entry.parts % 8u)) - 1u));
        at += held_bytes;
    }
    if (written != nullptr)
        *written = bytes;
    return WireError::ok;
}

WireError
decode_route_batch_ack(const uint8_t* bytes, std::size_t size, RouteBatchAck* out) noexcept {
    if (out == nullptr)
        return WireError::bad_argument;
    uint16_t length = 0;
    const auto measured = route_batch_record_length(bytes, size, &length);
    if (measured != WireError::ok)
        return measured;
    if (bytes[0] != static_cast<uint8_t>(RouteBatchRecordType::acknowledgement))
        return WireError::invalid_type;
    if (length != size)
        return WireError::length_mismatch;
    RouteBatchAck ack{};
    ack.owner_id = load_u32(bytes + ack_owner_offset);
    ack.applied_through = load_u32(bytes + ack_applied_offset);
    ack.highest_held = load_u32(bytes + ack_highest_offset);
    ack.entry_count = bytes[ack_count_offset];
    if (ack.entry_count > route_batch_ack_max_entries)
        return WireError::bad_argument;
    std::size_t at = route_batch_ack_header_bytes;
    for (std::size_t i = 0; i < ack.entry_count; ++i) {
        if (size - at < route_batch_ack_entry_header_bytes)
            return WireError::length_mismatch;
        auto& entry = ack.entries[i];
        entry.tick = load_u32(bytes + at);
        entry.parts = load_u16(bytes + at + 4);
        const uint8_t flags = bytes[at + 6];
        if (entry.parts > route_batch_max_parts || (flags & ~ack_known_flags) != 0)
            return WireError::bad_argument;
        entry.last_held = (flags & route_batch_ack_last_held) != 0;
        at += route_batch_ack_entry_header_bytes;
        const auto held_bytes = map_bytes(entry.parts);
        if (size - at < held_bytes)
            return WireError::length_mismatch;
        std::memcpy(entry.held.data(), bytes + at, held_bytes);
        // Bits past the parts mapped name no part.
        if ((entry.parts % 8u) != 0)
            entry.held[held_bytes - 1] = static_cast<uint8_t>(
                entry.held[held_bytes - 1] & ((1u << (entry.parts % 8u)) - 1u)
            );
        at += held_bytes;
    }
    if (at != size)
        return WireError::length_mismatch;
    *out = ack;
    return WireError::ok;
}

} // namespace oa::netgame
