// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Standard control behaviour: buttons, check boxes, radio groups, stage
// buttons, progress bars and an installed-data draw of a check box.
#include <cstdio>
#include <cstring>

#include "test_data.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::testing;

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
    NotifyCode code;
    Window* source;
    int32_t value;
};

NotifyLog g_log;

void log_notify(Window*, const Notify* notify) {
    ++g_log.count;
    g_log.code = notify->code;
    g_log.source = notify->source;
    g_log.value = notify->value;
}

WindowOps g_host_ops;

Window* make_host() {
    Window* host = gadget_create();
    host->ops = &g_host_ops;
    window_create(host, "host", 0, 0, 200, 150, nullptr, true);
    return host;
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void test_button(Desktop* desktop) {
    Window* host = make_host();
    Window* button = button_create();
    window_create(button, "ok", 10, 10, 50, 20, host, true);
    check(
        button->states.count == 3 && button->states.current == button_state::unpressed,
        "button starts unpressed"
    );
    g_log = NotifyLog{};
    Message down = pointer(MessageCode::left_down, 15, 15, button_left);
    desktop_route(desktop, &down);
    check(button->states.current == button_state::pressed, "press");
    check(desktop->capture == button, "press captures");
    Message away = pointer(MessageCode::mouse_move, 100, 100, button_left);
    desktop_route(desktop, &away);
    check(button->states.current == button_state::unpressed, "dragging off unpresses");
    Message back = pointer(MessageCode::mouse_move, 20, 20, button_left);
    desktop_route(desktop, &back);
    check(button->states.current == button_state::pressed, "dragging back presses");
    Message up = pointer(MessageCode::left_up, 20, 20, 0);
    desktop_route(desktop, &up);
    check(
        g_log.count == 1 && g_log.source == button && g_log.code == NotifyCode::activated,
        "release over the button activates"
    );
    check(desktop->capture == nullptr, "release frees the capture");

    desktop_route(desktop, &down);
    Message up_away = pointer(MessageCode::left_up, 150, 100, 0);
    desktop_route(desktop, &up_away);
    check(g_log.count == 1, "release elsewhere does not activate");

    Message enter{};
    enter.code = MessageCode::key_down;
    enter.key = key::enter;
    desktop_set_focus(desktop, button);
    desktop_route(desktop, &enter);
    check(g_log.count == 2, "enter activates the focused button");

    // Button table: the right press and release are the left ones.
    Message right_down = pointer(MessageCode::right_down, 15, 15, button_right);
    Message right_up = pointer(MessageCode::right_up, 15, 15, 0);
    desktop_route(desktop, &right_down);
    check(
        button->states.current == button_state::pressed && desktop->capture == button,
        "right press presses"
    );
    desktop_route(desktop, &right_up);
    check(g_log.count == 3 && g_log.code == NotifyCode::activated, "right release activates");

    window_set_state(button, button_state::disabled);
    desktop_route(desktop, &down);
    check(
        button->states.current == button_state::disabled && desktop->capture == nullptr,
        "disabled button ignores presses"
    );
    window_destroy(host);
}

// Class names and state tables: Button ("Button"), Gadget ("Gadget") and
// Panel ("Panel"), each false for null; the button's names come from its
// three-entry state-name table through state_index and state_name.
void test_class_and_state_names() {
    Window* button = button_create();
    const WindowOps* ops = button->ops;
    check(
        ops->is_class("Button") && !ops->is_class("Gadget") && !ops->is_class(nullptr),
        "Button class name"
    );
    check(
        ops->state_index(button, "Disabled") == 0 && ops->state_index(button, "Pressed") == 1 &&
            ops->state_index(button, "Unpressed") == 2,
        "button state indices"
    );
    check(
        ops->state_index(button, "Normal") == -1 && ops->state_index(button, nullptr) == -1,
        "unknown button state"
    );
    check(
        std::strcmp(ops->state_name(button, 2), "Unpressed") == 0 &&
            ops->state_name(button, 3) == nullptr && ops->state_name(button, -1) == nullptr,
        "button state names"
    );
    window_destroy(button);

    Window* gadget = gadget_create();
    check(
        gadget->ops->is_class("Gadget") && !gadget->ops->is_class("Window") &&
            !gadget->ops->is_class(nullptr),
        "Gadget class name"
    );
    check(gadget->ops->state_index(gadget, "Normal") == 1, "gadget keeps the window states");
    window_destroy(gadget);

    Window* panel = panel_create();
    check(
        panel->ops->is_class("Panel") && !panel->ops->is_class("Gadget") &&
            !panel->ops->is_class(nullptr),
        "Panel class name"
    );
    window_destroy(panel);

    // Class-name slots: Slider, Dialog, Static, SButton, ListBox, CheckBox,
    // RadioButton and NoFocusBtn.
    struct ClassName {
        const WindowOps* ops;
        const char* name;
    };

    const ClassName classes[] = {
        {&k_slider_ops, "Slider"},
        {&k_dialog_ops, "Dialog"},
        {&k_static_ops, "Static"},
        {&k_scroll_button_ops, "SButton"},
        {&k_list_box_ops, "ListBox"},
        {&k_check_box_ops, "CheckBox"},
        {&k_radio_button_ops, "RadioButton"},
        {&k_no_focus_button_ops, "NoFocusBtn"},
    };
    for (const ClassName& entry : classes) {
        check(
            entry.ops->is_class(entry.name) && !entry.ops->is_class("Gadget") &&
                !entry.ops->is_class(nullptr),
            entry.name
        );
    }

    // Check box names from its five-entry table; radio names from the
    // button's three-entry table.
    CheckBox* box = check_box_create();
    Window* b = &box->base;
    check(
        b->ops->state_index(b, "Unchecked Down") == 1 && b->ops->state_index(b, "Checked Up") == 4,
        "check box state indices"
    );
    check(
        b->ops->state_index(b, "Pressed") == -1 && b->ops->state_index(b, nullptr) == -1,
        "unknown check box state"
    );
    check(
        std::strcmp(b->ops->state_name(b, 3), "Unchecked Up") == 0 &&
            b->ops->state_name(b, 5) == nullptr && b->ops->state_name(b, -1) == nullptr,
        "check box state names"
    );
    window_destroy(b);
    Window* radio = radio_button_create();
    check(
        radio->ops->state_index(radio, "Unpressed") == 2 &&
            radio->ops->state_index(radio, "Checked Up") == -1,
        "radio state indices"
    );
    check(
        std::strcmp(radio->ops->state_name(radio, 1), "Pressed") == 0 &&
            radio->ops->state_name(radio, 3) == nullptr,
        "radio state names"
    );
    window_destroy(radio);
}

void test_check_box(Desktop* desktop) {
    Window* host = make_host();
    CheckBox* box = check_box_create();
    window_create(&box->base, "check", 10, 40, 80, 12, host, true);
    check(!check_box_checked(box), "unchecked initially");
    g_log = NotifyLog{};
    Message down = pointer(MessageCode::left_down, 12, 42, button_left);
    Message up = pointer(MessageCode::left_up, 12, 42, 0);
    desktop_route(desktop, &down);
    check(box->base.states.current == check_state::unchecked_down, "pressed unchecked");
    desktop_route(desktop, &up);
    check(check_box_checked(box) && g_log.value == 1, "click checks and reports 1");
    desktop_route(desktop, &down);
    desktop_route(desktop, &up);
    check(!check_box_checked(box) && g_log.value == 0 && g_log.count == 2, "click unchecks");
    // CheckBox table: a right double click is a right press, itself a left
    // press, and the right release a left one.
    Message right_double = pointer(MessageCode::right_double, 12, 42, button_right);
    Message right_up = pointer(MessageCode::right_up, 12, 42, 0);
    desktop_route(desktop, &right_double);
    check(box->base.states.current == check_state::unchecked_down, "right double click presses");
    desktop_route(desktop, &right_up);
    check(check_box_checked(box) && g_log.value == 1 && g_log.count == 3, "right release checks");
    check(
        box->base.ops->state_index(&box->base, "Checked Up") == check_state::checked_up,
        "check box state names"
    );
    window_destroy(host);
}

void test_radio_group(Desktop* desktop) {
    Window* host = make_host();
    Window* a = radio_button_create();
    Window* b = radio_button_create();
    window_create(a, "a", 10, 60, 10, 10, host, true);
    window_create(b, "b", 30, 60, 10, 10, host, true);
    Message down_a = pointer(MessageCode::left_down, 12, 62, button_left);
    Message up_a = pointer(MessageCode::left_up, 12, 62, 0);
    Message down_b = pointer(MessageCode::left_down, 32, 62, button_left);
    Message up_b = pointer(MessageCode::left_up, 32, 62, 0);
    desktop_route(desktop, &down_a);
    desktop_route(desktop, &up_a);
    check(a->states.current == button_state::pressed, "a selected");
    desktop_route(desktop, &down_b);
    desktop_route(desktop, &up_b);
    check(b->states.current == button_state::pressed, "b selected");
    check(a->states.current == button_state::unpressed, "a deselected by b");
    window_destroy(host);
}

void test_stage_button(Desktop* desktop) {
    Window* host = make_host();
    StageButton* stage = stage_button_create();
    window_set_state_count(&stage->base, 5);
    window_set_state(&stage->base, 2);
    window_create(&stage->base, "stage", 10, 80, 40, 12, host, true);
    Message down = pointer(MessageCode::left_down, 12, 82, button_left);
    Message up = pointer(MessageCode::left_up, 12, 82, 0);
    const int32_t expected[] = {3, 4, 2, 3};
    bool ok = true;
    for (int32_t next : expected) {
        desktop_route(desktop, &down);
        ok = ok && stage->base.states.current == button_state::pressed;
        desktop_route(desktop, &up);
        ok = ok && stage->base.states.current == next;
    }
    check(ok, "stage button cycles First..last and wraps");
    window_destroy(host);
}

struct FillLog {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

FillLog g_fill;

void log_fill(void*, int32_t x, int32_t y, int32_t w, int32_t h, Handle) {
    g_fill = FillLog{x, y, w, h};
}

Handle fake_color(void*, uint8_t, uint8_t, uint8_t, uint8_t) {
    return 1;
}

// The desktop router treats 0x200 as a move (tracking the hover window) and
// only 0x201..0x209 as button messages that move the focus to the window
// under the pointer.
void test_pointer_codes(Desktop* desktop) {
    Window* host = make_host();
    Window* left = button_create();
    Window* right = button_create();
    window_create(left, "left", 10, 10, 40, 20, host, true);
    window_create(right, "right", 100, 10, 40, 20, host, true);
    desktop_set_focus(desktop, left);
    Message wheel = pointer(MessageCode::wheel, 110, 15, 0);
    check(
        !desktop_route(desktop, &wheel) && desktop->focus == left,
        "the wheel code is not a button message"
    );
    Message middle = pointer(MessageCode::middle_double, 110, 15, 0);
    desktop_route(desktop, &middle);
    check(desktop->focus == right, "a middle double click focuses the window under it");
    Message move = pointer(MessageCode::mouse_move, 20, 15, 0);
    desktop_route(desktop, &move);
    check(desktop->hover == left && desktop->focus == right, "a move sets the hover only");
    check(!desktop_route(desktop, nullptr), "no message");
    window_destroy(host);
}

void test_progress() {
    ColorService colors{nullptr, nullptr, fake_color, log_fill};
    Services s = services();
    const Services previous = s;
    s.colors = &colors;
    services_install(s);
    Progress* bar = progress_create();
    window_create(&bar->base, "bar", 10, 10, 100, 8, nullptr, true);
    g_fill = FillLog{-1, -1, -1, -1};
    window_send(&bar->base, MessageCode::draw);
    check(g_fill.w == -1, "zero value draws nothing");
    progress_set(bar, 3, 4);
    window_send(&bar->base, MessageCode::draw);
    check(g_fill.x == 10 && g_fill.w == 75 && g_fill.h == 8, "horizontal fill is value/range");
    bar->reverse = 1;
    window_send(&bar->base, MessageCode::draw);
    check(g_fill.x == 35 && g_fill.w == 75, "reversed fill ends at the right");
    progress_set(bar, 9, 4);
    bar->reverse = 0;
    window_send(&bar->base, MessageCode::draw);
    check(g_fill.w == 100, "fill clamps to the width");
    window_create(&bar->base, "bar", 10, 10, 8, 100, nullptr, true);
    progress_set(bar, 1, 4);
    window_send(&bar->base, MessageCode::draw);
    check(g_fill.y == 85 && g_fill.h == 25, "vertical fill grows upwards");
    window_destroy(&bar->base);
    services_install(previous);
}

int32_t g_char_width = 5;
int32_t g_star_width = 5;
int32_t g_box_beeps = 0;
char g_box_drawn[160]{};
char g_prefix_text[160]{};

int32_t edit_glyph(int32_t ch) {
    return ch == '*' ? g_star_width : g_char_width;
}

void edit_measure(void*, Handle, int32_t* width, int32_t* height, const char* text) {
    int32_t measured = 0;
    for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
        measured += edit_glyph(static_cast<unsigned char>(*p));
    }
    *width = measured;
    *height = 8;
}

int32_t edit_char_width(void*, Handle, int32_t ch) {
    return edit_glyph(ch);
}

int32_t edit_prefix(void*, Handle, const char* text, int32_t last) {
    std::snprintf(g_prefix_text, sizeof g_prefix_text, "%s", text != nullptr ? text : "");
    if (text == nullptr || last < 0) {
        return 0;
    }
    int32_t measured = 0;
    for (int32_t i = 0; i <= last && text[i] != '\0'; ++i) {
        measured += edit_glyph(static_cast<unsigned char>(text[i]));
    }
    return measured;
}

void edit_draw_string(
    void*, Handle, int32_t, int32_t, int32_t, int32_t, const char* text, int32_t
) {
    std::snprintf(g_box_drawn, sizeof g_box_drawn, "%s", text != nullptr ? text : "");
}

void edit_beep(void*) {
    ++g_box_beeps;
}

void type_box(EditBox* edit, const char* text) {
    Message typed{};
    typed.code = MessageCode::key_down;
    for (; *text != '\0'; ++text) {
        typed.key = *text;
        window_dispatch(&edit->base, &typed);
    }
}

/// A masked edit box draws asterisks, keeps the typed text and sizes the caret
/// and the box from the mask character.
void test_edit_box_password() {
    TextService texts{};
    texts.measure_string = edit_measure;
    texts.char_width = edit_char_width;
    texts.prefix_width = edit_prefix;
    texts.draw_string = edit_draw_string;
    SoundService sounds{};
    sounds.beep = edit_beep;
    Services hooked = services();
    hooked.texts = &texts;
    hooked.sounds = &sounds;
    const Services previous = services_install(hooked);

    g_char_width = 8;
    g_star_width = 5;
    g_box_beeps = 0;
    EditBox* plain = edit_box_create();
    plain->base.font = 1;
    window_create(&plain->base, "name", 0, 0, 20, 14, nullptr, true);
    type_box(plain, "abcd");
    check(
        plain->length == 2 && std::strcmp(plain->text, "ab") == 0,
        "an unmasked box still fits the typed characters"
    );
    window_destroy(&plain->base);

    EditBox* edit = edit_box_create();
    edit->mask = '*';
    edit->base.font = 1;
    window_create(&edit->base, "password", 0, 0, 20, 14, nullptr, true);
    type_box(edit, "abcd");
    check(
        edit->length == 4 && std::strcmp(edit->text, "abcd") == 0,
        "asterisks fit where the typed characters would not"
    );
    const int32_t beeps = g_box_beeps;
    type_box(edit, "e");
    check(edit->length == 4 && g_box_beeps == beeps + 1, "one more asterisk does not fit");
    edit->base.focused = 1;
    window_send(&edit->base, MessageCode::draw);
    check(std::strcmp(g_box_drawn, "****") == 0, "edit box draws asterisks");
    check(std::strcmp(g_prefix_text, "****") == 0, "caret measures asterisks");
    check(std::strcmp(edit->text, "abcd") == 0, "drawing leaves the typed text");
    Message left{};
    left.code = MessageCode::key_down;
    left.key = key::left;
    window_dispatch(&edit->base, &left);
    check(
        edit->caret == 3 && std::strcmp(edit->text, "abcd") == 0, "caret still moves in the text"
    );
    edit->base.ops->set_caption(&edit->base, "uvwxyz", 0);
    check(std::strcmp(edit->text, "uvwx") == 0, "a caption is trimmed to the asterisks that fit");
    window_destroy(&edit->base);
    services_install(previous);
}

/// Draws a check box with the installed game data's widget art and font and checks its pixels,
/// then types into an edit box in that font and checks the width limits.
void test_installed_draw() {
    InstalledData data;
    open_installed_data(&data);
    ToolkitConfig config = installed_config(&data, 120, 40);
    Toolkit* tk = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(tk));
    CheckBox* box = check_box_create();
    window_create(&box->base, "check", 4, 4, 100, 20, nullptr, true);
    box->base.font = toolkit_default_font(tk);
    font_add_ref(box->base.font);
    box->base.ops->bind_images(&box->base, "Widgets.gaf", "Check_Box", 0, 9);
    box->base.ops->set_caption(&box->base, "Remember me", text_style::left);
    window_send(&box->base, MessageCode::draw);
    const Surface* overlay = toolkit_surface(tk);
    check(opaque_pixels(overlay) > 60, "check box image and caption drawn");
    check(
        overlay->pixels[(4 + 6) * overlay->pitch + 4] != k_overlay_transparent,
        "box image centred vertically at the left edge"
    );
    const uint32_t digest = surface_digest(overlay);
    std::printf("check box snapshot digest %08x\n", digest);
    check(digest == 0x5DF3B6C5U, "check box snapshot matches");
    window_destroy(&box->base);

    EditBox* edit = edit_box_create();
    edit->base.font = toolkit_default_font(tk);
    font_add_ref(edit->base.font);
    window_create(&edit->base, "name", 0, 0, 30, 14, nullptr, true);
    check(edit->caret_height == 11, "caret height is the I height plus two");
    Message typed{};
    typed.code = MessageCode::key_down;
    for (const char* c = "Hello"; *c != '\0'; ++c) {
        typed.key = *c;
        window_dispatch(&edit->base, &typed);
    }
    check(std::strcmp(edit->text, "Hello") == 0 && edit->caret == 5, "typing appends");
    typed.key = key::left;
    window_dispatch(&edit->base, &typed);
    typed.key = key::backspace;
    window_dispatch(&edit->base, &typed);
    check(std::strcmp(edit->text, "Helo") == 0 && edit->caret == 3, "backspace before caret");
    typed.key = key::del;
    window_dispatch(&edit->base, &typed);
    check(std::strcmp(edit->text, "Hel") == 0, "delete at caret");
    for (int i = 0; i < 20; ++i) {
        typed.key = 'W';
        window_dispatch(&edit->base, &typed);
    }
    check(text_char_width(edit->base.font, 'W') > 0 && edit->length < 10, "width limits typing");
    edit->base.ops->set_caption(&edit->base, "A much longer caption than fits", 0);
    int32_t w = 0;
    int32_t h = 0;
    text_measure_string(edit->base.font, edit->text, &w, &h);
    check(w <= 30 && edit->caret == edit->length, "caption is trimmed to the box");
    window_destroy(&edit->base);
    services_install(previous);
    toolkit_destroy(tk);
}

} // namespace

int main(int argc, char** argv) {
    // --data draws the controls with the BY1117a install's art instead.
    if (oa::test::game_data_requested(argc, argv)) {
        test_installed_draw();
        if (g_failures != 0) {
            std::fprintf(stderr, "%d failure(s)\n", g_failures);
            return 1;
        }
        std::puts("installed control drawing passed");
        return 0;
    }
    g_host_ops = k_gadget_ops;
    g_host_ops.on_notify = log_notify;

    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Services s{};
    s.desktop = &desktop;
    services_install(s);
    test_button(&desktop);
    test_class_and_state_names();
    test_check_box(&desktop);
    test_radio_group(&desktop);
    test_stage_button(&desktop);
    test_pointer_codes(&desktop);
    test_progress();
    test_edit_box_password();
    desktop_release(&desktop);
    services_install(Services{});

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("boneyards control tests passed");
    return 0;
}
