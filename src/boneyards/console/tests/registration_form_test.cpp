// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The in-game registration form: the checks, the sending lines, the answer
// this server gives, and the picture against the enlistment designs. With
// --frames DIR after the mod folder, the form, the form while sending and the
// two answers are written there as registration-form.png,
// registration-sending.png, registration-unavailable.png and
// registration-registered.png, to compare with the design.
#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/console/registration.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
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

void press_key(StatusWidget* widget, int32_t key_code) {
    Message message{};
    message.code = MessageCode::key_down;
    message.key = key_code;
    boneyards_status_widget_event(widget, message);
    message.code = MessageCode::key_up;
    boneyards_status_widget_event(widget, message);
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

void put(char* out, std::size_t capacity, const char* text) {
    std::snprintf(out, capacity, "%s", text != nullptr ? text : "");
}

void fill_ready(screens::RegistrationForm* form) {
    put(form->name, sizeof form->name, "Commander_Vex");
    put(form->password, sizeof form->password, "password");
    put(form->confirm, sizeof form->confirm, "password");
    put(form->email, sizeof form->email, "vex@example.net");
    form->side = screens::registration_side::core;
    form->rules = 1;
}

void test_model() {
    screens::RegistrationForm form{};
    screens::registration_form_init(&form);
    check(form.side == screens::registration_side::core, "Core starts selected");
    check(
        form.rules == 0 && form.sending == 0 && form.reply == screens::registration_reply::none,
        "the rules start unaccepted and nothing is sending"
    );
    check(form.check == screens::registration_check::empty_name, "an empty form asks for a name");
    check(std::strcmp(form.status[2], "Enter a name.") == 0, "the idle verdict asks for a name");

    screens::registration_form_review(&form);
    check(form.check == screens::registration_check::empty_name, "spaces are not a name");
    put(form.name, sizeof form.name, "   ");
    screens::registration_form_submit(&form);
    check(
        form.check == screens::registration_check::empty_name && form.sending == 0,
        "a blank name is not sent"
    );

    put(form.name, sizeof form.name, "abcdefghijklmnop");
    check(
        screens::registration_form_submit(&form) == screens::registration_check::name_too_long,
        "a name longer than 15 characters is refused"
    );
    check(
        std::strcmp(form.status[2], "Choose a name of 3 to 15 characters.") == 0,
        "a long name is told the length"
    );

    put(form.name, sizeof form.name, "Al");
    check(
        screens::registration_form_submit(&form) == screens::registration_check::name_too_short,
        "a name shorter than 3 characters is refused"
    );

    char long_mark[40];
    int32_t used = 0;
    for (int32_t i = 0; i < 16; ++i) {
        long_mark[used++] = static_cast<char>(0xC3);
        long_mark[used++] = static_cast<char>(0xA9);
    }
    long_mark[used] = '\0';
    put(form.name, sizeof form.name, long_mark);
    check(
        screens::registration_form_submit(&form) == screens::registration_check::name_too_long,
        "length counts characters, so sixteen marks are too long"
    );

    put(form.name, sizeof form.name, "\xC3\xA4\xC3\xB6\xC3\xBC");
    put(form.password, sizeof form.password, "password");
    put(form.confirm, sizeof form.confirm, "other");
    check(
        screens::registration_form_submit(&form) == screens::registration_check::password_mismatch,
        "passwords that differ are refused"
    );
    check(std::strcmp(form.status[2], "The passwords do not match.") == 0, "the mismatch is named");

    put(form.confirm, sizeof form.confirm, "pass'word");
    put(form.password, sizeof form.password, "pass'word");
    check(
        screens::registration_form_submit(&form) == screens::registration_check::password_mark,
        "an apostrophe in a password is refused"
    );

    form.password[0] = '\0';
    form.confirm[0] = '\0';
    check(
        screens::registration_form_submit(&form) == screens::registration_check::password_empty,
        "an empty password is refused"
    );

    put(form.password, sizeof form.password, "password");
    put(form.confirm, sizeof form.confirm, "password");
    put(form.email, sizeof form.email, "a@b.c");
    check(
        screens::registration_form_submit(&form) == screens::registration_check::bad_email,
        "an implausible e-mail is refused"
    );
    put(form.email, sizeof form.email, "vex@example.net");
    form.rules = 0;
    check(
        screens::registration_form_submit(&form) == screens::registration_check::rules,
        "the rules must be accepted"
    );
    check(std::strcmp(form.status[2], "Accept the Rules of Conduct.") == 0, "the rules are named");

    form.rules = 1;
    check(
        screens::registration_form_submit(&form) == screens::registration_check::ok,
        "three marks are a long enough name"
    );
    check(
        form.sending == 1 && form.status_count == 4 && form.status_bright == 3,
        "a form that passes is sending"
    );
    check(std::strcmp(form.status[0], "Contacting Central Command") == 0, "sending line 1");
    check(std::strcmp(form.status[1], "Contact Established") == 0, "sending line 2");
    check(
        std::strcmp(form.status[2], "Checking name \xC3\xA4\xC3\xB6\xC3\xBC") == 0,
        "sending names the player"
    );
    check(std::strcmp(form.status[3], "Creating dossier...") == 0, "sending line 4");
    const char* held = form.status[3];
    check(
        screens::registration_form_submit(&form) == screens::registration_check::ok &&
            form.status[3] == held,
        "sending again leaves the request as it is"
    );

    screens::registration_form_answer(&form, screens::registration_reply::registered);
    form.sending = 0;
    const int32_t ignored = form.reply;
    screens::registration_form_answer(&form, screens::registration_reply::name_taken);
    check(form.reply == ignored, "an answer is kept only while a request is out");
    form.sending = 1;
    screens::registration_form_receive(&form);
    check(
        form.reply == screens::registration_reply::unavailable && form.sending == 1,
        "this server answers that it does not take the form"
    );
    check(
        std::strcmp(form.status[3], "Creating dossier...") == 0,
        "the answer leaves the sending lines"
    );

    char reply[160];
    screens::registration_reply_text(
        screens::registration_reply::unavailable, form.name, reply, sizeof reply
    );
    check(
        std::strcmp(
            reply,
            "This server does not take\\nregistrations from the game yet.\\n"
            "Please use the web page."
        ) == 0,
        "the unavailable answer points at the web page"
    );
    screens::registration_reply_text(
        screens::registration_reply::name_taken, form.name, reply, sizeof reply
    );
    check(
        std::strcmp(reply, "That name is taken.\\nPlease choose another.") == 0,
        "a taken name asks for another"
    );
    screens::registration_reply_text(
        screens::registration_reply::registered, "Commander_Vex", reply, sizeof reply
    );
    check(
        std::strcmp(reply, "Welcome, Commander_Vex!\\nYour dossier is ready.\\nLog in to begin.") ==
            0,
        "a finished dossier welcomes the name"
    );

    screens::registration_form_abandon(&form);
    check(
        form.sending == 0 && form.reply == screens::registration_reply::none &&
            form.check == screens::registration_check::ok &&
            std::strcmp(form.status[2], "Ready to enlist.") == 0,
        "abandoning returns to the idle lines"
    );
    check(
        screens::registration_form_submit(nullptr) == screens::registration_check::bad_email,
        "a missing form is not sent"
    );
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

// The folder --frames names, where the faces are written as pictures to
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

// Counts the pixels of a rectangle's one-pixel outline that are not `color`.
int32_t
off_outline(const Screen& screen, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color) {
    int32_t off = 0;
    for (int32_t i = 0; i < w; ++i) {
        off += screen.at(x + i, y) != color ? 1 : 0;
        off += screen.at(x + i, y + h - 1) != color ? 1 : 0;
    }
    for (int32_t i = 0; i < h; ++i) {
        off += screen.at(x, y + i) != color ? 1 : 0;
        off += screen.at(x + w - 1, y + i) != color ? 1 : 0;
    }
    return off;
}

// Counts the pixels of an area that are not the console glass: the
// Green_Screen border where it is opaque, otherwise the green console
// layer's remapped war map.
int32_t off_glass(
    const StatusWidget* widget, const Screen& screen, int32_t x0, int32_t y0, int32_t x1, int32_t y1
) {
    const ConsoleLayer* layer =
        widget->system.main_frame != nullptr ? widget->system.main_frame->console : nullptr;
    const Handle green = image_load("Widgets.gaf", "Green_Screen", 0, k_overlay_transparent);
    Surface border{};
    int32_t off = 0;
    const bool ready = layer != nullptr && layer->green != nullptr && green >= 0 &&
                       image_surface(green, &border) && border.pixels != nullptr;
    for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
            const int32_t col = x - main_frame_layout::console_x;
            const int32_t row = y - main_frame_layout::console_y;
            int want = -1;
            if (ready && col >= 0 && row >= 0 && col < border.width && row < border.height) {
                const uint8_t edge =
                    border.pixels[static_cast<std::ptrdiff_t>(row) * border.pitch + col];
                const Surface& glass = layer->green->surface;
                if (edge != k_overlay_transparent)
                    want = edge;
                else if (col < glass.width && row < glass.height)
                    want = glass.pixels[static_cast<std::ptrdiff_t>(row) * glass.pitch + col];
            }
            off += want < 0 || screen.at(x, y) != want ? 1 : 0;
        }
    }
    if (green >= 0)
        image_release(green);
    return off;
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

void expect_frame(
    StatusWidget* widget,
    const Screen& screen,
    const char* file,
    const char* sequence,
    int32_t frame,
    int32_t x,
    int32_t y,
    const char* what
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
            if (screen.at(x - sprite->origin_x + column, y - sprite->origin_y + row) != value)
                ++mismatches;
        }
    }
    image_release(image);
    char message[160];
    std::snprintf(
        message, sizeof message, "%s matches (%d opaque, %d differ)", what, opaque, mismatches
    );
    check(opaque > 8 && mismatches == 0, message);
}

void expect_ink(
    StatusWidget* widget,
    const Screen& screen,
    Window* label,
    int32_t style,
    uint8_t colour,
    const char* what
) {
    ScopedServices scope(widget);
    Surface* kit = toolkit_surface(widget->toolkit);
    check(label != nullptr && kit != nullptr && kit->pixels != nullptr, what);
    if (label == nullptr || kit == nullptr || kit->pixels == nullptr)
        return;
    std::memset(
        kit->pixels,
        255,
        static_cast<std::size_t>(kit->pitch) * static_cast<std::size_t>(kit->height)
    );
    text_draw_string(label->font, 0, 0, label->width, label->height, text_of(label), style);
    int32_t opaque = 0;
    int32_t mismatches = 0;
    int32_t wrong = 0;
    const int32_t width = label->width < kit->width ? label->width : kit->width;
    const int32_t height = label->height < kit->height ? label->height : kit->height;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const uint8_t ink = kit->pixels[static_cast<std::ptrdiff_t>(y) * kit->pitch + x];
            if (ink == 255)
                continue;
            ++opaque;
            if (ink != colour)
                ++wrong;
            if (screen.at(label->x + x, label->y + y) != ink)
                ++mismatches;
        }
    }
    char message[180];
    std::snprintf(
        message,
        sizeof message,
        "%s ink (%d opaque, %d differ, %d off colour %u)",
        what,
        opaque,
        mismatches,
        wrong,
        colour
    );
    check(opaque > 0 && mismatches == 0 && wrong == 0, message);
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

void test_geometry(StatusWidget* widget) {
    ScopedServices scope(widget);
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load("gui/registration_form.gui", &error);
    check(
        screen != nullptr, error.message[0] != '\0' ? error.message : "registration_form.gui loads"
    );
    if (screen == nullptr)
        return;
    expect_rect(screen, "Enlist", 23, 126, 498, 313);
    expect_rect(screen, "Rule", 146, 129, 1, 307);
    expect_rect(screen, "Heading", 160, 136, 230, 16);
    expect_rect(screen, "UserLabel", 175, 164, 70, 14);
    expect_rect(screen, "User", 252, 160, 150, 21);
    expect_rect(screen, "Pass", 252, 184, 150, 21);
    expect_rect(screen, "Confirm", 252, 208, 150, 21);
    expect_rect(screen, "Email", 252, 232, 200, 21);
    expect_rect(screen, "ArmRadio", 252, 263, 11, 11);
    expect_rect(screen, "CoreRadio", 302, 263, 11, 11);
    expect_rect(screen, "OpenRadio", 356, 263, 11, 11);
    expect_rect(screen, "ArmLogo", 263, 262, 12, 12);
    expect_rect(screen, "CoreLogo", 313, 262, 12, 12);
    expect_rect(screen, "RulesBox", 252, 285, 12, 12);
    expect_rect(screen, "RulesLink", 264, 284, 170, 12);
    expect_rect(screen, "Register", 252, 308, 99, 21);
    expect_rect(screen, "Cancel", 360, 308, 99, 21);
    expect_rect(screen, "WebHot", 23, 147, 107, 17);
    expect_rect(screen, "GameHot", 23, 164, 107, 17);
    expect_rect(screen, "Status", 160, 336, 346, 70);
    expect_rect(screen, "StatusBox", 160, 336, 350, 70);
    Window* pass = named(gui_screen_root(screen), "Pass");
    Window* user = named(gui_screen_root(screen), "User");
    check(
        pass != nullptr && pass->ops->is_class(class_name::edit_box) &&
            reinterpret_cast<EditBox*>(pass)->mask == '*' &&
            reinterpret_cast<EditBox*>(pass)->padding == 6,
        "the password draws a star and keeps a six-pixel inset"
    );
    check(
        user != nullptr && reinterpret_cast<EditBox*>(user)->mask == 0 &&
            reinterpret_cast<EditBox*>(user)->padding == 6,
        "the name is shown and keeps a six-pixel inset"
    );
    const GuiControl* rules = gui_screen_find(screen, "RulesLink");
    const GuiControl* toggle = gui_screen_find(screen, "RulesHot");
    check(
        rules != nullptr && std::strcmp(rules->action, "rules:page") == 0,
        "the rules sentence opens the page"
    );
    check(
        toggle != nullptr && std::strcmp(toggle->action, "rules:toggle") == 0,
        "the box accepts the rules"
    );
    gui_screen_destroy(screen);
}

Window* open_registration(StatusWidget* widget) {
    Window* screen = widget->system.main_frame->active_screen;
    click(widget, register_button(screen));
    return named(screen, "Registration");
}

void set_edit(Window* edit, const char* text) {
    if (edit != nullptr && edit->ops != nullptr && edit->ops->set_caption != nullptr)
        edit->ops->set_caption(edit, text, text_style::left | text_style::top);
}

Window* status_label(Window* list, int32_t index) {
    if (list == nullptr)
        return nullptr;
    int32_t seen = 0;
    for (int32_t i = 0; i < list->children.count; ++i) {
        Window* child = list->children.items[i];
        if (child == nullptr || (child->name != nullptr && std::strcmp(child->name, "slider") == 0))
            continue;
        if (seen == index)
            return child->children.count > 0 ? child->children.items[0] : nullptr;
        ++seen;
    }
    return nullptr;
}

void test_closed(StatusWidget* widget) {
    ScopedServices scope(widget);
    Window* registration = open_registration(widget);
    check(registration != nullptr, "REGISTER opens the page");
    if (registration == nullptr)
        return;
    RegistrationView* view = registration_from_window(registration);
    check(
        registration_form_window(view) == nullptr,
        "the form stays unbuilt without the in-game route"
    );
    check(!window_is_shown(named(registration, "GameHot")), "the in-game row is hidden");
    Window* back = named(registration, "Back");
    Window* open_button = named(registration, "Open");
    Window* opener = register_button(widget->system.main_frame->active_screen);
    check(widget->desktop.focus == opener, "opening the page leaves the keyboard on REGISTER");
    press_key(widget, key::tab);
    Window* landed = widget->desktop.focus;
    check(
        landed != nullptr && (landed == back || landed == open_button),
        "the first Tab lands on the page"
    );
    Window* username = named(widget->system.main_frame->active_screen, "Username");
    press_key(widget, key::tab);
    bool on_page = false;
    for (Window* cursor = widget->desktop.focus; cursor != nullptr; cursor = cursor->parent)
        on_page = on_page || cursor == registration;
    check(on_page && widget->desktop.focus != username, "Tab stays on the registration page");
    click(widget, named(registration, "Back"));
    check(
        widget->desktop.focus != opener,
        "a click on Back leaves REGISTER without the keyboard, as before"
    );
    boneyards_status_widget_tick(widget);
    // A key that closes the page hands the keyboard back to REGISTER.
    Window* again = open_registration(widget);
    check(again != nullptr, "REGISTER opens the page again");
    if (again == nullptr)
        return;
    Window* again_back = named(again, "Back");
    for (int32_t i = 0; i < 3 && widget->desktop.focus != again_back; ++i)
        press_key(widget, key::tab);
    check(widget->desktop.focus == again_back, "Tab reaches Back");
    press_key(widget, key::escape);
    // The next pass destroys the closed page, so it is read before the tick.
    check(!window_is_shown(again), "Escape closes the page");
    check(widget->desktop.focus == opener, "Escape on Back hands the keyboard back to REGISTER");
    boneyards_status_widget_tick(widget);
}

void paint_sample(RegistrationView* view) {
    screens::RegistrationForm* form = registration_form(view);
    fill_ready(form);
    screens::registration_form_review(form);
    registration_form_apply(view);
}

void test_picture(StatusWidget* widget, RegistrationView* view) {
    Window* form = registration_form_window(view);
    Screen screen;
    desktop_set_focus(&widget->desktop, nullptr);
    boneyards_status_widget_draw(widget, &screen.surface);
    write_picture(widget, screen, "registration-form.png");
    expect_frame(widget, screen, "Widgets.gaf", "Icon_Circle", 2, 26, 150, "web row");
    expect_frame(widget, screen, "Widgets.gaf", "Icon_Circle", 1, 26, 167, "in-game row");
    expect_frame(widget, screen, "Widgets.gaf", "Radio_Button", 2, 252, 263, "Arm off");
    expect_frame(widget, screen, "Widgets.gaf", "Radio_Button", 1, 302, 263, "Core on");
    expect_frame(widget, screen, "Widgets.gaf", "Radio_Button", 2, 356, 263, "Undecided off");
    expect_frame(widget, screen, "Widgets.gaf", "ArmCoreLogos", 0, 263, 262, "Arm mark");
    expect_frame(widget, screen, "Widgets.gaf", "ArmCoreLogos", 1, 313, 262, "Core mark");
    expect_frame(widget, screen, "Widgets.gaf", "Check_Box", 1, 252, 285, "rules accepted");
    expect_frame(widget, screen, "Widgets.gaf", "Function_Button_Large", 2, 252, 308, "REGISTER");
    expect_frame(widget, screen, "Widgets.gaf", "Function_Button_Large", 2, 360, 308, "CANCEL");
    check(screen.at(146, 180) == 93 && screen.at(146, 400) == 93, "the rule is the rule colour");
    // The fields and the status list are outlines in the dim colour, with
    // the console glass inside them, as the design draws them.
    check(off_outline(screen, 252, 160, 150, 21, 90) == 0, "the name field is outlined");
    check(off_outline(screen, 252, 184, 150, 21, 90) == 0, "the password field is outlined");
    check(off_outline(screen, 252, 208, 150, 21, 90) == 0, "the confirmation is outlined");
    check(off_outline(screen, 252, 232, 200, 21, 90) == 0, "the e-mail field is outlined");
    check(off_outline(screen, 160, 336, 350, 70, 90) == 0, "the status list is outlined");
    check(off_glass(widget, screen, 380, 162, 400, 179) == 0, "the name field shows the glass");
    check(off_glass(widget, screen, 400, 386, 508, 404) == 0, "the status list shows the glass");
    check(off_glass(widget, screen, 470, 160, 505, 300) == 0, "the form shows the glass");
    // The status lines sit 14 pixels apart from the list's top.
    for (int32_t i = 0; i < 3; ++i) {
        const Window* line = status_label(named(form, "Status"), i);
        char message[64];
        std::snprintf(message, sizeof message, "status line %d hangs at its row", i);
        check(line != nullptr && line->y == 336 + 14 * i + 3, message);
    }
    // The page under the form keeps its BACK TO LOGIN out of the status list.
    const Window* back = named(registration_window(view), "Back");
    check(back == nullptr || !window_is_shown(back), "the page's BACK TO LOGIN is hidden");
    expect_ink(
        widget,
        screen,
        named(form, "Body0"),
        text_style::left | text_style::top,
        87,
        "the enlistment copy"
    );
    expect_ink(
        widget, screen, named(form, "Dim0"), text_style::left | text_style::top, 90, "the dim copy"
    );
    expect_ink(
        widget,
        screen,
        named(form, "GameLabel"),
        text_style::left | text_style::top,
        84,
        "the selected route"
    );
    expect_ink(
        widget,
        screen,
        status_label(named(form, "Status"), 2),
        text_style::left | text_style::top,
        87,
        "Ready to enlist."
    );
    auto* list = reinterpret_cast<screens::ByListBox*>(named(form, "Status"));
    check(
        list != nullptr && list->slider != nullptr && list->slider->slider.base.visible == 0,
        "the status list has no scroll bar"
    );
    check(
        std::strcmp(
            text_of(status_label(named(form, "Status"), 0)), "Choose a name of 3 to 15 characters."
        ) == 0,
        "the name hint is shown"
    );
    Window* user = named(form, "User");
    check(
        user != nullptr &&
            std::strcmp(reinterpret_cast<EditBox*>(user)->text, "Commander_Vex") == 0,
        "the sample name is shown"
    );
    Window* pass = named(form, "Pass");
    check(
        pass != nullptr && std::strcmp(reinterpret_cast<EditBox*>(pass)->text, "password") == 0 &&
            reinterpret_cast<EditBox*>(pass)->mask == '*',
        "the sample password is kept and masked"
    );
}

void expect_check(StatusWidget* widget, RegistrationView* view, int32_t want, const char* what) {
    click(widget, named(registration_form_window(view), "Register"));
    screens::RegistrationForm* form = registration_form(view);
    char message[80];
    std::snprintf(message, sizeof message, "%s (%d)", what, form != nullptr ? form->check : -1);
    check(
        form != nullptr && form->check == want && form->sending == 0 &&
            !boneyards_status_widget_message_open(widget),
        message
    );
}

void test_form(StatusWidget* widget) {
    ScopedServices scope(widget);
    test_geometry(widget);
    Window* registration = open_registration(widget);
    check(registration != nullptr && window_is_shown(registration), "REGISTER opens the page");
    if (registration == nullptr)
        return;
    RegistrationView* view = registration_from_window(registration);
    check(
        view != nullptr && registration_form_window(view) == nullptr, "the form waits for the route"
    );
    click(widget, named(registration, "GameHot"));
    Window* form = registration_form_window(view);
    check(
        form != nullptr && window_is_shown(form) && form->parent == registration->parent,
        "the in-game route shows the form over the login page"
    );
    check(named(registration, "Enlist") == nullptr, "the form is not a child of the web page");
    check(
        std::strcmp(text_of(named(registration, "Heading")), "ENLIST IN THE GAME") == 0,
        "the page is retitled"
    );
    check(!window_is_shown(named(registration, "Qr")), "the code is hidden");
    check(
        std::strcmp(text_of(named(registration, "Body0")), "Enlist without leaving") == 0,
        "the page copy changes"
    );
    check(registration_model(view)->route == 1, "the in-game route is selected");
    Window* user = named(form, "User");
    press_key(widget, 'Q');
    check(
        user != nullptr && std::strcmp(reinterpret_cast<EditBox*>(user)->text, "Q") == 0,
        "the enlistment form takes typing in its first field"
    );

    paint_sample(view);
    test_picture(widget, view);

    Window* enlist = registration_form_window(view);
    set_edit(named(enlist, "User"), "");
    expect_check(widget, view, screens::registration_check::empty_name, "empty name");
    set_edit(named(enlist, "User"), "abcdefghijklmnop");
    set_edit(named(enlist, "Pass"), "password");
    set_edit(named(enlist, "Confirm"), "password");
    set_edit(named(enlist, "Email"), "vex@example.net");
    expect_check(widget, view, screens::registration_check::name_too_long, "name too long");
    set_edit(named(enlist, "User"), "Commander_Vex");
    set_edit(named(enlist, "Confirm"), "other");
    expect_check(
        widget, view, screens::registration_check::password_mismatch, "passwords don't match"
    );
    set_edit(named(enlist, "Confirm"), "password");
    set_edit(named(enlist, "Email"), "a@b.c");
    expect_check(widget, view, screens::registration_check::bad_email, "bad e-mail");
    set_edit(named(enlist, "Email"), "vex@example.net");
    click(widget, named(enlist, "RulesHot"));
    expect_check(widget, view, screens::registration_check::rules, "rules not accepted");
    click(widget, named(enlist, "RulesLink"));
    check(
        registration_form(view)->rules_page == 1 && registration_form(view)->rules == 0,
        "the rules sentence asks for the page and leaves the box"
    );
    click(widget, named(enlist, "ArmHot"));
    check(registration_form(view)->side == screens::registration_side::arm, "Arm can be chosen");

    fill_ready(registration_form(view));
    check(
        screens::registration_form_submit(registration_form(view)) ==
            screens::registration_check::ok,
        "the sample can be sent"
    );
    registration_form_apply(view);
    check(!boneyards_status_widget_message_open(widget), "sending has no answer yet");
    check(
        named(enlist, "Register")->states.current == button_state::disabled, "REGISTER is disabled"
    );
    Screen sending;
    desktop_set_focus(&widget->desktop, nullptr);
    boneyards_status_widget_draw(widget, &sending.surface);
    write_picture(widget, sending, "registration-sending.png");
    expect_frame(
        widget, sending, "Widgets.gaf", "Function_Button_Large", 0, 252, 308, "REGISTER disabled"
    );
    check(
        std::strcmp(
            text_of(status_label(named(enlist, "Status"), 0)), "Contacting Central Command"
        ) == 0,
        "sending shows the contact line"
    );
    check(
        std::strcmp(
            text_of(status_label(named(enlist, "Status"), 2)), "Checking name Commander_Vex"
        ) == 0,
        "sending names Commander_Vex"
    );
    expect_ink(
        widget,
        sending,
        status_label(named(enlist, "Status"), 3),
        text_style::left | text_style::top,
        233,
        "Creating dossier..."
    );
    click(widget, named(enlist, "Register"));
    check(
        registration_form(view)->sending == 1 && !boneyards_status_widget_message_open(widget),
        "a disabled REGISTER does not send again"
    );
    click(widget, named(enlist, "Cancel"));
    check(
        registration_form(view)->sending == 0 &&
            std::strcmp(registration_form(view)->status[2], "Ready to enlist.") == 0,
        "CANCEL while sending returns to the form"
    );
    check(
        named(enlist, "Register")->states.current == button_state::unpressed,
        "REGISTER is offered again"
    );

    click(widget, named(enlist, "Register"));
    MessageBox* box = message_box_find_open(&widget->desktop);
    Window* body = box != nullptr ? named(&box->base, message_box_name::text) : nullptr;
    check(
        box != nullptr && box->base.x == 0x8E && box->base.y == 0xAC,
        "the answer uses the message box"
    );
    check(
        body != nullptr && std::strstr(text_of(body), "does not take") != nullptr &&
            std::strstr(text_of(body), "web page") != nullptr,
        "the answer says this server does not take registrations"
    );
    {
        Screen answer;
        boneyards_status_widget_draw(widget, &answer.surface);
        write_picture(widget, answer, "registration-unavailable.png");
    }
    Window* ok = box != nullptr ? named(&box->base, message_box_name::ok) : nullptr;
    check(ok != nullptr && ok->x == 222 && ok->y == 278, "Ok sits at the message box's button");
    click(widget, ok);
    boneyards_status_widget_tick(widget);
    check(!boneyards_status_widget_message_open(widget), "Ok closes the answer");
    check(
        registration_model(view)->route == 0 && !window_is_shown(registration_form_window(view)),
        "Ok returns to the web page"
    );
    check(window_is_shown(named(registration, "Qr")), "the code returns");

    click(widget, named(registration, "GameHot"));
    fill_ready(registration_form(view));
    screens::registration_form_submit(registration_form(view));
    screens::registration_form_answer(
        registration_form(view), screens::registration_reply::registered
    );
    registration_form_show_reply(view);
    box = message_box_find_open(&widget->desktop);
    body = box != nullptr ? named(&box->base, message_box_name::text) : nullptr;
    check(
        body != nullptr && std::strstr(text_of(body), "Welcome, Commander_Vex!") != nullptr,
        "a finished dossier welcomes the player"
    );
    {
        Screen answer;
        boneyards_status_widget_draw(widget, &answer.surface);
        write_picture(widget, answer, "registration-registered.png");
    }
    click(widget, box != nullptr ? named(&box->base, message_box_name::ok) : nullptr);
    boneyards_status_widget_tick(widget);
    check(
        named(widget->system.main_frame->active_screen, "Registration") == nullptr,
        "Ok on a finished dossier returns to login"
    );

    registration = open_registration(widget);
    view = registration_from_window(registration);
    click(widget, named(registration, "GameHot"));
    enlist = registration_form_window(view);
    fill_ready(registration_form(view));
    screens::registration_form_submit(registration_form(view));
    screens::registration_form_answer(
        registration_form(view), screens::registration_reply::name_taken
    );
    registration_form_show_reply(view);
    box = message_box_find_open(&widget->desktop);
    body = box != nullptr ? named(&box->base, message_box_name::text) : nullptr;
    check(
        body != nullptr && std::strstr(text_of(body), "That name is taken.") != nullptr,
        "a taken name is reported"
    );
    click(widget, box != nullptr ? named(&box->base, message_box_name::ok) : nullptr);
    check(registration_form(view)->sending == 0 && window_is_shown(enlist), "Ok stays on the form");
    check(
        named(enlist, "User") != nullptr && named(enlist, "User")->focused != 0,
        "Ok gives the name the keyboard"
    );
    check(
        std::strcmp(
            registration_form(view)->status[2], "That name is taken. Please choose another."
        ) == 0,
        "the form asks for another name"
    );
    boneyards_status_widget_tick(widget);
    check(
        window_is_shown(named(widget->system.main_frame->active_screen, "Registration")),
        "the page stays open"
    );
}

} // namespace

int main(int argc, char** argv) {
    test_model();
    if (failures != 0) {
        std::fprintf(stderr, "%d registration form failure(s)\n", failures);
        return 1;
    }
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the registration form");
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
    rover_session_set_virtual_files(read_archive, &*opened.value);
    StatusWidget* widget = open_login(&fixture);
    if (widget != nullptr) {
        test_closed(widget);
        boneyards_status_widget_destroy(widget);
    }
    fixture.beta = 1;
    widget = open_login(&fixture);
    if (widget != nullptr) {
        test_form(widget);
        boneyards_status_widget_destroy(widget);
    }
    rover_session_set_virtual_files(nullptr, nullptr);
    if (failures != 0) {
        std::fprintf(stderr, "%d registration form failure(s)\n", failures);
        return 1;
    }
    std::printf("registration form tests passed\n");
    return 0;
}
