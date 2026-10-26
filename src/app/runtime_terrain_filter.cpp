// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Area-filtered terrain for zoomed-out battlefield views.
#include "oa/app/runtime.hpp"
#include "oa/app/far_view.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace oa::app {
namespace {

constexpr float kFilteredZoomLimit = 1.0F - 1.0e-4F;
constexpr uint32_t kFixedOne = 65536u;
/// Destination rows each band of box_filter_terrain averages.
constexpr uint32_t kFilterBandRows = 32;

/// Lists the map coordinate under each destination column (or row) plus one past the end.
///
/// Stepped with the terrain sampler's fixed-point DDA so every footprint
/// shares the nearest sampler's and the fog's tile boundaries.
///
/// @param[out] bounds `count` + 1 map coordinates; below 0 before the map
/// @param start map coordinate of the first destination pixel
/// @param count destination pixels
/// @param scale_fp zoom in 16.16, at least 1
/// @param phase how far into map pixel `start` the first destination pixel
///        starts, 16.16 parts of scale_fp, below it (scene_grid.hpp's phase)
void footprint_bounds(
    std::vector<int64_t>& bounds, int32_t start, int count, uint32_t scale_fp, uint32_t phase
) {
    bounds.resize(static_cast<std::size_t>(count) + 1U);
    int64_t position = start;
    uint32_t fraction = phase % scale_fp;
    for (auto& bound : bounds) {
        bound = position;
        fraction += kFixedOne;
        while (fraction >= scale_fp) {
            fraction -= scale_fp;
            ++position;
        }
    }
}

/// Averages every map pixel under each destination pixel's footprint.
///
/// Ground beyond the terrain, and before it, counts as black, as the
/// nearest sampler paints it. Each visible map pixel is read once, so the
/// cost is the source area in view. The rows are averaged in bands of
/// kFilterBandRows, each band's pixels the same on whichever thread runs it.
///
/// @param map terrain
/// @param palette palette the tiles' indices are shown in
/// @param source_x map column of the first destination pixel; below 0 left of the map
/// @param source_y map row of the first destination pixel; below 0 above the map
/// @param shown_width map pixels across the view may show, the mosaic's or fewer
/// @param shown_height map pixels down the view may show, the mosaic's or fewer
/// @param dest_w destination width
/// @param dest_h destination height
/// @param zoom destination pixels per map pixel (the draw scale), below 1
/// @param[out] dest_rgb 3 bytes per destination pixel
/// @param phase_x how far into map column source_x the first destination
///        column starts, 16.16 parts of the zoom's step, below it
/// @param phase_y the same for map row source_y and the first destination row
/// @param pool threads to average the bands on; null averages them on the calling thread
/// @return false when the terrain's tile tables are malformed or name a missing tile
bool box_filter_terrain(
    const oa::formats::tnt::Map& map,
    const oa::PaletteBytes& palette,
    int32_t source_x,
    int32_t source_y,
    uint32_t shown_width,
    uint32_t shown_height,
    int dest_w,
    int dest_h,
    float zoom,
    uint8_t* dest_rgb,
    uint32_t phase_x,
    uint32_t phase_y,
    oa::platform::job_pool::Pool* pool
) {
    constexpr auto tile_edge = static_cast<uint32_t>(oa::formats::tnt::layout::tile_edge_pixels);
    constexpr auto tile_bytes = oa::formats::tnt::layout::tile_bytes;
    const auto grid_cells = static_cast<uint64_t>(map.tile_width) * map.tile_height;
    const auto tile_pixels = static_cast<uint64_t>(map.tile_count) * tile_bytes;
    if (grid_cells != map.tile_indices.size() || tile_pixels != map.tile_palette_indices.size())
        return false;
    // The map the view shows ends at the shown size, within the mosaic;
    // past it the filter sums nothing, as past the mosaic.
    const auto terrain_w = std::min<int64_t>(
        static_cast<int64_t>(map.tile_width) * tile_edge, static_cast<int64_t>(shown_width)
    );
    const auto terrain_h = std::min<int64_t>(
        static_cast<int64_t>(map.tile_height) * tile_edge, static_cast<int64_t>(shown_height)
    );
    auto scale_fp = static_cast<uint32_t>(std::lround(static_cast<double>(zoom) * kFixedOne));
    if (scale_fp == 0)
        scale_fp = 1;
    std::array<uint8_t, 256U * 3U> lut{};
    for (std::size_t index = 0; index < 256U; ++index)
        std::memcpy(&lut[index * 3U], &palette[index * oa::palette_entry_bytes], 3U);
    std::vector<int64_t> columns;
    std::vector<int64_t> rows;
    footprint_bounds(columns, source_x, dest_w, scale_fp, phase_x);
    footprint_bounds(rows, source_y, dest_h, scale_fp, phase_y);
    const uint16_t* const grid = map.tile_indices.data();
    const uint8_t* const tiles = map.tile_palette_indices.data();
    const int64_t column_end = std::min(columns[static_cast<std::size_t>(dest_w)], terrain_w);
    const auto sums_per_row = static_cast<std::size_t>(dest_w) * 3U;
    // Averages one destination row, summed in `totals`, into its pixels;
    // false at a tile the grid names that the map does not hold.
    const auto filter_row = [&](int dy, uint32_t* totals) {
        const int64_t row_begin = rows[static_cast<std::size_t>(dy)];
        const int64_t row_end = rows[static_cast<std::size_t>(dy) + 1U];
        std::fill(totals, totals + sums_per_row, 0u);
        // Rows and columns before the map sum nothing, as those past it.
        for (int64_t sy = std::max<int64_t>(row_begin, 0); sy < row_end && sy < terrain_h; ++sy) {
            const auto row_tiles = static_cast<std::size_t>(sy / tile_edge) * map.tile_width;
            const auto within_y = static_cast<std::size_t>(sy % tile_edge) * tile_edge;
            std::size_t column = 0;
            int64_t next_column = columns[1];
            uint32_t* sum = totals;
            int64_t sx = std::max<int64_t>(columns[0], 0);
            while (sx < column_end) {
                const auto tile_index = grid[row_tiles + static_cast<std::size_t>(sx / tile_edge)];
                if (tile_index >= map.tile_count)
                    return false;
                const int64_t within_x = sx % tile_edge;
                const int64_t run_end = std::min<int64_t>(column_end, sx - within_x + tile_edge);
                const uint8_t* source = tiles + static_cast<std::size_t>(tile_index) * tile_bytes +
                                        within_y + static_cast<std::size_t>(within_x);
                for (; sx < run_end; ++sx, ++source) {
                    while (sx >= next_column) {
                        ++column;
                        sum += 3;
                        next_column = columns[column + 1U];
                    }
                    const uint8_t* rgb = &lut[static_cast<std::size_t>(*source) * 3U];
                    sum[0] += rgb[0];
                    sum[1] += rgb[1];
                    sum[2] += rgb[2];
                }
            }
        }
        const auto row_count = static_cast<uint32_t>(row_end - row_begin);
        uint8_t* out =
            dest_rgb + static_cast<std::size_t>(dy) * static_cast<std::size_t>(dest_w) * 3U;
        const uint32_t* sum = totals;
        for (int dx = 0; dx < dest_w; ++dx, out += 3, sum += 3) {
            const auto index = static_cast<std::size_t>(dx);
            const uint32_t count =
                row_count * static_cast<uint32_t>(columns[index + 1U] - columns[index]);
            out[0] = static_cast<uint8_t>((sum[0] + count / 2U) / count);
            out[1] = static_cast<uint8_t>((sum[1] + count / 2U) / count);
            out[2] = static_cast<uint8_t>((sum[2] + count / 2U) / count);
        }
        return true;
    };
    const uint32_t bands =
        oa::platform::job_pool::bands_of_rows(static_cast<uint32_t>(dest_h), kFilterBandRows);
    // Each band's sums, so that the bands share nothing they write.
    std::vector<uint32_t> sums(static_cast<std::size_t>(bands) * sums_per_row);
    std::atomic<bool> malformed{false};
    oa::platform::job_pool::run_bands(pool, bands, [&](uint32_t band) {
        uint32_t* band_sums = sums.data() + static_cast<std::size_t>(band) * sums_per_row;
        const auto first_row = static_cast<int>(band * kFilterBandRows);
        const int end_row = std::min(dest_h, first_row + static_cast<int>(kFilterBandRows));
        for (int dy = first_row; dy < end_row; ++dy) {
            if (malformed.load(std::memory_order_relaxed))
                return;
            if (!filter_row(dy, band_sums)) {
                malformed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });
    return !malformed.load(std::memory_order_relaxed);
}

} // namespace

void Runtime::refresh_filtered_terrain() {
    if (screen_ != Screen::match || !match_ || !selected_tnt_)
        return;
    // The Full tier's zoomed-out terrain is the card's, from the atlas
    // levels: its base stays the nearest fill, and the box filter never runs.
    // The far view is the processor's in every tier.
    const bool far = far_view_frame();
    if (full_presentation() && !far)
        return;
    // The scene's terrain, at the draw scale: below one pixel per map pixel
    // only, so a scene drawn 1:1 or magnified keeps the nearest fill.
    const auto scaling = world_scaling();
    const auto draw_scale = scaling.draw_scale;
    if (!(draw_scale > 0.0F) || draw_scale >= kFilteredZoomLimit)
        return;
    const auto& map = *selected_tnt_;
    const auto [map_width, map_height] = shown_map_size();
    const int dest_w = scaling.scene_width;
    const int dest_h = scaling.scene_height;
    if (dest_w <= 0 || dest_h <= 0)
        return;
    // The renderer holds the camera the same way before it checks its cache,
    // and lays the scene from the view's phase.
    const auto [camera_x, camera_y] = view_camera();
    const auto phase = scene_phase(scaling);
    auto& cache = match_terrain_cache_;
    const auto pixels = static_cast<std::size_t>(dest_w) * static_cast<std::size_t>(dest_h) * 3U;
    const bool resized = cache.width != static_cast<uint32_t>(dest_w) ||
                         cache.height != static_cast<uint32_t>(dest_h) ||
                         cache.rgb.size() != pixels;
    if (resized) {
        cache.width = static_cast<uint32_t>(dest_w);
        cache.height = static_cast<uint32_t>(dest_h);
        cache.rgb.resize(pixels);
    }
    const auto current = [&](int32_t cam_x, int32_t cam_y, float cam_zoom) {
        return cam_x == camera_x && cam_y == camera_y && std::abs(cam_zoom - draw_scale) <= 1.0e-4F;
    };
    if (!resized &&
        current(terrain_filtered_cam_x_, terrain_filtered_cam_y_, terrain_filtered_zoom_) &&
        current(terrain_cache_cam_x_, terrain_cache_cam_y_, terrain_cache_zoom_) &&
        terrain_filtered_phase_ == phase && terrain_cache_phase_ == phase)
        return;
    const auto filter_start = std::chrono::steady_clock::now();
    bool filtered = false;
    if (far) {
        // The far view reads the map's pyramid, made at its first far frame
        // and again when the map or the palette changes.
        if (far_terrain_.map != &map || far_terrain_.palette != match_palette_ ||
            far_terrain_.pyramid.tile_count != map.tile_count || far_terrain_.pyramid.rgb.empty()) {
            far_terrain_.pyramid = build_terrain_pyramid(map, match_palette_);
            far_terrain_.map = &map;
            far_terrain_.palette = match_palette_;
        }
        filtered = filter_far_terrain(
            map,
            far_terrain_.pyramid,
            camera_x,
            camera_y,
            static_cast<uint32_t>(map_width),
            static_cast<uint32_t>(map_height),
            dest_w,
            dest_h,
            draw_scale,
            cache.rgb.data(),
            draw_pool_.get(),
            phase[0],
            phase[1]
        );
    } else {
        filtered = box_filter_terrain(
            map,
            match_palette_,
            camera_x,
            camera_y,
            static_cast<uint32_t>(map_width),
            static_cast<uint32_t>(map_height),
            dest_w,
            dest_h,
            draw_scale,
            cache.rgb.data(),
            phase[0],
            phase[1],
            draw_pool_.get()
        );
    }
    terrain_box_filter_ns_ +=
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - filter_start
        )
                                  .count());
    ++terrain_box_filter_runs_;
    if (!filtered)
        return; // A malformed map is reported by the renderer's own fill.
    terrain_cache_cam_x_ = camera_x;
    terrain_cache_cam_y_ = camera_y;
    terrain_cache_zoom_ = draw_scale;
    terrain_cache_phase_ = phase;
    terrain_filtered_cam_x_ = camera_x;
    terrain_filtered_cam_y_ = camera_y;
    terrain_filtered_zoom_ = draw_scale;
    terrain_filtered_phase_ = phase;
}

} // namespace oa::app
