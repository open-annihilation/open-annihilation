// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boundary between the Boneyards screens and the online service.
//
// The game's client reaches Central Command through the Rover interface
// table; a screen request that needs the server goes through one of its
// slots. Matchmaking is the exception: its records are built and read beside
// the other lobby traffic, not through a new slot. Each request is a
// function pointer here; the online binding (boneyards client package) calls
// the Rover table and the offline default answers "not connected" or empty
// data. What the client reports while it pumps reaches the screens through
// ServiceEvents. Other Boneyards packages include this header and extend it
// only through its owner.
#pragma once

#include "oa/boneyards/utf8.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards {

enum class ServiceResult : int32_t {
    ok = 0,
    not_connected = 1, // offline default for every request
    failed = 2,
    pending = 3, // accepted; the answer arrives through a callback
};

// Result of bringing up the online client module.
enum class ActivationResult : int32_t {
    ok = 0,
    module_missing = 1, // the client module could not be loaded
    attach_failed = 2,
    activate_failed = 3,
};

// Where a located user currently is; the numeric values are the locator
// status codes the chat console formats.
enum class LocateStatus : int32_t {
    logged_out = 0,
    chatting = 1,
    examining_battles = 2,
    in_battle = 3,
    other_server = 4,
    no_such_user = 5,
    viewing_pages = 6, // map, news, help, ladders or recon
};

inline constexpr std::size_t kLocateTextCapacity = 64;

// Fields of one locator reply record (keys area_id, room_id, game_id,
// pass_req, game_name, area_name, room_name, server_name).
struct LocateReport {
    int32_t status; // LocateStatus, or any other value for a locator error
    char user_name[kLocateTextCapacity];
    uint32_t area_id;
    uint32_t room_id;
    uint32_t pass_required;
    char game_id[kLocateTextCapacity];
    char game_name[kLocateTextCapacity];
    char area_name[kLocateTextCapacity];
    char room_name[kLocateTextCapacity];
    char server_name[kLocateTextCapacity];
};

using LocateCallback = void (*)(void* context, const LocateReport* report);

// One answer to a battle search: a battle whose name holds the text asked
// for. A search that matches nothing is answered once, with no game id.
struct GameLocateReport {
    uint32_t area_id{}; // planet the battle is on
    char game_id[kLocateTextCapacity]{};
    char name[kLocateTextCapacity]{};
    int32_t type{};       // game mode
    bool forming{};       // false once the battle is in progress
    bool pass_required{}; // the battle asks for a password
    bool last{};          // no further answer follows for this search
    // Seats of a forming battle. Known only when the answer carried both
    // players (seats taken, the host included) and playerlimit.
    bool seats_known{};
    int32_t players{};
    int32_t playerlimit{};
};

using GameLocateCallback = void (*)(void* context, const GameLocateReport* report);

namespace screens {
struct ProfileFields;
}

// A profile request's replies: each ladder standing of the player, then the
// profile itself (ok false when it could not be read). The fields are the
// reply's and can be read, and written back, only during the call.
using ProfileCallback = void (*)(void* context, bool ok, const screens::ProfileFields* fields);
using LadderCallback = void (*)(
    void* context, int32_t ladder_id, int32_t ladder_subid, const screens::ProfileFields* fields
);

// Account fields the login screen submits.
struct LoginRequest {
    const char* user_name;
    const char* password;
    bool register_new; // true for the registration path
    // Server list entry (its location text); null for the list's selected entry.
    const char* server_name;
};

// Levels of the Central Command session (SessionRecord.level).
enum class SessionLevel : int32_t {
    offline = 0,
    client_active = 1,  // client module loaded, attached and activated
    disconnected = 2,   // the link dropped outside a login attempt
    logged_in = 3,      // login confirmed; the dossier is being fetched
    dossier_loaded = 4, // the planetary functions may load
};

// A player name is at most this many characters. The session stores that many.
inline constexpr int32_t kPlayerNameCharacters = 15;
inline constexpr std::size_t kSessionNameCapacity =
    utf8::bytes_for(static_cast<std::size_t>(kPlayerNameCharacters));

// Kinds of system alert, as the Main Frame's alert handler tells them apart.
enum class AlertKind : int32_t {
    ticker = 0,       // text for the news ticker
    message_box = 1,  // text for a message box
    page = 2,         // a page; the fields carry the sender's user_id
    notification = 3, // the fields name a notification_type (PROMOTION)
};

// The notification_type of a promotion.
inline constexpr const char* kPromotionNotification = "PROMOTION";

// A system alert with the field of its property bag that its kind reads;
// the texts live for the call.
struct AlertReport {
    int32_t kind; // AlertKind
    const char* text;
    uint32_t sender_id;            // a page's user_id (bit 31 set for a guest), 0 without one
    const char* notification_type; // a notification's notification_type, "" without one
};

// The session fields of the Boneyards system object that the screens read.
struct SessionRecord {
    SessionLevel level;
    uint32_t user_id;              // from login_confirmed
    char allegiance;               // 'A', 'C' or 'U'; 'U' when the dossier omits ALLEGIANCE
    char allegiance_previous = 0;  // 'A' or 'C' from ALLEGIANCE_PREVIOUS, 0 when it names none
    int32_t allegiance_locked = 0; // ALLEGIANCE_LOCKED, 0 or 1
    char user_name[kSessionNameCapacity]; // truncated to 15 characters
    uint32_t area_id;                     // from login_confirmed
    uint8_t matchmaking_enabled;          // set for a dossier USER_STATUS of 4 or 5
};

inline constexpr std::size_t kSectorNameCapacity = 0x33;

// An open war-map file (the Rover sector reader).
struct SectorMapReader;

// Header record of a war-map file.
struct SectorMapHeader {
    int32_t sector_id;
    char sector_name[kSectorNameCapacity];
    int32_t width; // map image size in map units
    int32_t height;
    int32_t planet_count;
    int32_t connector_count;
};

// One planet of a war-map file: the comms layer's 0x500-byte record, field
// for field, whose layout the Rover interface shares.
struct SectorPlanetRecord {
    int32_t id;
    char name[0x10];
    int32_t x;
    int32_t y;
    char seqname[0x20]; // sprite sequence base name without its size letter
    int32_t size_class; // 0, 1 or 2 for a seqname ending _L, _M or _S
    int32_t planet_rot;
    char side; // 'A', 'C' or 'I'
    bool capitol;
    char desc[0x400];
    char ta_map[0x3f];
    char ta_mapsource[4];
    char ta_maptype[0x10];
    char pan_gaf[0x20];
    char pan_seq[0x20];
    int32_t playerlimit;
    int32_t deathopt;
    int32_t energy;
    int32_t metal;
    int32_t location;
    int32_t los;
    int32_t mapping;
    int32_t maxunits;
    int32_t watching;
};

static_assert(sizeof(SectorPlanetRecord) == 0x500, "SectorPlanetRecord size");
static_assert(offsetof(SectorPlanetRecord, size_class) == 0x3c, "size_class offset");
static_assert(offsetof(SectorPlanetRecord, side) == 0x44, "side offset");
static_assert(offsetof(SectorPlanetRecord, desc) == 0x46, "desc offset");
static_assert(offsetof(SectorPlanetRecord, ta_mapsource) == 0x485, "ta_mapsource offset");
static_assert(offsetof(SectorPlanetRecord, pan_seq) == 0x4b9, "pan_seq offset");
static_assert(offsetof(SectorPlanetRecord, playerlimit) == 0x4dc, "playerlimit offset");

// A war-map link between two planets, by id.
struct ConnectorRecord {
    int32_t planet1;
    int32_t planet2;
};

// A census event for one planet, in the order the event delivers it.
struct CensusReport {
    uint32_t area_id; // the planet's id
    int32_t population;
    int32_t activity;
    int32_t in_progress;
    int32_t forming;
    int32_t core_wins;
    int32_t arm_wins;
};

// A battle's settings answering get_battle (battle_intel); the texts live
// for the call.
struct BattleIntel {
    const char* game_id;
    uint32_t user_id; // the host
    const char* user_name;
    int32_t has_password;
    const char* mission;
    int32_t skill;    // 0 novice, 1 seasoned, 2 expert
    int32_t players;  // number-of-players selector: 0 two, 1 three, 2 four, 3 one
    int32_t on_death; // 1 continues, 2 ends, 3 deathmatch
    bool failed;
};

// One player's line of a battle's report answering get_battle
// (battle_report); the texts live for the call.
struct BattleReport {
    const char* game_id;
    uint32_t user_id;
    int32_t icon; // User_Icons frame
    const char* user_name;
    int32_t kills;
    int32_t losses;
    int32_t energy;
    int32_t metal;
    int32_t score;
    const char* message; // empty on a score line
};

// The launch_game reply to a battle hosted or joined: result 0 launches the
// game the other fields describe, any other refuses it. The texts live for
// the call.
struct LaunchReport {
    const char* game_id;
    const char* session;
    const char* address;
    const char* password;
    bool creator;
    bool lock_options;
    const char* mission;
    int32_t playerlimit;
    int32_t deathopt;
    int32_t energy;
    int32_t metal;
    int32_t location;
    int32_t los;
    int32_t mapping;
    int32_t maxunits;
    int32_t watching;
    int32_t cheats;
    int32_t tournament;
    const char* allegiance;
    int32_t team;
    int32_t result;
    // The planet the launch_game reply names. A reply without one leaves this
    // 0. left_game and the battle's uploads use this planet, not the planet
    // the Main Frame has selected.
    uint32_t area_id = 0;
};

// Where the online client reports what happens while it pumps (and during a
// login request). Every entry may be null.
struct ServiceEvents {
    void* user;
    // Adds a line to the login dialog's status.
    void (*login_status)(void* user, const char* text);
    // The login attempt is over: the dialog returns to idle with LOGIN enabled.
    void (*login_reset)(void* user);
    // The session record changed.
    void (*session_changed)(void* user, const SessionRecord* session);
    // Text for the Main Frame's notice box.
    void (*notice)(void* user, const char* text);
    // A Central Command system alert.
    void (*system_alert)(void* user, const AlertReport* alert);
    // True while a battle launched from the Boneyards is running.
    bool (*battle_active)(void* user);
    // A line for the in-battle message area.
    void (*battle_message)(void* user, const char* text);
    // The war-map feeds: one allegiance letter per planet in id order, one
    // activity digit per planet in id order, and one planet's census.
    void (*allegiances)(void* user, const char* allegiances);
    void (*activity)(void* user, const char* digits);
    void (*census)(void* user, const CensusReport* report);
    // The chat room feeds, field for field as the client reports them. A
    // room-list row (change 0 adds or refreshes, 1 removes) answering a
    // room-list request, the row marked last ending it; the same rows as the
    // entered room's area changes; the result of entering or switching to a
    // room (0 entered); a member of the entered room arriving, changing or
    // leaving (latency 1 to 3); a line said in it (from_server when it names
    // no user); the result of leaving it; and the frequency a creation made
    // (area or room 0 when it failed). global_known is false when that row
    // carried no global; global 1 is the sector-wide frequency. The two
    // travel with the row, on this call, and are not remembered after it.
    void (*room_listed)(
        void* user,
        int32_t change,
        uint32_t area_id,
        uint32_t room_id,
        const char* name,
        const char* description,
        bool needs_password,
        int32_t users,
        int32_t type,
        const char* tag,
        bool last,
        bool global_known,
        int32_t global
    );
    void (*room_updated)(
        void* user,
        int32_t change,
        uint32_t area_id,
        uint32_t room_id,
        const char* name,
        const char* description,
        bool needs_password,
        int32_t users,
        int32_t type,
        const char* tag
    );
    void (*room_entered)(void* user, int32_t result);
    void (*room_member)(
        void* user,
        int32_t change,
        uint32_t user_id,
        const char* name,
        int32_t latency,
        int32_t icon,
        int32_t rank,
        const char* allegiance
    );
    void (*chat_message)(void* user, bool from_server, uint32_t user_id, const char* text);
    void (*room_left)(void* user, bool failed);
    void (*room_created)(void* user, uint32_t area_id, uint32_t room_id, const char* name);
    // A document fetch_file started has finished: its URL, the file it was
    // saved to and the fetch status (kFetchFailed when it failed).
    void (*file_arrived)(void* user, const char* url, const char* path, int32_t status);
    // The battle feeds, field for field as the client reports them. The
    // answer to entering or leaving an area's battles; a battle of the
    // entered area added or refreshed (change 0) or removed (change 1), with
    // forming 1 while it forms and host_code its host's side (0 Arm, 1 Core,
    // 2 unknown); a line for the planet intelligence list (game_id null when
    // it names no battle); a battle's intel and each line of its report,
    // answering get_battle; the launch of a battle hosted or joined; and,
    // twice once it has run, the room its players are debriefed in (first
    // on the first).
    void (*battles_done)(void* user, bool failed);
    void (*battle_info)(
        void* user,
        int32_t change,
        const char* game_id,
        const char* name,
        int32_t type,
        int32_t forming,
        int32_t host_code
    );
    void (*intel)(void* user, const char* game_id, const char* text);
    void (*battle_intel)(void* user, const BattleIntel* intel);
    void (*battle_report)(void* user, const BattleReport* report);
    void (*launch)(void* user, const LaunchReport* launch);
    void (*debriefing)(void* user, uint32_t area_id, uint32_t room_id, bool first);
};

// Status of a fetched document that did not arrive.
inline constexpr int32_t kFetchFailed = 1;

// Content server kinds: the News and the Ladders consoles' pages.
inline constexpr int32_t kContentServerNews = 0;
inline constexpr int32_t kContentServerLadders = 1;

// Fields the Profile console's APPLY can send: PASSWORD, TAG, HAS_CC,
// HAS_BT, TAFORUM_ALIAS, ICQ_ALIAS, EMAIL_ALIAS, ALLEGIANCE, NUMPLAYERS,
// PLAYERSKILL, ONCOMDEATH and FAVORITEMAP, each at most once.
inline constexpr std::size_t kProfileEditCapacity = 12;
// Bytes a field's text keeps, terminator included: the longest field text a
// profile reply gives.
inline constexpr std::size_t kProfileEditValueCapacity = 0x400;

// One field of a profile change: the property-bag key and its text.
struct ProfileEditField {
    const char* key; // a static key name
    char value[kProfileEditValueCapacity];
};

// The property bag of a profile change the Profile console's APPLY builds,
// its fields in the order it adds them.
struct ProfileEdits {
    ProfileEditField fields[kProfileEditCapacity];
    std::size_t count;
};

// Answer to a profile change: whether every field was stored.
using ProfileUpdatedCallback = void (*)(void* context, bool ok);

// Profile.def's categories, one per Profile console tab.
inline constexpr std::size_t kProfileCategoryCount = 7;

// One ladder_template record of Profile.def.
struct LadderTemplate {
    int32_t type{}; // ladder type
    int32_t subtype{};
    int32_t sort{}; // order among the ladders
    char* page{};   // the template file's text; null when it could not be read
};

// The tables Profile.def fills the first time a profile record is built.
// Texts are heap copies owned by the tables.
struct ProfileTemplates {
    bool loaded{};
    char* category_descriptions[kProfileCategoryCount]{}; // by category id
    char* category_names[kProfileCategoryCount]{};
    LadderTemplate** ladders{}; // in file order
    int32_t ladder_count{};
    int32_t ladder_capacity{};
    char* byinfo_page{}; // profile_byinfo_template text
    char* popup_page{};  // profile_popup_template text
};

// Battle hosting parameters from the game setup panel.
struct HostGameRequest {
    uint32_t area_id;
    int32_t game_mode; // GameMode value
    const char* name;
    const char* password;    // null for an open game
    int32_t skill;           // 0 novice, 1 seasoned, 2 expert
    int32_t players;         // number-of-players radio index
    int32_t commander_death; // 1 continues, 2 ends, 3 deathmatch
};

/// Bytes, terminator included, of a matchmaking offer id or game id (14 hex characters).
inline constexpr std::size_t kMatchmakingIdCapacity = 16;

/// Bytes, terminator included, of a matchmaking opponent or battle name (47 characters).
inline constexpr std::size_t kMatchmakingNameCapacity = 48;

/// Filters of matchmaking_look and matchmaking_join.
///
/// game_type is a GameMode value. The bracket is not a field: the server reads it itself.
struct MatchmakingQuery {
    int32_t game_type{};
    int32_t skill_mode{}; // 0 near mine, 1 any
    int32_t side{};       // 0 Arm, 1 Core, 2 Either
    int32_t planets{};    // 0 this planet, 1 any planet
    uint32_t area_id{};
    bool area_known{}; // false omits area_id
};

/// One matchmaking_status.
///
/// count is the commanders in the planet scope. meter is the Queue_Meter frame.
struct MatchmakingStatusNotice {
    int32_t result{};
    int32_t queued{};
    int32_t count{};
    int32_t meter{};
};

/// One matchmaking_found. The strings live in this record, not in the inbound entry.
struct MatchmakingFoundNotice {
    char offer_id[kMatchmakingIdCapacity]{};
    char name[kMatchmakingNameCapacity]{};
    int32_t skill{};
    int32_t game_type{};
    int32_t role{};
    int32_t side{};
    uint32_t area_id{};
};

/// One matchmaking_go.
///
/// Host fields are set when role is 1 and result is 0. game_id is set when role is 0 and result
/// is 0.
struct MatchmakingGoNotice {
    char offer_id[kMatchmakingIdCapacity]{};
    int32_t result{};
    int32_t role{};
    uint32_t area_id{};
    char game_id[kMatchmakingIdCapacity]{};
    bool game_known{};
    char name[kMatchmakingNameCapacity]{};
    bool name_known{};
    int32_t game_type{};
    int32_t skill{};
    int32_t seats{};
    int32_t on_death{};
    int32_t side{};
    bool side_known{};
};

/// Bytes, terminator included, of a QuickPlay planet name (15 characters).
inline constexpr std::size_t kQuickPlayPlanetBytes = 16;

/// Bytes, terminator included, of a QuickPlay battle or offer name (31 characters).
inline constexpr std::size_t kQuickPlayBattleBytes = 32;

/// Bytes, terminator included, of a QuickPlay favorite or mission name (47 characters).
inline constexpr std::size_t kQuickPlayMapBytes = 48;

/// Filters of quickplay_join. game_type is a GameMode value, not the console's row.
struct QuickPlayQuery {
    int32_t game_type{};
    int32_t where{}; // 0 any planet, 1 Fuel Dump
    int32_t skill{}; // 0 Novice, 1 Seasoned, 2 Expert
    int32_t side{};  // 0 Arm, 1 Core, 2 Either
    int32_t prefer_favorite{};
    char favorite[kQuickPlayMapBytes]{};
    uint32_t area_id{};
    bool area_known{}; // false omits area_id
};

/// One quickplay_status. meter is the Queue_Meter frame. planets is set while queued.
struct QuickPlayStatusNotice {
    int32_t result{};
    int32_t queued{};
    int32_t meter{};
    int32_t fits{};
    int32_t planets{};
};

/// One quickplay_scan. The client writes the sentence. The server sends no finished line.
struct QuickPlayScanNotice {
    int32_t seq{};
    int32_t kind{}; // 1 empty, 2 skipped, 3 a fit
    int32_t reason{};
    int32_t game_type{};
    int32_t skill{};
    int32_t seats{};
    int32_t seats_max{};
    char planet[kQuickPlayPlanetBytes]{};
    char battle[kQuickPlayBattleBytes]{};
};

/// One quickplay_withdrawn. game_id is set when the offer named a battle.
/// reason is 0 the battle filled, 1 it started, 2 it was abandoned, 3 the seat reservation lapsed.
struct QuickPlayWithdrawnNotice {
    char offer_id[kMatchmakingIdCapacity]{};
    char game_id[kMatchmakingIdCapacity]{};
    bool game_known{};
    int32_t reason{};
};

/// One quickplay_found. game_id is set for a join. seats and on_death are set for a host.
struct QuickPlayFoundNotice {
    char offer_id[kMatchmakingIdCapacity]{};
    int32_t role{}; // 0 join, 1 host
    uint32_t area_id{};
    char game_id[kMatchmakingIdCapacity]{};
    bool game_known{};
    char name[kQuickPlayBattleBytes]{};
    char planet[kQuickPlayPlanetBytes]{};
    int32_t game_type{};
    int32_t skill{};
    int32_t seats{};
    int32_t on_death{};
    char map[kQuickPlayMapBytes]{};
    bool map_known{};
};

/// Which inbound QuickPlay record a notice carries.
enum class QuickPlayNoticeKind : int32_t {
    none = 0,
    status = 1,
    scan = 2,
    found = 3,
    withdrawn = 4,
};

/// One inbound QuickPlay record, copied out of the pump that delivered it.
struct QuickPlayNotice {
    QuickPlayNoticeKind kind{QuickPlayNoticeKind::none};
    QuickPlayStatusNotice status{};
    QuickPlayScanNotice scan{};
    QuickPlayFoundNotice found{};
    QuickPlayWithdrawnNotice withdrawn{};
};

/// Which inbound matchmaking record a notice carries.
enum class MatchmakingNoticeKind : int32_t { none = 0, status = 1, found = 2, go = 3 };

/// One inbound matchmaking record, copied out of the pump that delivered it.
struct MatchmakingNotice {
    MatchmakingNoticeKind kind{MatchmakingNoticeKind::none};
    MatchmakingStatusNotice status{};
    MatchmakingFoundNotice found{};
    MatchmakingGoNotice go{};
};

struct BoneyardsService {
    void* user;
    void (*list_servers)(void* user, void (*entry)(void*, const char*), void* context){};
    // Region of a server listed by location, or null when that server's list
    // named none. Null leaves Server Transfer's WHERE empty.
    const char* (*server_region)(void* user, const char* location){};
    const char* (*current_server)(void* user){};
    // True when a usable network adapter is present (main-menu entry check).
    bool (*network_available)(void* user);
    // Loads and starts the online client module.
    ActivationResult (*activate)(void* user);
    // Logs off and unloads the online client module.
    void (*close)(void* user);
    // True while a session with Central Command is open.
    bool (*connected)(void* user);
    // Logs in (or registers) and opens a session.
    ServiceResult (*login)(void* user, const LoginRequest* request);
    // Cancels a login in progress.
    void (*cancel_login)(void* user);
    // Sends chat text to a room; target_user 0 broadcasts to the room.
    ServiceResult (*send_chat)(
        void* user, uint32_t room_id, uint32_t area_id, uint32_t target_user, const char* text
    );
    // Pages a user anywhere on the server.
    ServiceResult (*page_user)(void* user, const char* user_name, const char* text);
    // Starts a locator query; the callback receives the reply.
    ServiceResult (*locate_user)(
        void* user, const char* user_name, LocateCallback callback, void* context
    );
    // Requests entry to a chat frequency (room) in an area.
    ServiceResult (*join_room)(
        void* user, uint32_t area_id, uint32_t room_id, const char* password, const char* room_name
    );
    // Moves from the current frequency to another; the old one is left once
    // the new one has been entered.
    ServiceResult (*switch_room)(
        void* user,
        uint32_t area_id,
        uint32_t room_id,
        const char* password,
        uint32_t old_area_id,
        uint32_t old_room_id,
        const char* room_name
    );
    // Requests the room list of an area.
    ServiceResult (*request_room_list)(void* user, uint32_t area_id);
    // Leaves the current frequency.
    ServiceResult (*leave_room)(void* user, uint32_t area_id, uint32_t room_id);
    // Creates a frequency in an area; password may be empty or null for a
    // public one.
    ServiceResult (*create_room)(
        void* user,
        uint32_t area_id,
        const char* name,
        const char* description,
        const char* password
    );
    // Requests a player's profile; its replies go to the callbacks (either
    // may be null) with the context.
    ServiceResult (*request_profile)(
        void* user,
        uint32_t user_id,
        ProfileCallback on_profile,
        LadderCallback on_ladder,
        void* context
    );
    // Sends a user's profile changes as one property bag (set_profile); the
    // answer goes to the callback with the context. A bag without fields is
    // not sent.
    ServiceResult (*submit_profile)(
        void* user,
        uint32_t user_id,
        const ProfileEdits* edits,
        ProfileUpdatedCallback updated,
        void* context
    );
    // Hosts a battle on the current planet; its launch arrives through the
    // launch event.
    ServiceResult (*host_game)(void* user, const HostGameRequest* request);
    // Enters an area's battles: its battles and intelligence lines arrive
    // through the battle events, the answer through battles_done.
    ServiceResult (*enter_battles)(void* user, uint32_t area_id);
    // Leaves an area's battles; the answer arrives through battles_done.
    ServiceResult (*leave_battles)(void* user, uint32_t area_id);
    // Asks for a battle's intel and report (battle_intel, battle_report).
    ServiceResult (*get_battle)(void* user, uint32_t area_id, const char* game_id);
    // Joins a forming battle, password null when its panel asks none; the
    // launch arrives through the launch event.
    ServiceResult (*join_game)(
        void* user, uint32_t area_id, const char* game_id, const char* password
    );
    // Host and port of the content server of one kind (kContentServerNews
    // or kContentServerLadders) used by "#Remote#" locators; false while the
    // server has named none.
    bool (*content_server)(
        void* user, int32_t server_type, char* host, std::size_t host_capacity, uint16_t* port
    );
    // Downloads a document URL into a new temporary file, whose path is
    // written to dest_path; its arrival is reported through file_arrived.
    ServiceResult (*fetch_file)(
        void* user, const char* url, char* dest_path, std::size_t dest_capacity
    );
    // Local people lists ("Silenced", ...): membership, insert, erase, and
    // release, which writes them back when the chat console closes.
    bool (*peoplelist_contains)(void* user, const char* list, uint32_t user_id);
    void (*peoplelist_insert)(void* user, const char* list, uint32_t user_id);
    void (*peoplelist_erase)(void* user, const char* list, uint32_t user_id);
    void (*peoplelist_release)(void* user);
    // Runs the client's per-frame pump; events arrive from inside it.
    void (*pump)(void* user);
    // Installs where events are delivered (copied; null removes the sink).
    void (*set_events)(void* user, const ServiceEvents* events);
    // Name of the server whose yard master answered the login (its war map
    // installs as Boneyards\<name>_map.gwm); null when none did.
    const char* (*area_name)(void* user);
    // Opens a war-map file and reads its header; null when the file is
    // missing or its header is invalid.
    SectorMapReader* (*open_sector_map)(void* user, const char* path, SectorMapHeader* header);
    // Reads the planets, then the connectors, the header counts; false at the
    // first invalid record.
    bool (*read_sector_map)(
        void* user,
        SectorMapReader* reader,
        int32_t width,
        int32_t height,
        SectorPlanetRecord* planets,
        int32_t planet_count,
        ConnectorRecord* connectors,
        int32_t connector_count
    );
    void (*close_sector_map)(void* user, SectorMapReader* reader);
    // Asks for the planets' allegiances (answered through the allegiances event).
    void (*request_allegiance)(void* user);
    // Asks for one planet's census, or with area 0 for every planet's and
    // the activity digits (answered through the census and activity events).
    void (*request_census)(void* user, uint32_t area_id);
    // Profile.def's tables, read the first time they are asked for; null
    // when the service has none.
    const ProfileTemplates* (*profile_templates)(void* user);
    // Logs out of the current server. Once that finishes, the login reset
    // runs; a session that is not logged in resets at once.
    void (*logout)(void* user){};
    // Searches every planet's battles for names that hold `name`; each
    // answer goes to the callback with the context, the last marked as such.
    ServiceResult (*locate_game)(
        void* user, const char* name, GameLocateCallback callback, void* context
    ){};
    /// Asks how many commanders are queued for these filters. The answer is a status notice.
    ServiceResult (*matchmaking_look)(void* user, const MatchmakingQuery* query){};
    /// Joins the queue, or replaces the filters of a ticket already queued.
    ServiceResult (*matchmaking_join)(void* user, const MatchmakingQuery* query){};
    /// Leaves the queue. Close does not call this.
    ServiceResult (*matchmaking_cancel)(void* user){};
    /// Answers the open offer. accept is 1 for Yes and 0 for No.
    ServiceResult (*matchmaking_answer)(void* user, const char* offer_id, int32_t accept){};
    /// Takes the next status, found or go copied during a pump. False when none is waiting.
    bool (*matchmaking_take)(void* user, MatchmakingNotice* out){};
    /// Starts a QuickPlay search, or replaces one already running.
    ServiceResult (*quickplay_join)(void* user, const QuickPlayQuery* query){};
    /// Leaves the QuickPlay search.
    ServiceResult (*quickplay_cancel)(void* user){};
    /// Answers the open QuickPlay offer. accept is 1 for Yes and 0 for No.
    ServiceResult (*quickplay_answer)(void* user, const char* offer_id, int32_t accept){};
    /// Takes the next status, scan or found copied during a pump. False when none is waiting.
    bool (*quickplay_take)(void* user, QuickPlayNotice* out){};
};

// Offline people-list storage used by the offline service.
inline constexpr std::size_t kOfflinePeopleCapacity = 256;

struct OfflineServiceState {
    uint32_t silenced[kOfflinePeopleCapacity];
    std::size_t silenced_count;
};

/// Builds a service that is never connected.
///
/// @param state people-list storage; null keeps the lists always empty
/// @return the service
BoneyardsService boneyards_offline_service(OfflineServiceState* state);

/// Fills every null entry of a service with the offline behaviour.
///
/// @param[in,out] service service to complete
void boneyards_service_fill_offline(BoneyardsService* service);

// Name of the people list that records muzzled users.
inline constexpr const char* kSilencedList = "Silenced";

} // namespace oa::boneyards
