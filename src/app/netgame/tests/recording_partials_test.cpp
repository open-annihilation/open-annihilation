// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How a game is recorded and where, and how a recording a crash left is finished: the plan
// from the recorder, the command line and the player's setting; the files of a recording
// being made and the lock a game holds on them; a replay finished after a crash, with its
// content note; the recorder hack's `.tad` moved as it is; recordings that hold nothing or
// cannot be finished; the files a finish leaves behind; and a game still being played, whose
// recording a start leaves alone.

#include "recording_partials.hpp"

#include "oa/app/user_folder.hpp"
#include "oa/core/player_setup.h"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"
#include "oa/session/demo/recording.hpp"
#include "oa/session/demo/recording_stream.hpp"
#include "oa/session/demo/replay_record.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace demo = oa::session::demo;
using namespace oa::app;

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
        }                                                                                          \
    } while (0)

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Lists the files below a folder of a folder, relative to it; none when it is not there.
///
/// @param root the folder
/// @param below its folder to read
/// @return the files' paths, sorted
std::vector<std::string> files_below(const fs::path& root, const fs::path& below) {
    std::vector<std::string> found;
    std::error_code error;
    if (!fs::is_directory(root / below, error))
        return found;
    for (fs::recursive_directory_iterator entry(root / below, error), end; !error && entry != end;
         entry.increment(error))
        if (entry->is_regular_file())
            found.push_back(entry->path().lexically_relative(root).generic_string());
    std::sort(found.begin(), found.end());
    return found;
}

/// A recording of two players with some packets, streamed to a file.
///
/// @param path the stream file
/// @param packets how many camera packets to add
/// @return the recording's setup, for a replay's description
demo::RecordingSetup make_stream(const fs::path& path, uint32_t packets) {
    demo::RecordingSetup setup{};
    setup.max_units = 20;
    setup.map_name = "Test Map";
    setup.recorder_text = "Program 1.0";
    setup.date_text = "2026-11-06 10:00";
    for (uint32_t index = 0; index < 2; ++index) {
        demo::RecordingPlayer player{};
        player.player_id = 0x100 * (index + 1);
        player.name = index == 0 ? "Ada" : "Bo";
        player.info.side = static_cast<uint8_t>(index);
        player.info.color = static_cast<uint8_t>(index);
        player.team = 3;
        setup.players.push_back(player);
    }
    demo::DemoRecording recording;
    demo::recording_begin(&recording, setup);
    std::string error;
    CHECK(demo::recording_stream_to(&recording, path, &error));
    const uint8_t camera[] = {0xfc, 0x00, 0x01, 0xd0, 0x00};
    for (uint32_t index = 0; index < packets; ++index)
        demo::recording_add(&recording, 0x100, 1000 + 33ull * index, camera);
    CHECK(recording.stream->close(&error));
    return setup;
}

/// A recording's files in the folder of one mod's recordings being made, as a game that
/// stopped without finishing leaves them.
///
/// @param user the player's own folder
/// @param mod_id the mod's id; empty for none
/// @param name the file the recording becomes
/// @param packets how many packets it holds
/// @param replay whether it is a replay, with its description beside it
/// @return the files
PartialFiles left_by_a_crash(
    const fs::path& user,
    std::string_view mod_id,
    const std::string& name,
    uint32_t packets,
    bool replay
) {
    std::string problem;
    auto partial = partial_make(
        user, mod_id, name, replay ? RecordingLayout::replay : RecordingLayout::hack_tad, &problem
    );
    CHECK(partial != nullptr);
    if (partial == nullptr)
        return {};
    const PartialFiles files = partial->files;
    const demo::RecordingSetup setup = make_stream(files.stream, packets);
    if (replay) {
        demo::ReplayFacts facts;
        facts.recorded_at = "2026-11-06T00:00:00Z";
        facts.engine_version = "0.9.1";
        facts.map_name = "Test Map";
        facts.players = {{"Ada", "human", true}, {"Bo", "human", false}};
        CHECK(partial_write_start(files, demo::replay_begin(setup, facts)));
    }
    CHECK(partial_write_note(files, "base-game: totala\nexpansions: \nmod: \n"));
    // The process is gone, and with it the hold on the lock.
    partial.reset();
    return files;
}

void the_plan_follows_the_recorder_the_files_and_the_setting() {
    const fs::path none;
    const fs::path tad = "game.tad";
    const fs::path replay = "game.oademo";
    // Nothing asks: a game is not recorded, a headless run or a check never by the setting.
    CHECK(plan_recording(false, none, none, false).layout == RecordingLayout::none);
    // The setting records a replay; the recorder hack records its own layout, never both.
    CHECK(plan_recording(false, none, none, true).layout == RecordingLayout::replay);
    CHECK(plan_recording(true, none, none, false).layout == RecordingLayout::hack_tad);
    CHECK(plan_recording(true, none, none, true).layout == RecordingLayout::hack_tad);
    CHECK(!plan_recording(true, none, none, true).named);
    // --net-record keeps the hack's layout, whatever the file's name says.
    const RecordingPlan net = plan_recording(false, replay, none, true);
    CHECK(net.layout == RecordingLayout::hack_tad && net.named);
    // --record-game writes a replay, or the hack's layout for a name that ends in .tad, in any case.
    const RecordingPlan game = plan_recording(false, none, replay, false);
    CHECK(game.layout == RecordingLayout::replay && game.named);
    CHECK(plan_recording(true, none, replay, false).layout == RecordingLayout::replay);
    CHECK(plan_recording(false, none, tad, false).layout == RecordingLayout::hack_tad);
    CHECK(plan_recording(false, none, "GAME.TAD", true).layout == RecordingLayout::hack_tad);
    CHECK(plan_recording(false, tad, replay, false).layout == RecordingLayout::replay);
}

void an_unrecorded_game_forgets_the_previous_file() {
    const fs::path previous = "Recordings/previous.oademo";
    CHECK(remembered_recording_path(RecordingLayout::none, previous).empty());
    const fs::path next = "Recordings/next.oademo";
    CHECK(remembered_recording_path(RecordingLayout::replay, next) == next);
    CHECK(remembered_recording_path(RecordingLayout::hack_tad, next) == next);
}

void the_files_of_a_recording_are_named_together_and_locked() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    std::string problem;
    auto first = partial_make(
        user, "ridge", "2026-11-06 1000 Test Map.oademo", RecordingLayout::replay, &problem
    );
    CHECK(first != nullptr);
    if (first == nullptr)
        return;
    const fs::path folder = user / "Recordings" / ".replays" / "partial" / "ridge";
    CHECK(first->files.stream == folder / "2026-11-06 1000 Test Map.oademo.part");
    CHECK(first->files.start == folder / "2026-11-06 1000 Test Map.oademo.start");
    CHECK(first->files.note == folder / "2026-11-06 1000 Test Map.oademo.note");
    CHECK(first->files.lock_file == folder / "2026-11-06 1000 Test Map.oademo.lock");
    CHECK(partial_files_of(first->files.stream).start == first->files.start);
    // Nothing of the game's own folders is made: a game with no mod is in default.
    auto plain = partial_make(user, {}, "a.tad", RecordingLayout::hack_tad, &problem);
    CHECK(plain != nullptr && plain->files.stream.parent_path().filename() == "default");
    // The stream's name says its layout, whatever the file is called: a replay's ends
    // .oademo.part and the recorder hack's .tad.part.
    auto bare = partial_make(user, {}, "no extension", RecordingLayout::replay, &problem);
    CHECK(bare != nullptr && bare->files.stream.filename() == "no extension.oademo.part");
    auto odd = partial_make(user, {}, "other.rec", RecordingLayout::replay, &problem);
    CHECK(odd != nullptr && odd->files.stream.filename() == "other.rec.oademo.part");
    auto hack = partial_make(user, {}, "hack.rec", RecordingLayout::hack_tad, &problem);
    CHECK(hack != nullptr && hack->files.stream.filename() == "hack.rec.tad.part");
    CHECK(plain != nullptr && plain->files.stream.filename() == "a.tad.part");
    CHECK(partial_make(user, {}, "x", RecordingLayout::none, &problem) == nullptr);
    // A name taken by a recording not yet finished is not used twice.
    write_text(first->files.stream, "stream");
    auto second = partial_make(
        user, "ridge", "2026-11-06 1000 Test Map.oademo", RecordingLayout::replay, &problem
    );
    CHECK(second != nullptr);
    CHECK(
        second != nullptr &&
        second->files.stream.filename() == "2026-11-06 1000 Test Map (2).oademo.part"
    );
    std::error_code removed;
    fs::remove_all(scratch, removed);
}

void a_replay_a_crash_left_is_finished_with_its_note() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    const PartialFiles files = left_by_a_crash(user, "ridge", "game one.oademo", 6, true);
    // The stream was cut part way through its last packet.
    auto bytes = read_bytes(files.stream);
    bytes.resize(bytes.size() - 2);
    {
        std::ofstream out(files.stream, std::ios::binary | std::ios::trunc);
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    }
    const PartialsFinished done = finish_partial_recordings(user);
    CHECK(done.finished == 1 && done.empty == 0 && done.failed == 0 && done.lines.size() == 1);
    const fs::path placed = user / "Recordings" / "ridge" / "game one.oademo";
    CHECK(fs::exists(placed));
    const auto replay = oa::formats::oademo::read(read_bytes(placed));
    CHECK(replay.ok());
    if (replay.ok()) {
        const auto& meta = replay.replay->meta;
        CHECK(meta.recorded.after_crash && meta.game.ended == "cut");
        CHECK(meta.recorded.at == "2026-11-06T00:00:00Z" && meta.recording.packets == 5);
        CHECK(meta.players.size() == 2 && meta.players[0].name == "Ada");
        const auto inside = oa::formats::tad::parse(replay.replay->recording);
        CHECK(inside.ok() && inside.demo->packets.size() == 5);
    }
    // The content note came along, and nothing of the recording stays where it was made.
    CHECK(fs::exists(user / "Recordings" / "ridge" / "game one.oademo.oa-content"));
    for (const fs::path& left : {files.stream, files.start, files.note, files.lock_file})
        CHECK(!fs::exists(left));
    // A second start finds nothing, and changes nothing.
    const PartialsFinished again = finish_partial_recordings(user);
    CHECK(again.finished == 0 && again.empty == 0 && again.failed == 0 && again.lines.empty());
    std::error_code removed;
    fs::remove_all(scratch, removed);
}

void a_layout_is_told_by_the_streams_name() {
    CHECK(partial_layout_of("a.oademo.part") == RecordingLayout::replay);
    CHECK(partial_layout_of("A B (2).OADEMO.PART") == RecordingLayout::replay);
    CHECK(partial_layout_of("a.tad.part") == RecordingLayout::hack_tad);
    CHECK(partial_layout_of(fs::path("dir") / "a.TAD.part") == RecordingLayout::hack_tad);
    CHECK(partial_layout_of("a.part") == RecordingLayout::none);
    CHECK(partial_layout_of("a.oademo") == RecordingLayout::none);
    CHECK(partial_layout_of("a.tad.part.failed") == RecordingLayout::none);
}

void a_replay_without_its_description_is_never_placed() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    const PartialFiles files = left_by_a_crash(user, {}, "lost start.oademo", 4, true);
    CHECK(fs::exists(files.start));
    // The description is gone, as a write that failed or a crash between the two writes leaves it.
    std::error_code removed;
    fs::remove(files.start, removed);
    const PartialsFinished done = finish_partial_recordings(user);
    CHECK(done.finished == 0 && done.failed == 1 && done.empty == 0 && done.lines.size() == 1);
    CHECK(
        done.lines.size() == 1 && done.lines[0].find("description is missing") != std::string::npos
    );
    // Nothing of it is in Recordings, as a recorder's file or as anything else.
    CHECK(!fs::exists(user / "Recordings" / "default" / "lost start.oademo"));
    CHECK(files_below(user / "Recordings", "default").empty());
    // The stream is kept under a name no start tries again, and a second start finds nothing.
    CHECK(!fs::exists(files.stream) && fs::exists(fs::path(files.stream.string() + ".failed")));
    const PartialsFinished again = finish_partial_recordings(user);
    CHECK(again.finished == 0 && again.failed == 0 && again.lines.empty());
    CHECK(fs::exists(fs::path(files.stream.string() + ".failed")));
    fs::remove_all(scratch, removed);
}

void a_replay_whose_name_has_no_extension_is_still_a_replay() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    const PartialFiles files = left_by_a_crash(user, {}, "no extension", 4, true);
    CHECK(files.stream.filename() == "no extension.oademo.part");
    const PartialsFinished done = finish_partial_recordings(user);
    CHECK(done.finished == 1 && done.failed == 0);
    const fs::path placed = user / "Recordings" / "default" / "no extension.oademo";
    const auto replay = oa::formats::oademo::read(read_bytes(placed));
    CHECK(replay.ok());
    CHECK(replay.ok() && replay.replay->meta.recorded.after_crash);
    // The recorder hack's layout, whatever its extension, ends .tad once finished and is moved
    // as it is, with no description beside it.
    const PartialFiles hack = left_by_a_crash(user, {}, "hack.rec", 3, false);
    CHECK(hack.stream.filename() == "hack.rec.tad.part" && !fs::exists(hack.start));
    const auto whole = read_bytes(hack.stream);
    CHECK(finish_partial_recordings(user).finished == 1);
    CHECK(read_bytes(user / "Recordings" / "default" / "hack.rec.tad") == whole);
    std::error_code removed;
    fs::remove_all(scratch, removed);
}

void a_stream_whose_name_says_no_layout_is_kept_apart() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    const fs::path folder = user / "Recordings" / ".replays" / "partial" / "default";
    write_text(folder / "old.part", "a stream of an earlier layout of the names");
    const PartialsFinished done = finish_partial_recordings(user);
    CHECK(done.finished == 0 && done.failed == 1);
    CHECK(!fs::exists(user / "Recordings" / "default"));
    CHECK(fs::exists(folder / "old.part.failed"));
    std::error_code removed;
    fs::remove_all(scratch, removed);
}

void a_description_that_cannot_be_written_is_refused() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    std::string problem;
    auto partial = partial_make(user, {}, "refused.oademo", RecordingLayout::replay, &problem);
    CHECK(partial != nullptr);
    if (partial == nullptr)
        return;
    demo::RecordingSetup setup{};
    setup.max_units = 20;
    demo::RecordingPlayer player{};
    player.player_id = 0x100;
    player.name = "Ada";
    setup.players.push_back(player);
    const auto start = demo::replay_begin(setup, demo::ReplayFacts{});
    // Where the description would go there is a folder, as a read-only or a full disk refuses.
    fs::create_directories(partial->files.start);
    CHECK(!partial_write_start(partial->files, start));
    std::error_code removed;
    fs::remove_all(partial->files.start, removed);
    CHECK(partial_write_start(partial->files, start));
    CHECK(fs::exists(partial->files.start));
    // Giving the recording up leaves none of its files, and frees the name for the next.
    partial_discard(*partial);
    partial.reset();
    CHECK(files_below(user / "Recordings", ".replays").empty());
    fs::remove_all(scratch, removed);
}

void a_disk_that_refuses_leaves_the_recording_for_a_later_start() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    const PartialFiles files = left_by_a_crash(user, {}, "later.tad", 4, false);
    const auto whole = read_bytes(files.stream);
    // Where the recording would go there is a file, so that it cannot be moved there.
    write_text(user / "Recordings" / "default", "not a folder");
    const PartialsFinished refused = finish_partial_recordings(user);
    CHECK(refused.finished == 0 && refused.failed == 0 && refused.postponed == 1);
    CHECK(fs::exists(files.stream) && !fs::exists(fs::path(files.stream.string() + ".failed")));
    // A later start, with the disk as it should be, finishes it.
    std::error_code removed;
    fs::remove(user / "Recordings" / "default", removed);
    const PartialsFinished later = finish_partial_recordings(user);
    CHECK(later.finished == 1 && later.postponed == 0);
    CHECK(read_bytes(user / "Recordings" / "default" / "later.tad") == whole);
    fs::remove_all(scratch, removed);
}

void the_hack_layout_is_moved_as_it_is() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    const PartialFiles files = left_by_a_crash(user, {}, "hack.tad", 4, false);
    const auto whole = read_bytes(files.stream);
    const PartialsFinished done = finish_partial_recordings(user);
    CHECK(done.finished == 1 && done.failed == 0);
    const fs::path placed = user / "Recordings" / "default" / "hack.tad";
    CHECK(read_bytes(placed) == whole);
    CHECK(!fs::exists(files.stream) && !fs::exists(files.note) && !fs::exists(files.lock_file));
    // A name taken in Recordings is never replaced: the second takes " (2)".
    const PartialFiles next = left_by_a_crash(user, {}, "hack.tad", 3, false);
    const auto next_whole = read_bytes(next.stream);
    CHECK(finish_partial_recordings(user).finished == 1);
    CHECK(read_bytes(placed) == whole);
    CHECK(read_bytes(user / "Recordings" / "default" / "hack (2).tad") == next_whole);
    std::error_code removed;
    fs::remove_all(scratch, removed);
}

void nothing_recorded_is_dropped_and_what_cannot_be_finished_is_kept_apart() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    const PartialFiles empty = left_by_a_crash(user, {}, "empty.oademo", 0, true);
    // A replay whose description is lost cannot be finished.
    const PartialFiles lost = left_by_a_crash(user, {}, "lost.oademo", 3, true);
    write_text(lost.start, "not a replay");
    const PartialsFinished done = finish_partial_recordings(user);
    CHECK(done.finished == 0 && done.empty == 1 && done.failed == 1 && done.lines.size() == 2);
    CHECK(!fs::exists(empty.stream) && !fs::exists(empty.start));
    CHECK(!fs::exists(user / "Recordings" / "default" / "empty.oademo"));
    CHECK(!fs::exists(user / "Recordings" / "default" / "lost.oademo"));
    // Its stream is kept under a name no start tries again.
    CHECK(!fs::exists(lost.stream) && fs::exists(fs::path(lost.stream.string() + ".failed")));
    CHECK(finish_partial_recordings(user).lines.empty());
    std::error_code removed;
    fs::remove_all(scratch, removed);
}

void a_game_still_being_played_is_left_alone() {
    const fs::path scratch = oa::test::make_scratch_directory("netgame-partials");
    const fs::path user = scratch / "user";
    std::string problem;
    auto live = partial_make(user, {}, "live.oademo", RecordingLayout::replay, &problem);
    CHECK(live != nullptr);
    if (live == nullptr)
        return;
    const demo::RecordingSetup setup = make_stream(live->files.stream, 5);
    demo::ReplayFacts facts;
    CHECK(partial_write_start(live->files, demo::replay_begin(setup, facts)));
    // The game holds its lock, in this process as in another: its recording is not finished.
    const PartialsFinished done = finish_partial_recordings(user);
    CHECK(done.finished == 0 && done.empty == 0 && done.failed == 0 && done.lines.empty());
    CHECK(fs::exists(live->files.stream) && fs::exists(live->files.start));
    CHECK(!fs::exists(user / "Recordings" / "default" / "live.oademo"));
    // Files without a stream, from a finish that stopped, are removed; a game's own are not.
    write_text(live->files.stream.parent_path() / "gone.oademo.start", "left");
    write_text(live->files.stream.parent_path() / "gone.oademo.note", "left");
    CHECK(finish_partial_recordings(user).lines.empty());
    CHECK(!fs::exists(live->files.stream.parent_path() / "gone.oademo.start"));
    CHECK(!fs::exists(live->files.stream.parent_path() / "gone.oademo.note"));
    CHECK(fs::exists(live->files.start));
    // A folder that holds no recording being made is not touched.
    const fs::path other = scratch / "other";
    fs::create_directories(other);
    CHECK(finish_partial_recordings(other).lines.empty());
    CHECK(fs::is_empty(other));
    std::error_code removed;
    live.reset();
    fs::remove_all(scratch, removed);
}

} // namespace

int main() {
    the_plan_follows_the_recorder_the_files_and_the_setting();
    an_unrecorded_game_forgets_the_previous_file();
    the_files_of_a_recording_are_named_together_and_locked();
    a_replay_a_crash_left_is_finished_with_its_note();
    a_layout_is_told_by_the_streams_name();
    a_replay_without_its_description_is_never_placed();
    a_replay_whose_name_has_no_extension_is_still_a_replay();
    a_stream_whose_name_says_no_layout_is_kept_apart();
    a_description_that_cannot_be_written_is_refused();
    a_disk_that_refuses_leaves_the_recording_for_a_later_start();
    the_hack_layout_is_moved_as_it_is();
    nothing_recorded_is_dropped_and_what_cannot_be_finished_is_kept_apart();
    a_game_still_being_played_is_left_alone();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("recording partials: all tests passed");
    return 0;
}
