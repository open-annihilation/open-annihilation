// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The unit tools (units.cpp): units.list, units.get, units.orders and
// units.types. They read a MatchView the frame hook publishes, and they
// drop every unit the local player is not shown unless the call asks for
// full awareness and the match allows it.
#pragma once

#include "oa/app/match_view.hpp"
#include "oa/app/mcp/session.hpp"

namespace oa::app::mcp {

/// The match view a unit tool reads. The frame hook sets it before it pumps.
struct UnitHost {
    MatchView table{}; ///< the view of this frame; its entries are null outside one
};

/// Adds units.list, units.get, units.orders and units.types.
///
/// @param[in,out] session the session
/// @param[in,out] host the view the tools read; it outlives the session's calls
void add_unit_tools(Session& session, UnitHost& host);

} // namespace oa::app::mcp
