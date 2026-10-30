// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Summarises TA Demo recordings: header, players, packet timing and a
// histogram of record type bytes across all packet payloads.

#include "oa/formats/tad.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tad = oa::formats::tad;

namespace {

std::string_view until_nul(std::string_view text) {
    return text.substr(0, text.find('\0'));
}

const char* recorder_type_name(uint8_t type) {
    switch (static_cast<tad::RecordType>(type)) {
    case tad::RecordType::recorder_command:
        return "recorder command";
    case tad::RecordType::enemy_chat:
        return "enemy chat";
    case tad::RecordType::replayer_server:
        return "replayer server";
    case tad::RecordType::recorder_message:
        return "recorder message";
    case tad::RecordType::map_position:
        return "map position";
    case tad::RecordType::elided_unit_state:
        return "unit state, tick elided";
    case tad::RecordType::tick_base:
        return "tick base";
    case tad::RecordType::empty_tick:
        return "empty tick";
    default:
        return "";
    }
}

bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > tad::limit::input_bytes)
        return false;
    std::ifstream stream(path, std::ios::binary);
    bytes.resize(static_cast<std::size_t>(size));
    return static_cast<bool>(
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))
    );
}

double percent(uint64_t part, uint64_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

bool dump(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) {
        std::fprintf(
            stderr, "%s: cannot read (missing or over the size limit)\n", path.string().c_str()
        );
        return false;
    }
    const auto result = tad::parse(bytes);
    if (!result.ok()) {
        std::fprintf(
            stderr,
            "%s: offset %zu: %s\n",
            path.string().c_str(),
            result.error->offset,
            result.error->message.c_str()
        );
        return false;
    }
    const tad::Demo& demo = *result.demo;

    std::printf("file            %s\n", path.string().c_str());
    std::printf("version         %u\n", demo.version);
    std::printf("map             %s\n", std::string(until_nul(demo.map_name)).c_str());
    std::printf("unit limit      %u\n", demo.max_units);
    if (const auto* sector = tad::find_sector(demo, tad::SectorType::recorder_version))
        std::printf("recorder        %s\n", std::string(tad::sector_text(*sector)).c_str());
    if (const auto* sector = tad::find_sector(demo, tad::SectorType::recording_date))
        std::printf("date            %s\n", std::string(tad::sector_text(*sector)).c_str());
    tad::RecordForms record_forms = tad::RecordForms::narrow;
    if (const auto* limits_sector = tad::find_sector(demo, tad::SectorType::limits)) {
        if (const auto decoded = tad::decode_limits_sector(*limits_sector)) {
            std::printf(
                "limits          %u weapon types, %u sides\n", decoded->weapon_types, decoded->sides
            );
            if (decoded->weapon_types > tad::recorded_base_weapon_types)
                record_forms = tad::RecordForms::wide;
        }
    }
    for (const auto& sector : demo.sectors)
        std::printf("sector          type %u, %zu bytes\n", sector.type, sector.data.size());
    for (const auto& player : demo.players) {
        const char* side = player.side == static_cast<uint8_t>(tad::RecordedSide::arm)    ? "arm"
                           : player.side == static_cast<uint8_t>(tad::RecordedSide::core) ? "core"
                                                                                          : "?";
        std::printf(
            "player          #%u %-20s side %u (%s) color %u\n",
            player.number,
            std::string(until_nul(player.name)).c_str(),
            player.side,
            side,
            player.color
        );
    }
    for (const auto& status : demo.statuses)
        std::printf(
            "status          #%u datagram type 0x%02x, %zu bytes\n",
            status.number,
            status.datagram.empty() ? 0U : status.datagram[0],
            status.datagram.size()
        );
    std::printf("unit checks     %zu\n", tad::unit_check_records(demo).size());

    const uint64_t duration = demo.packets.empty() ? 0 : demo.packets.back().time_ms;
    std::printf(
        "packets         %zu over %llu:%02llu (%llu ms)\n",
        demo.packets.size(),
        static_cast<unsigned long long>(duration / 60000U),
        static_cast<unsigned long long>(duration / 1000U % 60U),
        static_cast<unsigned long long>(duration)
    );

    // Each sender's unit-state ticks run on from packet to packet unless a
    // frame was lost on the way (a gap) or overtaken (late ticks).
    struct SenderTicks {
        bool seen{};
        uint32_t next{};
        uint64_t last_ms{};
        uint64_t skipped{};
        uint64_t gaps{};
        uint64_t late{};
    };

    std::array<SenderTicks, 256> ticks{};
    uint64_t gap_lines = 0;
    std::array<uint64_t, 256> senders{};
    std::array<uint64_t, 256> types{};
    uint64_t records = 0;
    uint64_t decode_failures = 0;
    uint64_t split_failures = 0;
    uint64_t untimed = 0;
    for (const auto& packet : demo.packets) {
        ++senders[packet.sender];
        const auto decoded = tad::decode_payload(packet.payload);
        if (!decoded.ok()) {
            ++decode_failures;
            if (++split_failures <= 5)
                std::printf(
                    "decode failure  packet at %zu: %s\n",
                    packet.offset,
                    decoded.error->message.c_str()
                );
            continue;
        }
        const auto split = tad::split_records(decoded.bytes, record_forms);
        if (split.error && !decoded.bytes.empty()) {
            if (++split_failures <= 5)
                std::printf(
                    "split failure   packet at %zu, payload offset %zu, type 0x%02x\n",
                    packet.offset,
                    split.error->offset,
                    split.error->offset < decoded.bytes.size() ? decoded.bytes[split.error->offset]
                                                               : 0U
                );
        }
        auto& sender = ticks[packet.sender];
        const uint32_t expected = sender.next;
        std::optional<uint32_t> first;
        uint32_t late = 0;
        for (const auto& record : split.records) {
            ++types[record.type];
            ++records;
            const auto kind = static_cast<tad::RecordType>(record.type);
            if (kind != tad::RecordType::elided_unit_state && kind != tad::RecordType::empty_tick &&
                kind != tad::RecordType::unit_state)
                continue;
            if (!record.tick) {
                ++untimed;
                continue;
            }
            const uint32_t tick = *record.tick;
            if (!first)
                first = tick;
            if (sender.seen && tick < expected)
                ++late;
            sender.next = std::max(sender.next, tick + 1);
        }
        if (!first)
            continue;
        if (sender.seen && *first != expected) {
            const bool gap = *first > expected;
            if (gap) {
                sender.skipped += *first - expected;
                ++sender.gaps;
            }
            sender.late += late;
            if (++gap_lines <= 10)
                std::printf(
                    "tick %s       #%u ticks %u..%u, packet at %zu, %llu ms after its last\n",
                    gap ? "gap " : "late",
                    packet.sender,
                    gap ? expected : *first,
                    gap ? *first - 1 : *first + late - 1,
                    packet.offset,
                    static_cast<unsigned long long>(packet.time_ms - sender.last_ms)
                );
        }
        sender.seen = true;
        sender.last_ms = packet.time_ms;
    }
    for (std::size_t sender = 0; sender < senders.size(); ++sender) {
        if (senders[sender] != 0)
            std::printf(
                "sender          #%zu %llu packets, ticks skipped %llu in %llu gaps, %llu late\n",
                sender,
                static_cast<unsigned long long>(senders[sender]),
                static_cast<unsigned long long>(ticks[sender].skipped),
                static_cast<unsigned long long>(ticks[sender].gaps),
                static_cast<unsigned long long>(ticks[sender].late)
            );
    }
    std::printf("decode failures %llu\n", static_cast<unsigned long long>(decode_failures));
    std::printf("split failures  %llu\n", static_cast<unsigned long long>(split_failures));
    std::printf("untimed ticks   %llu\n", static_cast<unsigned long long>(untimed));

    std::printf("records         %llu\n", static_cast<unsigned long long>(records));
    uint64_t game = 0;
    for (std::size_t type = 0; type < types.size(); ++type) {
        if (types[type] == 0)
            continue;
        if (type >= tad::first_game_record_type && type <= tad::last_game_record_type)
            game += types[type];
        std::printf(
            "  0x%02zx %10llu %6.2f%% %s\n",
            type,
            static_cast<unsigned long long>(types[type]),
            percent(types[type], records),
            recorder_type_name(static_cast<uint8_t>(type))
        );
    }
    const uint64_t unit_states =
        types[static_cast<std::size_t>(tad::RecordType::elided_unit_state)];
    std::printf("types 0x02..0x2c        %6.2f%% of records\n", percent(game, records));
    std::printf("  counting 0xfd as 0x2c %6.2f%%\n", percent(game + unit_states, records));
    const uint64_t bookkeeping = types[static_cast<std::size_t>(tad::RecordType::tick_base)] +
                                 types[static_cast<std::size_t>(tad::RecordType::empty_tick)];
    std::printf(
        "  and without 0xfe/0xff %6.2f%%\n", percent(game + unit_states, records - bookkeeping)
    );
    std::printf("\n");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: oa-formats-tad-dump <demo.tad>...\n");
        return 2;
    }
    bool ok = true;
    for (int index = 1; index < argc; ++index)
        ok = dump(argv[index]) && ok;
    return ok ? 0 : 1;
}
