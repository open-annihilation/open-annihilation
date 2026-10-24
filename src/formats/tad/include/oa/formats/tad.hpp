// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// TA Demo recordings (.tad, .ted, .pro). The container is a sequence of chunks,
// each a little-endian u16 total length (length field included) and its data:
//
//   header          magic, version, player count, unit limit, map name
//   sector count    u32
//   sectors         u32 type + data, one chunk each
//   players         color, side, number, name; one chunk per player
//   player status   player number + that player's status datagram, one chunk each
//   unit checks     back-to-back 0x1a unit-check records (one chunk)
//   packets         u16 delay in ms, u8 sender, payload; until end of file
//
// Packet payloads are not wire datagrams: the checksum, masking and sequence
// dword are gone. A 0x03 payload holds game records directly; a 0x04 payload
// holds their fresh-dictionary LZ77 stream. The markers are the condenser's
// frame types: its receiver copies a type 3 body and unpacks a type 4 one.
// Unit-state (0x2c) records are usually stored without their tick (see
// RecordType). Status chunks, by contrast, hold datagrams exactly as sent.
//
// Packets are stored in the order they arrived. A match sends its frames
// without delivery guarantee, so a frame lost on the way leaves its sender's
// ticks missing and a late one is stored after the frames that overtook it.
// The same frame can also be stored twice, and a sender's frame can begin
// with the last tick of its frame before, holding the same unit state again.
//
// Recorders also send their peers packets of their own: a command typed in
// chat (".sharelos") as bare text, and short recorder messages whose second
// and third bytes are not a length. Each fills its packet.
//
// Open Annihilation adds a sector of type 0x4f414d31 (the bytes 1 M A O on
// disk) when a game was played with mutators. It holds the loadout as text
// and is absent otherwise, so a recording without it is unchanged. It adds
// a sector of type 0x4f414231 (the bytes 1 B A O on disk) when computer
// players ran brains. It holds those seats as text and is absent otherwise,
// so a recording without brains is unchanged. Readers that do not know a
// type skip it; this parser keeps every sector and continues.
//
// Only version 5 has been checked against recordings; other versions are rejected.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::tad {
namespace limit {
inline constexpr std::size_t input_bytes = 512U * 1024U * 1024U;
inline constexpr std::size_t sectors = 256;
inline constexpr std::size_t players = 10;
inline constexpr std::size_t packets = 16U * 1024U * 1024U;
inline constexpr std::size_t chunk_bytes = 0xffff;
inline constexpr std::size_t decoded_payload_bytes = 28000; // the condenser's receive storage
} // namespace limit

namespace layout {
inline constexpr std::size_t chunk_length_bytes = 2;
inline constexpr std::size_t magic_bytes = 8;
// magic + u16 version + u8 player count + u16 unit limit; the map name fills the rest.
inline constexpr std::size_t header_fixed_bytes = magic_bytes + 5;
inline constexpr std::size_t sector_count_bytes = 4;
inline constexpr std::size_t sector_type_bytes = 4;
inline constexpr std::size_t player_fixed_bytes = 3;
inline constexpr std::size_t status_fixed_bytes = 1;
inline constexpr std::size_t packet_fixed_bytes = 3;
inline constexpr std::size_t unit_check_record_bytes = 14;
// Tick-elided unit state: 0xfd + u16 length of the full 0x2c record + body.
inline constexpr std::size_t elided_tick_bytes = 4;
inline constexpr std::size_t unit_state_header_bytes = 7;
inline constexpr std::size_t unit_state_tick_offset = 3; // after the type byte and u16 length
} // namespace layout

inline constexpr std::string_view magic{"TA Demo\0", layout::magic_bytes};
inline constexpr uint16_t supported_version = 5;
inline constexpr uint8_t payload_marker = 0x03;
inline constexpr uint8_t compressed_payload_marker = 0x04;
inline constexpr uint8_t unit_check_type = 0x1a;

enum class SectorType : uint32_t {
    recorder_version = 3, // ASCII, not terminated
    recording_date = 4,   // ASCII, not terminated
    player_address = 6,   // one per player; masked text, not decoded here
    /// Loadout of a game played with mutators, as text. Open Annihilation's
    /// own type, the bytes 1 M A O on disk. Other readers skip it.
    mutators = 0x4f414d31,
    /// The brains that played a game's computer players, as text. Open
    /// Annihilation's own type, the bytes 1 B A O on disk. Other readers
    /// skip it.
    brains = 0x4f414231,
};

enum class RecordedSide : uint8_t { arm = 0, core = 1 };

// Record type bytes found in packet payloads. 0x02..0x2c are the game's own
// records; the others exist only in recordings.
enum class RecordType : uint8_t {
    unit_state = 0x2c,
    recorder_command = 0x2e,  // '.' of a recorder command's text, which fills its packet
    enemy_chat = 0xf9,        // length 73; not seen in the checked recordings
    replayer_server = 0xfa,   // length 1; not seen in the checked recordings
    recorder_message = 0xfb,  // 0xfb + u16 payload length + payload
    map_position = 0xfc,      // length 5
    elided_unit_state = 0xfd, // a 0x2c record minus its tick; advances the tick by one
    tick_base = 0xfe,         // 0xfe + u32 tick of the next unit-state slot
    empty_tick = 0xff,        // a tick with no unit state; advances the tick by one
};

inline constexpr uint8_t first_game_record_type = 0x02;
inline constexpr uint8_t last_game_record_type = 0x2c;

struct Sector {
    uint32_t type{};
    std::span<const uint8_t> data;
};

struct Player {
    uint8_t color{};
    uint8_t side{}; // RecordedSide for 0 and 1; other values kept as recorded.
    uint8_t number{};
    std::string name;
};

struct PlayerStatus {
    uint8_t number{};
    std::span<const uint8_t> datagram; // condenser type byte, checksum, masked body
};

struct Packet {
    std::size_t offset{}; // file offset of the chunk
    uint16_t delay_ms{};
    uint64_t time_ms{};               // sum of delays up to and including this packet
    uint8_t sender{};                 // player number
    std::span<const uint8_t> payload; // marker byte + records
};

// Spans view the buffer handed to parse(); they stay valid only while it does.
struct Demo {
    uint16_t version{};
    uint16_t max_units{};
    std::string map_name;
    std::vector<Sector> sectors;
    std::vector<Player> players;
    std::vector<PlayerStatus> statuses;
    std::span<const uint8_t> unit_checks;
    std::vector<Packet> packets;
};

enum class ErrorCode {
    none,
    input_limit,
    truncated,
    bad_magic,
    unsupported_version,
    malformed,
    count_limit,
    unknown_record,
};

struct Error {
    ErrorCode code{};
    std::size_t offset{};
    std::string message;
};

struct ParseResult {
    std::optional<Demo> demo;
    std::optional<Error> error;

    /// Tells whether the recording parsed.
    [[nodiscard]] bool ok() const noexcept { return demo.has_value(); }
};

/// Parses a version 5 recording.
///
/// @param bytes Whole file; the result's spans view it and stay valid only while it does.
/// @return The demo, or an error with its file offset (input_limit, truncated, bad_magic,
///         unsupported_version, malformed, count_limit).
[[nodiscard]] ParseResult parse(std::span<const uint8_t> bytes);

struct WriteResult {
    std::vector<uint8_t> bytes;
    std::optional<Error> error;

    /// Tells whether the container was written.
    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

/// Rebuilds the container; parse(write(demo)) reproduces demo.
///
/// @param demo Recording to write.
/// @return The file bytes, or a malformed error (with no bytes) when a field does not fit its chunk, the
///         player and status counts differ or exceed 10, there are more than 256 sectors, or the
///         unit-check data is not whole records.
[[nodiscard]] WriteResult write(const Demo& demo);

/// Finds the first sector of a type.
///
/// @param demo Parsed recording.
/// @param type Sector type.
/// @return The sector, or null.
[[nodiscard]] const Sector* find_sector(const Demo& demo, SectorType type) noexcept;

/// Returns a text sector's contents up to the first NUL, if any.
///
/// @param sector Sector holding ASCII text.
/// @return A view into the sector's data.
[[nodiscard]] std::string_view sector_text(const Sector& sector) noexcept;

/// Splits the unit-check chunk into its 14-byte 0x1a records.
///
/// @param demo Parsed recording.
/// @return Views of every whole record; trailing bytes are ignored.
[[nodiscard]] std::vector<std::span<const uint8_t>> unit_check_records(const Demo& demo);

struct Record {
    uint8_t type{};
    std::span<const uint8_t> bytes;
    // For tick_base, elided_unit_state and empty_tick records, the tick they
    // stand for; absent when no tick_base preceded them in the packet. For a
    // stored unit_state record, the tick it carries.
    std::optional<uint32_t> tick;
};

struct RecordSplit {
    std::vector<Record> records;
    bool marker_ok{};           // payload started with payload_marker
    std::optional<Error> error; // offset is within the payload
};

struct DecodedPayload {
    std::vector<uint8_t> bytes; // 0x03 marker followed by uncompressed records
    std::optional<Error> error;

    /// Tells whether the payload decoded.
    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

/// Expands a 0x04 packet before record splitting, keeping the receive buffer's 28000-byte bound.
///
/// @param payload Packet payload, marker byte first.
/// @return A 0x03 payload unchanged, or the 0x03 marker followed by the unpacked records; an error for an
///         empty payload, an unknown marker or a malformed compressed stream.
[[nodiscard]] DecodedPayload decode_payload(std::span<const uint8_t> payload);

/// Returns the length of the record at the start of bytes, including its type byte.
///
/// Game types use the game's own length table; 0x2c and the recording-only
/// variable records carry their own length.
///
/// @param bytes Record start.
/// @return The length, or 0 when the type is unknown or the length does not fit bytes.
[[nodiscard]] std::size_t record_length(std::span<const uint8_t> bytes) noexcept;

/// Cuts an uncompressed 0x03 payload into records and assigns ticks to the tick records.
///
/// A payload that starts with a recorder command, or with a recorder message
/// whose length does not fit the payload, is one record of the whole payload.
/// Otherwise stops at the first byte that cannot start a record or a record
/// that runs past the payload, keeping what came before. Rejects other markers
/// without parsing their bytes.
///
/// @param payload Packet payload, marker byte first.
/// @return The records, whether the marker was 0x03, and the error that stopped the split, if any.
[[nodiscard]] RecordSplit split_records(std::span<const uint8_t> payload);

/// Rebuilds the 0x2c wire record an elided_unit_state record stands for, with its tick restored.
///
/// @param record Record from split_records.
/// @return The full record, or empty when the record is not a well-formed elided unit state or has no tick.
[[nodiscard]] std::vector<uint8_t> expand_unit_state(const Record& record);
} // namespace oa::formats::tad
