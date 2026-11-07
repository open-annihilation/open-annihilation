// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// An exported recording: version 5, every packet plain, no network address.
// A .tad cuts names and statuses to what TA Demo Recorder reads. A recording
// stored in a .oademo does not, and keeps Open Annihilation's own sectors.

#include "oa/formats/tad.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::tad {
namespace {

bool oa_sector(uint32_t type) noexcept {
    return type == static_cast<uint32_t>(SectorType::mutators) ||
           type == static_cast<uint32_t>(SectorType::brains) ||
           type == static_cast<uint32_t>(SectorType::limits);
}

bool wide_record_type(uint8_t type) noexcept {
    return type >= first_wide_recorded_type &&
           static_cast<std::size_t>(type - first_wide_recorded_type) < wide_record_lengths.size();
}

/// Cuts `text` to `most` bytes. Set `cut` when it was longer.
std::string cut_bytes(std::string text, std::size_t most, bool& cut) {
    if (text.size() <= most)
        return text;
    cut = true;
    text.resize(most);
    return text;
}

/// Reads a JSON string starting at the opening quote. `end` is the byte after it.
bool take_string(std::string_view text, std::size_t index, std::string& value, std::size_t& end) {
    if (index >= text.size() || text[index] != '"')
        return false;
    value.clear();
    std::size_t at = index + 1;
    while (at < text.size()) {
        const char character = text[at];
        if (character == '\\') {
            if (at + 1 >= text.size())
                return false;
            value.push_back(text[at + 1]);
            at += 2;
            continue;
        }
        if (character == '"') {
            end = at + 1;
            return true;
        }
        value.push_back(character);
        ++at;
    }
    return false;
}

void skip_space(std::string_view text, std::size_t& index) {
    while (index < text.size()) {
        const char character = text[index];
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r')
            return;
        ++index;
    }
}

/// Mutator ids in a loadout, each entry's own `id`, not one inside its options.
///
/// The text is `{"mutators":[{...}],"oaloadout":1}`. An entry's object sits
/// three braces deep: the loadout, the array, then the entry. An `id` inside
/// `options` is one deeper and is not a mutator.
std::vector<std::string> mutator_ids(std::string_view text) {
    std::vector<std::string> ids;
    int depth = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const char character = text[index];
        if (character == '"') {
            std::string value;
            std::size_t after = index;
            if (!take_string(text, index, value, after))
                break;
            index = after;
            skip_space(text, index);
            if (depth == 3 && value == "id" && index < text.size() && text[index] == ':') {
                ++index;
                skip_space(text, index);
                std::string id;
                if (!take_string(text, index, id, after))
                    break;
                index = after;
                if (!id.empty())
                    ids.push_back(std::move(id));
            }
            continue;
        }
        if (character == '{' || character == '[')
            ++depth;
        else if ((character == '}' || character == ']') && depth > 0)
            --depth;
        ++index;
    }
    return ids;
}

bool packet_has_wide_record(std::span<const uint8_t> payload) {
    const RecordSplit split = split_records(payload, RecordForms::wide);
    for (const Record& record : split.records) {
        if (wide_record_type(record.type))
            return true;
    }
    return false;
}

} // namespace

ExportResult export_recording(std::span<const uint8_t> bytes, ExportOptions options) {
    ExportResult result;
    const ParseResult parsed = parse(bytes);
    if (!parsed.ok()) {
        result.error = parsed.error;
        return result;
    }
    const Demo& source = *parsed.demo;

    bool names_cut = false;
    bool status_cut = false;
    bool wide = false;
    std::vector<std::string> mutators;
    bool played_with_mutators = false;

    // Owned copies. The demo's spans view them, so nothing here grows after
    // those spans are taken.
    const std::string map_name = options.fit_tadr_sizes
                                     ? cut_bytes(source.map_name, exported_name_bytes, names_cut)
                                     : source.map_name;
    std::vector<std::vector<uint8_t>> sector_data;
    std::vector<uint32_t> sector_types;
    sector_data.reserve(source.sectors.size());
    sector_types.reserve(source.sectors.size());
    for (const Sector& sector : source.sectors) {
        if (sector.type == static_cast<uint32_t>(SectorType::player_address))
            continue;
        if (sector.type == static_cast<uint32_t>(SectorType::mutators)) {
            played_with_mutators = true;
            mutators = mutator_ids(sector_text(sector));
        }
        if (!options.keep_oa_sectors && oa_sector(sector.type))
            continue;
        sector_types.push_back(sector.type);
        sector_data.emplace_back(sector.data.begin(), sector.data.end());
    }

    std::vector<Player> players;
    players.reserve(source.players.size());
    for (const Player& player : source.players) {
        Player copy = player;
        if (options.fit_tadr_sizes)
            copy.name = cut_bytes(std::move(copy.name), exported_name_bytes, names_cut);
        players.push_back(std::move(copy));
    }

    std::vector<std::vector<uint8_t>> status_data;
    std::vector<uint8_t> status_numbers;
    status_data.reserve(source.statuses.size());
    status_numbers.reserve(source.statuses.size());
    for (const PlayerStatus& status : source.statuses) {
        status_numbers.push_back(status.number);
        std::vector<uint8_t> datagram(status.datagram.begin(), status.datagram.end());
        if (options.fit_tadr_sizes && datagram.size() > exported_status_bytes) {
            datagram.resize(exported_status_bytes);
            status_cut = true;
        }
        status_data.push_back(std::move(datagram));
    }

    std::vector<uint8_t> unit_checks(source.unit_checks.begin(), source.unit_checks.end());

    struct HeldPacket {
        uint16_t delay_ms{};
        uint8_t sender{};
        std::vector<uint8_t> payload{};
    };

    std::vector<HeldPacket> packets;
    packets.reserve(source.packets.size());
    for (const Packet& packet : source.packets) {
        const DecodedPayload decoded = decode_payload(packet.payload);
        if (!decoded.ok()) {
            result.error = decoded.error;
            return result;
        }
        if (packet_has_wide_record(decoded.bytes))
            wide = true;
        HeldPacket held;
        held.delay_ms = packet.delay_ms;
        held.sender = packet.sender;
        held.payload = decoded.bytes;
        packets.push_back(std::move(held));
    }

    Demo demo;
    demo.version = supported_version;
    demo.max_units = source.max_units;
    demo.map_name = map_name;
    demo.players = std::move(players);
    demo.unit_checks = unit_checks;
    demo.sectors.reserve(sector_data.size());
    for (std::size_t at = 0; at < sector_data.size(); ++at)
        demo.sectors.push_back(Sector{sector_types[at], sector_data[at]});
    demo.statuses.reserve(status_data.size());
    for (std::size_t at = 0; at < status_data.size(); ++at)
        demo.statuses.push_back(PlayerStatus{status_numbers[at], status_data[at]});
    demo.packets.reserve(packets.size());
    for (const HeldPacket& packet : packets)
        demo.packets.push_back(Packet{0, packet.delay_ms, 0, packet.sender, packet.payload});

    WriteResult written = write(demo);
    if (!written.ok()) {
        result.error = std::move(written.error);
        return result;
    }
    result.bytes = std::move(written.bytes);
    if (names_cut)
        result.notices.push_back(ExportNotice{ExportNoticeKind::names_cut, {}});
    if (status_cut)
        result.notices.push_back(ExportNotice{ExportNoticeKind::status_cut, {}});
    if (wide)
        result.notices.push_back(ExportNotice{ExportNoticeKind::wide_records, {}});
    if (played_with_mutators)
        result.notices.push_back(ExportNotice{ExportNoticeKind::mutators, std::move(mutators)});
    return result;
}

} // namespace oa::formats::tad
