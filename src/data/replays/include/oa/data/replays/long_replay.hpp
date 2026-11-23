// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Replay files read and exported a piece at a time, so a replay of any length
// is described, exported and shared without being held. A replay file is read
// at offsets, as its zip is read, and a `.tad` in order. A replay longer than
// a whole read takes (`whole_read_most_bytes`, or a recording its description
// says is longer) is a long replay: the readers that held a replay whole read
// it in pieces, and an export of it is written in pieces. A replay a whole
// read takes is still read whole where its export's bytes depend on it.
#pragma once

#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/recording_reader.hpp"
#include "oa/formats/oademo/recording_walk.hpp"
#include "oa/formats/tad.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::data::replays {

/// The most bytes a replay file, or the recording a replay holds, may have
/// to be read whole: the recording format's limit, 512 MiB.
inline constexpr uint64_t whole_read_most_bytes = oa::formats::tad::limit::input_bytes;

/// A replay or recording file, read at offsets or in order and never whole.
class ReplayFile {
  public:

    ReplayFile() = default;
    ReplayFile(const ReplayFile&) = delete;
    ReplayFile& operator=(const ReplayFile&) = delete;

    /// Opens a file for reading.
    ///
    /// @param file the file
    /// @return false when it cannot be opened or its size read
    [[nodiscard]] bool open(const std::filesystem::path& file);

    /// Returns the file's size.
    ///
    /// @return the bytes; 0 before a file was opened
    [[nodiscard]] uint64_t size() const noexcept { return size_; }

    /// Returns the file as a replay's source, read at offsets. It stays valid while this file does.
    ///
    /// @return the source
    [[nodiscard]] oa::formats::oademo::ArchiveSource archive() noexcept;

    /// Returns the file as a recording read in order from its first byte. It
    /// stays valid while this file does; each call starts again at the first byte.
    ///
    /// @return the source
    [[nodiscard]] oa::formats::oademo::RecordingSource recording() noexcept;

    /// Reads the file's first bytes.
    ///
    /// @param count at most this many
    /// @return the bytes; fewer when the file is shorter, none when it does not read
    [[nodiscard]] std::vector<uint8_t> first_bytes(std::size_t count);

  private:

    /// Fills `into` with the file's bytes from `offset` on (ArchiveSource::read_at).
    ///
    /// @param context the file
    /// @param offset where the bytes start
    /// @param into room for them
    /// @return false when they cannot all be read
    static bool read_at(void* context, uint64_t offset, std::span<uint8_t> into);

    /// Reads the file's next bytes in order (RecordingSource::read).
    ///
    /// @param context the file
    /// @param into room for them
    /// @param[out] got how many were read; fewer only at the end
    /// @return false when the file cannot be read
    static bool read_on(void* context, std::span<uint8_t> into, std::size_t& got);

    std::ifstream in_{};
    uint64_t size_{};
    uint64_t next_{}; ///< where the next read in order reads
};

/// A file a replay is written to a piece at a time, at offsets; or, counting
/// only, the size such a file would have.
class ReplayOutput {
  public:

    ReplayOutput() = default;
    ReplayOutput(const ReplayOutput&) = delete;
    ReplayOutput& operator=(const ReplayOutput&) = delete;

    /// Creates a file, or empties one that is there, for writing.
    ///
    /// @param file the file
    /// @return false when it cannot be opened
    [[nodiscard]] bool open(const std::filesystem::path& file);

    /// Writes nothing and counts the bytes a file would hold.
    void count_only() noexcept;

    /// Returns where the replay's bytes go. It stays valid while this output does.
    ///
    /// @return the output
    [[nodiscard]] oa::formats::oademo::ArchiveOutput archive() noexcept;

    /// Returns the size written so far: the end of the furthest bytes written.
    ///
    /// @return the bytes
    [[nodiscard]] uint64_t bytes() const noexcept { return bytes_; }

    /// Writes what is held and closes the file.
    ///
    /// @return false when a write failed
    [[nodiscard]] bool close();

  private:

    /// Writes `bytes` at `offset`, or counts them (ArchiveOutput::write_at).
    ///
    /// @param context the output
    /// @param offset where they go
    /// @param bytes the bytes
    /// @return false when they cannot all be written
    static bool write_at(void* context, uint64_t offset, std::span<const uint8_t> bytes);

    std::fstream out_{};
    uint64_t bytes_{};
    bool counting_{};
};

/// Reads a `.oademo` file's description and manifest, a piece at a time.
///
/// @param file the replay
/// @return the description and the manifest, or the sentence from
///         oa/formats/oademo/interface_words.hpp that says why not;
///         not_a_replay when the file cannot be opened
[[nodiscard]] oa::formats::oademo::DescriptionResult
read_description_file(const std::filesystem::path& file);

/// Tells whether a replay file is too long to read whole.
///
/// A file past `whole_read_most_bytes` is. So is a `.oademo` whose
/// description names a recording past it, which a whole read refuses to
/// decompress.
///
/// @param file the replay or the recording
/// @return true when it is read and exported a piece at a time
[[nodiscard]] bool long_replay(const std::filesystem::path& file);

/// Where an export written a piece at a time goes, in order.
struct ExportSink {
    /// Passed to `write`.
    void* context{};
    /// Takes the next bytes. False stops the export. Null takes nothing, so
    /// the export only measures.
    bool (*write)(void* context, std::span<const uint8_t> bytes){};
};

/// What a caller watches while an export is written a piece at a time.
struct ExportProgress {
    /// Passed to `report` and `stop`.
    void* context{};
    /// Told how many of the source's recording bytes have been taken, of
    /// `whole` (0 when not known). Null tells nothing.
    void (*report)(void* context, uint64_t taken, uint64_t whole){};
    /// Asked between pieces. True stops the export. Null never stops.
    bool (*stop)(void* context){};
};

/// A recording cleaned for export a piece at a time, or why it was not.
struct CleanedRecording {
    /// Why it was not, as tad::export_recording or the replay's reader says;
    /// empty when it was. "the export was stopped" when `ExportProgress` stopped it.
    std::string error{};
    /// The cleaned recording's size.
    uint64_t bytes{};
    /// Its SHA-256, lower-case hexadecimal.
    std::string sha256{};
    /// Its packets.
    uint64_t packets{};
    /// The recorder's clock at its last packet, in milliseconds; 0 with no packet.
    uint64_t last_time_ms{};
    /// What tad::export_recording notices of the same recording, in its order.
    std::vector<oa::formats::tad::ExportNotice> notices{};
    /// True when the source's recording already was plain and held no
    /// network address: no packet was expanded and no address sector dropped.
    bool clean{};

    /// Tells whether the recording was cleaned.
    ///
    /// @return true when `error` is empty
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Writes the recording of a replay or `.tad` file as tad::export_recording
/// writes it, a packet at a time: the same bytes, the same notices and the
/// same refusals, for a recording of any length.
///
/// @param file the `.oademo` or `.tad`
/// @param options which sectors to keep, and whether names and statuses are cut
/// @param sink where the bytes go; a null `write` measures only
/// @param progress progress and a way to stop; may be null
/// @return the cleaned recording's size, digest and packets, or why not
[[nodiscard]] CleanedRecording write_cleaned_recording(
    const std::filesystem::path& file,
    const oa::formats::tad::ExportOptions& options,
    const ExportSink& sink,
    const ExportProgress* progress = nullptr
);

/// Writes a `.oademo` export of a long replay file.
///
/// A recording that is plain and holds no network address keeps its own
/// zstd frame, copied as it is, as an export of a replay held whole keeps
/// its recording. Its frame is not compressed again at
/// oa::formats::oademo::level_on_export: a recording gigabytes long takes
/// far too long at that level. One that is not is cleaned as an export
/// cleans it, compressed a piece at a time at
/// oa::formats::oademo::level_when_a_game_ends into `frame_file`, and then
/// stored; its description's recording identity and packet count follow the
/// cleaned bytes. `timeline`, when set, replaces the replay's.
///
/// @param file the long `.oademo`
/// @param timeline the timeline to write; nothing keeps the replay's own
/// @param frame_file a scratch file for a frame compressed again; removed afterwards
/// @param out where the archive goes; a positioned write
/// @param progress progress and a way to stop; may be null
/// @return empty, or why the export was not written
[[nodiscard]] std::string export_long_replay(
    const std::filesystem::path& file,
    const std::optional<std::string>& timeline,
    const std::filesystem::path& frame_file,
    const oa::formats::oademo::ArchiveOutput& out,
    const ExportProgress* progress = nullptr
);

/// Writes a `.oademo` whose recording is the cleaned recording of a `.tad`
/// or replay file (Open Annihilation's sectors kept, names not cut),
/// compressed a piece at a time at `level` into `frame_file` and then stored.
///
/// The description's recording identity, packet count and, when it names
/// none, length follow the cleaned recording; the rest of the description,
/// the manifest and the timeline are `replay`'s.
///
/// @param file the `.tad` or `.oademo`
/// @param replay the description, the manifest and the timeline
/// @param level the zstd level
/// @param frame_file a scratch file for the frame; removed afterwards
/// @param out where the archive goes; a positioned write
/// @param progress progress and a way to stop; may be null
/// @return empty, or why the replay was not written
[[nodiscard]] std::string write_replay_in_pieces(
    const std::filesystem::path& file,
    oa::formats::oademo::Replay replay,
    int level,
    const std::filesystem::path& frame_file,
    const oa::formats::oademo::ArchiveOutput& out,
    const ExportProgress* progress = nullptr
);

} // namespace oa::data::replays
