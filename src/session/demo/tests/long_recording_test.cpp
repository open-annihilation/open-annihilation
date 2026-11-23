// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A recording longer than a stream finished in one piece may be, made here
// packet by packet rather than by a battle: as a battle of 20,000 units
// records, every packet a tick base and one unit state, two players taking
// turns. Cut part way through a packet, as a crash or a full disk cuts it,
// it finishes into a replay that keeps every whole packet before the cut,
// and reads back packet for packet and record for record to the last of
// them. Whole, the recorder hack's layout finishes into a `.tad` of the same
// bytes. Both play: the playback counts every frame, delivers each at its
// tick, and goes back to an earlier frame and on to a later one.

#include "oa/session/demo.hpp"
#include "oa/session/demo/recording.hpp"
#include "oa/session/demo/replay_record.hpp"

#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/recording_reader.hpp"
#include "oa/formats/tad.hpp"
#include "oa/netgame/network.hpp"
#include "oa/netgame/records.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace tad = oa::formats::tad;
namespace oademo = oa::formats::oademo;
using namespace oa::session::demo;

/// Unit states a packet holds after its tick base: a unit's elided 0x2c record.
constexpr std::size_t state_body_bytes = 800;
/// Packets a tick: half each player's.
constexpr uint32_t packets_per_tick = 64;
/// The tick the recording starts at.
constexpr uint32_t first_tick = 1;

// Two players, as a game of the engine records them, with a unit table of three types.
RecordingSetup two_players() {
    RecordingSetup setup{};
    setup.max_units = 10000;
    setup.map_name = "Long Map";
    setup.recorder_text = "Program 1.0";
    setup.date_text = "2026-11-23 08:00";
    for (std::size_t index = 0; index < 2; ++index) {
        RecordingPlayer player{};
        player.player_id = static_cast<uint32_t>(0x100 * (index + 1));
        player.name = "player" + std::to_string(index + 1);
        player.info.side = static_cast<uint8_t>(index);
        player.info.color = static_cast<uint8_t>(index);
        player.info.max_units = setup.max_units;
        player.info.version_major = 3;
        player.info.version_minor = 1;
        player.team = 5;
        setup.players.push_back(std::move(player));
    }
    for (uint32_t type = 1; type <= 3; ++type) {
        oa::netgame::UnitDefHandshakeRecord check{};
        check.subtype = static_cast<uint8_t>(oa::netgame::HandshakeSubtype::def_checksum);
        check.key = 0x1000 + type;
        check.value = 0x2000 + type;
        std::array<uint8_t, tad::layout::unit_check_record_bytes> bytes{};
        OA_CHECK(
            oa::netgame::encode_record(check, bytes.data(), bytes.size(), nullptr) ==
            oa::netgame::WireError::ok
        );
        setup.unit_checks.push_back(bytes);
    }
    return setup;
}

// The tick packet `index` belongs to.
uint32_t tick_of(uint64_t index) {
    return first_tick + static_cast<uint32_t>(index / packets_per_tick);
}

// Packet `index` of the recording, as its chunk: length, delay, sender, then the plain marker, a
// tick base and one elided unit state whose body changes a little from tick to tick, as a
// battle's do.
void packet_chunk(uint64_t index, std::vector<uint8_t>& chunk) {
    const uint32_t tick = tick_of(index);
    const auto unit = static_cast<uint32_t>(index % packets_per_tick);
    const std::size_t full = tad::layout::unit_state_header_bytes + state_body_bytes;
    const std::size_t payload = 1 + 5 + 3 + state_body_bytes;
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
    put16(unit == 0 ? 33U : 0U); // the recorder's clock moves once a tick
    put8(unit % 2 == 0 ? 1U : 2U);
    put8(tad::payload_marker);
    put8(static_cast<uint8_t>(tad::RecordType::tick_base));
    put32(tick);
    put8(static_cast<uint8_t>(tad::RecordType::elided_unit_state));
    put16(static_cast<uint32_t>(full));
    uint32_t state = 0x9E3779B9U ^ (tick * 0x85EBCA6BU) ^ (unit * 0xC2B2AE35U);
    for (std::size_t byte = 0; byte < state_body_bytes; ++byte) {
        if (byte % 16 == 0) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
        }
        put8(byte % 16 == 0 ? state : static_cast<uint32_t>(byte * 7U + unit));
    }
}

// Writes a stream: the header, then `packets` whole packets, then the first `cut` bytes of the
// next one.
void write_stream(const fs::path& path, uint64_t packets, std::size_t cut) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto header = recording_header(two_players());
    OA_CHECK(header.ok());
    out.write(
        reinterpret_cast<const char*>(header.bytes.data()),
        static_cast<std::streamsize>(header.bytes.size())
    );
    std::vector<uint8_t> chunk;
    std::vector<uint8_t> batch;
    batch.reserve(std::size_t{1} << 20);
    for (uint64_t index = 0; index < packets; ++index) {
        packet_chunk(index, chunk);
        batch.insert(batch.end(), chunk.begin(), chunk.end());
        if (batch.size() >= (std::size_t{1} << 20)) {
            out.write(
                reinterpret_cast<const char*>(batch.data()),
                static_cast<std::streamsize>(batch.size())
            );
            batch.clear();
        }
    }
    packet_chunk(packets, chunk);
    batch.insert(batch.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(cut));
    out.write(
        reinterpret_cast<const char*>(batch.data()), static_cast<std::streamsize>(batch.size())
    );
    out.flush();
    OA_CHECK(static_cast<bool>(out));
}

// Enough packets that the stream passes what is finished in one piece.
uint64_t packets_past_the_limit() {
    std::vector<uint8_t> chunk;
    packet_chunk(0, chunk);
    return finish_whole_most_bytes / chunk.size() + 1000;
}

// A replay read from its file at offsets.
struct ReplayFile {
    std::ifstream in{};
};

bool read_replay_file(void* context, uint64_t offset, std::span<uint8_t> into) {
    auto& file = *static_cast<ReplayFile*>(context);
    file.in.clear();
    file.in.seekg(static_cast<std::streamoff>(offset));
    file.in.read(reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size()));
    return file.in.gcount() == static_cast<std::streamsize>(into.size());
}

// Reads the replay's recording back a piece at a time and compares it, packet for packet and
// record for record, with the packets the stream was made of.
void reads_back_packet_for_packet(const fs::path& replay, uint64_t packets) {
    ReplayFile file;
    file.in.open(replay, std::ios::binary);
    file.in.seekg(0, std::ios::end);
    const auto size = static_cast<uint64_t>(file.in.tellg());
    oademo::RecordingReader reader;
    OA_CHECK(reader.open(oademo::ArchiveSource{&file, read_replay_file, size}).empty());
    OA_CHECK(reader.meta().recording.packets == packets);
    // Bytes read and not yet compared.
    std::vector<uint8_t> pending;
    std::vector<uint8_t> room(std::size_t{1} << 20);
    const auto header = recording_header(two_players());
    std::vector<uint8_t> chunk;
    bool header_checked = false;
    bool same = true;
    uint64_t checked = 0;
    uint32_t last_tick = 0;
    for (;;) {
        std::size_t got = 0;
        OA_CHECK(reader.read(room, got).empty());
        if (got == 0)
            break;
        pending.insert(
            pending.end(), room.begin(), room.begin() + static_cast<std::ptrdiff_t>(got)
        );
        std::size_t used = 0;
        if (!header_checked) {
            if (pending.size() < header.bytes.size())
                continue;
            same = same && std::equal(header.bytes.begin(), header.bytes.end(), pending.begin());
            used = header.bytes.size();
            header_checked = true;
        }
        // Each whole packet as it comes in; the one the cut split is never there.
        while (checked < packets) {
            packet_chunk(checked, chunk);
            if (pending.size() - used < chunk.size())
                break;
            const std::span<const uint8_t> stored(pending.data() + used, chunk.size());
            same = same && std::equal(chunk.begin(), chunk.end(), stored.begin());
            // Record for record: the tick base, then the unit state it gives the tick.
            const auto split = tad::split_records(
                stored.subspan(tad::layout::chunk_length_bytes + tad::layout::packet_fixed_bytes)
            );
            same = same && !split.error && split.records.size() == 2 &&
                   split.records[0].type == static_cast<uint8_t>(tad::RecordType::tick_base) &&
                   split.records[1].tick == tick_of(checked);
            last_tick = tick_of(checked);
            used += chunk.size();
            ++checked;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(used));
    }
    OA_CHECK(same);
    OA_CHECK(header_checked && pending.empty());
    OA_CHECK(checked == packets);
    OA_CHECK(last_tick == tick_of(packets - 1));
    OA_CHECK(reader.finished());
}

// Plays a recording's frames: every one is counted, each comes at its tick, and the delivery
// goes back to an earlier frame and on to a later one.
void plays_every_frame(const fs::path& recording, uint64_t packets) {
    DemoPlayback playback;
    std::string error;
    OA_CHECK(demo_open(&playback, recording, &error));
    OA_CHECK(error.empty());
    OA_CHECK(playback.frame_count == packets);
    OA_CHECK(playback.last_due_tick == tick_of(packets - 1));
    // The recorder's clock moved at each tick's first packet.
    OA_CHECK(playback.duration_ms == 33U * (tick_of(packets - 1) - first_tick + 1U));
    OA_CHECK(playback.stats.frames == packets && playback.stats.bad_packets == 0);
    auto transport = demo_transport(&playback);
    std::vector<uint8_t> buffer(std::size_t{64} << 10);
    const auto receive = [&](uint32_t tick) {
        playback.tick = tick;
        uint32_t from = 0;
        uint32_t to = 0;
        auto size = static_cast<uint32_t>(buffer.size());
        return transport.receive(transport.context, &from, &to, buffer.data(), &size) ==
               oa::netgame::transport_result::ok;
    };
    // Nothing is due before the first tick; the first tick's frames come at it.
    OA_CHECK(!receive(first_tick - 1));
    uint64_t delivered = 0;
    while (receive(first_tick))
        ++delivered;
    OA_CHECK(delivered == packets_per_tick && playback.cursor == packets_per_tick);
    // Half way, then back to the third frame, then on to the end.
    const auto half = static_cast<std::size_t>(packets / 2);
    OA_CHECK(demo_deliver_from(&playback, half));
    OA_CHECK(!receive(tick_of(half) - 1) && receive(tick_of(half)));
    OA_CHECK(demo_deliver_from(&playback, 2));
    OA_CHECK(receive(first_tick) && playback.cursor == 3);
    while (receive(std::numeric_limits<uint32_t>::max()))
        ++delivered;
    OA_CHECK(demo_exhausted(playback) && playback.cursor == playback.frame_count);
    OA_CHECK(demo_read_failure(playback).empty());
    OA_CHECK(playback.stats.frames_delivered == delivered + 2);
}

void a_long_stream_cut_part_way_finishes_and_plays() {
    const fs::path scratch = oa::test::make_scratch_directory("demo-long");
    const uint64_t packets = packets_past_the_limit();
    std::vector<uint8_t> chunk;
    packet_chunk(packets, chunk);
    const fs::path stream = scratch / "long.oademo.part";
    write_stream(stream, packets, chunk.size() / 2);
    std::error_code failure;
    OA_CHECK(fs::file_size(stream, failure) > finish_whole_most_bytes);

    ReplayFacts facts;
    facts.recorded_at = "2026-11-23T08:00:00Z";
    facts.map_name = "Long Map";
    ReplayEnd end;
    end.ended = "cut";
    end.after_crash = true;
    const fs::path replay = scratch / "Recordings" / "long.oademo";
    const FinishResult finished =
        finish_as_replay(stream, replay_begin(two_players(), facts), end, replay);
    OA_CHECK(finished.ok && finished.packets == packets && finished.placed == replay);
    // It stops at the tick of the last whole packet, and says so.
    OA_CHECK(finished.last_tick == tick_of(packets - 1));
    const uint32_t seconds = finished.last_tick / recorded_ticks_per_second;
    std::string clock = std::to_string(seconds / 60) + ":";
    clock += (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
    OA_CHECK(
        recording_stop_line(finished, "the file cannot be written") ==
        "the recording stopped at " + clock + " of the game (tick " +
            std::to_string(finished.last_tick) +
            "): the file cannot be written; the replay keeps the game up to there"
    );
    OA_CHECK(!fs::exists(stream, failure));
    OA_CHECK(!fs::exists(scratch / "long.oademo.part.writing", failure));
    reads_back_packet_for_packet(replay, packets);
    plays_every_frame(replay, packets);
    fs::remove(replay, failure);

    // The recorder hack's layout, whole: moved into place as it is, never read whole.
    const fs::path tad_stream = scratch / "long.tad.part";
    write_stream(tad_stream, packets, 0);
    const uint64_t tad_bytes = fs::file_size(tad_stream, failure);
    const fs::path tad_file = scratch / "Recordings" / "long.tad";
    const FinishResult moved = finish_as_tad(tad_stream, tad_file, true);
    OA_CHECK(moved.ok && moved.packets == packets);
    OA_CHECK(fs::file_size(tad_file, failure) == tad_bytes);
    plays_every_frame(tad_file, packets);
    fs::remove_all(scratch, failure);
}

} // namespace

int main() {
    a_long_stream_cut_part_way_finishes_and_plays();
    return oa::test::check_exit_status();
}
