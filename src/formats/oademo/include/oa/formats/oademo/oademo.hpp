// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A .oademo replay: a zip whose first entry is the line `oademo 1`, then a
// description, a manifest, an optional timeline, and one recording. The
// recording is a .tad stream, every packet plain, either one zstd frame or
// a deflated entry. The same description and recording always give the same
// bytes. A member this version does not know is kept and written back.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oademo {

/// The version this reader and writer use.
inline constexpr int format_version = 1;

/// Deflate level of every deflated entry, including a deflated recording.
inline constexpr int deflate_level = 6;

/// zstd level used when a game ends.
inline constexpr int level_when_a_game_ends = 3;

/// zstd level used when a replay is exported.
inline constexpr int level_on_export = 19;

/// The line stored in the first entry, including its newline.
inline constexpr std::string_view signature_line = "oademo 1\n";

/// One JSON member this version does not interpret, kept in the order read.
struct JsonExtra {
    /// The member's name.
    std::string name{};
    /// The member's value as one compact JSON value.
    std::string json{};
};

/// The recording entry's identity and size.
struct RecordingInfo {
    /// SHA-256 of the uncompressed recording, as hexadecimal.
    std::string sha256{};
    /// Size of the uncompressed recording, in bytes.
    uint64_t bytes{};
    /// The recording container's version.
    int tad_version{5};
    /// How many packets the recording holds.
    uint64_t packets{};
    /// True when the recording holds no network address.
    bool addresses_removed{true};
    /// Members of `recording` this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// Who recorded the game, and how.
struct RecordedInfo {
    /// An instant in UTC, `YYYY-MM-DDTHH:MM:SSZ`. Set only by a writer that
    /// knows the zone, such as a recording made from the clock. A tool that
    /// wraps a `.tad` leaves it out.
    std::optional<std::string> at{};
    /// The date as the recording wrote it. A date copied from a `.tad` has no
    /// time zone, and is kept here only.
    std::optional<std::string> local{};
    /// The program line the recording names.
    std::optional<std::string> by{};
    /// The machine's platform, when the writer knew it.
    std::optional<std::string> platform{};
    /// How the game was recorded: multiplayer, skirmish, campaign, imported,
    /// core-prime, or a value an extension names.
    std::string how{};
    /// True when the recording was finished after a crash.
    bool after_crash{false};
    /// Members of `recorded` this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// What kind of game was played, and how long it ran.
struct GameInfo {
    /// skirmish, campaign, multiplayer, or a kind an extension names.
    std::string kind{};
    /// Length in simulation ticks, when known.
    std::optional<uint64_t> length_ticks{};
    /// Length in milliseconds, from the packet times when a recording is wrapped.
    std::optional<uint64_t> length_ms{};
    /// finished, left, cut, or a value an extension names.
    std::optional<std::string> ended{};
    /// The object under `game` whose name is `kind`, as compact JSON.
    std::optional<std::string> kind_object{};
    /// Members of `game` this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// The map the game was played on.
struct MapInfo {
    /// The map's name.
    std::string name{};
    /// SHA-256 of the map, when the writer knew the file.
    std::optional<std::string> sha256{};
    /// Members of `map` this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// One package named in the description or the manifest.
struct PackageRef {
    /// The package id.
    std::string id{};
    /// The package version.
    std::string version{};
    /// Members this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// A mod named in the description.
struct ModRef {
    /// The mod id.
    std::string id{};
    /// The mod's name.
    std::string name{};
    /// The mod's version.
    std::string version{};
    /// The mod's revision, when named.
    std::optional<int64_t> revision{};
    /// The simulation's digest, when named.
    std::optional<std::string> sim_hash{};
    /// Members this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// Weapon types and sides the game numbered.
struct LimitsInfo {
    /// Weapon types the game numbered.
    int64_t weapon_types{256};
    /// Sides the game loaded.
    int64_t sides{5};
    /// Members this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// The content the game was played with.
struct ContentInfo {
    /// The base game.
    PackageRef base{};
    /// Expansion ids.
    std::vector<std::string> expansions{};
    /// The mod, when one was played.
    std::optional<ModRef> mod{};
    /// Mutator ids.
    std::vector<std::string> mutators{};
    /// Brain ids.
    std::vector<std::string> brains{};
    /// The two limits.
    LimitsInfo limits{};
    /// Members of `content` this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// The recorded unit table's digest.
struct UnitsInfo {
    /// How many definitions the digest covers.
    uint32_t count{};
    /// SHA-256 of the recorded unit table, as hexadecimal.
    std::string table_sha256{};
    /// The 0x2c index width: the bit length of `count + 1`.
    unsigned index_bits{};
    /// Members of `units` this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// One player, as the description lists them.
struct PlayerInfo {
    /// The player's number in the recording.
    int number{};
    /// The player's name.
    std::string name{};
    /// arm, core, watcher, or the side as text.
    std::string side{};
    /// The player's colour.
    int colour{};
    /// The player's team, when known.
    std::optional<int> team{};
    /// human, computer or watcher.
    std::string kind{};
    /// True for the machine that recorded, when known.
    std::optional<bool> recorded_here{};
    /// won, lost, or a value an extension names, when known.
    std::optional<std::string> result{};
    /// Units the player destroyed, when known.
    std::optional<int64_t> kills{};
    /// Members of this player this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// What the Library shows.
struct Meta {
    /// The container version. This reader accepts 1.
    int oademo{format_version};
    /// The recording's identity.
    RecordingInfo recording{};
    /// Who recorded it.
    RecordedInfo recorded{};
    /// The game.
    GameInfo game{};
    /// The map.
    MapInfo map{};
    /// The content.
    ContentInfo content{};
    /// The unit table.
    UnitsInfo units{};
    /// The players, in the recording's order.
    std::vector<PlayerInfo> players{};
    /// Members of the description this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// One map a package has to hold for the replay to be watched.
struct MapNeed {
    /// The map's name.
    std::string name{};
    /// SHA-256 of the map's `.ota` and `.tnt`, as hexadecimal.
    std::string sha256{};
    /// Members of this map this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// One thing that has to be installed to watch the replay.
struct Need {
    /// base, expansion, oamod, oamap, oamut, or a kind an extension names.
    std::string kind{};
    /// The package id.
    std::string id{};
    /// The version, when the kind names one.
    std::optional<std::string> version{};
    /// The revision, when the kind names one.
    std::optional<int64_t> revision{};
    /// SHA-256 of the package file, when named.
    std::optional<std::string> sha256{};
    /// The simulation's digest, when named.
    std::optional<std::string> sim_hash{};
    /// The unit table's digest, when named.
    std::optional<std::string> units_table_sha256{};
    /// The maps a map pack has to hold, each by name and digest. Empty is not written.
    std::vector<MapNeed> maps{};
    /// Members of this need this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// The engine that recorded the replay, and the oldest that plays it.
struct EngineNeed {
    /// The version that recorded it.
    std::string recorded_with{};
    /// The oldest version that plays it, such as `>=0.8.14`.
    std::string needs{};
    /// Members of `engine` this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// What has to be installed to watch the replay.
struct Manifest {
    /// The container version. This reader accepts 1.
    int oademo{format_version};
    /// The engine.
    EngineNeed engine{};
    /// The packages, in the order written.
    std::vector<Need> needs{};
    /// Members of the manifest this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// One replay: its description, its manifest, an optional timeline, and the
/// uncompressed recording.
struct Replay {
    /// The description.
    Meta meta{};
    /// The manifest.
    Manifest manifest{};
    /// `timeline.json` as one compact JSON value, when the replay has one.
    std::optional<std::string> timeline{};
    /// The uncompressed recording.
    std::vector<uint8_t> recording{};
};

/// A replay read from a file, or the sentence that says why not.
struct ReadResult {
    /// The replay. Set only when `error` is empty.
    std::optional<Replay> replay{};
    /// A sentence from interface_words.hpp, or empty when the read finished.
    std::string error{};

    /// Tells whether the file was read.
    ///
    /// @return true when `error` is empty and `replay` is set
    [[nodiscard]] bool ok() const noexcept { return error.empty() && replay.has_value(); }
};

/// A replay's description and manifest, read without its recording, or the
/// sentence that says why not.
struct DescriptionResult {
    /// The description. Set only when `error` is empty.
    std::optional<Meta> meta{};
    /// The manifest. Set only when `error` is empty.
    std::optional<Manifest> manifest{};
    /// A sentence from interface_words.hpp, or empty when both were read.
    std::string error{};

    /// Tells whether the description was read.
    ///
    /// @return true when `error` is empty and `meta` is set
    [[nodiscard]] bool ok() const noexcept { return error.empty() && meta.has_value(); }
};

/// What a caller watches while a zstd recording is compressed.
///
/// The callbacks run on the thread that writes, after some of the recording
/// has been taken and before the next piece. Either may be null. Left unset,
/// `write` compresses in one step, and the bytes are the ones it has always
/// written.
struct WriteWatch {
    /// Told how many recording bytes have been taken, of `whole`.
    void (*report)(void* context, uint64_t taken, uint64_t whole){};
    /// Asked before the next piece. True stops before the frame has ended.
    bool (*stop)(void* context){};
    /// Passed to `report` and `stop`.
    void* context{};
};

/// How a replay is compressed.
struct WriteOptions {
    /// zstd level, from 1 to the highest zstd accepts. Ignored when `deflate` is set.
    int level{level_when_a_game_ends};
    /// Store the recording deflated, instead of as one zstd frame.
    bool deflate{false};
    /// Progress and a way to stop. Set only when the caller wants either;
    /// left unset, the recording is compressed in one step.
    WriteWatch watch{};
};

/// The bytes of a replay, or why they were not written.
struct WriteResult {
    /// The archive. Empty when `error` is set.
    std::vector<uint8_t> bytes{};
    /// Why the archive was not written. Empty when it was.
    std::string error{};

    /// Tells whether the archive was written.
    ///
    /// @return true when `error` is empty
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// The unit table a recording's unit-check records name.
struct UnitTableDigest {
    /// SHA-256 of the table, as lower-case hexadecimal.
    std::string sha256{};
    /// How many definitions the digest covers.
    uint32_t count{};
    /// The 0x2c index width: the bit length of `count + 1`.
    unsigned index_bits{};
};

/// Tells whether `bytes` open like a `.oademo`.
///
/// Reads the zip local header and the start of the first entry only. A file
/// whose first entry is the stored `oademo` line of any version is a
/// `.oademo`: `read` then says whether this game reads that version. Bytes
/// that pass may still fail `read`.
///
/// @param bytes a whole file, or any other bytes
/// @return true when the first entry is the stored name `oademo` and its
///         payload starts with `oademo ` (the word and a space)
[[nodiscard]] bool recognised(std::span<const uint8_t> bytes) noexcept;

/// Reads a `.oademo`.
///
/// Checks the recording's size against the recording format's limit before
/// the recording is allocated, and checks its SHA-256 against the description.
///
/// @param bytes the whole file
/// @return the replay, or a sentence from interface_words.hpp
[[nodiscard]] ReadResult read(std::span<const uint8_t> bytes);

/// Reads a `.oademo`'s description and manifest, checking its layout as
/// `read` does, without reading, decompressing or checking its recording.
///
/// Makes the checks and refusals `read` makes up to and including reading
/// `meta.json` and `manifest.json`: the zip directory, the stored `oademo`
/// entry and its line, one recording entry, the 4 or 5 entries in order and
/// their methods, and the size a deflated recording names. The timeline is
/// not read. Bytes `read` refuses for their recording or their timeline read
/// here.
///
/// @param bytes the whole file
/// @return the description and the manifest, or the sentence from
///         interface_words.hpp that `read` gives for the same bytes
[[nodiscard]] DescriptionResult read_description(std::span<const uint8_t> bytes);

/// Writes a `.oademo`.
///
/// The same replay and options give the same bytes when no watch is set.
/// A watch that is set compresses the recording in pieces, so those bytes
/// need not match a write of the same replay with no watch; two such writes
/// at the same level still match each other. Entry times are fixed.
/// Deflated entries use `deflate_level`. A zstd recording is one frame.
///
/// @param replay the replay to write; `recording.sha256` and `recording.bytes`
///        are written as given
/// @param options the zstd level, or deflate
/// @return the archive, or a sentence when the replay cannot be written
[[nodiscard]] WriteResult write(const Replay& replay, WriteOptions options = {});

/// Sets `recording.sha256` and `recording.bytes` from `replay.recording`.
///
/// @param replay the replay whose recording identity is filled in
void set_recording_identity(Replay& replay);

/// Writes the description as compact JSON.
///
/// Known members are written in a fixed order, then any member this version
/// does not interpret. The same description gives the same text.
///
/// @param meta the description
/// @return the JSON text
[[nodiscard]] std::string meta_text(const Meta& meta);

/// Writes the manifest as compact JSON.
///
/// @param manifest the manifest
/// @return the JSON text
[[nodiscard]] std::string manifest_text(const Manifest& manifest);

/// What a caller adds when a recording is exported as a `.oademo`.
struct RecordingExport {
    /// The version line a `.tad` is wrapped with. A re-export of a `.oademo`
    /// keeps the file's own and ignores this.
    std::string recorded_with{};
    /// `timeline.json` to write. When unset, a re-export keeps the file's
    /// timeline and a `.tad` is wrapped with none.
    std::optional<std::string> timeline{};
    /// Progress and a way to stop, for the recording's compression. Null
    /// compresses in one step.
    const WriteWatch* watch{};
};

/// Writes a `.oademo` export of a replay already read, at `level_on_export`.
///
/// A recording that holds a network address, or a packet that is not plain,
/// is cleaned first: the address goes, the packet is expanded, and the
/// description's recording identity follows the cleaned bytes. A recording
/// that is already plain and holds no address is stored unchanged, so its
/// identity stays. A timeline the caller supplies replaces the file's; none
/// supplied keeps it. Open Annihilation's own sectors are kept. Names are
/// not cut. The same replay and options give the same bytes when no watch
/// is set. A watch compresses the recording in pieces, so those bytes need
/// not match an export of the same replay with no watch; two exports with a
/// watch at the same level still match each other.
///
/// @param source the replay
/// @param options a timeline to write instead of the file's
/// @return the archive, or a sentence when it cannot be written
[[nodiscard]] WriteResult export_replay(const Replay& source, const RecordingExport& options = {});

/// Wraps a `.tad` as a `.oademo` export, at `level_on_export`.
///
/// The recording is stored as plain packets, with no network address and
/// with Open Annihilation's own sectors kept. Names are not cut. The
/// description is filled from the recording's header. `options.recorded_with`
/// is the version that writes it, and `options.timeline` is written when set.
/// The same bytes and options give the same archive.
///
/// @param bytes a whole recording
/// @param options the version line, and a timeline when the caller has one
/// @return the archive, or a sentence when the recording cannot be exported
[[nodiscard]] WriteResult
export_tad(std::span<const uint8_t> bytes, const RecordingExport& options = {});

/// Computes `units.table_sha256` from a recording's unit-check chunk.
///
/// When the chunk holds a verdict, each definition both sides accepted is
/// taken in the order it was first seen: its FBI hash and its content
/// checksum, each a little-endian u32. With no verdict, the checksum records
/// are taken in order. `index_bits` is the bit length of `count + 1`.
///
/// @param unit_checks the unit-check chunk, 14-byte records back to back
/// @return the digest, the count and the index width
[[nodiscard]] UnitTableDigest unit_table_digest(std::span<const uint8_t> unit_checks);

} // namespace oa::formats::oademo
