// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Navigation between the main-menu overlay and the Boneyards Main Frame, and
// Main Frame layout/draw snapshots, against the BY1117a by.ccx data.
#include <algorithm>
#include <cstdint>
#include <cstdio>
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
#include "oa/platform/system.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/screens/update_pass.hpp"

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

struct NamedSound {
    std::string category;
    std::string file;
};

struct Log {
    std::vector<std::string> sounds;
    // "name CATEGORY" and "play NAME", in the order the host heard them.
    std::vector<std::string> sound_order;
    std::vector<NamedSound> named;
    std::vector<uint32_t> census;
    int32_t state = -1;
    int32_t signal = -1;
    int32_t panel_reloads = 0;
    int32_t closes = 0;
    std::vector<int32_t> screens;
    Window* last_screen = nullptr;
    std::vector<std::string> battle_lines;
};

Log g_log;

void host_sound(void*, const char* name) {
    g_log.sounds.emplace_back(name);
    g_log.sound_order.emplace_back(std::string("play ") + name);
}

void host_register_sound(void*, const char* category, const char* file) {
    g_log.named.push_back(
        NamedSound{category != nullptr ? category : "", file != nullptr ? file : ""}
    );
    g_log.sound_order.emplace_back(std::string("name ") + (category != nullptr ? category : ""));
}

void host_state(void*, uint8_t state) {
    g_log.state = state;
}

void host_signal(void*, uint8_t signal) {
    g_log.signal = signal;
}

void host_panel(void*) {
    ++g_log.panel_reloads;
}

void host_battle_line(void*, const char* text) {
    g_log.battle_lines.emplace_back(text);
}

bool service_network(void*) {
    return true;
}

ActivationResult service_activate(void*) {
    return ActivationResult::ok;
}

void service_close(void*) {
    ++g_log.closes;
}

// A session that already reads as connected must not skip the update and
// login consoles.
bool service_connected(void*) {
    return true;
}

void service_census(void*, uint32_t area) {
    g_log.census.push_back(area);
}

Window*
create_screen(void*, int32_t screen, Window* frame, int32_t x, int32_t y, int32_t w, int32_t h) {
    g_log.screens.push_back(screen);
    Window* window = window_create_plain();
    window_create(window, "TestScreen", x, y, w, h, frame, true);
    g_log.last_screen = window;
    return window;
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

uint64_t draw_hash(StatusWidget* widget, const char* name) {
    std::vector<uint8_t> pixels(640 * 480, 0);
    oa::Surface surface{};
    surface.width = 640;
    surface.height = 480;
    surface.pitch = 640;
    surface.pixels = pixels.data();
    surface.clip = oa::Rect32{0, 0, 639, 479};
    boneyards_status_widget_draw(widget, &surface);
    if (const auto dir = oa::platform::environment_value("OA_SNAPSHOT_DIR")) {
        std::ofstream out(std::filesystem::path(*dir) / name, std::ios::binary);
        out.write(
            reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size())
        );
    }
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (uint8_t b : pixels) {
        hash ^= b;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Runs the Main Frame until the green layer stops sliding; returns the
// cycles taken.
int32_t finish_slide(StatusWidget* widget) {
    const ConsoleLayer* layer = widget->system.main_frame->console;
    int32_t cycles = 0;
    while (layer->sliding != 0 && cycles < 100) {
        boneyards_status_widget_tick(widget);
        ++cycles;
    }
    return cycles;
}

void check_hash(uint64_t actual, uint64_t expected, const char* what) {
    if (actual != expected)
        std::fprintf(stderr, "%s: got %016llx\n", what, static_cast<unsigned long long>(actual));
    check(actual == expected, what);
}

bool at(const Window* w, int32_t x, int32_t y, int32_t width, int32_t height) {
    return w->x == x && w->y == y && w->width == width && w->height == height;
}

// The planet summary as the Main Frame builds it (battle_summary_create): eight labels
// and two SmallArmIcon icons at the frame's own coordinates, all hidden, and
// the picture area of the background (0x23A..0x26D x 0x7E..0x9C) filled
// with 0xAF by the frame's draw (battle_summary_draw). Texts and fonts need the
// toolkit and are checked in battle_summary_test.
void test_summary_built(StatusWidget* widget, const MainFrame* frame) {
    const BattleSummary* s = frame->battle_summary;
    check(s != nullptr && s->frame == frame, "the Main Frame holds its planet summary");
    if (s == nullptr)
        return;
    ScopedServices scope(widget);
    check(
        at(s->name, 0x211, 0x80, 0x3c, 9) && at(s->owner, 0x211, 0x8d, 0x3c, 9),
        "name and owner labels"
    );
    check(
        at(s->battles_caption, 0x211, 0x9e, 0x22, 9) && at(s->battles, 0x235, 0x9e, 0xf, 9) &&
            at(s->forming_caption, 0x211, 0xab, 0x22, 9) && at(s->forming, 0x235, 0xab, 0xf, 9),
        "Battles and Forming captions and counts"
    );
    check(
        at(s->arm_icon, 0x248, 0x9d, 0xb, 0xb) && at(s->core_icon, 0x248, 0xaa, 0xb, 0xb) &&
            window_is_named(s->arm_icon, "SmallArmIcon") &&
            window_is_named(s->core_icon, "SmallArmIcon") && at(s->arm_wins, 0x256, 0x9e, 0xf, 9) &&
            at(s->core_wins, 0x256, 0xab, 0xf, 9),
        "both logo icons are SmallArmIcon beside their win counts"
    );
    check(
        s->arm_logo >= 0 && s->core_logo >= 0 && s->arm_icon->states.images[1] == s->arm_logo &&
            s->core_icon->states.images[1] == s->core_logo,
        "the icons show ArmCoreLogos frames 0 and 1"
    );
    const Window* const all[] = {
        s->name,
        s->owner,
        s->battles_caption,
        s->battles,
        s->forming_caption,
        s->forming,
        s->arm_icon,
        s->arm_wins,
        s->core_icon,
        s->core_wins,
    };
    bool hidden = true;
    for (const Window* w : all)
        hidden = hidden && w->visible == 0 && w->parent == &frame->base;
    check(hidden, "every summary window is the frame's child and starts hidden");
    const oa::Surface& b = s->background;
    check(
        b.pixels != nullptr && b.width == 640 && b.height == 480,
        "the picture paints into the frame's background"
    );
    if (b.pixels == nullptr)
        return;
    bool filled = true;
    for (int32_t y = 0x7e; y <= 0x9c; ++y)
        for (int32_t x = 0x23a; x <= 0x26d; ++x)
            filled = filled && b.pixels[y * b.pitch + x] == 0xaf;
    check(filled, "the picture area is 0xAF without a planet");
}

// The summary of the selected planet: the hotspot press shows its first
// rows (enable_planet_buttons runs battle_summary_update); contested, the logos and win counts
// show too; the picture (battle_summary_draw) is the planet's current frame of its
// second (50) size anchored at (0x254, 0x8D) and clipped to the area.
void test_summary_selected(StatusWidget* widget, MainFrame* frame) {
    BattleSummary* s = frame->battle_summary;
    if (s == nullptr)
        return;
    ScopedServices scope(widget);
    screens::GalaxyPlanet& p = widget->navigation->galaxy.planets[1];
    check(
        s->name->visible == 1 && s->owner->visible == 1 && s->battles->visible == 1 &&
            s->forming_caption->visible == 1 && s->arm_icon->visible == 0 &&
            s->core_wins->visible == 0,
        "a selection shows the first rows only"
    );
    p.side = 'i';
    battle_summary_update(s);
    check(
        s->arm_icon->visible == 1 && s->arm_wins->visible == 1 && s->core_icon->visible == 1 &&
            s->core_wins->visible == 1,
        "contested shows the logos and win counts"
    );
    p.side = 'A';
    battle_summary_update(s);
    check(s->arm_icon->visible == 0 && s->name->visible == 1, "Arm hides them again");
    MainFrameProbe probe{};
    check(
        boneyards_status_widget_probe(widget, &probe) && probe.summary_shown &&
            probe.summary_planet == 1 && probe.summary_picture_pixels == 0,
        "the probe reads the shown summary and its empty picture"
    );
    int32_t point_x{};
    int32_t point_y{};
    const Window* hotspot = frame->map_panel->hotspots[1];
    check(
        boneyards_status_widget_planet_point(widget, 1, &point_x, &point_y) &&
            point_x == hotspot->x + hotspot->width / 2 &&
            point_y == hotspot->y + hotspot->height / 2,
        "the probe finds a planet's hotspot centre"
    );

    GalaxyGraphics* graphics = widget->galaxy_graphics.get();
    check(graphics != nullptr && graphics->planets.has_value(), "war-map graphics loaded");
    if (graphics == nullptr || !graphics->planets.has_value() || s->background.pixels == nullptr)
        return;
    const oa::formats::gaf::Sequence* sequence =
        galaxy_find_sequence(*graphics->planets, "Metal_50_L");
    check(sequence != nullptr && sequence->frames.size() > 2, "Planets.gaf has Metal_50_L");
    if (sequence == nullptr || sequence->frames.size() <= 2)
        return;
    graphics->planet_sequences[1][1] = sequence;
    p.frame = 2;
    const oa::Surface& b = s->background;
    const std::vector<uint8_t> before(b.pixels, b.pixels + b.pitch * b.height);
    battle_summary_draw(s);
    oa::formats::gaf::RenderResult rendered = oa::formats::gaf::render_normal(sequence->frames[2]);
    check(rendered.ok(), "the picture frame decodes");
    if (!rendered.ok())
        return;
    const oa::formats::gaf::RenderedFrame& image = *rendered.frame;
    bool matches = true;
    int32_t planet_pixels = 0;
    for (int32_t y = 0; y < b.height; ++y) {
        for (int32_t x = 0; x < b.width; ++x) {
            const uint8_t got = b.pixels[y * b.pitch + x];
            const bool inside = x >= 0x23a && x <= 0x26d && y >= 0x7e && y <= 0x9c;
            if (!inside) {
                matches = matches && got == before[static_cast<std::size_t>(y * b.pitch + x)];
                continue;
            }
            uint8_t want = 0xaf;
            const int32_t sx = x - 0x254 + image.origin_x;
            const int32_t sy = y - 0x8d + image.origin_y;
            if (sx >= 0 && sy >= 0 && sx < image.width && sy < image.height) {
                const uint8_t pixel = image.pixels[static_cast<std::size_t>(sy * image.width + sx)];
                if (pixel != image.transparency_index) {
                    want = pixel;
                    ++planet_pixels;
                }
            }
            matches = matches && got == want;
        }
    }
    check(
        matches && planet_pixels > 0,
        "the planet's third frame painted over 0xAF inside the area only"
    );
    check(
        boneyards_status_widget_probe(widget, &probe) && probe.summary_picture_pixels > 0,
        "the probe counts the painted picture"
    );
    check(
        b.clip.x1 == 0 && b.clip.y1 == 0 && b.clip.x2 == 639 && b.clip.y2 == 479,
        "the background's clip is restored"
    );
    graphics->planet_sequences[1][1] = nullptr;
    battle_summary_draw(s);
}

// A two-planet war map on the Main Frame: hotspots, selection, the
// magnification slider and an edge scroll zone.
void test_galaxy_panel(StatusWidget* widget, MainFrame* frame) {
    screens::BoneyardsScreens* nav = widget->navigation;
    check(
        nav->galaxy.locked && frame->mag_slider->base.states.current == button_state::disabled &&
            frame->map_panel->base.focusable == 0,
        "the built Main Frame locks the map and its slider"
    );
    check(test::load_two_planet_galaxy(&nav->galaxy), "war map loaded");
    // The info area closed while the galaxy data was loading.
    frame->unlock_pending = 1;
    boneyards_status_widget_tick(widget);
    check(nav->galaxy.locked && frame->unlock_pending == 1, "the unlock waits for the galaxy data");
    screens::galaxy_apply_allegiances(&nav->galaxy, "CA");
    boneyards_status_widget_tick(widget);
    check(
        !nav->galaxy.locked && frame->unlock_pending == 0 &&
            frame->mag_slider->base.states.current == button_state::unpressed &&
            frame->map_panel->base.focusable == 1,
        "the loaded galaxy unlocks the map and enables the slider"
    );
    // The open console screen covers the map; the test's stand-in screen is
    // hidden for the duration.
    Window* covering = frame->active_screen;
    if (covering != nullptr)
        covering->visible = 0;
    {
        ScopedServices scope(widget);
        galaxy_panel_sync_hotspots(frame);
    }
    GalaxyPanel* panel = frame->map_panel;
    check(
        panel->hotspots[0] != nullptr && panel->hotspots[1] != nullptr &&
            panel->base.children.count == 7,
        "hotspots created for the visible planets"
    );
    check(
        panel->hotspots[1]->x == panel->base.x + 23 &&
            panel->hotspots[1]->y == panel->base.y + 23 && panel->hotspots[1]->width == 4,
        "hotspot placed on the planet's view rectangle"
    );
    g_log.sounds.clear();
    const Window* target = panel->hotspots[1];
    boneyards_status_widget_event(
        widget,
        pointer(
            MessageCode::mouse_move,
            target->x + target->width / 2,
            target->y + target->height / 2,
            0
        )
    );
    click_window(widget, target);
    check(nav->galaxy.selected == 1 && nav->session.area_id == 2, "hotspot selects its planet");
    check(
        target->states.current == button_state::unpressed, "the hotspot press is not a button press"
    );
    check(
        !g_log.sounds.empty() && g_log.sounds[0] == "MORE" && g_log.sounds.back() == "GREEN_BUTTON",
        "hotspot hover and press sounds"
    );
    check(nav->galaxy.hovered == 1, "the pointer rests on the planet");
    check(frame->activity_light->running == 1, "a selection runs the activity light");
    check(nav->galaxy.territory.planet == 1, "territory summary follows the selection");
    test_summary_selected(widget, frame);
    check(
        frame->console_buttons[static_cast<int32_t>(ConsoleButton::recon)]->states.current ==
            button_state::unpressed,
        "recon enabled by the selection"
    );

    {
        ScopedServices scope(widget);
        slider_set_value(frame->mag_slider, 3);
    }
    check(nav->galaxy.scale == 3 && nav->galaxy.view_width == 498, "slider zooms the map");
    check(
        nav->galaxy.scroll_x == 75 && nav->galaxy.scroll_y == 75,
        "zoom keeps the selection in place"
    );
    check(
        panel->hotspots[1]->width == 16 && panel->hotspots[1]->x == panel->base.x + 17,
        "hotspots rescaled"
    );

    frame->scrollers[2]->hovered = 1; // right edge
    boneyards_status_widget_tick(widget);
    check(
        nav->galaxy.scroll_x == 78 && nav->galaxy.scroll_y == 75, "edge zone scrolls while hovered"
    );
    frame->scrollers[2]->hovered = 0;
    boneyards_status_widget_tick(widget);
    check(nav->galaxy.scroll_x == 78, "scrolling stops when the pointer leaves");
    main_frame_lock_map(frame);
    check(
        nav->galaxy.hovered == screens::kNoPlanet &&
            frame->mag_slider->base.states.current == button_state::disabled,
        "locking forgets the hovered planet and disables the slider"
    );
    frame->scrollers[2]->hovered = 1;
    boneyards_status_widget_tick(widget);
    check(nav->galaxy.scroll_x == 78, "locked map ignores the zones");
    frame->scrollers[2]->hovered = 0;
    main_frame_unlock_map(frame);
    if (covering != nullptr)
        covering->visible = 1;
}

void test_navigation(Data* data) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.host.play_sound = host_sound;
    config.host.set_frontend_state = host_state;
    config.host.set_signal = host_signal;
    config.host.init_main_menu_panel = host_panel;
    config.service.network_available = service_network;
    config.service.activate = service_activate;
    config.service.close = service_close;
    config.service.connected = service_connected;
    config.service.request_census = service_census;
    config.screens.create = create_screen;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget created");
    if (widget == nullptr)
        return;

    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    check(g_log.state == frontend_value::state_boneyards, "entered the Boneyards state");
    check(boneyards_status_widget_in_boneyards(widget), "Main Frame shown");
    check(!boneyards_status_widget_visible(widget), "overlay hidden");
    boneyards_status_widget_tick(widget);
    check(widget->desktop.windows.count == 1, "overlay destroyed, Main Frame remains");

    MainFrame* frame = widget->system.main_frame;
    check(frame->base.width == 640 && frame->base.height == 480, "Main Frame is full screen");
    check(
        frame->base.children.count == 34,
        "exit, quickplay, nine console buttons, light, banner, ticker label, slider, map panel, "
        "eight scrollers, the planet summary's eight labels and two icons"
    );
    check(
        frame->map_panel != nullptr && frame->map_panel->base.x == 23 &&
            frame->map_panel->base.width == 498 && frame->map_panel->base.children.count == 5,
        "map panel over the info area with its green layer and four corners"
    );
    check(
        frame->mag_slider != nullptr && frame->mag_slider->range == 4 &&
            frame->mag_slider->index == 0 && frame->mag_slider->base.children.count == 3,
        "magnification slider with thumb and two buttons"
    );
    check(frame->exit_button->x == 19 && frame->exit_button->y == 46, "exit bounds");
    for (Window* b : frame->console_buttons)
        check(b->states.current == button_state::disabled, "console buttons start disabled");
    check_hash(draw_hash(widget, "main_frame.raw"), 0xd82f2d7698aa1775ULL, "Main Frame snapshot");
    test_summary_built(widget, frame);
    const int32_t live = console_images_live_count(widget->images);
    draw_hash(widget, "main_frame_again.raw");
    check(console_images_live_count(widget->images) == live, "drawing does not leak images");

    // The sixteenth cycle with an active client opens the update console (one
    // cycle already ran above) behind the green layer, even over a session
    // that reads as connected; the layer slides in over BYConsoleSlide
    // pulses and shows it on the pulse after. The placeholder pass then plays
    // for about two seconds of the clock before the login console opens.
    for (int32_t i = 0; i < 14; ++i)
        boneyards_status_widget_tick(widget);
    check(g_log.screens.empty(), "the update console waits fifteen cycles");
    g_log.sounds.clear();
    boneyards_status_widget_tick(widget);
    check(
        g_log.screens.size() == 1 && g_log.screens[0] == console_screen::update,
        "update console opened"
    );
    Window* update = g_log.last_screen;
    check(
        update->x == 23 && update->y == 126 && update->width == 498 && update->height == 313 &&
            update->visible == 0,
        "the update console waits hidden in the green layer's rectangle"
    );
    check(
        !g_log.sounds.empty() && g_log.sounds[0] == "GREEN_SLIDE" && frame->console->sliding == 1,
        "the green slides in"
    );
    // The slide's first pulse ran in the cycle that opened the console.
    check(
        finish_slide(widget) + 1 == frame->console->slide_steps + 2,
        "the slide takes BYConsoleSlide pulses and two more"
    );
    check(update->visible == 1 && g_log.sounds.back() == "BIGBUTTON", "update console shown");
    for (int32_t i = 0; i < screens::kUpdatePassFrames / 2; ++i)
        boneyards_status_widget_tick(widget);
    check(g_log.screens.size() == 1, "the update console plays its pass before login");
    for (int32_t i = 0; g_log.screens.size() < 2 && i < screens::kUpdatePassFrames; ++i)
        boneyards_status_widget_tick(widget);
    check(
        g_log.screens.size() == 2 && g_log.screens[1] == console_screen::login,
        "login screen opened"
    );
    check(
        g_log.last_screen->visible == 1 && frame->console->sliding == 0,
        "the login console shows at once under the covering layer"
    );

    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    check(
        frame->console_buttons[static_cast<int32_t>(ConsoleButton::news)]->states.current ==
                button_state::unpressed &&
            frame->console_buttons[static_cast<int32_t>(ConsoleButton::chat)]->states.current ==
                button_state::disabled,
        "chat waits while the galaxy data loads"
    );
    check(
        frame->console_buttons[static_cast<int32_t>(ConsoleButton::locator)]->states.current ==
            button_state::disabled,
        "locator stays disabled without matchmaking_enabled"
    );
    test_galaxy_panel(widget, frame);
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    check(
        frame->console_buttons[static_cast<int32_t>(ConsoleButton::chat)]->states.current ==
            button_state::unpressed,
        "chat enabled once the galaxy has loaded and a planet is selected"
    );

    // The keep-alive asks for the selected area's census when the countdown
    // passes zero, then every 0x1C2 + 1 cycles.
    frame->keepalive_countdown = 1;
    g_log.census.clear();
    boneyards_status_widget_tick(widget);
    check(g_log.census.empty(), "no census before the countdown passes zero");
    boneyards_status_widget_tick(widget);
    check(
        g_log.census == std::vector<uint32_t>{2} && frame->keepalive_countdown == 0x1c2,
        "the keep-alive census of the current area"
    );
    for (int32_t i = 0; i < 0x1c2; ++i)
        boneyards_status_widget_tick(widget);
    check(g_log.census.size() == 1, "the countdown restarts from 0x1C2");
    boneyards_status_widget_tick(widget);
    check(g_log.census == std::vector<uint32_t>({2, 2}), "the next keep-alive");

    g_log.sounds.clear();
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::news)]);
    check(
        frame->console_screen == console_screen::news &&
            g_log.screens.back() == console_screen::news,
        "news opens its console screen"
    );
    check(!g_log.sounds.empty() && g_log.sounds.back() == "GALACTIC_BUTTON", "news sound");
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::news)]);
    check(
        frame->console_screen == console_screen::none && frame->active_screen == nullptr,
        "second press closes the screen"
    );
    finish_slide(widget);

    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::chat)]);
    check(frame->console_screen == console_screen::chat, "chat opens");
    finish_slide(widget);
    Message escape{};
    escape.code = MessageCode::key_down;
    escape.key = key::escape;
    check(boneyards_status_widget_event(widget, escape), "escape consumed");
    check(frame->console_screen == console_screen::none, "escape releases the open screen");

    click_window(widget, frame->exit_button);
    check(boneyards_status_widget_visible(widget), "exit returns to the overlay");
    check(!boneyards_status_widget_in_boneyards(widget), "Main Frame hidden");
    check(
        g_log.state == frontend_value::state_main_menu && g_log.signal == 1, "TA main menu state"
    );
    check(g_log.panel_reloads == 1 && g_log.closes == 1, "panel reloaded and client closed");
    check(widget->system.connection_level == 0, "offline after exit");
    boneyards_status_widget_tick(widget);
    check(widget->desktop.windows.count == 1, "Main Frame destroyed on the next cycle");
    const Window* enter = boneyards_status_widget_part(widget, OverlayPart::enter);
    check(enter->states.current == button_state::unpressed, "enter button available again");
    boneyards_status_widget_destroy(widget);
}

std::string label_text(StatusWidget* widget, Window* label) {
    ScopedServices scope(widget);
    const char* text = label->ops->state_text(label, -1);
    return text != nullptr ? text : "";
}

// What main_frame_tick_ticker gives the ticker label at scroll offset `offset`: 99
// characters read around the ticker's text but for its last character.
std::string ticker_window(const char* ticker, int32_t offset) {
    const auto span = static_cast<int32_t>(std::strlen(ticker)) - 1;
    std::string shown;
    for (int32_t i = 0; i < 99; ++i)
        shown += ticker[(i + offset) % span];
    return shown;
}

void alert(
    StatusWidget* widget, int32_t kind, const char* text, uint32_t sender, const char* type
) {
    ScopedServices scope(widget);
    const AlertReport report{kind, text, sender, type};
    main_frame_on_alert(widget->system.main_frame, &report);
}

std::string box_text(StatusWidget* widget, MessageBox* box) {
    Window* text = window_find_named(&box->base, message_box_name::text, nullptr);
    return text != nullptr ? label_text(widget, text) : "";
}

// The news ticker (main_frame_build, main_frame_tick_ticker) and the system alerts
// (main_frame_on_alert) on a Main Frame with text services.
void test_alerts(Data* data) {
    g_log = Log{};
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.host.play_sound = host_sound;
    config.host.register_sound = host_register_sound;
    config.host.battle_message = host_battle_line;
    config.service.network_available = service_network;
    config.service.activate = service_activate;
    config.service.close = service_close;
    config.screens.create = create_screen;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr && widget->toolkit != nullptr, "alert widget with text services");
    if (widget == nullptr)
        return;
    g_log.sounds.clear();
    g_log.sound_order.clear();
    g_log.named.clear();
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    MainFrame* frame = widget->system.main_frame;
    check(frame != nullptr, "Main Frame for the alerts");
    if (frame == nullptr)
        return;

    // The label under the exit button, after the banner among the frame's
    // children, and the welcome seeded through a ticker alert.
    const std::string welcome = "    Welcome to the Boneyards ...                       ";
    Window* label = frame->ticker_label;
    check(
        label != nullptr && label->parent == &frame->base && label->x == 0x2d && label->y == 0x5f &&
            label->width == 0x7c && label->font == widget->system.hat_fonts.small,
        "ticker label at (0x2D, 0x5F), 0x7C wide, in the small font"
    );
    check(
        frame->base.children.count > 14 && frame->base.children.items[13] == frame->ad_banner &&
            frame->base.children.items[14] == label,
        "the label follows the advert banner"
    );
    check(frame->ticker == welcome, "the ticker starts with the welcome and a four-space spacer");
    check(
        std::find(g_log.sounds.begin(), g_log.sounds.end(), "MESSAGE") != g_log.sounds.end(),
        "the welcome goes through the alert handler, which sounds MESSAGE"
    );
    const auto message_named =
        std::find(g_log.sound_order.begin(), g_log.sound_order.end(), "name MESSAGE");
    const auto message_played =
        std::find(g_log.sound_order.begin(), g_log.sound_order.end(), "play MESSAGE");
    const auto message_file =
        std::find_if(g_log.named.begin(), g_log.named.end(), [](const NamedSound& sound) {
            return sound.category == "MESSAGE";
        });
    check(
        message_named != g_log.sound_order.end() && message_played != g_log.sound_order.end() &&
            message_named < message_played && message_file != g_log.named.end() &&
            message_file->file == "hovsmsl1",
        "the welcome's MESSAGE is registered as hovsmsl1 before it plays"
    );

    // A step every sixth pulse, one character further on.
    widget->system.ticker_countdown = 0;
    widget->system.ticker_offset = 0;
    boneyards_status_widget_tick(widget);
    const std::string first = ticker_window(frame->ticker, 0);
    check(
        first.substr(0, 54) == welcome.substr(0, 54) && first.substr(54) == welcome.substr(0, 45),
        "the window reads around the text but for its last character"
    );
    check(label_text(widget, label) == first, "a step shows the first 99 characters");
    check(
        widget->system.ticker_countdown == 5 && widget->system.ticker_offset == 1,
        "the countdown restarts at 5 and the offset moves on"
    );
    for (int32_t i = 0; i < 5; ++i)
        boneyards_status_widget_tick(widget);
    check(
        label_text(widget, label) == first && widget->system.ticker_countdown == 0,
        "the next five pulses count down"
    );
    boneyards_status_widget_tick(widget);
    check(
        label_text(widget, label) == ticker_window(frame->ticker, 1) &&
            label_text(widget, label) == first.substr(1) + welcome[45],
        "the sixth pulse scrolls the ticker by one character"
    );

    // A ticker alert replaces the text; a text too long for the spacer goes
    // without it, and one past the buffer is cut.
    g_log.sounds.clear();
    alert(widget, 0, "Ladder reset at noon", 0, "");
    check(
        std::strcmp(frame->ticker, "Ladder reset at noon    ") == 0 &&
            g_log.sounds == std::vector<std::string>{"MESSAGE"},
        "a ticker alert sounds MESSAGE and replaces the text"
    );
    widget->system.ticker_countdown = 0;
    boneyards_status_widget_tick(widget);
    check(
        label_text(widget, label) == ticker_window(frame->ticker, 2),
        "the label scrolls the new text from the offset reached"
    );
    const std::string fits(0xfa, 'x');
    alert(widget, 0, fits.c_str(), 0, "");
    check(std::strlen(frame->ticker) == 0xfe, "0xFA characters take the spacer");
    const std::string full(0xfb, 'y');
    alert(widget, 0, full.c_str(), 0, "");
    check(frame->ticker == full, "0xFB characters go without it");
    const std::string past(0x120, 'z');
    alert(widget, 0, past.c_str(), 0, "");
    check(std::strlen(frame->ticker) == 0xff, "a longer text is cut to the buffer");

    // A PROMOTION notification sounds PROMOTED and sets the ticker; another
    // type does nothing.
    g_log.sounds.clear();
    alert(widget, 3, "Promoted to Major", 0, "PROMOTION");
    check(
        std::strcmp(frame->ticker, "Promoted to Major    ") == 0 &&
            g_log.sounds == std::vector<std::string>{"PROMOTED"},
        "a promotion sounds PROMOTED and replaces the ticker"
    );
    g_log.sounds.clear();
    alert(widget, 3, "Demoted", 0, "promotion");
    alert(widget, 3, "Demoted", 0, "");
    check(
        std::strcmp(frame->ticker, "Promoted to Major    ") == 0 && g_log.sounds.empty(),
        "any other notification is dropped"
    );
    MainFrameProbe probe{};
    {
        ScopedServices scope(widget);
        check(boneyards_status_widget_probe(widget, &probe), "Main Frame probed");
    }
    check(
        std::strcmp(probe.ticker_text, frame->ticker) == 0 &&
            probe.ticker_shown == label_text(widget, label),
        "the probe reads the ticker and its label"
    );

    // A message-box alert replaces the box the last one opened.
    g_log.sounds.clear();
    alert(widget, 1, "Server going down at noon", 0, "");
    MessageBox* first_box = frame->notice_box;
    check(
        first_box != nullptr && message_box_find_open(&widget->desktop) == first_box &&
            box_text(widget, first_box) == "Server going down at noon" &&
            g_log.sounds == std::vector<std::string>{"MESSAGE"},
        "a message-box alert sounds MESSAGE and opens a box"
    );
    alert(widget, 1, "Server going down now", 0, "");
    MessageBox* second_box = frame->notice_box;
    check(
        first_box->base.owned == 1 && second_box != first_box &&
            message_box_find_open(&widget->desktop) == second_box &&
            box_text(widget, second_box) == "Server going down now",
        "the next closes the open box and opens its own"
    );
    boneyards_status_widget_tick(widget);
    check(!desktop_holds(&widget->desktop, &first_box->base), "the replaced box is destroyed");
    Message enter{};
    enter.code = MessageCode::key_down;
    enter.key = key::enter;
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, &second_box->base);
    }
    boneyards_status_widget_event(widget, enter);
    boneyards_status_widget_tick(widget);
    check(message_box_find_open(&widget->desktop) == nullptr, "Enter closes the notice box");
    alert(widget, 1, "Back up", 0, "");
    check(
        frame->notice_box != nullptr &&
            message_box_find_open(&widget->desktop) == frame->notice_box,
        "a box closed by the player is not closed again"
    );
    boneyards_status_widget_tick(widget);

    // A page with no Chat console shown opens a box of its own; a guest's is
    // dropped without a sound.
    MessageBox* notice = frame->notice_box;
    g_log.sounds.clear();
    alert(widget, 2, "<Page from Rex> hi", 7, "");
    MessageBox* page_box = message_box_find_open(&widget->desktop);
    check(
        page_box != nullptr && page_box != notice && frame->notice_box == notice &&
            box_text(widget, page_box) == "<Page from Rex> hi" &&
            g_log.sounds == std::vector<std::string>{"MESSAGE"},
        "a page without the chat opens a box"
    );
    g_log.sounds.clear();
    alert(widget, 2, "<Page from guest> hi", 0x80000007U, "");
    check(
        message_box_find_open(&widget->desktop) == page_box && g_log.sounds.empty(),
        "a guest's page is dropped"
    );

    // During a battle launched from the Boneyards the texts go to its
    // messages.
    widget->system.chat_console_pending = 1;
    g_log.sounds.clear();
    const std::string ticker = frame->ticker;
    alert(widget, 0, "Ticker in battle", 0, "");
    alert(widget, 1, "Box in battle", 0, "");
    alert(widget, 2, "Page in battle", 9, "");
    alert(widget, 3, "Promotion in battle", 0, "PROMOTION");
    check(
        g_log.battle_lines ==
            std::vector<std::string>{
                "Ticker in battle", "Box in battle", "Page in battle", "Promotion in battle"
            },
        "every kind goes to the battle's messages"
    );
    check(
        g_log.sounds == std::vector<std::string>{"MESSAGE", "MESSAGE", "MESSAGE", "PROMOTED"},
        "with its sound"
    );
    check(
        frame->ticker == ticker && message_box_find_open(&widget->desktop) == page_box,
        "leaving the ticker and the boxes alone"
    );
    widget->system.chat_console_pending = 0;
    boneyards_status_widget_destroy(widget);
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the main frame tests");
    const std::filesystem::path by_ccx = root / "by.ccx";
    Data data;
    data.store.mount(by_ccx);
    test_navigation(&data);

    {
        Data alerts;
        alerts.store.mount(by_ccx);
        alerts.store.mount(root / "totala1.hpi");
        const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
        for (int i = 0; i < OA_PALETTE_COLORS; ++i)
            alerts.palette.entries[i] =
                oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
        test_alerts(&alerts);
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("main frame tests passed\n");
    return 0;
}
