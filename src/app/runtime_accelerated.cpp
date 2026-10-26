// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The accelerated presentation: the battlefield drawn at its draw scale and
// reduced by the exact area pass when zoomed out, or magnified by the
// graphics card with what the painters changed laid over it when zoomed in;
// the interface and the 640x480 screens scaled by the pixel-art or
// sharp-bilinear filter; and the standard tier's world drawn again for the
// readers that keep a picture. It is off unless its host switches it on.
#include "oa/app/runtime.hpp"

#include "render_host.hpp"
#include "render_run.hpp"
#include "xrgb_conversion.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace oa::app {

namespace {

namespace policy = render_policy;
namespace wr = oa::present::world_renderer;

/// Returns the scale the HUD layer is drawn at: the chrome's, or in placed
/// mode the largest a placed region draws at.
///
/// @param layout the match layout
/// @return the scale, in layout pixels per source pixel
double hud_chrome_scale(const oa::ui::display_layout::MatchLayout& layout) {
    return oa::ui::display_layout::placed_mode(layout) && layout.chrome_scale > 0.0
               ? layout.chrome_scale
               : layout.scale;
}

/// Steps the zoom range above 1 is walked in to find the largest prescale
/// target a magnified scene needs.
constexpr int prescale_zoom_steps = 960;

/// The most a prescale factor is asked for, whatever the scale.
constexpr double most_prescale_factor = 64.0;

/// Bytes a texel of the tier's textures and targets holds, and a pixel of
/// the overlay's buffer: ARGB8888.
constexpr uint64_t bytes_per_texel = 4;

/// Returns the nanoseconds since a moment of the steady clock.
///
/// @param since the moment
/// @return nanoseconds
[[nodiscard]] int64_t nanoseconds_since(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - since
    )
        .count();
}

} // namespace

void Runtime::switch_accelerated_presentation(bool on, const policy::LadderState& rung) {
    free_accelerated_presentation();
    accelerated_.on = on;
    accelerated_.rung = rung;
    // The renderer host's limit, which the standard tier's layers are tiled by too.
    accelerated_.texture_limit = on && sdl_.renderer != nullptr ? render_texture_limit() : 0;
}

bool Runtime::accelerated_presentation() const noexcept {
    // A director's frames are the processor's, unless the graphics card's
    // renderer with no window draws them (director_draws_on_card).
    return accelerated_.on && !accelerated_.suspended && sdl_.renderer != nullptr &&
           ((!options_.headless_check && director_ == nullptr) || director_draws_on_card());
}

void Runtime::drop_acceleration(const std::string& reason, render_policy::Drop drop) {
    if (accelerated_.on)
        std::cout << "open-annihilation: graphics: the accelerated tier stopped (" << reason
                  << "); the processor draws everything from now on\n";
    free_accelerated_presentation();
    accelerated_.on = false;
    if (render_run_ && render_run_->host != nullptr) {
        auto& host = *render_run_->host;
        auto& inputs = host.tier_inputs();
        if (inputs.drop == render_policy::Drop::none)
            inputs.drop =
                render_run_->path_refused ? render_policy::Drop::path_trial_unwritten : drop;
        // A path whose first frames stood under its own sentinel stops with
        // the tier.
        host.end_path_stage();
    }
}

void Runtime::take_acceleration_error(const AccelerationError& error) {
    if (render_run_ && render_run_->host != nullptr && !render_run_->path_refused) {
        auto& host = *render_run_->host;
        if (error.fault() == AccelerationFault::driver) {
            // The call that failed is struck against the driver; the same in
            // the next run on it keeps the driver on the standard tier.
            renderer_state::Strike failure;
            failure.stage = renderer_state::StrikeStage::call;
            failure.call = failing_call_name(error.what());
            std::ignore = host.note_running_failure(failure);
        } else if (host.tier_inputs().drop == render_policy::Drop::none) {
            // The game's own error says nothing of the driver.
            host.tier_inputs().drop = render_policy::Drop::engine_fault;
        }
    }
    drop_acceleration(error.what());
    if (render_run_)
        render_run_->path_refused = false;
}

void Runtime::begin_accelerated_path(renderer_state::AcceleratedPath path) {
    if (!render_run_ || render_run_->host == nullptr)
        return;
    if (!render_run_->host->begin_path(path)) {
        render_run_->path_refused = true;
        throw AccelerationError(std::string(path_trial_unwritten_reason));
    }
}

void Runtime::note_card_scale_drawn(const CardScale& scale) noexcept {
    if (render_run_ && scale.filter == policy::ScaleFilter::sharp_bilinear && scale.factor > 1)
        render_run_->paths_drawn = static_cast<PathSet>(
            render_run_->paths_drawn | path_bit(renderer_state::AcceleratedPath::prescale)
        );
}

void Runtime::free_accelerated_layout_textures() noexcept {
    auto& state = accelerated_;
    state.scene.reset();
    state.overlay_texture.reset();
    state.world_prescale.destroy();
    state.hud_prescale.destroy();
    state.overlay = {};
    state.opaque_bands = {};
    state.uploaded_bands = {};
    state.overlay_uploaded = false;
}

void Runtime::free_accelerated_match_textures() noexcept {
    auto& state = accelerated_;
    free_full_match_textures();
    free_accelerated_layout_textures();
    state.base = {};
    state.area_plan = {};
    state.layout_width = 0;
    state.layout_height = 0;
}

void Runtime::free_accelerated_presentation() noexcept {
    free_full_presentation();
    free_accelerated_match_textures();
    // Only the presentation holds the scene's buffers at a scene's size; the
    // standard tier's stay as they are.
    if (accelerated_.on)
        free_accelerated_scene_buffers();
    accelerated_.screen_prescale.destroy();
    accelerated_.frame = {};
    accelerated_.magnified = false;
}

void Runtime::free_accelerated_scene_buffers() noexcept {
    match_scene_cpu_ = {};
    match_terrain_cache_ = {};
    terrain_cache_cam_x_ = kUncachedTerrainCamera;
    terrain_cache_zoom_ = -1.0F;
}

void Runtime::area_filter_scene(const wr::Surface& scene) {
    const uint32_t width = match_world_cpu_.width;
    const uint32_t height = match_world_cpu_.height;
    const float draw_scale = accelerated_.frame.draw_scale;
    const uint32_t scale = area_scale(match_zoom(), draw_scale);
    // A view drawn between map pixels starts the picture that far into the
    // scene; its weights are built again whenever that moves.
    const auto& offset = accelerated_.frame_offset;
    const uint32_t phase_x = area_phase(offset.x, draw_scale, scale);
    const uint32_t phase_y = area_phase(offset.y, draw_scale, scale);
    auto& plan = accelerated_.area_plan;
    if (plan.scale() != scale || plan.picture_width() != width || plan.picture_height() != height ||
        plan.phase_x() != phase_x || plan.phase_y() != phase_y)
        if (wr::plan_area_filter(plan, scale, width, height, phase_x, phase_y) !=
            wr::AreaError::none)
            throw std::runtime_error("cannot weigh the area pass of the battlefield's scene");
    const wr::RgbSource source{scene.rgb.data(), scene.width, scene.height, scene.width};
    const wr::RgbTarget picture{match_world_cpu_.rgb.data(), width, height, width};
    if (wr::area_filter_rgb24(plan, source, picture, draw_pool_.get()) != wr::AreaError::none)
        throw std::runtime_error("cannot reduce the battlefield's scene by the area pass");
}

uint32_t Runtime::accelerated_prescale_factor(
    double scale, uint32_t width, uint32_t height, uint64_t charged
) const {
    // A target beyond the renderer's texture limit is split into tiles, so
    // the budget alone bounds the factor.
    const policy::PrescaleBudget budget{policy::prescale_budget(accelerated_.rung.card), charged};
    return policy::prescale_factor(budget, width, height, scale);
}

CardScale Runtime::accelerated_card_scale(
    policy::ScaleFilter filter,
    double scale,
    uint32_t width,
    uint32_t height,
    const PrescaleTarget& target
) const {
    if (filter != policy::ScaleFilter::sharp_bilinear)
        return {filter, 1};
    // The factor the scale asks for, falling to what the target holds.
    auto factor = static_cast<uint32_t>(std::min(std::ceil(scale), most_prescale_factor));
    while (factor > 1 && (target.width() < factor * width || target.height() < factor * height))
        --factor;
    if (factor <= 1)
        return {policy::ScaleFilter::linear, 1};
    return {filter, factor};
}

void Runtime::ensure_accelerated_match_textures() {
    auto& state = accelerated_;
    // A new layout, or a display of another density, remakes the card's
    // textures and the overlay; the base and the area pass's weights belong
    // to the frame just drawn for it, and stay.
    const double density = match_display_density();
    if (state.layout_width != match_layout_.width || state.layout_height != match_layout_.height ||
        state.layout_density != density) {
        free_accelerated_layout_textures();
        state.layout_width = match_layout_.width;
        state.layout_height = match_layout_.height;
        state.layout_density = density;
    }
    const int battlefield_width = match_layout_.battlefield_width();
    const int battlefield_height = match_layout_.battlefield_height();
    if (battlefield_width <= 0 || battlefield_height <= 0)
        return;
    const auto bf_w = static_cast<uint32_t>(battlefield_width);
    const auto bf_h = static_cast<uint32_t>(battlefield_height);
    const uint32_t limit = state.texture_limit;
    // The HUD layer's prescale target, at the chrome's scale on the
    // display (in placed mode the largest a region draws at), made larger
    // only when a taller build page needs it.
    const double chrome = hud_chrome_scale(match_layout_) * density;
    const uint32_t hud_w = match_hud_cpu_.width;
    const uint32_t hud_h = match_hud_cpu_.height;
    if (hud_w != 0 && hud_h != 0 &&
        policy::chrome_filter(state.rung, chrome) == policy::ScaleFilter::sharp_bilinear) {
        const uint32_t factor = accelerated_prescale_factor(chrome, hud_w, hud_h, 0);
        if (factor > 1 && (state.hud_prescale.width() < factor * hud_w ||
                           state.hud_prescale.height() < factor * hud_h)) {
            const uint32_t target_w = std::max(state.hud_prescale.width(), factor * hud_w);
            const uint32_t target_h = std::max(state.hud_prescale.height(), factor * hud_h);
            if (accelerated_buffer_allowed(
                    policy::AcceleratedBuffer::prescale,
                    uint64_t{target_w} * target_h * bytes_per_texel
                )) {
                begin_accelerated_path(renderer_state::AcceleratedPath::prescale);
                state.hud_prescale.ensure(
                    sdl_.renderer, nullptr, target_w, target_h, limit, state.counts
                );
            }
        }
    }
    // The magnified world's textures are made at the first frame drawn
    // through them, so that the stage of the path's first frames, which
    // counts the frames drawn with it, begins with its first draws.
    if (!state.rung.magnify || state.frame.method != SceneMethod::magnify)
        return;
    // The scene, at the largest a magnified frame draws, and the overlay.
    const auto largest = largest_magnified_scene(battlefield_width, battlefield_height);
    const auto scene_w = static_cast<uint32_t>(largest.width);
    const auto scene_h = static_cast<uint32_t>(largest.height);
    // The scene's texture, the overlay's and the overlay's buffer, unless
    // the memory guard refuses them: the tier then stays at magnify off.
    if (!state.scene.made() || !state.overlay_texture.made()) {
        if (!accelerated_buffer_allowed(
                policy::AcceleratedBuffer::scene,
                (uint64_t{scene_w} * scene_h + 2 * uint64_t{bf_w} * bf_h) * bytes_per_texel
            ))
            return;
        begin_accelerated_path(renderer_state::AcceleratedPath::magnify);
    }
    if (!state.scene.made())
        state.scene.create(
            sdl_.renderer, scene_w, scene_h, limit, SDL_BLENDMODE_NONE, state.counts
        );
    if (!state.overlay_texture.made()) {
        state.overlay_texture.create(
            sdl_.renderer, bf_w, bf_h, limit, SDL_BLENDMODE_BLEND, state.counts
        );
        state.overlay_uploaded = false;
        const uint32_t bands = platform::job_pool::bands_of_rows(bf_h, xrgb_band_rows);
        state.opaque_bands.assign(bands, 0);
        state.uploaded_bands.assign(bands, 0);
        state.overlay.assign(std::size_t{bf_w} * bf_h * 4U, 0);
    }
    // The scene's prescale target, at the largest any zoom above 1 needs on
    // the display within what the HUD's target leaves of the budget, in
    // tiles beyond the renderer's limit as the scene is; made at the first
    // frame whose scale on the display the card magnifies through it.
    const bool prescaled = state.rung.card == policy::CardFilter::prescale_full ||
                           state.rung.card == policy::CardFilter::prescale_quarter;
    if (!prescaled || state.world_prescale.made())
        return;

    // The corner a zoom shows and the column and row past it, which LINEAR
    // reads, and its prescale factor; 1 or less where nothing is prescaled.
    struct Corner {
        uint32_t width{};
        uint32_t height{};
        uint32_t factor{};
    };

    const auto corner_at = [&](float zoom, double scale) {
        Corner corner;
        corner.width = std::min(magnified_span(bf_w, zoom, scene_w, 0.0).corner + 1, scene_w);
        corner.height = std::min(magnified_span(bf_h, zoom, scene_h, 0.0).corner + 1, scene_h);
        corner.factor = accelerated_prescale_factor(
            scale, corner.width, corner.height, state.hud_prescale.pixels()
        );
        return corner;
    };
    // The frame's scene at its scale on the display, as the draw takes it.
    const float frame_zoom = match_zoom();
    const double frame_scale = world_display_scale(state.frame, frame_zoom, density);
    if (policy::world_filter(state.rung, frame_scale) != policy::ScaleFilter::sharp_bilinear ||
        corner_at(frame_zoom, frame_scale).factor <= 1)
        return;
    uint32_t widest = 0;
    uint32_t tallest = 0;
    for (int step = 1; step <= prescale_zoom_steps; ++step) {
        const float zoom = 1.0F + (kMaxBattlefieldZoom - 1.0F) * static_cast<float>(step) /
                                      static_cast<float>(prescale_zoom_steps);
        const double scale = static_cast<double>(zoom) * density;
        if (std::floor(scale) == scale)
            continue;
        const Corner corner = corner_at(zoom, scale);
        if (corner.factor <= 1)
            continue;
        widest = std::max(widest, corner.factor * corner.width);
        tallest = std::max(tallest, corner.factor * corner.height);
    }
    const policy::PrescaleBudget left{
        policy::prescale_budget(state.rung.card), state.hud_prescale.pixels()
    };
    if (widest != 0 && uint64_t{widest} * tallest + left.charged <= left.limit &&
        accelerated_buffer_allowed(
            policy::AcceleratedBuffer::prescale, uint64_t{widest} * tallest * bytes_per_texel
        )) {
        begin_accelerated_path(renderer_state::AcceleratedPath::prescale);
        state.world_prescale.ensure(sdl_.renderer, nullptr, widest, tallest, limit, state.counts);
    }
}

void Runtime::present_accelerated_match_layers(bool dialogs) {
    auto& state = accelerated_;
    // The front end's prescale target is not kept during a match.
    state.screen_prescale.destroy();
    const auto frame_format = opaque_layer_format();
    ensure_streaming_texture(
        match_hud_tex_,
        frame_format,
        static_cast<int>(match_hud_cpu_.width),
        static_cast<int>(match_hud_cpu_.height),
        match_hud_tex_w_,
        match_hud_tex_h_
    );
    ensure_accelerated_match_textures();
    const auto& frame = state.frame;
    const uint32_t bf_w = match_world_cpu_.width;
    const uint32_t bf_h = match_world_cpu_.height;
    const bool magnified = frame.method == SceneMethod::magnify && state.scene.made() &&
                           state.overlay_texture.made_for(bf_w, bf_h, state.texture_limit) &&
                           match_scene_cpu_.width == static_cast<uint32_t>(frame.scene_width) &&
                           match_scene_cpu_.height == static_cast<uint32_t>(frame.scene_height) &&
                           match_scene_cpu_.width <= state.scene.grid().width &&
                           match_scene_cpu_.height <= state.scene.grid().height &&
                           state.base.size() == match_world_cpu_.rgb.size();
    state.magnified = magnified;
    if (!magnified)
        match_world_tex_.ensure(
            sdl_.renderer,
            frame_format,
            static_cast<int>(bf_w),
            static_cast<int>(bf_h),
            render_texture_limit(),
            SDL_BLENDMODE_NONE
        );
    const auto* gamma = gamma_identity_ ? nullptr : &gamma_table_;
    const auto upload_start = std::chrono::steady_clock::now();
    upload_rgb24_frame(
        match_hud_tex_, match_hud_cpu_, accelerated_.hud_revision, &spare_match_hud_
    );
    if (magnified) {
        // The scene's upload and the overlay's conversion and upload.
        state.scene.upload_rgb24(
            match_scene_cpu_.rgb.data(),
            std::size_t{match_scene_cpu_.width} * 3U,
            match_scene_cpu_.width,
            match_scene_cpu_.height,
            gamma,
            draw_pool_.get()
        );
        // What the painters changed, uploaded in the bands that hold it now
        // or held it at the last upload, so the texture holds this frame's
        // overlay whatever frames came before.
        const std::size_t pitch = std::size_t{bf_w} * 4U;
        convert_rgb24_overlay_argb(
            match_world_cpu_.rgb.data(),
            state.base.data(),
            bf_w,
            bf_h,
            state.overlay.data(),
            pitch,
            gamma,
            state.opaque_bands,
            draw_pool_.get()
        );
        const auto bands = static_cast<uint32_t>(state.opaque_bands.size());
        const auto stale = [&](uint32_t band) {
            return !state.overlay_uploaded || state.opaque_bands[band] != 0 ||
                   state.uploaded_bands[band] != 0;
        };
        for (uint32_t band = 0; band < bands;) {
            if (!stale(band)) {
                ++band;
                continue;
            }
            uint32_t end = band + 1;
            while (end < bands && stale(end))
                ++end;
            state.overlay_texture.update(
                state.overlay.data(),
                static_cast<int>(pitch),
                4,
                band * xrgb_band_rows,
                std::min(bf_h, end * xrgb_band_rows)
            );
            band = end;
        }
        state.uploaded_bands = state.opaque_bands;
        state.overlay_uploaded = true;
    } else {
        upload_rgb24_tiles(match_world_tex_, match_world_cpu_, gamma);
    }
    const auto present_start = std::chrono::steady_clock::now();
    phase_times_.upload += nanoseconds_since(upload_start);
    if (!SDL_SetRenderDrawColor(sdl_.renderer, 0, 0, 0, 255) || !SDL_RenderClear(sdl_.renderer))
        throw_present_error("SDL_RenderClear");
    // Every scale is the one at the display's pixels: on a window at native
    // density the layout's scale times the density.
    const double density = match_display_density();
    // In placed mode the HUD's pieces are drawn after the world
    // (finish_match_layers).
    if (!oa::ui::display_layout::placed_mode(match_layout_))
        draw_accelerated_hud_strips();
    const SDL_Rect battlefield{
        match_layout_.left, match_layout_.top, static_cast<int>(bf_w), static_cast<int>(bf_h)
    };
    const SDL_FRect world{
        static_cast<float>(battlefield.x),
        static_cast<float>(battlefield.y),
        static_cast<float>(bf_w),
        static_cast<float>(bf_h)
    };
    if (!magnified) {
        // 1:1 in layout pixels, at the density on the display; below zoom
        // 1, where the area pass runs, its result, made at the layout's
        // size at every density.
        draw_one_to_one(sdl_.renderer, match_world_tex_, nullptr, &world, one_to_one_scale_mode());
    } else {
        const float zoom = match_zoom();
        const uint32_t scene_w = match_scene_cpu_.width;
        const uint32_t scene_h = match_scene_cpu_.height;
        // A view drawn between map pixels shows one more column and row,
        // and lands its offset before the battlefield's edge, which the clip
        // cuts off. The card's filter is the one a view on its camera's map
        // pixel has, so the picture keeps its filter as the view moves.
        const auto& offset = state.frame_offset;
        const auto across = magnified_span(bf_w, zoom, scene_w, offset.x);
        const auto down = magnified_span(bf_h, zoom, scene_h, offset.y);
        const uint32_t whole_width = magnified_span(bf_w, zoom, scene_w, 0.0).corner;
        const uint32_t whole_height = magnified_span(bf_h, zoom, scene_h, 0.0).corner;
        const SDL_FRect destination{
            static_cast<float>(battlefield.x + across.start),
            static_cast<float>(battlefield.y + down.start),
            static_cast<float>(across.extent),
            static_cast<float>(down.extent)
        };
        // The magnified scene's filter, chosen on its scale at the display.
        const double scene_scale = world_display_scale(frame, zoom, density);
        const CardScale world_scale = accelerated_card_scale(
            policy::world_filter(state.rung, scene_scale),
            scene_scale,
            std::min(whole_width + 1, scene_w),
            std::min(whole_height + 1, scene_h),
            state.world_prescale
        );
        note_card_scale_drawn(world_scale);
        if (render_run_)
            render_run_->paths_drawn = static_cast<PathSet>(
                render_run_->paths_drawn | path_bit(renderer_state::AcceleratedPath::magnify)
            );
        draw_scaled_world(
            sdl_.renderer,
            nullptr,
            state.scene,
            scene_w,
            scene_h,
            across.corner,
            down.corner,
            destination,
            battlefield,
            world_scale,
            state.world_prescale,
            state.counts
        );
        draw_one_to_one(
            sdl_.renderer, state.overlay_texture, nullptr, &world, one_to_one_scale_mode()
        );
    }
    finish_match_layers(frame_format, dialogs, upload_start, present_start);
}

void Runtime::draw_accelerated_hud_strips() {
    auto& state = accelerated_;
    // The HUD strips by the chrome's filter, at the display's scale; in
    // placed mode the placed regions instead, at the largest region's scale.
    std::vector<SharpPart> strips;
    if (oa::ui::display_layout::placed_mode(match_layout_)) {
        const std::size_t count = std::min<std::size_t>(
            match_layout_.placed_count, oa::ui::display_layout::kMaxPlacedRegions
        );
        for (std::size_t index = 0; index < count; ++index) {
            const auto& region = match_layout_.placed[index];
            if (region.source.width <= 0 || region.source.height <= 0 || region.canvas.width <= 0 ||
                region.canvas.height <= 0)
                continue;
            strips.push_back(
                {{static_cast<float>(region.source.x),
                  static_cast<float>(region.source.y),
                  static_cast<float>(region.source.width),
                  static_cast<float>(region.source.height)},
                 {static_cast<float>(region.canvas.x),
                  static_cast<float>(region.canvas.y),
                  static_cast<float>(region.canvas.width),
                  static_cast<float>(region.canvas.height)}}
            );
        }
        if (strips.empty())
            return;
    } else {
        for (const auto& strip : match_hud_strips())
            if (strip.w > 0 && strip.h > 0 && strip.source_w > 0 && strip.source_h > 0)
                strips.push_back(
                    {{static_cast<float>(strip.source_x),
                      static_cast<float>(strip.source_y),
                      static_cast<float>(strip.source_w),
                      static_cast<float>(strip.source_h)},
                     {static_cast<float>(strip.x),
                      static_cast<float>(strip.y),
                      static_cast<float>(strip.w),
                      static_cast<float>(strip.h)}}
                );
    }
    const double chrome = hud_chrome_scale(match_layout_) * match_display_density();
    const CardScale hud_scale = accelerated_card_scale(
        policy::chrome_filter(state.rung, chrome),
        chrome,
        match_hud_cpu_.width,
        match_hud_cpu_.height,
        state.hud_prescale
    );
    note_card_scale_drawn(hud_scale);
    sharp_draw(
        sdl_.renderer,
        nullptr,
        match_hud_tex_,
        match_hud_cpu_.width,
        match_hud_cpu_.height,
        strips,
        hud_scale,
        state.hud_revision,
        state.hud_prescale,
        state.counts
    );
}

void Runtime::draw_accelerated_screen(
    SDL_Texture* texture, int width, int height, uint64_t revision
) {
    if (texture == nullptr || width <= 0 || height <= 0)
        return;
    // The letterbox's scale: window pixels per frame pixel.
    SDL_FRect area{};
    // The letterbox is SDL's own reckoning, which no driver call makes: one
    // that cannot be had is the game's error.
    if (!SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &area) || !(area.w > 0.0F))
        throw AccelerationError(
            std::string("SDL_GetRenderLogicalPresentationRect: ") + SDL_GetError(),
            AccelerationFault::engine
        );
    const double scale = static_cast<double>(area.w) / static_cast<double>(width);
    const auto w = static_cast<uint32_t>(width);
    const auto h = static_cast<uint32_t>(height);
    auto& state = accelerated_;
    const policy::ScaleFilter filter =
        policy::frame_filter(menu_scaling(), &state.rung, false, scale);
    if (filter == policy::ScaleFilter::sharp_bilinear) {
        const uint32_t factor = accelerated_prescale_factor(scale, w, h, 0);
        // A target of a new size is made only where the memory guard allows it.
        const bool made = state.screen_prescale.made() &&
                          state.screen_prescale.width() == factor * w &&
                          state.screen_prescale.height() == factor * h;
        if (factor > 1 && (made || accelerated_buffer_allowed(
                                       policy::AcceleratedBuffer::prescale,
                                       uint64_t{factor * w} * (factor * h) * bytes_per_texel
                                   ))) {
            begin_accelerated_path(renderer_state::AcceleratedPath::prescale);
            state.screen_prescale.ensure(
                sdl_.renderer, nullptr, factor * w, factor * h, state.texture_limit, state.counts
            );
        }
    }
    const SharpPart whole{
        {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)},
        {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)}
    };
    const CardScale screen_scale =
        accelerated_card_scale(filter, scale, w, h, state.screen_prescale);
    note_card_scale_drawn(screen_scale);
    sharp_draw(
        sdl_.renderer,
        nullptr,
        texture,
        w,
        h,
        {&whole, 1},
        screen_scale,
        revision,
        state.screen_prescale,
        state.counts
    );
}

void Runtime::ensure_screen_world() {
    if (!match_ || screen_ != Screen::match)
        return;
    // A Full frame's world layer is the overlay canvas, the key colour but
    // where the painters painted, not a picture.
    const auto method = accelerated_.frame.method;
    if (method != SceneMethod::area && method != SceneMethod::magnify && !full_frame_drawn())
        return;

    // The world drawn again as the standard tier draws it, at the moment
    // the last frame showed; its units counted for this draw alone, and the
    // resource readout, which the frame's draw eased, not eased again.
    struct Restore {
        Runtime& runtime;
        frame_pacing::FrameDrawCounts draws;
        float alpha{};
        std::optional<uint32_t> readout_eases;

        ~Restore() {
            runtime.accelerated_.suspended = false;
            runtime.set_presentation_alpha(alpha);
            runtime.frame_draws_ = draws;
            runtime.readout_eases_ = readout_eases;
        }
    } restore{*this, frame_draws_, presentation_alpha(), readout_eases_};

    frame_draws_ = {};
    readout_eases_ = 0U;
    set_presentation_alpha(accelerated_.frame_alpha);
    accelerated_.suspended = true;
    refresh_filtered_terrain();
    render_match_surface();
}

} // namespace oa::app
