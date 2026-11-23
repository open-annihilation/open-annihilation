// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oademo/recording_walk.hpp"

#include "oa/formats/oademo/interface_words.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oademo {

namespace {

/// The bytes a walk holds of the packets: room for every chunk, which a 16-bit length sizes.
constexpr std::size_t packet_buffer_bytes = std::size_t{1} << 20;

/// The first bytes read for the part before the packets; doubled until that part is whole.
constexpr std::size_t first_head_bytes = std::size_t{64} << 10;

uint16_t load16(const uint8_t* at) noexcept {
    return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

/// Finds where a recording's packets start, walking its chunks as tad::parse
/// reads them: the header, the sector count, the sectors, the players, their
/// statuses and the unit checks.
///
/// @param bytes the recording's first bytes
/// @return the offset of the first packet's chunk; 0 when the chunks before it
///         are not all in `bytes`, or their counts are past the format's limits
std::size_t packets_start(std::span<const uint8_t> bytes) {
    namespace layout = tad::layout;
    std::size_t at = 0;
    // The length of the whole chunk at a position; 0 when it is not all there.
    const auto chunk_at = [&bytes](std::size_t position) -> std::size_t {
        if (bytes.size() - position < layout::chunk_length_bytes)
            return 0;
        const std::size_t length = load16(bytes.data() + position);
        if (length < layout::chunk_length_bytes || length > bytes.size() - position)
            return 0;
        return length;
    };
    const std::size_t header = chunk_at(at);
    if (header < layout::chunk_length_bytes + layout::header_fixed_bytes)
        return 0;
    const std::size_t player_count =
        bytes[at + layout::chunk_length_bytes + layout::magic_bytes + 2];
    if (player_count > tad::limit::players)
        return 0;
    at += header;
    const std::size_t counted = chunk_at(at);
    if (counted != layout::chunk_length_bytes + layout::sector_count_bytes)
        return 0;
    const std::size_t sectors = static_cast<std::size_t>(load16(bytes.data() + at + 2)) |
                                (static_cast<std::size_t>(load16(bytes.data() + at + 4)) << 16);
    if (sectors > tad::limit::sectors)
        return 0;
    at += counted;
    for (std::size_t index = 0; index < sectors + player_count * 2 + 1; ++index) {
        const std::size_t length = chunk_at(at);
        if (length == 0)
            return 0;
        at += length;
    }
    return at;
}

tad::Error packet_error(tad::ErrorCode code, uint64_t offset, std::string_view message) {
    tad::Error error;
    error.code = code;
    error.offset = static_cast<std::size_t>(offset);
    error.message = std::string(message);
    return error;
}

} // namespace

struct RecordingWalk::State {
    /// A `.tad`'s source; unused for a replay.
    RecordingSource tad_source{};
    /// A replay's reader; null for a `.tad`.
    std::unique_ptr<RecordingReader> reader{};
    /// The part before the packets, which `demo`'s spans view.
    std::vector<uint8_t> head{};
    /// That part, parsed.
    tad::Demo demo{};
    /// Packet bytes read and not yet walked, from `at` to `end`.
    std::vector<uint8_t> buffer{};
    std::size_t at{};
    std::size_t end{};
    /// The recording's offset of `buffer[0]`.
    uint64_t buffer_offset{};
    /// True once the source gave no more bytes.
    bool source_ended{};
    /// The sum of the delays so far.
    uint64_t time_ms{};
    /// Packets walked.
    uint64_t packets{};
    /// Where the last packet starts.
    uint64_t last_offset{};
    /// Why the walk stopped.
    WalkFailure failure{};
    /// True once the walk stopped.
    bool failed{};

    /// Reads the recording's next bytes, filling `into` unless it ends first.
    ///
    /// @param into room for the bytes
    /// @param[out] got bytes written to `into`
    /// @return false when the source refused; `failure` says how
    bool pull(std::span<uint8_t> into, std::size_t& got) {
        got = 0;
        while (got < into.size()) {
            std::size_t piece = 0;
            if (reader != nullptr) {
                if (std::string why_not = reader->read(into.subspan(got), piece);
                    !why_not.empty()) {
                    failure.sentence = std::move(why_not);
                    failed = true;
                    return false;
                }
            } else if (
                tad_source.read == nullptr ||
                !tad_source.read(tad_source.context, into.subspan(got), piece)
            ) {
                failure.unreadable = true;
                failed = true;
                return false;
            }
            if (piece == 0)
                break;
            got += piece;
        }
        return true;
    }

    /// Reads a replay's recording to its end, so a replay whose bytes do not
    /// read says so first, as a whole read of it does.
    void drain_replay() {
        if (reader == nullptr)
            return;
        std::vector<uint8_t> room(packet_buffer_bytes);
        for (;;) {
            std::size_t got = 0;
            if (std::string why_not = reader->read(room, got); !why_not.empty()) {
                failure.sentence = std::move(why_not);
                return;
            }
            if (got == 0)
                return;
        }
    }

    /// Stops the walk on a parse error.
    ///
    /// @param error what tad::parse says
    /// @return false
    bool refuse(tad::Error error) {
        failure.parse = std::move(error);
        failed = true;
        drain_replay();
        return false;
    }

    /// Reads on until `count` bytes are buffered from `at`, or the recording ends.
    ///
    /// @param count at most the buffer's size
    /// @return false when the source refused
    bool fill(std::size_t count) {
        if (end - at >= count || source_ended)
            return true;
        if (at != 0) {
            std::memmove(buffer.data(), buffer.data() + at, end - at);
            buffer_offset += at;
            end -= at;
            at = 0;
        }
        while (end < count && !source_ended) {
            std::size_t got = 0;
            if (!pull(std::span(buffer).subspan(end), got))
                return false;
            if (got < buffer.size() - end)
                source_ended = true;
            end += got;
        }
        return true;
    }

    /// Reads and parses the part before the packets, and keeps what follows it.
    ///
    /// @return false when it cannot be read or does not parse
    bool open_head() {
        std::vector<uint8_t> first;
        std::size_t packets_at = 0;
        bool ended = false;
        for (std::size_t want = first_head_bytes; packets_at == 0 && !ended;
             want = std::min(want * 2, most_head_bytes)) {
            const std::size_t have = first.size();
            first.resize(want);
            std::size_t got = 0;
            if (!pull(std::span(first).subspan(have), got))
                return false;
            first.resize(have + got);
            ended = have + got < want;
            packets_at = packets_start(first);
            if (want == most_head_bytes)
                break;
        }
        if (packets_at == 0) {
            const tad::ParseResult parsed = tad::parse(first);
            if (parsed.ok())
                return refuse(packet_error(
                    tad::ErrorCode::malformed, 0, "the part before the packets does not read"
                ));
            return refuse(*parsed.error);
        }
        head.assign(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(packets_at));
        tad::ParseResult parsed = tad::parse(head);
        if (!parsed.ok())
            return refuse(*parsed.error);
        demo = std::move(*parsed.demo);
        buffer.assign(std::max(packet_buffer_bytes, first.size() - packets_at), 0);
        std::copy(
            first.begin() + static_cast<std::ptrdiff_t>(packets_at), first.end(), buffer.begin()
        );
        at = 0;
        end = first.size() - packets_at;
        buffer_offset = packets_at;
        source_ended = ended;
        return true;
    }
};

RecordingWalk::RecordingWalk() = default;

RecordingWalk::RecordingWalk(RecordingWalk&& other) noexcept = default;

RecordingWalk& RecordingWalk::operator=(RecordingWalk&& other) noexcept = default;

RecordingWalk::~RecordingWalk() = default;

bool RecordingWalk::open_tad(const RecordingSource& source) {
    state_ = std::make_unique<State>();
    state_->tad_source = source;
    return state_->open_head();
}

bool RecordingWalk::open_replay(const ArchiveSource& source) {
    state_ = std::make_unique<State>();
    State& state = *state_;
    state.reader = std::make_unique<RecordingReader>();
    if (std::string why_not = state.reader->open(source); !why_not.empty()) {
        state.failure.sentence = std::move(why_not);
        state.failed = true;
        return false;
    }
    return state.open_head();
}

const tad::Demo& RecordingWalk::head() const noexcept {
    static const tad::Demo none{};
    return state_ != nullptr ? state_->demo : none;
}

std::span<const uint8_t> RecordingWalk::head_bytes() const noexcept {
    if (state_ == nullptr)
        return {};
    return state_->head;
}

RecordingReader* RecordingWalk::replay() noexcept {
    return state_ != nullptr ? state_->reader.get() : nullptr;
}

bool RecordingWalk::next(tad::Packet& packet, bool& ended) {
    ended = false;
    if (state_ == nullptr || state_->failed)
        return false;
    State& state = *state_;
    // The checks and the texts tad::parse makes of a packet's chunk.
    if (!state.fill(tad::layout::chunk_length_bytes))
        return false;
    const uint64_t at = state.buffer_offset + state.at;
    if (state.end == state.at) {
        ended = true;
        return true;
    }
    if (state.end - state.at < tad::layout::chunk_length_bytes)
        return state.refuse(
            packet_error(tad::ErrorCode::truncated, at, "packet: missing chunk length")
        );
    const std::size_t length = load16(state.buffer.data() + state.at);
    if (length < tad::layout::chunk_length_bytes)
        return state.refuse(
            packet_error(tad::ErrorCode::malformed, at, "packet: chunk length below 2")
        );
    if (!state.fill(length))
        return false;
    if (state.end - state.at < length)
        return state.refuse(
            packet_error(tad::ErrorCode::truncated, at, "packet: chunk runs past end of file")
        );
    const std::span<const uint8_t> data(
        state.buffer.data() + state.at + tad::layout::chunk_length_bytes,
        length - tad::layout::chunk_length_bytes
    );
    if (data.size() < tad::layout::packet_fixed_bytes)
        return state.refuse(
            packet_error(tad::ErrorCode::malformed, at, "packet chunk shorter than 3 bytes")
        );
    const uint16_t delay = load16(data.data());
    state.time_ms += delay;
    packet = tad::Packet{
        static_cast<std::size_t>(at),
        delay,
        state.time_ms,
        data[2],
        data.subspan(tad::layout::packet_fixed_bytes)
    };
    state.at += length;
    state.last_offset = at;
    ++state.packets;
    return true;
}

const WalkFailure& RecordingWalk::failure() const noexcept {
    static const WalkFailure none{};
    return state_ != nullptr ? state_->failure : none;
}

uint64_t RecordingWalk::offset() const noexcept {
    return state_ != nullptr ? state_->last_offset : 0;
}

uint64_t RecordingWalk::packets() const noexcept {
    return state_ != nullptr ? state_->packets : 0;
}

uint64_t RecordingWalk::bytes_read() const noexcept {
    return state_ != nullptr ? state_->buffer_offset + state_->at : 0;
}

} // namespace oa::formats::oademo
