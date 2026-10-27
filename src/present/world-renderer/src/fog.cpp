// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/world_renderer/world_fog.hpp"

#include "oa/present/palette_tables.hpp"

#include <algorithm>
#include <cstring>

namespace oa::present::world_renderer {
namespace {

enum class FogLayer { unmapped, unseen };

uint8_t& layer_mask(FogTile& tile, FogLayer layer) noexcept {
    return layer == FogLayer::unmapped ? tile.unmapped : tile.unseen;
}

void mark_cell_corners(FogGrid& grid, FogLayer layer, int32_t column, int32_t row) {
    const auto mark = [&](int32_t c, int32_t r, uint8_t bit) {
        if (c < 0 || r < 0 || c >= grid.width || r >= grid.height)
            return;
        layer_mask(
            grid.tiles
                [static_cast<std::size_t>(r) * static_cast<std::size_t>(grid.width) +
                 static_cast<std::size_t>(c)],
            layer
        ) |= bit;
    };
    mark(column, row, fog_corner_top_left);
    mark(column - 1, row, fog_corner_top_right);
    mark(column, row - 1, fog_corner_bottom_left);
    mark(column - 1, row - 1, fog_corner_bottom_right);
}

void extend_corner(FogTile& tile, bool unseen_layer, uint8_t from, uint8_t to) {
    if (unseen_layer && (tile.unseen & from) != 0)
        tile.unseen |= to;
    if ((tile.unmapped & from) != 0)
        tile.unmapped |= to;
}

// The fog cell a map pixel lies in, rounded toward negative infinity, so
// that a camera left of or above the map counts its cells as one on it does.
int32_t cell_of(int32_t pixel) noexcept {
    return pixel >= 0
               ? pixel / fog_cell_pixels
               : static_cast<int32_t>(
                     -((-static_cast<int64_t>(pixel) + fog_cell_pixels - 1) / fog_cell_pixels)
                 );
}

// How far into its fog cell a map pixel lies, from 0 to fog_cell_pixels - 1.
int32_t within_cell(int32_t pixel) noexcept {
    return static_cast<int32_t>(
        static_cast<int64_t>(pixel) - int64_t{cell_of(pixel)} * fog_cell_pixels
    );
}

int32_t first_cell(int32_t camera) noexcept {
    return within_cell(camera) < fog_cell_pixels / 2 ? cell_of(camera) - 1 : cell_of(camera);
}

int32_t tile_offset(int32_t camera) noexcept {
    const auto half = fog_cell_pixels / 2;
    return (within_cell(camera) < half ? -half : half) - within_cell(camera);
}

// First destination pixel of map pixel `map` (relative to the camera) under
// the terrain DDA, which shows map pixel floor(d * 65536 / zoom) at pixel d.
int32_t dest_edge(uint32_t zoom_fp, int32_t map) noexcept {
    return static_cast<int32_t>(
        (static_cast<int64_t>(map) * zoom_fp + (fog_zoom_one - 1)) / fog_zoom_one
    );
}

struct PixelSpan {
    int32_t begin{};
    int32_t end{};

    [[nodiscard]] bool empty() const noexcept { return begin >= end; }
};

// Destination pixels of map pixels [map_begin, map_end) clipped to the view.
PixelSpan dest_span(
    uint32_t zoom_fp, int32_t map_begin, int32_t map_end, int32_t clip_begin, int32_t clip_end
) noexcept {
    if (map_end <= 0)
        return {};
    return {
        std::max(dest_edge(zoom_fp, std::max(map_begin, 0)), clip_begin),
        std::min(dest_edge(zoom_fp, map_end), clip_end)
    };
}

struct FogPainter {
    Surface& surface;
    const FogView& view;
    const FogShading& shading;
    uint32_t zoom_fp{};
    // View-relative clip of the destination rectangle against the surface.
    int32_t clip_x0{}, clip_x1{}, clip_y0{}, clip_y1{};

    [[nodiscard]] uint8_t* pixel(int32_t x, int32_t y) noexcept {
        return surface.rgb.data() + (static_cast<std::size_t>(y + view.dest_y) * surface.width +
                                     static_cast<std::size_t>(x + view.dest_x)) *
                                        3U;
    }

    void fill(PixelSpan xs, PixelSpan ys, const std::array<uint8_t, 3>& rgb) noexcept {
        const bool flat = rgb[0] == rgb[1] && rgb[1] == rgb[2];
        for (int32_t y = ys.begin; y < ys.end; ++y) {
            auto* out = pixel(xs.begin, y);
            const auto count = static_cast<std::size_t>(xs.end - xs.begin);
            if (flat) {
                std::memset(out, rgb[0], count * 3U);
                continue;
            }
            for (std::size_t x = 0; x < count; ++x, out += 3) {
                out[0] = rgb[0];
                out[1] = rgb[1];
                out[2] = rgb[2];
            }
        }
    }

    // Grays one pixel: its level, the mean of its channels, through the gray table.
    void gray_pixel(uint8_t* out) const noexcept {
        const auto level = (static_cast<unsigned>(out[0]) + out[1] + out[2]) / 3u;
        const auto& rgb = shading.gray_levels[level];
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
    }

    // Graying remaps the ground through the gray table.
    void gray(PixelSpan xs, PixelSpan ys) noexcept {
        for (int32_t y = ys.begin; y < ys.end; ++y) {
            auto* out = pixel(xs.begin, y);
            for (int32_t x = xs.begin; x < xs.end; ++x, out += 3)
                gray_pixel(out);
        }
    }

    // Whether dithering clears the destination pixel (x, y) of the view,
    // counted from its top-left corner: every other one, in a checkerboard
    // on the screen's pixels whose phase follows the camera's map pixel, so
    // that at the whole zoom, where a destination pixel is a map pixel, it
    // clears every other map pixel as the game does, and at any other zoom
    // the dither stays one pixel fine.
    [[nodiscard]] bool dithered_out(int32_t x, int32_t y) const noexcept {
        return ((static_cast<uint32_t>(view.camera_x + x) +
                 static_cast<uint32_t>(view.camera_z + y)) &
                1u) == 0;
    }

    // Writes one pixel's colour.
    static void put(uint8_t* out, const std::array<uint8_t, 3>& rgb) noexcept {
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
    }

    // Moves one pixel half way to the dither colour, the even tone of the
    // dither, rounding to nearest.
    void half_dither(uint8_t* out) const noexcept {
        for (std::size_t channel = 0; channel < 3; ++channel)
            out[channel] = static_cast<uint8_t>(
                (static_cast<unsigned>(out[channel]) + shading.dither_rgb[channel] + 1u) / 2u
            );
    }

    // Dithers one destination pixel at (x, y) of the view: clears it to the
    // dither colour when dithering clears it, or draws the even tone.
    void dither_pixel(uint8_t* out, int32_t x, int32_t y) const noexcept {
        if (shading.dither_even)
            half_dither(out);
        else if (dithered_out(x, y))
            put(out, shading.dither_rgb);
    }

    // Dithers the destination pixels xs x ys, each by its own place on the
    // screen, whatever map pixel it shows.
    void dither(PixelSpan xs, PixelSpan ys) noexcept {
        for (int32_t y = ys.begin; y < ys.end; ++y) {
            auto* out = pixel(xs.begin, y);
            for (int32_t x = xs.begin; x < xs.end; ++x, out += 3)
                dither_pixel(out, x, y);
        }
    }

    // Grays or dithers the destination pixels the map pixels [map_x0,
    // map_x1) x [map_z0, map_z1) cover.
    void shade(int32_t map_x0, int32_t map_x1, int32_t map_z0, int32_t map_z1) noexcept {
        const auto xs = dest_span(zoom_fp, map_x0, map_x1, clip_x0, clip_x1);
        const auto ys = dest_span(zoom_fp, map_z0, map_z1, clip_y0, clip_y1);
        if (shading.dithered)
            dither(xs, ys);
        else
            gray(xs, ys);
    }

    // Draws the opaque texels of a tile whose top-left map pixel is (map_x0,
    // map_z0) at the whole zoom, where a texel is one destination pixel:
    // `texel` gets each texel's pixel, its palette index and its map pixel.
    template <typename Texel>
    void
    masked_whole(const FogTileArt& art, int32_t map_x0, int32_t map_z0, Texel&& texel) noexcept {
        const int32_t first_tx = std::max({0, -map_x0, clip_x0 - map_x0});
        const int32_t end_tx = std::min(fog_cell_pixels, clip_x1 - map_x0);
        if (first_tx >= end_tx)
            return;
        for (int32_t ty = 0; ty < fog_cell_pixels; ++ty) {
            const auto mz = map_z0 + ty;
            if (mz < 0 || mz < clip_y0 || mz >= clip_y1)
                continue;
            const auto* row = &art.opaque[static_cast<std::size_t>(ty) * fog_cell_pixels];
            const auto* index = &art.index[static_cast<std::size_t>(ty) * fog_cell_pixels];
            auto* out = pixel(map_x0 + first_tx, mz);
            for (int32_t tx = first_tx; tx < end_tx; ++tx, out += 3)
                if (row[tx] != 0)
                    texel(out, index[tx], map_x0 + tx, mz);
        }
    }

    // Destination pixels of the one map pixel `map` (not negative), clipped to
    // [clip_begin, clip_end): dest_span's, worked out without its products
    // at the whole zoom, where a map pixel is one destination pixel.
    [[nodiscard]] PixelSpan
    pixel_span(int32_t map, int32_t clip_begin, int32_t clip_end) const noexcept {
        if (zoom_fp == fog_zoom_one)
            return {std::max(map, clip_begin), std::min(map + 1, clip_end)};
        return dest_span(zoom_fp, map, map + 1, clip_begin, clip_end);
    }

    // Draws the opaque texels of a tile whose top-left map pixel is (map_x0, map_z0).
    template <typename Texel>
    void masked(const FogTileArt& art, int32_t map_x0, int32_t map_z0, Texel&& texel) noexcept {
        for (int32_t ty = 0; ty < fog_cell_pixels; ++ty) {
            const auto mz = map_z0 + ty;
            if (mz < 0)
                continue;
            const auto ys = pixel_span(mz, clip_y0, clip_y1);
            if (ys.empty())
                continue;
            const auto* row = &art.opaque[static_cast<std::size_t>(ty) * fog_cell_pixels];
            const auto* index = &art.index[static_cast<std::size_t>(ty) * fog_cell_pixels];
            for (int32_t tx = 0; tx < fog_cell_pixels; ++tx) {
                if (row[tx] == 0)
                    continue;
                const auto mx = map_x0 + tx;
                if (mx < 0)
                    continue;
                const auto xs = pixel_span(mx, clip_x0, clip_x1);
                if (!xs.empty())
                    texel(index[tx], mx, mz, xs, ys);
            }
        }
    }
};

/// What every row of draw_fog_grid reads besides its painter.
struct FogRows {
    const FogGrid& grid;
    const FogTileSet& tiles;
    const FogShading& shading;
    int32_t span_x{}; ///< map pixels the view covers across
    int32_t span_z{}; ///< map pixels the view covers down
    bool have_art{};  ///< the FOG.GAF tile art is loaded
};

/// Draws one row of the fog grid: the surface rows under its map rows alone.
///
/// @param painter the view's painter; it keeps no state of its own
/// @param rows the grid, its art and its shading
/// @param row grid row to draw
void draw_fog_row(FogPainter painter, const FogRows& rows, int32_t row) noexcept {
    const FogGrid& grid = rows.grid;
    const FogShading& shading = rows.shading;
    const auto zoom_fp = painter.zoom_fp;
    const auto map_z0 = grid.offset_z + row * fog_cell_pixels;
    if (map_z0 >= rows.span_z || map_z0 + fog_cell_pixels <= 0)
        return;
    const auto ys =
        dest_span(zoom_fp, map_z0, map_z0 + fog_cell_pixels, painter.clip_y0, painter.clip_y1);
    if (ys.empty())
        return;
    for (int32_t column = 0; column < grid.width; ++column) {
        const auto& tile = grid.at(column, row);
        if (tile.unmapped == 0 && tile.unseen == 0)
            continue;
        const auto map_x0 = grid.offset_x + column * fog_cell_pixels;
        if (map_x0 >= rows.span_x || map_x0 + fog_cell_pixels <= 0)
            continue;
        const auto xs =
            dest_span(zoom_fp, map_x0, map_x0 + fog_cell_pixels, painter.clip_x0, painter.clip_x1);
        if (xs.empty())
            continue;
        if (tile.unmapped == fog_mask_full) {
            painter.fill(xs, ys, shading.unmapped_rgb);
            continue;
        }
        const auto variant = (row + column + grid.variant_phase) & (fog_tile_variants - 1);
        const bool whole = zoom_fp == fog_zoom_one;
        if (tile.unseen == fog_mask_full) {
            painter.shade(map_x0, map_x0 + fog_cell_pixels, map_z0, map_z0 + fog_cell_pixels);
        } else if (tile.unseen != 0 && rows.have_art) {
            const auto& art = rows.tiles.at(FogTileSet::gray, variant, tile.unseen);
            if (whole && shading.dithered)
                painter.masked_whole(
                    art, map_x0, map_z0, [&](uint8_t* out, uint8_t, int32_t x, int32_t y) {
                        // At the whole zoom a map pixel is its destination pixel.
                        painter.dither_pixel(out, x, y);
                    }
                );
            else if (whole)
                painter.masked_whole(
                    art, map_x0, map_z0, [&](uint8_t* out, uint8_t, int32_t, int32_t) {
                        painter.gray_pixel(out);
                    }
                );
            else
                painter.masked(
                    art,
                    map_x0,
                    map_z0,
                    [&](uint8_t, int32_t, int32_t, PixelSpan tx, PixelSpan ty) {
                        if (shading.dithered)
                            painter.dither(tx, ty);
                        else
                            painter.gray(tx, ty);
                    }
                );
        }
        if (tile.unmapped != 0 && rows.have_art) {
            const auto& art = rows.tiles.at(FogTileSet::black, variant, tile.unmapped);
            if (whole)
                painter.masked_whole(
                    art, map_x0, map_z0, [&](uint8_t* out, uint8_t index, int32_t, int32_t) {
                        FogPainter::put(out, shading.palette_rgb[index]);
                    }
                );
            else
                painter.masked(
                    art,
                    map_x0,
                    map_z0,
                    [&](uint8_t index, int32_t, int32_t, PixelSpan tx, PixelSpan ty) {
                        painter.fill(tx, ty, shading.palette_rgb[index]);
                    }
                );
        }
    }
}

} // namespace

int32_t fog_map_span(uint32_t zoom_fp, int32_t count) noexcept {
    if (count <= 0)
        return 0;
    if (zoom_fp == 0)
        zoom_fp = 1;
    return static_cast<int32_t>(static_cast<int64_t>(count - 1) * fog_zoom_one / zoom_fp) + 1;
}

FogGrid build_fog_grid(
    const sim::visibility_state::PlayerSightGrid& sight,
    uint8_t viewer,
    std::span<const uint16_t> coverage,
    FogOptions options,
    int32_t camera_x,
    int32_t camera_z,
    int32_t view_width,
    int32_t view_height
) {
    FogGrid grid;
    grid.width = (view_width + fog_cell_pixels - 1) / fog_cell_pixels + 2;
    grid.height = (view_height + fog_cell_pixels - 1) / fog_cell_pixels + 2;
    if (grid.width < 2 || grid.height < 2)
        return {};
    grid.first_cell_x = first_cell(camera_x);
    grid.first_cell_z = first_cell(camera_z);
    grid.offset_x = tile_offset(camera_x);
    grid.offset_z = tile_offset(camera_z);
    grid.variant_phase =
        cell_of(camera_x + fog_cell_pixels / 2) + cell_of(camera_z + fog_cell_pixels / 2);
    grid.tiles.assign(
        static_cast<std::size_t>(grid.width) * static_cast<std::size_t>(grid.height), FogTile{}
    );
    const auto viewer_bit = static_cast<uint16_t>(1u << (viewer & 0x1fu));
    const auto cells = static_cast<std::size_t>(sight.width > 0 ? sight.width : 0) *
                       static_cast<std::size_t>(sight.height > 0 ? sight.height : 0);
    for (int32_t row = 0; row < grid.height; ++row) {
        const auto z = grid.first_cell_z + row;
        for (int32_t column = 0; column < grid.width; ++column) {
            const auto x = grid.first_cell_x + column;
            if (x < 0 || z < 0 || x >= sight.width || z >= sight.height)
                continue;
            const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(sight.width) +
                               static_cast<std::size_t>(x);
            if (index >= cells)
                continue;
            const bool in_sight = index < coverage.size() &&
                                  sim::visibility_state::count_in_sight(
                                      coverage[index], sim::visibility_state::SightReading::player
                                  );
            if (!in_sight && options.line_of_sight)
                mark_cell_corners(grid, FogLayer::unseen, column, row);
            const bool mapped = !options.mapping || (index < sight.player_bits.size() &&
                                                     (sight.player_bits[index] & viewer_bit) != 0);
            if (!mapped)
                mark_cell_corners(grid, FogLayer::unmapped, column, row);
        }
    }
    // The game copies the corners of the first tile and the last-but-one
    // tile, which straddle the border when the view is a whole number of
    // tiles wide; here the straddling tile is found from the border cell so
    // any view size and zoom keeps a clean border.
    const bool unseen_layer = options.line_of_sight;
    const auto tile = [&](int32_t column, int32_t row) -> FogTile& {
        return grid.tiles
            [static_cast<std::size_t>(row) * static_cast<std::size_t>(grid.width) +
             static_cast<std::size_t>(column)];
    };
    const auto top_row = -1 - grid.first_cell_z; // top corners off the map
    if (top_row >= 0 && top_row < grid.height)
        for (int32_t column = 0; column < grid.width; ++column) {
            auto& edge = tile(column, top_row);
            extend_corner(edge, unseen_layer, fog_corner_bottom_left, fog_corner_top_left);
            extend_corner(edge, unseen_layer, fog_corner_bottom_right, fog_corner_top_right);
        }
    const auto bottom_row = sight.height - 1 - grid.first_cell_z; // bottom corners off the map
    if (bottom_row >= 0 && bottom_row < grid.height)
        for (int32_t column = 0; column < grid.width; ++column) {
            auto& edge = tile(column, bottom_row);
            extend_corner(edge, unseen_layer, fog_corner_top_left, fog_corner_bottom_left);
            extend_corner(edge, unseen_layer, fog_corner_top_right, fog_corner_bottom_right);
        }
    const auto left_column = -1 - grid.first_cell_x;
    if (left_column >= 0 && left_column < grid.width)
        for (int32_t row = 0; row < grid.height; ++row) {
            auto& edge = tile(left_column, row);
            extend_corner(edge, unseen_layer, fog_corner_bottom_right, fog_corner_bottom_left);
            extend_corner(edge, unseen_layer, fog_corner_top_right, fog_corner_top_left);
        }
    const auto right_column = sight.width - 1 - grid.first_cell_x;
    if (right_column >= 0 && right_column < grid.width)
        for (int32_t row = 0; row < grid.height; ++row) {
            auto& edge = tile(right_column, row);
            extend_corner(edge, unseen_layer, fog_corner_bottom_left, fog_corner_bottom_right);
            extend_corner(edge, unseen_layer, fog_corner_top_left, fog_corner_top_right);
        }
    return grid;
}

bool update_fog_grid(
    const sim::visibility_state::PlayerSightGrid& sight,
    uint8_t viewer,
    std::span<const uint16_t> coverage,
    FogOptions options,
    int32_t camera_x,
    int32_t camera_z,
    int32_t view_width,
    int32_t view_height,
    FogGrid& grid,
    FogGridSource& source
) {
    bool same = source.built && source.camera_x == camera_x && source.camera_z == camera_z &&
                source.view_width == view_width && source.view_height == view_height &&
                source.sight_width == sight.width && source.sight_height == sight.height &&
                source.viewer == viewer && source.options.line_of_sight == options.line_of_sight &&
                source.options.mapping == options.mapping;
    // The cells build_fog_grid reads, read as it reads them.
    const int32_t width = (view_width + fog_cell_pixels - 1) / fog_cell_pixels + 2;
    const int32_t height = (view_height + fog_cell_pixels - 1) / fog_cell_pixels + 2;
    const std::size_t count = width < 2 || height < 2 ? 0
                                                      : static_cast<std::size_t>(width) *
                                                            static_cast<std::size_t>(height);
    if (source.cells.size() != count) {
        same = false;
        source.cells.assign(count, 0);
    }
    const int32_t first_x = first_cell(camera_x);
    const int32_t first_z = first_cell(camera_z);
    const auto viewer_bit = static_cast<uint16_t>(1u << (viewer & 0x1fu));
    const auto cells = static_cast<std::size_t>(sight.width > 0 ? sight.width : 0) *
                       static_cast<std::size_t>(sight.height > 0 ? sight.height : 0);
    std::size_t at = 0;
    for (int32_t row = 0; count != 0 && row < height; ++row) {
        const auto z = first_z + row;
        for (int32_t column = 0; column < width; ++column, ++at) {
            const auto x = first_x + column;
            uint8_t read = 0;
            if (x >= 0 && z >= 0 && x < sight.width && z < sight.height) {
                const auto index =
                    static_cast<std::size_t>(z) * static_cast<std::size_t>(sight.width) +
                    static_cast<std::size_t>(x);
                if (index < cells) {
                    read = fog_source_on_map;
                    if (index < coverage.size() &&
                        sim::visibility_state::count_in_sight(
                            coverage[index], sim::visibility_state::SightReading::player
                        ))
                        read |= fog_source_in_sight;
                    if (index < sight.player_bits.size() &&
                        (sight.player_bits[index] & viewer_bit) != 0)
                        read |= fog_source_mapped;
                }
            }
            if (source.cells[at] != read) {
                same = false;
                source.cells[at] = read;
            }
        }
    }
    if (same)
        return false;
    source.built = true;
    source.camera_x = camera_x;
    source.camera_z = camera_z;
    source.view_width = view_width;
    source.view_height = view_height;
    source.sight_width = sight.width;
    source.sight_height = sight.height;
    source.viewer = viewer;
    source.options = options;
    grid = build_fog_grid(
        sight, viewer, coverage, options, camera_x, camera_z, view_width, view_height
    );
    return true;
}

void draw_fog_grid(
    Surface& surface,
    const FogView& view,
    const FogGrid& grid,
    const FogTileSet& tiles,
    const FogShading& shading,
    platform::job_pool::Pool* pool
) {
    if (grid.tiles.empty() || view.dest_width <= 0 || view.dest_height <= 0)
        return;
    const auto zoom_fp = view.zoom_fp == 0 ? fog_zoom_one : view.zoom_fp;
    const FogPainter painter{
        surface,
        view,
        shading,
        zoom_fp,
        std::max(view.dest_x, 0) - view.dest_x,
        std::min(view.dest_x + view.dest_width, static_cast<int32_t>(surface.width)) - view.dest_x,
        std::max(view.dest_y, 0) - view.dest_y,
        std::min(view.dest_y + view.dest_height, static_cast<int32_t>(surface.height)) - view.dest_y
    };
    if (painter.clip_x0 >= painter.clip_x1 || painter.clip_y0 >= painter.clip_y1)
        return;
    const FogRows rows{
        grid,
        tiles,
        shading,
        fog_map_span(zoom_fp, view.dest_width),
        fog_map_span(zoom_fp, view.dest_height),
        tiles.loaded()
    };
    // A grid row draws the map rows [map_z0, map_z0 + fog_cell_pixels) and
    // the surface rows they land on, which no other grid row touches.
    platform::job_pool::run_bands(pool, static_cast<uint32_t>(grid.height), [&](uint32_t band) {
        draw_fog_row(painter, rows, static_cast<int32_t>(band));
    });
}

void build_gray_levels(const Palette& palette, std::array<uint8_t, OA_PALETTE_COLORS>& levels) {
    int32_t brightness[OA_PALETTE_COLORS];
    uint8_t order[OA_PALETTE_COLORS];
    ::oa::present::sort_palette_by_brightness(palette, brightness, order);
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const auto gray = static_cast<uint8_t>(level);
        levels[level] =
            ::oa::present::find_nearest_sorted_color(palette, brightness, order, gray, gray, gray);
    }
}

} // namespace oa::present::world_renderer
