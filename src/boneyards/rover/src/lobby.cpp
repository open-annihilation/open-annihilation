// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The galactic war feeds (slots 38 and 39: census, activity and allegiances),
// pages (slots 41 and 42), the locators (slots 65 and 66), the helpers the
// lobby slots share and the offer of an entry to the lobby handlers.

#include "lobby.h"

#include "bags.h"
#include "lobby_handlers.h"
#include "wire_text.h"
#include "protocol.h"
#include "requests.h"
#include "slots.h"
#include "state.h"

#include <string.h>

// Seats and the room global flag travel beside the fixed slot callbacks.
// A note is pending until the callback's client takes it, or until the
// handler drops it because nothing did.
struct GameSeatNote {
    bool pending{};
    bool known{};
    int32_t players{};
    int32_t playerlimit{};
};

static GameSeatNote g_game_seats{};

struct RoomGlobalNote {
    bool pending{};
    bool known{};
    int32_t global{};
};

static RoomGlobalNote g_room_global{};

namespace {

constexpr char kCommandGetCensus[] = "get_census";
constexpr char kCommandGetAllegiance[] = "get_allegiance";
constexpr char kCommandPage[] = "page";
constexpr char kCommandCensus[] = "census";
constexpr char kCommandAllegiances[] = "allegiances";
constexpr char kCommandActivity[] = "activity";
constexpr char kCommandLocate[] = "locate";
constexpr char kCommandUserLocated[] = "user_located";
constexpr char kCommandGameLocated[] = "game_located";

constexpr char kKeyPopulation[] = "pop";
constexpr char kKeyActivity[] = "activity";
constexpr char kKeyInProgress[] = "in_progress";
constexpr char kKeyArmWins[] = "arm_wins";
constexpr char kKeyCoreWins[] = "core_wins";
constexpr char kKeyLocCode[] = "loc_code";
constexpr char kKeyUserIcon[] = "user_icon";
constexpr char kKeyPassReq[] = "pass_req";
constexpr char kKeyPlayers[] = "players";
constexpr char kKeyPlayerLimit[] = "playerlimit";

constexpr char kChangeAdd[] = "add";
constexpr char kChangeDelete[] = "delete";

// A share count of 0xfe or more makes an assignment copy a string's text
// instead of sharing its buffer.
constexpr uint8_t kStringCopyOnly = 0xfe;

// Room for one key or value of a record with its share count and terminator.
constexpr int32_t kStagedStringCapacity = kRoverRecordTextLimit + 2;

/// Builds get_census for the war manager.
///
/// @param area_id area to count; 0 asks for every area and sends no area_id
/// @return a new request record, or null when it could not be built
RoverRecord* request_census(uint32_t area_id) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::WarManager, kCommandGetCensus);
    if (entry && area_id != 0)
        rover_entry_set_int(*entry, kRoverKeyAreaId, static_cast<int32_t>(area_id));
    return record;
}

/// Builds get_allegiance for the war manager.
///
/// @return a new request record, or null when it could not be built
RoverRecord* request_allegiance() {
    RoverRecord* record = nullptr;
    rover_add_request(&record, RoverService::WarManager, kCommandGetAllegiance);
    return record;
}

/// Builds a page to a user by name for the user manager.
///
/// @param user_name user to page; sent only when non-null and non-empty
/// @param message page text; sent only when non-null and non-empty
/// @return a new request record, or null when it could not be built
RoverRecord* request_page_by_name(const char* user_name, const char* message) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::UserManager, kCommandPage);
    if (!entry)
        return record;
    if (user_name && user_name[0] != '\0')
        rover_entry_set_string(*entry, kRoverKeyName, user_name);
    if (message && message[0] != '\0')
        rover_entry_set_string(*entry, kRoverKeyMessage, message);
    return record;
}

/// Builds a page to a user by id for the user manager.
///
/// @param user_id user to page
/// @param message page text
/// @return a new request record, or null when it could not be built
RoverRecord* request_page_by_id(uint32_t user_id, const char* message) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::UserManager, kCommandPage);
    if (entry) {
        rover_entry_set_uint(*entry, kRoverKeyUserId, user_id);
        rover_entry_set_string(*entry, kRoverKeyMessage, message);
    }
    return record;
}

/// Builds locate for the user manager.
///
/// @param user_name user to locate; sent only when non-null and non-empty
/// @return a new request record, or null when it could not be built
RoverRecord* request_locate_user(const char* user_name) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::UserManager, kCommandLocate);
    if (entry && user_name && user_name[0] != '\0')
        rover_entry_set_string(*entry, kRoverKeyName, user_name);
    return record;
}

/// Builds locate for the game manager.
///
/// @param name game to locate; sent only when non-null and non-empty
/// @return a new request record, or null when it could not be built
RoverRecord* request_locate_game(const char* name) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::GameManager, kCommandLocate);
    if (entry && name && name[0] != '\0')
        rover_entry_set_string(*entry, kRoverKeyName, name);
    return record;
}

/// Takes census and hands the game one area's population, activity, games in progress and forming,
/// and war score.
///
/// Every field must be present or the message is taken but not reported. The handler stays armed.
///
/// @param handlers handler set holding the census callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is census, false otherwise
bool accept_census(RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry) {
    if (!rover_command_is(command, kCommandCensus))
        return false;
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* population = rover_entry_find(entry, kKeyPopulation);
    const RoverField* activity = rover_entry_find(entry, kKeyActivity);
    const RoverField* in_progress = rover_entry_find(entry, kKeyInProgress);
    const RoverField* forming = rover_entry_find(entry, kRoverKeyForming);
    const RoverField* arm_wins = rover_entry_find(entry, kKeyArmWins);
    const RoverField* core_wins = rover_entry_find(entry, kKeyCoreWins);
    RoverCensusFn census = handlers.lobby.census;
    if (area && population && activity && in_progress && forming && arm_wins && core_wins &&
        census) {
        census(
            rover_field_uint(*area, 0),
            rover_field_int(*population, -1),
            rover_field_int(*activity, -1),
            rover_field_int(*in_progress, -1),
            rover_field_int(*forming, -1),
            rover_field_int(*core_wins, -1),
            rover_field_int(*arm_wins, -1)
        );
    }
    return true;
}

// The allegiances and activity handlers pass the text of "array" when it is
// not empty; they stay armed.
bool accept_array(
    const RoverField& command,
    const char* name,
    const RoverEntry& entry,
    void(ROVER_CALL* callback)(const char*)
) {
    if (!rover_command_is(command, name))
        return false;
    const RoverField* array = rover_entry_find(entry, kRoverKeyArray);
    if (array && array->value_length != 0 && callback)
        callback(rover_show(rover_field_text(*array)));
    return true;
}

/// Takes allegiances and hands the game one status letter per planet of the galaxy, in map order.
///
/// An empty or missing array is not reported. The handler stays armed.
///
/// @param handlers handler set holding the allegiances callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is allegiances, false otherwise
bool accept_allegiances(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    return accept_array(command, kCommandAllegiances, entry, handlers.lobby.allegiances);
}

/// Takes activity and hands the game one activity digit per area.
///
/// An empty or missing array is not reported. The handler stays armed.
///
/// @param handlers handler set holding the activity callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is activity, false otherwise
bool accept_activity(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    return accept_array(command, kCommandActivity, entry, handlers.lobby.activity);
}

// The case-blind compare folds only ASCII letters.
char fold_case(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// True when the texts agree, whatever their case, up to their ends or over
// the first limit characters.
bool same_name(const char* a, const char* b, int32_t limit) {
    for (int32_t i = 0; i < limit; ++i) {
        if (fold_case(a[i]) != fold_case(b[i]))
            return false;
        if (a[i] == '\0')
            break;
    }
    return true;
}

/// Takes user_located, the locator's answer about the name slot 65 asked for, and hands it to the
/// game.
///
/// The name is matched on its first 15 characters whatever their case. Every field but loc_code,
/// user_id, name and user_icon, command included, reaches the game in a bag, and the handler
/// disarms. An answer missing one of those four is taken and ignored.
///
/// @param[in,out] handlers handler set holding the locate continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is user_located and not about another name, false otherwise
bool accept_user_located(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandUserLocated))
        return false;
    const RoverField* loc_code = rover_entry_find(entry, kKeyLocCode);
    const RoverField* user_id = rover_entry_find(entry, kRoverKeyUserId);
    const RoverField* name = rover_entry_find(entry, kRoverKeyName);
    const RoverField* user_icon = rover_entry_find(entry, kKeyUserIcon);
    if (!loc_code || !user_id || !name || !user_icon)
        return true;
    RoverUserLocatedHandler& located = handlers.lobby.user_located;
    if (!same_name(located.name, rover_field_text(*name), kRoverLocateNameLimit))
        return false;
    static const char* const kPassedAlone[] = {
        kKeyLocCode, kRoverKeyUserId, kRoverKeyName, kKeyUserIcon
    };
    constexpr int32_t kPassedAloneCount =
        static_cast<int32_t>(sizeof kPassedAlone / sizeof kPassedAlone[0]);
    RoverPropertyBag* fields =
        rover_collect_field_bag(entry, kPassedAlone, kPassedAloneCount, RoverBagCommand::Kept);
    int32_t code = rover_field_int(*loc_code, -1);
    int32_t icon = rover_field_int(*user_icon, -1);
    if (located.callback)
        located.callback(
            code, rover_field_uint(*user_id, 0), rover_show(rover_field_text(*name)), icon, fields
        );
    rover_release_field_bag(fields);
    rover_handler_disarm(handlers, RoverHandlerId::UserLocated);
    located.callback = nullptr;
    located.name[0] = '\0';
    return true;
}

/// Takes game_located, an answer to the game slot 66 asked the game manager to find, and hands it
/// to the game.
///
/// area_id reads 0 when absent or not a U field. type, forming, pass_req and last read 0 when
/// absent and -1 when present but not an I field. game_id is null when absent and name is "" when
/// absent. forming must be 0 or 1; any other value, including -1, is taken and not reported, and
/// does not look at last. pass_req other than zero, -1 included, is reported as 1. A last other
/// than zero disarms the handler after the callback. The handler stays armed otherwise, so several
/// answers can arrive.
///
/// @param[in,out] handlers handler set holding the game-locate continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is game_located, false otherwise
bool accept_game_located(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandGameLocated))
        return false;
    int32_t forming = 0;
    rover_entry_get_int(entry, kRoverKeyForming, &forming, 0);
    if (forming != 0 && forming != 1)
        return true;
    uint32_t area_id = 0;
    const char* game_id = nullptr;
    const char* name = kRoverNoText;
    int32_t type = 0;
    int32_t last = 0;
    int32_t pass_req = 0;
    rover_entry_get_uint(entry, kRoverKeyAreaId, &area_id, 0);
    rover_entry_get_string(entry, kRoverKeyGameId, &game_id, nullptr);
    rover_entry_get_string(entry, kRoverKeyName, &name, kRoverNoText);
    rover_entry_get_int(entry, kRoverKeyType, &type, 0);
    rover_entry_get_int(entry, kRoverKeyLast, &last, 0);
    rover_entry_get_int(entry, kKeyPassReq, &pass_req, 0);
    int32_t players = 0;
    int32_t player_limit = 0;
    const bool players_known = rover_entry_get_int(entry, kKeyPlayers, &players, 0);
    const bool limit_known = rover_entry_get_int(entry, kKeyPlayerLimit, &player_limit, 0);
    rover_note_game_seats(players_known && limit_known, players, player_limit);
    RoverGameLocatedHandler& located = handlers.lobby.game_located;
    if (located.callback) {
        located.callback(
            area_id,
            rover_show(game_id),
            rover_show(name),
            type,
            forming,
            pass_req != 0 ? 1 : 0,
            last
        );
    }
    int32_t dropped_players = 0;
    int32_t dropped_limit = 0;
    (void)rover_take_game_seats(&dropped_players, &dropped_limit);
    if (last != 0) {
        rover_handler_disarm(handlers, RoverHandlerId::GameLocated);
        located.callback = nullptr;
    }
    return true;
}

// A copy of a key or value in RoverString's layout,
// marked so that a property bag copies rather than shares it.
struct StagedString {
    char buffer[kStagedStringCapacity]{};
    RoverString string{};
};

void stage(StagedString& staged, const char* text, int32_t length) {
    if (length > kRoverRecordTextLimit)
        length = kRoverRecordTextLimit;
    staged.buffer[0] = static_cast<char>(kStringCopyOnly);
    memcpy(staged.buffer + 1, text, static_cast<size_t>(length));
    staged.buffer[length + 1] = '\0';
    staged.string.data = length != 0 ? staged.buffer + 1 : nullptr;
    staged.string.length = static_cast<uint32_t>(length);
    staged.string.capacity = static_cast<uint32_t>(length);
}

bool is_skipped(
    const RoverField& field,
    const char* const* skipped,
    int32_t skipped_count,
    RoverBagCommand command
) {
    if (command == RoverBagCommand::Skipped &&
        rover_text_equals(field.key, field.key_length, kRoverKeyCommand))
        return true;
    for (int32_t i = 0; i < skipped_count; ++i) {
        if (rover_text_equals(field.key, field.key_length, skipped[i]))
            return true;
    }
    return false;
}

} // namespace

void rover_send_to_boneyard(RoverRecord* record) {
    if (!record || !g_rover_state)
        return;
    RoverState& state = *g_rover_state;
    rover_queue_push(state.outbound[static_cast<int32_t>(rover_active_boneyard(state))], record);
}

void rover_handler_rearm(RoverHandlerSet& handlers, RoverHandlerId id) {
    if (rover_handler_is_armed(handlers, id))
        rover_handler_disarm(handlers, id);
    rover_handler_arm(handlers, id);
}

bool rover_command_is(const RoverField& command, const char* name) {
    return rover_text_equals(command.value, command.value_length, name);
}

const char* rover_field_text(const RoverField& field) {
    return field.value ? field.value : kRoverNoText;
}

bool rover_read_change(const RoverEntry& entry, RoverChange* out_change) {
    *out_change = RoverChange::Add;
    const RoverField* change = rover_entry_find(entry, kRoverKeyChange);
    if (!change)
        return true;
    if (rover_text_equals(change->value, change->value_length, kChangeAdd))
        return true;
    if (!rover_text_equals(change->value, change->value_length, kChangeDelete))
        return false;
    *out_change = RoverChange::Delete;
    return true;
}

bool rover_game_id_matches(const RoverField& game_id, const char* stored) {
    return rover_text_equals(game_id.value, game_id.value_length, stored);
}

void rover_store_game_id(char* stored, const char* game_id) {
    rover_copy_bounded(stored, game_id ? game_id : kRoverNoText, kRoverGameIdLimit + 1);
}

void rover_fill_field_bag(
    RoverPropertyBag* bag,
    const RoverEntry& entry,
    const char* const* skipped,
    int32_t skipped_count,
    RoverBagCommand command
) {
    if (!bag)
        return;
    StagedString key;
    StagedString value;
    for (const RoverField* field = entry.first_field; field; field = rover_field_next(*field)) {
        if (is_skipped(*field, skipped, skipped_count, command))
            continue;
        stage(key, rover_field_key(*field), field->key_length);
        switch (rover_field_type(*field)) {
        case RoverFieldType::Int:
            rover_set_field_int(bag, &key.string, rover_field_int(*field, -1));
            break;
        case RoverFieldType::Uint:
            rover_set_field_uint(bag, &key.string, rover_field_uint(*field, 0));
            break;
        case RoverFieldType::String:
            stage(value, rover_field_text(*field), field->value_length);
            rover_bag_store_string(bag, &key.string, &value.string);
            break;
        }
    }
}

RoverPropertyBag* rover_collect_field_bag(
    const RoverEntry& entry,
    const char* const* skipped,
    int32_t skipped_count,
    RoverBagCommand command
) {
    RoverPropertyBag* bag = rover_create_property_bag();
    rover_fill_field_bag(bag, entry, skipped, skipped_count, command);
    return bag;
}

void rover_release_field_bag(RoverPropertyBag* bag) {
    if (bag)
        rover_destroy_property_bag(bag);
}

bool rover_offer_lobby_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
) {
    switch (id) {
    case RoverHandlerId::HttpRequestComplete:
        return rover_accept_http_complete(handlers, command, entry);
    case RoverHandlerId::Census:
        return accept_census(handlers, command, entry);
    case RoverHandlerId::Allegiances:
        return accept_allegiances(handlers, command, entry);
    case RoverHandlerId::Activity:
        return accept_activity(handlers, command, entry);
    case RoverHandlerId::UserLocated:
        return accept_user_located(handlers, command, entry);
    case RoverHandlerId::GameLocated:
        return accept_game_located(handlers, command, entry);
    default:
        break;
    }
    if (id == RoverHandlerId::ProfileInfo || id == RoverHandlerId::ProfileUpdated)
        return rover_offer_profile_message(handlers, id, command, entry);
    if ((id >= RoverHandlerId::RoomListFinal && id <= RoverHandlerId::ChatMessage) ||
        id == RoverHandlerId::RoomCreated)
        return rover_offer_room_message(handlers, id, command, entry);
    if (id >= RoverHandlerId::BattlesEntered && id <= RoverHandlerId::BattlesLeft)
        return rover_offer_battle_message(handlers, id, command, entry);
    if (id >= RoverHandlerId::LaunchGameFields && id <= RoverHandlerId::Debriefing)
        return rover_offer_game_message(handlers, id, command, entry);
    return false;
}

void ROVER_CALL
rover_request_census(uint32_t area_id, RoverCensusFn census, RoverActivityFn activity) {
    if (!g_rover_state)
        return;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    if (rover_handler_is_armed(handlers, RoverHandlerId::Census) && lobby.census != census)
        rover_handler_disarm(handlers, RoverHandlerId::Census);
    if (!rover_handler_is_armed(handlers, RoverHandlerId::Census)) {
        lobby.census = census;
        rover_handler_arm(handlers, RoverHandlerId::Census);
    }
    if (rover_handler_is_armed(handlers, RoverHandlerId::Activity) && lobby.activity != activity)
        rover_handler_disarm(handlers, RoverHandlerId::Activity);
    if (!rover_handler_is_armed(handlers, RoverHandlerId::Activity)) {
        lobby.activity = activity;
        rover_handler_arm(handlers, RoverHandlerId::Activity);
    }
    rover_send_to_boneyard(request_census(area_id));
}

void ROVER_CALL rover_request_allegiance(RoverAllegianceFn allegiances) {
    if (!g_rover_state)
        return;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    handlers.lobby.allegiances = allegiances;
    rover_handler_arm(handlers, RoverHandlerId::Allegiances);
    rover_send_to_boneyard(request_allegiance());
}

void ROVER_CALL rover_send_page_by_name(const char* user_name, const char* message) {
    if (!g_rover_state)
        return;
    char name_in[kRoverInterfaceTextBytes];
    char message_in[kRoverInterfaceTextBytes];
    rover_send_to_boneyard(request_page_by_name(
        rover_take_text(name_in, kRoverInterfaceTextBytes, user_name),
        rover_take_text(message_in, kRoverInterfaceTextBytes, message)
    ));
}

void ROVER_CALL rover_send_page_by_id(int32_t user_id, const char* message) {
    if (!g_rover_state)
        return;
    char message_in[kRoverInterfaceTextBytes];
    rover_send_to_boneyard(request_page_by_id(
        static_cast<uint32_t>(user_id),
        rover_take_text(message_in, kRoverInterfaceTextBytes, message)
    ));
}

void ROVER_CALL rover_locate_player(const char* username, RoverLocateFn located) {
    if (!g_rover_state)
        return;
    char name_in[kRoverInterfaceTextBytes];
    const char* name_text = rover_take_text(name_in, kRoverInterfaceTextBytes, username);
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverUserLocatedHandler& pending = handlers.lobby.user_located;
    pending.callback = located;
    rover_copy_bounded(
        pending.name, name_text ? name_text : kRoverNoText, kRoverLocateNameLimit + 1
    );
    rover_handler_rearm(handlers, RoverHandlerId::UserLocated);
    rover_send_to_boneyard(request_locate_user(name_text));
}

void rover_note_game_seats(bool known, int32_t players, int32_t playerlimit) {
    g_game_seats.pending = true;
    g_game_seats.known = known;
    g_game_seats.players = players;
    g_game_seats.playerlimit = playerlimit;
}

bool rover_take_game_seats(int32_t* players, int32_t* playerlimit) {
    if (!g_game_seats.pending)
        return false;
    g_game_seats.pending = false;
    if (!g_game_seats.known)
        return false;
    if (players != nullptr)
        *players = g_game_seats.players;
    if (playerlimit != nullptr)
        *playerlimit = g_game_seats.playerlimit;
    return true;
}

void rover_note_room_global(bool known, int32_t global) {
    g_room_global.pending = true;
    g_room_global.known = known;
    g_room_global.global = global;
}

bool rover_take_room_global(int32_t* global) {
    if (!g_room_global.pending)
        return false;
    g_room_global.pending = false;
    if (!g_room_global.known)
        return false;
    if (global != nullptr)
        *global = g_room_global.global;
    return true;
}

void ROVER_CALL rover_locate_target(const char* name, RoverGameLocatedFn located) {
    if (!g_rover_state)
        return;
    char name_in[kRoverInterfaceTextBytes];
    RoverHandlerSet& handlers = g_rover_state->handlers;
    handlers.lobby.game_located.callback = located;
    rover_handler_rearm(handlers, RoverHandlerId::GameLocated);
    rover_send_to_boneyard(
        request_locate_game(rover_take_text(name_in, kRoverInterfaceTextBytes, name))
    );
}

// Matchmaking is not an interface slot. The dialog's look, join, cancel and answer are built here
// and queued the same way as host_game. The bracket is not a field: the server reads it itself
// (decision 6). Replies are offered from dispatch, before the armed handlers.
namespace {

constexpr char kCommandMatchmakingLook[] = "matchmaking_look";
constexpr char kCommandMatchmakingJoin[] = "matchmaking_join";
constexpr char kCommandMatchmakingCancel[] = "matchmaking_cancel";
constexpr char kCommandMatchmakingAnswer[] = "matchmaking_answer";
constexpr char kCommandMatchmakingStatus[] = "matchmaking_status";
constexpr char kCommandMatchmakingFound[] = "matchmaking_found";
constexpr char kCommandMatchmakingGo[] = "matchmaking_go";

constexpr char kKeyGameType[] = "game_type";
constexpr char kKeySkillMode[] = "skill_mode";
constexpr char kKeySide[] = "side";
constexpr char kKeyPlanets[] = "planets";
constexpr char kKeyOfferId[] = "offer_id";
constexpr char kKeyAccept[] = "accept";
constexpr char kKeyResult[] = "result";
constexpr char kKeyQueued[] = "queued";
constexpr char kKeyCount[] = "count";
constexpr char kKeyMeter[] = "meter";
constexpr char kKeySkill[] = "skill";
constexpr char kKeyRole[] = "role";
constexpr char kKeySeats[] = "seats";
constexpr char kKeyOnDeath[] = "on_death";

RoverMatchmakingStatusFn g_match_status = nullptr;
RoverMatchmakingFoundFn g_match_found = nullptr;
RoverMatchmakingGoFn g_match_go = nullptr;

void copy_match_text(char* destination, int32_t capacity, const char* source) {
    if (destination == nullptr || capacity <= 0)
        return;
    destination[0] = '\0';
    rover_copy_bounded(destination, source, capacity);
}

void copy_match_field(
    char* destination, int32_t capacity, const RoverEntry& entry, const char* key, bool* known
) {
    copy_match_text(destination, capacity, nullptr);
    const char* text = nullptr;
    if (!rover_entry_get_string(entry, key, &text, nullptr) || text == nullptr || text[0] == '\0') {
        if (known != nullptr)
            *known = false;
        return;
    }
    rover_copy_bounded(destination, text, capacity);
    if (known != nullptr)
        *known = destination[0] != '\0';
}

bool begin_match_request(const char* command, RoverRecord** record, RoverEntry** entry) {
    if (!g_rover_state || record == nullptr || entry == nullptr)
        return false;
    *record = nullptr;
    *entry = rover_add_request(record, RoverService::GameManager, command);
    if (*entry == nullptr) {
        rover_record_destroy(record);
        return false;
    }
    return true;
}

void write_match_filters(RoverEntry& entry, const RoverMatchmakingQuery& query) {
    rover_entry_set_int(entry, kKeyGameType, query.game_type);
    rover_entry_set_int(entry, kKeySkillMode, query.skill_mode);
    rover_entry_set_int(entry, kKeySide, query.side);
    rover_entry_set_int(entry, kKeyPlanets, query.planets);
    // Any planet, and a this-planet search with no known area, omit area_id.
    if (query.area_known && query.planets == 0)
        rover_entry_set_uint(entry, kRoverKeyAreaId, query.area_id);
}

bool send_match_filters(const char* command, const RoverMatchmakingQuery* query) {
    if (query == nullptr)
        return false;
    RoverRecord* record = nullptr;
    RoverEntry* entry = nullptr;
    if (!begin_match_request(command, &record, &entry))
        return false;
    write_match_filters(*entry, *query);
    rover_send_to_boneyard(record);
    return true;
}

} // namespace

void rover_matchmaking_set_handlers(
    RoverMatchmakingStatusFn status, RoverMatchmakingFoundFn found, RoverMatchmakingGoFn go
) {
    g_match_status = status;
    g_match_found = found;
    g_match_go = go;
}

bool rover_matchmaking_look(const RoverMatchmakingQuery* query) {
    return send_match_filters(kCommandMatchmakingLook, query);
}

bool rover_matchmaking_join(const RoverMatchmakingQuery* query) {
    return send_match_filters(kCommandMatchmakingJoin, query);
}

bool rover_matchmaking_cancel() {
    RoverRecord* record = nullptr;
    RoverEntry* entry = nullptr;
    if (!begin_match_request(kCommandMatchmakingCancel, &record, &entry))
        return false;
    rover_send_to_boneyard(record);
    return true;
}

bool rover_matchmaking_answer(const char* offer_id, int32_t accept) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = nullptr;
    if (!begin_match_request(kCommandMatchmakingAnswer, &record, &entry))
        return false;
    rover_entry_set_int(*entry, kKeyAccept, accept);
    char id_in[kRoverMatchmakingIdBytes];
    id_in[0] = '\0';
    const char* id_text = rover_take_text(id_in, kRoverMatchmakingIdBytes, offer_id);
    if (id_text != nullptr && id_text[0] != '\0')
        rover_entry_set_string(*entry, kKeyOfferId, id_text);
    rover_send_to_boneyard(record);
    return true;
}

bool rover_matchmaking_offer(const RoverField& command, const RoverEntry& entry) {
    if (rover_command_is(command, kCommandMatchmakingStatus)) {
        RoverMatchmakingStatus status{};
        rover_entry_get_int(entry, kKeyResult, &status.result, 0);
        rover_entry_get_int(entry, kKeyQueued, &status.queued, 0);
        rover_entry_get_int(entry, kKeyCount, &status.count, 0);
        rover_entry_get_int(entry, kKeyMeter, &status.meter, 0);
        if (g_match_status != nullptr)
            g_match_status(&status);
        return true;
    }
    if (rover_command_is(command, kCommandMatchmakingFound)) {
        RoverMatchmakingFound found{};
        copy_match_field(found.offer_id, kRoverMatchmakingIdBytes, entry, kKeyOfferId, nullptr);
        copy_match_field(found.name, kRoverMatchmakingNameBytes, entry, kRoverKeyName, nullptr);
        rover_entry_get_int(entry, kKeySkill, &found.skill, 0);
        rover_entry_get_int(entry, kKeyGameType, &found.game_type, 0);
        rover_entry_get_int(entry, kKeyRole, &found.role, 0);
        rover_entry_get_int(entry, kKeySide, &found.side, 0);
        rover_entry_get_uint(entry, kRoverKeyAreaId, &found.area_id, 0);
        if (g_match_found != nullptr)
            g_match_found(&found);
        return true;
    }
    if (rover_command_is(command, kCommandMatchmakingGo)) {
        RoverMatchmakingGo go{};
        copy_match_field(go.offer_id, kRoverMatchmakingIdBytes, entry, kKeyOfferId, nullptr);
        rover_entry_get_int(entry, kKeyResult, &go.result, 0);
        rover_entry_get_int(entry, kKeyRole, &go.role, 0);
        rover_entry_get_uint(entry, kRoverKeyAreaId, &go.area_id, 0);
        copy_match_field(
            go.game_id, kRoverMatchmakingIdBytes, entry, kRoverKeyGameId, &go.game_known
        );
        copy_match_field(go.name, kRoverMatchmakingNameBytes, entry, kRoverKeyName, &go.name_known);
        rover_entry_get_int(entry, kKeyGameType, &go.game_type, 0);
        rover_entry_get_int(entry, kKeySkill, &go.skill, 0);
        rover_entry_get_int(entry, kKeySeats, &go.seats, 0);
        rover_entry_get_int(entry, kKeyOnDeath, &go.on_death, 0);
        go.side_known = rover_entry_get_int(entry, kKeySide, &go.side, 0);
        if (g_match_go != nullptr)
            g_match_go(&go);
        return true;
    }
    return false;
}

// QuickPlay shares the request builder with matchmaking. Scan lines carry kind, names and numbers.
// The client writes the sentence; nothing here copies a finished line off the entry.
namespace {

constexpr char kCommandQuickPlayJoin[] = "quickplay_join";
constexpr char kCommandQuickPlayCancel[] = "quickplay_cancel";
constexpr char kCommandQuickPlayAnswer[] = "quickplay_answer";
constexpr char kCommandQuickPlayStatus[] = "quickplay_status";
constexpr char kCommandQuickPlayScan[] = "quickplay_scan";
constexpr char kCommandQuickPlayFound[] = "quickplay_found";
constexpr char kCommandQuickPlayWithdrawn[] = "quickplay_withdrawn";

constexpr char kKeyWhere[] = "where";
constexpr char kKeyPreferFavorite[] = "prefer_favorite";
constexpr char kKeyFavorite[] = "favorite";
constexpr char kKeyFits[] = "fits";
constexpr char kKeySeq[] = "seq";
constexpr char kKeyKind[] = "kind";
constexpr char kKeyPlanet[] = "planet";
constexpr char kKeyBattle[] = "battle";
constexpr char kKeyReason[] = "reason";
constexpr char kKeySeatsMax[] = "seats_max";
constexpr char kKeyMap[] = "map";

RoverQuickPlayStatusFn g_quick_status = nullptr;
RoverQuickPlayScanFn g_quick_scan = nullptr;
RoverQuickPlayFoundFn g_quick_found = nullptr;
RoverQuickPlayWithdrawnFn g_quick_withdrawn = nullptr;

} // namespace

void rover_quickplay_set_handlers(
    RoverQuickPlayStatusFn status,
    RoverQuickPlayScanFn scan,
    RoverQuickPlayFoundFn found,
    RoverQuickPlayWithdrawnFn withdrawn
) {
    g_quick_status = status;
    g_quick_scan = scan;
    g_quick_found = found;
    g_quick_withdrawn = withdrawn;
}

bool rover_quickplay_join(const RoverQuickPlayQuery* query) {
    if (query == nullptr)
        return false;
    RoverRecord* record = nullptr;
    RoverEntry* entry = nullptr;
    if (!begin_match_request(kCommandQuickPlayJoin, &record, &entry))
        return false;
    rover_entry_set_int(*entry, kKeyGameType, query->game_type);
    rover_entry_set_int(*entry, kKeyWhere, query->where);
    rover_entry_set_int(*entry, kKeySkill, query->skill);
    rover_entry_set_int(*entry, kKeySide, query->side);
    rover_entry_set_int(*entry, kKeyPreferFavorite, query->prefer_favorite);
    if (query->prefer_favorite != 0 && query->favorite[0] != '\0')
        rover_entry_set_string(*entry, kKeyFavorite, query->favorite);
    if (query->area_known)
        rover_entry_set_uint(*entry, kRoverKeyAreaId, query->area_id);
    rover_send_to_boneyard(record);
    return true;
}

bool rover_quickplay_cancel() {
    RoverRecord* record = nullptr;
    RoverEntry* entry = nullptr;
    if (!begin_match_request(kCommandQuickPlayCancel, &record, &entry))
        return false;
    rover_send_to_boneyard(record);
    return true;
}

bool rover_quickplay_answer(const char* offer_id, int32_t accept) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = nullptr;
    if (!begin_match_request(kCommandQuickPlayAnswer, &record, &entry))
        return false;
    rover_entry_set_int(*entry, kKeyAccept, accept);
    char id_in[kRoverMatchmakingIdBytes];
    id_in[0] = '\0';
    const char* id_text = rover_take_text(id_in, kRoverMatchmakingIdBytes, offer_id);
    if (id_text != nullptr && id_text[0] != '\0')
        rover_entry_set_string(*entry, kKeyOfferId, id_text);
    rover_send_to_boneyard(record);
    return true;
}

bool rover_quickplay_offer(const RoverField& command, const RoverEntry& entry) {
    if (rover_command_is(command, kCommandQuickPlayStatus)) {
        RoverQuickPlayStatus status{};
        rover_entry_get_int(entry, kKeyResult, &status.result, 0);
        rover_entry_get_int(entry, kKeyQueued, &status.queued, 0);
        rover_entry_get_int(entry, kKeyMeter, &status.meter, 0);
        rover_entry_get_int(entry, kKeyFits, &status.fits, 0);
        rover_entry_get_int(entry, kKeyPlanets, &status.planets, 0);
        if (g_quick_status != nullptr)
            g_quick_status(&status);
        return true;
    }
    if (rover_command_is(command, kCommandQuickPlayScan)) {
        RoverQuickPlayScan scan{};
        rover_entry_get_int(entry, kKeySeq, &scan.seq, 0);
        rover_entry_get_int(entry, kKeyKind, &scan.kind, 0);
        rover_entry_get_int(entry, kKeyReason, &scan.reason, 0);
        rover_entry_get_int(entry, kKeyGameType, &scan.game_type, 0);
        rover_entry_get_int(entry, kKeySkill, &scan.skill, 0);
        rover_entry_get_int(entry, kKeySeats, &scan.seats, 0);
        rover_entry_get_int(entry, kKeySeatsMax, &scan.seats_max, 0);
        copy_match_field(scan.planet, kRoverQuickPlayPlanetBytes, entry, kKeyPlanet, nullptr);
        copy_match_field(scan.battle, kRoverQuickPlayBattleBytes, entry, kKeyBattle, nullptr);
        if (g_quick_scan != nullptr)
            g_quick_scan(&scan);
        return true;
    }
    if (rover_command_is(command, kCommandQuickPlayFound)) {
        RoverQuickPlayFound found{};
        copy_match_field(found.offer_id, kRoverMatchmakingIdBytes, entry, kKeyOfferId, nullptr);
        rover_entry_get_int(entry, kKeyRole, &found.role, 0);
        rover_entry_get_uint(entry, kRoverKeyAreaId, &found.area_id, 0);
        copy_match_field(
            found.game_id, kRoverMatchmakingIdBytes, entry, kRoverKeyGameId, &found.game_known
        );
        copy_match_field(found.name, kRoverQuickPlayBattleBytes, entry, kRoverKeyName, nullptr);
        copy_match_field(found.planet, kRoverQuickPlayPlanetBytes, entry, kKeyPlanet, nullptr);
        rover_entry_get_int(entry, kKeyGameType, &found.game_type, 0);
        rover_entry_get_int(entry, kKeySkill, &found.skill, 0);
        rover_entry_get_int(entry, kKeySeats, &found.seats, 0);
        rover_entry_get_int(entry, kKeyOnDeath, &found.on_death, 0);
        copy_match_field(found.map, kRoverQuickPlayMapBytes, entry, kKeyMap, &found.map_known);
        if (g_quick_found != nullptr)
            g_quick_found(&found);
        return true;
    }
    if (rover_command_is(command, kCommandQuickPlayWithdrawn)) {
        RoverQuickPlayWithdrawn withdrawn{};
        copy_match_field(withdrawn.offer_id, kRoverMatchmakingIdBytes, entry, kKeyOfferId, nullptr);
        copy_match_field(
            withdrawn.game_id,
            kRoverMatchmakingIdBytes,
            entry,
            kRoverKeyGameId,
            &withdrawn.game_known
        );
        rover_entry_get_int(entry, kKeyReason, &withdrawn.reason, 0);
        if (g_quick_withdrawn != nullptr)
            g_quick_withdrawn(&withdrawn);
        return true;
    }
    return false;
}
