// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/lobby_widgets.hpp"

#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/screens/widgets.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/present/raster.hpp"

#include "text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <span>
#include <unordered_map>
#include <vector>

namespace oa::boneyards::screens {
namespace {

const HatFonts* g_button_fonts{};
std::vector<uint8_t> g_palette_bytes;
int32_t g_muzzle_font = k_no_handle;
std::unordered_map<const Window*, uint8_t> g_latency_applied;
void (*g_player_row_base_destroy)(Window*) = nullptr;
Handle g_lock_mark = k_no_handle;
Handle g_mute_mark = k_no_handle;

void paint_latency_icon(PlayerRowIcons* row, bool reload);

/// Draws a function button's face and its caption in the state's font, then the focus frame.
///
/// @param self the button
/// @return true always
bool function_button_draw(Window* self) {
    if (!window_is_shown(self))
        return true;
    const int32_t state = self->states.current;
    if (state < 0 || state >= self->states.count)
        return true;
    image_draw(self->x, self->y, self->width, self->height, self->states.images[state]);
    Handle font = self->font;
    if (g_button_fonts != nullptr) {
        if (state < 2)
            font = g_button_fonts->button_pressed;
        else if (state == 2)
            font = g_button_fonts->button_normal;
        else if (state == 3)
            font = g_button_fonts->button_hover;
    }
    text_draw(font, self->x, self->y, self->width, self->height, self->states.texts[state]);
    if (self->focused != 0)
        draw_focus_frame(self->x, self->y, self->width, self->height);
    return true;
}

/// Draws the radio face one pixel down and its caption eleven pixels right.
///
/// @param self the radio button
/// @return true always
bool lobby_radio_draw(Window* self) {
    if (!window_is_shown(self))
        return true;
    const int32_t state = self->states.current;
    if (state < 0 || state >= self->states.count)
        return true;
    image_draw(self->x, self->y + 1, self->width, self->height, self->states.images[state]);
    text_draw(
        self->font, self->x + 0xb, self->y, self->width, self->height, self->states.texts[state]
    );
    return true;
}

/// Lets a selected radio go of its selection when one of its members reports its own selection.
///
/// Only while shown; other radios' reports are ignored.
///
/// @param self the lobby radio button
/// @param notify control notice
void lobby_radio_on_notify(Window* self, const Notify* notify) {
    if (!window_is_shown(self) || notify == nullptr || notify->code != NotifyCode::radio_selected)
        return;
    const auto* radio = reinterpret_cast<const LobbyRadio*>(self);
    for (int32_t i = 0; i < radio->member_count; ++i) {
        if (notify->source != radio->members[i])
            continue;
        if (self->states.current == button_state::pressed)
            window_set_state(self, button_state::unpressed);
        return;
    }
}

WindowOps lobby_radio_ops() {
    WindowOps ops = k_radio_button_ops;
    ops.on_notify = lobby_radio_on_notify;
    ops.draw = lobby_radio_draw;
    return ops;
}

const WindowOps k_lobby_radio_ops = lobby_radio_ops();

/// Replaces the text field's text; the caption style is not used.
///
/// @param self the text field
/// @param text new text
/// @param style caption style; not read
void lobby_edit_set_caption(Window* self, const char* text, int32_t /*style*/) {
    single_edit_set_text(reinterpret_cast<SingleEdit*>(self), text);
}

WindowOps function_button_ops() {
    WindowOps ops = k_button_ops;
    ops.draw = function_button_draw;
    return ops;
}

const WindowOps k_function_button_ops = function_button_ops();

// The lobby check box is its caption's width and this wide, and at least
// kCheckBoxMinHeight high.
constexpr int32_t kCheckBoxPadding = 0xb;
constexpr int32_t kCheckBoxMinHeight = 9;
constexpr int32_t kCheckBoxLabelGap = 2;

void bind_images(Window* w, const char* file, const char* sequence, int32_t first) {
    if (w != nullptr && w->ops != nullptr && w->ops->bind_images != nullptr)
        w->ops->bind_images(w, file, sequence, first, kWidgetKey);
}

// Scroll bar geometry and art. The "scrollbar" sequence holds the increment
// arrow frames from 0, the decrement arrow from 4, the long thumb from 8 and
// the short thumb from 12.
constexpr const char* kScrollbarSequence = "scrollbar";
constexpr int32_t kSliderWidth = 0x11;
constexpr int32_t kSliderInset = 0x12; // from the list's right edge to the bar
constexpr int32_t kArrowHeight = 9;
constexpr int32_t kTallSliderHeight = 0x59; // taller bars get the long thumb
constexpr int32_t kLongThumbHeight = 0x1e;
constexpr int32_t kShortThumbHeight = 0x11;
constexpr int32_t kIncrementFrame = 0;
constexpr int32_t kDecrementFrame = 4;
constexpr int32_t kLongThumbFrame = 8;
constexpr int32_t kShortThumbFrame = 12;
// The track is a vertical line in the bar's own image.
constexpr int32_t kTrackX = 8;
constexpr int32_t kTrackTop = 0xb;
constexpr int32_t kTrackBottomInset = 0xc;
constexpr uint8_t kTrackColor = 0x5a; // also the framed list outline
constexpr uint8_t kTrackDisabledColor = 0x5d;
constexpr int32_t kListStateDisabled = 0;
constexpr int32_t kListStateNormal = 1;

BySlider* as_by_slider(Window* w) {
    return reinterpret_cast<BySlider*>(w);
}

ByListBox* as_by_list(Window* w) {
    return reinterpret_cast<ByListBox*>(w);
}

// A blank image of the window's size, transparent throughout. Callers drop
// the creation reference once the window states hold the image.
Handle blank_window_image(const Window* w) {
    return image_create_blank(w->width, w->height, kWidgetKey, kWidgetKey);
}

void draw_track(Handle image, int32_t height, uint8_t color) {
    Surface surface{};
    if (image_surface(image, &surface))
        (void)present::draw_clipped_line(
            &surface, kTrackX, kTrackTop, kTrackX, height - kTrackBottomInset, color
        );
}

Window*
add_scroll_button(BySlider* bar, const char* name, int32_t y, int32_t height, int32_t frame) {
    Window* self = &bar->slider.base;
    Window* button = scroll_button_create();
    window_create(button, name, self->x, y, kSliderWidth, height, self, true);
    bind_images(button, kWidgetsGaf, kScrollbarSequence, frame);
    return button;
}

/// Adds the thumb and the two arrows, resets the position, lays the bar out and leaves it disabled
/// and hidden until its list needs it.
///
/// @param self the scroll bar
void by_slider_on_bounds(Window* self) {
    BySlider* bar = as_by_slider(self);
    const bool tall = self->height > kTallSliderHeight;
    bar->thumb = add_scroll_button(
        bar,
        "sbutton",
        self->y + kArrowHeight,
        tall ? kLongThumbHeight : kShortThumbHeight,
        tall ? kLongThumbFrame : kShortThumbFrame
    );
    bar->increment = add_scroll_button(bar, "incbutton", self->y, kArrowHeight, kIncrementFrame);
    bar->decrement = add_scroll_button(
        bar, "decbutton", self->height - kArrowHeight + self->y, kArrowHeight, kDecrementFrame
    );
    slider_set_index(&bar->slider, 0);
    slider_set_range(&bar->slider, 1, false);
    self->ops->refresh(self);
    slider_disable(self);
    self->visible = 0;
}

/// Redraws the track images (dimmer while disabled), keeps the arrows at the ends of the bar and
/// re-centres the thumb.
///
/// @param self the scroll bar
/// @return true always
bool by_slider_refresh(Window* self) {
    BySlider* bar = as_by_slider(self);
    const Handle enabled = blank_window_image(self);
    window_set_state_image(self, button_state::pressed, enabled);
    window_set_state_image(self, button_state::unpressed, enabled);
    draw_track(enabled, self->height, kTrackColor);
    image_release(enabled);
    const Handle disabled = blank_window_image(self);
    window_set_state_image(self, button_state::disabled, disabled);
    draw_track(disabled, self->height, kTrackDisabledColor);
    image_release(disabled);
    if (bar->increment != nullptr)
        bar->increment->y = self->y;
    if (bar->decrement != nullptr)
        bar->decrement->y = self->height - kArrowHeight + self->y;
    slider_set_index(&bar->slider, slider_index(&bar->slider));
    return true;
}

/// Raises the list in place of its scroll bar, which a press on the bar or its buttons raises.
///
/// The list lays out every child after its first as a row, so the bar stays its first child.
///
/// @param self the scroll bar
void by_slider_raise(Window* self) {
    Window* list = self->parent;
    if (list != nullptr)
        list->ops->raise(list);
}

WindowOps make_by_slider_ops() {
    WindowOps ops = k_slider_ops;
    ops.on_bounds = by_slider_on_bounds;
    ops.refresh = by_slider_refresh;
    ops.raise = by_slider_raise;
    return ops;
}

const WindowOps k_by_slider_ops = make_by_slider_ops();

/// Creates the scroll bar along the right edge (a pixel inside a framed list) and lays the list
/// out.
///
/// The list never highlights a selection.
///
/// @param self the list box
void by_list_on_bounds(Window* self) {
    ByListBox* list = as_by_list(self);
    int32_t y = self->y;
    int32_t height = self->height;
    list->list.no_highlight = 1;
    if (list->framed != 0) {
        y += 1;
        height -= 2;
    }
    list->slider = by_slider_create();
    window_create(
        &list->slider->slider.base,
        "slider",
        self->width - kSliderInset + self->x,
        y,
        kSliderWidth,
        height,
        self,
        true
    );
    self->ops->refresh(self);
}

/// Shows a transparent image (outlined when framed) instead of the background colour, fits the
/// scroll bar and lays the items out.
///
/// @param self the list box
/// @return the toolkit list box's refresh result
bool by_list_refresh(Window* self) {
    ByListBox* list = as_by_list(self);
    const Handle image = blank_window_image(self);
    window_set_state_image(self, kListStateNormal, image);
    window_set_state_image(self, kListStateDisabled, image);
    Rect32 bar{};
    bar.x1 = self->width - kSliderInset + self->x;
    bar.x2 = bar.x1 + kSliderWidth - 1;
    bar.y1 = self->y + 1;
    bar.y2 = self->height - 3 + bar.y1;
    if (list->framed != 0) {
        Surface surface{};
        if (image_surface(image, &surface))
            (void)present::draw_rect_outline(&surface, surface.clip, kTrackColor);
        ++bar.y1;
        --bar.y2;
    }
    image_release(image);
    if (list->slider != nullptr)
        window_set_rect(&list->slider->slider.base, bar);
    return k_list_box_ops.refresh(self);
}

/// Passes notices on to the toolkit list box, dropping `changed` reports unless the list forwards
/// them.
///
/// @param self the list box
/// @param notify control notice; null is ignored
void by_list_on_notify(Window* self, const Notify* notify) {
    if (notify != nullptr &&
        (as_by_list(self)->forward_changes != 0 || notify->code != NotifyCode::changed))
        k_list_box_ops.on_notify(self, notify);
}

WindowOps make_by_list_ops() {
    WindowOps ops = k_list_box_ops;
    ops.on_bounds = by_list_on_bounds;
    ops.on_notify = by_list_on_notify;
    ops.refresh = by_list_refresh;
    return ops;
}

const WindowOps k_by_list_ops = make_by_list_ops();

} // namespace

void function_button_fonts(const HatFonts* fonts) {
    g_button_fonts = fonts;
}

namespace {

// Text style of a label with `alignment` (0..8, rows top to bottom, columns
// left to right): an edge applies only along a given width or height.
int32_t label_style(int32_t alignment, int32_t width, int32_t height) {
    int32_t style{};
    if (width != 0) {
        if (alignment == 0 || alignment == 3 || alignment == 6)
            style |= text_style::left;
        if (alignment == 2 || alignment == 5 || alignment == 8)
            style |= text_style::right;
    }
    if (height != 0) {
        if (alignment >= 0 && alignment < 3)
            style |= text_style::top;
        if (alignment > 5 && alignment < 9)
            style |= text_style::bottom;
    }
    return style;
}

/// Draws the caption and value labels while shown.
///
/// @param self the field
/// @return true always
bool lobby_field_draw(Window* self) {
    if (window_is_shown(self))
        window_draw_children(self);
    return true;
}

WindowOps lobby_field_ops() {
    WindowOps ops = k_window_ops;
    ops.draw = lobby_field_draw;
    return ops;
}

const WindowOps k_lobby_field_ops = lobby_field_ops();

} // namespace

Window* lobby_label_create(
    Window* parent,
    int32_t x,
    int32_t y,
    const char* text,
    Handle font,
    int32_t alignment,
    int32_t anchor,
    int32_t width,
    int32_t height
) {
    int32_t measured_width{}, measured_height{};
    text_measure_string(font, text, &measured_width, &measured_height);
    const int32_t style = label_style(alignment, width, height);
    if (width == 0)
        width = measured_width;
    if (height == 0)
        height = measured_height;
    if (anchor == 1 || anchor == 3)
        x -= width - 1;
    if (anchor == 2 || anchor == 3)
        y -= height - 1;
    auto* record = static_cast<LobbyLabel*>(std::calloc(1, sizeof(LobbyLabel)));
    if (record == nullptr)
        std::abort();
    Window* label = &record->base;
    window_init(label, &k_static_ops);
    label->focusable = 0;
    window_set_bounds(label, parent->x + x, parent->y + y, width, height, parent, true);
    record->style = style;
    label->ops->set_caption(label, text, style);
    window_set_font_handle(label, font);
    return label;
}

void lobby_label_set_text(Window* label, const char* text) {
    label->ops->set_caption(label, text, reinterpret_cast<LobbyLabel*>(label)->style);
}

LobbyField* lobby_field_create(
    Window* parent,
    int32_t x,
    int32_t y,
    int32_t w,
    int32_t h,
    const char* caption,
    const HatFonts* fonts,
    const char* value
) {
    auto* field = static_cast<LobbyField*>(std::calloc(1, sizeof(LobbyField)));
    if (field == nullptr)
        std::abort();
    Window* self = &field->base;
    window_init(self, &k_lobby_field_ops);
    window_set_bounds(self, parent->x + x, parent->y + y, w, h, parent, true);
    int32_t value_x{};
    if (caption != nullptr) {
        int32_t caption_height{};
        text_measure_string(fonts->small_dim, caption, &value_x, &caption_height);
        (void)lobby_label_create(
            self, 0, 0, caption, fonts->small_dim, kLobbyFieldAlignment, 0, value_x, h
        );
        value_x += kLobbyFieldGap;
    }
    if (value != nullptr && value_x < w) {
        field->value = lobby_label_create(
            self, value_x, 0, value, fonts->small, kLobbyFieldAlignment, 0, w - value_x, h
        );
    }
    return field;
}

void lobby_field_set_text(LobbyField* field, const char* text) {
    if (field->value != nullptr)
        lobby_label_set_text(field->value, text);
}

void lobby_field_set_count(LobbyField* field, uint32_t count) {
    if (field->value == nullptr)
        return;
    char text[kCountLabelCapacity]{};
    format_signed_label(text, sizeof text, count);
    lobby_field_set_text(field, text);
}

LobbyRadio*
lobby_radio_create(Window* parent, int32_t x, int32_t y, const char* text, Handle font) {
    int32_t width{}, height{};
    text_measure_string(font, text, &width, &height);
    auto* radio = static_cast<LobbyRadio*>(std::calloc(1, sizeof(LobbyRadio)));
    if (radio == nullptr)
        std::abort();
    Window* self = &radio->base;
    // The RadioButton base: three button states, unpressed.
    window_init(self, &k_lobby_radio_ops);
    window_set_state_count(self, button_state::count);
    window_set_state(self, button_state::unpressed);
    window_create(
        self, "BYRadioButton", parent->x + x, parent->y + y, width + 0xb, height, parent, true
    );
    self->ops->set_caption(self, text, 5);
    window_set_state_text(self, 0, text, 5);
    window_set_font_handle(self, font);
    window_set_name(self, text);
    bind_images(self, kWidgetsGaf, "Radio_Button", 0);
    return radio;
}

CheckBox*
lobby_check_box_create(Window* parent, int32_t x, int32_t y, const char* text, Handle font) {
    int32_t width{}, height{};
    text_measure_string(font, text, &width, &height);
    CheckBox* box = check_box_create();
    Window* self = &box->base;
    window_create(
        self,
        "BYCheckBox",
        parent->x + x,
        parent->y + y,
        width + kCheckBoxPadding,
        height < kCheckBoxMinHeight ? kCheckBoxMinHeight : height,
        parent,
        true
    );
    box->label_gap = kCheckBoxLabelGap;
    self->ops->set_caption(self, text, 0);
    window_set_font_handle(self, font);
    bind_images(self, kWidgetsGaf, "Check_Box", 0);
    return box;
}

void lobby_radio_add_member(LobbyRadio* radio, Window* member) {
    if (radio->member_count < kLobbyRadioMembers)
        radio->members[radio->member_count++] = member;
}

bool list_row_draw(
    Window* row, bool highlight, const Window* selected_row, const ListRowLook& look
) {
    if (!window_is_shown(row))
        return true;
    if (highlight) {
        void (*apply)(Window*) = row == selected_row ? look.selected : look.unselected;
        if (apply != nullptr)
            apply(row);
    }
    k_static_ops.draw(row);
    window_draw_children(row);
    return true;
}

namespace {
/// Gives a selectable text row's label the light font.
///
/// @param self the row
void text_row_select(Window* self) {
    auto* row = reinterpret_cast<ByTextRow*>(self);
    if (row->selectable)
        window_set_font_handle(row->label, row->fonts->small_light);
}

/// Restores a selectable text row's label to the small font.
///
/// @param self the row
void text_row_unselect(Window* self) {
    auto* row = reinterpret_cast<ByTextRow*>(self);
    if (row->selectable)
        window_set_font_handle(row->label, row->fonts->small);
}

constexpr ListRowLook k_text_row_look{text_row_select, text_row_unselect};

bool text_row_draw(Window* self) {
    auto* row = reinterpret_cast<ByTextRow*>(self);
    return list_row_draw(self, row->selectable, row->list->selected_item, k_text_row_look);
}

/// Makes the clicked row its list's selected row and forwards the notice to the list as the row's.
///
/// @param self the row
/// @param notify control notice
void text_row_notify(Window* self, const Notify* notify) {
    auto* row = reinterpret_cast<ByTextRow*>(self);
    row->list->selected_item = self;
    Notify forwarded = *notify;
    forwarded.source = self;
    row->list->list.base.ops->on_notify(&row->list->list.base, &forwarded);
}

/// Returns the row label's current text.
///
/// @param self the row
/// @param state state asked for; not read
/// @return the text, or "" for a row without a label
const char* text_row_state_text(Window* self, int32_t /*state*/) {
    Window* label = reinterpret_cast<ByTextRow*>(self)->label;
    return label != nullptr ? label->ops->state_text(label, -1) : "";
}

WindowOps text_row_ops() {
    WindowOps ops = k_static_ops;
    ops.state_text = text_row_state_text;
    ops.draw = text_row_draw;
    ops.on_notify = text_row_notify;
    return ops;
}

const WindowOps k_text_row_ops = text_row_ops();
} // namespace

const char* by_list_selected_text(const ByListBox* list) {
    Window* row = list->selected_item;
    return row != nullptr ? row->ops->state_text(row, -1) : "";
}

int32_t by_list_row_width(const Window* list) {
    return list->width - 0x11;
}

void by_text_row_set_font(ByTextRow* row, Handle font) {
    if (row->label != nullptr)
        window_set_font_handle(row->label, font);
}

ByTextRow* by_text_row_create(
    ByListBox* list, const char* text, const HatFonts* fonts, bool selectable, int32_t indent
) {
    auto* row = static_cast<ByTextRow*>(std::calloc(1, sizeof(ByTextRow)));
    if (row == nullptr)
        std::abort();
    window_init(&row->base, &k_text_row_ops);
    row->list = list;
    row->fonts = fonts;
    row->selectable = selectable;
    Window* parent = &list->list.base;
    window_set_bounds(
        &row->base, parent->x, parent->y, by_list_row_width(parent), 0x11, parent, true
    );
    if (text != nullptr)
        row->label = lobby_label_create(
            &row->base, indent, 0, text, fonts->small, 3, 0, row->base.width - indent, 0x11
        );
    list_box_add(&list->list, &row->base);
    return row;
}

namespace {

constexpr const char* kRoomIconsGaf = "widgets.Gaf";
constexpr const char* kRoomIconSequence = "Icon_Circle";
constexpr int32_t kLabelLeftMiddle = 3;
constexpr int32_t kLabelCentred = 4;
constexpr int32_t kIconNormalState = 1;
constexpr int32_t kLatencySlotWidth = 0x11; // kept free past a player's name

// A row starts as a Static that never takes focus: a selectable row of
// `list`, not yet placed.
void list_row_init(ByTextRow* row, const WindowOps* ops, ByListBox* list, const HatFonts* fonts) {
    window_init(&row->base, ops);
    row->base.focusable = 0;
    row->list = list;
    row->fonts = fonts;
    row->selectable = true;
}

void place_list_row(ByTextRow* row) {
    Window* list = &row->list->list.base;
    window_set_bounds(
        &row->base, list->x, list->y, by_list_row_width(list), kByRowHeight, nullptr, true
    );
}

Window* add_row_icon(ByTextRow* row, const char* name, int32_t x, int32_t y) {
    Window* icon = static_create();
    window_create(icon, name, x, y, kByRowIconSize, kByRowIconSize, &row->base, true);
    return icon;
}

void label_show_count(Window* label, uint32_t count) {
    char text[kCountLabelCapacity]{};
    format_signed_label(text, sizeof text, count);
    lobby_label_set_text(label, text);
}

ByPlayerRow* as_player_row(Window* self) {
    return reinterpret_cast<ByPlayerRow*>(self);
}

ByChatRoomRow* as_chat_room_row(Window* self) {
    return reinterpret_cast<ByChatRoomRow*>(self);
}

/// Shows the selected player's name in the light font.
///
/// @param self the player row
void player_row_select(Window* self) {
    const ByPlayerRow* player = as_player_row(self);
    window_set_font_handle(player->name_label, player->row.fonts->small_light);
}

/// Shows an unselected player's name: the system colour while whispers are locked onto the player,
/// the mode's bad-health colour while muzzled, else the small font. Off keeps today's green and
/// dim tints.
///
/// @param self the player row
void player_row_unselect(Window* self) {
    const ByPlayerRow* player = as_player_row(self);
    const HatFonts* fonts = player->row.fonts;
    Handle font = fonts->small;
    if (player->whisper_locked)
        font = fonts->small_green;
    else if (player->silenced) {
        font = fonts->small_dim;
        if (present::colour_vision::active_mode() != present::colour_vision::Mode::off &&
            g_muzzle_font >= 0)
            font = g_muzzle_font;
    }
    window_set_font_handle(player->name_label, font);
}

constexpr ListRowLook k_player_row_look{player_row_select, player_row_unselect};

// Moves the name right while a lock or mute mark needs the gap, and puts it back otherwise.
// The latency icon stays where the row was built, at the name's old right edge.
void place_name_mark(ByPlayerRow* player) {
    Window* label = player->name_label;
    Window* latency = player->icons.latency_icon;
    if (label == nullptr || latency == nullptr)
        return;
    const int32_t home = player->row.base.x + kByRowNameX;
    const int32_t full = latency->x - home;
    const bool mark =
        present::colour_vision::shape_cues_active() && (player->whisper_locked || player->silenced);
    const int32_t room = mark && full > kNameMarkRoom ? kNameMarkRoom : 0;
    const int32_t left = home + room;
    const int32_t width = full > room ? full - room : full;
    if (label->x != left)
        label->x = left;
    if (width > 0 && label->width != width)
        label->width = width;
}

void stamp_name_mark(ByPlayerRow* player) {
    if (!present::colour_vision::shape_cues_active())
        return;
    NameMark mark = NameMark::none;
    if (player->whisper_locked)
        mark = NameMark::lock;
    else if (player->silenced)
        mark = NameMark::mute;
    if (mark == NameMark::none)
        return;
    Handle& slot = mark == NameMark::lock ? g_lock_mark : g_mute_mark;
    if (slot < 0)
        slot = image_create_blank(kNameMarkSide, kNameMarkSide, kWidgetKey, kWidgetKey);
    Surface surface{};
    if (slot < 0 || !image_surface(slot, &surface))
        return;
    paint_name_mark(
        surface.pixels, surface.width, surface.height, surface.pitch, mark, kCueInk, kWidgetKey
    );
    image_draw(
        player->row.base.x + kByRowNameX, player->row.base.y + 2, kNameMarkSide, kNameMarkSide, slot
    );
}

bool player_row_draw(Window* self) {
    ByPlayerRow* player = as_player_row(self);
    paint_latency_icon(&player->icons, false);
    place_name_mark(player);
    const ByTextRow* row = &player->row;
    const bool drawn =
        list_row_draw(self, row->selectable, row->list->selected_item, k_player_row_look);
    stamp_name_mark(player);
    return drawn;
}

void player_row_destroy(Window* self) {
    if (self != nullptr)
        g_latency_applied.erase(as_player_row(self)->icons.latency_icon);
    if (g_player_row_base_destroy != nullptr)
        g_player_row_base_destroy(self);
}

bool player_row_is_class(const char* name) {
    return is_list_item_class(ListItemClass::player, name);
}

WindowOps player_row_ops() {
    WindowOps ops = k_static_ops;
    g_player_row_base_destroy = ops.destroy;
    ops.class_name = kByPlayerRowClass;
    ops.is_class = player_row_is_class;
    ops.draw = player_row_draw;
    ops.destroy = player_row_destroy;
    ops.on_notify = text_row_notify;
    return ops;
}

const WindowOps k_player_row_ops = player_row_ops();

/// Shows the selected frequency's name and count in the light font.
///
/// @param self the frequency row
void chat_room_row_select(Window* self) {
    const ByChatRoomRow* room = as_chat_room_row(self);
    window_set_font_handle(room->name_label, room->row.fonts->small_light);
    window_set_font_handle(room->count_label, room->row.fonts->small_light);
}

/// Shows an unselected frequency's name and count in the small font.
///
/// @param self the frequency row
void chat_room_row_unselect(Window* self) {
    const ByChatRoomRow* room = as_chat_room_row(self);
    window_set_font_handle(room->name_label, room->row.fonts->small);
    window_set_font_handle(room->count_label, room->row.fonts->small);
}

constexpr ListRowLook k_chat_room_row_look{chat_room_row_select, chat_room_row_unselect};

/// Circles the session's current frequency, then draws the row.
///
/// @param self the frequency row
/// @return true always
bool chat_room_row_draw(Window* self) {
    const ByChatRoomRow* room = as_chat_room_row(self);
    if (window_is_shown(self)) {
        const bool current = room->current_room != nullptr && *room->current_room == room->room_id;
        window_set_state_image(
            room->icon, kIconNormalState, current ? room->current_icon : room->other_icon
        );
    }
    return list_row_draw(
        self, room->row.selectable, room->row.list->selected_item, k_chat_room_row_look
    );
}

/// Releases the two Icon_Circle frames and frees the row.
///
/// @param self the frequency row
void chat_room_row_destroy(Window* self) {
    ByChatRoomRow* room = as_chat_room_row(self);
    if (room->current_icon != k_no_handle)
        image_release(room->current_icon);
    if (room->other_icon != k_no_handle)
        image_release(room->other_icon);
    window_release(self);
    std::free(room);
}

bool chat_room_row_is_class(const char* name) {
    return is_list_item_class(ListItemClass::chat_room, name);
}

WindowOps chat_room_row_ops() {
    WindowOps ops = k_static_ops;
    ops.class_name = kByChatRoomRowClass;
    ops.destroy = chat_room_row_destroy;
    ops.is_class = chat_room_row_is_class;
    ops.draw = chat_room_row_draw;
    ops.on_notify = text_row_notify;
    return ops;
}

const WindowOps k_chat_room_row_ops = chat_room_row_ops();

// The list child a new frequency row goes before: past the slider, ahead of
// the first battle debriefing for a user-created frequency, ahead of the
// first row that is not a planetary default or does not sort before it for
// a planetary default; debriefings and other types go last.
int32_t chat_room_row_position(ByListBox* list, int32_t type, const char* name) {
    Window* self = &list->list.base;
    if (type == room_type::battle_debriefing)
        return -1;
    int32_t i = window_find_named(self, "slider", class_name::slider) != nullptr ? 1 : 0;
    for (; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child->ops != &k_chat_room_row_ops)
            continue;
        const ByChatRoomRow* other = as_chat_room_row(child);
        if (type == room_type::user_created) {
            if (other->type == room_type::battle_debriefing)
                break;
        } else if (type == room_type::planetary_default) {
            if (other->type != room_type::planetary_default ||
                compare_word_sort(name, other->name) <= 0)
                break;
        }
    }
    return i;
}

void copy_field(char* out, size_t capacity, const char* text) {
    utf8::copy(out, capacity, text, 0);
}

} // namespace

ByPlayerRow*
by_player_row_create(ByListBox* list, const char* name, uint32_t user_id, const HatFonts* fonts) {
    auto* player = static_cast<ByPlayerRow*>(std::calloc(1, sizeof(ByPlayerRow)));
    if (player == nullptr)
        std::abort();
    ByTextRow* row = &player->row;
    list_row_init(row, &k_player_row_ops, list, fonts);
    player->user_id = user_id;
    copy_field(player->name, sizeof player->name, name);
    place_list_row(row);
    Window* self = &row->base;
    player->icons.user_icon = add_row_icon(row, "RankIcon", self->x + 3, self->y + 3);
    player_row_set_user_icon(&player->icons, kByPlayerRowFirstUserIcon);
    player->name_label = lobby_label_create(
        self,
        kByRowNameX,
        0,
        player->name,
        fonts->small,
        kLabelLeftMiddle,
        0,
        by_list_row_width(&list->list.base) - kByRowNameX - kLatencySlotWidth,
        kByRowHeight
    );
    Window* label = player->name_label;
    player->icons.latency_icon =
        add_row_icon(row, "LatencyIcon", label->x + label->width, self->y + 4);
    player_row_set_latency(&player->icons, kByPlayerRowFirstLatency);
    list_box_add(&list->list, self);
    return player;
}

ByChatRoomRow* by_chat_room_row_create(
    ByListBox* list,
    const char* name,
    const char* description,
    const char* tag,
    uint32_t room_id,
    bool needs_password,
    int32_t type,
    const HatFonts* fonts,
    const uint32_t* current_room
) {
    auto* room = static_cast<ByChatRoomRow*>(std::calloc(1, sizeof(ByChatRoomRow)));
    if (room == nullptr)
        std::abort();
    ByTextRow* row = &room->row;
    list_row_init(row, &k_chat_room_row_ops, list, fonts);
    room->current_icon = k_no_handle;
    room->other_icon = k_no_handle;
    room->room_id = room_id;
    room->needs_password = needs_password;
    room->type = type;
    room->current_room = current_room;
    if (type == room_type::unlisted)
        return room;
    utf8::copy(room->name, sizeof room->name, name, kRoomNameCharacters);
    utf8::copy(
        room->description, sizeof room->description, description, kRoomDescriptionCharacters
    );
    copy_field(room->tag, sizeof room->tag, tag);
    place_list_row(row);
    Window* self = &row->base;
    room->current_icon =
        image_load(kRoomIconsGaf, kRoomIconSequence, kByRoomCurrentFrame, kWidgetKey);
    room->other_icon = image_load(kRoomIconsGaf, kRoomIconSequence, kByRoomOtherFrame, kWidgetKey);
    room->icon = add_row_icon(
        row, "FrequencyIcon", self->x + 3, (self->height - kByRowIconSize) / 2 + self->y
    );
    window_set_state_image(room->icon, kIconNormalState, room->other_icon);
    room->name_label = lobby_label_create(
        self,
        kByRowNameX,
        0,
        room->name,
        fonts->small,
        kLabelLeftMiddle,
        0,
        by_list_row_width(&list->list.base) - kByRowNameX - kByRoomCountWidth,
        kByRowHeight
    );
    room->count_label = lobby_label_create(
        self,
        self->width - kByRoomCountWidth,
        0,
        "",
        fonts->small,
        kLabelCentred,
        0,
        kByRoomCountWidth,
        kByRowHeight
    );
    label_show_count(room->count_label, static_cast<uint32_t>(room->users));
    list_box_insert(&list->list, self, chat_room_row_position(list, type, room->name));
    return room;
}

void by_chat_room_row_set_count(ByChatRoomRow* row, uint32_t users) {
    row->users = static_cast<int32_t>(users);
    if (row->count_label != nullptr)
        label_show_count(row->count_label, users);
}

void by_chat_room_row_add_count(ByChatRoomRow* row, int32_t delta) {
    by_chat_room_row_set_count(
        row, static_cast<uint32_t>(row->users) + static_cast<uint32_t>(delta)
    );
}

namespace {

ByCircleRow* as_circle_row(Window* self) {
    return reinterpret_cast<ByCircleRow*>(self);
}

/// Shows the selected category's circle frame and its name in the light font.
///
/// @param self the category row
void circle_row_select(Window* self) {
    const ByCircleRow* circle = as_circle_row(self);
    window_set_font_handle(circle->name_label, circle->row.fonts->small_light);
    window_set_state_image(circle->icon, kIconNormalState, circle->selected_icon);
}

/// Shows an unselected category's circle frame and its name in the small font.
///
/// @param self the category row
void circle_row_unselect(Window* self) {
    const ByCircleRow* circle = as_circle_row(self);
    window_set_font_handle(circle->name_label, circle->row.fonts->small);
    window_set_state_image(circle->icon, kIconNormalState, circle->other_icon);
}

constexpr ListRowLook k_circle_row_look{circle_row_select, circle_row_unselect};

bool circle_row_draw(Window* self) {
    const ByTextRow* row = &as_circle_row(self)->row;
    return list_row_draw(self, row->selectable, row->list->selected_item, k_circle_row_look);
}

/// Releases the two Icon_Circle frames and frees the row.
///
/// @param self the category row
void circle_row_destroy(Window* self) {
    ByCircleRow* circle = as_circle_row(self);
    if (circle->selected_icon != k_no_handle)
        image_release(circle->selected_icon);
    if (circle->other_icon != k_no_handle)
        image_release(circle->other_icon);
    window_release(self);
    std::free(circle);
}

/// Tells whether a class name is BYCircleLstItem.
///
/// @param name class name; null never matches
/// @return true on a match
bool circle_row_is_class(const char* name) {
    return is_list_item_class(ListItemClass::circle, name);
}

WindowOps circle_row_ops() {
    WindowOps ops = k_static_ops;
    ops.class_name = kByCircleRowClass;
    ops.destroy = circle_row_destroy;
    ops.is_class = circle_row_is_class;
    ops.draw = circle_row_draw;
    ops.on_notify = text_row_notify;
    return ops;
}

const WindowOps k_circle_row_ops = circle_row_ops();

} // namespace

ByCircleRow*
by_circle_row_create(ByListBox* list, const char* name, int32_t tab, const HatFonts* fonts) {
    auto* circle = static_cast<ByCircleRow*>(std::calloc(1, sizeof(ByCircleRow)));
    if (circle == nullptr)
        std::abort();
    ByTextRow* row = &circle->row;
    list_row_init(row, &k_circle_row_ops, list, fonts);
    circle->tab = tab;
    place_list_row(row);
    Window* self = &row->base;
    circle->selected_icon =
        image_load(kRoomIconsGaf, kRoomIconSequence, kByCircleSelectedFrame, kWidgetKey);
    circle->other_icon =
        image_load(kRoomIconsGaf, kRoomIconSequence, kByCircleOtherFrame, kWidgetKey);
    circle->icon = add_row_icon(
        row, "ProfileCategoryIcon", self->x + 3, (self->height - kByRowIconSize) / 2 + self->y
    );
    window_set_state_image(circle->icon, kIconNormalState, circle->other_icon);
    circle->name_label = lobby_label_create(
        self,
        kByRowNameX,
        0,
        name,
        fonts->small,
        kLabelLeftMiddle,
        0,
        by_list_row_width(&list->list.base) - kByRowNameX,
        kByRowHeight
    );
    list_box_add(&list->list, self);
    return circle;
}

namespace {

ByTwoLabelRow* as_two_label_row(Window* self) {
    return reinterpret_cast<ByTwoLabelRow*>(self);
}

// The two-label row follows its list's selection without a look of its own.
constexpr ListRowLook k_two_label_row_look{};

bool two_label_row_draw(Window* self) {
    const ByTextRow* row = &as_two_label_row(self)->row;
    return list_row_draw(self, row->selectable, row->list->selected_item, k_two_label_row_look);
}

WindowOps two_label_row_ops() {
    WindowOps ops = k_static_ops;
    ops.draw = two_label_row_draw;
    ops.on_notify = text_row_notify;
    return ops;
}

const WindowOps k_two_label_row_ops = two_label_row_ops();

constexpr int32_t kTwoLabelInset = 3;
constexpr int32_t kTwoLabelIndent = 0x11;
constexpr int32_t kTwoLabelGap = 3;

} // namespace

ByTwoLabelRow* by_two_label_row_create(
    ByListBox* list, const char* first, const char* second, const HatFonts* fonts, bool indent
) {
    auto* two = static_cast<ByTwoLabelRow*>(std::calloc(1, sizeof(ByTwoLabelRow)));
    if (two == nullptr)
        std::abort();
    ByTextRow* row = &two->row;
    list_row_init(row, &k_two_label_row_ops, list, fonts);
    place_list_row(row);
    Window* self = &row->base;
    int32_t x = indent ? kTwoLabelIndent : kTwoLabelInset;
    if (first != nullptr) {
        int32_t width{}, height{};
        text_measure_string(fonts->small_dim, first, &width, &height);
        two->first_label = lobby_label_create(
            self, x, 0, first, fonts->small_dim, kLabelLeftMiddle, 0, width, kByRowHeight
        );
        x += width + kTwoLabelGap;
    }
    if (second != nullptr)
        two->second_label = lobby_label_create(
            self,
            x,
            0,
            second,
            fonts->small,
            kLabelLeftMiddle,
            0,
            by_list_row_width(&list->list.base) - x,
            kByRowHeight
        );
    list_box_add(&list->list, self);
    return two;
}

namespace {

constexpr const char* kRadioGroupName = "RadioGroup";
// The buttons sit this far below the group's top, and this far apart.
constexpr int32_t kRadioButtonsY = 0xf;
constexpr int32_t kRadioButtonGap = 5;
// A radio button is its caption and the radio face this wide.
constexpr int32_t kRadioFaceWidth = 0xb;

ByRadioGroup* as_radio_group(Window* self) {
    return reinterpret_cast<ByRadioGroup*>(self);
}

void set_member_value(LobbyRadio* member, int32_t value) {
    window_set_state(&member->base, value);
}

/// Handles a member picked: GREEN_BUTTON, the member becomes the selection, and the group's parent
/// hears code 200 with its index.
///
/// Notices from windows that are not members are ignored.
///
/// @param self the radio group
/// @param notify control notice; null is ignored
void radio_group_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr)
        return;
    ByRadioGroup* group = as_radio_group(self);
    if (group->play_sound != nullptr)
        group->play_sound(group->sound_user, kByRadioGroupClick);
    for (int32_t i = 0; i < group->count; ++i) {
        if (notify->source != &group->members[i]->base)
            continue;
        group->selected = i;
        by_radio_group_select(group, &group->members[i]->base);
        if (self->parent != nullptr) {
            Notify forwarded{static_cast<NotifyCode>(kByRadioGroupNotify), self, i, nullptr};
            self->parent->ops->on_notify(self->parent, &forwarded);
        }
        return;
    }
}

/// Draws the caption and the buttons while the group is shown.
///
/// @param self the radio group
/// @return true always
bool radio_group_draw(Window* self) {
    if (window_is_shown(self))
        window_draw_children(self);
    return true;
}

WindowOps radio_group_ops() {
    WindowOps ops = k_static_ops;
    ops.on_notify = radio_group_on_notify;
    ops.draw = radio_group_draw;
    return ops;
}

const WindowOps k_radio_group_ops = radio_group_ops();

} // namespace

ByRadioGroup* by_radio_group_create(
    Window* parent,
    int32_t x,
    int32_t y,
    const char* caption,
    const char* const* names,
    int32_t count,
    const HatFonts* fonts
) {
    auto* group = static_cast<ByRadioGroup*>(std::calloc(1, sizeof(ByRadioGroup)));
    if (group == nullptr)
        std::abort();
    if (count > kLobbyRadioMembers)
        count = kLobbyRadioMembers;
    if (count < 0)
        count = 0;
    Window* self = &group->base;
    window_init(self, &k_radio_group_ops);
    group->selected = -1;
    int32_t offsets[kLobbyRadioMembers]{};
    int32_t width = 0;
    for (int32_t i = 0; i < count; ++i) {
        offsets[i] = width;
        int32_t name_width{}, name_height{};
        text_measure_string(fonts->small, names[i], &name_width, &name_height);
        width += kRadioFaceWidth + name_width + kRadioButtonGap;
    }
    if (count > 0)
        width -= kRadioButtonGap;
    window_create(
        self,
        kRadioGroupName,
        parent->x + x,
        parent->y + y,
        width,
        kByRadioGroupHeight,
        parent,
        true
    );
    (void)lobby_label_create(self, 0, 0, caption, fonts->medium, 0, 0);
    for (int32_t i = 0; i < count; ++i)
        group->members[i] =
            lobby_radio_create(self, offsets[i], kRadioButtonsY, names[i], fonts->small);
    group->count = count;
    for (int32_t i = 0; i < count; ++i)
        for (int32_t j = 0; j < count; ++j)
            if (j != i)
                lobby_radio_add_member(group->members[i], &group->members[j]->base);
    if (count > 0)
        by_radio_group_select(group, &group->members[0]->base);
    return group;
}

void by_radio_group_select(ByRadioGroup* group, const Window* member) {
    group->selected = -1;
    for (int32_t i = 0; i < group->count; ++i) {
        LobbyRadio* radio = group->members[i];
        if (&radio->base == member) {
            group->selected = i;
            set_member_value(radio, widget_value::selected);
        } else if (radio->base.states.current != widget_value::disabled) {
            set_member_value(radio, widget_value::enabled);
        }
    }
}

void by_radio_group_set_index(ByRadioGroup* group, int32_t index) {
    group->selected = index;
    if (index >= 0 && index < group->count)
        by_radio_group_select(group, &group->members[index]->base);
    else
        by_radio_group_select(group, nullptr);
}

void by_radio_group_select_name(ByRadioGroup* group, const char* name) {
    for (int32_t i = 0; i < group->count; ++i) {
        if (group->members[i]->base.name != nullptr &&
            std::strcmp(group->members[i]->base.name, name) == 0) {
            by_radio_group_set_index(group, i);
            return;
        }
    }
}

void by_radio_group_refresh(ByRadioGroup* group) {
    for (int32_t i = 0; i < group->count; ++i)
        set_member_value(
            group->members[i], i == group->selected ? widget_value::selected : widget_value::enabled
        );
}

void by_radio_group_disable_name(ByRadioGroup* group, const char* name) {
    for (int32_t i = 0; i < group->count; ++i) {
        if (group->members[i]->base.name != nullptr &&
            std::strcmp(group->members[i]->base.name, name) == 0) {
            set_member_value(group->members[i], widget_value::disabled);
            return;
        }
    }
}

void by_radio_group_disable_others(ByRadioGroup* group) {
    for (int32_t i = 0; i < group->count; ++i)
        if (i != group->selected)
            set_member_value(group->members[i], widget_value::disabled);
}

const char* by_radio_group_selected_name(const ByRadioGroup* group) {
    if (group->selected < 0 || group->selected >= group->count ||
        group->members[group->selected]->base.name == nullptr)
        return "";
    return group->members[group->selected]->base.name;
}

void by_radio_group_show(ByRadioGroup* group, const RadioGroup& model) {
    group->selected = model.selected;
    for (int32_t i = 0; i < group->count && i < model.count; ++i)
        set_member_value(group->members[i], model.members[i].value);
}

void by_radio_group_read(const ByRadioGroup* group, RadioGroup* model) {
    model->selected = group->selected;
    for (int32_t i = 0; i < group->count && i < model->count; ++i)
        model->members[i].value = group->members[i]->base.states.current;
}

namespace {

// An ideograph, kana or hangul syllable may end a line. Fullwidth Latin stays
// with the word around it.
bool breaks_anywhere(char32_t code) {
    if (code >= 0x1100 && code <= 0x11FF) {
        return true;
    }
    if (code >= 0x3040 && code <= 0x30FF) {
        return true;
    }
    if (code >= 0x31F0 && code <= 0x31FF) {
        return true;
    }
    if (code >= 0x3400 && code <= 0x4DBF) {
        return true;
    }
    if (code >= 0x4E00 && code <= 0x9FFF) {
        return true;
    }
    if (code >= 0xAC00 && code <= 0xD7AF) {
        return true;
    }
    return code >= 0xF900 && code <= 0xFAFF;
}

} // namespace

int32_t wrapped_line_length(const char* text, Handle font, int32_t available) {
    int32_t count = 0;
    int32_t width = 0;
    int32_t last_break = 0;
    while (text[count] != '\0' && text[count] != '\n') {
        const utf8::Unit unit = utf8::read(text + count);
        const int32_t step = unit.bytes > 0 ? unit.bytes : 1;
        char glyph[5]{};
        std::memcpy(glyph, text + count, static_cast<std::size_t>(step));
        int32_t next = 0;
        int32_t height = 0;
        text_measure_string(font, glyph, &next, &height);
        width += next;
        if (width > available) {
            return last_break != 0 ? last_break : count;
        }
        const char32_t code =
            unit.well_formed ? unit.code : static_cast<unsigned char>(text[count]);
        if (step == 1 && (text[count] == ' ' || text[count] == '\t' || text[count] == '-' ||
                          text[count] == '/' || text[count] == '\\')) {
            last_break = count;
        } else if (breaks_anywhere(code)) {
            last_break = count + step;
        }
        count += step;
    }
    return count;
}

void by_list_wrap_text(
    ByListBox* list, const char* text, const HatFonts* fonts, int32_t indent, Handle font
) {
    const bool at_end = list->list.last == list->list.count - 1;
    if (font == k_no_handle)
        font = fonts->small;
    int32_t continuation{}, height{};
    char padding[32]{};
    const char* filler = "          ";
    const int spaces = indent > 0 && indent < 32 ? indent - 1 : 0;
    std::snprintf(padding, sizeof padding, "%.*s", spaces, filler);
    text_measure_string(font, padding, &continuation, &height);
    bool first = true;
    while (*text != '\0') {
        while (*text == ' ' || *text == '\t' || *text == '\n')
            ++text;
        if (*text == '\0')
            break;
        const int32_t available = by_list_row_width(&list->list.base) - (first ? 4 : continuation);
        const int32_t count = wrapped_line_length(text, font, available);
        if (count == 0) {
            const utf8::Unit unit = utf8::read(text);
            text += unit.bytes > 0 ? unit.bytes : 1;
            continue;
        }
        auto* line = static_cast<char*>(std::malloc(static_cast<std::size_t>(count) + 1));
        if (line == nullptr)
            return;
        std::memcpy(line, text, static_cast<std::size_t>(count));
        line[count] = '\0';
        by_text_row_set_font(
            by_text_row_create(list, line, fonts, false, first ? 4 : continuation), font
        );
        std::free(line);
        text += count;
        first = false;
    }
    if (at_end)
        list_box_scroll_to_end(&list->list);
}

BySlider* by_slider_create() {
    auto* bar = static_cast<BySlider*>(std::calloc(1, sizeof(BySlider)));
    if (bar == nullptr)
        std::abort();
    slider_init(&bar->slider, &k_by_slider_ops);
    return bar;
}

ByListBox* by_list_box_create(bool framed, bool forward_changes) {
    auto* list = static_cast<ByListBox*>(std::calloc(1, sizeof(ByListBox)));
    if (list == nullptr)
        std::abort();
    list_box_init(&list->list, &k_by_list_ops);
    list->framed = framed ? 1 : 0;
    list->forward_changes = forward_changes ? 1 : 0;
    return list;
}

void by_list_box_clear(ByListBox* list) {
    list->selected_item = nullptr;
    list_box_clear(&list->list);
}

void function_button_setup(
    Window* button, bool small, int32_t dx, int32_t dy, const char* caption, Window* parent
) {
    button->ops = &k_function_button_ops;
    const int32_t width = small ? function_button::small_width : function_button::large_width;
    window_set_bounds(
        button, parent->x + dx, parent->y + dy, width, function_button::height, parent, true
    );
    window_set_state_count(button, function_button::state_count);
    window_set_state(button, 0);
    bind_images(button, kWidgetsGaf, small ? "Function_Button_Small" : "Function_Button_Large", 0);
    window_set_four_state_texts(button, caption);
}

SingleEdit* lobby_edit_create(
    Window* parent,
    int32_t dx,
    int32_t dy,
    int32_t width,
    Handle font,
    int32_t max_length,
    bool framed
) {
    SingleEdit* edit = single_edit_create();
    edit->base.ops = lobby_edit_ops();
    window_set_bounds(
        &edit->base, parent->x + dx, parent->y + dy, width, kLobbyEditHeight, parent, true
    );
    if (edit->base.font >= 0)
        font_release(edit->base.font);
    font_add_ref(font);
    edit->base.font = font;
    if (framed) {
        const Handle image = blank_window_image(&edit->base);
        Surface surface{};
        if (image_surface(image, &surface))
            (void)present::draw_rect_outline(&surface, surface.clip, kTrackColor);
        window_set_state_image(&edit->base, 0, image);
        window_set_state_image(&edit->base, 1, image);
        image_release(image);
    }
    single_edit_set_max_length(edit, max_length);
    single_edit_set_selection_height(edit, kLobbyEditSelectionHeight);
    return edit;
}

const WindowOps* lobby_edit_ops() {
    static const WindowOps ops = [] {
        WindowOps o = k_single_edit_ops;
        o.set_caption = lobby_edit_set_caption;
        return o;
    }();
    return &ops;
}

void window_set_four_state_texts(Window* window, const char* text) {
    const char* face = shown_text(text != nullptr ? text : "");
    for (int32_t state = 0; state < function_button::state_count; ++state)
        window_set_state_text(window, state, face, 0);
}

void player_row_set_latency(PlayerRowIcons* row, int32_t frame) {
    if (row == nullptr)
        return;
    row->latency_frame = frame;
    paint_latency_icon(row, true);
}

void player_row_set_user_icon(PlayerRowIcons* row, int32_t frame) {
    row->user_frame = frame;
    bind_images(row->user_icon, "Widgets.Gaf", "User_Icons", frame);
}

const char* latency_grade_phrase(int32_t grade) noexcept {
    // The drawn square runs from 1 (red, the worst) to 3 (green, the best).
    if (grade == 1)
        return "Latency poor";
    if (grade == 2)
        return "Latency fair";
    if (grade == 3)
        return "Latency good";
    return "Latency unknown";
}

bool player_icon_words(const Window* icon, PlayerIconWords* out) {
    if (out != nullptr)
        *out = {};
    if (icon == nullptr || out == nullptr || icon->name == nullptr || icon->parent == nullptr)
        return false;
    const bool rank = std::strcmp(icon->name, "RankIcon") == 0;
    const bool latency = std::strcmp(icon->name, "LatencyIcon") == 0;
    if (!rank && !latency)
        return false;
    const Window* row_window = icon->parent;
    if (row_window->ops == nullptr || row_window->ops->class_name == nullptr ||
        std::strcmp(row_window->ops->class_name, kByPlayerRowClass) != 0)
        return false;
    const auto* player = reinterpret_cast<const ByPlayerRow*>(row_window);
    if (rank && icon != player->icons.user_icon)
        return false;
    if (latency && icon != player->icons.latency_icon)
        return false;
    if (rank) {
        out->kind = PlayerIconKind::rank;
        const char* title = rank_title(player->icons.user_frame, true);
        out->rank = title != nullptr ? title : "";
        return true;
    }
    out->kind = PlayerIconKind::latency;
    const int32_t grade = player->icons.latency_frame;
    // The drawn square runs from 1 (red, the worst) to 3 (green, the best).
    // Anything else has no name, and the picture is left as the row drew it.
    out->latency_grade = grade >= 1 && grade <= 3 ? grade : 0;
    return true;
}

namespace {

constexpr std::size_t kPaletteBytes = 256U * 4U;
constexpr double kNearGrey = 0.05;
constexpr double kHueStep = 0.07;
constexpr int kRowWidth = 16;

struct Hue {
    double hue = 0.0;
    double saturation = 0.0;
};

// Hue and saturation of one palette entry, on the same scale the colour-vision ramps use.
Hue hue_of(present::colour_vision::Rgb colour) {
    const double red = static_cast<double>(colour.red) / 255.0;
    const double green = static_cast<double>(colour.green) / 255.0;
    const double blue = static_cast<double>(colour.blue) / 255.0;
    const double upper = std::max(red, std::max(green, blue));
    const double lower = std::min(red, std::min(green, blue));
    Hue result;
    result.saturation = upper == 0.0 ? 0.0 : (upper - lower) / upper;
    const double span = upper - lower;
    if (span == 0.0)
        return result;
    double sector = 0.0;
    if (upper == red)
        sector = (green - blue) / span;
    else if (upper == green)
        sector = (blue - red) / span + 2.0;
    else
        sector = (red - green) / span + 4.0;
    if (sector < 0.0)
        sector += 6.0;
    result.hue = sector / 6.0;
    return result;
}

bool ramp_continues(Hue left, Hue right) {
    if (left.saturation < kNearGrey && right.saturation < kNearGrey)
        return true;
    double distance = std::abs(left.hue - right.hue);
    if (distance > 0.5)
        distance = 1.0 - distance;
    return distance <= kHueStep;
}

bool palette_ready(std::span<const uint8_t> palette) {
    return palette.size() >= kPaletteBytes;
}

present::colour_vision::Rgb entry_rgb(std::span<const uint8_t> palette, int index) {
    const auto at = static_cast<std::size_t>(index) * 4U;
    return present::colour_vision::Rgb{palette[at], palette[at + 1U], palette[at + 2U]};
}

std::array<uint8_t, 256> identity_map() {
    std::array<uint8_t, 256> map{};
    for (int index = 0; index < 256; ++index)
        map[static_cast<std::size_t>(index)] = static_cast<uint8_t>(index);
    return map;
}

struct ShadeRamp {
    int left = 0;
    int right = 0;
    std::array<double, 256> lightness{};
};

ShadeRamp ramp_around(std::span<const uint8_t> palette, int anchor) {
    ShadeRamp ramp;
    std::array<Hue, 256> hue{};
    for (int index = 0; index < 256; ++index) {
        const present::colour_vision::Rgb colour = entry_rgb(palette, index);
        hue[static_cast<std::size_t>(index)] = hue_of(colour);
        ramp.lightness[static_cast<std::size_t>(index)] =
            present::colour_vision::lab_from_srgb(colour).l;
    }
    const int row = anchor / kRowWidth * kRowWidth;
    int left = anchor;
    while (left > row &&
           ramp_continues(
               hue[static_cast<std::size_t>(left)], hue[static_cast<std::size_t>(left - 1)]
           ))
        --left;
    int right = anchor;
    while (right + 1 < row + kRowWidth &&
           ramp_continues(
               hue[static_cast<std::size_t>(right)], hue[static_cast<std::size_t>(right + 1)]
           ))
        ++right;
    ramp.left = left;
    ramp.right = right;
    return ramp;
}

// The entry of the ramp nearest `want` in lightness. The lower index wins a tie.
int nearest_lightness(const ShadeRamp& ramp, double want) {
    int destination = ramp.left;
    double best = std::abs(want - ramp.lightness[static_cast<std::size_t>(ramp.left)]);
    for (int index = ramp.left + 1; index <= ramp.right; ++index) {
        const double distance = std::abs(want - ramp.lightness[static_cast<std::size_t>(index)]);
        if (distance < best) {
            best = distance;
            destination = index;
        }
    }
    return destination;
}

// Sends each source shade to the destination shade nearest its lightness. The source nearest the
// mode's colour, the lower index on a tie, is that colour exactly.
void map_sources(
    std::array<uint8_t, 256>& map, const ShadeRamp& ramp, int target, std::span<const int> sources
) {
    if (sources.empty() || target < 0 || target >= 256)
        return;
    const double target_lightness = ramp.lightness[static_cast<std::size_t>(target)];
    int forced = sources[0];
    double forced_distance =
        std::abs(ramp.lightness[static_cast<std::size_t>(forced)] - target_lightness);
    for (const int source : sources) {
        if (source < 0 || source >= 256)
            continue;
        const double source_lightness = ramp.lightness[static_cast<std::size_t>(source)];
        map[static_cast<std::size_t>(source)] =
            static_cast<uint8_t>(nearest_lightness(ramp, source_lightness));
        const double from_target = std::abs(source_lightness - target_lightness);
        if (from_target < forced_distance || (from_target == forced_distance && source < forced)) {
            forced = source;
            forced_distance = from_target;
        }
    }
    if (forced >= 0 && forced < 256)
        map[static_cast<std::size_t>(forced)] = static_cast<uint8_t>(target);
}

// Moves every shade by `offset` lightness and picks the ramp entry nearest that. Nothing is forced
// onto the ramp's anchor. The lower index wins a tie.
void map_offset(
    std::array<uint8_t, 256>& map,
    const ShadeRamp& ramp,
    std::span<const int> sources,
    double offset
) {
    for (const int source : sources) {
        if (source < 0 || source >= 256)
            continue;
        const double want = ramp.lightness[static_cast<std::size_t>(source)] + offset;
        map[static_cast<std::size_t>(source)] = static_cast<uint8_t>(nearest_lightness(ramp, want));
    }
}

// The reference shade becomes `target`. Every other shade keeps its distance in lightness from the
// reference, on the ramp around `target`. The lower index wins a tie.
void map_shifted(
    std::array<uint8_t, 256>& map,
    const ShadeRamp& ramp,
    int target,
    int reference,
    std::span<const int> sources
) {
    if (target < 0 || target >= 256 || reference < 0 || reference >= 256)
        return;
    const double shift = ramp.lightness[static_cast<std::size_t>(target)] -
                         ramp.lightness[static_cast<std::size_t>(reference)];
    map[static_cast<std::size_t>(reference)] = static_cast<uint8_t>(target);
    for (const int source : sources) {
        if (source < 0 || source >= 256 || source == reference)
            continue;
        const double want = ramp.lightness[static_cast<std::size_t>(source)] + shift;
        map[static_cast<std::size_t>(source)] = static_cast<uint8_t>(nearest_lightness(ramp, want));
    }
}

void remap_image(Handle image, const std::array<uint8_t, 256>& map) {
    Surface surface{};
    if (image < 0 || !image_surface(image, &surface) || surface.pixels == nullptr ||
        surface.width <= 0 || surface.height <= 0)
        return;
    for (int32_t row = 0; row < surface.height; ++row) {
        uint8_t* pixels = surface.pixels + static_cast<std::ptrdiff_t>(row) * surface.pitch;
        present::colour_vision::remap_indices(
            std::span<uint8_t>(pixels, static_cast<std::size_t>(surface.width)), map, kWidgetKey
        );
    }
}

void remap_window(const Window* window, const std::array<uint8_t, 256>& map) {
    if (window == nullptr || window->states.images == nullptr)
        return;
    for (int32_t state = 0; state < window->states.count; ++state)
        remap_image(window->states.images[state], map);
}

constexpr uint8_t k_cue_stamp = 0x80;

void paint_row_bars(const Window* icon, int grade) {
    if (icon == nullptr || icon->states.images == nullptr || grade < 1 || grade > 3)
        return;
    for (int32_t state = 0; state < icon->states.count; ++state) {
        Surface surface{};
        if (!image_surface(icon->states.images[state], &surface) || surface.pixels == nullptr)
            continue;
        paint_latency_bars(
            surface.pixels, surface.width, surface.height, surface.pitch, grade, kCueInk, kCueGround
        );
    }
}

void paint_latency_icon(PlayerRowIcons* row, bool reload) {
    if (row == nullptr || row->latency_icon == nullptr)
        return;
    const auto now = static_cast<uint8_t>(present::colour_vision::active_mode());
    const bool cues = present::colour_vision::shape_cues_active();
    const uint8_t stamp = static_cast<uint8_t>(now | (cues ? k_cue_stamp : 0));
    const auto found = g_latency_applied.find(row->latency_icon);
    const uint8_t applied = found == g_latency_applied.end()
                                ? static_cast<uint8_t>(present::colour_vision::Mode::off)
                                : found->second;
    if (!reload && applied == stamp)
        return;
    bind_images(row->latency_icon, "Widgets.Gaf", "Latency_Icons", row->latency_frame);
    if (now != static_cast<uint8_t>(present::colour_vision::Mode::off))
        remap_window(row->latency_icon, lobby_latency_map(boneyards_palette()));
    if (cues)
        paint_row_bars(row->latency_icon, row->latency_frame);
    if (stamp == static_cast<uint8_t>(present::colour_vision::Mode::off))
        g_latency_applied.erase(row->latency_icon);
    else
        g_latency_applied[row->latency_icon] = stamp;
}

// Catalogue section for a role. The section name has no closing bracket; its value is the tag
// the player reads, and the English tag stands in when that language has no line.
const char* role_catalogue_key(ChatRole role) noexcept {
    switch (role) {
    case ChatRole::system:
        return "SYS";
    case ChatRole::news:
        return "NEWS";
    case ChatRole::arm:
        return "ARM";
    case ChatRole::core:
        return "CORE";
    case ChatRole::none:
        break;
    }
    return nullptr;
}

bool mark_pixel(NameMark mark, int32_t x, int32_t y) noexcept {
    if (mark == NameMark::lock) {
        const bool shackle = (y >= 0 && y <= 2 && (x == 2 || x == 4)) || (y == 0 && x == 3);
        const bool body = y >= 3 && y <= 6 && x >= 1 && x <= 5;
        return shackle || body;
    }
    if (mark == NameMark::mute)
        return (x == y || x + y == 6) && x >= 0 && x <= 6 && y >= 0 && y <= 6;
    return false;
}

} // namespace

void boneyards_use_palette(const uint8_t* entries, std::size_t bytes) {
    if (entries == nullptr || bytes < kPaletteBytes)
        g_palette_bytes.clear();
    else
        g_palette_bytes.assign(entries, entries + kPaletteBytes);
}

std::span<const uint8_t> boneyards_palette() {
    return g_palette_bytes;
}

void boneyards_muzzle_font(int32_t font) {
    g_muzzle_font = font;
}

int32_t galaxy_link_index(int32_t stored, std::span<const uint8_t> palette) {
    using present::colour_vision::Mode;
    const Mode mode = present::colour_vision::active_mode();
    if (mode == Mode::off || !palette_ready(palette))
        return stored;
    const present::colour_vision::SemanticColours colours =
        present::colour_vision::semantic_colours(mode);
    const present::colour_vision::Rgb* chosen = nullptr;
    if (stored == connector_color::arm)
        chosen = &colours.war_arm;
    else if (stored == connector_color::core)
        chosen = &colours.war_core;
    else if (stored == connector_color::mixed)
        chosen = &colours.war_mixed;
    if (chosen == nullptr)
        return stored;
    return present::colour_vision::match_index(palette, *chosen);
}

GalaxyChatColours galaxy_chat_colours(std::span<const uint8_t> palette) {
    GalaxyChatColours tints{chat_tint::system, chat_tint::newsbot, chat_tint::arm, chat_tint::core};
    using present::colour_vision::Mode;
    const Mode mode = present::colour_vision::active_mode();
    if (mode == Mode::off || !palette_ready(palette))
        return tints;
    const present::colour_vision::SemanticColours colours =
        present::colour_vision::semantic_colours(mode);
    tints.system = present::colour_vision::match_index(palette, colours.chat_system);
    tints.newsbot = present::colour_vision::match_index(palette, colours.chat_newsbot);
    tints.arm = present::colour_vision::match_index(palette, colours.chat_arm);
    tints.core = present::colour_vision::match_index(palette, colours.chat_core);
    return tints;
}

int32_t muzzle_name_index(std::span<const uint8_t> palette) {
    using present::colour_vision::Mode;
    const Mode mode = present::colour_vision::active_mode();
    if (mode == Mode::off || !palette_ready(palette))
        return chat_tint::muzzle;
    return present::colour_vision::match_index(
        palette, present::colour_vision::semantic_colours(mode).health_bad
    );
}

SideIconColours side_icon_colours(std::span<const uint8_t> palette) {
    SideIconColours bodies{side_icon_body::arm, side_icon_body::core};
    using present::colour_vision::Mode;
    const Mode mode = present::colour_vision::active_mode();
    if (mode == Mode::off || !palette_ready(palette))
        return bodies;
    const present::colour_vision::SemanticColours colours =
        present::colour_vision::semantic_colours(mode);
    bodies.arm = present::colour_vision::match_index(palette, colours.war_arm);
    bodies.core = present::colour_vision::match_index(palette, colours.war_core);
    return bodies;
}

std::array<uint8_t, 256> galaxy_aura_map(bool arm, std::span<const uint8_t> palette) {
    std::array<uint8_t, 256> map = identity_map();
    using present::colour_vision::Mode;
    const Mode mode = present::colour_vision::active_mode();
    if (mode == Mode::off || !palette_ready(palette))
        return map;
    const present::colour_vision::SemanticColours colours =
        present::colour_vision::semantic_colours(mode);
    const int target =
        present::colour_vision::match_index(palette, arm ? colours.war_arm : colours.war_core);
    const ShadeRamp ramp = ramp_around(palette, target);
    const int arm_shades[] = {225, 226, 227, 228};
    const int core_shades[] = {210, 211, 212, 213};
    // An achromat sees lightness alone. Core is drawn lighter. Arm keeps its shades so a quiet
    // Arm planet's thin ring stays clear of space.
    if (mode == Mode::achromatopsia) {
        if (arm)
            map_offset(map, ramp, arm_shades, 0.0);
        else
            map_offset(map, ramp, core_shades, 12.0);
    } else if (arm) {
        map_sources(map, ramp, target, arm_shades);
    } else {
        map_sources(map, ramp, target, core_shades);
    }
    map[static_cast<std::size_t>(kWidgetKey)] = static_cast<uint8_t>(kWidgetKey);
    return map;
}

std::array<uint8_t, 256> lobby_latency_map(std::span<const uint8_t> palette) {
    std::array<uint8_t, 256> map = identity_map();
    using present::colour_vision::Mode;
    const Mode mode = present::colour_vision::active_mode();
    if (mode == Mode::off || !palette_ready(palette))
        return map;
    const present::colour_vision::SemanticColours colours =
        present::colour_vision::semantic_colours(mode);
    const int good = present::colour_vision::match_index(palette, colours.health_good);
    const int middle = present::colour_vision::match_index(palette, colours.health_mid);
    const int bad = present::colour_vision::match_index(palette, colours.health_bad);
    const int good_shades[] = {160, 161, 162, 163, 164};
    const int middle_shades[] = {195, 196, 197, 198, 199};
    const int bad_shades[] = {201, 202, 203, 204, 205, 212};
    // The middle shade of each grade is the reference that lands on the mode's colour.
    constexpr int good_reference = 162;
    constexpr int middle_reference = 197;
    constexpr int bad_reference = 203;
    map_shifted(map, ramp_around(palette, good), good, good_reference, good_shades);
    map_shifted(map, ramp_around(palette, middle), middle, middle_reference, middle_shades);
    map_shifted(map, ramp_around(palette, bad), bad, bad_reference, bad_shades);
    map[static_cast<std::size_t>(kWidgetKey)] = static_cast<uint8_t>(kWidgetKey);
    return map;
}

ChatRole chat_role_for_handles(
    int32_t font, int32_t system, int32_t news, int32_t arm, int32_t core
) noexcept {
    if (font < 0)
        return ChatRole::none;
    if (font == system)
        return ChatRole::system;
    if (font == news)
        return ChatRole::news;
    if (font == arm)
        return ChatRole::arm;
    if (font == core)
        return ChatRole::core;
    return ChatRole::none;
}

const char* chat_role_tag(ChatRole role) noexcept {
    switch (role) {
    case ChatRole::system:
        return "[SYS]";
    case ChatRole::news:
        return "[NEWS]";
    case ChatRole::arm:
        return "[ARM]";
    case ChatRole::core:
        return "[CORE]";
    case ChatRole::none:
        break;
    }
    return nullptr;
}

void write_role_line(char* out, std::size_t capacity, const char* text, ChatRole role, bool cues) {
    if (out == nullptr || capacity == 0)
        return;
    const char* body = text != nullptr ? text : "";
    const char* tag = cues ? chat_role_tag(role) : nullptr;
    if (tag == nullptr) {
        std::snprintf(out, capacity, "%s", body);
        return;
    }
    // The tag is the Boneyards catalogue's own line. The game's translation of the section name
    // is a side name, not a tag, so it is not used here.
    char translated[64];
    const char* shown = tag;
    const char* key = role_catalogue_key(role);
    if (key != nullptr) {
        const std::string_view own = oa::data::languages::interface_text(key);
        if (!own.empty() && own != key) {
            const std::size_t count =
                own.size() < sizeof translated - 1 ? own.size() : sizeof translated - 1;
            std::memcpy(translated, own.data(), count);
            translated[count] = '\0';
            shown = translated;
        }
    }
    if (body[0] == '\0')
        std::snprintf(out, capacity, "%s", shown);
    else
        std::snprintf(out, capacity, "%s %s", shown, body);
}

void paint_name_mark(
    uint8_t* pixels,
    int32_t width,
    int32_t height,
    int32_t pitch,
    NameMark mark,
    uint8_t ink,
    uint8_t ground
) noexcept {
    if (pixels == nullptr || width <= 0 || height <= 0 || pitch <= 0 || mark == NameMark::none)
        return;
    for (int32_t row = 0; row < height; ++row) {
        uint8_t* line = pixels + static_cast<std::ptrdiff_t>(row) * pitch;
        for (int32_t column = 0; column < width; ++column)
            line[column] = mark_pixel(mark, column, row) ? ink : ground;
    }
}

void paint_latency_bars(
    uint8_t* pixels,
    int32_t width,
    int32_t height,
    int32_t pitch,
    int grade,
    uint8_t ink,
    uint8_t ground
) noexcept {
    if (pixels == nullptr || width <= 0 || height <= 0 || pitch <= 0 || grade < 1 || grade > 3)
        return;
    for (int32_t row = 0; row < height; ++row) {
        uint8_t* line = pixels + static_cast<std::ptrdiff_t>(row) * pitch;
        for (int32_t column = 0; column < width; ++column)
            line[column] = ground;
    }
    constexpr int32_t bar_width = 2;
    constexpr int32_t gap = 1;
    constexpr int32_t bar_height = 7;
    const int32_t total = grade * bar_width + (grade - 1) * gap;
    const int32_t origin = width > total ? (width - total) / 2 : 0;
    const int32_t top = height > bar_height ? (height - bar_height) / 2 : 0;
    const int32_t bottom = top + bar_height < height ? top + bar_height : height;
    for (int bar = 0; bar < grade; ++bar) {
        const int32_t left = origin + bar * (bar_width + gap);
        const int32_t right = left + bar_width < width ? left + bar_width : width;
        for (int32_t row = top; row < bottom; ++row) {
            uint8_t* line = pixels + static_cast<std::ptrdiff_t>(row) * pitch;
            for (int32_t column = left; column < right; ++column)
                line[column] = ink;
        }
    }
}

} // namespace oa::boneyards::screens
