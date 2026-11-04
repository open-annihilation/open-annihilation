// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Direction keys move the focus through the controls. Enter presses the
// focused one, and focusing a text field reports where the on-screen
// keyboard should stand.
#include <cstdio>
#include <cstring>

#include "oa/boneyards/toolkit.hpp"

using namespace oa::boneyards;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct NotifyLog {
    int32_t count;
    Window* source;
};

NotifyLog g_log;

void log_notify(Window*, const Notify* notify) {
    if (notify->code != NotifyCode::activated)
        return;
    ++g_log.count;
    g_log.source = notify->source;
}

uint8_t g_keys[0x200];

bool key_is_down(void*, int32_t key) {
    return key >= 0 && key < 0x200 && g_keys[key] != 0;
}

struct KeyboardLog {
    int32_t opens;
    int32_t closes;
    TextFieldPlace place;
};

void on_focus(Desktop*, Window* focused, void* user) {
    auto* log = static_cast<KeyboardLog*>(user);
    TextFieldPlace place{};
    if (focused != nullptr && window_text_field_place(focused, &place) && place.width > 0 &&
        place.height > 0) {
        ++log->opens;
        log->place = place;
    } else {
        ++log->closes;
    }
}

bool press(Desktop* desktop, int32_t key) {
    Message message{};
    message.code = MessageCode::key_down;
    message.key = key;
    const bool consumed = desktop_route(desktop, &message);
    message.code = MessageCode::key_up;
    desktop_route(desktop, &message);
    return consumed;
}

Window* button_at(Window* parent, const char* name, int32_t x, int32_t y) {
    Window* button = button_create();
    window_create(button, name, x, y, 40, 16, parent, true);
    return button;
}

Window* find_at(Window* window, int32_t x, int32_t y) {
    if (window == nullptr)
        return nullptr;
    if (window->x == x && window->y == y)
        return window;
    for (int32_t i = 0; i < window->children.count; ++i) {
        if (Window* found = find_at(window->children.items[i], x, y))
            return found;
    }
    return nullptr;
}

void test_grid(Desktop* desktop, Window* host) {
    // Tab walks a panel's children. A bare gadget would take the focus itself.
    Window* grid = panel_create();
    window_create(grid, "grid", 0, 0, 640, 200, host, true);
    Window* top_left = button_at(grid, "a", 10, 10);
    Window* top_right = button_at(grid, "b", 80, 10);
    Window* low_left = button_at(grid, "c", 10, 60);
    Window* low_right = button_at(grid, "d", 80, 60);
    Window* disabled = button_at(grid, "off", 10, 30);
    window_set_state(disabled, button_state::disabled);
    disabled->focusable = 0;

    desktop_set_focus(desktop, nullptr);
    check(
        press(desktop, key::enter) && desktop->focus == top_left, "Enter focuses the first control"
    );
    check(g_log.count == 0, "the first Enter does not press it");
    check(
        press(desktop, key::enter) && g_log.count == 1 && g_log.source == top_left,
        "Enter presses the focused control"
    );

    check(press(desktop, key::right) && desktop->focus == top_right, "right moves across the row");
    check(press(desktop, key::down) && desktop->focus == low_right, "down stays in the column");
    check(
        press(desktop, key::left) && desktop->focus == low_left, "left moves back across the row"
    );
    check(press(desktop, key::up) && desktop->focus == top_left, "up stays in the column");
    check(
        press(desktop, key::down) && desktop->focus == low_left, "down skips the disabled control"
    );
    check(
        press(desktop, key::down) && desktop->focus == top_right,
        "down continues at the top of the next column"
    );
    check(
        press(desktop, key::down) && desktop->focus == low_right,
        "down moves to the bottom of that column"
    );
    check(
        press(desktop, key::down) && desktop->focus == top_left, "down wraps to the first control"
    );

    g_keys[key::shift] = 1;
    check(press(desktop, key::tab) && desktop->focus == low_right, "Shift+Tab moves backward");
    g_keys[key::shift] = 0;
    check(press(desktop, key::tab) && desktop->focus == top_left, "Tab moves forward");

    desktop_set_focus(desktop, top_left);
    check(
        press(desktop, ' ') && g_log.count == 2 && g_log.source == top_left,
        "Space presses the control"
    );
}

void test_text_and_dialog(Desktop* desktop, Window* host) {
    KeyboardLog keyboard{};
    desktop->on_focus = on_focus;
    desktop->focus_user = &keyboard;

    SingleEdit* edit = single_edit_create();
    window_create(&edit->base, "name", 10, 120, 80, 16, host, true);
    single_edit_set_text(edit, "ab");
    const int32_t typed = edit->length;

    desktop_set_focus(desktop, nullptr);
    check(press(desktop, key::down), "a direction focuses a control");
    // The field is below the buttons. Down the left column wraps onto it
    // once the buttons have been passed; aim at it from the button above.
    Window* above = find_at(host, 10, 60);
    desktop_set_focus(desktop, above);
    const int32_t opens = keyboard.opens;
    check(
        press(desktop, key::down) && desktop->focus == &edit->base,
        "down leaves the button for the field"
    );
    check(keyboard.opens == opens + 1, "focusing the field opens the keyboard");
    check(
        keyboard.place.x == edit->base.x && keyboard.place.y == edit->base.y &&
            keyboard.place.width == edit->base.width &&
            keyboard.place.height == edit->base.height && keyboard.place.width > 0 &&
            keyboard.place.height > 0,
        "the keyboard is given the field's place"
    );

    const int32_t caret = edit->caret;
    g_keys[key::control] = 1;
    check(
        press(desktop, key::left) && desktop->focus == &edit->base && edit->caret == 0,
        "Ctrl+Left stays in the field and moves by word"
    );
    g_keys[key::control] = 0;
    edit->caret = caret;
    check(
        press(desktop, key::left) && desktop->focus == &edit->base && edit->caret == caret - 1,
        "Left stays in the field and moves the caret"
    );
    check(press(desktop, key::right) && desktop->focus == &edit->base, "Right stays in the field");
    check(press(desktop, key::up) && desktop->focus == above, "up leaves the field");
    check(keyboard.closes > 0, "leaving the field closes the keyboard");

    Message alt{};
    alt.code = MessageCode::key_down;
    alt.key = key::alt;
    desktop_set_focus(desktop, &edit->base);
    desktop_route(desktop, &alt);
    alt.key = key::system;
    desktop_route(desktop, &alt);
    check(edit->length == typed, "Alt and the system key are not typed");

    Window* dialog = dialog_create();
    window_create(dialog, "ask", 200, 40, 160, 80, nullptr, true);
    Window* yes = button_at(dialog, "yes", 220, 56);
    Window* no = button_at(dialog, "no", 220, 84);
    desktop_set_focus(desktop, nullptr);
    check(
        press(desktop, key::down) && (desktop->focus == yes || desktop->focus == no),
        "a dialog takes the first direction"
    );
    Window* first = desktop->focus;
    Window* other = first == yes ? no : yes;
    check(press(desktop, key::down) && desktop->focus == other, "down stays inside the dialog");
    check(press(desktop, key::down) && desktop->focus == first, "down wraps inside the dialog");
    check(desktop->focus != above && desktop->focus != &edit->base, "the dialog keeps the focus");
    window_destroy(dialog);
}

} // namespace

int main() {
    WindowOps host_ops = k_gadget_ops;
    host_ops.on_notify = log_notify;

    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Services services{};
    services.desktop = &desktop;
    InputService input{nullptr, key_is_down};
    services.input = &input;
    services_install(services);

    Window* host = gadget_create();
    host->ops = &host_ops;
    window_create(host, "host", 0, 0, 640, 480, nullptr, true);

    test_grid(&desktop, host);
    test_text_and_dialog(&desktop, host);

    window_destroy(host);
    desktop_release(&desktop);
    services_install(Services{});

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("gamepad focus passed");
    return 0;
}
