// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which photo sizes fit the battlefield, the render mode and the memory
// still free, and the box reduction of a smoother draw.
#include "oa/present/photo/photo_sizes.hpp"

#include "oa/present/photo/photo_tiles.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace oa::present::photo {
namespace {

constexpr int kSizeFactors[] = {1, 2, 3, 4};
constexpr int kSmoothFactors[] = {1, 2, 4};

/// A count rounded up to a multiple of a grain. A grain of 1 leaves it.
///
/// @param pixels the count
/// @param grain the grain, at least 1
/// @return the least multiple of the grain not below the count
int32_t rounded_up(int32_t pixels, int grain) {
    if (grain <= 1 || pixels <= 0)
        return pixels;
    const int32_t step = grain;
    return (pixels + step - 1) / step * step;
}

/// The pixels a Full world target holds at a size and a factor: its texture,
/// and the halves the reduction reads. The same count the Full tier uses.
///
/// @param width window pixels across the target
/// @param height window pixels down it
/// @param factor the anti-aliasing factor, a power of two
/// @return the pixels of every texture
uint64_t world_target_pixels(uint32_t width, uint32_t height, uint32_t factor) {
    if (width == 0 || height == 0 || factor == 0)
        return 0;
    const uint64_t texture_width = uint64_t{width} * factor;
    const uint64_t texture_height = uint64_t{height} * factor;
    uint64_t pixels = texture_width * texture_height;
    uint32_t doublings = 0;
    while ((uint32_t{1} << doublings) < factor && doublings < 16)
        ++doublings;
    const uint32_t halves = std::max<uint32_t>(1, doublings > 1 ? doublings - 1 : 0);
    for (uint32_t level = 1; level <= halves; ++level)
        pixels += (texture_width >> level) * (texture_height >> level);
    return pixels;
}

/// One factor, kept to the sizes and smooth steps the panel lists.
///
/// @param factor the caller's number
/// @param allowed the steps it may be
/// @param count how many `allowed` holds
/// @param fallback the step used when `factor` is none of them
/// @return the step
int kept_factor(int factor, const int* allowed, int count, int fallback) {
    for (int index = 0; index < count; ++index)
        if (allowed[index] == factor)
            return factor;
    return fallback;
}

/// What one draw needs, and whether the mode can make it.
struct Need {
    int32_t width{0}; ///< the picture
    int32_t height{0};
    int32_t draw_width{0}; ///< the draw
    int32_t draw_height{0};
    bool edge{false};   ///< the draw passes the pixel cap
    bool memory{false}; ///< the draw would leave too little memory
    int32_t limit{0};   ///< the cap the reason names, when `edge`
};

/// The pixel cap a reason names. Full names the largest picture whose
/// grain-rounded side, times the window's anti-aliasing, still fits the
/// card. Other modes name the cap itself.
///
/// @param machine the mode and its cap
/// @param photo_zoom the photo's own zoom, the window's times the factors
/// @return the number of pixels a side
int32_t reported_edge(const Machine& machine, float photo_zoom) {
    if (machine.edge_limit <= 0)
        return machine.edge_limit;
    if (machine.kind != CaptureKind::full || machine.supersample <= 1 || photo_zoom < 1.0F)
        return machine.edge_limit;
    const int supersample = std::max(machine.supersample, 1);
    const int grain = std::max(machine.grain, 1);
    const int32_t max_target = machine.edge_limit / supersample;
    if (max_target <= 0)
        return 0;
    return (max_target / grain) * grain;
}

/// A product that saturates instead of wrapping.
///
/// @param count one factor
/// @param scale the other
/// @return the product, or the largest count when it would not fit
uint64_t saturating_mul(uint64_t count, uint64_t scale) {
    if (count == 0 || scale == 0)
        return 0;
    if (count > ~uint64_t{0} / scale)
        return ~uint64_t{0};
    return count * scale;
}

/// A sum that saturates instead of wrapping.
///
/// @param count one count
/// @param extra the other
/// @return the sum, or the largest count when it would not fit
uint64_t saturating_add(uint64_t count, uint64_t extra) {
    if (count > ~uint64_t{0} - extra)
        return ~uint64_t{0};
    return count + extra;
}

/// The bytes a tiled Full photo asks for: the assembled photo, and the
/// largest tile's read and targets. One tile is drawn at a time.
///
/// @param plan the tiles
/// @param machine the factor and the texel size
/// @param photo_zoom the photo's zoom
/// @param draw_width the assembled draw
/// @param draw_height the assembled draw
/// @return the processor bytes and the card bytes
std::pair<uint64_t, uint64_t> tile_bytes(
    const TilePlan& plan,
    const Machine& machine,
    float photo_zoom,
    int32_t draw_width,
    int32_t draw_height
) {
    const uint64_t photo = saturating_mul(
        saturating_mul(
            static_cast<uint64_t>(std::max(draw_width, int32_t{0})),
            static_cast<uint64_t>(std::max(draw_height, int32_t{0}))
        ),
        uint64_t{6}
    );
    const int supersample = std::max(machine.supersample, 1);
    const uint64_t bytes = static_cast<uint64_t>(std::max(machine.bytes_per_texel, 1));
    uint64_t tile_cpu = 0;
    uint64_t tile_gpu = 0;
    for (const Tile& tile : plan.tiles) {
        const uint64_t area = saturating_mul(
            static_cast<uint64_t>(std::max(tile.draw_width, int32_t{0})),
            static_cast<uint64_t>(std::max(tile.draw_height, int32_t{0}))
        );
        tile_cpu = std::max(tile_cpu, saturating_mul(area, uint64_t{6}));
        uint64_t gpu = saturating_mul(area, bytes);
        if (supersample > 1 && photo_zoom >= 1.0F) {
            const uint64_t world = world_target_pixels(
                static_cast<uint32_t>(std::max(tile.target_width, int32_t{0})),
                static_cast<uint32_t>(std::max(tile.target_height, int32_t{0})),
                static_cast<uint32_t>(supersample)
            );
            gpu = saturating_add(gpu, saturating_mul(world, bytes));
        }
        tile_gpu = std::max(tile_gpu, gpu);
    }
    return {saturating_add(photo, tile_cpu), tile_gpu};
}

/// The tile plan for one Full draw.
///
/// @param need the picture and the draw
/// @param machine the card and the check's tile edge
/// @param photo_zoom the photo's zoom
/// @return the tiles
TilePlan tiles_for(const Need& need, const Machine& machine, float photo_zoom) {
    TileRequest request;
    request.width = need.draw_width;
    request.height = need.draw_height;
    request.zoom = photo_zoom > 0.0F ? photo_zoom : 1.0F;
    request.supersample = std::max(machine.supersample, 1);
    request.grain = std::max(machine.grain, 1);
    request.texture_limit = std::max(machine.edge_limit, int32_t{0});
    request.tile_edge = std::max(machine.tile_edge, int32_t{0});
    request.reach = full_tile_reach(request.zoom);
    return plan_tiles(request);
}

/// Judges one size and one smooth step.
///
/// @param field the battlefield
/// @param machine the mode and the memory
/// @param size_factor 1, 2, 3 or 4
/// @param smooth 1, 2 or 4
/// @return the picture, the draw and why it might be refused
Need judge(Field field, const Machine& machine, int size_factor, int smooth) {
    Need need;
    const float photo_zoom = machine.zoom * static_cast<float>(size_factor * smooth);
    need.limit = reported_edge(machine, photo_zoom);
    if (field.width <= 0 || field.height <= 0 || size_factor < 1 || smooth < 1)
        need.edge = true;
    const int64_t picture_w = int64_t{field.width} * size_factor;
    const int64_t picture_h = int64_t{field.height} * size_factor;
    const int64_t draw_w = picture_w * smooth;
    const int64_t draw_h = picture_h * smooth;
    if (picture_w <= 0 || picture_h <= 0 || picture_w > INT32_MAX || picture_h > INT32_MAX ||
        draw_w <= 0 || draw_h <= 0 || draw_w > INT32_MAX || draw_h > INT32_MAX) {
        need.edge = true;
        return need;
    }
    need.width = static_cast<int32_t>(picture_w);
    need.height = static_cast<int32_t>(picture_h);
    need.draw_width = static_cast<int32_t>(draw_w);
    need.draw_height = static_cast<int32_t>(draw_h);

    const int32_t draw_edge = std::max(need.draw_width, need.draw_height);
    if (machine.edge_limit > 0 && draw_edge > machine.edge_limit)
        need.edge = true;

    uint64_t gpu = 0;
    if (machine.kind == CaptureKind::full) {
        const int supersample = std::max(machine.supersample, 1);
        gpu = static_cast<uint64_t>(need.draw_width) * static_cast<uint64_t>(need.draw_height) *
              static_cast<uint64_t>(std::max(machine.bytes_per_texel, 1));
        if (supersample > 1 && photo_zoom >= 1.0F) {
            const int32_t target_w = rounded_up(need.draw_width, std::max(machine.grain, 1));
            const int32_t target_h = rounded_up(need.draw_height, std::max(machine.grain, 1));
            const int64_t texture_edge = int64_t{std::max(target_w, target_h)} * supersample;
            if (machine.edge_limit > 0 && texture_edge > machine.edge_limit)
                need.edge = true;
            gpu += world_target_pixels(
                       static_cast<uint32_t>(std::max(target_w, int32_t{0})),
                       static_cast<uint32_t>(std::max(target_h, int32_t{0})),
                       static_cast<uint32_t>(supersample)
                   ) *
                   static_cast<uint64_t>(std::max(machine.bytes_per_texel, 1));
        }
    }

    // A Full photo past the card is drawn in tiles, still by Full, at the
    // same factor. The photo and one tile have to fit in memory. A size that
    // already fits in one piece keeps that piece's cost, so a short memory
    // budget cannot be dodged by tiling. The check's tile edge can force a
    // split of a size that fits; when that split cannot be drawn, the size
    // is not offered. A shared match never draws tiles: Smooth edges 2×,
    // which it offers, stays one piece or is not offered.
    bool pay_for_tiles = false;
    if (machine.kind == CaptureKind::full && !machine.shared &&
        (need.edge || machine.tile_edge > 0)) {
        const TilePlan tiles = tiles_for(need, machine, photo_zoom);
        if (need.edge && tiles.fits)
            pay_for_tiles = true;
        if (!need.edge && machine.tile_edge > 0) {
            const bool forced =
                need.draw_width > machine.tile_edge || need.draw_height > machine.tile_edge;
            if (forced && !tiles.fits)
                need.edge = true;
        }
    }
    if (pay_for_tiles)
        need.edge = false;

    uint64_t cpu = static_cast<uint64_t>(need.draw_width) *
                   static_cast<uint64_t>(need.draw_height) * uint64_t{6};
    if (pay_for_tiles) {
        const TilePlan tiles = tiles_for(need, machine, photo_zoom);
        const std::pair<uint64_t, uint64_t> cost =
            tile_bytes(tiles, machine, photo_zoom, need.draw_width, need.draw_height);
        cpu = cost.first;
        gpu = cost.second;
    }
    // The look is painted on the picture after the draw, and a tilt-shift
    // keeps a copy of it while it blurs.
    cpu = saturating_add(
        cpu,
        saturating_add(
            saturating_mul(
                saturating_mul(
                    static_cast<uint64_t>(need.width), static_cast<uint64_t>(need.height)
                ),
                static_cast<uint64_t>(std::max(machine.look_bytes_per_pixel, 0))
            ),
            machine.look_bytes
        )
    );
    if (cpu > machine.cpu_bytes)
        need.memory = true;
    if (machine.kind == CaptureKind::full && gpu > machine.gpu_bytes)
        need.memory = true;
    return need;
}

/// An offer for one factor. Off and Basic lock every smooth step above Off,
/// because they draw whole pixels. A shared match then locks every size
/// above Screen and smooth edges at 4×, before the machine is asked.
///
/// @param field the battlefield
/// @param machine the mode and the memory
/// @param size_factor the size
/// @param smooth the smooth step
/// @param listed the factor the panel shows for this offer
/// @param as_size true when `listed` is a size; false when it is a smooth step
/// @return the offer
Offer offer_for(
    Field field, const Machine& machine, int size_factor, int smooth, int listed, bool as_size
) {
    Offer offer;
    offer.factor = listed;
    const bool whole_pixels = machine.kind == CaptureKind::processor && !as_size && smooth > 1;
    const bool shared_lock =
        machine.shared && ((as_size && size_factor > 1) || (!as_size && smooth >= 4));
    const Need need = judge(field, machine, size_factor, smooth);
    offer.width = need.width;
    offer.height = need.height;
    if (whole_pixels) {
        offer.lock = SizeLock::pixels;
        return offer;
    }
    if (shared_lock) {
        offer.lock = SizeLock::shared;
        return offer;
    }
    if (need.edge) {
        offer.lock = SizeLock::edge;
        offer.limit = need.limit;
        return offer;
    }
    if (need.memory) {
        offer.lock = SizeLock::memory;
        return offer;
    }
    offer.offered = true;
    offer.lock = SizeLock::open;
    return offer;
}

} // namespace

Plan plan_sizes(Field field, const Machine& machine, int size_factor, int smooth) {
    const int at_size = kept_factor(size_factor, kSizeFactors, 4, 1);
    const int at_smooth = kept_factor(smooth, kSmoothFactors, 3, 1);
    Plan plan;
    plan.kind = machine.kind;
    for (int index = 0; index < 4; ++index)
        plan.sizes[index] =
            offer_for(field, machine, kSizeFactors[index], at_smooth, kSizeFactors[index], true);
    for (int index = 0; index < 3; ++index)
        plan.smooth[index] =
            offer_for(field, machine, at_size, kSmoothFactors[index], kSmoothFactors[index], false);
    return plan;
}

Draw plan_draw(Field field, const Machine& machine, int size_factor, int smooth) {
    const int at_size = kept_factor(size_factor, kSizeFactors, 4, 1);
    const int at_smooth = kept_factor(smooth, kSmoothFactors, 3, 1);
    const Offer offer = offer_for(field, machine, at_size, at_smooth, at_size, true);
    // A smooth step a shared match locks is refused even at Screen.
    const Offer smooth_offer = offer_for(field, machine, at_size, at_smooth, at_smooth, false);
    Draw draw;
    draw.size_factor = at_size;
    draw.smooth = at_smooth;
    draw.width = offer.width;
    draw.height = offer.height;
    const Need need = judge(field, machine, at_size, at_smooth);
    draw.draw_width = need.draw_width;
    draw.draw_height = need.draw_height;
    if (!smooth_offer.offered) {
        draw.offered = false;
        draw.lock = smooth_offer.lock;
        draw.limit = smooth_offer.limit;
        return draw;
    }
    draw.offered = offer.offered;
    draw.lock = offer.lock;
    draw.limit = offer.limit;
    return draw;
}

bool box_reduce(
    const uint8_t* pixels,
    std::size_t count,
    int32_t width,
    int32_t height,
    int factor,
    std::vector<uint8_t>& reduced,
    int32_t& out_width,
    int32_t& out_height
) {
    reduced.clear();
    out_width = 0;
    out_height = 0;
    if (pixels == nullptr || factor < 1 || width <= 0 || height <= 0)
        return false;
    if (width % factor != 0 || height % factor != 0)
        return false;
    const std::size_t source_bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U;
    if (count != source_bytes)
        return false;
    out_width = width / factor;
    out_height = height / factor;
    reduced.assign(
        static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height) * 3U, 0
    );
    if (factor == 1) {
        std::copy(pixels, pixels + source_bytes, reduced.begin());
        return true;
    }
    const int blocks = factor * factor;
    const int half = blocks / 2;
    for (int32_t row = 0; row < out_height; ++row) {
        for (int32_t column = 0; column < out_width; ++column) {
            uint32_t red = 0;
            uint32_t green = 0;
            uint32_t blue = 0;
            for (int step_y = 0; step_y < factor; ++step_y) {
                for (int step_x = 0; step_x < factor; ++step_x) {
                    const std::size_t at = (static_cast<std::size_t>(row * factor + step_y) *
                                                static_cast<std::size_t>(width) +
                                            static_cast<std::size_t>(column * factor + step_x)) *
                                           3U;
                    red += pixels[at];
                    green += pixels[at + 1U];
                    blue += pixels[at + 2U];
                }
            }
            const std::size_t to =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(out_width) +
                 static_cast<std::size_t>(column)) *
                3U;
            reduced[to] = static_cast<uint8_t>(
                (red + static_cast<uint32_t>(half)) / static_cast<uint32_t>(blocks)
            );
            reduced[to + 1U] = static_cast<uint8_t>(
                (green + static_cast<uint32_t>(half)) / static_cast<uint32_t>(blocks)
            );
            reduced[to + 2U] = static_cast<uint8_t>(
                (blue + static_cast<uint32_t>(half)) / static_cast<uint32_t>(blocks)
            );
        }
    }
    return true;
}

} // namespace oa::present::photo
