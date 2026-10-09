// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// SDL output textures, viewport sizing and frame presentation.
#include "mod_install_watch.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/game_directory.hpp"
#include "graphics_report.hpp"
#include "pad_state.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "oa/app/input_hints.hpp"
#include "phone_hud.hpp"
#include "touch_layer.hpp"
#include "xrgb_conversion.hpp"
#include "oa/base/float_precision.hpp"
#include "oa/platform/machine.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace oa::app {
namespace {

[[nodiscard]] int64_t elapsed_since(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - since
    )
        .count();
}

} // namespace

void Runtime::ensure_texture(int width, int height) {
    if (frontend_texture_.ensure(
            sdl_.renderer,
            front_end_layer_format(),
            width,
            height,
            render_texture_limit(),
            SDL_BLENDMODE_NONE
        ) ||
        output_texture_w_ != width || output_texture_h_ != height) {
        output_texture_w_ = width;
        output_texture_h_ = height;
    }
}

void Runtime::apply_output_mode() {
    if (sdl_.renderer == nullptr)
        return;
    if (screen_ == Screen::match) {
        // On a window at native density a layout pixel is a window point,
        // which logical presentation stretches over the display's pixels,
        // so the processor lays out and draws what it does on any other
        // window of that size; elsewhere a layout pixel is a window pixel.
        const bool native_density = native_density_window();
        int width = kCanvasWidth, height = kCanvasHeight;
        // The window's size in points, from which the layout's density, its
        // safe area and (with touch controls) its device class come; none
        // without a window.
        int window_width = 0, window_height = 0;
        if (sdl_.window != nullptr) {
            if (native_density)
                SDL_GetWindowSize(sdl_.window, &width, &height);
            else
                SDL_GetWindowSizeInPixels(sdl_.window, &width, &height);
            SDL_GetWindowSize(sdl_.window, &window_width, &window_height);
        }
        // Full screen on the desktop's mode at a screen size of its own
        // draws the match at that size, laid out as a window of it, and
        // scales the frame to the screen.
        const SDL_Point scaled = scaled_frame_size();
        oa::ui::display_layout::Insets safe{};
        if (scaled.x > 0 && scaled.y > 0) {
            width = window_width = scaled.x;
            height = window_height = scaled.y;
        } else {
            safe = window_safe_insets();
        }
        scaled_frame_width_ = scaled.x;
        scaled_frame_height_ = scaled.y;
        const auto laid_out = match_layout_;
        match_layout_ = make_window_match_layout(width, height, window_width, window_height, safe);
        // The side column narrows to the game's tallest unit page; a phone
        // layout has none.
        match_layout_ =
            oa::ui::display_layout::fit_side_column(match_layout_, side_column_page_rows());
        // ui.resource-panel's clock line may need the chrome smaller.
        match_layout_ = make_room_for_clock_line(match_layout_);
        // On a phone the HUD's pieces are placed for this canvas.
        refresh_placed_hud_regions();
        // On a screen of another size the pointer's place is known again
        // only once SDL reports it.
        if (match_layout_.width != laid_out.width || match_layout_.height != laid_out.height)
            match_pointer_known_ = false;
        try {
            // A scaled frame is letterboxed, or held in whole steps, as Menu
            // scaling holds the menus' frame.
            if (scaled_frame_width_ > 0
                    ? !set_frame_presentation(
                          sdl_.renderer, menu_scaling(), match_layout_.width, match_layout_.height
                      )
                    : !SDL_SetRenderLogicalPresentation(
                          sdl_.renderer,
                          match_layout_.width,
                          match_layout_.height,
                          native_density ? SDL_LOGICAL_PRESENTATION_STRETCH
                                         : SDL_LOGICAL_PRESENTATION_DISABLED
                      ))
                throw_present_error("SDL logical presentation");
            // The standard tier draws the scaled frame's layers with the
            // frame's filter, found now, before any frame begins.
            scaled_frame_mode_ = scaled_frame_width_ > 0
                                     ? standard_frame_scale_mode(match_layout_.width)
                                     : SDL_SCALEMODE_NEAREST;
            // A match never draws the front end's texture: beyond the
            // renderer's limit it is not made at the window's size at all.
            const auto limit = render_texture_limit();
            if (limit != 0 && (static_cast<uint32_t>(match_layout_.width) > limit ||
                               static_cast<uint32_t>(match_layout_.height) > limit)) {
                frontend_texture_.reset();
                output_texture_w_ = 0;
                output_texture_h_ = 0;
            } else {
                ensure_texture(match_layout_.width, match_layout_.height);
            }
        } catch (const PresentError& error) {
            note_present_error(error.what(), false);
        }
    } else {
        // The load and save dialogs and the in-game briefing keep the size of the
        // frame they are drawn over, and the end screen the match's size while
        // it darkens the match's last frame.
        match_layout_ = {};
        match_pointer_known_ = false;
        scaled_frame_width_ = 0;
        scaled_frame_height_ = 0;
        scaled_frame_mode_ = SDL_SCALEMODE_NEAREST;
        int width = kCanvasWidth, height = kCanvasHeight;
        if (const auto* parent = panel_parent(); parent != nullptr) {
            width = static_cast<int>(parent->width);
            height = static_cast<int>(parent->height);
        } else if (const auto* battlefield = end_screen_battlefield_size()) {
            width = battlefield->width;
            height = battlefield->height;
        }
        try {
            if (!set_frame_presentation(sdl_.renderer, menu_scaling(), width, height))
                throw_present_error("SDL logical presentation");
            ensure_texture(width, height);
        } catch (const PresentError& error) {
            note_present_error(error.what(), false);
        }
    }
}

[[nodiscard]] float Runtime::match_zoom() const {
    return match_zoom_;
}

[[nodiscard]] int Runtime::visible_map_width() const {
    return std::max(
        1,
        static_cast<int>(std::lround(
            static_cast<double>(match_layout_.battlefield_width()) /
            static_cast<double>(match_zoom())
        ))
    );
}

[[nodiscard]] int Runtime::visible_map_height() const {
    return std::max(
        1,
        static_cast<int>(std::lround(
            static_cast<double>(match_layout_.battlefield_height()) /
            static_cast<double>(match_zoom())
        ))
    );
}

[[nodiscard]] oa::present::world_renderer::BattlefieldViewport
Runtime::live_viewport(int32_t camera_x, int32_t camera_y) const {
    return {
        camera_x,
        camera_y,
        match_layout_.left,
        match_layout_.top,
        static_cast<uint32_t>(match_layout_.battlefield_width()),
        static_cast<uint32_t>(match_layout_.battlefield_height()),
        static_cast<uint32_t>(match_layout_.width),
        static_cast<uint32_t>(match_layout_.height),
        match_zoom()
    };
}

WorldScaling Runtime::world_scaling() const {
    // The Full tier draws at the zoom with no split: the card draws the
    // terrain, and the processor the rest over it, in screen pixels at the
    // zoom, as the standard tier plans its frame. The far view is drawn at
    // the zoom with no split in every tier, by the processor.
    if (!scene_draw_scale_ && (full_presentation() || far_view_frame()) && screen_ == Screen::match)
        return oa::app::world_scaling(
            match_zoom(),
            match_layout_.battlefield_width(),
            match_layout_.battlefield_height(),
            std::nullopt
        );
    // The accelerated presentation draws at its own draw scale, within its
    // budget; a check's draw scale, and every other frame, as before. The
    // megamap covers the battlefield it opens over, so a frame under it
    // draws at the zoom and the megamap is presented 1:1, never through the
    // card's magnification of a scene it hides.
    if (!scene_draw_scale_ && accelerated_presentation() && screen_ == Screen::match &&
        !megamap_shown())
        return accelerated_world_scaling(
            match_zoom(),
            match_layout_.battlefield_width(),
            match_layout_.battlefield_height(),
            accelerated_.rung.budget,
            accelerated_.rung.magnify
        );
    return oa::app::world_scaling(
        match_zoom(),
        match_layout_.battlefield_width(),
        match_layout_.battlefield_height(),
        scene_draw_scale_
    );
}

void Runtime::initialize_sdl() {
    // Touch controls on from the start read fingers and the pen with their
    // own hints; a desktop without them keeps SDL's.
    if (touch_controls_active())
        oa::app::set_input_hints();
    if (sdl_.window == nullptr || sdl_.renderer == nullptr) {
        if (!SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1"))
            throw std::runtime_error("SDL mouse focus click-through hint was rejected");
        // Closing the window reaches the game as a close request, which a
        // running match answers with its surrender confirmation, rather than
        // as a quit SDL adds on its own.
        if (!SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0"))
            throw std::runtime_error("SDL last-window quit hint was rejected");
        if (!SDL_Init(SDL_INIT_VIDEO))
            throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
        // Sound starts on its own, as in main.cpp: without a sound driver the
        // game plays silently.
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
            std::cerr << "warning: sound is not available: " << SDL_GetError() << '\n';
        start_gamepad_subsystem();
        watch_opened_files();
        // A window the runtime makes itself has no renderer host, and opens
        // at the window system's density.
        sdl_.window = SDL_CreateWindow(
            "Open Annihilation",
            kDefaultWindowWidth,
            kDefaultWindowHeight,
            game_window_flags(options_.start_full_screen, false)
        );
        if (sdl_.window == nullptr)
            throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        sdl_.renderer = SDL_CreateRenderer(sdl_.window, nullptr);
        if (sdl_.renderer == nullptr)
            throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
        const auto facts = report_game_renderer(sdl_.renderer);
        take_renderer_names(facts.renderer, stats_adapter_name(facts));
        oa::base::float_precision::restore_program_float_control();
    }
    apply_output_mode();
    load_game_cursors();
    install_engine_settings_menu_item();
    // A renderer made here starts without Vertical sync, as every renderer does.
    apply_vertical_sync();
}

bool Runtime::take_full_screen_event(const SDL_Event& event) {
    return oa::app::take_full_screen_event(sdl_.window, full_screen_switch_, event);
}

void Runtime::take_full_screen_switch(const FullScreenSwitch& full_screen) noexcept {
    full_screen_switch_ = full_screen;
}

void Runtime::take_renderer_names(std::string driver, std::string adapter) {
    renderer_driver_ = std::move(driver);
    renderer_adapter_ = std::move(adapter);
}

void Runtime::ensure_streaming_texture(
    SDL_Texture*& texture,
    SDL_PixelFormat format,
    int width,
    int height,
    int& stored_w,
    int& stored_h
) {
    if (texture != nullptr && texture->format == format && stored_w == width && stored_h == height)
        return;
    // The old texture is forgotten before the new one is made, so that a
    // failure leaves none to draw, upload into or destroy again.
    if (texture != nullptr)
        SDL_DestroyTexture(texture);
    texture = nullptr;
    stored_w = 0;
    stored_h = 0;
    auto* made =
        SDL_CreateTexture(sdl_.renderer, format, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (made == nullptr)
        throw_present_error("SDL_CreateTexture");
    // An opaque layer in an alpha format is drawn with no blending, which
    // SDL turns on for alpha formats.
    if (SDL_ISPIXELFORMAT_ALPHA(format) && !SDL_SetTextureBlendMode(made, SDL_BLENDMODE_NONE)) {
        SDL_DestroyTexture(made);
        throw_present_error("SDL texture blend");
    }
    if (!SDL_SetTextureScaleMode(made, SDL_SCALEMODE_NEAREST)) {
        SDL_DestroyTexture(made);
        throw_present_error("SDL texture scale");
    }
    texture = made;
    stored_w = width;
    stored_h = height;
}

namespace {

/// Returns SDL's pixel format for a layer's format.
///
/// @param format the layer's format
/// @return SDL's format
SDL_PixelFormat sdl_format(render_policy::LayerFormat format) noexcept {
    switch (format) {
    case render_policy::LayerFormat::rgb24:
        return SDL_PIXELFORMAT_RGB24;
    case render_policy::LayerFormat::xrgb8888:
        return SDL_PIXELFORMAT_XRGB8888;
    case render_policy::LayerFormat::rgb565:
        return SDL_PIXELFORMAT_RGB565;
    case render_policy::LayerFormat::argb8888:
        return SDL_PIXELFORMAT_ARGB8888;
    }
    return SDL_PIXELFORMAT_XRGB8888;
}

} // namespace

SDL_PixelFormat Runtime::opaque_layer_format() const {
    if (render_run_)
        return sdl_format(render_run_->host->opaque_format());
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        return SDL_PIXELFORMAT_XRGB8888;
    const char* name = SDL_GetRendererName(sdl_.renderer);
    const bool software = name != nullptr && std::strcmp(name, SDL_SOFTWARE_RENDERER) == 0;
    return software && SDL_GetWindowPixelFormat(sdl_.window) == SDL_PIXELFORMAT_RGB565
               ? SDL_PIXELFORMAT_RGB565
               : SDL_PIXELFORMAT_XRGB8888;
}

SDL_PixelFormat Runtime::loading_layer_format() const {
    return render_run_ ? sdl_format(render_run_->host->layer_formats().loading)
                       : SDL_PIXELFORMAT_XRGB8888;
}

SDL_PixelFormat Runtime::front_end_layer_format() const {
    return render_run_ ? sdl_format(render_run_->host->layer_formats().front_end)
                       : SDL_PIXELFORMAT_RGB24;
}

uint32_t Runtime::render_texture_limit() const {
    return render_run_ ? render_run_->host->texture_limit() : 0;
}

void Runtime::upload_rgb24_frame(SDL_Texture* texture, const renderer::Surface& source) {
    if (texture == nullptr || source.rgb.empty())
        return;
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(texture, nullptr, &pixels, &pitch))
        throw_present_error("SDL_LockTexture");
    // XRGB8888 and ARGB8888 take the same opaque words.
    const auto convert =
        texture->format == SDL_PIXELFORMAT_RGB565 ? convert_rgb24_rgb565 : convert_rgb24_xrgb;
    convert(
        source.rgb.data(),
        source.width,
        source.height,
        static_cast<uint8_t*>(pixels),
        static_cast<std::size_t>(pitch),
        gamma_identity_ ? nullptr : &gamma_table_,
        draw_pool_.get()
    );
    SDL_UnlockTexture(texture);
}

void Runtime::upload_rgb24_tiles(
    TiledTexture& texture, const renderer::Surface& source, const std::array<uint8_t, 256>* gamma
) {
    if (source.rgb.empty() || static_cast<int>(source.width) != texture.width() ||
        static_cast<int>(source.height) != texture.height())
        return;
    const auto convert = texture.format() == SDL_PIXELFORMAT_RGB565 ? convert_rgb24_rgb565_rect
                                                                    : convert_rgb24_xrgb_rect;
    const auto rgb_pitch = static_cast<std::size_t>(source.width) * 3U;
    texture.upload([&](const SDL_Rect& part, uint8_t* pixels, int pitch) {
        convert(
            source.rgb.data() + static_cast<std::size_t>(part.y) * rgb_pitch +
                static_cast<std::size_t>(part.x) * 3U,
            rgb_pitch,
            static_cast<uint32_t>(part.w),
            static_cast<uint32_t>(part.h),
            pixels,
            static_cast<std::size_t>(pitch),
            gamma,
            draw_pool_.get()
        );
    });
}

void Runtime::destroy_match_layer_textures() {
    if (match_hud_tex_ != nullptr) {
        SDL_DestroyTexture(match_hud_tex_);
        match_hud_tex_ = nullptr;
    }
    match_world_tex_.reset();
    if (match_cursor_tex_ != nullptr) {
        SDL_DestroyTexture(match_cursor_tex_);
        match_cursor_tex_ = nullptr;
    }
    match_dialog_tex_.reset();
    if (match_dialog_side_tex_ != nullptr) {
        SDL_DestroyTexture(match_dialog_side_tex_);
        match_dialog_side_tex_ = nullptr;
    }
    destroy_engine_settings_textures();
    // The touch layer's texture goes with them; the next present makes it again.
    TouchDrawAccess::forget_textures(*this);
    match_dialog_side_tex_w_ = match_dialog_side_tex_h_ = 0;
    match_hud_tex_w_ = match_hud_tex_h_ = 0;
    match_cursor_tex_w_ = match_cursor_tex_h_ = 0;
}

void Runtime::release_renderer_textures() {
    destroy_match_layer_textures();
    if (indexed_output_.texture != nullptr)
        SDL_DestroyTexture(indexed_output_.texture);
    indexed_output_.texture = nullptr;
    indexed_output_.width = 0;
    indexed_output_.height = 0;
    frontend_texture_.reset();
}

std::array<Runtime::HudStrip, 4> Runtime::match_hud_strips() const {
    const int left = match_layout_.left;
    // The bars run from the column's edge to the window's right edge at
    // their scale, over as many source columns as that width holds, the
    // last cut by the window's edge: past 640 on the layer on a window
    // wider than the interface, where extend_match_bars continues their
    // art (no more than the layer holds).
    int bar_w = match_layout_.bar_width();
    int bar_columns = match_layout_.bar_columns();
    if (const auto held = static_cast<int>(match_hud_cpu_.width) - kBattlefieldLeft;
        bar_columns > held) {
        bar_columns = std::max(0, held);
        bar_w = static_cast<int>(std::lround(bar_columns * match_layout_.scale));
    }
    // The side column is drawn at its own scale, the chrome's unless the
    // game's tallest unit page narrows it (display_layout::fit_side_column):
    // its 128 columns then fill its width exactly.
    const double column_scale =
        match_layout_.column_narrowed() ? match_layout_.column_scale : match_layout_.scale;
    const auto canvas = [column_scale](int source) {
        return static_cast<int>(std::lround(source * column_scale));
    };
    // A unit's page past 480 rows grows the HUD; the side column then shows
    // those rows too, at the column's scale.
    int column_rows = kCanvasHeight;
    int column_height =
        match_layout_.column_narrowed() ? canvas(kCanvasHeight) : match_layout_.hud_height;
    const auto rows = static_cast<int>(match_hud_cpu_.height);
    if (rows > kCanvasHeight) {
        column_rows = rows;
        column_height = std::min(match_layout_.height, canvas(rows));
    }
    // A page taller than the column, which only a page the game's tallest
    // did not count can be: the column shows the rows above its panel, and
    // the page, whole, is scaled down under them to the column's last row.
    HudStrip page{};
    if (const auto scale = match_side_page_scale();
        scale.scaled() && scale.top + scale.authored_rows <= rows) {
        const int top = canvas(scale.top);
        const int bottom = canvas(scale.top + scale.shown_rows);
        const int page_left = canvas(scale.left);
        const int page_columns = kBattlefieldLeft - scale.left;
        column_rows = scale.top;
        column_height = top;
        page = {
            scale.left,
            scale.top,
            page_columns,
            scale.authored_rows,
            page_left,
            top,
            canvas(scale.left + scale.to_column(page_columns)) - page_left,
            std::min(match_layout_.height, bottom) - top
        };
    }
    return {{
        {0, 0, kBattlefieldLeft, column_rows, 0, 0, left, column_height},
        {kBattlefieldLeft, 0, bar_columns, kBattlefieldTop, left, 0, bar_w, match_layout_.top},
        {kBattlefieldLeft,
         kCanvasHeight - kBattlefieldBottom,
         bar_columns,
         kBattlefieldBottom,
         left,
         match_layout_.bottom_bar_y(),
         bar_w,
         match_layout_.bottom},
        page,
    }};
}

void Runtime::compose_match_layers(renderer::Surface& frame) {
    frame.width = static_cast<uint32_t>(match_layout_.width);
    frame.height = static_cast<uint32_t>(match_layout_.height);
    frame.rgb.assign(static_cast<std::size_t>(frame.width) * frame.height * 3U, 0);
    if (match_hud_cpu_.rgb.empty() || match_world_cpu_.rgb.empty())
        return;
    PhoneHudAccess::prepare_frame(*this);
    // In placed mode the HUD's pieces go over the full-bleed battlefield.
    const bool placed = oa::ui::display_layout::placed_mode(match_layout_);
    if (!placed)
        for (const auto& strip : match_hud_strips())
            if (strip.w > 0 && strip.h > 0 && strip.source_w > 0 && strip.source_h > 0)
                scale_blit(
                    frame,
                    match_hud_cpu_,
                    strip.x,
                    strip.y,
                    strip.w,
                    strip.h,
                    strip.source_x,
                    strip.source_y,
                    strip.source_w,
                    strip.source_h
                );
    blit_rect(
        frame,
        match_world_cpu_,
        match_layout_.left,
        match_layout_.top,
        0,
        0,
        static_cast<int>(match_world_cpu_.width),
        static_cast<int>(match_world_cpu_.height)
    );
    if (placed)
        PhoneHudAccess::blit_regions(*this, frame);
    if (!match_dialog_side_.rgb.empty() && placed_panel_area())
        blit_rect(
            frame,
            match_dialog_side_,
            match_dialog_side_at_.x,
            match_dialog_side_at_.y,
            0,
            0,
            static_cast<int>(match_dialog_side_.width),
            static_cast<int>(match_dialog_side_.height)
        );
}

void Runtime::compose_match_frame(renderer::Surface& frame) {
    compose_match_layers(frame);
    if (match_hud_cpu_.rgb.empty() || match_world_cpu_.rgb.empty())
        return;
    const auto pixels = static_cast<std::size_t>(frame.width) * frame.height;
    apply_gamma_rgb(frame.rgb.data(), pixels, 3);
    // The touch controls go over the world and the HUD, under the settings
    // and the dialogs; the placed regions over the touch controls' sheets.
    compose_touch_layer(frame);
    compose_placed_hud_regions(frame);
    compose_engine_settings_layer(frame);
    if (!match_use_layers_ || oa::ui::frontend_dialogs::dialog_count() == 0 ||
        match_dialog_rgba_.size() != pixels * 4U)
        return;
    for (std::size_t i = 0; i < pixels; ++i) {
        const auto* source = match_dialog_rgba_.data() + i * 4U;
        const unsigned alpha = source[3];
        auto* target = frame.rgb.data() + i * 3U;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const unsigned shown =
                gamma_identity_ ? source[channel] : gamma_table_[source[channel]];
            target[channel] =
                static_cast<uint8_t>((shown * alpha + target[channel] * (255U - alpha)) / 255U);
        }
    }
}

bool Runtime::compose_match_dialog_layer() {
    if (oa::ui::frontend_dialogs::dialog_count() == 0)
        return false;
    if (match_hud_ && !match_hud_->layout.gadgets.empty()) {
        const auto& root = match_hud_->layout.gadgets.front().common;
        oa::ui::frontend_dialogs::dialog_shade_below(
            match_hud_cpu_, root.x, root.y, root.width, root.height, match_palette_
        );
    }
    oa::ui::frontend_dialogs::dialog_draw_layer(
        static_cast<uint32_t>(match_layout_.width),
        static_cast<uint32_t>(match_layout_.height),
        match_dialog_rgba_
    );
    match_dialog_tex_.ensure(
        sdl_.renderer,
        SDL_PIXELFORMAT_RGBA32,
        match_layout_.width,
        match_layout_.height,
        render_texture_limit(),
        SDL_BLENDMODE_BLEND
    );
    const uint8_t* dialog_pixels = match_dialog_rgba_.data();
    std::vector<uint8_t> corrected;
    if (!gamma_identity_) {
        corrected = match_dialog_rgba_;
        apply_gamma_rgb(corrected.data(), corrected.size() / 4U, 4);
        dialog_pixels = corrected.data();
    }
    match_dialog_tex_.update(dialog_pixels, match_layout_.width * 4, 4);
    return true;
}

void Runtime::present_match_layers() {
    if (match_hud_cpu_.rgb.empty() || match_world_cpu_.rgb.empty())
        return;
    PhoneHudAccess::prepare_frame(*this);
    const bool dialogs = compose_match_dialog_layer();
    if (accelerated_presentation()) {
        try {
            // The Full tier draws the battlefield on the card; a frame it
            // drops, or cannot draw, Basic presents, the world drawn again
            // as the standard tier draws it, since the frame's world layer
            // is the overlay canvas.
            if (full_presentation() && present_full_match_layers(dialogs))
                return;
            if (full_frame_drawn())
                ensure_screen_world();
            present_accelerated_match_layers(dialogs);
            return;
        } catch (const AccelerationError& error) {
            take_acceleration_error(error);
        }
    }
    const auto frame_format = opaque_layer_format();
    ensure_streaming_texture(
        match_hud_tex_,
        frame_format,
        static_cast<int>(match_hud_cpu_.width),
        static_cast<int>(match_hud_cpu_.height),
        match_hud_tex_w_,
        match_hud_tex_h_
    );
    match_world_tex_.ensure(
        sdl_.renderer,
        frame_format,
        static_cast<int>(match_world_cpu_.width),
        static_cast<int>(match_world_cpu_.height),
        render_texture_limit(),
        SDL_BLENDMODE_NONE
    );
    const auto upload_start = std::chrono::steady_clock::now();
    upload_rgb24_frame(match_hud_tex_, match_hud_cpu_);
    upload_rgb24_tiles(
        match_world_tex_, match_world_cpu_, gamma_identity_ ? nullptr : &gamma_table_
    );
    const auto present_start = std::chrono::steady_clock::now();
    phase_times_.upload += elapsed_since(upload_start);
    // The clear is the blank fill for strip area beyond the chrome's largest
    // (1280x1024) size: under the side column.
    if (!SDL_SetRenderDrawColor(sdl_.renderer, 0, 0, 0, 255) || !SDL_RenderClear(sdl_.renderer))
        throw_present_error("SDL_RenderClear");
    // A scaled frame's layers take the frame's filter; a mode the texture
    // refuses leaves it NEAREST.
    if (!SDL_SetTextureScaleMode(match_hud_tex_, scaled_frame_mode_))
        std::ignore = SDL_SetTextureScaleMode(match_hud_tex_, SDL_SCALEMODE_NEAREST);
    if (!match_world_tex_.set_scale_mode(scaled_frame_mode_))
        std::ignore = match_world_tex_.set_scale_mode(SDL_SCALEMODE_NEAREST);
    // In placed mode the HUD's pieces are drawn after the world
    // (finish_match_layers).
    if (!oa::ui::display_layout::placed_mode(match_layout_))
        for (const auto& strip : match_hud_strips()) {
            if (strip.w <= 0 || strip.h <= 0 || strip.source_w <= 0 || strip.source_h <= 0)
                continue;
            const SDL_FRect source{
                static_cast<float>(strip.source_x),
                static_cast<float>(strip.source_y),
                static_cast<float>(strip.source_w),
                static_cast<float>(strip.source_h)
            };
            const SDL_FRect destination{
                static_cast<float>(strip.x),
                static_cast<float>(strip.y),
                static_cast<float>(strip.w),
                static_cast<float>(strip.h)
            };
            if (!SDL_RenderTexture(sdl_.renderer, match_hud_tex_, &source, &destination))
                throw_present_error("SDL_RenderTexture");
        }
    const SDL_FRect world{
        static_cast<float>(match_layout_.left),
        static_cast<float>(match_layout_.top),
        static_cast<float>(match_world_cpu_.width),
        static_cast<float>(match_world_cpu_.height)
    };
    match_world_tex_.draw(sdl_.renderer, nullptr, &world);
    finish_match_layers(frame_format, dialogs, upload_start, present_start);
    // Once the frame is presented the textures go back to NEAREST, as the
    // accelerated tier draws them.
    if (scaled_frame_mode_ != SDL_SCALEMODE_NEAREST) {
        std::ignore = SDL_SetTextureScaleMode(match_hud_tex_, SDL_SCALEMODE_NEAREST);
        std::ignore = match_world_tex_.set_scale_mode(SDL_SCALEMODE_NEAREST);
    }
}

void Runtime::finish_match_layers(
    SDL_PixelFormat frame_format,
    bool dialogs,
    std::chrono::steady_clock::time_point upload_start,
    std::chrono::steady_clock::time_point present_start
) {
    // The layers laid out 1:1 reach the display's pixels on a window at
    // native density: NEAREST, but in the accelerated tier at a density
    // that is not a whole number.
    const SDL_ScaleMode one_to_one = one_to_one_scale_mode();
    // The touch controls go over the world and the HUD, under the dialogs;
    // the placed regions over the touch controls' sheets.
    present_touch_layer();
    present_placed_hud_regions();
    // A placed dialog's part over the side column goes over the HUD layer.
    if (!match_dialog_side_.rgb.empty() && placed_panel_area()) {
        ensure_streaming_texture(
            match_dialog_side_tex_,
            frame_format,
            static_cast<int>(match_dialog_side_.width),
            static_cast<int>(match_dialog_side_.height),
            match_dialog_side_tex_w_,
            match_dialog_side_tex_h_
        );
        upload_rgb24_frame(match_dialog_side_tex_, match_dialog_side_);
        const SDL_FRect side{
            static_cast<float>(match_dialog_side_at_.x),
            static_cast<float>(match_dialog_side_at_.y),
            static_cast<float>(match_dialog_side_.width),
            static_cast<float>(match_dialog_side_.height)
        };
        draw_one_to_one(sdl_.renderer, match_dialog_side_tex_, &side, one_to_one);
    }
    present_engine_settings_layer();
    if (dialogs)
        draw_one_to_one(sdl_.renderer, match_dialog_tex_, nullptr, nullptr, one_to_one);
    present_software_cursor(true);
    capture_render_target();
    if (render_fault_due(RenderFaultPoint::present))
        throw PresentError("injected present error");
    if (render_fault_due(RenderFaultPoint::float_state))
        std::fesetround(FE_TOWARDZERO);
    if (!SDL_RenderPresent(sdl_.renderer))
        note_present_refused("SDL_RenderPresent");
    oa::base::float_precision::restore_program_float_control();
    phase_times_.present += elapsed_since(present_start);
    const auto present_ns = static_cast<uint64_t>(elapsed_since(upload_start));
    frame_pacing::note_frame_measure(frame_stats_, frame_pacing::FrameMeasure::present, present_ns);
    note_present_time(present_ns);
}

void Runtime::capture_render_target() {
    // A lost device has no picture to read.
    if (render_run_ && render_run_->device_lost)
        return;
    if (video_capture_)
        video_capture_->add_frame(sdl_.renderer);
    if (capture_frame_ == nullptr)
        return;
    SDL_Surface* target = SDL_RenderReadPixels(sdl_.renderer, nullptr);
    SDL_Surface* rgb =
        target != nullptr ? SDL_ConvertSurface(target, SDL_PIXELFORMAT_RGB24) : nullptr;
    SDL_DestroySurface(target);
    if (rgb == nullptr)
        throw_present_error("SDL_RenderReadPixels");
    const auto width = static_cast<std::size_t>(rgb->w);
    capture_frame_->width = static_cast<uint32_t>(rgb->w);
    capture_frame_->height = static_cast<uint32_t>(rgb->h);
    capture_frame_->rgb.resize(width * static_cast<std::size_t>(rgb->h) * 3U);
    for (int row = 0; row < rgb->h; ++row)
        std::memcpy(
            capture_frame_->rgb.data() + static_cast<std::size_t>(row) * width * 3U,
            static_cast<const uint8_t*>(rgb->pixels) +
                static_cast<std::ptrdiff_t>(row) * rgb->pitch,
            width * 3U
        );
    SDL_DestroySurface(rgb);
}

void Runtime::present_software_cursor(bool match_layers) {
    if (!cursors_loaded_ || cursor_image_ == nullptr || frame_without_cursor_)
        return;
    // With touch controls the cursor is hidden while no finger rests, or
    // lifted above the finger on the battlefield.
    float cursor_x = pointer_x_;
    float cursor_y = pointer_y_;
    if (const auto touch = touch_cursor(); touch.replaces_pointer) {
        if (!touch.visible)
            return;
        cursor_x = touch.x;
        cursor_y = touch.y;
    } else if (!pointer_shows_cursor())
        return;
    const auto rendered = oa::formats::gaf::render_normal(*cursor_image_);
    if (!rendered.ok())
        return;
    const auto& frame = *rendered.frame;
    if (match_cursor_tex_ == nullptr || match_cursor_tex_w_ != static_cast<int>(frame.width) ||
        match_cursor_tex_h_ != static_cast<int>(frame.height)) {
        if (match_cursor_tex_ != nullptr)
            SDL_DestroyTexture(match_cursor_tex_);
        match_cursor_tex_ = SDL_CreateTexture(
            sdl_.renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(frame.width),
            static_cast<int>(frame.height)
        );
        if (match_cursor_tex_ == nullptr)
            return;
        // Unblended, the cursor would cover the battlefield with its clear
        // pixels; a texture that cannot blend is dropped as one that cannot
        // be made is, and the next frame tries again.
        if (!SDL_SetTextureBlendMode(match_cursor_tex_, SDL_BLENDMODE_BLEND)) {
            SDL_DestroyTexture(match_cursor_tex_);
            match_cursor_tex_ = nullptr;
            return;
        }
        match_cursor_tex_w_ = static_cast<int>(frame.width);
        match_cursor_tex_h_ = static_cast<int>(frame.height);
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(match_cursor_tex_, nullptr, &pixels, &pitch))
        return;
    const auto& pal = match_palette_.size() >= 1024 ? match_palette_ : resources_.gui_palette;
    for (uint32_t row = 0; row < frame.height; ++row) {
        auto* dst = reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(pixels) + static_cast<std::size_t>(row) * pitch
        );
        for (uint32_t column = 0; column < frame.width; ++column) {
            const auto offset = static_cast<std::size_t>(row) * frame.width + column;
            if (offset >= frame.coverage.size() || frame.coverage[offset] == 0) {
                dst[column] = 0;
                continue;
            }
            const auto pal_i = static_cast<std::size_t>(frame.pixels[offset]) * 4U;
            if (pal_i + 2 >= pal.size()) {
                dst[column] = 0;
                continue;
            }
            dst[column] = 0xff000000u | (static_cast<uint32_t>(pal[pal_i]) << 16) |
                          (static_cast<uint32_t>(pal[pal_i + 1]) << 8) |
                          static_cast<uint32_t>(pal[pal_i + 2]);
        }
        if (!gamma_identity_)
            gamma_xrgb_row(dst, static_cast<int>(frame.width), gamma_table_);
    }
    SDL_UnlockTexture(match_cursor_tex_);
    const SDL_FRect dest{
        cursor_x - static_cast<float>(frame.origin_x),
        cursor_y - static_cast<float>(frame.origin_y),
        static_cast<float>(frame.width),
        static_cast<float>(frame.height)
    };
    // On a window at native density, and over a scaled frame, the cursor
    // over the match goes to the display's pixels with the match's layers;
    // a mode the texture refuses leaves it as it was.
    if (native_density_window() || scaled_frame_width_ > 0)
        std::ignore = SDL_SetTextureScaleMode(
            match_cursor_tex_, match_layers ? one_to_one_scale_mode() : SDL_SCALEMODE_LINEAR
        );
    // A cursor the renderer refuses is missing from this frame alone: the
    // next frame draws it again.
    std::ignore = SDL_RenderTexture(sdl_.renderer, match_cursor_tex_, nullptr, &dest);
}

bool Runtime::native_density_window() const noexcept {
    return sdl_.window != nullptr && at_native_density(SDL_GetWindowFlags(sdl_.window));
}

double Runtime::match_display_density() const {
    if (sdl_.renderer == nullptr || match_layout_.width <= 0)
        return 1.0;
    // A scaled frame covers the width it is presented at.
    if (scaled_frame_width_ > 0) {
        SDL_FRect area{};
        if (!SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &area) || !(area.w > 0.0F))
            return 1.0;
        return static_cast<double>(area.w) / static_cast<double>(match_layout_.width);
    }
    if (!native_density_window())
        return 1.0;
    int output_width = 0;
    int output_height = 0;
    if (!SDL_GetRenderOutputSize(sdl_.renderer, &output_width, &output_height) || output_width <= 0)
        return 1.0;
    return static_cast<double>(output_width) / static_cast<double>(match_layout_.width);
}

SDL_ScaleMode Runtime::one_to_one_scale_mode() const {
    if (scaled_frame_width_ > 0 && !accelerated_presentation())
        return scaled_frame_mode_;
    if (!accelerated_presentation() || (!native_density_window() && scaled_frame_width_ <= 0))
        return SDL_SCALEMODE_NEAREST;
    return direct_scale_mode(
        render_policy::chrome_filter(accelerated_.rung, match_display_density())
    );
}

SDL_ScaleMode Runtime::standard_frame_scale_mode(int width) {
    SDL_FRect area{};
    if (sdl_.renderer == nullptr || width <= 0 ||
        !SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &area) || !(area.w > 0.0F))
        return SDL_SCALEMODE_NEAREST;
    const double scale = static_cast<double>(area.w) / static_cast<double>(width);
    const auto scaling = menu_scaling();
    // The pixel-art mode is looked for only where the frame would take it.
    const bool pixelart =
        render_policy::frame_wants_pixelart(scaling, scale) && frame_pixelart_works();
    return direct_scale_mode(render_policy::frame_filter(scaling, nullptr, pixelart, scale));
}

bool Runtime::frame_pixelart_works() {
    if (frame_pixelart_)
        return *frame_pixelart_;
    bool works = false;
    if (render_run_ && sdl_.renderer != nullptr && !render_run_->device_lost) {
        const RendererHost& host = *render_run_->host;
        const render_policy::TierInputs& tier = host.tier_inputs();
        const char* name = SDL_GetRendererName(sdl_.renderer);
        const bool software =
            name != nullptr && name == oa::platform::render_probe::software_renderer;
        // The probe draws into a render target of its own, so it runs only
        // where the graphics card could be used for more.
        const bool probe = tier.function_test == render_policy::FunctionTest::not_run &&
                           !software && tier.memory >= render_policy::smallest_accelerated_memory &&
                           !oa::platform::running_on_windows_before_vista() &&
                           tier.capability == render_policy::Capability::capable &&
                           !tier.accelerated_unusable_record &&
                           tier.drop == render_policy::Drop::none && !tier.device_lost;
        if (tier.function_test == render_policy::FunctionTest::passed)
            works = host.start_rung().card == render_policy::CardFilter::pixelart;
        else if (probe)
            works = probe_pixelart(sdl_.renderer, nullptr);
    }
    frame_pixelart_ = works;
    return works;
}

void Runtime::render() {
    if (render_run_ && !render_run_->pending_rebuild.empty() && !render_run_->device_lost)
        rebuild_renderer(render_run_->pending_rebuild);
    // A reset device took the front end's texture with it.
    if (sdl_.renderer != nullptr && frontend_texture_.tile_count() == 0 &&
        screen_ != Screen::match && screen_ != Screen::loading)
        apply_output_mode();
    // The frame's tier decides how its battlefield is drawn, so it comes
    // before the frame is composed; the memory guard may drop it first.
    update_render_tier();
    watch_accelerated_memory();
    const auto compose_start = std::chrono::steady_clock::now();
    rebuild_surface();
    const auto composed = elapsed_since(compose_start);
    phase_times_.compose += composed;
    frame_pacing::note_frame_measure(
        frame_stats_, frame_pacing::FrameMeasure::draw, static_cast<uint64_t>(composed)
    );
    if (render_run_ && render_run_->last_screen != screen_) {
        render_run_->last_screen = screen_;
        render_run_->screen_since_ns =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()
            )
                                      .count());
    }
    // The loading screen went out through the display sink as it was drawn.
    if (screen_ == Screen::loading)
        return;
    try {
        if (screen_ == Screen::match && match_use_layers_) {
            present_match_layers();
            return;
        }
        present_front_end();
    } catch (const PresentError& error) {
        note_present_error(error.what(), true);
    }
}

void Runtime::present_front_end() {
    // A match's surface_ is composed at the display gamma already.
    const auto present_start = std::chrono::steady_clock::now();
    const bool gamma = !gamma_identity_ && screen_ != Screen::match;
    if (frontend_texture_.format() == SDL_PIXELFORMAT_RGB24) {
        const uint8_t* frame = surface_.rgb.data();
        std::vector<uint8_t> corrected;
        if (gamma) {
            corrected = surface_.rgb;
            apply_gamma_rgb(corrected.data(), corrected.size() / 3U, 3);
            frame = corrected.data();
        }
        frontend_texture_.update(frame, static_cast<int>(surface_.width * 3U), 3);
    } else {
        upload_rgb24_tiles(frontend_texture_, surface_, gamma ? &gamma_table_ : nullptr);
    }
    // The standard tier's scale mode is found before the frame begins: the
    // first look at the pixel-art mode draws into a target of its own.
    const SDL_ScaleMode standard_mode = standard_frame_scale_mode(output_texture_w_);
    if (!SDL_RenderClear(sdl_.renderer))
        throw_present_error("SDL render");
    // The front end's picture goes through the card's filter where it is one
    // texture; tiles beyond the renderer's limit are drawn as before.
    bool drawn = false;
    if (accelerated_presentation() && frontend_texture_.single() != nullptr) {
        try {
            draw_accelerated_screen(
                frontend_texture_.single(),
                output_texture_w_,
                output_texture_h_,
                accelerated_.screen_revision
            );
            drawn = true;
        } catch (const AccelerationError& error) {
            take_acceleration_error(error);
        }
    }
    if (!drawn)
        draw_frame(sdl_.renderer, frontend_texture_, standard_mode);
    capture_render_target();
    if (render_fault_due(RenderFaultPoint::present))
        throw PresentError("injected present error");
    if (render_fault_due(RenderFaultPoint::float_state))
        std::fesetround(FE_TOWARDZERO);
    if (!SDL_RenderPresent(sdl_.renderer))
        note_present_refused("SDL render");
    oa::base::float_precision::restore_program_float_control();
    const auto present_ns = static_cast<uint64_t>(elapsed_since(present_start));
    frame_pacing::note_frame_measure(frame_stats_, frame_pacing::FrameMeasure::present, present_ns);
    note_present_time(present_ns);
}

[[nodiscard]] oa::ui::gui_input::MenuObject Runtime::input_menu() const {
    return {resources_.layout.gadgets, selected_};
}

void write_ppm(const fs::path& path, const renderer::Surface& surface) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot create snapshot: " + path_to_utf8(path));
    output << "P6\n" << surface.width << ' ' << surface.height << "\n255\n";
    output.write(
        reinterpret_cast<const char*>(surface.rgb.data()),
        static_cast<std::streamsize>(surface.rgb.size())
    );
    if (!output)
        throw std::runtime_error("cannot finish snapshot: " + path_to_utf8(path));
}

} // namespace oa::app
