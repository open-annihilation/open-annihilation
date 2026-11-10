// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Spatial strike fronts retain the native enemy rules and canonical world.
#include "../src/strike_front_targets.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {
bool watch_allocations{};
size_t allocation_calls{};
int failures{};
} // namespace

void* operator new(size_t count) {
    if (watch_allocations)
        ++allocation_calls;
    if (void* memory = std::malloc(count != 0 ? count : 1))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](size_t count) {
    return ::operator new(count);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, size_t) noexcept {
    std::free(memory);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using namespace oa::sim::ai;
namespace simulation = oa::sim::simulation_state;

namespace {

/// Releases the World record while the fixture owns its unit storage.
struct FreeWorld {
    void operator()(oa::World* world) const noexcept { std::free(world); }
};

/// Holds canonical player spans without game assets or runtime callbacks.
struct Fixture {
    std::unique_ptr<oa::World, FreeWorld> world{oa::world_create()};
    std::vector<oa::Unit> units;
    std::array<uint8_t, OA_PLAYER_COUNT + 1> relation{};
    uint32_t own_count{};

    Fixture(uint32_t own, uint32_t enemies) : units(own + enemies + 1), own_count(own) {
        if (!world)
            throw std::bad_alloc();
        world->units = units.data();
        world->unit_slot_count = static_cast<uint32_t>(units.size());
        range(0, 1, own);
        range(1, own + 1, enemies);
        relation[0] = relation[OA_PLAYER_COUNT] = 1;
        for (uint32_t slot = 1; slot < units.size(); ++slot) {
            auto& unit = units[slot];
            unit.id = static_cast<uint16_t>(slot);
            unit.type_index = 1;
            unit.owner_index = slot <= own ? 0 : 1;
            unit.flags = OA_UNIT_FLAG_LIVE;
            unit.health = 100;
            unit.squad = slot <= own ? static_cast<int32_t>(Squad::land_strike) : 0;
        }
    }

    /// Binds one inclusive native player span.
    void range(uint8_t slot, uint32_t first, uint32_t count) {
        auto& player = world->game.players[slot];
        player.index = slot;
        player.in_use = 1;
        player.status = 1;
        player.first_unit = count != 0 ? oa::oa_unit_ref_from_slot(first) : 0;
        player.last_unit = count != 0 ? oa::oa_unit_ref_from_slot(first + count - 1) : 0;
    }

    /// Returns the native target already chosen from the original centroid.
    const oa::Unit* fallback(const oa::FixedVec3& center, simulation::CandidateFilter filter = {}) {
        return simulation::nearest_candidate_unit(*world, relation, center.x, center.z, filter);
    }
};

/// Places a canonical unit without signed shifts.
void position(oa::Unit& unit, int32_t x, int32_t z, uint32_t fraction = 0) {
    unit.position = {
        static_cast<oa::oa_fixed>(int64_t(x) * 65'536 + fraction),
        static_cast<oa::oa_fixed>(int64_t(x % 7) * 65'536),
        static_cast<oa::oa_fixed>(int64_t(z) * 65'536 + fraction)
    };
}

/// Checks each populated front against a separate native nearest query.
void native_fronts(
    Fixture& fixture, const StrikeFrontTargets& plan, simulation::CandidateFilter filter = {}
) {
    CHECK(plan.enabled);
    if (!plan.enabled)
        return;
    for (uint32_t front = 0; front < strike_front_count; ++front) {
        if (plan.member_counts[front] == 0)
            continue;
        const auto& center = plan.centers[front];
        const auto* native = simulation::nearest_candidate_unit(
            *fixture.world, fixture.relation, center.x, center.z, filter
        );
        CHECK(plan.targets[front] == (native != nullptr ? native : plan.fallback));
    }
}

/// Exercises twenty thousand units across mirrored and rotated battle fronts.
void large_spatial_fronts_follow_native_targets() {
    Fixture fixture(10'000, 10'000);
    for (uint32_t turn = 0; turn < 4; ++turn) {
        for (uint32_t i = 0; i < 10'000; ++i) {
            const int32_t lateral = static_cast<int32_t>(i) - 5000;
            for (uint32_t side = 0; side < 2; ++side) {
                auto& unit = fixture.units[1 + side * 10'000 + i];
                const auto forward = side == 0 ? -1200 : 1200;
                int32_t x = forward, z = lateral;
                if (turn == 1)
                    x = -x;
                if (turn == 2) {
                    x = lateral;
                    z = forward;
                }
                if (turn == 3) {
                    x = -lateral;
                    z = -forward;
                }
                position(unit, x, z, (i % 4) * 1000);
            }
        }
        oa::FixedVec3 center{};
        if (turn == 0 || turn == 1)
            center.x = (turn == 0 ? -1200 : 1200) * 65'536;
        else
            center.z = (turn == 2 ? -1200 : 1200) * 65'536;
        const auto* fallback = fixture.fallback(center);
        CHECK(fallback != nullptr);
        if (!fallback)
            return;
        StrikeFrontTargets plan;
        plan_strike_front_targets(
            *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
        );
        CHECK(plan.axis == ((turn < 2) ? StrikeFrontAxis::z : StrikeFrontAxis::x));
        native_fronts(fixture, plan);
        uint32_t members{}, distinct{};
        for (uint32_t front = 0; front < strike_front_count; ++front) {
            members += plan.member_counts[front];
            CHECK(plan.member_counts[front] > 0);
            bool first = true;
            for (uint32_t earlier = 0; earlier < front; ++earlier)
                first &= plan.targets[earlier] != plan.targets[front];
            distinct += first;
        }
        CHECK(members == 10'000 && distinct == 16);
        std::array<uint32_t, strike_front_count> expected_counts{};
        std::array<std::array<int64_t, 3>, strike_front_count> expected_sums{};
        const int32_t expected_minimum = turn == 3 ? -4999 : -5000;
        CHECK(plan.minimum_lateral == expected_minimum);
        CHECK(plan.maximum_lateral == expected_minimum + 9999);
        for (uint32_t i = 1; i <= fixture.own_count; ++i) {
            const auto& unit = fixture.units[i];
            const auto coordinate = static_cast<int32_t>(static_cast<int16_t>(
                static_cast<uint32_t>(
                    plan.axis == StrikeFrontAxis::x ? unit.position.x : unit.position.z
                ) >>
                16
            ));
            const auto front = std::min<uint32_t>(
                15, static_cast<uint32_t>(int64_t(coordinate - expected_minimum) * 16 / 9999)
            );
            CHECK(strike_front_target(plan, unit) == plan.targets[front]);
            ++expected_counts[front];
            expected_sums[front][0] +=
                static_cast<int16_t>(static_cast<uint32_t>(unit.position.x) >> 16);
            expected_sums[front][1] +=
                static_cast<int16_t>(static_cast<uint32_t>(unit.position.y) >> 16);
            expected_sums[front][2] +=
                static_cast<int16_t>(static_cast<uint32_t>(unit.position.z) >> 16);
        }
        for (uint32_t front = 0; front < strike_front_count; ++front) {
            CHECK(plan.member_counts[front] == expected_counts[front]);
            CHECK(
                plan.centers[front].x == expected_sums[front][0] / expected_counts[front] * 65'536
            );
            CHECK(
                plan.centers[front].y == expected_sums[front][1] / expected_counts[front] * 65'536
            );
            CHECK(
                plan.centers[front].z == expected_sums[front][2] / expected_counts[front] * 65'536
            );
        }
    }
}

/// Preserves every native candidate filter, including unseen eligible units and ties.
void candidate_filters_and_first_minima_match_native() {
    Fixture fixture(16, 24);
    for (uint32_t i = 1; i <= 16; ++i)
        position(fixture.units[i], -400, static_cast<int32_t>(i) * 10 - 80);
    for (uint32_t i = 17; i < fixture.units.size(); ++i)
        position(fixture.units[i], 400, static_cast<int32_t>(i - 17) * 8 - 80);
    fixture.units[17].type_index = 0; // Native enemy eligibility does not test the type id.
    fixture.units[18].flags = 0;
    fixture.units[19].state_flags = OA_UNIT_STATE_CLOAKED;
    fixture.units[20].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    fixture.units[21].health = 0;
    fixture.units[22].health = -1;
    fixture.units[23].flags |= 2;
    fixture.units[24].flags |= 3;
    fixture.units[25].flags |= OA_UNIT_FLAG_NOT_SELECTABLE;
    fixture.units[26].flags |= 8;
    const uint32_t submerged = 3;
    std::memcpy(fixture.units[27].last_occupy_code, &submerged, sizeof submerged);
    const oa::FixedVec3 center{-400 * 65'536, 0, 0};
    for (uint32_t bits = 0; bits < 8; ++bits) {
        const simulation::CandidateFilter filter{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0};
        const auto* fallback = fixture.fallback(center, filter);
        CHECK(fallback != nullptr);
        if (!fallback)
            return;
        StrikeFrontTargets plan;
        plan_strike_front_targets(
            *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, filter, plan
        );
        native_fronts(fixture, plan, filter);
    }
    // Identical candidates choose native player-slot and unit-span order,
    // without visibility, forced uniqueness, or sorting by unit id.
    fixture.range(1, 17, 12);
    fixture.range(2, 29, 12);
    for (uint32_t i = 17; i < fixture.units.size(); ++i) {
        fixture.units[i] = {};
        fixture.units[i].id = static_cast<uint16_t>(100 - i);
        fixture.units[i].flags = OA_UNIT_FLAG_LIVE;
        fixture.units[i].health = 100;
        position(fixture.units[i], 400, 0);
    }
    const auto* fallback = fixture.fallback(center);
    CHECK(fallback == &fixture.units[17]);
    StrikeFrontTargets plan;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    native_fronts(fixture, plan);
    for (uint32_t front = 0; front < strike_front_count; ++front)
        CHECK(plan.member_counts[front] == 0 || plan.targets[front] == &fixture.units[17]);
    fixture.world->game.players[1].index = 3;
    fixture.relation[3] = 1;
    fallback = fixture.fallback(center);
    CHECK(fallback == &fixture.units[29]);
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    native_fronts(fixture, plan);
    fixture.world->game.players[2].status = 3;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    native_fronts(fixture, plan);
}

/// Retains the real fallback when spatial planning has no supported lateral span.
void narrow_empty_and_unsupported_inputs_keep_the_fallback() {
    Fixture fixture(32, 1);
    for (uint32_t i = 1; i <= 32; ++i)
        position(fixture.units[i], static_cast<int32_t>(i) * 20 - 700, 10);
    position(fixture.units[33], 1200, 10);
    const oa::FixedVec3 center{};
    auto* fallback = &fixture.units[33];
    StrikeFrontTargets plan;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    CHECK(!plan.enabled && plan.axis == StrikeFrontAxis::z);
    for (uint32_t i = 1; i <= 32; ++i)
        CHECK(strike_front_target(plan, fixture.units[i]) == fallback);
    // A two-coordinate lateral range occupies sparse spatial strips.
    fixture.units[1].position.z += 65'536;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    native_fronts(fixture, plan);
    CHECK(plan.member_counts[0] == 31 && plan.member_counts[15] == 1);
    for (uint32_t front = 1; front < 15; ++front)
        CHECK(plan.member_counts[front] == 0);
    fixture.units[2].type_index = 0;
    fixture.units[3].squad = static_cast<int32_t>(Squad::naval_strike);
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    CHECK(plan.member_counts[0] == 29 && plan.member_counts[15] == 1);
    CHECK(strike_front_target(plan, fixture.units[2]) == fallback);
    CHECK(strike_front_target(plan, fixture.units[3]) == fallback);
    for (const auto squad : {Squad::naval_strike, Squad::aircraft, Squad::none}) {
        plan_strike_front_targets(
            *fixture.world, 0, squad, center, *fallback, fixture.relation, {}, plan
        );
        CHECK(!plan.enabled && strike_front_target(plan, fixture.units[1]) == fallback);
    }
    fixture.world->game.players[0].first_unit = fixture.world->game.players[0].last_unit = 0;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    CHECK(!plan.enabled && strike_front_target(plan, fixture.units[1]) == fallback);
    fixture.range(0, 1, 32);
    fixture.units[33].flags = 0;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    CHECK(!plan.enabled && strike_front_target(plan, fixture.units[1]) == fallback);
    Fixture no_enemies(2, 0);
    position(no_enemies.units[1], -10, -10);
    position(no_enemies.units[2], -10, 10);
    oa::Unit previous_target{};
    position(previous_target, 100, 0);
    CHECK(no_enemies.fallback(center) == nullptr);
    plan_strike_front_targets(
        *no_enemies.world,
        0,
        Squad::land_strike,
        center,
        previous_target,
        no_enemies.relation,
        {},
        plan
    );
    CHECK(!plan.enabled && strike_front_target(plan, no_enemies.units[1]) == &previous_target);
    fixture.units[33].flags = OA_UNIT_FLAG_LIVE;
    plan_strike_front_targets(
        *fixture.world,
        0,
        Squad::land_strike,
        center,
        *fallback,
        std::span<const uint8_t>(fixture.relation).first(1),
        {},
        plan
    );
    CHECK(!plan.enabled && strike_front_target(plan, fixture.units[1]) == fallback);
}

/// Keeps wrapped direction, extreme distance and signed centroid arithmetic native.
void wrapped_axes_and_costs_match_native() {
    Fixture fixture(2, 2);
    position(fixture.units[1], -10, -20);
    position(fixture.units[2], 10, 20);
    fixture.units[3].position = {std::numeric_limits<int32_t>::min(), 0, 100 * 65'536};
    fixture.units[4].flags = 0;
    oa::FixedVec3 center{std::numeric_limits<int32_t>::max(), 0, 0};
    StrikeFrontTargets plan;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, fixture.units[3], fixture.relation, {}, plan
    );
    CHECK(plan.axis == StrikeFrontAxis::x);
    native_fronts(fixture, plan);
    fixture.units[3].position = {std::numeric_limits<int32_t>::min(), 0, 0};
    center = {};
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, fixture.units[3], fixture.relation, {}, plan
    );
    CHECK(plan.axis == StrikeFrontAxis::z);
    native_fronts(fixture, plan);
    fixture.units[3].position = {100 * 65'536, 0, 100 * 65'536};
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, fixture.units[3], fixture.relation, {}, plan
    );
    CHECK(plan.axis == StrikeFrontAxis::z);
    native_fronts(fixture, plan);
    center = fixture.units[3].position;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, fixture.units[3], fixture.relation, {}, plan
    );
    CHECK(plan.axis == StrikeFrontAxis::z);
    native_fronts(fixture, plan);
}

/// Leaves canonical bytes and random state untouched without heap allocation.
void planning_is_transient_and_allocation_free() {
    Fixture fixture(128, 128);
    for (uint32_t i = 1; i <= 128; ++i) {
        position(fixture.units[i], -300, static_cast<int32_t>(i) - 64, i % 3);
        position(fixture.units[i + 128], 300, static_cast<int32_t>(i) - 64, i % 5);
    }
    const oa::FixedVec3 center{-300 * 65'536, 0, 0};
    const auto* fallback = fixture.fallback(center);
    CHECK(fallback != nullptr);
    if (!fallback)
        return;
    const auto before_game = std::make_unique<oa::Game>(fixture.world->game);
    const auto before_units = fixture.units;
    StrikeFrontTargets plan;
    allocation_calls = 0;
    watch_allocations = true;
    plan_strike_front_targets(
        *fixture.world, 0, Squad::land_strike, center, *fallback, fixture.relation, {}, plan
    );
    bool all_targets = true;
    for (uint32_t i = 1; i <= fixture.own_count; ++i)
        all_targets &= strike_front_target(plan, fixture.units[i]) != nullptr;
    watch_allocations = false;
    CHECK(allocation_calls == 0 && all_targets);
    CHECK(std::memcmp(before_game.get(), &fixture.world->game, sizeof(oa::Game)) == 0);
    CHECK(
        std::memcmp(
            before_units.data(), fixture.units.data(), fixture.units.size() * sizeof(oa::Unit)
        ) == 0
    );
}

} // namespace

int main() {
    large_spatial_fronts_follow_native_targets();
    candidate_filters_and_first_minima_match_native();
    narrow_empty_and_unsupported_inputs_keep_the_fallback();
    wrapped_axes_and_costs_match_native();
    planning_is_transient_and_allocation_free();
    return failures == 0 ? 0 : 1;
}
