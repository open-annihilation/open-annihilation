// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace oa::app {

/// Counts the unit model draws a completed frame actually emitted.
///
/// Records culled before emission and non-unit scenery supply no model
/// count. Scissored geometry still counts. Vertices count logical triangle
/// corners, including those stored in card packets.
struct BodyLodCounts {
    uint32_t full_mesh_models{}; ///< unit draws using the original model geometry
    uint32_t body_lod_models{};  ///< unit draws using animated reduced geometry
    uint32_t distance_markers{}; ///< unit draws selected as dots by distance
    uint32_t budget_markers{};   ///< unit draws reduced to dots by a geometry limit
    uint64_t full_mesh_vertices{};
    uint64_t body_lod_vertices{};
    uint64_t shadow_vertices{}; ///< triangle corners emitted for unit geometry shadows
};

} // namespace oa::app
