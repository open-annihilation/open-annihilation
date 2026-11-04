// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The green Console layer of the Boneyards map panel.
#include "console_layer.hpp"

#include "oa/present/blit.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

namespace layout = main_frame_layout;

constexpr const char* k_console_name = "Console";
constexpr const char* k_widgets_gaf = "Widgets.gaf";
constexpr const char* k_green_screen = "Green_Screen";
constexpr int32_t k_image_state = 1;

ConsoleLayer* as_layer(Window* w) {
    return reinterpret_cast<ConsoleLayer*>(w);
}

Rect32 whole_rect(const Surface& surface) {
    return Rect32{0, 0, surface.width - 1, surface.height - 1};
}

// The image a window currently shows, as a drawing surface.
bool state_surface(const Window& w, Surface* surface) {
    const int32_t state = w.states.current;
    if (state < 0 || state >= w.states.count)
        return false;
    return image_surface(w.states.images[state], surface);
}

int32_t read_slide_steps(const screens::SettingsStore& settings) {
    int32_t steps = screens::kSlideStepsDefault;
    if (settings.read_number != nullptr)
        steps = settings.read_number(settings.user, screens::kConsoleSlideKey, steps);
    if (steps > screens::kSlideStepsMax)
        steps = screens::kSlideStepsMax;
    if (steps < screens::kSlideStepsMin)
        steps = screens::kSlideStepsMin;
    return steps;
}

// Copies the viewer rectangle and applies the game's palette remap.
void snapshot_map(ConsoleLayer* layer) {
    const Window& panel = layer->panel->base;
    Surface viewer{};
    if (!state_surface(panel, &viewer))
        return;
    const Window& self = layer->base;
    const int32_t left = self.x - panel.x;
    const int32_t top = self.y - panel.y;
    const Rect32 source{left, top, left + self.width - 1, top + self.height - 1};
    Surface& map = layer->map->surface;
    present::blit_rect(&map, viewer, source, whole_rect(map));
    const std::size_t count = layer->map->pixels.size();
    for (std::size_t i = 0; i < count; ++i)
        layer->green->pixels[i] = k_console_green_remap[layer->map->pixels[i]];
}

// Resets the full-height slide rectangles and countdown.
void arm_slide(ConsoleLayer* layer, ConsoleSlide direction) {
    const Surface& map = layer->map->surface;
    layer->map_part = Rect32{0, 0, 0, map.height - 1};
    layer->green_part = Rect32{0, 0, map.width - 1, map.height - 1};
    layer->border_part = Rect32{0, 0, 0, map.height - 1};
    layer->sliding = 1;
    layer->countdown = layer->slide_steps;
    layer->direction = direction;
}

void show_map(ConsoleLayer* layer, Surface* image) {
    present::blit_surface(image, &layer->map->surface, 0, 0);
}

void show_green(ConsoleLayer* layer, Surface* image) {
    present::blit_surface(image, &layer->green->surface, 0, 0);
    Surface border{};
    if (image_surface(layer->green_screen, &border))
        present::blit_surface_keyed(image, border, 0, 0, resource::console_key);
}

/// Creates the transparent drawing image at the layer's bounds.
///
/// @param self the Console layer
void console_layer_bounds(Window* self) {
    const Handle image =
        image_create_blank(self->width, self->height, resource::console_key, resource::console_key);
    window_set_state_image(self, k_image_state, image);
    image_release(image);
}

/// Refreshes the map snapshot and advances the green slide by one pulse.
///
/// At the end of a slide in, the open screen is presented; at the end of a
/// slide out, the map is handed back and the layer hides.
///
/// @param self the Console layer
void console_layer_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    ConsoleLayer* layer = as_layer(self);
    snapshot_map(layer);
    Surface image{};
    const bool drawable = state_surface(*self, &image);
    const bool entering = layer->direction == ConsoleSlide::in;
    if (layer->sliding == 0) {
        if (!drawable)
            return;
        if (layer->covering == 0)
            show_map(layer, &image);
        else
            show_green(layer, &image);
        return;
    }
    const int32_t countdown = layer->countdown;
    if (countdown == layer->slide_steps) {
        if (drawable)
            entering ? show_map(layer, &image) : show_green(layer, &image);
    } else if (countdown == 0) {
        if (drawable)
            entering ? show_green(layer, &image) : show_map(layer, &image);
    } else if (countdown == -1) {
        layer->sliding = 0;
        if (entering) {
            main_frame_present_screen(layer->frame, true);
        } else {
            layer->covering = 0;
            main_frame_release_map(layer->frame, true);
            self->visible = 0;
        }
    } else {
        const int32_t width = layer->map->surface.width;
        int32_t edge = countdown * width / layer->slide_steps;
        if (!entering)
            edge = width - edge;
        layer->map_part.x2 = edge;
        layer->green_part.x1 = edge + 1;
        layer->border_part.x2 = layer->green_part.x2 - (edge + 1);
        if (drawable) {
            present::blit_rect(&image, layer->map->surface, layer->map_part, layer->map_part);
            present::blit_rect(&image, layer->green->surface, layer->green_part, layer->green_part);
            Surface border{};
            if (image_surface(layer->green_screen, &border))
                present::blit_rect_keyed(
                    &image, border, layer->border_part, layer->green_part, resource::console_key
                );
        }
    }
    --layer->countdown;
}

/// Releases the border and the two snapshot buffers, then the window.
///
/// @param self the Console layer
void console_layer_destroy(Window* self) {
    ConsoleLayer* layer = as_layer(self);
    image_release(layer->green_screen);
    layer->green_screen = k_no_handle;
    delete layer->map;
    layer->map = nullptr;
    delete layer->green;
    layer->green = nullptr;
    console_window_free(self);
}

const WindowOps* console_layer_ops() {
    static const WindowOps ops = [] {
        WindowOps o = console_panel_ops();
        o.destroy = console_layer_destroy;
        o.on_bounds = console_layer_bounds;
        o.pulse = console_layer_pulse;
        return o;
    }();
    return &ops;
}

} // namespace

ConsoleLayer* console_layer_create(MainFrame* frame, GalaxyPanel* panel) {
    auto* layer = reinterpret_cast<ConsoleLayer*>(
        console_window_alloc(sizeof(ConsoleLayer), console_layer_ops())
    );
    layer->frame = frame;
    layer->panel = panel;
    layer->slide_steps = screens::kSlideStepsDefault;
    layer->green_screen = console_images_load(
        frame->owner->images, k_widgets_gaf, k_green_screen, 0, resource::console_key
    );
    int32_t width = layout::console_width;
    int32_t height = layout::console_height;
    if (const Sprite* border = console_images_sprite(frame->owner->images, layer->green_screen)) {
        width = border->width;
        height = border->height;
    }
    layer->map = new present::SurfaceBuffer(present::create_surface(width, height));
    layer->green = new present::SurfaceBuffer(present::create_surface(width, height));
    layer->slide_steps = read_slide_steps(frame->owner->navigation->profile.settings);
    window_create(
        &layer->base,
        k_console_name,
        layout::console_x,
        layout::console_y,
        layout::console_width,
        layout::console_height,
        &panel->base,
        false
    );
    return layer;
}

void console_layer_start(ConsoleLayer* layer) {
    layer->base.visible = 1;
    snapshot_map(layer);
    arm_slide(layer, ConsoleSlide::in);
    layer->covering = 1;
}

void console_layer_reverse(ConsoleLayer* layer) {
    arm_slide(layer, ConsoleSlide::out);
}

} // namespace oa::boneyards::console
