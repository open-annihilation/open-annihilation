// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The multiplayer game reporter: the player, score-board and settings tables
// a network game assembles from the canonical Player records, and the events
// that refresh and upload them. The uploads themselves (the upload hooks and
// the external reporting library) are platform boundaries reached through
// Hooks.
#pragma once

#include "oa/core/world.h"

#include <cstdint>

namespace oa::data::campaign {
struct CampaignFile;
}

namespace oa::boneyards::reporter {

struct Reporter;

inline constexpr uint32_t score_line_count = 9;
inline constexpr uint32_t setting_text_bytes = 0x40;
inline constexpr uint32_t mission_bytes = 0x80;
inline constexpr uint32_t session_name_bytes = 0x11;
inline constexpr int32_t setting_unset = -1;
// Frames between two score uploads while a game runs.
inline constexpr int32_t upload_interval = 0x3c;
inline constexpr int32_t upload_off = -1;

// Results of init_game_reporter and report_event.
namespace result {
inline constexpr int32_t ok = 0;
inline constexpr int32_t no_buffers = 1;
inline constexpr int32_t library_failed = 2;
inline constexpr int32_t unavailable = 4;
inline constexpr int32_t version_mismatch = 6;
inline constexpr int32_t refused = 7; // the simulation is not one the Boneyards reports
} // namespace result

// Event codes passed to report_event; names follow their callers.
namespace event {
inline constexpr int32_t battleroom_opened = 1;
inline constexpr int32_t roster_changed = 2; // ? a player took a slot
inline constexpr int32_t player_removed = 3;
inline constexpr int32_t player_changed = 4; // ? alliance or watcher toggled
inline constexpr int32_t map_changed = 5;
inline constexpr int32_t game_started = 6;
inline constexpr int32_t game_ended = 7;
inline constexpr int32_t session_closed = 8;
inline constexpr int32_t game_launched = 10; // the host pressed Start
inline constexpr int32_t interval = 99;
} // namespace event

// Destinations of report_event.
inline constexpr uint8_t route_uploads = 1;
inline constexpr uint8_t route_library = 2;
inline constexpr uint8_t route_all = route_uploads | route_library;

// Bits of ReportPlayer.flags.
namespace player_flag {
inline constexpr uint32_t playing = 1;
inline constexpr uint32_t computer_or_out = 2;
inline constexpr uint32_t watching = 4; // a watcher that never built
inline constexpr uint32_t host = 8;
} // namespace player_flag

// Indices of Reporter.settings, in the game's table order.
enum class Setting : uint32_t {
    connect,
    creator,
    registered_only,
    player_limit,
    time_limit,
    provider,
    session,
    mission,
    password,
    player,
    pfs,
    address,
    lock_options,
    max_units,
    energy,
    metal,
    death_option,
    los,
    cheats,
    location,
    map_mode,
    watching,
    tournament,
    count
};
inline constexpr uint32_t setting_count = static_cast<uint32_t>(Setting::count);

// One "PlayerInfo%d" record.
struct ReportPlayer {
    const char* name{}; // Player.name
    uint32_t net_id{};
    uint32_t flags{}; // player_flag bits
    const char* side{};
    int32_t ally_count{};
    const ReportPlayer* allies[OA_PLAYER_COUNT]{}; // "Allies%d"
    const char* team{};
    int32_t team_member_count{};
    const ReportPlayer* team_members[OA_PLAYER_COUNT]{}; // "TeamMembers%d"
};

// One labelled "Scores%d" value.
struct ScoreLine {
    const char* label{};
    int32_t value{};
};

// One "ScoreBoard%d": the score and its nine lines (the game reaches the
// lines through the "ppScores%d" pointer table).
struct ScoreBoard {
    int32_t score{};
    int32_t line_count{};
    ScoreLine lines[score_line_count]{};
};

// Indices of ScoreBoard.lines.
namespace score_line {
inline constexpr uint32_t kills = 0;
inline constexpr uint32_t losses = 1;
inline constexpr uint32_t energy_produced = 2;
inline constexpr uint32_t metal_produced = 3;
inline constexpr uint32_t excess_energy = 4;
inline constexpr uint32_t excess_metal = 5;
inline constexpr uint32_t commanders_killed = 6;
inline constexpr uint32_t commanders_lost = 7;
inline constexpr uint32_t winner = 8;
} // namespace score_line

// One 12-byte "SettingsArray" entry.
struct ReportSetting {
    const char* name{};
    char* text{};     // its "SettingsTextValArray" slot
    int32_t* value{}; // setting_unset until a numeric setting fills it
};

// Options of the online launch block the report reads.
struct OnlineOptions {
    int32_t player_limit{};
    int32_t time_limit{};
    int32_t pfs{};          // ? meaning unresolved
    const char* provider{}; // short text
    int32_t tournament{};
    const char* address{}; // copy of the block's connection address
};

// Arguments of the settings upload (Hooks::upload_settings).
struct SettingsUpload {
    int32_t lock_options{};
    const char* mission{};
    int32_t player_limit{};
    int32_t death_option{};
    int32_t energy{};
    int32_t metal{};
    int32_t location{};
    int32_t los{};
    int32_t map_mode{};
    int32_t max_units{};
    int32_t watching{};
    int32_t cheats{};
    int32_t tournament{};
};

// Arguments of the player upload (Hooks::upload_player).
struct PlayerUpload {
    uint32_t net_id{};
    const char* name{};
    const char* side{};
    int32_t team{}; // 1..5 for diamond, x, equals, triangle, slug; 0 none
};

// Arguments of the score upload (Hooks::upload_score).
struct ScoreUpload {
    uint32_t net_id{};
    int32_t lines[score_line::winner]{}; // kills .. commanders lost
    int32_t score{};
    bool winner{};
    bool final_report{};
};

// Arguments of the reporting library's ri_report entry point.
struct LibraryReport {
    int32_t event{};
    const uint8_t* session_guid{};  // 16 bytes
    const uint8_t* provider_guid{}; // Game.provider_guid
    uint32_t session_value{};       // Hooks::session_value
    const char* session_name{};
    const char* map_name{};
    uint8_t local_player{};
    int32_t player_count{};
    const Reporter* reporter{};
};

// The external reporting library (".\reporter", ri_* entry points). No
// install ships one; its step and message callbacks are not registered.
struct Library {
    void* context{};
    uint32_t (*get_version)(void* context, const char* product){};
    int32_t (*initialize)(void* context, int32_t* status){};
    int32_t (*report)(void* context, const LibraryReport& report){};
    int32_t (*report_chat)(void* context, const ReportPlayer& player, const char* line){};
    void (*interval_timer)(void* context){};
    void (*terminate)(void* context){};
};

struct Hooks {
    void* context{};
    // Options of the online launch, or null when the game was not started
    // through one.
    const OnlineOptions* (*online_options)(void* context){};
    // Whether the uploads' destination is up; the upload hooks are called
    // only while it answers true. Null answers false.
    bool (*uploads_online)(void* context){};
    void (*upload_settings)(void* context, const SettingsUpload& upload){};
    void (*upload_player)(void* context, const PlayerUpload& upload){};
    void (*upload_score)(void* context, const ScoreUpload& upload){};
    void (*upload_chat)(void* context, uint32_t net_id, const char* line){};
    // Loads the reporting library; false when there is none.
    bool (*open_library)(void* context, Library* library){};
    void (*close_library)(void* context){};
    // Closes the transport session, as a loaded library requires on shutdown.
    void (*close_session)(void* context){};
    // The transport session's GUID and value for the library.
    const uint8_t* (*session_guid)(void* context){};
    uint32_t (*session_value)(void* context){};
    // Whether this machine's simulation may be reported. Null answers yes.
    bool (*simulation_permitted)(void* context){};
};

// Reporter state, one for the whole game.
struct Reporter {
    // The session's game options record (Game.game_options), whose OTA
    // killmul and timemul weight the score.
    const oa::data::campaign::CampaignFile* options{};
    int32_t active{}; // reporting initialised for this session
    bool buffers{};   // the report tables exist
    bool library_loaded{};
    Library library{};
    uint32_t upload_sends{}; // all bits after an opening event; bits 0-1 scores, bit 3 chat
    int32_t upload_countdown{upload_off};
    int32_t setting_total{}; // setting_count while the tables exist
    ReportPlayer players[OA_PLAYER_COUNT]{};
    ScoreBoard boards[OA_PLAYER_COUNT]{};
    ReportSetting settings[setting_count]{};
    char setting_text[setting_count][setting_text_bytes]{};
    int32_t setting_values[setting_count]{};
    char mission[mission_bytes]{};
    char session_name[session_name_bytes]{};
};

/// Returns the transport session name the game keeps at the start of Game.session_description.
///
/// @param game Game holding the name.
/// @return The name; not necessarily terminated within session_name_bytes.
[[nodiscard]] const char* session_name(const Game& game) noexcept;

/// Stores the transport session name at the start of Game.session_description.
///
/// @param[out] game Game receiving the name.
/// @param name Name to copy, truncated to 16 characters; null stores an empty name.
void set_session_name(Game& game, const char* name) noexcept;

/// Creates the report tables: settings named and pointed at their text and value slots.
///
/// The game zeroes only the first 0x5C bytes of the settings table; every
/// entry is written right after, so the tables are simply cleared here.
///
/// @param[out] reporter Reporter whose tables are created; numeric settings start as setting_unset.
/// @return result::ok.
int32_t alloc_report_tables(Reporter& reporter) noexcept;

/// Releases the reporting library and the report tables.
///
/// A loaded library first has the transport session closed, as it requires on shutdown.
///
/// @param[in,out] reporter Reporter to shut down; left inactive.
/// @param hooks Platform hooks used to close the session and the library.
void shutdown_reporter(Reporter& reporter, const Hooks& hooks) noexcept;

/// Starts reporting for a session and tries the reporting library.
///
/// Serial, modem and IPX sessions report only when the online launch named
/// a provider. The library is looked for once reporting is active, so its
/// absence (result::unavailable) leaves the uploads running.
///
/// @param[in,out] reporter Reporter to start; shut down first.
/// @param game Game whose provider GUID picks the "connect" setting.
/// @param options The session's game options record, whose OTA killmul and timemul weight the score.
/// @param[out] status Set to 0 once reporting is active; the library's initialise status after that.
/// @param hooks Platform hooks.
/// @return result::ok; refused when the simulation is not one the Boneyards reports; unavailable when the
///         provider does not report, there is no library or it fails to initialise; library_failed when it
///         lacks an entry point; version_mismatch.
int32_t init_game_reporter(
    Reporter& reporter,
    Game& game,
    const oa::data::campaign::CampaignFile* options,
    int32_t* status,
    const Hooks& hooks
) noexcept;

/// Fills the player and score tables from the Player records and, when asked, the settings text.
///
/// Rows are indexed by player slot and the count only counts included slots,
/// so callers that walk the first count rows (report_event) assume the
/// included slots come first, as the game does. The game also admits active
/// and watching slots; both imply in_use.
///
/// @param[in,out] reporter Reporter whose tables are filled.
/// @param world World holding the players.
/// @param full Also refresh the settings text.
/// @param final_report The game has ended; the local winner is marked.
/// @param hooks Platform hooks supplying the online launch options.
/// @return The number of slots in use or that created units.
int32_t build_score_report(
    Reporter& reporter, const World& world, bool full, bool final_report, const Hooks& hooks
) noexcept;

/// Rebuilds the report for an event and uploads it through the upload hooks and to the library.
///
/// Events below 7, launch (10) and the interval (99) also refresh the
/// settings text; the end-of-game event (7) marks the local winner.
///
/// @param[in,out] reporter Reporter to update.
/// @param[in,out] world World holding the players; the game password is cleared at the end of a game.
/// @param event event:: code.
/// @param routes route_uploads, route_library or both.
/// @param hooks Platform hooks.
/// @return result::ok; refused when the simulation is not one the Boneyards reports; unavailable when
///         reporting is off; no_buffers without tables.
int32_t report_event(
    Reporter& reporter, World& world, int32_t event, uint8_t routes, const Hooks& hooks
) noexcept;

/// Reports a broadcast chat line for the local player through the upload hooks and to the library.
///
/// The line is UTF-8. Both destinations receive those bytes unchanged. The
/// report record converts them when it is written.
///
/// @param[in,out] reporter Reporter whose player table is refreshed.
/// @param world World holding the players.
/// @param line Chat line, as UTF-8.
/// @param hooks Platform hooks.
/// @return result::ok, the library's result, refused when the simulation is not one the Boneyards reports,
///         or unavailable when neither destination takes chat.
int32_t
report_chat(Reporter& reporter, World& world, const char* line, const Hooks& hooks) noexcept;

/// Runs the per-frame reporter step: the library timer and, when its countdown runs out, the score upload.
///
/// Does nothing when the simulation is not one the Boneyards reports.
///
/// @param[in,out] reporter Reporter whose upload countdown advances.
/// @param[in,out] world World passed to report_event for the interval upload.
/// @param hooks Platform hooks.
void step_reporter(Reporter& reporter, World& world, const Hooks& hooks) noexcept;

/// Tells whether the reporting library is loaded.
///
/// @param reporter Reporter to test.
/// @return True while the library is loaded.
[[nodiscard]] bool reporter_library_loaded(const Reporter& reporter) noexcept;

} // namespace oa::boneyards::reporter
