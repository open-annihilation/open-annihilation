// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Director frames on the graphics card (DirectorPresentation::full,
// docs/director.md): director mode drawn by the Full tier, the game's own
// presenter of the battlefield on the card, on SDL's GPU renderer with no
// window. Each frame is presented into a texture of the layout's size,
// read back and cut at the view's offset as the processor's frame is, so
// that the director's camera, clock and sound stay as they are; only the
// pixels are the card's, which differ from one driver to the next.
#include "oa/app/runtime.hpp"
#include "director_state.hpp"
#include "full_presentation.hpp"

#include "oa/media/director.hpp"
#include "oa/ui/display_layout.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

namespace oa::app {
namespace {

namespace policy = render_policy;

/// Bytes of one pixel of a director frame: red, green and blue.
constexpr size_t kPixelBytes = 3;

/// Returns the rung the Full tier draws director frames at: the top of the
/// ladder, as a capable machine starts a match on Full, the card reading
/// its pages by the pixel-art sampling mode where the renderer takes it.
///
/// @return the rung
[[nodiscard]] policy::LadderState director_card_rung() noexcept {
    policy::LadderState rung;
    rung.full = true;
    rung.method = policy::ZoomOutMethod::area;
    rung.budget = policy::SceneBudget::full;
    rung.blend_allowed = true;
    rung.magnify = true;
    rung.filtered_chrome = true;
    rung.card = policy::CardFilter::pixelart;
    rung.standard = false;
    return rung;
}

/// Returns the widest margin a view of an output can need: the output's
/// height over the director's smallest view height, rounded up, as
/// engine_view rounds a view's zoom up into its margin. The battlefield is
/// drawn that much larger whatever the frame's own margin, so that the
/// card's targets keep their size from frame to frame.
///
/// @param height the output's height, pixels
/// @return the margin, pixels, at least 1
[[nodiscard]] uint32_t widest_margin(uint32_t height) noexcept {
    const double zoom = static_cast<double>(height) / oa::media::director::min_view_height;
    return std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(zoom)));
}

} // namespace

bool Runtime::director_draws_on_card() const noexcept {
    return director_ != nullptr && director_->card.renderer != nullptr &&
           sdl_.renderer == director_->card.renderer;
}

void Runtime::open_director_card() {
    if (director_ == nullptr)
        throw std::logic_error("the director's renderer opens in director mode");
#if !SDL_VERSION_ATLEAST(3, 4, 0)
    throw DirectorCardUnavailable(
        "drawing director frames on the graphics card needs SDL 3.4 or later, which makes a "
        "renderer with no window"
    );
#else
    auto& card = director_->card;
    // A run with no window takes no place among the system's applications,
    // unless SDL's environment variable of the hint's name says otherwise.
    std::ignore = SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        throw DirectorCardUnavailable(std::string("SDL's video did not start: ") + SDL_GetError());
    card.video_started = true;
    card.renderer = SDL_CreateGPURenderer(nullptr, nullptr);
    if (card.renderer == nullptr)
        throw DirectorCardUnavailable(
            std::string("no renderer on a graphics card: ") + SDL_GetError()
        );
    if (SDL_GPUDevice* device = static_cast<SDL_GPUDevice*>(SDL_GetPointerProperty(
            SDL_GetRendererProperties(card.renderer), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr
        ));
        device != nullptr) {
        const char* driver = SDL_GetGPUDeviceDriver(device);
        card.driver = driver != nullptr ? driver : "";
    }
    if (card.driver.empty())
        card.driver = "unknown";
    card.kept_renderer = sdl_.renderer;
    sdl_.renderer = card.renderer;
    switch_accelerated_presentation(true, director_card_rung());
    set_full_presentation(true);
    // The executor opens on the renderer and the Full function test runs
    // before any frame, so a card that cannot draw Full stops the render
    // before its first frame rather than part way.
    try {
        ensure_full_executor();
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string("the graphics card cannot draw the Full tier: ") + error.what()
        );
    }
#endif
}

void Runtime::close_director_card(DirectorState& state) noexcept {
    auto& card = state.card;
    // The live renderer that drew turned frames holds a device of its own
    // on SDL's video, which goes before the video stops.
    if (state.live_turned)
        live_renderer_.reset();
    state.live_turned = false;
    if (card.renderer != nullptr) {
        if (full_)
            full_->final_target = nullptr;
        set_full_presentation(false);
        free_accelerated_presentation();
        try {
            destroy_match_layer_textures();
        } catch (...) {
        }
        accelerated_.on = false;
        if (card.frame != nullptr)
            SDL_DestroyTexture(card.frame);
        SDL_DestroyRenderer(card.renderer);
        sdl_.renderer = card.kept_renderer;
    }
    if (card.video_started)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    card = {};
}

void Runtime::draw_director_card_frame(std::span<uint8_t> rgb) {
    if (!director_draws_on_card() || !director_->view_set)
        throw std::logic_error(
            "a director frame is drawn on the card once its renderer is open and its camera set"
        );
    auto& director = *director_;
    auto& card = director.card;
    const auto& presentation = director.presentation;
    if (rgb.size() != static_cast<size_t>(presentation.width) * presentation.height * kPixelBytes)
        throw std::invalid_argument("the director frame's buffer is not width * height * 3 bytes");
    const auto view = director.view;
    const auto width = static_cast<int>(presentation.width);
    const auto height = static_cast<int>(presentation.height);
    namespace layout = oa::ui::display_layout;
    const auto* gamma = director.gamma_identity ? nullptr : &director.gamma_table;
    const size_t row_bytes = static_cast<size_t>(width) * kPixelBytes;
    // A turned camera is the live renderer's rotated view, as the game draws
    // it on Full: drawn at the frame's own size, centred on the script's
    // exact centre and zoom, with the yaw and pitch the script gives.
    const auto& aim = director.aim;
    const bool turned =
        oa::media::director::turned(aim) &&
        turn_live_camera(static_cast<float>(aim.yaw), static_cast<float>(aim.pitch));
    if (turned) {
        director.live_turned = true;
        match_layout_ = presentation.show_interface
                            ? lay_out_match(width, height)
                            : layout::make_battlefield_layout(width, height);
        set_director_view(view);
        // The view's place, which the live renderer centres on, from the
        // script's centre rather than the whole map pixel at its corner.
        exact_view_ = {
            true,
            aim.x - static_cast<double>(match_layout_.battlefield_width()) / (2.0 * view.zoom),
            aim.z - static_cast<double>(match_layout_.battlefield_height()) / (2.0 * view.zoom),
            match_camera_x_,
            match_camera_z_
        };
        refresh_filtered_terrain();
        render_match_surface();
        exact_view_.held = false;
        if (live_renderer_unavailable())
            throw DirectorCardUnavailable(
                "the turned camera needs the 3D renderer, and SDL made no graphics device it "
                "can draw with: it draws on Metal or Vulkan"
            );
        if (!live_renderer_active() || live_renderer_failed())
            throw std::runtime_error("the 3D renderer did not draw the turned frame");
        renderer::Surface composed;
        const renderer::Surface* picture = &match_world_cpu_;
        if (presentation.show_interface) {
            compose_match_layers(composed);
            picture = &composed;
        }
        if (picture->width < presentation.width || picture->height < presentation.height ||
            picture->rgb.size() <
                static_cast<size_t>(picture->width) * picture->height * kPixelBytes)
            throw std::logic_error("the turned frame does not cover the director's frame");
        for (int row = 0; row < height; ++row) {
            const uint8_t* from =
                picture->rgb.data() + static_cast<size_t>(row) * picture->width * kPixelBytes;
            uint8_t* to = rgb.data() + static_cast<size_t>(row) * row_bytes;
            if (gamma == nullptr) {
                std::memcpy(to, from, row_bytes);
                continue;
            }
            for (size_t byte = 0; byte < row_bytes; ++byte)
                to[byte] = (*gamma)[from[byte]];
        }
        return;
    }
    // A classic frame after a turned one draws on the card again.
    if (director.live_turned)
        reset_live_renderer();
    // The frame read back: the whole frame with the interface, else the
    // battlefield drawn larger from the whole map pixel at the view's corner
    // and cut at the view's offset, as the processor's frame is cut.
    SDL_Rect area{0, 0, width, height};
    if (presentation.show_interface) {
        match_layout_ = lay_out_match(width, height);
    } else {
        const auto margin =
            static_cast<int>(std::max(view.margin, widest_margin(presentation.height)));
        match_layout_ = layout::make_battlefield_layout(width + margin, height + margin);
        area.x = match_layout_.left + static_cast<int>(view.offset_x);
        area.y = match_layout_.top + static_cast<int>(view.offset_y);
    }
    set_director_view(view);
    refresh_filtered_terrain();
    render_match_surface();
    if (card.frame == nullptr || card.frame_width != match_layout_.width ||
        card.frame_height != match_layout_.height) {
        if (card.frame != nullptr)
            SDL_DestroyTexture(card.frame);
        card.frame = SDL_CreateTexture(
            card.renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_TARGET,
            match_layout_.width,
            match_layout_.height
        );
        card.frame_width = card.frame != nullptr ? match_layout_.width : 0;
        card.frame_height = card.frame != nullptr ? match_layout_.height : 0;
        if (card.frame == nullptr)
            throw std::runtime_error(
                "the director's frame could not be made on the graphics card: " +
                std::string(SDL_GetError())
            );
    }
    full_->final_target = card.frame;
    if (!SDL_SetRenderTarget(card.renderer, card.frame))
        throw std::runtime_error(
            std::string("SDL_SetRenderTarget of the director's frame: ") + SDL_GetError()
        );
    if (!full_presentation() || !present_full_match_layers(false))
        throw std::runtime_error(
            "the graphics card's Full tier did not draw the frame; the processor's tiers "
            "are not used in its place"
        );
    using SurfaceOwner = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
    SurfaceOwner read(SDL_RenderReadPixels(card.renderer, &area), SDL_DestroySurface);
    SurfaceOwner frame(
        read ? SDL_ConvertSurface(read.get(), SDL_PIXELFORMAT_RGB24) : nullptr, SDL_DestroySurface
    );
    if (!frame || frame->w != width || frame->h != height)
        throw std::runtime_error(
            std::string("the director's frame could not be read back: ") + SDL_GetError()
        );
    for (int row = 0; row < height; ++row) {
        const auto* from =
            static_cast<const uint8_t*>(frame->pixels) + static_cast<ptrdiff_t>(row) * frame->pitch;
        uint8_t* to = rgb.data() + static_cast<size_t>(row) * row_bytes;
        if (gamma == nullptr) {
            std::memcpy(to, from, row_bytes);
            continue;
        }
        for (size_t byte = 0; byte < row_bytes; ++byte)
            to[byte] = (*gamma)[from[byte]];
    }
}

} // namespace oa::app
