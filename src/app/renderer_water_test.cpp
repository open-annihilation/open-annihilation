// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_water.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include "oa/test/check.hpp"
#include <limits>

int main() {
    using namespace oa::app::renderer_water;
    oa::formats::tnt::Map flow_map;
    flow_map.attribute_width = 32;
    flow_map.attribute_height = 64;
    flow_map.sea_level = 24;
    flow_map.attributes.resize(32 * 64);
    WindState flow;
    flow.reset(flow_map);
    const auto heading = flow.value;
    OA_CHECK(std::abs(std::hypot(heading.x, heading.z) - 1) < .0001F);
    OA_CHECK(heading.strength == .55F);
    flow.advance(30);
    OA_CHECK(std::abs(std::hypot(flow.value.drift_x, flow.value.drift_z) - 13.2F) < .001F);
    const auto paused = flow.value;
    flow.advance(30);
    OA_CHECK(flow.value.drift_x == paused.drift_x && flow.value.drift_z == paused.drift_z);
    for (uint32_t tick = 31; tick < 10000; ++tick)
        flow.advance(tick);
    OA_CHECK(
        flow.value.x == heading.x && flow.value.z == heading.z &&
        flow.value.strength == heading.strength
    );
    // Reload at an arbitrary tick gives the same phase as uninterrupted rendering.
    WindState loaded;
    loaded.reset(flow_map);
    loaded.advance(9999);
    OA_CHECK(
        loaded.value.drift_x == flow.value.drift_x && loaded.value.drift_z == flow.value.drift_z
    );
    // Seeking backwards recovers the old phase without restarting the water animation.
    loaded.advance(30);
    OA_CHECK(loaded.value.drift_x == paused.drift_x && loaded.value.drift_z == paused.drift_z);
    // Map identity changes the dominant heading; simulation wind is never an input.
    flow_map.sea_level = 25;
    loaded.reset(flow_map);
    OA_CHECK(loaded.value.x != heading.x || loaded.value.z != heading.z);
    flow.advance(0);
    OA_CHECK(flow.value.drift_x == 0 && flow.value.drift_z == 0);

    // A water sea shows the sliders' strengths and its shots' rings; a lava
    // sea none of the water effects, and a sea that swallows shots no rings.
    const Levels chosen{.6F, .3F, 1, 2};
    const auto shown = [&](const Sea& sea) {
        const auto levels = shown_levels(sea, chosen);
        return std::array<float, 4>{
            levels.ripples, levels.coast, levels.shimmer, levels.reflections
        };
    };
    OA_CHECK((shown({}) == std::array<float, 4>{.6F, .3F, 1, 2}));
    OA_CHECK((shown({false, true}) == std::array<float, 4>{.6F, .3F, 1, 2}));
    OA_CHECK((shown({true, false}) == std::array<float, 4>{}));
    OA_CHECK((shown({true, true}) == std::array<float, 4>{}));
    OA_CHECK(stirred_by_shots({}));
    OA_CHECK(!stirred_by_shots({false, true}));
    OA_CHECK(!stirred_by_shots({true, false}));
    OA_CHECK(!stirred_by_shots({true, true}));

    // Foam over a ground keeps the ground's hue and is lighter than it in
    // every channel; over white it stays white.
    for (const auto& ground :
         {std::array<float, 3>{.08F, .29F, .24F},
          std::array<float, 3>{.45F, .35F, .25F},
          std::array<float, 3>{.85F, .78F, .60F},
          std::array<float, 3>{.02F, .02F, .02F}}) {
        const auto foam = foam_tint(ground);
        for (std::size_t channel = 0; channel < 3; ++channel)
            OA_CHECK(foam[channel] > ground[channel] + .1F && foam[channel] <= 1);
        for (std::size_t a = 0; a < 3; ++a)
            for (std::size_t b = 0; b < 3; ++b)
                OA_CHECK(ground[a] <= ground[b] || foam[a] > foam[b]);
    }
    for (const float channel : foam_tint({1, 1, 1}))
        OA_CHECK(channel > .9999F && channel <= 1);

    // The foam's colour follows the map's picture: a cache made with the
    // palette gives each half of a two-tile picture its own tint, and one
    // made without it the foam's plain colour.
    {
        oa::formats::tnt::Map picture;
        picture.attribute_width = picture.attribute_height = 4;
        picture.tile_width = picture.tile_height = 2;
        picture.sea_level = 10;
        picture.attributes.resize(16);
        picture.tile_count = 2;
        picture.tile_indices = {0, 1, 0, 1};
        picture.tile_palette_indices.assign(2 * 1024, 1);
        std::fill(
            picture.tile_palette_indices.begin() + 1024, picture.tile_palette_indices.end(), 2
        );
        std::array<uint8_t, 768> palette{};
        palette[3] = 40, palette[4] = 120, palette[5] = 60;
        palette[6] = 120, palette[7] = 90, palette[8] = 50;
        const auto close = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return std::abs(a[0] - b[0]) < .01F && std::abs(a[1] - b[1]) < .01F &&
                   std::abs(a[2] - b[2]) < .01F;
        };
        SurfaceCache tinted;
        tinted.reset(picture, palette);
        const auto green = foam_tint({40 / 255.0F, 120 / 255.0F, 60 / 255.0F});
        const auto brown = foam_tint({120 / 255.0F, 90 / 255.0F, 50 / 255.0F});
        OA_CHECK(close(tinted.foam_colour(4, 4), green));
        OA_CHECK(close(tinted.foam_colour(4, 60), green));
        OA_CHECK(close(tinted.foam_colour(60, 4), brown));
        OA_CHECK(close(tinted.foam_colour(-100, 4), green));
        // Between the halves the colour runs evenly from one to the other.
        const auto middle = tinted.foam_colour(32, 32);
        OA_CHECK(middle[0] > green[0] && middle[0] < brown[0]);
        SurfaceCache plain;
        plain.reset(picture);
        OA_CHECK((plain.foam_colour(4, 4) == std::array<float, 3>{.88F, .94F, .97F}));
    }

    // Underwater weapons cannot create air-side rings, including at the waterline.
    for (auto kind : {Kind::firing, Kind::hit, Kind::water_impact})
        for (float height : {10.0F, 29.0F, 30.0F, 45.0F})
            OA_CHECK(surface_strength(kind, height, 30, true, false) == 0);
    OA_CHECK(surface_strength(Kind::water_impact, 25, 30, true, true) > 0);
    OA_CHECK(surface_strength(Kind::hit, 25, 30, true, true) == 0);
    OA_CHECK(surface_strength(Kind::firing, 25, 30, true, true) == 0);
    for (auto kind : {Kind::firing, Kind::hit}) {
        OA_CHECK(surface_strength(kind, 29.99F, 30, false, false) == 0);
        OA_CHECK(surface_strength(kind, 10, 30, false, false) == 0);
        OA_CHECK(surface_strength(kind, 30, 30, false, false) > 0);
        OA_CHECK(surface_strength(kind, 55, 30, false, false) > 0);
        OA_CHECK(surface_strength(kind, 100, 30, false, false) == 0);
    }
    OA_CHECK(surface_strength(Kind::water_impact, 28, 30, false, false) > 0);
    OA_CHECK(surface_strength(Kind::water_impact, 20, 30, false, false) == 0);
    OA_CHECK(surface_strength(Kind::water_impact, 5, 30, false, true) > 0);
    OA_CHECK(surface_strength(Kind::water_impact, 50, 30, false, true) == 0);
    OA_CHECK(
        surface_strength(Kind::hit, std::numeric_limits<float>::quiet_NaN(), 30, false, false) == 0
    );

    History history;
    history.advance(100);
    history.add({0, 0, 100, 1, Kind::firing}, false);
    history.add({0, 0, 101, 1, Kind::hit}, true);
    history.add({0, 0, 40, 1, Kind::water_impact}, true);
    OA_CHECK(history.active().empty());
    history.add({0, 0, 100, .4F, Kind::firing}, true);
    history.add({2, 1, 100, .4F, Kind::firing}, true);
    OA_CHECK(history.active().size() == 1 && history.active()[0].strength > .4F);
    history.add({2, 1, 100, .4F, Kind::hit}, true);
    OA_CHECK(history.active().size() == 2);
    for (std::size_t i = 0; i < History::capacity * 2; ++i)
        history.add({float(i * 32), 100, 100, 2, Kind::water_impact}, true);
    OA_CHECK(history.active().size() == History::capacity);
    OA_CHECK(history.active().back().strength == 1);
    history.advance(159);
    OA_CHECK(history.active().size() == History::capacity);
    history.advance(160);
    OA_CHECK(history.active().empty());
    history.add({0, 0, 160, 1, Kind::hit}, true);
    history.advance(159);
    OA_CHECK(history.active().empty());
    history.add({std::numeric_limits<float>::quiet_NaN(), 0, 159, 1, Kind::hit}, true);
    OA_CHECK(history.active().empty());

    oa::formats::tnt::Map map;
    map.attribute_width = map.attribute_height = 64;
    map.sea_level = 50;
    map.attributes.resize(64 * 64);
    for (auto& attribute : map.attributes)
        attribute.height = 60;
    // A tiny enclosed pool and a broad sea have different wave energy.
    map.attributes[4 * 64 + 4].height = 0;
    for (int z = 16; z < 64; ++z)
        for (int x = 16; x < 64; ++x)
            map.attributes[z * 64 + x].height = 0;
    SurfaceCache cache;
    cache.reset(map);
    const auto pond = cache.fetch(4 * 16 + 8, 4 * 16 + 8);
    const auto sea = cache.fetch(40 * 16, 40 * 16);
    OA_CHECK(pond > 0 && pond < sea * .25F);
    OA_CHECK(sea > .9F);
    OA_CHECK(cache.fetch(0, 0) == 0);
    OA_CHECK(cache.fetch(-1, 100) == 0);
    OA_CHECK(cache.fetch(1024, 100) == 0);
    OA_CHECK(cache.fetch(std::numeric_limits<float>::infinity(), 100) == 0);
    // Replacing a same-sized map invalidates its water metadata explicitly.
    for (auto& attribute : map.attributes)
        attribute.height = 60;
    cache.reset(map);
    OA_CHECK(cache.fetch(40 * 16, 40 * 16) == 0);
    cache.reset({});
    OA_CHECK(cache.fetch(0, 0) == 0);
    return oa::test::check_exit_status();
}
