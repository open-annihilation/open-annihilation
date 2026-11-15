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
/// Cache requests count original runs needing one or more patterns. A mixed
/// run's drawn corners are counted individually as dynamic or cached.
/// Procedural reuse requires at least sixteen actually cached casts in a run.
/// Lookup hits do not imply reuse; admissions count patterns, not missed runs.
struct ShadowIndexCounts {
    uint64_t logical_indices{};
    uint64_t dynamic_indices{};
    uint64_t cached_indices{};
    uint64_t dynamic_uploaded_indices{};
    uint64_t template_uploaded_indices{};
    uint64_t cache_bytes{}; ///< bytes occupied by index words in the retained template buffer
    uint32_t requested_runs{};
    uint32_t cached_runs{}; ///< requested runs with actual retained-corner reuse
    uint32_t cache_hits{};  ///< requested runs whose needed patterns were all resident before frame
    uint32_t cache_misses{}; ///< remaining requests, including admissions and dynamic fallbacks
    uint32_t admitted_patterns{}; ///< distinct patterns uploaded during this frame
    uint32_t fallback_runs{};     ///< requested runs with no retained-corner reuse
    uint32_t cached_patterns{};   ///< descriptors resident after this frame
    /// Four descriptor words per cast in an actual procedural run and four
    /// uniform words per nonempty procedural draw. Includes zero-corner
    /// descriptors; excludes corner words and unobservable driver allocation padding.
    uint64_t mapping_uploaded_words{};
};

} // namespace oa::app
