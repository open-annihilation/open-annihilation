// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace oa::sim::match_runtime {

/// Cumulative Enhanced work; observations never affect simulation decisions.
struct EnhancedCounters {
    uint64_t shared_route_requests{}, shared_route_hits{}, shared_route_fields{};
    uint64_t shared_route_cells{}, shared_route_waits{}, shared_route_invalidations{};
    uint64_t local_flow_probes{}, moving_blockers{}, steering_adjustments{};
    uint64_t yielded_ticks{}, blocked_ticks{}, deferred_replans{};
    uint64_t target_queries{}, target_candidates{}, target_full_scans{};
    uint64_t full_route_searches{};
};

} // namespace oa::sim::match_runtime
