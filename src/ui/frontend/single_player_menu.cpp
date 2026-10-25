// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// SINGLE.GUI entries the loaded game can open.
#include "oa/ui/frontend/single_player_menu.hpp"

namespace oa::ui::frontend {

void apply_single_player_offers(Panel& panel, SinglePlayerOffers offers) noexcept {
    if (!offers.saved_games)
        panel_set_grayed(panel, "LoadGame", true);
    if (!offers.any_mission)
        panel_set_active(panel, "AnyMsn", 0);
    if (!offers.skirmish)
        panel_set_grayed(panel, "Skirmish", true);
    if (!offers.campaigns) {
        panel_set_grayed(panel, "NewCamp", true);
        panel_set_active(panel, "AnyMsn", 0);
    }
}

} // namespace oa::ui::frontend
