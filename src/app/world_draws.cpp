// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battlefield's draws of one frame, drawn band by band.
#include "world_draws.hpp"

#include "oa/present/blit.hpp"
#include "oa/present/display.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/rle.hpp"
#include "oa/present/scene_grid.hpp"
#include "oa/present/world_renderer/world_overlays.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

namespace oa::app {

namespace model_render = oa::present::model;

void put_world_pixel(
    const WorldTarget& target, int x, int y, const std::array<uint8_t, 3>& color
) noexcept {
    if (x < target.clip_x || y < target.clip_y || x >= target.clip_x + target.clip_width ||
        y >= target.clip_y + target.clip_height)
        return;
    if (x < 0 || y < 0 || x >= target.width || y >= target.height)
        return;
    if (y < target.first_row || y >= target.end_row)
        return;
    const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(target.width) +
                     static_cast<std::size_t>(x)) *
                    3U;
    target.rgb[at] = color[0];
    target.rgb[at + 1] = color[1];
    target.rgb[at + 2] = color[2];
}

void draw_world_line(
    const WorldTarget& target, int x0, int y0, int x1, int y1, const std::array<uint8_t, 3>& color
) noexcept {
    // Clip the segment to the visible world rectangle before stepping it, so
    // off-map endpoints (e.g. a build ghost under an off-map cursor) cannot
    // produce billion-step walks or overflow the differences below. The band
    // takes no part: the line's pixels are those of the whole frame.
    const auto left = static_cast<int64_t>(std::max(target.clip_x, 0));
    const auto top = static_cast<int64_t>(std::max(target.clip_y, 0));
    const auto right =
        std::min<int64_t>(static_cast<int64_t>(target.clip_x) + target.clip_width, target.width) -
        1;
    const auto bottom =
        std::min<int64_t>(static_cast<int64_t>(target.clip_y) + target.clip_height, target.height) -
        1;
    if (right < left || bottom < top)
        return;
    double ax = x0, ay = y0, bx = x1, by = y1;
    const auto outcode = [&](double x, double y) {
        return (x < left ? 1 : 0) | (x > right ? 2 : 0) | (y < top ? 4 : 0) | (y > bottom ? 8 : 0);
    };
    for (int code_a = outcode(ax, ay), code_b = outcode(bx, by);;) {
        if ((code_a | code_b) == 0)
            break;
        if ((code_a & code_b) != 0)
            return;
        const int code = code_a != 0 ? code_a : code_b;
        double x = 0, y = 0;
        if (code & 8) {
            x = ax + (bx - ax) * (bottom - ay) / (by - ay);
            y = static_cast<double>(bottom);
        } else if (code & 4) {
            x = ax + (bx - ax) * (top - ay) / (by - ay);
            y = static_cast<double>(top);
        } else if (code & 2) {
            y = ay + (by - ay) * (right - ax) / (bx - ax);
            x = static_cast<double>(right);
        } else {
            y = ay + (by - ay) * (left - ax) / (bx - ax);
            x = static_cast<double>(left);
        }
        if (code == code_a) {
            ax = x;
            ay = y;
            code_a = outcode(ax, ay);
        } else {
            bx = x;
            by = y;
            code_b = outcode(bx, by);
        }
    }
    x0 = static_cast<int>(std::lround(ax));
    y0 = static_cast<int>(std::lround(ay));
    x1 = static_cast<int>(std::lround(bx));
    y1 = static_cast<int>(std::lround(by));
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        put_world_pixel(target, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        const auto twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void blit_world_frame(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    int destination_x,
    int destination_y,
    const oa::PaletteBytes& palette,
    float scale,
    uint32_t shadow_level
) noexcept {
    if (scale <= 0.0F)
        scale = 1.0F;
    const auto dest_w =
        std::max(1, static_cast<int>(std::lround(static_cast<double>(frame.width) * scale)));
    const auto dest_h =
        std::max(1, static_cast<int>(std::lround(static_cast<double>(frame.height) * scale)));
    // The rows and columns of the drawn rectangle that land in the band, the
    // visible world rectangle and the frame; each one's source row or column
    // is its index times the frame's size over the drawn size, rounded down.
    const auto lowest = [](int64_t a, int64_t b, int64_t c) { return std::max({a, b, c}); };
    const auto highest = [](int64_t a, int64_t b, int64_t c) { return std::min({a, b, c}); };
    const int64_t first_row =
        std::max<int64_t>(0, lowest(target.first_row, target.clip_y, 0) - int64_t{destination_y});
    const int64_t end_row = std::min<int64_t>(
        dest_h,
        highest(target.end_row, int64_t{target.clip_y} + target.clip_height, target.height) -
            int64_t{destination_y}
    );
    const int64_t first_column =
        std::max<int64_t>(0, lowest(target.clip_x, 0, 0) - int64_t{destination_x});
    const int64_t end_column = std::min<int64_t>(
        dest_w,
        highest(int64_t{target.clip_x} + target.clip_width, target.width, target.width) -
            int64_t{destination_x}
    );
    if (first_row >= end_row || first_column >= end_column)
        return;
    const auto source_width = static_cast<std::size_t>(frame.width);
    const auto source_pixels = source_width * frame.height;
    const uint8_t* const coverage = frame.coverage.data();
    const uint8_t* const pixels = frame.pixels.data();
    // A frame whose pixels or coverage are short is read only where they reach.
    const std::size_t readable =
        std::min({frame.coverage.size(), frame.pixels.size(), source_pixels});
    const auto columns = static_cast<std::size_t>(dest_w);
    for (int64_t row = first_row; row < end_row; ++row) {
        const auto source_row =
            static_cast<std::size_t>(row) * frame.height / static_cast<std::size_t>(dest_h);
        const std::size_t row_offset = source_row * source_width;
        uint8_t* out = target.rgb + ((static_cast<std::size_t>(destination_y + row) *
                                          static_cast<std::size_t>(target.width) +
                                      static_cast<std::size_t>(destination_x + first_column)) *
                                     3U);
        // The source column of first_column, kept as a whole part and a
        // remainder over the drawn width while the column steps.
        const std::size_t start = static_cast<std::size_t>(first_column) * source_width;
        std::size_t source_column = start / columns;
        std::size_t remainder = start % columns;
        for (int64_t column = first_column; column < end_column; ++column, out += 3) {
            const std::size_t offset = row_offset + source_column;
            remainder += source_width;
            while (remainder >= columns) {
                remainder -= columns;
                ++source_column;
            }
            if (offset >= readable || coverage[offset] == 0)
                continue;
            const auto pal = static_cast<std::size_t>(pixels[offset]) * 4U;
            for (std::size_t channel = 0; channel < 3; ++channel)
                out[channel] = model_render::fade_shadow_channel(
                    out[channel], palette[pal + channel], shadow_level
                );
        }
    }
}

void blit_world_hotspot(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& screen,
    const oa::PaletteBytes& palette,
    float scale,
    uint32_t shadow_level
) noexcept {
    if (scale <= 0.0F)
        scale = 1.0F;
    const auto x =
        screen.x - static_cast<int>(
                       std::lround(static_cast<double>(frame.origin_x) * static_cast<double>(scale))
                   );
    const auto y =
        screen.y - static_cast<int>(
                       std::lround(static_cast<double>(frame.origin_y) * static_cast<double>(scale))
                   );
    blit_world_frame(target, frame, x, y, palette, scale, shadow_level);
}

namespace {

/// Visits the frame pixels a GAF frame covers on the scene grid, within the
/// visible world rectangle, the frame and the band: the frame's column
/// (or row) `i` lies on the map pixel `place - origin + i` from the scene's
/// first, and covers the scene pixels the grid gives that map pixel at the
/// scale's step (oa/present/scene_grid.hpp), as the terrain under it does.
///
/// @param target the frame and what may be written
/// @param frame the GAF frame
/// @param place where the frame's origin lies, in map pixels from the scene's first
/// @param scale scene pixels per map pixel; 0 or less draws at 1
/// @param visit called with each pixel's offset in the GAF frame, row by
///     row, and the RGB pixel it covers
template <typename Visit>
void visit_sprite_pixels(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& place,
    float scale,
    Visit&& visit
) {
    const uint32_t step = oa::present::scene_step(scale);
    const uint32_t phase_x = target.phase_x % step;
    const uint32_t phase_y = target.phase_y % step;
    const int64_t left = int64_t{place.x} - frame.origin_x;
    const int64_t top = int64_t{place.y} - frame.origin_y;
    const int64_t first_row = std::max<int64_t>(
        {oa::present::first_scene_pixel(top, step, phase_y), target.first_row, target.clip_y, 0}
    );
    const int64_t end_row = std::min<int64_t>(
        {oa::present::first_scene_pixel(top + frame.height, step, phase_y),
         target.end_row,
         int64_t{target.clip_y} + target.clip_height,
         target.height}
    );
    const int64_t first_column =
        std::max<int64_t>({oa::present::first_scene_pixel(left, step, phase_x), target.clip_x, 0});
    const int64_t end_column = std::min<int64_t>(
        {oa::present::first_scene_pixel(left + frame.width, step, phase_x),
         int64_t{target.clip_x} + target.clip_width,
         target.width}
    );
    if (first_row >= end_row || first_column >= end_column)
        return;
    const auto source_width = static_cast<std::size_t>(frame.width);
    // The map pixel the first column shows, and how far into it the column
    // lies, in 16.16 parts of the step, kept while the column steps.
    const int64_t first_shown = oa::present::map_pixel_shown(first_column, step, phase_x);
    const int64_t first_into = first_column * int64_t{oa::present::scene_step_one} +
                               int64_t{phase_x} - first_shown * int64_t{step};
    for (int64_t row = first_row; row < end_row; ++row) {
        const auto source_row =
            static_cast<std::size_t>(oa::present::map_pixel_shown(row, step, phase_y) - top);
        const std::size_t row_offset = source_row * source_width;
        uint8_t* out =
            target.rgb + ((static_cast<std::size_t>(row) * static_cast<std::size_t>(target.width) +
                           static_cast<std::size_t>(first_column)) *
                          3U);
        auto source_column = static_cast<std::size_t>(first_shown - left);
        int64_t into = first_into;
        for (int64_t column = first_column; column < end_column; ++column, out += 3) {
            visit(row_offset + source_column, out);
            into += oa::present::scene_step_one;
            while (into >= step) {
                into -= step;
                ++source_column;
            }
        }
    }
}

} // namespace

void blit_world_sprite(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& place,
    const oa::PaletteBytes& palette,
    float scale,
    uint32_t shadow_level
) noexcept {
    // A frame whose pixels or coverage are short is read only where they reach.
    const std::size_t readable = std::min(
        {frame.coverage.size(),
         frame.pixels.size(),
         static_cast<std::size_t>(frame.width) * frame.height}
    );
    visit_sprite_pixels(target, frame, place, scale, [&](std::size_t offset, uint8_t* out) {
        if (offset >= readable || frame.coverage[offset] == 0)
            return;
        const auto pal = static_cast<std::size_t>(frame.pixels[offset]) * 4U;
        for (std::size_t channel = 0; channel < 3; ++channel)
            out[channel] = model_render::fade_shadow_channel(
                out[channel], palette[pal + channel], shadow_level
            );
    });
}

void blit_world_blended_hotspot(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& place,
    float scale,
    const model_render::ModelDisplay& display,
    model_render::RgbBridge& bridge,
    model_render::BridgeBand* band,
    uint32_t shadow_level
) {
    const auto& context = display.context;
    if ((context.flags & oa::present::display_flag_alpha_table) == 0)
        return;
    const auto& palette = display.palette;
    const std::size_t readable = std::min(frame.coverage.size(), frame.pixels.size());
    visit_sprite_pixels(target, frame, place, scale, [&](std::size_t offset, uint8_t* pixel) {
        if (offset >= readable || frame.coverage[offset] == 0)
            return;
        const uint8_t under =
            band != nullptr
                ? model_render::bridge_index(bridge, *band, pixel[0], pixel[1], pixel[2])
                : model_render::bridge_index(bridge, pixel[0], pixel[1], pixel[2]);
        const auto& blended =
            palette.entries[context.alpha_table
                                [static_cast<std::size_t>(frame.pixels[offset]) * 0x100 + under]];
        pixel[0] = model_render::fade_shadow_channel(pixel[0], blended.r, shadow_level);
        pixel[1] = model_render::fade_shadow_channel(pixel[1], blended.g, shadow_level);
        pixel[2] = model_render::fade_shadow_channel(pixel[2], blended.b, shadow_level);
    });
}

void blit_world_lit_hotspot(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& place,
    float scale,
    const uint8_t* light_table,
    const model_render::ModelDisplay& display,
    model_render::RgbBridge& bridge,
    model_render::BridgeBand* band,
    FlashStrength strength
) {
    if (light_table == nullptr)
        return;
    const auto& palette = display.palette;
    const uint32_t row_shift = strength == FlashStrength::reduced ? 1U : 0U;
    const std::size_t readable = std::min(frame.coverage.size(), frame.pixels.size());
    visit_sprite_pixels(target, frame, place, scale, [&](std::size_t offset, uint8_t* pixel) {
        if (offset >= readable || frame.coverage[offset] == 0)
            return;
        const uint8_t value = frame.pixels[offset];
        if (value == frame.transparency_index || value < oa::present::shade_ramp_base)
            return;
        const auto row_named = static_cast<uint32_t>(value - oa::present::shade_ramp_base);
        if (row_named >= static_cast<uint32_t>(oa::present::ramp_table_rows))
            return;
        const uint8_t under =
            band != nullptr
                ? model_render::bridge_index(bridge, *band, pixel[0], pixel[1], pixel[2])
                : model_render::bridge_index(bridge, pixel[0], pixel[1], pixel[2]);
        const auto& lit =
            palette.entries
                [light_table[static_cast<std::size_t>(row_named >> row_shift) * 0x100 + under]];
        pixel[0] = lit.r;
        pixel[1] = lit.g;
        pixel[2] = lit.b;
    });
}

namespace {

/// Where one map pixel of a projectile's lens takes its colour from: the
/// pixel of the lens's square it reads, across and down; a pixel the lens
/// leaves as it is reads none.
struct LensSource {
    int8_t x{};
    int8_t y{};
    bool moved{}; ///< the pixel reads another
};

/// The sources of every pixel of a lens's square, row by row.
using LensSources =
    std::array<LensSource, static_cast<std::size_t>(projectile_lens_side) * projectile_lens_side>;

/// Returns the sources of a projectile's lens, read from the lens table
/// the first time they are asked for.
///
/// @return the sources, row by row
const LensSources& projectile_lens_sources() {
    static const LensSources sources = [] {
        LensSources read{};
        const auto lens = oa::present::build_lens_frame(
            projectile_lens_side, projectile_lens_side, projectile_lens_strength
        );
        if (lens.sprite.data == nullptr)
            return read;
        constexpr int32_t cells = projectile_lens_side * projectile_lens_side;
        for (int32_t cell = 0; cell < cells; ++cell) {
            const uint16_t entry = oa::present::lens_offset(lens.sprite, cell);
            if (entry == oa::present::lens_outside)
                continue;
            // An entry is the step, in pixels of the square's rows, to the
            // pixel read.
            const int32_t source = cell + static_cast<int16_t>(entry);
            if (source == cell || source < 0 || source >= cells)
                continue;
            read[static_cast<std::size_t>(cell)] = {
                static_cast<int8_t>(source % projectile_lens_side),
                static_cast<int8_t>(source / projectile_lens_side),
                true
            };
        }
        return read;
    }();
    return sources;
}

} // namespace

oa::present::world_renderer::ScreenPoint project_world_point(
    const oa::present::world_renderer::BattlefieldViewport& viewport,
    const std::array<uint32_t, 3>& position,
    HeightLift lift
) noexcept {
    const auto map_x = static_cast<int32_t>(position[0]) >> 16;
    const auto map_z = static_cast<int32_t>(position[2]) >> 16;
    const auto height = static_cast<int32_t>(position[1]) >> 16;
    auto screen = oa::present::world_renderer::map_pixel_to_screen(viewport, {map_x, map_z});
    const auto scale = viewport.scale == 0.0F ? 1.0F : viewport.scale;
    if (lift == HeightLift::down) {
        const int32_t half = height >> 1;
        screen.y -= scale == 1.0F
                        ? half
                        : static_cast<int32_t>(
                              std::lround(static_cast<double>(half) * static_cast<double>(scale))
                          );
        return screen;
    }
    // At the whole scale the lift is half the height rounded half away from
    // zero, worked out in whole numbers.
    screen.y -= scale == 1.0F ? (height >= 0 ? (height + 1) / 2 : -((1 - height) / 2))
                              : static_cast<int32_t>(std::lround(
                                    static_cast<double>(height) * 0.5 * static_cast<double>(scale)
                                ));
    return screen;
}

SceneRect sprite_scene_rect(
    const SpriteDraw& sprite, float scale, uint32_t phase_x, uint32_t phase_y
) noexcept {
    const uint32_t step = oa::present::scene_step(scale);
    phase_x %= step;
    phase_y %= step;
    const int64_t left = int64_t{sprite.place.x} - sprite.frame->origin_x;
    const int64_t top = int64_t{sprite.place.y} - sprite.frame->origin_y;
    return {
        oa::present::first_scene_pixel(left, step, phase_x),
        oa::present::first_scene_pixel(top, step, phase_y),
        oa::present::first_scene_pixel(left + sprite.frame->width, step, phase_x),
        oa::present::first_scene_pixel(top + sprite.frame->height, step, phase_y)
    };
}

oa::present::world_renderer::ScreenPoint place_world_point(
    const oa::present::world_renderer::BattlefieldViewport& viewport,
    const std::array<uint32_t, 3>& position,
    HeightLift lift
) noexcept {
    auto unscaled = viewport;
    unscaled.scale = 1.0F;
    return project_world_point(unscaled, position, lift);
}

bool projectile_lens_on_battlefield(
    const oa::sim::effect_particles::ExplosionView& view, const std::array<uint32_t, 3>& position
) noexcept {
    // The whole map pixels of a 16.16 value, as a signed 16-bit value.
    const auto whole = [](uint32_t fixed) {
        return static_cast<int32_t>(static_cast<int16_t>(fixed >> 16));
    };
    const int32_t x = whole(position[0]) - static_cast<int16_t>(view.camera_x) +
                      oa::sim::effect_particles::battlefield_screen_x;
    const int32_t y = whole(position[2]) - static_cast<int16_t>(view.camera_y) -
                      (whole(position[1]) >> 1) + oa::sim::effect_particles::battlefield_screen_y;
    return x >= view.battlefield.x1 && x <= view.battlefield.x2 && y >= view.battlefield.y1 &&
           y <= view.battlefield.y2;
}

void draw_world_lens(
    const WorldTarget& target, const oa::present::world_renderer::ScreenPoint& screen, float scale
) {
    if (scale <= 0.0F)
        scale = 1.0F;
    const LensSources& sources = projectile_lens_sources();
    constexpr int32_t side = projectile_lens_side;
    const auto scaled = [scale](int32_t pixels) {
        return static_cast<int32_t>(
            std::lround(static_cast<double>(pixels) * static_cast<double>(scale))
        );
    };
    const int32_t left = screen.x - scaled(side / 2);
    const int32_t top = screen.y - scaled(side / 2);
    const int32_t drawn = std::max(1, scaled(side));
    // The square's pixels in the visible world rectangle and the frame,
    // which the lens reads; of them it writes those in the band.
    const int32_t read_left = std::max({left, target.clip_x, 0});
    const int32_t read_top = std::max({top, target.clip_y, 0});
    const int32_t read_right = static_cast<int32_t>(std::min<int64_t>(
        {int64_t{left} + drawn, int64_t{target.clip_x} + target.clip_width, target.width}
    ));
    const int32_t read_bottom = static_cast<int32_t>(std::min<int64_t>(
        {int64_t{top} + drawn, int64_t{target.clip_y} + target.clip_height, target.height}
    ));
    const int32_t first_row = std::max(read_top, target.first_row);
    const int32_t end_row = std::min(read_bottom, target.end_row);
    if (read_left >= read_right || first_row >= end_row)
        return;
    // The pixels read, as they stood before the lens.
    const auto columns = static_cast<std::size_t>(read_right - read_left);
    const auto row_bytes = static_cast<std::size_t>(target.width) * 3U;
    std::vector<uint8_t> before(columns * static_cast<std::size_t>(read_bottom - read_top) * 3U);
    for (int32_t y = read_top; y < read_bottom; ++y)
        std::copy_n(
            target.rgb + static_cast<std::size_t>(y) * row_bytes +
                static_cast<std::size_t>(read_left) * 3U,
            columns * 3U,
            before.begin() +
                static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y - read_top) * columns * 3U)
        );
    for (int32_t y = first_row; y < end_row; ++y) {
        const auto cell_y = static_cast<int32_t>(int64_t{y - top} * side / drawn);
        for (int32_t x = read_left; x < read_right; ++x) {
            const auto cell_x = static_cast<int32_t>(int64_t{x - left} * side / drawn);
            const LensSource& source =
                sources[static_cast<std::size_t>(cell_y) * side + static_cast<std::size_t>(cell_x)];
            if (!source.moved)
                continue;
            const int32_t from_x = x + scaled(source.x - cell_x);
            const int32_t from_y = y + scaled(source.y - cell_y);
            if (from_x < read_left || from_x >= read_right || from_y < read_top ||
                from_y >= read_bottom)
                continue;
            const auto* from =
                before.data() + (static_cast<std::size_t>(from_y - read_top) * columns +
                                 static_cast<std::size_t>(from_x - read_left)) *
                                    3U;
            std::copy_n(
                from,
                3,
                target.rgb + static_cast<std::size_t>(y) * row_bytes +
                    static_cast<std::size_t>(x) * 3U
            );
        }
    }
}

void set_frame_shadows(
    WorldDrawList& list,
    model_render::ModelRenderer& renderer,
    model_render::ShadowTable& table,
    const model_render::ModelDisplay& display,
    const oa::Sprite* projectile_shadow,
    float zoom
) {
    list.shadow_level = model_render::shadow_level(zoom);
    renderer.shadow_table = nullptr;
    if (list.shadow_level == 0) {
        renderer.graphics_flags =
            static_cast<uint16_t>(renderer.graphics_flags & ~model_render::graphics_shadows);
        return;
    }
    // The models' silhouettes are drawn in colour 0; the projectiles'
    // shadow sprite in its own colours.
    std::array<bool, 256> colours{};
    colours[0] = true;
    model_render::note_shadow_colours(projectile_shadow, colours);
    renderer.shadow_table = table.prepare(display, list.shadow_level, colours);
}

void clear_world_draws(WorldDrawList& list) {
    list.draws.clear();
    list.squares.clear();
    list.spatial_particles.clear();
    list.sprites.clear();
    list.lines.clear();
    list.models.clear();
    list.stand_ins.clear();
    list.projectiles.clear();
    list.debris.clear();
    list.fragments.clear();
    list.lenses.clear();
    list.decoded.clear();
    list.decoded_of.clear();
    list.held.clear();
}

const oa::formats::gaf::RenderedFrame*
decoded_frame(WorldDrawList& list, const oa::formats::gaf::Frame& frame) {
    if (const auto found = list.decoded_of.find(&frame); found != list.decoded_of.end())
        return found->second;
    auto rendered = oa::formats::gaf::render_normal(frame);
    const oa::formats::gaf::RenderedFrame* decoded = nullptr;
    if (rendered.ok()) {
        list.decoded.push_back(std::move(*rendered.frame));
        decoded = &list.decoded.back();
    }
    list.decoded_of.emplace(&frame, decoded);
    return decoded;
}

bool draws_frame_by_frame(const oa::formats::gaf::Archive& checked, uint64_t threshold) noexcept {
    return oa::formats::gaf::decoded_bytes(checked) > threshold;
}

GafFrameCache::GafFrameCache(std::size_t budget) noexcept : budget_bytes_(budget) {
}

std::shared_ptr<const oa::formats::gaf::RenderedFrame>
GafFrameCache::find(const oa::formats::gaf::Frame* frame) {
    const auto found = entries_.find(frame);
    if (found == entries_.end())
        return nullptr;
    recent_.splice(recent_.begin(), recent_, found->second);
    return found->second->rendered;
}

bool GafFrameCache::keep(
    const oa::formats::gaf::Frame* frame,
    std::shared_ptr<const oa::formats::gaf::RenderedFrame> rendered
) {
    if (const auto found = entries_.find(frame); found != entries_.end()) {
        kept_bytes_ -= found->second->bytes;
        recent_.erase(found->second);
        entries_.erase(found);
    }
    if (rendered == nullptr)
        return false;
    const std::size_t bytes = rendered->pixels.size() + rendered->coverage.size();
    if (bytes > budget_bytes_)
        return false;
    // The kept bytes never pass the budget, so the room left cannot wrap.
    while (bytes > budget_bytes_ - kept_bytes_ && !recent_.empty()) {
        kept_bytes_ -= recent_.back().bytes;
        entries_.erase(recent_.back().frame);
        recent_.pop_back();
    }
    recent_.push_front({frame, std::move(rendered), bytes});
    entries_[frame] = recent_.begin();
    kept_bytes_ += bytes;
    return true;
}

void GafFrameCache::mark_failed(const oa::formats::gaf::Frame* frame) {
    failed_.insert(frame);
}

bool GafFrameCache::failed(const oa::formats::gaf::Frame* frame) const {
    return failed_.count(frame) != 0;
}

void GafFrameCache::clear() noexcept {
    recent_.clear();
    entries_.clear();
    failed_.clear();
    kept_bytes_ = 0;
}

const oa::formats::gaf::RenderedFrame* ranged_frame(
    WorldDrawList& list,
    GafFrameCache& cache,
    const oa::formats::gaf::Frame& frame,
    const oa::formats::gaf::ReadHooks& reader,
    std::optional<oa::formats::gaf::Error>* failure
) {
    if (const auto found = list.decoded_of.find(&frame); found != list.decoded_of.end())
        return found->second;
    if (cache.failed(&frame)) {
        list.decoded_of.emplace(&frame, nullptr);
        return nullptr;
    }
    auto rendered = cache.find(&frame);
    if (rendered == nullptr) {
        auto result = oa::formats::gaf::render_ranged(frame, reader);
        if (!result.ok()) {
            cache.mark_failed(&frame);
            if (failure != nullptr)
                *failure = std::move(result.error);
            list.decoded_of.emplace(&frame, nullptr);
            return nullptr;
        }
        rendered =
            std::make_shared<const oa::formats::gaf::RenderedFrame>(std::move(*result.frame));
        (void)cache.keep(&frame, rendered);
    }
    list.held.push_back(rendered);
    list.decoded_of.emplace(&frame, rendered.get());
    return rendered.get();
}

void add_world_draw(WorldDrawList& list, WorldDrawKind kind, std::size_t index) {
    list.draws.push_back({kind, static_cast<uint32_t>(index)});
}

namespace {

/// Returns a target whose clip is narrowed to an area of the frame.
///
/// @param target the frame and what may be written
/// @param area the area
/// @return the target, its clip the part of its own within the area; no
///     columns or no rows when they do not meet
WorldTarget within_area(const WorldTarget& target, const FrameArea& area) noexcept {
    const int64_t left = std::max<int64_t>(target.clip_x, area.left);
    const int64_t top = std::max<int64_t>(target.clip_y, area.top);
    const int64_t right = std::min<int64_t>(int64_t{target.clip_x} + target.clip_width, area.right);
    const int64_t bottom =
        std::min<int64_t>(int64_t{target.clip_y} + target.clip_height, area.bottom);
    WorldTarget narrowed = target;
    narrowed.clip_x = static_cast<int32_t>(left);
    narrowed.clip_y = static_cast<int32_t>(top);
    narrowed.clip_width = static_cast<int32_t>(std::max<int64_t>(0, right - left));
    narrowed.clip_height = static_cast<int32_t>(std::max<int64_t>(0, bottom - top));
    return narrowed;
}

} // namespace

void draw_world_band(
    const WorldDrawList& list,
    const WorldFrameDraw& frame,
    model_render::BridgeBand& band,
    model_render::ModelRenderer& renderer,
    model_render::SupersampleScratch& supersample,
    std::vector<oa::formats::objects3d::FixedVector3>& debris_points
) {
    // The models' display, on the thread drawing the band, for as long as it draws.
    struct DisplayBinding {
        oa::present::DisplayContext* previous{};

        explicit DisplayBinding(oa::present::DisplayContext* bound)
            : previous(oa::present::display_context()) {
            oa::present::bind_display(bound);
        }

        DisplayBinding(const DisplayBinding&) = delete;
        DisplayBinding& operator=(const DisplayBinding&) = delete;

        ~DisplayBinding() { oa::present::bind_display(previous); }
    } binding{&frame.display->context};

    model_render::RgbBridge& bridge = *frame.bridge;
    WorldTarget target = frame.target;
    target.first_row = band.frame_first_row;
    target.end_row = band.frame_end_row;
    // A feature's frames draw within the pixels that show the map.
    const WorldTarget on_map = within_area(target, frame.shown_map);
    oa::Surface* surface = &band.surface;
    // The bridge holds draws not yet written back to the frame: sprites and
    // squares draw straight into the frame, so the bridge's pixels go back
    // to the frame before one draws over them.
    bool bridge_holds_draws = false;
    for (const WorldDraw& draw : list.draws) {
        switch (draw.kind) {
        case WorldDrawKind::commit:
            if (bridge_holds_draws)
                model_render::bridge_end(bridge, band);
            bridge_holds_draws = false;
            break;
        case WorldDrawKind::commit_always:
            model_render::bridge_end(bridge, band);
            bridge_holds_draws = false;
            break;
        case WorldDrawKind::pixel_square: {
            const SquareDraw& square = list.squares[draw.index];
            const int32_t top = std::max(square.top, target.first_row);
            const int32_t bottom = std::min(square.bottom, target.end_row);
            for (int32_t y = top; y < bottom; ++y)
                for (int32_t x = square.left; x < square.right; ++x)
                    put_world_pixel(target, x, y, square.color);
            break;
        }
        case WorldDrawKind::sprite: {
            const SpriteDraw& sprite = list.sprites[draw.index];
            if (sprite.shadow && !shadows_drawn(list))
                break;
            blit_world_sprite(
                sprite.on_map ? on_map : target,
                *sprite.frame,
                sprite.place,
                *frame.palette,
                frame.scale,
                sprite.shadow ? list.shadow_level : model_render::shadow_full_level
            );
            break;
        }
        case WorldDrawKind::blended_sprite: {
            const SpriteDraw& sprite = list.sprites[draw.index];
            if (sprite.shadow && !shadows_drawn(list))
                break;
            blit_world_blended_hotspot(
                sprite.on_map ? on_map : target,
                *sprite.frame,
                sprite.place,
                frame.scale,
                *frame.display,
                bridge,
                &band,
                sprite.shadow ? list.shadow_level : model_render::shadow_full_level
            );
            break;
        }
        case WorldDrawKind::lit_sprite: {
            const SpriteDraw& sprite = list.sprites[draw.index];
            blit_world_lit_hotspot(
                target,
                *sprite.frame,
                sprite.place,
                frame.scale,
                frame.light_table,
                *frame.display,
                bridge,
                &band,
                list.flash_strength
            );
            break;
        }
        case WorldDrawKind::line: {
            // A thick line is the line drawn again at each offset of a
            // square pen, centred where the thickness allows.
            const LineDraw& line = list.lines[draw.index];
            const int32_t thickness = std::max(frame.line_thickness, 1);
            const int32_t first = -(thickness - 1) / 2;
            for (int32_t down = first; down < first + thickness; ++down)
                for (int32_t across = first; across < first + thickness; ++across)
                    draw_world_line(
                        target,
                        line.x0 + across,
                        line.y0 + down,
                        line.x1 + across,
                        line.y1 + down,
                        line.color
                    );
            break;
        }
        case WorldDrawKind::selection_line: {
            // Each line captures the tiles it crosses before drawing, with
            // its thickness.
            const LineDraw& line = list.lines[draw.index];
            const int32_t thickness = std::max(frame.bridge_line_thickness, 1);
            const int32_t first = -(thickness - 1) / 2;
            const int32_t last = first + thickness - 1;
            model_render::bridge_open(
                bridge,
                band,
                {std::min(line.x0, line.x1) + first,
                 std::min(line.y0, line.y1) + first,
                 std::max(line.x0, line.x1) + last,
                 std::max(line.y0, line.y1) + last}
            );
            for (int32_t down = first; down <= last; ++down)
                for (int32_t across = first; across <= last; ++across)
                    oa::present::draw_clipped_line(
                        surface,
                        line.x0 + across,
                        line.y0 + down,
                        line.x1 + across,
                        line.y1 + down,
                        line.palette_index
                    );
            break;
        }
        case WorldDrawKind::model: {
            const ModelDraw& model = list.models[draw.index];
            model_render::ModelRef drawn = model.model;
            if (model.stand_in >= 0)
                drawn.unit = &list.stand_ins[static_cast<std::size_t>(model.stand_in)];
            model_render::draw_planned_unit(
                renderer, bridge, &band, supersample, drawn, model.plan, bridge_holds_draws
            );
            break;
        }
        case WorldDrawKind::projectile: {
            const ProjectileDraw& shot = list.projectiles[draw.index];
            model_render::bridge_open(bridge, band, shot.region);
            if (shot.shadow && shadows_drawn(list)) {
                if (renderer.shadow_table != nullptr)
                    oa::present::draw_sprite_blended_through(
                        surface,
                        frame.projectile_shadow,
                        shot.x,
                        shot.shadow_y,
                        renderer.shadow_table
                    );
                else
                    oa::present::draw_sprite_blended(
                        surface, frame.projectile_shadow, shot.x, shot.shadow_y
                    );
            }
            model_render::draw_projectile_model(
                renderer, surface, shot.position, *shot.object, *shot.prepared, shot.rotation
            );
            if (shot.child != nullptr)
                model_render::draw_projectile_model(
                    renderer,
                    surface,
                    shot.position,
                    *shot.child,
                    *shot.child_prepared,
                    shot.child_rotation
                );
            break;
        }
        case WorldDrawKind::debris: {
            const DebrisDraw& piece = list.debris[draw.index];
            model_render::bridge_open(bridge, band, piece.region);
            model_render::draw_rotated_debris(
                renderer,
                surface,
                frame.debris_view,
                *piece.object,
                *piece.prepared,
                piece.spin,
                piece.origin,
                piece.team,
                debris_points
            );
            model_render::bridge_end(bridge, band);
            break;
        }
        case WorldDrawKind::fragment: {
            const FragmentDraw& fragment = list.fragments[draw.index];
            model_render::bridge_open(bridge, band, fragment.region);
            model_render::draw_shatter_fragment(
                renderer,
                surface,
                fragment.position,
                *fragment.fragment,
                *fragment.primitive,
                fragment.spin
            );
            model_render::bridge_end(bridge, band);
            break;
        }
        case WorldDrawKind::lens:
            draw_world_lens(target, list.lenses[draw.index], frame.scale);
            break;
        }
    }
}

} // namespace oa::app
