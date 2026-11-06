// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Main-menu overlay -> Main Frame -> Boneyards screens with the built-in
// views over a fake Central Command: a login through the login console to
// the galactic map and on to chat and profile, an offline login through the
// SingleEdit fields, the registration console and message boxes, and the transition from the dossier
// to the galactic map.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "profile_reply.hpp"
#include "console_layer.hpp"
#include "console_state.hpp"
#include "profile_tabs_view.hpp"
#include "galaxy_graphics.hpp"
#include "html_view.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/platform/system.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct Data {
    oa::AssetStore store{oa::boneyards_test::by_install()};
    oa::Palette palette{};
};

bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Data*>(user)->store.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct FakeServer {
    bool connected = false;
    int32_t logins = 0;
    int32_t chats = 0;
    std::string last_chat;
    // The room, area and target of the last line sent.
    uint32_t chat_room = 0;
    uint32_t chat_area = 0;
    uint32_t chat_target = 0;
    // The chat room requests, in order, and the password of the last switch.
    std::vector<std::string> room_calls;
    std::string switch_password;
    ServiceEvents events{};
    void (*on_pump)() = nullptr;
    int32_t pumps = 0;
    // War map: the yard master's server name, the path the galaxy load
    // opened and every war-map request in order.
    const char* area = "alpha";
    std::string map_path;
    std::vector<std::string> calls;
    // Battle list entries and exits by area, and the answer they get.
    std::vector<std::string> battle_calls;
    ServiceResult battle_result = ServiceResult::pending;
    // Profile requests in order, and the Silenced people list.
    std::vector<by_test::ProfileRequestSeen> profile_requests;
    std::vector<uint32_t> silenced;
};

FakeServer g_server;

std::vector<std::string> g_sounds;

void host_sound(void*, const char* name) {
    g_sounds.emplace_back(name);
}

bool heard(const char* name) {
    for (const std::string& sound : g_sounds)
        if (sound == name)
            return true;
    return false;
}

bool fake_network(void*) {
    return true;
}

ActivationResult fake_activate(void*) {
    return ActivationResult::ok;
}

bool fake_connected(void*) {
    return g_server.connected;
}

ServiceResult fake_login(void*, const LoginRequest*) {
    ++g_server.logins;
    return g_server.connected ? ServiceResult::ok : ServiceResult::not_connected;
}

ServiceResult fake_pending_login(void*, const LoginRequest*) {
    ++g_server.logins;
    return ServiceResult::pending;
}

void fake_set_events(void*, const ServiceEvents* events) {
    g_server.events = events != nullptr ? *events : ServiceEvents{};
}

void fake_pump(void*) {
    ++g_server.pumps;
    void (*action)() = g_server.on_pump;
    g_server.on_pump = nullptr;
    if (action != nullptr)
        action();
}

ServiceResult fake_enter_battles(void*, uint32_t area_id) {
    g_server.battle_calls.push_back("enter " + std::to_string(area_id));
    return g_server.battle_result;
}

ServiceResult fake_leave_battles(void*, uint32_t area_id) {
    g_server.battle_calls.push_back("leave " + std::to_string(area_id));
    return g_server.battle_result;
}

ServiceResult fake_chat(void*, uint32_t room, uint32_t area, uint32_t target, const char* text) {
    ++g_server.chats;
    g_server.last_chat = text;
    g_server.chat_room = room;
    g_server.chat_area = area;
    g_server.chat_target = target;
    return ServiceResult::ok;
}

ServiceResult fake_room_list(void*, uint32_t area) {
    g_server.room_calls.push_back("rooms(" + std::to_string(area) + ")");
    return ServiceResult::pending;
}

ServiceResult fake_join_room(void*, uint32_t area, uint32_t room, const char*, const char*) {
    g_server.room_calls.push_back(
        "join(" + std::to_string(area) + "," + std::to_string(room) + ")"
    );
    return ServiceResult::pending;
}

ServiceResult fake_switch_room(
    void*,
    uint32_t area,
    uint32_t room,
    const char* password,
    uint32_t old_area,
    uint32_t old_room,
    const char*
) {
    g_server.room_calls.push_back(
        "switch(" + std::to_string(area) + "," + std::to_string(room) + " from " +
        std::to_string(old_area) + "," + std::to_string(old_room) + ")"
    );
    g_server.switch_password = password != nullptr ? password : "(none)";
    return ServiceResult::pending;
}

ServiceResult fake_create_room(
    void*, uint32_t area, const char* name, const char* description, const char* password
) {
    g_server.room_calls.push_back(
        "create(" + std::to_string(area) + "," + name + "," + description + "," + password + ")"
    );
    return ServiceResult::pending;
}

ServiceResult fake_request_profile(
    void*, uint32_t user_id, ProfileCallback on_profile, LadderCallback on_ladder, void* context
) {
    g_server.profile_requests.push_back({user_id, on_profile, on_ladder, context});
    return ServiceResult::pending;
}

std::vector<uint32_t>::iterator silenced_entry(const char* list, uint32_t user_id) {
    std::vector<uint32_t>& ids = g_server.silenced;
    if (std::strcmp(list, kSilencedList) != 0)
        return ids.end();
    auto it = ids.begin();
    while (it != ids.end() && *it != user_id)
        ++it;
    return it;
}

bool fake_people_contains(void*, const char* list, uint32_t user_id) {
    return silenced_entry(list, user_id) != g_server.silenced.end();
}

void fake_people_insert(void*, const char* list, uint32_t user_id) {
    if (std::strcmp(list, kSilencedList) == 0 && !fake_people_contains(nullptr, list, user_id))
        g_server.silenced.push_back(user_id);
}

void fake_people_erase(void*, const char* list, uint32_t user_id) {
    const auto it = silenced_entry(list, user_id);
    if (it != g_server.silenced.end())
        g_server.silenced.erase(it);
}

// Profile.def's pop-up page and a Duel ladder page (type 1, subtype 0).
char g_popup_page[] = "<html><BODY>General: <$TAG|no tag$><P></BODY></html>";
char g_duel_page[] = "<b>Duel</b> rank <$RANK|Below 500$>";
LadderTemplate g_duel_ladder{1, 0, 3, g_duel_page};
LadderTemplate* g_ladders[] = {&g_duel_ladder};

// Profile.def's profile_category_info names and descriptions, by id.
char g_category_names[kProfileCategoryCount][0x20] = {
    "Login",
    "Personal",
    "Boneyards",
    "Game Preferences",
    "Ladder Standings",
    "Achievements",
    "Local Settings",
};
char g_category_descriptions[kProfileCategoryCount][0x50] = {
    "Private user account information.",
    "Personal information about yourself you can share with the world.",
    "Data boneyards keeps track of.",
    "Choose the game settings you prefer.",
    "Scrolling summary of ladder standings. ",
    "Special honors and awards are shown here.",
    " Machine specific configuration options.",
};

const ProfileTemplates* fake_profile_templates(void*) {
    static ProfileTemplates templates{};
    templates.loaded = true;
    for (size_t i = 0; i < kProfileCategoryCount; ++i) {
        templates.category_names[i] = g_category_names[i];
        templates.category_descriptions[i] = g_category_descriptions[i];
    }
    templates.popup_page = g_popup_page;
    templates.ladders = g_ladders;
    templates.ladder_count = 1;
    templates.ladder_capacity = 1;
    return &templates;
}

ServiceResult fake_leave_room(void*, uint32_t area, uint32_t room) {
    g_server.room_calls.push_back(
        "leave(" + std::to_string(area) + "," + std::to_string(room) + ")"
    );
    return ServiceResult::pending;
}

// A 2000x1400 war map of three planets, ids 11 to 13, joined in a line; only
// Boneyards\alpha_map.gwm exists.
constexpr const char* k_alpha_map = "Boneyards\\alpha_map.gwm";
constexpr int32_t k_first_planet = 11;
constexpr int32_t k_planet_count = 3;

const char* fake_area_name(void*) {
    return g_server.area;
}

SectorMapReader* fake_open_map(void*, const char* path, SectorMapHeader* header) {
    g_server.calls.emplace_back("open");
    g_server.map_path = path;
    if (g_server.map_path != k_alpha_map)
        return nullptr;
    *header = SectorMapHeader{};
    header->sector_id = 45;
    std::snprintf(header->sector_name, sizeof header->sector_name, "Test Sector");
    header->width = 2000;
    header->height = 1400;
    header->planet_count = k_planet_count;
    header->connector_count = k_planet_count - 1;
    return reinterpret_cast<SectorMapReader*>(&g_server);
}

bool fake_read_map(
    void*,
    SectorMapReader*,
    int32_t,
    int32_t,
    SectorPlanetRecord* planets,
    int32_t planet_count,
    ConnectorRecord* connectors,
    int32_t connector_count
) {
    g_server.calls.emplace_back("read");
    const char sides[] = {'C', 'A', 'I'};
    for (int32_t i = 0; i < planet_count; ++i) {
        SectorPlanetRecord& p = planets[i];
        p = SectorPlanetRecord{};
        p.id = k_first_planet + i;
        std::snprintf(p.name, sizeof p.name, "Planet %d", i);
        p.x = 400 + 600 * i;
        p.y = 300 + 400 * i;
        std::snprintf(p.seqname, sizeof p.seqname, "Metal_");
        p.planet_rot = 5;
        p.side = sides[i % 3];
    }
    for (int32_t i = 0; i < connector_count; ++i)
        connectors[i] = ConnectorRecord{k_first_planet + i, k_first_planet + i + 1};
    return true;
}

void fake_close_map(void*, SectorMapReader*) {
    g_server.calls.emplace_back("close");
}

void fake_request_allegiance(void*) {
    g_server.calls.emplace_back("allegiance");
}

void fake_request_census(void*, uint32_t area) {
    g_server.calls.push_back("census(" + std::to_string(area) + ")");
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void click_window(StatusWidget* widget, const Window* w) {
    const int32_t x = w->x + w->width / 2;
    const int32_t y = w->y + w->height / 2;
    boneyards_status_widget_event(widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, x, y, 0));
}

void double_click_window(StatusWidget* widget, const Window* w) {
    const int32_t x = w->x + w->width / 2;
    const int32_t y = w->y + w->height / 2;
    click_window(widget, w);
    boneyards_status_widget_event(widget, pointer(MessageCode::left_double, x, y, button_left));
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, x, y, 0));
}

void press(StatusWidget* widget, int32_t k) {
    Message msg{};
    msg.code = MessageCode::key_down;
    msg.key = k;
    boneyards_status_widget_event(widget, msg);
    msg.code = MessageCode::key_up;
    boneyards_status_widget_event(widget, msg);
}

void type(StatusWidget* widget, const char* text) {
    for (; *text != '\0'; ++text)
        press(widget, *text);
}

void (*g_entry_draw_string)(
    void*, Handle, int32_t, int32_t, int32_t, int32_t, const char*, int32_t
) = nullptr;
std::vector<std::string> g_entry_drawn;

void record_entry_string(
    void* self,
    Handle font,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char* text,
    int32_t style
) {
    if (text != nullptr && text[0] != '\0')
        g_entry_drawn.emplace_back(text);
    if (g_entry_draw_string != nullptr)
        g_entry_draw_string(self, font, x, y, width, height, text, style);
}

// The entry draws one asterisk per typed character and still holds that text.
bool draws_password(StatusWidget* widget, SingleEdit* edit, const char* typed) {
    if (edit == nullptr || typed == nullptr || edit->mask != '*' ||
        std::strcmp(single_edit_text(edit), typed) != 0)
        return false;
    ScopedServices scope(widget);
    if (services().texts == nullptr || services().texts->draw_string == nullptr)
        return false;
    TextService texts = *services().texts;
    g_entry_draw_string = texts.draw_string;
    g_entry_drawn.clear();
    texts.draw_string = record_entry_string;
    Services hooked = services();
    hooked.texts = &texts;
    const Services previous = services_install(hooked);
    edit->base.ops->draw(&edit->base);
    services_install(previous);
    const std::string stars(std::strlen(typed), '*');
    bool stars_drawn = false;
    bool plain_drawn = false;
    for (const std::string& drawn : g_entry_drawn) {
        stars_drawn = stars_drawn || drawn == stars;
        plain_drawn = plain_drawn || drawn == typed;
    }
    return stars_drawn && !plain_drawn;
}

Window* console_button(MainFrame* frame, ConsoleButton which) {
    return frame->console_buttons[static_cast<int32_t>(which)];
}

constexpr int32_t k_frame_width = 640;
constexpr int32_t k_frame_height = 480;

// Draws a frame into a 640x480 buffer of palette indices.
std::vector<uint8_t> draw_pixels(StatusWidget* widget) {
    std::vector<uint8_t> pixels(k_frame_width * k_frame_height, 0);
    oa::Surface surface{};
    surface.width = k_frame_width;
    surface.height = k_frame_height;
    surface.pitch = k_frame_width;
    surface.pixels = pixels.data();
    surface.clip = oa::Rect32{0, 0, k_frame_width - 1, k_frame_height - 1};
    boneyards_status_widget_draw(widget, &surface);
    return pixels;
}

// Draws a frame; with OA_SNAPSHOT_DIR set also writes it as a PPM.
int32_t draw_frame(StatusWidget* widget, const Data& data, const char* name) {
    const std::vector<uint8_t> pixels = draw_pixels(widget);
    if (const auto dir = oa::platform::environment_value("OA_SNAPSHOT_DIR")) {
        std::ofstream out(std::filesystem::path(*dir) / name, std::ios::binary);
        out << "P6\n640 480\n255\n";
        for (uint8_t index : pixels) {
            const oa::PaletteEntry& e = data.palette.entries[index];
            out.put(static_cast<char>(e.r)).put(static_cast<char>(e.g)).put(static_cast<char>(e.b));
        }
    }
    int32_t painted = 0;
    for (uint8_t index : pixels)
        painted += index != 0 ? 1 : 0;
    return painted;
}

// Pixel of an image the store holds, or -1.
int32_t image_pixel(StatusWidget* widget, Handle image, int32_t x, int32_t y) {
    const Sprite* sprite = console_images_sprite(boneyards_status_widget_images(widget), image);
    if (sprite == nullptr || x < 0 || y < 0 || x >= sprite->width || y >= sprite->height)
        return -1;
    return static_cast<const uint8_t*>(sprite->data)[y * sprite->width + x];
}

// The ChatList's scroll bar as by_list_on_bounds, by_list_refresh, by_slider_on_bounds and
// by_slider_refresh lay it out: 0x11 wide, 0x12 in from the list's right edge and a
// pixel inside its top and bottom; arrows 9 high at either end and the long
// 0x1e thumb on a bar taller than 0x59; a transparent (index 9) image with a
// track line at x=8 from y=0xb to height-0xc, 0x5a while enabled and 0x5d
// while disabled. The list itself shows a transparent image of its size.
void check_chat_list_scroll_bar(StatusWidget* widget, Window* list) {
    auto* log = reinterpret_cast<screens::ByListBox*>(list);
    Window* bar = window_find_named(list, "slider", class_name::slider);
    check(log->slider != nullptr && bar == &log->slider->slider.base, "ChatList builds its slider");
    if (bar == nullptr)
        return;
    check(
        bar->x == list->x + 0x176 - 0x12 && bar->y == list->y + 1 && bar->width == 0x11 &&
            bar->height == 0x122 - 2,
        "slider along the right edge"
    );
    const screens::BySlider* s = log->slider;
    check(
        s->increment->x == bar->x && s->increment->y == bar->y && s->increment->width == 0x11 &&
            s->increment->height == 9,
        "increment arrow at the top"
    );
    check(
        s->decrement->y == bar->y + bar->height - 9 && s->decrement->height == 9,
        "decrement arrow at the bottom"
    );
    check(s->thumb->width == 0x11 && s->thumb->height == 0x1e, "long thumb on a tall bar");
    check(
        bar->visible == 0 && bar->states.current == button_state::disabled,
        "bar hidden and disabled while the log fits"
    );
    check(log->list.no_highlight == 1, "no selection highlight");
    const Handle enabled = bar->states.images[button_state::unpressed];
    const Handle disabled = bar->states.images[button_state::disabled];
    check(
        enabled >= 0 && enabled == bar->states.images[button_state::pressed] && disabled >= 0 &&
            disabled != enabled,
        "enabled and disabled track images"
    );
    check(
        image_pixel(widget, enabled, 8, 0xb) == 0x5a &&
            image_pixel(widget, enabled, 8, bar->height - 0xc) == 0x5a &&
            image_pixel(widget, enabled, 8, 0xa) == 9 &&
            image_pixel(widget, enabled, 8, bar->height - 0xb) == 9 &&
            image_pixel(widget, enabled, 7, 0x40) == 9,
        "enabled track line"
    );
    check(image_pixel(widget, disabled, 8, 0x40) == 0x5d, "disabled track colour");
    const Handle background = list->states.images[1];
    check(
        background >= 0 && background == list->states.images[0] &&
            image_pixel(widget, background, 0, 0) == 9 &&
            image_pixel(widget, background, 0x175, 0x121) == 9,
        "transparent list image"
    );
    check(
        s->increment->states.images[button_state::unpressed] >= 0 &&
            s->thumb->states.images[button_state::unpressed] >= 0,
        "arrow and thumb art from Widgets.gaf"
    );
}

// The text of the ChatList's last line.
std::string last_log_line(StatusWidget* widget, ListBox* log) {
    Window* line = log != nullptr && log->count > 0 ? list_box_item(log, log->count - 1) : nullptr;
    if (line == nullptr)
        return "";
    ScopedServices scope(widget);
    const char* text = line->ops->state_text(line, -1);
    return text != nullptr ? text : "";
}

// The open Chat console over the service's room feeds: it asked for the
// planet's frequencies with the Main Frame's activity light running, the
// listing's last row enters a default one (slot 10), the entry and a
// member's line land in the log with GREEN_BUTTON, the listing after the
// entry stops the light, and a double-click on another frequency starts it
// again and switches to it from the entered one (slot 11).
void check_room_feeds(StatusWidget* widget, ListBox* log) {
    screens::BoneyardsScreens* nav = widget->navigation;
    const ActivityLight* light = widget->system.main_frame->activity_light;
    const uint32_t area_id = nav->session.area_id;
    const std::string area = std::to_string(area_id);
    check(
        !g_server.room_calls.empty() && g_server.room_calls.back() == "rooms(" + area + ")",
        "opening the console asks for the planet's frequencies"
    );
    check(
        light->running == 1, "chat_open_on_planet starts the activity light as the console opens"
    );
    const ServiceEvents& events = g_server.events;
    ScopedServices scope(widget);
    events.room_listed(
        events.user, 0, area_id, 13, "Calm", "quiet", false, 2, 1, "", false, false, 0
    );
    events.room_listed(events.user, 0, area_id, 14, "Other", "", false, 3, 2, "", true, false, 0);
    check(
        g_server.room_calls.back() == "join(" + area + ",13)", "the default frequency is entered"
    );
    check(light->running == 1, "the default entry (chat_on_room_listed) keeps the light running");
    g_sounds.clear();
    events.room_entered(events.user, 0);
    check(
        nav->session.room_id == 13 && last_log_line(widget, log) == "[Entered 'Calm']",
        "the entry is announced in the log"
    );
    check(
        g_sounds.size() == 1 && g_sounds[0] == "GREEN_BUTTON", "chat_print_line sounds GREEN_BUTTON"
    );
    events.room_listed(
        events.user, 0, area_id, 13, "Calm", "quiet", false, 2, 1, "", false, false, 0
    );
    events.room_listed(
        events.user, 0, area_id, 14, "Other", "", false, 3, 2, "Ann", true, false, 0
    );
    check(light->running == 0, "the listing's last row (chat_on_room_listed) stops the light");
    check(
        nav->chat.room_total == 2 && std::strcmp(nav->chat.rooms[1].tag, "Ann") == 0,
        "the rows keep their tags"
    );
    events.room_member(events.user, 0, 9, "Dan", 2, 3, 12, "C");
    check(
        nav->chat.player_total == 1 && nav->chat.players[0].user_icon == 3 &&
            nav->chat.players[0].rank == 12 &&
            std::strcmp(nav->chat.players[0].allegiance, "C") == 0,
        "Dan's rank icon is his icon frame, and his rank and allegiance are kept"
    );
    const int32_t before_server = log->count;
    events.chat_message(events.user, true, 0, "Welcome to Slag, commander.");
    check(log->count == before_server, "a line from the server is not shown");
    events.chat_message(events.user, false, 9, "hi all");
    check(last_log_line(widget, log) == "Dan: hi all", "a member's line is printed");
    events.room_listed(
        events.user, 0, area_id, 13, "Calm", "quiet", false, 3, 1, "", false, false, 0
    );
    events.room_listed(events.user, 0, area_id, 14, "Other", "", false, 3, 2, "", true, false, 0);
    screens::chat_on_control(
        &nav->chat, screens::ChatControl::room_list, screens::kListActivate, 1
    );
    check(
        g_server.room_calls.back() == "switch(" + area + ",14 from " + area + ",13)",
        "a double-clicked frequency is switched to from the entered one"
    );
    check(light->running == 1, "chat_go_to_room starts the light for the switch");
    events.room_entered(events.user, 0);
    check(
        nav->session.room_id == 14 && last_log_line(widget, log) == "[Entered 'Other']",
        "the switch is announced"
    );
}

// The widget over a fake Central Command: by default the login stays
// pending until the test delivers its progress through the service events
// the pump hands on; the war-map requests are recorded.
StatusWidget* make_widget(
    Data* data,
    ServiceResult (*login)(void*, const LoginRequest*) = fake_pending_login,
    const screens::SettingsStore* settings = nullptr
) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    if (settings != nullptr)
        config.settings = *settings;
    config.host.play_sound = host_sound;
    config.service.network_available = fake_network;
    config.service.activate = fake_activate;
    config.service.connected = fake_connected;
    config.service.login = login;
    config.service.send_chat = fake_chat;
    config.service.enter_battles = fake_enter_battles;
    config.service.leave_battles = fake_leave_battles;
    config.service.request_room_list = fake_room_list;
    config.service.join_room = fake_join_room;
    config.service.switch_room = fake_switch_room;
    config.service.leave_room = fake_leave_room;
    config.service.create_room = fake_create_room;
    config.service.request_profile = fake_request_profile;
    config.service.profile_templates = fake_profile_templates;
    config.service.peoplelist_contains = fake_people_contains;
    config.service.peoplelist_insert = fake_people_insert;
    config.service.peoplelist_erase = fake_people_erase;
    config.service.set_events = fake_set_events;
    config.service.pump = fake_pump;
    config.service.area_name = fake_area_name;
    config.service.open_sector_map = fake_open_map;
    config.service.read_sector_map = fake_read_map;
    config.service.close_sector_map = fake_close_map;
    config.service.request_allegiance = fake_request_allegiance;
    config.service.request_census = fake_request_census;
    return boneyards_status_widget_create(config);
}

// Runs the Main Frame until the green layer stops sliding.
void finish_slide(StatusWidget* widget) {
    const ConsoleLayer* layer = widget->system.main_frame->console;
    for (int32_t i = 0; layer->sliding != 0 && i < 100; ++i)
        boneyards_status_widget_tick(widget);
}

// Enters the Boneyards and runs the Main Frame until its login delay ends
// and the green layer has slid over the map for the update console.
MainFrame* enter_boneyards(StatusWidget* widget) {
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    for (int32_t i = 0; i < 16; ++i)
        boneyards_status_widget_tick(widget);
    if (widget->system.main_frame != nullptr)
        finish_slide(widget);
    return widget->system.main_frame;
}

// Runs the update console until its placeholder pass hands over to the login
// console, which shows at once under the covering layer.
void finish_update_console(StatusWidget* widget) {
    MainFrame* frame = widget->system.main_frame;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
}

// Types a member's name and password into the login console and presses
// Enter in the password field once its pulse has enabled LOGIN.
void submit_login(StatusWidget* widget, MainFrame* frame) {
    type(widget, "commander");
    Window* view = frame->active_screen;
    for (int32_t i = 0; i < view->children.count; ++i) {
        Window* child = view->children.items[i];
        if (child->ops->is_class(class_name::radio_button) && child->y == view->y + 0x5c)
            click_window(widget, child);
    }
    for (int32_t i = 0; i < frame->active_screen->children.count; ++i) {
        Window* child = frame->active_screen->children.items[i];
        if (child != widget->desktop.focus && child->ops->is_class(class_name::single_edit) &&
            reinterpret_cast<SingleEdit*>(child)->mask == '*') {
            ScopedServices scope(widget);
            desktop_set_focus(&widget->desktop, child);
        }
    }
    type(widget, "secret");
    boneyards_status_widget_tick(widget);
    press(widget, key::enter);
}

// login_confirmed for user 42 in area 7, then the dossier (USER_STATUS 4).
void deliver_dossier() {
    g_server.connected = true;
    SessionRecord session{};
    session.level = SessionLevel::logged_in;
    session.user_id = 42;
    session.area_id = 7;
    g_server.events.session_changed(g_server.events.user, &session);
    session.level = SessionLevel::dossier_loaded;
    session.matchmaking_enabled = 1;
    g_server.events.session_changed(g_server.events.user, &session);
}

void deliver_allegiances() {
    g_server.events.allegiances(g_server.events.user, "CAI");
}

// A login from the login console to the galactic map, with the map handed
// back and its allegiances applied.
MainFrame* log_in(StatusWidget* widget) {
    MainFrame* frame = enter_boneyards(widget);
    if (frame == nullptr)
        return nullptr;
    finish_update_console(widget);
    submit_login(widget, frame);
    g_server.on_pump = deliver_dossier;
    boneyards_status_widget_tick(widget);
    finish_slide(widget);
    g_server.on_pump = deliver_allegiances;
    boneyards_status_widget_tick(widget);
    return frame;
}

Window* hotspot(MainFrame* frame, int32_t planet) {
    return frame->map_panel->hotspots[planet];
}

bool planet_buttons_are(MainFrame* frame, int32_t state) {
    for (ConsoleButton which : {ConsoleButton::recon, ConsoleButton::chat, ConsoleButton::battle})
        if (console_button(frame, which)->states.current != state)
            return false;
    return true;
}

MainFrameProbe probe_of(StatusWidget* widget) {
    MainFrameProbe probe{};
    ScopedServices scope(widget);
    check(boneyards_status_widget_probe(widget, &probe), "Main Frame probed");
    return probe;
}

bool reads(const char* actual, const char* expected) {
    return std::strcmp(actual, expected) == 0;
}

void deliver_census(
    uint32_t area, int32_t in_progress, int32_t forming, int32_t arm_wins, int32_t core_wins
) {
    CensusReport census{};
    census.area_id = area;
    census.in_progress = in_progress;
    census.forming = forming;
    census.arm_wins = arm_wins;
    census.core_wins = core_wins;
    g_server.events.census(g_server.events.user, &census);
}

// Through the map's hotspots and the census events, the planet summary
// under Quickplay reads the pressed planet's name and owner and the counts
// of each census for it; each side's wins show only for the contested
// planet, whose 50-size sprite frame from the war-map load is the picture.
// Planet 0 is selected again at the end.
void check_planet_summary(StatusWidget* widget, MainFrame* frame, const Data& data) {
    MainFrameProbe probe = probe_of(widget);
    check(
        probe.summary_shown && reads(probe.summary_name, "Planet 0") &&
            reads(probe.summary_owner, "Core") && reads(probe.summary_battles, "0") &&
            reads(probe.summary_forming, "0") && !probe.summary_wins_shown,
        "the pressed planet's name, owner and counts"
    );
    deliver_census(k_first_planet, 2, 1, 4, 6);
    draw_frame(widget, data, "by_planet_core.ppm");
    probe = probe_of(widget);
    check(
        reads(probe.summary_battles, "2") && reads(probe.summary_forming, "1") &&
            !probe.summary_wins_shown,
        "the census counts of a Core planet, without the wins row"
    );

    click_window(widget, hotspot(frame, 2));
    deliver_census(k_first_planet + 2, 3, 12, 41, 7);
    draw_frame(widget, data, "by_planet_contested.ppm");
    probe = probe_of(widget);
    check(
        probe.summary_shown && reads(probe.summary_name, "Planet 2") &&
            reads(probe.summary_owner, "Contested") && reads(probe.summary_battles, "3") &&
            reads(probe.summary_forming, "12") && probe.summary_wins_shown &&
            reads(probe.summary_arm_wins, "41") && reads(probe.summary_core_wins, "7"),
        "a contested planet shows each side's wins"
    );

    const GalaxyGraphics* graphics = widget->galaxy_graphics.get();
    const screens::GalaxyPlanet& planet = widget->navigation->galaxy.planets[2];
    const oa::formats::gaf::Frame* sprite =
        graphics != nullptr
            ? oa::formats::gaf::frame_at(graphics->planet_sequences[2][1], planet.frame)
            : nullptr;
    check(sprite != nullptr, "the contested planet's 50-size sprite");
    if (sprite != nullptr) {
        const oa::formats::gaf::RenderResult rendered = oa::formats::gaf::render_normal(*sprite);
        const Handle image = frame->base.states.images[1];
        bool same = rendered.ok();
        int32_t drawn = 0;
        for (int32_t y = 0x7e; same && y <= 0x9c; ++y) {
            for (int32_t x = 0x23a; x <= 0x26d; ++x) {
                const oa::formats::gaf::RenderedFrame& f = *rendered.frame;
                const int32_t sx = x - 0x254 + f.origin_x;
                const int32_t sy = y - 0x8d + f.origin_y;
                int32_t expected = 0xaf;
                if (sx >= 0 && sy >= 0 && sx < f.width && sy < f.height &&
                    f.pixels[static_cast<std::size_t>(sy * f.width + sx)] != f.transparency_index) {
                    expected = f.pixels[static_cast<std::size_t>(sy * f.width + sx)];
                    ++drawn;
                }
                same = same && image_pixel(widget, image, x, y) == expected;
            }
        }
        check(same && drawn > 100, "the picture area holds the sprite frame over 0xAF");
    }
    click_window(widget, hotspot(frame, 0));
}

// The Chat console closing for the Battle console (chat_console_close): the Chat
// button back on its normal face, both lists emptied and the console hidden.
// The console stays, so a later opening shows the same one, panes included.
void check_chat_console_closed(StatusWidget* widget, MainFrame* frame, ChatView* chat) {
    auto* chat_button =
        reinterpret_cast<ConsoleNavButton*>(console_button(frame, ConsoleButton::chat));
    check(
        chat_button->base.states.images[button_state::unpressed] == chat_button->normal,
        "chat_console_close puts the Chat button back to its normal face"
    );
    check(
        chat->room_list->list.count == 0 && chat->player_list->list.count == 0 &&
            chat->base.visible == 0,
        "chat_console_close empties the frequency and player lists and hides the console"
    );
    boneyards_status_widget_tick(widget);
    check(
        console_open_chat() == chat && chat->base.visible == 0,
        "the closed chat console stays, hidden"
    );
    bool kept = true;
    for (PaneKind kind : {PaneKind::player, PaneKind::create_frequency, PaneKind::frequency_info}) {
        const InfoPane* pane = frame->panes[static_cast<int32_t>(kind)];
        kept = kept && (pane == nullptr || pane->base.parent == &chat->base);
    }
    check(kept, "the closed chat console keeps its panes");
}

// The Battle console lights its button, runs the activity light and enters
// the planet's battles as it opens, replacing the Chat console; Release
// closes it, empties its categories and leaves them. A refused request stops
// the light. Chat then opens again over the map.
void check_battle_console(StatusWidget* widget, MainFrame* frame) {
    screens::BoneyardsScreens* nav = widget->navigation;
    const std::string area = std::to_string(nav->session.area_id);
    g_server.battle_calls.clear();
    g_server.battle_result = ServiceResult::pending;
    auto* chat = reinterpret_cast<ChatView*>(frame->active_screen);
    const InfoPane* create_pane = frame->panes[static_cast<int32_t>(PaneKind::create_frequency)];
    const InfoPane* info_pane = frame->panes[static_cast<int32_t>(PaneKind::frequency_info)];
    {
        ScopedServices scope(widget);
        activity_light_stop(frame->activity_light);
        screens::by_text_row_create(chat->room_list, "Calm", &widget->system.hat_fonts, true);
        screens::by_text_row_create(chat->player_list, "Dan", &widget->system.hat_fonts, true);
    }
    click_window(widget, console_button(frame, ConsoleButton::battle));
    check_chat_console_closed(widget, frame, chat);
    finish_slide(widget);
    auto* battle_button =
        reinterpret_cast<ConsoleNavButton*>(console_button(frame, ConsoleButton::battle));
    check(
        frame->console_screen == console_screen::battle && frame->active_screen != nullptr &&
            frame->active_screen->visible == 1,
        "battle_view_open shows the battle console"
    );
    check(
        battle_button->base.states.images[button_state::unpressed] == battle_button->lit,
        "battle_view_open lights the Battle button"
    );
    check(
        g_server.battle_calls.size() == 1 && g_server.battle_calls[0] == "enter " + area,
        "battle_view_open enters the planet's battles (Rover slot 15)"
    );
    check(frame->activity_light->running == 1, "the light runs while the battles are pending");
    const ServiceEvents& events = g_server.events;
    {
        ScopedServices scope(widget);
        events.battles_done(events.user, false);
        check(
            frame->activity_light->running == 0,
            "battle_list_on_done stops the light once the battles are entered"
        );
        events.battle_info(events.user, 0, "21", "Open", 0, 1, 2);
    }
    check(
        nav->battles.categories[0].battle_count == 1 && nav->battles.rows_dirty,
        "battle_list_on_battle_info files a reported battle under its mode"
    );
    nav->battles.rows_dirty = false;
    nav->battles.host_visible = true;
    g_server.battle_result = ServiceResult::not_connected;
    click_window(widget, console_button(frame, ConsoleButton::release));
    check(frame->console_screen == console_screen::none, "Release closes the battle console");
    check(
        battle_button->base.states.images[button_state::unpressed] == battle_button->normal,
        "battle_view_close puts the Battle button back to its normal face"
    );
    check(
        !nav->battles.host_visible && nav->battles.categories[0].battle_count == 0 &&
            nav->battles.rows_dirty,
        "battle_view_close closes the shown pane, empties the categories "
        "(battle_list_reset_categories) and dirties the rows"
    );
    check(
        g_server.battle_calls.size() == 2 && g_server.battle_calls[1] == "leave " + area,
        "battle_view_close leaves the planet's battles (Rover slot 17)"
    );
    check(frame->activity_light->running == 0, "a refused leave stops the light it started");
    finish_slide(widget);
    click_window(widget, console_button(frame, ConsoleButton::chat));
    finish_slide(widget);
    check(
        frame->console_screen == console_screen::chat && frame->active_screen->visible == 1,
        "chat opens again over the map"
    );
    const auto* reopened = reinterpret_cast<const ChatView*>(frame->active_screen);
    check(
        info_pane != nullptr && &reopened->info_pane->pane == info_pane &&
            &reopened->create_pane->pane == create_pane &&
            info_pane->base.parent == &reopened->base && !window_is_shown(&info_pane->base),
        "main_frame_pane hands the reopened console the panes it built before"
    );
}

bool bounds_are(const Window* w, int32_t x, int32_t y, int32_t width, int32_t height) {
    return w != nullptr && w->x == x && w->y == y && w->width == width && w->height == height;
}

// A static child of `parent` showing `text`, or null.
Window* find_label(StatusWidget* widget, Window* parent, const char* text) {
    ScopedServices scope(widget);
    for (int32_t i = 0; i < parent->children.count; ++i) {
        Window* child = parent->children.items[i];
        if (!child->ops->is_class(class_name::static_text))
            continue;
        const char* caption = child->ops->state_text(child, -1);
        if (caption != nullptr && std::strcmp(caption, text) == 0)
            return child;
    }
    return nullptr;
}

std::string window_text(StatusWidget* widget, Window* w) {
    ScopedServices scope(widget);
    const char* text = w->ops->state_text(w, -1);
    return text != nullptr ? text : "";
}

// The Chat console as chat_console_build builds it (hidden) when CHAT starts the
// slide, in the green layer's rectangle: the separators in its keyed image,
// the ChatList, "Enter Chat >>" in the dim small font with its bottom-left
// corner at (0x81, 0x131), the input line, the big-font titles centred over
// the left column, the frequency list passing its changes on, CREATE, and
// with ConsoleSystem.matchmaking_enabled a shorter player list over MATCHMAKING and its
// Minimize_Button. Nothing asks for the planet's frequencies yet.
void check_chat_console_built(StatusWidget* widget, MainFrame* frame) {
    auto* view = reinterpret_cast<ChatView*>(frame->active_screen);
    check(view != nullptr, "chat console built");
    if (view == nullptr)
        return;
    const Window& c = view->base;
    const Window& layer = frame->console->base;
    const screens::HatFonts& f = widget->system.hat_fonts;
    screens::BoneyardsScreens* nav = widget->navigation;
    check(
        window_is_named(&c, "ChatConsole") &&
            bounds_are(&c, layer.x, layer.y, layer.width, layer.height),
        "the ChatConsole window fills the green layer"
    );
    check(
        c.visible == 0 && frame->console->sliding == 1,
        "chat_console_build builds the console hidden while the green slides in"
    );
    check(
        g_server.room_calls.empty() && !nav->chat.visible &&
            nav->current == screens::ByScreen::chat_frequencies,
        "the chat is current but asks for no frequency before the console opens"
    );
    auto* chat_button =
        reinterpret_cast<ConsoleNavButton*>(console_button(frame, ConsoleButton::chat));
    check(
        view->frame == frame && view->chat_button == &chat_button->base,
        "chat_console_create keeps the Main Frame and its Chat button"
    );
    check(
        chat_button->base.states.images[button_state::unpressed] == chat_button->normal,
        "the Chat button keeps its normal face until the console opens"
    );

    const ListBox& log = view->log->list;
    check(
        window_is_named(&log.base, "ChatList") &&
            bounds_are(&log.base, c.x + 0x7c, c.y + 2, 0x176, 0x122) && log.capacity == 100 &&
            log.bottom_align == 1 && log.log_mode == 1 && view->log->framed == 0 &&
            view->log->forward_changes == 0,
        "the ChatList: 100 lines, bottom-aligned, a log"
    );
    check(console_open_chat() == view, "the new console takes the chat's lines and lists");
    const SingleEdit& input = *view->input;
    check(
        bounds_are(&input.base, c.x + 0xc2, c.y + 0x127, c.width - 0xc2, 0xe) &&
            input.base.font == f.small && input.scroll == 1 && input.max_length == 0xf3 &&
            input.selection_height == 0xe && input.base.ops->key_down == k_single_edit_ops.key_down,
        "the input line: a scrolling SingleEdit in the small font, 243 characters"
    );
    check(
        bounds_are(&view->room_list->list.base, c.x, c.y + 0x15, 0x7b, 0x35) &&
            view->room_list->forward_changes == 1 && view->room_list->framed == 0,
        "the frequency list passes its changes on"
    );
    check(
        bounds_are(view->create_button, c.x + 10, c.y + 0x4a, 0x63, 0x15) &&
            view->create_button->states.current == button_state::unpressed &&
            window_text(widget, view->create_button) == "CREATE",
        "CREATE, a large function button"
    );
    check(
        bounds_are(&view->player_list->list.base, c.x, c.y + 0x76, 0x7b, 0xad) &&
            view->player_list->forward_changes == 0,
        "the player list stops short of MATCHMAKING"
    );
    check(
        view->matchmaking != nullptr &&
            bounds_are(view->matchmaking, c.x + 0x6c, c.y + 0x127, 0xd, 0xc) &&
            view->matchmaking->states.images[button_state::unpressed] >= 0,
        "the Minimize_Button beside MATCHMAKING"
    );

    Window* title = find_label(widget, &view->base, "CHAT FREQUENCIES");
    check(
        title != nullptr && title->x == c.x && title->y == c.y + 6 && title->width == 0x7c &&
            title->font == f.big,
        "the title centred over the left column in the big font"
    );
    Window* prompt = find_label(widget, &view->base, "Enter Chat >>");
    check(
        prompt != nullptr && prompt->x == c.x + 0x81 &&
            prompt->y + prompt->height - 1 == c.y + 0x131 && prompt->width == 0x40 &&
            prompt->font == f.small_dim,
        "the prompt in the dim small font, anchored by its bottom-left corner"
    );
    Window* players = find_label(widget, &view->base, "PLAYERS");
    check(
        players != nullptr && players->x == c.x && players->y == c.y + 0x66 &&
            players->width == 0x7b && players->font == f.big,
        "PLAYERS over the player list"
    );
    Window* matchmaking = find_label(widget, &view->base, "MATCHMAKING");
    check(
        matchmaking != nullptr && matchmaking->x == c.x && matchmaking->y == c.y + 0x127 &&
            matchmaking->width == 0x6c && matchmaking->font == f.big,
        "MATCHMAKING under the player list"
    );

    const Handle image = c.states.images[1];
    const auto pixel = [&](int32_t x, int32_t y) { return image_pixel(widget, image, x, y); };
    check(
        pixel(0x7b, 3) == 0x5d && pixel(0x7b, c.height - 4) == 0x5d && pixel(0x7b, 2) == 9 &&
            pixel(0x7b, c.height - 3) == 9,
        "the column's edge from y 3 to 4 short of the bottom"
    );
    check(
        pixel(0x7c, 0x124) == 0x5d && pixel(c.width - 1, 0x124) == 0x5d && pixel(0, 0x62) == 0x5d &&
            pixel(0x7a, 0x62) == 0x5d && pixel(0x7c, 0x62) == 9,
        "the rules over the input line and over PLAYERS"
    );
    check(
        pixel(0, 0x124) == 0x5d && pixel(0x40, 0x40) == 9,
        "the rule over MATCHMAKING, keyed elsewhere"
    );
}

// main_frame_present_screen calls the open slot chat_console_open once the green covers the map:
// the Chat button shows its lit face, the activity light runs (start_main_frame_light),
// the planet's frequencies are asked for (no preferred one is remembered)
// and the input line takes the focus.
void check_chat_console_opened(StatusWidget* widget, MainFrame* frame) {
    auto* view = reinterpret_cast<ChatView*>(frame->active_screen);
    if (view == nullptr)
        return;
    auto* chat_button =
        reinterpret_cast<ConsoleNavButton*>(console_button(frame, ConsoleButton::chat));
    check(
        view->base.visible == 1 && widget->navigation->chat.visible,
        "chat_console_open shows the console"
    );
    check(
        chat_button->base.states.images[button_state::unpressed] == chat_button->lit,
        "chat_console_open lights the Chat button"
    );
    check(frame->activity_light->running == 1, "chat_console_open starts the activity light");
    const std::string area = std::to_string(widget->navigation->session.area_id);
    check(
        g_server.room_calls == std::vector<std::string>{"rooms(" + area + ")"},
        "chat_console_open asks for the planet's frequencies"
    );
    check(
        widget->desktop.focus == &view->input->base && view->input->base.focused == 1,
        "chat_console_open gives the input line the focus"
    );
}

// chat_print_line: a line sounds GREEN_BUTTON unless it is quiet, then by_list_wrap_text
// wraps it into the ChatList in its colour (the small font by default), in
// rows 0x11 high: the first 4 pixels in, the others as far in as seven
// spaces of the font (indent 8).
void check_chat_lines(StatusWidget* widget, ChatView* view) {
    screens::ChatConsole* chat = &widget->navigation->chat;
    ListBox* log = &view->log->list;
    const screens::HatFonts& f = widget->system.hat_fonts;
    ScopedServices scope(widget);
    g_sounds.clear();
    screens::chat_print_line(chat, "a quiet line", screens::kDefaultTextColor, true);
    check(
        g_sounds.empty() && last_log_line(widget, log) == "a quiet line",
        "a quiet line makes no sound"
    );
    const std::string text =
        "The Core has taken the ridge above the river crossing and every commander on this "
        "frequency is asked to hold the line until the reinforcements from the northern "
        "sector arrive before nightfall";
    const int32_t before = log->count;
    screens::chat_print_line(chat, text.c_str(), screens::kDefaultTextColor, false);
    check(heard("GREEN_BUTTON"), "a line sounds GREEN_BUTTON");
    const int32_t rows = log->count - before;
    check(rows >= 3, "a long line wraps into several rows");
    int32_t indent{}, height{};
    text_measure_string(f.small, "       ", &indent, &height);
    std::string joined;
    bool placed = true;
    for (int32_t i = 0; i < rows; ++i) {
        auto* row = reinterpret_cast<screens::ByTextRow*>(list_box_item(log, before + i));
        placed = placed && row->base.height == 0x11 && row->label->font == f.small &&
                 row->label->x - row->base.x == (i == 0 ? 4 : indent);
        joined += i == 0 ? "" : " ";
        joined += window_text(widget, row->label);
    }
    check(indent > 0 && placed, "the first row 4 pixels in, the rest seven spaces in");
    check(joined == text, "the rows carry the whole line");
    screens::chat_print_line(chat, "[Whisper Lock Deactivated]", f.small_green, true);
    auto* last = reinterpret_cast<screens::ByTextRow*>(list_box_item(log, log->count - 1));
    check(last->label->font == f.small_green, "a line in its own colour");
}

// A page reaching the Main Frame (main_frame_on_alert) while the Chat console is
// shown: MESSAGE, then the line printed in the log in the default colour
// through chat_print_line (GREEN_BUTTON); a guest's (user_id bit 31) and a
// Silenced user's are dropped without a sound.
void check_chat_pages(StatusWidget* widget, ChatView* view) {
    ListBox* log = &view->log->list;
    const ServiceEvents& events = g_server.events;
    const auto page = [&](const char* text, uint32_t sender) {
        ScopedServices scope(widget);
        const AlertReport alert{static_cast<int32_t>(AlertKind::page), text, sender, ""};
        events.system_alert(events.user, &alert);
    };
    g_sounds.clear();
    page("<Page from Dan> over here", 9);
    auto* last = reinterpret_cast<screens::ByTextRow*>(list_box_item(log, log->count - 1));
    check(
        last_log_line(widget, log) == "<Page from Dan> over here" &&
            last->label->font == widget->system.hat_fonts.small,
        "a page is printed in the shown chat log in the default colour"
    );
    check(
        g_sounds == std::vector<std::string>{"MESSAGE", "GREEN_BUTTON"},
        "the page sounds MESSAGE, then the line GREEN_BUTTON"
    );
    check(!boneyards_status_widget_message_open(widget), "no box while the chat shows");
    const int32_t lines = log->count;
    g_server.silenced.push_back(12);
    g_sounds.clear();
    page("<Page from Eve> psst", 12);
    page("<Page from guest> hi", 0x80000009U);
    check(
        log->count == lines && g_sounds.empty() && !boneyards_status_widget_message_open(widget),
        "Silenced users' and guests' pages are dropped"
    );
    g_server.silenced.clear();
}

// chat_console_key_down hands Page Up, Page Down, Up and Down from the focused input
// line to the ChatList. A line printed while the log is scrolled back leaves
// it there: by_list_wrap_text follows the end only from the end.
void check_chat_scroll_keys(StatusWidget* widget, ChatView* view) {
    ListBox* log = &view->log->list;
    check(log->last == log->count - 1 && log->top > log->page, "the overflowing log shows its end");
    const int32_t top = log->top;
    press(widget, key::page_up);
    check(log->top == top - log->page, "Page Up scrolls the log back a page");
    press(widget, key::up);
    check(log->top == top - log->page - 1, "Up scrolls it back a line");
    {
        ScopedServices scope(widget);
        screens::chat_print_line(
            &widget->navigation->chat, "a later line", screens::kDefaultTextColor, true
        );
    }
    check(log->top == top - log->page - 1, "a new line leaves the scrolled-back log where it is");
    press(widget, key::down);
    press(widget, key::page_down);
    press(widget, key::page_down);
    check(log->last == log->count - 1, "Down and Page Down return to the end");
    check(single_edit_text(view->input)[0] == '\0', "the keys leave the input line alone");
}

std::string message_text(StatusWidget* widget) {
    MessageBox* box = message_box_find_open(&widget->desktop);
    Window* text =
        box != nullptr ? window_find_named(&box->base, message_box_name::text, nullptr) : nullptr;
    if (text == nullptr)
        return {};
    ScopedServices scope(widget);
    return text->ops->state_text(text, -1);
}

// chat_console_pulse: CREATE is disabled while the session is entering a
// frequency and enabled again once it is not, only while the console shows.
void check_create_follows_pending(StatusWidget* widget, ChatView* view) {
    screens::ChatSession* session = widget->navigation->chat.session;
    const uint32_t pending = session->pending_room_id;
    session->pending_room_id = 77;
    boneyards_status_widget_tick(widget);
    check(
        view->create_button->states.current == button_state::disabled,
        "CREATE is disabled while a frequency change is pending"
    );
    session->pending_room_id = 0;
    boneyards_status_widget_tick(widget);
    check(
        view->create_button->states.current == button_state::unpressed,
        "CREATE is enabled once no change is pending"
    );
    view->base.visible = 0;
    session->pending_room_id = 77;
    boneyards_status_widget_tick(widget);
    check(
        view->create_button->states.current == button_state::unpressed,
        "a hidden console leaves CREATE alone"
    );
    view->base.visible = 1;
    session->pending_room_id = pending;
    boneyards_status_widget_tick(widget);
}

// MATCHMAKING for a Beta Tester (status 5, the same session bit a status 4
// dossier sets): GREEN_BUTTON and the Matchmaking dialog on its settings face.
void check_matchmaking(StatusWidget* widget, ChatView* view) {
    g_sounds.clear();
    widget->system.matchmaking_enabled = account_sees_unfinished(kAccountStatusBetaTester) ? 1 : 0;
    click_window(widget, view->matchmaking);
    Window* dialog = matchmaking_dialog_find(widget);
    check(
        heard("GREEN_BUTTON") && dialog != nullptr && window_is_shown(dialog) &&
            matchmaking_dialog_searching(dialog) == k_matchmaking_settings &&
            dialog->x == matchmaking_dialog_layout::x &&
            dialog->y == matchmaking_dialog_layout::y &&
            dialog->width == matchmaking_dialog_layout::width &&
            dialog->height == matchmaking_dialog_layout::height &&
            !boneyards_status_widget_message_open(widget),
        "MATCHMAKING opens the Matchmaking dialog on its settings face"
    );
    if (dialog != nullptr)
        dialog->visible = 0;
}

screens::ByChatRoomRow* room_row(ChatView* view, int32_t index) {
    return reinterpret_cast<screens::ByChatRoomRow*>(list_box_item(&view->room_list->list, index));
}

screens::ByPlayerRow* player_row(ChatView* view, int32_t index) {
    return reinterpret_cast<screens::ByPlayerRow*>(list_box_item(&view->player_list->list, index));
}

// The frequency rows' names in list order.
std::string room_rows(ChatView* view) {
    std::string names;
    for (int32_t i = 0; i < view->room_list->list.count; ++i) {
        names += i == 0 ? "" : ",";
        names += room_row(view, i)->name;
    }
    return names;
}

// What the headless checks read of the Chat console through the Main Frame
// probe: a row's centre, whether it is in view, its frequency or user id,
// type, circle, name and count as drawn, the log's rows, and the rows each
// list holds.
void check_chat_probe(StatusWidget* widget, ChatView* view) {
    ChatRowProbe row{};
    const Window& other = room_row(view, 4)->row.base;
    check(
        boneyards_status_widget_chat_row(widget, ChatList::frequencies, 4, &row) && row.shown &&
            row.x == other.x + other.width / 2 && row.y == other.y + other.height / 2 &&
            row.id == 14 && row.type == 2 && row.current && !row.selected &&
            std::string(row.name) == "Other" && std::string(row.count) == "3",
        "the probe reads the session's frequency row"
    );
    check(
        boneyards_status_widget_chat_row(widget, ChatList::frequencies, 0, &row) && !row.shown &&
            row.id == 23 && row.type == 1 && !row.current && std::string(row.name) == "main",
        "a frequency row scrolled out of view reads as hidden"
    );
    check(
        !boneyards_status_widget_chat_row(widget, ChatList::frequencies, 5, &row),
        "no frequency row past the last"
    );
    check(
        boneyards_status_widget_chat_row(widget, ChatList::players, 1, &row) && row.shown &&
            row.id == 10 && row.type == 0 && !row.current && std::string(row.name) == "Eve",
        "the probe reads a player row"
    );
    MainFrameProbe probe{};
    check(
        boneyards_status_widget_probe(widget, &probe) && probe.chat_frequency_rows == 5 &&
            probe.chat_player_rows == 3 && probe.chat_pending_room_id == 0,
        "the probe counts both lists' rows"
    );
    ListBox* log = &view->log->list;
    const int32_t last = log->count - 1;
    const auto* newest = reinterpret_cast<screens::ByTextRow*>(list_box_item(log, last));
    char line[0x100]{};
    check(
        newest != nullptr &&
            boneyards_status_widget_chat_log_line(widget, last, line, sizeof line) &&
            line == window_text(widget, newest->label) &&
            std::string(line) == probe.chat_last_line &&
            !boneyards_status_widget_chat_log_line(widget, log->count, line, sizeof line),
        "the probe reads the log's rows"
    );
}

// The frequency and player lists as the room feeds fill them after an
// entry emptied both (chat_on_join_result). chat_room_row makes each listed frequency's
// BYChatRoomListItem where by_chat_room_row_create puts it (planetary defaults by name,
// user-created, then debriefings) with its listed count, or the members
// counted so far for the session's frequency; chat_player_row gives
// each member with an icon in 1..0x24 a BYPlayerListItem with its rank and
// latency icons and adds one to that count (chat_add_room_population). The listing's
// last row scrolls the session's frequency into view (chat_on_room_listed) and
// chat_room_row_draw circles it. +m and +wl leave the muzzled name dim and the
// locked one green at the next draw (player_row_unselect). A member who leaves and
// a frequency listed as gone lose their rows. On the frequency list
// (chat_console_on_notify) a click opens the frequency's information, a double click
// on another open frequency switches to it (message 300) while one on a
// password frequency only opens its information; CREATE toggles the create
// pane and a player row opens the player's pane, both after GREEN_BUTTON.
void check_chat_lists(StatusWidget* widget, ChatView* view, const Data& data) {
    screens::BoneyardsScreens* nav = widget->navigation;
    screens::ChatConsole& chat = nav->chat;
    const screens::HatFonts& f = widget->system.hat_fonts;
    const uint32_t area_id = nav->session.area_id;
    const std::string area = std::to_string(area_id);
    ListBox* rooms = &view->room_list->list;
    ListBox* players = &view->player_list->list;
    check(
        nav->session.room_id == 14 && rooms->count == 0 && players->count == 0,
        "chat_on_join_result empties both lists as a frequency is entered"
    );
    const ServiceEvents& events = g_server.events;
    {
        ScopedServices scope(widget);
        events.room_member(events.user, 0, 9, "Dan", 2, 3, 0, "C");

        const struct {
            uint32_t id;
            const char* name;
            int32_t users;
            int32_t type;
            bool password;
        } listing[] = {
            {21, "Tactics", 6, 1, false},
            {14, "Other", 40, 2, false},
            {22, "den", 2, 3, true},
            {23, "main", 8, 1, false},
            {24, "War_Room", 3, 1, false},
        };

        for (std::size_t i = 0; i < std::size(listing); ++i)
            events.room_listed(
                events.user,
                0,
                area_id,
                listing[i].id,
                listing[i].name,
                "",
                listing[i].password,
                listing[i].users,
                listing[i].type,
                "",
                i + 1 == std::size(listing),
                false,
                0
            );
        events.room_member(events.user, 0, 10, "Eve", 1, 0x17, 0, "A");
        events.room_member(events.user, 0, 0x80000005u, "Ghost", 1, 2, 0, "U");
        events.room_member(events.user, 0, 11, "Nobody", 1, 0x25, 0, "U");
    }
    check(
        room_rows(view) == "main,Tactics,War_Room,den,Other" && rooms->count == 5,
        "by_chat_room_row_create orders the frequency rows"
    );
    bool counts = true;
    const uint32_t expected_counts[] = {8, 6, 3, 2, 3};
    for (int32_t i = 0; i < 5; ++i) {
        screens::ByChatRoomRow* row = room_row(view, i);
        counts = counts && row->users == static_cast<int32_t>(expected_counts[i]) &&
                 window_text(widget, row->count_label) == std::to_string(expected_counts[i]) &&
                 window_text(widget, row->name_label) == row->name;
    }
    check(counts, "by_chat_room_row_set_count shows the listed counts beside the names");
    check(
        players->count == 3 && room_row(view, 4)->users == players->count,
        "the session's frequency counts the members seen (chat_add_room_population)"
    );
    check(
        rooms->top == 2 && rooms->last == 4,
        "chat_on_room_listed scrolls the session's frequency into view"
    );
    check(view->room_list->selected_item == nullptr, "the listing selects no frequency");
    check_chat_probe(widget, view);

    const struct {
        uint32_t id;
        int32_t icon;
        int32_t latency;
    } members[] = {{9, 3, 2}, {10, 0x17, 1}, {0x80000005u, 2, 1}};

    bool icons = true;
    for (int32_t i = 0; i < 3; ++i) {
        screens::ByPlayerRow* row = player_row(view, i);
        icons = icons && row->user_id == members[i].id &&
                row->icons.user_frame == members[i].icon &&
                row->icons.latency_frame == members[i].latency &&
                row->icons.user_icon->states.images[1] >= 0 &&
                row->icons.latency_icon->states.images[1] >= 0;
    }
    check(
        icons,
        "player_row_set_user_icon and player_row_set_latency show the members' rank and latency "
        "icons"
    );
    {
        ScopedServices scope(widget);
        events.chat_message(events.user, false, 10, "steady");
    }
    check(
        last_log_line(widget, &view->log->list) == "Eve [SysOp] : steady",
        "a SysOp's line carries the tag"
    );

    type(widget, "+m ghost");
    press(widget, key::enter);
    type(widget, "+wl eve");
    press(widget, key::enter);
    check(
        player_row(view, 2)->silenced && player_row(view, 1)->whisper_locked &&
            !player_row(view, 0)->silenced && !player_row(view, 0)->whisper_locked,
        "chat_toggle_silence and chat_set_whisper_lock flag the rows"
    );
    check(draw_frame(widget, data, "by_chat_lists.ppm") > 100000, "chat lists drawn");
    check(
        player_row(view, 0)->name_label->font == f.small &&
            player_row(view, 1)->name_label->font == f.small_green &&
            player_row(view, 2)->name_label->font == f.small_dim,
        "player_row_unselect: locked green, muzzled dim"
    );
    check(
        room_row(view, 4)->icon->states.images[1] == room_row(view, 4)->current_icon &&
            room_row(view, 3)->icon->states.images[1] == room_row(view, 3)->other_icon &&
            room_row(view, 2)->icon->states.images[1] == room_row(view, 2)->other_icon,
        "chat_room_row_draw circles the session's frequency"
    );

    {
        ScopedServices scope(widget);
        events.room_member(events.user, 1, 9, "Dan", 0, 0, 0, "C");
        events.room_listed(
            events.user, 1, area_id, 21, "Tactics", "", false, 6, 1, "", false, false, 0
        );
    }
    check(
        players->count == 2 && player_row(view, 0)->user_id == 10 && room_row(view, 3)->users == 2,
        "a member who leaves loses the row and the count"
    );
    check(room_rows(view) == "main,War_Room,den,Other", "a frequency listed as gone loses its row");

    screens::ByChatRoomRow* den = room_row(view, 2);
    click_window(widget, den->name_label);
    check(
        view->room_list->selected_item == &den->row.base && chat.selected_room == 2 &&
            chat.room_info_visible && chat.room_info_room_id == 22,
        "a click on a frequency opens its information"
    );
    const std::size_t calls = g_server.room_calls.size();
    double_click_window(widget, den->name_label);
    check(
        g_server.room_calls.size() == calls && chat.room_info_visible,
        "a double click on a password frequency only shows its information"
    );

    g_sounds.clear();
    click_window(widget, view->create_button);
    check(
        heard("GREEN_BUTTON") && chat.create_dialog.visible && !chat.room_info_visible,
        "CREATE closes the information and shows the create pane"
    );
    click_window(widget, view->create_button);
    check(!chat.create_dialog.visible, "CREATE again hides it");

    g_sounds.clear();
    click_window(widget, player_row(view, 1)->name_label);
    check(
        heard("GREEN_BUTTON") && chat.pane.visible && chat.pane.user_id == 0x80000005u,
        "a player row opens the player's pane"
    );

    screens::ByChatRoomRow* war_room = room_row(view, 1);
    double_click_window(widget, war_room->name_label);
    check(
        g_server.room_calls.back() == "switch(" + area + ",24 from " + area + ",14)" &&
            nav->session.pending_room_id == 24 && chat.selected_room == 1,
        "a double click on another frequency switches to it"
    );
    MainFrameProbe switching{};
    check(
        boneyards_status_widget_probe(widget, &switching) && switching.chat_pending_room_id == 24 &&
            switching.chat_room_id == 14,
        "the probe reads the frequency the switch awaits"
    );
    {
        ScopedServices scope(widget);
        events.room_entered(events.user, 1);
        desktop_set_focus(&widget->desktop, &view->input->base);
    }
    check(
        nav->session.room_id == 14 && rooms->count == 0 && players->count == 2,
        "a refused switch keeps the frequency and asks for the list again"
    );
    type(widget, "-wl");
    press(widget, key::enter);
}

bool entry_at(
    const SingleEdit* entry,
    const Window& pane,
    int32_t dx,
    int32_t dy,
    int32_t width,
    int32_t max_length
) {
    return bounds_are(&entry->base, pane.x + dx, pane.y + dy, width, 0x15) &&
           entry->max_length == max_length && entry->scroll == 1;
}

// A caption whose right edge is at `right` and top at `top` in the pane.
bool caption_ends_at(
    StatusWidget* widget, Window* pane, const char* text, int32_t right, int32_t top
) {
    const Window* label = find_label(widget, pane, text);
    return label != nullptr && label->x + label->width - 1 == pane->x + right &&
           label->y == pane->y + top && label->height == 0x15 &&
           label->font == widget->system.hat_fonts.medium;
}

// chat_console_open takes the Main Frame's create-frequency (kind 4) and frequency information
// (kind 5) panes from its getter main_frame_pane, which builds each hidden in the frame the first
// time (create_frequency_pane_create, frequency_info_pane_create) over the info pane base
// info_pane_init and makes it a child of the console, as it does the player pane (kind 3,
// player_pane_create). A click on a frequency opens the information pane (info_pane_open_slot),
// whose opening moves the ChatList under it (info_pane_open), and shows the frequency in it
// (frequency_info_pane_show); its GO goes to the frequency with the typed password
// (info_pane_on_notify, message 300) once the pulse (info_pane_pulse) allows it. The Close_Button
// closes a pane with INFO_PANE_CLOSE (info_pane_on_close_click, info_pane_close) and the
// information pane's closing drops the frequency selection. CREATE swaps in the create pane, whose
// GO asks for the frequency (create_pane_on_notify, slot 69) and whose pulse (create_pane_pulse)
// holds GO and the entries while the request is out; the answer (chat_on_room_created) reports a
// refusal or goes to the new frequency with the password, and the entry closes the pane
// (chat_on_join_result).
void check_chat_panes(StatusWidget* widget, MainFrame* frame, ChatView* view, const Data& data) {
    screens::BoneyardsScreens* nav = widget->navigation;
    screens::ChatConsole& chat = nav->chat;
    const screens::HatFonts& f = widget->system.hat_fonts;
    const uint32_t area_id = nav->session.area_id;
    const std::string area = std::to_string(area_id);
    const Window& c = view->base;
    FrequencyInfoPane* info = view->info_pane;
    CreateFrequencyPane* create = view->create_pane;
    check(
        info != nullptr && create != nullptr && view->player_pane != nullptr &&
            frame->panes[static_cast<int32_t>(PaneKind::frequency_info)] == &info->pane &&
            frame->panes[static_cast<int32_t>(PaneKind::create_frequency)] == &create->pane &&
            frame->panes[static_cast<int32_t>(PaneKind::player)] == &view->player_pane->pane,
        "chat_console_open takes the frame's player, create-frequency and information panes "
        "(main_frame_pane)"
    );
    if (info == nullptr || create == nullptr)
        return;
    Window* info_window = &info->pane.base;
    Window* create_window = &create->pane.base;
    check(
        info_window->parent == &c && create_window->parent == &c,
        "main_frame_pane makes the panes children of the console"
    );
    check(
        window_is_shown(info_window) && !window_is_shown(create_window) && info->room_id == 24,
        "the first click of the double click on War_Room showed its information"
    );
    check(
        bounds_are(info_window, 0x93, 0x7e, 0x176, 0x9d) && info_window->x == c.x + 0x7c &&
            bounds_are(create_window, 0x93, 0x7e, 0x176, 0x9d),
        "info_pane_init places a pane over the top of the ChatList"
    );
    const Window* close = info->pane.close_button;
    check(
        bounds_are(close, 0x93 + 0x166, 0x7e + 6, 0xc, 0xd) &&
            close->states.images[button_state::unpressed] >= 0 && close->parent == info_window,
        "info_pane_init gives the pane its Close_Button"
    );
    const Window* title = find_label(widget, info_window, "CHANGE FREQUENCY");
    check(
        title != nullptr && title->x == 0x93 && title->y == 0x7e + 6 && title->width == 0x166 &&
            title->font == f.big &&
            find_label(widget, create_window, "CREATE FREQUENCY") != nullptr,
        "the pane titles centred over the pane in the big font"
    );
    const Handle image = info_window->states.images[1];
    check(
        image >= 0 && image == create_window->states.images[1] &&
            image_pixel(widget, image, 0, 0x9c) == 0x5d &&
            image_pixel(widget, image, 0x175, 0x9c) == 0x5d &&
            image_pixel(widget, image, 0x40, 0x9b) == 9,
        "the panes' image (console_open_main_frame): keyed, a rule along its bottom"
    );

    const Window& p = *info_window;
    const int32_t row_width = 0x176 - 0xc;
    check(
        bounds_are(info->name_label, p.x + 6, p.y + 0x13, row_width, 0x10) &&
            info->name_label->font == f.small &&
            bounds_are(&info->type->base, p.x + 6, p.y + 0x23, row_width, 0x10) &&
            bounds_are(&info->creator->base, p.x + 6, p.y + 0x33, row_width, 0x10) &&
            bounds_are(&info->population_field->base, p.x + 6, p.y + 0x43, row_width, 0x10) &&
            bounds_are(info->description[2], p.x + 6, p.y + 0x73, row_width, 0x10),
        "frequency_info_pane_create: the name, the three fields and the description rows"
    );
    const Window* password_label = info->password_label;
    check(
        password_label->x == p.x + 6 && password_label->y == p.y + 0x83 &&
            password_label->font == f.medium &&
            entry_at(info->password, p, password_label->width + 9, 0x83, 0x6e, 0xf) &&
            bounds_are(info->go, p.x + 0x100, p.y + 0x83, 0x63, 0x15) &&
            window_text(widget, info->go) == "GO" &&
            frame->panes[static_cast<int32_t>(PaneKind::frequency_info)] != nullptr,
        "frequency_info_pane_create: the password entry beside its caption, and GO"
    );

    const ServiceEvents& events = g_server.events;
    const char* description = "Where the pack plans its raids on the northern ridge before dawn "
                              "and trades maps of every canyon route";
    {
        ScopedServices scope(widget);
        events.room_listed(
            events.user, 0, area_id, 23, "main", "", false, 8, 1, "", false, false, 0
        );
        events.room_listed(
            events.user, 0, area_id, 22, "den", description, true, 2, 3, "Dan", false, false, 0
        );
        events.room_listed(
            events.user, 0, area_id, 14, "Other", "", false, 3, 2, "", true, false, 0
        );
    }
    check(room_rows(view) == "main,den,Other", "the frequencies are listed again");
    ListBox* log = &view->log->list;
    g_sounds.clear();
    click_window(widget, room_row(view, 1)->name_label);
    check(
        window_is_shown(info_window) && !window_is_shown(create_window) && chat.room_info_visible,
        "a click on a frequency opens its information (info_pane_open_slot)"
    );
    check(
        bounds_are(&log->base, c.x + 0x7c, c.y + 0x9b, 0x176, 0x89),
        "info_pane_open moves the ChatList down under the pane and shrinks it"
    );
    check(
        heard("GREEN_BUTTON") && info->room_id == 22 &&
            window_text(widget, info->name_label) == "den" &&
            window_text(widget, info->type->value) == "User Created" &&
            window_text(widget, info->creator->value) == "Dan" &&
            window_text(widget, info->population_field->value) == "2",
        "frequency_info_pane_show: a user-created frequency by its tag, with its population"
    );
    check(
        window_is_shown(info->password_label) && window_is_shown(&info->password->base),
        "a frequency that needs a password shows the password entry"
    );
    type(widget, "z");
    check(
        std::strcmp(single_edit_text(info->password), "z") == 0,
        "change frequency takes a password without a click"
    );
    single_edit_set_text(info->password, "");
    std::string lines = window_text(widget, info->description[0]);
    for (int32_t i = 1; i < 3; ++i)
        if (!window_text(widget, info->description[i]).empty()) {
            lines += ' ';
            lines += window_text(widget, info->description[i]);
        }
    check(
        !window_text(widget, info->description[1]).empty() && lines == description,
        "the description wrapped into the rows"
    );
    check(
        info->go->states.current == button_state::disabled,
        "info_pane_pulse turns GO off while the password is empty"
    );
    draw_frame(widget, data, "by_chat_info_pane.ppm");

    g_sounds.clear();
    click_window(widget, info->pane.close_button);
    check(
        heard("INFO_PANE_CLOSE") && !window_is_shown(info_window) && !chat.room_info_visible,
        "info_pane_on_close_click: the Close_Button closes the pane with INFO_PANE_CLOSE"
    );
    check(
        bounds_are(&log->base, c.x + 0x7c, c.y + 2, 0x176, 0x122),
        "info_pane_close gives the ChatList its place back"
    );
    check(
        view->room_list->selected_item == nullptr && chat.selected_room == -1,
        "the information closing drops the frequency selection (chat_console_on_notify, 0xC9)"
    );

    click_window(widget, room_row(view, 0)->name_label);
    check(
        window_is_shown(info_window) && info->room_id == 23 &&
            window_text(widget, info->type->value) == "Planetary Default" &&
            window_text(widget, info->creator->value) == "System" &&
            !window_is_shown(&info->password->base) &&
            window_text(widget, info->description[0]).empty(),
        "another frequency's click opens the pane again: a planetary default by the System"
    );
    const std::string password_kept = single_edit_text(info->password);
    const std::string chat_kept = single_edit_text(view->input);
    type(widget, "q");
    check(
        std::strcmp(single_edit_text(info->password), password_kept.c_str()) == 0 &&
            widget->desktop.focus != &info->password->base,
        "a frequency with no password does not take typing in the password"
    );
    if (std::strcmp(single_edit_text(view->input), chat_kept.c_str()) != 0)
        single_edit_set_text(view->input, chat_kept.c_str());
    g_sounds.clear();
    click_window(widget, room_row(view, 0)->name_label);
    check(
        !heard("GREEN_BUTTON") && info->room_id == 23, "a click on the selected row changes nothing"
    );
    click_window(widget, room_row(view, 1)->name_label);
    check(
        heard("GREEN_BUTTON") && info->room_id == 22 && window_is_shown(&info->password->base),
        "and back to the password frequency"
    );
    click_window(widget, &info->password->base);
    type(widget, "ridge");
    boneyards_status_widget_tick(widget);
    check(info->go->states.current == button_state::unpressed, "a typed password turns GO on");
    check(
        info->password->mask == '*' && draws_password(widget, info->password, "ridge"),
        "change frequency password draws asterisks and keeps ridge"
    );
    click_window(widget, info->go);
    check(
        g_server.room_calls.back() == "switch(" + area + ",22 from " + area + ",14)" &&
            g_server.switch_password == "ridge" && nav->session.pending_room_id == 22,
        "info_pane_on_notify: GO goes to the frequency with the typed password"
    );
    check(info->go->states.current == button_state::disabled, "the pending switch turns GO off");
    {
        ScopedServices scope(widget);
        events.room_entered(events.user, 1);
    }
    check(
        nav->session.pending_room_id == 0 && window_is_shown(info_window),
        "a refused switch leaves the information up"
    );

    g_sounds.clear();
    click_window(widget, view->create_button);
    check(
        heard("GREEN_BUTTON") && window_is_shown(create_window) && !window_is_shown(info_window) &&
            !chat.room_info_visible && chat.create_dialog.visible,
        "CREATE closes the information and opens the create pane (create_pane_open)"
    );
    check(
        bounds_are(&log->base, c.x + 0x7c, c.y + 0x9b, 0x176, 0x89),
        "the ChatList makes room for the create pane"
    );
    const Window& q = *create_window;
    check(
        entry_at(create->name, q, 0x78, 0x23, 0x64, 0x17) &&
            entry_at(create->password, q, 0x78, 0x3b, 0x64, 0xf) &&
            entry_at(create->description, q, 0x78, 0x53, 0x96, 0x7f) &&
            create->name->base.font == f.small,
        "create_frequency_pane_create: the name, password and description entries"
    );
    check(
        caption_ends_at(widget, create_window, "Frequency Name", 0x75, 0x23) &&
            caption_ends_at(widget, create_window, "Password", 0x75, 0x3b) &&
            caption_ends_at(widget, create_window, "Description", 0x75, 0x53),
        "create_frequency_pane_create: the captions end just left of their entries"
    );
    check(
        bounds_are(create->go, q.x + 0x78, q.y + 0x6c, 0x63, 0x15) &&
            window_text(widget, create->go) == "GO" &&
            create->go->states.current == button_state::disabled,
        "create_frequency_pane_create: GO under the entries, off without a name"
    );
    type(widget, "F");
    check(
        std::strcmp(single_edit_text(create->name), "F") == 0,
        "create frequency takes a name without a click"
    );
    single_edit_set_text(create->name, "");
    click_window(widget, &create->name->base);
    type(widget, "Forward");
    click_window(widget, &create->password->base);
    type(widget, "pw1");
    check(
        create->password->mask == '*' && create->name->mask == '\0' &&
            create->description->mask == '\0' && draws_password(widget, create->password, "pw1"),
        "create frequency password draws asterisks and keeps pw1"
    );
    click_window(widget, &create->description->base);
    type(widget, "ridge staging");
    boneyards_status_widget_tick(widget);
    check(
        create->go->states.current == button_state::unpressed &&
            create->name->base.states.current == 1,
        "create_pane_pulse: a name turns GO on"
    );
    draw_frame(widget, data, "by_chat_create_pane.ppm");
    g_sounds.clear();
    click_window(widget, create->go);
    check(
        heard("GREEN_BUTTON") &&
            g_server.room_calls.back() == "create(" + area + ",Forward,ridge staging,pw1)" &&
            chat.create_dialog.submitting,
        "create_pane_on_notify: GO asks Rover to create the frequency (slot 69)"
    );
    boneyards_status_widget_tick(widget);
    check(
        create->go->states.current == button_state::disabled &&
            create->name->base.states.current == 0 && create->description->base.states.current == 0,
        "create_pane_pulse holds GO and the entries while the request is out"
    );
    {
        ScopedServices scope(widget);
        events.room_created(events.user, 0, 0, "Forward");
    }
    check(
        boneyards_status_widget_message_open(widget) &&
            message_text(widget) == "Unable to Create New Frequency" &&
            !chat.create_dialog.submitting && window_is_shown(create_window),
        "chat_on_room_created reports a refused frequency"
    );
    press(widget, key::enter);
    boneyards_status_widget_tick(widget);
    check(
        create->go->states.current == button_state::unpressed &&
            std::string(single_edit_text(create->name)) == "Forward",
        "the entries keep the refused frequency for another try"
    );
    click_window(widget, create->go);
    {
        ScopedServices scope(widget);
        events.room_created(events.user, area_id, 30, "Forward");
    }
    check(
        g_server.room_calls.back() == "switch(" + area + ",30 from " + area + ",14)" &&
            g_server.switch_password == "pw1" && nav->session.pending_room_id == 30,
        "chat_on_room_created goes to the new frequency with its password"
    );
    {
        ScopedServices scope(widget);
        events.room_entered(events.user, 0);
    }
    check(
        nav->session.room_id == 30 && !window_is_shown(create_window) &&
            !chat.create_dialog.visible &&
            bounds_are(&log->base, c.x + 0x7c, c.y + 2, 0x176, 0x122),
        "entering it closes the create pane (chat_on_join_result)"
    );
    {
        ScopedServices scope(widget);
        screens::chat_go_to_room(&chat, area_id, 14, nullptr, "Other");
        events.room_entered(events.user, 0);
        desktop_set_focus(&widget->desktop, &view->input->base);
    }
    check(nav->session.room_id == 14, "back on the first frequency");
}

// The markup the view holds, as its parser left it (NULs end its words).
std::string_view shown_markup(const HtmlView* view) {
    const std::vector<char>& buffer = view->state->doc.buffer;
    return std::string_view(buffer.data(), buffer.size());
}

bool shows(const HtmlView* view, const char* text) {
    return shown_markup(view).find(text) != std::string_view::npos;
}

// The player pane (kind 3) as player_pane_create builds it: the rank icon at (5, 7)
// with the name beside it, the rank title, the insignia at (5, 0x29) and the
// clan down the left in the small font, Whisper Lock and Muzzle in the dim
// small font, PAGE at (5, 0x83), and the General, Ladder and Awards captions
// in the button font at x 0x67, 0xBC and 0x111 under invisible buttons,
// over the HTML view at (0x6B, 0x19). The list's selection starts on the
// first row chat_player_row added, and a click there, then a second click on
// that same row, each ask for his profile and show "ACCESSING...". A click
// on the second row asks for his (chat_pane_begin_profile) and shows
// "ACCESSING..."; the reply fills the pane (chat_pane_on_profile). The tabs switch
// the page (player_pane_on_notify), PAGE fills the input line, the Muzzle box puts the
// player on the Silenced list (his row dims and his lines are dropped) and
// the Whisper Lock box locks whispers onto him (his row turns green and a
// typed line goes to him); the Close_Button closes the pane and drops the
// player list's selection (player_pane_close, chat_handle_pane_message).
void check_chat_player_pane(
    StatusWidget* widget, MainFrame* frame, ChatView* view, const Data& data
) {
    screens::BoneyardsScreens* nav = widget->navigation;
    screens::ChatConsole& chat = nav->chat;
    const screens::HatFonts& f = widget->system.hat_fonts;
    PlayerPaneView* pane = view->player_pane;
    if (pane == nullptr)
        return;
    Window* w = &pane->pane.base;
    const Window& p = *w;
    check(
        frame->panes[static_cast<int32_t>(PaneKind::player)] == &pane->pane &&
            w->parent == &view->base && !window_is_shown(w) &&
            bounds_are(w, 0x93, 0x7e, 0x176, 0x9d),
        "main_frame_pane builds the player pane hidden over the ChatList"
    );
    check(
        bounds_are(pane->rank_icon, p.x + 5, p.y + 7, 0xb, 0xb) && pane->rank_icon->visible == 0 &&
            bounds_are(pane->name, p.x + 0x12, p.y + 7, 0x53, 0xb) && pane->name->font == f.small &&
            pane->rank->x == p.x + 5 && pane->rank->y == p.y + 0x18 && pane->rank->width == 0x60 &&
            pane->rank->font == f.small &&
            bounds_are(pane->insignia, p.x + 5, p.y + 0x29, 0x20, 0x20) &&
            pane->insignia->visible == 0 && pane->clan->x == p.x + 5 &&
            pane->clan->y == p.y + 0x4c && pane->clan->width == 0x60,
        "player_pane_create: the rank icon, name, rank, insignia and clan down the left"
    );
    check(
        pane->whisper_lock->base.x == p.x + 5 && pane->whisper_lock->base.y == p.y + 0x63 &&
            pane->whisper_lock->base.font == f.small_dim &&
            window_text(widget, &pane->whisper_lock->base) == "Whisper Lock" &&
            pane->muzzle->base.x == p.x + 5 && pane->muzzle->base.y == p.y + 0x73 &&
            window_text(widget, &pane->muzzle->base) == "Muzzle" &&
            bounds_are(pane->page, p.x + 5, p.y + 0x83, 0x39, 0x15) &&
            window_text(widget, pane->page) == "PAGE",
        "player_pane_create: Whisper Lock and Muzzle, then PAGE"
    );
    const char* captions[] = {"General", "Ladder", "Awards"};
    const char* names[] = {"GeneralTabBtn", "LadderTabBtn", "AwardsTabBtn"};
    const int32_t tab_x[] = {0x67, 0xbc, 0x111};
    bool tabs = true;
    for (int32_t i = 0; i < 3; ++i)
        tabs = tabs && bounds_are(pane->tabs[i], p.x + tab_x[i], p.y + 3, 0x55, 0x13) &&
               window_is_named(pane->tabs[i], names[i]) &&
               bounds_are(pane->tab_labels[i], p.x + tab_x[i], p.y + 3, 0x55, 0x13) &&
               window_text(widget, pane->tab_labels[i]) == captions[i] &&
               pane->tab_labels[i]->font == f.button_normal;
    check(tabs, "player_pane_create: the General, Ladder and Awards tabs");
    check(
        bounds_are(&pane->html->base, p.x + 0x6b, p.y + 0x19, 0x108, 0x81) &&
            pane->html->base.visible == 1 && pane->arm_insignia >= 0 && pane->core_insignia >= 0,
        "player_pane_create: the HTML view, and the Insignia frames kept"
    );

    const ServiceEvents& events = g_server.events;
    {
        ScopedServices scope(widget);
        events.room_member(events.user, 0, 9, "Dan", 2, 3, 0, "C");
        events.room_member(events.user, 0, 12, "Sentinel", 1, 0x12, 0, "A");
    }
    g_server.profile_requests.clear();
    ListBox* log = &view->log->list;
    check(
        view->player_list->list.selected == 0 && chat.player_total == 2 &&
            player_row(view, 0)->user_id == 9 && player_row(view, 1)->user_id == 12,
        "chat_player_row fills Dan then Sentinel, and the selection starts on the first row"
    );
    click_window(widget, player_row(view, 0)->name_label);
    check(
        g_server.profile_requests.size() == 1 && g_server.profile_requests[0].user_id == 9 &&
            g_server.profile_requests[0].context == &chat && window_is_shown(w) &&
            chat.pane.visible && chat.pane.user_id == 9 && shows(pane->html, "ACCESSING..."),
        "a click on the first player row asks for his profile and shows ACCESSING..."
    );
    click_window(widget, player_row(view, 0)->name_label);
    check(
        g_server.profile_requests.size() == 2 && g_server.profile_requests[1].user_id == 9 &&
            g_server.profile_requests[1].context == &chat && chat.pane.user_id == 9 &&
            shows(pane->html, "ACCESSING..."),
        "a second click on the same row asks for his profile again"
    );
    g_server.profile_requests.clear();
    click_window(widget, player_row(view, 1)->name_label);
    check(
        g_server.profile_requests.size() == 1 && g_server.profile_requests[0].user_id == 12 &&
            g_server.profile_requests[0].context == &chat,
        "a click on the second player row asks for his profile (chat_pane_begin_profile)"
    );
    check(
        window_is_shown(w) && chat.pane.visible && !window_is_shown(pane->rank_icon) &&
            shows(pane->html, "ACCESSING...") &&
            bounds_are(&log->base, view->base.x + 0x7c, view->base.y + 0x9b, 0x176, 0x89),
        "the pane opens over the ChatList showing ACCESSING..."
    );
    check(
        pane->page->states.current == button_state::disabled &&
            pane->whisper_lock->base.states.current == check_state::disabled &&
            pane->muzzle->base.states.current == check_state::disabled &&
            pane->tabs[0]->states.current == button_state::disabled,
        "the pane's controls are off while the profile is out"
    );

    by_test::ProfileReply duel{{"user_id", "12"}, {"RANK", "7"}};
    by_test::ProfileReply profile{
        {"user_id", "12"},
        {"LOGINNAME", "Sentinel"},
        {"ICON", "18"},
        {"ALLEGIANCE", "Arm"},
        {"CLAN", "Iron Legion"},
        {"TAG", "Never surrender"},
    };
    {
        ScopedServices scope(widget);
        const by_test::ProfileRequestSeen& request = g_server.profile_requests[0];
        const screens::ProfileFields duel_fields = duel.fields();
        request.on_ladder(request.context, 1, 0, &duel_fields);
        const screens::ProfileFields fields = profile.fields();
        request.on_profile(request.context, true, &fields);
    }
    check(
        window_text(widget, pane->name) == "Sentinel" && pane->name->font == f.small_blue &&
            window_text(widget, pane->rank) == "Supreme Commander" &&
            pane->rank->font == f.small_blue && window_text(widget, pane->clan) == "Iron Legion",
        "chat_pane_on_profile: the name and rank in the Arm colour, and the clan"
    );
    check(
        pane->rank_icon->visible == 1 && pane->rank_frame == 0x12 &&
            pane->rank_icon->states.images[1] >= 0 && pane->insignia->visible == 1 &&
            pane->insignia->states.images[1] == pane->arm_insignia,
        "chat_pane_on_profile: the User_Icons rank icon and the Arm insignia"
    );
    check(
        std::string(chat.pane.html) == "<html><BODY>General: Never surrender<P></BODY></html>" &&
            shows(pane->html, "General:") && shows(pane->html, "surrender") &&
            pane->tab_labels[0]->font == f.button_hover &&
            pane->tab_labels[1]->font == f.button_normal,
        "chat_pane_on_profile: the General page, its caption lit"
    );
    check(
        pane->page->states.current == button_state::unpressed &&
            pane->whisper_lock->base.states.current == check_state::unchecked_up &&
            pane->muzzle->base.states.current == check_state::unchecked_up &&
            pane->tabs[2]->states.current == button_state::unpressed,
        "chat_pane_on_profile turns PAGE, the boxes and the tabs on for another player"
    );
    std::vector<uint8_t> pixels = draw_pixels(widget);
    const auto pixel = [&](int32_t x, int32_t y) { return pixels[y * k_frame_width + x]; };
    check(
        pixel(p.x + 0xc0, p.y + 0x15) == 0x5d && pixel(p.x + 0x115, p.y + 0x15) == 0x5d &&
            pixel(p.x + 0x69, p.y + 0x15) != 0x5d && pixel(p.x + 0x67, p.y + 0x60) == 0x5d &&
            pixel(p.x + 0xbc, p.y + 0x5) == 0x5d,
        "player_pane_draw rules the tabs, open under the chosen one"
    );
    draw_frame(widget, data, "by_chat_player_pane.ppm");

    click_window(widget, pane->tabs[1]);
    check(
        std::string(chat.pane.html) == "<html><BODY><b>Duel</b> rank 7<P></BODY></html>" &&
            shows(pane->html, "Duel") && shows(pane->html, "rank") &&
            !shows(pane->html, "surrender") && pane->tab_labels[1]->font == f.button_hover &&
            pane->tab_labels[0]->font == f.button_normal && chat.pane.tab == 1,
        "player_pane_on_notify: the Ladder tab shows the ladder page"
    );
    pixels = draw_pixels(widget);
    check(
        pixel(p.x + 0x69, p.y + 0x15) == 0x5d && pixel(p.x + 0xc0, p.y + 0x15) != 0x5d,
        "the rule opens under the Ladder tab"
    );

    click_window(widget, pane->page);
    check(
        std::string(single_edit_text(view->input)) == "+Page Sentinel " &&
            widget->desktop.focus == &view->input->base,
        "PAGE puts '+Page Sentinel ' in the focused input line (chat_handle_pane_message)"
    );
    single_edit_set_text(view->input, "");

    click_window(widget, &pane->muzzle->base);
    check(
        g_server.silenced == std::vector<uint32_t>{12} && player_row(view, 1)->silenced &&
            pane->muzzle->base.states.current == check_state::checked_up,
        "Muzzle puts him on the Silenced list (chat_toggle_silence)"
    );
    // The clicked row is the selected one, which takes the light font; with
    // the selection elsewhere the next draw shows the muzzle.
    view->player_list->selected_item = nullptr;
    draw_frame(widget, data, "by_chat_player_muzzled.ppm");
    check(
        player_row(view, 1)->name_label->font == f.small_dim, "his row dims (player_row_unselect)"
    );
    const int32_t lines = log->count;
    {
        ScopedServices scope(widget);
        events.chat_message(events.user, false, 12, "can anyone hear me");
    }
    check(log->count == lines, "his lines are dropped while he is muzzled (chat_receive_message)");

    click_window(widget, &pane->whisper_lock->base);
    check(
        chat.whisper_lock_user == 12 && player_row(view, 1)->whisper_locked &&
            pane->whisper_lock->base.states.current == check_state::checked_up &&
            last_log_line(widget, log) == "[Whisper Lock on 'Sentinel' Activated]",
        "Whisper Lock locks whispers onto him (chat_set_whisper_lock)"
    );
    draw_frame(widget, data, "by_chat_player_locked.ppm");
    check(
        player_row(view, 1)->name_label->font == f.small_green,
        "his row turns green (player_row_unselect)"
    );
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, &view->input->base);
    }
    type(widget, "ridge at dawn");
    press(widget, key::enter);
    check(
        g_server.chat_target == 12 && g_server.last_chat == "<Whispered> ridge at dawn" &&
            last_log_line(widget, log) == "<You whispered to Sentinel> ridge at dawn",
        "the next line goes to him as '+w Sentinel' (chat_console_submit)"
    );
    click_window(widget, &pane->whisper_lock->base);
    click_window(widget, &pane->muzzle->base);
    check(
        chat.whisper_lock_user == 0 && g_server.silenced.empty() &&
            pane->whisper_lock->base.states.current == check_state::unchecked_up &&
            pane->muzzle->base.states.current == check_state::unchecked_up,
        "the boxes let him go again"
    );

    click_window(widget, player_row(view, 0)->name_label);
    check(
        g_server.profile_requests.size() == 2 && g_server.profile_requests[1].user_id == 9 &&
            window_is_shown(w) && shows(pane->html, "ACCESSING...") &&
            window_text(widget, pane->name).empty() && !window_is_shown(pane->insignia) &&
            view->player_list->selected_item == &player_row(view, 0)->row.base,
        "a click on another row blanks the shown pane and asks for his profile"
    );
    g_sounds.clear();
    click_window(widget, pane->pane.close_button);
    check(
        heard("INFO_PANE_CLOSE") && !window_is_shown(w) && !chat.pane.visible &&
            chat.pane.user_id == 0 && chat.pane.record == nullptr &&
            view->player_list->selected_item == nullptr &&
            bounds_are(&log->base, view->base.x + 0x7c, view->base.y + 2, 0x176, 0x122),
        "the Close_Button closes the pane and drops the player selection (player_pane_close, "
        "chat_handle_pane_message)"
    );
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, &view->input->base);
    }
}

// The shown description of the Profile console: its visible lines joined,
// or "!" when a hidden line holds text or a visible one follows a hidden one.
std::string profile_description(StatusWidget* widget, ProfileView* view) {
    std::string text;
    bool ended = false;
    for (Window* line : view->description) {
        const std::string shown = window_text(widget, line);
        if (line->visible == 0) {
            if (!shown.empty())
                return "!";
            ended = true;
        } else if (ended) {
            return "!";
        } else {
            text += shown;
        }
    }
    return text;
}

int32_t visible_description_lines(const ProfileView* view) {
    int32_t lines = 0;
    for (const Window* line : view->description)
        lines += line->visible;
    return lines;
}

screens::ByCircleRow* profile_row(ProfileView* view, int32_t index) {
    return reinterpret_cast<screens::ByCircleRow*>(list_box_item(&view->list->list, index));
}

// Runs the Main Frame until the green layer has slid in with console
// `screen`.
void open_console(StatusWidget* widget, MainFrame* frame, ConsoleButton button) {
    click_window(widget, console_button(frame, button));
    for (int32_t i = 0; frame->console->sliding != 0 && i < 100; ++i)
        boneyards_status_widget_tick(widget);
}

// The Profile console as profile_build builds it in the green layer's
// rectangle: the keyed image with its rule down x 0x7B, PROFILE in the big
// font centred over the 0x7C-wide list, the ProfileList at (0x17, 0x93)
// with a BYCircleLstItem per Profile.def category in the order Login,
// Boneyards, Personal, then ids 3 to 6 (by_circle_row_create), five description lines
// in the small font at (6, 0x9B + 0x11 i) and 0x72 wide, APPLY and RESET at
// (0xC, 0x105) and (0xC, 0x11D), both off, and the status line at (0xA0,
// 0x5A). The build selects the account tab (show_tab_description), whose description
// wraps into the lines, and asks Rover for the player's own record with
// "RECEIVING DATA..." up and the list off (profile_begin_receive, slot 63). A failed
// reply puts up "ACCESS FAILED. Try again later." (profile_on_received). Closing
// and opening again shows that same console, still on the failed reply, and
// does not ask again; answering the request already out takes the status line
// down and turns the list on. A click on a category row sounds GREEN_BUTTON and switches to
// its tab and description (profile_on_notify); the pulse (profile_pulse) turns APPLY
// and RESET on while a tab is dirty and off again once it is not.
void check_profile_console(StatusWidget* widget, MainFrame* frame, const Data& data) {
    screens::BoneyardsScreens* nav = widget->navigation;
    auto* view = reinterpret_cast<ProfileView*>(frame->active_screen);
    if (view == nullptr || view->list == nullptr)
        return;
    const Window& c = view->base;
    const screens::HatFonts& f = widget->system.hat_fonts;
    const Handle image = widget->system.profile_image;
    check(
        window_is_named(&c, "ProfileConsole") && bounds_are(&c, 0x17, 0x7e, 0x1f2, 0x139) &&
            c.states.images[1] == image && image_pixel(widget, image, 0x7b, 3) == 0x5d &&
            image_pixel(widget, image, 0x7b, 0x135) == 0x5d &&
            image_pixel(widget, image, 0x7b, 2) == screens::kWidgetKey &&
            image_pixel(widget, image, 0x7c, 0x80) == screens::kWidgetKey,
        "console_open_main_frame/profile_build: the console shows the keyed image with the rule at "
        "x 0x7B"
    );
    const Window* title = find_label(widget, &view->base, "PROFILE");
    check(
        title != nullptr && title->x == c.x && title->y == c.y + 6 && title->width == 0x7c &&
            title->font == f.big,
        "profile_build: PROFILE in the big font over the list"
    );
    ListBox* list = &view->list->list;
    check(
        window_is_named(&list->base, "ProfileList") &&
            bounds_are(&list->base, 0x17, 0x93, 0x7c, 0xed) && list->count == 7,
        "profile_build: the ProfileList at (0x17, 0x93) lists the seven categories"
    );
    const int32_t row_tabs[] = {0, 2, 1, 3, 4, 5, 6};
    bool rows = list->count == 7;
    for (int32_t i = 0; rows && i < 7; ++i) {
        screens::ByCircleRow* row = profile_row(view, i);
        const Window& r = row->row.base;
        rows = row->row.base.ops->is_class(screens::kByCircleRowClass) && row->tab == row_tabs[i] &&
               bounds_are(&r, 0x17, 0x93 + 0x11 * i, r.width, 0x11) &&
               window_text(widget, row->name_label) == g_category_names[row_tabs[i]] &&
               window_is_named(row->icon, "ProfileCategoryIcon") &&
               bounds_are(row->icon, r.x + 3, r.y + 3, 0xb, 0xb);
    }
    check(rows, "by_circle_row_create: a circle row per category, tabs 0, 2, 1, 3, 4, 5, 6");
    bool lines = true;
    for (int32_t i = 0; i < k_profile_description_lines; ++i)
        lines = lines &&
                bounds_are(view->description[i], c.x + 6, c.y + 0x9b + 0x11 * i, 0x72, 0x11) &&
                view->description[i]->font == f.small;
    check(lines, "profile_build: five 0x72-wide description lines from (6, 0x9B)");
    check(
        view->apply->x == c.x + 0xc && view->apply->y == c.y + 0x105 &&
            window_text(widget, view->apply) == "APPLY" &&
            view->apply->states.current == button_state::disabled && view->reset->x == c.x + 0xc &&
            view->reset->y == c.y + 0x11d && window_text(widget, view->reset) == "RESET" &&
            view->reset->states.current == button_state::disabled,
        "profile_build: APPLY and RESET under the list, both off"
    );
    const screens::ByListBox* by_list = view->list;
    check(
        by_list->selected_item == &profile_row(view, 0)->row.base &&
            profile_description(widget, view) == g_category_descriptions[0] &&
            visible_description_lines(view) >= 1 &&
            window_text(widget, view->description[0]).size() <
                std::strlen(g_category_descriptions[0]),
        "show_tab_description: the account row selected and its description wrapped under the rows"
    );
    check(
        view->status->x == c.x + 0xa0 && view->status->y == c.y + 0x5a &&
            view->status->font == f.big && window_is_shown(view->status) &&
            window_text(widget, view->status) == "RECEIVING DATA..." &&
            list->base.states.current == screens::widget_value::disabled,
        "profile_begin_receive: RECEIVING DATA... up and the list off while the record is out"
    );
    const size_t asked = g_server.profile_requests.size();
    check(
        asked >= 1 && g_server.profile_requests.back().user_id == nav->session.self_user_id &&
            g_server.profile_requests.back().context == &nav->profile &&
            frame->activity_light->running != 0,
        "profile_begin_receive asks Rover for the player's own profile with the light running"
    );
    if (asked == 0)
        return;
    {
        ScopedServices scope(widget);
        const by_test::ProfileRequestSeen request = g_server.profile_requests.back();
        request.on_profile(request.context, false, nullptr);
    }
    boneyards_status_widget_tick(widget);
    check(
        window_is_shown(view->status) &&
            window_text(widget, view->status) == "ACCESS FAILED. Try again later." &&
            list->base.states.current == screens::widget_value::disabled &&
            frame->activity_light->running == 0,
        "profile_on_received: a failed reply puts up ACCESS FAILED and stops the light"
    );
    draw_frame(widget, data, "by_profile_failed.ppm");

    ProfileView* kept = view;
    click_window(widget, console_button(frame, ConsoleButton::release));
    finish_slide(widget);
    open_console(widget, frame, ConsoleButton::profile);
    view = reinterpret_cast<ProfileView*>(frame->active_screen);
    check(
        view == kept && frame->console_screen == console_screen::profile &&
            g_server.profile_requests.size() == asked,
        "the reopened Profile console is the one that closed and does not ask again"
    );
    if (view != kept || g_server.profile_requests.size() != asked)
        return;
    check(
        window_is_shown(view->status) &&
            window_text(widget, view->status) == "ACCESS FAILED. Try again later.",
        "the failed reply stays on the console"
    );
    list = &view->list->list;
    by_test::ProfileReply own{
        {"user_id", std::to_string(nav->session.self_user_id)},
        {"LOGINNAME", "oa_check"},
        {"ICON", "1"},
        {"ALLEGIANCE", "Core"},
    };
    {
        ScopedServices scope(widget);
        const by_test::ProfileRequestSeen request = g_server.profile_requests.back();
        const screens::ProfileFields fields = own.fields();
        request.on_profile(request.context, true, &fields);
    }
    boneyards_status_widget_tick(widget);
    check(
        !window_is_shown(view->status) &&
            list->base.states.current == screens::widget_value::selected &&
            std::strcmp(nav->profile.record->name, "oa_check") == 0,
        "profile_on_received: the record's arrival takes the status line down and the list on"
    );

    g_sounds.clear();
    screens::ByCircleRow* personal = profile_row(view, 2);
    click_window(widget, personal->name_label);
    check(
        heard("GREEN_BUTTON") && nav->profile.current_tab == 1 &&
            view->list->selected_item == &personal->row.base &&
            profile_description(widget, view) == g_category_descriptions[1] &&
            visible_description_lines(view) >= 2,
        "profile_on_notify: the Personal row sounds GREEN_BUTTON and wraps its description"
    );
    draw_frame(widget, data, "by_profile.ppm");
    screens::ByCircleRow* login = profile_row(view, 0);
    click_window(widget, login->name_label);
    check(
        nav->profile.current_tab == 0 && view->list->selected_item == &login->row.base &&
            profile_description(widget, view) == g_category_descriptions[0],
        "profile_on_notify: the Login row switches back to the account tab"
    );

    ProfileTabWindows* tabs = profile_console_tabs(&view->base);
    if (tabs != nullptr && tabs->password != nullptr && tabs->confirm != nullptr) {
        Window* focus = widget->desktop.focus;
        std::string focused_text;
        SingleEdit* focused_edit = nullptr;
        if (focus != nullptr && focus->ops != nullptr &&
            focus->ops->is_class(class_name::single_edit)) {
            focused_edit = reinterpret_cast<SingleEdit*>(focus);
            focused_text = single_edit_text(focused_edit);
        }
        const std::string password_before = single_edit_text(tabs->password);
        const std::string confirm_before = single_edit_text(tabs->confirm);
        type(widget, "q");
        check(
            widget->desktop.focus != &tabs->password->base &&
                widget->desktop.focus != &tabs->confirm->base &&
                std::strcmp(single_edit_text(tabs->password), password_before.c_str()) == 0 &&
                std::strcmp(single_edit_text(tabs->confirm), confirm_before.c_str()) == 0,
            "the account tab does not take typing when it opens"
        );
        if (focused_edit != nullptr)
            single_edit_set_text(focused_edit, focused_text.c_str());
    }
    single_edit_set_text(tabs->confirm, "x");
    boneyards_status_widget_tick(widget);
    check(
        view->apply->states.current == button_state::unpressed &&
            view->reset->states.current == button_state::unpressed,
        "profile_pulse: a dirty tab turns APPLY and RESET on"
    );
    single_edit_set_text(tabs->confirm, "");
    boneyards_status_widget_tick(widget);
    check(
        view->apply->states.current == button_state::disabled &&
            view->reset->states.current == button_state::disabled,
        "profile_pulse: with no tab dirty APPLY and RESET go off"
    );
}

struct SettingLog {
    struct Item {
        char key[32];
        int32_t value;
    };

    Item items[4]{};
    int32_t count = 0;
};

bool save_logged_number(void* user, const char* key, int32_t value) {
    auto* log = static_cast<SettingLog*>(user);
    if (log == nullptr || key == nullptr || log->count >= 4)
        return false;
    std::snprintf(log->items[log->count].key, sizeof log->items[0].key, "%s", key);
    log->items[log->count].value = value;
    ++log->count;
    return true;
}

int32_t& applied_unit_limit() {
    static int32_t units = -1;
    return units;
}

void note_unit_limit(int32_t units) {
    applied_unit_limit() = units;
}

// Restores the game's unit-limit hook, which a console opening does not clear.
struct RestoreUnitLimit {
    RestoreUnitLimit() {
        applied_unit_limit() = -1;
        screens::profile_on_unit_limit(note_unit_limit);
    }

    ~RestoreUnitLimit() { screens::profile_on_unit_limit(nullptr); }

    RestoreUnitLimit(const RestoreUnitLimit&) = delete;
    RestoreUnitLimit& operator=(const RestoreUnitLimit&) = delete;
};

// APPLY on the Profile console's Local Settings tab stores the unit limit
// the edit shows and hands that same number to the game. The press is the
// console's own APPLY, not a direct call.
void test_profile_apply_through_screen(Data* data) {
    RestoreUnitLimit restore;
    SettingLog log{};
    const screens::SettingsStore settings{&log, save_logged_number, nullptr};
    g_server = FakeServer{};
    StatusWidget* widget = make_widget(data, fake_pending_login, &settings);
    check(widget != nullptr, "widget for Profile APPLY");
    if (widget == nullptr)
        return;
    MainFrame* frame = log_in(widget);
    check(frame != nullptr, "logged in before Profile APPLY");
    if (frame == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    open_console(widget, frame, ConsoleButton::profile);
    auto* view = reinterpret_cast<ProfileView*>(frame->active_screen);
    screens::BoneyardsScreens* nav = widget->navigation;
    const bool open = view != nullptr && view->apply != nullptr && view->list != nullptr &&
                      nav != nullptr && !g_server.profile_requests.empty();
    check(open, "the Profile console is open and has asked for the record");
    if (!open) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    by_test::ProfileReply own{
        {"user_id", std::to_string(nav->session.self_user_id)},
        {"LOGINNAME", "oa_check"},
        {"ICON", "1"},
        {"ALLEGIANCE", "Core"},
    };
    {
        ScopedServices scope(widget);
        const by_test::ProfileRequestSeen request = g_server.profile_requests.back();
        const screens::ProfileFields fields = own.fields();
        request.on_profile(request.context, true, &fields);
    }
    boneyards_status_widget_tick(widget);
    screens::ByCircleRow* local = profile_row(view, 6);
    check(
        local != nullptr && local->name_label != nullptr &&
            local->tab == static_cast<int32_t>(screens::ProfileTab::local_settings),
        "Local Settings is the last category"
    );
    if (local == nullptr || local->name_label == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    click_window(widget, local->name_label);
    ProfileTabWindows* tabs = profile_console_tabs(&view->base);
    check(
        nav->profile.current_tab == static_cast<int32_t>(screens::ProfileTab::local_settings) &&
            tabs != nullptr && tabs->unit_limit != nullptr,
        "the Local Settings tab is showing"
    );
    if (tabs == nullptr || tabs->unit_limit == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    single_edit_set_text(tabs->unit_limit, "333");
    boneyards_status_widget_tick(widget);
    check(
        view->apply->states.current == button_state::unpressed,
        "a changed unit limit turns APPLY on"
    );
    // A login already stores BetaLoginUX through the same settings, so the
    // unit limit is the write APPLY adds, not the only one in the log.
    const int32_t writes_before = log.count;
    click_window(widget, view->apply);
    check(
        nav->profile.unit_limit == 333 && nav->profile.game_unit_limit == 333,
        "APPLY stores the unit limit the screen shows"
    );
    check(applied_unit_limit() == 333, "APPLY hands the unit limit to the game");
    bool wrote_limit = false;
    for (int32_t i = writes_before; i < log.count; ++i) {
        if (std::strcmp(log.items[i].key, screens::kUnitLimitKey) == 0 && log.items[i].value == 333)
            wrote_limit = true;
    }
    check(wrote_limit, "APPLY writes the unit limit");
    boneyards_status_widget_destroy(widget);
}

// Release on the login console leaves the Boneyards. A login reaches the
// galactic map; a selected planet enables Chat, whose console is built as the
// green slides in and opens when it covers the map, then sends typed lines
// and prints the chat; opening Profile closes it, and Release on Profile
// closes to the map.
void test_connected_navigation(Data* data) {
    g_server = FakeServer{};
    StatusWidget* widget = make_widget(data);
    check(widget != nullptr && widget->toolkit != nullptr, "widget with text services");
    if (widget == nullptr)
        return;
    MainFrame* frame = enter_boneyards(widget);
    check(frame != nullptr && boneyards_status_widget_in_boneyards(widget), "Main Frame shown");
    if (frame == nullptr)
        return;
    finish_update_console(widget);
    check(frame->console_screen == console_screen::login, "login console open");
    check(
        console_button(frame, ConsoleButton::release)->states.current == button_state::disabled,
        "Release stays off as the update and login consoles open"
    );
    // A login console opened over enabled buttons leaves Release on.
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    click_window(widget, console_button(frame, ConsoleButton::release));
    check(
        boneyards_status_widget_visible(widget) && !boneyards_status_widget_in_boneyards(widget),
        "release on the login console returns to the main menu"
    );
    check(widget->system.connection_level == 0, "the client is closed");
    boneyards_status_widget_tick(widget);

    frame = log_in(widget);
    check(
        frame != nullptr && frame->console_screen == console_screen::none, "logged in to the map"
    );
    if (frame == nullptr)
        return;
    screens::BoneyardsScreens* nav = widget->navigation;
    check(
        !nav->galaxy.locked && planet_buttons_are(frame, button_state::disabled),
        "the unlocked map waits for a planet"
    );
    click_window(widget, hotspot(frame, 0));
    check(planet_buttons_are(frame, button_state::unpressed), "a selected planet enables chat");
    check_planet_summary(widget, frame, *data);

    {
        ScopedServices scope(widget);
        activity_light_stop(frame->activity_light);
    }
    click_window(widget, console_button(frame, ConsoleButton::chat));
    check_chat_console_built(widget, frame);
    finish_slide(widget);
    check(frame->console_screen == console_screen::chat, "chat console opened");
    check(
        nav->current == screens::ByScreen::chat_frequencies && nav->chat.visible,
        "model on Chat Frequencies"
    );
    check_chat_console_opened(widget, frame);
    Window* chat_view = frame->active_screen;
    check(chat_view != nullptr, "chat view built");
    Window* list =
        chat_view != nullptr ? window_find_named(chat_view, "ChatList", nullptr) : nullptr;
    check(list != nullptr && list->width == 0x176, "chat log list");
    if (list != nullptr)
        check_chat_list_scroll_bar(widget, list);
    Window* input = widget->desktop.focus;
    check(
        input != nullptr && input->ops->is_class(class_name::single_edit),
        "chat input is a focused SingleEdit"
    );
    type(widget, "hello there");
    auto* edit = reinterpret_cast<SingleEdit*>(input);
    check(std::strcmp(single_edit_text(edit), "hello there") == 0, "typing into chat");
    press(widget, key::enter);
    const uint32_t area_id = nav->session.area_id;
    check(
        g_server.chats == 1 && g_server.last_chat == "hello there" && g_server.chat_room == 0 &&
            g_server.chat_area == area_id && g_server.chat_target == 0,
        "Enter sends the line to the room being entered"
    );
    check(edit->length == 0, "input cleared after sending");
    check_room_feeds(widget, reinterpret_cast<ListBox*>(list));
    type(widget, "second line");
    press(widget, key::enter);
    check(
        g_server.chats == 2 && g_server.last_chat == "second line" && g_server.chat_room == 14 &&
            g_server.chat_area == area_id && g_server.chat_target == 0,
        "chat_console_submit sends to everyone on the entered frequency (room, area, 0, text)"
    );
    auto* chat = reinterpret_cast<ChatView*>(chat_view);
    if (chat != nullptr) {
        check_chat_lists(widget, chat, *data);
        check_chat_panes(widget, frame, chat, *data);
        check_chat_player_pane(widget, frame, chat, *data);
        check_chat_lines(widget, chat);
        check_chat_pages(widget, chat);
        check_create_follows_pending(widget, chat);
    }
    check(draw_frame(widget, *data, "by_chat.ppm") > 100000, "chat frame drawn");
    if (list != nullptr && chat != nullptr) {
        // A log longer than the list brings the scroll bar up, enabled.
        {
            ScopedServices scope(widget);
            for (int32_t line = 0; line < 40; ++line) {
                const std::string text = "chat line " + std::to_string(line);
                screens::chat_print_line(
                    &nav->chat, text.c_str(), screens::kDefaultTextColor, true
                );
            }
        }
        Window* bar = window_find_named(list, "slider", class_name::slider);
        check(
            bar != nullptr && bar->visible == 1 && bar->states.current == button_state::unpressed,
            "overflowing log shows the enabled scroll bar"
        );
        check_chat_scroll_keys(widget, chat);
        press(widget, key::page_up);
        draw_frame(widget, *data, "by_chat_scrolled.ppm");
    }
    if (chat != nullptr && chat->matchmaking != nullptr)
        check_matchmaking(widget, chat);

    check_battle_console(widget, frame);

    click_window(widget, console_button(frame, ConsoleButton::profile));
    check(frame->console_screen == console_screen::profile, "profile console opened");
    check(
        nav->current == screens::ByScreen::profile_console && !nav->chat.visible,
        "the chat console closed as Profile opened"
    );
    // Battle closed the chat console, leaving the frequency; chat reopened
    // straight into the remembered one and closed again before any answer.
    const std::string area = std::to_string(nav->session.area_id);
    const std::vector<std::string>& rooms = g_server.room_calls;
    check(
        rooms.size() >= 2 && rooms[rooms.size() - 2] == "leave(" + area + ",14)" &&
            rooms.back() == "join(" + area + ",14)",
        "closing leaves the frequency and reopening enters the remembered one"
    );
    {
        ScopedServices scope(widget);
        g_server.events.room_member(g_server.events.user, 0, 10, "Eve", 1, 3, 0, "A");
    }
    check(nav->chat.player_total == 0, "no chat console, no room feeds");
    for (int32_t i = 0; frame->console->sliding != 0 && i < 100; ++i)
        boneyards_status_widget_tick(widget);
    check_profile_console(widget, frame, *data);
    auto* profile_button =
        reinterpret_cast<ConsoleNavButton*>(console_button(frame, ConsoleButton::profile));
    check(
        frame->active_screen != nullptr && frame->active_screen->visible == 1 &&
            profile_button->base.states.images[button_state::unpressed] == profile_button->lit,
        "profile_console_open lights the Profile button and shows its console"
    );
    ProfileTabWindows* tabs = profile_console_tabs(frame->active_screen);
    single_edit_set_text(tabs->confirm, "x");
    boneyards_status_widget_tick(widget);
    check(
        nav->profile.tab_dirty[0],
        "the console's pulse (profile_pulse) rechecks the account tab (profile_check_tab)"
    );
    single_edit_set_text(tabs->confirm, "");
    boneyards_status_widget_tick(widget);

    g_sounds.clear();
    click_window(widget, console_button(frame, ConsoleButton::release));
    check(
        profile_button->base.states.images[button_state::unpressed] == profile_button->normal,
        "console_screen_close puts the Profile button back to its normal face"
    );
    check(
        frame->console_screen == console_screen::none && frame->active_screen == nullptr,
        "release on Profile closes to the map"
    );
    check(
        heard("GREEN_SLIDE") && frame->console->direction == ConsoleSlide::out,
        "the green slides out"
    );
    finish_slide(widget);
    const Window* release = console_button(frame, ConsoleButton::release);
    check(
        heard("BIGBUTTON") && !nav->galaxy.locked &&
            release->states.current == button_state::disabled,
        "the map is handed back with Release off"
    );
    draw_frame(widget, *data, "by_map.ppm");

    // With the Chat console closed a page opens a message box instead.
    g_sounds.clear();
    {
        ScopedServices scope(widget);
        const AlertReport alert{static_cast<int32_t>(AlertKind::page), "<Page from Dan> hi", 9, ""};
        g_server.events.system_alert(g_server.events.user, &alert);
    }
    check(
        boneyards_status_widget_message_open(widget) &&
            message_text(widget) == "<Page from Dan> hi" &&
            g_sounds == std::vector<std::string>{"MESSAGE"},
        "a page with the chat closed opens a box"
    );
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, &message_box_find_open(&widget->desktop)->base);
    }
    press(widget, key::enter);
    check(!boneyards_status_widget_message_open(widget), "Enter closes the page's box");

    click_window(widget, frame->exit_button);
    check(boneyards_status_widget_visible(widget), "exit returns to the main menu");
    boneyards_status_widget_destroy(widget);
}

void test_offline_login(Data* data) {
    g_server = FakeServer{};
    StatusWidget* widget = make_widget(data, fake_login);
    if (widget == nullptr)
        return;
    MainFrame* frame = enter_boneyards(widget);
    check(
        frame != nullptr && frame->console_screen == console_screen::update,
        "update console first when not connected"
    );
    if (frame == nullptr)
        return;
    finish_update_console(widget);
    check(frame->console_screen == console_screen::login, "then the login screen");
    screens::BoneyardsScreens* nav = widget->navigation;
    Window* name = widget->desktop.focus;
    check(name != nullptr && name->ops->is_class(class_name::single_edit), "name field focused");
    type(widget, "commander");
    for (int32_t i = 0; i < frame->active_screen->children.count; ++i) {
        Window* child = frame->active_screen->children.items[i];
        if (child->ops->is_class(class_name::radio_button) &&
            child->y == frame->active_screen->y + 0x5c)
            click_window(widget, child);
    }
    Window* password = nullptr;
    for (int32_t i = 0; i < frame->active_screen->children.count; ++i) {
        Window* child = frame->active_screen->children.items[i];
        if (child != name && child->ops->is_class(class_name::single_edit))
            password = child;
    }
    check(
        password != nullptr && reinterpret_cast<SingleEdit*>(password)->mask == '*',
        "masked password field"
    );
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, password);
    }
    type(widget, "secret");
    boneyards_status_widget_tick(widget);
    check(
        std::strcmp(nav->login.name, "commander") == 0 &&
            std::strcmp(nav->login.password, "secret") == 0,
        "fields feed the login model"
    );
    check(draw_frame(widget, *data, "by_login.ppm") > 100000, "login frame drawn");
    press(widget, key::enter);
    check(g_server.logins == 1, "Enter in the password field logs in");
    check(std::strcmp(nav->login.status, "Server Not responding") == 0, "offline status");
    check(widget->system.connection_level == 1, "still not connected");
    check(frame->activity_light->running == 0, "a refused login leaves the light dark");

    Window* register_button =
        window_find_named(frame->active_screen, "Function_Button_Large", nullptr);
    for (int32_t i = 0; i < frame->active_screen->children.count; ++i) {
        Window* child = frame->active_screen->children.items[i];
        if (child->ops->is_class(class_name::button) && child->y == frame->active_screen->y + 0x7E)
            register_button = child;
    }
    check(register_button != nullptr, "register button");
    if (register_button != nullptr)
        click_window(widget, register_button);
    Window* registration = window_find_named(frame->active_screen, "Registration", nullptr);
    check(
        registration != nullptr && window_is_shown(registration),
        "register opens the registration console"
    );
    check(!boneyards_status_widget_message_open(widget), "register leaves the message box closed");
    Window* back =
        registration != nullptr ? window_find_named(registration, "Back", nullptr) : nullptr;
    check(back != nullptr, "registration back button");
    if (back != nullptr)
        click_window(widget, back);
    boneyards_status_widget_tick(widget);
    check(
        window_find_named(frame->active_screen, "Registration", nullptr) == nullptr,
        "back returns to the login console"
    );
    boneyards_status_widget_destroy(widget);
}

constexpr const char* k_disconnect_notice = "An unexpected network\ndisconnect has occurred.";
constexpr const char* k_missing_map_text =
    "Unable to load:\nBoneyards\\_map.gwm\n\nPlanetary functions unavailable at this time";

// A login whose progress arrives as service events while the widget pumps. The activity light runs
// from LOGIN until the dossier; the pass that sees the dossier (login_view_pulse at connection
// level 4) loads the war map of the yard master's server (galaxy_view_load) and closes the login
// console (main_frame_close_screen): GREEN_SLIDE, the green sliding out over BYConsoleSlide pulses
// and two more, then BIGBUTTON with the buttons enabled but Release, Recon, Chat and Battle
// (main_frame_release_map, main_frame_enable_buttons) and the map unlocked once the allegiances
// arrive (main_frame_update). A hotspot press asks for its planet's census and runs the light until
// that census arrives. With no server name the load fails with the game's message box and the slide
// still runs.
void test_event_login(Data* data, const char* area) {
    g_server = FakeServer{};
    g_server.area = area;
    StatusWidget* widget = make_widget(data);
    if (widget == nullptr)
        return;
    check(g_server.events.session_changed != nullptr, "the widget listens to the service");
    MainFrame* frame = enter_boneyards(widget);
    if (frame == nullptr)
        return;
    finish_update_console(widget);
    check(g_server.pumps > 16, "every cycle pumps the service");
    screens::BoneyardsScreens* nav = widget->navigation;
    submit_login(widget, frame);
    check(
        g_server.logins == 1 && nav->login.state == screens::LoginState::connecting,
        "the login is pending"
    );
    check(std::strcmp(nav->login.status, "Contacting Central Command") == 0, "contacting status");
    check(frame->activity_light->running == 1, "LOGIN starts the activity light");

    g_server.on_pump = [] {
        g_server.events.login_status(g_server.events.user, "Contact Established");
    };
    boneyards_status_widget_tick(widget);
    check(
        std::strcmp(nav->login.status, "Contact Established") == 0,
        "progress reaches the login dialog"
    );
    check(frame->activity_light->running == 1, "the light runs while the login is pending");

    g_sounds.clear();
    g_server.calls.clear();
    g_server.on_pump = deliver_dossier;
    boneyards_status_widget_tick(widget);
    const ConsoleLayer* layer = frame->console;
    check(
        widget->system.connection_level == 4 && widget->system.matchmaking_enabled == 1,
        "the session level and matchmaking_enabled are mirrored"
    );
    check(nav->session.self_user_id == 42 && nav->session.area_id == 7, "identity for the chat");
    check(
        nav->login.state == screens::LoginState::planetary &&
            nav->current == screens::ByScreen::none,
        "the dossier completes the login and opens no screen"
    );
    check(frame->activity_light->running == 0, "the dossier stops the activity light");
    check(
        frame->console_screen == console_screen::none && frame->active_screen == nullptr,
        "the first pulse at level 4 closes the login console"
    );
    check(
        heard("GREEN_SLIDE") && layer->direction == ConsoleSlide::out && layer->sliding == 1 &&
            layer->countdown == layer->slide_steps,
        "the green starts sliding out"
    );
    const bool loaded = area != nullptr;
    const std::string expected_path = loaded ? k_alpha_map : "Boneyards\\_map.gwm";
    check(g_server.map_path == expected_path, "war map of the yard master's server");
    if (loaded) {
        check(
            g_server.calls ==
                std::vector<std::string>{"allegiance", "census(0)", "open", "read", "close"},
            "feeds requested, then the war map read"
        );
        check(
            nav->galaxy.planet_count == k_planet_count && nav->galaxy.loading,
            "the planets wait for their allegiances"
        );
        check(frame->map_panel->hotspots[k_planet_count - 1] != nullptr, "planet hotspots built");
        check(!boneyards_status_widget_message_open(widget), "no message box");
    } else {
        check(
            g_server.calls == std::vector<std::string>{"allegiance", "census(0)", "open"},
            "the missing war map is not read"
        );
        check(message_text(widget) == k_missing_map_text, "the map that failed to load is named");
    }

    for (int32_t i = 0; i < layer->slide_steps + 1; ++i) {
        boneyards_status_widget_tick(widget);
        check(frame->console_screen != console_screen::chat, "no chat console during the slide");
    }
    check(
        layer->sliding == 1 && !heard("BIGBUTTON"),
        "the slide takes BYConsoleSlide pulses and two more"
    );
    boneyards_status_widget_tick(widget);
    check(
        heard("BIGBUTTON") && layer->sliding == 0 && layer->covering == 0 &&
            layer->base.visible == 0,
        "BIGBUTTON as the green leaves the map"
    );
    check(
        console_button(frame, ConsoleButton::release)->states.current == button_state::disabled,
        "Release disabled on the map"
    );
    check(planet_buttons_are(frame, button_state::disabled), "Recon, Chat and Battle wait");
    const Window* news = console_button(frame, ConsoleButton::news);
    const Window* locator = console_button(frame, ConsoleButton::locator);
    check(
        news->states.current == button_state::unpressed &&
            locator->states.current == button_state::unpressed,
        "News enabled, and Locator by matchmaking_enabled"
    );
    check(frame->unlock_pending == 1 && nav->galaxy.locked, "the unlock waits for the galaxy data");
    draw_frame(widget, *data, loaded ? "by_post_login.ppm" : "by_post_login_missing_map.ppm");
    if (!loaded) {
        boneyards_status_widget_destroy(widget);
        return;
    }

    g_server.on_pump = deliver_allegiances;
    boneyards_status_widget_tick(widget);
    check(
        !nav->galaxy.loading && !nav->galaxy.locked && frame->unlock_pending == 0,
        "the allegiances unlock the map"
    );
    draw_frame(widget, *data, "by_galactic_map.ppm");
    MainFrameProbe probe{};
    check(
        boneyards_status_widget_probe(widget, &probe) && probe.planets_drawn == k_planet_count,
        "galaxy_draw the map viewer shows every planet's sprite once the allegiances arrive"
    );
    check(planet_buttons_are(frame, button_state::disabled), "no planet selected yet");
    g_server.calls.clear();
    click_window(widget, hotspot(frame, 1));
    check(
        nav->galaxy.selected == 1 && nav->session.area_id == k_first_planet + 1,
        "the hotspot selects its planet"
    );
    check(planet_buttons_are(frame, button_state::unpressed), "a selected planet enables them");
    check(
        g_server.calls == std::vector<std::string>{"census(12)"}, "the press asks for its census"
    );
    check(frame->activity_light->running == 1, "the press runs the light");
    g_server.on_pump = [] {
        const CensusReport elsewhere{99, 1, 0, 0, 0, 0, 0};
        g_server.events.census(g_server.events.user, &elsewhere);
    };
    boneyards_status_widget_tick(widget);
    check(frame->activity_light->running == 1, "an unknown planet's census leaves it running");
    g_server.on_pump = [] {
        const CensusReport census{12, 17, 3, 2, 1, 6, 5};
        g_server.events.census(g_server.events.user, &census);
    };
    boneyards_status_widget_tick(widget);
    check(
        frame->activity_light->running == 0 && nav->galaxy.planets[1].population == 17,
        "the planet's census stops the light"
    );

    g_server.on_pump = [] {
        g_server.events.notice(g_server.events.user, k_disconnect_notice);
        SessionRecord session{};
        session.level = SessionLevel::disconnected;
        g_server.events.session_changed(g_server.events.user, &session);
    };
    const int32_t messages = nav->message_count;
    boneyards_status_widget_tick(widget);
    check(
        frame->pending_notice == k_pending_notice_shown && frame->return_requested == false,
        "the dropped link leaves its notice pending"
    );
    check(
        boneyards_status_widget_message_open(widget) &&
            message_text(widget) == k_disconnect_notice && nav->message_count == messages,
        "the pending notice shows"
    );
    check(
        widget->system.main_frame == frame && widget->system.connection_level == 2 &&
            widget->system.matchmaking_enabled == 0,
        "the notice holds the disconnected Main Frame"
    );
    press(widget, key::enter);
    check(
        frame->return_requested && !boneyards_status_widget_message_open(widget) &&
            widget->system.main_frame == frame,
        "Ok asks the next cycle to leave"
    );
    boneyards_status_widget_tick(widget);
    check(
        widget->system.main_frame == nullptr && widget->system.connection_level == 0 &&
            widget->system.session_started == 0 &&
            boneyards_status_widget_part(widget, OverlayPart::enter) != nullptr &&
            boneyards_status_widget_part(widget, OverlayPart::enter)->visible != 0,
        "the player lands in the main menu once the notice is dismissed"
    );
    boneyards_status_widget_destroy(widget);
    check(g_server.events.session_changed == nullptr, "the widget stops listening when destroyed");
}

void test_overlay_message(Data* data) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    StatusWidget* widget = boneyards_status_widget_create(config);
    if (widget == nullptr)
        return;
    press(widget, 'b');
    check(boneyards_status_widget_message_open(widget), "offline enter opens a message box");
    MessageBox* box = message_box_find_open(&widget->desktop);
    const Window* text =
        box != nullptr ? window_find_named(&box->base, message_box_name::text, nullptr) : nullptr;
    check(text != nullptr, "message text");
    check(
        boneyards_status_widget_part(widget, OverlayPart::enter)->states.current ==
            button_state::disabled,
        "overlay buttons disabled while the box is open"
    );
    draw_frame(widget, *data, "by_overlay_message.ppm");
    Window* ok = box != nullptr
                     ? window_find_named(&box->base, message_box_name::ok, class_name::button)
                     : nullptr;
    check(ok != nullptr, "Ok button");
    if (ok != nullptr)
        click_window(widget, ok);
    check(!boneyards_status_widget_message_open(widget), "Ok closes the box");
    check(
        boneyards_status_widget_part(widget, OverlayPart::enter)->states.current ==
            button_state::unpressed,
        "overlay restored"
    );
    check(
        widget->desktop.focus == boneyards_status_widget_part(widget, OverlayPart::panel),
        "focus back on the overlay"
    );
    boneyards_status_widget_tick(widget);
    check(widget->desktop.windows.count == 1, "box destroyed on the next cycle");
    boneyards_status_widget_destroy(widget);
}

// Chat keeps its entry line and the log's scroll position across a close and
// the opening that follows. The console shown the second time is the first one.
// Selecting a planet, then a session record of another area or of 0, then
// CHAT, asks for the selected planet. The debriefing open enters the
// debriefing room; CHAT again asks for the selected planet. An open with
// no planet and area 0 shows no alert.
void test_chat_on_selected_planet(Data* data) {
    g_server = FakeServer{};
    StatusWidget* widget = make_widget(data);
    if (widget == nullptr)
        return;
    MainFrame* frame = log_in(widget);
    if (frame == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    screens::BoneyardsScreens* nav = widget->navigation;
    click_window(widget, hotspot(frame, 1));
    const uint32_t planet = k_first_planet + 1;
    check(nav->galaxy.selected == 1 && nav->session.area_id == planet, "planet 1 is selected");

    SessionRecord record{};
    record.level = SessionLevel::dossier_loaded;
    record.user_id = 42;
    record.area_id = 0;
    g_server.events.session_changed(g_server.events.user, &record);
    check(nav->session.area_id == planet, "a session record of area 0 leaves the selected planet");
    record.area_id = 7;
    g_server.events.session_changed(g_server.events.user, &record);
    check(
        nav->session.area_id == planet && nav->session.home_area_id == 7,
        "a session record of the home area leaves the selected planet"
    );

    {
        ScopedServices scope(widget);
        activity_light_stop(frame->activity_light);
    }
    g_server.room_calls.clear();
    click_window(widget, console_button(frame, ConsoleButton::chat));
    finish_slide(widget);
    check(
        !g_server.room_calls.empty() &&
            g_server.room_calls.back() == "rooms(" + std::to_string(planet) + ")",
        "CHAT asks for the selected planet after the session record"
    );

    click_window(widget, console_button(frame, ConsoleButton::chat));
    finish_slide(widget);
    check(frame->console_screen == console_screen::none, "CHAT closes before the debriefing");
    nav->session.debriefing_pending = true;
    nav->session.debrief_area_id = k_first_planet;
    nav->session.debrief_room_id = 40;
    g_server.room_calls.clear();
    {
        ScopedServices scope(widget);
        main_frame_reopen_chat(frame);
    }
    check(
        !g_server.room_calls.empty() &&
            g_server.room_calls.back() == "join(" + std::to_string(k_first_planet) + ",40)",
        "the debriefing open enters the debriefing room"
    );
    nav->session.debriefing_pending = false;
    check(
        nav->session.area_id == static_cast<uint32_t>(k_first_planet),
        "the debriefing leaves its area"
    );

    click_window(widget, console_button(frame, ConsoleButton::chat));
    finish_slide(widget);
    check(frame->console_screen == console_screen::none, "CHAT closes");
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    g_server.room_calls.clear();
    click_window(widget, console_button(frame, ConsoleButton::chat));
    finish_slide(widget);
    check(
        frame->console_screen == console_screen::chat && !g_server.room_calls.empty() &&
            g_server.room_calls.back() == "rooms(" + std::to_string(planet) + ")",
        "CHAT again asks for the selected planet"
    );

    nav->galaxy.selected = screens::kNoPlanet;
    nav->session.area_id = 0;
    nav->session.home_area_id = 0;
    nav->session.debrief_area_id = 0;
    const int32_t messages = nav->message_count;
    g_server.room_calls.clear();
    {
        ScopedServices scope(widget);
        screens::chat_open_on_planet(&nav->chat);
    }
    check(
        g_server.room_calls.empty() && !nav->chat.enter_default_room &&
            nav->message_count == messages,
        "an area-0 open asks for no list and shows no alert"
    );
    boneyards_status_widget_destroy(widget);
}

void test_chat_console_kept(Data* data) {
    g_server = FakeServer{};
    StatusWidget* widget = make_widget(data);
    check(widget != nullptr, "widget for the kept chat console");
    if (widget == nullptr)
        return;
    MainFrame* frame = log_in(widget);
    check(frame != nullptr, "logged in before Chat");
    if (frame == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    click_window(widget, hotspot(frame, 0));
    click_window(widget, console_button(frame, ConsoleButton::chat));
    finish_slide(widget);
    check(
        frame->console_screen == console_screen::chat && frame->active_screen != nullptr,
        "chat opens"
    );
    auto* chat = reinterpret_cast<ChatView*>(frame->active_screen);
    check(chat != nullptr && chat->input != nullptr && chat->log != nullptr, "chat entry and log");
    if (chat == nullptr || chat->input == nullptr || chat->log == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    {
        ScopedServices scope(widget);
        single_edit_set_text(chat->input, "hold this");
        for (int32_t line = 0; line < 30; ++line) {
            const std::string text = "kept line " + std::to_string(line);
            screens::chat_print_line(
                &widget->navigation->chat, text.c_str(), screens::kDefaultTextColor, true
            );
        }
        list_box_scroll(&chat->log->list, -3);
    }
    const int32_t top = chat->log->list.top;
    check(chat->log->list.count > chat->log->list.page && top > 0, "the log scrolls off its end");
    check(std::strcmp(single_edit_text(chat->input), "hold this") == 0, "entry line set");
    click_window(widget, console_button(frame, ConsoleButton::chat));
    check(
        frame->console_screen == console_screen::none && frame->active_screen == nullptr,
        "closing chat leaves no active screen"
    );
    check(chat->base.visible == 0, "the chat console hides");
    finish_slide(widget);
    boneyards_status_widget_tick(widget);
    check(console_open_chat() == chat, "the closed chat console is kept");
    check(
        std::strcmp(single_edit_text(chat->input), "hold this") == 0,
        "the entry line survives the close"
    );
    check(chat->log->list.top == top, "the log's scroll position survives the close");
    click_window(widget, console_button(frame, ConsoleButton::chat));
    finish_slide(widget);
    check(
        frame->active_screen == &chat->base && frame->console_screen == console_screen::chat &&
            chat->base.visible == 1,
        "reopening chat shows the same console"
    );
    check(std::strcmp(single_edit_text(chat->input), "hold this") == 0, "the entry line is intact");
    check(chat->log->list.top == top, "the log's scroll position is intact");
    boneyards_status_widget_destroy(widget);
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the Boneyards navigation tests");
    const std::filesystem::path coreprime = oa::boneyards_test::coreprime_archive(argc, argv);
    if (coreprime.empty()) {
        std::fprintf(stderr, "the mod folder is required\n");
        return 1;
    }
    Data data;
    data.store.mount(root / "by.ccx");
    data.store.mount(root / "totala1.hpi");
    data.store.mount(coreprime);
    oa::boneyards::rover_session_set_virtual_files(read_asset, &data);
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};

    test_connected_navigation(&data);
    test_profile_apply_through_screen(&data);
    test_chat_console_kept(&data);
    test_chat_on_selected_planet(&data);
    test_offline_login(&data);
    test_event_login(&data, "alpha");
    test_event_login(&data, nullptr);
    test_overlay_message(&data);
    oa::boneyards::rover_session_set_virtual_files(nullptr, nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("boneyards navigation tests passed\n");
    return 0;
}
