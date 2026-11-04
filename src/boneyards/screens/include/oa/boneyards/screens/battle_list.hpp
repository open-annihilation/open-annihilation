// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Battles / Planet Intelligence console model: the entered planet's battles
// grouped by game mode, their records as the service reports them, and the
// Host, Join summary and Scoreboard panels the console opens over its list.
#pragma once

#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

// Records the list holds at once. 3.1c holds any number; this bound only
// stops a feed that never ends from taking all memory, and a battle past it
// is dropped, as one whose record cannot be allocated is.
inline constexpr std::size_t kMaxBattles = 0x4000;
inline constexpr std::size_t kBattlePlayerRows = 4;
inline constexpr int32_t kNoBattle = -1;
inline constexpr int32_t kNoBattleCategory = -1;

inline constexpr std::size_t kBattleGameIdCapacity = 0x25;
// A battle name is at most 31 characters. A host or player name is 15.
inline constexpr int32_t kBattleNameCharacters = 0x1f;
inline constexpr std::size_t kBattleNameCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(kBattleNameCharacters));
inline constexpr std::size_t kBattleUserNameCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(kPlayerNameCharacters));
inline constexpr std::size_t kBattleMissionCapacity = 0x40;
inline constexpr std::size_t kBattleReportNamesCapacity = 0xbd;
inline constexpr std::size_t kBattleNumberCapacity = 0x10;

// The host's side battle_info reports (BattleEntry.host_code).
enum class BattleHost : int32_t {
    arm = 0,
    core = 1,
    unknown = 2,
};

// One player's line of a battle's report; the numbers are kept as the
// decimal text the Scoreboard shows.
struct BattlePlayer {
    uint32_t user_id; // 0 while the row is free
    int32_t icon;     // User_Icons frame
    char name[kBattleUserNameCapacity];
    char kills[kBattleNumberCapacity];
    char losses[kBattleNumberCapacity];
    char metal[kBattleNumberCapacity];
    char energy[kBattleNumberCapacity];
    char score[kBattleNumberCapacity];
};

// A battle record.
struct BattleEntry {
    bool used;          // the slot holds a record
    int32_t category;   // the mode category listing it, kNoBattleCategory when none
    int32_t references; // its category and the panels showing it
    uint32_t area_id;   // the area it is listed for, 0 while unlisted
    char game_id[kBattleGameIdCapacity];
    char name[kBattleNameCapacity];
    int32_t type;          // GameMode id
    int32_t forming;       // 1 while forming, 0 once in progress
    int32_t host_code;     // BattleHost
    uint32_t host_user_id; // from the intel
    char host_name[kBattleUserNameCapacity];
    int32_t has_password;
    char mission[kBattleMissionCapacity];
    int32_t skill;
    int32_t players; // number-of-players selector
    int32_t on_death;
    int32_t report_count; // report lines taken since the intel
    // The reported players, each after its rank title, ", " between
    char report_names[kBattleReportNamesCapacity];
    BattlePlayer scores[kBattlePlayerRows];
};

struct BattleCategory {
    GameModeEntry mode; // name, mode id
    bool expanded;      // category row toggle
    int32_t* battles;   // the records it lists, in arrival order; grows as they arrive
    std::size_t battle_count;
    std::size_t battle_capacity; // entries `battles` has room for
};

struct BattleRow {
    bool is_category; // CategoryIcon row (list type 9) or BattleIcon row (type 6)
    std::size_t category;
    int32_t battle; // a battle row's record
};

// Controls of the battle list screen.
enum class BattleControl : int32_t {
    battle_list,
    host_button,
};

// The panels the console opens over its list, by the pane kind that shows
// each.
enum class BattlePanel : int32_t {
    host = 0,       // the Host panel (game setup)
    join = 1,       // the Join summary of a forming battle
    scoreboard = 2, // the Scoreboard of a battle in progress
};

// The console's output; every entry may be null.
struct BattleUi {
    void* user;
    // Starts (running) or stops the Main Frame's activity light.
    void (*activity)(void* user, bool running);
    // The Host panel opens: its game types follow the selected planet and
    // the player's allegiance.
    void (*open_host)(void* user);
    // A line for the Planet Intelligence list.
    void (*intel)(void* user, const char* text);
    // A report message about the battle the Scoreboard shows.
    void (*report_message)(void* user, const char* text);
    // A battle's feed opened or closed a panel.
    void (*panels_changed)(void* user);
};

struct BattleList {
    const BoneyardsService* service;
    BattleUi ui;
    uint32_t area_id; // the area whose battles were entered
    // The records by index, free slots reused; grows as battles arrive.
    BattleEntry* battles;
    std::size_t battle_slots; // slots `battles` holds
    BattleCategory categories[kGameModeCount];
    std::size_t category_count;
    bool rows_dirty;
    BattleRow* rows; // the rows the last rebuild listed
    std::size_t row_count;
    std::size_t row_capacity;  // rows `rows` has room for
    bool host_visible;         // pane kind 0, the Host panel
    bool join_visible;         // pane kind 1, the Join summary of a forming battle
    bool scoreboard_visible;   // pane kind 2, the Scoreboard of a battle in progress
    int32_t join_battle;       // the Join summary's record, kNoBattle when none
    int32_t scoreboard_battle; // the Scoreboard's record
};

/// Constructs the battle console's list: rows dirty, one category per game mode, no records or
/// panels.
///
/// Frees what an earlier construction of the list held first; the list must
/// start zeroed the first time.
///
/// @param[in,out] list list to build
/// @param service service the requests go to
/// @param ui the console's output hooks
void battle_list_init(BattleList* list, const BoneyardsService* service, BattleUi ui);

/// Frees the records, the category lists and the rows the list holds, leaving it empty.
///
/// @param[in,out] list battle list
void battle_list_release(BattleList* list);

/// Starts a game-mode category: expanded, with no battles yet.
///
/// @param[out] category category to start
/// @param mode game mode it lists
void battle_category_init(BattleCategory* category, const GameModeEntry& mode);

/// Starts the activity light and enters an area's battles (the model's half of the console's open).
///
/// A request the service refuses gets no answer to stop the light, so the
/// refusal stops it.
///
/// @param[in,out] list battle list
/// @param area_id the session's current area
void battle_list_open(BattleList* list, uint32_t area_id);

/// Leaves an area's battles (the model's half of the console's close).
///
/// Closes the shown panel, empties the categories, marks the rows for a
/// rebuild, starts the light and leaves the battles. A request the service
/// refuses gets no answer to stop the light, so the refusal stops it.
///
/// @param[in,out] list battle list
/// @param area_id the session's current area
void battle_list_close(BattleList* list, uint32_t area_id);

/// Finds the first record whose game id matches over 0x24 characters.
///
/// @param list battle list
/// @param game_id battle id; null finds nothing
/// @return the record's index, or kNoBattle when there is none
int32_t battle_list_find(const BattleList* list, const char* game_id);

/// Frees the first record of a game id unless its category or a panel still holds it.
///
/// @param[in,out] list battle list
/// @param game_id battle id
void battle_list_remove(BattleList* list, const char* game_id);

/// Returns the record at an index.
///
/// The record may move when a new one is created, so the pointer is good
/// until then; its index stays.
///
/// @param list battle list
/// @param index record index
/// @return the record, or null for kNoBattle or a free slot
const BattleEntry* battle_list_entry(const BattleList* list, int32_t index);

/// Empties every game-mode category of its battles, as the console does when it closes and leaves
/// the planet's battles.
///
/// @param[in,out] list battle list
void battle_list_reset_categories(BattleList* list);

/// Rebuilds the category rows, and the battle rows under expanded categories, when the rows are
/// marked dirty.
///
/// @param[in,out] list battle list
void battle_list_rebuild_rows(BattleList* list);

/// Closes each shown panel: the Host panel, then the Join summary and the Scoreboard, which let go
/// of their records.
///
/// @param[in,out] list battle list
void battle_list_close_panels(BattleList* list);

/// Asks the service for a battle's intel and report with the activity light running until they
/// arrive.
///
/// @param[in,out] list battle list
/// @param battle record index; a free slot asks nothing
void battle_list_request_intel(BattleList* list, int32_t battle);

/// Shows a battle on the Join summary or the Scoreboard.
///
/// The panel lets go of the record it held (freeing it unless something
/// else holds it), holds the new one and counts as shown.
///
/// @param[in,out] list battle list
/// @param panel BattlePanel::join or BattlePanel::scoreboard
/// @param battle record index
void battle_list_hold(BattleList* list, BattlePanel panel, int32_t battle);

/// Closes one panel; the Join summary and the Scoreboard let go of their records.
///
/// @param[in,out] list battle list
/// @param panel the panel
void battle_list_close_panel(BattleList* list, BattlePanel panel);

/// Tells whether a panel counts as shown.
///
/// @param list battle list
/// @param panel the panel
/// @return true while it is shown
bool battle_list_panel_shown(const BattleList* list, BattlePanel panel);

/// Handles HOST and list selections.
///
/// HOST toggles the Host panel; a category row toggles its battles; a battle
/// row asks for the battle's intel and shows it on the Scoreboard while in
/// progress or on the Join summary while forming.
///
/// @param[in,out] list battle list
/// @param control control clicked
/// @param row index into the current rows, for a list selection
void battle_list_on_control(BattleList* list, BattleControl control, std::size_t row);

/// Stops the activity light once entering or leaving the battles is answered.
///
/// @param[in,out] list battle list
void battle_list_on_done(BattleList* list);

/// Applies a battle added, refreshed or removed in the list feed.
///
/// A battle added or refreshed gets its record (filed under its mode, or
/// again when it was listed for another area) and its name, forming flag and
/// host side; one removed leaves its category. A battle the Join summary
/// shows closes the panel when removed and moves to the Scoreboard once in
/// progress. A removed battle's record is freed unless a panel holds it; one
/// still shown on a panel asks for its intel again. The console hears of a
/// panel opened or closed.
///
/// @param[in,out] list battle list
/// @param area_id the session's current area
/// @param change 0 added or refreshed, 1 removed
/// @param game_id battle id
/// @param name battle name
/// @param type game mode id
/// @param forming 1 while forming, 0 once in progress
/// @param host_code the host's side (BattleHost)
void battle_list_on_battle_info(
    BattleList* list,
    uint32_t area_id,
    int32_t change,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t host_code
);

/// Hands an intelligence line to the Planet Intelligence list.
///
/// @param[in,out] list battle list
/// @param text the line
/// @quirk The line goes to the list whether or not it names a battle; the game id is dropped.
void battle_list_on_intel(BattleList* list, const char* text);

/// Stops the light and, unless the request failed, fills the battle's record from its intel.
///
/// @param[in,out] list battle list
/// @param intel the battle's intel
void battle_list_on_battle_intel(BattleList* list, const BattleIntel* intel);

/// Applies one line of a battle's report.
///
/// A line carrying a message goes to the Scoreboard instead, when it shows
/// that battle; any other line fills the battle's record.
///
/// @param[in,out] list battle list
/// @param report the report line
void battle_list_on_battle_report(BattleList* list, const BattleReport* report);

/// Takes a battle's host, password flag, mission and settings, and starts its report names over.
///
/// @param[in,out] battle battle record
/// @param intel the battle's intel
void battle_entry_set_intel(BattleEntry* battle, const BattleIntel* intel);

/// Fills the player's report row, or the first free one, from a report line.
///
/// Sets the name, rank icon and numbers; the player's rank title and name
/// join the report names after the others.
///
/// @param[in,out] battle battle record
/// @param report the report line
/// @quirk A fifth player finds no row and is dropped.
void battle_entry_add_report(BattleEntry* battle, const BattleReport* report);

} // namespace oa::boneyards::screens
