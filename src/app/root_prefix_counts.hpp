// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace oa::app {

/// Owned root-prefix instances built and successfully drawn by one live frame.
/// Surface indices count triangle corners referenced by submitted model runs.
struct RootPrefixActivationCounts {
    uint64_t scene_instances{};
    uint64_t gpu_instances{};
    uint64_t gpu_surface_indices{};
};

} // namespace oa::app
