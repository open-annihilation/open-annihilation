// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/service.hpp"

#include <cstring>

namespace oa::boneyards {
namespace {

bool offline_network_available(void*) {
    return false;
}

ActivationResult offline_activate(void*) {
    return ActivationResult::module_missing;
}

void offline_close(void*) {
}

bool offline_connected(void*) {
    return false;
}

ServiceResult offline_login(void*, const LoginRequest*) {
    return ServiceResult::not_connected;
}

void offline_cancel_login(void*) {
}

ServiceResult offline_send_chat(void*, uint32_t, uint32_t, uint32_t, const char*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_page_user(void*, const char*, const char*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_locate_user(void*, const char*, LocateCallback, void*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_locate_game(void*, const char*, GameLocateCallback, void*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_matchmaking_look(void*, const MatchmakingQuery*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_matchmaking_join(void*, const MatchmakingQuery*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_matchmaking_cancel(void*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_matchmaking_answer(void*, const char*, int32_t) {
    return ServiceResult::not_connected;
}

bool offline_matchmaking_take(void*, MatchmakingNotice*) {
    return false;
}

ServiceResult offline_quickplay_join(void*, const QuickPlayQuery*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_quickplay_cancel(void*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_quickplay_answer(void*, const char*, int32_t) {
    return ServiceResult::not_connected;
}

bool offline_quickplay_take(void*, QuickPlayNotice*) {
    return false;
}

ServiceResult offline_join_room(void*, uint32_t, uint32_t, const char*, const char*) {
    return ServiceResult::not_connected;
}

ServiceResult
offline_switch_room(void*, uint32_t, uint32_t, const char*, uint32_t, uint32_t, const char*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_request_room_list(void*, uint32_t) {
    return ServiceResult::not_connected;
}

ServiceResult offline_leave_room(void*, uint32_t, uint32_t) {
    return ServiceResult::not_connected;
}

ServiceResult offline_create_room(void*, uint32_t, const char*, const char*, const char*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_request_profile(void*, uint32_t, ProfileCallback, LadderCallback, void*) {
    return ServiceResult::not_connected;
}

ServiceResult
offline_submit_profile(void*, uint32_t, const ProfileEdits*, ProfileUpdatedCallback, void*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_host_game(void*, const HostGameRequest*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_enter_battles(void*, uint32_t) {
    return ServiceResult::not_connected;
}

ServiceResult offline_leave_battles(void*, uint32_t) {
    return ServiceResult::not_connected;
}

ServiceResult offline_get_battle(void*, uint32_t, const char*) {
    return ServiceResult::not_connected;
}

ServiceResult offline_join_game(void*, uint32_t, const char*, const char*) {
    return ServiceResult::not_connected;
}

bool offline_content_server(void*, int32_t, char* host, std::size_t capacity, uint16_t* port) {
    if (host != nullptr && capacity != 0)
        host[0] = '\0';
    if (port != nullptr)
        *port = 0;
    return false;
}

ServiceResult offline_fetch_file(void*, const char*, char* dest, std::size_t capacity) {
    if (dest != nullptr && capacity != 0)
        dest[0] = '\0';
    return ServiceResult::not_connected;
}

bool empty_contains(void*, const char*, uint32_t) {
    return false;
}

void empty_insert(void*, const char*, uint32_t) {
}

void empty_erase(void*, const char*, uint32_t) {
}

void empty_release(void*) {
}

void offline_pump(void*) {
}

void offline_set_events(void*, const ServiceEvents*) {
}

const char* offline_area_name(void*) {
    return nullptr;
}

SectorMapReader* offline_open_sector_map(void*, const char*, SectorMapHeader*) {
    return nullptr;
}

bool offline_read_sector_map(
    void*,
    SectorMapReader*,
    int32_t,
    int32_t,
    SectorPlanetRecord*,
    int32_t,
    ConnectorRecord*,
    int32_t
) {
    return false;
}

void offline_close_sector_map(void*, SectorMapReader*) {
}

void offline_request_allegiance(void*) {
}

void offline_request_census(void*, uint32_t) {
}

const ProfileTemplates* offline_profile_templates(void*) {
    return nullptr;
}

void offline_logout(void*) {
}

bool is_silenced_list(const char* list) {
    return list != nullptr && std::strcmp(list, kSilencedList) == 0;
}

bool state_contains(void* user, const char* list, uint32_t id) {
    const auto* state = static_cast<const OfflineServiceState*>(user);
    if (!is_silenced_list(list))
        return false;
    for (std::size_t i = 0; i < state->silenced_count; ++i)
        if (state->silenced[i] == id)
            return true;
    return false;
}

void state_insert(void* user, const char* list, uint32_t id) {
    auto* state = static_cast<OfflineServiceState*>(user);
    if (!is_silenced_list(list) || state_contains(user, list, id) ||
        state->silenced_count >= kOfflinePeopleCapacity)
        return;
    state->silenced[state->silenced_count++] = id;
}

void state_erase(void* user, const char* list, uint32_t id) {
    auto* state = static_cast<OfflineServiceState*>(user);
    if (!is_silenced_list(list))
        return;
    for (std::size_t i = 0; i < state->silenced_count; ++i) {
        if (state->silenced[i] == id) {
            state->silenced[i] = state->silenced[--state->silenced_count];
            return;
        }
    }
}

} // namespace

void boneyards_service_fill_offline(BoneyardsService* s) {
    if (s->network_available == nullptr)
        s->network_available = offline_network_available;
    if (s->activate == nullptr)
        s->activate = offline_activate;
    if (s->close == nullptr)
        s->close = offline_close;
    if (s->connected == nullptr)
        s->connected = offline_connected;
    if (s->login == nullptr)
        s->login = offline_login;
    if (s->cancel_login == nullptr)
        s->cancel_login = offline_cancel_login;
    if (s->send_chat == nullptr)
        s->send_chat = offline_send_chat;
    if (s->page_user == nullptr)
        s->page_user = offline_page_user;
    if (s->locate_user == nullptr)
        s->locate_user = offline_locate_user;
    if (s->locate_game == nullptr)
        s->locate_game = offline_locate_game;
    if (s->join_room == nullptr)
        s->join_room = offline_join_room;
    if (s->switch_room == nullptr)
        s->switch_room = offline_switch_room;
    if (s->request_room_list == nullptr)
        s->request_room_list = offline_request_room_list;
    if (s->leave_room == nullptr)
        s->leave_room = offline_leave_room;
    if (s->create_room == nullptr)
        s->create_room = offline_create_room;
    if (s->request_profile == nullptr)
        s->request_profile = offline_request_profile;
    if (s->submit_profile == nullptr)
        s->submit_profile = offline_submit_profile;
    if (s->host_game == nullptr)
        s->host_game = offline_host_game;
    if (s->enter_battles == nullptr)
        s->enter_battles = offline_enter_battles;
    if (s->leave_battles == nullptr)
        s->leave_battles = offline_leave_battles;
    if (s->get_battle == nullptr)
        s->get_battle = offline_get_battle;
    if (s->join_game == nullptr)
        s->join_game = offline_join_game;
    if (s->content_server == nullptr)
        s->content_server = offline_content_server;
    if (s->fetch_file == nullptr)
        s->fetch_file = offline_fetch_file;
    if (s->peoplelist_contains == nullptr)
        s->peoplelist_contains = empty_contains;
    if (s->peoplelist_insert == nullptr)
        s->peoplelist_insert = empty_insert;
    if (s->peoplelist_erase == nullptr)
        s->peoplelist_erase = empty_erase;
    if (s->peoplelist_release == nullptr)
        s->peoplelist_release = empty_release;
    if (s->pump == nullptr)
        s->pump = offline_pump;
    if (s->set_events == nullptr)
        s->set_events = offline_set_events;
    if (s->area_name == nullptr)
        s->area_name = offline_area_name;
    if (s->open_sector_map == nullptr)
        s->open_sector_map = offline_open_sector_map;
    if (s->read_sector_map == nullptr)
        s->read_sector_map = offline_read_sector_map;
    if (s->close_sector_map == nullptr)
        s->close_sector_map = offline_close_sector_map;
    if (s->request_allegiance == nullptr)
        s->request_allegiance = offline_request_allegiance;
    if (s->request_census == nullptr)
        s->request_census = offline_request_census;
    if (s->profile_templates == nullptr)
        s->profile_templates = offline_profile_templates;
    if (s->logout == nullptr)
        s->logout = offline_logout;
    if (s->matchmaking_look == nullptr)
        s->matchmaking_look = offline_matchmaking_look;
    if (s->matchmaking_join == nullptr)
        s->matchmaking_join = offline_matchmaking_join;
    if (s->matchmaking_cancel == nullptr)
        s->matchmaking_cancel = offline_matchmaking_cancel;
    if (s->matchmaking_answer == nullptr)
        s->matchmaking_answer = offline_matchmaking_answer;
    if (s->matchmaking_take == nullptr)
        s->matchmaking_take = offline_matchmaking_take;
    if (s->quickplay_join == nullptr)
        s->quickplay_join = offline_quickplay_join;
    if (s->quickplay_cancel == nullptr)
        s->quickplay_cancel = offline_quickplay_cancel;
    if (s->quickplay_answer == nullptr)
        s->quickplay_answer = offline_quickplay_answer;
    if (s->quickplay_take == nullptr)
        s->quickplay_take = offline_quickplay_take;
}

BoneyardsService boneyards_offline_service(OfflineServiceState* state) {
    BoneyardsService s{};
    if (state != nullptr) {
        s.user = state;
        s.peoplelist_contains = state_contains;
        s.peoplelist_insert = state_insert;
        s.peoplelist_erase = state_erase;
    }
    boneyards_service_fill_offline(&s);
    return s;
}

} // namespace oa::boneyards
