// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map panel's viewer with its moving static and the green Console layer
// sliding over it, against the BY1117a by.ccx data.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/platform/system.hpp"
#include "oa/boneyards/console/status_widget.hpp"

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

constexpr uint8_t k_sentinel = resource::console_key;

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
    int32_t slide_setting = -1; // BYConsoleSlide as stored, -1 = unset
};

Log g_log;

void host_sound(void*, const char* name) {
    g_log.sounds.emplace_back(name);
}

bool service_network(void*) {
    return true;
}

ActivationResult service_activate(void*) {
    return ActivationResult::ok;
}

int32_t read_setting(void*, const char* key, int32_t fallback) {
    if (std::strcmp(key, screens::kConsoleSlideKey) == 0 && g_log.slide_setting >= 0)
        return g_log.slide_setting;
    return fallback;
}

bool played(const char* name) {
    for (const std::string& sound : g_log.sounds) {
        if (sound == name)
            return true;
    }
    return false;
}

void test_green_remap(const Data& data) {
    for (int32_t i = 0; i < 256; ++i) {
        const uint8_t green = k_console_green_remap[i];
        const bool fixed = (i >= 1 && i <= 15) || (i >= 240 && i <= 254);
        if (fixed) {
            check(green == 200, "the fixed colours remap to orange 200");
        } else {
            check(green >= 42 && green <= 46, "every other index remaps to a green 42..46");
        }
    }
    check(k_console_green_remap[0] == 46 && k_console_green_remap[255] == 42, "black and white");
    const oa::PaletteEntry& bright = data.palette.entries[42];
    const oa::PaletteEntry& dark = data.palette.entries[46];
    check(bright.r == 0 && bright.g == 71 && bright.b == 39, "42 is the brightest green");
    check(dark.r == 0 && dark.g == 27 && dark.b == 15, "46 is the darkest green");
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

// Draws a frame over `fill`; with OA_SNAPSHOT_DIR set also writes it as a PPM.
std::vector<uint8_t>
draw_frame(StatusWidget* widget, const Data& data, uint8_t fill, const char* name) {
    std::vector<uint8_t> pixels(640 * 480, fill);
    oa::Surface surface{};
    surface.width = 640;
    surface.height = 480;
    surface.pitch = 640;
    surface.pixels = pixels.data();
    surface.clip = oa::Rect32{0, 0, 639, 479};
    boneyards_status_widget_draw(widget, &surface);
    if (const auto dir = oa::platform::environment_value("OA_SNAPSHOT_DIR");
        dir && name != nullptr) {
        std::ofstream out(std::filesystem::path(*dir) / name, std::ios::binary);
        out << "P6\n640 480\n255\n";
        for (uint8_t index : pixels) {
            const oa::PaletteEntry& e = data.palette.entries[index];
            out.put(static_cast<char>(e.r)).put(static_cast<char>(e.g)).put(static_cast<char>(e.b));
        }
    }
    return pixels;
}

// No pixel of the map panel's area kept the sentinel it was drawn over.
bool info_area_covered(const std::vector<uint8_t>& pixels) {
    namespace layout = main_frame_layout;
    for (int32_t y = layout::info_y; y < layout::info_y + layout::info_height; ++y) {
        for (int32_t x = layout::info_x; x < layout::info_x + layout::info_width; ++x) {
            if (pixels[static_cast<std::size_t>(y) * 640 + static_cast<std::size_t>(x)] ==
                k_sentinel)
                return false;
        }
    }
    return true;
}

Sprite* image_sprite(StatusWidget* widget, Handle image) {
    return console_images_sprite(boneyards_status_widget_images(widget), image);
}

StatusWidget* enter_boneyards(Data* data) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.host.play_sound = host_sound;
    config.service.network_available = service_network;
    config.service.activate = service_activate;
    StatusWidget* widget = boneyards_status_widget_create(config);
    if (widget == nullptr)
        return nullptr;
    widget->navigation->profile.settings.read_number = read_setting;
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    return widget;
}

void test_layer_build(Data* data) {
    g_log = Log{};
    StatusWidget* widget = enter_boneyards(data);
    check(widget != nullptr && widget->system.main_frame != nullptr, "Main Frame");
    if (widget == nullptr || widget->system.main_frame == nullptr)
        return;
    MainFrame* frame = widget->system.main_frame;
    GalaxyPanel* panel = frame->map_panel;
    ConsoleLayer* layer = frame->console;
    check(layer != nullptr && layer == panel->console, "the Main Frame keeps the panel's layer");
    if (layer == nullptr)
        return;
    check(
        window_is_named(&layer->base, "Console") && layer->base.parent == &panel->base,
        "the Console window is a child of the map panel"
    );
    check(
        layer->base.x == 0x17 && layer->base.y == 0x7e && layer->base.width == 0x1f2 &&
            layer->base.height == 0x139,
        "Console bounds 23,126 498x313"
    );
    check(
        layer->base.visible == 0 && layer->covering == 0 && layer->sliding == 0,
        "the layer starts hidden and not covering"
    );
    check(layer->slide_steps == screens::kSlideStepsDefault, "BYConsoleSlide defaults to 20");
    const Sprite* border = image_sprite(widget, layer->green_screen);
    check(
        border != nullptr && border->width == 498 && border->height == 313,
        "Widgets.gaf Green_Screen"
    );
    check(
        layer->map->surface.width == 498 && layer->map->surface.height == 313 &&
            layer->green->surface.width == 498 && layer->green->surface.height == 313,
        "ConMap and ConGreen take the Green_Screen size"
    );
    const Sprite* image = image_sprite(widget, layer->base.states.images[1]);
    check(
        image != nullptr && image->width == 498 && image->height == 313 && image->key == 9 &&
            static_cast<const uint8_t*>(image->data)[0] == 9,
        "the layer's image is blank and keyed on 9"
    );
    const Sprite* viewer = image_sprite(widget, panel->base.states.images[1]);
    check(
        viewer != nullptr && viewer->width == 498 && viewer->height == 328 && viewer->key == 9,
        "ViewerSurf fills the panel, keyed on 9"
    );
    const Sprite* background = image_sprite(widget, panel->background);
    check(
        background != nullptr && background->width == 381 && background->height == 313 &&
            background->origin_x == -59 && background->origin_y == -6,
        "GMap_BG kept by the panel"
    );
    check(panel->base.children.count == 1 + k_map_corner_count, "the layer and the four corners");

    // The static moves every cycle while the galaxy data loads.
    screens::GalaxyView& galaxy = widget->navigation->galaxy;
    check(galaxy.loading && galaxy.static_frame == 0, "the static starts at frame 0");
    const int32_t expected[] = {1, 2, 0};
    for (int32_t frame_index : expected) {
        boneyards_status_widget_tick(widget);
        check(galaxy.static_frame == frame_index, "the static cycles through its three frames");
    }
    const std::vector<uint8_t> map = draw_frame(widget, *data, k_sentinel, "by_map_static.ppm");
    check(info_area_covered(map), "the viewer covers the whole info area");
    screens::galaxy_apply_allegiances(&galaxy, "");
    boneyards_status_widget_tick(widget);
    check(galaxy.static_frame == 0 && !galaxy.loading, "the static stops once the data arrives");
    boneyards_status_widget_destroy(widget);

    // BYConsoleSlide is held to 1..40.
    const int32_t settings[][2] = {{3, 3}, {99, 40}, {0, 1}};
    for (const auto& setting : settings) {
        g_log = Log{};
        g_log.slide_setting = setting[0];
        widget = enter_boneyards(data);
        check(
            widget != nullptr && widget->system.main_frame->console->slide_steps == setting[1],
            "BYConsoleSlide clamp"
        );
        boneyards_status_widget_destroy(widget);
    }
}

void test_slide(Data* data) {
    g_log = Log{};
    StatusWidget* widget = enter_boneyards(data);
    if (widget == nullptr)
        return;
    MainFrame* frame = widget->system.main_frame;
    ConsoleLayer* layer = frame->console;
    // Keep the update console out of the way.
    frame->login_countdown = 1000;
    draw_frame(widget, *data, 0, nullptr);

    // A known viewer pattern: the layer copies rows 7..319 and remaps them.
    Surface viewer{};
    Surface image{};
    {
        ScopedServices scope(widget);
        check(image_surface(frame->map_panel->base.states.images[1], &viewer), "viewer pixels");
        check(image_surface(layer->base.states.images[1], &image), "layer pixels");
    }
    std::vector<uint8_t> saved(viewer.pixels, viewer.pixels + viewer.width * viewer.height);
    for (int32_t y = 0; y < viewer.height; ++y) {
        for (int32_t x = 0; x < viewer.width; ++x)
            viewer.pixels[y * viewer.pitch + x] = static_cast<uint8_t>(x + 3 * y);
    }
    g_log.sounds.clear();
    {
        ScopedServices scope(widget);
        main_frame_show_screen(frame, console_screen::login);
    }
    bool copied = true;
    bool remapped = true;
    for (int32_t y = 0; y < 313; ++y) {
        for (int32_t x = 0; x < 498; ++x) {
            const uint8_t map = layer->map->pixels[y * 498 + x];
            copied = copied && map == static_cast<uint8_t>(x + 3 * (y + 7));
            remapped = remapped && layer->green->pixels[y * 498 + x] == k_console_green_remap[map];
        }
    }
    check(copied, "ConMap holds the viewer rows under the layer");
    check(remapped, "ConGreen is ConMap through the green remap");
    std::copy(saved.begin(), saved.end(), viewer.pixels);

    Window* screen = frame->active_screen;
    check(
        layer->base.visible == 1 && layer->covering == 1 && layer->sliding == 1 &&
            layer->countdown == 20 && layer->direction == ConsoleSlide::in,
        "opening a screen starts the green sliding in"
    );
    check(screen != nullptr && screen->visible == 0, "the screen waits hidden");
    check(
        screen != nullptr && screen->x == 23 && screen->y == 126 && screen->width == 498 &&
            screen->height == 313,
        "the screen opens in the layer's rectangle"
    );
    check(played(console_sound::green_slide), "GREEN_SLIDE");
    check(
        widget->navigation->galaxy.locked &&
            frame->mag_slider->base.states.current == button_state::disabled,
        "the slide locks the map"
    );
    for (Window* b : frame->console_buttons)
        check(b->states.current == button_state::disabled, "console buttons disabled");

    // Mid-slide the layer shows the map left of the edge and the green right
    // of it, under the Green_Screen border.
    for (int32_t i = 0; i < 10; ++i) {
        boneyards_status_widget_tick(widget);
        draw_frame(widget, *data, 0, nullptr);
    }
    check(layer->countdown == 10, "ten pulses into the slide");
    const int32_t edge = layer->map_part.x2;
    check(
        edge == 11 * 498 / 20 && layer->green_part.x1 == edge + 1, "the edge follows the countdown"
    );
    const uint8_t* shown = image.pixels;
    const int32_t row = 150;
    check(shown[row * 498 + edge] == layer->map->pixels[row * 498 + edge], "map left of the edge");
    check(
        shown[row * 498 + edge + 1] == layer->green->pixels[row * 498 + edge + 1],
        "green right of the edge"
    );
    const std::vector<uint8_t> slide = draw_frame(widget, *data, k_sentinel, "by_login_slide.ppm");
    check(info_area_covered(slide), "no hole mid-slide");

    for (int32_t i = 0; i < 11; ++i)
        boneyards_status_widget_tick(widget);
    check(
        layer->countdown == -1 && screen->visible == 0 && !played(console_sound::big_button),
        "the countdown passes zero first"
    );
    boneyards_status_widget_tick(widget);
    check(
        layer->sliding == 0 && layer->covering == 1 && screen->visible == 1,
        "the screen shows on the next pulse"
    );
    check(played(console_sound::big_button), "BIGBUTTON");
    check(shown[row * 498 + 5] == layer->green->pixels[row * 498 + 5], "the layer is green");
    const std::vector<uint8_t> covered =
        draw_frame(widget, *data, k_sentinel, "by_login_green.ppm");
    check(info_area_covered(covered), "no hole while covering");

    // A second screen opens at once under the covering layer.
    g_log.sounds.clear();
    screens::galaxy_apply_allegiances(&widget->navigation->galaxy, "");
    {
        ScopedServices scope(widget);
        main_frame_show_screen(frame, console_screen::news);
    }
    check(
        frame->active_screen != nullptr && frame->active_screen->visible == 1 &&
            layer->sliding == 0 && !played(console_sound::green_slide),
        "no slide while the layer covers the map"
    );
    Window* news = frame->console_buttons[static_cast<int32_t>(ConsoleButton::news)];
    check(news->states.current == button_state::unpressed, "buttons back for other screens");

    // Pressing News again closes it: the green slides out and the map
    // unlocks when it has left.
    click_window(widget, news);
    check(
        frame->active_screen == nullptr && frame->console_screen == console_screen::none,
        "the screen closes at once"
    );
    check(
        played(console_sound::green_slide) && layer->sliding == 1 &&
            layer->direction == ConsoleSlide::out && layer->countdown == 20,
        "the green starts sliding out"
    );
    for (int32_t i = 0; i < 21; ++i) {
        boneyards_status_widget_tick(widget);
        check(widget->navigation->galaxy.locked, "the map stays locked during the slide");
    }
    g_log.sounds.clear();
    boneyards_status_widget_tick(widget);
    check(
        layer->sliding == 0 && layer->covering == 0 && layer->base.visible == 0, "the layer leaves"
    );
    check(!widget->navigation->galaxy.locked && frame->unlock_pending == 0, "the map unlocks");
    check(played(console_sound::big_button), "BIGBUTTON as the map returns");
    check(
        frame->console_buttons[static_cast<int32_t>(ConsoleButton::release)]->states.current ==
            button_state::disabled,
        "Release off with no screen open"
    );
    const std::vector<uint8_t> map = draw_frame(widget, *data, k_sentinel, "by_map.ppm");
    check(info_area_covered(map), "no hole once the map returns");
    boneyards_status_widget_destroy(widget);
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the console layer tests");
    Data data;
    data.store.mount(root / "by.ccx");
    data.store.mount(root / "totala1.hpi");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};

    test_green_remap(data);
    test_layer_build(&data);
    test_slide(&data);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("console layer tests passed\n");
    return 0;
}
