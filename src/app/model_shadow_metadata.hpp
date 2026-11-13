// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "world_draws.hpp"

#include <cstdint>

namespace oa::app {

/// Selects completed ground units whose geometry can supply an ordinary shadow.
///
/// This only bounds the image policy. The caller must also prove that the
/// current GPU model type is supported before deferring the image's pixels.
///
/// @param draw the unit's ordinary or interpolated model draw
/// @param sea_level the match's sea level, in world pixels
/// @return true for an unlinked ordinary ground unit at or above the sea
[[nodiscard]] inline bool
geometry_shadow_metadata_candidate(const ModelDraw& draw, int32_t sea_level) noexcept {
    const auto& model = draw.model;
    const auto* unit = model.unit;
    if (draw.on_map || draw.stand_in >= 0 || unit == nullptr || model.def == nullptr ||
        model.state == nullptr || model.instance == nullptr || model.prepared == nullptr ||
        unit->id == 0 || unit->type_index == 0 || unit->build_remaining != 0.0F ||
        unit->attach_parent != 0 || unit->attach_first_child != 0 ||
        (unit->flags & OA_UNIT_FLAG_BUILDING) != 0)
        return false;
    constexpr uint32_t non_ground = OA_UNIT_DEF_FLAG_DIGGER | OA_UNIT_DEF_FLAG_CAN_FLY |
                                    OA_UNIT_DEF_FLAG_FLOATER | OA_UNIT_DEF_FLAG_CAN_HOVER;
    constexpr int64_t fixed_scale = 65536;
    const auto position = draw.shown_position.value_or(unit->position);
    return (model.def->flags & non_ground) == 0 &&
           int64_t(position.y) >= int64_t(sea_level) * fixed_scale;
}

} // namespace oa::app
