// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::formats::gaf {

// TA's GAF version word. The game does not reject other values, and stock
// GAFs also store zero, so parse() retains the header word verbatim.
inline constexpr uint32_t version_ta = 0x00010100U;

// On-disk records: header, sequence header, frame-list item and frame info.
// Multi-byte fields are little-endian.
namespace on_disk {

inline constexpr std::size_t sequence_name_bytes = 32;

struct Header {
    uint32_t version = 0;
    uint32_t sequence_count = 0;
    // Zero in every shipped GAF. Kept as read; 3.1c does not interpret it.
    uint32_t reserved = 0;
};

struct SequenceHeader {
    uint16_t frame_count = 0;
    uint16_t repeat_flags = 0;
    // Zero in every shipped GAF. Kept as read and not interpreted.
    uint32_t reserved = 0;
    std::array<uint8_t, sequence_name_bytes> name{};
};

struct FrameListItem {
    uint32_t frame_offset = 0;
    // Full on-disk word. The game observes only the low 16 bits.
    uint32_t duration = 0;
};

struct FrameInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t origin_x = 0;
    int16_t origin_y = 0;
    uint8_t transparency_index = 0;
    uint8_t compressed = 0;
    // Two separate bytes, not one uint16 layer count: the game takes byte 10
    // as the layer count and byte 11 as a child's special-render flag.
    uint8_t layer_count = 0;
    uint8_t special_render_flag = 0;
    // Zero in every shipped GAF. Kept as read and not interpreted.
    uint32_t reserved = 0;
    uint32_t data_offset = 0;
    // ? The word a loaded frame keeps its auxiliary plane in. The file's value
    // is not an offset and varies between shipped frames; kept as read and
    // not interpreted.
    uint32_t aux_plane_slot = 0;
};

} // namespace on_disk

namespace layout {
inline constexpr std::size_t header_bytes = sizeof(on_disk::Header);
inline constexpr std::size_t sequence_header_bytes = sizeof(on_disk::SequenceHeader);
inline constexpr std::size_t sequence_name_bytes = on_disk::sequence_name_bytes;
inline constexpr std::size_t frame_list_item_bytes = sizeof(on_disk::FrameListItem);
inline constexpr std::size_t frame_record_bytes = sizeof(on_disk::FrameInfo);
inline constexpr std::size_t frame_layer_count_offset = offsetof(on_disk::FrameInfo, layer_count);
inline constexpr std::size_t frame_special_render_offset =
    offsetof(on_disk::FrameInfo, special_render_flag);
} // namespace layout

static_assert(layout::header_bytes == 12);
static_assert(layout::sequence_header_bytes == 40);
static_assert(layout::sequence_name_bytes == 32);
static_assert(layout::frame_list_item_bytes == 8);
static_assert(layout::frame_record_bytes == 24);
static_assert(layout::frame_layer_count_offset == 10);
static_assert(layout::frame_special_render_offset == 11);
static_assert(offsetof(on_disk::Header, sequence_count) == 4);
static_assert(offsetof(on_disk::SequenceHeader, name) == 8);
static_assert(offsetof(on_disk::Header, reserved) == 8);
static_assert(offsetof(on_disk::SequenceHeader, reserved) == 4);
static_assert(offsetof(on_disk::FrameListItem, duration) == 4);
static_assert(offsetof(on_disk::FrameInfo, reserved) == 12);
static_assert(offsetof(on_disk::FrameInfo, data_offset) == 16);
static_assert(offsetof(on_disk::FrameInfo, aux_plane_slot) == 20);

namespace limit {
inline constexpr std::size_t input_bytes = 256U * 1024U * 1024U;
inline constexpr std::size_t sequences = 4096;
inline constexpr std::size_t frames_per_sequence = 4096;
inline constexpr std::size_t total_frame_records = 131072;
inline constexpr std::size_t nesting_depth = 32;
/// Pixels of one frame or layer: 4096 by 4096. The largest frame of the game
/// and the mods it was measured against is 3240 by 3240.
inline constexpr std::size_t pixels_per_frame = 4096U * 4096U;
inline constexpr std::size_t total_decoded_bytes = 256U * 1024U * 1024U;
} // namespace limit

struct Frame {
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t origin_x = 0;
    int16_t origin_y = 0;
    uint8_t transparency_index = 0;
    bool compressed = false;
    uint8_t layer_count = 0;
    // Frame byte 11. The game draws a child frame with this byte set with
    // special blending, which this renderer does not implement.
    uint8_t special_render_flag = 0;
    uint32_t reserved = 0;       // on_disk::FrameInfo::reserved, as read
    uint32_t aux_plane_slot = 0; // ? on_disk::FrameInfo::aux_plane_slot, as read
    uint16_t duration = 0;
    /// Where a simple frame's pixel data lies in the file, from its start:
    /// its raw palette indices, or its compressed rows with their length
    /// words. Zero, with pixel_data_bytes, for a frame with layers or one
    /// of no pixels.
    uint32_t pixel_data_offset{};
    /// Bytes of a simple frame's pixel data in the file: width * height for
    /// a raw frame, every row with its length word for a compressed one.
    uint32_t pixel_data_bytes{};
    // Simple frames contain width*height palette indices. Composite frames
    // contain layers instead. Coverage distinguishes compressed transparent
    // skip commands from literal pixels equal to the transparency index.
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> coverage;
    std::vector<Frame> layers;
};

struct Sequence {
    std::string name;
    uint16_t repeat_flags = 0;
    uint32_t reserved = 0; // on_disk::SequenceHeader::reserved, as read
    std::vector<Frame> frames;
};

struct Archive {
    uint32_t version = 0;
    uint32_t raw_sequence_count = 0;
    int16_t header_sequence_count = 0;
    uint32_t reserved = 0; // on_disk::Header::reserved, as read
    std::vector<Sequence> sequences;
};

enum class ErrorCode {
    none,
    input_limit,
    truncated,
    offset_out_of_range,
    sequence_limit,
    frame_limit,
    nesting_limit,
    pointer_cycle,
    pixel_limit,
    malformed_compression,
    unsupported_special_render,
    side_limit, ///< a frame or layer wider or taller than the caller allows
    unreadable, ///< a frame's pixel data could not be read from the file (render_ranged)
};

struct Error {
    ErrorCode code = ErrorCode::none;
    std::size_t offset = 0;
    std::string message;
};

struct ParseResult {
    std::optional<Archive> archive;
    std::optional<Error> error;

    /// Returns whether an archive was parsed.
    [[nodiscard]] bool ok() const noexcept { return archive.has_value(); }
};

struct SequenceResult {
    std::optional<Sequence> sequence;
    std::optional<Error> error;

    /// Returns whether a sequence was parsed.
    [[nodiscard]] bool ok() const noexcept { return sequence.has_value(); }
};

/// What `parse` keeps of each simple frame's palette indices.
enum class PixelData {
    /// The pixels and coverage are decoded and kept.
    decoded,
    /// The pixels are checked as `decoded` checks them, and the frame's
    /// pixels and coverage are left empty; `parse_sequence` decodes a
    /// sequence's pixels when they are needed, and `render_ranged` one
    /// frame's as it is drawn. Since nothing is kept, the file's frames
    /// together may decode past `limit::total_decoded_bytes`.
    checked,
};

/// Where `render_ranged` reads a file's bytes from.
struct ReadHooks {
    void* context{};
    /// Fills `output` with the file's bytes from `offset`, counted from the
    /// start of the file; returns false when they cannot all be read. Null
    /// reads nothing, so that every frame with pixels fails to render.
    bool (*read)(void* context, uint32_t offset, std::span<uint8_t> output){};
};

struct RenderedFrame {
    /// Drawn width, in pixels. The frame record's width when every layer fits
    /// inside it, and wider when a layer reaches past its left or right edge.
    uint16_t width = 0;
    /// Drawn height, in pixels. The frame record's height when every layer
    /// fits inside it, and taller when a layer reaches past its top or bottom.
    uint16_t height = 0;
    /// Hotspot column. The frame record's origin when every layer fits inside
    /// it; moved with the canvas when a layer reaches past the top or the
    /// left, so drawing the frame at one point keeps every layer there.
    int16_t origin_x = 0;
    /// Hotspot row, moved with the canvas as origin_x is.
    int16_t origin_y = 0;
    uint8_t transparency_index = 0;
    std::vector<uint8_t> pixels;
    // One byte per pixel. A set byte means the normal renderer performed an
    // overwrite at that position, including compressed literal/repeat values
    // equal to transparency_index.
    std::vector<uint8_t> coverage;
};

struct RenderResult {
    std::optional<RenderedFrame> frame;
    std::optional<Error> error;

    /// Returns whether a frame was rendered.
    [[nodiscard]] bool ok() const noexcept { return frame.has_value(); }
};

/// Parses the pointer-based GAF structure.
///
/// The sequence count is the signed low 16 bits of the header word, frame
/// counts are uint16 and byte 10 of a frame is its layer count. Pixel data
/// is decoded with a coverage mask; special-render blending is deferred to
/// the renderer. Every offset, row, run, pointer cycle and size is checked
/// against the limit namespace, and a frame's size against the limits and
/// its raw pixels or row lengths against the file before its buffers are
/// allocated.
///
/// @param bytes the whole file
/// @param pixels whether each simple frame keeps its decoded pixels and
///     coverage; either way a file parses, or fails with the same error
/// @param largest_side widest and tallest frame or layer accepted, in
///     pixels; a larger one fails with side_limit before it is decoded
/// @return the archive, or the first error and its byte offset
[[nodiscard]] ParseResult parse(
    std::span<const uint8_t> bytes,
    PixelData pixels = PixelData::decoded,
    uint16_t largest_side = UINT16_MAX
);

/// Parses one sequence of a file, with every frame's pixels and coverage
/// decoded, as `parse` parses it.
///
/// @param bytes the whole file
/// @param index the sequence's place in the file's sequence table, from 0
/// @return the sequence, or the first error and its byte offset
[[nodiscard]] SequenceResult parse_sequence(std::span<const uint8_t> bytes, std::size_t index);

/// Returns one frame of a sequence.
///
/// @param sequence parsed sequence, or null
/// @param index frame index; compared signed against the uint16 frame count
/// @return the frame, or null for a negative or too-large index or a null sequence
[[nodiscard]] const Frame* frame_at(const Sequence* sequence, int32_t index) noexcept;

/// Renders a frame and its layers as the game draws them normally.
///
/// Layers are drawn recursively. A layer's top left is its parent's hotspot
/// minus the layer's own origin, so the layer lands where drawing that layer
/// at the frame's point would. The canvas covers the frame's rectangle and
/// every layer's. Its origin is the frame's, moved by however far the canvas
/// grew past the frame's top and left, so drawing the result at the same
/// point puts every layer where it was. A frame whose layers fit inside it
/// keeps its size, its origin and its pixels. The canvas still has to fit in
/// one frame: each side in the width and height fields, and no more than
/// limit::pixels_per_frame pixels. The frame's own special-render flag is
/// ignored, as 3.1c ignores it when it draws a frame.
///
/// @param frame parsed frame
/// @return pixels, coverage, size and origin, or unsupported_special_render
///     when a child needs special blending, or pixel_limit when the canvas
///     exceeds the frame limits
[[nodiscard]] RenderResult render_normal(const Frame& frame);

/// Renders a frame of a checked parse as render_normal renders it decoded,
/// reading the pixel data of each simple frame from the file as it draws it.
///
/// Each simple frame's pixel data (Frame::pixel_data_offset and
/// pixel_data_bytes) is read through `reader` and decoded and checked as
/// `parse` decodes it, one simple frame at a time; nothing read is kept
/// once the frame is rendered. A simple frame that holds its pixels and
/// coverage is drawn from them without a read.
///
/// @param frame parsed frame, as `parse` gives it with PixelData::checked
/// @param reader where the file's bytes are read from
/// @return the pixels, coverage, size and origin render_normal gives for the
///     frame decoded; unreadable when a read fails, or the error `parse`
///     gives for bytes that do not decode
[[nodiscard]] RenderResult render_ranged(const Frame& frame, const ReadHooks& reader);

/// Returns the bytes `parse` keeps for a frame's decoded pixels and coverage,
/// two for each pixel of the frame or of each of its layers, nested layers
/// included.
///
/// @param frame parsed frame, with or without its pixels
/// @return the bytes
[[nodiscard]] uint64_t decoded_bytes(const Frame& frame) noexcept;

/// Returns the bytes `parse` keeps for the decoded pixels and coverage of
/// every frame of an archive.
///
/// @param archive parsed archive, with or without its pixels
/// @return the bytes
[[nodiscard]] uint64_t decoded_bytes(const Archive& archive) noexcept;

} // namespace oa::formats::gaf
