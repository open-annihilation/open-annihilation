// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The chunked copy and the ImportRun worker (game_files_import.hpp): the
// planned files copied into the staging folder a chunk at a time, then the
// engine's check on the copy.
//
// A file is written as <name>.part, synced, given its source's modified time
// and renamed, so a staged file without .part is always whole and its size
// and time say which source file it came from. Continuing an interrupted
// copy therefore needs no list of finished files: each planned file whose
// staged copy has the same size and time is skipped.
#include "oa/app/game_files_import.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <system_error>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace oa::app::game_files {
namespace {

namespace threads = oa::base::threads;

/// Opens a file, by its wide path on Windows.
///
/// @param path the file
/// @param write true to create or truncate it for writing, false to read it
/// @return the open file; null when it cannot be opened (errno says why)
std::FILE* open_file(const fs::path& path, bool write) {
#if defined(_WIN32)
    return _wfsopen(path.c_str(), write ? L"wb" : L"rb", _SH_DENYNO);
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

/// Writes a file's buffered bytes through to the disk.
///
/// @param file an open file, flushed
/// @return true when the system reports the bytes written
bool sync_file(std::FILE* file) {
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

/// Tells whether an error number says the disk is full.
///
/// @param number an errno value
/// @return true for no space, or no quota left
bool disk_full(int number) noexcept {
#if defined(EDQUOT)
    if (number == EDQUOT)
        return true;
#endif
    return number == ENOSPC;
}

/// Tells how reading a source ended from an error number.
///
/// @param number an errno value
/// @return gone when the file or its drive went away, denied when access was withdrawn,
///     else unreadable
CopyOutcome read_outcome(int number) noexcept {
    switch (number) {
    case ENOENT:
    case ENXIO:
    case ENODEV:
        return CopyOutcome::gone;
    case EACCES:
    case EPERM:
        return CopyOutcome::denied;
    default:
        break;
    }
#if defined(ESTALE)
    if (number == ESTALE)
        return CopyOutcome::gone;
#endif
#if defined(ENOTCONN)
    if (number == ENOTCONN)
        return CopyOutcome::gone;
#endif
    return CopyOutcome::unreadable;
}

/// Says what an error number means, in the system's words.
///
/// @param number an errno value
/// @return its message
std::string error_text(int number) {
    return std::generic_category().message(number);
}

/// Returns the bytes free on the volume of a folder: the hooks' answer, else the system's.
///
/// @param hooks the platform's hooks
/// @param folder an existing folder
/// @param[out] bytes the bytes free
/// @return true when the free space could be read
bool free_bytes(const GameFilesHooks& hooks, const fs::path& folder, uint64_t& bytes) {
    if (hooks.free_space != nullptr)
        return hooks.free_space(hooks.context, path_to_utf8(folder).c_str(), &bytes);
    std::error_code error;
    const auto space = fs::space(folder, error);
    bytes = error ? 0 : space.available;
    return !error;
}

/// Lower-cases the ASCII letters of a path, as the engine matches names without case.
///
/// @param text UTF-8 text
/// @return the text with A-Z made a-z
std::string folded(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

/// Tells whether a path ends with the suffix of a file being copied.
///
/// @param text a path
/// @return true for a .part file
bool is_part_file(std::string_view text) noexcept {
    return text.size() > part_suffix.size() &&
           text.substr(text.size() - part_suffix.size()) == part_suffix;
}

/// Returns the file a planned file is read from.
///
/// @param plan the plan
/// @param file one of its files
/// @return the source file, absolute
fs::path source_of(const ImportPlan& plan, const PlannedFile& file) {
    if (plan.kind == SourceKind::demo_installer)
        return plan.source;
    return plan.source / path_from_utf8(file.path);
}

/// Removes from the staging folder every .part file and every file the plan does not copy,
/// then the folders left empty, so the staging folder holds only planned files.
///
/// @param staging the staging folder
/// @param wanted the planned targets, lower case
void tidy_staging(const fs::path& staging, const std::unordered_set<std::string>& wanted) {
    std::error_code error;
    std::vector<fs::path> unwanted;
    std::vector<fs::path> folders;
    for (fs::recursive_directory_iterator entry{staging, error}, end; !error && entry != end;
         entry.increment(error)) {
        std::error_code status;
        if (entry->is_directory(status) && !entry->is_symlink(status)) {
            folders.push_back(entry->path());
            continue;
        }
        auto relative = path_to_utf8(entry->path().lexically_relative(staging));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        if (is_part_file(relative) || !wanted.contains(folded(relative)))
            unwanted.push_back(entry->path());
    }
    for (const auto& file : unwanted) {
        std::error_code ignored;
        fs::remove(file, ignored);
    }
    // The deepest folders first, so a folder emptied by its children goes too.
    std::sort(folders.begin(), folders.end(), [](const fs::path& left, const fs::path& right) {
        return left.native().size() > right.native().size();
    });
    for (const auto& folder : folders) {
        std::error_code ignored;
        if (fs::is_empty(folder, ignored))
            fs::remove(folder, ignored);
    }
}

/// What a run's worker and the screen share.
struct RunState {
    mutable threads::Mutex mutex{}; ///< guards snapshot
    /// The latest state; bytes_done holds the finished files' bytes, and the current file's
    /// are added when it is read.
    RunSnapshot snapshot{};
    std::atomic<uint64_t> current_bytes{0};   ///< bytes of the current file written so far
    std::atomic<bool> stop{false};            ///< Stop, or the background time ran out
    std::atomic<bool> expired{false};         ///< the background time ran out
    std::atomic<bool> running{false};         ///< the worker runs
    GameFilesHooks hooks{};                   ///< the platform's hooks
    ImportPaths paths{};                      ///< the import's folders
    std::shared_ptr<const ImportPlan> plan{}; ///< what to copy
    Switches switches{};                      ///< the player's switches
    RunRequest request{};                     ///< what the run does
    std::vector<std::string> removals{};      ///< removals waiting for the next start, kept
};

/// Changes a run's snapshot under its lock.
///
/// @param[in,out] run the run
/// @param change what to change
template <typename Change>
void publish(RunState& run, Change change) {
    threads::LockGuard lock(run.mutex);
    change(run.snapshot);
}

/// Ends a run as failed.
///
/// @param[in,out] run the run
/// @param failure why
/// @param file the file it names; empty for none
/// @param error what to tell
void fail_run(RunState& run, RunFailure failure, std::string file, std::string error) {
    publish(run, [&](RunSnapshot& snapshot) {
        snapshot.stage = RunStage::failed;
        snapshot.failure = failure;
        snapshot.file = std::move(file);
        snapshot.error = std::move(error);
        snapshot.current.clear();
        snapshot.current_remote = false;
    });
}

/// Maps how copying one file ended to why the run failed.
///
/// @param outcome how the copy ended, not copied or stopped
/// @return the run's failure
RunFailure failure_of(CopyOutcome outcome) noexcept {
    switch (outcome) {
    case CopyOutcome::no_space:
        return RunFailure::no_space;
    case CopyOutcome::gone:
        return RunFailure::gone;
    case CopyOutcome::offline:
        return RunFailure::offline;
    case CopyOutcome::denied:
        return RunFailure::denied;
    case CopyOutcome::changed:
        return RunFailure::changed;
    case CopyOutcome::copied:
    case CopyOutcome::stopped:
    case CopyOutcome::unreadable:
        break;
    }
    return RunFailure::unreadable;
}

/// Stops a run when the platform's time away from the screen runs out.
///
/// @param userdata the run's RunState
void run_expired(void* userdata) {
    auto& run = *static_cast<RunState*>(userdata);
    run.expired.store(true);
    run.stop.store(true);
}

/// Returns the last component of a '/'-separated path.
///
/// @param path the path
/// @return its last component
std::string_view name_of(std::string_view path) noexcept {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// Returns the staged mod folder a mode mod run checks.
///
/// @param run the run
/// @return staging/mods/<id>, as the plan spells it
fs::path staged_mod_folder(const RunState& run) {
    const auto& plan = *run.plan;
    for (const auto& file : plan.files) {
        const auto& target = file.target;
        const auto first = target.find('/');
        const auto second = first == std::string::npos ? first : target.find('/', first + 1);
        if (second != std::string::npos)
            return run.paths.staging / path_from_utf8(target.substr(0, second));
    }
    const std::string id = plan.mods.empty() ? std::string{} : plan.mods.front().id;
    return run.paths.staging / fs::path(mods_folder_name) / path_from_utf8(id);
}

/// Runs the engine's check on the copy, as the run's mode lays it out.
///
/// @param run the run
/// @return the check's result
GameInstall check_copy(const RunState& run) {
    const auto& plan = *run.plan;
    const auto& paths = run.paths;
    switch (run.request.mode) {
    case ImportMode::add:
        return inspect_game_install(
            paths.game_folder, paths.data_folder, demo_1997, run.request.mod, paths.staging
        );
    case ImportMode::mod: {
        ModChoice choice = run.request.mod;
        if (choice.folder.empty())
            choice.folder = staged_mod_folder(run);
        return inspect_game_install(paths.game_folder, paths.data_folder, demo_1997, choice);
    }
    case ImportMode::replace:
    case ImportMode::remove:
        break;
    }
    return inspect_game_install(
        plan.move_in_place ? plan.source : paths.staging,
        paths.data_folder,
        demo_1997,
        run.request.mod
    );
}

/// Tells whether a planned file is copied under the switches.
///
/// @param file the planned file
/// @param switches the player's switches
/// @return true when its part, and its mod, are on (parts without a switch always are)
bool switched_on(const PlannedFile& file, const Switches& switches) noexcept {
    if (!part_switchable(file.part))
        return true;
    if (!switches.parts[static_cast<std::size_t>(file.part)])
        return false;
    if (file.part == Part::mods && file.mod != PlannedFile::no_mod &&
        file.mod < switches.mods.size())
        return switches.mods[file.mod] != 0;
    return true;
}

/// Copies the plan into the staging folder and checks it, on the run's worker thread.
///
/// @param[in,out] run the run
void copy_and_check(RunState& run) {
    const auto& plan = *run.plan;
    const auto& paths = run.paths;
    const auto& hooks = run.hooks;

    std::vector<const PlannedFile*> work;
    std::unordered_set<std::string> wanted;
    uint64_t total = 0;
    std::array<uint32_t, part_count> remaining{};
    std::vector<uint32_t> remaining_mods(plan.mods.size());
    for (const auto& file : plan.files) {
        if (!switched_on(file, run.switches))
            continue;
        work.push_back(&file);
        wanted.insert(folded(file.target));
        if (file.size_known)
            total += file.size;
        ++remaining[static_cast<std::size_t>(file.part)];
        if (file.part == Part::mods && file.mod < remaining_mods.size())
            ++remaining_mods[file.mod];
    }

    std::error_code error;
    fs::create_directories(paths.staging, error);
    std::error_code status;
    if (error || !fs::is_directory(paths.staging, status)) {
        fail_run(
            run,
            RunFailure::staging_unwritable,
            {},
            error ? error.message() : "the staging folder could not be made"
        );
        return;
    }
    ImportState state;
    state.phase = ImportPhase::copying;
    state.mode = run.request.mode;
    state.kind = plan.kind;
    state.location = plan.location;
    state.bytes = total;
    state.files = static_cast<uint32_t>(work.size());
    state.parts = run.switches.parts;
    for (std::size_t index = 0; index < plan.mods.size(); ++index)
        if (index < run.switches.mods.size() && run.switches.mods[index] == 0)
            state.mods_off.push_back(plan.mods[index].folder);
    if (run.request.mode == ImportMode::mod && !plan.mods.empty())
        state.mod_id = plan.mods.front().id;
    state.removals = run.removals;
    if (plan.move_in_place)
        state.move_source = path_to_utf8(plan.source);
    std::string state_error;
    if (!write_import_state(paths.state_file, state, &state_error)) {
        fail_run(run, RunFailure::staging_unwritable, {}, std::move(state_error));
        return;
    }
    tidy_staging(paths.staging, plan.move_in_place ? std::unordered_set<std::string>{} : wanted);

    publish(run, [&](RunSnapshot& snapshot) {
        snapshot.stage = RunStage::copying;
        snapshot.bytes_total = plan.move_in_place ? 0 : total;
        snapshot.files_total = static_cast<uint32_t>(work.size());
        snapshot.mods_done.assign(plan.mods.size(), 0);
    });

    // Marks a file finished: its part and mod tick off once all of theirs are.
    const auto finished = [&](const PlannedFile& file, uint64_t bytes, bool skipped) {
        const auto part = static_cast<std::size_t>(file.part);
        publish(run, [&](RunSnapshot& snapshot) {
            snapshot.bytes_done += bytes;
            run.current_bytes.store(0);
            ++snapshot.files_done;
            if (skipped)
                ++snapshot.files_skipped;
            if (remaining[part] > 0 && --remaining[part] == 0)
                snapshot.parts_done[part] = 2;
            if (file.part == Part::mods && file.mod < remaining_mods.size() &&
                remaining_mods[file.mod] > 0 && --remaining_mods[file.mod] == 0)
                snapshot.mods_done[file.mod] = 2;
        });
    };

    bool stopped = false;
    if (plan.move_in_place) {
        // The folder is renamed into place whole at the commit; nothing is copied.
        for (const auto* file : work)
            finished(*file, 0, true);
    } else {
        if (hooks.keep_running != nullptr)
            hooks.keep_running(hooks.context, true, run_expired, &run);
        for (const auto* planned : work) {
            const auto& file = *planned;
            if (run.stop.load()) {
                stopped = true;
                break;
            }
            const auto staged = paths.staging / path_from_utf8(file.target);
            const auto part = static_cast<std::size_t>(file.part);
            publish(run, [&](RunSnapshot& snapshot) {
                snapshot.current = std::string(name_of(file.target));
                snapshot.current_remote = file.remote;
                if (snapshot.parts_done[part] == 0)
                    snapshot.parts_done[part] = 1;
                if (file.part == Part::mods && file.mod < snapshot.mods_done.size() &&
                    snapshot.mods_done[file.mod] == 0)
                    snapshot.mods_done[file.mod] = 1;
            });
            if (staged_matches(staged, file)) {
                finished(file, file.size, true);
                continue;
            }
            // Nothing is written through a link in the staging folder.
            if (passes_through_link(paths.staging, staged)) {
                fail_run(
                    run,
                    RunFailure::staging_unwritable,
                    file.target,
                    "a part of its path in the staging folder is a link"
                );
                stopped = true;
                break;
            }
            fs::create_directories(staged.parent_path(), error);
            if (error) {
                fail_run(run, RunFailure::staging_unwritable, file.target, error.message());
                stopped = true;
                break;
            }
            // A file of unknown size may need any space: copy it only with the margin free.
            uint64_t free = 0;
            if (!file.size_known && free_bytes(hooks, paths.staging, free) &&
                free < space_margin_bytes) {
                fail_run(run, RunFailure::no_space, file.target, error_text(ENOSPC));
                stopped = true;
                break;
            }
            const auto source = source_of(plan, file);
            if (plan.movable) {
                // A file the system copied for the game is moved, not copied again.
                std::error_code moved;
                fs::rename(source, staged, moved);
                if (!moved) {
                    finished(file, file.size_known ? file.size : 0, false);
                    continue;
                }
            }
            const auto source_text = path_to_utf8(source);
            const auto target_text = path_to_utf8(staged) + std::string(part_suffix);
            FileCopy copy;
            copy.source = source_text.c_str();
            copy.target = target_text.c_str();
            copy.size = file.size;
            copy.size_known = file.size_known;
            copy.modified = file.modified;
            copy.bytes_done = &run.current_bytes;
            copy.stop = &run.stop;
            copy.copy = chunked_copy;
            run.current_bytes.store(0);
            std::string copy_error;
            auto outcome = hooks.copy_file != nullptr
                               ? hooks.copy_file(hooks.context, copy, &copy_error)
                               : chunked_copy(copy.source, copy, &copy_error);
            std::error_code ignored;
            if (outcome == CopyOutcome::copied && !fs::is_regular_file(staged, ignored)) {
                outcome = CopyOutcome::unreadable;
                copy_error = "the copy did not arrive in the staging folder";
            }
            if (outcome != CopyOutcome::copied)
                fs::remove(path_from_utf8(target_text), ignored);
            if (outcome == CopyOutcome::stopped) {
                run.current_bytes.store(0);
                stopped = true;
                break;
            }
            if (outcome != CopyOutcome::copied) {
                run.current_bytes.store(0);
                fail_run(run, failure_of(outcome), file.target, std::move(copy_error));
                stopped = true;
                break;
            }
            const auto size = file.size_known ? file.size : fs::file_size(staged, ignored);
            finished(file, ignored ? 0 : size, false);
        }
        if (hooks.keep_running != nullptr)
            hooks.keep_running(hooks.context, false, run_expired, &run);
    }
    if (stopped) {
        publish(run, [&](RunSnapshot& snapshot) {
            snapshot.current.clear();
            snapshot.current_remote = false;
            if (snapshot.stage == RunStage::failed)
                return;
            snapshot.stage = RunStage::stopped;
            snapshot.stopped_by_expiry = run.expired.load();
        });
        return;
    }
    if (!run.request.check) {
        state.phase = ImportPhase::checked;
        static_cast<void>(write_import_state(paths.state_file, state, &state_error));
        publish(run, [&](RunSnapshot& snapshot) {
            snapshot.current.clear();
            snapshot.stage = RunStage::checked;
        });
        return;
    }
    publish(run, [&](RunSnapshot& snapshot) {
        snapshot.current.clear();
        snapshot.current_remote = false;
        snapshot.stage = RunStage::checking;
    });
    auto install = check_copy(run);
    const bool playable = usable(install);
    if (playable) {
        state.phase = ImportPhase::checked;
        static_cast<void>(write_import_state(paths.state_file, state, &state_error));
    }
    publish(run, [&](RunSnapshot& snapshot) {
        snapshot.check = std::move(install);
        snapshot.failure = playable ? RunFailure::none : RunFailure::not_usable;
        snapshot.stage = RunStage::checked;
    });
}

/// Runs an import on its worker thread.
///
/// @param argument the run's RunState
void run_import(void* argument) {
    auto& run = *static_cast<RunState*>(argument);
    copy_and_check(run);
    run.running.store(false);
}

} // namespace

/// What the run's worker and the screen share.
struct ImportRun::Shared : RunState {};

CopyOutcome chunked_copy(const char* readable_source, const FileCopy& file, std::string* error) {
    return chunked_copy_with(readable_source, file, error, {});
}

CopyOutcome chunked_copy_with(
    const char* readable_source,
    const FileCopy& file,
    std::string* error,
    const ChunkedCopyHooks& hooks
) {
    std::string ignored_error;
    auto& why = error != nullptr ? *error : ignored_error;
    why.clear();
    const auto stopped = [&] { return file.stop != nullptr && file.stop->load(); };
    if (readable_source == nullptr || file.target == nullptr) {
        why = "no file to copy";
        return CopyOutcome::unreadable;
    }
    if (stopped())
        return CopyOutcome::stopped;
    const auto source = path_from_utf8(readable_source);
    const auto part = path_from_utf8(file.target);

    std::error_code failure;
    const auto status = fs::status(source, failure);
    if (failure) {
        why = failure.message();
        return read_outcome(failure.value());
    }
    if (!fs::is_regular_file(status)) {
        why = "it is not a file";
        return CopyOutcome::unreadable;
    }
    const auto size = fs::file_size(source, failure);
    const auto time = failure ? fs::file_time_type{} : fs::last_write_time(source, failure);
    if (failure) {
        why = failure.message();
        return read_outcome(failure.value());
    }
    const int64_t modified = seconds_since_1970(time);
    if (file.size_known && (size != file.size || modified != file.modified)) {
        why = "its size or time changed since the folder was read";
        return CopyOutcome::changed;
    }

    std::FILE* in = open_file(source, false);
    if (in == nullptr) {
        const int number = errno;
        why = error_text(number);
        return read_outcome(number);
    }
    fs::create_directories(part.parent_path(), failure);
    std::FILE* out = open_file(part, true);
    if (out == nullptr) {
        const int number = errno;
        std::fclose(in);
        why = error_text(number);
        return disk_full(number) ? CopyOutcome::no_space : CopyOutcome::unreadable;
    }
    std::vector<unsigned char> buffer(copy_chunk_bytes);
    uint64_t written = 0;
    CopyOutcome outcome = CopyOutcome::copied;
    while (true) {
        if (stopped()) {
            outcome = CopyOutcome::stopped;
            break;
        }
        const auto read = std::fread(buffer.data(), 1, buffer.size(), in);
        if (read < buffer.size() && std::ferror(in) != 0) {
            const int number = errno;
            why = error_text(number);
            outcome = read_outcome(number);
            break;
        }
        if (read > 0) {
            if (std::fwrite(buffer.data(), 1, read, out) != read) {
                const int number = errno;
                why = error_text(number);
                outcome = disk_full(number) ? CopyOutcome::no_space : CopyOutcome::unreadable;
                break;
            }
            written += read;
            if (file.bytes_done != nullptr)
                file.bytes_done->fetch_add(read);
            if (hooks.chunk_written != nullptr)
                hooks.chunk_written(hooks.context, written);
        }
        if (read < buffer.size())
            break;
    }
    std::fclose(in);
    if (outcome == CopyOutcome::copied && file.size_known && written != file.size) {
        why = "its size changed while it was copied";
        outcome = CopyOutcome::changed;
    }
    if (outcome == CopyOutcome::copied && (std::fflush(out) != 0 || !sync_file(out))) {
        const int number = errno;
        why = error_text(number);
        outcome = disk_full(number) ? CopyOutcome::no_space : CopyOutcome::unreadable;
    }
    if (std::fclose(out) != 0 && outcome == CopyOutcome::copied) {
        const int number = errno;
        why = error_text(number);
        outcome = disk_full(number) ? CopyOutcome::no_space : CopyOutcome::unreadable;
    }
    std::error_code ignored;
    if (outcome != CopyOutcome::copied) {
        fs::remove(part, ignored);
        return outcome;
    }
    // The planned time, so a later run knows the staged file by its size and time.
    fs::last_write_time(
        part,
        file_time_from_seconds(file.size_known || file.modified != 0 ? file.modified : modified),
        ignored
    );
    const auto text = path_to_utf8(part);
    if (is_part_file(text)) {
        fs::rename(part, path_from_utf8(text.substr(0, text.size() - part_suffix.size())), failure);
        if (failure) {
            why = failure.message();
            fs::remove(part, ignored);
            return disk_full(failure.value()) ? CopyOutcome::no_space : CopyOutcome::unreadable;
        }
    }
    return CopyOutcome::copied;
}

ImportRun::~ImportRun() {
    if (shared_)
        shared_->stop.store(true);
    join();
}

bool ImportRun::start(
    const GameFilesHooks& hooks,
    const ImportPaths& paths,
    std::shared_ptr<const ImportPlan> plan,
    const Switches& switches,
    const RunRequest& request,
    std::string* error
) {
    std::string ignored;
    auto& why = error != nullptr ? *error : ignored;
    if (busy()) {
        why = "a copy is already running";
        return false;
    }
    if (!plan) {
        why = "there is nothing to copy";
        return false;
    }
    if (request.mode == ImportMode::remove) {
        why = "a removal copies nothing";
        return false;
    }
    std::string state_error;
    const auto waiting = read_import_state(paths.state_file, &state_error);
    if (waiting && waiting->phase == ImportPhase::checked && waiting->mode == ImportMode::replace &&
        request.mode != ImportMode::replace) {
        why = "the new game files wait for the next start of Open Annihilation";
        return false;
    }
    join();
    auto shared = std::make_shared<Shared>();
    shared->hooks = hooks;
    shared->paths = paths;
    shared->plan = std::move(plan);
    shared->switches = switches;
    shared->request = request;
    if (waiting)
        shared->removals = waiting->removals;
    shared->running.store(true);
    shared_ = shared;
    if (!threads::start_thread(thread_, run_import, static_cast<RunState*>(shared.get()))) {
        shared->running.store(false);
        fail_run(*shared, RunFailure::staging_unwritable, {}, "the copy could not start");
        why = "the copy could not start";
        return false;
    }
    return true;
}

void ImportRun::stop() noexcept {
    if (shared_)
        shared_->stop.store(true);
}

void ImportRun::expire() noexcept {
    if (!shared_)
        return;
    shared_->expired.store(true);
    shared_->stop.store(true);
}

RunSnapshot ImportRun::snapshot() const {
    if (!shared_)
        return {};
    threads::LockGuard lock(shared_->mutex);
    RunSnapshot snapshot = shared_->snapshot;
    snapshot.bytes_done += shared_->current_bytes.load();
    return snapshot;
}

bool ImportRun::busy() const noexcept {
    return shared_ && shared_->running.load();
}

void ImportRun::join() noexcept {
    threads::join_thread(thread_);
}

} // namespace oa::app::game_files
