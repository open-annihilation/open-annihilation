// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The streamed zip reader: an archive read a piece at a time through a hook,
// however large, as a mod package (.oamod) is. It reads the central
// directory, the 64-bit extension included, then each entry's data in fixed
// pieces, inflating deflated data through a fixed window and checking each
// entry's size and CRC-32 as it ends. Nothing it holds grows with an entry's
// size: about 44 KiB of decoder state and two 64 KiB buffers for an open
// entry, and the directory, bounded by StreamLimits.
//
// It refuses what the in-memory reader refuses (oa/formats/zip.hpp), apart
// from the 64-bit extension, which it reads, and adds: 64-bit records that
// are missing or disagree with the others, a directory larger than its
// limit, entries whose data overlaps another entry's or the directory, and
// names marked UTF-8 that are not. Names not marked UTF-8 are read as code
// page 437, the format's own default, and the backslashes of an entry made
// on MS-DOS or Windows become '/'. Links and special files are marked, not
// refused: the caller decides.
#pragma once

#include "oa/formats/zip.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace oa::formats::zip {

/// Where a streamed archive's bytes come from.
struct SourceHooks {
    void* context{}; ///< passed back to read_at
    /// Reads bytes.size() bytes starting `offset` bytes into the archive into
    /// `bytes`; false when they cannot all be read. Null reads nothing, so
    /// every read fails.
    bool (*read_at)(void* context, uint64_t offset, std::span<uint8_t> bytes){};
};

/// Where an entry's data goes as it is read.
struct SinkHooks {
    void* context{}; ///< passed back to write
    /// Takes the next piece of the entry's uncompressed data, in order; false
    /// stops the read with ZipStatus::write_failed. Null drops the data.
    bool (*write)(void* context, std::span<const uint8_t> bytes){};
};

/// The bounds a streamed read keeps.
struct StreamLimits {
    uint64_t max_archive_bytes{uint64_t{64} << 30}; ///< 64 GiB
    uint32_t max_entry_count{65'535};               ///< entries in the directory
    /// The central directory's size, in bytes: its records, names, extra
    /// fields and comments together.
    uint64_t max_directory_bytes{uint64_t{8} << 20};
    uint64_t max_entry_bytes{uint64_t{4} << 30}; ///< one entry, uncompressed
};

/// One entry of a streamed archive's central directory.
struct StreamEntry {
    std::string name{}; ///< UTF-8, '/' between folders, as name_is_safe accepts
    /// The name's bytes as recorded, which the local header repeats; empty
    /// when they are `name`'s own.
    std::string recorded_name{};
    Method method{Method::stored};
    uint32_t crc32{};            ///< of the uncompressed data
    uint64_t compressed_bytes{}; ///< the data as stored
    uint64_t bytes{};            ///< uncompressed
    uint64_t local_header_offset{};
    /// Where the next entry's local header, or the directory, starts: the
    /// entry's header and data end at or before it.
    uint64_t data_limit{};
    bool directory{};     ///< the name ends in '/'
    bool symbolic_link{}; ///< made on Unix or macOS with the link file type
    bool special_file{};  ///< made on Unix or macOS as a device, pipe or socket
    /// Made on Unix or macOS: the file type and permission bits the entry
    /// records, the high 16 bits of its external attributes. 0 for an entry
    /// made on another system.
    uint32_t unix_mode{};
};

/// A streamed archive's central directory.
struct StreamDirectory {
    std::vector<StreamEntry> entries{}; ///< in central directory order
    uint64_t total_bytes{};             ///< every entry's uncompressed size added up
    uint64_t total_compressed_bytes{};  ///< every entry's stored size added up
    bool zip64{};                       ///< the 64-bit extension's end records were read
};

/// Returns the bytes an entry's local header repeats as its name.
///
/// @param entry the entry
/// @return recorded_name, or the name when they are the same
[[nodiscard]] const std::string& recorded_name_of(const StreamEntry& entry) noexcept;

/// Reads a streamed archive's central directory.
///
/// Finds the end record (and before it, the 64-bit locator and the record
/// it names), checks the counts, sizes and offsets against each other and
/// the limits, reads the directory into one buffer, freed on return, and
/// checks each record: its 64-bit fields, flags, method, name and sizes,
/// then that no two names are equal and that no entry's local header, name
/// and data reach the next entry or the directory.
///
/// @param source where the archive's bytes come from
/// @param archive_bytes the archive's size
/// @param limits the bounds kept
/// @param[out] directory the entries; left empty on failure
/// @param[out] error the status, and the record at fault
/// @return true when every entry was read
[[nodiscard]] bool read_stream_directory(
    const SourceHooks& source,
    uint64_t archive_bytes,
    const StreamLimits& limits,
    StreamDirectory& directory,
    ZipError& error
);

/// How far a step of an entry's read came.
enum class StreamStep : uint8_t {
    more,   ///< data is left to read
    done,   ///< the whole entry was read and checked
    failed, ///< the read stopped; the error says why
};

/// Reads one entry's data in steps, inflating deflated data through a fixed
/// window and fixed buffers, and checking its size and CRC-32 at its end.
class EntryStream {
  public:

    /// Makes a stream with no entry open.
    EntryStream();

    /// Takes another stream's entry, buffers and decoder.
    ///
    /// @param other the stream taken from; it is left with no entry open
    EntryStream(EntryStream&& other) noexcept;

    /// Takes another stream's entry, buffers and decoder, ending this one's.
    ///
    /// @param other the stream taken from; it is left with no entry open
    /// @return this stream
    EntryStream& operator=(EntryStream&& other) noexcept;
    EntryStream(const EntryStream&) = delete;
    EntryStream& operator=(const EntryStream&) = delete;

    /// Ends the decoder and frees the buffers.
    ~EntryStream();

    /// Opens an entry: reads and checks its local header against the entry,
    /// and that its data lies within the archive and before data_limit.
    ///
    /// @param source where the archive's bytes come from; it must outlive the reads
    /// @param archive_bytes the archive's size
    /// @param entry the entry, as read_stream_directory gave it
    /// @param[out] error the status, and the local header's offset
    /// @return true when the entry can be read
    [[nodiscard]] bool open(
        const SourceHooks& source, uint64_t archive_bytes, const StreamEntry& entry, ZipError& error
    );

    /// Reads on: at most `input_budget` bytes of the archive (at least one),
    /// and at most four times as many, or 256 KiB, of the entry's data, each
    /// piece handed to `sink` in order. Data past the entry's recorded size
    /// is never handed on: its first piece stops the read.
    ///
    /// @param sink where the data goes
    /// @param input_budget the archive bytes this step may read
    /// @param[out] error the status when the read fails
    /// @return more, done once the size and CRC-32 checked, or failed
    [[nodiscard]] StreamStep step(const SinkHooks& sink, uint64_t input_budget, ZipError& error);

    /// Returns the archive bytes of the entry's data read so far.
    ///
    /// @return the bytes
    [[nodiscard]] uint64_t input_done() const noexcept;

    /// Returns the uncompressed bytes handed on so far.
    ///
    /// @return the bytes
    [[nodiscard]] uint64_t output_done() const noexcept;

  private:

    struct State;
    std::unique_ptr<State> state_;
};

/// Reads a whole entry into memory, as for a small file such as a profile.
///
/// @param source where the archive's bytes come from
/// @param archive_bytes the archive's size
/// @param entry the entry
/// @param most_bytes the most bytes the entry may hold; a larger entry is
///        entry_too_large
/// @param[out] bytes the data; left empty on failure
/// @param[out] error the status, and the record at fault
/// @return true when the data was read and checked
[[nodiscard]] bool read_stream_entry(
    const SourceHooks& source,
    uint64_t archive_bytes,
    const StreamEntry& entry,
    uint64_t most_bytes,
    std::vector<uint8_t>& bytes,
    ZipError& error
);

} // namespace oa::formats::zip
