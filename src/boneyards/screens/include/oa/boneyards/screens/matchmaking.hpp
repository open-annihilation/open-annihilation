// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Matchmaking choices and the queue behind the Matchmaking dialog.
// The dialog sends the look, join, cancel and answer records. Status,
// found and go come back through the service and change this record.
// Search lines, the scanner and the minimized blink stay local.
#pragma once

#include "oa/boneyards/service.hpp"
#include "oa/boneyards/toolkit.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards {
struct MessageBox;
}

namespace oa::boneyards::screens {

// Game the queue is asked to host. The wire value is GameMode, not this index.
enum class MatchGame : int32_t { duel = 0, winner_takes_all = 1, for_fun = 2 };

// Skill band. Near mine stands in for a seasoned commander.
enum class MatchSkill : int32_t { near_mine = 0, any = 1 };

// Side the commander plays.
enum class MatchSide : int32_t { arm = 0, core = 1, either = 2 };

// Where the battle may be hosted.
enum class MatchPlanets : int32_t { this_planet = 0, any_planet = 1 };

/// Pulses with no status after a join before the queue is called unavailable. Thirty pulses a second.
inline constexpr int32_t k_matchmaking_status_wait = 150;

/// Pulses between looks while the dialog is visible and the player is not queued.
inline constexpr int32_t k_matchmaking_look_period = 450;

/// Last frame of the queue meter. A status meter above this is drawn as this frame.
inline constexpr int32_t k_matchmaking_meter_last = 10;

// The dialog's choices and the search. The battle panel's face is a different
// record; these choices belong to the dialog.
struct MatchmakingChoices {
    MatchGame game{MatchGame::duel};
    MatchSkill skill{MatchSkill::near_mine};
    MatchSide side{MatchSide::core};
    MatchPlanets planets{MatchPlanets::this_planet};
    bool queued{false};
    int32_t elapsed_pulses{0}; // pulses spent in the queue
    int32_t requests_sent{0};  // look, join, cancel and answer records actually sent
    bool offer_open{false};    // the opponent box is on screen
    int32_t queue_count{0};    // commanders a status reported; 0 until one arrives
    int32_t meter{0};          // Queue_Meter frame a status reported
    bool awaiting_status{false};
    int32_t status_wait{0}; // pulses since the join, until the first status
    int32_t look_wait{0};   // pulses since the last look while the dialog is up
    char offer_id[kMatchmakingIdCapacity]{};
    char offer_name[kMatchmakingNameCapacity]{};
    int32_t offer_skill{0};
    int32_t offer_game_type{0};
    int32_t offer_role{0};
    int32_t offer_side{0};
    uint32_t offer_area{0};
    char game_id[kMatchmakingIdCapacity]{};
    int32_t assigned_side{0}; // side from found or go, kept for the battleroom
    bool side_known{false};
};

/// What applying one inbound record asks the dialog to change its face to.
enum class MatchmakingFaceChange : int32_t {
    none = 0,
    settings = 1,
    searching = 2,
};

/// What the dialog shows after one status, found or go record.
struct MatchmakingEffect {
    MatchmakingFaceChange face{MatchmakingFaceChange::none};
    bool close_offer{false};
    bool show_offer{false};
    bool show_box{false};
    int32_t box_result{0};
    bool launch_host{false};
    bool launch_join{false};
};

/// Restores the choices a new dialog shows: Duel, Near mine, Core, This planet, and no search.
///
/// @param[out] choices choices to reset; null is ignored
void matchmaking_setup(MatchmakingChoices* choices);

/// Marks the search queued. A search already running keeps its elapsed time.
///
/// The dialog sends matchmaking_join itself. A second join does not start the clock over.
///
/// @param[in,out] choices the dialog's choices; null is ignored
void matchmaking_queue_activate(MatchmakingChoices* choices);

/// Leaves the queue, clears the elapsed time and closes the offer in the record.
///
/// @param[in,out] choices the dialog's choices; null is ignored
void matchmaking_queue_cancel(MatchmakingChoices* choices);

/// Counts one more pulse of a search that is still queued.
///
/// @param[in,out] choices the dialog's choices; null, or a search that is not queued, is ignored
void matchmaking_queue_pulse(MatchmakingChoices* choices);

/// Fills a look or join from the radios and the selected planet.
///
/// `area_known` is stored as given. Any planet still omits the area: the dialog passes false.
///
/// @param choices the dialog's choices; null is ignored
/// @param area_id selected planet, used only when `area_known` is true
/// @param area_known whether `area_id` is the selected planet
/// @param[out] out query to fill; null is ignored
void matchmaking_query(
    const MatchmakingChoices* choices, uint32_t area_id, bool area_known, MatchmakingQuery* out
);

/// Returns the scanner frame for the time spent queued: eight frames, stepping about ten times a
/// second, or the resting frame when no search is queued.
///
/// @param choices the dialog's choices; null reads as the resting frame
/// @return frame 0..7
int32_t matchmaking_scanner_frame(const MatchmakingChoices* choices);

/// Returns how many commanders the last status reported.
///
/// @param choices the dialog's choices; null reads as 0
/// @return the status count, or 0 before a status
int32_t matchmaking_queue_count(const MatchmakingChoices* choices);

/// Returns the queue meter's frame. The frame is the status meter, clamped at the meter's last
/// frame, or 0 before a status.
///
/// @param choices the dialog's choices; null reads as 0
/// @return frame 0..10
int32_t matchmaking_meter_frame(const MatchmakingChoices* choices);

/// Writes "Commanders queued on …" for the planet line.
///
/// Any planet reads "any planet". Otherwise `planet` is the selected planet's name, or "Fuel Dump"
/// when that name is missing.
///
/// @param choices the dialog's choices; null writes nothing
/// @param planet selected planet name; null or empty stands in as Fuel Dump
/// @param[out] out buffer; null is ignored
/// @param capacity size of `out` in bytes, terminator included
void matchmaking_queue_line(
    const MatchmakingChoices* choices, const char* planet, char* out, std::size_t capacity
);

/// Writes the searching face's "Looking for a …" line from the choices.
///
/// @param choices the dialog's choices; null writes nothing
/// @param[out] out buffer; null is ignored
/// @param capacity size of `out` in bytes, terminator included
void matchmaking_detail_line(const MatchmakingChoices* choices, char* out, std::size_t capacity);

/// Writes "SEARCHING  m:ss" from the pulses spent in the queue.
///
/// @param choices the dialog's choices; null writes nothing
/// @param[out] out buffer; null is ignored
/// @param capacity size of `out` in bytes, terminator included
void matchmaking_search_line(const MatchmakingChoices* choices, char* out, std::size_t capacity);

/// Writes the opponent box from the offer last stored on the choices.
///
/// The three lines keep the mock's shape: "Opponent found:", the name and skill, then
/// "Join the Duel?" (or whichever type the offer named). The host is asked the same question.
///
/// @param choices the dialog's choices; null writes nothing
/// @param[out] out buffer; null is ignored
/// @param capacity size of `out` in bytes, terminator included
void matchmaking_offer_line(const MatchmakingChoices* choices, char* out, std::size_t capacity);

/// Returns the English of a status result box, or null when that result has no box.
///
/// Result 0 is acceptance. Result 3 is unused. The words are the catalogue keys.
///
/// @param result shared result code
/// @return the English sentence, or null
const char* matchmaking_result_text(int32_t result);

/// Returns the English shown when a join hears nothing for five seconds.
///
/// @return "The queue is unavailable."
const char* matchmaking_unavailable_text();

/// Applies a status: the count, the meter, and whether the search ends.
///
/// A non-zero result with nobody queued returns to settings and names that result, except result
/// 5, which stays on the search and still names the result. A queued value of 0 while the player
/// was searching returns to settings. The first status clears the five-second wait.
///
/// @param[in,out] choices the dialog's choices; null is ignored
/// @param status the status; null is ignored
/// @return what the dialog should show
MatchmakingEffect
matchmaking_apply_status(MatchmakingChoices* choices, const MatchmakingStatusNotice* status);

/// Stores an opponent and asks the dialog to open the searching face with the offer on top.
///
/// @param[in,out] choices the dialog's choices; null is ignored
/// @param found the opponent; null is ignored
/// @return what the dialog should show
MatchmakingEffect
matchmaking_apply_found(MatchmakingChoices* choices, const MatchmakingFoundNotice* found);

/// Closes the offer. Result 0 asks for host_game or join_game. Result 1 stays on the search.
///
/// @param[in,out] choices the dialog's choices; null is ignored
/// @param go the go record; null is ignored
/// @return what the dialog should show
MatchmakingEffect matchmaking_apply_go(MatchmakingChoices* choices, const MatchmakingGoNotice* go);

/// Maps an offer's seat count onto the host dialog's player radio.
///
/// Two seats are radio 0, three are radio 1 and four are radio 2. host_game already turns those
/// radios into the player count it sends. Any other count uses radio 0.
///
/// @param seats seats named on the offer
/// @return the radio index stored on HostGameRequest::players
int32_t matchmaking_players_radio(int32_t seats);

/// Fills a host request from a go record, using the existing radio mapping.
///
/// The request's name points at `go`'s name. Password is null. The side is not a host field.
///
/// @param go go record with result 0 and role 1; null fails
/// @param[out] out request to fill; null fails
/// @return false when `go` or `out` is null
bool matchmaking_host_request(const MatchmakingGoNotice* go, HostGameRequest* out);

/// Builds the opponent-found Yes/No box from the choices. The buttons themselves send nothing;
/// the dialog answers when they are pressed.
///
/// @param choices the dialog's choices; null shows an empty offer
/// @param focus_return window given the focus when a button closes the box; may be null
/// @return the box
MessageBox* matchmaking_offer_create(const MatchmakingChoices* choices, Window* focus_return);

} // namespace oa::boneyards::screens
