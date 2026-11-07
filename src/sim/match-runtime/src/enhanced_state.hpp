// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/enhanced_counters.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/sim/ground_orders/shared_routes.hpp"

namespace oa::sim::match_runtime {

/// Preallocated v2 navigation and query storage, outside packed world records.
struct Match::EnhancedState {
    EnhancedCounters counters{};
    sim::unit_movement::LocalFlowCounters local_flow_counters{};
    std::unique_ptr<sim::unit_movement::LocalFlowHead[]> local_flow_heads{};
    std::unique_ptr<uint8_t[]> local_flow_save_bytes{};
    std::unique_ptr<uint8_t[]> local_flow_progress_bytes{};
    size_t unit_count{};
    std::unique_ptr<uint16_t[]> target_query_slots{}, target_query_extras{};
    size_t target_query_extra_count{};
    uint32_t target_query_extras_tick{};
    bool target_query_extras_valid{};
    std::unique_ptr<sim::ground_orders::FlowCache> shared_routes{};
    uint32_t shared_static_revision{};
    std::array<uint8_t, 4> shared_static_revision_bytes{};
};

} // namespace oa::sim::match_runtime
