// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/shared_routes.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"
#include "oa/base/game_math.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

void finish(FlowCache& cache, Grid& grid, uint32_t revision = 0, uint32_t first_tick = 1) {
    for (uint32_t tick = first_tick; tick < first_tick + 39; ++tick)
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
    // A new clear direct connection is usable before the retained tree refreshes.
    CHECK(query(cache, grid, unit_at(8, 28), goal, context(0, 1)) == FlowResult::ready);
    CHECK(query(cache, grid, unit_at(8, 28, 2), goal, context(1, 1)) == FlowResult::fallback);
    CHECK(query(cache, grid, unit_at(10, 28, 2), goal, context(1, 1)) == FlowResult::pending);
    finish(cache, grid, 1, 41);
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
    // The repeated destination's admission survives an unrelated revision.
    CHECK(query(first, grid, unit, goal, context(0, 1)) == FlowResult::pending);
    finish(first, grid, 1, 31);
    CHECK(query(first, grid, unit, goal, context(0, 1)) == FlowResult::fallback);
    std::vector<uint8_t> broken(first.state_bytes().begin(), first.state_bytes().end());
    broken[0] ^= 1;
    const std::vector<uint8_t> before(resumed.state_bytes().begin(), resumed.state_bytes().end());
    CHECK(!resumed.restore(broken));
    CHECK(std::equal(before.begin(), before.end(), resumed.state_bytes().begin()));
}

/// Keeps useful trees and repeated-goal admission after unrelated map changes.
void unrelated_revisions_keep_complete_routes_and_admissions() {
    Grid grid(64, 64);
    FlowCache cache(64, 64);
    const auto first_goal = circle(52, 52), second_goal = circle(52, 12);
    const auto first_unit = unit_at(8, 8), second_unit = unit_at(8, 12);
    (void)query(cache, grid, first_unit, first_goal);
    (void)query(cache, grid, first_unit, first_goal);
    // Keep this second destination's first admission across the revision too.
    CHECK(query(cache, grid, second_unit, second_goal) == FlowResult::fallback);
    finish(cache, grid);
    const auto before = cache.counters();
    grid.cells[63 * 64] = 0;
    cache.advance(40, 1, grid.sampler());
    CHECK(query(cache, grid, first_unit, first_goal, context(0, 1)) == FlowResult::ready);
    CHECK(cache.counters().invalidations == before.invalidations);
    CHECK(cache.counters().fields_started == before.fields_started);
    CHECK(query(cache, grid, second_unit, second_goal, context(0, 1)) == FlowResult::pending);
    finish(cache, grid, 1, 41);
    CHECK(query(cache, grid, second_unit, second_goal, context(0, 1)) == FlowResult::ready);

    // A real new barrier invalidates the touched tree, not every destination.
    grid.cells[30 * 64 + 30] = 0;
    cache.advance(80, 2, grid.sampler());
    const auto invalidations = cache.counters().invalidations;
    CHECK(query(cache, grid, first_unit, first_goal, context(0, 2)) == FlowResult::fallback);
    CHECK(cache.counters().invalidations == invalidations + 1);
    CHECK(query(cache, grid, second_unit, second_goal, context(0, 2)) == FlowResult::ready);
    CHECK(cache.counters().invalidations == invalidations + 1);
}

/// Completes a frontier despite a different map revision on every tick.
void changing_revisions_cannot_restart_building_frontiers() {
    Grid grid(128, 128);
    for (uint32_t z = 48; z <= 80; ++z)
        grid.cells[z * 128 + 64] = 0;
    FlowCache cache(128, 128, {1, 1024, 32});
    const auto unit = unit_at(16, 64);
    const auto goal = circle(112, 64);
    (void)query(cache, grid, unit, goal);
    (void)query(cache, grid, unit, goal);
    uint32_t first_ready = 0;
    for (uint32_t tick = 1; tick <= 60; ++tick) {
        const auto before = cache.counters();
        cache.advance(tick, tick, grid.sampler());
        const auto after = cache.counters();
        CHECK(after.cells_cleared - before.cells_cleared <= 65'536);
        CHECK(after.cells_expanded - before.cells_expanded <= 32);
        if (query(cache, grid, unit, goal, context(0, tick)) == FlowResult::ready && !first_ready)
            first_ready = tick;
    }
    CHECK(first_ready != 0 && first_ready <= 60);
    CHECK(cache.counters().fields_started == 1);
    CHECK(cache.counters().invalidations == 0);
    CHECK(cache.counters().cells_expanded > 500);
}

/// Measures the currently returned route around the fixture's wall.
double route_length(FlowCache& cache, Grid& grid, FlowContext ctx) {
    std::array<RoutePoint, navigation_capacity> points{};
    uint32_t count{};
    CHECK(
        cache.route(ctx, unit_at(32, 64), circle(96, 64), points, count, grid.sampler()) ==
        FlowResult::ready
    );
    double length = 0;
    for (uint32_t i = 1; i < count; ++i)
        length += std::hypot(
            double(points[i][0] - points[i - 1][0]), double(points[i][1] - points[i - 1][1])
        );
    return length;
}

/// Replaces a legal long detour after a closer off-axis bridge opens.
void periodic_refresh_discovers_a_new_bridge() {
    Grid grid(128, 128);
    for (uint32_t z = 0; z < 128; ++z)
        if (z < 8 || z >= 16)
            grid.cells[z * 128 + 64] = 0;
    FlowCache cache(128, 128, {1, 1024, 2048});
    (void)query(cache, grid, unit_at(32, 64), circle(96, 64));
    (void)query(cache, grid, unit_at(32, 64), circle(96, 64));
    finish(cache, grid);
    const auto original_length = route_length(cache, grid, context());
    for (uint32_t z = 80; z < 88; ++z)
        grid.cells[z * 128 + 64] = 1;
    cache.advance(40, 1, grid.sampler());
    const auto retained_length = route_length(cache, grid, context(0, 1));
    CHECK(retained_length == original_length);
    for (uint32_t tick = 41; tick <= 90; ++tick)
        cache.advance(tick, 1, grid.sampler());
    const auto refreshed_length = route_length(cache, grid, context(0, 1));
    CHECK(refreshed_length + 500 < original_length);
    CHECK(cache.counters().fields_started == 2);
    CHECK(cache.counters().invalidations == 1);
    std::printf("opened-bridge: %.1f -> %.1f world units\n", original_length, refreshed_length);
}

/// Preserves pending work, refresh policy and tick budgets across a saved state.
void dirty_pending_state_resumes_with_the_same_budget_and_refresh_order() {
    Grid grid(128, 128);
    for (uint32_t z = 48; z <= 80; ++z)
        grid.cells[z * 128 + 64] = 0;
    FlowCache first(128, 128, {2, 2048, 32}), resumed(128, 128, {2, 2048, 32});
    const auto unit = unit_at(16, 64);
    const auto goal = circle(112, 64);
    (void)query(first, grid, unit, goal);
    (void)query(first, grid, unit, goal);
    for (uint32_t tick = 1; tick <= 4; ++tick)
        first.advance(tick, tick, grid.sampler());
    CHECK(resumed.restore(first.state_bytes()));
    for (uint32_t tick = 5; tick <= 180; ++tick) {
        const auto before = first.counters();
        first.advance(tick, tick, grid.sampler());
        resumed.advance(tick, tick, grid.sampler());
        const auto after = first.counters();
        CHECK(after.cells_cleared - before.cells_cleared <= 65'536);
        CHECK(after.cells_expanded - before.cells_expanded <= 32);
        CHECK(
            query(first, grid, unit, goal, context(0, tick)) ==
            query(resumed, grid, unit, goal, context(0, tick))
        );
        const auto once = first.counters();
        first.advance(tick, tick, grid.sampler());
        resumed.advance(tick, tick, grid.sampler());
        CHECK(first.counters().cells_expanded == once.cells_expanded);
        CHECK(first.counters().cells_cleared == once.cells_cleared);
        CHECK(
            std::equal(
                first.state_bytes().begin(),
                first.state_bytes().end(),
                resumed.state_bytes().begin()
            )
        );
    }
    CHECK(first.counters().fields_started >= 2);
    CHECK(resumed.restore(first.state_bytes()));
}

/// Matches continuation when loading observes a pending revision one pass earlier.
void pending_map_revision_keeps_the_same_age_across_a_load_pass() {
    Grid grid(64, 64);
    FlowCache original(64, 64), loaded(64, 64);
    const auto unit = unit_at(8, 8);
    const auto goal = circle(52, 52);
    (void)query(original, grid, unit, goal);
    (void)query(original, grid, unit, goal);
    finish(original, grid);
    CHECK(loaded.restore(original.state_bytes()));
    // The save's actual map revision is ahead of the last cache advance.
    // Loading runs a pass on the saved tick; the original next advances on40.
    loaded.advance(39, 1, grid.sampler());
    original.advance(40, 1, grid.sampler());
    loaded.advance(40, 1, grid.sampler());
    CHECK(
        std::equal(
            original.state_bytes().begin(),
            original.state_bytes().end(),
            loaded.state_bytes().begin()
        )
    );
    for (uint32_t tick = 41; tick <= 130; ++tick) {
        original.advance(tick, 1, grid.sampler());
        loaded.advance(tick, 1, grid.sampler());
        CHECK(
            query(original, grid, unit, goal, context(0, 1)) ==
            query(loaded, grid, unit, goal, context(0, 1))
        );
        CHECK(
            std::equal(
                original.state_bytes().begin(),
                original.state_bytes().end(),
                loaded.state_bytes().begin()
            )
        );
    }
}

/// Services older dirty destinations before a newly dirtied low-index field.
void periodic_refresh_keeps_older_destinations_ahead_of_recent_refreshes() {
    Grid grid(64, 64);
    for (uint32_t z = 16; z <= 48; ++z)
        grid.cells[z * 64 + 32] = 0;
    FlowCache cache(64, 64, {4, 1024, 32});
    const auto unit = unit_at(8, 32);
    const auto goal = circle(56, 32);
    for (uint32_t player = 0; player < 4; ++player) {
        auto ctx = context();
        ctx.player = player;
        (void)query(cache, grid, unit, goal, ctx);
        (void)query(cache, grid, unit, goal, ctx);
    }
    finish(cache, grid);
    CHECK(cache.counters().fields_started == 4);
    for (uint32_t tick = 40; tick <= 210; ++tick) {
        const auto before = cache.counters();
        cache.advance(tick, tick, grid.sampler());
        CHECK(cache.counters().cells_expanded - before.cells_expanded <= 32);
        if (tick == 120 || tick == 150 || tick == 180 || tick == 210) {
            const uint32_t selected_player = (tick - 120) / 30;
            for (uint32_t player = 0; player < 4; ++player) {
                auto ctx = context(0, tick);
                ctx.player = player;
                CHECK(
                    query(cache, grid, unit, goal, ctx) ==
                    (player == selected_player ? FlowResult::pending : FlowResult::ready)
                );
            }
            CHECK(cache.counters().fields_started == 5 + selected_player);
            const auto once = cache.counters();
            cache.advance(tick, tick, grid.sampler());
            CHECK(cache.counters().fields_started == once.fields_started);
            CHECK(cache.counters().cells_expanded == once.cells_expanded);
        }
    }
    CHECK(cache.counters().invalidations == 4);
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

/// Supplies full static-footprint collision checks to one real steering driver.
struct StaticSteeringHost : oa::sim::unit_movement::Host, Host {
    Grid& grid;

    explicit StaticSteeringHost(Grid& value) : grid(value) {}

    bool can_occupy(
        const oa::sim::unit_movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ) override {
        constexpr uint8_t ground_occupancy = 1;
        if (occupancy != ground_occupancy || cell[0] < 0 || cell[1] < 0 ||
            int32_t(cell[0]) + unit.footprint[0] > int32_t(grid.width) ||
            int32_t(cell[1]) + unit.footprint[1] > int32_t(grid.height))
            return false;
        for (int16_t z = 0; z < unit.footprint[1]; ++z)
            for (int16_t x = 0; x < unit.footprint[0]; ++x)
                if (!grid.cells[size_t(cell[1] + z) * grid.width + size_t(cell[0] + x)])
                    return false;
        return true;
    }

    void remove_occupancy(oa::sim::unit_movement::Unit&) override {}

    void insert_occupancy(oa::sim::unit_movement::Unit&) override {}

    void update_spatial_membership(oa::sim::unit_movement::Unit&) override {}

    void cancel_search(Navigation&) override {}

    void play_sound(oa::sim::simulation_state::Unit&, uint32_t) override {}

    void wake_weapon(oa::sim::simulation_state::Unit&, uint32_t) override {}

    oa::sim::simulation_state::Unit* find_target(oa::sim::simulation_state::Unit&) override {
        return nullptr;
    }

    bool issue_attack(oa::sim::simulation_state::Unit&, oa::sim::simulation_state::Unit&) override {
        return false;
    }

    uint32_t random(uint32_t) override { return 0; }
};

struct CornerArrival {
    uint32_t tick{};
    uint64_t distance{};
    uint64_t turn{};
    uint32_t blocked{};
    std::array<int32_t, 3> position{};
    bool operator==(const CornerArrival&) const = default;
};

/// Follows the actual navigator, turn, acceleration and collision steps to a corner.
CornerArrival
steer_to_corner(Grid& grid, std::span<const RoutePoint> points, const RoutePoint& corner) {
    namespace movement = oa::sim::unit_movement;
    constexpr int32_t fixed_one = 65536;
    constexpr uint8_t ground_occupancy = 1;
    constexpr uint16_t east = 49152;
    oa::sim::unit_spawn::LegacyWorld world(2);
    movement::Unit unit{};
    unit.position = {int32_t(points[0][0]) * fixed_one, 0, int32_t(points[0][1]) * fixed_one};
    constexpr int32_t cell_shift = 4, half_cell = 8;
    unit.cell = {
        static_cast<int16_t>((int32_t(points[0][0]) - half_cell) >> cell_shift),
        static_cast<int16_t>((int32_t(points[0][1]) - half_cell) >> cell_shift)
    };
    unit.footprint = {2, 2};
    unit.flags = ground_occupancy;
    unit.id = 1;
    unit.heading = east;
    unit.type.maximum_speed = 18 * fixed_one / 10;
    unit.type.maximum_turn = 1120;
    movement::Movement motion{};
    motion.flags = ground_occupancy;
    motion.speed = unit.type.maximum_speed;
    motion.velocity = movement::aim_velocity(east, 0, motion.speed);
    Navigation navigation{};
    navigation.flags = route_present_flag;
    navigation.count = static_cast<uint32_t>(points.size());
    std::copy(points.begin(), points.end(), navigation.points.begin());
    std::array<uint8_t, 3> weapons{};
    UnitView view{world.unit(1), unit, motion, navigation, false, weapons};
    StaticSteeringHost host(grid);
    CornerArrival result{};
    constexpr int32_t acceleration = fixed_one / 10;
    for (uint32_t tick = 1; tick <= 900; ++tick) {
        tick_navigation(view, tick, host);
        if (navigation.points[0] == corner) {
            result.tick = tick;
            break;
        }
        const auto previous = unit.position;
        const auto heading = unit.heading;
        steer_ground(
            unit, motion, navigation, acceleration, acceleration, 0, FastUnitSteering::as_3_1c
        );
        result.turn += std::abs(std::bit_cast<int16_t>(uint16_t(unit.heading - heading)));
        movement::integrate_unattached(unit, motion, tick, true, host);
        result.distance += oa::base::game_math::distance(
            unit.position[0] - previous[0], unit.position[2] - previous[2]
        );
        result.blocked += (motion.flags & movement::collision_blocked) != 0;
    }
    result.position = unit.position;
    return result;
}

/// Removes a short anchor hook only when the next corner has a clear footprint path.
void visible_first_corner_does_not_require_a_short_anchor_turn() {
    Grid grid(64, 64);
    for (uint32_t z = 16; z <= 48; ++z)
        grid.cells[z * 64 + 32] = 0;
    FlowCache cache(64, 64), loaded(64, 64);
    auto unit = unit_at(15, 15, 2);
    unit.position.x += 7 << 16;
    unit.position.z += 7 << 16;
    const auto goal = circle(56, 32);
    (void)query(cache, grid, unit, goal, context(1));
    (void)query(cache, grid, unit, goal, context(1));
    finish(cache, grid);
    CHECK(loaded.restore(cache.state_bytes()));
    std::array<RoutePoint, navigation_capacity> points{}, restored{};
    uint32_t count{}, restored_count{};
    CHECK(cache.route(context(1), unit, goal, points, count, grid.sampler()) == FlowResult::ready);
    CHECK(
        loaded.route(context(1), unit, goal, restored, restored_count, grid.sampler()) ==
        FlowResult::ready
    );
    CHECK(count == restored_count);
    CHECK(std::equal(points.begin(), points.begin() + count, restored.begin()));
    CHECK(
        std::equal(
            cache.state_bytes().begin(), cache.state_bytes().end(), loaded.state_bytes().begin()
        )
    );
    CHECK(count == 5);
    CHECK(points[0] == (RoutePoint{263, 263}));
    CHECK(points[1] == (RoutePoint{336, 208}));

    // The previous extractor required a turn toward(272,272), then a sharp
    // turn back toward the visible corner. Keep it as a physical control.
    std::vector<RoutePoint> anchor_control(points.begin(), points.begin() + count);
    anchor_control.insert(anchor_control.begin() + 1, RoutePoint{272, 272});
    const auto original = steer_to_corner(grid, anchor_control, RoutePoint{336, 208});
    const auto direct = steer_to_corner(grid, std::span(points).first(count), RoutePoint{336, 208});
    CHECK(original.tick != 0 && direct.tick != 0);
    CHECK(direct.tick + 5 < original.tick);
    CHECK(direct.distance < original.distance);
    CHECK(direct.turn * 2 < original.turn);
    CHECK(direct.blocked == 0 && original.blocked == 0);
    CHECK(direct == steer_to_corner(grid, std::span(points).first(count), RoutePoint{336, 208}));
    std::printf(
        "start-anchor: corner arrival%u ->%u ticks, turn%.1f ->%.1f degrees\n",
        original.tick,
        direct.tick,
        double(original.turn) * 360 / 65536,
        double(direct.turn) * 360 / 65536
    );

    // The optional ray must not turn a valid route into an unbounded query.
    FlowCache limited(64, 64, {1, 256, 2048, 512});
    (void)query(limited, grid, unit, goal, context(1));
    (void)query(limited, grid, unit, goal, context(1));
    finish(limited, grid);
    limited.advance(40, 0, grid.sampler());
    const auto probes_before = grid.queries;
    uint32_t waiting = 0;
    for (uint32_t i = 0; i < 1000; ++i)
        waiting += query(limited, grid, unit, goal, context(1)) == FlowResult::pending;
    CHECK(waiting > 900);
    CHECK(grid.queries - probes_before <= 512);
    limited.advance(41, 0, grid.sampler());
    CHECK(query(limited, grid, unit, goal, context(1)) == FlowResult::ready);
}

/// Keeps the anchor when skipping it would cut a blocked two-cell corner.
void a_blocked_first_corner_keeps_the_start_anchor() {
    Grid grid(32, 32);
    for (uint32_t z = 2; z < 28; ++z)
        for (uint32_t x = 2; x < 28; ++x)
            if ((x * 17 + z * 23) % 43 == 0)
                grid.cells[z * 32 + x] = 0;
    FlowCache cache(32, 32);
    auto unit = unit_at(2, 4, 2);
    unit.position.x += 7 << 16;
    unit.position.z += 7 << 16;
    const auto goal = circle(28, 28);
    (void)query(cache, grid, unit, goal, context(1));
    (void)query(cache, grid, unit, goal, context(1));
    finish(cache, grid);
    std::array<RoutePoint, navigation_capacity> points{};
    uint32_t count{};
    CHECK(cache.route(context(1), unit, goal, points, count, grid.sampler()) == FlowResult::ready);
    CHECK(count == 7);
    CHECK(points[0] == (RoutePoint{55, 87}));
    CHECK(points[1] == (RoutePoint{80, 80}));
    CHECK(points[2] == (RoutePoint{272, 272}));
}

/// Pins the original guides and saved frontier bytes across revisions and a load pass.
uint64_t legacy_flow_hash() {
    constexpr uint64_t basis = 14695981039346656037ull, prime = 1099511628211ull;
    uint64_t hash = basis;
    const auto byte = [&](uint8_t value) { hash = (hash ^ value) * prime; };
    const auto number = [&](uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            byte(static_cast<uint8_t>(value >> shift));
    };
    Grid grid(192, 128);
    for (uint32_t z = 24; z <= 100; ++z)
        grid.cells[z * grid.width + 96] = 0;
    FlowCache cache(192, 128, {8, 12288, 64, 1024});
    FlowCache loaded(192, 128, {8, 12288, 64, 1024});
    uint32_t revision = 0;
    for (uint32_t tick = 1; tick <= 160; ++tick) {
        if (tick == 60)
            for (uint32_t z = 0; z < 24; ++z)
                grid.cells[z * grid.width + 96] = 0;
        if (tick == 70 || tick == 125)
            ++revision;
        if (tick == 125)
            grid.cells[64 * grid.width + 96] = 1;
        cache.advance(tick, revision, grid.sampler());
        if (tick > 50)
            loaded.advance(tick, revision, grid.sampler());
        for (uint32_t actor = 0; actor < 8; ++actor) {
            const auto unit = unit_at(
                static_cast<int16_t>(12 + actor * 2), static_cast<int16_t>(16 + actor * 12)
            );
            const auto goal =
                circle(static_cast<int16_t>(160 + actor), static_cast<int16_t>(16 + actor * 12));
            std::array<RoutePoint, navigation_capacity> points{}, resumed{};
            uint32_t count{}, resumed_count{};
            const auto ctx = context(0, revision);
            const auto result = cache.route(ctx, unit, goal, points, count, grid.sampler());
            number(static_cast<uint32_t>(result));
            number(count);
            for (uint32_t i = 0; i < count; ++i) {
                number(static_cast<uint16_t>(points[i][0]));
                number(static_cast<uint16_t>(points[i][1]));
            }
            if (tick > 50) {
                CHECK(
                    loaded.route(ctx, unit, goal, resumed, resumed_count, grid.sampler()) == result
                );
                CHECK(count == resumed_count && points == resumed);
            }
        }
        if (tick == 50) {
            CHECK(loaded.restore(cache.state_bytes()));
            loaded.advance(tick, revision, grid.sampler());
        }
        if (tick >= 50)
            CHECK(
                std::equal(
                    cache.state_bytes().begin(),
                    cache.state_bytes().end(),
                    loaded.state_bytes().begin()
                )
            );
        for (const auto value : cache.state_bytes())
            byte(value);
    }
    return hash;
}

void original_preset_keeps_its_state_and_guides() {
    constexpr uint64_t expected = 0xaa4f8ef12e56dfc2ull;
    CHECK(legacy_flow_hash() == expected);
}

/// Keeps old constructor bytes and refuses cross-preset or inconsistent state atomically.
void route_presets_keep_distinct_formats_and_goal_regions() {
    Grid grid(128, 128);
    const FlowLimits limits{4, 4096, 64, 256};
    FlowCache original(128, 128, limits);
    FlowCache explicit_original(128, 128, limits, FlowPreset::original);
    FlowCache regional(128, 128, limits, FlowPreset::large_regions);
    FlowCache loaded(128, 128, limits, FlowPreset::large_regions);
    CHECK(
        std::equal(
            original.state_bytes().begin(),
            original.state_bytes().end(),
            explicit_original.state_bytes().begin()
        )
    );
    CHECK(original.state_bytes().size() == regional.state_bytes().size());
    CHECK(original.state_bytes()[4] == 1 && regional.state_bytes()[4] == 2);
    const auto unit = unit_at(4, 4);
    const auto first = circle(40, 40), second = circle(80, 80);
    CHECK(query(original, grid, unit, first) == FlowResult::fallback);
    CHECK(query(original, grid, unit, second) == FlowResult::fallback);
    CHECK(query(regional, grid, unit, first) == FlowResult::fallback);
    CHECK(query(regional, grid, unit, second) == FlowResult::pending);
    CHECK(regional.counters().fields_started == 1);
    CHECK(original.counters().fields_started == 0);
    CHECK(query(original, grid, unit, first) == FlowResult::pending);
    CHECK(query(original, grid, unit, second) == FlowResult::pending);
    CHECK(original.counters().fields_started == 2);
    const std::vector<uint8_t> old_bytes(
        original.state_bytes().begin(), original.state_bytes().end()
    );
    const std::vector<uint8_t> new_bytes(
        regional.state_bytes().begin(), regional.state_bytes().end()
    );
    CHECK(!original.restore(new_bytes));
    CHECK(!regional.restore(old_bytes));
    CHECK(std::equal(old_bytes.begin(), old_bytes.end(), original.state_bytes().begin()));
    CHECK(std::equal(new_bytes.begin(), new_bytes.end(), regional.state_bytes().begin()));
    auto forged = old_bytes;
    forged[4] = 2;
    CHECK(!regional.restore(forged));
    forged = new_bytes;
    constexpr size_t admission_region_x = 128 + 28;
    forged[admission_region_x] = 255;
    CHECK(!regional.restore(forged));
    CHECK(std::equal(new_bytes.begin(), new_bytes.end(), regional.state_bytes().begin()));
    CHECK(loaded.restore(new_bytes));
    for (uint32_t tick = 1; tick <= 40; ++tick) {
        regional.advance(tick, 0, grid.sampler());
        loaded.advance(tick, 0, grid.sampler());
        CHECK(query(regional, grid, unit, second) == query(loaded, grid, unit, second));
        CHECK(
            std::equal(
                regional.state_bytes().begin(),
                regional.state_bytes().end(),
                loaded.state_bytes().begin()
            )
        );
    }
    std::array<RoutePoint, navigation_capacity> points{};
    uint32_t count{};
    regional.advance(41, 0, grid.sampler());
    CHECK(
        regional.route(context(), unit, second, points, count, grid.sampler()) == FlowResult::ready
    );
    CHECK(count >= 2);
    if (count >= 2)
        CHECK(points[count - 1] == (RoutePoint{1288, 1288}));
}

/// A newly learned wall must not discard a useful branch of the same field.
void regional_blocked_edge_keeps_an_unaffected_branch_ready() {
    Grid grid(128, 128);
    for (uint32_t z = 48; z <= 80; ++z)
        grid.cells[z * 128 + 64] = 0;
    FlowCache cache(128, 128, {1, 1024, 2048}, FlowPreset::large_regions);
    const auto goal = circle(112, 64);
    const auto north = unit_at(32, 40), south = unit_at(32, 88);
    (void)query(cache, grid, north, goal);
    (void)query(cache, grid, north, goal);
    finish(cache, grid);
    CHECK(query(cache, grid, north, goal) == FlowResult::ready);
    CHECK(query(cache, grid, south, goal) == FlowResult::ready);
    const auto before = cache.counters();
    // Close the northern end of the original wall; the south stays open.
    for (uint32_t z = 0; z < 48; ++z)
        grid.cells[z * 128 + 64] = 0;
    CHECK(query(cache, grid, north, goal) == FlowResult::fallback);
    CHECK(query(cache, grid, south, goal) == FlowResult::ready);
    CHECK(query(cache, grid, north, goal) == FlowResult::fallback);
    CHECK(cache.counters().fields_started == before.fields_started);
    CHECK(cache.counters().invalidations == before.invalidations);
    cache.advance(40, 0, grid.sampler());
    const auto probes = grid.queries;
    uint32_t pending = 0;
    for (uint32_t i = 0; i < 1000; ++i)
        pending += query(cache, grid, north, goal) == FlowResult::pending;
    CHECK(pending > 0);
    CHECK(grid.queries - probes <= 65'536);
    CHECK(cache.counters().fields_started == before.fields_started);
    cache.advance(41, 0, grid.sampler());
    CHECK(query(cache, grid, south, goal) == FlowResult::ready);
}

/// Lets a dirty frontier finish, then repairs its blocked branch after restore.
void regional_blocked_building_edge_resumes_and_refreshes() {
    Grid grid(128, 128);
    FlowCache original(128, 128, {1, 1024, 32}, FlowPreset::large_regions);
    FlowCache restored(128, 128, {1, 1024, 32}, FlowPreset::large_regions);
    const auto goal = circle(112, 64);
    const auto affected = unit_at(108, 68), unaffected = unit_at(112, 72);
    (void)query(original, grid, affected, goal);
    (void)query(original, grid, affected, goal);
    original.advance(1, 0, grid.sampler());
    CHECK(query(original, grid, affected, goal) == FlowResult::ready);
    grid.cells[66 * 128 + 110] = 0;
    CHECK(query(original, grid, affected, goal) == FlowResult::fallback);
    CHECK(query(original, grid, unaffected, goal) == FlowResult::ready);
    CHECK(restored.restore(original.state_bytes()));
    restored.advance(1, 0, grid.sampler());
    CHECK(
        std::equal(
            original.state_bytes().begin(),
            original.state_bytes().end(),
            restored.state_bytes().begin()
        )
    );
    bool repaired = false;
    for (uint32_t tick = 2; tick <= 180; ++tick) {
        const auto before = original.counters();
        original.advance(tick, 0, grid.sampler());
        restored.advance(tick, 0, grid.sampler());
        const auto after = original.counters();
        CHECK(after.cells_expanded - before.cells_expanded <= 32);
        CHECK(after.cells_cleared - before.cells_cleared <= 65'536);
        const auto probes = grid.queries;
        const auto result = query(original, grid, affected, goal);
        CHECK(grid.queries - probes <= 65'536);
        CHECK(result == query(restored, grid, affected, goal));
        repaired |= result == FlowResult::ready;
        CHECK(query(original, grid, unaffected, goal) == query(restored, grid, unaffected, goal));
        if (tick == 80) {
            CHECK(original.counters().cells_expanded == 1024);
            CHECK(original.counters().fields_started == 1);
            CHECK(original.counters().invalidations == 0);
            CHECK(!repaired);
        }
        const auto once = original.counters();
        original.advance(tick, 0, grid.sampler());
        restored.advance(tick, 0, grid.sampler());
        CHECK(original.counters().cells_expanded == once.cells_expanded);
        CHECK(original.counters().cells_cleared == once.cells_cleared);
        CHECK(
            std::equal(
                original.state_bytes().begin(),
                original.state_bytes().end(),
                restored.state_bytes().begin()
            )
        );
    }
    CHECK(repaired);
    CHECK(original.counters().fields_started == 2);
    CHECK(original.counters().invalidations == 1);
    std::array<RoutePoint, navigation_capacity> points{};
    uint32_t count{};
    CHECK(
        original.route(context(), affected, goal, points, count, grid.sampler()) ==
        FlowResult::ready
    );
    CHECK(count >= 3); // The repaired route still goes around the learned obstacle.
}

/// Checks continuous segment intersections independently of the sampler's walk.
bool segment_avoids_blocked_cells(const Grid& grid, RoutePoint from, RoutePoint to) {
    const std::array<double, 2> start{double(from[0] - 8) / 16, double(from[1] - 8) / 16};
    const std::array<double, 2> delta{double(to[0] - from[0]) / 16, double(to[1] - from[1]) / 16};
    for (uint32_t z = 0; z < grid.height; ++z)
        for (uint32_t x = 0; x < grid.width; ++x) {
            if (grid.cells[z * grid.width + x] != 0)
                continue;
            const std::array<double, 2> centre{double(x), double(z)};
            double first = 0, last = 1;
            for (size_t axis = 0; axis < 2; ++axis) {
                if (delta[axis] == 0) {
                    if (std::abs(start[axis] - centre[axis]) > 0.5)
                        first = 2;
                } else {
                    const auto a = (centre[axis] - 0.5 - start[axis]) / delta[axis];
                    const auto b = (centre[axis] + 0.5 - start[axis]) / delta[axis];
                    first = std::max(first, std::min(a, b));
                    last = std::min(last, std::max(a, b));
                }
            }
            if (first <= last)
                return false;
        }
    return true;
}

/// Delivers a legal nearby prefix while a stale connection farther away waits for repair.
void regional_prefixes_continue_to_the_original_goal_after_restore() {
    Grid grid(512, 128);
    for (uint32_t x = 0; x <= 256; ++x)
        grid.cells[48 * 512 + x] = 0;
    for (uint32_t z = 48; z <= 80; ++z)
        grid.cells[z * 512 + 256] = 0;
    FlowCache original(512, 128, {1, 4096, 4096, 256}, FlowPreset::large_regions);
    FlowCache restored(512, 128, {1, 4096, 4096, 256}, FlowPreset::large_regions);
    auto unit = unit_at(32, 64);
    const auto goal = circle(480, 64);
    const auto saved_goal = goal.cell;
    (void)query(original, grid, unit, goal);
    (void)query(original, grid, unit, goal);
    finish(original, grid);
    // The old tree crossed row84. A longer guide would encounter this new
    // barrier before delivering any of the still useful western approach.
    for (uint32_t z = 81; z < 88; ++z)
        grid.cells[z * 512 + 256] = 0;
    bool saved = false, arrived = false, initial_prefix = true;
    uint32_t consumed = 0;
    for (uint32_t tick = 40; tick <= 180; ++tick) {
        const auto before = original.counters();
        original.advance(tick, 0, grid.sampler());
        if (saved)
            restored.advance(tick, 0, grid.sampler());
        CHECK(original.counters().cells_expanded - before.cells_expanded <= 4096);
        CHECK(original.counters().cells_cleared - before.cells_cleared <= 65'536);
        std::array<RoutePoint, navigation_capacity> points{};
        uint32_t count{};
        const auto probes = grid.queries;
        const auto result = original.route(context(), unit, goal, points, count, grid.sampler());
        CHECK(grid.queries - probes <= 256);
        CHECK(goal.cell == saved_goal && goal.shape == GoalShape::circle);
        if (saved) {
            std::array<RoutePoint, navigation_capacity> resumed{};
            uint32_t resumed_count{};
            CHECK(
                restored.route(context(), unit, goal, resumed, resumed_count, grid.sampler()) ==
                result
            );
            CHECK(resumed_count == count);
            CHECK(std::equal(points.begin(), points.begin() + count, resumed.begin()));
            CHECK(
                std::equal(
                    original.state_bytes().begin(),
                    original.state_bytes().end(),
                    restored.state_bytes().begin()
                )
            );
        }
        if (initial_prefix) {
            CHECK(result == FlowResult::ready && count >= 2);
            if (result != FlowResult::ready || count < 2)
                return;
            CHECK(points[count - 1][0] < 256 * 16);
            CHECK(points[count - 1] != (RoutePoint{7688, 1032}));
            CHECK(std::abs(int32_t(points[count - 1][0]) - points[0][0]) <= 1024);
            CHECK(std::abs(int32_t(points[count - 1][1]) - points[0][1]) <= 1024);
            initial_prefix = false;
        }
        if (result == FlowResult::fallback && !saved) {
            CHECK(restored.restore(original.state_bytes()));
            restored.advance(tick, 0, grid.sampler());
            saved = true;
        }
        if (result != FlowResult::ready)
            continue;
        for (uint32_t i = 1; i < count; ++i)
            CHECK(segment_avoids_blocked_cells(grid, points[i - 1], points[i]));
        ++consumed;
        unit = unit_at(
            static_cast<int16_t>((points[count - 1][0] - 8) / 16),
            static_cast<int16_t>((points[count - 1][1] - 8) / 16)
        );
        if (unit.cell_x == goal.cell[0] && unit.cell_z == goal.cell[1]) {
            arrived = true;
            break;
        }
    }
    CHECK(saved && arrived && consumed >= 4);
    CHECK(original.counters().fields_started == 2);
    CHECK(original.counters().invalidations == 1);
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
    broken.assign(cache.state_bytes().begin(), cache.state_bytes().end());
    write32(broken, field + 68, 2); // dirty state is a boolean
    CHECK(!restored.restore(broken));
}

} // namespace

int main() {
    original_preset_keeps_its_state_and_guides();
    route_presets_keep_distinct_formats_and_goal_regions();
    regional_blocked_edge_keeps_an_unaffected_branch_ready();
    regional_blocked_building_edge_resumes_and_refreshes();
    regional_prefixes_continue_to_the_original_goal_after_restore();
    visible_first_corner_does_not_require_a_short_anchor_turn();
    a_blocked_first_corner_keeps_the_start_anchor();
    unrelated_revisions_keep_complete_routes_and_admissions();
    changing_revisions_cannot_restart_building_frontiers();
    periodic_refresh_discovers_a_new_bridge();
    dirty_pending_state_resumes_with_the_same_budget_and_refresh_order();
    pending_map_revision_keeps_the_same_age_across_a_load_pass();
    periodic_refresh_keeps_older_destinations_ahead_of_recent_refreshes();
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
