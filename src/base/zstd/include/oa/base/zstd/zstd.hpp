// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// One zstd frame, into and out of buffers the caller owns. The base tier
// holds no heap container, so nothing here allocates the bytes it returns:
// the caller asks for a size, makes room for it, and passes the room in.
// Compression pledges the input's size, so a frame this module writes names
// that size in its header, and never declares a window above
// `most_window_log`. Decompression reads the size a frame names, refusing one
// past the caller's limit, before the caller allocates anything.

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
