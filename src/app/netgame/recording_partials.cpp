// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "recording_partials.hpp"

#include "oa/app/user_folder.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace oa::app {

namespace {

namespace fs = std::filesystem;
namespace demo = oa::session::demo;

using oa::platform::path_from_utf8;
using oa::platform::path_to_utf8;

/// Tells whether a name ends with another.
///
/// @param name the name
/// @param suffix the ending
/// @return true when it does
bool ends_with(std::string_view name, std::string_view suffix) noexcept {
    return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
}

/// Returns a name with its ASCII letters in lower case.
///
/// @param name the name
/// @return the name, lower-cased
std::string lowered(std::string_view name) {
    std::string made(name);
    for (char& character : made)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return made;
}

/// Tells whether a file name ends in `.tad`, whatever the case.
///
/// @param path the file
/// @return true when its extension is .tad
bool names_a_tad(const fs::path& path) {
    std::string extension = path_to_utf8(path.extension());
    for (char& character : extension)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return extension == ".tad";
}

/// Returns a path with letters added to its name.
///
/// @param path the path
/// @param suffix the letters, ASCII
/// @return the path with the letters after its last name
fs::path with_suffix(const fs::path& path, std::string_view suffix) {
    fs::path made = path;
    made += fs::path(std::string(suffix));
    return made;
}

/// Writes a file whole.
///
/// @param path the file; replaced
/// @param bytes what it holds
/// @return false when it could not be written
bool write_whole(const fs::path& path, const char* bytes, std::size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (size != 0)
        out.write(bytes, static_cast<std::streamsize>(size));
    out.flush();
    return static_cast<bool>(out);
}

/// Reads a file whole.
///
/// @param path the file
/// @param[out] bytes what it holds
/// @return false when it cannot be read
bool read_whole(const fs::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

/// Puts a file where it goes, replacing what is there.
///
/// @param from the file
/// @param to where it goes
void move_replacing(const fs::path& from, const fs::path& to) {
    std::error_code failure;
    fs::remove(to, failure);
    failure.clear();
    fs::rename(from, to, failure);
    if (!failure)
        return;
    failure.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, failure);
    if (!failure)
        fs::remove(from, failure);
}

/// Removes the files of a recording that is finished or dropped, the lock file apart.
///
/// @param files the recording's files
void remove_files(const PartialFiles& files) {
    std::error_code failure;
    fs::remove(files.stream, failure);
    fs::remove(with_suffix(files.stream, ".writing"), failure);
    fs::remove(files.start, failure);
    fs::remove(files.note, failure);
}

} // namespace

RecordingPlan plan_recording(
    bool recorder_protocol, const fs::path& net_record, const fs::path& record_game, bool by_setting
) {
    RecordingPlan plan;
    if (!record_game.empty()) {
        plan.layout =
            names_a_tad(record_game) ? RecordingLayout::hack_tad : RecordingLayout::replay;
        plan.named = true;
    } else if (!net_record.empty()) {
        plan.layout = RecordingLayout::hack_tad;
        plan.named = true;
    } else if (recorder_protocol) {
        plan.layout = RecordingLayout::hack_tad;
    } else if (by_setting) {
        plan.layout = RecordingLayout::replay;
    }
    return plan;
}

RecordingLayout partial_layout_of(const fs::path& stream) {
    const std::string name = lowered(path_to_utf8(stream.filename()));
    if (!ends_with(name, partial_stream_suffix))
        return RecordingLayout::none;
    const std::string_view before =
        std::string_view(name).substr(0, name.size() - partial_stream_suffix.size());
    if (ends_with(before, partial_replay_extension))
        return RecordingLayout::replay;
    if (ends_with(before, partial_tad_extension))
        return RecordingLayout::hack_tad;
    return RecordingLayout::none;
}

PartialFiles partial_files_of(const fs::path& stream) {
    PartialFiles files;
    files.stream = stream;
    // The name without its last suffix, partial_stream_suffix.
    const fs::path folder = stream.parent_path();
    const fs::path base = stream.stem();
    files.start = folder / with_suffix(base, partial_start_suffix);
    files.note = folder / with_suffix(base, partial_note_suffix);
    files.lock_file = folder / with_suffix(base, partial_lock_suffix);
    return files;
}

std::unique_ptr<PartialRecording> partial_make(
    const fs::path& user_folder,
    std::string_view mod_id,
    const fs::path& final_name,
    RecordingLayout layout,
    std::string* error
) {
    const auto fail = [error](std::string reason) {
        if (error != nullptr)
            *error = std::move(reason);
        return std::unique_ptr<PartialRecording>();
    };
    if (layout != RecordingLayout::replay && layout != RecordingLayout::hack_tad)
        return fail("the recording has no layout");
    const std::string_view layout_extension =
        layout == RecordingLayout::replay ? partial_replay_extension : partial_tad_extension;
    const fs::path folder = recordings_partial_folder(user_folder, mod_id);
    std::error_code failure;
    fs::create_directories(folder, failure);
    if (failure)
        return fail("the folder for recordings being made cannot be made: " + failure.message());
    const std::string stem = path_to_utf8(final_name.stem());
    const std::string extension = path_to_utf8(final_name.extension());
    for (int copy = 1; copy < 1000; ++copy) {
        std::string name =
            copy == 1 ? stem + extension : stem + " (" + std::to_string(copy) + ")" + extension;
        // The stream's name ends with its layout's extension, whatever the file is called.
        if (!ends_with(lowered(name), layout_extension))
            name.append(layout_extension);
        auto partial = std::make_unique<PartialRecording>();
        partial->files =
            partial_files_of(folder / with_suffix(path_from_utf8(name), partial_stream_suffix));
        if (fs::exists(partial->files.stream, failure) ||
            fs::exists(partial->files.start, failure) || fs::exists(partial->files.note, failure))
            continue;
        partial->lock = oa::app::package_install::FileLock::take(partial->files.lock_file);
        if (partial->lock == nullptr)
            continue;
        return partial;
    }
    return fail("no name for the recording being made is free");
}

bool partial_write_start(const PartialFiles& files, const oa::formats::oademo::Replay& start) {
    const oa::formats::oademo::WriteResult written = oa::formats::oademo::write(start);
    if (!written.ok())
        return false;
    return write_whole(
        files.start, reinterpret_cast<const char*>(written.bytes.data()), written.bytes.size()
    );
}

void partial_discard(PartialRecording& partial) {
    remove_files(partial.files);
    partial.lock.reset();
    std::error_code failure;
    fs::remove(partial.files.lock_file, failure);
}

bool partial_write_note(const PartialFiles& files, std::string_view note) {
    if (note.empty())
        return true;
    return write_whole(files.note, note.data(), note.size());
}

demo::FinishResult finish_partial(const PartialFinish& finish) {
    demo::FinishResult result =
        finish.replay_layout
            ? demo::finish_as_replay(
                  finish.files.stream, finish.start, finish.end, finish.destination, finish.replace
              )
            : demo::finish_as_tad(
                  finish.files.stream, finish.destination, !finish.cut, finish.replace
              );
    if (result.empty) {
        remove_files(finish.files);
        return result;
    }
    if (!result.ok)
        return result;
    std::error_code failure;
    if (fs::exists(finish.files.note, failure)) {
        fs::path note = result.placed;
        note += fs::path(std::string(oa::data::saves::content_note_suffix));
        move_replacing(finish.files.note, note);
    }
    remove_files(finish.files);
    return result;
}

namespace {

/// Finishes one recording a crash left.
///
/// @param user_folder the player's own folder
/// @param stream the stream file, in Recordings/.replays/partial/<mod folder>
/// @param[in,out] done what is done so far
void finish_one(const fs::path& user_folder, const fs::path& stream, PartialsFinished& done) {
    const PartialFiles files = partial_files_of(stream);
    std::unique_ptr<oa::app::package_install::FileLock> lock =
        oa::app::package_install::FileLock::take(files.lock_file);
    // A lock another game holds belongs to a game still being played.
    if (lock == nullptr)
        return;
    const std::string name = path_to_utf8(stream.stem());
    std::error_code failure;
    // Puts the stream aside, under a name no start tries again, and gives up the lock.
    const auto keep_aside = [&](std::string reason) {
        done.failed += 1;
        done.lines.push_back(
            "the recording " + name + " that a crash left cannot be finished: " + reason
        );
        fs::rename(stream, with_suffix(stream, partial_failed_suffix), failure);
        lock.reset();
        fs::remove(files.lock_file, failure);
    };
    // The layout is the stream's name's, which no lost file can change.
    const RecordingLayout layout = partial_layout_of(stream);
    if (layout == RecordingLayout::none) {
        keep_aside("its name does not say whether it is a replay or a recorder's file");
        return;
    }
    // Nothing in Recordings is replaced: the file takes the next free name.
    const fs::path destination = user_folder / path_from_utf8(std::string(recordings_folder_name)) /
                                 stream.parent_path().filename() / path_from_utf8(name);
    PartialFinish finish;
    finish.files = files;
    finish.destination = destination;
    finish.replace = false;
    finish.cut = true;
    if (layout == RecordingLayout::replay) {
        // A replay is never finished without its description: the stream alone is the recorder's
        // layout, which a replay's name would then misname.
        std::vector<uint8_t> bytes;
        if (!fs::exists(files.start, failure)) {
            keep_aside("its description is missing");
            return;
        }
        if (!read_whole(files.start, bytes)) {
            done.postponed += 1;
            done.lines.push_back(
                "the recording " + name +
                " that a crash left waits for a later start: its "
                "description cannot be read now"
            );
            lock.reset();
            fs::remove(files.lock_file, failure);
            return;
        }
        const oa::formats::oademo::ReadResult start = oa::formats::oademo::read(bytes);
        if (!start.ok()) {
            keep_aside("its description cannot be read");
            return;
        }
        finish.replay_layout = true;
        finish.start = *start.replay;
        finish.end.ended = "cut";
        finish.end.after_crash = true;
    }
    const demo::FinishResult result = finish_partial(finish);
    if (result.ok) {
        done.finished += 1;
        done.lines.push_back(
            "finished the recording " + name + " that a crash left: " + path_to_utf8(result.placed)
        );
    } else if (result.empty) {
        done.empty += 1;
        done.lines.push_back(
            "dropped the recording " + name + " that a crash left: it held nothing"
        );
    } else if (result.transient) {
        // The disk refused; the stream stays where it is for a later start.
        done.postponed += 1;
        done.lines.push_back(
            "the recording " + name + " that a crash left waits for a later start: " + result.error
        );
    } else {
        keep_aside(result.error);
        return;
    }
    lock.reset();
    fs::remove(files.lock_file, failure);
}

/// Removes the files that a finished or dropped recording left behind without its stream.
///
/// @param folder the folder of one mod's recordings being made
void remove_orphans(const fs::path& folder) {
    std::error_code failure;
    // Each file with the name its recording's other files share.
    std::vector<std::pair<fs::path, std::string>> orphans;
    for (fs::directory_iterator entry(folder, failure), end; !failure && entry != end;
         entry.increment(failure)) {
        const std::string name = path_to_utf8(entry->path().filename());
        std::string base;
        if (ends_with(name, partial_start_suffix))
            base = name.substr(0, name.size() - partial_start_suffix.size());
        else if (ends_with(name, partial_note_suffix))
            base = name.substr(0, name.size() - partial_note_suffix.size());
        else if (ends_with(name, partial_lock_suffix))
            base = name.substr(0, name.size() - partial_lock_suffix.size());
        else if (ends_with(name, ".part.writing"))
            base = name.substr(0, name.size() - std::string_view(".part.writing").size());
        else
            continue;
        const std::string stream = base + std::string(partial_stream_suffix);
        std::error_code probe;
        if (fs::exists(folder / path_from_utf8(stream), probe) ||
            fs::exists(folder / path_from_utf8(stream + std::string(partial_failed_suffix)), probe))
            continue;
        orphans.emplace_back(entry->path(), std::move(base));
    }
    for (const auto& [orphan, base] : orphans) {
        // A game that has made its lock file and not yet its stream holds the lock.
        const fs::path lock_file = folder / path_from_utf8(base + std::string(partial_lock_suffix));
        std::unique_ptr<oa::app::package_install::FileLock> lock =
            oa::app::package_install::FileLock::take(lock_file);
        if (lock == nullptr)
            continue;
        fs::remove(orphan, failure);
        lock.reset();
        fs::remove(lock_file, failure);
    }
}

} // namespace

PartialsFinished finish_partial_recordings(const fs::path& user_folder) {
    PartialsFinished done;
    const fs::path root = recordings_partial_root(user_folder);
    std::error_code failure;
    if (!fs::is_directory(root, failure))
        return done;
    std::vector<fs::path> folders;
    for (fs::directory_iterator entry(root, failure), end; !failure && entry != end;
         entry.increment(failure)) {
        std::error_code probe;
        if (entry->is_directory(probe))
            folders.push_back(entry->path());
    }
    std::sort(folders.begin(), folders.end());
    for (const fs::path& folder : folders) {
        std::vector<fs::path> streams;
        failure.clear();
        for (fs::directory_iterator entry(folder, failure), end; !failure && entry != end;
             entry.increment(failure)) {
            std::error_code probe;
            if (entry->is_regular_file(probe) &&
                ends_with(path_to_utf8(entry->path().filename()), partial_stream_suffix))
                streams.push_back(entry->path());
        }
        std::sort(streams.begin(), streams.end());
        for (const fs::path& stream : streams)
            finish_one(user_folder, stream, done);
        remove_orphans(folder);
    }
    return done;
}

} // namespace oa::app
