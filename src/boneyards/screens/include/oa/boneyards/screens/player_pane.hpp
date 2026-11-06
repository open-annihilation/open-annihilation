// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The player-profile pane opened from the Chat Frequencies player list, with
// its General, Ladder and Awards pages.
#pragma once

#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/profile.hpp"

#include <cstdint>

namespace oa::boneyards::screens {

// Controls of the pane that post messages.
enum class PaneControl : int32_t {
    page_button,
    whisper_icon,
    silence_icon,
    general_tab,
    ladder_tab,
    awards_tab,
};

/// Builds the player pane's controls with the console.
///
/// No player or record, the General page, the rank icon and insignia hidden,
/// PAGE enabled and every tab label in the normal colour.
///
/// @param[in,out] console chat console holding the pane
void chat_pane_build(ChatConsole* console);

/// Tells the pane's view that its fields changed.
///
/// @param console chat console holding the pane
/// @param page whether pane.html is a new page
void chat_pane_changed(ChatConsole* console, bool page);

/// Starts the Main Frame's activity light for the pane's profile request.
///
/// @param console chat console holding the pane
void chat_pane_start_activity_light(ChatConsole* console);

/// Stops the Main Frame's activity light once the pane's profile arrives.
///
/// @param console chat console holding the pane
void chat_pane_stop_activity_light(ChatConsole* console);

/// Adds a ladder standing of the pane's profile reply to the record the pane holds.
///
/// The standing goes to the page builder unchanged; nothing happens when no
/// record is held.
///
/// @param console the ChatConsole
/// @param ladder_id ladder of the standing
/// @param ladder_subid ladder subdivision of the standing
/// @param fields the standing's fields
void chat_pane_on_ladder(
    void* console, int32_t ladder_id, int32_t ladder_subid, const ProfileFields* fields
);

/// Takes the pane's profile reply.
///
/// Stops the activity light, reads the reply into the record the pane holds
/// (a failed reply leaves it as it is), then renders the pane.
///
/// @param console the ChatConsole
/// @param ok whether the request succeeded
/// @param fields the profile's fields
void chat_pane_on_profile(void* console, bool ok, const ProfileFields* fields);

/// Fills the pane from its record and posts its profile-shown message.
///
/// Sets the rank icon and title, side colour and insignia, clan, the current
/// page and its tab highlight, and the PAGE, whisper and muzzle controls for
/// other players. Without a record, or when `ok` is false, the pane shows
/// "No Data" and posts the unlock message instead.
///
/// @param[in,out] console chat console holding the pane
/// @param ok whether the profile request succeeded
void chat_pane_render(ChatConsole* console, bool ok);

/// Handles a click on a pane control while it holds a player's record.
///
/// PAGE posts the player's name for a page, the whisper and muzzle icons post
/// the player to the console, and a tab button moves the highlight to its
/// label and shows its page. The pane's close button is not modelled.
///
/// @param[in,out] console chat console holding the pane
/// @param control control clicked
/// @return false when the pane holds no profile or the click changes nothing
bool chat_pane_on_click(ChatConsole* console, PaneControl control);

/// Returns the whisper icon to its first frame, as when the pane's player joins the frequency
/// again.
///
/// @param[in,out] pane player pane
void chat_pane_whisper_icon_off(PlayerPane* pane);

/// Disables the whisper icon, as when the pane's player ends the whisper lock without a player list
/// row or leaves the frequency.
///
/// @param[in,out] pane player pane
void chat_pane_disable_whisper_icon(PlayerPane* pane);

/// Hides the pane, frees its profile record and lets go of its player.
///
/// The pane's page text becomes the empty page, so nothing points into the
/// freed record; the view keeps showing the page it was given until the pane
/// opens again.
///
/// @param[in,out] pane player pane
void chat_pane_close(PlayerPane* pane);

} // namespace oa::boneyards::screens
