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

// Enough stored captions for one test. Handles start at 1.
char g_captions[8][32];
int32_t g_caption_count = 0;

Handle caption_create(void*, const char* text, int32_t) {
    if (g_caption_count >= 8)
        return k_no_handle;
    const Handle handle = g_caption_count + 1;
    std::snprintf(
        g_captions[g_caption_count], sizeof g_captions[0], "%s", text != nullptr ? text : ""
    );
    ++g_caption_count;
    return handle;
}

const char* caption_get(void*, Handle text) {
    if (text < 1 || text > g_caption_count)
        return "";
    return g_captions[text - 1];
}

void caption_release(void*, Handle) {
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
    // The log lives on this function's stack; later tests move the focus too.
    desktop->on_focus = nullptr;
    desktop->focus_user = nullptr;
}

void click_at(Desktop* desktop, const Window* window) {
    Message message{};
    message.code = MessageCode::left_down;
    message.x = window->x + 1;
    message.y = window->y + 1;
    message.buttons = button_left;
    desktop_route(desktop, &message);
    message.code = MessageCode::left_up;
    message.buttons = 0;
    desktop_route(desktop, &message);
}

// F6 steps the regions that are showing. Tab stays in the one that has the
// keyboard, and in a dialog. A disabled control, a control with no name and
// a control with no area are not stops.
void test_regions(Desktop* desktop) {
    Window* frame = panel_create();
    window_create(frame, "frame", 0, 0, 220, 160, nullptr, true);
    frame->region = 1;
    Window* frame_button = button_at(frame, "frame-button", 8, 8);

    Window* console = panel_create();
    window_create(console, "console", 16, 16, 180, 120, frame, true);
    console->region = 2;
    Window* first = button_at(console, "one", 24, 24);
    Window* blank = button_at(console, "blank", 24, 48);
    window_set_name(blank, "");
    Window* flat = button_at(console, "flat", 24, 72);
    window_set_width(flat, 0);
    Window* second = button_at(console, "two", 80, 24);
    Window* disabled = button_at(console, "disabled", 80, 48);
    window_set_state(disabled, button_state::disabled);

    Window* map = panel_create();
    window_create(map, "map", 0, 200, 80, 40, nullptr, true);
    map->region = 3;

    desktop_set_focus(desktop, frame_button);
    check(press(desktop, key::tab) && desktop->focus == frame_button, "Tab stays in the frame");
    check(press(desktop, key::f6) && desktop->focus == first, "F6 moves to the console");
    check(
        press(desktop, key::tab) && desktop->focus == second, "Tab skips a blank and a flat stop"
    );
    check(
        press(desktop, key::tab) && desktop->focus == first, "Tab wraps without repeating a stop"
    );
    click_at(desktop, disabled);
    check(desktop->focus == first, "a click on a disabled control leaves the keyboard");
    g_keys[key::shift] = 1;
    check(press(desktop, key::f6) && desktop->focus == frame_button, "Shift+F6 moves back");
    g_keys[key::shift] = 0;
    check(press(desktop, key::f6) && desktop->focus == first, "F6 moves to the console again");
    check(press(desktop, key::f6) && desktop->focus == map, "F6 moves to the map");
    check(press(desktop, key::f6) && desktop->focus == frame_button, "F6 wraps to the frame");

    Window* dialog = dialog_create();
    window_create(dialog, "ask", 300, 8, 80, 64, nullptr, true);
    Window* yes = button_at(dialog, "yes", 308, 16);
    Window* no = button_at(dialog, "no", 308, 40);
    desktop_set_focus(desktop, yes);
    check(press(desktop, key::down) && desktop->focus == no, "down stays inside the dialog");
    check(press(desktop, key::tab) && desktop->focus == yes, "Tab stays inside the dialog");
    check(press(desktop, key::f6) && desktop->focus == yes, "F6 does not leave the dialog");
    window_destroy(dialog);

    Window* prompt = panel_create();
    window_create(prompt, "prompt", 300, 100, 120, 40, nullptr, true);
    prompt->modal = 1;
    Window* choose = button_at(prompt, "choose", 308, 108);
    Window* other = button_at(prompt, "other", 360, 108);
    desktop_set_focus(desktop, choose);
    check(press(desktop, key::right) && desktop->focus == other, "right stays in a modal panel");
    check(press(desktop, key::tab) && desktop->focus == choose, "Tab stays in a modal panel");
    check(press(desktop, key::f6) && desktop->focus == choose, "F6 does not leave a modal panel");
    window_destroy(prompt);

    window_destroy(frame);
    window_destroy(map);
}

// Opening a dialog leaves the focus where it was. The first Tab or arrow
// lands on the default answer. An unconfined desktop, with the keyboard on
// nothing, does not take Tab. A button with a caption and no name is a stop,
// and a click focuses it. F6 leaves a console that was stamped after the
// keyboard moved.
void test_open_and_names(Desktop* desktop) {
    Window* credits = button_at(nullptr, "CreditsBtn", 8, 400);
    desktop_set_focus(desktop, nullptr);
    check(
        !press(desktop, key::tab) && desktop->focus == nullptr,
        "an unconfined desktop does not consume Tab"
    );
    window_destroy(credits);

    Window* opener = button_at(nullptr, "Minimize_Button", 8, 420);
    desktop_set_focus(desktop, opener);
    Window* dialog = dialog_create();
    window_create(dialog, "Matchmaking", 200, 200, 120, 80, nullptr, true);
    dialog->modal = 1;
    Window* activate = button_at(dialog, "Activate", 210, 248);
    button_at(dialog, "Back", 210, 220);
    check(desktop->focus == opener, "opening a dialog leaves the focus");
    check(
        press(desktop, key::tab) && desktop->focus == activate, "the first Tab lands on Activate"
    );
    desktop_set_focus(desktop, opener);
    check(
        press(desktop, key::down) && desktop->focus == activate, "the first arrow lands on Activate"
    );
    window_destroy(dialog);

    Window* box = dialog_create();
    window_create(box, "ask", 200, 300, 80, 40, nullptr, true);
    Window* ok = button_at(box, "Ok", 208, 308);
    desktop_set_focus(desktop, box);
    check(
        press(desktop, key::tab) && desktop->focus == ok, "the first Tab from the box lands on Ok"
    );
    window_destroy(box);

    Window* pane = panel_create();
    window_create(pane, "pane", 400, 180, 100, 60, nullptr, true);
    pane->region = 1;
    Window* named = button_at(pane, "named", 408, 188);
    Window* search = button_create();
    window_create(search, nullptr, 408, 210, 48, 16, pane, true);
    search->ops->set_caption(search, "SEARCH", 0);
    desktop_set_focus(desktop, named);
    check(
        press(desktop, key::tab) && desktop->focus == search, "a caption is enough to make a stop"
    );
    click_at(desktop, named);
    click_at(desktop, search);
    check(desktop->focus == search, "a click gives the keyboard to a captioned button");
    window_destroy(pane);

    Window* frame = panel_create();
    window_create(frame, "frame-late", 0, 0, 80, 30, nullptr, true);
    frame->region = 1;
    Window* frame_button = button_at(frame, "frame-late-button", 4, 4);
    Window* console = panel_create();
    window_create(console, "console-late", 0, 40, 80, 30, nullptr, true);
    Window* entry = button_at(console, "entry", 4, 44);
    button_at(console, "ChatList", 40, 44);
    Window* map = panel_create();
    window_create(map, "map-late", 0, 80, 40, 20, nullptr, true);
    map->region = 3;
    map->focusable = 1;
    desktop_set_focus(desktop, frame_button);
    console->region = 2;
    desktop->focus = entry;
    desktop->keyboard_region = frame;
    check(press(desktop, key::f6) && desktop->focus == map, "F6 leaves a just-stamped console");
    window_destroy(frame);
    window_destroy(console);
    window_destroy(map);
}

// A box that hands the keyboard back into the frame leaves the frame
// remembering the box's answer, and the box is then destroyed. A key that
// finds the focus hidden must not read that answer. Nothing may land on a
// window that is no longer on the desktop.
void test_stale_opener(Desktop* desktop) {
    Window* frame = panel_create();
    window_create(frame, "stale-frame", 0, 300, 200, 100, nullptr, true);
    frame->region = 1;
    Window* search = button_at(frame, "stale-search", 8, 308);
    Window* pane = panel_create();
    window_create(pane, "stale-pane", 60, 300, 100, 60, frame, true);
    Window* close = button_at(pane, "stale-close", 68, 308);
    desktop_set_focus(desktop, search);
    Window* box = dialog_create();
    window_create(box, "stale-box", 300, 300, 80, 40, nullptr, true);
    Window* ok = button_at(box, "Ok", 308, 308);
    desktop_set_focus(desktop, box);
    desktop_set_focus(desktop, ok);
    desktop_set_focus(desktop, search);
    window_destroy(box);
    // A click on the pane's own close button, which then hides the pane, as
    // matchmaking's Close does; the pane forgets its opener.
    desktop_set_focus(desktop, close);
    pane->focus_return = nullptr;
    pane->visible = 0;
    press(desktop, key::enter);
    check(
        desktop->focus == nullptr || desktop_holds(desktop, desktop->focus),
        "a key after a hidden focus lands on a window the desktop holds, or none"
    );
    window_destroy(frame);
}

// F6 starts from nothing while a region is showing, so the war map is
// reachable before anything has been clicked. A desktop with no region, and
// Tab from nothing, are left alone as before. A click marks the pointer as
// the last input, and a key clears the mark.
void test_f6_from_nothing(Desktop* desktop) {
    Window* lone = button_at(nullptr, "lone", 8, 440);
    desktop->keyboard_region = nullptr;
    desktop_set_focus(desktop, nullptr);
    check(!press(desktop, key::f6) && desktop->focus == nullptr, "F6 with no region is left alone");
    window_destroy(lone);

    Window* frame = panel_create();
    window_create(frame, "f6-frame", 0, 0, 120, 60, nullptr, true);
    frame->region = 1;
    Window* first = button_at(frame, "f6-first", 4, 4);
    Window* map = panel_create();
    window_create(map, "f6-map", 0, 80, 120, 40, nullptr, true);
    map->region = 3;
    Window* planet = button_at(map, "f6-planet", 4, 84);
    desktop->keyboard_region = nullptr;
    desktop_set_focus(desktop, nullptr);
    check(
        press(desktop, key::f6) && desktop->focus == first,
        "F6 from nothing lands on the first region"
    );
    check(desktop->keyboard_ring == 1, "F6 marks the keyboard ring");
    desktop->keyboard_region = nullptr;
    desktop_set_focus(desktop, nullptr);
    g_keys[key::shift] = 1;
    check(
        press(desktop, key::f6) && desktop->focus == planet,
        "Shift+F6 from nothing lands on the last region"
    );
    g_keys[key::shift] = 0;
    desktop_set_focus(desktop, nullptr);
    check(!press(desktop, key::tab) && desktop->focus == nullptr, "Tab from nothing is left alone");
    click_at(desktop, planet);
    check(
        desktop->pointer_last == 1 && desktop->focus == planet,
        "a click marks the pointer as the last input"
    );
    press(desktop, key::right);
    check(desktop->pointer_last == 0, "a key clears the mark");
    check(desktop->keyboard_ring == 1, "the keyboard ring stays until Return");
    // Right may leave the region. Return is tested on the planet, which is in one.
    desktop_set_focus(desktop, planet);
    check(
        press(desktop, key::enter) && desktop->keyboard_ring == 0, "Return ends the keyboard ring"
    );
    check(
        press(desktop, key::f6) && desktop->keyboard_ring == 1, "F6 marks the keyboard ring again"
    );
    Message move{};
    move.code = MessageCode::mouse_move;
    move.x = 12;
    move.y = 12;
    desktop_route(desktop, &move);
    check(press(desktop, key::f6) && desktop->keyboard_ring == 1, "F6 restores the keyboard ring");
    desktop_route(desktop, &move);
    check(desktop->keyboard_ring == 1, "a pointer move that stays put keeps the keyboard ring");
    move.x = 40;
    desktop_route(desktop, &move);
    check(desktop->keyboard_ring == 0, "a pointer move to a new place ends the keyboard ring");
    window_destroy(frame);
    window_destroy(map);
}

Window* radio_at(Window* parent, const char* name, int32_t x, int32_t y, uint8_t group) {
    Window* radio = radio_button_create();
    window_create(radio, name, x, y, 40, 16, parent, true);
    radio->radio_group = group;
    return radio;
}

// A row steps with Left and Right and leaves with Up and Down. A stack is
// the other way round. An arrow at the open console's edge stays there.
void test_radio_axes(Desktop* desktop) {
    Window* console = panel_create();
    window_create(console, "radio-console", 0, 0, 320, 160, nullptr, true);
    console->region = 2;
    Window* above = button_at(console, "above", 20, 8);
    Window* novice = radio_at(console, "Novice", 20, 40, 1);
    Window* seasoned = radio_at(console, "Seasoned", 80, 40, 1);
    Window* expert = radio_at(console, "Expert", 140, 40, 1);
    Window* below = button_at(console, "below", 20, 120);
    Window* edge = button_at(console, "edge", 260, 120);
    Window* guest = radio_at(console, "Guest", 240, 40, 2);
    Window* registered = radio_at(console, "Registered", 240, 70, 2);
    window_set_state(novice, button_state::pressed);
    window_set_state(seasoned, button_state::unpressed);
    window_set_state(expert, button_state::unpressed);
    window_set_state(registered, button_state::pressed);
    window_set_state(guest, button_state::unpressed);

    Window* map = panel_create();
    window_create(map, "radio-map", 0, 200, 320, 40, nullptr, true);
    map->region = 5;
    button_at(map, "planet-below", 20, 208);
    button_at(map, "planet-right", 400, 120);

    desktop_set_focus(desktop, novice);
    check(
        press(desktop, key::right) && desktop->focus == seasoned &&
            seasoned->states.current == button_state::pressed &&
            novice->states.current != button_state::pressed,
        "right steps a row and chooses"
    );
    check(
        press(desktop, key::right) && desktop->focus == expert &&
            expert->states.current == button_state::pressed,
        "right steps to the end of a row"
    );
    check(
        press(desktop, key::right) && desktop->focus == expert, "right at the end of a row stays"
    );
    check(
        press(desktop, key::left) && desktop->focus == seasoned &&
            seasoned->states.current == button_state::pressed,
        "left steps back along a row"
    );
    check(
        press(desktop, key::up) && desktop->focus == above &&
            seasoned->states.current == button_state::pressed,
        "up leaves a row without changing it"
    );
    desktop_set_focus(desktop, seasoned);
    check(
        press(desktop, key::down) && desktop->focus == below &&
            seasoned->states.current == button_state::pressed,
        "down leaves a row without changing it"
    );

    desktop_set_focus(desktop, registered);
    check(
        press(desktop, key::up) && desktop->focus == guest &&
            guest->states.current == button_state::pressed,
        "up steps a stack and chooses"
    );
    check(press(desktop, key::up) && desktop->focus == guest, "up at the end of a stack stays");
    check(
        press(desktop, key::down) && desktop->focus == registered &&
            registered->states.current == button_state::pressed,
        "down steps back along a stack"
    );
    check(
        press(desktop, key::left) && desktop->focus != registered &&
            registered->states.current == button_state::pressed,
        "left leaves a stack without changing it"
    );

    // edge is the last stop down and to the right. The arrow stays, and it
    // does not cross onto the map.
    desktop_set_focus(desktop, edge);
    check(press(desktop, key::down) && desktop->focus == edge, "down at the console's edge stays");
    check(
        press(desktop, key::right) && desktop->focus == edge, "right at the console's edge stays"
    );

    window_destroy(console);
    window_destroy(map);
}

// Three radio rows. Down from the top lands on the middle, even though a
// stop further down lines up with the chosen radio of the top row.
void test_radio_rows(Desktop* desktop) {
    Window* console = panel_create();
    window_create(console, "rows", 280, 280, 220, 160, nullptr, true);
    console->region = 2;
    Window* left = radio_at(console, "row-left", 300, 300, 1);
    Window* right = radio_at(console, "row-right", 420, 300, 1);
    Window* mid = radio_at(console, "row-mid", 300, 340, 2);
    Window* mid_right = radio_at(console, "row-mid-right", 420, 340, 2);
    Window* far = button_at(console, "row-far", 420, 400);
    window_set_state(left, button_state::unpressed);
    window_set_state(right, button_state::pressed);
    window_set_state(mid, button_state::pressed);
    window_set_state(mid_right, button_state::unpressed);

    desktop_set_focus(desktop, right);
    check(
        press(desktop, key::down) && desktop->focus == mid &&
            right->states.current == button_state::pressed,
        "down from a radio row lands on the next row"
    );
    check(
        press(desktop, key::up) && desktop->focus == right,
        "up from a radio row lands on the row above"
    );
    desktop_set_focus(desktop, mid);
    check(
        press(desktop, key::down) && desktop->focus == far,
        "down from the middle row reaches the stop that lines up below"
    );

    window_destroy(console);
}

// A button under a panel of another region is never a landing.
//
// The cover is a later child of the same top-level window, as an open console
// is a later child of the main frame. Focusing a button raises that top-level
// and leaves the cover in front, so a click at the hidden centre hits the cover.
void test_covered_stop(Desktop* desktop) {
    Window* frame = panel_create();
    window_create(frame, "cover-frame", 340, 180, 280, 180, nullptr, true);
    frame->region = 1;
    Window* above = button_at(frame, "cover-above", 450, 200);
    Window* hidden = button_at(frame, "cover-hidden", 450, 240);
    Window* below = button_at(frame, "cover-below", 450, 320);
    Window* left = button_at(frame, "cover-left", 360, 240);
    Window* right = button_at(frame, "cover-right", 560, 240);
    Window* cover = panel_create();
    window_create(cover, "cover", 430, 230, 80, 36, frame, true);
    cover->region = 2;

    desktop_set_focus(desktop, above);
    check(press(desktop, key::down) && desktop->focus == below, "down skips a covered control");
    desktop_set_focus(desktop, below);
    check(press(desktop, key::up) && desktop->focus == above, "up skips a covered control");
    desktop_set_focus(desktop, left);
    check(press(desktop, key::right) && desktop->focus == right, "right skips a covered control");
    desktop_set_focus(desktop, right);
    check(press(desktop, key::left) && desktop->focus == left, "left skips a covered control");
    check(desktop->focus != hidden, "no arrow lands on a covered control");

    window_destroy(cover);
    window_destroy(frame);
}

// The frame is not walled off from the war map.
void test_frame_reaches_map(Desktop* desktop) {
    Window* frame = panel_create();
    window_create(frame, "side-frame", 180, 400, 80, 40, nullptr, true);
    frame->region = 1;
    Window* frame_button = button_at(frame, "side-frame-button", 200, 408);
    Window* map = panel_create();
    window_create(map, "side-map", 360, 400, 80, 40, nullptr, true);
    map->region = 3;
    Window* planet = button_at(map, "side-planet", 380, 408);

    desktop_set_focus(desktop, frame_button);
    check(
        press(desktop, key::right) && desktop->focus == planet,
        "an arrow in the frame reaches the map beside it"
    );

    window_destroy(frame);
    window_destroy(map);
}

// Focusing a window raises it to the end of its parent's children. Tab still
// follows the order the windows were created in.
void test_tab_keeps_creation_order(Desktop* desktop) {
    Window* frame = panel_create();
    window_create(frame, "order-frame", 0, 200, 220, 40, nullptr, true);
    frame->region = 1;
    Window* first = button_at(frame, "order-first", 4, 204);
    Window* second = button_at(frame, "order-second", 50, 204);
    Window* third = button_at(frame, "order-third", 100, 204);
    desktop_set_focus(desktop, second);
    check(
        press(desktop, key::tab) && desktop->focus == third, "Tab follows creation, not the raise"
    );
    check(press(desktop, key::tab) && desktop->focus == first, "Tab wraps in creation order");
    window_destroy(frame);
}

// A control moved last, as the Locator moves Relocate and Page after rows
// that arrive later, is reached by Tab after them.
void test_moved_child_tabs_last(Desktop* desktop) {
    Window* frame = panel_create();
    window_create(frame, "moved-frame", 0, 200, 220, 40, nullptr, true);
    frame->region = 1;
    Window* first = button_at(frame, "moved-first", 4, 204);
    Window* after = button_at(frame, "moved-after", 50, 204);
    Window* row = button_at(frame, "moved-row", 100, 204);
    window_move_child_last(frame, after);
    desktop_set_focus(desktop, first);
    check(press(desktop, key::tab) && desktop->focus == row, "Tab reaches a later row first");
    check(press(desktop, key::tab) && desktop->focus == after, "Tab reaches a moved control last");
    window_destroy(frame);
}

// Chat rooms, the player list and the contents list scroll their own bars
// with the arrows, so Tab passes those bars. Recon's bar, and a page's
// HTMLSlider, stay stops.
void test_list_slider_is_not_a_tab_stop(Desktop* desktop) {
    Window* frame = panel_create();
    window_create(frame, "slider-frame", 0, 260, 360, 80, nullptr, true);
    frame->region = 1;
    Window* before = button_at(frame, "slider-before", 4, 264);
    const char* lists[] = {"Chat rooms", "ChatList", "ContentsList"};
    Window* boxes[3];
    for (int32_t i = 0; i < 3; ++i) {
        ListBox* list = list_box_create();
        window_create(&list->base, lists[i], 4, 264, 40, 16, frame, true);
        Slider* bar = slider_create();
        window_create(&bar->base, "slider", 50, 264, 12, 16, &list->base, true);
        bar->base.visible = 1;
        window_set_state(&bar->base, button_state::unpressed);
        boxes[i] = &list->base;
    }
    Window* after = button_at(frame, "slider-after", 80, 264);
    ListBox* recon = list_box_create();
    window_create(&recon->base, "ReconList", 140, 264, 40, 16, frame, true);
    Slider* recon_bar = slider_create();
    window_create(&recon_bar->base, "slider", 186, 264, 12, 16, &recon->base, true);
    recon_bar->base.visible = 1;
    window_set_state(&recon_bar->base, button_state::unpressed);
    Slider* page = slider_create();
    window_create(&page->base, "HTMLSlider", 220, 264, 12, 16, frame, true);
    page->base.visible = 1;
    window_set_state(&page->base, button_state::unpressed);

    desktop_set_focus(desktop, boxes[0]);
    check(
        press(desktop, key::tab) && desktop->focus == boxes[1], "Tab passes Chat rooms' scroll bar"
    );
    check(
        press(desktop, key::tab) && desktop->focus == boxes[2],
        "Tab passes the player list's scroll bar"
    );
    check(
        press(desktop, key::tab) && desktop->focus == after,
        "Tab passes the contents list's scroll bar"
    );
    check(
        press(desktop, key::tab) && desktop->focus == &recon->base,
        "Tab reaches Recon's description"
    );
    check(
        press(desktop, key::tab) && desktop->focus == &recon_bar->base,
        "Tab still stops on Recon's scroll bar"
    );
    check(
        press(desktop, key::tab) && desktop->focus == &page->base,
        "Tab still stops on the page's scroll bar"
    );
    desktop_set_focus(desktop, before);
    check(
        press(desktop, key::tab) && desktop->focus == boxes[0],
        "Tab reaches Chat rooms ahead of its scroll bar"
    );
    window_destroy(frame);
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
    TextService texts{};
    texts.create = caption_create;
    texts.get = caption_get;
    texts.release = caption_release;
    services.texts = &texts;
    services_install(services);

    Window* host = gadget_create();
    host->ops = &host_ops;
    window_create(host, "host", 0, 0, 640, 480, nullptr, true);

    test_grid(&desktop, host);
    test_text_and_dialog(&desktop, host);
    test_regions(&desktop);
    test_open_and_names(&desktop);
    test_stale_opener(&desktop);
    test_f6_from_nothing(&desktop);
    test_radio_axes(&desktop);
    test_radio_rows(&desktop);
    test_covered_stop(&desktop);
    test_frame_reaches_map(&desktop);
    test_tab_keeps_creation_order(&desktop);
    test_moved_child_tabs_last(&desktop);
    test_list_slider_is_not_a_tab_stop(&desktop);

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
