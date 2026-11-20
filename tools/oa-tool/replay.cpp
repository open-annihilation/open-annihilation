// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// `oa-tool replay`: what a recording holds, and wrapping one as a .oademo.
// Pack leaves out an address sector and stores every packet plain. It reads a
// player's side and colour from the player's status datagram, as playback
// does, and turns the recorder's text from the game's code page to UTF-8.

#include "arguments.hpp"
#include "command.hpp"

#include "oa/core/player_setup.h"
#include "oa/formats/json.hpp"
#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"
#include "oa/formats/zip.hpp"
#include "oa/present/game_text.hpp"
#include "oa/session/demo.hpp"
#include "oa/session/demo/imported_recording.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t recording_cap = oa::formats::tad::limit::input_bytes;
constexpr uint64_t container_cap = oa::formats::zip::max_archive_bytes;
constexpr int highest_pack_level = 22;

std::string utf8_of(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path path_from(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

int usage(Output& output, std::string_view name, std::string_view problem) {
    output.err << "oa-tool replay " << name << ": " << problem << '\n';
    output.err << "run 'oa-tool help replay " << name << "'\n";
    return exit_usage;
}

std::vector<uint8_t> read_up_to(const fs::path& path, uint64_t cap) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Failure("cannot open " + utf8_of(path));
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (!in || end < 0)
        throw Failure("cannot read " + utf8_of(path));
    const auto size = static_cast<uint64_t>(end);
    if (size > cap)
        throw Failure("the file is too long to read");
    in.seekg(0);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!in)
        throw Failure("cannot read " + utf8_of(path));
    return bytes;
}

void write_new(const fs::path& path, std::span<const uint8_t> bytes, bool force) {
    std::error_code exists_error;
    if (fs::exists(path, exists_error) && !force)
        throw Failure("the file already exists");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw Failure("cannot write " + utf8_of(path));
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    if (!out)
        throw Failure("cannot write " + utf8_of(path));
}

std::string swapped_suffix(const fs::path& path, std::string_view from, std::string_view to) {
    std::string name = utf8_of(path);
    bool match = name.size() >= from.size();
    if (match) {
        for (std::size_t index = 0; index < from.size(); ++index) {
            char left = name[name.size() - from.size() + index];
            char right = from[index];
            if (left >= 'A' && left <= 'Z')
                left = static_cast<char>(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z')
                right = static_cast<char>(right - 'A' + 'a');
            if (left != right) {
                match = false;
                break;
            }
        }
    }
    if (match)
        name.resize(name.size() - from.size());
    name.append(to);
    return name;
}

bool parse_level(std::string_view text, int& level) {
    if (text.empty() || text.size() > 2 || text[0] == '0')
        return false;
    int value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9')
            return false;
        value = value * 10 + (character - '0');
    }
    if (value < 1 || value > highest_pack_level)
        return false;
    level = value;
    return true;
}

/// The recording's text as UTF-8. The recorder, the date, the map and the
/// players' names are bytes of the game's code page (Windows-1252).
std::string utf8_text(std::string_view bytes) {
    return oa::present::decode_game_text(bytes, false);
}

std::string side_name(uint8_t side) {
    if (side == static_cast<uint8_t>(oa::formats::tad::RecordedSide::arm))
        return "arm";
    if (side == static_cast<uint8_t>(oa::formats::tad::RecordedSide::core))
        return "core";
    return std::to_string(side);
}

/// What a player is, as the description lists them.
struct PlayerFacts {
    std::string name{};
    std::string side{};
    int colour{};
    std::string kind{};
};

/// Reads a player as playback binds it: the side and colour of the player's
/// status datagram, and only without one the bytes of the player's chunk.
PlayerFacts
player_facts(const oa::formats::tad::Demo& demo, const oa::formats::tad::Player& player) {
    const oa::session::demo::RecordedPlayer recorded =
        oa::session::demo::demo_recorded_player(demo, player);
    PlayerFacts facts;
    facts.name = utf8_text(player.name);
    facts.colour = recorded.info.color;
    const bool watcher =
        recorded.from_status && (recorded.info.options & OA_SETUP_OPTION_WATCHER) != 0;
    facts.side = watcher ? "watcher" : side_name(recorded.info.side);
    facts.kind = watcher ? "watcher" : "human";
    return facts;
}

oa::formats::oademo::Replay replay_from_recording(const std::vector<uint8_t>& bytes) {
    const oa::session::demo::ImportedRecording described =
        oa::session::demo::describe_imported_recording(bytes, OA_ENGINE_VERSION);
    if (!described.ok())
        throw Failure(described.error);
    return described.replay;
}

void print_tad(const oa::formats::tad::Demo& demo, bool as_json, Output& output) {
    const oa::formats::oademo::UnitTableDigest digest =
        oa::formats::oademo::unit_table_digest(demo.unit_checks);
    const uint64_t length = demo.packets.empty() ? 0 : demo.packets.back().time_ms;
    std::string recorder;
    std::string date;
    bool has_recorder = false;
    bool has_date = false;
    if (const oa::formats::tad::Sector* sector =
            oa::formats::tad::find_sector(demo, oa::formats::tad::SectorType::recorder_version)) {
        recorder = utf8_text(oa::formats::tad::sector_text(*sector));
        has_recorder = true;
    }
    if (const oa::formats::tad::Sector* sector =
            oa::formats::tad::find_sector(demo, oa::formats::tad::SectorType::recording_date)) {
        date = utf8_text(oa::formats::tad::sector_text(*sector));
        has_date = true;
    }
    if (as_json) {
        oa::formats::json::JsonWriter writer;
        writer.begin_object();
        writer.key("file");
        writer.string("tad");
        writer.key("version");
        writer.integer(demo.version);
        writer.key("map");
        writer.string(utf8_text(demo.map_name));
        writer.key("players");
        writer.begin_array();
        for (const oa::formats::tad::Player& player : demo.players) {
            const PlayerFacts facts = player_facts(demo, player);
            writer.begin_object();
            writer.key("number");
            writer.integer(player.number);
            writer.key("name");
            writer.string(facts.name);
            writer.key("side");
            writer.string(facts.side);
            writer.key("colour");
            writer.integer(facts.colour);
            writer.end_object();
        }
        writer.end_array();
        writer.key("packets");
        writer.integer(static_cast<int64_t>(demo.packets.size()));
        writer.key("length_ms");
        writer.integer(static_cast<int64_t>(length));
        writer.key("units");
        writer.begin_object();
        writer.key("count");
        writer.integer(digest.count);
        writer.key("table_sha256");
        writer.string(digest.sha256);
        writer.key("index_bits");
        writer.integer(static_cast<int64_t>(digest.index_bits));
        writer.end_object();
        if (has_recorder) {
            writer.key("recorder");
            writer.string(recorder);
        }
        if (has_date) {
            writer.key("date");
            writer.string(date);
        }
        writer.end_object();
        output.out << writer.text() << '\n';
        return;
    }
    output.out << "file: tad\n";
    output.out << "version: " << demo.version << '\n';
    output.out << "map: " << utf8_text(demo.map_name) << '\n';
    output.out << "players: " << demo.players.size() << '\n';
    for (const oa::formats::tad::Player& player : demo.players) {
        const PlayerFacts facts = player_facts(demo, player);
        output.out << "player " << static_cast<int>(player.number) << ": " << facts.name << " "
                   << facts.side << " " << facts.colour << '\n';
    }
    output.out << "packets: " << demo.packets.size() << '\n';
    output.out << "length_ms: " << length << '\n';
    output.out << "units: " << digest.count << " " << digest.sha256 << '\n';
    output.out << "index_bits: " << digest.index_bits << '\n';
    if (has_recorder)
        output.out << "recorder: " << recorder << '\n';
    if (has_date)
        output.out << "date: " << date << '\n';
}

void print_oademo(const oa::formats::oademo::Replay& replay, bool as_json, Output& output) {
    if (as_json) {
        oa::formats::json::JsonWriter writer;
        writer.begin_object();
        writer.key("file");
        writer.string("oademo");
        writer.key("meta");
        writer.raw(oa::formats::oademo::meta_text(replay.meta));
        writer.key("manifest");
        writer.raw(oa::formats::oademo::manifest_text(replay.manifest));
        if (replay.timeline.has_value()) {
            writer.key("timeline");
            writer.raw(*replay.timeline);
        }
        writer.end_object();
        output.out << writer.text() << '\n';
        return;
    }
    output.out << "file: oademo\n";
    output.out << "map: " << replay.meta.map.name << '\n';
    output.out << "kind: " << replay.meta.game.kind << '\n';
    output.out << "how: " << replay.meta.recorded.how << '\n';
    output.out << "players: " << replay.meta.players.size() << '\n';
    output.out << "packets: " << replay.meta.recording.packets << '\n';
    if (replay.meta.game.length_ms.has_value())
        output.out << "length_ms: " << *replay.meta.game.length_ms << '\n';
    output.out << "sha256: " << replay.meta.recording.sha256 << '\n';
    output.out << "bytes: " << replay.meta.recording.bytes << '\n';
    output.out << "engine: " << replay.manifest.engine.recorded_with << " "
               << replay.manifest.engine.needs << '\n';
    if (replay.timeline.has_value())
        output.out << "timeline: yes\n";
}

} // namespace

int run_replay_info(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {{"json", false, false}};
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed)
        return usage(output, "info", problem);
    if (parsed->positional.size() != 1)
        return usage(output, "info", "give one file");
    const fs::path path = path_from(parsed->positional[0]);
    const std::vector<uint8_t> bytes = read_up_to(path, container_cap);
    const bool as_json = parsed->flags.contains("json");
    if (oa::formats::oademo::recognised(bytes)) {
        const oa::formats::oademo::ReadResult replay = oa::formats::oademo::read(bytes);
        if (!replay.ok())
            throw Failure(replay.error);
        print_oademo(*replay.replay, as_json, output);
        return exit_done;
    }
    if (bytes.size() > recording_cap)
        throw Failure("the file is too long to read");
    const oa::formats::tad::ParseResult parsed_tad = oa::formats::tad::parse(bytes);
    if (!parsed_tad.ok()) {
        if (parsed_tad.error->code == oa::formats::tad::ErrorCode::bad_magic)
            throw Failure(std::string(oa::formats::oademo::not_a_replay));
        throw Failure(parsed_tad.error->message);
    }
    print_tad(*parsed_tad.demo, as_json, output);
    return exit_done;
}

int run_replay_pack(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"level", true, false},
        {"deflate", false, false},
        {"force", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed)
        return usage(output, "pack", problem);
    if (parsed->positional.size() != 1)
        return usage(output, "pack", "give one recording");
    if (parsed->flags.contains("deflate") && parsed->value("level") != nullptr)
        return usage(output, "pack", "give --level or --deflate, not both");
    oa::formats::oademo::WriteOptions write_options;
    if (parsed->flags.contains("deflate"))
        write_options.deflate = true;
    if (const std::string* level = parsed->value("level")) {
        if (!parse_level(*level, write_options.level))
            return usage(output, "pack", "the level is not from 1 to 22");
    }
    const fs::path source = path_from(parsed->positional[0]);
    const fs::path destination = parsed->value("out") != nullptr
                                     ? path_from(*parsed->value("out"))
                                     : path_from(swapped_suffix(source, ".tad", ".oademo"));
    const std::vector<uint8_t> bytes = read_up_to(source, recording_cap);
    const oa::formats::oademo::Replay replay = replay_from_recording(bytes);
    const oa::formats::oademo::WriteResult written =
        oa::formats::oademo::write(replay, write_options);
    if (!written.ok())
        throw Failure(written.error);
    write_new(destination, written.bytes, parsed->flags.contains("force"));
    output.out << utf8_of(destination) << '\n';
    return exit_done;
}

int run_replay_unpack(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"force", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed)
        return usage(output, "unpack", problem);
    if (parsed->positional.size() != 1)
        return usage(output, "unpack", "give one replay");
    const fs::path source = path_from(parsed->positional[0]);
    const fs::path destination = parsed->value("out") != nullptr
                                     ? path_from(*parsed->value("out"))
                                     : path_from(swapped_suffix(source, ".oademo", ".tad"));
    const std::vector<uint8_t> bytes = read_up_to(source, container_cap);
    const oa::formats::oademo::ReadResult replay = oa::formats::oademo::read(bytes);
    if (!replay.ok())
        throw Failure(replay.error);
    write_new(destination, replay.replay->recording, parsed->flags.contains("force"));
    output.out << utf8_of(destination) << '\n';
    return exit_done;
}

void warn_tad_export(const oa::formats::tad::ExportResult& exported, Output& output) {
    for (const oa::formats::tad::ExportNotice& notice : exported.notices) {
        switch (notice.kind) {
        case oa::formats::tad::ExportNoticeKind::names_cut:
            output.err << "Names longer than 63 letters are cut.\n";
            break;
        case oa::formats::tad::ExportNoticeKind::status_cut:
            output.err << "A player's status is cut to 512 bytes.\n";
            break;
        case oa::formats::tad::ExportNoticeKind::wide_records:
            output.err << "Other replayers stop at the first of this mod's wide weapon records.\n";
            break;
        case oa::formats::tad::ExportNoticeKind::mutators:
            if (notice.mutators.empty()) {
                output.err << "It was played with mutators. Other replayers cannot apply them, "
                              "so commanders may not last as they did.\n";
                break;
            }
            for (const std::string& name : notice.mutators) {
                output.err << "It was played with the mutator " << name
                           << ". Other replayers cannot apply it, so commanders may not last as "
                              "they did.\n";
            }
            break;
        }
    }
}

int run_replay_export(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"format", true, false},
        {"no-oa-sectors", false, false},
        {"out", true, false},
        {"force", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed)
        return usage(output, "export", problem);
    if (parsed->positional.size() != 1)
        return usage(output, "export", "give one recording");
    const std::string* format = parsed->value("format");
    const std::string* out = parsed->value("out");
    if (format == nullptr)
        return usage(output, "export", "give --format oademo or --format tad");
    if (*format != "oademo" && *format != "tad")
        return usage(output, "export", "give --format oademo or --format tad");
    if (out == nullptr)
        return usage(output, "export", "give --out");
    const bool as_oademo = *format == "oademo";
    if (parsed->flags.contains("no-oa-sectors") && as_oademo)
        return usage(output, "export", "--no-oa-sectors applies to a .tad export");

    const fs::path source = path_from(parsed->positional[0]);
    const fs::path destination = path_from(*out);
    const std::vector<uint8_t> bytes = read_up_to(source, container_cap);
    const bool container = oa::formats::oademo::recognised(bytes);
    std::vector<uint8_t> written;
    if (as_oademo) {
        oa::formats::oademo::RecordingExport wrapping;
        wrapping.recorded_with = OA_ENGINE_VERSION;
        oa::formats::oademo::WriteResult archive;
        if (container) {
            const oa::formats::oademo::ReadResult replay = oa::formats::oademo::read(bytes);
            if (!replay.ok())
                throw Failure(replay.error);
            archive = oa::formats::oademo::export_replay(*replay.replay, wrapping);
        } else {
            if (bytes.size() > recording_cap)
                throw Failure("the file is too long to read");
            const oa::session::demo::ImportedRecording described =
                oa::session::demo::describe_imported_recording(bytes, OA_ENGINE_VERSION);
            if (!described.ok())
                throw Failure(described.error);
            oa::formats::oademo::WriteOptions writing;
            writing.level = oa::formats::oademo::level_on_export;
            archive = oa::formats::oademo::write(described.replay, writing);
        }
        if (!archive.ok())
            throw Failure(archive.error);
        written = std::move(archive.bytes);
    } else {
        oa::formats::tad::ExportOptions cleaning;
        cleaning.keep_oa_sectors = !parsed->flags.contains("no-oa-sectors");
        cleaning.fit_tadr_sizes = true;
        std::span<const uint8_t> recording = bytes;
        std::vector<uint8_t> inner;
        if (container) {
            const oa::formats::oademo::ReadResult replay = oa::formats::oademo::read(bytes);
            if (!replay.ok())
                throw Failure(replay.error);
            inner = replay.replay->recording;
            recording = inner;
        } else if (bytes.size() > recording_cap) {
            throw Failure("the file is too long to read");
        }
        const oa::formats::tad::ExportResult exported =
            oa::formats::tad::export_recording(recording, cleaning);
        if (!exported.ok())
            throw Failure(exported.error->message);
        warn_tad_export(exported, output);
        written = exported.bytes;
    }
    write_new(destination, written, parsed->flags.contains("force"));
    output.out << utf8_of(destination) << '\n';
    return exit_done;
}

} // namespace oa::tool
