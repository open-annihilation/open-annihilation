// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What outlives each run of the game within one process: the packages opened
// in the game and waiting for the main menu, the .oareg files waiting to be
// added, the change that waits for the run playing its target to end, its
// outcome for the next run, and the discard folders the start's recovery
// found. Every function may be called from any thread.
#pragma once

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/origin.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace oa::app::package_install {

/// A package waiting to be installed, and where it came from.
struct OpenedPackage {
    std::filesystem::path file{}; ///< the package
    Origin origin{};              ///< where it came from; a file when none was given
};

/// Queues a package to be installed once the main menu shows. A path already
/// queued, or the one being installed now, is not queued again: paths are
/// compared after the path is made absolute, then canonical as far as it
/// exists, then lexically normal. A path posted twice keeps the origin of the
/// first post.
///
/// @param file the package
/// @param origin where it came from; a file the player opened when omitted
void post_package_file(const std::filesystem::path& file, const Origin& origin = {});

/// Takes the next package, oldest first, and notes it as the one being
/// installed until finish_package_file.
///
/// @return the package; nothing when none waits
[[nodiscard]] std::optional<OpenedPackage> take_package_file();

/// Returns the next package, oldest first, without taking it or noting it
/// as the one being installed.
///
/// @return the package; nothing when none waits
[[nodiscard]] std::optional<OpenedPackage> next_package_file();

/// Puts a package back at the front, as when its question was set aside.
///
/// @param package the package
void return_package_file(const OpenedPackage& package);

/// What became of a package that came from a catalogue.
enum class OutcomeResult : uint8_t {
    installed, ///< the change is in place
    refused,   ///< refused before a change
    failed,    ///< a change was refused or undone
    set_aside, ///< the player answered CANCEL, or cancelled the unpacking
};

/// What became of one package, for the download queue.
struct PackageOutcome {
    std::filesystem::path file{}; ///< the package
    OutcomeResult result{};       ///< what became of it
    std::string reason{};         ///< why it was refused or failed; empty otherwise
};

/// Keeps what became of a package whose origin is a catalogue. A file origin
/// is dropped. At most 256 are kept; the oldest is dropped first.
///
/// @param outcome what became of it
void report_package_outcome(PackageOutcome outcome);

/// Takes what became of catalogue packages, oldest first, once each.
///
/// @return the outcomes
[[nodiscard]] std::vector<PackageOutcome> take_package_outcomes();

/// Notes that the package take_package_file gave is done with.
void finish_package_file();

/// Tells whether any package waits.
///
/// @return true while one does
[[nodiscard]] bool package_files_waiting();

/// Tells whether a file is one of the six the game installs or adds: a .oamod, .oalang,
/// .oamap, .oamut, .oabrain or .oareg file, by its extension, without case. A .oalaunch launch file, which the
/// game also opens, is not one of them.
///
/// @param file the file
/// @return true when the extension is one of those
[[nodiscard]] bool opens_file(const std::filesystem::path& file);

/// Queues a .oareg file to be added once the main menu shows. A path already
/// queued is not queued again: it is made absolute, then canonical as far as
/// it exists, then lexically normal, before it is compared. The file waits on
/// the add-registry queue, and is logged.
///
/// @param file the .oareg file
void post_registry_file(const std::filesystem::path& file);

/// Takes the next .oareg file, oldest first. M08 takes the add-registry queue with this.
///
/// @return the .oareg file; nothing when none waits
[[nodiscard]] std::optional<std::filesystem::path> take_registry_file();

/// Tells whether any .oareg file waits on the add-registry queue.
///
/// @return true while one does
[[nodiscard]] bool registry_files_waiting();

/// Queues a file the game was opened with. A .oareg file, in any case, goes to the
/// add-registry queue (post_registry_file); anything else goes to the package queue
/// (post_package_file). This is the only place that chooses the queue.
///
/// @param file the file
void post_opened_file(const std::filesystem::path& file);

/// A change whose target is what the game plays: it waits for the run to end
/// and its archives to close, then main() puts it in place before the next
/// run, or before the game ends when the player quits.
struct PendingChange {
    const PackageKind* kind{};    ///< the kind; null never
    std::filesystem::path root{}; ///< the kind's root folder
    std::string target{};         ///< the folder in the root
    Change change{Change::install};
    Incoming incoming{};         ///< what it installs; for a roll back, the kept version
    InstalledPackage replaced{}; ///< what the target holds now, which it must still hold
    std::string file_name{};     ///< the package's name; empty for a roll back
    /// Where the package came from, so a change that waited can be reported.
    Origin origin{};
    /// The package file the outcome names; empty for a roll back, which is
    /// not reported.
    std::filesystem::path file{};
    /// The hold on the root folder's lock the unpacking took, kept until the
    /// change is in place; null for a roll back, which takes its own.
    RootHold hold{};
};

/// Sets the change that waits for the run to end, replacing any.
///
/// @param change the change
void set_pending_change(PendingChange change);

/// Tells whether a change waits.
///
/// @return true while one does
[[nodiscard]] bool pending_change_waiting();

/// Returns the folder the waiting change acts on, so that what the player
/// sees can say the change finishes when they leave it: the folder in its
/// kind's root (PendingChange::target).
///
/// @return the folder's name; nothing while no change waits
[[nodiscard]] std::optional<std::string> pending_change_target();

/// What a change that waited for its run did, for the next run to tell.
struct ChangeOutcome {
    PendingChange change{};
    ChangeResult result{};
};

/// Puts the waiting change in place, if any, keeps its outcome for the next
/// run, releases the hold on the root folder, then settles the folder again
/// (recover_changes) and keeps the discard folders found. main() calls it
/// after each run: after a soft restart, and on the way out.
///
/// @param options the hooks and the waits
void finish_pending_change(const ChangeOptions& options = {});

/// Drops a waiting install, replace or reinstall without putting it in place.
///
/// The hold on the root folder is released, then the staged files are settled
/// away as the next start settles a change a stop left (recover_changes): the
/// installed folder, its record and its .backup stay as they are, and the
/// folders to delete are kept (take_discards). Nothing is reported, so a
/// download already counted installed is not fetched again. A waiting roll
/// back is left waiting. Nothing is done while no change waits.
///
/// @param options the hooks and the waits
/// @return true when a change was dropped
[[nodiscard]] bool discard_pending_change(const ChangeOptions& options = {});

/// Takes the outcome of the change that waited, once.
///
/// @return the outcome; nothing when no change waited
[[nodiscard]] std::optional<ChangeOutcome> take_change_outcome();

/// Keeps discard folders for the next run to delete.
///
/// @param folders the folders
void keep_discards(const std::vector<std::filesystem::path>& folders);

/// Takes the discard folders kept.
///
/// @return the folders
[[nodiscard]] std::vector<std::filesystem::path> take_discards();

/// Keeps the hand-off folder this process takes a second start's files from,
/// while it holds the instance lock (handoff.hpp).
///
/// @param folder the folder
void set_handoff_folder(const std::filesystem::path& folder);

/// Returns the hand-off folder this process takes files from.
///
/// @return the folder; nothing when the process holds no instance lock
[[nodiscard]] std::optional<std::filesystem::path> handoff_folder();

} // namespace oa::app::package_install
