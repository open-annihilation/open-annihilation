// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool replay: pack a recording, read it as one JSON object, and unpack
// the plain stream. The packed recording holds no address sector. A recording
// of the original recorder is described as playback reads it: each player's
// side and colour from the player's status datagram, the recorder's text from
// the game's code page to UTF-8, and OA's mutators and brains from their sectors.

#include "command.hpp"

#include "oa/core/player_setup.h"
#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/writer.hpp"
#include "oa/netgame/frame.hpp"
#include "oa/netgame/network.hpp"
#include "oa/netgame/records.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Scratch {
    std::filesystem::path path;

    Scratch() : path(oa::test::make_scratch_directory("oa-tool-replay")) {}

    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

struct Captured {
    int status = 0;
    std::string out;
    std::string err;
};

Captured run(std::vector<std::string> arguments) {
    std::ostringstream out;
    std::ostringstream err;
    oa::tool::Output output{out, err};
    const int status = oa::tool::run_tool(
        std::span<const std::string>(arguments.data(), arguments.size()), output
    );
    return {status, out.str(), err.str()};
}

std::string utf8_of(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

void write_bytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(static_cast<bool>(out));
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    OA_CHECK(static_cast<bool>(out));
}

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (!in || size < 0)
        return {};
    in.seekg(0);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

bool one_json_object(std::string_view text) {
    return !text.empty() && text.front() == '{' && text.back() == '\n' &&
           text.find('\n') == text.size() - 1 && text[text.size() - 2] == '}';
}

std::vector<uint8_t> sample_recording() {
    const std::string date_text = "2026-11-04 14:32";
    const std::string address_text = "ADDR-9.9.9.9";
    const std::vector<uint8_t> recorder{'o', 'a', '-', 't', 'e', 's', 't'};
    const std::vector<uint8_t> date(date_text.begin(), date_text.end());
    const std::vector<uint8_t> address(address_text.begin(), address_text.end());
    const std::vector<uint8_t> status{0x04};
    const std::vector<uint8_t> payload{oa::formats::tad::payload_marker, 0xff};
    oa::formats::tad::Demo demo;
    demo.version = oa::formats::tad::supported_version;
    demo.max_units = 250;
    demo.map_name = "Ridge";
    demo.sectors = {
        {static_cast<uint32_t>(oa::formats::tad::SectorType::recorder_version), recorder},
        {static_cast<uint32_t>(oa::formats::tad::SectorType::recording_date), date},
        {static_cast<uint32_t>(oa::formats::tad::SectorType::player_address), address},
    };
    demo.players = {{1, 0, 1, "alpha"}};
    demo.statuses = {{1, status}};
    demo.packets = {{0, 40, 40, 1, payload}};
    const oa::formats::tad::WriteResult written = oa::formats::tad::write(demo);
    OA_CHECK(written.ok());
    return written.bytes;
}

void pack_info_unpack() {
    const Scratch scratch;
    const std::vector<uint8_t> tad_bytes = sample_recording();
    const auto tad_path = scratch.path / "ridge.tad";
    const auto packed_path = scratch.path / "ridge.oademo";
    const auto unpacked_path = scratch.path / "ridge-out.tad";
    write_bytes(tad_path, tad_bytes);

    const Captured packed = run({
        "replay",
        "pack",
        utf8_of(tad_path),
        "--out",
        utf8_of(packed_path),
    });
    OA_CHECK(packed.status == oa::tool::exit_done);
    OA_CHECK(packed.err.empty());
    OA_CHECK(packed.out == utf8_of(packed_path) + "\n");

    const std::vector<uint8_t> packed_bytes = read_bytes(packed_path);
    const oa::formats::oademo::ReadResult replay = oa::formats::oademo::read(packed_bytes);
    OA_CHECK(replay.ok());
    if (!replay.ok())
        return;
    const oa::formats::tad::ParseResult stored = oa::formats::tad::parse(replay.replay->recording);
    OA_CHECK(stored.ok());
    if (stored.ok()) {
        OA_CHECK(
            oa::formats::tad::find_sector(
                *stored.demo, oa::formats::tad::SectorType::player_address
            ) == nullptr
        );
        OA_CHECK(!stored.demo->packets.empty());
        if (!stored.demo->packets.empty())
            OA_CHECK(stored.demo->packets[0].payload[0] == oa::formats::tad::payload_marker);
    }
    const std::string address = "ADDR-9.9.9.9";
    OA_CHECK(
        std::string(replay.replay->recording.begin(), replay.replay->recording.end())
            .find(address) == std::string::npos
    );

    const Captured info = run({"replay", "info", utf8_of(packed_path), "--json"});
    OA_CHECK(info.status == oa::tool::exit_done);
    OA_CHECK(info.err.empty());
    OA_CHECK(one_json_object(info.out));
    OA_CHECK(info.out.find("\"file\":\"oademo\"") != std::string::npos);
    // A recorder's date has no time zone: it stays text in `recorded.local`, and
    // `recorded.at`, an instant in UTC, is left out.
    OA_CHECK(info.out.find("\"local\":\"2026-11-04 14:32\"") != std::string::npos);
    OA_CHECK(info.out.find("\"at\":") == std::string::npos);
    OA_CHECK(info.out.find(address) == std::string::npos);

    const Captured tad_info = run({"replay", "info", utf8_of(tad_path), "--json"});
    OA_CHECK(tad_info.status == oa::tool::exit_done);
    OA_CHECK(one_json_object(tad_info.out));
    OA_CHECK(tad_info.out.find("\"file\":\"tad\"") != std::string::npos);
    OA_CHECK(tad_info.out.find(address) == std::string::npos);
    const Captured tad_lines = run({"replay", "info", utf8_of(tad_path)});
    OA_CHECK(tad_lines.status == oa::tool::exit_done);
    OA_CHECK(tad_lines.out.find(address) == std::string::npos);
    OA_CHECK(tad_lines.out.find("oa-test") != std::string::npos);

    const Captured unpacked = run({
        "replay",
        "unpack",
        utf8_of(packed_path),
        "--out",
        utf8_of(unpacked_path),
    });
    OA_CHECK(unpacked.status == oa::tool::exit_done);
    OA_CHECK(read_bytes(unpacked_path) == replay.replay->recording);
}

// The 0x20 record of a status datagram, which carries a player's lobby block.
std::vector<uint8_t> status_datagram(uint8_t side, uint8_t colour, uint16_t options) {
    oa::PlayerSetupInfo info{};
    info.player_id = 100U + colour;
    info.side = side;
    info.color = colour;
    info.options = options;
    std::vector<uint8_t> frame(oa::netgame::frame_header_bytes, 0xff);
    frame.push_back(static_cast<uint8_t>(oa::netgame::RecordType::player_info));
    const auto* block = reinterpret_cast<const uint8_t*>(&info);
    frame.insert(frame.end(), block, block + sizeof info);
    return oa::netgame::network::encode_frame(frame, false);
}

const std::string mutators_text =
    R"({"mutators":[{"clashes":"replace","folder":false,"id":"ridge-pace","options":{"boost":false,"pace":1.5,"temper":"hot"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"clashes":"replace","folder":true,"id":"ridge-kit","options":{},"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","version":"3"}],"oaloadout":1})";

const std::string brains_text =
    R"({"seats":[{"level":"hard","place":0},{"brain":"ridge-brain","level":"firm","place":1},{"brain":"steady-commander","level":"firm","place":2},{"brain":"steady-commander","level":"firm","place":3}]})";

// A recording the way the original recorder writes one: text in the game's
// code page, a player chunk whose side bytes are not the sides, and each
// player's lobby block in the status datagram. The fourth player has none.
// With `oa_sectors` it also carries a mutators sector and a brains sector.
std::vector<uint8_t> recorder_recording(bool oa_sectors) {
    // 0xDF, 0xE9, 0xFC and 0xEB are bytes of Windows-1252: a sharp s and three
    // accented letters.
    const std::string recorder_text = "0.93\xDF"
                                      "2";
    const std::string date_text = "2002-04-15 12:33";
    const std::vector<uint8_t> recorder(recorder_text.begin(), recorder_text.end());
    const std::vector<uint8_t> date(date_text.begin(), date_text.end());
    const std::vector<uint8_t> mutators(mutators_text.begin(), mutators_text.end());
    const std::vector<uint8_t> brains(brains_text.begin(), brains_text.end());
    const std::vector<uint8_t> payload{oa::formats::tad::payload_marker, 0xff};
    const std::vector<uint8_t> unreadable{0x04};
    const std::vector<std::vector<uint8_t>> statuses{
        status_datagram(0, 3, 0),
        status_datagram(1, 4, 0),
        status_datagram(0, 6, OA_SETUP_OPTION_WATCHER),
        unreadable,
    };
    oa::formats::tad::Demo demo;
    demo.version = oa::formats::tad::supported_version;
    demo.max_units = 250;
    demo.map_name = "Caf\xE9 Rim";
    demo.sectors = {
        {static_cast<uint32_t>(oa::formats::tad::SectorType::recorder_version), recorder},
        {static_cast<uint32_t>(oa::formats::tad::SectorType::recording_date), date},
    };
    if (oa_sectors) {
        demo.sectors.push_back(
            {static_cast<uint32_t>(oa::formats::tad::SectorType::mutators), mutators}
        );
        demo.sectors.push_back(
            {static_cast<uint32_t>(oa::formats::tad::SectorType::brains), brains}
        );
    }
    const auto player = [](uint8_t number, uint8_t colour, uint8_t side, const char* name) {
        oa::formats::tad::Player chunk;
        chunk.number = number;
        chunk.color = colour;
        chunk.side = side;
        chunk.name = name;
        return chunk;
    };
    demo.players = {
        player(
            1,
            1,
            7,
            "J\xFC"
            "rgen"
        ),
        player(2, 2, 4, "Zo\xEB"),
        player(5, 5, 9, "Watcher"),
        player(7, 7, 7, "NoStatus"),
    };
    for (std::size_t index = 0; index < demo.players.size(); ++index)
        demo.statuses.push_back({demo.players[index].number, statuses[index]});
    demo.packets = {{0, 40, 40, 1, payload}};
    const oa::formats::tad::WriteResult written = oa::formats::tad::write(demo);
    OA_CHECK(written.ok());
    return written.bytes;
}

// The `players` array of an info reply, as the compact writer prints it.
std::string players_of(const std::string& json) {
    const std::size_t begin = json.find("\"players\":[");
    if (begin == std::string::npos)
        return {};
    const std::size_t end = json.find(']', begin);
    return end == std::string::npos ? std::string{} : json.substr(begin, end - begin + 1);
}

void sides_and_colours_come_from_the_status() {
    const Scratch scratch;
    const auto tad_path = scratch.path / "recorder.tad";
    const auto packed_path = scratch.path / "recorder.oademo";
    write_bytes(tad_path, recorder_recording(false));
    OA_CHECK(
        run({"replay", "pack", utf8_of(tad_path), "--out", utf8_of(packed_path)}).status ==
        oa::tool::exit_done
    );
    const std::string expected =
        "\"players\":["
        "{\"number\":1,\"name\":\"J\xC3\xBC"
        "rgen\",\"side\":\"arm\",\"colour\":3,\"kind\":\"human\"},"
        "{\"number\":2,\"name\":\"Zo\xC3\xAB\",\"side\":\"core\",\"colour\":4,\"kind\":\"human\"},"
        "{\"number\":5,\"name\":\"Watcher\",\"side\":\"watcher\",\"colour\":6,"
        "\"kind\":\"watcher\"},"
        // Without a lobby block the bytes of the player chunk stand: 7 is no side.
        "{\"number\":7,\"name\":\"NoStatus\",\"side\":\"7\",\"colour\":7,\"kind\":\"human\"}]";
    const Captured packed = run({"replay", "info", utf8_of(packed_path), "--json"});
    OA_CHECK(packed.status == oa::tool::exit_done);
    OA_CHECK(players_of(packed.out) == expected);
    const Captured loose = run({"replay", "info", utf8_of(tad_path), "--json"});
    OA_CHECK(loose.status == oa::tool::exit_done);
    OA_CHECK(one_json_object(loose.out));
    OA_CHECK(
        players_of(loose.out) ==
        "\"players\":[{\"number\":1,\"name\":\"J\xC3\xBC"
        "rgen\",\"side\":\"arm\",\"colour\":3},"
        "{\"number\":2,\"name\":\"Zo\xC3\xAB\",\"side\":\"core\",\"colour\":4},"
        "{\"number\":5,\"name\":\"Watcher\",\"side\":\"watcher\",\"colour\":6},"
        "{\"number\":7,\"name\":\"NoStatus\",\"side\":\"7\",\"colour\":7}]"
    );
    const Captured lines = run({"replay", "info", utf8_of(tad_path)});
    OA_CHECK(
        lines.out.find(
            "player 1: J\xC3\xBC"
            "rgen arm 3\n"
        ) != std::string::npos
    );
    OA_CHECK(lines.out.find("player 5: Watcher watcher 6\n") != std::string::npos);
}

void the_recorders_text_is_utf8() {
    const Scratch scratch;
    const auto tad_path = scratch.path / "recorder.tad";
    const auto packed_path = scratch.path / "recorder.oademo";
    write_bytes(tad_path, recorder_recording(false));
    OA_CHECK(
        run({"replay", "pack", utf8_of(tad_path), "--out", utf8_of(packed_path)}).status ==
        oa::tool::exit_done
    );
    const std::string recorder = "\"by\":\"0.93\xC3\x9F"
                                 "2\"";
    const std::string map = "\"map\":{\"name\":\"Caf\xC3\xA9 Rim\"}";
    const Captured packed = run({"replay", "info", utf8_of(packed_path), "--json"});
    OA_CHECK(packed.out.find(recorder) != std::string::npos);
    OA_CHECK(packed.out.find(map) != std::string::npos);
    OA_CHECK(packed.out.find("Non-Unicode") == std::string::npos);
    const std::vector<uint8_t> stored = read_bytes(packed_path);
    const oa::formats::oademo::ReadResult replay = oa::formats::oademo::read(stored);
    OA_CHECK(replay.ok());
    if (replay.ok()) {
        OA_CHECK(
            replay.replay->meta.recorded.by == std::string(
                                                   "0.93\xC3\x9F"
                                                   "2"
                                               )
        );
        OA_CHECK(replay.replay->meta.recorded.local == "2002-04-15 12:33");
        OA_CHECK(!replay.replay->meta.recorded.at.has_value());
        OA_CHECK(replay.replay->meta.map.name == "Caf\xC3\xA9 Rim");
        OA_CHECK(
            replay.replay->meta.players[0].name == "J\xC3\xBC"
                                                   "rgen"
        );
    }
    // The loose recording reads the same way.
    const Captured loose = run({"replay", "info", utf8_of(tad_path), "--json"});
    OA_CHECK(
        loose.out.find(
            "\"recorder\":\"0.93\xC3\x9F"
            "2\""
        ) != std::string::npos
    );
    OA_CHECK(loose.out.find("\"map\":\"Caf\xC3\xA9 Rim\"") != std::string::npos);
    OA_CHECK(loose.out.find("Non-Unicode") == std::string::npos);
}

void oa_sectors_fill_the_mutators_and_the_brains() {
    const Scratch scratch;
    const auto plain_path = scratch.path / "plain.tad";
    const auto rich_path = scratch.path / "rich.tad";
    const auto plain_packed = scratch.path / "plain.oademo";
    const auto rich_packed = scratch.path / "rich.oademo";
    write_bytes(plain_path, recorder_recording(false));
    write_bytes(rich_path, recorder_recording(true));
    OA_CHECK(
        run({"replay", "pack", utf8_of(plain_path), "--out", utf8_of(plain_packed)}).status ==
        oa::tool::exit_done
    );
    OA_CHECK(
        run({"replay", "pack", utf8_of(rich_path), "--out", utf8_of(rich_packed)}).status ==
        oa::tool::exit_done
    );
    const std::vector<uint8_t> plain_bytes = read_bytes(plain_packed);
    const oa::formats::oademo::ReadResult plain = oa::formats::oademo::read(plain_bytes);
    OA_CHECK(plain.ok());
    if (plain.ok()) {
        OA_CHECK(plain.replay->meta.content.mutators.empty());
        OA_CHECK(plain.replay->meta.content.brains.empty());
        OA_CHECK(plain.replay->manifest.needs.size() == 1);
    }
    const std::vector<uint8_t> rich_bytes = read_bytes(rich_packed);
    const oa::formats::oademo::ReadResult rich = oa::formats::oademo::read(rich_bytes);
    OA_CHECK(rich.ok());
    if (!rich.ok())
        return;
    const auto& content = rich.replay->meta.content;
    OA_CHECK(content.mutators == (std::vector<std::string>{"ridge-pace", "ridge-kit"}));
    // Each brain once, in the order of the places that played it; the
    // built-in player at place 0 names none.
    OA_CHECK(content.brains == (std::vector<std::string>{"ridge-brain", "steady-commander"}));
    // A mutator the replay needs is asked for by its package digest, or by its
    // folder's digest when it was a folder. A brain is never a need.
    const auto& needs = rich.replay->manifest.needs;
    OA_CHECK(needs.size() == 3);
    if (needs.size() == 3) {
        OA_CHECK(needs[1].kind == "oamut" && needs[1].id == "ridge-pace");
        OA_CHECK(needs[1].version == "1.2");
        OA_CHECK(
            needs[1].sha256 ==
            std::string("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
        );
        OA_CHECK(needs[2].kind == "oamut" && needs[2].id == "ridge-kit");
        OA_CHECK(
            needs[2].sha256 ==
            std::string("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210")
        );
    }
    // Every package a replay needs is named by a 64-digit lower-case digest; a
    // reader that refuses a package without one refuses the replay.
    for (const oa::formats::oademo::Need& need : needs) {
        if (need.kind != "oamod" && need.kind != "oamap" && need.kind != "oamut")
            continue;
        OA_CHECK(need.sha256.has_value());
        if (!need.sha256.has_value())
            continue;
        OA_CHECK(need.sha256->size() == 64);
        for (const char digit : *need.sha256)
            OA_CHECK((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'));
    }
    const Captured info = run({"replay", "info", utf8_of(rich_packed), "--json"});
    OA_CHECK(info.out.find("\"mutators\":[\"ridge-pace\",\"ridge-kit\"]") != std::string::npos);
    OA_CHECK(
        info.out.find("\"brains\":[\"ridge-brain\",\"steady-commander\"]") != std::string::npos
    );
    // The sectors stay in the recording the replay holds.
    const oa::formats::tad::ParseResult stored = oa::formats::tad::parse(rich.replay->recording);
    OA_CHECK(stored.ok());
    if (stored.ok()) {
        OA_CHECK(
            oa::formats::tad::find_sector(*stored.demo, oa::formats::tad::SectorType::mutators) !=
            nullptr
        );
        OA_CHECK(
            oa::formats::tad::find_sector(*stored.demo, oa::formats::tad::SectorType::brains) !=
            nullptr
        );
    }
}

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

// A zip whose first entry is the stored `oademo` line of version 2.
std::vector<uint8_t> version_two_file() {
    MemoryOut out;
    oa::formats::zip::StreamWriter writer({&out, write_memory}, {});
    oa::formats::zip::ZipError problem;
    const auto put =
        [&](std::string_view name, std::string_view text, oa::formats::zip::Method method) {
            const std::span<const uint8_t> bytes{
                reinterpret_cast<const uint8_t*>(text.data()), text.size()
            };
            return writer.begin_entry(name, bytes.size(), method, problem) &&
                   writer.write(bytes, problem) && writer.end_entry(problem);
        };
    OA_CHECK(
        put("oademo", "oademo 2\n", oa::formats::zip::Method::stored) &&
        put("meta.json", "{}", oa::formats::zip::Method::deflated) &&
        put("manifest.json", "{}", oa::formats::zip::Method::deflated) &&
        put("recording.tad.zst", "x", oa::formats::zip::Method::stored) && writer.finish(problem)
    );
    return out.bytes;
}

void a_version_other_than_one_is_not_a_version_this_game_reads() {
    const Scratch scratch;
    const auto path = scratch.path / "later.oademo";
    const std::vector<uint8_t> bytes = version_two_file();
    OA_CHECK(oa::formats::oademo::recognised(bytes));
    write_bytes(path, bytes);
    const Captured info = run({"replay", "info", utf8_of(path)});
    OA_CHECK(info.status == oa::tool::exit_failed);
    OA_CHECK(info.out.empty());
    OA_CHECK(info.err.find(oa::formats::oademo::not_this_version) != std::string::npos);
    const Captured json = run({"replay", "info", utf8_of(path), "--json"});
    OA_CHECK(json.status == oa::tool::exit_failed);
    OA_CHECK(json.err.find(oa::formats::oademo::not_this_version) != std::string::npos);
    OA_CHECK(json.err.find(oa::formats::oademo::not_a_replay) == std::string::npos);
}

std::vector<uint8_t> exportable_recording(bool mutators, bool long_name) {
    const std::string loadout =
        R"({"mutators":[{"id":"ridge","options":{"id":"nope"}}],"oaloadout":1})";
    const std::vector<uint8_t> mutator_bytes(loadout.begin(), loadout.end());
    const std::vector<uint8_t> address{'A', 'D', 'D', 'R'};
    const std::vector<uint8_t> status{0x04};
    const std::vector<uint8_t> payload{oa::formats::tad::payload_marker, 0xff};
    oa::formats::tad::Demo demo;
    demo.version = oa::formats::tad::supported_version;
    demo.max_units = 250;
    demo.map_name =
        long_name ? std::string(oa::formats::tad::exported_name_bytes + 1, 'M') : "Ridge";
    demo.sectors = {
        {static_cast<uint32_t>(oa::formats::tad::SectorType::player_address), address},
    };
    if (mutators)
        demo.sectors.push_back(
            {static_cast<uint32_t>(oa::formats::tad::SectorType::mutators), mutator_bytes}
        );
    demo.players = {{1, 0, 1, long_name ? std::string(80, 'N') : "alpha"}};
    demo.statuses = {{1, status}};
    demo.packets = {{0, 40, 40, 1, payload}};
    const oa::formats::tad::WriteResult written = oa::formats::tad::write(demo);
    OA_CHECK(written.ok());
    return written.bytes;
}

void export_writes_both_formats() {
    const Scratch scratch;
    const auto tad_path = scratch.path / "ridge.tad";
    const auto oademo_path = scratch.path / "ridge.oademo";
    const auto tad_out = scratch.path / "out.tad";
    write_bytes(tad_path, exportable_recording(true, true));

    const Captured missing = run({"replay", "export", utf8_of(tad_path)});
    OA_CHECK(missing.status == oa::tool::exit_usage);

    const Captured sectors = run({
        "replay",
        "export",
        utf8_of(tad_path),
        "--format",
        "oademo",
        "--no-oa-sectors",
        "--out",
        utf8_of(oademo_path),
    });
    OA_CHECK(sectors.status == oa::tool::exit_usage);
    OA_CHECK(sectors.err.find("--no-oa-sectors applies to a .tad export") != std::string::npos);

    const Captured packed = run({
        "replay",
        "export",
        utf8_of(tad_path),
        "--format",
        "oademo",
        "--out",
        utf8_of(oademo_path),
    });
    OA_CHECK(packed.status == oa::tool::exit_done);
    OA_CHECK(packed.out == utf8_of(oademo_path) + "\n");
    const oa::formats::oademo::ReadResult replay =
        oa::formats::oademo::read(read_bytes(oademo_path));
    OA_CHECK(replay.ok());
    if (replay.ok()) {
        OA_CHECK(replay.replay->meta.content.mutators.size() == 1);
        OA_CHECK(replay.replay->meta.content.mutators[0] == "ridge");
        OA_CHECK(replay.replay->meta.players[0].name.size() == 80);
        const oa::formats::tad::ParseResult stored =
            oa::formats::tad::parse(replay.replay->recording);
        OA_CHECK(stored.ok());
        if (stored.ok())
            OA_CHECK(
                oa::formats::tad::find_sector(
                    *stored.demo, oa::formats::tad::SectorType::player_address
                ) == nullptr
            );
    }

    const Captured again = run({
        "replay",
        "export",
        utf8_of(tad_path),
        "--format",
        "oademo",
        "--out",
        utf8_of(oademo_path),
    });
    OA_CHECK(again.status == oa::tool::exit_failed);
    OA_CHECK(again.err.find("the file already exists") != std::string::npos);

    const Captured tad = run({
        "replay",
        "export",
        utf8_of(tad_path),
        "--format",
        "tad",
        "--out",
        utf8_of(tad_out),
    });
    OA_CHECK(tad.status == oa::tool::exit_done);
    OA_CHECK(tad.err.find("Names longer than 63 letters are cut") != std::string::npos);
    OA_CHECK(tad.err.find("It was played with the mutator ridge.") != std::string::npos);
    OA_CHECK(tad.err.find("nope") == std::string::npos);
    // The parsed recording's spans view these bytes, so they have to outlive the check.
    const std::vector<uint8_t> written = read_bytes(tad_out);
    const oa::formats::tad::ParseResult parsed = oa::formats::tad::parse(written);
    OA_CHECK(parsed.ok());
    if (parsed.ok()) {
        OA_CHECK(parsed.demo->version == oa::formats::tad::supported_version);
        OA_CHECK(
            oa::formats::tad::find_sector(*parsed.demo, oa::formats::tad::SectorType::mutators) !=
            nullptr
        );
        OA_CHECK(parsed.demo->map_name.size() == oa::formats::tad::exported_name_bytes);
        OA_CHECK(!parsed.demo->packets.empty());
        if (!parsed.demo->packets.empty())
            OA_CHECK(parsed.demo->packets[0].payload[0] == oa::formats::tad::payload_marker);
    }

    const Captured forced = run({
        "replay",
        "export",
        utf8_of(tad_path),
        "--format",
        "tad",
        "--force",
        "--out",
        utf8_of(tad_out),
    });
    OA_CHECK(forced.status == oa::tool::exit_done);
    const std::vector<uint8_t> once = read_bytes(tad_out);
    const Captured forced_again = run({
        "replay",
        "export",
        utf8_of(tad_path),
        "--format",
        "tad",
        "--force",
        "--out",
        utf8_of(tad_out),
    });
    OA_CHECK(forced_again.status == oa::tool::exit_done);
    OA_CHECK(read_bytes(tad_out) == once);

    const auto bare = scratch.path / "bare.tad";
    const Captured dropped = run({
        "replay",
        "export",
        utf8_of(tad_path),
        "--format",
        "tad",
        "--no-oa-sectors",
        "--out",
        utf8_of(bare),
    });
    OA_CHECK(dropped.status == oa::tool::exit_done);
    const std::vector<uint8_t> bare_bytes = read_bytes(bare);
    const oa::formats::tad::ParseResult bare_parsed = oa::formats::tad::parse(bare_bytes);
    OA_CHECK(bare_parsed.ok());
    if (bare_parsed.ok())
        OA_CHECK(
            oa::formats::tad::find_sector(
                *bare_parsed.demo, oa::formats::tad::SectorType::mutators
            ) == nullptr
        );
}

// Pack and export of one recording describe it the same way: the lobby
// block's sides and colours, a watcher, the mutators' needs, and the first
// release that reads a replay.
void pack_and_export_describe_a_tad_the_same() {
    const Scratch scratch;
    const auto tad_path = scratch.path / "recorder.tad";
    const auto packed_path = scratch.path / "packed.oademo";
    const auto exported_path = scratch.path / "exported.oademo";
    write_bytes(tad_path, recorder_recording(true));
    OA_CHECK(
        run({"replay", "pack", utf8_of(tad_path), "--out", utf8_of(packed_path)}).status ==
        oa::tool::exit_done
    );
    OA_CHECK(
        run({
                "replay",
                "export",
                utf8_of(tad_path),
                "--format",
                "oademo",
                "--out",
                utf8_of(exported_path),
            })
            .status == oa::tool::exit_done
    );
    const oa::formats::oademo::ReadResult packed =
        oa::formats::oademo::read(read_bytes(packed_path));
    const oa::formats::oademo::ReadResult exported =
        oa::formats::oademo::read(read_bytes(exported_path));
    OA_CHECK(packed.ok());
    OA_CHECK(exported.ok());
    if (!packed.ok() || !exported.ok())
        return;
    OA_CHECK(
        oa::formats::oademo::meta_text(packed.replay->meta) ==
        oa::formats::oademo::meta_text(exported.replay->meta)
    );
    OA_CHECK(
        oa::formats::oademo::manifest_text(packed.replay->manifest) ==
        oa::formats::oademo::manifest_text(exported.replay->manifest)
    );
    OA_CHECK(packed.replay->manifest.engine.needs == ">=0.9.1");
    OA_CHECK(packed.replay->meta.players.size() == 4);
    OA_CHECK(packed.replay->meta.players[0].side == "arm");
    OA_CHECK(packed.replay->meta.players[0].colour == 3);
    OA_CHECK(packed.replay->meta.players[2].kind == "watcher");
    OA_CHECK(packed.replay->meta.players[2].side == "watcher");
    OA_CHECK(packed.replay->meta.players[2].colour == 6);
    OA_CHECK(!packed.replay->manifest.needs.empty());
    OA_CHECK(packed.replay->manifest.needs.size() > 1);
}

void level_with_deflate_is_usage() {
    const Scratch scratch;
    const auto tad_path = scratch.path / "ridge.tad";
    const auto out_path = scratch.path / "ridge.oademo";
    write_bytes(tad_path, sample_recording());
    const Captured both = run({
        "replay",
        "pack",
        utf8_of(tad_path),
        "--level",
        "3",
        "--deflate",
        "--out",
        utf8_of(out_path),
    });
    OA_CHECK(both.status == oa::tool::exit_usage);
    OA_CHECK(both.out.empty());
}

} // namespace

int main() {
    pack_info_unpack();
    sides_and_colours_come_from_the_status();
    the_recorders_text_is_utf8();
    oa_sectors_fill_the_mutators_and_the_brains();
    a_version_other_than_one_is_not_a_version_this_game_reads();
    level_with_deflate_is_usage();
    export_writes_both_formats();
    pack_and_export_describe_a_tad_the_same();
    return oa::test::check_exit_status();
}
