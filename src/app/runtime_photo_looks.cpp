// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A photo's look on the preview and on the picture that is saved. The
// filter runs after the player's gamma. With no look on, nothing here runs.
#include "oa/app/runtime.hpp"

#include "full_effects.hpp"
#include "photo_state.hpp"

#include "oa/platform/text_font.hpp"
#include "oa/present/photo/looks.hpp"
#include "oa/ui/photo_panel/crop.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace photo = oa::present::photo;

/// Two digits, or more when the number needs them. A negative value is 00.
std::string two_digits(int value) {
    if (value < 0)
        value = 0;
    std::string text = std::to_string(value);
    if (text.size() < 2)
        text.insert(0, 2 - text.size(), '0');
    return text;
}

/// Four digits for a year. A negative year is 0000.
std::string four_digits(int value) {
    if (value < 0)
        value = 0;
    std::string text = std::to_string(value);
    if (text.size() < 4)
        text.insert(0, 4 - text.size(), '0');
    return text;
}

/// The local date as YYYY-MM-DD. The filter draws this text; it does not
/// format a date of its own.
std::string iso_date() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    return four_digits(parts.tm_year + 1900) + "-" + two_digits(parts.tm_mon + 1) + "-" +
           two_digits(parts.tm_mday);
}

/// The game clock as hh:mm:ss. Thirty ticks are one second.
std::string clock_text(uint32_t tick) {
    const uint32_t seconds = tick / 30U;
    const auto two = [](uint32_t part) {
        const std::string digits = std::to_string(part);
        return part < 10U ? "0" + digits : digits;
    };
    return two(seconds / 3600U) + ":" + two(seconds / 60U % 60U) + ":" + two(seconds % 60U);
}

/// The faces a caption bar draws with. One stack for the process, opened
/// from the faces that ship with the engine. Null when they did not open:
/// the bar is still painted, with no letters.
oa::platform::text_font::FontStack* caption_fonts() {
    static std::unique_ptr<oa::platform::text_font::FontStack> stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    return stack.get();
}

/// Mixes one value into a preview key.
void mix_key(uint64_t& hash, uint64_t value) noexcept {
    hash ^= value + 0x9E3779B97F4A7C15ULL + (hash << 6U) + (hash >> 2U);
}

/// The bits of a float, so a zoom or an angle can join the preview key.
uint32_t float_bits(float value) noexcept {
    static_assert(sizeof(uint32_t) == sizeof(float));
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// A look built from the visit. The caption names the map, the clock and
/// the date, joined with a middle dot. An idle look is still built: the
/// caller decides whether to paint it. The fields are passed one by one
/// because the visit's state is private to Runtime.
photo::Look make_look(
    uint8_t preset_index,
    int32_t vignette,
    int32_t roundness,
    int32_t grain,
    int32_t grain_size,
    uint8_t frame_index,
    uint32_t seed,
    const std::string& map,
    uint32_t tick
) {
    photo::Look look;
    const int preset = std::clamp(static_cast<int>(preset_index), 0, 5);
    const int frame = std::clamp(static_cast<int>(frame_index), 0, 3);
    look.preset = static_cast<photo::Preset>(preset);
    look.vignette = std::clamp(vignette, 0, 100);
    look.roundness = std::clamp(roundness, 0, 100);
    look.grain = std::clamp(grain, 0, 100);
    look.grain_size = std::max(grain_size, 1);
    look.seed = seed;
    look.frame = static_cast<photo::Frame>(frame);
    if (look.frame != photo::Frame::caption)
        return look;
    const std::string clock = clock_text(tick);
    const std::string date = iso_date();
    if (!map.empty())
        look.caption = map + " \xC2\xB7 " + clock + " \xC2\xB7 " + date;
    else
        look.caption = clock + " \xC2\xB7 " + date;
    return look;
}

/// True when a render scale is one, within a hundredth.
bool scale_is_one(float scale) noexcept {
    const float delta = scale - 1.0F;
    return delta < 0.01F && delta > -0.01F;
}

/// Copies RGBA rows into a packed buffer. The pitch may be wider than the row.
void pack_rgba(
    std::vector<uint8_t>& packed,
    const uint8_t* bytes,
    uint32_t width,
    uint32_t height,
    uint32_t pitch
) {
    const std::size_t row = static_cast<std::size_t>(width) * 4U;
    packed.resize(row * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(
            packed.data() + static_cast<std::size_t>(y) * row,
            bytes + static_cast<std::size_t>(y) * pitch,
            row
        );
    }
}

} // namespace

bool Runtime::photo_look_active() const noexcept {
    if (!photo_mode() || photo_ == nullptr)
        return false;
    return photo_->look_preset != 0 || photo_->look_vignette > 0 || photo_->look_grain > 0 ||
           photo_->look_frame != 0;
}

uint64_t Runtime::photo_look_key() const {
    uint64_t hash = 0x6C6F6F6BULL;
    if (photo_ == nullptr)
        return hash;
    const bool held = photo_->picture_held && photo_->camera_held;
    const uint32_t tick =
        held ? photo_->held_tick : (match_ != nullptr ? match_->state().game.tick : 0U);
    mix_key(hash, photo_->look_preset);
    mix_key(hash, static_cast<uint64_t>(photo_->look_vignette));
    mix_key(hash, static_cast<uint64_t>(photo_->look_roundness));
    mix_key(hash, static_cast<uint64_t>(photo_->look_grain));
    mix_key(hash, static_cast<uint64_t>(photo_->look_grain_size));
    mix_key(hash, photo_->look_frame);
    mix_key(hash, tick);
    mix_key(hash, static_cast<uint64_t>(photo_->shutter_eighth));
    mix_key(hash, photo_->show_selection ? 1U : 0U);
    mix_key(hash, photo_->show_health_bars ? 1U : 0U);
    mix_key(hash, held ? 1U : 0U);
    // A crop moves the look to the crop, so a new guide filters again.
    mix_key(hash, photo_->guide);
    const int32_t camera_x = held ? photo_->held_camera_x : match_camera_x_;
    const int32_t camera_z = held ? photo_->held_camera_z : match_camera_z_;
    const float zoom = held ? photo_->held_zoom : match_zoom_;
    mix_key(hash, static_cast<uint64_t>(static_cast<uint32_t>(camera_x)));
    mix_key(hash, static_cast<uint64_t>(static_cast<uint32_t>(camera_z)));
    mix_key(hash, float_bits(zoom));
    if (live_renderer_active() || held) {
        const float yaw = held ? photo_->held_live_yaw : live_camera_angles()[0];
        const float pitch = held ? photo_->held_live_pitch : live_camera_angles()[1];
        mix_key(hash, float_bits(yaw));
        mix_key(hash, float_bits(pitch));
    }
    const uint32_t width = held ? photo_->held_width : match_world_cpu_.width;
    const uint32_t height = held ? photo_->held_height : match_world_cpu_.height;
    mix_key(hash, width);
    mix_key(hash, height);
    if (photo_->look_frame != 0) {
        const photo::Look look = make_look(
            photo_->look_preset,
            photo_->look_vignette,
            photo_->look_roundness,
            photo_->look_grain,
            photo_->look_grain_size,
            photo_->look_frame,
            tick,
            selected_map_name_runtime_,
            tick
        );
        for (const unsigned char byte : look.caption)
            mix_key(hash, byte);
    }
    return hash;
}

bool Runtime::photo_look_preview_due() const {
    if (!photo_look_active() || photo_ == nullptr)
        return false;
    if (!photo_look_key_set_ || photo_look_key_ != photo_look_key())
        return true;
    const bool still = photo_->picture_held || photo_->time == PhotoState::Time::frozen;
    return !still;
}

void Runtime::apply_photo_look_rgb(
    std::vector<uint8_t>& rgb, uint32_t width, uint32_t height, int size_factor
) const {
    if (!photo_look_active() || photo_ == nullptr || width == 0 || height == 0)
        return;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (rgb.size() < pixels * 3U)
        return;
    if (width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return;
    const bool held = photo_->picture_held;
    const uint32_t tick =
        held ? photo_->held_tick : (match_ != nullptr ? match_->state().game.tick : 0U);
    const photo::Look look = make_look(
        photo_->look_preset,
        photo_->look_vignette,
        photo_->look_roundness,
        photo_->look_grain,
        photo::photo_grain_size(photo_->look_grain_size, size_factor),
        photo_->look_frame,
        tick,
        selected_map_name_runtime_,
        tick
    );
    photo::Picture picture;
    picture.bytes = rgb.data();
    picture.width = static_cast<int32_t>(width);
    picture.height = static_cast<int32_t>(height);
    picture.channels = 3;
    photo::apply_look(picture, look, caption_fonts(), draw_pool_.get());
}

void Runtime::paint_photo_look_preview(
    uint8_t* bytes, uint32_t width, uint32_t height, uint32_t stride, uint32_t channels
) const {
    if (!photo_look_active() || photo_ == nullptr || bytes == nullptr || width == 0 || height == 0)
        return;
    constexpr auto most = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    if (width > most || height > most || stride > most || (channels != 3U && channels != 4U))
        return;
    const bool held = photo_->picture_held;
    const uint32_t tick =
        held ? photo_->held_tick : (match_ != nullptr ? match_->state().game.tick : 0U);
    const photo::Look look = make_look(
        photo_->look_preset,
        photo_->look_vignette,
        photo_->look_roundness,
        photo_->look_grain,
        photo_->look_grain_size,
        photo_->look_frame,
        tick,
        selected_map_name_runtime_,
        tick
    );
    photo::Picture picture;
    picture.bytes = bytes;
    picture.width = static_cast<int32_t>(width);
    picture.height = static_cast<int32_t>(height);
    picture.stride = static_cast<int32_t>(stride);
    picture.channels = static_cast<int32_t>(channels);
    // The photo is cropped before its look is painted, so the look goes on
    // the crop's own rectangle here too.
    const auto guide = static_cast<oa::ui::photo_panel::Guide>(photo_->guide);
    const oa::ui::kit::Rect crop =
        oa::ui::photo_panel::crop_bounds(picture.width, picture.height, guide);
    if (crop.width > 0 && crop.height > 0) {
        picture.bytes = bytes + static_cast<std::size_t>(crop.y) * stride +
                        static_cast<std::size_t>(crop.x) * channels;
        picture.width = crop.width;
        picture.height = crop.height;
    }
    photo::apply_look(picture, look, caption_fonts(), draw_pool_.get());
}

bool Runtime::refresh_photo_look_cpu() {
    if (!photo_look_active() || photo_ == nullptr)
        return false;
    const uint64_t key = photo_look_key();
    if (photo_look_cpu_valid_ && photo_look_key_set_ && photo_look_key_ == key &&
        !photo_look_preview_due())
        return !photo_look_cpu_.rgb.empty();

    std::vector<uint8_t> rgb;
    uint32_t width = 0;
    uint32_t height = 0;
    if (photo_picture_held()) {
        width = photo_->held_width;
        height = photo_->held_height;
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        if (width == 0 || height == 0 || photo_->held_rgb.size() != pixels * 3U)
            return false;
        rgb = photo_->held_rgb;
        apply_gamma_rgb(rgb.data(), pixels, 3);
    } else if (!copy_world_with_player_gamma(rgb, width, height)) {
        return false;
    }
    if (rgb.size() < static_cast<std::size_t>(width) * height * 3U)
        return false;
    paint_photo_look_preview(rgb.data(), width, height, width * 3U, 3U);
    photo_look_cpu_.width = width;
    photo_look_cpu_.height = height;
    photo_look_cpu_.rgb = std::move(rgb);
    photo_look_key_ = key;
    photo_look_key_set_ = true;
    photo_look_cpu_valid_ = true;
    return !photo_look_cpu_.rgb.empty();
}

void Runtime::present_photo_look_full(
    int x, int y, int width, int height, bool moved, SDL_Texture* target
) {
    if (!photo_look_active() || sdl_.renderer == nullptr || width <= 0 || height <= 0)
        return;
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    int logical_width = 0;
    int logical_height = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    const bool scales =
        SDL_GetRenderScale(sdl_.renderer, &scale_x, &scale_y) &&
        SDL_GetRenderLogicalPresentation(sdl_.renderer, &logical_width, &logical_height, &mode);
    const bool one_to_one = !moved && scales && scale_is_one(scale_x) && scale_is_one(scale_y) &&
                            mode == SDL_LOGICAL_PRESENTATION_DISABLED;
    const uint64_t key = photo_look_key();
    const bool cached = one_to_one && photo_look_rgba_valid_ && photo_look_key_set_ &&
                        photo_look_key_ == key && !photo_look_preview_due() &&
                        photo_look_rgba_width_ == static_cast<uint32_t>(width) &&
                        photo_look_rgba_height_ == static_cast<uint32_t>(height) &&
                        photo_look_rgba_.size() ==
                            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    if (cached) {
        SDL_Texture* texture = SDL_CreateTexture(
            sdl_.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height
        );
        if (texture != nullptr) {
            const bool drawn =
                SDL_UpdateTexture(texture, nullptr, photo_look_rgba_.data(), width * 4) &&
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) &&
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
            if (drawn) {
                const SDL_FRect destination{
                    static_cast<float>(x),
                    static_cast<float>(y),
                    static_cast<float>(width),
                    static_cast<float>(height)
                };
                if (SDL_RenderTexture(sdl_.renderer, texture, nullptr, &destination)) {
                    SDL_DestroyTexture(texture);
                    return;
                }
            }
            SDL_DestroyTexture(texture);
        }
    }

    const auto process = [this, width, height, one_to_one](const full_effects::Pixels& image) {
        if (image.bytes.empty() || image.width == 0 || image.height == 0)
            return;
        if (image.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            image.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            return;
        paint_photo_look_preview(image.bytes.data(), image.width, image.height, image.pitch, 4U);
        if (!one_to_one || image.width != static_cast<uint32_t>(width) ||
            image.height != static_cast<uint32_t>(height)) {
            photo_look_rgba_valid_ = false;
            return;
        }
        pack_rgba(photo_look_rgba_, image.bytes.data(), image.width, image.height, image.pitch);
        photo_look_rgba_width_ = image.width;
        photo_look_rgba_height_ = image.height;
        photo_look_rgba_valid_ = true;
    };
    full_effects::Upload upload;
    const std::string failure = full_effects::compose(
        sdl_.renderer,
        upload,
        SDL_Rect{x, y, width, height},
        nullptr,
        1U,
        process,
        nullptr,
        [this](uint64_t bytes) { return accelerated_buffer_fits(bytes); },
        target,
        full_effects::Pass::world
    );
    if (failure.empty()) {
        photo_look_key_ = key;
        photo_look_key_set_ = true;
    } else {
        photo_look_rgba_valid_ = false;
    }
}

void Runtime::clear_photo_look_cache() noexcept {
    photo_look_cpu_valid_ = false;
    photo_look_key_set_ = false;
    photo_look_rgba_valid_ = false;
    photo_look_rgba_width_ = 0;
    photo_look_rgba_height_ = 0;
    photo_look_cpu_.rgb.clear();
    photo_look_cpu_.width = 0;
    photo_look_cpu_.height = 0;
    photo_look_rgba_.clear();
}

} // namespace oa::app
