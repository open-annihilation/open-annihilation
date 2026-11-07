// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A gamepad drives the main menu and every console: the focus order, pressing
// the focused control, and a text field opening the on-screen keyboard.
#include "console_state.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/console/registration.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

struct Fixture {
    oa::AssetStore assets;
    oa::Palette palette{};
    int32_t logins = 0;

    explicit Fixture(std::filesystem::path root) : assets(std::move(root)) {}
};

struct KeyboardLog {
    int32_t opens = 0;
    int32_t closes = 0;
    TextFieldPlace places[48]{};
    int32_t count = 0;
};

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

bool read_asset(void* context, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Fixture*>(context)->assets.read(path).bytes;
        return true;
    } catch (...) {
        return false;
    }
}

ServiceResult login(void* context, const LoginRequest*) {
    ++static_cast<Fixture*>(context)->logins;
    return ServiceResult::failed;
}

void on_keyboard(void* user, const TextFieldPlace* place) {
    auto* log = static_cast<KeyboardLog*>(user);
    if (place != nullptr && place->width > 0 && place->height > 0) {
        ++log->opens;
        if (log->count < 48)
            log->places[log->count++] = *place;
    } else {
        ++log->closes;
    }
}

bool is_stop_class(const Window* window, const char* name) {
    return window != nullptr && window->ops != nullptr && window->ops->is_class != nullptr &&
           window->ops->is_class(name);
}

bool is_text_field(const Window* window) {
    return is_stop_class(window, class_name::single_edit) ||
           is_stop_class(window, class_name::edit_box);
}

bool is_button(const Window* window) {
    return is_stop_class(window, class_name::button) &&
           window->states.current != button_state::disabled && window->focusable != 0 &&
           window_is_shown(window);
}

bool under(const Window* root, const Window* window) {
    for (const Window* at = window; at != nullptr; at = at->parent) {
        if (at == root)
            return true;
    }
    return false;
}

Window* find_caption(Window* window, const char* text) {
    if (window == nullptr)
        return nullptr;
    if (window->ops != nullptr && window->ops->state_text != nullptr) {
        const char* caption = window->ops->state_text(window, -1);
        if (caption != nullptr && std::strcmp(caption, text) == 0)
            return window;
    }
    for (int32_t i = 0; i < window->children.count; ++i) {
        if (Window* found = find_caption(window->children.items[i], text))
            return found;
    }
    return nullptr;
}

// The MATCHMAKING caption is a label. The button that opens the dialog sits
// on that same row.
Window* button_on_row(Window* label) {
    if (label == nullptr || label->parent == nullptr)
        return nullptr;
    Window* parent = label->parent;
    for (int32_t i = 0; i < parent->children.count; ++i) {
        Window* child = parent->children.items[i];
        if (child != label && is_button(child) && child->y == label->y)
            return child;
    }
    return nullptr;
}

Window* find_button(Window* window) {
    if (window == nullptr || !window_is_shown(window))
        return nullptr;
    if (is_button(window))
        return window;
    for (int32_t i = 0; i < window->children.count; ++i) {
        if (Window* found = find_button(window->children.items[i]))
            return found;
    }
    return nullptr;
}

bool place_matches(const KeyboardLog& log, const Window* field) {
    for (int32_t i = 0; i < log.count; ++i) {
        const TextFieldPlace& place = log.places[i];
        if (place.x == field->x && place.y == field->y && place.width == field->width &&
            place.height == field->height && place.width > 0 && place.height > 0)
            return true;
    }
    return false;
}

void dismiss_box(StatusWidget* widget) {
    MessageBox* box = message_box_find_open(&widget->desktop);
    if (box == nullptr)
        return;
    Window* answer = window_find_named(&box->base, message_box_name::ok, nullptr);
    if (answer == nullptr)
        answer = window_find_named(&box->base, message_box_name::no, nullptr);
    if (answer == nullptr)
        return;
    desktop_set_focus(&widget->desktop, answer);
    boneyards_status_widget_pad(widget, PadPress::accept);
}

constexpr int32_t k_walk_cap = 240;

struct Walk {
    Window* order[k_walk_cap]{};
    int32_t count = 0;
};

bool already_walked(const Walk& walk, const Window* window) {
    for (int32_t i = 0; i < walk.count; ++i) {
        if (walk.order[i] == window)
            return true;
    }
    return false;
}

// Tries each direction of the pad. A move onto a control already walked is
// undone, so a column does not hide the controls beside it.
Window* step_pad(StatusWidget* widget, const Walk& walk) {
    Window* before = widget->desktop.focus;
    const PadPress presses[] = {PadPress::down, PadPress::right, PadPress::up, PadPress::left};
    for (PadPress press : presses) {
        boneyards_status_widget_pad(widget, press);
        Window* now = widget->desktop.focus;
        if (now == nullptr || now == before)
            continue;
        if (!already_walked(walk, now))
            return now;
        desktop_set_focus(&widget->desktop, before);
    }
    return before;
}

void walk_focus(StatusWidget* widget, Walk* walk) {
    desktop_set_focus(&widget->desktop, nullptr);
    boneyards_status_widget_pad(widget, PadPress::down);
    walk->count = 0;
    Window* first = widget->desktop.focus;
    if (first == nullptr)
        return;
    walk->order[walk->count++] = first;
    for (int32_t i = 0; i < k_walk_cap - 1; ++i) {
        Window* next = step_pad(widget, *walk);
        if (next == nullptr || next == walk->order[walk->count - 1])
            break;
        walk->order[walk->count++] = next;
    }
}

bool same_walk(StatusWidget* widget, const Walk& first) {
    Walk again{};
    walk_focus(widget, &again);
    if (again.count != first.count)
        return false;
    for (int32_t i = 0; i < first.count; ++i) {
        if (again.order[i] != first.order[i])
            return false;
    }
    return true;
}

void collect_fields(Window* window, Window** out, int32_t* count) {
    if (window == nullptr || !window_is_shown(window) || *count >= 32)
        return;
    if (is_text_field(window))
        out[(*count)++] = window;
    for (int32_t i = 0; i < window->children.count; ++i)
        collect_fields(window->children.items[i], out, count);
}

void present(StatusWidget* widget) {
    for (int32_t i = 0; i < 40; ++i) {
        boneyards_status_widget_tick(widget);
        Window* screen = widget->system.main_frame->active_screen;
        if (screen != nullptr && window_is_shown(screen))
            return;
    }
}

void check_screen(StatusWidget* widget, const char* name, KeyboardLog* keyboard) {
    MainFrame* frame = widget->system.main_frame;
    Window* screen = frame->active_screen;
    char label[96];
    std::snprintf(label, sizeof label, "%s is on screen", name);
    check(screen != nullptr && window_is_shown(screen), label);
    if (screen == nullptr || !window_is_shown(screen))
        return;
    dismiss_box(widget);
    Walk walk{};
    walk_focus(widget, &walk);
    std::snprintf(label, sizeof label, "%s focus order has two controls", name);
    check(walk.count >= 2, label);
    std::snprintf(label, sizeof label, "%s focus order is stable", name);
    check(same_walk(widget, walk), label);
    bool on_screen = false;
    for (int32_t i = 0; i < walk.count; ++i)
        on_screen = on_screen || under(screen, walk.order[i]);
    std::snprintf(label, sizeof label, "%s is in the focus order", name);
    check(on_screen, label);
    Window* fields[32]{};
    int32_t field_count = 0;
    collect_fields(screen, fields, &field_count);
    const int32_t opens = keyboard->opens;
    for (int32_t i = 0; i < field_count; ++i) {
        desktop_set_focus(&widget->desktop, nullptr);
        desktop_set_focus(&widget->desktop, fields[i]);
        std::snprintf(label, sizeof label, "%s text field opens the keyboard", name);
        check(place_matches(*keyboard, fields[i]), label);
    }
    if (field_count > 0) {
        std::snprintf(label, sizeof label, "%s opened the keyboard", name);
        check(keyboard->opens > opens, label);
    }
    Window* button = find_button(screen);
    if (button != nullptr) {
        desktop_set_focus(&widget->desktop, button);
        std::snprintf(label, sizeof label, "%s accepts the focused control", name);
        check(boneyards_status_widget_pad(widget, PadPress::accept), label);
        dismiss_box(widget);
    }
}

StatusWidget* create_widget(Fixture* fixture) {
    StatusWidgetConfig config{};
    config.files = {fixture, read_asset};
    config.palette = &fixture->palette;
    config.service.user = fixture;
    config.service.network_available = [](void*) { return true; };
    config.service.activate = [](void*) { return ActivationResult::ok; };
    config.service.login = login;
    config.service.set_events = [](void*, const ServiceEvents*) {};
    config.service.list_servers = [](void*, void (*entry)(void*, const char*), void* user) {
        entry(user, "CorePrime Alpha Test");
        entry(user, "");
    };
    config.service.current_server = [](void*) { return "CorePrime Alpha Test"; };
    return boneyards_status_widget_create(config);
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path root = oa::boneyards_test::require_by_install("gamepad consoles");
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
    oa::HpiArchive archive = std::move(*opened.value);
    rover_session_set_virtual_files(read_archive, &archive);
    Fixture fixture(root);
    fixture.assets.mount(root / "by.ccx");
    fixture.assets.mount(root / "totala1.hpi");
    fixture.assets.mount(coreprime);
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    if (raw.size() >= 256u * 4u) {
        for (int32_t i = 0; i < 256; ++i) {
            fixture.palette.entries[i] = {
                raw[static_cast<std::size_t>(i) * 4],
                raw[static_cast<std::size_t>(i) * 4 + 1],
                raw[static_cast<std::size_t>(i) * 4 + 2],
                0
            };
        }
    }

    KeyboardLog keyboard{};
    StatusWidget* widget = create_widget(&fixture);
    check(widget != nullptr, "widget created");
    if (widget == nullptr)
        return 1;
    boneyards_status_widget_set_screen_keyboard(widget, on_keyboard, &keyboard);
    ScopedServices scope(widget);
    widget->system.matchmaking_enabled = account_sees_unfinished(kAccountStatusBetaTester) ? 1 : 0;

    Window* enter = boneyards_status_widget_part(widget, OverlayPart::enter);
    Window* credits = boneyards_status_widget_part(widget, OverlayPart::credits);
    desktop_set_focus(&widget->desktop, nullptr);
    check(
        boneyards_status_widget_pad(widget, PadPress::down) && widget->desktop.focus == enter,
        "down focuses ENTER BONEYARDS"
    );
    check(
        boneyards_status_widget_pad(widget, PadPress::down) && widget->desktop.focus == credits,
        "down moves on to credits"
    );
    check(
        boneyards_status_widget_pad(widget, PadPress::down) && widget->desktop.focus == enter,
        "down wraps back to ENTER BONEYARDS"
    );
    check(boneyards_status_widget_pad(widget, PadPress::accept), "accept presses ENTER BONEYARDS");
    for (int32_t i = 0; i < 180 && widget->system.main_frame != nullptr &&
                        widget->system.main_frame->console_screen != console_screen::login;
         ++i)
        boneyards_status_widget_tick(widget);
    check(
        widget->system.main_frame != nullptr &&
            widget->system.main_frame->console_screen == console_screen::login,
        "accept enters and the login console opens"
    );
    if (widget->system.main_frame == nullptr)
        return 1;
    widget->system.matchmaking_enabled = 1;

    Window* login_screen = widget->system.main_frame->active_screen;
    check(login_screen != nullptr && window_is_shown(login_screen), "login is on screen");
    check(keyboard.opens > 0, "the login field opens the keyboard");
    if (login_screen != nullptr && is_text_field(widget->desktop.focus))
        check(place_matches(keyboard, widget->desktop.focus), "the keyboard uses the login field");
    Window* name = widget->desktop.focus;
    if (is_text_field(name)) {
        check(
            boneyards_status_widget_pad(widget, PadPress::left) && widget->desktop.focus == name,
            "left stays in the login field"
        );
        check(
            boneyards_status_widget_pad(widget, PadPress::down) && widget->desktop.focus != name,
            "down leaves the login field"
        );
    }
    if (is_text_field(name) && is_stop_class(name, class_name::single_edit)) {
        ScopedServices field_scope(widget);
        single_edit_set_text(reinterpret_cast<SingleEdit*>(name), "Pilot");
    }
    // Registered User is the default, so LOGIN waits for a password too.
    Window* password = nullptr;
    for (int32_t i = 0; login_screen != nullptr && i < login_screen->children.count; ++i) {
        Window* child = login_screen->children.items[i];
        if (child->x == login_screen->x + 0x6f && child->y == login_screen->y + 0x44)
            password = child;
    }
    check(
        password != nullptr && is_stop_class(password, class_name::single_edit),
        "the password field is shown"
    );
    if (password != nullptr && is_stop_class(password, class_name::single_edit)) {
        ScopedServices password_scope(widget);
        single_edit_set_text(reinterpret_cast<SingleEdit*>(password), "secret");
    }
    boneyards_status_widget_tick(widget);
    Window* login_button = login_screen != nullptr ? find_caption(login_screen, "LOGIN") : nullptr;
    if (login_button != nullptr) {
        desktop_set_focus(&widget->desktop, login_button);
        const int32_t before = fixture.logins;
        check(boneyards_status_widget_pad(widget, PadPress::accept), "accept presses LOGIN");
        check(fixture.logins == before + 1, "LOGIN is activated");
    } else {
        check(false, "LOGIN is on the login console");
    }
    dismiss_box(widget);

    {
        ScopedServices modifier_scope(widget);
        boneyards_status_widget_modifiers(widget, false, true, true, true);
        check(
            input_key_down(key::control) && input_key_down(key::alt) && input_key_down(key::system),
            "Ctrl, Alt and the system key reach the toolkit"
        );
        check(!input_key_down(key::shift), "Shift is not held");
        boneyards_status_widget_modifiers(widget, false, false, false, false);
        if (name != nullptr && login_screen != nullptr) {
            Window* next_field = nullptr;
            for (int32_t i = 0; i < login_screen->children.count; ++i) {
                Window* child = login_screen->children.items[i];
                if (child != name && is_text_field(child))
                    next_field = child;
            }
            if (next_field != nullptr) {
                desktop_set_focus(&widget->desktop, next_field);
                boneyards_status_widget_event_modifiers(widget, true, false, false, false);
                Message tab{};
                tab.code = MessageCode::key_down;
                tab.key = key::tab;
                boneyards_status_widget_event(widget, tab);
                check(
                    widget->desktop.focus != next_field,
                    "Shift+Tab from the modifier bit moves backward"
                );
                check(!input_key_down(key::shift), "the Shift bit does not stay held");
            }
        }
    }

    check_screen(widget, "login", &keyboard);

    Window* register_button =
        login_screen != nullptr ? find_caption(login_screen, "REGISTER") : nullptr;
    if (register_button != nullptr) {
        desktop_set_focus(&widget->desktop, register_button);
        boneyards_status_widget_pad(widget, PadPress::accept);
        present(widget);
        Window* back = window_find_named(&widget->system.main_frame->base, "Back", nullptr);
        check(back != nullptr && window_is_shown(back), "REGISTER opens the registration console");
        if (back != nullptr)
            check_screen(widget, "registration", &keyboard);
        dismiss_box(widget);
    }

    const int32_t screens[] = {
        console_screen::update,
        console_screen::recon,
        console_screen::chat,
        console_screen::battle,
        console_screen::news,
        console_screen::ladders,
        console_screen::profile,
        console_screen::help,
        console_screen::quickplay,
        console_screen::locator,
    };
    const char* names[] = {
        "update",
        "recon",
        "chat",
        "battle",
        "news",
        "ladders",
        "profile",
        "help",
        "quickplay",
        "locator",
    };
    for (int32_t i = 0; i < 10; ++i) {
        widget->system.matchmaking_enabled = 1;
        main_frame_show_screen(widget->system.main_frame, screens[i]);
        present(widget);
        check_screen(widget, names[i], &keyboard);
    }

    widget->system.matchmaking_enabled = 1;
    main_frame_show_screen(widget->system.main_frame, console_screen::chat);
    present(widget);
    Window* chat = widget->system.main_frame->active_screen;
    Window* match_label = chat != nullptr ? find_caption(chat, "MATCHMAKING") : nullptr;
    Window* match = button_on_row(match_label);
    if (match != nullptr) {
        desktop_set_focus(&widget->desktop, match);
        boneyards_status_widget_pad(widget, PadPress::accept);
        Window* dialog = matchmaking_dialog_find(widget);
        check(dialog != nullptr && window_is_shown(dialog), "accept opens matchmaking");
        if (dialog != nullptr) {
            Walk walk{};
            walk_focus(widget, &walk);
            bool inside = false;
            for (int32_t i = 0; i < walk.count; ++i)
                inside = inside || under(dialog, walk.order[i]);
            check(walk.count >= 2 && inside, "matchmaking keeps a focus order");
            check(same_walk(widget, walk), "matchmaking focus order is stable");
            Window* button = find_button(dialog);
            if (button != nullptr) {
                desktop_set_focus(&widget->desktop, button);
                check(boneyards_status_widget_pad(widget, PadPress::accept), "matchmaking accepts");
            }
        }
    } else {
        check(false, "MATCHMAKING is on the chat console");
    }

    boneyards_status_widget_destroy(widget);
    rover_session_set_virtual_files(nullptr, nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("gamepad consoles passed");
    return 0;
}
