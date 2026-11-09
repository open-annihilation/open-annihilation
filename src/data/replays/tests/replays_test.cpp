// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A scratch Recordings folder of made-up replays. A scan lists only the
// replays, not the games being recorded, the hidden folders, the content
// notes, a link or what lies too deep, and reads each one's description or
// header. The index reads back as written, a second scan takes unchanged
// files from it without opening them, and a deleted, damaged or junk index is
// rebuilt from the folder. The replays the player keeps are marked in a file
// of their own by recording, so no change to the index forgets one, and a
// damaged index is set aside rather than overwritten. A .tad's names in
// Windows-1252 come out as UTF-8. Damaged files are listed, damaged, with
// their sentences. Kinds register by id, and a list groups by day. While a
// damaged kept file is set aside, the replays recorded before it stay. An
// exported mark keeps a replay past the span, and a file that is not those
// marks is damaged. A read of it leaves the bytes.

#include "oa/data/replays/import.hpp"
#include "oa/data/replays/interface_words.hpp"
#include "oa/data/replays/replays.hpp"

#include "oa/base/sha256.hpp"
#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/writer.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace lib = oa::data::replays;
namespace oademo = oa::formats::oademo;
namespace tad = oa::formats::tad;
namespace zip = oa::formats::zip;

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-data-replays-")) {}

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

  private:

    fs::path path_;
};

/// Writes a file's bytes, making its folder.
///
/// @param file the file
/// @param bytes its bytes
void write_bytes(const fs::path& file, std::span<const uint8_t> bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

/// Writes a file's text, making its folder.
///
/// @param file the file
/// @param text its text
void write_text(const fs::path& file, std::string_view text) {
    write_bytes(file, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

/// Reads a whole file.
///
/// @param file the file
/// @return its bytes
std::vector<uint8_t> file_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Returns the SHA-256 of some bytes, as lower-case hexadecimal.
///
/// @param bytes the bytes
/// @return the digest
std::string sha256_of(std::span<const uint8_t> bytes) {
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::digest_of(bytes));
    return {hex.begin(), hex.end()};
}

/// Returns the seconds since 1970 now.
///
/// @return the system clock's time, in seconds
int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

/// A small TA Demo recording: a header, two players and their statuses, an
/// empty unit-check chunk and three packets, 1,500 ms in all.
///
/// @param map the map's name as recorded
/// @param first_name the first player's name as recorded
/// @param recorder the recorder-version sector's text as recorded
/// @return the file's bytes
std::vector<uint8_t> tad_recording(std::string map, std::string first_name, std::string recorder) {
    const std::vector<uint8_t> recorder_bytes(recorder.begin(), recorder.end());
    const std::vector<uint8_t> status{0x04};
    const std::vector<uint8_t> payload{tad::payload_marker, 0xff};
    tad::Demo demo;
    demo.version = tad::supported_version;
    demo.max_units = 250;
    demo.map_name = std::move(map);
    demo.sectors = {
        {static_cast<uint32_t>(tad::SectorType::recorder_version), recorder_bytes},
    };
    demo.players = {{2, 0, 1, std::move(first_name)}, {5, 1, 2, "Screamer"}};
    demo.statuses = {{1, status}, {2, status}};
    demo.packets = {
        {0, 40, 40, 1, payload},
        {0, 60, 100, 2, payload},
        {0, 1400, 1500, 1, payload},
    };
    const tad::WriteResult written = tad::write(demo);
    OA_CHECK(written.ok());
    return written.bytes;
}

/// A replay of a small recording, with every member the Library reads.
///
/// @param map the map's name
/// @return the replay, ready to write
oademo::Replay sample_replay(const std::string& map) {
    oademo::Replay replay;
    replay.recording = tad_recording(map, "Pilot", "oa-test 1");
    oademo::set_recording_identity(replay);
    replay.meta.recording.tad_version = 5;
    replay.meta.recording.packets = 3;
    replay.meta.recorded.how = "multiplayer";
    replay.meta.recorded.at = "2026-11-04T14:32:00Z";
    replay.meta.recorded.local = "2026-11-04 14:32";
    replay.meta.recorded.by = "Open Annihilation 0.9.0";
    replay.meta.game.kind = "multiplayer";
    replay.meta.game.length_ms = 1500;
    replay.meta.game.ended = "finished";
    replay.meta.map.name = map;
    replay.meta.content.base.id = "totala";
    replay.meta.content.base.version = "3.1c";
    replay.meta.units.count = 1;
    replay.meta.units.table_sha256 = std::string(64, 'a');
    replay.meta.units.index_bits = 1;
    oademo::PlayerInfo first;
    first.number = 1;
    first.name = "Pilot";
    first.side = "arm";
    first.colour = 2;
    first.team = 1;
    first.kind = "human";
    first.recorded_here = true;
    oademo::PlayerInfo second;
    second.number = 2;
    second.name = "Screamer";
    second.side = "core";
    second.colour = 5;
    second.kind = "computer";
    replay.meta.players = {first, second};
    replay.manifest.engine.recorded_with = "0.9.0";
    replay.manifest.engine.needs = ">=0.9.0";
    oademo::Need base;
    base.kind = "base";
    base.id = "totala";
    base.version = "3.1c";
    replay.manifest.needs.push_back(base);
    return replay;
}

/// Writes a replay's bytes.
///
/// @param replay the replay
/// @return the `.oademo`'s bytes
std::vector<uint8_t> oademo_bytes(const oademo::Replay& replay) {
    const oademo::WriteResult written = oademo::write(replay, {});
    OA_CHECK(written.ok());
    return written.bytes;
}

/// The replay of an outpost battle, after a crash, with a mod.
///
/// @return the replay
oademo::Replay outpost_replay() {
    oademo::Replay replay = sample_replay("Show Down");
    replay.meta.game.kind = "outpost";
    replay.meta.game.kind_object = "{\"planet\":\"Corvane\",\"sector\":\"Low Reach\",\"war\":7}";
    replay.meta.recorded.after_crash = true;
    replay.meta.game.ended = "cut";
    oademo::ModRef mod;
    mod.id = "fixture-mod";
    mod.name = "Fixture Mod";
    mod.version = "1.2";
    mod.revision = 3;
    replay.meta.content.mod = mod;
    replay.meta.content.expansions = {"expansion-one"};
    replay.meta.content.mutators = {"double-metal", "fast-build"};
    return replay;
}

/// The replay of a skirmish whose description names no instant.
///
/// @return the replay
oademo::Replay skirmish_replay() {
    oademo::Replay replay = sample_replay("Dune Wars");
    replay.meta.game.kind = "skirmish";
    replay.meta.recorded.how = "skirmish";
    replay.meta.recorded.at.reset();
    replay.meta.game.length_ms.reset();
    return replay;
}

struct MemoryOut {
    std::vector<uint8_t> bytes{};
};

/// Writes zip bytes into memory.
///
/// @param context the MemoryOut
/// @param offset where the bytes go
/// @param bytes the bytes
/// @return true when they were written
bool write_memory(void* context, uint64_t offset, std::span<const uint8_t> bytes) {
    auto& out = *static_cast<MemoryOut*>(context);
    if (offset > out.bytes.size())
        return false;
    const std::size_t at = static_cast<std::size_t>(offset);
    if (out.bytes.size() < at + bytes.size())
        out.bytes.resize(at + bytes.size());
    for (std::size_t offset_in = 0; offset_in < bytes.size(); ++offset_in)
        out.bytes[at + offset_in] = bytes[offset_in];
    return true;
}

/// A zip that holds one note and is no replay.
///
/// @return the zip's bytes
std::vector<uint8_t> plain_zip() {
    MemoryOut out;
    zip::WriterOptions options;
    zip::StreamWriter writer({&out, write_memory}, options);
    zip::ZipError problem;
    const std::string_view note = "a note\n";
    const std::span<const uint8_t> note_bytes{
        reinterpret_cast<const uint8_t*>(note.data()), note.size()
    };
    OA_CHECK(
        writer.begin_entry("readme.txt", note_bytes.size(), zip::Method::stored, problem) &&
        writer.write(note_bytes, problem) && writer.end_entry(problem) && writer.finish(problem)
    );
    return out.bytes;
}

/// Tells whether two players are the same in every member.
///
/// @param left a player
/// @param right another
/// @return true when every member is equal
bool same_player(const lib::Player& left, const lib::Player& right) {
    return left.name == right.name && left.colour == right.colour && left.side == right.side &&
           left.team == right.team && left.kind == right.kind &&
           left.recorded_here == right.recorded_here && left.number == right.number;
}

/// Tells whether two replays are the same in every member.
///
/// @param left a replay
/// @param right another
/// @return true when every member is equal
bool same_replay(const lib::Replay& left, const lib::Replay& right) {
    if (left.players.size() != right.players.size())
        return false;
    for (std::size_t at = 0; at < left.players.size(); ++at)
        if (!same_player(left.players[at], right.players[at]))
            return false;
    return left.path == right.path && left.size == right.size && left.modified == right.modified &&
           left.file == right.file && left.damage == right.damage && left.sha256 == right.sha256 &&
           left.map == right.map && left.kind == right.kind && left.how == right.how &&
           left.recorded == right.recorded && left.recorded_by == right.recorded_by &&
           left.length_ms == right.length_ms && left.ended == right.ended &&
           left.after_crash == right.after_crash && left.mod_id == right.mod_id &&
           left.mod_name == right.mod_name && left.mod_version == right.mod_version &&
           left.mod_revision == right.mod_revision && left.expansions == right.expansions &&
           left.mutators == right.mutators && left.planet == right.planet &&
           left.sector == right.sector && left.mission_name == right.mission_name &&
           left.timeline == right.timeline && left.watched_tick == right.watched_tick &&
           left.read_stamp == right.read_stamp && left.kept == right.kept;
}

/// Tells whether two indexes hold the same replays in the same order.
///
/// @param left an index
/// @param right another
/// @return true when every replay is the same
bool same_index(const lib::Index& left, const lib::Index& right) {
    if (left.replays.size() != right.replays.size())
        return false;
    for (std::size_t at = 0; at < left.replays.size(); ++at)
        if (!same_replay(left.replays[at], right.replays[at]))
            return false;
    return true;
}

/// What a scan told its hooks.
struct Seen {
    std::vector<std::string> paths{};
    std::vector<std::size_t> done{};
    std::vector<std::size_t> totals{};
    std::size_t stop_at{std::numeric_limits<std::size_t>::max()};
};

/// Notes one replay a scan finished.
///
/// @param context the Seen
/// @param replay the replay
/// @param done how many are done
/// @param total how many were listed
void note_replay(void* context, const lib::Replay& replay, std::size_t done, std::size_t total) {
    Seen& seen = *static_cast<Seen*>(context);
    seen.paths.push_back(replay.path);
    seen.done.push_back(done);
    seen.totals.push_back(total);
}

/// Tells a scan to stop once enough replays are done.
///
/// @param context the Seen
/// @return true when stop_at replays are done
bool enough_seen(void* context) {
    const Seen& seen = *static_cast<const Seen*>(context);
    return seen.done.size() >= seen.stop_at;
}

/// Fills a Recordings folder with the three replays every case starts from.
///
/// @param recordings the folder
void three_replays(const fs::path& recordings) {
    write_bytes(recordings / "default" / "a.oademo", oademo_bytes(outpost_replay()));
    write_bytes(recordings / "ridge" / "b.tad", tad_recording("Ridge One", "Pilot", "oa-test 1"));
    write_bytes(recordings / "Imported" / "c.oademo", oademo_bytes(skirmish_replay()));
}

void a_scan_lists_and_reads_only_the_replays(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "scan" / "Recordings";
    three_replays(recordings);
    write_bytes(
        recordings / ".replays" / "partial" / "default" / "x.oademo.part",
        tad_recording("Show Down", "Pilot", "oa-test 1")
    );
    write_bytes(recordings / ".hidden" / "d.oademo", oademo_bytes(outpost_replay()));
    write_text(recordings / "default" / "a.oademo.oa-content", "content note\n");
    write_text(recordings / "default" / "notes.txt", "notes\n");
    write_bytes(
        recordings / "deep" / "one" / "two" / "three" / "e.oademo", oademo_bytes(outpost_replay())
    );
    std::error_code link_error;
    fs::create_symlink("a.oademo", recordings / "default" / "link.oademo", link_error);

    const std::vector<lib::Listed> listed = lib::list_folder(recordings);
    OA_CHECK(listed.size() == 3);
    if (listed.size() != 3)
        return;
    OA_CHECK(listed[0].path == "Imported/c.oademo" && listed[0].file == lib::FileKind::oademo);
    OA_CHECK(listed[1].path == "default/a.oademo" && listed[1].file == lib::FileKind::oademo);
    OA_CHECK(listed[2].path == "ridge/b.tad" && listed[2].file == lib::FileKind::tad);
    OA_CHECK(listed[0].size == fs::file_size(recordings / "Imported" / "c.oademo"));
    OA_CHECK(listed[1].size == fs::file_size(recordings / "default" / "a.oademo"));
    OA_CHECK(listed[2].size == fs::file_size(recordings / "ridge" / "b.tad"));

    Seen seen;
    lib::ScanHooks hooks;
    hooks.context = &seen;
    hooks.replay = note_replay;
    const lib::Index scanned = lib::scan(recordings, {}, hooks);
    OA_CHECK(scanned.replays.size() == 3);
    OA_CHECK((seen.done == std::vector<std::size_t>{1, 2, 3}));
    OA_CHECK((seen.totals == std::vector<std::size_t>{3, 3, 3}));
    OA_CHECK(
        (seen.paths ==
         std::vector<std::string>{"Imported/c.oademo", "default/a.oademo", "ridge/b.tad"})
    );
    OA_CHECK(!fs::exists(lib::index_path(recordings)));
    if (scanned.replays.size() != 3)
        return;
    const lib::Replay& skirmish = scanned.replays[0];
    const lib::Replay& outpost = scanned.replays[1];
    const lib::Replay& recording = scanned.replays[2];
    const int64_t now = now_seconds();
    for (std::size_t at = 0; at < 3; ++at) {
        const lib::Replay& replay = scanned.replays[at];
        OA_CHECK(replay.path == listed[at].path);
        OA_CHECK(replay.size == listed[at].size && replay.modified == listed[at].modified);
        OA_CHECK(replay.modified > now - 3600 && replay.modified < now + 3600);
        OA_CHECK(!lib::damaged(replay) && !replay.kept);
        OA_CHECK(replay.sha256.size() == 64);
    }

    OA_CHECK(outpost.file == lib::FileKind::oademo);
    OA_CHECK(outpost.map == "Show Down");
    OA_CHECK(outpost.kind == "outpost" && outpost.how == "multiplayer");
    OA_CHECK(outpost.recorded == 1793802720);
    OA_CHECK(outpost.recorded_by == "Open Annihilation 0.9.0");
    OA_CHECK(outpost.length_ms == 1500 && outpost.ended == "cut" && outpost.after_crash);
    OA_CHECK(outpost.mod_id == "fixture-mod" && outpost.mod_name == "Fixture Mod");
    OA_CHECK(outpost.mod_version == "1.2" && outpost.mod_revision == std::optional<int64_t>{3});
    OA_CHECK(outpost.expansions == std::vector<std::string>{"expansion-one"});
    OA_CHECK((outpost.mutators == std::vector<std::string>{"double-metal", "fast-build"}));
    OA_CHECK(outpost.planet == "Corvane" && outpost.sector == "Low Reach");
    OA_CHECK(outpost.sha256 == outpost_replay().meta.recording.sha256);
    OA_CHECK(outpost.players.size() == 2);
    if (outpost.players.size() == 2) {
        OA_CHECK(outpost.players[0].name == "Pilot" && outpost.players[0].colour == 2);
        OA_CHECK(outpost.players[0].side == "arm" && outpost.players[0].kind == "human");
        OA_CHECK(outpost.players[0].team == std::optional<int32_t>{1});
        OA_CHECK(outpost.players[0].recorded_here && outpost.players[0].number == 1);
        OA_CHECK(outpost.players[1].name == "Screamer" && outpost.players[1].colour == 5);
        OA_CHECK(outpost.players[1].side == "core" && outpost.players[1].kind == "computer");
        OA_CHECK(!outpost.players[1].team.has_value() && !outpost.players[1].recorded_here);
        OA_CHECK(outpost.players[1].number == 2);
    }

    OA_CHECK(skirmish.kind == "skirmish" && skirmish.how == "skirmish");
    OA_CHECK(skirmish.map == "Dune Wars");
    OA_CHECK(skirmish.recorded == skirmish.modified);
    OA_CHECK(skirmish.length_ms == 0 && !skirmish.after_crash && skirmish.ended == "finished");
    OA_CHECK(skirmish.mod_id.empty() && !skirmish.mod_revision.has_value());
    OA_CHECK(skirmish.planet.empty() && skirmish.sector.empty());

    OA_CHECK(recording.file == lib::FileKind::tad);
    OA_CHECK(recording.map == "Ridge One" && recording.kind == lib::kind_multiplayer);
    OA_CHECK(recording.how.empty() && recording.recorded == recording.modified);
    OA_CHECK(recording.length_ms == 1500 && recording.recorded_by == "oa-test 1");
    OA_CHECK(recording.sha256 == sha256_of(file_bytes(recordings / "ridge" / "b.tad")));
    OA_CHECK(recording.players.size() == 2);
    if (recording.players.size() == 2) {
        OA_CHECK(recording.players[0].name == "Pilot" && recording.players[0].colour == 2);
        OA_CHECK(recording.players[0].side == "arm" && recording.players[0].kind == "human");
        OA_CHECK(recording.players[0].number == 1);
        OA_CHECK(recording.players[1].name == "Screamer" && recording.players[1].side == "core");
        OA_CHECK(recording.players[1].number == 2);
    }

    OA_CHECK(lib::imported(skirmish));
    OA_CHECK(!lib::imported(outpost) && !lib::imported(recording));
    lib::Replay named;
    named.path = "Imported";
    OA_CHECK(!lib::imported(named));
    named.path = "Importedx/c.oademo";
    OA_CHECK(!lib::imported(named));

    // A scan that is told to stop returns what it has done.
    Seen stopping;
    stopping.stop_at = 1;
    lib::ScanHooks stop_hooks;
    stop_hooks.context = &stopping;
    stop_hooks.replay = note_replay;
    stop_hooks.stop = enough_seen;
    const lib::Index stopped = lib::scan(recordings, {}, stop_hooks);
    OA_CHECK(stopped.replays.size() == 1 && stopping.done == std::vector<std::size_t>{1});

    // Files three folders down are listed, and four folders down are not.
    const fs::path edge = scratch.path() / "edge";
    write_bytes(
        edge / "one" / "two" / "three" / "kept.tad",
        tad_recording("Ridge One", "Pilot", "oa-test 1")
    );
    write_bytes(
        edge / "one" / "two" / "three" / "four" / "lost.tad",
        tad_recording("Ridge One", "Pilot", "oa-test 1")
    );
    write_bytes(edge / "top.TAD", tad_recording("Ridge One", "Pilot", "oa-test 1"));
    const std::vector<lib::Listed> edge_listed = lib::list_folder(edge);
    OA_CHECK(edge_listed.size() == 2);
    if (edge_listed.size() == 2) {
        OA_CHECK(edge_listed[0].path == "one/two/three/kept.tad");
        OA_CHECK(edge_listed[1].path == "top.TAD" && edge_listed[1].file == lib::FileKind::tad);
    }
    OA_CHECK(lib::list_folder(scratch.path() / "missing").empty());

    OA_CHECK(lib::file_kind_of("a.OADEMO") == lib::FileKind::oademo);
    OA_CHECK(lib::file_kind_of("ridge/b.Tad") == lib::FileKind::tad);
    OA_CHECK(!lib::file_kind_of("x.oademo.part").has_value());
    OA_CHECK(!lib::file_kind_of("a.oademo.oa-content").has_value());
    OA_CHECK(!lib::file_kind_of("").has_value());
}

void the_index_reads_back_as_written(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "index" / "Recordings";
    three_replays(recordings);
    lib::Index index = lib::scan(recordings, {});
    OA_CHECK(index.replays.size() == 3);
    lib::Replay odd;
    odd.path = "z/odd.tad";
    odd.size = std::numeric_limits<uint64_t>::max();
    odd.modified = -86400;
    odd.file = lib::FileKind::tad;
    odd.damage = std::string(lib::tad_unreadable);
    odd.recorded = -1;
    odd.length_ms = std::numeric_limits<uint64_t>::max() - 1;
    odd.mod_revision = std::numeric_limits<int64_t>::min();
    odd.map = "Caf\xC3\xA9 \"quoted\" \\ line\nend";
    lib::Player watcher;
    watcher.name = "Jos\xC3\xA9";
    watcher.colour = 9;
    watcher.side = "watcher";
    watcher.team = -3;
    watcher.kind = "watcher";
    watcher.number = 4;
    odd.players.push_back(watcher);
    // A campaign name can end in a space, and the index keeps it. The timeline
    // is the pass's text, and the watched tick is where a Watch was left.
    odd.mission_name = "Core Contingency ";
    odd.timeline = std::string("{\"markers\":[]}");
    odd.watched_tick = 21000;
    odd.read_stamp = "installed-1";
    index.replays.push_back(odd);

    const std::string text = lib::index_text(index);
    OA_CHECK(text == lib::index_text(index));
    OA_CHECK(
        text.rfind("{\"replays_index\":1,\"replays\":[{\"path\":\"Imported/c.oademo\",", 0) == 0
    );
    OA_CHECK(text.find('\n') == std::string::npos);
    OA_CHECK(text.find("\"mod_revision\":3,") != std::string::npos);
    OA_CHECK(text.find("\"size\":18446744073709551615,") != std::string::npos);
    const std::optional<lib::Index> parsed = lib::parse_index(text);
    OA_CHECK(parsed.has_value());
    if (!parsed.has_value())
        return;
    OA_CHECK(same_index(*parsed, index));
    OA_CHECK(lib::index_text(*parsed) == text);

    OA_CHECK(lib::save_index(recordings, index));
    OA_CHECK(same_index(lib::load_index(recordings), index));
    OA_CHECK(file_bytes(lib::index_path(recordings)).size() == text.size());
    std::size_t names = 0;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(recordings / std::string(lib::replays_folder_name)))
        names += entry.path().filename() == "index.json" ? 1 : 100;
    OA_CHECK(names == 1);
    // A second save replaces the first whole.
    lib::Index fewer = index;
    fewer.replays.resize(1);
    OA_CHECK(lib::save_index(recordings, fewer));
    OA_CHECK(same_index(lib::load_index(recordings), fewer));

    // Unknown members are ignored; an entry with no path, a wrong member or a
    // path already read is dropped; the entries are sorted.
    const std::optional<lib::Index> lenient = lib::parse_index(
        "{\"later\":[1],\"replays_index\":1,\"replays\":["
        "{\"path\":\"b.tad\",\"file\":\"tad\",\"size\":7,\"later\":{\"x\":1}},"
        "{\"size\":1},"
        "{\"path\":\"\"},"
        "5,"
        "{\"path\":\"c.oademo\",\"size\":\"big\"},"
        "{\"path\":\"d.oademo\",\"file\":\"film\"},"
        "{\"path\":\"e.oademo\",\"players\":[{\"colour\":4294967296}]},"
        "{\"path\":\"a.oademo\",\"kept\":true,\"players\":[{\"name\":\"Pilot\"}]},"
        "{\"path\":\"b.tad\",\"file\":\"tad\",\"size\":8}"
        "]}"
    );
    OA_CHECK(lenient.has_value());
    if (lenient.has_value()) {
        OA_CHECK(lenient->replays.size() == 2);
        if (lenient->replays.size() == 2) {
            // An old index's `kept` member is not read: the marks are not the index's.
            OA_CHECK(lenient->replays[0].path == "a.oademo" && !lenient->replays[0].kept);
            OA_CHECK(lenient->replays[0].file == lib::FileKind::oademo);
            OA_CHECK(lenient->replays[0].players.size() == 1);
            OA_CHECK(lenient->replays[1].path == "b.tad" && lenient->replays[1].size == 7);
            OA_CHECK(lenient->replays[1].file == lib::FileKind::tad);
        }
    }

    // Refused: not JSON, not an object, another version, no array of replays.
    OA_CHECK(!lib::parse_index("").has_value());
    OA_CHECK(!lib::parse_index("{").has_value());
    OA_CHECK(!lib::parse_index("[]").has_value());
    OA_CHECK(!lib::parse_index("{\"replays_index\":2,\"replays\":[]}").has_value());
    OA_CHECK(!lib::parse_index("{\"replays_index\":\"1\",\"replays\":[]}").has_value());
    OA_CHECK(!lib::parse_index("{\"replays_index\":1}").has_value());
    OA_CHECK(!lib::parse_index("{\"replays_index\":1,\"replays\":{}}").has_value());
    OA_CHECK(lib::parse_index("{\"replays_index\":1,\"replays\":[]}").has_value());

    // An empty timeline is a finished pass that left nothing, not a file still
    // waiting. A timeline that is not text drops the entry. A watched tick of
    // 0 is kept, and a campaign name keeps its trailing space.
    const std::optional<lib::Index> finished = lib::parse_index(
        "{\"replays_index\":1,\"replays\":["
        "{\"path\":\"wait.oademo\",\"file\":\"oademo\"},"
        "{\"path\":\"done.oademo\",\"file\":\"oademo\",\"timeline\":\"\","
        "\"mission_name\":\"Core Contingency \",\"watched_tick\":0},"
        "{\"path\":\"bad.oademo\",\"timeline\":1}"
        "]}"
    );
    OA_CHECK(finished.has_value());
    if (finished.has_value() && finished->replays.size() == 2) {
        OA_CHECK(finished->replays[0].path == "done.oademo");
        OA_CHECK(finished->replays[0].timeline.has_value());
        OA_CHECK(finished->replays[0].timeline->empty());
        OA_CHECK(finished->replays[0].mission_name == "Core Contingency ");
        OA_CHECK(finished->replays[0].watched_tick == std::optional<uint32_t>{0});
        OA_CHECK(finished->replays[1].path == "wait.oademo");
        OA_CHECK(!finished->replays[1].timeline.has_value());
    } else {
        OA_CHECK(false);
    }

    // An index that is not an index loads as an empty one, and is damaged; a
    // missing one is empty and not damaged.
    write_text(lib::index_path(recordings), "not an index");
    OA_CHECK(lib::load_index(recordings).replays.empty());
    OA_CHECK(lib::read_index(recordings).damaged);
    OA_CHECK(lib::load_index(scratch.path() / "nowhere").replays.empty());
    OA_CHECK(!lib::read_index(scratch.path() / "nowhere").damaged);
    write_text(lib::index_path(recordings), "{\"replays_index\":1,\"replays\":[{\"path\":");
    OA_CHECK(lib::read_index(recordings).damaged);
    std::string junk(300, '\0');
    for (std::size_t at = 0; at < junk.size(); ++at)
        junk[at] = static_cast<char>((at * 37U + 11U) & 0xFFU);
    write_text(lib::index_path(recordings), junk);
    OA_CHECK(
        lib::read_index(recordings).damaged && lib::read_index(recordings).index.replays.empty()
    );
    OA_CHECK(lib::save_index(recordings, index));
    OA_CHECK(!lib::read_index(recordings).damaged);
    OA_CHECK(
        lib::index_path(recordings) ==
        recordings / std::string(lib::replays_folder_name) / std::string(lib::index_name)
    );

    const lib::Replay* found = lib::find(index, "ridge/b.tad");
    OA_CHECK(found != nullptr && found->map == "Ridge One");
    OA_CHECK(lib::find(index, "ridge/none.tad") == nullptr);
}

void a_deleted_index_is_rebuilt(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "rebuild" / "Recordings";
    three_replays(recordings);
    const fs::path tad_file = recordings / "ridge" / "b.tad";
    const std::vector<uint8_t> original = file_bytes(tad_file);
    const fs::file_time_type first_stamp = fs::last_write_time(tad_file);

    lib::Index first = lib::scan(recordings, {});
    OA_CHECK(first.replays.size() == 3);
    OA_CHECK(lib::save_index(recordings, first));
    const lib::Index loaded = lib::load_index(recordings);
    OA_CHECK(same_index(loaded, first));

    // The same size and time: the index's entry is taken without opening the
    // file, so the new map's name is not seen.
    const std::vector<uint8_t> same_size = tad_recording("Ridge Two", "Pilot", "oa-test 1");
    OA_CHECK(same_size.size() == original.size());
    write_bytes(tad_file, same_size);
    fs::last_write_time(tad_file, first_stamp);
    const lib::Index second = lib::scan(recordings, loaded);
    OA_CHECK(same_index(second, first));

    // Another size: the file is read again.
    write_bytes(tad_file, tad_recording("Ridge Three", "Pilot", "oa-test 1"));
    const lib::Index third = lib::scan(recordings, loaded);
    const lib::Replay* changed = lib::find(third, "ridge/b.tad");
    OA_CHECK(changed != nullptr && changed->map == "Ridge Three");
    OA_CHECK(third.replays.size() == 3);

    // The index deleted: the folder is read again and gives the same replays.
    write_bytes(tad_file, original);
    fs::last_write_time(tad_file, first_stamp);
    std::error_code removed;
    OA_CHECK(fs::remove(lib::index_path(recordings), removed));
    const lib::Index empty = lib::load_index(recordings);
    OA_CHECK(empty.replays.empty());
    const lib::Index rebuilt = lib::scan(recordings, empty);
    OA_CHECK(same_index(rebuilt, first));
}

/// Finds a replay of an index, which the test needs to be there.
///
/// @param index the index
/// @param path the replay's path
/// @return the replay; an empty one, after a failed check, when the index has none
const lib::Replay& replay_at(const lib::Index& index, std::string_view path) {
    const lib::Replay* found = lib::find(index, path);
    OA_CHECK(found != nullptr);
    static const lib::Replay none;
    return found != nullptr ? *found : none;
}

void kept_marks_outlive_the_index(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "kept" / "Recordings";
    three_replays(recordings);
    write_bytes(recordings / "default" / "cut.oademo", std::vector<uint8_t>(64, 0x5A));
    lib::Index index = lib::scan(recordings, {});
    OA_CHECK(index.replays.size() == 4);
    OA_CHECK(lib::damaged(replay_at(index, "default/cut.oademo")));
    OA_CHECK(replay_at(index, "default/cut.oademo").sha256.empty());
    OA_CHECK(!lib::read_kept(recordings).damaged && lib::read_kept(recordings).kept.marks.empty());

    // Two replays are kept: one by its recording, one damaged, by its path.
    lib::KeptMarks kept;
    OA_CHECK(lib::set_kept(kept, replay_at(index, "ridge/b.tad"), true));
    OA_CHECK(!lib::set_kept(kept, replay_at(index, "ridge/b.tad"), true));
    OA_CHECK(lib::set_kept(kept, replay_at(index, "default/cut.oademo"), true));
    OA_CHECK(kept.marks.size() == 2);
    OA_CHECK(lib::is_kept(kept, replay_at(index, "ridge/b.tad")));
    OA_CHECK(lib::is_kept(kept, replay_at(index, "default/cut.oademo")));
    OA_CHECK(!lib::is_kept(kept, replay_at(index, "default/a.oademo")));
    OA_CHECK(lib::save_kept(recordings, kept));
    OA_CHECK(
        lib::kept_path(recordings) ==
        recordings / std::string(lib::replays_folder_name) / std::string(lib::kept_name)
    );

    // The text reads back to the same marks, and is the same text again.
    const std::string text = lib::kept_text(kept);
    OA_CHECK(text.find('\n') == std::string::npos);
    OA_CHECK(
        text.rfind("{\"replays_kept\":1,\"kept\":[{\"sha256\":\"\",\"path\":\"default/cut", 0) == 0
    );
    const std::optional<lib::KeptMarks> parsed = lib::parse_kept(text);
    OA_CHECK(parsed.has_value() && lib::kept_text(*parsed) == text);
    OA_CHECK(file_bytes(lib::kept_path(recordings)).size() == text.size());

    // Deleting the index, damaging it or filling it with junk loses nothing: the
    // marks are read from their own file, and the rebuilt replays are kept.
    lib::apply_kept(kept, index.replays);
    OA_CHECK(lib::save_index(recordings, index));
    const auto check_kept_after = [&](const std::string& why) {
        const lib::KeptRead read = lib::read_kept(recordings);
        OA_CHECK(!read.damaged && read.kept.marks.size() == 2);
        const lib::IndexRead index_read = lib::read_index(recordings);
        lib::Index rebuilt = lib::scan(recordings, index_read.index);
        lib::KeptMarks marks = read.kept;
        lib::apply_kept(marks, rebuilt.replays);
        OA_CHECK(rebuilt.replays.size() == 4);
        for (const lib::Replay& replay : rebuilt.replays)
            OA_CHECK(
                replay.kept == (replay.path == "ridge/b.tad" || replay.path == "default/cut.oademo")
            );
        static_cast<void>(why);
    };
    std::error_code removed;
    OA_CHECK(fs::remove(lib::index_path(recordings), removed));
    check_kept_after("deleted");
    write_text(lib::index_path(recordings), "{\"replays_index\":1,\"replays\":[{\"path\":");
    check_kept_after("truncated");
    write_text(lib::index_path(recordings), std::string(200, '\x07'));
    check_kept_after("junk");

    // A damaged index is set aside, not overwritten: the next save leaves it be.
    OA_CHECK(lib::read_index(recordings).damaged);
    const std::optional<fs::path> aside = lib::set_aside(lib::index_path(recordings));
    OA_CHECK(aside.has_value() && aside->filename() == "index.json.damaged");
    OA_CHECK(!fs::exists(lib::index_path(recordings)));
    OA_CHECK(aside.has_value() && file_bytes(*aside).size() == 200);
    OA_CHECK(lib::save_index(recordings, index));
    OA_CHECK(!lib::read_index(recordings).damaged);
    OA_CHECK(aside.has_value() && file_bytes(*aside).size() == 200);
    // A file set aside again keeps the earlier copy beside the new one.
    write_text(lib::index_path(recordings), "second damage");
    const std::optional<fs::path> second = lib::set_aside(lib::index_path(recordings));
    OA_CHECK(second.has_value() && second->filename() == "index.json.damaged-2");
    OA_CHECK(aside.has_value() && file_bytes(*aside).size() == 200);
    OA_CHECK(second.has_value() && file_bytes(*second).size() == 13);
    OA_CHECK(!lib::set_aside(recordings / "nowhere.json").has_value());

    // A moved or renamed file keeps its identity, so it stays kept; the mark's
    // path is brought up to date, and saving it changes the file.
    const std::vector<uint8_t> moved_bytes = file_bytes(recordings / "ridge" / "b.tad");
    OA_CHECK(fs::remove(recordings / "ridge" / "b.tad", removed));
    write_bytes(recordings / "Imported" / "renamed.tad", moved_bytes);
    lib::Index after_move = lib::scan(recordings, {});
    lib::KeptMarks marks = lib::read_kept(recordings).kept;
    OA_CHECK(lib::apply_kept(marks, after_move.replays));
    OA_CHECK(!lib::apply_kept(marks, after_move.replays));
    OA_CHECK(replay_at(after_move, "Imported/renamed.tad").kept);
    bool path_follows = false;
    for (const lib::KeptMark& mark : marks.marks)
        path_follows = path_follows || mark.path == "Imported/renamed.tad";
    OA_CHECK(path_follows);
    OA_CHECK(!replay_at(after_move, "default/a.oademo").kept);

    // Two copies of one recording are one replay: both kept, one mark.
    write_bytes(recordings / "default" / "copy.tad", moved_bytes);
    lib::Index twins = lib::scan(recordings, {});
    OA_CHECK(!lib::apply_kept(marks, twins.replays));
    OA_CHECK(
        replay_at(twins, "default/copy.tad").kept && replay_at(twins, "Imported/renamed.tad").kept
    );
    OA_CHECK(marks.marks.size() == 2);

    // Deleting one copy leaves the mark on the copy that stays.
    {
        lib::KeptMarks shared = marks;
        const lib::Replay& gone = replay_at(twins, "default/copy.tad");
        for (lib::KeptMark& mark : shared.marks)
            if (mark.sha256 == gone.sha256)
                mark.path = gone.path;
        OA_CHECK(lib::release_kept(shared, twins.replays, gone));
        bool retargeted = false;
        for (const lib::KeptMark& mark : shared.marks)
            retargeted =
                retargeted || (mark.sha256 == gone.sha256 && mark.path == "Imported/renamed.tad");
        OA_CHECK(retargeted);
        std::vector<lib::Replay> remaining;
        for (const lib::Replay& replay : twins.replays)
            if (replay.path != gone.path)
                remaining.push_back(replay);
        OA_CHECK(!lib::apply_kept(shared, remaining));
        bool other_kept = false;
        for (const lib::Replay& replay : remaining)
            if (replay.path == "Imported/renamed.tad")
                other_kept = replay.kept;
        OA_CHECK(other_kept);
    }

    // A kept file whose bytes changed, and one that no longer reads, stay kept
    // by the mark's path. Each gains a mark of its own. A twin that still has
    // the identity is not stolen: both stay kept.
    {
        const lib::Replay original = replay_at(index, "ridge/b.tad");
        lib::KeptMarks path_marks;
        OA_CHECK(lib::set_kept(path_marks, original, true));
        lib::Replay changed = original;
        changed.sha256 = "0123456789abcdef";
        changed.size += 16;
        std::vector<lib::Replay> one{changed};
        OA_CHECK(lib::apply_kept(path_marks, one));
        OA_CHECK(!lib::apply_kept(path_marks, one));
        OA_CHECK(one[0].kept);
        OA_CHECK(lib::is_kept(path_marks, one[0]));
        OA_CHECK(path_marks.marks.size() == 2);
        bool old_stayed = false;
        bool added = false;
        for (const lib::KeptMark& mark : path_marks.marks) {
            old_stayed =
                old_stayed || (mark.sha256 == original.sha256 && mark.path == original.path);
            added = added || (mark.sha256 == changed.sha256 && mark.path == changed.path);
        }
        OA_CHECK(old_stayed && added);

        lib::Replay cut = original;
        cut.sha256.clear();
        cut.damage = "cut";
        std::vector<lib::Replay> half{cut};
        OA_CHECK(lib::apply_kept(path_marks, half));
        OA_CHECK(half[0].kept);
        OA_CHECK(lib::is_kept(path_marks, half[0]));
        bool path_alone = false;
        for (const lib::KeptMark& mark : path_marks.marks)
            path_alone = path_alone || (mark.sha256.empty() && mark.path == cut.path);
        OA_CHECK(path_alone);

        // A twin that still has the identity is not stolen: both stay kept.
        lib::Replay mutated = original;
        mutated.path = "default/mutated.tad";
        mutated.sha256 = "fedcba9876543210";
        std::vector<lib::Replay> both{original, mutated};
        lib::KeptMarks owned;
        OA_CHECK(lib::set_kept(owned, original, true));
        owned.marks[0].path = mutated.path;
        OA_CHECK(lib::apply_kept(owned, both));
        OA_CHECK(!lib::apply_kept(owned, both));
        OA_CHECK(both[0].kept && both[1].kept);
        OA_CHECK(lib::is_kept(owned, both[0]) && lib::is_kept(owned, both[1]));
        bool path_stayed = false;
        bool mutated_marked = false;
        for (const lib::KeptMark& mark : owned.marks) {
            path_stayed =
                path_stayed || (mark.sha256 == original.sha256 && mark.path == mutated.path);
            mutated_marked =
                mutated_marked || (mark.sha256 == mutated.sha256 && mark.path == mutated.path);
        }
        OA_CHECK(path_stayed && mutated_marked);
    }

    // A kept.json cut in half still yields the mark objects that are whole.
    // A replay whose identity or path occurs in a set-aside file's bytes is spared.
    {
        const std::string whole = lib::kept_text(kept);
        OA_CHECK(whole.size() > 4);
        const lib::KeptMarks from_whole = lib::salvage_kept(whole);
        OA_CHECK(lib::kept_text(from_whole) == whole);
        const std::string cut = whole.substr(0, whole.size() / 2);
        const lib::KeptMarks from_cut = lib::salvage_kept(cut);
        OA_CHECK(!from_cut.marks.empty());
        OA_CHECK(from_cut.marks.size() < kept.marks.size() || cut.find('}') != std::string::npos);
        lib::Replay named;
        named.path = "default/spared.oademo";
        named.sha256 = "abcdef0123456789";
        const std::string aside_bytes = std::string("junk ") + std::string(named.sha256) + " more";
        OA_CHECK(lib::spared_by_kept_bytes(aside_bytes, named.sha256, named.path));
        OA_CHECK(lib::spared_by_kept_bytes(std::string("see ") + named.path, "", named.path));
        OA_CHECK(!lib::spared_by_kept_bytes(aside_bytes, "", ""));
        OA_CHECK(!lib::spared_by_kept_bytes("nothing here", named.sha256, named.path));
    }

    // Stopping to keep a replay removes its mark, by recording or by path.
    OA_CHECK(lib::set_kept(marks, replay_at(twins, "default/copy.tad"), false));
    OA_CHECK(!lib::set_kept(marks, replay_at(twins, "default/copy.tad"), false));
    OA_CHECK(lib::set_kept(marks, replay_at(twins, "default/cut.oademo"), false));
    OA_CHECK(marks.marks.empty());

    // Marks of the wrong shape: not kept marks, or marks with entries dropped.
    OA_CHECK(!lib::parse_kept("").has_value());
    OA_CHECK(!lib::parse_kept("[]").has_value());
    OA_CHECK(!lib::parse_kept("{\"replays_kept\":2,\"kept\":[]}").has_value());
    OA_CHECK(!lib::parse_kept("{\"replays_kept\":1}").has_value());
    OA_CHECK(!lib::parse_kept("{\"replays_kept\":1,\"kept\":{}}").has_value());
    const std::optional<lib::KeptMarks> lenient = lib::parse_kept(
        "{\"later\":1,\"replays_kept\":1,\"kept\":["
        "{\"sha256\":\"AB12\",\"path\":\"x.tad\",\"later\":true},"
        "{\"sha256\":\"ab12\",\"path\":\"y.tad\"},"
        "{\"path\":\"damaged.tad\"},"
        "{\"path\":\"damaged.tad\"},"
        "{\"sha256\":5},"
        "{},"
        "7"
        "]}"
    );
    OA_CHECK(lenient.has_value());
    if (lenient.has_value()) {
        OA_CHECK(lenient->marks.size() == 2);
        if (lenient->marks.size() == 2) {
            OA_CHECK(lenient->marks[0].sha256.empty() && lenient->marks[0].path == "damaged.tad");
            OA_CHECK(lenient->marks[1].sha256 == "ab12" && lenient->marks[1].path == "x.tad");
        }
    }

    // A kept file that is damaged is damaged, and a missing one is not.
    write_text(lib::kept_path(recordings), "not kept marks");
    OA_CHECK(lib::read_kept(recordings).damaged && lib::read_kept(recordings).kept.marks.empty());
    OA_CHECK(lib::set_aside(lib::kept_path(recordings)).has_value());
    OA_CHECK(!lib::read_kept(recordings).damaged);
    OA_CHECK(!lib::read_kept(scratch.path() / "nowhere").damaged);
}

void a_tad_header_is_read() {
    // A map name ending in a NUL, a player named in Windows-1252, one in
    // UTF-8, and the recorder's text with a Windows-1252 sign.
    const std::vector<uint8_t> bytes =
        tad_recording(std::string("Caf\xE9 Isle\0junk", 14), "Jos\xE9 \x80\x81", "TA Demo\x99 2");
    const lib::Replay replay = lib::read_replay(bytes, lib::FileKind::tad);
    OA_CHECK(!lib::damaged(replay));
    OA_CHECK(replay.file == lib::FileKind::tad);
    OA_CHECK(replay.map == "Caf\xC3\xA9 Isle");
    OA_CHECK(replay.recorded_by == "TA Demo\xE2\x84\xA2 2");
    OA_CHECK(replay.length_ms == 1500);
    OA_CHECK(replay.kind == "multiplayer" && replay.how.empty());
    OA_CHECK(replay.sha256 == sha256_of(bytes));
    OA_CHECK(replay.path.empty() && replay.size == 0 && replay.modified == 0);
    OA_CHECK(replay.recorded == 0 && !replay.kept);
    OA_CHECK(replay.players.size() == 2);
    if (replay.players.size() == 2) {
        OA_CHECK(replay.players[0].name == "Jos\xC3\xA9 \xE2\x82\xAC\xC2\x81");
        OA_CHECK(replay.players[0].colour == 2 && replay.players[0].side == "arm");
        OA_CHECK(replay.players[0].number == 1);
        OA_CHECK(replay.players[1].name == "Screamer" && replay.players[1].colour == 5);
        OA_CHECK(replay.players[1].number == 2);
        OA_CHECK(replay.players[1].side == "core" && replay.players[1].kind == "human");
        OA_CHECK(!replay.players[1].team.has_value() && !replay.players[1].recorded_here);
    }
    const lib::Replay utf8 =
        lib::read_replay(tad_recording("Zo\xC3\xAB", "Zo\xC3\xAB", ""), lib::FileKind::tad);
    OA_CHECK(utf8.map == "Zo\xC3\xAB" && utf8.recorded_by.empty());
    OA_CHECK(!utf8.players.empty() && utf8.players[0].name == "Zo\xC3\xAB");
}

void damaged_files_are_listed_damaged(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "damaged" / "Recordings";
    write_bytes(recordings / "default" / "a.oademo", oademo_bytes(outpost_replay()));
    std::vector<uint8_t> cut = oademo_bytes(outpost_replay());
    cut.resize(cut.size() / 2);
    write_bytes(recordings / "default" / "cut.oademo", cut);
    write_bytes(recordings / "default" / "other.oademo", plain_zip());
    std::vector<uint8_t> noise(4096);
    uint32_t state = 0x2468ACE1U;
    for (uint8_t& byte : noise) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<uint8_t>(state);
    }
    write_bytes(recordings / "default" / "noise.tad", noise);
    write_text(recordings / "default" / "empty.oademo", "");
    write_text(recordings / "default" / "empty.tad", "");

    lib::Index scanned;
    bool threw = false;
    try {
        scanned = lib::scan(recordings, {});
    } catch (...) {
        threw = true;
    }
    OA_CHECK(!threw);
    const auto expect = [&](std::string_view path, std::string_view damage) {
        const lib::Replay* replay = lib::find(scanned, path);
        OA_CHECK(replay != nullptr);
        if (replay == nullptr)
            return;
        OA_CHECK(replay->damage == damage);
        OA_CHECK(lib::damaged(*replay) == !damage.empty());
        if (!damage.empty()) {
            OA_CHECK(replay->map.empty() && replay->players.empty() && replay->sha256.empty());
            OA_CHECK(replay->recorded == replay->modified);
        }
    };
    expect("default/a.oademo", "");
    expect("default/cut.oademo", oademo::not_a_replay);
    expect("default/other.oademo", oademo::not_a_replay);
    expect("default/noise.tad", lib::tad_unreadable);
    expect("default/empty.oademo", oademo::not_a_replay);
    expect("default/empty.tad", lib::tad_unreadable);
    const lib::Replay* good = lib::find(scanned, "default/a.oademo");
    OA_CHECK(good != nullptr && good->map == "Show Down");

    // Read directly: bytes of the other kind, a missing file, and paths that
    // leave Recordings.
    OA_CHECK(lib::read_replay(noise, lib::FileKind::oademo).damage == oademo::not_a_replay);
    OA_CHECK(lib::read_replay({}, lib::FileKind::tad).damage == lib::tad_unreadable);
    OA_CHECK(lib::read_file(recordings, "default/none.tad").damage == lib::file_unreadable);
    OA_CHECK(
        lib::read_file(recordings, "../Recordings/default/a.oademo").damage == lib::file_unreadable
    );
    OA_CHECK(lib::read_file(recordings, "default//a.oademo").damage == lib::file_unreadable);
    OA_CHECK(lib::read_file(recordings, "").damage == lib::file_unreadable);
    OA_CHECK(lib::read_file(recordings, "default").damage == lib::file_unreadable);
    const lib::Replay direct = lib::read_file(recordings, "default/a.oademo");
    OA_CHECK(
        !lib::damaged(direct) && direct.path == "default/a.oademo" && direct.map == "Show Down"
    );
}

void kinds_register_by_id() {
    lib::forget_kinds();
    OA_CHECK(lib::registered_kinds().empty());
    lib::register_kind({"outpost", "Outpost battle"});
    lib::register_kind({"siege", "Siege"});
    lib::register_kind({"", "Nothing"});
    lib::register_kind({"outpost", "Outpost"});
    const std::vector<lib::Kind> kinds = lib::registered_kinds();
    OA_CHECK(kinds.size() == 2);
    if (kinds.size() == 2) {
        OA_CHECK(kinds[0].id == "outpost" && kinds[0].title == "Outpost");
        OA_CHECK(kinds[1].id == "siege" && kinds[1].title == "Siege");
    }
    lib::forget_kinds();
    OA_CHECK(lib::registered_kinds().empty());
}

void days_group_a_list() {
    OA_CHECK(lib::day_number(1970, 1, 1) == 0);
    OA_CHECK(lib::day_number(1969, 12, 31) == -1);
    OA_CHECK(lib::day_number(2025, 11, 6) == 20398);
    OA_CHECK(lib::day_number(2026, 11, 6) == 20763);
    OA_CHECK(lib::day_number(2024, 2, 29) == 19782);
    OA_CHECK(lib::day_number(2024, 3, 1) == 19783);
    OA_CHECK(lib::day_number(2000, 3, 1) == 11017);
    OA_CHECK(lib::day_number(1600, 2, 29) == -135081);
    OA_CHECK(
        lib::day_number(std::numeric_limits<int32_t>::max(), 12, 31) ==
        std::numeric_limits<int32_t>::max()
    );

    const std::vector<int32_t> days{20398, 20398, 20397, 20395, 20395, 20395};
    const std::vector<lib::DayGroup> groups = lib::group_by_day(days);
    OA_CHECK(groups.size() == 3);
    if (groups.size() == 3) {
        OA_CHECK(groups[0].day == 20398 && groups[0].first == 0 && groups[0].count == 2);
        OA_CHECK(groups[1].day == 20397 && groups[1].first == 2 && groups[1].count == 1);
        OA_CHECK(groups[2].day == 20395 && groups[2].first == 3 && groups[2].count == 3);
    }
    OA_CHECK(lib::group_by_day({}).empty());
}

/// Reads a little-endian u16.
///
/// @param bytes the bytes
/// @param at where it starts
/// @return the value
uint16_t u16_at(std::span<const uint8_t> bytes, std::size_t at) {
    return static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8U));
}

/// Writes a little-endian u32 over four bytes already there.
///
/// @param[in,out] bytes the bytes
/// @param at where it starts
/// @param value the value
void put_u32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8U);
    bytes[at + 2] = static_cast<uint8_t>(value >> 16U);
    bytes[at + 3] = static_cast<uint8_t>(value >> 24U);
}

/// A recording with one sector 6 spliced in after the count, and the count raised by one.
///
/// @param clean a recording tad::write made, with no sector 6
/// @return the same bytes plus that sector
std::vector<uint8_t> spliced_sector_six(std::vector<uint8_t> clean) {
    OA_CHECK(clean.size() >= 12);
    const uint16_t header = u16_at(clean, 0);
    const std::size_t count_at = header;
    OA_CHECK(count_at + 6 <= clean.size());
    const uint16_t count_total = u16_at(clean, count_at);
    OA_CHECK(count_total == 6);
    const std::size_t count_value = count_at + tad::layout::chunk_length_bytes;
    const uint32_t count = static_cast<uint32_t>(clean[count_value]) |
                           (static_cast<uint32_t>(clean[count_value + 1]) << 8U) |
                           (static_cast<uint32_t>(clean[count_value + 2]) << 16U) |
                           (static_cast<uint32_t>(clean[count_value + 3]) << 24U);
    put_u32(clean, count_value, count + 1U);
    const std::array<uint8_t, 6> sector{
        0x06,
        0x00,
        0x06,
        0x00,
        0x00,
        0x00,
    };
    clean.insert(
        clean.begin() + static_cast<std::ptrdiff_t>(count_at + count_total),
        sector.begin(),
        sector.end()
    );
    return clean;
}

/// How many sectors of type 6 a recording holds. -1 when it does not parse.
///
/// @param bytes the recording
/// @return the count
int sector_six_count(std::span<const uint8_t> bytes) {
    const tad::ParseResult parsed = tad::parse(bytes);
    if (!parsed.ok())
        return -1;
    int count = 0;
    for (const tad::Sector& sector : parsed.demo->sectors)
        if (sector.type == static_cast<uint32_t>(tad::SectorType::player_address))
            ++count;
    return count;
}

/// A `.oademo` whose recording is deflated and holds sector 6, so `write` would refuse it.
///
/// @param recording the recording, sector 6 included
/// @param replay the description and the manifest, its identity already the recording's
/// @return the archive
std::vector<uint8_t>
dirty_oademo(std::span<const uint8_t> recording, const oademo::Replay& replay) {
    MemoryOut out;
    zip::WriterOptions options;
    options.deflate_level = oademo::deflate_level;
    zip::StreamWriter writer(zip::OutputHooks{&out, write_memory}, options);
    zip::ZipError problem;
    const std::string meta = oademo::meta_text(replay.meta);
    const std::string manifest = oademo::manifest_text(replay.manifest);
    const auto put =
        [&](std::string_view name, std::span<const uint8_t> bytes, zip::Method method) {
            return writer.begin_entry(name, bytes.size(), method, problem) &&
                   writer.write(bytes, problem) && writer.end_entry(problem);
        };
    const bool wrote = put("oademo",
                           {reinterpret_cast<const uint8_t*>(oademo::signature_line.data()),
                            oademo::signature_line.size()},
                           zip::Method::stored) &&
                       put("meta.json",
                           {reinterpret_cast<const uint8_t*>(meta.data()), meta.size()},
                           zip::Method::deflated) &&
                       put("manifest.json",
                           {reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()},
                           zip::Method::deflated) &&
                       put("recording.tad", recording, zip::Method::deflated) &&
                       writer.finish(problem);
    OA_CHECK(wrote);
    return out.bytes;
}

/// A replay opened from outside is copied into Imported, sector 6 left out, and
/// a second copy of the same recording is selected rather than written again.
///
/// @param scratch the scratch folder
void an_opened_replay_is_imported(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "Recordings";
    const fs::path sources = scratch.path() / "opened";
    const std::vector<uint8_t> clean = tad_recording("Anteer", "Pilot", "oa-test 1");
    OA_CHECK(sector_six_count(clean) == 0);
    const std::vector<uint8_t> dirty = spliced_sector_six(clean);
    OA_CHECK(sector_six_count(dirty) == 1);
    write_bytes(sources / "anteer.tad", dirty);

    const lib::ImportResult first = lib::import_replay(recordings, sources / "anteer.tad");
    OA_CHECK(first.ok && first.copied);
    OA_CHECK(first.path == "Imported/anteer.tad");
    OA_CHECK(first.title == "Anteer");
    OA_CHECK(first.error.empty());
    const std::vector<uint8_t> kept = file_bytes(recordings / "Imported" / "anteer.tad");
    OA_CHECK(kept == clean);
    OA_CHECK(sector_six_count(kept) == 0);
    OA_CHECK(first.sha256 == sha256_of(kept));

    const lib::ImportResult again = lib::import_replay(recordings, sources / "anteer.tad");
    OA_CHECK(again.ok && !again.copied);
    OA_CHECK(again.path == first.path);
    OA_CHECK(again.sha256 == first.sha256);

    const std::vector<uint8_t> plain = tad_recording("Plain", "Pilot", "oa-test 1");
    write_bytes(sources / "plain.tad", plain);
    const lib::ImportResult same = lib::import_replay(recordings, sources / "plain.tad");
    OA_CHECK(same.ok && same.copied);
    OA_CHECK(file_bytes(recordings / "Imported" / "plain.tad") == plain);

    write_bytes(sources / "broken.tad", {reinterpret_cast<const uint8_t*>("hello"), 5});
    const lib::ImportResult broken = lib::import_replay(recordings, sources / "broken.tad");
    OA_CHECK(!broken.ok);
    OA_CHECK(broken.error == lib::tad_not_a_recording);
    OA_CHECK(broken.sentence == lib::tad_not_a_recording);

    const char notes[] = "these are notes, not a recording";
    write_bytes(
        sources / "notes.tad", {reinterpret_cast<const uint8_t*>(notes), sizeof(notes) - 1}
    );
    const lib::ImportResult text = lib::import_replay(recordings, sources / "notes.tad");
    OA_CHECK(!text.ok);
    OA_CHECK(text.error == lib::tad_not_a_recording);
    OA_CHECK(text.sentence == lib::tad_not_a_recording);

    std::vector<uint8_t> versioned = clean;
    OA_CHECK(versioned.size() > 11);
    versioned[10] = 3;
    versioned[11] = 0;
    write_bytes(sources / "version.tad", versioned);
    const lib::ImportResult version = lib::import_replay(recordings, sources / "version.tad");
    OA_CHECK(!version.ok);
    OA_CHECK(version.version == "3");
    OA_CHECK(version.sentence == lib::tad_unsupported_version);
    OA_CHECK(
        version.error ==
        "This recording is in version 3 of the format, which Open Annihilation does not play"
    );

    oademo::Replay replay = sample_replay("Show Down");
    tad::Demo demo;
    const tad::ParseResult parsed = tad::parse(replay.recording);
    OA_CHECK(parsed.ok());
    if (parsed.ok())
        demo = *parsed.demo;
    const std::vector<uint8_t> extra{1, 2, 3, 4};
    demo.sectors.push_back({static_cast<uint32_t>(tad::SectorType::player_address), extra});
    const tad::WriteResult written = tad::write(demo);
    OA_CHECK(written.ok());
    replay.recording = written.bytes;
    oademo::set_recording_identity(replay);
    const std::vector<uint8_t> archive = dirty_oademo(replay.recording, replay);
    const oademo::ReadResult readable = oademo::read(archive);
    OA_CHECK(readable.ok());
    write_bytes(sources / "show-down.oademo", archive);
    const lib::ImportResult opened = lib::import_replay(recordings, sources / "show-down.oademo");
    OA_CHECK(opened.ok && opened.copied);
    OA_CHECK(opened.title == "Show Down");
    OA_CHECK(opened.path == "Imported/show-down.oademo");
    const std::vector<uint8_t> kept_demo = file_bytes(recordings / "Imported" / "show-down.oademo");
    const oademo::ReadResult kept_read = oademo::read(kept_demo);
    OA_CHECK(kept_read.ok());
    if (kept_read.ok())
        OA_CHECK(sector_six_count(kept_read.replay->recording) == 0);

    // Once sector 6 is left out, this recording is the one just written.
    const oademo::Replay clean_replay = sample_replay("Show Down");
    const std::vector<uint8_t> clean_demo = oademo_bytes(clean_replay);
    write_bytes(sources / "clean.oademo", clean_demo);
    const lib::ImportResult copied = lib::import_replay(recordings, sources / "clean.oademo");
    OA_CHECK(copied.ok && !copied.copied);
    OA_CHECK(copied.title == "Show Down");
    OA_CHECK(copied.path == "Imported/show-down.oademo");
    const lib::ImportResult kept_again = lib::import_replay(recordings, sources / "clean.oademo");
    OA_CHECK(kept_again.ok && !kept_again.copied);
    OA_CHECK(kept_again.path == copied.path);

    const oademo::Replay coast_replay = sample_replay("Coast");
    const std::vector<uint8_t> coast_demo = oademo_bytes(coast_replay);
    write_bytes(sources / "coast.oademo", coast_demo);
    const lib::ImportResult coast = lib::import_replay(recordings, sources / "coast.oademo");
    OA_CHECK(coast.ok && coast.copied);
    OA_CHECK(coast.title == "Coast");
    OA_CHECK(coast.path == "Imported/coast.oademo");
    OA_CHECK(file_bytes(recordings / "Imported" / "coast.oademo") == coast_demo);
}

/// A recorded replay goes the day after 30, 90 or 365 days, never under
/// forever, and a kept, imported, downloaded or exported one never goes.
void retention_keeps_the_sides_of_each_span() {
    lib::Replay replay;
    replay.path = "default/game.oademo";
    replay.how = "multiplayer";
    constexpr int32_t recorded = 10000;
    const auto goes = [&](lib::KeepSpan span, int32_t today) {
        return lib::due_to_go(replay, recorded, span, today);
    };
    OA_CHECK(!goes(lib::KeepSpan::days_30, recorded + 30));
    OA_CHECK(goes(lib::KeepSpan::days_30, recorded + 31));
    OA_CHECK(!goes(lib::KeepSpan::days_90, recorded + 90));
    OA_CHECK(goes(lib::KeepSpan::days_90, recorded + 91));
    OA_CHECK(!goes(lib::KeepSpan::one_year, recorded + 365));
    OA_CHECK(goes(lib::KeepSpan::one_year, recorded + 366));
    OA_CHECK(!goes(lib::KeepSpan::forever, recorded + 100000));
    OA_CHECK(!goes(lib::KeepSpan::days_30, recorded));
    OA_CHECK(!goes(lib::KeepSpan::days_30, recorded - 1));

    OA_CHECK(lib::keep_span_of("") == lib::KeepSpan::days_30);
    OA_CHECK(lib::keep_span_of("nope") == lib::KeepSpan::days_30);
    OA_CHECK(lib::keep_span_of(lib::keep_30_days) == lib::KeepSpan::days_30);
    OA_CHECK(lib::keep_span_of(lib::keep_90_days) == lib::KeepSpan::days_90);
    OA_CHECK(lib::keep_span_of(lib::keep_one_year) == lib::KeepSpan::one_year);
    OA_CHECK(lib::keep_span_of(lib::keep_forever) == lib::KeepSpan::forever);
    OA_CHECK(lib::keep_span_text(lib::KeepSpan::days_90) == lib::keep_90_days);
    OA_CHECK(lib::keep_span_days(lib::KeepSpan::days_30) == lib::keep_span_30);
    OA_CHECK(lib::keep_span_days(lib::KeepSpan::days_90) == lib::keep_span_90);
    OA_CHECK(lib::keep_span_days(lib::KeepSpan::one_year) == lib::keep_span_year);
    OA_CHECK(!lib::keep_span_days(lib::KeepSpan::forever).has_value());

    replay.kept = true;
    OA_CHECK(lib::retention_spares(replay));
    OA_CHECK(!goes(lib::KeepSpan::days_30, recorded + 1000));
    replay.kept = false;

    replay.path = "Imported/game.oademo";
    OA_CHECK(lib::imported(replay));
    OA_CHECK(!goes(lib::KeepSpan::days_30, recorded + 1000));
    replay.path = "default/game.oademo";
    replay.how = std::string(lib::how_imported);
    OA_CHECK(!goes(lib::KeepSpan::days_90, recorded + 1000));

    replay.how = "multiplayer";
    replay.path = "Core Prime/game.oademo";
    OA_CHECK(lib::downloaded(replay));
    OA_CHECK(!goes(lib::KeepSpan::one_year, recorded + 1000));
    replay.path = "default/game.oademo";
    replay.how = std::string(lib::how_core_prime);
    OA_CHECK(lib::downloaded(replay));
    OA_CHECK(!goes(lib::KeepSpan::days_30, recorded + 1000));

    replay.how = "multiplayer";
    replay.exported = true;
    OA_CHECK(lib::exported(replay));
    OA_CHECK(!goes(lib::KeepSpan::days_30, recorded + 1000));
    OA_CHECK(!goes(lib::KeepSpan::days_30, recorded + 400));
    replay.exported = false;
    OA_CHECK(!lib::exported(replay));
    OA_CHECK(goes(lib::KeepSpan::days_30, recorded + 31));
}

/// An exported replay is a mark in exported.json, by identity and by path
/// when the file's bytes no longer match, not a folder and not a how value.
void exported_marks_follow_the_recording(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "exported" / "Recordings";
    three_replays(recordings);
    lib::Index index = lib::scan(recordings, {});
    const lib::Replay& original = replay_at(index, "ridge/b.tad");
    lib::KeptMarks marks;
    OA_CHECK(lib::set_exported(marks, original, true));
    OA_CHECK(lib::save_exported(recordings, marks));
    const std::string text = lib::exported_text(marks);
    OA_CHECK(text.rfind("{\"replays_exported\":1,\"exported\":[", 0) == 0);
    const lib::ExportedRead read = lib::read_exported(recordings);
    OA_CHECK(!read.damaged && read.exported.marks.size() == 1);
    lib::Replay changed = original;
    changed.sha256 = "0123456789abcdef";
    std::vector<lib::Replay> one{changed};
    lib::KeptMarks loaded = read.exported;
    OA_CHECK(lib::apply_exported(loaded, one));
    OA_CHECK(!lib::apply_exported(loaded, one));
    OA_CHECK(one[0].exported);
    OA_CHECK(loaded.marks.size() == 2);
    OA_CHECK(lib::retention_spares(one[0]));
    OA_CHECK(!lib::due_to_go(one[0], 10000, lib::KeepSpan::days_30, 10000 + 400));
}

/// An exported mark keeps a replay past the span. A replay of the same age
/// with no mark still goes. A file that is not exported marks is damaged,
/// and reading it leaves the bytes where they are.
void an_exported_mark_keeps_a_replay_past_the_span(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "export-span" / "Recordings";
    three_replays(recordings);
    const lib::Index index = lib::scan(recordings, {});
    const lib::Replay marked_replay = replay_at(index, "ridge/b.tad");
    const lib::Replay plain = replay_at(index, "default/a.oademo");
    constexpr int32_t recorded = 10000;
    constexpr int32_t past = recorded + lib::keep_span_30 + 1;
    OA_CHECK(lib::due_to_go(marked_replay, recorded, lib::KeepSpan::days_30, past));
    OA_CHECK(lib::due_to_go(plain, recorded, lib::KeepSpan::days_30, past));

    lib::KeptMarks marks;
    OA_CHECK(lib::set_exported(marks, marked_replay, true));
    OA_CHECK(!lib::set_exported(marks, marked_replay, true));
    OA_CHECK(lib::save_exported(recordings, marks));
    const lib::ExportedRead read = lib::read_exported(recordings);
    OA_CHECK(!read.damaged && read.exported.marks.size() == 1);
    OA_CHECK(lib::is_kept(read.exported, marked_replay));
    OA_CHECK(!lib::is_kept(read.exported, plain));

    std::vector<lib::Replay> rows{marked_replay, plain};
    lib::KeptMarks loaded = read.exported;
    // The mark already names the file, so applying it changes nothing.
    OA_CHECK(!lib::apply_exported(loaded, rows));
    OA_CHECK(rows[0].exported && lib::exported(rows[0]));
    OA_CHECK(!rows[1].exported && !lib::exported(rows[1]));
    OA_CHECK(!lib::due_to_go(rows[0], recorded, lib::KeepSpan::days_30, past));
    OA_CHECK(lib::due_to_go(rows[1], recorded, lib::KeepSpan::days_30, past));
    OA_CHECK(lib::retention_spares(rows[0]));
    OA_CHECK(!lib::retention_spares(rows[1]));

    const std::string broken = "{\"replays_exported\":1,\"exported\":[";
    write_text(lib::exported_path(recordings), broken);
    const lib::ExportedRead damaged = lib::read_exported(recordings);
    OA_CHECK(damaged.damaged && damaged.exported.marks.empty());
    const std::vector<uint8_t> kept_bytes = file_bytes(lib::exported_path(recordings));
    OA_CHECK(kept_bytes.size() == broken.size());
    if (kept_bytes.size() == broken.size() && !kept_bytes.empty()) {
        const std::string kept_text(
            reinterpret_cast<const char*>(kept_bytes.data()), kept_bytes.size()
        );
        OA_CHECK(kept_text == broken);
    }
}

/// While a damaged kept.json is set aside, every replay recorded by the time
/// it was set aside stays, however old: a mark cut off with it cannot be
/// known. A replay recorded later goes by age, and so does every replay once
/// no set-aside copy is there.
void a_set_aside_kept_file_spares_what_it_may_have_kept(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "set-aside" / "Recordings";
    write_text(lib::index_path(recordings), "an index set aside does not count");
    OA_CHECK(lib::set_aside(lib::index_path(recordings)).has_value());
    OA_CHECK(!lib::kept_set_aside_at(recordings).has_value());
    OA_CHECK(!lib::kept_set_aside_at(scratch.path() / "nowhere").has_value());

    lib::Replay replay;
    replay.path = "default/game.tad";
    replay.recorded = 1000;
    OA_CHECK(!lib::spared_by_set_aside(replay, std::nullopt));

    // A kept.json last written 400 days ago is dated when it is set aside.
    const fs::path kept = lib::kept_path(recordings);
    write_text(kept, "{\"replays_kept\":1,\"kept\":[{\"sha256\":\"ab");
    std::error_code dated;
    fs::last_write_time(kept, fs::last_write_time(kept) - std::chrono::hours(24 * 400), dated);
    OA_CHECK(!dated);
    const auto before = static_cast<int64_t>(std::time(nullptr));
    const std::optional<fs::path> first = lib::set_aside(kept);
    const auto after = static_cast<int64_t>(std::time(nullptr));
    OA_CHECK(first.has_value());
    const std::optional<int64_t> at = lib::kept_set_aside_at(recordings);
    OA_CHECK(at.has_value());
    if (!at.has_value())
        return;
    // Two seconds either way for a file system that keeps coarse times.
    OA_CHECK(*at >= before - 2 && *at <= after + 2);

    // Recorded at or before that second: it stays, however old. After it:
    // the rule judges it by age as before.
    replay.recorded = *at - 400LL * 86400LL;
    OA_CHECK(lib::spared_by_set_aside(replay, at));
    replay.recorded = *at;
    OA_CHECK(lib::spared_by_set_aside(replay, at));
    replay.recorded = *at + 1;
    OA_CHECK(!lib::spared_by_set_aside(replay, at));
    lib::Replay long_ago = replay;
    long_ago.recorded = *at - 400LL * 86400LL;
    constexpr int32_t recorded_day = 10000;
    OA_CHECK(lib::due_to_go(long_ago, recorded_day, lib::KeepSpan::days_30, recorded_day + 400));
    OA_CHECK(lib::spared_by_set_aside(long_ago, lib::kept_set_aside_at(recordings)));

    // The newest of several copies counts, whatever their numbers.
    if (first.has_value()) {
        fs::last_write_time(
            *first, fs::last_write_time(*first) - std::chrono::hours(24 * 10), dated
        );
        OA_CHECK(!dated);
    }
    OA_CHECK(lib::kept_set_aside_at(recordings) == *at - 10LL * 86400LL);
    write_text(kept, "damaged again");
    const std::optional<fs::path> second = lib::set_aside(kept);
    OA_CHECK(second.has_value() && second->filename() == "kept.json.damaged-2");
    const std::optional<int64_t> newest = lib::kept_set_aside_at(recordings);
    OA_CHECK(newest.has_value() && *newest >= before - 2);
    if (second.has_value()) {
        fs::last_write_time(
            *second, fs::last_write_time(*second) - std::chrono::hours(24 * 20), dated
        );
        OA_CHECK(!dated);
    }
    OA_CHECK(lib::kept_set_aside_at(recordings) == *at - 10LL * 86400LL);

    // Once no copy is there, nothing is spared by one.
    std::error_code removed;
    if (first.has_value())
        fs::remove(*first, removed);
    if (second.has_value())
        fs::remove(*second, removed);
    OA_CHECK(!lib::kept_set_aside_at(recordings).has_value());
    OA_CHECK(!lib::spared_by_set_aside(long_ago, lib::kept_set_aside_at(recordings)));
}

void a_multibyte_title_is_cut_on_a_character() {
    std::string title = "a";
    for (int count = 0; count < 80; ++count)
        title += "\xC3\xA9";
    const std::string stem = lib::recording_stem(title);
    OA_CHECK(stem.size() == 119);
    OA_CHECK(static_cast<unsigned char>(stem[stem.size() - 2]) == 0xC3);
    OA_CHECK(static_cast<unsigned char>(stem.back()) == 0xA9);
    OA_CHECK(lib::recording_stem("North Reach.oademo") == "North Reach");
    OA_CHECK(lib::recording_stem("CON") == "CON_");
    OA_CHECK(lib::recording_stem("") == "replay");
    OA_CHECK(lib::recording_stem("../../../../escaped") == "_.._.._.._escaped");
}

/// A campaign's mission name, including a trailing space, is what a scan stores.
///
/// @param scratch the test's folder
void a_campaign_keeps_its_mission_name(const Scratch& scratch) {
    const fs::path recordings = scratch.path() / "campaign" / "Recordings";
    oademo::Replay made = sample_replay("Rougpelt");
    made.meta.game.kind = "campaign";
    made.meta.game.kind_object = "{\"mission_name\":\"Core Contingency \"}";
    write_bytes(recordings / "default" / "campaign.oademo", oademo_bytes(made));
    const lib::Index scanned = lib::scan(recordings, {});
    OA_CHECK(scanned.replays.size() == 1);
    if (scanned.replays.size() != 1)
        return;
    OA_CHECK(scanned.replays[0].kind == lib::kind_campaign);
    OA_CHECK(scanned.replays[0].map == "Rougpelt");
    OA_CHECK(scanned.replays[0].mission_name == "Core Contingency ");
    OA_CHECK(scanned.replays[0].mission_name.back() == ' ');
    // An unchanged file keeps the name, and a timeline written onto the index.
    lib::Index stored = scanned;
    stored.replays[0].timeline = std::string("{\"markers\":[]}");
    stored.replays[0].watched_tick = 21000;
    const lib::Index again = lib::scan(recordings, stored);
    OA_CHECK(same_index(again, stored));
}

} // namespace

void test_replay_content_rows();
void test_replay_feed();
void test_replay_share();

int main() {
    const Scratch scratch;
    a_scan_lists_and_reads_only_the_replays(scratch);
    the_index_reads_back_as_written(scratch);
    a_campaign_keeps_its_mission_name(scratch);
    a_deleted_index_is_rebuilt(scratch);
    kept_marks_outlive_the_index(scratch);
    a_tad_header_is_read();
    damaged_files_are_listed_damaged(scratch);
    kinds_register_by_id();
    days_group_a_list();
    an_opened_replay_is_imported(scratch);
    retention_keeps_the_sides_of_each_span();
    exported_marks_follow_the_recording(scratch);
    an_exported_mark_keeps_a_replay_past_the_span(scratch);
    a_set_aside_kept_file_spares_what_it_may_have_kept(scratch);
    test_replay_content_rows();
    test_replay_feed();
    test_replay_share();
    a_multibyte_title_is_cut_on_a_character();
    return oa::test::check_exit_status();
}
