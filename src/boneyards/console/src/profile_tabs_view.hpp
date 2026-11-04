// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Profile console's category tabs as windows: each tab's captions,
// entries, check boxes, radio buttons and pages, built the first time the
// console's model creates the tab. The model (screens::ProfileConsole) keeps
// what the controls hold; these windows show it, and hand it what the player
// types and picks.
#pragma once

#include "console_state.hpp"
#include "html_view.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/profile.hpp"

namespace oa::boneyards::console {

// The windows of the seven tabs, made as each tab is first shown.
struct ProfileTabWindows {
    bool built[screens::kProfileTabCount];
    // The model's tab revision the windows last took.
    uint32_t shown_revision[screens::kProfileTabCount];
    // Account: the login name, status and email address with their
    // captions, the Password and Confirm entries and CHANGE.
    Window* login_caption;
    Window* login_name;
    Window* status_caption;
    Window* status;
    Window* email_caption;
    Window* email;
    Window* password_caption;
    SingleEdit* password;
    Window* confirm_caption;
    SingleEdit* confirm;
    Window* change;
    // Personal: the tag line, the two expansion checks, the forum alias, the
    // ICQ number and the email alias check.
    Window* tag_caption;
    SingleEdit* tag_line;
    CheckBox* core_contingency;
    CheckBox* battle_tactics;
    Window* forum_caption;
    SingleEdit* forum_alias;
    Window* icq_caption;
    SingleEdit* icq;
    CheckBox* email_alias;
    // Allegiance: the rank icon and name, the side's insignia, name and rank
    // title, or "Join Forces of:" with the Arm and Core radios and their
    // insignia, and the side page.
    Window* user_icon;
    Window* user_name;
    Window* insignia;
    Window* side;
    Window* rank;
    Window* join_caption;
    screens::LobbyRadio* arm;
    Window* arm_insignia;
    screens::LobbyRadio* core;
    Window* core_insignia;
    HtmlView* allegiance_page;
    int32_t user_icon_frame; // the User_Icons frame the icon shows, -1 none
    // Game preferences: the three radio groups and the favourite map.
    screens::ByRadioGroup* skill;
    screens::ByRadioGroup* players;
    screens::ByRadioGroup* commander_death;
    Window* map_caption;
    SingleEdit* favorite_map;
    // Ladder and awards pages.
    HtmlView* ladder_page;
    HtmlView* awards_page;
    // Local settings: the unit limit and slide steps with their captions,
    // and the background sound check.
    Window* unit_limit_caption;
    SingleEdit* unit_limit;
    Window* slide_caption;
    SingleEdit* slide_steps;
    CheckBox* sound_off;
};

/// Returns the tab windows of a Profile console.
///
/// @param console the Profile console
/// @return its tab windows
ProfileTabWindows* profile_console_tabs(Window* console);

/// Brings the tab windows in step with the model.
///
/// Builds the windows of each tab the model has created and they lack, shows
/// in them what the model holds for each tab it has refreshed since, then
/// hands the model what the player typed and picked, and shows or hides
/// every window as the model shows its tab and controls.
///
/// @param[in,out] windows the tab windows
/// @param[in,out] console the Profile console
/// @param[in,out] model the console's model
void profile_tabs_follow(
    ProfileTabWindows* windows, ProfileView* console, screens::ProfileConsole* model
);

/// Hands the model what the player typed and picked in the built tabs.
///
/// @param windows the tab windows
/// @param[in,out] model the console's model
void profile_tabs_read(const ProfileTabWindows* windows, screens::ProfileConsole* model);

/// Handles CHANGE: the model shows the Password and Confirm entries in its place, and the Password
/// entry takes the focus.
///
/// @param[in,out] windows the tab windows
/// @param[in,out] model the console's model
/// @param notify control notice
/// @return false when the notice is not from CHANGE
bool profile_tabs_on_notify(
    ProfileTabWindows* windows, screens::ProfileConsole* model, const Notify* notify
);

/// Selects the text of the entry the model's last rejected field names.
///
/// @param windows the tab windows
/// @param model the console's model
void profile_tabs_select_rejected(
    const ProfileTabWindows* windows, const screens::ProfileConsole* model
);

} // namespace oa::boneyards::console
