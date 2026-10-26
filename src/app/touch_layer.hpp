// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch layer's own state, kept in Runtime::TouchState, and the
// drawing's access to the runtime (docs/touch-controls.md). The layer holds
// the touch controls painted at the display's pixels: the match's layout
// size times its density, so that their text is drawn sharp on a window at
// native density, while every place the controls take stays in layout
// pixels.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/app/scaled_world.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/touch_hud.hpp"
#include "oa/ui/paint/painter.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <memory>
#include <optional>
#include <stdint.h>
#include <string>
#include <vector>

namespace oa::app {

// Named once, whichever app header that draws with the painter is included first.
#ifndef OA_APP_UI_PAINT
#define OA_APP_UI_PAINT
namespace paint = oa::ui::paint;
#endif

/// The touch layer's drawing state: its buffer, texture, fonts and what it last drew.
struct TouchLayer {
    /// What the layer was drawn for, beside the frame: the layer is drawn
    /// again when any of it changes.
    struct Look {
        uint32_t revision{};    ///< the HudState revision drawn
        int32_t width{};        ///< the match's layout width, in layout pixels
        int32_t height{};       ///< the match's layout height, in layout pixels
        int32_t layer_width{};  ///< the layer's width, in its own pixels
        int32_t layer_height{}; ///< the layer's height, in its own pixels
        float px_per_point{};   ///< layout pixels per point the controls were laid out at

        /// Compares two looks field by field.
        ///
        /// @return true when every field matches
        bool operator==(const Look&) const = default;
    };

    /// The layer: layer_width × layer_height pixels, before the display gamma.
    paint::Canvas canvas{};
    /// The part of the layer that is not clear, in layer pixels; empty when nothing shows.
    paint::Box bounds{};
    /// What the canvas holds; nothing before the first draw.
    std::optional<Look> drawn{};
    /// The frame the canvas was drawn from.
    oa::ui::touch_hud::Frame drawn_frame{};
    uint32_t drawn_revision{}; ///< the HudState revision the layer last drew
    /// What the texture holds; nothing when it is stale.
    std::optional<Look> uploaded{};
    std::array<uint8_t, 256> uploaded_gamma{}; ///< the gamma table the texture was uploaded at
    SDL_Renderer* texture_renderer{};          ///< the renderer the texture was made on
    uint32_t texture_resets{};                 ///< device resets seen when it was uploaded
    /// The painted rows through the gamma table, kept between uploads.
    std::vector<uint8_t> corrected{};
    /// The layer's texture, in tiles beyond the renderer's limit; none before the first upload.
    TiledTexture texture;
    /// The fonts the controls' labels are drawn with; null when they could not be opened.
    std::unique_ptr<oa::platform::text_font::FontStack> fonts{};
    bool fonts_tried{}; ///< the fonts were looked for once
};

/// The drawing's helpers that reach the runtime's private members: static functions that take
/// Runtime&.
struct TouchDrawAccess {
    /// Draws the layer again when what it shows changed. Draws the touch
    /// controls or the pad HUD, and a wheel a mouse button opened. With
    /// neither the controls nor the pad HUD on, that wheel is the only thing
    /// drawn. Draws nothing when no touch state was made or the screen is
    /// not the match.
    ///
    /// @param runtime the runtime
    /// @return true when the layer shows anything
    static bool refresh_layer(Runtime& runtime);

    /// Returns the banner's title as the layer draws it, in the language shown
    /// (oa::ui::touch_hud::banner_title).
    ///
    /// @param runtime the runtime
    /// @return the title; empty when no touch state was made or no banner shows
    [[nodiscard]] static std::string banner_title(Runtime& runtime);

    /// Forgets the layer's texture, as the match's other textures are forgotten when the
    /// renderer's textures are lost or the match ends; the next present makes it again.
    ///
    /// @param runtime the runtime; nothing happens when it made no touch state
    static void forget_textures(Runtime& runtime) noexcept;

    /// Returns the device resets the renderer has come through, so that an
    /// upload made before one is made again.
    ///
    /// @param runtime the runtime
    /// @return the count; 0 without a borrowed renderer
    [[nodiscard]] static uint32_t device_resets(const Runtime& runtime) noexcept;
};

} // namespace oa::app
