// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/runtime.hpp"
#include "match_models.hpp"
#include "oa/app/match_console.hpp"
#include "oa/present/world_renderer/world_overlays.hpp"
#include "oa/ui/console/console.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

#include "oa/present/display.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/present/model/rgb_bridge.hpp"

namespace oa::app {

void Runtime::draw_live_overlays() {
    if (!live_renderer_active() || !match_ || match_world_cpu_.rgb.empty())
        return;
    const auto& game = match_->state().game;
    const bool debug = game.debug_overlay != oa::present::world_renderer::debug_view::off ||
                       (console_ && console_->state.contour_values[0] != 0);
    if (debug) {
        auto& models = match_models();
        const int32_t width = static_cast<int32_t>(match_world_cpu_.width);
        const int32_t height = static_cast<int32_t>(match_world_cpu_.height);
        const auto before = match_world_cpu_.rgb;
        const auto terrain = live_terrain_visibility_mask();
        const oa::present::model::RgbFrame frame{
            match_world_cpu_.rgb.data(), width, height, width * 3
        };
        auto* previous = oa::present::display_context();
        oa::present::bind_display(&models.display.context);
        draw_match_debug_grid(
            models.bridge, models.display, frame, {0, 0, width - 1, height - 1}, 1.0F
        );
        oa::present::bind_display(previous);
        // Ground annotations share fog brightness and cannot cover objects or unknown cells.
        for (std::size_t pixel = 0; pixel < before.size() / 3; ++pixel) {
            const auto* original = before.data() + pixel * 3;
            auto* painted = match_world_cpu_.rgb.data() + pixel * 3;
            if (std::equal(original, original + 3, painted))
                continue;
            if (pixel >= terrain.size() || terrain[pixel] == 0) {
                std::copy_n(original, 3, painted);
                continue;
            }
            // The existing scene already includes fog; dim only newly painted annotation pixels.
            for (int channel = 0; channel < 3; ++channel)
                painted[channel] = static_cast<uint8_t>(
                    (static_cast<unsigned>(painted[channel]) * terrain[pixel] + 127U) / 255U
                );
        }
    }
    if (modifier_role_active("order.queue-modifier") || virtual_shift(ModifierUse::order)) {
        oa::present::world_renderer::Surface surface{
            match_world_cpu_.width, match_world_cpu_.height, std::move(match_world_cpu_.rgb)
        };
        auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto old_clip = world_pixel_clip_;
        world_pixel_clip_ = {
            0, 0, static_cast<int32_t>(surface.width), static_cast<int32_t>(surface.height)
        };
        std::ignore = draw_order_overlays(surface, viewport);
        world_pixel_clip_ = old_clip;
        match_world_cpu_.rgb = std::move(surface.rgb);
    }
}

} // namespace oa::app
