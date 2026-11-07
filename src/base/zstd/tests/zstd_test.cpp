// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Round trips at the levels a replay uses, a damaged frame (junk, and a real
// frame cut short), a frame whose header names more bytes than the call
// allows (refused from the header, before any buffer exists), a frame with
// no size in its header, and the window every frame this module writes
// declares.

// ZSTD_getFrameHeader, which reads the window a frame declares, is in zstd's
// static-linking section.
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#include "oa/base/zstd/zstd.hpp"

#include "oa/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using oa::base::zstd::Error;
using oa::base::zstd::Sized;

std::vector<uint8_t> sample() {
    const std::string text(4000, 'a');
    std::vector<uint8_t> bytes(text.begin(), text.end());
    bytes.insert(bytes.end(), {1, 2, 3, 4, 5, 9, 8, 7});
    return bytes;
}

// The frame of `bytes` at `level`, sized by the module's own bound.
std::vector<uint8_t> frame_of(const std::vector<uint8_t>& bytes, int level) {
    std::vector<uint8_t> frame(oa::base::zstd::compress_bound(bytes.size()));
    OA_CHECK(!frame.empty());
    const Sized written = oa::base::zstd::compress(bytes, level, frame);
    OA_CHECK(written.ok());
    frame.resize(written.bytes);
    return frame;
}

void round_trips_at(int level) {
    const std::vector<uint8_t> bytes = sample();
    const std::vector<uint8_t> frame = frame_of(bytes, level);
    OA_CHECK(frame.size() < bytes.size());
    // The header names the size, so a reader sizes its buffer from the frame.
    const Sized named = oa::base::zstd::content_size(frame, bytes.size());
    OA_CHECK(named.ok());
    OA_CHECK(named.bytes == bytes.size());
    std::vector<uint8_t> back(named.bytes);
    const Sized read = oa::base::zstd::decompress(frame, back);
    OA_CHECK(read.ok());
    OA_CHECK(read.bytes == bytes.size());
    OA_CHECK(back == bytes);
    // The limit one byte under the size refuses the frame.
    const Sized short_limit = oa::base::zstd::content_size(frame, bytes.size() - 1);
    OA_CHECK(short_limit.error == Error::too_large);
    OA_CHECK(short_limit.bytes == 0);
    // The same input and level give the same frame.
    OA_CHECK(frame_of(bytes, level) == frame);
}

void empty_round_trip() {
    const std::vector<uint8_t> frame = frame_of({}, 3);
    const Sized named = oa::base::zstd::content_size(frame, 0);
    OA_CHECK(named.ok());
    OA_CHECK(named.bytes == 0);
    const Sized read = oa::base::zstd::decompress(frame, {});
    OA_CHECK(read.ok());
    OA_CHECK(read.bytes == 0);
}

void damaged_frame_is_refused() {
    const std::vector<uint8_t> junk{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> room(64);
    const Sized damaged = oa::base::zstd::decompress(junk, room);
    OA_CHECK(damaged.error == Error::damaged);
    OA_CHECK(damaged.bytes == 0);
    OA_CHECK(oa::base::zstd::content_size(junk, 64).error == Error::damaged);
    const Sized empty = oa::base::zstd::decompress({}, room);
    OA_CHECK(empty.error == Error::damaged);
    OA_CHECK(oa::base::zstd::content_size({}, 64).error == Error::damaged);
}

void truncated_real_frame_is_refused() {
    const std::vector<uint8_t> bytes = sample();
    const std::vector<uint8_t> frame = frame_of(bytes, 3);
    OA_CHECK(frame.size() > 16);
    std::vector<uint8_t> room(bytes.size());
    // A frame cut anywhere short of its end keeps a header that reads, and is
    // still not one whole frame.
    for (const std::size_t cut : {std::size_t{1}, std::size_t{4}, frame.size() / 2}) {
        const std::vector<uint8_t> shorter(
            frame.begin(), frame.end() - static_cast<std::ptrdiff_t>(cut)
        );
        OA_CHECK(oa::base::zstd::content_size(shorter, bytes.size()).ok());
        const Sized refused = oa::base::zstd::decompress(shorter, room);
        OA_CHECK(refused.error == Error::damaged);
    }
    // Cut inside the first block, just past the header.
    const std::vector<uint8_t> into_block(frame.begin(), frame.begin() + 9);
    OA_CHECK(oa::base::zstd::decompress(into_block, room).error == Error::damaged);
    // A byte after the frame is not one frame either.
    std::vector<uint8_t> longer = frame;
    longer.push_back(0);
    OA_CHECK(oa::base::zstd::decompress(longer, room).error == Error::damaged);
}

void a_skippable_frame_is_refused() {
    // The skippable magic, a length of zero, and nothing to skip.
    const std::vector<uint8_t> skippable{0x50, 0x2A, 0x4D, 0x18, 0x00, 0x00, 0x00, 0x00};
    std::vector<uint8_t> room(64);
    OA_CHECK(oa::base::zstd::content_size(skippable, 64).error == Error::damaged);
    OA_CHECK(oa::base::zstd::decompress(skippable, room).error == Error::damaged);
}

void declared_size_above_the_limit_is_refused_before_allocation() {
    // Magic, a single-segment header with an 8-byte content size, then 1 GiB.
    // Thirteen bytes are the whole header. The size is read from them alone,
    // and refused against a limit of 64 bytes, with no buffer made anywhere.
    const std::vector<uint8_t> frame{
        0x28, 0xB5, 0x2F, 0xFD, 0xE0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00
    };
    const Sized refused = oa::base::zstd::content_size(frame, 64);
    OA_CHECK(refused.error == Error::too_large);
    OA_CHECK(refused.bytes == 0);
    const Sized allowed = oa::base::zstd::content_size(frame, std::size_t{1} << 30);
    OA_CHECK(allowed.ok());
    OA_CHECK(allowed.bytes == std::size_t{1} << 30);
}

void a_buffer_too_small_is_refused() {
    const std::vector<uint8_t> bytes = sample();
    const std::vector<uint8_t> frame = frame_of(bytes, 3);
    std::vector<uint8_t> small(bytes.size() - 1);
    OA_CHECK(oa::base::zstd::decompress(frame, small).error == Error::buffer_too_small);
    std::vector<uint8_t> tiny(2);
    OA_CHECK(oa::base::zstd::compress(bytes, 3, tiny).error == Error::buffer_too_small);
}

void two_parts_decompress_to_their_concatenation() {
    const std::vector<uint8_t> first{'a', 'b', 'c'};
    const std::vector<uint8_t> second{'d', 'e'};
    const std::size_t total = first.size() + second.size();
    std::vector<uint8_t> room(oa::base::zstd::compress_bound(total));
    const Sized written = oa::base::zstd::compress_parts(first, second, 1, room);
    OA_CHECK(written.ok());
    room.resize(written.bytes);
    const Sized named = oa::base::zstd::content_size(room, total);
    OA_CHECK(named.ok());
    OA_CHECK(named.bytes == total);
    std::vector<uint8_t> back(total);
    const Sized read = oa::base::zstd::decompress(room, back);
    OA_CHECK(read.ok());
    OA_CHECK(back == std::vector<uint8_t>({'a', 'b', 'c', 'd', 'e'}));

    // An empty span is skipped, and two empty spans are still one frame.
    std::vector<uint8_t> one_room(oa::base::zstd::compress_bound(second.size()));
    const Sized one = oa::base::zstd::compress_parts({}, second, 1, one_room);
    OA_CHECK(one.ok());
    one_room.resize(one.bytes);
    std::vector<uint8_t> one_back(second.size());
    OA_CHECK(oa::base::zstd::decompress(one_room, one_back).ok());
    OA_CHECK(one_back == second);

    std::vector<uint8_t> empty_room(oa::base::zstd::compress_bound(0));
    const Sized empty = oa::base::zstd::compress_parts({}, {}, 1, empty_room);
    OA_CHECK(empty.ok());
    empty_room.resize(empty.bytes);
    std::vector<uint8_t> none(4);
    const Sized none_read = oa::base::zstd::decompress(empty_room, none);
    OA_CHECK(none_read.ok());
    OA_CHECK(none_read.bytes == 0);

    std::vector<uint8_t> tiny(1);
    OA_CHECK(
        oa::base::zstd::compress_parts(first, second, 1, tiny).error == Error::buffer_too_small
    );
}

void a_bad_level_is_refused() {
    const std::vector<uint8_t> bytes = sample();
    std::vector<uint8_t> room(oa::base::zstd::compress_bound(bytes.size()));
    const int above = oa::base::zstd::highest_level() + 1;
    OA_CHECK(oa::base::zstd::compress(bytes, above, room).error == Error::bad_level);
    const int below = oa::base::zstd::lowest_level() - 1;
    OA_CHECK(oa::base::zstd::compress(bytes, below, room).error == Error::bad_level);
}

void two_frames_are_refused() {
    const std::vector<uint8_t> bytes{1, 2, 3, 4};
    const std::vector<uint8_t> first = frame_of(bytes, 1);
    std::vector<uint8_t> both = first;
    both.insert(both.end(), first.begin(), first.end());
    std::vector<uint8_t> room(64);
    OA_CHECK(oa::base::zstd::decompress(both, room).error == Error::damaged);
}

void a_frame_with_no_size_is_read_until_it_ends() {
    // Magic, a header with a window byte and no content size, then one raw
    // block of five bytes that ends the frame.
    const std::vector<uint8_t> frame{
        0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x00, 0x29, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'
    };
    OA_CHECK(oa::base::zstd::content_size(frame, 64).error == Error::size_unknown);
    std::vector<uint8_t> room(64);
    const Sized read = oa::base::zstd::decompress(frame, room);
    OA_CHECK(read.ok());
    OA_CHECK(read.bytes == 5);
    OA_CHECK(room[0] == 'h' && room[4] == 'o');
    // A buffer that cannot hold it is refused, not cut.
    std::vector<uint8_t> short_room(4);
    OA_CHECK(oa::base::zstd::decompress(frame, short_room).error == Error::buffer_too_small);
}

// The window a frame declares, from its header.
unsigned long long window_of(const std::vector<uint8_t>& frame) {
    ZSTD_FrameHeader header{};
    const std::size_t needed = ZSTD_getFrameHeader(&header, frame.data(), frame.size());
    OA_CHECK(ZSTD_isError(needed) == 0);
    OA_CHECK(needed == 0);
    return header.windowSize;
}

void every_frame_declares_a_window_within_the_cap() {
    constexpr unsigned long long cap = 1ULL << oa::base::zstd::most_window_log;
    OA_CHECK(cap == 8ULL * 1024ULL * 1024ULL);
    // A little over 8 MiB: sixty-four KiB of noise, repeated, so level 22 has
    // long matches to take and the input is past what an 8 MiB window holds.
    std::vector<uint8_t> noise(65536);
    uint32_t state = 0x2545F491U;
    for (uint8_t& byte : noise) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<uint8_t>(state >> 8U);
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(cap + noise.size());
    while (bytes.size() <= cap)
        bytes.insert(bytes.end(), noise.begin(), noise.end());
    OA_CHECK(bytes.size() > cap);
    for (const int level : {19, 22}) {
        const std::vector<uint8_t> frame = frame_of(bytes, level);
        OA_CHECK(window_of(frame) <= cap);
        // It is not a smaller window by luck: the input does not fit it, and
        // the frame still reads back whole.
        const Sized named = oa::base::zstd::content_size(frame, bytes.size());
        OA_CHECK(named.ok());
        OA_CHECK(named.bytes == bytes.size());
        std::vector<uint8_t> back(named.bytes);
        OA_CHECK(oa::base::zstd::decompress(frame, back).ok());
        OA_CHECK(back == bytes);
    }
}

struct WatchCounts {
    uint64_t last_taken{};
    uint64_t whole{};
    int reports{};
    bool stop{};
};

void report_taken(void* context, uint64_t taken, uint64_t whole) {
    auto& counts = *static_cast<WatchCounts*>(context);
    OA_CHECK(taken >= counts.last_taken);
    OA_CHECK(taken <= whole);
    counts.last_taken = taken;
    counts.whole = whole;
    ++counts.reports;
}

bool stop_now(void* context) {
    return static_cast<WatchCounts*>(context)->stop;
}

void a_watched_frame_reads_back() {
    // Past one chunk, so the frame is written in pieces and the watch is told
    // more than once. Two runs of the same bytes match each other.
    std::vector<uint8_t> bytes(300U * 1024U);
    uint32_t state = 0xA5A5F00DU;
    for (uint8_t& byte : bytes) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<uint8_t>(state);
    }
    oa::base::zstd::CompressWatch watch;
    WatchCounts counts;
    watch.report = report_taken;
    watch.context = &counts;
    std::vector<uint8_t> frame(oa::base::zstd::compress_bound(bytes.size()));
    const Sized written = oa::base::zstd::compress_watched(bytes, 1, frame, watch);
    OA_CHECK(written.ok());
    OA_CHECK(counts.reports > 1);
    OA_CHECK(counts.last_taken == bytes.size());
    OA_CHECK(counts.whole == bytes.size());
    frame.resize(written.bytes);
    const Sized named = oa::base::zstd::content_size(frame, bytes.size());
    OA_CHECK(named.ok());
    OA_CHECK(named.bytes == bytes.size());
    std::vector<uint8_t> back(named.bytes);
    OA_CHECK(oa::base::zstd::decompress(frame, back).ok());
    OA_CHECK(back == bytes);

    WatchCounts again_counts;
    watch.context = &again_counts;
    std::vector<uint8_t> again(oa::base::zstd::compress_bound(bytes.size()));
    const Sized second = oa::base::zstd::compress_watched(bytes, 1, again, watch);
    OA_CHECK(second.ok());
    again.resize(second.bytes);
    OA_CHECK(again == frame);

    // One shot is left as it was: a watched frame need not match it.
    const std::vector<uint8_t> once = frame_of(bytes, 1);
    OA_CHECK(oa::base::zstd::decompress(once, back).ok());
    OA_CHECK(back == bytes);
}

void a_watched_compression_can_be_stopped() {
    const std::vector<uint8_t> bytes = sample();
    oa::base::zstd::CompressWatch watch;
    WatchCounts counts;
    counts.stop = true;
    watch.stop = stop_now;
    watch.context = &counts;
    std::vector<uint8_t> frame(oa::base::zstd::compress_bound(bytes.size()));
    const Sized written = oa::base::zstd::compress_watched(bytes, 1, frame, watch);
    OA_CHECK(written.error == Error::stopped);
    OA_CHECK(counts.reports == 0);
}

} // namespace

int main() {
    round_trips_at(1);
    round_trips_at(3);
    round_trips_at(19);
    empty_round_trip();
    damaged_frame_is_refused();
    truncated_real_frame_is_refused();
    a_skippable_frame_is_refused();
    declared_size_above_the_limit_is_refused_before_allocation();
    a_buffer_too_small_is_refused();
    two_parts_decompress_to_their_concatenation();
    a_bad_level_is_refused();
    two_frames_are_refused();
    a_frame_with_no_size_is_read_until_it_ends();
    every_frame_declares_a_window_within_the_cap();
    a_watched_frame_reads_back();
    a_watched_compression_can_be_stopped();
    return oa::test::check_exit_status();
}
