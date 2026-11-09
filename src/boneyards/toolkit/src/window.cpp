// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Window base class: construction, the parent/child tree, geometry, per-state
// resources, message dispatch and the default slot bodies.
#include <cstdlib>
#include <cstring>

#include "list.hpp"
#include "ops_defaults.hpp"
#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards {
namespace {

constexpr int32_t k_default_state_count = 2;
constexpr int32_t k_state_normal = 1;
constexpr int32_t k_desktop_margin = 5; // pixels a top-level window must stay on screen

// Base state names ("Disabled", "Normal").
const char* const k_base_state_names[] = {"Disabled", "Normal"};

void release_state_handles(Window* self) {
    WindowStates& s = self->states;
    for (int32_t i = 0; i < s.count; ++i) {
        if (s.images[i] >= 0) {
            image_release(s.images[i]);
            s.images[i] = k_no_handle;
        }
        if (s.texts[i] >= 0) {
            text_release(s.texts[i]);
            s.texts[i] = k_no_handle;
        }
        if (s.sounds[i] >= 0) {
            sound_release(s.sounds[i]);
            s.sounds[i] = k_no_handle;
        }
    }
}

Handle* resize_handles(Handle* handles, int32_t old_count, int32_t count) {
    if (count <= 0) {
        std::free(handles);
        return nullptr;
    }
    auto* resized = static_cast<Handle*>(std::realloc(handles, sizeof(Handle) * count));
    if (resized == nullptr) {
        std::abort();
    }
    for (int32_t i = old_count; i < count; ++i) {
        resized[i] = k_no_handle;
    }
    return resized;
}

bool state_in_range(const Window* self, int32_t state) {
    return state >= 0 && state < self->states.count;
}

// Rectangle a window may occupy: its parent's, or the desktop inset so at
// least a margin stays visible.
void movement_bounds(const Window* self, int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    if (self->parent == nullptr) {
        const int32_t area_w = services_desktop_width();
        const int32_t area_h = services_desktop_height();
        *x1 = k_desktop_margin - self->width;
        *y1 = k_desktop_margin - self->height;
        *x2 = area_w - k_desktop_margin + self->width;
        *y2 = area_h - k_desktop_margin + self->height;
        return;
    }
    const Window* p = self->parent;
    *x1 = p->x;
    *y1 = p->y;
    *x2 = p->x + p->width;
    *y2 = p->y + p->height;
}

} // namespace

bool names_equal(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

namespace detail {

// Destroy-time teardown shared by the plain Window's destroy handler.
void window_plain_destroy(Window* self) {
    window_release(self);
    std::free(self);
}

const char* window_plain_state_text(Window* self, int32_t state) {
    if (state == -1) {
        state = self->states.current;
    }
    // A state outside the window's states reads as "".
    if (!state_in_range(self, state)) {
        return "";
    }
    return text_get(self->states.texts[state]);
}

int32_t window_plain_state_index(Window* self, const char* state_name) {
    if (state_name == nullptr) {
        return -1;
    }
    // At most the two base names are searched, however many states the
    // window has.
    const int32_t limit = self->states.count < 2 ? self->states.count : 2;
    for (int32_t i = 0; i < limit; ++i) {
        if (std::strcmp(k_base_state_names[i], state_name) == 0) {
            return i;
        }
    }
    return -1;
}

const char* window_plain_state_name(Window* self, int32_t state) {
    if (state < 0 || state >= self->states.count || state >= 2) {
        return nullptr;
    }
    return k_base_state_names[state];
}

bool window_plain_is_class(const char* name) {
    return names_equal(name, class_name::window);
}

int32_t window_plain_bind_images(
    Window* self, const char* file, const char* sequence, int32_t first, int32_t key
) {
    WindowStates& s = self->states;
    if (file == nullptr || file[0] == '\0') {
        for (int32_t i = 0; i < s.count; ++i) {
            if (s.images[i] >= 0) {
                image_release(s.images[i]);
            }
            s.images[i] = k_no_handle;
        }
        return first;
    }
    if (sequence == nullptr) {
        return first;
    }
    for (int32_t i = 0; i < s.count; ++i) {
        if (s.images[i] >= 0) {
            image_release(s.images[i]);
        }
        s.images[i] = image_load(file, sequence, first, key);
        ++first;
    }
    return first;
}

int32_t window_plain_type_code() {
    return 0;
}

void noop_window(Window*) {
}

void noop_notify(Window*, const Notify*) {
}

bool false_window(Window*) {
    return false;
}

bool window_plain_draw(Window*) {
    return true;
}

bool false_message(Window*, const Message*) {
    return false;
}

void noop_caption(Window*, const char*, int32_t) {
}

void gadget_raise(Window* self) {
    if (self->parent != nullptr) {
        self->parent->ops->raise(self->parent);
        return;
    }
    services_desktop_raise(self);
}

bool window_plain_focus_gained(Window* self) {
    self->focused = 1;
    self->ops->raise(self);
    return true;
}

bool window_plain_focus_lost(Window* self) {
    self->focused = 0;
    return true;
}

} // namespace detail

constinit const WindowOps k_window_ops = detail::plain_window_ops();

void window_init(Window* self, const WindowOps* ops) {
    *self = Window{};
    self->ops = ops;
    self->focusable = 1;
    self->color = k_no_handle;
    self->cursor = -1;
    self->font = k_no_handle;
    self->text_handle = k_no_handle;
    self->reserved_after_text_handle = 1;
    window_set_state_count(self, k_default_state_count);
    window_set_state(self, k_state_normal);
    window_set_color(self, 0xFF, 0xFF, 0xFF, 0xFF);
}

void window_release(Window* self) {
    while (self->children.count > 0) {
        Window* child = self->children.items[0];
        if (child == nullptr) {
            list_remove_at(&self->children, 0);
            continue;
        }
        window_destroy(child);
    }
    std::free(self->name);
    self->name = nullptr;
    if (self->color >= 0) {
        color_release(self->color);
        self->color = k_no_handle;
    }
    if (self->font >= 0) {
        font_release(self->font);
        self->font = k_no_handle;
    }
    release_state_handles(self);
    if (self->text_handle >= 0) {
        text_release(self->text_handle);
        self->text_handle = k_no_handle;
    }
    std::free(self->states.images);
    std::free(self->states.texts);
    std::free(self->states.sounds);
    self->states = WindowStates{};
    list_free(&self->children);
}

Window* window_create_plain() {
    auto* window = static_cast<Window*>(std::malloc(sizeof(Window)));
    if (window == nullptr) {
        std::abort();
    }
    window_init(window, &k_window_ops);
    return window;
}

void window_destroy(Window* self) {
    if (self == nullptr) {
        return;
    }
    if (self->focused != 0) {
        services_desktop_focus(nullptr);
    }
    if (services_desktop_captured() == self) {
        services_desktop_release_capture();
    }
    if (services_desktop_hover() == self) {
        services_desktop_set_hover(nullptr);
    }
    self->ops->on_destroy(self);
    while (self->children.count > 0) {
        Window* child = self->children.items[0];
        if (child == nullptr) {
            list_remove_at(&self->children, 0);
            continue;
        }
        window_destroy(child);
    }
    if (self->parent != nullptr) {
        list_remove(&self->parent->children, self);
        self->parent = nullptr;
    } else {
        services_desktop_remove(self);
    }
    self->ops->destroy(self);
}

void window_create(
    Window* self,
    const char* name,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    Window* parent,
    bool visible
) {
    window_set_name(self, name);
    window_set_bounds(self, x, y, width, height, parent, visible);
}

void window_set_bounds(
    Window* self, int32_t x, int32_t y, int32_t width, int32_t height, Window* parent, bool visible
) {
    self->x = x;
    self->y = y;
    self->width = width;
    self->height = height;
    window_set_parent(self, parent);
    self->visible = visible ? 1 : 0;
    self->ops->on_bounds(self);
}

void window_set_width(Window* self, int32_t width) {
    self->width = width;
    self->ops->refresh(self);
}

void window_set_height(Window* self, int32_t height) {
    self->height = height;
    self->ops->refresh(self);
}

void window_set_size(Window* self, int32_t width, int32_t height) {
    self->width = width;
    self->height = height;
    self->ops->refresh(self);
}

void window_set_rect(Window* self, const Rect32& rect) {
    self->x = rect.x1;
    self->y = rect.y1;
    self->width = rect.x2 - rect.x1 + 1;
    self->height = rect.y2 - rect.y1 + 1;
    self->ops->refresh(self);
}

void window_set_name(Window* self, const char* name) {
    if (name == nullptr) {
        return;
    }
    std::free(self->name);
    const size_t length = std::strlen(name) + 1;
    self->name = static_cast<char*>(std::malloc(length));
    if (self->name != nullptr) {
        std::memcpy(self->name, name, length);
    }
}

void window_set_parent(Window* self, Window* parent, int32_t position) {
    if (self->parent == nullptr) {
        services_desktop_remove(self);
    } else {
        list_remove(&self->parent->children, self);
    }
    self->parent = parent;
    if (parent != nullptr) {
        if (self->font == k_no_handle && parent->font >= 0) {
            font_add_ref(parent->font);
            self->font = parent->font;
        }
        list_insert_unique(&parent->children, self, position);
        return;
    }
    services_desktop_add(self);
}

void window_set_state(Window* self, int32_t state) {
    if (state_in_range(self, state)) {
        self->states.current = state;
    }
}

void window_set_state_count(Window* self, int32_t count) {
    if (count < 0) {
        count = 0;
    }
    release_state_handles(self);
    WindowStates& s = self->states;
    s.images = resize_handles(s.images, s.count, count);
    s.texts = resize_handles(s.texts, s.count, count);
    s.sounds = resize_handles(s.sounds, s.count, count);
    s.count = count;
}

void window_set_state_image(Window* self, int32_t state, Handle image) {
    if (!state_in_range(self, state)) {
        return;
    }
    Handle& slot = self->states.images[state];
    if (slot >= 0) {
        image_release(slot);
    }
    slot = image;
    image_add_ref(image);
}

void window_set_state_text(Window* self, int32_t state, const char* text, int32_t style) {
    if (!state_in_range(self, state)) {
        return;
    }
    Handle& slot = self->states.texts[state];
    if (slot >= 0) {
        text_release(slot);
    }
    slot = text != nullptr && text[0] != '\0' ? text_create(text, style) : k_no_handle;
}

void window_set_state_sound(Window* self, int32_t state, Handle sound) {
    if (state < 0 || state >= self->states.count) {
        return;
    }
    Handle& slot = self->states.sounds[state];
    if (slot >= 0) {
        sound_release(slot);
    }
    slot = sound;
}

void window_set_font_handle(Window* self, Handle font) {
    if (self->font >= 0)
        font_release(self->font);
    if (font >= 0)
        font_add_ref(font);
    self->font = font >= 0 ? font : k_no_handle;
}

void window_set_font(Window* self, const char* name) {
    if (self->font >= 0) {
        font_release(self->font);
    }
    self->font = font_load(name);
}

void window_set_color(Window* self, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (self->color >= 0) {
        color_release(self->color);
    }
    self->color = color_create(r, g, b, a);
}

void window_move_to(Window* self, int32_t x, int32_t y) {
    const int32_t old_x = self->x;
    const int32_t old_y = self->y;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    movement_bounds(self, &x1, &y1, &x2, &y2);
    if (x < x1) {
        self->x = x1;
    } else if (x2 < x + self->width) {
        self->x = x2 - self->width;
    } else {
        self->x = x;
    }
    if (y < y1) {
        self->y = y1;
    } else if (y2 < self->height + y) {
        self->y = y2 - self->height;
    } else {
        self->y = y;
    }
    for (int32_t i = 0; i < self->children.count; ++i) {
        if (self->children.items[i] != nullptr) {
            window_move_by(self->children.items[i], self->x - old_x, self->y - old_y);
        }
    }
}

void window_move_by(Window* self, int32_t dx, int32_t dy) {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    movement_bounds(self, &x1, &y1, &x2, &y2);
    if (dx + self->x < x1) {
        dx = x1 - self->x;
    } else if (x2 < self->x + self->width + dx) {
        dx = (x2 - self->x) - self->width;
    }
    if (self->y + dy < y1) {
        dy = y1 - self->y;
    } else if (y2 < self->y + self->height + dy) {
        dy = (y2 - self->y) - self->height;
    }
    self->y += dy;
    self->x += dx;
    for (int32_t i = 0; i < self->children.count; ++i) {
        if (self->children.items[i] != nullptr) {
            window_move_by(self->children.items[i], dx, dy);
        }
    }
}

bool window_is_shown(const Window* self) {
    if (self->parent == nullptr) {
        return self->visible != 0;
    }
    return window_is_shown(self->parent) && self->visible != 0;
}

bool window_is_named(const Window* self, const char* name) {
    return name != nullptr && self->name != nullptr && std::strcmp(self->name, name) == 0;
}

Window* window_find_named(Window* self, const char* name, const char* class_filter) {
    if (name == nullptr) {
        return nullptr;
    }
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child == nullptr) {
            continue;
        }
        if (window_is_named(child, name) &&
            (class_filter == nullptr || child->ops->is_class(class_filter))) {
            return child;
        }
        Window* found = window_find_named(child, name, class_filter);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

Window* window_hit_test(Window* self, int32_t x, int32_t y) {
    if (!window_is_shown(self) || !self->ops->contains(self, x, y)) {
        return nullptr;
    }
    Window* hit = self;
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child == nullptr) {
            continue;
        }
        Window* found = window_hit_test(child, x, y);
        if (found != nullptr) {
            hit = found;
        }
    }
    return hit;
}

bool window_dispatch(Window* self, const Message* msg) {
    const WindowOps* ops = self->ops;
    switch (msg->code) {
    case MessageCode::draw:
        return ops->draw(self);
    case MessageCode::reserved_2:
        return ops->on_reserved_2(self);
    case MessageCode::reserved_3:
        return ops->on_reserved_3(self);
    case MessageCode::focus_gained:
        return ops->focus_gained(self);
    case MessageCode::focus_lost:
        return ops->focus_lost(self);
    case MessageCode::key_down:
        return ops->key_down(self, msg);
    case MessageCode::key_up:
        return ops->key_up(self, msg);
    case MessageCode::mouse_move:
        return ops->mouse_move(self, msg);
    case MessageCode::left_down:
        return ops->left_down(self, msg);
    case MessageCode::left_up:
        return ops->left_up(self, msg);
    case MessageCode::left_double:
        return ops->left_double(self, msg);
    case MessageCode::right_down:
        return ops->right_down(self, msg);
    case MessageCode::right_up:
        return ops->right_up(self, msg);
    case MessageCode::right_double:
        return ops->right_double(self, msg);
    case MessageCode::middle_down:
        return ops->middle_down(self, msg);
    case MessageCode::middle_up:
        return ops->middle_up(self, msg);
    case MessageCode::middle_double:
        return ops->middle_double(self, msg);
    case MessageCode::wheel:
        return ops->wheel(self, msg);
    }
    return false;
}

bool window_send(Window* self, MessageCode code) {
    Message msg{};
    msg.code = code;
    return window_dispatch(self, &msg);
}

void window_draw_children(Window* self) {
    Message msg{};
    msg.code = MessageCode::draw;
    for (int32_t i = 0; i < self->children.count; ++i) {
        if (self->children.items[i] != nullptr) {
            window_dispatch(self->children.items[i], &msg);
        }
    }
}

bool window_draw_background(Window* self) {
    if (!window_is_shown(self)) {
        return detail::window_plain_draw(self);
    }
    const Handle image = state_in_range(self, self->states.current)
                             ? self->states.images[self->states.current]
                             : k_no_handle;
    if (image >= 0) {
        image_draw(self->x, self->y, self->width, self->height, image);
    } else if (self->color >= 0) {
        color_fill(self->x, self->y, self->width, self->height, self->color);
    }
    window_draw_children(self);
    return detail::window_plain_draw(self);
}

Window* window_default_focus_target(Window* self, Window* from, bool backwards) {
    if (window_is_shown(self) && self->focusable != 0) {
        return self;
    }
    Window* parent = self->parent;
    if (parent != nullptr && from != parent) {
        return parent->ops->focus_target(parent, self, backwards);
    }
    return nullptr;
}

void window_default_raise(Window* self) {
    Window* parent = self->parent;
    if (parent == nullptr) {
        services_desktop_raise(self);
        return;
    }
    if (list_remove(&parent->children, self)) {
        list_add_unique(&parent->children, self);
    }
    parent->ops->raise(parent);
}

bool window_default_contains(Window* self, int32_t x, int32_t y) {
    return x >= self->x && x <= self->x + self->width && y >= self->y &&
           y <= self->y + self->height;
}

Window* window_default_child_at(Window* self, int32_t x, int32_t y) {
    if (!self->ops->contains(self, x, y) || !window_is_shown(self)) {
        return nullptr;
    }
    Window* hit = self;
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child == nullptr) {
            continue;
        }
        Window* found = child->ops->child_at(child, x, y);
        if (found != nullptr) {
            hit = found;
        }
    }
    return hit;
}

bool window_default_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    // F6 is the desktop's, and only while a dialog or a region confines it.
    // An unconfined desktop leaves the key for whoever had it before.
    if (msg->key == key::f6)
        return false;
    // Tab is this control's when nothing confines it, as it was: the parent
    // (or this window) picks the next target. A dialog or a region takes the
    // key before it gets here.
    if (msg->key == key::tab) {
        const bool backwards = input_key_down(key::shift);
        Window* owner = self->parent != nullptr ? self->parent : self;
        Window* target = owner->ops->focus_target(owner, self, backwards);
        if (target != nullptr)
            services_desktop_focus(target);
        return true;
    }
    if (self->parent != nullptr) {
        return self->parent->ops->key_down(self->parent, msg);
    }
    return false;
}

bool window_default_key_up(Window* self, const Message* msg) {
    if (msg != nullptr && self->parent != nullptr) {
        return self->parent->ops->key_up(self->parent, msg);
    }
    return false;
}

bool window_default_mouse_move(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    if (services_desktop_captured() != self && !self->ops->contains(self, msg->x, msg->y)) {
        return false;
    }
    cursor_set(self->cursor);
    return true;
}

void window_play_sound(Window* self, int32_t index) {
    if (index >= 0 && index < self->states.count) {
        sound_play(self->states.sounds[index]);
    }
}

void window_pulse_tree(Window* self) {
    self->ops->pulse(self);
    for (int32_t i = 0; i < self->children.count; ++i) {
        if (self->children.items[i] != nullptr) {
            window_pulse_tree(self->children.items[i]);
        }
    }
}

void window_notify_parent(Window* self, NotifyCode code, int32_t value) {
    if (self->parent == nullptr) {
        return;
    }
    Notify notify{code, self, value, nullptr};
    self->parent->ops->on_notify(self->parent, &notify);
}

} // namespace oa::boneyards
