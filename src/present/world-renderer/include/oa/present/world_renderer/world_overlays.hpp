// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Battlefield debug and status overlays drawn into the 8-bit Surface. Line
// and rectangle rasterization goes through OverlayRaster, which
// present_overlay_raster() binds to the shared presentation raster.

#include "oa/core/game_state.h"
#include "oa/core/map_plot.h"
#include "oa/core/unit.h"
#include "oa/sim/model_runtime/instance.hpp"
#include "oa/present/surface.h"
#include "oa/present/world_renderer/world_camera.hpp"

#include <cstdint>

namespace oa::present::world_renderer {

struct OverlayRaster {
    void* user = nullptr;
    /// Draws a clipped line with inclusive endpoints.
    ///
    /// @param user OverlayRaster::user
    /// @param surface 8-bit surface
    /// @param x0 first endpoint X
    /// @param y0 first endpoint Y
    /// @param x1 second endpoint X
    /// @param y1 second endpoint Y
    /// @param color palette index
    void (*line)(
        void* user,
        ::oa::Surface* surface,
        int32_t x0,
        int32_t y0,
        int32_t x1,
        int32_t y1,
        uint8_t color
    ) = nullptr;
    /// Draws clipped rectangle edges with inclusive corners.
    ///
    /// @param user OverlayRaster::user
    /// @param surface 8-bit surface
    /// @param rect inclusive rectangle
    /// @param color palette index
    void (*rect_outline)(void* user, ::oa::Surface* surface, const Rect32& rect, uint8_t color) =
        nullptr;
    /// Fills a clipped solid rectangle with inclusive corners.
    ///
    /// @param user OverlayRaster::user
    /// @param surface 8-bit surface
    /// @param rect inclusive rectangle
    /// @param color palette index
    void (*fill_rect)(void* user, ::oa::Surface* surface, const Rect32& rect, uint8_t color) =
        nullptr;
    /// Fills a clipped convex polygon.
    ///
    /// @param user OverlayRaster::user
    /// @param surface 8-bit surface
    /// @param points `count` x,y pairs
    /// @param count number of vertices
    /// @param color palette index
    void (*polygon)(
        void* user, ::oa::Surface* surface, const int32_t* points, int32_t count, uint8_t color
    ) = nullptr;
    /// Sets the active font.
    ///
    /// @param user OverlayRaster::user
    /// @param font font to draw text with
    void (*set_font)(void* user, const void* font) = nullptr;
    /// Returns the colour text drawing skips.
    ///
    /// @param user OverlayRaster::user
    /// @return the transparent palette index
    uint32_t (*text_transparent)(void* user) = nullptr;
    /// Sets the text and background colours.
    ///
    /// @param user OverlayRaster::user
    /// @param color text palette index
    /// @param background background palette index
    void (*set_text_colors)(void* user, int32_t color, int32_t background) = nullptr;
    /// Draws text in the active font with no width limit.
    ///
    /// @param user OverlayRaster::user
    /// @param surface 8-bit surface
    /// @param text NUL-terminated text
    /// @param x left edge
    /// @param y top edge
    void (*text)(void* user, ::oa::Surface* surface, const char* text, int32_t x, int32_t y) =
        nullptr;
};

/// Returns an OverlayRaster over the oa::present line, rectangle, polygon and text routines.
///
/// @return the bound raster
[[nodiscard]] OverlayRaster present_overlay_raster() noexcept;

// Game.ui_colors slots used by these overlays.
inline constexpr int32_t ui_color_rotated_box = 10;
inline constexpr int32_t ui_color_traffic_bar = 15;
inline constexpr int32_t cell_outline_inset_slot = 4;
inline constexpr int32_t traffic_bar_full = 100;

/// Rotates four corner offsets about a camera-relative 16.16 origin and draws the closed outline.
///
/// @param game game block giving the UI colours
/// @param raster line drawing
/// @param surface 8-bit battlefield surface
/// @param origin signed 16.16 origin relative to the camera
/// @param corners signed 16.16 corner offsets, drawn in order
/// @param rotation angle words applied to each corner
void overlay_rotated_box(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const FixedVec3& origin,
    const FixedVec3 (&corners)[4],
    sim::model_runtime::RotationWords rotation
);

// Game.console_flags bit ("SelBoxes") under which selected units are
// outlined.
inline constexpr uint16_t console_flag_selection_boxes = OA_CONSOLE_FLAG_SELECTION_BOXES;

/// Outlines one selected unit while SelBoxes is on.
///
/// Draws the root object's box of its model at the box's lowest height, turned
/// by the unit's angles about its position less the camera.
///
/// @param game game block giving the console options, camera and UI colours
/// @param raster line drawing
/// @param surface 8-bit battlefield surface
/// @param unit the selected unit
/// @param root the root object of the unit's model
void overlay_selection_box(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const Unit& unit,
    const formats::objects3d::Object& root
);

/// Draws an outlined bar filled to a 0..100 percentage.
///
/// @param game game block giving the UI colours
/// @param raster rectangle drawing
/// @param surface 8-bit surface
/// @param rect inclusive bar rectangle
/// @param percent fill; values of 100 and more fill the bar, non-positive values leave it empty
void overlay_traffic_bar(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    Rect32 rect,
    int32_t percent
);

/// Draws a bar split at value/maximum into a filled and a remaining part.
///
/// @param raster rectangle drawing
/// @param surface 8-bit surface
/// @param value current value, clamped to 0..maximum
/// @param maximum full value; 0 draws nothing
/// @param rect inclusive bar rectangle
/// @param colors UI colour table (Game.ui_colors): entry 10 fills the filled
///     part, entry 4 the rest; null draws nothing
/// @param y_offset pixels the bar is shifted down
void overlay_meter_bar(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    int32_t value,
    int32_t maximum,
    const Rect32& rect,
    const uint8_t* colors,
    int32_t y_offset
);

/// Outlines a footprint of map cells at its terrain height.
///
/// @param game game block giving the camera and UI colours
/// @param raster rectangle drawing
/// @param surface 8-bit battlefield surface
/// @param cell_x first cell column
/// @param cell_y first cell row
/// @param cells_wide footprint width in cells
/// @param cells_high footprint depth in cells
/// @param cell_height terrain height; the outline is raised by half of it
/// @param color_slot UI colour slot; slot 4 draws the outline one pixel inside
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
);

// Contour spacing and phase (debug variables) plus the sea level they are
// coloured against. Heights are terrain heights shifted left by 8.
struct ContourStyle {
    int32_t spacing = 0;
    int32_t phase = 0;
    uint8_t sea_level = 0;
};

inline constexpr int32_t contour_ramp_size = 32;
extern const uint8_t contour_ramp[contour_ramp_size];

struct ContourVertex {
    int32_t x = 0;
    int32_t y = 0;
    int32_t height = 0;
};

/// Draws the height contour lines crossing one screen triangle.
///
/// A non-positive spacing draws nothing.
///
/// @param raster line drawing
/// @param surface 8-bit surface
/// @param style contour spacing, phase and sea level
/// @param a first vertex, height shifted left by 8
/// @param b second vertex
/// @param c third vertex
void overlay_contour_triangle(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const ContourStyle& style,
    ContourVertex a,
    ContourVertex b,
    ContourVertex c
);

/// Draws the contours of a quad split into four triangles around its centre.
///
/// @param raster line drawing
/// @param surface 8-bit surface
/// @param style contour spacing, phase and sea level
/// @param points four screen corners as x,y pairs
/// @param heights the four corner terrain bytes
void overlay_contour_quad(
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const ContourStyle& style,
    const int32_t (&points)[8],
    const uint8_t (&heights)[4]
);

// Views of the debug grid, cycled by debug key 'm' (Game.debug_overlay).
namespace debug_view {
inline constexpr uint8_t off = 0;
inline constexpr uint8_t path_search = 1; // movement classes and the path search map
inline constexpr uint8_t terrain = 2;     // plot edges against sea level, plot marks
inline constexpr uint8_t metal = 3;       // plot edges and each plot's metal
inline constexpr uint8_t sight = 4;       // cell grid and the viewer's sight coverage
} // namespace debug_view

// Bits of a path search map cell's flags as the search view reads them.
inline constexpr uint8_t search_cell_goal = 0x04;
inline constexpr uint8_t search_cell_open = 1;
inline constexpr uint8_t search_cell_closed = 2;
inline constexpr uint8_t search_cell_blocked = 3;

// Movement class of a cell whose unit footprint and its rim are clear at the
// preferred slopes; the search view marks the other classes.
inline constexpr uint8_t movement_class_clear = 3;

// The goal mark the search view prints.
inline constexpr char debug_goal_mark[] = "G";

struct DebugSearchCell {
    uint8_t flags = 0;
    uint8_t direction = 0; // 0..7, the step the search came from
};

// What the debug grid reads outside the Game block, resolved by the caller.
struct DebugGridSources {
    const MapPlot* plots = nullptr;     // Game.map_cells
    const uint16_t* coverage = nullptr; // viewpoint Player.coverage_grid (sight view)
    const void* small_font = nullptr;   // smlfont, loaded at start-up
    int32_t contour_spacing = 0;        // "Contour" debug variable; 0 draws no contours
    int32_t contour_phase = 0;          // contour phase debug variable
    void* user = nullptr;
    /// Opens the movement class grid of the local player's next selected unit.
    ///
    /// Finds the next selected unit from none and opens the grid of its type
    /// (UnitDef.move_class).
    ///
    /// @param user DebugGridSources::user
    /// @return false when no unit is selected or the type has no class
    bool (*open_movement_class)(void* user) = nullptr;
    /// Returns the two-bit class of a cell of the opened grid.
    ///
    /// @param user DebugGridSources::user
    /// @param x cell column
    /// @param z cell row
    /// @return the movement class, 0..3
    uint8_t (*movement_class)(void* user, int32_t x, int32_t z) = nullptr;
    /// Returns a cell of the path search map.
    ///
    /// @param user DebugGridSources::user
    /// @param x cell column
    /// @param z cell row
    /// @return the cell's flags and direction
    DebugSearchCell (*search_cell)(void* user, int32_t x, int32_t z) = nullptr;
    /// Optional projection of terrain corners in map pixels to overlay pixels.
    /// @param user projection_user
    /// @param x map horizontal coordinate
    /// @param height terrain altitude
    /// @param z map vertical coordinate
    /// @param[out] screen_x horizontal overlay coordinate
    /// @param[out] screen_y vertical overlay coordinate
    void (*project)(
        void* user, int32_t x, int32_t height, int32_t z, int32_t& screen_x, int32_t& screen_y
    ) = nullptr;
    void* projection_user = nullptr;
    RandomSource random{}; // goal mark colours
};

/// Draws the debug view over every visible map cell.
///
/// Walks row by row until a row lies wholly below the screen: the 'm' key's view
/// of the cell, then the height contours when "Contour" has set a spacing. Cell
/// corners sit at their terrain height, as the terrain does. A goal cell's
/// arrow drawn before any other arrow uses colour 0.
///
/// @param game game block giving the debug view, camera, map and UI colours
/// @param raster line, polygon and text drawing
/// @param surface 8-bit battlefield surface
/// @param sources plots, coverage, font, contour settings and search callbacks
/// @quirk A search cell drawn with the goal bit set keeps the previous arrow's
///        colour.
void overlay_debug_grid(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    const DebugGridSources& sources
);

/// Draws the terrain view's cross at the pointer's map position, after the grid.
///
/// Drawn only while the debug view is debug_view::terrain.
///
/// @param game game block giving the pointer position, camera and UI colours
/// @param raster line drawing
/// @param surface 8-bit battlefield surface
void overlay_debug_cursor_cross(
    const Game& game, const OverlayRaster& raster, ::oa::Surface* surface
);

/// Draws one "Profile" category.
///
/// The frame around all nine bars is drawn in colour 0xFF, then the label, then
/// a bar in colour category + 1, two pixels long per percent of the shown total,
/// growing left from screen_width - 0x5A.
///
/// @param game game block holding the profile timings
/// @param raster rectangle and text drawing
/// @param surface 8-bit surface
/// @param screen_width screen width in pixels
/// @param font_height row height in pixels
/// @param label category name
/// @param category category index, 0..8
/// @quirk The frame's right edge is x 0x27F whatever the screen width.
void overlay_profile_bar(
    const Game& game,
    const OverlayRaster& raster,
    ::oa::Surface* surface,
    int32_t screen_width,
    int32_t font_height,
    const char* label,
    int32_t category
);

} // namespace oa::present::world_renderer
