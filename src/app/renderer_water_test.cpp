// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_water.hpp"
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
