// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A recording longer than the 512 MiB a recording or a replay may hold when
// it is read whole, made packet by packet rather than by a battle, and the
// `.tad` and `.oademo` files that hold it, written a piece at a time. Each
// packet is a tick base and one unit state whose bytes do not repeat, so the
// replay's zstd frame is as long as the recording and the file passes 512
// MiB too. The tests of every reader that takes a replay a piece at a time
// share it. A test that includes this links oa-formats-oademo,
// oa-formats-tad, oa-base-zstd and oa-base-sha256.

#pragma once

#include "oa/base/sha256.hpp"
#include "oa/base/zstd/zstd.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace oa::test {

/// The most bytes a whole read takes: a longer recording, or a longer replay,
/// is read a piece at a time.
inline constexpr uint64_t whole_read_most_bytes = oa::formats::tad::limit::input_bytes;

/// Bytes of one unit state's body in the made recording's packets.
inline constexpr std::size_t made_state_body_bytes = 4000;

/// Packets of the made recording a tick: the recorder's clock moves 33 ms on each first.
inline constexpr uint64_t made_packets_per_tick = 8;

/// Makes the chunk of a recording's packet: its length, delay, sender and payload.
///
/// @param index the packet's place in the recording, from 0
/// @param[out] chunk the chunk
using PacketMaker = std::function<void(uint64_t index, std::vector<uint8_t>& chunk)>;

/// A recording made one packet at a time.
struct MadeRecording {
    /// The part before the packets: the header, the sectors, the players,
    /// their statuses and the unit checks.
    std::vector<uint8_t> head{};
    /// How many packets follow it.
    uint64_t packets{};
    /// Makes each packet's chunk.
    PacketMaker packet{};
};

/// A file written from a made recording.
struct MadeFile {
    /// The file's size, in bytes.
    uint64_t bytes{};
    /// The recording's size, in bytes.
    uint64_t recording_bytes{};
    /// The recording's SHA-256, lower-case hexadecimal.
    std::string recording_sha256{};
};

/// Returns the tick a made packet belongs to.
///
/// @param index the packet's place, from 0
/// @return the tick, from 1
inline uint32_t made_tick_of(uint64_t index) {
    return 1U + static_cast<uint32_t>(index / made_packets_per_tick);
}

/// Makes a unit-state packet of the made recording: the plain marker, a tick
/// base and one elided unit state, from player 1 or 2 by turns.
///
/// @param index the packet's place, from 0
/// @param[out] chunk the chunk
inline void made_state_packet(uint64_t index, std::vector<uint8_t>& chunk) {
    namespace tad = oa::formats::tad;
    const std::size_t full = tad::layout::unit_state_header_bytes + made_state_body_bytes;
    const std::size_t payload = 1 + 5 + 3 + made_state_body_bytes;
    const std::size_t length =
        tad::layout::chunk_length_bytes + tad::layout::packet_fixed_bytes + payload;
    chunk.resize(length);
    std::size_t at = 0;
    const auto put8 = [&](uint32_t value) { chunk[at++] = static_cast<uint8_t>(value); };
    const auto put16 = [&](uint32_t value) {
        put8(value);
        put8(value >> 8U);
    };
    const auto put32 = [&](uint32_t value) {
        put16(value);
        put16(value >> 16U);
    };
    put16(static_cast<uint32_t>(length));
    put16(index % made_packets_per_tick == 0 ? 33U : 0U);
    put8(index % 2 == 0 ? 1U : 2U);
    put8(tad::payload_marker);
    put8(static_cast<uint8_t>(tad::RecordType::tick_base));
    put32(made_tick_of(index));
    put8(static_cast<uint8_t>(tad::RecordType::elided_unit_state));
    put16(static_cast<uint32_t>(full));
    // A body no compressor shortens, so the replay grows with its recording.
    uint64_t state = 0x9E3779B97F4A7C15ULL ^ (index * 0xC2B2AE3D27D4EB4FULL);
    for (std::size_t byte = 0; byte < made_state_body_bytes; byte += 8) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        for (std::size_t part = 0; part < 8 && byte + part < made_state_body_bytes; ++part)
            put8(static_cast<uint32_t>(state >> (8U * part)));
    }
}

/// Returns the part before a made recording's packets: version 5, the map
/// "Long Map", two players with their statuses, the recorder's line and
/// date, and three unit checks.
///
/// @return the bytes
inline std::vector<uint8_t> made_recording_head() {
    namespace tad = oa::formats::tad;
    const std::string recorder_text = "Program 1.0";
    const std::string date_text = "2026-11-23 08:00";
    const std::vector<uint8_t> recorder(recorder_text.begin(), recorder_text.end());
    const std::vector<uint8_t> date(date_text.begin(), date_text.end());
    const std::vector<uint8_t> status{0x04};
    std::vector<uint8_t> checks;
    for (uint8_t type = 1; type <= 3; ++type) {
        std::array<uint8_t, tad::layout::unit_check_record_bytes> record{};
        record[0] = tad::unit_check_type;
        record[1] = type;
        record[5] = static_cast<uint8_t>(0x10 + type);
        record[9] = static_cast<uint8_t>(0x20 + type);
        checks.insert(checks.end(), record.begin(), record.end());
    }
    tad::Demo demo;
    demo.version = tad::supported_version;
    demo.max_units = 10000;
    demo.map_name = "Long Map";
    demo.sectors = {
        {static_cast<uint32_t>(tad::SectorType::recorder_version), recorder},
        {static_cast<uint32_t>(tad::SectorType::recording_date), date},
    };
    demo.players = {{0, 0, 1, "player1"}, {1, 1, 2, "player2"}};
    demo.statuses = {{1, status}, {2, status}};
    demo.unit_checks = checks;
    const tad::WriteResult written = tad::write(demo);
    if (!written.ok())
        throw std::runtime_error("the made recording's head does not write");
    return written.bytes;
}

/// Returns a recording of unit-state packets just past `whole_read_most_bytes`.
///
/// @return the recording, its packets made as they are asked for
inline MadeRecording recording_past_whole_reads() {
    MadeRecording made;
    made.head = made_recording_head();
    std::vector<uint8_t> chunk;
    made_state_packet(0, chunk);
    made.packets = (whole_read_most_bytes - made.head.size()) / chunk.size() + 4096;
    made.packet = made_state_packet;
    return made;
}

/// Hands a made recording's bytes on in order, a megabyte or so at a time.
///
/// @param made the recording
/// @param take called with each piece, in order
inline void for_each_piece(
    const MadeRecording& made, const std::function<void(std::span<const uint8_t>)>& take
) {
    take(made.head);
    std::vector<uint8_t> chunk;
    std::vector<uint8_t> batch;
    batch.reserve(std::size_t{1} << 20);
    for (uint64_t index = 0; index < made.packets; ++index) {
        made.packet(index, chunk);
        batch.insert(batch.end(), chunk.begin(), chunk.end());
        if (batch.size() >= (std::size_t{1} << 20)) {
            take(batch);
            batch.clear();
        }
    }
    if (!batch.empty())
        take(batch);
}

/// Writes a made recording as a `.tad` file.
///
/// @param made the recording
/// @param file the file to write
/// @return its size and the recording's digest
inline MadeFile write_made_tad(const MadeRecording& made, const std::filesystem::path& file) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    MadeFile written;
    oa::base::sha256::Hasher hasher;
    for_each_piece(made, [&](std::span<const uint8_t> piece) {
        oa::base::sha256::update(hasher, piece);
        out.write(
            reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size())
        );
        written.recording_bytes += piece.size();
    });
    out.flush();
    if (!out)
        throw std::runtime_error("the made recording's file did not write");
    written.bytes = written.recording_bytes;
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::finish(hasher));
    written.recording_sha256.assign(hex.begin(), hex.end());
    return written;
}

/// Writes a made recording as a `.oademo` replay: the recording one zstd
/// frame at `level`, made a piece at a time into `frame_file` and then stored.
///
/// The replay's recording identity and packet count are set from the
/// recording; the rest of its description, its manifest and its timeline
/// are `replay`'s.
///
/// @param made the recording
/// @param replay the description, the manifest and the timeline
/// @param file the replay to write
/// @param frame_file a scratch file for the frame, removed afterwards
/// @param level the zstd level
/// @return the replay's size, and the recording's size and digest
inline MadeFile write_made_replay(
    const MadeRecording& made,
    oa::formats::oademo::Replay replay,
    const std::filesystem::path& file,
    const std::filesystem::path& frame_file,
    int level = 1
) {
    namespace zstd = oa::base::zstd;
    namespace oademo = oa::formats::oademo;
    MadeFile written;
    for_each_piece(made, [&](std::span<const uint8_t> piece) {
        written.recording_bytes += piece.size();
    });

    struct FrameFile {
        std::ofstream out{};
        uint64_t bytes{};
    } frame;

    frame.out.open(frame_file, std::ios::binary | std::ios::trunc);
    std::vector<uint8_t> room(std::size_t{1} << 20);
    zstd::FrameWriter writer;
    const zstd::FrameSink sink{&frame, [](void* context, std::span<const uint8_t> bytes) {
                                   auto& to = *static_cast<FrameFile*>(context);
                                   to.out.write(
                                       reinterpret_cast<const char*>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size())
                                   );
                                   to.bytes += bytes.size();
                                   return static_cast<bool>(to.out);
                               }};
    if (zstd::frame_writer_begin(writer, level, written.recording_bytes, room, sink) !=
        zstd::Error::none)
        throw std::runtime_error("the made recording's frame does not begin");
    oa::base::sha256::Hasher hasher;
    for_each_piece(made, [&](std::span<const uint8_t> piece) {
        oa::base::sha256::update(hasher, piece);
        if (zstd::frame_writer_write(writer, piece) != zstd::Error::none)
            throw std::runtime_error("the made recording's frame does not write");
    });
    if (zstd::frame_writer_end(writer) != zstd::Error::none)
        throw std::runtime_error("the made recording's frame does not end");
    zstd::frame_writer_free(writer);
    frame.out.close();
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::finish(hasher));
    written.recording_sha256.assign(hex.begin(), hex.end());

    replay.recording.clear();
    replay.meta.recording.sha256 = written.recording_sha256;
    replay.meta.recording.bytes = written.recording_bytes;
    replay.meta.recording.packets = made.packets;
    std::ifstream frame_in(frame_file, std::ios::binary);
    std::fstream out(file, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    const oademo::FrameSource source{
        &frame_in,
        [](void* context, std::span<uint8_t> into) {
            auto& in = *static_cast<std::ifstream*>(context);
            in.read(
                reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size())
            );
            return in.gcount() == static_cast<std::streamsize>(into.size());
        },
        frame.bytes
    };
    const oademo::ArchiveOutput output{
        &out, [](void* context, uint64_t offset, std::span<const uint8_t> bytes) {
            auto& to = *static_cast<std::fstream*>(context);
            to.seekp(static_cast<std::streamoff>(offset));
            to.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
            return static_cast<bool>(to);
        }
    };
    if (!oademo::write_framed(replay, source, output).empty())
        throw std::runtime_error("the made replay does not write");
    out.flush();
    out.seekp(0, std::ios::end);
    written.bytes = static_cast<uint64_t>(out.tellp());
    out.close();
    frame_in.close();
    std::error_code removed;
    std::filesystem::remove(frame_file, removed);
    return written;
}

} // namespace oa::test
