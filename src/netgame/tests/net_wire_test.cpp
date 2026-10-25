// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/condenser.hpp"
#include "oa/netgame/player_slots.hpp"
#include "oa/netgame/content_hash.hpp"
#include "oa/netgame/dplay.hpp"
#include "oa/netgame/frame.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unit_state.hpp"
#include "oa/netgame/network.hpp"
#include "oa/formats/tad.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <vector>

using namespace oa::netgame;
using Bytes = std::vector<uint8_t>;

namespace {

int failures = 0;
const char* current_test = "";

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s: %s:%d: %s\n", current_test, __FILE__, __LINE__, #condition); \
        }                                                                                          \
    } while (0)

uint32_t lcg_state = 0x12345678;

uint8_t next_byte() {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return static_cast<uint8_t>(lcg_state >> 16);
}

// ---- length table and splitting ----

/// Checks that 0x1d, two u32 after its type byte, is 9 bytes long.
void length_0x1d_is_9() {
    CHECK(record_length_table[0x1d] == 9);
}

/// Checks that 0x27, a player id and 12 more bytes after its type byte, is 17 bytes long.
void length_0x27_is_17() {
    CHECK(record_length_table[0x27] == 17);
}

/// Checks that 0x03 and 0x25, which 3.1c neither sends nor handles, are 3 and 5 bytes long.
void types_0x03_and_0x25_have_table_lengths() {
    CHECK(record_length_table[0x03] == 3);
    CHECK(record_length_table[0x25] == 5);
}

void record_wire_length_rules() {
    uint16_t length = 0;
    const uint8_t invalid[] = {0x01, 0x2d, 0x00};
    for (auto b : invalid)
        CHECK(record_wire_length(&b, 1, &length) == WireError::invalid_type);
    const uint8_t zero_types[] = {0x04, 0x2b};
    for (auto b : zero_types)
        CHECK(record_wire_length(&b, 1, &length) == WireError::zero_length_record);
    const uint8_t state[] = {0x2c, 0x34, 0x12};
    CHECK(record_wire_length(state, 2, &length) == WireError::truncated);
    CHECK(record_wire_length(state, 3, &length) == WireError::ok && length == 0x1234);
    const uint8_t chat = 0x05;
    CHECK(record_wire_length(&chat, 1, &length) == WireError::ok && length == 65);
    CHECK(
        record_phase_table[0x1d] == 0 && record_phase_table[0x2c] == 4 &&
        record_phase_table[0x1a] == 1
    );
}

// ---- record codecs ----

void every_record_type_round_trips() {
    for (unsigned t = first_record_type; t <= last_record_type; ++t) {
        if (t == 0x04 || t == 0x2b)
            continue;
        Bytes wire;
        if (t == 0x2c) {
            wire = {0x2c, 0, 0, 0x78, 0x56, 0x34, 0x12};
            for (int i = 0; i < 9; ++i)
                wire.push_back(next_byte());
            store_u16(wire.data() + 1, static_cast<uint16_t>(wire.size()));
        } else {
            wire.resize(record_length_table[t]);
            wire[0] = static_cast<uint8_t>(t);
            for (std::size_t i = 1; i < wire.size(); ++i)
                wire[i] = next_byte();
        }
        AnyRecord record;
        CHECK(decode_any_record(wire.data(), wire.size(), &record) == WireError::ok);
        CHECK(static_cast<unsigned>(record.type) == t);
        Bytes again(wire.size() + 8, 0xee);
        std::size_t written = 0;
        CHECK(encode_any_record(record, again.data(), again.size(), &written) == WireError::ok);
        CHECK(written == wire.size());
        CHECK(std::equal(wire.begin(), wire.end(), again.begin()));
        CHECK(
            encode_any_record(record, again.data(), wire.size() - 1, &written) ==
            WireError::buffer_too_small
        );
        if (t != 0x2c)
            CHECK(decode_any_record(wire.data(), wire.size() - 1, &record) == WireError::truncated);
    }
    AnyRecord record;
    const uint8_t undefined[] = {0x04, 0, 0};
    CHECK(decode_any_record(undefined, sizeof undefined, &record) == WireError::zero_length_record);
    const uint8_t wrong_size[] = {0x06, 0};
    CHECK(decode_any_record(wrong_size, sizeof wrong_size, &record) == WireError::length_mismatch);
}

void unit_created_matches_existing_encoder() {
    oa::netgame::network::UnitCreated network_unit_created{
        0x0102, 0x0304, {0x11223344, 0x55667788, 0x99aabbcc}, {0xa1a2, 0xb1b2, 0xc1c2}
    };
    const auto expected = oa::netgame::network::encode_unit_created(network_unit_created);
    UnitCreatedRecord r{};
    r.unit_def_index = 0x0102;
    r.unit_index = 0x0304;
    r.position[0] = 0x11223344;
    r.position[1] = 0x55667788;
    r.position[2] = static_cast<int32_t>(0x99aabbccu);
    r.bank_heading = 0xb1b2a1a2u;
    r.pitch = 0xc1c2;
    uint8_t wire[23]{};
    std::size_t written = 0;
    CHECK(encode_record(r, wire, sizeof wire, &written) == WireError::ok && written == 23);
    CHECK(std::equal(expected.begin(), expected.end(), wire));

    BuilderLinkRecord link{0x1234, 0x5678};
    uint8_t link_wire[5]{};
    CHECK(encode_record(link, link_wire, sizeof link_wire, &written) == WireError::ok);
    const auto network_unit_finished = oa::netgame::network::encode_unit_finished(0x1234, 0x5678);
    CHECK(std::equal(network_unit_finished.begin(), network_unit_finished.end(), link_wire));
}

void known_record_layouts() {
    const uint8_t ping[] = {0x02, 1, 2, 3, 4, 0, 0, 0, 0, 0xef, 0xbe, 0xad, 0xde};
    PingRecord p{};
    CHECK(decode_record(ping, sizeof ping, &p) == WireError::ok);
    CHECK(
        p.origin_tick_count == 0x04030201 && p.echo_tick_count == 0 &&
        p.origin_player_id == 0xdeadbeef
    );

    const uint8_t reject[] = {0x1b, 0x78, 0x56, 0x34, 0x12, 4};
    RejectRecord r{};
    CHECK(decode_record(reject, sizeof reject, &r) == WireError::ok);
    CHECK(
        r.player_id == 0x12345678 &&
        static_cast<RejectReason>(r.reason) == RejectReason::wrong_password
    );

    UnitDamageRecord d{0x0010, 0x0020, 300, 0x01, 1};
    uint8_t damage[9]{};
    CHECK(encode_record(d, damage, sizeof damage, nullptr) == WireError::ok);
    const uint8_t expected_damage[] = {0x0b, 0x10, 0, 0x20, 0, 0x2c, 0x01, 0x01, 0x01};
    CHECK(std::memcmp(damage, expected_damage, sizeof damage) == 0);

    EconomyRecord e{};
    e.want_reply = 1;
    e.kills = -2;
    e.metal = 1.5f;
    uint8_t economy[58]{};
    CHECK(encode_record(e, economy, sizeof economy, nullptr) == WireError::ok);
    CHECK(
        economy[1] == 1 && load_u32(economy + 2) == 0xfffffffeu &&
        load_u32(economy + 0x12) == 0x3fc00000u
    );

    PlayerInfoRecord info{};
    info.player_id = 0xaabbccdd;
    store_u32(info.info_tail + (player_info_map_hash_offset - player_info_tail_offset), 0x01020304);
    uint8_t info_wire[186]{};
    CHECK(encode_record(info, info_wire, sizeof info_wire, nullptr) == WireError::ok);
    CHECK(load_u32(info_wire + 0x91) == 0xaabbccdd && load_u32(info_wire + 1 + 0xa9) == 0x01020304);
    CHECK(player_info_map_hash(info) == 0x01020304);

    // Spec example: 0x2c header for length 0x1234 and tick 0xa1b2c3d4.
    const uint8_t state[] = {0x2c, 0x34, 0x12, 0xd4, 0xc3, 0xb2, 0xa1};
    UnitStateRecord header{};
    CHECK(decode_unit_state_record(state, sizeof state, &header) == WireError::length_mismatch);
    uint8_t big[0x1234]{};
    std::memcpy(big, state, sizeof state);
    CHECK(decode_unit_state_record(big, sizeof big, &header) == WireError::ok);
    CHECK(
        header.length == 0x1234 && header.sender_tick == 0xa1b2c3d4u &&
        header.body_size == 0x1234 - 7
    );

    WeaponFireRecord fire{};
    fire.start[0] = 1;
    fire.start[1] = 2;
    fire.start[2] = 3;
    fire.target[0] = 4;
    fire.target[1] = 5;
    fire.target[2] = 6;
    fire.weapon_id = 7;
    fire.flags = 1;
    fire.aim_heading = 0x0102;
    fire.aim_pitch = -3;
    fire.target_unit_index = 0x0405;
    fire.source_unit_index = 0x0607;
    fire.slot = 2;
    uint8_t fire_wire[36]{};
    CHECK(encode_record(fire, fire_wire, sizeof fire_wire, nullptr) == WireError::ok);
    const uint8_t expected_fire[] = {
        0x0d, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0,    0,    4, 0, 0, 0, 5,
        0,    0, 0, 6, 0, 0, 0, 7, 1, 2, 1, 0xfd, 0xff, 5, 4, 7, 6, 2,
    };
    CHECK(std::memcmp(fire_wire, expected_fire, sizeof fire_wire) == 0);

    ProjectileInterceptedRecord intercepted{};
    intercepted.target[0] = 9;
    intercepted.target[1] = 8;
    intercepted.target[2] = 7;
    intercepted.weapon_id = 6;
    uint8_t intercepted_wire[14]{};
    CHECK(
        encode_record(intercepted, intercepted_wire, sizeof intercepted_wire, nullptr) ==
        WireError::ok
    );
    const uint8_t expected_intercepted[] = {0x0e, 9, 0, 0, 0, 8, 0, 0, 0, 7, 0, 0, 0, 6};
    CHECK(std::memcmp(intercepted_wire, expected_intercepted, sizeof intercepted_wire) == 0);

    FeatureEventRecord feature{};
    feature.action = 4;
    feature.x = 0x0102;
    feature.y = 0x0304;
    uint8_t feature_wire[6]{};
    CHECK(encode_record(feature, feature_wire, sizeof feature_wire, nullptr) == WireError::ok);
    const uint8_t expected_feature[] = {0x0f, 4, 2, 1, 4, 3};
    CHECK(std::memcmp(feature_wire, expected_feature, sizeof feature_wire) == 0);
}

// ---- bit stream ----

void wide_records_round_trip() {
    const uint16_t ids[] = {0, 255, 256, 65535};
    for (const auto id : ids) {
        WideWeaponFireRecord fire{};
        fire.start[0] = 1;
        fire.start[1] = 2;
        fire.start[2] = 3;
        fire.target[0] = 4;
        fire.target[1] = 5;
        fire.target[2] = 6;
        fire.weapon_id = id;
        fire.flags = 1;
        fire.aim_heading = 0x0102;
        fire.aim_pitch = -3;
        fire.target_unit_index = 0x0405;
        fire.source_unit_index = 0x0607;
        fire.slot = 2;
        uint8_t bytes[64]{};
        std::size_t written = 0;
        CHECK(encode_record(fire, bytes, sizeof bytes, &written) == WireError::ok);
        CHECK(written == wide_record_length_table[0] && bytes[0] == 0xe0);
        CHECK(load_u16(bytes + 25) == id);
        WideWeaponFireRecord fire_back{};
        CHECK(decode_record(bytes, written, &fire_back) == WireError::ok && fire_back == fire);

        WideProjectileInterceptedRecord hit{};
        hit.target[0] = 9;
        hit.target[1] = 8;
        hit.target[2] = 7;
        hit.weapon_id = id;
        CHECK(encode_record(hit, bytes, sizeof bytes, &written) == WireError::ok);
        CHECK(written == wide_record_length_table[1] && bytes[0] == 0xe1);
        CHECK(load_u16(bytes + 13) == id);
        WideProjectileInterceptedRecord hit_back{};
        CHECK(decode_record(bytes, written, &hit_back) == WireError::ok && hit_back == hit);

        WideFeatureEventRecord event{};
        event.action = static_cast<uint8_t>(WideFeatureAction::weapon_hit);
        event.weapon_id = id;
        event.x = 0x0102;
        event.y = 0x0304;
        CHECK(encode_record(event, bytes, sizeof bytes, &written) == WireError::ok);
        CHECK(written == wide_record_length_table[2] && bytes[0] == 0xe2);
        CHECK(load_u16(bytes + 2) == id);
        WideFeatureEventRecord event_back{};
        CHECK(decode_record(bytes, written, &event_back) == WireError::ok && event_back == event);
    }

    FeatureEventRecord narrow{};
    narrow.x = 1;
    narrow.y = 2;
    narrow.action = feature_action_queue_event;
    auto widened = widen(narrow);
    CHECK(
        widened.action == static_cast<uint8_t>(WideFeatureAction::queue_event) &&
        widened.weapon_id == 0 && widened.x == 1 && widened.y == 2
    );
    narrow.action = feature_action_tree_burn;
    widened = widen(narrow);
    CHECK(
        widened.action == static_cast<uint8_t>(WideFeatureAction::tree_burn) &&
        widened.weapon_id == 0
    );
    narrow.action = feature_action_queue_event_flagged;
    widened = widen(narrow);
    CHECK(
        widened.action == static_cast<uint8_t>(WideFeatureAction::queue_event_flagged) &&
        widened.weapon_id == 0
    );
    narrow.action = 4;
    widened = widen(narrow);
    CHECK(
        widened.action == static_cast<uint8_t>(WideFeatureAction::weapon_hit) &&
        widened.weapon_id == 4
    );

    uint8_t sample[40]{};
    uint16_t length = 0;
    sample[0] = 0xe0;
    CHECK(wide_record_wire_length(sample, sizeof sample, &length) == WireError::ok && length == 37);
    sample[0] = 0xe1;
    CHECK(wide_record_wire_length(sample, sizeof sample, &length) == WireError::ok && length == 15);
    sample[0] = 0xe2;
    CHECK(wide_record_wire_length(sample, sizeof sample, &length) == WireError::ok && length == 8);
    sample[0] = 0xdf;
    CHECK(wide_record_wire_length(sample, sizeof sample, &length) == WireError::invalid_type);
    sample[0] = 0xe3;
    CHECK(wide_record_wire_length(sample, sizeof sample, &length) == WireError::invalid_type);
    sample[0] = 0xe0;
    CHECK(record_wire_length(sample, sizeof sample, &length) == WireError::invalid_type);
}

// A frame of 0x0b, 0xe0, 0x0b splits into three records only when wide records are admitted.
void wide_records_split_only_when_admitted() {
    constexpr std::size_t damage = 9;
    constexpr std::size_t wide = 37;
    uint8_t frame[frame_header_bytes + damage + wide + damage]{};
    frame[frame_header_bytes] = 0x0b;
    frame[frame_header_bytes + damage] = 0xe0;
    frame[frame_header_bytes + damage + wide] = 0x0b;

    auto without = std::make_unique<PeerRecords>();
    peer_records_reset(without.get());
    auto outcome = UnpackOutcome::unpacked;
    CHECK(
        unpack_frame_records(without.get(), frame, sizeof frame, 0, 1, 0, false, &outcome) ==
        WireError::ok
    );
    const uint8_t* data = nullptr;
    uint16_t length = 0;
    CHECK(pop_due_record(without.get(), 0, &data, &length) && data[0] == 0x0b && length == damage);
    CHECK(!pop_due_record(without.get(), 0, &data, &length));

    auto admitted = std::make_unique<PeerRecords>();
    peer_records_reset(admitted.get());
    admitted->wide_records = true;
    CHECK(
        unpack_frame_records(admitted.get(), frame, sizeof frame, 0, 1, 0, false, &outcome) ==
        WireError::ok
    );
    CHECK(pop_due_record(admitted.get(), 0, &data, &length) && data[0] == 0x0b && length == damage);
    CHECK(pop_due_record(admitted.get(), 0, &data, &length) && data[0] == 0xe0 && length == wide);
    CHECK(pop_due_record(admitted.get(), 0, &data, &length) && data[0] == 0x0b && length == damage);
    CHECK(!pop_due_record(admitted.get(), 0, &data, &length));
}

void bit_writer_is_bounded() {
    uint8_t storage[8]{};
    BitWriter w;
    bit_writer_init(&w, storage, 2);
    bit_writer_write(&w, 0xffffffffu, 32);
    CHECK(w.error == WireError::ok);
    bit_writer_write(&w, 0xffffffffu, 32);
    CHECK(w.error == WireError::overflow);
    bit_writer_init(&w, storage, 2);
    bit_writer_write(&w, 1, 33);
    CHECK(w.error == WireError::bad_argument);
}

// ---- unit state ----

void build_fraction_coding_is_asymmetric() {
    CHECK(unit_state_build_byte(0.0f) == 0);
    CHECK(unit_state_build_byte(-0.0f) == 0);
    CHECK(unit_state_build_byte(1.0f) == 255);
    CHECK(unit_state_build_byte(0.5f) == 128);
    CHECK(unit_state_build_byte(0.25f) == 64);
    CHECK(unit_state_build_byte(0.001f) == 1);
    CHECK(unit_state_build_fraction(0) == 0.0f);
    // 128/255 is not 0.5: decoding does not invert encoding.
    CHECK(unit_state_build_fraction(unit_state_build_byte(0.5f)) != 0.5f);
    CHECK(unit_state_build_fraction_differs(128, 0.5f));
    CHECK(!unit_state_build_fraction_differs(128, unit_state_build_fraction(128)));
    CHECK(unit_state_full_record_slot(10, 4) == 2);
    CHECK(unit_state_full_record_slot(0xfffffffeu, 4) == -2);
}

WireError read_waypoints(void* context, uint16_t, uint16_t, BitReader* reader) {
    auto* seen = static_cast<WaypointDelta*>(context);
    read_waypoint_delta(reader, seen);
    return WireError::ok;
}

void unit_state_stream_round_trip() {
    constexpr unsigned bits = 9;
    uint8_t storage[unit_state_writer_words * 4]{};
    BitWriter w;
    bit_writer_init(&w, storage, unit_state_writer_words);
    unit_state_begin(&w, 0xa1b2c3d4u);
    WaypointDelta delta{true, 2, {{100, -200}, {-3, 4}, {0, 0}}};
    unit_state_write_entry_header(&w, 5, 300, bits);
    write_waypoint_delta(&w, delta);
    CHECK(!unit_state_full(&w));
    FullUnitRecord full{};
    full.unit_def_index = 17;
    full.health = -5;
    full.build_byte = 7;
    full.position[0] = 0x10000;
    full.has_object_word = true;
    full.object_word = 0xcafef00d;
    uint16_t length = 0;
    CHECK(unit_state_finish(&w, full, bits, &length) == WireError::ok);
    CHECK(length == bit_writer_byte_length(&w));
    CHECK(
        storage[0] == 0x2c && load_u16(storage + 1) == length &&
        load_u32(storage + 3) == 0xa1b2c3d4u
    );
    // The first entry's unit index is byte aligned right after the header.
    CHECK(load_u16(storage + 7) == 5);

    auto body = std::make_unique<UnitStateBody>();
    UnitStateDecodeOptions options{bits, true, nullptr};
    CHECK(
        decode_unit_state(storage, length, options, body.get()) ==
        WireError::unsupported_delta_layout
    );
    CHECK(
        body->entry_count == 1 && body->entries[0].unit_index == 5 &&
        body->entries[0].def_index == 300
    );

    WaypointDelta seen{};
    UnitDeltaCodec codec{&seen, read_waypoints, nullptr};
    options.delta_codec = &codec;
    CHECK(decode_unit_state(storage, length, options, body.get()) == WireError::ok);
    CHECK(body->sender_tick == 0xa1b2c3d4u && body->entry_count == 1 && body->has_full_record);
    CHECK(body->entries[0].delta_bit_count == 1 + 2 + 2 * 32);
    CHECK(seen.flag && seen.count == 2 && seen.points[0][1] == -200 && seen.points[1][0] == -3);
    CHECK(
        body->full.unit_def_index == 17 && body->full.health == -5 &&
        body->full.object_word == 0xcafef00d
    );
    CHECK((body->bits_consumed + 7) / 8 == length);

    // Empty list: decodes without any codec; the flag and record follow the terminator.
    bit_writer_init(&w, storage, unit_state_writer_words);
    unit_state_begin(&w, 3);
    FullUnitRecord empty_slot{};
    CHECK(unit_state_finish(&w, empty_slot, bits, &length) == WireError::ok);
    CHECK(length == 7 + 4);
    options.delta_codec = nullptr;
    CHECK(decode_unit_state(storage, length, options, body.get()) == WireError::ok);
    CHECK(body->entry_count == 0 && body->has_full_record && body->full.unit_def_index == 0);
    CHECK(
        decode_unit_state(storage, length - 1, options, body.get()) == WireError::length_mismatch
    );
    store_u16(storage + 1, static_cast<uint16_t>(length - 1));
    CHECK(decode_unit_state(storage, length - 1, options, body.get()) == WireError::truncated);
}

// ---- frame layer ----

struct Captured {
    uint32_t from = 0;
    uint32_t to = 0;
    Bytes frame;
};

struct Capture {
    std::vector<Captured> frames;

    static void emit(void* ctx, uint32_t from, uint32_t to, const uint8_t* f, std::size_t n) {
        static_cast<Capture*>(ctx)->frames.push_back({from, to, Bytes(f, f + n)});
    }
};

void sequence_numbering_wraps_to_minus_two() {
    CHECK(next_frame_sequence(-2) == -3);
    CHECK(next_frame_sequence(INT32_MIN) == -2);
    CHECK(next_frame_sequence(-1) == -2);
    CHECK(next_frame_sequence(5) == -2);
}

void send_pacing_values() {
    CHECK(!send_pacing(-1).layer_enabled);
    CHECK(send_pacing(0).interval_ms == 200 && send_pacing(0).ticks_between_sends == 6);
    CHECK(send_pacing(5).interval_ms == 200 && send_pacing(5).ticks_between_sends == 6);
    CHECK(send_pacing(10).interval_ms == 100 && send_pacing(10).ticks_between_sends == 3);
    CHECK(send_pacing(30).interval_ms == 33 && send_pacing(30).ticks_between_sends == 1);
    CHECK(send_pacing(1).interval_ms == 500 && send_pacing(99).interval_ms == 33);
}

void send_frames_group_by_head_sender() {
    auto ch = std::make_unique<SendChannel>();
    send_channel_init(ch.get(), broadcast_destination_id, 6);
    Capture cap;
    FrameSink sink{&cap, Capture::emit};
    const uint8_t a1[] = {0x06}, b1[] = {0x07}, a2[] = {0x15};
    CHECK(send_channel_queue(ch.get(), 11, a1, 1, 0, sink) == WireError::ok);
    CHECK(send_channel_queue(ch.get(), 22, b1, 1, 0, sink) == WireError::ok);
    CHECK(send_channel_queue(ch.get(), 11, a2, 1, 0, sink) == WireError::ok);
    CHECK(send_channel_flush(ch.get(), 10, false, sink));
    CHECK(cap.frames.size() == 2);
    CHECK(
        cap.frames[0].from == 11 && cap.frames[0].to == 0 &&
        (cap.frames[0].frame == Bytes{0xfe, 0xff, 0xff, 0xff, 0x06, 0x15})
    );
    CHECK(cap.frames[1].from == 22 && (cap.frames[1].frame == Bytes{0xfd, 0xff, 0xff, 0xff, 0x07}));
    // Paced out until tick 16.
    CHECK(send_channel_queue(ch.get(), 11, a1, 1, 11, sink) == WireError::ok);
    CHECK(!send_channel_flush(ch.get(), 15, false, sink));
    CHECK(send_channel_flush(ch.get(), 16, false, sink));
    CHECK(cap.frames.size() == 3 && load_u32(cap.frames[2].frame.data()) == 0xfffffffcu);

    // Unicast channels stamp -1 but still step their counter.
    send_channel_init(ch.get(), 77, 6);
    CHECK(send_channel_queue(ch.get(), 11, a1, 1, 0, sink) == WireError::ok);
    send_channel_flush(ch.get(), 0, true, sink);
    CHECK(load_u32(cap.frames.back().frame.data()) == 0xffffffffu && ch->frame_number == -3);
    ch->frame_number = INT32_MIN;
    CHECK(send_channel_queue(ch.get(), 11, a1, 1, 0, sink) == WireError::ok);
    send_channel_flush(ch.get(), 0, true, sink);
    CHECK(ch->frame_number == -2);
}

void send_pacing_across_the_clock_turn() {
    // The connection's clock reads 30 units a second, from 0 through
    // 4,294,967, and then turns over to 0, about every 39.8 hours.
    constexpr uint32_t kLargestReading = 4'294'967;
    auto ch = std::make_unique<SendChannel>();
    send_channel_init(ch.get(), 77, 6);
    Capture cap;
    FrameSink sink{&cap, Capture::emit};
    const uint8_t a1[] = {0x06};
    // A send just before the turn sets the next send past the largest reading.
    CHECK(send_channel_queue(ch.get(), 11, a1, 1, kLargestReading - 2, sink) == WireError::ok);
    CHECK(send_channel_flush(ch.get(), kLargestReading - 2, false, sink));
    CHECK(ch->next_send_tick == kLargestReading + 4);
    CHECK(send_channel_queue(ch.get(), 11, a1, 1, kLargestReading, sink) == WireError::ok);
    CHECK(!send_channel_flush(ch.get(), kLargestReading, false, sink));
    // After the turn the queued packet goes out at the next paced flush,
    // and the pacing goes on from there.
    CHECK(send_channel_flush(ch.get(), 1, false, sink));
    CHECK(ch->queue_count == 0 && ch->next_send_tick == 7 && cap.frames.size() == 2);
    CHECK(send_channel_queue(ch.get(), 11, a1, 1, 3, sink) == WireError::ok);
    CHECK(!send_channel_flush(ch.get(), 6, false, sink));
    CHECK(send_channel_flush(ch.get(), 7, false, sink));
    CHECK(cap.frames.size() == 3);
    // A send due well before the turn goes out at the first flush after it.
    ch->next_send_tick = kLargestReading - 30;
    CHECK(send_channel_queue(ch.get(), 11, a1, 1, 0, sink) == WireError::ok);
    CHECK(send_channel_flush(ch.get(), 0, false, sink));
    CHECK(cap.frames.size() == 4);
}

void send_forces_flush_at_threshold() {
    auto ch = std::make_unique<SendChannel>();
    send_channel_init(ch.get(), broadcast_destination_id, 6);
    Capture cap;
    FrameSink sink{&cap, Capture::emit};
    Bytes big(0x400, 0x02);
    Bytes small(0x40, 0x06);
    CHECK(send_channel_queue(ch.get(), 1, big.data(), big.size(), 0, sink) == WireError::ok);
    CHECK(send_channel_queue(ch.get(), 1, small.data(), small.size(), 0, sink) == WireError::ok);
    CHECK(cap.frames.empty());
    CHECK(send_channel_queue(ch.get(), 1, small.data(), small.size(), 0, sink) == WireError::ok);
    CHECK(cap.frames.size() == 1 && cap.frames[0].frame.size() == 4 + 0x440);
    Bytes too_big(send_buffer_bytes + 1, 0);
    CHECK(
        send_channel_queue(ch.get(), 1, too_big.data(), too_big.size(), 0, sink) ==
        WireError::buffer_too_small
    );
}

// A zero-length record would never advance the split; the split reports it instead.
void split_zero_length_record_is_reported() {
    auto records = std::make_unique<PeerRecords>();
    peer_records_reset(records.get());
    const uint8_t frame[] = {0xfe, 0xff, 0xff, 0xff, 0x06, 0x04, 0x00};
    auto outcome = UnpackOutcome::unpacked;
    CHECK(
        unpack_frame_records(records.get(), frame, sizeof frame, 5, 1, 0, true, &outcome) ==
        WireError::zero_length_record
    );
}

void delivery_window_holds_future_records() {
    auto records = std::make_unique<PeerRecords>();
    peer_records_reset(records.get());
    const uint8_t frame[] = {0xfe, 0xff, 0xff, 0xff, 0x06, 0x07};
    auto outcome = UnpackOutcome::unpacked;
    CHECK(
        unpack_frame_records(records.get(), frame, sizeof frame, 100, 1, 0, true, &outcome) ==
        WireError::ok
    );
    const uint8_t* data = nullptr;
    uint16_t length = 0;
    CHECK(pop_due_record(records.get(), 100, &data, &length) && data[0] == 0x06 && length == 1);
    CHECK(!pop_due_record(records.get(), 100, &data, &length));
    CHECK(pop_due_record(records.get(), 101, &data, &length) && data[0] == 0x07);
    CHECK(!pop_due_record(records.get(), 101, &data, &length));
}

Bytes sequenced(int32_t sequence, uint8_t marker) {
    Bytes f(5);
    store_u32(f.data(), static_cast<uint32_t>(sequence));
    f[4] = marker;
    return f;
}

void receiver_orders_and_holds_frames() {
    auto rx = std::make_unique<FrameReceiver>();
    frame_receiver_init(rx.get());
    FrameDeliveries out;
    Bytes current;
    const auto accept = [&](Bytes f) {
        current = std::move(f);
        return frame_receiver_accept(rx.get(), 7, 0, current.data(), current.size(), &out);
    };
    CHECK(accept(sequenced(-2, 0x06)) == WireError::ok && out.count == 1 && out.items[0].fresh);
    CHECK(accept(sequenced(-3, 0x06)) == WireError::ok && out.count == 1 && out.items[0].fresh);
    // -5 arrives before -4: held.
    CHECK(accept(sequenced(-5, 0x15)) == WireError::ok && out.held && out.count == 0);
    // -4 arrives: the held newer frame goes first (not fresh), then -4 (fresh).
    CHECK(accept(sequenced(-4, 0x07)) == WireError::ok && out.count == 2);
    CHECK(out.items[0].frame[4] == 0x15 && !out.items[0].fresh);
    CHECK(out.items[1].frame[4] == 0x07 && out.items[1].fresh);
    CHECK(rx->peers[0].last_sequence == -4);
    // Duplicate / older frames are processed again: no de-duplication.
    CHECK(accept(sequenced(-4, 0x07)) == WireError::ok && out.count == 1);
    // Unicast frames bypass sequencing.
    CHECK(
        accept(sequenced(-1, 0x02)) == WireError::ok && out.count == 1 &&
        rx->peers[0].last_sequence == -4
    );

    // Gap with a held frame newer than the current one: current first, both not fresh.
    CHECK(accept(sequenced(-8, 0x1b)) == WireError::ok && out.held);
    CHECK(accept(sequenced(-6, 0x1c)) == WireError::ok && out.count == 2);
    CHECK(
        out.items[0].frame[4] == 0x1c && !out.items[0].fresh && out.items[1].frame[4] == 0x1b &&
        !out.items[1].fresh
    );
    CHECK(rx->peers[0].last_sequence == -8);

    // Gap with a held frame older than the current one: held first, then current (fresh).
    CHECK(accept(sequenced(-10, 0x1e)) == WireError::ok && out.held);
    CHECK(accept(sequenced(-11, 0x1f)) == WireError::ok && out.count == 2);
    CHECK(
        out.items[0].frame[4] == 0x1e && !out.items[0].fresh && out.items[1].frame[4] == 0x1f &&
        out.items[1].fresh
    );
    CHECK(rx->peers[0].last_sequence == -11);

    // The sequence wraps from INT32_MIN to -2.
    rx->peers[0].last_sequence = INT32_MIN;
    CHECK(accept(sequenced(-2, 0x06)) == WireError::ok && out.count == 1 && !out.held);

    const Bytes header_only(4, 0xff);
    CHECK(
        frame_receiver_accept(rx.get(), 7, 0, header_only.data(), header_only.size(), &out) ==
        WireError::truncated
    );
    const auto first = sequenced(-2, 6);
    for (uint32_t id = 100; id < 109; ++id)
        CHECK(
            frame_receiver_accept(rx.get(), id, 0, first.data(), first.size(), &out) ==
            WireError::ok
        );
    // Every slot is taken and no player table is bound: refused.
    CHECK(
        frame_receiver_accept(rx.get(), 200, 0, first.data(), first.size(), &out) ==
        WireError::unknown_peer
    );
    // The player table names every peer: refused.
    oa::Player players[OA_PLAYER_COUNT]{};
    for (std::size_t i = 0; i < OA_PLAYER_COUNT; ++i)
        players[i].player_id = static_cast<uint32_t>(rx->peers[i].peer_id);
    rx->players = players;
    CHECK(
        frame_receiver_accept(rx.get(), 200, 0, first.data(), first.size(), &out) ==
        WireError::unknown_peer
    );
    // Peer 103's player record now names the newcomer: it takes 103's
    // slot, sequence and ring fresh.
    CHECK(rx->peers[4].peer_id == 103);
    players[4].player_id = 200;
    CHECK(
        frame_receiver_accept(rx.get(), 200, 0, first.data(), first.size(), &out) == WireError::ok
    );
    CHECK(rx->peers[4].peer_id == 200 && rx->peers[4].last_sequence == -2 && out.count == 1);
    CHECK(std::none_of(std::begin(rx->peers), std::end(rx->peers), [](const PeerFrameState& p) {
        return p.peer_id == 103;
    }));
}

// ---- condenser ----

struct Datagram {
    uint32_t from{};
    uint32_t to{};
    Bytes bytes;
};

struct Wire {
    std::vector<Datagram> sent;
    std::vector<Datagram> inbox;
    std::size_t next{};
    uint32_t last_flags{};

    static uint32_t send(
        void* context,
        uint32_t from,
        uint32_t to,
        uint32_t flags,
        const uint8_t* data,
        uint32_t size
    ) {
        auto* wire = static_cast<Wire*>(context);
        wire->last_flags = flags;
        wire->sent.push_back({from, to, Bytes(data, data + size)});
        return transport_result::ok;
    }

    static uint32_t
    receive(void* context, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) {
        auto* wire = static_cast<Wire*>(context);
        if (wire->next == wire->inbox.size())
            return transport_result::no_messages;
        const auto& d = wire->inbox[wire->next];
        *from = d.from;
        *to = d.to;
        if (d.bytes.size() > *size) {
            *size = static_cast<uint32_t>(d.bytes.size());
            return transport_result::buffer_too_small;
        }
        std::memcpy(buffer, d.bytes.data(), d.bytes.size());
        *size = static_cast<uint32_t>(d.bytes.size());
        ++wire->next;
        return transport_result::ok;
    }

    NetTransport transport() { return {this, send, receive}; }
};

int32_t highest_roll() {
    return 0x7fff;
}

Bytes condensed(
    Condenser* c, Wire& wire, TrafficStats* stats, const Bytes& payload, bool compress
) {
    CHECK(condenser_stage(c, 9, payload.data(), static_cast<uint32_t>(payload.size())));
    CHECK(
        condenser_send(c, wire.transport(), stats, 4, send_flag_guaranteed, compress, 0, nullptr) ==
        transport_result::ok
    );
    return wire.sent.empty() ? Bytes{} : wire.sent.back().bytes;
}

void condenser_send_selects_as_encode_frame() {
    auto c = std::make_unique<Condenser>();
    condenser_init(c.get());
    Wire wire;
    TrafficStats stats{};
    Bytes repetitive(200);
    for (std::size_t i = 0; i < repetitive.size(); ++i)
        repetitive[i] = static_cast<uint8_t>(i % 5);
    const auto packed = condensed(c.get(), wire, &stats, repetitive, true);
    CHECK(
        packed.size() > 3 && packed[0] == condenser_type_compressed &&
        packed.size() < repetitive.size()
    );
    CHECK(packed == oa::netgame::network::encode_frame(repetitive, true));
    CHECK(
        wire.sent.back().from == 4 && wire.sent.back().to == 9 &&
        wire.last_flags == send_flag_guaranteed
    );
    CHECK(
        stats.sent_bytes == repetitive.size() + condenser_header_bytes &&
        stats.sent_datagrams == 1 && stats.condensed_bytes == packed.size()
    );
    // Compression off, and payloads under 13 bytes, go out stored.
    CHECK(
        condensed(c.get(), wire, &stats, repetitive, false) ==
        oa::netgame::network::encode_frame(repetitive, false)
    );
    const Bytes twelve(12, 0x41);
    const auto stored = condensed(c.get(), wire, &stats, twelve, true);
    CHECK(
        stored[0] == condenser_type_stored &&
        stored == oa::netgame::network::encode_stored_frame(twelve)
    );
    CHECK(c->staged_length == 0);
    // Too large for the staging storage.
    const Bytes huge(condenser_stage_bytes, 1);
    CHECK(!condenser_stage(c.get(), 9, huge.data(), static_cast<uint32_t>(huge.size())));
}

void condenser_loss_roll_drops_but_counts() {
    auto c = std::make_unique<Condenser>();
    condenser_init(c.get());
    Wire wire;
    TrafficStats stats{};
    const Bytes payload(20, 7);
    // A roll of 0x7fff maps to 100: loss 100 drops it, loss 50 does not.
    CHECK(condenser_stage(c.get(), 2, payload.data(), 20));
    CHECK(
        condenser_send(c.get(), wire.transport(), &stats, 1, 0, true, 100, highest_roll) ==
        transport_result::ok
    );
    CHECK(wire.sent.empty() && stats.sent_bytes == 23 && c->staged_length == 0);
    CHECK(condenser_stage(c.get(), 2, payload.data(), 20));
    CHECK(
        condenser_send(c.get(), wire.transport(), &stats, 1, 0, true, 50, highest_roll) ==
        transport_result::ok
    );
    CHECK(wire.sent.size() == 1 && stats.sent_bytes == 46);
}

void condenser_receive_unwraps_and_retries() {
    auto sender = std::make_unique<Condenser>();
    auto receiver = std::make_unique<Condenser>();
    condenser_init(sender.get());
    condenser_init(receiver.get());
    Wire out;
    Bytes payload(300);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(i * 7 % 11);
    const auto datagram = condensed(sender.get(), out, nullptr, payload, true);
    Wire in;
    in.inbox.push_back({0, 3, {0x05, 0, 0, 0}}); // system message: raw
    in.inbox.push_back({5, 0, datagram});
    auto corrupt = datagram;
    corrupt[4] ^= 0x10;
    in.inbox.push_back({5, 0, corrupt});
    in.inbox.push_back({5, 0, {0x02, 0xaa}}); // not a condenser type: raw
    TrafficStats stats{};
    Bytes buffer(1024);
    uint32_t from = 0, to = 0, size = static_cast<uint32_t>(buffer.size());
    CHECK(
        condenser_receive(
            receiver.get(), in.transport(), &stats, &from, &to, buffer.data(), &size
        ) == transport_result::ok
    );
    CHECK(from == 0 && size == 4 && buffer[0] == 0x05 && stats.received_bytes == 0);
    // Too small: the needed size comes back and the next call delivers it
    // without receiving again.
    size = 100;
    CHECK(
        condenser_receive(
            receiver.get(), in.transport(), &stats, &from, &to, buffer.data(), &size
        ) == transport_result::buffer_too_small
    );
    CHECK(size == payload.size() && receiver->retry_pending && in.next == 2);
    CHECK(stats.received_bytes == datagram.size());
    size = static_cast<uint32_t>(buffer.size());
    CHECK(
        condenser_receive(
            receiver.get(), in.transport(), &stats, &from, &to, buffer.data(), &size
        ) == transport_result::ok
    );
    CHECK(
        size == payload.size() && std::equal(payload.begin(), payload.end(), buffer.begin()) &&
        in.next == 2
    );
    size = static_cast<uint32_t>(buffer.size());
    CHECK(
        condenser_receive(
            receiver.get(), in.transport(), &stats, &from, &to, buffer.data(), &size
        ) == transport_result::no_messages
    );
    size = static_cast<uint32_t>(buffer.size());
    CHECK(
        condenser_receive(
            receiver.get(), in.transport(), &stats, &from, &to, buffer.data(), &size
        ) == transport_result::ok
    );
    CHECK(size == 2 && buffer[0] == 0x02);
    size = static_cast<uint32_t>(buffer.size());
    CHECK(
        condenser_receive(
            receiver.get(), in.transport(), &stats, &from, &to, buffer.data(), &size
        ) == transport_result::no_messages
    );
}

void traffic_rates_sample_every_thirty_ticks() {
    TrafficStats stats{};
    uint32_t send = 0, receive = 0;
    stats.sent_bytes = 3000;
    stats.received_bytes = 600;
    traffic_stats_rates(&stats, 30, &send, &receive);
    CHECK(send == 0 && receive == 0); // 30 ticks is not more than 30
    traffic_stats_rates(&stats, 60, &send, &receive);
    CHECK(send == 1500 && receive == 300 && stats.rates.sample_time == 60);
    stats.sent_bytes += 900;
    traffic_stats_rates(&stats, 80, &send, &receive);
    CHECK(send == 1500); // held until the next sample
    traffic_stats_rates(&stats, 150, &send, &receive);
    CHECK(send == 300 && receive == 0);
    traffic_stats_reset(&stats, 77);
    CHECK(
        stats.sent_bytes == 0 && stats.received_bytes == 0 &&
        stats.rates.sampled_sent_bytes == 3900 && stats.reset_tick == 77
    );
}

void traffic_counts_records_and_datagrams() {
    TrafficStats stats{};
    // Types 2..0x2c are counted per type; every type reaches the channel total.
    traffic_stats_count_record(&stats, 0x2c, 10, TrafficChannel::sent);
    traffic_stats_count_record(&stats, 0x2d, 7, TrafficChannel::sent);
    traffic_stats_count_record(&stats, 1, 5, TrafficChannel::received);
    traffic_stats_count_record(&stats, 0x05, 65, TrafficChannel::received);
    CHECK(stats.channel_bytes[1] == 17 && stats.channel_bytes[0] == 70);
    CHECK(stats.record_count[0x2c][1] == 1 && stats.record_bytes[0x2c][1] == 10);
    CHECK(stats.record_count[5][0] == 1 && stats.record_count[1][0] == 0);
    traffic_stats_count_datagram(&stats, 23, 20, TrafficChannel::sent);
    traffic_stats_count_datagram(&stats, 30, 0, TrafficChannel::received);
    traffic_stats_count_datagram(&stats, 30, -1, TrafficChannel::received);
    CHECK(stats.sent_datagrams == 1 && stats.sent_bytes == 23 && stats.condensed_bytes == 20);
    CHECK(stats.received_datagrams == 2 && stats.received_bytes == 60);
    traffic_stats_reset(&stats, 0);
    CHECK(
        stats.channel_bytes[0] == 0 && stats.record_count[5][0] == 0 &&
        stats.record_bytes[0x2c][1] == 0
    );
    CHECK(stats.condensed_bytes == 20 && stats.sent_datagrams == 0);
}

void traffic_debug_line_formats_rates() {
    TrafficStats stats{};
    char line[traffic_debug_line_bytes]{};
    // 60 ticks after the zero sample: 400 record bytes sent condensed into
    // 300, 120 received, 4 datagrams (410 bytes) out, 2 (140 bytes) in.
    stats.channel_bytes[1] = 400;
    stats.channel_bytes[0] = 120;
    stats.condensed_bytes = 300;
    stats.sent_datagrams = 4;
    stats.sent_bytes = 410;
    stats.received_datagrams = 2;
    stats.received_bytes = 140;
    traffic_stats_debug_line(&stats, 60, line, sizeof line);
    CHECK(std::strcmp(line, "pS= 200 pR=  60 (S=2/ 205, R=1/  70) C= 25%\n") == 0);
    // Nothing sent since the sample: the percentage reads 0; held rates
    // are reprinted until 30 ticks pass.
    traffic_stats_debug_line(&stats, 80, line, sizeof line);
    CHECK(std::strcmp(line, "pS= 200 pR=  60 (S=2/ 205, R=1/  70) C= 25%\n") == 0);
    traffic_stats_debug_line(&stats, 120, line, sizeof line);
    CHECK(std::strcmp(line, "pS=   0 pR=   0 (S=0/   0, R=0/   0) C=  0%\n") == 0);
}

void frame_survives_condenser_round_trip() {
    auto ch = std::make_unique<SendChannel>();
    send_channel_init(ch.get(), broadcast_destination_id, 1);
    Capture cap;
    FrameSink sink{&cap, Capture::emit};
    ChatRecord chat{};
    std::memcpy(chat.text, "hello, commander", 16);
    uint8_t wire[65];
    CHECK(encode_record(chat, wire, sizeof wire, nullptr) == WireError::ok);
    CHECK(send_channel_queue(ch.get(), 3, wire, sizeof wire, 0, sink) == WireError::ok);
    CHECK(send_channel_queue(ch.get(), 3, wire, sizeof wire, 0, sink) == WireError::ok);
    send_channel_flush(ch.get(), 0, true, sink);
    CHECK(cap.frames.size() == 1);
    const auto datagram = oa::netgame::network::encode_frame(cap.frames[0].frame);
    const auto frame = oa::netgame::network::decode_frame(datagram);
    CHECK(frame == cap.frames[0].frame);
    auto records = std::make_unique<PeerRecords>();
    peer_records_reset(records.get());
    auto outcome = UnpackOutcome::unpacked;
    CHECK(
        unpack_frame_records(records.get(), frame.data(), frame.size(), 50, 3, 0, true, &outcome) ==
        WireError::ok
    );
    CHECK(records->count == 2);
    const uint8_t* data = nullptr;
    uint16_t length = 0;
    CHECK(pop_due_record(records.get(), 50, &data, &length));
    ChatRecord back{};
    CHECK(
        decode_record(data, length, &back) == WireError::ok &&
        std::memcmp(back.text, chat.text, 64) == 0
    );
}

// ---- Player slot table ----

void player_slots_follow_status_not_in_use() {
    auto game = std::make_unique<oa::Game>();
    auto& players = game->players;
    players[0] = {};
    players[0].status = OA_PLAYER_STATUS_COMPUTER;
    players[0].in_use = 1;
    players[0].player_id = 5;
    players[1].status = OA_PLAYER_STATUS_MIRRORED;
    players[1].player_id = 9; // not in use, still answers to its id
    players[2].status = OA_PLAYER_STATUS_LOCAL;
    players[2].in_use = 1;
    players[2].player_id = 7;
    players[3].status = OA_PLAYER_STATUS_CLOSED;
    players[4].player_id = 11; // free: answers to nothing
    CHECK(
        player_slot_id(*game, 1) == 9 && player_slot_id(*game, 4) == no_player_id &&
        player_slot_id(*game, no_player_slot) == no_player_id
    );
    CHECK(
        player_slot_of(*game, 9) == 1 && player_slot_of(*game, 11) == no_player_slot &&
        player_slot_of(*game, no_player_id) == no_player_slot
    );
    CHECK(player_of_id(*game, 7) == &players[2] && player_of_id(*game, 12) == nullptr);
    CHECK(first_local_player_id(*game) == 7);
    // Slot 1 is not in use: it is the first free one; closed slots never are.
    CHECK(free_player_slot(*game) == 1);
    for (auto& player : players)
        player.in_use = 1;
    players[3].in_use = 0;
    CHECK(free_player_slot(*game) == no_player_slot);
}

// ---- DirectPlay boundary ----

void dplay_envelope_round_trip() {
    const uint8_t body[] = {1, 2, 3};
    DplayEnvelope e{};
    e.port = 2300;
    e.address[0] = 192;
    e.address[3] = 7;
    e.has_play_header = true;
    e.command = static_cast<uint16_t>(DplayCommand::request_player_id);
    e.version = 0x0e;
    e.body = body;
    e.body_size = sizeof body;
    uint8_t wire[64];
    std::size_t written = 0;
    CHECK(encode_dplay_envelope(e, wire, sizeof wire, &written) == WireError::ok && written == 31);
    CHECK(load_u32(wire) == (0xfabu << 20 | 31u) && wire[6] == 0x08 && wire[7] == 0xfc);
    CHECK(load_u32(wire + 20) == 0x79616c70u);
    DplayEnvelope back{};
    std::size_t next = 0;
    CHECK(decode_dplay_envelope(wire, written, &back, &next) == WireError::ok && next == 31);
    CHECK(back.port == 2300 && back.has_play_header && back.command == 5 && back.version == 0x0e);
    CHECK(back.body_size == 3 && back.body[2] == 3);
    const uint8_t condenser[] = {0x03, 0, 0, 0, 0, 0, 0, 0};
    CHECK(
        decode_dplay_envelope(condenser, sizeof condenser, &back, &next) == WireError::invalid_type
    );
}

void create_player_data_verdicts() {
    CreatePlayerData data{};
    std::memcpy(data.tag, "Secret", 6);
    data.version_high = create_player_version_high;
    uint8_t wire[create_player_data_bytes];
    CHECK(encode_create_player_data(data, wire, sizeof wire) == WireError::ok);
    CHECK(check_create_player_data(wire, sizeof wire, true, "sECRET") == JoinVerdict::accepted);
    CHECK(
        check_create_player_data(wire, sizeof wire, true, "other") == JoinVerdict::wrong_password
    );
    CHECK(check_create_player_data(wire, sizeof wire, false, nullptr) == JoinVerdict::accepted);
    CHECK(
        check_create_player_data(wire, sizeof wire - 1, false, nullptr) ==
        JoinVerdict::version_mismatch
    );
    wire[0x13] = 0x51;
    CHECK(
        check_create_player_data(wire, sizeof wire, false, nullptr) == JoinVerdict::version_mismatch
    );

    SessionDesc desc{};
    desc.flags = session_flag_game | session_flag_password_required;
    std::memcpy(desc.application_guid, application_guid, 16);
    desc.max_players = 10;
    desc.user[3] = 0x44;
    uint8_t desc_wire[session_desc_bytes];
    CHECK(encode_session_desc(desc, desc_wire, sizeof desc_wire) == WireError::ok);
    SessionDesc back{};
    CHECK(decode_session_desc(desc_wire, sizeof desc_wire, &back) == WireError::ok);
    CHECK(
        back.size == 0x50 && back.flags == desc.flags && back.max_players == 10 &&
        back.user[3] == 0x44
    );
    CHECK(std::memcmp(back.application_guid, application_guid, 16) == 0);
}

} // namespace

int main() {
    struct Test {
        const char* name;
        void (*run)();
    };

    const Test tests[] = {
        {"length_0x1d_is_9", length_0x1d_is_9},
        {"length_0x27_is_17", length_0x27_is_17},
        {"types_0x03_and_0x25_have_table_lengths", types_0x03_and_0x25_have_table_lengths},
        {"record_wire_length_rules", record_wire_length_rules},
        {"every_record_type_round_trips", every_record_type_round_trips},
        {"unit_created_matches_existing_encoder", unit_created_matches_existing_encoder},
        {"known_record_layouts", known_record_layouts},
        {"wide_records_round_trip", wide_records_round_trip},
        {"wide_records_split_only_when_admitted", wide_records_split_only_when_admitted},
        {"bit_writer_is_bounded", bit_writer_is_bounded},
        {"build_fraction_coding_is_asymmetric", build_fraction_coding_is_asymmetric},
        {"unit_state_stream_round_trip", unit_state_stream_round_trip},
        {"sequence_numbering_wraps_to_minus_two", sequence_numbering_wraps_to_minus_two},
        {"send_pacing_values", send_pacing_values},
        {"send_frames_group_by_head_sender", send_frames_group_by_head_sender},
        {"send_pacing_across_the_clock_turn", send_pacing_across_the_clock_turn},
        {"send_forces_flush_at_threshold", send_forces_flush_at_threshold},
        {"split_zero_length_record_is_reported", split_zero_length_record_is_reported},
        {"delivery_window_holds_future_records", delivery_window_holds_future_records},
        {"receiver_orders_and_holds_frames", receiver_orders_and_holds_frames},
        {"frame_survives_condenser_round_trip", frame_survives_condenser_round_trip},
        {"condenser_send_selects_as_encode_frame", condenser_send_selects_as_encode_frame},
        {"condenser_loss_roll_drops_but_counts", condenser_loss_roll_drops_but_counts},
        {"condenser_receive_unwraps_and_retries", condenser_receive_unwraps_and_retries},
        {"traffic_rates_sample_every_thirty_ticks", traffic_rates_sample_every_thirty_ticks},
        {"traffic_counts_records_and_datagrams", traffic_counts_records_and_datagrams},
        {"traffic_debug_line_formats_rates", traffic_debug_line_formats_rates},
        {"player_slots_follow_status_not_in_use", player_slots_follow_status_not_in_use},
        {"dplay_envelope_round_trip", dplay_envelope_round_trip},
        {"create_player_data_verdicts", create_player_data_verdicts},
    };
    for (const auto& test : tests) {
        current_test = test.name;
        test.run();
    }
    std::printf("net-wire: %zu tests; %d failures\n", std::size(tests), failures);
    return failures == 0 ? 0 : 1;
}
