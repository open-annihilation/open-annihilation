// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat rooms (slots 9-13 and 69): the room list of an area, entering,
// switching and leaving a room, saying something in it and creating one;
// with the handlers of their replies (16-21 and 44). Requests go to the chat
// manager.

#include "connect.h"
#include "lobby.h"
#include "lobby_handlers.h"
#include "protocol.h"
#include "requests.h"
#include "slots.h"
#include "state.h"
#include "wire_text.h"

namespace {

constexpr char kCommandGetRooms[] = "get_rooms";
constexpr char kCommandEnterRoom[] = "enter_room";
constexpr char kCommandLeaveRoom[] = "leave_room";
constexpr char kCommandSay[] = "say";
constexpr char kCommandCreateRoom[] = "create_room";
constexpr char kCommandRoomList[] = "room_list";
constexpr char kCommandRoomInfo[] = "room_info";
constexpr char kCommandEnteredRoom[] = "entered_room";
constexpr char kCommandLeftRoom[] = "left_room";
constexpr char kCommandChatMessage[] = "chat_msg";
constexpr char kCommandRoomCreated[] = "room_created";

constexpr char kKeyUsers[] = "users";
constexpr char kKeyFull[] = "full";
constexpr char kKeyNeedsPassword[] = "needs_pw";
constexpr char kKeyGlobal[] = "global";
constexpr char kKeyTag[] = "tag";
constexpr char kKeyLatency[] = "latency";
constexpr char kKeyRank[] = "rank";
// create_room names the description "desc"; room_list rows "description".
constexpr char kKeyDesc[] = "desc";

// Room types the game knows; any other is reported as 0.
constexpr int32_t kRoomTypeDefault = 1;
constexpr int32_t kRoomTypeLast = 3;
// Latency grades of a room member; any other is reported as 1.
constexpr int32_t kLatencyBest = 1;
constexpr int32_t kLatencyWorst = 3;

// entered_room results: 0 entered, 2 the room is on the other boneyard (the
// reply then names it), anything else a failure.
constexpr int32_t kEnteredOk = 0;
constexpr int32_t kEnteredFailed = 1;
constexpr int32_t kEnteredOnOtherYard = 2;

/// Builds get_rooms for an area, addressed to the chat manager.
///
/// @param area_id area whose rooms to list
/// @return a new request record, or null when it could not be built
RoverRecord* request_get_rooms(uint32_t area_id) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::ChatManager, kCommandGetRooms);
    if (entry)
        rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
    return record;
}

/// Builds leave_room for a room of an area, addressed to the chat manager.
///
/// @param area_id area of the room
/// @param room_id room to leave
/// @return a new request record, or null when it could not be built
RoverRecord* request_leave_room(uint32_t area_id, uint32_t room_id) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::ChatManager, kCommandLeaveRoom);
    if (entry) {
        rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
        rover_entry_set_uint(*entry, kRoverKeyRoomId, room_id);
    }
    return record;
}

/// Builds say for a room, or for one member of it when directed.
///
/// @param area_id area of the room
/// @param room_id room to speak in
/// @param message line to say
/// @param directed true to address one member
/// @param user_id member to address; sent only when directed
/// @return a new request record, or null when it could not be built
RoverRecord* request_say(
    uint32_t area_id, uint32_t room_id, const char* message, bool directed, uint32_t user_id
) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::ChatManager, kCommandSay);
    if (!entry)
        return record;
    rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
    rover_entry_set_uint(*entry, kRoverKeyRoomId, room_id);
    rover_entry_set_string(*entry, kRoverKeyMessage, message);
    if (directed)
        rover_entry_set_uint(*entry, kRoverKeyUserId, user_id);
    return record;
}

/// Builds create_room in an area with the room's name; the description (as "desc") and password go
/// only when not empty.
///
/// @param area_id area to create the room in
/// @param name room name, or null for ""
/// @param description room description, or null
/// @param password room password, or null
/// @return a new request record, or null when it could not be built
RoverRecord* request_create_room(
    uint32_t area_id, const char* name, const char* description, const char* password
) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::ChatManager, kCommandCreateRoom);
    if (!entry)
        return record;
    rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
    rover_entry_set_string(*entry, kRoverKeyName, name ? name : kRoverNoText);
    if (description && description[0] != '\0')
        rover_entry_set_string(*entry, kKeyDesc, description);
    if (password && password[0] != '\0')
        rover_entry_set_string(*entry, kRoverKeyPassword, password);
    return record;
}

// One room_list row, as both room_list handlers decode it.
struct RoomRow {
    RoverChange change{};
    uint32_t room_id{};
    const char* name{};
    const char* description{};
    bool needs_password{};
    int32_t users{};
    int32_t type{};
    const char* tag{};
};

// Decodes the row after its required fields were found; false when the
// change is unknown or the name empty, which the handlers take and ignore.
bool read_room_row(
    const RoverEntry& entry,
    const RoverField& room,
    const RoverField& name,
    const RoverField& users,
    RoomRow* out_row
) {
    if (!rover_read_change(entry, &out_row->change))
        return false;
    rover_entry_get_string(entry, kRoverKeyDescription, &out_row->description, kRoverNoText);
    out_row->room_id = rover_field_uint(room, 0);
    if (name.value_length == 0)
        return false;
    out_row->name = rover_field_text(name);
    out_row->users = rover_field_int(users, -1);
    int32_t flag = 0;
    rover_entry_get_int(entry, kKeyNeedsPassword, &flag, 0);
    out_row->needs_password = flag != 0;
    int32_t type = 0;
    rover_entry_get_int(entry, kRoverKeyType, &type, kRoomTypeDefault);
    out_row->type = type < 0 || type > kRoomTypeLast ? 0 : type;
    rover_entry_get_string(entry, kKeyTag, &out_row->tag, kRoverNoText);
    return true;
}

// Notes global for the room callback, then drops it if the callback did not.
void report_room_global(const RoverEntry& entry) {
    int32_t global = 0;
    const bool known = rover_entry_get_int(entry, kKeyGlobal, &global, 0);
    rover_note_room_global(known, global);
}

void drop_room_global() {
    int32_t global = 0;
    (void)rover_take_room_global(&global);
}

/// Takes a room_list row answering get_rooms and reports it; the row marked "last" ends the list
/// and disarms the handler.
///
/// Only rows of the asked area carrying "full" are taken; others are left to the room's own
/// handler. A row without room_id, area_id, name or users, with an unknown change or an empty name,
/// is taken and ignored. A type outside 0-3 is reported as 0, a missing one as 1.
///
/// @param[in,out] handlers handler set holding the room list continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the row is taken, false when it is not this handler's
bool accept_room_list_final(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandRoomList))
        return false;
    const RoverField* room = rover_entry_find(entry, kRoverKeyRoomId);
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* name = rover_entry_find(entry, kRoverKeyName);
    const RoverField* users = rover_entry_find(entry, kKeyUsers);
    const RoverField* full = rover_entry_find(entry, kKeyFull);
    if (!room || !area || !name || !users)
        return true;
    RoverRoomListHandler& handler = handlers.lobby.room_list_final;
    if (rover_field_uint(*area, 0) != static_cast<uint32_t>(handler.area_id) || !full)
        return false;
    RoomRow row;
    if (!read_room_row(entry, *room, *name, *users, &row))
        return true;
    int32_t last = 0;
    rover_entry_get_int(entry, kRoverKeyLast, &last, 0);
    report_room_global(entry);
    if (handler.callback) {
        handler.callback(
            static_cast<int32_t>(row.change),
            handler.area_id,
            static_cast<int32_t>(row.room_id),
            rover_show(row.name),
            rover_show(row.description),
            row.needs_password,
            row.users,
            row.type,
            rover_show(row.tag),
            last != 0
        );
    }
    drop_room_global();
    if (last != 0) {
        rover_handler_disarm(handlers, RoverHandlerId::RoomListFinal);
        handler = RoverRoomListHandler{};
    }
    return true;
}

/// Takes a room_list row of the area the entered room is in and reports it.
///
/// Rows are filtered and decoded as for accept_room_list_final, without the "full" and "last"
/// fields. The handler stays armed.
///
/// @param handlers handler set holding the room update callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is room_list and not for another area, false otherwise
bool accept_room_list(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandRoomList))
        return false;
    const RoverField* room = rover_entry_find(entry, kRoverKeyRoomId);
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* name = rover_entry_find(entry, kRoverKeyName);
    const RoverField* users = rover_entry_find(entry, kKeyUsers);
    if (!room || !area || !name || !users)
        return true;
    RoverRoomUpdateHandler& handler = handlers.lobby.room_list;
    if (rover_field_uint(*area, 0) != static_cast<uint32_t>(handler.area_id))
        return false;
    RoomRow row;
    if (read_room_row(entry, *room, *name, *users, &row)) {
        report_room_global(entry);
        if (handler.callback)
            handler.callback(
                static_cast<int32_t>(row.change),
                handler.area_id,
                static_cast<int32_t>(row.room_id),
                rover_show(row.name),
                rover_show(row.description),
                row.needs_password,
                row.users,
                row.type,
                rover_show(row.tag)
            );
        drop_room_global();
    }
    return true;
}

// True when the entry's room and area are the ones a room handler holds.
bool is_room(const RoverField& room, const RoverField& area, int32_t room_id, int32_t area_id) {
    return rover_field_uint(room, 0) == static_cast<uint32_t>(room_id) &&
           rover_field_uint(area, 0) == static_cast<uint32_t>(area_id);
}

/// Takes room_info: a member of the entered room arrived, changed or left.
///
/// A latency grade outside 1-3, or none, is reported as 1. An entry without room_id, area_id,
/// user_id or username, with an unknown change or an empty username, is taken but not reported.
///
/// @param handlers handler set holding the room member callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is room_info and not for another room, false otherwise
bool accept_room_info(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandRoomInfo))
        return false;
    const RoverField* room = rover_entry_find(entry, kRoverKeyRoomId);
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* user = rover_entry_find(entry, kRoverKeyUserId);
    const RoverField* username = rover_entry_find(entry, kRoverKeyUsername);
    if (!room || !area || !user || !username)
        return true;
    RoverRoomInfoHandler& handler = handlers.lobby.room_info;
    if (!is_room(*room, *area, handler.room_id, handler.area_id))
        return false;
    RoverChange change;
    if (!rover_read_change(entry, &change))
        return true;
    int32_t latency = 0;
    rover_entry_get_int(entry, kKeyLatency, &latency, kLatencyBest);
    if (latency < kLatencyBest || latency > kLatencyWorst)
        latency = kLatencyBest;
    int32_t icon = 0;
    int32_t rank = 0;
    const char* allegiance = nullptr;
    rover_entry_get_int(entry, kRoverKeyIcon, &icon, 0);
    rover_entry_get_int(entry, kKeyRank, &rank, 0);
    rover_entry_get_string(entry, kRoverKeyAllegiance, &allegiance, kRoverNoText);
    uint32_t user_id = rover_field_uint(*user, 0);
    if (username->value_length != 0 && handler.callback) {
        handler.callback(
            static_cast<int32_t>(change),
            static_cast<int32_t>(user_id),
            rover_show(rover_field_text(*username)),
            latency,
            icon,
            rank,
            rover_show(allegiance)
        );
    }
    return true;
}

/// Clears what the entered_room handler was armed with.
///
/// @param[out] handler handler to clear
/// @quirk Only the first byte of the password is cleared.
void reset_entered_room_handler(RoverEnteredRoomHandler& handler) {
    handler.room_id = 0;
    handler.area_id = 0;
    handler.callback = nullptr;
    handler.password[0] = '\0';
    handler.switching = false;
    handler.old_room_id = 0;
    handler.old_area_id = 0;
    handler.room_info = nullptr;
    handler.room_list = nullptr;
    handler.chat_msg = nullptr;
}

/// Takes entered_room, the result of entering (slot 10) or switching to (slot 11) a room, and
/// reports it, then disarms the handler.
///
/// A successful switch moves the room handlers to the new room and leaves the old one before the
/// game hears of it. Result 2 moves the session to the other boneyard the reply names to enter the
/// room again; the game hears of it only once the room has been entered there or the move failed,
/// and a reply that cannot start a move reports 1.
///
/// @param[in,out] handlers handler set holding the enter continuation and the room handlers
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is entered_room and not for another room, false otherwise
bool accept_entered_room(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandEnteredRoom))
        return false;
    const RoverField* room = rover_entry_find(entry, kRoverKeyRoomId);
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    if (!room || !area)
        return true;
    RoverLobbyHandlers& lobby = handlers.lobby;
    RoverEnteredRoomHandler& handler = lobby.entered_room;
    if (!is_room(*room, *area, handler.room_id, handler.area_id))
        return false;
    int32_t result = kEnteredOk;
    if (const RoverField* reply = rover_entry_find(entry, kRoverKeyResult)) {
        result = rover_field_int(*reply, -1);
        // The game hears of a move to the other boneyard only once the room
        // has been entered there, or the move failed.
        if (result == kEnteredOnOtherYard) {
            if (rover_move_to_other_boneyard(handler, entry)) {
                rover_handler_disarm(handlers, RoverHandlerId::EnteredRoom);
                reset_entered_room_handler(handler);
                return true;
            }
            result = kEnteredFailed;
        }
    }
    if (handler.callback) {
        if (handler.switching && result == kEnteredOk) {
            rover_handler_rearm(handlers, RoverHandlerId::RoomList);
            lobby.room_list = RoverRoomUpdateHandler{handler.area_id, handler.room_list};
            rover_handler_rearm(handlers, RoverHandlerId::RoomInfo);
            lobby.room_info =
                RoverRoomInfoHandler{handler.room_id, handler.area_id, handler.room_info};
            rover_handler_rearm(handlers, RoverHandlerId::ChatMessage);
            lobby.chat_msg = RoverChatHandler{handler.room_id, handler.area_id, handler.chat_msg};
            rover_send_to_boneyard(request_leave_room(
                static_cast<uint32_t>(handler.old_area_id),
                static_cast<uint32_t>(handler.old_room_id)
            ));
        }
        handler.callback(result);
    }
    rover_handler_disarm(handlers, RoverHandlerId::EnteredRoom);
    reset_entered_room_handler(handler);
    return true;
}

/// Takes left_room, reports whether leaving failed, then disarms the handler.
///
/// A non-zero or unreadable result counts as a failure. An entry without room_id or area_id is
/// taken and ignored.
///
/// @param[in,out] handlers handler set holding the leave continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is left_room and not for another room, false otherwise
bool accept_left_room(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandLeftRoom))
        return false;
    const RoverField* room = rover_entry_find(entry, kRoverKeyRoomId);
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    if (!room || !area)
        return true;
    RoverRoomLeftHandler& handler = handlers.lobby.left_room;
    if (!is_room(*room, *area, handler.room_id, handler.area_id))
        return false;
    bool failed = false;
    if (const RoverField* result = rover_entry_find(entry, kRoverKeyResult))
        failed = rover_field_int(*result, -1) != 0;
    if (handler.callback)
        handler.callback(failed);
    rover_handler_disarm(handlers, RoverHandlerId::LeftRoom);
    handler = RoverRoomLeftHandler{};
    return true;
}

/// Takes chat_msg, a line said in the room, and hands it to the game.
///
/// A line without a user id comes from the server, which the first callback argument flags. An
/// entry without room_id, area_id or message, or with an empty message, is taken and ignored.
///
/// @param handlers handler set holding the chat callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is chat_msg and not for another room, false otherwise
bool accept_chat_message(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandChatMessage))
        return false;
    const RoverField* room = rover_entry_find(entry, kRoverKeyRoomId);
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    const RoverField* message = rover_entry_find(entry, kRoverKeyMessage);
    if (!room || !area || !message)
        return true;
    RoverChatHandler& handler = handlers.lobby.chat_msg;
    if (!is_room(*room, *area, handler.room_id, handler.area_id))
        return false;
    if (message->value_length == 0)
        return true;
    const RoverField* user = rover_entry_find(entry, kRoverKeyUserId);
    uint32_t user_id = user ? rover_field_uint(*user, 0) : 0;
    if (handler.callback)
        handler.callback(
            user == nullptr, static_cast<int32_t>(user_id), rover_show(rover_field_text(*message))
        );
    return true;
}

/// Takes room_created and hands the game its area, room id and name, whatever the reply, then
/// disarms the handler.
///
/// A field the reply lacks reads as 0 or empty; the area and name the handler keeps are not
/// compared.
///
/// @param[in,out] handlers handler set holding the room creation continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is room_created, false otherwise
bool accept_room_created(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandRoomCreated))
        return false;
    uint32_t area_id = 0;
    const char* name = nullptr;
    uint32_t room_id = 0;
    rover_entry_get_uint(entry, kRoverKeyAreaId, &area_id, 0);
    rover_entry_get_string(entry, kRoverKeyName, &name, kRoverNoText);
    rover_entry_get_uint(entry, kRoverKeyRoomId, &room_id, 0);
    RoverRoomCreatedHandler& handler = handlers.lobby.room_created;
    if (handler.callback)
        handler.callback(&area_id, &room_id, rover_show(name));
    rover_handler_disarm(handlers, RoverHandlerId::RoomCreated);
    handler.callback = nullptr;
    return true;
}

} // namespace

RoverRecord* rover_request_enter_room(uint32_t area_id, uint32_t room_id, const char* password) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::ChatManager, kCommandEnterRoom);
    if (!entry)
        return record;
    rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id);
    rover_entry_set_uint(*entry, kRoverKeyRoomId, room_id);
    if (password && password[0] != '\0')
        rover_entry_set_string(*entry, kRoverKeyPassword, password);
    return record;
}

bool rover_offer_room_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
) {
    switch (id) {
    case RoverHandlerId::RoomListFinal:
        return accept_room_list_final(handlers, command, entry);
    case RoverHandlerId::RoomList:
        return accept_room_list(handlers, command, entry);
    case RoverHandlerId::RoomInfo:
        return accept_room_info(handlers, command, entry);
    case RoverHandlerId::EnteredRoom:
        return accept_entered_room(handlers, command, entry);
    case RoverHandlerId::LeftRoom:
        return accept_left_room(handlers, command, entry);
    case RoverHandlerId::ChatMessage:
        return accept_chat_message(handlers, command, entry);
    case RoverHandlerId::RoomCreated:
        return accept_room_created(handlers, command, entry);
    default:
        return false;
    }
}

bool ROVER_CALL rover_get_rooms(int32_t area_id, RoverRoomListFn room_list) {
    if (!g_rover_state)
        return false;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    handlers.lobby.room_list_final = RoverRoomListHandler{area_id, room_list};
    rover_handler_arm(handlers, RoverHandlerId::RoomListFinal);
    rover_send_to_boneyard(request_get_rooms(static_cast<uint32_t>(area_id)));
    return true;
}

bool ROVER_CALL rover_enter_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
) {
    if (!g_rover_state)
        return false;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    rover_handler_rearm(handlers, RoverHandlerId::RoomList);
    lobby.room_list = RoverRoomUpdateHandler{area_id, room_list};
    lobby.room_info = RoverRoomInfoHandler{room_id, area_id, room_info};
    rover_handler_arm(handlers, RoverHandlerId::RoomInfo);
    char password_in[kRoverInterfaceTextBytes];
    const char* password_text = rover_take_text(password_in, kRoverInterfaceTextBytes, password);
    RoverEnteredRoomHandler& enter = lobby.entered_room;
    enter.room_id = room_id;
    enter.area_id = area_id;
    enter.callback = entered;
    rover_copy_bounded(
        enter.password, password_text ? password_text : kRoverNoText, sizeof enter.password
    );
    enter.switching = false;
    rover_handler_arm(handlers, RoverHandlerId::EnteredRoom);
    lobby.chat_msg = RoverChatHandler{room_id, area_id, chat_msg};
    rover_handler_arm(handlers, RoverHandlerId::ChatMessage);
    rover_send_to_boneyard(rover_request_enter_room(
        static_cast<uint32_t>(area_id), static_cast<uint32_t>(room_id), password_text
    ));
    return true;
}

bool ROVER_CALL rover_switch_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    int32_t old_room_id,
    int32_t old_area_id,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
) {
    if (!g_rover_state)
        return false;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    char password_in[kRoverInterfaceTextBytes];
    const char* password_text = rover_take_text(password_in, kRoverInterfaceTextBytes, password);
    RoverEnteredRoomHandler& enter = handlers.lobby.entered_room;
    enter.room_id = room_id;
    enter.area_id = area_id;
    enter.callback = entered;
    rover_copy_bounded(
        enter.password, password_text ? password_text : kRoverNoText, sizeof enter.password
    );
    enter.old_room_id = old_room_id;
    enter.old_area_id = old_area_id;
    enter.room_info = room_info;
    enter.room_list = room_list;
    enter.switching = true;
    enter.chat_msg = chat_msg;
    rover_handler_arm(handlers, RoverHandlerId::EnteredRoom);
    rover_send_to_boneyard(rover_request_enter_room(
        static_cast<uint32_t>(area_id), static_cast<uint32_t>(room_id), password_text
    ));
    return true;
}

bool ROVER_CALL rover_leave_room(int32_t room_id, int32_t area_id, RoverRoomLeftFn left) {
    if (!g_rover_state)
        return false;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverLobbyHandlers& lobby = handlers.lobby;
    rover_handler_disarm(handlers, RoverHandlerId::ChatMessage);
    lobby.chat_msg = RoverChatHandler{};
    rover_handler_disarm(handlers, RoverHandlerId::RoomInfo);
    lobby.room_info = RoverRoomInfoHandler{};
    rover_handler_disarm(handlers, RoverHandlerId::RoomList);
    lobby.room_list = RoverRoomUpdateHandler{};
    if (room_id != 0) {
        lobby.left_room = RoverRoomLeftHandler{room_id, area_id, left};
        rover_handler_arm(handlers, RoverHandlerId::LeftRoom);
        rover_send_to_boneyard(
            request_leave_room(static_cast<uint32_t>(area_id), static_cast<uint32_t>(room_id))
        );
    }
    return true;
}

bool ROVER_CALL
rover_send_chat(int32_t room_id, int32_t area_id, int32_t target_user_id, const char* message) {
    if (!g_rover_state)
        return false;
    char message_in[kRoverInterfaceTextBytes];
    rover_send_to_boneyard(request_say(
        static_cast<uint32_t>(area_id),
        static_cast<uint32_t>(room_id),
        rover_take_text(message_in, kRoverInterfaceTextBytes, message),
        target_user_id != 0,
        static_cast<uint32_t>(target_user_id)
    ));
    return true;
}

void ROVER_CALL rover_create_room(
    uint32_t area_id,
    const char* room_name,
    const char* description,
    const char* password,
    RoverRoomCreatedFn created
) {
    if (!g_rover_state)
        return;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    char name_in[kRoverInterfaceTextBytes];
    char description_in[kRoverInterfaceTextBytes];
    char password_in[kRoverInterfaceTextBytes];
    const char* name_text = rover_take_text(name_in, kRoverInterfaceTextBytes, room_name);
    const char* description_text =
        rover_take_text(description_in, kRoverInterfaceTextBytes, description);
    const char* password_text = rover_take_text(password_in, kRoverInterfaceTextBytes, password);
    RoverRoomCreatedHandler& pending = handlers.lobby.room_created;
    pending.area_id = area_id;
    rover_copy_bounded(pending.name, name_text ? name_text : kRoverNoText, kRoverRoomNameLimit + 1);
    pending.callback = created;
    rover_handler_rearm(handlers, RoverHandlerId::RoomCreated);
    rover_send_to_boneyard(
        request_create_room(area_id, name_text, description_text, password_text)
    );
}
