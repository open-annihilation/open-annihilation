// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The two-planet war map the Main Frame tests load: Core Prime (id 1, Core)
// at map (10, 100) and Empyrrean (id 2, Arm) at (100, 100), one connector
// between them and one 16 x 16 sprite frame anchored at its centre each, on
// a 2400 x 1600 map image. At zoom level 0 (four map units per view pixel)
// Empyrrean's hotspot is the 4 x 4 view rectangle at (23, 23).
#pragma once

#include <cstddef>
#include <cstdint>

#include "oa/boneyards/screens/galaxy.hpp"

namespace oa::boneyards::console::test {

inline constexpr int32_t k_core_prime_id = 1;
inline constexpr int32_t k_empyrrean_id = 2;
inline constexpr int32_t k_empyrrean_view_left = 23;
inline constexpr int32_t k_empyrrean_view_size = 4;

inline screens::PlanetRecord
fixture_planet(int32_t id, const char* name, int32_t x, int32_t y, char side) {
    screens::PlanetRecord r{};
    r.id = id;
    r.name = name;
    r.x = x;
    r.y = y;
    r.sequence = "Planet";
    r.side = side;
    r.ta_map = "Coast to Coast";
    r.ta_mapsource = "TA";
    r.ta_maptype = "Green";
    return r;
}

inline bool load_two_planet_galaxy(screens::GalaxyView* view) {
    const screens::PlanetRecord planets[2] = {
        fixture_planet(k_core_prime_id, "Core Prime", 10, 100, 'C'),
        fixture_planet(k_empyrrean_id, "Empyrrean", 100, 100, 'A')
    };
    const screens::ConnectorRecord link{k_core_prime_id, k_empyrrean_id};
    const screens::SpriteFrame frames[2] = {{16, 16, 8, 8}, {16, 16, 8, 8}};
    const std::size_t counts[2] = {1, 1};
    const screens::PlanetSprites sprites{frames, counts};
    return screens::galaxy_load(view, 2400, 1600, planets, 2, &link, 1, &sprites);
}

} // namespace oa::boneyards::console::test
