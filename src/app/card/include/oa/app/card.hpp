// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The card command list: what one frame asks the graphics card to draw, as
// plain data with no SDL. A CardFrame holds one array of vertices, one
// array of indices into it and an ordered list of batches. A draw batch
// draws a range of the indices as triangles, from one level of a texture
// page or untextured, with a blend mode, a sampling mode and an optional
// scissor, into the frame's final target or into a render target; a clear
// fills a render target with a colour; a resolve draws a render target,
// reduced from its supersampling factor to its own size, into a rectangle
// of another target. The executor (card/executor.hpp) makes the pages and
// targets and runs the list on SDL's renderer. Nothing here is reached by
// the standard tier or the Basic tier: only the Full tier builds and runs
// frames, the terrain's first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace oa::app::card {

// ---------------------------------------------------------------------------
// Texels, handles and limits

/// A texel of a page: four bytes, red, green, blue and alpha, in that order
/// in memory, the order the terrain and sprite pages hold their texels.
/// Where a page is drawn by Blend::alpha_premultiplied, each colour is
/// multiplied by the alpha already and a transparent texel is zero in every
/// byte.
struct Texel {
    uint8_t red{};
    uint8_t green{};
    uint8_t blue{};
    uint8_t alpha{};
};

/// Bytes of one texel.
inline constexpr uint32_t texel_bytes = 4;

static_assert(sizeof(Texel) == texel_bytes);
static_assert(std::is_standard_layout_v<Texel>);

/// A texture page the executor made; value 0 names no page, which draws
/// untextured.
struct PageHandle {
    uint32_t value{};

    friend bool operator==(const PageHandle&, const PageHandle&) = default;
};

/// A render target the executor made; value 0 names the frame's final
/// target, the one the executor is given with the frame.
struct TargetHandle {
    uint32_t value{};

    friend bool operator==(const TargetHandle&, const TargetHandle&) = default;
};

/// Most vertices one frame holds: 256 MiB of them. The renderer takes any
/// count; the limit only bounds the memory a frame built wrong could take,
/// far above what a battle of thousands of units at the widest zoom needs.
inline constexpr uint32_t most_frame_vertices = 1U << 23;
/// Most indices one frame holds: 128 MiB of them.
inline constexpr uint32_t most_frame_indices = 1U << 25;
/// Most batches one frame holds.
inline constexpr uint32_t most_frame_batches = 1U << 20;
/// Most levels of one page, level 0 included.
inline constexpr uint8_t most_page_levels = 8;
/// The largest edge of a page's level 0, in texels, whatever the renderer allows.
inline constexpr uint32_t largest_page_edge = 16384;
/// The largest edge of a render target's texture, its factor applied.
inline constexpr uint32_t largest_target_edge = 16384;
/// The largest supersampling factor a render target takes: the factors are
/// the powers of two from 1 to this, each halving of the reduction
/// averaging every sample.
inline constexpr uint32_t largest_supersampling_factor = 16;
/// The furthest a vertex may lie from the target's origin, in pixels.
inline constexpr float largest_coordinate = 1048576.0F;

/// Returns the edge of a page's level: level 0's edge halved once for each
/// level, rounded down, never below 1.
///
/// @param edge the edge of level 0, in texels
/// @param level the level, 0 to most_page_levels - 1
/// @return the level's edge in texels
[[nodiscard]] constexpr uint32_t level_edge(uint32_t edge, uint8_t level) noexcept {
    const uint32_t halved = edge >> level;
    return halved == 0 ? 1U : halved;
}

// ---------------------------------------------------------------------------
// Vertices and batches

/// How a draw's pixels combine with the pixels under them. The source
/// colour is the texel times the vertex colour, and the source alpha the
/// texel's alpha times the vertex's.
enum class Blend : uint8_t {
    none,  ///< the source replaces what is under it
    alpha, ///< source times its alpha, over what is under it times one less the alpha
    /// The source, its colour multiplied by its alpha already, over what is
    /// under it times one less the alpha: how the sprite pages are drawn,
    /// and how a render target cleared transparent, whose colour is
    /// multiplied by its alpha already too, is resolved.
    alpha_premultiplied,
    additive, ///< source times its alpha added to what is under it, clamped at white
    modulate, ///< what is under it times the source colour; the alpha is ignored
    /// What is under it times one less the source alpha: the shade a shadow
    /// casts. The source colour is ignored.
    darken,
    /// Each channel the lesser of the source colour and what is under it:
    /// what is under it held to a colour at the most, as the outline of
    /// game text in the modern fonts holds the picture. The alpha is
    /// ignored. Where the renderer cannot (Capabilities::minimum_composed),
    /// it draws as alpha.
    minimum,
    /// What is under it times one more than the source colour less the
    /// source alpha, each channel clamped at white: a source of no alpha
    /// lights what is under it by its colour, up to twice as bright, as an
    /// explosion's flash lights the battlefield. Drawn from a page: SDL's
    /// software renderer leaves what is under an untextured draw of no
    /// alpha as it is.
    lighten,
};

/// The number of blend modes; a Blend value is below it.
inline constexpr uint8_t blend_count = 8;

/// How a draw reads a page's texels where it enlarges or reduces them.
enum class Sampling : uint8_t {
    nearest, ///< the nearest texel
    linear,  ///< the two nearest texels each way, weighted
    /// The nearest texel, each texel's edge blended over one pixel where
    /// the draw enlarges the page: even blocks with soft edges at a zoom
    /// that is not a whole number. Read as nearest where the renderer lacks
    /// the mode.
    pixel_art,
};

/// The number of sampling modes; a Sampling value is below it.
inline constexpr uint8_t sampling_count = 3;

/// A colour with each channel from 0 to 1, laid out as four floats in the
/// order the renderer reads them.
struct Colour {
    float red{1.0F};
    float green{1.0F};
    float blue{1.0F};
    float alpha{1.0F};
};

/// One vertex: where it lands, in pixels of the target it is drawn into,
/// with the target's own size whatever its supersampling factor; its
/// colour; and the texture coordinate across the page's level, 0 to 1,
/// read from a page of texels at the texel the coordinate falls in.
struct Vertex {
    float x{};
    float y{};
    Colour colour{};
    float u{};
    float v{};
};

static_assert(sizeof(Colour) == 4 * sizeof(float));
static_assert(sizeof(Vertex) == 8 * sizeof(float));
static_assert(std::is_standard_layout_v<Vertex>);

/// An index into a frame's vertices.
using Index = uint32_t;

/// A rectangle in whole pixels of a target, or whole texels of a page.
struct Rect {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

/// What a batch does.
enum class Operation : uint8_t {
    draw,    ///< draws a range of the frame's indices as triangles
    clear,   ///< fills a target with a colour
    resolve, ///< draws a render target, reduced to its size, into a rectangle of another
    /// Draws a part of a render target's texture, reduced into a rectangle
    /// of another target by the two-level blend: the texture's half, each
    /// of whose pixels is the mean of four, drawn LINEAR at twice the
    /// scale, then the part itself drawn LINEAR over it at alpha 1 - t,
    /// t = log2(1 / scale), by the scale across, which runs from one half
    /// to 1. The part is taken as opaque. At a scale of one half the
    /// result is the half alone, each pixel a box of four; at 1 it is the
    /// part itself; between, weights that change evenly with the scale,
    /// so nothing pops as the scale eases.
    blend_reduce,
};

/// The number of operations; an Operation value is below it.
inline constexpr uint8_t operation_count = 4;

/// The least a blend_reduce scales its part by, across and down; the most
/// is 1.
inline constexpr double smallest_blend_reduce_scale = 0.5;

/// One batch of a frame. A draw uses the page, level, blend, sampling,
/// scissor and index range; a clear the target and colour; a resolve the
/// source, destination, blend (none, alpha or alpha_premultiplied) and
/// scissor; a blend_reduce the source, source_part, destination and
/// scissor, its blend none. Fields an operation does not use are left at
/// their defaults.
struct Batch {
    Operation operation{Operation::draw};
    /// The target drawn into, cleared or resolved into; none for the frame's
    /// final target.
    TargetHandle target{};
    PageHandle page{}; ///< the page a draw reads; none draws the vertex colours alone
    uint8_t level{};   ///< the page's level a draw reads; 0 without a page
    Blend blend{Blend::none};
    Sampling sampling{Sampling::nearest}; ///< how a draw reads the page's texels
    bool scissored{};                     ///< true when `scissor` limits the pixels written
    Rect scissor{};                       ///< in pixels of the target, with the target's own size
    Index first_index{};                  ///< the first index of a draw, into CardFrame::indices
    uint32_t index_count{};               ///< the indices of a draw, a multiple of 3
    TargetHandle source{}; ///< the render target a resolve or a blend_reduce draws; never none
    /// Where a resolve or a blend_reduce lands, in pixels of the target it
    /// draws into; a resolve scales the whole source, reduced to its own
    /// size, into it, and a blend_reduce its source_part. A resolve halves
    /// its source until its factor is at most twice that of the target it
    /// draws into (1 for the frame's final target), so that its one LINEAR
    /// draw reduces it by at most 2 in that target's own texels: into a
    /// target at the source's factor it lands texel for texel.
    Rect destination{};
    /// How far past the destination's corner a resolve or a blend_reduce
    /// lands, across and down, in pixels of the target it draws into, each
    /// within one pixel either way: the source is drawn LINEAR between that
    /// target's pixels, so that its picture moves by a fraction of a pixel.
    /// 0 lands it on the destination; every other batch leaves it at 0.
    float shift_x{};
    float shift_y{};
    /// The part of the source a blend_reduce draws, in pixels of the
    /// source's texture, its factor applied: whole even pixels, so that the
    /// part's half is whole pixels of the texture's half and no renderer
    /// truncates a fraction.
    Rect source_part{};
    Colour colour{}; ///< what a clear fills with
};

/// One frame's command list.
struct CardFrame {
    std::vector<Vertex> vertices;
    std::vector<Index> indices;
    std::vector<Batch> batches; ///< in the order they run

    /// Empties the frame, keeping its arrays' memory for the next one.
    void reset() noexcept {
        vertices.clear();
        indices.clear();
        batches.clear();
    }
};

/// Appends a quad as two triangles, which share the diagonal from its
/// top-left to its bottom-right corner: four vertices and six indices.
///
/// @param[in,out] frame the frame
/// @param x the left edge, in pixels of the target
/// @param y the top edge
/// @param width pixels across
/// @param height pixels down
/// @param u0 the texture coordinate of the left edge, 0 to 1
/// @param v0 the texture coordinate of the top edge
/// @param u1 the texture coordinate of the right edge
/// @param v1 the texture coordinate of the bottom edge
/// @param colour the colour of every corner
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
);

/// Returns what is malformed about a frame, apart from which pages and
/// targets exist, which only the executor knows: a count beyond a limit,
/// an operation, blend or sampling value that names none, a draw whose
/// index range leaves the indices or whose count is not a multiple of 3,
/// an index beyond the vertices, a level without a page or beyond
/// most_page_levels, an empty scissor or destination, a resolve of no
/// source, into its own source or with a blend other than none, alpha or
/// alpha_premultiplied, a blend_reduce of no source, into its own source,
/// with a blend other than none, of a part that is empty, lies left of or
/// above the texture or is not on even pixels, or at a scale across or
/// down outside smallest_blend_reduce_scale to 1, a shift that is not a
/// number or lies a whole pixel or more from 0, or one on a batch that is
/// neither a resolve nor a blend_reduce, or a vertex that is not finite or
/// lies beyond largest_coordinate. The executor refuses such a
/// frame whole and draws nothing of it.
///
/// @param frame the frame
/// @return what is wrong, naming the batch, index or vertex; empty when nothing is
[[nodiscard]] std::string check_frame(const CardFrame& frame);

} // namespace oa::app::card
