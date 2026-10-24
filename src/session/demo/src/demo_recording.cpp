// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo/recording.hpp"

#include "oa/formats/sqsh.hpp"
#include "oa/netgame/frame.hpp"
#include "oa/netgame/network.hpp"
#include "oa/netgame/records.hpp"

#include <algorithm>
#include <cstring>

namespace oa::session::demo {

namespace {

namespace tad = formats::tad;

// Stored player addresses are masked with this byte.
constexpr uint8_t address_mask = 0x2a;
// A unit state with no entries and no full record: header, terminator, flag.
constexpr std::size_t empty_unit_state_bytes = 11;
constexpr uint8_t first_player_number = 1;
constexpr uint32_t no_sender = 0;

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
        if (setup.players[i].player_id == player_id)
            return static_cast<uint8_t>(first_player_number + i);
    return no_sender;
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
    if (!recording->started || record.empty() || recording->packets.size() >= tad::limit::packets)
        return;
    const auto sender = number_of(recording->setup, sender_id);
    if (sender == no_sender) {
        ++recording->dropped_records;
        return;
    }
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
    const uint64_t delay = recording->packets.empty() ? 0 : now_ms - recording->last_ms;
    recording->last_ms = now_ms;
    std::vector<uint8_t> packet;
    put_u16(packet, static_cast<uint16_t>(std::min<uint64_t>(delay, 0xffff)));
    packet.push_back(sender);
    packet.insert(packet.end(), payload.begin(), payload.end());
    recording->packets.push_back(std::move(packet));
}

tad::WriteResult recording_write(const DemoRecording& recording) {
    const auto& setup = recording.setup;
    tad::Demo demo{};
    demo.version = tad::supported_version;
    demo.max_units = setup.max_units;
    demo.map_name = setup.map_name;
    // The views below point into storage that lives until the write returns.
    std::vector<std::vector<uint8_t>> storage;
    // Recorder text, date, the loadout sector, the brains sector, then an
    // address and a status per player.
    storage.reserve(setup.players.size() * 2 + 4);
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
    for (std::size_t i = 0; i < setup.players.size(); ++i) {
        const auto& player = setup.players[i];
        std::vector<uint8_t> address(player.address.begin(), player.address.end());
        for (auto& byte : address)
            byte = static_cast<uint8_t>(byte ^ address_mask);
        demo.sectors.push_back(
            {static_cast<uint32_t>(tad::SectorType::player_address), keep(std::move(address))}
        );
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
