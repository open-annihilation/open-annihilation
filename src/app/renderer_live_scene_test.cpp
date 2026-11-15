// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Live terrain preserves painted coordinates, visibility and bounded atlas reuse.
#include "renderer_live_scene.hpp"
#include "model_shadow_metadata.hpp"
#include "parallel_model_cards.hpp"
#include "bench_timers.hpp"
#include "renderer_live_camera.hpp"
#include "renderer_water_surface.hpp"
#include "renderer_aircraft_shadow.hpp"
#include "renderer_water_appearance.hpp"
#include "oa/test/check.hpp"
#include "oa/present/rle.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/sim/unit_movement/terrain.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {
thread_local bool watch_ground_allocations{};
thread_local std::size_t ground_allocations{}, refused_ground_allocations{};
thread_local std::size_t ground_allocation_to_refuse = std::numeric_limits<std::size_t>::max();
} // namespace

/// Refuses one selected ordinary allocation only during an explicit preparation check.
void* operator new(std::size_t bytes) {
    if (watch_ground_allocations && ground_allocations++ == ground_allocation_to_refuse) {
        ++refused_ground_allocations;
        throw std::bad_alloc{};
    }
    if (void* result = std::malloc(bytes == 0 ? 1 : bytes))
        return result;
    throw std::bad_alloc{};
}

/// Routes array allocation through the same bounded failure check.
void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}

/// Releases ordinary allocation storage.
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

/// Releases array allocation storage.
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

/// Releases sized ordinary allocation storage.
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

/// Releases sized array allocation storage.
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

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

/// Discards optional coordinates so retained terrain queries read their original vertices.
/// @param scene the scene whose coordinate cache is discarded
void discard_ground_points(live::Scene& scene) {
    scene.ground_points_revision = 0;
    scene.ground_points.clear();
}

/// Requires every published coordinate to preserve the source vertex's exact bits.
/// @param scene the scene whose published coordinate cache is checked
void check_ground_points(const live::Scene& scene) {
    if (scene.ground_points_revision == 0)
        return;
    const bool cut = scene.cut_source != 0 && scene.ground_points_revision == scene.cut_revision;
    const auto& vertices = cut ? scene.cut_ground : scene.ground_vertices;
    OA_CHECK(
        scene.ground_points_revision == (cut ? scene.cut_revision : scene.kept_ground_revision)
    );
    OA_CHECK(scene.ground_points.size() == vertices.size());
    for (std::size_t index = 0; index < std::min(scene.ground_points.size(), vertices.size());
         ++index) {
        const auto& point = scene.ground_points[index];
        const auto& vertex = vertices[index];
        OA_CHECK(std::bit_cast<uint32_t>(point.x) == std::bit_cast<uint32_t>(vertex.x));
        OA_CHECK(std::bit_cast<uint32_t>(point.z) == std::bit_cast<uint32_t>(vertex.z));
        OA_CHECK(std::bit_cast<uint32_t>(point.height) == std::bit_cast<uint32_t>(vertex.height));
    }
}

/// Deferred image metadata preserves geometry, materials, detail decisions
/// and emitted-face picking bounds on both model drawing paths.
void check_deferred_image_draw(const live::Inputs& inputs, oa::present::model::ModelState& state) {
    const auto prior_deferred = state.deferred_image;
    const bool prior_depth = state.deferred_image_depth;
    for (const bool on_card : {false, true}) {
        auto in = inputs;
        in.models_on_card = on_card;
        std::array<live::Scene, 2> scenes;
        std::string error;
        OA_CHECK(live::build(in, scenes[0], error));
        auto image = std::move(state.image);
        state.image = {};
        const auto& sprite = image.sprite;
        state.deferred_image = oa::present::model::ImageFrame{
            sprite.width, sprite.height, sprite.origin_x, sprite.origin_y
        };
        state.deferred_image_depth = sprite.aux != nullptr;
        OA_CHECK(live::build(in, scenes[1], error));
        OA_CHECK(state.image.sprite.data == nullptr && state.image.pixels.empty());
        state.image = std::move(image);
        state.deferred_image = prior_deferred;
        state.deferred_image_depth = prior_depth;
        const auto& expected = scenes[0];
        const auto& actual = scenes[1];
        OA_CHECK(expected.shadow_count == 0 && actual.shadow_count == 0);
        OA_CHECK(expected.model_count == actual.model_count);
        OA_CHECK(expected.terrain_vertex_count == actual.terrain_vertex_count);
        OA_CHECK(expected.vertices.size() == actual.vertices.size());
        if (expected.vertices.size() == actual.vertices.size() && !expected.vertices.empty())
            OA_CHECK(
                std::memcmp(
                    expected.vertices.data(),
                    actual.vertices.data(),
                    expected.vertices.size() * sizeof(expected.vertices[0])
                ) == 0
            );
        OA_CHECK(expected.texture_rgba == actual.texture_rgba);
        OA_CHECK(expected.cards.positions == actual.cards.positions);
        OA_CHECK(expected.cards.indices == actual.cards.indices);
        OA_CHECK(expected.cards.piece_frames == actual.cards.piece_frames);
        OA_CHECK(expected.cards.textures == actual.cards.textures);
        OA_CHECK(
            std::equal(
                expected.unit_ranges.begin(),
                expected.unit_ranges.end(),
                actual.unit_ranges.begin(),
                actual.unit_ranges.end(),
                [](const live::UnitRange& a, const live::UnitRange& b) {
                    return a.unit_id == b.unit_id && a.first_vertex == b.first_vertex &&
                           a.vertex_count == b.vertex_count;
                }
            )
        );
        OA_CHECK(
            std::equal(
                expected.cards.units.begin(),
                expected.cards.units.end(),
                actual.cards.units.begin(),
                actual.cards.units.end(),
                [](const live::CardUnit& a, const live::CardUnit& b) {
                    return a.unit_id == b.unit_id && a.vertex == b.vertex &&
                           a.instance == b.instance && a.type == b.type &&
                           a.first_index == b.first_index && a.index_count == b.index_count &&
                           a.left == b.left && a.top == b.top && a.right == b.right &&
                           a.bottom == b.bottom && a.bounded == b.bounded &&
                           a.body_lod == b.body_lod;
                }
            )
        );
        OA_CHECK(
            std::equal(
                expected.unit_detail.begin(),
                expected.unit_detail.end(),
                actual.unit_detail.begin(),
                actual.unit_detail.end(),
                [](const live::UnitDetail& a, const live::UnitDetail& b) {
                    return a.surface == b.surface && a.shadow == b.shadow &&
                           std::bit_cast<uint32_t>(a.pixels) == std::bit_cast<uint32_t>(b.pixels) &&
                           std::bit_cast<uint32_t>(a.brightness) ==
                               std::bit_cast<uint32_t>(b.brightness) &&
                           std::bit_cast<std::array<uint32_t, 3>>(a.center) ==
                               std::bit_cast<std::array<uint32_t, 3>>(b.center) &&
                           a.eligible == b.eligible && a.marker == b.marker;
                }
            )
        );
    }
}

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
        check_deferred_image_draw(fixture.inputs, state);
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
    // Construction keeps the same cached/moving frame choices for both
    // kinds of unit and for the moving-piece presentation setting.
    unit.build_remaining = 0.5F;
    for (const bool building : {false, true}) {
        unit.flags = building ? OA_UNIT_FLAG_BUILDING : 0;
        for (const bool moving_once_built : {false, true}) {
            fixture.inputs.moving_pieces_once_built = moving_once_built;
            check_deferred_image_draw(fixture.inputs, state);
        }
    }
    unit.build_remaining = 0;
    unit.flags = 0;
    fixture.inputs.moving_pieces_once_built = false;
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

/// An inward zoom shares the exact terrain already covering its picture;
/// the ordinary path still redraws, and settling releases the wider ground.
void contained_camera_ground() {
    constexpr uint32_t tiles = 32;
    constexpr uint32_t width = 200, height = 150;
    constexpr std::array<float, 12> projection{1, 0, -1, 100, 0.5F, -1, 0.5F, -250, 0, 1, 0, 0};
    const auto make = [&](Fixture& fixture, bool contained) {
        auto& map = fixture.map;
        map.tile_width = map.tile_height = tiles;
        map.attribute_width = map.attribute_height = tiles * 2;
        map.tile_indices.assign(tiles * tiles, 0);
        map.attributes.assign(map.attribute_width * map.attribute_height, {});
        for (uint32_t row = 0; row < map.attribute_height; ++row)
            for (uint32_t column = 0; column < map.attribute_width; ++column)
                map.attributes[row * map.attribute_width + column].height =
                    static_cast<uint8_t>((column * 3 + row * 5) % 40);
        auto& in = fixture.inputs;
        in.sight.mapping = in.sight.line_of_sight = false;
        in.right = in.bottom = static_cast<int32_t>(tiles) * tile_edge;
        in.clip_width = width;
        in.clip_height = height;
        in.clip_projection = projection;
        in.detail_scale = 0.4F;
        in.tiles_hold_while_moving = true;
        in.ground_holds_contained_views = contained;
    };
    const auto zoom = [&](live::Inputs& in, float scale) {
        for (std::size_t row : {std::size_t{0}, std::size_t{4}}) {
            for (std::size_t column = 0; column < 4; ++column)
                in.clip_projection[row + column] = projection[row + column] * scale;
            const float center = row == 0 ? width / 2.0F : height / 2.0F;
            in.clip_projection[row + 3] += center * (1 - scale);
        }
    };
    using Triangle = std::array<uint32_t, 3 * sizeof(oa::app::renderer_live_gpu::Vertex) / 4>;
    const auto triangles = [](const live::Scene& scene) {
        std::vector<Triangle> result;
        for (std::size_t first = 0; first + 2 < scene.terrain_vertex_count; first += 3) {
            Triangle triangle{};
            std::memcpy(triangle.data(), scene.vertices.data() + first, sizeof(triangle));
            result.push_back(triangle);
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    Fixture moving, ordinary;
    make(moving, true);
    make(ordinary, false);
    const auto build = [](Fixture& fixture) {
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.error.empty());
        check_ground_points(fixture.scene);
        OA_CHECK(fixture.scene.vertices.size() <= oa::app::renderer_live_gpu::largest_vertex_count);
        return fixture.scene.ground_revision;
    };
    build(moving);
    const auto original = build(moving);
    build(ordinary);
    const auto ordinary_original = build(ordinary);
    const auto retained = triangles(moving.scene);
    OA_CHECK(moving.scene.ground_kept && moving.scene.ground_source.picture_margin == 0);
    for (int frame = 1; frame <= 24; ++frame) {
        const float scale = 1 + frame * 0.025F;
        zoom(moving.inputs, scale);
        zoom(ordinary.inputs, scale);
        OA_CHECK(build(moving) == original);
        OA_CHECK(build(ordinary) != ordinary_original);
        OA_CHECK(!moving.scene.ground_ahead.active);
        OA_CHECK(triangles(moving.scene) == retained);
        Fixture fresh;
        make(fresh, false);
        zoom(fresh.inputs, scale);
        build(fresh);
        const auto pictured = triangles(fresh.scene);
        OA_CHECK(std::includes(retained.begin(), retained.end(), pictured.begin(), pictured.end()));
    }
    for (int frame = 0; frame < 10; ++frame)
        build(moving);
    OA_CHECK(moving.scene.ground_revision != original);
    OA_CHECK(moving.scene.terrain_vertex_count < retained.size() * 3);
    // A pan outside the retained picture cannot take its geometry.
    const auto settled = moving.scene.ground_revision;
    moving.inputs.clip_projection[3] += width * 2;
    OA_CHECK(build(moving) != settled);
    // Every retained-source edit remains an invalidation, including edits
    // at the same address and a detail threshold crossed during the zoom.
    const auto changed = [&](const auto& edit) {
        Fixture fixture;
        make(fixture, true);
        build(fixture);
        const auto before = build(fixture);
        zoom(fixture.inputs, 1.2F);
        OA_CHECK(build(fixture) == before);
        edit(fixture);
        OA_CHECK(build(fixture) != before);
    };
    changed([](Fixture& fixture) { ++fixture.map.attributes[100].height; });
    changed([](Fixture& fixture) { ++fixture.map.tile_palette_indices[0]; });
    changed([](Fixture& fixture) { ++fixture.palette[ground_index * oa::palette_entry_bytes]; });
    changed([](Fixture& fixture) { fixture.inputs.sight.mapping = true; });
    changed([](Fixture& fixture) { fixture.inputs.detail_scale = 0.6F; });
    changed([](Fixture& fixture) { ++fixture.inputs.clip_width; });
    changed([](Fixture& fixture) {
        for (std::size_t row : {std::size_t{0}, std::size_t{4}})
            for (std::size_t column = 0; column < 4; ++column)
                fixture.inputs.clip_projection[row + column] *= 0.5F;
    });
    changed([](Fixture& fixture) {
        fixture.inputs.clip_projection[6] = -fixture.inputs.clip_projection[4];
    });
    const int rounding = std::fegetround();
    changed([](Fixture&) { OA_CHECK(std::fesetround(FE_DOWNWARD) == 0); });
    OA_CHECK(std::fesetround(rounding) == 0);
    // The wider retained ground gives way to the current picture before
    // full-detail models force the terrain to lose height samples.
    Fixture crowded, fresh;
    make(crowded, true);
    make(fresh, false);
    build(crowded);
    const auto wide = build(crowded);
    zoom(crowded.inputs, 3);
    zoom(fresh.inputs, 3);
    OA_CHECK(build(crowded) == wide);
    build(fresh);
    const auto narrow_count = fresh.scene.terrain_vertex_count;
    OA_CHECK(crowded.scene.terrain_vertex_count > narrow_count);
    constexpr std::size_t polygons = 128;
    constexpr std::size_t model_vertices = polygons * 3;
    const std::size_t population =
        (oa::app::renderer_live_gpu::largest_vertex_count - narrow_count) / model_vertices;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 0, 0}, {4 * fixed, 0, 0}, {0, 4 * fixed, 0}};
    source->objects[0].primitives.resize(polygons);
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(polygons);
    for (std::size_t index = 0; index < polygons; ++index) {
        source->objects[0].primitives[index].vertex_indices = {0, 1, 2};
        auto& primitive = prepared.objects[0].primitives[index];
        primitive.source_index = static_cast<uint32_t>(index);
        primitive.flags = drawing::primitive_colored;
        primitive.color = alternate_index;
    }
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    std::vector<oa::Unit> units(population);
    for (Fixture* fixture : {&crowded, &fresh}) {
        fixture->inputs.models_on_card = true;
        fixture->draws.models.resize(population);
        for (std::size_t index = 0; index < population; ++index) {
            auto& unit = units[index];
            unit.id = static_cast<uint16_t>(index + 1);
            unit.flags = OA_UNIT_FLAG_BUILDING;
            unit.position = {325 * fixed, 20 * fixed, 325 * fixed};
            auto& draw = fixture->draws.models[index];
            draw.model.unit = &unit;
            draw.model.instance = &instance;
            draw.model.prepared = &prepared;
            draw.plan.linked.drawn = true;
        }
        build(*fixture);
        OA_CHECK(fixture->scene.cards.vertices == population * model_vertices);
    }
    OA_CHECK(crowded.scene.ground_revision != wide);
    OA_CHECK(crowded.scene.ground_source.step == 1 && fresh.scene.ground_source.step == 1);
    OA_CHECK(crowded.scene.terrain_square_limit == fresh.scene.terrain_square_limit);
    OA_CHECK(triangles(crowded.scene) == triangles(fresh.scene));
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
    check_deferred_image_draw(fixture.inputs, state);
    OA_CHECK(fixture.scene.model_count == 1);
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
    auto detailed = fixture.inputs;
    detailed.unit_marker_pixels = 5;
    detailed.clip_width = detailed.clip_height = 128;
    detailed.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, -0.001F, 0, 0.5F};
    check_deferred_image_draw(detailed, state);
    draw.plan.model.from_image = false;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    check_deferred_image_draw(fixture.inputs, state);
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
    check_deferred_image_draw(fixture.inputs, state);
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
    check_deferred_image_draw(fixture.inputs, state);
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

/// Persistent counters distinguish the first packing source and charge cache misses only.
void persistent_material_sources() {
    namespace timers = oa::app::bench_timers;
    Fixture fixture;
    std::fill_n(fixture.palette.begin() + 3 * oa::palette_entry_bytes, 3, 70);
    std::fill_n(fixture.palette.begin() + 4 * oa::palette_entry_bytes, 3, 130);
    std::array<oa::formats::gaf::RenderedFrame, 4> frames{};
    constexpr std::array<uint8_t, 4> colors{ground_index, alternate_index, 110, 3};
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        auto& frame = frames[index];
        frame.width = frame.height = 4;
        frame.pixels.assign(16, colors[index]);
        frame.coverage.assign(16, 1);
        fixture.draws.sprites.push_back({&frame, {4, 4}});
    }
    fixture.draws.sprites[0].on_map = true;
    fixture.draws.sprites[1].world_position = oa::FixedVec3{8 * fixed, 8 * fixed, 16 * fixed};
    fixture.draws.sprites[2].world_position = oa::FixedVec3{8 * fixed, 0, 16 * fixed};
    fixture.draws.sprites[3].shadow = true;
    fixture.draws.shadow_level = oa::present::model::shadow_full_level;
    fixture.draws.draws.push_back({oa::app::WorldDrawKind::lit_sprite, 2});
    timers::reset(true);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto first = timers::timers.totals;
    const auto kind = [](timers::MaterialKind value) { return static_cast<std::size_t>(value); };
    OA_CHECK(first.persistent_materials[kind(timers::MaterialKind::surface)] == 0);
    OA_CHECK(first.persistent_materials[kind(timers::MaterialKind::scenery)] == 2);
    for (const auto value : {
             timers::MaterialKind::effect, timers::MaterialKind::light, timers::MaterialKind::shadow
         }) {
        OA_CHECK(first.persistent_materials[kind(value)] == 1);
        OA_CHECK(first.persistent_texels[kind(value)] == 16);
    }
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto reused = timers::totals_since(timers::timers.totals, first);
    for (std::size_t index = 0; index < timers::material_kind_count; ++index) {
        OA_CHECK(reused.persistent_materials[index] == 0);
        OA_CHECK(reused.persistent_texels[index] == 0);
        OA_CHECK(reused.persistent_row_texels[index] == 0);
    }
    frames[1].pixels[0] = 3;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    const auto changed = timers::totals_since(timers::timers.totals, first);
    OA_CHECK(changed.persistent_materials[kind(timers::MaterialKind::effect)] == 0);
    OA_CHECK(changed.persistent_texels[kind(timers::MaterialKind::effect)] == 0);
    OA_CHECK(fixture.scene.frame.effect_area == 16);
    auto sum = first;
    timers::add_totals(sum, changed);
    OA_CHECK(sum.persistent_materials == timers::timers.totals.persistent_materials);
    OA_CHECK(sum.persistent_texels == timers::timers.totals.persistent_texels);
    OA_CHECK(sum.persistent_row_texels == timers::timers.totals.persistent_row_texels);
    timers::reset(false);
    fixture.scene = {};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(timers::timers.totals.persistent_materials == timers::Totals{}.persistent_materials);

    // A wide mask starts a new skyline row: the counter includes its row's
    // unused columns, rather than reporting those as painted texels.
    Fixture wide;
    oa::formats::gaf::RenderedFrame mask{};
    mask.width = 2000;
    mask.height = 200;
    mask.pixels.assign(400'000, 110);
    mask.coverage.assign(400'000, 1);
    wide.draws.sprites.push_back({&mask, {4, 4}});
    wide.draws.draws.push_back({oa::app::WorldDrawKind::lit_sprite, 0});
    timers::reset(true);
    OA_CHECK(live::build(wide.inputs, wide.scene, wide.error));
    const auto light = kind(timers::MaterialKind::light);
    OA_CHECK(timers::timers.totals.persistent_materials[light] == 1);
    OA_CHECK(timers::timers.totals.persistent_texels[light] == 400'000);
    OA_CHECK(timers::timers.totals.persistent_row_texels[light] == 200 * 2048);
    timers::reset(false);
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
/// of the frame before takes the cut ground and its revision, keeps it
/// listed at the head of its vertices, and cuts only its decals, and other
/// plots cut it again. Every frame is the one a scene cutting the ground
/// each frame builds.
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
        cutting.passes.ground_source = 0;
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
    // The frame lists the ground the frame before listed by keeping it at
    // the head of its vertices, and draws what a full cut draws.
    OA_CHECK(fixture.scene.listed_ground_revision == first);
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

/// A kept ground whose plots change is cut again only where a changed plot
/// reaches it: two structures far apart, one moving, give every frame the
/// ground a scene cutting all of it each frame gives, on any number of
/// threads, and a band the moving one never reaches keeps its cut (marked
/// in a scene of its own, the mark stays) until a plot reaches it.
void foundations_recut_where_plots_change() {
    constexpr uint32_t attributes = 128;
    constexpr int32_t fixed = 65536;
    const auto make = [&](Fixture& fixture) {
        fixture.map.attribute_width = fixture.map.attribute_height = attributes;
        fixture.map.tile_width = fixture.map.tile_height = attributes / 2;
        fixture.map.tile_indices.assign(
            static_cast<std::size_t>(attributes / 2) * (attributes / 2), 0
        );
        fixture.map.attributes.resize(static_cast<std::size_t>(attributes) * attributes);
        for (std::size_t i = 0; i < fixture.map.attributes.size(); ++i)
            fixture.map.attributes[i].height =
                static_cast<uint8_t>((i * 7 + i / attributes * 3) % 40);
        fixture.inputs.right = fixture.inputs.bottom = static_cast<int32_t>(attributes) * 16;
        fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    };
    std::array<oa::Unit, 2> units{};
    for (auto& unit : units) {
        unit.flags = OA_UNIT_FLAG_BUILDING;
        unit.footprint_x = unit.footprint_z = 2;
    }
    units[0].position = {200 * fixed, 16 * fixed, 200 * fixed};
    units[1].position = {1800 * fixed, 16 * fixed, 1800 * fixed};
    Fixture reference;
    make(reference);
    reference.draws.models.resize(units.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        reference.draws.models[index].model.unit = &units[index];
        reference.draws.models[index].plan.linked.drawn = true;
    }
    std::vector<std::unique_ptr<oa::platform::job_pool::Pool>> pools;
    pools.push_back(nullptr);
    for (const uint32_t threads : {2U, 3U})
        pools.push_back(std::make_unique<oa::platform::job_pool::Pool>(threads));
    std::vector<live::Scene> scenes(pools.size());
    live::Scene cutting{};
    live::Scene marked{};
    const auto build = [&] {
        for (std::size_t scene = 0; scene < scenes.size(); ++scene) {
            reference.inputs.pool = pools[scene].get();
            OA_CHECK(live::build(reference.inputs, scenes[scene], reference.error));
        }
        reference.inputs.pool = nullptr;
        cutting.cut_source = 0;
        cutting.passes.ground_source = 0;
        OA_CHECK(live::build(reference.inputs, cutting, reference.error));
        for (const auto& scene : scenes) {
            OA_CHECK(scene.terrain_vertex_count == cutting.terrain_vertex_count);
            OA_CHECK(
                scene.vertices.size() == cutting.vertices.size() &&
                std::memcmp(
                    scene.vertices.data(),
                    cutting.vertices.data(),
                    scene.vertices.size() * sizeof(scene.vertices.front())
                ) == 0
            );
        }
        OA_CHECK(live::build(reference.inputs, marked, reference.error));
    };
    // The first vertex of the marked scene's first band of cut ground, as
    // listed in its frame.
    const auto first_height = [&] { return marked.vertices.front().height; };
    build();
    build();
    OA_CHECK(scenes[0].ground_kept && scenes[0].cut_source != 0);
    OA_CHECK(marked.passes.ground.size() > 2);
    const float height = first_height();
    marked.passes.ground.front().vertices.front().height = height + 1000;
    for (int step = 1; step <= 4; ++step) {
        units[1].position.x = (1800 - 40 * step) * fixed;
        build();
        OA_CHECK(first_height() == height + 1000);
    }
    // The first structure moves: its band is cut again.
    units[0].position.x = 240 * fixed;
    build();
    OA_CHECK(first_height() == cutting.vertices.front().height);
    // The structure goes; the frames are still those of a full cut.
    units[1].flags = 0;
    build();
    units[1].flags = OA_UNIT_FLAG_BUILDING;
    build();
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

/// Compares emitted geometry and sampled colors while allowing different atlas rectangles.
/// @param a the frame-local material scene
/// @param b the ordinary persistent-material reference
void same_effect_picture(const live::Scene& a, const live::Scene& b) {
    OA_CHECK(a.vertices.size() == b.vertices.size());
    OA_CHECK(a.terrain_vertex_count == b.terrain_vertex_count);
    OA_CHECK(a.shadow_count == b.shadow_count && a.sprite_count == b.sprite_count);
    OA_CHECK(a.model_count == b.model_count);
    if (a.vertices.size() != b.vertices.size())
        return;
    for (std::size_t index = 0; index < a.vertices.size(); ++index) {
        auto left = a.vertices[index];
        auto right = b.vertices[index];
        left.u = left.v = right.u = right.v = 0;
        if (left.terrain_size > 0)
            left.terrain_x = left.terrain_y = right.terrain_x = right.terrain_y = 0;
        OA_CHECK(std::memcmp(&left, &right, sizeof(left)) == 0);
    }
    for (std::size_t first = 0; first + 2 < a.vertices.size(); first += 3)
        OA_CHECK(triangle_color(a, first) == triangle_color(b, first));
}

/// Compares every RGBA texel of one transient picture with its persistent reference.
/// @param actual the frame-local atlas
/// @param rectangle its material rectangle
/// @param expected the cold persistent reference atlas
void same_effect_texels(
    const live::Scene& actual, const live::AtlasRect& rectangle, const live::Scene& expected
) {
    const auto material =
        std::find_if(expected.materials.begin(), expected.materials.end(), [&](const auto& entry) {
            return entry.second.width == rectangle.width && entry.second.height == rectangle.height;
        });
    OA_CHECK(material != expected.materials.end());
    if (material == expected.materials.end())
        return;
    const auto& other = material->second;
    for (uint32_t row = 0; row < rectangle.height; ++row) {
        const auto from = (std::size_t{rectangle.y + row} * actual.texture_width + rectangle.x) * 4;
        const auto reference = (std::size_t{other.y + row} * expected.texture_width + other.x) * 4;
        OA_CHECK(
            std::memcmp(
                actual.texture_rgba.data() + from,
                expected.texture_rgba.data() + reference,
                std::size_t{rectangle.width} * 4
            ) == 0
        );
    }
}

/// Colored and light variants share reserved rows safely with animated unit shadows.
void frame_rows_hold_effects() {
    namespace timers = oa::app::bench_timers;
    Fixture fixture;
    fixture.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0, 0};
    fixture.inputs.clip_width = fixture.inputs.clip_height = 64;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    std::array<oa::formats::gaf::RenderedFrame, 2> art{};
    art[0].width = art[0].height = 200;
    art[0].pixels.assign(40'000, 110);
    art[0].coverage.assign(40'000, 1);
    art[0].coverage[2] = 0;
    art[1].width = 16;
    art[1].height = 8;
    art[1].pixels.assign(128, ground_index);
    art[1].coverage.assign(128, 1);
    art[1].coverage[0] = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        fixture.draws.sprites.push_back({&art[index == 1 ? 1 : 0], {4, 4}});
        fixture.draws.sprites.back().world_position =
            oa::FixedVec3{8 * fixed, index == 1 ? 8 * fixed : 0, 16 * fixed};
        fixture.draws.draws.push_back(
            {index == 1 ? oa::app::WorldDrawKind::sprite : oa::app::WorldDrawKind::lit_sprite,
             static_cast<uint32_t>(index)}
        );
    }
    oa::present::model::ModelState state{};
    state.image = oa::present::create_sprite(8, 8);
    state.image.sprite.key = 1;
    std::fill(state.image.pixels.begin(), state.image.pixels.end(), 1);
    oa::Unit unit{};
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
    fixture.scene.texture_width = fixture.scene.texture_height = 4096;
    fixture.scene.frame_rows = 256;
    timers::reset(true);
    uint64_t ground{};
    for (uint32_t frame = 0; frame < 48; ++frame) {
        art[0].pixels[0] = static_cast<uint8_t>(80 + frame % 31);
        art[0].pixels[1] = static_cast<uint8_t>(80 + frame / 31);
        art[1].pixels[3] = static_cast<uint8_t>(ground_index + frame % 2);
        state.image.pixels[4] = static_cast<uint8_t>(frame % 2);
        fixture.draws.flash_strength =
            frame % 2 ? oa::app::FlashStrength::reduced : oa::app::FlashStrength::full;
        fixture.draws.sprites[1].effect_opacity = frame % 2 ? 0.4F : 1.0F;
        fixture.draws.sprites[1].effect_scale = frame % 2 ? 0.5F : 1.0F;
        const auto before = timers::timers.totals;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        const auto packed = timers::totals_since(timers::timers.totals, before);
        for (const auto value : {timers::MaterialKind::effect, timers::MaterialKind::light})
            OA_CHECK(packed.persistent_materials[static_cast<std::size_t>(value)] == 0);
        OA_CHECK(packed.atlas_emptied == 0);
        OA_CHECK(fixture.scene.frame.effect_area == 40'128);
        OA_CHECK(fixture.scene.passes.effect_materials.size() == 3);
        OA_CHECK(fixture.scene.passes.effect_materials[0].has_value());
        OA_CHECK(fixture.scene.passes.effect_materials[1].has_value());
        OA_CHECK(
            fixture.scene.passes.effect_materials[0] == fixture.scene.passes.effect_materials[2]
        );
        for (const auto& [first_key, first] : fixture.scene.frame.materials)
            for (const auto& [second_key, second] : fixture.scene.frame.materials)
                if (first_key != second_key)
                    OA_CHECK(
                        first.x + first.width <= second.x || second.x + second.width <= first.x ||
                        first.y + first.height <= second.y || second.y + second.height <= first.y
                    );
        if (frame < 2)
            ground = fixture.scene.ground_revision;
        OA_CHECK(fixture.scene.ground_revision == ground);
        live::Scene reference{};
        reference.texture_width = reference.texture_height = 4096;
        OA_CHECK(live::build(fixture.inputs, reference, fixture.error));
        same_effect_picture(fixture.scene, reference);
        if (fixture.scene.passes.effect_materials[0] && fixture.scene.passes.effect_materials[1]) {
            same_effect_texels(fixture.scene, *fixture.scene.passes.effect_materials[0], reference);
            same_effect_texels(fixture.scene, *fixture.scene.passes.effect_materials[1], reference);
        }
        OA_CHECK(
            unit.position.x == 3 * fixed && unit.position.y == 0 && unit.position.z == 8 * fixed
        );
    }
    const auto revision = fixture.scene.texture_revision;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.texture_revision == revision);
    OA_CHECK(
        fixture.scene.atlas_update.frame_first_row == fixture.scene.atlas_update.frame_end_row
    );
    timers::reset(false);
}

/// A tall flash starts the frame arena before persistent animation variants accumulate.
void frame_rows_begin_for_tall_effects() {
    Fixture fixture;
    fixture.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0, 0};
    fixture.inputs.clip_width = fixture.inputs.clip_height = 64;
    oa::formats::gaf::RenderedFrame flash{};
    flash.width = flash.height = 200;
    flash.pixels.assign(40'000, 110);
    flash.coverage.assign(40'000, 1);
    fixture.draws.sprites.push_back({&flash, {4, 4}});
    fixture.draws.sprites[0].world_position = oa::FixedVec3{8 * 65536, 0, 16 * 65536};
    fixture.draws.draws.push_back({oa::app::WorldDrawKind::lit_sprite, 0});
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.frame_rows == 208);
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.ground_kept);
    const auto materials = fixture.scene.materials.size();
    const auto ground = fixture.scene.ground_revision;
    fixture.draws.sprites.resize(128, fixture.draws.sprites[0]);
    for (uint32_t index = 1; index < 128; ++index)
        fixture.draws.draws.push_back({oa::app::WorldDrawKind::lit_sprite, index});
    flash.pixels[0] = 80;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.frame_rows == 208);
    OA_CHECK(fixture.scene.frame.effect_area == 40'000);
    OA_CHECK(fixture.scene.frame.spilled_area == 0);
    OA_CHECK(fixture.scene.materials.size() == materials);
    OA_CHECK(fixture.scene.ground_revision == ground);
    OA_CHECK(fixture.scene.atlas_update.first_row == fixture.scene.atlas_update.end_row);
    for (const auto& rectangle : fixture.scene.passes.effect_materials)
        OA_CHECK(rectangle == fixture.scene.passes.effect_materials[0]);
}

/// Over-cap lists and pictures too tall for frame rows retain the ordinary material path.
void frame_effect_fallbacks() {
    Fixture fixture;
    oa::formats::gaf::RenderedFrame art{};
    art.width = art.height = 4;
    art.pixels.assign(16, ground_index);
    art.coverage.assign(16, 1);
    fixture.draws.sprites.resize(65'537);
    fixture.draws.sprites[0] = {&art, {4, 4}};
    fixture.draws.sprites[0].world_position = oa::FixedVec3{8 * 65536, 8 * 65536, 16 * 65536};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.passes.effect_materials.empty());
    OA_CHECK(fixture.scene.frame.effect_area == 0);
    OA_CHECK(fixture.scene.sprite_count == 1);
    fixture.draws.sprites.resize(1);
    art.pixels[0] = alternate_index;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.passes.effect_materials.size() == 1);
    art.pixels[1] = alternate_index;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.passes.effect_materials[0].has_value());
    // A valid picture taller than the half-atlas arena must still fit the
    // persistent path after a previous frame reserved many transient rows.
    Fixture tall;
    art.width = 16;
    art.height = 2050;
    art.pixels.assign(32'800, ground_index);
    art.coverage.assign(32'800, 1);
    tall.draws.sprites.push_back({&art, {4, 4}});
    tall.draws.sprites[0].world_position = oa::FixedVec3{8 * 65536, 8 * 65536, 16 * 65536};
    tall.map.attribute_width = tall.map.attribute_height = 128;
    tall.map.attributes.assign(128 * 128, {});
    tall.map.tile_width = tall.map.tile_height = 64;
    tall.map.tile_count = 4096;
    tall.map.tile_indices.resize(4096);
    for (uint16_t index = 0; index < 4096; ++index)
        tall.map.tile_indices[index] = index;
    tall.map.tile_palette_indices.assign(4096 * 32 * 32, ground_index);
    tall.inputs.right = tall.inputs.bottom = 2048;
    tall.inputs.sight.mapping = tall.inputs.sight.line_of_sight = false;
    tall.scene.texture_width = tall.scene.texture_height = 4096;
    tall.scene.frame_rows = 2048;
    tall.scene.last_frame_material_area = 8 * 1024 * 1024;
    OA_CHECK(live::build(tall.inputs, tall.scene, tall.error));
    OA_CHECK(!tall.scene.out_of_room);
    OA_CHECK(tall.scene.frame_rows == 0);
    OA_CHECK(tall.scene.sprite_count == 1);
    OA_CHECK(!tall.scene.passes.effect_materials[0]);
    live::Scene reference{};
    reference.texture_width = reference.texture_height = 4096;
    OA_CHECK(live::build(tall.inputs, reference, tall.error));
    same_effect_picture(tall.scene, reference);
    // Culled malformed art must not be decoded by the preparation pass.
    art.pixels.clear();
    tall.inputs.sight.mapping = tall.inputs.sight.line_of_sight = true;
    tall.mapped[0] = 0;
    OA_CHECK(live::build(tall.inputs, tall.scene, tall.error));
    OA_CHECK(tall.scene.sprite_count == 0);
}

/// Retained terrain indices match cache-cleared queries through geometry, camera and FP changes.
void ground_index_reuse() {
    namespace timers = oa::app::bench_timers;
    timers::reset(true);
    Fixture fixture;
    fixture.map.attribute_width = fixture.map.attribute_height = 8;
    fixture.map.attributes.assign(64, {});
    fixture.map.tile_width = fixture.map.tile_height = 4;
    fixture.map.tile_indices.assign(16, 0);
    fixture.inputs.right = fixture.inputs.bottom = 128;
    fixture.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0, 0};
    fixture.inputs.clip_width = fixture.inputs.clip_height = 128;
    fixture.inputs.view_direction = {0, 1, 0.5F};
    std::array<uint16_t, 16> coverage{};
    std::array<uint16_t, 16> mapped{};
    coverage.fill(1);
    mapped.fill(viewer_bit);
    fixture.inputs.sight.coverage = coverage;
    fixture.inputs.sight.player_bits = mapped;
    fixture.inputs.sight.width = fixture.inputs.sight.height = 4;
    oa::formats::gaf::RenderedFrame art{};
    art.width = art.height = 8;
    art.origin_x = 4;
    art.origin_y = 8;
    art.pixels.assign(64, alternate_index);
    art.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&art, {48, 48}});
    fixture.draws.sprites[0].world_position = oa::FixedVec3{48 * 65536, 0, 48 * 65536};
    fixture.draws.sprites[0].surface_effect = true;
    live::Scene reference{};
    const auto compare = [&]() {
        reference.ground_index_cache.valid = false;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(live::build(fixture.inputs, reference, fixture.error));
        same_effect_picture(fixture.scene, reference);
        OA_CHECK(reference.passes.ground_indices.builds == 1);
        OA_CHECK(reference.passes.ground_indices.hits == 0);
    };
    const auto warmed = [&]() {
        compare();
        compare();
        compare();
        OA_CHECK(fixture.scene.passes.ground_indices.hits == 1);
        OA_CHECK(fixture.scene.passes.ground_indices.builds == 0);
        OA_CHECK(fixture.scene.passes.ground_indices.triangles == 0);
        OA_CHECK(fixture.scene.ground_index_cache.valid);
    };
    warmed();
    const auto retained = fixture.scene.ground_index_cache.key.revision;
    fixture.inputs.view_direction = {0.5F, 1, 0.5F};
    compare();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    OA_CHECK(fixture.scene.ground_index_cache.key.revision == retained);
    warmed();
    const auto before_height = fixture.scene.ground_index_cache.key.revision;
    fixture.map.attributes[3 * 8 + 3].height = 12;
    compare();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    warmed();
    OA_CHECK(fixture.scene.ground_index_cache.key.revision != before_height);
    coverage[15] = 0;
    compare();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    warmed();
    fixture.palette[alternate_index * oa::palette_entry_bytes] += 3;
    compare();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    warmed();
    oa::Unit building{};
    building.flags = OA_UNIT_FLAG_BUILDING;
    building.footprint_x = building.footprint_z = 2;
    building.position = {48 * 65536, 8 * 65536, 48 * 65536};
    fixture.draws.models.resize(1);
    fixture.draws.models[0].model.unit = &building;
    fixture.draws.models[0].plan.linked.drawn = true;
    compare();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    OA_CHECK(!fixture.scene.ground_index_cache.valid);
    warmed();
    const auto cut = fixture.scene.ground_index_cache.key.revision;
    building.position.x += 16 * 65536;
    compare();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    OA_CHECK(!fixture.scene.ground_index_cache.valid);
    warmed();
    OA_CHECK(fixture.scene.ground_index_cache.key.revision != cut);
    const int rounding = std::fegetround();
    if (rounding != -1 && std::fesetround(FE_UPWARD) == 0) {
        compare();
        OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
        warmed();
        const auto altered_control = fixture.scene.ground_index_cache.key.float_control;
        OA_CHECK(std::fesetround(rounding) == 0);
        compare();
        OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
        warmed();
        OA_CHECK(fixture.scene.ground_index_cache.key.float_control != altered_control);
    }
    // Copying or moving a Scene retains owned buffers, never a GroundIndex
    // pointing at the other Scene's memory.
    auto copied = fixture.scene;
    OA_CHECK(live::build(fixture.inputs, copied, fixture.error));
    OA_CHECK(copied.passes.ground_indices.hits == 1);
    same_effect_picture(copied, fixture.scene);
    fixture.scene = std::move(copied);
    compare();
    OA_CHECK(fixture.scene.passes.ground_indices.hits == 1);
    // The same indices are safe when sprite bands execute on pool workers.
    oa::platform::job_pool::Pool pool{3};
    fixture.inputs.pool = &pool;
    warmed();
    const auto totals = timers::timers.totals;
    OA_CHECK(totals.ground_index_builds > 0 && totals.ground_index_hits > 0);
    OA_CHECK(totals.ground_index_triangles > 0 && totals.ground_index_wall_ms > 0);
    const auto delta = timers::totals_since(totals, timers::Totals{});
    timers::Totals summed{};
    timers::add_totals(summed, delta);
    OA_CHECK(summed.ground_index_builds == totals.ground_index_builds);
    OA_CHECK(summed.ground_index_hits == totals.ground_index_hits);
    OA_CHECK(summed.ground_index_triangles == totals.ground_index_triangles);
    OA_CHECK(summed.ground_index_wall_ms == totals.ground_index_wall_ms);
    // A failed nonfinite rebuild cannot expose a previously valid bundle.
    auto clean = fixture.scene;
    fixture.inputs.pool = nullptr;
    fixture.inputs.view_direction = {-0.5F, 1, 0.5F};
    OA_CHECK(!fixture.scene.cut_ground.empty());
    if (!fixture.scene.cut_ground.empty()) {
        const auto before = fixture.scene.ground_index_cache;
        discard_ground_points(fixture.scene);
        fixture.scene.cut_ground[0].x = std::numeric_limits<float>::quiet_NaN();
        std::ignore = live::build(fixture.inputs, fixture.scene, fixture.error);
        OA_CHECK(!fixture.scene.ground_index_cache.valid);
        OA_CHECK(fixture.scene.ground_index_cache.attempts == before.attempts + 1);
        OA_CHECK(fixture.scene.ground_index_cache.key == before.key);
        const auto& records = fixture.scene.passes.ground_indices.diagnostics;
        OA_CHECK(records.size == 1 && records.dropped == 0);
        if (records.size == 1) {
            const auto& record = records.records[0];
            OA_CHECK(record.was_valid && !record.completed);
            OA_CHECK(record.before == before.key && record.previous_attempts == before.attempts);
            OA_CHECK(record.requested.revision == before.key.revision);
            OA_CHECK(record.requested.vertices == fixture.scene.cut_ground.size());
            OA_CHECK(record.reasons == static_cast<uint32_t>(live::GroundIndexMiss::index_axes));
            OA_CHECK(record.world_nanoseconds == 0 && record.card_nanoseconds == 0);
            OA_CHECK(record.total_nanoseconds == record.bounds_nanoseconds);
        }
    }
    fixture.scene = std::move(clean);
    timers::reset(true);

    Fixture empty;
    empty.map.attribute_width = empty.map.attribute_height = 1;
    empty.map.attributes.resize(1);
    empty.inputs.right = empty.inputs.bottom = 0;
    empty.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0, 0};
    empty.inputs.clip_width = empty.inputs.clip_height = 32;
    empty.draws.sprites.push_back({&art, {4, 4}});
    empty.draws.sprites[0].world_position = oa::FixedVec3{4 * 65536, 0, 8 * 65536};
    empty.draws.sprites[0].surface_effect = true;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        timers::clear_ground_index_builds();
        OA_CHECK(live::build(empty.inputs, empty.scene, empty.error));
        const auto& work = empty.scene.passes.ground_indices;
        OA_CHECK(work.diagnostics.dropped == 0);
        if (frame < 2) {
            OA_CHECK(work.diagnostics.size == 1 && work.builds == 1);
            if (work.diagnostics.size == 1) {
                const auto& record = work.diagnostics.records[0];
                OA_CHECK(record.completed && record.requested.vertices == 0);
                OA_CHECK(record.previous_attempts == frame);
                OA_CHECK(record.total_nanoseconds == work.build_nanoseconds);
            }
            OA_CHECK(timers::timers.ground_index_builds.size == 1);
        } else {
            OA_CHECK(work.diagnostics.size == 0 && work.builds == 0 && work.hits == 1);
            OA_CHECK(timers::timers.ground_index_builds.size == 0);
        }
    }
    OA_CHECK(empty.scene.terrain_vertex_count == 0);
    OA_CHECK(empty.scene.passes.ground_indices.hits == 1);
    OA_CHECK(empty.scene.ground_index_cache.layouts[0].columns == 0);
    OA_CHECK(empty.scene.ground_index_cache.layouts[0].rows == 0);
    OA_CHECK(empty.scene.ground_index_cache.attempts == 2);
    timers::reset(false);
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

/// Deferred ground queries distinguish first cache use from a later key mismatch.
void ground_index_build_diagnostics() {
    namespace timers = oa::app::bench_timers;
    Fixture fixture;
    auto& in = fixture.inputs;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.clip_width = in.clip_height = 32;
    in.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0, 0};
    in.view_direction = {0, 1, 0.5F};
    oa::formats::gaf::RenderedFrame art{};
    art.width = art.height = 8;
    art.origin_x = 4;
    art.origin_y = 8;
    art.pixels.assign(64, alternate_index);
    art.coverage.assign(64, 1);
    fixture.draws.sprites.push_back({&art, {8, 8}});
    fixture.draws.sprites[0].world_position = oa::FixedVec3{8 * 65536, 0, 8 * 65536};
    live::Scene ordinary;
    const auto compare = [&] {
        timers::reset(false);
        OA_CHECK(live::build(in, ordinary, fixture.error));
        timers::reset(true);
        OA_CHECK(live::build(in, fixture.scene, fixture.error));
        OA_CHECK(fixture.error.empty());
        OA_CHECK(same_frame(fixture.scene, ordinary));
    };
    // Ordinary standing sprites never request these ground indices. The
    // cache stays unused while the scene and material atlas become warm.
    for (uint32_t frame = 0; frame < 5; ++frame) {
        compare();
        const auto& work = fixture.scene.passes.ground_indices;
        OA_CHECK(work.builds == 0 && work.hits == 0 && work.diagnostics.size == 0);
        OA_CHECK(fixture.scene.ground_index_cache.attempts == 0);
        OA_CHECK(!fixture.scene.ground_index_cache.valid);
        OA_CHECK(timers::timers.ground_index_builds.size == 0);
    }
    fixture.draws.sprites[0].surface_effect = true;
    compare();
    const auto& first_work = fixture.scene.passes.ground_indices;
    OA_CHECK(first_work.builds == 1 && first_work.hits == 0);
    OA_CHECK(first_work.diagnostics.size == 1 && first_work.diagnostics.dropped == 0);
    if (first_work.diagnostics.size == 1) {
        const auto& record = first_work.diagnostics.records[0];
        OA_CHECK(!record.was_valid && record.completed && record.previous_attempts == 0);
        OA_CHECK(!record.constructors_parallel);
        OA_CHECK(record.before == live::GroundIndexKey{});
        OA_CHECK(record.requested == fixture.scene.ground_index_cache.key);
        OA_CHECK(record.requested.revision != 0 && record.requested.vertices != 0);
        constexpr auto invalid = static_cast<uint32_t>(live::GroundIndexMiss::index_invalid);
        constexpr auto uncached = static_cast<uint32_t>(live::GroundIndexMiss::index_uncached);
        OA_CHECK((record.reasons & invalid) != 0 && (record.reasons & uncached) == 0);
        OA_CHECK(
            record.total_nanoseconds ==
            record.bounds_nanoseconds + record.world_nanoseconds + record.card_nanoseconds
        );
        OA_CHECK(record.total_nanoseconds == first_work.build_nanoseconds);
        const auto& global = timers::timers.ground_index_builds;
        OA_CHECK(global.size == 1 && global.dropped == 0);
        if (global.size == 1) {
            OA_CHECK(global.records[0].before == record.before);
            OA_CHECK(global.records[0].requested == record.requested);
            OA_CHECK(global.records[0].total_nanoseconds == record.total_nanoseconds);
            OA_CHECK(global.records[0].constructors_parallel == record.constructors_parallel);
        }
    }
    const auto first_key = fixture.scene.ground_index_cache.key;
    compare();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 0);
    OA_CHECK(fixture.scene.passes.ground_indices.hits == 1);
    OA_CHECK(fixture.scene.passes.ground_indices.diagnostics.size == 0);
    OA_CHECK(timers::timers.ground_index_builds.size == 0);
    OA_CHECK(fixture.scene.ground_index_cache.attempts == 1);
    in.view_direction = {0.5F, 1, 0.5F};
    compare();
    const auto& changed = fixture.scene.passes.ground_indices.diagnostics;
    OA_CHECK(changed.size == 1 && changed.dropped == 0);
    if (changed.size == 1) {
        const auto& record = changed.records[0];
        OA_CHECK(record.was_valid && record.completed && record.previous_attempts == 1);
        OA_CHECK(record.before == first_key);
        OA_CHECK(record.requested.revision == first_key.revision);
        OA_CHECK(record.requested.vertices == first_key.vertices);
        OA_CHECK(record.requested.card_axes != first_key.card_axes);
        OA_CHECK(record.reasons == static_cast<uint32_t>(live::GroundIndexMiss::index_axes));
    }
    // A copied Scene owns its index buffers and its cache-local history.
    // Clearing the benchmark's records never resets either cache's history.
    auto copied = fixture.scene;
    const auto old_key = fixture.scene.ground_index_cache.key;
    const auto old_attempts = fixture.scene.ground_index_cache.attempts;
    in.view_direction = {-0.5F, 1, 0.5F};
    timers::clear_ground_index_builds();
    OA_CHECK(live::build(in, copied, fixture.error));
    OA_CHECK(copied.ground_index_cache.attempts == old_attempts + 1);
    OA_CHECK(fixture.scene.ground_index_cache.attempts == old_attempts);
    OA_CHECK(fixture.scene.ground_index_cache.key == old_key);
    fixture.scene = std::move(copied);
    compare();
    OA_CHECK(fixture.scene.ground_index_cache.attempts == old_attempts + 1);
    OA_CHECK(fixture.scene.passes.ground_indices.hits == 1);
    OA_CHECK(fixture.scene.passes.ground_indices.diagnostics.size == 0);
    // Disabled timing changes neither the result nor cache bookkeeping,
    // while avoiding records and phase-clock reads for a forced rebuild.
    const auto before_disabled = fixture.scene.ground_index_cache.attempts;
    fixture.scene.ground_index_cache.valid = false;
    timers::reset(false);
    OA_CHECK(live::build(in, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.ground_index_cache.attempts == before_disabled + 1);
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    OA_CHECK(fixture.scene.passes.ground_indices.build_nanoseconds == 0);
    OA_CHECK(fixture.scene.passes.ground_indices.diagnostics.size == 0);
    OA_CHECK(timers::timers.ground_index_builds.size == 0);
    OA_CHECK(same_frame(fixture.scene, ordinary));
}

/// Ground-build records retain their bounded prefix and count overflow without allocation.
void ground_index_build_record_storage() {
    namespace timers = oa::app::bench_timers;
    live::GroundIndexBuilds more{};
    more.size = more.capacity;
    more.dropped = 3;
    for (std::size_t index = 0; index < more.size; ++index) {
        more.records[index].requested.revision = index + 1;
        more.records[index].previous_attempts = index;
    }
    timers::reset(true);
    timers::count_ground_index_builds(more);
    OA_CHECK(timers::timers.ground_index_builds.size == more.capacity);
    OA_CHECK(timers::timers.ground_index_builds.dropped == more.dropped);
    timers::count_ground_index_builds(more);
    OA_CHECK(timers::timers.ground_index_builds.size == more.capacity);
    OA_CHECK(timers::timers.ground_index_builds.dropped == more.capacity + more.dropped * 2);
    for (std::size_t index = 0; index < more.capacity; ++index) {
        const auto& record = timers::timers.ground_index_builds.records[index];
        OA_CHECK(record.requested.revision == index + 1 && record.previous_attempts == index);
    }
    timers::count_ground_indices(1, 2, 3, 4);
    const auto counted = timers::timers.totals;
    timers::clear_ground_index_builds();
    OA_CHECK(timers::timers.ground_index_builds.size == 0);
    OA_CHECK(timers::timers.ground_index_builds.dropped == 0);
    OA_CHECK(timers::timers.totals.ground_index_builds == counted.ground_index_builds);
    OA_CHECK(timers::timers.totals.ground_index_hits == counted.ground_index_hits);
    timers::reset(false);
    timers::count_ground_index_builds(more);
    OA_CHECK(timers::timers.ground_index_builds.size == 0);
    OA_CHECK(timers::timers.ground_index_builds.dropped == 0);
}

/// Keeps a real saddle-shaped map large enough for several bounds bands, using one tile's art.
struct GroundIndexThreadsFixture {
    Fixture fixture{};
    live::Scene ordinary{};
    oa::formats::gaf::RenderedFrame art{};
    oa::platform::job_pool::Pool one{1}, four{4};

    /// Presents the whole map in a small target without reducing its ground detail.
    /// @param tiles the map's width and height in terrain tiles
    explicit GroundIndexThreadsFixture(uint32_t tiles = 96) {
        const int32_t pixels = static_cast<int32_t>(tiles) * tile_edge;
        const int32_t center = pixels / 2;
        auto& map = fixture.map;
        map.attribute_width = map.attribute_height = tiles * 2;
        map.tile_width = map.tile_height = tiles;
        map.tile_indices.assign(tiles * tiles, 0);
        map.attributes.resize(map.attribute_width * map.attribute_height);
        for (uint32_t y = 0; y < map.attribute_height; ++y)
            for (uint32_t x = 0; x < map.attribute_width; ++x)
                map.attributes[y * map.attribute_width + x].height = ((x + y) & 1U) * 32;
        auto& in = fixture.inputs;
        in.sight.mapping = in.sight.line_of_sight = false;
        in.right = in.bottom = pixels;
        in.clip_width = in.clip_height = 128;
        in.detail_scale = 1;
        const float scale = 128.0F / pixels;
        in.clip_projection = {scale, 0, 0, 0, 0, -0.5F * scale, scale, 0, 0, 1, 0, 0};
        in.view_direction = {0.25F, 1, 0.5F};
        art.width = art.height = 8;
        art.origin_x = 4;
        art.origin_y = 8;
        art.pixels.assign(64, alternate_index);
        art.coverage.assign(64, 1);
        fixture.draws.sprites.push_back({&art, {center, center}});
        fixture.draws.sprites[0].world_position = oa::FixedVec3{center * 65536, 0, center * 65536};
    }

    /// Compares complete rendered frames on one and four drawing threads.
    void build() {
        discard_ground_points(ordinary);
        fixture.inputs.pool = &one;
        OA_CHECK(live::build(fixture.inputs, ordinary, fixture.error));
        OA_CHECK(fixture.error.empty());
        fixture.inputs.pool = &four;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.error.empty());
        OA_CHECK(same_frame(fixture.scene, ordinary));
        if (fixture.scene.ground_points_revision == fixture.scene.ground_revision &&
            fixture.scene.ground_points.size() == fixture.scene.terrain_vertex_count)
            check_ground_points(fixture.scene);
    }

    /// Keeps serial phases additive and bounds concurrent constructor walls by the total.
    /// @param parallel the four-thread scene may construct both indices concurrently
    void check_constructor_times(bool parallel) const {
        const auto check = [](const live::Scene& scene, bool concurrent) {
            const auto& work = scene.passes.ground_indices;
            OA_CHECK(work.builds != 0 && work.diagnostics.size == work.builds);
            OA_CHECK(work.diagnostics.dropped == 0);
            uint64_t total{};
            bool dispatched = false;
            for (std::size_t index = 0; index < work.diagnostics.size; ++index) {
                const auto& record = work.diagnostics.records[index];
                OA_CHECK(record.completed);
                dispatched |= record.constructors_parallel;
                OA_CHECK(record.world_nanoseconds <= record.total_nanoseconds);
                OA_CHECK(record.card_nanoseconds <= record.total_nanoseconds);
                OA_CHECK(
                    record.bounds_nanoseconds +
                        std::max(record.world_nanoseconds, record.card_nanoseconds) <=
                    record.total_nanoseconds
                );
                if (!record.constructors_parallel)
                    OA_CHECK(
                        record.bounds_nanoseconds + record.world_nanoseconds +
                            record.card_nanoseconds ==
                        record.total_nanoseconds
                    );
                total += record.total_nanoseconds;
            }
            // A first real query may reject the prepared caller's FP key
            // and rebuild serially on its actual sprite worker.
            OA_CHECK(dispatched == concurrent);
            OA_CHECK(total == work.build_nanoseconds);
        };
        check(ordinary, false);
        check(fixture.scene, parallel);
    }

    /// Requires the same owned bounds, bucket order and restored scalar layout.
    void check_indices() const {
        const auto& actual = fixture.scene;
        OA_CHECK(actual.ground_index_cache.valid && ordinary.ground_index_cache.valid);
        OA_CHECK(
            actual.ground_index_cache.key.vertices == ordinary.ground_index_cache.key.vertices
        );
        OA_CHECK(
            actual.ground_index_cache.key.card_axes == ordinary.ground_index_cache.key.card_axes
        );
        for (std::size_t index = 0; index < actual.ground_index_memory.size(); ++index) {
            const auto& a = actual.ground_index_memory[index];
            const auto& b = ordinary.ground_index_memory[index];
            OA_CHECK(a.bounds.size() == b.bounds.size());
            if (a.bounds.size() == b.bounds.size() && !a.bounds.empty())
                OA_CHECK(
                    std::memcmp(
                        a.bounds.data(), b.bounds.data(), a.bounds.size() * sizeof(a.bounds[0])
                    ) == 0
                );
            OA_CHECK(
                a.starts == b.starts && a.triangles == b.triangles && a.oversized == b.oversized
            );
            OA_CHECK(
                std::equal(
                    a.buckets.begin(),
                    a.buckets.end(),
                    b.buckets.begin(),
                    b.buckets.end(),
                    [](const auto& left, const auto& right) {
                        return left.bucket == right.bucket && left.columns == right.columns &&
                               left.rows == right.rows;
                    }
                )
            );
            const auto& layout = actual.ground_index_cache.layouts[index];
            const auto& other = ordinary.ground_index_cache.layouts[index];
            OA_CHECK(
                layout.low == other.low && layout.high == other.high && layout.reach == other.reach
            );
            OA_CHECK(layout.edge == other.edge && layout.inverse_edge == other.inverse_edge);
            OA_CHECK(
                layout.columns == other.columns && layout.rows == other.rows &&
                layout.listed_reach == other.listed_reach
            );
        }
    }
};

/// Compact coordinates retain exact queries across ownership, fallback and ground generations.
void ground_points_preserve_queries() {
    namespace timers = oa::app::bench_timers;
    timers::reset(true);
    GroundIndexThreadsFixture pair{4};
    auto& fixture = pair.fixture;
    auto& scene = fixture.scene;
    auto& in = fixture.inputs;
    const auto compare = [&] {
        scene.ground_index_cache.valid = pair.ordinary.ground_index_cache.valid = false;
        pair.build();
        pair.check_indices();
        OA_CHECK(scene.passes.ground_indices.builds == 1);
    };
    const auto publish = [&](bool remembered) {
        fixture.draws.sprites[0].surface_effect = false;
        for (uint32_t frame = 0; frame < 3; ++frame)
            pair.build();
        fixture.draws.sprites[0].surface_effect = true;
        compare();
        if (remembered) {
            OA_CHECK(scene.ground_points_revision != 0);
            OA_CHECK(scene.ground_points_revision == scene.ground_index_cache.key.revision);
            check_ground_points(scene);
        }
    };
    publish(true);
    const auto points = scene.ground_points;
    const auto revision = scene.ground_points_revision;
    OA_CHECK(!points.empty() && points.size() == scene.terrain_vertex_count);
    if (points.empty()) {
        timers::reset(false);
        return;
    }
    const auto restore = [&] {
        scene.ground_points = points;
        scene.ground_points_revision = revision;
    };
    // Poisoned optional data must remain unread when any selection key is
    // absent or stale. Every reference query reads original retained vertices.
    for (uint32_t fallback = 0; fallback < 4; ++fallback) {
        restore();
        scene.ground_points[0].x = std::numeric_limits<float>::quiet_NaN();
        if (fallback == 0)
            scene.ground_points_revision = 0;
        else if (fallback == 1)
            ++scene.ground_points_revision;
        else if (fallback == 2)
            scene.ground_points.pop_back();
        else
            scene.ground_points.clear();
        compare();
    }
    restore();
    auto copied = scene;
    OA_CHECK(copied.ground_points.data() != scene.ground_points.data());
    copied.ground_points[0].x = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(
        std::bit_cast<uint32_t>(scene.ground_points[0].x) == std::bit_cast<uint32_t>(points[0].x)
    );
    copied.ground_points[0] = points[0];
    const auto* owned = copied.ground_points.data();
    auto moved = std::move(copied);
    OA_CHECK(moved.ground_points.data() == owned);
    scene = std::move(moved);
    compare();
    // The selected payload's nonfinite point cannot leave an older valid
    // bundle published, even when its original GPU vertex is still finite.
    const auto attempts = scene.ground_index_cache.attempts;
    scene.ground_points[0].x = std::numeric_limits<float>::quiet_NaN();
    scene.ground_index_cache.valid = false;
    in.pool = &pair.four;
    OA_CHECK(live::build(in, scene, fixture.error));
    OA_CHECK(!scene.ground_index_cache.valid);
    OA_CHECK(scene.ground_index_cache.attempts == attempts + 1);
    const auto& diagnostics = scene.passes.ground_indices.diagnostics;
    OA_CHECK(diagnostics.size == 1 && diagnostics.dropped == 0);
    if (diagnostics.size == 1)
        OA_CHECK(!diagnostics.records[0].completed);
    restore();
    compare();
    // Matching source and compact signed zeros preserve the bounds' raw
    // bits, including the ordered min/max choice among equal corners.
    bool signed_zero = false;
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (points[index].x != 0)
            continue;
        for (auto* current : {&scene, &pair.ordinary}) {
            current->ground_vertices[index].x = -0.0F;
            current->listed_ground_revision = 0;
        }
        scene.ground_points[index].x = -0.0F;
        compare();
        OA_CHECK(std::bit_cast<uint32_t>(scene.ground_points[index].x) == uint32_t{0x80000000});
        for (auto* current : {&scene, &pair.ordinary}) {
            current->ground_vertices[index].x = points[index].x;
            current->listed_ground_revision = 0;
        }
        restore();
        compare();
        signed_zero = true;
        break;
    }
    OA_CHECK(signed_zero);
    // Editing the same map object replaces its generation. Foundation
    // cuts and later uncut ground must each match the original query path.
    fixture.map.attributes[3].height = 12;
    publish(true);
    OA_CHECK(scene.ground_points_revision != revision);
    oa::Unit building{};
    building.flags = OA_UNIT_FLAG_BUILDING;
    building.footprint_x = building.footprint_z = 2;
    building.position = {64 * 65536, 8 * 65536, 64 * 65536};
    fixture.draws.models.resize(1);
    fixture.draws.models[0].model.unit = &building;
    fixture.draws.models[0].plan.linked.drawn = true;
    publish(true);
    OA_CHECK(scene.cut_source != 0);
    const auto cut = scene.ground_points_revision;
    building.position.x += cell_edge * 65536;
    publish(true);
    OA_CHECK(scene.ground_points_revision != cut);
    fixture.draws.models.clear();
    publish(false);
    in.clip_projection[3] += 1;
    publish(false);
    timers::reset(false);
}

/// Cold bounds workers preserve actual demand, material replay and exact index contents.
void ground_index_bounds_on_threads() {
    namespace timers = oa::app::bench_timers;
    timers::reset(true);
    GroundIndexThreadsFixture pair;
    auto& fixture = pair.fixture;
    auto& in = fixture.inputs;
    auto& sprite = fixture.draws.sprites[0];
    for (uint32_t frame = 0; frame < 3; ++frame)
        pair.build();
    OA_CHECK(fixture.scene.terrain_vertex_count >= 3 * 32768);
    OA_CHECK(fixture.scene.ground_kept && pair.ordinary.ground_kept);
    OA_CHECK(pair.four.threads() >= 2);
    const auto unused = [&] {
        pair.build();
        OA_CHECK(fixture.scene.ground_index_cache.attempts == 0);
        OA_CHECK(pair.ordinary.ground_index_cache.attempts == 0);
        OA_CHECK(fixture.scene.passes.ground_indices.builds == 0);
        OA_CHECK(fixture.scene.passes.ground_indices.diagnostics.size == 0);
    };
    sprite.surface_effect = true;
    sprite.world_position->y = -65536;
    unused();
    sprite.world_position->y = 0;
    sprite.terrain_decal = true;
    unused();
    sprite.terrain_decal = false;
    const auto* art = sprite.frame;
    sprite.frame = nullptr;
    unused();
    sprite.frame = art;
    in.sight.mapping = in.sight.line_of_sight = true;
    fixture.coverage[0] = fixture.mapped[0] = 0;
    unused();
    in.sight.mapping = in.sight.line_of_sight = false;
    fixture.coverage[0] = fixture.mapped[0] = viewer_bit;
    sprite.surface_effect = false;
    sprite.on_map = true;
    for (uint32_t frame = 0; frame < 3; ++frame)
        unused();
    // A malformed scenery material fails before its surface query. Frame
    // effect packing deliberately leaves this on-map sprite to its pass.
    sprite.surface_effect = true;
    pair.art.pixels.clear();
    in.pool = &pair.one;
    OA_CHECK(!live::build(in, pair.ordinary, fixture.error));
    const auto ordinary_error = fixture.error;
    in.pool = &pair.four;
    OA_CHECK(!live::build(in, fixture.scene, fixture.error));
    OA_CHECK(!ordinary_error.empty() && fixture.error == ordinary_error);
    OA_CHECK(fixture.scene.ground_index_cache.attempts == 0);
    OA_CHECK(pair.ordinary.ground_index_cache.attempts == 0);
    // A repaired new material must replay packing before its first query.
    pair.art.pixels.assign(64, ground_index);
    pair.build();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    OA_CHECK(fixture.scene.ground_index_cache.attempts == 1);
    pair.check_indices();
    pair.check_constructor_times(false);
    sprite.on_map = false;
    for (uint32_t frame = 0; frame < 2; ++frame) {
        fixture.scene.ground_index_cache.valid = false;
        pair.ordinary.ground_index_cache.valid = false;
        pair.build();
        OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
        OA_CHECK(fixture.scene.passes.ground_indices.diagnostics.size == 1);
        pair.check_indices();
        pair.check_constructor_times(true);
    }
    pair.build();
    OA_CHECK(fixture.scene.passes.ground_indices.hits == 1);
    OA_CHECK(fixture.scene.passes.ground_indices.diagnostics.size == 0);
    // A late eligible sprite crosses a card-band boundary. Reversing the
    // list changes drawing order without changing the resulting index.
    const auto surface = sprite;
    fixture.draws.sprites.assign(70, surface);
    for (auto& draw : fixture.draws.sprites)
        draw.surface_effect = false;
    fixture.draws.sprites[65].surface_effect = true;
    for (uint32_t order = 0; order < 2; ++order) {
        fixture.scene.ground_index_cache.valid = false;
        pair.ordinary.ground_index_cache.valid = false;
        pair.build();
        OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
        pair.check_indices();
        std::reverse(fixture.draws.sprites.begin(), fixture.draws.sprites.end());
    }
    fixture.draws.sprites.assign(1, surface);
    // These cards still request the index before their height/coverage
    // early exits, even when they cannot contribute visible ground lift.
    fixture.draws.sprites[0].world_position->y = 1000 * 65536;
    fixture.scene.ground_index_cache.valid = pair.ordinary.ground_index_cache.valid = false;
    pair.build();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    fixture.draws.sprites[0] = surface;
    std::fill(pair.art.coverage.begin(), pair.art.coverage.end(), 0);
    fixture.scene.ground_index_cache.valid = pair.ordinary.ground_index_cache.valid = false;
    pair.build();
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    std::fill(pair.art.coverage.begin(), pair.art.coverage.end(), 1);
    pair.build();
    // Only one outer band owns the Scene, while the other reserves the
    // pool. Every nested bounds job must finish on its actual caller.
    fixture.scene.ground_index_cache.valid = pair.ordinary.ground_index_cache.valid = false;
    in.pool = &pair.one;
    OA_CHECK(live::build(in, pair.ordinary, fixture.error));
    in.pool = &pair.four;
    bool busy_built = false;
    oa::platform::job_pool::run_bands(&pair.four, 2, [&](uint32_t band) noexcept {
        if (band == 0)
            busy_built = live::build(in, fixture.scene, fixture.error);
    });
    OA_CHECK(busy_built && fixture.error.empty());
    OA_CHECK(same_frame(fixture.scene, pair.ordinary));
    OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
    pair.check_indices();
    pair.check_constructor_times(true);
    // A nonfinite point in a later bounds band leaves the bundle invalid
    // and listed. The actual sprite query must not retry that same failure.
    constexpr std::size_t broken_vertex = 3 * 16384 + 1;
    OA_CHECK(fixture.scene.ground_vertices.size() > broken_vertex);
    OA_CHECK(pair.ordinary.ground_vertices.size() > broken_vertex);
    if (fixture.scene.ground_vertices.size() > broken_vertex &&
        pair.ordinary.ground_vertices.size() > broken_vertex) {
        const auto x = fixture.scene.ground_vertices[broken_vertex].x;
        const auto other_x = pair.ordinary.ground_vertices[broken_vertex].x;
        const auto before = fixture.scene.ground_index_cache.attempts;
        const auto other_before = pair.ordinary.ground_index_cache.attempts;
        discard_ground_points(fixture.scene);
        discard_ground_points(pair.ordinary);
        fixture.scene.ground_vertices[broken_vertex].x = std::numeric_limits<float>::quiet_NaN();
        pair.ordinary.ground_vertices[broken_vertex].x = std::numeric_limits<float>::quiet_NaN();
        fixture.scene.ground_index_cache.valid = pair.ordinary.ground_index_cache.valid = false;
        in.pool = &pair.one;
        const bool ordinary_built = live::build(in, pair.ordinary, fixture.error);
        const auto ordinary_failure = fixture.error;
        in.pool = &pair.four;
        const bool parallel_built = live::build(in, fixture.scene, fixture.error);
        OA_CHECK(parallel_built == ordinary_built && fixture.error == ordinary_failure);
        OA_CHECK(!fixture.scene.ground_index_cache.valid);
        OA_CHECK(!pair.ordinary.ground_index_cache.valid);
        OA_CHECK(fixture.scene.ground_index_cache.attempts == before + 1);
        OA_CHECK(pair.ordinary.ground_index_cache.attempts == other_before + 1);
        OA_CHECK(fixture.scene.passes.ground_indices.builds == 1);
        const auto& records = fixture.scene.passes.ground_indices.diagnostics;
        OA_CHECK(records.size == 1);
        if (records.size == 1)
            OA_CHECK(!records.records[0].completed);
        fixture.scene.ground_vertices[broken_vertex].x = x;
        pair.ordinary.ground_vertices[broken_vertex].x = other_x;
        pair.build();
        pair.check_indices();
    }
    // A busy pool's actual caller may be a worker. Give that caller the
    // same nondefault environment as the reference and restore it afterward.
    std::fenv_t original{};
    if (std::fegetenv(&original) == 0) {
        if (std::fesetround(FE_UPWARD) == 0) {
            std::fenv_t requested{};
            if (std::fegetenv(&requested) == 0) {
                fixture.scene.ground_index_cache.valid = pair.ordinary.ground_index_cache.valid =
                    false;
                in.pool = &pair.one;
                OA_CHECK(live::build(in, pair.ordinary, fixture.error));
                const auto expected_control = pair.ordinary.ground_index_cache.key.float_control;
                OA_CHECK(std::fesetenv(&original) == 0);
                in.pool = &pair.four;
                bool built = false, retained = false, restored = false;
                oa::platform::job_pool::run_bands(&pair.four, 2, [&](uint32_t band) noexcept {
                    if (band != 0)
                        return;
                    std::fenv_t worker_before{};
                    if (std::fegetenv(&worker_before) != 0)
                        return;
                    if (std::fesetenv(&requested) == 0) {
                        built = live::build(in, fixture.scene, fixture.error);
                        retained = std::fegetround() == FE_UPWARD;
                    }
                    restored = std::fesetenv(&worker_before) == 0;
                });
                OA_CHECK(built && retained && restored);
                OA_CHECK(same_frame(fixture.scene, pair.ordinary));
                OA_CHECK(fixture.scene.ground_index_cache.key.float_control == expected_control);
                pair.check_indices();
                pair.check_constructor_times(true);
            }
        }
        OA_CHECK(std::fesetenv(&original) == 0);
    }
    // A null pool preserves the serial phase sum while producing the
    // same bounds, buckets, layout and picture as both constructor jobs.
    fixture.scene.ground_index_cache.valid = pair.ordinary.ground_index_cache.valid = false;
    in.pool = nullptr;
    OA_CHECK(live::build(in, pair.ordinary, fixture.error));
    OA_CHECK(fixture.error.empty());
    in.pool = &pair.four;
    OA_CHECK(live::build(in, fixture.scene, fixture.error));
    OA_CHECK(fixture.error.empty());
    OA_CHECK(same_frame(fixture.scene, pair.ordinary));
    pair.check_indices();
    pair.check_constructor_times(true);
    // A card parallel to world height has no ground-plane card index;
    // even a free multi-thread pool retains the serial constructor path.
    in.view_direction = {0, 0, 1};
    pair.build();
    OA_CHECK(!fixture.scene.ground_index_cache.key.card);
    OA_CHECK(fixture.scene.ground_index_memory[1].bounds.empty());
    pair.check_indices();
    pair.check_constructor_times(false);
    timers::reset(false);
}

/// Revision labels reuse indices only after ordered bounds, reach and query context match exactly.
void ground_index_bounds_revision_reuse() {
    namespace timers = oa::app::bench_timers;
    timers::reset(true);
    GroundIndexThreadsFixture pair;
    auto& fixture = pair.fixture;
    for (uint32_t frame = 0; frame < 3; ++frame)
        pair.build();
    fixture.draws.sprites[0].surface_effect = true;
    pair.build();
    pair.check_indices();
    OA_CHECK(fixture.scene.terrain_vertex_count >= 3 * 32768);
    auto& cache = fixture.scene.ground_index_cache;
    const auto revision_miss = [&] {
        OA_CHECK(cache.valid && cache.key.revision != 0);
        ++cache.key.revision;
    };
    const auto compare = [&](bool reused) {
        const auto before = cache.attempts;
        // The reference always constructs both indices independently,
        // even when its recomputed bounds happen to equal its old ones.
        pair.ordinary.ground_index_cache.valid = false;
        pair.build();
        pair.check_indices();
        OA_CHECK(cache.attempts == before + 1);
        const auto& work = fixture.scene.passes.ground_indices;
        const auto& records = work.diagnostics;
        OA_CHECK(work.builds == 1 && work.hits == 0);
        OA_CHECK(records.size == 1 && records.dropped == 0);
        if (records.size == 1) {
            const auto& record = records.records[0];
            OA_CHECK(record.completed && record.requested == cache.key);
            if (reused) {
                OA_CHECK(record.was_valid);
                OA_CHECK(
                    record.reasons == static_cast<uint32_t>(live::GroundIndexMiss::index_revision)
                );
                OA_CHECK(record.world_nanoseconds == 0 && record.card_nanoseconds == 0);
                OA_CHECK(record.bounds_nanoseconds == record.total_nanoseconds);
            } else {
                OA_CHECK(record.world_nanoseconds != 0 && record.card_nanoseconds != 0);
            }
        }
        const auto& full = pair.ordinary.passes.ground_indices.diagnostics;
        OA_CHECK(full.size == 1);
        if (full.size == 1)
            OA_CHECK(
                full.records[0].world_nanoseconds != 0 && full.records[0].card_nanoseconds != 0
            );
    };
    for (uint32_t frame = 0; frame < 2; ++frame) {
        revision_miss();
        compare(true);
    }
    // Indices hold horizontal bounds, not copies of terrain heights.
    // Equal bounds must still query the current, raised triangles.
    const auto effect_first = fixture.scene.terrain_vertex_count;
    OA_CHECK(effect_first < fixture.scene.vertices.size());
    if (effect_first < fixture.scene.vertices.size()) {
        const auto old_height = fixture.scene.vertices[effect_first].height;
        for (auto* scene : {&fixture.scene, &pair.ordinary}) {
            discard_ground_points(*scene);
            for (auto& vertex : scene->ground_vertices)
                vertex.height += 4;
            scene->listed_ground_revision = 0;
        }
        revision_miss();
        compare(true);
        OA_CHECK(
            fixture.scene.vertices[fixture.scene.terrain_vertex_count].height > old_height + 3.9F
        );
    }
    // Moving an extreme vertex changes a bound while preserving the
    // triangle order. The repaired index must match a complete rebuild.
    for (const bool along_z : {false, true}) {
        const auto old = pair.ordinary.ground_index_memory[0].bounds[0];
        for (auto* scene : {&fixture.scene, &pair.ordinary}) {
            discard_ground_points(*scene);
            if (along_z)
                scene->ground_vertices[0].z -= 4 * cell_edge;
            else
                scene->ground_vertices[0].x -= 4 * cell_edge;
            scene->listed_ground_revision = 0;
        }
        revision_miss();
        compare(false);
        const auto now = pair.ordinary.ground_index_memory[0].bounds[0];
        OA_CHECK(std::memcmp(old.data(), now.data(), sizeof(old)) != 0);
    }
    const auto reject = [&](auto change) {
        revision_miss();
        change();
        compare(false);
    };
    reject([&] { cache.valid = false; });
    reject([&] { cache.key.revision = 0; });
    reject([&] { cache.key.vertices -= 3; });
    reject([&] { cache.key.card = !cache.key.card; });
    reject([&] { cache.key.card_axes[0] ^= uint64_t{1}; });
    reject([&] { cache.key.float_control[0] ^= uint64_t{1}; });
    for (std::size_t index = 0; index < fixture.scene.ground_index_memory.size(); ++index) {
        reject([&] { fixture.scene.ground_index_memory[index].bounds.pop_back(); });
        reject([&] {
            cache.layouts[index].reach =
                std::nextafter(cache.layouts[index].reach, std::numeric_limits<double>::infinity());
        });
    }
    // Numerically equal signed zero still names different stored bounds.
    auto& bounds = fixture.scene.ground_index_memory[0].bounds;
    bool tested_zero = false;
    for (auto& bound : bounds) {
        for (auto& value : bound) {
            if (value != 0)
                continue;
            const auto original = std::bit_cast<uint32_t>(value);
            reject([&] { value = std::bit_cast<float>(original ^ uint32_t{0x80000000}); });
            tested_zero = true;
            break;
        }
        if (tested_zero)
            break;
    }
    OA_CHECK(tested_zero);
    fixture.inputs.view_direction = {-0.25F, 1, 0.5F};
    revision_miss();
    compare(false);
    pair.build();
    OA_CHECK(fixture.scene.passes.ground_indices.hits == 1);
    OA_CHECK(fixture.scene.passes.ground_indices.diagnostics.size == 0);
    // The last fixed-point value below this sea rounds onto its surface
    // in one environment and remains submerged in the other. Its actual
    // caller keeps that choice, including whether it queries the ground.
    std::fenv_t original{};
    if (std::fegetenv(&original) == 0) {
        fixture.inputs.sea_level = 512;
        fixture.draws.sprites[0].world_position->y = 512 * 65536 - 1;
        for (const auto rounding : {FE_TONEAREST, FE_DOWNWARD}) {
            if (std::fesetround(rounding) != 0)
                continue;
            std::fenv_t requested{};
            if (std::fegetenv(&requested) != 0)
                continue;
            cache.valid = pair.ordinary.ground_index_cache.valid = false;
            fixture.inputs.pool = &pair.one;
            OA_CHECK(live::build(fixture.inputs, pair.ordinary, fixture.error));
            fixture.inputs.pool = &pair.four;
            bool built = false, restored = false;
            oa::platform::job_pool::run_bands(&pair.four, 2, [&](uint32_t band) noexcept {
                if (band != 0)
                    return;
                std::fenv_t before{};
                if (std::fegetenv(&before) != 0)
                    return;
                if (std::fesetenv(&requested) == 0)
                    built = live::build(fixture.inputs, fixture.scene, fixture.error);
                restored = std::fesetenv(&before) == 0;
            });
            OA_CHECK(built && restored && fixture.error.empty());
            OA_CHECK(same_frame(fixture.scene, pair.ordinary));
            const uint64_t expected_builds = rounding == FE_TONEAREST ? 1 : 0;
            OA_CHECK(fixture.scene.passes.ground_indices.builds == expected_builds);
            OA_CHECK(pair.ordinary.passes.ground_indices.builds == expected_builds);
        }
        OA_CHECK(std::fesetenv(&original) == 0);
    }
    timers::reset(false);
}

/// Builds matching scene histories while the reference surveys afresh on every frame.
struct TerrainDemandPair {
    Fixture fixture{};
    live::Scene ordinary{};
    std::vector<uint16_t> coverage{};
    std::vector<uint16_t> mapped{};
    oa::Unit marker{};

    /// Prepares a wide view whose tiles require the full terrain survey.
    TerrainDemandPair() {
        constexpr uint32_t tiles = 80;
        constexpr uint32_t pixels = 512;
        constexpr int32_t center = static_cast<int32_t>(tiles) * tile_edge / 2;
        constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
        wide_terrain(fixture, tiles, tiles);
        coverage.assign(tiles * tiles, viewer_bit);
        mapped.assign(coverage.size(), viewer_bit);
        fixture.inputs.sight.coverage = coverage;
        fixture.inputs.sight.player_bits = mapped;
        fixture.inputs.sight.width = fixture.inputs.sight.height = tiles;
        fixture.inputs.sight.mapping = true;
        fixture.inputs.sight.line_of_sight = true;
        fixture.inputs.clip_width = fixture.inputs.clip_height = pixels;
        fixture.inputs.unit_marker_pixels = 5;
        fixture.inputs.owner_colors[0] = {255, 80, 20};
        OA_CHECK(
            oa::app::renderer_live_camera::projection(
                {}, {center, 0, center, pixels, pixels, 0.15F}, fixture.inputs.clip_projection
            )
        );
        marker.id = 17;
        marker.position = {center * fixed, 0, center * fixed};
        fixture.draws.unit_markers = {{&marker, marker.position}};
    }

    /// Compares all emitted geometry, texels, picking ranges and upload rectangles.
    std::size_t build() {
        ordinary.terrain_demand_cache = {};
        std::string ordinary_error;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(live::build(fixture.inputs, ordinary, ordinary_error));
        OA_CHECK(fixture.error.empty() && ordinary_error.empty());
        OA_CHECK(same_frame(fixture.scene, ordinary));
        OA_CHECK(
            std::equal(
                fixture.scene.atlas_update.regions.begin(),
                fixture.scene.atlas_update.regions.end(),
                ordinary.atlas_update.regions.begin(),
                ordinary.atlas_update.regions.end(),
                [](const auto& a, const auto& b) {
                    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
                }
            )
        );
        OA_CHECK(ordinary.terrain_demand_hits == 0);
        OA_CHECK(
            fixture.scene.terrain_demand_cache.storage_bytes() <=
            live::TerrainDemandCache::storage_limit
        );
        return fixture.scene.terrain_demand_hits;
    }
};

/// Repeated surveys reuse only exact source bytes, including edits to the same map.
void terrain_demand_exact_sources() {
    TerrainDemandPair pair;
    auto& fixture = pair.fixture;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(fixture.scene.terrain_demand_cache.valid);
    OA_CHECK(!fixture.scene.terrain_demand_cache.demand.empty());
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    OA_CHECK(pair.build() == 1);

    const auto height_cell = fixture.map.attribute_width * 80 + 80;
    fixture.map.attributes[height_cell].height = 28;
    OA_CHECK(pair.build() == 0);
    fixture.map.attributes[height_cell].height = 0;
    OA_CHECK(pair.build() == 0);
    const auto tile = fixture.map.tile_width * 40 + 40;
    fixture.map.tile_indices[tile] = static_cast<uint16_t>(tile + 1);
    OA_CHECK(pair.build() == 0);
    fixture.map.tile_indices[tile] = static_cast<uint16_t>(tile);
    OA_CHECK(pair.build() == 0);

    fixture.inputs.clip_projection[8] += 0.125F;
    OA_CHECK(pair.build() == 0);
    fixture.inputs.left = cell_edge;
    fixture.inputs.right -= cell_edge + 1;
    OA_CHECK(pair.build() == 0);

    // The memo owns its source bytes; a replacement map does not borrow the old map's buffers.
    {
        auto replacement = fixture.map;
        fixture.inputs.map = &replacement;
        OA_CHECK(pair.build() == 0);
        replacement.attributes[height_cell].height = 31;
        OA_CHECK(pair.build() == 0);
    }
    fixture.inputs.map = &fixture.map;
    OA_CHECK(pair.build() == 0);
    fixture.scene = {};
    pair.ordinary = {};
    OA_CHECK(pair.build() == 0);
    OA_CHECK(pair.build() == 1);
}

/// Only a change between black and known fog changes the survey's tile keys.
void terrain_demand_fog_sources() {
    TerrainDemandPair pair;
    auto& fixture = pair.fixture;
    const auto tile = fixture.map.tile_width * 40 + 40;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(pair.build() == 1);
    std::fill(pair.coverage.begin(), pair.coverage.end(), 0);
    OA_CHECK(pair.build() == 1);
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    if (!fixture.scene.unit_ranges.empty())
        OA_CHECK(fixture.scene.vertices[fixture.scene.unit_ranges[0].first_vertex].brightness < 1);
    std::fill(pair.coverage.begin(), pair.coverage.end(), viewer_bit);
    OA_CHECK(pair.build() == 1);

    pair.mapped[tile] = 0;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(fixture.scene.unit_ranges.empty());
    OA_CHECK(pair.build() == 1);
    constexpr uint16_t ally_bit = 2;
    pair.mapped[tile] = ally_bit;
    OA_CHECK(pair.build() == 1);
    fixture.inputs.sight.shared_bits = ally_bit;
    fixture.inputs.sight.mapped_bits = viewer_bit | ally_bit;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    OA_CHECK(pair.build() == 1);
    pair.mapped[tile] = viewer_bit;
    fixture.inputs.sight.shared_bits = 0;
    fixture.inputs.sight.mapped_bits = viewer_bit;
    OA_CHECK(pair.build() == 1);

    --fixture.inputs.sight.width;
    OA_CHECK(pair.build() == 0);
    ++fixture.inputs.sight.width;
    OA_CHECK(pair.build() == 0);
    pair.mapped[tile] = 0;
    OA_CHECK(pair.build() == 0);
    fixture.inputs.sight.mapping = false;
    OA_CHECK(pair.build() == 0);
    fixture.inputs.sight.mapping = true;
    OA_CHECK(pair.build() == 0);
}

/// Palette, art, water and screen-only camera changes still build their current frame.
void terrain_demand_other_frame_inputs() {
    TerrainDemandPair pair;
    auto& fixture = pair.fixture;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(pair.build() == 1);
    fixture.palette[wide_first_colour * oa::palette_entry_bytes] = 99;
    OA_CHECK(pair.build() == 1);
    fixture.map.tile_palette_indices[0] = alternate_index;
    OA_CHECK(pair.build() == 1);
    fixture.inputs.sea_level = 6;
    fixture.inputs.water.seconds = 3;
    OA_CHECK(pair.build() == 1);
    fixture.inputs.water.seconds = 5;
    OA_CHECK(pair.build() == 1);

    fixture.inputs.tiles_hold_while_moving = true;
    fixture.inputs.clip_projection[3] += 5;
    OA_CHECK(pair.build() == 1);
    // The last held frame and the settled frame use the same exact source survey.
    fixture.scene.camera_settling_frames = pair.ordinary.camera_settling_frames = 1;
    OA_CHECK(pair.build() == 1);
    OA_CHECK(fixture.scene.camera_settling_frames == 0);
    OA_CHECK(pair.build() == 1);
}

/// A different rounding mode cannot reuse a survey completed under the old mode.
void terrain_demand_rounding_control() {
    const int saved = std::fegetround();
    OA_CHECK(saved != -1);
    if (saved == -1)
        return;

    struct RestoreRounding {
        int mode{};

        ~RestoreRounding() { std::fesetround(mode); }
    } restore{saved};

    const bool nearest = std::fesetround(FE_TONEAREST) == 0;
    OA_CHECK(nearest);
    if (!nearest)
        return;
    TerrainDemandPair pair;
    OA_CHECK(pair.build() == 0);
    const bool upward = std::fesetround(FE_UPWARD) == 0;
    OA_CHECK(upward);
    if (!upward)
        return;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(pair.build() == 1);
    OA_CHECK(std::fesetround(FE_TONEAREST) == 0);
    OA_CHECK(pair.build() == 0);
}

/// Fast empty surveys and atlas-room changes retain the ordinary planning decisions.
void terrain_demand_room_and_empty_views() {
    TerrainDemandPair pair;
    auto& fixture = pair.fixture;
    const auto whole_right = fixture.inputs.right;
    const auto whole_bottom = fixture.inputs.bottom;
    fixture.inputs.right = fixture.inputs.bottom = 40 * tile_edge;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(!fixture.scene.terrain_demand_cache.valid);
    // Full squares no longer fit in the room left by the other materials.
    constexpr uint64_t material_area = 8U * 1024U * 1024U;
    fixture.scene.material_area = pair.ordinary.material_area = material_area;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(fixture.scene.terrain_demand_cache.valid);
    fixture.scene.material_area = pair.ordinary.material_area = 0;
    OA_CHECK(pair.build() == 0);
    fixture.scene.material_area = pair.ordinary.material_area = material_area;
    OA_CHECK(pair.build() == 1);
    fixture.scene.material_area = pair.ordinary.material_area = 0;
    fixture.inputs.right = whole_right;
    fixture.inputs.bottom = whole_bottom;
    OA_CHECK(pair.build() == 0);
    OA_CHECK(pair.build() == 1);
    fixture.inputs.left = fixture.inputs.right;
    OA_CHECK(pair.build() == 0);
    fixture.inputs.left = 0;
    OA_CHECK(pair.build() == 1);
}

/// A declared tile grid beyond the memo's fixed storage keeps the ordinary survey path.
void terrain_demand_storage_fallback() {
    TerrainDemandPair pair;
    auto& map = pair.fixture.map;
    constexpr uint32_t declared_tiles = 1280;
    map.tile_width = map.tile_height = declared_tiles;
    map.tile_count = 1;
    map.tile_indices.assign(static_cast<std::size_t>(declared_tiles) * declared_tiles, 0);
    map.tile_palette_indices.assign(tile_edge * tile_edge, ground_index);
    OA_CHECK(pair.build() == 0);
    OA_CHECK(!pair.fixture.scene.terrain_demand_cache.valid);
    OA_CHECK(pair.build() == 0);
    OA_CHECK(!pair.fixture.scene.terrain_demand_cache.valid);
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
            static_cast<uint16_t>(6 + 3 * picture), static_cast<uint16_t>(5 + 4 * picture)
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
        frame.width = static_cast<uint16_t>(6 + card * 2);
        frame.height = static_cast<uint16_t>(4 + card * 3);
        frame.origin_x = frame.width / 2;
        frame.origin_y = frame.height / 2;
        const auto count = static_cast<std::size_t>(frame.width) * frame.height;
        frame.pixels.assign(count, static_cast<uint8_t>(ground_index + card % 2));
        frame.coverage.assign(count, 1);
    }
    // Enough cards for several bands of each card pass (64 cards a band),
    // the bands of the cards standing up sharing their ground's indices.
    for (int32_t card = 0; card < 200; ++card) {
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
            check_ground_points(scenes[scene]);
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

/// Strategic markers use presented positions and fixed screen bounds without
/// any model instances, including above water and through a turned camera.
void strategic_unit_markers_without_models() {
    namespace camera = oa::app::renderer_live_camera;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    constexpr float marker_side = 5;
    Fixture fixture;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    fixture.inputs.clip_width = fixture.inputs.clip_height = 128;
    fixture.inputs.unit_marker_pixels = marker_side;
    fixture.inputs.owner_colors[1] = {20, 120, 240};
    fixture.inputs.sea_level = 6;
    for (auto& cell : fixture.map.attributes)
        cell.height = 4;
    oa::Unit unit{};
    unit.id = 7;
    unit.owner_index = 1;
    unit.flags = OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_SELECTED;
    unit.state_flags = OA_UNIT_STATE_CLOAKED;
    unit.build_remaining = 0.5F;
    unit.position = {24 * fixed, -12 * fixed, 24 * fixed};
    const auto canonical = unit;
    for (const auto angles :
         {camera::State{},
          camera::State{20, -53, true},
          camera::State{-90, 20, true},
          camera::State{145, -50, true}}) {
        for (const int32_t altitude : {-8, 0, 20}) {
            const int32_t anchor = std::max(altitude, int32_t{6});
            OA_CHECK(
                camera::projection(
                    angles,
                    {8, static_cast<float>(anchor), 16, 64, 64, 0.25F},
                    fixture.inputs.clip_projection
                )
            );
            fixture.draws.unit_markers = {{&unit, {8 * fixed, altitude * fixed, 16 * fixed}}};
            OA_CHECK(fixture.draws.models.empty());
            OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
            OA_CHECK(fixture.scene.cards.units.empty());
            OA_CHECK(fixture.scene.unit_ranges.size() == 1);
            OA_CHECK(fixture.scene.shadow_count == 0);
            if (fixture.scene.unit_ranges.size() != 1)
                continue;
            const auto range = fixture.scene.unit_ranges[0];
            OA_CHECK(range.unit_id == unit.id && range.vertex_count == 6);
            OA_CHECK(range.first_vertex + range.vertex_count <= fixture.scene.vertices.size());
            if (range.first_vertex + range.vertex_count > fixture.scene.vertices.size())
                continue;
            std::array<float, 4> bounds{
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity()
            };
            std::array<std::array<float, 3>, 6> projected{};
            std::array<float, 3> center{};
            const auto& projection = fixture.inputs.clip_projection;
            for (std::size_t corner = 0; corner < range.vertex_count; ++corner) {
                const auto& vertex = fixture.scene.vertices[range.first_vertex + corner];
                OA_CHECK(
                    std::isfinite(vertex.x) && std::isfinite(vertex.height) &&
                    std::isfinite(vertex.z)
                );
                OA_CHECK(vertex.height > anchor);
                OA_CHECK(
                    vertex.red == 20 / 255.0F && vertex.green == 120 / 255.0F &&
                    vertex.blue == 240 / 255.0F && vertex.opacity == 1
                );
                center[0] += vertex.x / 6;
                center[1] += vertex.height / 6;
                center[2] += vertex.z / 6;
                auto& point = projected[corner];
                for (std::size_t row = 0; row < point.size(); ++row)
                    point[row] = projection[row * 4] * vertex.x +
                                 projection[row * 4 + 1] * vertex.height +
                                 projection[row * 4 + 2] * vertex.z + projection[row * 4 + 3];
                bounds = {
                    std::min(bounds[0], point[0]),
                    std::min(bounds[1], point[1]),
                    std::max(bounds[2], point[0]),
                    std::max(bounds[3], point[1])
                };
            }
            OA_CHECK(std::abs(center[0] - 8) < tolerance);
            OA_CHECK(std::abs(center[2] - 16) < tolerance);
            OA_CHECK(std::abs(bounds[2] - bounds[0] - marker_side) < tolerance);
            OA_CHECK(std::abs(bounds[3] - bounds[1] - marker_side) < tolerance);
            const float across = (bounds[0] + bounds[2]) * 0.5F;
            const float down = (bounds[1] + bounds[3]) * 0.5F;
            std::array<float, 3> weights{};
            float depth{};
            OA_CHECK(
                camera::triangle_hit(
                    {projected[0], projected[1], projected[2]}, across, down, weights, depth
                ) ||
                camera::triangle_hit(
                    {projected[3], projected[4], projected[5]}, across, down, weights, depth
                )
            );
            OA_CHECK(std::isfinite(depth));
            OA_CHECK(std::memcmp(&canonical, &unit, sizeof(unit)) == 0);
        }
    }
    // Ground above the sea wins over an underwater unit's altitude.
    fixture.inputs.sea_level = 2;
    for (auto& cell : fixture.map.attributes)
        cell.height = 12;
    OA_CHECK(camera::projection({}, {8, 12, 16, 64, 64, 0.25F}, fixture.inputs.clip_projection));
    fixture.draws.unit_markers = {{&unit, {8 * fixed, -8 * fixed, 16 * fixed}}};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    for (const auto& range : fixture.scene.unit_ranges)
        for (std::size_t corner = 0; corner < range.vertex_count; ++corner)
            OA_CHECK(fixture.scene.vertices[range.first_vertex + corner].height > 12);
    OA_CHECK(std::memcmp(&canonical, &unit, sizeof(unit)) == 0);
}

/// Visibility, invalid identities, attachments and the viewport bound the
/// explicit marker list before any triangle can become a picking target.
void strategic_unit_marker_visibility() {
    namespace camera = oa::app::renderer_live_camera;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    Fixture fixture;
    fixture.inputs.clip_width = fixture.inputs.clip_height = 128;
    fixture.inputs.unit_marker_pixels = 5;
    OA_CHECK(camera::projection({}, {16, 0, 16, 64, 64, 1}, fixture.inputs.clip_projection));
    fixture.inputs.owner_colors[0] = {255, 80, 20};
    oa::Unit unit{};
    unit.id = 7;
    unit.position = {16 * fixed, 0, 16 * fixed};
    oa::Unit child = unit;
    child.id = 8;
    child.attach_parent = unit.id;
    oa::Unit unnamed = unit;
    unnamed.id = 0;
    fixture.draws.unit_markers = {
        {nullptr, unit.position},
        {&unnamed, unit.position},
        {&child, child.position},
        {&unit, {30000 * fixed, 0, 30000 * fixed}},
        {&unit, unit.position}
    };
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    if (fixture.scene.unit_ranges.empty())
        return;
    const auto first = fixture.scene.unit_ranges[0].first_vertex;
    const float seen = fixture.scene.vertices[first].brightness;
    fixture.coverage[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.size() == 1);
    if (!fixture.scene.unit_ranges.empty()) {
        const float grey =
            fixture.scene.vertices[fixture.scene.unit_ranges[0].first_vertex].brightness;
        OA_CHECK(grey > 0 && grey < seen);
    }
    fixture.mapped[0] = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.empty());
    fixture.mapped[0] = viewer_bit;
    fixture.coverage[0] = viewer_bit;
    fixture.inputs.clip_projection[0] = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.empty());
}

/// Mapping and line of sight independently restrict strategic markers, while
/// an unrestricted view remains fully bright even without sight cells.
void strategic_unit_marker_visibility_controls() {
    namespace camera = oa::app::renderer_live_camera;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;

    struct Visibility {
        bool mapping{}, line_of_sight{};
        uint16_t mapped{}, coverage{};
        bool drawable{}, grey{};
    };

    constexpr std::array<Visibility, 8> cases{
        {{false, false, 0, 0, true, false},
         {false, true, 0, 0, true, true},
         {false, true, 0, 1, true, false},
         {true, false, 0, 1, false, false},
         {true, false, 1, 0, true, false},
         {true, true, 1, 0, true, true},
         {true, true, 1, 1, true, false},
         {true, true, 0, 1, false, false}}
    };
    Fixture fixture;
    auto& in = fixture.inputs;
    in.clip_width = in.clip_height = 128;
    in.unit_marker_pixels = 5;
    OA_CHECK(camera::projection({}, {16, 0, 16, 64, 64, 1}, in.clip_projection));
    oa::Unit unit{};
    unit.id = 7;
    fixture.draws.unit_markers = {{&unit, {16 * fixed, 0, 16 * fixed}}};
    const auto check_marker = [&](bool drawable, float brightness) {
        OA_CHECK(fixture.scene.unit_ranges.size() == (drawable ? 1U : 0U));
        if (!drawable || fixture.scene.unit_ranges.size() != 1)
            return;
        const auto& range = fixture.scene.unit_ranges[0];
        OA_CHECK(range.unit_id == unit.id && range.vertex_count == 6);
        const auto& vertices = fixture.scene.vertices;
        const bool bounded = range.first_vertex <= vertices.size() &&
                             range.vertex_count <= vertices.size() - range.first_vertex;
        OA_CHECK(bounded);
        if (!bounded)
            return;
        for (std::size_t corner = 0; corner < range.vertex_count; ++corner) {
            const auto& vertex = vertices[range.first_vertex + corner];
            OA_CHECK(
                std::bit_cast<uint32_t>(vertex.brightness) == std::bit_cast<uint32_t>(brightness)
            );
            for (std::size_t row = 0; row < 2; ++row) {
                const auto at = row * 4;
                const float projected =
                    in.clip_projection[at] * vertex.x + in.clip_projection[at + 1] * vertex.height +
                    in.clip_projection[at + 2] * vertex.z + in.clip_projection[at + 3];
                const auto edge = row == 0 ? in.clip_width : in.clip_height;
                OA_CHECK(std::isfinite(projected) && projected >= 0 && projected <= edge);
            }
        }
    };
    for (const auto& visibility : cases)
        for (const bool dithered : {false, true}) {
            in.sight.mapping = visibility.mapping;
            in.sight.line_of_sight = visibility.line_of_sight;
            in.sight.dithered = dithered;
            fixture.mapped[0] = visibility.mapped;
            fixture.coverage[0] = visibility.coverage;
            OA_CHECK(live::build(in, fixture.scene, fixture.error));
            const float expected = visibility.grey ? (dithered ? 0.5F : 0.35F) : 1.0F;
            check_marker(visibility.drawable, expected);
        }
    in.sight.mapping = in.sight.line_of_sight = false;
    in.sight.player_bits = {};
    in.sight.coverage = {};
    // Empty sight grids do not restrict a view with both controls off;
    // an elevated unit's painted fog point can lie beyond either grid edge.
    for (const oa::FixedVec3 position :
         {oa::FixedVec3{16 * fixed, 0, 16 * fixed},
          oa::FixedVec3{-8 * fixed, 0, 16 * fixed},
          oa::FixedVec3{16 * fixed, 96 * fixed, 16 * fixed},
          oa::FixedVec3{48 * fixed, 0, 16 * fixed}}) {
        fixture.draws.unit_markers[0].shown_position = position;
        // Keep the marker inside the viewport while its fog point moves.
        OA_CHECK(
            camera::projection(
                {},
                {static_cast<float>(position.x) / fixed,
                 static_cast<float>(position.y) / fixed,
                 static_cast<float>(position.z) / fixed,
                 64,
                 64,
                 1},
                in.clip_projection
            )
        );
        OA_CHECK(live::build(in, fixture.scene, fixture.error));
        check_marker(true, 1.0F);
    }
}

/// Twenty thousand canonical units need only bounded billboard geometry,
/// retain every picking identity and leave no markers after the list clears.
void strategic_unit_marker_population() {
    namespace camera = oa::app::renderer_live_camera;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    constexpr uint32_t population = 20000;
    Fixture fixture;
    fixture.inputs.sight.mapping = fixture.inputs.sight.line_of_sight = false;
    fixture.inputs.clip_width = fixture.inputs.clip_height = 128;
    fixture.inputs.unit_marker_pixels = 5;
    OA_CHECK(
        camera::projection(
            {20, -53, true}, {16, 0, 16, 64, 64, 0.25F}, fixture.inputs.clip_projection
        )
    );
    std::vector<oa::Unit> units(population);
    fixture.draws.unit_markers.reserve(population);
    for (uint32_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.owner_index = static_cast<uint8_t>(index % fixture.inputs.owner_colors.size());
        unit.position = {
            static_cast<int32_t>(8 + index % 16) * fixed,
            0,
            static_cast<int32_t>(8 + (index / 16) % 16) * fixed
        };
        fixture.draws.unit_markers.push_back({&unit, unit.position});
    }
    for (const bool on_card : {false, true}) {
        fixture.inputs.models_on_card = on_card;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.draws.models.empty() && fixture.scene.cards.units.empty());
        OA_CHECK(fixture.scene.unit_ranges.size() == population);
        OA_CHECK(fixture.scene.shadow_count == 0);
        OA_CHECK(
            fixture.scene.vertices.size() ==
            fixture.scene.terrain_vertex_count + std::size_t{population} * 6
        );
        OA_CHECK(fixture.scene.vertices.size() < oa::app::renderer_live_gpu::largest_vertex_count);
        for (std::size_t index = 0; index < fixture.scene.unit_ranges.size(); ++index) {
            const auto& range = fixture.scene.unit_ranges[index];
            OA_CHECK(range.unit_id == units[index].id && range.vertex_count == 6);
            OA_CHECK(range.first_vertex == fixture.scene.terrain_vertex_count + index * 6);
        }
    }
    const auto vertices = fixture.scene.vertices;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(vertices.size() == fixture.scene.vertices.size());
    if (vertices.size() == fixture.scene.vertices.size())
        OA_CHECK(
            std::memcmp(
                vertices.data(),
                fixture.scene.vertices.data(),
                vertices.size() * sizeof(vertices[0])
            ) == 0
        );
    fixture.draws.unit_markers.clear();
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(fixture.scene.unit_ranges.empty());
    OA_CHECK(fixture.scene.vertices.size() == fixture.scene.terrain_vertex_count);
    const auto empty = fixture.scene.vertices;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    OA_CHECK(empty.size() == fixture.scene.vertices.size());
    if (empty.size() == fixture.scene.vertices.size())
        OA_CHECK(
            std::memcmp(
                empty.data(), fixture.scene.vertices.data(), empty.size() * sizeof(empty[0])
            ) == 0
        );
}

/// Compares marker frames, including upload rectangles and geometry drawn on the card.
/// @param actual the scene drawn with marker bands
/// @param expected the scene drawn on one thread
void check_marker_frames(const live::Scene& actual, const live::Scene& expected) {
    OA_CHECK(same_frame(actual, expected));
    OA_CHECK(
        std::equal(
            actual.atlas_update.regions.begin(),
            actual.atlas_update.regions.end(),
            expected.atlas_update.regions.begin(),
            expected.atlas_update.regions.end(),
            [](const auto& a, const auto& b) {
                return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
            }
        )
    );
    OA_CHECK(actual.cards.positions == expected.cards.positions);
    OA_CHECK(actual.cards.indices == expected.cards.indices);
    OA_CHECK(actual.cards.piece_frames == expected.cards.piece_frames);
    OA_CHECK(actual.cards.textures == expected.cards.textures);
    OA_CHECK(
        std::equal(
            actual.cards.units.begin(),
            actual.cards.units.end(),
            expected.cards.units.begin(),
            expected.cards.units.end(),
            [](const live::CardUnit& a, const live::CardUnit& b) {
                return a.unit_id == b.unit_id && a.vertex == b.vertex && a.instance == b.instance &&
                       a.type == b.type && a.first_index == b.first_index &&
                       a.index_count == b.index_count && a.left == b.left && a.top == b.top &&
                       a.right == b.right && a.bottom == b.bottom && a.bounded == b.bounded &&
                       a.body_lod == b.body_lod;
            }
        )
    );
    if (!actual.cards.units.empty() || !expected.cards.units.empty()) {
        const auto a = card_frame(actual);
        const auto b = card_frame(expected);
        OA_CHECK(a.units == b.units);
        OA_CHECK(a.vertices.size() == b.vertices.size());
        if (a.vertices.size() == b.vertices.size() && !a.vertices.empty())
            OA_CHECK(
                std::memcmp(
                    a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(a.vertices[0])
                ) == 0
            );
    }
}

/// Compact markers reconstruct the same ordered triangles, picking identities
/// and atlas as the ordinary path, including full models between card draws.
void check_marker_card_frame(const live::Scene& actual, const live::Scene& expected) {
    const auto drawn = card_frame(actual);
    const auto ordinary = card_frame(expected);
    OA_CHECK(drawn.units == ordinary.units);
    OA_CHECK(drawn.vertices.size() == ordinary.vertices.size());
    if (drawn.vertices.size() == ordinary.vertices.size() && !drawn.vertices.empty())
        OA_CHECK(
            std::memcmp(
                drawn.vertices.data(),
                ordinary.vertices.data(),
                drawn.vertices.size() * sizeof(drawn.vertices[0])
            ) == 0
        );
    OA_CHECK(
        actual.vertices.size() + actual.cards.vertices ==
        expected.vertices.size() + expected.cards.vertices
    );
    OA_CHECK(actual.model_count == expected.model_count);
    OA_CHECK(actual.shadow_count == expected.shadow_count);
    OA_CHECK(actual.sprite_count == expected.sprite_count);
    OA_CHECK(actual.particle_count == expected.particle_count);
    OA_CHECK(actual.nano_particle_count == expected.nano_particle_count);
    OA_CHECK(actual.terrain_vertex_count == expected.terrain_vertex_count);
    OA_CHECK(actual.texture_rgba == expected.texture_rgba);
    OA_CHECK(actual.texture_width == expected.texture_width);
    OA_CHECK(actual.texture_height == expected.texture_height);
    OA_CHECK(actual.texture_revision == expected.texture_revision);
    OA_CHECK(actual.frame_rows == expected.frame_rows);
    OA_CHECK(actual.frame.order == expected.frame.order);
    OA_CHECK(actual.materials.size() == expected.materials.size());
    const auto& update = actual.atlas_update;
    const auto& other = expected.atlas_update;
    OA_CHECK(update.previous_revision == other.previous_revision);
    OA_CHECK(update.first_row == other.first_row && update.end_row == other.end_row);
    OA_CHECK(
        update.frame_first_row == other.frame_first_row &&
        update.frame_end_row == other.frame_end_row
    );
    OA_CHECK(
        std::equal(
            update.regions.begin(),
            update.regions.end(),
            other.regions.begin(),
            other.regions.end(),
            [](const auto& a, const auto& b) {
                return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
            }
        )
    );
    constexpr std::array<uint32_t, 6> fan{0, 1, 2, 0, 2, 3};
    std::size_t markers{};
    for (const auto& unit : actual.cards.units) {
        if (!unit.marker)
            continue;
        ++markers;
        OA_CHECK(unit.index_count == fan.size());
        OA_CHECK(unit.instance < actual.cards.markers.size());
        OA_CHECK(
            unit.first_index <= actual.cards.indices.size() &&
            fan.size() <= actual.cards.indices.size() - unit.first_index
        );
        if (unit.instance >= actual.cards.markers.size() ||
            unit.first_index > actual.cards.indices.size() ||
            fan.size() > actual.cards.indices.size() - unit.first_index)
            continue;
        const auto& marker = actual.cards.markers[unit.instance];
        for (std::size_t at = 0; at < fan.size(); ++at) {
            const auto index = actual.cards.indices[unit.first_index + at];
            OA_CHECK(index == fan[at]);
            const auto vertex = live::card_vertex(actual.card_types, actual.cards, unit, index);
            const auto& corner = marker.corners[fan[at]];
            OA_CHECK(vertex.x == corner[0] && vertex.height == corner[1] && vertex.z == corner[2]);
            OA_CHECK(vertex.brightness == marker.brightness);
            OA_CHECK(
                vertex.red == marker.color[0] && vertex.green == marker.color[1] &&
                vertex.blue == marker.color[2]
            );
        }
    }
    OA_CHECK(markers == actual.cards.markers.size());
    std::size_t next_marker{};
    for (const auto& run : actual.cards.marker_runs) {
        OA_CHECK(run.first_instance == next_marker && run.instance_count != 0);
        OA_CHECK(run.vertex <= actual.vertices.size());
        next_marker += run.instance_count;
    }
    OA_CHECK(next_marker == markers);
    const auto card_draws = live::card_draws(actual.card_types, actual.cards);
    OA_CHECK(card_draws.markers.size() == markers);
    OA_CHECK(card_draws.marker_runs.size() == actual.cards.marker_runs.size());
    OA_CHECK(card_draws.marker_uv == actual.cards.marker_uv);
}

/// One retained ground and marker, compared against the ordinary lazy query path.
struct GroundPreparationFixture {
    GroundIndexThreadsFixture pair{4};
    oa::Unit marker{};

    GroundPreparationFixture() {
        constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
        marker.id = 7;
        marker.type_index = 1;
        marker.position = {64 * fixed, 32 * fixed, 64 * fixed};
        auto& in = pair.fixture.inputs;
        in.unit_marker_pixels = 5;
        in.markers_on_card = true;
        in.owner_colors[0] = {240, 30, 90};
        pair.fixture.draws.unit_markers.push_back({&marker, marker.position});
    }

    /// Builds real frames with and without a surface effect, retaining their own camera bounds.
    /// @param effect whether the standing sprite queries the rendered ground
    /// @param pool the candidate's drawing pool; the reference stays on its caller
    void build(bool effect, oa::platform::job_pool::Pool* pool = nullptr) {
        auto& fixture = pair.fixture;
        fixture.draws.sprites[0].surface_effect = effect;
        fixture.inputs.pool = nullptr;
        OA_CHECK(live::build(fixture.inputs, pair.ordinary, fixture.error));
        fixture.inputs.pool = pool;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.error.empty());
        OA_CHECK(same_frame(fixture.scene, pair.ordinary));
        check_marker_frames(fixture.scene, pair.ordinary);
        OA_CHECK(!fixture.scene.cards.units.empty());
    }

    /// Produces real retained geometry without requesting or preparing any query index.
    void retain() {
        for (unsigned frame = 0; frame < 3; ++frame)
            build(false);
        OA_CHECK(pair.fixture.scene.ground_kept);
        OA_CHECK(pair.fixture.scene.ground_revision != 0);
    }
};

/// Requires preparation to leave every emitted input and exact ground descriptor unchanged.
/// @param actual the prepared Scene
/// @param before the same Scene immediately before preparation
void check_prepared_frame(const live::Scene& actual, const live::Scene& before) {
    OA_CHECK(same_frame(actual, before));
    check_marker_frames(actual, before);
    OA_CHECK(actual.ground_revision == before.ground_revision);
    OA_CHECK(actual.kept_ground_revision == before.kept_ground_revision);
    OA_CHECK(actual.ground_points_revision == before.ground_points_revision);
    OA_CHECK(actual.card_ground.revision == before.card_ground.revision);
    OA_CHECK(actual.card_ground.next_revision == before.card_ground.next_revision);
}

/// Explicit preparation relocates one build; later ordinary query-free frames do no index work.
void ground_query_preparation() {
    namespace timers = oa::app::bench_timers;
    timers::reset(false);
    GroundPreparationFixture fixture;
    auto& pair = fixture.pair;
    auto& scene = pair.fixture.scene;
    auto& in = pair.fixture.inputs;
    fixture.build(false);
    OA_CHECK(scene.ground_revision != 0 && !scene.ground_kept);
    OA_CHECK(!live::prepare_ground_queries(scene, in.view_direction));
    OA_CHECK(scene.ground_index_cache.attempts == 0);
    fixture.retain();
    OA_CHECK(!scene.ground_index_cache.valid && scene.ground_index_cache.attempts == 0);
    const auto before = scene;
    const auto canonical = fixture.marker;
    OA_CHECK(live::prepare_ground_queries(scene, in.view_direction, &pair.four));
    check_prepared_frame(scene, before);
    OA_CHECK(scene.ground_index_cache.valid && scene.ground_index_cache.attempts == 1);
    OA_CHECK(scene.passes.ground_indices.builds == 1);
    const auto ready = scene.ground_index_cache.key;
    OA_CHECK(
        ready.revision == scene.ground_revision && ready.vertices == scene.terrain_vertex_count
    );
    OA_CHECK(live::prepare_ground_queries(scene, in.view_direction));
    OA_CHECK(scene.ground_index_cache.attempts == 1);
    for (unsigned frame = 0; frame < 5; ++frame) {
        fixture.build(false);
        OA_CHECK(scene.passes.ground_indices.builds == 0 && scene.passes.ground_indices.hits == 0);
        OA_CHECK(pair.ordinary.ground_index_cache.attempts == 0);
        OA_CHECK(scene.ground_index_cache.attempts == 1 && scene.ground_index_cache.key == ready);
    }
    // The first real ground query, including its lifted effect vertices and
    // the marker's nonempty picking records, must match a cold lazy build.
    fixture.build(true);
    pair.check_indices();
    OA_CHECK(scene.passes.ground_indices.builds == 0 && scene.passes.ground_indices.hits == 1);
    OA_CHECK(pair.ordinary.passes.ground_indices.builds == 1);
    OA_CHECK(std::memcmp(&fixture.marker, &canonical, sizeof(canonical)) == 0);
    // No automatic preparation follows an ordinary camera-axis change.
    in.view_direction = {-0.25F, 1, 0.5F};
    fixture.build(false);
    OA_CHECK(scene.ground_index_cache.key == ready && scene.passes.ground_indices.builds == 0);
    fixture.build(true);
    pair.check_indices();
    OA_CHECK(scene.passes.ground_indices.builds == 1);
    OA_CHECK(scene.ground_index_cache.key.card_axes != ready.card_axes);
}

/// Readiness is tied to the final retained base or cut, with ordinary key invalidation.
void ground_query_preparation_sources() {
    GroundPreparationFixture fixture;
    auto& pair = fixture.pair;
    auto& scene = pair.fixture.scene;
    auto& in = pair.fixture.inputs;
    fixture.retain();
    const auto unprepared = scene;
    const auto reject = [&](auto change) {
        auto invalid = unprepared;
        change(invalid);
        const auto attempts = invalid.ground_index_cache.attempts;
        OA_CHECK(!live::prepare_ground_queries(invalid, in.view_direction));
        OA_CHECK(invalid.ground_index_cache.attempts == attempts);
    };
    reject([&](auto& value) { value.frame_palette = &pair.fixture.palette; });
    reject([](auto& value) { value.ground_kept = false; });
    reject([](auto& value) { value.kept_ground_revision = 0; });
    reject([](auto& value) { value.ground_revision = 0; });
    reject([](auto& value) { ++value.ground_revision; });
    reject([](auto& value) { ++value.card_ground.revision; });
    reject([](auto& value) { --value.card_ground.vertex_count; });
    reject([](auto& value) { ++value.terrain_vertex_count; });
    reject([](auto& value) { value.vertices.resize(value.terrain_vertex_count - 1); });
    reject([](auto& value) { value.ground_vertices.clear(); });
    OA_CHECK(!live::prepare_ground_queries(scene, {std::numeric_limits<float>::quiet_NaN(), 1, 1}));
    OA_CHECK(!live::prepare_ground_queries(scene, {1, std::numeric_limits<float>::infinity(), 1}));
    OA_CHECK(scene.ground_index_cache.attempts == 0);
    OA_CHECK(live::prepare_ground_queries(scene, in.view_direction));
    for (unsigned stale = 0; stale < 4; ++stale) {
        auto copied = scene;
        if (stale == 0)
            ++copied.ground_index_cache.key.revision;
        else if (stale == 1)
            copied.ground_index_cache.key.vertices -= 3;
        else if (stale == 2)
            copied.ground_index_cache.key.card_axes[0] ^= uint64_t{1};
        else
            copied.ground_index_cache.key.float_control[0] ^= uint64_t{1};
        const auto attempts = copied.ground_index_cache.attempts;
        OA_CHECK(live::prepare_ground_queries(copied, in.view_direction));
        OA_CHECK(copied.ground_index_cache.attempts == attempts + 1);
        OA_CHECK(copied.ground_index_cache.key == scene.ground_index_cache.key);
        OA_CHECK(same_frame(copied, scene));
    }
    // A changed height produces a new retained generation before explicit preparation.
    pair.fixture.map.attributes[3].height = 12;
    fixture.retain();
    OA_CHECK(live::prepare_ground_queries(scene, in.view_direction));
    fixture.build(true);
    pair.check_indices();
    OA_CHECK(scene.passes.ground_indices.builds == 0);
    // A fresh foundation cut publishes its final revision only after all passes.
    oa::Unit building{};
    building.flags = OA_UNIT_FLAG_BUILDING;
    building.footprint_x = building.footprint_z = 2;
    building.position = {64 * 65536, 8 * 65536, 64 * 65536};
    pair.fixture.draws.models.resize(1);
    pair.fixture.draws.models[0].model.unit = &building;
    pair.fixture.draws.models[0].plan.linked.drawn = true;
    fixture.retain();
    OA_CHECK(scene.cut_source == scene.kept_ground_revision);
    OA_CHECK(scene.cut_revision == scene.ground_revision);
    OA_CHECK(live::prepare_ground_queries(scene, in.view_direction));
    fixture.build(true);
    pair.check_indices();
    OA_CHECK(scene.passes.ground_indices.builds == 0);
    pair.fixture.draws.models.clear();
    fixture.retain();
    discard_ground_points(scene);
    OA_CHECK(live::prepare_ground_queries(scene, in.view_direction));
    fixture.build(true);
    pair.check_indices();
    OA_CHECK(scene.passes.ground_indices.builds == 0);
    // A real query under another FP key rebuilds lazily, rather than using
    // bounds prepared under a different caller's arithmetic environment.
    const int rounding = std::fegetround();
    const int altered_rounding = rounding == FE_UPWARD ? FE_DOWNWARD : FE_UPWARD;
    if (rounding != -1 && std::fesetround(altered_rounding) == 0) {
        OA_CHECK(live::prepare_ground_queries(scene, in.view_direction));
        const auto controls = scene.ground_index_cache.key.float_control;
        OA_CHECK(std::fesetround(rounding) == 0);
        fixture.build(true);
        pair.check_indices();
        OA_CHECK(scene.passes.ground_indices.builds == 1);
        OA_CHECK(scene.ground_index_cache.key.float_control != controls);
    }
    // A vertical card without projected ground axes still prepares the
    // world-only index used by its ordinary query path.
    in.view_direction = {0, 0, 1};
    OA_CHECK(live::prepare_ground_queries(scene, in.view_direction));
    OA_CHECK(!scene.ground_index_cache.key.card);
    fixture.build(true);
    pair.check_indices();
    OA_CHECK(scene.passes.ground_indices.builds == 0);
    // Selected nonfinite compact data cannot publish an old valid bundle.
    auto invalid = scene;
    invalid.ground_index_cache.valid = false;
    invalid.ground_points.resize(invalid.terrain_vertex_count);
    for (std::size_t index = 0; index < invalid.ground_points.size(); ++index) {
        const auto& vertex = invalid.vertices[index];
        invalid.ground_points[index] = {vertex.x, vertex.z, vertex.height};
    }
    invalid.ground_points_revision = invalid.ground_revision;
    invalid.ground_points[0].x = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(!live::prepare_ground_queries(invalid, in.view_direction));
    OA_CHECK(!invalid.ground_index_cache.valid);
    live::Scene empty;
    OA_CHECK(!live::prepare_ground_queries(empty, in.view_direction));
}

/// Preparation consumes owned Scene geometry after all original frame inputs are gone.
void ground_query_preparation_owns_sources() {
    live::Scene detached;
    std::array<float, 3> direction{};
    {
        GroundPreparationFixture fixture;
        fixture.retain();
        direction = fixture.pair.fixture.inputs.view_direction;
        detached = std::move(fixture.pair.fixture.scene);
    }
    const auto before = detached;
    OA_CHECK(live::prepare_ground_queries(detached, direction));
    check_prepared_frame(detached, before);
    OA_CHECK(detached.ground_index_cache.valid);
}

/// A partial optional allocation never publishes indices or changes the Scene's picture.
void ground_query_preparation_allocation_recovery() {
    GroundPreparationFixture fixture;
    fixture.retain();
    auto& pair = fixture.pair;
    auto& in = pair.fixture.inputs;
    const auto unprepared = pair.fixture.scene;
    auto counted = unprepared;
    ground_allocations = 0;
    ground_allocation_to_refuse = std::numeric_limits<std::size_t>::max();
    watch_ground_allocations = true;
    const bool ready = live::prepare_ground_queries(counted, in.view_direction);
    watch_ground_allocations = false;
    const auto allocations = ground_allocations;
    constexpr std::size_t maximum_failure_probes = 64;
    OA_CHECK(ready && allocations != 0 && allocations <= maximum_failure_probes);
    for (std::size_t fail = 0; fail < std::min(allocations, maximum_failure_probes); ++fail) {
        // Fresh baseline buffers preserve each probe's allocation sequence.
        auto candidate = unprepared;
        auto ordinary = unprepared;
        pair.fixture.scene = std::move(candidate);
        pair.ordinary = std::move(ordinary);
        ground_allocations = refused_ground_allocations = 0;
        ground_allocation_to_refuse = fail;
        watch_ground_allocations = true;
        const bool prepared = live::prepare_ground_queries(pair.fixture.scene, in.view_direction);
        watch_ground_allocations = false;
        OA_CHECK(!prepared && refused_ground_allocations == 1);
        OA_CHECK(!pair.fixture.scene.ground_index_cache.valid);
        check_prepared_frame(pair.fixture.scene, unprepared);
        fixture.build(true);
        pair.check_indices();
        OA_CHECK(pair.fixture.scene.passes.ground_indices.builds == 1);
    }
    ground_allocation_to_refuse = std::numeric_limits<std::size_t>::max();
}

/// Marker workers keep exact geometry, atlas updates and picking identities through moving armies.
void strategic_unit_markers_on_threads() {
    namespace camera = oa::app::renderer_live_camera;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    constexpr uint32_t population = 20000;
    Fixture battle;
    auto& in = battle.inputs;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.clip_width = in.clip_height = 128;
    in.unit_marker_pixels = 5;
    in.sea_level = 3;
    OA_CHECK(camera::projection({20, -53, true}, {16, 0, 16, 64, 64, 0.25F}, in.clip_projection));
    for (std::size_t owner = 0; owner < in.owner_colors.size(); ++owner)
        in.owner_colors[owner] = {
            static_cast<uint8_t>(20 + owner * 7),
            static_cast<uint8_t>(220 - owner * 3),
            static_cast<uint8_t>(40 + owner * 5)
        };
    std::vector<oa::Unit> units(population);
    for (uint32_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.owner_index = static_cast<uint8_t>(index % in.owner_colors.size());
        unit.position = {
            static_cast<int32_t>(8 + index % 16) * fixed,
            static_cast<int32_t>(index % 7) * fixed / 3 - 2 * fixed,
            static_cast<int32_t>(8 + (index / 16) % 16) * fixed
        };
        if (index % 7 == 0)
            unit.state_flags = OA_UNIT_STATE_CLOAKED;
        battle.draws.unit_markers.push_back({&unit, unit.position});
    }
    const auto canonical = units;
    oa::platform::job_pool::Pool one{1}, four{4};
    live::Scene ordinary;
    const auto compare = [&](bool banded) {
        in.pool = &one;
        OA_CHECK(live::build(in, ordinary, battle.error));
        OA_CHECK(battle.error.empty());
        in.pool = &four;
        OA_CHECK(live::build(in, battle.scene, battle.error));
        OA_CHECK(battle.error.empty());
        OA_CHECK(!ordinary.unit_detail_active && !battle.scene.unit_detail_active);
        OA_CHECK(ordinary.passes.marker_bands.empty());
        OA_CHECK(battle.scene.passes.marker_bands.empty() != banded);
        check_marker_frames(battle.scene, ordinary);
    };
    // A missing white material keeps the original packing order. Its next
    // frame uses real worker bands, rather than comparing two serial paths.
    compare(false);
    compare(true);
    OA_CHECK(battle.scene.passes.marker_bands.size() == 4);
    OA_CHECK(battle.scene.unit_ranges.size() == population);
    OA_CHECK(battle.scene.model_count == population);
    OA_CHECK(battle.scene.vertices.size() == battle.scene.terrain_vertex_count + population * 6);
    for (std::size_t index = 0; index < std::min(battle.scene.unit_ranges.size(), units.size());
         ++index) {
        const auto& range = battle.scene.unit_ranges[index];
        OA_CHECK(range.unit_id == units[index].id && range.vertex_count == 6);
        OA_CHECK(range.first_vertex == battle.scene.terrain_vertex_count + index * 6);
    }
    for (const bool on_card : {true, false}) {
        in.models_on_card = on_card;
        for (auto& marker : battle.draws.unit_markers) {
            marker.shown_position.x += fixed / 7;
            marker.shown_position.z -= fixed / 11;
        }
        in.owner_colors[1] = {240, 30, 120};
        battle.palette[alternate_index * oa::palette_entry_bytes] += 1;
        compare(true);
        OA_CHECK(battle.scene.cards.units.empty());
    }
    const int rounding = std::fegetround();
    if (rounding != -1 && std::fesetround(FE_UPWARD) == 0) {
        compare(true);
        OA_CHECK(std::fesetround(rounding) == 0);
        compare(true);
    }
    // Every copied band owns its buffers; neither a copy nor a move may
    // retain a pointer into the source Scene's lists.
    {
        auto copied = battle.scene;
        in.pool = &four;
        OA_CHECK(live::build(in, copied, battle.error));
        in.pool = &one;
        OA_CHECK(live::build(in, ordinary, battle.error));
        check_marker_frames(copied, ordinary);
        battle.scene = std::move(copied);
    }
    compare(true);
    const auto all = battle.draws.unit_markers;
    oa::Unit unnamed{}, attached{};
    attached.id = 60000;
    attached.attach_parent = units[0].id;
    battle.draws.unit_markers[0].unit = nullptr;
    battle.draws.unit_markers[4999].unit = &unnamed;
    battle.draws.unit_markers[5000].unit = &attached;
    battle.draws.unit_markers[9999].shown_position = {30000 * fixed, 0, 30000 * fixed};
    compare(true);
    OA_CHECK(battle.scene.unit_ranges.size() == population - 4);
    in.sight.mapping = in.sight.line_of_sight = true;
    battle.coverage[0] = 0;
    compare(true);
    battle.mapped[0] = 0;
    compare(true);
    OA_CHECK(battle.scene.unit_ranges.empty() && battle.scene.model_count == 0);
    battle.mapped[0] = battle.coverage[0] = viewer_bit;
    battle.draws.unit_markers = all;
    compare(true);
    // A cold material cache, a small tail and an empty tail all clear the
    // retained worker lists before the next full army uses them again.
    ordinary.materials.clear();
    battle.scene.materials.clear();
    compare(false);
    compare(true);
    battle.draws.unit_markers.resize(1023);
    compare(false);
    battle.draws.unit_markers.resize(1024);
    battle.draws.unit_markers.back() = all[1023];
    compare(true);
    OA_CHECK(battle.scene.passes.marker_bands.size() == 2);
    battle.draws.unit_markers.clear();
    compare(false);
    OA_CHECK(battle.scene.unit_ranges.empty());
    OA_CHECK(battle.scene.vertices.size() == battle.scene.terrain_vertex_count);
    battle.draws.unit_markers = all;
    compare(true);
    const auto projection = in.clip_projection;
    in.clip_projection[0] = std::numeric_limits<float>::quiet_NaN();
    compare(false);
    OA_CHECK(battle.scene.unit_ranges.empty());
    in.clip_projection = projection;
    compare(true);
    OA_CHECK(std::memcmp(units.data(), canonical.data(), units.size() * sizeof(units[0])) == 0);
}

/// Compact marker instances preserve the ordinary six-corner stream through
/// army movement, visibility changes, different drawing threads and reuse.
void strategic_unit_markers_on_card() {
    namespace camera = oa::app::renderer_live_camera;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    constexpr uint32_t population = 20000;
    Fixture battle;
    auto& in = battle.inputs;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.clip_width = in.clip_height = 128;
    in.unit_marker_pixels = 5;
    in.sea_level = 6;
    for (auto& cell : battle.map.attributes)
        cell.height = 4;
    OA_CHECK(camera::projection({20, -53, true}, {16, 6, 16, 64, 64, 0.25F}, in.clip_projection));
    for (std::size_t owner = 0; owner < in.owner_colors.size(); ++owner)
        in.owner_colors[owner] = {
            static_cast<uint8_t>(20 + owner * 7),
            static_cast<uint8_t>(220 - owner * 3),
            static_cast<uint8_t>(40 + owner * 5)
        };
    std::vector<oa::Unit> units(population);
    for (uint32_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.owner_index = static_cast<uint8_t>(index % in.owner_colors.size());
        unit.position = {24 * fixed, -12 * fixed, 24 * fixed};
        battle.draws.unit_markers.push_back(
            {&unit,
             {static_cast<int32_t>(8 + index % 16) * fixed,
              static_cast<int32_t>(index % 7) * 4 * fixed - 8 * fixed,
              static_cast<int32_t>(8 + (index / 16) % 16) * fixed}}
        );
    }
    const auto canonical = units;
    const auto all = battle.draws.unit_markers;
    oa::platform::job_pool::Pool one{1}, four{4};
    live::Scene ordinary;
    const auto compare = [&](oa::platform::job_pool::Pool& pool, bool compact) {
        in.pool = &one;
        in.markers_on_card = false;
        const bool reference_built = live::build(in, ordinary, battle.error);
        OA_CHECK(reference_built);
        in.pool = &pool;
        in.markers_on_card = compact;
        const bool actual_built = live::build(in, battle.scene, battle.error);
        OA_CHECK(actual_built);
        if (!reference_built || !actual_built)
            return;
        OA_CHECK(battle.error.empty());
        check_marker_card_frame(battle.scene, ordinary);
        OA_CHECK(
            battle.scene.vertices.size() + battle.scene.cards.vertices <=
            oa::app::renderer_live_gpu::largest_vertex_count
        );
        if (!compact) {
            OA_CHECK(battle.scene.cards.markers.empty() && battle.scene.cards.marker_runs.empty());
            check_marker_frames(battle.scene, ordinary);
        }
    };
    compare(one, true);
    compare(four, true);
    OA_CHECK(battle.scene.cards.markers.size() == population);
    OA_CHECK(battle.scene.cards.vertices == population * 6);
    OA_CHECK(battle.scene.unit_ranges.empty());
    OA_CHECK(battle.scene.cards.instances.empty() && battle.scene.cards.positions.empty());
    OA_CHECK(battle.scene.vertices.size() == battle.scene.terrain_vertex_count);
    OA_CHECK(same_on_card(battle.scene, ordinary, in));
    for (std::size_t index = 0; index < battle.scene.cards.units.size(); ++index)
        OA_CHECK(battle.scene.cards.units[index].unit_id == units[index].id);
    for (const bool models_on_card : {true, false}) {
        in.models_on_card = models_on_card;
        for (auto& marker : battle.draws.unit_markers) {
            marker.shown_position.x += fixed / 7;
            marker.shown_position.z -= fixed / 11;
        }
        in.owner_colors[1] = {240, 30, 120};
        battle.palette[alternate_index * oa::palette_entry_bytes] += 1;
        compare(four, true);
    }
    const int rounding = std::fegetround();
    if (rounding != -1 && std::fesetround(FE_UPWARD) == 0) {
        compare(four, true);
        OA_CHECK(std::fegetround() == FE_UPWARD);
        OA_CHECK(std::fesetround(rounding) == 0);
        compare(four, true);
    }
    auto copied = battle.scene;
    battle.scene = {};
    battle.scene = std::move(copied);
    compare(four, true);
    compare(four, false);
    compare(four, true);
    oa::Unit unnamed{}, attached{};
    attached.id = 60000;
    attached.attach_parent = units[0].id;
    battle.draws.unit_markers[0].unit = nullptr;
    battle.draws.unit_markers[4999].unit = &unnamed;
    battle.draws.unit_markers[5000].unit = &attached;
    battle.draws.unit_markers[9999].shown_position = {30000 * fixed, 0, 30000 * fixed};
    compare(four, true);
    OA_CHECK(battle.scene.cards.markers.size() == population - 4);
    in.sight.mapping = in.sight.line_of_sight = true;
    battle.coverage[0] = 0;
    compare(four, true);
    battle.mapped[0] = 0;
    compare(four, true);
    OA_CHECK(battle.scene.cards.markers.empty() && battle.scene.model_count == 0);
    battle.mapped[0] = battle.coverage[0] = viewer_bit;
    battle.draws.unit_markers = all;
    in.sea_level = 2;
    for (auto& cell : battle.map.attributes)
        cell.height = 12;
    compare(four, true);
    battle.draws.unit_markers.resize(1);
    compare(one, true);
    battle.draws.unit_markers.clear();
    compare(four, true);
    OA_CHECK(battle.scene.cards.markers.empty() && battle.scene.cards.marker_runs.empty());
    battle.draws.unit_markers = all;
    const auto projection = in.clip_projection;
    in.clip_projection[0] = std::numeric_limits<float>::quiet_NaN();
    compare(four, true);
    OA_CHECK(battle.scene.cards.markers.empty());
    in.clip_projection = projection;
    compare(four, true);
    OA_CHECK(std::memcmp(units.data(), canonical.data(), units.size() * sizeof(units[0])) == 0);
}

/// Real model geometry stays before marker bands and projectile geometry stays after them.
void strategic_unit_marker_mixed_passes() {
    namespace camera = oa::app::renderer_live_camera;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr std::size_t population = 2048;
    Fixture battle;
    auto& in = battle.inputs;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.clip_width = in.clip_height = 128;
    in.unit_marker_pixels = 5;
    in.owner_colors[0] = {255, 80, 20};
    OA_CHECK(camera::projection({}, {16, 0, 16, 64, 64, 1}, in.clip_projection));
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 0, 0}, {4 * fixed, 0, 0}, {0, 4 * fixed, 0}};
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(1);
    prepared.objects[0].primitives[0].flags = drawing::primitive_colored;
    prepared.objects[0].primitives[0].color = alternate_index;
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    oa::Unit model_unit{};
    model_unit.id = 60000;
    model_unit.flags = OA_UNIT_FLAG_BUILDING;
    model_unit.position = {8 * fixed, 0, 12 * fixed};
    battle.draws.models.resize(1);
    auto& model = battle.draws.models[0];
    model.model.unit = &model_unit;
    model.model.instance = &instance;
    model.model.prepared = &prepared;
    model.plan.linked.drawn = true;
    std::vector<oa::Unit> units(population);
    for (std::size_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.position = {
            static_cast<int32_t>(8 + index % 16) * fixed,
            0,
            static_cast<int32_t>(8 + index / 16 % 16) * fixed
        };
        battle.draws.unit_markers.push_back({&unit, unit.position});
    }
    oa::app::ProjectileDraw shot{};
    shot.object = shot.child = &source->objects[0];
    shot.prepared = shot.child_prepared = &prepared.objects[0];
    shot.position = {4 * fixed, 4 * fixed, 12 * fixed};
    battle.draws.projectiles.push_back(shot);
    oa::platform::job_pool::Pool one{1}, four{4};
    for (const bool marker_cards : {false, true}) {
        live::Scene ordinary;
        battle.scene = {};
        for (const bool on_card : {false, true}) {
            in.models_on_card = on_card;
            for (uint32_t frame = 0; frame < 3; ++frame) {
                if (frame == 2) {
                    model.shown_position = oa::FixedVec3{9 * fixed, 0, 13 * fixed};
                    battle.palette[alternate_index * oa::palette_entry_bytes] += 1;
                }
                in.pool = &one;
                in.markers_on_card = false;
                OA_CHECK(live::build(in, ordinary, battle.error));
                in.pool = &four;
                in.markers_on_card = marker_cards;
                OA_CHECK(live::build(in, battle.scene, battle.error));
                OA_CHECK(!ordinary.unit_detail_active && !battle.scene.unit_detail_active);
                if (marker_cards)
                    check_marker_card_frame(battle.scene, ordinary);
                else
                    check_marker_frames(battle.scene, ordinary);
                OA_CHECK(
                    std::count_if(
                        battle.scene.cards.units.begin(),
                        battle.scene.cards.units.end(),
                        [](const live::CardUnit& unit) { return !unit.marker; }
                    ) == (on_card ? 1 : 0)
                );
                if (frame != 0) {
                    if (marker_cards)
                        OA_CHECK(battle.scene.cards.markers.size() == population);
                    else
                        OA_CHECK(battle.scene.passes.marker_bands.size() == 4);
                    OA_CHECK(battle.scene.model_count == population + 1);
                    const auto expanded = card_frame(battle.scene);
                    const auto expected =
                        battle.scene.terrain_vertex_count + 3 + population * 6 + 6;
                    OA_CHECK(
                        expanded.vertices.size() == expected && expanded.units.size() == expected
                    );
                    if (expanded.vertices.size() != expected || expanded.units.size() != expected)
                        continue;
                    OA_CHECK(expanded.units[battle.scene.terrain_vertex_count] == model_unit.id);
                    const auto first_marker = battle.scene.terrain_vertex_count + 3;
                    for (std::size_t index = 0; index < population; ++index)
                        OA_CHECK(expanded.units[first_marker + index * 6] == units[index].id);
                    for (std::size_t vertex = expanded.vertices.size() - 6;
                         vertex < expanded.vertices.size();
                         ++vertex)
                        OA_CHECK(expanded.units[vertex] == 0);
                }
            }
        }
    }
}

/// Water and ripples count the markers' virtual triangles against the same
/// frame limit, including when most geometry is shared model draws.
void strategic_unit_marker_water_budget() {
    namespace camera = oa::app::renderer_live_camera;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr std::size_t polygons = 128;
    constexpr std::size_t population =
        oa::app::renderer_live_gpu::largest_vertex_count / (polygons * 3) - 1;
    Fixture battle;
    auto& in = battle.inputs;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.clip_width = in.clip_height = 128;
    in.models_on_card = true;
    in.sea_level = 6;
    in.water.detail = 1;
    in.water.ripples = in.water.coastal_waves = 0;
    in.tick = 8;
    OA_CHECK(camera::projection({}, {16, 0, 16, 64, 64, 1}, in.clip_projection));
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    auto& object = source->objects[0];
    object.vertices = {{0, 0, 0}, {4 * fixed, 0, 0}, {0, 4 * fixed, 0}};
    object.primitives.resize(polygons);
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    prepared.objects[0].primitives.resize(polygons);
    for (std::size_t index = 0; index < polygons; ++index) {
        object.primitives[index].vertex_indices = {0, 1, 2};
        auto& primitive = prepared.objects[0].primitives[index];
        primitive.source_index = static_cast<uint32_t>(index);
        primitive.flags = drawing::primitive_colored;
        primitive.color = alternate_index;
    }
    auto instance = oa::sim::model_runtime::make_instance(source);
    instance.rebuild_transforms();
    std::vector<oa::Unit> units(population);
    battle.draws.models.resize(population);
    for (std::size_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.flags = OA_UNIT_FLAG_BUILDING;
        unit.position = {16 * fixed, 20 * fixed, 16 * fixed};
        auto& draw = battle.draws.models[index];
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.plan.linked.drawn = true;
    }
    const bool initial = live::build(in, battle.scene, battle.error);
    OA_CHECK(initial);
    if (!initial)
        return;
    OA_CHECK(!battle.scene.unit_detail_active);
    OA_CHECK(battle.scene.cards.instances.size() == population);
    OA_CHECK(battle.scene.cards.vertices == population * polygons * 3);
    const auto budget = oa::app::renderer_live_gpu::largest_vertex_count;
    const auto occupied = battle.scene.vertices.size() + battle.scene.cards.vertices;
    OA_CHECK(occupied + 12 <= budget);
    if (occupied + 12 > budget)
        return;
    const auto marker_count = (budget - occupied) / 6 - 1;
    std::vector<oa::Unit> markers(marker_count);
    for (std::size_t index = 0; index < marker_count; ++index) {
        auto& unit = markers[index];
        unit.id = static_cast<uint16_t>(60000 + index);
        unit.position = {16 * fixed, 0, 16 * fixed};
        battle.draws.unit_markers.push_back({&unit, unit.position});
    }
    const std::array events{oa::app::renderer_water::Disturbance{
        16, 16, 0, 1, oa::app::renderer_water::Kind::water_impact
    }};
    in.water_disturbances = events;
    std::size_t expected_vertices{};
    for (const bool compact : {false, true}) {
        in.markers_on_card = compact;
        in.water.ripples = in.water.coastal_waves = 0;
        const bool fits = live::build(in, battle.scene, battle.error);
        OA_CHECK(fits);
        if (!fits)
            continue;
        OA_CHECK(!battle.scene.unit_detail_active);
        OA_CHECK(battle.scene.cards.markers.size() == (compact ? marker_count : 0));
        const auto logical = battle.scene.vertices.size() + battle.scene.cards.vertices;
        OA_CHECK(logical <= budget && budget - logical < 12);
        if (!compact)
            expected_vertices = logical;
        OA_CHECK(logical == expected_vertices);
        for (const bool ripples : {false, true}) {
            in.water.coastal_waves = ripples ? 0 : 1;
            in.water.ripples = ripples ? 1 : 0;
            OA_CHECK(!live::build(in, battle.scene, battle.error));
            OA_CHECK(battle.scene.out_of_room);
            OA_CHECK(battle.error.find("geometry budget") != std::string::npos);
        }
        in.water.ripples = in.water.coastal_waves = 0;
        OA_CHECK(live::build(in, battle.scene, battle.error));
        OA_CHECK(battle.scene.vertices.size() + battle.scene.cards.vertices == expected_vertices);
    }
}

/// Shared posed vertices preserve exact source values, retries and bounded memory.
void shared_model_positions() {
    namespace models = oa::formats::objects3d;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    live::ModelCards cards;
    live::clear_cards(cards);
    live::CardType type;
    type.piece_vertices = {0, 3, 5};
    std::array<runtime::PieceState, 2> pieces;
    pieces[0].transformed_vertices = {
        {fixed / 3, -fixed / 7, fixed / 11}, {0, 0, 0}, {-fixed, fixed, -fixed}
    };
    pieces[1].transformed_vertices = {
        {fixed * 3, -fixed * 2, fixed * 5}, {-fixed * 4, fixed / 13, 0}
    };
    const auto first = live::card_positions(cards, 0, type, pieces, {});
    OA_CHECK(first == 0);
    const auto original = cards.positions;
    constexpr uint32_t population = 20000;
    for (uint32_t unit = 1; unit < population; ++unit)
        OA_CHECK(live::card_positions(cards, 0, type, pieces, {}) == first);
    OA_CHECK(cards.positions == original);
    OA_CHECK(cards.positions.size() == 15 && cards.pose_points.size() == 5);
    OA_CHECK(cards.poses.size() == 1);
    // Different types never borrow a range even when their vertices match.
    OA_CHECK(live::card_positions(cards, 1, type, pieces, {}) == 5);
    // An interpolated source replaces only the pieces it supplies.
    std::vector<std::vector<models::FixedVector3>> shown{pieces[0].transformed_vertices};
    ++shown[0][0].x;
    const auto interpolated = live::card_positions(cards, 0, type, pieces, shown);
    OA_CHECK(interpolated == 10);
    OA_CHECK(cards.positions[3 * interpolated] == shown[0][0].x / float{fixed});
    OA_CHECK(live::card_positions(cards, 0, type, pieces, shown) == interpolated);
    const auto size = cards.positions.size();
    shown[0].clear();
    OA_CHECK(live::card_positions(cards, 0, type, pieces, shown) == -1);
    OA_CHECK(cards.positions.size() == size);
    // A matching digest alone is never enough: simulate a collision naming
    // a different source range and require the complete exact comparison.
    live::ModelCards other;
    live::clear_cards(other);
    ++pieces[0].transformed_vertices[0].y;
    OA_CHECK(live::card_positions(other, 0, type, pieces, {}) == 0);
    const auto occupied =
        std::find_if(other.pose_slots.begin(), other.pose_slots.end(), [&](const auto& slot) {
            return slot.epoch == other.pose_epoch;
        });
    OA_CHECK(occupied != other.pose_slots.end());
    if (occupied != other.pose_slots.end()) {
        const auto slot = std::size_t(occupied - other.pose_slots.begin());
        cards.pose_slots[slot] = {occupied->digest, 0, cards.pose_epoch};
        const auto distinct = live::card_positions(cards, 0, type, pieces, {});
        OA_CHECK(distinct == static_cast<int64_t>(size / 3));
        OA_CHECK(
            cards.positions[3 * distinct + 1] == pieces[0].transformed_vertices[0].y / float{fixed}
        );
    }
    // A material retry can remove a newly listed pose. No stale lookup may
    // return that truncated range when the following draw begins.
    live::mark_cards(cards);
    const auto marked = cards.positions.size();
    ++pieces[1].transformed_vertices[0].x;
    OA_CHECK(live::card_positions(cards, 0, type, pieces, {}) >= 0);
    live::unmark_cards(cards);
    OA_CHECK(cards.positions.size() == marked);
    OA_CHECK(cards.poses.empty() && cards.pose_points.empty());
    OA_CHECK(live::card_positions(cards, 0, type, pieces, {}) == static_cast<int64_t>(marked / 3));
    const auto point_capacity = cards.pose_points.capacity();
    const auto slot_capacity = cards.pose_slots.capacity();
    cards.pose_epoch = std::numeric_limits<uint32_t>::max();
    live::clear_cards(cards);
    OA_CHECK(cards.pose_epoch == 1 && cards.positions.empty());
    OA_CHECK(
        cards.pose_points.capacity() == point_capacity &&
        cards.pose_slots.capacity() == slot_capacity
    );
    OA_CHECK(live::card_positions(cards, 0, type, pieces, {}) == 0);

    // Distinct poses exhaust only the bounded lookup memory. Their geometry
    // is still listed, and a completely occupied hash table cannot loop.
    live::clear_cards(cards);
    type.piece_vertices = {0, 128};
    std::array<runtime::PieceState, 1> varied;
    varied[0].transformed_vertices.resize(128);
    for (uint32_t pose = 0; pose < 1200; ++pose) {
        varied[0].transformed_vertices[0].x = static_cast<int32_t>(pose);
        const auto at = live::card_positions(cards, 2, type, varied, {});
        OA_CHECK(at == static_cast<int64_t>(pose) * 128);
        OA_CHECK(cards.positions[3 * at] == pose / float{fixed});
    }
    OA_CHECK(cards.poses.size() <= 1024 && cards.pose_points.size() <= 131072);
    OA_CHECK(cards.pose_slots.size() == 2048 && cards.recent_poses.size() == 1024);
    for (auto& slot : cards.pose_slots)
        slot = {0, 0, cards.pose_epoch};
    varied[0].transformed_vertices[0].x = fixed;
    OA_CHECK(live::card_positions(cards, 3, type, varied, {}) == 1200 * 128);
}

/// Relative posed coordinates keep the old world addition's exact float rounding.
void shared_model_coordinate_rounding() {
    namespace models = oa::formats::objects3d;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr int32_t maximum = std::numeric_limits<int32_t>::max();
    constexpr int32_t minimum = std::numeric_limits<int32_t>::min();
    constexpr std::array<models::FixedVector3, 7> origins{
        {{0, 0, 0},
         {1, -1, 1},
         {-fixed / 3, fixed / 7, -fixed / 13},
         {fixed * 30000 + 1, -fixed * 20000 + 17, fixed * 25000 - 1},
         {-fixed * 30000 - 1, fixed * 20000 - 17, -fixed * 25000 + 1},
         {maximum, minimum, maximum - fixed},
         {minimum, maximum, minimum + fixed}}
    };
    std::array<runtime::PieceState, 1> pieces;
    pieces[0].transformed_vertices = {
        {0, 0, 0},
        {1, -1, 1},
        {-fixed / 3, fixed / 7, -fixed / 13},
        {fixed * 23 + 1, -fixed * 17 + 17, fixed * 13 - 1},
        {maximum, minimum, minimum},
        {minimum, maximum, maximum}
    };
    live::CardType type;
    type.piece_vertices = {0, static_cast<uint32_t>(pieces[0].transformed_vertices.size())};
    live::CardTypes types;
    for (uint32_t vertex = 0; vertex < type.piece_vertices.back(); ++vertex)
        types.records.insert(types.records.end(), {vertex, 0});
    live::ModelCards cards;
    live::clear_cards(cards);
    cards.piece_frames = {1};
    cards.textures = {0, 0, 1, 1};
    const auto first = live::card_positions(cards, 0, type, pieces, {});
    OA_CHECK(first == 0);
    for (const auto& origin : origins) {
        oa::app::renderer_live_gpu::ModelInstance instance;
        instance.positions = static_cast<uint32_t>(first);
        instance.origin = {
            origin.x / float{fixed}, origin.y / float{fixed}, origin.z / float{fixed}
        };
        cards.instances = {instance};
        for (uint32_t vertex = 0; vertex < pieces[0].transformed_vertices.size(); ++vertex) {
            const auto& point = pieces[0].transformed_vertices[vertex];
            const std::array old_world{
                origin.x / float{fixed} + point.x / float{fixed},
                origin.y / float{fixed} + point.y / float{fixed},
                origin.z / float{fixed} - point.z / float{fixed}
            };
            const auto corner = live::card_corner(types, cards, {}, vertex);
            const std::array shared_world{corner.x, corner.height, corner.z};
            for (std::size_t axis = 0; axis < old_world.size(); ++axis)
                OA_CHECK(
                    std::bit_cast<uint32_t>(old_world[axis]) ==
                    std::bit_cast<uint32_t>(shared_world[axis])
                );
        }
    }
}

/// Shared local indices retain exact world corners and emitted-face pick bounds.
void indexed_model_vertex_projection() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr float fixed_scale = static_cast<float>(fixed);
    constexpr uint32_t population = 2;
    constexpr double minimum_relative_area = 1e-9;
    Fixture battle;
    auto& in = battle.inputs;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.clip_width = in.clip_height = 128;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(3);
    auto& body = source->objects[0];
    body.first_child = 1;
    body.vertices = {
        {0, 0, 0},
        {4 * fixed + 1, 0, 0},
        {4 * fixed, 0, 4 * fixed - 1},
        {0, 0, 4 * fixed},
        {100 * fixed, 0, 100 * fixed},
        {-100 * fixed, 0, -80 * fixed}
    };
    body.primitives.resize(4);
    body.primitives[0].vertex_indices = {0, 1, 2, 3};
    body.primitives[1].vertex_indices = {0, 2, 3};
    body.primitives[2].vertex_indices = {0, 3, 4};
    body.primitives[3].vertex_indices = {0, 5, 1, 2};
    auto& child = source->objects[1];
    child.parent = 0;
    child.next_sibling = 2;
    child.offset_from_parent = {2 * fixed, fixed / 3, fixed};
    child.vertices = {{0, 0, 0}, {2 * fixed, 0, 0}, {2 * fixed, 0, 2 * fixed}, {0, 0, 2 * fixed}};
    child.primitives.resize(2);
    child.primitives[0].vertex_indices = {0, 1, 2, 3};
    child.primitives[1].vertex_indices = {0, 2, 3};
    auto& hidden = source->objects[2];
    hidden.parent = 0;
    hidden.vertices = {
        {-200 * fixed, 0, -200 * fixed},
        {200 * fixed, 0, -200 * fixed},
        {200 * fixed, 0, 200 * fixed}
    };
    hidden.primitives.resize(1);
    hidden.primitives[0].vertex_indices = {0, 1, 2};
    drawing::PreparedModel prepared;
    prepared.objects.resize(source->objects.size());
    for (uint32_t object = 0; object < source->objects.size(); ++object)
        for (uint32_t polygon = 0; polygon < source->objects[object].primitives.size(); ++polygon) {
            drawing::PreparedPrimitive primitive;
            primitive.source_index = polygon;
            primitive.flags = drawing::primitive_colored;
            primitive.color = static_cast<uint8_t>(40 + object * 4 + polygon);
            prepared.objects[object].primitives.push_back(primitive);
        }
    // This quad has valid indices but no readable texture. Its distant
    // corner must not enlarge the pick's bounds.
    prepared.objects[0].primitives[3].flags = 0;
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    for (auto& piece : instance.pieces())
        if (piece.object_index == 2)
            piece.flags = 0;
    const auto& posed_body = instance.pieces()[0].transformed_vertices;
    const std::array first{
        std::array{posed_body[0].x / fixed_scale, 0.0F, -posed_body[0].z / fixed_scale},
        std::array{posed_body[1].x / fixed_scale, 0.0F, -posed_body[1].z / fixed_scale},
        std::array{posed_body[2].x / fixed_scale, 0.0F, -posed_body[2].z / fixed_scale}
    };
    const float normal_y = (first[0][2] - first[1][2]) * (first[0][0] + first[1][0]) +
                           (first[1][2] - first[2][2]) * (first[1][0] + first[2][0]) +
                           (first[2][2] - first[0][2]) * (first[2][0] + first[0][0]);
    OA_CHECK(normal_y != 0);
    in.view_direction = {0, normal_y < 0 ? 1.0F : -1.0F, 0};
    std::array<oa::Unit, population> units{};
    battle.draws.models.resize(population);
    for (uint32_t index = 0; index < population; ++index) {
        units[index].id = static_cast<uint16_t>(index + 1);
        auto& draw = battle.draws.models[index];
        draw.model.unit = &units[index];
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.plan.linked.drawn = true;
    }
    constexpr std::array<models::FixedVector3, 4> origins{
        {{8 * fixed + 1, fixed / 7, 8 * fixed - 1},
         {8 * fixed + fixed / 3, -fixed / 9, 9 * fixed + fixed / 13},
         {30000 * fixed + 1, -20000 * fixed + 17, 25000 * fixed - 1},
         {-30000 * fixed - 1, 20000 * fixed - 17, -25000 * fixed + 1}}
    };
    std::array<live::Scene, 2> scenes;
    bool saw_bounded{}, saw_unbounded{};
    for (const auto& origin : origins)
        for (const bool sliver : {false, true}) {
            units[0].position = {origin.x, origin.y, origin.z};
            units[1].position = {origin.x + fixed / 3, origin.y - fixed / 7, origin.z + fixed / 5};
            const float x = origin.x / fixed_scale;
            const float y = origin.y / fixed_scale;
            const float z = origin.z / fixed_scale;
            in.clip_projection = {
                1, 0, 0, 32 - x, 0, -0.5F, 1, 32 - (z - y * 0.5F), 0, -0.001F, 0, 0.5F
            };
            if (sliver)
                std::copy_n(in.clip_projection.begin(), 4, in.clip_projection.begin() + 4);
            auto& changed = battle.draws.models[1];
            changed.shown_vertices.clear();
            if (sliver) {
                for (const auto& piece : instance.pieces())
                    changed.shown_vertices.push_back(piece.transformed_vertices);
                changed.shown_vertices[1][1].x += fixed / 11;
            }
            in.models_on_card = false;
            OA_CHECK(live::build(in, scenes[0], battle.error));
            in.models_on_card = true;
            OA_CHECK(live::build(in, scenes[1], battle.error));
            OA_CHECK(scenes[1].cards.units.size() == population);
            OA_CHECK(same_on_card(scenes[1], scenes[0], in));
            for (const auto& drawn : scenes[1].cards.units) {
                OA_CHECK(drawn.unit_id > 0 && drawn.unit_id <= population);
                if (drawn.unit_id == 0 || drawn.unit_id > population)
                    continue;
                const auto& draw = battle.draws.models[drawn.unit_id - 1];
                const auto position = draw.shown_position.value_or(draw.model.unit->position);
                const std::array place{
                    position.x / fixed_scale, position.y / fixed_scale, position.z / fixed_scale
                };
                std::vector<std::array<float, 3>> expected;
                std::array bounds{
                    std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()
                };
                bool bounded = true;
                uint32_t back_faces{}, missing_frames{}, hidden_pieces{};
                const auto pieces = draw.model.instance->pieces();
                for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                    if ((pieces[piece].flags &
                         static_cast<uint16_t>(runtime::PieceFlag::visible)) == 0) {
                        ++hidden_pieces;
                        continue;
                    }
                    const auto& points = piece < draw.shown_vertices.size()
                                             ? draw.shown_vertices[piece]
                                             : pieces[piece].transformed_vertices;
                    const auto object = pieces[piece].object_index;
                    for (const auto& primitive : prepared.objects[object].primitives) {
                        if ((primitive.flags & drawing::primitive_colored) == 0 &&
                            primitive.frame == nullptr) {
                            ++missing_frames;
                            continue;
                        }
                        const auto& indices = source->objects[object]
                                                  .primitives[primitive.source_index]
                                                  .vertex_indices;
                        std::vector<std::array<float, 3>> corners;
                        for (const auto index : indices) {
                            const auto& point = points[index];
                            corners.push_back(
                                {place[0] + point.x / fixed_scale,
                                 place[1] + point.y / fixed_scale,
                                 place[2] - point.z / fixed_scale}
                            );
                        }
                        std::array<float, 3> normal{};
                        for (std::size_t index = 0; index < corners.size(); ++index) {
                            const auto& a = corners[index];
                            const auto& b = corners[(index + 1) % corners.size()];
                            normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
                            normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
                            normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
                        }
                        if (normal[0] * in.view_direction[0] + normal[1] * in.view_direction[1] +
                                normal[2] * in.view_direction[2] >=
                            0) {
                            ++back_faces;
                            continue;
                        }
                        std::vector<std::array<float, 2>> projected;
                        const auto& p = in.clip_projection;
                        for (const auto& corner : corners) {
                            const float across =
                                p[0] * corner[0] + p[1] * corner[1] + p[2] * corner[2] + p[3];
                            const float down =
                                p[4] * corner[0] + p[5] * corner[1] + p[6] * corner[2] + p[7];
                            bounds = {
                                std::min(bounds[0], across),
                                std::min(bounds[1], down),
                                std::max(bounds[2], across),
                                std::max(bounds[3], down)
                            };
                            projected.push_back({across, down});
                        }
                        for (std::size_t fan = 1; fan + 1 < corners.size(); ++fan) {
                            for (const auto corner : {std::size_t{0}, fan, fan + 1})
                                expected.push_back(corners[corner]);
                            const auto& a = projected[0];
                            const auto& b = projected[fan];
                            const auto& c = projected[fan + 1];
                            const double first_area = (double(b[1]) - c[1]) * (double(a[0]) - c[0]);
                            const double second_area =
                                (double(c[0]) - b[0]) * (double(a[1]) - c[1]);
                            bounded &= std::abs(first_area + second_area) >
                                       (std::abs(first_area) + std::abs(second_area)) *
                                           minimum_relative_area;
                        }
                    }
                }
                OA_CHECK(back_faces > 0 && missing_frames == 1 && hidden_pieces == 1);
                OA_CHECK(expected.size() == drawn.index_count);
                OA_CHECK(drawn.left == bounds[0] && drawn.top == bounds[1]);
                OA_CHECK(drawn.right == bounds[2] && drawn.bottom == bounds[3]);
                OA_CHECK(drawn.bounded == bounded);
                saw_bounded |= bounded;
                saw_unbounded |= !bounded;
                for (std::size_t corner = 0;
                     corner < std::min(expected.size(), std::size_t{drawn.index_count});
                     ++corner) {
                    const auto actual = live::card_corner(
                        scenes[1].card_types,
                        scenes[1].cards,
                        drawn,
                        scenes[1].cards.indices[drawn.first_index + corner]
                    );
                    const std::array position{actual.x, actual.height, actual.z};
                    for (std::size_t axis = 0; axis < position.size(); ++axis)
                        OA_CHECK(
                            std::bit_cast<uint32_t>(position[axis]) ==
                            std::bit_cast<uint32_t>(expected[corner][axis])
                        );
                }
            }
        }
    OA_CHECK(saw_bounded && saw_unbounded);
}

/// A high indexed vertex retains exact geometry outside the reusable prefix.
void high_indexed_model_vertex_projection() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr float fixed_scale = static_cast<float>(fixed);
    constexpr uint16_t high_vertex = 33000;
    Fixture battle;
    auto& in = battle.inputs;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.clip_width = in.clip_height = 128;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    auto& object = source->objects[0];
    object.vertices.resize(high_vertex + 1);
    object.vertices[1].x = 4 * fixed + 1;
    object.vertices[high_vertex].z = 4 * fixed - 1;
    object.primitives.resize(2);
    object.primitives[0].vertex_indices = {0, 1, high_vertex};
    object.primitives[1].vertex_indices = {0, high_vertex, 1};
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    for (uint32_t polygon = 0; polygon < object.primitives.size(); ++polygon) {
        drawing::PreparedPrimitive primitive;
        primitive.source_index = polygon;
        primitive.flags = drawing::primitive_colored;
        primitive.color = static_cast<uint8_t>(40 + polygon);
        prepared.objects[0].primitives.push_back(primitive);
    }
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    oa::Unit unit{};
    unit.id = 7;
    unit.position = {30000 * fixed + 1, -fixed / 3, -25000 * fixed + 1};
    const std::array origin{
        unit.position.x / fixed_scale, unit.position.y / fixed_scale, unit.position.z / fixed_scale
    };
    in.clip_projection = {
        1,
        0,
        0,
        32 - origin[0],
        0,
        -0.5F,
        1,
        32 - (origin[2] - origin[1] * 0.5F),
        0,
        -0.001F,
        0,
        0.5F
    };
    battle.draws.models.resize(1);
    auto& draw = battle.draws.models[0];
    draw.model.unit = &unit;
    draw.model.instance = &instance;
    draw.model.prepared = &prepared;
    draw.plan.linked.drawn = true;
    live::Scene expanded;
    OA_CHECK(live::build(in, expanded, battle.error));
    in.models_on_card = true;
    OA_CHECK(live::build(in, battle.scene, battle.error));
    OA_CHECK(same_on_card(battle.scene, expanded, in));
    OA_CHECK(battle.scene.cards.units.size() == 1);
    if (battle.scene.cards.units.empty())
        return;
    const auto& drawn = battle.scene.cards.units[0];
    constexpr std::array<uint16_t, 6> indices{0, 1, high_vertex, 0, high_vertex, 1};
    OA_CHECK(drawn.index_count == indices.size() && drawn.bounded);
    std::array bounds{
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()
    };
    for (std::size_t index = 0; index < std::min(indices.size(), std::size_t{drawn.index_count});
         ++index) {
        const auto& point = instance.pieces()[0].transformed_vertices[indices[index]];
        const std::array expected{
            origin[0] + point.x / fixed_scale,
            origin[1] + point.y / fixed_scale,
            origin[2] - point.z / fixed_scale
        };
        const auto corner = live::card_corner(
            battle.scene.card_types,
            battle.scene.cards,
            drawn,
            battle.scene.cards.indices[drawn.first_index + index]
        );
        const std::array actual{corner.x, corner.height, corner.z};
        for (std::size_t axis = 0; axis < actual.size(); ++axis)
            OA_CHECK(
                std::bit_cast<uint32_t>(actual[axis]) == std::bit_cast<uint32_t>(expected[axis])
            );
        const auto& p = in.clip_projection;
        const float x = p[0] * expected[0] + p[1] * expected[1] + p[2] * expected[2] + p[3];
        const float y = p[4] * expected[0] + p[5] * expected[1] + p[6] * expected[2] + p[7];
        bounds = {
            std::min(bounds[0], x),
            std::min(bounds[1], y),
            std::max(bounds[2], x),
            std::max(bounds[3], y)
        };
    }
    OA_CHECK(drawn.left == bounds[0] && drawn.top == bounds[1]);
    OA_CHECK(drawn.right == bounds[2] && drawn.bottom == bounds[3]);
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
        check_deferred_image_draw(in, pictured);
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

/// Projected detail follows the presented animated hull and zoom, keeps
/// near geometry exact and never replaces hidden/cloaked/unfinished units.
void projected_unit_detail() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    Fixture battle;
    battle.inputs.sight.mapping = battle.inputs.sight.line_of_sight = false;
    battle.inputs.clip_width = battle.inputs.clip_height = 128;
    battle.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, -0.001F, 0, 0.5F};
    battle.inputs.owner_colors[0] = {255, 128, 32};
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 0, 0}, {4 * fixed, 0, 0}, {0, 0, 4 * fixed}};
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    prepared.objects[0].primitives.push_back({});
    prepared.objects[0].primitives[0].flags = drawing::primitive_colored;
    prepared.objects[0].primitives[0].color = ground_index;
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    oa::Unit unit{};
    unit.id = 7;
    unit.position = {16 * fixed, 0, 16 * fixed};
    battle.draws.models.resize(1);
    auto& draw = battle.draws.models[0];
    draw.model.unit = &unit;
    draw.model.instance = &instance;
    draw.model.prepared = &prepared;
    draw.plan.linked.drawn = true;
    live::Scene full;
    OA_CHECK(live::build(battle.inputs, full, battle.error));
    battle.inputs.unit_marker_pixels = 5;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_detail.size() == 1);
    if (battle.scene.unit_detail.size() != 1)
        return;
    OA_CHECK(battle.scene.unit_detail_active && battle.scene.unit_detail[0].marker);
    OA_CHECK(battle.scene.unit_ranges.size() == 1);
    OA_CHECK(battle.scene.unit_ranges[0].unit_id == unit.id);
    OA_CHECK(battle.scene.unit_ranges[0].vertex_count == 6);
    const auto first = battle.scene.unit_ranges[0].first_vertex;
    OA_CHECK(triangle_color(battle.scene, first) == (std::array<uint8_t, 4>{255, 128, 32, 255}));
    const auto& p = battle.inputs.clip_projection;
    float left = 128, top = 128, right{}, bottom{};
    for (std::size_t index = first; index < first + 6; ++index) {
        const auto& vertex = battle.scene.vertices[index];
        const float x = p[0] * vertex.x + p[1] * vertex.height + p[2] * vertex.z + p[3];
        const float y = p[4] * vertex.x + p[5] * vertex.height + p[6] * vertex.z + p[7];
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    }
    OA_CHECK(std::abs(right - left - 5) < tolerance);
    OA_CHECK(std::abs(bottom - top - 5) < tolerance);
    // A zoomed-in hull keeps the exact complete draw/atlas/ranges.
    battle.inputs.clip_projection[0] *= 4;
    battle.inputs.clip_projection[5] *= 4;
    battle.inputs.clip_projection[6] *= 4;
    battle.inputs.unit_marker_pixels = 0;
    live::Scene near_full;
    OA_CHECK(live::build(battle.inputs, near_full, battle.error));
    battle.inputs.unit_marker_pixels = 5;
    live::Scene near;
    OA_CHECK(live::build(battle.inputs, near, battle.error));
    OA_CHECK(near.unit_detail_active && !near.unit_detail[0].marker);
    OA_CHECK(same_frame(near_full, near));
    // The actual interpolated extended piece prevents a static-footprint
    // estimate from turning a large animated model into a tiny marker.
    battle.inputs.clip_projection[0] /= 4;
    battle.inputs.clip_projection[5] /= 4;
    battle.inputs.clip_projection[6] /= 4;
    draw.shown_vertices = {instance.pieces()[0].transformed_vertices};
    draw.shown_vertices[0][1].x = -20 * fixed;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.unit_detail[0].marker);
    draw.shown_vertices.clear();
    for (const auto remaining : {0.5F, 1.0F}) {
        unit.build_remaining = remaining;
        OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
        OA_CHECK(!battle.scene.unit_detail[0].eligible);
    }
    unit.build_remaining = 0;
    unit.state_flags |= drawing::unit_state_cloaked;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.unit_detail[0].eligible);
    OA_CHECK(!battle.scene.translucent_ranges.empty());
    unit.state_flags = 0;
    instance.pieces()[0].flags = 0;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.unit_detail[0].eligible && battle.scene.unit_ranges.empty());
    instance.pieces()[0].flags = static_cast<uint16_t>(runtime::PieceFlag::visible);
    battle.inputs.view_direction = {0, 1, 0};
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.unit_detail[0].eligible && battle.scene.unit_ranges.empty());
    battle.inputs.view_direction = {};
    drawing::ModelState walls;
    walls.image = oa::present::create_sprite(8, 8);
    draw.model.state = &walls;
    draw.plan.model.from_image = true;
    prepared.objects[0].primitives[0].color = drawing::image_key;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.unit_detail[0].eligible && battle.scene.unit_ranges.empty());
    draw.plan.model.from_image = false;
    battle.inputs.sight.mapping = battle.inputs.sight.line_of_sight = true;
    battle.coverage[0] = battle.mapped[0] = 0;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.unit_detail[0].eligible && battle.scene.unit_ranges.empty());
}

/// Shared posed hulls keep each translated unit's exact bounds and identity.
void shared_unit_detail_bounds() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr uint32_t population = 12;
    Fixture battle;
    battle.inputs.sight.mapping = battle.inputs.sight.line_of_sight = false;
    battle.inputs.clip_width = battle.inputs.clip_height = 128;
    battle.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, -0.001F, 0, 0.5F};
    battle.inputs.unit_marker_pixels = 5;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {
        {-fixed / 3, fixed / 7, fixed / 5},
        {3 * fixed + 1, fixed / 11, -fixed / 13},
        {fixed / 9, -fixed / 17, 4 * fixed - 1}
    };
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2};
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    prepared.objects[0].primitives.push_back({});
    prepared.objects[0].primitives[0].flags = drawing::primitive_colored;
    prepared.objects[0].primitives[0].color = ground_index;
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    std::vector<oa::Unit> units(population);
    battle.draws.models.resize(population);
    for (uint32_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.position = {
            8 * fixed + int32_t(index) * (fixed / 10),
            int32_t(index % 3) * (fixed / 5),
            8 * fixed + int32_t(index / 3) * (fixed / 11)
        };
        auto& draw = battle.draws.models[index];
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.plan.linked.drawn = true;
    }
    const auto check_single_draws = [&] {
        for (std::size_t index = 0; index < battle.draws.models.size(); ++index) {
            oa::app::WorldDrawList one;
            one.models.push_back(battle.draws.models[index]);
            auto inputs = battle.inputs;
            inputs.draws = &one;
            live::Scene reference;
            OA_CHECK(live::build(inputs, reference, battle.error));
            if (reference.unit_detail.empty() || index >= battle.scene.unit_detail.size())
                continue;
            const auto& actual = battle.scene.unit_detail[index];
            const auto& expected = reference.unit_detail[0];
            if (std::all_of(expected.center.begin(), expected.center.end(), [](float value) {
                    return std::isfinite(value);
                }))
                OA_CHECK(actual.center == expected.center);
            OA_CHECK(actual.pixels == expected.pixels);
            OA_CHECK(actual.surface == expected.surface && actual.shadow == expected.shadow);
            OA_CHECK(actual.eligible == expected.eligible && actual.marker == expected.marker);
            // Translate every presented point before taking extrema, so
            // sharing local hulls is held to independent world arithmetic.
            constexpr float fixed_scale = static_cast<float>(fixed);
            const auto& draw = battle.draws.models[index];
            const auto position = draw.shown_position.value_or(draw.model.unit->position);
            const std::array origin{
                position.x / fixed_scale, position.y / fixed_scale, position.z / fixed_scale
            };
            std::array world_low{
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity()
            };
            std::array world_high{-world_low[0], -world_low[1], -world_low[2]};
            const auto pieces = draw.model.instance->pieces();
            for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                if ((pieces[piece].flags & static_cast<uint16_t>(runtime::PieceFlag::visible)) == 0)
                    continue;
                const auto& points = piece < draw.shown_vertices.size()
                                         ? draw.shown_vertices[piece]
                                         : pieces[piece].transformed_vertices;
                for (const auto& point : points) {
                    const std::array world{
                        origin[0] + point.x / fixed_scale,
                        origin[1] + point.y / fixed_scale,
                        origin[2] - point.z / fixed_scale
                    };
                    for (std::size_t axis = 0; axis < world.size(); ++axis) {
                        world_low[axis] = std::min(world_low[axis], world[axis]);
                        world_high[axis] = std::max(world_high[axis], world[axis]);
                    }
                }
            }
            if (std::all_of(world_low.begin(), world_low.end(), [](float value) {
                    return std::isfinite(value);
                })) {
                for (std::size_t axis = 0; axis < world_low.size(); ++axis)
                    OA_CHECK(actual.center[axis] == (world_low[axis] + world_high[axis]) * 0.5F);
                float expected_pixels{};
                for (std::size_t row = 0; row < 2; ++row) {
                    float extent{};
                    for (std::size_t axis = 0; axis < world_low.size(); ++axis)
                        extent += std::abs(inputs.clip_projection[row * 4 + axis]) *
                                  (world_high[axis] - world_low[axis]);
                    expected_pixels = std::max(expected_pixels, extent);
                }
                OA_CHECK(actual.pixels == expected_pixels);
            }
        }
    };
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.detail_poses.size() == 1);
    OA_CHECK(battle.scene.detail_pose_points.size() == 3);
    OA_CHECK(battle.scene.detail_pose_hits == population - 1);
    OA_CHECK(battle.scene.unit_ranges.size() == population);
    for (std::size_t index = 0; index < battle.scene.unit_ranges.size(); ++index) {
        OA_CHECK(battle.scene.unit_ranges[index].unit_id == units[index].id);
        OA_CHECK(battle.scene.unit_ranges[index].vertex_count == 6);
    }
    check_single_draws();
    // The same source model with one presented vertex extended is a
    // different hull, even while every other unit shares the old pose.
    auto& extended = battle.draws.models.back();
    extended.shown_vertices = {instance.pieces()[0].transformed_vertices};
    extended.shown_vertices[0][1].x += 20 * fixed;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.detail_poses.size() == 2);
    OA_CHECK(!battle.scene.unit_detail.back().marker);
    check_single_draws();
    // Visibility belongs to the pose as well as its local coordinates.
    auto hidden = runtime::make_instance(source);
    hidden.rebuild_transforms({});
    hidden.pieces()[0].flags = 0;
    battle.draws.models[0].model.instance = &hidden;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.unit_detail[0].eligible);
    OA_CHECK(
        std::none_of(
            battle.scene.unit_ranges.begin(),
            battle.scene.unit_ranges.end(),
            [&](const live::UnitRange& range) { return range.unit_id == units[0].id; }
        )
    );
    check_single_draws();
    // A later build sees a different presented view and pose, without
    // retaining the first build's face or hull summary.
    battle.draws.models[0].model.instance = &instance;
    extended.shown_vertices.clear();
    battle.inputs.view_direction = {0, 1, 0};
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_ranges.empty());
    battle.inputs.view_direction = {};
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.detail_poses.size() == 1);
    OA_CHECK(battle.scene.unit_ranges.size() == population);
    check_single_draws();
    // World translations near the fixed-point limits keep the same
    // result as separately classifying each model through its camera.
    constexpr int32_t distant_origin = 30000;
    for (const int32_t sign : {-1, 1}) {
        for (uint32_t index = 0; index < population; ++index)
            units[index].position = {
                sign * distant_origin * fixed + int32_t(index) * (fixed / 10),
                0,
                sign * distant_origin * fixed + int32_t(index) * (fixed / 11)
            };
        battle.inputs.clip_projection[3] = float(-sign * distant_origin + 8);
        battle.inputs.clip_projection[7] = float(-sign * distant_origin + 8);
        OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
        OA_CHECK(battle.scene.detail_poses.size() == 1);
        OA_CHECK(battle.scene.unit_ranges.size() == population);
        check_single_draws();
    }
    // Distinct poses beyond the bounded cache still retain every unit's
    // surface and identity through the ordinary uncached summary path.
    constexpr uint32_t pose_limit = 1024;
    constexpr uint32_t point_limit = 131072;
    constexpr uint32_t piece_limit = 32768;
    constexpr uint32_t distinct_population = pose_limit + 7;
    units.resize(distinct_population);
    battle.draws.models.resize(distinct_population);
    battle.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, -0.001F, 0, 0.5F};
    for (uint32_t index = 0; index < distinct_population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.position = {8 * fixed, 0, 8 * fixed};
        auto& draw = battle.draws.models[index];
        draw = {};
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.plan.linked.drawn = true;
        draw.shown_vertices = {instance.pieces()[0].transformed_vertices};
        draw.shown_vertices[0][1].x += int32_t(index);
    }
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.detail_poses.empty() && battle.scene.detail_poses.size() <= pose_limit);
    OA_CHECK(battle.scene.detail_pose_points.size() <= point_limit);
    OA_CHECK(battle.scene.detail_pose_pieces.size() <= piece_limit);
    OA_CHECK(battle.scene.unit_ranges.size() == distinct_population);
    std::vector<bool> represented(distinct_population + 1);
    for (const auto& range : battle.scene.unit_ranges) {
        OA_CHECK(range.unit_id > 0 && range.unit_id <= distinct_population);
        if (range.unit_id > 0 && range.unit_id <= distinct_population) {
            OA_CHECK(!represented[range.unit_id]);
            represented[range.unit_id] = true;
        }
    }
}

/// Equal posed hulls retain distinct visibility for each team's texture frame.
void shared_unit_detail_texture_visibility() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr uint32_t population = 6;
    Fixture battle;
    battle.inputs.sight.mapping = battle.inputs.sight.line_of_sight = false;
    battle.inputs.clip_width = battle.inputs.clip_height = 128;
    battle.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, -0.001F, 0, 0.5F};
    battle.inputs.unit_marker_pixels = 5;
    battle.inputs.team_colors[1] = 1;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {
        {0, 0, 0}, {4 * fixed, 0, 0}, {4 * fixed, 0, 4 * fixed}, {0, 0, 4 * fixed}
    };
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2, 3};
    drawing::TextureSequence texture;
    texture.frames.resize(2);
    uint8_t pixel = ground_index;
    for (auto& frame : texture.frames) {
        frame.width = frame.height = 1;
        frame.encoding = OA_SPRITE_RAW;
    }
    texture.frames[0].data = &pixel;
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    prepared.objects[0].primitives.push_back({});
    prepared.objects[0].primitives[0].flags = drawing::primitive_animated | drawing::primitive_team;
    prepared.objects[0].primitives[0].texture = &texture;
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    std::array<oa::Unit, population> units{};
    battle.draws.models.resize(population);
    for (uint32_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.owner_index = static_cast<uint8_t>(index % 2);
        unit.position = {int32_t(8 + index * 6) * fixed, 0, 8 * fixed};
        auto& draw = battle.draws.models[index];
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.plan.linked.drawn = true;
    }
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_ranges.size() == population / 2);
    for (uint32_t index = 0; index < population; ++index) {
        OA_CHECK(battle.scene.unit_detail[index].eligible == (index % 2 == 0));
        OA_CHECK(battle.scene.unit_detail[index].marker == (index % 2 == 0));
    }
    // Adding the previously missing team frame is visible on the next
    // build even though the prepared model and all posed vertices persist.
    texture.frames[1].data = &pixel;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_ranges.size() == population);
    for (const auto& detail : battle.scene.unit_detail)
        OA_CHECK(detail.eligible && detail.marker);
    // Image-key walls are suppressed only for draws that actually use
    // an image. Equal geometry cannot share that face-visibility result.
    auto& primitive = prepared.objects[0].primitives[0];
    primitive.flags = drawing::primitive_colored;
    primitive.color = drawing::image_key;
    drawing::ModelState image;
    image.image = oa::present::create_sprite(8, 8);
    for (uint32_t index = 0; index < population; ++index) {
        auto& draw = battle.draws.models[index];
        draw.model.state = &image;
        draw.plan.model.from_image = index % 2 != 0;
    }
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_ranges.size() == population / 2);
    for (uint32_t index = 0; index < population; ++index)
        OA_CHECK(battle.scene.unit_detail[index].eligible == (index % 2 == 0));
    static_assert(alternate_index != drawing::image_key);
    primitive.color = alternate_index;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_ranges.size() == population);
}

/// Detail workers retain serial decisions, drawing order and pick bounds across frames.
void unit_detail_on_threads() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr uint32_t population = 1536;
    Fixture battle;
    auto& map = battle.map;
    map.attribute_width = map.attribute_height = 16;
    map.tile_width = map.tile_height = 8;
    map.attributes.resize(256);
    map.tile_indices.assign(64, 0);
    auto& in = battle.inputs;
    in.right = in.bottom = 256;
    in.clip_width = in.clip_height = 256;
    in.clip_projection = {1, 0, 0.05F, 0, 0, -0.5F, 1, 0, 0, -0.001F, 0, 0.5F};
    in.unit_marker_pixels = 5;
    in.team_colors[1] = 1;
    in.owner_colors[0] = {255, 128, 32};
    in.owner_colors[1] = {32, 128, 255};
    std::vector<uint16_t> coverage(64), mapped(64);
    for (std::size_t cell = 0; cell < coverage.size(); ++cell) {
        coverage[cell] = cell % 5 == 0 ? 0 : 1;
        mapped[cell] = cell % 7 == 0 ? 0 : viewer_bit;
    }
    in.sight.coverage = coverage;
    in.sight.player_bits = mapped;
    in.sight.width = in.sight.height = 8;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {
        {0, 0, 0}, {4 * fixed, 0, 0}, {4 * fixed, 0, 4 * fixed}, {0, 0, 4 * fixed}
    };
    source->objects[0].primitives.resize(1);
    source->objects[0].primitives[0].vertex_indices = {0, 1, 2, 3};
    drawing::TextureSequence texture;
    texture.frames.resize(2);
    std::array<uint8_t, 2> pixels{ground_index, alternate_index};
    for (auto& frame : texture.frames) {
        frame.width = frame.height = 1;
        frame.encoding = OA_SPRITE_RAW;
    }
    texture.frames[0].data = &pixels[0];
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    prepared.objects[0].primitives.push_back({});
    prepared.objects[0].primitives[0].flags = drawing::primitive_animated | drawing::primitive_team;
    prepared.objects[0].primitives[0].texture = &texture;
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    auto hidden = runtime::make_instance(source);
    hidden.rebuild_transforms({});
    hidden.pieces()[0].flags = 0;
    const auto& posed = instance.pieces()[0].transformed_vertices;
    const float normal =
        (posed[0].z - posed[1].z) / float{fixed} * ((posed[0].x + posed[1].x) / float{fixed}) +
        (posed[1].z - posed[2].z) / float{fixed} * ((posed[1].x + posed[2].x) / float{fixed}) +
        (posed[2].z - posed[0].z) / float{fixed} * ((posed[2].x + posed[0].x) / float{fixed});
    // The scene negates local z on the way into world coordinates.
    const float toward_front = normal > 0 ? 1.0F : -1.0F;
    std::vector<oa::Unit> units(population);
    battle.draws.models.resize(population);
    for (uint32_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.owner_index = static_cast<uint8_t>(index % 2);
        unit.position = {
            int32_t(8 + index % 48 * 5) * fixed,
            int32_t(index % 3) * (fixed / 4),
            int32_t(8 + index / 48 * 7) * fixed
        };
        if (index % 8 == 1)
            unit.state_flags = OA_UNIT_STATE_CLOAKED;
        else if (index % 8 == 2)
            unit.build_remaining = 0.4F;
        else if (index % 8 == 3)
            unit.flags = OA_UNIT_FLAG_BUILDING;
        auto& draw = battle.draws.models[index];
        draw.model.unit = &unit;
        draw.model.instance = index % 13 == 0 ? &hidden : &instance;
        draw.model.prepared = &prepared;
        draw.plan.linked.drawn = true;
    }
    oa::platform::job_pool::Pool two(2), four(4);
    std::array<live::Scene, 3> scenes;
    for (uint32_t frame = 0; frame < 2; ++frame) {
        in.view_direction = {0, frame == 0 ? toward_front : -toward_front, 0};
        if (frame != 0) {
            instance.pieces()[0].translation.x += fixed / 13;
            instance.rebuild_transforms({});
            in.clip_projection[0] = 0.875F;
            in.clip_projection[3] = 2.5F;
            in.clip_projection[6] = 0.9375F;
            in.clip_projection[7] = -1.25F;
            texture.frames[1].data = &pixels[1];
            pixels[0] = alternate_index;
        }
        for (uint32_t index = 0; index < population; ++index) {
            auto& draw = battle.draws.models[index];
            units[index].position.x += fixed / 17;
            if (index % 19 == 0)
                draw.shown_position = oa::FixedVec3{
                    units[index].position.x + fixed / 7,
                    units[index].position.y,
                    units[index].position.z - fixed / 11
                };
            draw.shown_vertices.clear();
            if (index % 7 == 0 || index % 11 == 0 || index % 17 == 0) {
                draw.shown_vertices = {draw.model.instance->pieces()[0].transformed_vertices};
                if (index % 7 == 0)
                    draw.shown_vertices[0][1].x += int32_t(index);
                if (index % 17 == 0)
                    draw.shown_vertices[0][1].x += 10 * fixed;
                if (index % 11 == 0)
                    std::swap(draw.shown_vertices[0][1], draw.shown_vertices[0][3]);
            }
        }
        in.models_on_card = true;
        for (std::size_t scene = 0; scene < scenes.size(); ++scene) {
            in.pool = scene == 0 ? nullptr : scene == 1 ? &two : &four;
            OA_CHECK(live::build(in, scenes[scene], battle.error));
            OA_CHECK(scenes[scene].unit_detail.size() == population);
        }
        OA_CHECK(scenes[0].unit_detail_active);
        OA_CHECK(!scenes[0].cards.units.empty());
        OA_CHECK(
            std::any_of(
                scenes[0].unit_detail.begin(),
                scenes[0].unit_detail.end(),
                [](const live::UnitDetail& detail) { return detail.marker; }
            )
        );
        OA_CHECK(
            std::any_of(
                scenes[0].unit_detail.begin(),
                scenes[0].unit_detail.end(),
                [](const live::UnitDetail& detail) { return !detail.eligible; }
            )
        );
        const auto serial = card_frame(scenes[0]);
        for (std::size_t scene = 1; scene < scenes.size(); ++scene) {
            OA_CHECK(same_frame(scenes[scene], scenes[0]));
            OA_CHECK(scenes[scene].unit_detail_active == scenes[0].unit_detail_active);
            for (std::size_t draw = 0;
                 draw < std::min(scenes[scene].unit_detail.size(), scenes[0].unit_detail.size());
                 ++draw) {
                const auto& actual = scenes[scene].unit_detail[draw];
                const auto& expected = scenes[0].unit_detail[draw];
                OA_CHECK(actual.surface == expected.surface && actual.shadow == expected.shadow);
                OA_CHECK(
                    std::bit_cast<uint32_t>(actual.pixels) ==
                    std::bit_cast<uint32_t>(expected.pixels)
                );
                OA_CHECK(
                    std::bit_cast<uint32_t>(actual.brightness) ==
                    std::bit_cast<uint32_t>(expected.brightness)
                );
                OA_CHECK(
                    (std::bit_cast<std::array<uint32_t, 3>>(actual.center) ==
                     std::bit_cast<std::array<uint32_t, 3>>(expected.center))
                );
                OA_CHECK(actual.eligible == expected.eligible && actual.marker == expected.marker);
            }
            const auto parallel = card_frame(scenes[scene]);
            OA_CHECK(parallel.units == serial.units);
            OA_CHECK(parallel.vertices.size() == serial.vertices.size());
            if (!parallel.vertices.empty() && parallel.vertices.size() == serial.vertices.size())
                OA_CHECK(
                    std::memcmp(
                        parallel.vertices.data(),
                        serial.vertices.data(),
                        parallel.vertices.size() * sizeof(parallel.vertices[0])
                    ) == 0
                );
            const auto& actual = scenes[scene].cards.units;
            const auto& expected = scenes[0].cards.units;
            OA_CHECK(
                std::equal(
                    actual.begin(),
                    actual.end(),
                    expected.begin(),
                    expected.end(),
                    [](const live::CardUnit& a, const live::CardUnit& b) {
                        return a.unit_id == b.unit_id && a.vertex == b.vertex &&
                               a.instance == b.instance && a.type == b.type &&
                               a.first_index == b.first_index && a.index_count == b.index_count &&
                               a.left == b.left && a.top == b.top && a.right == b.right &&
                               a.bottom == b.bottom && a.bounded == b.bounded &&
                               a.body_lod == b.body_lod;
                    }
                )
            );
        }
    }
    // Every worker has now seen this source, view and texture state. A
    // malformed face must fail again, rather than borrowing a cached face
    // result, and a repaired source must draw the exact prior frame.
    const auto before_failure = card_frame(scenes[0]);
    const auto before_details = scenes[0].unit_detail;
    const auto before_picks = scenes[0].cards.units;
    const auto before_atlas = scenes[0].texture_rgba;
    auto& broken_index = source->objects[0].primitives[0].vertex_indices[1];
    const auto original_index = broken_index;
    broken_index = std::numeric_limits<uint16_t>::max();
    std::array<std::string, 3> errors;
    for (std::size_t scene = 0; scene < scenes.size(); ++scene) {
        in.pool = scene == 0 ? nullptr : scene == 1 ? &two : &four;
        OA_CHECK(!live::build(in, scenes[scene], errors[scene]));
    }
    OA_CHECK(!errors[0].empty() && errors[1] == errors[0] && errors[2] == errors[0]);
    broken_index = original_index;
    const auto same_details = [](const auto& actual, const auto& expected) {
        return std::equal(
            actual.begin(),
            actual.end(),
            expected.begin(),
            expected.end(),
            [](const live::UnitDetail& a, const live::UnitDetail& b) {
                return a.surface == b.surface && a.shadow == b.shadow &&
                       std::bit_cast<uint32_t>(a.pixels) == std::bit_cast<uint32_t>(b.pixels) &&
                       std::bit_cast<uint32_t>(a.brightness) ==
                           std::bit_cast<uint32_t>(b.brightness) &&
                       std::bit_cast<std::array<uint32_t, 3>>(a.center) ==
                           std::bit_cast<std::array<uint32_t, 3>>(b.center) &&
                       a.eligible == b.eligible && a.marker == b.marker;
            }
        );
    };
    const auto same_picks = [](const auto& actual, const auto& expected) {
        return std::equal(
            actual.begin(),
            actual.end(),
            expected.begin(),
            expected.end(),
            [](const live::CardUnit& a, const live::CardUnit& b) {
                return a.unit_id == b.unit_id && a.vertex == b.vertex && a.instance == b.instance &&
                       a.type == b.type && a.first_index == b.first_index &&
                       a.index_count == b.index_count && a.left == b.left && a.top == b.top &&
                       a.right == b.right && a.bottom == b.bottom && a.bounded == b.bounded &&
                       a.body_lod == b.body_lod;
            }
        );
    };
    for (std::size_t scene = 0; scene < scenes.size(); ++scene) {
        in.pool = scene == 0 ? nullptr : scene == 1 ? &two : &four;
        OA_CHECK(live::build(in, scenes[scene], errors[scene]));
        OA_CHECK(errors[scene].empty());
        OA_CHECK(same_details(scenes[scene].unit_detail, before_details));
        OA_CHECK(same_picks(scenes[scene].cards.units, before_picks));
        OA_CHECK(scenes[scene].texture_rgba == before_atlas);
        const auto recovered = card_frame(scenes[scene]);
        OA_CHECK(recovered.units == before_failure.units);
        OA_CHECK(recovered.vertices.size() == before_failure.vertices.size());
        if (!recovered.vertices.empty() &&
            recovered.vertices.size() == before_failure.vertices.size())
            OA_CHECK(
                std::memcmp(
                    recovered.vertices.data(),
                    before_failure.vertices.data(),
                    recovered.vertices.size() * sizeof(recovered.vertices[0])
                ) == 0
            );
        if (scene != 0)
            OA_CHECK(same_frame(scenes[scene], scenes[0]));
    }
}

/// Twenty thousand visible, otherwise over-budget models retain a
/// representation and canonical pick ID without increasing either cap.
void crowded_unit_detail() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr uint32_t population = 20000;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    Fixture battle;
    battle.inputs.sight.mapping = battle.inputs.sight.line_of_sight = false;
    battle.inputs.clip_width = battle.inputs.clip_height = 128;
    battle.inputs.clip_projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, -0.001F, 0, 0.5F};
    battle.inputs.unit_marker_pixels = 5;
    battle.inputs.owner_colors[0] = {255, 128, 32};
    battle.inputs.owner_colors[1] = {32, 128, 255};
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    auto& object = source->objects[0];
    object.vertices = {{0, 0, 0}, {8 * fixed, 0, 0}, {0, 0, 8 * fixed}};
    object.primitives.resize(128);
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    for (uint32_t polygon = 0; polygon < object.primitives.size(); ++polygon) {
        object.primitives[polygon].vertex_indices = {0, 1, 2};
        drawing::PreparedPrimitive primitive;
        primitive.source_index = polygon;
        primitive.flags = drawing::primitive_colored;
        primitive.color = alternate_index;
        prepared.objects[0].primitives.push_back(primitive);
    }
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    drawing::ModelState state;
    state.image = oa::present::create_sprite(8, 8);
    battle.inputs.graphics_flags = drawing::graphics_shadows | drawing::graphics_vehicle_shadows;
    std::vector<oa::Unit> units(population);
    battle.draws.models.resize(population);
    for (uint32_t index = 0; index < population; ++index) {
        auto& unit = units[index];
        unit.id = static_cast<uint16_t>(index + 1);
        unit.owner_index = index >= population / 2 ? 1 : 0;
        unit.position = {16 * fixed, 0, 16 * fixed};
        auto& draw = battle.draws.models[index];
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.model.state = &state;
        draw.plan.linked.drawn = true;
        draw.plan.model.from_image = true;
    }
    battle.inputs.models_on_card = true;
    const bool built = live::build(battle.inputs, battle.scene, battle.error);
    OA_CHECK(built);
    if (!built)
        return;
    OA_CHECK(battle.scene.unit_detail_active);
    OA_CHECK(battle.scene.model_count == population);
    OA_CHECK(battle.scene.unit_ranges.size() + battle.scene.cards.units.size() == population);
    OA_CHECK(
        battle.scene.vertices.size() + battle.scene.cards.vertices <=
        oa::app::renderer_live_gpu::largest_vertex_count
    );
    OA_CHECK(
        battle.scene.cards.cast_indices.size() <= oa::app::renderer_live_gpu::largest_vertex_count
    );
    OA_CHECK(!battle.scene.cards.cast_indices.empty());
    OA_CHECK(!battle.scene.unit_detail.front().marker);
    OA_CHECK(battle.scene.unit_detail.back().marker);
    // Sorting prioritises size then canonical identity, not planner order.
    std::reverse(battle.draws.models.begin(), battle.draws.models.end());
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_detail.front().marker);
    OA_CHECK(!battle.scene.unit_detail.back().marker);
    OA_CHECK(battle.scene.unit_ranges.size() + battle.scene.cards.units.size() == population);
    // Far out, every unit is a cheap marker and no model positions or
    // surface/shadow indices are expanded at all.
    battle.inputs.clip_projection[0] *= 0.25F;
    battle.inputs.clip_projection[5] *= 0.25F;
    battle.inputs.clip_projection[6] *= 0.25F;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_ranges.size() == population);
    OA_CHECK(battle.scene.cards.instances.empty() && battle.scene.cards.positions.empty());
    OA_CHECK(battle.scene.cards.indices.empty() && battle.scene.cards.cast_indices.empty());
    OA_CHECK(battle.scene.vertices.size() == battle.scene.terrain_vertex_count + population * 6);
    // Mandatory full features outside the camera must reserve the zero
    // surface they emit, rather than displacing visible unit markers.
    oa::Unit feature{};
    feature.position = {1000 * fixed, 0, 1000 * fixed};
    battle.draws.stand_ins.push_back(feature);
    oa::UnitDef feature_type{};
    feature_type.flags = OA_UNIT_DEF_FLAG_NO_SHADOW;
    auto feature_draw = battle.draws.models.front();
    feature_draw.model.unit = nullptr;
    feature_draw.model.def = &feature_type;
    feature_draw.stand_in = 0;
    feature_draw.on_map = true;
    battle.draws.models.insert(battle.draws.models.end(), population, feature_draw);
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(
        battle.scene.model_count == population && battle.scene.unit_ranges.size() == population
    );
    // An all-hidden feature also takes no surface or shadow allowance.
    auto hidden = runtime::make_instance(source);
    hidden.rebuild_transforms({});
    hidden.pieces()[0].flags = 0;
    battle.draws.stand_ins[0].position = units[0].position;
    for (std::size_t index = population; index < battle.draws.models.size(); ++index)
        battle.draws.models[index].model.instance = &hidden;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(
        battle.scene.model_count == population && battle.scene.unit_ranges.size() == population
    );
}

/// Twenty thousand translated models share their exact pose without changing a corner.
void large_shared_model_scene() {
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr uint32_t population = 20000;
    constexpr uint32_t vertex_count = 64;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    Fixture battle;
    battle.inputs.sight.mapping = battle.inputs.sight.line_of_sight = false;
    battle.inputs.view_direction = {0, 1, 0};
    auto source = std::make_shared<models::Model>();
    source->objects.resize(1);
    auto& object = source->objects[0];
    object.vertices.resize(vertex_count);
    object.vertices[1].x = 2 * fixed;
    object.vertices[2].z = 2 * fixed;
    object.primitives.resize(2);
    object.primitives[0].vertex_indices = {0, 1, 2};
    object.primitives[1].vertex_indices = {0, 2, 1};
    drawing::PreparedModel prepared;
    prepared.objects.resize(1);
    for (uint32_t polygon = 0; polygon < 2; ++polygon) {
        drawing::PreparedPrimitive primitive;
        primitive.source_index = polygon;
        primitive.flags = drawing::primitive_colored;
        primitive.color = 40;
        prepared.objects[0].primitives.push_back(primitive);
    }
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    std::vector<oa::Unit> units(population);
    battle.draws.models.resize(population);
    for (uint32_t unit = 0; unit < population; ++unit) {
        auto& record = units[unit];
        record.id = static_cast<uint16_t>(unit + 1);
        record.position = {
            8 * fixed + static_cast<int32_t>(unit % 100) * (fixed / 10),
            static_cast<int32_t>(unit % 3) * (fixed / 4),
            8 * fixed + static_cast<int32_t>((unit / 100) % 100) * (fixed / 10)
        };
        auto& draw = battle.draws.models[unit];
        draw.model.unit = &record;
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.plan.linked.drawn = true;
    }
    live::Scene expanded;
    live::Scene shared;
    battle.inputs.models_on_card = false;
    OA_CHECK(live::build(battle.inputs, expanded, battle.error));
    battle.inputs.models_on_card = true;
    OA_CHECK(live::build(battle.inputs, shared, battle.error));
    OA_CHECK(shared.cards.instances.size() == population);
    OA_CHECK(shared.cards.positions.size() == vertex_count * 3);
    OA_CHECK(same_on_card(shared, expanded, battle.inputs));
    for (const auto& drawn : shared.cards.instances) {
        OA_CHECK(drawn.positions == 0 && drawn.position_count == vertex_count);
        OA_CHECK(drawn.origin[0] >= 8 && drawn.origin[2] >= 8);
    }
    // A later frame changes every translation without changing the retained
    // type or turning stale pose entries into a different model.
    for (auto& unit : units)
        unit.position.x += fixed / 17;
    battle.inputs.models_on_card = false;
    OA_CHECK(live::build(battle.inputs, expanded, battle.error));
    battle.inputs.models_on_card = true;
    OA_CHECK(live::build(battle.inputs, shared, battle.error));
    OA_CHECK(shared.cards.positions.size() == vertex_count * 3);
    OA_CHECK(same_on_card(shared, expanded, battle.inputs));
}

/// Keeps a real visible caster beside a large offscreen army in a cropped view.
struct ShadowViewFixture {
    static constexpr uint32_t view_edge = 64;
    static constexpr int32_t view_origin = 7168;
    static constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    oa::formats::tnt::Map map{};
    oa::PaletteBytes palette{};
    std::shared_ptr<oa::formats::objects3d::Model> source{};
    oa::sim::model_runtime::Instance instance{};
    oa::present::model::PreparedModel prepared{};
    oa::UnitDef definition{};
    oa::present::model::ModelState state{};
    std::vector<oa::Unit> units{};
    oa::app::WorldDrawList few{};
    oa::app::WorldDrawList many{};
    oa::app::renderer_live_scene::Inputs inputs{};

    /// Builds a shared posed model whose ordinary shadow allowance is substantial.
    /// @param population visible unit and offscreen units in the army
    explicit ShadowViewFixture(uint32_t population) : units(population) {
        namespace drawing = oa::present::model;
        constexpr uint32_t world_edge = 8192;
        constexpr uint32_t primitives = 256;
        map.attribute_width = map.attribute_height = world_edge / 16;
        map.tile_width = map.tile_height = world_edge / 32;
        map.tile_count = 1;
        map.tile_indices.assign(map.tile_width * map.tile_height, 0);
        map.attributes.resize(map.attribute_width * map.attribute_height);
        map.tile_palette_indices.assign(32 * 32, 1);
        for (const auto channel : {0U, 1U, 2U}) {
            palette[oa::palette_entry_bytes + channel] = 160;
            palette[2 * oa::palette_entry_bytes + channel] = 220;
        }
        source = std::make_shared<oa::formats::objects3d::Model>();
        source->objects.resize(1);
        auto& object = source->objects[0];
        object.vertices = {
            {0, 6 * fixed, 0},
            {8 * fixed, 6 * fixed, 0},
            {8 * fixed, 6 * fixed, -8 * fixed},
            {0, 6 * fixed, -8 * fixed}
        };
        object.primitives.resize(primitives);
        prepared.objects.resize(1);
        for (uint32_t index = 0; index < primitives; ++index) {
            object.primitives[index].vertex_indices = index % 2 == 0
                                                          ? std::vector<uint16_t>{0, 1, 2, 3}
                                                          : std::vector<uint16_t>{3, 2, 1, 0};
            drawing::PreparedPrimitive primitive;
            primitive.source_index = index;
            primitive.flags = drawing::primitive_colored;
            primitive.color = 2;
            prepared.objects[0].primitives.push_back(primitive);
        }
        instance = oa::sim::model_runtime::make_instance(source);
        instance.rebuild_transforms();
        state.image = oa::present::create_sprite(16, 16);
        std::fill(state.image.pixels.begin(), state.image.pixels.end(), 2);
        many.models.resize(population);
        for (uint32_t index = 0; index < population; ++index) {
            auto& unit = units[index];
            unit.id = static_cast<uint16_t>(index + 1);
            unit.type_index = 1;
            unit.position = {
                static_cast<int32_t>(1024 + index % 256) * fixed,
                0,
                static_cast<int32_t>(1024 + index / 256) * fixed
            };
            auto& draw = many.models[index];
            draw.model.unit = &unit;
            draw.model.def = &definition;
            draw.model.instance = &instance;
            draw.model.prepared = &prepared;
            draw.model.state = &state;
            draw.plan.linked.drawn = true;
            draw.plan.model.from_image = true;
        }
        units[0].position = {(view_origin + 24) * fixed, 0, (view_origin + 32) * fixed};
        few.models.push_back(many.models[0]);
        inputs.map = &map;
        inputs.palette = &palette;
        inputs.draws = &many;
        inputs.left = inputs.top = view_origin;
        inputs.right = inputs.bottom = view_origin + view_edge;
        inputs.clip_width = inputs.clip_height = view_edge;
        inputs.clip_projection = {
            1, 0, 0, -float(view_origin), 0, -0.5F, 1, -float(view_origin), 0, 1, 0.5F, 0
        };
        inputs.view_direction = {0, 1, 0};
        inputs.sight.mapping = inputs.sight.line_of_sight = false;
        inputs.models_on_card = true;
        inputs.unit_marker_pixels = 1;
        inputs.graphics_flags = drawing::graphics_shadows | drawing::graphics_vehicle_shadows;
    }
};

/// Independent packet bands retain the exact ordered surfaces and unit picks.
void physical_model_bands() {
    constexpr uint32_t population = 1100;
    ShadowViewFixture fixture(population);
    fixture.source->objects[0].primitives.resize(4);
    fixture.prepared.objects[0].primitives.resize(4);
    auto& in = fixture.inputs;
    in.geometry_shadow_metadata = true;
    in.geometry_shadows_in_view = true;
    in.draws = &fixture.many;
    for (uint32_t index = 0; index < population; ++index) {
        auto& unit = fixture.units[index];
        unit.position = {
            (fixture.view_origin + 16 + int32_t(index % 16)) * fixture.fixed,
            0,
            (fixture.view_origin + 16 + int32_t(index / 16 % 16)) * fixture.fixed
        };
    }
    // Serial records on either side of the parallel interval retain their
    // ordinary opacity and consume the same shadow depth slots.
    fixture.units.front().flags |= OA_UNIT_FLAG_BUILDING;
    fixture.units.back().state_flags |= oa::present::model::unit_state_cloaked;
    const auto canonical = fixture.units;
    oa::platform::job_pool::Pool pool(4);
    live::Scene serial, parallel;
    std::string error;
    const auto compare = [&](bool expect_parallel) {
        in.pool = nullptr;
        OA_CHECK(live::build(in, serial, error));
        in.pool = &pool;
        OA_CHECK(live::build(in, parallel, error));
        OA_CHECK(serial.model_count == parallel.model_count);
        const auto& serial_work = serial.model_work_counts;
        const auto& work = parallel.model_work_counts;
        OA_CHECK(serial_work.candidate_bands == 0 && serial_work.candidate_slots == 0);
        OA_CHECK(serial_work.accepted_bands == 0 && serial_work.accepted_slots == 0);
        OA_CHECK(serial_work.fallback_frames == 0);
        if (expect_parallel) {
            OA_CHECK(work.candidate_bands == 4 && work.accepted_bands == 4);
            OA_CHECK(work.candidate_slots == population - 2);
            OA_CHECK(work.accepted_slots == work.candidate_slots);
            OA_CHECK(work.fallback_frames == 0);
        } else {
            OA_CHECK(work.accepted_bands == 0 && work.accepted_slots == 0);
            OA_CHECK(work.fallback_frames == (work.candidate_bands != 0 ? 1U : 0U));
        }
        OA_CHECK(serial.texture_rgba == parallel.texture_rgba);
        OA_CHECK(serial.terrain_vertex_count == parallel.terrain_vertex_count);
        const auto same_range = [](const auto& a, const auto& b) {
            return a.unit_id == b.unit_id && a.first_vertex == b.first_vertex &&
                   a.vertex_count == b.vertex_count;
        };
        OA_CHECK(
            std::equal(
                serial.unit_ranges.begin(),
                serial.unit_ranges.end(),
                parallel.unit_ranges.begin(),
                parallel.unit_ranges.end(),
                same_range
            )
        );
        OA_CHECK(
            std::equal(
                serial.translucent_ranges.begin(),
                serial.translucent_ranges.end(),
                parallel.translucent_ranges.begin(),
                parallel.translucent_ranges.end(),
                same_range
            )
        );
        OA_CHECK(serial.translucent_vertices.size() == parallel.translucent_vertices.size());
        if (serial.translucent_vertices.size() == parallel.translucent_vertices.size() &&
            !serial.translucent_vertices.empty())
            OA_CHECK(
                std::memcmp(
                    serial.translucent_vertices.data(),
                    parallel.translucent_vertices.data(),
                    serial.translucent_vertices.size() * sizeof(serial.translucent_vertices[0])
                ) == 0
            );
        const auto expected = card_frame(serial), actual = card_frame(parallel);
        OA_CHECK(expected.units == actual.units);
        OA_CHECK(expected.vertices.size() == actual.vertices.size());
        if (expected.vertices.size() == actual.vertices.size() && !expected.vertices.empty())
            OA_CHECK(
                std::memcmp(
                    expected.vertices.data(),
                    actual.vertices.data(),
                    expected.vertices.size() * sizeof(expected.vertices[0])
                ) == 0
            );
        OA_CHECK(serial.cards.units.size() == parallel.cards.units.size());
        for (std::size_t index = 0;
             index < std::min(serial.cards.units.size(), parallel.cards.units.size());
             ++index) {
            const auto& a = serial.cards.units[index];
            const auto& b = parallel.cards.units[index];
            OA_CHECK(a.unit_id == b.unit_id && a.type == b.type && a.index_count == b.index_count);
            OA_CHECK(
                a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom &&
                a.bounded == b.bounded && a.body_lod == b.body_lod && a.marker == b.marker
            );
        }
        OA_CHECK(serial.cards.casts.size() == parallel.cards.casts.size());
        for (std::size_t index = 0;
             index < std::min(serial.cards.casts.size(), parallel.cards.casts.size());
             ++index) {
            const auto& a = serial.cards.casts[index];
            const auto& b = parallel.cards.casts[index];
            OA_CHECK(a.depth == b.depth && a.kind == b.kind && a.light == b.light);
            const auto& left = serial.cards.instances[a.instance];
            const auto& right = parallel.cards.instances[b.instance];
            OA_CHECK(left.origin == right.origin && left.position_count == right.position_count);
        }
        OA_CHECK(serial.cards.vertices == parallel.cards.vertices);
        // Every worker remembers its own posed range. A warm all-shared
        // model therefore proves the packet bands actually ran.
        if (expect_parallel)
            OA_CHECK(parallel.cards.positions.size() > serial.cards.positions.size());
        else
            OA_CHECK(parallel.cards.positions.size() == serial.cards.positions.size());
    };
    compare(false); // Missing materials retain the exact serial retry.
    compare(true);
    fixture.palette[2 * oa::palette_entry_bytes] ^= uint8_t{17};
    compare(false);
    compare(true);
    fixture.instance.pieces()[0].translation.x = fixture.fixed / 3;
    fixture.instance.rebuild_transforms();
    compare(true);
    in.geometry_shadow_metadata = false;
    compare(false);
    in.geometry_shadow_metadata = true;
    compare(true);
    const int rounding = std::fegetround();
    if (rounding != -1 && std::fesetround(FE_UPWARD) == 0) {
        compare(true);
        OA_CHECK(std::fegetround() == FE_UPWARD);
        OA_CHECK(std::fesetround(rounding) == 0);
    }
    // A false-linked draw is an original no-op even with no model references.
    // Interleave enough of these to break every consecutive drawn run,
    // while retaining more than 512 ordinary eligible units overall.
    const auto complete = fixture.many.models;
    for (std::size_t index = 2; index + 1 < fixture.many.models.size(); index += 2) {
        auto& draw = fixture.many.models[index];
        draw.plan.linked.drawn = false;
        draw.model = {};
    }
    compare(true);
    fixture.many.models = complete;
    compare(true);
    OA_CHECK(
        std::memcmp(
            canonical.data(), fixture.units.data(), canonical.size() * sizeof(canonical[0])
        ) == 0
    );
}

/// Selects local Enhanced frames independently of the presence of a playout object.
void geometry_shadow_frame_policy() {
    using oa::app::local_geometry_shadow_frame;
    oa::Game game{};
    game.players[0].in_use = 1;
    game.players[0].status = OA_PLAYER_STATUS_LOCAL;
    OA_CHECK(!local_geometry_shadow_frame(game, false, false));
    OA_CHECK(!local_geometry_shadow_frame(game, true, true));
    OA_CHECK(local_geometry_shadow_frame(game, true, false));
    auto& remote = game.players[1];
    remote.in_use = 1;
    remote.status = OA_PLAYER_STATUS_MIRRORED;
    OA_CHECK(!local_geometry_shadow_frame(game, true, false));
    remote.in_use = 0;
    OA_CHECK(local_geometry_shadow_frame(game, true, false));
}

/// Reserves and emits only shadows that can reach the actual shadow map.
void ground_shadows_in_view() {
    constexpr uint32_t population = 20000;
    ShadowViewFixture fixture(population);
    auto& inputs = fixture.inputs;
    std::string error;
    live::Scene expected;
    inputs.draws = &fixture.few;
    OA_CHECK(live::build(inputs, expected, error));
    OA_CHECK(expected.shadow_count == 1 && expected.cards.casts.size() == 1);
    inputs.draws = &fixture.many;
    live::Scene outside;
    OA_CHECK(!live::build(inputs, outside, error));
    OA_CHECK(outside.out_of_room);
    OA_CHECK(error.find("geometry budget") != std::string::npos);
    inputs.geometry_shadows_in_view = true;
    live::Scene actual;
    OA_CHECK(live::build(inputs, actual, error));
    OA_CHECK(actual.unit_detail_active && actual.unit_detail.size() == population);
    OA_CHECK(actual.shadow_count == 1 && actual.cards.casts.size() == 1);
    OA_CHECK(actual.cards.instances.size() == 1);
    OA_CHECK(actual.unit_detail[0].shadow_outside == false);
    for (std::size_t index = 1; index < actual.unit_detail.size(); ++index) {
        OA_CHECK(actual.unit_detail[index].shadow_outside);
        OA_CHECK(actual.unit_detail[index].surface == 0 && actual.unit_detail[index].shadow == 0);
    }
    OA_CHECK(actual.cards.positions == expected.cards.positions);
    OA_CHECK(actual.cards.indices == expected.cards.indices);
    OA_CHECK(actual.cards.cast_indices == expected.cards.cast_indices);
    OA_CHECK(actual.texture_rgba == expected.texture_rgba);
    OA_CHECK(actual.vertices.size() == expected.vertices.size());
    if (actual.vertices.size() == expected.vertices.size())
        OA_CHECK(
            std::memcmp(
                actual.vertices.data(),
                expected.vertices.data(),
                actual.vertices.size() * sizeof(actual.vertices[0])
            ) == 0
        );
    OA_CHECK(actual.cards.units.size() == 1 && expected.cards.units.size() == 1);
    if (actual.cards.units.size() == 1 && expected.cards.units.size() == 1) {
        const auto& a = actual.cards.units[0];
        const auto& b = expected.cards.units[0];
        OA_CHECK(a.unit_id == fixture.units[0].id && a.unit_id == b.unit_id);
        OA_CHECK(
            a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom &&
            a.bounded == b.bounded && a.body_lod == b.body_lod
        );
    }
    // Complex casters retain their original path, even wholly outside.
    fixture.few.models.push_back(fixture.many.models[1]);
    inputs.draws = &fixture.few;
    oa::UnitDef complex_definition{};
    fixture.few.models[1].model.def = &complex_definition;
    for (const uint32_t flags :
         {uint32_t(OA_UNIT_DEF_FLAG_CAN_FLY), uint32_t(OA_UNIT_DEF_FLAG_DIGGER)}) {
        complex_definition.flags = flags;
        OA_CHECK(live::build(inputs, actual, error));
        OA_CHECK(actual.unit_detail.size() == 2);
        if (actual.unit_detail.size() == 2)
            OA_CHECK(!actual.unit_detail[1].shadow_outside);
        OA_CHECK(actual.cards.casts.size() == 2);
    }
    complex_definition.flags = 0;
    fixture.units[1].flags = OA_UNIT_FLAG_BUILDING;
    fixture.state.shadow = oa::present::create_sprite(16, 16);
    OA_CHECK(live::build(inputs, actual, error));
    OA_CHECK(actual.unit_detail.size() == 2);
    if (actual.unit_detail.size() == 2)
        OA_CHECK(!actual.unit_detail[1].shadow_outside);
    OA_CHECK(actual.cards.casts.size() == 3);
}

/// Omitted-only frames still validate their current producer with shadows off.
void preplanned_omission_validation() {
    namespace models = oa::formats::objects3d;
    namespace runtime = oa::sim::model_runtime;
    ShadowViewFixture fixture(1);
    auto& inputs = fixture.inputs;
    auto& draw = fixture.many.models[0];
    fixture.units[0].position = {1024 * fixture.fixed, 0, 1024 * fixture.fixed};
    fixture.many.shadow_level = 0;
    inputs.graphics_flags = 0;
    inputs.geometry_shadow_metadata = inputs.geometry_shadows_in_view = true;
    std::string error;
    live::Scene ordinary;
    OA_CHECK(live::build(inputs, ordinary, error));
    OA_CHECK(ordinary.cards.instances.empty() && ordinary.cards.units.empty());
    OA_CHECK(live::cached_card_type(ordinary.card_types, fixture.instance, fixture.prepared));
    // This is the caller's record shape: retain references and list position,
    // but no linked plan, transformed snapshot or deferred-image requirement.
    draw.plan = {};
    draw.shown_position = fixture.units[0].position;
    draw.preplan_omitted = true;
    OA_CHECK(!fixture.state.deferred_image);
    live::Scene omitted;
    OA_CHECK(live::build(inputs, omitted, error));
    OA_CHECK(omitted.cards.instances.empty() && omitted.cards.units.empty());
    OA_CHECK(omitted.cards.casts.empty() && omitted.cards.markers.empty());
    OA_CHECK(omitted.unit_ranges.empty() && omitted.model_count == 0);
    OA_CHECK(same_on_card(omitted, ordinary, inputs));
    OA_CHECK(omitted.card_types.draw_types.size() == 1);
    if (omitted.card_types.draw_types.size() == 1)
        OA_CHECK(omitted.card_types.draw_types[0] >= 0);
    // A retained proof is insufficient after the ordinary registry trim.
    // An omitted record must recreate its supported type in the same build.
    const auto revision = omitted.card_types.revision;
    omitted.card_types.records.resize((live::card_records_kept + 1) * 2);
    OA_CHECK(live::build(inputs, omitted, error));
    OA_CHECK(omitted.card_types.revision != revision);
    OA_CHECK(live::cached_card_type(omitted.card_types, fixture.instance, fixture.prepared));
    OA_CHECK(omitted.card_types.records.size() / 2 <= live::card_records_kept);
    OA_CHECK(same_on_card(omitted, ordinary, inputs));
    const auto refuses_omission = [&] {
        error.clear();
        OA_CHECK(!live::build(inputs, omitted, error));
        OA_CHECK(!omitted.out_of_room);
        OA_CHECK(error.find("preplanned model needs its ordinary plan") != std::string::npos);
    };
    // These are Scene-level controls: no Runtime producer guard can mask an
    // accidental early return in the shadow/deferred-image verifier.
    inputs.models_on_card = false;
    refuses_omission();
    inputs.models_on_card = true;
    inputs.geometry_shadow_metadata = false;
    refuses_omission();
    inputs.geometry_shadow_metadata = true;
    inputs.geometry_shadows_in_view = false;
    refuses_omission();
    inputs.geometry_shadows_in_view = true;
    OA_CHECK(live::build(inputs, omitted, error));
    // A different, valid native model can exceed the GPU type's vertex
    // field. It must be refused after trimming, despite the prior proof.
    auto unsupported_source = std::make_shared<models::Model>(*fixture.source);
    unsupported_source->objects[0].vertices.resize(
        (std::size_t{1} << oa::app::renderer_live_gpu::model_piece_shift) + 1
    );
    auto unsupported = runtime::make_instance(unsupported_source);
    unsupported.rebuild_transforms();
    draw.model.instance = &unsupported;
    omitted.card_types.records.resize((live::card_records_kept + 1) * 2);
    refuses_omission();
    OA_CHECK(omitted.card_types.draw_types.size() == 1);
    if (omitted.card_types.draw_types.size() == 1)
        OA_CHECK(omitted.card_types.draw_types[0] < 0);
    // Untagged false-linked records keep their ordinary harmless behavior.
    draw.preplan_omitted = false;
    OA_CHECK(live::build(inputs, omitted, error));
    OA_CHECK(same_on_card(omitted, ordinary, inputs));
    draw.model.instance = &fixture.instance;
    draw.preplan_omitted = true;
    OA_CHECK(live::build(inputs, omitted, error));
    OA_CHECK(same_on_card(omitted, ordinary, inputs));
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
    // A completed ground unit above the sea can retain the image's frame
    // while the card casts its shadow from the same cached pieces.
    in.sea_level = 0;
    live::Scene pixels_shadow;
    live::Scene deferred_shadow;
    OA_CHECK(live::build(in, pixels_shadow, battle.error));
    auto& vehicle_draw = battle.draws.models[vehicle];
    auto& vehicle_state = states[vehicle];
    auto retained_image = std::move(vehicle_state.image);
    vehicle_state.image = {};
    vehicle_state.deferred_image = drawing::ImageFrame{
        retained_image.sprite.width,
        retained_image.sprite.height,
        retained_image.sprite.origin_x,
        retained_image.sprite.origin_y
    };
    vehicle_state.deferred_image_depth = retained_image.sprite.aux != nullptr;
    in.geometry_shadow_metadata = true;
    OA_CHECK(live::build(in, deferred_shadow, battle.error));
    OA_CHECK(vehicle_state.image.sprite.data == nullptr);
    OA_CHECK(vehicle_state.image.pixels.empty());
    OA_CHECK(pixels_shadow.shadow_count != 0);
    OA_CHECK(pixels_shadow.shadow_count == deferred_shadow.shadow_count);
    OA_CHECK(pixels_shadow.vertices.size() == deferred_shadow.vertices.size());
    if (pixels_shadow.vertices.size() == deferred_shadow.vertices.size())
        OA_CHECK(same(
            pixels_shadow.vertices.data(),
            deferred_shadow.vertices.data(),
            pixels_shadow.vertices.size()
        ));
    OA_CHECK(pixels_shadow.texture_rgba == deferred_shadow.texture_rgba);
    OA_CHECK(pixels_shadow.cards.positions == deferred_shadow.cards.positions);
    OA_CHECK(pixels_shadow.cards.indices == deferred_shadow.cards.indices);
    OA_CHECK(pixels_shadow.cards.piece_frames == deferred_shadow.cards.piece_frames);
    OA_CHECK(pixels_shadow.cards.textures == deferred_shadow.cards.textures);
    OA_CHECK(pixels_shadow.cards.cast_indices == deferred_shadow.cards.cast_indices);
    OA_CHECK(pixels_shadow.cards.cast_depths == deferred_shadow.cards.cast_depths);
    OA_CHECK(
        std::equal(
            pixels_shadow.cards.casts.begin(),
            pixels_shadow.cards.casts.end(),
            deferred_shadow.cards.casts.begin(),
            deferred_shadow.cards.casts.end(),
            [](const auto& a, const auto& b) {
                return a.instance == b.instance && a.kind == b.kind && a.base == b.base &&
                       a.ground_half == b.ground_half && a.least_height == b.least_height &&
                       a.light == b.light && a.depth == b.depth && a.across == b.across &&
                       a.down == b.down;
            }
        )
    );
    OA_CHECK(
        std::equal(
            pixels_shadow.cards.cast_runs.begin(),
            pixels_shadow.cards.cast_runs.end(),
            deferred_shadow.cards.cast_runs.begin(),
            deferred_shadow.cards.cast_runs.end(),
            [](const auto& a, const auto& b) {
                return a.vertex == b.vertex && a.first_index == b.first_index &&
                       a.index_count == b.index_count && a.first_instance == b.first_instance;
            }
        )
    );
    // Picking consumes the same emitted faces and bounds; shadows retain
    // their original position among those faces.
    OA_CHECK(
        std::equal(
            pixels_shadow.cards.units.begin(),
            pixels_shadow.cards.units.end(),
            deferred_shadow.cards.units.begin(),
            deferred_shadow.cards.units.end(),
            [](const live::CardUnit& a, const live::CardUnit& b) {
                return a.unit_id == b.unit_id && a.vertex == b.vertex && a.instance == b.instance &&
                       a.type == b.type && a.first_index == b.first_index &&
                       a.index_count == b.index_count && a.left == b.left && a.top == b.top &&
                       a.right == b.right && a.bottom == b.bottom && a.bounded == b.bounded &&
                       a.body_lod == b.body_lod;
            }
        )
    );
    // The drawn interpolation can be below the sea while the canonical
    // endpoint is above it. Eligibility follows the actual shown position.
    vehicle_draw.shown_position = units[vehicle].position;
    vehicle_draw.shown_position->y = -fixed;
    OA_CHECK(units[vehicle].position.y >= 0);
    OA_CHECK(!live::build(in, deferred_shadow, battle.error));
    OA_CHECK(!battle.error.empty());
    vehicle_draw.shown_position.reset();
    definitions[vehicle].flags = OA_UNIT_DEF_FLAG_CAN_HOVER;
    OA_CHECK(!live::build(in, deferred_shadow, battle.error));
    definitions[vehicle].flags = 0;
    units[vehicle].build_remaining = 0.5F;
    OA_CHECK(!live::build(in, deferred_shadow, battle.error));
    units[vehicle].build_remaining = 0;
    units[vehicle].attach_parent = 1;
    OA_CHECK(!live::build(in, deferred_shadow, battle.error));
    units[vehicle].attach_parent = 0;
    units[vehicle].attach_first_child = 2;
    OA_CHECK(!live::build(in, deferred_shadow, battle.error));
    units[vehicle].attach_first_child = 0;
    // Removing the current GPU producer must refuse the deferred shadow
    // rather than quietly losing it.
    in.models_on_card = false;
    OA_CHECK(!live::build(in, deferred_shadow, battle.error));
    OA_CHECK(!battle.error.empty());
    in.models_on_card = true;
    // An otherwise valid model larger than a GPU type's vertex field is
    // still drawable through the ordinary path, but cannot borrow support.
    auto unsupported_source = std::make_shared<models::Model>(*source);
    unsupported_source->objects[0].vertices.resize(
        (std::size_t{1} << oa::app::renderer_live_gpu::model_piece_shift) + 1
    );
    auto unsupported = runtime::make_instance(unsupported_source);
    unsupported.rebuild_transforms();
    // The planner's retained-type proof reads a warmed registry without
    // creating types, borrowing another model's ownership or allocating.
    live::CardTypes cold_types;
    live::CardTypes unsupported_types;
    OA_CHECK(live::card_type(unsupported_types, unsupported, prepared) == -1);
    auto different_prepared = prepared;
    const std::shared_ptr<const models::Model> borrowed_model(source.get(), [](const auto*) {});
    const auto borrowed = runtime::make_instance(borrowed_model);
    auto reordered = runtime::make_instance(source);
    std::swap(reordered.pieces()[0].object_index, reordered.pieces()[1].object_index);
    const auto& retained = pixels_shadow.card_types;
    const auto original_records = retained.records;
    const auto original_models = retained.by_model;
    const auto original_types = retained.types.size();
    const auto original_revision = retained.revision;
    const auto unsupported_records = unsupported_types.records;
    const auto unsupported_models = unsupported_types.by_model;
    const auto unsupported_type_count = unsupported_types.types.size();
    const auto unsupported_revision = unsupported_types.revision;
    std::array<bool, 6> supported{};
    ground_allocations = refused_ground_allocations = 0;
    ground_allocation_to_refuse = std::numeric_limits<std::size_t>::max();
    watch_ground_allocations = true;
    supported[0] = live::cached_card_type(cold_types, instances[vehicle], prepared);
    supported[1] = live::cached_card_type(retained, instances[vehicle], prepared);
    supported[2] = live::cached_card_type(retained, instances[vehicle], different_prepared);
    supported[3] = live::cached_card_type(retained, borrowed, prepared);
    supported[4] = live::cached_card_type(retained, reordered, prepared);
    supported[5] = live::cached_card_type(unsupported_types, unsupported, prepared);
    watch_ground_allocations = false;
    OA_CHECK(ground_allocations == 0 && refused_ground_allocations == 0);
    OA_CHECK((supported == std::array<bool, 6>{false, true, false, false, false, false}));
    OA_CHECK(cold_types.types.empty() && cold_types.records.empty() && cold_types.by_model.empty());
    OA_CHECK(cold_types.revision == 0);
    OA_CHECK(retained.records == original_records && retained.by_model == original_models);
    OA_CHECK(retained.types.size() == original_types && retained.revision == original_revision);
    OA_CHECK(unsupported_types.records == unsupported_records);
    OA_CHECK(unsupported_types.by_model == unsupported_models);
    OA_CHECK(unsupported_types.types.size() == unsupported_type_count);
    OA_CHECK(unsupported_types.revision == unsupported_revision);
    vehicle_draw.model.instance = &unsupported;
    OA_CHECK(!live::build(in, deferred_shadow, battle.error));
    OA_CHECK(!battle.error.empty());
    // The existing metadata path needs no GPU shadow producer when all
    // shadows are disabled, including for an unsupported model type.
    const auto shadow_flags = in.graphics_flags;
    in.graphics_flags = 0;
    OA_CHECK(live::build(in, deferred_shadow, battle.error));
    OA_CHECK(deferred_shadow.cards.casts.empty() && deferred_shadow.shadow_count == 0);
    in.graphics_flags = shadow_flags;
    vehicle_draw.model.instance = &instances[vehicle];
    vehicle_state.image = std::move(retained_image);
    vehicle_state.deferred_image.reset();
    vehicle_state.deferred_image_depth = false;
    // Each excluded class keeps its pixel-backed path. A deferred image
    // with such a class is refused before any shadow reader runs.
    for (const auto kind : {building, digger, aircraft, floater, under_water}) {
        in.sea_level = kind == under_water ? 30 : 0;
        auto& excluded = battle.draws.models[kind];
        const auto prior_flags = definitions[kind].flags;
        if (kind == aircraft)
            definitions[kind].flags = OA_UNIT_DEF_FLAG_CAN_FLY;
        OA_CHECK(live::build(in, pixels_shadow, battle.error));
        auto& excluded_state = *excluded.model.state;
        auto pixels = std::move(excluded_state.image);
        excluded_state.image = {};
        excluded_state.deferred_image = drawing::ImageFrame{
            pixels.sprite.width,
            pixels.sprite.height,
            pixels.sprite.origin_x,
            pixels.sprite.origin_y
        };
        excluded_state.deferred_image_depth = pixels.sprite.aux != nullptr;
        OA_CHECK(!live::build(in, deferred_shadow, battle.error));
        OA_CHECK(!battle.error.empty());
        excluded_state.image = std::move(pixels);
        excluded_state.deferred_image.reset();
        excluded_state.deferred_image_depth = false;
        definitions[kind].flags = prior_flags;
    }
    in.sea_level = 0;
    in.geometry_shadow_metadata = false;
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
    draw.plan.linked.drawn = true;
    drawing::ModelState state;
    state.image = oa::present::create_sprite(8, 8);
    draw.model.state = &state;
    draw.plan.model.from_image = true;
    for (auto& primitive : prepared.objects[0].primitives)
        primitive.color = alternate_index;
    check_deferred_image_draw(fixture.inputs, state);
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
    check_deferred_image_draw(fixture.inputs, state);
    OA_CHECK(!fixture.scene.unit_ranges.empty());
    if (fixture.scene.unit_ranges.empty())
        return;
    const auto first = fixture.scene.unit_ranges[0].first_vertex;
    OA_CHECK(triangle_color(fixture.scene, first)[0] == 165);
    OA_CHECK(std::abs(fixture.scene.vertices[first].brightness - 20 * 0.06875F / 2) < tolerance);
    // Camera orientation changes visibility, never the baked model light direction.
    fixture.inputs.view_direction = {0, 0, 1};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    check_deferred_image_draw(fixture.inputs, state);
    OA_CHECK(std::abs(fixture.scene.vertices[first].brightness - 20 * 0.06875F / 2) < tolerance);
    fixture.inputs.view_direction = {};
    unit.flags = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    check_deferred_image_draw(fixture.inputs, state);
    OA_CHECK(triangle_color(fixture.scene, first)[0] == 120);
    OA_CHECK(fixture.scene.vertices[first].brightness == 1);
    unit.flags = OA_UNIT_FLAG_BUILDING;
    fixture.inputs.graphics_flags = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    check_deferred_image_draw(fixture.inputs, state);
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
    check_deferred_image_draw(fixture.inputs, state);
    const auto& vertex = fixture.scene.vertices[first];
    OA_CHECK(triangle_color(fixture.scene, first)[0] == 30);
    OA_CHECK(std::abs(vertex.red - 30 / 255.0F) < tolerance);
    OA_CHECK(std::abs(vertex.green - 120 / 255.0F) < tolerance);
    OA_CHECK(std::abs(vertex.blue - 240 / 255.0F) < tolerance);
    // Standalone callers without a complete classic lighting bundle keep source art.
    fixture.inputs.shade_table = {};
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    check_deferred_image_draw(fixture.inputs, state);
    OA_CHECK(triangle_color(fixture.scene, first)[0] == ground_color[0]);
    fixture.inputs.shade_table = shades;
    fixture.inputs.light_scale = 0;
    prepared.objects[0].primitives[0].flags = 0;
    OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
    check_deferred_image_draw(fixture.inputs, state);
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
    // An explosion's flash and sprite stay where they went off, but take no
    // depth, so the water leaves their colours as the game draws them.
    sprite.explosion = true;
    for (const auto kind : {oa::app::WorldDrawKind::lit_sprite, oa::app::WorldDrawKind::sprite}) {
        fixture.draws.draws[0].kind = kind;
        OA_CHECK(live::build(fixture.inputs, fixture.scene, fixture.error));
        OA_CHECK(fixture.scene.sprite_count == 1);
        float lowest = 100, highest = -100;
        for (std::size_t i = fixture.scene.terrain_vertex_count; i < fixture.scene.vertices.size();
             ++i) {
            const auto& vertex = fixture.scene.vertices[i];
            lowest = std::min(lowest, vertex.height);
            highest = std::max(highest, vertex.height);
            OA_CHECK(vertex.terrain_size == -5 && vertex.terrain_x == 0);
        }
        OA_CHECK(highest < 16);
        OA_CHECK(std::abs((lowest + highest) / 2 - 4) < tolerance);
    }
    sprite.explosion = false;
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

#include "parallel_model_cards_test.inc"
#include "body_lod_geometry_test.inc"
#include "body_lod_cards_test.inc"
#include "body_lod_scene_test.inc"
#include "body_lod_shadow_test.inc"
#include "caster_index_runs_test.inc"
#include "caster_index_scene_test.inc"
#include "interior_model_cull_test.inc"
#include "body_detail_pose_test.inc"
#include "shadow_index_templates_test.inc"
#include "shadow_index_procedural_scene_test.inc"
#include "physical_model_multirange_test.inc"
#include "omitted_model_snapshots_test.inc"

namespace {

/// Keeps an owned arbitrary-heading body recipe after its canonical buffers go cold.
struct RootPrefixSceneFixture : BodyLodFixture {
    oa::UnitDef definition{};
    oa::app::RootPrefixRecipe recipe{};

    explicit RootPrefixSceneFixture(size_t pieces = 2, int16_t angle = 7319)
        : BodyLodFixture(pieces) {
        namespace runtime = oa::sim::model_runtime;
        constexpr int32_t fixed = oa::app::root_prefix_fixed_scale;
        const auto visible = static_cast<uint16_t>(runtime::PieceFlag::visible);
        const auto cached = static_cast<uint16_t>(runtime::PieceFlag::cached);
        for (auto& piece : instance.pieces())
            piece.flags = visible | cached;
        instance.pieces().back().flags = visible;
        // A translated limb makes the containing box deliberately asymmetric.
        if (pieces == 2)
            instance.pieces()[1].translation.x = 19 * fixed;
        instance.rebuild_transforms({}, true);
        const auto placement = oa::app::root_prefix_placement(instance);
        const auto rotation = oa::app::prepare_root_prefix_rotation(angle);
        OA_CHECK(placement && rotation);
        if (!placement || !rotation)
            return;
        recipe.geometry = oa::app::make_root_prefix_geometry(
            instance, *placement, std::numeric_limits<size_t>::max()
        );
        OA_CHECK(recipe.geometry != nullptr);
        recipe.rotation = *rotation;
        recipe.placement = *placement;
        unit.type_index = 1;
        prepared.model = source.get();
        prepared.owner = source;
        auto& draw = battle.draws.models[0];
        draw.model.def = &definition;
        draw.root_prefix = recipe;
        draw.plan.model.from_image = false;
        battle.inputs.root_prefix_enabled = true;
        battle.inputs.nanolathe_lighting = false;
        battle.inputs.geometry_shadow_metadata = true;
        battle.inputs.geometry_shadows_in_view = true;
        state.image = {};
        state.shadow = {};
        state.deferred_image.reset();
        for (auto& piece : instance.pieces()) {
            piece.transformed_vertices.clear();
            piece.transformed_origin = {INT32_MAX, INT32_MIN, INT32_MAX};
        }
    }
};

/// Checks lazy corners against the owned source mapping for full and coarse body types.
void check_root_prefix_scene_corners(const RootPrefixSceneFixture& fixture) {
    const auto& scene = fixture.battle.scene;
    const auto& cards = scene.cards;
    constexpr uint32_t corner_mask = (uint32_t{1} << joined_gpu::model_corner_bits) - 1;
    constexpr uint32_t point_mask = (uint32_t{1} << joined_gpu::model_piece_shift) - 1;
    constexpr float fixed = oa::app::root_prefix_fixed_scale;
    for (const auto& unit : cards.units) {
        OA_CHECK(unit.root_prefix && !unit.marker);
        const auto& instance = cards.instances[unit.instance];
        const auto& type = scene.card_types.types[unit.type];
        OA_CHECK(instance.root_prefix != 0 && instance.positions == 0);
        for (size_t at = unit.first_index; at < size_t{unit.first_index} + unit.index_count; ++at) {
            const auto index = cards.indices[at];
            const auto record = (size_t{instance.corners} + (index & corner_mask)) * 2;
            const auto place = scene.card_types.records[record];
            const uint32_t piece = place >> joined_gpu::model_piece_shift;
            const uint32_t point = place & point_mask;
            const uint32_t source =
                type.body_detail ? type.source_vertices[point] : point - type.piece_vertices[piece];
            const auto local = oa::app::root_prefix_point(fixture.recipe, piece, source);
            const std::array expected{
                instance.origin[0] + local.x / fixed,
                instance.origin[1] + local.y / fixed,
                instance.origin[2] + -(local.z / fixed)
            };
            const auto corner = live::card_corner(scene.card_types, cards, unit, index);
            OA_CHECK(std::bit_cast<uint32_t>(corner.x) == std::bit_cast<uint32_t>(expected[0]));
            OA_CHECK(
                std::bit_cast<uint32_t>(corner.height) == std::bit_cast<uint32_t>(expected[1])
            );
            OA_CHECK(std::bit_cast<uint32_t>(corner.z) == std::bit_cast<uint32_t>(expected[2]));
        }
    }
}

/// Draws arbitrary headings, physical limbs and geometry shadows without canonical vertices.
void root_prefix_scene_owned_geometry() {
    constexpr auto moving_piece = uint64_t{1} << 7;
    RootPrefixSceneFixture fixture(8);
    auto& battle = fixture.battle;
    battle.inputs.detail_scale = 0.5F;
    const auto revision = fixture.instance.materialization_revision();
    const auto record = fixture.unit;
    OA_CHECK(live::root_prefix_supported(battle.inputs, battle.draws.models[0], fixture.recipe));
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.error.empty());
    OA_CHECK(battle.scene.cards.positions.empty());
    OA_CHECK(!battle.scene.cards.root_prefix_points.empty());
    OA_CHECK(!battle.scene.cards.root_prefix_poses.empty());
    OA_CHECK(
        battle.scene.cards.root_prefix_poses.size() == battle.scene.cards.root_prefix_recipes.size()
    );
    OA_CHECK(!battle.scene.cards.units.empty() && !battle.scene.cards.casts.empty());
    OA_CHECK(battle.scene.detail_poses.empty() && battle.scene.detail_pose_points.empty());
    OA_CHECK(battle.scene.unit_detail.size() == 1);
    if (battle.scene.unit_detail.size() == 1) {
        const auto& detail = battle.scene.unit_detail[0];
        OA_CHECK(detail.body_available && detail.body_lod == 1);
        OA_CHECK((detail.body_piece_mask & moving_piece) != 0);
    }
    check_root_prefix_scene_corners(fixture);
    OA_CHECK(fixture.instance.materialization_revision() == revision);
    OA_CHECK(std::memcmp(&fixture.unit, &record, sizeof(record)) == 0);
    OA_CHECK(fixture.state.image.pixels.empty() && fixture.state.shadow.pixels.empty());
    OA_CHECK(!fixture.state.deferred_image);
    for (const auto& piece : fixture.instance.pieces()) {
        OA_CHECK(piece.transformed_vertices.empty());
        OA_CHECK(
            piece.transformed_origin.x == INT32_MAX && piece.transformed_origin.y == INT32_MIN
        );
    }
}

/// Keeps proxy endpoints integer and centred exactly on the displayed anchor.
void root_prefix_scene_proxy_symmetry() {
    RootPrefixSceneFixture fixture;
    auto& battle = fixture.battle;
    constexpr std::array<std::array<float, 3>, 3> cameras{
        {{0.43F, 31.37F, 33.81F}, {0.79F, 29.14F, 34.27F}, {1.15F, 27.91F, 35.63F}}
    };
    for (const auto& camera : cameras) {
        fixture.projection(camera[0]);
        battle.inputs.clip_projection[3] = camera[1];
        battle.inputs.clip_projection[7] = camera[2];
        OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
        OA_CHECK(battle.scene.cards.units.size() == 1);
        if (battle.scene.cards.units.size() != 1)
            continue;
        const auto& drawn = battle.scene.cards.units[0];
        OA_CHECK(drawn.root_prefix && drawn.index_count != 0);
        OA_CHECK(std::floor(drawn.left) == drawn.left && std::floor(drawn.top) == drawn.top);
        OA_CHECK(
            std::floor(drawn.right) == drawn.right && std::floor(drawn.bottom) == drawn.bottom
        );
        OA_CHECK(drawn.left + (drawn.right - drawn.left) * 0.5F == drawn.displayed_anchor[0]);
        OA_CHECK(drawn.top + (drawn.bottom - drawn.top) * 0.5F == drawn.displayed_anchor[1]);
        const auto& projection = battle.inputs.clip_projection;
        constexpr float fixed = oa::app::root_prefix_fixed_scale;
        const auto& point = fixture.unit.position;
        const std::array expected{
            std::round(
                projection[0] * (point.x / fixed) + projection[1] * (point.y / fixed) +
                projection[2] * (point.z / fixed) + projection[3]
            ),
            std::round(
                projection[4] * (point.x / fixed) + projection[5] * (point.y / fixed) +
                projection[6] * (point.z / fixed) + projection[7]
            )
        };
        OA_CHECK(drawn.displayed_anchor == expected);
        check_root_prefix_scene_corners(fixture);
    }
}

/// Leaves clear image-key geometry without a fabricated body or owner marker.
void root_prefix_scene_clear_geometry() {
    RootPrefixSceneFixture fixture;
    fixture.projection(0.05F);
    fixture.battle.inputs.graphics_flags = 0;
    for (auto& object : fixture.prepared.objects)
        for (auto& primitive : object.primitives)
            primitive.color = oa::present::model::image_key;
    OA_CHECK(live::build(fixture.battle.inputs, fixture.battle.scene, fixture.battle.error));
    const auto& cards = fixture.battle.scene.cards;
    OA_CHECK(cards.markers.empty() && cards.vertices == 0);
    for (const auto& unit : cards.units)
        OA_CHECK(unit.index_count == 0);
    OA_CHECK(fixture.battle.scene.body_lod_counts.distance_markers == 0);
    OA_CHECK(fixture.battle.scene.body_lod_counts.full_mesh_models == 0);
    OA_CHECK(fixture.battle.scene.body_lod_counts.body_lod_models == 0);
}

/// Keeps cached and moving texture choices from the virtual image's native depth flag.
void root_prefix_scene_virtual_image_frames() {
    RootPrefixSceneFixture fixture;
    auto& battle = fixture.battle;
    battle.inputs.graphics_flags = 0;
    battle.draws.models[0].plan.linked.unlit = true;
    const auto check_frames = [&](uint32_t moving_frame) {
        OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
        OA_CHECK(battle.scene.cards.instances.size() == 1);
        if (battle.scene.cards.instances.size() != 1)
            return;
        const auto& instance = battle.scene.cards.instances[0];
        OA_CHECK(instance.root_prefix != 0 && instance.piece_count == 2);
        OA_CHECK(battle.scene.cards.piece_frames[instance.pieces] == 1);
        OA_CHECK(battle.scene.cards.piece_frames[instance.pieces + 1] == moving_frame);
        OA_CHECK(fixture.state.image.pixels.empty() && !fixture.state.deferred_image);
    };
    fixture.unit.flags2 = 0;
    check_frames(1);
    fixture.unit.flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    check_frames(0);
}

/// Refuses unsupported body policies without materializing or changing canonical state.
void root_prefix_scene_admission() {
    RootPrefixSceneFixture fixture;
    auto& battle = fixture.battle;
    auto& draw = battle.draws.models[0];
    const auto supported = [&](bool expected) {
        const auto record = fixture.unit;
        const auto revision = fixture.instance.materialization_revision();
        const auto dirty = fixture.instance.transforms_dirty();
        OA_CHECK(live::root_prefix_supported(battle.inputs, draw, fixture.recipe) == expected);
        OA_CHECK(std::memcmp(&fixture.unit, &record, sizeof(record)) == 0);
        OA_CHECK(fixture.instance.materialization_revision() == revision);
        OA_CHECK(fixture.instance.transforms_dirty() == dirty);
        for (const auto& piece : fixture.instance.pieces())
            OA_CHECK(piece.transformed_vertices.empty());
    };
    supported(true);
    // A foot below sea level on dry terrain does not make the robot a water unit.
    const auto placement = fixture.recipe.placement;
    fixture.recipe.placement.y -= 4 * oa::app::root_prefix_fixed_scale;
    draw.root_prefix = fixture.recipe;
    const auto foot = oa::app::root_prefix_piece_bounds(fixture.recipe, 1);
    OA_CHECK(foot && foot->low.y < 0);
    supported(true);
    fixture.recipe.placement = placement;
    draw.root_prefix = fixture.recipe;
    battle.inputs.root_prefix_enabled = false;
    supported(false);
    battle.inputs.root_prefix_enabled = true;
    fixture.unit.state_flags |= oa::present::model::unit_state_cloaked;
    supported(false);
    fixture.unit.state_flags &= ~oa::present::model::unit_state_cloaked;
    fixture.unit.attach_parent = 1;
    supported(false);
    fixture.unit.attach_parent = 0;
    fixture.unit.flags |= OA_UNIT_FLAG_BUILDING;
    supported(false);
    fixture.unit.flags &= ~OA_UNIT_FLAG_BUILDING;
    fixture.definition.flags |= OA_UNIT_DEF_FLAG_DIGGER;
    supported(false);
    fixture.definition.flags &= ~OA_UNIT_DEF_FLAG_DIGGER;
    battle.inputs.sea_level = 2;
    fixture.unit.position.y = 4 * oa::app::root_prefix_fixed_scale;
    supported(false);
    battle.inputs.sea_level = 0;
    fixture.unit.position.y = 0;
    battle.inputs.nanolathe_lighting = true;
    oa::app::SpatialParticle light;
    light.position = {
        fixture.unit.position.x, 8 * oa::app::root_prefix_fixed_scale, fixture.unit.position.z
    };
    light.emissive = true;
    battle.draws.spatial_particles.push_back(light);
    supported(false);
    battle.draws.spatial_particles[0].position.x = 256 * oa::app::root_prefix_fixed_scale;
    battle.draws.spatial_particles[0].position.z = 256 * oa::app::root_prefix_fixed_scale;
    supported(true);
}

/// Refuses incomplete terrain before a recipe can sample the height grid.
void root_prefix_scene_incomplete_terrain() {
    RootPrefixSceneFixture fixture;
    auto& battle = fixture.battle;
    const auto& draw = battle.draws.models[0];
    const auto record = fixture.unit;
    const auto revision = fixture.instance.materialization_revision();
    const auto attributes = battle.map.attributes;
    battle.map.attributes.clear();
    OA_CHECK(!live::root_prefix_supported(battle.inputs, draw, fixture.recipe));
    OA_CHECK(!live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.error.empty());
    OA_CHECK(std::memcmp(&fixture.unit, &record, sizeof(record)) == 0);
    OA_CHECK(fixture.instance.materialization_revision() == revision);
    for (const auto& piece : fixture.instance.pieces())
        OA_CHECK(piece.transformed_vertices.empty());
    battle.map.attributes = attributes;
    OA_CHECK(live::root_prefix_supported(battle.inputs, draw, fixture.recipe));
}

/// Matches GPU qualification of the complete raw box, including hidden pieces.
void root_prefix_scene_conservative_admission() {
    constexpr int32_t fixed = oa::app::root_prefix_fixed_scale;
    RootPrefixSceneFixture correlated(2, 8192);
    auto geometry = std::make_shared<oa::app::RootPrefixGeometry>(*correlated.recipe.geometry);
    // The individual boxes lie on the same diagonal, so rotating each keeps
    // its X extent small. The complete raw box also contains the opposite
    // corners, whose rotated X extent cannot fit this root placement.
    for (size_t piece = 0; piece < geometry->objects.size(); ++piece) {
        auto& bounds = geometry->piece_bounds[piece];
        const int32_t center = (piece == 0 ? 480 : -480) * fixed;
        const int32_t dx = center - (bounds.low.x + (bounds.high.x - bounds.low.x) / 2);
        const int32_t dz = center - (bounds.low.z + (bounds.high.z - bounds.low.z) / 2);
        for (auto point = geometry->piece_vertices[piece];
             point < geometry->piece_vertices[piece + 1];
             ++point) {
            geometry->points[point].x += dx;
            geometry->points[point].z += dz;
        }
        bounds.low.x += dx;
        bounds.high.x += dx;
        bounds.low.z += dz;
        bounds.high.z += dz;
    }
    correlated.recipe.geometry = geometry;
    correlated.recipe.placement.x = 2000 * fixed;
    correlated.battle.draws.models[0].root_prefix = correlated.recipe;
    for (uint32_t piece = 0; piece < geometry->objects.size(); ++piece)
        OA_CHECK(oa::app::root_prefix_piece_bounds(correlated.recipe, piece).has_value());
    const auto opposite_corner = oa::app::place_root_prefix_point(
        {geometry->piece_bounds[0].high.x, 0, geometry->piece_bounds[1].low.z},
        correlated.recipe.rotation,
        correlated.recipe.placement
    );
    OA_CHECK(opposite_corner.x > oa::app::root_prefix_local_limit);
    OA_CHECK(!live::root_prefix_supported(
        correlated.battle.inputs, correlated.battle.draws.models[0], correlated.recipe
    ));

    RootPrefixSceneFixture hidden(2, 0);
    geometry = std::make_shared<oa::app::RootPrefixGeometry>(*hidden.recipe.geometry);
    for (auto point = geometry->piece_vertices[1]; point < geometry->piece_vertices[2]; ++point)
        geometry->points[point].x += 480 * fixed;
    geometry->piece_bounds[1].low.x += 480 * fixed;
    geometry->piece_bounds[1].high.x += 480 * fixed;
    hidden.recipe.geometry = geometry;
    hidden.battle.draws.models[0].root_prefix = hidden.recipe;
    hidden.instance.pieces()[1].flags &=
        ~static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
    hidden.unit.position.x = 32760 * fixed;
    const auto visible_box = oa::app::root_prefix_piece_bounds(hidden.recipe, 0);
    const auto hidden_box = oa::app::root_prefix_piece_bounds(hidden.recipe, 1);
    OA_CHECK(visible_box && hidden_box);
    if (visible_box && hidden_box) {
        OA_CHECK(int64_t{hidden.unit.position.x} + visible_box->high.x < int64_t{32768} * fixed);
        OA_CHECK(int64_t{hidden.unit.position.x} + hidden_box->high.x > int64_t{32768} * fixed);
    }
    OA_CHECK(!live::root_prefix_supported(
        hidden.battle.inputs, hidden.battle.draws.models[0], hidden.recipe
    ));
}

/// Restores every packet range and memo when a material retry truncates a draw.
void root_prefix_scene_card_rollback() {
    RootPrefixSceneFixture fixture;
    live::CardTypes types;
    const auto type_index = live::card_type(types, fixture.instance, fixture.prepared);
    OA_CHECK(type_index >= 0);
    if (type_index < 0)
        return;
    const auto& type = types.types[static_cast<size_t>(type_index)];
    live::ModelCards cards;
    OA_CHECK(
        live::card_root_prefix(cards, static_cast<uint32_t>(type_index), type, fixture.recipe) == 0
    );
    const auto points = cards.root_prefix_points.size();
    live::mark_cards(cards);
    auto next = fixture.recipe;
    next.geometry = std::make_shared<oa::app::RootPrefixGeometry>(*fixture.recipe.geometry);
    OA_CHECK(live::card_root_prefix(cards, static_cast<uint32_t>(type_index), type, next) == 1);
    OA_CHECK(cards.root_prefix_points.size() == points * 2);
    OA_CHECK(cards.root_prefix_ranges.size() == 2);
    live::unmark_cards(cards);
    OA_CHECK(cards.root_prefix_points.size() == points);
    OA_CHECK(cards.root_prefix_poses.size() == 1 && cards.root_prefix_recipes.size() == 1);
    OA_CHECK(cards.root_prefix_ranges.size() == 1);
    OA_CHECK(live::card_root_prefix(cards, static_cast<uint32_t>(type_index), type, next) == 1);
    OA_CHECK(cards.root_prefix_points.size() == points * 2);
    live::clear_cards(cards);
    OA_CHECK(cards.root_prefix_points.empty() && cards.root_prefix_poses.empty());
    OA_CHECK(cards.root_prefix_recipes.empty() && cards.root_prefix_ranges.empty());
}

/// A saturated lookup keeps rendering distinct owners through uncached packets.
void root_prefix_scene_memo_saturation() {
    RootPrefixSceneFixture fixture;
    live::CardTypes types;
    const auto type_index = live::card_type(types, fixture.instance, fixture.prepared);
    OA_CHECK(type_index >= 0);
    if (type_index < 0)
        return;
    const auto& type = types.types[static_cast<size_t>(type_index)];
    live::ModelCards cards;
    OA_CHECK(
        live::card_root_prefix(cards, static_cast<uint32_t>(type_index), type, fixture.recipe) == 0
    );
    OA_CHECK(cards.root_prefix_ranges.size() == 1 && cards.root_prefix_slots.size() == 2048);
    const auto points = cards.root_prefix_points.size();
    std::fill(cards.root_prefix_slots.begin(), cards.root_prefix_slots.end(), 1);
    auto next = fixture.recipe;
    next.geometry = std::make_shared<oa::app::RootPrefixGeometry>(*fixture.recipe.geometry);
    OA_CHECK(live::card_root_prefix(cards, static_cast<uint32_t>(type_index), type, next) == 1);
    OA_CHECK(cards.root_prefix_ranges.size() == 1);
    OA_CHECK(cards.root_prefix_points.size() == points * 2);
    OA_CHECK(cards.root_prefix_poses.size() == 2 && cards.root_prefix_recipes.size() == 2);
    if (cards.root_prefix_poses.size() == 2) {
        OA_CHECK(cards.root_prefix_poses[1].first_point == points);
        OA_CHECK(cards.root_prefix_poses[1].point_count == points);
        OA_CHECK(cards.root_prefix_recipes[1].geometry == next.geometry);
        OA_CHECK(
            std::memcmp(
                cards.root_prefix_points.data(),
                cards.root_prefix_points.data() + points,
                points * sizeof(cards.root_prefix_points[0])
            ) == 0
        );
    }
    OA_CHECK(
        std::all_of(
            cards.root_prefix_slots.begin(), cards.root_prefix_slots.end(), [](uint32_t slot) {
                return slot == 1;
            }
        )
    );
}

/// Preserves lazy corners while joining mixed and repeated prefix bands.
void root_prefix_scene_parallel_packets() {
    RootPrefixSceneFixture fixture;
    fixture.battle.inputs.graphics_flags = 0;
    OA_CHECK(live::build(fixture.battle.inputs, fixture.battle.scene, fixture.battle.error));
    const auto& source = fixture.battle.scene.cards;
    OA_CHECK(source.units.size() == 1 && !source.root_prefix_poses.empty());
    if (source.units.size() != 1 || source.root_prefix_poses.empty())
        return;
    const auto ordinary = joined_card_fixture(1, 20, false);
    auto single = ordinary;
    OA_CHECK(live::append_parallel_model_cards(single, source, 0));
    OA_CHECK(live::append_parallel_model_cards(single, source, 0));
    auto batch = ordinary;
    const std::array<const live::ModelCards*, 2> bands{&source, &source};
    OA_CHECK(live::append_parallel_model_card_bands(batch, bands, 0));
    OA_CHECK(single.root_prefix_points.size() == source.root_prefix_points.size() * 2);
    OA_CHECK(single.root_prefix_poses.size() == source.root_prefix_poses.size() * 2);
    OA_CHECK(single.root_prefix_recipes.size() == single.root_prefix_poses.size());
    OA_CHECK(single.root_prefix_ranges.empty() && batch.root_prefix_ranges.empty());
    OA_CHECK(single.instances[0].root_prefix == 0);
    for (size_t copy = 0; copy < bands.size(); ++copy) {
        const auto& expected_unit = source.units[0];
        const auto& actual_unit = single.units[copy + 1];
        const auto& batch_unit = batch.units[copy + 1];
        OA_CHECK(single.instances[actual_unit.instance].positions == 0);
        OA_CHECK(single.instances[actual_unit.instance].root_prefix == copy + 1);
        for (size_t corner = 0; corner < expected_unit.index_count; ++corner) {
            const auto expected = live::card_corner(
                fixture.battle.scene.card_types,
                source,
                expected_unit,
                source.indices[expected_unit.first_index + corner]
            );
            const auto actual = live::card_corner(
                fixture.battle.scene.card_types,
                single,
                actual_unit,
                single.indices[actual_unit.first_index + corner]
            );
            const auto batched = live::card_corner(
                fixture.battle.scene.card_types,
                batch,
                batch_unit,
                batch.indices[batch_unit.first_index + corner]
            );
            OA_CHECK(std::memcmp(&expected, &actual, sizeof(expected)) == 0);
            OA_CHECK(std::memcmp(&expected, &batched, sizeof(expected)) == 0);
        }
    }
    auto bad = source;
    ++bad.root_prefix_poses[0].cosine;
    const auto instances = single.instances.size();
    const auto points = single.root_prefix_points.size();
    OA_CHECK(!live::can_append_parallel_model_cards(single, bad, 0));
    OA_CHECK(!live::append_parallel_model_cards(single, bad, 0));
    OA_CHECK(single.instances.size() == instances && single.root_prefix_points.size() == points);
    const std::array<const live::ModelCards*, 2> malformed{&source, &bad};
    OA_CHECK(!live::append_parallel_model_card_bands(single, malformed, 0));
    OA_CHECK(single.instances.size() == instances && single.root_prefix_points.size() == points);
    bad = source;
    bad.root_prefix_recipes.clear();
    OA_CHECK(!live::can_append_parallel_model_cards(single, bad, 0));
    bad = source;
    bad.units[0].root_prefix = false;
    OA_CHECK(!live::can_append_parallel_model_cards(single, bad, 0));
}

/// Resolves ordinary and owned-prefix shadow corners independently of packet offsets.
std::vector<std::array<float, 3>> root_prefix_shadow_points(const live::Scene& scene) {
    const auto draws = live::card_draws(scene.card_types, scene.cards);
    std::pair<std::vector<uint32_t>, std::vector<joined_gpu::ModelRun>> expanded;
    if (draws.shadow_index_encoding == joined_gpu::ShadowIndexEncoding::compact)
        expanded = expanded_shadow_templates(draws);
    else if (draws.shadow_index_encoding == joined_gpu::ShadowIndexEncoding::procedural)
        expanded = procedural_shadow_expansion(draws);
    else {
        expanded.first = scene.cards.cast_indices;
        expanded.second = scene.cards.cast_runs;
    }
    constexpr uint32_t corner_mask = (uint32_t{1} << joined_gpu::model_corner_bits) - 1;
    std::vector<std::array<float, 3>> result;
    result.reserve(expanded.first.size());
    for (const auto& run : expanded.second)
        for (std::size_t at = run.first_index; at < std::size_t{run.first_index} + run.index_count;
             ++at) {
            const auto word = expanded.first[at];
            const auto cast = run.first_instance + (word >> joined_gpu::model_corner_bits);
            live::CardUnit unit;
            unit.instance = scene.cards.casts[cast].instance;
            const auto point =
                live::card_corner(scene.card_types, scene.cards, unit, word & corner_mask);
            result.push_back({point.x, point.height, point.z});
        }
    return result;
}

/// Captures fixed and float bounds without retaining recipe owners or padding bytes.
std::vector<uint64_t> root_prefix_bounds_payload(const live::RootPrefixBoundsMemo& memo) {
    std::vector<uint64_t> result;
    const auto floats = [&](const auto& values) {
        for (const auto value : values)
            result.push_back(std::bit_cast<uint32_t>(value));
    };
    result.push_back(memo.epoch);
    result.push_back(memo.draws.size());
    result.push_back(memo.pieces.size());
    for (const auto& record : memo.draws) {
        for (const auto value : record.origin)
            result.push_back(std::bit_cast<uint32_t>(value));
        result.insert(
            result.end(),
            {record.visible_mask,
             record.nonempty_mask,
             record.epoch,
             record.first_piece,
             record.piece_count,
             record.domain,
             record.full_view_valid}
        );
        floats(record.full_view.low);
        floats(record.full_view.high);
        floats(record.full_view.picture);
    }
    for (const auto& piece : memo.pieces) {
        for (const auto point : {piece.fixed.low, piece.fixed.high})
            for (const auto value : {point.x, point.y, point.z})
                result.push_back(std::bit_cast<uint32_t>(value));
        floats(piece.low);
        floats(piece.high);
        result.push_back(piece.valid);
    }
    return result;
}

/// Compares rendered triangles and the scalar decisions affected by exact bounds.
void check_root_prefix_bounds_frame(const live::Scene& expected, const live::Scene& actual) {
    check_body_material_frame(expected, actual);
    OA_CHECK(root_prefix_shadow_points(expected) == root_prefix_shadow_points(actual));
    OA_CHECK(expected.cards.units.size() == actual.cards.units.size());
    for (size_t at = 0; at < std::min(expected.cards.units.size(), actual.cards.units.size());
         ++at) {
        const auto& a = expected.cards.units[at];
        const auto& b = actual.cards.units[at];
        OA_CHECK(
            std::tie(
                a.unit_id,
                a.vertex,
                a.index_count,
                a.left,
                a.top,
                a.right,
                a.bottom,
                a.bounded,
                a.marker,
                a.body_lod,
                a.root_prefix,
                a.displayed_anchor
            ) ==
            std::tie(
                b.unit_id,
                b.vertex,
                b.index_count,
                b.left,
                b.top,
                b.right,
                b.bottom,
                b.bounded,
                b.marker,
                b.body_lod,
                b.root_prefix,
                b.displayed_anchor
            )
        );
    }
    OA_CHECK(expected.cards.casts.size() == actual.cards.casts.size());
    for (size_t at = 0; at < std::min(expected.cards.casts.size(), actual.cards.casts.size());
         ++at) {
        const auto& a = expected.cards.casts[at];
        const auto& b = actual.cards.casts[at];
        OA_CHECK(
            std::tie(
                a.kind, a.base, a.ground_half, a.least_height, a.light, a.depth, a.across, a.down
            ) ==
            std::tie(
                b.kind, b.base, b.ground_half, b.least_height, b.light, b.depth, b.across, b.down
            )
        );
    }
    for (size_t at = 0; at < std::min(expected.unit_detail.size(), actual.unit_detail.size());
         ++at) {
        const auto& a = expected.unit_detail[at];
        const auto& b = actual.unit_detail[at];
        OA_CHECK(std::bit_cast<uint32_t>(a.pixels) == std::bit_cast<uint32_t>(b.pixels));
        for (size_t axis = 0; axis < a.center.size(); ++axis)
            OA_CHECK(
                std::bit_cast<uint32_t>(a.center[axis]) == std::bit_cast<uint32_t>(b.center[axis])
            );
        OA_CHECK(
            std::tie(
                a.pixels,
                a.center,
                a.body_piece_mask,
                a.body_medium_piece_mask,
                a.surface_outside,
                a.shadow_outside
            ) ==
            std::tie(
                b.pixels,
                b.center,
                b.body_piece_mask,
                b.body_medium_piece_mask,
                b.surface_outside,
                b.shadow_outside
            )
        );
    }
}

/// Keeps fractional, signed and sparse body bounds identical to uncached arithmetic.
void root_prefix_bounds_exact_frames() {
    bool strict_subset = false;
    for (const size_t pieces : {size_t{2}, size_t{8}, size_t{64}})
        for (const float scale : {0.4F, 0.8F, 1.6F}) {
            RootPrefixSceneFixture fixture(pieces);
            auto& battle = fixture.battle;
            auto& draw = battle.draws.models[0];
            constexpr int32_t fixed = oa::app::root_prefix_fixed_scale;
            draw.shown_position =
                oa::FixedVec3{16 * fixed + fixed / 3, fixed / 7, 16 * fixed - fixed / 5};
            fixture.recipe.placement.x += fixed / 11;
            draw.root_prefix = fixture.recipe;
            if (pieces == 64)
                for (size_t piece = 1; piece + 1 < pieces; ++piece)
                    fixture.instance.pieces()[piece].flags &=
                        ~static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
            battle.inputs.clip_projection = {
                scale,
                -0.0F,
                -0.21F * scale,
                48.375F,
                -0.17F * scale,
                -0.5F * scale,
                scale,
                49.8125F,
                0,
                -0.001F,
                0,
                0.5F
            };
            battle.inputs.detail_scale = 0.5F;
            live::Scene cached, ordinary;
            auto cached_inputs = battle.inputs;
            live::begin_root_prefix_bounds(cached_inputs, cached);
            OA_CHECK(live::root_prefix_supported(cached_inputs, draw, *draw.root_prefix));
            OA_CHECK(cached.root_prefix_bounds.draws.size() == 1);
            if (cached.root_prefix_bounds.draws.empty())
                continue;
            auto& record = cached.root_prefix_bounds.draws[0];
            OA_CHECK(record.epoch == cached.root_prefix_bounds.epoch && record.full_view_valid);
            if (pieces == 64)
                OA_CHECK(record.visible_mask == (uint64_t{1} | (uint64_t{1} << 63)));
            auto ordinary_inputs = battle.inputs;
            live::begin_root_prefix_bounds(ordinary_inputs, ordinary, 0);
            // A matching key must consume the retained numeric view, rather
            // than silently measuring every request through the uncached path.
            const auto saved_view = record.full_view;
            constexpr float outside_picture_limit = 2097152;
            record.full_view.picture.fill(outside_picture_limit);
            OA_CHECK(!live::root_prefix_supported(cached_inputs, draw, *draw.root_prefix));
            OA_CHECK(live::root_prefix_supported(ordinary_inputs, draw, *draw.root_prefix));
            record.full_view = saved_view;
            const auto payload = root_prefix_bounds_payload(cached.root_prefix_bounds);
            OA_CHECK(live::build(ordinary_inputs, ordinary, battle.error));
            OA_CHECK(live::build(cached_inputs, cached, battle.error));
            check_root_prefix_bounds_frame(ordinary, cached);
            if (pieces == 8 && cached.unit_detail.size() == 1) {
                const auto& detail = cached.unit_detail[0];
                const auto selected =
                    detail.body_lod == 1 ? detail.body_piece_mask : detail.body_medium_piece_mask;
                strict_subset |= detail.body_lod != 0 && selected != record.visible_mask;
            }
            OA_CHECK(root_prefix_bounds_payload(cached.root_prefix_bounds) == payload);
            OA_CHECK(!cached.root_prefix_bounds.draws[0].recipe.geometry);
            OA_CHECK(ordinary.root_prefix_bounds.draws.empty());
        }
    OA_CHECK(strict_subset);
}

/// Every changed key bypasses poisoned bounds rather than reusing an earlier recipe.
void root_prefix_bounds_key_misses() {
    for (unsigned change = 0; change < 9; ++change) {
        RootPrefixSceneFixture fixture;
        auto& battle = fixture.battle;
        auto& draw = battle.draws.models[0];
        live::Scene cached, ordinary;
        auto inputs = battle.inputs;
        live::begin_root_prefix_bounds(inputs, cached);
        OA_CHECK(live::root_prefix_supported(inputs, draw, *draw.root_prefix));
        OA_CHECK(cached.root_prefix_bounds.draws.size() == 1);
        if (cached.root_prefix_bounds.draws.empty())
            continue;
        auto& record = cached.root_prefix_bounds.draws[0];
        record.full_view.low.fill(-2000);
        record.full_view.high.fill(-1900);
        record.full_view.picture.fill(-2000);
        for (auto& piece : cached.root_prefix_bounds.pieces) {
            piece.fixed = {
                {-100 * 65536, -100 * 65536, -100 * 65536}, {-90 * 65536, -90 * 65536, -90 * 65536}
            };
            piece.low.fill(-2000);
            piece.high.fill(-1900);
        }
        auto& recipe = *draw.root_prefix;
        switch (change) {
        case 0:
            recipe.geometry = std::make_shared<oa::app::RootPrefixGeometry>(*recipe.geometry);
            break;
        case 1:
            ++recipe.rotation.sine;
            break;
        case 2:
            --recipe.rotation.cosine;
            break;
        case 3:
            ++recipe.placement.x;
            break;
        case 4:
            draw.shown_position = fixture.unit.position;
            ++draw.shown_position->x;
            break;
        case 5:
            inputs.clip_projection[1] = -0.0F;
            break;
        case 6:
            inputs.clip_projection[0] = std::nextafter(inputs.clip_projection[0], 1.0F);
            break;
        case 7:
            fixture.instance.pieces()[1].flags &=
                ~static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
            break;
        case 8:
            ++inputs.root_prefix_bounds_epoch;
            break;
        }
        auto ordinary_inputs = inputs;
        live::begin_root_prefix_bounds(ordinary_inputs, ordinary, 0);
        OA_CHECK(live::build(ordinary_inputs, ordinary, battle.error));
        OA_CHECK(live::build(inputs, cached, battle.error));
        check_root_prefix_bounds_frame(ordinary, cached);
    }
}

/// Admission keeps live map and unit gates, and foreign draws cannot seed an epoch.
void root_prefix_bounds_live_admission() {
    RootPrefixSceneFixture fixture;
    auto& battle = fixture.battle;
    auto& draw = battle.draws.models[0];
    live::Scene scene;
    auto inputs = battle.inputs;
    live::begin_root_prefix_bounds(inputs, scene);
    const auto copied = draw;
    OA_CHECK(live::root_prefix_supported(inputs, copied, *copied.root_prefix));
    OA_CHECK(scene.root_prefix_bounds.draws.size() == 1);
    if (scene.root_prefix_bounds.draws.empty())
        return;
    OA_CHECK(scene.root_prefix_bounds.draws[0].epoch == 0);
    OA_CHECK(live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    fixture.unit.state_flags |= oa::present::model::unit_state_cloaked;
    OA_CHECK(!live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    fixture.unit.state_flags &= ~oa::present::model::unit_state_cloaked;
    const auto attributes = battle.map.attributes;
    battle.map.attributes.clear();
    OA_CHECK(!live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    battle.map.attributes = attributes;
    fixture.unit.position.y = 4 * oa::app::root_prefix_fixed_scale;
    inputs.sea_level = 2;
    for (auto& attribute : battle.map.attributes)
        attribute.height = 3;
    live::begin_root_prefix_bounds(inputs, scene);
    OA_CHECK(live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    for (auto& attribute : battle.map.attributes)
        attribute.height = 0;
    OA_CHECK(!live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    for (auto& attribute : battle.map.attributes)
        attribute.height = 3;
    auto replacement = std::make_shared<oa::app::RootPrefixGeometry>(*fixture.recipe.geometry);
    draw.root_prefix->geometry = replacement;
    live::begin_root_prefix_bounds(inputs, scene);
    OA_CHECK(live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    std::weak_ptr<const oa::app::RootPrefixGeometry> old_owner = replacement;
    replacement.reset();
    draw.root_prefix->geometry = fixture.recipe.geometry;
    live::begin_root_prefix_bounds(inputs, scene);
    OA_CHECK(old_owner.expired());
    replacement = std::make_shared<oa::app::RootPrefixGeometry>(*fixture.recipe.geometry);
    draw.root_prefix->geometry = replacement;
    live::begin_root_prefix_bounds(inputs, scene);
    OA_CHECK(live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    std::weak_ptr<const oa::app::RootPrefixGeometry> aborted_owner = replacement;
    replacement.reset();
    draw.root_prefix->geometry = fixture.recipe.geometry;
    inputs.root_prefix_enabled = false;
    live::begin_root_prefix_bounds(inputs, scene);
    OA_CHECK(aborted_owner.expired() && !scene.root_prefix_bounds.prepared);
    OA_CHECK(inputs.root_prefix_bounds == nullptr && inputs.root_prefix_bounds_epoch == 0);
}

/// Epochs, moved Scenes and refused allocations retain the uncached frame.
void root_prefix_bounds_lifecycle() {
    RootPrefixSceneFixture fixture;
    auto& battle = fixture.battle;
    const auto& draw = battle.draws.models[0];
    live::Scene ordinary;
    auto ordinary_inputs = battle.inputs;
    live::begin_root_prefix_bounds(ordinary_inputs, ordinary, 0);
    OA_CHECK(live::build(ordinary_inputs, ordinary, battle.error));
    live::Scene capped;
    auto capped_inputs = battle.inputs;
    live::begin_root_prefix_bounds(
        capped_inputs,
        capped,
        sizeof(live::RootPrefixBoundsMemo) + sizeof(live::RootPrefixBoundsRecord)
    );
    OA_CHECK(capped.root_prefix_bounds.draws.empty() && capped.root_prefix_bounds.pieces.empty());
    OA_CHECK(live::build(capped_inputs, capped, battle.error));
    check_root_prefix_bounds_frame(ordinary, capped);
    for (const size_t failed_allocation : {size_t{0}, size_t{1}}) {
        live::Scene scene;
        auto inputs = battle.inputs;
        ground_allocations = refused_ground_allocations = 0;
        ground_allocation_to_refuse = failed_allocation;
        watch_ground_allocations = true;
        live::begin_root_prefix_bounds(inputs, scene);
        watch_ground_allocations = false;
        ground_allocation_to_refuse = std::numeric_limits<size_t>::max();
        OA_CHECK(refused_ground_allocations == 1);
        OA_CHECK(scene.root_prefix_bounds.draws.empty() && scene.root_prefix_bounds.pieces.empty());
        OA_CHECK(live::root_prefix_supported(inputs, draw, *draw.root_prefix));
        OA_CHECK(live::build(inputs, scene, battle.error));
        check_root_prefix_bounds_frame(ordinary, scene);
    }
    live::Scene consumed;
    auto consumed_inputs = battle.inputs;
    live::begin_root_prefix_bounds(consumed_inputs, consumed);
    const auto prepared = consumed.root_prefix_bounds.epoch;
    OA_CHECK(live::build(consumed_inputs, consumed, battle.error));
    OA_CHECK(
        consumed.root_prefix_bounds.epoch == prepared && !consumed.root_prefix_bounds.prepared
    );
    OA_CHECK(live::build(consumed_inputs, consumed, battle.error));
    OA_CHECK(consumed.root_prefix_bounds.epoch != prepared);
    check_root_prefix_bounds_frame(ordinary, consumed);
    consumed = live::Scene{};
    OA_CHECK(live::build(consumed_inputs, consumed, battle.error));
    check_root_prefix_bounds_frame(ordinary, consumed);
    live::Scene source;
    auto inputs = battle.inputs;
    live::begin_root_prefix_bounds(inputs, source);
    OA_CHECK(live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    const auto epoch = source.root_prefix_bounds.epoch;
    live::Scene moved = std::move(source);
    OA_CHECK(live::build(inputs, moved, battle.error));
    OA_CHECK(moved.root_prefix_bounds.epoch != epoch);
    check_root_prefix_bounds_frame(ordinary, moved);
    const auto consumed_epoch = moved.root_prefix_bounds.epoch;
    OA_CHECK(live::build(inputs, moved, battle.error));
    OA_CHECK(moved.root_prefix_bounds.epoch != consumed_epoch);
    check_root_prefix_bounds_frame(ordinary, moved);
}

/// A changed floating-point control cannot reuse earlier floating bounds.
void root_prefix_bounds_rounding() {
    const int saved = std::fegetround();
    OA_CHECK(saved != -1);
    if (saved == -1)
        return;

    struct RestoreRounding {
        int mode{};

        ~RestoreRounding() { std::fesetround(mode); }
    } restore{saved};

    const bool nearest = std::fesetround(FE_TONEAREST) == 0;
    OA_CHECK(nearest);
    if (!nearest)
        return;
    RootPrefixSceneFixture fixture;
    auto& battle = fixture.battle;
    const auto& draw = battle.draws.models[0];
    live::Scene cached, ordinary;
    auto inputs = battle.inputs;
    live::begin_root_prefix_bounds(inputs, cached);
    OA_CHECK(live::root_prefix_supported(inputs, draw, *draw.root_prefix));
    if (cached.root_prefix_bounds.draws.empty())
        return;
    cached.root_prefix_bounds.draws[0].full_view.picture.fill(-2000);
    OA_CHECK(std::fesetround(FE_UPWARD) == 0);
    auto ordinary_inputs = inputs;
    live::begin_root_prefix_bounds(ordinary_inputs, ordinary, 0);
    OA_CHECK(live::build(ordinary_inputs, ordinary, battle.error));
    OA_CHECK(live::build(inputs, cached, battle.error));
    check_root_prefix_bounds_frame(ordinary, cached);
}

/// Keeps owned recipes through separated worker intervals and actual material pauses.
/// @param ordered_packing enables the Enhanced ordered model-material pass
void root_prefix_scene_multiple_ranges(bool ordered_packing) {
    MultipleModelRangeFixture fixture;
    RootPrefixSceneFixture prefix;
    auto& battle = fixture.bodies.battle;
    battle.inputs.body_lod_enabled = ordered_packing;
    battle.inputs.root_prefix_enabled = true;
    battle.inputs.nanolathe_lighting = false;
    constexpr std::array gaps{
        MultipleModelRangeFixture::first_gap,
        MultipleModelRangeFixture::second_gap,
        MultipleModelRangeFixture::suffix
    };
    for (uint32_t index = 0; index < MultipleModelRangeFixture::population; ++index) {
        if (std::find(gaps.begin(), gaps.end(), index) != gaps.end())
            continue;
        auto& draw = battle.draws.models[index];
        draw.model.instance = &prefix.instance;
        draw.model.prepared = &prefix.prepared;
        draw.model.state = &prefix.state;
        draw.root_prefix = prefix.recipe;
    }
    auto suffix_material = fixture.unique_material;
    constexpr uint8_t suffix_color = 4;
    for (auto& object : suffix_material.objects)
        for (auto& primitive : object.primitives)
            primitive.color = suffix_color;
    std::fill_n(battle.palette.begin() + suffix_color * oa::palette_entry_bytes, 3, uint8_t{190});
    const auto canonical = fixture.units;
    const auto revision = prefix.instance.materialization_revision();
    oa::platform::job_pool::Pool pool(4);
    live::Scene serial, parallel;
    const auto compare = [&] {
        battle.inputs.pool = nullptr;
        live::begin_root_prefix_bounds(battle.inputs, serial, 0);
        OA_CHECK(live::build(battle.inputs, serial, battle.error));
        battle.inputs.pool = &pool;
        live::begin_root_prefix_bounds(battle.inputs, parallel);
        for (const auto& draw : battle.draws.models)
            if (draw.root_prefix)
                OA_CHECK(live::root_prefix_supported(battle.inputs, draw, *draw.root_prefix));
        const auto payload = root_prefix_bounds_payload(parallel.root_prefix_bounds);
        OA_CHECK(live::build(battle.inputs, parallel, battle.error));
        OA_CHECK(root_prefix_bounds_payload(parallel.root_prefix_bounds) == payload);
        OA_CHECK(battle.error.empty());
        check_body_material_frame(serial, parallel);
        OA_CHECK(serial.model_count == parallel.model_count);
        OA_CHECK(serial.shadow_count == parallel.shadow_count);
        OA_CHECK(serial.translucent_vertices.size() == parallel.translucent_vertices.size());
        if (serial.translucent_vertices.size() == parallel.translucent_vertices.size() &&
            !serial.translucent_vertices.empty())
            OA_CHECK(
                std::memcmp(
                    serial.translucent_vertices.data(),
                    parallel.translucent_vertices.data(),
                    serial.translucent_vertices.size() * sizeof(serial.translucent_vertices[0])
                ) == 0
            );
        OA_CHECK(
            std::equal(
                serial.translucent_ranges.begin(),
                serial.translucent_ranges.end(),
                parallel.translucent_ranges.begin(),
                parallel.translucent_ranges.end(),
                [](const auto& left, const auto& right) {
                    return left.unit_id == right.unit_id &&
                           left.first_vertex == right.first_vertex &&
                           left.vertex_count == right.vertex_count;
                }
            )
        );
        OA_CHECK(serial.cards.units.size() == parallel.cards.units.size());
        for (std::size_t at = 0;
             at < std::min(serial.cards.units.size(), parallel.cards.units.size());
             ++at) {
            const auto& left = serial.cards.units[at];
            const auto& right = parallel.cards.units[at];
            OA_CHECK(
                std::tie(
                    left.unit_id,
                    left.index_count,
                    left.root_prefix,
                    left.left,
                    left.top,
                    left.right,
                    left.bottom,
                    left.bounded,
                    left.displayed_anchor
                ) ==
                std::tie(
                    right.unit_id,
                    right.index_count,
                    right.root_prefix,
                    right.left,
                    right.top,
                    right.right,
                    right.bottom,
                    right.bounded,
                    right.displayed_anchor
                )
            );
        }
        OA_CHECK(
            std::count_if(
                parallel.cards.units.begin(), parallel.cards.units.end(), [](const auto& unit) {
                    return unit.root_prefix;
                }
            ) == MultipleModelRangeFixture::candidate_slots
        );
        OA_CHECK(serial.cards.casts.size() == parallel.cards.casts.size());
        for (std::size_t at = 0;
             at < std::min(serial.cards.casts.size(), parallel.cards.casts.size());
             ++at) {
            const auto& left = serial.cards.casts[at];
            const auto& right = parallel.cards.casts[at];
            OA_CHECK(
                std::tie(
                    left.kind,
                    left.base,
                    left.ground_half,
                    left.least_height,
                    left.light,
                    left.depth,
                    left.across,
                    left.down
                ) ==
                std::tie(
                    right.kind,
                    right.base,
                    right.ground_half,
                    right.least_height,
                    right.light,
                    right.depth,
                    right.across,
                    right.down
                )
            );
        }
        OA_CHECK(root_prefix_shadow_points(serial) == root_prefix_shadow_points(parallel));
        OA_CHECK(!parallel.cards.root_prefix_points.empty());
        OA_CHECK(!parallel.cards.root_prefix_recipes.empty());
        for (const auto& recipe : parallel.cards.root_prefix_recipes)
            OA_CHECK(recipe.geometry == prefix.recipe.geometry);
        OA_CHECK(prefix.instance.materialization_revision() == revision);
        for (const auto& piece : prefix.instance.pieces())
            OA_CHECK(piece.transformed_vertices.empty());
        OA_CHECK(
            std::memcmp(
                canonical.data(), fixture.units.data(), canonical.size() * sizeof(canonical[0])
            ) == 0
        );
    };
    const auto accepted = [&](bool packed = false) {
        const auto& work = parallel.model_work_counts;
        OA_CHECK(work.candidate_bands == 4 && work.accepted_bands == 4);
        OA_CHECK(work.candidate_slots == MultipleModelRangeFixture::candidate_slots);
        OA_CHECK(work.accepted_slots == MultipleModelRangeFixture::candidate_slots);
        OA_CHECK(work.fallback_frames == 0);
        if (packed) {
            OA_CHECK(parallel.passes.models.materials_packed > 0);
            OA_CHECK(parallel.passes.models.texels_packed > 0);
        } else {
            OA_CHECK(parallel.passes.models.materials_packed == 0);
            OA_CHECK(parallel.passes.models.texels_packed == 0);
        }
    };
    compare();
    compare();
    accepted();
    // Only the serial middle draw requests this cold material. Enhanced
    // ordered packing retains the joined packets; the false gate keeps
    // the original complete serial retry and the same geometry oracle.
    battle.draws.models[MultipleModelRangeFixture::first_gap].model.prepared =
        &fixture.unique_material;
    compare();
    if (ordered_packing) {
        accepted(true);
    } else {
        OA_CHECK(parallel.model_work_counts.candidate_bands == 4);
        OA_CHECK(parallel.model_work_counts.accepted_bands == 0);
        OA_CHECK(parallel.model_work_counts.accepted_slots == 0);
        OA_CHECK(parallel.model_work_counts.fallback_frames == 1);
        OA_CHECK(parallel.passes.models.materials_packed > 0);
        OA_CHECK(parallel.passes.models.texels_packed > 0);
    }
    compare();
    accepted();
    // Both policies retain complete joined intervals before a cold suffix.
    battle.draws.models[MultipleModelRangeFixture::suffix].model.prepared = &suffix_material;
    compare();
    accepted(true);
    compare();
    accepted();
    for (const bool procedural : {false, true}) {
        battle.inputs.shadow_index_templates = true;
        battle.inputs.shadow_index_procedural = procedural;
        compare();
        compare();
        accepted();
    }
}

/// The same owned-geometry oracle covers ordered packing and the old false gate.
void root_prefix_scene_multiple_ranges() {
    root_prefix_scene_multiple_ranges(false);
    root_prefix_scene_multiple_ranges(true);
}

} // namespace

int main() {
    root_prefix_bounds_exact_frames();
    root_prefix_bounds_key_misses();
    root_prefix_bounds_live_admission();
    root_prefix_bounds_lifecycle();
    root_prefix_bounds_rounding();
    root_prefix_scene_owned_geometry();
    root_prefix_scene_proxy_symmetry();
    root_prefix_scene_clear_geometry();
    root_prefix_scene_virtual_image_frames();
    root_prefix_scene_admission();
    root_prefix_scene_incomplete_terrain();
    root_prefix_scene_conservative_admission();
    root_prefix_scene_card_rollback();
    root_prefix_scene_memo_saturation();
    root_prefix_scene_parallel_packets();
    root_prefix_scene_multiple_ranges();
    omitted_model_snapshots();
    omitted_model_material_retries();
    interior_model_cull_tests();
    body_detail_pose_geometry();
    body_lod_geometry_tests();
    body_lod_card_tests();
    caster_index_runs_tests();
    animated_body_lod_scene();
    crowded_animated_body_lod();
    crowded_body_position_budget();
    offscreen_body_surface_skip();
    caster_index_piece_policy();
    caster_index_building_walls();
    caster_index_run_boundary();
    caster_index_capacity_fallback();
    caster_index_coarse_equivalence();
    shadow_index_template_runs();
    shadow_index_template_mixed_masks();
    shadow_index_template_piece_policy();
    shadow_index_template_lifecycle();
    shadow_index_template_candidates();
    shadow_index_template_registry_copies();
    shadow_index_procedural_scene();
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
    terrain_demand_exact_sources();
    terrain_demand_fog_sources();
    terrain_demand_other_frame_inputs();
    terrain_demand_rounding_control();
    terrain_demand_room_and_empty_views();
    terrain_demand_storage_fallback();
    terrain_room_remembered();
    terrain_held_while_moving();
    terrain_square_given_back();
    terrain_squares_given_back_whole();
    coarse_ground_far_out();
    terrain_beyond_picture();
    ground_kept_while_moving();
    contained_camera_ground();
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
    persistent_material_sources();
    cropped_ground_sprites();
    building_foundations();
    foundations_cut_once();
    foundations_recut_where_plots_change();
    foundation_candidates();
    structure_base_above_ground();
    cached_unit_shadows();
    frame_rows_hold_unit_shadows();
    frame_rows_grow_for_spilled_shadows();
    frame_rows_hold_effects();
    frame_rows_begin_for_tall_effects();
    frame_effect_fallbacks();
    ground_index_reuse();
    ground_index_build_diagnostics();
    ground_index_build_record_storage();
    ground_points_preserve_queries();
    ground_index_bounds_on_threads();
    ground_index_bounds_revision_reuse();
    ground_query_preparation();
    ground_query_preparation_sources();
    ground_query_preparation_owns_sources();
    ground_query_preparation_allocation_recovery();
    passes_on_threads();
    ground_drawn_ahead_on_threads();
    moving_camera_comes_back_to_its_picture();
    models_on_card();
    shared_model_positions();
    shared_model_coordinate_rounding();
    indexed_model_vertex_projection();
    high_indexed_model_vertex_projection();
    strategic_unit_markers_without_models();
    strategic_unit_marker_visibility();
    strategic_unit_marker_visibility_controls();
    strategic_unit_marker_population();
    strategic_unit_markers_on_threads();
    strategic_unit_markers_on_card();
    strategic_unit_marker_mixed_passes();
    strategic_unit_marker_water_budget();
    projected_unit_detail();
    shared_unit_detail_bounds();
    shared_unit_detail_texture_visibility();
    unit_detail_on_threads();
    crowded_unit_detail();
    large_shared_model_scene();
    shadows_on_card();
    ground_shadows_in_view();
    preplanned_omission_validation();
    geometry_shadow_frame_policy();
    physical_model_bands();
    physical_model_multiple_ranges();
    parallel_model_card_join();
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
