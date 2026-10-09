// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The renderer records' two files, renderer-state.conf and
// renderer-sentinel.conf, read and written in the folder that holds the
// player's preferences file (renderer_records.hpp holds what they mean and
// the rules). Every write is best effort: one that fails, as in a read-only
// profile, on a full disk or with a file locked by another program, is
// logged once and the records stay in memory for the run. Only the trial's
// write reports its failure to its caller, which then skips the stage the
// trial would have covered. The records are written only when one changes,
// each write flushed and, on POSIX systems, its folder synced; the sentinel
// is rewritten in place and never flushed, so a start that runs no function
// test makes no flushed write. A clean exit erases the run's trial, writes
// what is left and deletes the sentinel. A missing or corrupt file is never
// fatal: it is read as empty, and rewritten by the next write.
#pragma once

#include "oa/app/renderer_records.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::renderer_state {

/// Name of the records file, in the folder that holds preferences.conf.
inline constexpr std::string_view records_file_name = "renderer-state.conf";
/// Name of the sentinel's file, in the same folder.
inline constexpr std::string_view sentinel_file_name = "renderer-sentinel.conf";

/// Where the records live.
enum class Storage : uint8_t {
    disk,   ///< the player's own profile: both files beside preferences.conf
    memory, ///< a named --preferences-file: the records and the sentinel live in memory for the run
    disabled, ///< SDL_RENDER_DRIVER: nothing is read or written, and there is no sentinel
};

/// Where the records' log lines go.
struct LogHooks {
    void* context{};
    /// Writes one line of the game's log, without its end of line; null
    /// writes it to standard error.
    void (*line)(void* context, std::string_view text){};
};

/// What a start found in the folder, before it changed anything.
struct Found {
    /// The records file exists but cannot be read; it was ignored. After a
    /// start that did not end cleanly, such a file keeps the start on the
    /// standard tier, so a crash that garbled it cannot wipe its own
    /// evidence.
    bool records_unreadable{};
    uint32_t dropped{}; ///< keys of the records file that were dropped
    LeftoverSentinel sentinel{LeftoverSentinel::none};
    Sentinel leftover{}; ///< the left-over sentinel, for LeftoverSentinel::read
};

/// The records of one run and the files they are kept in.
///
/// It is the run's own object, never a process-wide one: a second run in the
/// same process keeps records of its own.
class RendererState {
  public:

    /// Reads the records and the left-over sentinel from a folder.
    ///
    /// A missing file is empty; a file that cannot be read is ignored and
    /// logged. Nothing is written until a write is asked for.
    ///
    /// @param folder the folder that holds preferences.conf
    /// @param engine_version the running engine's build, its version and the commit it was built from
    /// @param rules how this platform counts a left-over trial, and whether
    ///     the machine is under 2 GiB
    /// @param log where log lines go
    /// @return the records found there
    static RendererState open_folder(
        const std::filesystem::path& folder,
        std::string engine_version,
        const RecordRules& rules,
        LogHooks log = {}
    );

    /// Starts records that live in memory for the run, as with a named
    /// --preferences-file.
    ///
    /// @param engine_version the running engine's build, its version and the commit it was built from
    /// @param rules whether the machine is under 2 GiB
    /// @param log where log lines go
    /// @return empty records with no left-over sentinel
    static RendererState
    in_memory(std::string engine_version, const RecordRules& rules, LogHooks log = {});

    /// Starts records that read and write nothing, as under SDL_RENDER_DRIVER.
    ///
    /// @return empty records that every write leaves alone
    static RendererState disabled();

    /// Tells where the records live.
    ///
    /// @return the storage
    [[nodiscard]] Storage storage() const noexcept { return storage_; }

    /// Returns the rules the records follow on this machine, for the rules
    /// of renderer_records.hpp.
    ///
    /// @return the rules
    [[nodiscard]] const RecordRules& rules() const noexcept { return rules_; }

    /// Returns what the start found in the folder.
    ///
    /// @return what was found; all empty in memory and when disabled
    [[nodiscard]] const Found& found() const noexcept { return found_; }

    /// Tells whether this start must stay on the standard tier because the
    /// last run did not end cleanly and its records cannot be read, so that
    /// a crash that garbled the file cannot wipe its own evidence.
    ///
    /// @return true when a sentinel was left over and the records file could
    ///     not be read
    [[nodiscard]] bool records_unreadable_after_unclean_start() const noexcept {
        return found_.records_unreadable && found_.sentinel != LeftoverSentinel::none;
    }

    /// Returns the records as they stand in memory.
    ///
    /// @return the records
    [[nodiscard]] const Records& records() const noexcept { return records_; }

    /// Returns the records for the rules of renderer_records.hpp to change.
    ///
    /// The trial is changed through write_trial(), erase_trial() and
    /// clean_exit(), and by resolve_leftovers().
    ///
    /// @return the records
    [[nodiscard]] Records& records() noexcept { return records_; }

    /// Applies what the last run left behind to the records, once, and logs
    /// an unclean exit of the last run.
    ///
    /// The change is kept in memory until write_records() or write_trial().
    ///
    /// @return what changed; empty on a second call
    LeftoverOutcome resolve_leftovers();

    /// Writes the records when they differ from what the file holds.
    ///
    /// While a match runs the changes stay in memory, to be written by the
    /// first call after it ends. While a clear() waits for the player's OK
    /// (confirm_clear()), the file keeps what the clear took away, with what
    /// was struck or recorded since laid over it, so that Cancel and a crash
    /// before OK leave the records as they were and lose nothing found
    /// since. A failure is logged once and the records stay in memory; the
    /// next call tries again.
    ///
    /// @return true when the file holds the records, or the records live in
    ///     memory, are disabled or wait for the match to end
    bool write_records();

    /// Writes the trial before the stage it covers, flushed.
    ///
    /// The file then holds the records as write_records() writes them, with
    /// the trial, so that a strike made of what the last run left reaches
    /// the disk with the trial or before it; while a clear() waits, that is
    /// what the clear took away with what was found since. While a match
    /// runs, the trial is written with the records as the file holds them
    /// instead. The trial stands until erase_trial() once its stage passes,
    /// or clean_exit().
    ///
    /// @param trial the stage and driver; the driver's name valid
    /// @return false when it could not be written (logged once), and the
    ///     caller then skips the stage; false also under 2 GiB, where no
    ///     trial is written; else true in memory and when disabled, where a
    ///     trial cannot fail
    bool write_trial(const Trial& trial);

    /// Erases the trial this run wrote once its stage has passed, flushed.
    ///
    /// A trial an earlier run left is never erased here: resolve_leftovers()
    /// takes it up, or under 2 GiB keeps it.
    ///
    /// @return false when the file could not be written (logged once)
    bool erase_trial();

    /// Sets the sentinel, rewriting its file in place with no flush.
    ///
    /// A failure is logged once and the sentinel kept in memory. When
    /// disabled there is no sentinel.
    ///
    /// @param sentinel the stage and driver the run is at
    void set_sentinel(const Sentinel& sentinel);

    /// Deletes the sentinel and its file.
    void delete_sentinel();

    /// Ends the run cleanly: erases the trial this run wrote, writes the
    /// records as they stand in memory, a match's included, and deletes the
    /// sentinel, so that the next start counts nothing against a driver.
    ///
    /// Every exit that unwinds through main is clean, the fatal error's
    /// included. The writes are best effort, a failure logged once. A clear()
    /// the player has not confirmed is put back first (restore_failures()).
    void clean_exit();

    /// Returns the sentinel this run set.
    ///
    /// @return the sentinel, or nullopt when none is set
    [[nodiscard]] const std::optional<Sentinel>& sentinel() const noexcept { return sentinel_; }

    /// Tells the records whether a match is running, during which they are
    /// not written.
    ///
    /// @param running a match runs
    void set_match_running(bool running) noexcept { match_running_ = running; }

    /// Clears every strike, failure record and remembered rung in memory, as
    /// switching the setting Off then On and Restore defaults do.
    ///
    /// The clear waits for the player's OK (confirm_clear()) or Cancel
    /// (restore_failures()): until then the file keeps what it took away,
    /// with what was struck or recorded since (write_records()). A second
    /// clear() before either keeps what the first took away for Cancel.
    ///
    /// @return what changed
    Change clear();

    /// Writes the records a clear() cleared, as OK does, with what was struck
    /// or recorded since; with no clear waiting, as write_records().
    ///
    /// @return as write_records()
    bool confirm_clear();

    /// Puts back the strikes, failure records and remembered rungs a clear()
    /// took away, as Cancel does, with what was struck or recorded since laid
    /// over them: a driver's strike, record or remembered rung found since
    /// replaces its own, and the adapter, the native-density key and a trial
    /// written since stay as they are. Nothing changes with no clear
    /// waiting. The file is written by the next write_records().
    void restore_failures() noexcept;

  private:

    /// Starts empty records.
    ///
    /// @param storage where the records live
    /// @param engine_version the running engine's build, its version and the commit it was built from
    /// @param rules the rules the records follow on this machine
    /// @param log where log lines go
    RendererState(
        Storage storage, std::string engine_version, const RecordRules& rules, LogHooks log
    );

    /// Writes one log line.
    ///
    /// @param text the line
    void log(std::string_view text) const;

    /// Saves the records file's keys and values, flushed with its folder
    /// synced.
    ///
    /// @param values the keys and values
    /// @return true when the file holds them; a failure is logged once
    bool save_records_file(const Values& values);

    /// Returns the keys and values the records file is to hold: the records
    /// in memory, or while a clear() waits, what it took away with what was
    /// struck or recorded since.
    ///
    /// @return the keys and values
    [[nodiscard]] Values records_values() const;

    /// Writes the records to the file when they differ from what it holds,
    /// whether or not a match runs (records_values()).
    ///
    /// @return true when the file holds the records; a failure is logged once
    bool save_records();

    Storage storage_{Storage::disabled};
    std::filesystem::path folder_{};
    std::string engine_version_{};
    RecordRules rules_{};
    LogHooks log_{};
    Found found_{};
    Records records_{};
    /// What the records file holds: what was read, or last written.
    Values written_{};
    /// The records file could not be read, so the next write replaces it
    /// whatever it holds.
    bool file_unreadable_{};
    std::optional<Sentinel> sentinel_{};
    /// The trial that stands was written by this run.
    bool own_trial_{};
    /// A clear() changed the records and waits for confirm_clear() or
    /// restore_failures(); the file does not hold the cleared records.
    bool clear_pending_{};
    /// Every driver's entry as it stood before the clear() that waits.
    std::vector<DriverRecords> cleared_{};
    bool match_running_{};
    bool resolved_{};
    bool records_failure_logged_{};
    bool sentinel_failure_logged_{};
};

} // namespace oa::app::renderer_state
