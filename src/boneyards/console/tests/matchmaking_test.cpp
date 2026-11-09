// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Matchmaking dialog over Chat and Battle: the settings face, the
// searching face, the queue records, the offer, the result boxes and the
// five-second wait. Pixels are checked against the dialog's glass, meter,
// scanner and controls.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "battle_console.hpp"
#include "console_layer.hpp"
#include "console_state.hpp"
#include "galaxy_fixture.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/matchmaking.hpp"
#include "oa/boneyards/service.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"

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
};

Log g_log;
StatusWidget* g_widget = nullptr;
std::vector<MatchmakingNotice> g_notices;
ServiceResult g_look_result = ServiceResult::pending;

void record_sound(void*, const char* name) {
    g_log.sounds.emplace_back(name != nullptr ? name : "");
}

bool connected(void*) {
    return true;
}

ActivationResult activate(void*) {
    return ActivationResult::ok;
}

void ignore_census(void*, uint32_t) {
}

void keep_events(void*, const ServiceEvents*) {
}

ServiceResult record_enter(void*, uint32_t area) {
    g_log.calls.push_back("enter " + std::to_string(area));
    return ServiceResult::pending;
}

ServiceResult record_leave(void*, uint32_t area) {
    g_log.calls.push_back("leave " + std::to_string(area));
    return ServiceResult::pending;
}

ServiceResult record_get(void*, uint32_t, const char*) {
    g_log.calls.emplace_back("get");
    return ServiceResult::pending;
}

ServiceResult record_join(void*, uint32_t area, const char* game_id, const char* password) {
    g_log.calls.push_back(
        "join area=" + std::to_string(area) + " game=" + (game_id != nullptr ? game_id : "") +
        " password=" + (password != nullptr ? password : "-")
    );
    return ServiceResult::pending;
}

ServiceResult record_host(void*, const HostGameRequest* request) {
    const char* name = request != nullptr && request->name != nullptr ? request->name : "";
    const char* password =
        request != nullptr && request->password != nullptr ? request->password : "-";
    g_log.calls.push_back(
        "host area=" + std::to_string(request->area_id) +
        " mode=" + std::to_string(request->game_mode) + " name=" + name + " password=" + password +
        " skill=" + std::to_string(request->skill) + " players=" +
        std::to_string(request->players) + " death=" + std::to_string(request->commander_death)
    );
    return ServiceResult::pending;
}

std::string area_of(const MatchmakingQuery* query) {
    if (query == nullptr || !query->area_known)
        return "-";
    return std::to_string(query->area_id);
}

std::string filters_of(const char* verb, const MatchmakingQuery* query) {
    return std::string(verb) + " type=" + std::to_string(query->game_type) +
           " skill=" + std::to_string(query->skill_mode) + " side=" + std::to_string(query->side) +
           " planets=" + std::to_string(query->planets) + " area=" + area_of(query);
}

ServiceResult record_match_look(void*, const MatchmakingQuery* query) {
    g_log.calls.push_back(filters_of("look", query));
    return g_look_result;
}

ServiceResult record_match_join(void*, const MatchmakingQuery* query) {
    g_log.calls.push_back(filters_of("join", query));
    return ServiceResult::pending;
}

ServiceResult record_match_cancel(void*) {
    g_log.calls.emplace_back("cancel");
    return ServiceResult::pending;
}

ServiceResult record_match_answer(void*, const char* offer_id, int32_t accept) {
    g_log.calls.push_back(
        "answer accept=" + std::to_string(accept) + " id=" + (offer_id != nullptr ? offer_id : "")
    );
    return ServiceResult::pending;
}

bool record_take(void*, MatchmakingNotice* out) {
    if (g_notices.empty() || out == nullptr)
        return false;
    *out = g_notices.front();
    g_notices.erase(g_notices.begin());
    return true;
}

ServiceResult record_chat(void*, uint32_t, uint32_t, uint32_t, const char*) {
    g_log.calls.emplace_back("chat");
    return ServiceResult::pending;
}

ServiceResult record_page(void*, const char*, const char*) {
    g_log.calls.emplace_back("page");
    return ServiceResult::pending;
}

ServiceResult record_locate(void*, const char*, LocateCallback, void*) {
    g_log.calls.emplace_back("locate");
    return ServiceResult::pending;
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

void tick(int32_t cycles) {
    for (int32_t i = 0; i < cycles; ++i)
        boneyards_status_widget_tick(g_widget);
}

void finish_slide(const MainFrame* frame) {
    for (int32_t i = 0; frame->console->sliding != 0 && i < 100; ++i)
        tick(1);
}

std::string text_of(Window* w) {
    ScopedServices scope(g_widget);
    const char* text = w != nullptr ? w->ops->state_text(w, -1) : nullptr;
    return text != nullptr ? text : "";
}

bool bounds_are(const Window* w, int32_t x, int32_t y, int32_t width, int32_t height) {
    return w != nullptr && w->x == x && w->y == y && w->width == width && w->height == height;
}

Window* named(Window* parent, const char* name) {
    return parent != nullptr ? window_find_named(parent, name, nullptr) : nullptr;
}

Window* button_of(const MainFrame* frame, ConsoleButton which) {
    return frame->console_buttons[static_cast<int32_t>(which)];
}

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
    config.service.get_battle = record_get;
    config.service.join_game = record_join;
    config.service.host_game = record_host;
    config.service.send_chat = record_chat;
    config.service.page_user = record_page;
    config.service.locate_user = record_locate;
    config.service.matchmaking_look = record_match_look;
    config.service.matchmaking_join = record_match_join;
    config.service.matchmaking_cancel = record_match_cancel;
    config.service.matchmaking_answer = record_match_answer;
    config.service.matchmaking_take = record_take;
    g_notices.clear();
    g_look_result = ServiceResult::pending;
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
    click_window(frame->map_panel->hotspots[1]);
    g_widget->navigation->allegiance = 'A';
    g_widget->navigation->session.area_id = test::k_empyrrean_id;
    g_widget->system.matchmaking_enabled = 1;
    main_frame_enable_buttons(frame);
    return g_widget;
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

uint8_t pixel_at(const std::vector<uint8_t>& screen, int32_t x, int32_t y) {
    return screen[static_cast<std::size_t>(y * 640 + x)];
}

void check_pixel(const std::vector<uint8_t>& screen, int32_t x, int32_t y, uint8_t color) {
    const uint8_t got = pixel_at(screen, x, y);
    if (got != color)
        std::fprintf(stderr, "pixel %d,%d is %u, expected %u\n", x, y, got, color);
    CHECK(got == color);
}

// Opaque pixels of one frame, drawn at (x, y). `max_rows` limits the comparison to the top of a
// tall picture. `border` compares only the outer rows and columns, clear of a centred caption.
bool art_at(
    const std::vector<uint8_t>& screen,
    const char* gaf,
    const char* sequence,
    int32_t frame,
    int32_t x,
    int32_t y,
    int32_t max_rows,
    bool border,
    bool report = true
) {
    ScopedServices scope(g_widget);
    const Handle image = console_images_load(
        g_widget->images, gaf, sequence, frame, static_cast<uint8_t>(screens::kWidgetKey)
    );
    Surface surface{};
    const bool opened = image >= 0 && image_surface(image, &surface);
    int32_t matched = 0;
    int32_t mismatched = 0;
    if (opened) {
        const int32_t rows = max_rows > 0 && max_rows < surface.height ? max_rows : surface.height;
        for (int32_t row = 0; row < rows; ++row) {
            for (int32_t col = 0; col < surface.width; ++col) {
                const bool edge =
                    row < 2 || row >= surface.height - 2 || col < 2 || col >= surface.width - 2;
                if (border && !edge)
                    continue;
                const uint8_t pixel = surface.pixels[row * surface.pitch + col];
                if (pixel == screens::kWidgetKey)
                    continue;
                const int32_t sx = x + col;
                const int32_t sy = y + row;
                if (sx < 0 || sy < 0 || sx >= 640 || sy >= 480 || pixel_at(screen, sx, sy) != pixel)
                    ++mismatched;
                else
                    ++matched;
            }
        }
    }
    if (image >= 0)
        image_release(image);
    if (report && (!opened || mismatched != 0 || matched == 0))
        std::fprintf(
            stderr,
            "%s frame %d at %d,%d matched %d mismatched %d\n",
            sequence,
            frame,
            x,
            y,
            matched,
            mismatched
        );
    return opened && mismatched == 0 && matched > 0;
}

bool minimize_is(Window* button, int32_t frame) {
    if (button == nullptr)
        return false;
    ScopedServices scope(g_widget);
    const Handle image = console_images_load(
        g_widget->images,
        screens::kWidgetsGaf,
        "Minimize_Button",
        frame,
        static_cast<uint8_t>(screens::kWidgetKey)
    );
    Surface want{};
    Surface have{};
    const Handle shown = button->states.images[button_state::unpressed];
    bool same = image >= 0 && shown >= 0 && image_surface(image, &want) &&
                image_surface(shown, &have) && want.width == have.width &&
                want.height == have.height;
    if (same) {
        for (int32_t row = 0; row < want.height && same; ++row)
            for (int32_t col = 0; col < want.width; ++col)
                if (want.pixels[row * want.pitch + col] != have.pixels[row * have.pitch + col])
                    same = false;
    }
    if (image >= 0)
        image_release(image);
    return same;
}

bool read_ccx(void* context, const char* path, std::vector<uint8_t>* out) {
    if (context == nullptr || path == nullptr || out == nullptr)
        return false;
    oa::base::bytes::Decoded<std::vector<uint8_t>> read =
        static_cast<oa::HpiArchive*>(context)->read(path);
    if (!read.ok() || !read.value.has_value())
        return false;
    *out = std::move(*read.value);
    return true;
}

void expect_control(
    const GuiScreen* screen, const char* name, int32_t x, int32_t y, int32_t width, int32_t height
) {
    const GuiControl* control = gui_screen_find(screen, name);
    CHECK(control != nullptr);
    if (control == nullptr) {
        std::fprintf(stderr, "missing gadget %s\n", name);
        return;
    }
    const bool placed =
        control->x == x && control->y == y && control->width == width && control->height == height;
    if (!placed)
        std::fprintf(
            stderr,
            "%s is %d,%d %dx%d, expected %d,%d %dx%d\n",
            name,
            control->x,
            control->y,
            control->width,
            control->height,
            x,
            y,
            width,
            height
        );
    CHECK(placed);
}

void push_status(int32_t result, int32_t queued, int32_t count, int32_t meter) {
    MatchmakingNotice notice{};
    notice.kind = MatchmakingNoticeKind::status;
    notice.status.result = result;
    notice.status.queued = queued;
    notice.status.count = count;
    notice.status.meter = meter;
    g_notices.push_back(notice);
}

void push_found(
    const char* offer_id,
    const char* name,
    int32_t skill,
    int32_t game_type,
    int32_t role,
    int32_t side,
    uint32_t area_id
) {
    MatchmakingNotice notice{};
    notice.kind = MatchmakingNoticeKind::found;
    std::snprintf(notice.found.offer_id, sizeof notice.found.offer_id, "%s", offer_id);
    std::snprintf(notice.found.name, sizeof notice.found.name, "%s", name);
    notice.found.skill = skill;
    notice.found.game_type = game_type;
    notice.found.role = role;
    notice.found.side = side;
    notice.found.area_id = area_id;
    g_notices.push_back(notice);
}

void push_go_host() {
    MatchmakingNotice notice{};
    notice.kind = MatchmakingNoticeKind::go;
    std::snprintf(notice.go.offer_id, sizeof notice.go.offer_id, "a1b2c3d4e5f607");
    std::snprintf(notice.go.name, sizeof notice.go.name, "ArmSniper");
    notice.go.name_known = true;
    notice.go.result = 0;
    notice.go.role = 1;
    notice.go.area_id = 2;
    notice.go.game_type = 1;
    notice.go.skill = 1;
    notice.go.seats = 2;
    notice.go.on_death = 2;
    notice.go.side = 0;
    notice.go.side_known = true;
    g_notices.push_back(notice);
}

void push_go_join() {
    MatchmakingNotice notice{};
    notice.kind = MatchmakingNoticeKind::go;
    std::snprintf(notice.go.offer_id, sizeof notice.go.offer_id, "a1b2c3d4e5f607");
    std::snprintf(notice.go.game_id, sizeof notice.go.game_id, "c0ffee0a11b223");
    notice.go.game_known = true;
    notice.go.result = 0;
    notice.go.role = 0;
    notice.go.area_id = 2;
    notice.go.side = 1;
    notice.go.side_known = true;
    g_notices.push_back(notice);
}

void dismiss_ok() {
    int32_t x = 0;
    int32_t y = 0;
    if (!boneyards_status_widget_message_button(g_widget, message_box_name::ok, &x, &y))
        return;
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_up, x, y, 0));
}

std::string box_text() {
    MessageBox* box = message_box_find_open(&g_widget->desktop);
    Window* text = box != nullptr ? named(&box->base, message_box_name::text) : nullptr;
    return text_of(text);
}

const char* k_any_join = "join type=0 skill=1 side=2 planets=1 area=-";
const char* k_any_look = "look type=0 skill=1 side=2 planets=1 area=-";

void check_mapping() {
    CHECK(screens::matchmaking_players_radio(2) == 0);
    CHECK(screens::matchmaking_players_radio(3) == 1);
    CHECK(screens::matchmaking_players_radio(4) == 2);
    CHECK(screens::matchmaking_players_radio(9) == 0);
    MatchmakingGoNotice go{};
    std::snprintf(go.name, sizeof go.name, "ArmSniper");
    go.name_known = true;
    go.seats = 2;
    go.skill = 1;
    go.on_death = 2;
    go.game_type = 1;
    go.area_id = 2;
    HostGameRequest request{};
    CHECK(screens::matchmaking_host_request(&go, &request));
    CHECK(request.players == 0 && request.password == nullptr);
    CHECK(std::strcmp(request.name, "ArmSniper") == 0);
    CHECK(request.skill == 1 && request.commander_death == 2);
    CHECK(request.game_mode == 1 && request.area_id == 2);

    screens::MatchmakingChoices choices{};
    screens::matchmaking_setup(&choices);
    choices.queued = true;
    MatchmakingStatusNotice status{};
    status.queued = 1;
    status.count = 4;
    status.meter = 4;
    screens::MatchmakingEffect effect = screens::matchmaking_apply_status(&choices, &status);
    CHECK(choices.queue_count == 4 && screens::matchmaking_meter_frame(&choices) == 4);
    CHECK(choices.queued && effect.face == screens::MatchmakingFaceChange::none);
    status.count = 12;
    status.meter = 12;
    screens::matchmaking_apply_status(&choices, &status);
    CHECK(choices.queue_count == 12 && screens::matchmaking_meter_frame(&choices) == 10);
    status.result = 5;
    status.queued = 1;
    status.meter = 4;
    effect = screens::matchmaking_apply_status(&choices, &status);
    CHECK(effect.show_box && effect.box_result == 5);
    CHECK(effect.face == screens::MatchmakingFaceChange::none && choices.queued);
    status.result = 4;
    status.queued = 0;
    effect = screens::matchmaking_apply_status(&choices, &status);
    CHECK(effect.face == screens::MatchmakingFaceChange::settings);
    CHECK(effect.show_box && effect.box_result == 4 && !choices.queued);
    CHECK(std::strcmp(screens::matchmaking_unavailable_text(), "The queue is unavailable.") == 0);
    CHECK(screens::matchmaking_result_text(0) == nullptr);
    CHECK(screens::matchmaking_result_text(3) == nullptr);
}

std::vector<std::string> g_launches;

void pump_service(BoneyardsService* online) {
    online->pump(online->user);
}

bool sent_is(std::size_t index, const char* text) {
    ReplaySent sent{};
    const bool found = rover_replay_sent(index, &sent);
    if (!found || std::strcmp(sent.text, text) != 0) {
        std::fprintf(
            stderr,
            "sent %zu is \"%s\", expected \"%s\"\n",
            index,
            found ? sent.text : "(none)",
            text
        );
        return false;
    }
    return true;
}

void check_replay() {
    char error[0x200] = {};
    const bool installed =
        rover_replay_install(OA_BY_CLIENT_TEST_DATA "/matchmaking.replay", error, sizeof error);
    if (!installed)
        std::fprintf(stderr, "%s\n", error);
    CHECK(installed);
    if (!installed)
        return;
    *rover_session() = RoverSession{};
    BoneyardsService online = rover_session_service();
    g_launches.clear();
    ServiceEvents events{};
    events.launch = [](void*, const LaunchReport* launch) {
        const char* game_id =
            launch != nullptr && launch->game_id != nullptr ? launch->game_id : "";
        const char* address =
            launch != nullptr && launch->address != nullptr ? launch->address : "";
        const char* password =
            launch != nullptr && launch->password != nullptr ? launch->password : "";
        const int32_t creator = launch != nullptr && launch->creator ? 1 : 0;
        const int32_t result = launch != nullptr ? launch->result : -1;
        const uint32_t area = launch != nullptr ? launch->area_id : 0;
        g_launches.push_back(
            std::string(game_id) + "," + address + "," + password + "," + std::to_string(creator) +
            "," + std::to_string(result) + "," + std::to_string(area)
        );
    };
    online.set_events(online.user, &events);
    if (online.activate(online.user) != ActivationResult::ok) {
        CHECK(false);
        online.set_events(online.user, nullptr);
        rover_replay_uninstall();
        return;
    }
    MatchmakingQuery query{};
    query.game_type = 1;
    query.skill_mode = 0;
    query.side = 1;
    query.planets = 0;
    query.area_id = 2;
    query.area_known = true;
    std::size_t at = rover_replay_sent_count();
    CHECK(online.matchmaking_look(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at,
        "{[GameManager<U:area_id=2><S:command=matchmaking_look><I:game_type=1><I:planets=0>"
        "<I:side=1><I:skill_mode=0>]}"
    ));
    MatchmakingNotice notice{};
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::status);
    CHECK(notice.status.count == 4 && notice.status.meter == 4 && notice.status.queued == 0);

    at = rover_replay_sent_count();
    CHECK(online.matchmaking_join(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at,
        "{[GameManager<U:area_id=2><S:command=matchmaking_join><I:game_type=1><I:planets=0>"
        "<I:side=1><I:skill_mode=0>]}"
    ));
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::status && notice.status.queued == 1);
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::found);
    CHECK(std::strcmp(notice.found.name, "ArmSniper") == 0);
    CHECK(notice.found.role == 1 && notice.found.skill == 1 && notice.found.area_id == 2);

    at = rover_replay_sent_count();
    CHECK(online.matchmaking_answer(online.user, "a1b2c3d4e5f607", 1) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at, "{[GameManager<I:accept=1><S:command=matchmaking_answer><S:offer_id=a1b2c3d4e5f607>]}"
    ));
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::go && notice.go.result == 1);

    at = rover_replay_sent_count();
    CHECK(online.matchmaking_join(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::found && notice.found.role == 1);
    CHECK(
        online.matchmaking_answer(online.user, notice.found.offer_id, 1) == ServiceResult::pending
    );
    pump_service(&online);
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::go && notice.go.result == 0 && notice.go.role == 1);
    HostGameRequest host{};
    CHECK(screens::matchmaking_host_request(&notice.go, &host));
    CHECK(host.players == 0 && host.password == nullptr);
    at = rover_replay_sent_count();
    CHECK(online.host_game(online.user, &host) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at,
        "{[GameManager<U:area_id=2><S:command=host_game><S:name=ArmSniper><I:on_death=2>"
        "<I:players=2><I:skill=1><I:type=1>]}"
    ));
    CHECK(g_launches.size() == 1);
    CHECK(g_launches[0] == "c0ffee0a11b223,127.0.0.1,QWERTY,1,0,2");

    CHECK(online.matchmaking_join(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::found && notice.found.role == 0);
    CHECK(
        online.matchmaking_answer(online.user, notice.found.offer_id, 1) == ServiceResult::pending
    );
    pump_service(&online);
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::go && notice.go.role == 0 && notice.go.game_known);
    at = rover_replay_sent_count();
    CHECK(
        online.join_game(online.user, notice.go.area_id, notice.go.game_id, nullptr) ==
        ServiceResult::pending
    );
    pump_service(&online);
    CHECK(
        sent_is(at, "{[GameManager<U:area_id=2><S:command=join_game><S:game_id=c0ffee0a11b223>]}")
    );
    CHECK(g_launches.size() == 2);
    CHECK(g_launches[1] == "c0ffee0a11b223,127.0.0.1,QWERTY,0,0,2");

    at = rover_replay_sent_count();
    CHECK(online.matchmaking_cancel(online.user) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(at, "{[GameManager<S:command=matchmaking_cancel>]}"));
    CHECK(online.matchmaking_take(online.user, &notice));
    CHECK(notice.kind == MatchmakingNoticeKind::status && notice.status.queued == 0);

    query.game_type = 0;
    query.skill_mode = 1;
    query.side = 2;
    query.planets = 1;
    query.area_known = false;
    at = rover_replay_sent_count();
    CHECK(online.matchmaking_look(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at,
        "{[GameManager<S:command=matchmaking_look><I:game_type=0><I:planets=1><I:side=2>"
        "<I:skill_mode=1>]}"
    ));
    CHECK(!online.matchmaking_take(online.user, &notice));

    online.close(online.user);
    pump_service(&online);
    pump_service(&online);
    online.set_events(online.user, nullptr);
    rover_replay_uninstall();
}

} // namespace

int main(int argc, char** argv) {
    check_mapping();
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the matchmaking dialog");
    const std::filesystem::path coreprime = oa::boneyards_test::coreprime_archive(argc, argv);
    if (coreprime.empty()) {
        std::fprintf(stderr, "the mod folder is required\n");
        return 1;
    }
    oa::base::bytes::Decoded<oa::HpiArchive> packed = oa::open_hpi_file(coreprime);
    CHECK(packed.ok() && packed.value.has_value());
    if (!(packed.ok() && packed.value.has_value()))
        return 1;
    rover_session_set_virtual_files(read_ccx, &*packed.value);

    // The game's files are the install's alone, as on a profile that does not
    // mount the mod: the dialog's layout and its scanner and meter come
    // through the client session's reader, from the archive.
    Data data;
    (void)data.store.discover("31");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};

    if (open_widget(&data) == nullptr || g_widget->system.main_frame == nullptr) {
        CHECK(false);
        return 1;
    }
    MainFrame* frame = g_widget->system.main_frame;
    CHECK(g_widget->navigation->galaxy.selected == 1);

    g_log.sounds.clear();
    g_log.calls.clear();
    click_window(button_of(frame, ConsoleButton::chat));
    finish_slide(frame);
    ChatView* chat = console_open_chat();
    CHECK(chat != nullptr && chat->matchmaking != nullptr && window_is_shown(&chat->base));
    if (chat == nullptr || chat->matchmaking == nullptr)
        return 1;

    g_log.sounds.clear();
    g_log.calls.clear();
    click_window(chat->matchmaking);
    Window* dialog = matchmaking_dialog_find(g_widget);
    CHECK(
        dialog != nullptr && window_is_shown(dialog) &&
        matchmaking_dialog_searching(dialog) == k_matchmaking_settings &&
        !matchmaking_dialog_queued(dialog) &&
        bounds_are(
            dialog,
            matchmaking_dialog_layout::x,
            matchmaking_dialog_layout::y,
            matchmaking_dialog_layout::width,
            matchmaking_dialog_layout::height
        )
    );
    CHECK(heard_count("GREEN_BUTTON") == 1);
    CHECK(g_log.calls.size() == 1);
    CHECK(g_log.calls[0] == "look type=1 skill=0 side=1 planets=0 area=2");
    CHECK(matchmaking_dialog_requests(dialog) == 1);
    CHECK(!boneyards_status_widget_message_open(g_widget));
    CHECK(text_of(named(dialog, "QueueLine")) == "Commanders queued on Empyrrean");
    CHECK(text_of(named(dialog, "QueueCount")) == "0");
    CHECK(
        named(dialog, "Duel") != nullptr &&
        named(dialog, "Duel")->states.current == button_state::pressed
    );
    CHECK(
        named(dialog, "Core") != nullptr &&
        named(dialog, "Core")->states.current == button_state::pressed
    );
    CHECK(
        named(dialog, "Near mine") != nullptr &&
        named(dialog, "Near mine")->states.current == button_state::pressed
    );
    CHECK(
        named(dialog, "This planet") != nullptr &&
        named(dialog, "This planet")->states.current == button_state::pressed
    );
    CHECK(bounds_are(named(dialog, "Close"), 472, 146, 13, 12));
    CHECK(bounds_are(named(dialog, "Activate"), 162, 358, 99, 21));
    CHECK(named(dialog, "Activate")->visible == 1 && named(dialog, "Deactivate")->visible == 0);
    CHECK(g_widget->desktop.focus == chat->matchmaking);
    // Chat is open, so the button that opened the dialog is showing. Tab lands
    // on Activate. Escape closes and hands the keyboard back. Return then must
    // not start a search.
    Message tab{};
    tab.code = MessageCode::key_down;
    tab.key = key::tab;
    boneyards_status_widget_event(g_widget, tab);
    CHECK(g_widget->desktop.focus == named(dialog, "Activate"));
    Message escape{};
    escape.code = MessageCode::key_down;
    escape.key = key::escape;
    CHECK(boneyards_status_widget_event(g_widget, escape));
    CHECK(!window_is_shown(dialog));
    CHECK(g_widget->desktop.focus == chat->matchmaking);
    g_log.calls.clear();
    Message ret{};
    ret.code = MessageCode::key_down;
    ret.key = key::enter;
    boneyards_status_widget_event(g_widget, ret);
    bool searched = false;
    for (const std::string& call : g_log.calls)
        searched = searched || call.rfind("join ", 0) == 0;
    CHECK(!searched && !matchmaking_dialog_queued(dialog));
    CHECK(g_widget->desktop.focus == chat->matchmaking);
    if (!window_is_shown(dialog)) {
        ScopedServices scope(g_widget);
        dialog->focus_return = nullptr;
        services_desktop_focus(chat->matchmaking);
        matchmaking_dialog_open(g_widget, k_matchmaking_settings);
    }
    // Tab onto Close, then Escape: a key closed the dialog, so the opener gets
    // the keyboard back. A click on Close leaves the keyboard on Close, as it
    // always has, so the opener draws no white frame.
    CHECK(window_is_shown(dialog) && g_widget->desktop.focus == chat->matchmaking);
    boneyards_status_widget_event(g_widget, tab);
    boneyards_status_widget_event(g_widget, tab);
    CHECK(g_widget->desktop.focus == named(dialog, "Close"));
    CHECK(boneyards_status_widget_event(g_widget, escape));
    CHECK(!window_is_shown(dialog));
    CHECK(g_widget->desktop.focus == chat->matchmaking);
    {
        ScopedServices scope(g_widget);
        dialog->focus_return = nullptr;
        services_desktop_focus(chat->matchmaking);
        matchmaking_dialog_open(g_widget, k_matchmaking_settings);
    }
    click_window(named(dialog, "Close"));
    CHECK(!window_is_shown(dialog));
    CHECK(g_widget->desktop.focus == named(dialog, "Close"));
    {
        ScopedServices scope(g_widget);
        dialog->focus_return = nullptr;
        services_desktop_focus(chat->matchmaking);
        matchmaking_dialog_open(g_widget, k_matchmaking_settings);
    }
    g_log.calls.clear();
    g_log.sounds.clear();
    CHECK(bounds_are(named(dialog, "QueueMeter"), 162, 278, 104, 9));
    CHECK(bounds_are(named(dialog, "Scanner"), 403, 265, 72, 72));
    CHECK(named(dialog, "Duel")->x == 230 && named(dialog, "Duel")->y == 174);
    CHECK(
        named(dialog, "Title")->x == 150 && named(dialog, "Title")->y == 148 &&
        named(dialog, "Title")->width == 340
    );
    CHECK(
        named(dialog, "GameType")->x == 154 && named(dialog, "GameType")->y == 174 &&
        named(dialog, "GameType")->width == 70
    );
    CHECK(named(dialog, "ArmMark")->x == 241 && named(dialog, "ArmMark")->y == 214);
    CHECK(named(dialog, "Yes") != nullptr && named(dialog, "Yes")->visible == 0);
    CHECK(named(dialog, "OfferFace") != nullptr && named(dialog, "OfferFace")->visible == 0);

    const std::vector<uint8_t> settings = frame_pixels();
    check_pixel(settings, 150, 200, 90);
    check_pixel(settings, 489, 200, 90);
    check_pixel(settings, 200, 389, 90);
    check_pixel(settings, 151, 200, 42);
    check_pixel(settings, 152, 200, 93);
    check_pixel(settings, 200, 345, 42);
    check_pixel(settings, 158, 256, 93);
    check_pixel(settings, 482, 256, 93);
    check_pixel(settings, 398, 260, 93);
    CHECK(art_at(settings, "Widgets.gaf", "Radio_Button", 1, 230, 175, 0, false));
    CHECK(art_at(settings, "Widgets.gaf", "Radio_Button", 2, 382, 175, 0, false));
    CHECK(art_at(settings, "BY_New.gaf", "Queue_Meter", 0, 162, 278, 0, false));
    CHECK(art_at(settings, "BY_New.gaf", "Scanner", 0, 403, 265, 20, false));
    CHECK(art_at(settings, "Widgets.gaf", "Function_Button_Large", 2, 162, 358, 0, true));
    CHECK(art_at(settings, "Widgets.gaf", "Close_Button", 2, 472, 146, 0, false));

    g_log.sounds.clear();
    g_log.calls.clear();
    click_window(named(dialog, "For Fun"));
    click_window(named(dialog, "Any"));
    click_window(named(dialog, "Either"));
    click_window(named(dialog, "Any planet"));
    CHECK(named(dialog, "For Fun")->states.current == button_state::pressed);
    CHECK(named(dialog, "Duel")->states.current == button_state::unpressed);
    CHECK(g_log.calls.size() == 4);
    CHECK(g_log.calls[0] == "look type=0 skill=0 side=1 planets=0 area=2");
    CHECK(g_log.calls[1] == "look type=0 skill=1 side=1 planets=0 area=2");
    CHECK(g_log.calls[2] == "look type=0 skill=1 side=2 planets=0 area=2");
    CHECK(g_log.calls[3] == "look type=0 skill=1 side=2 planets=1 area=-");
    g_log.calls.clear();
    click_window(named(dialog, "Activate"));
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_searching);
    CHECK(matchmaking_dialog_queued(dialog));
    CHECK(named(dialog, "Activate")->visible == 0 && named(dialog, "Deactivate")->visible == 1);
    CHECK(text_of(named(dialog, "SearchLine")) == "SEARCHING  0:00");
    CHECK(
        text_of(named(dialog, "SearchDetail")) == "Looking for a For Fun at any skill, either side."
    );
    CHECK(text_of(named(dialog, "QueueLine")) == "Commanders queued on any planet");
    CHECK(heard_count("GREEN_BUTTON") == 1);
    CHECK(g_log.calls.size() == 1);
    CHECK(g_log.calls[0] == "join type=0 skill=1 side=2 planets=1 area=-");
    g_log.calls.clear();
    CHECK(!boneyards_status_widget_message_open(g_widget));
    const std::vector<uint8_t> searching = frame_pixels();
    CHECK(art_at(searching, "BY_New.gaf", "Scanner", 0, 403, 265, 20, false));

    tick(3);
    const std::vector<uint8_t> sweeping = frame_pixels();
    CHECK(art_at(sweeping, "BY_New.gaf", "Scanner", 1, 403, 265, 20, false));
    CHECK(!art_at(sweeping, "BY_New.gaf", "Scanner", 0, 403, 265, 20, false, false));
    CHECK(text_of(named(dialog, "SearchLine")) == "SEARCHING  0:00");
    tick(27);
    CHECK(text_of(named(dialog, "SearchLine")) == "SEARCHING  0:01");
    CHECK(g_log.calls.empty());
    push_status(0, 1, 4, 4);
    tick(1);
    CHECK(text_of(named(dialog, "QueueCount")) == "4");
    CHECK(text_of(named(dialog, "SearchLine")) == "SEARCHING  0:01");
    CHECK(art_at(frame_pixels(), "BY_New.gaf", "Queue_Meter", 4, 162, 278, 0, false));
    CHECK(matchmaking_dialog_queued(dialog));

    click_window(named(dialog, "Close"));
    CHECK(!window_is_shown(dialog) && matchmaking_dialog_queued(dialog));
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_searching);
    tick(15);
    CHECK(minimize_is(chat->matchmaking, 3));
    CHECK(!minimize_is(chat->matchmaking, 2));

    g_log.sounds.clear();
    g_log.calls.clear();
    click_window(chat->matchmaking);
    CHECK(
        window_is_shown(dialog) && matchmaking_dialog_searching(dialog) == k_matchmaking_settings
    );
    CHECK(matchmaking_dialog_queued(dialog));
    CHECK(minimize_is(chat->matchmaking, 2));
    CHECK(heard_count("GREEN_BUTTON") == 1 && g_log.calls.empty());
    {
        ScopedServices scope(g_widget);
        matchmaking_dialog_open(g_widget, k_matchmaking_searching);
    }
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_searching);
    CHECK(text_of(named(dialog, "SearchLine")) == "SEARCHING  0:01");
    CHECK(g_log.calls.size() == 1 && g_log.calls[0] == k_any_join);
    tick(15);
    CHECK(text_of(named(dialog, "SearchLine")) == "SEARCHING  0:02");
    CHECK(!boneyards_status_widget_message_open(g_widget));
    CHECK(g_log.calls.size() == 1);

    g_log.sounds.clear();
    g_log.calls.clear();
    click_window(named(dialog, "Deactivate"));
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_settings);
    CHECK(!matchmaking_dialog_queued(dialog));
    CHECK(heard_count("GREEN_BUTTON") == 1);
    CHECK(g_log.calls.size() == 1 && g_log.calls[0] == "cancel");
    tick(3);
    const std::vector<uint8_t> stopped = frame_pixels();
    CHECK(art_at(stopped, "BY_New.gaf", "Scanner", 0, 403, 265, 20, false));

    g_log.calls.clear();
    g_log.sounds.clear();
    click_window(button_of(frame, ConsoleButton::battle));
    finish_slide(frame);
    Window* battle_window = frame->active_screen;
    CHECK(battle_window != nullptr && window_is_named(battle_window, "BattleConsole"));
    auto* battle = reinterpret_cast<BattleConsole*>(battle_window);
    MatchControlPanel* panel = battle != nullptr ? battle->match_control : nullptr;
    CHECK(panel != nullptr);
    if (panel != nullptr) {
        click_window(panel->minimize);
        g_widget->navigation->matchmaking.face = match_face::active;
        g_log.calls.clear();
        g_log.sounds.clear();
        click_window(panel->activate);
        CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_searching);
        CHECK(matchmaking_dialog_queued(dialog) && window_is_shown(dialog));
        CHECK(heard_count("GREEN_BUTTON") == 1);
        CHECK(g_log.calls.size() == 1 && g_log.calls[0] == k_any_join);
        CHECK(!boneyards_status_widget_message_open(g_widget));
        g_log.calls.clear();
        g_log.sounds.clear();
        click_window(panel->settings);
        CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_settings);
        CHECK(matchmaking_dialog_queued(dialog));
        CHECK(heard_count("GREEN_BUTTON") == 1 && g_log.sounds.size() == 1 && g_log.calls.empty());
        CHECK(!boneyards_status_widget_message_open(g_widget));
        tick(40);
        CHECK(!boneyards_status_widget_message_open(g_widget) && g_log.calls.empty());
    }

    g_log.calls.clear();
    push_found("a1b2c3d4e5f607", "ArmSniper", 1, 1, 1, 0, 2);
    tick(1);
    CHECK(window_is_shown(dialog));
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_searching);
    CHECK(!boneyards_status_widget_message_open(g_widget));
    Window* yes = named(dialog, "Yes");
    Window* offer_text = named(dialog, "OfferText");
    CHECK(yes != nullptr && yes->visible == 1);
    CHECK(named(dialog, "No") != nullptr && named(dialog, "No")->visible == 1);
    CHECK(named(dialog, "OfferFace") != nullptr && named(dialog, "OfferFace")->visible == 1);
    if (offer_text != nullptr) {
        const std::string offer_line = text_of(offer_text);
        CHECK(offer_line.find("ArmSniper") != std::string::npos);
        CHECK(offer_line.find("Join the Duel?") != std::string::npos);
    } else {
        CHECK(false);
    }
    CHECK(g_log.calls.empty() && matchmaking_dialog_queued(dialog));
    if (Window* no = named(dialog, "No"))
        click_window(no);
    CHECK(yes != nullptr && yes->visible == 0 && matchmaking_dialog_queued(dialog));
    CHECK(g_log.calls.size() == 1);
    CHECK(g_log.calls[0] == "answer accept=0 id=a1b2c3d4e5f607");

    g_log.calls.clear();
    push_found("a1b2c3d4e5f607", "ArmSniper", 1, 1, 1, 0, 2);
    tick(1);
    yes = named(dialog, "Yes");
    CHECK(yes != nullptr && yes->visible == 1);
    if (yes != nullptr)
        click_window(yes);
    CHECK(yes != nullptr && yes->visible == 0 && matchmaking_dialog_queued(dialog));
    CHECK(g_log.calls.size() == 1);
    CHECK(g_log.calls[0] == "answer accept=1 id=a1b2c3d4e5f607");

    g_log.calls.clear();
    push_go_host();
    tick(1);
    CHECK(matchmaking_dialog_queued(dialog));
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_searching);
    CHECK(g_log.calls.size() == 1);
    CHECK(
        g_log.calls[0] == "host area=2 mode=1 name=ArmSniper password=- skill=1 players=0 death=2"
    );

    g_log.calls.clear();
    push_go_join();
    tick(1);
    CHECK(matchmaking_dialog_queued(dialog));
    CHECK(g_log.calls.size() == 1);
    CHECK(g_log.calls[0] == "join area=2 game=c0ffee0a11b223 password=-");

    g_log.calls.clear();
    click_window(named(dialog, "Deactivate"));
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_settings);
    CHECK(!matchmaking_dialog_queued(dialog));
    CHECK(g_log.calls.size() == 1 && g_log.calls[0] == "cancel");

    auto expect_ended = [&](int32_t result, const char* sentence) {
        g_log.calls.clear();
        click_window(named(dialog, "Activate"));
        CHECK(matchmaking_dialog_queued(dialog));
        CHECK(g_log.calls.size() == 1 && g_log.calls[0] == k_any_join);
        push_status(result, 0, 0, 0);
        tick(1);
        CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_settings);
        CHECK(!matchmaking_dialog_queued(dialog));
        CHECK(box_text().find(sentence) != std::string::npos);
        CHECK(g_log.calls.size() == 1);
        dismiss_ok();
        CHECK(!boneyards_status_widget_message_open(g_widget));
    };
    expect_ended(1, "Matchmaking is not open for this account.");
    expect_ended(2, "That battle is not allowed on this planet.");
    expect_ended(4, "You are already in a battle.");
    expect_ended(6, "Fuel Dump is not in this sector.");
    expect_ended(7, "The queue dropped this search.");
    expect_ended(8, "Those settings cannot be queued.");

    g_log.calls.clear();
    click_window(named(dialog, "Activate"));
    push_status(5, 1, 3, 3);
    tick(1);
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_searching);
    CHECK(matchmaking_dialog_queued(dialog));
    CHECK(box_text().find("That battle has filled.") != std::string::npos);
    CHECK(g_log.calls.size() == 1 && g_log.calls[0] == k_any_join);
    dismiss_ok();
    g_log.calls.clear();
    click_window(named(dialog, "Deactivate"));
    CHECK(!matchmaking_dialog_queued(dialog));
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_settings);
    CHECK(g_log.calls.size() == 1 && g_log.calls[0] == "cancel");

    g_log.calls.clear();
    click_window(named(dialog, "Activate"));
    CHECK(g_log.calls.size() == 1 && g_log.calls[0] == k_any_join);
    tick(149);
    CHECK(!boneyards_status_widget_message_open(g_widget) && matchmaking_dialog_queued(dialog));
    tick(1);
    CHECK(!matchmaking_dialog_queued(dialog));
    CHECK(matchmaking_dialog_searching(dialog) == k_matchmaking_settings);
    CHECK(box_text().find("The queue is unavailable.") != std::string::npos);
    CHECK(g_log.calls.size() == 1 && g_log.calls[0] == k_any_join);
    dismiss_ok();
    // The timeout already left the queue and did not send cancel. Settings shows
    // Activate in Deactivate's place, and that pulse already counted toward the next look.
    g_log.calls.clear();
    tick(448);
    CHECK(g_log.calls.empty());
    tick(1);
    CHECK(g_log.calls.size() == 1 && g_log.calls[0] == k_any_look);

    const int32_t sent = matchmaking_dialog_requests(dialog);
    g_look_result = ServiceResult::not_connected;
    g_log.calls.clear();
    click_window(named(dialog, "This planet"));
    CHECK(g_log.calls.size() == 1);
    CHECK(g_log.calls[0] == "look type=0 skill=1 side=2 planets=0 area=2");
    CHECK(matchmaking_dialog_requests(dialog) == sent);
    g_look_result = ServiceResult::pending;

    g_log.calls.clear();
    click_window(named(dialog, "Close"));
    CHECK(!window_is_shown(dialog) && !matchmaking_dialog_queued(dialog));
    tick(450);
    CHECK(g_log.calls.empty());

    GuiLoadError error{};
    GuiScreen* screen = nullptr;
    {
        ScopedServices scope(g_widget);
        screen = gui_screen_load("gui/matchmaking.gui", &error);
    }
    CHECK(screen != nullptr);
    if (screen == nullptr)
        std::fprintf(stderr, "matchmaking.gui: %s\n", error.message);
    expect_control(screen, "Matchmaking", 150, 140, 340, 250);
    expect_control(screen, "Fill", 150, 140, 340, 250);
    expect_control(screen, "Activate", 162, 358, 99, 21);
    expect_control(screen, "Deactivate", 162, 358, 99, 21);
    expect_control(screen, "Close", 472, 146, 13, 12);
    expect_control(screen, "QueueMeter", 162, 278, 104, 9);
    expect_control(screen, "Scanner", 403, 265, 72, 72);
    expect_control(screen, "Duel", 230, 174, 48, 14);
    expect_control(screen, "For Fun", 382, 174, 90, 14);
    expect_control(screen, "ArmMark", 241, 214, 12, 12);
    expect_control(screen, "CoreMark", 291, 214, 12, 12);
    expect_control(screen, "QueueCount", 272, 278, 16, 12);
    expect_control(screen, "OfferFace", 142, 172, 216, 136);
    expect_control(screen, "Yes", 172, 278, 57, 21);
    expect_control(screen, "No", 272, 278, 57, 21);
    {
        ScopedServices scope(g_widget);
        gui_screen_destroy(screen);
    }
    rover_session_set_virtual_files(nullptr, nullptr);

    boneyards_status_widget_destroy(g_widget);
    g_widget = nullptr;
    check_replay();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("matchmaking tests passed\n");
    return 0;
}
