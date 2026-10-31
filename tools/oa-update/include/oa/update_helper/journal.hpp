// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The operations a route carries out, and the journal that undoes them.
// The journal file is replaced whole after each step that finished.
#pragma once

#include "oa/update_helper/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oa::update_helper {

/// One step a route asks for. The inverse is fixed by the kind.
struct Operation {
    /// What the step does.
    enum class Kind : uint8_t {
        rename,       ///< move from onto to; to must not exist
        copy_tree,    ///< copy from onto to; to must not exist
        remove_tree,  ///< move path aside so it can be moved back
        make_link,    ///< make a link at path
        replace_link, ///< point the link at path somewhere else
        write_file,   ///< replace path with bytes
        run_and_wait, ///< run program and wait for it
    };

    Kind kind{Kind::rename};
    std::filesystem::path from{};         ///< rename and copy: the source
    std::filesystem::path to{};           ///< rename and copy: the destination
    std::filesystem::path path{};         ///< the file the other kinds change
    std::string bytes{};                  ///< write_file: the new bytes
    bool executable{false};               ///< write_file: mark the file executable
    std::string link_target{};            ///< make_link and replace_link: the new target
    std::string program{};                ///< run_and_wait: the program
    std::vector<std::string> arguments{}; ///< run_and_wait: its arguments
    std::string inverse_program{};        ///< run_and_wait: what undoes it; empty undoes nothing
    std::vector<std::string> inverse_arguments{}; ///< that program's arguments
};

/// What a rename hook did with one file.
enum class MoveOutcome : uint8_t {
    moved,     ///< the hook renamed the file
    postponed, ///< the file stays; the step is recorded and undone as nothing
    failed,    ///< the hook could not rename the file
};

/// How far a journal run got.
enum class JournalResult : uint8_t {
    finished, ///< every step finished; the journal is left for the caller
    failed,   ///< a step failed; finished steps were undone when that was possible
    stopped,  ///< the caller asked to stop; the journal holds the finished steps
};

/// Hooks a test uses in place of the real step. A null hook is the real one.
struct JournalHooks {
    void* context{}; ///< passed back to the hooks
    /// Return true to fail before this 1-based step, without carrying it out.
    bool (*fail_before_step)(void* context, int step){};
    /// Return true to stop after this 1-based step has been recorded.
    bool (*stop_after_step)(void* context, int step){};
    /// Run a program and wait. Null uses the real runner. Return false on failure.
    bool (*run)(
        void* context,
        const std::filesystem::path& program,
        const std::vector<std::string>& arguments,
        std::string* error
    ){};
    /// Rename one file. Null uses the real rename.
    ///
    /// `moved` means the hook renamed it. `postponed` records the step and
    /// moves nothing. `failed` stops the journal; set `error` when it is not null.
    MoveOutcome (*move)(
        void* context,
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::string* error
    ){};
    /// One line for the log. Null writes nothing.
    void (*note)(void* context, std::string_view line){};
    /// Renames one file or folder of a step: a rename that `move` does not
    /// take, a file moved aside or back, and an undone rename. Null uses the
    /// real rename. The macOS copy that holds an administrator's rights
    /// raises them here and nowhere else. Return false when it did not move.
    bool (*rename)(
        void* context, const std::filesystem::path& from, const std::filesystem::path& to
    ){};
};

/// One rename a journal recorded as postponed.
struct PostponedRenameRecord {
    std::string from; ///< the source that stayed
    std::string to;   ///< the destination that was not written
};

/// Carries out `operations`, recording each finished step under `updates`.
///
/// On a failure the finished steps are undone in reverse and the journal is
/// removed. When an undo itself fails, the journal is rewritten with the
/// steps still to undo so a later recover continues. A stop leaves the journal.
///
/// @param updates the data folder's updates directory
/// @param operations the steps, in order
/// @param hooks test hooks; the real runner and no early failure when the hooks are null
/// @param[out] error why a step failed; may be null
/// @return how far the run got
[[nodiscard]] JournalResult apply_journal(
    const std::filesystem::path& updates,
    const std::vector<Operation>& operations,
    const JournalHooks& hooks,
    std::string* error
);

/// Carries out `operations` after the steps a journal under `updates`
/// already holds, recording each finished step in that journal.
///
/// The steps already there count first, so a step's number continues
/// theirs. On a failure every finished step is undone in reverse, the
/// earlier ones too, and the journal is removed; an undo that fails leaves
/// the journal as apply_journal does. A missing journal starts empty. A
/// journal that cannot be read changes nothing and fails.
///
/// @param updates the data folder's updates directory
/// @param operations the steps to add, in order
/// @param hooks test hooks; the real runner and no early failure when the hooks are null
/// @param[out] error why a step failed; may be null
/// @return how far the run got
[[nodiscard]] JournalResult continue_journal(
    const std::filesystem::path& updates,
    const std::vector<Operation>& operations,
    const JournalHooks& hooks,
    std::string* error
);

/// Undoes a journal a killed run left.
///
/// No journal is success. A journal that undoes completely is removed,
/// together with the files it held aside.
///
/// @param updates the data folder's updates directory
/// @param hooks the runner for a step that runs a program; may be null
/// @param[out] error why an undo failed; may be null
/// @return true when there is nothing left to undo
[[nodiscard]] bool
undo_journal(const std::filesystem::path& updates, const JournalHooks& hooks, std::string* error);

/// The postponed renames a journal still holds.
///
/// A missing journal, and a journal that cannot be read, are both empty.
/// The caller still asks `undo_journal` when a read can fail.
///
/// @param updates the data folder's updates directory
/// @return the postponed rows, source then destination
[[nodiscard]] std::vector<PostponedRenameRecord>
postponed_renames(const std::filesystem::path& updates);

} // namespace oa::update_helper
