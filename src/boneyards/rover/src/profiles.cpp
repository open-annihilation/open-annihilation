// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Profiles (slots 63 and 64): a user's dossier, which the game asks for
// right after login and without which it aborts the login, and dossier
// updates; with the handlers of their replies (13 and 14). Requests go to the
// user manager.

#include "bags.h"
#include "lobby.h"
#include "lobby_handlers.h"
#include "protocol.h"
#include "requests.h"
#include "slots.h"
#include "state.h"

namespace {

constexpr char kCommandGetProfile[] = "get_profile";
constexpr char kCommandSetProfile[] = "set_profile";
constexpr char kCommandProfileInfo[] = "profile_info";
constexpr char kCommandLadderProfile[] = "ladder_profile";
constexpr char kCommandProfileUpdated[] = "profile_updated";

constexpr char kKeyLadderId[] = "ladder_id";
constexpr char kKeyLadderSubId[] = "ladder_subid";

// profile_info's "last": 0 while more follow, -1 when the profile could not
// be read, anything else when it is complete.
constexpr int32_t kProfileMore = 0;
constexpr int32_t kProfileFailed = -1;
// A set_profile result of 0 is success.
constexpr int32_t kProfileUpdated = 0;

/// Builds get_profile for a user, addressed to the user manager.
///
/// @param user_id user whose profile to read
/// @return a new request record, or null when it could not be built
RoverRecord* request_get_profile(uint32_t user_id) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::UserManager, kCommandGetProfile);
    if (entry)
        rover_entry_set_uint(*entry, kRoverKeyUserId, user_id);
    return record;
}

/// Appends a set_profile entry for a user, addressed to the user manager.
///
/// @param[in,out] record record to append to; a new one is created when *record is null
/// @param user_id user whose profile to change
/// @return the new entry, or null when it could not be built
RoverEntry* request_set_profile(RoverRecord** record, uint32_t user_id) {
    RoverEntry* entry = rover_add_request(record, RoverService::UserManager, kCommandSetProfile);
    if (entry)
        rover_entry_set_uint(*entry, kRoverKeyUserId, user_id);
    return entry;
}

/// Drops the gathered profile: frees its bag and clears the handler.
///
/// @param[in,out] handler profile handler to clear
void reset_profile(RoverProfileHandler& handler) {
    rover_release_field_bag(handler.properties);
    handler = RoverProfileHandler{};
}

/// Takes profile_info or ladder_profile of the user asked for and gathers its fields.
///
/// Each message's fields join the profile's bag, except when "last" is -1; a ladder profile also
/// gets a bag of its own for the ladder callback. The message marked last hands the profile to the
/// game (failed when last is -1) and disarms the handler. An entry without user_id is taken and
/// ignored.
///
/// @param[in,out] handlers handler set holding the profile continuation and its bag
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is a profile message and not about another user, false otherwise
bool accept_profile(RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry) {
    bool ladder = rover_command_is(command, kCommandLadderProfile);
    if (!ladder && !rover_command_is(command, kCommandProfileInfo))
        return false;
    const RoverField* user = rover_entry_find(entry, kRoverKeyUserId);
    if (!user)
        return true;
    RoverProfileHandler& handler = handlers.lobby.profile;
    if (rover_field_uint(*user, 0) != handler.user_id)
        return false;
    static const char* const kDecoded[] = {kKeyLadderId, kKeyLadderSubId, kRoverKeyLast};
    constexpr int32_t kDecodedCount = static_cast<int32_t>(sizeof kDecoded / sizeof kDecoded[0]);
    RoverPropertyBag* fields = handler.properties;
    int32_t ladder_id = 0;
    int32_t ladder_subid = 0;
    if (ladder) {
        rover_entry_get_int(entry, kKeyLadderId, &ladder_id, 0);
        rover_entry_get_int(entry, kKeyLadderSubId, &ladder_subid, 0);
        fields = rover_create_property_bag();
    }
    int32_t last = kProfileMore;
    rover_entry_get_int(entry, kRoverKeyLast, &last, kProfileMore);
    if (last != kProfileFailed)
        rover_fill_field_bag(fields, entry, kDecoded, kDecodedCount);
    if (ladder) {
        if (handler.ladder)
            handler.ladder(ladder_id, ladder_subid, fields);
        rover_release_field_bag(fields);
    }
    if (last != kProfileMore) {
        if (handler.callback)
            handler.callback(last != kProfileFailed, handler.properties);
        rover_handler_disarm(handlers, RoverHandlerId::ProfileInfo);
        reset_profile(handler);
    }
    return true;
}

/// Takes profile_updated, reports whether the update succeeded, then disarms the handler.
///
/// A reply without a user id is taken and ignored; a missing result reads 0, success.
///
/// @param[in,out] handlers handler set holding the update continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is profile_updated and not about another user, false otherwise
bool accept_profile_updated(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandProfileUpdated))
        return false;
    uint32_t user_id = 0;
    int32_t result = 0;
    rover_entry_get_uint(entry, kRoverKeyUserId, &user_id, 0);
    rover_entry_get_int(entry, kRoverKeyResult, &result, 0);
    if (user_id == 0)
        return true;
    RoverProfileUpdatedHandler& handler = handlers.lobby.profile_updated;
    if (user_id != handler.user_id)
        return false;
    if (handler.callback)
        handler.callback(result == kProfileUpdated);
    rover_handler_disarm(handlers, RoverHandlerId::ProfileUpdated);
    handler = RoverProfileUpdatedHandler{};
    return true;
}

} // namespace

bool rover_offer_profile_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
) {
    if (id == RoverHandlerId::ProfileInfo)
        return accept_profile(handlers, command, entry);
    if (id == RoverHandlerId::ProfileUpdated)
        return accept_profile_updated(handlers, command, entry);
    return false;
}

void ROVER_CALL rover_request_get_profile(
    uint32_t user_id, RoverProfileFn profile, RoverLadderProfileFn ladder_profile
) {
    if (!g_rover_state)
        return;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverProfileHandler& handler = handlers.lobby.profile;
    handler.user_id = user_id;
    rover_release_field_bag(handler.properties);
    handler.properties = rover_create_property_bag();
    handler.callback = profile;
    handler.ladder = ladder_profile;
    rover_handler_arm(handlers, RoverHandlerId::ProfileInfo);
    rover_send_to_boneyard(request_get_profile(user_id));
}

void ROVER_CALL rover_request_set_profile(
    uint32_t user_id, RoverPropertyBag* properties, RoverProfileUpdatedFn updated
) {
    if (!g_rover_state)
        return;
    RoverHandlerSet& handlers = g_rover_state->handlers;
    handlers.lobby.profile_updated = RoverProfileUpdatedHandler{user_id, updated};
    rover_handler_arm(handlers, RoverHandlerId::ProfileUpdated);
    RoverRecord* record = nullptr;
    RoverEntry* entry = request_set_profile(&record, user_id);
    if (entry && properties)
        rover_walk_record_fields(properties, *entry);
    rover_send_to_boneyard(record);
}
