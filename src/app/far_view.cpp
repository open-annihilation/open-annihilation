// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/far_view.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

namespace oa::app {

namespace {

namespace tnt_layout = oa::formats::tnt::layout;

/// Map pixels across a tile.
constexpr uint32_t tile_edge = tnt_layout::tile_edge_pixels;

/// 16.16 fixed-point one: a step of one destination pixel.
constexpr uint32_t fixed_one = 65536U;

/// Destination rows each band of filter_far_terrain averages.
constexpr uint32_t far_band_rows = 32;

/// Returns the texels of one tile's level: its edge squared.
///
/// @param level the level, first_pyramid_level to last_pyramid_level
/// @return texels
[[nodiscard]] constexpr std::size_t level_texels(uint32_t level) noexcept {
    const std::size_t edge = tile_edge >> level;
    return edge * edge;
}

/// Returns where a level starts within a tile's texels.
///
/// @param level the level, first_pyramid_level to last_pyramid_level
/// @return texels before it
[[nodiscard]] constexpr std::size_t level_offset(uint32_t level) noexcept {
    std::size_t offset = 0;
    for (uint32_t before = first_pyramid_level; before < level; ++before)
        offset += level_texels(before);
    return offset;
}

static_assert(level_offset(last_pyramid_level + 1U) == pyramid_tile_texels);
static_assert((tile_edge >> last_pyramid_level) == 1U, "the last level holds a texel a tile");

/// Lists the first texel of each destination column (or row) and one past
/// the last: the texels whose centres lie in the map pixels the nearest
/// fill's steps give the column, at 16.16 screen pixels per map pixel.
///
/// @param[out] bounds `count` + 1 texel coordinates
/// @param start map coordinate of the first destination pixel; below 0
///        before the map, where every column (or row) holds no texel
/// @param count destination pixels
/// @param scale_fp the zoom in 16.16, at least 1
/// @param level the pyramid level
/// @param phase how far into map pixel `start` the first destination pixel
///        starts, 16.16 parts of scale_fp, below it
void texel_bounds(
    std::vector<uint32_t>& bounds,
    int32_t start,
    int32_t count,
    uint32_t scale_fp,
    uint32_t level,
    uint32_t phase
) {
    bounds.resize(static_cast<std::size_t>(count) + 1U);
    const int64_t half = int64_t{1} << (level - 1U);
    int64_t position = start;
    uint32_t fraction = phase % scale_fp;
    for (auto& bound : bounds) {
        // The first texel whose centre, at (texel + 1/2) * 2^level, is at or
        // past the map pixel; the map's first texel for a pixel before it.
        bound = static_cast<uint32_t>(std::max<int64_t>((position + half - 1) >> level, 0));
        fraction += fixed_one;
        while (fraction >= scale_fp) {
            fraction -= scale_fp;
            ++position;
        }
    }
}

/// What the bands of filter_far_terrain read and write.
struct FarFill {
    const oa::formats::tnt::Map* map{};
    const uint8_t* level_texels{}; ///< the pyramid's first byte of the level in tile 0
    uint32_t texel_edge{};         ///< the level's texels across a tile
    std::size_t tile_stride{};     ///< bytes from one tile's levels to the next's
    uint32_t texels_wide{};        ///< the level's texels across the shown map
    uint32_t texels_high{};        ///< and down it
    const std::vector<uint32_t>* columns{};
    const std::vector<uint32_t>* rows{};
    uint32_t dest_width{};
    uint8_t* dest_rgb{};
};

/// Averages destination rows [first_row, end_row).
///
/// @param fill what the bands read and write
/// @param first_row the first row
/// @param end_row the row after the last
/// @param[in,out] sums dest_width * 3 sums, overwritten
/// @return false at a tile the map's grid names but its table lacks
bool fill_far_rows(
    const FarFill& fill, uint32_t first_row, uint32_t end_row, uint32_t* sums
) noexcept {
    const auto& map = *fill.map;
    const auto& columns = *fill.columns;
    const auto& rows = *fill.rows;
    const uint16_t* const grid = map.tile_indices.data();
    const uint32_t edge = fill.texel_edge;
    const uint32_t texel_shift = static_cast<uint32_t>(std::countr_zero(edge));
    const uint32_t column_end = std::min(columns[fill.dest_width], fill.texels_wide);
    for (uint32_t dy = first_row; dy < end_row; ++dy) {
        const uint32_t row_begin = rows[dy];
        const uint32_t row_end = rows[dy + 1U];
        std::fill(sums, sums + static_cast<std::size_t>(fill.dest_width) * 3U, 0U);
        for (uint32_t ty = row_begin; ty < row_end && ty < fill.texels_high; ++ty) {
            const auto row_tiles = static_cast<std::size_t>(ty >> texel_shift) * map.tile_width;
            const auto within_y = static_cast<std::size_t>(ty & (edge - 1U)) * edge;
            uint32_t column = 0;
            uint32_t next_column = columns[1];
            uint32_t* sum = sums;
            for (uint32_t tx = columns[0]; tx < column_end; ++tx) {
                while (tx >= next_column) {
                    ++column;
                    sum += 3;
                    next_column = columns[column + 1U];
                }
                const uint16_t tile = grid[row_tiles + (tx >> texel_shift)];
                if (tile >= map.tile_count)
                    return false;
                const uint8_t* rgb = fill.level_texels +
                                     static_cast<std::size_t>(tile) * fill.tile_stride +
                                     (within_y + (tx & (edge - 1U))) * pyramid_texel_bytes;
                sum[0] += rgb[0];
                sum[1] += rgb[1];
                sum[2] += rgb[2];
            }
        }
        const uint32_t row_count = row_end - row_begin;
        uint8_t* out = fill.dest_rgb + static_cast<std::size_t>(dy) * fill.dest_width * 3U;
        const uint32_t* summed = sums;
        for (uint32_t dx = 0; dx < fill.dest_width; ++dx, out += 3, summed += 3) {
            const uint32_t count = row_count * (columns[dx + 1U] - columns[dx]);
            if (count == 0) {
                std::memset(out, 0, 3);
                continue;
            }
            out[0] = static_cast<uint8_t>((summed[0] + count / 2U) / count);
            out[1] = static_cast<uint8_t>((summed[1] + count / 2U) / count);
            out[2] = static_cast<uint8_t>((summed[2] + count / 2U) / count);
        }
    }
    return true;
}

} // namespace

float whole_map_zoom(
    int32_t map_width, int32_t map_height, int32_t battlefield_width, int32_t battlefield_height
) noexcept {
    if (map_width <= 0 || map_height <= 0 || battlefield_width <= 0 || battlefield_height <= 0)
        return 0.0F;
    return static_cast<float>(std::min(
        static_cast<double>(battlefield_width) / static_cast<double>(map_width),
        static_cast<double>(battlefield_height) / static_cast<double>(map_height)
    ));
}

float least_battlefield_zoom(
    oa::ui::engine_settings::ZoomOutLimit limit,
    float automatic_floor,
    int32_t map_width,
    int32_t map_height,
    int32_t battlefield_width,
    int32_t battlefield_height
) noexcept {
    const auto share = oa::ui::engine_settings::zoom_out_share(limit);
    if (!share)
        return automatic_floor;
    const float fit = whole_map_zoom(map_width, map_height, battlefield_width, battlefield_height);
    if (!(fit > 0.0F))
        return automatic_floor;
    return std::min(1.0F, std::max({*share, fit, furthest_battlefield_zoom}));
}

ViewCentreSpan view_centre_span(double map, double visible, double share) noexcept {
    // No wider than the map, the view's centre keeps the rest of the view's
    // half on the map's side of each edge; wider, the map's centre keeps
    // within the share of the view of the view's centre.
    if (visible <= map) {
        const double inside = (0.5 - share) * visible;
        return {inside, map - inside};
    }
    const double middle = map / 2.0;
    const double reach = share * visible;
    return {middle - reach, middle + reach};
}

double held_view(
    double view, double visible, double map, double share, std::optional<double> from
) noexcept {
    auto span = view_centre_span(map, visible, share);
    if (from) {
        span.least = std::min(span.least, *from);
        span.most = std::max(span.most, *from);
    }
    const double half_view = visible / 2.0;
    return std::clamp(view + half_view, span.least, span.most) - half_view;
}

int32_t held_camera(
    int32_t camera, double visible, double map, double share, std::optional<double> from
) noexcept {
    const auto exact = static_cast<double>(camera);
    const double held = held_view(exact, visible, map, share, from);
    // A camera is taken from the view's exact place, to the nearest whole
    // map pixel or the one before it: within a map pixel of a place the
    // limits hold, it is held already.
    if (std::abs(held - exact) < 1.0)
        return camera;
    // Rounded toward the inside of the limits, so that the camera stays
    // within them.
    return static_cast<int32_t>(held > exact ? std::ceil(held) : std::floor(held));
}

TerrainPyramid
build_terrain_pyramid(const oa::formats::tnt::Map& map, const oa::PaletteBytes& palette) {
    TerrainPyramid pyramid;
    const auto tiles = static_cast<std::size_t>(map.tile_count);
    if (map.tile_palette_indices.size() < tiles * tnt_layout::tile_bytes)
        return pyramid;
    pyramid.tile_count = map.tile_count;
    pyramid.rgb.resize(tiles * pyramid_tile_texels * pyramid_texel_bytes);
    // Each level's sums of the map pixels its texels cover, channel by
    // channel, for one tile at a time: level 0 is the tile's own pixels.
    std::array<uint32_t, tile_edge * tile_edge * 3U> sums{};
    std::array<uint32_t, tile_edge * tile_edge * 3U> next{};
    for (std::size_t tile = 0; tile < tiles; ++tile) {
        const uint8_t* pixels = map.tile_palette_indices.data() + tile * tnt_layout::tile_bytes;
        for (std::size_t pixel = 0; pixel < tnt_layout::tile_bytes; ++pixel) {
            const std::size_t entry = static_cast<std::size_t>(pixels[pixel]) * 4U;
            sums[pixel * 3U] = palette[entry];
            sums[pixel * 3U + 1U] = palette[entry + 1U];
            sums[pixel * 3U + 2U] = palette[entry + 2U];
        }
        uint8_t* out = pyramid.rgb.data() + tile * pyramid_tile_texels * pyramid_texel_bytes;
        for (uint32_t level = first_pyramid_level; level <= last_pyramid_level; ++level) {
            const uint32_t edge = tile_edge >> level;
            const uint32_t previous = edge * 2U;
            // A texel of this level covers 4 to the level map pixels.
            const uint32_t covered = 1U << (2U * level);
            for (uint32_t y = 0; y < edge; ++y)
                for (uint32_t x = 0; x < edge; ++x)
                    for (uint32_t channel = 0; channel < 3U; ++channel) {
                        const auto at = [&](uint32_t px, uint32_t py) {
                            return sums[(py * previous + px) * 3U + channel];
                        };
                        const uint32_t sum = at(2U * x, 2U * y) + at(2U * x + 1U, 2U * y) +
                                             at(2U * x, 2U * y + 1U) + at(2U * x + 1U, 2U * y + 1U);
                        next[(y * edge + x) * 3U + channel] = sum;
                        out[(level_offset(level) + y * edge + x) * 3U + channel] =
                            static_cast<uint8_t>((sum + covered / 2U) / covered);
                    }
            std::copy_n(next.begin(), static_cast<std::size_t>(edge) * edge * 3U, sums.begin());
        }
    }
    return pyramid;
}

uint8_t far_terrain_level(float zoom) noexcept {
    if (!(zoom > 0.0F))
        return last_pyramid_level;
    const double level = std::floor(std::log2(1.0 / static_cast<double>(zoom)));
    return static_cast<uint8_t>(std::clamp(
        level, static_cast<double>(first_pyramid_level), static_cast<double>(last_pyramid_level)
    ));
}

bool filter_far_terrain(
    const oa::formats::tnt::Map& map,
    const TerrainPyramid& pyramid,
    int32_t source_x,
    int32_t source_y,
    uint32_t shown_width,
    uint32_t shown_height,
    int32_t dest_width,
    int32_t dest_height,
    float zoom,
    uint8_t* dest_rgb,
    oa::platform::job_pool::Pool* pool,
    uint32_t phase_x,
    uint32_t phase_y
) {
    if (dest_width <= 0 || dest_height <= 0)
        return true;
    if (dest_rgb == nullptr || pyramid.tile_count != map.tile_count ||
        pyramid.rgb.size() !=
            static_cast<std::size_t>(map.tile_count) * pyramid_tile_texels * pyramid_texel_bytes ||
        static_cast<uint64_t>(map.tile_width) * map.tile_height != map.tile_indices.size())
        return false;
    const uint32_t level = far_terrain_level(zoom);
    const uint32_t texel_edge = tile_edge >> level;
    auto scale_fp = static_cast<uint32_t>(std::lround(static_cast<double>(zoom) * fixed_one));
    if (scale_fp == 0)
        scale_fp = 1;
    // The texels of the shown map: those whose centres lie on it, within
    // the mosaic.
    const auto shown_texels = [&](uint64_t shown, uint32_t tiles) {
        const uint64_t pixels = std::min<uint64_t>(uint64_t{tiles} * tile_edge, shown);
        return static_cast<uint32_t>((pixels + (uint64_t{1} << (level - 1U)) - 1U) >> level);
    };
    std::vector<uint32_t> columns;
    std::vector<uint32_t> rows;
    texel_bounds(columns, source_x, dest_width, scale_fp, level, phase_x);
    texel_bounds(rows, source_y, dest_height, scale_fp, level, phase_y);
    const FarFill fill{
        &map,
        pyramid.rgb.data() + level_offset(level) * pyramid_texel_bytes,
        texel_edge,
        pyramid_tile_texels * pyramid_texel_bytes,
        shown_texels(shown_width, map.tile_width),
        shown_texels(shown_height, map.tile_height),
        &columns,
        &rows,
        static_cast<uint32_t>(dest_width),
        dest_rgb
    };
    const uint32_t bands =
        oa::platform::job_pool::bands_of_rows(static_cast<uint32_t>(dest_height), far_band_rows);
    // Each band's sums, so that the bands share nothing they write.
    std::vector<uint32_t> sums(static_cast<std::size_t>(bands) * fill.dest_width * 3U);
    std::atomic<bool> malformed{false};
    oa::platform::job_pool::run_bands(pool, bands, [&](uint32_t band) {
        if (malformed.load(std::memory_order_relaxed))
            return;
        const uint32_t first_row = band * far_band_rows;
        const uint32_t end_row =
            std::min(static_cast<uint32_t>(dest_height), first_row + far_band_rows);
        if (!fill_far_rows(
                fill,
                first_row,
                end_row,
                sums.data() + static_cast<std::size_t>(band) * fill.dest_width * 3U
            ))
            malformed.store(true, std::memory_order_relaxed);
    });
    return !malformed.load(std::memory_order_relaxed);
}

void draw_far_view_dots(
    uint8_t* rgb, int32_t width, int32_t height, std::span<const FarViewDot> dots
) noexcept {
    if (rgb == nullptr || width <= 0 || height <= 0)
        return;
    const auto fill =
        [&](int32_t left, int32_t top, int32_t side, const std::array<uint8_t, 3>& c) {
            const int32_t x0 = std::max(left, 0);
            const int32_t y0 = std::max(top, 0);
            const int32_t x1 = std::min(left + side, width);
            const int32_t y1 = std::min(top + side, height);
            for (int32_t y = y0; y < y1; ++y) {
                uint8_t* out =
                    rgb + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x0)) *
                              3U;
                for (int32_t x = x0; x < x1; ++x, out += 3)
                    std::memcpy(out, c.data(), 3);
            }
        };
    constexpr int32_t half = far_view_dot_side / 2;
    const auto border =
        [&](int32_t left, int32_t top, int32_t side, const std::array<uint8_t, 3>& c) {
            if (side <= 0)
                return;
            const auto stroke = [&](int32_t x, int32_t y) {
                if (x < 0 || y < 0 || x >= width || y >= height)
                    return;
                std::memcpy(
                    rgb + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x)) *
                              3U,
                    c.data(),
                    3
                );
            };
            for (int32_t x = left; x < left + side; ++x) {
                stroke(x, top);
                stroke(x, top + side - 1);
            }
            for (int32_t y = top; y < top + side; ++y) {
                stroke(left, y);
                stroke(left + side - 1, y);
            }
        };
    for (const FarViewDot& dot : dots) {
        // A seat is a compact marker, already stamped by the caller. The
        // square would hide it. The selection's frame is one pixel outside
        // the 7-pixel marker, centred on the marker as it was stamped.
        if (dot.player >= 0 && dot.player < 10) {
            constexpr int32_t frame_side = 9;
            if (dot.framed)
                border(
                    dot.x - frame_side / 2, dot.y - frame_side / 2, frame_side, dot.frame_colour
                );
            continue;
        }
        if (dot.framed)
            fill(dot.x - half - 1, dot.y - half - 1, far_view_dot_side + 2, dot.frame_colour);
        fill(dot.x - half, dot.y - half, far_view_dot_side, dot.colour);
    }
}

} // namespace oa::app
