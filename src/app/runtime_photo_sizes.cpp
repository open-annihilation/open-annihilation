// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Photo sizes larger than the window, and smooth edges. The size plan says
// which ones the battlefield, the render mode and the memory can take. The
// picture is drawn by the mode the window is using, then box-reduced, and
// the window's own picture is put back.
#include "oa/app/runtime.hpp"

#include "full_presentation.hpp"
#include "photo_clipboard.hpp"
#include "photo_state.hpp"
#include "photo_writer.hpp"
#include "render_run.hpp"
#include "render_state.hpp"

#include "renderer_live_gpu.hpp"

#include "oa/app/card.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/media/director.hpp"
#include "oa/present/model/unit_supersampling.hpp"
#include "oa/present/photo/looks.hpp"
#include "oa/present/photo/photo_sizes.hpp"
#include "oa/present/photo/photo_tiles.hpp"
#include "oa/ui/photo_panel/words.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace photo = oa::present::photo;
namespace policy = render_policy;
namespace supersampling = full_supersampling;

constexpr std::string_view kLimit = "{limit}";

/// Puts `limit` in place of `{limit}`.
///
/// @param pattern the sentence
/// @param limit the pixel cap
/// @return the sentence
std::string fill_limit(std::string_view pattern, int32_t limit) {
    std::string text(pattern);
    const std::size_t at = text.find(kLimit);
    if (at == std::string::npos)
        return text;
    text.replace(at, kLimit.size(), std::to_string(limit));
    return text;
}

/// The anti-aliasing request whose fitted sampling is `target` for a tile.
///
/// @param width the tile's draw
/// @param height the tile's draw
/// @param target the window's sampling
/// @param device_multisamples whether the card multisamples
/// @return the request, or 0 when none of 1, 2, 4, 8 or 16 matches
uint32_t sampling_request_for(
    uint32_t width, uint32_t height, renderer_live_gpu::Sampling target, bool device_multisamples
) {
    if (target.supersample == 0)
        return 0;
    for (uint32_t ask = 1; ask <= 16; ask *= 2) {
        const renderer_live_gpu::Sampling fitted =
            renderer_live_gpu::fitted_sampling(width, height, ask, device_multisamples);
        if (fitted.supersample == target.supersample && fitted.multisample == target.multisample)
            return ask;
    }
    return 0;
}

/// The largest same-aspect picture, in pixels on its long side, whose
/// sampling matches the window's frame. `cap` is the hard edge. A picture
/// the window itself cannot describe keeps the window's long side, so
/// Screen stays offered.
///
/// @param field_w the battlefield, in pixels
/// @param field_h the battlefield, in pixels
/// @param cap the hard edge, in pixels
/// @param requested the anti-aliasing the window asked for
/// @param device_multisamples whether the card multisamples
/// @return the edge, in pixels
int32_t turned_sampling_edge(
    int32_t field_w, int32_t field_h, int32_t cap, uint32_t requested, bool device_multisamples
) {
    if (cap < 1)
        cap = 1;
    if (field_w < 1 || field_h < 1)
        return cap;
    const int32_t long_side = std::max(field_w, field_h);
    const int32_t short_side = std::min(field_w, field_h);
    if (long_side > cap)
        return cap;
    const auto play = renderer_live_gpu::fitted_sampling(
        static_cast<uint32_t>(field_w),
        static_cast<uint32_t>(field_h),
        requested,
        device_multisamples
    );
    const auto same = [&](int32_t edge) {
        if (edge < 1)
            return false;
        const int64_t other64 = int64_t{edge} * int64_t{short_side} / int64_t{long_side};
        if (other64 < 1 || other64 > INT32_MAX)
            return false;
        const int32_t other = static_cast<int32_t>(other64);
        const bool wide = field_w >= field_h;
        const auto fitted = renderer_live_gpu::fitted_sampling(
            static_cast<uint32_t>(wide ? edge : other),
            static_cast<uint32_t>(wide ? other : edge),
            requested,
            device_multisamples
        );
        return fitted.supersample == play.supersample && fitted.multisample == play.multisample &&
               fitted.supersample != 0;
    };
    if (play.supersample == 0 || !same(long_side))
        return long_side;
    if (same(cap))
        return cap;
    int32_t low = long_side;
    int32_t high = cap;
    while (low + 1 < high) {
        const int32_t mid = low + (high - low) / 2;
        if (same(mid))
            low = mid;
        else
            high = mid;
    }
    return low;
}

/// The largest square, in pixels a side, that an anti-aliasing request draws
/// with the window's sampling. A turned photo's tiles are squarer than the
/// window, and the card's multisampling is limited by area, so a tile as long
/// as the sampling edge can lose it where a picture of the window's shape
/// keeps it.
///
/// @param cap the hard edge, in pixels
/// @param play the window's sampling
/// @param device_multisamples whether the card multisamples
/// @return the side, at least 1
int32_t
turned_tile_square(int32_t cap, renderer_live_gpu::Sampling play, bool device_multisamples) {
    const auto keeps = [&](int32_t side) {
        return sampling_request_for(
                   static_cast<uint32_t>(side),
                   static_cast<uint32_t>(side),
                   play,
                   device_multisamples
               ) != 0;
    };
    if (cap <= 1 || !keeps(1))
        return 1;
    if (keeps(cap))
        return cap;
    int32_t low = 1;
    int32_t high = cap;
    while (low + 1 < high) {
        const int32_t mid = low + (high - low) / 2;
        if (keeps(mid))
            low = mid;
        else
            high = mid;
    }
    return low;
}

/// The largest byte count `allows` accepts, or every byte when it accepts
/// any count that fits in the search.
///
/// @param allows true when a draw of that many bytes still fits
/// @return the count, 0 when one byte does not fit
template <class Allows>
uint64_t largest_room(const Allows& allows) {
    if (!allows(uint64_t{1}))
        return 0;
    uint64_t low = 1;
    uint64_t high = 1;
    while (allows(high)) {
        if (high > (uint64_t{1} << 62))
            return ~uint64_t{0};
        low = high;
        high *= 2;
    }
    while (low + 1 < high) {
        const uint64_t mid = low + (high - low) / 2;
        if (allows(mid))
            low = mid;
        else
            high = mid;
    }
    return low;
}

/// Map pixels the draw shows. A zoom that is not a view returns 1, and the
/// director's camera then refuses the shot.
///
/// @param draw the draw's pixels on this axis
/// @param zoom output pixels a map pixel
/// @return the map pixels, at least 1
int32_t visible_of(int32_t draw, double zoom) {
    if (!(zoom > 0.0) || !std::isfinite(zoom) || draw <= 0)
        return 1;
    const double pixels = static_cast<double>(draw) / zoom;
    if (!(pixels > 0.0) || pixels > static_cast<double>(std::numeric_limits<int32_t>::max()))
        return 1;
    const long rounded = std::lround(pixels);
    if (rounded < 1 || rounded > std::numeric_limits<int32_t>::max())
        return 1;
    return static_cast<int32_t>(rounded);
}

/// Output pixels past the whole map pixel, kept within the margin.
///
/// @param fraction map pixels past the camera, from 0 to 1
/// @param zoom output pixels a map pixel
/// @param margin the draw's extra pixels
/// @return the offset
uint32_t offset_of(double fraction, double zoom, uint32_t margin) {
    if (!(fraction > 0.0) || !(zoom > 0.0) || !std::isfinite(zoom))
        return 0;
    const double pixels = std::floor(fraction * zoom);
    if (!(pixels > 0.0))
        return 0;
    if (pixels >= static_cast<double>(margin))
        return margin;
    return static_cast<uint32_t>(pixels);
}

/// The processor camera that draws one photo.
///
/// @param camera_x the window's camera column
/// @param camera_z the window's camera row
/// @param fraction_x map pixels past that column
/// @param fraction_y map pixels past that row
/// @param draw_width the draw, in pixels
/// @param draw_height the draw, in pixels
/// @param zoom output pixels a map pixel
/// @return the camera
oa::media::director::EngineView engine_shot_of(
    int32_t camera_x,
    int32_t camera_z,
    double fraction_x,
    double fraction_y,
    int32_t draw_width,
    int32_t draw_height,
    double zoom
) {
    oa::media::director::EngineView shot;
    shot.left = std::max(camera_x, int32_t{0});
    shot.top = std::max(camera_z, int32_t{0});
    shot.zoom = zoom;
    shot.visible_width = visible_of(draw_width, zoom);
    shot.visible_height = visible_of(draw_height, zoom);
    uint32_t margin = 1;
    const double rounded = std::ceil(zoom);
    if (rounded >= 1.0 && rounded <= static_cast<double>(std::numeric_limits<uint32_t>::max()))
        margin = std::max(uint32_t{1}, static_cast<uint32_t>(rounded));
    shot.margin = margin;
    shot.offset_x = offset_of(fraction_x, zoom, margin);
    shot.offset_y = offset_of(fraction_y, zoom, margin);
    return shot;
}

/// The unit supersampling level whose factor is `factor`. Any other factor
/// is off, one sample.
///
/// @param factor the factor the window draws at
/// @return the level
oa::present::model::UnitSupersampling level_for(uint32_t factor) noexcept {
    using oa::present::model::UnitSupersampling;
    switch (factor) {
    case 2:
        return UnitSupersampling::x2;
    case 4:
        return UnitSupersampling::x4;
    case 8:
        return UnitSupersampling::x8;
    case 16:
        return UnitSupersampling::x16;
    default:
        return UnitSupersampling::off;
    }
}

/// Keeps a target texture and destroys it.
struct TextureHold {
    SDL_Texture* texture{}; ///< the target, or null

    TextureHold() = default;
    TextureHold(const TextureHold&) = delete;
    TextureHold& operator=(const TextureHold&) = delete;

    ~TextureHold() {
        if (texture != nullptr)
            SDL_DestroyTexture(texture);
    }
};

/// Copies packed RGB out of a surface, row by row.
///
/// @param frame the surface
/// @param[out] rgb the picture
/// @return false when the surface is not the draw
bool take_rgb(
    const SDL_Surface* frame, int32_t draw_width, int32_t draw_height, std::vector<uint8_t>& rgb
) {
    if (frame == nullptr || frame->pixels == nullptr || frame->w != draw_width ||
        frame->h != draw_height || frame->pitch < draw_width * 3)
        return false;
    const auto row_bytes = static_cast<std::size_t>(draw_width) * 3U;
    const auto* bytes = static_cast<const uint8_t*>(frame->pixels);
    const auto pitch = static_cast<std::size_t>(frame->pitch);
    rgb.resize(row_bytes * static_cast<std::size_t>(draw_height));
    for (int32_t row = 0; row < draw_height; ++row)
        std::memcpy(
            rgb.data() + static_cast<std::size_t>(row) * row_bytes,
            bytes + static_cast<std::size_t>(row) * pitch,
            row_bytes
        );
    return true;
}

/// Box-reduces a draw into the picture the player asked for. A smooth step
/// of 1 keeps the draw.
///
/// @param draw the plan's picture and draw
/// @param drawn the draw
/// @param[out] rgb the picture
/// @param[out] width its width
/// @param[out] height its height
/// @return false when the draw is not a whole number of blocks
bool commit_picture(
    const photo::Draw& draw,
    std::vector<uint8_t>& drawn,
    std::vector<uint8_t>& rgb,
    uint32_t& width,
    uint32_t& height
) {
    if (draw.width <= 0 || draw.height <= 0)
        return false;
    if (draw.smooth <= 1) {
        const std::size_t bytes =
            static_cast<std::size_t>(draw.width) * static_cast<std::size_t>(draw.height) * 3U;
        if (drawn.size() != bytes)
            return false;
        width = static_cast<uint32_t>(draw.width);
        height = static_cast<uint32_t>(draw.height);
        rgb = std::move(drawn);
        return true;
    }
    std::vector<uint8_t> reduced;
    int32_t out_width = 0;
    int32_t out_height = 0;
    if (!photo::box_reduce(
            drawn.data(),
            drawn.size(),
            draw.draw_width,
            draw.draw_height,
            draw.smooth,
            reduced,
            out_width,
            out_height
        ) ||
        out_width != draw.width || out_height != draw.height)
        return false;
    width = static_cast<uint32_t>(out_width);
    height = static_cast<uint32_t>(out_height);
    rgb = std::move(reduced);
    return true;
}

/// The factor the offers keep: the chosen one when it is offered, else the
/// greatest that is.
///
/// @param chosen the factor stored
/// @param offers the row
/// @param count how many offers
/// @return the factor to store
int kept_offer(int chosen, const photo::Offer* offers, int count) {
    int greatest = chosen;
    bool open = false;
    bool any = false;
    for (int index = 0; index < count; ++index) {
        if (!offers[index].offered)
            continue;
        any = true;
        greatest = offers[index].factor;
        if (offers[index].factor == chosen)
            open = true;
    }
    if (open || !any)
        return chosen;
    return greatest;
}

} // namespace

namespace words = oa::ui::photo_panel::words;

bool Runtime::restoring_photo_window(const std::function<bool()>& draw) {
    // The window's picture, camera and Full targets, put back however the
    // photo returns. Constructed before the photo changes any of them.
    struct WindowRestorer {
        Runtime& self;
        oa::ui::display_layout::MatchLayout layout;
        int32_t camera_x{0};
        int32_t camera_z{0};
        float zoom{1.0F};
        float zoom_target{1.0F};
        ExactView exact{};
        ViewHold view_hold{};
        bool between_pixels{false};
        bool tracking{false};
        uint16_t tracked{0};
        bool live{false};
        bool reading_back{false};
        oa::present::model::UnitSupersampling units{oa::present::model::UnitSupersampling::off};
        bool have_full{false};
        SDL_Texture* final_target{};
        uint32_t supersample{1};
        uint32_t supersample_asked{1};
        uint32_t refused_width{0};
        uint32_t refused_height{0};
        uint32_t refused_factor{0};
        renderer::Surface world{};
        // The player's Game block. A photo's draw binds its own camera there,
        // and photo mode's frames do not bind the player's back.
        std::unique_ptr<oa::Game> game;
        // In a replay, photo mode puts the framed view back on the window's
        // frames while a seek is in flight. That belongs to the window: the
        // photo draws its own view at its own zoom, and leaves it armed.
        bool guard_camera{false};
        bool guard_awaiting{false};

        explicit WindowRestorer(Runtime& runtime)
            : self(runtime), layout(runtime.match_layout_), camera_x(runtime.match_camera_x_),
              camera_z(runtime.match_camera_z_), zoom(runtime.match_zoom_),
              zoom_target(runtime.match_zoom_target_), exact(runtime.exact_view_),
              view_hold(runtime.view_hold_), between_pixels(runtime.view_between_pixels_),
              tracking(runtime.match_tracking_), tracked(runtime.tracked_match_unit_),
              live(runtime.photo_capture_live_), reading_back(runtime.accelerated_.reading_back),
              units(runtime.unit_supersampling_), have_full(runtime.full_ != nullptr) {
            if (have_full) {
                final_target = runtime.full_->final_target;
                supersample = runtime.full_->supersample;
                supersample_asked = runtime.full_->supersample_asked;
                refused_width = runtime.full_->refused_world_width;
                refused_height = runtime.full_->refused_world_height;
                refused_factor = runtime.full_->refused_world_factor;
            }
            world.width = runtime.match_world_cpu_.width;
            world.height = runtime.match_world_cpu_.height;
            world.rgb = runtime.match_world_cpu_.rgb;
            if (runtime.match_ != nullptr)
                game = std::make_unique<oa::Game>(runtime.match_->state().game);
            if (runtime.photo_ != nullptr) {
                guard_camera = runtime.photo_->guard_camera;
                guard_awaiting = runtime.photo_->guard_awaiting;
                runtime.photo_->guard_camera = false;
            }
        }

        WindowRestorer(const WindowRestorer&) = delete;
        WindowRestorer& operator=(const WindowRestorer&) = delete;

        ~WindowRestorer() {
            if (self.director_mode())
                self.leave_director_mode();
            self.match_layout_ = layout;
            self.match_camera_x_ = camera_x;
            self.match_camera_z_ = camera_z;
            self.match_zoom_ = zoom;
            self.match_zoom_target_ = zoom_target;
            self.exact_view_ = exact;
            self.view_hold_ = view_hold;
            self.view_between_pixels_ = between_pixels;
            self.match_tracking_ = tracking;
            self.tracked_match_unit_ = tracked;
            self.photo_capture_live_ = live;
            self.accelerated_.reading_back = reading_back;
            self.unit_supersampling_ = units;
            if (have_full && self.full_ != nullptr) {
                self.full_->final_target = final_target;
                self.full_->supersample = supersample;
                self.full_->supersample_asked = supersample_asked;
                self.full_->refused_world_width = refused_width;
                self.full_->refused_world_height = refused_height;
                self.full_->refused_world_factor = refused_factor;
            }
            self.match_world_cpu_.width = world.width;
            self.match_world_cpu_.height = world.height;
            self.match_world_cpu_.rgb.swap(world.rgb);
            try {
                self.render_match_surface();
            } catch (...) {
            }
            if (game && self.match_ != nullptr)
                self.match_->state().game = *game;
            if (self.photo_ != nullptr) {
                self.photo_->guard_camera = guard_camera;
                self.photo_->guard_awaiting = guard_awaiting;
            }
        }
    };

    const WindowRestorer held(*this);
    return draw();
}

photo::Machine Runtime::photo_capture_machine() {
    photo::Machine machine;
    photo::Field field;
    if (match_) {
        field.width = match_layout_.battlefield_width();
        field.height = match_layout_.battlefield_height();
    }
    if (live_renderer_active())
        machine.kind = photo::CaptureKind::turned;
    else if (full_presentation())
        machine.kind = photo::CaptureKind::full;
    else
        machine.kind = photo::CaptureKind::processor;

    const uint32_t texture = render_texture_limit();
    const int32_t texture_edge =
        texture == 0
            ? 0
            : static_cast<int32_t>(
                  std::min(texture, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
              );
    if (machine.kind == photo::CaptureKind::processor)
        machine.edge_limit = photo::kProcessorEdge;
    else if (machine.kind == photo::CaptureKind::turned) {
        const int32_t cap =
            texture_edge > 0 ? std::min(photo::kTurnedEdge, texture_edge) : photo::kTurnedEdge;
        uint32_t requested = static_cast<uint32_t>(engine_settings().anti_aliasing);
        if (requested == 0)
            requested = 1;
        const TurnedFrameSampling frame = turned_frame_sampling();
        machine.edge_limit = turned_sampling_edge(
            field.width, field.height, cap, requested, frame.device_multisamples
        );
        const renderer_live_gpu::Sampling window_sampling = renderer_live_gpu::fitted_sampling(
            static_cast<uint32_t>(std::max(field.width, int32_t{0})),
            static_cast<uint32_t>(std::max(field.height, int32_t{0})),
            requested,
            frame.device_multisamples
        );
        machine.tile_limit = turned_tile_square(cap, window_sampling, frame.device_multisamples);
        // The card's own viewport, once a frame has drawn at the sampling the
        // tiles keep. A check's shorter limit is not part of the plan: the
        // picture drawn whole stays one piece.
        if (SDL_GPUDevice* card = turned_view_device(); card != nullptr && frame.supersample > 0) {
            const renderer_live_gpu::ViewportBounds bounds =
                renderer_live_gpu::viewport_bounds(card);
            const uint32_t capped =
                std::min(bounds.limit, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
            machine.viewport_limit = static_cast<int32_t>(capped);
            machine.viewport_samples = static_cast<int>(frame.supersample);
        }
    } else
        machine.edge_limit = texture_edge;

    machine.supersample = 1;
    machine.grain = 1;
    if (machine.kind == photo::CaptureKind::full && full_ != nullptr) {
        machine.supersample = static_cast<int>(std::max(uint32_t{1}, full_->supersample));
        machine.grain = static_cast<int>(supersampling::target_grain);
    }
    machine.bytes_per_texel = static_cast<int>(card::texel_bytes);
    machine.zoom = match_zoom();
    machine.shared = photo_ != nullptr && !photo_time_is_replay() && !photo_time_is_local();

    // A tilt-shift blurs a copy of the picture, with its sums beside it.
    if (photo_ != nullptr && photo_->look_tilt_radius > 0) {
        machine.look_bytes_per_pixel = 3;
        machine.look_bytes = photo::kLookScratchBytes;
    }
    const int32_t field_w = std::max(field.width, int32_t{0});
    const int32_t field_h = std::max(field.height, int32_t{0});
    const uint64_t screen_cpu =
        static_cast<uint64_t>(field_w) * static_cast<uint64_t>(field_h) *
            (uint64_t{6} + static_cast<uint64_t>(machine.look_bytes_per_pixel)) +
        machine.look_bytes;
    uint64_t cpu_room = ~uint64_t{0};
    uint64_t gpu_room = ~uint64_t{0};
    oa::platform::SystemMemorySample taken{};
    bool got_sample = false;
    // The plan is built on every tick and draw while the panel is open. The
    // system's memory is read again once a second, as the memory guard
    // reads it; the photos held and queued are counted on every plan.
    const uint64_t now_ns = frame_pacing::steady_now_ns();
    if (photo_ != nullptr && photo_->memory_read_ns != 0 && now_ns >= photo_->memory_read_ns &&
        now_ns - photo_->memory_read_ns < policy::memory_guard_sample_interval_ns) {
        taken = photo_->memory_read;
        got_sample = photo_->memory_read_ok;
    } else {
        got_sample = oa::platform::sample_system_memory(&taken);
        if (photo_ != nullptr) {
            photo_->memory_read = taken;
            photo_->memory_read_ok = got_sample;
            photo_->memory_read_ns = now_ns == 0 ? 1 : now_ns;
        }
    }
    const bool forced = render_run_ != nullptr && render_run_->forced_memory.has_value();
    const bool unlimited = !forced && (!got_sample || taken.physical == 0);
    if (!unlimited) {
        const oa::platform::SystemMemorySample& used = forced ? *render_run_->forced_memory : taken;
        const policy::MemoryGuard memory = render_run_ != nullptr && render_run_->watch
                                               ? render_run_->watch->memory
                                               : policy::make_memory_guard(used.physical);
        uint64_t held = remembered_photo_bytes();
        if (photo_writer_ != nullptr) {
            const uint64_t queued = photo_writer_->queued_bytes();
            held = held > ~uint64_t{0} - queued ? ~uint64_t{0} : held + queued;
        }
        cpu_room = largest_room([&](uint64_t bytes) {
            const uint64_t ask = held > ~uint64_t{0} - bytes ? ~uint64_t{0} : held + bytes;
            return policy::memory_guard_allows(memory, used, ask);
        });
        // Screen with smooth edges off is the picture already drawn. Keep it
        // offered. A larger size still has to pay for a new draw.
        if (cpu_room != ~uint64_t{0} && cpu_room < screen_cpu)
            cpu_room = screen_cpu;
        // One sample for the card too. The search does not read memory again.
        if (machine.kind == photo::CaptureKind::full) {
            gpu_room = largest_room([&](uint64_t bytes) {
                return policy::memory_guard_allows(memory, used, bytes);
            });
        }
    }
    machine.cpu_bytes = cpu_room;

    if (machine.kind == photo::CaptureKind::full) {
        uint64_t screen_gpu = static_cast<uint64_t>(field_w) * static_cast<uint64_t>(field_h) *
                              uint64_t{card::texel_bytes};
        if (machine.supersample > 1 && machine.zoom >= 1.0F) {
            const uint32_t grain = static_cast<uint32_t>(std::max(machine.grain, 1));
            const uint32_t target_w =
                supersampling::rounded_up(static_cast<uint32_t>(field_w), grain);
            const uint32_t target_h =
                supersampling::rounded_up(static_cast<uint32_t>(field_h), grain);
            const uint64_t world =
                policy::supersample_target_pixels(
                    target_w, target_h, static_cast<uint32_t>(machine.supersample)
                ) *
                uint64_t{card::texel_bytes};
            screen_gpu = screen_gpu > ~uint64_t{0} - world ? ~uint64_t{0} : screen_gpu + world;
        }
        if (gpu_room != ~uint64_t{0} && gpu_room < screen_gpu)
            gpu_room = screen_gpu;
    }
    machine.gpu_bytes = gpu_room;
    machine.tile_edge = std::max(photo_check_tile_edge_, int32_t{0});
    if (photo_ != nullptr)
        machine.depth_of_field = std::clamp(photo_->look_depth_of_field, 0, 32);
    return machine;
}

void Runtime::set_photo_size_factor(int factor) {
    if (photo_ == nullptr)
        return;
    const photo::Machine machine = photo_capture_machine();
    const photo::Plan plan = photo_size_plan(machine);
    for (const photo::Offer& offer : plan.sizes) {
        if (offer.factor == factor && offer.offered) {
            photo_->size_factor = factor;
            photo_->poster = false;
            photo_->poster_marking = false;
            snap_photo_factors(machine);
            return;
        }
    }
}

void Runtime::set_photo_smooth_factor(int factor) {
    if (photo_ == nullptr)
        return;
    const photo::Machine machine = photo_capture_machine();
    const photo::Plan plan = photo_size_plan(machine);
    for (const photo::Offer& offer : plan.smooth) {
        if (offer.factor == factor && offer.offered) {
            photo_->smooth_factor = factor;
            snap_photo_factors(machine);
            return;
        }
    }
}

void Runtime::snap_photo_factors() {
    if (photo_ == nullptr)
        return;
    snap_photo_factors(photo_capture_machine());
}

void Runtime::snap_photo_factors(const photo::Machine& machine) {
    if (photo_ == nullptr)
        return;
    for (int pass = 0; pass < 2; ++pass) {
        const photo::Plan at_size = photo_size_plan(machine);
        photo_->size_factor = kept_offer(photo_->size_factor, at_size.sizes, 4);
        const photo::Plan at_smooth = photo_size_plan(machine);
        photo_->smooth_factor = kept_offer(photo_->smooth_factor, at_smooth.smooth, 3);
    }
    if (!photo_->poster)
        return;
    const photo::PosterPlan poster = photo_poster_plan(machine);
    if (poster.button != photo::SizeLock::open) {
        photo_->poster = false;
        photo_->poster_marking = false;
        return;
    }
    const int current = std::clamp(static_cast<int>(photo_->poster_extent), 0, 2);
    const photo::PosterOffer* extents[3] = {&poster.view, &poster.marked, &poster.map};
    if (extents[current]->offered)
        return;
    for (int index = 0; index < 3; ++index) {
        if (!extents[index]->offered)
            continue;
        photo_->poster_extent = static_cast<uint8_t>(index);
        return;
    }
    photo_->poster = false;
    photo_->poster_marking = false;
}

photo::Plan Runtime::photo_size_plan() {
    return photo_size_plan(photo_capture_machine());
}

photo::Plan Runtime::photo_size_plan(const photo::Machine& machine) {
    photo::Field field;
    if (match_) {
        field.width = match_layout_.battlefield_width();
        field.height = match_layout_.battlefield_height();
    }
    const int size_factor = photo_ != nullptr ? photo_->size_factor : 1;
    const int smooth = photo_ != nullptr ? photo_->smooth_factor : 1;
    return photo::plan_sizes(field, machine, size_factor, smooth);
}

std::string Runtime::photo_offer_reason(photo::CaptureKind kind, const photo::Offer& offer) const {
    if (offer.offered || offer.lock == photo::SizeLock::open)
        return {};
    if (offer.lock == photo::SizeLock::pixels)
        return std::string(oa::data::languages::interface_text(words::whole_pixels));
    if (offer.lock == photo::SizeLock::shared)
        return std::string(oa::data::languages::interface_text(words::shared_game));
    if (offer.lock == photo::SizeLock::memory)
        return std::string(oa::data::languages::interface_text(words::not_enough_memory));
    const std::string_view key = kind == photo::CaptureKind::full     ? words::card_edge
                                 : kind == photo::CaptureKind::turned ? words::turned_edge
                                                                      : words::photo_edge;
    return fill_limit(oa::data::languages::interface_text(key), offer.limit);
}

bool Runtime::draw_processor_photo(
    std::vector<uint8_t>& rgb, int32_t draw_width, int32_t draw_height, double zoom
) {
    rgb.clear();
    if (!match_ || !selected_tnt_ || draw_width <= 0 || draw_height <= 0 ||
        draw_width > photo::kProcessorEdge || draw_height > photo::kProcessorEdge ||
        !(zoom > 0.0) || !std::isfinite(zoom))
        return false;
    // The director's camera starts on the map. A window looking past the
    // left or top edge would then be framed from that edge, and a larger
    // photo would show another view than the preview. Draw that photo the
    // way the window draws it, margin included. A camera on the map stays
    // on the director's frame, which those photos already match.
    if (match_camera_x_ < 0 || match_camera_z_ < 0) {
        try {
            return restoring_photo_window([&] {
                match_layout_ =
                    oa::ui::display_layout::make_battlefield_layout(draw_width, draw_height);
                const float photo_zoom = static_cast<float>(zoom);
                match_zoom_ = photo_zoom;
                match_zoom_target_ = photo_zoom;
                if (!draw_match_surface())
                    return false;
                if (match_world_cpu_.width != static_cast<uint32_t>(draw_width) ||
                    match_world_cpu_.height != static_cast<uint32_t>(draw_height))
                    return false;
                const std::size_t pixels =
                    static_cast<std::size_t>(draw_width) * static_cast<std::size_t>(draw_height);
                if (match_world_cpu_.rgb.size() != pixels * 3U)
                    return false;
                rgb = match_world_cpu_.rgb;
                apply_gamma_rgb(rgb.data(), pixels, 3);
                return true;
            });
        } catch (const std::exception&) {
            rgb.clear();
            return false;
        }
    }
    const auto fraction = view_offset();
    const oa::media::director::EngineView shot = engine_shot_of(
        match_camera_x_, match_camera_z_, fraction.x, fraction.y, draw_width, draw_height, zoom
    );
    try {
        return restoring_photo_window([&] {
            DirectorPresentation presentation;
            presentation.width = static_cast<uint32_t>(draw_width);
            presentation.height = static_cast<uint32_t>(draw_height);
            presentation.show_interface = false;
            presentation.every_player_speaks = false;
            presentation.full = false;
            presentation.player_gamma = true;
            enter_director_mode(presentation, {});
            set_director_view(shot);
            rgb.assign(
                static_cast<std::size_t>(draw_width) * static_cast<std::size_t>(draw_height) * 3U, 0
            );
            draw_director_frame(rgb);
            leave_director_mode();
            return true;
        });
    } catch (const std::exception&) {
        rgb.clear();
        return false;
    }
}

bool Runtime::draw_full_rgb(
    std::vector<uint8_t>& drawn,
    int32_t draw_width,
    int32_t draw_height,
    float photo_zoom,
    const photo::Machine& machine
) {
    if (full_ == nullptr || sdl_.renderer == nullptr)
        return false;
    photo::TileRequest tile_request;
    tile_request.width = draw_width;
    tile_request.height = draw_height;
    tile_request.zoom = photo_zoom;
    tile_request.supersample = machine.supersample;
    tile_request.grain = machine.grain;
    tile_request.texture_limit = machine.edge_limit;
    tile_request.tile_edge = machine.tile_edge;
    tile_request.reach = photo::full_tile_reach(photo_zoom);
    const photo::TilePlan tile_plan = photo::plan_tiles(tile_request);
    if (!tile_plan.fits)
        return false;
    // A shared match never draws tiles.
    if (machine.shared && !photo::draws_whole(tile_plan, draw_width, draw_height))
        return false;
    // A photo the card can draw in one piece stays on that path,
    // including its camera, so its pixels stay as they are.
    if (photo::draws_whole(tile_plan, draw_width, draw_height)) {
        if (!draw_match_surface())
            return false;
        if (match_world_cpu_.width != static_cast<uint32_t>(draw_width) ||
            match_world_cpu_.height != static_cast<uint32_t>(draw_height))
            return false;
        const uint32_t factor = full_->supersample;
        unit_supersampling_ = level_for(factor);
        accelerated_.reading_back = true;
        TextureHold target;
        target.texture = SDL_CreateTexture(
            sdl_.renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_TARGET,
            draw_width,
            draw_height
        );
        if (target.texture == nullptr)
            return false;

        // The pointer is put back before the texture is destroyed.
        struct TargetPin {
            Runtime& self;
            FullPresentation* full{nullptr};
            SDL_Texture* previous{};

            ~TargetPin() {
                if (full != nullptr && self.full_.get() == full)
                    full->final_target = previous;
            }
        } pin{*this, full_.get(), full_->final_target};

        {
            const RenderState state(sdl_.renderer);
            state.use_target_pixels(target.texture);
            full_->final_target = target.texture;
            bool presented = false;
            try {
                presented = present_full_match_layers(false);
            } catch (const std::exception&) {
                return false;
            }
            if (!presented || full_ == nullptr || !full_->drawn || full_->supersample != factor)
                return false;
            if (!SDL_SetRenderTarget(sdl_.renderer, target.texture))
                return false;
            const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> read(
                SDL_RenderReadPixels(sdl_.renderer, nullptr), SDL_DestroySurface
            );
            const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> frame(
                read ? SDL_ConvertSurface(read.get(), SDL_PIXELFORMAT_RGB24) : nullptr,
                SDL_DestroySurface
            );
            if (!take_rgb(frame.get(), draw_width, draw_height, drawn))
                return false;
        }
        return true;
    }

    // The row bias is the tile's place in the photo. It is cleared
    // before the window's own frame is drawn again.
    struct TileRowReset {
        Runtime& self;

        explicit TileRowReset(Runtime& runtime) : self(runtime) {}

        ~TileRowReset() {
            self.photo_tile_row_ = 0;
            self.photo_tile_anchor_x_ = 0;
            self.photo_tile_map_offset_.reset();
        }
    } tile_row_reset{*this};

    const int32_t camera_column = match_camera_x_;
    const int32_t camera_row = match_camera_z_;
    drawn.assign(
        static_cast<std::size_t>(draw_width) * static_cast<std::size_t>(draw_height) * 3U, 0
    );
    const auto draw_piece = [&](const photo::Tile& tile, std::vector<uint8_t>& piece) {
        if (tile.map_column > 0 &&
            camera_column > std::numeric_limits<int32_t>::max() - tile.map_column)
            return false;
        if (tile.map_row > 0 && camera_row > std::numeric_limits<int32_t>::max() - tile.map_row)
            return false;
        photo_tile_row_ = tile.draw_y;
        photo_tile_anchor_x_ = tile.draw_x;
        photo_tile_map_offset_ = std::array<int32_t, 2>{tile.map_column, tile.map_row};
        match_layout_ =
            oa::ui::display_layout::make_battlefield_layout(tile.draw_width, tile.draw_height);
        match_zoom_ = photo_zoom;
        match_zoom_target_ = photo_zoom;
        exact_view_.held = false;
        match_camera_x_ = camera_column + tile.map_column;
        match_camera_z_ = camera_row + tile.map_row;
        if (!draw_match_surface())
            return false;
        if (match_world_cpu_.width != static_cast<uint32_t>(tile.draw_width) ||
            match_world_cpu_.height != static_cast<uint32_t>(tile.draw_height))
            return false;
        const uint32_t factor = full_->supersample;
        unit_supersampling_ = level_for(factor);
        accelerated_.reading_back = true;
        TextureHold target;
        target.texture = SDL_CreateTexture(
            sdl_.renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_TARGET,
            tile.draw_width,
            tile.draw_height
        );
        if (target.texture == nullptr)
            return false;
        struct TargetPin {
            Runtime& self;
            FullPresentation* full{nullptr};
            SDL_Texture* previous{};

            ~TargetPin() {
                if (full != nullptr && self.full_.get() == full)
                    full->final_target = previous;
            }
        } pin{*this, full_.get(), full_->final_target};
        {
            const RenderState state(sdl_.renderer);
            state.use_target_pixels(target.texture);
            full_->final_target = target.texture;
            bool presented = false;
            try {
                presented = present_full_match_layers(false);
            } catch (const std::exception&) {
                return false;
            }
            if (!presented || full_ == nullptr || !full_->drawn || full_->supersample != factor)
                return false;
            if (!SDL_SetRenderTarget(sdl_.renderer, target.texture))
                return false;
            const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> read(
                SDL_RenderReadPixels(sdl_.renderer, nullptr), SDL_DestroySurface
            );
            const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> frame(
                read ? SDL_ConvertSurface(read.get(), SDL_PIXELFORMAT_RGB24) : nullptr,
                SDL_DestroySurface
            );
            if (!take_rgb(frame.get(), tile.draw_width, tile.draw_height, piece))
                return false;
        }
        return true;
    };
    for (const photo::Tile& tile : tile_plan.tiles) {
        std::vector<uint8_t> piece;
        if (!draw_piece(tile, piece))
            return false;
        if (tile.x < 0 || tile.y < 0 || tile.width <= 0 || tile.height <= 0 ||
            tile.x > draw_width - tile.width || tile.y > draw_height - tile.height ||
            tile.border_left > tile.draw_width - tile.width ||
            tile.border_top > tile.draw_height - tile.height)
            return false;
        const std::size_t piece_bytes = static_cast<std::size_t>(tile.draw_width) *
                                        static_cast<std::size_t>(tile.draw_height) * 3U;
        if (piece.size() != piece_bytes)
            return false;
        for (int32_t kept_row = 0; kept_row < tile.height; ++kept_row) {
            const std::size_t from = (static_cast<std::size_t>(kept_row + tile.border_top) *
                                          static_cast<std::size_t>(tile.draw_width) +
                                      static_cast<std::size_t>(tile.border_left)) *
                                     3U;
            const std::size_t to = (static_cast<std::size_t>(tile.y + kept_row) *
                                        static_cast<std::size_t>(draw_width) +
                                    static_cast<std::size_t>(tile.x)) *
                                   3U;
            std::memcpy(
                drawn.data() + to, piece.data() + from, static_cast<std::size_t>(tile.width) * 3U
            );
        }
    }
    return true;
}

bool Runtime::capture_sized_photo(std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
    rgb.clear();
    width = 0;
    height = 0;
    if (photo_ == nullptr || !match_ || !selected_tnt_)
        return false;
    photo::Field field;
    field.width = match_layout_.battlefield_width();
    field.height = match_layout_.battlefield_height();
    const photo::Machine machine = photo_capture_machine();
    const photo::Draw draw =
        photo::plan_draw(field, machine, photo_->size_factor, photo_->smooth_factor);
    if (!draw.offered || draw.draw_width <= 0 || draw.draw_height <= 0)
        return false;

    const float window_zoom = match_zoom_ > 0.0F ? match_zoom_ : 1.0F;
    const double scale = static_cast<double>(draw.size_factor) * static_cast<double>(draw.smooth);
    const float photo_zoom = static_cast<float>(static_cast<double>(window_zoom) * scale);
    std::vector<uint8_t> drawn;
    renderer_live_gpu::Sampling play_sampling{};
    if (machine.kind == photo::CaptureKind::turned) {
        uint32_t requested = static_cast<uint32_t>(engine_settings().anti_aliasing);
        if (requested == 0)
            requested = 1;
        play_sampling = renderer_live_gpu::fitted_sampling(
            static_cast<uint32_t>(std::max(field.width, int32_t{0})),
            static_cast<uint32_t>(std::max(field.height, int32_t{0})),
            requested,
            turned_frame_sampling().device_multisamples
        );
    }

    if (machine.kind == photo::CaptureKind::processor) {
        if (!draw_processor_photo(drawn, draw.draw_width, draw.draw_height, window_zoom * scale))
            return false;
        return commit_picture(draw, drawn, rgb, width, height);
    }

    const bool drew = [&] {
        try {
            return restoring_photo_window([&] {
                match_layout_ = oa::ui::display_layout::make_battlefield_layout(
                    draw.draw_width, draw.draw_height
                );
                match_zoom_ = photo_zoom;
                match_zoom_target_ = photo_zoom;
                exact_view_.held = false;
                if (machine.kind == photo::CaptureKind::turned) {
                    photo::TurnedTileRequest tile_request;
                    tile_request.width = draw.draw_width;
                    tile_request.height = draw.draw_height;
                    tile_request.zoom = photo_zoom;
                    tile_request.draw_limit = photo::turned_tile_draw_limit(machine);
                    tile_request.tile_edge = machine.shared ? 0 : machine.tile_edge;
                    const int blur_times = std::max(draw.size_factor, 1) * std::max(draw.smooth, 1);
                    tile_request.blur = photo::photo_tilt_radius(
                        std::clamp(machine.depth_of_field, 0, 32), blur_times
                    );
                    const photo::TilePlan tile_plan = photo::plan_turned_tiles(tile_request);
                    if (!tile_plan.fits)
                        return false;
                    if (machine.shared &&
                        !photo::draws_whole(tile_plan, draw.draw_width, draw.draw_height))
                        return false;
                    // Pack from a fresh scene, and put the window's back
                    // before its frame is drawn again.
                    struct IsolateLiveScene {
                        Runtime& self;

                        explicit IsolateLiveScene(Runtime& runtime) : self(runtime) {
                            self.photo_isolate_scene_ = true;
                        }

                        ~IsolateLiveScene() { self.photo_isolate_scene_ = false; }
                    } isolate_live_scene{*this};
                    // A photo one frame can draw stays on that path, so its
                    // pixels stay as they are.
                    if (photo::draws_whole(tile_plan, draw.draw_width, draw.draw_height)) {
                        photo_capture_live_ = true;
                        if (!draw_match_surface() || live_renderer_failed())
                            return false;
                        if (match_world_cpu_.width != static_cast<uint32_t>(draw.draw_width) ||
                            match_world_cpu_.height != static_cast<uint32_t>(draw.draw_height))
                            return false;
                        const std::size_t pixels = static_cast<std::size_t>(draw.draw_width) *
                                                   static_cast<std::size_t>(draw.draw_height);
                        if (match_world_cpu_.rgb.size() != pixels * 3U)
                            return false;
                        drawn = match_world_cpu_.rgb;
                        apply_gamma_rgb(drawn.data(), pixels, 3);
                        const TurnedFrameSampling got = turned_frame_sampling();
                        if (play_sampling.supersample == 0 || got.supersample == 0 ||
                            got.multisample == 0 || got.supersample < play_sampling.supersample ||
                            got.multisample < play_sampling.multisample)
                            return false;
                        return true;
                    }
                    // The tile's shift is cleared before the window's own
                    // frame is drawn again.
                    struct TurnedTileReset {
                        Runtime& self;

                        explicit TurnedTileReset(Runtime& runtime) : self(runtime) {}

                        ~TurnedTileReset() {
                            self.photo_tile_whole_width_ = 0;
                            self.photo_tile_whole_height_ = 0;
                            self.photo_tile_shift_x_ = 0;
                            self.photo_tile_shift_y_ = 0;
                            self.photo_tile_hold_scene_ = false;
                            self.photo_tile_frame_ = false;
                            self.photo_tile_supersample_ = 0;
                        }
                    } turned_tile_reset{*this};
                    const bool device_multisamples = turned_frame_sampling().device_multisamples;
                    drawn.assign(
                        static_cast<std::size_t>(draw.draw_width) *
                            static_cast<std::size_t>(draw.draw_height) * 3U,
                        0
                    );
                    bool hold_scene = false;
                    for (const photo::Tile& tile : tile_plan.tiles) {
                        if (tile.draw_width <= 0 || tile.draw_height <= 0)
                            return false;
                        const uint32_t ask = sampling_request_for(
                            static_cast<uint32_t>(tile.draw_width),
                            static_cast<uint32_t>(tile.draw_height),
                            play_sampling,
                            device_multisamples
                        );
                        if (ask == 0)
                            return false;
                        match_layout_ = oa::ui::display_layout::make_battlefield_layout(
                            tile.draw_width, tile.draw_height
                        );
                        match_zoom_ = photo_zoom;
                        match_zoom_target_ = photo_zoom;
                        exact_view_.held = false;
                        photo_capture_live_ = true;
                        photo_tile_frame_ = true;
                        photo_tile_whole_width_ = draw.draw_width;
                        photo_tile_whole_height_ = draw.draw_height;
                        photo_tile_shift_x_ = tile.draw_x;
                        photo_tile_shift_y_ = tile.draw_y;
                        photo_tile_hold_scene_ = hold_scene;
                        photo_tile_supersample_ = ask;
                        if (!draw_match_surface() || live_renderer_failed())
                            return false;
                        if (match_world_cpu_.width != static_cast<uint32_t>(tile.draw_width) ||
                            match_world_cpu_.height != static_cast<uint32_t>(tile.draw_height))
                            return false;
                        const std::size_t piece_pixels = static_cast<std::size_t>(tile.draw_width) *
                                                         static_cast<std::size_t>(tile.draw_height);
                        if (match_world_cpu_.rgb.size() != piece_pixels * 3U)
                            return false;
                        const TurnedFrameSampling got = turned_frame_sampling();
                        if (got.supersample != play_sampling.supersample ||
                            got.multisample != play_sampling.multisample)
                            return false;
                        if (tile.x < 0 || tile.y < 0 || tile.width <= 0 || tile.height <= 0 ||
                            tile.x > draw.draw_width - tile.width ||
                            tile.y > draw.draw_height - tile.height ||
                            tile.border_left > tile.draw_width - tile.width ||
                            tile.border_top > tile.draw_height - tile.height)
                            return false;
                        for (int32_t kept_row = 0; kept_row < tile.height; ++kept_row) {
                            const std::size_t from =
                                (static_cast<std::size_t>(kept_row + tile.border_top) *
                                     static_cast<std::size_t>(tile.draw_width) +
                                 static_cast<std::size_t>(tile.border_left)) *
                                3U;
                            const std::size_t to = (static_cast<std::size_t>(tile.y + kept_row) *
                                                        static_cast<std::size_t>(draw.draw_width) +
                                                    static_cast<std::size_t>(tile.x)) *
                                                   3U;
                            std::memcpy(
                                drawn.data() + to,
                                match_world_cpu_.rgb.data() + from,
                                static_cast<std::size_t>(tile.width) * 3U
                            );
                        }
                        hold_scene = true;
                    }
                    const std::size_t pixels = static_cast<std::size_t>(draw.draw_width) *
                                               static_cast<std::size_t>(draw.draw_height);
                    apply_gamma_rgb(drawn.data(), pixels, 3);
                    return true;
                }
                return draw_full_rgb(drawn, draw.draw_width, draw.draw_height, photo_zoom, machine);
            });
        } catch (const std::exception&) {
            return false;
        }
    }();
    if (!drew || !commit_picture(draw, drawn, rgb, width, height))
        return false;
    if (machine.kind == photo::CaptureKind::turned)
        photo_->sized_by_live = true;
    else {
        photo_->sized_by_full = true;
        photo_->sized_supersample = full_ != nullptr ? full_->supersample : uint32_t{1};
    }
    return true;
}

} // namespace oa::app
