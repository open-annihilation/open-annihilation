// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "console_state.hpp"
#include "console_layer.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/login.hpp"
#include "oa/boneyards/screens/update_pass.hpp"
#include "oa/platform/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {
int failures{};

void check(bool result, const char* text) {
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++failures;
    }
}

struct Fixture {
    oa::AssetStore assets{oa::boneyards_test::by_install()};
    oa::Palette palette{};
    const char* current{"CorePrime Alpha Test"};
    const char* saved{};
    int32_t rows{2};
    int32_t logins{};
    int32_t cancels{};
    char server[0x18]{};
    char saved_name[0x20]{};
    ServiceEvents events{};
};

bool read_asset(void* context, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Fixture*>(context)->assets.read(path).bytes;
        return true;
    } catch (...) {
        return false;
    }
}

void enumerate(void* context, void (*entry)(void*, const char*), void* user) {
    auto* f = static_cast<Fixture*>(context);
    if (f->rows > 0)
        entry(user, "USA, West Coast");
    if (f->rows > 1)
        entry(user, "CorePrime Alpha Test");
    entry(user, "");
}

const char* current(void* context) {
    return static_cast<Fixture*>(context)->current;
}

bool saved(void* context, const char* key, char* out, std::size_t capacity) {
    auto* f = static_cast<Fixture*>(context);
    check(std::strcmp(key, "Username") == 0, "Boneyards Username setting");
    std::snprintf(out, capacity, "%s", f->saved != nullptr ? f->saved : "");
    return f->saved != nullptr;
}

ServiceResult login(void* context, const LoginRequest* request) {
    auto* f = static_cast<Fixture*>(context);
    ++f->logins;
    std::snprintf(
        f->server,
        sizeof f->server,
        "%s",
        request->server_name != nullptr ? request->server_name : ""
    );
    return ServiceResult::failed;
}

void cancel(void* context) {
    ++static_cast<Fixture*>(context)->cancels;
}

void pointer_button(StatusWidget* widget, Window* window, MessageCode code, uint32_t buttons) {
    if (window == nullptr)
        return;
    Message message{};
    message.x = window->x + window->width / 2;
    message.y = window->y + window->height / 2;
    message.code = code;
    message.buttons = buttons;
    boneyards_status_widget_event(widget, message);
}

void click(StatusWidget* widget, Window* window) {
    pointer_button(widget, window, MessageCode::left_down, button_left);
    pointer_button(widget, window, MessageCode::left_up, 0);
}

// A player's click is a press, a later pulse, then the release.
void click_across_pulse(StatusWidget* widget, Window* window) {
    pointer_button(widget, window, MessageCode::left_down, button_left);
    boneyards_status_widget_tick(widget);
    pointer_button(widget, window, MessageCode::left_up, 0);
}

void key_down(StatusWidget* widget, int32_t key_code) {
    Message message{};
    message.code = MessageCode::key_down;
    message.key = key_code;
    boneyards_status_widget_event(widget, message);
}

Window* caption(StatusWidget* widget, Window* parent, const char* text) {
    ScopedServices scope(widget);
    for (int32_t i = 0; i < parent->children.count; ++i) {
        Window* child = parent->children.items[i];
        if (std::strcmp(child->ops->state_text(child, -1), text) == 0)
            return child;
    }
    return nullptr;
}

Window* at(Window* parent, int32_t x, int32_t y) {
    for (int32_t i = 0; i < parent->children.count; ++i) {
        Window* child = parent->children.items[i];
        if (child->x == parent->x + x && child->y == parent->y + y)
            return child;
    }
    return nullptr;
}

StatusWidget* create(Fixture* f) {
    StatusWidgetConfig config{};
    config.files = {f, read_asset};
    config.palette = &f->palette;
    config.settings.user = f;
    config.settings.read_string = saved;
    config.settings.save_string = [](void* context, const char*, const char* value) {
        auto* fixture = static_cast<Fixture*>(context);
        std::snprintf(fixture->saved_name, sizeof fixture->saved_name, "%s", value);
        return true;
    };
    config.service.user = f;
    config.service.network_available = [](void*) { return true; };
    config.service.activate = [](void*) { return ActivationResult::ok; };
    config.service.list_servers = enumerate;
    config.service.current_server = current;
    config.service.login = login;
    config.service.cancel_login = cancel;
    config.service.set_events = [](void* context, const ServiceEvents* events) {
        static_cast<Fixture*>(context)->events = events != nullptr ? *events : ServiceEvents{};
    };
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget created");
    if (widget == nullptr)
        return nullptr;
    click(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    return widget;
}

StatusWidget* open(Fixture* f) {
    StatusWidget* widget = create(f);
    if (widget == nullptr)
        return nullptr;
    for (int32_t i = 0; i < 180; ++i)
        boneyards_status_widget_tick(widget);
    check(
        widget->system.main_frame->console_screen == console_screen::login,
        "login reached through the real navigation"
    );
    return widget;
}

void check_fonts(StatusWidget* widget) {
    ScopedServices scope(widget);
    const auto& f = widget->system.hat_fonts;
    const Handle order[] = {
        f.small,
        f.small_dim,
        f.small_light,
        f.small_green,
        f.small_orange,
        f.small_blue,
        f.small_red,
        f.medium,
        f.button_pressed,
        f.button_normal,
        f.button_hover,
        f.big
    };
    for (int32_t i = 0; i < 12; ++i)
        check(order[i] == order[0] + i, "console_open_main_frame font allocation order");
    const Handle tinted[] = {
        f.small_dim,
        f.small_light,
        f.small_green,
        f.small_orange,
        f.small_blue,
        f.small_red,
        f.button_pressed,
        f.button_normal,
        f.button_hover
    };
    const uint8_t colors[] = {0x5a, 0x54, 0xe9, 0xc7, 0xe2, 0xd4, 0x5d, 0x57, 0x54};
    Surface* surface = toolkit_surface(widget->toolkit);
    for (int32_t i = 0; i < 9; ++i) {
        std::memset(
            surface->pixels, 255, static_cast<std::size_t>(surface->pitch * surface->height)
        );
        text_draw_string(tinted[i], 0, 0, 200, 32, "BONEYARDS", text_style::left | text_style::top);
        int32_t opaque = 0;
        bool exact = true;
        for (int32_t y = 0; y < 32; ++y)
            for (int32_t x = 0; x < 200; ++x) {
                const uint8_t pixel = surface->pixels[y * surface->pitch + x];
                if (pixel == 255)
                    continue;
                ++opaque;
                if (pixel != colors[i])
                    exact = false;
            }
        check(opaque > 0 && exact, "toolkit_font_tint every opaque font pixel has its tint");
    }
}

Handle drawn_font{};

void record_font(void*, Handle font, int32_t, int32_t, int32_t, int32_t, Handle) {
    drawn_font = font;
}

void check_button_and_wrap(StatusWidget* widget, Window* view) {
    ScopedServices scope(widget);
    const auto& fonts = widget->system.hat_fonts;
    Window* button = at(view, 0x6f, 0xf2);
    TextService text = *services().texts;
    text.draw = record_font;
    Services draw_services = services();
    draw_services.texts = &text;
    Services previous = services_install(draw_services);
    const Handle expected[] = {
        fonts.button_pressed, fonts.button_pressed, fonts.button_normal, fonts.button_hover
    };
    for (int32_t state = 0; state < 4; ++state) {
        window_set_state(button, state);
        drawn_font = k_no_handle;
        button->ops->draw(button);
        check(drawn_font == expected[state], "function_button_draw state-to-font mapping");
    }
    services_install(previous);
    auto* list = reinterpret_cast<screens::ByListBox*>(at(view, 0xd5, 0x96));
    list_box_clear(&list->list);
    screens::by_list_wrap_text(list, "alpha\nbeta", &fonts, 3, k_no_handle);
    check(list->list.count == 2, "by_list_wrap_text newline creates two rows");
    auto* first = reinterpret_cast<screens::ByTextRow*>(list_box_item(&list->list, 0));
    auto* second = reinterpret_cast<screens::ByTextRow*>(list_box_item(&list->list, 1));
    int32_t continuation{}, height{};
    text_measure_string(fonts.small, "  ", &continuation, &height);
    check(
        first->label->x == first->base.x + 4 && second->label->x == second->base.x + continuation,
        "by_list_wrap_text first-line four-pixel indent and subsequent two-space indent"
    );
    check(
        std::strcmp(first->label->ops->state_text(first->label, -1), "alpha") == 0 &&
            std::strcmp(second->label->ops->state_text(second->label, -1), "beta") == 0,
        "wrapped text omits separators"
    );
    list_box_clear(&list->list);
}

void snapshot(StatusWidget* widget, const Fixture& f) {
    const auto directory_value = oa::platform::environment_value("OA_SNAPSHOT_DIR");
    const char* directory = directory_value ? directory_value->c_str() : nullptr;
    if (directory == nullptr)
        return;
    std::vector<uint8_t> pixels(640 * 480);
    Surface surface{};
    surface.width = surface.pitch = 640;
    surface.height = 480;
    surface.pixels = pixels.data();
    surface.clip = {0, 0, 639, 479};
    boneyards_status_widget_draw(widget, &surface);
    std::ofstream out(std::filesystem::path(directory) / "by_login_servers.ppm", std::ios::binary);
    out << "P6\n640 480\n255\n";
    for (uint8_t pixel : pixels) {
        const auto& rgb = f.palette.entries[pixel];
        out.put(static_cast<char>(rgb.r))
            .put(static_cast<char>(rgb.g))
            .put(static_cast<char>(rgb.b));
    }
}

void check_login_animation(StatusWidget* widget) {
    std::vector<uint8_t> pixels(640 * 480);
    Surface surface{};
    surface.width = surface.pitch = 640;
    surface.height = 480;
    surface.pixels = pixels.data();
    surface.clip = {0, 0, 639, 479};
    check(widget->galaxy_graphics != nullptr, "installed loading-static graphics available");
    check(
        widget->galaxy_graphics != nullptr &&
            widget->galaxy_graphics->system_fonts == &widget->system.hat_fonts &&
            widget->galaxy_graphics->label_toolkit == widget->toolkit,
        "galaxy_build_planet planet labels take the system font table the Main Frame loaded"
    );
    boneyards_status_widget_draw(widget, &surface);
    std::vector<uint8_t> previous;
    for (int32_t frame = 0; frame < 4; ++frame) {
        boneyards_status_widget_tick(widget);
        boneyards_status_widget_draw(widget, &surface);
        const auto& green = widget->system.main_frame->console->green->pixels;
        bool varied = false;
        for (uint8_t pixel : green)
            if (pixel != green[0])
                varied = true;
        check(varied, "galaxy_draw static survives console_layer_pulse green remap during login");
        if (!previous.empty())
            check(
                green != previous, "galaxy_step_animation next static frame reaches login backdrop"
            );
        previous = green;
    }
}

void test_update(Fixture* f) {
    StatusWidget* widget = create(f);
    if (widget == nullptr)
        return;
    const MainFrame* frame = nullptr;
    for (int32_t i = 0; i < 180; ++i) {
        boneyards_status_widget_tick(widget);
        frame = widget->system.main_frame;
        if (frame != nullptr && frame->console_screen == console_screen::update &&
            frame->active_screen != nullptr)
            break;
    }
    check(
        frame != nullptr && frame->console_screen == console_screen::update &&
            frame->active_screen != nullptr,
        "the update console opens before the login console"
    );
    if (frame == nullptr || frame->active_screen == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    Window* view = frame->active_screen;
    Window* title = caption(widget, view, "BONEYARDS UPDATE");
    check(
        title != nullptr && title->x == view->x + 0x6f && title->y == view->y + 0x14 &&
            title->font == widget->system.hat_fonts.big,
        "update_view_build Big84 title at (0x6F, 0x14)"
    );
    Window* next = at(view, 0x6f, 0x10a);
    Window* cancel = at(view, 0xde, 0x10a);
    check(
        next == caption(widget, view, "CONTINUE") && cancel == caption(widget, view, "CANCEL"),
        "update_view_build CONTINUE at (0x6F, 0x10A) and CANCEL at (0xDE, 0x10A)"
    );
    check(
        next != nullptr && cancel != nullptr && next->visible == 0 && cancel->visible == 0 &&
            next->states.current == button_state::unpressed &&
            cancel->states.current == button_state::unpressed,
        "update_view_build both buttons value 2 and hidden"
    );
    auto* status = reinterpret_cast<screens::ByListBox*>(at(view, 0x6f, 0x32));
    check(
        status != nullptr && status->list.base.width == 0xe5 && status->list.base.height == 0xbb &&
            status->framed == 1 && status->forward_changes == 0,
        "update_view_build framed 0xE5 x 0xBB status list"
    );
    if (status != nullptr) {
        ScopedServices scope(widget);
        check(status->list.count == 1, "update_view_append_status opens the status with one row");
        auto* row = reinterpret_cast<screens::ByTextRow*>(list_box_item(&status->list, 0));
        check(
            row != nullptr && row->label != nullptr &&
                std::strcmp(
                    row->label->ops->state_text(row->label, -1), "Checking auto-updates..."
                ) == 0,
            "update_view_append_status Checking auto-updates..."
        );
    }
    Window* bar = at(view, 0x190, 0x32);
    check(
        bar != nullptr && std::strcmp(bar->name, "ProgressBar") == 0 && bar->width == 0x1a4 &&
            bar->height == 0xed,
        "update_view_build ProgressBar at (0x190, 0x32), 0x1A4 x 0xED"
    );
    if (bar != nullptr) {
        ScopedServices scope(widget);
        Surface track{};
        const bool present = image_surface(bar->states.images[1], &track);
        check(
            present && track.width == 0x14 && track.height == 0xbb, "update_view_build 20x187 track"
        );
        bool outlined = present;
        for (int32_t py = 0; present && py < track.height; ++py)
            for (int32_t px = 0; px < track.width; ++px) {
                const bool edge =
                    px == 0 || py == 0 || px == track.width - 1 || py == track.height - 1;
                if (track.pixels[py * track.pitch + px] != (edge ? 0x5a : 9))
                    outlined = false;
            }
        check(outlined, "update_view_build track outlined in 0x5A over key 9");
    }
    Window* progress = caption(widget, view, "Progress");
    check(
        progress != nullptr && progress->x == view->x + view->x + 0x16d &&
            progress->y == view->y + 0x19,
        "update_view_build Progress caption placed with the console x added twice"
    );
    boneyards_status_widget_destroy(widget);
}

void test_login(Fixture* f) {
    StatusWidget* widget = open(f);
    if (widget == nullptr)
        return;
    Window* view = widget->system.main_frame->active_screen;
    auto* list = reinterpret_cast<screens::ByListBox*>(at(view, 0x6f, 0x96));
    check(
        list != nullptr && list->list.count == 2, "login_server_entry skips empty server locations"
    );
    Window* second = list_box_item(&list->list, 1);
    check(
        second->height == 0x11 && second->y == view->y + 0x96 + 0x11,
        "server rows have the game's 17-pixel pitch"
    );
    check(list->selected_item == second, "login_view_build selects the current location");
    Window* name_label = caption(widget, view, "Username");
    Window* pass_label = caption(widget, view, "Password");
    Window* status_label = caption(widget, view, "Status");
    check(
        name_label && name_label->x + name_label->width - 1 == view->x + 0x6b,
        "Username right anchor"
    );
    check(
        pass_label && pass_label->x + pass_label->width - 1 == view->x + 0x6b,
        "Password right anchor"
    );
    check(
        status_label && status_label->y + status_label->height - 1 == view->y + 0x92,
        "Status bottom anchor"
    );
    check(
        caption(widget, view, "BONEYARDS LOGIN")->font == widget->system.hat_fonts.big,
        "Big84 login title"
    );
    Window* password = at(view, 0x6f, 0x44);
    Window* guest = caption(widget, view, "Guest Login");
    Window* member = caption(widget, view, "Registered User");
    // Registered User is chosen on open either way. Guest Login is grey while
    // guests are not offered, and unchosen but enabled when they are.
    const int32_t guest_on_open =
        screens::kGuestsOffered ? button_state::unpressed : button_state::disabled;
    check(
        member->states.current == button_state::pressed && password->states.current == 1 &&
            guest->states.current == guest_on_open,
        screens::kGuestsOffered
            ? "the login opens on Registered User, with the password shown and Guest Login unchosen"
            : "the login opens on Registered User, with the password shown and Guest Login grey"
    );
    {
        auto* member_radio = reinterpret_cast<screens::LobbyRadio*>(member);
        auto* guest_radio = reinterpret_cast<screens::LobbyRadio*>(guest);
        check(
            member_radio->member_count == 1 && member_radio->members[0] == &guest_radio->base &&
                guest_radio->member_count == 1 && guest_radio->members[0] == &member_radio->base,
            "login_view_build lists each login radio as the other's member"
        );
    }
    {
        std::vector<uint8_t> pixels(640 * 480, 0xff);
        Surface surface{};
        surface.width = surface.pitch = 640;
        surface.height = 480;
        surface.pixels = pixels.data();
        surface.clip = {0, 0, 639, 479};
        boneyards_status_widget_draw(widget, &surface);
        for (int32_t y : {0x2c, 0x44}) {
            Window* field = at(view, 0x6f, y);
            check(
                field->width == 0x63 && field->height == 0x15, "lobby_edit_create entry dimensions"
            );
            check(
                field->states.images[0] < 0 && field->states.images[1] < 0,
                "login_view_build builds the login entries without a frame of their own"
            );
            const int32_t x1 = field->x, y1 = field->y;
            const int32_t x2 = x1 + field->width - 1, y2 = y1 + field->height - 1;
            const auto pixel = [&](int32_t px, int32_t py) { return pixels[py * 640 + px]; };
            bool outlined = true;
            for (int32_t px = x1; px <= x2; ++px)
                outlined = outlined && pixel(px, y1) == 0x5a && pixel(px, y2) == 0x5a;
            for (int32_t py = y1; py <= y2; ++py)
                outlined = outlined && pixel(x1, py) == 0x5a && pixel(x2, py) == 0x5a;
            check(
                outlined, "login_view_draw outlines each login entry in 5a when the console draws"
            );
        }
    }
    check_login_animation(widget);
    check_fonts(widget);
    snapshot(widget, *f);
    check_button_and_wrap(widget, view);
    Window* login = at(view, 0x6f, 0xf2);
    {
        ScopedServices scope(widget);
        single_edit_set_text(reinterpret_cast<SingleEdit*>(at(view, 0x6f, 0x2c)), "test");
    }
    boneyards_status_widget_tick(widget);
    check(
        login->states.current == button_state::disabled && f->logins == 0,
        "a name without a password leaves LOGIN disabled"
    );
    click(widget, login);
    click_across_pulse(widget, login);
    check(f->logins == 0, "a click on a disabled LOGIN does nothing");
    {
        ScopedServices scope(widget);
        single_edit_set_text(reinterpret_cast<SingleEdit*>(password), "secret");
    }
    boneyards_status_widget_tick(widget);
    check(
        login->states.current == button_state::unpressed &&
            widget->navigation->login.login_value == 2,
        "a name and a password enable LOGIN"
    );
    pointer_button(widget, login, MessageCode::left_down, button_left);
    check(login->states.current == button_state::pressed, "LOGIN shows pressed while held");
    boneyards_status_widget_tick(widget);
    check(login->states.current == button_state::pressed, "a pulse leaves a held LOGIN pressed");
    pointer_button(widget, login, MessageCode::left_up, 0);
    check(
        f->logins == 1 && std::strcmp(widget->navigation->login.session_name, "test") == 0 &&
            std::strcmp(widget->navigation->login.session_password, "secret") == 0 &&
            std::strcmp(f->server, "CorePrime Alpha Test") == 0,
        "a click on an enabled LOGIN starts the login"
    );
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, password);
    }
    key_down(widget, key::enter);
    check(
        f->logins == 2 && std::strcmp(f->server, "CorePrime Alpha Test") == 0,
        "Enter in the password field starts the same login"
    );
    click(widget, at(view, 0x6f, 0x10a));
    check(
        f->cancels == 1 && !widget->navigation->login.closed &&
            widget->system.main_frame->active_screen == view,
        "failed login CANCEL resets without closing"
    );
    boneyards_status_widget_tick(widget);
    click(widget, login);
    check(f->logins == 3, "LOGIN retries after CANCEL");
    click(widget, list_box_item(&list->list, 0));
    check(
        list->selected_item == list_box_item(&list->list, 0),
        "text_row_notify row click selects the clicked item"
    );
    boneyards_status_widget_tick(widget);
    click(widget, login);
    check(std::strcmp(f->server, "USA, West Coast") == 0, "LOGIN reads the newly selected row");
    auto* status = reinterpret_cast<screens::ByListBox*>(at(view, 0xd5, 0x96));
    {
        ScopedServices scope(widget);
        list_box_clear(&status->list);
        screens::login_set_status(&widget->navigation->login, "Contact Established");
        screens::login_set_status(&widget->navigation->login, "Verifying Clearance");
    }
    check(
        status->list.count == 2,
        "login_set_status preserves status events delivered within one tick"
    );
    widget->navigation->login.state = screens::LoginState::connecting;
    SessionRecord session{};
    session.level = SessionLevel::logged_in;
    {
        ScopedServices scope(widget);
        f->events.session_changed(f->events.user, &session);
    }
    check(
        std::strcmp(f->saved_name, "test") == 0,
        "on_login_progress saves username at stage 8 before the dossier arrives"
    );
    boneyards_status_widget_destroy(widget);
    f->current = nullptr;
    f->saved = "commander";
    widget = open(f);
    view = widget->system.main_frame->active_screen;
    list = reinterpret_cast<screens::ByListBox*>(at(view, 0x6f, 0x96));
    check(
        list->selected_item == list_box_item(&list->list, 0), "no current location selects row zero"
    );
    check(
        caption(widget, view, "Registered User")->states.current == 1 &&
            widget->desktop.focus == at(view, 0x6f, 0x44),
        "saved user selects member and focuses password"
    );
    {
        ScopedServices scope(widget);
        auto* name = reinterpret_cast<SingleEdit*>(at(view, 0x6f, 0x2c));
        auto* secret = reinterpret_cast<SingleEdit*>(at(view, 0x6f, 0x44));
        check(
            std::strcmp(single_edit_text(name), "commander") == 0,
            "login_view_build captions the name field with the saved user"
        );
        single_edit_set_text(secret, "hunter2");
    }
    Window* saved_guest = caption(widget, view, "Guest Login");
    Window* saved_member = caption(widget, view, "Registered User");
    if (screens::kGuestsOffered) {
        click(widget, saved_guest);
        {
            ScopedServices scope(widget);
            auto* secret = reinterpret_cast<SingleEdit*>(at(view, 0x6f, 0x44));
            check(
                saved_guest->states.current == button_state::pressed,
                "a click on Guest Login selects it"
            );
            check(
                single_edit_text(secret)[0] == '\0' && secret->base.states.current == 0,
                "login_on_control guest radio blanks the password through its caption slot"
            );
            check(
                saved_member->states.current == button_state::unpressed,
                "lobby_radio_on_notify member radio lets go when its guest member is selected"
            );
        }
    } else {
        click(widget, saved_guest);
        click_across_pulse(widget, saved_guest);
        {
            ScopedServices scope(widget);
            desktop_set_focus(&widget->desktop, saved_guest);
        }
        key_down(widget, key::enter);
        key_down(widget, ' ');
        {
            ScopedServices scope(widget);
            auto* secret = reinterpret_cast<SingleEdit*>(at(view, 0x6f, 0x44));
            check(
                saved_guest->states.current == button_state::disabled &&
                    saved_member->states.current == button_state::pressed &&
                    std::strcmp(single_edit_text(secret), "hunter2") == 0 &&
                    widget->navigation->login.guest_value == 0 &&
                    widget->navigation->login.member_value == 1,
                "Guest Login is drawn disabled and ignores a click and a key"
            );
        }
    }
    boneyards_status_widget_destroy(widget);
    f->rows = 0;
    f->saved = nullptr;
    widget = open(f);
    view = widget->system.main_frame->active_screen;
    {
        ScopedServices scope(widget);
        single_edit_set_text(reinterpret_cast<SingleEdit*>(at(view, 0x6f, 0x2c)), "test");
        single_edit_set_text(reinterpret_cast<SingleEdit*>(at(view, 0x6f, 0x44)), "secret");
    }
    boneyards_status_widget_tick(widget);
    click(widget, at(view, 0x6f, 0xf2));
    check(f->server[0] == '\0', "no rows passes an empty location");
    boneyards_status_widget_destroy(widget);
}

const char* status_text(screens::ByListBox* list, int32_t index) {
    auto* row = reinterpret_cast<screens::ByTextRow*>(list_box_item(&list->list, index));
    if (row == nullptr || row->label == nullptr)
        return "";
    return row->label->ops->state_text(row->label, -1);
}

// Rows of the track image, above the bottom edge, that the rising fill covers.
int32_t bar_fill_rows(Window* bar) {
    Surface track{};
    if (bar == nullptr || !image_surface(bar->states.images[1], &track) || track.height < 3)
        return -1;
    int32_t rows = 0;
    for (int32_t y = track.height - 2; y >= 1; --y) {
        if (track.pixels[y * track.pitch + 1] == screens::kWidgetKey)
            break;
        ++rows;
    }
    return rows;
}

// Steps the shown update console one frame at a time: the status lines arrive
// in order, the bar grows on every frame, and the login console follows.
void test_update_pass(Fixture* f) {
    StatusWidget* widget = create(f);
    if (widget == nullptr)
        return;
    MainFrame* frame = nullptr;
    for (int32_t i = 0; i < 80; ++i) {
        boneyards_status_widget_tick(widget);
        frame = widget->system.main_frame;
        if (frame != nullptr && frame->console_screen == console_screen::update &&
            frame->active_screen != nullptr && frame->active_screen->visible != 0)
            break;
    }
    check(
        frame != nullptr && frame->active_screen != nullptr && frame->active_screen->visible != 0,
        "the update console is shown for its pass"
    );
    if (frame == nullptr || frame->active_screen == nullptr || frame->active_screen->visible == 0) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    Window* view = frame->active_screen;
    auto* status = reinterpret_cast<screens::ByListBox*>(at(view, 0x6f, 0x32));
    Window* bar = at(view, 0x190, 0x32);
    check(status != nullptr && bar != nullptr, "the pass has its status list and progress bar");
    if (status == nullptr || bar == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    int32_t samples = 0;
    int32_t previous = 0;
    int32_t first_fill = -1;
    int32_t rose = 0;
    int32_t previous_lines = 0;
    int32_t first_lines = -1;
    bool ordered = true;
    bool steady = true;
    int32_t final_fill = -1;
    int32_t final_lines = 0;
    while (frame->console_screen == console_screen::update &&
           samples < screens::kUpdatePassFrames + 5) {
        int32_t fill = 0;
        int32_t lines = 0;
        {
            ScopedServices scope(widget);
            fill = bar_fill_rows(bar);
            lines = status->list.count;
            if (lines < previous_lines)
                ordered = false;
            for (int32_t i = 0; i < lines; ++i) {
                if (i >= screens::kUpdatePassLineCount ||
                    std::strcmp(status_text(status, i), screens::kUpdatePassLines[i]) != 0)
                    ordered = false;
            }
        }
        if (first_fill < 0)
            first_fill = fill;
        else if (fill < previous)
            steady = false;
        else if (fill > previous)
            ++rose;
        if (first_lines < 0)
            first_lines = lines;
        previous = fill;
        previous_lines = lines;
        final_fill = fill;
        final_lines = lines;
        ++samples;
        boneyards_status_widget_tick(widget);
    }
    char message[96];
    std::snprintf(
        message, sizeof message, "the pass plays %d frames before the login console", samples
    );
    check(samples == screens::kUpdatePassFrames + 1, message);
    check(
        frame->console_screen == console_screen::login,
        "the login console opens at the end of the pass"
    );
    check(ordered, "status lines arrive in order");
    check(steady && first_fill == 0, "the bar starts empty and never falls");
    check(
        first_lines == 1 && final_lines == screens::kUpdatePassLineCount &&
            previous_lines > first_lines,
        "the list opens on Checking auto-updates... and fills through No updates needed"
    );
    check(
        rose == screens::kUpdatePassFrames && final_fill == 0xbb - 2,
        "the vertical bar rises on every frame of the pass to the top of its track"
    );
    boneyards_status_widget_destroy(widget);
}

// Name, Tab, password, Return logs a member in. With Password disabled, Tab
// from Username reaches LOGIN, and Return there logs a guest in. Return on
// the chosen radio does the same.
void test_login_keys(Fixture* f) {
    const int32_t before = f->logins;
    StatusWidget* widget = open(f);
    if (widget == nullptr)
        return;
    Window* view = widget->system.main_frame->active_screen;
    Window* name = at(view, 0x6f, 0x2c);
    Window* password = at(view, 0x6f, 0x44);
    Window* guest = caption(widget, view, "Guest Login");
    Window* member = caption(widget, view, "Registered User");
    Window* login = at(view, 0x6f, 0xf2);
    window_set_state(guest, button_state::unpressed);
    click(widget, guest);
    check(password->states.current == button_state::disabled, "Guest Login disables Password");
    {
        ScopedServices scope(widget);
        single_edit_set_text(reinterpret_cast<SingleEdit*>(name), "GuestOne");
        desktop_set_focus(&widget->desktop, name);
    }
    boneyards_status_widget_tick(widget);
    key_down(widget, key::tab);
    check(
        widget->desktop.focus == login, "Tab from Username reaches LOGIN when Password is skipped"
    );
    key_down(widget, key::enter);
    check(
        f->logins == before + 1 &&
            std::strcmp(widget->navigation->login.session_name, "GuestOne") == 0 &&
            widget->navigation->login.session_password[0] == '\0',
        "Return on LOGIN logs a guest in"
    );

    click(widget, member);
    check(password->states.current != button_state::disabled, "Registered User shows Password");
    {
        ScopedServices scope(widget);
        single_edit_set_text(reinterpret_cast<SingleEdit*>(name), "MemberOne");
        single_edit_set_text(reinterpret_cast<SingleEdit*>(password), "");
        desktop_set_focus(&widget->desktop, name);
    }
    boneyards_status_widget_tick(widget);
    key_down(widget, key::tab);
    check(widget->desktop.focus == password, "Tab from Username reaches Password");
    {
        ScopedServices scope(widget);
        single_edit_set_text(reinterpret_cast<SingleEdit*>(password), "secret");
    }
    key_down(widget, key::enter);
    check(
        f->logins == before + 2 &&
            std::strcmp(widget->navigation->login.session_name, "MemberOne") == 0 &&
            std::strcmp(widget->navigation->login.session_password, "secret") == 0,
        "Name, Tab, password, Return logs a member in"
    );
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, member);
    }
    boneyards_status_widget_tick(widget);
    key_down(widget, key::enter);
    check(
        f->logins == before + 3 &&
            std::strcmp(widget->navigation->login.session_password, "secret") == 0,
        "Return on the chosen login radio logs in"
    );
    boneyards_status_widget_destroy(widget);
}
} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the login console tests");
    Fixture fixture;
    fixture.assets.mount(root / "by.ccx");
    fixture.assets.mount(root / "totala1.hpi");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int32_t i = 0; i < 256; ++i)
        fixture.palette.entries[i] = {raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    test_update(&fixture);
    test_update_pass(&fixture);
    test_login(&fixture);
    test_login_keys(&fixture);
    return failures == 0 ? 0 : 1;
}
