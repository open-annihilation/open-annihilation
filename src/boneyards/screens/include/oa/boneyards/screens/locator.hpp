// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Locator: find a player, a planet, a battle or a frequency, or move to
// another server. The searches use the requests the client already sends.
// A player search is one locate. A planet search reads the war map already
// loaded. A battle search is one game locate, which the game manager answers
// for every planet. A frequency search reads each planet's room list in
// turn. Server transfer logs out and then logs in to the chosen server with
// the same name and password.
#pragma once

#include "oa/boneyards/screens/login.hpp"
#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

// Search shown in the left column, top to bottom.
namespace locator_kind {
inline constexpr int32_t player = 0;
inline constexpr int32_t planet = 1;
inline constexpr int32_t battle = 2;
inline constexpr int32_t frequency = 3;
inline constexpr int32_t server = 4;
inline constexpr int32_t count = 5;
} // namespace locator_kind

inline constexpr int32_t kLocatorResultCapacity = 64;
inline constexpr int32_t kLocatorPlanetCapacity = 128;
inline constexpr std::size_t kLocatorNameCapacity = 64;
inline constexpr std::size_t kLocatorWhereCapacity = 80;
inline constexpr std::size_t kLocatorStatusCapacity = 32;
inline constexpr std::size_t kLocatorDetailCapacity = 180;
inline constexpr std::size_t kLocatorQueryCapacity = 64;
inline constexpr std::size_t kLocatorGameCapacity = 40;
// The chat entry line. A page prefills "+page Name " into it.
inline constexpr std::size_t kLocatorPageCapacity = 0x100;
// Pulses of the console (30 a second) a search waits for a planet's room
// list, or for the battle search's answers, once nothing more arrives.
inline constexpr int32_t kLocatorAnswerWaitPulses = 90;
// The WHERE of a frequency every planet lists.
inline constexpr char kLocatorEveryPlanet[] = "every planet";

// One planet the locator can search. Copied from the war map at search time.
struct LocatorPlanet {
    uint32_t id{};
    char name[16]{};
    char side{}; // 'A', 'C', or anything else for an independent world
    char ta_map[64]{};
    int32_t map_source{}; // MapSource
    int32_t in_progress{};
    int32_t forming{};
};

// One row of the results list.
struct LocatorResult {
    char name[kLocatorNameCapacity]{};
    char where[kLocatorWhereCapacity]{};
    char status[kLocatorStatusCapacity]{};
    char detail[kLocatorDetailCapacity]{};
    char game_id[kLocatorGameCapacity]{};
    char server[kLoginServerCapacity]{}; // location text a login names
    uint32_t area_id{};
    uint32_t room_id{};
    int32_t battle_type{};
    bool password{};
    bool current_server{};
    bool bright{};       // status drawn in the bright font
    bool every_planet{}; // a frequency every planet lists under the same id and name
};

// The locator's search. Hooks may be null. `changed` runs after the rows,
// the selection or the detail text change; a host refreshes its list then.
struct Locator {
    BoneyardsService* service{};
    void* user{};
    void (*changed)(void* user){};
    // Opens chat. page_line is empty when this is a move, and "+page Name "
    // when PAGE was used. area_id 0 leaves the current planet alone.
    void (*open_chat)(void* user, uint32_t area_id, uint32_t room_id, const char* page_line){};
    // Selects the planet on the war map. The census was already requested.
    void (*open_planet)(void* user, uint32_t area_id){};
    // Opens the battle console on the planet.
    void (*open_battle)(void* user, uint32_t area_id){};
    // Opens the battle console on the planet. A battle that needs a password
    // opens Battle Intelligence so the password can be entered there; the
    // join itself waits for that pane.
    void (*open_join)(void* user, uint32_t area_id, const char* game_id, bool needs_password){};

    int32_t kind{};
    char query[kLocatorQueryCapacity]{};
    LocatorPlanet planets[kLocatorPlanetCapacity]{};
    int32_t planet_count{};
    LocatorResult results[kLocatorResultCapacity]{};
    int32_t result_count{};
    int32_t selected{};
    char page_line[kLocatorPageCapacity]{};
    char session_name[kLoginFieldCapacity]{};
    char session_password[kLoginFieldCapacity]{};
    uint32_t here_area{};
    uint32_t here_room{};
    bool listening_rooms{};
    bool listening_battles{}; // the battle search awaits its answers
    bool awaiting_intel{};
    bool transfer_pending{};
    char transfer_server[kLoginServerCapacity]{};
    int32_t walk{};
    int32_t quiet_pumps{}; // pulses since the last answer while a search waits
    uint32_t asked_area{};
    char asked_planet[16]{};
};

/// Clears a locator and points it at a service.
///
/// The first search is Locate Player. A null service answers nothing.
///
/// @param[out] locator locator to reset; null is ignored
/// @param service service the searches call; may be null
void locator_init(Locator* locator, BoneyardsService* service);

/// Stores the war-map planets a planet, battle or frequency search reads.
///
/// Planets past the locator's capacity are dropped. The copy is kept, so
/// the caller's array need not outlive the call.
///
/// @param[in,out] locator locator; null is ignored
/// @param planets planets in walk order; null clears the list
/// @param count planets in `planets`
void locator_set_planets(Locator* locator, const LocatorPlanet* planets, int32_t count);

/// Stores the name and password a server transfer logs in with.
///
/// @param[in,out] locator locator; null is ignored
/// @param user_name session name; null stores empty
/// @param password session password; null stores empty
void locator_set_identity(Locator* locator, const char* user_name, const char* password);

/// Stores the planet and frequency the player is in, for a later move.
///
/// @param[in,out] locator locator; null is ignored
/// @param area_id current planet
/// @param room_id current frequency, 0 when none
void locator_set_here(Locator* locator, uint32_t area_id, uint32_t room_id);

/// Shows one search and clears the rows of the previous one.
///
/// Server Transfer fills its list at once from the service. Any other kind
/// waits for SEARCH.
///
/// @param[in,out] locator locator; null is ignored
/// @param kind locator_kind value; an unknown kind becomes Locate Player
void locator_set_kind(Locator* locator, int32_t kind);

/// Stores the text SEARCH will use.
///
/// @param[in,out] locator locator; null is ignored
/// @param query text; null stores empty
void locator_set_query(Locator* locator, const char* query);

/// Runs the search that is showing.
///
/// Locate Player sends a locate for the query. Locate Planet matches the
/// war map and sends nothing. Find Battle sends one game locate for the
/// query. Find Frequency asks each planet for its rooms. Server Transfer
/// lists the servers again. An empty query sends no locate: the server
/// answers none.
///
/// @param[in,out] locator locator; null is ignored
void locator_search(Locator* locator);

/// Selects a result row.
///
/// The first row of a finished battle search asks for that battle's intel,
/// and selecting another battle asks again.
///
/// @param[in,out] locator locator; null is ignored
/// @param index row index; an index outside the rows is ignored
void locator_select(Locator* locator, int32_t index);

/// True when RELOCATE would do something for the selected row.
///
/// Server Transfer is false for the server the player is on. A player row
/// needs a planet and a frequency. The other searches need a selected row.
///
/// @param locator locator; null is false
/// @return whether RELOCATE is available
bool locator_relocate_enabled(const Locator* locator);

/// True when PAGE or JOIN would do something for the selected row.
///
/// @param locator locator; null is false
/// @return whether the second button is available
bool locator_extra_enabled(const Locator* locator);

/// Moves to the selected row.
///
/// A player or a frequency changes to that room and opens chat; a frequency
/// every planet lists is entered from the planet the player is on. A planet
/// selects it and asks for its census. A battle opens the battle console
/// there. A server logs out and, once that finishes, logs in to it.
///
/// @param[in,out] locator locator; null is ignored
void locator_relocate(Locator* locator);

/// Runs PAGE or JOIN for the selected row.
///
/// PAGE opens chat with "+page Name " in the entry line and does not send
/// the page itself. JOIN on an open battle sends a join and opens the
/// battle console. JOIN on a battle that needs a password opens Battle
/// Intelligence for it and does not join yet.
///
/// @param[in,out] locator locator; null is ignored
void locator_extra(Locator* locator);

/// Counts a pulse of a search waiting for answers.
///
/// A frequency search waits for each planet's last room row. After
/// kLocatorAnswerWaitPulses pulses with no row, that planet is passed over
/// and the next one is asked. A battle search waits as long for its last
/// answer, then shows what arrived. The host calls this once per pulse.
///
/// @param[in,out] locator locator; null is ignored
void locator_pump(Locator* locator);

/// Takes the reply to the locate SEARCH sent.
///
/// @param[in,out] locator locator; null is ignored
/// @param report the reply; null is ignored
void locator_on_located(Locator* locator, const LocateReport* report);

/// Takes one frequency row while a frequency search is walking planets.
///
/// A row that is not from the search is ignored. The last row of a planet
/// starts the next planet. A row whose server sent global 1 is the
/// sector-wide frequency: WHERE is kLocatorEveryPlanet on the first
/// sighting, and a later sighting of the same id and name keeps that one
/// row. A global other than 1 is that planet's own frequency and is not
/// folded. When the server sends no global, a frequency already listed
/// under another planet with the same id and name is one that every planet
/// lists: it keeps its one row, whose WHERE becomes kLocatorEveryPlanet.
/// global_known and global belong to this row only. Nothing is kept for
/// the next call.
///
/// @param[in,out] locator locator; null is ignored
/// @param change 0 adds the row; any other change does not
/// @param area_id planet the row belongs to
/// @param room_id frequency id
/// @param name frequency name; null is skipped
/// @param needs_password whether the frequency asks for a password
/// @param users commanders in the frequency
/// @param type frequency type
/// @param last true for the planet's last row
/// @param global_known true when this row carried global
/// @param global the field's value; 1 is the sector-wide frequency
void locator_on_room(
    Locator* locator,
    int32_t change,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    bool needs_password,
    int32_t users,
    int32_t type,
    bool last,
    bool global_known = false,
    int32_t global = 0
);

/// Takes one answer to the battle search.
///
/// An answer with a game id adds a row, its WHERE the war map's name for
/// the planet. A forming battle whose answer carried both seat counts shows
/// "forming n/m"; without them it shows "forming". A battle in progress
/// shows "in progress". The answer marked last ends the search and selects
/// the first row, which asks for its intel. An answer that arrives when no
/// battle search waits is ignored.
///
/// @param[in,out] locator locator; null is ignored
/// @param report the answer; null is ignored
void locator_on_game_located(Locator* locator, const GameLocateReport* report);

/// Fills the selected battle's detail from its intel.
///
/// @param[in,out] locator locator; null is ignored
/// @param game_id battle the intel describes; null is ignored
/// @param host_name the host's name; null reads as unknown
/// @param skill 0 novice, 1 seasoned, 2 expert
/// @param failed true leaves the detail as it was
void locator_on_intel(
    Locator* locator, const char* game_id, const char* host_name, int32_t skill, bool failed
);

/// Continues a server transfer once the logout has finished.
///
/// The following login is sent once. A later reset does not log in again.
///
/// @param[in,out] locator locator; null is ignored
void locator_on_login_reset(Locator* locator);

/// Returns the search that is showing.
///
/// @param locator locator; null reads as Locate Player
/// @return a locator_kind value
int32_t locator_kind_of(const Locator* locator);

/// Returns the label of one search in the left column.
///
/// @param kind locator_kind value
/// @return the label; an unknown kind reads as Locate Player
const char* locator_kind_name(int32_t kind);

/// Returns the caption of the name field for a search.
///
/// @param kind locator_kind value
/// @return the caption, or null for Server Transfer, which has no field
const char* locator_field_label(int32_t kind);

/// True when the search has a name field and a SEARCH button.
///
/// @param kind locator_kind value
/// @return false for Server Transfer
bool locator_shows_field(int32_t kind);

/// True when the second button is PAGE.
///
/// @param kind locator_kind value
/// @return true for Locate Player
bool locator_shows_page(int32_t kind);

/// True when the second button is JOIN.
///
/// @param kind locator_kind value
/// @return true for Find Battle
bool locator_shows_join(int32_t kind);

/// Returns how many result rows are showing.
///
/// @param locator locator; null reads as none
/// @return the row count
int32_t locator_result_count(const Locator* locator);

/// Returns one result row.
///
/// @param locator locator; null has no rows
/// @param index row index
/// @return the row, or null when the index is outside the rows
const LocatorResult* locator_result(const Locator* locator, int32_t index);

/// Returns the selected row index.
///
/// @param locator locator; null reads as no selection
/// @return the index, or -1 when nothing is selected
int32_t locator_selected(const Locator* locator);

/// Returns the detail text under the list for the selected row.
///
/// @param locator locator; null reads as empty
/// @return the text, "" when nothing is selected
const char* locator_detail(const Locator* locator);

/// True while a frequency search wants room rows.
///
/// @param locator locator; null is false
/// @return whether room rows should be delivered here
bool locator_listening_rooms(const Locator* locator);

/// True while the battle search awaits its answers.
///
/// @param locator locator; null is false
/// @return whether the battle search is still answering
bool locator_listening_battles(const Locator* locator);

/// True while the selected battle's intel should be delivered here.
///
/// @param locator locator; null is false
/// @return whether intel should be delivered here
bool locator_awaiting_intel(const Locator* locator);

} // namespace oa::boneyards::screens
