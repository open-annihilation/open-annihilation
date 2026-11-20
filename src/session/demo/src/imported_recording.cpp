// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A TA Demo recording described as a .oademo. The same description `oa-tool
// replay pack` writes: the lobby block gives a player's side and colour, the
// mutators and brains sectors are read, and the recorder's text is turned
// from the game's code page into UTF-8. The recording is stored plain, with
// no network address.

#include "oa/session/demo/imported_recording.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/player_setup.h"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/tad.hpp"
#include "oa/present/game_text.hpp"
#include "oa/session/demo.hpp"
#include "oa/session/demo/replay_record.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oa::session::demo {
namespace {

namespace tad = formats::tad;
namespace oademo = formats::oademo;

ImportedRecording failed(std::string reason) {
    ImportedRecording described;
    described.error = std::move(reason);
    return described;
}

/// The recording's text as UTF-8. The recorder, the date, the map and the
/// players' names are bytes of the game's code page.
std::string utf8_text(std::string_view bytes) {
    return present::decode_game_text(bytes, false);
}

std::string side_name(uint8_t side) {
    if (side == static_cast<uint8_t>(tad::RecordedSide::arm))
        return "arm";
    if (side == static_cast<uint8_t>(tad::RecordedSide::core))
        return "core";
    return std::to_string(side);
}

/// The recording with its address sector gone and every packet plain. The
/// spans of `demo` view `bytes` and are not used after this returns.
std::vector<uint8_t> plain_recording(std::span<const uint8_t> bytes, std::string& reason) {
    const tad::ParseResult parsed = tad::parse(bytes);
    if (!parsed.ok()) {
        reason = parsed.error->message;
        return {};
    }
    tad::Demo demo = *parsed.demo;
    std::vector<tad::Sector> kept;
    kept.reserve(demo.sectors.size());
    for (const tad::Sector& sector : demo.sectors) {
        if (sector.type == static_cast<uint32_t>(tad::SectorType::player_address))
            continue;
        kept.push_back(sector);
    }
    demo.sectors = std::move(kept);
    std::vector<std::vector<uint8_t>> plain;
    plain.reserve(demo.packets.size());
    for (const tad::Packet& packet : demo.packets) {
        const tad::DecodedPayload decoded = tad::decode_payload(packet.payload);
        if (!decoded.ok()) {
            reason = "a packet could not be read";
            return {};
        }
        plain.push_back(decoded.bytes);
    }
    for (std::size_t at = 0; at < demo.packets.size(); ++at)
        demo.packets[at].payload = plain[at];
    const tad::WriteResult written = tad::write(demo);
    if (!written.ok()) {
        reason = written.error->message;
        return {};
    }
    return written.bytes;
}

/// What the manifest asks for to install one mutator: its package, by id,
/// version and digest.
oademo::Need mutator_need(const data::mutators::LoadoutEntry& entry) {
    oademo::Need need;
    need.kind = "oamut";
    need.id = entry.id;
    need.version = std::optional<std::string>(entry.version);
    const std::array<char, base::sha256::hex_size> hex = base::sha256::to_hex(entry.package);
    need.sha256 = std::optional<std::string>(std::in_place, hex.data(), hex.size());
    return need;
}

/// A mutator's own id in a loadout, not an id written inside its options.
///
/// An entry's object sits three braces deep. An `id` inside `options` is
/// one deeper. This is how a `.tad` export names the mutators it warns
/// about, including a loadout the package reader refuses.
std::vector<std::string> named_mutators(std::string_view text) {
    std::vector<std::string> ids;
    int depth = 0;
    std::size_t scan = 0;
    const auto take_string = [&](std::size_t at, std::string& value, std::size_t& end) {
        if (at >= text.size() || text[at] != '"')
            return false;
        value.clear();
        std::size_t cursor = at + 1;
        while (cursor < text.size()) {
            const char character = text[cursor];
            if (character == '\\') {
                if (cursor + 1 >= text.size())
                    return false;
                value.push_back(text[cursor + 1]);
                cursor += 2;
                continue;
            }
            if (character == '"') {
                end = cursor + 1;
                return true;
            }
            value.push_back(character);
            ++cursor;
        }
        return false;
    };
    while (scan < text.size()) {
        const char character = text[scan];
        if (character == '"') {
            std::string value;
            std::size_t after = scan;
            if (!take_string(scan, value, after))
                break;
            scan = after;
            while (scan < text.size() && (text[scan] == ' ' || text[scan] == '\t' ||
                                          text[scan] == '\n' || text[scan] == '\r'))
                ++scan;
            if (depth == 3 && value == "id" && scan < text.size() && text[scan] == ':') {
                ++scan;
                while (scan < text.size() && (text[scan] == ' ' || text[scan] == '\t' ||
                                              text[scan] == '\n' || text[scan] == '\r'))
                    ++scan;
                std::string id;
                if (!take_string(scan, id, after))
                    break;
                scan = after;
                if (!id.empty())
                    ids.push_back(std::move(id));
            }
            continue;
        }
        if (character == '{' || character == '[')
            ++depth;
        else if ((character == '}' || character == ']') && depth > 0)
            --depth;
        ++scan;
    }
    return ids;
}

void add_mutators(const tad::Demo& demo, oademo::Replay& replay) {
    const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::mutators);
    if (sector == nullptr)
        return;
    const std::string_view text = tad::sector_text(*sector);
    const std::optional<data::mutators::Loadout> loadout =
        data::mutators::parse_loadout(text, nullptr);
    if (loadout.has_value()) {
        for (const data::mutators::LoadoutEntry& entry : loadout->entries) {
            replay.meta.content.mutators.push_back(entry.id);
            replay.manifest.needs.push_back(mutator_need(entry));
        }
        return;
    }
    // No digest is invented for a loadout this build cannot read. The ids
    // are still the ones a `.tad` export warns about.
    replay.meta.content.mutators = named_mutators(text);
}

void add_brains(const tad::Demo& demo, oademo::Replay& replay) {
    const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::brains);
    if (sector == nullptr)
        return;
    const std::optional<data::brain_library::Seats> seats =
        data::brain_library::parse_seats(tad::sector_text(*sector));
    if (!seats.has_value())
        return;
    std::vector<const data::brain_library::Seat*> by_place;
    by_place.reserve(seats->seats.size());
    for (const data::brain_library::Seat& seat : seats->seats)
        by_place.push_back(&seat);
    std::stable_sort(
        by_place.begin(),
        by_place.end(),
        [](const data::brain_library::Seat* left, const data::brain_library::Seat* right) {
            return left->place < right->place;
        }
    );
    for (const data::brain_library::Seat* seat : by_place) {
        if (seat->brain.empty())
            continue;
        std::vector<std::string>& brains = replay.meta.content.brains;
        if (std::find(brains.begin(), brains.end(), seat->brain) == brains.end())
            brains.push_back(seat->brain);
    }
}

} // namespace

ImportedRecording
describe_imported_recording(std::span<const uint8_t> bytes, std::string_view engine_version) {
    const tad::ParseResult parsed = tad::parse(bytes);
    if (!parsed.ok())
        return failed(parsed.error->message);
    const tad::Demo& demo = *parsed.demo;
    std::string reason;
    std::vector<uint8_t> recording = plain_recording(bytes, reason);
    if (!reason.empty())
        return failed(std::move(reason));
    ImportedRecording described;
    oademo::Replay& replay = described.replay;
    replay.recording = std::move(recording);
    oademo::set_recording_identity(replay);
    replay.meta.recording.tad_version = demo.version;
    replay.meta.recording.packets = demo.packets.size();
    replay.meta.recording.addresses_removed = true;
    replay.meta.recorded.how = "imported";
    replay.meta.recorded.after_crash = false;
    if (const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::recorder_version))
        replay.meta.recorded.by = utf8_text(tad::sector_text(*sector));
    if (const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::recording_date)) {
        // A recorder's date has no time zone, so it is kept as the text it
        // is and `recorded.at`, an instant in UTC, is left out.
        replay.meta.recorded.local = utf8_text(tad::sector_text(*sector));
    }
    replay.meta.game.kind = "multiplayer";
    if (!demo.packets.empty())
        replay.meta.game.length_ms = demo.packets.back().time_ms;
    replay.meta.map.name = utf8_text(demo.map_name);
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
    const oademo::UnitTableDigest digest = oademo::unit_table_digest(demo.unit_checks);
    replay.meta.units.count = digest.count;
    replay.meta.units.table_sha256 = digest.sha256;
    replay.meta.units.index_bits = digest.index_bits;
    for (const tad::Player& player : demo.players) {
        const RecordedPlayer recorded = demo_recorded_player(demo, player);
        const bool watcher =
            recorded.from_status && (recorded.info.options & OA_SETUP_OPTION_WATCHER) != 0;
        oademo::PlayerInfo info;
        info.number = player.number;
        info.name = utf8_text(player.name);
        info.side = watcher ? "watcher" : side_name(recorded.info.side);
        info.colour = recorded.info.color;
        info.kind = watcher ? "watcher" : "human";
        replay.meta.players.push_back(std::move(info));
    }
    replay.manifest.engine.recorded_with = std::string(engine_version);
    replay.manifest.engine.needs = std::string(">=") + std::string(first_release_reading_replays);
    oademo::Need need;
    need.kind = "base";
    need.id = "totala";
    need.version = "3.1c";
    replay.manifest.needs.push_back(std::move(need));
    add_mutators(demo, replay);
    add_brains(demo, replay);
    return described;
}

} // namespace oa::session::demo
