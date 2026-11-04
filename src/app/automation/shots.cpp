// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pictures for the MCP server (include/oa/app/automation/shots.hpp).
#include "oa/app/automation/shots.hpp"

#include "frames.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/frame_coordinates.hpp"
#include "oa/formats/png.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <SDL3/SDL.h>

#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace oa::app::automation {

static_assert(shot_frame_wait_limit == presented_frame_wait_limit);

namespace {

namespace png = oa::formats::png;
namespace renderer = oa::ui::frontend_renderer;

constexpr int64_t kRgbBytes = 3;
constexpr uint8_t kRgbSampleBits = 8;
/// A source wider or taller than this is refused before it is copied.
constexpr int64_t kSourceEdge = 8192;
/// A PNG larger than this, after scaling, is refused.
constexpr int64_t kOutputPixels = 1920LL * 1080LL;

/// A rectangle of pixels.
struct PixelRect {
    int64_t x{};
    int64_t y{};
    int64_t width{};
    int64_t height{};
};

/// Where a presented frame lies in the window, and the canvas it shows.
struct FramePlacement {
    PixelRect in_window;
    int64_t canvas_width{};
    int64_t canvas_height{};
};

/// Returns where a presented frame lies, as the frame request reads it.
///
/// @param check the check host
/// @param picture the presented frame
/// @return its placement
FramePlacement placed(const CheckHost& check, const renderer::Surface& picture) {
    SDL_Window* window = SDL_GetWindowFromID(check.window_id(check.context));
    SDL_Renderer* sdl_renderer = window != nullptr ? SDL_GetRenderer(window) : nullptr;
    int logical_width = 0;
    int logical_height = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_FRect reckoned{};
    if (sdl_renderer != nullptr &&
        SDL_GetRenderLogicalPresentation(sdl_renderer, &logical_width, &logical_height, &mode) &&
        mode != SDL_LOGICAL_PRESENTATION_DISABLED &&
        SDL_GetRenderLogicalPresentationRect(sdl_renderer, &reckoned)) {
        const SDL_Rect drawn = drawn_frame_rect(reckoned);
        return {
            {drawn.x,
             drawn.y,
             static_cast<int64_t>(picture.width),
             static_cast<int64_t>(picture.height)},
            logical_width,
            logical_height
        };
    }
    return {
        {0, 0, static_cast<int64_t>(picture.width), static_cast<int64_t>(picture.height)},
        static_cast<int64_t>(picture.width),
        static_cast<int64_t>(picture.height)
    };
}

/// The pixels a request asks for, in the frame.
///
/// @param ask the request
/// @param where a presented frame lies; unused for a composed one
/// @param picture the frame
/// @return the pixels, or nothing when the region lies outside the frame
std::optional<PixelRect>
crop(const ShotRequest& ask, const FramePlacement& where, const renderer::Surface& picture) {
    const int64_t picture_w = static_cast<int64_t>(picture.width);
    const int64_t picture_h = static_cast<int64_t>(picture.height);
    if (!ask.has_region)
        return PixelRect{0, 0, picture_w, picture_h};
    PixelRect region{ask.region_x, ask.region_y, ask.region_w, ask.region_h};
    if (ask.presented && !ask.window_space) {
        if (where.canvas_width <= 0 || where.canvas_height <= 0 ||
            region.x + region.width > where.canvas_width ||
            region.y + region.height > where.canvas_height)
            return std::nullopt;
        const int64_t left = region.x * picture_w / where.canvas_width;
        const int64_t top = region.y * picture_h / where.canvas_height;
        const int64_t right =
            ((region.x + region.width) * picture_w + where.canvas_width - 1) / where.canvas_width;
        const int64_t bottom = ((region.y + region.height) * picture_h + where.canvas_height - 1) /
                               where.canvas_height;
        region = {left, top, right - left, bottom - top};
    } else if (ask.presented) {
        region.x -= where.in_window.x;
        region.y -= where.in_window.y;
    }
    if (region.x < 0 || region.y < 0 || region.width <= 0 || region.height <= 0 ||
        region.x + region.width > picture_w || region.y + region.height > picture_h)
        return std::nullopt;
    return region;
}

/// Copies a region's pixels, three bytes each, rows from the top.
///
/// @param picture the frame
/// @param region the region, inside the frame
/// @return the pixels
std::vector<uint8_t> copy_region(const renderer::Surface& picture, const PixelRect& region) {
    const auto row_bytes = static_cast<std::size_t>(region.width * kRgbBytes);
    std::vector<uint8_t> pixels(row_bytes * static_cast<std::size_t>(region.height));
    for (int64_t row = 0; row < region.height; ++row) {
        const auto from = static_cast<std::size_t>(
            ((region.y + row) * static_cast<int64_t>(picture.width) + region.x) * kRgbBytes
        );
        std::memcpy(
            pixels.data() + static_cast<std::size_t>(row) * row_bytes,
            picture.rgb.data() + from,
            row_bytes
        );
    }
    return pixels;
}

/// Scales pixels with the nearest source pixel. A destination of the same
/// size returns the source.
///
/// @param source the source pixels
/// @param source_w the source width
/// @param source_h the source height
/// @param dest_w the destination width
/// @param dest_h the destination height
/// @return the destination pixels
std::vector<uint8_t> scale_pixels(
    const std::vector<uint8_t>& source,
    int32_t source_w,
    int32_t source_h,
    int32_t dest_w,
    int32_t dest_h
) {
    if (dest_w == source_w && dest_h == source_h)
        return source;
    std::vector<uint8_t> dest(
        static_cast<std::size_t>(dest_w) * static_cast<std::size_t>(dest_h) *
        static_cast<std::size_t>(kRgbBytes)
    );
    for (int32_t row = 0; row < dest_h; ++row) {
        const int32_t source_row =
            static_cast<int32_t>((static_cast<int64_t>(row) * source_h) / dest_h);
        for (int32_t column = 0; column < dest_w; ++column) {
            const int32_t source_column =
                static_cast<int32_t>((static_cast<int64_t>(column) * source_w) / dest_w);
            const std::size_t from =
                (static_cast<std::size_t>(source_row) * static_cast<std::size_t>(source_w) +
                 static_cast<std::size_t>(source_column)) *
                static_cast<std::size_t>(kRgbBytes);
            const std::size_t to =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(dest_w) +
                 static_cast<std::size_t>(column)) *
                static_cast<std::size_t>(kRgbBytes);
            dest[to] = source[from];
            dest[to + 1] = source[from + 1];
            dest[to + 2] = source[from + 2];
        }
    }
    return dest;
}

} // namespace

struct ShotBuffer::Impl {
    renderer::Surface surface{};
    bool capturing{};
    bool waiting{};
    bool ready{};
    bool failed{};
    uint32_t waited{};
    std::string failure;
    std::vector<uint8_t> png;
    int32_t width{};
    int32_t height{};
    std::string source;
};

/// Marks a picture failed and stops its capture.
///
/// @param[in,out] picture the picture
/// @param automation the automation host
/// @param why the reason
void fail_shot(ShotBuffer::Impl& picture, const AutomationHost& automation, std::string why) {
    if (picture.capturing && automation.stop_frame_capture != nullptr)
        automation.stop_frame_capture(automation.context, &picture.surface);
    picture.capturing = false;
    picture.waiting = false;
    picture.failed = true;
    picture.failure = std::move(why);
}

/// Encodes a frame's region as a PNG, scaled as the request asks.
///
/// @param[in,out] held the picture, which receives the PNG
/// @param ask the request
/// @param automation the automation host, to stop a capture
/// @param check the check host, for a presented frame's place
/// @param picture the frame
/// @param source_name presented or composed
void encode_shot(
    ShotBuffer::Impl& held,
    const ShotRequest& ask,
    const AutomationHost& automation,
    const CheckHost& check,
    const renderer::Surface& picture,
    std::string source_name
) {
    const std::size_t need = static_cast<std::size_t>(picture.width) * picture.height *
                             static_cast<std::size_t>(kRgbBytes);
    if (picture.width == 0 || picture.height == 0 || picture.rgb.size() < need) {
        fail_shot(held, automation, "the game holds no such frame");
        return;
    }
    if (picture.width > kSourceEdge || picture.height > kSourceEdge) {
        fail_shot(held, automation, "the picture is too large; pass scale or a region");
        return;
    }
    const FramePlacement where = ask.presented ? placed(check, picture) : FramePlacement{};
    const std::optional<PixelRect> region = crop(ask, where, picture);
    if (!region) {
        fail_shot(held, automation, "the region is outside the frame");
        return;
    }
    int32_t dest_w = static_cast<int32_t>(region->width * ask.scale_percent / 100);
    int32_t dest_h = static_cast<int32_t>(region->height * ask.scale_percent / 100);
    if (dest_w < 1)
        dest_w = 1;
    if (dest_h < 1)
        dest_h = 1;
    if (ask.max_width > 0 && dest_w > ask.max_width) {
        dest_h = static_cast<int32_t>((static_cast<int64_t>(dest_h) * ask.max_width) / dest_w);
        if (dest_h < 1)
            dest_h = 1;
        dest_w = ask.max_width;
    }
    if (static_cast<int64_t>(dest_w) * dest_h > kOutputPixels) {
        fail_shot(held, automation, "the picture is too large; pass scale or a region");
        return;
    }
    std::vector<uint8_t> pixels = copy_region(picture, *region);
    pixels = scale_pixels(
        pixels,
        static_cast<int32_t>(region->width),
        static_cast<int32_t>(region->height),
        dest_w,
        dest_h
    );
    const png::Image image{
        {static_cast<uint32_t>(dest_w),
         static_cast<uint32_t>(dest_h),
         kRgbSampleBits,
         png::ColorType::rgb,
         png::Interlace::none},
        {},
        pixels
    };
    std::vector<uint8_t> file;
    if (!png::write_stored(image, &file)) {
        fail_shot(held, automation, "the picture could not be encoded");
        return;
    }
    if (held.capturing && automation.stop_frame_capture != nullptr)
        automation.stop_frame_capture(automation.context, &held.surface);
    held.capturing = false;
    held.waiting = false;
    held.ready = true;
    held.png = std::move(file);
    held.width = dest_w;
    held.height = dest_h;
    held.source = std::move(source_name);
}

ShotBuffer::ShotBuffer() : impl_(std::make_unique<Impl>()) {
}

ShotBuffer::~ShotBuffer() = default;
ShotBuffer::ShotBuffer(ShotBuffer&&) noexcept = default;
ShotBuffer& ShotBuffer::operator=(ShotBuffer&&) noexcept = default;

bool ShotBuffer::waiting() const noexcept {
    return impl_ && impl_->waiting;
}

bool ShotBuffer::ready() const noexcept {
    return impl_ && impl_->ready;
}

bool ShotBuffer::failed() const noexcept {
    return impl_ && impl_->failed;
}

const std::string& ShotBuffer::failure() const {
    static const std::string kEmpty;
    return impl_ ? impl_->failure : kEmpty;
}

const std::vector<uint8_t>& ShotBuffer::png() const {
    static const std::vector<uint8_t> kEmpty;
    return impl_ ? impl_->png : kEmpty;
}

int32_t ShotBuffer::width() const noexcept {
    return impl_ ? impl_->width : 0;
}

int32_t ShotBuffer::height() const noexcept {
    return impl_ ? impl_->height : 0;
}

const std::string& ShotBuffer::source() const {
    static const std::string kEmpty;
    return impl_ ? impl_->source : kEmpty;
}

uint32_t ShotBuffer::waited() const noexcept {
    return impl_ ? impl_->waited : 0;
}

void ShotBuffer::stop(const AutomationHost& automation) {
    if (impl_ == nullptr || !impl_->capturing)
        return;
    if (automation.stop_frame_capture != nullptr)
        automation.stop_frame_capture(automation.context, &impl_->surface);
    impl_->capturing = false;
}

void ShotBuffer::abandon(const AutomationHost& automation) {
    stop(automation);
    if (impl_ == nullptr)
        return;
    impl_->waiting = false;
    impl_->ready = false;
    impl_->failed = false;
    impl_->waited = 0;
    impl_->failure.clear();
    impl_->png.clear();
    impl_->width = 0;
    impl_->height = 0;
    impl_->source.clear();
    impl_->surface = {};
}

void advance_shot(
    ShotBuffer& buffer,
    const ShotRequest& ask,
    const CheckHost& check,
    const AutomationHost& automation,
    ShotStep step
) {
    ShotBuffer::Impl& impl = *buffer.impl_;
    if (impl.ready || impl.failed)
        return;
    if (step == ShotStep::pump) {
        if (impl.waiting)
            return;
        if (ask.presented) {
            if (automation.start_frame_capture == nullptr) {
                fail_shot(impl, automation, "the game cannot copy a frame");
                return;
            }
            impl.surface = {};
            if (!automation.start_frame_capture(automation.context, &impl.surface)) {
                fail_shot(impl, automation, "a picture is already being taken");
                return;
            }
            impl.capturing = true;
        }
        impl.waiting = true;
        return;
    }
    if (!impl.waiting)
        return;
    if (ask.presented) {
        const std::size_t need = static_cast<std::size_t>(impl.surface.width) *
                                 impl.surface.height * static_cast<std::size_t>(kRgbBytes);
        if (impl.surface.width == 0 || impl.surface.height == 0 || impl.surface.rgb.size() < need) {
            ++impl.waited;
            if (impl.waited >= presented_frame_wait_limit)
                fail_shot(impl, automation, "the game presented no frame");
            return;
        }
        encode_shot(impl, ask, automation, check, impl.surface, "presented");
        return;
    }
    const renderer::Surface* composed =
        check.surface != nullptr ? check.surface(check.context) : nullptr;
    if (composed == nullptr || composed->width == 0 || composed->height == 0) {
        ++impl.waited;
        if (impl.waited >= presented_frame_wait_limit)
            fail_shot(impl, automation, "the game holds no such frame");
        return;
    }
    encode_shot(impl, ask, automation, check, *composed, "composed");
}

} // namespace oa::app::automation
