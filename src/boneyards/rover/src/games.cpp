// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Hosting, joining and reporting games (slots 20-31): host_game and
// join_game, whose launch_game reply starts the game and arms the debriefing
// that follows it, and the in-game reports: left_game, game_settings,
// player_info and score_report. Requests go to the game manager. The
// key/value variants (slots 20, 22, 25, 27 and 29) add a caller's property
// bag to the request, and slot 22's launch hands the game a bag of the
// reply's other fields.

#include "bags.h"
#include "lobby.h"
#include "lobby_handlers.h"
#include "protocol.h"
#include "requests.h"
#include "slots.h"
#include "state.h"
#include "wire_text.h"

#include "oa/rover/launch_overlay.h"

#include <string.h>

namespace {

const char* g_launch_wire = nullptr;
int32_t g_launch_wire_length = 0;

} // namespace

void rover_set_launch_wire(const char* text, int32_t length) {
    if (text == nullptr || length < 0) {
        g_launch_wire = nullptr;
        g_launch_wire_length = 0;
        return;
    }
    g_launch_wire = text;
    g_launch_wire_length = length;
}

void rover_clear_launch_wire(void) {
    g_launch_wire = nullptr;
    g_launch_wire_length = 0;
}

const char* rover_launch_wire(int32_t* length) {
    if (length != nullptr)
        *length = g_launch_wire_length;
    return g_launch_wire;
}

namespace {

constexpr char kCommandHostGame[] = "host_game";
constexpr char kCommandJoinGame[] = "join_game";
constexpr char kCommandLeftGame[] = "left_game";
constexpr char kCommandGameSettings[] = "game_settings";
constexpr char kCommandPlayerInfo[] = "player_info";
constexpr char kCommandScoreReport[] = "score_report";
constexpr char kCommandLaunchGame[] = "launch_game";
constexpr char kCommandDebriefing[] = "debriefing";

constexpr char kKeyKeyword[] = "keyword";
constexpr char kKeySkill[] = "skill";
constexpr char kKeyPlayers[] = "players";
constexpr char kKeyOnDeath[] = "on_death";
constexpr char kKeySession[] = "session";
constexpr char kKeyAddress[] = "address";
constexpr char kKeyCreator[] = "creator";
constexpr char kKeyLockOptions[] = "lockOptions";
constexpr char kKeyPlayerLimit[] = "playerlimit";
constexpr char kKeyDeathOption[] = "deathopt";
constexpr char kKeyLocation[] = "location";
constexpr char kKeyLineOfSight[] = "los";
constexpr char kKeyMapping[] = "mapping";
constexpr char kKeyMaxUnits[] = "maxunits";
constexpr char kKeyWatching[] = "watching";
constexpr char kKeyCheats[] = "cheats";
constexpr char kKeyTournament[] = "tournament";
constexpr char kKeyTeam[] = "team";
constexpr char kKeyKills[] = "kills";
constexpr char kKeyLosses[] = "losses";
constexpr char kKeyExcessEnergy[] = "xs_energy";
constexpr char kKeyExcessMetal[] = "xs_metal";
constexpr char kKeyScore[] = "score";
constexpr char kKeyVictory[] = "victory";

// The game options with the value game_settings leaves out and launch_game
// assumes when a field is absent. mapping and maxunits differ between them.
struct GameOptionDefaults {
    int32_t lock_options;
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
};

constexpr GameOptionDefaults kSettingsOmitted = {0, 2, 2, 1000, 1000, 2, 1, 1, 250, 1, 1, 0};
constexpr GameOptionDefaults kLaunchDefaults = {0, 2, 2, 1000, 1000, 2, 1, 2, 0, 1, 1, 0};

// A launch_game result of 0 starts the game; the debriefing then comes twice.
constexpr int32_t kLaunchOk = 0;
constexpr int32_t kDebriefingCount = 2;
// "creator" and "lockOptions" are true only when 1.
constexpr int32_t kFlagOn = 1;
// A flag field of score_report.
constexpr int32_t kReportFlag = 1;

// Match reports and chat relays (slots 26-31) cut each text field to the bytes its wire form may
// take, on a character boundary. The kept text stays UTF-8. Four UTF-8 bytes is the longest one
// wire byte stands for. The game id is the length already stored for a game. A player name fits
// the 30-byte name with its terminator, a side the 16-byte allegiance, a mission the 128-byte
// mission text. A chat line, and any other string on a report bag, fits 255 bytes.
constexpr int32_t kWireKeepBytes = 1024;
constexpr int32_t kWirePlayerNameBytes = 29;
constexpr int32_t kWireSideBytes = 15;
constexpr int32_t kWireMissionBytes = 127;
constexpr int32_t kWireChatBytes = 255;
constexpr int32_t kWireReportFieldBytes = 255;

/// Copies text cut to the wire limit. Null stays null, so a field left out stays left out.
///
/// @param[out] buffer room for the kept UTF-8, capacity bytes
/// @param capacity size of buffer
/// @param text text to cut, or null
/// @param limit most wire bytes
/// @return buffer, or null when text is null
const char* fit_text(char* buffer, int32_t capacity, const char* text, int32_t limit) {
    if (!text)
        return nullptr;
    rover_keep_for_wire(buffer, capacity, text, static_cast<int32_t>(strlen(text)), limit);
    return buffer;
}

// The host dialog's selectors as host_game sends them: skill 1 and 2 as they
// are (others as none), player selectors 1-3 as 3, 4 and 1 (others 2), and
// on_death 1 and 3 as they are (others 2).
int32_t wire_skill(int32_t selector) {
    return selector == 1 || selector == 2 ? selector : 0;
}

int32_t wire_players(int32_t selector) {
    switch (selector) {
    case 1:
        return 3;
    case 2:
        return 4;
    case 3:
        return 1;
    default:
        return 2;
    }
}

int32_t wire_on_death(int32_t selector) {
    return selector == 1 || selector == 3 ? selector : 2;
}

RoverEntry* game_request(RoverRecord** record, const char* command, uint32_t area_id) {
    RoverEntry* entry = rover_add_request(record, RoverService::GameManager, command);
    if (entry)
        rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
    return entry;
}

void set_text(RoverEntry& entry, const char* key, const char* value) {
    if (value)
        rover_entry_set_string(entry, key, value);
}

/// Appends a host_game entry with the fields every host sends: type, name, and a password and
/// description when given.
///
/// @param[in,out] record record to append to; a new one is created when *record is null
/// @param area_id area to host in
/// @param type game type
/// @param name game name, or null to leave it out
/// @param password game password, or null to leave it out
/// @param description game description, or null to leave it out
/// @return the new entry, or null when it could not be built
RoverEntry* request_host_game_basic(
    RoverRecord** record,
    uint32_t area_id,
    int32_t type,
    const char* name,
    const char* password,
    const char* description
) {
    RoverEntry* entry = game_request(record, kCommandHostGame, area_id);
    if (!entry)
        return nullptr;
    rover_entry_set_int(*entry, kRoverKeyType, type);
    set_text(*entry, kRoverKeyName, name);
    set_text(*entry, kRoverKeyPassword, password);
    set_text(*entry, kRoverKeyDescription, description);
    return entry;
}

/// Builds host_game from the host dialog: the basic fields plus a keyword and the skill, players
/// and on_death selectors, each sent when above 0.
///
/// @param area_id area to host in
/// @param type game type
/// @param name game name, or null to leave it out
/// @param password game password, or null to leave it out
/// @param description game description, or null to leave it out
/// @param keyword keyword, or null to leave it out
/// @param skill wire skill value; sent when above 0
/// @param players wire player count; sent when above 0
/// @param on_death wire on_death value; sent when above 0
/// @return the request record, or null when it could not be built
RoverRecord* request_host_game(
    uint32_t area_id,
    int32_t type,
    const char* name,
    const char* password,
    const char* description,
    const char* keyword,
    int32_t skill,
    int32_t players,
    int32_t on_death
) {
    RoverRecord* record = nullptr;
    RoverEntry* entry =
        request_host_game_basic(&record, area_id, type, name, password, description);
    if (!entry)
        return record;
    set_text(*entry, kKeyKeyword, keyword);
    if (skill > 0)
        rover_entry_set_int(*entry, kKeySkill, skill);
    if (players > 0)
        rover_entry_set_int(*entry, kKeyPlayers, players);
    if (on_death > 0)
        rover_entry_set_int(*entry, kKeyOnDeath, on_death);
    return record;
}

/// Appends a join_game entry, with a password when given.
///
/// @param[in,out] record record to append to; a new one is created when *record is null
/// @param area_id area of the game
/// @param game_id game to join, or null to leave it out
/// @param password game password, or null to leave it out
/// @return the new entry, or null when it could not be built
RoverEntry* request_join_game(
    RoverRecord** record, uint32_t area_id, const char* game_id, const char* password
) {
    RoverEntry* entry = game_request(record, kCommandJoinGame, area_id);
    if (entry) {
        set_text(*entry, kRoverKeyGameId, game_id);
        set_text(*entry, kRoverKeyPassword, password);
    }
    return entry;
}

/// Appends an entry naming one game: left_game, and game_settings or player_info before their
/// fields.
///
/// @param[in,out] record record to append to; a new one is created when *record is null
/// @param command command of the entry
/// @param area_id area of the game
/// @param game_id game the entry is about, or null to leave it out
/// @return the new entry, or null when it could not be built
RoverEntry* request_about_game(
    RoverRecord** record, const char* command, uint32_t area_id, const char* game_id
) {
    RoverEntry* entry = game_request(record, command, area_id);
    if (entry)
        set_text(*entry, kRoverKeyGameId, game_id);
    return entry;
}

// Adds an option unless it holds the value that is left out.
void set_option(RoverEntry& entry, const char* key, int32_t value, int32_t omitted) {
    if (value != omitted)
        rover_entry_set_int(entry, key, value);
}

/// Appends a player_info entry with one player's dpid and name.
///
/// @param[in,out] record record to append to; a new one is created when *record is null
/// @param area_id area of the game
/// @param game_id game the player is in
/// @param dpid the player's DirectPlay id
/// @param name the player's name, or null to leave it out
/// @return the new entry, or null when it could not be built
RoverEntry* request_player_info(
    RoverRecord** record, uint32_t area_id, const char* game_id, uint32_t dpid, const char* name
) {
    RoverEntry* entry = request_about_game(record, kCommandPlayerInfo, area_id, game_id);
    if (entry) {
        rover_entry_set_uint(*entry, kRoverKeyDpid, dpid);
        set_text(*entry, kRoverKeyName, name);
    }
    return entry;
}

/// Appends a score_report entry naming the player, with a message and the last flag when given.
///
/// @param[in,out] record record to append to; a new one is created when *record is null
/// @param area_id area of the game
/// @param game_id game the report is about
/// @param dpid the reporting player's DirectPlay id
/// @param message message text, or null to leave it out
/// @param last true to mark the player's last report
/// @return the new entry, or null when it could not be built
RoverEntry* request_score_message(
    RoverRecord** record,
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    const char* message,
    bool last
) {
    RoverEntry* entry = request_about_game(record, kCommandScoreReport, area_id, game_id);
    if (!entry)
        return nullptr;
    rover_entry_set_uint(*entry, kRoverKeyDpid, dpid);
    set_text(*entry, kRoverKeyMessage, message);
    if (last)
        rover_entry_set_int(*entry, kRoverKeyLast, kReportFlag);
    return entry;
}

// Sends a request whose entry could be built, after the caller's bag; false
// when the request could not be built. text_limit cuts each bag string to
// that many wire bytes; negative leaves the strings whole.
bool send_with_bag(
    RoverRecord* record, RoverEntry* entry, RoverPropertyBag* extra_fields, int32_t text_limit
) {
    if (!entry) {
        rover_record_destroy(&record);
        return false;
    }
    if (extra_fields)
        rover_walk_record_fields(extra_fields, *entry, text_limit);
    rover_send_to_boneyard(record);
    return true;
}

// The launch fields both launch_game handlers read first: area and game
// match the handler, the reply names an address and password.
enum class LaunchRead { Taken, Ignored, NotOurs };

struct LaunchReply {
    const RoverField* area{};
    const RoverField* game{};
    const char* session{};
    const char* address{};
    const char* password{};
};

LaunchRead read_launch(
    const RoverEntry& entry,
    bool any_game,
    uint32_t area_id,
    const char* game_id,
    LaunchReply* out_reply
) {
    out_reply->area = rover_entry_find(entry, kRoverKeyAreaId);
    out_reply->game = rover_entry_find(entry, kRoverKeyGameId);
    const RoverField* address = rover_entry_find(entry, kKeyAddress);
    const RoverField* password = rover_entry_find(entry, kRoverKeyPassword);
    if (!out_reply->area || !out_reply->game || !address || !password)
        return LaunchRead::Ignored;
    if (rover_field_uint(*out_reply->area, 0) != area_id)
        return LaunchRead::NotOurs;
    if (!any_game && !rover_game_id_matches(*out_reply->game, game_id))
        return LaunchRead::NotOurs;
    rover_entry_get_string(entry, kKeySession, &out_reply->session, kRoverNoText);
    rover_entry_get_string(entry, kKeyAddress, &out_reply->address, kRoverNoText);
    if (strlen(out_reply->address) == 0)
        return LaunchRead::Ignored;
    rover_entry_get_string(entry, kRoverKeyPassword, &out_reply->password, kRoverNoText);
    if (strlen(out_reply->password) == 0)
        return LaunchRead::Ignored;
    return LaunchRead::Taken;
}

int32_t launch_result(const RoverEntry& entry) {
    const RoverField* result = rover_entry_find(entry, kRoverKeyResult);
    return result ? rover_field_int(*result, -1) : kLaunchOk;
}

// A launch flag: present and 1.
bool launch_flag(const RoverEntry& entry, const char* key) {
    const RoverField* field = rover_entry_find(entry, key);
    return field && rover_field_int(*field, -1) == kFlagOn;
}

int32_t launch_option(const RoverEntry& entry, const char* key, int32_t fallback) {
    int32_t value = 0;
    rover_entry_get_int(entry, key, &value, fallback);
    return value;
}

// A started game is followed by two debriefing messages for it.
void arm_debriefing(
    RoverHandlerSet& handlers, const LaunchReply& reply, RoverDebriefingFn debriefing
) {
    RoverDebriefingHandler& handler = handlers.lobby.debriefing;
    rover_handler_disarm(handlers, RoverHandlerId::Debriefing);
    handler.area_id = rover_field_uint(*reply.area, 0);
    rover_store_game_id(handler.game_id, rover_field_text(*reply.game));
    handler.callback = debriefing;
    handler.remaining = kDebriefingCount;
    rover_handler_arm(handlers, RoverHandlerId::Debriefing);
}

/// Clears what the launch_game handler was armed with.
///
/// @param[out] handler handler to clear
/// @quirk Only the first byte of the game id is cleared.
void reset_launch_handler(RoverLaunchHandler& handler) {
    handler.any_game = false;
    handler.area_id = 0;
    handler.game_id[0] = '\0';
    handler.callback = nullptr;
    handler.debriefing = nullptr;
}

/// Takes launch_game for the handler's game and hands the game the game to start with every option,
/// then disarms the handler.
///
/// A result of 0 (or none) arms the debriefing that follows the game. A host takes the launch of
/// any game in its area, a joiner only that of the game it asked to join. An entry without area_id,
/// game_id, address or password, or with an empty address or password, is taken and ignored. Absent
/// options take the launch defaults; "creator" and "lockOptions" are true only when 1.
///
/// @param[in,out] handlers handler set holding the launch continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is launch_game and not for another area or game, false otherwise
bool accept_launch(RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry) {
    if (!rover_command_is(command, kCommandLaunchGame))
        return false;
    RoverLaunchHandler& handler = handlers.lobby.launch;
    LaunchReply reply;
    LaunchRead read =
        read_launch(entry, handler.any_game, handler.area_id, handler.game_id, &reply);
    if (read != LaunchRead::Taken)
        return read == LaunchRead::Ignored;
    bool creator = launch_flag(entry, kKeyCreator);
    bool lock_options = launch_flag(entry, kKeyLockOptions);
    const char* mission = nullptr;
    rover_entry_get_string(entry, kRoverKeyMission, &mission, kRoverNoText);
    const GameOptionDefaults& options = kLaunchDefaults;
    int32_t playerlimit = launch_option(entry, kKeyPlayerLimit, options.playerlimit);
    int32_t deathopt = launch_option(entry, kKeyDeathOption, options.deathopt);
    int32_t energy = launch_option(entry, kRoverKeyEnergy, options.energy);
    int32_t metal = launch_option(entry, kRoverKeyMetal, options.metal);
    int32_t location = launch_option(entry, kKeyLocation, options.location);
    int32_t los = launch_option(entry, kKeyLineOfSight, options.los);
    int32_t mapping = launch_option(entry, kKeyMapping, options.mapping);
    int32_t maxunits = launch_option(entry, kKeyMaxUnits, options.maxunits);
    int32_t watching = launch_option(entry, kKeyWatching, options.watching);
    int32_t cheats = launch_option(entry, kKeyCheats, options.cheats);
    int32_t tournament = launch_option(entry, kKeyTournament, options.tournament);
    const char* allegiance = nullptr;
    rover_entry_get_string(entry, kRoverKeyAllegiance, &allegiance, kRoverNoText);
    int32_t team = launch_option(entry, kKeyTeam, 0);
    int32_t result = launch_result(entry);
    if (handler.callback) {
        if (result == kLaunchOk)
            arm_debriefing(handlers, reply, handler.debriefing);
        rover_set_launch_wire(entry.wire_text, entry.wire_length);
        handler.callback(
            rover_show(rover_field_text(*reply.game)),
            rover_show(reply.session),
            rover_show(reply.address),
            rover_show(reply.password),
            creator,
            lock_options,
            rover_show(mission),
            playerlimit,
            deathopt,
            energy,
            metal,
            location,
            los,
            mapping,
            maxunits,
            watching,
            cheats,
            tournament,
            rover_show(allegiance),
            team,
            result
        );
        rover_clear_launch_wire();
    }
    rover_handler_disarm(handlers, RoverHandlerId::LaunchGame);
    reset_launch_handler(handler);
    return true;
}

/// Takes launch_game for the key/value requests and hands the game the launch with the reply's
/// other fields in a bag, then disarms the handler.
///
/// Replies are filtered as for accept_launch; a result of 0 (or none) arms the debriefing.
///
/// @param[in,out] handlers handler set holding the key/value launch continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is launch_game and not for another area or game, false otherwise
bool accept_launch_fields(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandLaunchGame))
        return false;
    RoverLaunchFieldsHandler& handler = handlers.lobby.launch_fields;
    LaunchReply reply;
    LaunchRead read =
        read_launch(entry, handler.any_game, handler.area_id, handler.game_id, &reply);
    if (read != LaunchRead::Taken)
        return read == LaunchRead::Ignored;
    int32_t result = launch_result(entry);
    static const char* const kDecoded[] = {
        kRoverKeyAreaId,
        kRoverKeyGameId,
        kKeyAddress,
        kRoverKeyPassword,
        kKeySession,
        kRoverKeyResult
    };
    constexpr int32_t kDecodedCount = static_cast<int32_t>(sizeof kDecoded / sizeof kDecoded[0]);
    RoverPropertyBag* fields = rover_collect_field_bag(entry, kDecoded, kDecodedCount);
    if (handler.callback) {
        if (result == kLaunchOk)
            arm_debriefing(handlers, reply, handler.debriefing);
        rover_set_launch_wire(entry.wire_text, entry.wire_length);
        handler.callback(
            rover_show(rover_field_text(*reply.game)),
            rover_show(reply.session),
            rover_show(reply.address),
            rover_show(reply.password),
            result,
            fields
        );
        rover_clear_launch_wire();
    }
    rover_release_field_bag(fields);
    rover_handler_disarm(handlers, RoverHandlerId::LaunchGameFields);
    handler = RoverLaunchFieldsHandler{};
    return true;
}

/// Takes a debriefing for the launched game and hands the game the room the players return to.
///
/// The game is told whether this is the first of the two debriefings; the handler disarms after the
/// second. An entry without area_id, game_id or room_id is taken and ignored.
///
/// @param[in,out] handlers handler set holding the debriefing continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is debriefing and not for another area or game, false otherwise
bool accept_debriefing(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandDebriefing))
        return false;
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* game = rover_entry_find(entry, kRoverKeyGameId);
    const RoverField* room = rover_entry_find(entry, kRoverKeyRoomId);
    if (!area || !game || !room)
        return true;
    RoverDebriefingHandler& handler = handlers.lobby.debriefing;
    if (rover_field_uint(*area, 0) != handler.area_id ||
        !rover_game_id_matches(*game, handler.game_id))
        return false;
    if (handler.callback)
        handler.callback(
            handler.area_id, rover_field_uint(*room, 0), handler.remaining == kDebriefingCount
        );
    if (--handler.remaining == 0) {
        rover_handler_disarm(handlers, RoverHandlerId::Debriefing);
        handler = RoverDebriefingHandler{};
    }
    return true;
}

// Arms a launch handler for a host (any game of the area) or a joiner.
void arm_launch(
    RoverHandlerSet& handlers,
    bool host,
    uint32_t area_id,
    const char* game_id,
    RoverLaunchGameFn launch,
    RoverDebriefingFn debriefing
) {
    RoverLaunchHandler& handler = handlers.lobby.launch;
    handler.any_game = host;
    handler.area_id = area_id;
    rover_store_game_id(handler.game_id, game_id);
    handler.callback = launch;
    handler.debriefing = debriefing;
    rover_handler_arm(handlers, RoverHandlerId::LaunchGame);
}

void arm_launch_fields(
    RoverHandlerSet& handlers,
    bool host,
    uint32_t area_id,
    const char* game_id,
    RoverLaunchGameKvFn launch,
    RoverDebriefingFn debriefing
) {
    RoverLaunchFieldsHandler& handler = handlers.lobby.launch_fields;
    handler.any_game = host;
    handler.area_id = area_id;
    rover_store_game_id(handler.game_id, game_id);
    handler.callback = launch;
    handler.debriefing = debriefing;
    rover_handler_arm(handlers, RoverHandlerId::LaunchGameFields);
}

} // namespace

bool rover_offer_game_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
) {
    switch (id) {
    case RoverHandlerId::LaunchGameFields:
        return accept_launch_fields(handlers, command, entry);
    case RoverHandlerId::LaunchGame:
        return accept_launch(handlers, command, entry);
    case RoverHandlerId::Debriefing:
        return accept_debriefing(handlers, command, entry);
    default:
        return false;
    }
}

bool ROVER_CALL rover_host_game_kv(
    uint32_t area_id,
    int32_t type,
    const char* name,
    const char* password,
    const char* description,
    RoverPropertyBag* extra_fields,
    RoverLaunchGameKvFn launch,
    RoverDebriefingFn debriefing
) {
    if (!g_rover_state)
        return false;
    char name_in[kRoverInterfaceTextBytes];
    char password_in[kRoverInterfaceTextBytes];
    char description_in[kRoverInterfaceTextBytes];
    const char* name_text = rover_take_text(name_in, kRoverInterfaceTextBytes, name);
    const char* password_text = rover_take_text(password_in, kRoverInterfaceTextBytes, password);
    const char* description_text =
        rover_take_text(description_in, kRoverInterfaceTextBytes, description);
    arm_launch_fields(g_rover_state->handlers, true, area_id, nullptr, launch, debriefing);
    RoverRecord* record = nullptr;
    RoverEntry* entry =
        request_host_game_basic(&record, area_id, type, name_text, password_text, description_text);
    return send_with_bag(record, entry, extra_fields, -1);
}

bool ROVER_CALL rover_host_game(
    uint32_t area_id,
    int32_t game_type,
    const char* name,
    const char* password,
    const char* description,
    const char* keyword,
    int32_t skill_sel,
    int32_t players_sel,
    int32_t on_death_sel,
    RoverLaunchGameFn launch,
    RoverDebriefingFn debriefing
) {
    if (!g_rover_state)
        return false;
    char name_in[kRoverInterfaceTextBytes];
    char password_in[kRoverInterfaceTextBytes];
    char description_in[kRoverInterfaceTextBytes];
    char keyword_in[kRoverInterfaceTextBytes];
    arm_launch(g_rover_state->handlers, true, area_id, nullptr, launch, debriefing);
    rover_send_to_boneyard(request_host_game(
        area_id,
        game_type,
        rover_take_text(name_in, kRoverInterfaceTextBytes, name),
        rover_take_text(password_in, kRoverInterfaceTextBytes, password),
        rover_take_text(description_in, kRoverInterfaceTextBytes, description),
        rover_take_text(keyword_in, kRoverInterfaceTextBytes, keyword),
        wire_skill(skill_sel),
        wire_players(players_sel),
        wire_on_death(on_death_sel)
    ));
    return true;
}

bool ROVER_CALL rover_join_game_with_fields(
    uint32_t area_id,
    const char* game_id,
    const char* password,
    RoverPropertyBag* extra_fields,
    RoverLaunchGameKvFn launch,
    RoverDebriefingFn debriefing
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char password_in[kRoverInterfaceTextBytes];
    const char* game_text = rover_take_text(game_in, kRoverInterfaceTextBytes, game_id);
    const char* password_text = rover_take_text(password_in, kRoverInterfaceTextBytes, password);
    arm_launch_fields(g_rover_state->handlers, false, area_id, game_text, launch, debriefing);
    RoverRecord* record = nullptr;
    RoverEntry* entry = request_join_game(&record, area_id, game_text, password_text);
    return send_with_bag(record, entry, extra_fields, -1);
}

bool ROVER_CALL rover_join_game(
    uint32_t area_id,
    const char* game_id,
    const char* password,
    RoverLaunchGameFn launch,
    RoverDebriefingFn debriefing
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char password_in[kRoverInterfaceTextBytes];
    const char* game_text = rover_take_text(game_in, kRoverInterfaceTextBytes, game_id);
    const char* password_text = rover_take_text(password_in, kRoverInterfaceTextBytes, password);
    arm_launch(g_rover_state->handlers, false, area_id, game_text, launch, debriefing);
    RoverRecord* record = nullptr;
    request_join_game(&record, area_id, game_text, password_text);
    rover_send_to_boneyard(record);
    return true;
}

bool ROVER_CALL rover_send_left_game(uint32_t area_id, const char* game_id) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    RoverRecord* record = nullptr;
    request_about_game(
        &record,
        kCommandLeftGame,
        area_id,
        rover_take_text(game_in, kRoverInterfaceTextBytes, game_id)
    );
    rover_send_to_boneyard(record);
    return true;
}

bool ROVER_CALL rover_send_game_settings_fields(
    uint32_t area_id, const char* game_id, RoverPropertyBag* extra_fields
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char game_id_text[kWireKeepBytes];
    RoverRecord* record = nullptr;
    RoverEntry* entry = request_about_game(
        &record,
        kCommandGameSettings,
        area_id,
        fit_text(
            game_id_text,
            kWireKeepBytes,
            rover_take_text(game_in, kRoverInterfaceTextBytes, game_id),
            kRoverGameIdLimit
        )
    );
    return send_with_bag(record, entry, extra_fields, kWireReportFieldBytes);
}

bool ROVER_CALL rover_send_game_settings(
    uint32_t area_id,
    const char* game_id,
    int32_t lock_options,
    const char* mission,
    int32_t playerlimit,
    int32_t deathopt,
    int32_t energy,
    int32_t metal,
    int32_t location,
    int32_t los,
    int32_t mapping,
    int32_t maxunits,
    int32_t watching,
    int32_t cheats,
    int32_t tournament
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char mission_in[kRoverInterfaceTextBytes];
    RoverRecord* record = nullptr;
    char game_id_text[kWireKeepBytes];
    char mission_text[kWireKeepBytes];
    const char* mission_kept = fit_text(
        mission_text,
        kWireKeepBytes,
        rover_take_text(mission_in, kRoverInterfaceTextBytes, mission),
        kWireMissionBytes
    );
    RoverEntry* entry = request_about_game(
        &record,
        kCommandGameSettings,
        area_id,
        fit_text(
            game_id_text,
            kWireKeepBytes,
            rover_take_text(game_in, kRoverInterfaceTextBytes, game_id),
            kRoverGameIdLimit
        )
    );
    if (entry) {
        const GameOptionDefaults& omitted = kSettingsOmitted;
        set_option(*entry, kKeyLockOptions, lock_options, omitted.lock_options);
        if (mission_kept && mission_kept[0] != '\0')
            rover_entry_set_string(*entry, kRoverKeyMission, mission_kept);
        set_option(*entry, kKeyPlayerLimit, playerlimit, omitted.playerlimit);
        set_option(*entry, kKeyDeathOption, deathopt, omitted.deathopt);
        set_option(*entry, kRoverKeyEnergy, energy, omitted.energy);
        set_option(*entry, kRoverKeyMetal, metal, omitted.metal);
        set_option(*entry, kKeyLocation, location, omitted.location);
        set_option(*entry, kKeyLineOfSight, los, omitted.los);
        set_option(*entry, kKeyMapping, mapping, omitted.mapping);
        set_option(*entry, kKeyMaxUnits, maxunits, omitted.maxunits);
        set_option(*entry, kKeyWatching, watching, omitted.watching);
        set_option(*entry, kKeyCheats, cheats, omitted.cheats);
        set_option(*entry, kKeyTournament, tournament, omitted.tournament);
    }
    rover_send_to_boneyard(record);
    return true;
}

bool ROVER_CALL rover_send_player_info_fields(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    const char* name,
    RoverPropertyBag* extra_fields
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char name_in[kRoverInterfaceTextBytes];
    RoverRecord* record = nullptr;
    char game_id_text[kWireKeepBytes];
    char name_text[kWireKeepBytes];
    RoverEntry* entry = request_player_info(
        &record,
        area_id,
        fit_text(
            game_id_text,
            kWireKeepBytes,
            rover_take_text(game_in, kRoverInterfaceTextBytes, game_id),
            kRoverGameIdLimit
        ),
        dpid,
        fit_text(
            name_text,
            kWireKeepBytes,
            rover_take_text(name_in, kRoverInterfaceTextBytes, name),
            kWirePlayerNameBytes
        )
    );
    return send_with_bag(record, entry, extra_fields, kWireReportFieldBytes);
}

bool ROVER_CALL rover_send_player_info(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    const char* name,
    const char* allegiance,
    int32_t team
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char name_in[kRoverInterfaceTextBytes];
    char side_in[kRoverInterfaceTextBytes];
    RoverRecord* record = nullptr;
    char game_id_text[kWireKeepBytes];
    char name_text[kWireKeepBytes];
    char side_text[kWireKeepBytes];
    RoverEntry* entry = request_player_info(
        &record,
        area_id,
        fit_text(
            game_id_text,
            kWireKeepBytes,
            rover_take_text(game_in, kRoverInterfaceTextBytes, game_id),
            kRoverGameIdLimit
        ),
        dpid,
        fit_text(
            name_text,
            kWireKeepBytes,
            rover_take_text(name_in, kRoverInterfaceTextBytes, name),
            kWirePlayerNameBytes
        )
    );
    if (entry) {
        set_text(
            *entry,
            kRoverKeyAllegiance,
            fit_text(
                side_text,
                kWireKeepBytes,
                rover_take_text(side_in, kRoverInterfaceTextBytes, allegiance),
                kWireSideBytes
            )
        );
        rover_entry_set_int(*entry, kKeyTeam, team);
    }
    rover_send_to_boneyard(record);
    return true;
}

bool ROVER_CALL rover_send_score_report_props(
    uint32_t area_id, const char* game_id, uint32_t dpid, RoverPropertyBag* props, bool last
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char game_id_text[kWireKeepBytes];
    RoverRecord* record = nullptr;
    RoverEntry* entry = request_score_message(
        &record,
        area_id,
        fit_text(
            game_id_text,
            kWireKeepBytes,
            rover_take_text(game_in, kRoverInterfaceTextBytes, game_id),
            kRoverGameIdLimit
        ),
        dpid,
        nullptr,
        last
    );
    return send_with_bag(record, entry, props, kWireReportFieldBytes);
}

bool ROVER_CALL rover_send_score_report(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    int32_t kills,
    int32_t losses,
    int32_t energy,
    int32_t metal,
    int32_t xs_energy,
    int32_t xs_metal,
    int32_t,
    int32_t,
    int32_t score,
    bool victory,
    bool last
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char game_id_text[kWireKeepBytes];
    RoverRecord* record = nullptr;
    RoverEntry* entry = request_about_game(
        &record,
        kCommandScoreReport,
        area_id,
        fit_text(
            game_id_text,
            kWireKeepBytes,
            rover_take_text(game_in, kRoverInterfaceTextBytes, game_id),
            kRoverGameIdLimit
        )
    );
    if (entry) {
        rover_entry_set_uint(*entry, kRoverKeyDpid, dpid);

        const struct {
            const char* key;
            int32_t value;
        } statistics[] = {
            {kKeyKills, kills},
            {kKeyLosses, losses},
            {kRoverKeyEnergy, energy},
            {kRoverKeyMetal, metal},
            {kKeyExcessEnergy, xs_energy},
            {kKeyExcessMetal, xs_metal},
            {kKeyScore, score},
        };

        for (const auto& statistic : statistics) {
            if (statistic.value >= 0)
                rover_entry_set_int(*entry, statistic.key, statistic.value);
        }
        if (victory)
            rover_entry_set_int(*entry, kKeyVictory, kReportFlag);
        if (last)
            rover_entry_set_int(*entry, kRoverKeyLast, kReportFlag);
    }
    rover_send_to_boneyard(record);
    return true;
}

bool ROVER_CALL rover_send_score_report_message(
    uint32_t area_id, const char* game_id, uint32_t dpid, const char* message
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    char message_in[kRoverInterfaceTextBytes];
    RoverRecord* record = nullptr;
    char game_id_text[kWireKeepBytes];
    char message_text[kWireKeepBytes];
    request_score_message(
        &record,
        area_id,
        fit_text(
            game_id_text,
            kWireKeepBytes,
            rover_take_text(game_in, kRoverInterfaceTextBytes, game_id),
            kRoverGameIdLimit
        ),
        dpid,
        fit_text(
            message_text,
            kWireKeepBytes,
            rover_take_text(message_in, kRoverInterfaceTextBytes, message),
            kWireChatBytes
        ),
        false
    );
    rover_send_to_boneyard(record);
    return true;
}
