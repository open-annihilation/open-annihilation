// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/navigator.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/sim/unit_movement/movement.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {

/// Tests an actual shared-route fallback for an old, physically motionless search.
///
/// Desired speed may remain nonzero against a static wall; the last actual
/// movement tick distinguishes that case from a moving unit. Mobile queues
/// retain the ordinary cursor's share of the existing search budget.
[[nodiscard]] inline bool preferred_fallback_search(
    const sim::ground_orders::Navigation& navigation,
    const sim::unit_movement::Movement& movement,
    const sim::unit_movement::LocalFlowHead& traffic,
    uint32_t tick
) noexcept {
    constexpr uint32_t stationary_ticks = sim::ground_orders::search_retry_ticks;
    constexpr uint32_t stale_search_ticks = 2 * sim::ground_orders::search_retry_ticks;
    return navigation.goal && navigation.goal->order && navigation.needs_search() &&
           !traffic.mobile_blocked && tick - movement.last_motion_tick >= stationary_ticks &&
           tick - navigation.last_search_tick >= stale_search_ticks;
}

/// Keeps an offered search bound to the same owned native Goal geometry.
[[nodiscard]] inline bool same_preferred_search_goal(
    const sim::ground_orders::Goal& a, const sim::ground_orders::Goal& b
) noexcept {
    return a.order == b.order && a.shape == b.shape && a.cell == b.cell &&
           a.tolerance == b.tolerance && a.radius_squared == b.radius_squared &&
           a.inner_range == b.inner_range && a.inner_radius_squared == b.inner_radius_squared &&
           a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}

} // namespace oa::sim::match_runtime
