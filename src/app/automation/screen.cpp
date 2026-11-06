// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen names (include/oa/app/automation/screen.hpp).
#include "oa/app/automation/screen.hpp"

#include "oa/app/app.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <array>
#include <string>

namespace oa::app::automation {
namespace {

namespace mp = oa::ui::frontend_multiplayer;

/// A screen and the name it is given.
struct NamedScreen {
    ScreenId shown{};
    std::string_view called;
};

/// The screens that have a name: the built-in screens, and the multiplayer
/// screens. A screen that is not here is screen_ and its id.
constexpr std::array kScreenNames = {
    NamedScreen{screen_id(Screen::main_menu), "main_menu"},
    NamedScreen{screen_id(Screen::single_player), "single_player"},
    NamedScreen{screen_id(Screen::skirmish), "skirmish"},
    NamedScreen{screen_id(Screen::map_selection), "map_selection"},
    NamedScreen{screen_id(Screen::loading), "loading"},
    NamedScreen{screen_id(Screen::match), "match"},
    NamedScreen{screen_id(Screen::options), "options"},
    NamedScreen{screen_id(Screen::sound), "sound"},
    NamedScreen{screen_id(Screen::visuals), "visuals"},
    NamedScreen{screen_id(Screen::speeds), "speeds"},
    NamedScreen{screen_id(Screen::music), "music"},
    NamedScreen{screen_id(Screen::new_campaign), "new_campaign"},
    NamedScreen{screen_id(Screen::any_mission), "any_mission"},
    NamedScreen{screen_id(Screen::load_game), "load_game"},
    NamedScreen{screen_id(Screen::campaign_end), "campaign_end"},
    NamedScreen{screen_id(Screen::briefing), "briefing"},
    NamedScreen{mp::kScreenProviders, "mp_providers"},
    NamedScreen{mp::kScreenTcp, "mp_tcp"},
    NamedScreen{mp::kScreenGameList, "mp_game_list"},
    NamedScreen{mp::kScreenNewGame, "mp_new_game"},
    NamedScreen{mp::kScreenBattleroom, "mp_battleroom"},
};

} // namespace

std::string screen_name(ScreenId shown) {
    for (const NamedScreen& named : kScreenNames)
        if (named.shown == shown)
            return std::string(named.called);
    return "screen_" + std::to_string(shown);
}

} // namespace oa::app::automation
