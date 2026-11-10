// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "presentation_interpolation.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/sim/match_runtime/script.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <vector>

namespace oa::app {

/// The presentation fields needed by a categorical global Dots frame or models.
enum class UnitPoseCapture { full, root };

/// Selects root-only capture for the explicitly enabled version-7 preset's Dots.
/// Other presets and rendered model frames keep complete rig history.
/// @param rules the match's resolved rules
/// @param global_dots the existing graphics policy draws every unit as a dot
/// @return the fields this observation captures
[[nodiscard]] UnitPoseCapture
unit_pose_capture(const oa::data::match_rules::MatchRules& rules, bool global_dots) noexcept;

/// One validated unit's canonical inputs and exclusive presentation history.
/// Pointers are non-null except script, and each motion occurs at most once.
/// Inputs remain unchanged until capture_unit_poses returns.
struct UnitCaptureItem {
    UnitMotion* motion{};
    const oa::Unit* unit{};
    const oa::sim::model_runtime::Instance* instance{};
    const oa::sim::match_runtime::ScriptInstance* script{};
    uint32_t generation{};
};

/// The actual work completed by a unit-pose capture. Units count new root
/// endpoints; a same-tick rig prime adds pieces and triplets without another unit.
struct UnitCaptureTotals {
    uint64_t units{}, pieces{}, triplets{};
};

/// One band's reused rate words and completed work; no two bands share it.
struct UnitCaptureBand {
    std::vector<int32_t> turns;
    UnitCaptureTotals totals{};
    std::exception_ptr failure;
    std::size_t failed_item{};
};

/// Storage kept between captures, bounded by actual pieces and pool threads.
struct UnitCaptureScratch {
    std::vector<int32_t> serial_turns;
    std::vector<UnitCaptureBand> bands;
};

/// Captures independent histories, joining the drawing workers before returning.
///
/// Missing pools, small batches, new generations and histories without enough
/// capacity use the original serial order. Parallel bands use preallocated
/// storage and the caller's floating-point control. Scratch allocation failure
/// falls back to serial capture; an unexpected worker failure is rethrown on
/// the caller after all bands finish, choosing the first item in input order.
///
/// @param items canonical inputs validated by the caller, in slot order
/// @param tick the tick captured, unchanged for the entire call
/// @param[in,out] scratch reusable rate storage
/// @param pool drawing workers; null captures on the caller alone
/// @param fields root-only capture never reads script rates or piece poses
/// @return counts of units, model pieces and logical VM triplet reads captured
[[nodiscard]] UnitCaptureTotals capture_unit_poses(
    std::span<const UnitCaptureItem> items,
    uint32_t tick,
    UnitCaptureScratch& scratch,
    oa::platform::job_pool::Pool* pool = nullptr,
    UnitPoseCapture fields = UnitPoseCapture::full
);

} // namespace oa::app
