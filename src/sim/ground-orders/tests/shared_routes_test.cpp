// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/shared_routes.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <vector>

using namespace oa::sim::ground_orders;

namespace {
int failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

struct Grid {
    uint32_t width, height;
    std::vector<uint8_t> cells;
    uint64_t queries{};

    explicit Grid(uint32_t w, uint32_t h) : width(w), height(h), cells(w * h, 1) {}

    FlowSampler sampler() {
        return {this, [](void* p, uint32_t klass, uint32_t, int32_t x, int32_t z) -> uint8_t {
                    auto& g = *static_cast<Grid*>(p);
                    ++g.queries;
                    const auto footprint = klass + 1;
                    if (x < 0 || z < 0 || static_cast<uint32_t>(x) + footprint > g.width ||
                        static_cast<uint32_t>(z) + footprint > g.height)
                        return 0;
                    for (uint32_t dz = 0; dz < footprint; ++dz)
                        for (uint32_t dx = 0; dx < footprint; ++dx)
                            if (!g.cells
                                     [(static_cast<uint32_t>(z) + dz) * g.width +
                                      static_cast<uint32_t>(x) + dx])
                                return 0;
                    return 1;
                }};
    }
};

oa::Unit unit_at(int16_t x, int16_t z, int16_t footprint = 1) {
    oa::Unit unit{};
    unit.cell_x = x;
    unit.cell_z = z;
    unit.footprint_x = unit.footprint_z = footprint;
    unit.position.x = (x * 16 + footprint * 8) << 16;
    unit.position.z = (z * 16 + footprint * 8) << 16;
    return unit;
}

Goal circle(int16_t x, int16_t z) {
    Goal goal;
    goal.cell = {x, z};
    return goal;
}

FlowContext context(uint32_t klass = 0, uint32_t revision = 0) {
    return {klass, 0, revision, 0, klass + 1, klass + 1};
}

FlowResult query(
    FlowCache& cache,
    Grid& grid,
    const oa::Unit& unit,
    const Goal& goal,
    FlowContext ctx = context()
) {
    std::array<RoutePoint, navigation_capacity> points{};
    uint32_t count{};
    const auto result = cache.route(ctx, unit, goal, points, count, grid.sampler());
    CHECK(
        (result == FlowResult::ready && count >= 2) || (result != FlowResult::ready && count == 0)
    );
    return result;
}

void finish(FlowCache& cache, Grid& grid, uint32_t revision = 0) {
    for (uint32_t tick = 1; tick < 40; ++tick)
        cache.advance(tick, revision, grid.sampler());
}

void static_sampler_matches_full_classifier_passability() {
    namespace spatial = oa::sim::spatial_state;
    spatial::World world;
    world.terrain_width = world.terrain_height = 16;
    world.plots.resize(256);
    std::array<spatial::Unit, 3> units{};
    world.units = units;
    world.sea_level = 20;
    units[1].object_present = true; // mobiles are ignored, including old ones
    units[1].object_tick = 0;
    units[2].object_present = false; // structures still block
    spatial::CellClassifier movement{1, 1, 10, -10, 6, 3, 8, 4, 0};
    for (uint32_t variant = 0; variant < 5; ++variant) {
        for (uint32_t i = 0; i < world.plots.size(); ++i) {
            auto& plot = world.plots[i];
            plot = {};
            plot.low_height = static_cast<uint8_t>(variant == 2 ? 8 + i % 25 : 20);
            plot.high_height = static_cast<uint8_t>(plot.low_height + (variant == 2 ? i % 10 : 0));
            plot.blocking_feature = variant == 1 && i % 17 == 0;
            plot.ground = variant == 3 && i % 19 == 0 ? 1 : variant == 4 && i % 23 == 0 ? 2 : 0;
        }
        for (int16_t width = 1; width <= 4; ++width)
            for (int16_t depth = 1; depth <= 4; ++depth) {
                movement.footprint_x = width;
                movement.footprint_z = depth;
                for (int32_t z = -1; z <= 16; ++z)
                    for (int32_t x = -1; x <= 16; ++x)
                        CHECK(
                            static_flow_passable(movement, x, z, world) ==
                            (spatial::classify_movement_cell(movement, x, z, world) != 0)
                        );
            }
    }
}

void shared_cohort_and_budget() {
    Grid grid(128, 128);
    FlowCache cache(128, 128, {4, 4096, 128});
    const auto goal = circle(112, 112);
    CHECK(query(cache, grid, unit_at(8, 8), goal) == FlowResult::fallback);
    CHECK(query(cache, grid, unit_at(9, 8), goal) == FlowResult::pending);
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        const auto before = cache.counters();
        cache.advance(tick, 0, grid.sampler());
        const auto after = cache.counters();
        CHECK(after.cells_cleared - before.cells_cleared <= 65'536);
        CHECK(after.cells_expanded - before.cells_expanded <= 128);
    }
    finish(cache, grid);
    uint32_t tick = 40;
    for (uint32_t i = 0; i < 1000; ++i) {
        const auto unit =
            unit_at(static_cast<int16_t>(4 + i % 24), static_cast<int16_t>(4 + (i / 24) % 24));
        const auto arrival =
            circle(static_cast<int16_t>(96 + i % 24), static_cast<int16_t>(96 + (i / 24) % 24));
        auto result = query(cache, grid, unit, arrival);
        if (result == FlowResult::pending) {
            cache.advance(++tick, 0, grid.sampler());
            result = query(cache, grid, unit, arrival);
        }
        CHECK(result == FlowResult::ready);
    }
    const auto stats = cache.counters();
    CHECK(stats.fields_started == 1 && stats.hits == 1000);
    CHECK(stats.cells_expanded == 1024 && stats.cells_cleared == 1024);
    // One reverse traversal replaces 1,000 independent traversals of the
    // same 16,384-cell map. Also bound actual edge-classifier calls.
    CHECK(grid.queries < 500000);
    std::printf(
        "shared-cohort: 1000 routes, %llu shared expansions, %llu cell probes\n",
        static_cast<unsigned long long>(stats.cells_expanded),
        static_cast<unsigned long long>(grid.queries)
    );
}

void open_routes_keep_heading_and_individual_destinations() {
    Grid grid(400, 400);
    FlowCache cache(400, 400, {2, 20'000, 2048});
    const auto seed = circle(200, 200);
    (void)query(cache, grid, unit_at(40, 200), seed);
    (void)query(cache, grid, unit_at(41, 200), seed);
    finish(cache, grid);
    constexpr std::array<std::array<int16_t, 2>, 4> starts{
        {{40, 200}, {360, 200}, {200, 40}, {200, 360}}
    };
    for (const auto& start : starts) {
        std::array<RoutePoint, navigation_capacity> points{};
        uint32_t count{};
        CHECK(
            cache.route(
                context(), unit_at(start[0], start[1]), seed, points, count, grid.sampler()
            ) == FlowResult::ready
        );
        CHECK(count == 2);
        CHECK(points[1] == (RoutePoint{3208, 3208}));
    }
    // A neighbour's field must not pull this unit toward the first unit's
    // arrival row when its own horizontal route is completely clear.
    std::array<RoutePoint, navigation_capacity> points{};
    uint32_t count{};
    CHECK(
        cache.route(context(), unit_at(40, 208), circle(216, 208), points, count, grid.sampler()) ==
        FlowResult::ready
    );
    CHECK(count == 2);
    CHECK(points[0][1] == points[1][1]);
    CHECK(points[1] == (RoutePoint{3464, 3336}));
}

void blocked_lane_detours_are_symmetric_and_bounded() {
    const auto measure = [](int16_t from_x, int16_t to_x) {
        Grid grid(128, 128);
        for (uint32_t z = 48; z <= 80; ++z)
            grid.cells[z * 128 + 64] = 0;
        FlowCache cache(128, 128);
        const auto start = unit_at(from_x, 64);
        const auto goal = circle(to_x, 64);
        (void)query(cache, grid, start, goal);
        (void)query(cache, grid, start, goal);
        finish(cache, grid);
        std::array<RoutePoint, navigation_capacity> points{};
        uint32_t count{};
        CHECK(
            cache.route(context(), start, goal, points, count, grid.sampler()) == FlowResult::ready
        );
        double length = 0;
        for (uint32_t i = 0; i < count; ++i) {
            CHECK(std::abs(int32_t{points[i][1]} - 1032) <= 320);
            if (i != 0)
                length += std::hypot(
                    double(points[i][0] - points[i - 1][0]), double(points[i][1] - points[i - 1][1])
                );
        }
        CHECK(length <= 1850);
        return length;
    };
    // Direct visibility is blocked in both directions. A region's BFS tie
    // order must not send only the west bank far south before the choke.
    const auto eastbound = measure(16, 112), westbound = measure(112, 16);
    CHECK(std::abs(eastbound - westbound) < 1.0);
    std::printf(
        "wall-route: east %.3f, west %.3f footprint cells\n", eastbound / 16, westbound / 16
    );
}

void own_destination_can_cross_a_region_split() {
    Grid grid(64, 64);
    for (uint32_t z = 0; z < 64; ++z)
        grid.cells[z * 64 + 42] = 0;
    FlowCache cache(64, 64);
    const auto seed = circle(52, 40);
    (void)query(cache, grid, unit_at(52, 8), seed);
    (void)query(cache, grid, unit_at(53, 8), seed);
    finish(cache, grid);
    // Both goals key the same32x32region, but lie in disconnected rooms.
    // A directly reachable own goal still has a valid route.
    std::array<RoutePoint, navigation_capacity> points{};
    uint32_t count{};
    CHECK(
        cache.route(context(), unit_at(36, 8), circle(36, 52), points, count, grid.sampler()) ==
        FlowResult::ready
    );
    CHECK(count == 2);
    CHECK(points[1] == (RoutePoint{584, 840}));
}

void multiple_fields_clear_with_a_separate_byte_budget() {
    Grid grid(336, 336);
    FlowCache cache(336, 336);
    const auto unit = unit_at(8, 8);
    for (int16_t i = 0; i < 22; ++i) {
        const auto goal = circle(
            static_cast<int16_t>(16 + (i % 10) * 32), static_cast<int16_t>(16 + (i / 10) * 32)
        );
        CHECK(query(cache, grid, unit, goal) == FlowResult::fallback);
        CHECK(query(cache, grid, unit, goal) == FlowResult::pending);
    }
    CHECK(cache.counters().fields_started == 22);
    for (uint32_t tick = 1; tick <= 3; ++tick) {
        const auto before = cache.counters();
        cache.advance(tick, 0, grid.sampler());
        const auto after = cache.counters();
        CHECK(after.cells_cleared - before.cells_cleared <= 65'536);
        CHECK(after.cells_expanded - before.cells_expanded <= 2048);
        cache.advance(tick, 0, grid.sampler());
        CHECK(cache.counters().cells_cleared == after.cells_cleared);
    }
    CHECK(cache.counters().cells_cleared == 22 * 84 * 84);
    CHECK(cache.counters().cells_expanded > 0);
}

void narrow_choke_and_mixed_footprints() {
    Grid grid(64, 64);
    // A one-cell passage at z=29 has no stride-four connection. Full search
    // remains available instead of silently returning an invalid corridor.
    for (uint32_t z = 0; z < 64; ++z)
        if (z != 29)
            grid.cells[z * 64 + 30] = 0;
    FlowCache cache(64, 64, {4, 4096, 1024});
    const auto goal = circle(52, 28);
    CHECK(query(cache, grid, unit_at(8, 28), goal) == FlowResult::fallback);
    CHECK(query(cache, grid, unit_at(9, 28), goal) == FlowResult::pending);
    finish(cache, grid);
    CHECK(query(cache, grid, unit_at(8, 28), goal) == FlowResult::fallback);
    // A broad on-grid passage allows 1x1, but a separate 2x2 field must not
    // reuse that route through a one-cell gap.
    grid.cells[29 * 64 + 30] = 0;
    grid.cells[28 * 64 + 30] = 1;
    cache.advance(40, 1, grid.sampler());
    CHECK(query(cache, grid, unit_at(8, 28), goal, context(0, 1)) == FlowResult::fallback);
    CHECK(query(cache, grid, unit_at(9, 28), goal, context(0, 1)) == FlowResult::pending);
    finish(cache, grid, 1);
    CHECK(query(cache, grid, unit_at(8, 28), goal, context(0, 1)) == FlowResult::ready);
    CHECK(query(cache, grid, unit_at(8, 28, 2), goal, context(1, 1)) == FlowResult::fallback);
    CHECK(query(cache, grid, unit_at(10, 28, 2), goal, context(1, 1)) == FlowResult::pending);
    finish(cache, grid, 1);
    CHECK(query(cache, grid, unit_at(8, 28, 2), goal, context(1, 1)) == FlowResult::fallback);
}

void revisions_and_restore_pending() {
    Grid grid(64, 64);
    FlowCache first(64, 64, {2, 1024, 32}), resumed(64, 64, {2, 1024, 32});
    const auto unit = unit_at(8, 8);
    const auto goal = circle(52, 52);
    (void)query(first, grid, unit, goal);
    (void)query(first, grid, unit, goal);
    first.advance(1, 0, grid.sampler());
    CHECK(resumed.restore(first.state_bytes()));
    for (uint32_t tick = 2; tick < 30; ++tick) {
        first.advance(tick, 0, grid.sampler());
        resumed.advance(tick, 0, grid.sampler());
        CHECK(
            std::equal(
                first.state_bytes().begin(),
                first.state_bytes().end(),
                resumed.state_bytes().begin()
            )
        );
    }
    CHECK(resumed.restore(first.state_bytes()));
    CHECK(query(first, grid, unit, goal) == FlowResult::ready);
    // A new visible obstacle is rejected even without a knowledge revision.
    for (uint32_t z = 0; z < 64; ++z)
        grid.cells[z * 64 + 30] = 0;
    CHECK(query(first, grid, unit, goal) == FlowResult::fallback);
    CHECK(first.counters().invalidations == 1);
    first.advance(30, 1, grid.sampler());
    CHECK(query(first, grid, unit, goal, context(0, 1)) == FlowResult::fallback);
    CHECK(query(first, grid, unit, goal, context(0, 1)) == FlowResult::pending);
    finish(first, grid, 1);
    CHECK(query(first, grid, unit, goal, context(0, 1)) == FlowResult::fallback);
    std::vector<uint8_t> broken(first.state_bytes().begin(), first.state_bytes().end());
    broken[0] ^= 1;
    const std::vector<uint8_t> before(resumed.state_bytes().begin(), resumed.state_bytes().end());
    CHECK(!resumed.restore(broken));
    CHECK(std::equal(before.begin(), before.end(), resumed.state_bytes().begin()));
}

void singletons_do_not_starve_building_fields() {
    Grid grid(256, 256);
    FlowCache cache(256, 256, {1, 4096, 512});
    (void)query(cache, grid, unit_at(8, 8), circle(220, 220));
    CHECK(query(cache, grid, unit_at(9, 8), circle(220, 220)) == FlowResult::pending);
    for (int16_t x = 4; x < 220; x += 32)
        CHECK(query(cache, grid, unit_at(8, 8), circle(x, 4)) == FlowResult::fallback);
    CHECK(cache.counters().fields_started == 1);
    finish(cache, grid);
    CHECK(query(cache, grid, unit_at(8, 8), circle(220, 220)) == FlowResult::ready);
}

void small_output_and_extraction_budget_are_bounded() {
    Grid grid(64, 64);
    FlowCache cache(64, 64, {2, 1024, 512, 512});
    const auto unit = unit_at(9, 9);
    const auto goal = circle(52, 52);
    (void)query(cache, grid, unit, goal);
    (void)query(cache, grid, unit, goal);
    finish(cache, grid);
    std::array<RoutePoint, 2> points{};
    uint32_t count{};
    CHECK(cache.route(context(), unit, goal, points, count, grid.sampler()) == FlowResult::ready);
    CHECK(count == 2);
    cache.advance(40, 0, grid.sampler());
    const auto probes_before = grid.queries;
    uint32_t waiting = 0;
    for (uint32_t i = 0; i < 1000; ++i)
        if (query(cache, grid, unit, goal) == FlowResult::pending)
            ++waiting;
    CHECK(waiting > 900);
    CHECK(grid.queries - probes_before <= 512);
    const auto before = cache.counters();
    cache.advance(40, 0, grid.sampler()); // A second load/regular pass gets no second budget.
    CHECK(query(cache, grid, unit, goal) == FlowResult::pending);
    CHECK(cache.counters().cells_expanded == before.cells_expanded);
    cache.advance(41, 0, grid.sampler());
    CHECK(query(cache, grid, unit, goal) == FlowResult::ready);
}

void long_shortcut_cannot_starve_a_small_budget_prefix() {
    Grid grid(400, 400);
    FlowCache cache(400, 400, {1, 10'000, 2048, 128});
    auto unit = unit_at(8, 8);
    const auto goal = circle(360, 360);
    (void)query(cache, grid, unit, goal);
    (void)query(cache, grid, unit, goal);
    finish(cache, grid);
    // The complete diagonal visibility test costs more than 1,000 probes.
    // A local coarse edge still fits the128-probe budget and must progress.
    for (uint32_t tick = 40; tick < 46; ++tick) {
        cache.advance(tick, 0, grid.sampler());
        const auto probes_before = grid.queries;
        std::array<RoutePoint, 2> points{};
        uint32_t count{};
        const auto result = cache.route(context(), unit, goal, points, count, grid.sampler());
        CHECK(result == FlowResult::ready);
        CHECK(count == 2);
        CHECK(grid.queries - probes_before <= 128);
        if (result == FlowResult::ready && count == 2) {
            CHECK(points[1][0] > points[0][0] && points[1][1] > points[0][1]);
            CHECK(points[1] != (RoutePoint{5768, 5768}));
            unit = unit_at(
                static_cast<int16_t>((points[1][0] - 8) / 16),
                static_cast<int16_t>((points[1][1] - 8) / 16)
            );
        }
    }
    CHECK(cache.counters().hits == 6);
}

void malformed_frontiers_are_rejected() {
    Grid grid(32, 32);
    FlowCache cache(32, 32, {1, 64, 1}), restored(32, 32, {1, 64, 1});
    const auto unit = unit_at(4, 4);
    const auto goal = circle(24, 24);
    (void)query(cache, grid, unit, goal);
    (void)query(cache, grid, unit, goal);
    const auto write32 = [](std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    constexpr size_t field = 128 + 64 * 48;
    std::vector<uint8_t> broken(cache.state_bytes().begin(), cache.state_bytes().end());
    write32(broken, field + 52, 64); // clearing cannot resume beyond its last cell
    CHECK(!restored.restore(broken));
    for (uint32_t tick = 1; tick < 130; ++tick)
        cache.advance(tick, 0, grid.sampler());
    CHECK(restored.restore(cache.state_bytes()));
    broken.assign(cache.state_bytes().begin(), cache.state_bytes().end());
    constexpr size_t directions = field + 80, queue = directions + 64;
    write32(broken, queue + 4, broken[queue]); // duplicate seed queue entry
    CHECK(!restored.restore(broken));
    broken.assign(cache.state_bytes().begin(), cache.state_bytes().end());
    // Adjacent non-root nodes now point at one another. Queue uniqueness
    // and discovered count remain valid, but parent-before-child cannot.
    broken[directions] = 0;
    broken[directions + 1] = 4;
    CHECK(!restored.restore(broken));
}

} // namespace

int main() {
    blocked_lane_detours_are_symmetric_and_bounded();
    open_routes_keep_heading_and_individual_destinations();
    own_destination_can_cross_a_region_split();
    multiple_fields_clear_with_a_separate_byte_budget();
    static_sampler_matches_full_classifier_passability();
    small_output_and_extraction_budget_are_bounded();
    long_shortcut_cannot_starve_a_small_budget_prefix();
    malformed_frontiers_are_rejected();
    shared_cohort_and_budget();
    narrow_choke_and_mixed_footprints();
    revisions_and_restore_pending();
    singletons_do_not_starve_building_fields();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
