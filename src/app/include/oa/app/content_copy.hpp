// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's own copy of a base game and its expansions. Files are read from
// a source (a folder, or a Windows installer) and written under the player's
// folder, in Games/<bundle> and
// Expansions/<base>/<bundle>. Each bundle folder holds a .oa-content record
// of where the files came from and each file's size, modification time and
// SHA-256. The source is only read. Each start checks a copied bundle against
// that record: a file whose size and time still match is not read again, and
// one whose SHA-256 is not in the table marks the bundle as differing. A
// bundle this run's copy already checked is not read again at that start.
#pragma once

#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/record.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <memory>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// Bytes set aside for the record when asking whether the disk has room.
inline constexpr uint64_t content_copy_margin = uint64_t{1024} * 1024;
/// Bytes read and written at a time while a file is copied.
inline constexpr uint64_t content_copy_chunk_bytes = uint64_t{1024} * 1024;
/// The folder of a base game under the player's folder. Named once, in
/// content_bundles.
inline constexpr std::string_view managed_games_folder =
    oa::data::content_bundles::managed_games_folder;
/// The folder of expansions under the player's folder. Named once, in
/// content_bundles.
inline constexpr std::string_view managed_expansions_folder =
    oa::data::content_bundles::managed_expansions_folder;
/// The record written in a bundle folder.
inline constexpr std::string_view content_record_name =
    oa::data::content_bundles::content_record_name;

/// How many bytes to require free before a copy.
///
/// Shared blocks need only the margin, because the file bytes are not
/// written twice. A byte copy needs the remaining bytes and the margin.
/// The sum stops at the largest value rather than wrapping.
///
/// @param remaining bytes a byte copy still has to write
/// @param blocks_shared true when the volume will share the file's blocks
/// @return the free space to require
[[nodiscard]] inline uint64_t
bytes_to_make_room_for(uint64_t remaining, bool blocks_shared) noexcept {
    if (blocks_shared)
        return content_copy_margin;
    constexpr uint64_t limit = std::numeric_limits<uint64_t>::max();
    if (remaining > limit - content_copy_margin)
        return limit;
    return remaining + content_copy_margin;
}

/// Reports whether the free space covers what the copy needs.
///
/// Unknown free space is treated as enough: the copy tries, and stops if
/// the disk fills.
///
/// @param free_bytes free bytes, ignored unless `free_known`
/// @param free_known true when `free_bytes` was read from the volume
/// @param need bytes required
/// @return true when the copy may start
[[nodiscard]] inline bool
room_is_enough(uint64_t free_bytes, bool free_known, uint64_t need) noexcept {
    if (!free_known)
        return true;
    return free_bytes >= need;
}

/// What a start does with the managed copy.
enum class StartCopy : uint8_t {
    leave_alone, ///< --game-dir: the named folder is used as it is
    copy,        ///< an attended start: copy, or skip files already copied
    use_copy,    ///< an unattended start that already has a base game
    refuse,      ///< an unattended start with no base game
};

/// Decides whether this start copies.
///
/// A named --game-dir never copies, attended or not. An unattended start
/// never copies: it uses a base game already in the player's folder, or it
/// is refused. Any other start copies.
///
/// @param game_dir_given true when the command line named --game-dir
/// @param unattended true when nobody is watching this run
/// @param copy_exists true when a managed base game is already recorded
/// @return what the start does
[[nodiscard]] inline StartCopy
start_copy(bool game_dir_given, bool unattended, bool copy_exists) noexcept {
    if (game_dir_given)
        return StartCopy::leave_alone;
    if (unattended)
        return copy_exists ? StartCopy::use_copy : StartCopy::refuse;
    return StartCopy::copy;
}

/// What an unattended start is told when it has no managed base game.
///
/// @return the message shown to the player
[[nodiscard]] std::string_view unattended_copy_refusal() noexcept;

/// One file a source lists.
struct ContentSourceFile {
    /// Path relative to the source's root, with `/` between parts.
    std::string name;
    /// Size in bytes.
    uint64_t size{};
    /// Modification time, seconds since 1970; 0 when the source has none.
    int64_t modified{};
};

/// How far streaming one file got.
struct StreamResult {
    /// SHA-256 of the bytes written, when the file was finished.
    oa::base::sha256::Digest digest{};
    /// Bytes of the destination after the call, including a resumed prefix.
    uint64_t bytes{};
    /// True when the stop flag was honoured. The destination is left in place.
    bool stopped{};
    /// True when the disk filled.
    bool no_space{};
};

/// What streaming one file is asked to do.
struct StreamRequest {
    /// The listed name, relative to the source root.
    std::string_view name;
    /// Where the bytes are written. An existing shorter file is continued.
    std::filesystem::path destination;
    /// Bytes read at a time. 0 uses content_copy_chunk_bytes.
    uint64_t chunk_bytes{};
    /// Set to stop between chunks. Null never stops. The destination is kept.
    const std::atomic<bool>* stop{};
    /// Called after each chunk with the destination's size so far. May be null.
    void (*on_chunk)(void* context, uint64_t bytes_written){};
    /// Passed to `on_chunk`.
    void* chunk_context{};
};

struct CopyRequest;
struct CopyResult;

/// A place game files can be read from, without running an installer.
///
/// A folder implements it. An installer implements the same three operations:
/// open, list and stream one named file. Nothing in the interface writes the
/// source.
class ContentSource {
  public:

    virtual ~ContentSource() = default;

    /// Returns the place the player named, before a root inside it is found.
    ///
    /// @return that place, as text
    [[nodiscard]] virtual std::string origin() const = 0;

    /// Prepares the source. A second call is the same as the first.
    ///
    /// @param[out] error why it could not be opened; may be null
    /// @return true when list and stream_file can be used
    [[nodiscard]] virtual bool open(std::string* error) = 0;

    /// Lists the source's files.
    ///
    /// @param[out] files the files, replaced with the listing
    /// @param[out] error why they could not be listed; may be null
    /// @return true when `files` is the listing
    [[nodiscard]] virtual bool list(std::vector<ContentSourceFile>& files, std::string* error) = 0;

    /// Writes one listed file to `request.destination`, hashing it on the way.
    ///
    /// A destination that already holds a prefix of the file is continued
    /// from that prefix. A stop leaves that file in place. The source is
    /// only read.
    ///
    /// @param request what to write, and when to stop
    /// @param[out] result how far the write got
    /// @param[out] error why it failed; may be null
    /// @return true when the file was finished or the stop was honoured
    [[nodiscard]] virtual bool
    stream_file(const StreamRequest& request, StreamResult& result, std::string* error) = 0;

    /// Copies every matched file in one pass.
    ///
    /// The default does nothing and returns false, and the copy then reads
    /// the source one file at a time. A source whose files come in order
    /// returns true and has finished the copy, including a copy that failed:
    /// `result` says how.
    ///
    /// @param request where the files go, and when to stop
    /// @param[out] result how the copy ended, when this returns true
    /// @return true when this source copied the files itself
    [[nodiscard]] virtual bool copy_together(const CopyRequest& request, CopyResult& result) {
        (void)request;
        (void)result;
        return false;
    }

    /// Resolves a listed name to a file on disk, for a volume that can share
    /// its blocks.
    ///
    /// A source that only streams, such as an installer, leaves this false.
    ///
    /// @param name the listed name
    /// @param[out] resolved the file, when one exists
    /// @param[out] error why it could not be resolved; may be null
    /// @return true when `resolved` is a file that can be read
    [[nodiscard]] virtual bool
    locate(std::string_view name, std::filesystem::path& resolved, std::string* error) {
        (void)name;
        (void)resolved;
        (void)error;
        return false;
    }

    /// Returns the directory the files are read from, when they are on disk.
    ///
    /// @return that directory, or empty when the source has none
    [[nodiscard]] virtual std::filesystem::path disk_root() const { return {}; }
};

/// A folder of game files. Names match without regard to ASCII case, and a
/// Wineskin layout is read from the game folder inside its drive_c.
class FolderContentSource final : public ContentSource {
  public:

    /// Takes the folder the player named and the table its files are matched
    /// against. The table has to outlive the source.
    ///
    /// @param folder the folder the player named
    /// @param bundles the bundles a file of the right name and size belongs to
    FolderContentSource(
        std::filesystem::path folder, std::span<const oa::data::content_bundles::Bundle> bundles
    );

    /// The folder the player named, as the copy's record keeps it.
    [[nodiscard]] std::string origin() const override;
    /// Finds the game folder in the named folder, or in a Wineskin drive_c.
    [[nodiscard]] bool open(std::string* error) override;
    /// Lists the game folder's files that match the table by name and size.
    [[nodiscard]] bool list(std::vector<ContentSourceFile>& files, std::string* error) override;
    /// Writes one listed file from the game folder to `request.destination`.
    [[nodiscard]] bool
    stream_file(const StreamRequest& request, StreamResult& result, std::string* error) override;
    /// Resolves a listed name to its file in the game folder.
    [[nodiscard]] bool
    locate(std::string_view name, std::filesystem::path& resolved, std::string* error) override;
    /// The game folder the files are read from.
    [[nodiscard]] std::filesystem::path disk_root() const override;

  private:

    std::filesystem::path folder_;
    std::filesystem::path root_;
    std::span<const oa::data::content_bundles::Bundle> bundles_{};
    bool opened_{};
};

/// A Windows installer. Names are the installed paths, and streaming one file
/// reads it out of the installer. The installer itself is only read.
class WindowsInstallerSource final : public ContentSource {
  public:

    /// Takes the installer the player named. Every installed file is listed;
    /// the copy matches that listing against `bundles`.
    ///
    /// @param file the installer
    /// @param bundles the bundles a file of the right name and size belongs to.
    ///        Not kept: matching reads the table again when the copy runs.
    WindowsInstallerSource(
        std::filesystem::path file, std::span<const oa::data::content_bundles::Bundle> bundles
    );

    /// Closes the installer.
    ~WindowsInstallerSource() override;

    WindowsInstallerSource(const WindowsInstallerSource&) = delete;
    WindowsInstallerSource& operator=(const WindowsInstallerSource&) = delete;
    WindowsInstallerSource(WindowsInstallerSource&&) = delete;
    WindowsInstallerSource& operator=(WindowsInstallerSource&&) = delete;

    /// The installer the player named, as the copy's record keeps it.
    [[nodiscard]] std::string origin() const override;
    /// Recognises the installer and reads its file list.
    ///
    /// A file whose size and SHA-256 are not the Windows installer this game
    /// knows fails with "This installer is not recognised." A file that is
    /// that installer but cannot be read fails with "This installer could
    /// not be read."
    [[nodiscard]] bool open(std::string* error) override;
    /// Lists the installed files.
    [[nodiscard]] bool list(std::vector<ContentSourceFile>& files, std::string* error) override;
    /// Writes one listed file from the installer to `request.destination`.
    [[nodiscard]] bool
    stream_file(const StreamRequest& request, StreamResult& result, std::string* error) override;

    /// The SHA-256 of one listed file. The installer is only read, and nothing
    /// is written.
    ///
    /// @param name the listed name
    /// @param[out] digest the digest, when the file was read
    /// @param[out] error why it could not be read; may be null
    /// @return true when `digest` is the file's
    [[nodiscard]] bool
    hash_file(std::string_view name, oa::base::sha256::Digest& digest, std::string* error);

  private:

    struct State;
    std::unique_ptr<State> state_;
};

/// One file recorded in a bundle folder. The format lives in
/// `oa/data/content_bundles/record.hpp`.
using ContentRecordFile = oa::data::content_bundles::ContentRecordFile;

/// The .oa-content record of one bundle folder.
using ContentRecord = oa::data::content_bundles::ContentRecord;

/// Reads a bundle folder's record from the record file's path.
///
/// A missing file is an empty record with present false. A file that is not
/// a record is present and not valid, which a copy treats as empty. A file
/// that cannot be read sets io_error. The bytes are read by
/// `read_bundle_folder_record`.
///
/// @param file the record's path, `<bundle folder>/.oa-content`
/// @param[out] error why it could not be read; may be null; unset otherwise
/// @return the record
[[nodiscard]] ContentRecord
read_content_record(const std::filesystem::path& file, std::string* error);

/// Writes a bundle folder's record, replacing any record already there.
///
/// An empty file list writes nothing: a bundle with no file yet has no record.
///
/// @param file the record's path
/// @param record the record to write; its files are written in order
/// @param[out] error why it was not written; may be null
/// @return true when the record is stored, or when there was nothing to store
[[nodiscard]] bool write_content_record(
    const std::filesystem::path& file, const ContentRecord& record, std::string* error
);

/// A file of the source that belongs to a bundle.
struct MatchedContentFile {
    /// The bundle id.
    std::string bundle_id;
    /// The name it is stored under.
    std::string copy_name;
    /// The name the source listed.
    std::string source_name;
    /// Size in bytes.
    uint64_t size{};
    /// The source file's modification time, seconds since 1970.
    int64_t modified{};
};

/// A bundle file the source does not have.
struct MissingContentFile {
    /// The bundle id.
    std::string bundle_id;
    /// The name the bundle stores it under.
    std::string copy_name;
    /// The source has this name at a different size. It is not filled from
    /// another file, and it is not recorded as absent.
    bool different_size{};
    /// The size the source holds the name at, in bytes, when `different_size`.
    uint64_t source_size{};
};

/// What a source holds, before anything is copied.
struct SourceDescription {
    /// Bundles with at least one matching file, in the table's order.
    std::vector<std::string> bundles;
    /// Matching files, in the table's order.
    std::vector<MatchedContentFile> matched;
    /// Table files that did not match, in the table's order.
    std::vector<MissingContentFile> missing;
    /// The size of the matching files, in bytes.
    uint64_t bytes_to_copy{};
};

/// Describes a source against a bundle table: which bundles it holds, which
/// files match, and how many bytes those files are. The source is only read.
///
/// @param source the source, opened if it is not open yet
/// @param bundles the table to match against
/// @param[out] described the description, replaced
/// @param[out] error why the source could not be read; may be null
/// @return true when `described` is the source's contents
[[nodiscard]] bool describe_source(
    ContentSource& source,
    std::span<const oa::data::content_bundles::Bundle> bundles,
    SourceDescription& described,
    std::string* error
);

/// Tells whether a file's name is the installer's published name. ASCII
/// letters are compared regardless of case.
///
/// @param file the file
/// @param installer a known installer
/// @return true when the file's name is the published name
[[nodiscard]] bool is_published_installer_name(
    const std::filesystem::path& file, const oa::data::content_bundles::Installer& installer
);

/// Copy names a published installer does not hold. Empty when this installer
/// has no listing kept for the setup's scan.
///
/// @param installer a known installer
/// @return the names, in the order they are stored
[[nodiscard]] std::span<const std::string_view>
installer_not_held(const oa::data::content_bundles::Installer& installer) noexcept;

/// Describes what a published installer holds, from the bundle table and the
/// names that installer does not hold. No file is read.
///
/// @param installer a known installer
/// @param[out] described the description, replaced; empty when this installer
///        has no listing
/// @return true when `described` is that installer's listing
[[nodiscard]] bool describe_published_holdings(
    const oa::data::content_bundles::Installer& installer, SourceDescription& described
);

/// Describes a known installer from its size, its published name and a hash
/// of its first and last megabyte. The archive is not listed and the rest
/// of the file is not read. The description is what that published installer
/// holds. Copying and the installer check still read every byte.
///
/// @param file the installer
/// @param[out] described the description, replaced
/// @param[out] error why the file is not that installer; may be null
/// @return true when the file is a published installer
[[nodiscard]] bool describe_published_installer(
    const std::filesystem::path& file, SourceDescription& described, std::string* error
);

/// Free space reported for a folder.
struct FreeSpace {
    /// Free bytes. Ignored unless `known`.
    uint64_t bytes{};
    /// True when `bytes` was read.
    bool known{};
};

/// How far a copy has got. The views are valid only for the callback.
struct CopyProgress {
    /// Bytes finished, counting a file already copied as finished.
    uint64_t bytes_done{};
    /// Bytes of every matched file.
    uint64_t bytes_total{};
    /// Files finished, counting a file already copied.
    uint32_t files_done{};
    /// Files this copy takes from this source. A file the source does not hold
    /// is not counted. A finished copy has `files_done` equal to this.
    uint32_t files_total{};
    /// The bundle being copied.
    std::string_view bundle_id;
    /// The file being copied, under its stored name.
    std::string_view copy_name;
    /// True when this call is the file's last.
    bool file_finished{};
};

/// Files the copying step's overall count uses.
///
/// `total` is the files this copy takes from this source. A file the source
/// does not hold is not one of them. `done` is how many of those are finished.
/// A finished copy of every file it takes has `done == total` and reads 100%.
struct CopyFileCount {
    uint32_t done{};  ///< files finished
    uint32_t total{}; ///< files this copy takes
};

/// Counts the files a copy shows.
///
/// `finished` is how many taken files are done. `table_files` is every file
/// the table names. `not_taken` is files this copy does not take, because the
/// source does not hold them.
///
/// @param finished files already finished
/// @param table_files files the table names
/// @param not_taken files the source does not hold
/// @return the count the overall line uses
[[nodiscard]] constexpr CopyFileCount
copy_file_count(uint32_t finished, uint32_t table_files, uint32_t not_taken) noexcept {
    const uint32_t absent = not_taken > table_files ? table_files : not_taken;
    const uint32_t total = table_files - absent;
    const uint32_t done = finished > total ? total : finished;
    return {done, total};
}

/// The percent a copy's overall line shows, from 0 to 100.
///
/// A count with nothing to take reads 0. A finished copy of every file it
/// takes reads 100.
///
/// @param count the files this copy takes, and how many are finished
/// @return the percent
[[nodiscard]] constexpr uint32_t copy_file_percent(CopyFileCount count) noexcept {
    if (count.total == 0)
        return 0;
    return static_cast<uint32_t>((static_cast<uint64_t>(count.done) * 100) / count.total);
}

/// Called as a copy moves on. May set the request's stop flag.
using CopyProgressFn = void (*)(void* context, const CopyProgress& progress);

/// Reads the free space of a directory. Null uses the volume's own report.
using FreeSpaceFn = FreeSpace (*)(void* context, const std::filesystem::path& directory);

/// How a copy ended.
enum class CopyStatus : uint8_t {
    finished, ///< every matched file is in the player's folder
    stopped,  ///< the stop flag was honoured; a partial file is left to resume
    no_space, ///< the disk does not have room, or it filled
    failed,   ///< the copy could not be finished
    /// The source holds an archive of a base game or of an expansion at
    /// another size than the table's, as a mod or another program leaves
    /// it. Nothing is copied.
    changed,
};

/// An archive of a base game or of an expansion that the source holds at
/// another size than the table's.
struct ChangedContentFile {
    /// The id of the base game or the expansion it belongs to.
    std::string bundle_id;
    /// The name the table stores it under.
    std::string copy_name;
    /// The size the source holds it at, in bytes.
    uint64_t source_size{};
    /// The size the table gives it, in bytes.
    uint64_t size{};
};

/// Tells whether a stored name is an archive the game mounts, by its suffix:
/// .hpi, .ufo, .ccx or .gp3, in any case.
///
/// @param copy_name the name a file is stored under
/// @return true for an archive; false for a movie, a track or anything else
[[nodiscard]] bool archive_copy_name(std::string_view copy_name) noexcept;

/// The outcome of one copy.
struct CopyResult {
    /// How it ended.
    CopyStatus status{CopyStatus::finished};
    /// Why it did not finish, for the player. Empty when it finished.
    std::string error;
    /// When `changed`, the archives that stopped it, in the table's order.
    std::vector<ChangedContentFile> changed;
    /// Bytes written this time. A file already present counts as none.
    uint64_t bytes_copied{};
    /// Files finished this time.
    uint32_t files_copied{};
    /// Files already present with the recorded size and time.
    uint32_t files_skipped{};
};

/// What a copy is asked to do. The source is separate, so a request can be
/// filled before the source exists.
struct CopyRequest {
    /// The player's folder, which holds games and expansions.
    std::filesystem::path user_folder;
    /// The table to copy. Empty copies nothing.
    std::span<const oa::data::content_bundles::Bundle> bundles{};
    /// Bytes read at a time. 0 uses content_copy_chunk_bytes.
    uint64_t chunk_bytes{};
    /// True when a volume that can share blocks may be asked to.
    bool share_blocks{true};
    /// Free space, or null for the volume's own report.
    FreeSpaceFn free_space{};
    /// Passed to `free_space`.
    void* free_space_context{};
    /// Progress, or null for none.
    CopyProgressFn on_progress{};
    /// Passed to `on_progress`.
    void* progress_context{};
    /// Set to stop between files and between chunks. Null never stops.
    std::atomic<bool>* stop{};
    /// Receives one line when a copy starts and one when it finishes, and
    /// one for each file it leaves out because the source holds that name at
    /// another size, or that stops it. Null stays quiet, as a copy that
    /// finds the files already there does.
    std::ostream* news{};
};

/// Copies the source's matched files into the player's folder.
///
/// Bundle by bundle, in the table's order. A volume that can share a file's
/// blocks does; otherwise the bytes are copied. A file already recorded, with
/// the same size and modification time, is skipped. A copy that stopped is
/// continued from the bytes already written. Free space is checked first.
/// Nothing in the source is written. A source that is already inside the
/// managed folders, or a destination that would be written inside the source,
/// copies nothing.
///
/// A source that holds an archive of a base game or of an expansion under
/// its name at another size has been changed, by a mod or another program:
/// nothing is copied or recorded, and the copy ends `changed`, naming every
/// such archive. A bundle the source holds none of is not there and stops
/// nothing. A movie or a track the source holds at another size is recorded
/// as not in the source, with that size, and the copy goes on.
///
/// @param source the files to read
/// @param request where they go, and when to stop
/// @return how the copy ended
[[nodiscard]] CopyResult copy_content(ContentSource& source, const CopyRequest& request);

/// The folder one bundle is stored in.
///
/// A base game is Games/<id>. An expansion is Expansions/<base>/<id>, using
/// the first base game it extends.
///
/// @param user_folder the player's folder
/// @param bundle the bundle
/// @return the folder, which may not exist yet
[[nodiscard]] std::filesystem::path bundle_folder(
    const std::filesystem::path& user_folder, const oa::data::content_bundles::Bundle& bundle
);

/// Reports whether the player's folder already holds a managed base game.
///
/// An expansion on its own does not count. A record that names the base game
/// and every one of its files, copied or not in the source, does: that base
/// game can be played. A copy that stopped part way does not, so the setup
/// offers the copy again and the copy continues where it stopped.
///
/// @param user_folder the player's folder
/// @param bundles the table whose base games are looked for
/// @return true when one of those base games has a complete record
[[nodiscard]] bool managed_base_present(
    const std::filesystem::path& user_folder,
    std::span<const oa::data::content_bundles::Bundle> bundles
);

/// What a start knows when it decides whether to copy.
struct ManagedCopyRequest {
    /// The installation to read, when this start has one.
    std::filesystem::path installation;
    /// True when the command line named --game-dir.
    bool game_dir_named{};
    /// True when nobody is watching this run.
    bool unattended{};
    /// The player's folder.
    std::filesystem::path user_folder;
    /// True when `user_folder` was resolved.
    bool user_folder_known{};
    /// The table to copy. Empty uses the built-in table.
    std::span<const oa::data::content_bundles::Bundle> bundles{};
    /// Where a copy tells the player it has started. Null stays quiet.
    std::ostream* news{};
};

/// Copies when this start should, or refuses an unattended start that has
/// no managed base game.
///
/// --game-dir leaves the installation where it is. An unattended start never
/// copies. An attended start copies from `installation` when that is a
/// directory of game files, or a Windows installer this game recognises.
/// The installation is only read. A start whose player folder could not be
/// resolved skips the copy when somebody is watching, and refuses when nobody
/// is.
///
/// @param request what this start was given
/// @throws std::runtime_error when an unattended start has no managed base
///     game, or when the copy cannot be finished
void ensure_managed_copy(const ManagedCopyRequest& request);

/// One small file the content-copy check writes into its fixture install.
struct CopyCheckFile {
    /// The bundle the file belongs to.
    std::string_view bundle_id;
    /// The name it is stored under.
    std::string_view copy_name;
    /// The bytes.
    std::string_view bytes;
};

/// Returns the small files the content-copy check copies.
///
/// @return the files, in the order they are copied
[[nodiscard]] std::span<const CopyCheckFile> copy_check_files() noexcept;

/// Returns the bundle table the content-copy check matches against.
///
/// The sizes and SHA-256 values are the bytes copy_check_files() names.
///
/// @return the table
[[nodiscard]] std::span<const oa::data::content_bundles::Bundle> copy_check_bundles() noexcept;

/// Copies a fixture install into `documents` and checks the copy.
///
/// `documents` stands in for the Documents folder. The fixture is written
/// under it, copied once and stopped, then copied again. The check passes
/// when the second copy finishes identical to the fixture, the fixture's
/// bytes and modification times are unchanged, and each bundle's record
/// holds the file's SHA-256. Open Annihilation and G02-fixture-install under
/// `documents` are cleared first.
///
/// @param documents the scratch Documents folder
/// @return 0 when the check passed, 1 when it did not
[[nodiscard]] int run_content_copy_check(const std::filesystem::path& documents);

/// The macOS installer package. The game files are read from its payload in
/// one pass and matched against `bundles`. When `require_known` is set, the
/// file has to be the recognised installer before anything is copied.
class MacosInstallerSource final : public ContentSource {
  public:

    /// Takes the installer the player named and the table its files are
    /// matched against. The table has to outlive the source.
    ///
    /// @param file the installer file
    /// @param bundles the bundles a file of the right name and size belongs to
    /// @param require_known true when the file has to be the recognised installer
    MacosInstallerSource(
        std::filesystem::path file,
        std::span<const oa::data::content_bundles::Bundle> bundles,
        bool require_known = true
    );

    /// The installer file, as the copy's record keeps it.
    [[nodiscard]] std::string origin() const override;
    /// Checks the file, and reads the installer's table of contents.
    [[nodiscard]] bool open(std::string* error) override;
    /// Lists the game files the installer holds.
    [[nodiscard]] bool list(std::vector<ContentSourceFile>& files, std::string* error) override;
    /// Writes one listed game file. The payload is read from the start.
    [[nodiscard]] bool
    stream_file(const StreamRequest& request, StreamResult& result, std::string* error) override;
    /// Copies the matched game files in one pass over the payload.
    [[nodiscard]] bool copy_together(const CopyRequest& request, CopyResult& result) override;

  private:

    friend struct InstallerWalk;

    /// Writes the installer's check. See report_macos_installer.
    friend int report_macos_installer(
        const std::filesystem::path& file,
        std::span<const oa::data::content_bundles::Bundle> bundles,
        bool require_known,
        std::ostream& out
    );

    /// Checks the installer and writes one line for each table file.
    ///
    /// @param out where the lines are written
    /// @return 0 when every table file is present and known, 1 otherwise
    [[nodiscard]] int write_check(std::ostream& out);

    /// Opens the installer once.
    ///
    /// @param[out] error why it could not be opened; may be null
    /// @param news where a check of the file is announced; may be null
    /// @return true when the payload can be read
    [[nodiscard]] bool ensure(std::string* error, std::ostream* news);

    std::filesystem::path file_{};
    std::span<const oa::data::content_bundles::Bundle> bundles_{};
    bool require_known_{};
    bool opened_{};
    std::string open_error_{};
    uint64_t scripts_at_{};
    uint64_t scripts_bytes_{};
    bool outer_gzip_{};
    bool ready_{};
};

/// Reads the macOS installer and checks the game files it holds.
///
/// Nothing is written. A known installer is recognised by its size and
/// SHA-256 when `require_known` is set. Each file the table names is
/// reported, and a missing file or a file that is not a known hash fails
/// the check.
///
/// @param file the installer
/// @param bundles the table to match against
/// @param require_known true when the file has to be the recognised installer
/// @param out where the report is written
/// @return 0 when the check passed, 1 when it did not
[[nodiscard]] int report_macos_installer(
    const std::filesystem::path& file,
    std::span<const oa::data::content_bundles::Bundle> bundles,
    bool require_known,
    std::ostream& out
);

/// Checks the recognised macOS installer and writes the report to standard output.
///
/// @param file the installer
/// @return 0 when the check passed, 1 when it did not
[[nodiscard]] int run_macos_installer_check(const std::filesystem::path& file);

/// One file of a copied bundle whose bytes are not a hash the table knows.
struct ContentFileDifference {
    /// The name it is stored under, relative to the bundle folder.
    std::string copy_name;
};

/// One file of a copied bundle the copy left out because the source held it
/// at another size.
struct ContentFileLeftOut {
    /// The name the table stores it under.
    std::string copy_name;
    /// The size the source held it at, in bytes.
    uint64_t source_size{};
    /// The size the table gives it, in bytes.
    uint64_t size{};
};

/// An archive at the top of a copied bundle's folder that the bundle does
/// not list: a .hpi, .ufo, .ccx or .gp3 file under any other name. The game
/// does not mount it.
struct ContentStrayArchive {
    /// Its file name, as the folder spells it.
    std::string name;
    /// The folder it is in.
    std::filesystem::path folder;
    /// Its size in bytes.
    uint64_t size{};
};

/// One copied bundle, after the start's check.
struct ContentBundleCheck {
    /// The bundle id.
    std::string id;
    /// The name shown to the player, in English, as the table writes it.
    std::string english_name;
    /// Base game or expansion.
    oa::data::content_bundles::Kind kind{};
    /// How many files the table gives the bundle.
    uint32_t file_count{};
    /// How many of those files differ: missing, or not a hash the table knows.
    /// A music file the source never held is not one of these, nor a file
    /// left out for its size while it is still absent. Each stray archive in
    /// the bundle's folder counts here too.
    uint32_t files_differ{};
    /// Music files the source did not hold, and files left out for their
    /// size, while they are absent. Older records name none.
    uint32_t files_not_in_source{};
    /// The files that differ, in the table's order.
    std::vector<ContentFileDifference> differing;
    /// The files left out for their size and still absent, in the table's
    /// order. The source held each at a size the table does not give it.
    std::vector<ContentFileLeftOut> left_out;
    /// The stray archives in the bundle's folder, in name order.
    std::vector<ContentStrayArchive> strays;
};

/// Reports whether a file name is an archive's: .hpi, .ufo, .ccx or .gp3, in any case.
///
/// @param name the file name
/// @return true for those four
[[nodiscard]] bool archive_file_name(std::string_view name) noexcept;

/// Lists the stray archives at the top of a bundle's folder.
///
/// Every regular file there whose name is an archive's (archive_file_name)
/// and that the bundle does not list, compared without case, is one.
///
/// @param folder the bundle's folder
/// @param bundle the bundle
/// @return the strays, in name order; empty when the folder cannot be read
[[nodiscard]] std::vector<ContentStrayArchive> stray_archives(
    const std::filesystem::path& folder, const oa::data::content_bundles::Bundle& bundle
);

/// What a start knows about the managed copy.
struct ContentCheck {
    /// Every copied bundle, in the table's order, matching and differing.
    std::vector<ContentBundleCheck> bundles;
    /// Files read and hashed because their size or time was not the record's.
    uint32_t files_hashed{};
};

/// Reports whether a played base game or rule-changing expansion differs.
///
/// A bundle differs when a file of it is not a known hash or its folder
/// holds a stray archive (ContentBundleCheck::files_differ). Music changes
/// no rules and is not counted. A bundle the check did not see is not
/// counted either.
///
/// @param check the start's check
/// @param base_game the base game played
/// @param expansions the expansions played
/// @return true when one of them differs
[[nodiscard]] bool played_content_differs(
    const ContentCheck& check, std::string_view base_game, std::span<const std::string> expansions
) noexcept;

/// Reports whether any copied bundle differs.
///
/// @param check the start's check
/// @return true when a bundle has a file that differs
[[nodiscard]] bool content_notice_due(const ContentCheck& check) noexcept;

/// Reports whether this start shows the content notice.
///
/// A named --game-dir, a run nobody watches, and a notice already closed
/// this start stay quiet. Nothing is stored for a later start: while the
/// files differ, the next start shows it again.
///
/// @param game_dir_named the command line named --game-dir
/// @param unwatched nobody is watching this run
/// @param closed_this_run OK or Escape already closed it this start
/// @param check the start's check
/// @return true when the notice is shown
[[nodiscard]] bool show_content_notice(
    bool game_dir_named, bool unwatched, bool closed_this_run, const ContentCheck& check
) noexcept;

/// The words of one notice, in the language the interface shows.
struct ContentNoticeCopy {
    /// The header's title.
    std::string title;
    /// The text, top to bottom.
    std::vector<std::string> lines;
    /// The caption of the button that opens the list of files.
    std::string details;
    /// OK's caption.
    std::string ok;
};

/// The main menu's notice of the bundles that differ.
///
/// The first line says the sizes do not match, then one line for each
/// differing base game or expansion and how many of its files differ, then
/// one line on what that means. A bundle that matches is left out.
///
/// @param check the start's check
/// @return the notice
[[nodiscard]] ContentNoticeCopy content_difference_notice(const ContentCheck& check);

/// The list of files that differ, one line for each, under its bundle's name.
///
/// Game Content lists each bundle. DETAILS on the content notice still
/// shows the files that differ.
///
/// @param check the start's check
/// @return the list
[[nodiscard]] ContentNoticeCopy content_file_notice(const ContentCheck& check);

/// What the Library says of one bundle's check.
///
/// None differing is "All files match", one is "1 file differs", and more
/// is "{count} files differ" with the count filled in.
///
/// @param files_differ how many of the bundle's files differ
/// @return the line, in the language the interface shows
[[nodiscard]] std::string content_match_line(uint32_t files_differ);

/// "{held} of {total} tracks", for a music bundle the source did not finish.
///
/// @param held tracks the source held
/// @param total tracks the table names
/// @return the line, in the language the interface shows
[[nodiscard]] std::string content_tracks_line(uint32_t held, uint32_t total);

/// One stray archive, named with its folder and its size.
///
/// @param bundle the bundle whose folder holds it
/// @param stray the archive
/// @return "{file} in {folder}, {size} bytes, is not a file of {bundle}.
///         The game does not read it." in the language shown
[[nodiscard]] std::string
content_stray_line(const ContentBundleCheck& bundle, const ContentStrayArchive& stray);

/// The line under a bundle's name in the content check.
///
/// A bundle whose only differences are stray archives names the one, or
/// counts them. A bundle with differing files uses the file counts. Music the source
/// left short, with nothing damaged, uses content_tracks_line. A bundle
/// with a file left out for its size names it, with the size the source
/// held it at and the size it should be, or names the files when several
/// were. Anything else that matches is "All files match".
///
/// @param bundle one bundle from the check
/// @return the line, in the language the interface shows
[[nodiscard]] std::string content_bundle_line(const ContentBundleCheck& bundle);

/// Checks every copied bundle in the player's folder.
///
/// A bundle with no readable record, or a record of another id, or a record
/// with no file, is not copied and is left out. Each file in the table is
/// compared with the record: the same size and the same modification time
/// keep the recorded SHA-256 and are not read. Any other file is hashed
/// whole, and the record then stores that size, time and SHA-256, so the
/// next start does not read it again while they still match. A missing file
/// is named and not hashed. A SHA-256 the table does not know marks the
/// bundle as differing and names the file. A music file the record says the
/// source never held is left out of that count while it is still absent; a
/// music file that is present and not a known hash is still named. A file
/// the copy left out because the source held it at another size is listed
/// as left out, with both sizes, and is not a difference while it is still
/// absent. An empty `table` uses the built-in table.
///
/// A bundle remembered by keep_this_run_copy_check is answered from that
/// result and its files are not read. That check forgets those bundles, so
/// a later start reads them as usual. A bundle this run did not remember is
/// checked as usual.
///
/// Called as each file of a copied bundle is considered. `files_done` counts
/// files already considered and `files_total` the files of bundles that have
/// a record. Return false to stop. The callback must not call interface_text.
using ContentCheckProgress =
    bool (*)(uint32_t files_done, uint32_t files_total, void* progress_context);

/// @param user_folder the player's folder
/// @param table the bundles to check; empty uses the built-in table
/// @param on_progress optional; told after each file, and once before the first
/// @param progress_context passed to on_progress
/// @param stop when not null and set, the check returns what it has so far
/// @return the check; a stopped check is not a finished answer
[[nodiscard]] ContentCheck check_managed_content(
    const std::filesystem::path& user_folder,
    std::span<const oa::data::content_bundles::Bundle> table,
    ContentCheckProgress on_progress = nullptr,
    void* progress_context = nullptr,
    const std::atomic<bool>* stop = nullptr
);

/// Remembers bundles this run's copy already checked.
///
/// The next check_managed_content answers those bundle ids from `check` and
/// does not read their files. A bundle that is not listed is checked as
/// usual. That next check forgets the list, so a later start checks every
/// copied bundle as usual. An empty `check` forgets a list already kept.
///
/// @param check the bundles this run's copy checked, and nothing else
void keep_this_run_copy_check(ContentCheck check);

/// Reads a Windows installer and prints the bundles it holds.
///
/// Each bundle with at least one file is printed, then every file of that
/// bundle with its SHA-256, or that it is missing. The installer is only
/// read. An installer this game does not recognise is refused.
///
/// @param file the installer
/// @return 0 when every file of every found bundle matches, 1 otherwise
[[nodiscard]] int run_windows_installer_check(const std::filesystem::path& file);

/// --check-installer: checks a recognised installer with the reader of its
/// kind, known by its size. A file of no known installer's size goes to the
/// Windows check, which refuses it.
///
/// @param file the installer
/// @return 0 when the check passed, 1 when it did not
[[nodiscard]] int run_installer_check(const std::filesystem::path& file);

} // namespace oa::app
