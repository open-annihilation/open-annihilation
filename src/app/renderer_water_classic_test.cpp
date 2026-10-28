// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
// Classic water geometry remains bounded, visible, optional and outside dry land,
// and Water ripples move the map's own water art.
#include "renderer_water_classic.hpp"
#include "renderer_water_appearance.hpp"
#include "renderer_water_surface.hpp"
#include "renderer_water_effects.hpp"
#include "oa/test/check.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string_view>
#include <vector>
#include <utility>

using namespace oa::app;

namespace {
/// Draws a frame's draw batches over a picture, each triangle's colour and
/// alpha blended between its corners, within its batch's scissor: enough to
/// see where the water's geometry lands.
/// @param frame the frame to draw
/// @param[in,out] rgb the picture, 3 bytes a pixel, `width` across
/// @param width pixels across
/// @param height pixels down
void draw_frame(
    const card::CardFrame& frame, std::vector<uint8_t>& rgb, int32_t width, int32_t height
) {
    for (const auto& batch : frame.batches) {
        if (batch.operation != card::Operation::draw)
            continue;
        int32_t clip_left = 0, clip_top = 0, clip_right = width, clip_bottom = height;
        if (batch.scissored) {
            clip_left = std::max(clip_left, batch.scissor.x);
            clip_top = std::max(clip_top, batch.scissor.y);
            clip_right = std::min(clip_right, batch.scissor.x + batch.scissor.width);
            clip_bottom = std::min(clip_bottom, batch.scissor.y + batch.scissor.height);
        }
        for (uint32_t i = 0; i + 2 < batch.index_count; i += 3) {
            const auto& a = frame.vertices[frame.indices[batch.first_index + i]];
            const auto& b = frame.vertices[frame.indices[batch.first_index + i + 1]];
            const auto& c = frame.vertices[frame.indices[batch.first_index + i + 2]];
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (area == 0)
                continue;
            const int32_t left =
                std::max(clip_left, int32_t(std::floor(std::min({a.x, b.x, c.x}))));
            const int32_t right =
                std::min(clip_right, int32_t(std::ceil(std::max({a.x, b.x, c.x}))));
            const int32_t top = std::max(clip_top, int32_t(std::floor(std::min({a.y, b.y, c.y}))));
            const int32_t bottom =
                std::min(clip_bottom, int32_t(std::ceil(std::max({a.y, b.y, c.y}))));
            for (int32_t y = top; y < bottom; ++y)
                for (int32_t x = left; x < right; ++x) {
                    const float px = float(x) + 0.5F, py = float(y) + 0.5F;
                    const float wa = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                    const float wb = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                    const float wc = 1 - wa - wb;
                    if (wa < 0 || wb < 0 || wc < 0)
                        continue;
                    const auto mix = [&](float va, float vb, float vc) {
                        return va * wa + vb * wb + vc * wc;
                    };
                    const float alpha = mix(a.colour.alpha, b.colour.alpha, c.colour.alpha);
                    const std::array<float, 3> colour{
                        mix(a.colour.red, b.colour.red, c.colour.red),
                        mix(a.colour.green, b.colour.green, c.colour.green),
                        mix(a.colour.blue, b.colour.blue, c.colour.blue)
                    };
                    auto* pixel = &rgb[(std::size_t(y) * std::size_t(width) + std::size_t(x)) * 3];
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        pixel[channel] = uint8_t(
                            std::lround(
                                std::clamp(
                                    float(pixel[channel]) * (1 - alpha) +
                                        colour[channel] * 255.0F * alpha,
                                    0.0F,
                                    255.0F
                                )
                            )
                        );
                }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    // --pictures DIR also writes the map-edge case's pictures there.
    std::filesystem::path pictures;
    if (argc > 2 && std::string_view(argv[1]) == "--pictures")
        pictures = argv[2];
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
        oa::formats::tnt::Map ocean;
        ocean.attribute_width = ocean.attribute_height = 1024;
        ocean.sea_level = 32;
        ocean.attributes.resize(1024 * 1024);
        renderer_water_classic::Inputs water;
        water.map = &ocean;
        water.view.width = 1920;
        water.view.height = 1080;
        water.ripples = water.coast = 1;
        water.wind.strength = 1;
        water.visible = [](float, float) { return true; };
        card::CardFrame output;
        for (const float zoom : {1.0F, 0.5F, 0.25F}) {
            water.view.zoom = zoom;
            double total = 0, maximum = 0;
            std::size_t vertices = 0;
            for (int frame = 0; frame < 20; ++frame) {
                water.seconds = frame / 30.0;
                output.reset();
                const auto start = std::chrono::steady_clock::now();
                renderer_water_classic::surface(output, water);
                const auto motion =
                    renderer_water_classic::art_motion(water, 0, 0, 1920 / zoom, 1080 / zoom);
                vertices = std::max(vertices, motion.offsets.size());
                const double elapsed = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - start
                )
                                           .count();
                total += elapsed;
                maximum = std::max(maximum, elapsed);
                vertices = std::max(vertices, output.vertices.size());
            }
            std::printf(
                "classic water geometry and art motion zoom %.2f: 20 frames, mean %.4f ms, max "
                "%.4f ms, max %zu vertices or motion points\n",
                zoom,
                total / 20,
                maximum,
                vertices
            );
        }
        return 0;
    }
    oa::formats::tnt::Map map;
    map.attribute_width = 32;
    map.attribute_height = 32;
    map.sea_level = 32;
    map.attributes.resize(32 * 32);
    renderer_water_classic::Inputs input;
    input.map = &map;
    input.view.width = 320;
    input.view.height = 240;
    input.ripples = 1;
    input.coast = 1;
    input.wind.strength = 1;
    input.visible = [](float, float) { return true; };
    // Water ripples wash no layer of their own over the water: they move
    // the map's own water art (art_motion), and the surface keeps only the
    // coast's foam and the shots' rings, none on this deep sea.
    card::CardFrame frame;
    renderer_water_classic::surface(frame, input);
    OA_CHECK(frame.vertices.empty());
    input.view.zoom = 1;
    const auto motion = renderer_water_classic::art_motion(input, 0, 0, 320, 240);
    OA_CHECK(motion.spacing == 16 && motion.first_x == 0 && motion.first_y == 0);
    OA_CHECK(motion.columns == 21 && motion.rows == 16);
    OA_CHECK(motion.offsets.size() == 21U * 16U && !motion.still());
    // The grid's offsets are the shared waves' at the water plane each
    // picture row shows, as the rotated view's stages compute them.
    float largest = 0;
    for (uint32_t row = 0; row < motion.rows; ++row)
        for (uint32_t column = 0; column < motion.columns; ++column) {
            const float x = float(column * motion.spacing), y = float(row * motion.spacing);
            const auto expected = renderer_water_surface::art_offset(
                x, y + map.sea_level * 0.5F, input.wind, renderer_water_surface::fine_detail(1)
            );
            const auto& offset = motion.offsets[row * motion.columns + column];
            OA_CHECK(std::abs(offset[0] - expected[0]) < 1.0e-3F);
            OA_CHECK(std::abs(offset[1] - expected[1]) < 1.0e-3F);
            largest = std::max(largest, std::hypot(offset[0], offset[1]));
        }
    // A gentle motion of a pixel or two, never a jump across the art.
    OA_CHECK(largest > 0.75F && largest < 4.75F);
    // The grid keeps 8 to 16 window pixels between its points at any zoom.
    for (const auto& [zoom, spacing] : std::array<std::pair<float, uint32_t>, 6>{
             {{0.5F, 32}, {0.75F, 16}, {1.0F, 16}, {1.5F, 8}, {2.0F, 8}, {3.0F, 4}}
         }) {
        input.view.zoom = zoom;
        OA_CHECK(renderer_water_classic::art_motion(input, 0, 0, 320, 240).spacing == spacing);
    }
    // Faded out as the view zooms out to 0.25, and doubled at 200%.
    input.view.zoom = 0.25F;
    OA_CHECK(renderer_water_classic::art_motion(input, 0, 0, 320, 240).offsets.empty());
    input.view.zoom = 0.375F;
    const auto faded = renderer_water_classic::art_motion(input, 0, 0, 320, 240);
    input.view.zoom = 0.5F;
    const auto whole = renderer_water_classic::art_motion(input, 0, 0, 320, 240);
    OA_CHECK(faded.offsets.size() == whole.offsets.size());
    for (std::size_t i = 0; i < std::min(faded.offsets.size(), whole.offsets.size()); ++i)
        OA_CHECK(std::abs(faded.offsets[i][0] - whole.offsets[i][0] * 0.5F) < 1.0e-4F);
    input.view.zoom = 1;
    input.ripples = 2;
    const auto doubled = renderer_water_classic::art_motion(input, 0, 0, 320, 240);
    for (std::size_t i = 0; i < motion.offsets.size(); ++i)
        OA_CHECK(std::abs(doubled.offsets[i][0] - motion.offsets[i][0] * 2) < 1.0e-4F);
    input.ripples = 0;
    OA_CHECK(renderer_water_classic::art_motion(input, 0, 0, 320, 240).offsets.empty());
    input.ripples = 1;
    // The coast stays where it is: no motion on land or at the waterline,
    // which lies within the cell before the land's, and the whole of it
    // from 12 map pixels deep.
    for (uint32_t row = 0; row < map.attribute_height; ++row)
        for (uint32_t column = 16; column < map.attribute_width; ++column)
            map.attributes[row * map.attribute_width + column].height = 40;
    const auto coast = renderer_water_classic::art_motion(input, 0, 0, 512, 512);
    for (uint32_t row = 0; row < coast.rows; ++row)
        for (uint32_t column = 0; column < coast.columns; ++column) {
            const auto& offset = coast.offsets[row * coast.columns + column];
            const float x = float(coast.first_x) + float(column * coast.spacing);
            const float y = float(coast.first_y) + float(row * coast.spacing);
            if (x >= 17 * 16)
                OA_CHECK(offset[0] == 0 && offset[1] == 0);
            if (x <= 13 * 16 && y + map.sea_level * 0.5F < 31 * 16) {
                const auto expected =
                    renderer_water_surface::art_offset(x, y + map.sea_level * 0.5F, input.wind, 0);
                OA_CHECK(std::abs(offset[0] - expected[0]) < 1.0e-3F);
            }
        }
    for (auto& attribute : map.attributes)
        attribute.height = 64;
    OA_CHECK(renderer_water_classic::art_motion(input, 0, 0, 320, 240).offsets.empty());
    for (auto& attribute : map.attributes)
        attribute.height = 0;
    const renderer_water::Wind calm{1, 0, 0, 0, 0};
    const renderer_water::Wind east{1, 0, 1, 0, 0};
    const renderer_water::Wind north{0, 1, 1, 0, 0};
    const renderer_water::Wind drift_east{1, 0, 1, 12, 0};
    const renderer_water::Wind drift_north{0, 1, 1, 0, 12};
    // Calm water stays still; the map's flow turns and moves the motion.
    input.wind = {1, 0, 0, 0, 0};
    OA_CHECK(renderer_water_classic::art_motion(input, 0, 0, 320, 240).offsets.empty());
    input.wind = east;
    bool turned = false, travelled = false;
    for (int z = 0; z < 240; z += 17)
        for (int x = 0; x < 320; x += 19) {
            const float px = float(x), pz = float(z);
            OA_CHECK(
                (renderer_water_surface::art_offset(px, pz, calm, 1) == std::array<float, 2>{})
            );
            turned = turned || renderer_water_surface::art_offset(px, pz, east, 1) !=
                                   renderer_water_surface::art_offset(px, pz, north, 1);
            travelled = travelled || renderer_water_surface::art_offset(px, pz, drift_east, 1) !=
                                         renderer_water_surface::art_offset(px, pz, east, 1);
            OA_CHECK(
                renderer_water_surface::art_offset(px, pz, drift_east, 1) !=
                renderer_water_surface::art_offset(px, pz, drift_north, 1)
            );
        }
    OA_CHECK(turned && travelled);
    // The fine ripples show only up close.
    OA_CHECK(renderer_water_surface::fine_detail(1) == 0);
    OA_CHECK(renderer_water_surface::fine_detail(1.5F) == 1);
    bool fine_detail = false;
    for (int x = 0; x < 320; x += 11)
        fine_detail = fine_detail || renderer_water_surface::art_offset(float(x), 96, east, 0) !=
                                         renderer_water_surface::art_offset(float(x), 96, east, 1);
    OA_CHECK(fine_detail);
    // The swells and ripples travel at their own speeds: no translation of
    // the still field reproduces the moving one.
    for (float ratio : {0.28F, 0.45F, 0.65F, 1.0F, 1.1F, 1.35F}) {
        float difference = 0;
        for (int z = 24; z < 240; z += 31)
            for (int x = 24; x < 320; x += 37)
                difference += std::abs(
                    renderer_water_surface::art_offset(float(x), float(z), drift_east, 1)[0] -
                    renderer_water_surface::art_offset(float(x) - 12 * ratio, float(z), east, 1)[0]
                );
        OA_CHECK(difference > 0.01F);
    }
    input.wind = east;
    frame.reset();
    input.visible = [](float, float) { return false; };
    renderer_water_classic::surface(frame, input);
    OA_CHECK(frame.vertices.empty());
    input.visible = [](float, float) { return true; };
    input.view.zoom = 0.25F;
    renderer_water_classic::surface(frame, input);
    OA_CHECK(frame.vertices.empty());
    input.view.zoom = 1;
    for (auto& attribute : map.attributes)
        attribute.height = 64;
    renderer_water_classic::surface(frame, input);
    OA_CHECK(frame.vertices.empty());
    for (auto& attribute : map.attributes)
        attribute.height = 0;
    input.ripples = input.coast = 0;
    renderer_water_classic::surface(frame, input);
    OA_CHECK(frame.vertices.empty());
    input.reflections = 1;
    oa::Unit ship{};
    ship.position = {128 * 65536, 32 * 65536, 128 * 65536};
    oa::UnitDef definition{};
    definition.flags = OA_UNIT_DEF_FLAG_FLOATER;
    WorldDrawList draws;
    ModelDraw model;
    model.model.unit = &ship;
    model.model.def = &definition;
    draws.models.push_back(model);
    WorldDraw draw{WorldDrawKind::model, 0};
    const auto make_ship = [&]() {
        frame.reset();
        card::append_quad(frame, 120, 96, 16, 16, 0, 0, 1, 1, {1, 1, 1, 1});
        card::Batch batch;
        batch.index_count = 6;
        frame.batches.push_back(batch);
    };
    // A wall 32 map pixels high standing on the waterline: its top corners
    // 64 high, 16 screen pixels above its foot at the sea's 32.
    std::vector<float> heights{64, 64, 32, 32};
    input.vertex_heights = heights;
    make_ship();
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices.size() > 4);
    OA_CHECK(frame.batches.size() == 3);
    OA_CHECK(input.reflected_vertices == frame.vertices.size() - 4);
    OA_CHECK(frame.vertices[0].y == 96);
    // Mirrored in the water's surface right under the hull: each corner as
    // far below the waterline as it stands above it, its foot on the
    // waterline and its top 32 map pixels high shown 32 pixels down, fainter.
    float nearest = 1000, furthest = 0;
    for (std::size_t i = 4; i < frame.vertices.size(); ++i) {
        const auto& corner = frame.vertices[i];
        OA_CHECK(corner.y >= 112 - 1.0e-4F && corner.y <= 128 + 1.0e-4F);
        OA_CHECK(corner.x >= 120 - 0.66F && corner.x <= 136 + 0.66F);
        OA_CHECK(corner.colour.alpha <= renderer_water_appearance::reflection_opacity + 1.0e-6F);
        if (std::abs(corner.y - 128) < 1.0e-3F)
            OA_CHECK(
                std::abs(
                    corner.colour.alpha - renderer_water_appearance::reflection_opacity * 0.68F
                ) < 1.0e-4F
            );
        nearest = std::min(nearest, corner.y);
        furthest = std::max(furthest, corner.y);
    }
    OA_CHECK(std::abs(nearest - 112) < 1.0e-4F && std::abs(furthest - 128) < 1.0e-4F);
    OA_CHECK(frame.batches[1].blend == card::Blend::alpha);
    const auto reflected_x = frame.vertices[4].x;
    make_ship();
    input.seconds += 0.5;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices[4].x != reflected_x);
    // What lies under the water mirrors nothing: a wall standing 16 below
    // the waterline is cut there, at a third of its height up the screen.
    heights = {64, 64, 16, 16};
    input.vertex_heights = heights;
    make_ship();
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    nearest = 1000, furthest = 0;
    for (std::size_t i = 4; i < frame.vertices.size(); ++i) {
        nearest = std::min(nearest, frame.vertices[i].y);
        furthest = std::max(furthest, frame.vertices[i].y);
    }
    OA_CHECK(frame.vertices.size() > 4);
    OA_CHECK(std::abs(nearest - (96 + 16 * 2 / 3.0F)) < 1.0e-3F);
    OA_CHECK(std::abs(furthest - 128) < 1.0e-3F);
    heights = {64, 64, 32, 32};
    input.vertex_heights = heights;
    // Corners the model stage gave no height mirror nothing.
    heights.assign(heights.size(), std::numeric_limits<float>::quiet_NaN());
    make_ship();
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices.size() == 4);
    heights = {64, 64, 32, 32};
    input.vertex_heights = {};
    make_ship();
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices.size() == 4);
    input.vertex_heights = heights;
    make_ship();
    // Deep hull origins still float at their definition's water line.
    definition.water_line = 20;
    ship.position.y = 12 * 65536;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices.size() > 4);
    definition.water_line = 0;
    ship.position.y = 32 * 65536;
    make_ship();
    input.reflected_vertices = 24576;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices.size() == 4);
    make_ship();
    input.reflected_vertices = 0;
    // A dry cell crossed by the reflection rejects it, even though the ship is wet.
    map.attributes[8 * map.attribute_width + 7].height = 64;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices.size() == 4);
    map.attributes[8 * map.attribute_width + 7].height = 0;
    make_ship();
    input.reflections = 0;
    input.shimmer = 1;
    ship.position.y = 20 * 65536;
    SpriteDraw submerged;
    submerged.world_position = ship.position;
    submerged.terrain_decal = true;
    draws.sprites.push_back(submerged);
    draw.kind = WorldDrawKind::sprite;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices[0].x != 120);
    OA_CHECK(frame.vertices.size() == 4);
    const auto shimmer = frame.vertices;
    make_ship();
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    for (std::size_t i = 0; i < frame.vertices.size(); ++i) {
        OA_CHECK(frame.vertices[i].x == shimmer[i].x);
        OA_CHECK(frame.vertices[i].colour.red == shimmer[i].colour.red);
        OA_CHECK(std::isfinite(frame.vertices[i].x));
        OA_CHECK(frame.vertices[i].colour.red >= 0 && frame.vertices[i].colour.red <= 1);
    }
    make_ship();
    input.shimmer = 0;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices[0].x == 120);
    OA_CHECK(frame.vertices[0].colour.red == 1);
    make_ship();
    input.shimmer = 1;
    input.visible = [](float, float) { return false; };
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices[0].x == 120);
    OA_CHECK(frame.vertices[0].colour.red == 1);
    input.visible = [](float, float) { return true; };
    // A submerged flash remains at its physical projection and every corner
    // receives the shared depth tint, including the top of its optical halo.
    draw.kind = WorldDrawKind::lit_sprite;
    draws.sprites[0].terrain_decal = false;
    make_ship();
    for (auto& vertex : frame.vertices)
        vertex.colour.alpha = 0; // Lighten batches use RGB rather than alpha.
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    for (const auto& vertex : frame.vertices) {
        OA_CHECK(vertex.colour.red < 1);
        OA_CHECK(vertex.colour.alpha == 0);
        OA_CHECK(vertex.y == 96 || vertex.y == 112);
    }
    make_ship();
    input.shimmer = 0;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices[0].colour.red == 1 && frame.vertices[0].x == 120);
    input.shimmer = 1;
    draws.sprites[0].world_position->y = 40 * 65536;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices[0].colour.red == 1 && frame.vertices[0].x == 120);
    // Ordinary explosion artwork is also a halo, not an upright dry-topped feature.
    draw.kind = WorldDrawKind::sprite;
    draws.sprites[0].world_position->y = 20 * 65536;
    make_ship();
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    for (const auto& vertex : frame.vertices)
        OA_CHECK(vertex.colour.red < 1);
    const auto light = renderer_water_effects::source(1, 1, 1, 128, 128, 12, 1, 1);
    OA_CHECK(light[0] < light[2] && light[0] < 1 && light[3] == 0);
    OA_CHECK(light == renderer_water_effects::source(1, 1, 1, 128, 128, 12, 1, 1));
    for (const auto unchanged :
         {renderer_water_effects::source(1, 1, 1, 128, 128, -1, 1, 1),
          renderer_water_effects::source(1, 1, 1, 128, 128, 12, 1, 0)})
        OA_CHECK((unchanged == std::array<float, 4>{1, 1, 1, 1}));
    input.ripples = 1;
    input.seconds = 1;
    const std::array<renderer_water::Disturbance, 1> impact{
        {{128, 128, 15, 1, renderer_water::Kind::water_impact}}
    };
    frame.reset();
    renderer_water_classic::surface(frame, input);
    const auto without_impact = frame.vertices.size();
    input.disturbances = impact;
    frame.reset();
    renderer_water_classic::surface(frame, input);
    OA_CHECK(frame.vertices.size() > without_impact);
    // Early impact frames add six splash droplets; old rings and firing
    // disturbances have no droplets. Compare after subtracting surface detail.
    const auto disturbance_vertices = [&](double seconds, renderer_water::Kind kind) {
        input.seconds = seconds;
        input.disturbances = {};
        frame.reset();
        renderer_water_classic::surface(frame, input);
        const auto baseline = frame.vertices.size();
        const std::array<renderer_water::Disturbance, 1> event{{{128, 128, 15, 1, kind}}};
        input.disturbances = event;
        frame.reset();
        renderer_water_classic::surface(frame, input);
        const auto added = frame.vertices.size() - baseline;
        input.disturbances = {};
        return added;
    };
    OA_CHECK(disturbance_vertices(0.7, renderer_water::Kind::water_impact) == (24 + 6) * 4);
    OA_CHECK(disturbance_vertices(0.7, renderer_water::Kind::hit) == (24 + 6) * 4);
    OA_CHECK(disturbance_vertices(1.1, renderer_water::Kind::water_impact) == 24 * 4);
    OA_CHECK(disturbance_vertices(0.7, renderer_water::Kind::firing) == 24 * 4);
    input.disturbances = impact;
    input.seconds = 0.7;
    input.visible = [](float, float) { return false; };
    frame.reset();
    renderer_water_classic::surface(frame, input);
    OA_CHECK(frame.vertices.empty());
    // What lies on the sea bed moves with the art the ripples move under
    // it, and is drawn before the surface's foam and rings.
    {
        oa::formats::tnt::Map sea;
        sea.attribute_width = sea.attribute_height = 32;
        sea.sea_level = 32;
        sea.attributes.resize(32 * 32);
        renderer_water_classic::Inputs water;
        water.map = &sea;
        water.view.width = 320;
        water.view.height = 240;
        water.ripples = 1;
        water.wind.strength = 1;
        water.visible = [](float, float) { return true; };
        const auto art = renderer_water_classic::art_motion(water, 0, 0, 512, 512);
        OA_CHECK(!art.still());
        water.art = &art;
        WorldDrawList sea_bed_draws;
        SpriteDraw patch;
        patch.on_map = true;
        patch.terrain_decal = true;
        patch.world_position = oa::FixedVec3{128 * 65536, 16 * 65536, 128 * 65536};
        sea_bed_draws.sprites.push_back(patch);
        const WorldDraw lying{WorldDrawKind::sprite, 0};
        const auto quad = [&](float top) {
            card::CardFrame drawn;
            card::append_quad(drawn, 120, top, 16, 16, 0, 0, 1, 1, {1, 1, 1, 1});
            card::Batch batch;
            batch.index_count = 6;
            drawn.batches.push_back(batch);
            return drawn;
        };
        auto moved = quad(96);
        const auto placed = moved.vertices;
        renderer_water_classic::objects(moved, water, sea_bed_draws, lying, 0, 0);
        bool moves = false;
        for (std::size_t i = 0; i < placed.size(); ++i) {
            const auto offset = art.at(placed[i].x, placed[i].y);
            OA_CHECK(std::abs(moved.vertices[i].x - placed[i].x - offset[0]) < 1.0e-4F);
            OA_CHECK(std::abs(moved.vertices[i].y - placed[i].y - offset[1]) < 1.0e-4F);
            moves = moves || offset[0] != 0 || offset[1] != 0;
        }
        OA_CHECK(moves);
        OA_CHECK(renderer_water_classic::on_sea_bed(water, sea_bed_draws, lying));
        // Underwater shimmer leaves a feature's frame, the map's own art, in
        // the colours the map gives it: it moves with the art alone.
        water.shimmer = 1;
        auto unshaded = quad(96);
        renderer_water_classic::objects(unshaded, water, sea_bed_draws, lying, 0, 0);
        for (std::size_t i = 0; i < placed.size(); ++i) {
            OA_CHECK(unshaded.vertices[i].x == moved.vertices[i].x);
            OA_CHECK(unshaded.vertices[i].y == moved.vertices[i].y);
            OA_CHECK(
                unshaded.vertices[i].colour.red == 1 && unshaded.vertices[i].colour.green == 1
            );
            OA_CHECK(
                unshaded.vertices[i].colour.blue == 1 && unshaded.vertices[i].colour.alpha == 1
            );
        }
        // An effect under the water at the same place takes the shimmer.
        sea_bed_draws.sprites[0].on_map = false;
        auto effect = quad(96);
        renderer_water_classic::objects(effect, water, sea_bed_draws, lying, 0, 0);
        OA_CHECK(effect.vertices[0].colour.red < 1);
        sea_bed_draws.sprites[0].on_map = true;
        water.shimmer = 0;
        // Without the motion, or above the sea, it stays.
        water.art = nullptr;
        auto still = quad(96);
        renderer_water_classic::objects(still, water, sea_bed_draws, lying, 0, 0);
        OA_CHECK(still.vertices[0].x == 120 && still.vertices[0].y == 96);
        water.art = &art;
        sea_bed_draws.sprites[0].world_position->y = 40 * 65536;
        still = quad(96);
        renderer_water_classic::objects(still, water, sea_bed_draws, lying, 0, 0);
        OA_CHECK(still.vertices[0].x == 120 && still.vertices[0].y == 96);
        OA_CHECK(!renderer_water_classic::on_sea_bed(water, sea_bed_draws, lying));
        // An upright feature deep below moves at its foot; its top, out of
        // the water, stays.
        sea_bed_draws.sprites[0].terrain_decal = false;
        sea_bed_draws.sprites[0].world_position->y = 4 * 65536;
        auto upright = quad(110);
        const auto standing = upright.vertices;
        renderer_water_classic::objects(upright, water, sea_bed_draws, lying, 0, 0);
        OA_CHECK(upright.vertices[0].x == standing[0].x && upright.vertices[0].y == 110);
        const auto foot = art.at(standing[2].x, standing[2].y);
        OA_CHECK(std::abs(upright.vertices[2].x - standing[2].x - foot[0]) < 1.0e-4F);
        OA_CHECK(!renderer_water_classic::on_sea_bed(water, sea_bed_draws, lying));
        // Light's halos stay at their event.
        const WorldDraw lit{WorldDrawKind::lit_sprite, 0};
        sea_bed_draws.sprites[0].terrain_decal = true;
        auto halo = quad(96);
        renderer_water_classic::objects(halo, water, sea_bed_draws, lit, 0, 0);
        OA_CHECK(halo.vertices[0].x == 120 && halo.vertices[0].y == 96);
        OA_CHECK(!renderer_water_classic::on_sea_bed(water, sea_bed_draws, lit));
        // Shadows on the sea bed lie under the surface too.
        sea_bed_draws.sprites[0].terrain_decal = false;
        sea_bed_draws.sprites[0].shadow = true;
        OA_CHECK(renderer_water_classic::on_sea_bed(water, sea_bed_draws, lying));
        // The surface draws only with its effects on.
        OA_CHECK(renderer_water_classic::surface_drawn(water));
        water.ripples = 0;
        OA_CHECK(!renderer_water_classic::surface_drawn(water));
        water.coast = 1;
        OA_CHECK(renderer_water_classic::surface_drawn(water));
        water.view.zoom = 0.25F;
        OA_CHECK(!renderer_water_classic::surface_drawn(water));
    }
    // Coastal waves' foam shows plainly at 100%, as a gentle wash at 30%,
    // stronger at 200% up to its cap, and fades out as the view zooms out.
    {
        oa::formats::tnt::Map beach;
        beach.attribute_width = beach.attribute_height = 32;
        beach.sea_level = 32;
        beach.attributes.resize(32 * 32);
        for (auto& attribute : beach.attributes)
            attribute.height = 30;
        renderer_water_classic::Inputs waves;
        waves.map = &beach;
        waves.view.width = 320;
        waves.view.height = 240;
        waves.wind.strength = 1;
        // A crest over water 2 map pixels deep.
        waves.seconds = 5.2117;
        waves.visible = [](float, float) { return true; };
        const auto strongest = [&](float coast, float zoom) {
            waves.coast = coast;
            waves.view.zoom = zoom;
            card::CardFrame foam;
            renderer_water_classic::surface(foam, waves);
            OA_CHECK(card::check_frame(foam).empty());
            float alpha = 0;
            for (const auto& vertex : foam.vertices)
                alpha = std::max(alpha, vertex.colour.alpha);
            return alpha;
        };
        const float gentle = strongest(0.3F, 1), plain = strongest(1, 1), strong = strongest(2, 1);
        OA_CHECK(gentle > 0.1F && gentle < 0.2F);
        OA_CHECK(plain > 0.4F && std::abs(plain - gentle / 0.3F) < 1.0e-3F);
        OA_CHECK(strong > plain && strong <= renderer_water_appearance::foam_cap);
        OA_CHECK(std::abs(strongest(1, 0.375F) - plain * 0.5F) < 1.0e-3F);
        OA_CHECK(strongest(1, 0.25F) == 0);
        OA_CHECK(strongest(0, 1) == 0);
        // Gentler on the most sheltered water, never more than on the open sea.
        OA_CHECK(
            renderer_water_surface::foam_alpha(1, 1, 0, 1) <
            renderer_water_surface::foam_alpha(1, 1, 1, 1)
        );
        OA_CHECK(renderer_water_surface::foam_cover(0, 1) == 0);
        OA_CHECK(renderer_water_surface::foam_cover(renderer_water_surface::foam_depth, 1) == 0);
        // The crests run in to the shore: a crest's depth lessens with time.
        const auto crest_depth = [](float seconds) {
            float deepest = 0, best = 0;
            for (float depth = 6; depth < 14; depth += 0.01F) {
                const float cover = renderer_water_surface::foam_cover(depth, seconds);
                if (cover > best) {
                    best = cover;
                    deepest = depth;
                }
            }
            return deepest;
        };
        OA_CHECK(crest_depth(0.5F) < crest_depth(0));
    }
    // Along a shore that runs across the cells, the foam reaches every
    // square to the waterline and changes evenly from cell to cell: no steps
    // of whole squares or cells by the beach.
    {
        oa::formats::tnt::Map diagonal;
        diagonal.attribute_width = diagonal.attribute_height = 32;
        diagonal.sea_level = 32;
        diagonal.attributes.resize(32 * 32);
        // Water east of the diagonal, 3 map pixels deeper a cell each way.
        for (int row = 0; row < 32; ++row)
            for (int column = 0; column < 32; ++column)
                diagonal.attributes[std::size_t(row) * 32 + std::size_t(column)].height =
                    uint8_t(std::clamp(32 + (row - column) * 3, 0, 64));
        renderer_water::SurfaceCache fetches;
        fetches.reset(diagonal);
        renderer_water_classic::Inputs beach_waves;
        beach_waves.map = &diagonal;
        beach_waves.view.width = 320;
        beach_waves.view.height = 240;
        beach_waves.coast = 1;
        beach_waves.wind.strength = 1;
        beach_waves.seconds = 5.2117;
        beach_waves.water_surface = &fetches;
        beach_waves.visible = [](float, float) { return true; };
        card::CardFrame foam;
        renderer_water_classic::surface(foam, beach_waves);
        OA_CHECK(card::check_frame(foam).empty() && !foam.indices.empty());
        // At zoom 1 a corner at map pixel x of the water plane's row z lands
        // at x, z less half the sea; the waterline runs where x is z.
        std::map<std::pair<int, int>, float> alphas;
        std::map<int, int> nearest_shore;
        bool even = true;
        for (const auto index : foam.indices) {
            const auto& vertex = foam.vertices[index];
            const std::pair<int, int> place{
                int(std::lround(vertex.x)), int(std::lround(vertex.y + diagonal.sea_level * 0.5F))
            };
            const auto [kept, added] = alphas.emplace(place, vertex.colour.alpha);
            even = even && (added || std::abs(kept->second - vertex.colour.alpha) < 1.0e-6F);
            auto& shore_start = nearest_shore.try_emplace(place.second, place.first).first->second;
            shore_start = std::min(shore_start, place.first);
        }
        OA_CHECK(even);
        // Every row of squares the view shows the shore on starts at it.
        std::size_t rows = 0;
        for (const auto& [z, x] : nearest_shore)
            if (z >= 32 && z <= 200) {
                OA_CHECK(x <= z);
                ++rows;
            }
        OA_CHECK(rows >= 40);
        // Each corner is drawn by every cell it bounds at the same alpha,
        // with the water's fetch blended across the coarse cells.
        for (float x = 16; x < 496; x += 16) {
            OA_CHECK(
                std::abs(
                    fetches.smooth_fetch(x - 0.01F, 300) - fetches.smooth_fetch(x + 0.01F, 300)
                ) < 0.01F
            );
        }
        // No hard outer edge: among blocks of deep sea and high land, every
        // corner the foam reaches is drawn with its foam, however deep or
        // high the cells around it lie, so the foam thins out to nothing
        // within what is drawn.
        oa::formats::tnt::Map blocks;
        blocks.attribute_width = blocks.attribute_height = 32;
        blocks.sea_level = 32;
        blocks.attributes.resize(32 * 32);
        for (int row = 0; row < 32; ++row)
            for (int column = 0; column < 32; ++column)
                blocks.attributes[std::size_t(row) * 32 + std::size_t(column)].height =
                    (column / 3 + row / 3) % 2 == 0 ? 64 : 0;
        beach_waves.map = &blocks;
        beach_waves.water_surface = nullptr;
        card::CardFrame block_foam;
        renderer_water_classic::surface(block_foam, beach_waves);
        std::map<std::pair<int, int>, float> drawn;
        for (const auto index : block_foam.indices) {
            const auto& vertex = block_foam.vertices[index];
            drawn[{
                int(std::lround(vertex.x)), int(std::lround(vertex.y + blocks.sea_level * 0.5F))
            }] = vertex.colour.alpha;
        }
        // The depth blended between the cells' centres, as the foam takes it.
        const auto blended = [&](float x, float z) {
            const float across = x / 16 - 0.5F, down = z / 16 - 0.5F;
            const int column = int(std::floor(across)), row = int(std::floor(down));
            const auto at = [&](int c, int r) {
                return float(blocks.sea_level) -
                       float(blocks
                                 .attributes
                                     [std::size_t(std::clamp(r, 0, 31)) * 32 +
                                      std::size_t(std::clamp(c, 0, 31))]
                                 .height);
            };
            const float tx = across - float(column), tz = down - float(row);
            const float upper = at(column, row) + (at(column + 1, row) - at(column, row)) * tx;
            const float lower =
                at(column, row + 1) + (at(column + 1, row + 1) - at(column, row + 1)) * tx;
            return upper + (lower - upper) * tz;
        };
        // The wash's share at a depth, which the crests raise from 0.3 to 1 of.
        const auto wash = [](float depth) {
            const auto smooth = [](float from, float to, float value) {
                const float share = std::clamp((value - from) / (to - from), 0.0F, 1.0F);
                return share * share * (3 - 2 * share);
            };
            return depth > 0 && depth < renderer_water_surface::foam_depth
                       ? (1 - smooth(3, renderer_water_surface::foam_depth, depth)) *
                             smooth(0, 1, depth)
                       : 0.0F;
        };
        std::size_t reached = 0;
        for (int z = 32; z <= 224; z += 4)
            for (int x = 16; x <= 300; x += 4) {
                const float shore = wash(blended(float(x), float(z)));
                const float least = renderer_water_surface::foam_alpha(shore * 0.3F, 1, 0.25F, 1);
                const float most = renderer_water_surface::foam_alpha(shore, 1, 0.25F, 1);
                if (least < 0.01F)
                    continue;
                ++reached;
                const auto found = drawn.find({x, z});
                OA_CHECK(
                    found != drawn.end() && found->second >= least - 1.0e-3F &&
                    found->second <= most + 1.0e-3F
                );
            }
        OA_CHECK(reached > 100);
    }
    // The coast's foam lies on every shallow cell the view shows, at any
    // zoom: a wide view far out draws as many cells' foam as it shows, not
    // one cell in a few.
    {
        oa::formats::tnt::Map shallows;
        shallows.attribute_width = shallows.attribute_height = 512;
        shallows.sea_level = 32;
        shallows.attributes.resize(512 * 512);
        // Shores two cells wide every sixteen across a deep sea.
        for (std::size_t cell = 0; cell < shallows.attributes.size(); ++cell)
            shallows.attributes[cell].height = cell % 16 < 2 ? 24 : 0;
        renderer_water_classic::Inputs shore;
        shore.map = &shallows;
        shore.view.width = 1920;
        shore.view.height = 1080;
        shore.coast = 1;
        shore.wind.strength = 1;
        // A crest over the whole of water 8 pixels deep.
        shore.seconds = 1.61;
        shore.visible = [](float, float) { return true; };
        for (const float zoom : {1.0F, 0.5F, 0.3F}) {
            shore.view.zoom = zoom;
            card::CardFrame foam;
            renderer_water_classic::surface(foam, shore);
            const auto shown = [&](float extent) { return std::floor(extent / zoom / 16); };
            OA_CHECK(
                foam.vertices.size() / 4 >=
                std::size_t(std::floor(shown(1920) / 16) * 2 * shown(1080))
            );
            OA_CHECK(card::check_frame(foam).empty());
        }
        shore.coast = 0;
        card::CardFrame none;
        renderer_water_classic::surface(none, shore);
        OA_CHECK(none.vertices.empty());
    }
    // The foam ends at the edges of the map's picture the game shows, as
    // the terrain does, though the map's heights run on past the bottom and
    // right edges: a view past those edges, and past the top and left
    // ones, draws foam up to each edge and none beyond it.
    {
        oa::formats::tnt::Map islands;
        islands.attribute_width = islands.attribute_height = 40;
        islands.sea_level = 24;
        islands.attributes.resize(40 * 40);
        // Islands three cells square every six, in a sea 24 deep.
        for (uint32_t z = 0; z < 40; ++z)
            for (uint32_t x = 0; x < 40; ++x)
                islands.attributes[z * 40 + x].height = x % 6 < 3 && z % 6 < 3 ? 40 : 0;
        renderer_water_classic::Inputs edge;
        edge.map = &islands;
        edge.view.width = 800;
        edge.view.height = 720;
        edge.view.zoom = 1;
        edge.view.camera_x = -40;
        edge.view.camera_y = -60;
        edge.coast = 1;
        edge.wind.strength = 1;
        edge.seconds = 1.61;
        edge.visible = [](float, float) { return true; };
        // The map's picture is 640 across and down, of which the game shows
        // all but the last 32 across and 128 down.
        constexpr std::array<float, 2> shown{608, 512};
        const auto picture = [&](const card::CardFrame& frame, const char* name) {
            const int32_t width = edge.view.width, height = edge.view.height;
            std::vector<uint8_t> rgb(std::size_t(width) * std::size_t(height) * 3);
            // The terrain where the game draws it, sea blue and land sand,
            // and black past the map's edges.
            for (int32_t y = 0; y < height; ++y)
                for (int32_t x = 0; x < width; ++x) {
                    const float mx = float(x + edge.view.camera_x);
                    const float my = float(y + edge.view.camera_y);
                    if (mx < 0 || my < 0 || mx >= shown[0] || my >= shown[1])
                        continue;
                    const auto cell =
                        islands.attributes[std::size_t((my + 12) / 16) * 40 + std::size_t(mx / 16)];
                    const std::array<uint8_t, 3> ground =
                        cell.height > 24 ? std::array<uint8_t, 3>{214, 200, 160}
                                         : std::array<uint8_t, 3>{30, 80, 140};
                    std::copy(
                        ground.begin(),
                        ground.end(),
                        rgb.begin() + (std::ptrdiff_t(y) * width + x) * 3
                    );
                }
            draw_frame(frame, rgb, width, height);
            if (pictures.empty())
                return;
            std::filesystem::create_directories(pictures);
            std::ofstream out(pictures / name, std::ios::binary);
            out << "P6\n" << width << ' ' << height << "\n255\n";
            out.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
        };
        card::CardFrame unclipped;
        renderer_water_classic::surface(unclipped, edge);
        picture(unclipped, "classic-edge-before.ppm");
        // Without the shown size the view's edges alone bound it, and the
        // islands past the map's bottom and right edges have foam.
        OA_CHECK(!unclipped.batches.empty());
        if (!unclipped.batches.empty()) {
            const auto& scissor = unclipped.batches.back().scissor;
            OA_CHECK(scissor.x == 0 && scissor.y == 0 && scissor.width == 800);
            OA_CHECK(scissor.height == 720);
        }
        const bool past_bottom = std::any_of(
            unclipped.vertices.begin(), unclipped.vertices.end(), [](const card::Vertex& vertex) {
                return vertex.colour.alpha > 0.01F && vertex.y > 60 + 512 + 8;
            }
        );
        const bool past_right = std::any_of(
            unclipped.vertices.begin(), unclipped.vertices.end(), [](const card::Vertex& vertex) {
                return vertex.colour.alpha > 0.01F && vertex.x > 40 + 608 + 8;
            }
        );
        OA_CHECK(past_bottom && past_right);
        edge.shown = shown;
        card::CardFrame clipped;
        renderer_water_classic::surface(clipped, edge);
        picture(clipped, "classic-edge-after.ppm");
        // The same foam, its batch held to the map's picture on screen: from
        // its top left, at 40 and 60 in the view, to 608 and 512 on.
        OA_CHECK(clipped.vertices.size() == unclipped.vertices.size());
        OA_CHECK(!clipped.batches.empty());
        if (!clipped.batches.empty()) {
            const auto& batch = clipped.batches.back();
            OA_CHECK(batch.scissored);
            OA_CHECK(batch.scissor.x == 40 && batch.scissor.y == 60);
            OA_CHECK(batch.scissor.width == 608 && batch.scissor.height == 512);
        }
        OA_CHECK(card::check_frame(clipped).empty());
        // A view wholly past the map's edge draws nothing.
        edge.view.camera_x = 700;
        card::CardFrame beyond;
        renderer_water_classic::surface(beyond, edge);
        OA_CHECK(beyond.batches.empty());
    }
    return oa::test::check_exit_status();
}
