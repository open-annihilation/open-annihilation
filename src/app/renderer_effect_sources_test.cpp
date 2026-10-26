// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Cooling uses simulation time and never transfers to a replacement feature.
#include "renderer_effect_sources.hpp"
#include "oa/test/check.hpp"

#include <array>

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
    // The first wreck that may still glow: past the loaded, retired and
    // cooled ones, never before one still cooling.
    Timeline ages;
    OA_CHECK(observe(ages, 100, 2));
    OA_CHECK(first_live_wreck(ages) == 2);
    OA_CHECK(!observe(ages, 200, 4));
    OA_CHECK(first_live_wreck(ages) == 2);
    OA_CHECK(wreck_heat(ages, 2, false, true) == 0);
    OA_CHECK(first_live_wreck(ages) == 3);
    OA_CHECK(!observe(ages, 300, 5));
    OA_CHECK(first_live_wreck(ages) == 3);
    OA_CHECK(!observe(ages, 1100, 5));
    OA_CHECK(first_live_wreck(ages) == 4);
    OA_CHECK(wreck_heat(ages, 4, true, true) > 0);
    OA_CHECK(!observe(ages, 1200, 5));
    OA_CHECK(first_live_wreck(ages) == 5);
    OA_CHECK(observe(ages, 10, 5));
    OA_CHECK(first_live_wreck(ages) == 5);
    OA_CHECK(fade(100, 99, 6) == 0);
    OA_CHECK(fade(100, 103, 6) == .5F);
    OA_CHECK(fade(100, 106, 6) == 0);

    // A weapon lights in the colour its shot is drawn in. The commanders'
    // laser colour 232 is red in the interface palette, which its beam is
    // drawn in, and pale green in the battlefield palette's entry 232.
    std::array<uint8_t, 1024> palette{};
    std::array<uint8_t, 256> interface_colours{};
    const auto entry = [&](uint8_t index, uint8_t red, uint8_t green, uint8_t blue) {
        palette[index * 4U] = red;
        palette[index * 4U + 1] = green;
        palette[index * 4U + 2] = blue;
    };
    constexpr uint8_t commander_laser = 232;
    constexpr uint8_t lightning = 208;
    constexpr uint8_t dark = 5;
    entry(commander_laser, 123, 255, 119);
    entry(40, 211, 43, 0);
    entry(50, 179, 151, 63);
    entry(60, 20, 20, 20);
    interface_colours[commander_laser] = 40;
    interface_colours[lightning] = 50;
    interface_colours[dark] = 60;
    const auto lit = [&](uint8_t color, uint8_t render_type) {
        Source source;
        weapon_colour(source, color, render_type, palette, interface_colours);
        return std::array<float, 3>{source.red, source.green, source.blue};
    };
    const auto rgb = [](uint8_t red, uint8_t green, uint8_t blue) {
        return std::array<float, 3>{red / 255.0F, green / 255.0F, blue / 255.0F};
    };
    const std::array<float, 3> warm{1, .58F, .18F};
    constexpr uint8_t laser = 0, missile = 1, plasma = 4, bolt = 7;
    OA_CHECK(lit(commander_laser, laser) == rgb(211, 43, 0));
    OA_CHECK(lit(lightning, bolt) == rgb(179, 151, 63));
    // Plasma's colour picks its sprite, and a model shot is not drawn in one.
    OA_CHECK(lit(1, plasma) == warm);
    OA_CHECK(lit(commander_laser, missile) == warm);
    OA_CHECK(lit(0, laser) == warm);
    OA_CHECK(lit(dark, laser) == warm);
    Source unloaded;
    weapon_colour(unloaded, commander_laser, laser, {}, {});
    OA_CHECK(unloaded.red == warm[0] && unloaded.green == warm[1] && unloaded.blue == warm[2]);
    return oa::test::check_exit_status();
}
