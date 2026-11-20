// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle console on a two-planet war map with the BY1117a install: its
// build, the mode and battle rows the battle feed fills, the planet
// intelligence log, the Join summary and Scoreboard panes a battle row opens
// with the battle's intelligence and report, JOIN, the Host pane with its
// game types, radio groups and START, and the matchmaking panel's faces.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "battle_console.hpp"
#include "console_layer.hpp"
#include "console_state.hpp"
#include "galaxy_fixture.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

Window* button(const MainFrame* frame, ConsoleButton which);

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

struct Log {
    std::vector<std::string> sounds;
    std::vector<std::string> calls;
    HostGameRequest hosted{};
    std::string hosted_name;
    std::string hosted_password;
    ServiceEvents events{};
};

Log g_log;
StatusWidget* g_widget = nullptr;

void record_sound(void*, const char* name) {
    g_log.sounds.emplace_back(name);
}

bool connected(void*) {
    return true;
}

ActivationResult activate(void*) {
    return ActivationResult::ok;
}

void ignore_census(void*, uint32_t) {
}

void keep_events(void*, const ServiceEvents* events) {
    g_log.events = events != nullptr ? *events : ServiceEvents{};
}

ServiceResult record_enter(void*, uint32_t area) {
    g_log.calls.push_back("enter " + std::to_string(area));
    return ServiceResult::pending;
}

ServiceResult record_leave(void*, uint32_t area) {
    g_log.calls.push_back("leave " + std::to_string(area));
    return ServiceResult::pending;
}

ServiceResult record_get_battle(void*, uint32_t area, const char* game_id) {
    g_log.calls.push_back("get " + std::to_string(area) + " " + game_id);
    return ServiceResult::pending;
}

ServiceResult record_join(void*, uint32_t area, const char* game_id, const char* password) {
    g_log.calls.push_back(
        "join " + std::to_string(area) + " " + game_id + " " +
        (password != nullptr ? password : "(none)")
    );
    return ServiceResult::pending;
}

ServiceResult record_host(void*, const HostGameRequest* request) {
    g_log.hosted = *request;
    g_log.hosted_name = request->name != nullptr ? request->name : "";
    g_log.hosted_password = request->password != nullptr ? request->password : "(none)";
    g_log.calls.emplace_back("host");
    return ServiceResult::pending;
}

bool heard(const char* name) {
    for (const std::string& sound : g_log.sounds)
        if (sound == name)
            return true;
    return false;
}

int32_t heard_count(const char* name) {
    int32_t count = 0;
    for (const std::string& sound : g_log.sounds)
        count += sound == name ? 1 : 0;
    return count;
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void click_window(const Window* w) {
    const int32_t x = w->x + w->width / 2;
    const int32_t y = w->y + w->height / 2;
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_up, x, y, 0));
}

void press(int32_t code) {
    Message msg{};
    msg.code = MessageCode::key_down;
    msg.key = code;
    boneyards_status_widget_event(g_widget, msg);
    msg.code = MessageCode::key_up;
    boneyards_status_widget_event(g_widget, msg);
}

void tick(int32_t cycles) {
    for (int32_t i = 0; i < cycles; ++i)
        boneyards_status_widget_tick(g_widget);
}

void finish_slide(const MainFrame* frame) {
    for (int32_t i = 0; frame->console->sliding != 0 && i < 100; ++i)
        tick(1);
}

std::vector<uint8_t> frame_pixels() {
    std::vector<uint8_t> pixels(640 * 480, 0);
    Surface target{};
    target.width = 640;
    target.height = 480;
    target.pitch = 640;
    target.pixels = pixels.data();
    target.clip = oa::Rect32{0, 0, 639, 479};
    boneyards_status_widget_draw(g_widget, &target);
    return pixels;
}

void draw_frame() {
    (void)frame_pixels();
}

// Opaque pixels of a DustCovers frame at `origin`, against a drawn Main Frame.
// `mismatches` counts pixels that are not the cover. A missing frame matches nothing.
void cover_pixels(
    const std::vector<uint8_t>& drawn,
    int32_t frame,
    int32_t origin_x,
    int32_t origin_y,
    int32_t* opaque,
    int32_t* mismatches
) {
    *opaque = 0;
    *mismatches = 0;
    ScopedServices scope(g_widget);
    const Handle cover = console_images_load(
        g_widget->images, resource::main_console_gaf, "DustCovers", frame, resource::console_key
    );
    Surface surface{};
    if (cover < 0 || !image_surface(cover, &surface) || surface.pixels == nullptr) {
        image_release(cover);
        *mismatches = 1;
        return;
    }
    for (int32_t y = 0; y < surface.height; ++y) {
        const int32_t row = origin_y + y;
        if (row < 0 || row >= 480)
            continue;
        for (int32_t x = 0; x < surface.width; ++x) {
            const uint8_t value = surface.pixels[y * surface.pitch + x];
            if (value == resource::console_key)
                continue;
            ++*opaque;
            const int32_t column = origin_x + x;
            if (column < 0 || column >= 640 || drawn[row * 640 + column] != value)
                ++*mismatches;
        }
    }
    image_release(cover);
}

// Where Main_Console.Gaf's DustCovers frames sit: frame 0 over QuickPlay and
// frame 1 over Locator.
constexpr int32_t k_quickplay_cover_x = 525;
constexpr int32_t k_quickplay_cover_y = 84;
constexpr int32_t k_locator_cover_x = 530;
constexpr int32_t k_locator_cover_y = 336;

// Neither dust cover is drawn: each cover frame has opaque pixels, and the
// drawn Main Frame differs from it.
void check_uncovered() {
    const std::vector<uint8_t> drawn = frame_pixels();
    int32_t opaque = 0;
    int32_t mismatches = 0;
    cover_pixels(drawn, 0, k_quickplay_cover_x, k_quickplay_cover_y, &opaque, &mismatches);
    CHECK(opaque > 0 && mismatches > 0);
    opaque = 0;
    mismatches = 0;
    cover_pixels(drawn, 1, k_locator_cover_x, k_locator_cover_y, &opaque, &mismatches);
    CHECK(opaque > 0 && mismatches > 0);
}

// Opens `screen` from `control` and closes it again, and the console named
// `name` is what opens.
void open_and_close(
    MainFrame* frame, Window* control, int32_t screen, const char* name, const char* sound
) {
    g_log.sounds.clear();
    click_window(control);
    CHECK(frame->console_screen == screen);
    CHECK(!boneyards_status_widget_message_open(g_widget));
    CHECK(heard(sound));
    finish_slide(frame);
    Window* opened = frame->active_screen;
    CHECK(opened != nullptr && window_is_named(opened, name) && window_is_shown(opened));
    click_window(control);
    CHECK(frame->console_screen == console_screen::none && frame->active_screen == nullptr);
    finish_slide(frame);
    CHECK(opened != nullptr && opened->visible == 0);
}

// Opens and closes QuickPlay, then Locator.
void open_both(MainFrame* frame) {
    open_and_close(
        frame,
        frame->quickplay_button,
        console_screen::quickplay,
        k_quickplay_console_name,
        "JOINGAME"
    );
    open_and_close(
        frame,
        button(frame, ConsoleButton::locator),
        console_screen::locator,
        k_locator_console_name,
        "GALACTIC_BUTTON"
    );
}

// QuickPlay and Locator are open to every account. A status 1 account leaves
// matchmaking off, and still has both buttons lit with no dust cover over
// either, and opens and closes both consoles.
void check_ordinary_routes(MainFrame* frame) {
    CHECK(!account_sees_unfinished(kAccountStatusActive));
    CHECK(g_widget->system.matchmaking_enabled == 0);
    CHECK(frame->quickplay_button->states.current == button_state::unpressed);
    CHECK(button(frame, ConsoleButton::locator)->states.current == button_state::unpressed);
    check_uncovered();
    open_both(frame);
}

// A status 5 account opens QuickPlay and Locator the same way, then the
// session bit is cleared again so the battle console builds without its
// matchmaking panel; both buttons stay lit.
void check_privileged_routes(MainFrame* frame) {
    g_widget->system.matchmaking_enabled =
        account_sees_unfinished(kAccountStatusBetaTester) ? 1 : 0;
    main_frame_enable_buttons(frame);
    CHECK(frame->quickplay_button->states.current == button_state::unpressed);
    CHECK(button(frame, ConsoleButton::locator)->states.current == button_state::unpressed);
    check_uncovered();
    open_both(frame);
    g_widget->system.matchmaking_enabled = 0;
    main_frame_enable_buttons(frame);
    CHECK(frame->quickplay_button->states.current == button_state::unpressed);
    CHECK(button(frame, ConsoleButton::locator)->states.current == button_state::unpressed);
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
bool draws_password(SingleEdit* edit, const char* typed) {
    if (edit == nullptr || typed == nullptr || edit->mask != '*' ||
        std::strcmp(single_edit_text(edit), typed) != 0)
        return false;
    ScopedServices scope(g_widget);
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

std::string text_of(Window* w) {
    ScopedServices scope(g_widget);
    const char* text = w != nullptr ? w->ops->state_text(w, -1) : nullptr;
    return text != nullptr ? text : "";
}

// The child of `parent` whose caption reads `text`, or null.
Window* captioned(Window* parent, const char* text) {
    for (int32_t i = 0; i < parent->children.count; ++i)
        if (text_of(parent->children.items[i]) == text)
            return parent->children.items[i];
    return nullptr;
}

bool bounds_are(const Window* w, int32_t x, int32_t y, int32_t width, int32_t height) {
    return w != nullptr && w->x == x && w->y == y && w->width == width && w->height == height;
}

Window* button(const MainFrame* frame, ConsoleButton which) {
    return frame->console_buttons[static_cast<int32_t>(which)];
}

// The BattleList row's title: its label, the last child.
std::string row_title(Window* row) {
    return row != nullptr && row->children.count > 0
               ? text_of(row->children.items[row->children.count - 1])
               : "";
}

std::vector<std::string> row_titles(const BattleConsole* console) {
    std::vector<std::string> titles;
    ListBox* list = &console->list->list;
    for (int32_t i = 0; i < list->count; ++i)
        titles.push_back(row_title(list_box_item(list, i)));
    return titles;
}

Window* row_titled(const BattleConsole* console, const char* title) {
    ListBox* list = &console->list->list;
    for (int32_t i = 0; i < list->count; ++i)
        if (row_title(list_box_item(list, i)) == title)
            return list_box_item(list, i);
    return nullptr;
}

// An image's pixels, row by row.
std::vector<uint8_t> image_bytes(Handle image) {
    ScopedServices scope(g_widget);
    Surface surface{};
    std::vector<uint8_t> pixels;
    if (image >= 0 && image_surface(image, &surface))
        for (int32_t y = 0; y < surface.height; ++y)
            for (int32_t x = 0; x < surface.width; ++x)
                pixels.push_back(surface.pixels[y * surface.pitch + x]);
    return pixels;
}

std::string field_text(screens::LobbyField* field) {
    return field != nullptr ? text_of(field->value) : "";
}

void battle_info(
    int32_t change, const char* game_id, const char* name, int32_t type, int32_t forming
) {
    ScopedServices scope(g_widget);
    g_log.events.battle_info(g_log.events.user, change, game_id, name, type, forming, 2);
}

void report(
    const char* game_id, uint32_t user_id, int32_t icon, const char* name, const char* message
) {
    const BattleReport line{game_id, user_id, icon, name, 9, 2, 1500, 800, 40, message};
    ScopedServices scope(g_widget);
    g_log.events.battle_report(g_log.events.user, &line);
}

// The Main Frame tests' two planets; the allegiance feed unlocks the map
// once the login console has slid away.
void load_war_map(MainFrame* frame) {
    CHECK(test::load_two_planet_galaxy(&g_widget->navigation->galaxy));
    screens::galaxy_apply_allegiances(&g_widget->navigation->galaxy, "CA");
    ScopedServices scope(g_widget);
    galaxy_panel_sync_hotspots(frame);
    main_frame_close_screen(frame);
}

StatusWidget* open_widget(Data* data) {
    g_log = Log{};
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.host.play_sound = record_sound;
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    config.service.request_census = ignore_census;
    config.service.set_events = keep_events;
    config.service.enter_battles = record_enter;
    config.service.leave_battles = record_leave;
    config.service.get_battle = record_get_battle;
    config.service.join_game = record_join;
    config.service.host_game = record_host;
    g_widget = boneyards_status_widget_create(config);
    if (g_widget == nullptr)
        return nullptr;
    click_window(boneyards_status_widget_part(g_widget, OverlayPart::enter));
    MainFrame* frame = g_widget->system.main_frame;
    if (frame == nullptr)
        return g_widget;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        tick(1);
    load_war_map(frame);
    finish_slide(frame);
    // Empyrrean (Arm) selected, for an Arm commander on Empyrrean's area.
    click_window(frame->map_panel->hotspots[1]);
    g_widget->navigation->allegiance = 'A';
    g_widget->navigation->session.area_id = test::k_empyrrean_id;
    return g_widget;
}

BattleConsole* open_battles(MainFrame* frame) {
    g_log.sounds.clear();
    g_log.calls.clear();
    click_window(button(frame, ConsoleButton::battle));
    finish_slide(frame);
    tick(1);
    CHECK(frame->console_screen == console_screen::battle);
    Window* screen = frame->active_screen;
    CHECK(screen != nullptr && window_is_named(screen, "BattleConsole") && window_is_shown(screen));
    return reinterpret_cast<BattleConsole*>(screen);
}

// The build (battle_console_build) and the open slot: the shared console
// image, the titles, the BattleList and HOST, the PlanetIntelList log, the
// Battle button lit and the planet's battles entered.
void check_build(MainFrame* frame, BattleConsole* console) {
    const Window& c = console->base;
    const screens::HatFonts& f = g_widget->system.hat_fonts;
    CHECK(bounds_are(
        &c,
        main_frame_layout::console_x,
        main_frame_layout::console_y,
        main_frame_layout::console_width,
        main_frame_layout::console_height
    ));
    CHECK(c.states.images[1] == g_widget->system.profile_image);
    Window* title = captioned(&console->base, "BATTLES");
    CHECK(
        title != nullptr && title->x == c.x && title->y == c.y + 6 && title->width == 0x7c &&
        title->font == f.big
    );
    CHECK(
        window_is_named(&console->list->list.base, "BattleList") &&
        bounds_are(&console->list->list.base, 0x17, 0x93, 0x7c, 0x104)
    );
    CHECK(
        bounds_are(console->host_button, c.x + 0xc, c.y + 0x11c, 0x63, 0x15) &&
        text_of(console->host_button) == "HOST" &&
        console->host_button->states.current == button_state::unpressed
    );
    CHECK(
        text_of(console->intel_title) == "PLANET INTELLIGENCE" &&
        console->intel_title->x == c.x + 0x7c && console->intel_title->width == 0x176
    );
    const ListBox& intel = console->intel->list;
    CHECK(
        window_is_named(&intel.base, "PlanetIntelList") &&
        bounds_are(&intel.base, c.x + 0x7c, c.y + 0x12, 0x176, 0x122) && intel.capacity == 100 &&
        intel.bottom_align == 1 && intel.log_mode == 1
    );
    CHECK(console->match_control == nullptr);
    auto* battle_button = reinterpret_cast<ConsoleNavButton*>(button(frame, ConsoleButton::battle));
    CHECK(battle_button->base.states.images[button_state::unpressed] == battle_button->lit);
    CHECK(!g_log.calls.empty() && g_log.calls[0] == "enter 2");
    CHECK(frame->activity_light->running == 1);
    for (InfoPane* pane : console->panes)
        CHECK(
            pane != nullptr && pane->base.parent == &console->base && !window_is_shown(&pane->base)
        );
}

// The first pulse lists a row per mode, each with its CetegoryIcon arrow
// pointing down; the battle feed files battles under their modes and the
// pulse lists them indented under their mode, the row's BattleIcon showing
// Battle_Icons frame 1 in progress and 0 forming.
void check_rows(BattleConsole* console) {
    CHECK(
        (row_titles(console) ==
         std::vector<std::string>{
             "For Fun", "Duel", "Winner Takes All", "Galactic War", "War Training"
         })
    );
    Window* mode = row_titled(console, "For Fun");
    Window* arrow = mode != nullptr ? window_find_named(mode, "CetegoryIcon", nullptr) : nullptr;
    CHECK(
        arrow != nullptr && arrow->x == mode->x + 3 && arrow->y == mode->y + 3 &&
        arrow->states.images[1] >= 0
    );
    {
        ScopedServices scope(g_widget);
        g_log.events.battles_done(g_log.events.user, false);
    }
    battle_info(0, "gw", "Last Stand", 4, 1);
    battle_info(0, "ff", "Brawl", 0, 0);
    tick(1);
    CHECK(
        (row_titles(console) == std::vector<std::string>{
                                    "For Fun",
                                    "Brawl",
                                    "Duel",
                                    "Winner Takes All",
                                    "Galactic War",
                                    "Last Stand",
                                    "War Training"
                                })
    );
    Window* brawl = row_titled(console, "Brawl");
    Window* icon = brawl != nullptr ? window_find_named(brawl, "BattleIcon", nullptr) : nullptr;
    // The icon made at the screen point (0x11, 3) is clamped into the row's corner.
    CHECK(
        icon != nullptr && icon->x == brawl->x && icon->y == brawl->y && icon->states.images[1] >= 0
    );
    // The name, as wide as the row less 0xe from 0x1f in, is clamped to end
    // at the row's right edge.
    Window* label = brawl != nullptr ? brawl->children.items[brawl->children.count - 1] : nullptr;
    CHECK(
        label != nullptr && label->x == brawl->x + 0xe &&
        label->x + label->width == brawl->x + brawl->width
    );
    mode = row_titled(console, "For Fun");
    CHECK(mode != nullptr && mode->children.items[mode->children.count - 1]->x == mode->x + 0x10);
}

// Down from the first mode rests on the battle under it. The mode stays open,
// and a later tick does not put the highlight back on the first mode.
void check_arrow_chooses(BattleConsole* console) {
    ListBox* rows = &console->list->list;
    desktop_set_focus(&g_widget->desktop, &rows->base);
    rows->selected = 0;
    console->list->selected_item = list_box_item(rows, 0);
    const int32_t before = rows->count;
    // Arrows choose only once Tab or F6 has shown the keyboard ring.
    g_widget->desktop.keyboard_ring = 1;
    press(key::down);
    tick(1);
    CHECK(rows->selected == 1 && rows->count == before);
    CHECK(console->list->selected_item == list_box_item(rows, 1));
    CHECK(row_titled(console, "Brawl") != nullptr && row_titled(console, "For Fun") != nullptr);
}

// A mode row picked sounds GREEN_BUTTON and hides its battles, its arrow up;
// picked again it lists them. (The list tells the console only of a pick
// that changes its selection, so the first row, selected as the rows are
// built, is not the one picked.)
void check_mode_toggle(BattleConsole* console) {
    Window* mode = row_titled(console, "Galactic War");
    CHECK(mode != nullptr);
    if (mode == nullptr)
        return;
    Window* arrow = window_find_named(mode, "CetegoryIcon", nullptr);
    const std::vector<uint8_t> down =
        arrow != nullptr ? image_bytes(arrow->states.images[1]) : std::vector<uint8_t>{};
    g_log.sounds.clear();
    click_window(mode);
    CHECK(heard("GREEN_BUTTON"));
    tick(1);
    CHECK(row_titled(console, "Last Stand") == nullptr && row_titled(console, "Brawl") != nullptr);
    mode = row_titled(console, "Galactic War");
    arrow = mode != nullptr ? window_find_named(mode, "CetegoryIcon", nullptr) : nullptr;
    CHECK(arrow != nullptr && !down.empty() && image_bytes(arrow->states.images[1]) != down);
    if (mode != nullptr)
        click_window(mode);
    tick(1);
    CHECK(row_titled(console, "Last Stand") != nullptr);
}

// An intelligence line is wrapped into the log.
void check_intel(BattleConsole* console) {
    {
        ScopedServices scope(g_widget);
        g_log.events.intel(g_log.events.user, nullptr, "Arm forces hold the ridge");
    }
    ListBox* intel = &console->intel->list;
    CHECK(intel->count == 1 && text_of(list_box_item(intel, 0)) == "Arm forces hold the ridge");
}

// A forming battle's row opens the Join summary: the battle's intelligence
// is asked for, the log moves down under the pane, and the pulse shows the
// battle, its seats, skill and commander death and the password entry of a
// password game; JOIN tells the console (the light runs) and joins with the
// password typed.
std::string focus_label() {
    Window* focus = g_widget->desktop.focus;
    if (focus == nullptr)
        return "";
    if (focus->name != nullptr && focus->name[0] != '\0')
        return focus->name;
    return text_of(focus);
}

int32_t stop_at(const std::vector<std::string>& stops, const char* name) {
    for (int32_t i = 0; i < static_cast<int32_t>(stops.size()); ++i) {
        if (stops[i] == name)
            return i;
    }
    return -1;
}

// One Tab cycle from `start`, back to `start`. HOST's caption is its name.
std::vector<std::string> tab_cycle(Window* start) {
    std::vector<std::string> stops;
    desktop_set_focus(&g_widget->desktop, start);
    for (int32_t step = 0; step < 16; ++step) {
        press(key::tab);
        stops.push_back(focus_label());
        if (g_widget->desktop.focus == start)
            break;
    }
    return stops;
}

void check_join(MainFrame* frame, BattleConsole* console) {
    Window* row = row_titled(console, "Last Stand");
    CHECK(row != nullptr);
    if (row == nullptr)
        return;
    // Arrows choose the battle the list names. Down leaves Last Stand and
    // Up returns to it, which opens the summary; close that so the click
    // below is the one these checks count.
    ListBox* rows = &console->list->list;
    const int32_t last_index = list_box_index_of(rows, row);
    if (last_index >= 0) {
        rows->selected = last_index;
        console->list->selected_item = row;
        desktop_set_focus(&g_widget->desktop, &rows->base);
        g_widget->desktop.keyboard_ring = 1;
        press(key::down);
        CHECK(rows->selected != last_index);
        press(key::up);
        CHECK(rows->selected == last_index && console->list->selected_item == row);
        auto* opened = reinterpret_cast<JoinSummaryPane*>(console->panes[1]);
        if (opened != nullptr && window_is_shown(&opened->pane.base))
            click_window(opened->pane.close_button);
        // The click below has to change the selection. An arrow that lands
        // on this row again does not, and the list then keeps the click.
        if (rows->count > 1)
            rows->selected = last_index == 0 ? 1 : 0;
    }
    g_log.calls.clear();
    g_log.sounds.clear();
    click_window(row);
    CHECK(!g_log.calls.empty() && g_log.calls[0] == "get 2 gw");
    auto* pane = reinterpret_cast<JoinSummaryPane*>(console->panes[1]);
    CHECK(window_is_shown(&pane->pane.base) && g_widget->navigation->battles.join_visible);
    CHECK(heard_count("GREEN_BUTTON") == 2); // the row, and the pane opening
    // A click brings a pointer move, which hides the keyboard ring. Down still
    // moves the highlight, and the opened battle stays the one that was clicked.
    // Those arrows sound on their own, so they come after the click's count.
    g_widget->desktop.keyboard_ring = 0;
    const int32_t opened = rows->selected;
    const std::string opened_name = text_of(pane->name);
    press(key::down);
    CHECK(rows->selected != opened && console->list->selected_item == row);
    CHECK(window_is_shown(&pane->pane.base) && text_of(pane->name) == opened_name);
    press(key::up);
    CHECK(rows->selected == opened);
    const Window& c = console->base;
    CHECK(console->intel->list.base.y == c.y + 0xab && console->intel->list.base.height == 0x89);
    CHECK(console->intel_title->y == c.y + 0x9f);
    const BattleIntel intel{"gw", 7, "kestrel", 1, "Acid Pools", 1, 1, 3, false};
    {
        ScopedServices scope(g_widget);
        g_log.events.battle_intel(g_log.events.user, &intel);
    }
    report("gw", 7, 0x12, "kestrel", "");
    report("gw", 8, 3, "second", "");
    tick(1);
    CHECK(text_of(pane->name) == "Last Stand");
    CHECK(field_text(pane->game_type) == "Galactic War");
    CHECK(text_of(pane->seat_names[0]) == "kestrel, Arm Supreme Commander");
    CHECK(text_of(pane->seat_names[1]) == "second, Core Sergeant");
    CHECK(text_of(pane->seat_names[2]).empty() && pane->seat_icons[2]->states.images[1] < 0);
    CHECK(
        text_of(pane->seat_names[3]) == "Blocked" &&
        pane->seat_icons[3]->states.images[1] == pane->blocked_icon
    );
    CHECK(
        field_text(pane->skill) == "Seasoned" && field_text(pane->commander_death) == "Deathmatch"
    );
    CHECK(pane->password->base.visible == 1 && pane->password_label->visible == 1);
    press('z');
    CHECK(std::strcmp(single_edit_text(pane->password), "z") == 0);
    // Tab: the battle list, HOST once, then the password and JOIN.
    const std::vector<std::string> stops = tab_cycle(&console->list->list.base);
    const int32_t host_at = stop_at(stops, "HOST");
    const int32_t password_at = stop_at(stops, "Join password");
    const int32_t join_at = stop_at(stops, "JOIN");
    int32_t host_count = 0;
    for (const std::string& stop : stops)
        host_count += stop == "HOST" ? 1 : 0;
    CHECK(host_count == 1 && host_at >= 0 && password_at > host_at && join_at > password_at);
    CHECK(std::strcmp(single_edit_text(pane->password), "z") == 0);
    CHECK(
        text_of(pane->seat_captions[0]) == "Host:" && text_of(pane->seat_captions[3]) == "Player 4:"
    );

    activity_light_stop(frame->activity_light);
    single_edit_set_text(pane->password, "pw");
    CHECK(draws_password(pane->password, "pw"));
    g_log.calls.clear();
    click_window(pane->join);
    CHECK(!g_log.calls.empty() && g_log.calls.back() == "join 2 gw pw");
    CHECK(frame->activity_light->running == 1);
    // Its Close_Button closes it and lets go of the battle.
    click_window(pane->pane.close_button);
    CHECK(!window_is_shown(&pane->pane.base) && !g_widget->navigation->battles.join_visible);
    CHECK(console->intel->list.base.y == c.y + 0x12);
}

// A battle in progress opens the Scoreboard: its name and each seat's rank
// icon, name and numbers, empty seats blank. A report message for it goes to
// the pane's message list, which the pane never builds.
void check_scoreboard(BattleConsole* console) {
    Window* row = row_titled(console, "Brawl");
    CHECK(row != nullptr);
    if (row == nullptr)
        return;
    click_window(row);
    auto* pane = reinterpret_cast<ScoreboardPane*>(console->panes[2]);
    CHECK(window_is_shown(&pane->pane.base) && g_widget->navigation->battles.scoreboard_visible);
    report("ff", 7, 0x12, "kestrel", "");
    tick(1);
    CHECK(text_of(pane->name) == "Brawl");
    CHECK(
        text_of(pane->seats[0].name) == "kestrel" && text_of(pane->seats[0].kills) == "9" &&
        text_of(pane->seats[0].losses) == "2" && text_of(pane->seats[0].energy) == "1500" &&
        text_of(pane->seats[0].metal) == "800" && text_of(pane->seats[0].score) == "40"
    );
    CHECK(pane->seats[0].icon->states.images[1] >= 0);
    CHECK(text_of(pane->seats[1].name).empty() && pane->seats[1].icon->states.images[1] < 0);
    report("ff", 7, 0x12, "kestrel", "gg");
    CHECK(pane->messages == nullptr);
    Window* kills = captioned(&pane->pane.base, "Kills");
    CHECK(kills != nullptr && kills->x == pane->pane.base.x + 0x75 && kills->width == 0x33);
    // HOST closes the Scoreboard and opens the Host pane.
    click_window(console->host_button);
    CHECK(!window_is_shown(&pane->pane.base) && !g_widget->navigation->battles.scoreboard_visible);
}

std::string selected_names(const screens::ByRadioGroup* group) {
    return screens::by_radio_group_selected_name(group);
}

// The Host pane: the game types an Arm commander may host on an Arm planet,
// the first selected; Novice, 2 and Ends chosen; START only with a name;
// Duel fixes two players; START hosts and closes the pane.
void check_host(MainFrame* frame, BattleConsole* console) {
    auto* pane = reinterpret_cast<HostBattlePane*>(console->panes[0]);
    CHECK(window_is_shown(&pane->pane.base) && g_widget->navigation->battles.host_visible);
    ListBox* types = &pane->types->list;
    std::vector<std::string> names;
    for (int32_t i = 0; i < types->count; ++i)
        names.push_back(row_title(list_box_item(types, i)));
    CHECK(
        (names == std::vector<std::string>{"For Fun", "Duel", "Winner Takes All", "War Training"})
    );
    CHECK(pane->types->selected_item == list_box_item(types, 0));
    CHECK(
        selected_names(pane->skill) == "Novice" && selected_names(pane->players) == "2" &&
        selected_names(pane->commander_death) == "Ends"
    );
    const Window& p = pane->pane.base;
    CHECK(bounds_are(&pane->game_name->base, p.x + 0x3d, p.y + 0x1c, 0x6d, 0x15));
    CHECK(
        pane->skill->base.x == p.x + 0xb1 && pane->skill->base.y == p.y + 0x1c &&
        pane->skill->base.height == 0x1f
    );
    CHECK(pane->start->states.current == button_state::disabled);
    tick(1);
    CHECK(pane->start->states.current == button_state::disabled);
    press('Q');
    CHECK(std::strcmp(single_edit_text(pane->game_name), "Q") == 0);
    single_edit_set_text(pane->game_name, "Rumble");
    tick(1);
    CHECK(pane->start->states.current == button_state::unpressed);

    // The form ends at START. Each radio row is one stop. Left and Right move
    // along it, and the line does not wrap. Down steps to the next row.
    const std::vector<std::string> stops = tab_cycle(&pane->game_name->base);
    CHECK(stop_at(stops, "Battle password") >= 0);
    CHECK(stop_at(stops, "Game types") > stop_at(stops, "Battle password"));
    CHECK(stop_at(stops, "Novice") > stop_at(stops, "Game types"));
    CHECK(stop_at(stops, "Seasoned") < 0 && stop_at(stops, "Expert") < 0);
    CHECK(stop_at(stops, "2") > stop_at(stops, "Novice"));
    CHECK(stop_at(stops, "3") < 0 && stop_at(stops, "4") < 0);
    CHECK(stop_at(stops, "Ends") > stop_at(stops, "2"));
    CHECK(stop_at(stops, "START") > stop_at(stops, "Ends"));
    desktop_set_focus(&g_widget->desktop, &pane->skill->members[0]->base);
    press(key::right);
    CHECK(selected_names(pane->skill) == "Seasoned");
    press(key::right);
    CHECK(selected_names(pane->skill) == "Expert");
    press(key::right);
    CHECK(
        selected_names(pane->skill) == "Expert" &&
        g_widget->desktop.focus == &pane->skill->members[2]->base
    );
    press(key::down);
    CHECK(
        selected_names(pane->skill) == "Expert" &&
        g_widget->desktop.focus == &pane->players->members[0]->base
    );
    press(key::down);
    CHECK(g_widget->desktop.focus == &pane->commander_death->members[0]->base);
    // The click further down is what selects Expert, and it counts one sound.
    desktop_set_focus(&g_widget->desktop, &pane->skill->members[2]->base);
    press(key::left);
    press(key::left);
    CHECK(selected_names(pane->skill) == "Novice");
    desktop_set_focus(&g_widget->desktop, &pane->types->list.base);
    // Arrows choose a game type only once Tab or F6 has shown the keyboard
    // ring. Tab through the form already did. With no ring, the clicked
    // type stays.
    g_widget->desktop.keyboard_ring = 1;
    press(key::down);
    CHECK(pane->types->selected_item == list_box_item(types, 1));
    press(key::up);
    CHECK(
        pane->types->selected_item == list_box_item(types, 0) &&
        pane->players->members[1]->base.states.current != button_state::disabled &&
        pane->players->members[2]->base.states.current != button_state::disabled
    );
    g_widget->desktop.keyboard_ring = 0;
    const int32_t type_index = types->selected;
    const Window* type_kept = pane->types->selected_item;
    press(key::down);
    CHECK(types->selected != type_index && pane->types->selected_item == type_kept);
    press(key::up);
    CHECK(types->selected == type_index);

    // Duel: two players, the other counts disabled.
    g_log.sounds.clear();
    click_window(list_box_item(types, 1));
    CHECK(heard("GREEN_BUTTON"));
    CHECK(
        selected_names(pane->players) == "2" &&
        pane->players->members[1]->base.states.current == button_state::disabled &&
        pane->players->members[2]->base.states.current == button_state::disabled
    );
    // A radio picked is the group's selection; the group's notice (code 200)
    // reaches the Host pane, which takes it for no button of its own: the
    // pane stays up and only the group's GREEN_BUTTON sounds.
    g_log.sounds.clear();
    click_window(&pane->skill->members[2]->base);
    CHECK(
        heard("GREEN_BUTTON") && selected_names(pane->skill) == "Expert" &&
        pane->skill->members[2]->base.states.current == button_state::pressed
    );
    CHECK(heard_count("GREEN_BUTTON") == 1 && g_log.sounds.size() == 1);
    CHECK(window_is_shown(&pane->pane.base) && g_widget->navigation->battles.host_visible);

    activity_light_stop(frame->activity_light);
    single_edit_set_text(pane->password, "blaze");
    CHECK(pane->game_name->mask == '\0' && draws_password(pane->password, "blaze"));
    g_log.calls.clear();
    click_window(pane->start);
    CHECK(!g_log.calls.empty() && g_log.calls.back() == "host");
    CHECK(
        g_log.hosted.area_id == 2 && g_log.hosted.game_mode == 1 && g_log.hosted_name == "Rumble" &&
        g_log.hosted_password == "blaze" && g_log.hosted.skill == 2 && g_log.hosted.players == 0 &&
        g_log.hosted.commander_death == 2
    );
    CHECK(!window_is_shown(&pane->pane.base) && !g_widget->navigation->battles.host_visible);
    CHECK(frame->activity_light->running == 1);
}

// Release closes the console: the log empties, the battles are left.
void check_close(MainFrame* frame, BattleConsole* console) {
    g_log.calls.clear();
    click_window(button(frame, ConsoleButton::release));
    CHECK(frame->console_screen == console_screen::none && console->base.visible == 0);
    CHECK(console->intel->list.count == 0);
    CHECK(!g_log.calls.empty() && g_log.calls.back() == "leave 2");
    finish_slide(frame);
}

// With matchmaking the list and HOST make room for the matchmaking panel,
// which starts minimized along the bottom; its buttons move it between its
// faces, each sounding GREEN_BUTTON, and SETTINGS flips its flag, which the
// console hears of and ignores.
void check_matchmaking(MainFrame* frame) {
    g_widget->system.matchmaking_enabled = 1;
    main_frame_enable_buttons(frame);
    BattleConsole* console = open_battles(frame);
    CHECK(console != nullptr);
    if (console == nullptr)
        return;
    const Window& c = console->base;
    CHECK(console->list->list.base.height == 0xf3 && console->host_button->y == c.y + 0x10b);
    MatchControlPanel* panel = console->match_control;
    CHECK(panel != nullptr);
    if (panel == nullptr)
        return;
    const screens::MatchmakingState& state = g_widget->navigation->matchmaking;
    CHECK(
        window_is_named(&panel->base, "MatchControlPanel") && panel->base.x == c.x &&
        panel->base.width == 0x7b
    );
    CHECK(
        panel->base.y == c.y + 0x123 && panel->base.height == 0x16 &&
        panel->base.states.images[1] == panel->images[0]
    );
    CHECK(
        panel->activate->visible == 0 && panel->settings->visible == 0 &&
        panel->content->list.base.visible == 0
    );
    CHECK(text_of(panel->title) == "MATCHMAKING" && panel->title->y == panel->base.y + 4);

    g_log.sounds.clear();
    click_window(panel->minimize);
    CHECK(heard("GREEN_BUTTON") && state.face == match_face::minimized);
    CHECK(
        panel->base.y == c.y + 0x10b && panel->base.height == 0x2e &&
        panel->base.states.images[1] == panel->images[match_face::active] &&
        panel->activate->visible == 1 && text_of(panel->activate) == "ACTIVATE" &&
        panel->settings->visible == 1 && panel->content->list.base.visible == 0
    );
    // Minimized, ACTIVATE does nothing and SETTINGS flips nothing, and neither
    // opens the Matchmaking dialog.
    click_window(panel->activate);
    click_window(panel->settings);
    CHECK(state.face == match_face::minimized && !state.settings_flag);
    CHECK(matchmaking_dialog_find(g_widget) == nullptr);
    g_widget->navigation->matchmaking.face = match_face::active;
    click_window(panel->activate);
    CHECK(
        state.face == match_face::deactivated && panel->base.y == c.y + 0xd6 &&
        panel->base.height == 0x63 && text_of(panel->activate) == "DEACTIVATE" &&
        panel->content->list.base.visible == 1 &&
        panel->base.states.images[1] == panel->images[match_face::deactivated]
    );
    Window* dialog = matchmaking_dialog_find(g_widget);
    CHECK(
        dialog != nullptr && window_is_shown(dialog) &&
        matchmaking_dialog_searching(dialog) == k_matchmaking_searching &&
        bounds_are(
            dialog,
            matchmaking_dialog_layout::x,
            matchmaking_dialog_layout::y,
            matchmaking_dialog_layout::width,
            matchmaking_dialog_layout::height
        )
    );
    CHECK(!boneyards_status_widget_message_open(g_widget));
    // SETTINGS flips the flag and passes the press on; the console takes no
    // action on it: no sound of its own, no pane and no activity light.
    g_log.sounds.clear();
    g_log.calls.clear();
    activity_light_stop(frame->activity_light);
    click_window(panel->settings);
    CHECK(state.settings_flag && heard_count("GREEN_BUTTON") == 1 && g_log.sounds.size() == 1);
    CHECK(
        matchmaking_dialog_searching(matchmaking_dialog_find(g_widget)) == k_matchmaking_settings
    );
    CHECK(g_log.calls.empty() && frame->activity_light->running == 0);
    for (InfoPane* pane : console->panes)
        CHECK(pane == nullptr || !window_is_shown(&pane->base));
    click_window(panel->settings);
    CHECK(!state.settings_flag && heard_count("GREEN_BUTTON") == 2);
    click_window(panel->minimize);
    CHECK(
        state.face == match_face::active && panel->base.y == c.y + 0x123 &&
        panel->base.states.images[1] == panel->images[match_face::minimized]
    );
    // An account that does not see Matchmaking still moves between the faces,
    // and neither ACTIVATE nor SETTINGS opens the dialog.
    g_widget->system.matchmaking_enabled = 0;
    if (Window* open = matchmaking_dialog_find(g_widget))
        open->visible = 0;
    {
        ScopedServices scope(g_widget);
        match_control_panel_show(panel, match_face::active);
    }
    CHECK(
        state.face == match_face::active && panel->base.y == c.y + 0x10b &&
        panel->base.height == 0x2e && text_of(panel->activate) == "ACTIVATE" &&
        panel->content->list.base.visible == 0 &&
        panel->base.states.images[1] == panel->images[match_face::active]
    );
    click_window(panel->activate);
    CHECK(
        state.face == match_face::deactivated && text_of(panel->activate) == "DEACTIVATE" &&
        panel->base.states.images[1] == panel->images[match_face::deactivated] &&
        (matchmaking_dialog_find(g_widget) == nullptr ||
         !window_is_shown(matchmaking_dialog_find(g_widget)))
    );
    click_window(panel->activate);
    CHECK(
        state.face == match_face::active && text_of(panel->activate) == "ACTIVATE" &&
        panel->base.y == c.y + 0x10b && panel->base.height == 0x2e &&
        panel->content->list.base.visible == 0 &&
        panel->base.states.images[1] == panel->images[match_face::active] &&
        (matchmaking_dialog_find(g_widget) == nullptr ||
         !window_is_shown(matchmaking_dialog_find(g_widget)))
    );
    click_window(panel->settings);
    CHECK(
        state.settings_flag && (matchmaking_dialog_find(g_widget) == nullptr ||
                                !window_is_shown(matchmaking_dialog_find(g_widget)))
    );
    if (Window* open = matchmaking_dialog_find(g_widget))
        open->visible = 0;
    click_window(button(frame, ConsoleButton::release));
    finish_slide(frame);
}

void test_battle_console(Data* data) {
    if (open_widget(data) == nullptr) {
        CHECK(false);
        return;
    }
    MainFrame* frame = g_widget->system.main_frame;
    CHECK(frame != nullptr && g_widget->navigation->galaxy.selected == 1);
    if (frame == nullptr)
        return;
    check_ordinary_routes(frame);
    check_privileged_routes(frame);
    BattleConsole* console = open_battles(frame);
    if (console == nullptr)
        return;
    check_build(frame, console);
    check_rows(console);
    check_arrow_chooses(console);
    check_mode_toggle(console);
    check_intel(console);
    check_join(frame, console);
    check_scoreboard(console);
    check_host(frame, console);
    draw_frame();
    check_close(frame, console);
    check_matchmaking(frame);
    tick(1);
    boneyards_status_widget_destroy(g_widget);
    g_widget = nullptr;
}

// The Locator console is the screen in coreprime.ccx. This case opens it.
bool read_coreprime(void* context, const char* path, std::vector<uint8_t>* out) {
    if (context == nullptr || path == nullptr || out == nullptr)
        return false;
    auto* archive = static_cast<oa::HpiArchive*>(context);
    oa::base::bytes::Decoded<std::vector<uint8_t>> read = archive->read(path);
    if (!read.ok() || !read.value.has_value())
        return false;
    *out = std::move(*read.value);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the battle console tests");
    const std::filesystem::path archive_path = oa::boneyards_test::coreprime_archive(argc, argv);
    if (archive_path.empty()) {
        std::fprintf(stderr, "the mod folder is required\n");
        return 1;
    }
    oa::base::bytes::Decoded<oa::HpiArchive> coreprime = oa::open_hpi_file(archive_path);
    if (!coreprime.ok() || !coreprime.value.has_value()) {
        std::fprintf(stderr, "coreprime.ccx could not be opened\n");
        return 1;
    }
    rover_session_set_virtual_files(read_coreprime, &*coreprime.value);
    Data data;
    (void)data.store.discover("31");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    test_battle_console(&data);
    rover_session_set_virtual_files(nullptr, nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("battle console tests passed\n");
    return 0;
}
