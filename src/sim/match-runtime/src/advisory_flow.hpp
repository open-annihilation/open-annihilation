// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "advisory_lane_guides.hpp"
#include "oa/core/world.h"
#include "oa/core/move_class.h"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace oa::sim::match_runtime {

class Match;

inline constexpr size_t advisory_catalog_capacity = 64;
inline constexpr uint64_t advisory_storage_limit = 128ull * 1024 * 1024;
inline constexpr uint32_t advisory_construction_work = 2048;
inline constexpr uint32_t advisory_query_work = 8192;
inline constexpr uint32_t advisory_active_work = 32768;
inline constexpr uint32_t advisory_enrollment_slots = 2048;

/// One player's static corridor proofs for one native movement class.
struct AdvisoryCatalog {
    std::unique_ptr<ground_orders::CorridorCatalog> catalog{};
    Match* match{};
    uint32_t movement_class{}, player{};
    uint64_t revision{1};
    ground_orders::CorridorRectangle sampled{};
    bool sampled_valid{}, recording_samples{};
    std::array<uint32_t, 64> validated_tick{};
    std::array<bool, 64> validated{};
};

/// Stable rebinding information beside an owner-local helper state.
struct AdvisoryIdentity {
    uint64_t generation{1};
    uint32_t order_issue_tick{};
    uint16_t unit_type{};
    uint8_t owner{}, order_kind{};
    bool rebind{};
};

/// Construction-preallocated advisory storage outside the packed world.
struct AdvisoryFlowData {
    std::array<AdvisoryCatalog, advisory_catalog_capacity> catalogs{};
    std::array<std::array<int16_t, OA_MOVE_CLASS_COUNT>, OA_PLAYER_COUNT> catalog_indices{};
    uint32_t catalog_count{}, catalog_cursor{};
    size_t unit_count{};
    std::unique_ptr<AdvisoryLaneState[]> guides{};
    std::unique_ptr<AdvisoryIdentity[]> identities{};
    std::unique_ptr<sim::unit_movement::LocalFlowHead[]> published_guides{};
    std::unique_ptr<uint64_t[]> retained_units{};
    size_t retained_word_count{};
    std::unique_ptr<uint8_t[]> save_bytes{};
    size_t save_size{};
    uint64_t allocated_bytes{};
    ground_orders::CorridorBudget construction{}, query{}, active{};
    uint32_t tick{}, enrollment_begin{1}, enrollment_next{1};
    uint32_t active_cursor{1}, active_begin{1};
    uint32_t budget_override{std::numeric_limits<uint32_t>::max()};
    bool tick_valid{}, active_stopped{};
};

/// Rebuilds the derived retained-unit registry after a complete saved-state restore.
void rebuild_advisory_membership(AdvisoryFlowData& state) noexcept;

} // namespace oa::sim::match_runtime
