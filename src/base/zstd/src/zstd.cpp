// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/zstd/zstd.hpp"

// ZSTD_getCParams, which gives a level's own window, is in zstd's static-linking
// section: the library is linked as these sources, so it is stable here.
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zstd_errors.h"

#include <algorithm>
#include <limits>

namespace oa::base::zstd {
namespace {

Sized failed(Error error) noexcept {
    Sized result;
    result.error = error;
    return result;
}

Sized finished(std::size_t bytes) noexcept {
    Sized result;
    result.bytes = bytes;
    return result;
}

// One data frame and nothing else: a skippable frame holds no data, and a
// second frame or a stray byte is not one frame.
bool exactly_one_frame(std::span<const uint8_t> frame) noexcept {
    if (ZSTD_isSkippableFrame(frame.data(), frame.size()) != 0)
        return false;
    const std::size_t found = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    return ZSTD_isError(found) == 0 && found == frame.size();
}

// A buffer's address for zstd. An empty span has none, and zstd takes a
// pointer even when it will write nothing, so an empty one gets a byte here.
uint8_t* writable(std::span<uint8_t> out, uint8_t& spare) noexcept {
    return out.empty() ? &spare : out.data();
}

} // namespace

int lowest_level() noexcept {
    return ZSTD_minCLevel();
}

int highest_level() noexcept {
    return ZSTD_maxCLevel();
}

std::size_t compress_bound(std::size_t input_bytes) noexcept {
    const std::size_t bound = ZSTD_compressBound(input_bytes);
    return ZSTD_isError(bound) != 0 ? 0 : bound;
}

Sized compress(std::span<const uint8_t> input, int level, std::span<uint8_t> out) {
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        return failed(Error::bad_level);
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr)
        return failed(Error::out_of_memory);
    Error problem = Error::none;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level)) != 0)
        problem = Error::damaged;
    // The level's own window for this input, held to the module's cap. A level
    // that stays inside it is left as zstd sets it, so its frames do not move.
    const ZSTD_compressionParameters wanted = ZSTD_getCParams(level, input.size(), 0);
    if (problem == Error::none && wanted.windowLog > static_cast<unsigned>(most_window_log) &&
        ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, most_window_log)) != 0)
        problem = Error::damaged;
    std::size_t written = 0;
    if (problem == Error::none) {
        // One call ends the frame, so zstd pledges the input's size and the
        // header names it.
        const void* source = input.empty() ? nullptr : static_cast<const void*>(input.data());
        uint8_t spare{};
        written = ZSTD_compress2(context, writable(out, spare), out.size(), source, input.size());
        if (ZSTD_isError(written) != 0)
            problem = ZSTD_getErrorCode(written) == ZSTD_error_dstSize_tooSmall
                          ? Error::buffer_too_small
                          : Error::damaged;
    }
    ZSTD_freeCCtx(context);
    if (problem != Error::none)
        return failed(problem);
    return finished(written);
}

Sized compress_parts(
    std::span<const uint8_t> first,
    std::span<const uint8_t> second,
    int level,
    std::span<uint8_t> out
) {
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        return failed(Error::bad_level);
    if (first.size() > std::numeric_limits<std::size_t>::max() - second.size())
        return failed(Error::damaged);
    const std::size_t total = first.size() + second.size();
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr)
        return failed(Error::out_of_memory);
    Error problem = Error::none;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level)) != 0)
        problem = Error::damaged;
    const ZSTD_compressionParameters wanted = ZSTD_getCParams(level, total, 0);
    if (problem == Error::none && wanted.windowLog > static_cast<unsigned>(most_window_log) &&
        ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, most_window_log)) != 0)
        problem = Error::damaged;
    if (problem == Error::none && ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(
                                      context, static_cast<unsigned long long>(total)
                                  )) != 0)
        problem = Error::damaged;
    uint8_t spare_out{};
    ZSTD_outBuffer output{
        out.empty() ? static_cast<void*>(&spare_out) : static_cast<void*>(out.data()), out.size(), 0
    };
    const auto push = [&](std::span<const uint8_t> part, ZSTD_EndDirective how) {
        if (problem != Error::none)
            return;
        uint8_t spare_in{};
        const void* bytes = part.empty() ? static_cast<const void*>(&spare_in)
                                         : static_cast<const void*>(part.data());
        ZSTD_inBuffer input{bytes, part.size(), 0};
        for (;;) {
            const std::size_t remaining = ZSTD_compressStream2(context, &output, &input, how);
            if (ZSTD_isError(remaining) != 0) {
                problem = ZSTD_getErrorCode(remaining) == ZSTD_error_dstSize_tooSmall
                              ? Error::buffer_too_small
                              : Error::damaged;
                return;
            }
            if (how != ZSTD_e_end) {
                if (input.pos == input.size)
                    return;
                if (output.pos >= output.size) {
                    problem = Error::buffer_too_small;
                    return;
                }
                continue;
            }
            if (remaining == 0)
                return;
            if (output.pos >= output.size) {
                problem = Error::buffer_too_small;
                return;
            }
        }
    };
    if (problem == Error::none) {
        if (!first.empty())
            push(first, ZSTD_e_continue);
        if (!second.empty())
            push(second, ZSTD_e_continue);
        push({}, ZSTD_e_end);
    }
    const std::size_t written = output.pos;
    ZSTD_freeCCtx(context);
    if (problem != Error::none)
        return failed(problem);
    return finished(written);
}

Sized compress_watched(
    std::span<const uint8_t> input, int level, std::span<uint8_t> out, const CompressWatch& watch
) {
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        return failed(Error::bad_level);
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr)
        return failed(Error::out_of_memory);
    Error problem = Error::none;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level)) != 0)
        problem = Error::damaged;
    const ZSTD_compressionParameters wanted = ZSTD_getCParams(level, input.size(), 0);
    if (problem == Error::none && wanted.windowLog > static_cast<unsigned>(most_window_log) &&
        ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, most_window_log)) != 0)
        problem = Error::damaged;
    // The header names the input's size, as `compress` does.
    if (problem == Error::none &&
        ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(context, input.size())) != 0)
        problem = Error::damaged;

    // A piece of the input. Small enough that a stop is noticed before the
    // next one, and large enough that the frame is not a call per byte.
    constexpr std::size_t chunk_bytes = 256U * 1024U;
    uint8_t spare{};
    uint8_t* destination = writable(out, spare);
    std::size_t written = 0;
    std::size_t taken = 0;
    const std::size_t whole = input.size();
    const auto asked_to_stop = [&]() noexcept {
        return watch.stop != nullptr && watch.stop(watch.context);
    };
    if (problem == Error::none && asked_to_stop())
        problem = Error::stopped;

    while (problem == Error::none) {
        const std::size_t piece = taken < whole ? std::min(chunk_bytes, whole - taken) : 0;
        const ZSTD_EndDirective directive = taken + piece == whole ? ZSTD_e_end : ZSTD_e_continue;
        ZSTD_inBuffer incoming{
            piece == 0 ? nullptr : static_cast<const void*>(input.data() + taken), piece, 0
        };
        for (;;) {
            const std::size_t room = out.size() > written ? out.size() - written : 0;
            // An empty input still needs a byte for zstd to write into when
            // the caller passed none. A real frame that does not fit is refused.
            const bool spare_only = out.empty();
            if (!spare_only && room == 0) {
                problem = Error::buffer_too_small;
                break;
            }
            ZSTD_outBuffer outgoing{
                spare_only ? destination : destination + written, spare_only ? 1 : room, 0
            };
            const std::size_t remaining =
                ZSTD_compressStream2(context, &outgoing, &incoming, directive);
            if (ZSTD_isError(remaining) != 0) {
                problem = ZSTD_getErrorCode(remaining) == ZSTD_error_dstSize_tooSmall
                              ? Error::buffer_too_small
                              : Error::damaged;
                break;
            }
            if (!spare_only)
                written += outgoing.pos;
            const bool chunk_taken = incoming.pos == incoming.size;
            if (directive == ZSTD_e_end) {
                if (remaining == 0 && chunk_taken)
                    break;
            } else if (chunk_taken) {
                break;
            }
            if (outgoing.pos == 0 && chunk_taken) {
                problem = Error::damaged;
                break;
            }
        }
        if (problem != Error::none)
            break;
        taken += piece;
        if (watch.report != nullptr)
            watch.report(watch.context, static_cast<uint64_t>(taken), static_cast<uint64_t>(whole));
        if (directive == ZSTD_e_end)
            break;
        if (asked_to_stop()) {
            problem = Error::stopped;
            break;
        }
    }
    ZSTD_freeCCtx(context);
    if (problem != Error::none)
        return failed(problem);
    return finished(written);
}

namespace {

ZSTD_CCtx* compression_of(const FrameWriter& writer) noexcept {
    return static_cast<ZSTD_CCtx*>(writer.context);
}

ZSTD_DCtx* decompression_of(const FrameReader& reader) noexcept {
    return static_cast<ZSTD_DCtx*>(reader.context);
}

// Records a writer's first failure, which every later call returns.
Error writer_failed(FrameWriter& writer, Error error) noexcept {
    if (writer.failed == Error::none)
        writer.failed = error;
    return writer.failed;
}

// Records a reader's first failure, which every later call returns.
Error reader_failed(FrameReader& reader, Error error) noexcept {
    if (reader.failed == Error::none)
        reader.failed = error;
    return reader.failed;
}

// Runs the compressor once over `input` with `directive`, handing what it
// wrote to the sink. Sets `remaining` to what zstd still has to flush.
Error writer_pass(
    FrameWriter& writer, ZSTD_inBuffer& input, ZSTD_EndDirective directive, std::size_t& remaining
) noexcept {
    ZSTD_outBuffer output{writer.room.data(), writer.room.size(), 0};
    remaining = ZSTD_compressStream2(compression_of(writer), &output, &input, directive);
    if (ZSTD_isError(remaining) != 0)
        return writer_failed(writer, Error::damaged);
    if (output.pos != 0) {
        const std::span<const uint8_t> made(writer.room.data(), output.pos);
        if (writer.sink.write == nullptr || !writer.sink.write(writer.sink.context, made))
            return writer_failed(writer, Error::sink_refused);
        writer.written += output.pos;
    }
    return Error::none;
}

} // namespace

Error frame_writer_begin(
    FrameWriter& writer, int level, uint64_t whole, std::span<uint8_t> room, FrameSink sink
) {
    frame_writer_free(writer);
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        return writer_failed(writer, Error::bad_level);
    if (room.empty())
        return writer_failed(writer, Error::buffer_too_small);
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr)
        return writer_failed(writer, Error::out_of_memory);
    writer.context = context;
    writer.room = room;
    writer.sink = sink;
    writer.whole = whole;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level)) != 0)
        return writer_failed(writer, Error::damaged);
    // The level's own window for this input, held to the module's cap, as `compress` sets it.
    const ZSTD_compressionParameters wanted =
        ZSTD_getCParams(level, static_cast<unsigned long long>(whole), 0);
    if (wanted.windowLog > static_cast<unsigned>(most_window_log) &&
        ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, most_window_log)) != 0)
        return writer_failed(writer, Error::damaged);
    // The header names the input's size.
    if (ZSTD_isError(
            ZSTD_CCtx_setPledgedSrcSize(context, static_cast<unsigned long long>(whole))
        ) != 0)
        return writer_failed(writer, Error::damaged);
    return Error::none;
}

Error frame_writer_write(FrameWriter& writer, std::span<const uint8_t> input) {
    if (writer.failed != Error::none)
        return writer.failed;
    if (writer.context == nullptr)
        return writer_failed(writer, Error::damaged);
    if (input.empty())
        return Error::none;
    if (input.size() > writer.whole - writer.taken)
        return writer_failed(writer, Error::damaged);
    ZSTD_inBuffer incoming{input.data(), input.size(), 0};
    // Each pass takes input until zstd's own buffer is full; the room is handed on after each.
    while (incoming.pos < incoming.size) {
        std::size_t remaining = 0;
        if (const Error error = writer_pass(writer, incoming, ZSTD_e_continue, remaining);
            error != Error::none)
            return error;
    }
    writer.taken += input.size();
    return Error::none;
}

Error frame_writer_end(FrameWriter& writer) {
    if (writer.failed != Error::none)
        return writer.failed;
    if (writer.context == nullptr || writer.taken != writer.whole)
        return writer_failed(writer, Error::damaged);
    uint8_t spare{};
    ZSTD_inBuffer none{&spare, 0, 0};
    for (;;) {
        std::size_t remaining = 0;
        if (const Error error = writer_pass(writer, none, ZSTD_e_end, remaining);
            error != Error::none)
            return error;
        if (remaining == 0)
            return Error::none;
    }
}

void frame_writer_free(FrameWriter& writer) noexcept {
    if (writer.context != nullptr)
        ZSTD_freeCCtx(compression_of(writer));
    writer = FrameWriter{};
}

Error frame_reader_begin(FrameReader& reader) {
    frame_reader_free(reader);
    ZSTD_DCtx* context = ZSTD_createDCtx();
    if (context == nullptr)
        return reader_failed(reader, Error::out_of_memory);
    reader.context = context;
    if (ZSTD_isError(ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, most_read_window_log)) !=
        0)
        return reader_failed(reader, Error::damaged);
    return Error::none;
}

Error frame_reader_read(
    FrameReader& reader,
    std::span<const uint8_t>& input,
    std::span<uint8_t> out,
    std::size_t& decoded
) {
    decoded = 0;
    if (reader.failed != Error::none)
        return reader.failed;
    if (reader.context == nullptr)
        return reader_failed(reader, Error::damaged);
    if (reader.ended) {
        // One frame and nothing after it.
        if (!input.empty())
            return reader_failed(reader, Error::damaged);
        return Error::none;
    }
    // A skippable frame holds no data, and zstd would pass over it: it is refused here, as
    // `decompress` refuses it, once its magic number has arrived.
    for (std::size_t at = 0; reader.magic_seen < reader.magic.size() && at < input.size(); ++at) {
        reader.magic[reader.magic_seen] = input[at];
        ++reader.magic_seen;
    }
    if (reader.magic_seen == reader.magic.size() &&
        ZSTD_isSkippableFrame(reader.magic.data(), reader.magic.size()) != 0)
        return reader_failed(reader, Error::damaged);
    uint8_t spare_out{};
    ZSTD_outBuffer output{
        out.empty() ? static_cast<void*>(&spare_out) : static_cast<void*>(out.data()), out.size(), 0
    };
    uint8_t spare_in{};
    ZSTD_inBuffer incoming{
        input.empty() ? static_cast<const void*>(&spare_in)
                      : static_cast<const void*>(input.data()),
        input.size(),
        0
    };
    for (;;) {
        const std::size_t before_in = incoming.pos;
        const std::size_t before_out = output.pos;
        const std::size_t hint =
            ZSTD_decompressStream(decompression_of(reader), &output, &incoming);
        if (ZSTD_isError(hint) != 0) {
            input = input.subspan(incoming.pos);
            decoded = output.pos;
            return reader_failed(reader, Error::damaged);
        }
        if (hint == 0) {
            reader.ended = true;
            break;
        }
        // Room full, input used up, or a pass that moved nothing: the caller brings more.
        if (output.pos == output.size || incoming.pos == incoming.size ||
            (incoming.pos == before_in && output.pos == before_out))
            break;
    }
    input = input.subspan(incoming.pos);
    decoded = output.pos;
    reader.produced += output.pos;
    if (reader.ended && !input.empty())
        return reader_failed(reader, Error::damaged);
    return Error::none;
}

void frame_reader_free(FrameReader& reader) noexcept {
    if (reader.context != nullptr)
        ZSTD_freeDCtx(decompression_of(reader));
    reader = FrameReader{};
}

Sized content_size(std::span<const uint8_t> frame, std::size_t most_bytes) {
    if (frame.empty() || ZSTD_isSkippableFrame(frame.data(), frame.size()) != 0)
        return failed(Error::damaged);
    const unsigned long long declared = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (declared == ZSTD_CONTENTSIZE_ERROR)
        return failed(Error::damaged);
    if (declared == ZSTD_CONTENTSIZE_UNKNOWN)
        return failed(Error::size_unknown);
    if (declared > static_cast<unsigned long long>(most_bytes))
        return failed(Error::too_large);
    return finished(static_cast<std::size_t>(declared));
}

Sized decompress(std::span<const uint8_t> frame, std::span<uint8_t> out) {
    if (frame.empty() || !exactly_one_frame(frame))
        return failed(Error::damaged);
    const unsigned long long declared = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (declared == ZSTD_CONTENTSIZE_ERROR)
        return failed(Error::damaged);
    if (declared != ZSTD_CONTENTSIZE_UNKNOWN &&
        declared > static_cast<unsigned long long>(out.size()))
        return failed(Error::buffer_too_small);
    uint8_t spare{};
    const std::size_t written =
        ZSTD_decompress(writable(out, spare), out.size(), frame.data(), frame.size());
    if (ZSTD_isError(written) != 0)
        return failed(
            ZSTD_getErrorCode(written) == ZSTD_error_dstSize_tooSmall ? Error::buffer_too_small
                                                                      : Error::damaged
        );
    if (declared != ZSTD_CONTENTSIZE_UNKNOWN && written != declared)
        return failed(Error::damaged);
    return finished(written);
}

} // namespace oa::base::zstd
