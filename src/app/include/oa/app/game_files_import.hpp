// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Bringing the player's game files into the game's own storage, with no SDL:
// which files are copied and which are left out, the quick check by name of a
// chosen folder, the plan Ready to copy shows, the space it needs, the
// listing and the copy on worker threads, the import's state file, the
// commit that renames the copy into place, recovery at the next start,
// adopting files the player copied themselves, what is installed, and the
// additions and removals of the management state. The platform's side comes
// through GameFilesHooks (game_files_hooks.hpp).
#pragma once

#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/base/threads.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::game_files {

namespace fs = std::filesystem;

inline constexpr std::string_view game_folder_name =
    "Total Annihilation";                                        ///< the game folder's name
inline constexpr std::string_view old_folder_suffix = " (old)";  ///< an earlier folder set aside
inline constexpr std::string_view import_folder_name = "import"; ///< under the data folder
inline constexpr std::string_view state_file_name = "state";     ///< in the import folder
inline constexpr std::string_view part_suffix = ".part";         ///< a file being copied
inline constexpr uint64_t space_margin_bytes = 200'000'000;      ///< kept free above the files
inline constexpr uint64_t far_too_large_bytes = 10'000'000'000;  ///< "Is this the right folder?"
inline constexpr uint32_t nested_search_depth = 2;               ///< levels searched below the top
inline constexpr uint32_t copy_chunk_bytes = 1u << 20;           ///< 1 MiB per write
inline constexpr std::size_t most_listed_names = 8;              ///< file names a part row shows

/// The game's parts, as Ready to copy groups the files (display only).
enum class Part : uint8_t {
    game_archives,    ///< totala1-4.hpi, worlds.hpi (no switch)
    update_31c,       ///< rev31.gp3 (no switch)
    core_contingency, ///< ccdata, ccmaps, ccmiss .ccx
    battle_tactics,   ///< btdata, btmaps .ccx, tactics1-8.hpi
    extra,            ///< other .ufo, .ccx, .hpi, .gp3 at the top
    music,            ///< music/
    movies,           ///< the intro and ending movies (data/*.zrb and the like)
    mods,             ///< mods/<folder>/ with oamod.yaml, each its own switch
    other, ///< everything else kept: loose data folders, totala.ini (no switch, always copied)
    demo,  ///< the demo's installer (the demo route only)
};
/// How many parts there are.
inline constexpr std::size_t part_count = 10;

/// Tells whether a part has a switch on Ready to copy.
///
/// @param part the part
/// @return true for the optional parts the player may leave out
[[nodiscard]] constexpr bool part_switchable(Part part) noexcept {
    return part == Part::core_contingency || part == Part::battle_tactics || part == Part::extra ||
           part == Part::music || part == Part::movies || part == Part::mods;
}

/// Why a file is left out of the copy.
enum class LeftOutReason : uint8_t {
    windows_program, ///< .exe .dll .msi .cab .lnk .scr (but the demo installer's size is kept)
    help_file,       ///< .ico .pdf .doc .rtf .hlp .chm .cnt
    uninstaller,     ///< unins<digits>.{exe,dat,ini,msg}
    system_clutter,  ///< names starting with '.', Thumbs.db, desktop.ini, __MACOSX/
    link,            ///< a symbolic link
    unsafe_name,     ///< an empty, '.' or '..' part, a backslash, NUL, an absolute or drive path
    case_clash,      ///< differs only in capital letters from a file copied before it
};

/// Returns why a file no game data uses is left out, or nothing when it is copied.
///
/// @param relative_path the file's path relative to the source, '/'-separated
/// @param size its size in bytes
/// @param folder_has_archives false keeps a file of the demo installer's size whatever its name
/// @return the reason it is left out; nothing when it is copied
[[nodiscard]] std::optional<LeftOutReason>
left_out(std::string_view relative_path, uint64_t size, bool folder_has_archives) noexcept;

/// What a picker chose.
enum class SourceKind : uint8_t {
    game_folder,      ///< the Total Annihilation folder
    demo_installer,   ///< the demo's installer
    additions_folder, ///< an expansion, extra maps or a mod
    archives,         ///< several archives
};

/// What the top of a chosen folder looks like.
enum class TopLook : uint8_t {
    game,          ///< an archive name, the installer's size or oamod.yaml at the top
    nested,        ///< no archive at the top (an installer at most); folders below hold some
    nothing,       ///< none here or two levels down
    already_there, ///< the chosen folder is the game folder itself
    in_documents, ///< a game folder in the game folder's parent under another name: moved by rename
};

/// The quick check by name, before listing deeply.
struct NameCheck {
    TopLook look{TopLook::nothing}; ///< what the top looks like
    std::vector<std::string>
        nested{}; ///< folders below holding archives, relative, '/'-separated, first level that has any
    std::string error{}; ///< why the folder could not be read; look is nothing then
};

/// One file the plan copies (or moves).
struct PlannedFile {
    std::string path{}; ///< relative to the source; also its place in the staging folder
    std::string
        target{};    ///< relative to the staging folder (folded spelling; mods/<id>/... for a mod)
    uint64_t size{}; ///< bytes, when size_known
    bool size_known = true; ///< false when the listing reported no size
    int64_t modified{};     ///< seconds since 1970
    bool remote{};          ///< held by a cloud service and downloaded when it is read
    Part part{Part::other}; ///< the part it belongs to
    uint32_t mod{no_mod};   ///< index into ImportPlan::mods, for Part::mods
    static constexpr uint32_t no_mod = 0xffffffffu; ///< not a mod's file
};

/// One file left out, and why.
struct LeftOutFile {
    std::string path{};      ///< relative to the source
    uint64_t size{};         ///< bytes
    LeftOutReason reason{};  ///< why it is left out
    std::string kept_path{}; ///< case_clash: the file copied in its place, relative to the source
};

/// A mod found in the source (mods/<folder>/oamod.yaml, or the top for an additions folder).
struct ModFound {
    std::string folder{};              ///< relative folder in the source
    std::string id{};                  ///< its profile's id (the target folder mods/<id>)
    std::string name{};                ///< its profile's name, as players see it
    std::vector<std::string> errors{}; ///< why its profile cannot be used; empty when it can
};

/// What Ready to copy shows for one part.
struct PartSummary {
    Part part{};             ///< the part
    bool found{};            ///< any file of the part is in the source (or installed)
    uint32_t files{};        ///< its files
    uint64_t bytes{};        ///< their known sizes
    bool bytes_known = true; ///< false when some sizes were not reported
    std::vector<std::string>
        names{};             ///< up to most_listed_names display names, matched files' own spelling
    uint32_t music_tracks{}; ///< for Part::music: the tracks found
};

/// The player's switches, all on at first.
struct Switches {
    std::array<bool, part_count> parts{
        true, true, true, true, true, true, true, true, true, true
    }; ///< by Part
    std::vector<uint8_t> mods{}; ///< 1 per ImportPlan::mods entry, on
};

/// Everything Ready to copy needs, and what the copy does.
struct ImportPlan {
    SourceKind kind{SourceKind::game_folder}; ///< what the picker chose
    fs::path source{};                        ///< absolute: the folder, or the installer's file
    std::string location{}; ///< display: the source's place, as the platform names it
    bool movable{};         ///< the picker's files may be moved (a file copied for the game)
    bool move_in_place{};   ///< TopLook::in_documents: commit renames the source itself
    std::vector<PlannedFile> files{};            ///< the files copied
    std::vector<LeftOutFile> left_out{};         ///< the files left out
    std::vector<ModFound> mods{};                ///< the mods found
    std::array<PartSummary, part_count> parts{}; ///< by Part
    std::vector<std::string> warnings{};         ///< case clashes, one sentence each
    std::vector<std::string> kept{}; ///< additions: names already in the game folder, kept
    uint64_t total_bytes{};          ///< known sizes of every planned file
    uint64_t remote_bytes{};         ///< of which held in the cloud only
    bool sizes_unknown{};            ///< some sizes were not reported ("at least")
    bool has_archives{};             ///< the source holds game archives
    bool demo{};                     ///< the demo route: one installer, unpacked by the check
};

/// The folders an import uses.
struct ImportPaths {
    fs::path game_folder{}; ///< GameFilesHooks::game_folder
    fs::path documents{};   ///< its parent, where Copy it yourself and "(old)" folders lie
    fs::path data_folder{}; ///< --data-dir or the per-user data folder; the demo is unpacked there
    fs::path import_root{}; ///< data_folder/import
    fs::path staging{};     ///< import_root/Total Annihilation
    fs::path state_file{};  ///< import_root/state
};

/// Builds the paths from the game folder and the data folder.
///
/// @param game_folder where the game folder goes
/// @param data_folder the per-user data folder
/// @return every folder the import uses
[[nodiscard]] ImportPaths import_paths(const fs::path& game_folder, const fs::path& data_folder);

/// Lists a source through the hooks, or with std::filesystem without them.
///
/// @param hooks the platform's hooks; a null list_source walks the folder itself
/// @param source the folder to list
/// @param max_depth levels below `source` listed; 0 lists its own entries only
/// @param[out] entries the entries found, appended
/// @param[out] error why it could not be listed
/// @param stop may end it early (false, error empty); null: never
/// @param progress told the count and bytes so far; may be null
/// @param userdata passed to `progress`
/// @return true when the whole listing was read
[[nodiscard]] bool list_source(
    const GameFilesHooks& hooks,
    const fs::path& source,
    uint32_t max_depth,
    std::vector<SourceEntry>* entries,
    std::string* error,
    const std::atomic<bool>* stop = nullptr,
    void (*progress)(void* userdata, uint32_t files, uint64_t bytes) = nullptr,
    void* userdata = nullptr
);

/// The name check: reads the top, then up to nested_search_depth levels below.
///
/// @param hooks the platform's hooks
/// @param paths the import's folders (to tell the game folder and its parent)
/// @param folder the chosen folder
/// @return what the top looks like, and the folders below that hold archives
[[nodiscard]] NameCheck
check_names(const GameFilesHooks& hooks, const ImportPaths& paths, const fs::path& folder);

/// What is already installed, for additions, Settings and Manage.
struct InstalledSummary {
    bool present{};                              ///< the game folder exists
    std::array<PartSummary, part_count> parts{}; ///< by Part
    std::vector<ModFound> mods{};                ///< mods/<id>/ folders with their profiles
    uint64_t bytes{};                            ///< the game folder's size
    bool demo{};                                 ///< it holds the demo's installer and no archives
    uint64_t demo_data_bytes{};                  ///< the unpacked demo archive's size, 0 when none
    bool demo_data_unused{};     ///< archives are present, so the demo's data is not used
    uint64_t old_folder_bytes{}; ///< "Total Annihilation (old)" (and " 2", ...), 0 when none
    std::vector<fs::path> old_folders{}; ///< those folders
    /// Every file and folder of the game folder, relative, '/'-separated, as listed: the
    /// spellings an addition folds onto and the names it keeps.
    std::vector<SourceEntry> listing{};
};

/// Lists the game folder and summarises it by part.
///
/// @param hooks the platform's hooks
/// @param paths the import's folders
/// @return what is installed
[[nodiscard]] InstalledSummary
summarize_installed(const GameFilesHooks& hooks, const ImportPaths& paths);

/// Plans a copy from a listing: the leave-out table, safe names, case folding (the first
/// spelling, or the installed folder's for additions), parts, mods with their profiles
/// resolved from the source's oamod.yaml, and for additions the files already there (kept).
///
/// @param kind what the picker chose
/// @param source the chosen folder or file, absolute
/// @param location the source's display location
/// @param entries the source's listing
/// @param installed what is installed, for additions; null otherwise
/// @return the plan Ready to copy shows
[[nodiscard]] ImportPlan plan_import(
    SourceKind kind,
    const fs::path& source,
    std::string location,
    std::span<const SourceEntry> entries,
    const InstalledSummary* installed = nullptr
);

/// The space a copy needs and what is free.
struct SpaceNeed {
    uint64_t copy_bytes{}; ///< planned files under the switches, less staged files that match
    uint64_t need_bytes{}; ///< copy_bytes + demo unpack + space_margin_bytes
    uint64_t free_bytes{}; ///< free on the game folder's volume, when free_known
    bool free_known{};     ///< the free space could be read
    bool fits{};           ///< need_bytes is no more than free_bytes (or the free space is unknown)
    std::vector<Part>
        fitting_off{}; ///< switchable parts whose turning off would make it fit, largest first
};

/// Works out the space a plan needs under the switches, against the free space of the
/// game folder's volume (the hooks' free_space, else std::filesystem::space).
///
/// @param hooks the platform's hooks
/// @param paths the import's folders
/// @param plan the plan
/// @param switches the player's switches
/// @return the space needed and free
[[nodiscard]] SpaceNeed space_need(
    const GameFilesHooks& hooks,
    const ImportPaths& paths,
    const ImportPlan& plan,
    const Switches& switches
);

/// Tells whether a path below a folder leads through something that is not
/// a plain folder: a part of it below `root`, `path` itself included, that
/// is a symbolic link or another kind of entry that is neither a folder nor
/// a file. Writing or removing there could reach outside `root`.
///
/// @param root the folder
/// @param path a path below `root`; one not below it counts as leading out
/// @return true when a part of `path` below `root` is such an entry
[[nodiscard]] bool passes_through_link(const fs::path& root, const fs::path& path);

/// Tells whether a staged file matches its planned source (size and modified time).
///
/// @param staged the staged file
/// @param file the planned file
/// @return true when the staged file can be kept
[[nodiscard]] bool staged_matches(const fs::path& staged, const PlannedFile& file) noexcept;

/// The engine's chunked copy (FileCopy::copy): 1 MiB writes to file.target (the .part),
/// sync, the source's time set, renamed to the name without .part. Checks the stop flag
/// between chunks; removes the .part on any outcome but copied.
///
/// @param readable_source the file to read, UTF-8
/// @param file what to copy and where
/// @param[out] error why it failed
/// @return how the copy ended
CopyOutcome chunked_copy(const char* readable_source, const FileCopy& file, std::string* error);

/// What chunked_copy_with tells as it copies: a seam for tests, which can act at a chunk's
/// end without racing the copy.
struct ChunkedCopyHooks {
    void* context{}; ///< passed back to every hook
    /// Runs after each chunk is written and counted in file.bytes_done, before the stop
    /// flag is checked for the next, with the bytes of the file written so far. Null:
    /// nothing runs.
    void (*chunk_written)(void* context, uint64_t written){};
};

/// The chunked copy, telling `hooks` of each chunk written; chunked_copy with no hooks.
///
/// @param readable_source the file to read, UTF-8
/// @param file what to copy and where
/// @param[out] error why it failed
/// @param hooks run as the copy goes
/// @return how the copy ended
CopyOutcome chunked_copy_with(
    const char* readable_source,
    const FileCopy& file,
    std::string* error,
    const ChunkedCopyHooks& hooks
);

/// What a SourceScan does.
struct ScanRequest {
    SourceKind kind{SourceKind::game_folder}; ///< what the picker chose
    std::vector<std::string> paths{};         ///< the picker's answer
    bool movable{};                           ///< the picker's files may be moved
    std::string location{};                   ///< display location; the folder's name when empty
    bool inspect_local = true;                ///< run the engine's check when no file is remote
    ModChoice mod{};                          ///< the mod folder and preferences the check uses
};

/// Where a scan is.
enum class ScanStage : uint8_t {
    listing,       ///< the count grows
    nested,        ///< NameCheck::nested found: the player picks one (resume with use_nested)
    already_there, ///< the chosen folder is the game folder
    planned,       ///< plan, space and (when it ran) the source check are ready
    failed,        ///< error says why; failure names which
};

/// A scan's state, as the screen reads it.
struct ScanSnapshot {
    ScanStage stage{ScanStage::listing};      ///< where the scan is
    uint32_t files{};                         ///< entries listed so far
    uint64_t bytes{};                         ///< their known sizes
    NameCheck names{};                        ///< the name check
    std::shared_ptr<const ImportPlan> plan{}; ///< the plan, once planned
    std::optional<GameInstall>
        source_check{};          ///< the engine's check on a local source, when it ran
    bool source_check_skipped{}; ///< files are in the cloud: checked once copied
    std::string error{};         ///< why it failed
    /// Why a scan failed.
    enum class Failure : uint8_t {
        none,               ///< it did not fail
        unreadable,         ///< the source could not be read
        not_a_game,         ///< no archives and no installer, here or two levels down
        not_demo_installer, ///< the chosen file is not the demo's installer
        too_large,          ///< far too large
    } failure{};            ///< why it failed
};

/// Lists a source, runs the name check, plans the copy and checks a local source with the
/// engine's own check, on one worker thread.
class SourceScan {
  public:

    /// Makes a scan that has not started.
    SourceScan() = default;
    /// Stops and joins the worker.
    ~SourceScan();
    SourceScan(const SourceScan&) = delete;
    SourceScan& operator=(const SourceScan&) = delete;
    /// Starts a scan.
    ///
    /// @param hooks the platform's hooks
    /// @param paths the import's folders
    /// @param request what to scan
    /// @param[out] error why the worker cannot start
    /// @return false when the worker cannot start
    bool start(
        const GameFilesHooks& hooks,
        const ImportPaths& paths,
        ScanRequest request,
        std::string* error
    );
    /// Continues a scan that stopped at ScanStage::nested with one of NameCheck::nested.
    ///
    /// @param index the candidate in NameCheck::nested
    /// @param[out] error why it cannot continue
    /// @return false when it cannot continue
    bool use_nested(std::size_t index, std::string* error);
    /// Stops listing; the snapshot ends as failed with no error.
    void cancel() noexcept;
    /// Returns the scan's state.
    ///
    /// @return a copy of the latest snapshot
    [[nodiscard]] ScanSnapshot snapshot() const;
    /// Tells whether the worker runs.
    ///
    /// @return true while the worker runs
    [[nodiscard]] bool busy() const noexcept;

  private:

    struct Shared;
    std::shared_ptr<Shared> shared_{};   ///< what the worker and the screen share
    oa::base::threads::Thread thread_{}; ///< the worker
};

/// Where an import run is.
enum class RunStage : uint8_t {
    idle,     ///< not started
    copying,  ///< files are being copied
    checking, ///< the engine's check runs on the copy
    checked,  ///< the check ran; RunSnapshot::check holds it
    stopped,  ///< Stop or the background expiry ended it
    failed,   ///< failure says why
};

/// Why a run failed (RunStage::failed).
enum class RunFailure : uint8_t {
    none,               ///< it did not fail
    no_space,           ///< the disk filled
    unreadable,         ///< a file could not be read
    gone,               ///< the source went away
    offline,            ///< a cloud download failed
    denied,             ///< access to the source was withdrawn
    changed,            ///< a file changed since the plan
    staging_unwritable, ///< the staging folder cannot be written
    not_usable,         ///< the copy cannot be played
};

/// The phase an import's state is in.
enum class ImportPhase : uint8_t {
    copying,    ///< files are being copied, or a copy was interrupted
    checked,    ///< the copy was checked usable and waits to be committed
    committing, ///< the commit's renames are under way
};
/// What an import does.
enum class ImportMode : uint8_t {
    replace, ///< the game folder is replaced by the copy
    add,     ///< files are added to the game folder
    mod,     ///< a mod is added under mods/<id>
    remove,  ///< files are removed from the game folder
};

/// What a run does.
struct RunRequest {
    ImportMode mode{ImportMode::replace}; ///< what the copy is for
    ModChoice mod{};   ///< the check's mod choice (mode mod: the staged mod folder)
    bool check = true; ///< run inspect_game_install on the staging folder after the copy
};

/// A run's state, as the screen reads it.
struct RunSnapshot {
    RunStage stage{RunStage::idle};       ///< where the run is
    RunFailure failure{RunFailure::none}; ///< why it failed
    uint64_t bytes_done{};                ///< staged bytes, finished files plus the current one's
    uint64_t bytes_total{};               ///< bytes the run copies
    uint32_t files_done{};                ///< files finished
    uint32_t files_total{};               ///< files the run copies
    uint32_t files_skipped{};             ///< already staged with the same size and time (resumed)
    std::string current{};                ///< the current file's display name
    bool current_remote{};                ///< it is being downloaded
    std::array<uint8_t, part_count> parts_done{}; ///< 0 waiting, 1 copying, 2 done
    std::vector<uint8_t> mods_done{};             ///< the same per mod
    std::string file{};                           ///< the file a failure names
    std::string error{};                          ///< why it failed
    std::optional<GameInstall> check{};           ///< the engine's check on the copy, once it ran
    bool stopped_by_expiry{};                     ///< stopped by keep_running's expiry, not by Stop
};

/// Copies a plan into the staging folder and checks it, on one worker thread.
class ImportRun {
  public:

    /// Makes a run that has not started.
    ImportRun() = default;
    /// Stops and joins the worker.
    ~ImportRun();
    ImportRun(const ImportRun&) = delete;
    ImportRun& operator=(const ImportRun&) = delete;
    /// Writes the state (phase copying), removes stale .part files, skips staged files that
    /// match, asks keep_running, copies, then checks.
    ///
    /// @param hooks the platform's hooks
    /// @param paths the import's folders
    /// @param plan the plan to copy
    /// @param switches the player's switches
    /// @param request what the run does
    /// @param[out] error why it cannot start
    /// @return false when it cannot start
    bool start(
        const GameFilesHooks& hooks,
        const ImportPaths& paths,
        std::shared_ptr<const ImportPlan> plan,
        const Switches& switches,
        const RunRequest& request,
        std::string* error
    );
    /// Stop: ends at a chunk boundary.
    void stop() noexcept;
    /// The background time ran out: stops, marked by expiry.
    void expire() noexcept;
    /// Returns the run's state.
    ///
    /// @return a copy of the latest snapshot
    [[nodiscard]] RunSnapshot snapshot() const;
    /// Tells whether the worker runs.
    ///
    /// @return true while the worker runs
    [[nodiscard]] bool busy() const noexcept;
    /// Waits for the worker to end.
    void join() noexcept;

  private:

    struct Shared;
    std::shared_ptr<Shared> shared_{};   ///< what the worker and the screen share
    oa::base::threads::Thread thread_{}; ///< the worker
};

/// The import's state file, "key = value" lines, written whole by a rename.
struct ImportState {
    ImportPhase phase{ImportPhase::copying};  ///< where the import is
    ImportMode mode{ImportMode::replace};     ///< what it does
    SourceKind kind{SourceKind::game_folder}; ///< what the picker chose
    std::string location{};                   ///< the source's display location
    uint64_t bytes{};                         ///< planned bytes
    uint32_t files{};                         ///< planned files
    std::array<bool, part_count> parts{};     ///< the switches (all true by default)
    std::vector<std::string> mods_off{};      ///< mod folders switched off
    std::string mod_id{};                     ///< mode mod: the target mods/<id>
    std::vector<std::string>
        removals{}; ///< mode remove: relative paths in the game folder; "*" = all
    /// A folder in the game folder's parent that the commit renames into place instead of the
    /// staging folder (ImportPlan::move_in_place), absolute UTF-8; empty for none.
    std::string move_source{};
};

/// Reads the state file.
///
/// @param file the state file
/// @param[out] error why it cannot be read
/// @return the state; nothing when there is none or it cannot be read (error set then)
[[nodiscard]] std::optional<ImportState>
read_import_state(const fs::path& file, std::string* error);
/// Writes the state file to a temporary file and renames it over the old one.
///
/// @param file the state file
/// @param state what to write
/// @param[out] error why it could not be written
/// @return true when it was written
bool write_import_state(const fs::path& file, const ImportState& state, std::string* error);
/// Sums the finished (non-.part) files in the staging folder.
///
/// @param staging the staging folder
/// @return their bytes; 0 when there is none
[[nodiscard]] uint64_t staged_bytes(const fs::path& staging) noexcept;

/// What a commit did.
struct CommitResult {
    bool ok{};             ///< the commit finished
    fs::path old_folder{}; ///< where an earlier game folder was set aside; empty when none
    std::string error{};   ///< why it failed
};

/// Commits a checked copy: phase committing; the game folder renamed to
/// "Total Annihilation (old)" (" 2", " 3" when taken); staging (or the source itself when
/// move_in_place) renamed to the game folder; backups set (set_backed_up(folder, backed_up));
/// the state removed. Additions and mods: each staged file moved to its place, never over a
/// file that exists.
///
/// @param hooks the platform's hooks
/// @param paths the import's folders
/// @param mode what the import does
/// @param backed_up the player's backup setting
/// @return what the commit did
CommitResult commit_import(
    const GameFilesHooks& hooks, const ImportPaths& paths, ImportMode mode, bool backed_up
);

/// Records removals for the next start (mode remove; merged with any already waiting).
///
/// @param paths the import's folders
/// @param relative_paths paths in the game folder; "*" removes everything
/// @param[out] error why they could not be recorded
/// @return true when they were recorded
bool schedule_removal(
    const ImportPaths& paths, std::span<const std::string> relative_paths, std::string* error
);
/// Records a checked replacement for the next start (phase checked, mode replace).
///
/// @param paths the import's folders
/// @param[out] error why it could not be recorded
/// @return true when it was recorded
bool schedule_replacement(const ImportPaths& paths, std::string* error);
/// Forgets a change waiting for the next start; a waiting replacement's staging is removed.
///
/// @param paths the import's folders
/// @param[out] error why it could not be forgotten
/// @return true when nothing waits any more
bool cancel_scheduled(const ImportPaths& paths, std::string* error);

/// What recovery found at the start.
enum class Recovery : uint8_t {
    nothing,         ///< no state
    finished_commit, ///< a commit was cut short and has been finished
    applied,         ///< a change waiting for this start was applied
    copy_waiting,    ///< a copy was interrupted; S1 shows the continue banner
    unreadable,      ///< the state could not be read; it is left for the player (error)
};

/// What recovery did.
struct RecoveryResult {
    Recovery outcome{Recovery::nothing}; ///< what recovery found
    std::optional<ImportState> state{};  ///< the waiting copy's state
    uint64_t staged_bytes{};             ///< what was copied of it
    CommitResult commit{};               ///< the commit recovery finished or applied
    std::string error{};                 ///< why the state could not be read
};

/// Runs at every start before resolution where the hooks are installed.
///
/// @param hooks the platform's hooks
/// @param paths the import's folders
/// @param backed_up the player's backup setting
/// @return what recovery found and did
RecoveryResult
recover_import(const GameFilesHooks& hooks, const ImportPaths& paths, bool backed_up);
/// Removes the staging folder and the state.
///
/// @param paths the import's folders
/// @param[out] error why they could not be removed
/// @return true when both are gone
bool discard_import(const ImportPaths& paths, std::string* error);
/// Removes an "(old)" folder, or the unused demo data, now (the player confirmed).
///
/// @param folder the folder to remove
/// @param[out] error why it could not be removed
/// @return true when it is gone
bool remove_folder_now(const fs::path& folder, std::string* error);

/// Applies the backup setting to the game folder, the staging folder and the demo data folder.
///
/// @param hooks the platform's hooks; a null set_backed_up does nothing
/// @param paths the import's folders
/// @param backed_up true puts them in the device's backups, false keeps them out
void apply_backup_setting(const GameFilesHooks& hooks, const ImportPaths& paths, bool backed_up);

/// What "I have copied it" found in the game folder's parent.
enum class CopiedFind : uint8_t {
    nothing,         ///< no game folder and nothing to adopt
    game_folder,     ///< the game folder is there: check it
    misnamed_folder, ///< a folder under another name holds archives at its top
    loose_archives,  ///< archives lie loose in the parent
};

/// The find, and its names.
struct CopiedFiles {
    CopiedFind find{CopiedFind::nothing}; ///< what was found
    std::string folder{};                 ///< misnamed_folder: its name
    std::vector<std::string> archives{};  ///< loose_archives: their names
};

/// Looks in the game folder's parent for what the player copied.
///
/// @param paths the import's folders
/// @return what was found
[[nodiscard]] CopiedFiles find_copied_files(const ImportPaths& paths);
/// Renames a misnamed folder into the game folder, or gathers loose archives into it.
///
/// @param paths the import's folders
/// @param found find_copied_files()'s answer
/// @param[out] error why it could not be done
/// @return true when the game folder now holds the files
bool adopt_copied_files(const ImportPaths& paths, const CopiedFiles& found, std::string* error);

/// Relative paths of a part's files in the game folder, for Remove.
///
/// @param hooks the platform's hooks
/// @param paths the import's folders
/// @param part the part
/// @param mod_id for Part::mods: the mod's id
/// @return the files' paths relative to the game folder
[[nodiscard]] std::vector<std::string> part_files(
    const GameFilesHooks& hooks, const ImportPaths& paths, Part part, std::string_view mod_id = {}
);

/// Tells whether a file name is a game archive's, as the name check reads it: a .hpi, .ufo or
/// .ccx file, or rev31.gp3, matched without case.
///
/// @param name the file's name, without folders
/// @return true for an archive name
[[nodiscard]] bool is_archive_name(std::string_view name) noexcept;

/// Returns the part a file of a game folder belongs to, by its path alone (matched without
/// case): the archives of each part at the top, music/, the .zrb movies directly in data/, the
/// files of a mod folder, and the demo's installer at the top of a folder without archives.
///
/// @param relative_path the file's path in the game folder, '/'-separated
/// @param size its size in bytes, for the demo's installer
/// @param folder_has_archives whether the folder holds archives at its top
/// @param mod_folders the mod folders, lower-case "mods/<folder>", each holding an oamod.yaml
/// @return the part
[[nodiscard]] Part part_of(
    std::string_view relative_path,
    uint64_t size,
    bool folder_has_archives,
    std::span<const std::string> mod_folders
);

/// Tells what a plan's copy does: replace for a game folder or the demo's installer, add for
/// archives and an additions folder, mod for an additions folder whose top holds a mod profile
/// (copied into mods/<id>/).
///
/// @param plan the plan
/// @return the mode its ImportRun and commit take
[[nodiscard]] ImportMode import_mode_of(const ImportPlan& plan) noexcept;

/// Returns the name an earlier game folder is set aside under: "Total Annihilation (old)",
/// then "Total Annihilation (old) 2", " 3" and on.
///
/// @param number 1 for the first name, 2 and on for the next
/// @return the folder's name
[[nodiscard]] std::string old_folder_name(uint32_t number);

/// Converts a file time to whole seconds since 1970, rounded down, as SourceEntry::modified
/// holds it.
///
/// @param time a file time
/// @return seconds since 1970
[[nodiscard]] int64_t seconds_since_1970(fs::file_time_type time) noexcept;

/// Converts seconds since 1970 to a file time.
///
/// @param seconds seconds since 1970
/// @return the file time
[[nodiscard]] fs::file_time_type file_time_from_seconds(int64_t seconds) noexcept;

} // namespace oa::app::game_files
