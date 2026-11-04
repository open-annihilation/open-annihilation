// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/navigator.hpp"

#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/screens/rand15.hpp"
#include "text.hpp"

#include <cstring>
#include <type_traits>

namespace oa::boneyards::screens {
namespace {

// The account flags byte is the top byte of the user id.
constexpr int kAccountFlagsShift = 24;

// Keyboard dismissal does not hand focus back by itself; do it here, unless
// the window has closed while the box was up.
void restore_focus(void*, MessageBox* box) {
    Desktop* desktop = services().desktop;
    if (desktop_holds(desktop, box->focus_return) && desktop->focus == &box->base)
        desktop_set_focus(desktop, box->focus_return);
}

// Screen message boxes: recorded for inspection and, while a desktop is
// installed, shown as a Boneyards message box with an Ok button that holds
// the keyboard until it is closed.
void record_message(void* user, const char* text) {
    auto* s = static_cast<BoneyardsScreens*>(user);
    copy_bounded(s->last_message, sizeof s->last_message, text);
    ++s->message_count;
    Desktop* desktop = services().desktop;
    if (desktop == nullptr)
        return;
    MessageBox* box =
        message_box_create(text, message_box_buttons::ok, nullptr, nullptr, desktop->focus);
    box->on_closed = restore_focus;
    desktop_set_focus(desktop, &box->base);
}

/// Draws the chat screen's default frequency from the shared rand15 stream.
///
/// @return the next value of the stream, 0..kRand15Max
uint32_t chat_random(void*) {
    return static_cast<uint32_t>(rand15());
}

// The consoles' activity light is the Main Frame's.
void frame_activity_light(void* user, bool running) {
    const ScreensHost& host = static_cast<BoneyardsScreens*>(user)->host;
    if (host.activity_light != nullptr)
        host.activity_light(host.user, running);
}

// The Host panel opening readies its game setup.
void open_host_panel(void* user) {
    boneyards_prepare_game_setup(static_cast<BoneyardsScreens*>(user));
}

// The Chat Frequencies console exists from its opening until another
// Boneyards screen replaces it.
bool chat_exists(const BoneyardsScreens* s) {
    return s->current == ByScreen::chat_frequencies || s->chat.visible;
}

void enter(BoneyardsScreens* s, ByScreen screen) {
    switch (screen) {
    case ByScreen::login:
        login_init(&s->login, &s->service, s, record_message);
        break;
    case ByScreen::profile_console:
        profile_console_init(
            &s->profile,
            &s->service,
            &s->session,
            s->profile.settings,
            s,
            record_message,
            frame_activity_light
        );
        break;
    case ByScreen::battle_list: {
        BattleUi ui{};
        ui.user = s;
        ui.activity = frame_activity_light;
        ui.open_host = open_host_panel;
        battle_list_init(&s->battles, &s->service, ui);
        break;
    }
    case ByScreen::game_setup:
        game_setup_init(&s->setup, &s->service, s, record_message);
        boneyards_prepare_game_setup(s);
        break;
    case ByScreen::chat_frequencies: // the console enters a frequency as it opens
    case ByScreen::none:
        break;
    }
}

} // namespace

void boneyards_screens_init(
    BoneyardsScreens* s, const BoneyardsService* service, SettingsStore settings
) {
    boneyards_screens_release(s);
    // The screens are trivially copyable and too large for a temporary.
    static_assert(std::is_trivially_copyable_v<BoneyardsScreens>);
    std::memset(static_cast<void*>(s), 0, sizeof *s);
    if (service != nullptr)
        s->service = *service;
    boneyards_service_fill_offline(&s->service);
    s->profile.settings = settings;
    const int32_t sound_off =
        settings.read_number != nullptr ? settings.read_number(settings.user, kSoundOffKey, 0) : 0;
    s->session.background_sound = sound_off == 0;
    ChatUi ui{};
    ui.user = s;
    ui.message_box = record_message;
    ui.random = chat_random;
    ui.activity = frame_activity_light;
    chat_console_init(&s->chat, &s->session, &s->service, ui);
    galaxy_init(&s->galaxy);
    s->current = ByScreen::none;
}

void boneyards_screens_release(BoneyardsScreens* s) {
    battle_list_release(&s->battles);
    profile_record_release(&s->profile.profile);
}

ByScreen boneyards_open(BoneyardsScreens* s, ByScreen screen) {
    if (screen == ByScreen::none)
        return s->current;
    s->current = screen;
    enter(s, screen);
    return screen;
}

void boneyards_on_login_result(BoneyardsScreens* s, bool connected) {
    login_on_result(&s->login, connected, false);
    if (connected)
        s->current = ByScreen::none;
}

void boneyards_on_login_status(BoneyardsScreens* s, const char* text) {
    login_set_status(&s->login, text);
}

void boneyards_on_login_reset(BoneyardsScreens* s) {
    login_on_reset(&s->login);
}

void boneyards_on_session(BoneyardsScreens* s, const SessionRecord* session) {
    s->session.self_user_id = session->user_id;
    s->session.account_flags = static_cast<uint8_t>(session->user_id >> kAccountFlagsShift);
    s->session.area_id = session->area_id;
    s->allegiance = session->allegiance;
    s->allegiance_previous = session->allegiance_previous;
    if (session->level == SessionLevel::dossier_loaded && s->login.state == LoginState::connecting)
        boneyards_on_login_result(s, true);
}

void boneyards_on_notice(BoneyardsScreens* s, const char* text) {
    record_message(s, text);
}

void boneyards_on_system_alert(BoneyardsScreens* s, const AlertReport* alert) {
    if (alert->kind == static_cast<int32_t>(AlertKind::message_box))
        record_message(s, alert->text);
}

void boneyards_on_allegiances(BoneyardsScreens* s, const char* allegiances) {
    galaxy_apply_allegiances(&s->galaxy, allegiances);
}

void boneyards_on_activity(BoneyardsScreens* s, const char* digits) {
    galaxy_apply_activity(&s->galaxy, digits);
}

bool boneyards_on_census(BoneyardsScreens* s, const CensusReport* report) {
    return galaxy_apply_census(&s->galaxy, report, s->session.area_id);
}

void boneyards_on_room_listed(
    BoneyardsScreens* s, int32_t change, const RoomListing* room, bool last
) {
    if (chat_exists(s))
        chat_on_room_listed(&s->chat, static_cast<PlayerListChange>(change), room, last);
}

void boneyards_on_room_updated(BoneyardsScreens* s, int32_t change, const RoomListing* room) {
    if (chat_exists(s))
        chat_update_room_row(&s->chat, static_cast<PlayerListChange>(change), room);
}

void boneyards_on_room_entered(BoneyardsScreens* s, int32_t result) {
    if (chat_exists(s))
        chat_on_join_result(&s->chat, result == 0 ? JoinResult::entered : JoinResult::failed);
}

void boneyards_on_room_member(
    BoneyardsScreens* s,
    int32_t change,
    uint32_t user_id,
    const char* name,
    int32_t latency,
    int32_t icon,
    int32_t rank,
    const char* allegiance
) {
    if (chat_exists(s))
        chat_update_player(
            &s->chat,
            static_cast<PlayerListChange>(change),
            user_id,
            name,
            latency,
            icon,
            rank,
            allegiance
        );
}

void boneyards_on_chat_message(
    BoneyardsScreens* s, uint32_t user_id, const char* text, bool from_server
) {
    if (chat_exists(s))
        chat_receive_message(&s->chat, user_id, text, from_server);
}

void boneyards_on_room_left(BoneyardsScreens* s, bool) {
    if (chat_exists(s))
        chat_reset_room_counts(&s->chat);
}

void boneyards_on_room_created(
    BoneyardsScreens* s, uint32_t area_id, uint32_t room_id, const char* name
) {
    if (chat_exists(s))
        chat_on_room_created(&s->chat, area_id, room_id, name);
}

void boneyards_on_battles_done(BoneyardsScreens* s, bool) {
    battle_list_on_done(&s->battles);
}

void boneyards_on_battle_info(
    BoneyardsScreens* s,
    int32_t change,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t host_code
) {
    battle_list_on_battle_info(
        &s->battles, s->session.area_id, change, game_id, name, type, forming, host_code
    );
}

void boneyards_on_intel(BoneyardsScreens* s, const char*, const char* text) {
    battle_list_on_intel(&s->battles, text);
}

void boneyards_on_battle_intel(BoneyardsScreens* s, const BattleIntel* intel) {
    battle_list_on_battle_intel(&s->battles, intel);
}

void boneyards_on_battle_report(BoneyardsScreens* s, const BattleReport* report) {
    battle_list_on_battle_report(&s->battles, report);
}

void boneyards_prepare_game_setup(BoneyardsScreens* s) {
    GameSetupPanel* p = &s->setup;
    if (p->service == nullptr)
        game_setup_init(p, &s->service, s, record_message);
    p->area_id = s->session.area_id;
    p->player_allegiance = s->allegiance;
    const GalaxyView& galaxy = s->galaxy;
    p->has_planet = galaxy.selected != kNoPlanet &&
                    static_cast<std::size_t>(galaxy.selected) < galaxy.planet_count;
    if (p->has_planet) {
        const GalaxyPlanet& planet = galaxy.planets[galaxy.selected];
        p->planet_affiliation = planet.side;
        p->planet_players_limit = planet.player_limit;
        p->planet_commander_death = planet.commander_death;
    } else {
        p->planet_affiliation = 0;
        p->planet_players_limit = 0;
        p->planet_commander_death = 0;
    }
    p->visible = true;
    game_setup_update_types(p);
}

ServiceEvents boneyards_screens_events(BoneyardsScreens* s) {
    ServiceEvents events{};
    events.user = s;
    events.login_status = [](void* user, const char* text) {
        boneyards_on_login_status(static_cast<BoneyardsScreens*>(user), text);
    };
    events.login_reset = [](void* user) {
        boneyards_on_login_reset(static_cast<BoneyardsScreens*>(user));
    };
    events.session_changed = [](void* user, const SessionRecord* session) {
        boneyards_on_session(static_cast<BoneyardsScreens*>(user), session);
    };
    events.notice = [](void* user, const char* text) {
        boneyards_on_notice(static_cast<BoneyardsScreens*>(user), text);
    };
    events.system_alert = [](void* user, const AlertReport* alert) {
        boneyards_on_system_alert(static_cast<BoneyardsScreens*>(user), alert);
    };
    events.battle_active = [](void* user) {
        return static_cast<BoneyardsScreens*>(user)->session.debriefing_pending;
    };
    events.allegiances = [](void* user, const char* allegiances) {
        boneyards_on_allegiances(static_cast<BoneyardsScreens*>(user), allegiances);
    };
    events.activity = [](void* user, const char* digits) {
        boneyards_on_activity(static_cast<BoneyardsScreens*>(user), digits);
    };
    events.census = [](void* user, const CensusReport* report) {
        boneyards_on_census(static_cast<BoneyardsScreens*>(user), report);
    };
    events.room_listed = [](void* user,
                            int32_t change,
                            uint32_t,
                            uint32_t room_id,
                            const char* name,
                            const char* description,
                            bool needs_password,
                            int32_t users,
                            int32_t type,
                            const char* tag,
                            bool last,
                            bool,
                            int32_t) {
        const RoomListing room{room_id, name, description, needs_password, users, type, tag};
        boneyards_on_room_listed(static_cast<BoneyardsScreens*>(user), change, &room, last);
    };
    events.room_updated = [](void* user,
                             int32_t change,
                             uint32_t,
                             uint32_t room_id,
                             const char* name,
                             const char* description,
                             bool needs_password,
                             int32_t users,
                             int32_t type,
                             const char* tag) {
        const RoomListing room{room_id, name, description, needs_password, users, type, tag};
        boneyards_on_room_updated(static_cast<BoneyardsScreens*>(user), change, &room);
    };
    events.room_entered = [](void* user, int32_t result) {
        boneyards_on_room_entered(static_cast<BoneyardsScreens*>(user), result);
    };
    events.room_member = [](void* user,
                            int32_t change,
                            uint32_t user_id,
                            const char* name,
                            int32_t latency,
                            int32_t icon,
                            int32_t rank,
                            const char* allegiance) {
        boneyards_on_room_member(
            static_cast<BoneyardsScreens*>(user),
            change,
            user_id,
            name,
            latency,
            icon,
            rank,
            allegiance
        );
    };
    events.chat_message = [](void* user, bool from_server, uint32_t user_id, const char* text) {
        boneyards_on_chat_message(static_cast<BoneyardsScreens*>(user), user_id, text, from_server);
    };
    events.room_left = [](void* user, bool failed) {
        boneyards_on_room_left(static_cast<BoneyardsScreens*>(user), failed);
    };
    events.room_created = [](void* user, uint32_t area_id, uint32_t room_id, const char* name) {
        boneyards_on_room_created(static_cast<BoneyardsScreens*>(user), area_id, room_id, name);
    };
    events.battles_done = [](void* user, bool failed) {
        boneyards_on_battles_done(static_cast<BoneyardsScreens*>(user), failed);
    };
    events.battle_info = [](void* user,
                            int32_t change,
                            const char* game_id,
                            const char* name,
                            int32_t type,
                            int32_t forming,
                            int32_t host_code) {
        boneyards_on_battle_info(
            static_cast<BoneyardsScreens*>(user), change, game_id, name, type, forming, host_code
        );
    };
    events.intel = [](void* user, const char* game_id, const char* text) {
        boneyards_on_intel(static_cast<BoneyardsScreens*>(user), game_id, text);
    };
    events.battle_intel = [](void* user, const BattleIntel* intel) {
        boneyards_on_battle_intel(static_cast<BoneyardsScreens*>(user), intel);
    };
    events.battle_report = [](void* user, const BattleReport* report) {
        boneyards_on_battle_report(static_cast<BoneyardsScreens*>(user), report);
    };
    return events;
}

} // namespace oa::boneyards::screens
