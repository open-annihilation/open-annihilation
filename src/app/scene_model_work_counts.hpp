// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace oa::app {

/// Counts independent model ranges prepared for a completed scene build.
///
/// Slots are input draw records in the selected packet intervals, excluding
/// serial gaps and including records already omitted before scene emission.
/// Accepted bands and slots describe worker packets retained in the final
/// ordered model pass, not the number of threads that ran or models emitted.
struct SceneModelWorkCounts {
    uint32_t candidate_bands{}; ///< independent bands prepared before worker validation
    uint32_t candidate_slots{}; ///< input records assigned to those candidate bands
    uint32_t accepted_bands{};  ///< candidate bands retained by the complete model pass
    uint32_t accepted_slots{};  ///< input records covered by the retained bands
    uint32_t fallback_frames{}; ///< zero, or one when candidate packets required a serial retry
};

} // namespace oa::app
