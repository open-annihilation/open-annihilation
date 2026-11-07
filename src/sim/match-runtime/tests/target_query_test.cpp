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
    std::vector<uint8_t> membership;
    std::vector<combat::TargetUnit> found;
    std::vector<uint32_t> random_bounds;
    std::vector<combat::UnitIdentity> reached;
    uint32_t random_state{51};
    size_t resolutions{};
    size_t accelerated_queries{};
    size_t fallback_queries{};
    bool accelerated{};
    bool indexed{true};
    bool radar_fallback{true};
    int32_t radius{160};

    explicit Fixture(size_t count, uint32_t width = 32, uint32_t height = 40)
        : units(count + 1), projections(count + 1), scratch(2 * (count + 1)),
          membership(count + 1) {
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
            if (indexed)
                OA_CHECK(
                    oa::sim::match_runtime::rebuild_target_sighting_membership(
                        seen, radar, membership
                    )
                );
            const auto nearby = nearby_target_sightings(
                world,
                spatial,
                extras,
                visible,
                detected,
                center,
                reach,
                scratch,
                indexed ? std::span<const uint8_t>{membership} : std::span<const uint8_t>{},
                radar_fallback
            );
            if (nearby.complete) {
                ++accelerated_queries;
                visible = nearby.seen;
                detected = nearby.radar;
            } else
                ++fallback_queries;
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
    Fixture fixture(1000);
    for (uint16_t id = 1; id <= 100; ++id)
        fixture.add(id, (100u + id) * fixed_one, (100u + id % 10) * fixed_one);
    for (uint16_t id = 101; id <= 1000; ++id)
        fixture.add(id, (2000u + id % 32 * 32) * fixed_one, (2000u + id / 32 * 32) * fixed_one);
    combat::TargetSource source;
    source.identity = 200;
    source.owner = 1;
    source.position = {128 * fixed_one, 0, 128 * fixed_one};
    for (const int32_t radius : {160, 6000}) {
        fixture.radius = radius;
        fixture.random_bounds.clear();
        fixture.reached.clear();
        fixture.random_state = 51;
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
    OA_CHECK(fixture.accelerated_queries == 1);
    OA_CHECK(fixture.fallback_queries == 1);
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
        const auto full = fixture.identities(center, radius, false);
        fixture.indexed = true;
        OA_CHECK(fixture.identities(center, radius, true) == full);
        fixture.indexed = false;
        OA_CHECK(fixture.identities(center, radius, true) == full);
    }
    OA_CHECK(fixture.accelerated_queries != 0);
    OA_CHECK(fixture.fallback_queries != 0);
}

void indexes_only_known_nearby_slots_and_replaces_old_membership() {
    using oa::sim::match_runtime::rebuild_target_sighting_membership;
    Fixture fixture(1024);
    for (uint16_t id = 1; id <= 1024; ++id)
        fixture.add(
            id, (id <= 96 ? 128u : 2000u) * fixed_one, (id <= 96 ? 128u : 2000u) * fixed_one
        );
    fixture.seen = {2, 7};
    fixture.radar = {3, 8};
    for (uint16_t id = 97; id <= 1024; ++id) {
        fixture.seen.push_back(id);
        fixture.radar.push_back(id);
    }
    // A duplicate supplied through extras still appears only once.
    fixture.extras.push_back(2);
    const std::array<uint32_t, 3> center{128 * fixed_one, 0, 128 * fixed_one};
    const auto list = nearby_target_sightings(
        fixture.world,
        fixture.spatial,
        fixture.extras,
        fixture.seen,
        fixture.radar,
        center,
        32,
        fixture.scratch
    );
    OA_CHECK(list.complete && list.candidates == 96);
    const std::vector<uint16_t> seen(list.seen.begin(), list.seen.end());
    const std::vector<uint16_t> radar(list.radar.begin(), list.radar.end());
    OA_CHECK(rebuild_target_sighting_membership(fixture.seen, fixture.radar, fixture.membership));
    const auto indexed = nearby_target_sightings(
        fixture.world,
        fixture.spatial,
        fixture.extras,
        fixture.seen,
        fixture.radar,
        center,
        32,
        fixture.scratch,
        fixture.membership
    );
    OA_CHECK(indexed.complete && indexed.candidates == 4);
    OA_CHECK(std::equal(indexed.seen.begin(), indexed.seen.end(), seen.begin(), seen.end()));
    OA_CHECK(std::equal(indexed.radar.begin(), indexed.radar.end(), radar.begin(), radar.end()));

    // Ignoring radar results must not reduce the raw bucket budget to the
    // small seen list's minimum and force a full scan of this local query.
    const std::array<uint16_t, 2> small_seen{2, 7};
    std::vector<uint16_t> many_radar;
    for (uint16_t id = 1; id <= 1024; ++id)
        many_radar.push_back(id);
    OA_CHECK(rebuild_target_sighting_membership(small_seen, many_radar, fixture.membership));
    const auto no_radar = nearby_target_sightings(
        fixture.world,
        fixture.spatial,
        fixture.extras,
        small_seen,
        many_radar,
        center,
        32,
        fixture.scratch,
        fixture.membership,
        false
    );
    OA_CHECK(no_radar.complete && no_radar.candidates == 2);
    OA_CHECK(
        std::equal(no_radar.seen.begin(), no_radar.seen.end(), small_seen.begin(), small_seen.end())
    );
    OA_CHECK(no_radar.radar.empty());

    const std::array<uint16_t, 2> next_seen{3, 10};
    const std::array<uint16_t, 1> next_radar{7};
    OA_CHECK(rebuild_target_sighting_membership(next_seen, next_radar, fixture.membership));
    OA_CHECK(fixture.membership[2] == 0);
    OA_CHECK(fixture.membership[3] == oa::sim::match_runtime::target_sighting_seen);
    OA_CHECK(fixture.membership[7] == oa::sim::match_runtime::target_sighting_radar);
    OA_CHECK(fixture.membership[8] == 0);
    const std::array<uint16_t, 1> reserved{0}, outside{1025};
    OA_CHECK(!rebuild_target_sighting_membership(reserved, {}, fixture.membership));
    OA_CHECK(!rebuild_target_sighting_membership(outside, {}, fixture.membership));
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
    OA_CHECK(fixture.accelerated_queries == 1);
    OA_CHECK(fixture.fallback_queries == 0);
}

void keeps_broad_and_saturated_queries_on_the_linear_walk() {
    constexpr uint16_t count = 20000;
    Fixture fixture(count);
    for (uint16_t id = 1; id <= count; ++id)
        fixture.add(id, (1024u + id % 32) * fixed_one, (1024u + id / 32 % 32) * fixed_one);
    const std::array<uint32_t, 3> center{1024 * fixed_one, 0, 1024 * fixed_one};
    for (const int32_t radius : {160, 6000}) {
        const auto nearby = nearby_target_sightings(
            fixture.world,
            fixture.spatial,
            fixture.extras,
            fixture.seen,
            fixture.radar,
            center,
            radius,
            fixture.scratch
        );
        OA_CHECK(!nearby.complete);
        OA_CHECK(
            fixture.identities(center, radius, true) == fixture.identities(center, radius, false)
        );
    }
    OA_CHECK(fixture.accelerated_queries == 0);
    OA_CHECK(fixture.fallback_queries == 2);
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
    indexes_only_known_nearby_slots_and_replaces_old_membership();
    keeps_order_carried_units_outside_units_and_fractional_edges();
    uses_radar_only_when_the_seen_walk_finds_nothing();
    keeps_target_sampling_and_random_draws_for_more_than_fifty_candidates();
    preserves_outside_wrapped_distance_and_unusual_radius_fallback();
    matches_full_walk_across_many_fractional_queries();
    resolves_local_targets_without_projecting_twenty_thousand_sightings();
    keeps_broad_and_saturated_queries_on_the_linear_walk();
    falls_back_on_insufficient_scratch_and_broken_bucket_chains();
    return oa::test::check_exit_status();
}
