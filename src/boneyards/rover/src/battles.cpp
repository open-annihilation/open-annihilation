// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Battle lists (slots 14-19): subscribing to an area's battles, leaving it,
// and asking for one battle's intel and reports; with the handlers of their
// replies (22-30). Requests go to the game manager. The key/value variants
// (slots 14, 16 and 18) hand the game every other field in a property bag.

#include "lobby.h"
#include "lobby_handlers.h"
#include "protocol.h"
#include "requests.h"
#include "slots.h"
#include "state.h"
#include "wire_text.h"

namespace {

constexpr char kCommandEnterBattles[] = "enter_battles";
constexpr char kCommandLeaveBattles[] = "leave_battles";
constexpr char kCommandGetBattle[] = "get_battle";
constexpr char kCommandBattlesEntered[] = "battles_entered";
constexpr char kCommandBattlesLeft[] = "battles_left";
constexpr char kCommandBattleInfo[] = "battle_info";
constexpr char kCommandIntelMessage[] = "intel_msg";
constexpr char kCommandBattleIntel[] = "battle_intel";
constexpr char kCommandBattleReport[] = "battle_report";

constexpr char kKeyHost[] = "host";
constexpr char kKeySkill[] = "skill";
constexpr char kKeyPlayers[] = "players";
constexpr char kKeyOnDeath[] = "on_death";
constexpr char kKeyKills[] = "kills";
constexpr char kKeyLosses[] = "losses";
constexpr char kKeyScore[] = "score";

// battle_info's "forming" must be 1 when present.
constexpr int32_t kForming = 1;

// The first letter of battle_info's "host", as the game receives it.
enum class RoverBattleHost : int32_t {
    Arm = 0,     // 'A'
    Core = 1,    // 'C'
    Unknown = 2, // 'U' or absent; any other letter drops the row
};

// skill, players and on_death of battle_intel as the game receives them.
constexpr int32_t kSkillLimit = 2;
constexpr int32_t kOnDeathDefault = 2;
constexpr int32_t kOnDeathLimit = 3;

/// Builds an enter_battles request for an area, addressed to the game manager.
///
/// @param area_id area whose battle list to join
/// @return a new request record, or null when it could not be built
RoverRecord* request_enter_battles(uint32_t area_id) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::GameManager, kCommandEnterBattles);
    if (entry)
        rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
    return record;
}

/// Builds a leave_battles request for an area, addressed to the game manager.
///
/// @param area_id area whose battle list to leave
/// @return a new request record, or null when it could not be built
RoverRecord* request_leave_battles(uint32_t area_id) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::GameManager, kCommandLeaveBattles);
    if (entry)
        rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
    return record;
}

/// Builds a get_battle request for one game of an area, addressed to the game manager.
///
/// @param area_id area of the game
/// @param game_id id of the game to describe
/// @return a new request record, or null when it could not be built
RoverRecord* request_get_battle(uint32_t area_id, const char* game_id) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::GameManager, kCommandGetBattle);
    if (entry) {
        rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
        rover_entry_set_string(*entry, kRoverKeyGameId, game_id);
    }
    return record;
}

// battles_entered and battles_left: report whether the request failed, once.
bool accept_battles_done(
    RoverHandlerSet& handlers,
    RoverHandlerId id,
    RoverBattlesDoneHandler& handler,
    const RoverField& command,
    const char* name,
    const RoverEntry& entry
) {
    if (!rover_command_is(command, name))
        return false;
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    if (!area)
        return true;
    if (rover_field_uint(*area, 0) != handler.area_id)
        return false;
    bool failed = false;
    if (const RoverField* result = rover_entry_find(entry, kRoverKeyResult))
        failed = rover_field_int(*result, -1) != 0;
    if (handler.callback)
        handler.callback(failed);
    rover_handler_disarm(handlers, id);
    handler = RoverBattlesDoneHandler{};
    return true;
}

/// Takes battles_entered for the handler's area, reports once whether the request failed, then
/// disarms the handler.
///
/// A non-zero or unreadable result counts as a failure. An entry without area_id is taken and
/// ignored, leaving the handler armed.
///
/// @param[in,out] handlers handler set holding the battles_entered continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is battles_entered and not for another area, false otherwise
bool accept_battles_entered(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    return accept_battles_done(
        handlers,
        RoverHandlerId::BattlesEntered,
        handlers.lobby.battles_entered,
        command,
        kCommandBattlesEntered,
        entry
    );
}

/// Takes battles_left for the handler's area, reports once whether the request failed, then disarms
/// the handler.
///
/// A non-zero or unreadable result counts as a failure. An entry without area_id is taken and
/// ignored, leaving the handler armed.
///
/// @param[in,out] handlers handler set holding the battles_left continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is battles_left and not for another area, false otherwise
bool accept_battles_left(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    return accept_battles_done(
        handlers,
        RoverHandlerId::BattlesLeft,
        handlers.lobby.battles_left,
        command,
        kCommandBattlesLeft,
        entry
    );
}

// One battle_info row, as both battle_info handlers decode it.
struct BattleRow {
    RoverChange change{};
    const RoverField* game_id{};
    const RoverField* name{};
    int32_t type{};
    bool forming{};
};

enum class RowRead { Taken, Ignored, NotOurs };

// Reads the row of the handler's area; Ignored for a row the handler takes
// but does not report.
RowRead read_battle_row(const RoverEntry& entry, uint32_t area_id, BattleRow* out_row) {
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    out_row->game_id = rover_entry_find(entry, kRoverKeyGameId);
    out_row->name = rover_entry_find(entry, kRoverKeyName);
    const RoverField* type = rover_entry_find(entry, kRoverKeyType);
    if (!area || !out_row->game_id || !out_row->name || !type)
        return RowRead::Ignored;
    if (rover_field_uint(*area, 0) != area_id)
        return RowRead::NotOurs;
    if (!rover_read_change(entry, &out_row->change))
        return RowRead::Ignored;
    if (out_row->game_id->value_length == 0 || out_row->name->value_length == 0)
        return RowRead::Ignored;
    out_row->type = rover_field_int(*type, -1);
    out_row->forming = false;
    if (const RoverField* forming = rover_entry_find(entry, kRoverKeyForming)) {
        if (rover_field_int(*forming, -1) != kForming)
            return RowRead::Ignored;
        out_row->forming = true;
    }
    return RowRead::Taken;
}

/// Takes a battle_info row of the subscribed area and reports the battle's addition or removal.
///
/// The host's side comes from the first letter of "host": 'A' Arm, 'C' Core, 'U' or no field
/// unknown. A row with another letter, without area_id, game_id, name or type, with an empty game
/// id or name, an unknown change, or a "forming" other than 1 is taken but not reported.
///
/// @param handlers handler set holding the battle_info callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is battle_info and not for another area, false otherwise
bool accept_battle_info(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandBattleInfo))
        return false;
    RoverBattleInfoHandler& handler = handlers.lobby.battle_info;
    BattleRow row;
    RowRead read = read_battle_row(entry, handler.area_id, &row);
    if (read != RowRead::Taken)
        return read == RowRead::Ignored;
    RoverBattleHost host = RoverBattleHost::Unknown;
    if (const RoverField* letter = rover_entry_find(entry, kKeyHost)) {
        switch (rover_field_text(*letter)[0]) {
        case 'A':
            host = RoverBattleHost::Arm;
            break;
        case 'C':
            host = RoverBattleHost::Core;
            break;
        case 'U':
            break;
        default:
            return true;
        }
    }
    if (handler.callback) {
        handler.callback(
            static_cast<int32_t>(row.change),
            rover_show(rover_field_text(*row.game_id)),
            rover_show(rover_field_text(*row.name)),
            row.type,
            row.forming,
            static_cast<int32_t>(host)
        );
    }
    return true;
}

/// Takes a battle_info row of the key/value subscription and reports it with the row's other fields
/// in a bag that is freed after the callback.
///
/// Rows are filtered as for accept_battle_info, without the "host" letter.
///
/// @param handlers handler set holding the key/value battle_info callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is battle_info and not for another area, false otherwise
bool accept_battle_info_fields(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandBattleInfo))
        return false;
    RoverBattleInfoFieldsHandler& handler = handlers.lobby.battle_info_fields;
    BattleRow row;
    RowRead read = read_battle_row(entry, handler.area_id, &row);
    if (read != RowRead::Taken)
        return read == RowRead::Ignored;
    static const char* const kDecoded[] = {
        kRoverKeyAreaId,
        kRoverKeyGameId,
        kRoverKeyName,
        kRoverKeyType,
        kRoverKeyChange,
        kRoverKeyForming
    };
    constexpr int32_t kDecodedCount = static_cast<int32_t>(sizeof kDecoded / sizeof kDecoded[0]);
    RoverPropertyBag* fields = rover_collect_field_bag(entry, kDecoded, kDecodedCount);
    if (handler.callback) {
        handler.callback(
            static_cast<int32_t>(row.change),
            rover_show(rover_field_text(*row.game_id)),
            rover_show(rover_field_text(*row.name)),
            row.type,
            row.forming,
            fields
        );
    }
    rover_release_field_bag(fields);
    return true;
}

/// Takes an intel_msg line for the subscribed area's battle list and hands it to the game.
///
/// The callback learns whether the line names a game and, if so, which. An entry without area_id or
/// message, or with an empty message, is taken and ignored.
///
/// @param handlers handler set holding the intel_msg callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is intel_msg and not for another area, false otherwise
bool accept_intel_message(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandIntelMessage))
        return false;
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* message = rover_entry_find(entry, kRoverKeyMessage);
    if (!area || !message)
        return true;
    RoverIntelHandler& handler = handlers.lobby.intel_msg;
    if (rover_field_uint(*area, 0) != handler.area_id)
        return false;
    if (message->value_length == 0)
        return true;
    const RoverField* game = rover_entry_find(entry, kRoverKeyGameId);
    if (handler.callback)
        handler.callback(
            game == nullptr,
            game ? rover_show(rover_field_text(*game)) : nullptr,
            rover_show(rover_field_text(*message))
        );
    return true;
}

// True when the entry is about the battle a detail handler asked for.
bool is_battle(
    const RoverField& area, const RoverField& game, uint32_t area_id, const char* game_id
) {
    return rover_field_uint(area, 0) == area_id && rover_game_id_matches(game, game_id);
}

// Wire skill (0-2) and on_death (1-3) as the game receives them.
int32_t battle_skill(const RoverEntry& entry) {
    const RoverField* field = rover_entry_find(entry, kKeySkill);
    int32_t skill = field ? rover_field_int(*field, -1) : 0;
    return skill >= 0 && skill <= kSkillLimit ? skill : 0;
}

// Wire player counts 1-4 become the game's selector values 3, 0, 1 and 2.
int32_t battle_players(const RoverEntry& entry) {
    const RoverField* field = rover_entry_find(entry, kKeyPlayers);
    switch (field ? rover_field_int(*field, -1) : 0) {
    case 1:
        return 3;
    case 3:
        return 1;
    case 4:
        return 2;
    default:
        return 0;
    }
}

int32_t battle_on_death(const RoverEntry& entry) {
    const RoverField* field = rover_entry_find(entry, kKeyOnDeath);
    int32_t on_death = field ? rover_field_int(*field, -1) : kOnDeathDefault;
    return on_death >= 1 && on_death <= kOnDeathLimit ? on_death : kOnDeathDefault;
}

// An I field that is present and not 0.
bool flag_set(const RoverEntry& entry, const char* key) {
    const RoverField* field = rover_entry_find(entry, key);
    return field && rover_field_int(*field, -1) != 0;
}

/// Takes battle_intel for the battle asked for, reports its settings once, then disarms the
/// handler.
///
/// skill outside 0-2 becomes 0; players 1, 3 and 4 become the game's selector values 3, 1 and 2,
/// anything else 0; on_death outside 1-3 becomes 2. An entry without area_id or game_id is taken
/// and ignored.
///
/// @param[in,out] handlers handler set holding the battle_intel continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is battle_intel and not about another battle, false otherwise
bool accept_battle_intel(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandBattleIntel))
        return false;
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* game = rover_entry_find(entry, kRoverKeyGameId);
    if (!area || !game)
        return true;
    RoverBattleIntelHandler& handler = handlers.lobby.battle_intel;
    if (!is_battle(*area, *game, handler.area_id, handler.game_id))
        return false;
    uint32_t user_id = 0;
    const char* username = nullptr;
    const char* mission = nullptr;
    rover_entry_get_uint(entry, kRoverKeyUserId, &user_id, 0);
    rover_entry_get_string(entry, kRoverKeyUsername, &username, kRoverNoText);
    bool has_password = flag_set(entry, kRoverKeyPassword);
    rover_entry_get_string(entry, kRoverKeyMission, &mission, kRoverNoText);
    int32_t skill = battle_skill(entry);
    int32_t players = battle_players(entry);
    int32_t on_death = battle_on_death(entry);
    bool failed = flag_set(entry, kRoverKeyResult);
    if (handler.callback)
        handler.callback(
            rover_show(handler.game_id),
            user_id,
            rover_show(username),
            has_password,
            rover_show(mission),
            skill,
            players,
            on_death,
            failed
        );
    rover_handler_disarm(handlers, RoverHandlerId::BattleIntel);
    handler = RoverBattleIntelHandler{};
    return true;
}

/// Takes battle_intel for the battle asked for through the key/value request, reports it with its
/// other fields in a bag, then disarms the handler.
///
/// An entry without area_id or game_id is taken and ignored.
///
/// @param[in,out] handlers handler set holding the key/value battle_intel continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is battle_intel and not about another battle, false otherwise
bool accept_battle_intel_fields(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandBattleIntel))
        return false;
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* game = rover_entry_find(entry, kRoverKeyGameId);
    if (!area || !game)
        return true;
    RoverBattleIntelFieldsHandler& handler = handlers.lobby.battle_intel_fields;
    if (!is_battle(*area, *game, handler.area_id, handler.game_id))
        return false;
    uint32_t user_id = 0;
    const char* username = nullptr;
    rover_entry_get_uint(entry, kRoverKeyUserId, &user_id, 0);
    rover_entry_get_string(entry, kRoverKeyUsername, &username, kRoverNoText);
    bool has_password = flag_set(entry, kRoverKeyPassword);
    bool failed = flag_set(entry, kRoverKeyResult);
    static const char* const kDecoded[] = {
        kRoverKeyAreaId,
        kRoverKeyGameId,
        kRoverKeyUserId,
        kRoverKeyUsername,
        kRoverKeyPassword,
        kRoverKeyResult
    };
    constexpr int32_t kDecodedCount = static_cast<int32_t>(sizeof kDecoded / sizeof kDecoded[0]);
    RoverPropertyBag* fields = rover_collect_field_bag(entry, kDecoded, kDecodedCount);
    if (handler.callback)
        handler.callback(
            rover_show(handler.game_id), user_id, rover_show(username), has_password, failed, fields
        );
    rover_release_field_bag(fields);
    rover_handler_disarm(handlers, RoverHandlerId::BattleIntelFields);
    handler = RoverBattleIntelFieldsHandler{};
    return true;
}

/// Takes one player's battle_report line for the battle asked for and reports it.
///
/// The handler stays armed for the other players. An entry without area_id, game_id, user_id or
/// username, or with an empty username, is taken and ignored; missing numbers read 0.
///
/// @param handlers handler set holding the battle_report callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is battle_report and not about another battle, false otherwise
bool accept_battle_report(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandBattleReport))
        return false;
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* game = rover_entry_find(entry, kRoverKeyGameId);
    const RoverField* user = rover_entry_find(entry, kRoverKeyUserId);
    const RoverField* username = rover_entry_find(entry, kRoverKeyUsername);
    if (!area || !game || !user || !username)
        return true;
    RoverBattleReportHandler& handler = handlers.lobby.battle_report;
    if (!is_battle(*area, *game, handler.area_id, handler.game_id))
        return false;
    uint32_t user_id = rover_field_uint(*user, 0);
    int32_t icon = 0;
    rover_entry_get_int(entry, kRoverKeyIcon, &icon, 0);
    if (username->value_length == 0)
        return true;
    const char* message = nullptr;
    int32_t kills = 0;
    int32_t losses = 0;
    int32_t energy = 0;
    int32_t metal = 0;
    int32_t score = 0;
    rover_entry_get_string(entry, kRoverKeyMessage, &message, kRoverNoText);
    rover_entry_get_int(entry, kKeyKills, &kills, 0);
    rover_entry_get_int(entry, kKeyLosses, &losses, 0);
    rover_entry_get_int(entry, kRoverKeyEnergy, &energy, 0);
    rover_entry_get_int(entry, kRoverKeyMetal, &metal, 0);
    rover_entry_get_int(entry, kKeyScore, &score, 0);
    if (handler.callback)
        handler.callback(
            rover_show(handler.game_id),
            user_id,
            icon,
            rover_show(rover_field_text(*username)),
            kills,
            losses,
            energy,
            metal,
            score,
            rover_show(message)
        );
    return true;
}

/// Takes one player's battle_report line for the key/value request and reports it with the line's
/// other fields in a bag.
///
/// The handler stays armed for the other players. Lines are filtered as for accept_battle_report.
///
/// @param handlers handler set holding the key/value battle_report callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is battle_report and not about another battle, false otherwise
bool accept_battle_report_fields(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandBattleReport))
        return false;
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* game = rover_entry_find(entry, kRoverKeyGameId);
    const RoverField* user = rover_entry_find(entry, kRoverKeyUserId);
    const RoverField* username = rover_entry_find(entry, kRoverKeyUsername);
    if (!area || !game || !user || !username)
        return true;
    RoverBattleReportFieldsHandler& handler = handlers.lobby.battle_report_fields;
    if (!is_battle(*area, *game, handler.area_id, handler.game_id))
        return false;
    uint32_t user_id = rover_field_uint(*user, 0);
    if (username->value_length == 0)
        return true;
    const char* message = nullptr;
    rover_entry_get_string(entry, kRoverKeyMessage, &message, kRoverNoText);
    static const char* const kDecoded[] = {
        kRoverKeyAreaId, kRoverKeyGameId, kRoverKeyUserId, kRoverKeyUsername, kRoverKeyMessage
    };
    constexpr int32_t kDecodedCount = static_cast<int32_t>(sizeof kDecoded / sizeof kDecoded[0]);
    RoverPropertyBag* fields = rover_collect_field_bag(entry, kDecoded, kDecodedCount);
    if (handler.callback)
        handler.callback(
            rover_show(handler.game_id),
            user_id,
            rover_show(rover_field_text(*username)),
            rover_show(message),
            fields
        );
    rover_release_field_bag(fields);
    return true;
}

// Drops the intel_msg handler, arms battles_left and sends leave_battles;
// the slots first drop their variant's list and detail handlers.
void finish_leave_battles(RoverHandlerSet& handlers, uint32_t area_id, RoverBattlesDoneFn left) {
    RoverLobbyHandlers& lobby = handlers.lobby;
    rover_handler_disarm(handlers, RoverHandlerId::IntelMessage);
    lobby.intel_msg = RoverIntelHandler{};
    lobby.battles_left = RoverBattlesDoneHandler{area_id, left};
    rover_handler_arm(handlers, RoverHandlerId::BattlesLeft);
    rover_send_to_boneyard(request_leave_battles(area_id));
}

} // namespace

bool rover_offer_battle_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
) {
    switch (id) {
    case RoverHandlerId::BattlesEntered:
        return accept_battles_entered(handlers, command, entry);
    case RoverHandlerId::BattleInfoFields:
        return accept_battle_info_fields(handlers, command, entry);
    case RoverHandlerId::BattleInfo:
        return accept_battle_info(handlers, command, entry);
    case RoverHandlerId::IntelMessage:
        return accept_intel_message(handlers, command, entry);
    case RoverHandlerId::BattleIntelFields:
        return accept_battle_intel_fields(handlers, command, entry);
    case RoverHandlerId::BattleIntel:
        return accept_battle_intel(handlers, command, entry);
    case RoverHandlerId::BattleReportFields:
        return accept_battle_report_fields(handlers, command, entry);
    case RoverHandlerId::BattleReport:
        return accept_battle_report(handlers, command, entry);
    case RoverHandlerId::BattlesLeft:
        return accept_battles_left(handlers, command, entry);
    default:
        return false;
    }
}

bool ROVER_CALL rover_enter_battles_kv(
    uint32_t area_id,
    RoverBattlesDoneFn entered,
    RoverBattleInfoKvFn battle_info,
    RoverIntelMsgFn intel_msg
) {
    if (!g_rover_state)
        return false;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    lobby.battles_entered = RoverBattlesDoneHandler{area_id, entered};
    rover_handler_arm(handlers, RoverHandlerId::BattlesEntered);
    lobby.battle_info_fields = RoverBattleInfoFieldsHandler{area_id, battle_info};
    rover_handler_arm(handlers, RoverHandlerId::BattleInfoFields);
    lobby.intel_msg = RoverIntelHandler{area_id, intel_msg};
    rover_handler_arm(handlers, RoverHandlerId::IntelMessage);
    rover_send_to_boneyard(request_enter_battles(area_id));
    return true;
}

bool ROVER_CALL rover_enter_battles(
    uint32_t area_id,
    RoverBattlesDoneFn entered,
    RoverBattleInfoFn battle_info,
    RoverIntelMsgFn intel_msg
) {
    if (!g_rover_state)
        return false;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    lobby.battles_entered = RoverBattlesDoneHandler{area_id, entered};
    rover_handler_arm(handlers, RoverHandlerId::BattlesEntered);
    lobby.battle_info = RoverBattleInfoHandler{area_id, battle_info};
    rover_handler_arm(handlers, RoverHandlerId::BattleInfo);
    lobby.intel_msg = RoverIntelHandler{area_id, intel_msg};
    rover_handler_arm(handlers, RoverHandlerId::IntelMessage);
    rover_send_to_boneyard(request_enter_battles(area_id));
    return true;
}

bool ROVER_CALL rover_leave_battles_kv(uint32_t area_id, RoverBattlesDoneFn left) {
    if (!g_rover_state)
        return false;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    rover_handler_disarm(handlers, RoverHandlerId::BattleInfoFields);
    lobby.battle_info_fields = RoverBattleInfoFieldsHandler{};
    if (rover_handler_is_armed(handlers, RoverHandlerId::BattleIntelFields)) {
        rover_handler_disarm(handlers, RoverHandlerId::BattleIntelFields);
        lobby.battle_intel_fields = RoverBattleIntelFieldsHandler{};
    }
    if (rover_handler_is_armed(handlers, RoverHandlerId::BattleReportFields)) {
        rover_handler_disarm(handlers, RoverHandlerId::BattleReportFields);
        lobby.battle_report_fields = RoverBattleReportFieldsHandler{};
    }
    finish_leave_battles(handlers, area_id, left);
    return true;
}

bool ROVER_CALL rover_leave_battles(uint32_t area_id, RoverBattlesDoneFn left) {
    if (!g_rover_state)
        return false;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    rover_handler_disarm(handlers, RoverHandlerId::BattleInfo);
    lobby.battle_info = RoverBattleInfoHandler{};
    if (rover_handler_is_armed(handlers, RoverHandlerId::BattleIntel)) {
        rover_handler_disarm(handlers, RoverHandlerId::BattleIntel);
        lobby.battle_intel = RoverBattleIntelHandler{};
    }
    if (rover_handler_is_armed(handlers, RoverHandlerId::BattleReport)) {
        rover_handler_disarm(handlers, RoverHandlerId::BattleReport);
        lobby.battle_report = RoverBattleReportHandler{};
    }
    finish_leave_battles(handlers, area_id, left);
    return true;
}

bool ROVER_CALL rover_get_battle_kv(
    uint32_t area_id,
    const char* game_id,
    RoverBattleIntelKvFn battle_intel,
    RoverBattleReportKvFn battle_report
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    const char* game_text = rover_take_text(game_in, kRoverInterfaceTextBytes, game_id);
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    rover_handler_disarm(handlers, RoverHandlerId::BattleIntelFields);
    lobby.battle_intel_fields.area_id = area_id;
    rover_store_game_id(lobby.battle_intel_fields.game_id, game_text);
    lobby.battle_intel_fields.callback = battle_intel;
    rover_handler_arm(handlers, RoverHandlerId::BattleIntelFields);
    rover_handler_disarm(handlers, RoverHandlerId::BattleReportFields);
    lobby.battle_report_fields.area_id = area_id;
    rover_store_game_id(lobby.battle_report_fields.game_id, game_text);
    lobby.battle_report_fields.callback = battle_report;
    rover_handler_arm(handlers, RoverHandlerId::BattleReportFields);
    rover_send_to_boneyard(request_get_battle(area_id, game_text));
    return true;
}

bool ROVER_CALL rover_get_battle(
    uint32_t area_id,
    const char* game_id,
    RoverBattleIntelFn battle_intel,
    RoverBattleReportFn battle_report
) {
    if (!g_rover_state)
        return false;
    char game_in[kRoverInterfaceTextBytes];
    const char* game_text = rover_take_text(game_in, kRoverInterfaceTextBytes, game_id);
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    rover_handler_disarm(handlers, RoverHandlerId::BattleIntel);
    lobby.battle_intel.area_id = area_id;
    rover_store_game_id(lobby.battle_intel.game_id, game_text);
    lobby.battle_intel.callback = battle_intel;
    rover_handler_arm(handlers, RoverHandlerId::BattleIntel);
    rover_handler_disarm(handlers, RoverHandlerId::BattleReport);
    lobby.battle_report.area_id = area_id;
    rover_store_game_id(lobby.battle_report.game_id, game_text);
    lobby.battle_report.callback = battle_report;
    rover_handler_arm(handlers, RoverHandlerId::BattleReport);
    rover_send_to_boneyard(request_get_battle(area_id, game_text));
    return true;
}
