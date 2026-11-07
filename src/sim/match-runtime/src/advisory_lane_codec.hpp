// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "advisory_lane_guides.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::match_runtime {

/// Size of one versioned, pointer-free advisory lane record in bytes.
inline constexpr size_t advisory_lane_record_bytes = 176;

/// Writes all nonpointer guide fields into fixed-width little-endian records.
///
/// Input and output storage must be separate. Goal and order addresses are
/// ignored; the match keeps their stable saved identities separately. Invalid
/// input leaves all output bytes unchanged.
///
/// @param states caller-owned guide states, including unused unit slots
/// @param[out] bytes exactly one advisory_lane_record_bytes record per state
/// @return whether every state is valid and the complete output fits
[[nodiscard]] bool write_advisory_lane_states(
    std::span<const AdvisoryLaneState> states, std::span<uint8_t> bytes
) noexcept;

/// Validates every complete guide record without changing any live state.
///
/// The match checks the expected unit-slot count separately before restoring
/// its aggregate state. A partial record or unsupported version is rejected.
///
/// @param bytes complete versioned records, with no trailing bytes
/// @return whether every record is structurally and geometrically valid
[[nodiscard]] bool can_read_advisory_lane_states(std::span<const uint8_t> bytes) noexcept;

/// Restores complete guide records after validating the whole table.
///
/// Input and output storage must be separate. Restored goal and order pointers
/// are null; the match must rebind their saved identities before using advice.
/// Corridor geometry is checked against the detector's global bounds; the match also
/// checks its own map, class and player binding. Failure changes no state.
///
/// @param[out] states caller-owned guide storage, unchanged on malformed input
/// @param bytes exactly one versioned advisory_lane_record_bytes record per state
/// @return whether every record is valid and the complete table fits
[[nodiscard]] bool read_advisory_lane_states(
    std::span<AdvisoryLaneState> states, std::span<const uint8_t> bytes
) noexcept;

} // namespace oa::sim::match_runtime
