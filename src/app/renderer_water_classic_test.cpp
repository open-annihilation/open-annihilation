// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
// Classic water geometry remains bounded, visible, optional and outside dry land.
#include "renderer_water_classic.hpp"
#include "renderer_water_appearance.hpp"
#include "renderer_water_surface.hpp"
#include "renderer_water_effects.hpp"
#include "oa/test/check.hpp"
#include <cmath>
#include <chrono>
#include <cstdio>
#include <string_view>

using namespace oa::app;

int main(int argc, char** argv) {
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
                const double elapsed = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - start
                )
                                           .count();
                total += elapsed;
                maximum = std::max(maximum, elapsed);
                vertices = std::max(vertices, output.vertices.size());
            }
            std::printf(
                "classic water geometry zoom %.2f: 20 frames, mean %.4f ms, max %.4f ms, max %zu "
                "vertices\n",
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
    card::CardFrame frame;
    renderer_water_classic::surface(frame, input);
    OA_CHECK(!frame.vertices.empty());
    OA_CHECK(frame.vertices.size() <= 4096 * 28);
    const auto first_frame = frame.vertices;
    OA_CHECK(std::any_of(frame.vertices.begin(), frame.vertices.end(), [](const auto& vertex) {
        return vertex.colour.alpha > 0.05F;
    }));
    frame.reset();
    input.seconds = 0.5;
    input.wind.drift_x = 12;
    renderer_water_classic::surface(frame, input);
    OA_CHECK(!frame.vertices.empty());
    bool travelled = frame.vertices.size() != first_frame.size();
    for (std::size_t i = 0; i < std::min(frame.vertices.size(), first_frame.size()); ++i)
        travelled = travelled || frame.vertices[i].colour.alpha != first_frame[i].colour.alpha;
    OA_CHECK(travelled);
    const renderer_water::Wind calm{};
    const renderer_water::Wind east{1, 0, 1, 0, 0};
    const renderer_water::Wind north{0, 1, 1, 0, 0};
    const renderer_water::Wind drift_east{1, 0, 1, 12, 0};
    const renderer_water::Wind drift_north{0, 1, 1, 0, 12};
    bool stronger = false, redirected = false;
    for (int z = 0; z < 240; z += 17)
        for (int x = 0; x < 320; x += 19) {
            const auto still = renderer_water_surface::ripple_alpha(float(x), float(z), calm);
            const auto moving = renderer_water_surface::ripple_alpha(float(x), float(z), east);
            stronger = stronger || moving > still;
            OA_CHECK(moving == renderer_water_surface::ripple_alpha(float(x), float(z), north));
            redirected = redirected ||
                         renderer_water_surface::ripple_alpha(float(x), float(z), drift_east) !=
                             renderer_water_surface::ripple_alpha(float(x), float(z), drift_north);
        }
    OA_CHECK(stronger);
    OA_CHECK(redirected);
    bool fine_detail = false;
    for (int x = 0; x < 320; x += 11)
        fine_detail =
            fine_detail || renderer_water_surface::ripple_alpha(float(x), 96, east, 0.5F) !=
                               renderer_water_surface::ripple_alpha(float(x), 96, east, 1.25F);
    OA_CHECK(fine_detail);
    // No single component's translation reproduces the whole evolving field.
    for (float ratio : {0.28F, 0.65F, 1.0F, 1.35F}) {
        float difference = 0;
        for (int z = 24; z < 240; z += 31)
            for (int x = 24; x < 320; x += 37)
                difference += std::abs(
                    renderer_water_surface::ripple_alpha(float(x), float(z), drift_east, 1.25F) -
                    renderer_water_surface::ripple_alpha(
                        float(x) - 12 * ratio, float(z), east, 1.25F
                    )
                );
        OA_CHECK(difference > 0.01F);
    }
    input.wind = calm;
    frame.reset();
    renderer_water_classic::surface(frame, input);
    const auto calm_vertices = frame.vertices;
    input.seconds += 5;
    frame.reset();
    renderer_water_classic::surface(frame, input);
    OA_CHECK(frame.vertices.size() == calm_vertices.size());
    for (std::size_t i = 0; i < std::min(frame.vertices.size(), calm_vertices.size()); ++i)
        OA_CHECK(frame.vertices[i].colour.alpha == calm_vertices[i].colour.alpha);
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
    make_ship();
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices.size() > 4);
    OA_CHECK(frame.batches.size() == 3);
    OA_CHECK(input.reflected_vertices == frame.vertices.size() - 4);
    OA_CHECK(frame.vertices[0].y == 96);
    // The reflected silhouette starts at the near hull edge and extends
    // outside the original footprint, so the ship cannot paint it all over.
    bool reflection_visible = false, reflection_faded = false;
    for (std::size_t i = 4; i < frame.vertices.size(); ++i) {
        OA_CHECK(frame.vertices[i].y >= 112 && frame.vertices[i].y <= 115.841F);
        OA_CHECK(frame.vertices[i].colour.alpha <= renderer_water_appearance::reflection_opacity);
        reflection_visible = reflection_visible || frame.vertices[i].colour.alpha > 0.05F;
        reflection_faded = reflection_faded || frame.vertices[i].colour.alpha < 0.001F;
    }
    OA_CHECK(reflection_visible);
    OA_CHECK(reflection_faded);
    OA_CHECK(frame.batches[1].blend == card::Blend::alpha);
    const auto reflected_x = frame.vertices[4].x;
    make_ship();
    input.seconds += 0.5;
    renderer_water_classic::objects(frame, input, draws, draw, 0, 0);
    OA_CHECK(frame.vertices[4].x != reflected_x);
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
    return oa::test::check_exit_status();
}
