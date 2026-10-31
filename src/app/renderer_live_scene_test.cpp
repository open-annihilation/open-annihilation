// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Live terrain preserves painted coordinates, visibility and bounded atlas reuse.
#include "renderer_live_scene.hpp"
#include "renderer_water_surface.hpp"
#include "renderer_aircraft_shadow.hpp"
#include "renderer_water_appearance.hpp"
#include "oa/test/check.hpp"
#include "oa/present/rle.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/sim/unit_movement/terrain.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
namespace live = oa::app::renderer_live_scene;
constexpr int32_t tile_edge = 32;
constexpr int32_t cell_edge = 16;
constexpr uint8_t ground_index = 1;
constexpr uint8_t alternate_index = 2;
constexpr uint16_t viewer_bit = 1;
constexpr std::array<uint8_t, 3> ground_color{120, 60, 30};
constexpr std::array<uint8_t, 3> alternate_color{30, 120, 240};
constexpr float tolerance = 0.001F;

/// A flat tile with two distinguishable painted regions.
struct Fixture {
    oa::formats::tnt::Map map{};
    oa::PaletteBytes palette{};
    oa::app::WorldDrawList draws{};
    std::array<uint16_t, 1> coverage{1};
    std::array<uint16_t, 1> mapped{viewer_bit};
    live::Inputs inputs{};
    live::Scene scene{};
    std::string error{};

    Fixture() {
        map.attribute_width = 2;
        map.attribute_height = 2;
        map.tile_width = 1;
        map.tile_height = 1;
        map.tile_count = 1;
        map.tile_indices = {0};
        map.attributes.resize(4);
        map.attributes[0].height = 0;
        map.attributes[1].height = 0;
        map.attributes[2].height = 0;
        map.attributes[3].height = 0;
        map.tile_palette_indices.resize(tile_edge * tile_edge);
        for (int32_t y = 0; y < tile_edge; ++y)
            for (int32_t x = 0; x < tile_edge; ++x)
                map.tile_palette_indices[y * tile_edge + x] =
                    x < cell_edge ? ground_index : alternate_index;
        for (std::size_t channel = 0; channel < ground_color.size(); ++channel) {
            palette[ground_index * oa::palette_entry_bytes + channel] = ground_color[channel];
            palette[alternate_index * oa::palette_entry_bytes + channel] = alternate_color[channel];
        }
        inputs.map = &map;
        inputs.palette = &palette;
        inputs.draws = &draws;
        inputs.sight.coverage = coverage;
        inputs.sight.player_bits = mapped;
        inputs.sight.width = 1;
        inputs.sight.height = 1;
        inputs.sight.mapped_bits = viewer_bit;
        inputs.sight.line_of_sight = true;
        inputs.sight.mapping = true;
        inputs.right = tile_edge;
        inputs.bottom = tile_edge;
    }
};

/// Samples a triangle's interpolated centre from its material atlas.
std::array<uint8_t, 4> triangle_color(const live::Scene& scene, std::size_t first) {
    const auto& a = scene.vertices.at(first);
    const auto& b = scene.vertices.at(first + 1);
    const auto& c = scene.vertices.at(first + 2);
    const auto x = static_cast<uint32_t>((a.u + b.u + c.u) * scene.texture_width / 3);
    const auto y = static_cast<uint32_t>((a.v + b.v + c.v) * scene.texture_height / 3);
    const auto offset = (static_cast<std::size_t>(y) * scene.texture_width + x) * 4;
    const float brightness = a.terrain_size > 0
                                 ? (a.terrain_known + b.terrain_known + c.terrain_known) / 3
                                 : (a.brightness + b.brightness + c.brightness) / 3;
    const float grey = (a.terrain_grey + b.terrain_grey + c.terrain_grey) / 3;
    std::array<uint8_t, 4> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        float value = scene.texture_rgba.at(offset + channel);
        if (a.terrain_size > 0)
            value = std::lerp(
                value, float(scene.texture_rgba.at(offset + tile_edge * 4 + channel)), grey
            );
        const std::array modulation{
            (a.red + b.red + c.red) / 3,
            (a.green + b.green + c.green) / 3,
            (a.blue + b.blue + c.blue) / 3
        };
        result[channel] =
            static_cast<uint8_t>(std::lround(value * brightness * modulation[channel]));
    }
    result[3] = scene.texture_rgba.at(offset + 3);
    return result;
}

/// Flat terrain keeps classic pixel placement and painted UVs.
void painted_projection() {
    Fixture fixture;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.error.empty());
    OA_CHECK(fixture.scene.vertices.size() == 24);
    OA_CHECK(fixture.scene.terrain_vertex_count == fixture.scene.vertices.size());
    OA_CHECK(fixture.scene.model_count == 0);
    OA_CHECK(fixture.scene.sprite_count == 0);
    constexpr std::array<std::array<int32_t, 2>, 6> corners{
        {{0, 0}, {16, 0}, {16, 16}, {0, 0}, {16, 16}, {0, 16}}
    };
    for (std::size_t cell = 0; cell < 4; ++cell) {
        const auto origin_x = static_cast<int32_t>(cell % 2) * cell_edge;
        const auto origin_y = static_cast<int32_t>(cell / 2) * cell_edge;
        for (std::size_t corner = 0; corner < corners.size(); ++corner) {
            const auto& vertex = fixture.scene.vertices.at(cell * 6 + corner);
            const float painted_x = static_cast<float>(origin_x + corners[corner][0]);
            const float painted_y = static_cast<float>(origin_y + corners[corner][1]);
            OA_CHECK(std::abs(vertex.x - painted_x) < tolerance);
            OA_CHECK(std::abs(vertex.z - vertex.height * 0.5F - painted_y) < tolerance);
        }
        const auto sampled = triangle_color(fixture.scene, cell * 6);
        const auto& expected = cell % 2 == 0 ? ground_color : alternate_color;
        for (std::size_t channel = 0; channel < expected.size(); ++channel)
            OA_CHECK(sampled[channel] == expected[channel]);
        OA_CHECK(sampled[3] == 255);
    }
}

/// Keep a steep north/south ridge on the simulation grid and sample projected artwork.
void canonical_terrain_grid() {
    Fixture fixture;
    constexpr std::array<uint8_t, 12> heights{0, 0, 32, 96, 128, 128, 96, 16, 0, 0, 0, 0};
    fixture.map.attribute_width = 8;
    fixture.map.attribute_height = static_cast<uint32_t>(heights.size());
    fixture.map.tile_width = 4;
    fixture.map.tile_height = 6;
    fixture.map.tile_indices.assign(24, 0);
    fixture.map.attributes.resize(8 * heights.size());
    for (std::size_t row = 0; row < heights.size(); ++row)
        for (std::size_t column = 0; column < 8; ++column)
            fixture.map.attributes[row * 8 + column].height = heights[row];
    fixture.inputs.right = 128;
    fixture.inputs.bottom = 192;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    for (int row = 0; row < tile_edge; ++row)
        for (int column = 0; column < tile_edge; ++column)
            fixture.map.tile_palette_indices[row * tile_edge + column] =
                row < cell_edge ? ground_index : alternate_index;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    bool found_cliff_top{}, found_cliff_bottom{};
    for (const auto& vertex : fixture.scene.vertices) {
        const auto row = std::clamp(static_cast<int>(vertex.z / cell_edge), 0, 11);
        const float fraction = (vertex.z - row * cell_edge) / cell_edge;
        const float expected =
            std::lerp(float(heights[row]), float(heights[std::min(row + 1, 11)]), fraction);
        OA_CHECK(std::abs(vertex.height - expected) < tolerance);
        found_cliff_top |=
            std::abs(vertex.z - 96) < tolerance && std::abs(vertex.height - 96) < tolerance;
        found_cliff_bottom |=
            std::abs(vertex.z - 112) < tolerance && std::abs(vertex.height - 16) < tolerance;
    }
    OA_CHECK(found_cliff_top && found_cliff_bottom);
    for (std::size_t first = 0; first < fixture.scene.terrain_vertex_count; first += 3) {
        const auto& a = fixture.scene.vertices[first];
        const auto& b = fixture.scene.vertices[first + 1];
        const auto& c = fixture.scene.vertices[first + 2];
        const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
        OA_CHECK(area > 0);
        const float painted_y = (a.z + b.z + c.z - (a.height + b.height + c.height) * 0.5F) / 3;
        const auto row = static_cast<int>(std::floor(painted_y)) % tile_edge;
        const auto expected = row < cell_edge ? ground_color : alternate_color;
        const auto actual = triangle_color(fixture.scene, first);
        for (std::size_t channel = 0; channel < 3; ++channel)
            OA_CHECK(actual[channel] == expected[channel]);
    }
    // A surface feature on the ridge must reach the canonical plateau, without
    // moving its world anchor south by half the plateau's height.
    oa::formats::gaf::RenderedFrame geothermal{};
    geothermal.width = geothermal.height = 8;
    geothermal.pixels.assign(64, ground_index);
    geothermal.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&geothermal, {48, 12}, false, true, true});
    fixture.draws.sprites.back().world_position =
        oa::FixedVec3{48 * 65536, 128 * 65536, 76 * 65536};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    bool plateau_decal{};
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        if (std::abs(vertex.height - 128.05F) < tolerance) {
            plateau_decal = true;
            OA_CHECK(vertex.z >= 76 - tolerance && vertex.z <= 84 + tolerance);
        }
    }
    OA_CHECK(plateau_decal);
}

/// Returns the drawn terrain's height at a world point: the terrain triangle
/// under it, interpolated, or nothing where none is.
std::optional<float> drawn_ground(const live::Scene& scene, float x, float z) {
    for (std::size_t first = 0; first + 2 < scene.terrain_vertex_count; first += 3) {
        const auto& a = scene.vertices[first];
        const auto& b = scene.vertices[first + 1];
        const auto& c = scene.vertices[first + 2];
        const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
        if (std::abs(area) < tolerance)
            continue;
        const float wb = ((x - a.x) * (c.z - a.z) - (c.x - a.x) * (z - a.z)) / area;
        const float wc = ((b.x - a.x) * (z - a.z) - (x - a.x) * (b.z - a.z)) / area;
        const float wa = 1 - wb - wc;
        if (wa < -tolerance || wb < -tolerance || wc < -tolerance)
            continue;
        return wa * a.height + wb * b.height + wc * c.height;
    }
    return std::nullopt;
}

/// A height cell whose corners lie off one plane is drawn about its middle at
/// the simulation's height there, so that the ground drawn stays within a
/// sixteenth of the cell's twist of the ground units stand on; across one
/// diagonal it was a quarter of it away at the middle. Decals drape over the
/// same triangles, and a cell in one plane keeps its two.
void twisted_terrain_follows_simulation() {
    Fixture fixture;
    fixture.map.attribute_width = 4;
    fixture.map.attribute_height = 4;
    fixture.map.tile_width = 2;
    fixture.map.tile_height = 2;
    fixture.map.tile_indices.assign(4, 0);
    fixture.inputs.right = fixture.inputs.bottom = 2 * tile_edge;
    // The first cell of the second row is a saddle, high at its top left and
    // bottom right; the one right of it lies in one plane, rising south.
    constexpr std::array<uint8_t, 16> heights{
        0, 0, 0, 0, 20, 0, 0, 0, 0, 20, 20, 20, 0, 20, 20, 20
    };
    fixture.map.attributes.assign(heights.size(), {});
    for (std::size_t i = 0; i < heights.size(); ++i)
        fixture.map.attributes[i].height = heights[i];
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const oa::sim::unit_movement::Terrain terrain(fixture.map);
    constexpr float twist = 40;
    float worst = 0;
    for (int32_t z = cell_edge; z <= 2 * cell_edge; z += 2)
        for (int32_t x = 0; x <= cell_edge; x += 2) {
            const auto drawn = drawn_ground(fixture.scene, float(x), float(z));
            OA_CHECK(drawn.has_value());
            if (!drawn)
                continue;
            const float ground = float(terrain.height(x * 65536, z * 65536));
            worst = std::max(worst, std::abs(*drawn - ground));
        }
    // A sixteenth of the twist, and the simulation's whole steps.
    OA_CHECK(worst <= twist / 16 + 1);
    const auto middle = drawn_ground(fixture.scene, 8, 24);
    OA_CHECK(
        middle && std::abs(*middle - float(terrain.height(8 * 65536, 24 * 65536))) < tolerance
    );
    // The cell right of it lies in one plane: two triangles; the saddle four.
    const auto triangles_in = [&](float left, float top) {
        std::size_t count = 0;
        for (std::size_t first = 0; first < fixture.scene.terrain_vertex_count; first += 3) {
            bool inside = true;
            for (std::size_t k = 0; k < 3; ++k) {
                const auto& vertex = fixture.scene.vertices[first + k];
                inside = inside && vertex.x >= left - tolerance &&
                         vertex.x <= left + cell_edge + tolerance && vertex.z >= top - tolerance &&
                         vertex.z <= top + cell_edge + tolerance;
            }
            count += inside ? 1 : 0;
        }
        return count;
    };
    OA_CHECK(triangles_in(0, cell_edge) == 4);
    OA_CHECK(triangles_in(cell_edge, cell_edge) == 2);
    // A decal over the saddle lies on the ground drawn under it.
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 12;
    frame.pixels.assign(144, ground_index);
    frame.coverage.assign(144, 1);
    fixture.draws.sprites.push_back({&frame, {2, 13}, false, true, true});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    std::size_t decal_points = 0;
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        const auto under = drawn_ground(fixture.scene, vertex.x, vertex.z);
        OA_CHECK(under && std::abs(vertex.height - *under - 0.05F) < 0.01F);
        ++decal_points;
    }
    OA_CHECK(decal_points > 0);
}

/// Animated textures show the frames the classic tiers draw: the first on a
/// piece the unit's image holds, whatever the unit does; the running one on
/// a moving piece of a unit that is not idle, and on a unit drawn without an
/// image.
void animated_frames_follow_classic_tiers() {
    Fixture fixture;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 0, 0}, {fixed, 0, 0}, {fixed, fixed, 0}, {0, fixed, 0}};
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2, 3};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    std::array<uint8_t, 2> pixels{ground_index, alternate_index};
    drawing::TextureSequence texture;
    texture.frames.resize(2);
    for (std::size_t i = 0; i < texture.frames.size(); ++i) {
        texture.frames[i].width = texture.frames[i].height = 1;
        texture.frames[i].encoding = OA_SPRITE_RAW;
        texture.frames[i].data = &pixels[i];
    }
    const oa::formats::gaf::Sequence sequence{};
    drawing::PreparedModel prepared{};
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(1);
    auto& primitive = prepared.objects[0].primitives[0];
    primitive.flags = drawing::primitive_animated;
    primitive.texture = &texture;
    primitive.cursor.sequence = &sequence;
    primitive.cursor.frame_index = 1;
    std::array<uint8_t, 1> image{ground_index};
    drawing::ModelState state{};
    state.image.sprite.data = image.data();
    oa::Unit unit{};
    unit.id = 7;
    unit.position = {8 * fixed, 0, 8 * fixed};
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.instance = &instance;
    draw.model.prepared = &prepared;
    draw.model.state = &state;
    draw.model.unit = &unit;
    draw.plan.linked.drawn = true;
    draw.plan.model.from_image = true;
    const auto shown = [&] {
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(!fixture.scene.unit_ranges.empty());
        if (fixture.scene.unit_ranges.empty())
            return std::array<uint8_t, 4>{};
        return triangle_color(fixture.scene, fixture.scene.unit_ranges[0].first_vertex);
    };
    // A walking unit's cached piece keeps the first frame.
    auto& piece = instance.pieces()[0];
    const auto cached = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    OA_CHECK((piece.flags & cached) != 0);
    OA_CHECK(shown()[0] == ground_color[0]);
    // Its moving piece shows the running frame, unless the unit is idle.
    piece.flags = static_cast<uint16_t>(piece.flags & ~cached);
    OA_CHECK(shown()[0] == alternate_color[0]);
    draw.plan.linked.unlit = true;
    OA_CHECK(shown()[0] == ground_color[0]);
    // Over an image with a depth plane a moving piece always runs.
    state.image.sprite.aux = image.data();
    OA_CHECK(shown()[0] == alternate_color[0]);
    // Without an image every piece runs unless the unit is idle.
    piece.flags = static_cast<uint16_t>(piece.flags | cached);
    draw.plan.model.from_image = false;
    OA_CHECK(shown()[0] == ground_color[0]);
    draw.plan.linked.unlit = false;
    OA_CHECK(shown()[0] == alternate_color[0]);
}

/// Keep clipped atlas coordinates valid at the map edge, including fractional intersections.
void clipped_texture_bounds() {
    Fixture fixture;
    fixture.map.attributes[0].height = 16;
    fixture.map.attributes[1].height = 16;
    fixture.map.attributes[2].height = 16;
    fixture.map.attributes[3].height = 17;
    fixture.inputs.detail_scale = 0.125F;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.vertices.empty());
    bool map_edge{};
    for (const auto& vertex : fixture.scene.vertices) {
        OA_CHECK(vertex.u >= 0 && vertex.u <= 1);
        OA_CHECK(vertex.v >= 0 && vertex.v <= 1);
        if (std::abs(vertex.z - vertex.height * 0.5F) < tolerance)
            map_edge = true;
    }
    OA_CHECK(map_edge);
}

/// Mapping always hides undiscovered pixels, including when current coverage is present.
void fog_materials() {
    Fixture fixture;
    constexpr std::size_t grey_index = 3;
    std::fill_n(fixture.palette.begin() + grey_index * oa::palette_entry_bytes, 3, 70);
    fixture.mapped[0] = 0;
    fixture.coverage[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const std::array<uint8_t, 4> black{0, 0, 0, 255};
    OA_CHECK(triangle_color(fixture.scene, 0) == black);
    fixture.coverage[0] = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, 0) == black);
    fixture.mapped[0] = viewer_bit;
    fixture.coverage[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const std::array<uint8_t, 4> grey{70, 70, 70, 255};
    OA_CHECK(triangle_color(fixture.scene, 0) == grey);
    // Player presentation uses the whole count, even at the byte wrap boundary.
    fixture.coverage[0] = 256;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const std::array<uint8_t, 4> original{120, 60, 30, 255};
    OA_CHECK(triangle_color(fixture.scene, 0) == original);
    fixture.inputs.sight.mapping = false;
    fixture.inputs.sight.line_of_sight = false;
    fixture.mapped[0] = 0;
    fixture.coverage[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, 0) == original);
}

/// Terrain mipmaps retain the exact average of their original palette colours.
void terrain_mip_colours() {
    Fixture fixture;
    for (int y = 0; y < tile_edge; ++y)
        for (int x = 0; x < tile_edge; ++x)
            fixture.map.tile_palette_indices[y * tile_edge + x] =
                (x + y) % 2 == 0 ? ground_index : alternate_index;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto& vertex = fixture.scene.vertices.front();
    const auto x = static_cast<uint32_t>(vertex.terrain_x);
    const auto y = static_cast<uint32_t>(vertex.terrain_y);
    const auto offset =
        (static_cast<std::size_t>(y + tile_edge) * fixture.scene.texture_width + x) * 4;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto expected = (uint32_t(ground_color[channel]) + alternate_color[channel] + 1) / 2;
        OA_CHECK(fixture.scene.texture_rgba[offset + channel] == expected);
    }
}

/// The first palette index of the tiles of wide_terrain, and how many
/// colours they cycle through.
constexpr uint8_t wide_first_colour = 1;
constexpr uint32_t wide_colours = 250;

/// Lays a flat map of across by down distinct tiles, each one colour, under
/// a view of all of it whose depth grows down the map, so that the bottom
/// rows are nearest.
void wide_terrain(Fixture& fixture, uint32_t across, uint32_t down) {
    auto& map = fixture.map;
    map.tile_width = across;
    map.tile_height = down;
    map.attribute_width = across * 2;
    map.attribute_height = down * 2;
    map.tile_count = across * down;
    map.tile_indices.resize(static_cast<std::size_t>(across) * down);
    map.tile_palette_indices.resize(map.tile_indices.size() * tile_edge * tile_edge);
    for (std::size_t tile = 0; tile < map.tile_indices.size(); ++tile) {
        map.tile_indices[tile] = static_cast<uint16_t>(tile);
        std::fill_n(
            map.tile_palette_indices.begin() +
                static_cast<std::ptrdiff_t>(tile * tile_edge * tile_edge),
            tile_edge * tile_edge,
            static_cast<uint8_t>(wide_first_colour + tile % wide_colours)
        );
    }
    map.attributes.assign(static_cast<std::size_t>(map.attribute_width) * map.attribute_height, {});
    for (uint32_t index = 0; index < wide_colours; ++index) {
        const auto entry = (wide_first_colour + index) * oa::palette_entry_bytes;
        fixture.palette[entry] = static_cast<uint8_t>(index);
        fixture.palette[entry + 1] = static_cast<uint8_t>(255 - index);
        fixture.palette[entry + 2] = static_cast<uint8_t>(index / 2 + 10);
    }
    fixture.inputs.sight.mapping = false;
    fixture.inputs.sight.line_of_sight = false;
    fixture.inputs.right = static_cast<int32_t>(across) * tile_edge;
    fixture.inputs.bottom = static_cast<int32_t>(down) * tile_edge;
    fixture.inputs.clip_projection = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
}

/// A wide, low view of a map with more distinct tiles than the atlas holds
/// in full squares: the frame builds, the tiles nearest the camera in full
/// squares and the rest smaller, each sampled as its own colour; a view
/// whose tiles fit keeps them all in full squares.
void terrain_wider_than_atlas() {
    constexpr uint8_t first_colour = wide_first_colour;
    constexpr uint32_t colours = wide_colours;
    const auto check_view =
        [&](uint32_t across, uint32_t down, float expected_largest, float expected_smallest) {
            Fixture fixture;
            wide_terrain(fixture, across, down);
            OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
            OA_CHECK(fixture.error.empty());
            OA_CHECK(!fixture.scene.out_of_room);
            OA_CHECK(fixture.scene.terrain_vertex_count != 0);
            float largest = 0;
            float smallest = 64;
            float nearest_small = -1;
            float furthest_large = std::numeric_limits<float>::infinity();
            for (std::size_t first = 0; first + 2 < fixture.scene.terrain_vertex_count;
                 first += 3) {
                const auto& vertex = fixture.scene.vertices[first];
                const float size = vertex.terrain_size;
                OA_CHECK(size == 32 || size == 16 || size == 8 || size == 4);
                OA_CHECK(
                    vertex.terrain_x + 2 * size <= fixture.scene.texture_width &&
                    vertex.terrain_y + 2 * size <= fixture.scene.texture_height
                );
                largest = std::max(largest, size);
                smallest = std::min(smallest, size);
                const float z = (vertex.z + fixture.scene.vertices[first + 1].z +
                                 fixture.scene.vertices[first + 2].z) /
                                3;
                if (size == expected_largest)
                    furthest_large = std::min(furthest_large, z);
                else
                    nearest_small = std::max(nearest_small, z);
                // Every tile is one colour, whatever square it was packed into.
                const auto column = static_cast<uint32_t>(
                                        vertex.x + fixture.scene.vertices[first + 1].x +
                                        fixture.scene.vertices[first + 2].x
                                    ) /
                                    3 / tile_edge;
                const auto row = static_cast<uint32_t>(z) / tile_edge;
                const auto tile = static_cast<std::size_t>(row) * across + column;
                const auto entry = (first_colour + tile % colours) * oa::palette_entry_bytes;
                const auto sampled = triangle_color(fixture.scene, first);
                for (std::size_t channel = 0; channel < 3; ++channel)
                    OA_CHECK(sampled[channel] == fixture.palette[entry + channel]);
            }
            OA_CHECK(largest == expected_largest && smallest == expected_smallest);
            // The larger squares go to the nearest tiles.
            if (expected_largest != expected_smallest)
                OA_CHECK(nearest_small <= furthest_large + tile_edge);
            // The next frame over the same ground finds every tile packed.
            const auto revision = fixture.scene.texture_revision;
            OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
            OA_CHECK(fixture.scene.texture_revision == revision);
        };
    // 6400 tiles: the nearest in full squares and the rest in half ones.
    check_view(80, 80, 32, 16);
    // 16384 tiles, more than half squares hold: half and quarter squares.
    check_view(128, 128, 16, 8);
    // 1600 tiles fit in full squares, and every one is packed so.
    check_view(40, 40, 32, 32);
}

/// A wide view whose terrain and other materials do not fit even an emptied
/// atlas at its largest gives its terrain half the room. The next frame over
/// as many tiles starts in that room, and so finds every tile and material
/// where the last frame packed them: nothing is packed, the atlas is not
/// emptied, and the ground is the one the last frame drew. A frame over a
/// different number of tiles, as after a zoom, plans from the whole room
/// again and draws the ground a new scene draws for its view.
void terrain_room_remembered() {
    // A material as tall as a quarter of the atlas: the rows the terrain's
    // whole room leaves the other materials cannot hold it.
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = 8;
    frame.height = 1024;
    frame.pixels.assign(static_cast<std::size_t>(frame.width) * frame.height, alternate_index);
    frame.coverage.assign(frame.pixels.size(), 1);
    const auto view = [&](Fixture& fixture, uint32_t across, uint32_t down) {
        wide_terrain(fixture, 80, 80);
        fixture.inputs.right = static_cast<int32_t>(across) * tile_edge;
        fixture.inputs.bottom = static_cast<int32_t>(down) * tile_edge;
        fixture.draws.sprites.clear();
        fixture.draws.sprites.push_back({&frame, {8, 8}});
    };
    const auto ground = [](const live::Scene& scene) {
        return std::vector<oa::app::renderer_live_gpu::Vertex>(
            scene.vertices.begin(),
            scene.vertices.begin() + static_cast<std::ptrdiff_t>(scene.terrain_vertex_count)
        );
    };
    const auto same = [](const auto& a, const auto& b) {
        return a.size() == b.size() &&
               std::memcmp(a.data(), b.data(), a.size() * sizeof(a.front())) == 0;
    };
    Fixture fixture;
    view(fixture, 80, 80);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.out_of_room);
    OA_CHECK(fixture.scene.texture_width == 4096);
    const auto first = ground(fixture.scene);
    OA_CHECK(!first.empty());
    // Only the nearest tiles keep full squares: the room was halved.
    const auto full = std::count_if(first.begin(), first.end(), [](const auto& vertex) {
        return vertex.terrain_size == 32;
    });
    OA_CHECK(full != 0 && full < static_cast<std::ptrdiff_t>(first.size()) / 4);
    for (int frames = 0; frames < 2; ++frames) {
        const auto revision = fixture.scene.texture_revision;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.scene.texture_revision == revision);
        OA_CHECK(fixture.scene.atlas_update.first_row == 0);
        OA_CHECK(fixture.scene.atlas_update.end_row == 0);
        OA_CHECK(same(ground(fixture.scene), first));
    }
    // Zoomed in: fewer tiles. The frame plans afresh, empties the atlas and
    // draws what a new scene does; the frame after it keeps all of it.
    view(fixture, 60, 60);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    Fixture fresh;
    view(fresh, 60, 60);
    OA_CHECK(live::build(fresh.inputs, fresh.scene, fresh.error));
    OA_CHECK(same(ground(fixture.scene), ground(fresh.scene)));
    // Emptied for the frame: it writes and uploads the rows a new scene does.
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    OA_CHECK(fixture.scene.atlas_update.end_row == fresh.scene.atlas_update.end_row);
    const auto zoomed = fixture.scene.texture_revision;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision == zoomed);
    OA_CHECK(same(ground(fixture.scene), ground(fresh.scene)));
}

/// While the player's camera moves, a tile keeps its square while that is
/// at most one size smaller than the plan gives it, and a tile new to the
/// view is packed one size smaller than the plan's larger squares, which it
/// keeps however near it comes; once the camera has stood still long
/// enough, every tile takes the plan's square again. A recording's camera
/// packs every tile into the plan's square.
void terrain_held_while_moving() {
    // Each terrain triangle's tile detail.
    const auto sizes = [](const live::Scene& scene) {
        std::vector<float> result;
        for (std::size_t first = 0; first + 2 < scene.terrain_vertex_count; first += 3)
            result.push_back(scene.vertices[first].terrain_size);
        return result;
    };
    // A picture as large as the view, nearest at the map's foot, or at its
    // head; the depth's offset moves the camera without changing the plan.
    const auto view = [](Fixture& fixture, uint32_t columns, bool head, float offset, bool hold) {
        fixture.inputs.right = static_cast<int32_t>(columns) * tile_edge;
        fixture.inputs.clip_width = fixture.inputs.clip_height = 64;
        fixture.inputs.clip_projection[10] = head ? -1.0F : 1.0F;
        fixture.inputs.clip_projection[11] = offset;
        fixture.inputs.tiles_hold_while_moving = hold;
    };
    // The tile detail a new scene draws the view in.
    const auto planned = [&](uint32_t columns, bool head, float offset) {
        Fixture fresh;
        wide_terrain(fresh, 80, 80);
        view(fresh, columns, head, offset, true);
        OA_CHECK(live::build(fresh.inputs, fresh.scene, fresh.error));
        return sizes(fresh.scene);
    };
    // The first view's tiles all fit full squares; the wider view's nearest
    // take full squares and the rest half ones, and its tiles new to the
    // view are packed in half ones while the camera moves.
    constexpr uint32_t first_columns = 40;
    const auto wider = planned(80, false, 1);
    for (const bool hold : {true, false}) {
        Fixture moving;
        wide_terrain(moving, 80, 80);
        view(moving, first_columns, false, 0, hold);
        OA_CHECK(live::build(moving.inputs, moving.scene, moving.error));
        for (const float size : sizes(moving.scene))
            OA_CHECK(size == 32);
        view(moving, 80, false, 1, hold);
        OA_CHECK(live::build(moving.inputs, moving.scene, moving.error));
        const auto moved = sizes(moving.scene);
        OA_CHECK(moved.size() == wider.size() && !moved.empty());
        bool larger_planned = false;
        for (std::size_t triangle = 0; triangle < moved.size() && moved.size() == wider.size();
             ++triangle) {
            const auto* vertex = &moving.scene.vertices[triangle * 3];
            const bool new_tile =
                (vertex[0].x + vertex[1].x + vertex[2].x) / 3 > first_columns * tile_edge;
            if (new_tile && hold)
                OA_CHECK(moved[triangle] == 16);
            else
                OA_CHECK(moved[triangle] >= wider[triangle]);
            larger_planned |= new_tile && wider[triangle] == 32;
        }
        OA_CHECK(larger_planned);
    }
    // Nearest at the head now: the plan gives the head's tiles the full
    // squares the foot's had. A moving camera keeps every tile where it is
    // until it has stood still for ten frames.
    const auto head = planned(80, true, 2);
    for (const bool hold : {true, false}) {
        Fixture moving;
        wide_terrain(moving, 80, 80);
        view(moving, 80, false, 1, hold);
        OA_CHECK(live::build(moving.inputs, moving.scene, moving.error));
        OA_CHECK(sizes(moving.scene) == wider);
        view(moving, 80, true, 2, hold);
        auto revision = moving.scene.texture_revision;
        OA_CHECK(live::build(moving.inputs, moving.scene, moving.error));
        if (!hold) {
            OA_CHECK(moving.scene.texture_revision != revision);
            continue;
        }
        OA_CHECK(moving.scene.texture_revision == revision);
        OA_CHECK(sizes(moving.scene) == wider);
        for (int frame = 1; frame < 10; ++frame) {
            OA_CHECK(live::build(moving.inputs, moving.scene, moving.error));
            OA_CHECK(moving.scene.texture_revision == revision);
        }
        OA_CHECK(live::build(moving.inputs, moving.scene, moving.error));
        OA_CHECK(moving.scene.texture_revision != revision);
        const auto settled = sizes(moving.scene);
        OA_CHECK(settled.size() == head.size());
        for (std::size_t triangle = 0; triangle < settled.size() && settled.size() == head.size();
             ++triangle)
            OA_CHECK(settled[triangle] >= head[triangle]);
        revision = moving.scene.texture_revision;
        OA_CHECK(live::build(moving.inputs, moving.scene, moving.error));
        OA_CHECK(moving.scene.texture_revision == revision);
    }
}

/// Tells whether the squares of a scene's terrain materials and its spare
/// terrain squares lie in its atlas apart from one another, each on the
/// smallest square's grid, and whether every split square whose quarters
/// are all spare has been made whole again.
bool terrain_squares_apart(const live::Scene& scene) {
    constexpr uint32_t smallest = 8;
    const uint32_t across = scene.texture_width / smallest;
    std::vector<uint8_t> taken(static_cast<std::size_t>(across) * scene.texture_height / smallest);
    bool apart = true;
    const auto take = [&](const live::AtlasRect& square) {
        if (square.x % smallest != 0 || square.y % smallest != 0 ||
            square.x + square.width > scene.texture_width ||
            square.y + square.height > scene.texture_height) {
            apart = false;
            return;
        }
        for (uint32_t y = square.y / smallest; y < (square.y + square.height) / smallest; ++y)
            for (uint32_t x = square.x / smallest; x < (square.x + square.width) / smallest; ++x)
                apart = taken[static_cast<std::size_t>(y) * across + x]++ == 0 && apart;
    };
    for (const auto& [key, material] : scene.terrain_materials)
        take(material.rectangle);
    for (const auto& spare : scene.spare_terrain_squares)
        for (const auto& square : spare)
            take(square);
    for (const auto& [key, quarters] : scene.split_terrain_squares)
        apart = apart && quarters != 0b1111;
    return apart;
}

/// A tile whose art changes is packed again into the square it leaves: the
/// atlas fills no further however often it changes.
void terrain_square_given_back() {
    Fixture fixture;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.terrain_materials.size() == 1);
    const auto square = fixture.scene.terrain_materials.begin()->second.rectangle;
    const auto next_x = fixture.scene.next_x;
    const auto next_y = fixture.scene.next_y;
    for (const uint8_t index : {alternate_index, ground_index, alternate_index}) {
        std::fill(
            fixture.map.tile_palette_indices.begin(), fixture.map.tile_palette_indices.end(), index
        );
        const auto revision = fixture.scene.texture_revision;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.scene.texture_revision > revision);
        OA_CHECK(fixture.scene.terrain_materials.size() == 1);
        OA_CHECK(fixture.scene.terrain_materials.begin()->second.rectangle == square);
        OA_CHECK(fixture.scene.next_x == next_x && fixture.scene.next_y == next_y);
        const auto red = index == ground_index ? ground_color[0] : alternate_color[0];
        OA_CHECK(triangle_color(fixture.scene, 0)[0] == red);
    }
}

/// Zoomed in from a view of more tiles than full squares hold to one whose
/// tiles all fit in full squares, the atlas is not emptied: the tiles the
/// frame has not drawn give their squares back, the small squares are made
/// whole again, and the frame's tiles are packed in full squares in that
/// room. No two squares overlap, every tile samples its own colour, and the
/// frame after packs nothing.
void terrain_squares_given_back_whole() {
    constexpr uint32_t across = 128;
    Fixture fixture;
    wide_terrain(fixture, across, across);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_width == 4096);
    OA_CHECK(terrain_squares_apart(fixture.scene));
    // An emptied atlas would be packed again from its first row.
    const auto rows = fixture.scene.next_y;
    fixture.inputs.right = fixture.inputs.bottom = 40 * tile_edge;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.out_of_room);
    OA_CHECK(fixture.scene.next_y >= rows);
    OA_CHECK(terrain_squares_apart(fixture.scene));
    OA_CHECK(!fixture.scene.spare_terrain_squares[0].empty());
    for (const auto& [key, material] : fixture.scene.terrain_materials)
        OA_CHECK(material.drawn == fixture.scene.terrain_passes);
    OA_CHECK(fixture.scene.terrain_vertex_count != 0);
    for (std::size_t first = 0; first + 2 < fixture.scene.terrain_vertex_count; first += 3) {
        const auto& a = fixture.scene.vertices[first];
        const auto& b = fixture.scene.vertices[first + 1];
        const auto& c = fixture.scene.vertices[first + 2];
        OA_CHECK(a.terrain_size == 32);
        const auto column = static_cast<uint32_t>(a.x + b.x + c.x) / 3 / tile_edge;
        const auto row = static_cast<uint32_t>(a.z + b.z + c.z) / 3 / tile_edge;
        const auto tile = static_cast<std::size_t>(row) * across + column;
        const auto entry = (wide_first_colour + tile % wide_colours) * oa::palette_entry_bytes;
        const auto sampled = triangle_color(fixture.scene, first);
        for (std::size_t channel = 0; channel < 3; ++channel)
            OA_CHECK(sampled[channel] == fixture.palette[entry + channel]);
    }
    const auto revision = fixture.scene.texture_revision;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision == revision);
}

/// Ground too wide for the geometry budget is drawn from fewer height
/// samples: fewer triangles, each tile still sampling its own art, and the
/// samples kept at the simulation's heights.
void coarse_ground_far_out() {
    constexpr uint32_t across = 15;
    constexpr uint8_t first_colour = 1;
    const auto make = [&](live::Scene& scene,
                          std::string& error,
                          oa::formats::tnt::Map& map,
                          oa::PaletteBytes& palette,
                          live::Inputs& inputs) {
        map.tile_width = map.tile_height = across;
        map.attribute_width = map.attribute_height = across * 2;
        map.tile_count = across * across;
        map.tile_indices.resize(static_cast<std::size_t>(across) * across);
        map.tile_palette_indices.resize(map.tile_indices.size() * tile_edge * tile_edge);
        for (std::size_t tile = 0; tile < map.tile_indices.size(); ++tile) {
            map.tile_indices[tile] = static_cast<uint16_t>(tile);
            std::fill_n(
                map.tile_palette_indices.begin() +
                    static_cast<std::ptrdiff_t>(tile * tile_edge * tile_edge),
                tile_edge * tile_edge,
                static_cast<uint8_t>(first_colour + tile)
            );
            const auto entry = (first_colour + tile) * oa::palette_entry_bytes;
            palette[entry] = static_cast<uint8_t>(tile);
            palette[entry + 1] = static_cast<uint8_t>(255 - tile);
            palette[entry + 2] = 40;
        }
        map.attributes.assign(
            static_cast<std::size_t>(map.attribute_width) * map.attribute_height, {}
        );
        for (uint32_t row = 0; row < map.attribute_height; ++row)
            for (uint32_t column = 0; column < map.attribute_width; ++column)
                map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column]
                    .height = static_cast<uint8_t>((column * 3 + row * 5) % 40);
        inputs.sight.mapping = false;
        inputs.sight.line_of_sight = false;
        inputs.right = static_cast<int32_t>(across) * tile_edge;
        inputs.bottom = static_cast<int32_t>(across) * tile_edge;
        return live::build(inputs, scene, error);
    };
    Fixture fine;
    OA_CHECK(make(fine.scene, fine.error, fine.map, fine.palette, fine.inputs));
    Fixture coarse;
    coarse.scene.terrain_square_limit = 64;
    OA_CHECK(make(coarse.scene, coarse.error, coarse.map, coarse.palette, coarse.inputs));
    OA_CHECK(!coarse.scene.out_of_room);
    OA_CHECK(coarse.scene.terrain_vertex_count != 0);
    OA_CHECK(coarse.scene.terrain_vertex_count < fine.scene.terrain_vertex_count);
    bool sample_found = false;
    for (std::size_t first = 0; first + 2 < coarse.scene.terrain_vertex_count; first += 3) {
        const auto* vertex = &coarse.scene.vertices[first];
        OA_CHECK(vertex->terrain_size == 32);
        // A corner of a drawn square stands at the simulation's height.
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& point = vertex[corner];
            if (std::fmod(point.x, 64.0F) == 0 && std::fmod(point.z, 64.0F) == 0 &&
                point.x < across * tile_edge && point.z < across * tile_edge) {
                const auto column = static_cast<uint32_t>(point.x) / cell_edge;
                const auto row = static_cast<uint32_t>(point.z) / cell_edge;
                OA_CHECK(
                    point.height ==
                    coarse.map
                        .attributes
                            [static_cast<std::size_t>(row) * coarse.map.attribute_width + column]
                        .height
                );
                sample_found = true;
            }
        }
        const float x = (vertex[0].x + vertex[1].x + vertex[2].x) / 3;
        const float painted = (vertex[0].z - vertex[0].height * 0.5F + vertex[1].z -
                               vertex[1].height * 0.5F + vertex[2].z - vertex[2].height * 0.5F) /
                              3;
        const auto tile = static_cast<std::size_t>(painted / tile_edge) * across +
                          static_cast<std::size_t>(x / tile_edge);
        const auto entry = (first_colour + tile) * oa::palette_entry_bytes;
        const auto sampled = triangle_color(coarse.scene, first);
        for (std::size_t channel = 0; channel < 3; ++channel)
            OA_CHECK(sampled[channel] == coarse.palette[entry + channel]);
    }
    OA_CHECK(sample_found);
}

/// A turned view leaves out the ground wholly beyond its picture, and keeps
/// every triangle that reaches the picture as the whole crop draws it, at
/// full detail and with distant tiles merged.
void terrain_beyond_picture() {
    constexpr uint32_t across = 15;
    constexpr uint32_t picture_width = 200;
    constexpr uint32_t picture_height = 150;
    // Screen x, screen y and depth over map x, height and z: turned an
    // eighth, so that the crop's corners lie far beyond the picture.
    constexpr std::array<float, 12> projection{2, 0, -2, 0, 1, -1, 1, 0, 0, 1, 0, 0};
    const auto make = [&](Fixture& fixture, uint32_t width, uint32_t height, float detail) {
        auto& map = fixture.map;
        map.tile_width = map.tile_height = across;
        map.attribute_width = map.attribute_height = across * 2;
        map.tile_count = 1;
        map.tile_indices.assign(static_cast<std::size_t>(across) * across, 0);
        map.attributes.assign(
            static_cast<std::size_t>(map.attribute_width) * map.attribute_height, {}
        );
        // Level ground on the left, which merges far out, and relief on the
        // right, which does not.
        for (uint32_t row = 0; row < map.attribute_height; ++row)
            for (uint32_t column = 0; column < map.attribute_width; ++column)
                map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column]
                    .height =
                    static_cast<uint8_t>(column < across ? 10 : (column * 3 + row * 5) % 40);
        fixture.inputs.sight.mapping = false;
        fixture.inputs.sight.line_of_sight = false;
        fixture.inputs.right = static_cast<int32_t>(across) * tile_edge;
        fixture.inputs.bottom = static_cast<int32_t>(across) * tile_edge;
        fixture.inputs.detail_scale = detail;
        fixture.inputs.clip_projection = projection;
        fixture.inputs.clip_width = width;
        fixture.inputs.clip_height = height;
        return live::build(fixture.inputs, fixture.scene, fixture.error);
    };
    using Corners = std::array<float, 9>;
    const auto triangles = [](const live::Scene& scene) {
        std::vector<Corners> result;
        for (std::size_t first = 0; first + 2 < scene.terrain_vertex_count; first += 3) {
            Corners corners{};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto& vertex = scene.vertices[first + corner];
                corners[corner * 3] = vertex.x;
                corners[corner * 3 + 1] = vertex.height;
                corners[corner * 3 + 2] = vertex.z;
            }
            result.push_back(corners);
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    const auto reaches_picture = [&](const Corners& corners) {
        float left = std::numeric_limits<float>::infinity(), right = -left;
        float top = left, bottom = -left;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const float x = corners[corner * 3], height = corners[corner * 3 + 1],
                        z = corners[corner * 3 + 2];
            const float screen_x =
                projection[0] * x + projection[1] * height + projection[2] * z + projection[3];
            const float screen_y =
                projection[4] * x + projection[5] * height + projection[6] * z + projection[7];
            left = std::min(left, screen_x);
            right = std::max(right, screen_x);
            top = std::min(top, screen_y);
            bottom = std::max(bottom, screen_y);
        }
        return right > 0 && left < picture_width && bottom > 0 && top < picture_height;
    };
    for (const float detail : {1.0F, 0.5F}) {
        Fixture whole;
        OA_CHECK(make(whole, 0, 0, detail));
        Fixture pictured;
        OA_CHECK(make(pictured, picture_width, picture_height, detail));
        const auto all = triangles(whole.scene);
        const auto kept = triangles(pictured.scene);
        OA_CHECK(!kept.empty());
        OA_CHECK(kept.size() * 2 < all.size());
        // Only the crop's own triangles, and each that reaches the picture.
        OA_CHECK(std::includes(all.begin(), all.end(), kept.begin(), kept.end()));
        std::size_t reaching = 0;
        for (const auto& corners : all)
            if (reaches_picture(corners)) {
                ++reaching;
                OA_CHECK(std::binary_search(kept.begin(), kept.end(), corners));
            }
        OA_CHECK(reaching != 0);
    }
}

/// While the player's camera moves, its ground is drawn beyond the picture
/// by a tenth of the picture's larger side and kept: the frames after it
/// whose pictures move no further take that ground, the same revision,
/// which holds every triangle their own pictures show, and a picture that
/// moves further draws it again. Once the camera has settled, the ground
/// is the picture's own again.
void ground_kept_while_moving() {
    constexpr uint32_t across = 15;
    constexpr uint32_t picture_width = 200;
    constexpr uint32_t picture_height = 150;
    // Turned an eighth, as terrain_beyond_picture's view; the first row's
    // offset pans the picture across.
    constexpr std::array<float, 12> projection{2, 0, -2, 0, 1, -1, 1, 0, 0, 1, 0, 0};
    const auto make = [&](Fixture& fixture, float pan) {
        auto& map = fixture.map;
        map.tile_width = map.tile_height = across;
        map.attribute_width = map.attribute_height = across * 2;
        map.tile_count = 1;
        map.tile_indices.assign(static_cast<std::size_t>(across) * across, 0);
        map.attributes.assign(
            static_cast<std::size_t>(map.attribute_width) * map.attribute_height, {}
        );
        for (uint32_t row = 0; row < map.attribute_height; ++row)
            for (uint32_t column = 0; column < map.attribute_width; ++column)
                map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column]
                    .height = static_cast<uint8_t>((column * 3 + row * 5) % 40);
        fixture.inputs.sight.mapping = false;
        fixture.inputs.sight.line_of_sight = false;
        fixture.inputs.right = static_cast<int32_t>(across) * tile_edge;
        fixture.inputs.bottom = static_cast<int32_t>(across) * tile_edge;
        fixture.inputs.clip_projection = projection;
        fixture.inputs.clip_projection[3] = pan;
        fixture.inputs.clip_width = picture_width;
        fixture.inputs.clip_height = picture_height;
        fixture.inputs.tiles_hold_while_moving = true;
    };
    using Corners = std::array<float, 9>;
    const auto triangles = [](const live::Scene& scene) {
        std::vector<Corners> result;
        for (std::size_t first = 0; first + 2 < scene.terrain_vertex_count; first += 3) {
            Corners corners{};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto& vertex = scene.vertices[first + corner];
                corners[corner * 3] = vertex.x;
                corners[corner * 3 + 1] = vertex.height;
                corners[corner * 3 + 2] = vertex.z;
            }
            result.push_back(corners);
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    // The triangles a new scene draws for the picture panned so far.
    const auto pictured = [&](float pan) {
        Fixture fresh;
        make(fresh, pan);
        OA_CHECK(live::build(fresh.inputs, fresh.scene, fresh.error));
        return triangles(fresh.scene);
    };
    Fixture moving;
    make(moving, 0);
    const auto build = [&] {
        OA_CHECK(live::build(moving.inputs, moving.scene, moving.error));
        return moving.scene.ground_revision;
    };
    build();
    const auto still = build();
    OA_CHECK(moving.scene.ground_kept);
    OA_CHECK(triangles(moving.scene) == pictured(0));
    // Panned 5 pixels: the ground is drawn beyond the picture, 20 pixels
    // further, and kept at once.
    moving.inputs.clip_projection[3] = 5;
    const auto moved = build();
    OA_CHECK(moved != still && moving.scene.ground_kept);
    const auto wide = triangles(moving.scene);
    const auto first_picture = pictured(5);
    OA_CHECK(wide.size() > first_picture.size());
    OA_CHECK(std::includes(wide.begin(), wide.end(), first_picture.begin(), first_picture.end()));
    // Panned 15 pixels, 10 from the kept ground's picture: the same ground.
    moving.inputs.clip_projection[3] = 15;
    OA_CHECK(build() == moved);
    const auto second_picture = pictured(15);
    OA_CHECK(triangles(moving.scene) == wide);
    OA_CHECK(std::includes(wide.begin(), wide.end(), second_picture.begin(), second_picture.end()));
    // Panned 30 pixels, 25 from it: drawn again.
    moving.inputs.clip_projection[3] = 30;
    const auto further = build();
    OA_CHECK(further != moved);
    const auto third = triangles(moving.scene);
    const auto third_picture = pictured(30);
    OA_CHECK(std::includes(third.begin(), third.end(), third_picture.begin(), third_picture.end()));
    // Standing still, the camera settles after ten frames, and the ground
    // is the picture's own, kept at once.
    for (int frame = 1; frame < 10; ++frame)
        OA_CHECK(build() == further);
    const auto settled = build();
    OA_CHECK(settled != further && moving.scene.ground_kept);
    OA_CHECK(triangles(moving.scene) == third_picture);
    OA_CHECK(build() == settled);
    // A recording's camera draws each picture's own ground.
    moving.inputs.tiles_hold_while_moving = false;
    moving.inputs.clip_projection[3] = 35;
    OA_CHECK(build() != settled);
    OA_CHECK(triangles(moving.scene) == pictured(35));
}

/// Known sight boundaries interpolate, while every unknown terrain sample remains black.
void smooth_fog_boundary() {
    Fixture fixture;
    fixture.map.attribute_width = 6;
    fixture.map.tile_width = 3;
    fixture.map.attributes.assign(12, {});
    fixture.map.tile_indices = {0, 0, 0};
    std::array<uint16_t, 3> coverage{1, 0, 0};
    std::array<uint16_t, 3> mapped{viewer_bit, viewer_bit, 0};
    fixture.inputs.sight.coverage = coverage;
    fixture.inputs.sight.player_bits = mapped;
    fixture.inputs.sight.width = 3;
    fixture.inputs.right = 96;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto brightness = [&](float x) {
        for (const auto& v : fixture.scene.vertices)
            if (v.x == x && std::abs(v.z - v.height * 0.5F - 16) < tolerance)
                return v.brightness;
        OA_CHECK(false);
        return -1.0F;
    };
    OA_CHECK(brightness(16) > brightness(32));
    OA_CHECK(brightness(32) > brightness(48));
    OA_CHECK(brightness(48) > brightness(64));
    OA_CHECK(brightness(64) == 0);
    OA_CHECK(
        std::any_of(
            fixture.scene.vertices.begin(), fixture.scene.vertices.end(), [](const auto& vertex) {
                return vertex.x == 20;
            }
        )
    );
    const float grey = brightness(48);
    fixture.inputs.sight.dithered = true;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(brightness(48) > grey);
    for (std::size_t first = 0; first < fixture.scene.terrain_vertex_count; first += 3) {
        const float x = (fixture.scene.vertices[first].x + fixture.scene.vertices[first + 1].x +
                         fixture.scene.vertices[first + 2].x) /
                        3;
        if (x >= 64) {
            const std::array<uint8_t, 4> black{0, 0, 0, 255};
            OA_CHECK(triangle_color(fixture.scene, first) == black);
        }
    }
}

/// Distant tiles preserve coverage and relief, while sight transitions use fewer samples.
void terrain_zoom_detail() {
    Fixture fixture;
    for (auto& attribute : fixture.map.attributes)
        attribute.height = 10;
    const auto area = [&] {
        double result{};
        for (std::size_t i = 0; i < fixture.scene.terrain_vertex_count; i += 3) {
            const auto& a = fixture.scene.vertices[i];
            const auto& b = fixture.scene.vertices[i + 1];
            const auto& c = fixture.scene.vertices[i + 2];
            const double ay = a.z - a.height * 0.5F, by = b.z - b.height * 0.5F,
                         cy = c.z - c.height * 0.5F;
            result += std::abs((b.x - a.x) * (cy - ay) - (c.x - a.x) * (by - ay)) * 0.5;
        }
        return result;
    };
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto close_vertices = fixture.scene.terrain_vertex_count;
    const auto close_area = area();
    fixture.inputs.detail_scale = 0.25F;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.terrain_vertex_count < close_vertices);
    OA_CHECK(std::abs(area() - close_area) < tolerance);
    for (const auto& vertex : fixture.scene.vertices)
        OA_CHECK(vertex.height == 10);
    fixture.map.attributes[3].height = 26;
    fixture.inputs.detail_scale = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto ridge_area = area();
    fixture.inputs.detail_scale = 0.25F;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(
        std::any_of(
            fixture.scene.vertices.begin(), fixture.scene.vertices.end(), [](const auto& v) {
                return v.x == 16 && v.height == 26;
            }
        )
    );
    OA_CHECK(std::abs(area() - ridge_area) < tolerance);
    fixture.map.attribute_width = 6;
    fixture.map.tile_width = 3;
    fixture.map.attributes.assign(12, {});
    fixture.map.tile_indices = {0, 0, 0};
    std::array<uint16_t, 3> coverage{1, 0, 0};
    std::array<uint16_t, 3> mapped{viewer_bit, viewer_bit, 0};
    fixture.inputs.sight.coverage = coverage;
    fixture.inputs.sight.player_bits = mapped;
    fixture.inputs.sight.width = 3;
    fixture.inputs.right = 96;
    fixture.inputs.detail_scale = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto detailed_fog = fixture.scene.terrain_vertex_count;
    fixture.inputs.detail_scale = 0.25F;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.terrain_vertex_count < detailed_fog);
    OA_CHECK(std::abs(area() - 3072) < tolerance);
    for (std::size_t i = 0; i < fixture.scene.terrain_vertex_count; i += 3) {
        const float centre = (fixture.scene.vertices[i].x + fixture.scene.vertices[i + 1].x +
                              fixture.scene.vertices[i + 2].x) /
                             3;
        if (centre >= 64)
            OA_CHECK(triangle_color(fixture.scene, i)[0] == 0);
    }
}

/// Subpixel particle cores fade out before their wider halos disappear.
void particle_zoom_detail() {
    Fixture fixture;
    fixture.draws.spatial_particles.push_back(
        {{8 * 65536, 20 * 65536, 20 * 65536}, {80, 255, 80}, true}
    );
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() - fixture.scene.terrain_vertex_count == 48);
    fixture.inputs.detail_scale = 0.1F;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() - fixture.scene.terrain_vertex_count == 24);
    OA_CHECK(
        fixture.scene.vertices.back().opacity > 0 && fixture.scene.vertices.back().opacity < 0.16F
    );
    fixture.inputs.detail_scale = 0.01F;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.particle_count == 0);
}

/// Stable frames avoid atlas uploads; a palette change creates new material
/// pixels, the tile's in the square it gives back.
void atlas_reuse() {
    Fixture fixture;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto revision = fixture.scene.texture_revision;
    const auto materials = fixture.scene.materials.size();
    OA_CHECK(revision != 0);
    OA_CHECK(fixture.scene.atlas_update.previous_revision == 0);
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    // The tile's rows and the row under them, which a filtered material
    // packed beside it would sample.
    OA_CHECK(fixture.scene.atlas_update.end_row == tile_edge * 2 + 1);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision == revision);
    OA_CHECK(fixture.scene.materials.size() == materials);
    OA_CHECK(fixture.scene.atlas_update.previous_revision == revision);
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    OA_CHECK(fixture.scene.atlas_update.end_row == 0);
    constexpr uint8_t replacement_red = 210;
    fixture.palette[ground_index * oa::palette_entry_bytes] = replacement_red;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision > revision);
    OA_CHECK(fixture.scene.atlas_update.previous_revision == revision);
    OA_CHECK(fixture.scene.atlas_update.first_row == fixture.scene.atlas_update.end_row);
    const auto& regions = fixture.scene.atlas_update.regions;
    OA_CHECK(regions.size() == 1);
    OA_CHECK(
        !regions.empty() && regions[0].x == 0 && regions[0].y == 0 &&
        regions[0].width == tile_edge * 2 && regions[0].height == tile_edge * 2
    );
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == replacement_red);
}

/// Persistent tile entries compare source bytes, including mutations at the same address.
void terrain_material_mutation() {
    Fixture fixture;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.terrain_materials.empty());
    const auto revision = fixture.scene.texture_revision;
    std::fill(
        fixture.map.tile_palette_indices.begin(),
        fixture.map.tile_palette_indices.end(),
        alternate_index
    );
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision > revision);
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == alternate_color[0]);
    fixture.map.tile_palette_indices[0] = ground_index;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto& corner = fixture.scene.vertices[0];
    const auto column = static_cast<uint32_t>(std::lround(corner.u * fixture.scene.texture_width));
    const auto row = static_cast<uint32_t>(std::lround(corner.v * fixture.scene.texture_height));
    OA_CHECK(
        fixture.scene.texture_rgba[(row * fixture.scene.texture_width + column) * 4] ==
        ground_color[0]
    );
    fixture.mapped[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == 0);
    fixture.mapped[0] = viewer_bit;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == alternate_color[0]);
}

/// The ground of a frame drawn from the same as the two before it is the
/// one they drew; a change to the heights, the art, the fog or the picture
/// in place draws it again.
void ground_kept_while_unchanged() {
    Fixture fixture;
    fixture.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0, 0};
    fixture.inputs.clip_width = fixture.inputs.clip_height = 64;
    const auto ground = [&] {
        return std::vector<oa::app::renderer_live_gpu::Vertex>(
            fixture.scene.vertices.begin(),
            fixture.scene.vertices.begin() +
                static_cast<std::ptrdiff_t>(fixture.scene.terrain_vertex_count)
        );
    };
    const auto same = [](const auto& a, const auto& b) {
        return a.size() == b.size() &&
               std::memcmp(a.data(), b.data(), a.size() * sizeof(a.front())) == 0;
    };
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.ground_kept);
    const auto first = ground();
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.ground_kept);
    OA_CHECK(same(ground(), first));
    const auto revision = fixture.scene.texture_revision;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.ground_kept);
    OA_CHECK(same(ground(), first));
    OA_CHECK(fixture.scene.texture_revision == revision);
    // A height changed in place.
    fixture.map.attributes[3].height = 12;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.ground_kept);
    OA_CHECK(
        std::any_of(
            fixture.scene.vertices.begin(),
            fixture.scene.vertices.begin() +
                static_cast<std::ptrdiff_t>(fixture.scene.terrain_vertex_count),
            [](const auto& vertex) { return vertex.height == 12; }
        )
    );
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.ground_kept);
    // The art changed in place: the tile is packed again, and the kept
    // ground is not taken.
    std::fill(
        fixture.map.tile_palette_indices.begin(),
        fixture.map.tile_palette_indices.end(),
        alternate_index
    );
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision > revision);
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == alternate_color[0]);
    // The cell never mapped.
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    fixture.mapped[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.ground_kept);
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == 0);
    // A count of sight stamps that leaves the fog as it is keeps the ground.
    fixture.mapped[0] = viewer_bit;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.ground_kept);
    fixture.coverage[0] = 2;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.ground_kept);
    // Another picture.
    fixture.inputs.clip_projection[3] = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.ground_kept);
}

/// Atlas repacking uploads the rows packed afresh, and the row under them,
/// while retaining the prior GPU revision: no triangle samples the others.
void atlas_repack_rows() {
    Fixture fixture;
    fixture.scene.texture_width = fixture.scene.texture_height = tile_edge * 2;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto revision = fixture.scene.texture_revision;
    // A sprite the full atlas has no room for.
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 8;
    frame.pixels.assign(64, alternate_index);
    frame.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&frame, {8, 8}});
    fixture.palette[ground_index * oa::palette_entry_bytes] = 77;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_width > tile_edge * 2);
    OA_CHECK(fixture.scene.atlas_update.previous_revision == revision);
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    OA_CHECK(fixture.scene.atlas_update.end_row == tile_edge * 2 + 1);
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == 77);
    // An already enlarged atlas emptied uploads only its packed rows too.
    fixture.scene.next_x = fixture.scene.texture_width;
    fixture.scene.next_y = fixture.scene.texture_height;
    const auto enlarged_revision = fixture.scene.texture_revision;
    fixture.palette[ground_index * oa::palette_entry_bytes] = 88;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.atlas_update.previous_revision == enlarged_revision);
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    OA_CHECK(fixture.scene.atlas_update.end_row == tile_edge * 2 + 1);
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == 88);
}

/// An atlas emptied at its largest uploads the rows it packs afresh and
/// none past them: every texel a triangle of the frame samples, and those
/// beside them, lie in the rows uploaded, though the rows past them hold
/// what was packed before.
void emptied_atlas_uploads_packed_rows() {
    Fixture fixture;
    wide_terrain(fixture, 40, 40);
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 8;
    frame.pixels.assign(64, wide_first_colour);
    frame.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&frame, {8, 8}});
    auto& scene = fixture.scene;
    scene.texture_width = scene.texture_height = 4096;
    OA_CHECK(live::build(fixture.inputs, scene, fixture.error));
    // The atlas full, and a palette change for the sprite to be packed again.
    scene.next_x = scene.texture_width;
    scene.next_y = scene.texture_height;
    const auto revision = scene.texture_revision;
    fixture.palette[(wide_first_colour + 1) * oa::palette_entry_bytes] = 77;
    OA_CHECK(live::build(fixture.inputs, scene, fixture.error));
    const auto& update = scene.atlas_update;
    OA_CHECK(update.previous_revision == revision);
    OA_CHECK(update.first_row == 0);
    OA_CHECK(update.end_row == scene.next_y + scene.row_height + 1);
    OA_CHECK(update.end_row < scene.texture_height / 2);
    OA_CHECK(update.regions.empty());
    for (const auto& vertex : scene.vertices) {
        const float row = vertex.terrain_size > 0 ? vertex.terrain_y + 2 * vertex.terrain_size
                                                  : vertex.v * scene.texture_height + 1;
        OA_CHECK(row <= static_cast<float>(update.end_row));
    }
}

/// Repacking due to a sprite also invalidates unchanged cached terrain rectangles.
void terrain_material_repack() {
    Fixture fixture;
    fixture.scene.texture_width = fixture.scene.texture_height = tile_edge * 2;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto original = triangle_color(fixture.scene, 0);
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 8;
    frame.pixels.assign(64, alternate_index);
    frame.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&frame, {8, 8}});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_width > tile_edge);
    OA_CHECK(triangle_color(fixture.scene, 0) == original);
    OA_CHECK(fixture.scene.materials.size() == 1 && fixture.scene.terrain_materials.size() == 1);
}

/// A planner-suppressed attachment never emits its otherwise visible model.
void planned_model_visibility() {
    Fixture fixture;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    source->objects[0].vertices = {{0, 0, 0}, {fixed, 0, 0}, {0, fixed, 0}};
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    drawing::PreparedModel prepared{};
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(1);
    prepared.objects[0].primitives[0].flags = drawing::primitive_colored;
    prepared.objects[0].primitives[0].color = ground_index;
    oa::Unit unit{};
    unit.position = {8 * fixed, 0, 8 * fixed};
    unit.id = 7;
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.instance = &instance;
    draw.model.prepared = &prepared;
    draw.model.unit = &unit;
    draw.plan.linked.drawn = true;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 1);
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    OA_CHECK(fixture.scene.unit_ranges[0].unit_id == 7);
    OA_CHECK(fixture.scene.unit_ranges[0].first_vertex == fixture.scene.terrain_vertex_count);
    OA_CHECK(fixture.scene.unit_ranges[0].vertex_count == 3);
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 3);
    // The planner's presentation geometry survives later transforms and record positions.
    draw.shown_position = oa::FixedVec3{9 * fixed, 0, 8 * fixed};
    draw.shown_vertices.push_back(instance.pieces()[0].transformed_vertices);
    const float captured_x = draw.shown_vertices[0][0].x / static_cast<float>(fixed);
    unit.position.x = 20 * fixed;
    instance.pieces()[0].transformed_vertices[0].x += 5 * fixed;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto first = fixture.scene.unit_ranges[0].first_vertex;
    OA_CHECK(std::abs(fixture.scene.vertices[first].x - (9 + captured_x)) < tolerance);
    OA_CHECK(fixture.scene.vertices[first].emission_g == 0);
    unit.build_remaining = 0.5F;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[first].emission_g > 0);
    OA_CHECK(std::abs(fixture.scene.vertices[first].opacity - 0.5F) < tolerance);
    OA_CHECK(fixture.scene.unit_ranges[0].vertex_count > 3);
    OA_CHECK(fixture.scene.vertices[first].emission_g > fixture.scene.vertices[first].emission_r);
    const float dim_emission = fixture.scene.vertices[first].emission_g;
    fixture.inputs.tick = 30;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const float bright_emission = fixture.scene.vertices[first].emission_g;
    OA_CHECK(bright_emission > dim_emission);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[first].emission_g == bright_emission);
    const auto construction_vertices = fixture.scene.vertices.size();
    for (const float intensity : {0.0F, 0.5F, 1.0F, 1.5F, 2.0F}) {
        fixture.inputs.nanolathe_intensity = intensity;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(
            std::abs(fixture.scene.vertices[first].emission_g - bright_emission * intensity) <
            tolerance
        );
        OA_CHECK(fixture.scene.vertices.size() == construction_vertices);
        OA_CHECK(std::abs(fixture.scene.vertices[first].opacity - 0.5F) < tolerance);
    }
    fixture.inputs.nanolathe_intensity = 1;
    fixture.inputs.nanolathe_lighting = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[first].emission_g == 0);
    OA_CHECK(fixture.scene.vertices[first].brightness == 1);
    unit.build_remaining = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges[0].vertex_count == 18);
    for (std::size_t index = first; index < fixture.scene.vertices.size(); ++index)
        OA_CHECK(fixture.scene.vertices[index].opacity == 1);
    unit.build_remaining = 0.1F;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[first].opacity > 0.95F);
    unit.build_remaining = 0;
    fixture.inputs.nanolathe_lighting = true;
    fixture.draws.spatial_particles.push_back({{9 * fixed, 0, 8 * fixed}, {80, 255, 80}, true});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[first].emission_g > 0);
    fixture.inputs.nanolathe_lighting = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[first].emission_g == 0);
    fixture.draws.spatial_particles.clear();
    draw.plan.linked.drawn = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 0);
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
}

/// A pictured unit's walls of the image key draw nothing, as its picture
/// leaves them clear, while a unit drawn flat keeps them as a colour; a
/// cloaked unit, or every unit under the debug overlay, is drawn at half
/// opacity after the opaque units, and keeps its range for picking.
void model_key_walls_and_cloak() {
    Fixture fixture;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    source->objects[0].vertices = {{0, 0, 0}, {fixed, 0, 0}, {0, fixed, 0}};
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    drawing::PreparedModel wall{};
    wall.objects.resize(1);
    wall.objects[0].primitives.resize(1);
    wall.objects[0].primitives[0].flags = drawing::primitive_colored;
    wall.objects[0].primitives[0].color = drawing::image_key;
    drawing::ModelState state{};
    state.image = oa::present::create_sprite(8, 8);
    oa::Unit unit{};
    unit.position = {8 * fixed, 0, 8 * fixed};
    unit.id = 7;
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.instance = &instance;
    draw.model.prepared = &wall;
    draw.model.unit = &unit;
    draw.model.state = &state;
    draw.plan.linked.drawn = true;
    draw.plan.model.from_image = true;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 1);
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
    draw.plan.model.from_image = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 3);

    // A cloaked unit first in the plan, an opaque one after it.
    drawing::PreparedModel solid = wall;
    solid.objects[0].primitives[0].color = alternate_index;
    draw.model.prepared = &solid;
    unit.state_flags = OA_UNIT_STATE_CLOAKED;
    oa::Unit other = unit;
    other.id = 8;
    other.state_flags = 0;
    other.position.x += 2 * fixed;
    fixture.draws.models.push_back(draw);
    fixture.draws.models[1].model.unit = &other;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.size() == 2);
    const auto range_of = [&](uint16_t id) {
        for (const auto& range : fixture.scene.unit_ranges)
            if (range.unit_id == id)
                return range;
        return live::UnitRange{};
    };
    const auto cloaked = range_of(7);
    const auto opaque = range_of(8);
    OA_CHECK(cloaked.vertex_count == 3 && opaque.vertex_count == 3);
    OA_CHECK(cloaked.first_vertex > opaque.first_vertex);
    OA_CHECK(std::abs(fixture.scene.vertices[cloaked.first_vertex].opacity - 0.5F) < tolerance);
    OA_CHECK(fixture.scene.vertices[opaque.first_vertex].opacity == 1);
    OA_CHECK(std::abs(fixture.scene.vertices[cloaked.first_vertex].x - 8) < tolerance);
    fixture.inputs.translucent_units = true;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(std::abs(fixture.scene.vertices[range_of(8).first_vertex].opacity - 0.5F) < tolerance);
}

/// Team textures share only matching frames, and the next build observes edits.
void model_material_lifetime() {
    Fixture fixture;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 0, 0}, {fixed, 0, 0}, {fixed, fixed, 0}, {0, fixed, 0}};
    source->objects[0].primitives.resize(2);
    for (auto& primitive : source->objects[0].primitives)
        primitive.vertex_indices = {0, 1, 2, 3};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    drawing::TextureSequence texture;
    texture.frames.resize(2);
    std::array<uint8_t, 2> pixels{ground_index, alternate_index};
    for (std::size_t i = 0; i < texture.frames.size(); ++i) {
        auto& frame = texture.frames[i];
        frame.width = frame.height = 1;
        frame.encoding = OA_SPRITE_RAW;
        frame.data = &pixels[i];
    }
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(2);
    for (uint32_t i = 0; i < 2; ++i) {
        auto& primitive = prepared.objects[0].primitives[i];
        primitive.source_index = i;
        primitive.flags = drawing::primitive_animated | drawing::primitive_team;
        primitive.texture = &texture;
    }
    std::array<oa::Unit, 2> units{};
    fixture.draws.models.resize(2);
    fixture.inputs.team_colors[1] = 1;
    for (std::size_t i = 0; i < units.size(); ++i) {
        units[i].position = {8 * fixed, 0, 8 * fixed};
        units[i].owner_index = static_cast<uint8_t>(i);
        units[i].id = static_cast<uint16_t>(i + 1);
        auto& draw = fixture.draws.models[i];
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.model.unit = &units[i];
        draw.plan.linked.drawn = true;
    }
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.size() == 2);
    OA_CHECK(
        triangle_color(fixture.scene, fixture.scene.unit_ranges[0].first_vertex)[0] ==
        ground_color[0]
    );
    OA_CHECK(
        triangle_color(fixture.scene, fixture.scene.unit_ranges[1].first_vertex)[0] ==
        alternate_color[0]
    );
    pixels[0] = alternate_index;
    fixture.palette[alternate_index * oa::palette_entry_bytes] = 77;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, fixture.scene.unit_ranges[0].first_vertex)[0] == 77);
    // A wholly off-crop animated model can be skipped, but high altitude is
    // outside the terrain crop's guaranteed height range and stays conservative.
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    units[0].position.x = units[1].position.x = 500 * fixed;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 0);
    units[0].position.x = units[1].position.x = 33 * fixed;
    for (auto& draw : fixture.draws.models) {
        draw.shown_vertices.push_back(instance.pieces()[0].transformed_vertices);
        draw.shown_vertices[0][0].x -= 4 * fixed;
    }
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 2);
    units[0].position.x = units[1].position.x = 500 * fixed;
    units[0].position.y = units[1].position.y = 300 * fixed;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 2);
    // An explicit GPU screen transform culls every altitude. This view looks
    // directly across the high models, independently of the terrain crop.
    fixture.inputs.clip_projection = {1, 0, 0, 0, 0, 1, 0, -299, 0, 0, 1, 0};
    fixture.inputs.clip_width = fixture.inputs.clip_height = 32;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 0);
    units[0].position.x = units[1].position.x = 8 * fixed;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 2);
    units[0].position.x = units[1].position.x = 33 * fixed;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 2);
    units[0].position.y = units[1].position.y = 400 * fixed;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 0);
    fixture.inputs.clip_width = fixture.inputs.clip_height = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.model_count == 2);
}

/// Opposite faces sharing one plane show the face toward the current camera.
void model_face_winding() {
    Fixture fixture;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    auto& object = source->objects[0];
    object.vertices = {{0, 0, 0}, {fixed, 0, 0}, {0, fixed, 0}};
    object.primitives.resize(2);
    object.primitives[0].vertex_indices = {0, 1, 2};
    object.primitives[1].vertex_indices = {0, 2, 1};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    drawing::PreparedModel prepared{};
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(2);
    for (uint32_t index = 0; index < 2; ++index) {
        auto& primitive = prepared.objects[0].primitives[index];
        primitive.source_index = index;
        primitive.flags = drawing::primitive_colored;
        primitive.color = index == 0 ? ground_index : alternate_index;
    }
    oa::Unit unit{};
    unit.id = 7;
    unit.position = {8 * fixed, 0, 8 * fixed};
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.instance = &instance;
    draw.model.prepared = &prepared;
    draw.model.unit = &unit;
    draw.plan.linked.drawn = true;
    fixture.inputs.view_direction = {0, 1, 0.5F};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges[0].vertex_count == 3);
    const auto front = triangle_color(fixture.scene, fixture.scene.terrain_vertex_count);
    OA_CHECK(front[0] == ground_color[0]);
    fixture.inputs.view_direction = {0, -1, -0.5F};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges[0].vertex_count == 3);
    const auto back = triangle_color(fixture.scene, fixture.scene.terrain_vertex_count);
    OA_CHECK(back[0] == alternate_color[0]);
}

/// Nano particles preserve altitude, emit light and disappear outside current sight.
void spatial_particles() {
    Fixture fixture;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    fixture.draws.spatial_particles.push_back(
        {{8 * fixed, 20 * fixed, 20 * fixed}, {80, 255, 80}, true}
    );
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.particle_count == 1);
    OA_CHECK(fixture.scene.nano_particle_count == 1);
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 48);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        OA_CHECK(vertex.height >= 17 && vertex.height <= 23);
        OA_CHECK(vertex.emission_g == 1);
    }
    const auto first_particle = fixture.scene.terrain_vertex_count;
    for (const float intensity : {0.5F, 1.0F, 1.5F, 2.0F}) {
        fixture.inputs.nanolathe_intensity = intensity;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 48);
        OA_CHECK(
            std::abs(fixture.scene.vertices[first_particle + 24].opacity - 0.16F * intensity) <
            tolerance
        );
        for (std::size_t i = first_particle; i < fixture.scene.vertices.size(); ++i) {
            const auto& vertex = fixture.scene.vertices[i];
            OA_CHECK(vertex.emission_r >= 0 && vertex.emission_r <= 1);
            OA_CHECK(vertex.emission_g >= 0 && vertex.emission_g <= 1);
            OA_CHECK(vertex.emission_b >= 0 && vertex.emission_b <= 1);
            OA_CHECK(vertex.height >= 17 && vertex.height <= 23);
        }
    }
    fixture.inputs.nanolathe_intensity = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 24);
    OA_CHECK(fixture.scene.vertices[first_particle].emission_g == 0);
    fixture.inputs.nanolathe_intensity = 1;
    fixture.inputs.nanolathe_lighting = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.nano_particle_count == 1);
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 24);
    const auto first = fixture.scene.terrain_vertex_count;
    OA_CHECK(fixture.scene.vertices[first].emission_g == 0);
    const std::array<uint8_t, 4> untinted_colour{80, 255, 80, 255};
    OA_CHECK(triangle_color(fixture.scene, first) == untinted_colour);
    fixture.coverage[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.particle_count == 0);
    OA_CHECK(fixture.scene.nano_particle_count == 0);
    fixture.mapped[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.particle_count == 0);
    OA_CHECK(fixture.scene.nano_particle_count == 0);
}

/// Particle clipping retains glow crossing viewport edges and keeps RGB materials distinct.
void particle_screen_bounds() {
    Fixture fixture;
    constexpr int32_t fixed = 65536;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    fixture.inputs.clip_projection = {1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0};
    fixture.inputs.clip_width = fixture.inputs.clip_height = 32;
    fixture.draws.spatial_particles = {
        {{-2 * fixed, 300 * fixed, 8 * fixed}, {80, 255, 80}, true},
        {{34 * fixed, 300 * fixed, 8 * fixed}, {80, 255, 80}, true},
        {{36 * fixed, 300 * fixed, 8 * fixed}, {255, 80, 80}, true},
        {{8 * fixed, 300 * fixed, 8 * fixed}, {255, 80, 80}, false}
    };
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.particle_count == 3);
    OA_CHECK(fixture.scene.nano_particle_count == 2);
    const auto last = fixture.scene.vertices.size() - 3;
    const std::array<uint8_t, 4> red{255, 80, 80, 255};
    OA_CHECK(triangle_color(fixture.scene, last) == red);
    fixture.inputs.nanolathe_lighting = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.particle_count == 1);
    OA_CHECK(fixture.scene.nano_particle_count == 0);
    fixture.draws.spatial_particles.back().color = {25, 50, 75};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const std::array<uint8_t, 4> changed{25, 50, 75, 255};
    OA_CHECK(triangle_color(fixture.scene, fixture.scene.terrain_vertex_count) == changed);
}

/// A low feature follows both triangles of a height cell while a tree remains upright.
void terrain_decals() {
    Fixture fixture;
    for (std::size_t i = 0; i < fixture.map.attributes.size(); ++i)
        fixture.map.attributes[i].height = static_cast<uint8_t>(i * 2);
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = 24;
    frame.height = 24;
    frame.pixels.assign(frame.width * frame.height, ground_index);
    frame.coverage.assign(frame.width * frame.height, 1);
    fixture.draws.sprites.push_back({&frame, {4, 4}, false, true, true});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() > fixture.scene.terrain_vertex_count + 6);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        const float y = vertex.z - vertex.height * 0.5F;
        OA_CHECK(vertex.x >= 4 - tolerance && vertex.x <= 28 + tolerance);
        OA_CHECK(y >= 4 - 0.026F && y <= 28 + tolerance);
        const int32_t column = static_cast<int32_t>(vertex.x) / cell_edge;
        const int32_t row = static_cast<int32_t>(vertex.z) / cell_edge;
        const auto at = [&](int32_t x, int32_t z) {
            return static_cast<float>(
                fixture.map.attributes[std::min(z, 1) * 2 + std::min(x, 1)].height
            );
        };
        const float dx = (vertex.x - column * cell_edge) / cell_edge;
        const float dy = (vertex.z - row * cell_edge) / cell_edge;
        const float a = at(column, row);
        const float b = at(column + 1, row);
        const float c = at(column + 1, row + 1);
        const float d = at(column, row + 1);
        const float height =
            dx >= dy ? a + dx * (b - a) + dy * (c - b) : a + dy * (d - a) + dx * (c - d);
        OA_CHECK(std::abs(vertex.height - height - 0.05F) < tolerance);
    }
    fixture.draws.sprites[0].terrain_decal = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 6);
    const float z = fixture.scene.vertices[fixture.scene.terrain_vertex_count].z;
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size(); ++i)
        OA_CHECK(fixture.scene.vertices[i].z == z);
}

/// What lies on the sea bed moves with the swells the vertex stage moves
/// the ground by, and a feature's top standing out of the water stays.
void sea_bed_follows_swells() {
    Fixture fixture;
    fixture.inputs.sea_level = 32;
    fixture.inputs.water.wind.strength = 1;
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 24;
    frame.pixels.assign(frame.width * frame.height, ground_index);
    frame.coverage.assign(frame.width * frame.height, 1);
    fixture.draws.sprites.push_back({&frame, {4, 4}, false, true, true});
    fixture.draws.sprites.back().world_position = oa::FixedVec3{16 * 65536, 0, 16 * 65536};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto still = fixture.scene.vertices;
    fixture.inputs.water.ripples = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == still.size());
    bool moved = false;
    for (std::size_t i = 0; i < std::min(still.size(), fixture.scene.vertices.size()); ++i) {
        const auto& before = still[i];
        const auto& after = fixture.scene.vertices[i];
        if (i < fixture.scene.terrain_vertex_count) {
            // The ground itself moves in the vertex stage.
            OA_CHECK(after.x == before.x && after.z == before.z);
            continue;
        }
        const float depth = 32 - before.height;
        const auto offset = oa::app::renderer_water_surface::art_offset(
            before.x, before.z + depth * 0.5F, fixture.inputs.water.wind, 0
        );
        OA_CHECK(std::abs(after.x - before.x - offset[0]) < tolerance);
        OA_CHECK(std::abs(after.z - before.z - offset[1]) < tolerance);
        moved = moved || after.x != before.x;
    }
    OA_CHECK(moved);
    // An upright feature at the sea bed keeps the top it shows above water.
    frame.origin_y = frame.height;
    fixture.draws.sprites.back().terrain_decal = false;
    fixture.draws.sprites.back().world_position = oa::FixedVec3{16 * 65536, 20 * 65536, 16 * 65536};
    fixture.inputs.water.ripples = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto upright = fixture.scene.vertices;
    fixture.inputs.water.ripples = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    bool foot_moved = false, top_stayed = false;
    for (std::size_t i = fixture.scene.terrain_vertex_count;
         i < std::min(upright.size(), fixture.scene.vertices.size());
         ++i) {
        if (upright[i].height >= 32) {
            OA_CHECK(fixture.scene.vertices[i].x == upright[i].x);
            top_stayed = true;
        } else {
            foot_moved = foot_moved || fixture.scene.vertices[i].x != upright[i].x;
        }
    }
    OA_CHECK(foot_moved && top_stayed);
    // Dry ground's features stay.
    fixture.inputs.sea_level = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto dry = fixture.scene.vertices;
    fixture.inputs.water.ripples = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == dry.size());
    for (std::size_t i = 0; i < std::min(dry.size(), fixture.scene.vertices.size()); ++i)
        OA_CHECK(fixture.scene.vertices[i].x == dry[i].x);
}

/// Repeated frame references share materials only for the current build.
void sprite_material_lifetime() {
    Fixture fixture;
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 8;
    frame.pixels.assign(64, ground_index);
    frame.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&frame, {4, 4}, false, true, true});
    fixture.draws.sprites.push_back({&frame, {20, 20}, false, true, true});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.sprite_count == 2);
    auto color = triangle_color(fixture.scene, fixture.scene.terrain_vertex_count);
    OA_CHECK(color[0] == ground_color[0]);
    frame.pixels.assign(64, alternate_index);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    color = triangle_color(fixture.scene, fixture.scene.terrain_vertex_count);
    OA_CHECK(color[0] == alternate_color[0]);
    fixture.palette[alternate_index * oa::palette_entry_bytes] = 77;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    color = triangle_color(fixture.scene, fixture.scene.terrain_vertex_count);
    OA_CHECK(color[0] == 77);
    // The same frame may be a coloured decal and a black shadow in one pass.
    fixture.draws.sprites[1].shadow = true;
    fixture.draws.shadow_level = oa::present::model::shadow_full_level;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    color = triangle_color(fixture.scene, fixture.scene.vertices.size() - 3);
    OA_CHECK(color[0] == 0 && color[1] == 0 && color[2] == 0);
}

/// Cropped ground sprites retain overlapping cell edges and ignore wholly unseen art.
void cropped_ground_sprites() {
    Fixture fixture;
    fixture.inputs.left = fixture.inputs.top = 17;
    fixture.inputs.right = fixture.inputs.bottom = 31;
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 8;
    frame.pixels.assign(64, alternate_index);
    frame.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&frame, {12, 12}, false, true, true});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.sprite_count == 1);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        OA_CHECK(vertex.x >= 16 && vertex.x <= 20);
        const float painted_y = vertex.z - vertex.height * 0.5F;
        OA_CHECK(painted_y >= 16 - 0.026F && painted_y <= 20 + tolerance);
    }
    fixture.draws.sprites[0].place = {0, 0};
    // An off-crop frame is rejected before material decoding or hashing.
    frame.pixels.clear();
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.sprite_count == 0);
    fixture.draws.sprites[0].place = {20, 20};
    fixture.mapped[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.sprite_count == 0);
}

/// Foundations level slopes at canonical placement height without moving models or map data.
void building_foundations() {
    Fixture fixture;
    fixture.map.attribute_width = fixture.map.attribute_height = 8;
    fixture.map.tile_width = fixture.map.tile_height = 4;
    fixture.map.tile_indices.assign(16, 0);
    fixture.map.attributes.resize(64);
    fixture.inputs.right = fixture.inputs.bottom = 128;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    for (std::size_t i = 0; i < fixture.map.attributes.size(); ++i)
        fixture.map.attributes[i].height = static_cast<uint8_t>((i % 8) * 4);
    const auto original = fixture.map.attributes;
    constexpr int32_t fixed = 65536;
    std::array<oa::Unit, 2> units{};
    units[0].flags = units[1].flags = OA_UNIT_FLAG_BUILDING;
    units[0].footprint_x = units[0].footprint_z = 2;
    units[1].footprint_x = units[1].footprint_z = 2;
    units[0].position = {48 * fixed, 16 * fixed, 48 * fixed};
    units[1].position = {80 * fixed, 32 * fixed, 48 * fixed};
    units[0].build_remaining = 0.75F;
    fixture.draws.models.resize(2);
    for (std::size_t i = 0; i < units.size(); ++i) {
        fixture.draws.models[i].model.unit = &units[i];
        fixture.draws.models[i].plan.linked.drawn = true;
    }
    oa::formats::gaf::RenderedFrame decal{};
    decal.width = decal.height = 8;
    decal.pixels.assign(64, ground_index);
    decal.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&decal, {44, 44}, false, true, true});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    bool first_interior{}, second_interior{}, far_unchanged{};
    for (std::size_t i = 0; i < fixture.scene.vertices.size(); ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        const float y = vertex.z;
        const float offset = i < fixture.scene.terrain_vertex_count ? -0.1F : -0.05F;
        if (vertex.x > 32 + tolerance && vertex.x < 64 - tolerance && y > 32 + tolerance &&
            y < 64 - tolerance) {
            first_interior = true;
            OA_CHECK(std::abs(vertex.height - (16 + offset)) < tolerance);
        }
        if (vertex.x > 64 + tolerance && vertex.x < 96 - tolerance && y > 32 + tolerance &&
            y < 64 - tolerance) {
            second_interior = true;
            OA_CHECK(std::abs(vertex.height - (32 + offset)) < tolerance);
        }
        if (vertex.x == 0) {
            far_unchanged = true;
            OA_CHECK(vertex.height == 0);
        }
    }
    for (std::size_t i = 0; i + 2 < fixture.scene.terrain_vertex_count; i += 3) {
        float x{}, y{};
        for (std::size_t corner = i; corner < i + 3; ++corner) {
            const auto& vertex = fixture.scene.vertices[corner];
            x += vertex.x / 3;
            y += (vertex.z) / 3;
        }
        if (x > 32 && x < 96 && y > 32 && y < 64 && std::abs(x - 64) > tolerance) {
            const float expected = x < 64 ? 15.9F : 31.9F;
            for (std::size_t corner = i; corner < i + 3; ++corner)
                OA_CHECK(std::abs(fixture.scene.vertices[corner].height - expected) < tolerance);
        }
    }
    OA_CHECK(first_interior && second_interior && far_unchanged);
    OA_CHECK(units[0].position.y == 16 * fixed && units[1].position.y == 32 * fixed);
    for (std::size_t i = 0; i < original.size(); ++i)
        OA_CHECK(original[i].height == fixture.map.attributes[i].height);
    const auto forward = fixture.scene.vertices;
    const auto terrain_count = fixture.scene.terrain_vertex_count;
    std::reverse(fixture.draws.models.begin(), fixture.draws.models.end());
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.terrain_vertex_count == terrain_count);
    OA_CHECK(fixture.scene.vertices.size() == forward.size());
    for (std::size_t i = 0; i < std::min(forward.size(), fixture.scene.vertices.size()); ++i) {
        OA_CHECK(fixture.scene.vertices[i].height == forward[i].height);
        OA_CHECK(fixture.scene.vertices[i].u == forward[i].u);
        OA_CHECK(fixture.scene.vertices[i].v == forward[i].v);
    }
    // Mobile units and feature stand-ins must not alter the rendered terrain.
    units[0].flags = units[1].flags = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto unlevelled = fixture.scene.vertices;
    units[0].flags = units[1].flags = OA_UNIT_FLAG_BUILDING;
    fixture.draws.models[0].stand_in = fixture.draws.models[1].stand_in = 0;
    fixture.draws.models[0].on_map = fixture.draws.models[1].on_map = true;
    fixture.draws.stand_ins.push_back(units[0]);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == unlevelled.size());
    for (std::size_t i = 0; i < std::min(unlevelled.size(), fixture.scene.vertices.size()); ++i)
        OA_CHECK(fixture.scene.vertices[i].height == unlevelled[i].height);
    // A legal placement preview has a canonical position and footprint, but
    // uses a stand-in record rather than a live unit slot.
    fixture.draws.models.resize(1);
    fixture.draws.models[0].on_map = false;
    fixture.draws.models[0].model.unit = nullptr;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    bool preview_levelled{};
    for (std::size_t i = 0; i < fixture.scene.terrain_vertex_count; ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        const float y = vertex.z;
        if (vertex.x > 32 && vertex.x < 64 && y > 32 && y < 64) {
            preview_levelled = true;
            OA_CHECK(std::abs(vertex.height - 15.9F) < tolerance);
        }
    }
    OA_CHECK(preview_levelled);
    // Invalid preview references never read an absent stand-in record.
    fixture.draws.models[0].stand_in = 99;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == unlevelled.size());
    // A flat, coplanar plot still gains depth separation below its yard plate.
    fixture.draws.models[0].stand_in = -1;
    fixture.draws.models[0].model.unit = &units[0];
    fixture.draws.sprites.clear();
    for (auto& attribute : fixture.map.attributes)
        attribute.height = 16;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    for (const auto& vertex : fixture.scene.vertices) {
        const float y = vertex.z;
        if (vertex.x >= 32 && vertex.x <= 64 && y >= 32 && y <= 64)
            OA_CHECK(std::abs(vertex.height - 15.9F) < tolerance);
    }
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 5 * fixed, 0}, {fixed, 5 * fixed, 0}, {0, 5 * fixed, fixed}};
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    drawing::PreparedModel prepared{};
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(1);
    prepared.objects[0].primitives[0].flags = drawing::primitive_colored;
    prepared.objects[0].primitives[0].color = ground_index;
    fixture.draws.models[0].model.instance = &instance;
    fixture.draws.models[0].model.prepared = &prepared;
    units[0].build_remaining = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 3);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size(); ++i)
        OA_CHECK(fixture.scene.vertices[i].height == 21);
    oa::UnitDef definition{};
    definition.flags = OA_UNIT_DEF_FLAG_FLOATER;
    fixture.draws.models[0].model.def = &definition;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    for (std::size_t i = 0; i < fixture.scene.terrain_vertex_count; ++i)
        OA_CHECK(fixture.scene.vertices[i].height == 16);
}

/// The kept ground is cut once for a set of plots: a frame with the plots
/// of the frame before takes the cut ground and its revision and cuts only
/// its decals, and other plots cut it again. Every frame is the one a scene
/// cutting the ground each frame builds.
void foundations_cut_once() {
    Fixture fixture;
    fixture.map.attribute_width = fixture.map.attribute_height = 8;
    fixture.map.tile_width = fixture.map.tile_height = 4;
    fixture.map.tile_indices.assign(16, 0);
    fixture.map.attributes.resize(64);
    fixture.inputs.right = fixture.inputs.bottom = 128;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    for (std::size_t i = 0; i < fixture.map.attributes.size(); ++i)
        fixture.map.attributes[i].height = static_cast<uint8_t>((i % 8) * 4);
    constexpr int32_t fixed = 65536;
    oa::Unit unit{};
    unit.flags = OA_UNIT_FLAG_BUILDING;
    unit.footprint_x = unit.footprint_z = 2;
    unit.position = {48 * fixed, 16 * fixed, 48 * fixed};
    fixture.draws.models.resize(1);
    fixture.draws.models[0].model.unit = &unit;
    fixture.draws.models[0].plan.linked.drawn = true;
    oa::formats::gaf::RenderedFrame decal{};
    decal.width = decal.height = 8;
    decal.pixels.assign(64, ground_index);
    decal.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&decal, {44, 44}, false, true, true});
    live::Scene cutting{};
    const auto build = [&] {
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        cutting.cut_source = 0;
        OA_CHECK(live::build(fixture.inputs, cutting, fixture.error));
        const auto& vertices = fixture.scene.vertices;
        OA_CHECK(fixture.scene.terrain_vertex_count == cutting.terrain_vertex_count);
        OA_CHECK(
            vertices.size() == cutting.vertices.size() &&
            std::memcmp(
                vertices.data(), cutting.vertices.data(), vertices.size() * sizeof(vertices.front())
            ) == 0
        );
        return fixture.scene.ground_revision;
    };
    build();
    const auto first = build();
    OA_CHECK(fixture.scene.ground_kept && fixture.scene.cut_source != 0);
    OA_CHECK(first != fixture.scene.kept_ground_revision);
    OA_CHECK(build() == first);
    // A decal moves over the plot; the plots stay.
    fixture.draws.sprites[0].place = {52, 40};
    OA_CHECK(build() == first);
    // The structure moves: its new plot is cut once.
    unit.position.x = 64 * fixed;
    const auto moved = build();
    OA_CHECK(moved != first);
    OA_CHECK(build() == moved);
    // No plot leaves the kept ground as it is; the same plot again takes
    // the ground cut for it.
    unit.flags = 0;
    OA_CHECK(build() == fixture.scene.kept_ground_revision);
    unit.flags = OA_UNIT_FLAG_BUILDING;
    OA_CHECK(build() == moved);
    // Ground drawn again is cut again.
    fixture.map.attributes[0].height = 2;
    const auto drawn = build();
    OA_CHECK(drawn != moved);
    OA_CHECK(build() != drawn);
    const auto kept = fixture.scene.ground_revision;
    OA_CHECK(build() == kept);
}

/// Candidate bins retain structures across bin edges and bound oversized plot indexing.
void foundation_candidates() {
    Fixture fixture;
    fixture.map.attribute_width = fixture.map.attribute_height = 32;
    fixture.map.tile_width = fixture.map.tile_height = 16;
    fixture.map.attributes.resize(32 * 32);
    fixture.map.tile_indices.assign(16 * 16, 0);
    for (auto& attribute : fixture.map.attributes)
        attribute.height = 24;
    fixture.inputs.right = fixture.inputs.bottom = 512;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    constexpr int32_t fixed = 65536;
    oa::Unit unit{};
    unit.flags = OA_UNIT_FLAG_BUILDING;
    unit.footprint_x = unit.footprint_z = 2;
    unit.position = {128 * fixed, 16 * fixed, 136 * fixed};
    fixture.draws.models.resize(1);
    fixture.draws.models[0].model.unit = &unit;
    fixture.draws.models[0].plan.linked.drawn = true;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    bool left{}, right{};
    for (const auto& vertex : fixture.scene.vertices) {
        const float y = vertex.z - vertex.height * 0.5F;
        if (vertex.x >= 112 && vertex.x <= 144 && y >= 112 && y <= 144) {
            OA_CHECK(std::abs(vertex.height - 15.9F) < tolerance);
            left = left || vertex.x < 128;
            right = right || vertex.x > 128;
        }
        if (vertex.x >= 176)
            OA_CHECK(vertex.height == 24);
    }
    OA_CHECK(left && right);
    unit.footprint_x = unit.footprint_z = 64;
    unit.position = {256 * fixed, 16 * fixed, 264 * fixed};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    for (const auto& vertex : fixture.scene.vertices)
        OA_CHECK(std::abs(vertex.height - 15.9F) < tolerance);
}

/// A structure on ground that rises under its footprint keeps the whole of
/// its base piece above the ground drawn under it, as the aircraft plant's
/// does: a floor plate a little below the structure's height, reaching past
/// the footprint on the high side and the low side, lies on the ground with
/// no gap at every point. A piece sunk below the ground stays under it, a
/// canopy of the base high above the ground past the footprint leaves the
/// ground under it as it is, and a base piece reaching deeper than a map
/// pixel keeps the ground a map pixel below the structure. The map and the
/// structure's place do not change.
void structure_base_above_ground() {
    Fixture fixture;
    constexpr int32_t cells = 16;
    fixture.map.attribute_width = fixture.map.attribute_height = cells;
    fixture.map.tile_width = fixture.map.tile_height = cells / 2;
    fixture.map.tile_indices.assign(cells * cells / 4, 0);
    fixture.map.attributes.resize(cells * cells);
    fixture.inputs.right = fixture.inputs.bottom = cells * cell_edge;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    // The ground rises four steps a cell eastward, from 0 to 60.
    constexpr int32_t rise = 4;
    for (std::size_t i = 0; i < fixture.map.attributes.size(); ++i)
        fixture.map.attributes[i].height = static_cast<uint8_t>((i % cells) * rise);
    const auto original = fixture.map.attributes;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    // A footprint of four by four cells from cell 4, 4, map pixels 64 to 128:
    // the structure stands at its lowest corner's height, 16, as the game
    // places it, and the ground under it rises to 32.
    constexpr float centre = 96;
    constexpr float height = 16;
    oa::Unit unit{};
    unit.id = 1;
    unit.flags = OA_UNIT_FLAG_BUILDING;
    unit.footprint_x = unit.footprint_z = 4;
    unit.position = {96 * fixed, 16 * fixed, 96 * fixed};
    // The base piece is a floor plate 0.18 below the structure's height,
    // 72 map pixels wide, 4 past the footprint on each side, and 60 deep,
    // and a canopy 12 above it that reaches 28 past the footprint on one
    // side. A hatch hangs from it, sunk 3 below.
    const auto plate = static_cast<int32_t>(-0.18F * fixed);
    constexpr int32_t half_width = 36;
    constexpr int32_t half_depth = 30;
    constexpr int32_t canopy_height = 12;
    constexpr int32_t canopy_reach = 60;
    constexpr int32_t hatch_edge = 8;
    constexpr float hatch_depth = 3;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(2);
    auto& root = source->objects[0];
    root.vertices = {
        {-half_width * fixed, plate, -half_depth * fixed},
        {half_width * fixed, plate, -half_depth * fixed},
        {half_width * fixed, plate, half_depth * fixed},
        {-half_width * fixed, plate, half_depth * fixed},
        {0, canopy_height * fixed, 0},
        {canopy_reach * fixed, canopy_height * fixed, 0},
        {canopy_reach * fixed, canopy_height * fixed, half_depth * fixed}
    };
    root.primitives.resize(2);
    root.primitives[0].vertex_indices = {0, 1, 2, 3};
    root.primitives[1].vertex_indices = {4, 5, 6};
    root.first_child = 1;
    auto& hatch = source->objects[1];
    hatch.parent = 0;
    hatch.offset_from_parent = {0, static_cast<int32_t>(-hatch_depth * fixed), 0};
    hatch.vertices = {
        {-hatch_edge * fixed, 0, -hatch_edge * fixed},
        {hatch_edge * fixed, 0, -hatch_edge * fixed},
        {hatch_edge * fixed, 0, hatch_edge * fixed},
        {-hatch_edge * fixed, 0, hatch_edge * fixed}
    };
    hatch.primitives.resize(1);
    hatch.primitives[0].vertex_indices = {0, 1, 2, 3};
    auto instance = oa::sim::model_runtime::make_instance(source);
    OA_CHECK(instance.pieces().size() == 2);
    instance.rebuild_transforms();
    drawing::PreparedModel prepared{};
    prepared.objects.resize(2);
    for (std::size_t object = 0; object < prepared.objects.size(); ++object) {
        auto& primitives = prepared.objects[object].primitives;
        primitives.resize(source->objects[object].primitives.size());
        for (std::size_t face = 0; face < primitives.size(); ++face) {
            primitives[face].source_index = static_cast<uint32_t>(face);
            primitives[face].flags = drawing::primitive_colored;
            primitives[face].color = ground_index;
        }
    }
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.instance = &instance;
    draw.model.prepared = &prepared;
    draw.model.unit = &unit;
    draw.plan.linked.drawn = true;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    const float plate_height = height + static_cast<float>(plate) / fixed;
    std::size_t plate_corners{}, canopy_corners{}, hatch_corners{};
    const auto& range = fixture.scene.unit_ranges.at(0);
    for (std::size_t i = range.first_vertex; i < range.first_vertex + range.vertex_count; ++i) {
        const auto& vertex = fixture.scene.vertices.at(i);
        const auto ground = drawn_ground(fixture.scene, vertex.x, vertex.z);
        OA_CHECK(ground.has_value());
        if (!ground)
            continue;
        if (std::abs(vertex.height - plate_height) < tolerance) {
            ++plate_corners;
            OA_CHECK(*ground < vertex.height);
        } else if (std::abs(vertex.height - (height + canopy_height)) < tolerance) {
            ++canopy_corners;
        } else {
            ++hatch_corners;
            OA_CHECK(std::abs(vertex.height - (height - hatch_depth)) < tolerance);
            OA_CHECK(*ground > vertex.height);
        }
    }
    OA_CHECK(plate_corners == 6 && canopy_corners == 3 && hatch_corners == 6);
    // Every point of the plate, inside the footprint and past it on the
    // rising east and the falling west, is above the ground, and the ground
    // lies under it with no gap the eye could see.
    constexpr float step = 2;
    constexpr float no_gap = 0.5F;
    std::size_t sampled{};
    for (float x = centre - half_width; x <= centre + half_width; x += step)
        for (float z = centre - half_depth; z <= centre + half_depth; z += step) {
            const auto ground = drawn_ground(fixture.scene, x, z);
            OA_CHECK(ground.has_value());
            if (!ground)
                continue;
            OA_CHECK(*ground < plate_height);
            OA_CHECK(*ground > plate_height - no_gap);
            ++sampled;
        }
    OA_CHECK(sampled > 1000);
    // A cell past the plate's sides, the ground is the map's again, under
    // the canopy too.
    const float east = centre + half_width + cell_edge;
    const auto east_ground = drawn_ground(fixture.scene, east, centre);
    OA_CHECK(east_ground && std::abs(*east_ground - east / cell_edge * rise) < tolerance);
    const float west = centre - half_width - cell_edge;
    constexpr float under_canopy = centre + 8;
    const auto west_ground = drawn_ground(fixture.scene, west, under_canopy);
    OA_CHECK(west_ground && std::abs(*west_ground - west / cell_edge * rise) < tolerance);
    for (std::size_t i = 0; i < original.size(); ++i)
        OA_CHECK(original[i].height == fixture.map.attributes[i].height);
    OA_CHECK(unit.position.y == 16 * fixed);
    // A base piece reaching 3 below keeps the ground a map pixel below the
    // structure, over it still.
    for (auto& corner : instance.pieces()[instance.root_piece()].transformed_vertices)
        corner.y = static_cast<int32_t>(-hatch_depth * fixed);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto deep_ground = drawn_ground(fixture.scene, centre, centre - half_depth / 2.0F);
    OA_CHECK(deep_ground && std::abs(*deep_ground - (height - 1 - 0.1F)) < tolerance);
}

/// Flash shade rows become translucent ground lighting; ordinary art keeps its colours.
void flash_masks_and_effect_altitude() {
    Fixture fixture;
    for (auto& attribute : fixture.map.attributes)
        attribute.height = 0;
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = 4;
    frame.height = 4;
    frame.transparency_index = 255;
    frame.coverage.assign(16, 1);
    for (int row = 0; row < 4; ++row)
        frame.pixels.insert(frame.pixels.end(), {79, 80, 110, 255});
    fixture.draws.sprites.push_back({&frame, {4, 4}});
    fixture.draws.draws.push_back({oa::app::WorldDrawKind::lit_sprite, 0});
    fixture.inputs.nanolathe_lighting = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.sprite_count == 1);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size(); ++i)
        OA_CHECK(std::abs(fixture.scene.vertices[i].height - 0.05F) < tolerance);
    const auto alpha = [&](int column) {
        for (const auto& [key, rect] : fixture.scene.materials)
            if (rect.width == 4 && rect.height == 4) {
                const auto at = ((rect.y * fixture.scene.texture_width) + rect.x + column) * 4;
                OA_CHECK(fixture.scene.texture_rgba[at] == 255);
                return fixture.scene.texture_rgba[at + 3];
            }
        OA_CHECK(false);
        return uint8_t{};
    };
    OA_CHECK(alpha(0) == 0);
    OA_CHECK(alpha(1) > 0);
    OA_CHECK(alpha(2) > alpha(1) && alpha(2) <= 141);
    OA_CHECK(alpha(3) == 0);
    const auto full = alpha(2);
    fixture.draws.flash_strength = oa::app::FlashStrength::reduced;
    fixture.scene = {};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(alpha(2) > 0 && alpha(2) < full);
    // The same bytes in an ordinary sprite remain palette indices, at the true effect height.
    fixture.draws.draws[0].kind = oa::app::WorldDrawKind::sprite;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    fixture.draws.sprites[0].world_position = oa::FixedVec3{8 * fixed, 20 * fixed, 25 * fixed};
    fixture.scene = {};
    fixture.palette[110 * oa::palette_entry_bytes] = 17;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 6);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        OA_CHECK(vertex.height >= 18 && vertex.height <= 20);
        OA_CHECK(std::abs((vertex.height - 20) + 0.5F * (vertex.z - 25)) < tolerance);
    }
    // Compact effect art keeps the same world anchor and uses filtering only on its own vertices.
    fixture.draws.sprites[0].effect_scale = 0.5F;
    fixture.draws.sprites[0].effect_opacity = 0.4F;
    fixture.draws.sprites[0].soft_effect = true;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        OA_CHECK(vertex.height >= 19 && vertex.height <= 20);
        OA_CHECK(std::abs(vertex.opacity - 0.4F) < tolerance);
        OA_CHECK(vertex.terrain_size == -1);
    }
    fixture.draws.sprites[0].effect_scale = 1;
    fixture.draws.sprites[0].effect_opacity = 1;
    fixture.draws.sprites[0].soft_effect = false;
    fixture.inputs.view_direction = {0.5F, 1, 0};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    for (std::size_t index = fixture.scene.terrain_vertex_count;
         index < fixture.scene.vertices.size();
         ++index) {
        const auto& vertex = fixture.scene.vertices[index];
        OA_CHECK(std::abs(0.5F * (vertex.x - 8) + (vertex.height - 20)) < tolerance);
    }
    for (const auto& [key, rect] : fixture.scene.materials)
        if (rect.width == 4 && rect.height == 4) {
            const auto at = ((rect.y * fixture.scene.texture_width) + rect.x + 2) * 4;
            OA_CHECK(fixture.scene.texture_rgba[at] == 17);
            OA_CHECK(fixture.scene.texture_rgba[at + 3] == 255);
        }
}

/// Ground explosions clear terrain crests; airborne effects and projectiles retain altitude.
void surface_explosions() {
    Fixture fixture;
    constexpr int32_t fixed = 65536;
    for (auto& attribute : fixture.map.attributes)
        attribute.height = 0;
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 8;
    frame.origin_x = frame.origin_y = 4;
    frame.pixels.assign(64, ground_index);
    frame.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&frame, {16, 16}});
    auto& sprite = fixture.draws.sprites[0];
    sprite.world_position = oa::FixedVec3{16 * fixed, 0, 16 * fixed};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto before = fixture.scene.vertices;
    sprite.surface_effect = true;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto first = fixture.scene.terrain_vertex_count;
    OA_CHECK(fixture.scene.vertices.size() == first + 6);
    const float lift = fixture.scene.vertices[first].height - before[first].height;
    OA_CHECK(lift > 0);
    for (std::size_t i = first; i < fixture.scene.vertices.size(); ++i) {
        const auto& v = fixture.scene.vertices[i];
        OA_CHECK(std::abs(v.height - before[i].height - lift) < tolerance);
        OA_CHECK(v.height >= 0.1F - tolerance);
        OA_CHECK(v.x == before[i].x && v.z == before[i].z);
        OA_CHECK(v.u == before[i].u && v.v == before[i].v);
    }
    // Transparent padding must not determine the explosion's visible bottom.
    for (std::size_t i = 56; i < 64; ++i)
        frame.coverage[i] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[first].height - before[first].height < lift);
    frame.coverage.assign(64, 1);
    sprite.world_position->y = 20 * fixed;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    for (std::size_t i = first; i < fixture.scene.vertices.size(); ++i)
        OA_CHECK(std::abs(fixture.scene.vertices[i].height - before[i].height - 20) < tolerance);
    const auto clear_terrain = [&] {
        const auto start = fixture.scene.terrain_vertex_count;
        const auto& a = fixture.scene.vertices[start];
        const auto& b = fixture.scene.vertices[start + 1];
        const auto& c = fixture.scene.vertices[start + 5];
        int checked{};
        for (int row = 0; row <= 4; ++row)
            for (int column = 0; column <= 4; ++column) {
                const float u = column / 4.0F, v = row / 4.0F;
                const float x = a.x + u * (b.x - a.x) + v * (c.x - a.x);
                const float z = a.z + u * (b.z - a.z) + v * (c.z - a.z);
                const float h = a.height + u * (b.height - a.height) + v * (c.height - a.height);
                for (std::size_t i = 0; i + 2 < start; i += 3) {
                    const auto& p = fixture.scene.vertices[i];
                    const auto& q = fixture.scene.vertices[i + 1];
                    const auto& r = fixture.scene.vertices[i + 2];
                    const float determinant = (q.z - r.z) * (p.x - r.x) + (r.x - q.x) * (p.z - r.z);
                    if (std::abs(determinant) < tolerance)
                        continue;
                    const float s =
                        ((q.z - r.z) * (x - r.x) + (r.x - q.x) * (z - r.z)) / determinant;
                    const float t =
                        ((r.z - p.z) * (x - r.x) + (p.x - r.x) * (z - r.z)) / determinant;
                    if (s < -tolerance || t < -tolerance || s + t > 1 + tolerance)
                        continue;
                    OA_CHECK(
                        h >= s * p.height + t * q.height + (1 - s - t) * r.height + 0.1F - tolerance
                    );
                    ++checked;
                }
            }
        OA_CHECK(checked >= 25);
    };
    // A slope crossing the billboard's interior must also clear the whole card.
    fixture.map.attributes[1].height = fixture.map.attributes[3].height = 8;
    sprite.world_position = oa::FixedVec3{16 * fixed, 8 * fixed, 20 * fixed};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    clear_terrain();
    // Clearance uses the leveled presentation foundation, not the original map.
    oa::Unit unit{};
    unit.flags = OA_UNIT_FLAG_BUILDING;
    unit.footprint_x = unit.footprint_z = 1;
    unit.position = {16 * fixed, 4 * fixed, 18 * fixed};
    fixture.draws.models.resize(1);
    fixture.draws.models[0].model.unit = &unit;
    fixture.draws.models[0].plan.linked.drawn = true;
    sprite.world_position = unit.position;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    clear_terrain();
}

/// Shadows preserve the cached image's coverage and obey both shadow settings.
void cached_unit_shadows() {
    Fixture fixture;
    oa::present::model::ModelState state{};
    state.image = oa::present::create_sprite(8, 8);
    state.image.sprite.key = 1;
    std::fill(state.image.pixels.begin(), state.image.pixels.end(), 1);
    state.image.pixels[0] = 0;
    state.image.pixels[8] = 0;
    oa::Unit unit{};
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    unit.position = {3 * fixed, 0, 8 * fixed};
    oa::UnitDef definition{};
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.unit = &unit;
    draw.model.state = &state;
    draw.model.def = &definition;
    draw.plan.linked.drawn = true;
    draw.plan.model.from_image = true;
    fixture.inputs.graphics_flags =
        oa::present::model::graphics_shadows | oa::present::model::graphics_vehicle_shadows;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.shadow_count == 1);
    OA_CHECK(fixture.scene.vertices.size() > fixture.scene.terrain_vertex_count);
    const auto& corner = fixture.scene.vertices.at(fixture.scene.terrain_vertex_count);
    OA_CHECK(corner.opacity == 0.5F);
    const auto atlas_x = static_cast<uint32_t>(std::lround(corner.u * fixture.scene.texture_width));
    const auto atlas_y =
        static_cast<uint32_t>(std::lround(corner.v * fixture.scene.texture_height));
    const auto offset =
        (static_cast<std::size_t>(atlas_y) * fixture.scene.texture_width + atlas_x) * 4;
    OA_CHECK(fixture.scene.texture_rgba.at(offset + 3) == 255);
    OA_CHECK(fixture.scene.texture_rgba.at(offset + 7) == 0);
    fixture.inputs.graphics_flags = oa::present::model::graphics_shadows;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.shadow_count == 0);
    fixture.inputs.graphics_flags |= oa::present::model::graphics_vehicle_shadows;
    definition.flags = OA_UNIT_DEF_FLAG_NO_SHADOW;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.shadow_count == 0);
    definition.flags = 0;
    unit.flags = OA_UNIT_FLAG_BUILDING;
    fixture.inputs.graphics_flags = oa::present::model::graphics_shadows;
    oa::present::RleEncoder encoder{};
    const int32_t bytes = oa::present::encode_rle_sprite(encoder, nullptr, state.image.sprite);
    OA_CHECK(bytes > 0);
    state.shadow.pixels.resize(static_cast<std::size_t>(bytes));
    oa::present::encode_rle_sprite(encoder, state.shadow.pixels.data(), state.image.sprite);
    state.shadow.sprite = state.image.sprite;
    state.shadow.sprite.encoding = OA_SPRITE_ROW_RLE;
    state.shadow.sprite.data = state.shadow.pixels.data();
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.shadow_count == 1);
    fixture.draws.shadow_level = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.shadow_count == 0);
}

/// A unit's shadow is packed into the rows kept for the frame's own
/// materials, at the atlas's foot, and only those rows are sent; a frame
/// that casts the same shadow again finds it there and changes nothing,
/// and a changed shadow leaves the rest of the atlas alone.
void frame_rows_hold_unit_shadows() {
    Fixture fixture;
    oa::present::model::ModelState state{};
    state.image = oa::present::create_sprite(8, 8);
    state.image.sprite.key = 1;
    std::fill(state.image.pixels.begin(), state.image.pixels.end(), 1);
    state.image.pixels[0] = 0;
    oa::Unit unit{};
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    unit.position = {3 * fixed, 0, 8 * fixed};
    oa::UnitDef definition{};
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.unit = &unit;
    draw.model.state = &state;
    draw.model.def = &definition;
    draw.plan.linked.drawn = true;
    draw.plan.model.from_image = true;
    fixture.inputs.graphics_flags =
        oa::present::model::graphics_shadows | oa::present::model::graphics_vehicle_shadows;
    auto& scene = fixture.scene;
    scene.texture_width = scene.texture_height = 4096;
    scene.frame_rows = 64;
    const uint32_t first_frame_row = scene.texture_height - scene.frame_rows;
    OA_CHECK(live::build(fixture.inputs, scene, fixture.error));
    OA_CHECK(scene.shadow_count == 1);
    OA_CHECK(scene.frame.materials.size() == 1);
    OA_CHECK(scene.frame.materials.begin()->second.y == first_frame_row);
    // The shadow's rows, with the row on either side.
    OA_CHECK(scene.atlas_update.frame_first_row == first_frame_row - 1);
    OA_CHECK(scene.atlas_update.frame_end_row == first_frame_row + 9);
    OA_CHECK(scene.atlas_update.end_row < first_frame_row);
    const auto& corner = scene.vertices.at(scene.terrain_vertex_count);
    OA_CHECK(std::lround(corner.v * scene.texture_height) >= static_cast<long>(first_frame_row));
    const auto revision = scene.texture_revision;
    const auto materials = scene.materials.size();
    OA_CHECK(live::build(fixture.inputs, scene, fixture.error));
    OA_CHECK(scene.texture_revision == revision);
    OA_CHECK(scene.atlas_update.frame_first_row == scene.atlas_update.frame_end_row);
    state.image.pixels[1] = 0;
    OA_CHECK(live::build(fixture.inputs, scene, fixture.error));
    OA_CHECK(scene.texture_revision > revision);
    OA_CHECK(scene.materials.size() == materials);
    OA_CHECK(scene.atlas_update.frame_first_row == first_frame_row - 1);
    OA_CHECK(scene.atlas_update.first_row == scene.atlas_update.end_row);
    // The shadow's first row: its first two pixels covered now, the third clear.
    const auto offset = static_cast<std::size_t>(first_frame_row) * scene.texture_width * 4;
    OA_CHECK(scene.texture_rgba.at(offset + 3) == 255);
    OA_CHECK(scene.texture_rgba.at(offset + 7) == 255);
    OA_CHECK(scene.texture_rgba.at(offset + 11) == 0);
}

/// A unit's shadow taller than the rows kept for the frame's own materials
/// is packed with the other materials, and those rows grow from the foot of
/// the others' to hold it: the next frame packs it there.
void frame_rows_grow_for_spilled_shadows() {
    Fixture fixture;
    oa::present::model::ModelState state{};
    state.image = oa::present::create_sprite(8, 24);
    state.image.sprite.key = 1;
    std::fill(state.image.pixels.begin(), state.image.pixels.end(), 1);
    oa::Unit unit{};
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    unit.position = {3 * fixed, 0, 8 * fixed};
    oa::UnitDef definition{};
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.unit = &unit;
    draw.model.state = &state;
    draw.model.def = &definition;
    draw.plan.linked.drawn = true;
    draw.plan.model.from_image = true;
    fixture.inputs.graphics_flags =
        oa::present::model::graphics_shadows | oa::present::model::graphics_vehicle_shadows;
    auto& scene = fixture.scene;
    scene.texture_width = scene.texture_height = 4096;
    scene.frame_rows = 16;
    OA_CHECK(live::build(fixture.inputs, scene, fixture.error));
    OA_CHECK(scene.shadow_count == 1);
    OA_CHECK(scene.frame.materials.empty());
    OA_CHECK(scene.frame_rows == 32);
    const auto materials = scene.materials.size();
    // The shadow changed: packed afresh, in the rows grown.
    state.image.pixels[0] = 0;
    OA_CHECK(live::build(fixture.inputs, scene, fixture.error));
    OA_CHECK(scene.frame.materials.size() == 1);
    OA_CHECK(scene.frame.materials.begin()->second.y == scene.texture_height - 32);
    OA_CHECK(scene.materials.size() == materials);
    OA_CHECK(scene.frame_rows == 32);
}

/// Tells whether two scenes built the same frame: the same vertices in the
/// same order, unit ranges, counts and atlas, and the same share of the
/// next kept ground sent to the card.
///
/// @param a one scene
/// @param b the other
/// @return true when they are the same
bool same_frame(const live::Scene& a, const live::Scene& b) {
    const auto same_vertices = [](const std::vector<oa::app::renderer_live_gpu::Vertex>& x,
                                  const std::vector<oa::app::renderer_live_gpu::Vertex>& y) {
        return x.size() == y.size() &&
               (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    const auto same_ranges = [](const std::vector<live::UnitRange>& x,
                                const std::vector<live::UnitRange>& y) {
        return std::equal(
            x.begin(), x.end(), y.begin(), y.end(), [](const auto& left, const auto& right) {
                return left.unit_id == right.unit_id && left.first_vertex == right.first_vertex &&
                       left.vertex_count == right.vertex_count;
            }
        );
    };
    const auto& card = a.card_ground;
    const auto& other_card = b.card_ground;
    const bool same_card_ground =
        card.vertex_count == other_card.vertex_count &&
        (card.revision == 0) == (other_card.revision == 0) &&
        (card.next_revision == 0) == (other_card.next_revision == 0) &&
        card.next_first == other_card.next_first && card.next_count == other_card.next_count &&
        card.next.size() == other_card.next.size() &&
        (card.next.empty() ||
         std::memcmp(card.next.data(), other_card.next.data(), card.next.size_bytes()) == 0);
    const auto& update = a.atlas_update;
    const auto& other = b.atlas_update;
    return same_card_ground && same_vertices(a.vertices, b.vertices) &&
           same_ranges(a.unit_ranges, b.unit_ranges) &&
           same_vertices(a.translucent_vertices, b.translucent_vertices) &&
           same_ranges(a.translucent_ranges, b.translucent_ranges) &&
           a.terrain_vertex_count == b.terrain_vertex_count && a.model_count == b.model_count &&
           a.sprite_count == b.sprite_count && a.shadow_count == b.shadow_count &&
           a.particle_count == b.particle_count && a.nano_particle_count == b.nano_particle_count &&
           a.texture_rgba == b.texture_rgba && a.texture_width == b.texture_width &&
           a.texture_height == b.texture_height && a.texture_revision == b.texture_revision &&
           update.previous_revision == other.previous_revision &&
           update.first_row == other.first_row && update.end_row == other.end_row &&
           update.frame_first_row == other.frame_first_row &&
           update.frame_end_row == other.frame_end_row &&
           update.regions.size() == other.regions.size() && a.frame_rows == b.frame_rows &&
           a.frame.order == b.frame.order && a.materials.size() == b.materials.size();
}

/// A battle's passes build the same frames on any number of drawing threads
/// as on none, frame after frame: the same vertices in the same order, unit
/// ranges, counts and atlas. Each frame brings new materials to the cards
/// on the ground, the models, the standing cards and the particles, which
/// stop those passes for the thread building the frame to finish; units
/// cast shadows, a few too tall for the rows kept for them, aircraft soft
/// ones; structures level the ground under cards and shadows, and an
/// explosion near the ground stands clear of the levelled ground.
void passes_on_threads() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr std::size_t unit_count = 72;
    constexpr std::size_t colour_count = 12;
    constexpr int32_t frames = 8;
    Fixture battle;
    auto& map = battle.map;
    map.attribute_width = map.attribute_height = 16;
    map.tile_width = map.tile_height = 8;
    map.tile_indices.assign(64, 0);
    map.attributes.resize(256);
    for (std::size_t i = 0; i < map.attributes.size(); ++i)
        map.attributes[i].height = static_cast<uint8_t>((i * 7 % 13) * 3);
    battle.inputs.right = battle.inputs.bottom = 256;
    battle.inputs.sight.mapping = battle.inputs.sight.line_of_sight = false;
    battle.inputs.graphics_flags = drawing::graphics_shadows | drawing::graphics_vehicle_shadows;
    // Every unit is one triangle, in a colour of its own each frame.
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 0, 0}, {4 * fixed, 0, 0}, {0, 4 * fixed, 0}};
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    std::array<drawing::PreparedModel, colour_count> colours{};
    for (std::size_t colour = 0; colour < colours.size(); ++colour) {
        colours[colour].objects.resize(1);
        colours[colour].objects[0].primitives.resize(1);
        colours[colour].objects[0].primitives[0].flags = drawing::primitive_colored;
        colours[colour].objects[0].primitives[0].color = static_cast<uint8_t>(100 + colour);
    }
    // Pictures of several sizes, the last two taller than the rows kept.
    std::array<drawing::ModelState, 6> pictures{};
    for (std::size_t picture = 0; picture < pictures.size(); ++picture) {
        auto& image = pictures[picture].image;
        image = oa::present::create_sprite(
            static_cast<int32_t>(6 + 3 * picture), static_cast<int32_t>(5 + 4 * picture)
        );
        image.sprite.key = 1;
        for (std::size_t pixel = 0; pixel < image.pixels.size(); ++pixel)
            image.pixels[pixel] = (pixel + picture) % 5 == 0 ? 1 : 0;
    }
    std::array<oa::UnitDef, 3> definitions{};
    definitions[1].flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    std::vector<oa::Unit> units(unit_count);
    battle.draws.models.resize(unit_count);
    for (std::size_t index = 0; index < unit_count; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.position = {
            static_cast<int32_t>(8 + index * 37 % 236) * fixed,
            0,
            static_cast<int32_t>(12 + index * 53 % 232) * fixed
        };
        auto& draw = battle.draws.models[index];
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.state = &pictures[index % pictures.size()];
        draw.model.def = &definitions[0];
        draw.plan.linked.drawn = true;
        draw.plan.model.from_image = true;
        if (index % 9 == 4) {
            // A structure, levelling the ground.
            unit.flags = OA_UNIT_FLAG_BUILDING;
            unit.footprint_x = unit.footprint_z = 2;
            unit.position.y = 20 * fixed;
        } else if (index % 11 == 5) {
            // An aircraft, its shadow soft on the ground far below.
            draw.model.def = &definitions[1];
            unit.position.y = 120 * fixed;
        } else if (index % 13 == 6) {
            unit.state_flags = OA_UNIT_STATE_CLOAKED;
        }
    }
    // Cards: on the ground, a feature's shadow, standing effects, and
    // explosions near the ground.
    std::array<oa::formats::gaf::RenderedFrame, 8> cards{};
    for (std::size_t card = 0; card < cards.size(); ++card) {
        auto& frame = cards[card];
        frame.width = static_cast<int32_t>(6 + card * 2);
        frame.height = static_cast<int32_t>(4 + card * 3);
        frame.origin_x = frame.width / 2;
        frame.origin_y = frame.height / 2;
        const auto count = static_cast<std::size_t>(frame.width) * frame.height;
        frame.pixels.assign(count, static_cast<uint8_t>(ground_index + card % 2));
        frame.coverage.assign(count, 1);
    }
    for (int32_t card = 0; card < 40; ++card) {
        oa::app::SpriteDraw sprite{};
        sprite.frame = &cards[static_cast<std::size_t>(card) % cards.size()];
        sprite.place = {16 + card * 29 % 224, 16 + card * 41 % 224};
        switch (card % 4) {
        case 0:
            sprite.terrain_decal = true;
            sprite.on_map = true;
            break;
        case 1:
            sprite.shadow = true;
            sprite.terrain_decal = true;
            break;
        case 2:
            sprite.world_position =
                oa::FixedVec3{sprite.place.x * fixed, 30 * fixed, sprite.place.y * fixed};
            break;
        default:
            sprite.world_position =
                oa::FixedVec3{sprite.place.x * fixed, 2 * fixed, sprite.place.y * fixed};
            sprite.surface_effect = true;
            break;
        }
        battle.draws.sprites.push_back(sprite);
    }
    oa::app::ProjectileDraw shot{};
    shot.object = &source->objects[0];
    shot.prepared = &colours[0].objects[0];
    shot.position = {120 * fixed, 30 * fixed, 120 * fixed};
    battle.draws.projectiles.push_back(shot);
    // The scenes: none, two, three and eight drawing threads.
    std::array<std::unique_ptr<oa::platform::job_pool::Pool>, 4> pools{};
    pools[1] = std::make_unique<oa::platform::job_pool::Pool>(2);
    pools[2] = std::make_unique<oa::platform::job_pool::Pool>(3);
    pools[3] = std::make_unique<oa::platform::job_pool::Pool>(8);
    std::array<live::Scene, 4> scenes{};
    for (auto& scene : scenes) {
        scene.texture_width = scene.texture_height = 4096;
        scene.frame_rows = 16;
    }
    for (int32_t frame = 0; frame < frames; ++frame) {
        // New colours, new pictures, new cards and new particles.
        for (std::size_t index = 0; index < unit_count; ++index) {
            battle.draws.models[index].model.prepared =
                &colours[(index + static_cast<std::size_t>(frame)) % colours.size()];
            units[index].position.x += fixed / 2;
        }
        pictures[static_cast<std::size_t>(frame) % pictures.size()].image.pixels[1] ^= 1;
        cards[static_cast<std::size_t>(frame) % cards.size()].pixels[0] =
            static_cast<uint8_t>(frame + 3);
        if (frame == 5)
            units[4].position.x += 8 * fixed;
        battle.draws.spatial_particles.clear();
        for (int32_t particle = 0; particle < 30; ++particle)
            battle.draws.spatial_particles.push_back(
                {{(20 + particle * 7) * fixed, 10 * fixed, (30 + particle * 5) * fixed},
                 {static_cast<uint8_t>(frame * 20 + particle), 90, 40},
                 particle % 3 == 0}
            );
        for (std::size_t scene = 0; scene < scenes.size(); ++scene) {
            battle.inputs.pool = pools[scene].get();
            OA_CHECK(live::build(battle.inputs, scenes[scene], battle.error));
        }
        const auto& alone = scenes[0];
        OA_CHECK(alone.shadow_count > 0);
        OA_CHECK(alone.model_count > unit_count / 2);
        OA_CHECK(alone.sprite_count > 0);
        OA_CHECK(alone.particle_count == 30);
        OA_CHECK(!alone.translucent_vertices.empty());
        OA_CHECK(!alone.unit_ranges.empty());
        OA_CHECK(!alone.frame.aircraft_shadows.empty());
        for (std::size_t scene = 1; scene < scenes.size(); ++scene)
            OA_CHECK(same_frame(alone, scenes[scene]));
    }
    // A shadow too tall for the rows kept went with the other materials,
    // and the rows grew for it.
    OA_CHECK(scenes[0].frame_rows > 16);
}

/// A camera panning steadily takes the ground drawn ahead of it on the
/// drawing threads before the kept ground's margin runs out, rather than
/// drawing it again in the frame that passes the margin: every frame's
/// ground holds the picture's own, the frames are the same on any number of
/// threads as on none, and fewer frames change the ground than the margin
/// alone would. Panning slower, so that the kept ground's margin outlasts
/// the frames the ground ahead takes to be drawn, packed, cut and sent, the
/// ground taken was sent to the card whole in the frames before, slice
/// after slice, the very vertices the frame that takes it begins with.
void ground_drawn_ahead_on_threads() {
    constexpr uint32_t across = 15;
    constexpr uint32_t picture_width = 200;
    constexpr uint32_t picture_height = 150;
    constexpr float pan_a_frame = 4;
    constexpr int frames = 40;
    constexpr float slow_pan_a_frame = 1;
    constexpr int slow_frames = 40;
    constexpr std::array<float, 12> projection{2, 0, -2, 0, 1, -1, 1, 0, 0, 1, 0, 0};
    const auto make = [&](Fixture& fixture, float pan) {
        auto& map = fixture.map;
        map.tile_width = map.tile_height = across;
        map.attribute_width = map.attribute_height = across * 2;
        map.tile_count = 3;
        map.tile_indices.resize(static_cast<std::size_t>(across) * across);
        for (std::size_t tile = 0; tile < map.tile_indices.size(); ++tile)
            map.tile_indices[tile] = static_cast<uint16_t>(tile % 3);
        map.tile_palette_indices.resize(3 * tile_edge * tile_edge);
        for (std::size_t pixel = 0; pixel < map.tile_palette_indices.size(); ++pixel)
            map.tile_palette_indices[pixel] = static_cast<uint8_t>(pixel * 7 / tile_edge % 5);
        map.attributes.assign(
            static_cast<std::size_t>(map.attribute_width) * map.attribute_height, {}
        );
        for (uint32_t row = 0; row < map.attribute_height; ++row)
            for (uint32_t column = 0; column < map.attribute_width; ++column)
                map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column]
                    .height = static_cast<uint8_t>((column * 3 + row * 5) % 40);
        fixture.inputs.sight.mapping = false;
        fixture.inputs.sight.line_of_sight = false;
        fixture.inputs.right = static_cast<int32_t>(across) * tile_edge;
        fixture.inputs.bottom = static_cast<int32_t>(across) * tile_edge;
        fixture.inputs.clip_projection = projection;
        fixture.inputs.clip_projection[3] = pan;
        fixture.inputs.clip_width = picture_width;
        fixture.inputs.clip_height = picture_height;
        fixture.inputs.tiles_hold_while_moving = true;
    };
    using Corners = std::array<float, 9>;
    const auto triangles = [](const live::Scene& scene) {
        std::vector<Corners> result;
        const auto& ground = scene.ground_kept ? scene.ground_vertices : scene.vertices;
        const std::size_t count = scene.ground_kept ? ground.size() : scene.terrain_vertex_count;
        for (std::size_t first = 0; first + 2 < count; first += 3) {
            Corners corners{};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto& vertex = ground[first + corner];
                corners[corner * 3] = vertex.x;
                corners[corner * 3 + 1] = vertex.height;
                corners[corner * 3 + 2] = vertex.z;
            }
            result.push_back(corners);
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    // The triangles a new scene draws for the picture panned so far.
    const auto pictured = [&](float pan) {
        Fixture fresh;
        make(fresh, pan);
        OA_CHECK(live::build(fresh.inputs, fresh.scene, fresh.error));
        return triangles(fresh.scene);
    };
    Fixture moving;
    make(moving, 0);
    std::vector<std::unique_ptr<oa::platform::job_pool::Pool>> pools;
    pools.push_back(nullptr);
    for (const uint32_t threads : {2U, 3U, 8U})
        pools.push_back(std::make_unique<oa::platform::job_pool::Pool>(threads));
    std::vector<live::Scene> scenes(pools.size());
    const auto build_all = [&] {
        for (std::size_t scene = 0; scene < scenes.size(); ++scene) {
            moving.inputs.pool = pools[scene].get();
            OA_CHECK(live::build(moving.inputs, scenes[scene], moving.error));
        }
        for (std::size_t scene = 1; scene < scenes.size(); ++scene)
            OA_CHECK(same_frame(scenes[0], scenes[scene]));
    };
    build_all();
    build_all();
    uint64_t revision = scenes[0].ground_revision;
    int changes = 0;
    int taken_ahead = 0;
    int taken_sent = 0;
    bool drawn_ahead = false;
    // The next kept ground as the card would have it, slice after slice.
    std::vector<oa::app::renderer_live_gpu::Vertex> sent;
    uint64_t sent_revision = 0;
    float pan = 0;
    for (int frame = 1; frame <= frames + slow_frames; ++frame) {
        pan += frame <= frames ? pan_a_frame : slow_pan_a_frame;
        moving.inputs.clip_projection[3] = pan;
        const bool ahead_done =
            scenes[0].ground_ahead.active &&
            scenes[0].ground_ahead.bands_drawn == scenes[0].ground_ahead.bands.size();
        build_all();
        const auto& scene = scenes[0];
        drawn_ahead = drawn_ahead || scene.ground_ahead.active;
        if (scene.ground_revision != revision) {
            changes += frame <= frames ? 1 : 0;
            taken_ahead += ahead_done ? 1 : 0;
            revision = scene.ground_revision;
            const auto& card = scene.card_ground;
            if (card.revision == sent_revision && sent.size() == card.vertex_count &&
                card.vertex_count <= scene.vertices.size()) {
                OA_CHECK(
                    std::memcmp(
                        sent.data(), scene.vertices.data(), sent.size() * sizeof(sent[0])
                    ) == 0
                );
                ++taken_sent;
            }
        }
        const auto& card = scene.card_ground;
        if (card.next_revision != 0) {
            if (card.next_first == 0 || card.next_revision != sent_revision) {
                sent.clear();
                sent_revision = card.next_revision;
            }
            OA_CHECK(card.next_first == sent.size());
            sent.insert(
                sent.end(),
                card.next.begin() + static_cast<std::ptrdiff_t>(card.next_first),
                card.next.begin() + static_cast<std::ptrdiff_t>(card.next_first + card.next_count)
            );
        }
        // The ground drawn, ahead or not, holds the picture's own.
        const auto drawn = triangles(scene);
        const auto own = pictured(pan);
        OA_CHECK(std::includes(drawn.begin(), drawn.end(), own.begin(), own.end()));
    }
    // The margin is a tenth of the picture's width, 20 pixels: drawn again
    // each time the picture moves past it, the ground would change every
    // five frames.
    OA_CHECK(drawn_ahead);
    OA_CHECK(taken_ahead > 0);
    OA_CHECK(taken_sent > 0);
    OA_CHECK(changes < frames / 5);
    // Standing still, the camera settles, and the ground is the picture's
    // own on every number of threads.
    for (int frame = 0; frame < 11; ++frame)
        build_all();
    OA_CHECK(!scenes[0].ground_ahead.active);
    OA_CHECK(triangles(scenes[0]) == pictured(pan));
}

/// A camera that moves on over ground it has not shown, then comes back,
/// shows the tiles it showed at the detail it showed them while the atlas
/// has room: only the ground coming into view takes the moving camera's
/// smaller squares, so every triangle of the picture it left is drawn
/// again from the same square, byte for byte.
void moving_camera_comes_back_to_its_picture() {
    constexpr uint32_t across = 24;
    constexpr uint32_t picture_width = 200;
    constexpr uint32_t picture_height = 150;
    constexpr std::array<float, 12> projection{2, 0, -2, 0, 1, -1, 1, 0, 0, 1, 0, 0};
    Fixture fixture;
    auto& map = fixture.map;
    map.tile_width = map.tile_height = across;
    map.attribute_width = map.attribute_height = across * 2;
    // Every tile its own, so that each step brings new tiles to pack.
    map.tile_count = across * across;
    map.tile_indices.resize(static_cast<std::size_t>(across) * across);
    for (std::size_t tile = 0; tile < map.tile_indices.size(); ++tile)
        map.tile_indices[tile] = static_cast<uint16_t>(tile);
    map.tile_palette_indices.resize(map.tile_indices.size() * tile_edge * tile_edge);
    for (std::size_t pixel = 0; pixel < map.tile_palette_indices.size(); ++pixel)
        map.tile_palette_indices[pixel] =
            static_cast<uint8_t>((pixel / (tile_edge * tile_edge) + pixel % 7) % 5);
    map.attributes.assign(static_cast<std::size_t>(map.attribute_width) * map.attribute_height, {});
    for (uint32_t row = 0; row < map.attribute_height; ++row)
        for (uint32_t column = 0; column < map.attribute_width; ++column)
            map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column].height =
                static_cast<uint8_t>((column * 3 + row * 5) % 40);
    auto& inputs = fixture.inputs;
    inputs.sight.mapping = false;
    inputs.sight.line_of_sight = false;
    inputs.right = static_cast<int32_t>(across) * tile_edge;
    inputs.bottom = static_cast<int32_t>(across) * tile_edge;
    inputs.clip_projection = projection;
    inputs.clip_width = picture_width;
    inputs.clip_height = picture_height;
    inputs.tiles_hold_while_moving = true;
    using Corners = std::array<float, 9>;
    using Triangle = std::array<oa::app::renderer_live_gpu::Vertex, 3>;
    // The ground's triangles by their corners.
    const auto ground = [](const live::Scene& scene) {
        std::vector<std::pair<Corners, Triangle>> result;
        const auto& vertices = scene.ground_kept ? scene.ground_vertices : scene.vertices;
        const std::size_t count = scene.ground_kept ? vertices.size() : scene.terrain_vertex_count;
        for (std::size_t first = 0; first + 2 < count; first += 3) {
            Corners corners{};
            Triangle triangle{};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                triangle[corner] = vertices[first + corner];
                corners[corner * 3] = triangle[corner].x;
                corners[corner * 3 + 1] = triangle[corner].height;
                corners[corner * 3 + 2] = triangle[corner].z;
            }
            result.emplace_back(corners, triangle);
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        return result;
    };
    const auto build = [&](float pan) {
        inputs.clip_projection[3] = pan;
        OA_CHECK(live::build(inputs, fixture.scene, fixture.error));
    };
    build(0);
    build(0);
    const auto still = ground(fixture.scene);
    const std::size_t tiles_shown = fixture.scene.terrain_materials.size();
    // Six steps along, bringing new tiles into view, and straight back.
    for (int step = 1; step <= 6; ++step)
        build(-6.0F * static_cast<float>(step));
    OA_CHECK(fixture.scene.terrain_materials.size() > tiles_shown);
    build(0);
    const auto back = ground(fixture.scene);
    std::size_t found = 0;
    std::size_t same = 0;
    for (const auto& [corners, triangle] : still) {
        const auto match = std::lower_bound(
            back.begin(), back.end(), corners, [](const auto& entry, const Corners& wanted) {
                return entry.first < wanted;
            }
        );
        if (match == back.end() || match->first != corners)
            continue;
        ++found;
        same += std::memcmp(match->second.data(), triangle.data(), sizeof(Triangle)) == 0 ? 1 : 0;
    }
    OA_CHECK(found == still.size());
    OA_CHECK(same == found);
}

/// The vertices a frame with models on the card draws: its own, with each
/// card draw's triangles where it is drawn among them, as the card gives
/// them to the fragment stage; and each vertex's unit, by the scene's
/// ranges and the card draws.
struct CardFrame {
    std::vector<oa::app::renderer_live_gpu::Vertex> vertices{};
    std::vector<uint16_t> units{};
};

/// Returns the frame a scene with models on the card draws (CardFrame).
CardFrame card_frame(const live::Scene& scene) {
    std::vector<uint16_t> units(scene.vertices.size(), 0);
    for (const auto& range : scene.unit_ranges)
        std::fill_n(
            units.begin() + static_cast<std::ptrdiff_t>(range.first_vertex),
            range.vertex_count,
            range.unit_id
        );
    CardFrame frame;
    std::size_t next = 0;
    const auto take_to = [&](std::size_t end) {
        for (; next < end; ++next) {
            frame.vertices.push_back(scene.vertices[next]);
            frame.units.push_back(units[next]);
        }
    };
    for (const auto& drawn : scene.cards.units) {
        take_to(drawn.vertex);
        for (uint32_t at = 0; at < drawn.index_count; ++at) {
            frame.vertices.push_back(
                live::card_vertex(
                    scene.card_types,
                    scene.cards,
                    drawn,
                    scene.cards.indices[drawn.first_index + at]
                )
            );
            frame.units.push_back(drawn.unit_id);
        }
    }
    take_to(scene.vertices.size());
    return frame;
}

/// Whether a scene with models on the card draws what one without them
/// draws (same_frame): the same vertices, unit by unit, once its card
/// draws are in their places, and the same atlas; and whether each card
/// draw's bounds are its corners' through the frame's camera.
bool same_on_card(const live::Scene& on_card, const live::Scene& vertices, const live::Inputs& in) {
    const auto frame = card_frame(on_card);
    std::vector<uint16_t> units(vertices.vertices.size(), 0);
    for (const auto& range : vertices.unit_ranges)
        std::fill_n(
            units.begin() + static_cast<std::ptrdiff_t>(range.first_vertex),
            range.vertex_count,
            range.unit_id
        );
    bool bounds = true;
    const auto& m = in.clip_projection;
    for (const auto& drawn : on_card.cards.units) {
        float left = std::numeric_limits<float>::infinity();
        float top = left;
        float right = -left;
        float bottom = -left;
        for (uint32_t at = 0; at < drawn.index_count; ++at) {
            const auto corner = live::card_corner(
                on_card.card_types,
                on_card.cards,
                drawn,
                on_card.cards.indices[drawn.first_index + at]
            );
            const float x = m[0] * corner.x + m[1] * corner.height + m[2] * corner.z + m[3];
            const float y = m[4] * corner.x + m[5] * corner.height + m[6] * corner.z + m[7];
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
        bounds =
            bounds && (drawn.index_count == 0 ? drawn.left > drawn.right
                                              : drawn.left == left && drawn.top == top &&
                                                    drawn.right == right && drawn.bottom == bottom);
    }
    return bounds && frame.vertices.size() == vertices.vertices.size() &&
           (frame.vertices.empty() || std::memcmp(
                                          frame.vertices.data(),
                                          vertices.vertices.data(),
                                          frame.vertices.size() * sizeof(frame.vertices[0])
                                      ) == 0) &&
           frame.units == units && on_card.model_count == vertices.model_count &&
           on_card.terrain_vertex_count == vertices.terrain_vertex_count &&
           on_card.texture_rgba == vertices.texture_rgba &&
           on_card.texture_revision == vertices.texture_revision &&
           on_card.materials.size() == vertices.materials.size();
}

/// A battle with its models on the card draws what its vertices draw,
/// frame after frame, on any number of drawing threads: each card draw's
/// triangles, where they fall among the frame's vertices, are the
/// vertices its model would have listed, in their order, with the same
/// atlas places, and the atlas is packed alike. The models show textures
/// by team, animated textures on their first and running frames, colours,
/// walls of the image key, faces turned away and pieces hidden and moving;
/// a model under construction, cloaked, shaded as a building or lit by a
/// nanolathe particle stays in the vertices, and a feature is drawn on the
/// card for no unit. New colours and frames stop the models pass for the
/// thread building the frame to finish.
void models_on_card() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    Fixture battle;
    auto& map = battle.map;
    map.attribute_width = map.attribute_height = 16;
    map.tile_width = map.tile_height = 8;
    map.tile_indices.assign(64, 0);
    map.attributes.resize(256);
    for (std::size_t i = 0; i < map.attributes.size(); ++i)
        map.attributes[i].height = static_cast<uint8_t>((i * 5 % 11) * 2);
    auto& in = battle.inputs;
    in.right = in.bottom = 256;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.view_direction = {0.25F, 1, 0.5F};
    in.clip_projection = {1, 0, 0.1F, 0, 0, -0.5F, 1, 0, 0, 1, 0.5F, 0};
    in.clip_width = in.clip_height = 256;
    std::vector<uint8_t> shades(32 * 256);
    for (std::size_t at = 0; at < shades.size(); ++at)
        shades[at] = static_cast<uint8_t>(at % 256);
    in.shade_table = shades;
    in.light = {0, -1, 0.5F};
    in.light_scale = 16;
    in.graphics_flags = drawing::graphics_shading;
    in.team_colors[1] = 1;
    // A body with a turret on it and a hidden piece: a team texture, an
    // animated texture and a fixed frame on four corners, colours on three
    // to six, a wall of the image key, a face turned away, a selection
    // primitive, a line and a textured triangle that draws nothing.
    auto source = std::make_shared<models::Model>();
    source->objects.resize(3);
    auto& body = source->objects[0];
    body.first_child = 1;
    body.selection_primitive = 5;
    body.vertices = {
        {0, 0, 0},
        {6 * fixed, 0, 0},
        {6 * fixed, 0, 4 * fixed},
        {0, 0, 4 * fixed},
        {0, 3 * fixed, 0},
        {6 * fixed, 3 * fixed, 0},
        {3 * fixed, 5 * fixed, 2 * fixed}
    };
    body.primitives.resize(8);
    body.primitives[0].vertex_indices = {4, 5, 1, 0};
    body.primitives[1].vertex_indices = {0, 1, 2};
    body.primitives[2].vertex_indices = {4, 6, 5, 1, 0};
    body.primitives[3].vertex_indices = {3, 2, 1, 0};
    body.primitives[4].vertex_indices = {0, 1, 6};
    body.primitives[5].vertex_indices = {0, 2, 4};
    body.primitives[6].vertex_indices = {0, 1};
    body.primitives[7].vertex_indices = {1, 5, 6};
    auto& turret = source->objects[1];
    turret.parent = 0;
    turret.next_sibling = 2;
    turret.offset_from_parent = {2 * fixed, 3 * fixed, fixed};
    turret.vertices = {
        {0, 0, 0},
        {2 * fixed, 0, 0},
        {2 * fixed, 2 * fixed, 0},
        {0, 2 * fixed, 0},
        {fixed, 3 * fixed, fixed},
        {-fixed, fixed, 2 * fixed}
    };
    turret.primitives.resize(3);
    turret.primitives[0].vertex_indices = {0, 1, 2, 3};
    turret.primitives[1].vertex_indices = {3, 2, 1, 0};
    turret.primitives[2].vertex_indices = {0, 3, 4, 5, 2, 1};
    auto& hidden = source->objects[2];
    hidden.parent = 0;
    hidden.vertices = {{0, 0, 0}, {fixed, 0, 0}, {0, fixed, 0}};
    hidden.primitives.resize(1);
    hidden.primitives[0].vertex_indices = {0, 1, 2};
    std::array<uint8_t, 32> pixels{};
    for (std::size_t at = 0; at < pixels.size(); ++at)
        pixels[at] = static_cast<uint8_t>(at % 3 == 0 ? ground_index : alternate_index + at % 2);
    drawing::TextureSequence team_frames;
    drawing::TextureSequence animated;
    for (auto* frames : {&team_frames, &animated}) {
        frames->frames.resize(3);
        for (std::size_t frame = 0; frame < frames->frames.size(); ++frame) {
            auto& sprite = frames->frames[frame];
            sprite.width = static_cast<uint16_t>(2 + frame);
            sprite.height = 2;
            sprite.encoding = OA_SPRITE_RAW;
            sprite.data = pixels.data() + frame * 4 + (frames == &animated ? 12 : 0);
        }
    }
    drawing::TextureSequence fixed_frames;
    fixed_frames.frames.resize(1);
    auto& fixed_frame = fixed_frames.frames[0];
    fixed_frame.width = 3;
    fixed_frame.height = 3;
    fixed_frame.encoding = OA_SPRITE_RAW;
    fixed_frame.data = pixels.data() + 1;
    const oa::formats::gaf::Sequence sequence{};
    drawing::PreparedModel prepared;
    prepared.objects.resize(3);
    const auto colour = [](uint32_t index, uint8_t color) {
        drawing::PreparedPrimitive primitive;
        primitive.source_index = index;
        primitive.flags = drawing::primitive_colored;
        primitive.color = color;
        return primitive;
    };
    auto& body_primitives = prepared.objects[0].primitives;
    body_primitives.resize(1);
    body_primitives[0].source_index = 0;
    body_primitives[0].flags = drawing::primitive_animated | drawing::primitive_team;
    body_primitives[0].texture = &team_frames;
    body_primitives.push_back(colour(1, 40));
    body_primitives.push_back(colour(2, 41));
    body_primitives.push_back(colour(3, 42));
    body_primitives.push_back(colour(4, drawing::image_key));
    body_primitives.push_back(colour(5, 43));
    body_primitives.push_back(colour(6, 44));
    drawing::PreparedPrimitive triangle;
    triangle.source_index = 7;
    triangle.frame = &fixed_frame;
    body_primitives.push_back(triangle);
    auto& turret_primitives = prepared.objects[1].primitives;
    turret_primitives.resize(2);
    turret_primitives[0].source_index = 0;
    turret_primitives[0].flags = drawing::primitive_animated;
    turret_primitives[0].texture = &animated;
    turret_primitives[0].cursor.sequence = &sequence;
    turret_primitives[0].cursor.frame_index = 1;
    turret_primitives[1].source_index = 1;
    turret_primitives[1].frame = &fixed_frame;
    turret_primitives.push_back(colour(2, 45));
    prepared.objects[2].primitives.push_back(colour(0, 46));
    // A small craft of one coloured triangle.
    auto craft_source = std::make_shared<models::Model>();
    craft_source->objects.resize(1);
    craft_source->objects[0].vertices = {{0, 0, 0}, {3 * fixed, 0, 0}, {0, 0, 3 * fixed}};
    craft_source->objects[0].primitives.resize(1);
    craft_source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    drawing::PreparedModel craft_prepared;
    craft_prepared.objects.resize(1);
    craft_prepared.objects[0].primitives.push_back(colour(0, 47));
    constexpr std::size_t unit_count = 40;
    std::vector<runtime::Instance> instances;
    for (std::size_t index = 0; index < unit_count; ++index)
        instances.push_back(runtime::make_instance(index % 5 == 4 ? craft_source : source));
    drawing::ModelState pictured{};
    pictured.image = oa::present::create_sprite(8, 8);
    std::vector<oa::Unit> units(unit_count);
    battle.draws.models.resize(unit_count);
    battle.draws.stand_ins.resize(1);
    for (std::size_t index = 0; index < unit_count; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.owner_index = static_cast<uint8_t>(index % 2);
        unit.position = {
            static_cast<int32_t>(16 + index * 37 % 220) * fixed,
            static_cast<int32_t>(index % 3) * fixed,
            static_cast<int32_t>(20 + index * 53 % 210) * fixed
        };
        auto& instance = instances[index];
        if (index % 5 != 4) {
            auto& piece = instance.pieces()[1];
            piece.rotation.xz = static_cast<int16_t>(index * 2300);
            if (index % 3 == 0)
                piece.flags &=
                    static_cast<uint16_t>(~static_cast<uint16_t>(runtime::PieceFlag::cached));
            instance.pieces()[2].flags &=
                static_cast<uint16_t>(~static_cast<uint16_t>(runtime::PieceFlag::visible));
        }
        instance.rebuild_transforms({0, static_cast<int16_t>(index * 4100), 0});
        auto& draw = battle.draws.models[index];
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.prepared = index % 5 == 4 ? &craft_prepared : &prepared;
        draw.plan.linked.drawn = true;
        if (index % 2 == 0) {
            draw.model.state = &pictured;
            draw.plan.model.from_image = true;
        }
        switch (index % 8) {
        case 1:
            unit.state_flags = OA_UNIT_STATE_CLOAKED;
            break;
        case 3:
            unit.build_remaining = 0.4F;
            break;
        case 6:
            unit.flags = OA_UNIT_FLAG_BUILDING;
            break;
        default:
            break;
        }
    }
    // A 3D feature, drawn for no unit.
    battle.draws.stand_ins[0] = units[10];
    battle.draws.stand_ins[0].id = 0;
    battle.draws.models[10].stand_in = 0;
    battle.draws.models[10].on_map = true;
    oa::platform::job_pool::Pool pool(4);
    std::array<live::Scene, 3> scenes{};
    for (int32_t frame = 0; frame < 5; ++frame) {
        // A new colour, the running frame stepped, units on the move, a
        // nanolathe particle beside one of them.
        if (frame == 2)
            battle.palette[41 * oa::palette_entry_bytes] = 200;
        turret_primitives[0].cursor.frame_index = static_cast<uint16_t>(frame % 3);
        for (auto& unit : units)
            unit.position.x += fixed / 3;
        battle.draws.spatial_particles.clear();
        battle.draws.spatial_particles.push_back(
            {{units[frame + 12].position.x + fixed, 4 * fixed, units[frame + 12].position.z},
             {90, 255, 90},
             true}
        );
        in.models_on_card = false;
        in.pool = nullptr;
        OA_CHECK(live::build(in, scenes[0], battle.error));
        in.models_on_card = true;
        OA_CHECK(live::build(in, scenes[1], battle.error));
        in.pool = &pool;
        OA_CHECK(live::build(in, scenes[2], battle.error));
        OA_CHECK(scenes[0].cards.units.empty());
        OA_CHECK(scenes[1].cards.units.size() > unit_count / 3);
        OA_CHECK(!scenes[1].unit_ranges.empty());
        OA_CHECK(!scenes[1].translucent_ranges.empty());
        OA_CHECK(
            std::any_of(
                scenes[1].cards.units.begin(),
                scenes[1].cards.units.end(),
                [](const live::CardUnit& drawn) {
                    return drawn.unit_id == 0 && drawn.index_count != 0;
                }
            )
        );
        for (std::size_t scene = 1; scene < scenes.size(); ++scene)
            OA_CHECK(same_on_card(scenes[scene], scenes[0], in));
    }
    // The types are kept from frame to frame, the frame's draws are not.
    OA_CHECK(scenes[1].card_types.types.size() == 2);
    const auto revision = scenes[1].card_types.revision;
    OA_CHECK(revision != 0 && revision != scenes[2].card_types.revision);
    OA_CHECK(live::build(in, scenes[1], battle.error));
    OA_CHECK(scenes[1].card_types.revision == revision);
}

/// With its models on the card, a battle's units cast their shadows on the
/// card rather than as decals: a vehicle its picture's pieces, cached and
/// visible, without its walls of the image key; a building its cached
/// pieces sheared, walls and all, its picture taken out at the
/// silhouette's depth; a unit under the water and a digger only above the
/// water and the ground, where their pictures have a depth plane; each
/// cast nearer than the one before, at the frame's shadow level. An
/// aircraft's soft shadow stays a decal; a floater and a type that casts
/// none cast nothing. Everything but the decals the card casts is the same
/// as the frame drawn with its models in the vertices.
void shadows_on_card() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    Fixture battle;
    auto& map = battle.map;
    map.attribute_width = map.attribute_height = 16;
    map.tile_width = map.tile_height = 8;
    map.tile_indices.assign(64, 0);
    map.attributes.resize(256);
    for (auto& attribute : map.attributes)
        attribute.height = 20;
    map.sea_level = 0;
    auto& in = battle.inputs;
    in.right = in.bottom = 256;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.graphics_flags = drawing::graphics_shadows | drawing::graphics_vehicle_shadows;
    in.sea_level = 30;
    // A body and a turret, the turret moving and so out of the cached picture.
    auto source = std::make_shared<models::Model>();
    source->objects.resize(2);
    auto& body = source->objects[0];
    body.first_child = 1;
    body.vertices = {{0, 0, 0}, {6 * fixed, 0, 0}, {6 * fixed, 4 * fixed, 0}, {0, 4 * fixed, 0}};
    body.primitives.resize(2);
    body.primitives[0].vertex_indices = {0, 1, 2, 3};
    body.primitives[1].vertex_indices = {0, 1, 2};
    auto& turret = source->objects[1];
    turret.parent = 0;
    turret.offset_from_parent = {2 * fixed, 4 * fixed, 0};
    turret.vertices = {{0, 0, 0}, {fixed, 0, 0}, {0, fixed, 0}};
    turret.primitives.resize(1);
    turret.primitives[0].vertex_indices = {0, 1, 2};
    drawing::PreparedModel prepared;
    prepared.objects.resize(2);
    const auto colour = [](uint32_t index, uint8_t color) {
        drawing::PreparedPrimitive primitive;
        primitive.source_index = index;
        primitive.flags = drawing::primitive_colored;
        primitive.color = color;
        return primitive;
    };
    prepared.objects[0].primitives = {colour(0, 40), colour(1, drawing::image_key)};
    prepared.objects[1].primitives = {colour(0, 41)};

    enum Kind : std::size_t {
        vehicle,
        building,
        under_water,
        digger,
        aircraft,
        floater,
        none,
        kinds
    };

    std::array<runtime::Instance, kinds> instances{};
    std::array<oa::Unit, kinds> units{};
    std::array<oa::UnitDef, kinds> definitions{};
    std::array<drawing::ModelState, kinds> states{};
    battle.draws.models.resize(kinds);
    for (std::size_t kind = 0; kind < kinds; ++kind) {
        auto& instance = instances[kind] = runtime::make_instance(source);
        instance.pieces()[1].flags &=
            static_cast<uint16_t>(~static_cast<uint16_t>(runtime::PieceFlag::cached));
        instance.rebuild_transforms();
        auto& unit = units[kind];
        unit.id = static_cast<uint16_t>(kind + 1);
        unit.type_index = 1;
        unit.position = {static_cast<int32_t>(30 + kind * 28) * fixed, 20 * fixed, 60 * fixed};
        auto& state = states[kind];
        state.image = oa::present::create_sprite(8, 8);
        state.shadow = oa::present::create_sprite(8, 8);
        auto& draw = battle.draws.models[kind];
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.model.state = &state;
        draw.model.def = &definitions[kind];
        draw.plan.linked.drawn = true;
        draw.plan.model.from_image = true;
        draw.plan.model.ground = 20;
    }
    units[building].flags = OA_UNIT_FLAG_BUILDING;
    units[under_water].position.y = 26 * fixed;
    drawing::allocate_depth_image(states[under_water].image, 8, 8);
    definitions[digger].flags = OA_UNIT_DEF_FLAG_DIGGER;
    drawing::allocate_depth_image(states[digger].image, 8, 8);
    definitions[aircraft].flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    units[aircraft].position.y = 90 * fixed;
    definitions[floater].flags = OA_UNIT_DEF_FLAG_FLOATER;
    definitions[none].flags = OA_UNIT_DEF_FLAG_NO_SHADOW;
    // The frame's own materials, the decals' shadows, in rows of their own.
    live::Scene vertices_scene;
    live::Scene card_scene;
    for (auto* scene : {&vertices_scene, &card_scene}) {
        scene->texture_width = scene->texture_height = 4096;
        scene->frame_rows = 64;
    }
    in.models_on_card = false;
    OA_CHECK(live::build(in, vertices_scene, battle.error));
    in.models_on_card = true;
    OA_CHECK(live::build(in, card_scene, battle.error));
    const auto& cards = card_scene.cards;
    // A vehicle, a building's two, the unit under the water and the digger.
    OA_CHECK(cards.casts.size() == 5);
    OA_CHECK(cards.cast_depths == 4);
    OA_CHECK(vertices_scene.shadow_count == 5 && card_scene.shadow_count == 5);
    using oa::app::renderer_live_gpu::shadow_cast_cut;
    using oa::app::renderer_live_gpu::shadow_cast_picture;
    using oa::app::renderer_live_gpu::shadow_cast_sheared;
    const auto& casts = cards.casts;
    OA_CHECK(casts[0].kind == shadow_cast_picture && casts[0].light == 0.5F);
    OA_CHECK(casts[1].kind == shadow_cast_cut && casts[1].light == 1.0F);
    OA_CHECK(casts[2].kind == shadow_cast_sheared && casts[2].light == 0.5F);
    OA_CHECK(casts[1].depth == casts[2].depth);
    OA_CHECK(casts[0].depth > casts[1].depth && casts[2].depth > casts[3].depth);
    OA_CHECK(casts[3].depth > casts[4].depth);
    OA_CHECK(casts[0].base == 20 && casts[0].ground_half == 10);
    OA_CHECK(casts[0].least_height < -1e30F);
    OA_CHECK(casts[3].least_height == 4.0F);
    OA_CHECK(casts[4].least_height == 0.0F);
    // The picture: the body's quad alone (its triangle is a wall of the
    // image key, the turret not cached); the silhouette: the body's quad
    // and triangle.
    std::vector<uint32_t> per_cast(casts.size(), 0);
    for (const auto& run : cards.cast_runs)
        for (uint32_t at = 0; at < run.index_count; ++at)
            ++per_cast
                [run.first_instance + (cards.cast_indices[run.first_index + at] >>
                                       oa::app::renderer_live_gpu::model_corner_bits)];
    OA_CHECK(per_cast == (std::vector<uint32_t>{6, 6, 9, 6, 6}));
    // The aircraft's soft shadow stays a decal.
    OA_CHECK(card_scene.vertices.size() > cards.shadow_vertex);
    // Without the aircraft, the frame is the other's less its decals.
    units[aircraft].position.y = 20 * fixed;
    definitions[aircraft].flags = OA_UNIT_DEF_FLAG_NO_SHADOW;
    in.models_on_card = false;
    OA_CHECK(live::build(in, vertices_scene, battle.error));
    in.models_on_card = true;
    OA_CHECK(live::build(in, card_scene, battle.error));
    const auto card = card_frame(card_scene).vertices;
    const auto& all = vertices_scene.vertices;
    const std::size_t first = card_scene.cards.shadow_vertex;
    OA_CHECK(first == card_scene.terrain_vertex_count);
    OA_CHECK(all.size() > card.size() && card.size() > first);
    const auto same = [](const auto* a, const auto* b, std::size_t count) {
        return std::memcmp(a, b, count * sizeof(oa::app::renderer_live_gpu::Vertex)) == 0;
    };
    OA_CHECK(same(card.data(), all.data(), first));
    OA_CHECK(same(
        card.data() + first, all.data() + all.size() - (card.size() - first), card.size() - first
    ));
    // The shadows' map spans the part of the map the frame shows.
    const auto& shadow_map = card_scene.cards.shadow_map;
    OA_CHECK(shadow_map.left == in.left && shadow_map.top == in.top && shadow_map.scale == 1);
    OA_CHECK(shadow_map.width == uint32_t(in.right - in.left));
    // Shadows off, none is cast.
    in.graphics_flags = 0;
    OA_CHECK(live::build(in, card_scene, battle.error));
    OA_CHECK(card_scene.cards.casts.empty() && card_scene.shadow_count == 0);
}

/// Aircraft shadows keep their silhouette at full alpha at every altitude,
/// soften only their edge, a pixel more high up, and receive on water at
/// the vehicles' own shadow opacity.
void aircraft_shadow_altitude() {
    namespace shadow = oa::app::renderer_aircraft_shadow;
    for (const float height : {0.0F, 16.0F, 48.0F, 95.0F})
        OA_CHECK(shadow::edge_radius(height) == shadow::narrow_edge_radius);
    for (const float height : {96.0F, 110.0F, 220.0F, 4096.0F})
        OA_CHECK(shadow::edge_radius(height) == shadow::wide_edge_radius);
    OA_CHECK(shadow::edge_radius(std::nanf("")) == shadow::narrow_edge_radius);
    const std::array<uint8_t, 9> mask{0, 1, 0, 1, 1, 1, 0, 1, 0};
    const auto soft = shadow::soften(mask, 3, 3, 2);
    OA_CHECK(soft.size() == 11 * 11);
    // Every pixel of the cross, its one-pixel arms too, stays opaque; the
    // pixel above its top arm and the one between two arms take part of
    // the edge.
    for (std::size_t i = 0; i < mask.size(); ++i)
        if (mask[i] != 0)
            OA_CHECK(soft[(i / 3 + 4) * 11 + i % 3 + 4] == 255);
    const uint8_t beside = soft[3 * 11 + 5];
    const uint8_t corner = soft[4 * 11 + 4];
    OA_CHECK(beside > 0 && beside < 255);
    OA_CHECK(corner > 0 && corner < 255);
    OA_CHECK(soft.front() == 0 && soft.back() == 0);
    OA_CHECK(shadow::soften(mask, 3, 3, 1).size() == 7 * 7);
    Fixture fixture;
    oa::present::model::ModelState state{};
    state.image = oa::present::create_sprite(8, 8);
    state.image.sprite.key = 1;
    std::fill(state.image.pixels.begin(), state.image.pixels.end(), 0);
    constexpr int32_t fixed = 65536;
    oa::Unit unit{};
    unit.position = {16 * fixed, 104 * fixed, 16 * fixed};
    oa::UnitDef definition{};
    definition.flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.unit = &unit;
    draw.model.def = &definition;
    draw.model.state = &state;
    draw.plan.linked.drawn = true;
    draw.plan.model.from_image = true;
    fixture.inputs.sea_level = 8;
    fixture.inputs.graphics_flags =
        oa::present::model::graphics_shadows | oa::present::model::graphics_vehicle_shadows;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.shadow_count == 1);
    OA_CHECK(fixture.scene.frame.aircraft_shadows.size() == 1);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        OA_CHECK(std::abs(vertex.height - 8.05F) < tolerance);
        OA_CHECK(std::abs(vertex.opacity - 0.5F) < tolerance);
        OA_CHECK(vertex.terrain_size == -1);
    }
    const auto revision = fixture.scene.texture_revision;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision == revision);
}

/// With New unit shadows off an aircraft casts the game's shadow: its plain
/// silhouette, laid on the ground under it rather than on the water, at the
/// vehicles' opacity and with no soft edge, as every other unit's.
void aircraft_shadow_as_the_game_casts_it() {
    Fixture fixture;
    oa::present::model::ModelState state{};
    state.image = oa::present::create_sprite(8, 8);
    state.image.sprite.key = 1;
    std::fill(state.image.pixels.begin(), state.image.pixels.end(), 0);
    constexpr int32_t fixed = 65536;
    oa::Unit unit{};
    unit.position = {16 * fixed, 104 * fixed, 16 * fixed};
    oa::UnitDef definition{};
    definition.flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.unit = &unit;
    draw.model.def = &definition;
    draw.model.state = &state;
    draw.plan.linked.drawn = true;
    draw.plan.model.from_image = true;
    fixture.inputs.sea_level = 8;
    fixture.inputs.graphics_flags =
        oa::present::model::graphics_shadows | oa::present::model::graphics_vehicle_shadows;
    fixture.inputs.new_unit_shadows = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.shadow_count == 1);
    OA_CHECK(fixture.scene.frame.aircraft_shadows.empty());
    OA_CHECK(fixture.scene.vertices.size() > fixture.scene.terrain_vertex_count);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        OA_CHECK(vertex.height < 8.0F);
        OA_CHECK(std::abs(vertex.opacity - 0.5F) < tolerance);
        OA_CHECK(vertex.terrain_size != -1);
    }
}

/// Crops cover their intersecting cells but never leave the map, and invalid inputs fail cleanly.
void bounds_and_invalid_inputs() {
    Fixture fixture;
    fixture.inputs.left = -100;
    fixture.inputs.top = -100;
    fixture.inputs.right = 100;
    fixture.inputs.bottom = 100;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == 24);
    fixture.inputs.left = 17;
    fixture.inputs.top = 17;
    fixture.inputs.right = 31;
    fixture.inputs.bottom = 31;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == 6);
    for (const auto& vertex : fixture.scene.vertices) {
        OA_CHECK(vertex.x >= 16 && vertex.x <= 32);
        const float painted_y = vertex.z - vertex.height * 0.5F;
        OA_CHECK(painted_y >= 16 && painted_y <= 32);
    }
    fixture.inputs.left = tile_edge;
    fixture.inputs.right = tile_edge;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.empty());
    fixture.inputs.left = 40;
    OA_CHECK(!live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.error.empty());
    OA_CHECK(fixture.scene.vertices.empty());
    fixture.inputs.left = 0;
    fixture.inputs.map = nullptr;
    OA_CHECK(!live::build(fixture.inputs, fixture.scene, fixture.error));
    fixture.inputs.map = &fixture.map;
    fixture.map.attributes.clear();
    OA_CHECK(!live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.terrain_vertex_count == 0);
}

/// Surface effects follow sea height, clip beaches and vanish when disabled or distant.
void water_surface_bounds() {
    Fixture fixture;
    fixture.inputs.sea_level = 8;
    fixture.inputs.water.ripples = fixture.inputs.water.coastal_waves = 1;
    fixture.map.attributes[1].height = fixture.map.attributes[3].height = 16;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    std::size_t water_vertices{};
    bool coast{};
    for (const auto& vertex : fixture.scene.vertices) {
        if (vertex.terrain_size != -2)
            continue;
        ++water_vertices;
        OA_CHECK(std::abs(vertex.height - 8.05F) < tolerance);
        OA_CHECK(vertex.x <= 8 + tolerance);
        OA_CHECK(vertex.terrain_x >= 0);
        coast |= vertex.terrain_x == 0;
    }
    OA_CHECK(water_vertices > 0);
    OA_CHECK(coast);
    // Deep water takes no sheet: the coastal foam lies on the shallows
    // alone, so however far out the view, the sheet's budget holds the
    // whole shore.
    fixture.inputs.sea_level = 64;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(
        std::none_of(
            fixture.scene.vertices.begin(), fixture.scene.vertices.end(), [](const auto& vertex) {
                return vertex.terrain_size == -2;
            }
        )
    );
    fixture.inputs.sea_level = 8;
    fixture.inputs.water.detail = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
    fixture.inputs.water.detail = 1;
    fixture.inputs.water.ripples = fixture.inputs.water.coastal_waves = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
    // Water ripples lay no sheet over the water: the vertex stage moves the
    // ground's own art.
    fixture.inputs.water.ripples = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
    fixture.inputs.water.coastal_waves = 1;
    fixture.mapped[0] = fixture.coverage[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    for (const auto& vertex : fixture.scene.vertices)
        if (vertex.terrain_size == -2)
            OA_CHECK(vertex.brightness == 0);
}

/// Shot disturbances stay on water, age with simulation ticks and obey current visibility.
void water_disturbance_visibility() {
    Fixture fixture;
    fixture.inputs.sea_level = 8;
    fixture.inputs.water.ripples = 1;
    fixture.inputs.tick = 8;
    const std::array events{oa::app::renderer_water::Disturbance{
        16, 16, 0, 1, oa::app::renderer_water::Kind::water_impact
    }};
    fixture.inputs.water_disturbances = events;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    std::size_t disturbance_vertices{};
    bool splash{};
    for (const auto& vertex : fixture.scene.vertices) {
        if (vertex.terrain_size != -4)
            continue;
        ++disturbance_vertices;
        OA_CHECK(vertex.terrain_x >= 0);
        OA_CHECK(vertex.height >= fixture.inputs.sea_level);
        splash |= vertex.height > fixture.inputs.sea_level + 1;
    }
    OA_CHECK(disturbance_vertices > 0 && disturbance_vertices <= 306);
    OA_CHECK(splash);
    fixture.inputs.tick = 60;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(
        std::none_of(
            fixture.scene.vertices.begin(), fixture.scene.vertices.end(), [](const auto& vertex) {
                return vertex.terrain_size == -4;
            }
        )
    );
    fixture.inputs.tick = 8;
    fixture.coverage[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(
        std::none_of(
            fixture.scene.vertices.begin(), fixture.scene.vertices.end(), [](const auto& vertex) {
                return vertex.terrain_size == -4;
            }
        )
    );
    fixture.coverage[0] = 1;
    fixture.inputs.water.detail = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(
        std::none_of(
            fixture.scene.vertices.begin(), fixture.scene.vertices.end(), [](const auto& vertex) {
                return vertex.terrain_size == -4;
            }
        )
    );
}

/// Only visible floating models reflect, and reflected triangles never become picking targets.
void naval_reflection_visibility() {
    Fixture fixture;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {
        {0, 0, 0},
        {fixed, 0, 0},
        {0, fixed, 0},
        {0, fixed, 0},
        {0, fixed, -4 * fixed},
        {4 * fixed, fixed, 0}
    };
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    drawing::PreparedModel prepared{};
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(1);
    prepared.objects[0].primitives[0].flags = drawing::primitive_colored;
    prepared.objects[0].primitives[0].color = ground_index;
    oa::Unit unit{};
    unit.id = 7;
    unit.position = {8 * fixed, 9 * fixed, 16 * fixed};
    oa::UnitDef definition{};
    definition.flags = OA_UNIT_DEF_FLAG_FLOATER;
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.instance = &instance;
    draw.model.prepared = &prepared;
    draw.model.unit = &unit;
    draw.model.def = &definition;
    draw.plan.linked.drawn = true;
    fixture.inputs.sea_level = 8;
    fixture.inputs.water.reflections = 1;
    fixture.inputs.view_direction = {0, 1, 0.5F};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    std::size_t reflections{};
    for (const auto& vertex : fixture.scene.vertices) {
        if (vertex.terrain_size != -3)
            continue;
        ++reflections;
        OA_CHECK(std::abs(vertex.height - 8.12F) < tolerance);
        OA_CHECK(vertex.terrain_x > 0);
        OA_CHECK(
            vertex.opacity > oa::app::renderer_water_appearance::reflection_opacity * 0.97F &&
            vertex.opacity <= oa::app::renderer_water_appearance::reflection_opacity
        );
    }
    OA_CHECK(reflections == 3);
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    OA_CHECK(fixture.scene.unit_ranges[0].vertex_count == 3);
    const auto reflected_begin = fixture.scene.terrain_vertex_count + 3;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& original = fixture.scene.vertices[fixture.scene.terrain_vertex_count + i];
        const auto& reflected = fixture.scene.vertices[reflected_begin + i];
        const float mirrored_screen =
            original.z - (2 * fixture.inputs.sea_level - original.height) * 0.5F;
        OA_CHECK(
            std::abs(reflected.z - reflected.height * 0.5F + 0.06F - mirrored_screen) < tolerance
        );
    }
    // The former elevation cutoff must not remove reflections abruptly.
    float previous_opacity{};
    for (float elevation : {0.201F, 0.199F, 0.12F, 0.06F}) {
        fixture.inputs.view_direction = {0, elevation, 0.5F};
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        float opacity{};
        for (const auto& vertex : fixture.scene.vertices)
            if (vertex.terrain_size == -3) {
                opacity = std::max(opacity, vertex.opacity);
                OA_CHECK(std::isfinite(vertex.x) && std::isfinite(vertex.z));
            }
        OA_CHECK(opacity > 0);
        if (previous_opacity > 0)
            OA_CHECK(opacity <= previous_opacity + tolerance);
        if (elevation == 0.199F)
            OA_CHECK(std::abs(opacity - previous_opacity) < tolerance);
        previous_opacity = opacity;
    }
    fixture.inputs.view_direction = {0, 1, 0.5F};
    // A hull face intersecting the waterline retains its above-water portion.
    instance.pieces()[0].transformed_vertices[2].y = -2 * fixed;
    source->objects[0].primitives[0].vertex_indices = {0, 2, 1};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(
        std::count_if(
            fixture.scene.vertices.begin(), fixture.scene.vertices.end(), [](const auto& vertex) {
                return vertex.terrain_size == -3;
            }
        ) == 6
    );
    instance.pieces()[0].transformed_vertices[2].y = fixed;
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    // An underside is invisible to the direct camera but visible in the reflection.
    source->objects[0].primitives.resize(2);
    source->objects[0].primitives[1].vertex_indices = {3, 4, 5};
    prepared.objects[0].primitives.resize(2);
    prepared.objects[0].primitives[1] = prepared.objects[0].primitives[0];
    prepared.objects[0].primitives[1].source_index = 1;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges[0].vertex_count == 3);
    OA_CHECK(
        std::count_if(
            fixture.scene.vertices.begin(), fixture.scene.vertices.end(), [](const auto& vertex) {
                return vertex.terrain_size == -3;
            }
        ) == 6
    );
    definition.flags = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 3);
    definition.flags = OA_UNIT_DEF_FLAG_FLOATER;
    draw.plan.linked.drawn = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
}

/// Building shade rows match classic bright textures and indexed palette remaps.
void model_shade_parity() {
    Fixture fixture;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 0, 0}, {fixed, 0, 0}, {fixed, fixed, 0}, {0, fixed, 0}};
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2, 3};
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    instance.pieces()[0].flags |= static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached) |
                                  static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::shaded);
    std::array<uint8_t, 1> pixel{ground_index};
    drawing::TextureSequence texture;
    texture.frames.resize(1);
    texture.frames[0].width = texture.frames[0].height = 1;
    texture.frames[0].encoding = OA_SPRITE_RAW;
    texture.frames[0].data = pixel.data();
    drawing::PreparedModel prepared{};
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(1);
    prepared.objects[0].primitives[0].texture = &texture;
    prepared.objects[0].primitives[0].frame = &texture.frames[0];
    drawing::ModelState state{};
    state.image.sprite.data = pixel.data();
    oa::Unit unit{};
    unit.id = 7;
    unit.flags = OA_UNIT_FLAG_BUILDING;
    unit.position = {8 * fixed, 0, 8 * fixed};
    fixture.draws.models.resize(1);
    auto& draw = fixture.draws.models[0];
    draw.model.instance = &instance;
    draw.model.prepared = &prepared;
    draw.model.state = &state;
    draw.model.unit = &unit;
    draw.plan.linked.drawn = true;
    draw.plan.model.from_image = true;
    fixture.inputs.graphics_flags = drawing::graphics_shading;
    fixture.inputs.light = {0, 0, 1};
    fixture.inputs.light_scale = 20;
    std::array<uint8_t, 32 * 256> shades{};
    shades.fill(alternate_index);
    fixture.inputs.shade_table = shades;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(!fixture.scene.unit_ranges.empty());
    if (fixture.scene.unit_ranges.empty())
        return;
    const auto first = fixture.scene.unit_ranges[0].first_vertex;
    OA_CHECK(triangle_color(fixture.scene, first)[0] == 165);
    OA_CHECK(std::abs(fixture.scene.vertices[first].brightness - 20 * 0.06875F / 2) < tolerance);
    // Camera orientation changes visibility, never the baked model light direction.
    fixture.inputs.view_direction = {0, 0, 1};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(std::abs(fixture.scene.vertices[first].brightness - 20 * 0.06875F / 2) < tolerance);
    fixture.inputs.view_direction = {};
    unit.flags = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, first)[0] == 120);
    OA_CHECK(fixture.scene.vertices[first].brightness == 1);
    unit.flags = OA_UNIT_FLAG_BUILDING;
    fixture.inputs.graphics_flags = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, first)[0] == 120);
    fixture.inputs.graphics_flags = drawing::graphics_shading;
    // The ground's colour at an index other than the image key, which a
    // pictured unit's polygons leave clear.
    constexpr uint8_t model_index = 3;
    std::copy_n(
        fixture.palette.begin() + ground_index * oa::palette_entry_bytes,
        oa::palette_entry_bytes,
        fixture.palette.begin() + model_index * oa::palette_entry_bytes
    );
    prepared.objects[0].primitives[0].flags = drawing::primitive_colored;
    prepared.objects[0].primitives[0].color = model_index;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto& vertex = fixture.scene.vertices[first];
    OA_CHECK(triangle_color(fixture.scene, first)[0] == 30);
    OA_CHECK(std::abs(vertex.red - 30 / 255.0F) < tolerance);
    OA_CHECK(std::abs(vertex.green - 120 / 255.0F) < tolerance);
    OA_CHECK(std::abs(vertex.blue - 240 / 255.0F) < tolerance);
    // Standalone callers without a complete classic lighting bundle keep source art.
    fixture.inputs.shade_table = {};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, first)[0] == ground_color[0]);
    fixture.inputs.shade_table = shades;
    fixture.inputs.light_scale = 0;
    prepared.objects[0].primitives[0].flags = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(triangle_color(fixture.scene, first)[0] == ground_color[0]);
}

/// Model shots and their children keep true depth, camera origin and current visibility.
void projectile_world_depth() {
    Fixture fixture;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    models::Object object{};
    object.vertices = {{0, 0, 0}, {fixed, 0, 0}, {0, fixed, 0}};
    object.primitives.resize(1);
    object.primitives[0].vertex_indices = {0, 1, 2};
    drawing::PreparedObject prepared{};
    prepared.primitives.resize(1);
    prepared.primitives[0].flags = drawing::primitive_colored;
    prepared.primitives[0].color = ground_index;
    oa::app::ProjectileDraw shot{};
    shot.object = shot.child = &object;
    shot.prepared = shot.child_prepared = &prepared;
    shot.position = {4 * fixed, 4 * fixed, 12 * fixed};
    fixture.draws.projectiles.push_back(shot);
    fixture.inputs.camera_x = fixture.inputs.camera_y = 4;
    fixture.inputs.sea_level = 16;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count + 6);
    const auto first = fixture.scene.terrain_vertex_count;
    OA_CHECK(fixture.scene.vertices[first].x == 8);
    OA_CHECK(fixture.scene.vertices[first].height == 4);
    OA_CHECK(fixture.scene.vertices[first].z == 16);
    OA_CHECK(fixture.scene.unit_ranges.empty());
    for (std::size_t i = first; i < fixture.scene.vertices.size(); ++i) {
        OA_CHECK(fixture.scene.vertices[i].height >= 4 && fixture.scene.vertices[i].height <= 5);
        // A shot keeps the unit tint. It is not marked as map scenery.
        OA_CHECK(fixture.scene.vertices[i].terrain_size == 0);
        OA_CHECK(fixture.scene.vertices[i].terrain_grey == 0);
    }
    fixture.draws.projectiles[0].position.y = 20 * fixed;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[first].height == 20);
    fixture.coverage[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
}

/// Underwater flashes remain local billboards; impact art is never raised out of the water.
void underwater_effect_altitude() {
    Fixture fixture;
    for (auto& attribute : fixture.map.attributes)
        attribute.height = 3;
    fixture.inputs.sea_level = 16;
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 8;
    frame.origin_x = frame.origin_y = 4;
    frame.pixels.assign(64, 110);
    frame.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&frame, {8, 14}});
    auto& sprite = fixture.draws.sprites[0];
    sprite.world_position = oa::FixedVec3{8 * 65536, 4 * 65536, 16 * 65536};
    sprite.surface_effect = true;
    fixture.draws.draws.push_back({oa::app::WorldDrawKind::lit_sprite, 0});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.sprite_count == 1);
    float minimum = 100, maximum = -100;
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        minimum = std::min(minimum, vertex.height);
        maximum = std::max(maximum, vertex.height);
        OA_CHECK(vertex.opacity == 1);
        OA_CHECK(vertex.terrain_size == -5 && vertex.terrain_x == 12);
    }
    OA_CHECK(minimum < 3 && maximum < 16);
    OA_CHECK(std::abs((minimum + maximum) / 2 - 4) < tolerance);
    fixture.draws.draws[0].kind = oa::app::WorldDrawKind::sprite;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices[fixture.scene.terrain_vertex_count].height < 16);
    fixture.coverage[0] = 0;
    fixture.draws.draws[0].kind = oa::app::WorldDrawKind::lit_sprite;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.sprite_count == 0);
}

/// Reads one atlas texel at a normalised coordinate.
std::array<uint8_t, 3> atlas_texel(const live::Scene& scene, float u, float v) {
    const auto x = std::min(
        scene.texture_width - 1, static_cast<uint32_t>(std::max(0.0F, u) * scene.texture_width)
    );
    const auto y = std::min(
        scene.texture_height - 1, static_cast<uint32_t>(std::max(0.0F, v) * scene.texture_height)
    );
    const auto offset = (static_cast<std::size_t>(y) * scene.texture_width + x) * 4;
    return {
        scene.texture_rgba.at(offset),
        scene.texture_rgba.at(offset + 1),
        scene.texture_rgba.at(offset + 2)
    };
}

/// Map scenery under water takes no unit tint and greys with the ground.
/// A unit sprite and the open sea bed still follow the swells.
void underwater_features() {
    oa::formats::gaf::RenderedFrame frame{};
    frame.width = frame.height = 8;
    frame.pixels.assign(64, ground_index);
    frame.coverage.assign(64, 1);
    constexpr std::size_t grey_index = 3;
    constexpr std::array<uint8_t, 3> grey_color{70, 70, 70};

    // Wholly out of sight, on the sea bed: the grey palette, not a darkened tint.
    {
        Fixture fixture;
        std::fill_n(fixture.palette.begin() + grey_index * oa::palette_entry_bytes, 3, 70);
        fixture.coverage[0] = 0;
        fixture.inputs.sea_level = 40;
        fixture.inputs.water.wind.strength = 1;
        fixture.inputs.water.detail = 1;
        fixture.draws.sprites.push_back({&frame, {12, 12}, false, true, true});
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.scene.sprite_count == 1);
        bool marked = false;
        float min_u = 1, max_u = 0, min_v = 1, max_v = 0;
        float min_grey_u = 1, max_grey_u = 0, min_grey_v = 1, max_grey_v = 0;
        for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
             ++i) {
            const auto& vertex = fixture.scene.vertices[i];
            OA_CHECK(vertex.terrain_size == -6);
            OA_CHECK(std::abs(vertex.terrain_known - 1) < tolerance);
            OA_CHECK(std::abs(vertex.terrain_grey - 1) < tolerance);
            min_u = std::min(min_u, vertex.u);
            max_u = std::max(max_u, vertex.u);
            min_v = std::min(min_v, vertex.v);
            max_v = std::max(max_v, vertex.v);
            min_grey_u = std::min(min_grey_u, vertex.terrain_x);
            max_grey_u = std::max(max_grey_u, vertex.terrain_x);
            min_grey_v = std::min(min_grey_v, vertex.terrain_y);
            max_grey_v = std::max(max_grey_v, vertex.terrain_y);
            marked = true;
        }
        OA_CHECK(marked);
        // The frame's far edge is the next atlas texel. Its middle is the image.
        OA_CHECK(
            atlas_texel(fixture.scene, (min_u + max_u) / 2, (min_v + max_v) / 2) == ground_color
        );
        OA_CHECK(
            atlas_texel(
                fixture.scene, (min_grey_u + max_grey_u) / 2, (min_grey_v + max_grey_v) / 2
            ) == grey_color
        );
        const auto still = fixture.scene.vertices;
        fixture.inputs.water.ripples = 1;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.scene.vertices.size() == still.size());
        bool moved = false;
        for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
             ++i) {
            OA_CHECK(fixture.scene.vertices[i].terrain_size == -6);
            moved = moved || fixture.scene.vertices[i].x != still[i].x;
        }
        OA_CHECK(moved);
    }

    // Permanent allied vision. Ground only an ally explored is grey, as ground
    // out of sight is. The viewer's own explored ground stays in colour.
    {
        constexpr uint16_t ally_bit = 1U << 2;
        const auto on_ground = [&](uint16_t explored_by, bool greyed) {
            Fixture fixture;
            std::fill_n(fixture.palette.begin() + grey_index * oa::palette_entry_bytes, 3, 70);
            fixture.mapped[0] = explored_by;
            fixture.coverage[0] = 0;
            fixture.inputs.sight.line_of_sight = false;
            fixture.inputs.sight.mapped_bits = static_cast<uint16_t>(viewer_bit | ally_bit);
            fixture.inputs.sight.shared_bits = ally_bit;
            fixture.draws.sprites.push_back({&frame, {12, 12}, false, true, true});
            OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
            OA_CHECK(fixture.scene.sprite_count == 1);
            bool marked = false;
            for (std::size_t i = fixture.scene.terrain_vertex_count;
                 i < fixture.scene.vertices.size();
                 ++i) {
                const auto& vertex = fixture.scene.vertices[i];
                OA_CHECK(vertex.terrain_size == -6);
                OA_CHECK(std::abs(vertex.terrain_known - 1) < tolerance);
                if (greyed)
                    OA_CHECK(std::abs(vertex.terrain_grey - 1) < tolerance);
                else
                    OA_CHECK(vertex.terrain_grey == 0);
                marked = true;
            }
            OA_CHECK(marked);
        };
        on_ground(ally_bit, true);
        on_ground(viewer_bit, false);
    }

    // Shallows, still under the water and fully in sight: the same marker, no grey.
    {
        Fixture fixture;
        for (auto& attribute : fixture.map.attributes)
            attribute.height = 20;
        fixture.inputs.sea_level = 24;
        fixture.draws.sprites.push_back({&frame, {8, 8}, false, true, true});
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        bool marked = false;
        for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
             ++i) {
            const auto& vertex = fixture.scene.vertices[i];
            OA_CHECK(vertex.terrain_size == -6);
            OA_CHECK(std::abs(vertex.terrain_known - 1) < tolerance);
            OA_CHECK(vertex.terrain_grey == 0);
            OA_CHECK(std::abs(vertex.brightness - 1) < tolerance);
            marked = true;
        }
        OA_CHECK(marked);
    }

    // A patch across seen, grey and unexplored ground matches the ground's fog,
    // in both fog styles, and hides where the ground is black.
    {
        Fixture fixture;
        std::fill_n(fixture.palette.begin() + grey_index * oa::palette_entry_bytes, 3, 70);
        fixture.map.attribute_width = 6;
        fixture.map.tile_width = 3;
        fixture.map.attributes.assign(12, {});
        fixture.map.tile_indices = {0, 0, 0};
        std::array<uint16_t, 3> coverage{1, 0, 0};
        std::array<uint16_t, 3> mapped{viewer_bit, viewer_bit, 0};
        fixture.inputs.sight.coverage = coverage;
        fixture.inputs.sight.player_bits = mapped;
        fixture.inputs.sight.width = 3;
        fixture.inputs.right = 96;
        frame.width = 32;
        frame.height = 16;
        frame.pixels.assign(static_cast<std::size_t>(frame.width * frame.height), ground_index);
        frame.coverage.assign(frame.pixels.size(), 1);
        fixture.draws.sprites.push_back({&frame, {48, 8}, false, true, true});
        const auto check_patch = [&](bool dithered) {
            fixture.inputs.sight.dithered = dithered;
            OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
            bool grey = false;
            bool black = false;
            float boundary_brightness = -1;
            for (std::size_t i = fixture.scene.terrain_vertex_count;
                 i < fixture.scene.vertices.size();
                 ++i) {
                const auto& vertex = fixture.scene.vertices[i];
                OA_CHECK(vertex.terrain_size == -6);
                const float painted_y = vertex.z - (vertex.height - 0.05F) * 0.5F;
                if (vertex.x >= 66 && painted_y >= 8 && painted_y <= 24) {
                    OA_CHECK(vertex.terrain_known == 0);
                    OA_CHECK(vertex.terrain_grey == 0);
                    black = true;
                }
                if (std::abs(vertex.x - 48) < 1.5F && painted_y >= 8 && painted_y <= 20) {
                    OA_CHECK(vertex.terrain_known > 0.9F);
                    OA_CHECK(vertex.terrain_grey > 0.9F);
                    grey = true;
                }
                if (std::abs(vertex.x - 56) < 2.5F && painted_y >= 8 && painted_y <= 24)
                    boundary_brightness = vertex.brightness;
            }
            OA_CHECK(grey && black);
            OA_CHECK(boundary_brightness >= 0);
            return boundary_brightness;
        };
        const float smooth = check_patch(false);
        const float classic = check_patch(true);
        OA_CHECK(classic > smooth);

        // An upright feature takes the fog at each corner, not one darkening.
        frame.origin_y = 0;
        fixture.draws.sprites[0].terrain_decal = false;
        fixture.inputs.sight.dithered = false;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.scene.vertices.size() > fixture.scene.terrain_vertex_count + 6);
        bool upright_black = false;
        bool upright_known = false;
        for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
             ++i) {
            const auto& vertex = fixture.scene.vertices[i];
            OA_CHECK(vertex.terrain_size == -6);
            if (vertex.terrain_known == 0)
                upright_black = true;
            if (vertex.terrain_known > 0.9F && vertex.terrain_grey > 0.9F)
                upright_known = true;
        }
        OA_CHECK(upright_black && upright_known);
    }

    // A unit's sprite is not scenery: it keeps the ordinary marker.
    {
        Fixture fixture;
        fixture.inputs.sea_level = 30;
        frame.width = frame.height = 8;
        frame.origin_y = 0;
        frame.pixels.assign(64, ground_index);
        frame.coverage.assign(64, 1);
        fixture.draws.sprites.push_back({&frame, {16, 16}, false, false, false});
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        bool drawn = false;
        for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
             ++i) {
            const auto& vertex = fixture.scene.vertices[i];
            OA_CHECK(vertex.terrain_size != -6 && vertex.terrain_size != -7);
            OA_CHECK(vertex.terrain_grey == 0);
            drawn = true;
        }
        OA_CHECK(drawn);
    }
}

} // namespace

int main() {
    water_surface_bounds();
    sea_bed_follows_swells();
    water_disturbance_visibility();
    naval_reflection_visibility();
    model_shade_parity();
    projectile_world_depth();
    underwater_effect_altitude();
    underwater_features();
    painted_projection();
    canonical_terrain_grid();
    twisted_terrain_follows_simulation();
    clipped_texture_bounds();
    fog_materials();
    terrain_mip_colours();
    terrain_wider_than_atlas();
    terrain_room_remembered();
    terrain_held_while_moving();
    terrain_square_given_back();
    terrain_squares_given_back_whole();
    coarse_ground_far_out();
    terrain_beyond_picture();
    ground_kept_while_moving();
    smooth_fog_boundary();
    terrain_zoom_detail();
    particle_zoom_detail();
    atlas_reuse();
    terrain_material_mutation();
    ground_kept_while_unchanged();
    atlas_repack_rows();
    emptied_atlas_uploads_packed_rows();
    terrain_material_repack();
    planned_model_visibility();
    model_key_walls_and_cloak();
    model_material_lifetime();
    animated_frames_follow_classic_tiers();
    terrain_decals();
    sprite_material_lifetime();
    cropped_ground_sprites();
    building_foundations();
    foundations_cut_once();
    foundation_candidates();
    structure_base_above_ground();
    cached_unit_shadows();
    frame_rows_hold_unit_shadows();
    frame_rows_grow_for_spilled_shadows();
    passes_on_threads();
    ground_drawn_ahead_on_threads();
    moving_camera_comes_back_to_its_picture();
    models_on_card();
    shadows_on_card();
    aircraft_shadow_altitude();
    aircraft_shadow_as_the_game_casts_it();
    flash_masks_and_effect_altitude();
    spatial_particles();
    particle_screen_bounds();
    surface_explosions();
    model_face_winding();
    bounds_and_invalid_inputs();
    return oa::test::check_exit_status();
}
