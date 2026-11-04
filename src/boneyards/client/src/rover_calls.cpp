// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Rover table binding, its lock and the per-slot wrappers.
#include "oa/boneyards/rover_calls.hpp"

#include "oa/boneyards/rover_string.hpp"

#include <cstring>
#include <mutex>

namespace oa::boneyards {
namespace {

// The interface lock, recursive as in 3.1c.
std::recursive_mutex g_interface_lock;
RoverModuleState g_module{};
RoverGetInterfaceFn g_interface_source = rover_get_interface;

// One wrapper call: the lock is held and the call counted until it returns.
struct TableCall {
    TableCall() : hold(g_interface_lock) { ++g_module.call_depth; }

    ~TableCall() { --g_module.call_depth; }

    TableCall(const TableCall&) = delete;
    TableCall& operator=(const TableCall&) = delete;
    std::lock_guard<std::recursive_mutex> hold;
};

// Builds the temporary string object a keyed bag call passes for its key.
RoverString key_object(const char* key) {
    RoverString object{};
    rover_string_assign(&object, key, static_cast<uint32_t>(std::strlen(key)));
    return object;
}

} // namespace

void rover_module_set_source(RoverGetInterfaceFn get_interface) {
    std::lock_guard<std::recursive_mutex> hold(g_interface_lock);
    g_interface_source = get_interface;
}

const RoverModuleState* rover_module_state() {
    return &g_module;
}

bool rover_module_load() {
    std::lock_guard<std::recursive_mutex> hold(g_interface_lock);
    g_module.unload_deferred = 0;
    g_module.bound = g_interface_source != nullptr;
    if (!g_module.bound)
        return false;
    g_interface_source(&g_module.table);
    if (!rover_call_check_version(kRoverInterfaceVersion, kRoverBuildStamp)) {
        g_module.bound = false;
        g_module.table = nullptr;
        return false;
    }
    return true;
}

void rover_module_close() {
    std::lock_guard<std::recursive_mutex> hold(g_interface_lock);
    if (!g_module.bound)
        return;
    if (g_module.detach_on_close != 0)
        rover_call_detach();
    if (g_module.call_depth < 1)
        rover_module_unload();
    else
        g_module.unload_deferred = 1;
}

void rover_module_unload() {
    std::lock_guard<std::recursive_mutex> hold(g_interface_lock);
    if (!g_module.bound)
        return;
    g_module.table = nullptr;
    g_module.bound = false;
}

bool rover_call_check_version(int32_t iface_version, int32_t build_stamp) {
    TableCall call;
    if (g_module.table == nullptr)
        return false;
    return static_cast<uint8_t>(g_module.table->check_version(iface_version, build_stamp)) != 0;
}

bool rover_call_attach() {
    TableCall call;
    return g_module.table != nullptr && g_module.table->attach();
}

void rover_call_detach() {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->detach();
}

bool rover_call_activate() {
    TableCall call;
    return g_module.table != nullptr && g_module.table->activate();
}

void rover_call_deactivate() {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->deactivate();
}

void rover_call_pump_events() {
    std::lock_guard<std::recursive_mutex> hold(g_interface_lock);
    ++g_module.call_depth;
    if (g_module.table != nullptr)
        g_module.table->pump_events();
    --g_module.call_depth;
    if (g_module.unload_deferred != 0) {
        rover_module_unload();
        g_module.unload_deferred = 0;
    }
}

void rover_call_reset_session() {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->reset_session();
}

bool rover_call_start_login(
    const char* server_name,
    const char* username,
    const char* password,
    RoverLoginProgressFn progress,
    RoverSystemAlertFn system_alert,
    RoverDisconnectFn disconnect
) {
    TableCall call;
    return g_module.table != nullptr &&
           g_module.table->start_login(
               server_name, username, password, progress, system_alert, disconnect
           );
}

bool rover_call_send_logout(int32_t user_id, RoverLogoutFn logged_out) {
    TableCall call;
    return g_module.table != nullptr && g_module.table->send_logout(user_id, logged_out);
}

bool rover_call_get_rooms(int32_t area_id, RoverRoomListFn room_list) {
    TableCall call;
    return g_module.table != nullptr && g_module.table->get_rooms(area_id, room_list);
}

bool rover_call_enter_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
) {
    TableCall call;
    return g_module.table != nullptr &&
           g_module.table->enter_room(
               room_id, area_id, password, entered, room_info, room_list, chat_msg
           );
}

bool rover_call_switch_room(
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
    TableCall call;
    return g_module.table != nullptr && g_module.table->switch_room(
                                            room_id,
                                            area_id,
                                            password,
                                            old_room_id,
                                            old_area_id,
                                            entered,
                                            room_info,
                                            room_list,
                                            chat_msg
                                        );
}

bool rover_call_leave_room(int32_t room_id, int32_t area_id, RoverRoomLeftFn left) {
    TableCall call;
    return g_module.table != nullptr && g_module.table->leave_room(room_id, area_id, left);
}

bool rover_call_send_chat(
    int32_t room_id, int32_t area_id, int32_t target_user_id, const char* message
) {
    TableCall call;
    return g_module.table != nullptr &&
           g_module.table->send_chat(room_id, area_id, target_user_id, message);
}

bool rover_call_enter_battles(
    uint32_t area_id,
    RoverBattlesDoneFn entered,
    RoverBattleInfoFn battle_info,
    RoverIntelMsgFn intel_msg
) {
    TableCall call;
    return g_module.table != nullptr &&
           g_module.table->enter_battles(area_id, entered, battle_info, intel_msg);
}

bool rover_call_leave_battles(uint32_t area_id, RoverBattlesDoneFn left) {
    TableCall call;
    return g_module.table != nullptr && g_module.table->leave_battles(area_id, left);
}

bool rover_call_get_battle(
    uint32_t area_id,
    const char* game_id,
    RoverBattleIntelFn battle_intel,
    RoverBattleReportFn battle_report
) {
    TableCall call;
    return g_module.table != nullptr &&
           g_module.table->get_battle(area_id, game_id, battle_intel, battle_report);
}

bool rover_call_host_game(
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
    TableCall call;
    return g_module.table != nullptr && g_module.table->host_game(
                                            area_id,
                                            game_type,
                                            name,
                                            password,
                                            description,
                                            keyword,
                                            skill_sel,
                                            players_sel,
                                            on_death_sel,
                                            launch,
                                            debriefing
                                        );
}

bool rover_call_join_game(
    uint32_t area_id,
    const char* game_id,
    const char* password,
    RoverLaunchGameFn launch,
    RoverDebriefingFn debriefing
) {
    TableCall call;
    return g_module.table != nullptr &&
           g_module.table->join_game(area_id, game_id, password, launch, debriefing);
}

bool rover_call_send_left_game(uint32_t area_id, const char* game_id) {
    TableCall call;
    return g_module.table != nullptr && g_module.table->send_left_game(area_id, game_id);
}

bool rover_call_send_game_settings(
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
    TableCall call;
    return g_module.table != nullptr && g_module.table->send_game_settings(
                                            area_id,
                                            game_id,
                                            lock_options,
                                            mission,
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
                                            tournament
                                        );
}

bool rover_call_send_player_info(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    const char* name,
    const char* allegiance,
    int32_t team
) {
    TableCall call;
    return g_module.table != nullptr &&
           g_module.table->send_player_info(area_id, game_id, dpid, name, allegiance, team);
}

bool rover_call_send_score_report(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    int32_t kills,
    int32_t losses,
    int32_t energy,
    int32_t metal,
    int32_t xs_energy,
    int32_t xs_metal,
    int32_t ignored_9,
    int32_t ignored_10,
    int32_t score,
    bool victory,
    bool last
) {
    TableCall call;
    return g_module.table != nullptr && g_module.table->send_score_report(
                                            area_id,
                                            game_id,
                                            dpid,
                                            kills,
                                            losses,
                                            energy,
                                            metal,
                                            xs_energy,
                                            xs_metal,
                                            ignored_9,
                                            ignored_10,
                                            score,
                                            victory,
                                            last
                                        );
}

bool rover_call_send_score_report_message(
    uint32_t area_id, const char* game_id, uint32_t dpid, const char* message
) {
    TableCall call;
    return g_module.table != nullptr &&
           g_module.table->send_score_report_message(area_id, game_id, dpid, message);
}

void rover_call_fetch_http_file(
    const char* url,
    const char* dest_path,
    RoverHttpCompleteFn complete,
    RoverHttpProgressFn progress
) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->fetch_http_file(url, dest_path, complete, progress);
}

bool rover_call_parse_record_file(const char* path, RoverParsedRecordFn record, void* user) {
    TableCall call;
    return g_module.table != nullptr && g_module.table->parse_record_file(path, record, user);
}

RoverSectorReader* rover_call_open_sector_map(
    const char* path,
    int32_t* out_sector_id,
    char* out_sector_name,
    int32_t* out_width,
    int32_t* out_height,
    int32_t* out_planet_count,
    int32_t* out_connector_count
) {
    TableCall call;
    if (g_module.table == nullptr)
        return nullptr;
    return g_module.table->open_sector_map(
        path,
        out_sector_id,
        out_sector_name,
        out_width,
        out_height,
        out_planet_count,
        out_connector_count
    );
}

bool rover_call_read_sector_planets(
    RoverSectorReader* reader,
    int32_t sector_width,
    int32_t sector_height,
    RoverSectorPlanet* out_planets,
    int32_t planet_count,
    RoverSectorConnector* out_connectors,
    int32_t connector_count
) {
    TableCall call;
    return g_module.table != nullptr && g_module.table->read_sector_planets(
                                            reader,
                                            sector_width,
                                            sector_height,
                                            out_planets,
                                            planet_count,
                                            out_connectors,
                                            connector_count
                                        );
}

void rover_call_close_sector_map(RoverSectorReader* reader) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->close_sector_map(reader);
}

void rover_call_request_census(uint32_t area_id, RoverCensusFn census, RoverActivityFn activity) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->request_census(area_id, census, activity);
}

void rover_call_request_allegiance(RoverAllegianceFn allegiances) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->request_allegiance(allegiances);
}

void rover_call_get_file_server(int32_t server_type, RoverFileServer* out_server) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->get_file_server(server_type, out_server);
}

void rover_call_send_page_by_name(const char* user_name, const char* message) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->send_page_by_name(user_name, message);
}

void rover_call_set_latency_reply_flags(uint32_t flags) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->set_latency_reply_flags(flags);
}

RoverPropertyBag* rover_call_create_property_bag() {
    TableCall call;
    return g_module.table != nullptr ? g_module.table->create_property_bag() : nullptr;
}

void rover_call_destroy_property_bag(RoverPropertyBag* bag) {
    if (bag == nullptr)
        return;
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->destroy_property_bag(bag);
}

bool rover_call_get_field_int(RoverPropertyBag* bag, const char* key, int32_t* out_value) {
    TableCall call;
    if (g_module.table == nullptr)
        return false;
    RoverString key_string = key_object(key);
    const bool found = g_module.table->get_field_int(bag, &key_string, out_value);
    rover_string_release(&key_string, true);
    return found;
}

bool rover_call_get_field_uint(RoverPropertyBag* bag, const char* key, uint32_t* out_value) {
    TableCall call;
    if (g_module.table == nullptr)
        return false;
    RoverString key_string = key_object(key);
    const bool found = g_module.table->get_field_uint(bag, &key_string, out_value);
    rover_string_release(&key_string, true);
    return found;
}

bool rover_call_get_field_string(RoverPropertyBag* bag, const char* key, RoverString* out_value) {
    TableCall call;
    if (g_module.table == nullptr)
        return false;
    RoverString key_string = key_object(key);
    const bool found = g_module.table->get_field_string(bag, &key_string, out_value);
    rover_string_release(&key_string, true);
    return found;
}

bool rover_call_set_bag_string(RoverPropertyBag* bag, const char* key, const RoverString* value) {
    TableCall call;
    if (g_module.table == nullptr)
        return false;
    RoverString key_string = key_object(key);
    const bool stored = g_module.table->set_bag_string(bag, &key_string, value);
    rover_string_release(&key_string, true);
    return stored;
}

RoverBagEntry* rover_call_get_first_bag_entry(RoverPropertyBag* bag) {
    TableCall call;
    return g_module.table != nullptr ? g_module.table->get_first_bag_entry(bag) : nullptr;
}

void rover_call_request_get_profile(
    uint32_t user_id, RoverProfileFn profile, RoverLadderProfileFn ladder_profile
) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->request_get_profile(user_id, profile, ladder_profile);
}

void rover_call_request_set_profile(
    uint32_t user_id, RoverPropertyBag* properties, RoverProfileUpdatedFn updated
) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->request_set_profile(user_id, properties, updated);
}

void rover_call_locate_player(const char* username, RoverLocateFn located) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->locate_player(username, located);
}

void rover_call_locate_game(const char* name, RoverGameLocatedFn located) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->locate_target(name, located);
}

void rover_call_create_room(
    uint32_t area_id,
    const char* room_name,
    const char* description,
    const char* password,
    RoverRoomCreatedFn created
) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->create_room(area_id, room_name, description, password, created);
}

void rover_call_enumerate_areas(RoverAreaEntryFn location_fn) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->enumerate_areas(location_fn);
}

void rover_call_find_current_area(const char** out_location) {
    TableCall call;
    if (g_module.table != nullptr)
        g_module.table->find_current_area(out_location);
}

const char* rover_call_match_area_name() {
    TableCall call;
    return g_module.table != nullptr ? g_module.table->match_area_name() : nullptr;
}

} // namespace oa::boneyards
