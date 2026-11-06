// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// SingleEdit behaviour against fixed-width fake services (every glyph 5
// pixels wide) and an installed-data draw with a password mask.
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

constexpr int32_t k_glyph = 5;

// While set, '*' is 4 pixels and 'W' is 8; every other glyph stays k_glyph.
// The masked-layout test uses this so the caret follows asterisks.
int32_t g_varied_widths = 0;

int32_t glyph_width(int32_t ch) {
    if (g_varied_widths == 0) {
        return k_glyph;
    }
    if (ch == '*') {
        return 4;
    }
    if (ch == 'W') {
        return 8;
    }
    return k_glyph;
}

struct Fakes {
    bool shift = false;
    bool control = false;
    int32_t beeps = 0;
    int32_t fills = 0;
    int32_t last_fill_x = 0;
    int32_t last_fill_width = 0;
    Handle last_fill_color = k_no_handle;
    char drawn[128] = {};
    int32_t changes = 0;
};

Fakes g_fakes;

void measure_string(void*, Handle, int32_t* width, int32_t* height, const char* text) {
    int32_t measured = 0;
    for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
        measured += glyph_width(static_cast<unsigned char>(*p));
    }
    *width = measured;
    *height = 10;
}

int32_t char_width(void*, Handle, int32_t ch) {
    return glyph_width(ch);
}

void draw_string(void*, Handle, int32_t, int32_t, int32_t, int32_t, const char* text, int32_t) {
    std::snprintf(g_fakes.drawn, sizeof g_fakes.drawn, "%s", text);
}

int32_t line_height(void*, Handle) {
    return 12;
}

bool key_down(void*, int32_t k) {
    return (k == key::shift && g_fakes.shift) || (k == key::control && g_fakes.control);
}

constexpr Handle k_first_color = 100;
constexpr int32_t k_color_log = 64;
Handle next_color = k_first_color;
uint32_t g_color_rgba[k_color_log] = {};
int32_t g_color_releases = 0;
Handle g_last_released = k_no_handle;

Handle create_color(void*, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const Handle color = next_color++;
    if (color - k_first_color < k_color_log) {
        g_color_rgba[color - k_first_color] =
            (uint32_t{r} << 24) | (uint32_t{g} << 16) | (uint32_t{b} << 8) | a;
    }
    return color;
}

void release_color(void*, Handle color) {
    ++g_color_releases;
    g_last_released = color;
}

uint32_t rgba_of(Handle color) {
    const int32_t slot = color - k_first_color;
    return slot >= 0 && slot < k_color_log ? g_color_rgba[slot] : 0;
}

void fill(void*, int32_t x, int32_t, int32_t width, int32_t, Handle color) {
    ++g_fakes.fills;
    g_fakes.last_fill_x = x;
    g_fakes.last_fill_width = width;
    g_fakes.last_fill_color = color;
}

void beep(void*) {
    ++g_fakes.beeps;
}

TextService g_text{};
FontService g_font{};
InputService g_input{};
ColorService g_color{};
SoundService g_sound{};

Services fake_services() {
    g_text.measure_string = measure_string;
    g_text.char_width = char_width;
    g_text.draw_string = draw_string;
    g_font.line_height = line_height;
    g_input.is_key_down = key_down;
    g_color.create = create_color;
    g_color.release = release_color;
    g_color.fill = fill;
    g_sound.beep = beep;
    Services s{};
    s.texts = &g_text;
    s.fonts = &g_font;
    s.input = &g_input;
    s.colors = &g_color;
    s.sounds = &g_sound;
    return s;
}

void press(SingleEdit* edit, int32_t k) {
    Message msg{};
    msg.code = MessageCode::key_down;
    msg.key = k;
    window_dispatch(&edit->base, &msg);
}

void type(SingleEdit* edit, const char* text) {
    for (; *text != '\0'; ++text) {
        press(edit, *text);
    }
}

// 100-pixel box with the default 6-pixel padding: 88 pixels, 17 glyphs.
SingleEdit* make_edit(bool scroll) {
    SingleEdit* edit = single_edit_create();
    edit->base.font = 1;
    edit->scroll = scroll ? 1 : 0;
    window_create(&edit->base, "edit", 0, 0, 100, 20, nullptr, true);
    return edit;
}

void test_defaults() {
    SingleEdit* edit = make_edit(false);
    check(edit->base.states.count == 2 && edit->base.states.current == 1, "enabled state");
    check(edit->padding == 6 && edit->blink_period == 16 && edit->blink == 1, "defaults");
    check(edit->caret_height == -1 && edit->selection_height == -1, "derived heights");
    check(edit->selection_color >= 0 && edit->caret_color >= 0, "colours allocated");
    check(edit->base.ops->is_class(class_name::single_edit), "class name");
    check(edit->base.ops->state_index(&edit->base, "Enabled") == 1, "state names");
    window_destroy(&edit->base);
}

// The constructor takes a blue selection band and a white caret through the
// two colour setters; each setter releases only a colour that is set. The
// chat input replaces the band with 0x510000.
void test_colours() {
    SingleEdit* edit = make_edit(false);
    check(rgba_of(edit->selection_color) == 0x0000FFFFu, "blue selection band");
    check(rgba_of(edit->caret_color) == 0xFFFFFFFFu, "white caret");
    const Handle band = edit->selection_color;
    const int32_t releases = g_color_releases;
    single_edit_set_selection_color(edit, 0x51, 0, 0, 0xFF);
    check(
        g_color_releases == releases + 1 && g_last_released == band, "the previous band is released"
    );
    check(rgba_of(edit->selection_color) == 0x510000FFu, "chat input band");
    edit->caret_color = k_no_handle;
    single_edit_set_caret_color(edit, 1, 2, 3, 4);
    check(g_color_releases == releases + 1, "an unset caret colour is not released");
    check(rgba_of(edit->caret_color) == 0x01020304u, "new caret colour");
    window_destroy(&edit->base);
}

void test_typing_and_erasing() {
    SingleEdit* edit = make_edit(false);
    type(edit, "hello world");
    check(std::strcmp(single_edit_text(edit), "hello world") == 0 && edit->caret == 11, "typed");
    press(edit, key::left);
    press(edit, key::backspace);
    check(
        std::strcmp(single_edit_text(edit), "hello word") == 0 && edit->caret == 9,
        "backspace before caret"
    );
    press(edit, key::del);
    check(
        std::strcmp(single_edit_text(edit), "hello wor") == 0 && edit->caret == 9, "delete at caret"
    );
    g_fakes.beeps = 0;
    press(edit, key::del);
    check(g_fakes.beeps == 1, "delete at the end beeps");
    type(edit, "12345678");
    check(edit->length == 17, "fills the box");
    press(edit, 'x');
    check(edit->length == 17 && g_fakes.beeps == 2, "overflow refused without scrolling");
    single_edit_set_text(edit, "abcdefghijklmnopqrstuvwxyz");
    check(edit->length == 17 && edit->caret == 17, "set_text trims to the box");
    single_edit_set_max_length(edit, 3);
    single_edit_set_text(edit, "abcdef");
    check(std::strcmp(single_edit_text(edit), "abc") == 0, "set_text honours max length");
    press(edit, 'd');
    check(edit->length == 3 && g_fakes.beeps == 3, "max length refuses typing");
    edit->base.states.current = 0;
    press(edit, key::backspace);
    check(edit->length == 3, "disabled edit ignores keys");
    window_destroy(&edit->base);
}

void test_selection() {
    SingleEdit* edit = make_edit(false);
    type(edit, "one two three");
    g_fakes.shift = true;
    press(edit, key::left);
    press(edit, key::left);
    check(
        edit->selection_start == 11 && edit->selection_end == 13 && edit->caret == 11,
        "shift+left extends"
    );
    press(edit, key::right);
    check(edit->selection_start == 12 && edit->selection_end == 13, "shift+right shrinks");
    press(edit, key::home);
    check(
        edit->selection_start == 0 && edit->selection_end == 13 && edit->caret == 0,
        "shift+home keeps the anchor"
    );
    g_fakes.shift = false;
    press(edit, 'X');
    check(
        std::strcmp(single_edit_text(edit), "X") == 0 && edit->caret == 1,
        "typing replaces the selection"
    );
    single_edit_set_text(edit, "alpha beta gamma");
    single_edit_set_caret(edit, 0);
    g_fakes.control = true;
    press(edit, key::right);
    check(edit->caret == 6, "ctrl+right skips the word and its spaces");
    press(edit, key::right);
    check(edit->caret == 11, "ctrl+right again");
    press(edit, key::left);
    check(edit->caret == 6, "ctrl+left to the word start");
    g_fakes.control = false;
    single_edit_select(edit, 9, 2);
    check(edit->selection_start == 2 && edit->selection_end == 9, "select in either order");
    press(edit, key::del);
    check(
        std::strcmp(single_edit_text(edit), "ala gamma") == 0 && edit->caret == 2,
        "delete removes the selection"
    );
    single_edit_select(edit, -4, 99);
    check(edit->selection_start == 0 && edit->selection_end == 9, "select clamps");
    press(edit, key::end);
    check(
        edit->selection_start == 0 && edit->selection_end == 0 && edit->caret == 9,
        "plain movement clears the selection"
    );
    window_destroy(&edit->base);
}

void test_scrolling() {
    SingleEdit* edit = make_edit(true);
    type(edit, "abcdefghijklmnopqrstuvwxyz");
    check(edit->length == 26, "scrolling edit accepts long text");
    check(edit->last_visible == 26 && edit->first_visible == 9, "view follows the caret");
    press(edit, key::home);
    check(edit->first_visible == 0 && edit->last_visible == 17, "home scrolls back");
    press(edit, key::end);
    check(edit->first_visible == 9 && edit->last_visible == 26, "end scrolls forward");
    press(edit, key::backspace);
    press(edit, key::backspace);
    check(
        edit->length == 24 && edit->first_visible == 7 && edit->last_visible == 24,
        "erasing refills the view"
    );
    single_edit_set_text(edit, "0123456789012345678901234");
    check(edit->length == 25 && edit->first_visible == 8, "set_text scrolls, keeps all text");
    check(single_edit_caret_offset(edit) == 17 * k_glyph, "caret offset inside the view");
    window_destroy(&edit->base);
}

void test_draw_and_notify() {
    SingleEdit* edit = make_edit(false);
    edit->mask = '*';
    type(edit, "secret");
    edit->base.focused = 1;
    edit->blink = 0;
    g_fakes.fills = 0;
    window_send(&edit->base, MessageCode::draw);
    check(std::strcmp(g_fakes.drawn, "******") == 0, "mask drawn");
    check(std::strcmp(single_edit_text(edit), "secret") == 0, "text restored after drawing");
    check(
        g_fakes.fills == 1 && g_fakes.last_fill_color == edit->caret_color &&
            g_fakes.last_fill_x == 6 - 2 + 6 * k_glyph && g_fakes.last_fill_width == 2,
        "only the caret is filled"
    );
    single_edit_select(edit, 1, 3);
    edit->base.focused = 0;
    window_send(&edit->base, MessageCode::draw);
    check(
        g_fakes.last_fill_color == edit->selection_color && g_fakes.last_fill_x == 6 + 5 &&
            g_fakes.last_fill_width == 10,
        "selection band"
    );
    edit->notify_changes = 1;
    edit->on_changed = [](SingleEdit*) { ++g_fakes.changes; };
    press(edit, 'a');
    press(edit, key::left);
    check(g_fakes.changes == 1, "typed characters notify");
    window_destroy(&edit->base);
}

/// A masked edit lays out asterisks, so wide characters still fit and the caret
/// sits on the mask. The stored text and the selection stay on the characters typed.
void test_masked_layout() {
    g_varied_widths = 1;
    SingleEdit* edit = make_edit(false);
    edit->mask = '*';
    type(edit, "WWWWWWWWWWWWWWWWWWWWWW");
    check(
        edit->length == 22 && std::strcmp(single_edit_text(edit), "WWWWWWWWWWWWWWWWWWWWWW") == 0,
        "asterisks fit where the typed characters would not"
    );
    press(edit, 'W');
    check(edit->length == 22, "one more asterisk does not fit");
    edit->base.focused = 1;
    edit->blink = 0;
    g_fakes.fills = 0;
    window_send(&edit->base, MessageCode::draw);
    check(std::strcmp(g_fakes.drawn, "**********************") == 0, "twenty-two asterisks drawn");
    check(
        std::strcmp(single_edit_text(edit), "WWWWWWWWWWWWWWWWWWWWWW") == 0,
        "drawing leaves the typed text"
    );
    check(
        g_fakes.last_fill_x == 6 - 2 + 22 * 4 && g_fakes.last_fill_width == 2,
        "caret follows the asterisks"
    );
    single_edit_select(edit, 1, 4);
    edit->base.focused = 0;
    window_send(&edit->base, MessageCode::draw);
    check(
        g_fakes.last_fill_width == 12 && g_fakes.last_fill_color == edit->selection_color,
        "selection covers three asterisks"
    );
    single_edit_select(edit, 0, 0);
    single_edit_set_caret(edit, 4);
    g_fakes.shift = true;
    press(edit, key::left);
    check(
        edit->caret == 3 && edit->selection_start == 3 && edit->selection_end == 4 &&
            std::strcmp(single_edit_text(edit), "WWWWWWWWWWWWWWWWWWWWWW") == 0,
        "shift+left still selects the typed text"
    );
    g_fakes.shift = false;
    g_varied_widths = 0;
    window_destroy(&edit->base);
}

/// Draws a password-masked SingleEdit with the installed game data's font and checks that the
/// mask is drawn and the text kept.
void test_installed_draw() {
    InstalledData data;
    open_installed_data(&data);
    ToolkitConfig config = installed_config(&data, 120, 20);
    Toolkit* tk = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(tk));
    SingleEdit* edit = single_edit_create();
    edit->base.font = toolkit_default_font(tk);
    font_add_ref(edit->base.font);
    window_create(&edit->base, "password", 0, 0, 120, 20, nullptr, true);
    edit->mask = '*';
    single_edit_set_text(edit, "hunter2");
    window_send(&edit->base, MessageCode::draw);
    const Surface* overlay = toolkit_surface(tk);
    check(opaque_pixels(overlay) > 20, "masked text drawn");
    check(std::strcmp(single_edit_text(edit), "hunter2") == 0, "installed draw keeps the text");
    window_destroy(&edit->base);
    services_install(previous);
    toolkit_destroy(tk);
}

} // namespace

int main(int argc, char** argv) {
    // --data draws the edit with the BY1117a install's art instead.
    if (oa::test::game_data_requested(argc, argv)) {
        test_installed_draw();
        if (g_failures != 0) {
            std::fprintf(stderr, "%d failure(s)\n", g_failures);
            return 1;
        }
        std::puts("installed single edit drawing passed");
        return 0;
    }
    const Services previous = services_install(fake_services());
    test_defaults();
    test_colours();
    test_typing_and_erasing();
    test_selection();
    test_scrolling();
    test_draw_and_notify();
    test_masked_layout();
    services_install(previous);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("single edit tests passed");
    return 0;
}
