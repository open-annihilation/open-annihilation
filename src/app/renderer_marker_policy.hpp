// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/data/enhanced_mode.hpp"

namespace oa::app {

/// The current frame's eligibility for compact strategic marker draws.
struct MarkerFramePolicy {
    bool global_dots{};
    bool gpu_ready{};
    bool debug_queries{};
    bool directed{};
    bool retained{};
    bool headless{};
    bool explicit_check{}; ///< only the dedicated windowless live-renderer check
};

/// Selects compact markers for the opted-in preset and ordinary global Dots.
/// The explicit native check may exercise the same route headlessly with
/// baseline rules; it cannot bypass geometry, resource or query exclusions.
/// @param rules the match's resolved rules, read without changing the match
/// @param frame the current render path and already opened GPU resources
/// @return whether this freshly built frame may list compact marker packets
[[nodiscard]] inline bool compact_marker_frame(
    const oa::data::match_rules::MatchRules& rules, const MarkerFramePolicy& frame
) noexcept {
    if (!frame.global_dots || !frame.gpu_ready || frame.debug_queries || frame.directed ||
        frame.retained)
        return false;
    const auto& enhanced = rules.setup.enhanced_mode;
    return frame.explicit_check || (!frame.headless && enhanced.enabled &&
                                    oa::data::enhanced_mode::uses_distributed_battle_flow(
                                        oa::data::enhanced_mode::alias_of(enhanced)
                                    ));
}

} // namespace oa::app
