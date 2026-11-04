// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registers the status widget overlay and drives it through ScreenContext the
// way the native runtime does, using the BY1117a install: the main-menu strip
// in frontend state 2, then Enter Boneyards against a fake Rover table, which
// hands the frame, the keyboard and the 30 Hz clock to the Boneyards in
// state 7 until the Main Frame's exit reloads the main menu.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "console_state.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/ui/frontend_state/dispatcher.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/screen_registry.hpp"
#include "oa/platform/system.hpp"

namespace oa::app {
oa::boneyards::console::StatusWidget* boneyards_console_status_widget();
void register_boneyards_console_screens(ScreenRegistry* registry);
void boneyards_console_idle(uint32_t now_ms);
bool boneyards_console_up();
bool boneyards_console_ask_exit();
void boneyards_console_stop_sounds();
void boneyards_console_set_launch_bridge(const oa::boneyards::console::LaunchBridge& bridge);
void boneyards_console_show_notice(const char* text);
} // namespace oa::app

using namespace oa::app;
namespace by = oa::boneyards;
namespace console = oa::boneyards::console;

namespace {

int g_failures = 0;
std::string g_status;
std::string g_sound;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

std::vector<ScreenId> g_requested_screens;

void request_screen(void*, ScreenId id) {
    g_requested_screens.push_back(id);
}

void play_sound(void*, const char* name) {
    g_sound = name;
}

void set_status(void*, const char* text) {
    g_status = text;
}

int g_signal = -1;

void set_frontend_signal(void*, std::uint8_t signal) {
    g_signal = signal;
}

// Frontend calls in order, as "state N", "close", "reload" and "text N".
std::vector<std::string> g_calls;
std::uint32_t g_tick = 0;

void set_frontend_state(void*, std::uint8_t state) {
    g_calls.push_back("state " + std::to_string(state));
}

void close_main_menu_panel(void*) {
    g_calls.emplace_back("close");
}

void reload_main_menu_panel(void*) {
    g_calls.emplace_back("reload");
}

void set_text_input(void*, int enabled) {
    g_calls.push_back("text " + std::to_string(enabled));
}

std::uint32_t current_tick(void*) {
    return g_tick;
}

// Sound names registered with the host, as "CATEGORY=file".
std::vector<std::string> g_registered_sounds;

void register_sound(void*, const char* category, const char* file) {
    g_registered_sounds.push_back(std::string(category) + "=" + file);
}

int g_main_menu_overlay = -1;

void set_main_menu_overlay(void*, int present) {
    g_main_menu_overlay = present;
}

// The sound stops and alternate-route sounds in order, as "stop" and
// "alt <name>".
std::vector<std::string> g_audio;

void stop_sounds(void*) {
    g_audio.emplace_back("stop");
}

void play_sound_alternate(void*, const char* name) {
    g_audio.push_back(std::string("alt ") + name);
}

int g_frontend_passes = 0;
int g_launches_ended = 0;

void count_launch_ended(void*) {
    ++g_launches_ended;
}

void run_frontend(void*) {
    ++g_frontend_passes;
}

const ScreenServices k_services{
    request_screen,
    play_sound,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    set_status,
    set_frontend_signal,
    set_frontend_state,
    close_main_menu_panel,
    reload_main_menu_panel,
    set_text_input,
    current_tick,
    register_sound,
    nullptr,
    set_main_menu_overlay,
    nullptr,
    stop_sounds,
    play_sound_alternate,
    run_frontend
};

std::size_t call_index(const char* call) {
    for (std::size_t i = 0; i < g_calls.size(); ++i) {
        if (g_calls[i] == call)
            return i;
    }
    return g_calls.size();
}

bool called_in_order(const char* first, const char* second) {
    const std::size_t a = call_index(first);
    const std::size_t b = call_index(second);
    return a < g_calls.size() && b < g_calls.size() && a < b;
}

std::size_t overlay_pixels(const OverlayDesc& overlay, ScreenContext& ctx) {
    auto& surface = *ctx.surface;
    surface.rgb.assign(640U * 480U * 3U, 0x55);
    overlay.draw(&ctx, overlay.state);
    std::size_t painted = 0;
    for (std::size_t i = 378U * 640U * 3U; i < surface.rgb.size(); ++i)
        painted += surface.rgb[i] != 0x55 ? 1U : 0U;
    return painted;
}

bool menu_area_untouched(const oa::ui::frontend_renderer::Surface& surface) {
    for (std::size_t i = 0; i < 378U * 640U * 3U; ++i) {
        if (surface.rgb[i] != 0x55)
            return false;
    }
    return true;
}

int send_input(const OverlayDesc& overlay, ScreenContext& ctx, const ScreenInput& input) {
    ctx.input = &input;
    const int consumed = overlay.event(&ctx, overlay.state);
    ctx.input = nullptr;
    return consumed;
}

int send(
    const OverlayDesc& overlay,
    ScreenContext& ctx,
    ScreenInputKind kind,
    float x,
    float y,
    std::uint32_t key
) {
    ScreenInput input{};
    input.kind = kind;
    input.button = 1;
    input.x = x;
    input.y = y;
    input.key = key;
    return send_input(overlay, ctx, input);
}

int type_text(const OverlayDesc& overlay, ScreenContext& ctx, const char* text) {
    ScreenInput input{};
    input.kind = ScreenInputKind::text;
    input.text = text;
    return send_input(overlay, ctx, input);
}

// A key pressed and let go, as SDL reports it.
void press(const OverlayDesc& overlay, ScreenContext& ctx, std::uint32_t key) {
    send(overlay, ctx, ScreenInputKind::key_down, 0, 0, key);
    send(overlay, ctx, ScreenInputKind::key_up, 0, 0, key);
}

// One idle pass on the next clock tick.
void tick(const OverlayDesc& overlay, ScreenContext& ctx) {
    ++g_tick;
    overlay.tick(&ctx, overlay.state);
}

void click(const OverlayDesc& overlay, ScreenContext& ctx, float x, float y) {
    send(overlay, ctx, ScreenInputKind::pointer_down, x, y, 0);
    send(overlay, ctx, ScreenInputKind::pointer_up, x, y, 0);
}

int pointer_input(
    const OverlayDesc& overlay,
    ScreenContext& ctx,
    ScreenInputKind kind,
    float x,
    float y,
    std::uint8_t button,
    std::uint8_t clicks
) {
    ScreenInput input{};
    input.kind = kind;
    input.button = button;
    input.clicks = clicks;
    input.x = x;
    input.y = y;
    return send_input(overlay, ctx, input);
}

// A press of `button` that SDL counts as the `clicks`th of a run, then its
// release; the number of the two the overlay consumed.
int click_run(
    const OverlayDesc& overlay,
    ScreenContext& ctx,
    float x,
    float y,
    std::uint8_t button,
    std::uint8_t clicks
) {
    return pointer_input(overlay, ctx, ScreenInputKind::pointer_down, x, y, button, clicks) +
           pointer_input(overlay, ctx, ScreenInputKind::pointer_up, x, y, button, clicks);
}

void write_frame(const char* name, const oa::ui::frontend_renderer::Surface& surface) {
    const auto dir_value = oa::platform::environment_value("OA_SNAPSHOT_DIR");
    const char* dir = dir_value ? dir_value->c_str() : nullptr;
    if (dir == nullptr)
        return;
    std::ofstream out(std::filesystem::path(dir) / name, std::ios::binary);
    out << "P6\n" << surface.width << ' ' << surface.height << "\n255\n";
    out.write(
        reinterpret_cast<const char*>(surface.rgb.data()),
        static_cast<std::streamsize>(surface.rgb.size())
    );
}

// Fake Rover module: activation succeeds, the pump and login are recorded.
struct FakeRover {
    int pumps = 0;
    int deactivates = 0;
    std::string login_user;
    std::string login_password;
};

FakeRover g_rover;
RoverInterface g_table;

bool ROVER_CALL fake_attach() {
    return true;
}

void ROVER_CALL fake_detach() {
}

bool ROVER_CALL fake_activate() {
    return true;
}

void ROVER_CALL fake_deactivate() {
    ++g_rover.deactivates;
}

void ROVER_CALL fake_pump_events() {
    ++g_rover.pumps;
}

bool ROVER_CALL fake_start_login(
    const char*,
    const char* username,
    const char* password,
    RoverLoginProgressFn,
    RoverSystemAlertFn,
    RoverDisconnectFn
) {
    g_rover.login_user = username;
    g_rover.login_password = password;
    return true;
}

void ROVER_CALL fake_find_current_area(const char** out) {
    *out = "CorePrime Alpha Test";
}

void ROVER_CALL fake_get_interface(const RoverInterface** out) {
    *out = &g_table;
}

void install_fake_rover(const std::filesystem::path& root) {
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table = *real;
    g_table.attach = fake_attach;
    g_table.detach = fake_detach;
    g_table.activate = fake_activate;
    g_table.deactivate = fake_deactivate;
    g_table.pump_events = fake_pump_events;
    g_table.start_login = fake_start_login;
    g_table.find_current_area = fake_find_current_area;
    by::rover_module_set_source(fake_get_interface);
    by::rover_session_configure(root.string().c_str(), "");
}

// An RGB colour the frontend palette does not contain.
void pick_sentinel(const oa::PaletteBytes& palette, std::uint8_t out[3]) {
    for (int value = 0; value < 0x1000000; value += 0x010203) {
        const std::uint8_t rgb[3] = {
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value)
        };
        bool used = false;
        for (std::size_t i = 0; i < OA_PALETTE_COLORS && !used; ++i) {
            const std::uint8_t* entry = palette.data() + i * oa::palette_entry_bytes;
            used = entry[0] == rgb[0] && entry[1] == rgb[1] && entry[2] == rgb[2];
        }
        if (!used) {
            std::memcpy(out, rgb, 3);
            return;
        }
    }
}

std::size_t
sentinel_pixels(const oa::ui::frontend_renderer::Surface& surface, const std::uint8_t rgb[3]) {
    std::size_t left = 0;
    for (std::size_t i = 0; i + 2 < surface.rgb.size(); i += 3) {
        if (surface.rgb[i] == rgb[0] && surface.rgb[i + 1] == rgb[1] &&
            surface.rgb[i + 2] == rgb[2])
            ++left;
    }
    return left;
}

console::MainFrame* main_frame() {
    console::StatusWidget* widget = boneyards_console_status_widget();
    return widget != nullptr ? widget->system.main_frame : nullptr;
}

bool key_held(int32_t key) {
    console::ScopedServices scope(boneyards_console_status_widget());
    return by::input_key_down(key);
}

// What the probe window heard: its children's notifications, and the button
// messages and moves that reached it directly with their held buttons.
std::vector<by::NotifyCode> g_notified;
std::vector<by::MessageCode> g_heard;
std::vector<std::uint32_t> g_heard_buttons;
by::Window* g_probe_caption = nullptr;

std::size_t changes_notified() {
    std::size_t count = 0;
    for (const by::NotifyCode code : g_notified)
        count += code == by::NotifyCode::changed ? 1U : 0U;
    return count;
}

void probe_notify(by::Window*, const by::Notify* notify) {
    g_notified.push_back(notify->code);
    const std::string text = "Changed " + std::to_string(changes_notified());
    by::screens::lobby_label_set_text(g_probe_caption, text.c_str());
}

bool probe_heard(by::Window*, const by::Message* msg) {
    g_heard.push_back(msg->code);
    g_heard_buttons.push_back(msg->buttons);
    return true;
}

void draw_frame(const OverlayDesc& overlay, ScreenContext& ctx, const char* name) {
    overlay.draw(&ctx, overlay.state);
    write_frame(name, *ctx.surface);
}

// SDL counts the presses of a run; the Boneyards windows hear every second
// one as a double click, in place of its down, as Windows reports them. A
// row label reports a double click as `changed`, which a list that forwards
// its changes passes to its parent, as the Chat console's frequency
// list does for its notify handler. The game's window procedure queues only
// the left and right button messages, and a queued record reaches the
// windows with its held-button word cleared;
// a move carries the pointer message's held-button set, the middle button's included.
void test_double_clicks(const OverlayDesc& overlay, ScreenContext& ctx) {
    constexpr std::uint8_t k_left = 1;
    constexpr std::uint8_t k_middle = 2;
    constexpr std::uint8_t k_right = 3;
    constexpr std::uint8_t k_x1 = 4;
    console::StatusWidget* widget = boneyards_console_status_widget();
    by::Window* focus = widget->desktop.focus;
    by::WindowOps probe_ops = by::k_window_ops;
    probe_ops.draw = by::window_draw_background;
    probe_ops.on_notify = probe_notify;
    probe_ops.mouse_move = probe_heard;
    probe_ops.left_down = probe_heard;
    probe_ops.left_up = probe_heard;
    probe_ops.left_double = probe_heard;
    probe_ops.right_down = probe_heard;
    probe_ops.right_up = probe_heard;
    probe_ops.right_double = probe_heard;
    probe_ops.middle_down = probe_heard;
    probe_ops.middle_up = probe_heard;
    probe_ops.middle_double = probe_heard;
    by::Window* probe = nullptr;
    by::screens::ByListBox* list = nullptr;
    by::screens::ByTextRow* row = nullptr;
    {
        console::ScopedServices scope(widget);
        probe = by::window_create_plain();
        probe->ops = &probe_ops;
        by::window_create(probe, "DoubleClickProbe", 0x100, 0x80, 0xC0, 0x80, nullptr, true);
        by::window_set_color(probe, 0x10, 0x18, 0x10, 0xFF);
        const by::screens::HatFonts* fonts = &widget->system.hat_fonts;
        g_probe_caption = by::screens::lobby_label_create(
            probe, 8, 0x68, "Changed 0", fonts->small, 3, 0, 0x80, 0x11
        );
        list = by::screens::by_list_box_create(true, true);
        by::window_set_bounds(&list->list.base, 0x108, 0x88, 0x80, 0x58, probe, true);
        row = by::screens::by_text_row_create(list, "Frequency", fonts, true);
        by::desktop_add(&widget->desktop, probe);
    }
    // The row's label is the window under the pointer.
    const by::Window* label = row->label;
    const float row_x = static_cast<float>(label->x + label->width / 2);
    const float row_y = static_cast<float>(label->y + label->height / 2);

    g_notified.clear();
    click_run(overlay, ctx, row_x, row_y, k_left, 1);
    check(changes_notified() == 0, "a single click on a row reports no change");
    check(list->selected_item == &row->base, "a single click selects the row");
    draw_frame(overlay, ctx, "double_click_before.ppm");
    click_run(overlay, ctx, row_x, row_y, k_left, 2);
    check(changes_notified() == 1, "a double click on a row reaches the list's parent as changed");
    draw_frame(overlay, ctx, "double_click_after.ppm");
    click_run(overlay, ctx, row_x, row_y, k_left, 3);
    check(changes_notified() == 1, "the third press of a run is a single click");
    click_run(overlay, ctx, row_x, row_y, k_left, 4);
    check(changes_notified() == 2, "the fourth press of a run is a double click again");
    click_run(overlay, ctx, row_x, row_y, k_right, 2);
    check(changes_notified() == 2, "a row label ignores a right double click");
    click_run(overlay, ctx, row_x, row_y, k_middle, 2);
    click_run(overlay, ctx, row_x, row_y, k_x1, 2);
    check(changes_notified() == 2, "middle and extra buttons never reach a row label");

    // On the probe itself, clear of the list.
    const float x = 0x1A0;
    const float y = 0xF0;
    g_heard.clear();
    g_heard_buttons.clear();
    for (std::uint8_t clicks = 1; clicks <= 3; ++clicks)
        click_run(overlay, ctx, x, y, k_left, clicks);
    click_run(overlay, ctx, x, y, k_right, 1);
    click_run(overlay, ctx, x, y, k_right, 2);
    const std::vector<by::MessageCode> clicked = {
        by::MessageCode::left_down,
        by::MessageCode::left_up,
        by::MessageCode::left_double,
        by::MessageCode::left_up,
        by::MessageCode::left_down,
        by::MessageCode::left_up,
        by::MessageCode::right_down,
        by::MessageCode::right_up,
        by::MessageCode::right_double,
        by::MessageCode::right_up,
    };
    check(g_heard == clicked, "left and right double clicks replace the second down");
    check(
        g_heard_buttons == std::vector<std::uint32_t>(clicked.size(), 0),
        "button messages carry no held buttons"
    );

    g_heard.clear();
    g_heard_buttons.clear();
    check(
        click_run(overlay, ctx, x, y, k_middle, 1) + click_run(overlay, ctx, x, y, k_middle, 2) ==
            4,
        "the Boneyards take the middle button's presses"
    );
    check(
        click_run(overlay, ctx, x, y, k_x1, 1) == 2, "the Boneyards take an extra button's press"
    );
    check(g_heard.empty(), "no window hears the middle or an extra button");

    pointer_input(overlay, ctx, ScreenInputKind::pointer_down, x, y, k_middle, 1);
    pointer_input(overlay, ctx, ScreenInputKind::pointer_move, x + 1, y, 0, 0);
    pointer_input(overlay, ctx, ScreenInputKind::pointer_up, x + 1, y, k_middle, 1);
    pointer_input(overlay, ctx, ScreenInputKind::pointer_down, x + 1, y, k_left, 1);
    pointer_input(overlay, ctx, ScreenInputKind::pointer_move, x, y, 0, 0);
    pointer_input(overlay, ctx, ScreenInputKind::pointer_up, x, y, k_left, 1);
    pointer_input(overlay, ctx, ScreenInputKind::pointer_move, x + 1, y, 0, 0);
    const std::vector<by::MessageCode> dragged = {
        by::MessageCode::mouse_move,
        by::MessageCode::left_down,
        by::MessageCode::mouse_move,
        by::MessageCode::left_up,
        by::MessageCode::mouse_move,
    };
    check(g_heard == dragged, "a move with the middle button held still reaches the window");
    const std::vector<std::uint32_t> held = {by::button_middle, 0, by::button_left, 0, 0};
    check(g_heard_buttons == held, "moves carry the held buttons, the middle one included");

    {
        console::ScopedServices scope(widget);
        by::desktop_set_focus(&widget->desktop, focus);
        by::desktop_remove(&widget->desktop, probe);
        by::window_destroy(probe);
    }
    g_probe_caption = nullptr;
}

// Enter Boneyards in state 7, the clock, typing into the login console and
// the Main Frame's exit back to state 2.
void test_boneyards_state(const OverlayDesc& overlay, ScreenContext& ctx, oa::AssetStore& assets) {
    g_calls.clear();
    g_signal = -1;
    g_registered_sounds.clear();
    g_audio.clear();
    check(!boneyards_console_up(), "the Boneyards is not up on the main menu");
    check(send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 'b') == 1, "B enters the Boneyards");
    check(send(overlay, ctx, ScreenInputKind::key_up, 0, 0, 'b') == 1, "B release consumed");
    check(called_in_order("state 7", "close"), "state 7, then the main menu comes down");
    check(
        call_index("text 1") == g_calls.size(), "entering the Boneyards leaves the keyboard closed"
    );
    check(call_index("reload") == g_calls.size(), "main menu not reloaded on entry");
    check(g_signal == 1, "entry makes the update signal current");
    check(main_frame() != nullptr, "Main Frame opened");
    // The Main Frame build ends by caching eight sound names with their WAVs.
    const std::vector<std::string> console_sounds = {
        "PROMOTED=cdoggy",
        "GALACTIC_BUTTON=button4",
        "PLANET_BUTTON=butmain",
        "GREEN_BUTTON=beep2",
        "INFO_PANE_OPEN=button11",
        "INFO_PANE_CLOSE=cancel1",
        "GREEN_SLIDE=hoverok1",
        "MESSAGE=hovsmsl1",
    };
    check(g_registered_sounds == console_sounds, "the Main Frame registers its eight sounds");
    // Then it stops every sound and drones BGM on the alternate route
    // through the services the overlay kept.
    check(
        g_audio == std::vector<std::string>({"stop", "alt BGM"}),
        "the Main Frame build stops the sounds and drones BGM"
    );
    check(boneyards_console_up(), "the Boneyards is up");
    g_audio.clear();
    boneyards_console_stop_sounds();
    check(g_audio == std::vector<std::string>({"stop"}), "the console stops the sounds on request");
    // A request to close the window opens the "Exit to the system?" box, which
    // takes the input until it is answered: NO stays, YES has the next idle
    // work leave the game through the application's bridge.
    static int leaves = 0;
    by::console::LaunchBridge bridge{};
    bridge.leave_game = [](void*) { ++leaves; };
    boneyards_console_set_launch_bridge(bridge);
    for (const char* answer : {by::message_box_name::no, by::message_box_name::yes}) {
        check(boneyards_console_ask_exit(), "the exit box opens");
        console::StatusWidget* widget = boneyards_console_status_widget();
        by::MessageBox* box = by::message_box_find_open(&widget->desktop);
        by::Window* button =
            box != nullptr ? by::window_find_named(&box->base, answer, nullptr) : nullptr;
        check(button != nullptr, "the exit box shows YES and NO");
        if (button == nullptr)
            break;
        check(
            send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 'b') == 1, "the box takes the keys"
        );
        click(
            overlay,
            ctx,
            static_cast<float>(button->x + button->width / 2),
            static_cast<float>(button->y + button->height / 2)
        );
        tick(overlay, ctx);
        check(!console::boneyards_status_widget_message_open(widget), "an answer closes the box");
        boneyards_console_idle(900);
        const bool yes = std::strcmp(answer, by::message_box_name::yes) == 0;
        check(leaves == (yes ? 1 : 0), "only YES leaves the game");
    }
    boneyards_console_set_launch_bridge(by::console::LaunchBridge{});

    // Every pixel of the frame belongs to the Boneyards desktop.
    const auto decoded_frontend = oa::decode_pcx(assets.read("bitmaps/frontendx.pcx").bytes);
    check(decoded_frontend.ok(), "frontendx.pcx decodes");
    const oa::Image frontend = decoded_frontend.ok() ? *decoded_frontend.value : oa::Image{};
    std::uint8_t sentinel[3]{};
    if (frontend.palette)
        pick_sentinel(*frontend.palette, sentinel);
    auto& surface = *ctx.surface;
    for (std::size_t i = 0; i + 2 < surface.rgb.size(); i += 3)
        std::memcpy(&surface.rgb[i], sentinel, 3);
    overlay.draw(&ctx, overlay.state);
    check(sentinel_pixels(surface, sentinel) == 0, "state 7 draws every pixel");
    write_frame("boneyards_state7.ppm", surface);

    // Anything the Boneyards windows ignore is still kept from the main menu.
    check(
        send(overlay, ctx, ScreenInputKind::pointer_down, 5, 470, 0) == 1 &&
            send(overlay, ctx, ScreenInputKind::pointer_up, 5, 470, 0) == 1,
        "state 7 keeps clicks from the main menu"
    );
    check(send(overlay, ctx, ScreenInputKind::wheel, 300, 300, 0) == 1, "state 7 keeps the wheel");

    // The windows step once per clock tick however often the tick runs;
    // the client pump runs on every call.
    const int pumps = g_rover.pumps;
    const int32_t countdown = main_frame()->login_countdown;
    g_tick = 1000;
    for (int i = 0; i < 20; ++i)
        overlay.tick(&ctx, overlay.state);
    check(main_frame()->login_countdown == countdown - 1, "one step per clock tick");
    check(g_rover.pumps == pumps + 20, "the client pumps every call");
    g_tick += 5;
    overlay.tick(&ctx, overlay.state);
    overlay.tick(&ctx, overlay.state);
    check(main_frame()->login_countdown == countdown - 2, "a clock jump steps once");
    // The idle work of each frame pumps the client unless the overlay's tick
    // already did, so a frame pumps it once whatever screen is up.
    const int ticked_pumps = g_rover.pumps;
    boneyards_console_idle(1000);
    check(g_rover.pumps == ticked_pumps, "a frame the overlay ticked pumps once");
    boneyards_console_idle(1033);
    boneyards_console_idle(1066);
    check(g_rover.pumps == ticked_pumps + 2, "frames without the overlay pump from the idle work");

    // The update console's placeholder pass is about two seconds of the 30 Hz
    // clock, then the login console opens.
    for (int i = 0; i < 160 && main_frame()->console_screen != console::console_screen::login; ++i)
        tick(overlay, ctx);
    check(main_frame()->console_screen == console::console_screen::login, "login console opens");
    check(call_index("text 1") < g_calls.size(), "the login field opens the on-screen keyboard");
    test_double_clicks(overlay, ctx);
    const by::screens::LoginDialog& login = boneyards_console_status_widget()->navigation->login;
    auto step = [&]() { tick(overlay, ctx); };

    // Typed characters keep their case and shifted symbols; keycodes alone
    // insert nothing. Every character of a typed string is kept, and
    // backspace removes one character.
    check(type_text(overlay, ctx, "Secret_1") == 1, "text consumed");
    step();
    check(std::strcmp(login.name, "Secret_1") == 0, "typed name keeps case and symbols");
    press(overlay, ctx, 's');
    step();
    check(std::strcmp(login.name, "Secret_1") == 0, "a keycode alone inserts nothing");
    press(overlay, ctx, '\b');
    step();
    check(std::strcmp(login.name, "Secret_") == 0, "backspace erases");
    type_text(overlay, ctx, "1\xC2\xA3\xE2\x82\xAC");
    step();
    check(
        std::strcmp(login.name, "Secret_1\xC2\xA3\xE2\x82\xAC") == 0,
        "typed name keeps every character"
    );
    press(overlay, ctx, '\b');
    step();
    check(std::strcmp(login.name, "Secret_1\xC2\xA3") == 0, "backspace removes one character");
    press(overlay, ctx, '\b');
    step();
    check(std::strcmp(login.name, "Secret_1") == 0, "backspace removes the previous character");
    console::StatusWidget* widget = boneyards_console_status_widget();
    by::Window* name_field = widget->desktop.focus;
    auto* name_edit = reinterpret_cast<by::SingleEdit*>(name_field);
    check(
        std::strcmp(
            "Grüße",
            "Gr\xC3\xBC\xC3\x9F"
            "e"
        ) == 0,
        "the source text is intact"
    );
    by::single_edit_set_text(name_edit, "");
    step();
    check(type_text(overlay, ctx, "Grüße") == 1, "Grüße consumed");
    step();
    check(
        std::strcmp(
            login.name,
            "Gr\xC3\xBC\xC3\x9F"
            "e"
        ) == 0 &&
            name_edit->length == 5,
        "Grüße stored whole"
    );
    press(overlay, ctx, '\b');
    step();
    check(
        std::strcmp(login.name, "Gr\xC3\xBC\xC3\x9F") == 0 && name_edit->length == 4,
        "backspace removes one character of Grüße"
    );
    console::boneyards_status_widget_set_composition(widget, "e");
    check(std::strcmp(by::single_edit_composition(name_edit), "e") == 0, "composition is shown");
    check(
        std::strcmp(
            by::single_edit_shown(name_edit),
            "Gr\xC3\xBC\xC3\x9F"
            "e"
        ) == 0,
        "composition sits in the field"
    );
    check(
        std::strcmp(by::single_edit_text(name_edit), "Gr\xC3\xBC\xC3\x9F") == 0,
        "composition is not part of the text"
    );
    step();
    check(
        std::strcmp(login.name, "Gr\xC3\xBC\xC3\x9F") == 0, "composition is not committed by a tick"
    );
    type_text(overlay, ctx, "e");
    step();
    check(
        std::strcmp(
            login.name,
            "Gr\xC3\xBC\xC3\x9F"
            "e"
        ) == 0,
        "committed character is stored"
    );
    check(by::single_edit_composition(name_edit)[0] == '\0', "committing clears the composition");
    by::single_edit_set_text(name_edit, "");
    step();
    check(type_text(overlay, ctx, "Привет") == 1, "Привет consumed");
    step();
    check(
        std::strcmp(login.name, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == 0 &&
            name_edit->length == 6,
        "Привет stored whole"
    );
    press(overlay, ctx, '\b');
    step();
    check(
        std::strcmp(login.name, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5") == 0 &&
            name_edit->length == 5,
        "backspace removes one character of Привет"
    );
    by::single_edit_set_text(name_edit, "");
    step();
    check(type_text(overlay, ctx, "東京") == 1, "東京 consumed");
    step();
    check(
        std::strcmp(login.name, "\xE6\x9D\xB1\xE4\xBA\xAC") == 0 && name_edit->length == 2,
        "東京 stored whole"
    );
    press(overlay, ctx, 0x40000050U);
    press(overlay, ctx, '\b');
    step();
    check(
        std::strcmp(login.name, "\xE4\xBA\xAC") == 0 && name_edit->length == 1,
        "backspace removes one character of 東京"
    );
    by::single_edit_set_text(name_edit, "Secret_1");
    step();
    check(std::strcmp(login.name, "Secret_1") == 0, "the login name is restored");
    press(overlay, ctx, '\t');
    check(widget->desktop.focus != name_field, "tab moves the focus on");
    for (int32_t i = 0; i < name_field->parent->children.count; ++i) {
        by::Window* child = name_field->parent->children.items[i];
        if (child->ops->is_class(by::class_name::radio_button) &&
            child->y == name_field->parent->y + 0x5c)
            click(
                overlay,
                ctx,
                static_cast<float>(child->x + child->width / 2),
                static_cast<float>(child->y + child->height / 2)
            );
    }
    by::Window* password_field = nullptr;
    for (int32_t i = 0; name_field != nullptr && i < name_field->parent->children.count; ++i) {
        by::Window* child = name_field->parent->children.items[i];
        if (child != name_field && child->ops == name_field->ops)
            password_field = child;
    }
    check(password_field != nullptr, "login console has a password field");
    if (password_field == nullptr)
        return;
    click(
        overlay, ctx, static_cast<float>(name_field->x + 4), static_cast<float>(name_field->y + 4)
    );
    check(widget->desktop.focus == name_field, "click focuses username");
    press(overlay, ctx, '\t');
    check(widget->desktop.focus == password_field, "Tab selects the next entry");
    send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 0x400000E1U);
    press(overlay, ctx, '\t');
    send(overlay, ctx, ScreenInputKind::key_up, 0, 0, 0x400000E1U);
    check(widget->desktop.focus == name_field, "Shift+Tab selects the previous entry");
    press(overlay, ctx, '\t');
    check(widget->desktop.focus == password_field, "Tab selects the password again");
    {
        // The gamepad's previous shoulder arrives as Tab with the Shift bit set,
        // and no separate Shift key.
        ScreenInput input{};
        input.kind = ScreenInputKind::key_down;
        input.key = '\t';
        input.modifiers = 0x0003;
        send_input(overlay, ctx, input);
        input.kind = ScreenInputKind::key_up;
        send_input(overlay, ctx, input);
    }
    check(widget->desktop.focus == name_field, "Tab with the Shift modifier moves backward");
    send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 0x400000E2U);
    check(key_held(by::key::alt), "Alt reaches the toolkit");
    send(overlay, ctx, ScreenInputKind::key_up, 0, 0, 0x400000E2U);
    check(!key_held(by::key::alt), "letting Alt go clears it");
    send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 0x400000E3U);
    check(key_held(by::key::system), "the system key reaches the toolkit");
    send(overlay, ctx, ScreenInputKind::key_up, 0, 0, 0x400000E3U);
    check(!key_held(by::key::system), "letting the system key go clears it");
    {
        // Left with Ctrl, Alt and the system bit moves by word, then the bits
        // are released. The name is Secret_1, so the caret returns to its start.
        const int32_t caret = name_edit->caret;
        name_edit->caret = name_edit->length;
        ScreenInput input{};
        input.kind = ScreenInputKind::key_down;
        input.key = 0x40000050U;
        input.modifiers = oa::app::kInputModifierCtrl | oa::app::kInputModifierAlt |
                          oa::app::kInputModifierSystem;
        send_input(overlay, ctx, input);
        check(name_edit->caret == 0, "Ctrl+Left moves to the start of the word");
        input.kind = ScreenInputKind::key_up;
        input.modifiers = 0;
        send_input(overlay, ctx, input);
        name_edit->caret = caret;
    }
    check(
        !key_held(by::key::control) && !key_held(by::key::alt) && !key_held(by::key::system),
        "Ctrl, Alt and the system modifier do not stay held"
    );
    press(overlay, ctx, '\t');
    type_text(overlay, ctx, "Pa55word!");
    step();
    check(std::strcmp(login.password, "Pa55word!") == 0, "typed password keeps case and symbols");
    press(overlay, ctx, '\r');
    check(g_rover.login_user == "Secret_1", "enter logs in with the typed name");
    check(g_rover.login_password == "Pa55word!", "enter logs in with the typed password");
    bool held = false;
    for (int32_t key = 0; key < 0x200; ++key)
        held = held || key_held(key);
    check(!held, "no key stays held");

    // The Main Frame's exit reloads the main menu, then returns to state 2.
    g_calls.clear();
    const int deactivates = g_rover.deactivates;
    click(overlay, ctx, 19 + 22, 46 + 14);
    check(called_in_order("reload", "state 2"), "exit reloads the main menu, then state 2");
    // Pressing exit leaves the text field first, which closes the keyboard,
    // and leaving the Boneyards closes it again after the state change.
    bool text_stopped = false;
    const std::size_t state2 = call_index("state 2");
    for (std::size_t i = state2 + 1; i < g_calls.size(); ++i)
        text_stopped = text_stopped || g_calls[i] == "text 0";
    check(text_stopped, "text input stops in state 2");
    check(g_rover.deactivates == deactivates + 1, "exit releases the client");
    tick(overlay, ctx);
    check(overlay_pixels(overlay, ctx) > 640U * 90U, "overlay strip shown again");
    check(menu_area_untouched(surface), "state 2 draws only the strip");
    check(send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 's') == 0, "state 2 passes TA keys");
    check(type_text(overlay, ctx, "x") == 0, "state 2 ignores text");
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the Boneyards main-menu overlay");
    oa::AssetStore assets(root);
    for (const char* archive : {"by.ccx", "rev31.gp3", "totala1.hpi"}) {
        if (std::filesystem::exists(root / archive))
            assets.mount(root / archive);
    }

    ScreenRegistry registry{};
    register_boneyards_console_screens(&registry);
    check(registry.overlay_count == 1 && registry.rejected == nullptr, "overlay registered");
    bool return_step = false;
    bool entry_step = false;
    for (uint32_t i = 0; i < registry.step_count; ++i) {
        return_step = return_step || static_cast<uint32_t>(registry.steps[i].step) == 0x109;
        entry_step = entry_step || static_cast<uint32_t>(registry.steps[i].step) == 0x10b;
    }
    check(return_step, "the return to the Boneyards is a dispatcher step");
    check(entry_step, "the entry \"-y\" takes is a dispatcher step");
    const OverlayDesc& overlay = registry.overlays[0];
    check(overlay.screen == 0, "overlay targets the main menu");

    oa::ui::frontend_renderer::Surface surface;
    surface.width = 640;
    surface.height = 480;
    surface.rgb.assign(640U * 480U * 3U, 0x55);
    ScreenContext ctx{nullptr, &k_services, &assets, &surface, nullptr, nullptr, 0};
    overlay.create(&ctx, overlay.state);
    check(g_main_menu_overlay == 1, "the overlay reports itself over the main menu");
    check(
        boneyards_console_status_widget() != nullptr &&
            console::main_menu_overlay_host_connected(
                boneyards_console_status_widget()->config.host
            ),
        "every host entry the overlay calls is set"
    );
    {
        const console::MainMenuHost& menu = boneyards_console_status_widget()->config.host;
        check(menu.find_disc == nullptr, "the overlay does not ask whether a disc is in a drive");
        check(menu.select_cursor == nullptr, "the overlay does not select a cursor");
        check(menu.application_flags == nullptr, "the overlay does not read the display mode");
        check(menu.shift_key_down == nullptr, "the overlay does not read the shift key");
        check(menu.store_intro_shift == nullptr, "the overlay does not record a movie skip");
        check(menu.drain_input == nullptr, "the overlay does not drain input");
    }
    tick(overlay, ctx);
    overlay.draw(&ctx, overlay.state);

    check(menu_area_untouched(surface), "menu above the overlay untouched");
    std::size_t painted = 0;
    for (std::size_t i = 378U * 640U * 3U; i < surface.rgb.size(); ++i)
        painted += surface.rgb[i] != 0x55 ? 1U : 0U;
    check(painted > 640U * 90U, "overlay painted across the bottom");

    // The strip's buttons answer the click. The part of the old column the
    // strip does not cover is the same button. A click on the plaque is not.
    const auto menu_click = [&](float x, float y, const char* what) {
        const int before = g_frontend_passes;
        check(
            send(overlay, ctx, ScreenInputKind::pointer_down, x, y, 0) == 1 &&
                send(overlay, ctx, ScreenInputKind::pointer_up, x, y, 0) == 1,
            what
        );
        check(g_frontend_passes == before + 1, "the click asks for a menu pass");
        tick(overlay, ctx);
        check(g_frontend_passes == before + 2, "the next tick asks for the pass that opens");
    };
    menu_click(100.0F, 400.0F, "SINGLE is taken");
    check(g_sound == "BigButton", "SINGLE plays the big button");
    check(g_signal == 5, "SINGLE asks for single player");
    check(call_index("close") < g_calls.size(), "SINGLE closes the main-menu panel");
    menu_click(130.0F, 440.0F, "MULTI is taken");
    check(g_sound == "BigButton", "MULTI plays the big button");
    check(g_signal == 6, "MULTI asks for SELECT CONNECTION");
    check(
        !console::boneyards_status_widget_message_open(boneyards_console_status_widget()),
        "MULTI opens no Boneyards box when the multiplayer file is there"
    );
    menu_click(512.0F, 403.0F, "INTRO is taken");
    check(g_sound == "smlButton", "INTRO plays the small button");
    check(call_index("state 1") < g_calls.size(), "INTRO asks for the movie");
    menu_click(185.0F, 438.0F, "the old MULTI column is taken");
    check(g_sound == "BigButton" && g_signal == 6, "the old MULTI column is MULTI");
    menu_click(200.0F, 403.0F, "the old SINGLE column is taken");
    check(g_sound == "BigButton" && g_signal == 5, "the old SINGLE column is SINGLE");
    const int passes = g_frontend_passes;
    const std::size_t calls = g_calls.size();
    const std::string sound = g_sound;
    const int signal = g_signal;
    check(
        send(overlay, ctx, ScreenInputKind::pointer_down, 40, 400, 0) == 0 &&
            send(overlay, ctx, ScreenInputKind::pointer_up, 40, 400, 0) == 0,
        "the plaque is not a button"
    );
    check(
        g_frontend_passes == passes && g_calls.size() == calls && g_sound == sound &&
            g_signal == signal,
        "the plaque changes nothing"
    );
    g_calls.clear();
    g_sound.clear();
    g_signal = -1;

    // Enter Boneyards offline opens a Boneyards message box that takes all
    // input until Enter dismisses it.
    const std::vector<std::uint8_t> before = surface.rgb;
    check(
        send(overlay, ctx, ScreenInputKind::pointer_down, 282, 426, 0) == 1, "enter press consumed"
    );
    check(
        send(overlay, ctx, ScreenInputKind::pointer_up, 282, 426, 0) == 1, "enter release consumed"
    );
    check(g_sound == "BigButton", "enter sound");
    check(g_status.empty(), "no status line message");
    check(call_index("close") == g_calls.size(), "offline entry leaves the main menu up");
    overlay.draw(&ctx, overlay.state);
    std::size_t box_pixels = 0;
    for (int y = 0xAC; y < 0xAC + 0x88; ++y) {
        for (int x = 0x8E; x < 0x8E + 0xD8; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 640U + x) * 3U;
            box_pixels += surface.rgb[i] != before[i] ? 1U : 0U;
        }
    }
    check(box_pixels > 0xD8 * 0x88 / 2, "message box drawn over the menu");
    check(send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 's') == 1, "box takes keys");
    check(send(overlay, ctx, ScreenInputKind::pointer_down, 10, 10, 0) == 1, "box takes clicks");
    check(send(overlay, ctx, ScreenInputKind::key_down, 0, 0, '\r') == 1, "enter dismisses");
    tick(overlay, ctx);
    check(
        send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 's') == 0, "TA quick keys pass through"
    );
    check(send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 'b') == 1, "B consumed");
    check(send(overlay, ctx, ScreenInputKind::key_down, 0, 0, 's') == 1, "B shows the box again");
    check(send(overlay, ctx, ScreenInputKind::key_down, 0, 0, '\r') == 1, "enter dismisses again");
    tick(overlay, ctx);

    // Leaving the main menu releases the Boneyards GUI; coming back shows the
    // overlay again and makes the update signal current.
    const StepDesc* leave = step_find(&registry, oa::ui::frontend_state::Step::shut_down_resource);
    const StepDesc* back = step_find(&registry, oa::ui::frontend_state::Step::return_to_main_menu);
    check(leave != nullptr && back != nullptr, "main-menu steps registered");
    // The return to the Boneyards from another screen asks for the main
    // menu, whose own return then takes the player there.
    if (const StepDesc* to_boneyards = step_find(&registry, oa::ui::frontend_state::Step{0x109})) {
        g_requested_screens.clear();
        g_calls.clear();
        ScreenContext elsewhere = ctx;
        elsewhere.screen = 0x102;
        to_boneyards->run(&elsewhere, to_boneyards->state);
        check(
            g_requested_screens == std::vector<ScreenId>({0}) && g_calls.empty(),
            "from another screen the step asks for the main menu"
        );
    }
    if (leave != nullptr && back != nullptr) {
        leave->run(&ctx, leave->state);
        check(overlay_pixels(overlay, ctx) == 0, "released overlay draws nothing");
        check(g_signal == -1, "leaving writes no signal");
        back->run(&ctx, back->state);
        check(overlay_pixels(overlay, ctx) > 640U * 90U, "overlay shown again");
        check(g_signal == 1, "returning makes the update signal current");
    }

    // The same overlay on the Central Command session, over a fake Rover.
    console::boneyards_status_widget_destroy(boneyards_console_status_widget());
    install_fake_rover(root);
    overlay.create(&ctx, overlay.state);
    check(boneyards_console_status_widget() != nullptr, "overlay created on the session");
    if (boneyards_console_status_widget() != nullptr)
        test_boneyards_state(overlay, ctx, assets);

    // A failed join while the game list is still showing asks for the main
    // menu, and that menu's next tick returns to the Boneyards with the box.
    if (console::StatusWidget* widget = boneyards_console_status_widget();
        widget != nullptr && back != nullptr) {
        widget->system.chat_console_pending = 1;
        widget->system.launched_area_id = 1800;
        std::snprintf(
            widget->system.launched_game_id,
            sizeof widget->system.launched_game_id,
            "%s",
            "9bfaa0d0f7b93f12"
        );
        console::boneyards_status_widget_leave_main_menu(widget);
        boneyards_console_show_notice(
            "Host not found - Host may have left the game before you arrived."
        );
        check(
            !console::boneyards_status_widget_message_open(widget),
            "the join failure waits until the console is up"
        );
        g_requested_screens.clear();
        g_calls.clear();
        ScreenContext game_list = ctx;
        game_list.screen = 0x102;
        back->run(&game_list, back->state);
        check(
            g_requested_screens == std::vector<ScreenId>({0}) && g_calls.empty(),
            "a launched game still on the game list asks for the main menu"
        );
        if (const StepDesc* to_boneyards =
                step_find(&registry, oa::ui::frontend_state::Step{0x109})) {
            g_requested_screens.clear();
            to_boneyards->run(&game_list, to_boneyards->state);
            check(
                g_requested_screens == std::vector<ScreenId>({0}),
                "the battle return from the game list asks for the main menu"
            );
        }
        g_launches_ended = 0;
        console::LaunchBridge bridge{};
        bridge.launch_ended = count_launch_ended;
        boneyards_console_set_launch_bridge(bridge);
        ctx.screen = 0;
        tick(overlay, ctx);
        check(call_index("state 7") < g_calls.size(), "the failed join returns to the Boneyards");
        check(
            !console::boneyards_status_widget_launched_game(widget),
            "the battle no longer counts as launched"
        );
        check(g_launches_ended == 1, "the return ends the launch");
        by::MessageBox* box = by::message_box_find_open(&widget->desktop);
        by::Window* text = box != nullptr
                               ? window_find_named(&box->base, by::message_box_name::text, nullptr)
                               : nullptr;
        std::string shown;
        if (text != nullptr && text->ops != nullptr && text->ops->state_text != nullptr) {
            console::ScopedServices scope(widget);
            shown = text->ops->state_text(text, -1);
        }
        check(
            shown == "Host not found - Host may have left the game before you arrived.",
            "the return opens the join failure's box"
        );
        boneyards_console_set_launch_bridge(console::LaunchBridge{});
    }
    by::rover_module_set_source(nullptr);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("console screen registration tests passed\n");
    return 0;
}
