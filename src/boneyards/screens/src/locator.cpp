// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Locator searches. A player search sends one locate. A planet search reads
// the war-map copy. A battle search sends one game locate. A frequency
// search asks each planet in turn. Server transfer logs out and, once that
// finishes, logs in again.
#include "oa/boneyards/screens/locator.hpp"

#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/chat_room.hpp"
#include "oa/boneyards/screens/galaxy.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace oa::boneyards::screens {
namespace {

constexpr char kServerDetail[] = "The servers this build knows, and any the yard master lists.\n"
                                 "Relocating logs you out of this server and logs you in to\n"
                                 "the one selected, with the same name and password.";

static_assert(sizeof(kServerDetail) <= kLocatorDetailCapacity, "server detail fits one row");

void copy_field(char* out, std::size_t capacity, const char* text) {
    if (out == nullptr || capacity == 0)
        return;
    if (text == nullptr) {
        out[0] = '\0';
        return;
    }
    utf8::copy(out, capacity, text, 0);
}

void notify(Locator* locator) {
    if (locator->changed != nullptr)
        locator->changed(locator->user);
}

bool accepted(ServiceResult result) {
    return result == ServiceResult::ok || result == ServiceResult::pending;
}

bool contains_ci(const char* haystack, const char* needle) {
    if (needle == nullptr || needle[0] == '\0')
        return true;
    if (haystack == nullptr)
        return false;
    for (const char* start = haystack; *start != '\0'; ++start) {
        const char* left = start;
        const char* right = needle;
        while (*left != '\0' && *right != '\0' &&
               std::tolower(static_cast<unsigned char>(*left)) ==
                   std::tolower(static_cast<unsigned char>(*right))) {
            ++left;
            ++right;
        }
        if (*right == '\0')
            return true;
    }
    return false;
}

bool bright_status(const char* status) {
    return status != nullptr && (std::strstr(status, "forming") != nullptr ||
                                 std::strstr(status, "chatting") != nullptr);
}

void clear_results(Locator* locator) {
    locator->result_count = 0;
    locator->selected = -1;
    locator->page_line[0] = '\0';
}

void stop_listen(Locator* locator) {
    locator->listening_rooms = false;
    locator->listening_battles = false;
    locator->awaiting_intel = false;
    locator->walk = 0;
    locator->quiet_pumps = 0;
    locator->asked_area = 0;
    locator->asked_planet[0] = '\0';
}

const LocatorResult* selected_row(const Locator* locator) {
    if (locator == nullptr || locator->selected < 0 || locator->selected >= locator->result_count)
        return nullptr;
    return &locator->results[locator->selected];
}

LocatorResult* add_row(Locator* locator) {
    if (locator->result_count >= kLocatorResultCapacity)
        return nullptr;
    LocatorResult* row = &locator->results[locator->result_count];
    *row = {};
    ++locator->result_count;
    return row;
}

// The war map's name for a planet, or "" when the copy does not hold it.
const char* planet_name(const Locator* locator, uint32_t area_id) {
    for (int32_t i = 0; i < locator->planet_count; ++i) {
        if (locator->planets[i].id == area_id)
            return locator->planets[i].name;
    }
    return "";
}

const char* side_name(char side) {
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(side)));
    if (letter == 'A')
        return "Arm";
    if (letter == 'C')
        return "Core";
    return "Independent";
}

const char* source_name(int32_t source) {
    switch (source) {
    case static_cast<int32_t>(MapSource::ta_disk):
        return "Total Annihilation";
    case static_cast<int32_t>(MapSource::core_contingency):
    case static_cast<int32_t>(MapSource::web_core_contingency):
        return "Core Contingency";
    case static_cast<int32_t>(MapSource::battle_tactics):
    case static_cast<int32_t>(MapSource::web_battle_tactics):
        return "Battle Tactics";
    case static_cast<int32_t>(MapSource::web_ta):
        return "a downloaded map";
    default:
        return "Total Annihilation";
    }
}

const char* mode_name(int32_t type) {
    switch (type) {
    case 0:
        return "For Fun";
    case 1:
        return "Duel";
    case 2:
        return "Winner Takes All";
    case 4:
        return "Galactic War";
    case 7:
        return "War Training";
    default:
        return "Unknown";
    }
}

const char* skill_name(int32_t skill) {
    switch (skill) {
    case 0:
        return "Novice";
    case 1:
        return "Seasoned";
    case 2:
        return "Expert";
    default:
        return "Unknown";
    }
}

const char* player_status(int32_t status) {
    switch (status) {
    case static_cast<int32_t>(LocateStatus::chatting):
        return "chatting";
    case static_cast<int32_t>(LocateStatus::examining_battles):
        return "examining battles";
    case static_cast<int32_t>(LocateStatus::in_battle):
        return "engaged in battle";
    case static_cast<int32_t>(LocateStatus::other_server):
        return "other server";
    case static_cast<int32_t>(LocateStatus::logged_out):
        return "not logged in";
    case static_cast<int32_t>(LocateStatus::no_such_user):
        return "no such player";
    case static_cast<int32_t>(LocateStatus::viewing_pages):
        return "viewing pages";
    default:
        return "locator error";
    }
}

void finish_walk(Locator* locator);

void ask_intel(Locator* locator) {
    locator->awaiting_intel = false;
    if (locator->kind != locator_kind::battle)
        return;
    const LocatorResult* row = selected_row(locator);
    if (row == nullptr || row->game_id[0] == '\0' || locator->service == nullptr ||
        locator->service->get_battle == nullptr)
        return;
    const int32_t at = locator->selected;
    locator->awaiting_intel = true;
    const ServiceResult result =
        locator->service->get_battle(locator->service->user, row->area_id, row->game_id);
    if (locator->selected != at)
        return;
    if (!accepted(result))
        locator->awaiting_intel = false;
}

void request_next(Locator* locator) {
    if (!locator->listening_rooms)
        return;
    while (locator->walk < locator->planet_count) {
        const LocatorPlanet& planet = locator->planets[locator->walk];
        const int32_t at = locator->walk;
        locator->asked_area = planet.id;
        copy_field(locator->asked_planet, sizeof locator->asked_planet, planet.name);
        locator->quiet_pumps = 0;
        ServiceResult result = ServiceResult::failed;
        if (locator->service != nullptr && locator->service->request_room_list != nullptr)
            result = locator->service->request_room_list(locator->service->user, planet.id);
        if (locator->walk != at || !locator->listening_rooms)
            return;
        if (accepted(result))
            return;
        ++locator->walk;
    }
    finish_walk(locator);
}

void finish_walk(Locator* locator) {
    locator->listening_rooms = false;
    locator->listening_battles = false;
    locator->quiet_pumps = 0;
    if (locator->result_count > 0 && locator->selected < 0)
        locator_select(locator, 0);
    else
        notify(locator);
}

void on_locate_reply(void* context, const LocateReport* report) {
    locator_on_located(static_cast<Locator*>(context), report);
}

void search_player(Locator* locator) {
    clear_results(locator);
    stop_listen(locator);
    if (locator->query[0] == '\0' || locator->service == nullptr ||
        locator->service->locate_user == nullptr) {
        notify(locator);
        return;
    }
    (void)locator->service->locate_user(
        locator->service->user, locator->query, on_locate_reply, locator
    );
    notify(locator);
}

void search_planets(Locator* locator) {
    clear_results(locator);
    stop_listen(locator);
    for (int32_t i = 0; i < locator->planet_count; ++i) {
        const LocatorPlanet& planet = locator->planets[i];
        if (!contains_ci(planet.name, locator->query))
            continue;
        LocatorResult* row = add_row(locator);
        if (row == nullptr)
            break;
        copy_field(row->name, sizeof row->name, planet.name);
        copy_field(row->where, sizeof row->where, side_name(planet.side));
        std::snprintf(
            row->status,
            sizeof row->status,
            "%d battles, %d forming",
            planet.in_progress,
            planet.forming
        );
        row->bright = bright_status(row->status);
        std::snprintf(
            row->detail,
            sizeof row->detail,
            "%s is held by the %s. TA map %s (%s).",
            planet.name,
            side_name(planet.side),
            planet.ta_map,
            source_name(planet.map_source)
        );
        row->area_id = planet.id;
    }
    if (locator->result_count > 0)
        locator->selected = 0;
    notify(locator);
}

void on_game_reply(void* context, const GameLocateReport* report) {
    locator_on_game_located(static_cast<Locator*>(context), report);
}

void search_battles(Locator* locator) {
    clear_results(locator);
    stop_listen(locator);
    if (locator->query[0] == '\0' || locator->service == nullptr ||
        locator->service->locate_game == nullptr) {
        notify(locator);
        return;
    }
    locator->listening_battles = true;
    const ServiceResult result = locator->service->locate_game(
        locator->service->user, locator->query, on_game_reply, locator
    );
    if (!accepted(result))
        locator->listening_battles = false;
    notify(locator);
}

void search_frequencies(Locator* locator) {
    clear_results(locator);
    stop_listen(locator);
    locator->listening_rooms = true;
    locator->walk = 0;
    request_next(locator);
    if (locator->listening_rooms)
        notify(locator);
}

// The row of a frequency already listed under another planet with the same
// id and name, or null.
LocatorResult*
same_frequency(Locator* locator, uint32_t area_id, uint32_t room_id, const char* name) {
    for (int32_t i = 0; i < locator->result_count; ++i) {
        LocatorResult& row = locator->results[i];
        if (row.room_id == room_id && row.area_id != area_id && std::strcmp(row.name, name) == 0)
            return &row;
    }
    return nullptr;
}

void fill_frequency(Locator* locator, LocatorResult* row, const char* name, bool sector_wide) {
    copy_field(row->name, sizeof row->name, name);
    copy_field(
        row->where, sizeof row->where, sector_wide ? kLocatorEveryPlanet : locator->asked_planet
    );
    row->every_planet = sector_wide;
}

void frequency_detail(LocatorResult* row) {
    const char* made = row->battle_type == room_type::user_created ? "user-created" : "system";
    const char* lock = row->password ? "password required" : "no password";
    // A long name and planet are cut at the detail's end; the line is only shown.
    const int written = std::snprintf(
        row->detail, sizeof row->detail, "%s on %s: %s, %s.", row->name, row->where, made, lock
    );
    if (written < 0)
        row->detail[0] = '\0';
}

void take_server(void* context, const char* location) {
    auto* locator = static_cast<Locator*>(context);
    if (location == nullptr)
        return;
    LocatorResult* row = add_row(locator);
    if (row == nullptr)
        return;
    copy_field(row->name, sizeof row->name, location);
    copy_field(row->server, sizeof row->server, location);
    const char* current = locator->service != nullptr && locator->service->current_server != nullptr
                              ? locator->service->current_server(locator->service->user)
                              : nullptr;
    row->current_server = current != nullptr && std::strcmp(current, location) == 0;
    if (row->current_server)
        copy_field(row->status, sizeof row->status, "current server");
    if (locator->service != nullptr && locator->service->server_region != nullptr) {
        const char* region = locator->service->server_region(locator->service->user, location);
        if (region != nullptr && region[0] != '\0')
            copy_field(row->where, sizeof row->where, region);
    }
    copy_field(row->detail, sizeof row->detail, kServerDetail);
}

void search_servers(Locator* locator) {
    clear_results(locator);
    stop_listen(locator);
    if (locator->service != nullptr && locator->service->list_servers != nullptr)
        locator->service->list_servers(locator->service->user, take_server, locator);
    if (locator->result_count > 0)
        locator->selected = 0;
    notify(locator);
}

} // namespace

void locator_init(Locator* locator, BoneyardsService* service) {
    if (locator == nullptr)
        return;
    *locator = Locator{};
    locator->service = service;
    locator->kind = locator_kind::player;
    locator->selected = -1;
}

void locator_set_planets(Locator* locator, const LocatorPlanet* planets, int32_t count) {
    if (locator == nullptr)
        return;
    locator->planet_count = 0;
    if (planets == nullptr || count <= 0)
        return;
    if (count > kLocatorPlanetCapacity)
        count = kLocatorPlanetCapacity;
    for (int32_t i = 0; i < count; ++i)
        locator->planets[i] = planets[i];
    locator->planet_count = count;
}

void locator_set_identity(Locator* locator, const char* user_name, const char* password) {
    if (locator == nullptr)
        return;
    utf8::copy(
        locator->session_name, sizeof locator->session_name, user_name, kPlayerNameCharacters
    );
    utf8::copy(
        locator->session_password, sizeof locator->session_password, password, kPlayerNameCharacters
    );
}

void locator_set_here(Locator* locator, uint32_t area_id, uint32_t room_id) {
    if (locator == nullptr)
        return;
    locator->here_area = area_id;
    locator->here_room = room_id;
}

void locator_set_kind(Locator* locator, int32_t kind) {
    if (locator == nullptr)
        return;
    if (kind < 0 || kind >= locator_kind::count)
        kind = locator_kind::player;
    locator->kind = kind;
    clear_results(locator);
    stop_listen(locator);
    if (kind == locator_kind::server)
        search_servers(locator);
    else
        notify(locator);
}

void locator_set_query(Locator* locator, const char* query) {
    if (locator == nullptr)
        return;
    copy_field(locator->query, sizeof locator->query, query);
}

void locator_search(Locator* locator) {
    if (locator == nullptr)
        return;
    switch (locator->kind) {
    case locator_kind::planet:
        search_planets(locator);
        break;
    case locator_kind::battle:
        search_battles(locator);
        break;
    case locator_kind::frequency:
        search_frequencies(locator);
        break;
    case locator_kind::server:
        search_servers(locator);
        break;
    default:
        search_player(locator);
        break;
    }
}

void locator_select(Locator* locator, int32_t index) {
    if (locator == nullptr || index < 0 || index >= locator->result_count)
        return;
    locator->selected = index;
    if (locator->kind == locator_kind::battle)
        ask_intel(locator);
    notify(locator);
}

bool locator_relocate_enabled(const Locator* locator) {
    const LocatorResult* row = selected_row(locator);
    if (row == nullptr || locator->transfer_pending)
        return false;
    switch (locator->kind) {
    case locator_kind::server:
        return !row->current_server;
    case locator_kind::player:
    case locator_kind::frequency:
        return row->room_id != 0;
    default:
        return true;
    }
}

bool locator_extra_enabled(const Locator* locator) {
    const LocatorResult* row = selected_row(locator);
    if (row == nullptr)
        return false;
    if (locator->kind == locator_kind::player)
        return row->name[0] != '\0';
    if (locator->kind == locator_kind::battle)
        return row->game_id[0] != '\0';
    return false;
}

void locator_relocate(Locator* locator) {
    if (!locator_relocate_enabled(locator))
        return;
    const LocatorResult* row = selected_row(locator);
    BoneyardsService* service = locator->service;
    switch (locator->kind) {
    case locator_kind::player:
    case locator_kind::frequency: {
        // A frequency every planet lists is entered from the planet the
        // player is on, so the move does not change planets.
        const uint32_t area =
            row->every_planet && locator->here_area != 0 ? locator->here_area : row->area_id;
        if (service != nullptr && service->switch_room != nullptr) {
            const char* room_name =
                locator->kind == locator_kind::player ? row->game_id : row->name;
            service->switch_room(
                service->user,
                area,
                row->room_id,
                nullptr,
                locator->here_area,
                locator->here_room,
                room_name
            );
        }
        if (locator->open_chat != nullptr)
            locator->open_chat(locator->user, area, row->room_id, "");
        break;
    }
    case locator_kind::planet:
        if (service != nullptr && service->request_census != nullptr)
            service->request_census(service->user, row->area_id);
        if (locator->open_planet != nullptr)
            locator->open_planet(locator->user, row->area_id);
        break;
    case locator_kind::battle:
        if (locator->open_battle != nullptr)
            locator->open_battle(locator->user, row->area_id);
        break;
    case locator_kind::server:
        copy_field(locator->transfer_server, sizeof locator->transfer_server, row->server);
        locator->transfer_pending = true;
        notify(locator);
        if (service != nullptr && service->logout != nullptr)
            service->logout(service->user);
        break;
    default:
        break;
    }
}

void locator_extra(Locator* locator) {
    if (!locator_extra_enabled(locator))
        return;
    const LocatorResult* row = selected_row(locator);
    if (locator->kind == locator_kind::player) {
        std::snprintf(locator->page_line, sizeof locator->page_line, "+page %s ", row->name);
        if (locator->open_chat != nullptr)
            locator->open_chat(locator->user, 0, 0, locator->page_line);
        return;
    }
    if (locator->kind == locator_kind::battle) {
        // A password battle opens Battle Intelligence first. Joining with an
        // empty password would be the rejection this screen exists to avoid.
        if (!row->password && locator->service != nullptr &&
            locator->service->join_game != nullptr) {
            locator->service->join_game(
                locator->service->user, row->area_id, row->game_id, nullptr
            );
        }
        if (locator->open_join != nullptr)
            locator->open_join(locator->user, row->area_id, row->game_id, row->password);
    }
}

void locator_pump(Locator* locator) {
    if (locator == nullptr || (!locator->listening_rooms && !locator->listening_battles))
        return;
    ++locator->quiet_pumps;
    if (locator->quiet_pumps < kLocatorAnswerWaitPulses)
        return;
    if (locator->listening_battles) {
        finish_walk(locator);
        return;
    }
    ++locator->walk;
    request_next(locator);
}

void locator_on_located(Locator* locator, const LocateReport* report) {
    if (locator == nullptr || report == nullptr || locator->kind != locator_kind::player)
        return;
    clear_results(locator);
    LocatorResult* row = add_row(locator);
    if (row == nullptr)
        return;
    copy_field(row->name, sizeof row->name, report->user_name);
    copy_field(row->status, sizeof row->status, player_status(report->status));
    row->bright = bright_status(row->status);
    row->area_id = report->area_id;
    row->room_id = report->room_id;
    const int32_t status = report->status;
    if (status == static_cast<int32_t>(LocateStatus::chatting)) {
        // The two names together can be longer than the row. Build them
        // where they fit, then keep the prefix the row can show.
        char joined[kLocateTextCapacity * 2 + 3];
        std::snprintf(joined, sizeof joined, "%s / %s", report->area_name, report->room_name);
        copy_field(row->where, sizeof row->where, joined);
        // The frequency name a relocate enters. The row's own name is the player.
        copy_field(row->game_id, sizeof row->game_id, report->room_name);
    } else if (
        status == static_cast<int32_t>(LocateStatus::examining_battles) ||
        status == static_cast<int32_t>(LocateStatus::in_battle)
    ) {
        copy_field(row->where, sizeof row->where, report->area_name);
    } else if (status == static_cast<int32_t>(LocateStatus::other_server)) {
        copy_field(row->where, sizeof row->where, report->server_name);
    }
    chat_format_locate_result(report, row->detail, sizeof row->detail);
    locator->selected = 0;
    notify(locator);
}

void locator_on_room(
    Locator* locator,
    int32_t change,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    bool needs_password,
    int32_t users,
    int32_t type,
    bool last,
    bool global_known,
    int32_t global
) {
    if (locator == nullptr || !locator->listening_rooms || area_id != locator->asked_area)
        return;
    locator->quiet_pumps = 0;
    if (change == 0 && name != nullptr && contains_ci(name, locator->query)) {
        const bool sector_wide = global_known && global == 1;
        LocatorResult* shared = nullptr;
        if (!global_known || sector_wide)
            shared = same_frequency(locator, area_id, room_id, name);
        if (shared != nullptr) {
            if (!shared->every_planet) {
                shared->every_planet = true;
                copy_field(shared->where, sizeof shared->where, kLocatorEveryPlanet);
                frequency_detail(shared);
                notify(locator);
            }
        } else if (LocatorResult* row = add_row(locator); row != nullptr) {
            fill_frequency(locator, row, name, sector_wide);
            std::snprintf(row->status, sizeof row->status, "%d commanders", users);
            row->area_id = area_id;
            row->room_id = room_id;
            row->password = needs_password;
            row->battle_type = type;
            frequency_detail(row);
            notify(locator);
        }
    }
    if (last) {
        ++locator->walk;
        request_next(locator);
    }
}

void locator_on_game_located(Locator* locator, const GameLocateReport* report) {
    if (locator == nullptr || report == nullptr || !locator->listening_battles)
        return;
    locator->quiet_pumps = 0;
    if (report->game_id[0] != '\0') {
        if (LocatorResult* row = add_row(locator); row != nullptr) {
            copy_field(row->name, sizeof row->name, report->name);
            copy_field(row->where, sizeof row->where, planet_name(locator, report->area_id));
            copy_field(row->game_id, sizeof row->game_id, report->game_id);
            if (report->forming && report->seats_known) {
                std::snprintf(
                    row->status,
                    sizeof row->status,
                    "forming %d/%d",
                    static_cast<int>(report->players),
                    static_cast<int>(report->playerlimit)
                );
            } else {
                copy_field(
                    row->status, sizeof row->status, report->forming ? "forming" : "in progress"
                );
            }
            row->bright = bright_status(row->status);
            if (row->where[0] != '\0')
                std::snprintf(
                    row->detail, sizeof row->detail, "%s on %s.", report->name, row->where
                );
            else
                std::snprintf(row->detail, sizeof row->detail, "%s.", report->name);
            row->area_id = report->area_id;
            row->battle_type = report->type;
            row->password = report->pass_required;
            notify(locator);
        }
    }
    if (report->last)
        finish_walk(locator);
}

void locator_on_intel(
    Locator* locator, const char* game_id, const char* host_name, int32_t skill, bool failed
) {
    if (locator == nullptr || !locator->awaiting_intel)
        return;
    locator->awaiting_intel = false;
    if (failed || game_id == nullptr)
        return;
    if (locator->selected < 0 || locator->selected >= locator->result_count)
        return;
    LocatorResult* row = &locator->results[locator->selected];
    if (std::strcmp(row->game_id, game_id) != 0)
        return;
    const char* host = host_name != nullptr && host_name[0] != '\0' ? host_name : "unknown";
    // The name is copied out first: it sits in the same row as the detail.
    char name[kLocatorNameCapacity];
    copy_field(name, sizeof name, row->name);
    std::snprintf(
        row->detail,
        sizeof row->detail,
        "%s: %s, %s, hosted by %s.",
        name,
        mode_name(row->battle_type),
        skill_name(skill),
        host
    );
    notify(locator);
}

void locator_on_login_reset(Locator* locator) {
    if (locator == nullptr || !locator->transfer_pending)
        return;
    locator->transfer_pending = false;
    if (locator->service != nullptr && locator->service->login != nullptr) {
        LoginRequest request{};
        request.user_name = locator->session_name;
        request.password = locator->session_password;
        request.register_new = false;
        request.server_name = locator->transfer_server;
        locator->service->login(locator->service->user, &request);
    }
    notify(locator);
}

int32_t locator_kind_of(const Locator* locator) {
    return locator != nullptr ? locator->kind : locator_kind::player;
}

const char* locator_kind_name(int32_t kind) {
    switch (kind) {
    case locator_kind::planet:
        return "Locate Planet";
    case locator_kind::battle:
        return "Find Battle";
    case locator_kind::frequency:
        return "Find Frequency";
    case locator_kind::server:
        return "Server Transfer";
    default:
        return "Locate Player";
    }
}

const char* locator_field_label(int32_t kind) {
    switch (kind) {
    case locator_kind::planet:
        return "Planet Name";
    case locator_kind::battle:
        return "Battle Name";
    case locator_kind::frequency:
        return "Frequency";
    case locator_kind::server:
        return nullptr;
    default:
        return "Player Name";
    }
}

bool locator_shows_field(int32_t kind) {
    return kind != locator_kind::server;
}

bool locator_shows_page(int32_t kind) {
    return kind == locator_kind::player;
}

bool locator_shows_join(int32_t kind) {
    return kind == locator_kind::battle;
}

int32_t locator_result_count(const Locator* locator) {
    return locator != nullptr ? locator->result_count : 0;
}

const LocatorResult* locator_result(const Locator* locator, int32_t index) {
    if (locator == nullptr || index < 0 || index >= locator->result_count)
        return nullptr;
    return &locator->results[index];
}

int32_t locator_selected(const Locator* locator) {
    return locator != nullptr ? locator->selected : -1;
}

const char* locator_detail(const Locator* locator) {
    const LocatorResult* row = selected_row(locator);
    return row != nullptr ? row->detail : "";
}

bool locator_listening_rooms(const Locator* locator) {
    return locator != nullptr && locator->listening_rooms;
}

bool locator_listening_battles(const Locator* locator) {
    return locator != nullptr && locator->listening_battles;
}

bool locator_awaiting_intel(const Locator* locator) {
    return locator != nullptr && locator->awaiting_intel;
}

} // namespace oa::boneyards::screens
