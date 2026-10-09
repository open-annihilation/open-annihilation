// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battlefield's thin lines drawn band by band (world_draws.hpp): lines
// of the frame (lasers, lightning) and selection lines of the model bridge,
// drawn 2 pixels thick, as the accelerated tier draws them in a scene the
// area pass reduces, cover each of the line's pixels and the pixels right
// of, below and below and right of it, across the edges of the bands and
// of the bridge's tiles, byte for byte the same on 1 to 8 bands, each band
// on a drawing thread of its own. Drawn 1 pixel thick they cover the line's
// own pixels, as the game always draws them. A feature's shadow frame,
// drawn or blended, mixes from the ground toward the game's shadow by the
// frame's shadow level and is not drawn at level 0; a frame's shadows are
// set from its zoom (set_frame_shadows). A sprite stays over the ground it
// stands on through every frame of a zoom's ease from 0.25 to 3 and while a
// camera follows it a map pixel a frame zoomed far out: drawn at the zoom,
// it is the ground with it drawn on at zoom 1, resampled as the terrain is.
// An explosion's flash lights the palette entry under each of its pixels
// through the light table's row the pixel names, a second flash lighting
// what the first lit, at half the row when reduced, the same on 1 to 8
// bands. A projectile's lens moves exactly
// the 24 pixels its rule names, as the 8-bit lens sprite does, whole blocks
// at scales 2 and 3, within the clip, a second lens reading the first's
// pixels; its battlefield test takes the rectangle's edges and the height's
// lift. A file drawn a frame at a time is
// one over the decoded threshold; its frames render from the file's bytes as
// decoded, through a cache that keeps them within its budget, the frame
// drawn longest ago going first, draws a frame larger than its budget
// without keeping it, and does not read a frame that failed again.
#include "world_draws.hpp"

#include "oa/platform/job_pool.hpp"
#include "oa/present/blit.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/rle.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/world_renderer/scene_filter.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {

namespace model_render = oa::present::model;
namespace job_pool = oa::platform::job_pool;
using oa::app::WorldDrawKind;

/// The frame: eight rows of the bridge's tiles down, so that it splits into
/// as many as eight bands.
constexpr int32_t frame_width = 96;
constexpr int32_t frame_height = 8 * model_render::bridge_tile_side;
/// The most bands the frame is drawn in.
constexpr int32_t most_bands = 8;

/// The palette entry the frame is filled with, and the one selection lines
/// are drawn in.
constexpr uint8_t ground_index = 0;
constexpr uint8_t selection_index = 200;
/// The colour of the frame's lines.
constexpr std::array<uint8_t, 3> line_colour{0xF0, 0x20, 0x60};

/// A line from its first pixel to its last.
struct Line {
    int32_t x0{};
    int32_t y0{};
    int32_t x1{};
    int32_t y1{};
};

/// The lines: one down the whole frame, across every band's edge; one along
/// the last row of each tile row but the last, on every edge a band can
/// have; and two on the diagonal, across edges.
std::vector<Line> test_lines() {
    std::vector<Line> lines{{10, 0, 10, frame_height - 1}, {40, 0, 90, 50}, {30, 100, 90, 160}};
    for (int32_t row = model_render::bridge_tile_side - 1; row < frame_height - 1;
         row += model_render::bridge_tile_side)
        lines.push_back({20, row, 70, row});
    return lines;
}

/// Returns a line's pixels: a line down, along a row or on the diagonal
/// steps one pixel at a time from its first pixel to its last.
///
/// @param line the line
/// @return its pixels, as column and row
std::vector<std::array<int32_t, 2>> line_pixels(const Line& line) {
    const int32_t step_x = line.x1 > line.x0 ? 1 : (line.x1 < line.x0 ? -1 : 0);
    const int32_t step_y = line.y1 > line.y0 ? 1 : (line.y1 < line.y0 ? -1 : 0);
    std::vector<std::array<int32_t, 2>> pixels;
    for (int32_t x = line.x0, y = line.y0;; x += step_x, y += step_y) {
        pixels.push_back({x, y});
        if (x == line.x1 && y == line.y1)
            break;
    }
    return pixels;
}

/// A palette whose every entry differs in all three bytes.
oa::PaletteBytes test_palette() {
    oa::PaletteBytes palette{};
    for (std::size_t index = 0; index < oa::palette_color_count; ++index) {
        palette[index * oa::palette_entry_bytes] = static_cast<uint8_t>(index);
        palette[index * oa::palette_entry_bytes + 1] = static_cast<uint8_t>(index * 7U + 3U);
        palette[index * oa::palette_entry_bytes + 2] = static_cast<uint8_t>(255U - index);
    }
    return palette;
}

/// Returns a palette entry's colour.
///
/// @param palette the palette
/// @param index the entry
/// @return its red, green and blue
std::array<uint8_t, 3> entry_colour(const oa::PaletteBytes& palette, uint8_t index) {
    const std::size_t at = std::size_t{index} * oa::palette_entry_bytes;
    return {palette[at], palette[at + 1], palette[at + 2]};
}

/// Returns a frame filled with the ground's colour.
///
/// @param palette the palette
/// @return the frame's RGB pixels
std::vector<uint8_t> ground_frame(const oa::PaletteBytes& palette) {
    const auto ground = entry_colour(palette, ground_index);
    std::vector<uint8_t> rgb(std::size_t{frame_width} * frame_height * 3U);
    for (std::size_t pixel = 0; pixel < rgb.size() / 3U; ++pixel)
        for (std::size_t channel = 0; channel < 3; ++channel)
            rgb[pixel * 3U + channel] = ground[channel];
    return rgb;
}

/// Returns the frame the lines must give: the ground, and the colour on
/// each line's pixels and, drawn thick, on those pixels moved right and
/// down by up to one less than the thickness.
///
/// @param palette the palette
/// @param colour the lines' colour
/// @param thickness pixels across and down each is drawn
/// @return the frame's RGB pixels
std::vector<uint8_t> expected_frame(
    const oa::PaletteBytes& palette, const std::array<uint8_t, 3>& colour, int32_t thickness
) {
    auto rgb = ground_frame(palette);
    for (const Line& line : test_lines())
        for (const auto& [x, y] : line_pixels(line))
            for (int32_t down = 0; down < thickness; ++down)
                for (int32_t across = 0; across < thickness; ++across) {
                    const int32_t column = x + across;
                    const int32_t row = y + down;
                    if (column < 0 || row < 0 || column >= frame_width || row >= frame_height)
                        continue;
                    const std::size_t at = (static_cast<std::size_t>(row) * frame_width +
                                            static_cast<std::size_t>(column)) *
                                           3U;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        rgb[at + channel] = colour[channel];
                }
    return rgb;
}

/// Draws the lines into a frame of the ground in bands, each band on a
/// drawing thread of its own, through draw_world_band.
///
/// @param palette the palette
/// @param kind WorldDrawKind::line or WorldDrawKind::selection_line
/// @param thickness the frame's line_thickness and bridge_line_thickness
/// @param bands the bands wanted
/// @return the frame's RGB pixels
std::vector<uint8_t>
draw_lines(const oa::PaletteBytes& palette, WorldDrawKind kind, int32_t thickness, int32_t bands) {
    auto rgb = ground_frame(palette);
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    model_render::RgbBridge bridge;
    const model_render::RgbFrame frame{rgb.data(), frame_width, frame_height, frame_width * 3};
    model_render::bridge_begin(
        bridge, frame, {0, 0, frame_width - 1, frame_height - 1}, 1.0F, display.palette
    );
    oa::app::WorldDrawList list;
    for (const Line& line : test_lines()) {
        list.lines.push_back({line.x0, line.y0, line.x1, line.y1, line_colour, selection_index});
        oa::app::add_world_draw(list, kind, list.lines.size() - 1);
    }
    oa::app::add_world_draw(list, WorldDrawKind::commit_always, 0);
    oa::app::WorldFrameDraw draw;
    draw.target = {
        rgb.data(), frame_width, frame_height, 0, 0, frame_width, frame_height, 0, frame_height
    };
    draw.palette = &palette;
    draw.bridge = &bridge;
    draw.display = &display;
    draw.line_thickness = thickness;
    draw.bridge_line_thickness = thickness;
    std::vector<model_render::BridgeBand> split;
    const int32_t made = model_render::bridge_split(bridge, bands, split);
    OA_CHECK(made == bands);
    for (int32_t band = 1; band < made; ++band)
        model_render::bridge_band_colours(bridge, split[static_cast<std::size_t>(band)]);
    std::vector<model_render::ModelRenderer> renderers(static_cast<std::size_t>(made));
    std::vector<model_render::SupersampleScratch> supersample(static_cast<std::size_t>(made));
    std::vector<std::vector<oa::formats::objects3d::FixedVector3>> points(
        static_cast<std::size_t>(made)
    );
    std::array<bool, most_bands> failed{};
    const auto pool = std::make_unique<job_pool::Pool>(static_cast<uint32_t>(made));
    job_pool::run_bands(
        made > 1 ? pool.get() : nullptr, static_cast<uint32_t>(made), [&](uint32_t band) noexcept {
            try {
                oa::app::draw_world_band(
                    list, draw, split[band], renderers[band], supersample[band], points[band]
                );
            } catch (...) {
                failed[band] = true;
            }
        }
    );
    for (auto& band : split)
        model_render::bridge_join_band(bridge, band);
    for (const bool band_failed : failed)
        OA_CHECK(!band_failed);
    return rgb;
}

/// Checks the lines of each kind at each thickness on 1 to most_bands bands.
void test_thin_lines_in_bands() {
    const auto palette = test_palette();
    for (const auto kind : {WorldDrawKind::line, WorldDrawKind::selection_line}) {
        const auto colour =
            kind == WorldDrawKind::line ? line_colour : entry_colour(palette, selection_index);
        for (const int32_t thickness : {1, 2}) {
            const auto expected = expected_frame(palette, colour, thickness);
            for (int32_t bands = 1; bands <= most_bands; ++bands) {
                const bool same = draw_lines(palette, kind, thickness, bands) == expected;
                if (!same)
                    std::fprintf(
                        stderr,
                        "%s lines %d thick on %d bands differ from their pixels\n",
                        kind == WorldDrawKind::line ? "frame" : "selection",
                        static_cast<int>(thickness),
                        static_cast<int>(bands)
                    );
                OA_CHECK(same);
            }
        }
    }
}

/// Returns a frame's pixel.
///
/// @param rgb the frame's RGB pixels
/// @param x column
/// @param y row
/// @return its red, green and blue
std::array<uint8_t, 3> pixel_at(const std::vector<uint8_t>& rgb, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * frame_width + static_cast<std::size_t>(x)) * 3U;
    return {rgb[at], rgb[at + 1], rgb[at + 2]};
}

/// Draws a 4x4 GAF frame of one colour at (20, 20) into a frame of the
/// ground, as a sprite or blended, a feature's shadow or not, from a list
/// at a shadow level, on one band.
///
/// @param palette the palette
/// @param kind WorldDrawKind::sprite or WorldDrawKind::blended_sprite
/// @param shadow the frame is a feature's shadow
/// @param level the list's shadow level
/// @return the frame's RGB pixels
std::vector<uint8_t>
draw_square(const oa::PaletteBytes& palette, WorldDrawKind kind, bool shadow, uint32_t level) {
    auto rgb = ground_frame(palette);
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    model_render::RgbBridge bridge;
    const model_render::RgbFrame frame{rgb.data(), frame_width, frame_height, frame_width * 3};
    model_render::bridge_begin(
        bridge, frame, {0, 0, frame_width - 1, frame_height - 1}, 1.0F, display.palette
    );
    oa::formats::gaf::RenderedFrame square;
    square.width = 4;
    square.height = 4;
    square.pixels.assign(16, selection_index);
    square.coverage.assign(16, 1);
    oa::app::WorldDrawList list;
    list.shadow_level = level;
    list.sprites.push_back({&square, {20, 20}, shadow});
    oa::app::add_world_draw(list, kind, 0);
    oa::app::WorldFrameDraw draw;
    draw.target = {
        rgb.data(), frame_width, frame_height, 0, 0, frame_width, frame_height, 0, frame_height
    };
    draw.palette = &palette;
    draw.bridge = &bridge;
    draw.display = &display;
    std::vector<model_render::BridgeBand> split;
    model_render::bridge_split(bridge, 1, split);
    model_render::ModelRenderer renderer;
    model_render::SupersampleScratch supersample;
    std::vector<oa::formats::objects3d::FixedVector3> points;
    oa::app::draw_world_band(list, draw, split.front(), renderer, supersample, points);
    model_render::bridge_join_band(bridge, split.front());
    return rgb;
}

/// A feature's shadow frame, drawn as a sprite or blended through the
/// alpha table: at the full level as the game draws it; lighter, each
/// covered pixel mixed from the ground toward that by the level; at 0 not
/// at all. A frame that is not a shadow draws the same at any level.
void test_feature_shadows() {
    const auto palette = test_palette();
    const auto ground = entry_colour(palette, ground_index);
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    const auto blend_index = display.alpha[std::size_t{selection_index} * 256U + ground_index];
    for (const auto kind : {WorldDrawKind::sprite, WorldDrawKind::blended_sprite}) {
        const auto shadowed = kind == WorldDrawKind::sprite ? entry_colour(palette, selection_index)
                                                            : entry_colour(palette, blend_index);
        const auto full = draw_square(palette, kind, true, model_render::shadow_full_level);
        const auto plain = draw_square(palette, kind, false, model_render::shadow_full_level);
        OA_CHECK(full == plain);
        OA_CHECK(pixel_at(full, 21, 21) == shadowed);
        OA_CHECK(pixel_at(full, 19, 21) == ground);
        for (const uint32_t level : {1U, 16U, 32U, 63U}) {
            const auto faded = draw_square(palette, kind, true, level);
            std::array<uint8_t, 3> expected{};
            for (std::size_t channel = 0; channel < 3; ++channel)
                expected[channel] =
                    model_render::fade_shadow_channel(ground[channel], shadowed[channel], level);
            OA_CHECK(pixel_at(faded, 20, 20) == expected);
            OA_CHECK(pixel_at(faded, 23, 23) == expected);
            OA_CHECK(pixel_at(faded, 24, 23) == ground);
            OA_CHECK(draw_square(palette, kind, false, level) == plain);
        }
        OA_CHECK(draw_square(palette, kind, true, 0) == ground_frame(palette));
        OA_CHECK(draw_square(palette, kind, false, 0) == plain);
    }
}

/// The map a sprite stands on in test_sprites_stay_on_their_ground, in map
/// pixels, and the place of the sprite's origin on it.
constexpr int32_t ground_map_width = 1200;
constexpr int32_t ground_map_height = 400;
constexpr oa::present::world_renderer::ScreenPoint ground_sprite_place{243, 93};
/// The scene the sprite is drawn on, in scene pixels.
constexpr uint32_t ground_scene_width = 120;
constexpr uint32_t ground_scene_height = 40;
/// The zoom the wheel's ease starts at, the share it grows by each frame,
/// and the zoom it stops past.
constexpr double ease_first_zoom = 0.25;
constexpr double ease_growth = 1.06;
constexpr double ease_last_zoom = 3.0;
/// The camera's corner while the zoom eases, in map pixels.
constexpr int32_t ease_camera_x = 200;
constexpr int32_t ease_camera_y = 70;
/// The far zooms a camera follows the sprite at, a map pixel a frame, and
/// the frames it follows it for.
constexpr std::array<float, 2> follow_zooms{0.2F, 0.37F};
constexpr int32_t follow_frames = 40;
/// Seed of the map's and the sprite's pixels.
constexpr uint32_t ground_seed = 0x6A09E667U;

/// Returns the next number of a 32-bit xorshift sequence.
///
/// @param[in,out] state the sequence
/// @return 32 bits
uint32_t next_random(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/// The ground and the sprite the drawings in test_sprites_stay_on_their_ground share.
struct GroundScene {
    std::vector<uint8_t> map;                 ///< the ground, RGB, a map pixel a pixel
    std::vector<uint8_t> composite;           ///< the ground with the sprite drawn on it at zoom 1
    oa::formats::gaf::RenderedFrame sprite{}; ///< the sprite, some of its pixels transparent
    oa::PaletteBytes palette{};
};

/// Returns a whole-frame target over RGB pixels.
///
/// @param rgb the pixels
/// @param width their columns
/// @param height their rows
/// @return the target
oa::app::WorldTarget whole_target(std::vector<uint8_t>& rgb, int32_t width, int32_t height) {
    return {rgb.data(), width, height, 0, 0, width, height, 0, height};
}

/// Builds the seeded ground, the sprite and the ground with the sprite drawn
/// on it at zoom 1.
///
/// @return the scene
GroundScene ground_scene() {
    GroundScene scene;
    scene.palette = test_palette();
    uint32_t state = ground_seed;
    scene.map.resize(std::size_t{ground_map_width} * ground_map_height * 3U);
    for (auto& byte : scene.map)
        byte = static_cast<uint8_t>(next_random(state) >> 24);
    auto& sprite = scene.sprite;
    sprite.width = 23;
    sprite.height = 17;
    sprite.origin_x = 11;
    sprite.origin_y = 14;
    for (int32_t pixel = 0; pixel < sprite.width * sprite.height; ++pixel) {
        const uint32_t random = next_random(state);
        sprite.pixels.push_back(static_cast<uint8_t>(random >> 24));
        sprite.coverage.push_back((random & 3U) != 0 ? 1 : 0);
    }
    scene.composite = scene.map;
    oa::app::blit_world_sprite(
        whole_target(scene.composite, ground_map_width, ground_map_height),
        sprite,
        ground_sprite_place,
        scene.palette,
        1.0F
    );
    return scene;
}

/// Draws the ground's scene from a camera at a zoom: the ground resampled
/// on the scene grid, with the sprite drawn over it as the battlefield
/// draws its sprites, or the ground with the sprite already on it resampled
/// alike.
///
/// @param scene the ground and the sprite
/// @param camera_x the map column at the scene's first pixel
/// @param camera_y the map row at the scene's first pixel
/// @param zoom scene pixels per map pixel
/// @param sprite_drawn whether the sprite is drawn on the scene, or was drawn on the ground
/// @return the scene's RGB pixels
std::vector<uint8_t> ground_picture(
    const GroundScene& scene, int32_t camera_x, int32_t camera_y, float zoom, bool sprite_drawn
) {
    namespace wr = oa::present::world_renderer;
    const auto& ground = sprite_drawn ? scene.map : scene.composite;
    const wr::RgbSource source{
        ground.data() + (static_cast<std::size_t>(camera_y) * ground_map_width +
                         static_cast<std::size_t>(camera_x)) *
                            3U,
        static_cast<uint32_t>(ground_map_width - camera_x),
        static_cast<uint32_t>(ground_map_height - camera_y),
        static_cast<uint32_t>(ground_map_width)
    };
    std::vector<uint8_t> picture(std::size_t{ground_scene_width} * ground_scene_height * 3U, 0);
    OA_CHECK(
        wr::resample_nearest_rgb24(
            source,
            zoom,
            {picture.data(), ground_scene_width, ground_scene_height, ground_scene_width}
        ) == wr::AreaError::none
    );
    if (sprite_drawn)
        oa::app::blit_world_sprite(
            whole_target(
                picture,
                static_cast<int32_t>(ground_scene_width),
                static_cast<int32_t>(ground_scene_height)
            ),
            scene.sprite,
            {ground_sprite_place.x - camera_x, ground_sprite_place.y - camera_y},
            scene.palette,
            zoom
        );
    return picture;
}

/// A sprite stays over the ground it stands on: through every frame of the
/// wheel's ease from zoom 0.25 to 3, and as a camera follows it a map pixel
/// a frame at zooms 0.2 and 0.37, the scene with the sprite drawn on it at
/// the zoom is the scene of the ground with the sprite drawn on it at zoom
/// 1, byte for byte.
void test_sprites_stay_on_their_ground() {
    const GroundScene scene = ground_scene();
    const auto off_ground = [&](int32_t camera_x, int32_t camera_y, float zoom) {
        return ground_picture(scene, camera_x, camera_y, zoom, true) !=
               ground_picture(scene, camera_x, camera_y, zoom, false);
    };
    int frames = 0;
    int moved = 0;
    for (double zoom = ease_first_zoom; zoom <= ease_last_zoom; zoom *= ease_growth) {
        ++frames;
        moved += off_ground(ease_camera_x, ease_camera_y, static_cast<float>(zoom)) ? 1 : 0;
    }
    OA_CHECK(frames > 40);
    OA_CHECK(moved == 0);
    for (const float zoom : follow_zooms)
        for (int32_t frame = 0; frame < follow_frames; ++frame) {
            const bool off =
                off_ground(ground_sprite_place.x - follow_frames - 30 + frame, ease_camera_y, zoom);
            OA_CHECK(!off);
            moved += off ? 1 : 0;
        }
    if (moved != 0)
        std::fprintf(stderr, "a sprite left its ground on %d frames\n", moved);
}

/// A light table whose every row maps each palette entry to another, a
/// different one for each row.
///
/// @return the table, oa::present::light_table_size bytes
std::vector<uint8_t> test_light_table() {
    std::vector<uint8_t> table(static_cast<std::size_t>(oa::present::light_table_size));
    for (std::size_t row = 0; row < static_cast<std::size_t>(oa::present::ramp_table_rows); ++row)
        for (std::size_t index = 0; index < 256U; ++index)
            table[row * 256U + index] = static_cast<uint8_t>(index * 5U + row * 3U + 1U);
    return table;
}

/// The light table's row a flash pixel names.
///
/// @param row the row
/// @return the pixel's value
constexpr uint8_t flash_value(uint32_t row) noexcept {
    return static_cast<uint8_t>(oa::present::shade_ramp_base + static_cast<int32_t>(row));
}

/// A flash frame of flash_side pixels a side, its origin at its centre:
/// each pixel names row (column + row * flash_side) % 32 of the light
/// table, but for its first row, which holds the transparent index, a value
/// below the light ramp and one above it.
constexpr int32_t flash_side = 24;

/// Returns the test's flash frame.
///
/// @return the frame, each pixel covered
oa::formats::gaf::RenderedFrame flash_frame() {
    oa::formats::gaf::RenderedFrame frame;
    frame.width = flash_side;
    frame.height = flash_side;
    frame.origin_x = flash_side / 2;
    frame.origin_y = flash_side / 2;
    frame.transparency_index = 0xff;
    frame.pixels.resize(static_cast<std::size_t>(flash_side) * flash_side);
    frame.coverage.assign(frame.pixels.size(), 1);
    for (int32_t y = 0; y < flash_side; ++y)
        for (int32_t x = 0; x < flash_side; ++x)
            frame.pixels[static_cast<std::size_t>(y * flash_side + x)] =
                flash_value(static_cast<uint32_t>(x + y * flash_side) % 32U);
    for (int32_t x = 0; x < flash_side; ++x)
        frame.pixels[static_cast<std::size_t>(x)] =
            x % 3 == 0 ? frame.transparency_index
                       : (x % 3 == 1 ? static_cast<uint8_t>(oa::present::shade_ramp_base - 1)
                                     : flash_value(32));
    return frame;
}

/// Draws explosion flashes, each the test's flash frame with its origin at
/// a point, into a frame of the ground in bands, each band on a drawing
/// thread of its own.
///
/// @param palette the palette
/// @param light the light table; null for none
/// @param at the flashes' points, drawn in order
/// @param strength the list's flash strength
/// @param bands the bands wanted
/// @return the frame's RGB pixels
std::vector<uint8_t> draw_flashes(
    const oa::PaletteBytes& palette,
    const uint8_t* light,
    const std::vector<oa::present::world_renderer::ScreenPoint>& at,
    oa::app::FlashStrength strength,
    int32_t bands
) {
    auto rgb = ground_frame(palette);
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    model_render::RgbBridge bridge;
    const model_render::RgbFrame frame{rgb.data(), frame_width, frame_height, frame_width * 3};
    model_render::bridge_begin(
        bridge, frame, {0, 0, frame_width - 1, frame_height - 1}, 1.0F, display.palette
    );
    const auto flash = flash_frame();
    oa::app::WorldDrawList list;
    list.flash_strength = strength;
    for (const auto& point : at) {
        list.sprites.push_back({&flash, point});
        oa::app::add_world_draw(list, WorldDrawKind::lit_sprite, list.sprites.size() - 1);
    }
    oa::app::WorldFrameDraw draw;
    draw.target = {
        rgb.data(), frame_width, frame_height, 0, 0, frame_width, frame_height, 0, frame_height
    };
    draw.palette = &palette;
    draw.bridge = &bridge;
    draw.display = &display;
    draw.light_table = light;
    std::vector<model_render::BridgeBand> split;
    const int32_t made = model_render::bridge_split(bridge, bands, split);
    OA_CHECK(made == bands);
    for (int32_t band = 1; band < made; ++band)
        model_render::bridge_band_colours(bridge, split[static_cast<std::size_t>(band)]);
    std::vector<model_render::ModelRenderer> renderers(static_cast<std::size_t>(made));
    std::vector<model_render::SupersampleScratch> supersample(static_cast<std::size_t>(made));
    std::vector<std::vector<oa::formats::objects3d::FixedVector3>> points(
        static_cast<std::size_t>(made)
    );
    std::array<bool, most_bands> failed{};
    const auto pool = std::make_unique<job_pool::Pool>(static_cast<uint32_t>(made));
    job_pool::run_bands(
        made > 1 ? pool.get() : nullptr, static_cast<uint32_t>(made), [&](uint32_t band) noexcept {
            try {
                oa::app::draw_world_band(
                    list, draw, split[band], renderers[band], supersample[band], points[band]
                );
            } catch (...) {
                failed[band] = true;
            }
        }
    );
    for (auto& band : split)
        model_render::bridge_join_band(bridge, band);
    for (const bool band_failed : failed)
        OA_CHECK(!band_failed);
    return rgb;
}

/// Explosion flashes light what is under them as 3.1c draws them: each
/// covered pixel of the light ramp makes the palette entry under it the
/// light table's entry in the row the pixel names; the transparent index,
/// values outside the table's rows and every pixel outside the frame leave
/// the ground; a second flash over the first lights what the first lit; at
/// the reduced strength each row is halved, rounded down; without a light
/// table nothing is drawn; and the frame is the same drawn on 1 to 8
/// bands, a flash across the bands' edges among them.
void test_explosion_flashes() {
    const auto palette = test_palette();
    const auto light = test_light_table();
    const auto ground = entry_colour(palette, ground_index);
    const oa::present::world_renderer::ScreenPoint centre{40, 30};
    const auto flash = flash_frame();
    const int32_t left = centre.x - flash.origin_x;
    const int32_t top = centre.y - flash.origin_y;
    // The entry a pixel of the flash leaves under it, from the entry under it.
    const auto lit = [&](int32_t x, int32_t y, uint8_t under, uint32_t shift) -> int32_t {
        const uint8_t value =
            flash.pixels[static_cast<std::size_t>((y - top) * flash_side + (x - left))];
        if (value == flash.transparency_index || value < oa::present::shade_ramp_base)
            return -1;
        const auto row = static_cast<uint32_t>(value - oa::present::shade_ramp_base);
        if (row >= static_cast<uint32_t>(oa::present::ramp_table_rows))
            return -1;
        return light[(row >> shift) * 256U + under];
    };
    for (const auto strength : {oa::app::FlashStrength::full, oa::app::FlashStrength::reduced}) {
        const uint32_t shift = strength == oa::app::FlashStrength::reduced ? 1U : 0U;
        const auto once = draw_flashes(palette, light.data(), {centre}, strength, 1);
        const auto twice = draw_flashes(palette, light.data(), {centre, centre}, strength, 1);
        bool exact = true;
        for (int32_t y = 0; y < frame_height; ++y)
            for (int32_t x = 0; x < frame_width; ++x) {
                const bool inside =
                    x >= left && x < left + flash_side && y >= top && y < top + flash_side;
                const int32_t first = inside ? lit(x, y, ground_index, shift) : -1;
                const auto expected_once =
                    first < 0 ? ground : entry_colour(palette, static_cast<uint8_t>(first));
                const int32_t second =
                    first < 0 ? -1 : lit(x, y, static_cast<uint8_t>(first), shift);
                const auto expected_twice =
                    second < 0 ? expected_once
                               : entry_colour(palette, static_cast<uint8_t>(second));
                exact = exact && pixel_at(once, x, y) == expected_once &&
                        pixel_at(twice, x, y) == expected_twice;
            }
        OA_CHECK(exact);
        // Light row 31 at the full strength, 15 at the reduced.
        const int32_t brightest_x = left + 31 % flash_side;
        const int32_t brightest_y = top + 31 / flash_side;
        OA_CHECK(
            pixel_at(once, brightest_x, brightest_y) ==
            entry_colour(palette, light[(31U >> shift) * 256U + ground_index])
        );
        // A flash across the bands' edges, drawn on every count of bands.
        const std::vector<oa::present::world_renderer::ScreenPoint> across{
            centre,
            {70, model_render::bridge_tile_side * 2},
            {12, model_render::bridge_tile_side * 5}
        };
        const auto whole = draw_flashes(palette, light.data(), across, strength, 1);
        for (int32_t bands = 2; bands <= most_bands; ++bands)
            OA_CHECK(draw_flashes(palette, light.data(), across, strength, bands) == whole);
    }
    OA_CHECK(
        draw_flashes(palette, nullptr, {centre}, oa::app::FlashStrength::full, 1) ==
        ground_frame(palette)
    );
}

/// A frame's shadows set from its zoom: the game's own at zoom 1 and
/// closer, through the faded table between, and none once they would be
/// fainter than a sixteenth of the game's own, a little before a quarter
/// out, where the renderer's shadow option is cleared for the frame alone.
void test_frame_shadows() {
    const auto palette = test_palette();
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    model_render::ShadowTable table;
    oa::app::WorldDrawList list;
    model_render::ModelRenderer renderer;
    constexpr uint16_t game_flags =
        model_render::graphics_shadows | model_render::graphics_vehicle_shadows;
    std::vector<uint8_t> pixels{5, 5, 0xff, 9};
    oa::Sprite shadow_sprite{};
    shadow_sprite.width = 2;
    shadow_sprite.height = 2;
    shadow_sprite.key = 0xff;
    shadow_sprite.data = pixels.data();
    for (const float zoom : {1.0F, 2.0F, 4.0F}) {
        renderer.graphics_flags = game_flags;
        oa::app::set_frame_shadows(list, renderer, table, display, &shadow_sprite, zoom);
        OA_CHECK(list.shadow_level == model_render::shadow_full_level);
        OA_CHECK(oa::app::shadows_drawn(list));
        OA_CHECK(renderer.shadow_table == nullptr);
        OA_CHECK(renderer.graphics_flags == game_flags);
    }
    for (const float zoom : {0.75F, 0.5F, 1.0F / 3.0F}) {
        renderer.graphics_flags = game_flags;
        oa::app::set_frame_shadows(list, renderer, table, display, &shadow_sprite, zoom);
        OA_CHECK(list.shadow_level > 0 && list.shadow_level < model_render::shadow_full_level);
        OA_CHECK(renderer.shadow_table != nullptr);
        OA_CHECK(renderer.graphics_flags == game_flags);
        if (renderer.shadow_table == nullptr)
            continue;
        // The silhouettes' row and the shadow sprite's colours are faded,
        // and others are the display's.
        bool faded = false;
        for (const std::size_t colour : {0U, 5U, 9U})
            for (std::size_t under = 0; under < 256; ++under)
                faded = faded || renderer.shadow_table[colour * 256U + under] !=
                                     display.alpha[colour * 256U + under];
        OA_CHECK(faded);
        bool kept = true;
        for (std::size_t under = 0; under < 256; ++under)
            kept = kept &&
                   renderer.shadow_table[6U * 256U + under] == display.alpha[6U * 256U + under];
        OA_CHECK(kept);
    }
    for (const float zoom : {0.26F, 0.25F, 0.2F, 1.0F / 6.0F}) {
        renderer.graphics_flags = game_flags;
        oa::app::set_frame_shadows(list, renderer, table, display, &shadow_sprite, zoom);
        OA_CHECK(list.shadow_level == 0);
        OA_CHECK(!oa::app::shadows_drawn(list));
        OA_CHECK(renderer.shadow_table == nullptr);
        OA_CHECK(renderer.graphics_flags == model_render::graphics_vehicle_shadows);
    }
}

namespace gaf = oa::formats::gaf;

/// Writes a little-endian value of `bytes` bytes at `at`.
void put_le(std::vector<uint8_t>& file, std::size_t at, uint32_t value, std::size_t bytes) {
    for (std::size_t index = 0; index < bytes; ++index)
        file[at + index] = static_cast<uint8_t>(value >> (8U * index));
}

/// Returns a GAF file of one sequence of raw square frames, each as many
/// pixels across as its side, the pixels of frame i counting up from i, with
/// palette index 0 transparent.
///
/// @param sides each frame's side, in pixels
/// @return the file's bytes
std::vector<uint8_t> raw_frames_file(std::span<const uint16_t> sides) {
    constexpr std::size_t list_at = 56;
    constexpr std::size_t list_item_bytes = 8;
    constexpr std::size_t record_bytes = 24;
    const std::size_t records_at = list_at + sides.size() * list_item_bytes;
    std::size_t pixels_at = records_at + sides.size() * record_bytes;
    std::vector<uint8_t> file(pixels_at);
    put_le(file, 0, 0x00010100U, 4);
    put_le(file, 4, 1, 4);
    put_le(file, 12, 16, 4);
    put_le(file, 16, static_cast<uint32_t>(sides.size()), 2);
    for (std::size_t frame = 0; frame < sides.size(); ++frame) {
        const std::size_t record_at = records_at + frame * record_bytes;
        put_le(file, list_at + frame * list_item_bytes, static_cast<uint32_t>(record_at), 4);
        put_le(file, list_at + frame * list_item_bytes + 4, 2, 4);
        put_le(file, record_at, sides[frame], 2);
        put_le(file, record_at + 2, sides[frame], 2);
        put_le(file, record_at + 16, static_cast<uint32_t>(pixels_at), 4);
        const std::size_t pixel_count = std::size_t{sides[frame]} * sides[frame];
        for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
            file.push_back(static_cast<uint8_t>(frame + pixel));
        pixels_at += pixel_count;
    }
    return file;
}

/// The file a ranged render reads, the reads it made and whether they fail.
struct FileReads {
    const std::vector<uint8_t>* file{};
    std::size_t reads{};
    bool fail{};
};

/// Copies bytes of the file a FileReads names; past its end, or when told
/// to fail, reads nothing.
bool read_file(void* context, uint32_t offset, std::span<uint8_t> output) {
    auto& reads = *static_cast<FileReads*>(context);
    ++reads.reads;
    const auto& file = *reads.file;
    if (reads.fail || offset > file.size() || output.size() > file.size() - offset)
        return false;
    std::copy_n(file.begin() + offset, output.size(), output.begin());
    return true;
}

/// Returns a rendered frame, all its pixels covered.
///
/// @param width pixels across
/// @param height pixels down
/// @return the frame, 2 bytes a pixel
std::shared_ptr<const gaf::RenderedFrame> covered_frame(uint16_t width, uint16_t height) {
    const std::size_t pixels = std::size_t{width} * height;
    return std::make_shared<const gaf::RenderedFrame>(gaf::RenderedFrame{
        width, height, 0, 0, 0, std::vector<uint8_t>(pixels, 1), std::vector<uint8_t>(pixels, 1)
    });
}

/// A file is drawn a frame at a time once its pixels and coverage, decoded,
/// take more than the threshold.
void test_frame_by_frame_threshold() {
    const std::array<uint16_t, 2> sides{4, 2};
    const auto parsed = gaf::parse(raw_frames_file(sides), gaf::PixelData::checked);
    OA_CHECK(parsed.ok());
    if (!parsed.ok())
        return;
    // 16 and 4 pixels, 2 bytes each.
    OA_CHECK(gaf::decoded_bytes(*parsed.archive) == 40);
    OA_CHECK(oa::app::draws_frame_by_frame(*parsed.archive, 39));
    OA_CHECK(!oa::app::draws_frame_by_frame(*parsed.archive, 40));
    OA_CHECK(!oa::app::draws_frame_by_frame(*parsed.archive));
}

/// An empty cache holds nothing and knows no failed frame.
void test_frame_cache_empty() {
    oa::app::GafFrameCache cache(100);
    const gaf::Frame frame{};
    OA_CHECK(cache.find(&frame) == nullptr);
    OA_CHECK(!cache.failed(&frame));
    OA_CHECK(cache.kept_bytes() == 0 && cache.kept_frames() == 0);
    OA_CHECK(cache.budget_bytes() == 100);
    cache.clear();
    OA_CHECK(cache.kept_bytes() == 0 && cache.kept_frames() == 0);
}

/// A frame larger than the whole budget is not kept, and nothing kept goes
/// for it.
void test_frame_cache_over_budget() {
    oa::app::GafFrameCache cache(40);
    const std::array<gaf::Frame, 2> frames{};
    OA_CHECK(cache.keep(&frames[0], covered_frame(4, 4)));
    OA_CHECK(!cache.keep(&frames[1], covered_frame(5, 5)));
    OA_CHECK(cache.find(&frames[1]) == nullptr);
    OA_CHECK(cache.find(&frames[0]) != nullptr);
    OA_CHECK(cache.kept_bytes() == 32 && cache.kept_frames() == 1);
}

/// To make room, the frame drawn longest ago goes first; finding a frame
/// counts as drawing it.
void test_frame_cache_order() {
    // Room for three frames of 2 by 2, 8 bytes each.
    oa::app::GafFrameCache cache(24);
    const std::array<gaf::Frame, 4> frames{};
    for (std::size_t index = 0; index < 3; ++index)
        OA_CHECK(cache.keep(&frames[index], covered_frame(2, 2)));
    OA_CHECK(cache.find(&frames[0]) != nullptr);
    OA_CHECK(cache.keep(&frames[3], covered_frame(2, 2)));
    OA_CHECK(cache.find(&frames[1]) == nullptr);
    OA_CHECK(cache.find(&frames[0]) != nullptr);
    OA_CHECK(cache.find(&frames[2]) != nullptr);
    OA_CHECK(cache.find(&frames[3]) != nullptr);
    OA_CHECK(cache.kept_bytes() == 24 && cache.kept_frames() == 3);
    // A larger frame lets as many go as it needs, longest ago first: 2,
    // then 3, were drawn before 0.
    OA_CHECK(cache.find(&frames[2]) != nullptr);
    OA_CHECK(cache.find(&frames[3]) != nullptr);
    OA_CHECK(cache.find(&frames[0]) != nullptr);
    OA_CHECK(cache.keep(&frames[1], covered_frame(4, 2)));
    OA_CHECK(cache.find(&frames[2]) == nullptr && cache.find(&frames[3]) == nullptr);
    OA_CHECK(cache.find(&frames[0]) != nullptr && cache.find(&frames[1]) != nullptr);
    OA_CHECK(cache.kept_bytes() == 24 && cache.kept_frames() == 2);
    cache.clear();
    OA_CHECK(cache.find(&frames[0]) == nullptr && cache.kept_bytes() == 0);
}

/// Frames of a file drawn a frame at a time render from the file as they
/// render decoded, each read once while the cache keeps it; one larger than
/// the budget is drawn and read again for each frame's draws, a frame the
/// cache let go stays whole while the draws that use it hold it, and a
/// frame that failed is not read again.
void test_ranged_frames() {
    const std::array<uint16_t, 3> sides{4, 8, 4};
    const auto file = raw_frames_file(sides);
    const auto checked = gaf::parse(file, gaf::PixelData::checked);
    const auto decoded = gaf::parse(file);
    OA_CHECK(checked.ok() && decoded.ok());
    if (!checked.ok() || !decoded.ok())
        return;
    const auto& frames = checked.archive->sequences[0].frames;
    const auto rendered_as_decoded = [&](const gaf::RenderedFrame* rendered, std::size_t index) {
        const auto normal = gaf::render_normal(decoded.archive->sequences[0].frames[index]);
        return rendered != nullptr && normal.ok() && rendered->pixels == normal.frame->pixels &&
               rendered->coverage == normal.frame->coverage;
    };
    FileReads reads{&file};
    const gaf::ReadHooks reader{&reads, read_file};
    // Room for one frame of 4 by 4, 32 bytes.
    oa::app::GafFrameCache cache(32);
    oa::app::WorldDrawList list;
    const auto* first = oa::app::ranged_frame(list, cache, frames[0], reader);
    OA_CHECK(rendered_as_decoded(first, 0));
    OA_CHECK(oa::app::ranged_frame(list, cache, frames[0], reader) == first);
    OA_CHECK(reads.reads == 1 && cache.kept_frames() == 1);
    const auto* large = oa::app::ranged_frame(list, cache, frames[1], reader);
    OA_CHECK(rendered_as_decoded(large, 1));
    OA_CHECK(reads.reads == 2 && cache.kept_frames() == 1 && list.held.size() == 2);

    oa::app::clear_world_draws(list);
    OA_CHECK(list.held.empty());
    OA_CHECK(rendered_as_decoded(oa::app::ranged_frame(list, cache, frames[0], reader), 0));
    OA_CHECK(reads.reads == 2);
    OA_CHECK(rendered_as_decoded(oa::app::ranged_frame(list, cache, frames[1], reader), 1));
    OA_CHECK(reads.reads == 3);
    // Frame 2 takes frame 0's place in the cache; the draws still hold 0.
    const auto* held = oa::app::ranged_frame(list, cache, frames[0], reader);
    OA_CHECK(rendered_as_decoded(oa::app::ranged_frame(list, cache, frames[2], reader), 2));
    OA_CHECK(cache.kept_frames() == 1 && cache.find(&frames[0]) == nullptr);
    OA_CHECK(rendered_as_decoded(held, 0));

    oa::app::clear_world_draws(list);
    cache.clear();
    reads.fail = true;
    std::optional<gaf::Error> failure;
    OA_CHECK(oa::app::ranged_frame(list, cache, frames[0], reader, &failure) == nullptr);
    OA_CHECK(failure.has_value() && failure->code == gaf::ErrorCode::unreadable);
    OA_CHECK(cache.failed(&frames[0]));
    const auto reads_before = reads.reads;
    oa::app::clear_world_draws(list);
    reads.fail = false;
    failure.reset();
    OA_CHECK(oa::app::ranged_frame(list, cache, frames[0], reader, &failure) == nullptr);
    OA_CHECK(!failure.has_value() && reads.reads == reads_before);
}

/// Returns a frame whose pixels all differ: red the column, green the row,
/// blue the two mixed.
///
/// @return the frame's RGB pixels
std::vector<uint8_t> distinct_frame() {
    std::vector<uint8_t> rgb(std::size_t{frame_width} * frame_height * 3U);
    for (int32_t y = 0; y < frame_height; ++y)
        for (int32_t x = 0; x < frame_width; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * frame_width + static_cast<std::size_t>(x)) * 3U;
            rgb[at] = static_cast<uint8_t>(x);
            rgb[at + 1] = static_cast<uint8_t>(y);
            rgb[at + 2] = static_cast<uint8_t>(x * 3 + y * 5);
        }
    return rgb;
}

/// Returns a whole frame as a target, clipped to a rectangle, on one band.
///
/// @param rgb the frame's RGB pixels
/// @param clip_x the clip's left column
/// @param clip_y its top row
/// @param clip_width its columns
/// @param clip_height its rows
/// @return the target
oa::app::WorldTarget lens_target(
    std::vector<uint8_t>& rgb,
    int32_t clip_x = 0,
    int32_t clip_y = 0,
    int32_t clip_width = frame_width,
    int32_t clip_height = frame_height
) {
    return {
        rgb.data(),
        frame_width,
        frame_height,
        clip_x,
        clip_y,
        clip_width,
        clip_height,
        0,
        frame_height
    };
}

/// The step a map pixel of the lens's middle five takes toward the pixel it
/// shows: none for the middle three, one inward for the outer two.
///
/// @param offset the pixel's offset from the centre, -2 to 2
/// @return the offset of the pixel it shows
int32_t lens_step(int32_t offset) {
    return offset == -2 ? -1 : offset == 2 ? 1 : 0;
}

/// Returns a frame with the lens's rule applied at a whole scale: each map
/// pixel of the middle five shows the one lens_step names, read from the
/// frame before, every block of `scale` pixels moving whole.
///
/// @param before the frame before
/// @param centre the lens's centre
/// @param scale frame pixels per map pixel
/// @param target what may be written
/// @return the frame after
std::vector<uint8_t> lens_rule(
    const std::vector<uint8_t>& before,
    const oa::present::world_renderer::ScreenPoint& centre,
    int32_t scale,
    const oa::app::WorldTarget& target
) {
    auto after = before;
    const int32_t half = oa::app::projectile_lens_side / 2;
    const int32_t left = centre.x - half * scale;
    const int32_t top = centre.y - half * scale;
    for (int32_t y = top; y < top + oa::app::projectile_lens_side * scale; ++y)
        for (int32_t x = left; x < left + oa::app::projectile_lens_side * scale; ++x) {
            const int32_t dx = (x - left) / scale - half;
            const int32_t dy = (y - top) / scale - half;
            if (std::abs(dx) > 2 || std::abs(dy) > 2)
                continue;
            if (x < target.clip_x || x >= target.clip_x + target.clip_width || y < target.clip_y ||
                y >= target.clip_y + target.clip_height)
                continue;
            const int32_t from_x = x + (lens_step(dx) - dx) * scale;
            const int32_t from_y = y + (lens_step(dy) - dy) * scale;
            const auto to =
                (static_cast<std::size_t>(y) * frame_width + static_cast<std::size_t>(x)) * 3U;
            const auto from = (static_cast<std::size_t>(from_y) * frame_width +
                               static_cast<std::size_t>(from_x)) *
                              3U;
            for (std::size_t channel = 0; channel < 3; ++channel)
                after[to + channel] = before[from + channel];
        }
    return after;
}

/// A projectile's lens: at scale 1 exactly 24 pixels change, as the rule
/// says, and the same as the 8-bit lens sprite drawn through the lens
/// table; at scales 2 and 3 whole blocks move; a lens over the clip's edge
/// changes only the pixels inside it; a second lens reads what the first
/// drew; drawn as a draw of the list it is the same; the battlefield test
/// takes its edges and the height's lift, rounded down; and the lens is
/// drawn at the point the test takes, at scales 1 and 2.
void test_projectile_lens() {
    const oa::present::world_renderer::ScreenPoint centre{40, 50};
    const auto before = distinct_frame();
    {
        auto rgb = before;
        oa::app::draw_world_lens(lens_target(rgb), centre, 1.0F);
        OA_CHECK(rgb == lens_rule(before, centre, 1, lens_target(rgb)));
        std::size_t changed = 0;
        for (std::size_t at = 0; at < rgb.size(); at += 3)
            changed += rgb[at] != before[at] || rgb[at + 1] != before[at + 1];
        OA_CHECK(changed == 24);
    }
    // The 8-bit lens sprite over the same picture as palette indices, none of
    // them the sprite's key 0.
    {
        const auto palette = test_palette();
        auto surface = oa::present::create_surface(frame_width, frame_height);
        std::vector<uint8_t> rgb(std::size_t{frame_width} * frame_height * 3U);
        for (int32_t y = 0; y < frame_height; ++y)
            for (int32_t x = 0; x < frame_width; ++x) {
                const auto index = static_cast<uint8_t>(1 + (x * 7 + y * 13) % 255);
                surface.pixels
                    [static_cast<std::size_t>(y) * frame_width + static_cast<std::size_t>(x)] =
                    index;
                const auto colour = entry_colour(palette, index);
                std::copy(
                    colour.begin(),
                    colour.end(),
                    rgb.begin() + static_cast<std::ptrdiff_t>(
                                      (static_cast<std::size_t>(y) * frame_width +
                                       static_cast<std::size_t>(x)) *
                                      3U
                                  )
                );
            }
        auto lens = oa::present::build_lens_frame(
            oa::app::projectile_lens_side,
            oa::app::projectile_lens_side,
            oa::app::projectile_lens_strength
        );
        oa::present::draw_displacement_sprite(&surface.surface, lens.sprite, centre.x, centre.y);
        oa::app::draw_world_lens(lens_target(rgb), centre, 1.0F);
        bool same = true;
        for (int32_t y = 0; y < frame_height; ++y)
            for (int32_t x = 0; x < frame_width; ++x)
                same = same &&
                       pixel_at(rgb, x, y) == entry_colour(
                                                  palette,
                                                  surface.pixels
                                                      [static_cast<std::size_t>(y) * frame_width +
                                                       static_cast<std::size_t>(x)]
                                              );
        OA_CHECK(same);
    }
    for (const int32_t scale : {2, 3}) {
        auto rgb = before;
        oa::app::draw_world_lens(lens_target(rgb), centre, static_cast<float>(scale));
        OA_CHECK(rgb == lens_rule(before, centre, scale, lens_target(rgb)));
    }
    // The clip's left edge on the centre's column, as when the centre is on
    // the battlefield's edge.
    {
        auto rgb = before;
        const auto clipped = lens_target(rgb, centre.x, 0, frame_width - centre.x);
        oa::app::draw_world_lens(clipped, centre, 1.0F);
        OA_CHECK(rgb == lens_rule(before, centre, 1, clipped));
    }
    // A second lens a pixel to the right reads the first's pixels.
    {
        auto rgb = before;
        const oa::present::world_renderer::ScreenPoint next{centre.x + 1, centre.y};
        oa::app::draw_world_lens(lens_target(rgb), centre, 1.0F);
        oa::app::draw_world_lens(lens_target(rgb), next, 1.0F);
        const auto first = lens_rule(before, centre, 1, lens_target(rgb));
        OA_CHECK(rgb == lens_rule(first, next, 1, lens_target(rgb)));
    }
    // As a draw of the list, on one band.
    {
        const auto palette = test_palette();
        auto rgb = before;
        model_render::ModelDisplay display;
        model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
        model_render::RgbBridge bridge;
        const model_render::RgbFrame frame{rgb.data(), frame_width, frame_height, frame_width * 3};
        model_render::bridge_begin(
            bridge, frame, {0, 0, frame_width - 1, frame_height - 1}, 1.0F, display.palette
        );
        oa::app::WorldDrawList list;
        list.lenses.push_back(centre);
        oa::app::add_world_draw(list, WorldDrawKind::lens, 0);
        oa::app::WorldFrameDraw draw;
        draw.target = lens_target(rgb);
        draw.palette = &palette;
        draw.bridge = &bridge;
        draw.display = &display;
        std::vector<model_render::BridgeBand> split;
        model_render::bridge_split(bridge, 1, split);
        model_render::ModelRenderer renderer;
        model_render::SupersampleScratch supersample;
        std::vector<oa::formats::objects3d::FixedVector3> points;
        oa::app::draw_world_band(list, draw, split.front(), renderer, supersample, points);
        model_render::bridge_join_band(bridge, split.front());
        OA_CHECK(rgb == lens_rule(before, centre, 1, lens_target(rgb)));
        oa::app::clear_world_draws(list);
        OA_CHECK(list.lenses.empty());
    }
    // The battlefield from (128, 32) to (767, 511), the camera at (100, 200).
    const oa::sim::effect_particles::ExplosionView view{100, 200, {128, 32, 767, 511}};
    const auto at = [](int32_t x, int32_t height, int32_t z) {
        return std::array<uint32_t, 3>{
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(height) << 16,
            static_cast<uint32_t>(z) << 16
        };
    };
    OA_CHECK(oa::app::projectile_lens_on_battlefield(view, at(100, 0, 200)));
    OA_CHECK(oa::app::projectile_lens_on_battlefield(view, at(739, 0, 679)));
    OA_CHECK(!oa::app::projectile_lens_on_battlefield(view, at(99, 0, 300)));
    OA_CHECK(!oa::app::projectile_lens_on_battlefield(view, at(740, 0, 300)));
    OA_CHECK(!oa::app::projectile_lens_on_battlefield(view, at(300, 0, 680)));
    // Half the height lifts it back onto the bottom edge, and off the top.
    OA_CHECK(oa::app::projectile_lens_on_battlefield(view, at(300, 2, 680)));
    OA_CHECK(!oa::app::projectile_lens_on_battlefield(view, at(300, 2, 200)));
    // Half an odd height is rounded down: a height of 3 lifts it by 1, onto
    // the bottom edge from one row below it, and no further.
    OA_CHECK(oa::app::projectile_lens_on_battlefield(view, at(300, 3, 680)));
    OA_CHECK(!oa::app::projectile_lens_on_battlefield(view, at(300, 3, 681)));

    // The lens is drawn where the test places it, on the same battlefield
    // and camera: half the whole height rounded down, one row below where
    // half of an odd height rounded to the nearest pixel puts it.
    using oa::app::HeightLift;
    oa::present::world_renderer::BattlefieldViewport viewport{};
    viewport.source_x = 100;
    viewport.source_y = 200;
    viewport.width = 640;
    viewport.height = 480;
    const auto lens_at = [&viewport](const std::array<uint32_t, 3>& position) {
        return oa::app::project_world_point(viewport, position, HeightLift::down);
    };
    const auto nearest_at = [&viewport](const std::array<uint32_t, 3>& position) {
        return oa::app::project_world_point(viewport, position, HeightLift::nearest);
    };
    OA_CHECK(lens_at(at(300, 3, 680)).x == 328);
    OA_CHECK(lens_at(at(300, 3, 680)).y == 511);
    OA_CHECK(nearest_at(at(300, 3, 680)).y == 510);
    OA_CHECK(lens_at(at(300, 139, 400)).y == 163);
    OA_CHECK(nearest_at(at(300, 139, 400)).y == 162);
    // A height's fraction is dropped before it is halved.
    auto between = at(300, 139, 400);
    between[1] |= 0xffffU;
    OA_CHECK(lens_at(between).y == 163);
    // Even and negative heights land alike either way.
    OA_CHECK(lens_at(at(300, 138, 400)).y == 163);
    OA_CHECK(nearest_at(at(300, 138, 400)).y == 163);
    OA_CHECK(lens_at(at(300, -3, 400)).y == 234);
    OA_CHECK(nearest_at(at(300, -3, 400)).y == 234);
    // At scale 2 the rounded-down half is doubled.
    viewport.scale = 2.0F;
    OA_CHECK(lens_at(at(300, 139, 400)).x == 528);
    OA_CHECK(lens_at(at(300, 139, 400)).y == 294);
    OA_CHECK(nearest_at(at(300, 139, 400)).y == 293);
}

/// An explosion record's flash and sprite, placed with HeightLift::down, land
/// on the point the record is culled on (draw_explosions): a record at an odd
/// height on the battlefield's bottom row is drawn on that row, one row
/// below where half its height rounded to the nearest pixel would put it,
/// and the record a row lower is culled.
void test_explosion_record_points() {
    // The battlefield from (128, 32) to (767, 511), the camera at (100, 200).
    const oa::sim::effect_particles::ExplosionView view{100, 200, {128, 32, 767, 511}};
    oa::present::world_renderer::BattlefieldViewport viewport{};
    viewport.source_x = 100;
    viewport.source_y = 200;
    viewport.width = 640;
    viewport.height = 480;
    const oa::formats::gaf::Sequence sequence{};
    const auto world = std::make_unique<oa::sim::effect_particles::EffectWorld>();
    // Whole map pixels x, height and z: on the bottom row at an odd
    // height, one row below it, and an odd height inside the battlefield.
    const std::array<std::array<int32_t, 3>, 3> centres{
        {{300, 3, 680}, {300, 3, 681}, {400, 139, 400}}
    };
    for (const auto& centre : centres) {
        auto& record = world->explosions[world->explosion_count++];
        record.position = {centre[0] * 0x10000, centre[1] * 0x10000, centre[2] * 0x10000};
        record.flash.sequence = &sequence;
        record.sprite.sequence = &sequence;
    }

    struct Placed {
        oa::sim::effect_particles::DrawKind kind{};
        oa::present::world_renderer::ScreenPoint down{};
        oa::present::world_renderer::ScreenPoint nearest{};
    };

    struct Context {
        const oa::present::world_renderer::BattlefieldViewport* viewport{};
        std::vector<Placed> placed;
    } context{&viewport, {}};

    oa::sim::effect_particles::draw_explosions(
        *world, view, &context, [](void* raw, const oa::sim::effect_particles::ParticleDraw& item) {
            auto& out = *static_cast<Context*>(raw);
            const std::array<uint32_t, 3> position{
                static_cast<uint32_t>(item.position.x),
                static_cast<uint32_t>(item.position.y),
                static_cast<uint32_t>(item.position.z)
            };
            out.placed.push_back(
                {item.kind,
                 oa::app::project_world_point(*out.viewport, position, oa::app::HeightLift::down),
                 oa::app::project_world_point(
                     *out.viewport, position, oa::app::HeightLift::nearest
                 )}
            );
        }
    );
    using oa::sim::effect_particles::DrawKind;
    // Two flashes, then the two records' sprites; the record a row lower
    // draws nothing.
    OA_CHECK(context.placed.size() == 4);
    if (context.placed.size() != 4)
        return;
    OA_CHECK(context.placed[0].kind == DrawKind::flash);
    OA_CHECK(context.placed[1].kind == DrawKind::flash);
    OA_CHECK(context.placed[2].kind == DrawKind::sprite);
    OA_CHECK(context.placed[3].kind == DrawKind::sprite);
    for (const std::size_t index : {std::size_t{0}, std::size_t{2}}) {
        const auto& placed = context.placed[index];
        OA_CHECK(placed.down.x == 328 && placed.down.y == 511);
        OA_CHECK(placed.nearest.y == 510);
    }
    for (const std::size_t index : {std::size_t{1}, std::size_t{3}}) {
        const auto& placed = context.placed[index];
        OA_CHECK(placed.down.x == 428 && placed.down.y == 163);
        OA_CHECK(placed.nearest.y == 162);
    }
}

} // namespace

int main() {
    test_thin_lines_in_bands();
    test_feature_shadows();
    test_sprites_stay_on_their_ground();
    test_frame_shadows();
    test_explosion_flashes();
    test_projectile_lens();
    test_explosion_record_points();
    test_frame_by_frame_threshold();
    test_frame_cache_empty();
    test_frame_cache_over_budget();
    test_frame_cache_order();
    test_ranged_frames();
    return oa::test::check_exit_status();
}
