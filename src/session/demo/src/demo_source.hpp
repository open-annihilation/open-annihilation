// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Where playback reads a recording's packets from, as it reaches them: the
// bytes handed to demo_load, a `.tad` file, or a replay's frame read in
// pieces. Not a public header.
//
// A recording is read twice in order. The load reads every packet once to
// count the frames, take their totals and find where the frames start again
// every few thousand; the delivery reads the packets again as the match's
// ticks reach them, making each frame as it is due. Neither holds more than
// a piece of the recording.

#include "oa/session/demo.hpp"

#include "oa/formats/tad.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::session::demo {

/// A recording's bytes read in order from where the recording is.
class RecordingBytes {
  public:

    RecordingBytes() = default;
    RecordingBytes(const RecordingBytes&) = delete;
    RecordingBytes& operator=(const RecordingBytes&) = delete;
    virtual ~RecordingBytes() = default;

    /// Reads the next bytes.
    ///
    /// @param out room for the bytes
    /// @param[out] got how many were written; 0 at the end
    /// @return empty, or why the bytes cannot be read
    [[nodiscard]] virtual std::string read(std::span<uint8_t> out, std::size_t& got) = 0;

    /// Moves to a byte, from which the next read reads.
    ///
    /// @param position bytes from the recording's start
    /// @return empty, or why it cannot
    [[nodiscard]] virtual std::string seek(uint64_t position) = 0;

    /// Returns where the next read reads from.
    ///
    /// @return bytes from the recording's start
    [[nodiscard]] virtual uint64_t position() const noexcept = 0;
};

/// A recording's packets read in order through one buffer.
class PacketReader {
  public:

    /// Reads packets from a recording's bytes.
    ///
    /// @param bytes the recording's bytes; the reader keeps them
    explicit PacketReader(std::unique_ptr<RecordingBytes> bytes);

    /// Starts at a packet's chunk, with the bytes already read from it on.
    ///
    /// @param offset where the chunk starts, from the recording's start
    /// @param time_ms the recorder's clock before the packet
    /// @param read bytes from `offset` on that were already read; the reader
    ///        reads on from `offset` plus their size
    void start_with(uint64_t offset, uint64_t time_ms, std::span<const uint8_t> read);

    /// Starts at a packet's chunk, reading the recording again from there.
    ///
    /// @param offset where the chunk starts, from the recording's start
    /// @param time_ms the recorder's clock before the packet
    /// @return empty, or why the recording cannot be read from there
    [[nodiscard]] std::string start_at(uint64_t offset, uint64_t time_ms);

    /// Reads the next packet. Its payload views the reader's buffer until the next call.
    ///
    /// @param[out] packet the packet, its file offset and the recorder's clock
    /// @param[out] ended true when no packet is left
    /// @return empty, the refusal the recording's bytes gave, or the text a
    ///         whole parse gives for the chunk (for a chunk that runs past the
    ///         end, or is too short to be a packet), with its offset
    [[nodiscard]] std::string next(formats::tad::Packet& packet, bool& ended);

    /// Returns where the next packet's chunk starts.
    ///
    /// @return bytes from the recording's start
    [[nodiscard]] uint64_t offset() const noexcept { return buffer_offset_ + at_; }

    /// Returns the recorder's clock after the last packet read.
    ///
    /// @return milliseconds
    [[nodiscard]] uint64_t time_ms() const noexcept { return time_ms_; }

    /// Reads the recording's bytes to their end, without reading packets.
    ///
    /// @return empty, or the refusal the bytes gave at their end (a replay's
    ///         digest that is not its description's)
    [[nodiscard]] std::string drain();

  private:

    /// Reads on until `count` bytes are buffered from `at_`, or the bytes end.
    ///
    /// @param count at most the buffer's size
    /// @return empty, or the refusal the recording's bytes gave
    [[nodiscard]] std::string fill(std::size_t count);

    std::unique_ptr<RecordingBytes> bytes_{};
    std::vector<uint8_t> buffer_{};
    std::size_t at_{};
    std::size_t end_{};
    uint64_t buffer_offset_{}; ///< recording offset of buffer_[0]
    uint64_t time_ms_{};
    bool bytes_ended_{};
};

/// What carries from one packet to the next as frames are made from them.
struct FrameMaking {
    /// The tick the next frame without a unit state is due at: the one before it's.
    uint32_t due_tick{};
    /// Each recorded player number's next broadcast frame sequence.
    std::array<int32_t, sender_count> sequence{};
};

/// A place in the recording where frames can be made again from: before the
/// packet that makes frame `frames`.
struct SeekPoint {
    /// Frames made before this place.
    uint64_t frames{};
    /// Where the packet's chunk starts, from the recording's start.
    uint64_t offset{};
    /// The recorder's clock before the packet, in milliseconds.
    uint64_t time_ms{};
    /// The due tick that carries into the packet.
    uint32_t due_tick{};
    /// The next sequence of each recorded player, in the file's player order.
    std::array<int32_t, formats::tad::limit::players> sequence{};
};

/// Frames between two seek points.
inline constexpr uint64_t seek_point_frames = 4096;

/// Where a playback's recording is, and the delivery's place in it.
struct PlaybackSource {
    /// Where the recording's bytes are.
    enum class Origin : uint8_t {
        held_tad,    ///< `held` is the recording
        file_tad,    ///< `path` is the recording
        held_replay, ///< `held` is a `.oademo`
        file_replay, ///< `path` is a `.oademo`
    };
    /// Where the recording's bytes are.
    Origin origin{Origin::held_tad};
    /// The bytes handed to demo_load, shared with every reading of them.
    std::shared_ptr<const std::vector<uint8_t>> held{};
    /// The file demo_open named.
    std::filesystem::path path{};
    /// Where the first packet's chunk starts, from the recording's start.
    uint64_t packets_at{};
    /// Places frames can be made again from, by frame.
    std::vector<SeekPoint> seek_points{};
    /// The delivery's reader; null until the first frame is asked for.
    std::unique_ptr<PacketReader> reader{};
    /// What carries into the delivery's next packet.
    FrameMaking making{};
    /// The index of the frame the delivery makes next.
    uint64_t next_frame{};
    /// The frame made and not yet taken, with its index.
    std::optional<DemoFrame> pending{};
    /// Why the delivery stopped reading the recording; empty while it reads.
    std::string failure{};
    /// True once the session was told of `failure`.
    bool failure_told{};

    /// Opens the recording's bytes from its start.
    ///
    /// @param[out] bytes the recording's bytes
    /// @return empty, or why they cannot be read
    [[nodiscard]] std::string open_bytes(std::unique_ptr<RecordingBytes>& bytes) const;
};

} // namespace oa::session::demo
