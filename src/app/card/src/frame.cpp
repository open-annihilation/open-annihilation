// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Building and checking card command lists.
#include "oa/app/card.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace oa::app::card {

namespace {

// A frame's every vertex is checked, so the checks read a float's bits:
// under the strict floating-point model std::isfinite and std::fabs are
// calls into the runtime library, several a vertex.

/// The bits of a float less its sign: its magnitude, which orders as the
/// magnitudes do, every finite one below an infinity's and every infinity
/// below a NaN's.
///
/// @param value the float
/// @return its bits with the sign bit clear
[[nodiscard]] uint32_t magnitude_bits(float value) noexcept {
    return std::bit_cast<uint32_t>(value) & 0x7fff'ffffU;
}

/// The magnitude bits of an infinity: the least that is not a finite number.
constexpr uint32_t infinity_bits = 0x7f80'0000U;
static_assert(std::numeric_limits<float>::is_iec559);

/// Says whether a float is a finite number.
///
/// @param value the float
/// @return true when it is neither infinite nor a number
[[nodiscard]] bool finite(float value) noexcept {
    return magnitude_bits(value) < infinity_bits;
}

/// Says whether a colour's channels are all finite numbers.
///
/// @param colour the colour
/// @return true when none is infinite or not a number
bool finite(const Colour& colour) noexcept {
    return finite(colour.red) && finite(colour.green) && finite(colour.blue) &&
           finite(colour.alpha);
}

/// Says whether a coordinate is a finite number within largest_coordinate.
///
/// @param value the coordinate
/// @return true when it is
bool within_reach(float value) noexcept {
    return magnitude_bits(value) <= std::bit_cast<uint32_t>(largest_coordinate);
}

/// Names a batch in a fault.
///
/// @param batch the batch's position in the frame
/// @return "batch N: "
std::string batch_name(std::size_t batch) {
    return "batch " + std::to_string(batch) + ": ";
}

/// Returns what is wrong with a draw batch's range and page, or nothing.
///
/// @param frame the frame
/// @param batch the batch
/// @param position the batch's position in the frame
/// @return the fault; empty for none
std::string check_draw(const CardFrame& frame, const Batch& batch, std::size_t position) {
    const std::size_t end = std::size_t{batch.first_index} + batch.index_count;
    if (end > frame.indices.size())
        return batch_name(position) + "indices " + std::to_string(batch.first_index) + " to " +
               std::to_string(end) + " beyond the " + std::to_string(frame.indices.size()) +
               " of the frame";
    if (batch.index_count % 3 != 0)
        return batch_name(position) + "an index count of " + std::to_string(batch.index_count) +
               " is not whole triangles";
    if (batch.level >= most_page_levels)
        return batch_name(position) + "level " + std::to_string(batch.level) + " is beyond the " +
               std::to_string(most_page_levels) + " levels a page may have";
    if (batch.page == PageHandle{} && batch.level != 0)
        return batch_name(position) + "level " + std::to_string(batch.level) + " without a page";
    if (batch.scissored && (batch.scissor.width <= 0 || batch.scissor.height <= 0))
        return batch_name(position) + "an empty scissor";
    const std::size_t vertex_count = frame.vertices.size();
    for (std::size_t at = batch.first_index; at < end; ++at)
        if (frame.indices[at] >= vertex_count)
            return batch_name(position) + "index " + std::to_string(at) + " names vertex " +
                   std::to_string(frame.indices[at]) + " of " + std::to_string(vertex_count);
    return {};
}

/// Returns what is wrong with a resolve batch, or nothing.
///
/// @param batch the batch
/// @param position the batch's position in the frame
/// @return the fault; empty for none
std::string check_resolve(const Batch& batch, std::size_t position) {
    if (batch.source == TargetHandle{})
        return batch_name(position) + "a resolve of no render target";
    if (batch.source == batch.target)
        return batch_name(position) + "a resolve of a render target into itself";
    if (batch.blend != Blend::none && batch.blend != Blend::alpha &&
        batch.blend != Blend::alpha_premultiplied)
        return batch_name(position) + "a resolve blends only by none, alpha or premultiplied alpha";
    if (batch.destination.width <= 0 || batch.destination.height <= 0)
        return batch_name(position) + "an empty destination";
    if (batch.scissored && (batch.scissor.width <= 0 || batch.scissor.height <= 0))
        return batch_name(position) + "an empty scissor";
    return {};
}

/// Returns what is wrong with a two-level reduction batch, or nothing.
///
/// @param batch the batch
/// @param position the batch's position in the frame
/// @return the fault; empty for none
std::string check_blend_reduce(const Batch& batch, std::size_t position) {
    if (batch.source == TargetHandle{})
        return batch_name(position) + "a two-level reduction of no render target";
    if (batch.source == batch.target)
        return batch_name(position) + "a two-level reduction of a render target into itself";
    if (batch.blend != Blend::none)
        return batch_name(position) + "a two-level reduction blends only by none";
    const Rect& part = batch.source_part;
    if (part.width <= 0 || part.height <= 0)
        return batch_name(position) + "an empty source part";
    if (part.x < 0 || part.y < 0)
        return batch_name(position) + "a source part left of or above its texture";
    if (part.x % 2 != 0 || part.y % 2 != 0 || part.width % 2 != 0 || part.height % 2 != 0)
        return batch_name(position) + "a source part that is not on even pixels";
    if (batch.destination.width <= 0 || batch.destination.height <= 0)
        return batch_name(position) + "an empty destination";
    const double across = static_cast<double>(batch.destination.width) / part.width;
    const double down = static_cast<double>(batch.destination.height) / part.height;
    if (across < smallest_blend_reduce_scale || across > 1.0 ||
        down < smallest_blend_reduce_scale || down > 1.0)
        return batch_name(position) + "a two-level reduction scales by one half to 1, not " +
               std::to_string(across) + " across and " + std::to_string(down) + " down";
    if (batch.scissored && (batch.scissor.width <= 0 || batch.scissor.height <= 0))
        return batch_name(position) + "an empty scissor";
    return {};
}

} // namespace

void append_quad(
    CardFrame& frame,
    float x,
    float y,
    float width,
    float height,
    float u0,
    float v0,
    float u1,
    float v1,
    const Colour& colour
) {
    const auto first = static_cast<Index>(frame.vertices.size());
    frame.vertices.push_back({x, y, colour, u0, v0});
    frame.vertices.push_back({x + width, y, colour, u1, v0});
    frame.vertices.push_back({x + width, y + height, colour, u1, v1});
    frame.vertices.push_back({x, y + height, colour, u0, v1});
    // The diagonal runs from the top-left to the bottom-right corner.
    frame.indices.push_back(first);
    frame.indices.push_back(first + 1);
    frame.indices.push_back(first + 2);
    frame.indices.push_back(first);
    frame.indices.push_back(first + 2);
    frame.indices.push_back(first + 3);
}

std::string check_frame(const CardFrame& frame) {
    if (frame.vertices.size() > most_frame_vertices)
        return std::to_string(frame.vertices.size()) + " vertices, more than the " +
               std::to_string(most_frame_vertices) + " a frame may hold";
    if (frame.indices.size() > most_frame_indices)
        return std::to_string(frame.indices.size()) + " indices, more than the " +
               std::to_string(most_frame_indices) + " a frame may hold";
    if (frame.batches.size() > most_frame_batches)
        return std::to_string(frame.batches.size()) + " batches, more than the " +
               std::to_string(most_frame_batches) + " a frame may hold";
    for (std::size_t at = 0; at < frame.vertices.size(); ++at) {
        const Vertex& vertex = frame.vertices[at];
        if (!within_reach(vertex.x) || !within_reach(vertex.y))
            return "vertex " + std::to_string(at) + " lies beyond reach or is not a number";
        if (!finite(vertex.colour) || !finite(vertex.u) || !finite(vertex.v))
            return "vertex " + std::to_string(at) +
                   " has a colour or texture coordinate that is "
                   "not a number";
    }
    for (std::size_t position = 0; position < frame.batches.size(); ++position) {
        const Batch& batch = frame.batches[position];
        if (static_cast<uint8_t>(batch.operation) >= operation_count)
            return batch_name(position) + "operation " +
                   std::to_string(static_cast<unsigned>(batch.operation)) + " names none";
        if (static_cast<uint8_t>(batch.blend) >= blend_count)
            return batch_name(position) + "blend " +
                   std::to_string(static_cast<unsigned>(batch.blend)) + " names none";
        if (static_cast<uint8_t>(batch.sampling) >= sampling_count)
            return batch_name(position) + "sampling " +
                   std::to_string(static_cast<unsigned>(batch.sampling)) + " names none";
        // A shift moves a resolve's or a two-level reduction's picture by
        // less than a pixel.
        const bool shifted = batch.shift_x != 0.0F || batch.shift_y != 0.0F;
        if (!(std::abs(batch.shift_x) < 1.0F) || !(std::abs(batch.shift_y) < 1.0F))
            return batch_name(position) + "a shift of a whole pixel or more, or not a number";
        if (shifted && batch.operation != Operation::resolve &&
            batch.operation != Operation::blend_reduce)
            return batch_name(position) + "a shift on a batch that is neither a resolve nor a "
                                          "two-level reduction";
        std::string fault;
        switch (batch.operation) {
        case Operation::draw:
            fault = check_draw(frame, batch, position);
            break;
        case Operation::clear:
            if (!finite(batch.colour))
                fault = batch_name(position) + "a clear colour that is not a number";
            break;
        case Operation::resolve:
            fault = check_resolve(batch, position);
            break;
        case Operation::blend_reduce:
            fault = check_blend_reduce(batch, position);
            break;
        }
        if (!fault.empty())
            return fault;
    }
    return {};
}

} // namespace oa::app::card
