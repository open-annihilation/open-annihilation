// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the Full tier takes from the processor's frame for the card: the fog
// grid the frame built, which the card's fog passes draw from; the overlay
// canvas's key colour, which the world layer is cleared to before the
// painters paint it; and the quads the painters that shade the world beneath
// them, the kill board, the +stats panel and game text in the modern fonts,
// ask the card to draw in place of reading the world, with the same shading
// of what the canvas holds under them.
#include "oa/app/runtime.hpp"

#include "full_fog.hpp"
#include "full_presentation.hpp"
#include "xrgb_conversion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <tuple>
#include <vector>

namespace oa::app {

namespace {

/// The opacities frontend_renderer::blend_rect takes, in 256ths of a pixel.
constexpr float blend_opacity_whole = 256.0F;
/// The largest level of a channel.
constexpr double channel_whole = 255.0;
/// The most quads a pixel's count of them holds.
constexpr int most_counted_quads = 255;

/// Gives a channel's level with a quad's colour drawn over it, as the card
/// draws the quad by its blend.
///
/// @param under the level under the quad, from 0 to 255
/// @param colour the quad colour's level, from 0 to 255
/// @param alpha the quad's alpha, from 0 to 1
/// @param blend the quad's blend
/// @return the level shown, neither rounded nor held to 0 to 255
double blended_level(double under, double colour, double alpha, card::Blend blend) noexcept {
    double shown = under;
    switch (blend) {
    case card::Blend::none:
        shown = colour;
        break;
    case card::Blend::alpha:
        shown = colour * alpha + under * (1.0 - alpha);
        break;
    case card::Blend::alpha_premultiplied:
        shown = colour + under * (1.0 - alpha);
        break;
    case card::Blend::additive:
        shown = under + colour * alpha;
        break;
    case card::Blend::modulate:
        shown = under * colour / channel_whole;
        break;
    case card::Blend::darken:
        shown = under * (1.0 - alpha);
        break;
    case card::Blend::minimum:
        shown = std::min(under, colour);
        break;
    case card::Blend::lighten:
        shown = under * (1.0 + colour / channel_whole - alpha);
        break;
    }
    return shown;
}

/// Draws a painter's quad over the overlay canvas's own pixels inside it,
/// those not its key colour, by the quad's blend. The card draws the quad
/// under the canvas, over the world alone, so what the canvas holds there
/// from the painters before, such as health bars, markers and labels, is
/// covered here as the world is; over a quad of alpha blend the pixels come
/// out as frontend_renderer::blend_rect blends them in the other tiers.
///
/// @param[in,out] canvas the paint target holding the canvas's pixels
/// @param key the canvas's key colour
/// @param quad the quad, in pixels of the canvas
/// @param colour the quad's colour before the display gamma, as the canvas
///     holds its pixels
void cover_canvas_paint(
    renderer::Surface& canvas,
    const std::array<uint8_t, 3>& key,
    const FullWorldQuad& quad,
    const std::array<uint8_t, 3>& colour
) {
    const auto columns = static_cast<int64_t>(canvas.width);
    const auto rows = static_cast<int64_t>(canvas.height);
    if (canvas.rgb.size() < static_cast<std::size_t>(columns * rows) * 3U)
        return;
    const int64_t left = std::max<int64_t>(quad.x, 0);
    const int64_t top = std::max<int64_t>(quad.y, 0);
    const int64_t right = std::min<int64_t>(int64_t{quad.x} + quad.width, columns);
    const int64_t bottom = std::min<int64_t>(int64_t{quad.y} + quad.height, rows);
    const auto alpha = static_cast<double>(quad.colour.alpha);
    for (int64_t y = top; y < bottom; ++y)
        for (int64_t x = left; x < right; ++x) {
            uint8_t* pixel = canvas.rgb.data() + static_cast<std::size_t>(y * columns + x) * 3U;
            if (std::equal(key.begin(), key.end(), pixel))
                continue;
            for (std::size_t channel = 0; channel < 3; ++channel)
                pixel[channel] = static_cast<uint8_t>(std::lround(
                    std::clamp(
                        blended_level(pixel[channel], colour[channel], alpha, quad.blend),
                        0.0,
                        channel_whole
                    )
                ));
        }
}

} // namespace

void Runtime::note_full_fog_grid(
    const oa::present::world_renderer::FogGrid& grid,
    int32_t camera_x,
    int32_t camera_z,
    bool dithered,
    bool changed
) {
    if (!full_)
        return;
    auto& fog = full_->fog;
    if (changed)
        ++fog.revision;
    fog.grid = grid;
    fog.camera_x = camera_x;
    fog.camera_z = camera_z;
    fog.dithered = dithered;
    const auto* gamma = gamma_identity_ ? nullptr : &gamma_table_;
    fog.unmapped = full::flat_colour(fog_shading_.unmapped_rgb, gamma);
    fog.dither = full::flat_colour(fog_shading_.dither_rgb, gamma);
    for (std::size_t index = 0; index < fog.palette.size(); ++index)
        fog.palette[index] = full::flat_colour(fog_shading_.palette_rgb[index], gamma);
}

std::array<uint8_t, 3> Runtime::full_overlay_key() {
    if (!full_)
        return overlay_key_colour(match_palette_);
    auto& full = *full_;
    if (!full.overlay_key_found || full.overlay_key_palette != match_palette_) {
        full.overlay_key = overlay_key_colour(match_palette_);
        full.overlay_key_palette = match_palette_;
        full.overlay_key_found = true;
    }
    return full.overlay_key;
}

bool Runtime::full_frame_drawn() const noexcept {
    return full_ && full_->canvas_drawn;
}

void Runtime::note_full_canvas(bool canvas, int32_t camera_x, int32_t camera_y, float zoom) {
    if (!full_)
        return;
    full_->canvas_drawn = canvas;
    if (!canvas)
        return;
    full_->frame_camera_x = camera_x;
    full_->frame_camera_y = camera_y;
    full_->frame_zoom = zoom;
    full_->canvas_pixels = nullptr;
    full_->world_quads.clear();
}

void Runtime::note_full_canvas_pixels(const uint8_t* pixels) noexcept {
    if (full_ && full_->canvas_drawn)
        full_->canvas_pixels = pixels;
}

bool Runtime::paints_full_canvas() {
    if (!full_presentation() || !full_->canvas_drawn)
        return false;
    // The battlefield layer, or a surface that holds the canvas's pixels
    // while a painter after the fog paints on it.
    const auto& target = paint_target();
    return &target == &match_world_cpu_ ||
           (!target.rgb.empty() && target.rgb.data() == full_->canvas_pixels);
}

bool Runtime::paint_world_level(int x, int y, int width, int height, int32_t level) {
    if (!paints_full_canvas() || width <= 0 || height <= 0)
        return false;
    const full_fog::LevelQuad quad = full_fog::level_quad(level);
    if (quad.colour.alpha <= 0.0F)
        return true;
    full_->world_quads.push_back({x, y, width, height, quad.colour, quad.blend});
    // The level's colour is black or white, the same through the gamma.
    const auto level_colour = static_cast<uint8_t>(quad.colour.red > 0.0F ? 255 : 0);
    cover_canvas_paint(
        paint_target(),
        full_overlay_key(),
        full_->world_quads.back(),
        {level_colour, level_colour, level_colour}
    );
    return true;
}

bool Runtime::paint_world_blend(
    int x, int y, int width, int height, std::array<uint8_t, 3> colour, uint32_t opacity
) {
    if (!paints_full_canvas() || width <= 0 || height <= 0)
        return false;
    if (opacity == 0)
        return true;
    card::Colour shown = full::flat_colour(colour, gamma_identity_ ? nullptr : &gamma_table_);
    shown.alpha = std::min(1.0F, static_cast<float>(opacity) / blend_opacity_whole);
    full_->world_quads.push_back({x, y, width, height, shown, card::Blend::alpha});
    cover_canvas_paint(paint_target(), full_overlay_key(), full_->world_quads.back(), colour);
    return true;
}

bool Runtime::paint_world_minimum(
    int x, int y, int width, int height, std::array<uint8_t, 3> colour
) {
    if (!paints_full_canvas() || width <= 0 || height <= 0)
        return false;
    const card::Colour shown = full::flat_colour(colour, gamma_identity_ ? nullptr : &gamma_table_);
    full_->world_quads.push_back({x, y, width, height, shown, card::Blend::minimum});
    cover_canvas_paint(paint_target(), full_overlay_key(), full_->world_quads.back(), colour);
    return true;
}

void Runtime::paint_faded(
    int x, int y, int width, int height, uint32_t opacity, const std::function<void()>& paint
) {
    if (opacity == 0)
        return;
    const auto whole = static_cast<uint32_t>(blend_opacity_whole);
    if (opacity >= whole) {
        paint();
        return;
    }
    auto& target = paint_target();
    const int left = std::max(x, 0);
    const int top = std::max(y, 0);
    const int right = std::min(x + width, static_cast<int>(target.width));
    const int bottom = std::min(y + height, static_cast<int>(target.height));
    if (left >= right || top >= bottom ||
        target.rgb.size() < static_cast<std::size_t>(target.width) * target.height * 3U)
        return;
    const auto columns = static_cast<std::size_t>(right - left);
    const auto row_bytes = columns * 3U;
    const auto at = [&](int row) {
        return target.rgb.data() +
               (static_cast<std::size_t>(row) * target.width + static_cast<std::size_t>(left)) * 3U;
    };
    std::vector<uint8_t> before(row_bytes * static_cast<std::size_t>(bottom - top));
    for (int row = top; row < bottom; ++row)
        std::copy_n(at(row), row_bytes, before.data() + row_bytes * (row - top));
    const bool canvas = paints_full_canvas();
    const std::size_t first_quad = canvas ? full_->world_quads.size() : 0;
    {
        // What it paints is read back here, so no group holds its lines
        // (BackedText).
        const oa::ui::hud::TextHold::Pause painted_now(text_hold_);
        paint();
    }
    if (canvas) {
        // What the painter asked the card for, at the opacity: its blends
        // and shades fainter, the rest left out.
        auto& quads = full_->world_quads;
        const auto faint = static_cast<float>(opacity) / blend_opacity_whole;
        const auto kept = std::remove_if(
            quads.begin() + static_cast<std::ptrdiff_t>(first_quad),
            quads.end(),
            [](const FullWorldQuad& quad) {
                return quad.blend != card::Blend::alpha && quad.blend != card::Blend::darken;
            }
        );
        quads.erase(kept, quads.end());
        for (auto quad = quads.begin() + static_cast<std::ptrdiff_t>(first_quad);
             quad != quads.end();
             ++quad)
            quad->colour.alpha *= faint;
    }
    const auto key = canvas ? full_overlay_key() : std::array<uint8_t, 3>{};

    // Over the canvas's key a painted pixel is blended over the world by the
    // card, a row's run of one colour in a quad; elsewhere it is blended
    // over what the target held.
    struct Run {
        int start{};
        int length{};
        std::array<uint8_t, 3> colour{};
    };

    std::vector<Run> runs;
    for (int row = top; row < bottom; ++row) {
        uint8_t* pixel = at(row);
        const uint8_t* held = before.data() + row_bytes * (row - top);
        runs.clear();
        for (std::size_t column = 0; column < columns; ++column, pixel += 3, held += 3) {
            if (std::equal(pixel, pixel + 3, held))
                continue;
            if (canvas && std::equal(key.begin(), key.end(), held)) {
                const std::array<uint8_t, 3> colour{pixel[0], pixel[1], pixel[2]};
                std::copy(key.begin(), key.end(), pixel);
                const int column_at = left + static_cast<int>(column);
                if (!runs.empty() && runs.back().colour == colour &&
                    runs.back().start + runs.back().length == column_at)
                    ++runs.back().length;
                else
                    runs.push_back({column_at, 1, colour});
                continue;
            }
            for (std::size_t channel = 0; channel < 3; ++channel)
                pixel[channel] = static_cast<uint8_t>(
                    (held[channel] * (whole - opacity) + pixel[channel] * opacity + whole / 2) /
                    whole
                );
        }
        for (const Run& run : runs)
            std::ignore = paint_world_blend(run.start, row, run.length, 1, run.colour, opacity);
    }
}

std::vector<uint8_t> replay_world_quads(
    std::span<const FullWorldQuad> quads,
    bool minimum_composed,
    std::vector<uint8_t>& picture,
    uint32_t width,
    uint32_t height,
    int32_t origin_x,
    int32_t origin_y
) {
    std::vector<uint8_t> over(std::size_t{width} * height, 0);
    if (picture.size() < over.size() * 3U)
        return over;
    for (const FullWorldQuad& quad : quads) {
        const std::array<double, 3> colour{
            static_cast<double>(quad.colour.red) * channel_whole,
            static_cast<double>(quad.colour.green) * channel_whole,
            static_cast<double>(quad.colour.blue) * channel_whole
        };
        const auto alpha = static_cast<double>(quad.colour.alpha);
        const card::Blend blend = quad.blend == card::Blend::minimum && !minimum_composed
                                      ? card::Blend::alpha
                                      : quad.blend;
        const int64_t left = std::max<int64_t>(int64_t{origin_x} + quad.x, 0);
        const int64_t top = std::max<int64_t>(int64_t{origin_y} + quad.y, 0);
        const int64_t right =
            std::min<int64_t>(int64_t{origin_x} + quad.x + quad.width, int64_t{width});
        const int64_t bottom =
            std::min<int64_t>(int64_t{origin_y} + quad.y + quad.height, int64_t{height});
        for (int64_t y = top; y < bottom; ++y)
            for (int64_t x = left; x < right; ++x) {
                const auto at = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
                over[at] = static_cast<uint8_t>(std::min(over[at] + 1, most_counted_quads));
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    uint8_t& level = picture[at * 3U + channel];
                    const double shown = blended_level(level, colour[channel], alpha, blend);
                    level =
                        static_cast<uint8_t>(std::lround(std::clamp(shown, 0.0, channel_whole)));
                }
            }
    }
    return over;
}

} // namespace oa::app
