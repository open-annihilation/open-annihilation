// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map tools (map.cpp): map.info, map.visibility and map.radar. They read
// a MatchView the frame hook publishes. Another player's sight, explored
// ground and contacts are left out unless the call asks for full awareness
// and the match allows it.
#pragma once

#include "units.hpp"

namespace oa::app::mcp {

/// Adds map.info, map.visibility and map.radar.
///
/// @param[in,out] session the session
/// @param[in,out] host the view the tools read; it outlives the session's calls
void add_map_tools(Session& session, UnitHost& host);

} // namespace oa::app::mcp
