// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat log and entry line at the player's Text size: row height, wrapping,
// scrolling to the newest line, and the entry line's growth. Modern fonts
// off keeps the Boneyards' own sizes.
#include <cstdio>
#include <cstring>

#include "oa/boneyards/toolkit.hpp"

using namespace oa::boneyards;

namespace {

int g_failures = 0;

void fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

void expect_i(const char* what, int32_t got, int32_t want) {
    if (got != want) {
        std::fprintf(
            stderr, "FAIL: %s got %d want %d\n", what, static_cast<int>(got), static_cast<int>(want)
        );
        ++g_failures;
    }
}

void expect_true(const char* what, bool condition) {
    if (!condition)
        fail(what);
}

// Rows the log recorded, in the order they were added.
constexpr int32_t k_note_limit = 64;
constexpr int32_t k_row_width = 357;

struct Added {
    ListBox* list;
    int32_t width;
    int32_t indents[k_note_limit];
    int32_t heights[k_note_limit];
    int32_t count;
};

void add_row(void* user, const char* line, int32_t indent, int32_t height, uint8_t /*ink*/) {
    auto* added = static_cast<Added*>(user);
    Window* row = static_create();
    window_set_state_count(row, 1);
    window_set_state(row, 0);
    window_set_state_text(row, 0, line, 0);
    window_create(row, "row", 0, 0, added->width, height, nullptr, true);
    list_box_add(added->list, row);
    if (added->count < k_note_limit) {
        added->indents[added->count] = indent;
        added->heights[added->count] = height;
        ++added->count;
    }
}

bool write_line(Added* added, const char* text) {
    return modern_chat_write(added->list, text, k_row_width, 6, 7, 40, add_row, added);
}

ListBox* make_log(int32_t row_height) {
    ListBox* list = list_box_create();
    const int32_t height = row_height * 2 + 1;
    window_create(&list->base, "ChatList", 0, 0, 374, height, nullptr, true);
    list->bottom_align = 1;
    list->log_mode = 1;
    list->capacity = 100;
    Slider* bar = slider_create();
    window_create(&bar->base, "slider", 358, 0, 16, height, &list->base, true);
    return list;
}

void clear_log(Added* added) {
    list_box_clear(added->list);
    added->count = 0;
}

const char* row_text(ListBox* list, int32_t index) {
    Window* row = list_box_item(list, index);
    if (row == nullptr || row->ops->state_text == nullptr)
        return "";
    const char* text = row->ops->state_text(row, -1);
    return text != nullptr ? text : "";
}

int32_t count_ink(const Surface* surface, uint8_t ink) {
    int32_t count = 0;
    if (surface == nullptr || surface->pixels == nullptr)
        return 0;
    for (int32_t y = 0; y < surface->height; ++y) {
        const uint8_t* row = surface->pixels + static_cast<std::ptrdiff_t>(y) * surface->pitch;
        for (int32_t x = 0; x < surface->width; ++x) {
            if (row[x] == ink)
                ++count;
        }
    }
    return count;
}

void clear_surface(Surface* surface) {
    if (surface == nullptr || surface->pixels == nullptr)
        return;
    for (int32_t y = 0; y < surface->height; ++y)
        std::memset(
            surface->pixels + static_cast<std::ptrdiff_t>(y) * surface->pitch,
            0,
            static_cast<std::size_t>(surface->width)
        );
}

// A word wider than the row, so the line has to break inside it.
void fill_word(char* dest, int32_t dest_bytes, int32_t wider_than) {
    int32_t used = 0;
    dest[0] = '\0';
    while (used + 2 < dest_bytes && modern_chat_width(dest) <= wider_than) {
        dest[used] = 'M';
        ++used;
        dest[used] = '\0';
    }
}

void test_at_80(Added* added, SingleEdit* marked, SingleEdit* plain, Surface* surface) {
    const ModernChatLine* line = modern_chat_line();
    expect_true("faces open at 80", line != nullptr && modern_chat_on());
    if (line == nullptr)
        return;
    expect_i("80 pixel size", line->pixel_size, 11);
    expect_i("80 least cjk", line->least_cjk, 12);
    expect_i("80 ascent", line->ascent, 14);
    expect_i("80 descent", line->descent, 4);
    expect_i("80 row", line->row_height, 18);
    expect_i("80 entry", line->entry_height, 18);
    expect_i("empty width", modern_chat_width(""), 0);
    expect_i("null width", modern_chat_width(nullptr), 0);

    char word[256];
    fill_word(word, static_cast<int32_t>(sizeof word), k_row_width);
    const int32_t before = added->count;
    expect_true("long word is written", write_line(added, word));
    expect_true("long word wraps", added->count > before + 1);
    if (added->count > before) {
        expect_i("first indent", added->indents[before], 6);
        const int32_t continuation = modern_chat_width("       ");
        for (int32_t i = before + 1; i < added->count; ++i)
            expect_i("continuation indent", added->indents[i], continuation);
        expect_i("wrapped row height", added->heights[before], line->row_height);
    }

    // A glyph wider than the row still takes a row of its own.
    const int32_t narrow_before = added->list->count;
    expect_true(
        "wide glyph is written", modern_chat_write(added->list, "WW", 1, 6, 7, 40, add_row, added)
    );
    expect_i("wide glyph rows", added->list->count, narrow_before + 2);

    clear_log(added);
    expect_true("empty line adds nothing", write_line(added, ""));
    expect_i("empty line count", added->list->count, 0);
    expect_true("first line on an empty log", write_line(added, "one"));
    expect_i("first line visible", list_box_item(added->list, 0)->visible, 1);
    expect_i("empty log was at its end", added->list->last, 0);

    for (int32_t n = 0; n < 5; ++n) {
        char label[8];
        std::snprintf(label, sizeof label, "n%d", static_cast<int>(n));
        expect_true("fill line", write_line(added, label));
    }
    expect_true("log overflowed", added->list->count > 2);
    expect_i("followed the end", added->list->last, added->list->count - 1);
    Window* newest = list_box_item(added->list, added->list->count - 1);
    expect_true("newest row", newest != nullptr);
    if (newest != nullptr)
        expect_i("newest visible", newest->visible, 1);
    expect_i("oldest hidden", list_box_item(added->list, 0)->visible, 0);

    list_box_scroll(added->list, -100);
    const int32_t top = added->list->top;
    const int32_t count = added->list->count;
    expect_i("scrolled to the top", top, 0);
    expect_true("append while reading history", write_line(added, "history"));
    expect_i("top stays", added->list->top, top);
    expect_i("history grew", added->list->count, count + 1);
    Window* hidden = list_box_item(added->list, added->list->count - 1);
    expect_true("history row", hidden != nullptr);
    if (hidden != nullptr)
        expect_i("newest stays hidden", hidden->visible, 0);
    expect_true(
        "row text kept", std::strcmp(row_text(added->list, added->list->count - 1), "history") == 0
    );

    list_box_scroll_to_end(added->list);
    expect_true("append at the end", write_line(added, "live"));
    expect_i("end followed again", added->list->last, added->list->count - 1);
    Window* live = list_box_item(added->list, added->list->count - 1);
    expect_true("live row", live != nullptr);
    if (live != nullptr) {
        expect_i("live visible", live->visible, 1);
        expect_true(
            "live text", std::strcmp(row_text(added->list, added->list->count - 1), "live") == 0
        );
    }

    const int32_t bottom = marked->base.y + marked->base.height;
    single_edit_set_selection_height(marked, 14);
    modern_chat_mark(&marked->base, 40);
    single_edit_set_text(marked, "Hello");
    expect_i("entry height at 80", marked->base.height, line->entry_height);
    expect_i("entry bottom stays", marked->base.y + marked->base.height, bottom);
    expect_true("entry grew upward", marked->base.y < bottom - 14);
    expect_i("selection follows the entry", marked->selection_height, marked->base.height);

    const int32_t plain_y = plain->base.y;
    const int32_t plain_h = plain->base.height;
    single_edit_set_text(plain, "Hello");
    expect_i("unmarked entry y", plain->base.y, plain_y);
    expect_i("unmarked entry height", plain->base.height, plain_h);
    single_edit_set_text(plain, "東京");
    expect_i("unmarked cjk y", plain->base.y, plain_y);
    expect_i("unmarked cjk height", plain->base.height, plain_h);

    clear_surface(surface);
    expect_true(
        "paint hello", modern_chat_paint("Hello", 8, 24, 40, 0, 0, surface->width, surface->height)
    );
    expect_true("hello left ink", count_ink(surface, 40) > 0);

    Window* label = static_create();
    window_set_state_count(label, 1);
    window_set_state(label, 0);
    window_set_state_text(label, 0, "Hello", 0);
    window_create(label, "hello", 4, 40, 140, 24, nullptr, true);
    modern_chat_mark(label, 40);
    clear_surface(surface);
    label->ops->draw(label);
    expect_true("label left ink", count_ink(surface, 40) > 0);
    modern_chat_unmark(label);
    clear_surface(surface);
    label->ops->draw(label);
    expect_i("unmarked label leaves no modern ink", count_ink(surface, 40), 0);
    window_destroy(label);
}

void test_at_150(Added* added, SingleEdit* marked, int32_t default_row, int32_t default_entry) {
    modern_chat_set(true, 150);
    const ModernChatLine* line = modern_chat_line();
    expect_true("faces open at 150", line != nullptr && modern_chat_on());
    if (line == nullptr)
        return;
    expect_i("150 pixel size", line->pixel_size, 21);
    expect_i("150 least cjk", line->least_cjk, 12);
    const int32_t rows = line->ascent + line->descent;
    const int32_t from_face = line->pixel_size + 6;
    const int32_t entry = rows > from_face ? rows : from_face;
    expect_i("150 row", line->row_height, rows < 17 ? 17 : rows);
    expect_i("150 entry", line->entry_height, entry < 14 ? 14 : entry);
    expect_true("150 row is taller", line->row_height > default_row);
    expect_true("150 entry is taller", line->entry_height > default_entry);

    window_set_height(&added->list->base, line->row_height * 2 + 1);
    clear_log(added);
    expect_true("150 line", write_line(added, "large"));
    expect_i("150 new row", added->heights[0], line->row_height);

    const int32_t bottom = marked->base.y + marked->base.height;
    single_edit_set_text(marked, "Hello");
    expect_i("entry height at 150", marked->base.height, line->entry_height);
    expect_i("entry bottom stays at 150", marked->base.y + marked->base.height, bottom);
    expect_true("entry grew further", marked->base.height > default_entry);
}

void test_off(Added* added, SingleEdit* marked) {
    const int32_t top = added->list->top;
    const int32_t count = added->list->count;
    modern_chat_set(false, 80);
    expect_true("modern off", !modern_chat_on());
    expect_true("no line while off", modern_chat_line() == nullptr);
    const int32_t notes = added->count;
    expect_true("off writes nothing", !write_line(added, "silent"));
    expect_i("off count", added->list->count, count);
    expect_i("off top", added->list->top, top);
    expect_i("off notes", added->count, notes);

    modern_chat_unmark(&marked->base);
    single_edit_set_text(marked, "Hello");
    expect_i("entry y restored", marked->base.y, 200);
    expect_i("entry height restored", marked->base.height, 14);
    single_edit_set_text(marked, "東京");
    expect_i("entry y restored for cjk", marked->base.y, 200);
    expect_i("entry height restored for cjk", marked->base.height, 14);
}

void test_null_palette() {
    ToolkitConfig config{};
    config.width = 32;
    config.height = 32;
    Toolkit* toolkit = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(toolkit));
    SingleEdit* edit = single_edit_create();
    expect_i("null palette selection", edit->selection_color, k_no_handle);
    expect_i("null palette caret", edit->caret_color, k_no_handle);
    expect_i("null palette background", edit->base.color, k_no_handle);
    window_destroy(&edit->base);
    services_install(previous);
    toolkit_destroy(toolkit);
}

} // namespace

int main() {
    Palette palette{};
    palette.entries[1] = PaletteEntry{255, 255, 255, 0};
    ToolkitConfig config{};
    config.width = 400;
    config.height = 240;
    config.palette = &palette;
    Toolkit* toolkit = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(toolkit));
    modern_chat_use_engine();
    modern_chat_set(true, 80);

    const ModernChatLine* opened = modern_chat_line();
    const int32_t row = opened != nullptr ? opened->row_height : 18;
    Added added{};
    added.list = make_log(row);
    added.width = k_row_width;

    SingleEdit* marked = single_edit_create();
    window_set_bounds(&marked->base, 10, 200, 220, 14, nullptr, true);
    SingleEdit* plain = single_edit_create();
    window_set_bounds(&plain->base, 10, 80, 220, 14, nullptr, true);

    test_at_80(&added, marked, plain, toolkit_surface(toolkit));
    const int32_t default_row = opened != nullptr ? opened->row_height : 0;
    const int32_t default_entry = opened != nullptr ? opened->entry_height : 0;
    test_at_150(&added, marked, default_row, default_entry);
    test_off(&added, marked);
    test_null_palette();

    window_destroy(&plain->base);
    window_destroy(&marked->base);
    window_destroy(&added.list->base);
    modern_chat_set_faces(nullptr);
    services_install(previous);
    toolkit_destroy(toolkit);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("boneyards modern chat tests passed");
    return 0;
}
