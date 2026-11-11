// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which poster fits the view, the marked area or the whole map, and the
// strips a processor draws that poster in.
#include "oa/present/photo/photo_poster.hpp"

#include "oa/present/photo/photo_tiles.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace oa::present::photo {
namespace {

/// Draw rows advance by this many map rows. A camera that moves by a
/// multiple of it keeps each unit on the row a photo of that ground uses.
constexpr int32_t kRowPhase = 16;

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

/// The pixels a Full world target holds: its texture, and the halves the
/// reduction reads. The same count the Full tier uses.
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

/// True when the area is a picture that fits in a count of pixels.
///
/// @param area the rectangle
/// @return false when a side is empty or the product does not fit
bool area_pixels(const PosterArea& area) {
    if (area.width <= 0 || area.height <= 0)
        return false;
    const int64_t pixels = int64_t{area.width} * int64_t{area.height};
    return pixels > 0 && pixels <= static_cast<int64_t>(std::numeric_limits<int32_t>::max());
}

/// The view's rectangle. A view with no pixels is empty.
///
/// @param ask the camera and the view
/// @return the rectangle
PosterArea view_area(const PosterAsk& ask) {
    PosterArea area;
    area.x = ask.camera_x;
    area.z = ask.camera_z;
    area.width = ask.view_width;
    area.height = ask.view_height;
    return area;
}

/// The marked rectangle, or the view when the mark is empty.
///
/// @param ask the mark and the view
/// @return the rectangle
PosterArea marked_area(const PosterAsk& ask) {
    if (ask.marked.width <= 0 || ask.marked.height <= 0)
        return view_area(ask);
    return ask.marked;
}

/// The whole map, from its origin.
///
/// @param ask the map
/// @return the rectangle
PosterArea map_area(const PosterAsk& ask) {
    PosterArea area;
    area.x = 0;
    area.z = 0;
    area.width = ask.map_width;
    area.height = ask.map_height;
    return area;
}

/// The bytes a Full poster asks of the card when it is drawn in one piece.
///
/// @param width the poster
/// @param height the poster
/// @param machine the factor and the texel size
/// @param[out] past_card true when the target passes the card
/// @return the card bytes
uint64_t one_piece_gpu(int32_t width, int32_t height, const Machine& machine, bool& past_card) {
    past_card = false;
    const int supersample = std::max(machine.supersample, 1);
    const uint64_t bytes = static_cast<uint64_t>(std::max(machine.bytes_per_texel, 1));
    uint64_t gpu = saturating_mul(
        saturating_mul(
            static_cast<uint64_t>(std::max(width, int32_t{0})),
            static_cast<uint64_t>(std::max(height, int32_t{0}))
        ),
        bytes
    );
    if (supersample <= 1)
        return gpu;
    const int32_t target_w = rounded_up(width, std::max(machine.grain, 1));
    const int32_t target_h = rounded_up(height, std::max(machine.grain, 1));
    const int64_t texture_edge = int64_t{std::max(target_w, target_h)} * supersample;
    if (machine.edge_limit > 0 && texture_edge > machine.edge_limit)
        past_card = true;
    const uint64_t world = world_target_pixels(
        static_cast<uint32_t>(std::max(target_w, int32_t{0})),
        static_cast<uint32_t>(std::max(target_h, int32_t{0})),
        static_cast<uint32_t>(supersample)
    );
    return saturating_add(gpu, saturating_mul(world, bytes));
}

/// The bytes a tiled Full poster asks for: the assembled poster until its
/// file is written, and the largest tile's read and targets. One tile is
/// drawn at a time.
///
/// @param plan the tiles
/// @param machine the factor and the texel size
/// @param width the poster
/// @param height the poster
/// @return the processor bytes and the card bytes
std::pair<uint64_t, uint64_t>
tile_cost(const TilePlan& plan, const Machine& machine, int32_t width, int32_t height) {
    const uint64_t picture = saturating_mul(
        saturating_mul(
            static_cast<uint64_t>(std::max(width, int32_t{0})),
            static_cast<uint64_t>(std::max(height, int32_t{0}))
        ),
        kPosterBytesPerPixel
    );
    const int supersample = std::max(machine.supersample, 1);
    const uint64_t bytes = static_cast<uint64_t>(std::max(machine.bytes_per_texel, 1));
    uint64_t tile_cpu = 0;
    uint64_t tile_gpu = 0;
    for (const Tile& tile : plan.tiles) {
        const uint64_t tile_area = saturating_mul(
            static_cast<uint64_t>(std::max(tile.draw_width, int32_t{0})),
            static_cast<uint64_t>(std::max(tile.draw_height, int32_t{0}))
        );
        tile_cpu = std::max(tile_cpu, saturating_mul(tile_area, uint64_t{6}));
        uint64_t gpu = saturating_mul(tile_area, bytes);
        if (supersample > 1) {
            const uint64_t world = world_target_pixels(
                static_cast<uint32_t>(std::max(tile.target_width, int32_t{0})),
                static_cast<uint32_t>(std::max(tile.target_height, int32_t{0})),
                static_cast<uint32_t>(supersample)
            );
            gpu = saturating_add(gpu, saturating_mul(world, bytes));
        }
        tile_gpu = std::max(tile_gpu, gpu);
    }
    return {saturating_add(picture, tile_cpu), tile_gpu};
}

/// The tiles of one Full poster at zoom 1.
///
/// @param area the poster
/// @param machine the card and the check's tile edge
/// @return the tiles
TilePlan tiles_of(const PosterArea& area, const Machine& machine) {
    TileRequest request;
    request.width = area.width;
    request.height = area.height;
    request.zoom = 1.0F;
    request.supersample = std::max(machine.supersample, 1);
    request.grain = std::max(machine.grain, 1);
    request.texture_limit = std::max(machine.edge_limit, int32_t{0});
    request.tile_edge = std::max(machine.tile_edge, int32_t{0});
    request.reach = full_tile_reach(1.0F);
    return plan_tiles(request);
}

/// Why one extent is not offered, after the button's own lock has been passed.
struct ExtentNeed {
    bool edge{false};
    bool memory{false};
    int32_t limit{kProcessorEdge};
};

/// Judges one poster. The picture is the area, at one pixel a map pixel.
///
/// @param area the rectangle
/// @param machine the mode and the memory
/// @return why it might be refused
ExtentNeed judge_extent(const PosterArea& area, const Machine& machine) {
    ExtentNeed need;
    if (!area_pixels(area)) {
        need.edge = true;
        return need;
    }
    if (std::max(area.width, area.height) > kProcessorEdge)
        need.edge = true;

    uint64_t cpu = saturating_mul(
        saturating_mul(static_cast<uint64_t>(area.width), static_cast<uint64_t>(area.height)),
        kPosterBytesPerPixel
    );
    uint64_t gpu = 0;
    if (machine.kind == CaptureKind::full && !need.edge) {
        const TilePlan tiles = tiles_of(area, machine);
        const bool whole = draws_whole(tiles, area.width, area.height);
        const bool forced = machine.tile_edge > 0 &&
                            (area.width > machine.tile_edge || area.height > machine.tile_edge);
        if (whole && !forced) {
            bool past_card = false;
            gpu = one_piece_gpu(area.width, area.height, machine, past_card);
            if (past_card)
                need.edge = true;
        } else if (tiles.fits) {
            const std::pair<uint64_t, uint64_t> cost =
                tile_cost(tiles, machine, area.width, area.height);
            cpu = cost.first;
            gpu = cost.second;
        } else
            need.edge = true;
    }

    cpu = saturating_add(
        cpu,
        saturating_add(
            saturating_mul(
                saturating_mul(
                    static_cast<uint64_t>(area.width), static_cast<uint64_t>(area.height)
                ),
                static_cast<uint64_t>(std::max(machine.look_bytes_per_pixel, 0))
            ),
            machine.look_bytes
        )
    );
    if (!need.edge && cpu > machine.cpu_bytes)
        need.memory = true;
    if (!need.edge && machine.kind == CaptureKind::full && gpu > machine.gpu_bytes)
        need.memory = true;
    return need;
}

/// An extent locked for the button's reason, with its rectangle filled in.
///
/// @param extent which area
/// @param area the rectangle
/// @param lock the button's lock
/// @param limit the cap, when the lock is the edge
/// @return the offer
PosterOffer locked_offer(PosterExtent extent, PosterArea area, SizeLock lock, int32_t limit) {
    PosterOffer offer;
    offer.extent = extent;
    offer.area = area;
    offer.lock = lock;
    offer.limit = limit;
    return offer;
}

/// An extent judged on its own.
///
/// @param extent which area
/// @param area the rectangle
/// @param machine the mode and the memory
/// @return the offer
PosterOffer judged_offer(PosterExtent extent, PosterArea area, const Machine& machine) {
    PosterOffer offer;
    offer.extent = extent;
    offer.area = area;
    const ExtentNeed need = judge_extent(area, machine);
    offer.limit = need.limit;
    if (need.edge) {
        offer.lock = SizeLock::edge;
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

/// A sum of two map coordinates, or false when it does not fit.
///
/// @param base one coordinate
/// @param delta the other
/// @param[out] sum the sum
/// @return false when the sum does not fit
bool add_coord(int32_t base, int32_t delta, int32_t& sum) {
    const int64_t wide = int64_t{base} + int64_t{delta};
    if (wide < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        wide > static_cast<int64_t>(std::numeric_limits<int32_t>::max()))
        return false;
    sum = static_cast<int32_t>(wide);
    return true;
}

/// How many map pixels one strip keeps across.
///
/// @param remaining pixels still to cover
/// @param span the view, or 0 for the whole remainder
/// @return the strip
int32_t across_span(int32_t remaining, int32_t span) {
    if (span <= 0 || span >= remaining)
        return remaining;
    return span;
}

/// How many map pixels one strip keeps down. Every strip but the last is a
/// whole number of 16-pixel rows, so the next camera stays on the phase.
///
/// @param remaining pixels still to cover
/// @param span the view, or 0 for the whole remainder
/// @return the strip
int32_t down_span(int32_t remaining, int32_t span) {
    if (span <= 0 || span >= remaining)
        return remaining;
    if (span >= kRowPhase)
        return (span / kRowPhase) * kRowPhase;
    return span;
}

} // namespace

PosterPlan plan_poster(const PosterAsk& ask, const Machine& machine) {
    PosterPlan plan;
    plan.limit = kProcessorEdge;
    const PosterArea shown = view_area(ask);
    const PosterArea marked = marked_area(ask);
    const PosterArea whole = map_area(ask);

    SizeLock button = SizeLock::open;
    if (machine.shared)
        button = SizeLock::shared;
    else if (machine.kind == CaptureKind::turned)
        button = SizeLock::above;
    else if (ask.held)
        button = SizeLock::held;
    else if (!ask.frozen)
        button = SizeLock::moving;
    plan.button = button;

    if (button != SizeLock::open) {
        plan.view = locked_offer(PosterExtent::view, shown, button, plan.limit);
        plan.marked = locked_offer(PosterExtent::marked, marked, button, plan.limit);
        plan.map = locked_offer(PosterExtent::map, whole, button, plan.limit);
        return plan;
    }
    plan.view = judged_offer(PosterExtent::view, shown, machine);
    plan.marked = judged_offer(PosterExtent::marked, marked, machine);
    plan.map = judged_offer(PosterExtent::map, whole, machine);
    return plan;
}

std::vector<PosterStrip>
poster_strips(const PosterArea& area, int32_t view_width, int32_t view_height) {
    std::vector<PosterStrip> strips;
    if (!area_pixels(area))
        return strips;
    int32_t covered_down = 0;
    while (covered_down < area.height) {
        const int32_t strip_height = down_span(area.height - covered_down, view_height);
        if (strip_height <= 0)
            return {};
        int32_t row = 0;
        if (!add_coord(area.z, covered_down, row))
            return {};
        int32_t covered_across = 0;
        while (covered_across < area.width) {
            const int32_t strip_width = across_span(area.width - covered_across, view_width);
            if (strip_width <= 0)
                return {};
            int32_t column = 0;
            if (!add_coord(area.x, covered_across, column))
                return {};
            PosterStrip strip;
            strip.camera_x = column;
            strip.camera_z = row;
            strip.width = strip_width;
            strip.height = strip_height;
            strip.dest_x = covered_across;
            strip.dest_y = covered_down;
            // A strip that is not on the area's phase is drawn from the
            // phase and the pad rows are skipped. The spans above keep
            // every strip on the phase; this still holds a short view.
            const int32_t from_origin = row - area.z;
            int32_t phase = from_origin % kRowPhase;
            if (phase < 0)
                phase += kRowPhase;
            if (phase != 0) {
                int32_t snapped = 0;
                if (!add_coord(row, -phase, snapped))
                    return {};
                strip.camera_z = snapped;
                strip.skip_rows = phase;
            }
            strips.push_back(strip);
            if (covered_across > std::numeric_limits<int32_t>::max() - strip_width)
                return {};
            covered_across += strip_width;
        }
        if (covered_down > std::numeric_limits<int32_t>::max() - strip_height)
            return {};
        covered_down += strip_height;
    }
    return strips;
}

} // namespace oa::present::photo
