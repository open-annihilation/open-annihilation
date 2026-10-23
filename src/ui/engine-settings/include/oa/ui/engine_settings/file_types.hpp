// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// System's seven file-type switches as the file types they choose: the set a registration acts
// on and whose rest a withdrawal acts on (Windows, Linux), and the default-opener changes OK
// asks for (macOS). Each switch stands for its own type alone.

#pragma once

#include "oa/platform/file_types.hpp"
#include "oa/ui/engine_settings.hpp"

#include <vector>

namespace oa::ui::engine_settings {

/// Returns the file types whose switch is on in `settings`: the set a registration acts on.
/// Its complement is the set a withdrawal acts on.
///
/// @param settings the settings that hold the switches
/// @return one type for each switch that is on
[[nodiscard]] oa::platform::file_types::TypeSet
file_type_set(const EngineSettings& settings) noexcept;

/// Sets each file-type switch of `settings` from `types`: on where its type is in the set. A
/// host shows what macOS reports this way.
///
/// @param[in,out] settings the settings whose switches are set
/// @param types the types this copy opens
void set_file_type_switches(
    EngineSettings& settings, oa::platform::file_types::TypeSet types
) noexcept;

/// Returns the switches that moved from `opened` to `chosen`, in the order System lists them,
/// each as the default-opener change it asks for: this copy for a switch turned on, the opener
/// of the type it hands back to for one turned off.
///
/// @param opened the settings the dialog opened with
/// @param chosen the settings OK is keeping
/// @return one change for each switch that moved; empty when none did
[[nodiscard]] std::vector<oa::platform::file_types::OpenerChange>
moved_file_type_switches(const EngineSettings& opened, const EngineSettings& chosen);

} // namespace oa::ui::engine_settings
