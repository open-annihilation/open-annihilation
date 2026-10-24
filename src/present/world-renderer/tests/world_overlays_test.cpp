// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/present/world_renderer/world_display_modes.hpp"
#include "oa/present/world_renderer/world_overlays.hpp"
#include "oa/present/world_renderer/world_radar.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace wr = oa::present::world_renderer;

namespace {

enum Kind { line, outline, fill };

struct Call {
    Kind kind = line;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint8_t color = 0;
};

struct Log {
    std::vector<Call> calls;
};

wr::OverlayRaster recorder(Log& log) {
    wr::OverlayRaster raster;
    raster.user = &log;
    raster.line =
        [](void* u, oa::Surface*, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
            static_cast<Log*>(u)->calls.push_back({line, x0, y0, x1, y1, color});
        };
    raster.rect_outline = [](void* u, oa::Surface*, const oa::Rect32& r, uint8_t color) {
        static_cast<Log*>(u)->calls.push_back({outline, r.x1, r.y1, r.x2, r.y2, color});
    };
    raster.fill_rect = [](void* u, oa::Surface*, const oa::Rect32& r, uint8_t color) {
        static_cast<Log*>(u)->calls.push_back({fill, r.x1, r.y1, r.x2, r.y2, color});
    };
    return raster;
}

bool same(const Call& c, Kind kind, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
    return c.kind == kind && c.x0 == x0 && c.y0 == y0 && c.x1 == x1 && c.y1 == y1 &&
           c.color == color;
}

std::unique_ptr<oa::Game> make_game() {
    auto game = std::make_unique<oa::Game>();
    std::memset(game.get(), 0, sizeof(oa::Game));
    for (int i = 0; i < 16; ++i)
        game->ui_colors[i] = static_cast<uint8_t>(0xa0 + i);
    return game;
}

void test_projected_debug_grid() {
    auto game = make_game();
    game->map_width = 2;
    game->map_height = 2;
    game->offscreen_width = 640;
    game->offscreen_height = 480;
    game->debug_overlay = wr::debug_view::terrain;
    // The rotated view may show cells outside the classic camera bounds.
    game->camera_x = 1000;
    game->camera_y = 1000;
    oa::MapPlot plots[4]{};
    for (auto& plot : plots) {
        plot.feature = 0xffff;
        plot.height = 20;
    }
    wr::DebugGridSources sources;
    sources.plots = plots;
    sources.project = [](void*, int32_t x, int32_t h, int32_t z, int32_t& sx, int32_t& sy) {
        sx = 200 + x + z;
        sy = 100 + z - x - h;
    };
    Log log;
    wr::overlay_debug_grid(*game, recorder(log), nullptr, sources);
    OA_CHECK(log.calls.size() == 2);
    if (log.calls.size() == 2) {
        OA_CHECK(log.calls[0].x0 == 200 && log.calls[0].y0 == 80);
        OA_CHECK(log.calls[0].x1 == 216 && log.calls[0].y1 == 64);
        OA_CHECK(log.calls[1].x1 == 216 && log.calls[1].y1 == 96);
    }
}

void test_traffic_and_meter() {
    auto game = make_game();
    Log log;
    const auto raster = recorder(log);
    wr::overlay_traffic_bar(*game, raster, nullptr, {10, 20, 110, 25}, 250);
    OA_CHECK(log.calls.size() == 2);
    OA_CHECK(same(log.calls[0], outline, 10, 20, 110, 25, 0xaf));
    OA_CHECK(same(log.calls[1], fill, 10, 20, 110, 25, 0xaf));
    log.calls.clear();
    wr::overlay_traffic_bar(*game, raster, nullptr, {10, 20, 110, 25}, 37);
    OA_CHECK(same(log.calls[1], fill, 10, 20, 47, 25, 0xaf));
    log.calls.clear();
    wr::overlay_traffic_bar(*game, raster, nullptr, {10, 20, 110, 25}, 0);
    OA_CHECK(log.calls.size() == 1);

    log.calls.clear();
    uint8_t colors[12]{};
    colors[4] = 7;
    colors[10] = 9;
    wr::overlay_meter_bar(raster, nullptr, 3, 4, {0, 0, 40, 5}, colors, 100);
    OA_CHECK(log.calls.size() == 2);
    OA_CHECK(same(log.calls[0], fill, 0, 100, 30, 105, 9));
    OA_CHECK(same(log.calls[1], fill, 31, 100, 40, 105, 7));
    log.calls.clear();
    wr::overlay_meter_bar(raster, nullptr, 9, 4, {0, 0, 40, 5}, colors, 0);
    OA_CHECK(log.calls.size() == 1 && same(log.calls[0], fill, 0, 0, 40, 5, 9));
}

void test_cell_outline() {
    auto game = make_game();
    game->camera_x = 64;
    game->camera_y = 32;
    Log log;
    const auto raster = recorder(log);
    wr::overlay_cell_outline(*game, raster, nullptr, 10, 5, 2, 3, 20, 2);
    // x1 = (10 + 8) * 16 - 64 = 224; y1 = 80 - 32 - 10 + 32 = 70.
    OA_CHECK(log.calls.size() == 1);
    OA_CHECK(same(log.calls[0], outline, 224, 70, 256, 118, 0xa2));
    log.calls.clear();
    wr::overlay_cell_outline(*game, raster, nullptr, 10, 5, 2, 3, 20, wr::cell_outline_inset_slot);
    OA_CHECK(same(log.calls[0], outline, 225, 71, 255, 117, 0xa4));
}

void test_rotated_box() {
    auto game = make_game();
    Log log;
    const auto raster = recorder(log);
    const oa::FixedVec3 corners[4] = {
        {-(8 << 16), 0, -(4 << 16)},
        {8 << 16, 0, -(4 << 16)},
        {8 << 16, 0, 4 << 16},
        {-(8 << 16), 0, 4 << 16},
    };
    wr::overlay_rotated_box(*game, raster, nullptr, {100 << 16, 20 << 16, 50 << 16}, corners, {});
    OA_CHECK(log.calls.size() == 4);
    // Unrotated: x = 92/108 + 128, y = (50 -/+ 4) - 10 + 32 (z grows upward on screen).
    OA_CHECK(same(log.calls[0], line, 220, 76, 236, 76, 0xaa));
    OA_CHECK(same(log.calls[1], line, 236, 76, 236, 68, 0xaa));
    OA_CHECK(same(log.calls[3], line, 220, 68, 220, 76, 0xaa));
}

void test_selection_box() {
    auto game = make_game();
    game->camera_x = 64;
    game->camera_y = 32;
    oa::Unit unit{};
    unit.position = {200 << 16, 20 << 16, 100 << 16};
    oa::formats::objects3d::Object root;
    root.vertices = {
        {2 << 16, 0, -(1 << 16)},
        {10 << 16, 0, -(1 << 16)},
        {10 << 16, 0, -(6 << 16)},
        {2 << 16, 0, -(6 << 16)},
    };
    Log log;
    const auto raster = recorder(log);
    wr::overlay_selection_box(*game, raster, nullptr, unit, root);
    OA_CHECK(log.calls.empty());
    game->console_flags = wr::console_flag_selection_boxes;
    wr::overlay_selection_box(*game, raster, nullptr, unit, root);
    // Loaded X and Z are negated: x spans -10..0 and z 0..6 (the zero start).
    // Screen x = 200 - 64 + x + 128, y = 100 - 32 - z - 20 / 2 + 32.
    OA_CHECK(log.calls.size() == 4);
    OA_CHECK(same(log.calls[0], line, 254, 90, 264, 90, 0xaa));
    OA_CHECK(same(log.calls[1], line, 264, 90, 264, 84, 0xaa));
    OA_CHECK(same(log.calls[2], line, 264, 84, 254, 84, 0xaa));
    OA_CHECK(same(log.calls[3], line, 254, 84, 254, 90, 0xaa));

    // An offset root spanning both signs, turned a quarter by the heading word
    // (Unit.heading, the xz pair): the bounds are x -5..5, y -2..8, z -1..7, the
    // corners sit at y -2, and the turn maps (x, z) to (-z, x).
    log.calls.clear();
    root.offset_from_parent = {1 << 16, 3 << 16, -(2 << 16)};
    root.vertices = {
        {4 << 16, -(1 << 16), 3 << 16},
        {-(6 << 16), 5 << 16, 3 << 16},
        {-(6 << 16), -(5 << 16), -(5 << 16)},
    };
    unit.heading = 0x4000;
    wr::overlay_selection_box(*game, raster, nullptr, unit, root);
    // Screen x = 136 + x' + 128, y = 68 - z' - (20 - 2) / 2 + 32.
    OA_CHECK(log.calls.size() == 4);
    OA_CHECK(same(log.calls[0], line, 265, 96, 265, 86, 0xaa));
    OA_CHECK(same(log.calls[1], line, 265, 86, 257, 86, 0xaa));
    OA_CHECK(same(log.calls[2], line, 257, 86, 257, 96, 0xaa));
    OA_CHECK(same(log.calls[3], line, 257, 96, 265, 96, 0xaa));
}

void test_contours() {
    Log log;
    const auto raster = recorder(log);
    const wr::ContourStyle style{16 << 8, 0, 0x20};
    wr::overlay_contour_triangle(
        raster, nullptr, style, {0, 0, 0x5000}, {100, 0, 0x1000}, {0, 100, 0x3000}
    );
    // Sorted: A=(100,0,0x10), B=(0,100,0x30), C=(0,0,0x50); levels 0x50..0x20.
    OA_CHECK(log.calls.size() == 4);
    const auto ramp = [](int32_t level) { return wr::contour_ramp[(level - 0x20 + 0x100) >> 4]; };
    OA_CHECK(same(log.calls[0], line, 0, 0, 0, 0, ramp(0x50)));
    OA_CHECK(same(log.calls[1], line, 25, 0, 0, 50, ramp(0x40)));
    OA_CHECK(same(log.calls[2], line, 50, 0, 0, 100, ramp(0x30)));
    OA_CHECK(same(log.calls[3], line, 75, 0, 50, 50, ramp(0x20)));

    log.calls.clear();
    wr::overlay_contour_triangle(
        raster, nullptr, {0, 0, 0}, {0, 0, 0}, {1, 1, 0x100}, {2, 2, 0x200}
    );
    OA_CHECK(log.calls.empty());

    const int32_t quad[8] = {0, 0, 32, 0, 32, 32, 0, 32};
    const uint8_t flat[4] = {5, 5, 5, 5};
    wr::overlay_contour_quad(raster, nullptr, style, quad, flat);
    OA_CHECK(log.calls.empty());
    const uint8_t slope[4] = {0x00, 0x40, 0x40, 0x00};
    wr::overlay_contour_quad(raster, nullptr, style, quad, slope);
    OA_CHECK(!log.calls.empty());
    for (const auto& call : log.calls) {
        OA_CHECK(call.kind == line);
        OA_CHECK(call.x0 >= 0 && call.x0 <= 32 && call.x1 >= 0 && call.x1 <= 32);
    }
}

void test_display_modes() {
    wr::DisplayMode modes[] = {
        {1024, 768, 8},
        {640, 400, 8},
        {800, 600, 8},
        {640, 480, 8},
        {320, 200, 8},
        {1024, 600, 8},
        {1280, 1024, 8},
    };
    const auto count = wr::sort_display_modes(modes, 7);
    OA_CHECK(count == 5);
    const int32_t expected[5][2] = {{640, 480}, {800, 600}, {1024, 600}, {1024, 768}, {1280, 1024}};
    for (int i = 0; i < 5; ++i)
        OA_CHECK(modes[i].width == expected[i][0] && modes[i].height == expected[i][1]);

    // Kept only from 768 rows: any width at that height or taller stays.
    wr::DisplayMode tall[] = {
        {1024, 768, 8}, {800, 600, 8}, {640, 800, 8}, {1280, 720, 8}, {1280, 1024, 8}
    };
    const auto tall_count = wr::sort_display_modes(tall, 5, wr::tall_minimum_mode_height);
    OA_CHECK(tall_count == 3);
    const int32_t tall_expected[3][2] = {{640, 800}, {1024, 768}, {1280, 1024}};
    for (int i = 0; i < 3; ++i)
        OA_CHECK(tall[i].width == tall_expected[i][0] && tall[i].height == tall_expected[i][1]);

    wr::DisplayMode list[] = {{800, 600, 8}, {640, 480, 8}, {1024, 768, 8}};
    wr::DisplayMode chosen{};
    OA_CHECK(wr::cycle_display_mode(list, 3, 1024, 768, false, chosen));
    OA_CHECK(chosen.width == 640 && chosen.height == 480);
    wr::DisplayMode list2[] = {{800, 600, 8}, {640, 480, 8}, {1024, 768, 8}};
    OA_CHECK(wr::cycle_display_mode(list2, 3, 640, 480, true, chosen));
    OA_CHECK(chosen.width == 1024);
    OA_CHECK(!wr::cycle_display_mode(list2, 3, 1600, 1200, false, chosen));
}

void test_present_binding() {
    auto game = make_game();
    std::vector<uint8_t> pixels(64 * 16, 0);
    oa::Surface surface{};
    surface.width = 64;
    surface.height = 16;
    surface.pitch = 64;
    surface.pixels = pixels.data();
    surface.clip = {0, 0, 63, 15};
    const auto raster = wr::present_overlay_raster();
    wr::overlay_traffic_bar(*game, raster, &surface, {2, 2, 61, 9}, 50);
    // Outline corners and the half-filled interior; right interior stays clear.
    OA_CHECK(pixels[2 * 64 + 2] == 0xaf && pixels[9 * 64 + 61] == 0xaf);
    OA_CHECK(pixels[5 * 64 + 20] == 0xaf);
    OA_CHECK(pixels[5 * 64 + 50] == 0);
    wr::overlay_traffic_bar(*game, raster, &surface, {-10, -4, 100, 40}, 100);
    OA_CHECK(pixels[0] == 0xaf && pixels[15 * 64 + 63] == 0xaf);
}

// The terrain view drawn through the present raster: a plot's own colour and
// a feature's colour (feature - 0x38) fill their cells, edges take UI colour
// 0xD at or under sea level and 0xF above it.
void test_debug_grid_terrain() {
    auto game = make_game();
    constexpr int width = 4, height = 4, pitch = 200;
    game->map_width = width;
    game->map_height = height;
    game->view_cells_width = 2;
    game->offscreen_height = 480;
    game->sea_level = 10;
    game->debug_overlay = wr::debug_view::terrain;
    std::vector<oa::MapPlot> plots(width * height);
    for (auto& plot : plots)
        plot.feature = 0xffff;
    plots[0].ground_unit = 0x33;
    plots[1].feature = 0x40;
    plots[1 * width + 2].height = 20;
    plots[1 * width + 3].height = 20;
    std::vector<uint8_t> pixels(pitch * 100, 0);
    oa::Surface surface{};
    surface.width = pitch;
    surface.height = 100;
    surface.pitch = pitch;
    surface.pixels = pixels.data();
    surface.clip = {0, 0, pitch - 1, 99};
    wr::DebugGridSources sources;
    sources.plots = plots.data();
    wr::overlay_debug_grid(*game, wr::present_overlay_raster(), &surface, sources);
    OA_CHECK(pixels[40 * pitch + 136] == 0x33);
    OA_CHECK(pixels[40 * pitch + 152] == 0x08);
    OA_CHECK(pixels[48 * pitch + 136] == 0xad);
    OA_CHECK(pixels[38 * pitch + 168] == 0xaf);
    // Off (and no contour spacing) draws nothing.
    std::fill(pixels.begin(), pixels.end(), uint8_t{0});
    game->debug_overlay = wr::debug_view::off;
    wr::overlay_debug_grid(*game, wr::present_overlay_raster(), &surface, sources);
    OA_CHECK(std::count(pixels.begin(), pixels.end(), 0) == static_cast<long>(pixels.size()));
}

// A category at 25% of the shown total: a 50-pixel bar in colour category + 1
// left of x = width - 0x5A, inside the frame drawn in colour 0xFF.
void test_profile_bar() {
    auto game = make_game();
    constexpr int pitch = 640, rows = 200, font_height = 10;
    game->profile_times.shown[2] = 25;
    game->profile_times.shown_total = 100;
    std::vector<uint8_t> pixels(pitch * rows, 0);
    oa::Surface surface{};
    surface.width = pitch;
    surface.height = rows;
    surface.pitch = pitch;
    surface.pixels = pixels.data();
    surface.clip = {0, 0, pitch - 1, rows - 1};
    wr::overlay_profile_bar(
        *game, wr::present_overlay_raster(), &surface, pitch, font_height, "Logic", 2
    );
    const int y = font_height * 2 + 0x28 + 5;
    OA_CHECK(pixels[y * pitch + 500] == 3 && pixels[y * pitch + 550] == 3);
    OA_CHECK(pixels[y * pitch + 499] == 0 && pixels[y * pitch + 551] == 0);
    OA_CHECK(pixels[0x26 * pitch + 640 - 0x122] == 0xff);
    OA_CHECK(pixels[(font_height * 9 + 0x29) * pitch + 0x27f] == 0xff);
}

} // namespace

int main() {
    test_projected_debug_grid();
    test_debug_grid_terrain();
    test_profile_bar();
    test_present_binding();
    test_display_modes();
    test_traffic_and_meter();
    test_cell_outline();
    test_rotated_box();
    test_selection_box();
    test_contours();
    return oa::test::check_exit_status();
}
