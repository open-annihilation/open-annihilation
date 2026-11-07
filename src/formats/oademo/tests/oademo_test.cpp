// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The replay container: two writes of one replay are the same bytes, the
// first 45 bytes and every local header are pinned, each refusal names its
// sentence, a deflated recording reads like a zstd one, a member this
// version does not know is kept, and a writer refuses a recording that
// holds an address sector.

#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/timeline_index.hpp"

#include "oa/formats/sqsh.hpp"
#include "oa/formats/tad.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/writer.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using oa::formats::oademo::Meta;
using oa::formats::oademo::ReadResult;
using oa::formats::oademo::Replay;
using oa::formats::oademo::WriteOptions;
using oa::formats::oademo::WriteResult;

struct MemoryOut {
    std::vector<uint8_t> bytes{};
};

bool write_memory(void* context, uint64_t offset, std::span<const uint8_t> bytes) {
    auto& out = *static_cast<MemoryOut*>(context);
    if (offset > out.bytes.size())
        return false;
    const std::size_t at = static_cast<std::size_t>(offset);
    if (out.bytes.size() < at + bytes.size())
        out.bytes.resize(at + bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index)
        out.bytes[at + index] = bytes[index];
    return true;
}

std::span<const uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// A small TA Demo recording. With `with_address` it holds a sector of
// network addresses, which a replay never holds.
std::vector<uint8_t> tad_recording(bool with_address) {
    const std::string date_text = "2026-11-04 14:32";
    const std::vector<uint8_t> recorder{'o', 'a', '-', 't', 'e', 's', 't'};
    const std::vector<uint8_t> date(date_text.begin(), date_text.end());
    const std::vector<uint8_t> address{'A', 'D', 'D', 'R'};
    const std::vector<uint8_t> status{0x04};
    const std::vector<uint8_t> payload{oa::formats::tad::payload_marker, 0xff};
    oa::formats::tad::Demo demo;
    demo.version = oa::formats::tad::supported_version;
    demo.max_units = 250;
    demo.map_name = "Show Down";
    demo.sectors = {
        {static_cast<uint32_t>(oa::formats::tad::SectorType::recorder_version), recorder},
        {static_cast<uint32_t>(oa::formats::tad::SectorType::recording_date), date},
    };
    if (with_address)
        demo.sectors.push_back(
            {static_cast<uint32_t>(oa::formats::tad::SectorType::player_address), address}
        );
    demo.players = {{1, 0, 1, "Pilot"}};
    demo.statuses = {{1, status}};
    demo.packets = {{0, 40, 40, 1, payload}, {0, 40, 80, 1, payload}};
    const oa::formats::tad::WriteResult written = oa::formats::tad::write(demo);
    OA_CHECK(written.ok());
    return written.bytes;
}

// A recording of about a megabyte with no repetition, so it stays large once
// deflated.
std::vector<uint8_t> bulky_tad_recording() {
    std::vector<std::vector<uint8_t>> payloads(20, std::vector<uint8_t>(50000));
    uint32_t state = 0x12345678U;
    for (std::vector<uint8_t>& payload : payloads) {
        payload[0] = oa::formats::tad::payload_marker;
        for (std::size_t at = 1; at < payload.size(); ++at) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            payload[at] = static_cast<uint8_t>(state);
        }
    }
    const std::vector<uint8_t> status{0x04};
    oa::formats::tad::Demo demo;
    demo.version = oa::formats::tad::supported_version;
    demo.max_units = 250;
    demo.map_name = "Show Down";
    demo.players = {{1, 0, 1, "Pilot"}};
    demo.statuses = {{1, status}};
    for (const std::vector<uint8_t>& payload : payloads)
        demo.packets.push_back({0, 40, 40, 1, payload});
    const oa::formats::tad::WriteResult written = oa::formats::tad::write(demo);
    OA_CHECK(written.ok());
    return written.bytes;
}

void fill(Replay& replay, std::vector<uint8_t> recording) {
    replay = Replay{};
    replay.recording = std::move(recording);
    oa::formats::oademo::set_recording_identity(replay);
    replay.meta.recording.tad_version = 5;
    replay.meta.recording.packets = 2;
    replay.meta.recording.addresses_removed = true;
    replay.meta.recorded.how = "campaign";
    replay.meta.recorded.after_crash = true;
    replay.meta.recorded.local = "2026-11-04 14:32";
    replay.meta.recorded.at = "2026-11-04T14:32:00Z";
    replay.meta.game.kind = "ridge";
    replay.meta.game.kind_object = "{\"pace\":1}";
    replay.meta.game.length_ms = 40;
    replay.meta.game.extra.push_back({"later", "true"});
    replay.meta.map.name = "Show Down";
    replay.meta.content.base.id = "totala";
    replay.meta.content.base.version = "3.1c";
    replay.meta.content.limits.weapon_types = 256;
    replay.meta.content.limits.sides = 5;
    replay.meta.units.count = 1;
    replay.meta.units.table_sha256 = std::string(64, 'a');
    replay.meta.units.index_bits = 1;
    oa::formats::oademo::PlayerInfo player;
    player.number = 1;
    player.name = "Pilot";
    player.side = "arm";
    player.colour = 0;
    player.kind = "human";
    replay.meta.players.push_back(player);
    replay.meta.extra.push_back({"note", "\"kept\""});
    replay.manifest.engine.recorded_with = "0.8.14";
    replay.manifest.engine.needs = ">=0.8.14";
    oa::formats::oademo::Need need;
    need.kind = "base";
    need.id = "totala";
    need.version = "3.1c";
    need.extra.push_back({"later", "[1,2]"});
    replay.manifest.needs.push_back(need);
    oa::formats::oademo::Need pack;
    pack.kind = "oamap";
    pack.id = "fixture-isles";
    pack.version = "1.0";
    pack.sha256 = std::string(64, 'b');
    oa::formats::oademo::MapNeed map;
    map.name = "Twin Rivers";
    map.sha256 = std::string(64, 'c');
    map.extra.push_back({"note", "\"kept\""});
    pack.maps.push_back(map);
    replay.manifest.needs.push_back(pack);
    replay.timeline = "{\"mark\":1}";
}

Replay sample() {
    Replay replay;
    fill(replay, tad_recording(false));
    return replay;
}

bool put(
    oa::formats::zip::StreamWriter& writer,
    std::string_view name,
    std::span<const uint8_t> bytes,
    oa::formats::zip::Method method
) {
    oa::formats::zip::ZipError problem;
    if (!writer.begin_entry(name, bytes.size(), method, problem))
        return false;
    if (!writer.write(bytes, problem))
        return false;
    return writer.end_entry(problem);
}

std::vector<uint8_t> archive_of(
    std::string signature,
    std::string meta,
    std::string manifest,
    std::string recording_name,
    oa::formats::zip::Method method,
    std::vector<uint8_t> recording
) {
    MemoryOut out;
    oa::formats::zip::WriterOptions options;
    options.deflate_level = oa::formats::oademo::deflate_level;
    oa::formats::zip::StreamWriter writer({&out, write_memory}, options);
    oa::formats::zip::ZipError problem;
    const bool wrote =
        put(writer, "oademo", as_bytes(signature), oa::formats::zip::Method::stored) &&
        put(writer, "meta.json", as_bytes(meta), oa::formats::zip::Method::deflated) &&
        put(writer, "manifest.json", as_bytes(manifest), oa::formats::zip::Method::deflated) &&
        put(writer, recording_name, recording, method) && writer.finish(problem);
    OA_CHECK(wrote);
    return out.bytes;
}

void writes_are_identical_and_ordered() {
    const Replay replay = sample();
    const WriteResult first = oa::formats::oademo::write(replay, {});
    const WriteResult second = oa::formats::oademo::write(replay, {});
    OA_CHECK(first.ok());
    OA_CHECK(first.bytes == second.bytes);
    oa::formats::zip::CentralDirectory directory;
    oa::formats::zip::ZipError problem;
    OA_CHECK(oa::formats::zip::read_directory(first.bytes, directory, problem));
    OA_CHECK(directory.entries.size() == 5);
    if (directory.entries.size() != 5)
        return;
    OA_CHECK(directory.entries[0].name == "oademo");
    OA_CHECK(directory.entries[0].method == oa::formats::zip::Method::stored);
    OA_CHECK(directory.entries[1].name == "meta.json");
    OA_CHECK(directory.entries[1].method == oa::formats::zip::Method::deflated);
    OA_CHECK(directory.entries[2].name == "manifest.json");
    OA_CHECK(directory.entries[2].method == oa::formats::zip::Method::deflated);
    OA_CHECK(directory.entries[3].name == "timeline.json");
    OA_CHECK(directory.entries[3].method == oa::formats::zip::Method::deflated);
    OA_CHECK(directory.entries[4].name == "recording.tad.zst");
    OA_CHECK(directory.entries[4].method == oa::formats::zip::Method::stored);

    const ReadResult read_back = oa::formats::oademo::read(first.bytes);
    OA_CHECK(read_back.ok());
    if (!read_back.ok())
        return;
    OA_CHECK(read_back.replay->recording == replay.recording);
    OA_CHECK(read_back.replay->meta.game.kind == "ridge");
    OA_CHECK(read_back.replay->meta.game.kind_object == "{\"pace\":1}");
    OA_CHECK(read_back.replay->meta.game.extra.size() == 1);
    OA_CHECK(read_back.replay->meta.extra.size() == 1);
    OA_CHECK(read_back.replay->meta.extra[0].json == "\"kept\"");
    OA_CHECK(read_back.replay->meta.recorded.how == "campaign");
    OA_CHECK(read_back.replay->manifest.needs.size() == 2);
    OA_CHECK(read_back.replay->manifest.needs[0].extra.size() == 1);
    OA_CHECK(read_back.replay->timeline == "{\"mark\":1}");
    OA_CHECK(read_back.replay->meta.recorded.after_crash);
    const std::string text = oa::formats::oademo::meta_text(read_back.replay->meta);
    OA_CHECK(text.find("\"after_crash\":true") != std::string::npos);
    const WriteResult third = oa::formats::oademo::write(*read_back.replay, {});
    OA_CHECK(third.ok());
    OA_CHECK(third.bytes == first.bytes);
}

// The first 45 bytes of every .oademo: the local header of the stored
// `oademo` entry (version 20, flags 0, stored, the fixed time and date, the
// CRC-32 and both sizes of `oademo 1` and its newline, no extra field), the
// name and the line. A reader that finds the signature by its position relies on them.
void the_first_forty_five_bytes_are_pinned() {
    constexpr uint8_t pinned[45] = {
        0x50, 0x4B, 0x03, 0x04, // local header signature
        0x14, 0x00,             // version needed: 20
        0x00, 0x00,             // flags: none
        0x00, 0x00,             // method: stored
        0x00, 0x00,             // time
        0x21, 0x00,             // date: 1980-01-01
        0x04, 0xF0, 0x70, 0x42, // CRC-32 of "oademo 1\n"
        0x09, 0x00, 0x00, 0x00, // compressed size
        0x09, 0x00, 0x00, 0x00, // uncompressed size
        0x06, 0x00,             // name length
        0x00, 0x00,             // extra field length
        'o',  'a',  'd',  'e',  'm', 'o', 'o', 'a', 'd', 'e', 'm', 'o', ' ', '1', '\n',
    };
    const Replay replay = sample();
    for (const bool stored_deflated : {false, true}) {
        WriteOptions options;
        options.deflate = stored_deflated;
        const WriteResult written = oa::formats::oademo::write(replay, options);
        OA_CHECK(written.ok());
        OA_CHECK(written.bytes.size() > sizeof pinned);
        if (written.bytes.size() <= sizeof pinned)
            return;
        for (std::size_t index = 0; index < sizeof pinned; ++index)
            OA_CHECK(written.bytes[index] == pinned[index]);
        OA_CHECK(oa::formats::oademo::recognised(written.bytes));
        // Every entry's local header has flags 0 and no extra field.
        oa::formats::zip::CentralDirectory directory;
        oa::formats::zip::ZipError problem;
        OA_CHECK(oa::formats::zip::read_directory(written.bytes, directory, problem));
        OA_CHECK(directory.entries.size() == 5);
        for (const oa::formats::zip::Entry& entry : directory.entries) {
            const std::size_t at = entry.local_header_offset;
            OA_CHECK(at + 30 + entry.name.size() <= written.bytes.size());
            if (at + 30 + entry.name.size() > written.bytes.size())
                return;
            OA_CHECK(written.bytes[at] == 'P' && written.bytes[at + 1] == 'K');
            const uint16_t flags = static_cast<uint16_t>(
                written.bytes[at + 6] | static_cast<uint16_t>(written.bytes[at + 7] << 8U)
            );
            const uint16_t extra_length = static_cast<uint16_t>(
                written.bytes[at + 28] | static_cast<uint16_t>(written.bytes[at + 29] << 8U)
            );
            OA_CHECK(flags == 0);
            OA_CHECK(extra_length == 0);
        }
    }
}

void deflate_reads_like_zstd() {
    const Replay replay = sample();
    WriteOptions flat_options;
    flat_options.deflate = true;
    const WriteResult zstd = oa::formats::oademo::write(replay, {});
    const WriteResult flat = oa::formats::oademo::write(replay, flat_options);
    OA_CHECK(zstd.ok() && flat.ok());
    const ReadResult from_zstd = oa::formats::oademo::read(zstd.bytes);
    const ReadResult from_deflate = oa::formats::oademo::read(flat.bytes);
    OA_CHECK(from_zstd.ok() && from_deflate.ok());
    if (!from_zstd.ok() || !from_deflate.ok())
        return;
    OA_CHECK(from_zstd.replay->recording == from_deflate.replay->recording);
    OA_CHECK(from_deflate.replay->recording == replay.recording);
    oa::formats::zip::CentralDirectory directory;
    oa::formats::zip::ZipError problem;
    OA_CHECK(oa::formats::zip::read_directory(flat.bytes, directory, problem));
    OA_CHECK(directory.entries.size() == 5);
    if (directory.entries.size() == 5) {
        OA_CHECK(directory.entries[4].name == "recording.tad");
        OA_CHECK(directory.entries[4].method == oa::formats::zip::Method::deflated);
    }
}

void false_address_flag_is_kept() {
    Replay replay = sample();
    replay.meta.recording.addresses_removed = false;
    const WriteResult written = oa::formats::oademo::write(replay, {});
    OA_CHECK(written.ok());
    const ReadResult read_back = oa::formats::oademo::read(written.bytes);
    OA_CHECK(read_back.ok());
    if (!read_back.ok())
        return;
    OA_CHECK(read_back.replay->meta.recording.addresses_removed == false);
    const WriteResult again = oa::formats::oademo::write(*read_back.replay, {});
    OA_CHECK(again.bytes == written.bytes);
}

std::string meta_of(const Replay& replay) {
    return oa::formats::oademo::meta_text(replay.meta);
}

std::string manifest_of(const Replay& replay) {
    return oa::formats::oademo::manifest_text(replay.manifest);
}

void maps_of_a_package_are_read_and_written() {
    const Replay replay = sample();
    const WriteResult written = oa::formats::oademo::write(replay, {});
    OA_CHECK(written.ok());
    const ReadResult read_back = oa::formats::oademo::read(written.bytes);
    OA_CHECK(read_back.ok());
    if (!read_back.ok())
        return;
    const auto& needs = read_back.replay->manifest.needs;
    OA_CHECK(needs.size() == 2);
    if (needs.size() != 2)
        return;
    OA_CHECK(needs[0].maps.empty());
    OA_CHECK(needs[1].maps.size() == 1);
    if (needs[1].maps.size() != 1)
        return;
    OA_CHECK(needs[1].maps[0].name == "Twin Rivers");
    OA_CHECK(needs[1].maps[0].sha256 == std::string(64, 'c'));
    // The map is modelled, not kept as an unknown member; what the map holds
    // beyond the two members survives.
    OA_CHECK(needs[1].extra.empty());
    OA_CHECK(needs[1].maps[0].extra.size() == 1);
    OA_CHECK(needs[1].maps[0].extra[0].name == "note");
    const std::string text = oa::formats::oademo::manifest_text(read_back.replay->manifest);
    OA_CHECK(
        text.find(
            "\"maps\":[{\"name\":\"Twin Rivers\",\"sha256\":\"" + std::string(64, 'c') +
            "\",\"note\":\"kept\"}]"
        ) != std::string::npos
    );
    OA_CHECK(text.find("\"maps\"") == text.rfind("\"maps\""));

    // A map names itself and its digest, both.
    const std::string without_digest =
        "{\"oademo\":1,\"engine\":{\"recorded_with\":\"0.9.0\",\"needs\":\">=0.9.0\"},"
        "\"needs\":[{\"kind\":\"oamap\",\"id\":\"fixture-isles\",\"maps\":[{\"name\":\"Twin "
        "Rivers\"}]}]}";
    const std::string meta = meta_of(replay);
    const std::vector<uint8_t> archive = archive_of(
        std::string(oa::formats::oademo::signature_line),
        meta,
        without_digest,
        "recording.tad",
        oa::formats::zip::Method::deflated,
        replay.recording
    );
    OA_CHECK(oa::formats::oademo::read(archive).error == oa::formats::oademo::details_unreadable);
}

void refusals_name_their_sentences() {
    using namespace oa::formats::oademo;
    const std::vector<uint8_t> junk{1, 2, 3, 4};
    OA_CHECK(read(junk).error == not_a_replay);

    Replay replay = sample();
    replay.meta.oademo = 2;
    const WriteResult other_version = write(replay, {});
    OA_CHECK(other_version.ok());
    OA_CHECK(read(other_version.bytes).error == not_this_version);

    const std::string meta = meta_of(sample());
    const std::string manifest = manifest_of(sample());
    const std::vector<uint8_t> bad_signature = archive_of(
        "oademo 2\n", meta, manifest, "recording.tad.zst", oa::formats::zip::Method::stored, {1}
    );
    OA_CHECK(read(bad_signature).error == not_this_version);
    // A stored `oademo` line of another version is still an .oademo, so `read`
    // can say this game does not read it; a first entry with another line is not.
    OA_CHECK(recognised(bad_signature));
    const std::vector<uint8_t> other_line = archive_of(
        "mimetype\n", meta, manifest, "recording.tad.zst", oa::formats::zip::Method::stored, {1}
    );
    OA_CHECK(!recognised(other_line));
    OA_CHECK(read(other_line).error == not_this_version);
    OA_CHECK(!recognised(junk));

    // A zip whose first entry is not the stored `oademo` is some other zip.
    MemoryOut other_zip;
    {
        oa::formats::zip::WriterOptions options;
        options.deflate_level = deflate_level;
        oa::formats::zip::StreamWriter writer({&other_zip, write_memory}, options);
        oa::formats::zip::ZipError problem;
        OA_CHECK(
            put(writer, "meta.json", as_bytes(meta), oa::formats::zip::Method::deflated) &&
            put(writer, "oademo", as_bytes(signature_line), oa::formats::zip::Method::stored) &&
            put(writer, "manifest.json", as_bytes(manifest), oa::formats::zip::Method::deflated) &&
            put(writer, "recording.tad.zst", as_bytes("x"), oa::formats::zip::Method::stored) &&
            writer.finish(problem)
        );
    }
    OA_CHECK(read(other_zip.bytes).error == not_a_replay);
    MemoryOut deflated_signature;
    {
        oa::formats::zip::WriterOptions options;
        options.deflate_level = deflate_level;
        oa::formats::zip::StreamWriter writer({&deflated_signature, write_memory}, options);
        oa::formats::zip::ZipError problem;
        OA_CHECK(
            put(writer, "oademo", as_bytes(signature_line), oa::formats::zip::Method::deflated) &&
            put(writer, "meta.json", as_bytes(meta), oa::formats::zip::Method::deflated) &&
            put(writer, "manifest.json", as_bytes(manifest), oa::formats::zip::Method::deflated) &&
            put(writer, "recording.tad.zst", as_bytes("x"), oa::formats::zip::Method::stored) &&
            writer.finish(problem)
        );
    }
    OA_CHECK(read(deflated_signature.bytes).error == not_a_replay);

    MemoryOut missing;
    {
        oa::formats::zip::WriterOptions options;
        options.deflate_level = deflate_level;
        oa::formats::zip::StreamWriter writer({&missing, write_memory}, options);
        oa::formats::zip::ZipError problem;
        OA_CHECK(
            put(writer, "oademo", as_bytes(signature_line), oa::formats::zip::Method::stored) &&
            put(writer, "meta.json", as_bytes(meta), oa::formats::zip::Method::deflated) &&
            put(writer, "manifest.json", as_bytes(manifest), oa::formats::zip::Method::deflated) &&
            writer.finish(problem)
        );
    }
    OA_CHECK(read(missing.bytes).error == not_one_recording);

    MemoryOut both;
    {
        oa::formats::zip::WriterOptions options;
        options.deflate_level = deflate_level;
        oa::formats::zip::StreamWriter writer({&both, write_memory}, options);
        oa::formats::zip::ZipError problem;
        OA_CHECK(
            put(writer, "oademo", as_bytes(signature_line), oa::formats::zip::Method::stored) &&
            put(writer, "meta.json", as_bytes(meta), oa::formats::zip::Method::deflated) &&
            put(writer, "manifest.json", as_bytes(manifest), oa::formats::zip::Method::deflated) &&
            put(writer, "recording.tad.zst", as_bytes("x"), oa::formats::zip::Method::stored) &&
            put(writer, "recording.tad", as_bytes("y"), oa::formats::zip::Method::deflated) &&
            writer.finish(problem)
        );
    }
    OA_CHECK(read(both.bytes).error == not_one_recording);

    Replay mismatch = sample();
    mismatch.meta.recording.sha256 = std::string(64, '0');
    const WriteResult wrong_hash = write(mismatch, {});
    OA_CHECK(wrong_hash.ok());
    OA_CHECK(read(wrong_hash.bytes).error == recording_mismatch);
    mismatch = sample();
    mismatch.meta.recording.bytes = mismatch.recording.size() + 1;
    const WriteResult wrong_size = write(mismatch, {});
    OA_CHECK(read(wrong_size.bytes).error == recording_mismatch);

    const std::vector<uint8_t> damaged = archive_of(
        std::string(signature_line),
        meta,
        manifest,
        "recording.tad.zst",
        oa::formats::zip::Method::stored,
        {1, 2, 3, 4, 5, 6, 7, 8}
    );
    OA_CHECK(read(damaged).error == recording_unreadable);

    // A real frame with its end cut off is damaged too.
    const WriteResult whole = write(sample(), {});
    OA_CHECK(whole.ok());
    oa::formats::zip::CentralDirectory directory;
    oa::formats::zip::ZipError zip_problem;
    OA_CHECK(oa::formats::zip::read_directory(whole.bytes, directory, zip_problem));
    std::vector<uint8_t> frame;
    OA_CHECK(
        oa::formats::zip::read_entry(whole.bytes, directory.entries.back(), frame, zip_problem)
    );
    OA_CHECK(frame.size() > 8);
    frame.resize(frame.size() - 4);
    const std::vector<uint8_t> cut = archive_of(
        std::string(signature_line),
        meta,
        manifest,
        "recording.tad.zst",
        oa::formats::zip::Method::stored,
        frame
    );
    OA_CHECK(read(cut).error == recording_unreadable);

    const std::vector<uint8_t> oversize_frame{
        0x28, 0xB5, 0x2F, 0xFD, 0xE0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00
    };
    const std::vector<uint8_t> too_long = archive_of(
        std::string(signature_line),
        meta,
        manifest,
        "recording.tad.zst",
        oa::formats::zip::Method::stored,
        oversize_frame
    );
    OA_CHECK(read(too_long).error == recording_too_long);

    const std::vector<uint8_t> broken_meta = archive_of(
        std::string(signature_line),
        "{",
        manifest,
        "recording.tad.zst",
        oa::formats::zip::Method::stored,
        {9}
    );
    OA_CHECK(read(broken_meta).error == details_unreadable);
}

// The description and the manifest read back as written without the
// recording, a damaged recording does not stop them, and bytes refused before
// the recording get the sentence `read` gives.
void a_description_reads_without_its_recording() {
    using namespace oa::formats::oademo;
    const Replay replay = sample();
    const WriteResult written = write(replay, {});
    OA_CHECK(written.ok());
    const DescriptionResult described = read_description(written.bytes);
    OA_CHECK(described.ok() && described.manifest.has_value());
    if (!described.ok() || !described.manifest.has_value())
        return;
    OA_CHECK(meta_text(*described.meta) == meta_text(replay.meta));
    OA_CHECK(manifest_text(*described.manifest) == manifest_text(replay.manifest));

    const std::vector<uint8_t> damaged = archive_of(
        std::string(signature_line),
        meta_of(replay),
        manifest_of(replay),
        "recording.tad.zst",
        oa::formats::zip::Method::stored,
        {1, 2, 3, 4, 5, 6, 7, 8}
    );
    OA_CHECK(read(damaged).error == recording_unreadable);
    OA_CHECK(read_description(damaged).ok());

    std::vector<uint8_t> cut = written.bytes;
    cut.resize(cut.size() / 2);
    const DescriptionResult from_cut = read_description(cut);
    OA_CHECK(!from_cut.ok() && !from_cut.meta.has_value() && !from_cut.manifest.has_value());
    OA_CHECK(!from_cut.error.empty() && from_cut.error == read(cut).error);
    const std::vector<uint8_t> junk{1, 2, 3, 4};
    OA_CHECK(read_description(junk).error == not_a_replay);
    OA_CHECK(read_description(junk).error == read(junk).error);
    Replay other = sample();
    other.meta.oademo = 2;
    const WriteResult other_version = write(other, {});
    OA_CHECK(read_description(other_version.bytes).error == not_this_version);
    OA_CHECK(read_description(other_version.bytes).error == read(other_version.bytes).error);
}

void a_frame_with_no_size_reads_until_it_ends() {
    // A frame whose header names no size, holding five bytes. A reader that
    // cannot size its buffer from the header grows one, up to the limit.
    using namespace oa::formats::oademo;
    const std::vector<uint8_t> frame{
        0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x00, 0x29, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'
    };
    Replay replay = sample();
    replay.recording = {'h', 'e', 'l', 'l', 'o'};
    set_recording_identity(replay);
    const std::vector<uint8_t> archive = archive_of(
        std::string(signature_line),
        meta_of(replay),
        manifest_of(replay),
        "recording.tad.zst",
        oa::formats::zip::Method::stored,
        frame
    );
    const ReadResult read_back = read(archive);
    OA_CHECK(read_back.ok());
    if (read_back.ok())
        OA_CHECK(read_back.replay->recording == replay.recording);
}

void a_recording_with_addresses_is_not_written() {
    using namespace oa::formats::oademo;
    Replay replay;
    fill(replay, tad_recording(true));
    const WriteResult with_address = write(replay, {});
    OA_CHECK(!with_address.ok());
    OA_CHECK(with_address.error == recording_holds_addresses);
    OA_CHECK(with_address.bytes.empty());
    WriteOptions flat_options;
    flat_options.deflate = true;
    OA_CHECK(write(replay, flat_options).error == recording_holds_addresses);
    // The same recording without the sector is written.
    fill(replay, tad_recording(false));
    OA_CHECK(write(replay, {}).ok());
    // Bytes that are no recording cannot be vouched for.
    fill(replay, {1, 2, 3, 4, 9, 8, 7});
    OA_CHECK(write(replay, {}).error == recording_not_a_recording);
}

void store_u32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8U);
    bytes[at + 2] = static_cast<uint8_t>(value >> 16U);
    bytes[at + 3] = static_cast<uint8_t>(value >> 24U);
}

bool patch_uncompressed(std::vector<uint8_t>& archive, std::string_view name, uint32_t size) {
    bool local = false;
    bool central = false;
    for (std::size_t index = 0; index + 46 < archive.size(); ++index) {
        if (archive[index] == 'P' && archive[index + 1] == 'K' && archive[index + 2] == 3 &&
            archive[index + 3] == 4) {
            const uint16_t name_length = static_cast<uint16_t>(archive[index + 26]) |
                                         static_cast<uint16_t>(archive[index + 27] << 8U);
            if (index + 30 + name_length <= archive.size()) {
                const std::string_view got(
                    reinterpret_cast<const char*>(archive.data() + index + 30), name_length
                );
                if (got == name) {
                    store_u32(archive, index + 22, size);
                    local = true;
                }
            }
        }
        if (archive[index] == 'P' && archive[index + 1] == 'K' && archive[index + 2] == 1 &&
            archive[index + 3] == 2) {
            const uint16_t name_length = static_cast<uint16_t>(archive[index + 28]) |
                                         static_cast<uint16_t>(archive[index + 29] << 8U);
            if (index + 46 + name_length <= archive.size()) {
                const std::string_view got(
                    reinterpret_cast<const char*>(archive.data() + index + 46), name_length
                );
                if (got == name) {
                    store_u32(archive, index + 24, size);
                    central = true;
                }
            }
        }
    }
    return local && central;
}

void deflated_recording_past_the_limit_is_refused() {
    // A megabyte of recording with no repetition, so the deflated entry stays
    // large enough for the zip reader to accept a declared size past 512 MiB.
    // The reader then refuses that size before it allocates the buffer.
    Replay replay;
    fill(replay, bulky_tad_recording());
    WriteOptions flat_options;
    flat_options.deflate = true;
    const WriteResult written = oa::formats::oademo::write(replay, flat_options);
    OA_CHECK(written.ok());
    if (!written.ok())
        return;
    std::vector<uint8_t> patched = written.bytes;
    constexpr uint32_t past = 512U * 1024U * 1024U + 1U;
    OA_CHECK(patch_uncompressed(patched, "recording.tad", past));
    const ReadResult refused = oa::formats::oademo::read(patched);
    OA_CHECK(refused.error == oa::formats::oademo::recording_too_long);
}

void unit_table_digest_follows_the_verdicts() {
    auto record = [](uint8_t subtype, uint32_t key, uint32_t value) {
        std::vector<uint8_t> bytes(14, 0);
        bytes[0] = 0x1a;
        bytes[1] = subtype;
        for (int shift = 0; shift < 32; shift += 8) {
            bytes[6 + shift / 8] = static_cast<uint8_t>(key >> shift);
            bytes[10 + shift / 8] = static_cast<uint8_t>(value >> shift);
        }
        return bytes;
    };
    std::vector<uint8_t> checks;
    const auto append = [&](const std::vector<uint8_t>& bytes) {
        checks.insert(checks.end(), bytes.begin(), bytes.end());
    };
    append(record(2, 0x11, 0xaa));
    append(record(2, 0x22, 0xbb));
    append(record(3, 0x11, 1U | (1U << 8U)));
    append(record(3, 0x22, 0));
    const oa::formats::oademo::UnitTableDigest digest =
        oa::formats::oademo::unit_table_digest(checks);
    OA_CHECK(digest.count == 1);
    OA_CHECK(digest.index_bits == 2);
    OA_CHECK(digest.sha256.size() == 64);
    const oa::formats::oademo::UnitTableDigest empty = oa::formats::oademo::unit_table_digest({});
    OA_CHECK(empty.count == 0);
    OA_CHECK(empty.index_bits == 1);
    OA_CHECK(empty.sha256.size() == 64);
}

// An index with every kind of marker, two players with their series, a
// pause, a long chat line of non-ASCII text, a member this version does not
// know, and the damage of three stretches.
oa::formats::oademo::TimelineIndex sample_index() {
    using namespace oa::formats::oademo;
    TimelineIndex index;
    index.length_ticks = 42965;
    index.length_ms = 1432960;
    index.ended = IndexEnd::finished;
    index.ended_tick = 42965;
    index.verdict.finished = true;
    index.verdict.clean = false;
    index.verdict.paced = true;
    index.verdict.errors = 1;
    index.verdict.first_error_tick = 83000;
    index.verdict.last_error = "one packed packet does not split";
    index.result.winners = {2};
    index.result.by = ResultBy::commanders;
    IndexPlayer first;
    first.number = 1;
    first.name = "NJ_THUG";
    first.final = IndexTotals{41, 17, 1, 0, -5, 120};
    IndexPlayer second;
    second.number = 2;
    second.name = "Screamer";
    second.final = IndexTotals{17, 41, 0, 63, 900, 2200};
    IndexPlayer viewer;
    viewer.number = 3;
    viewer.name = "watcher";
    viewer.watcher = true;
    index.players = {first, second, viewer};
    const auto marker =
        [](uint32_t tick, MarkerKind kind, int player, int other, bool flag, std::string text) {
            IndexMarker made;
            made.tick = tick;
            made.kind = kind;
            made.player = player;
            made.other = other;
            made.flag = flag;
            made.text = std::move(text);
            return made;
        };
    index.markers = {
        marker(0, MarkerKind::start, 0, 0, false, ""),
        marker(1, MarkerKind::alliance, 1, 2, true, ""),
        marker(900, MarkerKind::chat, 1, 0, false, "<NJ_THUG> gg \"wp\" \xC3\xA9\xE6\x97\xA5"),
        marker(1200, MarkerKind::pause, 2, 0, true, ""),
        marker(1260, MarkerKind::pause, 2, 0, false, ""),
        marker(30000, MarkerKind::player_dropped, 3, 0, false, ""),
        marker(40000, MarkerKind::player_defeated, 1, 0, false, ""),
        marker(41000, MarkerKind::player_left, 3, 0, false, ""),
        marker(42000, MarkerKind::commander_death, 1, 2, false, ""),
        marker(42965, MarkerKind::end, 0, 0, false, ""),
    };
    index.markers_omitted = 2;
    IndexSeries one;
    one.player = 1;
    one.kills = {0, 3, 41};
    one.losses = {0, 2, 17};
    one.commanders_lost = {0, 0, 1};
    one.units = {1, 40, 0};
    one.metal = {1000, 350, -5};
    one.energy = {1000, 2900, 120};
    IndexSeries two;
    two.player = 2;
    two.kills = {0, 2, 17};
    two.losses = {0, 3, 41};
    two.commanders_lost = {0, 0, 0};
    two.units = {1, 38, 63};
    two.metal = {1000, 700, 900};
    two.energy = {1000, 2500, 2200};
    index.totals = {one, two};
    index.damage = {0, 12000, 5000000000ULL};
    index.extra.push_back(JsonExtra{"later", "{\"a\":[1,2]}"});
    return index;
}

// timeline.json: two writes are the same text, the text reads back to the
// index it was written from, every member is where the design puts it, and
// what is wrong with a text is refused.
void a_timeline_index_is_written_and_read() {
    using namespace oa::formats::oademo;
    const TimelineIndex index = sample_index();
    const std::string text = timeline_index_text(index);
    OA_CHECK(text == timeline_index_text(index));
    OA_CHECK(
        text.rfind("{\"oademo_timeline\":1,\"length\":{\"ticks\":42965,\"ms\":1432960},", 0) == 0
    );
    OA_CHECK(text.find("\"ended\":{\"how\":\"finished\",\"tick\":42965}") != std::string::npos);
    OA_CHECK(text.find("\"result\":{\"winners\":[2],\"by\":\"commanders\"}") != std::string::npos);
    OA_CHECK(
        text.find("{\"tick\":42000,\"kind\":\"commander_death\",\"player\":1,\"by\":2}") !=
        std::string::npos
    );
    OA_CHECK(
        text.find("{\"tick\":1200,\"kind\":\"pause\",\"player\":2,\"paused\":true}") !=
        std::string::npos
    );
    OA_CHECK(text.find("\"markers_omitted\":2") != std::string::npos);
    OA_CHECK(text.find("\"damage\":[0,12000,5000000000]") != std::string::npos);
    OA_CHECK(text.size() > 2 && text.back() == '}' && text.find('\n') == std::string::npos);

    const std::optional<TimelineIndex> read = parse_timeline_index(text);
    OA_CHECK(read.has_value());
    if (!read.has_value())
        return;
    OA_CHECK(read->length_ticks == 42965 && read->length_ms == std::optional<uint64_t>{1432960});
    OA_CHECK(read->ended == IndexEnd::finished && read->ended_tick == 42965);
    OA_CHECK(!read->verdict.clean && read->verdict.errors == 1);
    OA_CHECK(read->verdict.first_error_tick == 83000);
    OA_CHECK(read->verdict.last_error == "one packed packet does not split");
    OA_CHECK(read->result.winners == std::vector<int>{2});
    OA_CHECK(read->result.by == ResultBy::commanders);
    OA_CHECK(read->players.size() == 3 && read->players[2].watcher);
    OA_CHECK(read->players[0].final.metal == -5 && read->players[1].final.units == 63);
    OA_CHECK(read->markers.size() == index.markers.size());
    OA_CHECK(read->markers[2].text == index.markers[2].text);
    OA_CHECK(read->markers[8].kind == MarkerKind::commander_death && read->markers[8].other == 2);
    OA_CHECK(read->markers[1].flag && read->markers[3].flag && !read->markers[4].flag);
    OA_CHECK(read->markers_omitted == 2);
    OA_CHECK(read->totals.size() == 2 && read->totals[0].metal == index.totals[0].metal);
    OA_CHECK(read->damage == index.damage);
    OA_CHECK(read->period_ticks == index_period_ticks && read->first_tick == 0);
    OA_CHECK(read->extra.size() == 1 && read->extra[0].name == "later");
    OA_CHECK(timeline_index_text(*read) == text);

    // A timeline of an index travels in a replay as one value, which the
    // reader keeps whole.
    Replay replay;
    fill(replay, tad_recording(false));
    replay.timeline = text;
    const WriteResult written = oa::formats::oademo::write(replay);
    OA_CHECK(written.ok());
    const ReadResult back = oa::formats::oademo::read(written.bytes);
    OA_CHECK(back.ok() && back.replay->timeline.has_value());
    if (back.ok() && back.replay->timeline.has_value()) {
        const std::optional<TimelineIndex> inside = parse_timeline_index(*back.replay->timeline);
        OA_CHECK(inside.has_value() && inside->length_ticks == 42965);
    }

    // Without the length on the recorder's clock, and without omissions,
    // those members are left out, and a marker of a kind this version does
    // not know is skipped.
    TimelineIndex bare = index;
    bare.length_ms.reset();
    bare.markers_omitted = 0;
    bare.extra.clear();
    const std::string bare_text = timeline_index_text(bare);
    OA_CHECK(bare_text.find("\"ms\"") == std::string::npos);
    OA_CHECK(bare_text.find("markers_omitted") == std::string::npos);
    std::string future = bare_text;
    const std::string first_marker = "{\"tick\":0,\"kind\":\"start\"}";
    const std::size_t at = future.find(first_marker);
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        future.insert(at, "{\"tick\":0,\"kind\":\"a_later_kind\",\"player\":1},");
        const std::optional<TimelineIndex> skipped = parse_timeline_index(future);
        OA_CHECK(skipped.has_value() && skipped->markers.size() == bare.markers.size());
    }

    // What is refused: not JSON, another version, a missing member, a wrong
    // one and a player number above a byte.
    OA_CHECK(!parse_timeline_index("").has_value());
    OA_CHECK(!parse_timeline_index("[]").has_value());
    OA_CHECK(!parse_timeline_index("{").has_value());
    const auto refuses =
        [&](const std::string& from, const std::string& old_text, const std::string& new_text) {
            std::string changed = from;
            const std::size_t where = changed.find(old_text);
            OA_CHECK(where != std::string::npos);
            if (where == std::string::npos)
                return false;
            changed.replace(where, old_text.size(), new_text);
            return !parse_timeline_index(changed).has_value();
        };
    OA_CHECK(refuses(text, "\"oademo_timeline\":1", "\"oademo_timeline\":2"));
    OA_CHECK(refuses(text, "\"oademo_timeline\":1", "\"oademo_timeline\":\"1\""));
    OA_CHECK(refuses(text, "\"ticks\":42965", "\"ticks\":-1"));
    OA_CHECK(refuses(text, "\"how\":\"finished\"", "\"how\":\"vanished\""));
    OA_CHECK(refuses(text, "\"by\":\"commanders\"", "\"by\":\"luck\""));
    // The word of a result the pass never writes is a wrong member too.
    OA_CHECK(refuses(text, "\"by\":\"commanders\"", "\"by\":\"end_of_game\""));
    OA_CHECK(refuses(text, "\"errors\":1", "\"errors\":4294967296"));
    OA_CHECK(refuses(text, "\"watcher\":true", "\"watcher\":1"));
    OA_CHECK(refuses(text, "\"winners\":[2]", "\"winners\":[256]"));
    OA_CHECK(refuses(text, "\"kills\":[0,3,41]", "\"kills\":[0,3,\"41\"]"));
    OA_CHECK(refuses(text, "\"period_ticks\":300,\"players\"", "\"period_ticks\":0,\"players\""));
    OA_CHECK(refuses(text, "\"damage\":[0,12000,5000000000]", "\"damage\":[0,-1]"));
    OA_CHECK(refuses(text, "\"markers\":[", "\"markrs\":["));
    OA_CHECK(refuses(text, "\"text\":\"<NJ_THUG>", "\"text\":5,\"x\":\"<NJ_THUG>"));
}

// The words of an end and of a marker are the ones the design lists, and a
// player's text that is not UTF-8 is carried as the JSON writer carries it.
void the_words_of_an_index_are_pinned() {
    using namespace oa::formats::oademo;
    OA_CHECK(index_end_name(IndexEnd::finished) == "finished");
    OA_CHECK(index_end_name(IndexEnd::left) == "left");
    OA_CHECK(index_end_name(IndexEnd::cut) == "cut");
    OA_CHECK(result_by_name(ResultBy::none) == "none");
    OA_CHECK(result_by_name(ResultBy::commanders) == "commanders");
    OA_CHECK(result_by_name(ResultBy::mission) == "mission");
    OA_CHECK(marker_kind_name(MarkerKind::start) == "start");
    OA_CHECK(marker_kind_name(MarkerKind::end) == "end");
    OA_CHECK(marker_kind_name(MarkerKind::commander_death) == "commander_death");
    OA_CHECK(marker_kind_name(MarkerKind::player_left) == "player_left");
    OA_CHECK(marker_kind_name(MarkerKind::player_dropped) == "player_dropped");
    OA_CHECK(marker_kind_name(MarkerKind::player_defeated) == "player_defeated");
    OA_CHECK(marker_kind_name(MarkerKind::alliance) == "alliance");
    OA_CHECK(marker_kind_name(MarkerKind::pause) == "pause");
    OA_CHECK(marker_kind_name(MarkerKind::chat) == "chat");
    OA_CHECK(index_period_ticks == 300 && most_markers_of_a_kind == 1000);
}

bool notice_of(
    const oa::formats::tad::ExportResult& exported, oa::formats::tad::ExportNoticeKind kind
) {
    for (const oa::formats::tad::ExportNotice& notice : exported.notices)
        if (notice.kind == kind)
            return true;
    return false;
}

const oa::formats::tad::ExportNotice*
mutator_notice(const oa::formats::tad::ExportResult& exported) {
    for (const oa::formats::tad::ExportNotice& notice : exported.notices)
        if (notice.kind == oa::formats::tad::ExportNoticeKind::mutators)
            return &notice;
    return nullptr;
}

/// A recording with an address, a mutator whose options also name an id,
/// a long map name, a long player name, a long status and a wide weapon record.
std::vector<uint8_t> export_fixture(bool compressed) {
    using namespace oa::formats::tad;
    const std::string loadout =
        R"({"mutators":[{"id":"ridge","options":{"id":"nope"}}],"oaloadout":1})";
    const std::vector<uint8_t> mutators(loadout.begin(), loadout.end());
    const std::vector<uint8_t> brains{'b', 'r'};
    const std::array<uint8_t, limits_sector_bytes> limits =
        encode_limits_sector(RecordedLimits{512, 6});
    const std::vector<uint8_t> address{'A', 'D', 'D', 'R'};
    const std::vector<uint8_t> recorder{'o', 'a', '-', 't', 'e', 's', 't'};
    const std::string date_text = "2026-11-04 14:32";
    const std::vector<uint8_t> date(date_text.begin(), date_text.end());
    std::vector<uint8_t> plain{payload_marker, 0xff};
    std::vector<uint8_t> wide(1 + wide_record_lengths[0], 0);
    wide[0] = payload_marker;
    wide[1] = first_wide_recorded_type;
    std::vector<uint8_t> payload = plain;
    if (compressed) {
        const std::vector<uint8_t> records{0xff};
        const auto packed = oa::formats::sqsh::encode_lz77(records, records.size() * 2 + 64);
        OA_CHECK(packed.ok() && packed.value.has_value());
        if (packed.ok() && packed.value.has_value()) {
            payload = {compressed_payload_marker};
            payload.insert(payload.end(), packed.value->begin(), packed.value->end());
        }
    }
    Demo demo;
    demo.version = supported_version;
    demo.max_units = 250;
    demo.map_name = std::string(exported_name_bytes + 1, 'M');
    demo.sectors = {
        {static_cast<uint32_t>(SectorType::recorder_version), recorder},
        {static_cast<uint32_t>(SectorType::recording_date), date},
        {static_cast<uint32_t>(SectorType::player_address), address},
        {static_cast<uint32_t>(SectorType::mutators), mutators},
        {static_cast<uint32_t>(SectorType::brains), brains},
        {static_cast<uint32_t>(SectorType::limits), limits},
    };
    const std::string long_name(exported_name_bytes + 1, 'N');
    demo.players = {{1, 0, 1, "Pilot"}, {2, 1, 2, long_name}};
    const std::vector<uint8_t> status{0x04};
    const std::vector<uint8_t> long_status(exported_status_bytes + 1, 0x11);
    demo.statuses = {{1, status}, {2, long_status}};
    demo.packets = {{0, 40, 40, 1, payload}, {0, 40, 80, 1, wide}};
    // The file's WriteResult is the container's. This recording is a .tad.
    const oa::formats::tad::WriteResult written = oa::formats::tad::write(demo);
    OA_CHECK(written.ok());
    return written.bytes;
}

void an_export_is_plain_and_the_same_twice() {
    using namespace oa::formats::tad;
    const std::vector<uint8_t> source = export_fixture(true);
    ExportOptions fitted;
    const ExportResult first = export_recording(source, fitted);
    const ExportResult second = export_recording(source, fitted);
    OA_CHECK(first.ok() && second.ok());
    OA_CHECK(first.bytes == second.bytes);
    OA_CHECK(!first.bytes.empty());
    const ParseResult parsed = parse(first.bytes);
    OA_CHECK(parsed.ok());
    if (!parsed.ok())
        return;
    const Demo& demo = *parsed.demo;
    OA_CHECK(demo.version == supported_version);
    OA_CHECK(find_sector(demo, SectorType::player_address) == nullptr);
    OA_CHECK(find_sector(demo, SectorType::mutators) != nullptr);
    OA_CHECK(find_sector(demo, SectorType::brains) != nullptr);
    OA_CHECK(find_sector(demo, SectorType::limits) != nullptr);
    OA_CHECK(demo.players.size() == 2 && demo.statuses.size() == 2);
    OA_CHECK(demo.map_name.size() == exported_name_bytes);
    OA_CHECK(demo.players[1].name.size() == exported_name_bytes);
    OA_CHECK(demo.statuses[1].datagram.size() == exported_status_bytes);
    OA_CHECK(demo.packets.size() == 2);
    for (const Packet& packet : demo.packets)
        OA_CHECK(!packet.payload.empty() && packet.payload[0] == payload_marker);
    OA_CHECK(notice_of(first, ExportNoticeKind::names_cut));
    OA_CHECK(notice_of(first, ExportNoticeKind::status_cut));
    OA_CHECK(notice_of(first, ExportNoticeKind::wide_records));
    const ExportNotice* mutators = mutator_notice(first);
    OA_CHECK(
        mutators != nullptr && mutators->mutators.size() == 1 && mutators->mutators[0] == "ridge"
    );

    ExportOptions dropped;
    dropped.keep_oa_sectors = false;
    const ExportResult without = export_recording(source, dropped);
    OA_CHECK(without.ok());
    const ParseResult dropped_parsed = parse(without.bytes);
    OA_CHECK(dropped_parsed.ok());
    if (dropped_parsed.ok()) {
        OA_CHECK(find_sector(*dropped_parsed.demo, SectorType::mutators) == nullptr);
        OA_CHECK(find_sector(*dropped_parsed.demo, SectorType::brains) == nullptr);
        OA_CHECK(find_sector(*dropped_parsed.demo, SectorType::limits) == nullptr);
        OA_CHECK(find_sector(*dropped_parsed.demo, SectorType::player_address) == nullptr);
    }
    OA_CHECK(notice_of(without, ExportNoticeKind::mutators));
    OA_CHECK(notice_of(without, ExportNoticeKind::wide_records));

    // write stores the bytes it is given. The export then refuses a packet
    // that does not expand, and writes nothing.
    const std::vector<uint8_t> broken{compressed_payload_marker, 0x00};
    const std::vector<uint8_t> short_status{0x04};
    Demo bad;
    bad.version = supported_version;
    bad.max_units = 250;
    bad.map_name = "Show Down";
    bad.players = {{1, 0, 1, "Pilot"}};
    bad.statuses = {{1, short_status}};
    bad.packets = {{0, 40, 40, 1, broken}};
    const oa::formats::tad::WriteResult bad_file = oa::formats::tad::write(bad);
    OA_CHECK(bad_file.ok());
    if (bad_file.ok()) {
        const ExportResult refused = export_recording(bad_file.bytes, {});
        OA_CHECK(!refused.ok() && refused.bytes.empty());
    }

    ExportOptions whole;
    whole.fit_tadr_sizes = false;
    const ExportResult kept = export_recording(source, whole);
    OA_CHECK(kept.ok());
    const ParseResult kept_parsed = parse(kept.bytes);
    OA_CHECK(kept_parsed.ok());
    if (kept_parsed.ok()) {
        OA_CHECK(kept_parsed.demo->map_name.size() == exported_name_bytes + 1);
        OA_CHECK(kept_parsed.demo->players[1].name.size() == exported_name_bytes + 1);
        OA_CHECK(kept_parsed.demo->statuses[1].datagram.size() == exported_status_bytes + 1);
        OA_CHECK(!notice_of(kept, ExportNoticeKind::names_cut));
        OA_CHECK(!notice_of(kept, ExportNoticeKind::status_cut));
    }
}

void a_replay_export_keeps_a_clean_recording() {
    using namespace oa::formats::oademo;
    const Replay clean = sample();
    const WriteResult exported = export_replay(clean);
    OA_CHECK(exported.ok());
    WriteOptions at_export;
    at_export.level = level_on_export;
    const WriteResult direct = write(clean, at_export);
    OA_CHECK(direct.ok());
    OA_CHECK(exported.bytes == direct.bytes);
    const WriteResult again = export_replay(clean);
    OA_CHECK(again.ok() && again.bytes == exported.bytes);
    const ReadResult back = read(exported.bytes);
    OA_CHECK(back.ok());
    if (!back.ok())
        return;
    OA_CHECK(back.replay->recording == clean.recording);
    OA_CHECK(back.replay->meta.recording.sha256 == clean.meta.recording.sha256);
    OA_CHECK(back.replay->timeline == clean.timeline);

    RecordingExport replaced;
    replaced.timeline = "{\"mark\":2}";
    const WriteResult swapped = export_replay(clean, replaced);
    OA_CHECK(swapped.ok());
    const ReadResult swapped_back = read(swapped.bytes);
    OA_CHECK(
        swapped_back.ok() &&
        swapped_back.replay->timeline == std::optional<std::string>("{\"mark\":2}")
    );
    OA_CHECK(swapped_back.ok() && swapped_back.replay->recording == clean.recording);

    Replay dirty;
    fill(dirty, tad_recording(true));
    OA_CHECK(!write(dirty).ok());
    const WriteResult cleaned = export_replay(dirty);
    OA_CHECK(cleaned.ok());
    const ReadResult cleaned_back = read(cleaned.bytes);
    OA_CHECK(cleaned_back.ok());
    if (!cleaned_back.ok())
        return;
    const oa::formats::tad::ParseResult stored =
        oa::formats::tad::parse(cleaned_back.replay->recording);
    OA_CHECK(stored.ok());
    if (stored.ok()) {
        OA_CHECK(
            oa::formats::tad::find_sector(
                *stored.demo, oa::formats::tad::SectorType::player_address
            ) == nullptr
        );
        OA_CHECK(cleaned_back.replay->meta.recording.addresses_removed);
    }

    const std::vector<uint8_t> fixture = export_fixture(false);
    RecordingExport wrapped;
    wrapped.recorded_with = "0.8.14";
    wrapped.timeline = "{\"mark\":1}";
    const WriteResult from_tad = export_tad(fixture, wrapped);
    OA_CHECK(from_tad.ok());
    const ReadResult wrapped_back = read(from_tad.bytes);
    OA_CHECK(wrapped_back.ok());
    if (!wrapped_back.ok())
        return;
    const Replay& replay = *wrapped_back.replay;
    OA_CHECK(replay.meta.recorded.how == "imported");
    OA_CHECK(replay.meta.game.kind == "multiplayer");
    OA_CHECK(replay.meta.recording.addresses_removed);
    OA_CHECK(
        replay.meta.content.mutators.size() == 1 && replay.meta.content.mutators[0] == "ridge"
    );
    OA_CHECK(replay.meta.content.brains.empty());
    OA_CHECK(replay.meta.map.name.size() == oa::formats::tad::exported_name_bytes + 1);
    OA_CHECK(replay.meta.players.size() == 2);
    OA_CHECK(replay.meta.players[0].kind == "human" && replay.meta.players[0].side == "arm");
    OA_CHECK(replay.meta.players[1].side == "core");
    OA_CHECK(replay.meta.players[1].name.size() == oa::formats::tad::exported_name_bytes + 1);
    OA_CHECK(replay.manifest.engine.recorded_with == "0.8.14");
    OA_CHECK(replay.manifest.engine.needs == ">=0.8.14");
    OA_CHECK(replay.timeline == std::optional<std::string>("{\"mark\":1}"));
    OA_CHECK(replay.meta.content.limits.weapon_types == 512);
    OA_CHECK(replay.meta.content.limits.sides == 6);
    const oa::formats::tad::ParseResult inner = oa::formats::tad::parse(replay.recording);
    OA_CHECK(inner.ok());
    if (inner.ok())
        OA_CHECK(
            oa::formats::tad::find_sector(
                *inner.demo, oa::formats::tad::SectorType::player_address
            ) == nullptr
        );
}

} // namespace

int main() {
    writes_are_identical_and_ordered();
    the_first_forty_five_bytes_are_pinned();
    maps_of_a_package_are_read_and_written();
    deflate_reads_like_zstd();
    false_address_flag_is_kept();
    refusals_name_their_sentences();
    a_description_reads_without_its_recording();
    a_frame_with_no_size_reads_until_it_ends();
    a_recording_with_addresses_is_not_written();
    deflated_recording_past_the_limit_is_refused();
    unit_table_digest_follows_the_verdicts();
    a_timeline_index_is_written_and_read();
    the_words_of_an_index_are_pinned();
    an_export_is_plain_and_the_same_twice();
    a_replay_export_keeps_a_clean_recording();
    return oa::test::check_exit_status();
}
