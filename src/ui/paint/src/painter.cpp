// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Painting for the touch controls' layer (painter.hpp). Every shape is
// blended by how much of each pixel it covers, judged from the distance of
// the pixel's centre to the shape's edge.
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace oa::ui::paint {

namespace text_font = oa::platform::text_font;

namespace {

/// The ellipsis that ends shortened text.
constexpr std::string_view ellipsis = "\xE2\x80\xA6";

/// Returns the share of a pixel a shape covers from the signed distance of
/// the pixel's centre to the shape's edge: a one-pixel ramp across the edge.
///
/// @param distance pixels; negative inside
/// @return coverage, 0 to 255
uint32_t coverage_of(float distance) noexcept {
    const float covered = std::clamp(0.5F - distance, 0.0F, 1.0F);
    return static_cast<uint32_t>(std::lround(covered * 255.0F));
}

/// Returns the length of a vector.
///
/// @param x across
/// @param y down
/// @return its length
float length(float x, float y) noexcept {
    return std::sqrt(x * x + y * y);
}

/// Returns the signed distance to a rectangle with rounded corners.
///
/// @param x the point's column
/// @param y the point's row
/// @param area the rectangle
/// @param radius the corners' radius
/// @return pixels; negative inside
float rounded_rect_distance(float x, float y, Area area, float radius) noexcept {
    const float half_w = area.width * 0.5F;
    const float half_h = area.height * 0.5F;
    const float r = std::clamp(radius, 0.0F, std::min(half_w, half_h));
    const float qx = std::fabs(x - (area.x + half_w)) - (half_w - r);
    const float qy = std::fabs(y - (area.y + half_h)) - (half_h - r);
    const float outside = length(std::max(qx, 0.0F), std::max(qy, 0.0F));
    const float inside = std::min(std::max(qx, qy), 0.0F);
    return outside + inside - r;
}

/// A wedge between two directions around a centre, unbounded in radius,
/// with the sines and cosines its distance needs worked out once.
struct Wedge {
    float middle_cos{}; ///< cosine of the middle's direction
    float middle_sin{}; ///< sine of the middle's direction
    float edge_cos{};   ///< cosine of half the wedge's angle
    float edge_sin{};   ///< sine of half the wedge's angle
    bool whole{};       ///< the wedge is the whole turn
    bool complement{};  ///< the wedge is everything but the narrower one these name
};

/// Makes a wedge. A wedge wider than a half turn is held as everything but
/// the narrower wedge that points the other way.
///
/// @param middle the wedge's middle, radians clockwise from up
/// @param half_sweep half its angle, radians
/// @return the wedge
Wedge make_wedge(float middle, float half_sweep) noexcept {
    constexpr float pi = std::numbers::pi_v<float>;
    Wedge wedge{};
    wedge.whole = half_sweep >= pi;
    wedge.complement = !wedge.whole && half_sweep > pi / 2.0F;
    const float direction = wedge.complement ? middle + pi : middle;
    const float half = wedge.complement ? pi - half_sweep : half_sweep;
    wedge.middle_cos = std::cos(direction);
    wedge.middle_sin = std::sin(direction);
    wedge.edge_cos = std::cos(half);
    wedge.edge_sin = std::sin(half);
    return wedge;
}

/// Returns the signed distance to a wedge: negative between its two edges.
///
/// @param dx the point's column less the centre's
/// @param dy the point's row less the centre's
/// @param wedge the wedge
/// @return pixels
float wedge_distance(float dx, float dy, const Wedge& wedge) noexcept {
    if (wedge.whole)
        return -1.0e6F;
    // Measure the point along the wedge's middle and across it, folded onto
    // one side: the middle points at (sin, -cos) on the canvas, rows down.
    const float along = dx * wedge.middle_sin - dy * wedge.middle_cos;
    const float across = std::fabs(dx * wedge.middle_cos + dy * wedge.middle_sin);
    float distance = 0.0F;
    // Behind the centre, as seen along the edge, the centre is the nearest point;
    // else the distance past the edge's line, positive outside.
    if (along * wedge.edge_cos + across * wedge.edge_sin < 0.0F)
        distance = length(across, along);
    else
        distance = across * wedge.edge_cos - along * wedge.edge_sin;
    return wedge.complement ? -distance : distance;
}

/// Returns the distance from a point to a segment.
///
/// @param x the point's column
/// @param y the point's row
/// @param from one end
/// @param to the other end
/// @return pixels
float segment_distance(float x, float y, Spot from, Spot to) noexcept {
    const float vx = to.x - from.x;
    const float vy = to.y - from.y;
    const float wx = x - from.x;
    const float wy = y - from.y;
    const float squared = vx * vx + vy * vy;
    const float t = squared > 0.0F ? std::clamp((wx * vx + wy * vy) / squared, 0.0F, 1.0F) : 0.0F;
    return length(wx - vx * t, wy - vy * t);
}

/// Returns the whole-pixel box around a rectangle with fractions, one pixel wider each way.
///
/// @param area the rectangle
/// @return the box
Box bounds_of(Area area) noexcept {
    const int left = static_cast<int>(std::floor(area.x)) - 1;
    const int top = static_cast<int>(std::floor(area.y)) - 1;
    const int right = static_cast<int>(std::ceil(area.x + area.width)) + 1;
    const int bottom = static_cast<int>(std::ceil(area.y + area.height)) + 1;
    return {left, top, right - left, bottom - top};
}

/// Returns the box around a disc.
///
/// @param centre the centre
/// @param radius pixels
/// @return the box, one pixel wider each way
Box bounds_of(Spot centre, float radius) noexcept {
    return bounds_of(Area{centre.x - radius, centre.y - radius, radius * 2.0F, radius * 2.0F});
}

/// Returns the length in bytes of the UTF-8 character a byte starts.
///
/// @param lead the first byte
/// @return 1 to 4
std::size_t utf8_length(uint8_t lead) noexcept {
    if (lead >= 0xF0U)
        return 4;
    if (lead >= 0xE0U)
        return 3;
    if (lead >= 0xC0U)
        return 2;
    return 1;
}

} // namespace

Rgba with_opacity(Rgba colour, float opacity) noexcept {
    const float factor = std::clamp(opacity, 0.0F, 1.0F);
    colour.a = static_cast<uint8_t>(std::lround(static_cast<float>(colour.a) * factor));
    return colour;
}

Box intersect(Box a, Box b) noexcept {
    const int left = std::max(a.x, b.x);
    const int top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.width, b.x + b.width);
    const int bottom = std::min(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top)
        return {};
    return {left, top, right - left, bottom - top};
}

Box unite(Box a, Box b) noexcept {
    if (a.empty())
        return b.empty() ? Box{} : b;
    if (b.empty())
        return a;
    const int left = std::min(a.x, b.x);
    const int top = std::min(a.y, b.y);
    const int right = std::max(a.x + a.width, b.x + b.width);
    const int bottom = std::max(a.y + a.height, b.y + b.height);
    return {left, top, right - left, bottom - top};
}

Canvas make_canvas(int width, int height) {
    Canvas canvas{};
    canvas.width = std::max(width, 0);
    canvas.height = std::max(height, 0);
    canvas.rgba.assign(static_cast<std::size_t>(canvas.width) * canvas.height * 4U, 0);
    return canvas;
}

Rgba pixel_at(const Canvas& canvas, int x, int y) noexcept {
    if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height)
        return {};
    const auto* pixel = canvas.rgba.data() + (static_cast<std::size_t>(y) * canvas.width + x) * 4U;
    return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

void blend_pixel(uint8_t* pixel, Rgba colour, uint32_t coverage) noexcept {
    const uint32_t source_alpha =
        (static_cast<uint32_t>(colour.a) * std::min(coverage, 255U) + 127U) / 255U;
    if (source_alpha == 0)
        return;
    if (source_alpha == 255U) {
        pixel[0] = colour.r;
        pixel[1] = colour.g;
        pixel[2] = colour.b;
        pixel[3] = 255U;
        return;
    }
    const uint32_t target_alpha = pixel[3];
    if (target_alpha == 0) {
        pixel[0] = colour.r;
        pixel[1] = colour.g;
        pixel[2] = colour.b;
        pixel[3] = static_cast<uint8_t>(source_alpha);
        return;
    }
    if (target_alpha == 255U) {
        const uint32_t left = 255U - source_alpha;
        pixel[0] = static_cast<uint8_t>((colour.r * source_alpha + pixel[0] * left + 127U) / 255U);
        pixel[1] = static_cast<uint8_t>((colour.g * source_alpha + pixel[1] * left + 127U) / 255U);
        pixel[2] = static_cast<uint8_t>((colour.b * source_alpha + pixel[2] * left + 127U) / 255U);
        return;
    }
    // Straight alpha "over": the target's colour counts by its own opacity
    // times what the source leaves showing.
    const uint32_t kept = target_alpha * (255U - source_alpha);   // 0..65025
    const uint32_t out_alpha_scaled = source_alpha * 255U + kept; // alpha × 255
    if (out_alpha_scaled == 0) {
        pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
        return;
    }
    // One division for the three channels: a 32.32 reciprocal of the opacity.
    const uint64_t reciprocal = ((uint64_t{1} << 32U) + out_alpha_scaled - 1U) / out_alpha_scaled;
    const std::array<uint32_t, 3> source{colour.r, colour.g, colour.b};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const uint64_t mixed = uint64_t{source[channel]} * source_alpha * 255U +
                               uint64_t{pixel[channel]} * kept + out_alpha_scaled / 2U;
        pixel[channel] =
            static_cast<uint8_t>(std::min<uint64_t>((mixed * reciprocal) >> 32U, 255U));
    }
    pixel[3] = static_cast<uint8_t>((out_alpha_scaled + 127U) / 255U);
}

Painter::Painter(Canvas& canvas) noexcept : canvas_(&canvas) {
    reset_clip();
}

void Painter::set_clip(Box clip) noexcept {
    clip_ = intersect(clip, Box{0, 0, canvas_->width, canvas_->height});
}

void Painter::reset_clip() noexcept {
    clip_ = Box{0, 0, canvas_->width, canvas_->height};
}

void Painter::note_painted(Box box) noexcept {
    painted_ = unite(painted_, box);
}

void Painter::clear_box(Box box) noexcept {
    const Box part = intersect(box, clip_);
    for (int row = part.y; row < part.y + part.height; ++row) {
        auto* first =
            canvas_->rgba.data() + (static_cast<std::size_t>(row) * canvas_->width + part.x) * 4U;
        std::fill(first, first + static_cast<std::size_t>(part.width) * 4U, uint8_t{0});
    }
}

void Painter::fill_rect(Box box, Rgba colour) noexcept {
    const Box part = intersect(box, clip_);
    if (part.empty() || colour.a == 0)
        return;
    for (int row = part.y; row < part.y + part.height; ++row) {
        auto* pixel =
            canvas_->rgba.data() + (static_cast<std::size_t>(row) * canvas_->width + part.x) * 4U;
        for (int column = 0; column < part.width; ++column, pixel += 4)
            blend_pixel(pixel, colour, 255U);
    }
    note_painted(part);
}

void Painter::outline_rect(Box box, int thickness, Rgba colour) noexcept {
    if (box.empty() || thickness <= 0)
        return;
    const int t = std::min({thickness, (box.width + 1) / 2, (box.height + 1) / 2});
    fill_rect({box.x, box.y, box.width, t}, colour);
    fill_rect({box.x, box.y + box.height - t, box.width, t}, colour);
    fill_rect({box.x, box.y + t, t, box.height - 2 * t}, colour);
    fill_rect({box.x + box.width - t, box.y + t, t, box.height - 2 * t}, colour);
}

template <typename Distance>
void Painter::fill_shape(Box bounds, Distance&& distance, Rgba colour) noexcept {
    const Box part = intersect(bounds, clip_);
    if (part.empty() || colour.a == 0)
        return;
    // The shape is judged a tile at a time: a tile whose centre lies farther
    // from the edge than half its diagonal and the smoothing ramp is wholly
    // outside or wholly inside, and only the tiles the edge crosses are
    // judged pixel by pixel.
    constexpr int tile = 8;
    Box touched{};
    for (int top = part.y; top < part.y + part.height; top += tile) {
        const int rows = std::min(tile, part.y + part.height - top);
        for (int left = part.x; left < part.x + part.width; left += tile) {
            const int columns = std::min(tile, part.x + part.width - left);
            const float reach =
                0.5F * length(static_cast<float>(columns), static_cast<float>(rows)) + 0.5F;
            const float centre = distance(
                static_cast<float>(left) + static_cast<float>(columns) * 0.5F,
                static_cast<float>(top) + static_cast<float>(rows) * 0.5F
            );
            if (centre >= reach)
                continue;
            const bool covered = centre <= -reach;
            int first_column = -1;
            int last_column = -1;
            int first_row = -1;
            int last_row = -1;
            for (int row = top; row < top + rows; ++row) {
                const float y = static_cast<float>(row) + 0.5F;
                auto* pixel = canvas_->rgba.data() +
                              (static_cast<std::size_t>(row) * canvas_->width + left) * 4U;
                for (int column = left; column < left + columns; ++column, pixel += 4) {
                    const uint32_t coverage =
                        covered ? 255U
                                : coverage_of(distance(static_cast<float>(column) + 0.5F, y));
                    if (coverage == 0)
                        continue;
                    blend_pixel(pixel, colour, coverage);
                    first_column = first_column < 0 ? column : std::min(first_column, column);
                    last_column = std::max(last_column, column);
                    if (first_row < 0)
                        first_row = row;
                    last_row = row;
                }
            }
            if (first_column >= 0)
                touched = unite(
                    touched,
                    Box{first_column,
                        first_row,
                        last_column - first_column + 1,
                        last_row - first_row + 1}
                );
        }
    }
    note_painted(touched);
}

void Painter::fill_rounded_rect(Area area, float radius, Rgba colour) noexcept {
    if (area.width <= 0.0F || area.height <= 0.0F)
        return;
    fill_shape(
        bounds_of(area),
        [&](float x, float y) { return rounded_rect_distance(x, y, area, radius); },
        colour
    );
}

void Painter::outline_rounded_rect(Area area, float radius, float thickness, Rgba colour) noexcept {
    if (area.width <= 0.0F || area.height <= 0.0F || thickness <= 0.0F)
        return;
    fill_shape(
        bounds_of(area),
        [&](float x, float y) {
            const float d = rounded_rect_distance(x, y, area, radius);
            return std::max(d, -d - thickness);
        },
        colour
    );
}

void Painter::fill_circle(Spot centre, float radius, Rgba colour) noexcept {
    if (radius <= 0.0F)
        return;
    fill_shape(
        bounds_of(centre, radius),
        [&](float x, float y) { return length(x - centre.x, y - centre.y) - radius; },
        colour
    );
}

void Painter::outline_circle(Spot centre, float radius, float thickness, Rgba colour) noexcept {
    if (radius <= 0.0F || thickness <= 0.0F)
        return;
    const float half = thickness * 0.5F;
    fill_shape(
        bounds_of(centre, radius + half),
        [&](float x, float y) {
            return std::fabs(length(x - centre.x, y - centre.y) - radius) - half;
        },
        colour
    );
}

void Painter::fill_sector(
    Spot centre, float inner, float outer, float middle, float half_sweep, Rgba colour
) noexcept {
    if (outer <= 0.0F || outer <= inner || half_sweep <= 0.0F)
        return;
    const Wedge wedge = make_wedge(middle, half_sweep);
    fill_shape(
        bounds_of(centre, outer),
        [&](float x, float y) {
            const float dx = x - centre.x;
            const float dy = y - centre.y;
            const float r = length(dx, dy);
            const float ring = std::max(inner - r, r - outer);
            return std::max(ring, wedge_distance(dx, dy, wedge));
        },
        colour
    );
}

void Painter::outline_sector(
    Spot centre,
    float inner,
    float outer,
    float middle,
    float half_sweep,
    float thickness,
    Rgba colour
) noexcept {
    if (outer <= 0.0F || outer <= inner || half_sweep <= 0.0F || thickness <= 0.0F)
        return;
    const Wedge wedge = make_wedge(middle, half_sweep);
    fill_shape(
        bounds_of(centre, outer),
        [&](float x, float y) {
            const float dx = x - centre.x;
            const float dy = y - centre.y;
            const float r = length(dx, dy);
            const float ring = std::max(inner - r, r - outer);
            const float d = std::max(ring, wedge_distance(dx, dy, wedge));
            return std::max(d, -d - thickness);
        },
        colour
    );
}

void Painter::stroke_line(Spot from, Spot to, float width, Rgba colour) noexcept {
    if (width <= 0.0F)
        return;
    const float half = width * 0.5F;
    const Area around{
        std::min(from.x, to.x) - half,
        std::min(from.y, to.y) - half,
        std::fabs(to.x - from.x) + width,
        std::fabs(to.y - from.y) + width
    };
    fill_shape(
        bounds_of(around),
        [&](float x, float y) { return segment_distance(x, y, from, to) - half; },
        colour
    );
}

void Painter::stroke_arc(
    Spot centre, float radius, float middle, float half_sweep, float width, Rgba colour
) noexcept {
    if (radius <= 0.0F || width <= 0.0F || half_sweep <= 0.0F)
        return;
    const float half = width * 0.5F;
    const Wedge wedge = make_wedge(middle, half_sweep);
    fill_shape(
        bounds_of(centre, radius + half),
        [&](float x, float y) {
            const float dx = x - centre.x;
            const float dy = y - centre.y;
            const float band = std::fabs(length(dx, dy) - radius) - half;
            return std::max(band, wedge_distance(dx, dy, wedge));
        },
        colour
    );
}

void Painter::fill_polygon(std::span<const Spot> points, Rgba colour) noexcept {
    if (points.size() < 3)
        return;
    float left = points[0].x;
    float right = points[0].x;
    float top = points[0].y;
    float bottom = points[0].y;
    for (const auto& point : points) {
        left = std::min(left, point.x);
        right = std::max(right, point.x);
        top = std::min(top, point.y);
        bottom = std::max(bottom, point.y);
    }
    fill_shape(
        bounds_of(Area{left, top, right - left, bottom - top}),
        [&](float x, float y) {
            bool inside = false;
            float nearest = 1.0e9F;
            for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
                const Spot a = points[i];
                const Spot b = points[j];
                if ((a.y > y) != (b.y > y)) {
                    const float cross_x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
                    if (x < cross_x)
                        inside = !inside;
                }
                nearest = std::min(nearest, segment_distance(x, y, a, b));
            }
            return inside ? -nearest : nearest;
        },
        colour
    );
}

void Painter::draw_coverage(
    std::span<const uint8_t> alpha, int width, int height, int x, int y, Rgba colour
) noexcept {
    if (width <= 0 || height <= 0 || alpha.size() < static_cast<std::size_t>(width) * height)
        return;
    const Box part = intersect(Box{x, y, width, height}, clip_);
    if (part.empty() || colour.a == 0)
        return;
    Box touched{};
    for (int row = part.y; row < part.y + part.height; ++row) {
        const uint8_t* from =
            alpha.data() + static_cast<std::size_t>(row - y) * width + (part.x - x);
        auto* pixel =
            canvas_->rgba.data() + (static_cast<std::size_t>(row) * canvas_->width + part.x) * 4U;
        int first = -1;
        int last = -1;
        for (int column = part.x; column < part.x + part.width; ++column, ++from, pixel += 4) {
            if (*from == 0)
                continue;
            blend_pixel(pixel, colour, *from);
            if (first < 0)
                first = column;
            last = column;
        }
        if (first >= 0)
            touched = unite(touched, Box{first, row, last - first + 1, 1});
    }
    note_painted(touched);
}

void Painter::draw_rgb(
    std::span<const uint8_t> rgb, int width, int height, Box source, Area area, float opacity
) noexcept {
    constexpr std::size_t bytes_per_pixel = 3;
    if (width <= 0 || height <= 0 || area.width <= 0.0F || area.height <= 0.0F ||
        rgb.size() <
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytes_per_pixel)
        return;
    const Box part = intersect(source, Box{0, 0, width, height});
    const auto alpha = static_cast<uint8_t>(std::lround(std::clamp(opacity, 0.0F, 1.0F) * 255.0F));
    if (part.empty() || alpha == 0)
        return;
    // The source is scaled to the area as a whole, so a part cut off the picture keeps its place.
    const float across = static_cast<float>(source.width) / area.width;
    const float down = static_cast<float>(source.height) / area.height;
    const Box target = intersect(bounds_of(area), clip_);
    Box touched{};
    for (int row = target.y; row < target.y + target.height; ++row) {
        const float centre_y = static_cast<float>(row) + 0.5F;
        if (centre_y < area.y || centre_y >= area.y + area.height)
            continue;
        const int from_y = source.y + static_cast<int>(std::floor((centre_y - area.y) * down));
        if (from_y < part.y || from_y >= part.y + part.height)
            continue;
        int first = -1;
        int last = -1;
        auto* pixel =
            canvas_->rgba.data() + (static_cast<std::size_t>(row) * canvas_->width + target.x) * 4U;
        for (int column = target.x; column < target.x + target.width; ++column, pixel += 4) {
            const float centre_x = static_cast<float>(column) + 0.5F;
            if (centre_x < area.x || centre_x >= area.x + area.width)
                continue;
            const int from_x =
                source.x + static_cast<int>(std::floor((centre_x - area.x) * across));
            if (from_x < part.x || from_x >= part.x + part.width)
                continue;
            const uint8_t* from = rgb.data() + (static_cast<std::size_t>(from_y) * width +
                                                static_cast<std::size_t>(from_x)) *
                                                   bytes_per_pixel;
            blend_pixel(pixel, Rgba{from[0], from[1], from[2], alpha}, 255U);
            if (first < 0)
                first = column;
            last = column;
        }
        if (first >= 0)
            touched = unite(touched, Box{first, row, last - first + 1, 1});
    }
    note_painted(touched);
}

namespace {

/// One picture pixel a canvas pixel takes along an axis, and its weight.
struct Tap {
    int index{};     ///< the picture pixel
    double weight{}; ///< its share
};

/// Returns the picture pixels each canvas pixel along an axis takes: the ones
/// its span covers, weighted by how much of each, where the picture shrinks;
/// the two nearest its centre, weighted by how near each is, where it grows.
///
/// @param source the picture's pixels along the axis, 1 or more
/// @param target the canvas pixels along the axis, 1 or more
/// @return for each canvas pixel, its taps
std::vector<std::vector<Tap>> axis_taps(int source, int target) {
    std::vector<std::vector<Tap>> taps(static_cast<std::size_t>(target));
    const double ratio = static_cast<double>(source) / static_cast<double>(target);
    for (int at = 0; at < target; ++at) {
        auto& taken = taps[static_cast<std::size_t>(at)];
        if (ratio >= 1.0) {
            const double first = static_cast<double>(at) * ratio;
            const double last = static_cast<double>(at + 1) * ratio;
            const int lowest = static_cast<int>(std::floor(first));
            const int highest = std::min(static_cast<int>(std::ceil(last)), source);
            for (int index = lowest; index < highest; ++index) {
                const double covered = std::min(last, static_cast<double>(index + 1)) -
                                       std::max(first, static_cast<double>(index));
                if (covered > 0.0)
                    taken.push_back({index, covered});
            }
            continue;
        }
        // The canvas pixel's centre among the picture pixels' centres.
        const double centre = (static_cast<double>(at) + 0.5) * ratio - 0.5;
        const double below = std::floor(centre);
        const double share = centre - below;
        const int lower = std::clamp(static_cast<int>(below), 0, source - 1);
        const int upper = std::clamp(static_cast<int>(below) + 1, 0, source - 1);
        taken.push_back({lower, 1.0 - share});
        taken.push_back({upper, share});
    }
    return taps;
}

/// Returns a canvas pixel of a picture filtered through its taps: the colours
/// mixed weighted by their alpha, the alpha by itself.
///
/// @param rgba the picture, four bytes a pixel, rows top to bottom
/// @param width the picture's width in pixels
/// @param row_taps the picture rows the pixel takes
/// @param column_taps the picture columns the pixel takes
/// @return the pixel, its alpha straight; clear where the taps hold no alpha
Rgba filtered_pixel(
    std::span<const uint8_t> rgba,
    int width,
    const std::vector<Tap>& row_taps,
    const std::vector<Tap>& column_taps
) noexcept {
    constexpr std::size_t bytes_per_pixel = 4;
    double weights = 0.0;
    double alpha = 0.0;
    std::array<double, 3> colour{};
    for (const Tap& vertical : row_taps) {
        for (const Tap& horizontal : column_taps) {
            const double weight = vertical.weight * horizontal.weight;
            const uint8_t* from = rgba.data() + (static_cast<std::size_t>(vertical.index) * width +
                                                 static_cast<std::size_t>(horizontal.index)) *
                                                    bytes_per_pixel;
            const double opacity = weight * from[3];
            weights += weight;
            alpha += opacity;
            for (std::size_t channel = 0; channel < 3; ++channel)
                colour[channel] += opacity * from[channel];
        }
    }
    if (!(weights > 0.0) || !(alpha > 0.0))
        return {};
    const auto channel_of = [](double value) {
        return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
    };
    return {
        channel_of(colour[0] / alpha),
        channel_of(colour[1] / alpha),
        channel_of(colour[2] / alpha),
        channel_of(alpha / weights),
    };
}

} // namespace

void Painter::draw_rgba(std::span<const uint8_t> rgba, int width, int height, Box box) noexcept {
    constexpr std::size_t bytes_per_pixel = 4;
    if (width <= 0 || height <= 0 || box.empty() ||
        rgba.size() <
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytes_per_pixel)
        return;
    const Box part = intersect(box, clip_);
    if (part.empty())
        return;
    // At the picture's own size each pixel is the picture's, and the
    // filter is left out.
    const bool own_size = width == box.width && height == box.height;
    const auto across = own_size ? std::vector<std::vector<Tap>>{} : axis_taps(width, box.width);
    const auto down = own_size ? std::vector<std::vector<Tap>>{} : axis_taps(height, box.height);
    Box touched{};
    for (int row = part.y; row < part.y + part.height; ++row) {
        auto* pixel =
            canvas_->rgba.data() + (static_cast<std::size_t>(row) * canvas_->width + part.x) * 4U;
        int first = -1;
        int last = -1;
        for (int column = part.x; column < part.x + part.width; ++column, pixel += 4) {
            Rgba mixed{};
            if (own_size) {
                const uint8_t* from =
                    rgba.data() + (static_cast<std::size_t>(row - box.y) * width +
                                   static_cast<std::size_t>(column - box.x)) *
                                      bytes_per_pixel;
                mixed = {from[0], from[1], from[2], from[3]};
            } else {
                mixed = filtered_pixel(
                    rgba,
                    width,
                    down[static_cast<std::size_t>(row - box.y)],
                    across[static_cast<std::size_t>(column - box.x)]
                );
            }
            if (mixed.a == 0)
                continue;
            blend_pixel(pixel, mixed, 255U);
            if (first < 0)
                first = column;
            last = column;
        }
        if (first >= 0)
            touched = unite(touched, Box{first, row, last - first + 1, 1});
    }
    note_painted(touched);
}

std::vector<uint8_t>
resized_rgba(std::span<const uint8_t> rgba, int width, int height, int columns, int rows) {
    constexpr std::size_t bytes_per_pixel = 4;
    if (width <= 0 || height <= 0 || columns <= 0 || rows <= 0 ||
        rgba.size() <
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytes_per_pixel)
        return {};
    std::vector<uint8_t> resized(
        static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows) * bytes_per_pixel, 0
    );
    const auto across = axis_taps(width, columns);
    const auto down = axis_taps(height, rows);
    auto* to = resized.data();
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column, to += bytes_per_pixel) {
            const Rgba mixed = filtered_pixel(
                rgba,
                width,
                down[static_cast<std::size_t>(row)],
                across[static_cast<std::size_t>(column)]
            );
            if (mixed.a == 0)
                continue;
            to[0] = mixed.r;
            to[1] = mixed.g;
            to[2] = mixed.b;
            to[3] = mixed.a;
        }
    }
    return resized;
}

void Painter::draw_icon(Icon icon, Area box, Rgba colour) noexcept {
    const float size = std::min(box.width, box.height);
    if (icon == Icon::none || size <= 0.0F)
        return;
    // Marks are drawn on a 24-unit square centred in the box.
    const float unit = size / 24.0F;
    const float ox = box.x + (box.width - size) * 0.5F;
    const float oy = box.y + (box.height - size) * 0.5F;
    const auto at = [&](float x, float y) { return Spot{ox + x * unit, oy + y * unit}; };
    const float w = std::max(2.0F * unit, 1.2F); // stroke width
    const auto line = [&](float x0, float y0, float x1, float y1) {
        stroke_line(at(x0, y0), at(x1, y1), w, colour);
    };
    const auto path = [&](std::initializer_list<Spot> corners, bool closed) {
        const Spot* first = corners.begin();
        for (const Spot* p = first; p + 1 != corners.end(); ++p)
            line(p->x, p->y, (p + 1)->x, (p + 1)->y);
        if (closed && corners.size() > 2) {
            const Spot* back = corners.end() - 1;
            line(back->x, back->y, first->x, first->y);
        }
    };
    const auto magnifier = [&] {
        outline_circle(at(10.0F, 10.0F), 6.5F * unit, w, colour);
        line(15.0F, 15.0F, 20.5F, 20.5F);
    };
    const auto tray = [&] {
        path({{4.0F, 14.0F}, {4.0F, 20.0F}, {20.0F, 20.0F}, {20.0F, 14.0F}}, false);
    };
    constexpr float pi = std::numbers::pi_v<float>;
    switch (icon) {
    case Icon::none:
        return;
    case Icon::shift_arrow:
        path(
            {{12.0F, 3.5F},
             {20.5F, 12.0F},
             {16.0F, 12.0F},
             {16.0F, 20.0F},
             {8.0F, 20.0F},
             {8.0F, 12.0F},
             {3.5F, 12.0F}},
            true
        );
        return;
    case Icon::plus:
        line(12.0F, 4.0F, 12.0F, 20.0F);
        line(4.0F, 12.0F, 20.0F, 12.0F);
        return;
    case Icon::cross:
        line(5.5F, 5.5F, 18.5F, 18.5F);
        line(18.5F, 5.5F, 5.5F, 18.5F);
        return;
    case Icon::box_select:
        for (int side = 0; side < 4; ++side)
            for (int dash = 0; dash < 3; ++dash) {
                const float from = 4.0F + static_cast<float>(dash) * 6.0F;
                const float to = from + 3.5F;
                switch (side) {
                case 0:
                    line(from, 4.0F, to, 4.0F);
                    break;
                case 1:
                    line(20.0F, from, 20.0F, to);
                    break;
                case 2:
                    line(from + 0.5F, 20.0F, to + 0.5F, 20.0F);
                    break;
                default:
                    line(4.0F, from + 0.5F, 4.0F, to + 0.5F);
                    break;
                }
            }
        return;
    case Icon::pause:
        fill_rounded_rect(
            Area{ox + 6.0F * unit, oy + 4.0F * unit, 4.0F * unit, 16.0F * unit}, unit, colour
        );
        fill_rounded_rect(
            Area{ox + 14.0F * unit, oy + 4.0F * unit, 4.0F * unit, 16.0F * unit}, unit, colour
        );
        return;
    case Icon::gauge:
        stroke_arc(at(12.0F, 15.0F), 8.5F * unit, 0.0F, pi * 0.62F, w, colour);
        line(12.0F, 15.0F, 16.5F, 9.0F);
        fill_circle(at(12.0F, 15.0F), 1.8F * unit, colour);
        return;
    case Icon::speech:
        path(
            {{4.0F, 5.0F},
             {20.0F, 5.0F},
             {20.0F, 16.0F},
             {11.0F, 16.0F},
             {6.5F, 20.0F},
             {7.0F, 16.0F},
             {4.0F, 16.0F}},
            true
        );
        return;
    case Icon::crosshair:
        outline_circle(at(12.0F, 12.0F), 6.5F * unit, w, colour);
        line(12.0F, 2.5F, 12.0F, 7.0F);
        line(12.0F, 17.0F, 12.0F, 21.5F);
        line(2.5F, 12.0F, 7.0F, 12.0F);
        line(17.0F, 12.0F, 21.5F, 12.0F);
        fill_circle(at(12.0F, 12.0F), 1.6F * unit, colour);
        return;
    case Icon::eye:
        stroke_arc(at(12.0F, 20.0F), 12.0F * unit, 0.0F, pi * 0.27F, w, colour);
        stroke_arc(at(12.0F, 4.0F), 12.0F * unit, pi, pi * 0.27F, w, colour);
        fill_circle(at(12.0F, 12.0F), 3.0F * unit, colour);
        return;
    case Icon::chevrons:
        path({{5.0F, 5.0F}, {11.0F, 12.0F}, {5.0F, 19.0F}}, false);
        path({{12.0F, 5.0F}, {18.0F, 12.0F}, {12.0F, 19.0F}}, false);
        return;
    case Icon::info:
        outline_circle(at(12.0F, 12.0F), 9.0F * unit, w, colour);
        fill_circle(at(12.0F, 7.5F), 1.5F * unit, colour);
        line(12.0F, 11.0F, 12.0F, 17.0F);
        return;
    case Icon::menu:
        line(4.0F, 6.0F, 20.0F, 6.0F);
        line(4.0F, 12.0F, 20.0F, 12.0F);
        line(4.0F, 18.0F, 20.0F, 18.0F);
        return;
    case Icon::grid:
        path({{4.0F, 4.0F}, {10.5F, 4.0F}, {10.5F, 10.5F}, {4.0F, 10.5F}}, true);
        path({{13.5F, 4.0F}, {20.0F, 4.0F}, {20.0F, 10.5F}, {13.5F, 10.5F}}, true);
        path({{4.0F, 13.5F}, {10.5F, 13.5F}, {10.5F, 20.0F}, {4.0F, 20.0F}}, true);
        path({{13.5F, 13.5F}, {20.0F, 13.5F}, {20.0F, 20.0F}, {13.5F, 20.0F}}, true);
        return;
    case Icon::magnifier_minus:
        magnifier();
        line(7.0F, 10.0F, 13.0F, 10.0F);
        return;
    case Icon::magnifier_plus:
        magnifier();
        line(7.0F, 10.0F, 13.0F, 10.0F);
        line(10.0F, 7.0F, 10.0F, 13.0F);
        return;
    case Icon::arrow:
        line(4.0F, 12.0F, 19.0F, 12.0F);
        path({{13.0F, 6.0F}, {19.5F, 12.0F}, {13.0F, 18.0F}}, false);
        return;
    case Icon::patrol:
        line(4.0F, 8.5F, 19.0F, 8.5F);
        path({{15.0F, 4.5F}, {19.5F, 8.5F}, {15.0F, 12.5F}}, false);
        line(5.0F, 15.5F, 20.0F, 15.5F);
        path({{9.0F, 11.5F}, {4.5F, 15.5F}, {9.0F, 19.5F}}, false);
        return;
    case Icon::shield:
        path(
            {{12.0F, 3.0F},
             {19.5F, 6.0F},
             {19.0F, 13.0F},
             {15.5F, 18.0F},
             {12.0F, 21.0F},
             {8.5F, 18.0F},
             {5.0F, 13.0F},
             {4.5F, 6.0F}},
            true
        );
        return;
    case Icon::square:
        path({{5.5F, 5.5F}, {18.5F, 5.5F}, {18.5F, 18.5F}, {5.5F, 18.5F}}, true);
        return;
    case Icon::star: {
        std::array<Spot, 10> corners{};
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const float angle = static_cast<float>(i) * pi / 5.0F;
            const float radius = (i % 2U == 0U ? 9.5F : 4.0F) * unit;
            corners[i] = Spot{
                ox + 12.0F * unit + std::sin(angle) * radius,
                oy + 12.8F * unit - std::cos(angle) * radius
            };
        }
        for (std::size_t i = 0; i < corners.size(); ++i)
            stroke_line(corners[i], corners[(i + 1) % corners.size()], w, colour);
        return;
    }
    case Icon::dots:
        fill_circle(at(5.0F, 12.0F), 2.4F * unit, colour);
        fill_circle(at(12.0F, 12.0F), 2.4F * unit, colour);
        fill_circle(at(19.0F, 12.0F), 2.4F * unit, colour);
        return;
    case Icon::tray_down:
        tray();
        line(12.0F, 3.5F, 12.0F, 15.0F);
        path({{7.5F, 10.5F}, {12.0F, 15.0F}, {16.5F, 10.5F}}, false);
        return;
    case Icon::tray_up:
        tray();
        line(12.0F, 4.0F, 12.0F, 15.5F);
        path({{7.5F, 8.5F}, {12.0F, 4.0F}, {16.5F, 8.5F}}, false);
        return;
    case Icon::wrench:
        line(5.0F, 19.0F, 13.0F, 11.0F);
        stroke_arc(at(15.5F, 8.5F), 4.5F * unit, pi * 1.25F, pi * 0.62F, w, colour);
        return;
    case Icon::flag:
        line(6.0F, 3.5F, 6.0F, 21.0F);
        path({{6.0F, 4.5F}, {18.5F, 4.5F}, {15.0F, 8.5F}, {18.5F, 12.5F}, {6.0F, 12.5F}}, false);
        return;
    case Icon::warning:
        path({{12.0F, 3.5F}, {21.5F, 20.0F}, {2.5F, 20.0F}}, true);
        line(12.0F, 9.0F, 12.0F, 14.0F);
        fill_circle(at(12.0F, 17.0F), 1.4F * unit, colour);
        return;
    case Icon::chevron_left:
        path({{15.0F, 4.5F}, {8.0F, 12.0F}, {15.0F, 19.5F}}, false);
        return;
    case Icon::chevron_right:
        path({{9.0F, 4.5F}, {16.0F, 12.0F}, {9.0F, 19.5F}}, false);
        return;
    case Icon::chevron_down:
        path({{5.0F, 9.0F}, {12.0F, 16.0F}, {19.0F, 9.0F}}, false);
        return;
    case Icon::power:
        stroke_arc(at(12.0F, 13.0F), 7.5F * unit, pi, pi * 0.78F, w, colour);
        line(12.0F, 3.0F, 12.0F, 12.0F);
        return;
    case Icon::cloak:
        for (int dash = 0; dash < 4; ++dash) {
            const float middle = static_cast<float>(dash) * pi / 2.0F + pi / 4.0F;
            stroke_arc(at(12.0F, 12.0F), 8.0F * unit, middle, pi * 0.16F, w, colour);
        }
        fill_circle(at(12.0F, 12.0F), 2.5F * unit, colour);
        return;
    }
}

TextLine draw_line(text_font::FontStack& fonts, std::string_view text, int pixel_size, bool bold) {
    TextLine line{};
    text_font::Style style{};
    style.pixel_size = std::clamp(pixel_size, 1, text_font::max_pixel_size);
    style.weight = bold ? text_font::Weight::bold : text_font::Weight::regular;
    style.rendering = text_font::Rendering::antialiased;
    if (auto coverage = fonts.draw(text, style)) {
        line.coverage = std::move(*coverage);
        line.drawn = true;
    }
    return line;
}

int text_width(text_font::FontStack& fonts, std::string_view text, int pixel_size, bool bold) {
    if (text.empty())
        return 0;
    text_font::Style style{};
    style.pixel_size = std::clamp(pixel_size, 1, text_font::max_pixel_size);
    style.weight = bold ? text_font::Weight::bold : text_font::Weight::regular;
    style.rendering = text_font::Rendering::antialiased;
    const auto placed = fonts.layout(text, style);
    if (!placed || placed->empty())
        return 0;
    const auto& last = placed->back();
    return last.pen + last.advance;
}

text_font::LineMetrics line_metrics(text_font::FontStack& fonts, int pixel_size, bool bold) {
    text_font::Style style{};
    style.pixel_size = std::clamp(pixel_size, 1, text_font::max_pixel_size);
    style.weight = bold ? text_font::Weight::bold : text_font::Weight::regular;
    style.rendering = text_font::Rendering::antialiased;
    return fonts.metrics(style).value_or(text_font::LineMetrics{});
}

std::string fit_text(
    text_font::FontStack& fonts, std::string_view text, int pixel_size, bool bold, int max_width
) {
    if (text_width(fonts, text, pixel_size, bold) <= max_width)
        return std::string(text);
    // Character starts, so that the text is cut between characters.
    std::vector<std::size_t> starts;
    for (std::size_t at = 0; at < text.size(); at += utf8_length(static_cast<uint8_t>(text[at])))
        starts.push_back(at);
    std::size_t low = 0;
    std::size_t high = starts.size();
    // The most characters whose start with the ellipsis fits.
    while (low < high) {
        const std::size_t mid = (low + high + 1) / 2;
        std::string candidate(text.substr(0, mid < starts.size() ? starts[mid] : text.size()));
        while (!candidate.empty() && candidate.back() == ' ')
            candidate.pop_back();
        candidate += ellipsis;
        if (text_width(fonts, candidate, pixel_size, bold) <= max_width)
            low = mid;
        else
            high = mid - 1;
    }
    if (low == 0)
        return text_width(fonts, ellipsis, pixel_size, bold) <= max_width ? std::string(ellipsis)
                                                                          : std::string{};
    std::string kept(text.substr(0, low < starts.size() ? starts[low] : text.size()));
    while (!kept.empty() && kept.back() == ' ')
        kept.pop_back();
    return kept + std::string(ellipsis);
}

Box paint_line(Painter& painter, const TextLine& line, int pen_x, int baseline_y, Rgba colour) {
    if (!line.drawn || line.coverage.width <= 0 || line.coverage.height <= 0)
        return {};
    const int x = pen_x - line.coverage.origin;
    const int y = baseline_y - line.coverage.baseline;
    painter.draw_coverage(
        line.coverage.alpha, line.coverage.width, line.coverage.height, x, y, colour
    );
    return Box{x, y, line.coverage.width, line.coverage.height};
}

} // namespace oa::ui::paint
