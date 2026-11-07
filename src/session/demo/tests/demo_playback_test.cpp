// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Demo playback without game assets: frame rebuilding and sequencing from a
// hand-made recording, the recording's recognition by its magic, the
// tick-gated transport, the player table from the recorded lobby blocks, the
// unit-table check, the replay's verdict, and a round trip in which one tiny
// match records its own datagrams, stored as the recorder stores them, as a
// demo that a second match then replays.

#include "oa/session/demo.hpp"
#include "oa/session/demo/recording.hpp"
#include "oa/session/demo/recording_stream.hpp"
#include "oa/session/demo/replay_record.hpp"
#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/zip/writer.hpp"
#include "oa/netgame/unit_state.hpp"

#include "oa/data/defs/unit_records.hpp"
#include "oa/netgame/match/launch.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/network.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "oa/test/match_services.hpp"

using namespace oa;
using namespace oa::netgame;
using namespace oa::netgame::match;
using namespace oa::session::demo;

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
        }                                                                                          \
    } while (0)

constexpr uint16_t kUnitsPerPlayer = 4;
constexpr int32_t kFixedOne = 1 << 16;
constexpr auto checksum = static_cast<uint8_t>(HandshakeSubtype::def_checksum);
constexpr auto verdict_subtype = static_cast<uint8_t>(HandshakeSubtype::verdict);

void append16(std::vector<uint8_t>* out, uint16_t value) {
    out->push_back(static_cast<uint8_t>(value));
    out->push_back(static_cast<uint8_t>(value >> 8));
}

void append32(std::vector<uint8_t>* out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out->push_back(static_cast<uint8_t>(value >> shift));
}

// A recording with the given packets: payload storage stays alive in the
// returned demo's owner.
struct Recording {
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<uint8_t> unit_checks;
    std::vector<std::vector<uint8_t>> status_datagrams; // demo.statuses order
    formats::tad::Demo demo;

    // One 0x1a check record, as the unit-check chunk keeps them.
    void announce(uint8_t subtype, uint32_t key, uint32_t value) {
        UnitDefHandshakeRecord record{};
        record.subtype = subtype;
        record.key = key;
        record.value = value;
        std::array<uint8_t, record_length_table[0x1a]> bytes{};
        std::size_t written = 0;
        CHECK(encode_record(record, bytes.data(), bytes.size(), &written) == WireError::ok);
        const std::size_t at = unit_checks.size();
        unit_checks.resize(at + written);
        std::copy(
            bytes.begin(),
            bytes.begin() + static_cast<std::ptrdiff_t>(written),
            unit_checks.begin() + static_cast<std::ptrdiff_t>(at)
        );
    }

    void add(uint8_t sender, uint16_t delay_ms, std::vector<uint8_t> payload) {
        payloads.push_back(std::move(payload));
        formats::tad::Packet packet;
        packet.delay_ms = delay_ms;
        packet.sender = sender;
        demo.packets.push_back(packet);
    }

    std::vector<uint8_t> bytes() {
        uint64_t time = 0;
        for (std::size_t i = 0; i < demo.packets.size(); ++i) {
            time += demo.packets[i].delay_ms;
            demo.packets[i].time_ms = time;
            demo.packets[i].payload = payloads[i];
        }
        demo.unit_checks = unit_checks;
        for (std::size_t i = 0; i < demo.statuses.size(); ++i)
            demo.statuses[i].datagram = status_datagrams[i];
        const auto written = formats::tad::write(demo);
        if (!written.ok())
            std::fprintf(stderr, "formats::tad::write: %s\n", written.error->message.c_str());
        CHECK(written.ok());
        return written.bytes;
    }
};

// A status datagram as sent: sequence dword, then the 0x20 player info record.
std::vector<uint8_t> status_datagram(const PlayerSetupInfo& info) {
    std::vector<uint8_t> frame(frame_header_bytes, 0xff);
    frame.push_back(static_cast<uint8_t>(RecordType::player_info));
    const auto* block = reinterpret_cast<const uint8_t*>(&info);
    frame.insert(frame.end(), block, block + sizeof info);
    return oa::netgame::network::encode_frame(frame, false);
}

// Recorded ids descend with the player number, so unit ranges follow ids;
// all ten players' ids are usable.
constexpr uint32_t recorded_id(std::size_t player) {
    return 0xa000u - static_cast<uint32_t>(player) * 0x1000u;
}

constexpr uint8_t recorded_color(std::size_t player) {
    return static_cast<uint8_t>(player + 3);
}

// Host options: commander mode 1, mapping and both line-of-sight bits 6.
constexpr uint16_t recorded_host_options = 0x0e00;

// definitions: unit definitions the recording announces (keys 1..n).
// statuses: whether each player's status datagram carries its lobby block.
Recording make_recording(
    uint16_t max_units, std::size_t players, uint32_t definitions = 1, bool statuses = true
) {
    Recording recording;
    for (uint32_t key = 1; key <= definitions; ++key)
        recording.announce(checksum, key, 0);
    recording.demo.version = formats::tad::supported_version;
    recording.demo.max_units = max_units;
    recording.demo.map_name = "test";
    for (std::size_t i = 0; i < players; ++i) {
        formats::tad::Player player;
        player.color = static_cast<uint8_t>(i + 1);
        player.side = static_cast<uint8_t>(i & 1);
        player.number = static_cast<uint8_t>(i + 1);
        player.name = "player" + std::to_string(i + 1);
        recording.demo.players.push_back(player);
        formats::tad::PlayerStatus status;
        status.number = player.number;
        recording.demo.statuses.push_back(status);
        PlayerSetupInfo info{};
        info.player_id = recorded_id(i);
        info.side = player.side;
        info.color = recorded_color(i);
        info.role = i == 0 ? 1 : 0;
        info.options = i == 0 ? recorded_host_options : 0;
        recording.status_datagrams.push_back(
            statuses ? status_datagram(info) : std::vector<uint8_t>{0x04}
        );
    }
    return recording;
}

std::vector<uint8_t> probe_payload() {
    return {formats::tad::payload_marker, static_cast<uint8_t>(RecordType::probe)};
}

// 0xfe tick base then one elided unit state: the seven header bytes, an
// empty unit list, the full-record flag and a 2-bit empty full record.
std::vector<uint8_t> elided_state_payload(uint32_t tick, bool with_base) {
    std::vector<uint8_t> payload{formats::tad::payload_marker};
    if (with_base) {
        payload.push_back(static_cast<uint8_t>(formats::tad::RecordType::tick_base));
        append32(&payload, tick);
    }
    const std::vector<uint8_t> body{0xff, 0xff, 0x01};
    payload.push_back(static_cast<uint8_t>(formats::tad::RecordType::elided_unit_state));
    append16(&payload, static_cast<uint16_t>(unit_state_header_bytes + body.size()));
    payload.insert(payload.end(), body.begin(), body.end());
    payload.push_back(static_cast<uint8_t>(formats::tad::RecordType::empty_tick));
    return payload;
}

void frames_rebuild_broadcast_sequences() {
    auto recording = make_recording(kUnitsPerPlayer, 2);
    recording.add(1, 0, probe_payload());
    recording.add(
        1,
        40,
        {formats::tad::payload_marker,
         static_cast<uint8_t>(formats::tad::RecordType::empty_tick),
         static_cast<uint8_t>(formats::tad::RecordType::empty_tick)}
    );
    recording.add(2, 10, probe_payload());
    recording.add(1, 1000, elided_state_payload(5, true));
    recording.add(2, 5, elided_state_payload(9, false));
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, recording.bytes(), &error));
    CHECK(error.empty());
    CHECK(playback.players.size() == 2 && playback.players[1].from_status);
    CHECK(
        playback.players[1].info.side == 1 && playback.players[1].info.color == recorded_color(1)
    );
    CHECK(playback.recorded_definitions == 1 && playback.unit_def_bits == 2);
    CHECK(playback.recorded_verdicts == 0 && demo_unit_sync(playback) == nullptr);
    CHECK(playback.stats.frames == 3 && playback.stats.empty_packets == 2);
    CHECK(playback.stats.recorder_records == 1 && playback.stats.untimed_unit_states == 4);
    CHECK(playback.stats.empty_ticks == 1);
    CHECK(playback.frames.size() == 3);
    if (playback.frames.size() != 3)
        return;
    CHECK(
        playback.frames[0].sender == 1 && playback.frames[0].time_ms == 0 &&
        playback.frames[0].due_tick == 0
    );
    CHECK(
        playback.frames[1].sender == 2 && playback.frames[1].time_ms == 50 &&
        playback.frames[1].due_tick == 0
    );
    CHECK(
        playback.frames[2].sender == 1 && playback.frames[2].time_ms == 1050 &&
        playback.frames[2].due_tick == 6
    );
    const auto sequence_of = [](const DemoFrame& frame) {
        const auto decoded = oa::netgame::network::decode_frame(frame.datagram);
        return static_cast<int32_t>(load_u32(decoded.data()));
    };
    CHECK(sequence_of(playback.frames[0]) == first_broadcast_frame_sequence);
    CHECK(sequence_of(playback.frames[1]) == first_broadcast_frame_sequence);
    CHECK(sequence_of(playback.frames[2]) == next_frame_sequence(first_broadcast_frame_sequence));
    // The elided state at tick 5, then the empty tick 6 as a minimal 0x2c:
    // header, 16-bit terminator, flag bit and a 2-bit empty full record.
    constexpr std::size_t minimal_state_bytes = unit_state_header_bytes + 3;
    const auto state = oa::netgame::network::decode_frame(playback.frames[2].datagram);
    CHECK(state.size() == frame_header_bytes + unit_state_header_bytes + 3 + minimal_state_bytes);
    CHECK(state[frame_header_bytes] == static_cast<uint8_t>(RecordType::unit_state));
    CHECK(load_u32(state.data() + frame_header_bytes + unit_state_prefix_bytes) == 5);
    const auto* empty = state.data() + frame_header_bytes + unit_state_header_bytes + 3;
    CHECK(empty[0] == static_cast<uint8_t>(RecordType::unit_state));
    CHECK(load_u16(empty + 1) == minimal_state_bytes);
    CHECK(load_u32(empty + unit_state_prefix_bytes) == 6);
    CHECK(empty[unit_state_header_bytes] == 0xff && empty[unit_state_header_bytes + 1] == 0xff);
    CHECK(empty[unit_state_header_bytes + 2] == 0x01); // full-record flag set, definition 0
    CHECK(demo_duration_ms(playback) == 1055);
}

void compressed_packets_rebuild_without_spurious_records() {
    auto recording = make_recording(kUnitsPerPlayer, 1);
    // 884, packet at file offset 13538: the short LZ77 stream expands to
    // exactly one 58-byte 0x28 game record, with no wire sequence dword.
    recording.add(
        1,
        0,
        {0x04,
         0x9c,
         0x28,
         0x00,
         0x28,
         0x00,
         0xce,
         0x00,
         0x2e,
         0x00,
         0x7a,
         0x44,
         0x39,
         0x02,
         0xfe,
         0x44,
         0x00,
         0x00}
    );
    recording.add(1, 10, {0x04, 0x00}); // truncated LZ77 stream
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, recording.bytes(), &error));
    CHECK(playback.stats.frames == 1 && playback.stats.bad_packets == 1);
    CHECK(playback.stats.empty_packets == 1 && playback.frames.size() == 1);
    if (playback.frames.size() != 1)
        return;
    const auto frame = oa::netgame::network::decode_frame(playback.frames[0].datagram);
    CHECK(frame.size() == frame_header_bytes + 58);
    CHECK(frame[frame_header_bytes] == 0x28 && frame.back() == 0x44);
}

// 904, packet at file offset 19232: behind a chat record the recorder kept
// the sender's empty unit states whole, ticks 869..875 included. The frame
// is due at the last of them, as one with elided states would be.
void stored_unit_states_time_their_frame() {
    auto recording = make_recording(kUnitsPerPlayer, 1);
    std::vector<uint8_t> payload{formats::tad::payload_marker};
    constexpr uint32_t first_tick = 869;
    constexpr uint32_t states = 7;
    for (uint32_t tick = first_tick; tick < first_tick + states; ++tick) {
        const std::array<uint8_t, 11> state{
            0x2c,
            0x0b,
            0x00,
            static_cast<uint8_t>(tick),
            static_cast<uint8_t>(tick >> 8),
            0x00,
            0x00,
            0xff,
            0xff,
            0x01,
            0x00
        };
        payload.insert(payload.end(), state.begin(), state.end());
    }
    recording.add(1, 0, payload);
    recording.add(1, 40, probe_payload());
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, recording.bytes(), &error));
    CHECK(playback.frames.size() == 2 && playback.stats.untimed_unit_states == 0);
    if (playback.frames.size() != 2)
        return;
    CHECK(playback.frames[0].due_tick == first_tick + states - 1);
    CHECK(playback.frames[1].due_tick == first_tick + states - 1);
    const auto frame = oa::netgame::network::decode_frame(playback.frames[0].datagram);
    CHECK(frame.size() == frame_header_bytes + payload.size() - 1);
    CHECK(std::equal(payload.begin() + 1, payload.end(), frame.begin() + frame_header_bytes));
}

std::span<const uint8_t> text_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// A recording is recognised by the magic that opens its header chunk, even
// when it then fails to load; other bytes are not.
void recordings_are_recognised_by_their_magic() {
    auto recording = make_recording(kUnitsPerPlayer, 1);
    recording.add(1, 0, probe_payload());
    const auto whole = recording.bytes();
    CHECK(demo_recognised(whole));
    CHECK(!demo_recognised({}));
    CHECK(!demo_recognised(text_bytes("A director script names the recording it shows.\n")));
    // Bytes that follow no format: every one the one before plus 37.
    std::vector<uint8_t> patterned(64);
    for (std::size_t i = 0; i < patterned.size(); ++i)
        patterned[i] = static_cast<uint8_t>(11 + 37 * i);
    CHECK(!demo_recognised(patterned));
    // The magic without the chunk length before it, and a magic cut short.
    CHECK(!demo_recognised(text_bytes(formats::tad::magic)));
    CHECK(!demo_recognised(
        std::span(whole).first(
            formats::tad::layout::chunk_length_bytes + formats::tad::layout::magic_bytes - 1
        )
    ));

    // The header chunk claims the whole fixed header; only its magic follows.
    const std::size_t claimed =
        formats::tad::layout::chunk_length_bytes + formats::tad::layout::header_fixed_bytes;
    std::vector<uint8_t> truncated;
    append16(&truncated, static_cast<uint16_t>(claimed));
    truncated.insert(truncated.end(), formats::tad::magic.begin(), formats::tad::magic.end());
    CHECK(demo_recognised(truncated));
    DemoPlayback playback;
    std::string error;
    CHECK(!demo_load(&playback, truncated, &error));
    CHECK(error == "demo parse failed at offset 0: header: chunk runs past end of file");
    // A whole header of another version is recognised and refused.
    auto other_version = whole;
    other_version[formats::tad::layout::chunk_length_bytes + formats::tad::layout::magic_bytes] =
        formats::tad::supported_version + 1;
    CHECK(demo_recognised(other_version));
    CHECK(!demo_load(&playback, other_version, &error));
    CHECK(error == "demo parse failed at offset 0: demo version 6 is not supported");
}

void transport_follows_the_ticks() {
    auto recording = make_recording(kUnitsPerPlayer, 1);
    recording.add(1, 0, probe_payload());
    recording.add(1, 50, elided_state_payload(3, true));
    recording.add(1, 30, probe_payload());
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, recording.bytes(), &error));
    CHECK(playback.frames.size() == 3);
    if (playback.frames.size() != 3)
        return;
    CHECK(playback.frames[1].due_tick == 4 && playback.frames[2].due_tick == 4);
    auto transport = demo_transport(&playback);
    std::array<uint8_t, 64> buffer{};
    uint32_t from = 0, to = 7, size = static_cast<uint32_t>(buffer.size());
    CHECK(
        transport.receive(transport.context, &from, &to, buffer.data(), &size) ==
        transport_result::ok
    );
    CHECK(from == recorded_id(0) && to == broadcast_destination_id && size > frame_header_bytes);
    size = static_cast<uint32_t>(buffer.size());
    playback.tick = 3;
    CHECK(
        transport.receive(transport.context, &from, &to, buffer.data(), &size) ==
        transport_result::no_messages
    );
    // Room for the probe frame only: the unit-state frame is dropped.
    playback.tick = 4;
    size = static_cast<uint32_t>(playback.frames[2].datagram.size());
    CHECK(size < playback.frames[1].datagram.size());
    CHECK(
        transport.receive(transport.context, &from, &to, buffer.data(), &size) ==
        transport_result::ok
    );
    CHECK(playback.stats.frames_oversized == 1 && playback.stats.frames_delivered == 2);
    CHECK(demo_exhausted(playback));
    CHECK(transport.send(transport.context, 1, 0, 0, buffer.data(), 1) == transport_result::ok);
    CHECK(playback.stats.sends_dropped == 1);
}

// A verdict's value: the local and remote side bytes, then the s16 limit.
constexpr uint32_t verdict_value(uint8_t local, uint8_t remote, int16_t limit) {
    return local | uint32_t{remote} << 8 | uint32_t{static_cast<uint16_t>(limit)} << 16;
}

constexpr uint32_t shared_verdict = verdict_value(1, 1, -1);

void unit_table_check_compares_checksum_keys() {
    auto recording = make_recording(kUnitsPerPlayer, 1, 0);
    recording.announce(checksum, 0x11, 0xa);
    recording.announce(checksum, 0x22, 0xb);
    recording.announce(verdict_subtype, 0x11, shared_verdict);
    recording.announce(verdict_subtype, 0x22, shared_verdict);
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, recording.bytes(), &error));
    std::vector<std::size_t> extra;
    const std::array<uint32_t, 2> same{0x22, 0x11};
    auto check = demo_check_unit_table(playback, same, &extra);
    CHECK(
        check.recorded == 2 && check.matched == 2 && check.missing == 0 && check.extra == 0 &&
        check.identical
    );
    const std::array<uint32_t, 3> shifted{0x05, 0x11, 0x22};
    check = demo_check_unit_table(playback, shifted, &extra);
    CHECK(!check.identical && check.extra == 1 && extra.size() == 1 && extra[0] == 0);
    const std::array<uint32_t, 1> partial{0x11};
    check = demo_check_unit_table(playback, partial, nullptr);
    CHECK(!check.identical && check.missing == 1 && check.matched == 1);
}

// FBI hashes and checksums from the unit-check chunk of a TADR 1.0.0.545
// recording (Caldera's Rim, 2009).
struct CheckedKey {
    uint32_t key = 0;
    uint32_t checksum = 0;
};

// Three types every machine shared.
constexpr std::array<CheckedKey, 3> shared_keys{
    {{0xb8f878b6, 0x1ece8664}, {0xf32bae26, 0x0058487e}, {0x348e0036, 0x90286874}}
};
// A type the 2009 table shares that the TADR 0.93b2 tables lack.
constexpr CheckedKey later_key{0xd6d867f3, 0xd7d7ef67};
// Types only the recorder had: the host's verdicts clear their remote side
// in 2009, set it in the 2003-2026 recordings whose players all had them.
constexpr std::array<CheckedKey, 4> recorder_keys{
    {{0x0cea3bbf, 0xc1d984ec},
     {0x27eba103, 0x3b4d9fe7},
     {0x74b6e363, 0x42b45a5a},
     {0xae880371, 0x870b2c4c}}
};
// A key no unit file hashes to, which the recorder announces with a new
// checksum each game and the host never shares.
constexpr CheckedKey recorder_marker{0x92549357, 0x032cd2a8};

Recording checked_recording(bool later_type, bool recorder_types_shared) {
    auto recording = make_recording(kUnitsPerPlayer, 1, 0);
    const auto check = [&](const CheckedKey& type, bool shared) {
        recording.announce(checksum, type.key, type.checksum);
        recording.announce(verdict_subtype, type.key, verdict_value(1, shared ? 1 : 0, -1));
    };
    for (const auto& type : shared_keys)
        check(type, true);
    if (later_type)
        check(later_key, true);
    for (const auto& type : recorder_keys)
        check(type, recorder_types_shared);
    check(recorder_marker, false);
    return recording;
}

// The watcher's unit table: this install's types in name order, marked from
// the watcher's sync table and compacted, as the game does.
std::vector<uint32_t> watcher_table(const DemoPlayback& playback, uint32_t* count) {
    constexpr std::array<uint32_t, 4> installed{
        shared_keys[0].key, shared_keys[1].key, later_key.key, shared_keys[2].key
    };
    std::vector<UnitDef> table(installed.size() + 1);
    const auto table_count = static_cast<uint32_t>(table.size());
    for (std::size_t index = 0; index < installed.size(); ++index) {
        auto& unit = table[index + 1];
        std::snprintf(unit.unit_name, sizeof unit.unit_name, "TYPE%zu", index + 1);
        unit.fbi_hash = installed[index];
        unit.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
    }
    table[0].flags = OA_UNIT_DEF_FLAG_AVAILABLE;
    const auto* sync = demo_unit_sync(playback);
    CHECK(sync != nullptr);
    if (sync != nullptr)
        ui::frontend_multiplayer::unit_sync_mark_units(*sync, table.data(), table_count);
    *count = data::defs::unit_defs_finalize_catalog(table.data(), table_count);
    std::vector<uint32_t> keys;
    for (uint32_t index = 1; index < *count; ++index) {
        CHECK(table[index].type_id == index);
        const uint32_t fbi_hash = table[index].fbi_hash;
        keys.push_back(fbi_hash);
    }
    return keys;
}

// The recorded game's table is the verdicts' shared set: the recorder's own
// types and marker leave it when a verdict clears their remote side, and a
// local type without a verdict leaves the watcher's table.
void unit_table_follows_the_recorded_verdicts() {
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, checked_recording(true, false).bytes(), &error));
    CHECK(playback.recorded_verdicts == 9 && playback.recorded_definitions == 4);
    CHECK(playback.unit_def_bits == 3);
    uint32_t count = 0;
    auto keys = watcher_table(playback, &count);
    CHECK(count == 5);
    auto check = demo_check_unit_table(playback, keys, nullptr);
    CHECK(check.recorded == 4 && check.matched == 4 && check.identical);

    CHECK(demo_load(&playback, checked_recording(true, true).bytes(), &error));
    CHECK(playback.recorded_definitions == 8 && playback.unit_def_bits == 4);
    keys = watcher_table(playback, &count);
    CHECK(count == 5);
    check = demo_check_unit_table(playback, keys, nullptr);
    CHECK(check.recorded == 8 && check.matched == 4 && check.missing == 4 && check.extra == 0);
    CHECK(!check.identical);

    CHECK(demo_load(&playback, checked_recording(false, false).bytes(), &error));
    CHECK(playback.recorded_verdicts == 8 && playback.recorded_definitions == 3);
    keys = watcher_table(playback, &count);
    CHECK(count == 4 && keys.size() == 3);
    if (keys.size() == 3)
        CHECK(keys[2] == shared_keys[2].key);
    check = demo_check_unit_table(playback, keys, nullptr);
    CHECK(check.recorded == 3 && check.matched == 3 && check.extra == 0 && check.identical);
}

World* make_world() {
    World* world = world_create();
    const WorldCapacity capacity{kUnitsPerPlayer * 10u + 1u, 2, 0};
    CHECK(world_alloc_tables(world, &capacity));
    world->game.units_per_player = kUnitsPerPlayer;
    for (uint32_t i = 0; i < world->unit_slot_count; ++i)
        world->units[i].id = static_cast<uint16_t>(i);
    return world;
}

void player_table_follows_the_recorded_lobby() {
    auto recording = make_recording(kUnitsPerPlayer, 2);
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, recording.bytes(), &error));
    CHECK(playback.watcher_id == recorded_id(0) + 1);
    World* world = make_world();
    uint8_t watcher = 0xff;
    CHECK(demo_bind_players(playback, world, &watcher));
    CHECK(watcher == 2);
    const auto& players = world->game.players;
    CHECK(
        players[0].in_use == 1 && players[0].status == OA_PLAYER_STATUS_MIRRORED &&
        players[0].player_id == recorded_id(0)
    );
    CHECK(
        players[1].in_use == 1 && players[1].status == OA_PLAYER_STATUS_MIRRORED &&
        players[1].player_id == recorded_id(1)
    );
    CHECK(
        players[2].in_use == 1 && players[2].status == OA_PLAYER_STATUS_LOCAL &&
        players[2].player_id == playback.watcher_id
    );
    CHECK(players[3].in_use == 0 && players[3].player_id == no_player_id);
    CHECK(
        std::strcmp(players[0].name, "player1") == 0 && std::strcmp(players[1].name, "player2") == 0
    );
    CHECK((world->player_info[2].options & OA_SETUP_OPTION_WATCHER) != 0);
    CHECK(world->player_info[1].side == 1 && world->player_info[1].color == recorded_color(1));
    // The host plays with line of sight and its type (options 0x600); the
    // local watcher keeps only the type and views from its own slot.
    CHECK(world->game.session_rules == 1 && (world->game.visibility_flags & 7) == 4);
    CHECK(world->game.local_player_index == 2 && world->game.viewpoint_player == 2);
    CHECK(world->game.unit_def_id_bits == 2);
    CHECK((world->game.session_flags & kNetFlagLive) != 0);
    match_assign_unit_ranges(world);
    // Player 2's id is the lower one: its range comes first.
    CHECK(
        players[1].first_unit == oa_ref_from_index(1) &&
        players[1].last_unit == oa_ref_from_index(kUnitsPerPlayer)
    );
    CHECK(players[0].first_unit == oa_ref_from_index(kUnitsPerPlayer + 1));
    CHECK(players[2].first_unit == oa_ref_from_index(2 * kUnitsPerPlayer + 1));
    world_destroy(world);
}

void player_table_without_status_blocks() {
    auto recording = make_recording(kUnitsPerPlayer, 2, 1, false);
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, recording.bytes(), &error));
    CHECK(playback.players.size() == 2 && !playback.players[0].from_status);
    World* world = make_world();
    uint8_t watcher = 0xff;
    CHECK(demo_bind_players(playback, world, &watcher));
    CHECK(world->game.players[0].player_id == 1 && world->game.players[1].player_id == 2);
    CHECK(world->game.players[2].player_id == 3 && world->player_info[1].color == 2);
    world_destroy(world);

    auto duplicate = make_recording(kUnitsPerPlayer, 2, 1, false);
    duplicate.demo.players[1].number = 1;
    duplicate.demo.statuses[1].number = 1;
    CHECK(!demo_load(&playback, duplicate.bytes(), &error));
}

// ---- round trip through two tiny matches ----

using Services = oa::test::QuietServices;

using Scenario = oa::test::EmptyScenario;

struct Captured {
    uint32_t tick{};
    std::vector<uint8_t> datagram;
};

struct Capture {
    std::vector<Captured> datagrams;
    const Game* game{}; // the recorder's, whose tick stamps each datagram
};

uint32_t
capture_send(void* context, uint32_t, uint32_t, uint32_t, const uint8_t* data, uint32_t size) {
    auto* capture = static_cast<Capture*>(context);
    capture->datagrams.push_back({capture->game->tick, std::vector<uint8_t>(data, data + size)});
    return transport_result::ok;
}

uint32_t capture_receive(void*, uint32_t*, uint32_t*, uint8_t*, uint32_t*) {
    return transport_result::no_messages;
}

// One machine: a 16x16-cell map and one mobile unit type. The recorded
// host's rules select terrain-limited line of sight, so the map carries
// the altitude sight tables.
struct Machine {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values =
        std::vector<sim::visibility_state::TerrainCell>(256);
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::vector<sim::visibility_state::AltitudeCell> altitude_cells =
        std::vector<sim::visibility_state::AltitudeCell>(64);
    std::vector<sim::visibility_state::AltitudeSightPattern> altitude_patterns =
        std::vector<sim::visibility_state::AltitudeSightPattern>(6);
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    std::array<sim::unit_spawn::LoadedType, 2> loaded{};
    std::array<sim::unit_spawn::Type, 2> types{};
    data::unit_definitions::UnitDefinition def;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::vector<sim::spatial_state::Plot> collision_plots =
        std::vector<sim::spatial_state::Plot>(256);
    std::array<sim::match_runtime::RuntimeTypeFields, 2> fields{};
    std::array<uint8_t, 1> yard{4};
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    Machine() {
        map.attribute_width = map.attribute_height = 16;
        map.attributes.resize(256);
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        model->objects.resize(1);
        model->objects[0].name = "root";
        script->code = {sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        loaded[1].model = model;
        loaded[1].script = script;
        types[1].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
        types[1].simulation.maximum_health = 100;
        types[1].footprint_x = types[1].footprint_z = 1;
        types[1].model = reinterpret_cast<uintptr_t>(model.get());
        types[1].cob = reinterpret_cast<uintptr_t>(script.get());
        types[1].bm_code = 1;
        loaded[1].type = types[1];
        def.sight_distance = 160;
        def.acceleration_fixed = kFixedOne;
        def.brake_rate_fixed = kFixedOne;
        def.max_velocity_fixed = 2 * kFixedOne;
        def.turn_rate = 1024;
        def.energy_storage = 1000.0F;
        def.metal_storage = 1000.0F;
        fields[1].definition = &def;
        fields[1].yard_mask = yard;
        fields[1].runtime_metadata = &metadata;
        fields[1].target_masks = &target_masks;
        fields[1].movement_class = 0;
    }

    void build(uint8_t viewpoint) {
        sim::match_runtime::OfflineInputs input{
            map,
            loaded,
            types,
            fields,
            weapons,
            terrain_values,
            masks,
            8,
            8,
            kUnitsPerPlayer,
            2,
            viewpoint,
            30,
            1,
            &scenario,
            {},
            collision_plots,
            sim::visibility_state::AltitudeSightData{8, 8, altitude_cells, altitude_patterns}
        };
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        match->configure_strategic_environment({0, 0.5f, 0});
    }
};

// The recording machine: one local player whose datagrams are captured.
struct Recorder {
    Machine machine;
    Capture capture;
    NetConnection connection{};
    std::unique_ptr<NetMatch> net = std::make_unique<NetMatch>();
    MatchBinding binding;

    void start() {
        machine.build(0);
        auto& world = machine.match->state();
        auto& player = world.game.players[0];
        player.in_use = 1;
        player.index = 0;
        player.player_id = recorded_id(0);
        player.status = OA_PLAYER_STATUS_LOCAL;
        player.info = oa_ref_from_index(0);
        for (uint8_t slot = 1; slot < OA_PLAYER_COUNT; ++slot)
            world.game.players[slot].player_id = no_player_id;
        world.game.local_player_index = 0;
        world.game.unit_def_id_bits = unit_def_id_bits_for_count(world.unit_def_count);
        world.game.session_flags |= kNetFlagLive | kNetFlagGameStarted;
        std::array<uint8_t, 10> allies{};
        allies[0] = 1;
        machine.match->configure_outcomes(0, allies, false);
        machine.match->configure_player_alliances(0, allies);
        capture.game = &world.game;
        connection.packets = new PacketLayer();
        packet_layer_create(connection.packets);
        packet_layer_start(
            connection.packets, NetTransport{&capture, capture_send, capture_receive}
        );
        match_binding_init(&binding, machine.match.get(), net.get());
        net_match_begin(
            net.get(),
            &connection,
            &world,
            match_binding_sim(&binding),
            match_binding_hooks(&binding)
        );
        match_binding_install(&binding);
        net_match_enter_game(net.get());
    }

    void step() {
        try {
            match_binding_tick(&binding);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(
                stderr, "recorder tick %u: %s\n", machine.match->state().game.tick, error.what()
            );
        }
        packet_layer_flush(connection.packets, 0, true);
    }

    ~Recorder() { net_connection_destroy(&connection); }
};

// A captured 0x2c as the recorder keeps it: 0xff for the minimal record (no
// unit entry, flag set, empty full record), else 0xfd without the tick.
void append_recorded_state(
    std::span<const uint8_t> record, unsigned def_bits, std::vector<uint8_t>* payload
) {
    const auto body = record.subspan(unit_state_header_bytes);
    constexpr std::size_t list_and_flag_bits = 17;
    bool minimal = body.size() == (list_and_flag_bits + def_bits + 7) / 8 && body[0] == 0xff &&
                   body[1] == 0xff && (body[2] & 1) != 0;
    for (std::size_t bit = list_and_flag_bits; minimal && bit < body.size() * 8; ++bit)
        minimal = ((body[bit / 8] >> (bit % 8)) & 1) == 0;
    if (minimal) {
        payload->push_back(static_cast<uint8_t>(formats::tad::RecordType::empty_tick));
        return;
    }
    payload->push_back(static_cast<uint8_t>(formats::tad::RecordType::elided_unit_state));
    append16(payload, load_u16(record.data() + 1));
    payload->insert(payload->end(), body.begin(), body.end());
}

// paused_while_recording: the recording machine pauses the game for a few
// ticks while the unit moves; playback runs through the pause.
// The verdict of an unbound session, whose spacing is the recording's unit
// limit: each count that makes a replay unclean, the two a different unit
// table forgives, and the edges of the full records' spacing.
void verdict_judges_errors_and_spacing() {
    constexpr uint16_t period = 100;
    DemoSession session;
    session.playback.demo.max_units = period;
    auto verdict = demo_session_verdict(session, false);
    CHECK(verdict.clean && verdict.paced);

    session.net.record_errors = 1;
    CHECK(!demo_session_verdict(session, false).clean);
    CHECK(demo_session_verdict(session, true).clean);
    session.net.record_errors = 0;
    session.binding.creates_past_table = 1;
    CHECK(!demo_session_verdict(session, false).clean);
    CHECK(demo_session_verdict(session, true).clean);
    session.binding.creates_past_table = 0;
    session.tick_errors = 1;
    CHECK(!demo_session_verdict(session, true).clean);
    session.tick_errors = 0;
    session.binding.refused_creates = 1;
    CHECK(!demo_session_verdict(session, true).clean);
    session.binding.refused_creates = 0;

    // Gaps of the period, then of the period give or take the slack.
    constexpr auto slack = static_cast<uint32_t>(full_record_slack_ticks);
    for (const uint32_t tick : {10u, 10u + period, 10u + 2 * period + slack, 10u + 3 * period})
        session.tracked.push_back({tick, {}, {}});
    verdict = demo_session_verdict(session, false);
    CHECK(verdict.clean && verdict.paced);
    session.tracked.push_back({session.tracked.back().tick + period + slack + 1, {}, {}});
    CHECK(!demo_session_verdict(session, false).paced);
    session.tracked.back().tick -= 2 * slack + 2; // period - slack - 1
    CHECK(!demo_session_verdict(session, false).paced);
    ++session.tracked.back().tick; // period - slack
    CHECK(demo_session_verdict(session, false).paced);
}

// The recording of a short game in which one unit walks, as a recorder stores it, with the
// recorder's own machine to compare the replay with.
struct RecordedGame {
    std::unique_ptr<Recorder> recorder;
    Recording recording;
    uint16_t index{};
    uint32_t ticks{};
};

// paused_while_recording: the recording machine pauses the game for a few ticks while the unit
// moves; playback runs through the pause.
RecordedGame record_a_walking_unit(bool paused_while_recording) {
    RecordedGame game;
    game.recorder = std::make_unique<Recorder>();
    auto& recorder = game.recorder;
    recorder->start();
    auto* unit =
        recorder->machine.match->create({0, 1, {48u * kFixedOne, 0, 48u * kFixedOne}, true, 1, 0});
    CHECK(unit != nullptr);
    if (unit == nullptr)
        return game;
    const auto index = unit->unit_index;
    constexpr uint32_t recorded_ticks = 90;
    for (uint32_t t = 0; t < recorded_ticks; ++t) {
        if (t == 5) {
            try {
                (void)recorder->machine.match->issue_ground_move(
                    index, {48 * kFixedOne, 0, 96 * kFixedOne}, false
                );
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "order: %s\n", error.what());
            }
        }
        if (paused_while_recording && t == 40)
            net_match_set_pause(recorder->net.get(), true);
        if (paused_while_recording && t == 45)
            net_match_set_pause(recorder->net.get(), false);
        recorder->step();
    }
    const auto& recorded_unit = recorder->machine.match->state().units[index];
    CHECK(recorded_unit.position.z != 48 * kFixedOne);
    CHECK(recorder->capture.datagrams.size() > recorded_ticks / 2);

    // The recorder's datagrams as the .tad keeps them, six sender ticks per
    // packet as the game sends: marker, records, unit states behind a
    // tick base.
    constexpr std::size_t ticks_per_packet = 6;
    constexpr uint32_t milliseconds_per_tick = 33;
    const auto def_bits =
        static_cast<unsigned>(recorder->machine.match->state().game.unit_def_id_bits);
    auto recording = make_recording(kUnitsPerPlayer, 1);
    std::vector<uint8_t> payload{formats::tad::payload_marker};
    std::size_t in_packet = 0;
    uint32_t packet_tick = 0;
    uint32_t previous_packet_tick = 0;
    uint32_t next_state_tick = 0;
    bool based = false;
    const auto end_packet = [&] {
        if (payload.size() > 1)
            recording.add(
                1,
                static_cast<uint16_t>((packet_tick - previous_packet_tick) * milliseconds_per_tick),
                payload
            );
        previous_packet_tick = packet_tick;
        payload.assign(1, formats::tad::payload_marker);
        in_packet = 0;
        based = false;
    };
    for (const auto& captured : recorder->capture.datagrams) {
        const auto frame = oa::netgame::network::decode_frame(captured.datagram);
        if (in_packet == 0)
            packet_tick = captured.tick;
        for (std::size_t offset = frame_header_bytes; offset < frame.size();) {
            const auto length =
                formats::tad::record_length({frame.data() + offset, frame.size() - offset});
            CHECK(length != 0);
            if (length == 0)
                break;
            const std::span<const uint8_t> record(frame.data() + offset, length);
            offset += length;
            if (record[0] != static_cast<uint8_t>(RecordType::unit_state)) {
                payload.insert(payload.end(), record.begin(), record.end());
                continue;
            }
            const auto tick = load_u32(record.data() + unit_state_prefix_bytes);
            if (!based || tick != next_state_tick) {
                payload.push_back(static_cast<uint8_t>(formats::tad::RecordType::tick_base));
                append32(&payload, tick);
                based = true;
            }
            append_recorded_state(record, def_bits, &payload);
            next_state_tick = tick + 1;
        }
        if (++in_packet == ticks_per_packet)
            end_packet();
    }
    end_packet();
    game.recording = std::move(recording);
    game.index = index;
    game.ticks = recorded_ticks;
    return game;
}

void replay_reproduces_a_recorded_unit(bool paused_while_recording) {
    auto game = record_a_walking_unit(paused_while_recording);
    if (game.recorder == nullptr || game.ticks == 0)
        return;
    auto& recorder = game.recorder;
    auto& recording = game.recording;
    const auto index = game.index;
    const uint32_t recorded_ticks = game.ticks;
    const auto& recorded_unit = recorder->machine.match->state().units[index];
    const auto release = [](DemoSession* session) {
        demo_session_end(session);
        delete session;
    };
    std::unique_ptr<DemoSession, decltype(release)> session(new DemoSession(), release);
    std::string error;
    CHECK(demo_load(&session->playback, recording.bytes(), &error));
    const auto& stats = session->playback.stats;
    CHECK(stats.bad_packets == 0 && stats.frames == recording.demo.packets.size());
    CHECK(stats.empty_ticks > 0 && stats.untimed_unit_states == 0 && stats.recorder_records > 0);

    Machine replay;
    replay.build(1);
    std::array<uint8_t, OA_PLAYER_COUNT> watcher_allies{};
    watcher_allies[1] = 1;
    replay.match->configure_outcomes(1, watcher_allies, false);
    CHECK(demo_session_begin(session.get(), replay.match.get(), &error));
    CHECK(error.empty());
    if (session->match == nullptr)
        return;
    CHECK(session->watcher_slot == 1);
    const auto& world = replay.match->state();
    CHECK(world.game.players[0].first_unit == oa_ref_from_index(1));
    const auto initial_digest = demo_world_digest(&world);
    uint32_t held_frames = 0;
    for (uint32_t t = 0; t < recorded_ticks + record_hold_window_ticks + 10; ++t) {
        const auto tick = world.game.tick;
        demo_session_frame(session.get());
        held_frames += world.game.tick == tick ? 1u : 0u;
        CHECK((world.game.sim_run_flags & run_flag_paused) == 0);
    }
    CHECK(held_frames == 0);
    CHECK(session->tick_errors == 0);
    if (session->tick_errors != 0)
        std::fprintf(stderr, "replay: %s\n", session->last_error.c_str());
    CHECK(session->net.record_errors == 0);
    CHECK(session->net.records_applied > recorded_ticks / 2);
    CHECK(session->binding.created_remote == 1 && session->binding.refused_creates == 0);
    CHECK(demo_session_finished(*session));
    const auto verdict = demo_session_verdict(*session, false);
    CHECK(verdict.clean && verdict.paced);
    CHECK(demo_live_units(&world) == 1);
    // A full record rides on every units_per_player-th tick, and the unit's
    // recorded position changes between its records while it moves. The
    // replay's remote driver steps it through the same positions, so no
    // record moves it.
    const auto& full = session->full_records;
    CHECK(full.placements >= recorded_ticks / kUnitsPerPlayer / 2);
    CHECK(session->tracked_unit == index && session->tracked.size() >= 2);
    CHECK(full.ground.measured > 0 && full.ground.moved > 0 && full.air.measured == 0);
    CHECK(full.ground.drift_max == 0);
    uint32_t displaced = 0;
    for (const auto& sample : session->tracked)
        displaced += sample.before.x != sample.recorded.x || sample.before.y != sample.recorded.y ||
                             sample.before.z != sample.recorded.z
                         ? 1u
                         : 0u;
    CHECK(displaced == 0);
    if (full.ground.drift_max != 0 || displaced != 0)
        std::fprintf(
            stderr,
            "replay drift: max %u over %u records, %u of %zu tracked records displaced\n",
            full.ground.drift_max,
            full.ground.measured,
            displaced,
            session->tracked.size()
        );
    const auto& replayed = world.units[index];
    CHECK(replayed.type_index == 1 && replayed.owner_index == 0);
    CHECK(
        replayed.position.x == recorded_unit.position.x &&
        replayed.position.z == recorded_unit.position.z
    );
    CHECK(replayed.health == recorded_unit.health);
    CHECK(demo_world_digest(&world) != initial_digest);
    // The watcher's own unit states go out and are dropped.
    CHECK(stats.frames_oversized == 0 && stats.sends_dropped > 0);
}

template <class R>
void append_record(std::vector<uint8_t>* payload, const R& record) {
    std::array<uint8_t, record_length_table[static_cast<uint8_t>(R::type)]> bytes{};
    std::size_t written = 0;
    CHECK(encode_record(record, bytes.data(), bytes.size(), &written) == WireError::ok);
    payload->insert(
        payload->end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(written)
    );
}

// ---- keyframes ----

void release_session(DemoSession* session) {
    demo_session_end(session);
    delete session;
}

// A recording replayed in a match of its own, bound and ready to play.
struct Replaying {
    std::unique_ptr<Machine> machine = std::make_unique<Machine>();
    std::unique_ptr<DemoSession, void (*)(DemoSession*)> session{
        new DemoSession(), release_session
    };

    // Loads the recording and binds the match to it; false when either refuses.
    bool start(std::vector<uint8_t> bytes) {
        std::string error;
        if (!demo_load(&session->playback, std::move(bytes), &error))
            return false;
        machine->build(1);
        std::array<uint8_t, OA_PLAYER_COUNT> watcher_allies{};
        watcher_allies[1] = 1;
        machine->match->configure_outcomes(1, watcher_allies, false);
        return demo_session_begin(session.get(), machine->match.get(), &error);
    }

    // Plays frames and returns the digest of the world after each.
    std::vector<uint64_t> play(uint32_t frames) {
        std::vector<uint64_t> digests;
        for (uint32_t i = 0; i < frames; ++i) {
            demo_session_frame(session.get());
            digests.push_back(demo_world_digest(&machine->match->state()));
        }
        return digests;
    }

    uint32_t tick() const { return machine->match->state().game.tick; }

    uint64_t digest() const { return demo_world_digest(&machine->match->state()); }
};

// A keyframe taken and restored gives the same world and the same state, tick for tick; the keyframes
// the viewer takes leave what the replay computes as it is, whichever it restores and in whichever order.
void a_keyframe_restores_the_replay() {
    auto game = record_a_walking_unit(false);
    CHECK(game.recorder != nullptr);
    const auto bytes = game.recording.bytes();
    const uint32_t total = game.ticks + record_hold_window_ticks + 10;

    // The replay with no keyframe, to its end.
    Replaying plain;
    CHECK(plain.start(bytes));
    const auto plain_digests = plain.play(total);
    const auto end_digest = plain.digest();

    Replaying r;
    CHECK(r.start(bytes));
    std::string refusal;
    DemoKeyframe start;
    CHECK(demo_keyframe_take(r.session.get(), &start, &refusal));
    CHECK(refusal.empty() && start.tick == 0 && start.digest == r.digest());
    CHECK(demo_keyframe_base_bytes(*r.session) == start.fixed_bytes);
    r.play(30);
    DemoKeyframe key;
    CHECK(demo_keyframe_take(r.session.get(), &key, &refusal));
    CHECK(key.tick == 30 && key.digest == r.digest() && key.digest == plain_digests[29]);
    CHECK(key.raw_bytes() > key.frame.size() && !key.frame.empty());
    const auto first_run = r.play(40);
    CHECK(first_run[39] == plain_digests[69]);
    DemoKeyframe later;
    std::vector<uint8_t> image_fixed;
    std::vector<uint8_t> image_varying;
    DemoKeyframeOptions options;
    options.image_fixed = &image_fixed;
    options.image_varying = &image_varying;
    options.hash_image = true;
    CHECK(demo_keyframe_take(r.session.get(), &later, &refusal, options));
    CHECK(later.tick == 70);

    // Restored, the replay plays the same 40 ticks to the same digests and holds the same state.
    std::string error;
    CHECK(demo_keyframe_restore(r.session.get(), key, &later, &error));
    CHECK(error.empty() && r.tick() == 30 && r.digest() == key.digest);
    const auto second_run = r.play(40);
    CHECK(second_run == first_run);
    CHECK(r.session->keyframe_faults.empty());
    std::vector<uint8_t> after_fixed;
    std::vector<uint8_t> after_varying;
    DemoKeyframe check;
    options.image_fixed = &after_fixed;
    options.image_varying = &after_varying;
    options.image_only = true;
    CHECK(demo_keyframe_take(r.session.get(), &check, &refusal, options));
    CHECK(check.image_digest == later.image_digest);
    CHECK(demo_keyframe_difference(image_fixed, image_varying, after_fixed, after_varying).empty());

    // Forward to a later keyframe, back to the start, and on to the end: the replay still ends on
    // the digest the replay with no keyframe ends on.
    CHECK(demo_keyframe_restore(r.session.get(), later));
    CHECK(r.tick() == 70 && r.digest() == later.digest);
    CHECK(demo_keyframe_restore(r.session.get(), start));
    CHECK(r.tick() == 0);
    const auto whole = r.play(total);
    CHECK(whole == plain_digests);
    CHECK(r.digest() == end_digest);
    CHECK(demo_session_finished(*r.session));
    CHECK(r.session->net.records_applied == plain.session->net.records_applied);
    CHECK(
        r.session->playback.stats.frames_delivered == plain.session->playback.stats.frames_delivered
    );
    CHECK(r.session->tracked.size() == plain.session->tracked.size());
    CHECK(r.session->keyframe_faults.empty());
}

// A field the keyframe does not carry shows as a keyframe fault: the restored replay reaches the next
// keyframe's tick with another digest, and the fault names that tick.
void a_field_left_out_is_a_keyframe_fault() {
    auto game = record_a_walking_unit(false);
    const auto bytes = game.recording.bytes();

    struct Leave {
        // The playback's cursor and what it counted: a restored replay then does not deliver
        // again the frames its first run delivered, and misses the records of those ticks.
        static bool cursor(void*, std::string_view section) { return section == "demo.state"; }
    };

    for (const bool leave_out : {false, true}) {
        Replaying r;
        CHECK(r.start(bytes));
        std::string refusal;
        DemoKeyframe base;
        CHECK(demo_keyframe_take(r.session.get(), &base, &refusal));
        r.play(20);
        DemoKeyframe key;
        DemoKeyframeOptions options;
        if (leave_out)
            options.omission = {nullptr, &Leave::cursor};
        CHECK(demo_keyframe_take(r.session.get(), &key, &refusal, options));
        r.play(40);
        DemoKeyframe later;
        CHECK(demo_keyframe_take(r.session.get(), &later, &refusal));
        std::string error;
        CHECK(demo_keyframe_restore(r.session.get(), key, &later, &error));
        r.play(40);
        if (leave_out) {
            // The cursor stayed past the frames of those ticks, and the unit missed their records.
            CHECK(r.session->keyframe_faults.size() == 1);
            if (!r.session->keyframe_faults.empty()) {
                const auto& fault = r.session->keyframe_faults.front();
                CHECK(fault.tick == later.tick && fault.expected == later.digest);
                CHECK(fault.found != fault.expected && fault.found == r.digest());
            }
        } else {
            CHECK(r.session->keyframe_faults.empty());
            CHECK(r.digest() == later.digest);
        }
    }
}

// A session that holds what a keyframe cannot carry is refused, and says so.
void a_session_with_a_unit_of_its_own_is_refused() {
    auto game = record_a_walking_unit(false);
    Replaying r;
    CHECK(r.start(game.recording.bytes()));
    r.play(10);
    const auto census = r.machine->match->keyframe_census();
    CHECK(census.orders == 0 && census.unit_order_queues == 0);
    CHECK(census.live_units == 1 && census.mirrored_ground == 1);
    // A unit of this machine's own, with an order: nothing of a playback.
    auto* mine =
        r.machine->match->create({1, 1, {80u * kFixedOne, 0, 80u * kFixedOne}, true, 1, 0});
    CHECK(mine != nullptr);
    DemoKeyframe key;
    std::string refusal;
    CHECK(!demo_keyframe_take(r.session.get(), &key, &refusal));
    CHECK(!refusal.empty());
}

// Records that reach the match's own hooks (an economy record asking for an
// answer, an alliance, a resource give) reach the binding behind them, and
// a packet from a player number no player chunk names is dropped rather
// than offered as a system message.
void recorded_records_reach_the_binding() {
    auto recording = make_recording(kUnitsPerPlayer, 1);
    const uint32_t watcher_id = recorded_id(0) + 1;
    std::vector<uint8_t> payload{formats::tad::payload_marker};
    EconomyRecord economy{};
    economy.want_reply = 1;
    economy.metal = 10.0f;
    append_record(&payload, economy);
    AllianceRecord alliance{};
    alliance.player_id_a = recorded_id(0);
    alliance.player_id_b = watcher_id;
    alliance.value = 1;
    append_record(&payload, alliance);
    ResourceGiveRecord give{};
    give.subtype = 3; // shared sight
    give.from_id = recorded_id(0);
    give.to_id = watcher_id;
    append_record(&payload, give);
    recording.add(1, 33, payload);
    recording.add(9, 33, probe_payload());
    const auto release = [](DemoSession* session) {
        demo_session_end(session);
        delete session;
    };
    std::unique_ptr<DemoSession, decltype(release)> session(new DemoSession(), release);
    std::string error;
    CHECK(demo_load(&session->playback, recording.bytes(), &error));
    CHECK(session->playback.stats.unknown_senders == 1);
    CHECK(session->playback.frames.size() == 1);
    CHECK(session->playback.watcher_id == watcher_id);
    Machine replay;
    replay.build(1);
    std::array<uint8_t, OA_PLAYER_COUNT> watcher_allies{};
    watcher_allies[1] = 1;
    replay.match->configure_outcomes(1, watcher_allies, false);
    CHECK(demo_session_begin(session.get(), replay.match.get(), &error));
    if (session->match == nullptr)
        return;
    for (int t = 0; t < 10; ++t)
        demo_session_frame(session.get());
    CHECK(session->tick_errors == 0);
    CHECK(demo_session_finished(*session));
    const auto& world = replay.match->state();
    CHECK(world.game.players[0].metal == 10.0f);
    CHECK(world.game.players[0].alliance[1] == 1);
    // The answer to the economy record goes out from the watcher, and is dropped.
    CHECK(session->playback.stats.sends_dropped > 0);
}

// A live recording's setup: players with ids 0x100, 0x200, ... and a unit
// check for each of three unit types.
RecordingSetup live_setup(std::size_t players, bool compress) {
    RecordingSetup setup{};
    setup.max_units = kUnitsPerPlayer;
    setup.map_name = "Test Map";
    setup.recorder_text = "Program 1.0";
    setup.date_text = "2026-10-03 08:00";
    setup.compress = compress;
    for (std::size_t i = 0; i < players; ++i) {
        RecordingPlayer player{};
        player.player_id = static_cast<uint32_t>(0x100 * (i + 1));
        player.name = "player" + std::to_string(i + 1);
        player.info.side = static_cast<uint8_t>(i & 1);
        player.info.color = static_cast<uint8_t>(i);
        player.info.max_units = kUnitsPerPlayer;
        player.info.version_major = 3;
        player.info.version_minor = 1;
        if (i == 0)
            player.info.role = 1;
        player.team = 5;
        setup.players.push_back(std::move(player));
    }
    for (uint32_t type = 1; type <= 3; ++type) {
        UnitDefHandshakeRecord check{};
        check.subtype = checksum;
        check.key = 0x1000 + type;
        check.value = 0x2000 + type;
        std::array<uint8_t, formats::tad::layout::unit_check_record_bytes> bytes{};
        CHECK(encode_record(check, bytes.data(), bytes.size(), nullptr) == WireError::ok);
        setup.unit_checks.push_back(bytes);
    }
    return setup;
}

// A unit state with one waypoint delta for unit 0 of type 1, at a tick.
std::vector<uint8_t> moving_state(uint32_t tick) {
    uint8_t storage[unit_state_writer_words * bit_stream_word_bytes];
    BitWriter writer;
    bit_writer_init(&writer, storage, unit_state_writer_words);
    uint16_t length = 0;
    CHECK(
        unit_state_write_start_position(&writer, tick, {1, 10, 20, 10, 20}, 2, &length) ==
        WireError::ok
    );
    return {storage, storage + length};
}

// A live recording keeps every record it saw, stored as a recorder stores
// them: unit states without their tick behind a tick base, empty ones as
// the empty-tick marker, the rest as they are, each under its sender's
// number; the statuses are the players' blocks as sent. Playback reads it.
void live_recording_round_trips(bool compress) {
    DemoRecording recording;
    recording_begin(&recording, live_setup(2, compress));
    ChatRecord chat{};
    std::snprintf(chat.text, sizeof chat.text, "%s", "<player1> hello there, everyone in the game");
    uint8_t chat_bytes[65];
    CHECK(encode_record(chat, chat_bytes, sizeof chat_bytes, nullptr) == WireError::ok);
    recording_add(&recording, 0x100, 1000, chat_bytes);
    const auto state = moving_state(42);
    recording_add(&recording, 0x200, 1033, state);
    const uint8_t empty[] = {0x2c, 0x0b, 0x00, 43, 0, 0, 0, 0xff, 0xff, 0x01, 0x00};
    recording_add(&recording, 0x200, 1066, empty);
    const uint8_t camera[] = {0xfc, 0x00, 0x01, 0xd0, 0x00};
    recording_add(&recording, 0x100, 1100, camera);
    recording_add(&recording, 0x999, 1200, camera);
    CHECK(recording.dropped_records == 1 && recording.packets.size() == 4);
    const auto written = recording_write(recording);
    CHECK(written.ok());
    const auto parsed = formats::tad::parse(written.bytes);
    CHECK(parsed.ok());
    if (!parsed.ok())
        return;
    const auto& demo = *parsed.demo;
    CHECK(demo.version == 5 && demo.max_units == kUnitsPerPlayer && demo.map_name == "Test Map");
    CHECK(demo.players.size() == 2 && demo.statuses.size() == 2);
    CHECK(demo.players[0].number == 1 && demo.players[0].name == "player1");
    CHECK(demo.players[1].side == 1 && demo.players[1].color == 1);
    const auto* version =
        formats::tad::find_sector(demo, formats::tad::SectorType::recorder_version);
    CHECK(version != nullptr && formats::tad::sector_text(*version) == "Program 1.0");
    CHECK(formats::tad::unit_check_records(demo).size() == 3);
    CHECK(demo.packets.size() == 4);
    if (demo.packets.size() == 4) {
        CHECK(demo.packets[0].sender == 1 && demo.packets[1].sender == 2);
        CHECK(demo.packets[1].delay_ms == 33 && demo.packets[3].time_ms == 100);
        const auto decoded = formats::tad::decode_payload(demo.packets[1].payload);
        CHECK(decoded.ok());
        const auto split = formats::tad::split_records(decoded.bytes);
        CHECK(split.records.size() == 2);
        if (split.records.size() == 2) {
            CHECK(
                split.records[0].type == static_cast<uint8_t>(formats::tad::RecordType::tick_base)
            );
            const auto expanded = formats::tad::expand_unit_state(split.records[1]);
            CHECK(expanded == state);
        }
        const auto empty_split = formats::tad::split_records(
            formats::tad::decode_payload(demo.packets[2].payload).bytes
        );
        CHECK(
            empty_split.records.size() == 2 &&
            empty_split.records[1].type ==
                static_cast<uint8_t>(formats::tad::RecordType::empty_tick)
        );
        const auto chat_payload = formats::tad::decode_payload(demo.packets[0].payload);
        CHECK(
            chat_payload.ok() && chat_payload.bytes.size() == 66 && chat_payload.bytes[1] == 0x05
        );
        if (compress)
            CHECK(demo.packets[0].payload[0] == formats::tad::compressed_payload_marker);
        else
            CHECK(demo.packets[0].payload[0] == formats::tad::payload_marker);
    }
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, written.bytes, &error));
    CHECK(playback.players.size() == 2 && playback.players[0].from_status);
    CHECK(playback.players[1].info.color == 1 && playback.players[0].info.role == 1);
    CHECK(playback.recorded_definitions == 3);
    CHECK(
        recording_file_name("2026-10-03 0800", "Seven: Islands", ".ted") ==
        "2026-10-03 0800 Seven_ Islands.ted"
    );
}

// ---- The recording kept as the game goes, and finished into a replay ----

/// The records every streamed recording below is made of, added to a recording the same way
/// whether it keeps its packets in memory or streams them.
///
/// @param recording a started recording of live_setup's two players
/// @param count how many unit states follow the first few records
void add_sample_records(DemoRecording* recording, uint32_t count) {
    ChatRecord chat{};
    std::snprintf(chat.text, sizeof chat.text, "%s", "<player1> hello there, everyone in the game");
    uint8_t chat_bytes[65];
    CHECK(encode_record(chat, chat_bytes, sizeof chat_bytes, nullptr) == WireError::ok);
    recording_add(recording, 0x100, 1000, chat_bytes);
    const uint8_t empty[] = {0x2c, 0x0b, 0x00, 43, 0, 0, 0, 0xff, 0xff, 0x01, 0x00};
    recording_add(recording, 0x200, 1033, empty);
    const uint8_t camera[] = {0xfc, 0x00, 0x01, 0xd0, 0x00};
    recording_add(recording, 0x100, 1100, camera);
    // A sender the recording does not name is dropped, stream or no stream.
    recording_add(recording, 0x999, 1200, camera);
    uint64_t now = 1200;
    for (uint32_t tick = 100; tick < 100 + count; ++tick) {
        now += 33;
        recording_add(recording, tick % 2 == 0 ? 0x100 : 0x200, now, moving_state(tick));
    }
}

/// Reads a whole file.
///
/// @param path the file
/// @return its bytes; empty when it cannot be read
std::vector<uint8_t> file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Writes a whole file.
///
/// @param path the file
/// @param bytes what it holds
void write_bytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
}

/// Tells whether a recording holds a sector of a type.
///
/// @param bytes the recording
/// @param type the sector type
/// @return true when the recording parses and holds one
bool has_sector(std::span<const uint8_t> bytes, uint32_t type) {
    const auto parsed = formats::tad::parse(bytes);
    if (!parsed.ok())
        return false;
    for (const auto& sector : parsed.demo->sectors)
        if (sector.type == type)
            return true;
    return false;
}

// What the streamed recording writes is what the recording written whole gives, byte for byte,
// for the same records: with packets stored plain or packed, with a few packets and with enough
// to wake the writer, and with nothing a network address could be in.
void streaming_writer_gives_the_bytes_recording_write_gives() {
    const auto scratch = oa::test::make_scratch_directory("demo-stream");
    for (const bool compress : {false, true}) {
        for (const uint32_t count : {0u, 5u, 3000u}) {
            DemoRecording whole;
            recording_begin(&whole, live_setup(2, compress));
            add_sample_records(&whole, count);
            const auto written = recording_write(whole);
            CHECK(written.ok());
            DemoRecording streamed;
            recording_begin(&streamed, live_setup(2, compress));
            const auto path = scratch / "streamed.tad";
            std::string error;
            CHECK(recording_stream_to(&streamed, path, &error));
            // The header is on disk before the first packet is added.
            CHECK(file_bytes(path).size() == recording_header(streamed.setup).bytes.size());
            add_sample_records(&streamed, count);
            CHECK(streamed.packets.empty() && streamed.packet_count == whole.packet_count);
            CHECK(streamed.stream != nullptr && streamed.stream->packets() == whole.packet_count);
            CHECK(streamed.stream->close(&error));
            CHECK(file_bytes(path) == written.bytes);
            // The container the stream wrote is the one playback loads.
            DemoPlayback playback;
            CHECK(demo_load(&playback, file_bytes(path), &error));
        }
    }
    std::error_code removed;
    std::filesystem::remove_all(scratch, removed);
}

// No recording this module writes holds a sector of network addresses, and a recording of
// another recorder that does still parses.
void no_written_recording_holds_a_player_address() {
    const auto scratch = oa::test::make_scratch_directory("demo-address");
    constexpr auto address_type = static_cast<uint32_t>(formats::tad::SectorType::player_address);
    DemoRecording whole;
    recording_begin(&whole, live_setup(3, false));
    add_sample_records(&whole, 4);
    const auto written = recording_write(whole);
    CHECK(written.ok());
    CHECK(!has_sector(written.bytes, address_type));
    const auto parsed = formats::tad::parse(written.bytes);
    CHECK(parsed.ok());
    // The recorder's line and the date are all this setup writes: no address, one for each of
    // three players, is among them.
    CHECK(parsed.ok() && parsed.demo->sectors.size() == 2);
    DemoRecording streamed;
    recording_begin(&streamed, live_setup(3, false));
    const auto path = scratch / "streamed.tad";
    std::string error;
    CHECK(recording_stream_to(&streamed, path, &error));
    add_sample_records(&streamed, 4);
    CHECK(streamed.stream->close(&error));
    CHECK(!has_sector(file_bytes(path), address_type));
    // A replay finished from that stream holds none either.
    const ReplayFacts facts{};
    auto skeleton = replay_begin(streamed.setup, facts);
    const auto finished = scratch / "streamed.oademo";
    const auto result =
        finish_as_replay(path, skeleton, ReplayEnd{"finished", {}, false}, finished);
    CHECK(result.ok);
    const auto replay = formats::oademo::read(file_bytes(finished));
    CHECK(replay.ok());
    if (replay.ok()) {
        CHECK(!has_sector(replay.replay->recording, address_type));
        CHECK(replay.replay->meta.recording.addresses_removed);
    }
    // A sector 6 in another recorder's file is still read: only writing stops.
    formats::tad::Demo other = *parsed.demo;
    const std::vector<uint8_t> masked{0x1a ^ 0x2a, 0x2a ^ 0x2a};
    std::vector<formats::tad::Sector> sectors = other.sectors;
    sectors.push_back({address_type, masked});
    other.sectors = sectors;
    const auto rewritten = formats::tad::write(other);
    CHECK(rewritten.ok());
    CHECK(has_sector(rewritten.bytes, address_type));
    std::error_code removed;
    std::filesystem::remove_all(scratch, removed);
}

// A recording finished after a crash is cut to its last whole packet, and the replay it
// becomes loads and says it was cut and finished after a crash.
void a_partial_recording_is_finished_into_a_file_that_loads() {
    const auto scratch = oa::test::make_scratch_directory("demo-partial");
    DemoRecording recording;
    recording_begin(&recording, live_setup(2, false));
    const auto stream_file = scratch / "game.oademo.part";
    std::string error;
    CHECK(recording_stream_to(&recording, stream_file, &error));
    add_sample_records(&recording, 60);
    const uint64_t added = recording.packet_count;
    CHECK(recording.stream->close(&error));
    const auto complete = file_bytes(stream_file);
    const auto header_bytes = recording_header(recording.setup).bytes.size();
    // The walker counts every packet of a whole stream, and one cut part way through a packet
    // loses only that packet; a cut in the part before the packets loses the recording.
    uint64_t packets = 0;
    CHECK(whole_packets_end(complete, &packets) == complete.size() && packets == added);
    CHECK(
        whole_packets_end(std::span(complete).first(complete.size() - 3), &packets) <
        complete.size()
    );
    CHECK(packets == added - 1);
    CHECK(whole_packets_end(std::span(complete).first(header_bytes), &packets) == header_bytes);
    CHECK(packets == 0);
    CHECK(whole_packets_end(std::span(complete).first(header_bytes - 1), &packets) == 0);
    // The crash cut the last packet in two.
    write_bytes(stream_file, std::span(complete).first(complete.size() - 3));
    ReplayFacts facts;
    facts.recorded_at = "2026-11-06T01:02:03Z";
    facts.recorded_by = "open-annihilation 0.9.1";
    facts.platform = "macos-arm64";
    facts.map_name = "Test Map";
    facts.players = {{"Ada", "human", true}, {"Bo", "computer", false}};
    auto skeleton = replay_begin(recording.setup, facts);
    const auto destination = scratch / "Recordings" / "default" / "game.oademo";
    ReplayEnd end;
    end.ended = "cut";
    end.after_crash = true;
    const auto result = finish_as_replay(stream_file, skeleton, end, destination);
    CHECK(result.ok && result.packets == added - 1 && !result.empty);
    CHECK(!std::filesystem::exists(stream_file));
    CHECK(std::filesystem::exists(destination));
    CHECK(!std::filesystem::exists(scratch / "game.oademo.part.writing"));
    const auto replay = formats::oademo::read(file_bytes(destination));
    CHECK(replay.ok());
    if (replay.ok()) {
        const auto& meta = replay.replay->meta;
        CHECK(meta.recorded.after_crash && meta.game.ended == "cut");
        CHECK(
            meta.recorded.at == "2026-11-06T01:02:03Z" && meta.recorded.local == "2026-10-03 08:00"
        );
        CHECK(meta.recorded.how == "multiplayer" && meta.game.kind == "multiplayer");
        CHECK(meta.recording.packets == added - 1 && meta.recording.addresses_removed);
        CHECK(
            meta.players.size() == 2 && meta.players[0].name == "Ada" &&
            meta.players[1].name == "Bo"
        );
        CHECK(meta.players[0].recorded_here == std::optional<bool>(true));
        CHECK(meta.players[1].kind == "computer" && meta.players[1].side == "core");
        CHECK(meta.units.count == 3 && meta.units.index_bits == 3);
        CHECK(replay.replay->manifest.engine.needs == ">=0.9.1");
        CHECK(
            replay.replay->manifest.needs.size() == 1 &&
            replay.replay->manifest.needs[0].kind == "base"
        );
        // The recording inside is the whole packets and plays.
        const auto inside = formats::tad::parse(replay.replay->recording);
        CHECK(inside.ok() && inside.demo->packets.size() == added - 1);
    }
    DemoPlayback playback;
    CHECK(demo_open(&playback, destination, &error));
    CHECK(playback.players.size() == 2);
    CHECK(!playback.frames.empty());

    // A recording that holds no packet is not made into a replay.
    DemoRecording nothing;
    recording_begin(&nothing, live_setup(2, false));
    const auto empty_stream = scratch / "nothing.oademo.part";
    CHECK(recording_stream_to(&nothing, empty_stream, &error));
    CHECK(nothing.stream->close(&error));
    const auto none = finish_as_replay(
        empty_stream, replay_begin(nothing.setup, facts), end, scratch / "nothing.oademo"
    );
    CHECK(!none.ok && none.empty && !std::filesystem::exists(scratch / "nothing.oademo"));
    CHECK(std::filesystem::exists(empty_stream));

    // A packet stored packed cannot be in a replay, whose packets are plain.
    DemoRecording packed;
    recording_begin(&packed, live_setup(2, true));
    const auto packed_stream = scratch / "packed.oademo.part";
    CHECK(recording_stream_to(&packed, packed_stream, &error));
    add_sample_records(&packed, 10);
    CHECK(packed.stream->close(&error));
    const auto refused = finish_as_replay(
        packed_stream, replay_begin(packed.setup, facts), end, scratch / "packed.oademo"
    );
    CHECK(!refused.ok && !refused.empty && !std::filesystem::exists(scratch / "packed.oademo"));

    // The recorder hack's layout is moved as it is, or cut to a whole packet.
    const auto tad_stream = scratch / "hack.tad.part";
    write_bytes(tad_stream, complete);
    const auto tad_destination = scratch / "Recordings" / "default" / "hack.tad";
    const auto moved = finish_as_tad(tad_stream, tad_destination, true);
    CHECK(moved.ok && moved.packets == added && file_bytes(tad_destination) == complete);
    CHECK(!std::filesystem::exists(tad_stream));
    write_bytes(tad_stream, std::span(complete).first(complete.size() - 3));
    const auto cut = finish_as_tad(tad_stream, tad_destination, false);
    CHECK(cut.ok && cut.packets == added - 1);
    const auto cut_bytes = file_bytes(tad_destination);
    const auto cut_parsed = formats::tad::parse(cut_bytes);
    CHECK(cut_parsed.ok() && cut_parsed.demo->packets.size() == added - 1);
    std::error_code removed;
    std::filesystem::remove_all(scratch, removed);
}

// A finished file never replaces another unless it is asked to: it takes the next free name, and says
// which, for a recording and for a replay alike.
void a_finished_file_replaces_another_only_when_asked() {
    const auto scratch = oa::test::make_scratch_directory("demo-place");
    std::string error;
    ReplayFacts facts;
    const auto make_stream = [&](const std::filesystem::path& path, uint32_t count) {
        DemoRecording recording;
        recording_begin(&recording, live_setup(2, false));
        CHECK(recording_stream_to(&recording, path, &error));
        add_sample_records(&recording, count);
        CHECK(recording.stream->close(&error));
        return recording.setup;
    };
    const auto taken = scratch / "Recordings" / "game.tad";
    std::filesystem::create_directories(scratch / "Recordings");
    write_bytes(taken, std::vector<uint8_t>{1, 2, 3});
    const auto first = scratch / "first.tad.part";
    make_stream(first, 4);
    const auto kept = finish_as_tad(first, taken, true, false);
    CHECK(kept.ok && kept.placed == scratch / "Recordings" / "game (2).tad");
    CHECK(file_bytes(taken) == std::vector<uint8_t>({1, 2, 3}));
    CHECK(std::filesystem::exists(kept.placed) && !std::filesystem::exists(first));
    // The next takes the next name, and nothing is replaced.
    const auto second = scratch / "second.tad.part";
    make_stream(second, 5);
    const auto next = finish_as_tad(second, taken, true, false);
    CHECK(next.ok && next.placed == scratch / "Recordings" / "game (3).tad");
    // Asked to replace, it takes the name.
    const auto third = scratch / "third.tad.part";
    make_stream(third, 6);
    const auto replaced = finish_as_tad(third, taken, true, true);
    CHECK(replaced.ok && replaced.placed == taken && file_bytes(taken).size() > 3);
    // A replay is placed the same way.
    const auto replay_stream = scratch / "replay.oademo.part";
    const auto setup = make_stream(replay_stream, 4);
    const auto replay_taken = scratch / "Recordings" / "game.oademo";
    write_bytes(replay_taken, std::vector<uint8_t>{9});
    const auto replay = finish_as_replay(
        replay_stream,
        replay_begin(setup, facts),
        ReplayEnd{"finished", {}, false},
        replay_taken,
        false
    );
    CHECK(replay.ok && replay.placed == scratch / "Recordings" / "game (2).oademo");
    CHECK(file_bytes(replay_taken) == std::vector<uint8_t>({9}));
    CHECK(formats::oademo::read(file_bytes(replay.placed)).ok());
    std::error_code removed;
    std::filesystem::remove_all(scratch, removed);
}

// What the game knows when a recording starts reaches the replay's description and manifest:
// the map, the mod and its digests, the mutators, the kind of game and the object an
// extension names for it, and what the recording itself holds.
void a_replay_describes_the_game_it_was_made_from() {
    DemoRecording recording;
    recording_begin(&recording, live_setup(2, false));
    ReplayFacts facts;
    facts.recorded_at = "2026-11-06T01:02:03Z";
    facts.recorded_by = "open-annihilation 0.9.1";
    facts.platform = "linux-x86_64";
    facts.engine_version = "0.9.1";
    facts.map_name = "Show Down";
    facts.game_kind = "battle";
    facts.game_kind_object = R"({"game_id":"5f2e9a0c41b7d3","planet":"Empyrrean"})";
    facts.expansions = {"core-contingency"};
    formats::oademo::ModRef mod;
    mod.id = "ridge";
    mod.name = "Ridge";
    mod.version = "2.8";
    mod.revision = 1;
    mod.sim_hash = std::string(64, 'a');
    facts.mod = mod;
    formats::oademo::Need need;
    need.kind = "oamod";
    need.id = "ridge";
    need.version = "2.8";
    need.revision = 1;
    need.sha256 = std::string(64, 'b');
    need.sim_hash = mod.sim_hash;
    facts.packages.push_back(need);
    const auto replay = replay_begin(recording.setup, facts);
    CHECK(replay.meta.recorded.by == std::optional<std::string>("open-annihilation 0.9.1"));
    CHECK(replay.meta.recorded.platform == std::optional<std::string>("linux-x86_64"));
    CHECK(replay.meta.game.kind == "battle" && replay.meta.game.kind_object.has_value());
    CHECK(replay.meta.map.name == "Show Down" && replay.meta.content.mod.has_value());
    const auto text = formats::oademo::meta_text(replay.meta);
    CHECK(text.find(R"("game":{"kind":"battle")") != std::string::npos);
    CHECK(
        text.find(R"("battle":{"game_id":"5f2e9a0c41b7d3","planet":"Empyrrean"})") !=
        std::string::npos
    );
    // The manifest asks for the base game, the expansion and the mod, in that order, and the mod
    // takes the unit table's digest from the recording.
    const auto& needs = replay.manifest.needs;
    CHECK(needs.size() == 3 && needs[0].kind == "base" && needs[1].kind == "expansion");
    CHECK(needs.size() == 3 && needs[2].kind == "oamod" && needs[2].sha256 == need.sha256);
    CHECK(
        needs.size() == 3 &&
        needs[2].units_table_sha256 == std::optional<std::string>(replay.meta.units.table_sha256)
    );
    CHECK(replay.manifest.engine.recorded_with == "0.9.1");
    // Its recording is the header alone, which a replay can hold.
    CHECK(replay.meta.recording.packets == 0 && !replay.recording.empty());
    CHECK(formats::oademo::write(replay).ok());
}

// Ten recorded players leave no slot for the viewer: refused by 3.1c's
// rules, watched through the first recorded player's slot under the
// ten-player rule.
void ten_players_watch_without_a_slot() {
    DemoRecording recording;
    recording_begin(&recording, live_setup(OA_PLAYER_COUNT, false));
    recording_add(&recording, 0x100, 0, moving_state(1));
    const auto written = recording_write(recording);
    CHECK(written.ok());
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, written.bytes, &error));
    World* world = make_world();
    uint8_t watcher = 0xff;
    CHECK(!demo_watches_without_slot(playback));
    CHECK(!demo_bind_players(playback, world, &watcher));
    playback.ten_player_replay = TenPlayerReplay::watcher_view;
    CHECK(demo_watches_without_slot(playback));
    CHECK(demo_bind_players(playback, world, &watcher));
    CHECK(watcher == OA_PLAYER_COUNT);
    CHECK(world->game.local_player_index == 0 && world->game.viewpoint_player == 0);
    CHECK(world->game.player_count == OA_PLAYER_COUNT);
    bool all_remote = true;
    for (const auto& player : world->game.players)
        all_remote = all_remote && player.in_use == 1 && player.status == OA_PLAYER_STATUS_MIRRORED;
    CHECK(all_remote);
    CHECK(demo_slotless_viewer(playback) == sim::match_runtime::SlotlessViewer::watcher);
    playback.ten_player_replay = TenPlayerReplay::allied_fake_player;
    CHECK(
        demo_slotless_viewer(playback) == sim::match_runtime::SlotlessViewer::ally_of_every_player
    );
    playback.ten_player_replay = TenPlayerReplay::off;
    CHECK(demo_slotless_viewer(playback) == sim::match_runtime::SlotlessViewer::none);
    world_destroy(world);
}

// A recording with a loadout text writes that text as the mutators sector.
// An empty text writes none, and the file matches a recording that never
// carried one. A text one byte past one sector chunk is left out, and the
// recording still parses.
void recording_carries_loadout_text() {
    const std::string text = "ridge-pace 1";
    DemoRecording with_text;
    recording_begin(&with_text, live_setup(2, false));
    with_text.setup.loadout_text = text;
    const auto written = recording_write(with_text);
    CHECK(written.ok());
    const auto parsed = formats::tad::parse(written.bytes);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        const auto* sector =
            formats::tad::find_sector(*parsed.demo, formats::tad::SectorType::mutators);
        CHECK(sector != nullptr);
        if (sector != nullptr) {
            const std::string back(
                reinterpret_cast<const char*>(sector->data.data()), sector->data.size()
            );
            CHECK(back == text);
            CHECK(formats::tad::sector_text(*sector) == text);
        }
    }

    DemoRecording plain;
    recording_begin(&plain, live_setup(2, false));
    const auto plain_written = recording_write(plain);
    CHECK(plain_written.ok());
    const auto plain_parsed = formats::tad::parse(plain_written.bytes);
    CHECK(plain_parsed.ok());
    if (plain_parsed.ok() && parsed.ok()) {
        CHECK(
            formats::tad::find_sector(*plain_parsed.demo, formats::tad::SectorType::mutators) ==
            nullptr
        );
        CHECK(plain_parsed.demo->sectors.size() + 1 == parsed.demo->sectors.size());
    }
    DemoRecording empty_text;
    recording_begin(&empty_text, live_setup(2, false));
    empty_text.setup.loadout_text.clear();
    const auto empty_written = recording_write(empty_text);
    CHECK(empty_written.ok() && empty_written.bytes == plain_written.bytes);

    DemoRecording exact;
    recording_begin(&exact, live_setup(2, false));
    const std::string full(recording_loadout_most_bytes, 'q');
    exact.setup.loadout_text = full;
    const auto exact_written = recording_write(exact);
    CHECK(exact_written.ok());
    const auto exact_parsed = formats::tad::parse(exact_written.bytes);
    CHECK(exact_parsed.ok());
    if (exact_parsed.ok()) {
        const auto* sector =
            formats::tad::find_sector(*exact_parsed.demo, formats::tad::SectorType::mutators);
        CHECK(sector != nullptr && formats::tad::sector_text(*sector) == full);
    }

    DemoRecording too_long;
    recording_begin(&too_long, live_setup(2, false));
    too_long.setup.loadout_text.assign(recording_loadout_most_bytes + 1, 'q');
    const auto omitted = recording_write(too_long);
    CHECK(omitted.ok() && omitted.bytes == empty_written.bytes);
    const auto omitted_parsed = formats::tad::parse(omitted.bytes);
    CHECK(omitted_parsed.ok());
    if (omitted_parsed.ok()) {
        CHECK(
            formats::tad::find_sector(*omitted_parsed.demo, formats::tad::SectorType::mutators) ==
            nullptr
        );
    }
}

// A recording with brains text writes that text as the brains sector, after
// the mutators' sector, and reads it back byte for byte. An empty text writes
// none, and the file matches a recording that never carried one. A text one
// byte past one sector chunk is left out, and the recording still parses.
void recording_carries_brains_text() {
    const std::string text = "{\"seats\":[{\"brain\":\"steady-commander\",\"place\":1}]}";
    DemoRecording with_text;
    recording_begin(&with_text, live_setup(2, false));
    with_text.setup.brains_text = text;
    const auto written = recording_write(with_text);
    CHECK(written.ok());
    const auto parsed = formats::tad::parse(written.bytes);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        const auto* sector =
            formats::tad::find_sector(*parsed.demo, formats::tad::SectorType::brains);
        CHECK(sector != nullptr);
        if (sector != nullptr) {
            const std::string back(
                reinterpret_cast<const char*>(sector->data.data()), sector->data.size()
            );
            CHECK(back == text);
            CHECK(formats::tad::sector_text(*sector) == text);
        }
    }

    DemoRecording plain;
    recording_begin(&plain, live_setup(2, false));
    const auto plain_written = recording_write(plain);
    CHECK(plain_written.ok());
    DemoRecording empty_text;
    recording_begin(&empty_text, live_setup(2, false));
    empty_text.setup.brains_text.clear();
    const auto empty_written = recording_write(empty_text);
    CHECK(empty_written.ok() && empty_written.bytes == plain_written.bytes);
    const auto empty_parsed = formats::tad::parse(empty_written.bytes);
    CHECK(empty_parsed.ok());
    if (empty_parsed.ok()) {
        CHECK(
            formats::tad::find_sector(*empty_parsed.demo, formats::tad::SectorType::brains) ==
            nullptr
        );
    }

    DemoRecording both;
    recording_begin(&both, live_setup(2, false));
    both.setup.loadout_text = "ridge-pace 1";
    both.setup.brains_text = text;
    const auto both_written = recording_write(both);
    CHECK(both_written.ok());
    const auto both_parsed = formats::tad::parse(both_written.bytes);
    CHECK(both_parsed.ok());
    if (both_parsed.ok()) {
        int mutators_at = -1;
        int brains_at = -1;
        const auto& sectors = both_parsed.demo->sectors;
        for (std::size_t index = 0; index < sectors.size(); ++index) {
            if (sectors[index].type == static_cast<uint32_t>(formats::tad::SectorType::mutators))
                mutators_at = static_cast<int>(index);
            if (sectors[index].type == static_cast<uint32_t>(formats::tad::SectorType::brains))
                brains_at = static_cast<int>(index);
        }
        CHECK(mutators_at >= 0 && brains_at == mutators_at + 1);
    }
    DemoRecording loadout_only;
    recording_begin(&loadout_only, live_setup(2, false));
    loadout_only.setup.loadout_text = "ridge-pace 1";
    const auto loadout_written = recording_write(loadout_only);
    CHECK(loadout_written.ok() && loadout_written.bytes != both_written.bytes);

    DemoRecording exact;
    recording_begin(&exact, live_setup(2, false));
    const std::string full(recording_brains_most_bytes, 'q');
    exact.setup.brains_text = full;
    const auto exact_written = recording_write(exact);
    CHECK(exact_written.ok());
    const auto exact_parsed = formats::tad::parse(exact_written.bytes);
    CHECK(exact_parsed.ok());
    if (exact_parsed.ok()) {
        const auto* sector =
            formats::tad::find_sector(*exact_parsed.demo, formats::tad::SectorType::brains);
        CHECK(sector != nullptr && sector->data.size() == full.size());
        CHECK(sector != nullptr && formats::tad::sector_text(*sector) == full);
    }

    DemoRecording too_long;
    recording_begin(&too_long, live_setup(2, false));
    too_long.setup.brains_text.assign(recording_brains_most_bytes + 1, 'q');
    const auto omitted = recording_write(too_long);
    CHECK(omitted.ok() && omitted.bytes == empty_written.bytes);
    const auto omitted_parsed = formats::tad::parse(omitted.bytes);
    CHECK(omitted_parsed.ok());
    if (omitted_parsed.ok()) {
        CHECK(
            formats::tad::find_sector(*omitted_parsed.demo, formats::tad::SectorType::brains) ==
            nullptr
        );
    }
}

// A chat record from a recorded player.
std::vector<uint8_t> chat_payload(const char* text) {
    std::vector<uint8_t> payload{formats::tad::payload_marker};
    ChatRecord chat{};
    std::snprintf(chat.text, sizeof chat.text, "%s", text);
    append_record(&payload, chat);
    return payload;
}

// A slotless viewer of ten recorded players sees the whole map, the match
// learns how it sees, and it is shown the recorded chat, which the pump shows
// only to a local slot. A seated watcher is shown each line once.
void slotless_viewer_watches_the_recording() {
    for (const auto form : {TenPlayerReplay::watcher_view, TenPlayerReplay::allied_fake_player}) {
        auto recording = make_recording(kUnitsPerPlayer, OA_PLAYER_COUNT);
        recording.add(4, 33, chat_payload("player4: hello"));
        EconomyRecord economy{};
        economy.energy_produced_total = 50.0f;
        std::vector<uint8_t> payload{formats::tad::payload_marker};
        append_record(&payload, economy);
        recording.add(3, 33, payload);
        const auto release = [](DemoSession* session) {
            demo_session_end(session);
            delete session;
        };
        std::unique_ptr<DemoSession, decltype(release)> session(new DemoSession(), release);
        std::string error;
        CHECK(demo_load(&session->playback, recording.bytes(), &error));
        session->playback.ten_player_replay = form;
        Machine replay;
        replay.build(0);
        std::array<uint8_t, OA_PLAYER_COUNT> allies{};
        allies[0] = 1;
        replay.match->configure_outcomes(0, allies, false);
        CHECK(demo_session_begin(session.get(), replay.match.get(), &error));
        if (session->match == nullptr) {
            std::fprintf(stderr, "slotless session: %s\n", error.c_str());
            return;
        }
        CHECK(session->watcher_slot == OA_PLAYER_COUNT);
        const auto& game = replay.match->state().game;
        CHECK((game.visibility_flags & sim::visibility_state::update_sight_grid) == 0);
        CHECK((game.visibility_flags & sim::visibility_state::altitude_sight_algorithm) != 0);
        CHECK(replay.match->slotless_viewer() == demo_slotless_viewer(session->playback));
        CHECK(replay.match->slotless_full_radar());
        for (int t = 0; t < 10; ++t)
            demo_session_frame(session.get());
        CHECK(session->tick_errors == 0);
        CHECK(std::count(session->lines.begin(), session->lines.end(), "player4: hello") == 1);
        // Only watcher-view follows the recorded economy.
        const bool followed = form == TenPlayerReplay::watcher_view;
        CHECK(session->economy_samples[2].taken == followed);
        CHECK(session->economy_samples[2].energy_produced_total == (followed ? 50.0f : 0.0f));
    }
    auto recording = make_recording(kUnitsPerPlayer, 1);
    recording.add(1, 33, chat_payload("player1: hello"));
    const auto release = [](DemoSession* session) {
        demo_session_end(session);
        delete session;
    };
    std::unique_ptr<DemoSession, decltype(release)> session(new DemoSession(), release);
    std::string error;
    CHECK(demo_load(&session->playback, recording.bytes(), &error));
    session->playback.ten_player_replay = TenPlayerReplay::watcher_view;
    Machine replay;
    replay.build(1);
    std::array<uint8_t, OA_PLAYER_COUNT> watcher_allies{};
    watcher_allies[1] = 1;
    replay.match->configure_outcomes(1, watcher_allies, false);
    CHECK(demo_session_begin(session.get(), replay.match.get(), &error));
    if (session->match == nullptr)
        return;
    CHECK(replay.match->slotless_viewer() == sim::match_runtime::SlotlessViewer::none);
    for (int t = 0; t < 10; ++t)
        demo_session_frame(session.get());
    CHECK(std::count(session->lines.begin(), session->lines.end(), "player1: hello") == 1);
}

// A recording keeps a line in UTF-8 when its speaker's block says UTF-8
// chat: a viewer with Unicode chat on is shown it as it is, one with it
// off in the code page, '?' for each hanzi.
void recorded_unicode_chat_plays_back() {
    // U+4F60 U+597D in UTF-8.
    const std::string line = "player1: \xe4\xbd\xa0\xe5\xa5\xbd";
    for (const bool viewer_utf8 : {true, false}) {
        auto recording = make_recording(kUnitsPerPlayer, 1);
        PlayerSetupInfo info{};
        info.player_id = recorded_id(0);
        info.side = 0;
        info.color = recorded_color(0);
        info.role = 1;
        info.options = recorded_host_options;
        mark_unicode_chat(reinterpret_cast<uint8_t*>(&info), true);
        recording.status_datagrams[0] = status_datagram(info);
        recording.add(1, 33, chat_payload(line.c_str()));
        const auto release = [](DemoSession* session) {
            demo_session_end(session);
            delete session;
        };
        std::unique_ptr<DemoSession, decltype(release)> session(new DemoSession(), release);
        std::string error;
        CHECK(demo_load(&session->playback, recording.bytes(), &error));
        session->playback.ten_player_replay = TenPlayerReplay::watcher_view;
        session->unicode_chat = viewer_utf8;
        Machine replay;
        replay.build(1);
        std::array<uint8_t, OA_PLAYER_COUNT> watcher_allies{};
        watcher_allies[1] = 1;
        replay.match->configure_outcomes(1, watcher_allies, false);
        CHECK(demo_session_begin(session.get(), replay.match.get(), &error));
        if (session->match == nullptr)
            return;
        for (int t = 0; t < 10; ++t)
            demo_session_frame(session.get());
        const std::string shown = viewer_utf8 ? line : "player1: ??";
        CHECK(std::count(session->lines.begin(), session->lines.end(), shown) == 1);
    }
}

// A recorded alliance line keeps the English it went out in and reads in
// the viewer's language, the names as they were.
void recorded_alliance_line_reads_in_the_language_shown() {
    const std::string line = "<player1>  allied with player2";
    auto recording = make_recording(kUnitsPerPlayer, 1);
    recording.add(1, 33, chat_payload(line.c_str()));
    const auto release = [](DemoSession* session) {
        demo_session_end(session);
        delete session;
    };
    std::unique_ptr<DemoSession, decltype(release)> session(new DemoSession(), release);
    std::string error;
    CHECK(demo_load(&session->playback, recording.bytes(), &error));
    session->playback.ten_player_replay = TenPlayerReplay::watcher_view;
    session->translate_game_text = [](void*, const char* english) -> const char* {
        return std::string_view(english) == "allied with" ? "Verb\xc3\xbcndet mit" : nullptr;
    };
    Machine replay;
    replay.build(1);
    std::array<uint8_t, OA_PLAYER_COUNT> watcher_allies{};
    watcher_allies[1] = 1;
    replay.match->configure_outcomes(1, watcher_allies, false);
    CHECK(demo_session_begin(session.get(), replay.match.get(), &error));
    if (session->match == nullptr)
        return;
    for (int t = 0; t < 10; ++t)
        demo_session_frame(session.get());
    CHECK(
        std::count(
            session->lines.begin(), session->lines.end(), "<player1>  Verb\xc3\xbcndet mit player2"
        ) == 1
    );
    CHECK(std::count(session->lines.begin(), session->lines.end(), line) == 0);
}

// A recorded speed change reads as the viewer's language says it: the
// session's translation reaches the match, which builds the line from the
// game's own words.
void recorded_speed_reads_in_the_language_shown() {
    auto recording = make_recording(kUnitsPerPlayer, 1);
    std::vector<uint8_t> payload{formats::tad::payload_marker};
    PauseSpeedRecord speed{};
    speed.kind = 1;
    speed.value = 11;
    append_record(&payload, speed);
    recording.add(1, 33, payload);
    recording.add(9, 33, probe_payload());
    const auto release = [](DemoSession* session) {
        demo_session_end(session);
        delete session;
    };
    std::unique_ptr<DemoSession, decltype(release)> session(new DemoSession(), release);
    std::string error;
    CHECK(demo_load(&session->playback, recording.bytes(), &error));
    static const std::string tempo = "Tempo";
    session->translate_game_text = [](void*, const char* english) -> const char* {
        return std::string_view(english) == "Game Speed" ? tempo.c_str() : nullptr;
    };
    Machine replay;
    replay.build(1);
    std::array<uint8_t, OA_PLAYER_COUNT> watcher_allies{};
    watcher_allies[1] = 1;
    replay.match->configure_outcomes(1, watcher_allies, false);
    CHECK(demo_session_begin(session.get(), replay.match.get(), &error));
    if (session->match == nullptr)
        return;
    for (int t = 0; t < 10; ++t)
        demo_session_frame(session.get());
    CHECK(std::count(session->lines.begin(), session->lines.end(), "Tempo  +1\n") == 1);
}

// Economy records give a player simulated elsewhere the production and use
// per settlement its running totals imply, from the second record on.
void economy_records_give_income_figures() {
    Player player{};
    player.energy_produced = 7.0f;
    EconomySample sample{};
    EconomyRecord record{};
    record.energy_produced_total = 100.0f;
    record.energy_requested_total = 40.0f;
    record.metal_produced_total = 10.0f;
    record.metal_requested_total = 4.0f;
    demo_follow_economy(&sample, &player, record, 90);
    CHECK(sample.taken && sample.tick == 90);
    CHECK(player.energy_produced == 7.0f);
    // 120 ticks later: four settlements.
    record.energy_produced_total = 500.0f;
    record.energy_requested_total = 240.0f;
    record.metal_produced_total = 50.0f;
    record.metal_requested_total = 24.0f;
    demo_follow_economy(&sample, &player, record, 210);
    CHECK(player.energy_produced == 100.0f && player.energy_requested == 50.0f);
    CHECK(player.metal_produced == 10.0f && player.metal_requested == 5.0f);
    // Another record at the same tick changes nothing.
    record.energy_produced_total = 900.0f;
    demo_follow_economy(&sample, &player, record, 210);
    CHECK(player.energy_produced == 100.0f && sample.energy_produced_total == 500.0f);
}

// The limits sector is written after the brains sector and read back. A
// setup that does not carry limits writes no such sector, and the same
// sector list as a recording that never had the field.
void recording_carries_limits() {
    const std::string brains = "steady-commander 1";
    const formats::tad::RecordedLimits recorded{1024, 10};
    DemoRecording with_limits;
    recording_begin(&with_limits, live_setup(2, false));
    with_limits.setup.brains_text = brains;
    with_limits.setup.limits = recorded;
    const auto written = recording_write(with_limits);
    CHECK(written.ok());
    const auto parsed = formats::tad::parse(written.bytes);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        int brains_at = -1;
        int limits_at = -1;
        const auto& sectors = parsed.demo->sectors;
        for (std::size_t index = 0; index < sectors.size(); ++index) {
            if (sectors[index].type == static_cast<uint32_t>(formats::tad::SectorType::brains))
                brains_at = static_cast<int>(index);
            if (sectors[index].type == static_cast<uint32_t>(formats::tad::SectorType::limits))
                limits_at = static_cast<int>(index);
        }
        CHECK(brains_at >= 0 && limits_at == brains_at + 1);
        const auto* sector =
            formats::tad::find_sector(*parsed.demo, formats::tad::SectorType::limits);
        CHECK(sector != nullptr);
        if (sector != nullptr) {
            const auto back = formats::tad::decode_limits_sector(*sector);
            CHECK(back && *back == recorded);
        }
    }

    DemoRecording plain;
    recording_begin(&plain, live_setup(2, false));
    const auto plain_written = recording_write(plain);
    CHECK(plain_written.ok());
    DemoRecording absent;
    recording_begin(&absent, live_setup(2, false));
    absent.setup.limits.reset();
    const auto absent_written = recording_write(absent);
    CHECK(absent_written.ok() && absent_written.bytes == plain_written.bytes);
    const auto plain_parsed = formats::tad::parse(plain_written.bytes);
    const auto absent_parsed = formats::tad::parse(absent_written.bytes);
    CHECK(plain_parsed.ok() && absent_parsed.ok());
    if (plain_parsed.ok() && absent_parsed.ok()) {
        CHECK(
            formats::tad::find_sector(*absent_parsed.demo, formats::tad::SectorType::limits) ==
            nullptr
        );
        CHECK(absent_parsed.demo->sectors.size() == plain_parsed.demo->sectors.size());
        for (std::size_t index = 0; index < plain_parsed.demo->sectors.size(); ++index)
            CHECK(
                absent_parsed.demo->sectors[index].type == plain_parsed.demo->sectors[index].type
            );
    }
}

// The container's wide lengths are the wire's.
void tad_wide_lengths_match_the_wire() {
    CHECK(formats::tad::wide_record_lengths.size() == wide_record_type_count);
    for (std::size_t index = 0; index < formats::tad::wide_record_lengths.size(); ++index)
        CHECK(formats::tad::wide_record_lengths[index] == wide_record_length_table[index]);
}

// A 0xE0 payload whose weapon id is 300.
std::vector<uint8_t> wide_fire_payload() {
    WideWeaponFireRecord fire{};
    fire.weapon_id = 300;
    std::array<uint8_t, wide_record_length_table[0]> bytes{};
    std::size_t written = 0;
    CHECK(encode_record(fire, bytes.data(), bytes.size(), &written) == WireError::ok);
    std::vector<uint8_t> payload{formats::tad::payload_marker};
    payload.insert(
        payload.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(written)
    );
    return payload;
}

// Loads a one-player recording, optionally carrying a limits sector, with one
// wide weapon record, and plays it. Returns how many wide records the binding
// applied.
uint32_t applied_wide_records(std::optional<formats::tad::RecordedLimits> limits) {
    auto recording = make_recording(kUnitsPerPlayer, 1);
    const auto encoded = limits ? formats::tad::encode_limits_sector(*limits)
                                : std::array<uint8_t, formats::tad::limits_sector_bytes>{};
    if (limits) {
        recording.demo.sectors.push_back(
            {static_cast<uint32_t>(formats::tad::SectorType::limits), encoded}
        );
    }
    recording.add(1, 33, wide_fire_payload());
    const auto release = [](DemoSession* session) {
        demo_session_end(session);
        delete session;
    };
    std::unique_ptr<DemoSession, decltype(release)> session(new DemoSession(), release);
    std::string error;
    CHECK(demo_load(&session->playback, recording.bytes(), &error));
    const bool wide = limits && limits->weapon_types > formats::tad::recorded_base_weapon_types;
    CHECK(session->playback.stats.wide_records == (wide ? 1u : 0u));
    CHECK(session->playback.frames.size() == (wide ? 1u : 0u));
    if (wide && !session->playback.frames.empty()) {
        const auto unwrapped =
            oa::netgame::network::unwrap_frame(session->playback.frames[0].datagram);
        CHECK(unwrapped.ok());
        if (unwrapped.ok()) {
            const auto& frame = *unwrapped.value;
            CHECK(frame.size() >= frame_header_bytes + 27);
            if (frame.size() >= frame_header_bytes + 27) {
                CHECK(frame[frame_header_bytes] == 0xE0);
                const auto id = static_cast<uint16_t>(
                    frame[frame_header_bytes + 25] |
                    (static_cast<uint16_t>(frame[frame_header_bytes + 26]) << 8)
                );
                CHECK(id == 300);
            }
        }
    }
    Machine replay;
    replay.build(1);
    CHECK(demo_session_begin(session.get(), replay.match.get(), &error));
    if (session->match == nullptr)
        return 0;
    for (int tick = 0; tick < 10; ++tick)
        demo_session_frame(session.get());
    CHECK(session->tick_errors == 0);
    return session->net.wide_records_applied;
}

// A recording with the sector (1024, 10) delivers a 0xE0 of id 300 to the
// binding. The same packet without the sector, or with weapon types left at
// 256, does not.
void wide_records_reach_the_binding() {
    CHECK(applied_wide_records(formats::tad::RecordedLimits{1024, 10}) == 1);
    CHECK(applied_wide_records(std::nullopt) == 0);
    CHECK(applied_wide_records(formats::tad::RecordedLimits{256, 7}) == 0);
}

DemoPlayback playback_with(std::optional<std::vector<uint8_t>> limits_body) {
    auto recording = make_recording(kUnitsPerPlayer, 1);
    if (limits_body) {
        recording.demo.sectors.push_back(
            {static_cast<uint32_t>(formats::tad::SectorType::limits), *limits_body}
        );
    }
    DemoPlayback playback;
    std::string error;
    CHECK(demo_load(&playback, recording.bytes(), &error));
    return playback;
}

// Each verdict, including a sides-only sector.
void limits_verdicts() {
    const auto encoded = [](formats::tad::RecordedLimits limits) {
        const auto bytes = formats::tad::encode_limits_sector(limits);
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    };
    const auto raised = playback_with(encoded({1024, 10}));
    CHECK(demo_limits_verdict(raised, 1024, 10) == DemoLimitsVerdict::plays);
    CHECK(demo_limits_verdict(raised, 256, 5) == DemoLimitsVerdict::other_limits);
    CHECK(demo_limits_verdict(raised, 1024, 7) == DemoLimitsVerdict::other_limits);

    const auto stock = playback_with(std::nullopt);
    CHECK(demo_limits_verdict(stock, 256, 5) == DemoLimitsVerdict::plays);
    CHECK(demo_limits_verdict(stock, 1024, 5) == DemoLimitsVerdict::base_limits);
    CHECK(demo_limits_verdict(stock, 256, 7) == DemoLimitsVerdict::base_limits);

    const auto sides_only = playback_with(encoded({256, 7}));
    CHECK(demo_limits_verdict(sides_only, 256, 7) == DemoLimitsVerdict::plays);
    CHECK(demo_limits_verdict(sides_only, 256, 5) == DemoLimitsVerdict::other_limits);
    CHECK(sides_only.stats.wide_records == 0);

    const auto broken = playback_with(std::vector<uint8_t>(7, 0));
    CHECK(broken.limits_unreadable);
    CHECK(!broken.limits);
    CHECK(demo_limits_verdict(broken, 256, 5) == DemoLimitsVerdict::unreadable);
    CHECK(demo_limits_verdict(broken, 1024, 10) == DemoLimitsVerdict::unreadable);
}

// The same recording, as a .tad and as a .oademo, loads to the same playback.
void oademo_loads_the_same_playback() {
    auto recording = make_recording(kUnitsPerPlayer, 2);
    recording.add(1, 0, probe_payload());
    recording.add(1, 40, elided_state_payload(5, true));
    recording.add(2, 10, probe_payload());
    const std::vector<uint8_t> tad_bytes = recording.bytes();

    formats::oademo::Replay replay;
    replay.recording = tad_bytes;
    formats::oademo::set_recording_identity(replay);
    replay.meta.recording.tad_version = formats::tad::supported_version;
    replay.meta.recording.packets = 3;
    replay.meta.recording.addresses_removed = true;
    replay.meta.recorded.how = "multiplayer";
    replay.meta.game.kind = "ridge";
    replay.meta.map.name = "test";
    replay.meta.content.base.id = "totala";
    replay.meta.content.base.version = "3.1c";
    replay.meta.units.count = 1;
    replay.meta.units.table_sha256 = std::string(64, '0');
    replay.meta.units.index_bits = 2;
    formats::oademo::PlayerInfo player;
    player.number = 1;
    player.name = "player1";
    player.side = "arm";
    player.colour = 1;
    player.kind = "human";
    replay.meta.players.push_back(player);
    player.number = 2;
    player.name = "player2";
    player.side = "core";
    player.colour = 2;
    replay.meta.players.push_back(std::move(player));
    replay.manifest.engine.recorded_with = "0.8.14";
    replay.manifest.engine.needs = ">=0.8.14";
    formats::oademo::Need need;
    need.kind = "base";
    need.id = "totala";
    need.version = "3.1c";
    replay.manifest.needs.push_back(std::move(need));

    const formats::oademo::WriteResult written = formats::oademo::write(replay);
    if (!written.ok())
        std::fprintf(stderr, "oademo write: %s\n", written.error.c_str());
    CHECK(written.ok());
    CHECK(formats::oademo::recognised(written.bytes));
    CHECK(demo_recognised(written.bytes));

    DemoPlayback from_tad;
    DemoPlayback from_oademo;
    std::string error;
    CHECK(demo_load(&from_tad, tad_bytes, &error));
    CHECK(error.empty());
    error.clear();
    CHECK(demo_load(&from_oademo, written.bytes, &error));
    if (!error.empty())
        std::fprintf(stderr, "oademo load: %s\n", error.c_str());
    CHECK(error.empty());
    CHECK(from_oademo.file == tad_bytes);
    CHECK(from_tad.players.size() == from_oademo.players.size());
    for (std::size_t index = 0; index < from_tad.players.size(); ++index) {
        CHECK(from_tad.players[index].from_status == from_oademo.players[index].from_status);
        CHECK(from_tad.players[index].info.player_id == from_oademo.players[index].info.player_id);
        CHECK(from_tad.players[index].info.side == from_oademo.players[index].info.side);
        CHECK(from_tad.players[index].info.color == from_oademo.players[index].info.color);
        // The lobby block a tool reads is the one playback bound.
        const RecordedPlayer rebound =
            demo_recorded_player(from_tad.demo, from_tad.demo.players[index]);
        CHECK(rebound.from_status == from_tad.players[index].from_status);
        CHECK(rebound.info.side == from_tad.players[index].info.side);
        CHECK(rebound.info.color == from_tad.players[index].info.color);
        CHECK(rebound.info.player_id == from_tad.players[index].info.player_id);
        CHECK(from_tad.demo.players[index].name == from_oademo.demo.players[index].name);
        CHECK(from_tad.demo.players[index].number == from_oademo.demo.players[index].number);
    }
    CHECK(from_tad.frames.size() == from_oademo.frames.size());
    for (std::size_t index = 0; index < from_tad.frames.size(); ++index) {
        CHECK(from_tad.frames[index].datagram == from_oademo.frames[index].datagram);
        CHECK(from_tad.frames[index].time_ms == from_oademo.frames[index].time_ms);
        CHECK(from_tad.frames[index].sender == from_oademo.frames[index].sender);
        CHECK(from_tad.frames[index].due_tick == from_oademo.frames[index].due_tick);
    }
    CHECK(from_tad.recorded_definitions == from_oademo.recorded_definitions);
    CHECK(from_tad.recorded_verdicts == from_oademo.recorded_verdicts);
    CHECK(from_tad.unit_def_bits == from_oademo.unit_def_bits);
    const std::array<uint32_t, 1> keys{1};
    const UnitTableCheck tad_check = demo_check_unit_table(from_tad, keys, nullptr);
    const UnitTableCheck oademo_check = demo_check_unit_table(from_oademo, keys, nullptr);
    CHECK(tad_check.recorded == oademo_check.recorded);
    CHECK(tad_check.matched == oademo_check.matched);
    CHECK(tad_check.missing == oademo_check.missing);
    CHECK(tad_check.extra == oademo_check.extra);
    CHECK(tad_check.identical == oademo_check.identical);
}

bool write_into_memory(void* context, uint64_t offset, std::span<const uint8_t> bytes) {
    auto& out = *static_cast<std::vector<uint8_t>*>(context);
    if (offset > out.size())
        return false;
    const std::size_t at = static_cast<std::size_t>(offset);
    if (out.size() < at + bytes.size())
        out.resize(at + bytes.size());
    std::copy(bytes.begin(), bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
}

// A zip whose first entry is the stored `oademo` line of version 2 is a
// replay this game does not read, and loading it says so.
void a_replay_of_another_version_says_so() {
    std::vector<uint8_t> file;
    formats::zip::StreamWriter writer({&file, write_into_memory}, {});
    formats::zip::ZipError problem;
    const auto put = [&](std::string_view name, std::string_view text, formats::zip::Method how) {
        const std::span<const uint8_t> bytes{
            reinterpret_cast<const uint8_t*>(text.data()), text.size()
        };
        return writer.begin_entry(name, bytes.size(), how, problem) &&
               writer.write(bytes, problem) && writer.end_entry(problem);
    };
    CHECK(
        put("oademo", "oademo 2\n", formats::zip::Method::stored) &&
        put("meta.json", "{}", formats::zip::Method::deflated) &&
        put("manifest.json", "{}", formats::zip::Method::deflated) &&
        put("recording.tad.zst", "x", formats::zip::Method::stored) && writer.finish(problem)
    );
    CHECK(formats::oademo::recognised(file));
    CHECK(demo_recognised(file));
    DemoPlayback playback;
    std::string error;
    CHECK(!demo_load(&playback, file, &error));
    CHECK(error == formats::oademo::not_this_version);
}

// A jump gives the viewer its pause, camera, selection and the other fields it set, and the chat
// ring is emptied so the recording does not post the same lines again.
void a_jump_gives_the_viewer_its_picture_back() {
    World* world = world_create();
    CHECK(world != nullptr);
    if (world == nullptr)
        return;
    Unit units[4]{};
    world->units = units;
    world->unit_slot_count = 4;
    units[0].type_index = 0;
    units[0].flags = OA_UNIT_FLAG_SELECTED;
    units[1].type_index = 1;
    units[1].flags = OA_UNIT_FLAG_SELECTED | OA_UNIT_FLAG_SELECTABLE;
    units[2].type_index = 2;
    units[3].type_index = 3;
    units[3].flags = OA_UNIT_FLAG_LIVE;
    Game& game = world->game;
    game.sim_run_flags = 0x0011;
    game.viewpoint_player = 3;
    game.local_player_index = 2;
    game.follow_unit = 11;
    game.follow_target = 12;
    game.follow_point = FixedVec3{1, 2, 3};
    game.follow_point_ticks = 7;
    game.camera_slot_x[2] = 40;
    game.camera_slot_y[2] = 50;
    game.camera_slot_valid[2] = 1;
    game.debug_overlay = 4;
    game.chat_lines[0][0] = 'x';
    game.chat_head = 4;
    game.chat_tail = 2;

    ViewerPicture picture;
    take_viewer_picture(*world, &picture);

    game.sim_run_flags = 0x0010;
    game.viewpoint_player = 0;
    game.local_player_index = 0;
    game.follow_unit = 0;
    game.follow_target = 0;
    game.follow_point = {};
    game.follow_point_ticks = 0;
    game.camera_slot_x[2] = 0;
    game.camera_slot_y[2] = 0;
    game.camera_slot_valid[2] = 0;
    game.debug_overlay = 0;
    units[0].flags = OA_UNIT_FLAG_SELECTED;
    units[1].flags = OA_UNIT_FLAG_SELECTABLE;
    units[2].flags = OA_UNIT_FLAG_SELECTED;
    game.chat_lines[1][0] = 'y';
    game.chat_head = 9;
    game.chat_tail = 3;

    give_viewer_picture(world, picture);
    CHECK(game.sim_run_flags == 0x0011);
    CHECK(game.viewpoint_player == 3);
    CHECK(game.local_player_index == 2);
    CHECK(game.follow_unit == 11);
    CHECK(game.follow_target == 12);
    CHECK(game.follow_point.x == 1 && game.follow_point.y == 2 && game.follow_point.z == 3);
    CHECK(game.follow_point_ticks == 7);
    CHECK(game.camera_slot_x[2] == 40);
    CHECK(game.camera_slot_y[2] == 50);
    CHECK(game.camera_slot_valid[2] == 1);
    CHECK(game.debug_overlay == 4);
    CHECK((units[0].flags & OA_UNIT_FLAG_SELECTED) == 0);
    CHECK((units[1].flags & OA_UNIT_FLAG_SELECTED) != 0);
    CHECK((units[1].flags & OA_UNIT_FLAG_SELECTABLE) != 0);
    CHECK((units[2].flags & OA_UNIT_FLAG_SELECTED) == 0);
    CHECK((units[3].flags & OA_UNIT_FLAG_LIVE) != 0);
    CHECK(game.chat_head == 0);
    CHECK(game.chat_tail == 0);
    CHECK(game.chat_lines[0][0] == 0);
    CHECK(game.chat_lines[1][0] == 0);

    picture.paused = false;
    game.sim_run_flags = 0x0011;
    give_viewer_picture(world, picture);
    CHECK(game.sim_run_flags == 0x0010);

    world->units = nullptr;
    world->unit_slot_count = 0;
    world_destroy(world);
}

} // namespace

int main() {
    frames_rebuild_broadcast_sequences();
    compressed_packets_rebuild_without_spurious_records();
    stored_unit_states_time_their_frame();
    recordings_are_recognised_by_their_magic();
    oademo_loads_the_same_playback();
    a_replay_of_another_version_says_so();
    transport_follows_the_ticks();
    unit_table_check_compares_checksum_keys();
    unit_table_follows_the_recorded_verdicts();
    player_table_follows_the_recorded_lobby();
    player_table_without_status_blocks();
    verdict_judges_errors_and_spacing();
    replay_reproduces_a_recorded_unit(false);
    replay_reproduces_a_recorded_unit(true);
    a_keyframe_restores_the_replay();
    a_field_left_out_is_a_keyframe_fault();
    a_session_with_a_unit_of_its_own_is_refused();
    a_jump_gives_the_viewer_its_picture_back();
    recorded_records_reach_the_binding();
    live_recording_round_trips(false);
    live_recording_round_trips(true);
    streaming_writer_gives_the_bytes_recording_write_gives();
    no_written_recording_holds_a_player_address();
    a_partial_recording_is_finished_into_a_file_that_loads();
    a_finished_file_replaces_another_only_when_asked();
    a_replay_describes_the_game_it_was_made_from();
    ten_players_watch_without_a_slot();
    recording_carries_loadout_text();
    recording_carries_brains_text();
    recording_carries_limits();
    tad_wide_lengths_match_the_wire();
    wide_records_reach_the_binding();
    limits_verdicts();
    slotless_viewer_watches_the_recording();
    recorded_unicode_chat_plays_back();
    recorded_alliance_line_reads_in_the_language_shown();
    recorded_speed_reads_in_the_language_shown();
    economy_records_give_income_figures();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("demo playback: all tests passed");
    return 0;
}
