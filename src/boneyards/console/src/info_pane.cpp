// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Info panes: the side panels the Boneyards consoles open over the top of a
// list, which moves down and shrinks to make room. The Main Frame keeps them
// between consoles.
#include "console_state.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

namespace layout {
constexpr int32_t x = 0x93, y = 0x7e, width = 0x176, height = 0x9d;
constexpr int32_t title_y = 6, title_width = 0x166;
constexpr int32_t close_dx = 0x166, close_dy = 6, close_width = 0xc, close_height = 0xd;
constexpr int32_t rule_y = 0x9c;
constexpr uint8_t rule_color = 0x5d;
// Rows the list scrolls on as it shrinks, and at most back as it grows.
constexpr int32_t list_scroll = 9;
} // namespace layout

constexpr int32_t k_image_state = 1;
constexpr int32_t k_title_centred = 1;
constexpr int32_t k_anchor_top_left = 0;
constexpr const char* k_close_sequence = "Close_Button";
constexpr const char* k_close_sound = "INFO_PANE_CLOSE";

InfoPane* as_pane(Window* w) {
    return reinterpret_cast<InfoPane*>(w);
}

void notify_parent(InfoPane* pane, int32_t code) {
    window_notify_parent(&pane->base, static_cast<NotifyCode>(code), 0);
}

// The parent's top, which the layout's offsets count from.
int32_t parent_top(const InfoPane* pane) {
    return pane->base.parent != nullptr ? pane->base.parent->y : 0;
}

/// Closes the pane when its Close_Button is pressed.
///
/// @param self the info pane
/// @param notify control notice
void info_pane_on_notify(Window* self, const Notify* notify) {
    (void)info_pane_on_close_click(as_pane(self), notify);
}

} // namespace

WindowOps info_pane_ops() {
    WindowOps ops = console_panel_ops();
    ops.on_notify = info_pane_on_notify;
    return ops;
}

void info_pane_init(InfoPane* pane, const char* title, Window* parent, StatusWidget* owner) {
    namespace l = layout;
    Window* self = &pane->base;
    pane->layout = InfoPaneLayout{};
    pane->owner = owner;
    window_create(self, title, l::x, l::y, l::width, l::height, parent, false);
    window_set_state_image(self, k_image_state, owner->system.info_pane_image);
    screens::lobby_label_create(
        self,
        0,
        l::title_y,
        title,
        owner->system.hat_fonts.big,
        k_title_centred,
        k_anchor_top_left,
        l::title_width
    );
    Window* close = button_create();
    pane->close_button = close;
    window_set_bounds(
        close,
        self->x + l::close_dx,
        self->y + l::close_dy,
        l::close_width,
        l::close_height,
        self,
        true
    );
    close->ops->bind_images(close, screens::kWidgetsGaf, k_close_sequence, 0, screens::kWidgetKey);
}

void info_pane_set_layout(InfoPane* pane, const InfoPaneLayout& layout) {
    pane->layout = layout;
}

void info_pane_open(InfoPane* pane) {
    const InfoPaneLayout& l = pane->layout;
    pane->base.visible = 1;
    if (l.list != nullptr) {
        Window* list = &l.list->base;
        window_set_height(list, l.list_open_height);
        window_move_to(list, list->x, parent_top(pane) + l.list_open_dy);
        list_box_scroll(l.list, layout::list_scroll);
    }
    if (l.second != nullptr)
        window_move_to(l.second, l.second->x, parent_top(pane) + l.second_open_dy);
    notify_parent(pane, pane_notify::opened);
}

void info_pane_close(InfoPane* pane) {
    const InfoPaneLayout& l = pane->layout;
    pane->base.visible = 0;
    if (l.list != nullptr) {
        Window* list = &l.list->base;
        int32_t back = l.list->count - l.list->last - 1;
        window_set_height(list, l.list_closed_height);
        window_move_to(list, list->x, parent_top(pane) + l.list_closed_dy);
        if (back > layout::list_scroll)
            back = layout::list_scroll;
        list_box_scroll(l.list, -back);
    }
    if (l.second != nullptr)
        window_move_to(l.second, l.second->x, parent_top(pane) + l.second_closed_dy);
    notify_parent(pane, pane_notify::closed);
}

bool info_pane_on_close_click(InfoPane* pane, const Notify* notify) {
    if (notify == nullptr || notify->source != pane->close_button)
        return false;
    const MainMenuHost& host = pane->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, k_close_sound);
    pane->slots->close(pane);
    return true;
}

Handle info_pane_image_create() {
    namespace l = layout;
    const Handle image =
        image_create_blank(l::width, l::height, screens::kWidgetKey, screens::kWidgetKey);
    Surface surface{};
    if (image_surface(image, &surface))
        (void)present::draw_clipped_line(
            &surface, 0, l::rule_y, l::width - 1, l::rule_y, l::rule_color
        );
    return image;
}

} // namespace oa::boneyards::console
