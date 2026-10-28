// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What Open Annihilation's own screens on the OA layer draw alike (menu_paint.hpp).

#include "menu_paint.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <span>
#include <string>

namespace oa::app::menu_paint {

namespace {

namespace kit = oa::ui::kit;
namespace artless = oa::ui::frontend_renderer;

/// The most filtered pictures a screen keeps: its pictures at each of the
/// sizes they are drawn at, which rounding may make differ by a point.
constexpr std::size_t most_fitted_art = 16;

/// How many bands a steel plate's light and shade are drawn in.
constexpr int32_t steel_bands = 8;
/// How far down a steel plate the light reaches, as a share of its height.
constexpr double steel_sheen_reach = 0.45;
/// How far up a steel plate the shade reaches, as a share of its height.
constexpr double steel_shade_reach = 0.45;
/// The light's opacity at a steel plate's top, in 256ths.
constexpr double steel_sheen_opacity = 44.0;
/// The shade's opacity at a steel plate's bottom, in 256ths.
constexpr double steel_shade_opacity = 80.0;
/// The light's opacity over a frame's top bar, in 256ths.
constexpr uint32_t rim_sheen_opacity = 34;
/// The shade's opacity over a frame's bottom bar, in 256ths.
constexpr uint32_t rim_shade_opacity = 70;
/// A glow's opacity next to what it lies round, in 256ths.
constexpr uint32_t glow_opacity = 150;
/// How many lines a frame's vent has.
constexpr int32_t vent_lines = 3;
/// The points from one line of a vent to the next.
constexpr int32_t vent_pitch = 2;

constexpr kit::Colour steel_sheen_colour{236, 240, 236};      ///< the light on steel
constexpr kit::Colour rim_groove_light_colour{104, 110, 108}; ///< a rim's groove's lit side
constexpr kit::Colour label_plate_colour{56, 61, 64};         ///< a label plate's face

/// Returns a decoded picture as the canvas draws it.
///
/// @param icon the picture
/// @return the picture's view of its pixels
artless::RgbaPicture picture_of(const WindowIcon& icon) noexcept {
    return {icon.width, icon.height, icon.pixels};
}

} // namespace

int32_t nearest(double value) noexcept {
    return static_cast<int32_t>(std::lround(value));
}

void FittedArt::draw(const kit::Canvas& canvas, const kit::Rect& rect, const WindowIcon& picture)
    const {
    const kit::Point covered = kit::covered_pixels(canvas, rect);
    if (covered.x <= 0 || covered.y <= 0 || picture.width == 0 || picture.height == 0)
        return;
    auto kept = std::find_if(fitted_.begin(), fitted_.end(), [&](const Fitted& fitted) {
        return fitted.picture == &picture && fitted.columns == covered.x &&
               fitted.rows == covered.y;
    });
    if (kept == fitted_.end()) {
        if (fitted_.size() >= most_fitted_art)
            fitted_.clear();
        Fitted fitted;
        fitted.picture = &picture;
        fitted.columns = covered.x;
        fitted.rows = covered.y;
        fitted.pixels = oa::ui::paint::resized_rgba(
            picture.pixels,
            static_cast<int>(picture.width),
            static_cast<int>(picture.height),
            covered.x,
            covered.y
        );
        fitted_.push_back(std::move(fitted));
        kept = std::prev(fitted_.end());
    }
    if (kept->pixels.empty()) {
        kit::draw_picture(canvas, rect, picture_of(picture));
        return;
    }
    kit::draw_picture(
        canvas,
        rect,
        {static_cast<uint32_t>(kept->columns), static_cast<uint32_t>(kept->rows), kept->pixels}
    );
}

void outlined_text(
    const kit::Canvas& canvas,
    kit::FontRole role,
    std::string_view text,
    const kit::Rect& box,
    kit::Align align,
    kit::Colour ink,
    int32_t tracking
) {
    constexpr std::array<kit::Point, 8> around{
        {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}
    };
    for (const kit::Point step : around)
        kit::box_text(
            canvas,
            role,
            text,
            {box.x + step.x, box.y + step.y, box.width, box.height},
            align,
            outline_colour,
            tracking
        );
    kit::box_text(canvas, role, text, box, align, ink, tracking);
}

void draw_arrow(const kit::Canvas& canvas, kit::Point tip, int32_t reach, bool left, kit::Colour ink) {
    const auto triangle = [&](int32_t shift, kit::Colour colour) {
        for (int32_t step = -reach; step <= reach; ++step) {
            const int32_t away = step < 0 ? -step : step;
            const int32_t length = reach - away + 1;
            const int32_t from = left ? tip.x + away : tip.x - away - length + 1;
            kit::fill_rect(canvas, {from + shift, tip.y + shift + step, length, 1}, colour);
        }
    };
    triangle(1, outline_colour);
    triangle(0, ink);
}

void draw_bolt(const kit::Canvas& canvas, kit::Point centre, int32_t radius) {
    if (radius < 1)
        return;
    kit::fill_round_rect(
        canvas,
        {centre.x - radius - 1, centre.y - radius - 1, 2 * radius + 2, 2 * radius + 2},
        radius + 1,
        outline_colour
    );
    kit::fill_round_rect(
        canvas, {centre.x - radius, centre.y - radius, 2 * radius, 2 * radius}, radius, bolt_colour
    );
    const int32_t light = std::max(radius / 2, 1);
    const int32_t offset = radius / 3;
    kit::fill_round_rect(
        canvas,
        {centre.x - offset - light, centre.y - offset - light, light * 2 - 1, light * 2 - 1},
        light,
        bolt_light_colour
    );
}

void draw_steel(const kit::Canvas& canvas, const kit::Rect& rect, kit::Colour face) {
    if (rect.width <= 0 || rect.height <= 0)
        return;
    kit::fill_rect(canvas, rect, face);
    if (rect.width <= 4 || rect.height <= 4)
        return;
    for (int32_t band = 0; band < steel_bands; ++band) {
        const int32_t first = rect.y + rect.height * band / steel_bands;
        const int32_t end = rect.y + rect.height * (band + 1) / steel_bands;
        const kit::Rect stripe{rect.x, first, rect.width, end - first};
        const double along = (static_cast<double>(band) + 0.5) / steel_bands;
        if (along < steel_sheen_reach)
            kit::blend_rect(
                canvas,
                stripe,
                steel_sheen_colour,
                static_cast<uint32_t>(std::lround(
                    steel_sheen_opacity * (steel_sheen_reach - along) / steel_sheen_reach
                ))
            );
        else if (along > 1.0 - steel_shade_reach)
            kit::blend_rect(
                canvas,
                stripe,
                outline_colour,
                static_cast<uint32_t>(std::lround(
                    steel_shade_opacity * (along - 1.0 + steel_shade_reach) / steel_shade_reach
                ))
            );
    }
    kit::outline_rect(canvas, rect, outline_colour);
    kit::bevel_rect(
        canvas, {rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2}, steel_light_colour,
        steel_dark_colour
    );
}

void draw_glow(const kit::Canvas& canvas, const kit::Rect& rect, kit::Colour ink, int32_t reach) {
    for (int32_t step = 1; step <= reach; ++step) {
        const auto opacity =
            static_cast<uint32_t>(glow_opacity * static_cast<uint32_t>(reach - step + 1) /
                                  static_cast<uint32_t>(reach + 1));
        const kit::Rect ring{
            rect.x - step, rect.y - step, rect.width + 2 * step, rect.height + 2 * step
        };
        kit::blend_rect(canvas, {ring.x, ring.y, ring.width, 1}, ink, opacity);
        kit::blend_rect(canvas, {ring.x, ring.y + ring.height - 1, ring.width, 1}, ink, opacity);
        kit::blend_rect(canvas, {ring.x, ring.y + 1, 1, ring.height - 2}, ink, opacity);
        kit::blend_rect(
            canvas, {ring.x + ring.width - 1, ring.y + 1, 1, ring.height - 2}, ink, opacity
        );
    }
}

void draw_panel_frame(const kit::Canvas& canvas, const kit::Rect& panel, int32_t rim) {
    if (panel.width <= 0 || panel.height <= 0)
        return;
    if (rim < 3 || panel.width <= 4 * rim || panel.height <= 4 * rim) {
        kit::blend_rect(canvas, panel, plate_colour, plate_opacity);
        kit::outline_rect(canvas, panel, plate_edge_colour);
        return;
    }
    const kit::Rect inside{
        panel.x + rim, panel.y + rim, panel.width - 2 * rim, panel.height - 2 * rim
    };
    kit::blend_rect(canvas, inside, panel_ground_colour, panel_ground_opacity);

    // The rim's four bars, the top one lit and the bottom and right ones
    // shaded.
    const kit::Rect top{panel.x, panel.y, panel.width, rim};
    const kit::Rect bottom{panel.x, panel.y + panel.height - rim, panel.width, rim};
    const kit::Rect left{panel.x, inside.y, rim, inside.height};
    const kit::Rect right{panel.x + panel.width - rim, inside.y, rim, inside.height};
    for (const kit::Rect& bar : {top, bottom, left, right})
        kit::fill_rect(canvas, bar, steel_colour);
    kit::blend_rect(canvas, top, steel_sheen_colour, rim_sheen_opacity);
    kit::blend_rect(canvas, bottom, outline_colour, rim_shade_opacity);
    kit::blend_rect(canvas, right, outline_colour, rim_shade_opacity / 2);

    kit::outline_rect(canvas, panel, outline_colour);
    kit::bevel_rect(
        canvas, {panel.x + 1, panel.y + 1, panel.width - 2, panel.height - 2}, steel_light_colour,
        steel_dark_colour
    );
    // The groove along the middle of the rim, sunk: shaded over, lit under.
    const int32_t groove = rim / 2;
    kit::bevel_rect(
        canvas,
        {panel.x + groove, panel.y + groove, panel.width - 2 * groove, panel.height - 2 * groove},
        steel_dark_colour,
        rim_groove_light_colour
    );
    // The inside, sunk into the rim.
    kit::bevel_rect(
        canvas, {inside.x - 1, inside.y - 1, inside.width + 2, inside.height + 2},
        steel_dark_colour, steel_light_colour
    );
    kit::outline_rect(canvas, inside, outline_colour);

    // A vent on each side a little in from each corner.
    const int32_t vent_height = (vent_lines - 1) * vent_pitch + 1;
    if (inside.height > 4 * rim + 2 * vent_height)
        for (const int32_t x : {left.x + 2, right.x + 2})
            for (const int32_t from :
                 {inside.y + 2 * rim, inside.y + inside.height - 2 * rim - vent_height})
                for (int32_t line = 0; line < vent_lines; ++line)
                    kit::fill_rect(
                        canvas, {x, from + line * vent_pitch, rim - 4, 1}, steel_dark_colour
                    );

    const int32_t radius = std::max(rim * 2 / 5, 1);
    const int32_t centre = rim / 2;
    for (const kit::Point corner :
         {kit::Point{panel.x + centre, panel.y + centre},
          kit::Point{panel.x + panel.width - centre - 1, panel.y + centre},
          kit::Point{panel.x + centre, panel.y + panel.height - centre - 1},
          kit::Point{panel.x + panel.width - centre - 1, panel.y + panel.height - centre - 1}})
        draw_bolt(canvas, corner, radius);
}

int32_t label_plate_width(
    const kit::Canvas& canvas, std::string_view text, int32_t height, int32_t tracking
) {
    const auto characters = static_cast<int32_t>(text.size());
    return kit::measure_text(canvas, kit::FontRole::regular, text) +
           tracking * std::max(characters - 1, 0) + 2 * height;
}

void draw_label_plate(
    const kit::Canvas& canvas, const kit::Rect& plate, std::string_view text, int32_t tracking
) {
    if (plate.width <= 0 || plate.height <= 0)
        return;
    draw_steel(canvas, plate, label_plate_colour);
    const int32_t radius = std::max(plate.height / 7, 1);
    const int32_t middle = plate.y + plate.height / 2;
    const int32_t end = plate.height / 3;
    draw_bolt(canvas, {plate.x + end, middle}, radius);
    draw_bolt(canvas, {plate.x + plate.width - end - 1, middle}, radius);
    outlined_text(
        canvas,
        kit::FontRole::regular,
        text,
        {plate.x + plate.height, plate.y, plate.width - 2 * plate.height, plate.height},
        kit::Align::left,
        caption_colour,
        tracking
    );
}

std::string worded(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

std::string filled(std::string pattern, std::string_view token, std::string_view value) {
    if (const auto at = pattern.find(token); at != std::string::npos)
        pattern.replace(at, token.size(), value);
    return pattern;
}

WindowIcon picture_of_frame(const oa::formats::gaf::Frame& frame, const oa::PaletteBytes& palette) {
    WindowIcon picture;
    auto rendered = oa::formats::gaf::render_normal(frame);
    if (!rendered.ok())
        return picture;
    const auto& drawn = *rendered.frame;
    const std::size_t pixels = static_cast<std::size_t>(drawn.width) * drawn.height;
    if (drawn.pixels.size() < pixels || drawn.coverage.size() < pixels)
        return picture;
    picture.width = drawn.width;
    picture.height = drawn.height;
    picture.pixels.assign(pixels * 4U, 0);
    for (std::size_t at = 0; at < pixels; ++at) {
        const std::size_t colour = static_cast<std::size_t>(drawn.pixels[at]) * 4U;
        if (drawn.coverage[at] == 0 || colour + 2U >= palette.size())
            continue;
        std::memcpy(picture.pixels.data() + at * 4U, palette.data() + colour, 3);
        picture.pixels[at * 4U + 3U] = 255;
    }
    return picture;
}

void draw_upright_arrow(
    const kit::Canvas& canvas, kit::Point tip, int32_t reach, bool down, kit::Colour ink
) {
    const auto triangle = [&](int32_t shift, kit::Colour colour) {
        for (int32_t step = 0; step <= reach; ++step) {
            const int32_t row = down ? tip.y - step : tip.y + step;
            kit::fill_rect(canvas, {tip.x - step + shift, row + shift, 2 * step + 1, 1}, colour);
        }
    };
    triangle(1, outline_colour);
    triangle(0, ink);
}

void draw_steel_button(const kit::Canvas& canvas, const kit::Rect& rect, kit::Colour face) {
    draw_steel(canvas, rect, face);
    // The bolts follow the shorter side, so that a tall narrow plate, such
    // as a scroll bar's thumb, keeps them small and inside it.
    const int32_t side = std::min(rect.width, rect.height);
    const int32_t radius = side / 14;
    if (radius < 1)
        return;
    const int32_t inset = std::max(side / 6, radius + 2);
    for (const kit::Point corner :
         {kit::Point{rect.x + inset, rect.y + inset},
          kit::Point{rect.x + rect.width - inset - 1, rect.y + inset},
          kit::Point{rect.x + inset, rect.y + rect.height - inset - 1},
          kit::Point{rect.x + rect.width - inset - 1, rect.y + rect.height - inset - 1}})
        draw_bolt(canvas, corner, radius);
}

void draw_sunk_edge(const kit::Canvas& canvas, const kit::Rect& rect) {
    kit::bevel_rect(canvas, rect, steel_dark_colour, sunk_light_colour);
    kit::outline_rect(
        canvas, {rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2}, outline_colour
    );
}

void draw_ring(const kit::Canvas& canvas, const kit::Rect& rect, int32_t width, kit::Colour ink) {
    for (int32_t step = 0; step < width; ++step)
        kit::outline_rect(
            canvas,
            {rect.x + step, rect.y + step, rect.width - 2 * step, rect.height - 2 * step},
            ink
        );
}

void draw_line(
    const kit::Canvas& canvas, kit::Point from, kit::Point to, int32_t width, kit::Colour ink
) {
    const int32_t steps = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
    const int32_t half = width / 2;
    for (int32_t step = 0; step <= steps; ++step) {
        const double share = steps > 0 ? static_cast<double>(step) / steps : 0.0;
        const int32_t x = from.x + nearest((to.x - from.x) * share);
        const int32_t y = from.y + nearest((to.y - from.y) * share);
        kit::fill_rect(canvas, {x - half, y - half, std::max(width, 1), std::max(width, 1)}, ink);
    }
}

void draw_circle(const kit::Canvas& canvas, const kit::Rect& box, int32_t width, kit::Colour ink) {
    const int32_t side = std::min(box.width, box.height);
    const kit::Rect square{box.x + (box.width - side) / 2, box.y + (box.height - side) / 2, side, side};
    for (int32_t step = 0; step < width; ++step)
        kit::outline_round_rect(
            canvas,
            {square.x + step, square.y + step, square.width - 2 * step, square.height - 2 * step},
            std::max(side / 2 - step, 0),
            ink
        );
}

} // namespace oa::app::menu_paint
