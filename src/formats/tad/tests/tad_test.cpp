// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/tad.hpp"
#include "oa/formats/sqsh.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tad = oa::formats::tad;

namespace {

using Bytes = std::vector<uint8_t>;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void put16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8U));
}

void put32(Bytes& out, uint32_t value) {
    put16(out, static_cast<uint16_t>(value));
    put16(out, static_cast<uint16_t>(value >> 16U));
}

bool same(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

struct Fixture {
    Bytes recorder{'t', 'e', 's', 't', '-', '1'};
    Bytes date{'2', '0', '2', '6', '-', '0', '1', '-', '0', '2'};
    Bytes address = Bytes(12, 0x2a);
    Bytes status_one{0x04, 0x34, 0x12, 0x03, 0xfb, 0xfa};
    Bytes status_two{0x03, 0x10, 0x00, 0x03};
    Bytes checks;
    Bytes chat_packet;
    Bytes state_packet;
    tad::Demo demo;

    Fixture() {
        for (uint32_t unit = 0; unit < 3; ++unit) {
            checks.push_back(tad::unit_check_type);
            checks.push_back(0x02);
            put32(checks, 0);
            put32(checks, 0x1000U + unit);
            put32(checks, 0xabcdef00U ^ unit);
        }
        chat_packet = {tad::payload_marker, 0x2a, 0x64, 0x06, 0x05};
        chat_packet.resize(chat_packet.size() + 64, 'x');
        chat_packet.push_back(0xfb);
        put16(chat_packet, 2);
        chat_packet.push_back(0x01);
        chat_packet.push_back(0x02);

        state_packet = {tad::payload_marker, 0xfe};
        put32(state_packet, 100);
        state_packet.push_back(0xff);
        state_packet.push_back(0xfd); // full 0x2c length 12: 3 header bytes + 5 body bytes here
        put16(state_packet, 12);
        state_packet.insert(state_packet.end(), {0x07, 0x00, 0xaa, 0xbb, 0xcc});
        state_packet.push_back(0xff);
        state_packet.push_back(0x2c); // an untouched unit-state record
        put16(state_packet, 9);
        put32(state_packet, 55);
        state_packet.insert(state_packet.end(), {0x01, 0x02});
        state_packet.insert(state_packet.end(), {0xfc, 1, 2, 3, 4});
        state_packet.insert(state_packet.end(), {0x11, 9, 8, 7});

        demo.version = tad::supported_version;
        demo.max_units = 1500;
        demo.map_name = "Test Map";
        demo.sectors = {
            {static_cast<uint32_t>(tad::SectorType::recorder_version), recorder},
            {static_cast<uint32_t>(tad::SectorType::recording_date), date},
            {static_cast<uint32_t>(tad::SectorType::player_address), address},
        };
        demo.players = {{1, 0, 1, "alpha"}, {2, 1, 2, "beta"}};
        demo.statuses = {{1, status_one}, {2, status_two}};
        demo.unit_checks = checks;
        demo.packets = {
            {0, 0, 0, 2, chat_packet},
            {0, 178, 0, 1, state_packet},
        };
    }
};

Bytes encoded(const tad::Demo& demo) {
    const auto written = tad::write(demo);
    require(written.ok(), "fixture writes");
    return written.bytes;
}

void test_round_trip() {
    const Fixture fixture;
    const Bytes bytes = encoded(fixture.demo);
    require(bytes[0] == 8 + 13 + 2 && bytes[1] == 0, "header chunk length");
    require(
        std::string_view(reinterpret_cast<const char*>(bytes.data() + 2), 8) == tad::magic,
        "magic at offset 2"
    );

    const auto result = tad::parse(bytes);
    require(result.ok(), "fixture parses");
    const tad::Demo& demo = *result.demo;
    require(
        demo.version == 5 && demo.max_units == 1500 && demo.map_name == "Test Map", "header fields"
    );
    require(demo.sectors.size() == 3, "sector count");
    const auto* version = tad::find_sector(demo, tad::SectorType::recorder_version);
    require(
        version != nullptr && tad::sector_text(*version) == "test-1", "recorder version sector"
    );
    require(tad::find_sector(demo, tad::SectorType::recording_date) != nullptr, "date sector");
    require(
        demo.players.size() == 2 && demo.players[1].name == "beta" && demo.players[1].side == 1 &&
            demo.players[1].color == 2 && demo.players[1].number == 2,
        "players"
    );
    require(
        demo.statuses.size() == 2 && demo.statuses[0].number == 1 &&
            same(demo.statuses[0].datagram, fixture.status_one),
        "statuses"
    );
    const auto checks = tad::unit_check_records(demo);
    require(checks.size() == 3 && checks[2][0] == tad::unit_check_type, "unit checks");
    require(demo.packets.size() == 2, "packet count");
    require(
        demo.packets[1].sender == 1 && demo.packets[1].delay_ms == 178 &&
            demo.packets[1].time_ms == 178,
        "packet timing and sender"
    );
    require(same(demo.packets[1].payload, fixture.state_packet), "packet payload view");

    require(encoded(demo) == bytes, "write(parse(bytes)) reproduces bytes");
}

void test_split_records() {
    const Fixture fixture;
    const auto chat = tad::split_records(fixture.chat_packet);
    require(chat.marker_ok && !chat.error, "chat packet splits");
    require(chat.records.size() == 4, "chat packet record count");
    require(chat.records[0].type == 0x2a && chat.records[0].bytes.size() == 2, "0x2a length");
    require(chat.records[2].type == 0x05 && chat.records[2].bytes.size() == 65, "chat length");
    require(
        chat.records[3].type == 0xfb && chat.records[3].bytes.size() == 5, "recorder message length"
    );

    const auto state = tad::split_records(fixture.state_packet);
    require(state.marker_ok && !state.error, "state packet splits");
    require(state.records.size() == 7, "state packet record count");
    require(state.records[0].tick == 100U, "tick base");
    require(state.records[1].tick == 100U, "empty tick takes the base tick");
    require(
        state.records[2].tick == 101U && state.records[2].bytes.size() == 8,
        "elided state tick and length"
    );
    require(state.records[3].tick == 102U, "second empty tick");
    require(
        state.records[4].type == 0x2c && state.records[4].bytes.size() == 9 &&
            state.records[4].tick == 55U,
        "stored unit state carries its own tick"
    );
    require(state.records[5].type == 0xfc && state.records[6].type == 0x11, "trailing records");

    const Bytes expanded = tad::expand_unit_state(state.records[2]);
    const Bytes expected{0x2c, 12, 0, 101, 0, 0, 0, 0x07, 0x00, 0xaa, 0xbb, 0xcc};
    require(expanded == expected, "expanded unit state");
    require(tad::expand_unit_state(state.records[1]).empty(), "only 0xfd expands");

    const Bytes untimed{tad::payload_marker, 0xff, 0xfd, 7, 0};
    const auto no_base = tad::split_records(untimed);
    require(
        !no_base.error && no_base.records.size() == 2 && !no_base.records[0].tick &&
            !no_base.records[1].tick,
        "ticks without a base stay unknown"
    );
    require(tad::expand_unit_state(no_base.records[1]).empty(), "no tick, no expansion");

    // 904, packet at file offset 19232: behind a chat record, the recorder kept
    // the sender's empty unit states for ticks 869.. whole, ticks included.
    const Bytes stored{
        tad::payload_marker,
        0x2c,
        0x0b,
        0x00,
        0x65,
        0x03,
        0x00,
        0x00,
        0xff,
        0xff,
        0x01,
        0x00,
        0x2c,
        0x0b,
        0x00,
        0x66,
        0x03,
        0x00,
        0x00,
        0xff,
        0xff,
        0x01,
        0x00
    };
    const auto whole = tad::split_records(stored);
    require(
        !whole.error && whole.records.size() == 2 && whole.records[0].tick == 869U &&
            whole.records[1].tick == 870U,
        "recorded unit states carry their ticks"
    );

    const Bytes bad{tad::payload_marker, 0x06, 0x2b, 0x06};
    const auto stopped = tad::split_records(bad);
    require(
        stopped.error && stopped.error->offset == 2 && stopped.records.size() == 1,
        "unknown type stops split"
    );
    // A recorder's own packets fill their payload: 7300's chat command at
    // file offset 9335 and 9342's message at offset 9076, which has no length.
    const Bytes command{tad::payload_marker, '.', 's', 'h', 'a', 'r', 'e', 'l', 'o', 's'};
    const auto typed = tad::split_records(command);
    require(
        !typed.error && typed.records.size() == 1 && typed.records[0].type == '.' &&
            typed.records[0].bytes.size() == 9,
        "recorder command fills its packet"
    );
    const Bytes message{tad::payload_marker, 0xfb, 0x04, 0x02, 0x02, 0x00, 0x00, 0x00};
    const auto handshake = tad::split_records(message);
    require(
        !handshake.error && handshake.records.size() == 1 && handshake.records[0].type == 0xfb &&
            handshake.records[0].bytes.size() == 7,
        "recorder message without a length fills its packet"
    );
    for (const uint8_t type : {uint8_t{'.'}, uint8_t{0xfb}}) {
        const Bytes behind{tad::payload_marker, 0x06, type, 0x04, 0x02};
        const auto split = tad::split_records(behind);
        require(
            split.error && split.error->offset == 2 && split.records.size() == 1,
            "only a packet's first record fills it"
        );
    }
    const Bytes short_record{0x00, 0x0d, 0x01};
    const auto truncated = tad::split_records(short_record);
    require(
        !truncated.marker_ok && truncated.error && truncated.records.empty(),
        "overlong record stops split"
    );
    require(tad::split_records({}).error.has_value(), "empty payload");

    // 884, first compressed packet at file offset 13538: 0x04 precedes the
    // condenser's fresh-tree LZ77 bytes, which expand to one 0x28 record.
    const Bytes compressed{
        0x04,
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
        0x00
    };
    const auto decoded = tad::decode_payload(compressed);
    require(decoded.ok() && decoded.bytes.size() == 59, "recorded compressed payload expands");
    require(decoded.bytes[0] == tad::payload_marker, "decoded payload marker");
    const auto compressed_records = tad::split_records(decoded.bytes);
    require(
        !compressed_records.error && compressed_records.records.size() == 1 &&
            compressed_records.records[0].type == 0x28 &&
            compressed_records.records[0].bytes.size() == 58,
        "compressed packet splits into its record"
    );
    require(tad::split_records(compressed).error.has_value(), "compressed bytes are not records");
    require(!tad::decode_payload(Bytes{0x04, 0x00}).ok(), "truncated compressed stream");
    require(!tad::decode_payload(Bytes{0x05, 0x06}).ok(), "unknown payload marker");
    const Bytes oversized(tad::limit::decoded_payload_bytes + 1, 0);
    Bytes compressed_oversized{tad::compressed_payload_marker};
    const auto packed =
        oa::formats::sqsh::encode_lz77(oversized, oversized.size() * 2).value.value();
    compressed_oversized.insert(compressed_oversized.end(), packed.begin(), packed.end());
    require(!tad::decode_payload(compressed_oversized).ok(), "expanded receive limit");

    require(tad::record_length(Bytes{0x2c, 2, 0}) == 0, "0x2c shorter than its header");
    require(tad::record_length(Bytes{0xfd, 3, 0}) == 0, "0xfd full length below header");
    require(tad::record_length(Bytes{0x20}) == 0, "record past end");
    require(tad::record_length(Bytes{0x04}) == 0, "unused game type");
}

void expect_error(const Bytes& bytes, tad::ErrorCode code, std::string_view what) {
    const auto result = tad::parse(bytes);
    require(!result.ok() && result.error && result.error->code == code, what);
}

void test_malformed() {
    const Fixture fixture;
    const Bytes bytes = encoded(fixture.demo);

    // Every strict prefix either fails cleanly or (on a packet boundary) parses fewer packets.
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const Bytes prefix(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
        const auto result = tad::parse(prefix);
        if (result.ok())
            require(result.demo->packets.size() < 2, "prefix drops packets");
        else
            require(result.error->offset <= length, "error offset inside input");
    }

    Bytes wrong_magic = bytes;
    wrong_magic[3] = 'X';
    expect_error(wrong_magic, tad::ErrorCode::bad_magic, "bad magic");

    Bytes wrong_version = bytes;
    wrong_version[10] = 4;
    expect_error(wrong_version, tad::ErrorCode::unsupported_version, "version 4 rejected");

    Bytes many_players = bytes;
    many_players[12] = 200;
    expect_error(many_players, tad::ErrorCode::count_limit, "player count limit");

    Bytes tiny_chunk = bytes;
    tiny_chunk[0] = 1;
    expect_error(tiny_chunk, tad::ErrorCode::malformed, "chunk length below 2");

    Bytes many_sectors = bytes;
    const std::size_t count_at = bytes[0] + 2U;
    many_sectors[count_at + 3] = 0x7f;
    expect_error(many_sectors, tad::ErrorCode::count_limit, "sector count limit");

    tad::Demo odd = fixture.demo;
    const Bytes ragged(fixture.checks.begin(), fixture.checks.end() - 1);
    odd.unit_checks = ragged;
    require(!tad::write(odd).ok(), "writer rejects ragged unit checks");

    tad::Demo mismatched = fixture.demo;
    mismatched.statuses.pop_back();
    require(!tad::write(mismatched).ok(), "writer rejects status/player mismatch");

    // Deterministic byte corruption must never read out of bounds.
    uint32_t state = 0x12345678U;
    for (int round = 0; round < 4000; ++round) {
        Bytes corrupt = bytes;
        for (int flip = 0; flip < 3; ++flip) {
            state = state * 1664525U + 1013904223U;
            corrupt[(state >> 8U) % corrupt.size()] = static_cast<uint8_t>(state >> 24U);
        }
        const auto result = tad::parse(corrupt);
        if (result.ok()) {
            for (const auto& packet : result.demo->packets)
                if (const auto decoded = tad::decode_payload(packet.payload); decoded.ok())
                    (void)tad::split_records(decoded.bytes);
        }
    }
}

void test_mutators_sector_round_trips() {
    Fixture fixture;
    const std::string text = "ridge-pace 1";
    const Bytes loadout(text.begin(), text.end());
    fixture.demo.sectors.push_back({static_cast<uint32_t>(tad::SectorType::mutators), loadout});
    const Bytes bytes = encoded(fixture.demo);
    const auto result = tad::parse(bytes);
    require(result.ok(), "mutators sector parses");
    const auto* sector = tad::find_sector(*result.demo, tad::SectorType::mutators);
    require(sector != nullptr && tad::sector_text(*sector) == text, "mutators sector text");
    require(sector != nullptr && same(sector->data, loadout), "mutators sector bytes");
    require(encoded(*result.demo) == bytes, "mutators sector round trip");
}

void test_recording_without_mutators_sector() {
    const Fixture fixture;
    const auto result = tad::parse(encoded(fixture.demo));
    require(result.ok(), "recording without the mutators sector parses");
    require(result.demo->sectors.size() == 3, "no extra sector");
    require(
        tad::find_sector(*result.demo, tad::SectorType::mutators) == nullptr,
        "mutators sector absent"
    );
}

void test_brains_sector_round_trips() {
    Fixture fixture;
    const std::string text = "steady-commander 1";
    const Bytes body(text.begin(), text.end());
    fixture.demo.sectors.push_back({static_cast<uint32_t>(tad::SectorType::brains), body});
    const Bytes bytes = encoded(fixture.demo);
    const auto result = tad::parse(bytes);
    require(result.ok(), "brains sector parses");
    const auto* sector = tad::find_sector(*result.demo, tad::SectorType::brains);
    require(sector != nullptr && tad::sector_text(*sector) == text, "brains sector text");
    require(sector != nullptr && same(sector->data, body), "brains sector bytes");
    require(encoded(*result.demo) == bytes, "brains sector round trip");
}

void test_recording_without_brains_sector() {
    const Fixture fixture;
    const auto result = tad::parse(encoded(fixture.demo));
    require(result.ok(), "recording without the brains sector parses");
    require(result.demo->sectors.size() == 3, "no extra sector");
    require(
        tad::find_sector(*result.demo, tad::SectorType::brains) == nullptr, "brains sector absent"
    );
}

void test_unknown_sector_is_kept() {
    Fixture fixture;
    const Bytes extra{0x10, 0x20, 0x30};
    const uint32_t unknown = 0x51525354U;
    fixture.demo.sectors.insert(fixture.demo.sectors.begin() + 1, {unknown, extra});
    const Bytes bytes = encoded(fixture.demo);
    const auto result = tad::parse(bytes);
    require(result.ok(), "unknown sector parses");
    require(result.demo->sectors.size() == 4, "unknown sector kept");
    require(
        result.demo->sectors[1].type == unknown && same(result.demo->sectors[1].data, extra),
        "unknown sector bytes"
    );
    require(
        tad::find_sector(*result.demo, tad::SectorType::recording_date) != nullptr,
        "a later sector is kept"
    );
    require(
        result.demo->packets.size() == 2 && result.demo->players.size() == 2,
        "the recording continues after the unknown sector"
    );
    require(encoded(*result.demo) == bytes, "unknown sector round trip");
}

} // namespace

int main() {
    try {
        test_round_trip();
        test_split_records();
        test_malformed();
        test_mutators_sector_round_trips();
        test_recording_without_mutators_sector();
        test_brains_sector_round_trips();
        test_recording_without_brains_sector();
        test_unknown_sector_is_kept();
        std::cout << "tad-format: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tad-format: " << error.what() << '\n';
        return 1;
    }
}
