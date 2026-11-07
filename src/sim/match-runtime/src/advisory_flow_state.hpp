// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>

namespace oa::sim::match_runtime {

struct AdvisoryFlowData;

/// Returns the complete fixed save size for the configured advisory storage.
///
/// Partially consumed work and rotating service cursors occupy the fixed
/// header. The guide membership registry is derived and adds no saved bytes.
///
/// @param state construction-preallocated catalogs and unit-slot count
/// @return bytes required, or zero when a binding or total size is unsupported
[[nodiscard]] size_t advisory_flow_saved_size(const AdvisoryFlowData& state) noexcept;

} // namespace oa::sim::match_runtime
