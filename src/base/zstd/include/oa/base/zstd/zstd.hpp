// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// One zstd frame, into and out of buffers the caller owns, whole or in
// pieces. The base tier holds no heap container, so nothing here allocates
// the bytes it returns: the caller asks for a size, makes room for it, and
// passes the room in.
// Compression pledges the input's size, so a frame this module writes names
// that size in its header, and never declares a window above
// `most_window_log`. Decompression reads the size a frame names, refusing one
// past the caller's limit, before the caller allocates anything.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::base::zstd {

/// The most bits of window a frame written here declares: 8 MiB.
///
/// Every level of zstd up to 19 stays inside it on its own. Levels 20 to 22
/// would ask for a window of 64 or 128 MiB on an input past 8 MiB, and are
/// held to this one, so a reader that refuses a larger window reads every
/// frame this module writes.
inline constexpr int most_window_log = 23;

/// Why a frame was not compressed or decompressed.
enum class Error : uint8_t {
    none,             ///< the call finished
    bad_level,        ///< the level is outside the range zstd accepts
    damaged,          ///< the bytes are not one whole frame
    too_large,        ///< the frame declares more than the limit
    size_unknown,     ///< the frame's header names no size
    buffer_too_small, ///< the buffer cannot hold the result
    out_of_memory,    ///< zstd could not allocate its work area
    stopped,          ///< a watched compression was asked to stop before the frame ended
    sink_refused,     ///< the sink of a frame written in pieces refused its bytes
};

/// A byte count, or why there is none.
struct Sized {
    /// Bytes written, or bytes a frame will expand to. Zero when `error` is set.
    std::size_t bytes{};
    /// Why the call failed. `none` when it finished.
    Error error{Error::none};

    /// Tells whether the call finished.
    ///
    /// @return true when `error` is `none`
    [[nodiscard]] bool ok() const noexcept { return error == Error::none; }
};

/// Returns the lowest compression level zstd accepts.
///
/// @return the lowest level, which may be negative
[[nodiscard]] int lowest_level() noexcept;

/// Returns the highest compression level zstd accepts.
///
/// @return the highest level
[[nodiscard]] int highest_level() noexcept;

/// Returns the most bytes `compress` can write for an input of a size.
///
/// @param input_bytes the size of the input
/// @return room that always holds the frame; zero when the size is more than
///         zstd takes
[[nodiscard]] std::size_t compress_bound(std::size_t input_bytes) noexcept;

/// Compresses bytes as one zstd frame at `level`.
///
/// The frame's header names the size of `input`. Its window is at most
/// `most_window_log` bits, whatever the level. The same bytes and level give
/// the same frame. The contents of `out` past the bytes written are
/// unspecified, and the whole of it is after an error.
///
/// @param input the bytes to compress, of any size
/// @param level a level from `lowest_level` to `highest_level`
/// @param[out] out room for the frame; `compress_bound(input.size())` bytes always hold it
/// @return the frame's size, or `bad_level`, `buffer_too_small`, `out_of_memory` or `damaged`
[[nodiscard]] Sized compress(std::span<const uint8_t> input, int level, std::span<uint8_t> out);

/// Compresses two spans as one zstd frame at `level`, without joining them first.
///
/// The frame's header names the sum of the two sizes, and `decompress` reads
/// it back as their concatenation. Its window is at most `most_window_log`
/// bits, as `compress` writes. An empty span is not copied. A sum that does
/// not fit in `size_t` is refused. The contents of `out` past the bytes
/// written are unspecified, and the whole of it is after an error.
///
/// @param first the first span, of any size
/// @param second the second span, of any size
/// @param level a level from `lowest_level` to `highest_level`
/// @param[out] out room for the frame; `compress_bound` of the sum always holds it
/// @return the frame's size, or `bad_level`, `buffer_too_small`, `out_of_memory` or `damaged`
[[nodiscard]] Sized compress_parts(
    std::span<const uint8_t> first,
    std::span<const uint8_t> second,
    int level,
    std::span<uint8_t> out
);

/// What a caller watches while `compress_watched` writes a frame.
///
/// The callbacks run on the thread that compresses, after a chunk of the
/// input has been taken and before the next one. Either may be null.
struct CompressWatch {
    /// Told how many input bytes have been taken, of `whole`.
    void (*report)(void* context, uint64_t taken, uint64_t whole){};
    /// Asked before the next chunk. True stops the frame before it ends.
    bool (*stop)(void* context){};
    /// Passed to `report` and `stop`.
    void* context{};
};

/// Compresses bytes as one zstd frame at `level`, reporting as it goes.
///
/// The frame is built the same way as `compress`: the header names the
/// input's size and the window is at most `most_window_log` bits. It is
/// written in chunks, so the bytes need not match `compress` of the same
/// input. `watch.stop` is asked before the first chunk and before each one
/// after. When it says to stop before the frame has ended, nothing of `out`
/// is a frame and the result is `stopped`.
///
/// @param input the bytes to compress, of any size
/// @param level a level from `lowest_level` to `highest_level`
/// @param[out] out room for the frame; `compress_bound(input.size())` bytes always hold it
/// @param watch told as the input is taken, and asked whether to stop
/// @return the frame's size, or `bad_level`, `buffer_too_small`, `out_of_memory`,
///         `damaged` or `stopped`
[[nodiscard]] Sized compress_watched(
    std::span<const uint8_t> input, int level, std::span<uint8_t> out, const CompressWatch& watch
);

/// The most bits of window a frame read in pieces may declare: 128 MiB.
///
/// zstd's own bound for a frame read in pieces. A reader in pieces keeps a
/// window of the frame's declared size, or of its content when that is
/// smaller, so this bounds what a damaged or hostile frame can make it hold.
/// Every frame this module writes declares at most `most_window_log`.
inline constexpr int most_read_window_log = 27;

/// Where the bytes of a frame written in pieces go, in order.
struct FrameSink {
    /// Passed to `write`.
    void* context{};
    /// Takes the frame's next bytes. False stops the frame with `sink_refused`.
    /// Null takes nothing, so every frame stops at its first bytes.
    bool (*write)(void* context, std::span<const uint8_t> bytes){};
};

/// One zstd frame written from input that arrives in pieces.
///
/// The frame's size is given when it begins, and its header names it. The
/// window is at most `most_window_log` bits, as `compress` writes. The bytes
/// are the same however the input is cut into pieces, so two writers given
/// the same input at the same level write the same frame; they need not be
/// the bytes `compress` writes for that input in one piece. A writer holds
/// zstd's work area and the room it was given, and nothing that grows with
/// the input. Begin it with `frame_writer_begin` and free it with
/// `frame_writer_free`.
struct FrameWriter {
    /// zstd's compression context; null before `frame_writer_begin`.
    void* context{};
    /// Where the frame's bytes gather before the sink takes them.
    std::span<uint8_t> room{};
    /// Takes the frame's bytes.
    FrameSink sink{};
    /// The input's size, as begun.
    uint64_t whole{};
    /// Input bytes taken so far.
    uint64_t taken{};
    /// Frame bytes handed to the sink so far.
    uint64_t written{};
    /// The first failure, which every later call returns.
    Error failed{Error::none};
};

/// Begins a frame written in pieces.
///
/// @param[out] writer the writer; any frame it held is freed first
/// @param level a level from `lowest_level` to `highest_level`
/// @param whole the input's size: exactly this many bytes are given before the frame ends
/// @param room where the frame's bytes gather; at least one byte
/// @param sink takes the frame's bytes in order
/// @return `none`, or `bad_level`, `out_of_memory` or `damaged`
[[nodiscard]] Error frame_writer_begin(
    FrameWriter& writer, int level, uint64_t whole, std::span<uint8_t> room, FrameSink sink
);

/// Takes the next bytes of a frame's input, handing on what is compressed.
///
/// @param[in,out] writer a writer begun with `frame_writer_begin`
/// @param input the next bytes; empty takes nothing
/// @return `none`, or `damaged` (more input than begun with), `sink_refused`
///         or the writer's earlier failure
[[nodiscard]] Error frame_writer_write(FrameWriter& writer, std::span<const uint8_t> input);

/// Ends a frame, handing its last bytes to the sink.
///
/// @param[in,out] writer a writer that took the whole input it was begun with
/// @return `none`, or `damaged` (less input than begun with), `sink_refused`
///         or the writer's earlier failure
[[nodiscard]] Error frame_writer_end(FrameWriter& writer);

/// Frees a writer's work area. A writer freed, or never begun, is left as new.
///
/// @param[in,out] writer the writer
void frame_writer_free(FrameWriter& writer) noexcept;

/// One zstd frame read from bytes that arrive in pieces.
///
/// A reader takes one data frame and nothing else, as `decompress` does: a
/// skippable frame, or any byte after the frame ends, is `damaged`. It keeps
/// a window of at most `most_read_window_log` bits and nothing that grows
/// with the frame. Begin it with `frame_reader_begin` and free it with
/// `frame_reader_free`.
struct FrameReader {
    /// zstd's decompression context; null before `frame_reader_begin`.
    void* context{};
    /// The frame's first bytes, gathered to tell a skippable frame from a data frame.
    std::array<uint8_t, 4> magic{};
    /// How many of `magic` have arrived.
    uint8_t magic_seen{};
    /// True once the frame has ended and every byte of it was handed out.
    bool ended{};
    /// Bytes handed out so far.
    uint64_t produced{};
    /// The first failure, which every later call returns.
    Error failed{Error::none};
};

/// Begins reading a frame in pieces.
///
/// @param[out] reader the reader; any frame it held is freed first
/// @return `none`, or `out_of_memory`
[[nodiscard]] Error frame_reader_begin(FrameReader& reader);

/// Decodes as much of the frame as the input and the room allow.
///
/// Stops when the room is full, the input is used up or the frame ends.
/// The frame ends when `reader.ended` is set; input left then is `damaged`.
///
/// @param[in,out] reader a reader begun with `frame_reader_begin`
/// @param[in,out] input the frame's next bytes; left holding those not taken
/// @param out room for the decoded bytes
/// @param[out] decoded how many bytes of `out` were written
/// @return `none`, or `damaged` or the reader's earlier failure
[[nodiscard]] Error frame_reader_read(
    FrameReader& reader,
    std::span<const uint8_t>& input,
    std::span<uint8_t> out,
    std::size_t& decoded
);

/// Frees a reader's window. A reader freed, or never begun, is left as new.
///
/// @param[in,out] reader the reader
void frame_reader_free(FrameReader& reader) noexcept;

/// Reads the size a frame names, refusing one past `most_bytes`.
///
/// Only the frame's header is read, so nothing is allocated and a frame
/// that names a large size costs no more than a small one. A size equal to
/// `most_bytes` is accepted.
///
/// @param frame the frame's bytes
/// @param most_bytes the most bytes the frame may expand to
/// @return the size the frame names, or `damaged` for a header that cannot be
///         read, `too_large` for a size above `most_bytes`, or `size_unknown`
///         for a header that names no size
[[nodiscard]] Sized content_size(std::span<const uint8_t> frame, std::size_t most_bytes);

/// Decompresses one zstd frame into `out`.
///
/// The input must be exactly one frame. A frame that names a size larger
/// than `out` is refused before it is decoded. A frame that names no size
/// is decoded until it ends or `out` is full. The contents of `out` are
/// unspecified after an error.
///
/// @param frame the frame's bytes
/// @param[out] out room for the result; `content_size` says how much a frame needs
/// @return the number of bytes written, or `damaged` or `buffer_too_small`
[[nodiscard]] Sized decompress(std::span<const uint8_t> frame, std::span<uint8_t> out);

} // namespace oa::base::zstd
