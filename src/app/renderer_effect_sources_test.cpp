// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Cooling uses simulation time and never transfers to a replacement feature.
#include "renderer_effect_sources.hpp"
#include "oa/test/check.hpp"

using namespace oa::app::renderer_effect_sources;

int main() {
    OA_CHECK(construction_pulse(30) > construction_pulse(0));
    OA_CHECK(construction_pulse(0) == construction_pulse(60));
    for (uint32_t tick = 0; tick < 120; ++tick) {
        OA_CHECK(construction_pulse(tick) >= .849F);
        OA_CHECK(construction_pulse(tick) <= 1.151F);
    }
    Timeline timeline;
    OA_CHECK(observe(timeline, 100, 1));
    OA_CHECK(wreck_heat(timeline, 0, true, true) == 0); // loaded wrecks are cold
    OA_CHECK(!observe(timeline, 110, 2));
    OA_CHECK(wreck_heat(timeline, 1, true, true) == 1);
    OA_CHECK(!observe(timeline, 110, 2)); // pause cannot age or restart heat
    OA_CHECK(wreck_heat(timeline, 1, true, true) == 1);
    OA_CHECK(!observe(timeline, 560, 2));
    OA_CHECK(wreck_heat(timeline, 1, true, false) == 0);
    OA_CHECK(wreck_heat(timeline, 1, true, true) == .25F);
    OA_CHECK(wreck_heat(timeline, 1, false, false) == 0);
    OA_CHECK(wreck_heat(timeline, 1, true, true) == 0); // same feature id reused
    OA_CHECK(!observe(timeline, 561, 3));
    OA_CHECK(wreck_heat(timeline, 2, true, true) == 1); // new corpse gets its own age
    OA_CHECK(!observe(timeline, 1461, 3));
    OA_CHECK(wreck_heat(timeline, 2, true, true) == 0);
    OA_CHECK(observe(timeline, 50, 3));
    OA_CHECK(wreck_heat(timeline, 2, true, true) == 0);
    OA_CHECK(observe(timeline, 51, 0));
    OA_CHECK(fade(100, 99, 6) == 0);
    OA_CHECK(fade(100, 103, 6) == .5F);
    OA_CHECK(fade(100, 106, 6) == 0);
    return oa::test::check_exit_status();
}
