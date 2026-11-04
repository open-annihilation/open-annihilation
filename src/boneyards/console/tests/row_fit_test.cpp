// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rows grow where a glyph is taller than the font's line, and stay put for
// English. The 14-pixel entry line, the planet summary's labels, Recon's
// field boxes and a message box's lines are drawn and measured.
#include "console_state.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/utf8.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int failures{};

// 東京, Ковальчук, КОВА, Кру. Planet names hold 16 bytes, so the short forms
// are the ones the summary and Recon can show.
constexpr const char* k_hello = "Hello";
constexpr const char* k_tokyo = "東京";
constexpr const char* k_koval = "Ковальчук";
constexpr const char* k_kova = "КОВА";
constexpr const char* k_kru = "Кру";
constexpr const char* k_east = "東";
constexpr int32_t k_screen_w = 640;
constexpr int32_t k_screen_h = 480;
constexpr int32_t k_entry_line = 0xe;
constexpr int32_t k_entry_grown = 0x11;
constexpr int32_t k_entry_lift = 2;
constexpr int32_t k_label_line = 9;
constexpr int32_t k_field_line = 0xa;
constexpr int32_t k_list_row = 0x11;

void check(bool result, const char* text) {
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++failures;
    }
}

void check_at(bool result, const char* text, int32_t a, int32_t b) {
    if (!result) {
        std::fprintf(stderr, "FAIL: %s (%d, %d)\n", text, a, b);
        ++failures;
    }
}

struct Fixture {
    oa::AssetStore assets{oa::boneyards_test::by_install()};
    oa::Palette palette{};
};

bool read_asset(void* context, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Fixture*>(context)->assets.read(path).bytes;
        return true;
    } catch (...) {
        return false;
    }
}

void click(StatusWidget* widget, const Window* window) {
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

const char* text_of(const Window* label) {
    if (label->states.count > 1 && label->states.texts != nullptr && label->states.texts[1] >= 0) {
        return text_get(label->states.texts[1]);
    }
    return "";
}

screens::PlanetRecord planet(int32_t id, const char* name, int32_t x, char side) {
    screens::PlanetRecord r{};
    r.id = id;
    r.name = name;
    r.x = x;
    r.y = 100;
    r.sequence = "Planet";
    r.side = side;
    r.ta_map = "Coast to Coast";
    r.ta_mapsource = "TA";
    r.ta_maptype = "Green";
    return r;
}

struct Ink {
    int32_t left = k_screen_w;
    int32_t top = k_screen_h;
    int32_t right = -1;
    int32_t bottom = -1;
    int32_t count = 0;

    bool empty() const { return count == 0; }

    int32_t span() const { return empty() ? 0 : bottom - top + 1; }
};

bool inside(const Ink& ink, int32_t x, int32_t y, int32_t width, int32_t height) {
    return !ink.empty() && ink.left >= x && ink.top >= y && ink.right < x + width &&
           ink.bottom < y + height;
}

Ink draw_text(
    StatusWidget* widget,
    Handle font,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char* text,
    int32_t style
) {
    std::vector<uint8_t> pixels(
        static_cast<std::size_t>(k_screen_w * k_screen_h), k_overlay_transparent
    );
    oa::Surface surface{};
    surface.width = k_screen_w;
    surface.height = k_screen_h;
    surface.pitch = k_screen_w;
    surface.pixels = pixels.data();
    surface.clip = oa::Rect32{0, 0, k_screen_w - 1, k_screen_h - 1};
    toolkit_set_target(widget->toolkit, &surface);
    text_draw_string(font, x, y, width, height, text, style);
    toolkit_set_target(widget->toolkit, nullptr);
    Ink ink{};
    for (int32_t row = 0; row < k_screen_h; ++row) {
        for (int32_t col = 0; col < k_screen_w; ++col) {
            if (pixels[static_cast<std::size_t>(row * k_screen_w + col)] == k_overlay_transparent) {
                continue;
            }
            ++ink.count;
            if (col < ink.left) {
                ink.left = col;
            }
            if (col > ink.right) {
                ink.right = col;
            }
            if (row < ink.top) {
                ink.top = row;
            }
            if (row > ink.bottom) {
                ink.bottom = row;
            }
        }
    }
    return ink;
}

void check_ink(const char* what, const Ink& ink, const Window* box) {
    check(inside(ink, box->x, box->y, box->width, box->height), what);
    if (!inside(ink, box->x, box->y, box->width, box->height)) {
        std::fprintf(
            stderr,
            "  ink %d..%d, %d..%d (%d) box %d,%d %dx%d\n",
            ink.left,
            ink.right,
            ink.top,
            ink.bottom,
            ink.count,
            box->x,
            box->y,
            box->width,
            box->height
        );
    }
}

TextFit fit_of(StatusWidget* widget, Handle font, const char* text) {
    TextFit fit{};
    toolkit_text_fit(widget->toolkit, font, text, &fit);
    return fit;
}

bool spills(const TextFit& fit) {
    return fit.above > 0 || fit.below > 0;
}

std::string repeated(const char* line, int32_t count) {
    std::string text;
    for (int32_t i = 0; i < count; ++i) {
        text += line;
        if (i + 1 < count) {
            text += '\n';
        }
    }
    return text;
}

void check_entry(StatusWidget* widget, MainFrame* frame) {
    Window* console = chat_console_create(
        widget,
        &frame->base,
        main_frame_layout::console_x,
        main_frame_layout::console_y,
        main_frame_layout::console_width,
        main_frame_layout::console_height
    );
    check(console != nullptr, "chat console built");
    if (console == nullptr) {
        return;
    }
    auto* view = reinterpret_cast<ChatView*>(console);
    SingleEdit* input = view->input;
    check(input != nullptr, "chat entry built");
    if (input == nullptr) {
        return;
    }
    const int32_t rest = input->base.y;
    check(
        input->base.height == k_entry_line && input->selection_height == k_entry_line,
        "an empty entry is the 14-pixel line"
    );

    single_edit_set_text(input, k_hello);
    check(
        input->base.y == rest && input->base.height == k_entry_line &&
            input->selection_height == k_entry_line,
        "English leaves the entry line where it was"
    );
    const TextFit hello = fit_of(widget, input->base.font, k_hello);
    check(!spills(hello), "English fits the entry font's line");
    Ink hello_ink = draw_text(
        widget,
        input->base.font,
        input->padding + input->base.x,
        input->base.y,
        input->base.width - input->padding * 2,
        input->base.height,
        single_edit_text(input),
        text_style::left
    );
    check_ink("English sits inside the entry line", hello_ink, &input->base);
    check(
        hello_ink.top == input->base.y + (k_entry_line - hello.line_height) / 2 &&
            hello_ink.span() == hello.line_height,
        "English ink fills the font's line and nothing past it"
    );

    single_edit_set_text(input, k_tokyo);
    const TextFit tokyo = fit_of(widget, input->base.font, k_tokyo);
    check(spills(tokyo), "an ideograph passes the entry font's line");
    check(
        input->base.y == rest - k_entry_lift && input->base.height == k_entry_grown &&
            input->selection_height == k_entry_grown,
        "an ideograph lifts the entry line to 17 pixels"
    );
    check_ink(
        "the ideograph sits inside the grown entry",
        draw_text(
            widget,
            input->base.font,
            input->padding + input->base.x,
            input->base.y,
            input->base.width - input->padding * 2,
            input->base.height,
            single_edit_text(input),
            text_style::left
        ),
        &input->base
    );

    single_edit_set_text(input, k_koval);
    const TextFit koval = fit_of(widget, input->base.font, k_koval);
    check(spills(koval), "a descending Cyrillic letter passes the entry font's line");
    check(
        input->base.y == rest - k_entry_lift && input->base.height == k_entry_grown,
        "Cyrillic with a descender lifts the entry line to 17 pixels"
    );
    check_ink(
        "the Cyrillic entry sits inside the grown line",
        draw_text(
            widget,
            input->base.font,
            input->padding + input->base.x,
            input->base.y,
            input->base.width - input->padding * 2,
            input->base.height,
            single_edit_text(input),
            text_style::left
        ),
        &input->base
    );

    single_edit_set_text(input, k_kova);
    const TextFit kova = fit_of(widget, input->base.font, k_kova);
    check(!spills(kova), "Cyrillic capitals fit the entry font's line");
    check(
        input->base.y == rest && input->base.height == k_entry_line,
        "Cyrillic capitals leave the entry line at 14 pixels"
    );

    single_edit_set_text(input, "");
    check(
        input->base.y == rest && input->base.height == k_entry_line,
        "clearing the entry puts the 14-pixel line back"
    );

    const int32_t lobby_y = frame->base.y + 40;
    SingleEdit* lobby = screens::lobby_edit_create(
        &frame->base, 40, 40, 120, widget->system.hat_fonts.small, 16, false
    );
    check(
        lobby->base.height == screens::kLobbyEditHeight && lobby->base.y == lobby_y,
        "a lobby entry is 21 pixels"
    );
    const int32_t lobby_selection = lobby->selection_height;
    single_edit_set_text(lobby, k_tokyo);
    check(
        lobby->base.height == screens::kLobbyEditHeight && lobby->base.y == lobby_y &&
            lobby->selection_height == lobby_selection,
        "a lobby entry stays 21 pixels around an ideograph"
    );
    window_destroy(&lobby->base);

    screens::ByListBox* list = screens::by_list_box_create(false, false);
    window_set_bounds(
        &list->list.base, frame->base.x + 8, frame->base.y + 8, 50, 80, &frame->base, true
    );
    const screens::HatFonts& fonts = widget->system.hat_fonts;
    screens::by_list_wrap_text(list, "Hello Hello Hello", &fonts, 3, fonts.small);
    check(list->list.count == 3, "a narrow list breaks English on spaces");
    for (int32_t i = 0; i < list->list.count; ++i) {
        Window* row = list_box_item(&list->list, i);
        check(row != nullptr && row->height == k_list_row, "an English list row stays 17 pixels");
        if (row != nullptr && row->ops != nullptr && row->ops->state_text != nullptr) {
            check(
                std::strcmp(row->ops->state_text(row, -1), k_hello) == 0,
                "English breaks between words"
            );
        }
    }
    screens::by_list_box_clear(list);
    std::string ideographs;
    for (int32_t i = 0; i < 12; ++i) {
        ideographs += k_east;
    }
    screens::by_list_wrap_text(list, ideographs.c_str(), &fonts, 3, fonts.small);
    check(list->list.count > 1, "a run of ideographs breaks across list rows");
    for (int32_t i = 0; i < list->list.count; ++i) {
        Window* row = list_box_item(&list->list, i);
        check(row != nullptr && row->height == k_list_row, "a CJK list row stays 17 pixels");
    }
    window_destroy(&list->list.base);

    char broken[64]{};
    const int32_t lines = toolkit_wrap_text(
        widget->toolkit,
        30,
        "Hello Hello Hello",
        broken,
        static_cast<int32_t>(sizeof broken),
        fonts.small
    );
    check(lines == 3 && std::strcmp(broken, "Hello\nHello\nHello") == 0, "English wraps on spaces");
    char joined[64]{};
    const int32_t cjk_lines = toolkit_wrap_text(
        widget->toolkit,
        30,
        ideographs.c_str(),
        joined,
        static_cast<int32_t>(sizeof joined),
        fonts.small
    );
    check(
        cjk_lines > 1 && std::strchr(joined, '\n') != nullptr, "a line breaks between ideographs"
    );
    std::string flat;
    for (const char* p = joined; *p != '\0'; ++p) {
        if (*p != '\n') {
            flat.push_back(*p);
        }
    }
    check(flat == ideographs, "breaking between ideographs keeps every character");
}

void check_summary(StatusWidget* widget, BattleSummary* summary, screens::GalaxyPlanet* body) {
    const Window& frame = summary->frame->base;
    const int32_t name_y = frame.y + 0x80;
    const int32_t gap_owner = 4;
    const int32_t gap_battles = 8;
    const int32_t gap_forming = 4;
    battle_summary_update(summary);
    check(
        summary->name->y == name_y && summary->name->height == k_label_line &&
            summary->name->x == frame.x + 0x211,
        "an English planet name stays on its 9-pixel line"
    );
    check(
        summary->owner->y == summary->name->y + summary->name->height + gap_owner &&
            summary->owner->height == k_label_line,
        "the owner stays one gap under the English name"
    );
    check(
        summary->battles_caption->y == summary->owner->y + summary->owner->height + gap_battles &&
            summary->battles_caption->height == k_label_line &&
            summary->battles->height == k_label_line,
        "the battles row stays on the font's line"
    );
    check(
        summary->forming_caption->y ==
                summary->battles_caption->y + summary->battles_caption->height + gap_forming &&
            summary->forming_caption->height == k_label_line,
        "the forming row stays one gap under the battles row"
    );
    check(
        summary->arm_icon->y == summary->battles_caption->y - 1 &&
            summary->arm_icon->width == 0xb && summary->arm_icon->height == 0xb &&
            summary->core_icon->y == summary->forming_caption->y - 1,
        "the side icons stay a pixel above their rows"
    );
    const int32_t name_style = reinterpret_cast<screens::LobbyLabel*>(summary->name)->style;
    // The face's own letters, including the descenders in "Empyrrean", already
    // paint past the 9-pixel line. The line itself does not move.
    const Ink english_name = draw_text(
        widget,
        summary->name->font,
        summary->name->x,
        summary->name->y,
        summary->name->width,
        summary->name->height,
        text_of(summary->name),
        name_style
    );
    check(
        english_name.count > 0 && english_name.top == summary->name->y &&
            english_name.left >= summary->name->x &&
            english_name.right < summary->name->x + summary->name->width,
        "the English name is drawn on its line"
    );

    utf8::copy(body->name, sizeof body->name, k_tokyo, 0);
    battle_summary_update(summary);
    const TextFit tokyo = fit_of(widget, summary->name->font, text_of(summary->name));
    check(
        spills(tokyo) && summary->name->height > k_label_line, "an ideograph grows the planet name"
    );
    check(
        summary->name->x == frame.x + 0x211 && summary->name->y == name_y,
        "the name stays at the top of the stack"
    );
    check(
        summary->owner->height == k_label_line &&
            summary->owner->y == summary->name->y + summary->name->height + gap_owner,
        "the owner keeps the gap under the grown name"
    );
    check(
        summary->battles_caption->height == k_label_line &&
            summary->battles->height == k_label_line &&
            summary->battles_caption->y ==
                summary->owner->y + summary->owner->height + gap_battles &&
            summary->battles->y == summary->battles_caption->y,
        "the battles caption and count stay on the font's line"
    );
    const int32_t battles_row = summary->battles_caption->height;
    check(
        summary->forming_caption->y == summary->battles_caption->y + battles_row + gap_forming &&
            summary->forming_caption->height == k_label_line &&
            summary->arm_icon->y == summary->battles_caption->y - 1 &&
            summary->core_icon->y == summary->forming_caption->y - 1 &&
            summary->arm_icon->height == 0xb,
        "the rows below keep their gaps and the icons stay put beside them"
    );
    check_ink(
        "the ideograph sits inside the grown name",
        draw_text(
            widget,
            summary->name->font,
            summary->name->x,
            summary->name->y,
            summary->name->width,
            summary->name->height,
            text_of(summary->name),
            name_style
        ),
        summary->name
    );

    utf8::copy(body->name, sizeof body->name, k_kru, 0);
    battle_summary_update(summary);
    const TextFit kru = fit_of(widget, summary->name->font, text_of(summary->name));
    check(
        spills(kru) && summary->name->height > k_label_line,
        "a descending Cyrillic name grows the label"
    );
    check(
        summary->owner->y == summary->name->y + summary->name->height + gap_owner,
        "the owner keeps the gap under the Cyrillic name"
    );
    check_ink(
        "the Cyrillic name sits inside its label",
        draw_text(
            widget,
            summary->name->font,
            summary->name->x,
            summary->name->y,
            summary->name->width,
            summary->name->height,
            text_of(summary->name),
            name_style
        ),
        summary->name
    );

    utf8::copy(body->name, sizeof body->name, "Empyrrean", 0);
    battle_summary_update(summary);
    check(
        summary->name->height == k_label_line && summary->name->y == name_y,
        "English stacks back to the 9-pixel lines"
    );
}

int32_t row_height(StatusWidget* widget, const screens::LobbyField* field) {
    const Window* value = field != nullptr ? field->value : nullptr;
    if (value == nullptr) {
        return k_field_line;
    }
    const TextFit fit = fit_of(widget, value->font, text_of(value));
    const int32_t over = fit.above > fit.below ? fit.above : fit.below;
    int32_t height = fit.line_height + over * 2;
    if (height < k_field_line) {
        height = k_field_line;
    }
    return height;
}

void check_recon(StatusWidget* widget, MainFrame* frame, screens::GalaxyPlanet* body) {
    Window* screen = recon_console_create(
        widget,
        &frame->base,
        main_frame_layout::console_x,
        main_frame_layout::console_y,
        main_frame_layout::console_width,
        main_frame_layout::console_height
    );
    check(screen != nullptr, "recon console built");
    if (screen == nullptr) {
        return;
    }
    auto* console = reinterpret_cast<ReconConsole*>(screen);
    recon_console_open(screen);
    const int32_t top = screen->y;
    const int32_t list_bottom = top + 0xb4 + 0x7e;
    for (std::size_t i = 0; i < screens::kPlanetInfoRowCount; ++i) {
        const Window& field = console->fields[i]->base;
        check(field.height == k_field_line, "an English recon field stays 10 pixels");
    }
    const Window& list = console->text->list.base;
    check(
        list.y == top + 0xb4 && list.height == 0x7e && list.y + list.height == list_bottom,
        "the English recon list stays under the fields"
    );
    const Window* english = console->fields[screens::planet_info_row::planet]->value;
    // "Empyrrean" uses the face's own descenders, which already hang past the
    // 10-pixel field. The field stays put.
    const Ink english_ink = draw_text(
        widget,
        english->font,
        english->x,
        english->y,
        english->width,
        english->height,
        text_of(english),
        text_style::left
    );
    check(
        english_ink.count > 0 && english_ink.top == english->y && english_ink.left >= english->x &&
            english_ink.right < english->x + english->width,
        "the English planet field is drawn in its box"
    );
    recon_console_close(screen);

    utf8::copy(body->name, sizeof body->name, k_kru, 0);
    recon_console_open(screen);
    const screens::LobbyField* planet_field = console->fields[screens::planet_info_row::planet];
    const screens::LobbyField* map_field = console->fields[screens::planet_info_row::gw_map];
    const int32_t upper = row_height(widget, planet_field);
    check(upper > k_field_line, "a descending Cyrillic value grows the recon row");
    check(
        planet_field->base.height == upper &&
            console->fields[screens::planet_info_row::type]->base.height == upper &&
            console->fields[screens::planet_info_row::wind_speed]->base.height == upper &&
            console->fields[screens::planet_info_row::affiliation]->base.height == upper,
        "the whole upper row takes the tall value's height"
    );
    check(
        map_field->base.y == planet_field->base.y + upper + 7 &&
            map_field->base.height == k_field_line,
        "the lower row keeps the gap under the grown row"
    );
    check(
        console->text->list.base.y + console->text->list.base.height == list_bottom,
        "the recon list's bottom stays put"
    );
    const Window* value = planet_field->value;
    check_ink(
        "the Cyrillic recon value sits inside its field",
        draw_text(
            widget,
            value->font,
            value->x,
            value->y,
            value->width,
            value->height,
            text_of(value),
            text_style::left
        ),
        value
    );

    utf8::copy(body->name, sizeof body->name, k_tokyo, 0);
    recon_console_close(screen);
    recon_console_open(screen);
    const int32_t cjk = row_height(widget, console->fields[screens::planet_info_row::planet]);
    check(
        cjk > k_field_line && console->fields[screens::planet_info_row::planet]->base.height == cjk,
        "an ideograph grows the recon row"
    );
    value = console->fields[screens::planet_info_row::planet]->value;
    check_ink(
        "the ideograph sits inside the recon field",
        draw_text(
            widget,
            value->font,
            value->x,
            value->y,
            value->width,
            value->height,
            text_of(value),
            text_style::left
        ),
        value
    );
    const int32_t grown = console->fields[screens::planet_info_row::planet]->base.height;
    body->population = 34;
    CensusReport report{};
    report.area_id = static_cast<uint32_t>(body->id);
    recon_console_on_census(screen, &report);
    check(
        console->fields[screens::planet_info_row::planet]->base.height == grown &&
            console->text->list.base.y + console->text->list.base.height == list_bottom,
        "a census reply keeps the grown recon row and the list's bottom"
    );
    recon_console_close(screen);
}

void check_box_geometry(const char* what, MessageBox* box, int32_t extra) {
    const Window* label = window_find_named(&box->base, message_box_name::text, nullptr);
    const Window* ok = window_find_named(&box->base, message_box_name::ok, class_name::button);
    check(label != nullptr && ok != nullptr, what);
    if (label == nullptr || ok == nullptr) {
        return;
    }
    const int32_t text_extra = label->height - message_box_layout::text_height;
    const int32_t box_extra = box->base.height - message_box_layout::height;
    const int32_t button_extra = ok->y - (box->base.y + message_box_layout::button_dy);
    check_at(
        text_extra == extra && box_extra == extra && button_extra == extra,
        what,
        text_extra,
        box_extra
    );
}

// Pixels of the box picture that is drawn for the current state.
struct FacePixels {
    int32_t width = 0;
    int32_t height = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t key = 0;
    std::vector<uint8_t> pixels;
};

FacePixels face_of(const MessageBox* box) {
    FacePixels face;
    const ImageService* images = services().images;
    if (box == nullptr || images == nullptr || images->sprite == nullptr ||
        box->base.states.images == nullptr) {
        return face;
    }
    const int32_t state = box->base.states.current;
    if (state < 0 || state >= box->base.states.count) {
        return face;
    }
    const Sprite* sprite = images->sprite(images->self, box->base.states.images[state]);
    if (sprite == nullptr || sprite->data == nullptr || sprite->encoding != OA_SPRITE_RAW ||
        sprite->width <= 0 || sprite->height <= 0) {
        return face;
    }
    face.width = sprite->width;
    face.height = sprite->height;
    face.origin_x = sprite->origin_x;
    face.origin_y = sprite->origin_y;
    face.key = sprite->key;
    const auto* pixels = static_cast<const uint8_t*>(sprite->data);
    face.pixels.assign(
        pixels,
        pixels + static_cast<std::size_t>(face.width) * static_cast<std::size_t>(face.height)
    );
    return face;
}

bool same_rows(
    const FacePixels& source,
    int32_t source_row,
    const FacePixels& grown,
    int32_t grown_row,
    int32_t rows
) {
    if (source.width <= 0 || source.width != grown.width || rows <= 0 || source_row < 0 ||
        grown_row < 0 || source_row + rows > source.height || grown_row + rows > grown.height) {
        return false;
    }
    return std::memcmp(
               source.pixels.data() + static_cast<std::size_t>(source_row) * source.width,
               grown.pixels.data() + static_cast<std::size_t>(grown_row) * grown.width,
               static_cast<std::size_t>(source.width) * static_cast<std::size_t>(rows)
           ) == 0;
}

// Screen bounds of the pixels the picture actually draws. Inclusive.
Ink drawn_bounds(const MessageBox* box, const FacePixels& face) {
    Ink bounds{};
    const int32_t origin_x = box->base.x - face.origin_x;
    const int32_t origin_y = box->base.y - face.origin_y;
    for (int32_t row = 0; row < face.height; ++row) {
        for (int32_t col = 0; col < face.width; ++col) {
            if (face.pixels[static_cast<std::size_t>(row * face.width + col)] ==
                static_cast<uint8_t>(face.key)) {
                continue;
            }
            const int32_t x = origin_x + col;
            const int32_t y = origin_y + row;
            ++bounds.count;
            if (x < bounds.left) {
                bounds.left = x;
            }
            if (x > bounds.right) {
                bounds.right = x;
            }
            if (y < bounds.top) {
                bounds.top = y;
            }
            if (y > bounds.bottom) {
                bounds.bottom = y;
            }
        }
    }
    return bounds;
}

// The button's picture, its window and the one-pixel lift of a raised caption.
void button_extent(
    const Window* button, int32_t* left, int32_t* top, int32_t* right, int32_t* bottom
) {
    *left = button->x;
    *top = button->y - 1;
    *right = button->x + button->width - 1;
    *bottom = button->y + button->height - 1;
    const ImageService* images = services().images;
    if (images == nullptr || images->sprite == nullptr || button->states.images == nullptr) {
        return;
    }
    const int32_t state = button->states.current;
    if (state < 0 || state >= button->states.count) {
        return;
    }
    const Sprite* sprite = images->sprite(images->self, button->states.images[state]);
    if (sprite == nullptr || sprite->width <= 0 || sprite->height <= 0) {
        return;
    }
    const int32_t x0 = button->x - sprite->origin_x;
    const int32_t y0 = button->y - sprite->origin_y;
    const int32_t x1 = x0 + sprite->width - 1;
    const int32_t y1 = y0 + sprite->height - 1;
    if (x0 < *left) {
        *left = x0;
    }
    if (y0 < *top) {
        *top = y0;
    }
    if (x1 > *right) {
        *right = x1;
    }
    if (y1 > *bottom) {
        *bottom = y1;
    }
}

bool rect_inside(int32_t left, int32_t top, int32_t right, int32_t bottom, const Ink& bounds) {
    return bounds.count > 0 && left >= bounds.left && top >= bounds.top && right <= bounds.right &&
           bottom <= bounds.bottom;
}

void check_grown_picture(
    const char* what,
    StatusWidget* widget,
    const FacePixels& english,
    MessageBox* box,
    int32_t extra
) {
    const FacePixels grown = face_of(box);
    const int32_t top = message_box_layout::text_dy;
    const int32_t bar = message_box_layout::button_dy;
    const int32_t edge = bar + message_box_layout::button_height;
    const int32_t panel = bar - top;
    const int32_t band = panel / 2 > 0 ? panel / 2 : panel;
    const int32_t band_at = top + (panel - band) / 2;
    check_at(
        grown.width == english.width && grown.height == english.height + extra &&
            grown.origin_x == english.origin_x && grown.origin_y == english.origin_y,
        what,
        grown.width,
        grown.height
    );
    check(same_rows(english, 0, grown, 0, bar), "the grown box keeps the top of its picture");
    check(
        english.height > edge && same_rows(english, bar, grown, bar + extra, english.height - bar),
        "the grown box keeps the buttons' bar and the bottom edge"
    );
    bool repeated = extra > 0 && band > 0 && grown.width == english.width;
    for (int32_t row = 0; repeated && row < extra; ++row) {
        repeated = same_rows(english, band_at + (row % band), grown, bar + row, 1);
    }
    check(repeated, "the grown box repeats a middle band of its picture");

    const Ink drawn = drawn_bounds(box, grown);
    check(drawn.count > 0, "the grown box draws its picture");
    int32_t buttons = 0;
    int32_t lowest = box->base.y;
    for (int32_t i = 0; i < box->base.children.count; ++i) {
        const Window* child = box->base.children.items[i];
        if (child == nullptr || child->ops == nullptr || child->ops->is_class == nullptr ||
            !child->ops->is_class(class_name::button)) {
            continue;
        }
        int32_t left = 0;
        int32_t top_px = 0;
        int32_t right = 0;
        int32_t bottom = 0;
        button_extent(child, &left, &top_px, &right, &bottom);
        check(
            rect_inside(left, top_px, right, bottom, drawn), "a button lies inside the drawn box"
        );
        if (!rect_inside(left, top_px, right, bottom, drawn)) {
            std::fprintf(
                stderr,
                "  button %d..%d, %d..%d drawn %d..%d, %d..%d\n",
                left,
                right,
                top_px,
                bottom,
                drawn.left,
                drawn.right,
                drawn.top,
                drawn.bottom
            );
        }
        if (bottom > lowest) {
            lowest = bottom;
        }
        ++buttons;
    }
    check(buttons > 0, "the grown box has a button");
    const int32_t edge_y = box->base.y - grown.origin_y + edge + extra;
    check_at(edge_y > lowest, "the bottom edge is drawn below the buttons", edge_y, lowest);

    const Window* label = window_find_named(&box->base, message_box_name::text, nullptr);
    check(label != nullptr, "the grown box has its text");
    if (label == nullptr) {
        return;
    }
    const Ink ink = draw_text(
        widget,
        label->font,
        label->x,
        label->y,
        label->width,
        label->height,
        text_of(label),
        text_style::wrap
    );
    const bool text_inside = !ink.empty() && ink.left >= drawn.left && ink.top >= drawn.top &&
                             ink.right <= drawn.right && ink.bottom <= drawn.bottom;
    check(text_inside, "every text line lies inside the drawn box");
    if (!text_inside) {
        std::fprintf(
            stderr,
            "  text %d..%d, %d..%d drawn %d..%d, %d..%d\n",
            ink.left,
            ink.right,
            ink.top,
            ink.bottom,
            drawn.left,
            drawn.right,
            drawn.top,
            drawn.bottom
        );
    }
}

void check_message(StatusWidget* widget) {
    MessageBox* hello =
        message_box_create(k_hello, message_box_buttons::ok, nullptr, nullptr, nullptr);
    const FacePixels english = face_of(hello);
    check(
        english.height > message_box_layout::button_dy + message_box_layout::button_height,
        "the message picture reaches below its buttons"
    );
    check_box_geometry("English leaves the message box where it was", hello, 0);
    const Window* hello_label = window_find_named(&hello->base, message_box_name::text, nullptr);
    const TextFit hello_fit = fit_of(widget, hello_label->font, k_hello);
    check(!spills(hello_fit), "English fits the message font's line");
    Ink hello_ink = draw_text(
        widget,
        hello_label->font,
        hello_label->x,
        hello_label->y,
        hello_label->width,
        hello_label->height,
        text_of(hello_label),
        text_style::wrap
    );
    check_ink("English sits inside the message", hello_ink, hello_label);
    window_destroy(&hello->base);

    const std::string prose = repeated("Hello ", 40);
    MessageBox* long_english =
        message_box_create(prose.c_str(), message_box_buttons::ok, nullptr, nullptr, nullptr);
    check_box_geometry("a long English message keeps the box", long_english, 0);
    const FacePixels long_face = face_of(long_english);
    check(
        long_face.width == english.width && long_face.height == english.height &&
            long_face.pixels == english.pixels,
        "a long English message keeps the picture"
    );
    window_destroy(&long_english->base);

    MessageBox* one =
        message_box_create(k_tokyo, message_box_buttons::ok, nullptr, nullptr, nullptr);
    const Window* one_label = window_find_named(&one->base, message_box_name::text, nullptr);
    const TextFit one_fit = fit_of(widget, one_label->font, k_tokyo);
    check(spills(one_fit), "an ideograph passes the message font's line");
    check_box_geometry("one tall line still fits the message area", one, 0);
    const FacePixels one_face = face_of(one);
    check(
        one_face.width == english.width && one_face.height == english.height,
        "one tall line leaves the picture at its old size"
    );
    Ink one_ink = draw_text(
        widget,
        one_label->font,
        one_label->x,
        one_label->y,
        one_label->width,
        one_label->height,
        text_of(one_label),
        text_style::wrap
    );
    check_ink("one ideograph sits inside the message", one_ink, one_label);
    check(one_ink.span() > hello_ink.span(), "the ideograph draws taller than English");
    window_destroy(&one->base);

    const std::string cjk = repeated(k_tokyo, 12);
    MessageBox* tall =
        message_box_create(cjk.c_str(), message_box_buttons::ok, nullptr, nullptr, nullptr);
    const Window* tall_label = window_find_named(&tall->base, message_box_name::text, nullptr);
    const int32_t extra = tall_label->height - message_box_layout::text_height;
    check(extra > 0, "ideographs past the message area lengthen the box");
    check_box_geometry("the message, the box and the button grow together", tall, extra);
    check_grown_picture(
        "ideographs lengthen the picture with the box", widget, english, tall, extra
    );
    check_ink(
        "the ideographs sit inside the grown message",
        draw_text(
            widget,
            tall_label->font,
            tall_label->x,
            tall_label->y,
            tall_label->width,
            tall_label->height,
            text_of(tall_label),
            text_style::wrap
        ),
        tall_label
    );
    window_destroy(&tall->base);

    const std::string cyrillic = repeated(k_koval, 12);
    MessageBox* descender =
        message_box_create(cyrillic.c_str(), message_box_buttons::ok, nullptr, nullptr, nullptr);
    const Window* descender_label =
        window_find_named(&descender->base, message_box_name::text, nullptr);
    const int32_t cyr_extra = descender_label->height - message_box_layout::text_height;
    check(cyr_extra > 0, "Cyrillic past the message area lengthens the box");
    check_box_geometry(
        "Cyrillic grows the message, the box and the button together", descender, cyr_extra
    );
    check_grown_picture(
        "Cyrillic lengthens the picture with the box", widget, english, descender, cyr_extra
    );
    check_ink(
        "the Cyrillic message sits inside the grown box",
        draw_text(
            widget,
            descender_label->font,
            descender_label->x,
            descender_label->y,
            descender_label->width,
            descender_label->height,
            text_of(descender_label),
            text_style::wrap
        ),
        descender_label
    );
    window_destroy(&descender->base);
}

void test_rows(Fixture* fixture) {
    StatusWidgetConfig config{};
    config.files = {fixture, read_asset};
    config.palette = &fixture->palette;
    config.service.network_available = [](void*) { return true; };
    config.service.activate = [](void*) { return ActivationResult::ok; };
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget created");
    if (widget == nullptr) {
        return;
    }
    click(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    check(widget->analogues != nullptr, "analogue glyphs are loaded");
    if (widget->analogues == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    MainFrame* frame = widget->system.main_frame;
    check(frame != nullptr && frame->battle_summary != nullptr, "the Main Frame is open");
    if (frame == nullptr || frame->battle_summary == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }

    ScopedServices scope(widget);
    screens::GalaxyView& galaxy = widget->navigation->galaxy;
    const screens::PlanetRecord planets[2] = {
        planet(1, "Core Prime", 10, 'C'), planet(2, "Empyrrean", 100, 'A')
    };
    const screens::ConnectorRecord link{1, 2};
    const screens::SpriteFrame frames[2] = {{16, 16, 8, 8}, {16, 16, 8, 8}};
    const std::size_t counts[2] = {1, 1};
    const screens::PlanetSprites sprites{frames, counts};
    check(
        screens::galaxy_load(&galaxy, 2400, 1600, planets, 2, &link, 1, &sprites), "war map loaded"
    );
    galaxy.locked = false;
    uint32_t area{};
    check(screens::galaxy_select_planet(&galaxy, 1, &area), "planet selected");

    check_entry(widget, frame);
    check_summary(widget, frame->battle_summary, &galaxy.planets[1]);
    check_recon(widget, frame, &galaxy.planets[1]);
    check_message(widget);
    boneyards_status_widget_destroy(widget);
}

} // namespace

int main() {
    // The atlas is the one copied into the game's fonts folder. The environment
    // override stays unset, as it is for an installed game.
#ifdef _WIN32
    _putenv_s("OA_FONT_ATLASES_DIR", "");
#else
    unsetenv("OA_FONT_ATLASES_DIR");
#endif
#ifndef OA_BUNDLED_ATLAS
    std::fprintf(stderr, "FAIL: the bundled atlas path is not compiled in\n");
    return 1;
#else
    if (!std::filesystem::is_regular_file(OA_BUNDLED_ATLAS)) {
        std::fprintf(stderr, "FAIL: the game does not ship %s\n", OA_BUNDLED_ATLAS);
        return 1;
    }
    boneyards_status_widget_set_analogue_atlas_path(OA_BUNDLED_ATLAS);
#endif
    const std::filesystem::path root = oa::boneyards_test::require_by_install("the row fit tests");
    Fixture fixture;
    fixture.assets.mount(root / "by.ccx");
    fixture.assets.mount(root / "totala1.hpi");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int32_t i = 0; i < 256; ++i) {
        fixture.palette.entries[i] = {raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    }
    test_rows(&fixture);
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("row fit tests passed\n");
    return 0;
}
