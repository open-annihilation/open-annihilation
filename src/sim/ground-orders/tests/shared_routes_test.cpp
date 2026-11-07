// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/shared_routes.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
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
        CHECK(
            after.cells_cleared + after.cells_expanded - before.cells_cleared -
                before.cells_expanded <=
            128
        );
    }
    finish(cache, grid);
    for (uint32_t i = 0; i < 1000; ++i) {
        const auto unit =
            unit_at(static_cast<int16_t>(4 + i % 24), static_cast<int16_t>(4 + (i / 24) % 24));
        const auto arrival =
            circle(static_cast<int16_t>(96 + i % 24), static_cast<int16_t>(96 + (i / 24) % 24));
        CHECK(query(cache, grid, unit, arrival) == FlowResult::ready);
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
} // namespace

int main() {
    shared_cohort_and_budget();
    narrow_choke_and_mixed_footprints();
    revisions_and_restore_pending();
    singletons_do_not_starve_building_fields();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
