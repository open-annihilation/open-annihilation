// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The packages waiting for the main menu, the change waiting for its run to
// end and its outcome, kept for the whole process.

#include "files.hpp"

#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/base/threads.hpp"
#include "oa/platform/file_types.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace oa::app::package_install {

namespace fs = std::filesystem;

namespace {

/// The origin a post recorded, until its outcome is reported.
struct OriginNote {
    fs::path file{};
    Origin origin{};
};

/// What the process keeps between runs.
struct Kept {
    base::threads::Mutex lock{};
    std::deque<OpenedPackage> files{};      ///< packages waiting, oldest first
    std::deque<fs::path> registries{};      ///< .oareg files waiting, oldest first
    std::deque<fs::path> replays{};         ///< .oademo and .tad files waiting, oldest first
    std::optional<OpenedPackage> current{}; ///< the package being installed
    std::vector<OriginNote> notes{};        ///< the origin of a posted path, until reported
    std::deque<PackageOutcome> outcomes{};  ///< catalogue outcomes, oldest first, at most 256
    std::optional<PendingChange> pending{};
    std::optional<ChangeOutcome> outcome{};
    std::vector<fs::path> discards{};
    std::optional<fs::path> handoff{}; ///< the hand-off folder, while the lock is held
};

/// Returns the process's kept state.
///
/// @return the state
Kept& kept() {
    static Kept state;
    return state;
}

/// Returns the form a path is compared in. The path is made absolute first,
/// then canonical as far as it exists, then lexically normal, so two
/// spellings of one path match.
///
/// @param file the path
/// @return its form
fs::path compared(const fs::path& file) {
    std::error_code error;
    fs::path form = fs::absolute(file, error);
    if (error)
        form = file;
    fs::path canonical = fs::weakly_canonical(form, error);
    if (!error)
        form = std::move(canonical);
    return form.lexically_normal();
}

/// Returns a file's extension without its dot, with ASCII letters lowered.
///
/// @param file the file
/// @return the extension; empty when the name has none
std::string extension_of(const fs::path& file) {
    std::string extension = detail::folded(detail::utf8_of(file.extension()));
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());
    return extension;
}

/// Tells whether `file`'s extension is `extension`, without case.
///
/// @param file the file
/// @param extension the extension, without its dot
/// @return true when they are the same
bool extension_is(const fs::path& file, std::string_view extension) {
    return extension_of(file) == detail::folded(extension);
}

/// Returns the origin recorded for a path, when one is.
///
/// @param state the process's kept state
/// @param form the path, as compared gives it
/// @return the note; null when the path has none
OriginNote* note_of(Kept& state, const fs::path& form) {
    for (OriginNote& note : state.notes) {
        if (note.file == form)
            return &note;
    }
    return nullptr;
}

/// Returns the origin a path keeps: the first one posted, recording `origin`
/// when the path has none yet.
///
/// @param[in,out] state the process's kept state
/// @param form the path, as compared gives it
/// @param origin the origin of this post
/// @return the origin the queue keeps
Origin kept_origin(Kept& state, const fs::path& form, const Origin& origin) {
    if (OriginNote* note = note_of(state, form))
        return note->origin;
    state.notes.push_back(OriginNote{form, origin});
    return origin;
}

/// Tells whether a package of this path is queued.
///
/// @param state the process's kept state
/// @param form the path, as compared gives it
/// @return true when it is
bool queued(const Kept& state, const fs::path& form) {
    return std::find_if(state.files.begin(), state.files.end(), [&](const OpenedPackage& opened) {
               return opened.file == form;
           }) != state.files.end();
}

} // namespace

void post_package_file(const fs::path& file, const Origin& origin) {
    const fs::path form = compared(file);
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    const Origin kept = kept_origin(state, form, origin);
    if ((state.current && state.current->file == form) || queued(state, form))
        return;
    state.files.push_back(OpenedPackage{form, kept});
}

std::optional<OpenedPackage> take_package_file() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    if (state.files.empty())
        return std::nullopt;
    OpenedPackage opened = std::move(state.files.front());
    state.files.pop_front();
    state.current = opened;
    return opened;
}

std::optional<OpenedPackage> next_package_file() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    if (state.files.empty())
        return std::nullopt;
    return state.files.front();
}

void return_package_file(const OpenedPackage& package) {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    const fs::path form = compared(package.file);
    state.current.reset();
    if (queued(state, form))
        return;
    state.files.push_front(OpenedPackage{form, kept_origin(state, form, package.origin)});
}

void report_package_outcome(PackageOutcome outcome) {
    const fs::path form = compared(outcome.file);
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    const OriginNote* note = note_of(state, form);
    if (note == nullptr)
        return;
    const Origin origin = note->origin;
    const auto index = static_cast<std::size_t>(note - state.notes.data());
    state.notes.erase(state.notes.begin() + static_cast<std::ptrdiff_t>(index));
    if (origin.kind != OriginKind::catalogue)
        return;
    outcome.file = form;
    if (state.outcomes.size() >= 256)
        state.outcomes.pop_front();
    state.outcomes.push_back(std::move(outcome));
}

std::vector<PackageOutcome> take_package_outcomes() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    std::vector<PackageOutcome> taken(state.outcomes.begin(), state.outcomes.end());
    state.outcomes.clear();
    return taken;
}

void finish_package_file() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    state.current.reset();
}

bool package_files_waiting() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    return !state.files.empty();
}

bool opens_file(const fs::path& file) {
    const std::string extension = extension_of(file);
    // A launch file is network play's, not the inbox's.
    for (const oa::platform::file_types::FileType& type : oa::platform::file_types::file_types)
        if (extension == type.extension &&
            type.opening == oa::platform::file_types::Opening::install)
            return true;
    return false;
}

void post_registry_file(const fs::path& file) {
    const fs::path form = compared(file);
    {
        Kept& state = kept();
        const base::threads::LockGuard guard(state.lock);
        if (std::find(state.registries.begin(), state.registries.end(), form) !=
            state.registries.end())
            return;
        state.registries.push_back(form);
    }
    // A .oareg file waits here until M08 takes it, and the log says so.
    detail::log_line("a .oareg file waits: " + detail::utf8_of(form));
}

std::optional<fs::path> take_registry_file() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    if (state.registries.empty())
        return std::nullopt;
    fs::path file = std::move(state.registries.front());
    state.registries.pop_front();
    return file;
}

bool registry_files_waiting() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    return !state.registries.empty();
}

bool opens_replay(const fs::path& file) {
    return extension_is(file, oa::platform::file_types::replay_extension) ||
           extension_is(file, oa::platform::file_types::tad_extension);
}

void post_replay_file(const fs::path& file) {
    const fs::path form = compared(file);
    {
        Kept& state = kept();
        const base::threads::LockGuard guard(state.lock);
        if (std::find(state.replays.begin(), state.replays.end(), form) != state.replays.end())
            return;
        state.replays.push_back(form);
    }
    detail::log_line("a replay file waits: " + detail::utf8_of(form));
}

std::optional<fs::path> take_replay_file() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    if (state.replays.empty())
        return std::nullopt;
    fs::path file = std::move(state.replays.front());
    state.replays.pop_front();
    return file;
}

bool replay_files_waiting() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    return !state.replays.empty();
}

void post_opened_file(const fs::path& file) {
    if (extension_is(file, oa::platform::file_types::registry_extension))
        post_registry_file(file);
    else if (opens_replay(file))
        post_replay_file(file);
    else
        post_package_file(file);
}

void set_pending_change(PendingChange change) {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    state.pending = std::move(change);
}

bool pending_change_waiting() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    return state.pending.has_value();
}

std::optional<std::string> pending_change_target() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    if (!state.pending)
        return std::nullopt;
    return state.pending->target;
}

void finish_pending_change(const ChangeOptions& options) {
    std::optional<PendingChange> pending;
    {
        Kept& state = kept();
        const base::threads::LockGuard guard(state.lock);
        pending = std::move(state.pending);
        state.pending.reset();
    }
    if (!pending)
        return;
    ChangeOptions checked = options;
    checked.expected = pending->replaced;
    ChangeOutcome outcome{};
    if (pending->kind == nullptr) {
        pending->hold.reset();
        return;
    }
    outcome.result =
        commit_change(*pending->kind, pending->root, pending->target, pending->change, checked);
    detail::log_line(
        pending->target +
        (outcome.result.changed ? ": changed with the run's archives closed" : ": not changed")
    );
    // The hold goes once the change is in place; then the folder is settled.
    pending->hold.reset();
    const Recovery recovery = recover_changes(*pending->kind, pending->root, options);
    std::vector<fs::path> discards = outcome.result.discards;
    discards.insert(discards.end(), recovery.discards.begin(), recovery.discards.end());
    outcome.change = std::move(*pending);
    // A roll back names no package file, and is not an outcome. The report
    // takes its own lock, so it is made before this function takes the lock
    // again.
    if (!outcome.change.file.empty()) {
        PackageOutcome reported{};
        reported.file = outcome.change.file;
        if (outcome.result.changed) {
            reported.result = OutcomeResult::installed;
        } else {
            reported.result = OutcomeResult::failed;
            if (outcome.change.kind != nullptr) {
                Problem problem{};
                problem.refusal = outcome.result.refusal;
                problem.detail = outcome.result.detail;
                reported.reason = refusal_text(*outcome.change.kind, problem);
            }
        }
        report_package_outcome(std::move(reported));
    }
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    state.outcome = std::move(outcome);
    for (fs::path& folder : discards)
        if (std::find(state.discards.begin(), state.discards.end(), folder) == state.discards.end())
            state.discards.push_back(std::move(folder));
}

bool discard_pending_change(const ChangeOptions& options) {
    std::optional<PendingChange> pending;
    {
        Kept& state = kept();
        const base::threads::LockGuard guard(state.lock);
        // A roll back waiting for the run to end is not the Library's held
        // update, and CANCEL on that update must leave it.
        if (!state.pending || state.pending->change == Change::roll_back)
            return false;
        pending = std::move(state.pending);
        state.pending.reset();
    }
    pending->hold.reset();
    if (pending->kind == nullptr)
        return true;
    const Recovery recovery = recover_changes(*pending->kind, pending->root, options);
    keep_discards(recovery.discards);
    detail::log_line(pending->target + ": dropped before it was put in place");
    return true;
}

std::optional<ChangeOutcome> take_change_outcome() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    std::optional<ChangeOutcome> outcome = std::move(state.outcome);
    state.outcome.reset();
    return outcome;
}

void keep_discards(const std::vector<fs::path>& folders) {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    for (const fs::path& folder : folders)
        if (std::find(state.discards.begin(), state.discards.end(), folder) == state.discards.end())
            state.discards.push_back(folder);
}

std::vector<fs::path> take_discards() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    return std::exchange(state.discards, {});
}

void set_handoff_folder(const fs::path& folder) {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    state.handoff = folder;
}

std::optional<fs::path> handoff_folder() {
    Kept& state = kept();
    const base::threads::LockGuard guard(state.lock);
    return state.handoff;
}

} // namespace oa::app::package_install
