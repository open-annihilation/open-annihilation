// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend/single_player_menu.hpp"
#include "test_support.hpp"

namespace oa::ui::frontend::test {
namespace {

// The full game's SINGLE.GUI, with the offers of one base game applied.
// A missing layout counts as a failed check and returns null.
const Control* prepared(const char* name, SinglePlayerOffers offers) {
    const auto layout = load_gui("single.gui");
    if (!layout)
        return nullptr;
    static Panel panel;
    panel_load_layout(panel, *layout);
    apply_single_player_offers(panel, offers);
    return panel_control(panel, name);
}

// Whether SINGLE.GUI itself shows the control, before any offers.
bool laid_out_active(const char* name) {
    const auto layout = load_gui("single.gui");
    if (!layout)
        return false;
    static Panel panel;
    panel_load_layout(panel, *layout);
    const Control* control = panel_control(panel, name);
    return control != nullptr && control->active != 0;
}

} // namespace

// The full game offers every entry of Single Player.
OA_GAME_DATA_TEST(single_player_menu_offers_skirmish_for_the_full_game) {
    SinglePlayerOffers full;
    const Control* skirmish = prepared("Skirmish", full);
    if (skirmish == nullptr)
        return;
    OA_CHECK(skirmish->grayed == 0);
    OA_CHECK(skirmish->active != 0);
    OA_CHECK(prepared("LoadGame", full)->grayed == 0);
    // Any Mission stays as SINGLE.GUI has it: the full game's offers do not
    // change it.
    OA_CHECK((prepared("AnyMsn", full)->active != 0) == laid_out_active("AnyMsn"));
    OA_CHECK(prepared("NewCamp", full)->grayed == 0);
    OA_CHECK(prepared("Options", full)->grayed == 0);
    OA_CHECK(prepared("PrevMenu", full)->grayed == 0);
}

// The demo offers New Campaign, Options and Previous Menu. Skirmish and
// Load Game are greyed, and Any Mission is hidden.
OA_GAME_DATA_TEST(single_player_menu_hides_what_the_demo_cannot_play) {
    const SinglePlayerOffers demo{.saved_games = false, .any_mission = false, .skirmish = false};
    const Control* skirmish = prepared("Skirmish", demo);
    if (skirmish == nullptr)
        return;
    OA_CHECK(skirmish->grayed == 1);
    OA_CHECK(skirmish->active != 0);
    OA_CHECK(skirmish->quick_key != 0);
    OA_CHECK(prepared("LoadGame", demo)->grayed == 1);
    OA_CHECK(prepared("AnyMsn", demo)->active == 0);
    OA_CHECK(prepared("NewCamp", demo)->grayed == 0 && prepared("NewCamp", demo)->active != 0);
    OA_CHECK(prepared("Options", demo)->grayed == 0);
    OA_CHECK(prepared("PrevMenu", demo)->grayed == 0);
}

// Neither side has a campaign: New Campaign stays on the menu, greyed, and
// Any Mission is hidden. The other entries stay as the full game leaves them.
OA_GAME_DATA_TEST(single_player_menu_greys_new_campaign_without_campaigns) {
    const SinglePlayerOffers none{.campaigns = false};
    const Control* campaign = prepared("NewCamp", none);
    if (campaign == nullptr)
        return;
    OA_CHECK(campaign->grayed == 1);
    OA_CHECK(campaign->active != 0);
    OA_CHECK(campaign->quick_key != 0);
    OA_CHECK(prepared("AnyMsn", none)->active == 0);
    OA_CHECK(prepared("LoadGame", none)->grayed == 0);
    OA_CHECK(prepared("LoadGame", none)->active != 0);
    OA_CHECK(prepared("Skirmish", none)->grayed == 0);
    OA_CHECK(prepared("Skirmish", none)->active != 0);
    OA_CHECK(prepared("Options", none)->grayed == 0);
    OA_CHECK(prepared("Options", none)->active != 0);
    OA_CHECK(prepared("PrevMenu", none)->grayed == 0);
    OA_CHECK(prepared("PrevMenu", none)->active != 0);
}

} // namespace oa::ui::frontend::test
