// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// REGISTER opens the registration console: the web page's code and address,
// OPEN IN BROWSER where a browser can open, and the in-game route when
// BetaLoginUX offers it. The console draws no field of its own, so the
// console glass shows through it. With --frames DIR after the mod folder,
// the web page is written there as registration-web.png, to compare with
// the design.
#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/console/registration.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/registration.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/png.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int failures = 0;

void check(bool result, const char* text) {
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++failures;
    }
}

char g_opened[160]{};
int g_open_count = 0;
bool g_available = true;

void reset_browser() {
    g_opened[0] = '\0';
    g_open_count = 0;
}

bool stub_available() {
    return g_available;
}

bool stub_open(const char* address) {
    std::snprintf(g_opened, sizeof g_opened, "%s", address != nullptr ? address : "");
    ++g_open_count;
    return true;
}

struct Fixture {
    oa::AssetStore assets;
    oa::Palette palette{};
    int32_t beta = 0;

    explicit Fixture(const std::filesystem::path& root) : assets(root) {}
};

bool read_asset(void* context, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Fixture*>(context)->assets.read(path).bytes;
        return true;
    } catch (...) {
        return false;
    }
}

bool saved_name(void*, const char*, char* out, std::size_t capacity) {
    if (capacity > 0)
        out[0] = '\0';
    return false;
}

int32_t read_number(void* context, const char* key, int32_t fallback) {
    auto* fixture = static_cast<Fixture*>(context);
    if (key != nullptr && std::strcmp(key, kBetaLoginUxKey) == 0)
        return fixture->beta;
    return fallback;
}

void click(StatusWidget* widget, Window* window) {
    if (window == nullptr)
        return;
    Message message{};
    message.x = window->x + window->width / 2;
    message.y = window->y + window->height / 2;
    message.code = MessageCode::left_down;
    message.buttons = button_left;
    boneyards_status_widget_event(widget, message);
    message.code = MessageCode::left_up;
    message.buttons = 0;
    boneyards_status_widget_event(widget, message);
}

const char* text_of(Window* window) {
    if (window == nullptr || window->ops == nullptr || window->ops->state_text == nullptr)
        return "";
    const char* text = window->ops->state_text(window, -1);
    return text != nullptr ? text : "";
}

Window* named(Window* root, const char* name) {
    return root != nullptr ? window_find_named(root, name, nullptr) : nullptr;
}

Window* register_button(Window* screen) {
    if (screen == nullptr)
        return nullptr;
    for (int32_t i = 0; i < screen->children.count; ++i) {
        Window* child = screen->children.items[i];
        if (child != nullptr && child->ops->is_class(class_name::button) &&
            child->y == screen->y + 0x7E)
            return child;
    }
    return nullptr;
}

StatusWidget* open_login(Fixture* fixture) {
    StatusWidgetConfig config{};
    config.files = {fixture, read_asset};
    config.palette = &fixture->palette;
    config.settings.user = fixture;
    config.settings.read_number = read_number;
    config.settings.read_string = saved_name;
    config.service.user = fixture;
    config.service.network_available = [](void*) { return true; };
    config.service.activate = [](void*) { return ActivationResult::ok; };
    config.service.login = [](void*, const LoginRequest*) { return ServiceResult::failed; };
    config.service.cancel_login = [](void*) {};
    config.service.set_events = [](void*, const ServiceEvents*) {};
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget created");
    if (widget == nullptr)
        return nullptr;
    click(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    for (int32_t i = 0; i < 180; ++i)
        boneyards_status_widget_tick(widget);
    check(
        widget->system.main_frame != nullptr &&
            widget->system.main_frame->console_screen == console_screen::login,
        "login reached"
    );
    if (widget->system.main_frame == nullptr ||
        widget->system.main_frame->console_screen != console_screen::login) {
        boneyards_status_widget_destroy(widget);
        return nullptr;
    }
    return widget;
}

struct Screen {
    std::vector<uint8_t> pixels;
    Surface surface{};

    Screen() : pixels(640 * 480, 0) {
        surface.width = surface.pitch = 640;
        surface.height = 480;
        surface.pixels = pixels.data();
        surface.clip = {0, 0, 639, 479};
    }

    uint8_t at(int32_t x, int32_t y) const {
        if (x < 0 || y < 0 || x >= 640 || y >= 480)
            return 0;
        return pixels[static_cast<std::size_t>(y) * 640u + static_cast<std::size_t>(x)];
    }
};

// The folder --frames names, where the web page is written as a picture to
// compare with the design. Empty writes none.
std::filesystem::path g_frames;

// Writes a drawn frame as an indexed PNG in the widget's palette.
void write_picture(const StatusWidget* widget, const Screen& screen, const char* name) {
    if (g_frames.empty() || widget == nullptr || widget->config.palette == nullptr)
        return;
    std::vector<oa::formats::png::Rgb> colors(static_cast<std::size_t>(OA_PALETTE_COLORS));
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const oa::PaletteEntry& entry = widget->config.palette->entries[i];
        colors[static_cast<std::size_t>(i)] = oa::formats::png::Rgb{entry.r, entry.g, entry.b};
    }
    oa::formats::png::Image image;
    image.header.width = 640;
    image.header.height = 480;
    image.header.bit_depth = 8;
    image.header.color_type = oa::formats::png::ColorType::palette;
    image.palette = colors;
    image.rows = screen.pixels;
    std::vector<uint8_t> bytes;
    check(oa::formats::png::write(image, &bytes), "the picture is encoded");
    std::error_code error;
    std::filesystem::create_directories(g_frames, error);
    std::ofstream out(g_frames / name, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    check(out.good(), "the picture is written");
}

// The console glass at a screen pixel: the Green_Screen border where it is
// opaque, otherwise the green console layer's remapped war map; -1 when
// there is no layer.
int expected_glass(const StatusWidget* widget, const Sprite& border, int32_t x, int32_t y) {
    const ConsoleLayer* layer =
        widget->system.main_frame != nullptr ? widget->system.main_frame->console : nullptr;
    const int32_t col = x - main_frame_layout::console_x;
    const int32_t row = y - main_frame_layout::console_y;
    if (layer == nullptr || layer->green == nullptr || col < 0 || row < 0 || col >= border.width ||
        row >= border.height)
        return -1;
    const uint8_t edge = static_cast<const uint8_t*>(border.data)[row * border.width + col];
    if (edge != border.key)
        return edge;
    const Surface& glass = layer->green->surface;
    if (col >= glass.width || row >= glass.height)
        return -1;
    return glass.pixels[static_cast<std::ptrdiff_t>(row) * glass.pitch + col];
}

const Sprite* sprite_of(Handle image) {
    const ImageService* images = services().images;
    if (images == nullptr || images->sprite == nullptr)
        return nullptr;
    return images->sprite(images->self, image);
}

uint8_t sprite_pixel(const Sprite& sprite, int32_t x, int32_t y) {
    const auto* pixels = static_cast<const uint8_t*>(sprite.data);
    return pixels[static_cast<std::ptrdiff_t>(y) * sprite.width + x];
}

// Compares the non-transparent pixels of a frame drawn at (x, y) with the screen.
void expect_frame(
    StatusWidget* widget,
    const Screen& screen,
    const char* file,
    const char* sequence,
    int32_t frame,
    int32_t x,
    int32_t y,
    const char* what,
    bool present
) {
    ScopedServices scope(widget);
    const Handle image = image_load(file, sequence, frame, screens::kWidgetKey);
    const Sprite* sprite = sprite_of(image);
    check(sprite != nullptr && sprite->data != nullptr, what);
    if (sprite == nullptr || sprite->data == nullptr) {
        image_release(image);
        return;
    }
    int32_t opaque = 0;
    int32_t mismatches = 0;
    for (int32_t row = 0; row < sprite->height; ++row) {
        for (int32_t column = 0; column < sprite->width; ++column) {
            const uint8_t value = sprite_pixel(*sprite, column, row);
            if (value == sprite->key)
                continue;
            ++opaque;
            const int32_t sx = x - sprite->origin_x + column;
            const int32_t sy = y - sprite->origin_y + row;
            if (screen.at(sx, sy) != value)
                ++mismatches;
        }
    }
    image_release(image);
    char message[160];
    if (present) {
        std::snprintf(
            message, sizeof message, "%s matches (%d opaque, %d differ)", what, opaque, mismatches
        );
        check(opaque > 8 && mismatches == 0, message);
    } else {
        std::snprintf(
            message, sizeof message, "%s is absent (%d opaque, %d differ)", what, opaque, mismatches
        );
        check(opaque > 8 && mismatches * 2 > opaque, message);
    }
}

void expect_caps(const Screen& screen, const Window* button, const Sprite& face, const char* what) {
    char message[180];
    std::snprintf(message, sizeof message, "%s face is %dx%d", what, face.width, face.height);
    check(face.data != nullptr && face.width > 24 && face.height > 20, message);
    if (face.data == nullptr || face.width <= 24 || face.height <= 20 || button == nullptr)
        return;
    const int32_t rows[] = {0, 1, 20};
    bool matched = true;
    bool any = false;
    int32_t bad_x = -1;
    int32_t bad_y = -1;
    int32_t bad_got = 0;
    int32_t bad_want = 0;
    auto consider = [&](int32_t x, int32_t y, uint8_t want) {
        if (want == face.key)
            return;
        any = true;
        if (screen.at(x, y) == want)
            return;
        matched = false;
        if (bad_x < 0) {
            bad_x = x;
            bad_y = y;
            bad_got = screen.at(x, y);
            bad_want = want;
        }
    };
    for (int32_t row : rows) {
        for (int32_t column = 0; column < 12; ++column) {
            consider(
                button->x - face.origin_x + column,
                button->y - face.origin_y + row,
                sprite_pixel(face, column, row)
            );
            consider(
                button->x - face.origin_x + 148 + column,
                button->y - face.origin_y + row,
                sprite_pixel(face, face.width - 12 + column, row)
            );
        }
        const int32_t repeat = 12 + (face.width - 24);
        const uint8_t middle = sprite_pixel(face, 12, row);
        consider(button->x - face.origin_x + 12, button->y - face.origin_y + row, middle);
        consider(button->x - face.origin_x + repeat, button->y - face.origin_y + row, middle);
    }
    std::snprintf(
        message,
        sizeof message,
        "%s keeps the widened button caps (at %d,%d saw %d want %d)",
        what,
        bad_x,
        bad_y,
        bad_got,
        bad_want
    );
    check(any && matched, message);
}

void test_model() {
    check(
        std::strcmp(screens::kRegistrationAddress, "https://coreprime.net/register") == 0,
        "build address is https://coreprime.net/register"
    );
    screens::RegistrationConsole plain{};
    screens::registration_init(&plain, nullptr, 0, 0);
    check(
        std::strcmp(plain.address, "https://coreprime.net/register") == 0,
        "null address uses the build setting"
    );
    check(std::strcmp(plain.host, "coreprime.net") == 0, "host drops the scheme");

    screens::RegistrationConsole path{};
    screens::registration_init(&path, "https://register.example.test/path?q=1#h", 1, 0);
    check(
        std::strcmp(path.address, "https://register.example.test/path?q=1#h") == 0,
        "the console keeps the address it was given"
    );
    check(std::strcmp(path.host, "register.example.test") == 0, "host stops at the path");
    screens::registration_select(&path, 1);
    check(path.route == 0, "the in-game route is ignored until it is offered");

    screens::RegistrationConsole both{};
    screens::registration_init(&both, "http://register.example.test?x=1", 1, 1);
    check(std::strcmp(both.host, "register.example.test") == 0, "host stops at a query");
    screens::registration_select(&both, 1);
    check(both.route == 1, "the in-game route can be selected");
    screens::registration_select(&both, 0);
    check(both.route == 0, "the web route can be selected again");

    g_available = true;
    reset_browser();
    screens::registration_set_web_entry({stub_available, stub_open});
    screens::registration_init(&plain, nullptr, 1, 0);
    screens::registration_open_browser(&plain);
    check(
        g_open_count == 1 && std::strcmp(g_opened, screens::kRegistrationAddress) == 0,
        "opening the browser passes the build address"
    );
    check(
        std::strcmp(plain.opened_address, screens::kRegistrationAddress) == 0,
        "the console records that address"
    );

    reset_browser();
    screens::registration_init(&path, "https://register.example.test", 0, 0);
    screens::registration_open_browser(&path);
    check(g_open_count == 0, "a console without a browser does not open one");
    screens::registration_set_web_entry({});
    screens::registration_init(&path, "https://register.example.test", 1, 0);
    screens::registration_open_browser(&path);
    check(path.open_count == 0, "no open call leaves the browser unopened");
}

bool read_archive(void* context, const char* path, std::vector<uint8_t>* out) {
    if (context == nullptr || path == nullptr || out == nullptr)
        return false;
    oa::base::bytes::Decoded<std::vector<uint8_t>> read =
        static_cast<oa::HpiArchive*>(context)->read(path);
    if (!read.ok() || !read.value.has_value())
        return false;
    *out = std::move(*read.value);
    return true;
}

void expect_point(const GuiScreen* screen, const char* name, int32_t x, int32_t y) {
    const GuiControl* control = gui_screen_find(screen, name);
    char message[120];
    std::snprintf(message, sizeof message, "%s is at %d,%d", name, x, y);
    check(control != nullptr && control->x == x && control->y == y, message);
}

void expect_rect(
    const GuiScreen* screen, const char* name, int32_t x, int32_t y, int32_t width, int32_t height
) {
    const GuiControl* control = gui_screen_find(screen, name);
    char message[140];
    std::snprintf(message, sizeof message, "%s is %d,%d %dx%d", name, x, y, width, height);
    check(
        control != nullptr && control->x == x && control->y == y && control->width == width &&
            control->height == height,
        message
    );
}

void test_gui(StatusWidget* widget) {
    ScopedServices scope(widget);
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load("gui/registration.gui", &error);
    check(screen != nullptr, error.message[0] != '\0' ? error.message : "registration.gui loads");
    if (screen == nullptr)
        return;
    expect_rect(screen, "Registration", 23, 126, 498, 313);
    expect_point(screen, "Title", 23, 132);
    expect_point(screen, "WebLabel", 40, 151);
    expect_point(screen, "GameLabel", 40, 168);
    expect_point(screen, "Body0", 29, 200);
    expect_point(screen, "Heading", 160, 136);
    expect_point(screen, "Host", 290, 204);
    expect_point(screen, "Url", 290, 222);
    expect_point(screen, "Deck0", 290, 252);
    expect_rect(screen, "WebIcon", 26, 150, 9, 9);
    expect_rect(screen, "GameIcon", 26, 167, 9, 9);
    expect_rect(screen, "Qr", 160, 190, 113, 113);
    expect_rect(screen, "WebHot", 23, 147, 107, 17);
    expect_rect(screen, "GameHot", 23, 164, 107, 17);
    expect_rect(screen, "Open", 290, 248, 160, 21);
    expect_rect(screen, "Back", 160, 392, 160, 21);
    expect_rect(screen, "Rule", 146, 129, 1, 307);
    const GuiControl* open = gui_screen_find(screen, "Open");
    const GuiControl* back = gui_screen_find(screen, "Back");
    const GuiControl* web = gui_screen_find(screen, "WebHot");
    const GuiControl* game = gui_screen_find(screen, "GameHot");
    check(
        open != nullptr && std::strcmp(open->action, "open:browser") == 0, "OPEN IN BROWSER action"
    );
    check(
        back != nullptr && std::strcmp(back->action, "goto:login_console") == 0,
        "BACK TO LOGIN action"
    );
    check(web != nullptr && std::strcmp(web->action, "route:web") == 0, "web route action");
    check(game != nullptr && std::strcmp(game->action, "route:game") == 0, "in-game route action");
    check(
        open != nullptr && std::strcmp(open->caption, "OPEN IN BROWSER") == 0,
        "OPEN IN BROWSER caption"
    );
    check(
        std::strcmp(text_of(named(gui_screen_root(screen), "Host")), "coreprime.net") == 0 ||
            (gui_screen_find(screen, "Host") != nullptr &&
             std::strcmp(gui_screen_find(screen, "Host")->caption, "coreprime.net") == 0),
        "gadget host is coreprime.net"
    );
    gui_screen_destroy(screen);
}

Window* open_registration(StatusWidget* widget) {
    Window* screen = widget->system.main_frame->active_screen;
    click(widget, register_button(screen));
    Window* registration = named(screen, "Registration");
    check(
        registration != nullptr && window_is_shown(registration),
        "REGISTER opens the registration console"
    );
    check(!boneyards_status_widget_message_open(widget), "REGISTER leaves the message box closed");
    return registration;
}

void test_pixels(StatusWidget* widget, Window* registration, bool browser, bool beta) {
    Screen screen;
    desktop_set_focus(&widget->desktop, nullptr);
    boneyards_status_widget_draw(widget, &screen.surface);
    if (browser && !beta)
        write_picture(widget, screen, "registration-web.png");
    expect_frame(widget, screen, "BY_New.gaf", "QR_Register", 0, 160, 190, "QR code", true);
    expect_frame(
        widget, screen, "Widgets.gaf", "Icon_Circle", 1, 26, 150, "selected web row", true
    );
    expect_frame(widget, screen, "Widgets.gaf", "Icon_Circle", 2, 26, 167, "in-game row", beta);
    {
        ScopedServices scope(widget);
        const Handle face =
            image_load("Widgets.gaf", "Function_Button_Large", 2, screens::kWidgetKey);
        const Sprite* sprite = sprite_of(face);
        if (sprite != nullptr && browser) {
            expect_caps(screen, named(registration, "Open"), *sprite, "OPEN IN BROWSER");
            expect_caps(screen, named(registration, "Back"), *sprite, "BACK TO LOGIN");
        }
        if (sprite != nullptr && !browser) {
            const Window* open = named(registration, "Open");
            bool differs = false;
            if (open != nullptr && sprite->height > 1 && sprite->width > 12) {
                for (int32_t column = 0; column < 12 && !differs; ++column) {
                    const uint8_t value = sprite_pixel(*sprite, column, 1);
                    if (value != sprite->key && screen.at(open->x + column, open->y + 1) != value)
                        differs = true;
                }
            }
            check(differs, "the browser button is not drawn when no browser can open");
        }
        image_release(face);
        // No field of its own: the console glass shows round the page's text.
        const Handle green = image_load("Widgets.gaf", "Green_Screen", 0, screens::kWidgetKey);
        const Sprite* ground = sprite_of(green);
        check(
            ground != nullptr && ground->width > 480 && ground->height > 300, "Green_Screen frame"
        );
        if (ground != nullptr && ground->width > 480 && ground->height > 300) {
            int32_t off = 0;
            for (int32_t y = 300; y < 380; ++y) {
                for (int32_t x = 330; x < 500; ++x) {
                    const int want = expected_glass(widget, *ground, x, y);
                    off += want < 0 || screen.at(x, y) != want ? 1 : 0;
                }
            }
            char message[80];
            std::snprintf(message, sizeof message, "%d pixels are not the console glass", off);
            check(off == 0, message);
        }
        image_release(green);
    }
    const int32_t rule_rows[] = {180, 320, 420, 435};
    for (int32_t row : rule_rows) {
        char message[80];
        std::snprintf(message, sizeof message, "rule at y %d is %u", row, screen.at(146, row));
        check(screen.at(146, row) == 93, message);
    }
    if (browser) {
        ScopedServices scope(widget);
        Window* host = named(registration, "Host");
        Surface* kit = toolkit_surface(widget->toolkit);
        check(
            host != nullptr && kit != nullptr && kit->pixels != nullptr,
            "host label and text surface"
        );
        if (host != nullptr && kit != nullptr && kit->pixels != nullptr) {
            std::memset(
                kit->pixels,
                255,
                static_cast<std::size_t>(kit->pitch) * static_cast<std::size_t>(kit->height)
            );
            text_draw_string(
                widget->system.hat_fonts.big,
                0,
                0,
                host->width,
                host->height,
                text_of(host),
                text_style::left | text_style::top
            );
            int32_t opaque = 0;
            bool colour = true;
            const int32_t width = host->width < kit->width ? host->width : kit->width;
            const int32_t height = host->height < kit->height ? host->height : kit->height;
            for (int32_t y = 0; y < height; ++y) {
                for (int32_t x = 0; x < width; ++x) {
                    const uint8_t ink =
                        kit->pixels[static_cast<std::ptrdiff_t>(y) * kit->pitch + x];
                    if (ink == 255)
                        continue;
                    ++opaque;
                    if (screen.at(host->x + x, host->y + y) != 84)
                        colour = false;
                }
            }
            check(opaque > 0 && colour, "the address is drawn in the bright hat colour");
        }
    }
}

void test_desktop(StatusWidget* widget) {
    ScopedServices scope(widget);
    g_available = true;
    reset_browser();
    Window* registration = open_registration(widget);
    if (registration == nullptr)
        return;
    check(!window_is_shown(named(registration, "GameLabel")), "a single route hides In the game");
    check(!window_is_shown(named(registration, "WebHot")), "a single route has no row control");
    check(
        window_is_shown(named(registration, "WebLabel")) &&
            std::strcmp(text_of(named(registration, "WebLabel")), "On the web") == 0,
        "the web route stays labelled"
    );
    check(std::strcmp(text_of(named(registration, "Host")), "coreprime.net") == 0, "host label");
    check(
        std::strcmp(text_of(named(registration, "Url")), "https://coreprime.net/register") == 0,
        "address label"
    );
    check(window_is_shown(named(registration, "Open")), "OPEN IN BROWSER is shown");
    check(!window_is_shown(named(registration, "Deck0")), "the no-browser lines are hidden");
    check(window_is_shown(named(registration, "Back")), "BACK TO LOGIN is shown");
    check(
        std::strcmp(text_of(named(registration, "Heading")), "ENLIST ON THE WEB") == 0,
        "web heading"
    );
    test_pixels(widget, registration, true, false);

    click(widget, named(registration, "Open"));
    const screens::RegistrationConsole* model =
        registration_model(registration_from_window(registration));
    check(
        g_open_count == 1 && std::strcmp(g_opened, "https://coreprime.net/register") == 0,
        "OPEN IN BROWSER calls the open-address entry with the configured address"
    );
    check(
        model != nullptr && model->open_count == 1 &&
            std::strcmp(model->opened_address, screens::kRegistrationAddress) == 0,
        "the console records the address it opened"
    );

    click(widget, named(registration, "Back"));
    boneyards_status_widget_tick(widget);
    check(
        named(widget->system.main_frame->active_screen, "Registration") == nullptr,
        "BACK TO LOGIN returns to the login console"
    );
    check(register_button(widget->system.main_frame->active_screen) != nullptr, "REGISTER remains");
}

void test_deck(StatusWidget* widget) {
    ScopedServices scope(widget);
    g_available = false;
    reset_browser();
    Window* registration = open_registration(widget);
    if (registration == nullptr)
        return;
    check(!window_is_shown(named(registration, "Open")), "no browser button is shown");
    check(window_is_shown(named(registration, "Qr")), "the code stays");
    check(
        std::strcmp(text_of(named(registration, "Deck0")), "Scan with your phone,") == 0,
        "deck line 1"
    );
    check(
        std::strcmp(text_of(named(registration, "Deck1")), "or type the address on") == 0,
        "deck line 2"
    );
    check(std::strcmp(text_of(named(registration, "Deck2")), "any device.") == 0, "deck line 3");
    check(
        window_is_shown(named(registration, "Deck0")) &&
            window_is_shown(named(registration, "Deck2")),
        "the no-browser lines are shown"
    );
    check(window_is_shown(named(registration, "Back")), "BACK TO LOGIN stays");
    test_pixels(widget, registration, false, false);
    click(widget, named(registration, "Open"));
    check(g_open_count == 0, "a hidden browser button does not open an address");
    click(widget, named(registration, "Back"));
    boneyards_status_widget_tick(widget);
}

void test_custom_address(StatusWidget* widget) {
    ScopedServices scope(widget);
    g_available = true;
    reset_browser();
    RegistrationView* view = nullptr;
    {
        ScopedServices address_scope(widget);
        view = registration_open(
            widget->system.main_frame->active_screen,
            &widget->system.hat_fonts,
            "https://register.example.test",
            0
        );
    }
    check(view != nullptr, "a console can be given an address");
    if (view == nullptr)
        return;
    Window* registration = registration_window(view);
    check(
        std::strcmp(text_of(named(registration, "Host")), "register.example.test") == 0,
        "the host follows the address the console was given"
    );
    click(widget, named(registration, "Open"));
    check(
        g_open_count == 1 && std::strcmp(g_opened, "https://register.example.test") == 0,
        "OPEN IN BROWSER passes the address the console was given"
    );
    click(widget, named(registration, "Back"));
    boneyards_status_widget_tick(widget);
}

void test_beta(StatusWidget* widget) {
    ScopedServices scope(widget);
    g_available = true;
    reset_browser();
    Window* registration = open_registration(widget);
    if (registration == nullptr)
        return;
    check(
        window_is_shown(named(registration, "GameLabel")) &&
            std::strcmp(text_of(named(registration, "GameLabel")), "In the game") == 0,
        "BetaLoginUX adds the in-game route"
    );
    check(
        window_is_shown(named(registration, "WebHot")) &&
            window_is_shown(named(registration, "GameHot")),
        "both routes can be chosen"
    );
    test_pixels(widget, registration, true, true);
    click(widget, named(registration, "GameHot"));
    const screens::RegistrationConsole* model =
        registration_model(registration_from_window(registration));
    check(
        std::strcmp(text_of(named(registration, "Heading")), "ENLIST IN THE GAME") == 0,
        "the in-game route retitles the page"
    );
    check(!window_is_shown(named(registration, "Qr")), "the in-game route hides the code");
    check(
        std::strcmp(text_of(named(registration, "Body0")), "Enlist without leaving") == 0,
        "in-game copy"
    );
    check(model != nullptr && model->route == 1, "the in-game route is selected");
    click(widget, named(registration, "WebHot"));
    check(
        std::strcmp(text_of(named(registration, "Heading")), "ENLIST ON THE WEB") == 0,
        "the web route returns"
    );
    check(window_is_shown(named(registration, "Qr")), "the code returns with the web route");
    check(model != nullptr && model->route == 0, "the web route is selected");
}

bool read_no_mod(void*, const char*, std::vector<uint8_t>*) {
    return false;
}

void test_without_mod(StatusWidget* widget) {
    ScopedServices scope(widget);
    rover_session_set_virtual_files(read_no_mod, nullptr);
    Window* screen = widget->system.main_frame->active_screen;
    click(widget, register_button(screen));
    Window* registration = named(screen, "Registration");
    check(
        registration == nullptr || !window_is_shown(registration),
        "REGISTER without the mod does not open the registration console"
    );
    check(
        boneyards_status_widget_message_open(widget), "REGISTER without the mod opens a message box"
    );
    MessageBox* box = message_box_find_open(&widget->desktop);
    Window* label =
        box != nullptr ? window_find_named(&box->base, message_box_name::text, nullptr) : nullptr;
    check(
        std::strcmp(text_of(label), kRegistrationNeedsMod) == 0,
        "the box says the Registration console needs the Boneyards mod"
    );
}

} // namespace

int main(int argc, char** argv) {
    test_model();
    if (failures != 0)
        return 1;
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the registration console");
    const std::filesystem::path coreprime = oa::boneyards_test::coreprime_archive(argc, argv);
    if (coreprime.empty()) {
        std::fprintf(stderr, "the mod folder is required\n");
        return 1;
    }
    oa::base::bytes::Decoded<oa::HpiArchive> opened = oa::open_hpi_file(coreprime);
    if (!opened.ok() || !opened.value.has_value()) {
        std::fprintf(stderr, "coreprime.ccx could not be opened\n");
        return 1;
    }
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0)
            g_frames = std::filesystem::path(argv[i + 1]);
    }
    Fixture fixture(root);
    fixture.assets.mount(root / "by.ccx");
    fixture.assets.mount(root / "totala1.hpi");
    fixture.assets.mount(coreprime);
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    check(raw.size() >= 256u * 4u, "palette");
    if (raw.size() >= 256u * 4u) {
        for (int32_t i = 0; i < 256; ++i)
            fixture.palette.entries[i] = {
                raw[static_cast<std::size_t>(i) * 4],
                raw[static_cast<std::size_t>(i) * 4 + 1],
                raw[static_cast<std::size_t>(i) * 4 + 2],
                0
            };
    }
    screens::registration_set_web_entry({stub_available, stub_open});
    rover_session_set_virtual_files(read_archive, &*opened.value);
    StatusWidget* widget = open_login(&fixture);
    if (widget != nullptr) {
        test_gui(widget);
        test_desktop(widget);
        test_deck(widget);
        test_custom_address(widget);
        boneyards_status_widget_destroy(widget);
    }
    fixture.beta = 1;
    widget = open_login(&fixture);
    if (widget != nullptr) {
        test_beta(widget);
        boneyards_status_widget_destroy(widget);
    }
    fixture.beta = 0;
    rover_session_set_virtual_files(read_archive, &*opened.value);
    widget = open_login(&fixture);
    if (widget != nullptr) {
        test_without_mod(widget);
        boneyards_status_widget_destroy(widget);
    }
    screens::registration_set_web_entry({});
    rover_session_set_virtual_files(nullptr, nullptr);
    if (failures != 0) {
        std::fprintf(stderr, "%d registration failure(s)\n", failures);
        return 1;
    }
    std::printf("registration tests passed\n");
    return 0;
}
