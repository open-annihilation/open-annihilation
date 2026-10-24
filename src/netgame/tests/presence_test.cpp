// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The presence record: its bytes, the fields a reader keeps, and the one
// place a frame's split takes it.

#include "oa/data/mutators/loadout.hpp"
#include "oa/netgame/frame.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace oa::netgame;

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

void start_case(const char* name) {
    current_test = name;
    std::printf("presence: %s\n", name);
    std::fflush(stdout);
}

// A record under construction. The length word is filled in by finish.
struct Raw {
    std::vector<uint8_t> bytes{presence_record_type, 0, 0, presence_record_version};

    void u8(uint8_t value) { bytes.push_back(value); }

    void u16(uint16_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }

    void u32(uint32_t value) {
        u16(static_cast<uint16_t>(value));
        u16(static_cast<uint16_t>(value >> 16));
    }

    void raw(const void* data, std::size_t size) {
        const auto* bytes_in = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), bytes_in, bytes_in + size);
    }

    void str(std::string_view text) {
        u8(static_cast<uint8_t>(text.size()));
        raw(text.data(), text.size());
    }

    void field(uint8_t tag, const std::vector<uint8_t>& value) {
        u8(tag);
        u16(static_cast<uint16_t>(value.size()));
        raw(value.data(), value.size());
    }

    void finish() {
        const auto length = static_cast<uint16_t>(bytes.size());
        bytes[1] = static_cast<uint8_t>(length);
        bytes[2] = static_cast<uint8_t>(length >> 8);
    }
};

struct Value {
    std::vector<uint8_t> bytes;

    void u8(uint8_t value) { bytes.push_back(value); }

    void u16(uint16_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }

    void u32(uint32_t value) {
        u16(static_cast<uint16_t>(value));
        u16(static_cast<uint16_t>(value >> 16));
    }

    void raw(const void* data, std::size_t size) {
        const auto* bytes_in = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), bytes_in, bytes_in + size);
    }

    void str(std::string_view text) {
        u8(static_cast<uint8_t>(text.size()));
        raw(text.data(), text.size());
    }
};

PresenceDigest filled(uint8_t byte) {
    PresenceDigest digest{};
    digest.fill(byte);
    return digest;
}

// The card's sample, with Ridge where a real mod would be named.
PresenceRecord sample_record() {
    PresenceRecord record;
    record.engine = PresenceEngine{"0.8.0", "Windows", "x64"};
    record.mod = PresenceMod{"ridge", "Ridge", "4.8", 3};
    record.mod_origin = PresenceOrigin{"coreprime", "https://example.test/ridge", 28, filled(0x11)};
    record.sim_hash = filled(0xab);
    record.map_pack = PresenceMapPack{
        "archipelago",
        3,
        filled(0x22),
        "example-maps",
        "https://example.test/maps",
        "Archipelago Pack",
        "1.2"
    };
    record.developer = PresenceDeveloper{
        true,
        2,
        {PresenceOverride{"economy.deterministic-wind", true},
         PresenceOverride{"ai.income-multipliers", false}}
    };
    record.game_hacks = PresenceHacks{
        8,
        {"economy.deterministic-wind",
         "ai.income-multipliers",
         "air.gunships-hover-to-strafe",
         "navy.shore-build",
         "combat.damage-falloff",
         "builder.queue-assist",
         "resources.metal-share",
         "units.veteran-speed"}
    };
    record.view_hacks = PresenceHacks{
        4, {"view.unit-ranges", "view.build-icons", "view.resource-bars", "view.path-overlay"}
    };
    return record;
}

// header 4 + engine field 21 (value 18: 6 + 8 + 4) + sim field 35 + developer
// field 6 (flags 0 and a count of 0) = 66.
constexpr uint8_t kSmallRecord[] = {
    0xf0, 0x42, 0x00, 0x01, 0x01, 0x12, 0x00, 0x05, '0',  '.',  '8',  '.',  '0',  0x07,
    'W',  'i',  'n',  'd',  'o',  'w',  's',  0x03, 'x',  '6',  '4',  0x20, 0x20, 0x00,
    0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
    0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
    0xab, 0xab, 0xab, 0xab, 0x23, 0x03, 0x00, 0x00, 0x00, 0x00,
};
static_assert(sizeof kSmallRecord == 66);

// Engine 0.8.0 / Windows / x64, Developer Mode off with no overrides, and a
// sim hash of 32 bytes of 0xab. The bytes above are counted from the layout.
void a_small_record_is_encoded_exactly() {
    start_case("a_small_record_is_encoded_exactly");
    PresenceRecord record;
    record.engine = PresenceEngine{"0.8.0", "Windows", "x64"};
    record.sim_hash = filled(0xab);
    record.developer = PresenceDeveloper{};
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    CHECK(written == sizeof kSmallRecord);
    CHECK(std::memcmp(out, kSmallRecord, sizeof kSmallRecord) == 0);
    CHECK(
        encode_presence(record, out, sizeof kSmallRecord - 1, &written) ==
        WireError::buffer_too_small
    );
}

void every_field_round_trips() {
    start_case("every_field_round_trips");
    const auto record = sample_record();
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    PresenceRecord decoded;
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded == record);
}

void lists_are_cut_to_fit() {
    start_case("lists_are_cut_to_fit");
    auto record = sample_record();
    record.game_hacks = PresenceHacks{90, {}};
    record.game_hacks->ids.assign(90, std::string(30, 'a'));
    uint8_t out[presence_record_max_bytes + 64];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    CHECK(written <= presence_record_max_bytes);
    PresenceRecord decoded;
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded.engine == record.engine);
    CHECK(decoded.mod == record.mod);
    CHECK(decoded.mod_origin == record.mod_origin);
    CHECK(decoded.sim_hash == record.sim_hash);
    CHECK(decoded.map_pack == record.map_pack);
    CHECK(decoded.developer == record.developer);
    CHECK(decoded.game_hacks && decoded.game_hacks->total == 90);
    CHECK(decoded.game_hacks->ids.size() < 90 && !decoded.game_hacks->ids.empty());
    for (const auto& id : decoded.game_hacks->ids)
        CHECK(id == std::string(30, 'a'));
    CHECK(decoded.view_hacks && decoded.view_hacks->total == record.view_hacks->total);
    CHECK(decoded.view_hacks->ids.size() <= record.view_hacks->ids.size());

    PresenceRecord bad = record;
    bad.game_hacks->ids[0] = std::string(65, 'a');
    CHECK(encode_presence(bad, out, sizeof out, &written) == WireError::bad_argument);
    bad.game_hacks->ids[0] = "Not-an-id";
    CHECK(encode_presence(bad, out, sizeof out, &written) == WireError::bad_argument);
    CHECK(encode_presence(record, out, 8, &written) == WireError::buffer_too_small);
}

void unknown_fields_are_skipped() {
    start_case("unknown_fields_are_skipped");
    Raw record;
    Value engine;
    engine.str("0.8.0");
    engine.str("Linux");
    engine.str("arm64");
    record.field(0x01, engine.bytes);
    record.field(0x02, {});
    Value one;
    one.u8(0xee);
    record.field(0x7f, one.bytes);
    Value sim;
    sim.raw(filled(0xab).data(), 32);
    record.field(0x20, sim.bytes);
    Value wide;
    wide.bytes.assign(300, 0x11);
    record.field(0xff, wide.bytes);
    Value developer;
    developer.u8(0);
    developer.u16(0);
    record.field(0x23, developer.bytes);
    record.finish();
    CHECK(record.bytes.size() <= presence_record_max_bytes);

    PresenceRecord decoded;
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.version == 1);
    CHECK(decoded.engine == PresenceEngine("0.8.0", "Linux", "arm64"));
    CHECK(decoded.sim_hash == filled(0xab));
    CHECK(decoded.developer == PresenceDeveloper{});
    CHECK(!decoded.mod && !decoded.map_pack && !decoded.game_hacks);

    record.bytes[3] = 2;
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.version == 2);
    CHECK(decoded.engine == PresenceEngine("0.8.0", "Linux", "arm64"));
    CHECK(decoded.sim_hash == filled(0xab));
}

void a_bad_field_is_left_out() {
    start_case("a_bad_field_is_left_out");
    Raw record;
    // The engine version's length runs past the field. The field is 4 bytes.
    Value engine;
    engine.u8(10);
    engine.raw("abc", 3);
    record.field(0x01, engine.bytes);
    Value mod;
    mod.str("Ridge");
    mod.str("Ridge");
    mod.str("4.8");
    mod.u16(3);
    record.field(0x10, mod.bytes);
    Value sim;
    sim.raw(filled(0x11).data(), 32);
    record.field(0x20, sim.bytes);
    Value again;
    again.raw(filled(0x22).data(), 32);
    record.field(0x20, again.bytes);
    Value short_sim;
    short_sim.raw(filled(0x33).data(), 31);
    record.field(0x20, short_sim.bytes);
    Value developer;
    developer.u8(presence_record_version);
    developer.u16(0);
    record.field(0x23, developer.bytes);
    record.finish();

    PresenceRecord decoded;
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(!decoded.engine);
    CHECK(!decoded.mod);
    CHECK(decoded.sim_hash == filled(0x11));
    CHECK(decoded.developer && decoded.developer->on && decoded.developer->total == 0);

    Raw only_short;
    only_short.field(0x20, short_sim.bytes);
    Value kept;
    kept.str("0.8.0");
    kept.str("Windows");
    kept.str("x64");
    only_short.field(0x01, kept.bytes);
    only_short.finish();
    CHECK(
        decode_presence(only_short.bytes.data(), only_short.bytes.size(), &decoded) == WireError::ok
    );
    CHECK(!decoded.sim_hash);
    CHECK(decoded.engine == PresenceEngine("0.8.0", "Windows", "x64"));
}

void malformed_records_are_refused() {
    start_case("malformed_records_are_refused");
    PresenceRecord out;
    uint8_t byte = 0;
    CHECK(decode_presence(nullptr, 0, &out) == WireError::bad_argument);
    CHECK(decode_presence(&byte, 0, nullptr) == WireError::bad_argument);
    CHECK(decode_presence(&byte, 0, &out) == WireError::truncated);

    const uint8_t three[] = {0xf0, 0x04, 0x00};
    CHECK(decode_presence(three, sizeof three, &out) == WireError::length_mismatch);
    const uint8_t length_three[] = {0xf0, 0x03, 0x00};
    CHECK(decode_presence(length_three, sizeof length_three, &out) == WireError::length_mismatch);

    std::vector<uint8_t> huge(1025, 0);
    huge[0] = presence_record_type;
    huge[1] = 0x01;
    huge[2] = 0x04;
    huge[3] = 1;
    CHECK(decode_presence(huge.data(), huge.size(), &out) == WireError::length_mismatch);

    const uint8_t mismatched[] = {0xf0, 0x06, 0x00, 0x01, 0, 0, 0, 0};
    CHECK(decode_presence(mismatched, sizeof mismatched, &out) == WireError::length_mismatch);
    const uint8_t version_zero[] = {0xf0, 0x04, 0x00, 0x00};
    CHECK(decode_presence(version_zero, sizeof version_zero, &out) == WireError::bad_argument);
    const uint8_t cut_header[] = {0xf0, 0x05, 0x00, 0x01, 0x01};
    CHECK(decode_presence(cut_header, sizeof cut_header, &out) == WireError::truncated);
    // The value claims 5 bytes and the record ends on the size word.
    const uint8_t past_end[] = {0xf0, 0x07, 0x00, 0x01, 0x20, 0x05, 0x00};
    CHECK(decode_presence(past_end, sizeof past_end, &out) == WireError::truncated);
    // A value size of 65535 must refuse without reading past the record.
    const uint8_t enormous[] = {0xf0, 0x07, 0x00, 0x01, 0x20, 0xff, 0xff};
    CHECK(decode_presence(enormous, sizeof enormous, &out) == WireError::truncated);
    const uint8_t other_type[] = {0x05, 0x04, 0x00, 0x01};
    CHECK(decode_presence(other_type, sizeof other_type, &out) == WireError::invalid_type);
}

void append_and_find() {
    start_case("append_and_find");
    PresenceMapPack longest;
    longest.key.assign(64, 'k');
    longest.release = 9;
    longest.sha256 = filled(0x5a);
    longest.registry.assign(32, 'r');
    longest.descriptor_url.assign(255, 'u');
    longest.name.assign(64, 'n');
    longest.version.assign(32, 'v');
    uint8_t pack[presence_map_pack_field_max_bytes];
    std::size_t pack_bytes = 0;
    CHECK(encode_presence_map_pack(longest, pack, sizeof pack, &pack_bytes) == WireError::ok);
    CHECK(pack_bytes == presence_map_pack_field_max_bytes - presence_field_header_bytes);

    PresenceRecord head;
    head.engine = PresenceEngine{"0.8.0", "Windows", "x64"};
    head.sim_hash = filled(0xab);
    uint8_t record[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(head, record, sizeof record, &written) == WireError::ok);
    const auto before = written;
    std::size_t grown = 0;
    CHECK(
        append_presence_field(
            record,
            written,
            sizeof record,
            PresenceTag::map_pack,
            std::span<const uint8_t>(pack, pack_bytes),
            &grown
        ) == WireError::ok
    );
    CHECK(grown == before + presence_field_header_bytes + pack_bytes);
    CHECK(load_u16(record + 1) == grown);
    std::span<const uint8_t> found;
    CHECK(find_presence_field(record, grown, PresenceTag::map_pack, &found));
    CHECK(found.size() == pack_bytes && std::memcmp(found.data(), pack, pack_bytes) == 0);
    const auto hash = presence_sim_hash(record, grown);
    CHECK(hash == filled(0xab));

    uint8_t second[32];
    std::memset(second, 0x44, sizeof second);
    std::size_t with_second = 0;
    CHECK(
        append_presence_field(
            record,
            grown,
            sizeof record,
            PresenceTag::sim_hash,
            std::span<const uint8_t>(second, sizeof second),
            &with_second
        ) == WireError::ok
    );
    CHECK(presence_sim_hash(record, with_second) == filled(0xab));
    CHECK(find_presence_field(record, with_second, PresenceTag::sim_hash, &found));
    CHECK(found.size() == 32 && found[0] == 0xab);

    const auto room = presence_record_max_bytes - presence_map_pack_field_max_bytes;
    PresenceRecord crowded = sample_record();
    crowded.map_pack.reset();
    crowded.game_hacks = PresenceHacks{200, {}};
    crowded.game_hacks->ids.assign(200, std::string(24, 'b'));
    crowded.view_hacks = PresenceHacks{80, {}};
    crowded.view_hacks->ids.assign(80, std::string(12, 'c'));
    uint8_t limited[presence_record_max_bytes];
    std::size_t limited_bytes = 0;
    CHECK(encode_presence(crowded, limited, room, &limited_bytes) == WireError::ok);
    CHECK(limited_bytes <= room);
    CHECK(limited_bytes + presence_map_pack_field_max_bytes <= presence_record_max_bytes);
    std::size_t packed = 0;
    CHECK(
        append_presence_field(
            limited,
            limited_bytes,
            presence_record_max_bytes,
            PresenceTag::map_pack,
            std::span<const uint8_t>(pack, pack_bytes),
            &packed
        ) == WireError::ok
    );
    CHECK(packed == limited_bytes + presence_map_pack_field_max_bytes);
    CHECK(packed <= presence_record_max_bytes);

    PresenceHacks exact;
    exact.total = 35;
    exact.ids.assign(35, std::string(28, 'a'));
    PresenceRecord full;
    full.game_hacks = exact;
    std::size_t full_bytes = 0;
    CHECK(encode_presence(full, record, sizeof record, &full_bytes) == WireError::ok);
    CHECK(full_bytes == presence_record_max_bytes);
    CHECK(
        append_presence_field(
            record,
            full_bytes,
            sizeof record,
            PresenceTag::engine,
            std::span<const uint8_t>(),
            &grown
        ) == WireError::buffer_too_small
    );
    CHECK(load_u16(record + 1) == presence_record_max_bytes);
    // 1024 is 0x0400, so the low length byte is already zero. Clear the high
    // byte and the length word no longer matches the buffer.
    record[2] = 0;
    CHECK(find_presence_field(record, full_bytes, PresenceTag::game_hacks, &found) == false);
}

struct Split {
    WireError error{WireError::ok};
    std::vector<std::vector<uint8_t>> records;
};

Split split_frame(bool presence, const std::vector<uint8_t>& frame) {
    auto ring = std::make_unique<PeerRecords>();
    ring->presence_records = presence;
    UnpackOutcome outcome{};
    Split out;
    out.error =
        unpack_frame_records(ring.get(), frame.data(), frame.size(), 5, 1, 0, false, &outcome);
    const uint8_t* data = nullptr;
    uint16_t length = 0;
    while (pop_due_record(ring.get(), 5, &data, &length))
        out.records.emplace_back(data, data + length);
    return out;
}

std::vector<uint8_t> frame_of(std::initializer_list<std::vector<uint8_t>> records) {
    std::vector<uint8_t> frame{0xff, 0xff, 0xff, 0xff};
    for (const auto& record : records)
        frame.insert(frame.end(), record.begin(), record.end());
    return frame;
}

void frames_split_a_lone_presence_record() {
    start_case("frames_split_a_lone_presence_record");
    const std::vector<uint8_t> presence(std::begin(kSmallRecord), std::end(kSmallRecord));
    const auto alone = frame_of({presence});
    const auto with = split_frame(true, alone);
    CHECK(with.error == WireError::ok && with.records.size() == 1);
    CHECK(with.records[0] == presence);
    const auto without = split_frame(false, alone);
    CHECK(without.error == WireError::ok && without.records.empty());

    std::vector<uint8_t> chat(65, 0);
    chat[0] = 0x05;
    const auto chat_then = split_frame(true, frame_of({chat, presence}));
    CHECK(chat_then.records.size() == 1 && chat_then.records[0] == chat);
    CHECK(split_frame(false, frame_of({chat, presence})).records == chat_then.records);

    auto presence_then = frame_of({presence});
    presence_then.push_back(0x05);
    CHECK(split_frame(true, presence_then).records.empty());

    const std::vector<uint8_t> over{0xff, 0xff, 0xff, 0xff, 0xf0, 0x64, 0x00, 0x01};
    CHECK(split_frame(true, over).records.empty());

    std::vector<uint8_t> ping(13, 0);
    ping[0] = 0x02;
    const std::vector<uint8_t> probe{0x06};
    const auto classic = frame_of({ping, probe});
    const auto flag_on = split_frame(true, classic);
    const auto flag_off = split_frame(false, classic);
    CHECK(flag_on.error == WireError::ok && flag_off.error == WireError::ok);
    CHECK(flag_on.records.size() == 2 && flag_on.records[0].size() == 13);
    CHECK(flag_on.records[1].size() == 1 && flag_on.records == flag_off.records);
}

void only_oa_blocks_are_peers() {
    start_case("only_oa_blocks_are_peers");
    CHECK(!presence_peer(nullptr));
    uint8_t block[player_info_block_bytes]{};
    CHECK(!presence_peer(block));
    mark_engine_signature(block);
    CHECK(presence_peer(block));
}

// An engine field of 0.8.0 / Windows / x64, then a content field saying the
// machine can fetch content: header 4 + engine field 21 + content field 4 =
// 29. The bytes are counted from the layout.
constexpr uint8_t kContentRecord[] = {
    0xf0, 0x1d, 0x00, 0x01, 0x01, 0x12, 0x00, 0x05, '0', '.', '8',  '.',  '0',  0x07, 'W',
    'i',  'n',  'd',  'o',  'w',  's',  0x03, 'x',  '6', '4', 0x30, 0x01, 0x00, 0x01,
};
static_assert(sizeof kContentRecord == 29);

// The content field, tag 0x30: written right after the engine field as one
// byte of flags, read back, bits 1 to 7 ignored, and left out when its value
// is not one byte.
void the_content_field() {
    start_case("the content field");
    PresenceRecord record;
    record.engine = PresenceEngine{"0.8.0", "Windows", "x64"};
    record.content = PresenceContent{true};
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    CHECK(written == sizeof kContentRecord);
    CHECK(std::memcmp(out, kContentRecord, sizeof kContentRecord) == 0);
    PresenceRecord decoded;
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded == record);
    CHECK(decoded.content && decoded.content->fetches);

    record.content = PresenceContent{false};
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    uint8_t without[sizeof kContentRecord];
    std::memcpy(without, kContentRecord, sizeof without);
    without[sizeof without - 1] = 0x00;
    CHECK(written == sizeof without && std::memcmp(out, without, sizeof without) == 0);
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded == record);
    CHECK(decoded.content && !decoded.content->fetches);

    // Bits 1 to 7 are ignored when read.
    Value engine;
    engine.str("0.8.0");
    engine.str("Windows");
    engine.str("x64");
    Value sim;
    sim.raw(filled(0xab).data(), 32);
    Raw high_bits;
    high_bits.field(0x01, engine.bytes);
    high_bits.field(0x30, {0x03});
    high_bits.finish();
    CHECK(
        decode_presence(high_bits.bytes.data(), high_bits.bytes.size(), &decoded) == WireError::ok
    );
    CHECK(decoded.content == PresenceContent{true});

    // A value of 0 or 2 bytes breaks the field's rule: it is left out and the
    // rest is kept.
    for (const auto& value : {std::vector<uint8_t>{}, std::vector<uint8_t>{0x01, 0x00}}) {
        Raw bad;
        bad.field(0x01, engine.bytes);
        bad.field(0x30, value);
        bad.field(0x20, sim.bytes);
        bad.finish();
        PresenceRecord kept;
        CHECK(decode_presence(bad.bytes.data(), bad.bytes.size(), &kept) == WireError::ok);
        CHECK(!kept.content);
        CHECK(kept.engine == PresenceEngine("0.8.0", "Windows", "x64"));
        CHECK(kept.sim_hash == filled(0xab));
    }

    // A record without the field says nothing of it.
    Raw silent;
    silent.field(0x01, engine.bytes);
    silent.finish();
    CHECK(decode_presence(silent.bytes.data(), silent.bytes.size(), &decoded) == WireError::ok);
    CHECK(!decoded.content);
    CHECK(decoded.engine == PresenceEngine("0.8.0", "Windows", "x64"));
}

PresenceRecord content_record(const char* base, std::vector<std::string> expansions, bool differs) {
    PresenceRecord record;
    record.engine = PresenceEngine{"0.8.0", "Linux", "x64"};
    record.sim_hash = filled(0x11);
    record.bundles = PresenceBundles{base, std::move(expansions), differs};
    record.language = "English";
    return record;
}

std::vector<uint8_t> encoded(const PresenceRecord& record) {
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    return {out, out + written};
}

// The bundles and language fields round-trip, a broken bundles value is left
// out, and music stays on the wire but is not part of the comparison or the
// digest. A record without the field does not differ. The small golden
// record, which has neither field, is covered above.
void the_bundles_and_the_language() {
    start_case("the_bundles_and_the_language");
    const auto record = content_record("totala", {"campaign", "battle-tactics", "music"}, false);
    const auto bytes = encoded(record);
    PresenceRecord decoded;
    CHECK(decode_presence(bytes.data(), bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.bundles == record.bundles);
    CHECK(decoded.language == "English");
    CHECK(decoded.engine == record.engine);

    PresenceRecord hostile = record;
    hostile.bundles->expansions = {"music", "campaign"};
    const auto hostile_bytes = encoded(hostile);
    PresenceRecord kept;
    CHECK(decode_presence(hostile_bytes.data(), hostile_bytes.size(), &kept) == WireError::ok);
    CHECK(kept.bundles && kept.bundles->expansions.size() == 2);
    const auto plain = content_record("totala", {"campaign"}, false);
    const auto plain_bytes = encoded(plain);
    CHECK(presence_content_match(hostile_bytes, plain_bytes) == PresenceContentMatch::same);
    const auto other = encoded(content_record("totala", {"campaign", "core-contingency"}, false));
    CHECK(presence_content_match(plain_bytes, other) == PresenceContentMatch::differs);
    const auto demo = encoded(content_record("totala-demo", {"campaign"}, false));
    CHECK(presence_content_match(plain_bytes, demo) == PresenceContentMatch::differs);
    const auto flagged = encoded(content_record("totala", {"campaign"}, true));
    CHECK(presence_content_match(plain_bytes, flagged) == PresenceContentMatch::differs);
    const auto both_flagged = encoded(content_record("totala", {"campaign"}, true));
    CHECK(presence_content_match(flagged, both_flagged) == PresenceContentMatch::differs);
    PresenceRecord quiet = plain;
    quiet.bundles.reset();
    const auto quiet_bytes = encoded(quiet);
    CHECK(presence_content_match(plain_bytes, quiet_bytes) == PresenceContentMatch::unknown);
    CHECK(presence_content_match({}, plain_bytes) == PresenceContentMatch::unknown);

    const auto gap = presence_content_gap(
        content_record("totala", {"campaign", "music"}, false),
        content_record("totala", {"campaign", "battle-tactics"}, true)
    );
    CHECK(gap.comparable && gap.hashes_differ && !gap.bases_differ);
    CHECK(gap.missing.size() == 1 && gap.missing[0] == "battle-tactics");
    CHECK(gap.extra.empty());

    uint8_t identity[presence_content_identity_max_bytes];
    std::size_t written = 0;
    const auto swapped = content_record("totala", {"campaign", "battle-tactics"}, false);
    const auto ordered = content_record("totala", {"battle-tactics", "campaign", "music"}, false);
    CHECK(presence_content_identity(swapped, identity, sizeof identity, &written) == WireError::ok);
    const std::string first(reinterpret_cast<const char*>(identity), written);
    CHECK(presence_content_identity(ordered, identity, sizeof identity, &written) == WireError::ok);
    const std::string second(reinterpret_cast<const char*>(identity), written);
    // totala, battle-tactics, campaign, an extra zero, and the flag byte 0.
    const char canonical[] = {'t', 'o', 't', 'a', 'l', 'a', 0,   'b', 'a', 't', 't',
                              'l', 'e', '-', 't', 'a', 'c', 't', 'i', 'c', 's', 0,
                              'c', 'a', 'm', 'p', 'a', 'i', 'g', 'n', 0,   0,   0};
    CHECK(written == sizeof canonical);
    CHECK(std::memcmp(identity, canonical, sizeof canonical) == 0);
    CHECK(first == second);
    CHECK(presence_content_identity(quiet, identity, sizeof identity, &written) == WireError::ok);
    CHECK(written == 0);

    Raw broken;
    Value engine;
    engine.str("0.8.0");
    engine.str("Linux");
    engine.str("x64");
    broken.field(0x01, engine.bytes);
    Value bad;
    bad.u8(0);
    bad.u8(3);
    bad.raw("no", 2);
    broken.field(0x31, bad.bytes);
    broken.finish();
    CHECK(decode_presence(broken.bytes.data(), broken.bytes.size(), &decoded) == WireError::ok);
    CHECK(!decoded.bundles);
    CHECK(decoded.engine == PresenceEngine("0.8.0", "Linux", "x64"));
    CHECK(presence_content_match(broken.bytes, plain_bytes) == PresenceContentMatch::unknown);

    PresenceRecord too_many = plain;
    too_many.bundles->expansions.assign(presence_bundle_count_max + 1, "campaign");
    uint8_t out[presence_record_max_bytes];
    CHECK(encode_presence(too_many, out, sizeof out, &written) == WireError::bad_argument);
}

std::array<uint8_t, presence_mutator_prefix_bytes> prefix_of(uint8_t byte) {
    std::array<uint8_t, presence_mutator_prefix_bytes> prefix{};
    prefix.fill(byte);
    return prefix;
}

std::string two_digits(int number) {
    std::string text = std::to_string(number);
    if (text.size() == 1)
        text.insert(text.begin(), '0');
    return text;
}

void put_states(Value& value, uint64_t states) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        value.u8(static_cast<uint8_t>(states >> shift));
}

void put_mutator_item(Value& value, std::string_view id, std::string_view version) {
    value.str(id);
    value.str(version);
    const auto prefix = prefix_of(0xab);
    value.raw(prefix.data(), prefix.size());
    value.u8(0);
}

Value valid_part_value() {
    Value part;
    part.raw(filled(0x11).data(), 32);
    part.u8(0);
    put_mutator_item(part, "kept-part", "1");
    return part;
}

// Three mutators with options, and a guest field whose positions use every
// state code, both come back unchanged. So does a part carried with the host.
void mutators_round_trip() {
    start_case("mutators_round_trip");
    PresenceMutator first;
    first.id = "ridge-wind";
    first.version = "1.2";
    first.sha256_prefix = prefix_of(0x11);
    first.options = {PresenceMutatorOption{"rate", "2"}, PresenceMutatorOption{"mode", "high"}};
    PresenceMutator second;
    second.id = "swift-yard";
    second.version = "4";
    second.sha256_prefix = prefix_of(0x22);
    second.options = {PresenceMutatorOption{"cap", "8"}};
    PresenceMutator third;
    third.id = "open-keel";
    third.version = "";
    third.sha256_prefix = prefix_of(0x33);
    PresenceMutator fourth;
    fourth.id = "late-loom";
    fourth.version = "9";
    fourth.sha256_prefix = prefix_of(0x44);
    fourth.options = {PresenceMutatorOption{"bias", "1"}};

    PresenceRecord host;
    host.engine = PresenceEngine{"0.8.0", "Windows", "x64"};
    host.mutators = PresenceMutators{};
    host.mutators->built = true;
    host.mutators->digest = filled(0x51);
    host.mutators->layer_digest = filled(0x52);
    host.mutators->total = 4;
    host.mutators->held = 4;
    host.mutators->mutators = {first, second, third};
    host.mutator_part = PresenceMutatorPart{};
    host.mutator_part->digest = filled(0x51);
    host.mutator_part->first = 3;
    host.mutator_part->mutators = {fourth};

    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(host, out, sizeof out, &written) == WireError::ok);
    PresenceRecord decoded;
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded == host);
    CHECK(presence_mutator_state(decoded.mutators->states, 0) == 0);

    PresenceRecord guest;
    guest.mutators = PresenceMutators{};
    guest.mutators->lacking = true;
    guest.mutators->digest = filled(0x44);
    guest.mutators->index = 1;
    guest.mutators->total = 4;
    guest.mutators->held = 2;
    presence_mutator_set_state(guest.mutators->states, 0, 0);
    presence_mutator_set_state(guest.mutators->states, 1, 1);
    presence_mutator_set_state(guest.mutators->states, 2, 2);
    presence_mutator_set_state(guest.mutators->states, 3, 3);
    CHECK(encode_presence(guest, out, sizeof out, &written) == WireError::ok);
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded == guest);
    CHECK(decoded.mutators->mutators.empty());
    CHECK(presence_mutator_state(decoded.mutators->states, 0) == 0);
    CHECK(presence_mutator_state(decoded.mutators->states, 1) == 1);
    CHECK(presence_mutator_state(decoded.mutators->states, 2) == 2);
    CHECK(presence_mutator_state(decoded.mutators->states, 3) == 3);
    CHECK(presence_mutator_state(decoded.mutators->states, 4) == 0);
}

// The bytes of a small mutators field, written one by one, decode to that
// field and encode back to the same bytes.
void a_small_mutators_field_is_encoded_exactly() {
    start_case("a_small_mutators_field_is_encoded_exactly");
    Raw record;
    Value field;
    field.u8(0x01);
    field.raw(filled(0x11).data(), 32);
    field.raw(filled(0x22).data(), 32);
    field.u8(presence_mutator_index_none);
    field.u8(1);
    field.u8(1);
    put_states(field, 0);
    field.str("ridge-wind");
    field.str("1");
    const auto prefix = prefix_of(0xab);
    field.raw(prefix.data(), prefix.size());
    field.u8(1);
    field.str("rate");
    field.str("2");
    record.field(static_cast<uint8_t>(PresenceTag::mutators), field.bytes);
    record.finish();

    PresenceRecord decoded;
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.mutators);
    CHECK(decoded.mutators->built && !decoded.mutators->lacking && !decoded.mutators->failed);
    CHECK(decoded.mutators->digest == filled(0x11));
    CHECK(decoded.mutators->layer_digest == filled(0x22));
    CHECK(decoded.mutators->index == presence_mutator_index_none);
    CHECK(decoded.mutators->total == 1 && decoded.mutators->held == 1);
    CHECK(decoded.mutators->states == 0);
    CHECK(decoded.mutators->mutators.size() == 1);
    const auto& item = decoded.mutators->mutators[0];
    CHECK(item.id == "ridge-wind" && item.version == "1");
    CHECK(item.sha256_prefix == prefix);
    const PresenceMutatorOption rate{"rate", "2"};
    CHECK(item.options.size() == 1 && item.options[0] == rate);

    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(decoded, out, sizeof out, &written) == WireError::ok);
    CHECK(written == record.bytes.size());
    CHECK(std::memcmp(out, record.bytes.data(), written) == 0);
}

// Thirty-two mutators in the 533-byte budget are cut whole. Hack ids and
// developer overrides lose the room before any mutator does, and total stays.
void a_full_loadout_is_cut_before_hack_ids() {
    start_case("a_full_loadout_is_cut_before_hack_ids");
    PresenceRecord record;
    record.engine = PresenceEngine{"0.8.0", "Windows", "x64"};
    record.sim_hash = filled(0xab);
    record.mutators = PresenceMutators{};
    record.mutators->total = 32;
    record.mutators->held = 32;
    for (int number = 0; number < 32; ++number) {
        PresenceMutator item;
        item.id = "aaaa-bbbb-" + two_digits(number);
        item.sha256_prefix = prefix_of(0xcd);
        record.mutators->mutators.push_back(item);
    }
    CHECK(record.mutators->mutators[0].id.size() == 12);
    const std::string hack(20, 'a');
    record.developer = PresenceDeveloper{true, 1, {PresenceOverride{hack, true}}};
    record.game_hacks = PresenceHacks{3, {hack, hack, hack}};

    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, 533, &written) == WireError::ok);
    // Header 4, engine 21, sim 35, mutators fixed field 79, developer prefix 6
    // and hack prefix 5 leave 383 bytes. Each mutator is 23, so 16 fit.
    CHECK(written == 518);
    PresenceRecord decoded;
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded.mutators && decoded.mutators->total == 32);
    CHECK(decoded.mutators->mutators.size() == 16);
    for (std::size_t cursor = 0; cursor < 16; ++cursor)
        CHECK(decoded.mutators->mutators[cursor] == record.mutators->mutators[cursor]);
    CHECK(decoded.developer && decoded.developer->on && decoded.developer->total == 1);
    CHECK(decoded.developer->overrides.empty());
    CHECK(decoded.game_hacks && decoded.game_hacks->total == 3);
    CHECK(decoded.game_hacks->ids.empty());
    CHECK(decoded.engine == record.engine);
    CHECK(decoded.sim_hash == record.sim_hash);
}

PresenceMutator wide_mutator(int number) {
    PresenceMutator item;
    item.id = "mutator-sample" + two_digits(number);
    item.version = "1234567890123";
    item.sha256_prefix = prefix_of(static_cast<uint8_t>(number));
    return item;
}

// The mutators that miss the 533-byte head travel in two parts, and each
// part's field fits beside a 533-byte source and beside a full map-pack field.
void mutator_parts_carry_the_rest() {
    start_case("mutator_parts_carry_the_rest");
    PresenceMutators loadout;
    loadout.digest = filled(0x44);
    loadout.total = 32;
    loadout.held = 32;
    for (int number = 0; number < 32; ++number)
        loadout.mutators.push_back(wide_mutator(number));
    CHECK(loadout.mutators[0].id.size() == 16);
    CHECK(loadout.mutators[0].version.size() == 13);

    PresenceRecord head;
    head.mutators = loadout;
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(head, out, 533, &written) == WireError::ok);
    CHECK(written == 523);
    PresenceRecord decoded;
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded.mutators && decoded.mutators->total == 32);
    CHECK(decoded.mutators->mutators.size() == 11);

    const std::size_t beside_source = presence_record_max_bytes - 533;
    const std::size_t beside_pack = presence_record_max_bytes - presence_map_pack_field_max_bytes;
    const std::size_t part_field_room = beside_source < beside_pack ? beside_source : beside_pack;
    const std::size_t part_value_room = part_field_room - presence_field_header_bytes;
    uint8_t part_out[presence_record_max_bytes];
    std::size_t part_written = 0;
    uint8_t sent = 0;
    CHECK(
        encode_presence_mutator_part(
            loadout, 11, part_out, part_value_room, &part_written, &sent
        ) == WireError::ok
    );
    CHECK(sent == 11 && part_written == 473);
    CHECK(presence_field_header_bytes + part_written <= beside_source);
    CHECK(presence_field_header_bytes + part_written <= beside_pack);
    uint8_t second[presence_record_max_bytes];
    std::size_t second_written = 0;
    uint8_t second_sent = 0;
    CHECK(
        encode_presence_mutator_part(
            loadout,
            static_cast<uint8_t>(11 + sent),
            second,
            part_value_room,
            &second_written,
            &second_sent
        ) == WireError::ok
    );
    CHECK(second_sent == 10 && second_written == 433);
    CHECK(presence_field_header_bytes + second_written <= beside_source);
    CHECK(presence_field_header_bytes + second_written <= beside_pack);
    CHECK(static_cast<std::size_t>(11 + sent + second_sent) == loadout.mutators.size());

    uint8_t storage[presence_record_max_bytes];
    std::memcpy(storage, out, written);
    std::size_t grown = 0;
    CHECK(
        append_presence_field(
            storage,
            written,
            sizeof storage,
            PresenceTag::mutator_part,
            std::span<const uint8_t>(part_out, part_written),
            &grown
        ) == WireError::ok
    );
    CHECK(grown == written + presence_field_header_bytes + part_written);
    CHECK(grown <= presence_record_max_bytes);

    PresenceMapPack longest;
    longest.key.assign(64, 'k');
    longest.release = 9;
    longest.sha256 = filled(0x5a);
    longest.registry.assign(32, 'r');
    longest.descriptor_url.assign(255, 'u');
    longest.name.assign(64, 'n');
    longest.version.assign(32, 'v');
    uint8_t pack[presence_map_pack_field_max_bytes];
    std::size_t pack_bytes = 0;
    CHECK(encode_presence_map_pack(longest, pack, sizeof pack, &pack_bytes) == WireError::ok);
    CHECK(presence_field_header_bytes + pack_bytes == presence_map_pack_field_max_bytes);
    Raw base;
    base.finish();
    std::memcpy(storage, base.bytes.data(), base.bytes.size());
    std::size_t size = base.bytes.size();
    CHECK(
        append_presence_field(
            storage,
            size,
            sizeof storage,
            PresenceTag::map_pack,
            std::span<const uint8_t>(pack, pack_bytes),
            &size
        ) == WireError::ok
    );
    CHECK(
        append_presence_field(
            storage,
            size,
            sizeof storage,
            PresenceTag::mutator_part,
            std::span<const uint8_t>(part_out, part_written),
            &size
        ) == WireError::ok
    );
    CHECK(
        size == base.bytes.size() + presence_map_pack_field_max_bytes +
                    presence_field_header_bytes + part_written
    );
    CHECK(size <= presence_record_max_bytes);

    CHECK(
        encode_presence_mutator_part(
            loadout, 32, part_out, sizeof part_out, &part_written, &sent
        ) == WireError::bad_argument
    );
    CHECK(
        encode_presence_mutator_part(loadout, 0, part_out, 1, &part_written, &sent) ==
        WireError::buffer_too_small
    );
    auto broken = loadout;
    broken.mutators[11].id = "Not-kebab";
    CHECK(
        encode_presence_mutator_part(broken, 11, part_out, part_value_room, &part_written, &sent) ==
        WireError::bad_argument
    );
}

PresenceMutator named_mutator(std::string id, uint8_t mark) {
    PresenceMutator item;
    item.id = std::move(id);
    item.version = "1";
    item.sha256_prefix = prefix_of(mark);
    return item;
}

PresenceRecord host_head(const PresenceDigest& digest, const PresenceMutator& item) {
    PresenceRecord record;
    record.mutators = PresenceMutators{};
    record.mutators->built = true;
    record.mutators->digest = digest;
    record.mutators->layer_digest = filled(0x52);
    record.mutators->total = 4;
    record.mutators->held = 4;
    record.mutators->mutators = {item};
    return record;
}

// Parts in order, out of order, twice, and of another digest. The list is
// whole only when every mutator has arrived, and a new digest starts again.
void the_loadout_assembles_from_the_hosts_records() {
    start_case("the_loadout_assembles_from_the_hosts_records");
    const auto digest = filled(0x51);
    const auto first = named_mutator("one", 1);
    const auto second = named_mutator("two", 2);
    const auto third = named_mutator("three", 3);
    const auto fourth = named_mutator("four", 4);
    const auto head = host_head(digest, first);

    auto with_part = [&](uint8_t at, std::vector<PresenceMutator> items) {
        PresenceRecord record = head;
        record.mutator_part = PresenceMutatorPart{};
        record.mutator_part->digest = digest;
        record.mutator_part->first = at;
        record.mutator_part->mutators = std::move(items);
        return record;
    };
    const auto part_late = with_part(2, {third});
    const auto part_next = with_part(1, {second});
    const auto part_rest = with_part(2, {third, fourth});
    auto other = with_part(2, {third});
    other.mutator_part->digest = filled(0x99);
    const auto foreign = host_head(filled(0x77), first);

    PresenceLoadoutAssembly assembly;
    CHECK(presence_assembly_take(assembly, head));
    CHECK(assembly.mutators.size() == 1 && assembly.total == 4);
    CHECK(!presence_assembly_whole(assembly));
    CHECK(!presence_assembly_take(assembly, head));
    CHECK(!presence_assembly_take(assembly, part_late));
    CHECK(assembly.mutators.size() == 1);
    CHECK(presence_assembly_take(assembly, part_next));
    CHECK(assembly.mutators.size() == 2 && assembly.mutators[1] == second);
    CHECK(!presence_assembly_whole(assembly));
    CHECK(!presence_assembly_take(assembly, other));
    CHECK(presence_assembly_take(assembly, part_rest));
    CHECK(assembly.mutators.size() == 4);
    CHECK(assembly.mutators[2] == third && assembly.mutators[3] == fourth);
    CHECK(presence_assembly_whole(assembly));
    CHECK(!presence_assembly_take(assembly, part_rest));
    CHECK(presence_assembly_whole(assembly));

    PresenceLoadoutAssembly in_order;
    CHECK(presence_assembly_take(in_order, head));
    CHECK(presence_assembly_take(in_order, part_next));
    CHECK(!presence_assembly_whole(in_order));
    CHECK(presence_assembly_take(in_order, part_rest));
    CHECK(presence_assembly_whole(in_order));
    CHECK(in_order.digest == digest);

    CHECK(presence_assembly_take(assembly, foreign));
    CHECK(assembly.digest == filled(0x77));
    CHECK(assembly.mutators.size() == 1 && assembly.mutators[0] == first);
    CHECK(!presence_assembly_whole(assembly));

    PresenceRecord quiet;
    quiet.engine = PresenceEngine{"0.8.0", "Linux", "x64"};
    CHECK(presence_assembly_take(assembly, quiet));
    CHECK(assembly.mutators.empty() && assembly.total == 0);
    CHECK(assembly.digest == PresenceDigest{});
    CHECK(!presence_assembly_whole(assembly));
    CHECK(!presence_assembly_take(assembly, quiet));
}

void expect_field_left_out(const Value& field, uint8_t tag, bool keep_part) {
    Raw record;
    Value engine;
    engine.str("0.8.0");
    engine.str("Windows");
    engine.str("x64");
    record.field(static_cast<uint8_t>(PresenceTag::engine), engine.bytes);
    Value sim;
    sim.raw(filled(0xab).data(), 32);
    record.field(static_cast<uint8_t>(PresenceTag::sim_hash), sim.bytes);
    if (keep_part)
        record.field(static_cast<uint8_t>(PresenceTag::mutator_part), valid_part_value().bytes);
    else {
        Value kept;
        kept.u8(0);
        kept.raw(filled(0x11).data(), 32);
        uint8_t layer[32] = {};
        kept.raw(layer, sizeof layer);
        kept.u8(presence_mutator_index_none);
        kept.u8(1);
        kept.u8(0);
        put_states(kept, 0);
        put_mutator_item(kept, "kept-head", "1");
        record.field(static_cast<uint8_t>(PresenceTag::mutators), kept.bytes);
    }
    record.field(tag, field.bytes);
    record.finish();

    PresenceRecord decoded;
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.engine == PresenceEngine("0.8.0", "Windows", "x64"));
    CHECK(decoded.sim_hash == filled(0xab));
    if (tag == static_cast<uint8_t>(PresenceTag::mutators)) {
        CHECK(!decoded.mutators);
        CHECK(decoded.mutator_part && decoded.mutator_part->mutators.size() == 1);
        CHECK(decoded.mutator_part->mutators[0].id == "kept-part");
    } else {
        CHECK(!decoded.mutator_part);
        CHECK(decoded.mutators && decoded.mutators->mutators.size() == 1);
        CHECK(decoded.mutators->mutators[0].id == "kept-head");
    }
}

Value fixed_mutators(uint8_t total, uint8_t held, uint8_t index, uint64_t states) {
    Value field;
    field.u8(0);
    field.raw(filled(0x11).data(), 32);
    uint8_t layer[32] = {};
    field.raw(layer, sizeof layer);
    field.u8(index);
    field.u8(total);
    field.u8(held);
    put_states(field, states);
    return field;
}

// Each broken mutators or mutator-part value is left out. The record's other
// fields stay.
void a_mutator_field_that_breaks_its_rule_is_left_out() {
    start_case("a_mutator_field_that_breaks_its_rule_is_left_out");
    const auto tag = static_cast<uint8_t>(PresenceTag::mutators);
    {
        auto field = fixed_mutators(1, 0, presence_mutator_index_none, 0);
        field.str(std::string(65, 'a'));
        field.str("1");
        const auto prefix = prefix_of(0xab);
        field.raw(prefix.data(), prefix.size());
        field.u8(0);
        expect_field_left_out(field, tag, true);
    }
    {
        auto field = fixed_mutators(1, 0, presence_mutator_index_none, 0);
        put_mutator_item(field, "Bad-id", "1");
        expect_field_left_out(field, tag, true);
    }
    {
        auto field = fixed_mutators(1, 0, presence_mutator_index_none, 0);
        field.str("ridge-wind");
        field.str(std::string(33, 'v'));
        const auto prefix = prefix_of(0xab);
        field.raw(prefix.data(), prefix.size());
        field.u8(0);
        expect_field_left_out(field, tag, true);
    }
    {
        auto field = fixed_mutators(1, 0, presence_mutator_index_none, 0);
        field.str("ridge-wind");
        field.str("1");
        const auto prefix = prefix_of(0xab);
        field.raw(prefix.data(), prefix.size());
        field.u8(9);
        for (int option = 0; option < 9; ++option) {
            field.str("k");
            field.str("1");
        }
        expect_field_left_out(field, tag, true);
    }
    expect_field_left_out(fixed_mutators(0, 0, presence_mutator_index_none, 0), tag, true);
    expect_field_left_out(fixed_mutators(33, 0, presence_mutator_index_none, 0), tag, true);
    expect_field_left_out(fixed_mutators(2, 3, presence_mutator_index_none, 0), tag, true);
    expect_field_left_out(fixed_mutators(3, 0, 7, 0), tag, true);
    // Position 1 is past a total of 1, so bit 2 of the state word is refused.
    expect_field_left_out(fixed_mutators(1, 0, presence_mutator_index_none, 0x4), tag, true);
    {
        Value field;
        field.u8(0);
        field.raw(filled(0x11).data(), 32);
        field.raw(filled(0x22).data(), 32);
        field.u8(presence_mutator_index_none);
        field.u8(1);
        field.u8(0);
        put_states(field, 0);
        expect_field_left_out(field, tag, true);
    }

    Value part;
    part.raw(filled(0x11).data(), 32);
    part.u8(30);
    put_mutator_item(part, "one", "1");
    put_mutator_item(part, "two", "1");
    put_mutator_item(part, "three", "1");
    expect_field_left_out(part, static_cast<uint8_t>(PresenceTag::mutator_part), false);

    Value flags;
    flags.u8(0xf9);
    flags.raw(filled(0x11).data(), 32);
    flags.raw(filled(0x22).data(), 32);
    flags.u8(presence_mutator_index_none);
    flags.u8(1);
    flags.u8(0);
    put_states(flags, 0);
    Raw record;
    Value engine;
    engine.str("0.8.0");
    engine.str("Windows");
    engine.str("x64");
    record.field(static_cast<uint8_t>(PresenceTag::engine), engine.bytes);
    record.field(tag, flags.bytes);
    record.finish();
    PresenceRecord decoded;
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.engine == PresenceEngine("0.8.0", "Windows", "x64"));
    CHECK(decoded.mutators && decoded.mutators->built);
    CHECK(!decoded.mutators->lacking && !decoded.mutators->failed);

    PresenceRecord refused;
    refused.mutators = PresenceMutators{};
    refused.mutators->total = 0;
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(refused, out, sizeof out, &written) == WireError::bad_argument);
}

// The state reader reports the fixed part and does not need the mutators.
void presence_mutators_state_reads_the_fixed_part() {
    start_case("presence_mutators_state_reads_the_fixed_part");
    PresenceRecord record;
    record.engine = PresenceEngine{"0.8.0", "Windows", "x64"};
    record.mutators = PresenceMutators{};
    record.mutators->built = true;
    record.mutators->failed = true;
    record.mutators->digest = filled(0x51);
    record.mutators->layer_digest = filled(0x52);
    record.mutators->index = 1;
    record.mutators->total = 2;
    record.mutators->held = 1;
    presence_mutator_set_state(record.mutators->states, 1, 2);
    record.mutators->mutators = {named_mutator("one", 1), named_mutator("two", 2)};
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    const auto state = presence_mutators_state(out, written);
    CHECK(state);
    CHECK(state->built && !state->lacking && state->failed);
    CHECK(state->digest == filled(0x51) && state->layer_digest == filled(0x52));
    CHECK(state->index == 1 && state->total == 2 && state->held == 1);
    CHECK(presence_mutator_state(state->states, 1) == 2);

    CHECK(!presence_mutators_state(kSmallRecord, sizeof kSmallRecord));

    Raw broken;
    Value engine;
    engine.str("0.8.0");
    engine.str("Windows");
    engine.str("x64");
    broken.field(static_cast<uint8_t>(PresenceTag::engine), engine.bytes);
    broken.field(
        static_cast<uint8_t>(PresenceTag::mutators),
        fixed_mutators(0, 0, presence_mutator_index_none, 0).bytes
    );
    broken.finish();
    CHECK(!presence_mutators_state(broken.bytes.data(), broken.bytes.size()));
    PresenceRecord decoded;
    CHECK(decode_presence(broken.bytes.data(), broken.bytes.size(), &decoded) == WireError::ok);
    CHECK(!decoded.mutators);
    CHECK(decoded.engine == PresenceEngine("0.8.0", "Windows", "x64"));
}

// A record that names no mutators still encodes to the pinned bytes.
void a_record_without_mutators_matches_the_pinned_bytes() {
    start_case("a_record_without_mutators_matches_the_pinned_bytes");
    PresenceRecord record;
    record.engine = PresenceEngine{"0.8.0", "Windows", "x64"};
    record.sim_hash = filled(0xab);
    record.developer = PresenceDeveloper{};
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    CHECK(written == sizeof kSmallRecord);
    CHECK(std::memcmp(out, kSmallRecord, sizeof kSmallRecord) == 0);
    CHECK(!record.mutators && !record.mutator_part);
}

// The sim hash and a field search still find their bytes when mutators are
// carried too.
PresenceMutators short_loadout() {
    PresenceMutators field;
    field.digest = filled(0x51);
    field.total = 3;
    field.held = 3;
    field.mutators = {
        named_mutator("ridge-wind", 1),
        named_mutator("ridge-keel", 2),
        named_mutator("ridge-loom", 3),
    };
    return field;
}

// Every mask of a three-mutator list comes back as the same bits, and those
// bits are the clash mode of each entry.
void every_clash_mask_of_a_short_list_round_trips() {
    start_case("every_clash_mask_of_a_short_list_round_trips");
    for (uint32_t mask = 0; mask < 8; ++mask) {
        PresenceRecord host;
        host.mutators = short_loadout();
        host.mutator_modes = mask;
        uint8_t out[presence_record_max_bytes];
        std::size_t written = 0;
        CHECK(encode_presence(host, out, sizeof out, &written) == WireError::ok);
        PresenceRecord decoded;
        CHECK(decode_presence(out, written, &decoded) == WireError::ok);
        CHECK(decoded == host);
        PresenceClashMode modes[3]{};
        CHECK(presence_clash_modes(mask, 3, modes) == WireError::ok);
        for (uint8_t position = 0; position < 3; ++position) {
            const bool adds = (mask & (uint32_t{1} << position)) != 0;
            CHECK(modes[position] == (adds ? PresenceClashMode::add : PresenceClashMode::replace));
        }
    }
    PresenceClashMode refused[3]{
        PresenceClashMode::add, PresenceClashMode::add, PresenceClashMode::add
    };
    CHECK(presence_clash_modes(0x8, 3, refused) == WireError::bad_argument);
    CHECK(refused[0] == PresenceClashMode::add);
    CHECK(presence_clash_modes(0, 3, nullptr) == WireError::bad_argument);
}

// A record from a sender that has no 0x52 decodes as that record does today.
// Building the loadout with Replace for every entry then disagrees with the
// host's digest whenever any mode is Add.
void a_record_without_modes_keeps_todays_loadout() {
    start_case("a_record_without_modes_keeps_todays_loadout");
    PresenceRecord today;
    today.mutators = short_loadout();
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(today, out, sizeof out, &written) == WireError::ok);
    PresenceRecord decoded;
    CHECK(decode_presence(out, written, &decoded) == WireError::ok);
    CHECK(decoded == today);
    CHECK(!decoded.mutator_modes);

    oa::data::mutators::Loadout host;
    oa::data::mutators::LoadoutEntry wind;
    wind.id = "ridge-wind";
    wind.version = "1";
    wind.clashes = oa::formats::oamut::ClashMode::add;
    oa::data::mutators::LoadoutEntry keel;
    keel.id = "ridge-keel";
    keel.version = "1";
    host.entries = {wind, keel};
    oa::data::mutators::Loadout guest = host;
    for (auto& entry : guest.entries)
        entry.clashes = oa::formats::oamut::ClashMode::replace;
    CHECK(oa::data::mutators::loadout_digest(guest) != oa::data::mutators::loadout_digest(host));
}

// A mask that names a position past the list is not a mode the reader can use.
void a_clash_mask_past_the_list_is_refused() {
    start_case("a_clash_mask_past_the_list_is_refused");
    PresenceRecord host;
    host.mutators = short_loadout();
    host.mutator_modes = 0x8;
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(host, out, sizeof out, &written) == WireError::bad_argument);

    Raw record;
    Value field;
    field.u8(0);
    field.raw(filled(0x51).data(), 32);
    field.raw(filled(0).data(), 32);
    field.u8(presence_mutator_index_none);
    field.u8(3);
    field.u8(3);
    put_states(field, 0);
    put_mutator_item(field, "ridge-wind", "1");
    put_mutator_item(field, "ridge-keel", "1");
    put_mutator_item(field, "ridge-loom", "1");
    record.field(static_cast<uint8_t>(PresenceTag::mutators), field.bytes);
    Value mask;
    mask.u32(0x8);
    record.field(static_cast<uint8_t>(PresenceTag::mutator_modes), mask.bytes);
    record.finish();
    PresenceRecord decoded;
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.mutators && decoded.mutators->total == 3);
    CHECK(decoded.mutators->mutators.size() == 3);
    CHECK(!decoded.mutator_modes);

    Value wide;
    wide.u32(0);
    wide.u8(0);
    record = Raw{};
    record.field(static_cast<uint8_t>(PresenceTag::mutators), field.bytes);
    record.field(static_cast<uint8_t>(PresenceTag::mutator_modes), wide.bytes);
    record.finish();
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(!decoded.mutator_modes);
}

void find_and_sim_hash_see_mutator_fields() {
    start_case("find_and_sim_hash_see_mutator_fields");
    PresenceRecord record;
    record.sim_hash = filled(0xab);
    record.mutators = PresenceMutators{};
    record.mutators->digest = filled(0x51);
    record.mutators->total = 1;
    record.mutators->held = 1;
    record.mutators->mutators = {named_mutator("one", 1)};
    record.mutator_part = PresenceMutatorPart{};
    record.mutator_part->digest = filled(0x51);
    record.mutator_part->first = 0;
    record.mutator_part->mutators = {named_mutator("two", 2)};
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    CHECK(encode_presence(record, out, sizeof out, &written) == WireError::ok);
    const auto hash = presence_sim_hash(out, written);
    CHECK(hash && *hash == filled(0xab));
    std::span<const uint8_t> value;
    CHECK(find_presence_field(out, written, PresenceTag::sim_hash, &value));
    CHECK(value.size() == 32);
    CHECK(find_presence_field(out, written, PresenceTag::mutators, &value));
    CHECK(value.size() > 76);
    CHECK(find_presence_field(out, written, PresenceTag::mutator_part, &value));
    CHECK(value.size() > 33);
}

PresenceBrain sample_brain(
    uint32_t player,
    const char* id,
    const char* name,
    const char* version,
    const char* level_id,
    const char* level,
    bool full_awareness
) {
    PresenceBrain brain;
    brain.player = player;
    brain.id = id;
    brain.name = name;
    brain.version = version;
    brain.level_id = level_id;
    brain.level = level;
    brain.full_awareness = full_awareness;
    return brain;
}

void write_sample_item(Raw& raw, const PresenceBrain& brain) {
    raw.u32(brain.player);
    raw.str(brain.id);
    raw.str(brain.name);
    raw.str(brain.version);
    raw.str(brain.level_id);
    raw.str(brain.level);
    raw.u8(brain.full_awareness ? 0x01 : 0x00);
}

void computer_brains_round_trip() {
    start_case("computer brains round trip");
    const PresenceBrain brains[] = {
        sample_brain(0x11, "steady-commander", "Steady Commander", "1.2", "firm", "Firm", false),
        sample_brain(0x22, "lantern-scout", "Lantern Scout", "", "wide", "Wide", true),
        sample_brain(0x33, "ridge-keeper", "Ridge Keeper", "0.4", "calm", "Calm", false),
    };
    uint8_t out[presence_record_max_bytes];
    std::size_t written = 0;
    std::size_t sent = 0;
    CHECK(
        encode_presence_brains(brains, out, sizeof out, &written, &sent) == WireError::ok &&
        sent == 3
    );
    const auto decoded = decode_presence_brains(std::span<const uint8_t>(out, written));
    CHECK(decoded && decoded->size() == 3);
    if (decoded && decoded->size() == 3) {
        CHECK((*decoded)[0] == brains[0]);
        CHECK((*decoded)[1] == brains[1]);
        CHECK((*decoded)[2] == brains[2]);
    }

    Raw exact;
    exact.u8(1);
    write_sample_item(exact, brains[0]);
    written = 0;
    sent = 0;
    CHECK(
        encode_presence_brains(
            std::span<const PresenceBrain>(brains, 1), out, sizeof out, &written, &sent
        ) == WireError::ok &&
        sent == 1 && written == exact.bytes.size() - 4
    );
    CHECK(std::equal(out, out + written, exact.bytes.begin() + 4));

    PresenceBrain long_name = brains[0];
    long_name.name = std::string(30, 'a') + "\xc3\xa9" + "zz";
    written = 0;
    sent = 0;
    CHECK(
        encode_presence_brains(
            std::span<const PresenceBrain>(&long_name, 1), out, sizeof out, &written, &sent
        ) == WireError::ok
    );
    const auto cut = decode_presence_brains(std::span<const uint8_t>(out, written));
    CHECK(cut && cut->size() == 1 && (*cut)[0].name == std::string(30, 'a') + "\xc3\xa9");

    std::size_t one = 0;
    std::size_t one_sent = 0;
    CHECK(
        encode_presence_brains(
            std::span<const PresenceBrain>(brains, 1), out, sizeof out, &one, &one_sent
        ) == WireError::ok
    );
    written = 0;
    sent = 99;
    CHECK(
        encode_presence_brains(brains, out, one, &written, &sent) == WireError::ok && sent == 1 &&
        written == one
    );
    CHECK(
        encode_presence_brains(brains, out, one - 1, &written, &sent) == WireError::buffer_too_small
    );
}

void computer_brains_refuse_a_broken_value() {
    start_case("computer brains refuse a broken value");
    const auto item =
        sample_brain(7, "steady-commander", "Steady Commander", "1.2", "firm", "Firm", false);
    auto broken = [&](auto fill) {
        Raw raw;
        fill(raw);
        return decode_presence_brains(
            std::span<const uint8_t>(raw.bytes.data() + 4, raw.bytes.size() - 4)
        );
    };
    CHECK(!broken([&](Raw& raw) { raw.u8(0); }));
    CHECK(!broken([&](Raw& raw) {
        raw.u8(11);
        for (int index = 0; index < 11; ++index)
            write_sample_item(raw, item);
    }));
    CHECK(!broken([&](Raw& raw) {
        raw.u8(1);
        auto upper = item;
        upper.id = "Steady-commander";
        write_sample_item(raw, upper);
    }));
    CHECK(!broken([&](Raw& raw) {
        raw.u8(1);
        auto upper = item;
        upper.level_id = "Firm";
        write_sample_item(raw, upper);
    }));
    CHECK(!broken([&](Raw& raw) {
        raw.u8(1);
        raw.u32(item.player);
        raw.str(item.id);
        raw.str(item.name);
        raw.str(item.version);
        raw.str("");
        raw.str(item.level);
        raw.u8(0);
    }));
    CHECK(!broken([&](Raw& raw) {
        raw.u8(1);
        raw.u32(item.player);
        raw.str(item.id);
        raw.str(item.name);
        raw.str(item.version);
        raw.str(item.level_id);
        raw.str("");
        raw.u8(0);
    }));
    CHECK(!broken([&](Raw& raw) {
        raw.u8(1);
        auto flagged = item;
        write_sample_item(raw, flagged);
        raw.bytes.back() = 0x02;
    }));
    CHECK(!broken([&](Raw& raw) {
        raw.u8(1);
        raw.u8(0x11);
        raw.u8(0x00);
    }));
}

void a_record_with_computer_brains_keeps_the_other_fields() {
    start_case("a record with computer brains keeps the other fields");
    const auto item =
        sample_brain(0x42, "steady-commander", "Steady Commander", "1.2", "firm", "Firm", false);
    Value computers;
    computers.u32(0x42);
    computers.u8(2);
    Value brains;
    brains.u8(1);
    Raw writer;
    write_sample_item(writer, item);
    brains.raw(writer.bytes.data() + 4, writer.bytes.size() - 4);

    Raw record;
    record.u8(static_cast<uint8_t>(PresenceTag::sim_hash));
    record.u16(32);
    for (int index = 0; index < 32; ++index)
        record.u8(0xab);
    record.field(static_cast<uint8_t>(PresenceTag::computers), computers.bytes);
    record.field(static_cast<uint8_t>(PresenceTag::computer_brains), brains.bytes);
    record.finish();

    std::span<const uint8_t> value;
    CHECK(
        find_presence_field(record.bytes.data(), record.bytes.size(), PresenceTag::sim_hash, &value)
    );
    CHECK(value.size() == 32 && value[0] == 0xab);
    CHECK(find_presence_field(
        record.bytes.data(), record.bytes.size(), PresenceTag::computers, &value
    ));
    CHECK(value.size() == 5);
    PresenceRecord decoded;
    CHECK(decode_presence(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.sim_hash && (*decoded.sim_hash)[0] == 0xab);
    const auto hash = presence_sim_hash(record.bytes.data(), record.bytes.size());
    CHECK(hash && (*hash)[0] == 0xab);

    Raw refused;
    refused.u8(static_cast<uint8_t>(PresenceTag::sim_hash));
    refused.u16(32);
    for (int index = 0; index < 32; ++index)
        refused.u8(0xcd);
    refused.field(static_cast<uint8_t>(PresenceTag::computers), computers.bytes);
    Value bad = brains;
    bad.bytes.back() = 0x02;
    refused.field(static_cast<uint8_t>(PresenceTag::computer_brains), bad.bytes);
    refused.finish();
    CHECK(decode_presence(refused.bytes.data(), refused.bytes.size(), &decoded) == WireError::ok);
    CHECK(decoded.sim_hash && (*decoded.sim_hash)[0] == 0xcd);
    CHECK(
        find_presence_field(
            refused.bytes.data(), refused.bytes.size(), PresenceTag::computers, &value
        ) &&
        value.size() == 5
    );
    CHECK(!decode_presence_brains(bad.bytes));
}

} // namespace

int main() {
    a_small_record_is_encoded_exactly();
    every_field_round_trips();
    lists_are_cut_to_fit();
    unknown_fields_are_skipped();
    a_bad_field_is_left_out();
    malformed_records_are_refused();
    append_and_find();
    frames_split_a_lone_presence_record();
    only_oa_blocks_are_peers();
    the_content_field();
    the_bundles_and_the_language();
    mutators_round_trip();
    a_small_mutators_field_is_encoded_exactly();
    a_full_loadout_is_cut_before_hack_ids();
    mutator_parts_carry_the_rest();
    the_loadout_assembles_from_the_hosts_records();
    a_mutator_field_that_breaks_its_rule_is_left_out();
    presence_mutators_state_reads_the_fixed_part();
    a_record_without_mutators_matches_the_pinned_bytes();
    every_clash_mask_of_a_short_list_round_trips();
    a_record_without_modes_keeps_todays_loadout();
    a_clash_mask_past_the_list_is_refused();
    find_and_sim_hash_see_mutator_fields();
    computer_brains_round_trip();
    computer_brains_refuse_a_broken_value();
    a_record_with_computer_brains_keeps_the_other_fields();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("presence: all tests passed");
    return 0;
}
