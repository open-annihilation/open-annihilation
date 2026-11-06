// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Galaxy map panel of the Main Frame: the war-map view with its corner
// frames and planet hotspots, the magnification slider and the screen-edge
// scroll zones, all driven by the GalaxyView model of the navigation state.
#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/present/blit.hpp"
#include "oa/present/display.hpp"
#include "widgets.hpp"

#include <cstdio>
#include <cstring>

namespace oa::boneyards::console {

namespace {

namespace layout = main_frame_layout;
using screens::GalaxyPlanet;
using screens::GalaxyView;

constexpr const char* k_map_panel_name = "MapPanel";
constexpr const char* k_map_background = "GMap_BG";
constexpr const char* k_corners = "Corners";
constexpr const char* k_corner_names[k_map_corner_count] = {"TL", "TR", "BL", "BR"};
constexpr const char* k_slider_name = "Mag_Slider";
constexpr const char* k_slider_sequence = "Mag_ScrollBar";
constexpr const char* k_slider_thumb = "sbutton";
constexpr const char* k_slider_dec = "decbutton";
constexpr const char* k_slider_inc = "incbutton";
constexpr const char* k_hover_sound = "MORE";
constexpr const char* k_select_sound = "GREEN_BUTTON";

constexpr int32_t k_slider_x = 0x216, k_slider_y = 0x1c0, k_slider_width = 0x59,
                  k_slider_height = 0x15;
constexpr int32_t k_slider_button_width = 0x12, k_slider_thumb_width = 0xb;
constexpr int32_t k_slider_thumb_frame = 9, k_slider_dec_frame = 1, k_slider_inc_frame = 5;
constexpr int32_t k_slider_states = 3;

// Corner frame of the "Corners" sequence at each panel corner.
constexpr int32_t k_corner_frames[k_map_corner_count] = {3, 0, 2, 1};

struct ScrollerSpec {
    int32_t x, y, width, height;
    int32_t dx, dy;
};

// The eight screen-edge zones: top, top-right, right, bottom-right, bottom,
// bottom-left, left, top-left.
constexpr ScrollerSpec k_scrollers[k_map_scroller_count] = {
    {8, 0, 0x270, 8, 0, -1},
    {0x278, 0, 8, 8, 1, -1},
    {0x278, 8, 8, 0x1d0, 1, 0},
    {0x278, 0x1d8, 8, 8, 1, 1},
    {8, 0x1d8, 0x270, 8, 0, 1},
    {0, 0x1d8, 8, 8, -1, 1},
    {0, 8, 8, 0x1d0, -1, 0},
    {0, 0, 8, 8, -1, -1},
};

GalaxyView* galaxy_of(MainFrame* frame) {
    return &frame->owner->navigation->galaxy;
}

GalaxyPanel* as_panel(Window* w) {
    return reinterpret_cast<GalaxyPanel*>(w);
}

MapScroller* as_scroller(Window* w) {
    return reinterpret_cast<MapScroller*>(w);
}

PlanetHotspot* as_hotspot(Window* w) {
    return reinterpret_cast<PlanetHotspot*>(w);
}

void play_sound(const MainFrame* frame, const char* name) {
    const MainMenuHost& host = frame->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

void image_size(Handle image, int32_t* width, int32_t* height) {
    *width = 0;
    *height = 0;
    const ImageService* images = services().images;
    if (images != nullptr && images->size != nullptr && image != k_no_handle)
        images->size(images->self, image, width, height);
}

void panel_destroy(Window* self) {
    GalaxyPanel* panel = as_panel(self);
    for (Window*& hotspot : panel->hotspots)
        hotspot = nullptr;
    image_release(panel->background);
    panel->background = k_no_handle;
    panel->console = nullptr;
    console_window_free(self);
}

/// Draws the war map panel.
///
/// Clears the viewer image, draws GMap_BG (placed by its frame origin) and
/// the map into it and shows it, then the green layer while it covers the
/// map, then the four corners. The hotspots and the layer are never drawn as
/// children.
///
/// @param self the map panel
/// @return true always
bool map_panel_draw(Window* self) {
    GalaxyPanel* panel = as_panel(self);
    const Handle viewer_image = self->states.images[self->states.current];
    Surface viewer{};
    if (image_surface(viewer_image, &viewer)) {
        present::clear_surface(&viewer, 0);
        StatusWidget* owner = panel->frame->owner;
        present::draw_sprite(
            &viewer, console_images_sprite(owner->images, panel->background), 0, 0
        );
        if (owner->galaxy_graphics != nullptr)
            galaxy_draw(*owner->galaxy_graphics, owner->navigation->galaxy, &viewer);
    }
    image_draw(self->x, self->y, self->width, self->height, viewer_image);
    ConsoleLayer* console = panel->console;
    if (console != nullptr && console->covering != 0)
        console->base.ops->draw(&console->base);
    for (Window* corner : panel->corners)
        corner->ops->draw(corner);
    return true;
}

// Dispatch tables are built on first use: they copy tables that other
// translation units initialise dynamically.
const WindowOps* galaxy_panel_ops() {
    static const WindowOps ops = [] {
        WindowOps o = console_panel_ops();
        o.destroy = panel_destroy;
        o.draw = map_panel_draw;
        return o;
    }();
    return &ops;
}

/// Enables Recon, Chat and Battle for a newly selected planet, then refreshes the planet summary
/// and paints its picture.
///
/// The model already holds the planet.
///
/// @param[in,out] frame Main Frame
void enable_planet_buttons(MainFrame* frame) {
    for (ConsoleButton which : {ConsoleButton::recon, ConsoleButton::chat, ConsoleButton::battle})
        window_set_state(
            frame->console_buttons[static_cast<int32_t>(which)], button_state::unpressed
        );
    if (frame->battle_summary != nullptr) {
        battle_summary_update(frame->battle_summary);
        battle_summary_draw(frame->battle_summary);
    }
}

/// Makes the planet under the pointer the hovered planet, with the MORE sound, unless the map is
/// locked.
///
/// @param self the planet hotspot
void hotspot_enter(Window* self) {
    PlanetHotspot* hotspot = as_hotspot(self);
    if (screens::galaxy_hover_planet(galaxy_of(hotspot->frame), hotspot->planet))
        play_sound(hotspot->frame, k_hover_sound);
}

/// Clears the hovered planet unless the map is locked.
///
/// @param self the planet hotspot
void hotspot_leave(Window* self) {
    screens::galaxy_hover_planet(galaxy_of(as_hotspot(self)->frame), screens::kNoPlanet);
}

/// The facts a program driving the game reads from the hotspot.
///
/// @param self the planet hotspot
/// @param state unused
/// @return the facts
const char* hotspot_state_text(Window* self, int32_t /*state*/) {
    return as_hotspot(self)->facts;
}

/// Selects the hotspot's planet on a press unless the map is locked.
///
/// Sounds GREEN_BUTTON, sets the current area, the territory summary and the
/// Recon, Chat and Battle buttons, then runs the activity light until the
/// census asked for the planet arrives. It replaces the button press, so the
/// hotspot is never pressed or activated.
///
/// @param self the planet hotspot
/// @param msg mouse message; not read
/// @return true always
bool hotspot_left_down(Window* self, const Message* /*msg*/) {
    PlanetHotspot* hotspot = as_hotspot(self);
    MainFrame* frame = hotspot->frame;
    screens::BoneyardsScreens* nav = frame->owner->navigation;
    if (nav->galaxy.locked)
        return true;
    play_sound(frame, k_select_sound);
    screens::galaxy_select_planet(&nav->galaxy, hotspot->planet, &nav->session.area_id);
    enable_planet_buttons(frame);
    activity_light_start(frame->activity_light);
    main_frame_request_census(frame);
    return true;
}

const WindowOps* hotspot_ops() {
    static const WindowOps ops = [] {
        WindowOps o = k_hotspot_button_ops;
        o.mouse_enter = hotspot_enter;
        o.mouse_leave = hotspot_leave;
        o.left_down = hotspot_left_down;
        o.state_text = hotspot_state_text;
        return o;
    }();
    return &ops;
}

/// Arms the scroll zone while the pointer rests on it.
///
/// @param self the map scroller
void scroller_enter(Window* self) {
    as_scroller(self)->hovered = 1;
}

/// Disarms the scroll zone as the pointer leaves it.
///
/// @param self the map scroller
void scroller_leave(Window* self) {
    as_scroller(self)->hovered = 0;
}

/// Scrolls the map by the zone's step each cycle while the pointer rests on it and the map is not
/// locked.
///
/// @param self the map scroller
void scroller_pulse(Window* self) {
    MapScroller* scroller = as_scroller(self);
    GalaxyView* galaxy = galaxy_of(scroller->frame);
    if (galaxy->locked || scroller->hovered == 0)
        return;
    screens::galaxy_scroll(galaxy, scroller->dx, scroller->dy, false);
    galaxy_panel_sync_hotspots(scroller->frame);
}

const WindowOps* scroller_ops() {
    static const WindowOps ops = [] {
        WindowOps o = k_hotspot_button_ops;
        o.mouse_enter = scroller_enter;
        o.mouse_leave = scroller_leave;
        o.pulse = scroller_pulse;
        return o;
    }();
    return &ops;
}

/// Builds the map panel over the info area.
///
/// Adds the "ViewerSurf" image the war map is drawn into (keyed on index 9),
/// the GMap_BG background, the hidden green Console layer and the four
/// corner frames, which only the panel's draw paints. The viewer starts at
/// index 0, the value each draw clears it to.
///
/// @param[in,out] frame Main Frame
/// @return the map panel
GalaxyPanel* map_panel_create(MainFrame* frame) {
    auto* panel = reinterpret_cast<GalaxyPanel*>(
        console_window_alloc(sizeof(GalaxyPanel), galaxy_panel_ops())
    );
    panel->frame = frame;
    panel->background = k_no_handle;
    window_create(
        &panel->base,
        k_map_panel_name,
        layout::info_x,
        layout::info_y,
        layout::info_width,
        layout::info_height,
        &frame->base,
        true
    );
    // The viewer starts at index 0, the value each draw clears it to.
    const Handle viewer =
        image_create_blank(panel->base.width, panel->base.height, 0, resource::console_key);
    window_set_state_image(&panel->base, 1, viewer);
    image_release(viewer);
    ConsoleImages* images = frame->owner->images;
    panel->background = console_images_load(
        images, resource::main_console_gaf, k_map_background, 0, resource::console_key
    );
    panel->console = console_layer_create(frame, panel);
    for (int32_t i = 0; i < k_map_corner_count; ++i) {
        const Handle corner = console_images_load(
            images, resource::main_console_gaf, k_corners, k_corner_frames[i], resource::console_key
        );
        int32_t width = 0;
        int32_t height = 0;
        image_size(corner, &width, &height);
        const int32_t x = (i % 2 == 0) ? panel->base.x : panel->base.x + panel->base.width - width;
        const int32_t y = (i < 2) ? panel->base.y : panel->base.y + panel->base.height - height;
        Window* w = panel_create();
        window_create(w, k_corner_names[i], x, y, width, height, &panel->base, true);
        window_set_state_image(w, 1, corner);
        image_release(corner);
        panel->corners[i] = w;
    }
    // The Main Frame is built after the system object's fonts load, so the
    // war-map graphics take the font table from here.
    StatusWidget* owner = frame->owner;
    if (owner->galaxy_graphics != nullptr) {
        owner->galaxy_graphics->label_toolkit = owner->toolkit;
        owner->galaxy_graphics->system_fonts = &owner->system.hat_fonts;
    }
    return panel;
}

/// Builds the magnification slider's track and buttons.
///
/// Mag_ScrollBar frame 0 is the track, with the thumb (frame 9), decrement
/// (frames 1..3) and increment (5..7) buttons and four positions stepping
/// one at a time.
///
/// @param frame Main Frame whose image store loads the frames
/// @param[in,out] slider the Mag_Slider
void mag_slider_build(MainFrame* frame, Slider* slider) {
    ConsoleImages* images = frame->owner->images;
    const Handle track = console_images_load(
        images, resource::main_console_gaf, k_slider_sequence, 0, resource::console_key
    );
    window_set_state_count(&slider->base, k_slider_states);
    for (int32_t state = 0; state < k_slider_states; ++state)
        window_set_state_image(&slider->base, state, track);
    image_release(track);
    const int32_t x = slider->base.x;
    const int32_t y = slider->base.y;
    const int32_t height = slider->base.height;

    Window* thumb = scroll_button_create();
    window_create(
        thumb,
        k_slider_thumb,
        x + k_slider_button_width,
        y,
        k_slider_thumb_width,
        height,
        &slider->base,
        true
    );
    const Handle thumb_image = console_images_load(
        images,
        resource::main_console_gaf,
        k_slider_sequence,
        k_slider_thumb_frame,
        resource::console_key
    );
    window_set_state_count(thumb, k_slider_states);
    for (int32_t state = 0; state < k_slider_states; ++state)
        window_set_state_image(thumb, state, thumb_image);
    image_release(thumb_image);

    Window* dec = scroll_button_create();
    window_create(dec, k_slider_dec, x, y, k_slider_button_width, height, &slider->base, true);
    console_bind_images(
        dec,
        resource::main_console_gaf,
        k_slider_sequence,
        k_slider_dec_frame,
        resource::console_key
    );
    Window* inc = scroll_button_create();
    window_create(
        inc,
        k_slider_inc,
        x + slider->base.width - k_slider_button_width,
        y,
        k_slider_button_width,
        height,
        &slider->base,
        true
    );
    console_bind_images(
        inc,
        resource::main_console_gaf,
        k_slider_sequence,
        k_slider_inc_frame,
        resource::console_key
    );
    slider_set_range(slider, screens::kGalaxyScaleCount, false);
    slider->page_step = 1;
    slider_set_index(slider, 0);
}

/// The name of one screen-edge scroll zone, from the direction it scrolls.
///
/// @param dx horizontal steps
/// @param dy vertical steps
/// @return the name
const char* scroller_title(int32_t dx, int32_t dy) {
    if (dx == 0 && dy < 0)
        return "Map up";
    if (dx > 0 && dy < 0)
        return "Map up right";
    if (dx > 0 && dy == 0)
        return "Map right";
    if (dx > 0 && dy > 0)
        return "Map down right";
    if (dx == 0 && dy > 0)
        return "Map down";
    if (dx < 0 && dy > 0)
        return "Map down left";
    if (dx < 0 && dy == 0)
        return "Map left";
    return "Map up left";
}

MapScroller* scroller_create(MainFrame* frame, const ScrollerSpec& spec) {
    auto* scroller =
        reinterpret_cast<MapScroller*>(console_window_alloc(sizeof(MapScroller), scroller_ops()));
    scroller->frame = frame;
    scroller->dx = spec.dx;
    scroller->dy = spec.dy;
    window_create(
        &scroller->base,
        scroller_title(spec.dx, spec.dy),
        spec.x,
        spec.y,
        spec.width,
        spec.height,
        &frame->base,
        true
    );
    window_set_state_count(&scroller->base, button_state::count);
    window_set_state(&scroller->base, button_state::unpressed);
    return scroller;
}

} // namespace

void galaxy_panel_planet_label(
    const screens::GalaxyView& galaxy, std::size_t index, char* out, std::size_t capacity
) {
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (index >= galaxy.planet_count)
        return;
    const screens::GalaxyPlanet& planet = galaxy.planets[index];
    int same = 0;
    for (std::size_t at = 0; at < galaxy.planet_count; ++at) {
        if (std::strcmp(galaxy.planets[at].name, planet.name) == 0)
            ++same;
    }
    if (planet.name[0] == '\0' || same > 1) {
        if (planet.name[0] == '\0')
            std::snprintf(out, capacity, "#%d", planet.id);
        else
            std::snprintf(out, capacity, "%s #%d", planet.name, planet.id);
        return;
    }
    std::snprintf(out, capacity, "%s", planet.name);
}

namespace {

/// Writes the hotspot's name and the facts a program driving the game reads.
///
/// @param[in,out] hotspot the planet button
/// @param galaxy the war map
void describe_hotspot(PlanetHotspot* hotspot, const screens::GalaxyView& galaxy) {
    const auto index = static_cast<std::size_t>(hotspot->planet);
    char label[48];
    galaxy_panel_planet_label(galaxy, index, label, sizeof label);
    if (hotspot->base.name == nullptr || std::strcmp(hotspot->base.name, label) != 0)
        window_set_name(&hotspot->base, label);
    if (index >= galaxy.planet_count) {
        hotspot->facts[0] = '\0';
        return;
    }
    const screens::GalaxyPlanet& planet = galaxy.planets[index];
    const char owner = planet.side != 0 ? planet.side : '?';
    std::snprintf(
        hotspot->facts,
        sizeof hotspot->facts,
        "id=%d owner=%c battles=%d forming=%d",
        planet.id,
        owner,
        planet.in_progress,
        planet.forming
    );
}

} // namespace

Slider* main_frame_create_mag_slider(MainFrame* frame) {
    Slider* slider = slider_create();
    window_create(
        &slider->base,
        k_slider_name,
        k_slider_x,
        k_slider_y,
        k_slider_width,
        k_slider_height,
        &frame->base,
        true
    );
    mag_slider_build(frame, slider);
    slider_set_index(slider, galaxy_of(frame)->scale);
    return slider;
}

void main_frame_create_map(MainFrame* frame) {
    frame->map_panel = map_panel_create(frame);
    frame->console = frame->map_panel->console;
    for (int32_t i = 0; i < k_map_scroller_count; ++i)
        frame->scrollers[i] = scroller_create(frame, k_scrollers[i]);
    galaxy_panel_sync_hotspots(frame);
}

void main_frame_lock_map(MainFrame* frame) {
    slider_disable(&frame->mag_slider->base);
    GalaxyView* galaxy = galaxy_of(frame);
    galaxy->hovered = screens::kNoPlanet;
    galaxy->locked = true;
    frame->map_panel->base.focusable = 0;
}

void main_frame_unlock_map(MainFrame* frame) {
    slider_enable(&frame->mag_slider->base);
    galaxy_of(frame)->locked = false;
    frame->map_panel->base.focusable = 1;
}

std::size_t galaxy_panel_planets_drawn(const MainFrame* frame) {
    const GalaxyPanel* panel = frame->map_panel;
    const StatusWidget* owner = frame->owner;
    if (panel == nullptr || owner->galaxy_graphics == nullptr ||
        owner->image_service.sprite == nullptr)
        return 0;
    const Window& base = panel->base;
    const Sprite* sprite = owner->image_service.sprite(
        owner->image_service.self, base.states.images[base.states.current]
    );
    if (sprite == nullptr || sprite->data == nullptr || sprite->encoding != OA_SPRITE_RAW)
        return 0;
    Surface viewer{};
    present::surface_from_sprite(viewer, *sprite);
    return galaxy_count_drawn_planets(*owner->galaxy_graphics, owner->navigation->galaxy, viewer);
}

void galaxy_panel_sync_hotspots(MainFrame* frame) {
    GalaxyPanel* panel = frame->map_panel;
    if (panel == nullptr)
        return;
    const GalaxyView* galaxy = galaxy_of(frame);
    for (std::size_t i = 0; i < screens::kMaxGalaxyPlanets; ++i) {
        Window*& hotspot = panel->hotspots[i];
        const bool wanted = i < galaxy->planet_count && galaxy->planets[i].hotspot;
        if (!wanted) {
            if (hotspot != nullptr) {
                window_destroy(hotspot);
                hotspot = nullptr;
            }
            continue;
        }
        const GalaxyPlanet& p = galaxy->planets[i];
        const int32_t x = panel->base.x + p.view_left;
        const int32_t y = panel->base.y + p.view_top;
        if (hotspot == nullptr) {
            auto* h = reinterpret_cast<PlanetHotspot*>(
                console_window_alloc(sizeof(PlanetHotspot), hotspot_ops())
            );
            h->frame = frame;
            h->planet = static_cast<int32_t>(i);
            window_create(&h->base, "", x, y, p.view_width, p.view_height, &panel->base, true);
            window_set_state_count(&h->base, button_state::count);
            window_set_state(&h->base, button_state::unpressed);
            describe_hotspot(h, *galaxy);
            hotspot = &h->base;
        } else {
            hotspot->x = x;
            hotspot->y = y;
            window_set_width(hotspot, p.view_width);
            window_set_height(hotspot, p.view_height);
            describe_hotspot(as_hotspot(hotspot), *galaxy);
        }
    }
}

} // namespace oa::boneyards::console
