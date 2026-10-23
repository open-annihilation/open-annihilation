// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Packages: a zip archive of one kind of package, installed into that kind's
// folder of the player's own folder under the id its manifest gives. A
// package is read and checked before anything is written (open_package);
// what the kind's root folder holds decides where it goes and what the
// player is asked (the kind's plan); its files are unpacked into a staging
// folder inside that root, a piece at a time (Unpacking), then put in place
// by renames on the same volume (commit_change), which keep the version an
// update replaced in the package's .backup folder, one version back. An
// uninstall moves a package's folder aside whole in one rename
// (uninstall_package). A crash
// at any point leaves folders that the next start settles to the state
// before the change or after it (recover_changes), and the folders a change
// drops are deleted a little at a time (Discarder). One table of kinds says
// what differs: the oamod kind, whose manifest is oamod.yaml and whose root
// folder is Mods, the oamap kind, whose manifest is oamap.yaml and whose
// root folder is Maps, the oalang kind, whose manifest is language.yaml
// and whose root folder is Languages, the expansion kind, whose manifest
// is oaexp.yaml, the oamut kind, whose manifest is oamut.yaml and whose
// root folder is Mutators, and the oabrain kind, whose manifest is
// oabrain.yaml and whose root folder is Brains. An expansion is copied by
// install_oaexp into Expansions/<game>/<id> with the .oa-content record; its
// plan refuses the generic unpack. No function here throws: errors are values.
#pragma once

#include "oa/app/package_install/origin.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/platform/preferences.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app::package_install {

struct Package;
struct Problem;
struct InstallPlan;
struct FolderHooks;
struct PackageKind;
struct KindPrompts;
struct Incoming;

/// The folder inside an installed package's folder that keeps the version an
/// update replaced, which the game never reads (oa::backup_folder_name).
inline constexpr std::string_view backup_folder_name = oa::backup_folder_name;
/// What a folder recovery cannot settle is renamed to, after its target's
/// name, so that it shows in the root folder and never blocks a later change.
inline constexpr std::string_view left_over_suffix = "-left-over";
/// The most a package may unpack to, in bytes.
inline constexpr uint64_t max_install_bytes = uint64_t{4} << 30;
/// The bytes kept free on the disk holding Mods besides the package's files.
inline constexpr uint64_t free_space_margin = uint64_t{64} << 20;
/// The most bytes a package may unpack to for each byte it holds, once it
/// unpacks to more than bomb_floor.
inline constexpr uint64_t bomb_ratio = 200;
/// The unpacked size below which bomb_ratio is not checked, in bytes.
inline constexpr uint64_t bomb_floor = uint64_t{64} << 20;
/// The most folders a package may unpack, those its paths pass through
/// included.
inline constexpr std::size_t max_package_folders = 16'384;
/// What each folder or file an unpacking makes, and each folder it syncs,
/// counts for in a step's budget, in bytes, for the system calls it takes.
inline constexpr uint64_t unpack_entry_cost = uint64_t{16} << 10;
/// The characters each unpacked file's path keeps spare below the longest
/// path the system opens.
inline constexpr uint64_t path_room = 16;
/// The folders Install alongside tries: <id>-<version>, then -2 to -99.
inline constexpr uint32_t alongside_tries = 99;

/// Why a package was not installed.
enum class Refusal : uint8_t {
    none,
    unreadable,         ///< the file cannot be opened or read
    not_zip,            ///< it is not a zip archive
    damaged,            ///< a record or an entry's data is not as recorded
    no_manifest,        ///< no manifest at its top, or in one folder at its top
    manifest_too_large, ///< its manifest is larger than the kind takes
    manifest_errors,    ///< its manifest does not resolve
    unsafe_name,        ///< a name that cannot be unpacked on every system
    case_clash,         ///< two names that differ only in case, or one that exists already
    link,               ///< a link or a special file
    encrypted,          ///< an encrypted entry
    method,             ///< an entry packed other than stored or deflated
    too_large,          ///< it unpacks to more than max_install_bytes
    bomb,               ///< it unpacks to far more than its own size
    too_many_folders,   ///< it unpacks more than max_package_folders folders
    no_space,           ///< the disk holding the root folder has too little room
    path_too_long,      ///< an unpacked path is longer than the system opens
    reserved_id,        ///< its id names a device on Windows
    no_free_folder,     ///< no folder name is free for it alongside the others
    not_placed,         ///< its files could not be put in place
    changed,            ///< the root folder changed while it was being installed
    busy,               ///< another copy of the game is changing the root folder
    unknown_kind,       ///< it is not a kind of package this game installs
    missing_entry,      ///< a map pack lists a file it does not hold
    stray_file,         ///< a map pack holds a file no map uses
    outside_folder,     ///< a map pack holds a file outside the folders it may
    preview_unreadable, ///< a map's preview is not a PNG
    preview_too_big,    ///< a map's preview is larger than 2 MiB or 1024 pixels
    engine_unmet,       ///< requires.engine is not met by this build
    unfit,              ///< a map does not fit the base game
    not_a_pack,         ///< the folder does not hold this map pack
    font_missing,       ///< a language pack lists a font it does not hold
    font_unreadable,    ///< a language pack's font file does not open
    warmup_invalid,     ///< a language pack's warm-up text is missing, too large or not UTF-8
    table_errors,       ///< a language pack's table or interface.tdf does not parse
    script_missing,     ///< a mutator's or a brain's script is missing, or larger than 1 MiB
    precompiled,        ///< a mutator's or a brain's Lua file is precompiled bytecode
    outside_layout,     ///< a mutator or a brain holds a file its layout does not allow
    icon_invalid,       ///< a mutator's or a brain's icon is not a square PNG within the limits
    network_missing,    ///< a brain names a network file it does not hold
    networks_too_large, ///< a brain's networks together are larger than 64 MiB
    data_too_large,     ///< a brain's data table is larger than 4 MiB
    network_mismatch,   ///< a brain's network is not the SHA-256 its manifest names
};

/// What went wrong, for the player's text and the log.
struct Problem {
    Refusal refusal{Refusal::none};
    std::string subject{};            ///< the entry name or id the reason names
    std::vector<std::string> lines{}; ///< profile diagnostics, one each (manifest_errors)
    std::string detail{};             ///< English, for the log: a system error, a zip status
    /// The size the reason names: the unpacked size (too_large, bomb) or
    /// the room needed (no_space).
    uint64_t size_bytes{};
    /// The size it is measured against: the limit (too_large), the
    /// package's own size (bomb) or the room free (no_space).
    uint64_t limit_bytes{};
};

/// One file or folder a package unpacks.
struct PackagedFile {
    std::size_t entry{}; ///< its entry in the package's directory
    /// Its path below the mod's folder, '/' between folders, with the
    /// package's top folder stripped; a folder's ends in '/'.
    std::string name{};
};

/// A package as its manifest names it.
struct Incoming {
    std::string id{};
    std::string name{};
    std::string version{};
    int64_t revision{};
    /// Where the package came from, which the runtime copies from the inbox
    /// before it plans. A plan may read it; the oamod plan does not.
    Origin origin{};
    /// How many maps an oamap pack lists, for its install question. 0 for
    /// every other kind.
    uint32_t maps{};
    /// A language pack's name in English, for its install question. Empty
    /// for every other kind.
    std::string english_name{};
};

/// A package, read and checked: what it installs and where its files come from.
struct Package {
    const PackageKind* kind{};    ///< the kind its extension named; null before it is known
    std::filesystem::path file{}; ///< the package
    std::string file_name{};      ///< its name, UTF-8, which the texts name
    uint64_t archive_bytes{};     ///< its size
    oa::formats::zip::StreamDirectory directory{};
    std::string root{};                ///< the stripped top folder with its '/', or empty
    std::vector<PackagedFile> files{}; ///< what it unpacks, in archive order
    /// Every folder it unpacks, those its paths pass through included, '/'
    /// between parts, each after the folder that holds it.
    std::vector<std::string> folders{};
    std::size_t manifest_entry{}; ///< the manifest's entry
    Incoming incoming{};          ///< what the manifest installs, set by the kind
    /// The oamod kind's resolved profile; null for every other kind.
    std::shared_ptr<const oa::data::mod_profile::ModProfile> profile{};
    std::vector<std::string> warnings{}; ///< the manifest's warnings, formatted
    uint64_t unpacked_bytes{};           ///< its files' sizes added up
    bool backup_left_out{}; ///< it held a .backup folder of its own, which is not unpacked
    /// It held a .oa-origin.yaml at its top, which is not unpacked.
    bool origin_left_out{};
};

/// How a package's manifest is resolved.
struct PackageOptions {
    /// Accept hacks this build does not implement yet, with a warning each.
    bool accept_unimplemented_hacks{};
    /// The player's preferences, whose registry section the oamod profile's
    /// registry bindings read; null for none.
    const oa::platform::preferences::Values* preferences{};
    /// The game folder the oamod kind layers over, whose INI file the
    /// profile's settings come from when the package holds none; empty for none.
    std::filesystem::path game_folder{};
    /// What a kind's hooks need from the app, which the runtime sets per kind
    /// (Runtime::package_options). Null for oamod. A map pack's points at
    /// oamap::FitHooks.
    void* context{};
};

/// A package read, or why not.
struct PackageResult {
    std::optional<Package> package{}; ///< set when it can be installed
    Problem problem{};                ///< why not; refusal none when it can
};

/// Reads and checks a package, writing nothing: its directory, the names
/// and sizes of what it unpacks, and its manifest, which the kind reads.
/// The first problem found refuses it. A file whose extension is no kind's
/// is refused as unknown_kind, and nothing is read.
///
/// It ignores __MACOSX folders, .DS_Store files and AppleDouble files (a
/// last part starting "._"), and a .backup folder of its own. Its manifest
/// lies at its top, matched without case, or in the one folder its top
/// holds, which is stripped, as the Finder's Compress makes it.
///
/// @param file the package
/// @param options how its manifest is resolved; a kind's hooks read context
/// @return the package, or the problem
[[nodiscard]] PackageResult
open_package(const std::filesystem::path& file, const PackageOptions& options = {});

/// Says why a relative path in a package cannot be unpacked on every system
/// the game runs on. Each part must be 1 to 255 bytes of valid UTF-8 with no
/// control character, none of < > : " | ? * and backslash, not end in '.' or
/// ' ', and its part before the first '.' must not be a name Windows keeps
/// for a device (CON, PRN, AUX, NUL, COM0 to COM9, LPT0 to LPT9, COM and LPT
/// with a superscript 1, 2 or 3, CONIN$ and CONOUT$, in any case).
///
/// @param relative_name the path, '/' between its parts; a folder's may end in '/'
/// @return the reason, in English; empty when it can be unpacked
[[nodiscard]] std::string portable_name_problem(std::string_view relative_name);

/// Tells whether a name is one Windows keeps for a device, in any case.
///
/// @param name the name, without an extension
/// @return true for CON, PRN, AUX, NUL, COM0 to COM9, LPT0 to LPT9, and the others
[[nodiscard]] bool windows_device_name(std::string_view name) noexcept;

/// What a folder in a kind's root holds.
enum class FolderKind : uint8_t {
    missing, ///< nothing of that name
    /// a file, a link or junction, a folder without a manifest, or one
    /// whose manifest or id cannot be read
    other,
    package, ///< a folder whose manifest gives an id
};

/// What a folder holds, as its kind reads the manifest, without the kind's full check.
struct InstalledPackage {
    FolderKind kind{FolderKind::missing};
    std::string id{};      ///< the profile's id
    std::string name{};    ///< its name
    std::string version{}; ///< its version, as written
    int64_t revision{};    ///< packaging.revision; 0 when it cannot be read
    /// The folder's origin record; nothing when it has none. A missing
    /// record is not an error.
    std::optional<Origin> origin{};
    /// The manifest's author.name, as written; empty when it has none or the
    /// kind's manifest names no author.
    std::string author{};
    /// The manifest's description, as written; empty when it has none or the
    /// kind's manifest gives none.
    std::string description{};
    /// Base games the package runs with, from its manifest. totala when a
    /// mod's list cannot be read.
    std::vector<std::string> supported_games{};
    /// Expansions that must be on. Empty when the manifest names none.
    std::vector<std::string> required_expansions{};
};

/// Tells whether two reads of a folder found the same: kind, id, version
/// and revision.
///
/// @param left one read
/// @param right the other
/// @return true when they agree
[[nodiscard]] bool
same_package(const InstalledPackage& left, const InstalledPackage& right) noexcept;

/// The bit of a Windows reparse tag that marks a name surrogate
/// (IsReparseTagNameSurrogate): a reparse point that names another place.
inline constexpr uint32_t reparse_tag_name_surrogate = 0x20000000U;

/// Tells whether a Windows reparse point with this tag is a link: a name
/// surrogate, such as a junction or mount point (IO_REPARSE_TAG_MOUNT_POINT)
/// or a symbolic link (IO_REPARSE_TAG_SYMLINK), which names another place.
/// Any other tag, such as the cloud tags a file sync puts on the folders and
/// files it keeps, marks a folder or file that is itself.
///
/// @param tag the reparse tag
/// @return true for a name surrogate
[[nodiscard]] constexpr bool reparse_tag_is_link(uint32_t tag) noexcept {
    return (tag & reparse_tag_name_surrogate) != 0;
}

/// Tells whether a path is a link: a symbolic link, or on Windows a
/// reparse point whose tag is a name surrogate (reparse_tag_is_link), such
/// as a junction; something that is removed, never entered. A reparse point
/// whose tag cannot be read counts as a link.
///
/// @param path the path
/// @return true for a link or junction
[[nodiscard]] bool is_link_or_junction(const std::filesystem::path& path);

/// What an install does, or asks first.
enum class PlanKind : uint8_t {
    install,       ///< no question: unpack into `target`
    ask_update,    ///< the same version, another revision: Replace | Cancel
    ask_reinstall, ///< the same version and revision: Reinstall | Cancel
    ask_version,   ///< another version installed: Replace | Install alongside | Cancel
    ask_alongside, ///< the folder holds something else: Install alongside | Cancel
    refuse,        ///< refused: no folder is free, or the kind's own reason
    ask_install,   ///< a new map pack the player opened: Install | Cancel
};

/// Where an install goes and what it asks.
struct InstallPlan {
    PlanKind kind{PlanKind::install};
    std::string target{};    ///< the folder in the root that install, replace and reinstall act on
    std::string alongside{}; ///< the folder Install alongside makes; empty when not offered
    InstalledPackage installed{};             ///< what `target` holds now
    std::optional<InstalledPackage> backup{}; ///< what target/.backup holds now
    bool older{}; ///< ask_update: the incoming revision is below the installed one
    /// install: the target already holds this pack and is replaced, keeping
    /// one .backup. A kind that leaves this false installs into an empty target.
    bool replacing{};
    /// install: the target holds this pack at the same version and revision,
    /// and the files are installed again. The kept version stays.
    bool reinstalling{};
    /// refuse: why. no_free_folder when the kind does not set another.
    Refusal refusal{Refusal::no_free_folder};
};

/// What the folders of a kind's root hold, for the planner.
struct FolderHooks {
    void* context{}; ///< passed back to each function
    /// Returns what a folder of the root, by its name, holds; null finds every
    /// folder missing.
    InstalledPackage (*look)(void* context, std::string_view folder){};
    /// Returns what a folder's .backup holds; null finds none.
    std::optional<InstalledPackage> (*backup_of)(void* context, std::string_view folder){};
    /// Keeps the copied root path folder_hooks reads, so the hooks outlive
    /// the path they were given. Empty when the caller sets context itself.
    std::shared_ptr<void> owned{};
};

/// How a kind's manifest reader fares when it asks for another file of the package.
enum class EntryRead : uint8_t {
    missing,    ///< no entry has that name
    read,       ///< the bytes were read
    unreadable, ///< the entry is there, and larger than asked or not read
};

/// What a kind's manifest reader may use besides the manifest's bytes.
struct ManifestContext {
    const PackageOptions* options{}; ///< never null
    std::string source{};            ///< "<file name>/<manifest name>", for diagnostics
    void* reader{};                  ///< passed back to read_entry
    /// Reads a file below the package's top folder, matched without case, whole,
    /// when it is at most most_bytes.
    EntryRead (*read_entry)(
        void* reader,
        std::string_view relative_name,
        uint64_t most_bytes,
        std::vector<uint8_t>& bytes
    ){};
};

/// One kind of package (design section 02).
struct PackageKind {
    std::string_view name{};           ///< the catalogue's kind: "oamod"
    std::string_view extension{};      ///< ".oamod", matched without case
    std::string_view manifest{};       ///< "oamod.yaml", matched without case
    std::size_t manifest_most_bytes{}; ///< larger refuses as manifest_too_large
    std::string_view root_folder{};    ///< its folder in the player's folder: "Mods"
    std::string_view prefix{};         ///< the installer's own folders and lock there: ".oamod-"
    /// Reads the manifest and checks the kind's own rules: fills package.incoming,
    /// package.warnings and the kind's own fields; false with problem set to refuse.
    bool (*read_manifest)(
        std::span<const uint8_t> manifest,
        const ManifestContext& context,
        Package& package,
        Problem& problem
    ){};
    InstalledPackage (*read_installed)(const std::filesystem::path& folder){};
    std::optional<InstalledPackage> (*read_backup)(const std::filesystem::path& folder){};
    InstallPlan (*plan)(const Incoming& incoming, const FolderHooks& hooks){};
    /// Checks the unpacked files before they are put in place; null checks nothing.
    bool (*check_staged)(
        const std::filesystem::path& staging,
        const Package& package,
        const PackageOptions& options,
        Problem& problem
    ){};
    const KindPrompts* prompts{};
};

/// Returns every kind the game installs, in table order.
///
/// @return the kinds; oamod, then oamap, then oalang, then expansion, then oamut, then oabrain
[[nodiscard]] std::span<const PackageKind> package_kinds() noexcept;

/// Returns the kind whose extension a file's name ends with, matched without case.
///
/// @param file the path
/// @return the kind; null when the name is not a package of a known kind
[[nodiscard]] const PackageKind* kind_for_file(const std::filesystem::path& file) noexcept;

/// Returns the kind of a catalogue name.
///
/// @param name the kind's name, such as "oamod"
/// @return the kind; null when the table holds none of that name
[[nodiscard]] const PackageKind* find_kind(std::string_view name) noexcept;

/// Returns the oamod kind.
///
/// @return the kind
[[nodiscard]] const PackageKind& mod_kind() noexcept;

/// The names of the installer's own entries in a kind's root folder.
struct FolderNames {
    std::string reserved{}; ///< the prefix itself
    std::string staging{};  ///< files unpacked before they are put in place
    std::string old{};      ///< the version leaving the target
    std::string replaced{}; ///< the version a reinstall drops
    std::string restore{};  ///< the kept version a roll back brings back
    std::string discard{};  ///< a folder to be deleted
    std::string lock{};     ///< the file a copy of the game holds while it changes the root
};

/// Returns the installer's own names in a kind's root folder, each built from its prefix.
///
/// @param kind the kind
/// @return reserved, staging, old, replaced, restore, discard and lock
[[nodiscard]] FolderNames folder_names(const PackageKind& kind);

/// Returns the hooks that read a kind's real root folder.
///
/// The path is copied, so it need not outlive the hooks. The kind must.
///
/// @param kind the kind, whose read_installed and read_backup the hooks call
/// @param root the kind's root folder
/// @return the hooks
[[nodiscard]] FolderHooks folder_hooks(const PackageKind& kind, const std::filesystem::path& root);

/// A lock on a file, held for the life of the object, which another process,
/// or another FileLock in this one, cannot take while it is held. The file
/// stays open: on Windows others may read it but not write it; elsewhere it
/// carries an exclusive advisory lock. No child process inherits it.
class FileLock {
  public:

    /// Takes the lock, making the file when it is missing.
    ///
    /// @param file the lock file
    /// @return the lock; null when another holder has it or the file cannot be made
    [[nodiscard]] static std::unique_ptr<FileLock> take(const std::filesystem::path& file);

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    /// Releases the lock and closes the file.
    ~FileLock();

  private:

    FileLock() = default;
#ifdef _WIN32
    void* handle_{};
#else
    int descriptor_{-1};
#endif
};

/// A hold on a root folder's lock, shared by every hold this process takes
/// on the same folder: changes and recovery in one process never wait for
/// each other, and another copy of the game cannot change the folder while
/// any is held.
using RootHold = std::shared_ptr<const FileLock>;

/// Takes a hold on a kind's root folder's lock, making the lock file when
/// it is missing.
///
/// @param kind the kind, whose lock name the file takes
/// @param root the root folder, which must exist
/// @return the hold; null while another copy of the game holds the lock
[[nodiscard]] RootHold hold_root(const PackageKind& kind, const std::filesystem::path& root);

/// Stand-ins for the unpacking's reads of the file system.
struct UnpackHooks {
    void* context{}; ///< passed back to each function
    /// Returns the bytes free on the disk that holds a folder; null asks the system.
    uint64_t (*available)(void* context, const std::filesystem::path& folder){};
    /// Runs just before a file of the package is made in staging; null does
    /// nothing. A test makes the file there first, as a name the file system
    /// folds to another would be.
    void (*before_file)(void* context, const std::filesystem::path& file){};
};

/// A package's files unpacked into a staging folder inside its kind's root,
/// a piece at a time: its folders made, then each file made anew (never over
/// another), written, flushed and synced, its size and CRC-32 checked, then
/// every folder synced and, on macOS, the drive's cache written to its
/// storage. A failure turns the staging folder into a discard folder;
/// nothing else in the root changes.
class Unpacking {
  public:

    /// Makes an unpacking that has not started.
    Unpacking();

    /// Ends the unpacking; a staging folder it left stays for recovery or
    /// for its change.
    ~Unpacking();
    Unpacking(const Unpacking&) = delete;
    Unpacking& operator=(const Unpacking&) = delete;

    /// Starts: makes the root, takes its lock, checks that the target still
    /// holds what the plan found, that the disk has room for the files and
    /// free_space_margin and that no unpacked path is too long, then makes
    /// the staging folder afresh (an old one becomes a discard folder) and
    /// opens the package. The kind is package.kind.
    ///
    /// @param package the package
    /// @param root the kind's root folder
    /// @param target the folder in the root the files are for
    /// @param expected what the target held when the plan was made
    /// @param origin where the package came from; its installed date is
    ///        written as given, and its sha256, when set, is checked against
    ///        the package file
    /// @param[out] problem why it cannot start
    /// @param hooks stand-ins for the file system's reads
    /// @return true when the unpacking started
    [[nodiscard]] bool start(
        const Package& package,
        const std::filesystem::path& root,
        std::string_view target,
        const InstalledPackage& expected,
        const Origin& origin,
        Problem& problem,
        const UnpackHooks& hooks = {}
    );

    /// Unpacks on until `budget` bytes are read, first of the package file
    /// while its SHA-256 is computed and then of its entries, each folder
    /// or file made and each folder synced counting as unpack_entry_cost
    /// bytes, or until about 4 ms have passed, or the end.
    ///
    /// @param budget the package bytes this step may read
    /// @param[out] problem why it failed
    /// @return more, done once every file is in staging and synced, or failed
    [[nodiscard]] oa::formats::zip::StreamStep step(uint64_t budget, Problem& problem);

    /// Stops: the staging folder becomes a discard folder.
    void cancel() noexcept;

    /// Returns the uncompressed bytes written so far.
    ///
    /// @return the bytes
    [[nodiscard]] uint64_t done_bytes() const noexcept;

    /// Returns the uncompressed bytes the package unpacks to.
    ///
    /// @return the bytes
    [[nodiscard]] uint64_t total_bytes() const noexcept;

    /// Tells whether the unpacking is still hashing the package file.
    ///
    /// @return true until the SHA-256 is known, and false once it failed or finished
    [[nodiscard]] bool checking() const noexcept;

    /// Returns the package-file bytes hashed so far.
    ///
    /// @return the bytes
    [[nodiscard]] uint64_t checked_bytes() const noexcept;

    /// Returns the package file's size, which the hash phase reads.
    ///
    /// @return the bytes; 0 before start
    [[nodiscard]] uint64_t archive_bytes() const noexcept;

    /// Returns the staging folder.
    ///
    /// @return its path; empty before start
    [[nodiscard]] std::filesystem::path staging() const;

    /// Hands over the hold on the root folder's lock, for the change that
    /// follows the unpacking.
    ///
    /// @return the hold; null when none is held
    [[nodiscard]] RootHold take_hold() noexcept;

    /// Returns the discard folders a failure or a cancel made, for deletion.
    ///
    /// @return their paths
    [[nodiscard]] const std::vector<std::filesystem::path>& discards() const noexcept;

  private:

    struct State;

    /// Moves the staging folder aside to be deleted, as a failed or stopped
    /// unpacking leaves it, closing the file being written.
    void discard_staging() noexcept;

    std::unique_ptr<State> state_;
};

/// A change to a folder of a kind's root.
enum class Change : uint8_t {
    install,   ///< the staged files become the target, which is missing
    replace,   ///< the staged files replace the target, which becomes its .backup
    reinstall, ///< the staged files replace the target, which is dropped; its .backup stays
    roll_back, ///< the target and its .backup swap places
};

/// Stand-ins for the change's renames and its waits between tries.
struct ChangeHooks {
    void* context{}; ///< passed back to each function
    /// Renames `from` to `to`, failing when `to` exists, and sets `error`
    /// when it fails; null renames exclusively (rename_exclusively).
    void (*rename)(
        void* context,
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::error_code& error
    ){};
    /// Waits a while before a rename is tried again, as another program,
    /// such as a file sync or a virus scanner, may hold its files; null sleeps.
    void (*wait)(void* context, uint32_t milliseconds){};
};

/// How a change runs.
struct ChangeOptions {
    ChangeHooks hooks{};
    /// The first wait before a refused rename is tried again, in
    /// milliseconds: each wait doubles, up to ten times this, until
    /// most_wait_ms have passed. 0 tries again at once, up to eight times.
    uint32_t first_retry_ms{100};
    /// The most a rename waits in all, in milliseconds.
    uint32_t most_wait_ms{10'000};
    /// What the target must still hold; nothing checks nothing.
    std::optional<InstalledPackage> expected{};
};

/// What a change did.
struct ChangeResult {
    bool changed{};     ///< the new files, or the kept version, are in the target
    bool backup_kept{}; ///< target/.backup holds the version the target held before
    /// A folder the change could not settle, which holds the version the
    /// target held: the player is told where it is; empty for none.
    std::filesystem::path left_over{};
    Refusal refusal{Refusal::none}; ///< why it changed nothing: not_placed, changed or busy
    std::string detail{};           ///< English, for the log
    std::vector<std::filesystem::path> discards{}; ///< folders to delete (Discarder)
};

/// Renames a file or folder, failing when the new name exists, as one step
/// of the file system: never replacing what is there.
///
/// @param from the path
/// @param to the new path
/// @param[out] error set when it fails; on Windows the system's own error
void rename_exclusively(
    const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error
) noexcept;

/// Puts a change in place with renames on the same volume, each tried again
/// for a while when another program holds the files. Install moves the
/// staged files to the target. Replace moves the target aside, the staged
/// files in, drops the old version's own .backup and moves the old version
/// into the target's .backup. Reinstall moves the target aside, the staged
/// files in, the old version's .backup over and drops the old version.
/// Roll back swaps the target and its .backup. A failure before the new
/// files are in place undoes what was done; after it, the rest is tried
/// once more, and what it could not settle is renamed to a visible
/// <target>-left-over folder. Roll back refuses a .backup that is a link
/// or junction. Once the files are in place, the folders the renames
/// changed are synced, as the staging folder's files were. The kind's own
/// folder names (folder_names) name the staging folder and the others.
///
/// @param kind the kind
/// @param root the kind's root folder
/// @param target the folder in the root
/// @param change what to do
/// @param options the hooks, the waits and what the target must hold
/// @return what it did
[[nodiscard]] ChangeResult commit_change(
    const PackageKind& kind,
    const std::filesystem::path& root,
    std::string_view target,
    Change change,
    const ChangeOptions& options = {}
);

/// What an uninstall did.
struct UninstallResult {
    /// The package's folder, with the copy ROLL BACK keeps inside it, left
    /// the root folder whole: it is now the discard folder `discards` names.
    bool removed{};
    /// Why it removed nothing: unsafe_name, busy, link, changed or not_placed.
    Refusal refusal{Refusal::none};
    std::string detail{}; ///< English, for the log
    /// The discard folder the package's folder became, to delete (Discarder).
    std::vector<std::filesystem::path> discards{};
};

/// Tells whether a name is one plain folder of a root that an uninstall may
/// act on: 1 to 255 bytes, no separator, colon or control character, not "."
/// or "..", and not starting with '.', as the installer's own folders do.
///
/// @param target the folder's name, UTF-8
/// @return true when it is
[[nodiscard]] bool plain_folder_name(std::string_view target) noexcept;

/// Uninstalls a package: moves its folder in a kind's root, and with it the
/// copy ROLL BACK keeps inside it (.backup), aside to a discard folder in
/// one rename on the same volume, tried again for a while as a change's
/// renames are. The Discarder then deletes that folder a little at a time,
/// removing links and junctions and never entering them, and the next
/// start's recover_changes deletes whatever a stop left of it. The discard
/// folder's name starts with the kind's prefix, so nothing ever reads it as
/// a package: the root holds the package whole, or not at all.
///
/// Removes nothing, and says why, for a target that is not a plain folder
/// name (unsafe_name); while another copy of the game holds the root
/// (busy); for a target that is a link or junction (link), which is never
/// followed; for one that is missing, not a folder, holds no package of the
/// kind or not what `options.expected` names (changed); while the target's
/// staging, old, replaced or restore folder lies in the root, which an
/// unfinished change or recovery would put back (busy); and when the rename
/// fails (not_placed), the folder then staying whole where it was. Once the
/// folder has moved, the root is synced and, on macOS, the drive's cache
/// written to its storage.
///
/// @param kind the kind
/// @param root the kind's root folder in the player's folder
/// @param target the package's folder in the root, a plain folder name
/// @param options the hooks, the waits and what the target must hold
/// @return what it did
[[nodiscard]] UninstallResult uninstall_package(
    const PackageKind& kind,
    const std::filesystem::path& root,
    std::string_view target,
    const ChangeOptions& options = {}
);

/// What recovery did.
struct Recovery {
    std::vector<std::string> lines{};              ///< what it did, for the log
    std::vector<std::filesystem::path> discards{}; ///< folders to delete (Discarder)
    bool skipped{}; ///< another copy of the game held the folder's lock: nothing was done
};

/// Settles every change a stop left in a kind's root folder to the state
/// before it or after it, by the folders it left, and lists the folders to
/// delete. A folder it cannot settle is renamed to a visible
/// <target>-left-over folder. Nothing is done while another copy of the game
/// holds the lock.
///
/// @param kind the kind, whose folder names the change left
/// @param root the kind's root folder
/// @param options the hooks and the waits
/// @return what it did
[[nodiscard]] Recovery recover_changes(
    const PackageKind& kind, const std::filesystem::path& root, const ChangeOptions& options = {}
);

/// Deletes discard folders a little at a time: each step removes at most
/// 64 entries, or works 4 ms, under the lock of the kind whose prefix the
/// first folder's name starts with. Links and junctions, a discard folder
/// that is one included, are removed, never entered; read-only files are
/// made writable first. An entry that cannot be removed is left, and logged once.
class Discarder {
  public:

    /// Adds folders to delete.
    ///
    /// @param folders their paths, inside one root folder
    void add(std::span<const std::filesystem::path> folders);

    /// Deletes a little more, under the root folder's lock.
    ///
    /// @return true while anything is left to delete
    bool step();

    /// Tells whether anything is left to delete.
    ///
    /// @return true while it is
    [[nodiscard]] bool busy() const noexcept;

  private:

    std::vector<std::filesystem::path> folders_;
    std::vector<std::filesystem::path> stack_;   ///< the folders being emptied, the deepest last
    std::vector<std::filesystem::path> skipped_; ///< entries that could not be removed
};

/// A file made anew, never over another, written and synced to the disk
/// before it is closed: on macOS, to the drive, whose own cache the
/// unpacking has written to storage once its last file is made.
class NewFile {
  public:

    NewFile() = default;
    NewFile(const NewFile&) = delete;
    NewFile& operator=(const NewFile&) = delete;

    /// Closes the file when it is open.
    ~NewFile();

    /// Makes the file, failing when anything of its name exists.
    ///
    /// @param file the path
    /// @param[out] error set when it fails; std::errc::file_exists when it exists
    /// @return true when it was made
    [[nodiscard]] bool create(const std::filesystem::path& file, std::error_code& error) noexcept;

    /// Writes bytes at its end.
    ///
    /// @param bytes the bytes
    /// @return true when all were written
    [[nodiscard]] bool write(std::span<const uint8_t> bytes) noexcept;

    /// Flushes and syncs the file to the disk and closes it.
    ///
    /// @return true when it reached the disk
    [[nodiscard]] bool finish() noexcept;

    /// Closes the file as it is, when it is open, without syncing it.
    void close() noexcept;

  private:

#ifdef _WIN32
    void* handle_{};
#else
    int descriptor_{-1};
#endif
};

} // namespace oa::app::package_install
