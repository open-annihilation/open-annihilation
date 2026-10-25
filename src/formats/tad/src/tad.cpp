// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/tad.hpp"
#include "oa/formats/sqsh.hpp"
#include "oa/base/bytes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>

namespace oa::formats::tad {
namespace {

using base::bytes::load_le16;
using base::bytes::load_le32;

using Bytes = std::span<const uint8_t>;

void append16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8U));
}

void append32(std::vector<uint8_t>& out, uint32_t value) {
    append16(out, static_cast<uint16_t>(value));
    append16(out, static_cast<uint16_t>(value >> 16U));
}

std::string text_of(Bytes bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

Error make_error(ErrorCode code, std::size_t offset, std::string message) {
    return Error{code, offset, std::move(message)};
}

// Wire lengths of game record types 0x00..0x2c, type byte included; 0 marks a
// type the game never sends. 0x2c carries its own length (u16 at +1).
// clang-format off
constexpr std::array<uint8_t, 0x2d> game_record_lengths{
    0,   0,  13, 3,  0,  65, 1,  1,  1,  23, 7,  9,  11, 36, 14, 6,  // 0x00
    22,  4,  5,  18, 24, 1,  17, 2,  2,  3,  14, 6,  5,  9,  2,  5,  // 0x10
    186, 10, 6,  14, 6,  5,  41, 17, 58, 3,  2,  0,  3,              // 0x20
};
// clang-format on

// Length of a recording-only record, 0 when bytes does not start one.
std::size_t recorder_record_length(Bytes bytes) noexcept {
    switch (static_cast<RecordType>(bytes[0])) {
    case RecordType::enemy_chat:
        return 73;
    case RecordType::replayer_server:
    case RecordType::empty_tick:
        return 1;
    case RecordType::map_position:
    case RecordType::tick_base:
        return 5;
    case RecordType::recorder_message:
        return bytes.size() < 3 ? 0 : 3U + load_le16(bytes.data() + 1);
    case RecordType::elided_unit_state: {
        if (bytes.size() < 3)
            return 0;
        const std::size_t full = load_le16(bytes.data() + 1);
        return full < layout::unit_state_header_bytes ? 0 : full - layout::elided_tick_bytes;
    }
    default:
        return 0;
    }
}

/// Returns the length of a recorder record that fills its packet when it comes first.
///
/// @param bytes the packet's records, from its first one
/// @return the whole of bytes for a recorder command, or for a recorder message whose
///         length does not fit them; 0 for anything else
std::size_t packet_filling_length(Bytes bytes) noexcept {
    switch (static_cast<RecordType>(bytes[0])) {
    case RecordType::recorder_command:
        return bytes.size();
    case RecordType::recorder_message:
        return record_length(bytes) == 0 ? bytes.size() : 0;
    default:
        return 0;
    }
}

class Reader {
  public:

    explicit Reader(Bytes bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }

    // Next chunk's data, or an error naming what was being read.
    std::optional<Bytes> chunk(std::string_view what, std::optional<Error>& error) {
        if (bytes_.size() - offset_ < layout::chunk_length_bytes) {
            error = make_error(
                ErrorCode::truncated, offset_, std::string(what) + ": missing chunk length"
            );
            return std::nullopt;
        }
        const std::size_t length = load_le16(bytes_.data() + offset_);
        if (length < layout::chunk_length_bytes) {
            error = make_error(
                ErrorCode::malformed, offset_, std::string(what) + ": chunk length below 2"
            );
            return std::nullopt;
        }
        if (length > bytes_.size() - offset_) {
            error = make_error(
                ErrorCode::truncated, offset_, std::string(what) + ": chunk runs past end of file"
            );
            return std::nullopt;
        }
        const Bytes data = bytes_.subspan(
            offset_ + layout::chunk_length_bytes, length - layout::chunk_length_bytes
        );
        offset_ += length;
        return data;
    }

  private:

    Bytes bytes_;
    std::size_t offset_ = 0;
};

bool append_chunk(std::vector<uint8_t>& out, Bytes head, Bytes data) {
    const std::size_t length = layout::chunk_length_bytes + head.size() + data.size();
    if (length > limit::chunk_bytes)
        return false;
    append16(out, static_cast<uint16_t>(length));
    out.insert(out.end(), head.begin(), head.end());
    out.insert(out.end(), data.begin(), data.end());
    return true;
}

Bytes as_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

} // namespace

ParseResult parse(Bytes bytes) {
    ParseResult result;
    if (bytes.size() > limit::input_bytes) {
        result.error = make_error(ErrorCode::input_limit, 0, "input exceeds the demo size limit");
        return result;
    }
    Reader reader(bytes);
    std::optional<Error>& error = result.error;
    Demo demo;

    const std::size_t header_at = reader.offset();
    const auto header = reader.chunk("header", error);
    if (!header)
        return result;
    if (header->size() < layout::header_fixed_bytes ||
        text_of(header->first(layout::magic_bytes)) != magic) {
        error = make_error(ErrorCode::bad_magic, header_at, "not a TA Demo recording");
        return result;
    }
    demo.version = load_le16((*header).data() + layout::magic_bytes);
    if (demo.version != supported_version) {
        error = make_error(
            ErrorCode::unsupported_version,
            header_at,
            "demo version " + std::to_string(demo.version) + " is not supported"
        );
        return result;
    }
    const std::size_t player_count = (*header)[layout::magic_bytes + 2];
    demo.max_units = load_le16((*header).data() + layout::magic_bytes + 3);
    demo.map_name = text_of(header->subspan(layout::header_fixed_bytes));
    if (player_count > limit::players) {
        error = make_error(ErrorCode::count_limit, header_at, "player count exceeds limit");
        return result;
    }

    const std::size_t count_at = reader.offset();
    const auto count = reader.chunk("sector count", error);
    if (!count)
        return result;
    if (count->size() != layout::sector_count_bytes) {
        error = make_error(ErrorCode::malformed, count_at, "sector count chunk is not 4 bytes");
        return result;
    }
    const uint32_t sector_count = load_le32((*count).data() + 0);
    if (sector_count > limit::sectors) {
        error = make_error(ErrorCode::count_limit, count_at, "sector count exceeds limit");
        return result;
    }
    for (uint32_t index = 0; index < sector_count; ++index) {
        const std::size_t at = reader.offset();
        const auto sector = reader.chunk("sector", error);
        if (!sector)
            return result;
        if (sector->size() < layout::sector_type_bytes) {
            error = make_error(ErrorCode::malformed, at, "sector shorter than its type field");
            return result;
        }
        demo.sectors.push_back(
            Sector{load_le32((*sector).data() + 0), sector->subspan(layout::sector_type_bytes)}
        );
    }

    for (std::size_t index = 0; index < player_count; ++index) {
        const std::size_t at = reader.offset();
        const auto player = reader.chunk("player", error);
        if (!player)
            return result;
        if (player->size() < layout::player_fixed_bytes) {
            error = make_error(ErrorCode::malformed, at, "player chunk shorter than 3 bytes");
            return result;
        }
        demo.players.push_back(
            Player{
                (*player)[0],
                (*player)[1],
                (*player)[2],
                text_of(player->subspan(layout::player_fixed_bytes))
            }
        );
    }

    for (std::size_t index = 0; index < player_count; ++index) {
        const std::size_t at = reader.offset();
        const auto status = reader.chunk("player status", error);
        if (!status)
            return result;
        if (status->size() < layout::status_fixed_bytes) {
            error = make_error(ErrorCode::malformed, at, "empty player status chunk");
            return result;
        }
        demo.statuses.push_back(
            PlayerStatus{(*status)[0], status->subspan(layout::status_fixed_bytes)}
        );
    }

    const std::size_t checks_at = reader.offset();
    const auto checks = reader.chunk("unit checks", error);
    if (!checks)
        return result;
    if (checks->size() % layout::unit_check_record_bytes != 0) {
        error = make_error(
            ErrorCode::malformed, checks_at, "unit-check chunk is not a whole number of records"
        );
        return result;
    }
    for (std::size_t at = 0; at < checks->size(); at += layout::unit_check_record_bytes) {
        if ((*checks)[at] != unit_check_type) {
            error = make_error(
                ErrorCode::malformed,
                checks_at + layout::chunk_length_bytes + at,
                "unit-check chunk holds a non-0x1a record"
            );
            return result;
        }
    }
    demo.unit_checks = *checks;

    uint64_t time_ms = 0;
    while (!reader.at_end()) {
        const std::size_t at = reader.offset();
        if (demo.packets.size() == limit::packets) {
            error = make_error(ErrorCode::count_limit, at, "packet count exceeds limit");
            return result;
        }
        const auto packet = reader.chunk("packet", error);
        if (!packet)
            return result;
        if (packet->size() < layout::packet_fixed_bytes) {
            error = make_error(ErrorCode::malformed, at, "packet chunk shorter than 3 bytes");
            return result;
        }
        const uint16_t delay = load_le16((*packet).data() + 0);
        time_ms += delay;
        demo.packets.push_back(
            Packet{at, delay, time_ms, (*packet)[2], packet->subspan(layout::packet_fixed_bytes)}
        );
    }

    result.demo = std::move(demo);
    return result;
}

WriteResult write(const Demo& demo) {
    WriteResult result;
    auto fail = [&result](std::string message) {
        result.bytes.clear();
        result.error = make_error(ErrorCode::malformed, 0, std::move(message));
        return result;
    };
    if (demo.players.size() > limit::players || demo.statuses.size() != demo.players.size())
        return fail("player and status counts must match and stay within the limit");
    if (demo.sectors.size() > limit::sectors)
        return fail("too many sectors");
    auto& out = result.bytes;

    std::vector<uint8_t> head(magic.begin(), magic.end());
    append16(head, demo.version);
    head.push_back(static_cast<uint8_t>(demo.players.size()));
    append16(head, demo.max_units);
    if (!append_chunk(out, head, as_bytes(demo.map_name)))
        return fail("map name does not fit the header chunk");

    head.clear();
    append32(head, static_cast<uint32_t>(demo.sectors.size()));
    append_chunk(out, head, {});
    for (const Sector& sector : demo.sectors) {
        head.clear();
        append32(head, sector.type);
        if (!append_chunk(out, head, sector.data))
            return fail("sector does not fit its chunk");
    }
    for (const Player& player : demo.players) {
        const std::array<uint8_t, layout::player_fixed_bytes> fixed{
            player.color, player.side, player.number
        };
        if (!append_chunk(out, fixed, as_bytes(player.name)))
            return fail("player name does not fit its chunk");
    }
    for (const PlayerStatus& status : demo.statuses) {
        const std::array<uint8_t, layout::status_fixed_bytes> fixed{status.number};
        if (!append_chunk(out, fixed, status.datagram))
            return fail("status datagram does not fit its chunk");
    }
    if (demo.unit_checks.size() % layout::unit_check_record_bytes != 0 ||
        !append_chunk(out, {}, demo.unit_checks))
        return fail("unit-check data is not a chunk of whole records");
    for (const Packet& packet : demo.packets) {
        head.clear();
        append16(head, packet.delay_ms);
        head.push_back(packet.sender);
        if (!append_chunk(out, head, packet.payload))
            return fail("packet does not fit its chunk");
    }
    return result;
}

const Sector* find_sector(const Demo& demo, SectorType type) noexcept {
    for (const Sector& sector : demo.sectors) {
        if (sector.type == static_cast<uint32_t>(type))
            return &sector;
    }
    return nullptr;
}

std::string_view sector_text(const Sector& sector) noexcept {
    std::string_view text{reinterpret_cast<const char*>(sector.data.data()), sector.data.size()};
    return text.substr(0, text.find('\0'));
}

std::array<uint8_t, limits_sector_bytes> encode_limits_sector(const RecordedLimits& limits) {
    std::array<uint8_t, limits_sector_bytes> out{};
    const auto put = [](uint8_t* at, uint32_t value) {
        at[0] = static_cast<uint8_t>(value);
        at[1] = static_cast<uint8_t>(value >> 8);
        at[2] = static_cast<uint8_t>(value >> 16);
        at[3] = static_cast<uint8_t>(value >> 24);
    };
    put(out.data(), limits.weapon_types);
    put(out.data() + 4, limits.sides);
    return out;
}

std::optional<RecordedLimits> decode_limits_sector(const Sector& sector) {
    if (sector.data.size() != limits_sector_bytes)
        return std::nullopt;
    RecordedLimits limits;
    limits.weapon_types = base::bytes::load_le32(sector.data.data());
    limits.sides = base::bytes::load_le32(sector.data.data() + 4);
    if (limits.weapon_types < recorded_base_weapon_types ||
        limits.weapon_types > recorded_highest_weapon_types || limits.sides < recorded_base_sides ||
        limits.sides > recorded_highest_sides)
        return std::nullopt;
    return limits;
}

std::vector<std::span<const uint8_t>> unit_check_records(const Demo& demo) {
    std::vector<std::span<const uint8_t>> records;
    for (std::size_t at = 0; at + layout::unit_check_record_bytes <= demo.unit_checks.size();
         at += layout::unit_check_record_bytes)
        records.push_back(demo.unit_checks.subspan(at, layout::unit_check_record_bytes));
    return records;
}

std::size_t record_length(Bytes bytes, RecordForms forms) noexcept {
    if (bytes.empty())
        return 0;
    const uint8_t type = bytes[0];
    std::size_t length = 0;
    if (type == last_game_record_type) {
        length = bytes.size() < 3 ? 0 : load_le16(bytes.data() + 1);
        if (length < layout::unit_state_header_bytes)
            length = 0;
    } else if (type < game_record_lengths.size())
        length = game_record_lengths[type];
    else if (
        forms == RecordForms::wide && type >= first_wide_recorded_type &&
        static_cast<std::size_t>(type - first_wide_recorded_type) < wide_record_lengths.size()
    )
        length = wide_record_lengths[static_cast<std::size_t>(type - first_wide_recorded_type)];
    else
        length = recorder_record_length(bytes);
    return length == 0 || length > bytes.size() ? 0 : length;
}

DecodedPayload decode_payload(Bytes payload) {
    DecodedPayload decoded;
    if (payload.empty()) {
        decoded.error = make_error(ErrorCode::truncated, 0, "empty packet payload");
    } else if (payload[0] == payload_marker) {
        decoded.bytes.assign(payload.begin(), payload.end());
    } else if (payload[0] == compressed_payload_marker) {
        const auto records =
            formats::sqsh::decode_lz77(payload.subspan(1), limit::decoded_payload_bytes);
        if (records.ok()) {
            decoded.bytes.reserve(records.value->size() + 1);
            decoded.bytes.push_back(payload_marker);
            decoded.bytes.insert(decoded.bytes.end(), records.value->begin(), records.value->end());
        } else {
            decoded.error = make_error(
                ErrorCode::malformed, 1, std::string("compressed packet: ") + records.error.message
            );
        }
    } else {
        decoded.error = make_error(ErrorCode::malformed, 0, "unknown packet payload marker");
    }
    return decoded;
}

RecordSplit split_records(Bytes payload, RecordForms forms) {
    RecordSplit split;
    if (payload.empty()) {
        split.error = make_error(ErrorCode::truncated, 0, "empty payload");
        return split;
    }
    split.marker_ok = payload[0] == payload_marker;
    if (!split.marker_ok) {
        split.error = make_error(ErrorCode::malformed, 0, "packet payload is not decoded");
        return split;
    }
    std::optional<uint32_t> tick;
    std::size_t at = 1;
    while (at < payload.size()) {
        const Bytes rest = payload.subspan(at);
        std::size_t length = record_length(rest, forms);
        if (length == 0 && at == 1)
            length = packet_filling_length(rest);
        if (length == 0) {
            split.error =
                make_error(ErrorCode::unknown_record, at, "cannot size a record of this type here");
            return split;
        }
        Record record{rest[0], rest.first(length), std::nullopt};
        switch (static_cast<RecordType>(record.type)) {
        case RecordType::tick_base:
            tick = load_le32(rest.data() + 1);
            record.tick = tick;
            break;
        case RecordType::elided_unit_state:
        case RecordType::empty_tick:
            record.tick = tick;
            if (tick)
                ++*tick;
            break;
        case RecordType::unit_state:
            record.tick = load_le32(rest.data() + layout::unit_state_tick_offset);
            break;
        default:
            break;
        }
        split.records.push_back(record);
        at += length;
    }
    return split;
}

std::vector<uint8_t> expand_unit_state(const Record& record) {
    std::vector<uint8_t> out;
    if (record.type != static_cast<uint8_t>(RecordType::elided_unit_state) || !record.tick ||
        record.bytes.size() < 3)
        return out;
    const std::size_t full = load_le16(record.bytes.data() + 1);
    if (full != record.bytes.size() + layout::elided_tick_bytes)
        return out;
    // The full record: its type, length and tick, then the body the elided
    // record carries after its own type and length.
    out.resize(full);
    out[0] = last_game_record_type;
    for (std::size_t i = 0; i < 2; ++i)
        out[1 + i] = static_cast<uint8_t>(full >> (8U * i));
    for (std::size_t i = 0; i < layout::elided_tick_bytes; ++i)
        out[layout::unit_state_tick_offset + i] = static_cast<uint8_t>(*record.tick >> (8U * i));
    std::copy(
        record.bytes.begin() + layout::unit_state_tick_offset,
        record.bytes.end(),
        out.begin() + static_cast<std::ptrdiff_t>(layout::unit_state_header_bytes)
    );
    return out;
}

} // namespace oa::formats::tad
