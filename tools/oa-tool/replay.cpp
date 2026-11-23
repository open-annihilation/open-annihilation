// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// `oa-tool replay`: what a recording holds, and wrapping one as a .oademo.
// Pack leaves out an address sector and stores every packet plain. It reads a
// player's side and colour from the player's status datagram, as playback
// does, and turns the recorder's text from the game's code page to UTF-8.
// Info and unpack read a file a piece at a time, whatever its length. Pack
// and export read a file a whole read takes as before, and one too long for
// it (oa/data/replays/long_replay.hpp) a piece at a time.

#include "arguments.hpp"
#include "command.hpp"

#include "oa/core/player_setup.h"
#include "oa/data/replays/long_replay.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/recording_reader.hpp"
#include "oa/formats/oademo/recording_walk.hpp"
#include "oa/formats/tad.hpp"
#include "oa/formats/zip.hpp"
#include "oa/present/game_text.hpp"
#include "oa/session/demo.hpp"
#include "oa/session/demo/imported_recording.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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
namespace replay_data = oa::data::replays;

constexpr uint64_t recording_cap = oa::formats::tad::limit::input_bytes;
constexpr uint64_t container_cap = oa::formats::zip::max_archive_bytes;
constexpr int highest_pack_level = 22;
/// The first bytes read to tell a `.oademo` from a `.tad`.
constexpr std::size_t first_bytes = 64;
/// The bytes a recording is unpacked through.
constexpr std::size_t piece_bytes = std::size_t{1} << 20;

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

/// Opens a file to be read a piece at a time.
///
/// @param path the file
/// @param[out] file the opened file
void open_in_pieces(const fs::path& path, replay_data::ReplayFile& file) {
    if (!file.open(path))
        throw Failure("cannot open " + utf8_of(path));
}

/// Writes a new file a piece at a time: beside its place first, then moved there.
///
/// @param path the file
/// @param force replace a file that is there
/// @param write writes the file to the path it is given; returns why it could
///        not, or empty
void write_new_in_pieces(
    const fs::path& path, bool force, const std::function<std::string(const fs::path&)>& write
) {
    std::error_code exists_error;
    if (fs::exists(path, exists_error) && !force)
        throw Failure("the file already exists");
    fs::path partial = path;
    partial += ".part";
    std::string why_not = write(partial);
    std::error_code moved;
    if (why_not.empty())
        fs::rename(partial, path, moved);
    if (!why_not.empty() || moved) {
        std::error_code removed;
        fs::remove(partial, removed);
        throw Failure(why_not.empty() ? "cannot write " + utf8_of(path) : why_not);
    }
}

/// Says why a walk of a recording stopped, as a whole read and parse of it say.
///
/// @param failure the walk's failure
/// @param path the file, for a read that failed
/// @return the sentence or the parse's words
std::string walk_failure(const oa::formats::oademo::WalkFailure& failure, const fs::path& path) {
    if (!failure.sentence.empty())
        return failure.sentence;
    if (failure.parse.has_value()) {
        if (failure.parse->code == oa::formats::tad::ErrorCode::bad_magic)
            return std::string(oa::formats::oademo::not_a_replay);
        return failure.parse->message;
    }
    return "cannot read " + utf8_of(path);
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

/// Prints what a `.tad` holds.
///
/// @param demo the part before its packets
/// @param packets how many packets it holds
/// @param length the recorder's clock at its last packet; 0 with none
/// @param as_json one JSON object rather than lines
/// @param output where it goes
void print_tad(
    const oa::formats::tad::Demo& demo,
    uint64_t packets,
    uint64_t length,
    bool as_json,
    Output& output
) {
    const oa::formats::oademo::UnitTableDigest digest =
        oa::formats::oademo::unit_table_digest(demo.unit_checks);
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
        writer.integer(static_cast<int64_t>(packets));
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
    output.out << "packets: " << packets << '\n';
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

/// Prints the warnings a `.tad` export gives.
///
/// @param notices the export's notices
/// @param output where they go
void warn_tad_export(std::span<const oa::formats::tad::ExportNotice> notices, Output& output) {
    for (const oa::formats::tad::ExportNotice& notice : notices) {
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

/// Describes a long `.tad` as pack describes one, from the part before its packets.
///
/// @param source the `.tad`
/// @return the description; its recording identity, packets and length are
///         set when it is written (replay_data::write_replay_in_pieces)
oa::formats::oademo::Replay long_recording_description(const fs::path& source) {
    replay_data::ReplayFile file;
    open_in_pieces(source, file);
    oa::formats::oademo::RecordingWalk walk;
    if (!walk.open_tad(file.recording()))
        throw Failure(walk_failure(walk.failure(), source));
    const std::vector<uint8_t> head(walk.head_bytes().begin(), walk.head_bytes().end());
    oa::formats::oademo::Replay replay = replay_from_recording(head);
    replay.recording.clear();
    return replay;
}

/// Writes a `.oademo` of a long recording a piece at a time.
///
/// @param source the recording or replay
/// @param destination the replay to make
/// @param replay its description
/// @param level the zstd level
/// @param force replace a file that is there
void write_long_replay(
    const fs::path& source,
    const fs::path& destination,
    const oa::formats::oademo::Replay& replay,
    int level,
    bool force
) {
    write_new_in_pieces(destination, force, [&](const fs::path& part) {
        replay_data::ReplayOutput out;
        if (!out.open(part))
            return "cannot write " + utf8_of(destination);
        fs::path frame = part;
        frame += ".frame";
        std::string why_not =
            replay_data::write_replay_in_pieces(source, replay, level, frame, out.archive());
        if (!out.close() && why_not.empty())
            why_not = "cannot write " + utf8_of(destination);
        return why_not;
    });
}

/// Packs a `.tad` past 512 MiB as a `.oademo`, a piece at a time, at `level`.
///
/// @param source the `.tad`
/// @param destination the replay to make
/// @param level the zstd level
/// @param force replace a file that is there
void pack_long(const fs::path& source, const fs::path& destination, int level, bool force) {
    write_long_replay(source, destination, long_recording_description(source), level, force);
}

/// Exports a replay or recording too long to read whole, a piece at a time.
///
/// A `.oademo` keeps its own frame when its recording is plain and holds no
/// address; a `.tad` is wrapped at the level a game ends at, since gigabytes
/// of recording take far too long at the export's level. A `.tad` export is
/// written as a whole one is, with the same warnings.
///
/// @param source the replay or recording
/// @param destination the file to make
/// @param as_oademo a `.oademo`, not a `.tad`
/// @param keep_oa_sectors a `.tad` keeps Open Annihilation's sectors
/// @param force replace a file that is there
/// @param output where the warnings go
void export_long(
    const fs::path& source,
    const fs::path& destination,
    bool as_oademo,
    bool keep_oa_sectors,
    bool force,
    Output& output
) {
    replay_data::ReplayFile file;
    open_in_pieces(source, file);
    const bool container = oa::formats::oademo::recognised(file.first_bytes(first_bytes));
    if (as_oademo && !container) {
        const oa::formats::oademo::Replay replay = long_recording_description(source);
        write_long_replay(
            source, destination, replay, oa::formats::oademo::level_when_a_game_ends, force
        );
        return;
    }
    if (as_oademo) {
        write_new_in_pieces(destination, force, [&](const fs::path& part) {
            replay_data::ReplayOutput out;
            if (!out.open(part))
                return "cannot write " + utf8_of(destination);
            fs::path frame = part;
            frame += ".frame";
            std::string why_not =
                replay_data::export_long_replay(source, std::nullopt, frame, out.archive());
            if (!out.close() && why_not.empty())
                why_not = "cannot write " + utf8_of(destination);
            return why_not;
        });
        return;
    }
    oa::formats::tad::ExportOptions cleaning;
    cleaning.keep_oa_sectors = keep_oa_sectors;
    cleaning.fit_tadr_sizes = true;
    std::vector<oa::formats::tad::ExportNotice> notices;
    write_new_in_pieces(destination, force, [&](const fs::path& part) {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        const replay_data::ExportSink sink{&out, [](void* context, std::span<const uint8_t> bytes) {
                                               auto& to = *static_cast<std::ofstream*>(context);
                                               to.write(
                                                   reinterpret_cast<const char*>(bytes.data()),
                                                   static_cast<std::streamsize>(bytes.size())
                                               );
                                               return static_cast<bool>(to);
                                           }};
        const replay_data::CleanedRecording cleaned =
            replay_data::write_cleaned_recording(source, cleaning, sink);
        out.close();
        if (!cleaned.ok())
            return cleaned.error;
        notices = cleaned.notices;
        return out.fail() ? "cannot write " + utf8_of(destination) : std::string();
    });
    warn_tad_export(notices, output);
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
    const bool as_json = parsed->flags.contains("json");
    // Read a piece at a time, whatever the file's length: a replay is checked
    // to its last byte, as a whole read checks it, and a `.tad` walked packet
    // by packet, as a whole parse walks it.
    replay_data::ReplayFile file;
    open_in_pieces(path, file);
    if (oa::formats::oademo::recognised(file.first_bytes(first_bytes))) {
        oa::formats::oademo::RecordingReader reader;
        if (std::string why_not = reader.open(file.archive()); !why_not.empty())
            throw Failure(why_not);
        oa::formats::oademo::Replay replay;
        replay.meta = reader.meta();
        replay.manifest = reader.manifest();
        if (std::string why_not = reader.read_timeline(replay.timeline); !why_not.empty())
            throw Failure(why_not);
        std::vector<uint8_t> piece(piece_bytes);
        for (;;) {
            std::size_t got = 0;
            if (std::string why_not = reader.read(piece, got); !why_not.empty())
                throw Failure(why_not);
            if (got == 0)
                break;
        }
        print_oademo(replay, as_json, output);
        return exit_done;
    }
    oa::formats::oademo::RecordingWalk walk;
    if (!walk.open_tad(file.recording()))
        throw Failure(walk_failure(walk.failure(), path));
    oa::formats::tad::Packet packet;
    uint64_t length = 0;
    for (;;) {
        bool ended = false;
        if (!walk.next(packet, ended))
            throw Failure(walk_failure(walk.failure(), path));
        if (ended)
            break;
        length = packet.time_ms;
    }
    print_tad(walk.head(), walk.packets(), length, as_json, output);
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
    if (replay_data::long_replay(source)) {
        if (write_options.deflate)
            throw Failure("a recording past 512 MiB is packed as a zstd frame, not deflated");
        pack_long(source, destination, write_options.level, parsed->flags.contains("force"));
        output.out << utf8_of(destination) << '\n';
        return exit_done;
    }
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
    // The recording is copied out a piece at a time and checked to its last
    // byte, whatever its length; a replay that does not read leaves no file.
    replay_data::ReplayFile file;
    open_in_pieces(source, file);
    oa::formats::oademo::RecordingReader reader;
    if (std::string why_not = reader.open(file.archive()); !why_not.empty())
        throw Failure(why_not);
    std::optional<std::string> timeline;
    if (std::string why_not = reader.read_timeline(timeline); !why_not.empty())
        throw Failure(why_not);
    write_new_in_pieces(destination, parsed->flags.contains("force"), [&](const fs::path& part) {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        std::vector<uint8_t> piece(piece_bytes);
        for (;;) {
            std::size_t got = 0;
            if (std::string why_not = reader.read(piece, got); !why_not.empty())
                return why_not;
            if (got == 0)
                break;
            out.write(
                reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(got)
            );
        }
        out.close();
        return out.fail() ? "cannot write " + utf8_of(destination) : std::string();
    });
    output.out << utf8_of(destination) << '\n';
    return exit_done;
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
    if (replay_data::long_replay(source)) {
        export_long(
            source,
            destination,
            as_oademo,
            !parsed->flags.contains("no-oa-sectors"),
            parsed->flags.contains("force"),
            output
        );
        output.out << utf8_of(destination) << '\n';
        return exit_done;
    }
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
        warn_tad_export(exported.notices, output);
        written = exported.bytes;
    }
    write_new(destination, written, parsed->flags.contains("force"));
    output.out << utf8_of(destination) << '\n';
    return exit_done;
}

} // namespace oa::tool
