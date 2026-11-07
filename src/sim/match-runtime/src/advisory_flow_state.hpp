// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>

namespace oa::sim::match_runtime {

struct AdvisoryFlowData;

/// Returns the complete fixed save size for the configured advisory storage.
///
/// @param state construction-preallocated catalogs and unit-slot count
/// @return bytes required, or zero when a binding or total size is unsupported
[[nodiscard]] size_t advisory_flow_saved_size(const AdvisoryFlowData& state) noexcept;

} // namespace oa::sim::match_runtime
