// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/navigator.hpp"

#include "check.hpp"
#include "profile_reply.hpp"
#include "oa/boneyards/message_box.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

struct Store {
    std::vector<std::string> saved;
    bool fail = false;
};

bool save_number(void* user, const char* key, std::int32_t value) {
    auto* s = static_cast<Store*>(user);
    if (s->fail)
        return false;
    s->saved.push_back(std::string(key) + "=" + std::to_string(value));
    return true;
}

std::vector<std::string> boxes;

void box(void*, const char* text) {
    boxes.emplace_back(text);
}

// The Main Frame's activity light as the console starts and stops it.
std::vector<bool> lights;

void light(void*, bool running) {
    lights.push_back(running);
}

int submissions = 0;

ServiceResult submit_ok(void*, uint32_t, const ProfileEdits*, ProfileUpdatedCallback, void*) {
    ++submissions;
    return ServiceResult::pending;
}

void test_profile_local_settings() {
    Store store;
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.submit_profile = submit_ok;
    static ProfileConsole c;
    static ChatSession session{};
    session.background_sound = true;
    by_test::ProfileReply reply;
    const ProfileFields fields = reply.fields();
    profile_console_init(
        &c, &service, &session, SettingsStore{&store, save_number, nullptr}, nullptr, box, light
    );
    CHECK(c.busy);
    CHECK_STR(c.status, "RECEIVING DATA...");
    profile_on_received(&c, false, &fields);
    CHECK_STR(c.status, "ACCESS FAILED. Try again later.");
    CHECK(c.busy);
    profile_on_received(&c, true, &fields);
    CHECK(!c.busy && !c.status_visible);
    profile_select_tab(&c, static_cast<int>(ProfileTab::account));
    CHECK(c.tab_visible[0]);

    c.unit_limit = 250;
    c.slide_steps = 10;
    c.tab_dirty[6] = true;
    std::snprintf(c.unit_limit_text, sizeof c.unit_limit_text, "%s", "600");
    std::snprintf(c.slide_steps_text, sizeof c.slide_steps_text, "%s", "10");
    boxes.clear();
    profile_apply(&c);
    CHECK(boxes.size() == 1);
    CHECK_STR(
        boxes[0].c_str(), "Invalid Entry!\nMax User Limit Must be a number between 21 and 500"
    );
    CHECK(c.current_tab == 6 && c.tab_visible[6] && !c.tab_visible[0]);
    CHECK(c.selected_field == 0);

    std::snprintf(c.unit_limit_text, sizeof c.unit_limit_text, "%s", "300");
    std::snprintf(c.slide_steps_text, sizeof c.slide_steps_text, "%s", "41");
    static int32_t game_limit = -1;
    game_limit = -1;
    profile_on_unit_limit([](int32_t units) { game_limit = units; });
    profile_apply(&c);
    CHECK_STR(
        boxes.back().c_str(), "Invalid Entry!\nSlide Steps Must be a number between 1 and 40"
    );
    CHECK(c.unit_limit == 300 && c.game_unit_limit == 300); // stored before the slide check
    CHECK(game_limit == 300); // handed to the game with the stored limit
    profile_on_unit_limit(nullptr);

    std::snprintf(c.slide_steps_text, sizeof c.slide_steps_text, "%s", "12");
    c.sound_off_checked = true;
    lights.clear();
    static int32_t slide_given = 0;
    c.set_slide_steps = [](void*, int32_t steps) { slide_given = steps; };
    // The background sound's change stops the sounds as it goes off.
    static std::vector<bool> background;
    background.clear();
    c.background_sound = [](void*, bool on) { background.push_back(on); };
    profile_apply(&c);
    CHECK(c.slide_steps == 12 && c.sound_off && !session.background_sound);
    CHECK(slide_given == 12);
    CHECK(background == std::vector<bool>{false});
    CHECK(!c.tab_dirty[6]);
    CHECK(store.saved.size() == 3);
    CHECK_STR(store.saved[2].c_str(), "BYStopThatIncessantDroning=1");
    // The local settings alone put nothing in the bag: nothing is sent and
    // the console does not start transmitting.
    CHECK(submissions == 0 && !c.busy);
    CHECK(lights.empty());
    profile_update_buttons(&c);
    CHECK(c.apply_value == 0);
    lights.clear();
    profile_on_submitted(&c, false);
    CHECK_STR(
        boxes.back().c_str(),
        "Update Failure\n.Some or all of the changes you requested were not completed."
    );
    CHECK(c.busy); // reloading the record
    // The submission's end stops the light, then the reload starts it.
    CHECK(lights == (std::vector<bool>{false, true}));

    // Storage failures abort before anything is submitted.
    profile_on_received(&c, true, &fields);
    store.fail = true;
    c.tab_dirty[6] = true;
    std::snprintf(c.unit_limit_text, sizeof c.unit_limit_text, "%s", "400");
    profile_apply(&c);
    CHECK(boxes.back().rfind("Update aborted!", 0) == 0);
    CHECK(submissions == 0 && c.tab_dirty[6]);
    store.fail = false;
    c.tab_dirty[6] = false;

    // Password checks.
    c.tab_dirty[0] = true;
    std::snprintf(c.tabs.password.text, sizeof c.tabs.password.text, "%s", "abc");
    std::snprintf(c.tabs.confirm.text, sizeof c.tabs.confirm.text, "%s", "abd");
    profile_apply(&c);
    CHECK_STR(boxes.back().c_str(), "Invalid Entry!\nPassword confirmation does not match");
    CHECK(c.current_tab == 0);
    std::snprintf(c.tabs.password.text, sizeof c.tabs.password.text, "%s", "a'b");
    std::snprintf(c.tabs.confirm.text, sizeof c.tabs.confirm.text, "%s", "a'b");
    profile_apply(&c);
    CHECK_STR(boxes.back().c_str(), "Invalid Entry!\nPassword may not contain the ' character");
    profile_update_buttons(&c);
    CHECK(c.apply_value == 2 && c.reset_value == 2);
    profile_on_control(&c, ProfileControl::reset_button, 0); // refreshes the created tabs
    CHECK(c.tabs.password.text[0] == '\0' && c.tabs.confirm.text[0] == '\0');
    CHECK(!c.tab_dirty[0]);
}

std::vector<by_test::ProfileRequestSeen> g_profile_requests;

ServiceResult request_profile(
    void*, uint32_t user_id, ProfileCallback on_profile, LadderCallback on_ladder, void* context
) {
    g_profile_requests.push_back({user_id, on_profile, on_ladder, context});
    return ServiceResult::pending;
}

// The Profile console asks slot 63 for the player's own profile (by the
// session's user id) with its received and ladder callbacks; each reload asks again.
void test_profile_requests_own_record() {
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.request_profile = request_profile;
    service.submit_profile = submit_ok;
    static ProfileConsole c;
    static ChatSession session{};
    session.self_user_id = 0x51;
    g_profile_requests.clear();
    lights.clear();
    profile_console_init(&c, &service, &session, SettingsStore{}, nullptr, box, light);
    CHECK(g_profile_requests.size() == 1);
    const by_test::ProfileRequestSeen request = g_profile_requests.at(0);
    CHECK(request.user_id == 0x51 && request.context == &c);
    CHECK(request.on_profile == profile_on_received && request.on_ladder == profile_on_ladder);
    CHECK(c.record == &c.profile && c.busy);
    // The light runs while the request is outstanding.
    CHECK(lights == (std::vector<bool>{true}));

    // A failed reply stops it too (the light stops before the result test).
    by_test::ProfileReply empty{};
    const ProfileFields none = empty.fields();
    request.on_profile(request.context, false, &none);
    CHECK(lights == (std::vector<bool>{true, false}));
    CHECK_STR(c.status, "ACCESS FAILED. Try again later.");

    by_test::ProfileReply reply{
        {"user_id", "81"}, {"LOGINNAME", "oa_check"}, {"ALLEGIANCE", "C"}, {"USER_STATUS", "1"}
    };
    const ProfileFields fields = reply.fields();
    request.on_profile(request.context, true, &fields);
    CHECK(!c.busy && c.record == &c.profile);
    CHECK(c.profile.user_id == 0x51 && c.profile.allegiance == 'C');
    CHECK_STR(c.tabs.login_name, "oa_check");
    CHECK(lights == (std::vector<bool>{true, false, false}));

    // A reload after a submission asks for the same id into a fresh record.
    lights.clear();
    profile_on_submitted(&c, true);
    CHECK(g_profile_requests.size() == 2 && g_profile_requests[1].user_id == 0x51);
    CHECK(c.busy && c.profile.name[0] == '\0' && c.profile.allegiance == 'U');
    CHECK(lights == (std::vector<bool>{false, true}));
}

// The tab dirty check the profile pulse and RESET run.
void test_profile_tab_dirty() {
    BoneyardsService service = boneyards_offline_service(nullptr);
    static ProfileConsole c;
    static ChatSession session{};
    by_test::ProfileReply reply{
        {"user_id", "12"},
        {"TAG", "tag"},
        {"HAS_BT", "Yes"},
        {"EMAIL_ALIAS", "alias"},
        {"PLAYERSKILL", "3"},
        {"NUMPLAYERS", "3"},
        {"ONCOMDEATH", "0"},
        {"FAVORITEMAP", "Tundra"},
    };
    const ProfileFields fields = reply.fields();
    profile_console_init(&c, &service, &session, SettingsStore{}, nullptr, box, nullptr);
    profile_on_received(&c, true, &fields);
    ProfileRecord& record = c.profile;
    CHECK_STR(record.preferred_skill, "Expert");
    CHECK_STR(record.preferred_death, "Continues");
    for (int tab : {1, 2, 3, 4})
        profile_select_tab(&c, tab);
    for (int tab = 0; tab < 5; ++tab) {
        c.tab_dirty[tab] = true;
        profile_check_tab(&c, tab);
        CHECK(!c.tab_dirty[tab]); // fresh from the record
    }
    c.tab_dirty[5] = true;
    profile_check_tab(&c, 5); // not created: left alone
    CHECK(c.tab_dirty[5]);
    c.tab_dirty[5] = false;

    std::snprintf(
        c.tabs.confirm.text, sizeof c.tabs.confirm.text, "%s", "x"
    ); // either password field
    profile_check_tab(&c, 0);
    CHECK(c.tab_dirty[0]);
    c.tabs.confirm.text[0] = '\0';
    profile_check_tab(&c, 0);
    CHECK(!c.tab_dirty[0]);

    std::snprintf(c.tabs.tag_line.text, sizeof c.tabs.tag_line.text, "%s", "tag2");
    profile_check_tab(&c, 1);
    CHECK(c.tab_dirty[1]);
    std::snprintf(c.tabs.tag_line.text, sizeof c.tabs.tag_line.text, "%s", "tag");
    c.tabs.battle_tactics_value = widget_value::icon_off; // the record's has_battle_tactics set
    profile_check_tab(&c, 1);
    CHECK(c.tab_dirty[1]);
    c.tabs.battle_tactics_value = widget_value::icon_on;
    c.tabs.display_email_alias_value = widget_value::icon_off; // the record's email_alias non-empty
    profile_check_tab(&c, 1);
    CHECK(c.tab_dirty[1]);
    c.tabs.display_email_alias_value = widget_value::icon_on;
    std::snprintf(
        c.tabs.icq.text, sizeof c.tabs.icq.text, "%s", "1"
    ); // record ICQ omitted: compares as empty
    profile_check_tab(&c, 1);
    CHECK(c.tab_dirty[1]);
    c.tabs.icq.text[0] = '\0';
    profile_check_tab(&c, 1);
    CHECK(!c.tab_dirty[1]);

    c.tabs.core_value = widget_value::selected; // a join radio, record side 'U'
    profile_check_tab(&c, 2);
    CHECK(c.tab_dirty[2]);
    record.allegiance = 'A';
    profile_check_tab(&c, 2);
    CHECK(!c.tab_dirty[2]);
    record.allegiance = 'U';

    radio_select_by_name(&c.tabs.players, "4");
    profile_check_tab(&c, 3);
    CHECK(c.tab_dirty[3]);
    radio_select_by_name(&c.tabs.players, "3");
    std::snprintf(c.tabs.favorite_map.text, sizeof c.tabs.favorite_map.text, "%s", "Lava");
    profile_check_tab(&c, 3);
    CHECK(c.tab_dirty[3]);
    std::snprintf(c.tabs.favorite_map.text, sizeof c.tabs.favorite_map.text, "%s", "Tundra");
    profile_check_tab(&c, 3);
    CHECK(!c.tab_dirty[3]);

    // A guest (user id < 0) is never dirty on the allegiance and preference tabs.
    record.user_id = -5;
    std::snprintf(c.tabs.favorite_map.text, sizeof c.tabs.favorite_map.text, "%s", "Lava");
    c.tab_dirty[2] = c.tab_dirty[3] = true;
    profile_check_tab(&c, 2);
    profile_check_tab(&c, 3);
    CHECK(!c.tab_dirty[2] && !c.tab_dirty[3]);
    c.tab_dirty[4] = true;
    profile_check_tab(&c, 4); // ladder
    CHECK(!c.tab_dirty[4]);

    // The pulse rechecks only the current tab before enabling APPLY/RESET.
    profile_select_tab(&c, 6);
    std::snprintf(c.unit_limit_text, sizeof c.unit_limit_text, "%d", c.unit_limit + 1);
    profile_update_buttons(&c);
    CHECK(c.tab_dirty[6] && c.apply_value == widget_value::enabled);
    std::snprintf(c.unit_limit_text, sizeof c.unit_limit_text, "%d", c.unit_limit);
    c.sound_off_checked = !c.sound_off;
    profile_check_tab(&c, 6);
    CHECK(c.tab_dirty[6]);
    c.sound_off_checked = c.sound_off;
    profile_update_buttons(&c);
    CHECK(!c.tab_dirty[6] && c.apply_value == widget_value::disabled);
}

void test_game_modes_and_setup() {
    GameModeEntry modes[8];
    CHECK(game_mode_list(modes, 8) == 5);
    CHECK_STR(modes[3].name, "Galactic War");
    CHECK(modes[4].mode == GameMode::war_training);

    static HostGameRequest last{};
    static int hosted = 0;
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.host_game = [](void*, const HostGameRequest* r) {
        last = *r;
        ++hosted;
        return ServiceResult::pending;
    };
    static GameSetupPanel p;
    boxes.clear();
    game_setup_init(&p, &service, nullptr, box);
    CHECK(p.type_count == 3);
    game_setup_select_type(&p, 1); // Duel
    CHECK(p.players.selected == 0);
    CHECK(p.players.members[1].value == 0 && p.players.members[2].value == 0);
    game_setup_select_type(&p, 2); // Winner Takes All
    CHECK(
        p.players.selected == 1 && p.players.members[0].value == 0 &&
        p.players.members[2].value == 2
    );

    p.has_planet = true;
    p.planet_affiliation = 'I';
    p.player_allegiance = 'A';
    p.planet_players_limit = 4;
    p.planet_commander_death = 3;
    game_setup_update_types(&p);
    CHECK(p.type_count == 4 && p.types[3].mode == GameMode::galactic_war);
    game_setup_select_type(&p, 3);
    CHECK(p.players.selected == 0 && p.players.members[2].value == 2);
    CHECK(p.commander_death.selected == 2 && p.commander_death.members[0].value == 0);
    p.planet_affiliation = 'C';
    game_setup_update_types(&p);
    CHECK(p.type_count == 3); // Arm player on a Core planet: no War Training
    p.player_allegiance = 'C';
    game_setup_update_types(&p);
    CHECK(p.types[3].mode == GameMode::war_training);
    p.has_planet = false;
    game_setup_select_type(&p, 3);
    CHECK(boxes.back().find("War Training") != std::string::npos);

    std::snprintf(p.game_name, sizeof p.game_name, "%s", "Brawl");
    radio_set_index(&p.skill, 2);
    radio_set_index(&p.commander_death, 0);
    p.area_id = 9;
    CHECK(game_setup_start(&p));
    CHECK(hosted == 1 && !p.visible);
    CHECK_STR(last.name, "Brawl");
    CHECK(last.password == nullptr && last.skill == 2 && last.commander_death == 2);
    CHECK(last.game_mode == 7 && last.area_id == 9);
}

void test_login() {
    BoneyardsService service = boneyards_offline_service(nullptr);
    LoginDialog d;
    boxes.clear();
    login_init(&d, &service, nullptr, box);
    login_update_button(&d);
    CHECK(d.login_value == 0);
    std::snprintf(d.name, sizeof d.name, "%s", "Kane");
    login_update_button(&d);
    CHECK(d.login_value == 0); // member without a password
    login_on_control(&d, LoginControl::guest_radio);
    CHECK(d.password_field_value == 0);
    login_update_button(&d);
    CHECK(d.login_value == 2);
    // An enabled LOGIN keeps its value (the update only enables a disabled one).
    d.login_value = 1;
    login_update_button(&d);
    CHECK(d.login_value == 1);
    d.login_value = 2;
    CHECK(login_on_enter(&d, true));
    CHECK_STR(d.session_name, "Kane");
    CHECK_STR(d.status, "Server Not responding"); // offline service
    CHECK(d.state == LoginState::idle && d.login_value == 2);
    std::string listed;
    d.status_user = &listed;
    d.append_status = [](void* user, const char* text) {
        *static_cast<std::string*>(user) = text != nullptr ? text : "";
    };
    login_set_status(&d, kWrongPasswordStatus);
    CHECK_STR(d.status, kWrongPasswordResetStatus);
    CHECK_STR(listed.c_str(), kWrongPasswordResetStatus);
    login_on_control(&d, LoginControl::register_button);
    CHECK_STR(boxes.back().c_str(), "To register please visit the Registration console");
    // An attached console opens instead of showing the box.
    const std::size_t boxes_before = boxes.size();
    int opened = 0;
    d.registration_user = &opened;
    d.open_registration = [](void* user) { *static_cast<int*>(user) += 1; };
    login_on_control(&d, LoginControl::register_button);
    CHECK(opened == 1 && boxes.size() == boxes_before);
    login_on_control(&d, LoginControl::cancel_button);
    CHECK(!d.closed);
    CHECK(d.state == LoginState::idle && d.login_value == 2);
    CHECK_STR(d.status, "");
}

// The model enters whichever screen its console opens; a login ends on the
// dialog, and a completed one leaves no screen current for the Main Frame
// to close the login console over.
void test_navigation() {
    static BoneyardsScreens s;
    boneyards_screens_init(&s, nullptr, SettingsStore{});
    CHECK(s.current == ByScreen::none);
    CHECK(boneyards_open(&s, ByScreen::login) == ByScreen::login);
    std::snprintf(s.login.name, sizeof s.login.name, "%s", "Kane");
    login_on_control(&s.login, LoginControl::guest_radio);
    login_on_control(&s.login, LoginControl::login_button);
    CHECK_STR(s.login.status, "Server Not responding");
    boneyards_on_login_result(&s, false);
    CHECK(s.current == ByScreen::login && s.login.state == LoginState::idle);
    CHECK(boneyards_open(&s, ByScreen::none) == ByScreen::login);

    login_on_control(&s.login, LoginControl::login_button);
    boneyards_on_login_result(&s, true);
    CHECK(s.login.state == LoginState::planetary && s.login.closed);
    CHECK(s.current == ByScreen::none);

    CHECK(boneyards_open(&s, ByScreen::battle_list) == ByScreen::battle_list);
    CHECK(s.battles.row_count == 5);
    // Chat Frequencies becomes current as its console is built; the console
    // enters a frequency only when it opens.
    CHECK(boneyards_open(&s, ByScreen::chat_frequencies) == ByScreen::chat_frequencies);
    CHECK(!s.chat.visible && !s.chat.enter_default_room);
    chat_open_on_planet(&s.chat);
    CHECK(s.chat.visible && s.chat.enter_default_room);
    CHECK(boneyards_open(&s, ByScreen::profile_console) == ByScreen::profile_console);
}

// A screen's message box stays up when the window it was opened over
// closes (the Main Frame's EXIT); dismissing it then leaves focus alone.
void test_message_outlives_focus() {
    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Services services{};
    services.desktop = &desktop;
    const Services previous = services_install(services);
    static BoneyardsScreens s;
    boneyards_screens_init(&s, nullptr, SettingsStore{});
    CHECK(boneyards_open(&s, ByScreen::login) == ByScreen::login);
    Window* view = gadget_create();
    window_create(view, "view", 0, 0, 10, 10, nullptr, true);
    desktop_set_focus(&desktop, view);
    login_on_control(&s.login, LoginControl::register_button);
    MessageBox* box = message_box_find_open(&desktop);
    CHECK(box != nullptr && box->focus_return == view && desktop.focus == &box->base);
    desktop_remove(&desktop, view);
    Message enter{};
    enter.code = MessageCode::key_down;
    enter.key = key::enter;
    if (box != nullptr) {
        window_dispatch(&box->base, &enter);
        CHECK(box->base.owned == 1 && desktop.focus != view);
    }
    desktop_destroy_owned(&desktop);
    window_destroy(view);
    desktop_release(&desktop);
    services_install(previous);
}

} // namespace

// The Boneyards background sound is on unless BYStopThatIncessantDroning is
// set; the Profile console's local settings check shows the setting, and
// APPLY turns the sound off with the check and stores it.
int32_t g_sound_off_setting = 0;
bool g_sound_off_present = false;

int32_t read_sound_setting(void*, const char* key, int32_t fallback) {
    if (std::strcmp(key, kSoundOffKey) == 0 && g_sound_off_present)
        return g_sound_off_setting;
    return fallback;
}

void test_background_sound_setting() {
    static BoneyardsScreens s;
    Store store;
    const SettingsStore settings{&store, save_number, read_sound_setting};
    g_sound_off_present = false;
    boneyards_screens_init(&s, nullptr, settings);
    CHECK(s.session.background_sound);
    g_sound_off_present = true;
    g_sound_off_setting = 0;
    boneyards_screens_init(&s, nullptr, settings);
    CHECK(s.session.background_sound);
    g_sound_off_setting = 1;
    boneyards_screens_init(&s, nullptr, settings);
    CHECK(!s.session.background_sound);

    boneyards_open(&s, ByScreen::profile_console);
    ProfileConsole* c = &s.profile;
    profile_select_tab(c, static_cast<int32_t>(ProfileTab::local_settings));
    CHECK(c->sound_off && c->sound_off_checked);
    c->sound_off_checked = false;
    c->tab_dirty[static_cast<int32_t>(ProfileTab::local_settings)] = true;
    static std::vector<bool> background;
    background.clear();
    c->background_sound = [](void*, bool on) { background.push_back(on); };
    profile_apply(c);
    CHECK(s.session.background_sound && !c->sound_off);
    CHECK(!store.saved.empty() && store.saved.back() == "BYStopThatIncessantDroning=0");
    // It comes on again at once.
    CHECK(background == std::vector<bool>{true});
    boneyards_screens_release(&s);
}

int main() {
    test_profile_local_settings();
    test_profile_tab_dirty();
    test_profile_requests_own_record();
    test_game_modes_and_setup();
    test_login();
    test_navigation();
    test_message_outlives_focus();
    test_background_sound_setting();
    return by_test::finish("boneyards screens");
}
