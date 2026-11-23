// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A recording's packets read in order a piece at a time: a `.tad` whose bytes
// come in order, or a replay's recording read through RecordingReader. The
// part before the packets is read whole and parsed as tad::parse parses it,
// and each packet is checked as tad::parse checks it, with the same error at
// the same offset. Nothing held grows with the recording: the part before the
// packets, at most `most_head_bytes`, and one piece of the packets are held,
// so a recording of any length is read.

#include "oa/formats/oademo/recording_reader.hpp"
#include "oa/formats/tad.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace oa::formats::oademo {

/// The most bytes before a recording's first packet: the header, the sector
/// count, 256 sectors, ten players and their statuses, and the unit checks,
/// each a chunk of at most 65,535 bytes.
inline constexpr std::size_t most_head_bytes =
    (2 + tad::limit::sectors + 2 * tad::limit::players + 1) * tad::limit::chunk_bytes;

/// Where a `.tad` recording's bytes come from, in order from its first.
struct RecordingSource {
    /// Passed to `read`.
    void* context{};
    /// Fills the start of `into` with the recording's next bytes. `got` is
    /// how many were written: fewer than asked only at the end, 0 past it.
    /// False when they cannot be read. Null reads nothing, so every read fails.
    bool (*read)(void* context, std::span<uint8_t> into, std::size_t& got){};
};

/// Why a recording read in pieces stopped.
struct WalkFailure {
    /// The sentence a replay's reader gave (interface_words.hpp), which a
    /// whole read of the replay gives first. Empty for a `.tad`, and when the
    /// replay's bytes read.
    std::string sentence{};
    /// True when a `.tad`'s source refused a read.
    bool unreadable{};
    /// What tad::parse says of the same recording, when its bytes read and
    /// do not parse. Its offset is from the recording's start.
    std::optional<tad::Error> parse{};
};

/// A recording's packets, read in order.
///
/// Open it, then take packets until `next` says the recording ended. Opening
/// reads the part before the packets; `head` is that part parsed, with no
/// packet. Every refusal stays: later calls fail with it.
class RecordingWalk {
  public:

    /// Makes a walk with no recording open.
    RecordingWalk();

    /// Takes another walk's recording and place.
    ///
    /// @param other the walk taken from; it is left with no recording open
    RecordingWalk(RecordingWalk&& other) noexcept;

    /// Takes another walk's recording and place, closing this one's.
    ///
    /// @param other the walk taken from; it is left with no recording open
    /// @return this walk
    RecordingWalk& operator=(RecordingWalk&& other) noexcept;

    RecordingWalk(const RecordingWalk&) = delete;
    RecordingWalk& operator=(const RecordingWalk&) = delete;

    /// Frees the buffers and the replay's reader.
    ~RecordingWalk();

    /// Opens a `.tad` recording and reads the part before its packets.
    ///
    /// @param source where the recording's bytes come from; it must outlive the walk's reads
    /// @return false when the source refused a read, or the part before the
    ///         packets does not parse (`failure` says which)
    [[nodiscard]] bool open_tad(const RecordingSource& source);

    /// Opens a replay's recording and reads the part before its packets.
    ///
    /// The replay is opened as RecordingReader::open opens it, and its
    /// recording read through that reader.
    ///
    /// @param source where the replay's bytes come from; it must outlive the walk's reads
    /// @return false when the replay or its recording does not read, or the
    ///         part before the packets does not parse (`failure` says which)
    [[nodiscard]] bool open_replay(const ArchiveSource& source);

    /// Returns the part before the packets, parsed: every member but `packets`.
    ///
    /// Its spans view the walk's own copy of that part, which stays while the walk is open.
    ///
    /// @return the part; empty before a recording was opened
    [[nodiscard]] const tad::Demo& head() const noexcept;

    /// Returns the bytes of the part before the packets.
    ///
    /// @return the bytes; empty before a recording was opened
    [[nodiscard]] std::span<const uint8_t> head_bytes() const noexcept;

    /// Returns the replay's reader, for its description, its manifest and its timeline.
    ///
    /// @return the reader; null when a `.tad` was opened, or nothing was
    [[nodiscard]] RecordingReader* replay() noexcept;

    /// Reads the next packet.
    ///
    /// Its payload views the walk's buffer until the next call. Its `offset`
    /// is where its chunk starts in the recording (`offset` gives it whole),
    /// and its `time_ms` the sum of the delays up to and including it.
    ///
    /// @param[out] packet the packet
    /// @param[out] ended true when the recording has ended; a replay's size
    ///             and digest have then been checked
    /// @return false when the recording stopped (`failure` says why)
    [[nodiscard]] bool next(tad::Packet& packet, bool& ended);

    /// Returns why the recording stopped.
    ///
    /// @return the failure; every member empty while the recording reads
    [[nodiscard]] const WalkFailure& failure() const noexcept;

    /// Returns where the last packet `next` gave starts, from the recording's start.
    ///
    /// @return the offset in bytes, whole on every platform
    [[nodiscard]] uint64_t offset() const noexcept;

    /// Returns how many packets have been read.
    ///
    /// @return the packets
    [[nodiscard]] uint64_t packets() const noexcept;

    /// Returns how many of the recording's bytes have been read.
    ///
    /// @return the bytes, the part before the packets included
    [[nodiscard]] uint64_t bytes_read() const noexcept;

  private:

    /// The recording's source, the part before the packets, the buffer and the place.
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace oa::formats::oademo
