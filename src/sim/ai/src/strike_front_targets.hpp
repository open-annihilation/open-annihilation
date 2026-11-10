// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ai.hpp"
#include "oa/sim/simulation_state.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace oa::sim::ai {

inline constexpr uint32_t strike_front_count = 16;

/// The lateral coordinate across the strike's direction of attack.
enum class StrikeFrontAxis : uint8_t { x, z };

/// Targets selected for spatial strips of the current land strike.
struct StrikeFrontTargets {
    const oa::Unit* fallback{};
    std::array<const oa::Unit*, strike_front_count> targets{};
    std::array<oa::FixedVec3, strike_front_count> centers{};
    std::array<uint32_t, strike_front_count> member_counts{};
    int32_t minimum_lateral{}; ///< signed whole-world coordinate
    int32_t maximum_lateral{}; ///< signed whole-world coordinate
    StrikeFrontAxis axis{StrikeFrontAxis::z};
    Squad squad{Squad::none};
    bool enabled{};
};

namespace strike_front_detail {

/// Returns the signed high word used by the squad's position average.
///
/// @param position 16.16 world coordinate
/// @return its signed whole-world high word
inline int32_t whole(oa_fixed position) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(position) >> 16));
}

/// Tests the same typed membership that receives a squad command.
///
/// @param unit canonical squad member
/// @param squad the squad receiving the command
/// @return true when the typed unit belongs to that squad
inline bool member(const oa::Unit& unit, Squad squad) noexcept {
    return unit.type_index != 0 && unit.squad == static_cast<int32_t>(squad);
}

/// Returns the whole-world coordinate across the planned attack direction.
///
/// @param unit canonical squad member
/// @param axis the lateral coordinate selected for this dispatch
/// @return its signed whole-world coordinate on that axis
inline int32_t lateral(const oa::Unit& unit, StrikeFrontAxis axis) noexcept {
    return whole(axis == StrikeFrontAxis::x ? unit.position.x : unit.position.z);
}

/// Finds a spatial strip, keeping the maximum in the last strip.
///
/// @param position whole-world coordinate within the range
/// @param minimum smallest member coordinate
/// @param maximum greatest member coordinate, strictly above minimum
/// @return a strip from zero through fifteen
inline uint32_t strip(int32_t position, int32_t minimum, int32_t maximum) noexcept {
    const auto offset = static_cast<uint64_t>(int64_t(position) - minimum);
    const auto extent = static_cast<uint64_t>(int64_t(maximum) - minimum);
    return static_cast<uint32_t>(
        std::min(uint64_t{strike_front_count - 1}, offset * strike_front_count / extent)
    );
}

/// Tests the native nearest-candidate conditions, without a visibility test.
///
/// @param unit canonical enemy candidate
/// @param filter the native candidate filter for the strike
/// @return true when the candidate passes every native unit condition
inline bool
candidate(const oa::Unit& unit, const simulation_state::CandidateFilter& filter) noexcept {
    constexpr uint32_t occupancy_upper_states = 2;
    constexpr uint32_t skipped_occupancy_state = 2;
    constexpr uint32_t move_rate_upper_tiers = 8;
    constexpr uint32_t widened_skip =
        occupancy_upper_states | move_rate_upper_tiers | OA_UNIT_FLAG_NOT_SELECTABLE;
    constexpr uint32_t submerged_occupy_code = 3;
    const auto flags = unit.flags;
    if ((flags & OA_UNIT_FLAG_LIVE) == 0 || (unit.state_flags & OA_UNIT_STATE_CLOAKED) != 0)
        return false;
    if (filter.skip_dying && ((flags & OA_UNIT_FLAG_DEATH_PENDING) != 0 || unit.health <= 0))
        return false;
    if (filter.widened_flags) {
        if ((flags & widened_skip) != 0)
            return false;
    } else if (
        (flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == skipped_occupancy_state ||
        (flags & OA_UNIT_FLAG_NOT_SELECTABLE) != 0
    ) {
        return false;
    }
    if (filter.skip_submerged) {
        uint32_t occupy_code{};
        std::memcpy(&occupy_code, unit.last_occupy_code, sizeof occupy_code);
        if (occupy_code == submerged_occupy_code)
            return false;
    }
    return true;
}

/// Ranks a unit by the native wrapped X/Z differences and high square halves.
///
/// @param center strip centroid in 16.16 world coordinates
/// @param unit canonical enemy candidate
/// @return the signed native cost, including its wrapped sum
inline int32_t metric(const oa::FixedVec3& center, const oa::Unit& unit) noexcept {
    const auto dx = std::bit_cast<int32_t>(
        static_cast<uint32_t>(center.x) - static_cast<uint32_t>(unit.position.x)
    );
    const auto dz = std::bit_cast<int32_t>(
        static_cast<uint32_t>(center.z) - static_cast<uint32_t>(unit.position.z)
    );
    const auto x_high = static_cast<uint32_t>(static_cast<uint64_t>(int64_t(dx) * dx) >> 32);
    const auto z_high = static_cast<uint32_t>(static_cast<uint64_t>(int64_t(dz) * dz) >> 32);
    return std::bit_cast<int32_t>(x_high + z_high);
}

} // namespace strike_front_detail

/// Plans at most sixteen targets in one additional native-order enemy pass.
///
/// The dominant wrapped displacement to the actual fallback selects the
/// attack axis; ties bucket Z. Typed members of the actual squad span define
/// sixteen equal spatial strips on the lateral signed high-word range. Each
/// nonempty strip uses its own member centroid and the native first strict
/// minimum. Unsupported inputs and zero lateral range retain the fallback.
///
/// @param world canonical players and unit spans
/// @param player the player whose land strike is ordered
/// @param squad the squad receiving the command
/// @param center the original strike centroid, in 16.16 world coordinates
/// @param fallback the actual native target already selected for that centroid
/// @param relation native relation row indexed by candidate Player.index
/// @param filter the same candidate filter used for the original target
/// @param[out] out transient targets and centroids, without canonical state changes
inline void plan_strike_front_targets(
    oa::World& world,
    uint8_t player,
    Squad squad,
    const oa::FixedVec3& center,
    const oa::Unit& fallback,
    std::span<const uint8_t> relation,
    const simulation_state::CandidateFilter& filter,
    StrikeFrontTargets& out
) noexcept {
    out = {};
    out.fallback = &fallback;
    out.targets.fill(&fallback);
    out.squad = squad;
    if (player >= OA_PLAYER_COUNT || squad != Squad::land_strike)
        return;
    const auto dx = std::bit_cast<int32_t>(
        static_cast<uint32_t>(fallback.position.x) - static_cast<uint32_t>(center.x)
    );
    const auto dz = std::bit_cast<int32_t>(
        static_cast<uint32_t>(fallback.position.z) - static_cast<uint32_t>(center.z)
    );
    const auto abs_x = dx < 0 ? -int64_t(dx) : int64_t(dx);
    const auto abs_z = dz < 0 ? -int64_t(dz) : int64_t(dz);
    out.axis = abs_x >= abs_z ? StrikeFrontAxis::z : StrikeFrontAxis::x;
    uint32_t count{};
    auto* units = oa::world_player_units(&world, &world.game.players[player], &count);
    if (units == nullptr)
        return;
    bool any_member{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto& unit = units[i];
        if (!strike_front_detail::member(unit, squad))
            continue;
        const auto position = strike_front_detail::lateral(unit, out.axis);
        if (!any_member) {
            out.minimum_lateral = out.maximum_lateral = position;
            any_member = true;
        } else {
            out.minimum_lateral = std::min(out.minimum_lateral, position);
            out.maximum_lateral = std::max(out.maximum_lateral, position);
        }
    }
    if (!any_member || out.minimum_lateral == out.maximum_lateral)
        return;
    std::array<std::array<int64_t, 3>, strike_front_count> sums{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto& unit = units[i];
        if (!strike_front_detail::member(unit, squad))
            continue;
        const auto front = strike_front_detail::strip(
            strike_front_detail::lateral(unit, out.axis), out.minimum_lateral, out.maximum_lateral
        );
        ++out.member_counts[front];
        sums[front][0] += strike_front_detail::whole(unit.position.x);
        sums[front][1] += strike_front_detail::whole(unit.position.y);
        sums[front][2] += strike_front_detail::whole(unit.position.z);
    }
    for (uint32_t front = 0; front < strike_front_count; ++front) {
        const auto members = out.member_counts[front];
        if (members == 0)
            continue;
        constexpr int64_t fixed_unit = 65'536;
        out.centers[front] = {
            static_cast<oa_fixed>((sums[front][0] / members) * fixed_unit),
            static_cast<oa_fixed>((sums[front][1] / members) * fixed_unit),
            static_cast<oa_fixed>((sums[front][2] / members) * fixed_unit)
        };
    }
    std::array<int32_t, strike_front_count> costs;
    costs.fill(std::numeric_limits<int32_t>::max());
    bool fallback_admitted{};
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const auto& owner = world.game.players[index];
        if (!simulation_state::player_slot_active(index, owner))
            continue;
        if (owner.index >= relation.size())
            return;
        if (relation[owner.index] != 0)
            continue;
        uint32_t enemy_count{};
        auto* enemies = oa::world_player_units(&world, &owner, &enemy_count);
        for (uint32_t i = 0; i < enemy_count; ++i) {
            const auto& enemy = enemies[i];
            if (!strike_front_detail::candidate(enemy, filter))
                continue;
            fallback_admitted |= &enemy == &fallback;
            for (uint32_t front = 0; front < strike_front_count; ++front) {
                if (out.member_counts[front] == 0)
                    continue;
                const auto cost = strike_front_detail::metric(out.centers[front], enemy);
                if (cost < costs[front]) {
                    costs[front] = cost;
                    out.targets[front] = &enemy;
                }
            }
        }
    }
    out.enabled = fallback_admitted;
}

/// Returns the target for a current member, retaining the actual fallback otherwise.
///
/// @param plan the transient plan for this command dispatch
/// @param unit the current canonical unit receiving the command
/// @return its strip's target, or the actual fallback for an unsupported member
inline const oa::Unit*
strike_front_target(const StrikeFrontTargets& plan, const oa::Unit& unit) noexcept {
    if (!plan.enabled || !strike_front_detail::member(unit, plan.squad))
        return plan.fallback;
    const auto position = strike_front_detail::lateral(unit, plan.axis);
    if (position < plan.minimum_lateral || position > plan.maximum_lateral)
        return plan.fallback;
    return plan
        .targets[strike_front_detail::strip(position, plan.minimum_lateral, plan.maximum_lateral)];
}

} // namespace oa::sim::ai
