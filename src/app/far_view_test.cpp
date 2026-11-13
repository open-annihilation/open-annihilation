// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The far view (far_view.hpp): the zoom at which the whole map fits a
// battlefield wider, taller and the same shape as the map; the view's zoom
// floor for each Maximum zoom out choice on a large map, a map that fits
// at normal size, a map past the farthest zoom and no map, Automatic
// keeping the drawing's floor; which frames draw the far view; the terrain
// pyramid's texels against the means of the map pixels they stand for; the
// level each zoom reads; the filter against a sum over the texels whose
// centres each screen pixel covers, black past the shown map and before it,
// the same on one and on four drawing threads; the units' dots, framed when
// selected and clipped at the picture's edges; the pixels a dot covers for
// the pointer, which are the pixels drawn in its colour; and how far past
// the map's edges the view's centre goes, and how a view and a camera are
// held there.
#include "oa/app/far_view.hpp"
#include "unit_detail_transition.hpp"

#include "oa/platform/job_pool.hpp"
#include "oa/test/check.hpp"
#include "oa/formats/objects3d.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <limits>
#include <vector>

namespace {

using oa::app::FarViewDot;
using oa::ui::engine_settings::ZoomOutLimit;

/// Map pixels across a tile.
constexpr uint32_t tile_edge = 32;

/// A palette whose entry n is (n, 255 - n, n / 2).
oa::PaletteBytes ramp_palette() {
    oa::PaletteBytes palette{};
    for (std::size_t entry = 0; entry < 256; ++entry) {
        palette[entry * 4] = static_cast<uint8_t>(entry);
        palette[entry * 4 + 1] = static_cast<uint8_t>(255 - entry);
        palette[entry * 4 + 2] = static_cast<uint8_t>(entry / 2);
    }
    return palette;
}

/// A map of tiles_wide by tiles_high tiles over three distinct tiles: the
/// first a ramp across and down, the second a checkerboard of entries 10
/// and 200, the third entry 77 throughout; the grid names them in turn.
oa::formats::tnt::Map three_tile_map(uint32_t tiles_wide, uint32_t tiles_high) {
    oa::formats::tnt::Map map;
    map.tile_width = tiles_wide;
    map.tile_height = tiles_high;
    map.tile_count = 3;
    map.tile_palette_indices.resize(3U * tile_edge * tile_edge);
    for (uint32_t y = 0; y < tile_edge; ++y)
        for (uint32_t x = 0; x < tile_edge; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * tile_edge + x;
            map.tile_palette_indices[at] = static_cast<uint8_t>(x * 4 + y * 3);
            map.tile_palette_indices[1024 + at] = (x + y) % 2 == 0 ? 10 : 200;
            map.tile_palette_indices[2048 + at] = 77;
        }
    map.tile_indices.resize(static_cast<std::size_t>(tiles_wide) * tiles_high);
    for (std::size_t cell = 0; cell < map.tile_indices.size(); ++cell)
        map.tile_indices[cell] = static_cast<uint16_t>((cell * 7 + cell / tiles_wide) % 3);
    return map;
}

/// The colour channel of one map pixel in the ramp palette.
uint32_t map_channel(
    const oa::formats::tnt::Map& map,
    const oa::PaletteBytes& palette,
    uint32_t x,
    uint32_t y,
    uint32_t channel
) {
    const auto tile =
        map.tile_indices[static_cast<std::size_t>(y / tile_edge) * map.tile_width + x / tile_edge];
    const auto index =
        map.tile_palette_indices
            [static_cast<std::size_t>(tile) * 1024 + (y % tile_edge) * tile_edge + x % tile_edge];
    return palette[static_cast<std::size_t>(index) * 4 + channel];
}

void the_whole_map_fits_one_way_and_fills_the_other() {
    // A map wider than the battlefield's shape fills its width.
    OA_CHECK(oa::app::whole_map_zoom(8192, 2048, 1664, 952) == 1664.0F / 8192.0F);
    // A taller one fills its height.
    OA_CHECK(oa::app::whole_map_zoom(4096, 8192, 1664, 952) == 952.0F / 8192.0F);
    // A map of the battlefield's own shape fills both.
    OA_CHECK(oa::app::whole_map_zoom(3328, 1904, 1664, 952) == 0.5F);
    // A map smaller than the battlefield fits beyond normal size.
    OA_CHECK(oa::app::whole_map_zoom(832, 476, 1664, 952) == 2.0F);
    OA_CHECK(oa::app::whole_map_zoom(0, 2048, 1664, 952) == 0.0F);
    OA_CHECK(oa::app::whole_map_zoom(2048, 2048, 0, 952) == 0.0F);
}

void automatic_keeps_the_drawings_floor() {
    for (const float floor : {0.5F, 0.5F / 3.0F}) {
        // Whatever the map: a map larger than the view, one smaller than
        // it, and none.
        OA_CHECK(
            oa::app::least_battlefield_zoom(
                ZoomOutLimit::automatic, floor, 20448, 20352, 1664, 952
            ) == floor
        );
        OA_CHECK(
            oa::app::least_battlefield_zoom(
                ZoomOutLimit::automatic, floor, 1024, 1024, 1664, 952
            ) == floor
        );
        OA_CHECK(
            oa::app::least_battlefield_zoom(ZoomOutLimit::automatic, floor, 0, 0, 1664, 952) ==
            floor
        );
    }
}

void each_choice_stops_at_its_share_or_the_whole_map() {
    constexpr float floor = 0.5F;
    // A large map at 1920x1080: the whole map fits at 952 / 20352.
    constexpr int32_t wide = 20448, high = 20352, bf_w = 1664, bf_h = 952;
    const float fit = oa::app::whole_map_zoom(wide, high, bf_w, bf_h);
    OA_CHECK(fit == 952.0F / 20352.0F);
    const auto least = [&](ZoomOutLimit limit) {
        return oa::app::least_battlefield_zoom(limit, floor, wide, high, bf_w, bf_h);
    };
    // The map fits at about a twenty-first: 1/32 stops there too.
    OA_CHECK(least(ZoomOutLimit::whole_map) == fit);
    OA_CHECK(least(ZoomOutLimit::one_thirty_second) == fit);
    OA_CHECK(least(ZoomOutLimit::one_sixteenth) == 1.0F / 16.0F);
    OA_CHECK(least(ZoomOutLimit::one_eighth) == 1.0F / 8.0F);
    OA_CHECK(least(ZoomOutLimit::one_quarter) == 0.25F);
    OA_CHECK(least(ZoomOutLimit::one_half) == 0.5F);
    // A map that fits at a fifth: the shares beyond it stop at the fit.
    // At 640x480 the same map fits at about a forty-ninth, past 1/32.
    OA_CHECK(
        oa::app::least_battlefield_zoom(
            ZoomOutLimit::one_thirty_second, floor, wide, high, 416, 416
        ) == 1.0F / 32.0F
    );
    const auto least_fifth = [&](ZoomOutLimit limit) {
        return oa::app::least_battlefield_zoom(limit, floor, 8320, 4760, bf_w, bf_h);
    };
    OA_CHECK(least_fifth(ZoomOutLimit::whole_map) == 0.2F);
    OA_CHECK(least_fifth(ZoomOutLimit::one_thirty_second) == 0.2F);
    OA_CHECK(least_fifth(ZoomOutLimit::one_eighth) == 0.2F);
    OA_CHECK(least_fifth(ZoomOutLimit::one_quarter) == 0.25F);
    // A map that fits at normal size or closer: no choice zooms out, and
    // none forces a zoom in.
    for (const auto limit : oa::ui::engine_settings::zoom_out_limits)
        if (limit != ZoomOutLimit::automatic)
            OA_CHECK(oa::app::least_battlefield_zoom(limit, floor, 1024, 512, bf_w, bf_h) == 1.0F);
    // A map past the farthest zoom stops there.
    OA_CHECK(
        oa::app::least_battlefield_zoom(ZoomOutLimit::whole_map, floor, 65536, 65536, 640, 520) ==
        oa::app::furthest_battlefield_zoom
    );
    // Without a map, every choice keeps Automatic's floor.
    for (const auto limit : oa::ui::engine_settings::zoom_out_limits)
        OA_CHECK(oa::app::least_battlefield_zoom(limit, floor, 0, 0, bf_w, bf_h) == floor);
}

void frames_past_the_detail_floor_draw_the_far_view() {
    OA_CHECK(!oa::app::far_view_zoom(0.5F, 0.5F));
    OA_CHECK(oa::app::far_view_zoom(0.49F, 0.5F));
    OA_CHECK(!oa::app::far_view_zoom(1.0F, 0.5F));
    OA_CHECK(!oa::app::far_view_zoom(0.5F / 3.0F, 0.5F / 3.0F));
    OA_CHECK(oa::app::far_view_zoom(0.1F, 0.5F / 3.0F));
    OA_CHECK(!oa::app::far_view_zoom(0.0F, 0.5F));
}

void the_pyramid_holds_the_means_of_the_map_pixels() {
    const auto palette = ramp_palette();
    const auto map = three_tile_map(2, 2);
    const auto pyramid = oa::app::build_terrain_pyramid(map, palette);
    OA_CHECK(pyramid.tile_count == 3);
    OA_CHECK(pyramid.rgb.size() == 3 * oa::app::pyramid_tile_texels * 3);
    std::size_t offset = 0;
    for (uint32_t level = 1; level <= 5; ++level) {
        const uint32_t edge = tile_edge >> level;
        const uint32_t side = 1U << level;
        for (uint32_t tile = 0; tile < 3; ++tile)
            for (uint32_t y = 0; y < edge; ++y)
                for (uint32_t x = 0; x < edge; ++x)
                    for (uint32_t channel = 0; channel < 3; ++channel) {
                        uint32_t sum = 0;
                        for (uint32_t py = 0; py < side; ++py)
                            for (uint32_t px = 0; px < side; ++px) {
                                const auto index =
                                    map.tile_palette_indices
                                        [tile * 1024 + (y * side + py) * tile_edge + x * side + px];
                                sum += palette[static_cast<std::size_t>(index) * 4 + channel];
                            }
                        const uint32_t covered = side * side;
                        const auto expected = static_cast<uint8_t>((sum + covered / 2) / covered);
                        const std::size_t at =
                            (tile * oa::app::pyramid_tile_texels + offset + y * edge + x) * 3 +
                            channel;
                        OA_CHECK(pyramid.rgb[at] == expected);
                    }
        offset += static_cast<std::size_t>(edge) * edge;
    }
    // A tile of one colour is that colour at every level.
    OA_CHECK(pyramid.rgb[(2 * oa::app::pyramid_tile_texels + 340) * 3] == 77);
    OA_CHECK(pyramid.rgb[(2 * oa::app::pyramid_tile_texels + 340) * 3 + 1] == 178);
    // A short tile table builds no pyramid.
    auto short_map = map;
    short_map.tile_palette_indices.resize(1024);
    OA_CHECK(oa::app::build_terrain_pyramid(short_map, palette).rgb.empty());
}

void each_zoom_reads_its_level() {
    OA_CHECK(oa::app::far_terrain_level(0.49F) == 1);
    OA_CHECK(oa::app::far_terrain_level(0.26F) == 1);
    OA_CHECK(oa::app::far_terrain_level(0.25F) == 2);
    OA_CHECK(oa::app::far_terrain_level(0.5F / 3.0F) == 2);
    OA_CHECK(oa::app::far_terrain_level(0.125F) == 3);
    OA_CHECK(oa::app::far_terrain_level(1.0F / 20.0F) == 4);
    OA_CHECK(oa::app::far_terrain_level(1.0F / 32.0F) == 5);
    OA_CHECK(oa::app::far_terrain_level(1.0F / 64.0F) == 5);
    OA_CHECK(oa::app::far_terrain_level(1.0F / 200.0F) == 5);
}

/// The filter's picture by a sum over the texels: each pixel's span of map
/// pixels stepped as the nearest fill steps them, the texels whose centres
/// lie in it, black past the shown map and before it.
std::vector<uint8_t> expected_far_terrain(
    const oa::formats::tnt::Map& map,
    const oa::PaletteBytes& palette,
    int32_t source_x,
    int32_t source_y,
    uint32_t shown_width,
    uint32_t shown_height,
    int32_t width,
    int32_t height,
    float zoom
) {
    const uint32_t level = oa::app::far_terrain_level(zoom);
    const uint32_t side = 1U << level;
    auto scale_fp = static_cast<uint32_t>(std::lround(static_cast<double>(zoom) * 65536.0));
    const auto spans = [&](int32_t start, int32_t count) {
        std::vector<int64_t> bounds;
        int64_t position = start;
        uint32_t fraction = 0;
        for (int32_t index = 0; index <= count; ++index) {
            bounds.push_back(position);
            fraction += 65536;
            while (fraction >= scale_fp) {
                fraction -= scale_fp;
                ++position;
            }
        }
        return bounds;
    };
    const auto columns = spans(source_x, width);
    const auto rows = spans(source_y, height);
    const uint32_t terrain_w = std::min(map.tile_width * tile_edge, shown_width);
    const uint32_t terrain_h = std::min(map.tile_height * tile_edge, shown_height);
    std::vector<uint8_t> picture(static_cast<std::size_t>(width) * height * 3);
    for (int32_t dy = 0; dy < height; ++dy)
        for (int32_t dx = 0; dx < width; ++dx)
            for (uint32_t channel = 0; channel < 3; ++channel) {
                uint64_t sum = 0;
                uint32_t count = 0;
                // Texels whose centres lie in the pixel's spans.
                for (uint32_t ty = 0; ty * side < rows.back() + side; ++ty) {
                    const int64_t centre_y = ty * side + side / 2;
                    if (centre_y < rows[dy] || centre_y >= rows[dy + 1])
                        continue;
                    for (uint32_t tx = 0; tx * side < columns.back() + side; ++tx) {
                        const int64_t centre_x = tx * side + side / 2;
                        if (centre_x < columns[dx] || centre_x >= columns[dx + 1])
                            continue;
                        ++count;
                        if (centre_x >= terrain_w || centre_y >= terrain_h)
                            continue;
                        uint32_t texel = 0;
                        for (uint32_t py = 0; py < side; ++py)
                            for (uint32_t px = 0; px < side; ++px)
                                texel += map_channel(
                                    map, palette, tx * side + px, ty * side + py, channel
                                );
                        sum += (texel + side * side / 2) / (side * side);
                    }
                }
                picture[(static_cast<std::size_t>(dy) * width + dx) * 3 + channel] =
                    count == 0 ? 0 : static_cast<uint8_t>((sum + count / 2) / count);
            }
    return picture;
}

void the_filter_averages_the_texels_each_pixel_covers() {
    const auto palette = ramp_palette();
    const auto map = three_tile_map(9, 7);
    const auto pyramid = oa::app::build_terrain_pyramid(map, palette);
    oa::platform::job_pool::Pool four(4);

    struct Case {
        float zoom;
        int32_t source_x, source_y;
        uint32_t shown_width, shown_height;
        int32_t width, height;
    };

    // Each level, from the camera at the map's corner and off it, the view
    // past the shown map's edges on the right and bottom, and before its
    // left and top edges.
    const std::array<Case, 9> cases{{
        {0.4F, 0, 0, 288, 224, 90, 70},
        {0.3F, 13, 7, 280, 200, 80, 60},
        {0.2F, 37, 5, 288, 224, 60, 48},
        {0.1F, 3, 61, 288, 224, 30, 20},
        {0.05F, 0, 0, 270, 210, 16, 12},
        {1.0F / 40.0F, 17, 0, 288, 224, 9, 7},
        {0.3F, -13, -7, 280, 200, 80, 60},
        {0.1F, -150, 20, 288, 224, 40, 20},
        {0.05F, 9, -333, 270, 210, 16, 30},
    }};
    for (const Case& c : cases) {
        const auto expected = expected_far_terrain(
            map,
            palette,
            c.source_x,
            c.source_y,
            c.shown_width,
            c.shown_height,
            c.width,
            c.height,
            c.zoom
        );
        for (oa::platform::job_pool::Pool* pool :
             {static_cast<oa::platform::job_pool::Pool*>(nullptr), &four}) {
            std::vector<uint8_t> picture(expected.size(), 0xAB);
            OA_CHECK(
                oa::app::filter_far_terrain(
                    map,
                    pyramid,
                    c.source_x,
                    c.source_y,
                    c.shown_width,
                    c.shown_height,
                    c.width,
                    c.height,
                    c.zoom,
                    picture.data(),
                    pool
                )
            );
            OA_CHECK(picture == expected);
        }
    }
    // A map of one colour reads that colour wherever the view lies on it.
    auto plain = map;
    for (auto& tile : plain.tile_indices)
        tile = 2;
    const auto plain_pyramid = oa::app::build_terrain_pyramid(plain, palette);
    std::vector<uint8_t> picture(20 * 10 * 3);
    OA_CHECK(
        oa::app::filter_far_terrain(
            plain, plain_pyramid, 0, 0, 288, 224, 20, 10, 0.1F, picture.data(), nullptr
        )
    );
    for (std::size_t pixel = 0; pixel < 20 * 10; ++pixel)
        OA_CHECK(
            picture[pixel * 3] == 77 && picture[pixel * 3 + 1] == 178 &&
            picture[pixel * 3 + 2] == 38
        );
    // A grid naming a missing tile, or a pyramid of no map, fails.
    auto broken = map;
    broken.tile_indices[0] = 9;
    OA_CHECK(!oa::app::filter_far_terrain(
        broken, pyramid, 0, 0, 288, 224, 20, 10, 0.1F, picture.data(), nullptr
    ));
    oa::app::TerrainPyramid empty;
    OA_CHECK(!oa::app::filter_far_terrain(
        map, empty, 0, 0, 288, 224, 20, 10, 0.1F, picture.data(), nullptr
    ));
}

void dots_are_framed_when_selected_and_clipped() {
    constexpr int32_t width = 12, height = 9;
    std::vector<uint8_t> picture(static_cast<std::size_t>(width) * height * 3, 0);
    const std::array<FarViewDot, 2> dots{{
        {5, 4, {200, 10, 20}, true, {255, 255, 255}},
        // Past the right edge: only its left columns land.
        {11, 0, {0, 0, 250}, false, {}},
    }};
    oa::app::draw_far_view_dots(picture.data(), width, height, dots);
    const auto at = [&](int32_t x, int32_t y) {
        const auto* pixel = &picture[(static_cast<std::size_t>(y) * width + x) * 3];
        return std::array<uint8_t, 3>{pixel[0], pixel[1], pixel[2]};
    };
    // The selected dot: 5x5 of its colour within a white ring.
    for (int32_t y = 1; y <= 7; ++y)
        for (int32_t x = 2; x <= 8; ++x) {
            const bool ring = x == 2 || x == 8 || y == 1 || y == 7;
            OA_CHECK(
                at(x, y) ==
                (ring ? std::array<uint8_t, 3>{255, 255, 255} : std::array<uint8_t, 3>{200, 10, 20})
            );
        }
    OA_CHECK(at(1, 4) == (std::array<uint8_t, 3>{0, 0, 0}));
    // The clipped dot covers columns 9 to 11 of rows 0 to 2.
    OA_CHECK(at(9, 0) == (std::array<uint8_t, 3>{0, 0, 250}));
    OA_CHECK(at(11, 2) == (std::array<uint8_t, 3>{0, 0, 250}));
    OA_CHECK(at(10, 3) == (std::array<uint8_t, 3>{0, 0, 0}));
}

void a_dot_covers_the_pixels_drawn_in_its_colour() {
    constexpr int32_t width = 11, height = 10;
    std::vector<uint8_t> picture(static_cast<std::size_t>(width) * height * 3, 0);
    const std::array<FarViewDot, 1> dot{{{5, 4, {9, 99, 199}, true, {255, 255, 255}}}};
    oa::app::draw_far_view_dots(picture.data(), width, height, dot);
    int32_t covered = 0;
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) {
            const auto* pixel = &picture[(static_cast<std::size_t>(y) * width + x) * 3];
            const bool drawn = pixel[0] == 9 && pixel[1] == 99 && pixel[2] == 199;
            const bool covers = oa::app::far_view_dot_covers(5, 4, x, y);
            OA_CHECK(covers == drawn);
            covered += covers ? 1 : 0;
        }
    OA_CHECK(covered == oa::app::far_view_dot_side * oa::app::far_view_dot_side);
}

void the_view_goes_past_the_map_until_its_edge_reaches_the_middle() {
    using oa::app::held_camera;
    using oa::app::held_view;
    using oa::app::view_centre_span;
    constexpr double half = 0.5;
    // A view narrower than the map: its centre stays on the map.
    auto span = view_centre_span(1000.0, 400.0, half);
    OA_CHECK(span.least == 0.0 && span.most == 1000.0);
    // A view wider than the map: the map's centre stays in the view.
    span = view_centre_span(1000.0, 3000.0, half);
    OA_CHECK(span.least == -1000.0 && span.most == 2000.0);
    // The two meet at the map's size.
    span = view_centre_span(1000.0, 1000.0, half);
    OA_CHECK(span.least == 0.0 && span.most == 1000.0);
    // A view within the span stays; one past it comes to its edge.
    OA_CHECK(held_view(-150.0, 400.0, 1000.0, half, std::nullopt) == -150.0);
    OA_CHECK(held_view(-250.0, 400.0, 1000.0, half, std::nullopt) == -200.0);
    OA_CHECK(held_view(900.0, 400.0, 1000.0, half, std::nullopt) == 800.0);
    OA_CHECK(held_view(-2500.0, 3000.0, 1000.0, half, std::nullopt) == -2500.0);
    OA_CHECK(held_view(-2600.0, 3000.0, 1000.0, half, std::nullopt) == -2500.0);
    OA_CHECK(held_view(600.0, 3000.0, 1000.0, half, std::nullopt) == 500.0);
    // A view held from one past the span goes no further from the map, and
    // never back toward it.
    OA_CHECK(held_view(-400.0, 400.0, 1000.0, half, -150.0) == -350.0);
    OA_CHECK(held_view(-320.0, 400.0, 1000.0, half, -150.0) == -320.0);
    OA_CHECK(held_view(-100.0, 400.0, 1000.0, half, -150.0) == -100.0);
    OA_CHECK(held_view(1000.0, 400.0, 1000.0, half, 1150.0) == 950.0);
    // A camera within a map pixel of the limits stays; one past them comes
    // to the nearest whole map pixel within them.
    OA_CHECK(held_camera(-200, 400.0, 1000.0, half, std::nullopt) == -200);
    OA_CHECK(held_camera(-201, 401.0, 1000.0, half, std::nullopt) == -201);
    OA_CHECK(held_camera(-210, 400.0, 1000.0, half, std::nullopt) == -200);
    OA_CHECK(held_camera(-210, 401.0, 1000.0, half, std::nullopt) == -200);
    OA_CHECK(held_camera(-210, 403.0, 1000.0, half, std::nullopt) == -201);
    OA_CHECK(held_camera(812, 400.0, 1000.0, half, std::nullopt) == 800);
    OA_CHECK(held_camera(812, 401.0, 1000.0, half, std::nullopt) == 799);
    OA_CHECK(held_camera(-500, 400.0, 1000.0, half, -150.0) == -350);
}

void a_smaller_share_keeps_more_of_the_view_on_the_map() {
    using oa::app::held_view;
    using oa::app::view_centre_span;
    constexpr double quarter = 0.25;
    constexpr double none = 0.0;
    // A quarter: a view narrower than the map shows at most a quarter of
    // itself past either edge; one wider keeps the map's centre within a
    // quarter of the view of the view's centre. The two meet at the map's
    // size.
    auto span = view_centre_span(1000.0, 400.0, quarter);
    OA_CHECK(span.least == 100.0 && span.most == 900.0);
    span = view_centre_span(1000.0, 3000.0, quarter);
    OA_CHECK(span.least == -250.0 && span.most == 1250.0);
    span = view_centre_span(1000.0, 1000.0, quarter);
    OA_CHECK(span.least == 250.0 && span.most == 750.0);
    OA_CHECK(held_view(-250.0, 400.0, 1000.0, quarter, std::nullopt) == -100.0);
    OA_CHECK(held_view(-2000.0, 3000.0, 1000.0, quarter, std::nullopt) == -1750.0);
    // None: a view narrower than the map stays on it, as 3.1c's does; one
    // wider has the map at its centre.
    span = view_centre_span(1000.0, 400.0, none);
    OA_CHECK(span.least == 200.0 && span.most == 800.0);
    span = view_centre_span(1000.0, 3000.0, none);
    OA_CHECK(span.least == 500.0 && span.most == 500.0);
    OA_CHECK(held_view(-250.0, 400.0, 1000.0, none, std::nullopt) == 0.0);
    OA_CHECK(held_view(900.0, 400.0, 1000.0, none, std::nullopt) == 600.0);
    OA_CHECK(held_view(0.0, 3000.0, 1000.0, none, std::nullopt) == -1000.0);
}

/// Known physical sizes mixed with rare large units and a hidden enemy.
struct UnitSizeArmy {
    std::unique_ptr<oa::World> world{std::make_unique<oa::World>()};
    std::array<oa::Unit, 12> units{};
    std::array<oa::UnitDef, 6> types{};
    std::array<oa::app::unit_detail_transition::Geometry, 7> geometry{};

    UnitSizeArmy() {
        world->units = units.data();
        world->unit_slot_count = static_cast<uint32_t>(units.size());
        world->unit_defs = types.data();
        world->unit_def_count = static_cast<uint32_t>(types.size());
        constexpr int32_t fixed_scale = 65536;
        const auto size = [&](std::size_t type, int32_t x, int32_t y, int32_t z) {
            types[type].size_x = x * fixed_scale;
            types[type].size_y = y * fixed_scale;
            types[type].size_z = z * fixed_scale;
            geometry[type + 1].dimensions = {
                static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)
            };
        };
        size(0, 20, 10, 10);
        size(1, 40, 20, 20);
        size(2, 200, 200, 200);
        types[2].abilities = OA_UNIT_DEF_ABILITY_COMMANDER;
        size(3, 1000, 1000, 1000);
        size(4, 500, 500, 500);
        for (uint32_t slot = 1; slot <= 7; ++slot) {
            units[slot].flags = OA_UNIT_FLAG_LIVE;
            units[slot].health = 100;
            units[slot].def = 1;
            units[slot].type_index = 1;
        }
        units[4].def = 2;
        units[4].type_index = 2;
        units[5].def = 3;
        units[5].type_index = 3;
        units[6].def = 4;
        units[6].type_index = 4;
        units[6].flags |= OA_UNIT_FLAG_BUILDING;
        units[7].def = 5;
        units[7].type_index = 5;
        units[7].owner_index = 1;
    }
};

/// A small body whose footprint and nondrawable vertices are much larger.
oa::formats::objects3d::Model physical_size_model() {
    namespace mesh = oa::formats::objects3d;
    constexpr int32_t fixed_scale = mesh::kThreeDoUnitsPerWorldUnit;
    const auto point = [](int32_t x, int32_t y, int32_t z) {
        return mesh::FixedVector3{x * fixed_scale, y * fixed_scale, z * fixed_scale};
    };
    mesh::Model model;
    auto& body = model.objects.emplace_back();
    body.vertices = {
        point(0, 0, 0),
        point(6, 0, 0),
        point(0, 10, 0),
        point(6, 10, 0),
        point(0, 0, 4),
        point(6, 0, 4),
        point(0, 10, 4),
        point(6, 10, 4),
        point(1000, 0, 1000), // unreferenced
        point(-200, 0, -200),
        point(200, 0, -200),
        point(-200, 1, 200),
        point(-400, 0, -400),
        point(400, 0, -400),
        point(-400, 1, 400)
    };
    const std::array<std::array<uint16_t, 4>, 6> faces{
        {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}}
    };
    for (const auto& face : faces) {
        auto& polygon = body.primitives.emplace_back();
        polygon.is_colored = 1;
        polygon.color_index = 37;
        polygon.vertex_indices.assign(face.begin(), face.end());
    }
    body.selection_primitive = static_cast<int32_t>(body.primitives.size());
    auto& selection = body.primitives.emplace_back();
    selection.is_colored = 1;
    selection.vertex_indices = {9, 10, 11};
    auto& invisible = body.primitives.emplace_back();
    invisible.vertex_indices = {12, 13, 14};
    return model;
}

void loaded_mesh_size_is_independent_of_collision_bounds() {
    namespace detail = oa::app::unit_detail_transition;
    namespace mesh = oa::formats::objects3d;

    struct LoadedType {
        std::shared_ptr<const mesh::Model> model{};
    };

    std::array<LoadedType, 2> loaded{};
    loaded[1].model = std::make_shared<mesh::Model>(physical_size_model());
    const auto collision = mesh::derive_unit_type_bounds(*loaded[1].model, 2, 2);
    OA_CHECK(collision.ok());
    if (!collision.ok())
        return;
    constexpr int32_t fixed_scale = mesh::kThreeDoUnitsPerWorldUnit;
    OA_CHECK(collision.value->size_x == 32 * fixed_scale);
    OA_CHECK(collision.value->size_z == 32 * fixed_scale);
    OA_CHECK(
        detail::derive_geometry(*loaded[1].model).dimensions == (std::array<double, 3>{6, 10, 4})
    );

    UnitSizeArmy army;
    for (std::size_t slot = 2; slot < army.units.size(); ++slot)
        army.units[slot].flags = 0;
    army.types[0].size_x = collision.value->size_x;
    army.types[0].size_y = collision.value->size_y;
    army.types[0].size_z = collision.value->size_z;
    detail::GeometryCache cache;
    auto geometry = cache.prepare(loaded);
    const detail::Projection classic{1, 0, 0, 0, -0.5F, 1};
    auto average = detail::average_model_pixels(*army.world, classic, geometry);
    // The physical 6x10x4 body spans nine screen pixels in this view;
    // the 32x10x32 targeting box would span thirty-seven.
    OA_CHECK(average.units == 1 && average.pixels == 9);
    army.types[0].size_x *= 4;
    army.types[0].size_z *= 4;
    OA_CHECK(detail::average_model_pixels(*army.world, classic, geometry).pixels == 9);

    std::weak_ptr<const mesh::Model> old_model = loaded[1].model;
    auto replacement = std::make_shared<mesh::Model>(*loaded[1].model);
    replacement->objects.front().first_child = 1;
    auto& child = replacement->objects.emplace_back();
    child.parent = 0;
    child.offset_from_parent.x = 8 * fixed_scale;
    child.vertices = {{0, 0, 0}, {2 * fixed_scale, 0, 0}, {0, 2 * fixed_scale, 2 * fixed_scale}};
    auto& child_face = child.primitives.emplace_back();
    child_face.is_colored = 1;
    child_face.vertex_indices = {0, 1, 2};
    loaded[1].model = replacement;
    OA_CHECK(old_model.expired());
    geometry = cache.prepare(loaded);
    OA_CHECK(geometry[1].dimensions == (std::array<double, 3>{10, 10, 4}));
    average = detail::average_model_pixels(*army.world, classic, geometry);
    OA_CHECK(average.units == 1 && average.pixels == 10);
    army.units[1].type_index = static_cast<uint16_t>(geometry.size());
    OA_CHECK(detail::average_model_pixels(*army.world, classic, geometry).units == 0);
    loaded[1].model.reset();
    geometry = cache.prepare(loaded);
    OA_CHECK(geometry[1].dimensions == (std::array<double, 3>{}));
}

void the_automatic_boundary_uses_physical_population_size() {
    namespace detail = oa::app::unit_detail_transition;
    const detail::Projection classic{1, 0, 0, 0, -0.5F, 1};
    UnitSizeArmy army;
    auto average = detail::average_model_pixels(*army.world, classic, army.geometry);
    // Three 20-pixel units and one 40-pixel unit contribute by population.
    // The commander, building and hidden enemy cannot dominate that army.
    OA_CHECK(average.units == 4 && average.pixels == 25);
    army.geometry[5].dimensions[0] *= 4;
    OA_CHECK(detail::average_model_pixels(*army.world, classic, army.geometry).pixels == 25);
    army.units[4].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    average = detail::average_model_pixels(*army.world, classic, army.geometry);
    OA_CHECK(average.units == 3 && average.pixels == 20);
    army.units[1].health = 0;
    army.units[2].build_remaining = 0.5F;
    army.units[3].flags = 0;
    average = detail::average_model_pixels(*army.world, classic, army.geometry);
    OA_CHECK(average.units == 2 && average.pixels == 900);
    army.units[5].flags = army.units[6].flags = 0;
    OA_CHECK(detail::average_model_pixels(*army.world, classic, army.geometry).units == 0);
}

void physical_size_follows_camera_and_unit_turn() {
    namespace detail = oa::app::unit_detail_transition;
    detail::Geometry type{{40, 5, 10}};
    const detail::Projection side{1, 0, 0, 0, 1, 0};
    constexpr uint16_t quarter_turn = 16384;
    OA_CHECK(detail::model_pixels(type, side) == 40);
    OA_CHECK(std::abs(detail::model_pixels(type, side, quarter_turn) - 10) < 0.001);
    // A camera looking along x gives the opposite apparent sizes.
    const detail::Projection other_side{0, 0, 1, 0, 1, 0};
    OA_CHECK(detail::model_pixels(type, other_side) == 10);
    OA_CHECK(std::abs(detail::model_pixels(type, other_side, quarter_turn) - 40) < 0.001);
    auto invalid = side;
    invalid[0] = std::numeric_limits<float>::infinity();
    OA_CHECK(detail::model_pixels(type, invalid) == 0);
    type.dimensions[0] = -1;
    OA_CHECK(detail::model_pixels(type, side) == 0);
}

void unit_size_keeps_models_farther_out_and_does_not_flicker() {
    namespace detail = oa::app::unit_detail_transition;
    const detail::Projection classic{1, 0, 0, 0, -0.5F, 1};
    UnitSizeArmy army;
    // An exact 24-pixel population mean reaches six pixels at zoom 1/4.
    army.geometry[2].dimensions[0] = 36;
    detail::State before;
    OA_CHECK(detail::as_dots(before, *army.world, 10, classic, 0.249F, army.geometry));
    detail::State at;
    OA_CHECK(!detail::as_dots(at, *army.world, 10, classic, 0.25F, army.geometry));
    // The old fixed 1/3 choice would already show dots at these zooms.
    detail::State moving;
    OA_CHECK(!detail::as_dots(moving, *army.world, 10, classic, 0.26F, army.geometry));
    OA_CHECK(!detail::as_dots(moving, *army.world, 10, classic, 0.249F, army.geometry));
    OA_CHECK(detail::as_dots(moving, *army.world, 10, classic, 0.24F, army.geometry));
    OA_CHECK(detail::as_dots(moving, *army.world, 10, classic, 0.25F, army.geometry));
    OA_CHECK(!detail::as_dots(moving, *army.world, 10, classic, 0.26F, army.geometry));
    OA_CHECK(moving.average.units == 4 && moving.average.pixels == 24);
    // Tick and viewpoint changes refresh the representative population.
    army.units[4].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    OA_CHECK(detail::as_dots(moving, *army.world, 11, classic, 0.26F, army.geometry));
    OA_CHECK(moving.average.units == 3 && moving.average.pixels == 20);
    army.world->game.viewpoint_player = 1;
    OA_CHECK(!detail::as_dots(moving, *army.world, 11, classic, 0.26F, army.geometry));
    OA_CHECK(moving.average.units == 1 && moving.average.pixels == 750);
    // A match with no owned physical model retains the earlier fallback.
    oa::World empty{};
    detail::State fallback;
    OA_CHECK(detail::as_dots(fallback, empty, 0, classic, 0.1F, {}));
    OA_CHECK(!detail::as_dots(fallback, empty, 0, classic, 0.2F, {}));
    OA_CHECK(!detail::as_dots(fallback, empty, 0, classic, 0, {}));
}

} // namespace

int main() {
    the_whole_map_fits_one_way_and_fills_the_other();
    automatic_keeps_the_drawings_floor();
    each_choice_stops_at_its_share_or_the_whole_map();
    frames_past_the_detail_floor_draw_the_far_view();
    the_pyramid_holds_the_means_of_the_map_pixels();
    each_zoom_reads_its_level();
    the_filter_averages_the_texels_each_pixel_covers();
    dots_are_framed_when_selected_and_clipped();
    a_dot_covers_the_pixels_drawn_in_its_colour();
    the_view_goes_past_the_map_until_its_edge_reaches_the_middle();
    a_smaller_share_keeps_more_of_the_view_on_the_map();
    loaded_mesh_size_is_independent_of_collision_bounds();
    the_automatic_boundary_uses_physical_population_size();
    physical_size_follows_camera_and_unit_turn();
    unit_size_keeps_models_farther_out_and_does_not_flicker();
    return oa::test::check_exit_status();
}
