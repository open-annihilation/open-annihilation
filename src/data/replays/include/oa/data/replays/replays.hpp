// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The player's replays, as the Library lists them: every `.oademo` and `.tad`
// below the Recordings folder, each read for what its description or its
// header says, and an index under `Recordings/.replays` that keeps what was
// read so that a file which has not changed is not opened again. The index is
// a cache: deleting it, or damaging it, loses nothing. The replays the player
// keeps are marked in a file of their own beside it (`kept.json`), by each
// recording's identity, so that no change to the index forgets one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oademo {
struct Manifest;
struct Meta;
} // namespace oa::formats::oademo

namespace oa::data::replays {

/// The folder of Recordings that holds the index, and the games being recorded.
inline constexpr std::string_view replays_folder_name = ".replays";

/// The index's file name, in replays_folder_name.
inline constexpr std::string_view index_name = "index.json";

/// The folder of Recordings that holds the replays opened from outside it.
inline constexpr std::string_view imported_folder_name = "Imported";

/// The folder of Recordings that holds the replays downloaded from Core Prime.
inline constexpr std::string_view core_prime_folder_name = "Core Prime";

/// A recording's title as a file name, without an extension.
///
/// Control characters and `/ \ : * ? " < > |` become `_`. A leading or trailing
/// space or dot is dropped, and a `.oademo` or `.tad` ending is dropped. The
/// name is kept to 120 bytes on a UTF-8 boundary, so a cut never ends inside
/// a character. An empty name is `replay`. A reserved Windows name gains `_`.
///
/// @param title the title, UTF-8
/// @return the stem
[[nodiscard]] std::string recording_stem(std::string_view title);

/// recorded.how of a replay opened from outside Recordings.
inline constexpr std::string_view how_imported = "imported";

/// recorded.how of a replay downloaded from Core Prime.
inline constexpr std::string_view how_core_prime = "core-prime";

/// The index version this module writes and reads.
inline constexpr int index_version = 1;

/// The file name, in replays_folder_name, of the replays the player keeps.
inline constexpr std::string_view kept_name = "kept.json";

/// The version of the kept marks this module writes and reads.
inline constexpr int kept_version = 1;

/// The file name, in replays_folder_name, of the replays the player exported.
inline constexpr std::string_view exported_name = "exported.json";

/// The version of the exported marks this module writes and reads.
inline constexpr int exported_version = 1;

/// The ending a stored file that does not read takes when it is set aside
/// (set_aside): `index.json` becomes `index.json.damaged`, and a later one
/// `index.json.damaged-2`, so an earlier copy is never replaced.
inline constexpr std::string_view damaged_ending = ".damaged";

/// The largest file read, in bytes. A larger file is listed damaged and never read.
inline constexpr uint64_t most_file_bytes = 512ULL * 1024ULL * 1024ULL;

/// The largest index read, in bytes. A larger one is not read, and is an empty index.
inline constexpr uint64_t most_index_bytes = 64ULL * 1024ULL * 1024ULL;

/// How many folders below Recordings are walked.
inline constexpr std::size_t most_folder_depth = 3;

/// game.kind of a network game, and the kind of every `.tad`.
inline constexpr std::string_view kind_multiplayer = "multiplayer";

/// game.kind of a skirmish.
inline constexpr std::string_view kind_skirmish = "skirmish";

/// game.kind of a campaign mission.
inline constexpr std::string_view kind_campaign = "campaign";

/// Which reader reads a file.
enum class FileKind : uint8_t {
    oademo, ///< a `.oademo` replay
    tad,    ///< a TA Demo recording
};

/// The most packages one replay's needs list keeps.
inline constexpr std::size_t most_content_needs = 64;

/// One package a replay needs before it can be watched.
///
/// A `.oademo` fills this from `manifest.json`. A `.tad` fills it from the
/// header, Open Annihilation's sectors and the `.oa-content` note beside the
/// file. `sha256` is the package file the manifest names, when it names one.
struct ContentNeed {
    /// `base`, `expansion`, `oamod`, `oamap`, `oamut`, or a kind an extension names.
    std::string kind{};
    /// The package id. Empty when the recording names only a display name.
    std::string id{};
    /// The name a player sees. Empty when the recording names none.
    std::string name{};
    /// The version, when the recording names one.
    std::string version{};
    /// The revision, when the recording names one.
    std::optional<int64_t> revision{};
    /// SHA-256 of the package file, 64 lower-case hex digits. Empty when unnamed.
    std::string sha256{};
    /// A map's name, for a map inside a pack. Empty for every other kind.
    std::string map_name{};
    /// A map pack's name, when the recording names it. The catalogue's name is
    /// used when this is empty and the pack is listed.
    std::string pack_name{};
};

/// One player of a replay.
struct Player {
    /// The player's name, UTF-8.
    std::string name{};
    /// The game's player colour, 0 to 9.
    int32_t colour{};
    /// arm, core, watcher, or the side as recorded.
    std::string side{};
    /// The player's team, when the replay names one.
    std::optional<int32_t> team{};
    /// human, computer or watcher.
    std::string kind{};
    /// True for the player on the machine that recorded.
    bool recorded_here{};
    /// The player's number in the recording. A file that names none is given
    /// 1 for its first player, then 2, and so on.
    int32_t number{};
};

/// One replay file, and what its content says.
struct Replay {
    /// The file, relative to Recordings, '/' between folders, UTF-8:
    /// `default/2026-11-04 1432 Show Down.oademo`.
    std::string path{};
    /// The file's size, in bytes.
    uint64_t size{};
    /// The file's modification time, in seconds since 1970 (UTC).
    int64_t modified{};
    /// Which reader read it.
    FileKind file{FileKind::oademo};
    /// Empty when the file was read; otherwise the sentence that says why not,
    /// from interface_words.hpp or the container's interface words.
    std::string damage{};
    /// The recording's identity, lower-case hexadecimal: a `.oademo`'s
    /// `recording.sha256`, or the SHA-256 of a `.tad`'s whole file.
    std::string sha256{};
    /// The map's name.
    std::string map{};
    /// game.kind; multiplayer for a `.tad`.
    std::string kind{};
    /// recorded.how; empty for a `.tad`.
    std::string how{};
    /// When the game was recorded, in seconds since 1970 (UTC): `recorded.at`
    /// when it is set and reads as `YYYY-MM-DDTHH:MM:SSZ`, else `modified`.
    int64_t recorded{};
    /// recorded.by; a `.tad`'s recorder-version sector.
    std::string recorded_by{};
    /// The game's length in milliseconds: game.length_ms, or a `.tad`'s last
    /// packet's time; 0 when not known.
    uint64_t length_ms{};
    /// game.ended, or empty.
    std::string ended{};
    /// recorded.after_crash.
    bool after_crash{};
    /// content.mod's id, or empty.
    std::string mod_id{};
    /// content.mod's name, or empty.
    std::string mod_name{};
    /// content.mod's version, or empty.
    std::string mod_version{};
    /// content.mod's revision, when named.
    std::optional<int64_t> mod_revision{};
    /// content.expansions.
    std::vector<std::string> expansions{};
    /// content.mutators.
    std::vector<std::string> mutators{};
    /// The string member `planet` of the object under `game` that the kind
    /// names, or empty.
    std::string planet{};
    /// The string member `sector` of that object, or empty.
    std::string sector{};
    /// The string member `game_id` of that object, when the recording names a battle.
    std::string game_id{};
    /// The players, in the recording's order.
    std::vector<Player> players{};
    /// `game.campaign.mission_name`, when the kind is campaign, as the file
    /// writes it. The Library trims trailing spaces when it shows the title.
    /// The map's file name stays in `map`, so a search can still name the file.
    std::string mission_name{};
    /// The timeline the index pass wrote, as `timeline.json` text. Nothing
    /// means the file has not been read yet. An empty string means a pass
    /// finished and left nothing the Library can show. A file that has not
    /// changed keeps this text on the next scan.
    std::optional<std::string> timeline{};
    /// The tick a Watch was left at, when the player has watched this replay
    /// and left it before the end. Nothing once the replay had finished.
    std::optional<uint32_t> watched_tick{};
    /// The installed-content stamp when a pass could not read this file.
    /// Nothing when it has not been refused, or a later pass stored a
    /// timeline. The file is read again when that stamp changes. A refusal
    /// does not store an empty timeline.
    std::optional<std::string> read_stamp{};
    /// The engine that recorded a `.oademo`, as `engine.recorded_with`. Empty
    /// for a `.tad` and when the manifest names none.
    std::string engine_recorded{};
    /// The oldest engine that plays a `.oademo`, as `engine.needs` (`>=0.9.2`).
    /// Empty when the manifest names none.
    std::string engine_needs{};
    /// The packages the replay needs, at most most_content_needs.
    std::vector<ContentNeed> needs{};
    /// True once this build has stored `needs` for the file. An index written
    /// before that is read again, so the needs are filled once.
    bool needs_read{};
    /// True when the player keeps the replay. Neither a file nor the index
    /// says so: apply_kept sets it from the kept marks, and index_text does
    /// not write it.
    bool kept{};
    /// True when the player exported the replay. Neither a file nor the index
    /// says so: apply_exported sets it from the exported marks, and index_text
    /// does not write it. Export (R09) calls set_exported after it writes the file.
    bool exported{};
};

/// Copies a `.oademo` manifest onto a replay.
///
/// Each package becomes one need. A map pack becomes one need for each map
/// it names. At most most_content_needs needs are kept, and a digest is stored
/// in lower case. A mod's name is taken from `meta` when the ids match. When
/// the manifest names no map and `replay.map` is set, one title-only map need
/// is added. The note beside a file is not read.
///
/// @param manifest the manifest
/// @param meta the description, for a mod's name
/// @param[in,out] replay the replay
void apply_manifest(
    const formats::oademo::Manifest& manifest, const formats::oademo::Meta& meta, Replay& replay
);

/// Tells whether a replay's file could not be read.
///
/// @param replay the replay
/// @return true when `damage` is set
[[nodiscard]] bool damaged(const Replay& replay) noexcept;

/// Tells whether a replay was opened from outside Recordings.
///
/// @param replay the replay
/// @return true when its path starts with `Imported/`
[[nodiscard]] bool imported(const Replay& replay) noexcept;

/// Tells which reader reads a file, by its name's ending.
///
/// @param name a file name or a path, '/' between folders
/// @return oademo for a name ending `.oademo`, tad for one ending `.tad`,
///         without ASCII case; nothing for any other name
[[nodiscard]] std::optional<FileKind> file_kind_of(std::string_view name) noexcept;

/// Reads what a replay's content says.
///
/// A `.oademo`'s description and manifest are read without its recording
/// (oa::formats::oademo::read_description). A `.tad` is parsed whole and its
/// header, players, sectors and packet times are read; a name or a sector
/// that is not UTF-8 is read as Windows-1252. `path`, `size`, `modified` and
/// `kept` are left as they are by default, and `recorded` is 0 unless a
/// `.oademo` names `recorded.at`.
///
/// @param bytes the whole file
/// @param kind which reader reads it
/// @return the replay; bytes that do not read give a replay with `damage` set,
///         never an exception
[[nodiscard]] Replay read_replay(std::span<const uint8_t> bytes, FileKind kind);

/// Opens one file below Recordings and reads it.
///
/// Fills `path`, `size` and `modified` from the file, then reads it as
/// read_replay does. The name's ending says which reader reads it; a name
/// with neither ending is read as a `.oademo` when its bytes open like one,
/// and as a `.tad` otherwise. A file that cannot be opened, or a path that
/// leaves Recordings (an empty name, `.` or `..`, a drive or a root), is
/// damaged with file_unreadable; a file larger than most_file_bytes is damaged
/// with file_too_large and is not read. A damaged file's `recorded` is its
/// modification time.
///
/// @param recordings the Recordings folder
/// @param path the file, relative to Recordings, '/' between folders, UTF-8
/// @return the replay; `kept` is false
[[nodiscard]] Replay read_file(const std::filesystem::path& recordings, std::string_view path);

/// One replay file a listing found.
struct Listed {
    /// The file, relative to Recordings, '/' between folders, UTF-8.
    std::string path{};
    /// The file's size, in bytes.
    uint64_t size{};
    /// The file's modification time, in seconds since 1970 (UTC).
    int64_t modified{};
    /// Which reader reads it, by its name's ending.
    FileKind file{FileKind::oademo};
};

/// Lists the replay files below Recordings, without opening them.
///
/// Every regular file whose name ends `.oademo` or `.tad` (file_kind_of), in
/// Recordings or at most most_folder_depth folders below it. A file or a
/// folder whose name starts with '.', such as `.replays`, is skipped, and so
/// is a symbolic link, which is never followed. Recordings itself may be a
/// link. A missing folder is an empty list. An entry the file system will not
/// describe is skipped, and a folder it will not list stops being listed;
/// neither throws.
///
/// @param recordings the Recordings folder
/// @return the files, sorted by path in byte order
[[nodiscard]] std::vector<Listed> list_folder(const std::filesystem::path& recordings);

/// The replays of a Recordings folder, as read or as the index keeps them.
struct Index {
    /// The replays, sorted by path in byte order.
    std::vector<Replay> replays{};
};

/// Writes an index as one compact JSON object.
///
/// `{"replays_index":1,"replays":[...]}` with every member of each replay
/// and of each player, in a fixed order, and the optional ones only when they
/// are set. The replays are written in the order given. The same index always
/// gives the same text.
///
/// @param index the index
/// @return the JSON text
[[nodiscard]] std::string index_text(const Index& index);

/// Reads an index's text.
///
/// Members this version does not know are ignored, and a known member that
/// is missing takes its default. An entry that is not an object, has no path,
/// or holds a known member of the wrong type is dropped, and so is a later
/// entry of a path already read. The replays are sorted by path.
///
/// @param text the JSON text
/// @return the index, or nothing for a text that is not JSON, names another
///         version, or is not an object holding the array `replays`
[[nodiscard]] std::optional<Index> parse_index(std::string_view text);

/// Returns where a Recordings folder's index is kept.
///
/// @param recordings the Recordings folder
/// @return `recordings/.replays/index.json`
[[nodiscard]] std::filesystem::path index_path(const std::filesystem::path& recordings);

/// An index as loaded, and whether the file that held it was damaged.
struct IndexRead {
    /// The index; empty when there was none or the file was damaged.
    Index index{};
    /// The file is there but cannot be read, is larger than most_index_bytes,
    /// or is not an index. A missing file is not damaged.
    bool damaged{};
};

/// Loads a Recordings folder's index.
///
/// @param recordings the Recordings folder
/// @return the index, and whether the file was damaged
[[nodiscard]] IndexRead read_index(const std::filesystem::path& recordings);

/// Loads a Recordings folder's index.
///
/// @param recordings the Recordings folder
/// @return the index; an empty one when the file is missing, cannot be read,
///         is larger than most_index_bytes, or is not an index
[[nodiscard]] Index load_index(const std::filesystem::path& recordings);

/// Moves a stored file that does not read out of the way, so that what the
/// next save writes does not replace it.
///
/// The file is renamed to its own name with damaged_ending added
/// (`kept.json.damaged`). A name already taken is kept: the next copy is
/// `kept.json.damaged-2`, then `kept.json.damaged-3`, and so on. An earlier
/// copy is never removed. The copy is dated now, so its modification time
/// says when it was set aside (kept_set_aside_at); a rename alone would keep
/// the time the file was last written.
///
/// @param file the stored file
/// @return where it went; nothing when it could not be moved
[[nodiscard]] std::optional<std::filesystem::path> set_aside(const std::filesystem::path& file);

/// Saves a Recordings folder's index, whole or not at all.
///
/// Makes `.replays` when it is missing, writes the text to a temporary file
/// beside `index.json` and renames it over `index.json`
/// (oa::platform::replace_file). A crash part-way leaves the old index whole.
///
/// @param recordings the Recordings folder
/// @param index the index
/// @return true when `index.json` holds the index; false when the disk refuses
bool save_index(const std::filesystem::path& recordings, const Index& index);

/// What a scan tells as it goes, and how it is stopped.
struct ScanHooks {
    /// Passed back to each hook.
    void* context{};
    /// Tells of one replay of the folder, read or taken from the index, as
    /// each is done; `done` counts from 1 to `total`, the files listed. Null
    /// tells nothing.
    void (*replay)(void* context, const Replay& replay, std::size_t done, std::size_t total){};
    /// Tells whether to stop now; asked before each file. Null never stops.
    bool (*stop)(void* context){};
};

/// Lists a Recordings folder and reads what has changed since an index.
///
/// A listed file whose path, size, modification time and reader match an
/// entry of `previous` takes that entry as it is, without being opened. Any
/// other file is read (read_file). A damaged file is in the result, damaged;
/// nothing throws for one. A scan that stops returns the files done. `kept`
/// is left false: apply_kept sets it.
///
/// @param recordings the Recordings folder
/// @param previous the index the folder was last scanned into, or an empty one
/// @param hooks what to tell as the scan goes, and when to stop
/// @return only the files listed, sorted by path
[[nodiscard]] Index
scan(const std::filesystem::path& recordings, const Index& previous, const ScanHooks& hooks = {});

/// One replay the player keeps.
struct KeptMark {
    /// The recording's identity (Replay::sha256), lower-case hexadecimal.
    /// Empty for a replay whose file did not read, which has none: it is kept
    /// by its path alone.
    std::string sha256{};
    /// Where the replay was last seen, relative to Recordings. A hint: a
    /// replay that moved or was renamed keeps its identity, so it stays kept.
    std::string path{};
};

/// The replays the player keeps, as `Recordings/.replays/kept.json` holds them.
struct KeptMarks {
    /// The marks, sorted by identity and then path. A mark with an identity
    /// is the only one of it; a mark without one is the only one of its path.
    std::vector<KeptMark> marks{};
};

/// Tells whether the player keeps a replay.
///
/// @param kept the marks
/// @param replay the replay
/// @return true when a mark has the replay's identity, or a mark names its path
[[nodiscard]] bool is_kept(const KeptMarks& kept, const Replay& replay);

/// Keeps a replay, or stops keeping it.
///
/// @param[in,out] kept the marks
/// @param replay the replay
/// @param keep true to keep it
/// @return true when the marks changed
bool set_kept(KeptMarks& kept, const Replay& replay, bool keep);

/// Sets `kept` of each replay from the marks.
///
/// A file a mark names by path stays kept, even when another listed file
/// still has that mark's identity. The mark's path is not moved off a file
/// that is still there. A mark is added for that file's identity now, or for
/// its path alone when it no longer reads. A path whose file is gone moves
/// to a listed file that still has the identity.
///
/// @param[in,out] kept the marks; a path may change and a mark may be added
/// @param[in,out] replays the replays
/// @return true when the marks changed, so they are worth saving
bool apply_kept(KeptMarks& kept, std::vector<Replay>& replays);

/// Drops a kept mark for a replay that was deleted, unless another listed
/// replay still has that identity.
///
/// Two copies of one recording share one mark. Deleting one leaves the mark,
/// and when the mark's path named the deleted copy it is moved to the copy
/// that stays. A replay no other file shares loses its mark.
///
/// @param[in,out] kept the marks
/// @param listed the replays listed, including the one deleted
/// @param deleted the replay that was deleted
/// @return true when a mark was removed or its path changed
bool release_kept(KeptMarks& kept, const std::vector<Replay>& listed, const Replay& deleted);

/// Writes the kept marks as one compact JSON object.
///
/// `{"replays_kept":1,"kept":[{"sha256":"...","path":"..."}]}`; the same marks
/// always give the same text.
///
/// @param kept the marks
/// @return the JSON text
[[nodiscard]] std::string kept_text(const KeptMarks& kept);

/// Reads the kept marks' text.
///
/// Members this version does not know are ignored. An entry that is not an
/// object, holds a member of the wrong type or names neither an identity nor
/// a path is dropped, and so is a later entry of an identity or a path already
/// read.
///
/// @param text the JSON text
/// @return the marks, or nothing for a text that is not JSON, names another
///         version, or is not an object holding the array `kept`
[[nodiscard]] std::optional<KeptMarks> parse_kept(std::string_view text);

/// Returns where a Recordings folder's kept marks are held.
///
/// @param recordings the Recordings folder
/// @return `recordings/.replays/kept.json`
[[nodiscard]] std::filesystem::path kept_path(const std::filesystem::path& recordings);

/// The kept marks as loaded, and whether the file that held them was damaged.
struct KeptRead {
    /// The marks; empty when there were none or the file was damaged.
    KeptMarks kept{};
    /// The file is there but cannot be read, or is not kept marks. A missing
    /// file is not damaged.
    bool damaged{};
};

/// Loads a Recordings folder's kept marks.
///
/// @param recordings the Recordings folder
/// @return the marks, and whether the file was damaged
[[nodiscard]] KeptRead read_kept(const std::filesystem::path& recordings);

/// Saves a Recordings folder's kept marks, whole or not at all, as save_index
/// does the index.
///
/// @param recordings the Recordings folder
/// @param kept the marks
/// @return true when `kept.json` holds the marks; false when the disk refuses
bool save_kept(const std::filesystem::path& recordings, const KeptMarks& kept);

/// Marks a replay the player exported, or stops marking it, as set_kept does.
///
/// Export calls this after it writes the file. The mark is the recording's
/// identity and its path, in `exported.json`, not a folder and not `how`.
///
/// @param[in,out] exported the marks
/// @param replay the replay that was written
/// @param mark true to mark it exported
/// @return true when the marks changed
bool set_exported(KeptMarks& exported, const Replay& replay, bool mark);

/// Sets `exported` of each replay from the marks, as apply_kept does for kept.
///
/// A file a mark names by path stays exported even when another copy still
/// has the mark's identity, and gains a mark of its own.
///
/// @param[in,out] exported the marks; a path may change and a mark may be added
/// @param[in,out] replays the replays
/// @return true when the marks changed, so they are worth saving
bool apply_exported(KeptMarks& exported, std::vector<Replay>& replays);

/// Reads every complete mark from a text that may be cut off.
///
/// Each whole object that names a `sha256` or a `path` string is a mark.
/// The object's place in the text does not matter, and a document that is
/// not valid kept marks still gives the objects that are whole.
///
/// @param text the file's bytes, as text
/// @return the marks those objects hold, sorted and without a repeated identity
[[nodiscard]] KeptMarks salvage_kept(std::string_view text);

/// Tells whether a set-aside kept file's bytes name a replay.
///
/// @param bytes the file's bytes
/// @param sha256 the replay's identity; empty matches nothing
/// @param path the replay's path; empty matches nothing
/// @return true when the identity or the path occurs in `bytes`
[[nodiscard]] bool spared_by_kept_bytes(
    std::string_view bytes, std::string_view sha256, std::string_view path
) noexcept;

/// Finds when the newest set-aside copy of `kept.json` was set aside.
///
/// Every file beside `kept.json` whose name starts `kept.json.damaged`
/// counts, and its modification time is read: set_aside dates each copy as it
/// makes it. A copy that was not dated keeps the time `kept.json` was last
/// written, which is no earlier than any mark it held. A copy whose time
/// cannot be read counts as set aside at the latest time there is.
///
/// @param recordings the Recordings folder
/// @return seconds since 1970 (UTC); nothing when no such copy is there
[[nodiscard]] std::optional<int64_t> kept_set_aside_at(const std::filesystem::path& recordings);

/// Tells whether a replay may have lost its kept mark when `kept.json` was damaged.
///
/// A damaged `kept.json` is set aside and its whole marks are written to a new
/// file, but a mark that was cut off cannot be known. Only a replay recorded
/// by the time the copy was set aside can have had one, so while a copy is
/// there, the retention rule leaves every such replay, however old it is, and
/// nobody is asked again. A replay recorded later goes by the new `kept.json`.
/// The player can still delete any of them. Once no set-aside copy is there,
/// the rule takes them by age again.
///
/// @param replay the replay
/// @param set_aside_at kept_set_aside_at's answer
/// @return true when a copy is there and the replay was recorded at or before
///         the second it was set aside
[[nodiscard]] bool
spared_by_set_aside(const Replay& replay, std::optional<int64_t> set_aside_at) noexcept;

/// Writes the exported marks as one compact JSON object.
///
/// `{"replays_exported":1,"exported":[{"sha256":"...","path":"..."}]}`.
///
/// @param exported the marks
/// @return the JSON text
[[nodiscard]] std::string exported_text(const KeptMarks& exported);

/// Reads the exported marks' text, as parse_kept does.
///
/// @param text the JSON text
/// @return the marks, or nothing for a text that is not those marks
[[nodiscard]] std::optional<KeptMarks> parse_exported(std::string_view text);

/// Returns where a Recordings folder's exported marks are held.
///
/// @param recordings the Recordings folder
/// @return `recordings/.replays/exported.json`
[[nodiscard]] std::filesystem::path exported_path(const std::filesystem::path& recordings);

/// The exported marks as loaded, and whether the file that held them was damaged.
struct ExportedRead {
    /// The marks; empty when there were none or the file was damaged.
    KeptMarks exported{};
    /// The file is there but cannot be read, or is not exported marks. A
    /// missing file is not damaged.
    bool damaged{};
};

/// Loads a Recordings folder's exported marks.
///
/// @param recordings the Recordings folder
/// @return the marks, and whether the file was damaged
[[nodiscard]] ExportedRead read_exported(const std::filesystem::path& recordings);

/// Saves a Recordings folder's exported marks, whole or not at all.
///
/// @param recordings the Recordings folder
/// @param exported the marks
/// @return true when `exported.json` holds the marks; false when the disk refuses
bool save_exported(const std::filesystem::path& recordings, const KeptMarks& exported);

/// Finds a replay of an index by its path.
///
/// @param index the index
/// @param path the replay's path, relative to Recordings
/// @return the replay, or null when the index holds none of that path
[[nodiscard]] const Replay* find(const Index& index, std::string_view path) noexcept;

/// A kind of game an extension records, beyond multiplayer, skirmish and campaign.
struct Kind {
    /// game.kind.
    std::string id{};
    /// The English the Library looks up and shows.
    std::string title{};
};

/// Adds a kind of game to the process-wide list, as an extension starts.
///
/// A kind of the same id replaces the one listed, in its place. An empty id
/// is ignored.
///
/// @param kind the kind
void register_kind(Kind kind);

/// Returns the kinds registered.
///
/// @return the kinds, in the order each id was first registered
[[nodiscard]] std::vector<Kind> registered_kinds();

/// Forgets every kind registered, for tests.
void forget_kinds() noexcept;

/// Counts the days from 1970-01-01 to a civil date of the proleptic Gregorian calendar.
///
/// @param year the year; 0 is 1 BC
/// @param month the month, 1 to 12
/// @param day the day of the month, 1 to 31
/// @return the days since 1970-01-01, negative before it; a date whose count
///         does not fit 32 bits gives the nearest that does
[[nodiscard]] int32_t day_number(int32_t year, uint32_t month, uint32_t day) noexcept;

/// One run of replays recorded on the same day.
struct DayGroup {
    /// The day, as day_number counts it.
    int32_t day{};
    /// The run's first item.
    std::size_t first{};
    /// How many items the run holds.
    std::size_t count{};
};

/// Groups a list's neighbouring items of the same day.
///
/// @param days each item's day, in the order the list shows them
/// @return each run of equal neighbouring days, in order; none for an empty list
[[nodiscard]] std::vector<DayGroup> group_by_day(std::span<const int32_t> days);

/// How long the replays the game recorded are kept.
enum class KeepSpan : uint8_t {
    days_30,  ///< 30 days, the default
    days_90,  ///< 90 days
    one_year, ///< 365 days
    forever,  ///< until the player deletes them
};

/// KeepSpan::days_30, as the preference writes it.
inline constexpr std::string_view keep_30_days = "30-days";
/// KeepSpan::days_90, as the preference writes it.
inline constexpr std::string_view keep_90_days = "90-days";
/// KeepSpan::one_year, as the preference writes it.
inline constexpr std::string_view keep_one_year = "1-year";
/// KeepSpan::forever, as the preference writes it.
inline constexpr std::string_view keep_forever = "forever";

/// The days KeepSpan::days_30 keeps a replay.
inline constexpr int32_t keep_span_30 = 30;
/// The days KeepSpan::days_90 keeps a replay.
inline constexpr int32_t keep_span_90 = 90;
/// The days KeepSpan::one_year keeps a replay.
inline constexpr int32_t keep_span_year = 365;

/// Reads how long replays are kept.
///
/// `90-days`, `1-year` and `forever` are those spans. Anything else, including
/// `30-days` and an empty text, is 30 days.
///
/// @param text the preference's text
/// @return the span
[[nodiscard]] KeepSpan keep_span_of(std::string_view text) noexcept;

/// Writes a span as the preference holds it.
///
/// @param span the span
/// @return `30-days`, `90-days`, `1-year` or `forever`
[[nodiscard]] std::string_view keep_span_text(KeepSpan span) noexcept;

/// Returns how many days a span keeps a replay.
///
/// @param span the span
/// @return the days; nothing for forever
[[nodiscard]] std::optional<int32_t> keep_span_days(KeepSpan span) noexcept;

/// Tells whether a replay was downloaded from Core Prime.
///
/// @param replay the replay
/// @return true when its path starts with `Core Prime/`, or its how is `core-prime`
[[nodiscard]] bool downloaded(const Replay& replay) noexcept;

/// Tells whether a replay was exported by the player.
///
/// @param replay the replay
/// @return true when apply_exported set `exported` from the exported marks
[[nodiscard]] bool exported(const Replay& replay) noexcept;

/// Tells whether the retention rule never takes a replay.
///
/// A replay the player keeps, one opened from outside, one downloaded and one
/// the player exported stay until the player deletes them.
///
/// @param replay the replay
/// @return true when the rule leaves it
[[nodiscard]] bool retention_spares(const Replay& replay) noexcept;

/// Tells whether a replay is old enough for the retention rule to take it.
///
/// The day it was recorded counts. It goes on the day after the span has
/// passed: an age of exactly 30, 90 or 365 days stays, and the next day goes.
/// Forever takes nothing. A spared replay stays however old it is, and a
/// replay dated after today stays.
///
/// @param replay the replay
/// @param recorded_day the local day it was recorded, as day_number counts
/// @param span how long recorded replays are kept
/// @param today the local day now, as day_number counts
/// @return true when it goes
[[nodiscard]] bool
due_to_go(const Replay& replay, int32_t recorded_day, KeepSpan span, int32_t today) noexcept;

} // namespace oa::data::replays
