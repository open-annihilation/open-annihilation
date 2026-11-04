// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Window classes and helpers shared by the console screens.
#include "widgets.hpp"

#include "oa/boneyards/utf8.hpp"

#include <cstdlib>
#include <cstring>

namespace oa::boneyards::console {

namespace {

bool hotspot_draw(Window*) {
    return true;
}

bool panel_is_class(const char* name) {
    return name != nullptr && std::strcmp(name, class_name::panel) == 0;
}

bool panel_draw(Window* self) {
    return window_draw_background(self);
}

WindowOps make_hotspot_ops() {
    WindowOps ops = k_button_ops;
    ops.draw = hotspot_draw;
    return ops;
}

} // namespace

WindowOps console_panel_ops() {
    WindowOps ops = k_window_ops;
    ops.class_name = class_name::panel;
    ops.destroy = console_window_free;
    ops.is_class = panel_is_class;
    ops.draw = panel_draw;
    ops.focus_target = oa::boneyards::k_panel_ops.focus_target;
    return ops;
}

const WindowOps k_hotspot_button_ops = make_hotspot_ops();
const WindowOps k_panel_ops = console_panel_ops();

void console_window_free(Window* self) {
    window_release(self);
    std::free(self);
}

Window* console_window_alloc(std::size_t size, const WindowOps* ops) {
    void* block = std::calloc(1, size);
    if (block == nullptr)
        std::abort();
    auto* window = static_cast<Window*>(block);
    window_init(window, ops);
    return window;
}

void copy_text(char* out, std::size_t capacity, const char* text) {
    oa::boneyards::utf8::copy(out, capacity, text, 0);
}

Window* panel_create() {
    return console_window_alloc(sizeof(Window), &k_panel_ops);
}

Window* hotspot_button_create() {
    Window* button = button_create();
    button->ops = &k_hotspot_button_ops;
    return button;
}

int32_t console_bind_images(
    Window* window, const char* gaf_name, const char* sequence, int32_t first, uint8_t key
) {
    return window->ops->bind_images(window, gaf_name, sequence, first, key);
}

} // namespace oa::boneyards::console
