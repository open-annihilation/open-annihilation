// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/enhanced_counters.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/sim/ground_orders/shared_routes.hpp"
#include "static_passability_memo.hpp"
#include "order_ownership.hpp"
#include "ai_retarget.hpp"

namespace oa::sim::match_runtime {

/// Tells whether a feature set builds shared routes, with any of the three
/// route builders.
///
/// @param features the match's Enhanced Mode features
/// @return true when the set holds a route builder
[[nodiscard]] constexpr bool
builds_shared_routes(data::enhanced_mode::FeatureSet features) noexcept {
    return features.has(data::enhanced_mode::Feature::routes_original) ||
           features.has(data::enhanced_mode::Feature::routes_large_regions) ||
           features.has(data::enhanced_mode::Feature::routes_fine_regions);
}

/// Tells whether a feature set indexes its target, sighting and order
/// searches, with either of the two indexed searches.
///
/// @param features the match's Enhanced Mode features
/// @return true when the set holds indexed_searches or exact_indexed_searches
[[nodiscard]] constexpr bool indexes_searches(data::enhanced_mode::FeatureSet features) noexcept {
    return features.has(data::enhanced_mode::Feature::indexed_searches) ||
           features.has(data::enhanced_mode::Feature::exact_indexed_searches);
}

/// Preallocated v2 navigation and query storage, outside packed world records.
struct Match::EnhancedState {
    /// Advances bounded target-loss checks before a built-in controller runs.
    ///
    /// @param[in,out] match the match and its canonical computer tasks
    /// @param player the built-in controller about to run
    void prepare_ai_retarget(Match& match, uint8_t player);

    /// Refreshes lost cohort attacks without recruiting or moving strike deadlines.
    ///
    /// @param[in,out] match the match and its canonical strike orders
    /// @param player the built-in controller that just ran
    void refresh_ai_cohort_targets(Match& match, uint8_t player);

    /// Retains a strike target issued within the built-in controller scope.
    ///
    /// @param[in,out] match the match containing the source and target
    /// @param source the squad member receiving the attack
    /// @param target the live enemy target slot
    void remember_ai_attack(Match& match, uint16_t source, uint16_t target);

    /// Retains native strike deadlines after the built-in controller dispatches.
    ///
    /// @param match the match containing the computer tasks
    /// @param player the built-in controller that finished
    void capture_ai_retarget(Match& match, uint8_t player);

    EnhancedCounters counters{};
    sim::unit_movement::LocalFlowCounters local_flow_counters{};
    std::unique_ptr<sim::unit_movement::LocalFlowHead[]> local_flow_heads{};
    std::unique_ptr<uint8_t[]> local_flow_save_bytes{};
    std::unique_ptr<uint8_t[]> local_flow_progress_bytes{};
    std::unique_ptr<uint32_t[]> local_flow_progress_at_tick_start{};
    uint32_t local_flow_progress_snapshot_tick{};
    bool local_flow_progress_snapshot_valid{};
    size_t unit_count{};
    std::unique_ptr<uint16_t[]> target_query_slots{}, target_query_extras{};
    size_t target_query_extra_count{};
    uint32_t target_query_extras_tick{};
    bool target_query_extras_valid{};
    std::unique_ptr<sim::ground_orders::FlowCache> shared_routes{};
    uint32_t shared_static_revision{};
    std::array<uint8_t, 4> shared_static_revision_bytes{};
    std::unique_ptr<uint8_t[]> target_sighting_membership{};
    std::array<bool, OA_PLAYER_COUNT> target_sighting_membership_valid{};
    std::array<uint8_t, 5> shared_route_scan_state{};
    bool air_goal_cleanup_required{};
    std::unique_ptr<StaticPassabilityMemo> static_passability_memo{};
    std::unique_ptr<OrderOwnershipIndex> order_ownership{};
    AiRetargetState ai_retarget{};
};

} // namespace oa::sim::match_runtime
