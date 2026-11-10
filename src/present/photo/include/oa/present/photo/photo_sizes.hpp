// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which photo sizes the battlefield can offer, and the box reduction that
// turns a smoother draw into the picture the player asked for. Pure: no
// window, no drawing and no files. The runtime supplies the battlefield,
// the render mode's limit and the memory still free.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace oa::present::photo {

/// The most pixels a side a processor photo is drawn at.
inline constexpr int32_t kProcessorEdge = 16384;

/// The most pixels a side one turned-view frame is drawn at. A larger photo
/// is drawn in tiles, each at most this long, up to 16384 photo pixels.
inline constexpr int32_t kTurnedEdge = 4096;

/// How the photo is drawn. The mode in use is the only one that draws it.
enum class CaptureKind : uint8_t {
    /// Off and Basic, on the processor.
    processor,
    /// Full classic, on the window's renderer.
    full,
    /// The turned view, on the live renderer.
    turned,
};

/// Why a size or a smooth step is not offered. `open` is offered.
enum class SizeLock : uint8_t {
    /// The player can choose it.
    open,
    /// A shared match draws only Screen, and smooth edges at 2×.
    shared,
    /// The draw would pass the mode's pixel limit.
    edge,
    /// The picture, or the targets it needs, would leave too little memory.
    memory,
    /// Off and Basic draw whole pixels, so a smooth step above Off is the
    /// Screen photo.
    pixels,
    /// A held picture. A poster is a new draw, so it waits.
    held,
    /// The turned view. A poster is the map from above.
    above,
    /// The picture is still moving. A poster needs a frozen picture.
    moving,
};

/// The battlefield a photo is taken from, in pixels.
struct Field {
    int32_t width{0};  ///< pixels across
    int32_t height{0}; ///< pixels down
};

/// What the machine and the match allow. A zero edge limit means the mode
/// has no pixel cap. Memory figures are bytes still free for this photo,
/// after the clipboard and the writer's queue have been counted.
struct Machine {
    CaptureKind kind{CaptureKind::processor}; ///< the render mode in use
    int32_t edge_limit{kProcessorEdge};       ///< pixels a side, or 0 for no cap
    /// Full's anti-aliasing factor, the one the window draws at now. It is
    /// never lowered to make a size fit. Other modes pass 1.
    int supersample{1};
    /// Full's world target is a multiple of this, in window pixels. Other
    /// modes pass 1.
    int grain{1};
    /// Bytes still free for the drawn picture and a world layer of the same
    /// size: six bytes a drawn pixel.
    uint64_t cpu_bytes{~uint64_t{0}};
    /// Bytes still free for Full's targets. Other modes ignore it.
    uint64_t gpu_bytes{~uint64_t{0}};
    int bytes_per_texel{4}; ///< Full's target, bytes a pixel
    bool shared{false};     ///< a shared match, or a watcher
    /// The window's zoom, pixels of the battlefield a map pixel. A Full
    /// photo whose own zoom is still below 1 makes no world target.
    float zoom{1.0F};
    /// A check-only cap on a tile's kept edge, in photo pixels. Zero leaves
    /// the card, or the turned view's sampling edge, to decide. It does not
    /// lower the anti-aliasing factor, and a shared match never reaches it.
    int32_t tile_edge{0};
    /// The turned view's longest tile side, in pixels, that still draws with
    /// the window's sampling: the largest square that keeps it. A tile is
    /// squarer than the window, and the card's multisampling is limited by
    /// area, so a square tile as long as edge_limit can lose it. Zero leaves
    /// edge_limit to decide. Other modes ignore it.
    int32_t tile_limit{0};
    /// The panel's depth of field, 0 to 32. The turned view adds that blur,
    /// scaled by the photo's size and smooth step, to each tile's border.
    /// Other modes ignore it.
    int depth_of_field{0};
    /// The card's largest viewport, in pixels. Zero means the card is not
    /// open yet, and a turned-view tile is not shortened for it. A tile's
    /// colour viewport is its draw times `viewport_samples`. Other modes
    /// ignore it.
    int32_t viewport_limit{0};
    /// The supersample the turned view keeps in every tile. At least 1 when
    /// `viewport_limit` is set. Other modes ignore it.
    int viewport_samples{1};
    /// Bytes the photo's look keeps beside the picture, for each pixel of
    /// the picture: 3 while a tilt-shift is on, which blurs a copy of it,
    /// otherwise 0.
    int look_bytes_per_pixel{0};
    /// Bytes the photo's look keeps once, whatever the picture's size: the
    /// tilt-shift's sums (kLookScratchBytes), or 0.
    uint64_t look_bytes{0};
};

/// The most the tilt-shift's blur keeps in sums at once, in bytes.
inline constexpr uint64_t kLookScratchBytes = uint64_t{64} * 1024U * 1024U;

/// One size, or one smooth step, as the panel lists it.
struct Offer {
    int factor{1};                 ///< 1, 2, 3 or 4 for a size; 1, 2 or 4 for smooth edges
    int32_t width{0};              ///< the picture's width, after the reduction
    int32_t height{0};             ///< the picture's height, after the reduction
    bool offered{false};           ///< the player can choose it
    SizeLock lock{SizeLock::open}; ///< why it is not offered
    /// The pixel cap the reason names when `lock` is `edge`. For Full this is
    /// the picture size that still fits at the window's anti-aliasing, not
    /// the card's raw texture limit.
    int32_t limit{0};
};

/// The sizes at one smooth step, and the smooth steps at one size.
struct Plan {
    CaptureKind kind{CaptureKind::processor}; ///< the mode the offers were judged for
    /// Screen, 2×, 3× and 4×, judged at `smooth`.
    Offer sizes[4]{};
    /// Off, 2× and 4×, judged at `size_factor`.
    Offer smooth[3]{};
};

/// One photo: the picture's size, and the larger draw the reduction starts from.
struct Draw {
    int size_factor{1}; ///< the size, 1 for Screen
    int smooth{1};      ///< the smooth step, 1 when smooth edges are off
    int32_t width{0};   ///< the picture
    int32_t height{0};
    int32_t draw_width{0};  ///< the draw, `width` times the smooth step
    int32_t draw_height{0}; ///< the draw, `height` times the smooth step
    bool offered{false};
    SizeLock lock{SizeLock::open};
    int32_t limit{0};
};

/// The sizes offered for a battlefield, and the smooth steps offered for the
/// size the player has. A larger size shows the same area of the map with
/// more pixels. A shared match offers Screen, and smooth edges at 2× where
/// the mode draws samples inside a pixel. Off and Basic draw whole pixels,
/// so every smooth step above Off is locked there. A size whose draw would
/// pass the mode's limit, or the memory still free, is not offered. Full
/// classic still offers a size past the card's texture when that picture can
/// be drawn in tiles, up to 16384 photo pixels a side, paying for the photo
/// and one tile. The turned view does the same past the size whose sampling
/// matches the window, up to 16384, each tile at most 4096. A shared match
/// does not offer those tiles.
///
/// @param field the battlefield
/// @param machine the mode, its limit and the memory still free
/// @param size_factor the size the smooth steps are judged at: 1, 2, 3 or 4
/// @param smooth the smooth step the sizes are judged at: 1, 2 or 4
/// @return the offers
[[nodiscard]] Plan plan_sizes(Field field, const Machine& machine, int size_factor, int smooth);

/// Whether one size and one smooth step can be drawn, and at what size.
///
/// @param field the battlefield
/// @param machine the mode, its limit and the memory still free
/// @param size_factor 1, 2, 3 or 4
/// @param smooth 1, 2 or 4
/// @return the picture and the draw
[[nodiscard]] Draw plan_draw(Field field, const Machine& machine, int size_factor, int smooth);

/// The longest side one turned-view tile may draw: the sampling edge, the
/// square tile limit when it is set, the card's viewport, and never more
/// than 4096.
///
/// @param machine the turned view's limits
/// @return the side, in pixels
[[nodiscard]] int32_t turned_tile_draw_limit(const Machine& machine);

/// Box-reduces packed RGB by an integer factor. Each output pixel is the
/// mean of one factor-by-factor block, rounded half up. A factor of 1 copies
/// the picture.
///
/// @param pixels the source, three bytes a pixel, row by row
/// @param count the source's bytes
/// @param width the source's width
/// @param height the source's height
/// @param factor the block's side, 1 or more; the source's sides are multiples of it
/// @param[out] reduced the picture
/// @param[out] out_width its width
/// @param[out] out_height its height
/// @return false when the source is not a whole number of blocks
[[nodiscard]] bool box_reduce(
    const uint8_t* pixels,
    std::size_t count,
    int32_t width,
    int32_t height,
    int factor,
    std::vector<uint8_t>& reduced,
    int32_t& out_width,
    int32_t& out_height
);

} // namespace oa::present::photo
