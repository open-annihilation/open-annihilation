// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo/replay_record.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/player_setup.h"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/tad.hpp"
#include "oa/session/demo/place_file.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::session::demo {

namespace {

namespace oademo = formats::oademo;
namespace tad = formats::tad;
namespace fs = std::filesystem;

/// The smallest chunk that holds a packet: the length word, the delay and the sender.
constexpr std::size_t smallest_packet_chunk =
    tad::layout::chunk_length_bytes + tad::layout::packet_fixed_bytes;

uint16_t load16(const uint8_t* at) noexcept {
    return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

uint32_t load32(const uint8_t* at) noexcept {
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
           (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

std::string side_name(uint8_t side) {
    if (side == static_cast<uint8_t>(tad::RecordedSide::arm))
        return "arm";
    if (side == static_cast<uint8_t>(tad::RecordedSide::core))
        return "core";
    return std::to_string(side);
}

/// Reads a file whole.
///
/// @param path the file
/// @param[out] bytes its bytes
/// @param[out] error why it could not be read
/// @param[out] transient true when the disk refused the read, false when the file is too long
/// @return false when it cannot be opened, is longer than a recording may be, or is not read whole
bool read_file(
    const fs::path& path, std::vector<uint8_t>& bytes, std::string& error, bool& transient
) {
    transient = true;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "the file cannot be opened";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (!in || end < 0) {
        error = "the file cannot be read";
        return false;
    }
    if (static_cast<uint64_t>(end) > tad::limit::input_bytes) {
        error = "the recording is too long to finish";
        transient = false;
        return false;
    }
    in.seekg(0);
    bytes.resize(static_cast<std::size_t>(end));
    if (end > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), end);
    if (!in) {
        error = "the file cannot be read";
        return false;
    }
    return true;
}

/// Writes a file whole.
///
/// @param path the file; replaced
/// @param bytes what it holds
/// @return false when it cannot be written
bool write_file(const fs::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    out.flush();
    return static_cast<bool>(out);
}

/// The file a finished file is written to, beside its stream, before it is placed.
fs::path work_file(const fs::path& stream_file) {
    fs::path made = stream_file;
    made += ".writing";
    return made;
}

/// Names the mutators of a loadout in the description, and asks for each one
/// by package in the manifest.
void add_mutators(const RecordingSetup& setup, oademo::Replay& replay) {
    if (setup.loadout_text.empty())
        return;
    const std::optional<oa::data::mutators::Loadout> loadout =
        oa::data::mutators::parse_loadout(setup.loadout_text, nullptr);
    if (!loadout.has_value())
        return;
    for (const oa::data::mutators::LoadoutEntry& entry : loadout->entries) {
        replay.meta.content.mutators.push_back(entry.id);
        oademo::Need need;
        need.kind = "oamut";
        need.id = entry.id;
        need.version = std::optional<std::string>(entry.version);
        const std::array<char, oa::base::sha256::hex_size> hex =
            oa::base::sha256::to_hex(entry.package);
        need.sha256 = std::optional<std::string>(std::in_place, hex.data(), hex.size());
        replay.manifest.needs.push_back(std::move(need));
    }
}

/// Names the brains of a seats text in the description, each once, in the
/// order of the places they played. A brain is never a need: playback seats none.
void add_brains(const RecordingSetup& setup, oademo::Replay& replay) {
    if (setup.brains_text.empty())
        return;
    const std::optional<oa::data::brain_library::Seats> seats =
        oa::data::brain_library::parse_seats(setup.brains_text);
    if (!seats.has_value())
        return;
    std::vector<const oa::data::brain_library::Seat*> by_place;
    by_place.reserve(seats->seats.size());
    for (const oa::data::brain_library::Seat& seat : seats->seats)
        by_place.push_back(&seat);
    std::stable_sort(
        by_place.begin(),
        by_place.end(),
        [](const oa::data::brain_library::Seat* left, const oa::data::brain_library::Seat* right) {
            return left->place < right->place;
        }
    );
    for (const oa::data::brain_library::Seat* seat : by_place) {
        if (seat->brain.empty())
            continue;
        std::vector<std::string>& brains = replay.meta.content.brains;
        if (std::find(brains.begin(), brains.end(), seat->brain) == brains.end())
            brains.push_back(seat->brain);
    }
}

} // namespace

oademo::Replay replay_begin(const RecordingSetup& setup, const ReplayFacts& facts) {
    oademo::Replay replay;
    const auto header = recording_header(setup);
    if (header.ok())
        replay.recording = header.bytes;
    oademo::set_recording_identity(replay);
    auto& meta = replay.meta;
    meta.recording.tad_version = tad::supported_version;
    meta.recording.packets = 0;
    meta.recording.addresses_removed = true;
    if (!facts.recorded_at.empty())
        meta.recorded.at = facts.recorded_at;
    meta.recorded.local = setup.date_text;
    if (!facts.recorded_by.empty())
        meta.recorded.by = facts.recorded_by;
    if (!facts.platform.empty())
        meta.recorded.platform = facts.platform;
    meta.recorded.how = facts.how;
    meta.recorded.after_crash = false;
    meta.game.kind = facts.game_kind;
    if (!facts.game_kind_object.empty())
        meta.game.kind_object = facts.game_kind_object;
    meta.map.name = facts.map_name;
    meta.content.base.id = facts.base_game;
    meta.content.base.version = facts.base_version;
    meta.content.expansions = facts.expansions;
    meta.content.mod = facts.mod;
    if (setup.limits) {
        meta.content.limits.weapon_types = setup.limits->weapon_types;
        meta.content.limits.sides = setup.limits->sides;
    } else {
        meta.content.limits.weapon_types = tad::recorded_base_weapon_types;
        meta.content.limits.sides = tad::recorded_base_sides;
    }
    std::vector<uint8_t> checks;
    for (const auto& check : setup.unit_checks)
        checks.insert(checks.end(), check.begin(), check.end());
    const oademo::UnitTableDigest digest = oademo::unit_table_digest(checks);
    meta.units.count = digest.count;
    meta.units.table_sha256 = digest.sha256;
    meta.units.index_bits = digest.index_bits;
    for (std::size_t index = 0; index < setup.players.size(); ++index) {
        const RecordingPlayer& player = setup.players[index];
        const ReplayPlayerFacts* known =
            index < facts.players.size() ? &facts.players[index] : nullptr;
        const bool watcher = (player.info.options & OA_SETUP_OPTION_WATCHER) != 0;
        oademo::PlayerInfo info;
        info.number = static_cast<int>(index + 1);
        info.name = known != nullptr ? known->name : player.name;
        info.side = watcher ? "watcher" : side_name(player.info.side);
        info.colour = player.info.color;
        info.team = static_cast<int>(player.team);
        info.kind = known != nullptr && !known->kind.empty() ? known->kind
                                                             : (watcher ? "watcher" : "human");
        if (known != nullptr)
            info.recorded_here = known->recorded_here;
        meta.players.push_back(std::move(info));
    }

    replay.manifest.engine.recorded_with = facts.engine_version;
    replay.manifest.engine.needs = std::string(">=") + std::string(first_release_reading_replays);
    oademo::Need base;
    base.kind = "base";
    base.id = facts.base_game;
    base.version = std::optional<std::string>(facts.base_version);
    replay.manifest.needs.push_back(std::move(base));
    for (const std::string& expansion : facts.expansions) {
        oademo::Need need;
        need.kind = "expansion";
        need.id = expansion;
        replay.manifest.needs.push_back(std::move(need));
    }
    for (oademo::Need need : facts.packages) {
        if (need.kind == "oamod" && !need.units_table_sha256.has_value() && digest.count != 0)
            need.units_table_sha256 = digest.sha256;
        replay.manifest.needs.push_back(std::move(need));
    }
    add_mutators(setup, replay);
    add_brains(setup, replay);
    return replay;
}

std::size_t whole_packets_end(std::span<const uint8_t> stream, uint64_t* packets) {
    if (packets != nullptr)
        *packets = 0;
    std::size_t at = 0;
    // The length of the whole chunk at a position; 0 when it is not all there.
    const auto chunk_at = [&stream](std::size_t position) -> std::size_t {
        if (position + tad::layout::chunk_length_bytes > stream.size())
            return 0;
        const std::size_t length = load16(stream.data() + position);
        if (length < tad::layout::chunk_length_bytes || position + length > stream.size())
            return 0;
        return length;
    };
    const std::size_t header = chunk_at(at);
    if (header < tad::layout::chunk_length_bytes + tad::layout::header_fixed_bytes)
        return 0;
    const std::size_t player_count =
        stream[at + tad::layout::chunk_length_bytes + tad::layout::magic_bytes + 2];
    at += header;
    const std::size_t counted = chunk_at(at);
    if (counted != tad::layout::chunk_length_bytes + tad::layout::sector_count_bytes)
        return 0;
    const uint32_t sectors = load32(stream.data() + at + tad::layout::chunk_length_bytes);
    at += counted;
    if (sectors > tad::limit::sectors)
        return 0;
    // The sectors, the players, their statuses, and the unit checks.
    const std::size_t before_packets = sectors + player_count * 2 + 1;
    for (std::size_t index = 0; index < before_packets; ++index) {
        const std::size_t length = chunk_at(at);
        if (length == 0)
            return 0;
        at += length;
    }
    uint64_t whole = 0;
    for (;;) {
        const std::size_t length = chunk_at(at);
        if (length < smallest_packet_chunk)
            break;
        at += length;
        ++whole;
    }
    if (packets != nullptr)
        *packets = whole;
    return at;
}

FinishResult finish_as_replay(
    const fs::path& stream_file,
    oademo::Replay replay,
    const ReplayEnd& end,
    const fs::path& destination,
    bool replace
) {
    FinishResult result;
    std::vector<uint8_t> bytes;
    if (!read_file(stream_file, bytes, result.error, result.transient))
        return result;
    result.transient = false;
    uint64_t packets = 0;
    const std::size_t whole = whole_packets_end(bytes, &packets);
    if (whole == 0) {
        result.error = "the recording does not hold its header";
        return result;
    }
    if (packets == 0) {
        result.empty = true;
        result.error = "the recording holds no packet";
        return result;
    }
    bytes.resize(whole);
    const tad::ParseResult parsed = tad::parse(bytes);
    if (!parsed.ok()) {
        result.error = "the recording cannot be read: " + parsed.error->message;
        return result;
    }
    for (const tad::Packet& packet : parsed.demo->packets) {
        if (packet.payload.empty() || packet.payload[0] != tad::payload_marker) {
            result.error = "the recording holds a packet that is not plain";
            return result;
        }
    }
    replay.meta.recording.packets = packets;
    replay.meta.recording.tad_version = parsed.demo->version;
    replay.meta.recording.addresses_removed = true;
    replay.meta.recorded.after_crash = end.after_crash;
    replay.meta.game.ended = end.ended;
    replay.meta.game.length_ticks = end.length_ticks;
    if (parsed.demo->packets.empty())
        replay.meta.game.length_ms.reset();
    else
        replay.meta.game.length_ms = parsed.demo->packets.back().time_ms;
    replay.recording = std::move(bytes);
    oademo::set_recording_identity(replay);
    const oademo::WriteResult written =
        oademo::write(replay, oademo::WriteOptions{oademo::level_when_a_game_ends, false});
    if (!written.ok()) {
        result.error = written.error;
        return result;
    }
    result.transient = true;
    std::error_code failure;
    fs::create_directories(destination.parent_path(), failure);
    const fs::path made = work_file(stream_file);
    if (!write_file(made, written.bytes)) {
        fs::remove(made, failure);
        result.error = "the replay cannot be written";
        return result;
    }
    if (!place_file(made, destination, replace, result.placed, result.error)) {
        fs::remove(made, failure);
        return result;
    }
    result.transient = false;
    fs::remove(stream_file, failure);
    result.ok = true;
    result.packets = packets;
    return result;
}

FinishResult finish_as_tad(
    const fs::path& stream_file, const fs::path& destination, bool keep_empty, bool replace
) {
    FinishResult result;
    std::vector<uint8_t> bytes;
    if (!read_file(stream_file, bytes, result.error, result.transient))
        return result;
    result.transient = false;
    uint64_t packets = 0;
    const std::size_t whole = whole_packets_end(bytes, &packets);
    if (whole == 0) {
        result.error = "the recording does not hold its header";
        return result;
    }
    if (packets == 0 && !keep_empty) {
        result.empty = true;
        result.error = "the recording holds no packet";
        return result;
    }
    std::error_code failure;
    result.transient = true;
    if (whole < bytes.size()) {
        // Cut part way through a packet: keep the packets that are whole.
        const fs::path made = work_file(stream_file);
        fs::create_directories(destination.parent_path(), failure);
        if (!write_file(made, std::span<const uint8_t>(bytes.data(), whole))) {
            fs::remove(made, failure);
            result.error = "the recording cannot be written";
            return result;
        }
        if (!place_file(made, destination, replace, result.placed, result.error)) {
            fs::remove(made, failure);
            return result;
        }
        fs::remove(stream_file, failure);
    } else if (!place_file(stream_file, destination, replace, result.placed, result.error)) {
        return result;
    }
    result.transient = false;
    result.ok = true;
    result.packets = packets;
    return result;
}

} // namespace oa::session::demo
