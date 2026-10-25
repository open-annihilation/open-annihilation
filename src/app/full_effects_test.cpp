// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Full's actual SDL postprocessing seam at ordinary, HiDPI and moved-target scales.
#include "full_effects.hpp"
#include "renderer_effects.hpp"
#include "oa/test/check.hpp"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void run_case(int density, bool moved, bool fractional_shift = false, bool cropped = false) {
    using namespace oa::app::full_effects;
    SDL_Surface* canvas = SDL_CreateSurface(80 * density, 60 * density, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = canvas ? SDL_CreateSoftwareRenderer(canvas) : nullptr;
    OA_CHECK(renderer != nullptr);
    if (!renderer) {
        SDL_DestroySurface(canvas);
        return;
    }
    const SDL_Rect field{10, 8, 40, 30};
    if (density > 1)
        OA_CHECK(
            SDL_SetRenderLogicalPresentation(renderer, 80, 60, SDL_LOGICAL_PRESENTATION_LETTERBOX)
        );
    OA_CHECK(SDL_SetRenderDrawColor(renderer, 30, 40, 50, 255));
    OA_CHECK(SDL_RenderClear(renderer));
    SDL_Texture* target =
        moved
            ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 84, 64)
            : nullptr;
    if (moved) {
        OA_CHECK(target != nullptr);
        OA_CHECK(SDL_SetRenderTarget(renderer, target));
        OA_CHECK(SDL_SetRenderScale(renderer, 2, 2));
        OA_CHECK(SDL_SetRenderDrawColor(renderer, 30, 40, 50, 255));
        OA_CHECK(SDL_RenderClear(renderer));
    }
    const SDL_FRect world = moved ? SDL_FRect{1, 1, 40, 30} : SDL_FRect{10, 8, 40, 30};
    OA_CHECK(SDL_SetRenderDrawColor(renderer, 80, 90, 100, 255));
    OA_CHECK(SDL_RenderFillRect(renderer, &world));
    if (moved)
        OA_CHECK(SDL_SetRenderTarget(renderer, nullptr));
    const SDL_Rect clip{3, 2, 72, 54};
    OA_CHECK(SDL_SetRenderClipRect(renderer, &clip));
    bool called = false;
    const SDL_FRect bounds{6, 4, 6, 6};
    {
        Upload upload;
        const std::string error = compose(
            renderer,
            upload,
            field,
            target,
            2,
            [&](const Pixels& image) {
                called = true;
                OA_CHECK(image.scale_x == (moved ? 2.0F : static_cast<float>(density)));
                OA_CHECK(image.scale_y == image.scale_x);
                OA_CHECK(image.origin_x == (cropped ? 6.0F : moved ? -1.0F : 0.0F));
                OA_CHECK(image.origin_y == (cropped ? 4.0F : image.origin_x));
                OA_CHECK(image.row_offset == (cropped ? (moved ? 10 : 4 * density) : 0));
                OA_CHECK(
                    image.width == static_cast<uint32_t>(
                                       cropped ? 6 * (moved ? 2 : density)
                                       : moved ? 84
                                               : 40 * density
                                   )
                );
                OA_CHECK(
                    image.height == static_cast<uint32_t>(
                                        cropped ? 6 * (moved ? 2 : density)
                                        : moved ? 64
                                                : 30 * density
                                    )
                );
                // Exact-view sources include the later resolver shift. Adding
                // the same shift to the target origin keeps their pre-resolve
                // pixels aligned with the rounded-view scene.
                const float shift_x = fractional_shift ? 0.25F : 0.0F;
                const float shift_y = fractional_shift ? -0.25F : 0.0F;
                const float source_x = 8.0F + shift_x;
                const float source_y = 6.0F + shift_y;
                for (uint32_t y = 0; y < image.height; ++y)
                    for (uint32_t x = 0; x < image.width; ++x) {
                        const float local_x =
                            image.origin_x + shift_x + static_cast<float>(x) / image.scale_x;
                        const float local_y =
                            image.origin_y + shift_y + static_cast<float>(y) / image.scale_y;
                        if (local_x >= source_x && local_x < source_x + 2 && local_y >= source_y &&
                            local_y < source_y + 2) {
                            auto* pixel = image.bytes.data() + y * image.pitch + x * 4;
                            pixel[0] = 0;
                            pixel[1] = 255;
                            pixel[2] = 0;
                        }
                    }
            },
            cropped ? &bounds : nullptr
        );
        OA_CHECK(error.empty());
        if (!error.empty())
            SDL_Log("%s", error.c_str());
        OA_CHECK(called);
        OA_CHECK(SDL_GetRenderTarget(renderer) == nullptr);
        SDL_Rect restored_clip{};
        OA_CHECK(SDL_RenderClipEnabled(renderer));
        OA_CHECK(SDL_GetRenderClipRect(renderer, &restored_clip));
        OA_CHECK(
            restored_clip.x == clip.x && restored_clip.y == clip.y && restored_clip.w == clip.w &&
            restored_clip.h == clip.h
        );
        int logical_w = 0, logical_h = 0;
        SDL_RendererLogicalPresentation mode{};
        OA_CHECK(SDL_GetRenderLogicalPresentation(renderer, &logical_w, &logical_h, &mode));
        OA_CHECK(
            mode ==
            (density > 1 ? SDL_LOGICAL_PRESENTATION_LETTERBOX : SDL_LOGICAL_PRESENTATION_DISABLED)
        );
        if (moved) {
            OA_CHECK(SDL_SetRenderTarget(renderer, target));
            float sx = 0, sy = 0;
            OA_CHECK(SDL_GetRenderScale(renderer, &sx, &sy));
            OA_CHECK(sx == 2 && sy == 2);
        }
        // A later overlay must remain sharp and at the restored layout scale.
        const SDL_FRect overlay = moved ? SDL_FRect{17, 13, 2, 2} : SDL_FRect{26, 20, 2, 2};
        OA_CHECK(SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255));
        OA_CHECK(SDL_RenderFillRect(renderer, &overlay));
        OA_CHECK(
            SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)
        );
        OA_CHECK(SDL_SetRenderScale(renderer, 1, 1));
        SDL_Surface* read = SDL_RenderReadPixels(renderer, nullptr);
        SDL_Surface* rgba = read ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32) : nullptr;
        OA_CHECK(rgba != nullptr);
        if (rgba) {
            const auto at = [&](int x, int y) {
                return static_cast<uint8_t*>(rgba->pixels) + y * rgba->pitch + x * 4;
            };
            const int scale = moved ? 2 : density;
            const int left = moved ? 1 : field.x;
            const int top = moved ? 1 : field.y;
            const auto* effect = at((left + 8) * scale, (top + 6) * scale);
            OA_CHECK(effect[0] == 0 && effect[1] == 255 && effect[2] == 0);
            const auto* hud = at(0, 0);
            OA_CHECK(hud[0] == 30 && hud[1] == 40 && hud[2] == 50);
            const auto* mark = at((left + 16) * scale, (top + 12) * scale);
            OA_CHECK(mark[0] == 255 && mark[1] == 0 && mark[2] == 255);
        }
        SDL_DestroySurface(rgba);
        SDL_DestroySurface(read);
    }
    SDL_SetRenderTarget(renderer, nullptr);
    SDL_DestroyTexture(target);
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(canvas);
}

/// Cropping must retain the heat wave phase and the pixels sampled at its edges.
std::vector<uint8_t> heat_picture(bool cropped) {
    using namespace oa::app;
    SDL_Surface* canvas = SDL_CreateSurface(80, 60, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = canvas ? SDL_CreateSoftwareRenderer(canvas) : nullptr;
    OA_CHECK(renderer != nullptr);
    if (!renderer) {
        SDL_DestroySurface(canvas);
        return {};
    }
    for (int x = 0; x < 80; ++x) {
        SDL_SetRenderDrawColor(renderer, static_cast<uint8_t>((x % 3) * 80), 40, 60, 255);
        const SDL_FRect stripe{float(x), 0, 1, 60};
        SDL_RenderFillRect(renderer, &stripe);
    }
    {
        full_effects::Upload upload;
        const SDL_FRect bounds{8, 4, 22, 22};
        const auto failure = full_effects::compose(
            renderer,
            upload,
            {10, 8, 40, 30},
            nullptr,
            1,
            [&](const full_effects::Pixels& image) {
                renderer_effects::Source source{
                    18 - image.origin_x,
                    14 - image.origin_y,
                    0,
                    7,
                    1,
                    1,
                    0.5F,
                    0.25F,
                    0.4F,
                    0.3F,
                    1,
                    0.5F + float(image.row_offset) * 0.55F
                };
                renderer_effects::compose(
                    {image.bytes, int(image.width), int(image.height), int(image.pitch), 4, {}, {}},
                    {&source, 1},
                    {true, true, true},
                    0.75
                );
            },
            cropped ? &bounds : nullptr
        );
        OA_CHECK(failure.empty());
        SDL_RenderPresent(renderer);
    }
    const auto* bytes = static_cast<const uint8_t*>(canvas->pixels);
    std::vector<uint8_t> result(bytes, bytes + canvas->pitch * canvas->h);
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(canvas);
    return result;
}

/// The memory guard is asked for the readback, its copy and the grown
/// upload together; refused, the pass reads, processes and draws nothing and
/// leaves the renderer as it was. Allowed, a later pass draws as usual.
void guarded_case() {
    using namespace oa::app::full_effects;
    SDL_Surface* canvas = SDL_CreateSurface(80, 60, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = canvas ? SDL_CreateSoftwareRenderer(canvas) : nullptr;
    OA_CHECK(renderer != nullptr);
    if (!renderer) {
        SDL_DestroySurface(canvas);
        return;
    }
    OA_CHECK(SDL_SetRenderDrawColor(renderer, 30, 40, 50, 255));
    OA_CHECK(SDL_RenderClear(renderer));
    const SDL_Rect clip{3, 2, 72, 54};
    OA_CHECK(SDL_SetRenderClipRect(renderer, &clip));
    const SDL_Rect field{10, 8, 40, 30};
    const auto paint_green = [](const Pixels& image) {
        for (uint32_t y = 0; y < image.height; ++y)
            for (uint32_t x = 0; x < image.width; ++x) {
                auto* pixel = image.bytes.data() + y * image.pitch + x * 4;
                pixel[0] = 0;
                pixel[1] = 255;
                pixel[2] = 0;
            }
    };
    Upload upload;
    std::vector<uint64_t> asked;
    bool called = false;
    const std::string refused = compose(
        renderer,
        upload,
        field,
        nullptr,
        1,
        [&](const Pixels& image) {
            called = true;
            paint_green(image);
        },
        nullptr,
        [&](uint64_t bytes) {
            asked.push_back(bytes);
            return false;
        }
    );
    OA_CHECK(!refused.empty());
    OA_CHECK(!called);
    OA_CHECK(upload.texture == nullptr);
    // The readback and its copy, and the upload it would have made.
    OA_CHECK(asked.size() == 1 && asked[0] == 3U * 40U * 30U * 4U);
    SDL_Rect restored_clip{};
    OA_CHECK(SDL_GetRenderClipRect(renderer, &restored_clip));
    OA_CHECK(restored_clip.x == clip.x && restored_clip.w == clip.w);
    SDL_RenderPresent(renderer);
    const auto at = [&](int x, int y) {
        return static_cast<const uint8_t*>(canvas->pixels) + y * canvas->pitch + x * 4;
    };
    OA_CHECK(at(20, 20)[0] == 30 && at(20, 20)[1] == 40 && at(20, 20)[2] == 50);
    // Allowed, the pass draws; an upload large enough is not asked for again.
    asked.clear();
    for (int pass = 0; pass < 2; ++pass)
        OA_CHECK(
            compose(renderer, upload, field, nullptr, 1, paint_green, nullptr, [&](uint64_t bytes) {
                asked.push_back(bytes);
                return true;
            }).empty()
        );
    OA_CHECK(
        asked.size() == 2 && asked[0] == 3U * 40U * 30U * 4U && asked[1] == 2U * 40U * 30U * 4U
    );
    SDL_RenderPresent(renderer);
    OA_CHECK(at(20, 20)[0] == 0 && at(20, 20)[1] == 255 && at(20, 20)[2] == 0);
    upload.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(canvas);
}
} // namespace

int main() {
    OA_CHECK(heat_picture(false) == heat_picture(true));
    run_case(1, false);
    run_case(2, false);
    run_case(1, true);
    run_case(2, true);
    run_case(2, true, true);
    run_case(1, false, false, true);
    run_case(2, false, false, true);
    run_case(1, true, false, true);
    run_case(2, true, true, true);
    guarded_case();
    return oa::test::check_exit_status();
}
