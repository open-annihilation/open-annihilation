// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// DockBar: a panel the user drags between the desktop edges.
#include <cstdlib>
#include <cstring>

#include "oa/boneyards/toolkit.hpp"
#include "ops_defaults.hpp"

namespace oa::boneyards {
namespace {

constexpr int32_t k_empty_dock_size = 0x32;

DockBar* as_dock(Window* w) {
    return reinterpret_cast<DockBar*>(w);
}

bool is_group_box(Window* w) {
    return w->ops->is_class(class_name::group_box);
}

// The desktop area docked bars span.
struct Area {
    int32_t x, y, width, height;
};

Area desktop_area() {
    return Area{
        services_desktop_x(),
        services_desktop_y(),
        services_desktop_width(),
        services_desktop_height(),
    };
}

void set_outline(DockBar* bar, int32_t x, int32_t y, int32_t width, int32_t height) {
    bar->outline_x = x;
    bar->outline_y = y;
    bar->outline_width = width;
    bar->outline_height = height;
}

/// Tests whether a class name is "DockBar".
///
/// @param name class name to test; may be null
/// @return true when `name` is "DockBar"
bool dock_bar_is_class(const char* name) {
    return names_equal(name, class_name::dock_bar);
}

/// Starts a drag when the press is inside the bar: captures the pointer,
/// measures the docked size and starts the drop outline at the bar's current
/// rectangle.
///
/// The right and middle buttons reach this slot too.
///
/// @param[in,out] self bar (a DockBar record) being pressed
/// @param msg press message with the pointer position; null is ignored
/// @return true (handled)
bool dock_bar_left_down(Window* self, const Message* msg) {
    if (msg == nullptr || !self->ops->contains(self, msg->x, msg->y)) {
        return true;
    }
    DockBar* bar = as_dock(self);
    services_desktop_capture(self);
    bar->dragging = 1;
    dock_bar_measure(bar);
    set_outline(bar, self->x, self->y, self->width, self->height);
    bar->drag_x = msg->x;
    bar->drag_y = msg->y;
    return true;
}

/// Drops the bar: releases the pointer capture, takes the drop outline's
/// size and position, restacks the children for the drop edge and sends
/// k_notify_docked to the parent's on_notify (the bar's own when top-level).
///
/// @param[in,out] self bar (a DockBar record) being released
/// @param msg pointer message (unused)
/// @return true (handled)
bool dock_bar_left_up(Window* self, const Message* /*msg*/) {
    DockBar* bar = as_dock(self);
    services_desktop_release_capture();
    bar->dragging = 0;
    window_set_size(self, bar->outline_width, bar->outline_height);
    window_move_to(self, bar->outline_x, bar->outline_y);
    bar->edge = bar->drop_edge;
    dock_bar_arrange(bar, self->x, self->y, self->width, self->height, true);
    Notify notify{};
    notify.code = k_notify_docked;
    notify.source = self;
    Window* target = self->parent != nullptr ? self->parent : self;
    target->ops->on_notify(target, &notify);
    return true;
}

/// Moves the drop outline with the pointer while the bar is dragged.
///
/// @param[in,out] self bar (a DockBar record) receiving the move
/// @param msg mouse_move message with the pointer position; null is ignored
/// @return true (handled)
bool dock_bar_mouse_move(Window* self, const Message* msg) {
    DockBar* bar = as_dock(self);
    if (msg != nullptr && bar->dragging != 0) {
        dock_bar_snap(bar, msg->x, msg->y);
        bar->drag_x = msg->x;
        bar->drag_y = msg->y;
    }
    return true;
}

/// Draws a dock bar: its state image or background colour, its children and,
/// while dragged, the drop outline through the draw service's frame slot.
///
/// A hidden bar draws nothing.
///
/// @param self bar (a DockBar record) to draw
/// @return true (handled)
bool dock_bar_draw(Window* self) {
    if (window_is_shown(self)) {
        const int32_t state = self->states.current;
        const Handle image =
            state >= 0 && state < self->states.count ? self->states.images[state] : k_no_handle;
        if (image < 0) {
            if (self->color >= 0) {
                color_fill(self->x, self->y, self->width, self->height, self->color);
            }
        } else {
            image_draw(self->x, self->y, self->width, self->height, image);
        }
        window_draw_children(self);
        DockBar* bar = as_dock(self);
        if (bar->dragging != 0) {
            draw_frame(bar->outline_x, bar->outline_y, bar->outline_width, bar->outline_height);
        }
    }
    return true;
}

void dock_bar_destroy(Window* self) {
    window_release(self);
    std::free(self);
}

WindowOps make_dock_bar_ops() {
    WindowOps ops = k_panel_ops;
    ops.class_name = class_name::dock_bar;
    ops.destroy = dock_bar_destroy;
    ops.is_class = dock_bar_is_class;
    ops.draw = dock_bar_draw;
    ops.left_down = dock_bar_left_down;
    ops.left_up = dock_bar_left_up;
    // The right and middle buttons drag the bar as the left one does.
    ops.right_down = detail::forward_left_down;
    ops.right_up = detail::forward_left_up;
    ops.middle_down = detail::forward_left_down;
    ops.middle_up = detail::forward_left_up;
    ops.mouse_move = dock_bar_mouse_move;
    return ops;
}

} // namespace

// Derived from a constant-initialised table, so copying it during dynamic
// initialisation is order-safe.
const WindowOps k_dock_bar_ops = make_dock_bar_ops();

DockBar* dock_bar_create() {
    auto* bar = static_cast<DockBar*>(std::calloc(1, sizeof(DockBar)));
    if (bar == nullptr) {
        std::abort();
    }
    window_init(&bar->base, &k_dock_bar_ops);
    return bar;
}

void dock_bar_measure(DockBar* bar) {
    const WindowList& children = bar->base.children;
    if (children.count == 0) {
        if (bar->dock_width == 0) {
            bar->dock_width = k_empty_dock_size;
        }
        if (bar->dock_height == 0) {
            bar->dock_height = k_empty_dock_size;
        }
        return;
    }
    for (int32_t i = 0; i < children.count; ++i) {
        Window* child = children.items[i];
        int32_t width = child->width;
        int32_t height = child->height;
        if (child != nullptr && !is_group_box(child)) {
            width += bar->spacing * 2;
            height += bar->spacing * 2;
        }
        if (bar->dock_width < width) {
            bar->dock_width = width;
        }
        if (bar->dock_height < height) {
            bar->dock_height = height;
        }
    }
    const int32_t edges = bar->edges;
    bar->edge = dock_edge::top;
    for (;;) {
        const Area area = desktop_area();
        if (dock_bar_arrange(bar, area.x, area.y, area.width, bar->dock_height, false)) {
            break;
        }
        int32_t total = 0;
        for (int32_t i = 0; i < children.count; ++i) {
            Window* child = children.items[i];
            int32_t height = child->height;
            if (child != nullptr && !is_group_box(child)) {
                total = bar->spacing;
                height += total;
            }
            total += height;
            if (bar->dock_height < total) {
                bar->dock_height = total;
                break;
            }
        }
        if (desktop_area().height / 2 <= bar->dock_height) {
            break;
        }
    }
    bar->edge = edges;
}

bool dock_bar_arrange(
    DockBar* bar, int32_t x, int32_t y, int32_t width, int32_t height, bool place
) {
    const WindowList& children = bar->base.children;
    if (children.count == 0) {
        return true;
    }
    const int32_t right = bar->base.x + width;
    const int32_t bottom = bar->base.y + height;
    int32_t column_width = children.items[0]->width;
    const int32_t edge = bar->edge;
    if (edge != dock_edge::top && edge != dock_edge::bottom && edge != dock_edge::left &&
        edge != dock_edge::right) {
        return true;
    }
    for (int32_t i = 0; i < children.count; ++i) {
        Window* child = children.items[i];
        if (child == nullptr) {
            continue;
        }
        int32_t child_width = child->width;
        int32_t child_height = child->height;
        int32_t dx = 0;
        int32_t dy = 0;
        if (!is_group_box(child)) {
            dx = bar->spacing;
            child_width += dx;
            child_height += dx;
            dy = dx;
        } else if (edge == dock_edge::top || edge == dock_edge::bottom) {
            dx = bar->spacing;
        } else {
            dy = bar->spacing;
        }
        if (bottom < child_height + y) {
            x += dx + column_width;
            if (right <= child_width + x) {
                return false;
            }
            column_width = child->width;
            y = bar->base.y + dy;
        }
        if (place) {
            window_move_to(child, dx + x, dy + y);
        }
        if (column_width < child->width) {
            column_width = child->width;
        }
        y += child_height;
    }
    return true;
}

void dock_bar_snap(DockBar* bar, int32_t px, int32_t py) {
    const Area a = desktop_area();
    const int32_t X = a.x;
    const int32_t Y = a.y;
    const int32_t W = a.width;
    const int32_t H = a.height;
    if (W == 0) {
        return;
    }
    enum class Drop { none, top, bottom, left, right };
    Drop drop = Drop::none;
    const int32_t d = ((X - px) * H) / W;
    const int32_t d_half = ((X - px) * H) / (W * 2);
    const bool left_half = px <= W / 2 + X;
    const bool upper_half = py <= H / 2 + Y;
    switch (bar->edges) {
    case 0xF:
        if (left_half) {
            if (upper_half) {
                drop = py <= Y - d ? Drop::top : Drop::left;
            } else {
                drop = py <= d + H + Y ? Drop::left : Drop::bottom;
            }
        } else if (!upper_half) {
            drop = Y - d < py ? Drop::bottom : Drop::right;
        } else {
            drop = py <= d + H + Y ? Drop::top : Drop::right;
        }
        break;
    case dock_edge::top | dock_edge::bottom:
        drop = upper_half ? Drop::top : Drop::bottom;
        break;
    case dock_edge::left | dock_edge::right:
        drop = left_half ? Drop::left : Drop::right;
        break;
    case dock_edge::top | dock_edge::left:
        drop = py <= Y - d ? Drop::top : Drop::left;
        break;
    case dock_edge::top | dock_edge::right:
        drop = py <= d + H + Y ? Drop::top : Drop::right;
        break;
    case dock_edge::bottom | dock_edge::left:
        drop = py <= d + H + Y ? Drop::left : Drop::bottom;
        break;
    case dock_edge::bottom | dock_edge::right:
        drop = Y - d < py ? Drop::bottom : Drop::right;
        break;
    case dock_edge::top | dock_edge::left | dock_edge::right:
        if (left_half) {
            drop = py <= (H * px * 2) / W - (H * X * 2) / W + Y ? Drop::top : Drop::left;
        } else {
            drop = py <= Y + ((X - px) * H * 2) / W + H * 2 ? Drop::top : Drop::right;
        }
        break;
    case dock_edge::bottom | dock_edge::left | dock_edge::right:
        if (left_half) {
            drop = py <= ((X - px) * H * 2) / W + H + Y ? Drop::left : Drop::bottom;
        } else {
            drop = (Y - ((X - px) * H * 2) / W) - H < py ? Drop::bottom : Drop::right;
        }
        break;
    case dock_edge::top | dock_edge::bottom | dock_edge::left:
        if (upper_half) {
            drop = py <= Y - d_half ? Drop::top : Drop::left;
        } else {
            drop = py <= d_half + H + Y ? Drop::left : Drop::bottom;
        }
        break;
    case dock_edge::top | dock_edge::bottom | dock_edge::right:
        if (H / 2 + Y < py) {
            drop = (H / 2 - d_half) + Y < py ? Drop::bottom : Drop::right;
        } else {
            drop = py <= d_half + H / 2 + Y ? Drop::top : Drop::right;
        }
        break;
    default:
        return;
    }
    switch (drop) {
    case Drop::top:
        set_outline(bar, X, Y, W, bar->dock_height);
        bar->drop_edge = dock_edge::top;
        break;
    case Drop::bottom:
        set_outline(bar, X, (H - bar->dock_height) + Y, W, bar->dock_height);
        bar->drop_edge = dock_edge::bottom;
        break;
    case Drop::left:
        set_outline(bar, X, Y, bar->dock_width, H);
        bar->drop_edge = dock_edge::left;
        break;
    case Drop::right:
        set_outline(bar, (W - bar->dock_width) + X, Y, bar->dock_width, H);
        bar->drop_edge = dock_edge::right;
        break;
    case Drop::none:
        break;
    }
}

} // namespace oa::boneyards
