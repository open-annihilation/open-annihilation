// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Spatial target queries keep the sightings order, fixed-point range quirks,
// radar fallback and sampled target choice while resolving fewer distant units.
#include "target_query.hpp"
#include "oa/sim/combat_state.hpp"
#include "oa/test/check.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {
namespace combat = oa::sim::combat_state;
using oa::sim::match_runtime::nearby_target_sightings;

constexpr uint32_t fixed_one = 1u << 16;

struct Fixture final : combat::IntelligenceHost, combat::TargetSearchHost {
    oa::World world{};
    oa::sim::spatial_state::World spatial;
    std::vector<oa::Unit> units;
    std::vector<combat::TargetUnit> projections;
    std::vector<uint16_t> seen;
    std::vector<uint16_t> radar;
    std::vector<uint16_t> extras;
    std::vector<uint16_t> scratch;
    std::vector<combat::TargetUnit> found;
    std::vector<uint32_t> random_bounds;
    std::vector<combat::UnitIdentity> reached;
    uint32_t random_state{51};
    size_t resolutions{};
    bool accelerated{};
    bool radar_fallback{true};
    int32_t radius{160};

    explicit Fixture(size_t count, uint32_t width = 32, uint32_t height = 40)
        : units(count + 1), projections(count + 1), scratch(2 * (count + 1)) {
        world.units = units.data();
        world.unit_slot_count = static_cast<uint32_t>(units.size());
        spatial.bucket_width = width;
        spatial.bucket_height = height;
        spatial.buckets.resize(static_cast<size_t>(width) * height);
        found.reserve(count);
    }

    void add(uint16_t slot, uint32_t x, uint32_t z, bool outside = false, bool carried = false) {
        auto& unit = units[slot];
        unit.position.x = x;
        unit.position.z = z;
        unit.flags = OA_UNIT_FLAG_LIVE;
        unit.type_index = 1;
        seen.push_back(slot);
        radar.push_back(slot);
        if (carried) {
            unit.attach_parent = oa::oa_unit_ref_from_slot(1);
            extras.push_back(slot);
            return;
        }
        auto& bucket =
            outside
                ? spatial.outside_bucket
                : spatial.buckets[static_cast<size_t>(z >> 23) * spatial.bucket_width + (x >> 23)];
        unit.attach_next = oa::oa_unit_ref_from_slot(bucket.head);
        bucket.head = slot;
    }

    bool unit_active(combat::UnitIdentity identity) override {
        return identity < units.size() && oa::unit_is_live_target(units[identity].flags);
    }

    const combat::TargetUnit* resolve(combat::UnitIdentity identity) override {
        ++resolutions;
        if (identity >= units.size() || units[identity].type_index == 0)
            return nullptr;
        const auto& unit = units[identity];
        projections[identity] = {
            identity,
            2,
            {static_cast<uint32_t>(unit.position.x),
             static_cast<uint32_t>(unit.position.y),
             static_cast<uint32_t>(unit.position.z)},
            unit.flags,
            0,
            static_cast<uint16_t>(unit.type_index),
            combat::candidate_type_auto_target_flag
        };
        return &projections[identity];
    }

    int32_t
    search_radius(const combat::TargetSource&, const combat::TargetSearchRequest&) override {
        return radius;
    }

    std::span<combat::TargetUnit>
    gather_nearby(uint8_t, const std::array<uint32_t, 3>& center, int32_t reach) override {
        std::span<const uint16_t> visible = seen;
        std::span<const uint16_t> detected = radar;
        if (accelerated) {
            const auto nearby = nearby_target_sightings(
                world, spatial, extras, visible, detected, center, reach, scratch
            );
            if (nearby.complete) {
                visible = nearby.seen;
                detected = nearby.radar;
            }
        }
        found.clear();
        combat::gather_sightings(visible, detected, radar_fallback, center, reach, *this, found);
        return found;
    }

    uint32_t random_bounded(uint32_t limit) override {
        random_bounds.push_back(limit);
        random_state = random_state * 1664525u + 1013904223u;
        return limit == 0 ? 0 : random_state % limit;
    }

    bool weapon_can_reach(
        const combat::TargetSource&, const combat::TargetUnit& target, uint8_t
    ) override {
        reached.push_back(target.identity);
        return target.identity % 7 != 0;
    }

    bool global_target_override() const noexcept override { return false; }

    std::vector<combat::UnitIdentity>
    identities(const std::array<uint32_t, 3>& center, int32_t reach, bool use_buckets) {
        accelerated = use_buckets;
        gather_nearby(0, center, reach);
        std::vector<combat::UnitIdentity> result;
        for (const auto& target : found)
            result.push_back(target.identity);
        return result;
    }
};

void keeps_order_carried_units_outside_units_and_fractional_edges() {
    Fixture fixture(8);
    fixture.add(1, 129 * fixed_one, 128 * fixed_one);
    fixture.add(2, 128 * fixed_one, 128 * fixed_one, true);
    fixture.add(3, 128 * fixed_one, 128 * fixed_one, false, true);
    fixture.add(4, 128 * fixed_one, 128 * fixed_one);
    fixture.add(5, 1900 * fixed_one, 1900 * fixed_one);
    fixture.add(6, 128 * fixed_one, 128 * fixed_one);
    fixture.units[6].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    fixture.add(7, 128 * fixed_one, 128 * fixed_one);
    fixture.units[7].type_index = 0;
    fixture.add(8, 128 * fixed_one + fixed_one / 4, 128 * fixed_one + fixed_one / 4);
    const std::array<uint32_t, 3> center{
        128 * fixed_one - fixed_one / 4, 0, 128 * fixed_one - fixed_one / 4
    };
    const auto full = fixture.identities(center, 1, false);
    OA_CHECK((full == std::vector<combat::UnitIdentity>{1, 2, 3, 4, 8}));
    OA_CHECK(fixture.identities(center, 1, true) == full);
    OA_CHECK(fixture.identities(center, 0, true) == fixture.identities(center, 0, false));
}

void uses_radar_only_when_the_seen_walk_finds_nothing() {
    Fixture fixture(3);
    fixture.add(1, 128 * fixed_one, 128 * fixed_one);
    fixture.add(2, 130 * fixed_one, 128 * fixed_one);
    fixture.add(3, 1900 * fixed_one, 1900 * fixed_one);
    const std::array<uint32_t, 3> center{128 * fixed_one, 0, 128 * fixed_one};
    fixture.seen = {1, 3};
    fixture.radar = {2, 3};
    OA_CHECK((fixture.identities(center, 10, true) == std::vector<combat::UnitIdentity>{1}));
    fixture.units[1].flags |= OA_UNIT_FLAG_DEATH_PENDING;
    OA_CHECK((fixture.identities(center, 10, true) == std::vector<combat::UnitIdentity>{2}));
    fixture.radar_fallback = false;
    OA_CHECK(fixture.identities(center, 10, true).empty());
}

void keeps_target_sampling_and_random_draws_for_more_than_fifty_candidates() {
    Fixture fixture(100);
    for (uint16_t id = 1; id <= 100; ++id)
        fixture.add(id, (100u + id) * fixed_one, (100u + id % 10) * fixed_one);
    combat::TargetSource source;
    source.identity = 200;
    source.owner = 1;
    source.position = {128 * fixed_one, 0, 128 * fixed_one};
    fixture.accelerated = false;
    const auto full_target = combat::select_automatic_target(source, {0, false}, fixture);
    const auto full_bounds = fixture.random_bounds;
    const auto full_reached = fixture.reached;
    const auto full_state = fixture.random_state;
    fixture.random_bounds.clear();
    fixture.reached.clear();
    fixture.random_state = 51;
    fixture.accelerated = true;
    OA_CHECK(combat::select_automatic_target(source, {0, false}, fixture) == full_target);
    OA_CHECK(fixture.random_bounds == full_bounds);
    OA_CHECK(fixture.reached == full_reached);
    OA_CHECK(fixture.random_state == full_state);
    OA_CHECK(fixture.reached.size() == combat::target_search_sample_limit);
}

void preserves_outside_wrapped_distance_and_unusual_radius_fallback() {
    Fixture fixture(3);
    fixture.add(1, 100 * fixed_one, 100 * fixed_one);
    fixture.add(2, 100 * fixed_one + 0x80000000u, 100 * fixed_one + 0x80000000u, true);
    fixture.add(3, static_cast<uint32_t>(-fixed_one), 100 * fixed_one, true);
    const std::array<uint32_t, 3> center{100 * fixed_one, 0, 100 * fixed_one};
    OA_CHECK((fixture.identities(center, 0, true) == std::vector<combat::UnitIdentity>{1, 2}));
    for (const int32_t radius :
         {-1, 0, 1, 32767, 46340, 46341, std::numeric_limits<int32_t>::max()})
        OA_CHECK(
            fixture.identities(center, radius, true) == fixture.identities(center, radius, false)
        );
    const std::array<uint32_t, 3> negative_center{
        static_cast<uint32_t>(-fixed_one), 0, 100 * fixed_one
    };
    OA_CHECK(
        fixture.identities(negative_center, 1, true) ==
        fixture.identities(negative_center, 1, false)
    );
}

void matches_full_walk_across_many_fractional_queries() {
    Fixture fixture(256);
    uint32_t state = 913;
    const auto random = [&]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    for (uint16_t id = 1; id <= 256; ++id) {
        const auto x = random() % (2000 * fixed_one);
        const auto z = random() % (2000 * fixed_one);
        fixture.add(id, x, z, id % 31 == 0, id % 29 == 0);
    }
    for (size_t query = 0; query < 1000; ++query) {
        const std::array<uint32_t, 3> center{
            random() % (2000 * fixed_one), 0, random() % (2000 * fixed_one)
        };
        const auto radius = static_cast<int32_t>(random() % 1000);
        OA_CHECK(
            fixture.identities(center, radius, true) == fixture.identities(center, radius, false)
        );
    }
}

void resolves_local_targets_without_projecting_twenty_thousand_sightings() {
    constexpr uint16_t count = 20000;
    Fixture fixture(count);
    for (uint16_t id = 1; id <= count; ++id) {
        const uint32_t index = id - 1u;
        fixture.add(id, (index % 128 * 32 + 16) * fixed_one, (index / 128 * 32 + 16) * fixed_one);
    }
    const std::array<uint32_t, 3> center{1024 * fixed_one, 0, 1024 * fixed_one};
    const auto full = fixture.identities(center, 160, false);
    OA_CHECK(fixture.resolutions == count);
    fixture.resolutions = 0;
    OA_CHECK(fixture.identities(center, 160, true) == full);
    OA_CHECK(fixture.resolutions < count / 10);
}

void falls_back_on_insufficient_scratch_and_broken_bucket_chains() {
    Fixture fixture(2);
    fixture.add(1, 128 * fixed_one, 128 * fixed_one);
    fixture.add(2, 129 * fixed_one, 128 * fixed_one);
    const std::array<uint32_t, 3> center{128 * fixed_one, 0, 128 * fixed_one};
    const auto full = fixture.identities(center, 10, false);
    fixture.scratch.resize(1);
    OA_CHECK(fixture.identities(center, 10, true) == full);
    fixture.scratch.resize(2 * fixture.units.size());
    fixture.units[1].attach_next = oa::oa_unit_ref_from_slot(2);
    OA_CHECK(fixture.identities(center, 10, true) == full);
}
} // namespace

int main() {
    keeps_order_carried_units_outside_units_and_fractional_edges();
    uses_radar_only_when_the_seen_walk_finds_nothing();
    keeps_target_sampling_and_random_draws_for_more_than_fifty_candidates();
    preserves_outside_wrapped_distance_and_unusual_radius_fallback();
    matches_full_walk_across_many_fractional_queries();
    resolves_local_targets_without_projecting_twenty_thousand_sightings();
    falls_back_on_insufficient_scratch_and_broken_bucket_chains();
    return oa::test::check_exit_status();
}
