// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace oa::app {

/// Counts shadow index draws and uploads of the last successful backend frame.
///
/// Logical indices are triangle corners actually drawn, split between the
/// dynamic buffer and retained templates. Uploads count words transferred,
/// including any unused words supplied by the ordinary dynamic path.
/// Cache requests count compact pattern runs, not scene eligibility estimates.
struct ShadowIndexCounts {
    uint64_t logical_indices{};
    uint64_t dynamic_indices{};
    uint64_t cached_indices{};
    uint64_t dynamic_uploaded_indices{};
    uint64_t template_uploaded_indices{};
    uint64_t cache_bytes{}; ///< occupied index words in the retained template buffer
    uint32_t requested_runs{};
    uint32_t cached_runs{}; ///< actual draws from the retained template buffer
    uint32_t cache_hits{}; ///< requested runs whose template was resident before this frame
    uint32_t cache_misses{}; ///< remaining requests, including admissions and dynamic fallbacks
    uint32_t admitted_patterns{}; ///< distinct patterns uploaded during this frame
    uint32_t fallback_runs{}; ///< requested runs drawn from the ordinary dynamic buffer
    uint32_t cached_patterns{}; ///< descriptors resident after this frame
};

} // namespace oa::app
