// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Recon console on a two-planet war map with the BY1117a install: the
// Recon button opens the selected planet's dossier behind the green layer,
// fills its fields from the planet and its map OTA, follows the census
// replies for its planet, steps its scan image, lists the participation
// notice of a planet whose map is missing, and closes on Release.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "console_layer.hpp"
#include "console_state.hpp"
#include "galaxy_fixture.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/screens/contents.hpp"
#include "oa/boneyards/screens/rand15.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;
namespace row = screens::planet_info_row;

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
    std::vector<uint32_t> census;
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

void record_census(void*, uint32_t area) {
    g_log.census.push_back(area);
}

void keep_events(void*, const ServiceEvents* events) {
    g_log.events = events != nullptr ? *events : ServiceEvents{};
}

// A census reply as the service delivers it, inside a widget tick.
void reply_census(uint32_t area, int32_t population) {
    CensusReport report{};
    report.area_id = area;
    report.population = population;
    ScopedServices scope(g_widget);
    g_log.events.census(g_log.events.user, &report);
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

std::string field_text(const ReconConsole* console, std::size_t which) {
    return text_of(console->fields[which]->value);
}

std::vector<std::string> rows_of(const ReconConsole* console) {
    std::vector<std::string> rows;
    ListBox* list = &console->text->list;
    for (int32_t i = 0; i < list->count; ++i)
        rows.push_back(text_of(list_box_item(list, i)));
    return rows;
}

Handle row_font(const ReconConsole* console, int32_t index) {
    auto* text_row =
        reinterpret_cast<screens::ByTextRow*>(list_box_item(&console->text->list, index));
    return text_row != nullptr && text_row->label != nullptr ? text_row->label->font : k_no_handle;
}

Window* button(const MainFrame* frame, ConsoleButton which) {
    return frame->console_buttons[static_cast<int32_t>(which)];
}

ConsoleNavButton* recon_button(const MainFrame* frame) {
    return reinterpret_cast<ConsoleNavButton*>(button(frame, ConsoleButton::recon));
}

constexpr const char* k_description =
    "A world of shallow seas where two continents face each other across a narrow strait, "
    "fought over since the first days of the war.";

screens::PlanetRecord planet(int32_t id, const char* name, int32_t x, int32_t y, char side) {
    screens::PlanetRecord r = test::fixture_planet(id, name, x, y, side);
    r.description = k_description;
    r.pan_gaf = "Metalbrief.gaf";
    r.pan_seq = "MetalPan";
    return r;
}

constexpr int32_t k_image_width = 0x1f2;
constexpr int32_t k_image_height = 0x139;

void draw_frame() {
    std::vector<uint8_t> pixels(640 * 480, 0);
    Surface target{};
    target.width = 640;
    target.height = 480;
    target.pitch = 640;
    target.pixels = pixels.data();
    target.clip = oa::Rect32{0, 0, 639, 479};
    boneyards_status_widget_draw(g_widget, &target);
}

std::vector<uint8_t> blank_image() {
    return std::vector<uint8_t>(static_cast<std::size_t>(k_image_width * k_image_height), 9);
}

Surface image_target(std::vector<uint8_t>* pixels) {
    Surface target{};
    target.width = k_image_width;
    target.height = k_image_height;
    target.pitch = k_image_width;
    target.pixels = pixels->data();
    target.clip = oa::Rect32{0, 0, k_image_width - 1, k_image_height - 1};
    return target;
}

// The console's own image, row by row.
std::vector<uint8_t> image_pixels(const ReconConsole* console) {
    ScopedServices scope(g_widget);
    Surface surface{};
    std::vector<uint8_t> pixels;
    if (image_surface(console->image, &surface))
        for (int32_t y = 0; y < surface.height; ++y)
            for (int32_t x = 0; x < surface.width; ++x)
                pixels.push_back(surface.pixels[y * surface.pitch + x]);
    return pixels;
}

bool in_scan_box(int32_t x, int32_t y) {
    return x >= 0xd && x <= 0x1e3 && y >= 0x10 && y <= 0x89;
}

// Inside the scan box the image shows what `expected` does.
bool scan_box_matches(const std::vector<uint8_t>& image, const std::vector<uint8_t>& expected) {
    if (image.size() != expected.size())
        return false;
    for (int32_t y = 0; y < k_image_height; ++y)
        for (int32_t x = 0; x < k_image_width; ++x) {
            const auto at = static_cast<std::size_t>(y * k_image_width + x);
            if (in_scan_box(x, y) && image[at] != expected[at])
                return false;
        }
    return true;
}

int32_t lit_in_scan_box(const std::vector<uint8_t>& image) {
    int32_t lit = 0;
    for (int32_t y = 0; y < k_image_height; ++y)
        for (int32_t x = 0; x < k_image_width; ++x)
            if (in_scan_box(x, y) && static_cast<std::size_t>(y * k_image_width + x) < image.size())
                lit += image[static_cast<std::size_t>(y * k_image_width + x)] != 9 ? 1 : 0;
    return lit;
}

// The Main Frame tests' two planets, both on Coast to Coast, with a
// description; the allegiance feed unlocks the map once the login console
// has slid away.
void load_war_map(MainFrame* frame) {
    screens::GalaxyView* galaxy = &g_widget->navigation->galaxy;
    const screens::PlanetRecord planets[2] = {
        planet(test::k_core_prime_id, "Core Prime", 10, 100, 'C'),
        planet(test::k_empyrrean_id, "Empyrrean", 100, 100, 'A')
    };
    const screens::ConnectorRecord link{test::k_core_prime_id, test::k_empyrrean_id};
    const screens::SpriteFrame frames[2] = {{16, 16, 8, 8}, {16, 16, 8, 8}};
    const std::size_t counts[2] = {1, 1};
    const screens::PlanetSprites sprites{frames, counts};
    CHECK(screens::galaxy_load(galaxy, 2400, 1600, planets, 2, &link, 1, &sprites));
    screens::galaxy_apply_allegiances(galaxy, "CA");
    ScopedServices scope(g_widget);
    galaxy_panel_sync_hotspots(frame);
    main_frame_close_screen(frame);
}

// Presses planet `index`'s hotspot, then the Recon button, and runs the
// slide in to where the console shows.
ReconConsole* open_recon(MainFrame* frame, std::size_t index) {
    click_window(frame->map_panel->hotspots[index]);
    CHECK(g_widget->navigation->galaxy.selected == static_cast<int32_t>(index));
    CHECK(button(frame, ConsoleButton::recon)->states.current == button_state::unpressed);
    g_log.sounds.clear();
    click_window(button(frame, ConsoleButton::recon));
    CHECK(frame->console_screen == console_screen::recon);
    CHECK(!g_log.sounds.empty() && g_log.sounds[0] == "PLANET_BUTTON");
    Window* screen = frame->active_screen;
    CHECK(screen != nullptr && window_is_named(screen, "ReconConsole"));
    if (screen == nullptr)
        return nullptr;
    CHECK(screen->visible == 0);
    CHECK(
        screen->x == main_frame_layout::console_x && screen->y == main_frame_layout::console_y &&
        screen->width == main_frame_layout::console_width &&
        screen->height == main_frame_layout::console_height
    );
    // The slide's first pulse runs in the cycle after the press.
    tick(frame->console->slide_steps + 2);
    CHECK(frame->console->sliding == 0 && frame->console->covering == 1);
    CHECK(window_is_shown(screen));
    return reinterpret_cast<ReconConsole*>(screen);
}

// The build (recon_build): the scan box outlined in 0x5A on a keyed image,
// the eight fields at their placements, and the list under them.
void check_build(const ReconConsole* console) {
    const Window& self = console->base;
    CHECK(self.states.images[1] == console->image && console->image >= 0);
    {
        ScopedServices scope(g_widget);
        Surface surface{};
        CHECK(image_surface(console->image, &surface));
        if (surface.pixels != nullptr) {
            const auto at = [&](int32_t x, int32_t y) {
                return surface.pixels[y * surface.pitch + x];
            };
            CHECK(surface.width == 0x1f2 && surface.height == 0x139);
            CHECK(at(0xc, 0xf) == 0x5a && at(0x1e4, 0x8a) == 0x5a && at(0x100, 0xf) == 0x5a);
            CHECK(at(0xc, 0x50) == 0x5a && at(0x1e4, 0x50) == 0x5a);
            CHECK(at(0, 0) == 9 && at(0x100, 0xb0) == 9);
        }
    }

    struct Placement {
        std::size_t row;
        int32_t x, y, width;
    };

    const Placement placements[] = {
        {row::planet, 0xc, 0x92, 0xa6},
        {row::gw_map, 0xc, 0xa3, 0xa6},
        {row::type, 0xb6, 0x92, 0x62},
        {row::gravity, 0xb6, 0xa3, 0x62},
        {row::wind_speed, 0x11c, 0x92, 0x64},
        {row::tide_strength, 0x11c, 0xa3, 0x64},
        {row::affiliation, 0x184, 0x92, 0x5c},
        {row::population, 0x184, 0xa3, 0x5c},
    };
    for (const Placement& p : placements) {
        const Window& field = console->fields[p.row]->base;
        CHECK(
            field.x == self.x + p.x && field.y == self.y + p.y && field.width == p.width &&
            field.height == 0xa && field.parent == &self
        );
    }
    const Window& list = console->text->list.base;
    CHECK(list.x == self.x + 0xc && list.y == self.y + 0xb4 && list.width == 0x1d9);
    CHECK(list.height == 0x7e && console->text->framed == 0);
}

// The description as the rows show it, joined again with spaces.
std::string joined(const std::vector<std::string>& rows, std::size_t first) {
    std::string text;
    for (std::size_t i = first; i < rows.size(); ++i)
        text += (i == first ? "" : " ") + rows[i];
    return text;
}

void test_recon(Data* data) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.host.play_sound = record_sound;
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    config.service.request_census = record_census;
    config.service.set_events = keep_events;
    g_widget = boneyards_status_widget_create(config);
    CHECK(g_widget != nullptr && g_log.events.census != nullptr);
    if (g_widget == nullptr || g_log.events.census == nullptr)
        return;
    click_window(boneyards_status_widget_part(g_widget, OverlayPart::enter));
    MainFrame* frame = g_widget->system.main_frame;
    CHECK(frame != nullptr);
    if (frame == nullptr)
        return;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        tick(1);
    CHECK(frame->console_screen == console_screen::login);
    load_war_map(frame);
    finish_slide(frame);
    CHECK(!g_widget->navigation->galaxy.locked);

    // Empyrrean: the hotspot asks for its census, the Recon button asks
    // again as the console opens.
    g_log.census.clear();
    const uint32_t requests = frame->census_requests;
    screens::rand15_seed(1);
    ReconConsole* console = open_recon(frame, 1);
    if (console == nullptr)
        return;
    check_build(console);
    CHECK(g_log.census == std::vector<uint32_t>({2, 2}) && frame->census_requests == requests + 2);
    CHECK(
        recon_button(frame)->base.states.images[button_state::unpressed] == recon_button(frame)->lit
    );
    CHECK(frame->activity_light->running == 1);
    CHECK(field_text(console, row::planet) == "Empyrrean");
    CHECK(field_text(console, row::gw_map) == "Coast to Coast");
    CHECK(field_text(console, row::type) == "Green");
    // Coast to Coast.ota: gravity 112, tidal strength 15, wind 0..3500.
    CHECK(field_text(console, row::gravity) == "112");
    CHECK(field_text(console, row::tide_strength) == "15");
    CHECK(field_text(console, row::affiliation) == "Arm");
    CHECK(field_text(console, row::population) == "0");
    const screens::ReconState& state = console->state;
    CHECK(state.planet == 1 && state.map_installed);
    CHECK(state.wind_min == 0 && state.wind_max == 3500 && state.wind_range == 3500);
    // The rand15 stream seeded 1 draws 41 (the wind countdown, % 0x51) and
    // 18467 (the speed, % 3500 over the minimum); every pulse since the open
    // has scrolled the scan a pixel and counted the wind down once.
    CHECK(state.wind == 18467 % 3500 && state.pan_scroll > 0);
    CHECK(state.wind_countdown == 41 - state.pan_scroll);
    CHECK(field_text(console, row::wind_speed) == "967");

    // The description wrapped into small-font rows three pixels short of the
    // row; no notice for an installed map.
    std::vector<std::string> rows = rows_of(console);
    CHECK(rows.size() > 1 && joined(rows, 0) == k_description);
    CHECK(row_font(console, 0) == g_widget->system.hat_fonts.small);
    {
        ScopedServices scope(g_widget);
        const int32_t room = screens::by_list_row_width(&console->text->list.base) - 3;
        for (const std::string& line : rows) {
            int32_t width{};
            int32_t height{};
            text_measure_string(g_widget->system.hat_fonts.small, line.c_str(), &width, &height);
            CHECK(width <= room);
        }
    }

    // Census replies: the light stops on the planet's; another planet's
    // leaves Population alone.
    tick(1);
    reply_census(2, 34);
    CHECK(frame->activity_light->running == 0);
    CHECK(field_text(console, row::population) == "34");
    reply_census(1, 99);
    CHECK(field_text(console, row::population) == "34");
    CHECK(g_widget->navigation->galaxy.planets[0].population == 99);
    MainFrameProbe probe{};
    CHECK(boneyards_status_widget_probe(g_widget, &probe));
    CHECK(probe.recon_shown && std::strcmp(probe.recon_planet_text, "Empyrrean") == 0);
    CHECK(std::strcmp(probe.recon_gravity_text, "112") == 0 && probe.recon_population == 34);
    CHECK(probe.recon_panorama && probe.recon_pan_scroll == state.pan_scroll);

    // The Main Frame's keep-alive asks for the current area's census without
    // starting the light, when its countdown from 0x1C2 goes below zero (every
    // 0x1C3 cycles); its reply reaches the open console too.
    {
        const uint32_t before = frame->census_requests;
        g_log.census.clear();
        int32_t cycles = 0;
        while (frame->census_requests == before && cycles <= 0x1c3) {
            tick(1);
            ++cycles;
        }
        CHECK(frame->census_requests == before + 1 && cycles <= 0x1c3);
        CHECK(g_log.census == std::vector<uint32_t>({2}));
        CHECK(frame->activity_light->running == 0);
        reply_census(2, 41);
        CHECK(field_text(console, row::population) == "41");
        tick(0x1c2);
        CHECK(frame->census_requests == before + 1);
        tick(1);
        CHECK(frame->census_requests == before + 2 && g_log.census.size() == 2);
    }

    // MetalPan (Metalbrief.gaf): frames of 640, 640, 640 and 520 pixels side
    // by side; each pulse scrolls the scan a pixel along the strip, and a
    // wind change reaches its field at the next draw, which paints the strip
    // under the scroll position into the scan box.
    CHECK(state.has_pan && state.pan_frame_count == 4 && state.pan_total == 2440);
    CHECK(
        state.pan_offsets[1] == 640 && state.pan_offsets[2] == 1280 && state.pan_offsets[3] == 1920
    );
    const int32_t scroll = state.pan_scroll;
    const int32_t before = state.static_frame;
    console->state.wind_countdown = 1;
    console->state.wind = 1000;
    screens::rand15_seed(7);
    tick(1);
    CHECK(state.pan_scroll == (scroll + 1) % 2440);
    CHECK(state.pan_end == (scroll + 1 + screens::kReconPanWidth) % 2440);
    CHECK(state.static_frame == before);
    CHECK(state.wind_dirty && state.wind >= 998 && state.wind <= 1002);
    draw_frame();
    CHECK(!state.wind_dirty && field_text(console, row::wind_speed) == std::to_string(state.wind));
    GalaxyGraphics* graphics = g_widget->galaxy_graphics.get();
    CHECK(graphics != nullptr && graphics->map_static_frames > 1);
    if (graphics != nullptr) {
        std::vector<uint8_t> scan = image_pixels(console);
        std::vector<uint8_t> expected = blank_image();
        Surface target = image_target(&expected);
        galaxy_draw_panorama(
            *graphics, 1, &target, state.pan_scroll, state.pan_end, k_recon_scan_clip
        );
        CHECK(scan_box_matches(scan, expected) && lit_in_scan_box(scan) > 0x1000);
    }
    const int32_t frames = graphics != nullptr ? graphics->map_static_frames : 0;

    // The Recon button again closes the console on the normal face.
    click_window(button(frame, ConsoleButton::recon));
    CHECK(frame->console_screen == console_screen::none && frame->active_screen == nullptr);
    CHECK(
        recon_button(frame)->base.states.images[button_state::unpressed] ==
        recon_button(frame)->normal
    );
    CHECK(console->base.visible == 0 && console->text->list.count == 0);
    finish_slide(frame);

    // Core Prime's map is missing: '?' in the environment fields, the TA
    // disk notice in small green over a blank row, then the description.
    std::snprintf(
        g_widget->navigation->galaxy.planets[0].ta_map,
        sizeof g_widget->navigation->galaxy.planets[0].ta_map,
        "%s",
        "No Such World"
    );
    console = open_recon(frame, 0);
    if (console == nullptr)
        return;
    CHECK(field_text(console, row::planet) == "Core Prime");
    CHECK(field_text(console, row::gw_map) == "No Such World");
    CHECK(field_text(console, row::gravity) == "?");
    CHECK(field_text(console, row::wind_speed) == "?");
    CHECK(field_text(console, row::tide_strength) == "?");
    CHECK(field_text(console, row::affiliation) == "Core");
    CHECK(field_text(console, row::population) == "99");
    CHECK(!console->state.map_installed);
    const char* notice[screens::kMaxRequirementLines]{};
    const std::size_t notice_count = screens::gw_requirement_lines(0, notice);
    CHECK(notice_count == 5);
    rows = rows_of(console);
    CHECK(rows.size() > notice_count);
    for (std::size_t i = 0; i < notice_count && i < rows.size(); ++i) {
        CHECK(rows[i] == notice[i]);
        CHECK(
            row_font(console, static_cast<int32_t>(i)) ==
            (i + 1 < notice_count ? g_widget->system.hat_fonts.small_green
                                  : g_widget->system.hat_fonts.small)
        );
    }
    CHECK(joined(rows, notice_count) == k_description);
    // Without a panorama each pulse steps the map static, whose frame the
    // draw paints into the scan box.
    CHECK(!console->state.has_pan);
    const int32_t static_before = console->state.static_frame;
    tick(1);
    CHECK(frames > 0 && console->state.static_frame == (static_before + 1) % frames);
    draw_frame();
    if (graphics != nullptr) {
        std::vector<uint8_t> scan = image_pixels(console);
        std::vector<uint8_t> expected = blank_image();
        Surface target = image_target(&expected);
        galaxy_draw_map_static(*graphics, console->state.static_frame, &target, k_recon_scan_clip);
        CHECK(scan_box_matches(scan, expected) && lit_in_scan_box(scan) > 0x1000);
    }

    // Release closes it the same way.
    click_window(button(frame, ConsoleButton::release));
    CHECK(frame->console_screen == console_screen::none);
    CHECK(
        recon_button(frame)->base.states.images[button_state::unpressed] ==
        recon_button(frame)->normal
    );
    CHECK(console->base.visible == 0);
    tick(1);
    boneyards_status_widget_destroy(g_widget);
    g_widget = nullptr;
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the Recon console tests");
    Data data;
    (void)data.store.discover("31");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    test_recon(&data);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("recon console tests passed\n");
    return 0;
}
