// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Full's postprocessing readback, coordinate mapping and upload.
#include "full_effects.hpp"
#include "render_state.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace oa::app::full_effects {
namespace {
/// Bytes a pixel of the readback, its RGBA copy and the upload holds.
constexpr uint64_t pixel_bytes = 4;
} // namespace

Upload::~Upload() {
    reset();
}

void Upload::reset() noexcept {
    SDL_DestroyTexture(texture);
    texture = nullptr;
    width = height = 0;
}

std::string compose(
    SDL_Renderer* renderer,
    Upload& upload,
    const SDL_Rect& battlefield,
    SDL_Texture* moved_target,
    uint32_t moved_factor,
    const std::function<void(const Pixels&)>& process,
    const SDL_FRect* effect_bounds,
    const std::function<bool(uint64_t bytes)>& fits,
    SDL_Texture* final_target
) {
    RenderState saved(renderer);
    std::optional<RenderState> moved_state;
    SDL_Rect area{};
    float origin_x = 0.0F;
    float origin_y = 0.0F;
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    if (moved_target != nullptr) {
        if (!SDL_SetRenderTarget(renderer, moved_target))
            return ("the effects world target: " + std::string(SDL_GetError()));
        moved_state.emplace(renderer);
        scale_x = scale_y = static_cast<float>(moved_factor);
        origin_x = origin_y = -1.0F;
        area = {
            0,
            0,
            static_cast<int>((battlefield.w + 2) * moved_factor),
            static_cast<int>((battlefield.h + 2) * moved_factor)
        };
        if (!SDL_SetRenderScale(renderer, 1.0F, 1.0F) ||
            !SDL_SetRenderViewport(renderer, nullptr) || !SDL_SetRenderClipRect(renderer, nullptr))
            return ("the effects target coordinates: " + std::string(SDL_GetError()));
    } else {
        int logical_width = 0;
        int logical_height = 0;
        SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
        SDL_FRect presentation{};
        if (!SDL_GetRenderLogicalPresentation(renderer, &logical_width, &logical_height, &mode) ||
            !SDL_GetRenderLogicalPresentationRect(renderer, &presentation) ||
            !SDL_GetRenderScale(renderer, &scale_x, &scale_y))
            return ("the effects window coordinates: " + std::string(SDL_GetError()));
        if (mode != SDL_LOGICAL_PRESENTATION_DISABLED && logical_width > 0 && logical_height > 0) {
            scale_x *= presentation.w / static_cast<float>(logical_width);
            scale_y *= presentation.h / static_cast<float>(logical_height);
        } else {
            presentation.x = presentation.y = 0.0F;
        }
        const float left = presentation.x + static_cast<float>(battlefield.x) * scale_x;
        const float top = presentation.y + static_cast<float>(battlefield.y) * scale_y;
        area = {
            static_cast<int>(std::ceil(left)),
            static_cast<int>(std::ceil(top)),
            static_cast<int>(std::floor(left + static_cast<float>(battlefield.w) * scale_x)) -
                static_cast<int>(std::ceil(left)),
            static_cast<int>(std::floor(top + static_cast<float>(battlefield.h) * scale_y)) -
                static_cast<int>(std::ceil(top))
        };
        int output_width = 0;
        int output_height = 0;
        if (!SDL_GetCurrentRenderOutputSize(renderer, &output_width, &output_height))
            return "the effects output size: " + std::string(SDL_GetError());
        const int right = std::min(area.x + area.w, output_width);
        const int bottom = std::min(area.y + area.h, output_height);
        area.x = std::max(area.x, 0);
        area.y = std::max(area.y, 0);
        area.w = std::max(right - area.x, 0);
        area.h = std::max(bottom - area.y, 0);
        origin_x = (static_cast<float>(area.x) - left) / scale_x;
        origin_y = (static_cast<float>(area.y) - top) / scale_y;
        saved.use_target_pixels(final_target);
    }
    int row_offset = 0;
    if (effect_bounds != nullptr) {
        // Clamp before conversion: an off-screen source may be far outside the target.
        const int left = static_cast<int>(
            std::clamp(std::floor((effect_bounds->x - origin_x) * scale_x), 0.0F, float(area.w))
        );
        const int top = static_cast<int>(
            std::clamp(std::floor((effect_bounds->y - origin_y) * scale_y), 0.0F, float(area.h))
        );
        const int right = static_cast<int>(std::clamp(
            std::ceil((effect_bounds->x + effect_bounds->w - origin_x) * scale_x),
            0.0F,
            float(area.w)
        ));
        const int bottom = static_cast<int>(std::clamp(
            std::ceil((effect_bounds->y + effect_bounds->h - origin_y) * scale_y),
            0.0F,
            float(area.h)
        ));
        row_offset = top;
        area.x += left;
        area.y += top;
        area.w = std::max(0, right - left);
        area.h = std::max(0, bottom - top);
        origin_x += float(left) / scale_x;
        origin_y += float(top) / scale_y;
    }
    if (area.w > 0 && area.h > 0) {
        // The readback and its RGBA copy are held at once, with the upload
        // when it must grow to take them.
        const uint64_t area_bytes = uint64_t(area.w) * uint64_t(area.h) * pixel_bytes;
        uint64_t held = 2 * area_bytes;
        if (!upload.texture || upload.width < area.w || upload.height < area.h)
            held += uint64_t(std::max(upload.width, area.w)) *
                    uint64_t(std::max(upload.height, area.h)) * pixel_bytes;
        if (fits && !fits(held))
            return "the effects readback of " + std::to_string(area.w) + "x" +
                   std::to_string(area.h) + " would leave too little memory";
        using SurfaceOwner = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
        SurfaceOwner read(SDL_RenderReadPixels(renderer, &area), SDL_DestroySurface);
        SurfaceOwner rgba(
            read ? SDL_ConvertSurface(read.get(), SDL_PIXELFORMAT_RGBA32) : nullptr,
            SDL_DestroySurface
        );
        if (!rgba)
            return ("reading the effects world: " + std::string(SDL_GetError()));
        process(
            {{static_cast<uint8_t*>(rgba->pixels), static_cast<std::size_t>(rgba->pitch) * rgba->h},
             static_cast<uint32_t>(rgba->w),
             static_cast<uint32_t>(rgba->h),
             static_cast<uint32_t>(rgba->pitch),
             origin_x,
             origin_y,
             scale_x,
             scale_y,
             row_offset}
        );
        if (!upload.texture || upload.width < rgba->w || upload.height < rgba->h) {
            const int width = std::max(upload.width, rgba->w);
            const int height = std::max(upload.height, rgba->h);
            upload.reset();
            upload.texture = SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height
            );
            if (!upload.texture)
                return "allocating the effects upload: " + std::string(SDL_GetError());
            upload.width = width;
            upload.height = height;
        }
        const SDL_FRect destination{
            static_cast<float>(area.x),
            static_cast<float>(area.y),
            static_cast<float>(rgba->w),
            static_cast<float>(rgba->h)
        };
        const SDL_Rect updated{0, 0, rgba->w, rgba->h};
        const SDL_FRect source{0, 0, static_cast<float>(rgba->w), static_cast<float>(rgba->h)};
        if (!SDL_UpdateTexture(upload.texture, &updated, rgba->pixels, rgba->pitch) ||
            !SDL_SetTextureBlendMode(upload.texture, SDL_BLENDMODE_NONE) ||
            !SDL_SetTextureScaleMode(upload.texture, SDL_SCALEMODE_NEAREST) ||
            !SDL_RenderTexture(renderer, upload.texture, &source, &destination))
            return ("presenting the effects world: " + std::string(SDL_GetError()));
    }

    return {};
}
} // namespace oa::app::full_effects
