// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Edit boxes keep typed text whole: "Grüße", "Привет" and "東京" stay intact,
// backspace removes one character, and characters being composed are shown
// and then committed.
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

constexpr int32_t k_glyph = 5;
constexpr const char* k_gruesse = "Gr\xC3\xBC\xC3\x9F"
                                  "e";
constexpr const char* k_privet = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
constexpr const char* k_tokyo = "\xE6\x9D\xB1\xE4\xBA\xAC";
constexpr const char* k_east = "\xE6\x9D\xB1";
constexpr const char* k_capital = "\xE4\xBA\xAC";

int32_t g_beeps = 0;
bool g_shift = false;
bool g_control = false;
char g_drawn[160]{};
char g_prefix[160]{};

constexpr int32_t k_stored = 4;
char g_stored[k_stored][64]{};
int32_t g_stored_count = 0;

int32_t glyph_width(int32_t ch) {
    return ch == 0 ? 0 : k_glyph;
}

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

int32_t prefix_width(void*, Handle, const char* text, int32_t last) {
    std::snprintf(g_prefix, sizeof g_prefix, "%s", text != nullptr ? text : "");
    if (text == nullptr || last < 0) {
        return 0;
    }
    int32_t measured = 0;
    for (int32_t i = 0; i <= last && text[i] != '\0'; ++i) {
        measured += glyph_width(static_cast<unsigned char>(text[i]));
    }
    return measured;
}

void draw_string(void*, Handle, int32_t, int32_t, int32_t, int32_t, const char* text, int32_t) {
    std::snprintf(g_drawn, sizeof g_drawn, "%s", text != nullptr ? text : "");
}

void beep(void*) {
    ++g_beeps;
}

bool key_down(void*, int32_t key) {
    return (key == key::shift && g_shift) || (key == key::control && g_control);
}

int32_t line_height(void*, Handle) {
    return 12;
}

Handle create_color(void*, uint8_t, uint8_t, uint8_t, uint8_t) {
    return 1;
}

void release_color(void*, Handle) {
}

void fill(void*, int32_t, int32_t, int32_t, int32_t, Handle) {
}

Handle store_text(void*, const char* text, int32_t) {
    if (g_stored_count >= k_stored) {
        return k_no_handle;
    }
    const Handle handle = g_stored_count++;
    std::snprintf(g_stored[handle], sizeof g_stored[handle], "%s", text != nullptr ? text : "");
    return handle;
}

const char* load_text(void*, Handle handle) {
    if (handle < 0 || handle >= g_stored_count) {
        return "";
    }
    return g_stored[handle];
}

TextService g_text{};
FontService g_font{};
InputService g_input{};
ColorService g_color{};
SoundService g_sound{};

Services fake_services() {
    g_text.measure_string = measure_string;
    g_text.char_width = char_width;
    g_text.prefix_width = prefix_width;
    g_text.draw_string = draw_string;
    g_text.create = store_text;
    g_text.get = load_text;
    g_font.line_height = line_height;
    g_input.is_key_down = key_down;
    g_color.create = create_color;
    g_color.release = release_color;
    g_color.fill = fill;
    g_sound.beep = beep;
    Services installed = services();
    installed.texts = &g_text;
    installed.fonts = &g_font;
    installed.input = &g_input;
    installed.colors = &g_color;
    installed.sounds = &g_sound;
    return installed;
}

void press(Window* window, int32_t key) {
    Message message{};
    message.code = MessageCode::key_down;
    message.key = key;
    window_dispatch(window, &message);
}

void expect_text(
    const char* got, int32_t length, const char* want, int32_t want_length, const char* what
) {
    check(got != nullptr && std::strcmp(got, want) == 0 && length == want_length, what);
}

void test_edit_box() {
    EditBox* box = edit_box_create();
    box->base.font = 1;
    window_create(&box->base, "name", 8, 12, 400, 18, nullptr, true);
    edit_box_insert_text(box, "Grüße");
    expect_text(box->text, box->length, k_gruesse, 5, "edit box stores Grüße");
    edit_box_insert_text(box, "Привет");
    check(box->length == 11, "edit box counts Привет in characters");
    press(&box->base, key::backspace);
    check(
        box->length == 10 && std::strcmp(
                                 box->text,
                                 "Gr\xC3\xBC\xC3\x9F"
                                 "e\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5"
                             ) == 0,
        "edit box backspace removes one character"
    );
    box->base.ops->set_caption(&box->base, k_tokyo, 0);
    expect_text(box->text, box->length, k_tokyo, 2, "edit box stores 東京");
    check(box->caret == 2, "caret is after 東京");
    press(&box->base, key::left);
    check(box->caret == 1 && std::strcmp(box->text, k_tokyo) == 0, "left moves a whole character");
    press(&box->base, key::backspace);
    expect_text(box->text, box->length, k_capital, 1, "backspace removes 東");
    press(&box->base, key::left);
    check(g_beeps > 0 && box->caret == 0, "left at the start beeps");
    edit_box_set_composition(box, k_east);
    check(std::strcmp(edit_box_composition(box), k_east) == 0, "edit box composition is kept");
    check(std::strcmp(edit_box_shown(box), k_tokyo) == 0, "edit box shows the composition");
    check(std::strcmp(box->text, k_capital) == 0, "composition is not stored");
    const int32_t beeps = g_beeps;
    press(&box->base, key::backspace);
    check(
        g_beeps == beeps && std::strcmp(box->text, k_capital) == 0, "a composition keeps backspace"
    );
    edit_box_insert_text(box, k_east);
    expect_text(box->text, box->length, k_tokyo, 2, "committing stores 東");
    check(edit_box_composition(box)[0] == '\0', "committing clears the composition");
    g_drawn[0] = '\0';
    box->base.focused = 1;
    window_send(&box->base, MessageCode::draw);
    check(std::strcmp(g_drawn, k_tokyo) == 0, "edit box draws 東京");
    TextFieldPlace place{};
    check(window_text_field_place(&box->base, &place), "edit box has a field place");
    check(
        place.x == 8 && place.y == 12 && place.width == 400 && place.height == 18,
        "edit box place is the window"
    );
    // Committing 東 in front of 京 leaves the caret after that character
    // (three bytes) and one pixel past the padding.
    check(place.cursor == 3 * k_glyph + 1, "edit box caret follows the characters");
    window_destroy(&box->base);
}

void test_single_edit() {
    SingleEdit* edit = single_edit_create();
    edit->base.font = 1;
    window_create(&edit->base, "edit", 10, 20, 400, 24, nullptr, true);
    single_edit_insert_text(edit, "Grüße");
    expect_text(single_edit_text(edit), edit->length, k_gruesse, 5, "single edit stores Grüße");
    single_edit_set_text(edit, "");
    single_edit_insert_text(edit, "Привет");
    expect_text(single_edit_text(edit), edit->length, k_privet, 6, "single edit stores Привет");
    press(&edit->base, key::backspace);
    expect_text(
        single_edit_text(edit),
        edit->length,
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5",
        5,
        "single edit backspace removes one character"
    );
    single_edit_set_text(edit, k_tokyo);
    expect_text(single_edit_text(edit), edit->length, k_tokyo, 2, "single edit stores 東京");
    g_shift = true;
    press(&edit->base, key::left);
    check(
        edit->caret == 1 && edit->selection_start == 1 && edit->selection_end == 2,
        "shift+left selects one character"
    );
    g_shift = false;
    press(&edit->base, key::del);
    expect_text(
        single_edit_text(edit), edit->length, k_east, 1, "delete removes the selected character"
    );
    single_edit_set_text(edit, "\xE6\x9D\xB1 \xE4\xBA\xAC");
    single_edit_set_caret(edit, 0);
    g_control = true;
    press(&edit->base, key::right);
    g_control = false;
    check(
        edit->caret == 2 && std::strcmp(single_edit_text(edit), "\xE6\x9D\xB1 \xE4\xBA\xAC") == 0,
        "ctrl+right steps over a whole word"
    );
    single_edit_set_text(edit, k_east);
    single_edit_set_composition(edit, k_capital);
    check(std::strcmp(single_edit_shown(edit), k_tokyo) == 0, "single edit shows the composition");
    check(
        std::strcmp(single_edit_text(edit), k_east) == 0, "single edit composition is not stored"
    );
    edit->base.focused = 1;
    edit->blink = 0;
    g_drawn[0] = '\0';
    window_send(&edit->base, MessageCode::draw);
    check(std::strcmp(g_drawn, k_tokyo) == 0, "single edit draws the composition");
    const int32_t beeps = g_beeps;
    press(&edit->base, key::backspace);
    check(g_beeps == beeps && edit->length == 1, "composition keeps backspace from the text");
    single_edit_insert_text(edit, k_capital);
    expect_text(single_edit_text(edit), edit->length, k_tokyo, 2, "committing stores 京");
    check(
        single_edit_composition(edit)[0] == '\0', "committing clears the single edit composition"
    );
    edit->mask = '*';
    single_edit_set_composition(edit, k_east);
    g_drawn[0] = '\0';
    window_send(&edit->base, MessageCode::draw);
    check(std::strcmp(g_drawn, "***") == 0, "a mask hides the composition");
    check(std::strcmp(single_edit_text(edit), k_tokyo) == 0, "a mask leaves the text");
    edit->mask = '\0';
    single_edit_set_composition(edit, nullptr);
    TextFieldPlace place{};
    check(window_text_field_place(&edit->base, &place), "single edit has a field place");
    check(
        place.x == 10 && place.y == 20 && place.width == 400 && place.height == 24,
        "single edit place is the window"
    );
    // Padding 6, caret inset 2, two characters of six bytes.
    check(place.cursor == 6 - 2 + 6 * k_glyph, "single edit caret follows the characters");
    single_edit_set_max_length(edit, 2);
    single_edit_set_text(edit, "");
    const int32_t before = g_beeps;
    single_edit_insert_text(edit, "東京A");
    expect_text(single_edit_text(edit), edit->length, k_tokyo, 2, "a limit counts characters");
    check(g_beeps == before + 1, "the character past the limit beeps");
    edit->base.states.current = 0;
    single_edit_insert_text(edit, "Z");
    check(edit->length == 2, "a disabled edit takes no text");
    window_destroy(&edit->base);
}

void test_dropped_bytes_and_rows() {
    EditBox* box = edit_box_create();
    box->base.font = 1;
    window_create(&box->base, "name", 0, 0, 400, 18, nullptr, true);
    const int32_t beeps = g_beeps;
    edit_box_insert_text(
        box,
        "\xFF"
        "A\x01"
    );
    expect_text(box->text, box->length, "A", 1, "typed text drops bytes that are not characters");
    check(g_beeps == beeps, "a string that adds a character does not beep");
    edit_box_insert_text(box, "\x01");
    check(g_beeps == beeps + 1 && box->length == 1, "a string that adds nothing beeps");
    box->base.ops->set_caption(&box->base, "\xA3", 0);
    expect_text(box->text, box->length, "\xA3", 1, "a placed byte is kept as one character");
    press(&box->base, key::backspace);
    expect_text(box->text, box->length, "", 0, "backspace removes the placed byte");
    press(&box->base, key::left);
    check(std::strcmp(box->text, "") == 0, "left is not inserted");
    window_destroy(&box->base);

    Window* row = static_create();
    row->ops->set_caption(row, "東京", 0);
    check(std::strcmp(row->ops->state_text(row, 0), k_tokyo) == 0, "a row keeps 東京");
    check(std::strcmp("東京", k_tokyo) == 0, "東京 in the test is the stored bytes");
    check(std::strcmp("Grüße", k_gruesse) == 0, "Grüße in the test is the stored bytes");
    check(std::strcmp("Привет", k_privet) == 0, "Привет in the test is the stored bytes");
    window_destroy(row);
    Window* plain = window_create_plain();
    TextFieldPlace place{};
    check(!window_text_field_place(plain, &place), "a label is not a text field");
    window_destroy(plain);
}

} // namespace

int main() {
    const Services previous = services_install(fake_services());
    test_edit_box();
    test_single_edit();
    test_dropped_bytes_and_rows();
    services_install(previous);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("text input tests passed");
    return 0;
}
