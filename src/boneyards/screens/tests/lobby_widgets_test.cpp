// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/lobby_widgets.hpp"

#include "test_data.hpp"
#include "check.hpp"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

void test_layout(Toolkit*) {
    Window* parent = window_create_plain();
    window_create(parent, "Panel", 100, 50, 300, 200, nullptr, true);
    Window* large = window_create_plain();
    function_button_setup(large, false, 12, 0x105, "APPLY", parent);
    CHECK(large->x == 112 && large->y == 50 + 0x105);
    CHECK(large->width == 99 && large->height == 21);
    CHECK(large->states.count == 4 && large->states.current == 0);
    CHECK(large->parent == parent && large->visible);
    for (int32_t s = 0; s < 4; ++s)
        CHECK_STR(text_get(large->states.texts[s]), "APPLY");
    Window* small = window_create_plain();
    function_button_setup(small, true, 0, 0, "HOST", parent);
    CHECK(small->width == 57);

    // A label keeps the caption style its alignment and fixed size chose
    // for the texts set later.
    Window* label = lobby_label_create(parent, 0x11, 0x20, "", k_no_handle, 0, 0, 0x3c, 9);
    CHECK(label->x == 100 + 0x11 && label->y == 50 + 0x20);
    CHECK(label->width == 0x3c && label->height == 9 && label->parent == parent);
    CHECK(label->ops->is_class(class_name::static_text) && label->focusable == 0);
    constexpr int32_t top_left = text_style::left | text_style::top;
    CHECK(reinterpret_cast<LobbyLabel*>(label)->style == top_left);
    lobby_label_set_text(label, "Contested");
    CHECK_STR(text_get(label->states.texts[1]), "Contested");
    CHECK(reinterpret_cast<LobbyLabel*>(label)->style == top_left);
    Window* right = lobby_label_create(parent, 0x40, 0, "Username", k_no_handle, 5, 1, 0x30, 0);
    CHECK(right->x == 100 + 0x40 - 0x2f);
    CHECK(reinterpret_cast<LobbyLabel*>(right)->style == text_style::right);
    window_destroy(parent);
}

/// Draws a function button and a player row's icons from the installed game data and checks
/// their images and pixels.
///
/// @param tk toolkit configured over the installed game data
void test_installed_draw(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Panel", 0, 0, 200, 60, nullptr, true);
    Window* button = window_create_plain();
    function_button_setup(button, false, 10, 10, "", parent);
    CHECK(button->states.images[0] >= 0 && button->states.images[3] >= 0);
    CHECK(button->states.images[0] != button->states.images[1]);
    int32_t w = 0;
    int32_t h = 0;
    image_size(button->states.images[2], &w, &h);
    CHECK(w == 99 && h == 21);
    window_set_state(button, 2);
    window_draw_background(button);
    const Surface* overlay = toolkit_surface(tk);
    // The button art is a one-pixel outline; the caption is drawn inside it.
    CHECK(testing::opaque_pixels(overlay) == 228);
    CHECK(testing::surface_digest(overlay) == 0xd85c295du);
    CHECK(overlay->pixels[20 * overlay->pitch + 10] != k_overlay_transparent);
    CHECK(overlay->pixels[9 * overlay->pitch + 9] == k_overlay_transparent);
    CHECK(overlay->pixels[31 * overlay->pitch + 50] == k_overlay_transparent);
    toolkit_compose(tk, nullptr);

    PlayerRowIcons row{};
    row.user_icon = window_create_plain();
    row.latency_icon = window_create_plain();
    player_row_set_user_icon(&row, 0x17);
    player_row_set_latency(&row, 2);
    CHECK(row.user_frame == 0x17 && row.latency_frame == 2);
    CHECK(row.user_icon->states.images[0] >= 0 && row.latency_icon->states.images[0] >= 0);
    window_destroy(row.user_icon);
    window_destroy(row.latency_icon);
    window_destroy(parent);
}

void test_lobby_controls(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Panel", 0, 0, 300, 200, nullptr, true);
    HatFonts fonts{};
    fonts.small = toolkit_default_font(tk);
    fonts.small_light = fonts.small;
    const Handle tinted = toolkit_font_tint(tk, fonts.small, 0x54);

    // Rows span the list less its 0x11-pixel scroll bar.
    ByListBox* list = by_list_box_create(true, false);
    window_set_bounds(&list->list.base, 10, 10, 0x63, 0x58, parent, true);
    CHECK(by_list_row_width(&list->list.base) == 0x52);
    ByTextRow* named = by_text_row_create(list, "USA, West Coast", &fonts, true);
    CHECK(named->base.width == 0x52);
    // A row's text is its label's, or empty without a label.
    CHECK_STR(named->base.ops->state_text(&named->base, -1), "USA, West Coast");
    ByTextRow* blank = by_text_row_create(list, nullptr, &fonts, false);
    CHECK(blank->label == nullptr);
    CHECK_STR(blank->base.ops->state_text(&blank->base, -1), "");
    // The selected row's text, empty with no selection.
    CHECK_STR(by_list_selected_text(list), "");
    list->selected_item = &named->base;
    CHECK_STR(by_list_selected_text(list), "USA, West Coast");
    list->selected_item = &blank->base;
    CHECK_STR(by_list_selected_text(list), "");
    list->selected_item = nullptr;
    // The row's font slot sets its label's font.
    by_text_row_set_font(named, tinted);
    CHECK(named->label->font == tinted);
    by_text_row_set_font(blank, tinted);
    list_box_clear(&list->list);
    // Wrapping hands each row the requested font, -1 meaning the small font.
    by_list_wrap_text(list, "one two", &fonts, 3, tinted);
    CHECK(list->list.count == 1);
    CHECK(reinterpret_cast<ByTextRow*>(list_box_item(&list->list, 0))->label->font == tinted);
    by_list_wrap_text(list, "three", &fonts, 3, k_no_handle);
    CHECK(list->list.count == 2);
    CHECK(reinterpret_cast<ByTextRow*>(list_box_item(&list->list, 1))->label->font == fonts.small);

    // The lobby field's caption slot replaces its text.
    SingleEdit* edit = lobby_edit_create(parent, 0x6f, 0x44, 0x63, fonts.small, 0xf, true);
    CHECK(edit->base.ops == lobby_edit_ops());
    single_edit_set_text(edit, "hunter2");
    edit->base.ops->set_caption(&edit->base, "", 0);
    CHECK_STR(single_edit_text(edit), "");
    edit->base.ops->set_caption(&edit->base, "commander", 0);
    CHECK_STR(single_edit_text(edit), "commander");

    // A lobby radio lets go of its selection only for a listed member, and
    // lists at most five.
    LobbyRadio* member = lobby_radio_create(parent, 0x6f, 0x5c, "Registered User", fonts.small);
    LobbyRadio* guest = lobby_radio_create(parent, 0x6f, 0x6d, "Guest Login", fonts.small);
    CHECK(window_is_named(&member->base, "Registered User"));
    CHECK(member->base.states.count == 3 && member->base.states.current == 2);
    Window* stranger = radio_button_create();
    window_create(stranger, "Stranger", 0, 0, 10, 10, parent, true);
    lobby_radio_add_member(member, &guest->base);
    lobby_radio_add_member(guest, &member->base);
    CHECK(member->member_count == 1 && member->members[0] == &guest->base);
    window_set_state(&member->base, 1);
    Notify notify{NotifyCode::radio_selected, stranger, 0, nullptr};
    member->base.ops->on_notify(&member->base, &notify);
    CHECK(member->base.states.current == 1);
    notify.code = NotifyCode::activated;
    notify.source = &guest->base;
    member->base.ops->on_notify(&member->base, &notify);
    CHECK(member->base.states.current == 1);
    notify.code = NotifyCode::radio_selected;
    member->base.ops->on_notify(&member->base, &notify);
    CHECK(member->base.states.current == 2);
    parent->visible = 0;
    window_set_state(&member->base, 1);
    member->base.ops->on_notify(&member->base, &notify);
    CHECK(member->base.states.current == 1);
    for (int32_t i = 0; i < kLobbyRadioMembers + 2; ++i)
        lobby_radio_add_member(guest, stranger);
    CHECK(guest->member_count == kLobbyRadioMembers);
    window_destroy(parent);

    // The player pane's check boxes, sized to their captions
    // (0xb wider, at least 9 high), two pixels between box and caption, the
    // five Check_Box frames and unchecked.
    Window* pane = window_create_plain();
    window_create(pane, "Pane", 0x93, 0x7e, 0x176, 0x9d, nullptr, true);
    const Handle dim = toolkit_font_tint(tk, fonts.small, 0x5a);
    CheckBox* lock = lobby_check_box_create(pane, 5, 0x63, "Whisper Lock", dim);
    int32_t caption_w{}, caption_h{};
    text_measure_string(dim, "Whisper Lock", &caption_w, &caption_h);
    const int32_t box_h = caption_h < 9 ? 9 : caption_h;
    CHECK(window_is_named(&lock->base, "BYCheckBox") && lock->base.parent == pane);
    CHECK(lock->base.x == 0x93 + 5 && lock->base.y == 0x7e + 0x63);
    CHECK(caption_w > 0 && lock->base.width == caption_w + 0xb && lock->base.height == box_h);
    CHECK(lock->label_gap == 2 && lock->base.font == dim && lock->base.visible);
    CHECK(lock->base.ops->is_class(class_name::check_box));
    CHECK(lock->base.states.count == 5 && lock->base.states.current == check_state::unchecked_up);
    bool captions = true, faces = true;
    for (int32_t s = 0; s < 5; ++s) {
        captions =
            captions && std::strcmp(text_get(lock->base.states.texts[s]), "Whisper Lock") == 0;
        faces = faces && lock->base.states.images[s] >= 0;
    }
    CHECK(captions && faces);
    CHECK(
        lock->base.states.images[check_state::unchecked_up] !=
        lock->base.states.images[check_state::checked_up]
    );
    CheckBox* tiny = lobby_check_box_create(pane, 5, 0x73, "", dim);
    CHECK(tiny->base.width == 0xb && tiny->base.height == 9);
    window_destroy(pane);
    font_release(dim);
}

// A caption at its measured width in the dim font, the value three pixels
// past it in the small font; the text and count setters rewrite the value.
void test_lobby_field(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Recon", 0x17, 0x7e, 0x1f2, 0x139, nullptr, true);
    HatFonts fonts{};
    fonts.small = toolkit_default_font(tk);
    fonts.small_dim = toolkit_font_tint(tk, fonts.small, 0x5a);
    int32_t caption_width{}, caption_height{};
    text_measure_string(fonts.small_dim, "Wind Speed:", &caption_width, &caption_height);
    CHECK(caption_width > 0);

    LobbyField* wind = lobby_field_create(parent, 0x11c, 0x92, 0x64, 0xa, "Wind Speed:", &fonts);
    Window* self = &wind->base;
    CHECK(self->x == 0x17 + 0x11c && self->y == 0x7e + 0x92);
    CHECK(self->width == 0x64 && self->height == 0xa && self->parent == parent);
    CHECK(self->children.count == 2);
    Window* caption = self->children.items[0];
    CHECK(caption->x == self->x && caption->y == self->y);
    CHECK(caption->width == caption_width && caption->height == 0xa);
    CHECK(caption->font == fonts.small_dim);
    CHECK_STR(text_get(caption->states.texts[1]), "Wind Speed:");
    CHECK(wind->value == self->children.items[1]);
    CHECK(wind->value->x == self->x + caption_width + 3 && wind->value->y == self->y);
    CHECK(wind->value->width == 0x64 - caption_width - 3 && wind->value->height == 0xa);
    CHECK(wind->value->font == fonts.small);
    CHECK(reinterpret_cast<LobbyLabel*>(wind->value)->style == text_style::left);
    CHECK_STR(text_get(wind->value->states.texts[1]), "");

    lobby_field_set_text(wind, "n/a");
    CHECK_STR(text_get(wind->value->states.texts[0]), "n/a");
    CHECK_STR(text_get(wind->value->states.texts[1]), "n/a");
    lobby_field_set_count(wind, 2967);
    CHECK_STR(text_get(wind->value->states.texts[1]), "2967");
    lobby_field_set_count(wind, static_cast<uint32_t>(-40));
    CHECK_STR(text_get(wind->value->states.texts[1]), "-40");

    // Without a caption the value spans the field.
    LobbyField* bare = lobby_field_create(parent, 0, 0, 0x40, 0xa, nullptr, &fonts, "7");
    CHECK(bare->base.children.count == 1 && bare->value != nullptr);
    CHECK(bare->value->x == bare->base.x && bare->value->width == 0x40);
    CHECK_STR(text_get(bare->value->states.texts[1]), "7");
    // No value text, or no room past the caption, leaves the value out and
    // the setters without effect.
    LobbyField* blank = lobby_field_create(parent, 0, 0x10, 0x64, 0xa, "Planet:", &fonts, nullptr);
    CHECK(blank->base.children.count == 1 && blank->value == nullptr);
    lobby_field_set_text(blank, "Fuel Dump");
    lobby_field_set_count(blank, 3);
    LobbyField* narrow =
        lobby_field_create(parent, 0, 0x20, caption_width + 3, 0xa, "Wind Speed:", &fonts);
    CHECK(narrow->base.children.count == 1 && narrow->value == nullptr);

    // Drawing shows the caption and value only while shown.
    toolkit_compose(tk, nullptr);
    const Surface* overlay = toolkit_surface(tk);
    parent->visible = 0;
    self->ops->draw(self);
    CHECK(testing::opaque_pixels(overlay) == 0);
    parent->visible = 1;
    self->ops->draw(self);
    CHECK(testing::opaque_pixels(overlay) > 0);
    toolkit_compose(tk, nullptr);
    window_destroy(parent);
    font_release(fonts.small_dim);
}

// Fonts with a distinct handle per HAT font the rows use.
struct RowFonts {
    HatFonts fonts{};

    void create(Toolkit* tk) {
        fonts.small = toolkit_default_font(tk);
        fonts.small_light = toolkit_font_tint(tk, fonts.small, 0xff);
        fonts.small_dim = toolkit_font_tint(tk, fonts.small, 0x5a);
        fonts.small_green = toolkit_font_tint(tk, fonts.small, 0xfa);
    }

    void release() {
        font_release(fonts.small_light);
        font_release(fonts.small_dim);
        font_release(fonts.small_green);
    }
};

// A framed Boneyards list at (0x1c, 0x26), 0x90 wide: rows 0x7f wide.
ByListBox* row_list(Window* parent, int32_t height) {
    ByListBox* list = by_list_box_create(true, false);
    window_set_bounds(&list->list.base, 0x1c, 0x26, 0x90, height, parent, true);
    return list;
}

// Clicks the window drawn at (x, y), as the desktop routes a press.
void click_at(Window* root, int32_t x, int32_t y) {
    Message press{};
    press.code = MessageCode::left_down;
    press.x = x;
    press.y = y;
    Window* hit = window_hit_test(root, x, y);
    CHECK(hit != nullptr);
    if (hit != nullptr)
        window_dispatch(hit, &press);
}

// Whether two images draw the same pixels (and draw any at all).
bool same_pixels(Toolkit* tk, Handle a, Handle b) {
    constexpr int32_t size = 0x10;
    uint8_t drawn[2][size * size]{};
    const Handle images[2] = {a, b};
    int32_t opaque{};
    for (int32_t i = 0; i < 2; ++i) {
        toolkit_compose(tk, nullptr);
        image_draw(0, 0, size, size, images[i]);
        const Surface* overlay = toolkit_surface(tk);
        for (int32_t y = 0; y < size; ++y)
            std::memcpy(drawn[i] + y * size, overlay->pixels + y * overlay->pitch, size);
        opaque += testing::opaque_pixels(overlay);
    }
    toolkit_compose(tk, nullptr);
    return opaque > 0 && std::memcmp(drawn[0], drawn[1], sizeof drawn[0]) == 0;
}

// A press on a list's scroll bar raises the list, not the bar: the bar stays the list's first
// child, so a scroll still hides the rows above the first one shown and keeps the bar along the
// list's right edge.
void test_list_bar_raise(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Chat", 0, 0, 0x140, 0xc0, nullptr, true);
    HatFonts fonts{};
    fonts.small = toolkit_default_font(tk);
    fonts.small_light = fonts.small;
    ByListBox* list = row_list(parent, 0x46);
    Window* box = &list->list.base;
    std::vector<Window*> rows;
    const char* const names[] = {"one", "two", "three", "four", "five", "six", "seven", "eight"};
    for (const char* name : names)
        rows.push_back(&by_text_row_create(list, name, &fonts, true)->base);
    Window* bar = &list->slider->slider.base;
    const int32_t bar_x = box->x + box->width - 0x12;
    CHECK(box->children.items[0] == bar && bar->x == bar_x && bar->visible == 1);
    bar->ops->raise(bar);
    CHECK(box->children.items[0] == bar);
    slider_set_value(&list->slider->slider, slider_index(&list->slider->slider) - 1);
    CHECK(list->list.top == 1);
    CHECK(rows[0]->visible == 0 && rows[1]->visible == 1 && rows[1]->y == box->y);
    CHECK(bar->x == bar_x && bar->visible == 1);
    window_destroy(parent);
}

// RankIcon at (3,3), the name 0x11 in, the LatencyIcon past the name,
// appended to the list; the row's selected and unselected looks pick the
// name's font.
void test_player_row(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Chat", 0, 0, 0x140, 0xc0, nullptr, true);
    RowFonts fonts{};
    fonts.create(tk);
    ByListBox* list = row_list(parent, 0x58);
    ByPlayerRow* doe = by_player_row_create(list, "Commander_Doe", 0x2a, &fonts.fonts);
    ByPlayerRow* roe = by_player_row_create(list, "Roe", 0x80000007u, &fonts.fonts);
    Window* self = &doe->row.base;
    CHECK(list->list.count == 2 && list_box_item(&list->list, 0) == self);
    CHECK(list_box_item(&list->list, 1) == &roe->row.base);
    CHECK(self->parent == &list->list.base && self->focusable == 0);
    CHECK(self->x == 0x1c && self->y == 0x26 && self->width == 0x7f && self->height == 0x11);
    CHECK(roe->row.base.y == 0x26 + 0x11);
    CHECK_STR(doe->name, "Commander_Doe");
    CHECK(doe->user_id == 0x2a && roe->user_id == 0x80000007u);
    CHECK(!doe->whisper_locked && !doe->silenced && doe->row.label == nullptr);

    CHECK(self->children.count == 3);
    Window* rank = doe->icons.user_icon;
    CHECK(self->children.items[0] == rank && window_is_named(rank, "RankIcon"));
    CHECK(rank->x == 0x1c + 3 && rank->y == 0x26 + 3 && rank->width == 0xb && rank->height == 0xb);
    CHECK(rank->ops->is_class(class_name::static_text));
    Window* name = doe->name_label;
    CHECK(self->children.items[1] == name);
    CHECK(name->x == 0x1c + 0x11 && name->y == 0x26);
    CHECK(name->width == 0x7f - 0x22 && name->height == 0x11);
    CHECK(reinterpret_cast<LobbyLabel*>(name)->style == text_style::left);
    CHECK_STR(text_get(name->states.texts[1]), "Commander_Doe");
    Window* latency = doe->icons.latency_icon;
    CHECK(self->children.items[2] == latency && window_is_named(latency, "LatencyIcon"));
    CHECK(latency->x == name->x + name->width && latency->y == 0x26 + 4);
    CHECK(latency->width == 0xb && latency->height == 0xb);
    // Until the member reply sets them: User_Icons from 0x13, Latency_Icons from 1.
    CHECK(doe->icons.user_frame == 0x13 && doe->icons.latency_frame == 1);

    // The shown state is the second frame of the bound pair. Grade 1 draws
    // the red square, grade 2 the amber and grade 3 the green, and the words
    // name those colours.
    const struct {
        int grade;
        const char* phrase;
        int low;
        int high;
    } grades[] = {
        {1, "Latency poor", 201, 205},
        {2, "Latency fair", 195, 199},
        {3, "Latency good", 160, 164},
    };

    for (const auto& grade : grades) {
        player_row_set_latency(&doe->icons, grade.grade);
        PlayerIconWords words{};
        CHECK(player_icon_words(latency, &words));
        CHECK(words.kind == PlayerIconKind::latency && words.latency_grade == grade.grade);
        CHECK_STR(latency_grade_phrase(grade.grade), grade.phrase);
        toolkit_compose(tk, nullptr);
        image_draw(0, 0, 0xb, 0xb, latency->states.images[1]);
        const Surface* drawn = toolkit_surface(tk);
        bool body = false;
        for (int32_t y = 0; y < 0xb; ++y) {
            const uint8_t* line = drawn->pixels + static_cast<std::ptrdiff_t>(y) * drawn->pitch;
            for (int32_t x = 0; x < 0xb; ++x) {
                const int index = line[x];
                if (index == k_overlay_transparent || index == 90)
                    continue;
                CHECK(index >= grade.low && index <= grade.high);
                body = true;
            }
        }
        CHECK(body);
        toolkit_compose(tk, nullptr);
    }
    player_row_set_latency(&doe->icons, 0);
    PlayerIconWords unnamed{};
    CHECK(player_icon_words(latency, &unnamed) && unnamed.latency_grade == 0);
    CHECK_STR(latency_grade_phrase(0), "Latency unknown");
    player_row_set_latency(&doe->icons, 4);
    CHECK(player_icon_words(latency, &unnamed) && unnamed.latency_grade == 0);
    CHECK_STR(latency_grade_phrase(4), "Latency unknown");
    player_row_set_latency(&doe->icons, 1);
    const Handle user_icon = image_load("Widgets.gaf", "User_Icons", 0x14, kWidgetKey);
    CHECK(same_pixels(tk, rank->states.images[1], user_icon));
    image_release(user_icon);

    // is_class answers only its own class name.
    CHECK(self->ops->is_class("BYPlayerListItem"));
    CHECK(!self->ops->is_class("BYChatRoomListItem"));
    CHECK(!self->ops->is_class(class_name::static_text));
    CHECK(!self->ops->is_class(nullptr));

    // Font per state, applied by the draw.
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small);
    doe->silenced = true;
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small_dim);
    doe->whisper_locked = true;
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small_green);
    list->selected_item = self;
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small_light);
    roe->row.base.ops->draw(&roe->row.base);
    CHECK(roe->name_label->font == fonts.fonts.small);
    doe->whisper_locked = false;
    list->selected_item = nullptr;
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small_dim);
    // The text-row font slot leaves the name alone.
    by_text_row_set_font(&doe->row, fonts.fonts.small_green);
    CHECK(name->font == fonts.fonts.small_dim);

    // A click on the rank icon or the name selects the row in its list.
    click_at(parent, rank->x + 5, rank->y + 5);
    CHECK(list->selected_item == self && list->list.selected == 0);
    click_at(parent, roe->name_label->x + 2, roe->name_label->y + 8);
    CHECK(list->selected_item == &roe->row.base && list->list.selected == 1);
    CHECK_STR(by_list_selected_text(list), "");

    window_destroy(parent);
    fonts.release();
}

// An Icon_Circle, the name and the count, placed by type; the draw circles
// the current frequency, the selected and unselected looks set the fonts
// and the count setters the count.
void test_chat_room_rows(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Chat", 0, 0, 0x140, 0x120, nullptr, true);
    RowFonts fonts{};
    fonts.create(tk);
    ByListBox* list = row_list(parent, 0xa0);
    uint32_t current_room{};

    struct Listing {
        const char* name;
        uint32_t room_id;
        int32_t type;
    };

    const Listing listings[] = {
        {"Tactics", 12, room_type::planetary_default},
        {"Debrief_9", 40, room_type::battle_debriefing},
        {"Main", 10, room_type::planetary_default},
        {"Newbies", 30, room_type::user_created},
        {"Debrief_1", 41, room_type::battle_debriefing},
        {"main", 11, room_type::planetary_default},
        {"Arena", 31, room_type::user_created},
        {"Zulu", 13, room_type::planetary_default},
        {"10th", 14, room_type::planetary_default},
    };
    ByChatRoomRow* rows[9]{};
    for (int32_t i = 0; i < 9; ++i)
        rows[i] = by_chat_room_row_create(
            list,
            listings[i].name,
            "A frequency",
            "tag",
            listings[i].room_id,
            i == 2,
            listings[i].type,
            &fonts.fonts,
            &current_room
        );
    // Planetary defaults by name (digits first, then letters without case,
    // lower case first on a tie), then user-created frequencies and battle
    // debriefings, each in arrival order.
    const char* order[] = {
        "10th", "main", "Main", "Tactics", "Zulu", "Newbies", "Arena", "Debrief_9", "Debrief_1"
    };
    CHECK(list->list.count == 9);
    for (int32_t i = 0; i < 9; ++i)
        CHECK_STR(reinterpret_cast<ByChatRoomRow*>(list_box_item(&list->list, i))->name, order[i]);

    ByChatRoomRow* main = rows[2];
    Window* self = &main->row.base;
    CHECK_STR(main->name, "Main");
    CHECK_STR(main->description, "A frequency");
    CHECK_STR(main->tag, "tag");
    CHECK(main->room_id == 10 && main->needs_password && !rows[0]->needs_password);
    CHECK(main->type == room_type::planetary_default && main->users == 0);
    CHECK(self->focusable == 0 && self->width == 0x7f && self->height == 0x11);
    CHECK(self->x == 0x1c && self->y == 0x26 + 2 * 0x11);
    CHECK(self->children.count == 3);
    Window* icon = main->icon;
    CHECK(self->children.items[0] == icon && window_is_named(icon, "FrequencyIcon"));
    CHECK(icon->x == self->x + 3 && icon->y == self->y + 3);
    CHECK(icon->width == 0xb && icon->height == 0xb);
    Window* name = main->name_label;
    CHECK(self->children.items[1] == name);
    CHECK(name->x == self->x + 0x11 && name->y == self->y);
    CHECK(name->width == 0x7f - 0x1f && name->height == 0x11);
    CHECK(reinterpret_cast<LobbyLabel*>(name)->style == text_style::left);
    CHECK_STR(text_get(name->states.texts[1]), "Main");
    Window* count = main->count_label;
    CHECK(self->children.items[2] == count);
    CHECK(count->x == self->x + 0x7f - 0xe && count->y == self->y);
    CHECK(count->width == 0xe && count->height == 0x11);
    CHECK(reinterpret_cast<LobbyLabel*>(count)->style == 0);
    CHECK_STR(text_get(count->states.texts[1]), "0");

    // Icon_Circle frame 1 marks the current frequency, frame 2 the others.
    const Handle frame_1 = image_load("Widgets.gaf", "Icon_Circle", 1, kWidgetKey);
    const Handle frame_2 = image_load("Widgets.gaf", "Icon_Circle", 2, kWidgetKey);
    CHECK(!same_pixels(tk, frame_1, frame_2));
    CHECK(same_pixels(tk, main->current_icon, frame_1));
    CHECK(same_pixels(tk, main->other_icon, frame_2));
    CHECK(icon->states.images[1] == main->other_icon);
    current_room = 10;
    self->ops->draw(self);
    CHECK(icon->states.images[1] == main->current_icon);
    rows[5]->row.base.ops->draw(&rows[5]->row.base);
    CHECK(rows[5]->icon->states.images[1] == rows[5]->other_icon);
    current_room = 11;
    self->ops->draw(self);
    CHECK(icon->states.images[1] == main->other_icon);
    ByChatRoomRow* unsessioned = by_chat_room_row_create(
        list, "Lobby", "", "", 10, false, room_type::battle_debriefing, &fonts.fonts, nullptr
    );
    Window* lobby = &unsessioned->row.base;
    lobby->ops->draw(lobby);
    CHECK(unsessioned->icon->states.images[1] == unsessioned->other_icon);
    CHECK(list_box_item(&list->list, 9) == lobby);
    image_release(frame_1);
    image_release(frame_2);

    // The name and the count take the light font on the selected row.
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small && count->font == fonts.fonts.small);
    list->selected_item = self;
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small_light && count->font == fonts.fonts.small_light);
    list->selected_item = nullptr;
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small && count->font == fonts.fonts.small);

    // The count label shows the stored count as signed decimal.
    by_chat_room_row_set_count(main, 17);
    CHECK(main->users == 17);
    CHECK_STR(text_get(count->states.texts[1]), "17");
    by_chat_room_row_add_count(main, -3);
    CHECK(main->users == 14);
    CHECK_STR(text_get(count->states.texts[1]), "14");
    by_chat_room_row_add_count(main, -20);
    CHECK_STR(text_get(count->states.texts[1]), "-6");

    CHECK(self->ops->is_class("BYChatRoomListItem"));
    CHECK(!self->ops->is_class("BYPlayerListItem"));
    CHECK(!self->ops->is_class(class_name::static_text));

    // A click anywhere on the row's children selects it.
    ByChatRoomRow* tactics = reinterpret_cast<ByChatRoomRow*>(list_box_item(&list->list, 3));
    click_at(parent, tactics->count_label->x + 4, tactics->count_label->y + 8);
    CHECK(list->selected_item == &tactics->row.base && list->list.selected == 3);
    click_at(parent, icon->x + 5, icon->y + 5);
    CHECK(list->selected_item == self && list->list.selected == 2);

    // An unlisted type (0) builds a bare row outside the list.
    ByChatRoomRow* stray = by_chat_room_row_create(
        list, "Stray", "", "", 50, false, room_type::unlisted, &fonts.fonts, &current_room
    );
    CHECK(stray->row.base.parent == nullptr && stray->row.base.children.count == 0);
    CHECK(stray->room_id == 50 && stray->type == room_type::unlisted);
    CHECK_STR(stray->name, "");
    CHECK(stray->current_icon == k_no_handle && stray->icon == nullptr);
    CHECK(list->list.count == 10);
    by_chat_room_row_set_count(stray, 4);
    CHECK(stray->users == 4);
    window_destroy(&stray->row.base);

    window_destroy(parent);
    fonts.release();
}

// An Icon_Circle (frame 2 until selected) and the category's name in the
// small font, appended in arrival order; the selected look puts the row in
// frame 1 and the light font.
void test_circle_rows(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Profile", 0, 0, 0x140, 0x120, nullptr, true);
    RowFonts fonts{};
    fonts.create(tk);
    ByListBox* list = row_list(parent, 0xa0);
    ByCircleRow* login = by_circle_row_create(list, "Login", 0, &fonts.fonts);
    ByCircleRow* boneyards = by_circle_row_create(list, "Boneyards", 2, &fonts.fonts);
    CHECK(list->list.count == 2 && list_box_item(&list->list, 0) == &login->row.base);
    CHECK(list_box_item(&list->list, 1) == &boneyards->row.base);
    CHECK(login->tab == 0 && boneyards->tab == 2);
    Window* self = &boneyards->row.base;
    CHECK(self->parent == &list->list.base && self->focusable == 0);
    CHECK(self->x == 0x1c && self->y == 0x26 + 0x11 && self->width == 0x7f && self->height == 0x11);
    CHECK(self->children.count == 2);
    Window* icon = boneyards->icon;
    CHECK(self->children.items[0] == icon && window_is_named(icon, "ProfileCategoryIcon"));
    CHECK(icon->x == self->x + 3 && icon->y == self->y + 3);
    CHECK(icon->width == 0xb && icon->height == 0xb);
    Window* name = boneyards->name_label;
    CHECK(self->children.items[1] == name);
    CHECK(name->x == self->x + 0x11 && name->y == self->y);
    CHECK(name->width == 0x7f - 0x11 && name->height == 0x11);
    CHECK(reinterpret_cast<LobbyLabel*>(name)->style == text_style::left);
    CHECK_STR(text_get(name->states.texts[1]), "Boneyards");

    const Handle frame_1 = image_load("Widgets.gaf", "Icon_Circle", 1, kWidgetKey);
    const Handle frame_2 = image_load("Widgets.gaf", "Icon_Circle", 2, kWidgetKey);
    CHECK(same_pixels(tk, boneyards->selected_icon, frame_1));
    CHECK(same_pixels(tk, boneyards->other_icon, frame_2));
    CHECK(icon->states.images[1] == boneyards->other_icon);
    image_release(frame_1);
    image_release(frame_2);

    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small && icon->states.images[1] == boneyards->other_icon);
    list->selected_item = self;
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small_light);
    CHECK(icon->states.images[1] == boneyards->selected_icon);
    list->selected_item = &login->row.base;
    self->ops->draw(self);
    CHECK(name->font == fonts.fonts.small && icon->states.images[1] == boneyards->other_icon);

    CHECK(self->ops->is_class("BYCircleLstItem"));
    CHECK(!self->ops->is_class("BYChatRoomListItem"));
    CHECK(!self->ops->is_class(class_name::static_text));

    click_at(parent, name->x + 4, name->y + 8);
    CHECK(list->selected_item == self && list->list.selected == 1);
    click_at(parent, login->icon->x + 5, login->icon->y + 5);
    CHECK(list->selected_item == &login->row.base && list->list.selected == 0);

    window_destroy(parent);
    fonts.release();
}

// The rows sort type 1 names as the Chat console does: the word sort
// weighs a hyphen after the letters and case only on a tie.
void test_chat_room_row_word_sort(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Chat", 0, 0, 0x140, 0x120, nullptr, true);
    RowFonts fonts{};
    fonts.create(tk);
    ByListBox* list = row_list(parent, 0xa0);
    for (const char* name : {"Calm", "co-op", "calm", "coop", "Bay"})
        by_chat_room_row_create(
            list, name, "", "", 1, false, room_type::planetary_default, &fonts.fonts, nullptr
        );
    const char* order[] = {"Bay", "calm", "Calm", "coop", "co-op"};
    CHECK(list->list.count == 5);
    for (int32_t i = 0; i < 5; ++i)
        CHECK_STR(reinterpret_cast<ByChatRoomRow*>(list_box_item(&list->list, i))->name, order[i]);
    window_destroy(parent);
    fonts.release();
}

// A frequency list and a player list as the Chat console shows them, with
// one selected row, the current frequency, a whisper-locked and a muzzled
// player. Written to $OA_SNAPSHOT_DIR/chat-rows.raw (640 x 480 palette
// indexes) when that is set.
void snapshot_chat_rows(Toolkit* tk) {
    const auto dir_value = oa::platform::environment_value("OA_SNAPSHOT_DIR");
    const char* dir = dir_value ? dir_value->c_str() : nullptr;
    if (dir == nullptr)
        return;
    Window* parent = window_create_plain();
    window_create(parent, "Chat", 0, 0, 640, 480, nullptr, true);
    RowFonts fonts{};
    fonts.create(tk);
    uint32_t current_room = 11;
    ByListBox* rooms = by_list_box_create(true, false);
    window_set_bounds(&rooms->list.base, 0x1c, 0x96, 0xa0, 0x80, parent, true);

    struct Room {
        const char* name;
        uint32_t room_id;
        int32_t type;
        uint32_t users;
    };

    const Room listed[] = {
        {"Clan_BSC", 12, room_type::user_created, 2},
        {"Main", 11, room_type::planetary_default, 4},
        {"Tactics", 13, room_type::planetary_default, 9},
        {"Debrief_7", 40, room_type::battle_debriefing, 1},
        {"Newbies", 14, room_type::planetary_default, 23},
    };
    for (const Room& room : listed) {
        ByChatRoomRow* row = by_chat_room_row_create(
            rooms, room.name, "", "", room.room_id, false, room.type, &fonts.fonts, &current_room
        );
        by_chat_room_row_set_count(row, room.users);
        if (room.room_id == 13)
            rooms->selected_item = &row->row.base;
    }
    ByListBox* players = by_list_box_create(true, false);
    window_set_bounds(&players->list.base, 0xd0, 0x96, 0xa0, 0x80, parent, true);

    struct Player {
        const char* name;
        int32_t icon;
        int32_t latency;
        bool whisper_locked;
        bool silenced;
    };

    const Player members[] = {
        {"Yard_Dog", 0x24, 0, false, false},
        {"Ghost_Commander", 0x11, 1, true, false},
        {"Dan", 0x08, 2, false, false},
        {"Spammer", 0x01, 3, false, true},
    };
    for (const Player& member : members) {
        ByPlayerRow* row = by_player_row_create(players, member.name, 1, &fonts.fonts);
        player_row_set_user_icon(&row->icons, member.icon);
        player_row_set_latency(&row->icons, member.latency);
        row->whisper_locked = member.whisper_locked;
        row->silenced = member.silenced;
        if (member.icon == 0x08)
            players->selected_item = &row->row.base;
    }
    // The rows over the transparent overlay, where the console's backdrop
    // shows through the lists.
    toolkit_compose(tk, nullptr);
    window_draw_children(&rooms->list.base);
    window_draw_children(&players->list.base);
    const Surface* overlay = toolkit_surface(tk);
    const std::string path = std::string(dir) + "/chat-rows.raw";
    if (std::FILE* out = oa::platform::open_file(path.c_str(), "wb")) {
        for (int32_t y = 0; y < overlay->height; ++y)
            std::fwrite(
                overlay->pixels + y * overlay->pitch,
                1,
                static_cast<std::size_t>(overlay->width),
                out
            );
        std::fclose(out);
    }
    toolkit_compose(tk, nullptr);
    window_destroy(parent);
    fonts.release();
}

// The Recon screen's eight fields at their places, filled for Fuel Dump.
// Written to $OA_SNAPSHOT_DIR/recon-fields.raw (640 x 480 palette indexes)
// when that is set.
void snapshot_recon_fields(Toolkit* tk) {
    const auto dir_value = oa::platform::environment_value("OA_SNAPSHOT_DIR");
    const char* dir = dir_value ? dir_value->c_str() : nullptr;
    if (dir == nullptr)
        return;

    struct Row {
        int32_t x, y, w;
        const char* caption;
        const char* value;
    };

    const Row rows[] = {
        {0xc, 0x92, 0xa6, "Planet:", "Fuel Dump"},
        {0xc, 0xa3, 0xa6, "GW Map:", "Over Crude Water"},
        {0xb6, 0x92, 0x62, "Type:", "Metal"},
        {0xb6, 0xa3, 0x62, "Gravity:", "112"},
        {0x11c, 0x92, 0x64, "Wind Speed:", "2967"},
        {0x11c, 0xa3, 0x64, "Tide Strength:", "25"},
        {0x184, 0x92, 0x5c, "Affiliation:", "Arm"},
        {0x184, 0xa3, 0x5c, "Population:", "17"},
    };
    Window* parent = window_create_plain();
    window_create(parent, "Recon", 0x17, 0x7e, 0x1f2, 0x139, nullptr, true);
    HatFonts fonts{};
    fonts.small = toolkit_default_font(tk);
    fonts.small_dim = toolkit_font_tint(tk, fonts.small, 0x5a);
    toolkit_compose(tk, nullptr);
    for (const Row& row : rows) {
        LobbyField* field =
            lobby_field_create(parent, row.x, row.y, row.w, 0xa, row.caption, &fonts);
        lobby_field_set_text(field, row.value);
        field->base.ops->draw(&field->base);
    }
    const Surface* overlay = toolkit_surface(tk);
    const std::string path = std::string(dir) + "/recon-fields.raw";
    if (std::FILE* out = oa::platform::open_file(path.c_str(), "wb")) {
        for (int32_t y = 0; y < overlay->height; ++y)
            std::fwrite(
                overlay->pixels + y * overlay->pitch,
                1,
                static_cast<std::size_t>(overlay->width),
                out
            );
        std::fclose(out);
    }
    toolkit_compose(tk, nullptr);
    window_destroy(parent);
    font_release(fonts.small_dim);
}

} // namespace

// The Scoreboard's message row: "Cartman" in the dim small font at its
// measured width 3 pixels in, the message in the small font 3 pixels past
// it to the row's end; 0x11 in when indented.
void test_two_label_row(Toolkit* tk) {
    Window* parent = window_create_plain();
    window_create(parent, "Scoreboard", 0, 0, 0x140, 0xc0, nullptr, true);
    RowFonts fonts{};
    fonts.create(tk);
    ByListBox* list = row_list(parent, 0x58);
    ByTwoLabelRow* row = by_two_label_row_create(list, "Cartman", "gg", &fonts.fonts, false);
    Window* self = &row->row.base;
    CHECK(list->list.count == 1 && list_box_item(&list->list, 0) == self);
    CHECK(self->x == 0x1c && self->width == 0x7f && self->height == 0x11);
    int32_t width{}, height{};
    text_measure_string(fonts.fonts.small_dim, "Cartman", &width, &height);
    CHECK(
        row->first_label != nullptr && row->first_label->x == self->x + 3 &&
        row->first_label->width == width && row->first_label->font == fonts.fonts.small_dim
    );
    CHECK_STR(text_get(row->first_label->states.texts[1]), "Cartman");
    CHECK(
        row->second_label != nullptr && row->second_label->x == self->x + 3 + width + 3 &&
        row->second_label->x + row->second_label->width == self->x + self->width &&
        row->second_label->font == fonts.fonts.small
    );
    CHECK_STR(text_get(row->second_label->states.texts[1]), "gg");
    ByTwoLabelRow* indented = by_two_label_row_create(list, nullptr, "only", &fonts.fonts, true);
    CHECK(
        indented->first_label == nullptr && indented->second_label->x == indented->row.base.x + 0x11
    );
    window_destroy(parent);
    fonts.release();
}

struct GroupEvents {
    std::vector<std::string> sounds;
    std::vector<int32_t> codes;
    std::vector<int32_t> values;
};

GroupEvents g_group;

void group_parent_notify(Window*, const Notify* notify) {
    g_group.codes.push_back(static_cast<int32_t>(notify->code));
    g_group.values.push_back(notify->value);
}

// The radio group: its caption over its buttons 0xf down, 5 pixels apart,
// the first selected; picking a member sounds GREEN_BUTTON, selects it and
// tells the parent code 200 with its index, and any other notice sounds
// GREEN_BUTTON alone; the name and index setters, the
// refresh and the disabling keep a disabled member disabled.
void test_radio_group(Toolkit* tk) {
    static WindowOps parent_ops = k_window_ops;
    parent_ops.on_notify = group_parent_notify;
    Window* parent = window_create_plain();
    parent->ops = &parent_ops;
    window_create(parent, "Pane", 0x20, 0x10, 0x176, 0x9d, nullptr, true);
    RowFonts fonts{};
    fonts.create(tk);
    fonts.fonts.medium = fonts.fonts.small;
    const char* const names[] = {"Novice", "Seasoned", "Expert"};
    ByRadioGroup* group =
        by_radio_group_create(parent, 0xb1, 0x1c, "Player Skill", names, 3, &fonts.fonts);
    group->play_sound = [](void*, const char* name) { g_group.sounds.emplace_back(name); };
    Window* self = &group->base;
    CHECK(
        window_is_named(self, "RadioGroup") && self->x == 0x20 + 0xb1 && self->y == 0x10 + 0x1c &&
        self->height == 0x1f && group->count == 3
    );
    int32_t widths[3]{};
    int32_t total = 0;
    for (int32_t i = 0; i < 3; ++i) {
        int32_t height{};
        text_measure_string(fonts.fonts.small, names[i], &widths[i], &height);
        total += widths[i] + 0xb + (i != 0 ? 5 : 0);
    }
    CHECK(self->width == total);
    CHECK(group->members[0]->base.x == self->x && group->members[0]->base.y == self->y + 0xf);
    CHECK(group->members[1]->base.x == self->x + widths[0] + 0xb + 5);
    CHECK(
        group->selected == 0 && group->members[0]->base.states.current == widget_value::selected &&
        group->members[1]->base.states.current == widget_value::enabled
    );
    CHECK(group->members[1]->member_count == 2);
    CHECK_STR(by_radio_group_selected_name(group), "Novice");

    Notify pick{NotifyCode::activated, &group->members[2]->base, 0, nullptr};
    self->ops->on_notify(self, &pick);
    CHECK((g_group.sounds == std::vector<std::string>{"GREEN_BUTTON"}));
    CHECK(
        (g_group.codes == std::vector<int32_t>{200}) && (g_group.values == std::vector<int32_t>{2})
    );
    CHECK(
        group->selected == 2 && group->members[2]->base.states.current == widget_value::selected &&
        group->members[0]->base.states.current == widget_value::enabled
    );
    // A notice from a window that is no member still sounds GREEN_BUTTON but
    // selects nothing and tells the parent nothing.
    Notify stray{NotifyCode::activated, parent, 0, nullptr};
    self->ops->on_notify(self, &stray);
    CHECK((g_group.sounds == std::vector<std::string>{"GREEN_BUTTON", "GREEN_BUTTON"}));
    CHECK(g_group.codes.size() == 1 && group->selected == 2);

    by_radio_group_disable_name(group, "Seasoned");
    by_radio_group_select_name(group, "Novice");
    CHECK(group->selected == 0 && group->members[1]->base.states.current == widget_value::disabled);
    by_radio_group_disable_others(group);
    CHECK(group->members[2]->base.states.current == widget_value::disabled);
    by_radio_group_refresh(group);
    CHECK(
        group->members[1]->base.states.current == widget_value::enabled &&
        group->members[2]->base.states.current == widget_value::enabled
    );
    RadioGroup model{};
    radio_add(&model, "Novice", widget_value::enabled);
    radio_add(&model, "Seasoned", widget_value::enabled);
    radio_add(&model, "Expert", widget_value::enabled);
    by_radio_group_read(group, &model);
    CHECK(model.selected == 0 && model.members[0].value == widget_value::selected);
    radio_select_by_name(&model, "Expert");
    radio_disable_by_name(&model, "Novice");
    by_radio_group_show(group, model);
    CHECK(group->selected == 2 && group->members[0]->base.states.current == widget_value::disabled);
    window_destroy(parent);
    fonts.release();
}

int main() {
    testing::InstalledData data;
    testing::open_installed_data(&data);
    const ToolkitConfig config = testing::installed_config(&data, 640, 480);
    Toolkit* tk = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(tk));
    test_layout(tk);
    test_installed_draw(tk);
    test_lobby_controls(tk);
    test_lobby_field(tk);
    test_player_row(tk);
    test_chat_room_rows(tk);
    test_chat_room_row_word_sort(tk);
    test_circle_rows(tk);
    test_two_label_row(tk);
    test_radio_group(tk);
    test_list_bar_raise(tk);
    snapshot_recon_fields(tk);
    snapshot_chat_rows(tk);
    services_install(previous);
    toolkit_destroy(tk);
    return by_test::finish("boneyards lobby widgets");
}
