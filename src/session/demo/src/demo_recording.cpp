// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo/recording.hpp"

#include "oa/session/demo/recording_stream.hpp"

#include "oa/formats/sqsh.hpp"
#include "oa/netgame/frame.hpp"
#include "oa/netgame/network.hpp"
#include "oa/netgame/player_slots.hpp"
#include "oa/netgame/private_channel.hpp"
#include "oa/netgame/recorder_messages.hpp"
#include "oa/netgame/records.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace oa::session::demo {

namespace {

namespace tad = formats::tad;

// A unit state with no entries and no full record: header, terminator, flag.
constexpr std::size_t empty_unit_state_bytes = 11;
constexpr uint8_t first_player_number = 1;
constexpr uint32_t no_sender = 0;
/// Recorder, date, loadout, brains, limits and Enhanced Mode sectors.
constexpr std::size_t most_metadata_sectors = 6;
/// The two peer ids before a relayed ally-chat line's text.
constexpr std::size_t ally_chat_from_offset = 1;
constexpr std::size_t ally_chat_to_offset = ally_chat_from_offset + sizeof(uint32_t);

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

/// Returns a player's number in the recording (1..n), or 0 when it has none.
uint8_t number_of(const RecordingSetup& setup, uint32_t player_id) {
    for (std::size_t i = 0; i < setup.players.size(); ++i)
        if (setup.players[i].source_player_id.value_or(setup.players[i].player_id) == player_id)
            return static_cast<uint8_t>(first_player_number + i);
    return no_sender;
}

/// Returns the recorded id of a source peer, carrying unknown ids and the no-player sentinel.
uint32_t recorded_peer_id(const RecordingSetup& setup, uint32_t source_id) {
    if (source_id == netgame::no_player_id)
        return source_id;
    for (const auto& player : setup.players)
        if (player.source_player_id.value_or(player.player_id) == source_id)
            return player.player_id;
    return source_id;
}

/// Rewrites only peer ids in one complete record when the setup maps source ids.
///
/// A malformed record, a record with no mapped peer ids and a setup with no
/// source ids leave the caller's record unchanged. Unit ids, opaque payloads
/// and reserved fields never take part in the mapping.
bool record_peer_ids(
    const RecordingSetup& setup, std::span<const uint8_t> record, std::vector<uint8_t>& mapped
) {
    if (!std::any_of(setup.players.begin(), setup.players.end(), [](const RecordingPlayer& player) {
            return player.source_player_id.has_value();
        }))
        return false;
    bool changed = false;
    const auto remap = [&](uint32_t& id) {
        const uint32_t recorded = recorded_peer_id(setup, id);
        changed = changed || recorded != id;
        id = recorded;
    };
    if (record[0] == static_cast<uint8_t>(netgame::RecorderRecordType::ally_chat)) {
        if (record.size() != netgame::recorder_ally_chat_bytes)
            return false;
        uint32_t from = netgame::load_u32(record.data() + ally_chat_from_offset);
        uint32_t to = netgame::load_u32(record.data() + ally_chat_to_offset);
        remap(from);
        remap(to);
        if (!changed)
            return false;
        mapped.assign(record.begin(), record.end());
        netgame::store_u32(mapped.data() + ally_chat_from_offset, from);
        netgame::store_u32(mapped.data() + ally_chat_to_offset, to);
        return true;
    }
    if (record[0] == static_cast<uint8_t>(netgame::RecordType::chat)) {
        netgame::PrivateMessage message{};
        if (record.size() != netgame::private_record_bytes ||
            netgame::decode_private_message(record.data(), record.size(), &message) !=
                netgame::WireError::ok ||
            message.sub_id != netgame::private_sub_vote ||
            (message.op != netgame::vote_op_propose && message.op != netgame::vote_op_yes &&
             message.op != netgame::vote_op_no))
            return false;
        uint32_t target = netgame::load_u32(message.payload + netgame::vote_target_offset);
        remap(target);
        if (!changed)
            return false;
        netgame::store_u32(message.payload + netgame::vote_target_offset, target);
        mapped.assign(record.begin(), record.end());
        std::size_t written = 0;
        return netgame::encode_private_message(message, mapped.data(), mapped.size(), &written) ==
                   netgame::WireError::ok &&
               written == record.size();
    }
    netgame::AnyRecord decoded;
    if (netgame::decode_any_record(record.data(), record.size(), &decoded) !=
        netgame::WireError::ok)
        return false;
    switch (decoded.type) {
    case netgame::RecordType::ping:
        remap(decoded.ping.origin_player_id);
        break;
    case netgame::RecordType::unit_killed:
        remap(decoded.unit_killed.attacker_owner_id);
        break;
    case netgame::RecordType::unit_transfer:
        remap(decoded.unit_transfer.new_owner_id);
        break;
    case netgame::RecordType::resource_give:
        remap(decoded.resource_give.from_id);
        remap(decoded.resource_give.to_id);
        break;
    case netgame::RecordType::reject:
        remap(decoded.reject.player_id);
        break;
    case netgame::RecordType::disconnect_notice:
        remap(decoded.disconnect_notice.player_id);
        break;
    case netgame::RecordType::start_position_ack:
        remap(decoded.start_position_ack.player_id);
        break;
    case netgame::RecordType::player_info:
        remap(decoded.player_info.player_id);
        break;
    case netgame::RecordType::machine_group_request:
        remap(decoded.machine_group_request.player_id);
        remap(decoded.machine_group_request.same_machine_id);
        break;
    case netgame::RecordType::machine_group_reply:
        remap(decoded.machine_group_reply.player_id);
        break;
    case netgame::RecordType::alliance:
        remap(decoded.alliance.player_id_a);
        remap(decoded.alliance.player_id_b);
        break;
    case netgame::RecordType::player_team:
        remap(decoded.player_team.player_id);
        break;
    case netgame::RecordType::slot_table:
        for (auto& id : decoded.slot_table.slot_ids)
            if (id != 0)
                remap(id);
        break;
    case netgame::RecordType::integrity_notice:
        remap(decoded.integrity_notice.player_id);
        break;
    default:
        return false;
    }
    if (!changed)
        return false;
    mapped.assign(record.begin(), record.end());
    std::size_t written = 0;
    return netgame::encode_any_record(decoded, mapped.data(), mapped.size(), &written) ==
               netgame::WireError::ok &&
           written == record.size();
}

/// The status datagram of one player: its setup block and team, as sent.
std::vector<uint8_t> status_datagram(const RecordingPlayer& player) {
    netgame::PlayerInfoRecord info{};
    const auto* bytes = reinterpret_cast<const uint8_t*>(&player.info);
    std::memcpy(info.info_head, bytes, sizeof info.info_head);
    info.player_id = player.player_id;
    std::memcpy(info.info_tail, bytes + netgame::player_info_tail_offset, sizeof info.info_tail);
    netgame::PlayerTeamRecord team{};
    team.player_id = player.player_id;
    team.value = player.team;
    std::vector<uint8_t> payload(netgame::frame_header_bytes, 0xff); // sequence -1
    uint8_t
        wire[netgame::record_length_table[static_cast<uint8_t>(netgame::RecordType::player_info)]];
    std::size_t written = 0;
    if (netgame::encode_record(info, wire, sizeof wire, &written) == netgame::WireError::ok)
        payload.insert(payload.end(), wire, wire + written);
    if (netgame::encode_record(team, wire, sizeof wire, &written) == netgame::WireError::ok)
        payload.insert(payload.end(), wire, wire + written);
    return netgame::network::encode_stored_frame(payload);
}

} // namespace

void recording_begin(DemoRecording* recording, RecordingSetup setup) {
    *recording = DemoRecording{};
    if (setup.players.size() > tad::limit::players)
        setup.players.resize(tad::limit::players);
    recording->setup = std::move(setup);
    recording->started = true;
}

void recording_add(
    DemoRecording* recording, uint32_t sender_id, uint64_t now_ms, std::span<const uint8_t> record
) {
    if (!recording->started || record.empty() || recording->packet_count >= tad::limit::packets)
        return;
    const auto sender = number_of(recording->setup, sender_id);
    if (sender == no_sender) {
        ++recording->dropped_records;
        return;
    }
    std::vector<uint8_t> mapped;
    if (record_peer_ids(recording->setup, record, mapped))
        record = mapped;
    std::vector<uint8_t> payload{tad::payload_marker};
    if (record[0] == static_cast<uint8_t>(netgame::RecordType::unit_state) &&
        record.size() >= tad::layout::unit_state_header_bytes) {
        const auto tick = netgame::load_u32(record.data() + tad::layout::unit_state_tick_offset);
        payload.push_back(static_cast<uint8_t>(tad::RecordType::tick_base));
        put_u32(payload, tick);
        if (record.size() == empty_unit_state_bytes &&
            record[tad::layout::unit_state_header_bytes] == 0xff &&
            record[tad::layout::unit_state_header_bytes + 1] == 0xff) {
            payload.push_back(static_cast<uint8_t>(tad::RecordType::empty_tick));
        } else {
            payload.push_back(static_cast<uint8_t>(tad::RecordType::elided_unit_state));
            put_u16(payload, static_cast<uint16_t>(record.size()));
            payload.insert(
                payload.end(), record.begin() + tad::layout::unit_state_header_bytes, record.end()
            );
        }
    } else {
        payload.insert(payload.end(), record.begin(), record.end());
    }
    if (recording->setup.compress && payload.size() > 1) {
        const auto packed = formats::sqsh::encode_lz77(
            {payload.data() + 1, payload.size() - 1}, tad::limit::decoded_payload_bytes
        );
        if (packed.ok() && packed.value->size() + 1 < payload.size()) {
            std::vector<uint8_t> compressed{tad::compressed_payload_marker};
            compressed.insert(compressed.end(), packed.value->begin(), packed.value->end());
            payload = std::move(compressed);
        }
    }
    const uint64_t delay = recording->packet_count == 0 ? 0 : now_ms - recording->last_ms;
    recording->last_ms = now_ms;
    std::vector<uint8_t> packet;
    put_u16(packet, static_cast<uint16_t>(std::min<uint64_t>(delay, 0xffff)));
    packet.push_back(sender);
    packet.insert(packet.end(), payload.begin(), payload.end());
    ++recording->packet_count;
    if (recording->stream != nullptr)
        recording->stream->append(packet, now_ms);
    else
        recording->packets.push_back(std::move(packet));
}

namespace {

/// Describes the container of a recording up to its packets.
///
/// The views of the result point into `storage`, which the caller keeps
/// alive until the container is written.
///
/// @param setup the game being recorded
/// @param[out] storage the bytes the sectors, players and statuses view
/// @return the container without its packets
tad::Demo build_demo(const RecordingSetup& setup, std::vector<std::vector<uint8_t>>& storage) {
    tad::Demo demo{};
    demo.version = tad::supported_version;
    demo.max_units = setup.max_units;
    demo.map_name = setup.map_name;
    // Recorder text, date, the loadout sector, the brains sector, the limits
    // sector, the Enhanced Mode sector, then a status per player and the unit
    // checks. The storage never reallocates, so the views below stay where they are.
    storage.reserve(setup.players.size() + most_metadata_sectors + 1);
    const auto keep = [&](std::vector<uint8_t> bytes) -> std::span<const uint8_t> {
        storage.push_back(std::move(bytes));
        return storage.back();
    };
    demo.sectors.push_back(
        {static_cast<uint32_t>(tad::SectorType::recorder_version),
         keep({setup.recorder_text.begin(), setup.recorder_text.end()})}
    );
    demo.sectors.push_back(
        {static_cast<uint32_t>(tad::SectorType::recording_date),
         keep({setup.date_text.begin(), setup.date_text.end()})}
    );
    // One sector chunk holds the length word, the type word and the text.
    if (!setup.loadout_text.empty() && setup.loadout_text.size() <= recording_loadout_most_bytes) {
        demo.sectors.push_back(
            {static_cast<uint32_t>(tad::SectorType::mutators),
             keep({setup.loadout_text.begin(), setup.loadout_text.end()})}
        );
    }
    // One sector chunk holds the length word, the type word and the text.
    // A text that does not fit is left out, and the recording still stands.
    if (!setup.brains_text.empty() && setup.brains_text.size() <= recording_brains_most_bytes) {
        demo.sectors.push_back(
            {static_cast<uint32_t>(tad::SectorType::brains),
             keep({setup.brains_text.begin(), setup.brains_text.end()})}
        );
    }
    if (setup.limits) {
        const auto encoded = formats::tad::encode_limits_sector(*setup.limits);
        demo.sectors.push_back(
            {static_cast<uint32_t>(tad::SectorType::limits), keep({encoded.begin(), encoded.end()})}
        );
    }
    if (setup.enhanced_mode.revision != 0) {
        const formats::tad::RecordedEnhancedMode recorded{
            setup.enhanced_mode.revision,
            oa::data::enhanced_mode::sector_feature_bits(setup.enhanced_mode.features),
            setup.needed_engine_version
        };
        const auto encoded = formats::tad::encode_enhanced_mode_sector(recorded);
        demo.sectors.push_back(
            {static_cast<uint32_t>(tad::SectorType::enhanced_mode),
             keep({encoded.begin(), encoded.end()})}
        );
    }
    for (std::size_t i = 0; i < setup.players.size(); ++i) {
        const auto& player = setup.players[i];
        tad::Player recorded{};
        recorded.color = player.info.color;
        recorded.side = player.info.side;
        recorded.number = static_cast<uint8_t>(first_player_number + i);
        recorded.name = player.name;
        demo.players.push_back(std::move(recorded));
        demo.statuses.push_back(
            {static_cast<uint8_t>(first_player_number + i), keep(status_datagram(player))}
        );
    }
    std::vector<uint8_t> checks;
    for (const auto& check : setup.unit_checks)
        checks.insert(checks.end(), check.begin(), check.end());
    demo.unit_checks = keep(std::move(checks));
    return demo;
}

} // namespace

tad::WriteResult recording_header(const RecordingSetup& setup) {
    std::vector<std::vector<uint8_t>> storage;
    return tad::write(build_demo(setup, storage));
}

tad::WriteResult recording_write(const DemoRecording& recording) {
    std::vector<std::vector<uint8_t>> storage;
    tad::Demo demo = build_demo(recording.setup, storage);
    uint64_t time = 0;
    for (const auto& packet : recording.packets) {
        tad::Packet entry{};
        entry.delay_ms = static_cast<uint16_t>(packet[0] | (packet[1] << 8));
        time += entry.delay_ms;
        entry.time_ms = time;
        entry.sender = packet[2];
        entry.payload = std::span<const uint8_t>(packet).subspan(tad::layout::packet_fixed_bytes);
        demo.packets.push_back(entry);
    }
    return tad::write(demo);
}

bool recording_stream_to(
    DemoRecording* recording, const std::filesystem::path& path, std::string* error
) {
    const auto fail = [error](std::string text) {
        if (error != nullptr)
            *error = std::move(text);
        return false;
    };
    if (recording == nullptr || !recording->started)
        return fail("the recording has not begun");
    if (recording->packet_count != 0)
        return fail("the recording already holds packets");
    const auto header = recording_header(recording->setup);
    if (!header.ok())
        return fail(header.error->message);
    auto stream = std::make_shared<RecordingStream>();
    if (!stream->open(path, header.bytes, error))
        return false;
    recording->stream = std::move(stream);
    return true;
}

std::string recording_file_name(
    std::string_view date_time, std::string_view map_name, std::string_view extension
) {
    std::string name;
    name.append(date_time);
    if (!date_time.empty())
        name.push_back(' ');
    name.append(map_name);
    for (auto& c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20)
            c = '_';
    name.append(extension);
    return name;
}

} // namespace oa::session::demo
