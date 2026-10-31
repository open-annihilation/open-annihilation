// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// SDL output textures, viewport sizing and frame presentation.
#include "mod_install_watch.hpp"
#include "window_live_size.hpp"
#include "oa/app/frame_coordinates.hpp"
#include "oa/app/window_surface.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/game_directory.hpp"
#include "graphics_report.hpp"
#include "oa_layer.hpp"
#include "pad_state.hpp"
#include "render_host.hpp"
#include "render_state.hpp"
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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

[[nodiscard]] int64_t elapsed_since(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - since
    )
        .count();
}

/// Tells whether a resize is waiting to be handled.
///
/// A size the game has not handled yet is still delivered as that resize,
/// so laying the match out for it early is the same scale, and is not a
/// size that arrived with no resize at all.
///
/// @return true when a resize or a pixel-size change is queued
[[nodiscard]] bool resize_event_waiting() {
    SDL_Event waiting{};
    if (SDL_PeepEvents(
            &waiting, 1, SDL_PEEKEVENT, SDL_EVENT_WINDOW_RESIZED, SDL_EVENT_WINDOW_RESIZED
        ) > 0)
        return true;
    return SDL_PeepEvents(
               &waiting,
               1,
               SDL_PEEKEVENT,
               SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
               SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
           ) > 0;
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

SDL_Point Runtime::match_canvas_point() const {
    // On a window at native density a layout pixel is a window point, which
    // logical presentation stretches over the display's pixels, so the
    // processor lays out and draws what it does on any other window of that
    // size; elsewhere a layout pixel is a window pixel.
    int width = kCanvasWidth;
    int height = kCanvasHeight;
    if (sdl_.window != nullptr) {
        if (native_density_window())
            SDL_GetWindowSize(sdl_.window, &width, &height);
        else
            SDL_GetWindowSizeInPixels(sdl_.window, &width, &height);
    }
    // Full screen on the desktop's mode at a screen size of its own draws
    // the match at that size.
    const SDL_Point scaled = scaled_frame_size();
    if (scaled.x > 0 && scaled.y > 0)
        return scaled;
    return {width, height};
}

void Runtime::sync_live_window_size() {
    if (sdl_.window == nullptr)
        return;
    const SDL_WindowFlags flags = SDL_GetWindowFlags(sdl_.window);
    // Full screen and a maximised or minimised window report their own size
    // as they change; asking again would move a window the player placed.
    if ((flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) != 0)
        return;
    int live_width = 0;
    int live_height = 0;
    if (!platform_window_content_size(sdl_.window, live_width, live_height))
        return;
    if (live_width == nudged_window_width_ && live_height == nudged_window_height_)
        return;
    int taken_width = 0;
    int taken_height = 0;
    SDL_GetWindowSize(sdl_.window, &taken_width, &taken_height);
    if (live_width == taken_width && live_height == taken_height)
        return;
    std::cout << "open-annihilation: the window is " << live_width << 'x' << live_height
              << " and had been taken as " << taken_width << 'x' << taken_height << '\n';
    // Asked once for this size, whether or not the window took it, so a size
    // it refuses is not asked again every frame.
    nudged_window_width_ = live_width;
    nudged_window_height_ = live_height;
    std::ignore = SDL_SetWindowSize(sdl_.window, live_width, live_height);
}

void Runtime::follow_window_output() {
    if (sdl_.renderer == nullptr)
        return;
    sync_live_window_size();
    if (screen_ != Screen::match)
        return;
    const SDL_Point canvas = match_canvas_point();
    if (canvas.x <= 0 || canvas.y <= 0)
        return;
    if (canvas.x == followed_canvas_width_ && canvas.y == followed_canvas_height_)
        return;
    // A resize still waiting is handled in its turn. A layout that is not
    // this window, with no resize waiting, was taken from an earlier size.
    if ((match_layout_.width != canvas.x || match_layout_.height != canvas.y) &&
        !resize_event_waiting()) {
        int window_width = 0;
        int window_height = 0;
        if (sdl_.window != nullptr)
            SDL_GetWindowSize(sdl_.window, &window_width, &window_height);
        std::cout << "open-annihilation: the match was laid out at " << match_layout_.width << 'x'
                  << match_layout_.height << " for a " << window_width << 'x' << window_height
                  << " window\n";
    }
    apply_output_mode();
}

void Runtime::apply_output_mode() {
    if (sdl_.renderer == nullptr)
        return;
    if (screen_ == Screen::match) {
        const bool native_density = native_density_window();
        const SDL_Point canvas = match_canvas_point();
        int width = canvas.x;
        int height = canvas.y;
        // The window's size in points, from which the layout's density, its
        // safe area and (with touch controls) its device class come; none
        // without a window.
        int window_width = 0, window_height = 0;
        if (sdl_.window != nullptr)
            SDL_GetWindowSize(sdl_.window, &window_width, &window_height);
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
        // The size just laid out, so a later frame lays out only when the
        // window's size is no longer this one.
        followed_canvas_width_ = width;
        followed_canvas_height_ = height;
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
        followed_canvas_width_ = 0;
        followed_canvas_height_ = 0;
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
        // A window this runtime made itself still takes a check's pixel density.
        apply_asked_pixel_density(sdl_.window);
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
    const bool key_event = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
    // Loading and a movie hand the event here without the run's feed. The
    // layout's chord is known only after that feed. A key the bindings hold
    // back is not the switch until they admit it; its release is still taken,
    // so a held chord's key can come up.
    if (key_event && !match_key_already_fed_ && feed_input_actions(event) &&
        event.type == SDL_EVENT_KEY_DOWN)
        return false;
    const oa::ui::key_bindings::ActionEvent* full_screen = fired_action("ui.full-screen");
    const bool switch_chord = event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                              full_screen != nullptr &&
                              full_screen->phase == oa::ui::key_bindings::Phase::pressed;
    return oa::app::take_full_screen_event(sdl_.window, full_screen_switch_, event, switch_chord);
}

void Runtime::trace_window(std::string_view reason) {
    if (!options_.trace_input || sdl_.window == nullptr)
        return;
    SDL_Window* const window = sdl_.window;
    WindowTrace trace{};
    trace.reason = reason;
    // A value SDL cannot read stays 0, and the line shows it so.
    std::ignore = SDL_GetWindowPosition(window, &trace.window.x, &trace.window.y);
    std::ignore = SDL_GetWindowSize(window, &trace.window.w, &trace.window.h);
    std::ignore = SDL_GetWindowSizeInPixels(window, &trace.pixel_width, &trace.pixel_height);
    trace.display = SDL_GetDisplayForWindow(window);
    if (trace.display != 0) {
        std::ignore = SDL_GetDisplayBounds(trace.display, &trace.bounds);
        std::ignore = SDL_GetDisplayUsableBounds(trace.display, &trace.usable);
    }
    trace.scale = SDL_GetWindowDisplayScale(window);
    trace.flags = SDL_GetWindowFlags(window);
    trace.exclusive = SDL_GetWindowFullscreenMode(window) != nullptr;
    if (const SDL_Rect* held = SDL_GetWindowMouseRect(window); held != nullptr)
        trace.mouse_rect = *held;
    std::ignore = SDL_GetGlobalMouseState(&trace.pointer_x, &trace.pointer_y);
    std::cerr << window_trace_line(trace) << '\n';
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

namespace {

/// Which frame a streaming texture holds since upload_rgb24_frame last
/// uploaded into it. It is kept among the texture's properties, so that it
/// goes with the texture: a texture made again, at a new size or after the
/// device went, starts without one and is uploaded whole. The frame's
/// pixels are not kept: the caller's frame before the next one stands for
/// them.
struct UploadedFrame {
    uint64_t frame{};  ///< the caller's number for the frame
    uint32_t width{};  ///< the surface's pixels in a row
    uint32_t height{}; ///< its rows
    /// The display gamma the texture's pixels went through; unset for a
    /// gamma of 1.
    std::optional<std::array<uint8_t, 256>> gamma;
    /// The bands of xrgb_band_rows rows that differ from the frame before,
    /// one entry a band.
    std::vector<uint8_t> changed;
};

/// The name of the texture property that holds its UploadedFrame.
constexpr const char* uploaded_frame_property = "oa.uploaded_rgb24_frame";

/// Lets a texture's UploadedFrame go with the texture's properties.
///
/// @param value the UploadedFrame
void free_uploaded_frame(void*, void* value) {
    delete static_cast<UploadedFrame*>(value);
}

} // namespace

void Runtime::upload_rgb24_frame(
    SDL_Texture* texture,
    const renderer::Surface& source,
    uint64_t frame,
    const renderer::Surface* before
) {
    if (texture == nullptr || source.rgb.empty())
        return;
    const auto* gamma = gamma_identity_ ? nullptr : &gamma_table_;
    // XRGB8888 and ARGB8888 take the same opaque words.
    const bool rgb565 = texture->format == SDL_PIXELFORMAT_RGB565;
    const SDL_PropertiesID properties = SDL_GetTextureProperties(texture);
    auto* uploaded = properties != 0
                         ? static_cast<UploadedFrame*>(
                               SDL_GetPointerProperty(properties, uploaded_frame_property, nullptr)
                           )
                         : nullptr;
    const std::size_t row_bytes = static_cast<std::size_t>(source.width) * 3U;
    // The frame before stands for what the texture holds when the texture
    // holds that frame, through the same gamma, and it is the source's size.
    const bool follows =
        uploaded != nullptr && before != nullptr && uploaded->frame + 1U == frame &&
        uploaded->width == source.width && uploaded->height == source.height &&
        before->width == source.width && before->height == source.height &&
        before->rgb.size() == source.rgb.size() && source.rgb.size() >= row_bytes * source.height &&
        uploaded->gamma.has_value() == (gamma != nullptr) &&
        (gamma == nullptr || *uploaded->gamma == *gamma);
    if (!follows) {
        void* pixels = nullptr;
        int pitch = 0;
        if (!SDL_LockTexture(texture, nullptr, &pixels, &pitch))
            throw_present_error("SDL_LockTexture");
        (rgb565 ? convert_rgb24_rgb565 : convert_rgb24_xrgb)(
            source.rgb.data(),
            source.width,
            source.height,
            static_cast<uint8_t*>(pixels),
            static_cast<std::size_t>(pitch),
            gamma,
            draw_pool_.get()
        );
        SDL_UnlockTexture(texture);
        if (properties == 0)
            return;
        if (uploaded == nullptr) {
            uploaded = new UploadedFrame;
            if (!SDL_SetPointerPropertyWithCleanup(
                    properties, uploaded_frame_property, uploaded, free_uploaded_frame, nullptr
                ))
                // SDL let the record go already; every upload stays whole.
                return;
        }
        uploaded->frame = frame;
        uploaded->width = source.width;
        uploaded->height = source.height;
        uploaded->gamma.reset();
        if (gamma != nullptr)
            uploaded->gamma = *gamma;
        uploaded->changed.assign(
            platform::job_pool::bands_of_rows(source.height, xrgb_band_rows), 0
        );
        return;
    }
    // Only the bands that differ from the frame before, which the texture
    // holds, are uploaded, a run of neighbouring bands at once.
    find_changed_rgb24_bands(
        source.rgb.data(), before->rgb.data(), source.width, source.height, uploaded->changed
    );
    const auto bands = static_cast<uint32_t>(uploaded->changed.size());
    for (uint32_t band = 0; band < bands;) {
        if (uploaded->changed[band] == 0) {
            ++band;
            continue;
        }
        uint32_t end = band + 1;
        while (end < bands && uploaded->changed[end] != 0)
            ++end;
        const uint32_t first_row = band * xrgb_band_rows;
        const uint32_t end_row = std::min(source.height, end * xrgb_band_rows);
        const SDL_Rect rows{
            0,
            static_cast<int>(first_row),
            static_cast<int>(source.width),
            static_cast<int>(end_row - first_row)
        };
        void* pixels = nullptr;
        int pitch = 0;
        if (!SDL_LockTexture(texture, &rows, &pixels, &pitch))
            throw_present_error("SDL_LockTexture");
        const uint8_t* from = source.rgb.data() + static_cast<std::size_t>(first_row) * row_bytes;
        (rgb565 ? convert_rgb24_rgb565 : convert_rgb24_xrgb)(
            from,
            source.width,
            end_row - first_row,
            static_cast<uint8_t*>(pixels),
            static_cast<std::size_t>(pitch),
            gamma,
            draw_pool_.get()
        );
        SDL_UnlockTexture(texture);
        band = end;
    }
    uploaded->frame = frame;
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
    if (turned_world_tex_ != nullptr)
        SDL_DestroyTexture(turned_world_tex_);
    turned_world_tex_ = nullptr;
    turned_world_source_ = nullptr;
    if (match_cursor_tex_ != nullptr) {
        SDL_DestroyTexture(match_cursor_tex_);
        match_cursor_tex_ = nullptr;
    }
    match_dialog_tex_.reset();
    if (match_dialog_side_tex_ != nullptr) {
        SDL_DestroyTexture(match_dialog_side_tex_);
        match_dialog_side_tex_ = nullptr;
    }
    if (oa_layer_)
        oa_layer_->destroy_textures();
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
    oa_layer().compose_match(frame);
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
    if (accelerated_presentation() && !live_renderer_active()) {
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
    const auto upload_start = std::chrono::steady_clock::now();
    // A turned frame the card keeps is drawn from the card, with what was
    // painted over it added there; any other battlefield is uploaded.
    SDL_Texture* const card_world = compose_turned_overlay();
    if (card_world == nullptr)
        match_world_tex_.ensure(
            sdl_.renderer,
            frame_format,
            static_cast<int>(match_world_cpu_.width),
            static_cast<int>(match_world_cpu_.height),
            render_texture_limit(),
            SDL_BLENDMODE_NONE
        );
    upload_rgb24_frame(
        match_hud_tex_, match_hud_cpu_, accelerated_.hud_revision, &spare_match_hud_
    );
    if (card_world == nullptr)
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
    if (card_world != nullptr) {
        if (!SDL_SetTextureScaleMode(card_world, scaled_frame_mode_))
            std::ignore = SDL_SetTextureScaleMode(card_world, SDL_SCALEMODE_NEAREST);
    } else if (!match_world_tex_.set_scale_mode(scaled_frame_mode_)) {
        std::ignore = match_world_tex_.set_scale_mode(SDL_SCALEMODE_NEAREST);
    }
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
    if (card_world == nullptr)
        match_world_tex_.draw(sdl_.renderer, nullptr, &world);
    else if (!SDL_RenderTexture(sdl_.renderer, card_world, nullptr, &world))
        throw_present_error("SDL_RenderTexture");
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
    // A frame drawn again to be read back is read as the battlefield stands.
    if (accelerated_.reading_back)
        return;
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
    oa_layer().present(nullptr);
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
    // A check of the OA layer, whose screens and backdrop are drawn in the
    // window's own pixels, reads back the whole window on the front end, the
    // letterbox round the picture with it (OaLayer::read_whole_window).
    std::optional<RenderState> window_pixels;
    if (screen_ != Screen::match && oa_layer().reads_whole_window()) {
        window_pixels.emplace(sdl_.renderer);
        window_pixels->use_window_pixels();
    }
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
    // A middle-click pan draws its own direction at the pointer.
    if (middle_pan_shows_direction())
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
    // Above an OA screen the cursor is stamped into a texture the size of
    // the picture, at the same whole pixels as the cursor in the picture,
    // and enlarged with the picture, while the whole sprite sits inside that
    // picture. A texture of the cursor alone, scaled on its own rectangle,
    // does not land on those pixels. A sprite that crosses the picture, as
    // the pointer does over a footer in the letterbox, is drawn in the
    // window's own pixels on the rectangle the picture is drawn in.
    const bool over_layer = !match_layers && screen_ != Screen::match && oa_layer().shows(false);
    const int sprite_x = static_cast<int>(cursor_x) - frame.origin_x;
    const int sprite_y = static_cast<int>(cursor_y) - frame.origin_y;
    const bool stamp_in_picture = over_layer && sprite_x >= 0 && sprite_y >= 0 &&
                                  sprite_x + static_cast<int>(frame.width) <= output_texture_w_ &&
                                  sprite_y + static_cast<int>(frame.height) <= output_texture_h_;
    const int canvas_w = stamp_in_picture ? output_texture_w_ : static_cast<int>(frame.width);
    const int canvas_h = stamp_in_picture ? output_texture_h_ : static_cast<int>(frame.height);
    if (canvas_w <= 0 || canvas_h <= 0)
        return;
    if (match_cursor_tex_ == nullptr || match_cursor_tex_w_ != canvas_w ||
        match_cursor_tex_h_ != canvas_h) {
        if (match_cursor_tex_ != nullptr)
            SDL_DestroyTexture(match_cursor_tex_);
        match_cursor_tex_ = SDL_CreateTexture(
            sdl_.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, canvas_w, canvas_h
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
        match_cursor_tex_w_ = canvas_w;
        match_cursor_tex_h_ = canvas_h;
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(match_cursor_tex_, nullptr, &pixels, &pitch))
        return;
    const auto& pal = match_palette_.size() >= 1024 ? match_palette_ : resources_.gui_palette;
    const auto cursor_texel = [&frame, &pal](std::size_t offset) {
        if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
            return 0u;
        const auto pal_i = static_cast<std::size_t>(frame.pixels[offset]) * 4U;
        if (pal_i + 2 >= pal.size())
            return 0u;
        return 0xff000000u | (static_cast<uint32_t>(pal[pal_i]) << 16) |
               (static_cast<uint32_t>(pal[pal_i + 1]) << 8) | static_cast<uint32_t>(pal[pal_i + 2]);
    };
    if (stamp_in_picture) {
        for (int row = 0; row < canvas_h; ++row) {
            auto* dst = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(pixels) + static_cast<std::size_t>(row) * pitch
            );
            std::fill(dst, dst + canvas_w, 0u);
        }
        // The same whole pixels tick_and_draw_cursor writes into the picture.
        const int destination_x = sprite_x;
        const int destination_y = sprite_y;
        for (uint32_t row = 0; row < frame.height; ++row) {
            const int y = destination_y + static_cast<int>(row);
            if (y < 0 || y >= canvas_h)
                continue;
            auto* dst = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch
            );
            for (uint32_t column = 0; column < frame.width; ++column) {
                const int x = destination_x + static_cast<int>(column);
                if (x < 0 || x >= canvas_w)
                    continue;
                dst[x] = cursor_texel(static_cast<std::size_t>(row) * frame.width + column);
            }
            if (!gamma_identity_)
                gamma_xrgb_row(dst, canvas_w, gamma_table_);
        }
    } else {
        for (uint32_t row = 0; row < frame.height; ++row) {
            auto* dst = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(pixels) + static_cast<std::size_t>(row) * pitch
            );
            for (uint32_t column = 0; column < frame.width; ++column)
                dst[column] = cursor_texel(static_cast<std::size_t>(row) * frame.width + column);
            if (!gamma_identity_)
                gamma_xrgb_row(dst, static_cast<int>(frame.width), gamma_table_);
        }
    }
    SDL_UnlockTexture(match_cursor_tex_);
    if (over_layer && !stamp_in_picture) {
        int logical_w = 0;
        int logical_h = 0;
        SDL_RendererLogicalPresentation presentation = SDL_LOGICAL_PRESENTATION_DISABLED;
        SDL_FRect reckoned{};
        if (SDL_GetRenderLogicalPresentation(
                sdl_.renderer, &logical_w, &logical_h, &presentation
            ) &&
            presentation != SDL_LOGICAL_PRESENTATION_DISABLED && logical_w > 0 && logical_h > 0 &&
            SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &reckoned) && reckoned.w > 0.0F &&
            reckoned.h > 0.0F) {
            // The picture's filter, read before the window-pixel state drops
            // the logical presentation this lookup needs.
            const SDL_ScaleMode picture_mode = standard_frame_scale_mode(output_texture_w_);
            RenderState window_pixels(sdl_.renderer);
            window_pixels.use_window_pixels();
            const SDL_Rect picture = drawn_frame_rect(reckoned);
            const float scale_x = static_cast<float>(picture.w) / static_cast<float>(logical_w);
            const float scale_y = static_cast<float>(picture.h) / static_cast<float>(logical_h);
            const SDL_FRect outside{
                static_cast<float>(picture.x) + static_cast<float>(sprite_x) * scale_x,
                static_cast<float>(picture.y) + static_cast<float>(sprite_y) * scale_y,
                static_cast<float>(frame.width) * scale_x,
                static_cast<float>(frame.height) * scale_y
            };
            const bool filtered = picture_mode != SDL_SCALEMODE_NEAREST;
            if (filtered)
                std::ignore = SDL_SetTextureScaleMode(match_cursor_tex_, picture_mode);
            else
                std::ignore = SDL_SetTextureScaleMode(match_cursor_tex_, SDL_SCALEMODE_NEAREST);
            std::ignore = SDL_RenderTexture(sdl_.renderer, match_cursor_tex_, nullptr, &outside);
            if (filtered && SDL_GetVersion() < SDL_VERSIONNUM(3, 2, 10))
                std::ignore = SDL_FlushRenderer(sdl_.renderer);
            if (filtered)
                std::ignore = SDL_SetTextureScaleMode(match_cursor_tex_, SDL_SCALEMODE_NEAREST);
            return;
        }
    }
    if (stamp_in_picture) {
        // The picture's filter, over the picture's whole rectangle, so the
        // cursor enlarges as it does inside the picture.
        const SDL_ScaleMode mode = standard_frame_scale_mode(canvas_w);
        const bool filtered = mode != SDL_SCALEMODE_NEAREST;
        if (filtered)
            std::ignore = SDL_SetTextureScaleMode(match_cursor_tex_, mode);
        else
            std::ignore = SDL_SetTextureScaleMode(match_cursor_tex_, SDL_SCALEMODE_NEAREST);
        std::ignore = SDL_RenderTexture(sdl_.renderer, match_cursor_tex_, nullptr, nullptr);
        // An older SDL draws a queued texture with the scale mode it has
        // when the queue runs. Put NEAREST back only after this draw has run.
        if (filtered && SDL_GetVersion() < SDL_VERSIONNUM(3, 2, 10))
            std::ignore = SDL_FlushRenderer(sdl_.renderer);
        if (filtered)
            std::ignore = SDL_SetTextureScaleMode(match_cursor_tex_, SDL_SCALEMODE_NEAREST);
        return;
    }
    const SDL_FRect dest{
        cursor_x - static_cast<float>(frame.origin_x),
        cursor_y - static_cast<float>(frame.origin_y),
        static_cast<float>(frame.width),
        static_cast<float>(frame.height)
    };
    // On a window at native density, and over a scaled frame, the cursor
    // over the match goes to the display's pixels with the match's layers;
    // any other frame keeps SDL's LINEAR. A mode the texture refuses leaves
    // it as it was.
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
    // The frame is drawn at the window's size, including a size that changed
    // with no resize. A headless check and a director lay the match out at a
    // size of their own; the window must not replace it.
    if (!options_.headless_check && director_ == nullptr)
        follow_window_output();
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
    // The window presents a turned view through the card's device.
    follow_card_presentation();
    const auto compose_start = std::chrono::steady_clock::now();
    {
        // What this frame draws is drawn to be presented.
        struct Presenting {
            bool& flag;

            explicit Presenting(bool& presenting) : flag(presenting) { flag = true; }

            Presenting(const Presenting&) = delete;
            Presenting& operator=(const Presenting&) = delete;

            ~Presenting() { flag = false; }
        } presenting{presenting_frame_};

        rebuild_surface();
    }
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
    if (screen_ == Screen::loading) {
        note_shown_frame();
        return;
    }
    try {
        if (screen_ == Screen::match && match_use_layers_) {
            present_match_layers();
            note_shown_frame();
            return;
        }
        present_front_end();
        note_shown_frame();
    } catch (const PresentError& error) {
        note_present_error(error.what(), true);
    }
}

void Runtime::present_front_end() {
    // A match's surface_ is composed at the display gamma already.
    const auto present_start = std::chrono::steady_clock::now();
    const bool gamma = !gamma_identity_ && screen_ != Screen::match;
    // A window larger than the picture draws the main menu's OA buttons again
    // from an icon at least that large, so those squares are put back to the
    // menu under them first.
    std::vector<uint8_t> menu_buttons;
    const bool sharp_menu_buttons = replace_menu_button_pixels(menu_buttons);
    const uint8_t* source_rgb = sharp_menu_buttons ? menu_buttons.data() : surface_.rgb.data();
    if (frontend_texture_.format() == SDL_PIXELFORMAT_RGB24) {
        const uint8_t* frame = source_rgb;
        std::vector<uint8_t> corrected;
        if (gamma) {
            corrected.assign(source_rgb, source_rgb + surface_.rgb.size());
            apply_gamma_rgb(corrected.data(), corrected.size() / 3U, 3);
            frame = corrected.data();
        }
        frontend_texture_.update(frame, static_cast<int>(surface_.width * 3U), 3);
    } else if (sharp_menu_buttons) {
        renderer::Surface punched = surface_;
        punched.rgb = menu_buttons;
        if (gamma)
            apply_gamma_rgb(punched.rgb.data(), punched.rgb.size() / 3U, 3);
        upload_rgb24_tiles(frontend_texture_, punched, nullptr);
    } else {
        upload_rgb24_tiles(frontend_texture_, surface_, gamma ? &gamma_table_ : nullptr);
    }
    // Place the picture from the drawing surface this frame, before the scale
    // mode is read from that place. A drawable that grew after the window
    // opened, as a 2× display whose first size was in points, would otherwise
    // keep the picture in the corner.
    const int frame_w = output_texture_w_ > 0 ? output_texture_w_ : kCanvasWidth;
    const int frame_h = output_texture_h_ > 0 ? output_texture_h_ : kCanvasHeight;
    if (!set_frame_presentation(sdl_.renderer, menu_scaling(), frame_w, frame_h))
        throw_present_error("SDL logical presentation");
    int output_w = 0;
    int output_h = 0;
    SDL_RendererLogicalPresentation presented_mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    const bool have_output = SDL_GetRenderOutputSize(sdl_.renderer, &output_w, &output_h) &&
                             output_w > 0 && output_h > 0;
    std::ignore =
        SDL_GetRenderLogicalPresentation(sdl_.renderer, nullptr, nullptr, &presented_mode);
    const bool whole_steps = presented_mode == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE;
    const SDL_FRect wanted =
        have_output ? frame_letterbox(output_w, output_h, frame_w, frame_h, whole_steps)
                    : SDL_FRect{};
    SDL_FRect reckoned{};
    const bool have_rect = SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &reckoned);
    const bool sdl_places =
        !have_output ||
        (have_rect && std::fabs(reckoned.x - wanted.x) <= 0.05F &&
         std::fabs(reckoned.y - wanted.y) <= 0.05F && std::fabs(reckoned.w - wanted.w) <= 0.05F &&
         std::fabs(reckoned.h - wanted.h) <= 0.05F);
    // The standard tier's scale mode is found before the frame begins: the
    // first look at the pixel-art mode draws into a target of its own.
    const SDL_ScaleMode standard_mode = standard_frame_scale_mode(output_texture_w_);
    // The logical presentation does not match the drawing surface. Draw the
    // frame into the letterbox ourselves, in the window's own pixels.
    if (!sdl_places) {
        std::ignore = SDL_SetRenderLogicalPresentation(
            sdl_.renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED
        );
        std::ignore = SDL_SetRenderScale(sdl_.renderer, 1.0F, 1.0F);
        std::ignore = SDL_SetRenderViewport(sdl_.renderer, nullptr);
        std::ignore = SDL_SetRenderClipRect(sdl_.renderer, nullptr);
    }
    // The bars beside the picture are black, whatever colour the renderer
    // last drew with (the launch's quiet frame leaves its backdrop colour).
    if (!SDL_SetRenderDrawColor(sdl_.renderer, 0, 0, 0, 255) || !SDL_RenderClear(sdl_.renderer))
        throw_present_error("SDL render");
    // The front end's picture goes through the card's filter where it is one
    // texture; tiles beyond the renderer's limit are drawn as before.
    bool drawn = false;
    if (!sdl_places) {
        draw_frame(sdl_.renderer, frontend_texture_, standard_mode, &wanted);
        drawn = true;
    } else if (accelerated_presentation() && frontend_texture_.single() != nullptr) {
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
    // The icons go over the scaled picture and under the screens, which cover
    // the buttons while a dialog is open.
    if (sharp_menu_buttons)
        present_menu_icon_buttons();
    // The cursor was left out of the picture so the window-pixel buttons
    // could replace it. It goes above those buttons.
    if (menu_button_cursor_held())
        present_software_cursor(false);
    // Open Annihilation's own screens go over the picture in the window's own
    // pixels, and the software cursor over them (tick_and_draw_cursor left it
    // out of the frame).
    if (oa_layer().shows(false)) {
        // The picture's place. When the logical presentation was set aside,
        // SDL's rectangle is the whole output; the frame was drawn at wanted.
        if (!sdl_places) {
            oa_layer().present(&wanted);
        } else {
            SDL_FRect picture_area{};
            if (SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &picture_area))
                oa_layer().present(&picture_area);
        }
        present_software_cursor(false);
    }
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
