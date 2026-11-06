// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Container classes: Panel, Dialog, GroupBox, PageControl, IWindow and the
// draggable, minimisable IWnd frame.
#include <cstdlib>
#include <cstring>

#include "list.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "ops_defaults.hpp"

namespace oa::boneyards {
namespace {

constexpr int32_t k_iwindow_states = 3;
constexpr int32_t k_iwindow_icon_size = 20; // default minimised rectangle
constexpr int32_t k_minimize_inset_right = 30;
constexpr int32_t k_minimize_inset_top = 10;
constexpr int32_t k_minimize_size = 20;
const char* const k_minimize_name = "MINIMIZE_BUTTON";
const char* const k_iwindow_state_names[] = {"Disabled", "Normal", "Icon"};

template <typename T>
T* allocate(const WindowOps* ops) {
    auto* record = static_cast<T*>(std::calloc(1, sizeof(T)));
    if (record == nullptr) {
        std::abort();
    }
    window_init(reinterpret_cast<Window*>(record), ops);
    return record;
}

void destroy_record(Window* self) {
    window_release(self);
    std::free(self);
}

int32_t zero_type_code() {
    return 0;
}

// Index to start a focus scan from: after (or before) `from` when it is a
// child, else the first (or last) child.
int32_t scan_start(const Window* self, const Window* from, bool backwards, bool wrap, bool* found) {
    const int32_t count = self->children.count;
    *found = false;
    for (int32_t i = 0; i < count; ++i) {
        if (self->children.items[i] == from) {
            *found = true;
            int32_t next = backwards ? i - 1 : i + 1;
            if (wrap) {
                next = next < 0 ? count - 1 : (next > count - 1 ? 0 : next);
            }
            return next;
        }
    }
    return backwards ? count - 1 : 0;
}

// Asks each child in turn (wrapping) for a focus target.
Window* scan_children_wrapping(Window* self, Window* from, bool backwards) {
    const int32_t count = self->children.count;
    if (count == 0) {
        return nullptr;
    }
    bool found = false;
    int32_t index = scan_start(self, from, backwards, true, &found);
    const int32_t stop = index;
    do {
        Window* child = self->children.items[index];
        if (child != nullptr) {
            Window* target = child->ops->focus_target(child, self, backwards);
            if (target != nullptr) {
                return target;
            }
        }
        index = backwards ? (index - 1 < 0 ? count - 1 : index - 1)
                          : (index + 1 > count - 1 ? 0 : index + 1);
    } while (index != stop);
    return nullptr;
}

// Asks each remaining child (no wrap), then the parent.
Window* scan_children_then_parent(Window* self, Window* from, bool backwards) {
    const int32_t count = self->children.count;
    if (count > 0) {
        bool found = false;
        int32_t index = scan_start(self, from, backwards, false, &found);
        const int32_t stop = index;
        while (index >= 0 && index <= count - 1) {
            Window* child = self->children.items[index];
            if (child != nullptr) {
                Window* target = child->ops->focus_target(child, self, backwards);
                if (target != nullptr) {
                    return target;
                }
            }
            index += backwards ? -1 : 1;
            if (index == stop) {
                break;
            }
        }
    }
    Window* parent = self->parent;
    if (parent != nullptr && parent != from) {
        return parent->ops->focus_target(parent, self, backwards);
    }
    return nullptr;
}

// Image or caption of the current state, then the children.
bool draw_image_text_children(Window* self) {
    if (window_is_shown(self)) {
        const int32_t state = self->states.current;
        const bool valid = state >= 0 && state < self->states.count;
        const Handle image = valid ? self->states.images[state] : k_no_handle;
        const Handle text = valid ? self->states.texts[state] : k_no_handle;
        if (image >= 0) {
            image_draw(self->x, self->y, self->width, self->height, image);
        }
        if (text >= 0) {
            text_draw(self->font, self->x, self->y, self->width, self->height, text);
        }
        window_draw_children(self);
    }
    return true;
}

void forward_notify(Window* self, const Notify* notify) {
    if (notify != nullptr && self->parent != nullptr) {
        self->parent->ops->on_notify(self->parent, notify);
    }
}

// ---------------------------------------------------------------------------
// Panel

/// Tests whether a class name is "Panel".
///
/// @param name class name to test; may be null
/// @return true when `name` is "Panel"
bool panel_is_class(const char* name) {
    return names_equal(name, class_name::panel);
}

/// Chooses the panel's next focus target: the first child after `from`
/// (wrapping round) that offers one, otherwise the default focus target.
///
/// @param self panel asked for a focus target
/// @param from window the focus is moving away from; the scan starts after
///     it when it is a child, else at the first (or last) child
/// @param backwards true for Shift+Tab, scanning the children in reverse
/// @return the window to focus, or null
Window* panel_focus_target(Window* self, Window* from, bool backwards) {
    Window* target = scan_children_wrapping(self, from, backwards);
    return target != nullptr ? target : window_default_focus_target(self, from, backwards);
}

constexpr WindowOps make_panel_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::panel;
    ops.destroy = destroy_record;
    ops.is_class = panel_is_class;
    ops.type_code = zero_type_code;
    ops.focus_target = panel_focus_target;
    ops.on_notify = forward_notify;
    ops.draw = window_draw_background;
    return ops;
}

// ---------------------------------------------------------------------------
// Dialog

/// Tests whether a class name is "Dialog".
///
/// @param name class name to test; may be null
/// @return true when `name` is "Dialog"
bool dialog_is_class(const char* name) {
    return names_equal(name, class_name::dialog);
}

/// Keeps the focus inside a shown dialog: the first child after `from`
/// (wrapping round) that offers a target, otherwise the dialog itself.
///
/// A hidden dialog takes the default focus target instead.
///
/// @param self dialog asked for a focus target
/// @param from window the focus is moving away from
/// @param backwards true for Shift+Tab
/// @return the window to focus
Window* dialog_focus_target(Window* self, Window* from, bool backwards) {
    if (!window_is_shown(self)) {
        return window_default_focus_target(self, from, backwards);
    }
    Window* target = scan_children_wrapping(self, from, backwards);
    return target != nullptr ? target : self;
}

/// Finds the window under a point: the last child whose child_at slot reports
/// a hit, otherwise the dialog itself.
///
/// @param self dialog to search
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
/// @return the hit window, or null when the dialog is hidden or missed
Window* dialog_child_at(Window* self, int32_t x, int32_t y) {
    if (!window_is_shown(self) || !self->ops->contains(self, x, y)) {
        return nullptr;
    }
    Window* hit = self;
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child != nullptr) {
            Window* found = child->ops->child_at(child, x, y);
            if (found != nullptr) {
                hit = found;
            }
        }
    }
    return hit;
}

/// Swallows a pointer press on a dialog, beeping when it falls outside the
/// dialog.
///
/// @param self dialog receiving the press
/// @param msg press message with the pointer position; must not be null
/// @return true (handled)
bool dialog_click(Window* self, const Message* msg) {
    if (!self->ops->contains(self, msg->x, msg->y)) {
        sound_beep();
    }
    return true;
}

constexpr WindowOps make_dialog_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::dialog;
    ops.destroy = destroy_record;
    ops.is_class = dialog_is_class;
    ops.type_code = zero_type_code;
    ops.focus_target = dialog_focus_target;
    ops.child_at = dialog_child_at;
    ops.draw = window_draw_background;
    ops.left_down = dialog_click;
    ops.right_down = dialog_click;
    ops.middle_down = dialog_click;
    return ops;
}

// ---------------------------------------------------------------------------
// GroupBox

bool group_box_is_class(const char* name) {
    return names_equal(name, class_name::group_box);
}

int32_t group_box_type_code() {
    return 0;
}

/// Chooses the group box's next focus target: the children after `from` up
/// to the last one (no wrapping), then the parent.
///
/// @param self group box asked for a focus target
/// @param from window the focus is moving away from; the scan starts after
///     it when it is a child, else at the first (or last) child; a request
///     from the parent is not passed back to it
/// @param backwards true for Shift+Tab, scanning towards the first child
/// @return the window to focus, or null
Window* group_box_focus_target(Window* self, Window* from, bool backwards) {
    return scan_children_then_parent(self, from, backwards);
}

/// Finds the window under a point: the last child whose child_at slot reports
/// a hit, otherwise the box itself. Visibility is not tested.
///
/// @param self group box to search
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
/// @return the hit window, or null when the box does not contain the point
Window* group_box_child_at(Window* self, int32_t x, int32_t y) {
    if (!self->ops->contains(self, x, y)) {
        return nullptr;
    }
    Window* hit = self;
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child != nullptr) {
            Window* found = child->ops->child_at(child, x, y);
            if (found != nullptr) {
                hit = found;
            }
        }
    }
    return hit;
}

/// Draws a group box or page control: the current state's image and caption,
/// then the children, with no background fill.
///
/// A hidden box draws nothing.
///
/// @param self window to draw
/// @return true (handled)
bool group_box_draw(Window* self) {
    return draw_image_text_children(self);
}

constexpr WindowOps make_group_box_ops() {
    WindowOps ops = make_panel_ops();
    ops.class_name = class_name::group_box;
    ops.is_class = group_box_is_class;
    ops.type_code = group_box_type_code;
    ops.focus_target = group_box_focus_target;
    ops.child_at = group_box_child_at;
    ops.on_notify = forward_notify;
    ops.draw = group_box_draw;
    return ops;
}

// ---------------------------------------------------------------------------
// PageControl

bool page_control_is_class(const char* name) {
    return names_equal(name, class_name::page_control);
}

/// Shows the page registered for an activated tab; other events go on to the
/// parent.
///
/// An activation from a window that is not a registered tab is dropped.
///
/// @param[in,out] self page control (a PageControl record) receiving the event
/// @param notify event from a child; null is ignored
void page_control_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr) {
        return;
    }
    if (notify->code == NotifyCode::activated) {
        auto* pages = reinterpret_cast<PageControl*>(self);
        Window* page = page_control_page_for(pages, notify->source);
        if (page != nullptr) {
            page_control_select(pages, page);
        }
        return;
    }
    forward_notify(self, notify);
}

void page_control_destroy(Window* self) {
    auto* pages = reinterpret_cast<PageControl*>(self);
    for (int32_t i = 0; i < pages->page_count; ++i) {
        std::free(pages->pages[i]);
    }
    std::free(pages->pages);
    destroy_record(self);
}

constexpr WindowOps make_page_control_ops() {
    WindowOps ops = make_panel_ops();
    ops.class_name = class_name::page_control;
    ops.destroy = page_control_destroy;
    ops.is_class = page_control_is_class;
    ops.type_code = detail::window_plain_type_code;
    ops.focus_target = group_box_focus_target;
    ops.on_notify = page_control_on_notify;
    ops.draw = group_box_draw;
    return ops;
}

// ---------------------------------------------------------------------------
// IWindow

bool iwindow_is_class(const char* name) {
    return names_equal(name, class_name::iwindow);
}

int32_t iwindow_type_code() {
    return 0;
}

int32_t iwindow_state_index(Window* self, const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int32_t i = 0; i < self->states.count && i < k_iwindow_states; ++i) {
        if (std::strcmp(k_iwindow_state_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

const char* iwindow_state_name(Window* self, int32_t state) {
    if (state < 0 || state >= self->states.count || state >= k_iwindow_states) {
        return nullptr;
    }
    return k_iwindow_state_names[state];
}

/// Draws an IWindow: its state image or background colour, then its children
/// while open, or only the focus frame (while focused) when minimised.
///
/// A hidden window draws nothing.
///
/// @param self window (an IWindow record) to draw
/// @return true (handled)
bool iwindow_draw(Window* self) {
    if (!window_is_shown(self)) {
        return true;
    }
    auto* w = reinterpret_cast<IWindow*>(self);
    const int32_t state = self->states.current;
    const Handle image =
        state >= 0 && state < self->states.count ? self->states.images[state] : k_no_handle;
    if (image >= 0) {
        image_draw(self->x, self->y, self->width, self->height, image);
    } else if (self->color >= 0) {
        color_fill(self->x, self->y, self->width, self->height, self->color);
    }
    if (w->minimized == 0) {
        window_draw_children(self);
    } else if (self->focused != 0) {
        draw_focus_frame(self->x, self->y, self->width, self->height);
    }
    return true;
}

constexpr WindowOps make_iwindow_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::iwindow;
    ops.destroy = destroy_record;
    ops.state_index = iwindow_state_index;
    ops.state_name = iwindow_state_name;
    ops.is_class = iwindow_is_class;
    ops.type_code = iwindow_type_code;
    ops.draw = iwindow_draw;
    return ops;
}

// ---------------------------------------------------------------------------
// IWnd frame

bool iwnd_is_class(const char* name) {
    return names_equal(name, "IWnd");
}

/// Adds the frame's minimise button, a black 20x20 Button named
/// "MINIMIZE_BUTTON" 30 pixels from the right edge and 10 pixels from the top
/// (the on_bounds slot).
///
/// Each placement of the frame adds another button.
///
/// @param[in,out] self frame (an IWnd record) that was placed
void iwnd_on_bounds(Window* self) {
    Window* button = button_create();
    window_create(
        button,
        k_minimize_name,
        self->x + self->width - k_minimize_inset_right,
        self->y + k_minimize_inset_top,
        k_minimize_size,
        k_minimize_size,
        self,
        false
    );
    window_set_color(button, 0, 0, 0, 0xFF);
    button->visible = 1;
}

/// Chooses the frame's next focus target: while open, the first child after
/// `from` (wrapping round) that offers one; otherwise the default focus
/// target.
///
/// @param self frame asked for a focus target
/// @param from window the focus is moving away from
/// @param backwards true for Shift+Tab
/// @return the window to focus, or null
Window* iwnd_focus_target(Window* self, Window* from, bool backwards) {
    if (reinterpret_cast<IWindow*>(self)->minimized == 0) {
        Window* target = scan_children_wrapping(self, from, backwards);
        if (target != nullptr) {
            return target;
        }
    }
    return window_default_focus_target(self, from, backwards);
}

/// Moves the frame to the top of its parent's children without raising the
/// parent; a top-level frame is raised on the installed desktop.
///
/// @param self frame to raise
void iwnd_raise(Window* self) {
    Window* parent = self->parent;
    if (parent == nullptr) {
        services_desktop_raise(self);
        return;
    }
    if (list_remove(&parent->children, self)) {
        list_add_unique(&parent->children, self);
    }
}

/// Minimises an open frame when its "MINIMIZE_BUTTON" is activated; other
/// events are dropped.
///
/// @param[in,out] self frame (an IWnd record) receiving the event
/// @param notify event from a child; null is ignored
void iwnd_on_notify(Window* self, const Notify* notify) {
    if (notify != nullptr && notify->source != nullptr &&
        window_is_named(notify->source, k_minimize_name) && notify->code == NotifyCode::activated &&
        reinterpret_cast<IWindow*>(self)->minimized == 0) {
        iwindow_minimize(reinterpret_cast<IWindow*>(self));
    }
}

/// Restores a minimised frame on Enter; other keys take the default key
/// handling.
///
/// @param[in,out] self frame (an IWnd record) receiving the key
/// @param msg key_down message; null is not handled
/// @return true when Enter restored the frame, false for Enter on an open
///     frame, otherwise the default key_down result
bool iwnd_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (msg->key != key::enter) {
        return window_default_key_down(self, msg);
    }
    if (reinterpret_cast<IWindow*>(self)->minimized != 0) {
        iwindow_restore(reinterpret_cast<IWindow*>(self));
        return true;
    }
    return false;
}

/// Starts dragging the frame when pressed inside it: captures the pointer and
/// records the press point.
///
/// @param[in,out] self frame (an IWnd record) being pressed
/// @param msg left_down message with the pointer position; null is not
///     handled
/// @return true, or false for a null message
bool iwnd_left_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (self->ops->contains(self, msg->x, msg->y)) {
        auto* frame = reinterpret_cast<IWnd*>(self);
        services_desktop_capture(self);
        frame->dragging = 1;
        frame->drag_x = msg->x;
        frame->drag_y = msg->y;
    }
    return true;
}

/// Ends a drag: releases the pointer capture and clears the dragging flag.
///
/// @param[in,out] self frame (an IWnd record) being released
/// @param msg pointer message (unused)
/// @return true (handled)
bool iwnd_left_up(Window* self, const Message* /*msg*/) {
    services_desktop_release_capture();
    reinterpret_cast<IWnd*>(self)->dragging = 0;
    return true;
}

/// Restores a minimised frame on a double click.
///
/// @param[in,out] self frame (an IWnd record) double-clicked
/// @param msg pointer message (unused)
/// @return true when the frame was restored, else false
bool iwnd_left_double(Window* self, const Message* /*msg*/) {
    auto* w = reinterpret_cast<IWindow*>(self);
    if (w->minimized != 0) {
        iwindow_restore(w);
        return true;
    }
    return false;
}

/// Drags the frame by the pointer's movement since the last move.
///
/// An open frame moves through window_move_by, taking its children along. A
/// minimised icon moves only itself (its hidden children stay put) and is
/// kept wholly on the desktop.
///
/// @param[in,out] self frame (an IWnd record) receiving the move
/// @param msg mouse_move message with the pointer position; null is not
///     handled
/// @return true, or false for a null message
bool iwnd_mouse_move(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    auto* frame = reinterpret_cast<IWnd*>(self);
    if (frame->dragging != 0) {
        int32_t dx = msg->x - frame->drag_x;
        int32_t dy = msg->y - frame->drag_y;
        if (frame->window.minimized == 0) {
            window_move_by(self, dx, dy);
        } else {
            const int32_t area_w = services_desktop_width();
            const int32_t area_h = services_desktop_height();
            if (dx + self->x < 0) {
                dx = -self->x;
            } else if (area_w < self->width + self->x + dx) {
                dx = (area_w - self->width) - self->x;
            }
            if (dy + self->y < 0) {
                dy = -self->y;
            } else if (area_h < self->height + self->y + dy) {
                dy = (area_h - self->height) - self->y;
            }
            self->x += dx;
            self->y += dy;
        }
        frame->drag_x = msg->x;
        frame->drag_y = msg->y;
    }
    return true;
}

constexpr WindowOps make_iwnd_ops() {
    WindowOps ops = make_iwindow_ops();
    ops.class_name = "IWnd";
    ops.is_class = iwnd_is_class;
    ops.type_code = zero_type_code;
    ops.focus_target = iwnd_focus_target;
    ops.raise = iwnd_raise;
    ops.on_bounds = iwnd_on_bounds;
    ops.on_notify = iwnd_on_notify;
    ops.key_down = iwnd_key_down;
    ops.left_down = iwnd_left_down;
    ops.left_up = iwnd_left_up;
    ops.left_double = iwnd_left_double;
    ops.mouse_move = iwnd_mouse_move;
    return ops;
}

} // namespace

constinit const WindowOps k_panel_ops = make_panel_ops();
constinit const WindowOps k_dialog_ops = make_dialog_ops();
constinit const WindowOps k_group_box_ops = make_group_box_ops();
constinit const WindowOps k_page_control_ops = make_page_control_ops();
constinit const WindowOps k_iwindow_ops = make_iwindow_ops();
constinit const WindowOps k_iwnd_ops = make_iwnd_ops();

Window* panel_create() {
    return allocate<Window>(&k_panel_ops);
}

Window* dialog_create() {
    return allocate<Window>(&k_dialog_ops);
}

Window* group_box_create() {
    return allocate<Window>(&k_group_box_ops);
}

PageControl* page_control_create() {
    return allocate<PageControl>(&k_page_control_ops);
}

void page_control_add(PageControl* control, Window* tab, Window* page) {
    auto* grown = static_cast<PageControlEntry**>(
        std::realloc(control->pages, sizeof(PageControlEntry*) * (control->page_count + 1))
    );
    auto* entry = static_cast<PageControlEntry*>(std::malloc(sizeof(PageControlEntry)));
    if (grown == nullptr || entry == nullptr) {
        std::abort();
    }
    entry->page = page;
    entry->tab = tab;
    control->pages = grown;
    control->pages[control->page_count++] = entry;
}

Window* page_control_page_for(const PageControl* control, const Window* tab) {
    if (tab == nullptr) {
        return nullptr;
    }
    for (int32_t i = 0; i < control->page_count; ++i) {
        const PageControlEntry* entry = control->pages[i];
        if (entry != nullptr && entry->tab == tab) {
            return entry->page;
        }
    }
    return nullptr;
}

void page_control_select(PageControl* control, Window* page) {
    if (page == control->selected) {
        return;
    }
    if (control->selected != nullptr) {
        control->selected->visible = 0;
        control->selected = nullptr;
    }
    if (page != nullptr) {
        page->visible = 1;
        control->selected = page;
    }
}

IWindow* iwindow_create() {
    auto* w = allocate<IWindow>(&k_iwindow_ops);
    iwindow_init_fields(w);
    return w;
}

void iwindow_init_fields(IWindow* window) {
    window->icon_x = k_iwindow_icon_size;
    window->icon_y = k_iwindow_icon_size;
    window->icon_width = k_iwindow_icon_size;
    window->icon_height = k_iwindow_icon_size;
    window->restore_x = 0;
    window->restore_y = 0;
    window->restore_width = 0;
    window->restore_height = 0;
    window->minimized = 0;
    window_set_state_count(&window->base, k_iwindow_states);
    window_set_state(&window->base, 1);
}

IWnd* iwnd_create() {
    auto* frame = allocate<IWnd>(&k_iwnd_ops);
    iwindow_init_fields(&frame->window);
    return frame;
}

void iwindow_minimize(IWindow* window) {
    if (window->minimized != 0) {
        return;
    }
    Window* w = &window->base;
    window->restore_x = w->x;
    window->restore_y = w->y;
    window->restore_height = w->height;
    window->restore_width = w->width;
    w->x = window->icon_x;
    w->y = window->icon_y;
    window_set_width(w, window->icon_width);
    window_set_height(w, window->icon_height);
    window_set_state(w, 2);
    for (int32_t i = 0; i < w->children.count; ++i) {
        if (w->children.items[i] != nullptr) {
            w->children.items[i]->visible = 0;
        }
    }
    window->minimized = 1;
}

void iwindow_restore(IWindow* window) {
    if (window->minimized == 0) {
        return;
    }
    Window* w = &window->base;
    window->icon_x = w->x;
    window->icon_y = w->y;
    window->icon_height = w->height;
    window->icon_width = w->width;
    w->x = window->restore_x;
    w->y = window->restore_y;
    window_set_width(w, window->restore_width);
    window_set_height(w, window->restore_height);
    window_set_state(w, 1);
    for (int32_t i = 0; i < w->children.count; ++i) {
        if (w->children.items[i] != nullptr) {
            w->children.items[i]->visible = 1;
        }
    }
    window->minimized = 0;
}

} // namespace oa::boneyards
