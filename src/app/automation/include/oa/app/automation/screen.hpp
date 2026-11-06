// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The name both servers give a screen. One table, so a --fark answer and an
// MCP tool name the same screen the same way.
#pragma once

#include "oa/app/check_host.hpp"

#include <string>

namespace oa::app::automation {

/// Returns the name a screen is given.
///
/// @param shown the screen's registry id
/// @return main_menu, single_player, skirmish, mp_battleroom and the like;
///         screen_<id> for a screen without a name
[[nodiscard]] std::string screen_name(ScreenId shown);

} // namespace oa::app::automation
