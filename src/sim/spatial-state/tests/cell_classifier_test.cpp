// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/spatial_state/cell_classifier.hpp"
#include "oa/sim/spatial_state/movement_footprint.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace spatial = oa::sim::spatial_state;

void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}

namespace {

/// Aggregates the original plot calls under the static footprint's bounds.
uint8_t ordered_footprint(
    const spatial::CellClassifier& movement, int32_t x, int32_t z, const spatial::World& world
) {
    if (x < 0 || z < 0 || movement.footprint_x <= 0 || movement.footprint_z <= 0 ||
        world.terrain_width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        world.terrain_height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        uint64_t{world.terrain_width} * world.terrain_height != world.plots.size() ||
        uint64_t{static_cast<uint32_t>(x)} + static_cast<uint32_t>(movement.footprint_x) >=
            world.terrain_width ||
        uint64_t{static_cast<uint32_t>(z)} + static_cast<uint32_t>(movement.footprint_z) >=
            world.terrain_height)
        return 0;
    uint8_t result = 3;
    for (int32_t row = 0; row < movement.footprint_z; ++row)
        for (int32_t column = 0; column < movement.footprint_x; ++column) {
            const auto plot = spatial::classify_movement_plot(movement, x + column, z + row, world);
            if (plot == 0)
                return 0;
            if (plot == 1)
                result = 1;
        }
    return result;
}

/// Restores clear level plots without changing the world's dimensions or units.
void clear_plots(spatial::World& world, uint8_t height = 20) {
    for (auto& plot : world.plots) {
        plot = {};
        plot.low_height = plot.high_height = height;
    }
}

/// Checks the full class and its boolean against the ordered plot oracle.
void compare_footprint(
    const spatial::CellClassifier& movement, int32_t x, int32_t z, const spatial::World& world
) {
    const auto expected = ordered_footprint(movement, x, z, world);
    const auto actual = spatial::classify_movement_footprint(movement, x, z, world);
    require(actual == expected, "footprint class differs from ordered plot calls");
    require((actual != 0) == (expected != 0), "footprint passability differs from ordered plots");
}

/// Keeps scanning after a slow plot so a later blocked plot still wins.
void test_footprint_order_and_perimeter() {
    spatial::World world;
    world.terrain_width = world.terrain_height = 8;
    world.plots.resize(64);
    world.sea_level = 20;
    clear_plots(world);
    spatial::CellClassifier movement{2, 2, 10, -10, 6, 3, 8, 4, 70};
    world.plots[2 * 8 + 2].high_height = 24;
    world.plots[3 * 8 + 3].blocking_feature = true;
    require(
        spatial::classify_movement_plot(movement, 2, 2, world) == 1,
        "the first footprint plot is slow"
    );
    require(
        spatial::classify_movement_footprint(movement, 2, 2, world) == 0,
        "a later blocked plot overrides the first slow plot"
    );
    compare_footprint(movement, 2, 2, world);
    world.plots[3 * 8 + 3].blocking_feature = false;
    require(
        spatial::classify_movement_footprint(movement, 2, 2, world) == 1,
        "a wholly passable footprint retains its slow class"
    );
    clear_plots(world);
    world.plots[1 * 8 + 2].blocking_feature = true;
    require(
        spatial::classify_movement_footprint(movement, 2, 2, world) == 3,
        "a perimeter obstacle does not enter the footprint class"
    );
    require(
        spatial::classify_movement_cell(movement, 2, 2, world) == 1,
        "the existing full cell still considers its perimeter"
    );
    require(
        spatial::classify_movement_footprint(movement, 0, 2, world) == 3,
        "a missing perimeter does not slow the actual footprint"
    );
    clear_plots(world);
    require(
        spatial::classify_movement_footprint(movement, 5, 5, world) == 3,
        "a footprint before the last row and column is clear"
    );
    require(
        spatial::classify_movement_footprint(movement, 6, 5, world) == 0 &&
            spatial::classify_movement_footprint(movement, 5, 6, world) == 0,
        "the static footprint keeps the strict outer-row bound"
    );
}

/// Preserves object-age, missing-object and invalid-occupant rules.
void test_footprint_occupants() {
    spatial::World world;
    world.terrain_width = world.terrain_height = 8;
    world.plots.resize(64);
    world.sea_level = 20;
    clear_plots(world);
    std::array<spatial::Unit, 3> units{};
    world.units = units;
    auto& plot = world.plots[3 * 8 + 3];
    spatial::CellClassifier movement{2, 2, 10, -10, 6, 3, 8, 4, 70};
    plot.ground = 1;
    require(
        spatial::classify_movement_footprint(movement, 2, 2, world) == 0,
        "an occupant without movement state blocks the footprint"
    );
    units[1].object_present = true;
    for (const uint32_t tick : {0u, 69u, 70u, 71u, std::numeric_limits<uint32_t>::max()}) {
        units[1].object_tick = tick;
        compare_footprint(movement, 2, 2, world);
        require(
            spatial::classify_movement_footprint(movement, 2, 2, world) == (tick < 70 ? 0 : 3),
            "footprint occupants use the unchanged unsigned age comparison"
        );
    }
    movement.occupancy_before_tick = 0;
    units[1].object_tick = 0;
    require(
        spatial::classify_movement_footprint(movement, 2, 2, world) == 3,
        "a zero occupancy cutoff ignores an existing movement object"
    );
    units[1].object_present = false;
    require(
        spatial::classify_movement_footprint(movement, 2, 2, world) == 0,
        "a zero cutoff does not ignore a missing movement object"
    );
    for (const auto occupant : {uint32_t{3}, std::numeric_limits<uint32_t>::max()}) {
        plot.ground = occupant;
        compare_footprint(movement, 2, 2, world);
        require(
            spatial::classify_movement_footprint(movement, 2, 2, world) == 0,
            "an out-of-range occupant blocks the footprint"
        );
    }
    world.units = {};
    plot.ground = 1;
    require(
        spatial::classify_movement_footprint(movement, 2, 2, world) == 0,
        "a missing occupant span blocks an occupied plot"
    );
    plot.ground = spatial::no_unit;
    require(
        spatial::classify_movement_footprint(movement, 2, 2, world) == 3,
        "an empty occupant span permits clear ground"
    );
}

/// Exercises exact depth and slope thresholds, including wrapped height differences.
void test_footprint_terrain_thresholds() {
    spatial::World world;
    world.terrain_width = world.terrain_height = 8;
    world.plots.resize(64);
    world.sea_level = 20;
    spatial::CellClassifier movement{1, 1, 10, -10, 6, 3, 8, 4, 70};

    struct TerrainCase {
        uint8_t low{}, high{}, expected{};
    };

    for (const auto terrain : {
             TerrainCase{20, 23, 3},
             TerrainCase{20, 24, 1},
             TerrainCase{20, 26, 1},
             TerrainCase{20, 27, 0},
             TerrainCase{19, 23, 3},
             TerrainCase{19, 24, 1},
             TerrainCase{19, 27, 1},
             TerrainCase{19, 28, 0},
             TerrainCase{10, 10, 3},
             TerrainCase{9, 9, 0},
             TerrainCase{30, 30, 3},
             TerrainCase{30, 31, 0},
             TerrainCase{20, 19, 0},
         }) {
        clear_plots(world);
        auto& plot = world.plots[2 * 8 + 2];
        plot.low_height = terrain.low;
        plot.high_height = terrain.high;
        compare_footprint(movement, 2, 2, world);
        require(
            spatial::classify_movement_footprint(movement, 2, 2, world) == terrain.expected,
            "the footprint preserves a terrain threshold"
        );
    }
    for (const uint8_t sea : {uint8_t{0}, uint8_t{20}, uint8_t{255}}) {
        world.sea_level = sea;
        for (const int16_t depth :
             {std::numeric_limits<int16_t>::min(),
              int16_t{-1},
              int16_t{0},
              int16_t{1},
              std::numeric_limits<int16_t>::max()}) {
            movement.max_water_depth = depth;
            movement.min_water_depth = static_cast<int16_t>(-int32_t(depth) - 1);
            for (const uint8_t low : {uint8_t{0}, uint8_t{19}, uint8_t{20}, uint8_t{255}}) {
                clear_plots(world, low);
                compare_footprint(movement, 2, 2, world);
            }
        }
    }
}

/// Rejects invalid shape, size and coordinate inputs before touching plots.
void test_footprint_malformed_bounds() {
    spatial::World world;
    world.terrain_width = world.terrain_height = 8;
    world.plots.resize(64);
    world.sea_level = 20;
    clear_plots(world);
    spatial::CellClassifier movement{1, 1, 10, -10, 6, 3, 8, 4, 70};
    for (const auto coordinate :
         {std::numeric_limits<int32_t>::min(), -1, 7, 8, std::numeric_limits<int32_t>::max()}) {
        compare_footprint(movement, coordinate, 2, world);
        compare_footprint(movement, 2, coordinate, world);
        require(
            spatial::classify_movement_footprint(movement, coordinate, 2, world) == 0 &&
                spatial::classify_movement_footprint(movement, 2, coordinate, world) == 0,
            "an invalid footprint origin is blocked"
        );
    }
    for (const int16_t size :
         {std::numeric_limits<int16_t>::min(),
          int16_t{-1},
          int16_t{0},
          std::numeric_limits<int16_t>::max()}) {
        movement.footprint_x = size;
        compare_footprint(movement, 2, 2, world);
        require(
            spatial::classify_movement_footprint(movement, 2, 2, world) == 0,
            "an invalid footprint width is blocked"
        );
        movement.footprint_x = 1;
        movement.footprint_z = size;
        compare_footprint(movement, 2, 2, world);
        require(
            spatial::classify_movement_footprint(movement, 2, 2, world) == 0,
            "an invalid footprint depth is blocked"
        );
        movement.footprint_z = 1;
    }
    for (const size_t count : {size_t{0}, size_t{63}, size_t{65}}) {
        world.plots.resize(count);
        compare_footprint(movement, 2, 2, world);
        require(
            spatial::classify_movement_footprint(movement, 2, 2, world) == 0,
            "a plot span inconsistent with the dimensions is blocked"
        );
    }
    world.plots.clear();
    for (const auto dimensions :
         {std::array<uint32_t, 2>{0, 0},
          {0, 8},
          {8, 0},
          {std::numeric_limits<uint32_t>::max(), 0},
          {0, std::numeric_limits<uint32_t>::max()},
          {std::numeric_limits<uint32_t>::max(), 2}}) {
        world.terrain_width = dimensions[0];
        world.terrain_height = dimensions[1];
        compare_footprint(movement, 0, 0, world);
        require(
            spatial::classify_movement_footprint(movement, 0, 0, world) == 0,
            "empty or oversized dimensions are blocked"
        );
    }
}

/// Produces deterministic test patterns without using any world random stream.
uint32_t next_pattern(uint32_t& state) {
    constexpr uint32_t multiplier = 1'664'525, increment = 1'013'904'223;
    state = state * multiplier + increment;
    return state;
}

/// Compares rectangular footprints from one through eight across varied small worlds.
void test_footprint_differential_patterns() {
    std::array<spatial::Unit, 5> units{};
    units[1].object_present = false;
    units[2].object_present = units[3].object_present = units[4].object_present = true;
    units[2].object_tick = 69;
    units[3].object_tick = 70;
    units[4].object_tick = 71;
    spatial::World world;
    world.terrain_width = 18;
    world.terrain_height = 17;
    world.plots.resize(18 * 17);
    world.units = units;
    world.tick = 12'345;
    uint32_t pattern = 0x7c3142a9;
    for (uint32_t variant = 0; variant < 8; ++variant) {
        world.sea_level = variant < 3 ? 20 : static_cast<uint8_t>(next_pattern(pattern));
        for (auto& plot : world.plots) {
            plot = {};
            const auto bits = next_pattern(pattern);
            plot.blocking_feature = variant >= 3 && bits % 97 == 0;
            plot.ground = variant >= 3 && bits % 31 == 0 ? (bits >> 8) % 7 : spatial::no_unit;
            plot.low_height = variant < 3 ? world.sea_level : static_cast<uint8_t>(bits >> 16);
            const auto slope = variant == 0 ? 0 : variant == 1 ? 4 : bits % (variant == 2 ? 7 : 11);
            plot.high_height = static_cast<uint8_t>(plot.low_height + slope);
        }
        spatial::CellClassifier movement{
            1,
            1,
            std::numeric_limits<int16_t>::max(),
            std::numeric_limits<int16_t>::min(),
            6,
            3,
            8,
            4,
            variant % 2 == 0 ? uint32_t{70} : uint32_t{0}
        };
        for (int16_t width = 1; width <= 8; ++width)
            for (int16_t depth = 1; depth <= 8; ++depth) {
                movement.footprint_x = width;
                movement.footprint_z = depth;
                for (int32_t z = -1; z <= 18; ++z)
                    for (int32_t x = -1; x <= 19; ++x)
                        compare_footprint(movement, x, z, world);
            }
        require(world.tick == 12'345, "footprint classification leaves the world clock unchanged");
    }
}

} // namespace

int main() {
    try {
        test_footprint_order_and_perimeter();
        test_footprint_occupants();
        test_footprint_terrain_thresholds();
        test_footprint_malformed_bounds();
        test_footprint_differential_patterns();
        std::vector<spatial::Unit> units(3);
        units[1].id = 1;
        units[2].id = 2;
        spatial::World world;
        world.terrain_width = 8;
        world.terrain_height = 8;
        world.plots.resize(64);
        world.units = units;
        world.sea_level = 20;
        for (auto& plot : world.plots) {
            plot.low_height = 20;
            plot.high_height = 20;
        }
        spatial::CellClassifier movement{2, 2, 10, -10, 6, 3, 8, 4, 70};
        require(
            spatial::classify_movement_cell(movement, 2, 2, world) == 3, "clear footprint/perimeter"
        );
        world.plots[2 * 8 + 2].high_height = 24;
        require(
            spatial::classify_movement_cell(movement, 2, 2, world) == 1, "preferred land slope band"
        );
        world.plots[2 * 8 + 2].high_height = 27;
        require(spatial::classify_movement_cell(movement, 2, 2, world) == 0, "maximum land slope");
        world.plots[2 * 8 + 2].high_height = 20;
        world.plots[1 * 8 + 2].high_height = 24;
        require(
            spatial::classify_movement_cell(movement, 2, 2, world) == 1, "perimeter degradation"
        );
        world.plots[1 * 8 + 2].high_height = 20;
        world.plots[2 * 8 + 2].ground = 1;
        units[1].object_present = true;
        units[1].object_tick = 69;
        require(spatial::classify_movement_cell(movement, 2, 2, world) == 0, "old occupant blocks");
        units[1].object_tick = 70;
        require(
            spatial::classify_movement_cell(movement, 2, 2, world) == 3,
            "current occupant is ignored"
        );
        units[1].object_present = false;
        require(
            spatial::classify_movement_cell(movement, 2, 2, world) == 0,
            "occupant without object blocks"
        );
        world.plots[2 * 8 + 2].ground = 0;
        world.plots[2 * 8 + 2].blocking_feature = true;
        require(spatial::classify_movement_cell(movement, 2, 2, world) == 0, "blocking feature");
        world.plots[2 * 8 + 2].blocking_feature = false;
        require(
            spatial::classify_movement_cell(movement, 0, 2, world) == 1,
            "missing perimeter is class one"
        );
        require(
            spatial::classify_movement_cell(movement, 7, 2, world) == 0, "footprint outside map"
        );
        world.plots[2 * 8 + 2].low_height = 19;
        world.plots[2 * 8 + 2].high_height = 24;
        require(
            spatial::classify_movement_cell(movement, 2, 2, world) == 1,
            "preferred underwater slope band"
        );
        world.plots[2 * 8 + 2].high_height = 28;
        require(
            spatial::classify_movement_cell(movement, 2, 2, world) == 0, "maximum underwater slope"
        );

        // Single-plot class used by the whole-map build.
        auto& plot = world.plots[2 * 8 + 2];
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 0,
            "plot: maximum underwater slope"
        );
        plot.high_height = 26;
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 1,
            "plot: underwater slope band"
        );
        plot.high_height = 23;
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 3,
            "plot: preferred underwater slope"
        );
        plot.low_height = 20;
        plot.high_height = 24;
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 1, "plot: land slope band"
        );
        plot.high_height = 27;
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 0, "plot: maximum land slope"
        );
        plot.high_height = 20;
        require(spatial::classify_movement_plot(movement, 2, 2, world) == 3, "plot: clear");
        plot.low_height = 9;
        require(spatial::classify_movement_plot(movement, 2, 2, world) == 0, "plot: too deep");
        plot.low_height = 20;
        plot.high_height = 31;
        require(spatial::classify_movement_plot(movement, 2, 2, world) == 0, "plot: too shallow");
        plot.high_height = 20;
        plot.ground = 1;
        units[1].object_present = true;
        units[1].object_tick = 69;
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 0, "plot: old occupant blocks"
        );
        units[1].object_tick = 70;
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 3,
            "plot: recent occupant is ignored"
        );
        plot.ground = 2;
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 0,
            "plot: occupant without object blocks"
        );
        plot.ground = 0;
        plot.blocking_feature = true;
        require(
            spatial::classify_movement_plot(movement, 2, 2, world) == 0, "plot: blocking feature"
        );
        plot.blocking_feature = false;
        require(spatial::classify_movement_plot(movement, 8, 2, world) == 0, "plot: outside map");
        require(
            spatial::classify_movement_plot(movement, -1, 2, world) == 0, "plot: negative cell"
        );
        std::cout << "cell classifier tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
