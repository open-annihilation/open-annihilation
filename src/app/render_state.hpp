// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The renderer's state a screen drawn in the window's own pixels changes,
// kept to put back (RenderState): the Game files screen, the folder chooser
// and the OA layer (oa_layer.hpp) each keep one while they draw.
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>

namespace oa::app {

/// The renderer's state a screen changes, kept to put back at the end.
class RenderState {
  public:

    /// Keeps the renderer's target, logical presentation, scale, viewport, clip, colour and
    /// blend mode.
    ///
    /// @param renderer the renderer
    explicit RenderState(SDL_Renderer* renderer) noexcept : renderer_(renderer) {
        target_ = SDL_GetRenderTarget(renderer_);
        SDL_GetRenderLogicalPresentation(
            renderer_, &logical_width_, &logical_height_, &logical_mode_
        );
        SDL_GetRenderScale(renderer_, &scale_x_, &scale_y_);
        viewport_set_ = SDL_RenderViewportSet(renderer_);
        SDL_GetRenderViewport(renderer_, &viewport_);
        clip_set_ = SDL_RenderClipEnabled(renderer_);
        SDL_GetRenderClipRect(renderer_, &clip_);
        SDL_GetRenderDrawColor(renderer_, &colour_[0], &colour_[1], &colour_[2], &colour_[3]);
        SDL_GetRenderDrawBlendMode(renderer_, &blend_);
    }

    /// Puts everything back as it was.
    ~RenderState() {
        SDL_SetRenderTarget(renderer_, target_);
        SDL_SetRenderLogicalPresentation(renderer_, logical_width_, logical_height_, logical_mode_);
        SDL_SetRenderScale(renderer_, scale_x_, scale_y_);
        SDL_SetRenderViewport(renderer_, viewport_set_ ? &viewport_ : nullptr);
        SDL_SetRenderClipRect(renderer_, clip_set_ ? &clip_ : nullptr);
        SDL_SetRenderDrawColor(renderer_, colour_[0], colour_[1], colour_[2], colour_[3]);
        SDL_SetRenderDrawBlendMode(renderer_, blend_);
    }

    RenderState(const RenderState&) = delete;
    RenderState& operator=(const RenderState&) = delete;

    /// Draws to the window directly, in its own pixels: no target, no logical presentation,
    /// no scale, no viewport and no clip.
    void use_window_pixels() const noexcept { use_target_pixels(nullptr); }

    /// Draws to a target directly, in its own pixels, as use_window_pixels draws to the
    /// window: no logical presentation, no scale, no viewport and no clip.
    ///
    /// @param target the texture drawn into; null for the window
    void use_target_pixels(SDL_Texture* target) const noexcept {
        SDL_SetRenderTarget(renderer_, target);
        SDL_SetRenderLogicalPresentation(renderer_, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        SDL_SetRenderScale(renderer_, 1.0F, 1.0F);
        SDL_SetRenderViewport(renderer_, nullptr);
        SDL_SetRenderClipRect(renderer_, nullptr);
    }

  private:

    SDL_Renderer* renderer_{}; ///< the renderer
    SDL_Texture* target_{};    ///< its target
    int logical_width_{};      ///< logical width
    int logical_height_{};     ///< logical height
    SDL_RendererLogicalPresentation logical_mode_{SDL_LOGICAL_PRESENTATION_DISABLED}; ///< its mode
    float scale_x_{1.0F};                      ///< scale across
    float scale_y_{1.0F};                      ///< scale down
    bool viewport_set_{};                      ///< a viewport was set
    SDL_Rect viewport_{};                      ///< the viewport
    bool clip_set_{};                          ///< a clip was set
    SDL_Rect clip_{};                          ///< the clip
    std::array<uint8_t, 4> colour_{};          ///< the draw colour
    SDL_BlendMode blend_{SDL_BLENDMODE_BLEND}; ///< the blend mode
};

} // namespace oa::app
