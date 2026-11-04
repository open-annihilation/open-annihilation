// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Recon console: the dossier of the planet selected on the war map. Its own
// image carries a scan box in which the planet's panorama scrolls by (the
// map static when there is none); eight labelled fields show the planet's
// map, environment, affiliation and population, and a list below them the
// Galactic War participation notice (when the planet's map is not
// installed) and the planet's description.

#include "oa/boneyards/utf8.hpp"

#include <cstring>

#include "console_state.hpp"
#include "map_tdf_reader.hpp"
#include "oa/boneyards/screens/rand15.hpp"
#include "oa/present/raster.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

namespace row = screens::planet_info_row;

constexpr const char* k_console_name = "ReconConsole";
constexpr int32_t k_image_state = 1;

// The scan box outline in the console's own image.
constexpr int32_t k_box_left = 0xc;
constexpr int32_t k_box_top = 0xf;
constexpr int32_t k_box_right = 0x1e4;
constexpr int32_t k_box_bottom = 0x8a;
constexpr uint8_t k_box_color = 0x5a;

struct FieldSpec {
    std::size_t row;
    const char* caption;
    int32_t x, y, width;
};

constexpr int32_t k_field_height = 0xa;

// In the order the build creates them.
constexpr FieldSpec k_fields[] = {
    {row::planet, "Planet:", 0xc, 0x92, 0xa6},
    {row::gw_map, "GW Map:", 0xc, 0xa3, 0xa6},
    {row::type, "Type:", 0xb6, 0x92, 0x62},
    {row::gravity, "Gravity:", 0xb6, 0xa3, 0x62},
    {row::wind_speed, "Wind Speed:", 0x11c, 0x92, 0x64},
    {row::tide_strength, "Tide Strength:", 0x11c, 0xa3, 0x64},
    {row::affiliation, "Affiliation:", 0x184, 0x92, 0x5c},
    {row::population, "Population:", 0x184, 0xa3, 0x5c},
};

constexpr int32_t k_list_dx = 0xc;
constexpr int32_t k_list_dy = 0xb4;
constexpr int32_t k_list_width = 0x1d9;
constexpr int32_t k_list_height = 0x7e;

// Text rows: 3 pixels in, the description wrapped 3 pixels short of the row.
constexpr int32_t k_row_indent = 3;
constexpr int32_t k_wrap_inset = 3;

ReconConsole* as_recon(Window* w) {
    return reinterpret_cast<ReconConsole*>(w);
}

MainFrame* frame_of(const ReconConsole* console) {
    return reinterpret_cast<MainFrame*>(console->base.parent);
}

ConsoleNavButton* recon_button_of(const ReconConsole* console) {
    return reinterpret_cast<ConsoleNavButton*>(console->recon_button);
}

screens::GalaxyPlanet* planet_of(ReconConsole* console) {
    screens::GalaxyView& galaxy = console->owner->navigation->galaxy;
    const int32_t index = console->state.planet;
    if (index < 0 || index >= static_cast<int32_t>(galaxy.planet_count))
        return nullptr;
    return &galaxy.planets[index];
}

int32_t static_frame_count(const ReconConsole* console) {
    const GalaxyGraphics* graphics = console->owner->galaxy_graphics.get();
    return graphics != nullptr ? graphics->map_static_frames : 0;
}

/// Builds the Recon console hidden when its bounds are set.
///
/// Adds its own image, a keyed blank with the scan box outlined in 0x5A,
/// the eight labelled fields, and the text list below them.
///
/// @param self the Recon console
void recon_build(Window* self) {
    ReconConsole* console = as_recon(self);
    const Handle image =
        image_create_blank(self->width, self->height, screens::kWidgetKey, screens::kWidgetKey);
    window_set_state_image(self, k_image_state, image);
    console->image = image;
    Surface surface{};
    if (image_surface(image, &surface)) {
        (void)present::draw_clipped_line(
            &surface, k_box_left, k_box_top, k_box_right, k_box_top, k_box_color
        );
        (void)present::draw_clipped_line(
            &surface, k_box_left, k_box_bottom, k_box_right, k_box_bottom, k_box_color
        );
        (void)present::draw_clipped_line(
            &surface, k_box_left, k_box_top, k_box_left, k_box_bottom, k_box_color
        );
        (void)present::draw_clipped_line(
            &surface, k_box_right, k_box_top, k_box_right, k_box_bottom, k_box_color
        );
    }
    const screens::HatFonts* fonts = &console->owner->system.hat_fonts;
    for (const FieldSpec& spec : k_fields) {
        console->fields[spec.row] = screens::lobby_field_create(
            self, spec.x, spec.y, spec.width, k_field_height, spec.caption, fonts
        );
    }
    console->text = screens::by_list_box_create(false, false);
    window_set_name(&console->text->list.base, "ReconList");
    window_set_bounds(
        &console->text->list.base,
        self->x + k_list_dx,
        self->y + k_list_dy,
        k_list_width,
        k_list_height,
        self,
        true
    );
    self->visible = 0;
}

void fit_fields(ReconConsole* console);

/// Shifts the wind and scrolls the panorama while shown, or steps the map static without one.
///
/// @param self the Recon console
void recon_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    ReconConsole* console = as_recon(self);
    screens::recon_step(
        &console->state, screens::rand15_source, nullptr, static_frame_count(console)
    );
}

/// Draws the Recon console while shown.
///
/// Shows a wind speed the pulse changed, paints the panorama at its scroll
/// position (or the map static's current frame) into the scan box of the
/// console's own image, then draws the image, the fields and the list.
///
/// @param self the Recon console
/// @return true when hidden; otherwise the background draw's result
bool recon_draw(Window* self) {
    if (!window_is_shown(self))
        return true;
    ReconConsole* console = as_recon(self);
    screens::ReconState& state = console->state;
    if (state.map_installed && state.wind_dirty) {
        screens::lobby_field_set_count(
            console->fields[row::wind_speed], static_cast<uint32_t>(state.wind)
        );
        state.wind_dirty = false;
        fit_fields(console);
    }
    GalaxyGraphics* graphics = console->owner->galaxy_graphics.get();
    Surface image{};
    if (graphics != nullptr && image_surface(console->image, &image)) {
        if (state.has_pan)
            galaxy_draw_panorama(
                *graphics,
                static_cast<std::size_t>(state.planet),
                &image,
                state.pan_scroll,
                state.pan_end,
                k_recon_scan_clip
            );
        else
            galaxy_draw_map_static(*graphics, state.static_frame, &image, k_recon_scan_clip);
    }
    return window_draw_background(self);
}

WindowOps make_recon_ops() {
    WindowOps ops = console_panel_ops();
    ops.on_bounds = recon_build;
    ops.pulse = recon_pulse;
    ops.draw = recon_draw;
    return ops;
}

const WindowOps k_recon_ops = make_recon_ops();

// A text row of the list; `font` replaces the row's small font unless it is
// k_no_handle.
void add_row(ReconConsole* console, const char* text, Handle font) {
    screens::ByTextRow* row = screens::by_text_row_create(
        console->text, text, &console->owner->system.hat_fonts, false, k_row_indent
    );
    if (font != k_no_handle)
        screens::by_text_row_set_font(row, font);
}

// The participation notice rows in the small green font, then a blank row.
void add_notice(ReconConsole* console, const screens::PlanetInfo& info) {
    for (std::size_t i = 0; i < info.notice_count; ++i) {
        const bool blank = i + 1 == info.notice_count;
        add_row(
            console,
            info.notice[i],
            blank ? k_no_handle : console->owner->system.hat_fonts.small_green
        );
    }
}

/// Adds a planet's description wrapped to the rows in the small font, one row per line.
///
/// @param[in,out] console the Recon console
/// @param description the planet's description text
void add_description(ReconConsole* console, const char* description) {
    char text[screens::kPlanetDescriptionCapacity]{};
    char wrapped[screens::kPlanetDescriptionCapacity * 2]{};
    oa::boneyards::utf8::copy(text, sizeof text, description, 0);
    const int32_t width = screens::by_list_row_width(&console->text->list.base) - k_wrap_inset;
    int32_t lines = console->owner->toolkit != nullptr ? toolkit_wrap_text(
                                                             console->owner->toolkit,
                                                             width,
                                                             text,
                                                             wrapped,
                                                             static_cast<int32_t>(sizeof wrapped),
                                                             console->owner->system.hat_fonts.small
                                                         )
                                                       : 0;
    char* line = wrapped;
    for (; lines > 0; --lines) {
        if (lines == 1) {
            add_row(console, line, k_no_handle);
            break;
        }
        char* end = std::strchr(line, '\n');
        if (end == nullptr) {
            add_row(console, line, k_no_handle);
            break;
        }
        *end = '\0';
        add_row(console, line, k_no_handle);
        line = end + 1;
    }
}

// Loads the planet's pan strip when its map is installed and gives its frame
// widths; 0 frames without one.
std::size_t load_panorama(
    ReconConsole* console,
    const screens::GalaxyPlanet& planet,
    bool map_installed,
    int32_t widths[screens::kMaxReconPanFrames]
) {
    GalaxyGraphics* graphics = console->owner->galaxy_graphics.get();
    const auto index = static_cast<std::size_t>(console->state.planet);
    if (!map_installed || graphics == nullptr ||
        !galaxy_load_panorama(*graphics, index, planet, console->owner->config.files))
        return 0;
    const PanoramaStrip& strip = graphics->pan_strips[index];
    const auto count = static_cast<std::size_t>(strip.frame_count);
    for (std::size_t i = 0; i < count; ++i)
        widths[i] = (i + 1 < count ? strip.offsets[i + 1] : strip.total) - strip.offsets[i];
    return count;
}

int32_t field_box(Toolkit* toolkit, const screens::LobbyField* field) {
    if (field == nullptr || field->value == nullptr || toolkit == nullptr) {
        return k_field_height;
    }
    const Window* value = field->value;
    const char* text = "";
    if (value->states.count > 1 && value->states.texts != nullptr && value->states.texts[1] >= 0) {
        text = text_get(value->states.texts[1]);
    }
    TextFit fit{};
    toolkit_text_fit(toolkit, value->font, text, &fit);
    const int32_t over = fit.above > fit.below ? fit.above : fit.below;
    int32_t height = fit.line_height + over * 2;
    if (height < k_field_height) {
        height = k_field_height;
    }
    return height;
}

void place_field(screens::LobbyField* field, int32_t y, int32_t height) {
    if (field == nullptr) {
        return;
    }
    Window* self = &field->base;
    const int32_t dy = y - self->y;
    self->y = y;
    self->height = height;
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child == nullptr) {
            continue;
        }
        child->y += dy;
        child->height = height;
    }
}

/// Gives each row the height its tallest value needs and keeps the list's
/// bottom edge. A value that fits the font's line leaves the 10-pixel row.
void fit_fields(ReconConsole* console) {
    if (console->text == nullptr) {
        return;
    }
    Toolkit* toolkit = console->owner != nullptr ? console->owner->toolkit : nullptr;
    int32_t upper = k_field_height;
    int32_t lower = k_field_height;
    for (const FieldSpec& spec : k_fields) {
        const int32_t height = field_box(toolkit, console->fields[spec.row]);
        if (spec.y == k_fields[0].y) {
            if (height > upper) {
                upper = height;
            }
        } else if (height > lower) {
            lower = height;
        }
    }
    constexpr int32_t upper_dy = k_fields[0].y;
    constexpr int32_t row_gap = k_fields[1].y - (k_fields[0].y + k_field_height);
    constexpr int32_t list_gap = k_list_dy - (k_fields[1].y + k_field_height);
    constexpr int32_t list_bottom = k_list_dy + k_list_height;
    const int32_t top = console->base.y;
    const int32_t upper_y = top + upper_dy;
    const int32_t lower_y = upper_y + upper + row_gap;
    for (const FieldSpec& spec : k_fields) {
        const bool on_upper = spec.y == upper_dy;
        place_field(
            console->fields[spec.row], on_upper ? upper_y : lower_y, on_upper ? upper : lower
        );
    }
    const int32_t list_y = lower_y + lower + list_gap;
    int32_t list_height = top + list_bottom - list_y;
    if (list_height < 0) {
        list_height = 0;
    }
    Window* list = &console->text->list.base;
    if (list->y == list_y && list->height == list_height) {
        return;
    }
    list->y = list_y;
    list->height = list_height;
    if (list_height > 0 && list->ops != nullptr && list->ops->refresh != nullptr) {
        list->ops->refresh(list);
    }
}

} // namespace

void recon_console_open(Window* self) {
    ReconConsole* console = as_recon(self);
    StatusWidget* owner = console->owner;
    MainFrame* frame = frame_of(console);
    ConsoleNavButton* button = recon_button_of(console);
    window_set_state_image(&button->base, button_state::unpressed, button->lit);
    activity_light_start(frame->activity_light);
    main_frame_request_census(frame);
    screens::BoneyardsScreens* nav = owner->navigation;

    console->state = screens::ReconState{};
    console->state.planet = nav->galaxy.selected;
    screens::GalaxyPlanet* planet = planet_of(console);
    by_list_box_clear(console->text);
    if (planet != nullptr) {
        screens::PlanetInfo info{};
        MapTdfFile* file = map_tdf_file_create(owner->config.files);
        const screens::MapTdfReader reader = map_tdf_reader(file);
        screens::planet_info_fill(&info, planet, &reader, screens::rand15_source, nullptr);
        map_tdf_file_destroy(file);
        for (std::size_t i = 0; i < screens::kPlanetInfoRowCount; ++i)
            screens::lobby_field_set_text(console->fields[i], info.values[i]);
        int32_t widths[screens::kMaxReconPanFrames]{};
        const std::size_t frames = load_panorama(console, *planet, info.map_installed, widths);
        screens::recon_begin(&console->state, nav->galaxy.selected, planet, &info, widths, frames);
        if (!info.map_installed)
            add_notice(console, info);
        add_description(console, info.description);
    }
    fit_fields(console);
    self->visible = 1;
}

void recon_console_close(Window* self) {
    ReconConsole* console = as_recon(self);
    ConsoleNavButton* button = recon_button_of(console);
    window_set_state_image(&button->base, button_state::unpressed, button->normal);
    by_list_box_clear(console->text);
    self->visible = 0;
}

void recon_console_on_census(Window* self, const CensusReport* report) {
    ReconConsole* console = as_recon(self);
    const screens::GalaxyPlanet* planet = planet_of(console);
    if (planet == nullptr || static_cast<uint32_t>(planet->id) != report->area_id)
        return;
    screens::lobby_field_set_count(
        console->fields[row::population], static_cast<uint32_t>(planet->population)
    );
    fit_fields(console);
}

Window* recon_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
) {
    auto* console =
        reinterpret_cast<ReconConsole*>(console_window_alloc(sizeof(ReconConsole), &k_recon_ops));
    console->owner = owner;
    console->recon_button = reinterpret_cast<MainFrame*>(frame)
                                ->console_buttons[static_cast<int32_t>(ConsoleButton::recon)];
    window_create(&console->base, k_console_name, x, y, width, height, frame, true);
    return &console->base;
}

} // namespace oa::boneyards::console
