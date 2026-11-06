// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Rover interface table and the slots that have no home of their own yet.
// Each stub reports failure (false, null or the invalid marker), so
// a host sees a lobby that is unavailable rather than one that half works.
// The game never uses slots 67, 68 or 73-81, so they and the handlers they
// arm (43 and 45-49) are left out. Slots 65 and 66, the locators,
// live with the pages and slot 69, room creation, with the rooms.

#include "slots.h"

int32_t ROVER_CALL rover_check_version(int32_t iface_version, int32_t build_stamp) {
    if (iface_version == OA_ROVER_INTERFACE_VERSION && build_stamp >= OA_ROVER_MINIMUM_BUILD_STAMP)
        return 1;
    return 0;
}

void ROVER_CALL rover_start_http_login(
    const char*, const char*, const char*, const char*, const char*, RoverHttpLoginFn
) {
}

void ROVER_CALL rover_request_areas(uint32_t, uint32_t, RoverOpaqueFn) {
}

bool ROVER_CALL rover_enter_metagame(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    return false;
}

bool ROVER_CALL rover_leave_metagame(uint32_t) {
    return false;
}

bool ROVER_CALL rover_get_house_info(uint32_t, uint32_t, uint32_t, uint32_t, const char*, void*) {
    return false;
}

bool ROVER_CALL rover_find_house(uint32_t, const char*, void*) {
    return false;
}

bool ROVER_CALL rover_set_house_info(uint32_t, uint32_t, void*) {
    return false;
}

bool ROVER_CALL rover_create_house(uint32_t, const char*, uint32_t, void*) {
    return false;
}

bool ROVER_CALL rover_queue_command(const char*, const char*, void*) {
    return false;
}

bool ROVER_CALL rover_register_command_handler(const char*, RoverOpaqueFn) {
    return false;
}

bool ROVER_CALL rover_unregister_command(const char*) {
    return false;
}

/// Holds the interface table in slot order, constant-initialised instead of being filled on process
/// attach; its contents never change.
static const RoverInterface kRoverInterface = {
    .check_version = rover_check_version,
    .attach = rover_attach,
    .detach = rover_detach,
    .activate = rover_activate,
    .deactivate = rover_deactivate,
    .pump_events = rover_pump_events,
    .reset_session = rover_reset_session,
    .start_login = rover_start_login,
    .send_logout = rover_send_logout,
    .get_rooms = rover_get_rooms,
    .enter_room = rover_enter_room,
    .switch_room = rover_switch_room,
    .leave_room = rover_leave_room,
    .send_chat = rover_send_chat,
    .enter_battles_kv = rover_enter_battles_kv,
    .enter_battles = rover_enter_battles,
    .leave_battles_kv = rover_leave_battles_kv,
    .leave_battles = rover_leave_battles,
    .get_battle_kv = rover_get_battle_kv,
    .get_battle = rover_get_battle,
    .host_game_kv = rover_host_game_kv,
    .host_game = rover_host_game,
    .join_game_with_fields = rover_join_game_with_fields,
    .join_game = rover_join_game,
    .send_left_game = rover_send_left_game,
    .send_game_settings_fields = rover_send_game_settings_fields,
    .send_game_settings = rover_send_game_settings,
    .send_player_info_fields = rover_send_player_info_fields,
    .send_player_info = rover_send_player_info,
    .send_score_report_props = rover_send_score_report_props,
    .send_score_report = rover_send_score_report,
    .send_score_report_message = rover_send_score_report_message,
    .fetch_http_file = rover_fetch_http_file,
    .parse_record_file = rover_parse_record_file,
    .open_sector_map = rover_open_sector_map,
    .read_sector_planets = rover_read_sector_planets,
    .close_sector_map = rover_close_sector_map,
    .enumerate_record_file = rover_enumerate_record_file,
    .request_census = rover_request_census,
    .request_allegiance = rover_request_allegiance,
    .get_file_server = rover_get_file_server,
    .send_page_by_name = rover_send_page_by_name,
    .send_page_by_id = rover_send_page_by_id,
    .set_latency_reply_flags = rover_set_latency_reply_flags,
    .create_property_bag = rover_create_property_bag,
    .destroy_property_bag = rover_destroy_property_bag,
    .get_field_int = rover_get_field_int,
    .get_field_uint = rover_get_field_uint,
    .get_field_string = rover_get_field_string,
    .set_field_int = rover_set_field_int,
    .set_field_uint = rover_set_field_uint,
    .set_bag_string = rover_set_bag_string,
    .get_first_bag_entry = rover_get_first_bag_entry,
    .get_next_bag_entry = rover_get_next_bag_entry,
    .find_bag_entry = rover_find_bag_entry,
    .get_entry_type = rover_get_entry_type,
    .get_entry_name = rover_get_entry_name,
    .get_entry_int = rover_get_entry_int,
    .get_entry_uint = rover_get_entry_uint,
    .get_entry_string = rover_get_entry_string,
    .set_entry_int = rover_set_entry_int,
    .set_entry_uint = rover_set_entry_uint,
    .set_entry_string = rover_set_entry_string,
    .request_get_profile = rover_request_get_profile,
    .request_set_profile = rover_request_set_profile,
    .locate_player = rover_locate_player,
    .locate_target = rover_locate_target,
    .start_http_login = rover_start_http_login,
    .request_areas = rover_request_areas,
    .create_room = rover_create_room,
    .enumerate_areas = rover_enumerate_areas,
    .find_current_area = rover_find_current_area,
    .match_area_name = rover_match_area_name,
    .enter_metagame = rover_enter_metagame,
    .leave_metagame = rover_leave_metagame,
    .get_house_info = rover_get_house_info,
    .find_house = rover_find_house,
    .set_house_info = rover_set_house_info,
    .create_house = rover_create_house,
    .queue_command = rover_queue_command,
    .register_command_handler = rover_register_command_handler,
    .unregister_command = rover_unregister_command,
};

void ROVER_CALL rover_get_interface(const RoverInterface** out) {
    *out = &kRoverInterface;
}
