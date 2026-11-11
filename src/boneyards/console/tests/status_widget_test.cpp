// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Main-menu overlay behaviour and snapshots against the BY1117a by.ccx data.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "console_layer.hpp"
#include "console_state.hpp"
#include "galaxy_fixture.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/platform/system.hpp"

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

// ---------------------------------------------------------------------------
// Installed data

struct GameData {
    oa::AssetStore store{oa::boneyards_test::by_install()};
    std::vector<uint8_t> shade;
    std::vector<uint8_t> light;
};

bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<GameData*>(user)->store.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Recording host

struct HostLog {
    std::vector<std::string> sounds;
    // Every sound call in order: "stop", "alt <name>" and the names of the
    // ordinary sounds.
    std::vector<std::string> audio;
    std::vector<std::string> messages;
    int32_t pending_signal = -1;
    int32_t signal = -1;
    int32_t state = -1;
    int32_t gadget_updates = 0;
    bool document_present = false;
    bool network = false;
    ActivationResult activation = ActivationResult::module_missing;
    int32_t activations = 0;
};

HostLog g_log;

void host_sound(void*, const char* name) {
    g_log.sounds.emplace_back(name);
    g_log.audio.emplace_back(name);
}

void host_stop_sounds(void*) {
    g_log.audio.emplace_back("stop");
}

void host_sound_alternate(void*, const char* name) {
    g_log.audio.push_back(std::string("alt ") + name);
}

bool host_document(void*) {
    return g_log.document_present;
}

void host_pending(void*, uint8_t signal) {
    g_log.pending_signal = signal;
}

void host_signal(void*, uint8_t signal) {
    g_log.signal = signal;
}

void host_state(void*, uint8_t state) {
    g_log.state = state;
}

void host_gadgets(void*) {
    ++g_log.gadget_updates;
}

void host_message(void*, const char* text) {
    g_log.messages.emplace_back(text);
}

bool g_press_taken_below = false;

bool host_press_taken_below(void*) {
    return g_press_taken_below;
}

bool service_network(void*) {
    return g_log.network;
}

ActivationResult service_activate(void*) {
    ++g_log.activations;
    return g_log.activation;
}

ServiceEvents g_events{};

void service_set_events(void*, const ServiceEvents* events) {
    g_events = events != nullptr ? *events : ServiceEvents{};
}

std::vector<uint32_t> g_census_requests;

void service_request_census(void*, uint32_t area_id) {
    g_census_requests.push_back(area_id);
}

StatusWidgetConfig make_config(GameData* data, bool fake_service) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.shade = ShadeTables{data->shade.data(), data->light.data()};
    MainMenuHost& host = config.host;
    host.play_sound = host_sound;
    host.load_multiplayer_document = host_document;
    host.set_pending_signal = host_pending;
    host.set_signal = host_signal;
    host.set_frontend_state = host_state;
    host.update_gadgets = host_gadgets;
    host.show_message_box = host_message;
    host.stop_sounds = host_stop_sounds;
    host.play_sound_alternate = host_sound_alternate;
    if (fake_service) {
        config.service.network_available = service_network;
        config.service.activate = service_activate;
        config.service.set_events = service_set_events;
    }
    return config;
}

// ---------------------------------------------------------------------------
// Helpers

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void click(StatusWidget* widget, int32_t x, int32_t y) {
    boneyards_status_widget_event(widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, x, y, 0));
}

void click_part(StatusWidget* widget, OverlayPart part) {
    const Window* w = boneyards_status_widget_part(widget, part);
    click(widget, w->x + w->width / 2, w->y + w->height / 2);
}

struct Frame {
    std::vector<uint8_t> pixels = std::vector<uint8_t>(640 * 480, 0);
    oa::Surface surface{};

    Frame() {
        surface.width = 640;
        surface.height = 480;
        surface.pitch = 640;
        surface.pixels = pixels.data();
        surface.clip = oa::Rect32{0, 0, 639, 479};
    }
};

void check_hash(uint64_t actual, uint64_t expected, const char* what) {
    if (actual != expected)
        std::fprintf(stderr, "%s: got %016llx\n", what, static_cast<unsigned long long>(actual));
    check(actual == expected, what);
}

uint64_t fnv1a(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (uint8_t b : bytes) {
        hash ^= b;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

void write_snapshot(const char* name, const Frame& frame) {
    const auto dir_value = oa::platform::environment_value("OA_SNAPSHOT_DIR");
    const char* dir = dir_value ? dir_value->c_str() : nullptr;
    if (dir == nullptr)
        return;
    const std::filesystem::path path = std::filesystem::path(dir) / name;
    std::ofstream out(path, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(frame.pixels.data()),
        static_cast<std::streamsize>(frame.pixels.size())
    );
}

uint64_t snapshot(StatusWidget* widget, const char* name) {
    Frame frame;
    boneyards_status_widget_draw(widget, &frame.surface);
    write_snapshot(name, frame);
    return fnv1a(frame.pixels);
}

void reset_log() {
    const bool network = g_log.network;
    const ActivationResult activation = g_log.activation;
    g_log = HostLog{};
    g_log.network = network;
    g_log.activation = activation;
}

// ---------------------------------------------------------------------------
// Tests

void test_layout(StatusWidget* widget) {
    check(boneyards_status_widget_visible(widget), "overlay visible after create");
    const Window* panel = boneyards_status_widget_part(widget, OverlayPart::panel);
    check(
        panel->x == 0 && panel->y == 378 && panel->width == 640 && panel->height == 102,
        "panel bounds"
    );
    check(std::strcmp(panel->name, "MainMenuOverlay") == 0, "panel name");
    // The strip still holds its six buttons and the activity light. Four
    // more children are invisible hotspots on the part of each old menu
    // button the strip's own columns do not cover. They belong on the strip:
    // those rectangles sit inside the panel, and a click there is that button.
    const auto* overlay = reinterpret_cast<const MainMenuOverlay*>(panel);
    int32_t former_children = 0;
    for (const Window* former :
         {overlay->single_former,
          overlay->multi_former,
          overlay->intro_former,
          overlay->exit_former}) {
        if (former != nullptr && former->parent == panel)
            ++former_children;
    }
    check(former_children == 4, "four old-column hotspots on the strip");
    check(
        panel->children.count == 7 + former_children,
        "six buttons plus the light, and the old-column hotspots"
    );
    const Window* single = boneyards_status_widget_part(widget, OverlayPart::single);
    check(
        single->x == 82 && single->y == 393 && single->width == 96 && single->height == 20,
        "single bounds"
    );
    const Window* exit = boneyards_status_widget_part(widget, OverlayPart::exit);
    check(exit->x == 464 && exit->y == 430, "exit bounds");
    const Window* enter = boneyards_status_widget_part(widget, OverlayPart::enter);
    check(
        enter->x == 260 && enter->y == 412 && enter->width == 45 && enter->height == 28,
        "enter bounds"
    );
    for (auto part :
         {OverlayPart::single,
          OverlayPart::multi,
          OverlayPart::intro,
          OverlayPart::exit,
          OverlayPart::credits,
          OverlayPart::enter}) {
        const Window* w = boneyards_status_widget_part(widget, part);
        check(
            w->states.count == 3 && w->states.current == button_state::unpressed,
            "button starts normal"
        );
    }
    const Window* credits = boneyards_status_widget_part(widget, OverlayPart::credits);
    check(
        credits->states.images[0] < 0 && credits->states.images[2] < 0,
        "credits hotspot has no images"
    );
    const Window* light = boneyards_status_widget_part(widget, OverlayPart::activity_light);
    check(light->children.count == 8, "eight lamps");
    for (int32_t i = 0; i < light->children.count; ++i) {
        const Window* lamp = light->children.items[i];
        check(lamp->x == 307 + 8 * i && lamp->width == 8 && lamp->height == 14, "lamp bounds");
        check(lamp->states.current == 0, "lamp dark");
    }
    // The enter button's disabled frame is a darkened copy of the normal frame.
    ConsoleImages* images = boneyards_status_widget_images(widget);
    const oa::Sprite* normal = console_images_sprite(images, enter->states.images[2]);
    const oa::Sprite* disabled = console_images_sprite(images, enter->states.images[0]);
    check(normal != nullptr && disabled != nullptr, "enter images present");
    check(
        disabled->width == 45 && disabled->height == 28,
        "disabled frame replaces the 1x1 placeholder"
    );
    check(std::memcmp(normal->data, disabled->data, 45 * 28) != 0, "disabled frame is shaded");
}

void test_snapshot(StatusWidget* widget) {
    Frame frame;
    boneyards_status_widget_draw(widget, &frame.surface);
    write_snapshot("overlay.raw", frame);
    // Transparent rows above the panel stay untouched.
    bool untouched = true;
    for (int32_t i = 0; i < 378 * 640; ++i)
        untouched = untouched && frame.pixels[static_cast<std::size_t>(i)] == 0;
    check(untouched, "nothing drawn above the panel");
    check_hash(fnv1a(frame.pixels), 0xa0b4c1c3a4b62e1eULL, "overlay snapshot");
}

void test_single_click(StatusWidget* widget) {
    reset_log();
    Window* single = boneyards_status_widget_part(widget, OverlayPart::single);
    boneyards_status_widget_event(widget, pointer(MessageCode::left_down, 100, 400, button_left));
    check(single->states.current == button_state::pressed, "single pressed on down");
    check(g_log.pending_signal == -1, "no action on press");
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, 100, 400, 0));
    check(single->states.current == button_state::unpressed, "single released");
    check(g_log.pending_signal == frontend_value::signal_single_player, "single sets signal 5");
    check(
        g_log.sounds.size() == 1 && g_log.sounds[0] == menu_sound::big_button,
        "single plays BigButton"
    );
    check(g_log.gadget_updates == 1, "single updates the gadgets");
}

void test_drag_cancel(StatusWidget* widget) {
    reset_log();
    Window* multi = boneyards_status_widget_part(widget, OverlayPart::multi);
    boneyards_status_widget_event(widget, pointer(MessageCode::left_down, 100, 440, button_left));
    check(multi->states.current == button_state::pressed, "multi pressed");
    boneyards_status_widget_event(widget, pointer(MessageCode::mouse_move, 300, 200, button_left));
    check(multi->states.current == button_state::unpressed, "drag off shows normal");
    boneyards_status_widget_event(widget, pointer(MessageCode::mouse_move, 101, 441, button_left));
    check(multi->states.current == button_state::pressed, "drag back shows pressed");
    boneyards_status_widget_event(widget, pointer(MessageCode::mouse_move, 300, 200, button_left));
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, 300, 200, 0));
    check(g_log.pending_signal == -1 && g_log.messages.empty(), "release outside does nothing");
}

void test_panel_passthrough(StatusWidget* widget) {
    check(
        !boneyards_status_widget_event(
            widget, pointer(MessageCode::left_down, 20, 470, button_left)
        ),
        "click on bare panel is not consumed"
    );
    check(
        !boneyards_status_widget_event(
            widget, pointer(MessageCode::left_down, 320, 100, button_left)
        ),
        "click above the panel is not consumed"
    );
    Message key{};
    key.code = MessageCode::key_down;
    key.key = 'S';
    check(!boneyards_status_widget_event(widget, key), "TA quick keys pass through");
}

void test_enter_offline(StatusWidget* widget) {
    reset_log();
    const uint64_t before = snapshot(widget, "before_message.raw");
    click_part(widget, OverlayPart::enter);
    check(
        g_log.messages.size() == 1 && g_log.messages[0] == menu_message::internet_required,
        "offline enter asks for an internet connection"
    );
    for (auto part :
         {OverlayPart::single,
          OverlayPart::multi,
          OverlayPart::intro,
          OverlayPart::exit,
          OverlayPart::credits,
          OverlayPart::enter}) {
        check(
            boneyards_status_widget_part(widget, part)->states.current == button_state::disabled,
            "message box disables overlay buttons"
        );
    }
    const uint64_t dimmed = snapshot(widget, "message_open.raw");
    check(dimmed != before, "overlay dimmed while the message is open");
    check_hash(dimmed, 0x6f755889ae3a74a5ULL, "dimmed overlay snapshot");
    reset_log();
    click_part(widget, OverlayPart::single);
    check(g_log.pending_signal == -1, "disabled buttons do nothing");
    boneyards_status_widget_dialog_closed(widget);
    for (auto part : {OverlayPart::single, OverlayPart::enter}) {
        check(
            boneyards_status_widget_part(widget, part)->states.current == button_state::unpressed,
            "closing the message re-enables buttons"
        );
    }
    // Dimming and lightening go through lossy palette tables.
    check_hash(
        snapshot(widget, "message_closed.raw"), 0x08947ecb4198b92bULL, "restored overlay snapshot"
    );
}

void test_b_key(StatusWidget* widget) {
    reset_log();
    Message key{};
    key.code = MessageCode::key_down;
    key.key = 'b';
    check(boneyards_status_widget_event(widget, key), "B is consumed");
    check(
        g_log.messages.size() == 1 && g_log.messages[0] == menu_message::internet_required,
        "B acts as the enter button"
    );
    boneyards_status_widget_dialog_closed(widget);
}

void test_multi(StatusWidget* widget) {
    reset_log();
    click_part(widget, OverlayPart::multi);
    check(
        g_log.messages.size() == 1 && g_log.messages[0] == menu_message::multiplayer_disc_required,
        "missing multiplay.tdf asks for the multiplayer disc"
    );
    check(g_log.pending_signal == -1, "no multiplayer signal without the document");
    boneyards_status_widget_dialog_closed(widget);
    reset_log();
    g_log.document_present = true;
    click_part(widget, OverlayPart::multi);
    check(g_log.pending_signal == frontend_value::signal_multiplayer, "multi sets signal 6");
    check(g_log.messages.empty(), "no message with the document");
}

void test_intro_and_credits(StatusWidget* widget) {
    reset_log();
    click_part(widget, OverlayPart::intro);
    check(g_log.messages.empty(), "intro does not ask for a disc or full screen");
    check(g_log.sounds.size() == 1 && g_log.sounds[0] == menu_sound::small_button, "intro sound");
    check(g_log.state == frontend_value::state_intro, "intro starts the movie state");
    reset_log();
    click_part(widget, OverlayPart::credits);
    check(g_log.messages.empty(), "credits does not ask for a disc or full screen");
    check(g_log.pending_signal == frontend_value::signal_credits, "credits hotspot sets signal 9");
    reset_log();
    click_part(widget, OverlayPart::exit);
    check(g_log.pending_signal == frontend_value::signal_exit_application, "exit sets signal 8");
    check(g_log.sounds.size() == 1 && g_log.sounds[0] == menu_sound::exit, "exit sound");
}

void test_activity_light(StatusWidget* widget) {
    MainMenuOverlay* overlay = widget->system.main_menu;
    widget->system.connection_level = 3;
    {
        ScopedServices scope(widget);
        main_menu_overlay_start_activity(overlay);
    }
    const Window* light = boneyards_status_widget_part(widget, OverlayPart::activity_light);
    const Window* enter = boneyards_status_widget_part(widget, OverlayPart::enter);
    check(enter->states.current == button_state::disabled, "enter disabled while busy");
    check(light->children.items[0]->states.current == 1, "first lamp lit");
    boneyards_status_widget_tick(widget);
    check(light->children.items[0]->states.current == 1, "lamp holds for one cycle");
    boneyards_status_widget_tick(widget);
    check(
        light->children.items[0]->states.current == 0 &&
            light->children.items[1]->states.current == 1,
        "lamp advances every second cycle"
    );
    for (int32_t i = 0; i < 14; ++i)
        boneyards_status_widget_tick(widget);
    check(light->children.items[0]->states.current == 1, "lamps wrap after eight steps");
    widget->system.connection_level = 0;
    boneyards_status_widget_tick(widget);
    check(enter->states.current == button_state::unpressed, "enter re-enabled when offline");
    for (int32_t i = 0; i < light->children.count; ++i)
        check(light->children.items[i]->states.current == 0, "lamps dark when stopped");
}

void test_enter_online(GameData* data) {
    reset_log();
    g_log.network = true;
    g_log.activation = ActivationResult::attach_failed;
    StatusWidget* widget = boneyards_status_widget_create(make_config(data, true));
    click_part(widget, OverlayPart::enter);
    check(
        g_log.messages.size() == 1 && g_log.messages[0] == menu_message::rover_attach_failed,
        "attach failure message"
    );
    check(
        g_log.state == frontend_value::state_main_menu && g_log.signal == 1,
        "stays on the main menu"
    );
    boneyards_status_widget_dialog_closed(widget);

    reset_log();
    g_log.activation = ActivationResult::ok;
    click_part(widget, OverlayPart::enter);
    check(g_log.messages.empty(), "no message when the client starts");
    check(
        g_log.state == frontend_value::state_boneyards && g_log.gadget_updates == 1,
        "enters the Boneyards frontend state"
    );
    check(!boneyards_status_widget_visible(widget), "overlay released");
    check(widget->system.connection_level == 1, "client marked active");
    boneyards_status_widget_tick(widget);
    check(widget->desktop.windows.count == 1, "released overlay destroyed on the next cycle");
    check(boneyards_status_widget_in_boneyards(widget), "Main Frame replaces the overlay");

    // The Main Frame's activity light, which LOGIN starts, stops when the
    // client resets the login (reset_login) and when the dossier arrives
    // (on_dossier).
    {
        ActivityLight* light = widget->system.main_frame->activity_light;
        ScopedServices scope(widget);
        activity_light_start(light);
        g_events.login_reset(g_events.user);
        check(light->running == 0, "a login reset stops the activity light");
        activity_light_start(light);
        SessionRecord session{};
        session.level = SessionLevel::logged_in;
        g_events.session_changed(g_events.user, &session);
        check(light->running == 1, "the light runs until the dossier");
        session.level = SessionLevel::dossier_loaded;
        g_events.session_changed(g_events.user, &session);
        check(light->running == 0 && widget->system.connection_level == 4, "the dossier stops it");
    }
    boneyards_status_widget_destroy(widget);
    g_log.network = false;
    g_log.activation = ActivationResult::module_missing;
}

bool same_census(const CensusReport& a, const CensusReport& b) {
    return a.area_id == b.area_id && a.population == b.population && a.activity == b.activity &&
           a.in_progress == b.in_progress && a.forming == b.forming && a.core_wins == b.core_wins &&
           a.arm_wins == b.arm_wins;
}

// What the Main Frame probe reads on the two-planet war map: the console
// button bounds the Main Frame build gives them (main_frame_build), the centre of
// a planet's hotspot once the map shows it, the area and census request of
// a hotspot press (hotspot_left_down), the census reply for that planet (another
// planet's leaves it), and the keep-alive (main_frame_update) as a second request.
void test_main_frame_probe(GameData* data) {
    reset_log();
    g_log.network = true;
    g_log.activation = ActivationResult::ok;
    g_census_requests.clear();
    StatusWidgetConfig config = make_config(data, true);
    config.service.request_census = service_request_census;
    StatusWidget* widget = boneyards_status_widget_create(config);
    MainFrameProbe probe{};
    int32_t x = 0;
    int32_t y = 0;
    check(!boneyards_status_widget_probe(widget, &probe), "no probe on the main menu");
    check(
        !boneyards_status_widget_planet_hotspot(widget, test::k_empyrrean_id, &x, &y),
        "no hotspot on the main menu"
    );
    click_part(widget, OverlayPart::enter);
    boneyards_status_widget_tick(widget);
    MainFrame* frame = widget->system.main_frame;
    check(boneyards_status_widget_probe(widget, &probe), "probe of the Main Frame");
    const auto bounds = [&](ConsoleButton which, int32_t bx, int32_t by, int32_t bw, int32_t bh) {
        const ScreenRect& r = probe.console_buttons[static_cast<int32_t>(which)];
        return r.x == bx && r.y == by && r.width == bw && r.height == bh;
    };
    check(bounds(ConsoleButton::release, 8, 404, 15, 43), "Release bounds");
    check(bounds(ConsoleButton::recon, 525, 188, 98, 20), "Recon bounds");
    check(bounds(ConsoleButton::battle, 525, 236, 98, 20), "Battle bounds");
    check(bounds(ConsoleButton::help, 538, 414, 86, 20), "Help bounds");
    check(
        probe.selected_area_id == 0 && probe.selected_census.area_id == 0 &&
            probe.census_requests == 0,
        "no area, census or request before a planet is picked"
    );
    check(
        !boneyards_status_widget_planet_hotspot(widget, test::k_empyrrean_id, &x, &y),
        "no hotspot before the war map loads"
    );

    screens::BoneyardsScreens* nav = widget->navigation;
    check(test::load_two_planet_galaxy(&nav->galaxy), "war map loaded");
    screens::galaxy_apply_allegiances(&nav->galaxy, "CA");
    {
        ScopedServices scope(widget);
        main_frame_unlock_map(frame);
        galaxy_panel_sync_hotspots(frame);
    }
    const Window& panel = frame->map_panel->base;
    const int32_t centre = test::k_empyrrean_view_left + test::k_empyrrean_view_size / 2;
    check(
        boneyards_status_widget_planet_hotspot(widget, test::k_empyrrean_id, &x, &y) &&
            x == panel.x + centre && y == panel.y + centre,
        "Empyrrean's hotspot centre"
    );
    check(
        !boneyards_status_widget_planet_hotspot(widget, 3, &x, &y), "no hotspot for an unknown id"
    );

    boneyards_status_widget_event(widget, pointer(MessageCode::mouse_move, x, y, 0));
    click(widget, x, y);
    boneyards_status_widget_probe(widget, &probe);
    check(
        probe.selected_area_id == static_cast<uint32_t>(test::k_empyrrean_id) &&
            probe.census_requests == 1 &&
            g_census_requests == std::vector<uint32_t>{static_cast<uint32_t>(test::k_empyrrean_id)},
        "the press makes Empyrrean the current area and asks for its census"
    );
    check(
        probe.activity_light_running && probe.recon_button == button_state::unpressed,
        "the press runs the light and enables Recon"
    );
    check(
        same_census(probe.selected_census, CensusReport{2, 0, 0, 0, 0, 0, 0}),
        "no census fields before the reply"
    );
    const CensusReport reply{2, 34, 3, 2, 1, 17, 9};
    g_events.census(g_events.user, &reply);
    boneyards_status_widget_probe(widget, &probe);
    check(same_census(probe.selected_census, reply), "the selected planet's census is its reply");
    check(!probe.activity_light_running, "the census reply stops the light");
    const CensusReport other{1, 5, 1, 0, 0, 0, 0};
    g_events.census(g_events.user, &other);
    boneyards_status_widget_probe(widget, &probe);
    check(same_census(probe.selected_census, reply), "another planet's census leaves it");

    frame->keepalive_countdown = 0;
    boneyards_status_widget_tick(widget);
    boneyards_status_widget_probe(widget, &probe);
    check(
        probe.census_requests == 2 && g_census_requests.size() == 2 && g_census_requests[1] == 2,
        "the keep-alive counts as a census request for the current area"
    );
    boneyards_status_widget_destroy(widget);
    g_log.network = false;
    g_log.activation = ActivationResult::module_missing;
}

// While the TA gadget under a menu button takes the press, the overlay only
// shows it: no sound, signal, gadget update or message of its own. Enter
// Boneyards has no gadget under it and still acts.
void test_press_taken_below(GameData* data) {
    reset_log();
    StatusWidgetConfig config = make_config(data, false);
    config.host.press_taken_below = host_press_taken_below;
    StatusWidget* widget = boneyards_status_widget_create(config);
    g_press_taken_below = true;
    for (auto part :
         {OverlayPart::single,
          OverlayPart::multi,
          OverlayPart::intro,
          OverlayPart::exit,
          OverlayPart::credits}) {
        click_part(widget, part);
        check(
            boneyards_status_widget_part(widget, part)->states.current == button_state::unpressed,
            "a press taken below leaves the button up"
        );
    }
    check(g_log.sounds.empty() && g_log.messages.empty(), "no sound or message of the overlay's");
    check(
        g_log.pending_signal == -1 && g_log.state == -1 && g_log.gadget_updates == 0,
        "no signal, state or gadget update of the overlay's"
    );
    click_part(widget, OverlayPart::enter);
    check(
        g_log.messages.size() == 1 && g_log.messages[0] == menu_message::internet_required,
        "Enter Boneyards still acts"
    );
    g_press_taken_below = false;
    boneyards_status_widget_destroy(widget);
}

// Without a TA box the overlay's message is a Boneyards box. Its Ok releases
// the dialog through the system object, as TA's box does (message_box_click): the
// overlay shown at the time, not the one that opened the box, which Enter
// Boneyards may have released while the box was up.
void test_box_release_through_system(GameData* data) {
    reset_log();
    StatusWidgetConfig config = make_config(data, true);
    config.host.show_message_box = nullptr;
    StatusWidget* widget = boneyards_status_widget_create(config);
    const auto press_ok = [&] {
        MessageBox* box = message_box_find_open(&widget->desktop);
        check(box != nullptr, "offline enter opens a Boneyards box");
        if (box == nullptr)
            return;
        const Window* ok = window_find_named(&box->base, message_box_name::ok, class_name::button);
        click(widget, ok->x + ok->width / 2, ok->y + ok->height / 2);
        check(!boneyards_status_widget_message_open(widget), "Ok closes the box");
    };
    click_part(widget, OverlayPart::enter);
    press_ok();
    for (auto part : {OverlayPart::single, OverlayPart::enter}) {
        check(
            boneyards_status_widget_part(widget, part)->states.current == button_state::unpressed,
            "Ok re-enables the shown overlay"
        );
    }
    boneyards_status_widget_tick(widget);

    click_part(widget, OverlayPart::enter);
    MainMenuOverlay* opener = widget->system.main_menu;
    console_release_main_menu(&widget->system);
    press_ok();
    check(
        opener->single_button->states.current == button_state::disabled,
        "Ok leaves the released overlay alone"
    );
    boneyards_status_widget_destroy(widget);
}

struct Pointer {
    bool known = false;
    int32_t x = 0;
    int32_t y = 0;
};

Pointer g_pointer;

bool host_pointer(void*, int32_t* x, int32_t* y) {
    *x = g_pointer.x;
    *y = g_pointer.y;
    return g_pointer.known;
}

// Pixels that differ between two draws, and whether all of them lie in the
// 64-pixel square below and right of the pointer.
int32_t cursor_pixels(StatusWidget* widget, bool* near_pointer) {
    Frame without;
    g_pointer.known = false;
    boneyards_status_widget_draw(widget, &without.surface);
    Frame with;
    g_pointer.known = true;
    boneyards_status_widget_draw(widget, &with.surface);
    int32_t changed = 0;
    *near_pointer = true;
    for (int32_t i = 0; i < 640 * 480; ++i) {
        if (without.pixels[i] == with.pixels[i])
            continue;
        ++changed;
        const int32_t x = i % 640;
        const int32_t y = i / 640;
        if (x < g_pointer.x - 8 || x > g_pointer.x + 64 || y < g_pointer.y - 8 ||
            y > g_pointer.y + 64)
            *near_pointer = false;
    }
    return changed;
}

// console_init_gui loads anims\cursors.gaf's cursornormal (one frame) as the default and shown
// cursor, toolkit_init shows it, boneyards_status_widget_draw draws it at the pointer,
// main_menu_overlay_show_message hides it for the message box and main_menu_overlay_release_dialog
// shows it again.
void test_cursor(GameData* data, const oa::Palette* palette) {
    reset_log();
    StatusWidgetConfig config = make_config(data, false);
    config.palette = palette;
    config.host.pointer_position = host_pointer;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr && widget->toolkit != nullptr, "widget with a toolkit");
    if (widget == nullptr || widget->toolkit == nullptr)
        return;
    check(toolkit_cursor(widget->toolkit) == 0, "cursornormal is the shown cursor");
    g_pointer.x = 300;
    g_pointer.y = 200;
    bool near_pointer = false;
    check(cursor_pixels(widget, &near_pointer) > 0 && near_pointer, "cursor drawn at the pointer");
    g_pointer.y = 420;
    check(cursor_pixels(widget, &near_pointer) == 0, "the overlay panel covers the cursor");
    g_pointer.y = 200;
    boneyards_status_widget_tick(widget);
    check(cursor_pixels(widget, &near_pointer) > 0 && near_pointer, "one frame: the cursor stays");
    boneyards_status_widget_show_message(widget, "message");
    check(cursor_pixels(widget, &near_pointer) == 0, "message box hides the cursor");
    boneyards_status_widget_dialog_closed(widget);
    check(cursor_pixels(widget, &near_pointer) > 0, "closing the message shows it again");
    boneyards_status_widget_destroy(widget);
    g_pointer = Pointer{};
}

// ---------------------------------------------------------------------------
// A battle the Boneyards launches

struct LaunchLog {
    int32_t launches = 0;
    int32_t launches_ended = 0; // MainMenuHost::launch_ended
    std::string launch_user;
    std::string launch_session;
    int32_t frontend_passes = 0;
    int32_t state_at_pass = -1;
    int32_t signal_at_pass = -1;
    int32_t leaves = 0;
    std::vector<std::pair<uint32_t, std::string>> left_games;
    std::vector<uint32_t> latency_flags;
    int32_t chat_ring_emptied = 0;
    std::vector<std::string> battle_messages;
    std::vector<std::pair<uint32_t, uint32_t>> joined_rooms;
    std::vector<uint32_t> left_battles;
    std::vector<uint32_t> entered_battles;
};

LaunchLog g_launch;

bool refuse_launch(void*, const char** text) {
    if (text != nullptr)
        *text = "The Boneyards launches and reports games only on 3.1c, Core Contingency and "
                "Battle Tactics.";
    return false;
}

void host_battle_launched(void*, const LaunchReport* launch, const char* user_name) {
    ++g_launch.launches;
    g_launch.launch_user = user_name != nullptr ? user_name : "";
    g_launch.launch_session = launch->session != nullptr ? launch->session : "";
}

void host_launch_ended(void*) {
    ++g_launch.launches_ended;
}

void host_run_frontend(void*) {
    ++g_launch.frontend_passes;
    g_launch.state_at_pass = g_log.state;
    g_launch.signal_at_pass = g_log.signal;
}

void host_leave_game(void*) {
    ++g_launch.leaves;
}

void host_left_game(void*, uint32_t area_id, const char* game_id) {
    g_launch.left_games.emplace_back(area_id, game_id);
}

void host_latency_flags(void*, uint32_t flags) {
    g_launch.latency_flags.push_back(flags);
}

void host_empty_chat_ring(void*) {
    ++g_launch.chat_ring_emptied;
}

void host_battle_message(void*, const char* text) {
    g_launch.battle_messages.emplace_back(text);
}

ServiceResult
launch_service_join_room(void*, uint32_t area_id, uint32_t room_id, const char*, const char*) {
    g_launch.joined_rooms.emplace_back(area_id, room_id);
    return ServiceResult::pending;
}

ServiceResult launch_service_leave_battles(void*, uint32_t area_id) {
    g_launch.left_battles.push_back(area_id);
    return ServiceResult::pending;
}

ServiceResult launch_service_enter_battles(void*, uint32_t area_id) {
    g_launch.entered_battles.push_back(area_id);
    return ServiceResult::pending;
}

// The text of the open Boneyards message box; "" without one.
std::string open_message_text(StatusWidget* widget) {
    MessageBox* box = message_box_find_open(&widget->desktop);
    Window* text =
        box != nullptr ? window_find_named(&box->base, message_box_name::text, nullptr) : nullptr;
    if (text == nullptr)
        return {};
    ScopedServices scope(widget);
    return text->ops->state_text(text, -1);
}

LaunchReport launch_report(int32_t result) {
    LaunchReport launch{};
    launch.game_id = "9bfaa0d0f7b93f12";
    launch.session = "Acid Pools";
    launch.address = "10.66.5.217";
    launch.password = "YFCPDVPKETZYKXA1";
    launch.mission = "Acid Pools";
    launch.allegiance = "Core";
    launch.result = result;
    // The planet this reply names. The tests that launch on the selected
    // planet use 709; a battle on another planet overwrites it.
    launch.area_id = 709;
    return launch;
}

// The launch_game reply (on_launch), its refusals, the debriefing ids, the
// idle work just after the launch, the in-battle lines, and the return to
// the Boneyards that reopens the chat console in the debriefing room.
void test_battle_launch(GameData* data, const oa::Palette* palette) {
    reset_log();
    g_launch = LaunchLog{};
    g_log.network = true;
    g_log.activation = ActivationResult::ok;
    StatusWidgetConfig config = make_config(data, true);
    config.palette = palette;
    config.host.battle_launched = host_battle_launched;
    config.host.launch_ended = host_launch_ended;
    config.host.run_frontend = host_run_frontend;
    config.host.leave_game = host_leave_game;
    config.host.left_game = host_left_game;
    config.host.set_latency_reply_flags = host_latency_flags;
    config.host.empty_chat_ring = host_empty_chat_ring;
    config.host.battle_message = host_battle_message;
    config.service.join_room = launch_service_join_room;
    config.service.leave_battles = launch_service_leave_battles;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget for the launch");
    if (widget == nullptr)
        return;
    click_part(widget, OverlayPart::enter);
    boneyards_status_widget_tick(widget);
    check(boneyards_status_widget_in_boneyards(widget), "in the Boneyards before the launch");
    check(
        g_log.audio.size() >= 2 && g_log.audio[g_log.audio.size() - 2] == "stop" &&
            g_log.audio.back() == "alt BGM",
        "the Main Frame build stops the sounds, then drones BGM on the alternate route"
    );
    // Both slides of the green end with a stop, BIGBUTTON and the drone;
    // without their sound they play nothing.
    for (const bool present : {true, false}) {
        g_log.audio.clear();
        {
            ScopedServices scope(widget);
            if (present)
                main_frame_present_screen(widget->system.main_frame, true);
            else
                main_frame_release_map(widget->system.main_frame, true);
        }
        check(
            g_log.audio == std::vector<std::string>({"stop", "BIGBUTTON", "alt BGM"}),
            "a slide ends with a stop, BIGBUTTON and BGM"
        );
        g_log.audio.clear();
        {
            ScopedServices scope(widget);
            if (present)
                main_frame_present_screen(widget->system.main_frame, false);
            else
                main_frame_release_map(widget->system.main_frame, false);
        }
        check(g_log.audio.empty(), "no sound without the slide's");
    }
    // Closing the window while the Boneyards is up asks "Exit to the system?":
    // NO only closes the box, YES has the idle work leave the game.
    check(boneyards_status_widget_up(widget), "the Boneyards is up");
    for (const bool yes : {false, true}) {
        check(boneyards_status_widget_ask_exit(widget), "the exit box opens");
        check(
            open_message_text(widget) == menu_message::exit_to_windows &&
                open_message_text(widget) == "Exit to the system?",
            "it asks Exit to the system?"
        );
        const char* answer = yes ? message_box_name::yes : message_box_name::no;
        {
            ScopedServices scope(widget);
            MessageBox* box = message_box_find_open(&widget->desktop);
            Window* button =
                box != nullptr ? window_find_named(&box->base, answer, nullptr) : nullptr;
            check(button != nullptr, "the box has YES and NO");
            if (button != nullptr) {
                const int32_t x = button->x + button->width / 2;
                const int32_t y = button->y + button->height / 2;
                boneyards_status_widget_event(
                    widget, pointer(MessageCode::left_down, x, y, button_left)
                );
                boneyards_status_widget_event(widget, pointer(MessageCode::left_up, x, y, 0));
            }
        }
        boneyards_status_widget_tick(widget);
        check(!boneyards_status_widget_message_open(widget), "an answer closes the box");
        boneyards_status_widget_idle(widget, 100, false);
        check(g_launch.leaves == (yes ? 1 : 0), yes ? "YES leaves the game" : "NO stays");
        boneyards_status_widget_idle(widget, 200, false);
        check(g_launch.leaves == (yes ? 1 : 0), "an answer acts once");
    }
    SessionRecord session{};
    session.level = SessionLevel::dossier_loaded;
    std::snprintf(session.user_name, sizeof session.user_name, "%s", "Ghost_Commander");
    g_events.session_changed(g_events.user, &session);
    widget->navigation->session.area_id = 709;

    // Refusals: one box each, with the reply's own text; nothing launches.
    for (int32_t result = 1; result <= 9; ++result) {
        const LaunchReport refused = launch_report(result);
        {
            ScopedServices scope(widget);
            g_events.launch(g_events.user, &refused);
        }
        const char* expected =
            result <= 8 ? launch_message::refused[result - 1] : launch_message::unknown;
        check(open_message_text(widget) == expected, "a refused launch shows the reply's text");
        {
            ScopedServices scope(widget);
            MessageBox* box = message_box_find_open(&widget->desktop);
            if (box != nullptr)
                box->base.owned = 1;
        }
        boneyards_status_widget_tick(widget);
    }
    check(
        g_launch.launches == 0 && !boneyards_status_widget_launched_game(widget),
        "a refusal launches nothing"
    );

    // A launch the host refuses for this simulation does not enter the battle.
    widget->config.host.launch_permitted = refuse_launch;
    {
        const LaunchReport refused = launch_report(0);
        ScopedServices scope(widget);
        g_events.launch(g_events.user, &refused);
    }
    check(
        open_message_text(widget) == "The Boneyards launches and reports games only on 3.1c, Core "
                                     "Contingency and Battle Tactics.",
        "a different simulation shows the refusal"
    );
    check(
        g_launch.launches == 0 && !boneyards_status_widget_launched_game(widget),
        "the refusal does not enter the battle"
    );
    check(
        g_launch.left_battles.size() == 1 && g_launch.left_battles[0] == 709,
        "a refused launch leaves the battle"
    );
    g_launch.left_battles.clear();
    {
        ScopedServices scope(widget);
        MessageBox* box = message_box_find_open(&widget->desktop);
        if (box != nullptr)
            box->base.owned = 1;
    }
    boneyards_status_widget_tick(widget);
    widget->config.host.launch_permitted = nullptr;

    // The galactic map names the planet the reply names, and its sector.
    {
        screens::GalaxyView& galaxy = widget->navigation->galaxy;
        galaxy.planet_count = 2;
        galaxy.planets[0].id = 12;
        std::snprintf(galaxy.planets[0].name, sizeof galaxy.planets[0].name, "%s", "Acid Pools");
        galaxy.planets[1].id = 709;
        std::snprintf(galaxy.planets[1].name, sizeof galaxy.planets[1].name, "%s", "Rosewood");
        std::snprintf(galaxy.sector_name, sizeof galaxy.sector_name, "%s", "Cygnus Reach");
    }
    // The battle console is open when the reply arrives.
    {
        ScopedServices scope(widget);
        main_frame_show_screen(widget->system.main_frame, console_screen::battle);
    }
    reset_log();
    const LaunchReport accepted = launch_report(0);
    {
        ScopedServices scope(widget);
        g_events.launch(g_events.user, &accepted);
    }
    {
        const char* planet = nullptr;
        const char* sector = nullptr;
        check(
            boneyards_status_widget_battle_place(widget, &planet, &sector) &&
                std::strcmp(planet, "Rosewood") == 0 && std::strcmp(sector, "Cygnus Reach") == 0,
            "the launch keeps the planet's name and its sector's"
        );
        std::snprintf(
            widget->navigation->galaxy.sector_name,
            sizeof widget->navigation->galaxy.sector_name,
            "%s",
            "Elsewhere"
        );
        widget->navigation->galaxy.planets[1].id = 3;
        check(
            boneyards_status_widget_battle_place(widget, &planet, &sector) &&
                std::strcmp(planet, "Rosewood") == 0 && std::strcmp(sector, "Cygnus Reach") == 0,
            "a later map leaves the battle's planet and sector"
        );
        check(
            !boneyards_status_widget_battle_place(nullptr, &planet, &sector),
            "without the widget there is no place"
        );
        // The map is as it was before the check, for the rest of this one.
        widget->navigation->galaxy.planet_count = 0;
        widget->navigation->galaxy.sector_name[0] = '\0';
    }
    check(
        g_launch.launches == 1 && g_launch.launch_user == "Ghost_Commander" &&
            g_launch.launch_session == "Acid Pools",
        "the host receives the launch with the Boneyards name"
    );
    check(
        g_launch.left_battles.size() == 1 && g_launch.left_battles[0] == 709,
        "closing the battle console leaves the planet's battles"
    );
    check(
        g_launch.frontend_passes == 1 && g_launch.state_at_pass == 16 &&
            g_launch.signal_at_pass == 0,
        "state 16 with signal 0, then a frontend pass runs the connection selection"
    );
    check(
        boneyards_status_widget_launched_game(widget) && widget->system.just_launched == 1 &&
            widget->navigation->session.debriefing_pending,
        "the battle counts as launched and just started"
    );
    check(!boneyards_status_widget_in_boneyards(widget), "the Main Frame hides");
    uint32_t area = 0;
    const char* game_id = nullptr;
    check(
        boneyards_status_widget_battle_ids(widget, &area, &game_id) && area == 709 &&
            std::strcmp(game_id, "9bfaa0d0f7b93f12") == 0,
        "the launch keeps the game id"
    );
    check(
        widget->navigation->session.debrief_area_id == 709 &&
            widget->navigation->session.debrief_room_id == 0 &&
            widget->system.debriefing_game_id[0] == '\0',
        "the debriefing starts as the current area's"
    );

    // Just after the launch: the chat ring empties and slot 43 gets 0, once.
    boneyards_status_widget_idle(widget, 5000, false);
    boneyards_status_widget_idle(widget, 5100, false);
    boneyards_status_widget_idle(widget, 7000, false);
    check(
        g_launch.chat_ring_emptied == 1 && g_launch.latency_flags.size() == 1 &&
            g_launch.latency_flags[0] == 0 && widget->system.just_launched == 0,
        "one slot 43 call with 0 and one emptied chat ring"
    );

    // Lines for the battle while it runs.
    check(g_events.battle_active(g_events.user), "the client sees the launched battle");
    g_events.battle_message(g_events.user, "Boneyards: WARNING! DISCONNECTED FROM THE BONEYARDS!");
    AlertReport alert{};
    alert.kind = static_cast<int32_t>(AlertKind::message_box);
    alert.text = "Maintenance at noon";
    alert.notification_type = "";
    {
        ScopedServices scope(widget);
        g_events.system_alert(g_events.user, &alert);
    }
    check(
        g_launch.battle_messages.size() == 2 &&
            g_launch.battle_messages[1] == "Maintenance at noon",
        "an alert during the battle goes to its messages"
    );

    // The debriefing: only the first names the room.
    g_events.debriefing(g_events.user, 800, 7, false);
    g_events.debriefing(g_events.user, 710, 42, true);
    check(
        widget->navigation->session.debrief_area_id == 710 &&
            widget->navigation->session.debrief_room_id == 42 &&
            std::strcmp(widget->system.debriefing_game_id, "9bfaa0d0f7b93f12") == 0,
        "the first debriefing names the room and the game"
    );

    // A launched battle keeps the Boneyards from asking "Exit to the system?".
    check(boneyards_status_widget_up(widget), "the Boneyards stays up under a launched battle");

    // The return to the Boneyards.
    reset_log();
    boneyards_status_widget_return_from_battle(widget);
    check(
        g_log.audio.size() >= 3 && g_log.audio[0] == "stop" && g_log.audio[1] == "alt BGM" &&
            g_log.audio.back() == "stop",
        "the chat console drones BGM as it reopens, and the return then stops every sound"
    );
    check(
        g_launch.left_games.size() == 1 && g_launch.left_games[0].first == 709 &&
            g_launch.left_games[0].second == "9bfaa0d0f7b93f12",
        "left_game names the area and the game"
    );
    check(
        boneyards_status_widget_in_boneyards(widget) &&
            widget->system.main_frame->console_screen == console_screen::chat,
        "the Main Frame opens on the chat console"
    );
    check(
        !g_launch.joined_rooms.empty() && g_launch.joined_rooms.back().first == 710 &&
            g_launch.joined_rooms.back().second == 42,
        "the chat console enters the debriefing room"
    );
    check(
        !boneyards_status_widget_launched_game(widget) &&
            !widget->navigation->session.debriefing_pending,
        "the battle no longer counts as launched"
    );
    check(g_launch.launches_ended == 1, "the host hears the launched battle end, once");
    check(
        g_log.state == frontend_value::state_boneyards && g_log.signal == 1, "state 7 with signal 1"
    );
    boneyards_status_widget_idle(widget, 9000, false);
    check(g_launch.latency_flags.size() == 1, "no slot 43 call without a launched battle");
    boneyards_status_widget_destroy(widget);
    g_log.network = false;
    g_log.activation = ActivationResult::module_missing;
}

// The topmost top-level window, or null when the desktop is empty.
Window* top_window(const StatusWidget* widget) {
    const WindowList& windows = widget->desktop.windows;
    if (windows.count <= 0 || windows.items == nullptr)
        return nullptr;
    return windows.items[windows.count - 1];
}

// A QuickPlay battle on a planet other than the selected one is left on the
// battle's planet. A message box opened before the console comes back stays
// in front of that console, and a click on Ok reaches the box.
void test_battle_on_another_planet(GameData* data, const oa::Palette* palette) {
    reset_log();
    g_launch = LaunchLog{};
    g_log.network = true;
    g_log.activation = ActivationResult::ok;
    StatusWidgetConfig config = make_config(data, true);
    config.palette = palette;
    config.host.battle_launched = host_battle_launched;
    config.host.launch_ended = host_launch_ended;
    config.host.left_game = host_left_game;
    config.service.leave_battles = launch_service_leave_battles;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget for a battle on another planet");
    if (widget == nullptr)
        return;
    click_part(widget, OverlayPart::enter);
    boneyards_status_widget_tick(widget);
    check(boneyards_status_widget_in_boneyards(widget), "in the Boneyards before the other planet");
    if (!boneyards_status_widget_in_boneyards(widget)) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    widget->navigation->session.area_id = 0;
    {
        ScopedServices scope(widget);
        main_frame_show_screen(widget->system.main_frame, console_screen::battle);
    }
    LaunchReport accepted = launch_report(0);
    accepted.area_id = 1800;
    {
        ScopedServices scope(widget);
        g_events.launch(g_events.user, &accepted);
    }
    check(
        g_launch.left_battles.size() == 1 && g_launch.left_battles[0] == 0,
        "closing the battle console leaves the selected planet's battles"
    );
    uint32_t area = 0;
    const char* game_id = nullptr;
    check(
        boneyards_status_widget_battle_ids(widget, &area, &game_id) && area == 1800 &&
            std::strcmp(game_id, "9bfaa0d0f7b93f12") == 0,
        "the launch keeps the battle's planet, not the selected one"
    );
    widget->navigation->session.area_id = 12;
    check(
        boneyards_status_widget_battle_ids(widget, &area, &game_id) && area == 1800,
        "selecting another planet leaves the battle's planet"
    );

    // "Host not found" opens before the return shows the console again.
    boneyards_status_widget_show_notice(widget, "Host not found");
    check(open_message_text(widget) == "Host not found", "the join failure shows its box");
    boneyards_status_widget_return_from_battle(widget);
    check(
        g_launch.left_games.size() == 1 && g_launch.left_games[0].first == 1800 &&
            g_launch.left_games[0].second == "9bfaa0d0f7b93f12",
        "left_game names the battle's planet"
    );
    MessageBox* box = message_box_find_open(&widget->desktop);
    check(
        box != nullptr && top_window(widget) == &box->base,
        "the return leaves the message box in front of the console"
    );
    desktop_raise(&widget->desktop, &widget->system.main_frame->base);
    check(
        top_window(widget) == &widget->system.main_frame->base,
        "the console can be raised over the box"
    );
    boneyards_status_widget_tick(widget);
    box = message_box_find_open(&widget->desktop);
    check(
        box != nullptr && top_window(widget) == &box->base,
        "a tick brings the message box back in front"
    );
    int32_t x = 0;
    int32_t y = 0;
    check(
        boneyards_status_widget_message_button(widget, message_box_name::ok, &x, &y),
        "the box has Ok"
    );
    desktop_raise(&widget->desktop, &widget->system.main_frame->base);
    click(widget, x, y);
    boneyards_status_widget_tick(widget);
    check(
        !boneyards_status_widget_message_open(widget),
        "Ok answers while the console covered the box"
    );

    // The game list has left the main menu, so the console's GUI is down and
    // a join failure cannot open its box yet. The return opens it in front
    // and sends the battle room's leave once.
    widget->system.chat_console_pending = 1;
    std::snprintf(
        widget->system.launched_game_id,
        sizeof widget->system.launched_game_id,
        "%s",
        "9bfaa0d0f7b93f12"
    );
    widget->system.launched_area_id = 1800;
    boneyards_status_widget_leave_main_menu(widget);
    const auto leaves_while_joining = g_launch.left_games.size();
    boneyards_status_widget_show_notice(
        widget, "Host not found - Host may have left the game before you arrived."
    );
    check(
        g_launch.left_games.size() == leaves_while_joining,
        "the join failure sends no leave while the player is still joining"
    );
    check(
        !boneyards_status_widget_message_open(widget),
        "a join failure keeps its text while the console is down"
    );
    const int ended_before = g_launch.launches_ended;
    boneyards_status_widget_return_from_battle(widget);
    check(
        g_log.state == frontend_value::state_boneyards &&
            !boneyards_status_widget_launched_game(widget),
        "the failed join returns to the Boneyards"
    );
    check(
        open_message_text(widget) ==
            "Host not found - Host may have left the game before you arrived.",
        "the return opens the join failure's box"
    );
    check(g_launch.launches_ended == ended_before + 1, "the return ends the launch");
    check(
        g_launch.left_games.size() == leaves_while_joining + 1 &&
            g_launch.left_games.back().first == 1800 &&
            g_launch.left_games.back().second == "9bfaa0d0f7b93f12",
        "the failed join sends the leave once, for the battle's planet"
    );
    boneyards_status_widget_return_from_battle(widget);
    check(
        g_launch.left_games.size() == leaves_while_joining + 1,
        "a second return does not leave again"
    );
    boneyards_status_widget_destroy(widget);
}

// Runs the Main Frame until the green layer stops sliding.
void finish_green(StatusWidget* widget) {
    const ConsoleLayer* layer = widget->system.main_frame->console;
    for (int32_t i = 0; layer != nullptr && layer->sliding != 0 && i < 100; ++i)
        boneyards_status_widget_tick(widget);
}

// No debriefing room: the return shows the battle console again, and the chat
// console kept from before the battle still holds the room it left and the
// entry line.
void test_return_shows_battle(GameData* data, const oa::Palette* palette) {
    reset_log();
    g_launch = LaunchLog{};
    g_log.network = true;
    g_log.activation = ActivationResult::ok;
    StatusWidgetConfig config = make_config(data, true);
    config.palette = palette;
    config.host.battle_launched = host_battle_launched;
    config.host.launch_ended = host_launch_ended;
    config.host.run_frontend = host_run_frontend;
    config.host.left_game = host_left_game;
    config.service.join_room = launch_service_join_room;
    config.service.leave_battles = launch_service_leave_battles;
    config.service.enter_battles = launch_service_enter_battles;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget for the return");
    if (widget == nullptr)
        return;
    click_part(widget, OverlayPart::enter);
    boneyards_status_widget_tick(widget);
    check(boneyards_status_widget_in_boneyards(widget), "in the Boneyards before the battle");
    MainFrame* frame = widget->system.main_frame;
    // The update console opens on its own and then hands over to login. Chat
    // waits until that handover is done, or a later cycle replaces it.
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
    check(frame->console_screen == console_screen::login, "the login console is up");
    if (frame->console_screen != console_screen::login) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    widget->navigation->session.area_id = 709;
    widget->navigation->chat.area_rooms[0] = screens::AreaRoom{709, 14};
    widget->navigation->chat.area_room_count = 1;
    {
        ScopedServices scope(widget);
        main_frame_show_screen(frame, console_screen::chat);
    }
    finish_green(widget);
    auto* chat = reinterpret_cast<ChatView*>(frame->active_screen);
    const bool chat_open =
        chat != nullptr && window_is_named(&chat->base, "ChatConsole") && chat->input != nullptr;
    check(chat_open, "the chat console is open");
    if (!chat_open) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    {
        ScopedServices scope(widget);
        single_edit_set_text(chat->input, "still here");
        g_events.room_entered(g_events.user, 0);
    }
    check(widget->navigation->session.room_id == 14, "the chat entered the remembered room");
    check(
        !g_launch.joined_rooms.empty() && g_launch.joined_rooms.back().first == 709 &&
            g_launch.joined_rooms.back().second == 14,
        "opening chat joins the room it left last time"
    );
    const std::size_t joins_before = g_launch.joined_rooms.size();
    {
        ScopedServices scope(widget);
        main_frame_show_screen(frame, console_screen::battle);
    }
    finish_green(widget);
    Window* battle = frame->active_screen;
    check(
        frame->console_screen == console_screen::battle && battle != nullptr &&
            battle->visible == 1,
        "the battle console is open"
    );
    const LaunchReport accepted = launch_report(0);
    {
        ScopedServices scope(widget);
        g_events.launch(g_events.user, &accepted);
    }
    check(
        widget->navigation->session.debrief_room_id == 0 && g_launch.left_battles.size() == 1,
        "the launch leaves the battles and names no debriefing room"
    );
    const std::size_t entered_before = g_launch.entered_battles.size();
    reset_log();
    boneyards_status_widget_return_from_battle(widget);
    check(
        g_log.audio.size() >= 3 && g_log.audio[0] == "stop" && g_log.audio[1] == "alt BGM" &&
            g_log.audio.back() == "stop",
        "the return stops the sounds, drones BGM, then stops them again"
    );
    check(
        frame->console_screen == console_screen::battle && frame->active_screen == battle &&
            battle->visible == 1,
        "the return shows the same battle console"
    );
    check(console_open_chat() == chat, "the chat console is the one from before the battle");
    check(
        std::strcmp(single_edit_text(chat->input), "still here") == 0,
        "the chat entry line is intact"
    );
    check(
        widget->navigation->chat.area_room_count == 1 &&
            widget->navigation->chat.area_rooms[0].area_id == 709 &&
            widget->navigation->chat.area_rooms[0].room_id == 14,
        "the room the chat left is still remembered"
    );
    check(g_launch.joined_rooms.size() == joins_before, "the return does not join another room");
    check(
        g_launch.entered_battles.size() == entered_before + 1 &&
            g_launch.entered_battles.back() == 709 && g_launch.left_battles.size() == 1,
        "the battle console enters the planet's battles again"
    );

    // Chat and Battle stay off while the map is loading or no planet is
    // selected. Closing and opening them again shows the consoles the
    // battle left behind.
    widget->navigation->galaxy.loading = false;
    widget->navigation->galaxy.selected = 0;
    Window* battle_button = frame->console_buttons[static_cast<int32_t>(ConsoleButton::battle)];
    Window* chat_button = frame->console_buttons[static_cast<int32_t>(ConsoleButton::chat)];
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    check(
        battle_button != nullptr && chat_button != nullptr &&
            battle_button->states.current == button_state::unpressed,
        "Battle is on so it can close"
    );
    if (battle_button == nullptr || chat_button == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    click(
        widget,
        battle_button->x + battle_button->width / 2,
        battle_button->y + battle_button->height / 2
    );
    finish_green(widget);
    check(
        frame->console_screen == console_screen::none && battle->visible == 0,
        "Battle closes the console the return showed"
    );
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    check(battle_button->states.current == button_state::unpressed, "Battle is on so it can open");
    click(
        widget,
        battle_button->x + battle_button->width / 2,
        battle_button->y + battle_button->height / 2
    );
    finish_green(widget);
    check(
        frame->console_screen == console_screen::battle && frame->active_screen == battle &&
            battle->visible == 1,
        "Battle opens the same console again"
    );
    check(chat_button->states.current == button_state::unpressed, "Chat is on");
    click(
        widget, chat_button->x + chat_button->width / 2, chat_button->y + chat_button->height / 2
    );
    finish_green(widget);
    check(
        frame->console_screen == console_screen::chat && frame->active_screen == &chat->base &&
            std::strcmp(single_edit_text(chat->input), "still here") == 0,
        "Chat opens the same console and keeps the entry line"
    );
    boneyards_status_widget_destroy(widget);
    g_log.network = false;
    g_log.activation = ActivationResult::module_missing;
}

// Recording settings service. BetaLoginUX is a number beside Username.
struct StoredNumber {
    std::string key;
    int32_t value;
};

struct StoredString {
    std::string key;
    std::string value;
};

std::vector<StoredNumber> g_numbers;
std::vector<StoredString> g_strings;
ServiceResult g_login_result = ServiceResult::pending;

bool setting_save_number(void*, const char* key, int32_t value) {
    g_numbers.push_back(StoredNumber{key != nullptr ? key : "", value});
    return true;
}

bool setting_save_string(void*, const char* key, const char* value) {
    g_strings.push_back(StoredString{key != nullptr ? key : "", value != nullptr ? value : ""});
    return true;
}

int32_t setting_read_number(void*, const char* key, int32_t fallback) {
    const std::string name = key != nullptr ? key : "";
    for (std::size_t i = g_numbers.size(); i > 0; --i) {
        if (g_numbers[i - 1].key == name)
            return g_numbers[i - 1].value;
    }
    return fallback;
}

int32_t count_stored(const std::vector<StoredNumber>& rows, const char* key) {
    int32_t count = 0;
    for (const StoredNumber& stored : rows) {
        if (stored.key == key)
            ++count;
    }
    return count;
}

int32_t count_stored(const std::vector<StoredString>& rows, const char* key) {
    int32_t count = 0;
    for (const StoredString& stored : rows) {
        if (stored.key == key)
            ++count;
    }
    return count;
}

const char* last_stored_string(const char* key) {
    for (std::size_t i = g_strings.size(); i > 0; --i) {
        if (g_strings[i - 1].key == key)
            return g_strings[i - 1].value.c_str();
    }
    return "";
}

ServiceResult scripted_login(void*, const LoginRequest*) {
    return g_login_result;
}

void scripted_cancel(void*) {
}

// The form follows its release constant or the stored BetaLoginUX number.
int32_t form_offer(int32_t beta) {
    const int32_t released = release_flag(ReleaseFeature::registration_form);
    const bool shown = feature_visible(ReleaseFeature::registration_form, 0, released, beta);
    return shown ? 1 : 0;
}

void publish_level(StatusWidget* widget, SessionLevel level, uint8_t matchmaking) {
    SessionRecord session{};
    session.level = level;
    session.matchmaking_enabled = matchmaking;
    ScopedServices scope(widget);
    g_events.session_changed(g_events.user, &session);
}

void start_member_login(StatusWidget* widget, const char* name) {
    screens::LoginDialog* dialog = &widget->navigation->login;
    std::snprintf(dialog->name, sizeof dialog->name, "%s", name);
    std::snprintf(dialog->password, sizeof dialog->password, "secret");
    screens::login_on_control(dialog, screens::LoginControl::login_button);
}

// A completed Admin or Beta Tester login stores BetaLoginUX 1. A completed
// ordinary login stores 0. A login that never reaches the dossier stores
// nothing. Opening login reads that number back through the same service.
void test_beta_login_ux(GameData* data) {
    reset_log();
    g_numbers.clear();
    g_strings.clear();
    g_log.network = true;
    g_log.activation = ActivationResult::ok;
    StatusWidgetConfig config = make_config(data, true);
    config.settings.save_number = setting_save_number;
    config.settings.save_string = setting_save_string;
    config.settings.read_number = setting_read_number;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget for BetaLoginUX");
    if (widget == nullptr)
        return;
    click_part(widget, OverlayPart::enter);
    boneyards_status_widget_tick(widget);
    check(boneyards_status_widget_in_boneyards(widget), "in the Boneyards for BetaLoginUX");
    MainFrame* frame = widget->system.main_frame;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
    check(frame->console_screen == console_screen::login, "login opens before a preference");
    if (frame->console_screen != console_screen::login || g_events.session_changed == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    check(
        widget->navigation->login.registration_form == form_offer(0),
        "login offers the form only from the release constant"
    );
    check(count_stored(g_numbers, kBetaLoginUxKey) == 0, "opening login stores nothing");

    // The login dialog calls the screens' copy of the service, filled when
    // the screens were initialised, not the widget config's copy.
    widget->navigation->service.login = scripted_login;
    widget->navigation->service.cancel_login = scripted_cancel;
    g_login_result = ServiceResult::pending;
    start_member_login(widget, "Ada");
    check(
        widget->navigation->login.state == screens::LoginState::connecting,
        "a pending login stays in progress"
    );
    publish_level(widget, SessionLevel::logged_in, 1);
    check(
        count_stored(g_strings, "Username") == 1 &&
            std::strcmp(last_stored_string("Username"), "Ada") == 0,
        "Username is saved when the login is confirmed"
    );
    check(count_stored(g_numbers, kBetaLoginUxKey) == 0, "BetaLoginUX waits for the dossier");
    publish_level(widget, SessionLevel::dossier_loaded, 1);
    check(count_stored(g_numbers, kBetaLoginUxKey) == 1, "a privileged login stores BetaLoginUX");
    check(setting_read_number(nullptr, kBetaLoginUxKey, 0) == 1, "that number is 1");
    check(
        widget->navigation->login.state == screens::LoginState::planetary,
        "the dossier completes the login"
    );

    // Still on the dossier: another copy of it is not a new login.
    widget->navigation->login.state = screens::LoginState::connecting;
    publish_level(widget, SessionLevel::dossier_loaded, 0);
    check(count_stored(g_numbers, kBetaLoginUxKey) == 1, "a repeat dossier does not store again");

    {
        ScopedServices scope(widget);
        main_frame_show_screen(frame, console_screen::login);
    }
    check(
        widget->navigation->login.registration_form == form_offer(1),
        "opening login reads BetaLoginUX 1"
    );

    g_login_result = ServiceResult::pending;
    start_member_login(widget, "Ada");
    publish_level(widget, SessionLevel::logged_in, 0);
    check(
        count_stored(g_numbers, kBetaLoginUxKey) == 1, "an ordinary confirmation stores nothing yet"
    );
    publish_level(widget, SessionLevel::dossier_loaded, 0);
    check(count_stored(g_numbers, kBetaLoginUxKey) == 2, "an ordinary login stores BetaLoginUX");
    check(setting_read_number(nullptr, kBetaLoginUxKey, 1) == 0, "that number is 0");
    {
        ScopedServices scope(widget);
        main_frame_show_screen(frame, console_screen::login);
    }
    check(
        widget->navigation->login.registration_form == form_offer(0),
        "opening login reads BetaLoginUX 0"
    );

    const int32_t beta_writes = count_stored(g_numbers, kBetaLoginUxKey);
    const int32_t name_writes = count_stored(g_strings, "Username");
    g_login_result = ServiceResult::failed;
    start_member_login(widget, "Bea");
    check(
        widget->navigation->login.state == screens::LoginState::idle,
        "a refused login leaves the dialog idle"
    );
    check(
        count_stored(g_numbers, kBetaLoginUxKey) == beta_writes, "a refused login stores nothing"
    );
    check(
        count_stored(g_strings, "Username") == name_writes, "a refused login does not save Username"
    );

    g_login_result = ServiceResult::pending;
    start_member_login(widget, "Bea");
    publish_level(widget, SessionLevel::logged_in, 1);
    publish_level(widget, SessionLevel::disconnected, 1);
    check(
        count_stored(g_strings, "Username") == name_writes + 1 &&
            std::strcmp(last_stored_string("Username"), "Bea") == 0,
        "Username is saved before a dropped dossier"
    );
    check(
        count_stored(g_numbers, kBetaLoginUxKey) == beta_writes,
        "a login without a dossier stores no BetaLoginUX"
    );
    screens::login_on_control(&widget->navigation->login, screens::LoginControl::cancel_button);
    publish_level(widget, SessionLevel::dossier_loaded, 1);
    check(
        count_stored(g_numbers, kBetaLoginUxKey) == beta_writes,
        "a dossier after the login was cancelled stores nothing"
    );

    boneyards_status_widget_destroy(widget);
    g_log.network = false;
    g_log.activation = ActivationResult::module_missing;
}

bool always_online(void*) {
    return true;
}

// A replayed login types a name in a different case from the dossier. The
// status then shows the server's spelling, and that spelling is the saved
// login name.
void test_dossier_login_name(GameData* data) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "oa-dossier-login-name.replay";
    {
        std::ofstream out(path);
        out << "progress 4 0 0 0\n"
               "progress 7 0 0 0\n"
               "progress 8 0 1 0\n"
               "dossier LOGINNAME string Oa_Check\n"
               "dossier USER_STATUS int 1\n"
               "dossier ALLEGIANCE string C\n";
    }
    char error[0x200]{};
    const bool installed = rover_replay_install(path.string().c_str(), error, sizeof error);
    check(installed, "the dossier-name replay installs");
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    if (!installed)
        return;
    *rover_session() = RoverSession{};

    reset_log();
    g_numbers.clear();
    g_strings.clear();
    StatusWidgetConfig config = make_config(data, false);
    config.service = rover_session_service();
    config.service.network_available = always_online;
    config.settings.save_number = setting_save_number;
    config.settings.save_string = setting_save_string;
    config.settings.read_number = setting_read_number;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget for the dossier login name");
    if (widget == nullptr) {
        rover_replay_uninstall();
        return;
    }
    click_part(widget, OverlayPart::enter);
    boneyards_status_widget_tick(widget);
    MainFrame* frame = widget->system.main_frame;
    for (int32_t i = 0;
         frame != nullptr && frame->console_screen != console_screen::login && i < 160;
         ++i)
        boneyards_status_widget_tick(widget);
    check(
        frame != nullptr && frame->console_screen == console_screen::login,
        "login opens for the replay"
    );
    if (frame == nullptr || frame->console_screen != console_screen::login) {
        widget->config.service.close(widget->config.service.user);
        boneyards_status_widget_destroy(widget);
        rover_replay_uninstall();
        return;
    }

    start_member_login(widget, "oa_check");
    for (int32_t i = 0;
         widget->system.connection_level != static_cast<int32_t>(SessionLevel::dossier_loaded) &&
         i < 40;
         ++i)
        boneyards_status_widget_tick(widget);

    MainFrameProbe probe{};
    check(boneyards_status_widget_probe(widget, &probe), "the status can be read after login");
    check(
        std::strcmp(probe.user_name, "Oa_Check") == 0 &&
            std::strcmp(widget->system.user_name, "Oa_Check") == 0,
        "the status shows the server's login name"
    );
    check(
        std::strcmp(last_stored_string("Username"), "Oa_Check") == 0,
        "the saved login name is the server's"
    );

    widget->config.service.close(widget->config.service.user);
    for (int32_t i = 0; widget->system.connection_level != 0 && i < 10; ++i)
        boneyards_status_widget_tick(widget);
    boneyards_status_widget_destroy(widget);
    rover_replay_uninstall();
}

/// Reads BYStopThatIncessantDroning as set; other keys give their fallback.
///
/// @param user unused
/// @param key setting asked for
/// @param fallback value without the setting
/// @return 1 for the droning setting
int32_t read_droning_off(void*, const char* key, int32_t fallback) {
    return std::strcmp(key, screens::kSoundOffKey) == 0 ? 1 : fallback;
}

// With BYStopThatIncessantDroning set the Boneyards still stops the sounds
// at its points, but never drones BGM.
void test_background_off(GameData* data, const oa::Palette* palette) {
    reset_log();
    g_log.network = true;
    g_log.activation = ActivationResult::ok;
    StatusWidgetConfig config = make_config(data, true);
    config.palette = palette;
    config.settings.read_number = read_droning_off;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget with the drone off");
    if (widget == nullptr)
        return;
    click_part(widget, OverlayPart::enter);
    boneyards_status_widget_tick(widget);
    check(boneyards_status_widget_in_boneyards(widget), "in the Boneyards with the drone off");
    check(
        !g_log.audio.empty() && g_log.audio.back() == "stop",
        "the Main Frame build only stops the sounds"
    );
    g_log.audio.clear();
    {
        ScopedServices scope(widget);
        main_frame_present_screen(widget->system.main_frame, true);
    }
    check(
        g_log.audio == std::vector<std::string>({"stop", "BIGBUTTON"}),
        "a slide ends with a stop and BIGBUTTON only"
    );
    boneyards_status_widget_stop_sounds(widget);
    check(g_log.audio.back() == "stop", "the widget stops the sounds on request");
    boneyards_status_widget_destroy(widget);
    g_log.network = false;
    g_log.activation = ActivationResult::module_missing;
}

void test_shade_round_trip(GameData* data) {
    // Dimming then lightening goes through the palette tables and is lossy.
    ConsoleImages* images = console_images_create(FileReader{data, read_asset});
    const Handle a = console_images_load(images, "Main_Console.Gaf", "main_menu_overlay", 0, 9);
    check(a >= 0, "case-insensitive sequence lookup");
    const Handle b = console_images_clone(images, a, 9);
    // console_images_clone copies the picture into a new entry keyed on its argument,
    // with no hold delay (insert_image).
    const Handle keyed = console_images_clone(images, a, 5);
    const Sprite* source = console_images_sprite(images, a);
    const Sprite* copy = console_images_sprite(images, keyed);
    const ImageService service = console_images_service(images);
    check(
        copy != nullptr && copy->key == 5 && copy->width == source->width &&
            copy->height == source->height && copy->data != source->data &&
            std::memcmp(copy->data, source->data, source->width * source->height) == 0,
        "clone copies the picture under the new key"
    );
    check(service.frame_delay(service.self, keyed) == 0, "clone holds no delay");
    service.release(service.self, keyed);
    const ShadeTables tables{data->shade.data(), data->light.data()};
    console_images_shade(images, b, -0x18, tables);
    const uint8_t first = static_cast<const uint8_t*>(console_images_sprite(images, a)->data)[0];
    const uint8_t dark = static_cast<const uint8_t*>(console_images_sprite(images, b)->data)[0];
    check(dark == data->shade[8 * 256 + first], "level -0x18 uses shade row 8");
    console_images_shade(images, b, 0x18, tables);
    const uint8_t back = static_cast<const uint8_t*>(console_images_sprite(images, b)->data)[0];
    check(back == data->light[24 * 256 + dark], "level 0x18 uses light row 24");
    check(
        console_images_load(images, "Main_Console.Gaf", "NoSuchSequence", 0, 9) == k_no_handle,
        "missing sequence"
    );
    check(
        console_images_load(images, "Main_Console.Gaf", "MM_Single_Button", 3, 9) == k_no_handle,
        "missing frame"
    );
    console_images_destroy(images);
}

// A toolkit sound and the error beep reach the host hook the overlay already
// uses for its own clicks. The file name is what the host looks up.
void test_toolkit_audio(GameData* data) {
    oa::Palette palette{};
    palette.entries[1] = oa::PaletteEntry{255, 255, 255, 0};
    StatusWidgetConfig config = make_config(data, false);
    config.palette = &palette;
    reset_log();
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr && widget->toolkit != nullptr, "toolkit attached with the host audio");
    if (widget == nullptr || widget->toolkit == nullptr)
        return;
    const Services shared = toolkit_services(widget->toolkit);
    check(widget->services.sounds == shared.sounds, "the overlay uses the toolkit sounds");
    const std::size_t before = g_log.sounds.size();
    const Handle press = shared.sounds->load(shared.sounds->self, "HOVEROK1.WAV");
    shared.sounds->play(shared.sounds->self, press);
    shared.sounds->beep(shared.sounds->self);
    check(g_log.sounds.size() == before + 2, "a sound and a beep reach the host");
    check(
        before + 1 <= g_log.sounds.size() && g_log.sounds[before] == "HOVEROK1.WAV" &&
            g_log.sounds[before + 1] == "beep2.wav",
        "the host hears the file name"
    );
    boneyards_status_widget_destroy(widget);
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the status widget tests");
    const std::filesystem::path by_ccx = root / "by.ccx";
    GameData data;
    data.store.mount(by_ccx);
    data.shade = oa::test::netgame::read_archived(
        root, oa::test::netgame::kBaseArchive, "palettes/palette.shd"
    );
    data.light = oa::test::netgame::read_archived(
        root, oa::test::netgame::kBaseArchive, "palettes/palette.lht"
    );
    check(!data.shade.empty() && !data.light.empty(), "the install's shade and light tables");

    StatusWidget* widget = boneyards_status_widget_create(make_config(&data, false));
    check(widget != nullptr, "status widget created");
    if (widget == nullptr)
        return 1;
    test_layout(widget);
    test_snapshot(widget);
    test_single_click(widget);
    test_drag_cancel(widget);
    test_panel_passthrough(widget);
    test_enter_offline(widget);
    test_b_key(widget);
    test_multi(widget);
    test_intro_and_credits(widget);
    test_activity_light(widget);
    boneyards_status_widget_destroy(widget);

    test_enter_online(&data);
    test_beta_login_ux(&data);
    test_dossier_login_name(&data);
    test_main_frame_probe(&data);
    test_press_taken_below(&data);
    test_box_release_through_system(&data);
    test_shade_round_trip(&data);

    const std::filesystem::path gp3 = root / "rev31.gp3";
    if (data.store.try_mount(gp3)) {
        const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
        oa::Palette palette{};
        for (int i = 0; i < OA_PALETTE_COLORS && static_cast<std::size_t>(i * 4 + 2) < raw.size();
             ++i)
            palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
        test_cursor(&data, &palette);
        test_battle_launch(&data, &palette);
        test_battle_on_another_planet(&data, &palette);
        test_return_shows_battle(&data, &palette);
        test_background_off(&data, &palette);
    }

    test_toolkit_audio(&data);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("status widget tests passed\n");
    return 0;
}
