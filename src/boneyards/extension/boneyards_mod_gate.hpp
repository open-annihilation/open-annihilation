// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether entering the Boneyards asks before a mod is switched off, and
// whether leaving asks to switch that mod back on. The decisions live in
// expansion_entry.hpp, which the running game and the tests both call.
#pragma once

#include "oa/app/expansion_entry.hpp"

#include <string_view>

namespace oa::app {

/// The Boneyards' own profile. It is not another mod.
///
/// @param id the profile id; empty when there is no profile
/// @return true when the id is the Boneyards
[[nodiscard]] inline bool boneyards_own_profile(std::string_view id) noexcept {
    return id == "boneyards";
}

} // namespace oa::app
