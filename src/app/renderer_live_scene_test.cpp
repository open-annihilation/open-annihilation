// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Live terrain preserves painted coordinates, visibility and bounded atlas reuse.
#include "renderer_live_scene.hpp"
#include "renderer_aircraft_shadow.hpp"
#include "renderer_water_appearance.hpp"
#include "oa/test/check.hpp"
#include "oa/present/rle.hpp"
#include "oa/sim/unit_movement/terrain.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

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
        inputs.sight.viewer_bit = viewer_bit;
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

/// Stable frames avoid atlas uploads; a palette change creates new material pixels.
void atlas_reuse() {
    Fixture fixture;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto revision = fixture.scene.texture_revision;
    const auto materials = fixture.scene.materials.size();
    OA_CHECK(revision != 0);
    OA_CHECK(fixture.scene.atlas_update.previous_revision == 0);
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    OA_CHECK(fixture.scene.atlas_update.end_row == tile_edge * 2);
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
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    OA_CHECK(fixture.scene.atlas_update.end_row == tile_edge * 2);
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

/// Atlas repacking replaces all rows while retaining the prior GPU revision.
void atlas_repack_rows() {
    Fixture fixture;
    fixture.scene.texture_width = fixture.scene.texture_height = tile_edge * 2;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto revision = fixture.scene.texture_revision;
    fixture.palette[ground_index * oa::palette_entry_bytes] = 77;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_width > tile_edge);
    OA_CHECK(fixture.scene.atlas_update.previous_revision == revision);
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    OA_CHECK(fixture.scene.atlas_update.end_row == fixture.scene.texture_height);
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == 77);
    // Filling an already enlarged atlas must also invalidate all old rows.
    fixture.scene.next_x = fixture.scene.texture_width;
    fixture.scene.next_y = fixture.scene.texture_height;
    const auto enlarged_revision = fixture.scene.texture_revision;
    fixture.palette[ground_index * oa::palette_entry_bytes] = 88;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.atlas_update.previous_revision == enlarged_revision);
    OA_CHECK(fixture.scene.atlas_update.first_row == 0);
    OA_CHECK(fixture.scene.atlas_update.end_row == fixture.scene.texture_height);
    OA_CHECK(triangle_color(fixture.scene, 0)[0] == 88);
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

/// Aircraft shadows broaden and fade with altitude, retain silhouette alpha and receive on water.
void aircraft_shadow_altitude() {
    namespace shadow = oa::app::renderer_aircraft_shadow;
    auto previous = shadow::style(0);
    for (const float height : {16.0F, 48.0F, 96.0F, 192.0F, 512.0F}) {
        const auto current = shadow::style(height);
        OA_CHECK(current.spread >= previous.spread && current.spread <= 1.45F);
        OA_CHECK(current.opacity <= previous.opacity && current.opacity >= 0.29F);
        OA_CHECK(current.radius >= previous.radius && current.radius <= 8);
        previous = current;
    }
    const std::array<uint8_t, 9> mask{0, 1, 0, 1, 1, 1, 0, 1, 0};
    const auto soft = shadow::soften(mask, 3, 3, 2);
    OA_CHECK(soft.size() == 11 * 11);
    OA_CHECK(std::any_of(soft.begin(), soft.end(), [](uint8_t alpha) {
        return alpha > 0 && alpha < 255;
    }));
    OA_CHECK(soft.front() == 0 && soft.back() == 0);
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
    OA_CHECK(fixture.scene.aircraft_shadow_materials.size() == 1);
    for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
         ++i) {
        const auto& vertex = fixture.scene.vertices[i];
        OA_CHECK(std::abs(vertex.height - 8.05F) < tolerance);
        OA_CHECK(vertex.opacity < 0.5F && vertex.opacity > 0.1F);
        OA_CHECK(vertex.terrain_size == -1);
    }
    const auto revision = fixture.scene.texture_revision;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision == revision);
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
    fixture.inputs.water.detail = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
    fixture.inputs.water.detail = 1;
    fixture.inputs.water.ripples = fixture.inputs.water.coastal_waves = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
    fixture.inputs.water.ripples = 1;
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
    for (std::size_t i = first; i < fixture.scene.vertices.size(); ++i)
        OA_CHECK(fixture.scene.vertices[i].height >= 4 && fixture.scene.vertices[i].height <= 5);
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

} // namespace

int main() {
    water_surface_bounds();
    water_disturbance_visibility();
    naval_reflection_visibility();
    model_shade_parity();
    projectile_world_depth();
    underwater_effect_altitude();
    painted_projection();
    canonical_terrain_grid();
    twisted_terrain_follows_simulation();
    clipped_texture_bounds();
    fog_materials();
    terrain_mip_colours();
    smooth_fog_boundary();
    terrain_zoom_detail();
    particle_zoom_detail();
    atlas_reuse();
    terrain_material_mutation();
    atlas_repack_rows();
    terrain_material_repack();
    planned_model_visibility();
    model_key_walls_and_cloak();
    model_material_lifetime();
    animated_frames_follow_classic_tiers();
    terrain_decals();
    sprite_material_lifetime();
    cropped_ground_sprites();
    building_foundations();
    foundation_candidates();
    cached_unit_shadows();
    aircraft_shadow_altitude();
    flash_masks_and_effect_altitude();
    spatial_particles();
    particle_screen_bounds();
    surface_explosions();
    model_face_winding();
    bounds_and_invalid_inputs();
    return oa::test::check_exit_status();
}
