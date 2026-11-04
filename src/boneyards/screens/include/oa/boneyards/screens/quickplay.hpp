// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// QuickPlay choices and the search. FIND BATTLE asks GameManager. Status sets
// the meter and Line0, scan lines fill the well, and a found battle opens
// the offer. The console sends the records and launches the battle.
#pragma once

#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

inline constexpr int32_t kQuickPlayTypeCount = 5;
inline constexpr int32_t kQuickPlayBlurbLines = 3;
inline constexpr int32_t kQuickPlayScanCapacity = 5;
inline constexpr int32_t kQuickPlayScannerFrames = 8;
inline constexpr int32_t kQuickPlayScannerMs = 110;
/// Queue_Meter frame painted until the first status, and the frame the layout bakes.
inline constexpr int32_t kQuickPlayMeterFrame = 6;
/// Last frame of the queue meter. A status meter above this is drawn as this frame.
inline constexpr int32_t kQuickPlayMeterLast = 10;
/// Milliseconds between revealed scan lines. Line0 is local and shows at once.
inline constexpr int32_t kQuickPlayLineIntervalMs = 1400;
/// Result lines under Line0. A further revealed line scrolls the oldest off Line1.
inline constexpr int32_t kQuickPlayResultLines = 4;
/// Milliseconds with no status after FIND BATTLE before the search is given up.
inline constexpr int32_t kQuickPlayStatusWaitMs = 5000;
/// Scan lines kept for one search. Already-scrolled lines are dropped once this fills.
inline constexpr int32_t kQuickPlayScanStore = 64;
inline constexpr int32_t kQuickPlayFavoriteCapacity = 48;
inline constexpr int32_t kQuickPlayTextCapacity = 96;

// Game types in the order the console lists them. Duel is the default.
inline constexpr int32_t kQuickPlayDuel = 0;
inline constexpr int32_t kQuickPlayWinnerTakesAll = 1;
inline constexpr int32_t kQuickPlayForFun = 2;
inline constexpr int32_t kQuickPlayGalacticWar = 3;
inline constexpr int32_t kQuickPlayWarTraining = 4;

enum class QuickPlayWhere : int32_t {
    any_planet = 0,
    fuel_dump = 1,
};

enum class QuickPlaySkill : int32_t {
    novice = 0,
    seasoned = 1,
    expert = 2,
};

enum class QuickPlaySide : int32_t {
    arm = 0,
    core = 1,
    either = 2,
};

/// One planet the search considered. The sentence is written from these fields.
struct QuickPlayScan {
    int32_t seq{};
    int32_t kind{};
    int32_t reason{};
    int32_t game_type{};
    int32_t skill{};
    int32_t seats{};
    int32_t seats_max{};
    char planet[kQuickPlayPlanetBytes]{};
    char battle[kQuickPlayBattleBytes]{};
};

// One commander's QuickPlay card and the search it is running, if any.
struct QuickPlay {
    int32_t selected; // kQuickPlayDuel .. kQuickPlayWarTraining
    bool allowed[kQuickPlayTypeCount];
    QuickPlayWhere where;
    QuickPlaySkill skill;
    QuickPlaySide side;
    bool prefer_favorite;
    char favorite[kQuickPlayFavoriteCapacity];
    bool searching;
    int32_t elapsed_ms;
    int32_t requests_sent{}; // join, cancel and answer records the console actually sent
    bool offer_open{};       // the battle-found offer is on screen
    bool awaiting_status{};  // FIND BATTLE is waiting for the first status
    bool launch_pending{};   // Yes ended the search; a status says the battle was refused
    bool has_status{};       // a status has set the meter for this search
    int32_t meter{};         // Queue_Meter frame, or kQuickPlayMeterFrame before a status
    int32_t planets{};       // planets a status reported; 0 until one arrives
    int32_t fits{};
    int32_t last_seq{}; // highest scan seq kept; a seq at or below this is ignored
    int32_t scan_count{};
    int32_t scan_dropped{}; // lines scrolled out of the store
    QuickPlayScan scans[kQuickPlayScanStore]{};
    char declined_game_id[kMatchmakingIdCapacity]{};
    char offer_id[kMatchmakingIdCapacity]{};
    char offer_name[kQuickPlayBattleBytes]{};
    char offer_planet[kQuickPlayPlanetBytes]{};
    char offer_game_id[kMatchmakingIdCapacity]{};
    bool offer_game_known{};
    int32_t offer_role{};
    int32_t offer_game_type{};
    int32_t offer_skill{};
    int32_t offer_seats{};
    int32_t offer_on_death{};
    uint32_t offer_area{};
    char offer_map[kQuickPlayMapBytes]{};
    bool offer_map_known{};
};

/// What applying one inbound record, or answering the offer, asks the console to do.
struct QuickPlayEffect {
    bool leave_search{false};
    bool close_offer{false};
    bool show_offer{false};
    bool show_box{false};
    int32_t box_result{0};
    bool launch_host{false};
    bool launch_join{false};
};

/// The words baked into the offer before a battle is found. A found battle replaces them.
inline constexpr const char* kQuickPlayOfferLine0 = "Rumble in the Rubble";
inline constexpr const char* kQuickPlayOfferLine1 = "Duel on Lava Alley";
inline constexpr const char* kQuickPlayOfferLine2 = "Join now?";

/// Sets the card a commander sees before choosing: Duel, any planet, Seasoned, either side, and
/// the favorite map "Lava Alley", with Galactic War and War Training unavailable.
///
/// @param[out] play card to reset; null does nothing
void quickplay_init(QuickPlay* play);

/// Applies the selected planet and the commander's allegiance to the game types.
///
/// Duel, Winner Takes All and For Fun are always available. Galactic War is
/// available on a contested planet for a commander who has an allegiance.
/// War Training is available on a planet of the commander's own side. A
/// selection that is no longer available becomes Duel.
///
/// @param[in,out] play card to update; null does nothing
/// @param has_planet whether a planet is selected
/// @param planet_side the planet's side, 'A', 'C' or 'I'
/// @param allegiance the commander's allegiance, 'A', 'C' or 'U'
void quickplay_set_place(QuickPlay* play, bool has_planet, char planet_side, char allegiance);

/// Remembers the favorite map named on the profile. An empty name stays "Lava Alley".
///
/// @param[in,out] play card to update; null does nothing
/// @param map favorite map; null or empty keeps the default
void quickplay_set_favorite(QuickPlay* play, const char* map);

/// Tells whether a listed game type can be chosen.
///
/// @param play card; null allows nothing
/// @param index game type, kQuickPlayDuel .. kQuickPlayWarTraining
/// @return true when a click on that row selects it
bool quickplay_type_allowed(const QuickPlay* play, int32_t index);

/// Selects a game type that is available.
///
/// @param[in,out] play card; null does nothing
/// @param index game type
/// @return false when the index is out of range or the type is unavailable
bool quickplay_select_type(QuickPlay* play, int32_t index);

/// Returns the name shown for a game type.
///
/// @param index game type
/// @return the name, or "" when the index is out of range
const char* quickplay_type_name(int32_t index);

/// Returns the name shown for a skill.
///
/// @param skill skill
/// @return "Novice", "Seasoned" or "Expert"
const char* quickplay_skill_name(QuickPlaySkill skill);

/// Returns one line of the note under the game-type list.
///
/// @param index selected game type
/// @param line line 0 .. 2
/// @return the line, or "" when either index is out of range
const char* quickplay_blurb(int32_t index, int32_t line);

/// Writes the favorite-map check caption, "Prefer my favorite: <map>".
///
/// @param play card; null uses Lava Alley
/// @param[out] out caption; null does nothing
/// @param capacity size of out, terminator included
void quickplay_favorite_caption(const QuickPlay* play, char* out, std::size_t capacity);

/// Starts the search on the card. A search already running is left as it is.
///
/// The console sends quickplay_join. The clock, the scans and a declined battle are cleared.
/// The meter stays on its resting frame until a status arrives.
///
/// @param[in,out] play card; null does nothing
void quickplay_find(QuickPlay* play);

/// Stops the search, closes the offer and returns the card to its settings.
///
/// The console sends quickplay_cancel when the player presses CANCEL SEARCH, and when the
/// console is destroyed during a search. Giving up after five seconds with no status uses this
/// and sends nothing.
///
/// @param[in,out] play card; null does nothing
void quickplay_cancel(QuickPlay* play);

/// Moves a running search forward. The scanner steps every 110 ms across eight frames.
///
/// @param[in,out] play card; null, or one that is not searching, does nothing
/// @param ms milliseconds to add; zero or negative adds nothing
void quickplay_advance(QuickPlay* play, int32_t ms);

/// Returns the scanner frame for the time the search has been running.
///
/// Frame 0 is the idle sweep and the frame at the moment FIND BATTLE is
/// pressed. Eight frames repeat every 110 ms.
///
/// @param play card; null reads as frame 0
/// @return frame 0 .. 7
int32_t quickplay_scanner_frame(const QuickPlay* play);

/// Returns how many lines the search is showing.
///
/// Line0 appears as the search starts. Each scan line appears 1400 ms after the one before it,
/// in the order they were kept. At most four scan lines are on screen; a fifth scrolls the
/// oldest off Line1.
///
/// @param play card; null shows nothing
/// @return the line count, Line0 included
int32_t quickplay_visible_lines(const QuickPlay* play);

/// Writes one search line. Line 0 is the local sentence. Later lines are composed from the scans.
///
/// A line not yet revealed, or out of range, is empty. Planet and battle names are copied as
/// sent. The sentence itself is never taken from the server.
///
/// @param play card; null writes nothing
/// @param index line index, 0 for Line0
/// @param[out] out text; null does nothing
/// @param capacity size of out, terminator included
void quickplay_scan_line(const QuickPlay* play, int32_t index, char* out, std::size_t capacity);

/// Writes the search timer as minutes and seconds, "0:07" after seven seconds.
///
/// @param play card; null writes "0:00"
/// @param[out] out text; null does nothing
/// @param capacity size of out, terminator included
void quickplay_timer(const QuickPlay* play, char* out, std::size_t capacity);

/// Returns the queue meter's frame. Before a status this is the resting frame. Afterwards it is
/// the status frame, clamped at the meter's last frame. A negative meter is drawn as frame 0.
///
/// @param play card; null reads as the resting frame
/// @return frame 0..10
int32_t quickplay_meter_frame(const QuickPlay* play);

/// Fills a join from the card and the selected planet.
///
/// game_type is the wire value: For Fun 0, Duel 1, Winner Takes All 2, Galactic War 4, War
/// Training 7. area_id is stored as given. favorite is copied only while the check is on.
///
/// @param play card; null leaves `out` empty
/// @param area_id selected planet, used only when `area_known` is true
/// @param area_known whether `area_id` is the selected planet
/// @param[out] out query to fill; null is ignored
void quickplay_query(const QuickPlay* play, uint32_t area_id, bool area_known, QuickPlayQuery* out);

/// Returns the English of a status result box, or null when that result has no box.
///
/// Result 0 is acceptance. Result 3 is unused. Result 1 names QuickPlay. The other sentences are
/// the ones Matchmaking already shows. The words are the catalogue keys.
///
/// @param result shared result code
/// @return the English sentence, or null
const char* quickplay_result_text(int32_t result);

/// Returns the English shown when FIND BATTLE hears nothing for five seconds.
///
/// @return "The search could not be started."
const char* quickplay_unavailable_text();

/// True when a search has been waiting for its first status for five seconds.
///
/// @param play card; null is not overdue
/// @return true when the console should give up without sending cancel
bool quickplay_status_overdue(const QuickPlay* play);

/// Applies a status: the meter, Line0's planet count, and whether the search ends.
///
/// A non-zero result with nobody queued returns to the settings and names that result, except
/// result 5, which stays on the search, closes the offer and still names the result. The first
/// status ends the five-second wait. A status after Yes ended the search puts the card back on
/// the search first. A status while the card is idle and not waiting is ignored.
///
/// @param[in,out] play card; null is ignored
/// @param status the status; null is ignored
/// @return what the console should show
QuickPlayEffect quickplay_apply_status(QuickPlay* play, const QuickPlayStatusNotice* status);

/// Keeps one scan in seq order, also after Yes ended the search. A seq at or below the last one
/// kept is ignored.
///
/// The line is not shown until its turn, one every 1400 ms from the start of the search.
///
/// @param[in,out] play card; null, or one that is neither searching nor after Yes, is ignored
/// @param scan the scan; null is ignored
void quickplay_apply_scan(QuickPlay* play, const QuickPlayScanNotice* scan);

/// Stores an offer and asks the console to show it over the search.
///
/// A join offer whose battle was already refused is ignored. A host offer has no battle id.
///
/// @param[in,out] play card; null, or one that is not searching, is ignored
/// @param found the offer; null is ignored
/// @return what the console should show
QuickPlayEffect quickplay_apply_found(QuickPlay* play, const QuickPlayFoundNotice* found);

/// Closes the offer. Yes on a join asks for join_game. Yes on a host asks for host_game.
///
/// Yes that asks for either ends the search, as the battle it launches does on the server: back
/// from the battle, the card shows FIND BATTLE. The offer stays for the launch to read. When
/// the battle is refused, the server goes on searching, and its next status puts the card back
/// on the search. No stays on the search. No on a join remembers that battle so it is not
/// offered again. The search clock is not restarted.
///
/// @param[in,out] play card; null, or one with no offer open, is ignored
/// @param accept 1 for Yes, 0 for No
/// @return what the console should send and launch
QuickPlayEffect quickplay_decide(QuickPlay* play, int32_t accept);

/// Writes the offer's three lines: the battle name, the type on the planet, then the question.
///
/// A host offer asks "No games available, host one?". A join offer asks "Join now?".
///
/// @param play card; null writes nothing
/// @param[out] out buffer; null is ignored
/// @param capacity size of `out` in bytes, terminator included
void quickplay_offer_line(const QuickPlay* play, char* out, std::size_t capacity);

/// Maps an offer's seat count onto the host dialog's player radio.
///
/// Two seats are radio 0, three are radio 1 and four are radio 2. host_game already turns those
/// radios into the player count it sends. Any other count uses radio 0, which hosts two.
///
/// @param seats seats named on the offer
/// @return the radio index stored on HostGameRequest::players
int32_t quickplay_players_radio(int32_t seats);

/// Fills a host request from the offer stored on the card.
///
/// The request's name points at the card's offer name. Password is null. The mission is not a
/// host field. A card that is not holding a host offer fails.
///
/// @param play card whose offer is a host offer; null fails
/// @param[out] out request to fill; null fails
/// @return false when `play` or `out` is null, or the offer is not a host offer
bool quickplay_host_request(const QuickPlay* play, HostGameRequest* out);

} // namespace oa::boneyards::screens
