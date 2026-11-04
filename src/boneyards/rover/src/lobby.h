// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Helpers the lobby slots and their handlers share.
#pragma once

#include "handlers.h"
#include "record.h"

#include <stdint.h>

// Field keys and values of the lobby messages.
inline constexpr char kRoverKeyRoomId[] = "room_id";
inline constexpr char kRoverKeyGameId[] = "game_id";
inline constexpr char kRoverKeyPassword[] = "password";
inline constexpr char kRoverKeyChange[] = "change";
inline constexpr char kRoverKeyDescription[] = "description";
inline constexpr char kRoverKeyUsername[] = "username";
inline constexpr char kRoverKeyMission[] = "mission";
inline constexpr char kRoverKeyForming[] = "forming";
inline constexpr char kRoverKeyAllegiance[] = "allegiance";
inline constexpr char kRoverKeyLast[] = "last";
inline constexpr char kRoverKeyDpid[] = "dpid";
inline constexpr char kRoverKeyIcon[] = "icon";
inline constexpr char kRoverKeyEnergy[] = "energy";
inline constexpr char kRoverKeyMetal[] = "metal";
inline constexpr char kRoverKeyArray[] = "array";

// What a room, user or battle row reports in its optional "change" field.
enum class RoverChange : int32_t {
    Add = 0, // also when the field is absent
    Delete = 1,
};

// The empty text 3.1c hands the game for an absent string.
inline constexpr char kRoverNoText[] = "";

/// Queues a request on the boneyard link in use.
///
/// @param record request to send; a null record, or any record while detached, is dropped
void rover_send_to_boneyard(RoverRecord* record);

/// Builds enter_room for a room of an area, addressed to the chat manager.
///
/// @param area_id area of the room
/// @param room_id room to enter
/// @param password room password; sent only when non-null and non-empty
/// @return a new request record, or null when it could not be built
RoverRecord* rover_request_enter_room(uint32_t area_id, uint32_t room_id, const char* password);

/// Disarms a handler if armed, then arms it.
///
/// @param[in,out] handlers handler set to change
/// @param id handler to rearm
void rover_handler_rearm(RoverHandlerSet& handlers, RoverHandlerId id);

/// Tests a command field against a command name.
///
/// @param command the entry's command field
/// @param name command name to compare with
/// @return true when the field's text equals name exactly
bool rover_command_is(const RoverField& command, const char* name);

/// Returns the field's decoded text; its length is value_length.
///
/// @param field field to read
/// @return the text, or "" for a field without a value
const char* rover_field_text(const RoverField& field);

/// Reads a row's optional "change" field.
///
/// @param entry the row's fields
/// @param[out] out_change receives Delete for "delete", Add for "add" or no field
/// @return false when the field holds something else, which the handlers take as a message to
///     ignore
bool rover_read_change(const RoverEntry& entry, RoverChange* out_change);

/// Tests a game_id field against the game id a handler stored.
///
/// @param game_id the entry's game_id field
/// @param stored the handler's game id
/// @return true when the texts are equal
bool rover_game_id_matches(const RoverField& game_id, const char* stored);

/// Copies a game id as the handlers keep it: 36 characters at most.
///
/// @param[out] stored handler's game id buffer of kRoverGameIdLimit + 1 bytes
/// @param game_id game id to keep, or null for ""
void rover_store_game_id(char* stored, const char* game_id);

// Whether a bag of an entry's other fields leaves out the command field:
// most handlers list it with their own keys, system_alert and user_located
// pass it on.
enum class RoverBagCommand : uint8_t {
    Skipped = 0,
    Kept = 1,
};

/// Adds every field of the entry except the keys listed (and command unless kept) to a property
/// bag, as the key/value handlers pass them to the game.
///
/// The fields go in through slots 49-51: I fields as signed, U as unsigned and S as text. Keys and
/// values are staged as copy-only strings, so the bag never shares the record's text.
///
/// @param bag bag to fill; null does nothing
/// @param entry entry whose fields are copied
/// @param skipped keys to leave out
/// @param skipped_count number of keys in skipped
/// @param command whether the command field is left out or kept
void rover_fill_field_bag(
    RoverPropertyBag* bag,
    const RoverEntry& entry,
    const char* const* skipped,
    int32_t skipped_count,
    RoverBagCommand command = RoverBagCommand::Skipped
);

/// Creates a bag (slot 44) and fills it with the entry's other fields as rover_fill_field_bag does.
///
/// Release it with rover_release_field_bag after the callback.
///
/// @param entry entry whose fields are copied
/// @param skipped keys to leave out
/// @param skipped_count number of keys in skipped
/// @param command whether the command field is left out or kept
/// @return the new bag, or null when no bag could be made
RoverPropertyBag* rover_collect_field_bag(
    const RoverEntry& entry,
    const char* const* skipped,
    int32_t skipped_count,
    RoverBagCommand command = RoverBagCommand::Skipped
);

/// Frees a bag made for a callback.
///
/// @param bag bag to free, or null
void rover_release_field_bag(RoverPropertyBag* bag);

/// Notes the seats of the game_located row about to be handed to slot 66.
///
/// The callback's arguments are the 3.1c table's, so the two counts travel
/// beside it. Both are known only when the row carried both fields. The note
/// is taken by the callback's client, or dropped when nothing takes it.
///
/// @param known true when players and playerlimit were both present
/// @param players seats taken, host included
/// @param playerlimit seat limit
void rover_note_game_seats(bool known, int32_t players, int32_t playerlimit);

/// Takes the seats noted for the game_located callback in progress.
///
/// The note is cleared either way, so a later row cannot inherit it.
///
/// @param[out] players receives the seats taken when known
/// @param[out] playerlimit receives the seat limit when known
/// @return true when both fields were present
bool rover_take_game_seats(int32_t* players, int32_t* playerlimit);

/// Notes whether the room_list row about to be handed over carried global.
///
/// @param known true when the row had a global field
/// @param global the field's value; 1 is the sector-wide frequency
void rover_note_room_global(bool known, int32_t global);

/// Takes the global flag noted for the room_list callback in progress.
///
/// @param[out] global receives the field's value when it was present
/// @return true when the row carried global
bool rover_take_room_global(int32_t* global);
