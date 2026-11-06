// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace oa::app {

class Runtime;

/// Returns the Enhanced Mode rules version of the running game or next start.
///
/// @param runtime the running application
/// @return zero while Enhanced Mode is off
[[nodiscard]] uint32_t enhanced_mode_version(const Runtime& runtime) noexcept;

/// Holds a recorded rules version for one loaded match without changing preferences.
///
/// @param runtime the running application
/// @param version the recorded version; zero selects the released rules
/// @return false when the version or resolved rules are unsupported
[[nodiscard]] bool hold_enhanced_mode_once(Runtime& runtime, uint32_t version);

/// Restores the preference after a held match or failed load.
///
/// @param runtime the running application
void cancel_enhanced_mode_once(Runtime& runtime) noexcept;

} // namespace oa::app
