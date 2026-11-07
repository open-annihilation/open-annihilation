// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Exporting a replay. A .oademo is written at the export zstd level. A
// recording that still holds a network address, or a packet that is not
// plain, is cleaned first; one that is already clean is stored byte for
// byte, so its identity does not move. A .tad wrapped here is described
// from its header only. The app and `oa-tool replay pack` describe one
// through session::demo, which reads the lobby block.

#include "oa/formats/oademo/oademo.hpp"

#include "oa/formats/tad.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oademo {
namespace {

namespace tad = oa::formats::tad;

/// The characters of the game's code page at 0x80-0x9F. The five it leaves
/// undefined stand for the control character of the same value.
constexpr std::array<char32_t, 32> code_page_marks{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

void append_utf8(std::string& out, char32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
        return;
    }
    if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        return;
    }
    out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
}

/// The recording's text as UTF-8. Names, the map, the recorder line and the
/// date are bytes of the game's code page.
std::string utf8_of(std::string_view bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const unsigned char byte : bytes) {
        char32_t code = byte;
        if (byte >= 0x80 && byte < 0xA0)
            code = code_page_marks[static_cast<std::size_t>(byte - 0x80)];
        append_utf8(text, code);
    }
    return text;
}

std::string side_name(uint8_t side) {
    if (side == static_cast<uint8_t>(tad::RecordedSide::arm))
        return "arm";
    if (side == static_cast<uint8_t>(tad::RecordedSide::core))
        return "core";
    return std::to_string(side);
}

bool recording_is_clean(const tad::Demo& demo) noexcept {
    if (demo.version != tad::supported_version)
        return false;
    if (tad::find_sector(demo, tad::SectorType::player_address) != nullptr)
        return false;
    for (const tad::Packet& packet : demo.packets) {
        if (packet.payload.empty() || packet.payload[0] != tad::payload_marker)
            return false;
    }
    return true;
}

WriteResult fail(std::string message) {
    WriteResult result;
    result.error = std::move(message);
    return result;
}

WriteResult write_export(const Replay& replay, const WriteWatch* watch) {
    WriteOptions options;
    options.level = level_on_export;
    if (watch != nullptr)
        options.watch = *watch;
    return write(replay, options);
}

/// Fills a replay from a cleaned recording. `cleaned` is the exported bytes.
/// `mutators` are the ids the export noticed, each entry's own id.
Replay replay_from_cleaned(
    const std::vector<uint8_t>& cleaned,
    const RecordingExport& options,
    const std::vector<std::string>& mutators
) {
    Replay replay;
    replay.recording = cleaned;
    set_recording_identity(replay);
    const tad::ParseResult parsed = tad::parse(cleaned);
    const tad::Demo& demo = *parsed.demo;
    replay.meta.recording.tad_version = demo.version;
    replay.meta.recording.packets = demo.packets.size();
    replay.meta.recording.addresses_removed = true;
    replay.meta.recorded.how = "imported";
    if (const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::recorder_version))
        replay.meta.recorded.by = utf8_of(tad::sector_text(*sector));
    if (const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::recording_date)) {
        // A recorder's date has no time zone, so it stays text and `recorded.at` is left out.
        replay.meta.recorded.local = utf8_of(tad::sector_text(*sector));
    }
    replay.meta.game.kind = "multiplayer";
    if (!demo.packets.empty())
        replay.meta.game.length_ms = demo.packets.back().time_ms;
    replay.meta.map.name = utf8_of(demo.map_name);
    replay.meta.content.base.id = "totala";
    replay.meta.content.base.version = "3.1c";
    replay.meta.content.limits.weapon_types = tad::recorded_base_weapon_types;
    replay.meta.content.limits.sides = tad::recorded_base_sides;
    if (const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::limits)) {
        if (const std::optional<tad::RecordedLimits> limits = tad::decode_limits_sector(*sector)) {
            replay.meta.content.limits.weapon_types = limits->weapon_types;
            replay.meta.content.limits.sides = limits->sides;
        }
    }
    // The same ids the recording export noticed. An id inside a mutator's
    // options is not one of them, and package digests are not invented.
    replay.meta.content.mutators = mutators;
    const UnitTableDigest digest = unit_table_digest(demo.unit_checks);
    replay.meta.units.count = digest.count;
    replay.meta.units.table_sha256 = digest.sha256;
    replay.meta.units.index_bits = digest.index_bits;
    for (const tad::Player& player : demo.players) {
        PlayerInfo info;
        info.number = player.number;
        info.name = utf8_of(player.name);
        info.side = side_name(player.side);
        info.colour = player.color;
        info.kind = "human";
        replay.meta.players.push_back(std::move(info));
    }
    if (!options.recorded_with.empty()) {
        replay.manifest.engine.recorded_with = options.recorded_with;
        replay.manifest.engine.needs = ">=" + options.recorded_with;
    }
    Need base;
    base.kind = "base";
    base.id = "totala";
    base.version = "3.1c";
    replay.manifest.needs.push_back(std::move(base));
    replay.timeline = options.timeline;
    return replay;
}

} // namespace

WriteResult export_replay(const Replay& source, const RecordingExport& options) {
    const tad::ParseResult parsed = tad::parse(source.recording);
    if (!parsed.ok())
        return fail(parsed.error->message);
    Replay replay = source;
    if (options.timeline.has_value())
        replay.timeline = options.timeline;
    if (!recording_is_clean(*parsed.demo)) {
        tad::ExportOptions cleaning;
        cleaning.keep_oa_sectors = true;
        cleaning.fit_tadr_sizes = false;
        const tad::ExportResult cleaned = tad::export_recording(source.recording, cleaning);
        if (!cleaned.ok())
            return fail(cleaned.error->message);
        replay.recording = cleaned.bytes;
        set_recording_identity(replay);
        const tad::ParseResult again = tad::parse(replay.recording);
        if (!again.ok())
            return fail(again.error->message);
        replay.meta.recording.tad_version = again.demo->version;
        replay.meta.recording.packets = again.demo->packets.size();
        replay.meta.recording.addresses_removed = true;
    }
    return write_export(replay, options.watch);
}

WriteResult export_tad(std::span<const uint8_t> bytes, const RecordingExport& options) {
    tad::ExportOptions cleaning;
    cleaning.keep_oa_sectors = true;
    cleaning.fit_tadr_sizes = false;
    const tad::ExportResult cleaned = tad::export_recording(bytes, cleaning);
    if (!cleaned.ok())
        return fail(cleaned.error->message);
    const tad::ParseResult parsed = tad::parse(cleaned.bytes);
    if (!parsed.ok())
        return fail(parsed.error->message);
    std::vector<std::string> mutators;
    for (const tad::ExportNotice& notice : cleaned.notices) {
        if (notice.kind == tad::ExportNoticeKind::mutators)
            mutators = notice.mutators;
    }
    const Replay replay = replay_from_cleaned(cleaned.bytes, options, mutators);
    return write_export(replay, nullptr);
}

} // namespace oa::formats::oademo
