// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/world_renderer/world_overlays.hpp"
#include "oa/core/map_plot.h"
#include "oa/sim/visibility_state.hpp"

#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/present/world_renderer/world_radar.hpp"

#include "oa/present/display.hpp"
#include "oa/present/polygon.hpp"
#include "oa/present/raster.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace oa::present::world_renderer {
namespace {

constexpr int32_t debug_grid_corners = 4;

void draw_line(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    uint8_t color
) {
    if (raster.line != nullptr)
        raster.line(raster.user, surface, x0, y0, x1, y1, color);
}

void rect_outline(
    const OverlayRaster& raster, ::oa::Surface* surface, const Rect32& rect, uint8_t color
) {
    if (raster.rect_outline != nullptr)
        raster.rect_outline(raster.user, surface, rect, color);
}

void present_line(
    void*, ::oa::Surface* surface, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color
) {
    (void)present::draw_clipped_line(surface, x0, y0, x1, y1, color);
}

void present_rect_outline(void*, ::oa::Surface* surface, const Rect32& rect, uint8_t color) {
    (void)present::draw_rect_outline(surface, rect, color);
}

void present_fill_rect(void*, ::oa::Surface* surface, const Rect32& rect, uint8_t color) {
    (void)present::fill_clipped_rect(surface, rect, color);
}

void present_polygon(
    void*, ::oa::Surface* surface, const int32_t* points, int32_t count, uint8_t color
) {
    present::PolygonVertex vertices[debug_grid_corners];
    if (count > debug_grid_corners)
        count = debug_grid_corners;
    for (int32_t i = 0; i < count; ++i)
        vertices[i] = {points[i * 2], points[i * 2 + 1]};
    (void)present::fill_polygon(surface, vertices, count, color);
}

void present_set_font(void*, const void* font) {
    present::set_active_font(font);
}

uint32_t present_text_transparent(void*) {
    return present::text_transparent();
}

void present_set_text_colors(void*, int32_t color, int32_t background) {
    present::set_text_colors(color, background);
}

void present_text(void*, ::oa::Surface* surface, const char* text, int32_t x, int32_t y) {
    present::draw_text(surface, text, x, y, present::text_width_unbounded);
}

void fill_rect(
    const OverlayRaster& raster, ::oa::Surface* surface, const Rect32& rect, uint8_t color
) {
    if (raster.fill_rect != nullptr)
        raster.fill_rect(raster.user, surface, rect, color);
}

// 32-bit two's-complement product, as the game computes it.
int32_t mul32(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

int32_t add32(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t sub32(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

// Point at `t` of `span` along p->q (integer lerp with C truncation).
int32_t lerp(int32_t p, int32_t q, int32_t t, int32_t span) noexcept {
    return add32(mul32(span - t, p), mul32(t, q)) / span;
}

uint8_t contour_color(int32_t level, uint8_t sea_level) noexcept {
    auto index = ((level >> 8) - static_cast<int32_t>(sea_level) + 0x100) >> 4;
    // Heights stay within one byte, so the index is always in range for
    // valid input; clamp instead of reading past the table.
    if (index < 0)
        index = 0;
    if (index >= contour_ramp_size)
        index = contour_ramp_size - 1;
    return contour_ramp[index];
}

int32_t high_word(oa_fixed value) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(value) >> 16);
}

void draw_polygon(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const int32_t (&points)[debug_grid_corners * 2],
    uint8_t color
) {
    if (raster.polygon != nullptr)
        raster.polygon(raster.user, surface, points, debug_grid_corners, color);
}

// Sets the small font and a text colour over the colour text skips, then
// prints, as the grid's labels do. `color` is produced after the skipped
// colour is read, as the goal mark's random colour is.
template <typename Color>
void print_label(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const void* font,
    const char* text,
    int32_t x,
    int32_t y,
    Color color
) {
    if (raster.set_font != nullptr)
        raster.set_font(raster.user, font);
    const auto background =
        raster.text_transparent != nullptr ? raster.text_transparent(raster.user) : 0U;
    const int32_t foreground = color();
    if (raster.set_text_colors != nullptr)
        raster.set_text_colors(raster.user, foreground, static_cast<int32_t>(background));
    if (raster.text != nullptr)
        raster.text(raster.user, surface, text, x, y);
}

} // namespace

// Colour ramp from sea floor to peaks; below-sea entries are colour 0.
const uint8_t contour_ramp[contour_ramp_size] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a,
    0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51, 0x50, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

OverlayRaster present_overlay_raster() noexcept {
    OverlayRaster raster;
    raster.line = present_line;
    raster.rect_outline = present_rect_outline;
    raster.fill_rect = present_fill_rect;
    raster.polygon = present_polygon;
    raster.set_font = present_set_font;
    raster.text_transparent = present_text_transparent;
    raster.set_text_colors = present_set_text_colors;
    raster.text = present_text;
    return raster;
}

void overlay_rotated_box(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const FixedVec3& origin,
    const FixedVec3 (&corners)[4],
    sim::model_runtime::RotationWords rotation
) {
    const auto color = game_ui_color(game, ui_color_rotated_box);
    int32_t screen[4][2]{};
    for (int i = 0; i < 4; ++i) {
        const auto r =
            sim::model_runtime::rotate_vector({corners[i].x, corners[i].y, corners[i].z}, rotation);
        screen[i][0] = high_word(add32(r.x, origin.x)) + battlefield_origin_x;
        screen[i][1] = (high_word(sub32(origin.z, r.z)) - (high_word(add32(r.y, origin.y)) >> 1)) +
                       battlefield_origin_y;
    }
    for (int i = 0; i < 4; ++i) {
        const auto next = (i + 1) & 3;
        draw_line(
            raster, surface, screen[i][0], screen[i][1], screen[next][0], screen[next][1], color
        );
    }
}

void overlay_selection_box(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const Unit& unit,
    const formats::objects3d::Object& root
) {
    const uint16_t options = game.console_flags;
    if ((options & console_flag_selection_boxes) == 0)
        return;
    const auto [low, high] = formats::objects3d::object_bounds(root);
    const FixedVec3 corners[4] = {
        {low.x, low.y, low.z},
        {high.x, low.y, low.z},
        {high.x, low.y, high.z},
        {low.x, low.y, high.z},
    };
    const FixedVec3 origin{
        sub32(unit.position.x, static_cast<int32_t>(game.camera_x << 16)),
        unit.position.y,
        sub32(unit.position.z, static_cast<int32_t>(game.camera_y << 16)),
    };
    overlay_rotated_box(
        game,
        raster,
        surface,
        origin,
        corners,
        {unit.bank, static_cast<int16_t>(unit.heading), unit.pitch}
    );
}

void overlay_traffic_bar(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    Rect32 rect,
    int32_t percent
) {
    const auto color = game_ui_color(game, ui_color_traffic_bar);
    rect_outline(raster, surface, rect, color);
    if (percent > traffic_bar_full - 1)
        percent = traffic_bar_full;
    if (percent > 0) {
        rect.x2 = add32(mul32(sub32(rect.x2, rect.x1), percent) / traffic_bar_full, rect.x1);
        fill_rect(raster, surface, rect, color);
    }
}

void overlay_meter_bar(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    int32_t value,
    int32_t maximum,
    const Rect32& rect,
    const uint8_t* colors,
    int32_t y_offset
) {
    if (colors == nullptr || maximum == 0)
        return;
    if (value < 0)
        value = 0;
    if (maximum < value)
        value = maximum;
    Rect32 part{rect.x1, rect.y1 + y_offset, 0, rect.y2 + y_offset};
    part.x2 = add32(mul32(sub32(rect.x2, rect.x1), value) / maximum, rect.x1);
    fill_rect(raster, surface, part, colors[10]);
    if (part.x2 != rect.x2) {
        part.x1 = part.x2 + 1;
        part.x2 = rect.x2;
        fill_rect(raster, surface, part, colors[4]);
    }
}

void overlay_cell_outline(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    int16_t cell_x,
    int16_t cell_y,
    int16_t cells_wide,
    int16_t cells_high,
    uint8_t cell_height,
    int32_t color_slot
) {
    const auto top =
        (cell_y * OA_MAP_CELL_PIXELS - static_cast<int32_t>(game.camera_y)) - (cell_height >> 1);
    // (cell + 8) * 16 folds the battlefield x origin into the cell column.
    Rect32 rect{};
    rect.x1 = (cell_x + 8) * OA_MAP_CELL_PIXELS - static_cast<int32_t>(game.camera_x);
    rect.y1 = top + battlefield_origin_y;
    rect.y2 = cells_high * OA_MAP_CELL_PIXELS + rect.y1;
    rect.x2 = cells_wide * OA_MAP_CELL_PIXELS + rect.x1;
    if (color_slot == cell_outline_inset_slot) {
        rect.x1 += 1;
        rect.y1 = top + battlefield_origin_y + 1;
        rect.x2 -= 1;
        rect.y2 -= 1;
    }
    rect_outline(raster, surface, rect, game_ui_color(game, color_slot));
}

void overlay_contour_triangle(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const ContourStyle& style,
    ContourVertex a,
    ContourVertex b,
    ContourVertex c
) {
    if (style.spacing <= 0)
        return;
    if (c.height < b.height) {
        const auto t = b;
        b = c;
        c = t;
    }
    if (b.height < a.height) {
        const auto t = a;
        a = b;
        b = t;
    }
    if (c.height < b.height) {
        const auto t = b;
        b = c;
        c = t;
    }
    auto level = (c.height / style.spacing) * style.spacing + style.phase;
    while (c.height < level)
        level -= style.spacing;
    if (b.height < level) {
        const auto span_ac = c.height - a.height;
        const auto span_bc = c.height - b.height;
        do {
            const auto t_bc = level - b.height;
            const auto t_ac = level - a.height;
            draw_line(
                raster,
                surface,
                lerp(a.x, c.x, t_ac, span_ac),
                lerp(a.y, c.y, t_ac, span_ac),
                lerp(b.x, c.x, t_bc, span_bc),
                lerp(b.y, c.y, t_bc, span_bc),
                contour_color(level, style.sea_level)
            );
            level -= style.spacing;
        } while (b.height < level);
    }
    if (a.height < level) {
        const auto span_ac = c.height - a.height;
        const auto span_ab = b.height - a.height;
        do {
            const auto t = level - a.height;
            draw_line(
                raster,
                surface,
                lerp(a.x, c.x, t, span_ac),
                lerp(a.y, c.y, t, span_ac),
                lerp(a.x, b.x, t, span_ab),
                lerp(a.y, b.y, t, span_ab),
                contour_color(level, style.sea_level)
            );
            level -= style.spacing;
        } while (a.height < level);
    }
}

void overlay_contour_quad(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const ContourStyle& style,
    const int32_t (&points)[8],
    const uint8_t (&heights)[4]
) {
    const ContourVertex centre{
        add32(add32(add32(add32(points[2], points[4]), points[0]), 2), points[6]) / 4,
        add32(add32(add32(add32(points[7], points[5]), points[3]), 2), points[1]) / 4,
        (static_cast<int32_t>(heights[2]) + heights[0] + heights[3] + heights[1]) * 0x40,
    };
    ContourVertex corner[4];
    for (int i = 0; i < 4; ++i)
        corner[i] = {points[i * 2], points[i * 2 + 1], static_cast<int32_t>(heights[i]) << 8};
    for (int i = 0; i < 4; ++i)
        overlay_contour_triangle(raster, surface, style, corner[i], corner[(i + 1) & 3], centre);
}

namespace {

// UI colour slots of the debug grid.
constexpr int32_t ui_color_grid = 0;
constexpr int32_t ui_color_closed_step = 4;
constexpr int32_t ui_color_underwater = 0xd;
constexpr int32_t ui_color_mark = 0xf;
// UI colour slot of each movement class short of clear.
constexpr int32_t movement_class_colors[movement_class_clear] = {4, 0xe, 0xa};
// Search step directions.
constexpr int8_t step_dx[8] = {0, -1, -1, -1, 0, 1, 1, 1};
constexpr int8_t step_dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
constexpr int32_t arrow_shaft = 14;
constexpr int32_t arrow_barb = 4;
constexpr int32_t cell_centre = 8;
constexpr int32_t diamond_inset = 2;
constexpr int32_t metal_label_inset = 2;
constexpr int32_t sight_mark_half = 5;
constexpr int32_t sight_cells_per_plot_cell = 2;
// MapPlot.flags bit the terrain view marks with a diamond.
constexpr uint8_t plot_flag_marked = 0x02;
// MapPlot.feature with no feature; other features fill in feature - 0x38.
constexpr uint16_t plot_no_feature = 0xffff;
constexpr int32_t plot_feature_color_base = 0x38;
constexpr int32_t cross_arm = 2;
// The profile frame: left edge from the screen's right, fixed right edge,
// top, and the bottom's margin below nine rows.
constexpr int32_t profile_frame_left = 0x122;
constexpr int32_t profile_frame_right = 0x27f;
constexpr int32_t profile_frame_top = 0x26;
constexpr int32_t profile_frame_bottom = 0x29;
constexpr int32_t profile_first_row = 0x28;
constexpr int32_t profile_bar_right = 0x5a;
constexpr int32_t profile_label_right = 0x55;
constexpr int32_t profile_rows = 9;
constexpr int32_t profile_pixels_per_percent = 2;
constexpr uint8_t profile_frame_color = 0xff;

int32_t midpoint(int32_t a, int32_t b) noexcept {
    return add32(a, b) / 2;
}

} // namespace

void overlay_debug_grid(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const DebugGridSources& sources
) {
    const auto view = game.debug_overlay;
    if (view == debug_view::off && sources.contour_spacing == 0)
        return;
    if (sources.plots == nullptr)
        return;
    const Player& viewer = game.players[game.viewpoint_player];
    const bool classes = view == debug_view::path_search &&
                         sources.open_movement_class != nullptr &&
                         sources.open_movement_class(sources.user);
    const ContourStyle contour{sources.contour_spacing, sources.contour_phase, game.sea_level};
    const auto camera_x = static_cast<int32_t>(game.camera_x);
    const auto camera_y = static_cast<int32_t>(game.camera_y);
    // A view left of or above the map starts the grid at the map's first
    // cell: there are no cells before it.
    const int32_t first_column =
        sources.project != nullptr ? 0 : std::max(camera_x, 0) / OA_MAP_CELL_PIXELS;
    int32_t end_column = game.view_cells_width + 1 + camera_x / OA_MAP_CELL_PIXELS;
    if (sources.project != nullptr || game.map_width - 1 <= end_column)
        end_column = game.map_width - 1;
    const int32_t end_row = game.map_height - 1;
    uint8_t step_color = 0;
    const int32_t first_row =
        sources.project != nullptr ? 0 : std::max(camera_y, 0) / OA_MAP_CELL_PIXELS;
    for (int32_t row = first_row; row < end_row; ++row) {
        if (first_column >= end_column)
            return;
        bool below_screen = true;
        const int32_t top = (row + 2) * OA_MAP_CELL_PIXELS;
        for (int32_t column = first_column; column < end_column; ++column) {
            const int32_t left = (column + 8) * OA_MAP_CELL_PIXELS;
            const auto at = static_cast<size_t>(game.map_width * row + column);
            const auto under = at + static_cast<size_t>(game.map_width);
            const MapPlot& plot = sources.plots[at];
            const uint8_t heights[debug_grid_corners] = {
                plot.height,
                sources.plots[at + 1].height,
                sources.plots[under + 1].height,
                sources.plots[under].height,
            };
            // Corners clockwise from the top left, x,y pairs.
            int32_t p[debug_grid_corners * 2] = {
                left - camera_x,
                top - (heights[0] >> 1) - camera_y,
                left + OA_MAP_CELL_PIXELS - camera_x,
                top - (heights[1] >> 1) - camera_y,
                left + OA_MAP_CELL_PIXELS - camera_x,
                top + OA_MAP_CELL_PIXELS - (heights[2] >> 1) - camera_y,
                left - camera_x,
                top + OA_MAP_CELL_PIXELS - (heights[3] >> 1) - camera_y,
            };
            if (sources.project != nullptr) {
                for (int corner = 0; corner < debug_grid_corners; ++corner) {
                    const int32_t x = column * OA_MAP_CELL_PIXELS +
                                      ((corner == 1 || corner == 2) ? OA_MAP_CELL_PIXELS : 0);
                    const int32_t z =
                        row * OA_MAP_CELL_PIXELS + (corner >= 2 ? OA_MAP_CELL_PIXELS : 0);
                    sources.project(
                        sources.projection_user,
                        x,
                        heights[corner],
                        z,
                        p[corner * 2],
                        p[corner * 2 + 1]
                    );
                }
                // A rotated row can enter the view again: cull cells, never whole rows.
                const int32_t clip_right = surface != nullptr
                                               ? surface->width + battlefield_origin_x
                                               : static_cast<int32_t>(game.offscreen_width);
                const int32_t clip_bottom = surface != nullptr
                                                ? surface->height + battlefield_origin_y
                                                : static_cast<int32_t>(game.offscreen_height);
                bool left = true, right = true, above = true, below = true;
                for (int corner = 0; corner < debug_grid_corners; ++corner) {
                    left &= p[corner * 2] < battlefield_origin_x;
                    right &= p[corner * 2] >= clip_right;
                    above &= p[corner * 2 + 1] < battlefield_origin_y;
                    below &= p[corner * 2 + 1] >= clip_bottom;
                }
                if (left || right || above || below)
                    continue;
            }
            if (p[1] < static_cast<int32_t>(game.offscreen_height))
                below_screen = false;
            const auto edge_color = game_ui_color(
                game, game.sea_level < plot.height ? ui_color_mark : ui_color_underwater
            );
            if (view == debug_view::path_search) {
                if (classes) {
                    const auto type = sources.movement_class(sources.user, column, row);
                    if (type < movement_class_clear) {
                        const auto color = game_ui_color(game, movement_class_colors[type]);
                        draw_line(raster, surface, p[0], p[1], p[4], p[5], color);
                        draw_line(raster, surface, p[2], p[3], p[6], p[7], color);
                    }
                }
                const DebugSearchCell cell = sources.search_cell != nullptr
                                                 ? sources.search_cell(sources.user, column, row)
                                                 : DebugSearchCell{};
                if ((cell.flags & search_cell_goal) != 0)
                    print_label(
                        raster, surface, sources.small_font, debug_goal_mark, p[0], p[1], [&] {
                            const auto value = sources.random.next != nullptr
                                                   ? sources.random.next(sources.random.user)
                                                   : 0;
                            return value & 0xff;
                        }
                    );
                const auto visit = static_cast<uint8_t>(cell.flags & ~search_cell_goal);
                if (visit != 0 && visit != search_cell_blocked) {
                    const int32_t x = p[0] + cell_centre;
                    const int32_t y = p[1] + cell_centre;
                    if (cell.flags == search_cell_open)
                        step_color = game_ui_color(game, ui_color_mark);
                    else if (cell.flags == search_cell_closed)
                        step_color = game_ui_color(game, ui_color_closed_step);
                    const auto from = cell.direction & 7;
                    const auto left_barb = (cell.direction + 1) & 7;
                    const auto right_barb = (cell.direction - 1) & 7;
                    draw_line(
                        raster,
                        surface,
                        x - step_dx[from] * arrow_shaft,
                        y - step_dy[from] * arrow_shaft,
                        x,
                        y,
                        step_color
                    );
                    draw_line(
                        raster,
                        surface,
                        x - step_dx[left_barb] * arrow_barb,
                        y - step_dy[left_barb] * arrow_barb,
                        x,
                        y,
                        step_color
                    );
                    draw_line(
                        raster,
                        surface,
                        x - step_dx[right_barb] * arrow_barb,
                        y - step_dy[right_barb] * arrow_barb,
                        x,
                        y,
                        step_color
                    );
                }
            } else if (view == debug_view::terrain) {
                draw_line(raster, surface, p[0], p[1], p[2], p[3], edge_color);
                draw_line(raster, surface, p[0], p[1], p[6], p[7], edge_color);
                if (plot.ground_unit != 0)
                    draw_polygon(raster, surface, p, static_cast<uint8_t>(plot.ground_unit));
                else if (plot.feature != plot_no_feature)
                    draw_polygon(
                        raster,
                        surface,
                        p,
                        static_cast<uint8_t>(plot.feature - plot_feature_color_base)
                    );
                if (plot.air_unit != 0) {
                    const auto color = static_cast<uint8_t>(plot.air_unit);
                    draw_line(raster, surface, p[0], p[1], p[4], p[5], color);
                    draw_line(raster, surface, p[2], p[3], p[6], p[7], color);
                }
                if ((plot.flags & plot_flag_marked) != 0) {
                    const auto color = game_ui_color(game, ui_color_mark);
                    const int32_t top_x = midpoint(p[0], p[2]);
                    const int32_t top_y = midpoint(p[1], p[3]);
                    const int32_t right_x = midpoint(p[2], p[4]);
                    const int32_t right_y = midpoint(p[3], p[5]);
                    const int32_t bottom_x = midpoint(p[4], p[6]);
                    const int32_t bottom_y = midpoint(p[5], p[7]);
                    const int32_t left_x = midpoint(p[0], p[6]);
                    const int32_t left_y = midpoint(p[1], p[7]);
                    draw_line(
                        raster,
                        surface,
                        top_x,
                        top_y + diamond_inset,
                        right_x - diamond_inset,
                        right_y,
                        color
                    );
                    draw_line(
                        raster,
                        surface,
                        right_x - diamond_inset,
                        right_y,
                        bottom_x,
                        bottom_y - diamond_inset,
                        color
                    );
                    draw_line(
                        raster,
                        surface,
                        bottom_x,
                        bottom_y - diamond_inset,
                        left_x + diamond_inset,
                        left_y,
                        color
                    );
                    draw_line(
                        raster,
                        surface,
                        left_x + diamond_inset,
                        left_y,
                        top_x,
                        top_y + diamond_inset,
                        color
                    );
                }
            } else if (view == debug_view::metal) {
                draw_line(raster, surface, p[0], p[1], p[2], p[3], edge_color);
                draw_line(raster, surface, p[0], p[1], p[6], p[7], edge_color);
                char number[20];
                std::snprintf(number, sizeof number, "%d", static_cast<int>(plot.metal));
                print_label(
                    raster,
                    surface,
                    sources.small_font,
                    number,
                    p[0] + metal_label_inset,
                    p[1] + metal_label_inset,
                    [&] { return static_cast<int32_t>(game_ui_color(game, ui_color_mark)); }
                );
            } else if (view == debug_view::sight) {
                const auto color = game_ui_color(game, ui_color_grid);
                draw_line(raster, surface, p[0], p[1], p[2], p[3], color);
                draw_line(raster, surface, p[0], p[1], p[6], p[7], color);
                if (sources.coverage != nullptr &&
                    sim::visibility_state::count_in_sight(
                        sources.coverage
                            [static_cast<int32_t>(viewer.sight_width) *
                                 (row / sight_cells_per_plot_cell) +
                             column / sight_cells_per_plot_cell],
                        sim::visibility_state::SightReading::player
                    ))
                    fill_rect(
                        raster,
                        surface,
                        {p[0] - sight_mark_half,
                         p[1] - sight_mark_half,
                         p[0] + sight_mark_half,
                         p[1] + sight_mark_half},
                        game_ui_color(game, ui_color_mark)
                    );
            }
            if (contour.spacing != 0)
                overlay_contour_quad(raster, surface, contour, p, heights);
        }
        if (below_screen && sources.project == nullptr)
            return;
    }
}

void overlay_debug_cursor_cross(
    const Game& game, const OverlayRaster& raster, ::oa::Surface* surface
) {
    if (game.debug_overlay != debug_view::terrain)
        return;
    const FixedVec3 cursor = game.cursor_position;
    const auto x = high_word(cursor.x) - static_cast<int32_t>(game.camera_x);
    const auto y =
        high_word(cursor.z) - (high_word(cursor.y) >> 1) - static_cast<int32_t>(game.camera_y);
    const auto color = game_ui_color(game, ui_color_mark);
    const auto cx = x + battlefield_origin_x;
    const auto cy = y + battlefield_origin_y;
    draw_line(raster, surface, cx - cross_arm, cy, cx + cross_arm, cy, color);
    draw_line(raster, surface, cx, cy - cross_arm, cx, cy + cross_arm, color);
}

void overlay_profile_bar(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    int32_t screen_width,
    int32_t font_height,
    const char* label,
    int32_t category
) {
    rect_outline(
        raster,
        surface,
        {screen_width - profile_frame_left,
         profile_frame_top,
         profile_frame_right,
         font_height * profile_rows + profile_frame_bottom},
        profile_frame_color
    );
    const int32_t y = font_height * category + profile_first_row;
    if (raster.text != nullptr)
        raster.text(raster.user, surface, label, screen_width - profile_label_right, y);
    const auto& times = game.profile_times;
    // The first sample window sets the total before anything draws.
    const int32_t total = times.shown_total != 0 ? times.shown_total : 1;
    const int32_t percent = mul32(times.shown[category], 100) / total;
    fill_rect(
        raster,
        surface,
        {sub32(screen_width - profile_bar_right, mul32(percent, profile_pixels_per_percent)),
         y,
         screen_width - profile_bar_right,
         y + font_height},
        static_cast<uint8_t>(category + 1)
    );
}

} // namespace oa::present::world_renderer
