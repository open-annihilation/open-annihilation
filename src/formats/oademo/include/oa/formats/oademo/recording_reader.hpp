// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A replay's recording read a piece at a time, from a file or from bytes held,
// without holding the recording. The description and the manifest are read and
// checked as read_description reads them; the recording entry's zstd frame is
// decoded through a fixed window (a deflated recording is inflated through a
// fixed window); and the recording's size and SHA-256 are checked against the
// description as its last byte is read. Nothing held grows with the recording,
// so a replay plays whatever its length.

#include "oa/formats/oademo/oademo.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace oa::formats::oademo {

/// The most bytes a replay read in pieces may hold, and so the most its
/// stored recording frame may: 64 GiB, about thirty hours of a battle of
/// 20,000 units. The reader holds nothing that grows with them.
inline constexpr uint64_t most_streamed_archive_bytes = uint64_t{64} << 30;

/// Where a replay's bytes come from: a positioned read, as from a file.
struct ArchiveSource {
    /// Passed to `read_at`.
    void* context{};
    /// Fills `into` with the replay's bytes from `offset` on. False when they
    /// cannot all be read. Null reads nothing, so every read fails.
    bool (*read_at)(void* context, uint64_t offset, std::span<uint8_t> into){};
    /// The replay's size in bytes.
    uint64_t bytes{};
};

/// A replay's recording, read in order a piece at a time.
///
/// Open it, then read until a read gives no byte. The recording can be read
/// again from its start with `rewind`. A refusal stays until the next
/// `open` or `rewind`.
class RecordingReader {
  public:

    /// Makes a reader with no replay open.
    RecordingReader();

    /// Takes another reader's replay and position.
    ///
    /// @param other the reader taken from; it is left with no replay open
    RecordingReader(RecordingReader&& other) noexcept;

    /// Takes another reader's replay and position, closing this one's.
    ///
    /// @param other the reader taken from; it is left with no replay open
    /// @return this reader
    RecordingReader& operator=(RecordingReader&& other) noexcept;

    RecordingReader(const RecordingReader&) = delete;
    RecordingReader& operator=(const RecordingReader&) = delete;

    /// Frees the window and the buffers.
    ~RecordingReader();

    /// Opens a replay's recording.
    ///
    /// Reads the zip directory (the 64-bit extension included), then makes
    /// the checks read_description makes and reads the description and the
    /// manifest. The source must outlive the reader's reads.
    ///
    /// @param source where the replay's bytes come from
    /// @return a sentence from interface_words.hpp, or empty when the
    ///         recording can be read
    [[nodiscard]] std::string open(const ArchiveSource& source);

    /// Returns the description read at `open`.
    ///
    /// @return the description; empty before a replay was opened
    [[nodiscard]] const Meta& meta() const noexcept;

    /// Returns the manifest read at `open`.
    ///
    /// @return the manifest; empty before a replay was opened
    [[nodiscard]] const Manifest& manifest() const noexcept;

    /// Reads the recording's next bytes.
    ///
    /// Fills `out` unless the recording ends first. When its last byte has
    /// been read, its size and SHA-256 are checked against the description.
    ///
    /// @param out room for the bytes
    /// @param[out] got how many bytes of `out` were written; 0 at the end
    /// @return empty, or a sentence: not_a_replay (the archive does not
    ///         read), recording_unreadable (the frame is damaged or cut
    ///         short) or recording_mismatch (more bytes than the description
    ///         names, or another digest)
    [[nodiscard]] std::string read(std::span<uint8_t> out, std::size_t& got);

    /// Starts the recording again from its first byte.
    ///
    /// @return empty, or not_a_replay when the entry cannot be opened again
    [[nodiscard]] std::string rewind();

    /// Returns how many bytes of the recording have been read since the start.
    ///
    /// @return the bytes
    [[nodiscard]] uint64_t position() const noexcept;

    /// Tells whether the whole recording has been read and checked.
    ///
    /// @return true once a read reached its end and its size and digest matched
    [[nodiscard]] bool finished() const noexcept;

  private:

    /// The open replay, its entry, the frame's window and the buffers.
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace oa::formats::oademo
