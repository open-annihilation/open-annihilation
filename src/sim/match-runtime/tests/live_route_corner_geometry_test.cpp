// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Visible-corner advancement uses actual fixed-point footprint origins,
// preflights bounded work and preserves the original route on every failure.
#include "live_route_corner.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace {
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;

constexpr int32_t fixed_one = 65536;
constexpr uint64_t previous_probes = 17;
using Origin = std::array<int16_t, 2>;

/// Converts an origin cell to its native whole-world footprint centre.
int16_t centre(int16_t cell, int16_t footprint) {
    return static_cast<int16_t>(cell * 16 + (footprint - 1) * 8);
}

struct Fixture {
    ground::Goal goal{};
    movement::Unit unit{};
    ground::Navigation navigation{};

    explicit Fixture(int16_t width = 2, int16_t height = 2) {
        unit.footprint = {width, height};
        unit.cell = {10, 10};
        const auto x = centre(10, width), z = centre(10, height);
        unit.position = {int32_t(x) * fixed_one, 0, int32_t(z) * fixed_one};
        navigation.goal = &goal;
        navigation.count = 4;
        navigation.last_search_tick = 123456789;
        navigation.flags = ground::route_present_flag | ground::search_pending_flag |
                           ground::route_sent_blocked_flag;
        for (size_t at = 0; at < navigation.points.size(); ++at)
            navigation.points[at] = {
                static_cast<int16_t>(1000 + at * 7), static_cast<int16_t>(2000 + at * 3)
            };
        navigation.points[0] = {static_cast<int16_t>(x - 128), static_cast<int16_t>(z - 128)};
        navigation.points[1] = {x, static_cast<int16_t>(z + 64)};
        navigation.points[2] = {static_cast<int16_t>(x + 64), static_cast<int16_t>(z + 64)};
        navigation.points[3] = {static_cast<int16_t>(x + 128), static_cast<int16_t>(z + 64)};
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

struct Terrain {
    int16_t width{2}, height{2};
    std::optional<Origin> blocked{}, unknown{};
    std::vector<Origin> origins{}, physical_cells{};

    /// Checks all mapped cells before reading any physical terrain in a footprint.
    bool operator()(int16_t x, int16_t z) {
        origins.push_back({x, z});
        for (int16_t dz = 0; dz < height; ++dz)
            for (int16_t dx = 0; dx < width; ++dx)
                if (unknown == Origin{static_cast<int16_t>(x + dx), static_cast<int16_t>(z + dz)})
                    return false;
        for (int16_t dz = 0; dz < height; ++dz)
            for (int16_t dx = 0; dx < width; ++dx) {
                const Origin at{static_cast<int16_t>(x + dx), static_cast<int16_t>(z + dz)};
                physical_cells.push_back(at);
                if (blocked == at)
                    return false;
            }
        return true;
    }
};

/// Retains every native byte, including unused route points and padding.
std::array<uint8_t, sizeof(ground::Navigation)> bytes_of(const ground::Navigation& navigation) {
    std::array<uint8_t, sizeof(ground::Navigation)> bytes{};
    std::memcpy(bytes.data(), &navigation, bytes.size());
    return bytes;
}

/// Calls the production helper with the fixture's matching classifier footprint.
bool advance(Fixture& fixture, Terrain& terrain, uint64_t& probes) {
    return runtime::try_advance_visible_corner(
        fixture.unit, fixture.navigation, terrain.width, terrain.height, terrain, probes
    );
}

/// Removes one corner and leaves every mission, retry and trailing point intact.
void test_exact_route_mutation_and_diagonal_origins() {
    for (const uint32_t count : {3u, 4u, uint32_t(ground::navigation_capacity)}) {
        Fixture fixture;
        fixture.navigation.count = count;
        const auto before = fixture.navigation;
        Terrain terrain;
        uint64_t probes = previous_probes;
        OA_CHECK(advance(fixture, terrain, probes));
        const std::vector<Origin> expected{
            {10, 10},
            {11, 10},
            {10, 11},
            {11, 11},
            {12, 11},
            {11, 12},
            {12, 12},
            {13, 12},
            {12, 13},
            {13, 13},
            {14, 13},
            {13, 14},
            {14, 14}
        };
        OA_CHECK(terrain.origins == expected);
        OA_CHECK(probes == previous_probes + expected.size());
        OA_CHECK(fixture.navigation.goal == before.goal);
        OA_CHECK(fixture.navigation.last_search_tick == before.last_search_tick);
        OA_CHECK(fixture.navigation.flags == (before.flags | ground::route_changed_flag));
        OA_CHECK(fixture.navigation.count == count - 1);
        OA_CHECK(fixture.navigation.points[0] == (ground::RoutePoint{168, 168}));
        for (uint32_t at = 1; at + 1 < count; ++at)
            OA_CHECK(fixture.navigation.points[at] == before.points[at + 1]);
        for (size_t at = count - 1; at < before.points.size(); ++at)
            OA_CHECK(fixture.navigation.points[at] == before.points[at]);
    }
}

/// Preserves subcell start geometry rather than treating the old P0 as current.
void test_fractional_pose_and_strict_noncollinearity() {
    Fixture fixture;
    fixture.unit.position[0] += 15 * fixed_one;
    fixture.unit.position[2] += fixed_one;
    Terrain terrain;
    uint64_t probes{};
    OA_CHECK(advance(fixture, terrain, probes));
    const std::vector<Origin> expected{
        {10, 10},
        {11, 10},
        {11, 11},
        {12, 11},
        {12, 12},
        {13, 12},
        {13, 13},
        {14, 13},
        {13, 14},
        {14, 14}
    };
    OA_CHECK(terrain.origins == expected);
    OA_CHECK(fixture.navigation.points[0] == (ground::RoutePoint{183, 169}));

    Fixture subpixel;
    subpixel.unit.position[2] += 1;
    subpixel.navigation.points[1] = {200, 168};
    subpixel.navigation.points[2] = {232, 168};
    Terrain fine;
    probes = 0;
    OA_CHECK(advance(subpixel, fine, probes));
    const std::vector<Origin> horizontal{{10, 10}, {11, 10}, {12, 10}, {13, 10}, {14, 10}};
    OA_CHECK(fine.origins == horizontal);
}

/// Refuses diagonal side blockers and unknown footprint cells without route writes.
void test_obstacle_and_fog_fail_atomically() {
    for (const Origin block : {Origin{12, 10}, Origin{10, 12}}) {
        Fixture fixture;
        const auto before = bytes_of(fixture.navigation);
        Terrain terrain;
        terrain.blocked = block;
        uint64_t probes = previous_probes;
        OA_CHECK(!advance(fixture, terrain, probes));
        OA_CHECK(bytes_of(fixture.navigation) == before);
        OA_CHECK(probes == previous_probes + terrain.origins.size());
        OA_CHECK(terrain.origins.size() > 1);
    }
    for (const Origin fog : {Origin{11, 10}, Origin{12, 10}, Origin{10, 12}}) {
        Fixture fixture;
        const auto before = bytes_of(fixture.navigation);
        Terrain terrain;
        terrain.unknown = fog;
        uint64_t probes{};
        OA_CHECK(!advance(fixture, terrain, probes));
        OA_CHECK(bytes_of(fixture.navigation) == before);
        for (const auto cell : terrain.physical_cells)
            OA_CHECK(cell != fog);
        if (fog == Origin{11, 10})
            OA_CHECK(terrain.physical_cells.empty());
    }
}

/// Creates a genuine corner whose direct shortcut has a selected cell displacement.
void configure_span(Fixture& fixture, int16_t cells_x, int16_t cells_z) {
    const auto x = centre(0, fixture.unit.footprint[0]);
    const auto z = centre(0, fixture.unit.footprint[1]);
    fixture.unit.position = {int32_t(x) * fixed_one, 0, int32_t(z) * fixed_one};
    fixture.unit.cell = {0, 0};
    fixture.navigation.points[1] = {x, static_cast<int16_t>(z + 16)};
    fixture.navigation.points[2] = {
        static_cast<int16_t>(x + cells_x * 16), static_cast<int16_t>(z + cells_z * 16)
    };
}

/// Bounds both origin visits and actual full-footprint terrain work before queries.
void test_origin_and_rectangular_footprint_budgets() {
    for (const auto footprint :
         {Origin{1, 1}, Origin{2, 2}, Origin{2, 8}, Origin{8, 2}, Origin{8, 8}}) {
        const auto area = uint32_t(footprint[0]) * uint32_t(footprint[1]);
        const auto limit = std::min(
            runtime::live_route_corner_maximum_origins,
            runtime::live_route_corner_maximum_plots / area
        );
        for (const bool over : {false, true}) {
            Fixture fixture(footprint[0], footprint[1]);
            configure_span(fixture, static_cast<int16_t>(limit - (over ? 0 : 1)), 0);
            const auto before = bytes_of(fixture.navigation);
            Terrain terrain;
            terrain.width = footprint[0];
            terrain.height = footprint[1];
            uint64_t probes = previous_probes;
            const auto changed = advance(fixture, terrain, probes);
            OA_CHECK(changed == !over);
            if (over) {
                OA_CHECK(bytes_of(fixture.navigation) == before);
                OA_CHECK(terrain.origins.empty() && terrain.physical_cells.empty());
                OA_CHECK(probes == previous_probes);
            } else {
                OA_CHECK(terrain.origins.size() == limit);
                OA_CHECK(terrain.physical_cells.size() == limit * area);
                OA_CHECK(terrain.physical_cells.size() <= runtime::live_route_corner_maximum_plots);
                OA_CHECK(probes == previous_probes + limit);
            }
        }
        // The Manhattan lower bound fits here, but diagonal side origins can
        // still exceed the exact budget. No sampler may run before that count.
        Fixture diagonal(footprint[0], footprint[1]);
        const auto ties = static_cast<int16_t>((limit - 1) / 3 + 1);
        configure_span(diagonal, ties, ties);
        const auto before = bytes_of(diagonal.navigation);
        Terrain terrain;
        terrain.width = footprint[0];
        terrain.height = footprint[1];
        uint64_t probes = previous_probes;
        OA_CHECK(!advance(diagonal, terrain, probes));
        OA_CHECK(bytes_of(diagonal.navigation) == before);
        OA_CHECK(terrain.origins.empty() && terrain.physical_cells.empty());
        OA_CHECK(probes == previous_probes);
    }
}

/// Leaves unsupported routes, classifiers and degenerate bends entirely untouched.
void test_invalid_inputs_make_no_queries() {
    for (unsigned failure = 0; failure < 11; ++failure) {
        Fixture fixture;
        Terrain terrain;
        switch (failure) {
        case 0:
            fixture.navigation.goal = nullptr;
            break;
        case 1:
            fixture.navigation.flags &= uint8_t(~ground::route_present_flag);
            break;
        case 2:
            fixture.navigation.count = 2;
            break;
        case 3:
            fixture.navigation.count = ground::navigation_capacity + 1;
            break;
        case 4:
            terrain.width = 0;
            break;
        case 5:
            terrain.height = -1;
            break;
        case 6:
            terrain.width = 9;
            fixture.unit.footprint[0] = 9;
            break;
        case 7:
            terrain.width = 1;
            break;
        case 8:
            fixture.navigation.points[1] = fixture.navigation.points[2];
            break;
        case 9:
            fixture.navigation.points[1] = {200, 168};
            fixture.navigation.points[2] = {232, 168};
            break;
        case 10:
            fixture.navigation.points[1] = {168, 168};
            break;
        }
        const auto before = bytes_of(fixture.navigation);
        uint64_t probes = previous_probes;
        OA_CHECK(!advance(fixture, terrain, probes));
        OA_CHECK(bytes_of(fixture.navigation) == before);
        OA_CHECK(terrain.origins.empty() && probes == previous_probes);
    }
}

/// Covers wide old-corner vectors and refuses native bias wrap before queries.
void test_extreme_fixed_coordinates() {
    for (const bool high : {false, true}) {
        Fixture fixture(1, 1);
        fixture.unit.position = high ? std::array<int32_t, 3>{INT32_MAX, 0, INT32_MAX}
                                     : std::array<int32_t, 3>{INT32_MIN, 0, INT32_MIN};
        fixture.navigation.points[1] = high ? ground::RoutePoint{INT16_MIN, INT16_MIN}
                                            : ground::RoutePoint{INT16_MAX, INT16_MAX};
        fixture.navigation.points[2] =
            high ? ground::RoutePoint{32751, 32767} : ground::RoutePoint{-32752, -32768};
        Terrain terrain;
        terrain.width = terrain.height = 1;
        uint64_t probes{};
        OA_CHECK(advance(fixture, terrain, probes));
        OA_CHECK(probes <= runtime::live_route_corner_maximum_origins);
    }
    for (const bool target_wrap : {false, true}) {
        Fixture fixture(8, 8);
        if (target_wrap)
            fixture.navigation.points[2][0] = INT16_MIN;
        else
            fixture.unit.position[0] = INT32_MIN;
        const auto before = bytes_of(fixture.navigation);
        Terrain terrain;
        terrain.width = terrain.height = 8;
        uint64_t probes = previous_probes;
        OA_CHECK(!advance(fixture, terrain, probes));
        OA_CHECK(bytes_of(fixture.navigation) == before);
        OA_CHECK(terrain.origins.empty() && probes == previous_probes);
    }
    Fixture wide(1, 1);
    wide.unit.position = {INT32_MIN, 0, INT32_MIN};
    wide.navigation.points[1] = {INT16_MIN, INT16_MAX};
    wide.navigation.points[2] = {INT16_MAX, INT16_MAX};
    Terrain terrain;
    terrain.width = terrain.height = 1;
    const auto before = bytes_of(wide.navigation);
    uint64_t probes = previous_probes;
    OA_CHECK(!advance(wide, terrain, probes));
    OA_CHECK(bytes_of(wide.navigation) == before);
    OA_CHECK(terrain.origins.empty() && probes == previous_probes);
}

/// Lets a blocked corner check spend more than 64 plots on its default budget.
void test_default_plot_budget() {
    Fixture original;
    configure_span(original, 16, 0);
    Terrain terrain;
    uint64_t probes{};
    OA_CHECK(advance(original, terrain, probes));
    OA_CHECK(terrain.physical_cells.size() == 68);
}

/// Exercises the real phase helper across the full unit cohort and tick wrap.
void test_phase_and_maturity() {
    for (uint16_t unit = 0; unit < 20000; ++unit) {
        uint32_t due{};
        for (uint32_t tick = 0; tick < runtime::live_route_corner_period; ++tick) {
            due += runtime::live_route_corner_due(30, tick, unit);
            OA_CHECK(!runtime::live_route_corner_due(29, tick, unit));
        }
        OA_CHECK(due == 1);
    }
    OA_CHECK(runtime::live_route_corner_due(31, 118, 2));
    OA_CHECK(!runtime::live_route_corner_due(31, 119, 2));
    constexpr auto last_tick = std::numeric_limits<uint32_t>::max();
    constexpr auto offset = static_cast<uint16_t>(
        runtime::live_route_corner_period - last_tick % runtime::live_route_corner_period
    );
    OA_CHECK(runtime::live_route_corner_due(30, last_tick, offset));
}
} // namespace

int main() {
    test_exact_route_mutation_and_diagonal_origins();
    test_fractional_pose_and_strict_noncollinearity();
    test_obstacle_and_fog_fail_atomically();
    test_origin_and_rectangular_footprint_budgets();
    test_invalid_inputs_make_no_queries();
    test_extreme_fixed_coordinates();
    test_default_plot_budget();
    test_phase_and_maturity();
    return oa::test::check_exit_status();
}
