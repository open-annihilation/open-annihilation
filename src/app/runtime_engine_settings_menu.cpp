// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's two buttons, an overlay on the main menu under the
// extensions' overlays: the OA button, which opens the Library over the
// darkened main menu (runtime_library.cpp) and wears a dot while updates
// wait there, and beside it the Settings button, which opens Settings with
// no Library under it; and the settings dialog as a screen of the OA layer
// (oa_layer.hpp), which the Settings button, the shortcut, the requests and
// the Library's SETTINGS… open on whichever screen can show them. While
// MAINMENU.GUI has LIBRARY and SETTINGS buttons of its own
// (Runtime::add_main_menu_entries) the two buttons step aside for them.

#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "gui_record.hpp"
#include "library_host.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "render_state.hpp"

#include "oa/formats/gaf.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/paint/painter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace artless = oa::ui::frontend_renderer;
namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;

namespace {

/// The Settings button's gap to the picture's right edge, and the buttons'
/// gap to its bottom edge (top edge while an extension's overlay stands
/// over the main menu), in source pixels.
constexpr int32_t kButtonInset = 12;
/// The gap between the OA button and the Settings button: a quarter of a
/// button's side, in source pixels.
constexpr int32_t kButtonGap = settings::menu_button_side / 4;
/// The button overlay's z: under the extensions' overlays, which draw over
/// the button and see input before it.
constexpr int16_t kButtonOverlayZ = -100;
/// The front end's clock, in units a second (Runtime::match_clock_scale).
constexpr uint32_t kMenuClockHz = 30;
/// How long one place of the Library button's spinner stays lit, in those
/// units. Eight places are one turn of kit::refresh_revolution_ms, so the
/// rate follows the clock and not the frame rate.
constexpr uint32_t kSpinnerUnitsPerPlace = kit::refresh_revolution_ms * kMenuClockHz / 1000u /
                                           static_cast<uint32_t>(kit::oa_button_spinner_places);
static_assert(kSpinnerUnitsPerPlace == 6u);
/// The sound Settings plays as it opens on the main menu.
constexpr std::string_view kOpenSound = "Options";

/// Returns how a main-menu button looks: lit under the pointer, sunk while
/// a press on it is held there, and at rest under a modal screen.
///
/// @param hovered the pointer is over the button
/// @param pressed a press on the button is held
/// @param covered a modal screen of the OA layer shows
/// @return the look
settings::ButtonLook button_look(bool hovered, bool pressed, bool covered) noexcept {
    if (covered || !hovered)
        return settings::ButtonLook::idle;
    return pressed ? settings::ButtonLook::pressed : settings::ButtonLook::hovered;
}

/// Tells whether a point lies in a rectangle.
///
/// @param rect the rectangle, in source pixels
/// @param x the point's column
/// @param y the point's row
/// @return true inside it
bool contains(const artless::SourceRect& rect, int32_t x, int32_t y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

/// Returns the source pixel an input's pointer is over: on the main menu
/// the pointer is in the picture's pixels.
///
/// @param input the input
/// @param[out] x the pixel's column
/// @param[out] y the pixel's row
void pointer_pixel(const ScreenInput& input, int32_t& x, int32_t& y) {
    x = static_cast<int32_t>(std::floor(input.x));
    y = static_cast<int32_t>(std::floor(input.y));
}

/// One blend of the menu's backdrop, in 256ths, as the picture is darkened.
constexpr uint32_t kBlendOpaque = 256;

/// Copies a rectangle of a picture's red, green and blue.
///
/// @param surface the picture
/// @param rect the rectangle, in the picture's pixels
/// @param[out] out the copy, three bytes a pixel
/// @return false when the rectangle does not lie on the picture
bool copy_rgb(
    const artless::Surface& surface, const artless::SourceRect& rect, std::vector<uint8_t>& out
) {
    if (rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0)
        return false;
    const int64_t columns = surface.width;
    const int64_t rows = surface.height;
    if (int64_t{rect.x} + rect.width > columns || int64_t{rect.y} + rect.height > rows)
        return false;
    if (surface.rgb.size() < static_cast<std::size_t>(columns * rows) * 3U)
        return false;
    out.resize(static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height) * 3U);
    for (int32_t row = 0; row < rect.height; ++row) {
        const uint8_t* from =
            surface.rgb.data() + (static_cast<std::size_t>(rect.y + row) * surface.width +
                                  static_cast<std::size_t>(rect.x)) *
                                     3U;
        std::memcpy(
            out.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(rect.width) * 3U,
            from,
            static_cast<std::size_t>(rect.width) * 3U
        );
    }
    return true;
}

/// Darkens one pixel as many times as the menu's picture is darkened.
///
/// @param[in,out] rgb red, green and blue
/// @param passes how many times
void darken_channels(uint8_t* rgb, uint32_t passes) noexcept {
    const auto backdrop = kit::rgb(kit::colour::backdrop);
    const uint32_t share = std::min(kit::menu_backdrop_opacity, kBlendOpaque);
    const uint32_t keep = kBlendOpaque - share;
    constexpr uint32_t half = kBlendOpaque / 2;
    std::array<uint32_t, 3> painted{};
    for (std::size_t channel = 0; channel < painted.size(); ++channel)
        painted[channel] = uint32_t{backdrop[channel]} * share + half;
    for (uint32_t pass = 0; pass < passes; ++pass) {
        for (std::size_t channel = 0; channel < painted.size(); ++channel)
            rgb[channel] = static_cast<uint8_t>(
                (uint32_t{rgb[channel]} * keep + painted[channel]) / kBlendOpaque
            );
    }
}

/// Tells whether two badges are the same.
///
/// @param first a badge
/// @param second another
/// @return true when every field matches
bool same_mark(const kit::OaButtonMark& first, const kit::OaButtonMark& second) noexcept {
    return first.dot == second.dot && first.corner == second.corner &&
           first.count == second.count && first.turn == second.turn;
}

/// Folds the display gamma's table, so a texture built for another gamma is
/// built again.
///
/// @param table the table
/// @return the fold
uint32_t gamma_fold(const std::array<uint8_t, 256>& table) noexcept {
    uint32_t fold = 2166136261u;
    for (const uint8_t channel : table)
        fold = (fold ^ channel) * 16777619u;
    return fold;
}

/// Tells whether the frame still holds the button that was drawn, darkened
/// as the menu is.
///
/// @param frame the frame's red, green and blue
/// @param frame_width the frame's width
/// @param frame_height the frame's height
/// @param at where the button was drawn, in source pixels
/// @param drawn the button's pixels, before the darkening
/// @param passes how many times the frame was darkened
/// @return true when every pixel still matches
bool frame_holds_button(
    const std::vector<uint8_t>& frame,
    uint32_t frame_width,
    uint32_t frame_height,
    const artless::SourceRect& at,
    const std::vector<uint8_t>& drawn,
    uint32_t passes
) {
    if (at.width <= 0 || at.height <= 0 || at.x < 0 || at.y < 0)
        return false;
    if (static_cast<uint32_t>(at.x) + static_cast<uint32_t>(at.width) > frame_width ||
        static_cast<uint32_t>(at.y) + static_cast<uint32_t>(at.height) > frame_height)
        return false;
    const std::size_t count =
        static_cast<std::size_t>(at.width) * static_cast<std::size_t>(at.height);
    if (drawn.size() != count * 3U ||
        frame.size() < static_cast<std::size_t>(frame_width) * frame_height * 3U)
        return false;
    for (int32_t row = 0; row < at.height; ++row) {
        for (int32_t column = 0; column < at.width; ++column) {
            const std::size_t kept_at =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(at.width) +
                 static_cast<std::size_t>(column)) *
                3U;
            uint8_t expected[3] = {drawn[kept_at], drawn[kept_at + 1], drawn[kept_at + 2]};
            darken_channels(expected, passes);
            const uint8_t* pixel =
                frame.data() + (static_cast<std::size_t>(at.y + row) * frame_width +
                                static_cast<std::size_t>(at.x + column)) *
                                   3U;
            if (pixel[0] != expected[0] || pixel[1] != expected[1] || pixel[2] != expected[2])
                return false;
        }
    }
    return true;
}

/// Puts the menu back where a button was drawn.
///
/// @param[in,out] frame the frame's red, green and blue
/// @param frame_width the frame's width
/// @param at where the button was drawn, in source pixels
/// @param under the menu under the button, before the darkening
/// @param passes how many times the frame was darkened
void restore_under_button(
    std::vector<uint8_t>& frame,
    uint32_t frame_width,
    const artless::SourceRect& at,
    const std::vector<uint8_t>& under,
    uint32_t passes
) {
    for (int32_t row = 0; row < at.height; ++row) {
        for (int32_t column = 0; column < at.width; ++column) {
            const std::size_t kept_at =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(at.width) +
                 static_cast<std::size_t>(column)) *
                3U;
            uint8_t restored[3] = {under[kept_at], under[kept_at + 1], under[kept_at + 2]};
            darken_channels(restored, passes);
            uint8_t* pixel = frame.data() + (static_cast<std::size_t>(at.y + row) * frame_width +
                                             static_cast<std::size_t>(at.x + column)) *
                                                3U;
            pixel[0] = restored[0];
            pixel[1] = restored[1];
            pixel[2] = restored[2];
        }
    }
}

} // namespace

Runtime::EngineSettingsMenuHost::~EngineSettingsMenuHost() {
    release_sharp_textures();
}

const WindowIcon* Runtime::EngineSettingsMenuHost::icon_for(bool mods, int32_t side) {
    std::optional<WindowIcon>& large = mods ? mods_large : settings_large;
    bool& large_missing = mods ? mods_large_missing : settings_large_missing;
    if (side > static_cast<int32_t>(window_icon_size) && !large_missing) {
        if (!large) {
            WindowIcon decoded;
            std::string error;
            const std::span<const uint8_t> png =
                mods ? mods_icon_512_png() : settings_icon_512_png();
            if (!decode_rgba_icon(png, decoded, error) || decoded.width < menu_icon_large_size ||
                decoded.height < menu_icon_large_size || decoded.pixels.empty()) {
                large.reset();
                large_missing = true;
                std::cerr << "open-annihilation: the " << (mods ? "mods" : "settings")
                          << " icon at 512 pixels is missing"
                          << (error.empty() ? std::string{} : ": " + error) << '\n';
            } else {
                large = std::move(decoded);
            }
        }
        if (large)
            return &*large;
    }
    std::optional<WindowIcon>& square = mods ? mods_square : settings_square;
    bool& square_missing = mods ? mods_square_missing : settings_square_missing;
    if (!square && !square_missing) {
        WindowIcon decoded;
        std::string error;
        const std::span<const uint8_t> png = mods ? mods_icon_png() : settings_icon_png();
        if (!decode_window_icon(png, decoded, error) || decoded.pixels.empty()) {
            square.reset();
            square_missing = true;
            std::cerr << "open-annihilation: the " << (mods ? "mods" : "settings")
                      << " icon is missing" << (error.empty() ? std::string{} : ": " + error)
                      << '\n';
        } else {
            square = std::move(decoded);
        }
    }
    return square ? &*square : nullptr;
}

void Runtime::destroy_engine_settings_menu_host(EngineSettingsMenuHost* host) noexcept {
    delete host;
}

Runtime::EngineSettingsMenuHost& Runtime::engine_settings_menu_host() {
    if (!engine_settings_menu_)
        engine_settings_menu_.reset(new EngineSettingsMenuHost{});
    return *engine_settings_menu_;
}

bool Runtime::EngineSettingsMenuHost::button_shown(Runtime& runtime) {
    return runtime.screen_ == Screen::main_menu && !runtime.frame_owned_by_package() &&
           !runtime.main_menu_entries_ &&
           runtime.surface_.width >= static_cast<uint32_t>(kCanvasWidth) &&
           runtime.surface_.height >= static_cast<uint32_t>(kCanvasHeight) &&
           runtime.engine_settings_fonts() != nullptr;
}

std::optional<artless::SourceRect>
Runtime::EngineSettingsMenuHost::entry_rect(const Runtime& runtime, std::string_view name) {
    if (!runtime.main_menu_entries_ || runtime.screen_ != Screen::main_menu)
        return std::nullopt;
    const auto found = gui_record_named(runtime.resources_.layout.gadgets, name);
    if (!found)
        return std::nullopt;
    const auto& common = runtime.resources_.layout.gadgets[*found].common;
    return artless::SourceRect{common.x, common.y, common.width, common.height};
}

artless::SourceRect Runtime::EngineSettingsMenuHost::button_rect(const Runtime& runtime) {
    if (const auto entry = entry_rect(runtime, main_menu_library_record))
        return *entry;
    const artless::SourceRect beside = settings_button_rect(runtime);
    return {beside.x - kButtonGap - beside.width, beside.y, beside.width, beside.height};
}

artless::SourceRect Runtime::EngineSettingsMenuHost::settings_button_rect(const Runtime& runtime) {
    if (const auto entry = entry_rect(runtime, main_menu_settings_record))
        return *entry;
    constexpr int32_t side = settings::menu_button_side;
    const int32_t y =
        runtime.main_menu_overlay_ ? kButtonInset : kCanvasHeight - kButtonInset - side;
    return {kCanvasWidth - kButtonInset - side, y, side, side};
}

artless::RgbaPicture Runtime::EngineSettingsMenuHost::settings_icon(Runtime& runtime) {
    auto& host = runtime.engine_settings_menu_host();
    if (!host.settings_picture && !host.settings_picture_missing) {
        WindowIcon decoded;
        std::string error;
        if (decode_window_icon(settings_icon_png(), decoded, error))
            host.settings_picture = visible_part(decoded);
        if (!host.settings_picture || host.settings_picture->pixels.empty()) {
            host.settings_picture.reset();
            host.settings_picture_missing = true;
            std::cerr << "open-annihilation: the settings icon is missing"
                      << (error.empty() ? std::string{} : ": " + error) << '\n';
        }
    }
    if (!host.settings_picture)
        return {};
    return {
        host.settings_picture->width, host.settings_picture->height, host.settings_picture->pixels
    };
}

artless::RgbaPicture Runtime::EngineSettingsMenuHost::mods_icon(Runtime& runtime) {
    auto& host = runtime.engine_settings_menu_host();
    if (!host.mods_picture && !host.mods_picture_missing) {
        WindowIcon decoded;
        std::string error;
        if (decode_window_icon(mods_icon_png(), decoded, error))
            host.mods_picture = visible_part(decoded);
        if (!host.mods_picture || host.mods_picture->pixels.empty()) {
            host.mods_picture.reset();
            host.mods_picture_missing = true;
            std::cerr << "open-annihilation: the mods icon is missing"
                      << (error.empty() ? std::string{} : ": " + error) << '\n';
        }
    }
    if (!host.mods_picture)
        return {};
    return {host.mods_picture->width, host.mods_picture->height, host.mods_picture->pixels};
}

int Runtime::EngineSettingsMenuHost::button_event(ScreenContext* context, void*) {
    auto& runtime = *static_cast<Runtime*>(context->host);
    auto& host = runtime.engine_settings_menu_host();
    const auto& input = *context->input;
    // Under a modal screen of the OA layer the buttons see no input: the
    // layer takes it first.
    const bool covered = runtime.oa_layer().modal_shown(false);
    // The shortcut opens Settings on MAINMENU.GUI with or without these
    // buttons; its own SETTINGS, when it has one, is pressed as its others.
    if (!covered && input.kind == ScreenInputKind::key_down &&
        runtime.screen_ == Screen::main_menu &&
        engine_settings_shortcut(input.key, input.modifiers)) {
        runtime.open_engine_settings_from_menu();
        return 1;
    }
    if (covered || !button_shown(runtime)) {
        host.button_hovered = false;
        host.button_pressed = false;
        host.settings_hovered = false;
        host.settings_pressed = false;
        return 0;
    }
    int32_t x = 0;
    int32_t y = 0;
    pointer_pixel(input, x, y);
    const bool over_library = contains(button_rect(runtime), x, y);
    const bool over_settings = contains(settings_button_rect(runtime), x, y);
    switch (input.kind) {
    case ScreenInputKind::pointer_move:
        host.button_hovered = over_library;
        host.settings_hovered = over_settings;
        return 0;
    case ScreenInputKind::pointer_down:
        host.button_hovered = over_library;
        host.settings_hovered = over_settings;
        if (input.button != SDL_BUTTON_LEFT || (!over_library && !over_settings))
            return 0;
        host.button_pressed = over_library;
        host.settings_pressed = over_settings;
        return 1;
    case ScreenInputKind::pointer_up: {
        host.button_hovered = over_library;
        host.settings_hovered = over_settings;
        if (input.button != SDL_BUTTON_LEFT || (!host.button_pressed && !host.settings_pressed))
            return 0;
        // A press opens what its button opens when it is released over that
        // button: the OA button the Library, on its Active tab,
        // and the Settings button Settings, with no Library under it.
        // Released elsewhere it does nothing.
        const bool opens_library = host.button_pressed && over_library;
        const bool opens_settings = host.settings_pressed && over_settings;
        host.button_pressed = false;
        host.settings_pressed = false;
        if (opens_library)
            runtime.open_library(std::nullopt);
        else if (opens_settings)
            runtime.open_engine_settings_from_menu();
        return 1;
    }
    default:
        return 0;
    }
}

kit::OaButtonMark Runtime::EngineSettingsMenuHost::library_mark(Runtime& runtime) {
    LibraryHost& shown = runtime.library_host();
    if (!shown.refreshed)
        shown.rebuild(runtime);
    // Every package the player has added: mods, map packs, language packs
    // and downloaded expansions. The base game and the built-in languages
    // are left out, and a map pack counts once.
    const std::size_t packages = oa::ui::library::added_package_count(shown.model.inputs);
    kit::OaButtonMark mark;
    mark.dot = runtime.library_update_count() > 0;
    // A fetch that has not yet produced a list has no number to show. Once
    // a list is in, a later fetch keeps that number: the spinner stops.
    const bool arriving = shown.model.inputs.checking && shown.model.inputs.list_age_seconds < 0;
    if (!shown.refreshed || arriving) {
        mark.corner = kit::OaButtonCorner::spinner;
        mark.turn = runtime.frontend_tick() / kSpinnerUnitsPerPlace;
        return mark;
    }
    mark.corner = kit::OaButtonCorner::count;
    mark.count = packages;
    return mark;
}

void Runtime::EngineSettingsMenuHost::button_draw(ScreenContext* context, void*) {
    auto& runtime = *static_cast<Runtime*>(context->host);
    auto& host = runtime.engine_settings_menu_host();
    if (!button_shown(runtime) || context->surface == nullptr) {
        host.forget_button_pixels();
        return;
    }
    const auto rect = button_rect(runtime);
    const auto beside = settings_button_rect(runtime);
    host.keep_button_under(*context->surface, rect, beside);
    const kit::OaButtonMark library_badge = library_mark(runtime);
    const bool covered = runtime.oa_layer().modal_shown(false);
    const auto library_state = static_cast<kit::OaButtonState>(
        button_look(host.button_hovered, host.button_pressed, covered)
    );
    const auto settings_state = static_cast<kit::OaButtonState>(
        button_look(host.settings_hovered, host.settings_pressed, covered)
    );
    // The dot stays in the top right while updates can be installed. The
    // bottom right is the package count, or the spinner while it is not final.
    // The icon itself, from the shipped square, at this picture's size: no
    // panel and no bevel, clear corners left as the menu. A larger window
    // draws that icon again from a source at least that large
    // (replace_scaled_buttons). A window the size of this picture shows this
    // draw, so nothing is painted over it.
    const auto draw_icon = [&](const artless::SourceRect& at,
                               bool mods,
                               kit::OaButtonState state,
                               const kit::OaButtonMark& mark) {
        artless::RgbaPicture picture;
        if (const WindowIcon* icon = host.icon_for(mods, at.width))
            picture = {icon->width, icon->height, icon->pixels};
        kit::draw_menu_icon_button(
            {context->surface, {at.x, at.y, 1}, runtime.engine_settings_fonts(), picture},
            at.width,
            state,
            false,
            mark
        );
    };
    draw_icon(rect, true, library_state, library_badge);
    // The Settings button wears neither the dot nor the package count.
    draw_icon(beside, false, settings_state, {});
    host.keep_button_drawn(*context->surface, library_state, library_badge, settings_state);
}

void Runtime::EngineSettingsMenuHost::forget_button_pixels() noexcept {
    library_pixels.kept = false;
    settings_pixels.kept = false;
    library_pixels.under.clear();
    library_pixels.drawn.clear();
    settings_pixels.under.clear();
    settings_pixels.drawn.clear();
}

void Runtime::EngineSettingsMenuHost::keep_button_under(
    const artless::Surface& surface, artless::SourceRect library, artless::SourceRect settings
) {
    library_pixels.kept = false;
    settings_pixels.kept = false;
    library_pixels.at = library;
    settings_pixels.at = settings;
    if (!copy_rgb(surface, library, library_pixels.under))
        library_pixels.under.clear();
    if (!copy_rgb(surface, settings, settings_pixels.under))
        settings_pixels.under.clear();
}

void Runtime::EngineSettingsMenuHost::keep_button_drawn(
    const artless::Surface& surface,
    kit::OaButtonState library_state,
    const kit::OaButtonMark& library_badge,
    kit::OaButtonState settings_state
) {
    library_pixels.state = library_state;
    library_pixels.mark = library_badge;
    settings_pixels.state = settings_state;
    settings_pixels.mark = {};
    const bool library_copied = copy_rgb(surface, library_pixels.at, library_pixels.drawn);
    const bool settings_copied = copy_rgb(surface, settings_pixels.at, settings_pixels.drawn);
    library_pixels.kept =
        library_copied && library_pixels.under.size() == library_pixels.drawn.size();
    settings_pixels.kept =
        settings_copied && settings_pixels.under.size() == settings_pixels.drawn.size();
    if (!library_pixels.kept)
        library_pixels.drawn.clear();
    if (!settings_pixels.kept)
        settings_pixels.drawn.clear();
}

bool Runtime::EngineSettingsMenuHost::prepare_sharp_button(
    Runtime& runtime,
    std::vector<uint8_t>& frame,
    const ButtonPixels& kept,
    int32_t window_x,
    int32_t window_y,
    int32_t window_width,
    int32_t window_height,
    SharpButton& sharp,
    bool mods,
    uint32_t passes
) {
    sharp.show = false;
    // The picture already holds the icon at this size. A window whose
    // rectangle is that same square shows the picture, and the presented
    // frame matches it after gamma. A larger rectangle draws the icon again.
    if (window_width <= 0 || window_height <= 0 ||
        (window_width == kept.at.width && window_height == kept.at.height))
        return false;
    if (!kept.kept || kept.under.size() != kept.drawn.size())
        return false;
    if (!frame_holds_button(
            frame, runtime.surface_.width, runtime.surface_.height, kept.at, kept.drawn, passes
        ))
        return false;
    const int32_t longer = std::max(window_width, window_height);
    const WindowIcon* icon = icon_for(mods, longer);
    if (icon == nullptr)
        return false;
    const int32_t point_side = settings::menu_button_side;
    const int32_t percent = static_cast<int32_t>(
        std::lround(static_cast<double>(window_width) * 100.0 / static_cast<double>(point_side))
    );
    const kit::Rect mapped = kit::to_canvas({0, 0, point_side, point_side}, percent);
    if (mapped.width <= 0 || mapped.height <= 0)
        return false;
    const uint32_t fold = gamma_fold(runtime.gamma_table_);
    const bool identity = runtime.gamma_identity_;
    const bool reuse =
        sharp.texture != nullptr && sharp.width == mapped.width && sharp.height == mapped.height &&
        sharp.point_side == point_side && sharp.state == kept.state && !sharp.focused &&
        same_mark(sharp.mark, kept.mark) && sharp.icon == icon->pixels.data() &&
        sharp.icon_width == icon->width && sharp.icon_height == icon->height &&
        sharp.darkenings == passes && sharp.gamma_fold == fold && sharp.identity_gamma == identity;
    if (!reuse) {
        paint::Canvas pixels = paint::make_canvas(mapped.width, mapped.height);
        paint::Painter painter(pixels);
        kit::Canvas canvas;
        canvas.smooth.painter = &painter;
        canvas.smooth.fonts = runtime.oa_layer().smooth_fonts();
        canvas.smooth.scale_percent = percent;
        canvas.icon = {icon->width, icon->height, icon->pixels};
        // These buttons have no keyboard focus of their own. The ring follows
        // the pointer, as the picture's outline does.
        kit::draw_menu_icon_button(canvas, point_side, kept.state, false, kept.mark);
        for (std::size_t pixel = 0; pixel < pixels.rgba.size(); pixel += 4)
            darken_channels(pixels.rgba.data() + pixel, passes);
        if (!identity)
            runtime.apply_gamma_rgb(pixels.rgba.data(), pixels.rgba.size() / 4U, 4);
        if (sharp.texture != nullptr) {
            SDL_DestroyTexture(sharp.texture);
            sharp.texture = nullptr;
        }
        SDL_Texture* made = SDL_CreateTexture(
            runtime.sdl_.renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STATIC,
            mapped.width,
            mapped.height
        );
        if (made == nullptr)
            return false;
        if (!SDL_SetTextureBlendMode(made, SDL_BLENDMODE_BLEND) ||
            !SDL_SetTextureScaleMode(made, SDL_SCALEMODE_NEAREST) ||
            !SDL_UpdateTexture(made, nullptr, pixels.rgba.data(), mapped.width * 4)) {
            SDL_DestroyTexture(made);
            return false;
        }
        sharp.texture = made;
        sharp.width = mapped.width;
        sharp.height = mapped.height;
        sharp.point_side = point_side;
        sharp.state = kept.state;
        sharp.focused = false;
        sharp.mark = kept.mark;
        sharp.icon = icon->pixels.data();
        sharp.icon_width = icon->width;
        sharp.icon_height = icon->height;
        sharp.darkenings = passes;
        sharp.gamma_fold = fold;
        sharp.identity_gamma = identity;
    }
    restore_under_button(frame, runtime.surface_.width, kept.at, kept.under, passes);
    sharp.x = window_x;
    sharp.y = window_y;
    sharp.show = true;
    sharp.place_width = window_width;
    sharp.place_height = window_height;
    return true;
}

void Runtime::EngineSettingsMenuHost::copy_presented_menu_buttons(
    Runtime& runtime,
    const artless::Surface& presented,
    artless::Surface& reference,
    artless::SourceRect covered
) {
    if (runtime.sdl_.renderer == nullptr || !button_shown(runtime))
        return;
    if (presented.width == 0 || presented.height == 0 || presented.width != reference.width ||
        presented.height != reference.height || presented.rgb.size() != reference.rgb.size())
        return;
    const std::size_t pixels =
        static_cast<std::size_t>(presented.width) * static_cast<std::size_t>(presented.height);
    if (presented.rgb.size() < pixels * 3U)
        return;
    int output_width = 0;
    int output_height = 0;
    if (!SDL_GetRenderOutputSize(runtime.sdl_.renderer, &output_width, &output_height) ||
        output_width <= 0 || output_height <= 0)
        return;
    // A whole-window read is already in the window's pixels. A letterbox
    // read starts at the picture's origin in that window.
    int origin_x = 0;
    int origin_y = 0;
    if (presented.width != static_cast<uint32_t>(output_width) ||
        presented.height != static_cast<uint32_t>(output_height)) {
        SDL_FRect picture{};
        if (!SDL_GetRenderLogicalPresentationRect(runtime.sdl_.renderer, &picture))
            return;
        const int picture_width = static_cast<int>(picture.w);
        const int picture_height = static_cast<int>(picture.h);
        if (picture_width <= 0 || picture_height <= 0 ||
            presented.width != static_cast<uint32_t>(picture_width) ||
            presented.height != static_cast<uint32_t>(picture_height))
            return;
        origin_x = static_cast<int>(picture.x);
        origin_y = static_cast<int>(picture.y);
    }
    const auto copy_one = [&](const oa::ui::display_layout::Rect& window, int32_t source_side) {
        if (window.width <= 0 || window.height <= 0)
            return;
        if (window.width == source_side && window.height == source_side)
            return;
        for (int button_row = window.y; button_row < window.y + window.height; ++button_row) {
            for (int button_column = window.x; button_column < window.x + window.width;
                 ++button_column) {
                const int frame_column = button_column - origin_x;
                const int frame_row = button_row - origin_y;
                if (frame_column < 0 || frame_row < 0 ||
                    static_cast<uint32_t>(frame_column) >= reference.width ||
                    static_cast<uint32_t>(frame_row) >= reference.height)
                    continue;
                if (covered.width > 0 && covered.height > 0 && frame_column >= covered.x &&
                    frame_row >= covered.y && frame_column < covered.x + covered.width &&
                    frame_row < covered.y + covered.height)
                    continue;
                const std::size_t at_pixel =
                    (static_cast<std::size_t>(frame_row) * reference.width +
                     static_cast<std::size_t>(frame_column)) *
                    3U;
                reference.rgb[at_pixel] = presented.rgb[at_pixel];
                reference.rgb[at_pixel + 1U] = presented.rgb[at_pixel + 1U];
                reference.rgb[at_pixel + 2U] = presented.rgb[at_pixel + 2U];
            }
        }
    };
    const auto windows = runtime.oa_layer().menu_button_windows();
    copy_one(windows.library, button_rect(runtime).width);
    copy_one(windows.settings, settings_button_rect(runtime).width);
}

bool Runtime::EngineSettingsMenuHost::replace_scaled_buttons(
    Runtime& runtime, std::vector<uint8_t>& frame
) {
    library_sharp.show = false;
    settings_sharp.show = false;
    if (runtime.sdl_.renderer == nullptr || !button_shown(runtime) || !library_pixels.kept ||
        !settings_pixels.kept)
        return false;
    const auto windows = runtime.oa_layer().menu_button_windows();
    const bool library_scaled = windows.library.width != library_pixels.at.width ||
                                windows.library.height != library_pixels.at.height;
    const bool settings_scaled = windows.settings.width != settings_pixels.at.width ||
                                 windows.settings.height != settings_pixels.at.height;
    if (!library_scaled && !settings_scaled)
        return false;
    if (runtime.surface_.rgb.size() <
        static_cast<std::size_t>(runtime.surface_.width) * runtime.surface_.height * 3U)
        return false;
    frame = runtime.surface_.rgb;
    const uint32_t passes = runtime.oa_layer().front_darkenings();
    const bool library_ready = library_scaled && prepare_sharp_button(
                                                     runtime,
                                                     frame,
                                                     library_pixels,
                                                     windows.library.x,
                                                     windows.library.y,
                                                     windows.library.width,
                                                     windows.library.height,
                                                     library_sharp,
                                                     true,
                                                     passes
                                                 );
    const bool settings_ready = settings_scaled && prepare_sharp_button(
                                                       runtime,
                                                       frame,
                                                       settings_pixels,
                                                       windows.settings.x,
                                                       windows.settings.y,
                                                       windows.settings.width,
                                                       windows.settings.height,
                                                       settings_sharp,
                                                       false,
                                                       passes
                                                   );
    return library_ready || settings_ready;
}

void Runtime::EngineSettingsMenuHost::draw_sharp_buttons(Runtime& runtime) {
    const auto draw_one = [&runtime](const SharpButton& sharp) {
        if (!sharp.show || sharp.texture == nullptr || sharp.place_width <= 0 ||
            sharp.place_height <= 0)
            return;
        const SDL_FRect at{
            static_cast<float>(sharp.x),
            static_cast<float>(sharp.y),
            static_cast<float>(sharp.place_width),
            static_cast<float>(sharp.place_height)
        };
        SDL_RenderTexture(runtime.sdl_.renderer, sharp.texture, nullptr, &at);
    };
    draw_one(library_sharp);
    draw_one(settings_sharp);
}

void Runtime::EngineSettingsMenuHost::release_sharp_textures() noexcept {
    if (library_sharp.texture != nullptr)
        SDL_DestroyTexture(library_sharp.texture);
    if (settings_sharp.texture != nullptr)
        SDL_DestroyTexture(settings_sharp.texture);
    library_sharp.texture = nullptr;
    settings_sharp.texture = nullptr;
    library_sharp.show = false;
    settings_sharp.show = false;
    library_sharp.width = 0;
    library_sharp.height = 0;
    settings_sharp.width = 0;
    settings_sharp.height = 0;
}

bool Runtime::replace_menu_button_pixels(std::vector<uint8_t>& frame) {
    if (!engine_settings_menu_)
        return false;
    return engine_settings_menu_->replace_scaled_buttons(*this, frame);
}

void Runtime::present_menu_icon_buttons() {
    if (!engine_settings_menu_ || sdl_.renderer == nullptr)
        return;
    const RenderState kept(sdl_.renderer);
    kept.use_window_pixels();
    engine_settings_menu_->draw_sharp_buttons(*this);
}

void Runtime::release_menu_button_textures() noexcept {
    if (engine_settings_menu_)
        engine_settings_menu_->release_sharp_textures();
}

void Runtime::clear_menu_button_cursor_hold() noexcept {
    if (engine_settings_menu_)
        engine_settings_menu_->cursor_above = false;
}

bool Runtime::menu_button_cursor_held() const noexcept {
    return engine_settings_menu_ && engine_settings_menu_->cursor_above;
}

bool Runtime::hold_menu_button_cursor() {
    if (!engine_settings_menu_ || sdl_.renderer == nullptr || cursor_image_ == nullptr ||
        surface_.rgb.empty() || !EngineSettingsMenuHost::button_shown(*this))
        return false;
    float cursor_x = pointer_x_;
    float cursor_y = pointer_y_;
    if (const TouchCursor touch = touch_cursor(); touch.replaces_pointer) {
        if (!touch.visible)
            return false;
        cursor_x = touch.x;
        cursor_y = touch.y;
    } else if (!pointer_shows_cursor()) {
        return false;
    }
    const oa::formats::gaf::RenderResult rendered = oa::formats::gaf::render_normal(*cursor_image_);
    if (!rendered.ok() || !rendered.frame)
        return false;
    const oa::formats::gaf::RenderedFrame& sprite = *rendered.frame;
    const artless::SourceRect cursor{
        static_cast<int32_t>(cursor_x) - sprite.origin_x,
        static_cast<int32_t>(cursor_y) - sprite.origin_y,
        static_cast<int32_t>(sprite.width),
        static_cast<int32_t>(sprite.height),
    };
    const artless::SourceRect library_at = EngineSettingsMenuHost::button_rect(*this);
    const artless::SourceRect settings_at = EngineSettingsMenuHost::settings_button_rect(*this);
    const auto overlaps = [](const artless::SourceRect& first, const artless::SourceRect& second) {
        return first.width > 0 && first.height > 0 && second.width > 0 && second.height > 0 &&
               first.x < second.x + second.width && second.x < first.x + first.width &&
               first.y < second.y + second.height && second.y < first.y + first.height;
    };
    const bool over_library = overlaps(cursor, library_at);
    const bool over_settings = overlaps(cursor, settings_at);
    if (!over_library && !over_settings)
        return false;
    const OaLayer::MenuButtonWindows windows = oa_layer().menu_button_windows();
    const bool library_scaled = over_library && (windows.library.width != library_at.width ||
                                                 windows.library.height != library_at.height);
    const bool settings_scaled = over_settings && (windows.settings.width != settings_at.width ||
                                                   windows.settings.height != settings_at.height);
    if (!library_scaled && !settings_scaled)
        return false;
    engine_settings_menu_->cursor_above = true;
    return true;
}

void Runtime::register_engine_settings_button() {
    OverlayDesc button{};
    button.name = "engine_settings_button";
    button.screen = screen_id(Screen::main_menu);
    button.z = kButtonOverlayZ;
    button.event = EngineSettingsMenuHost::button_event;
    button.draw = EngineSettingsMenuHost::button_draw;
    // A refused overlay is recorded in the registry, and register_screens
    // reports it once every screen and overlay is in.
    overlay_register(&screens_, &button);
}

void Runtime::open_engine_settings_from_menu() {
    // It opens from the main menu. Nothing opens over a message box, over a
    // frame a package owns, or a second time.
    if (screen_ != Screen::main_menu || frame_owned_by_package() ||
        oa::ui::frontend_dialogs::dialog_count() != 0 || engine_settings_dialog() != nullptr ||
        saves_notice_shown() || engine_settings_fonts() == nullptr)
        return;
    // The dialog is drawn and fed from engine_settings_dialog(), as the OA
    // layer's settings screen, over every other screen of the layer. A
    // settings screen whose dialog closed outside its events goes first.
    if (oa_layer().find("settings") != nullptr) {
        oa_layer().close_above("settings");
        oa_layer().close_top();
    }
    open_engine_settings_dialog();
    // On the main menu, unless the run takes the game's untouched main menu,
    // the dialog shows in the Settings screen drawn with the game's art, over
    // the whole window, with the game's own options.
    if (!main_menu_opens_game_art_screens() || !push_settings_menu_screen())
        push_settings_screen(false);
    auto& host = engine_settings_menu_host();
    host.button_hovered = false;
    host.button_pressed = false;
    host.settings_hovered = false;
    host.settings_pressed = false;
    play_ui_sound(kOpenSound, 0);
}

bool Runtime::engine_settings_shortcut(uint32_t key, uint16_t modifiers) noexcept {
#ifdef SDL_PLATFORM_MACOS
    constexpr uint16_t kShortcutModifier = SDL_KMOD_GUI;
#else
    constexpr uint16_t kShortcutModifier = SDL_KMOD_CTRL;
#endif
    return key == SDLK_COMMA && (modifiers & kShortcutModifier) != 0;
}

void Runtime::request_engine_settings() {
    if (screen_ == Screen::main_menu)
        open_engine_settings_from_menu();
    else if (screen_ == Screen::match)
        open_engine_settings_in_match();
}

bool Runtime::take_engine_settings_request(const SDL_Event& event) {
    // Whether the pointer event the screens are about to see is a finger's.
    EngineSettingsState::note_pointer_source(*this, event);
    if (!is_engine_settings_menu_event(event))
        return false;
    request_engine_settings();
    return true;
}

} // namespace oa::app
