// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The places of the overlays Arrange overlays moves: their keys and values
// in the preferences, and where an overlay shows at a place.

#include "oa_layer_overlays.hpp"

#include "window_placement.hpp"

#include "oa/app/family_ring.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app::overlay_placement {

namespace {

namespace kit = oa::ui::kit;

/// Returns a quotient rounded down, for a numerator below 0 too.
///
/// @param numerator the numerator
/// @param denominator the denominator, above 0
/// @return floor(numerator / denominator)
[[nodiscard]] int64_t floor_div(int64_t numerator, int64_t denominator) noexcept {
    const int64_t quotient = numerator / denominator;
    return numerator % denominator < 0 ? quotient - 1 : quotient;
}

/// Returns a distance rounded to the nearest multiple of grid_points, a half
/// away from zero.
///
/// @param points the distance, in points
/// @return the multiple
[[nodiscard]] int64_t on_grid(int64_t points) noexcept {
    const int64_t magnitude = (points < 0 ? -points : points) + grid_points / 2;
    const int64_t rounded = magnitude / grid_points * grid_points;
    return points < 0 ? -rounded : rounded;
}

/// Returns a distance held between two bounds on the grid: the nearest
/// multiple of grid_points within them, or 0 when none is.
///
/// @param points the distance, in points
/// @param least the least it may be
/// @param most the most it may be
/// @return the distance, a multiple of grid_points
[[nodiscard]] int64_t held_on_grid(int64_t points, int64_t least, int64_t most) noexcept {
    const int64_t low = -floor_div(-least, grid_points) * grid_points;
    const int64_t high = floor_div(most, grid_points) * grid_points;
    if (high < low)
        return 0;
    return std::clamp(on_grid(points), low, high);
}

/// Returns the near edge of an overlay on one axis at a place.
///
/// @param place the place on the axis
/// @param room_start the room's near edge
/// @param room_length the room's length
/// @param length the overlay's length
/// @return the overlay's near edge, before it is kept in the room
[[nodiscard]] int64_t
near_edge_at(const AxisPlace& place, int64_t room_start, int64_t room_length, int64_t length) {
    switch (place.anchor) {
    case Anchor::end:
        return room_start + room_length - length - place.points;
    case Anchor::centre:
        return room_start + floor_div(room_length - length, 2) + place.points;
    case Anchor::start:
        break;
    }
    return room_start + place.points;
}

/// Returns an overlay's place on one axis (place_at).
///
/// @param start the overlay's near edge
/// @param length its length
/// @param room_start the room's near edge
/// @param room_length the room's length
/// @return the place on the axis
[[nodiscard]] AxisPlace
axis_place_at(int64_t start, int64_t length, int64_t room_start, int64_t room_length) noexcept {
    const int64_t spare = room_length - length;
    if (spare <= 0)
        return {Anchor::start, 0};
    const int64_t from_start = start - room_start;
    // Thrice the centre against the room's thirds, so that no third is rounded.
    const int64_t centre_thrice = (2 * from_start + length) * 3;
    if (centre_thrice < 2 * room_length)
        return {Anchor::start, static_cast<int32_t>(held_on_grid(from_start, 0, spare))};
    if (centre_thrice > 4 * room_length)
        return {Anchor::end, static_cast<int32_t>(held_on_grid(spare - from_start, 0, spare))};
    const int64_t centred = floor_div(spare, 2);
    return {
        Anchor::centre,
        static_cast<int32_t>(held_on_grid(from_start - centred, -centred, spare - centred))
    };
}

/// Reads one axis of a place's text.
///
/// @param text the axis: its anchor's letter and a signed whole number
/// @param start the letter of the near edge: l across, t down
/// @param end the letter of the far edge: r across, b down
/// @return the axis's place; nothing for any other text
[[nodiscard]] std::optional<AxisPlace>
axis_of(std::string_view text, char start, char end) noexcept {
    if (text.size() < 2)
        return std::nullopt;
    AxisPlace place;
    const char letter = text.front();
    if (letter == start)
        place.anchor = Anchor::start;
    else if (letter == 'c')
        place.anchor = Anchor::centre;
    else if (letter == end)
        place.anchor = Anchor::end;
    else
        return std::nullopt;
    text.remove_prefix(1);
    int32_t value = 0;
    const auto [stop, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || stop != text.data() + text.size())
        return std::nullopt;
    if (value > most_place_points || value < -most_place_points)
        return std::nullopt;
    place.points = value;
    return place;
}

/// Writes one axis of a place.
///
/// @param place the axis's place
/// @param start the letter of the near edge
/// @param end the letter of the far edge
/// @return the letter and the distance
[[nodiscard]] std::string axis_text(const AxisPlace& place, char start, char end) {
    char letter = start;
    if (place.anchor == Anchor::centre)
        letter = 'c';
    else if (place.anchor == Anchor::end)
        letter = end;
    return letter + std::to_string(place.points);
}

} // namespace

bool arranged(std::string_view overlay) noexcept {
    return std::find(overlays.begin(), overlays.end(), overlay) != overlays.end();
}

std::string place_key(std::string_view overlay, kit::SizeClass size_class) {
    std::string key(place_key_start);
    key += overlay;
    key += '.';
    key += window_placement::class_word(size_class);
    return key;
}

std::string place_text(const OverlayPlace& place) {
    return axis_text(place.across, 'l', 'r') + ',' + axis_text(place.down, 't', 'b');
}

std::optional<OverlayPlace> place_of(std::string_view text) noexcept {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos)
        return std::nullopt;
    const auto across = axis_of(text.substr(0, comma), 'l', 'r');
    const auto down = axis_of(text.substr(comma + 1), 't', 'b');
    if (!across || !down)
        return std::nullopt;
    return OverlayPlace{*across, *down};
}

std::optional<OverlayPlace> stored_place(
    const std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    kit::SizeClass size_class
) {
    const auto found = preferences.find(place_key(overlay, size_class));
    if (found == preferences.end())
        return std::nullopt;
    return place_of(found->second);
}

bool store_place(
    std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    kit::SizeClass size_class,
    const OverlayPlace& place
) {
    const std::string key = place_key(overlay, size_class);
    const std::string text = place_text(place);
    const auto found = preferences.find(key);
    if (found != preferences.end() && found->second == text)
        return false;
    preferences.insert_or_assign(key, text);
    return true;
}

bool forget_place(
    std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    kit::SizeClass size_class
) {
    return preferences.erase(place_key(overlay, size_class)) != 0;
}

bool forget_places(std::map<std::string, std::string>& preferences) {
    bool erased = false;
    for (auto entry = preferences.lower_bound(std::string(place_key_start));
         entry != preferences.end() && entry->first.starts_with(place_key_start);) {
        entry = preferences.erase(entry);
        erased = true;
    }
    return erased;
}

kit::Rect kept_in(const kit::Rect& room, const kit::Rect& rect) noexcept {
    const auto axis = [](int32_t start, int32_t length, int32_t room_start, int32_t room_length) {
        const int32_t last = room_start + std::max(room_length - length, 0);
        return std::clamp(start, room_start, last);
    };
    return {
        axis(rect.x, rect.width, room.x, room.width),
        axis(rect.y, rect.height, room.y, room.height),
        rect.width,
        rect.height
    };
}

kit::Rect rect_at(const kit::Rect& room, kit::Point size, const OverlayPlace& place) noexcept {
    const int64_t x = near_edge_at(place.across, room.x, room.width, size.x);
    const int64_t y = near_edge_at(place.down, room.y, room.height, size.y);
    const auto bounded = [](int64_t value) {
        return static_cast<int32_t>(std::clamp<int64_t>(
            value, -2 * int64_t{most_place_points}, 2 * int64_t{most_place_points}
        ));
    };
    return kept_in(room, {bounded(x), bounded(y), size.x, size.y});
}

OverlayPlace place_at(const kit::Rect& room, const kit::Rect& shown) noexcept {
    return {
        axis_place_at(shown.x, shown.width, room.x, room.width),
        axis_place_at(shown.y, shown.height, room.y, room.height)
    };
}

kit::Rect shown_rect(
    const kit::Rect& room, const kit::Rect& unplaced, const std::optional<OverlayPlace>& place
) noexcept {
    if (!place)
        return kept_in(room, unplaced);
    return rect_at(room, {unplaced.width, unplaced.height}, *place);
}

kit::Rect handle_rect(const kit::Rect& room, const kit::Rect& shown, kit::Point least) noexcept {
    const auto axis =
        [](int32_t start, int32_t length, int32_t wanted, int32_t room_start, int32_t room_length) {
            if (length >= wanted)
                return start;
            const int64_t centre_thrice = (2 * int64_t{start - room_start} + length) * 3;
            if (centre_thrice < 2 * int64_t{room_length})
                return start;
            if (centre_thrice > 4 * int64_t{room_length})
                return start + length - wanted;
            return start + static_cast<int32_t>(floor_div(int64_t{length} - wanted, 2));
        };
    const int32_t width = std::max(shown.width, least.x);
    const int32_t height = std::max(shown.height, least.y);
    return kept_in(
        room,
        {axis(shown.x, shown.width, width, room.x, room.width),
         axis(shown.y, shown.height, height, room.y, room.height),
         width,
         height}
    );
}

OverlayPlace
dragged_place(const kit::Rect& room, const kit::Rect& start, kit::Point travel) noexcept {
    const kit::Rect moved{start.x + travel.x, start.y + travel.y, start.width, start.height};
    return place_at(room, kept_in(room, moved));
}

OverlayPlace moved_place(
    const kit::Rect& room, const kit::Rect& shown, int32_t across, int32_t down, bool large
) noexcept {
    const int32_t step = large ? large_step_points : grid_points;
    const kit::Rect moved{
        shown.x + across * step, shown.y + down * step, shown.width, shown.height
    };
    return place_at(room, kept_in(room, moved));
}

} // namespace oa::app::overlay_placement

namespace oa::app {
namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace renderer = oa::ui::frontend_renderer;

/// The largest picture the ring is drawn into, on either side. A control
/// larger than the window the game presents is not given a ring.
constexpr int32_t k_ring_limit = 8192;

/// The crisp blend's full share, the same 256 `blend_rect` takes as opaque.
constexpr uint32_t k_crisp_opaque = 256;

/// One channel of the crisp blend, the same arithmetic `blend_rect` uses.
///
/// @param ground the channel under the ring
/// @param ink the ring's channel
/// @param share the ring's share, 0 to 256
/// @return the mixed channel
[[nodiscard]] uint8_t crisp_channel(uint8_t ground, uint8_t ink, uint32_t share) noexcept {
    const uint32_t keep = k_crisp_opaque - share;
    const uint32_t painted = uint32_t{ink} * share + k_crisp_opaque / 2U;
    return static_cast<uint8_t>((uint32_t{ground} * keep + painted) / k_crisp_opaque);
}

/// The crisp share that reproduces the ring on black and on white.
///
/// One channel does not name a share on its own: the integer division
/// collides. The three channels on two grounds name one share, or none.
///
/// @param black the pixel drawn over black
/// @param white the pixel drawn over white
/// @param[out] share the share, 0 to 256
/// @return false when no share reproduces both pixels
[[nodiscard]] bool
crisp_share(const uint8_t black[3], const uint8_t white[3], uint16_t& share) noexcept {
    const std::array<uint8_t, 3> ink{
        kit::colour::focus.r, kit::colour::focus.g, kit::colour::focus.b
    };
    bool found = false;
    uint16_t kept = 0;
    for (uint32_t candidate = 0; candidate <= k_crisp_opaque; ++candidate) {
        bool matches = true;
        for (std::size_t channel = 0; channel < ink.size(); ++channel) {
            if (crisp_channel(0, ink[channel], candidate) != black[channel] ||
                crisp_channel(255, ink[channel], candidate) != white[channel]) {
                matches = false;
                break;
            }
        }
        if (!matches)
            continue;
        if (found)
            return false;
        found = true;
        kept = static_cast<uint16_t>(candidate);
    }
    if (!found)
        return false;
    share = kept;
    return true;
}

/// Draws the ring on the crisp form into a sheet filled with one ground.
///
/// @param[out] sheet the picture, replaced
/// @param sheet_w its columns
/// @param sheet_h its rows
/// @param point_width the control's width, in points
/// @param point_height the control's height, in points
/// @param scale surface pixels in one point
/// @param ground the colour under the ring, the same on every channel
void draw_crisp_sheet(
    renderer::Surface& sheet,
    int32_t sheet_w,
    int32_t sheet_h,
    int32_t point_width,
    int32_t point_height,
    int32_t scale,
    uint8_t ground
) {
    sheet.width = static_cast<uint32_t>(sheet_w);
    sheet.height = static_cast<uint32_t>(sheet_h);
    sheet.rgb.assign(
        static_cast<std::size_t>(sheet_w) * static_cast<std::size_t>(sheet_h) * 3U, ground
    );
    kit::Canvas canvas{};
    canvas.surface = &sheet;
    canvas.placement = {0, 0, scale, {}};
    const int32_t inset = kit::compact_metrics.focus_inset;
    kit::draw_focus_ring(canvas, {inset, inset, point_width, point_height}, true);
}

/// Draws the ring on the crisp form and keeps each pixel's share.
///
/// @param window_x the control's left column
/// @param window_y the control's top row
/// @param point_width the control's width, in points
/// @param point_height the control's height, in points
/// @param scale surface pixels in one point
/// @return the ring's pixels
[[nodiscard]] std::vector<FamilyRingPixel> crisp_ring_pixels(
    int32_t window_x, int32_t window_y, int32_t point_width, int32_t point_height, int32_t scale
) {
    const int32_t inset = kit::compact_metrics.focus_inset;
    const int32_t sheet_w = (point_width + 2 * inset) * scale;
    const int32_t sheet_h = (point_height + 2 * inset) * scale;
    if (sheet_w <= 0 || sheet_h <= 0 || sheet_w > k_ring_limit || sheet_h > k_ring_limit)
        return {};
    renderer::Surface on_black;
    renderer::Surface on_white;
    draw_crisp_sheet(on_black, sheet_w, sheet_h, point_width, point_height, scale, 0);
    draw_crisp_sheet(on_white, sheet_w, sheet_h, point_width, point_height, scale, 255);
    std::vector<FamilyRingPixel> pixels;
    const int32_t origin_x = window_x - inset * scale;
    const int32_t origin_y = window_y - inset * scale;
    for (int32_t row = 0; row < sheet_h; ++row) {
        for (int32_t column = 0; column < sheet_w; ++column) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(sheet_w) +
                 static_cast<std::size_t>(column)) *
                3U;
            const uint8_t* black = on_black.rgb.data() + at;
            const uint8_t* white = on_white.rgb.data() + at;
            if (black[0] == 0 && black[1] == 0 && black[2] == 0 && white[0] == 255 &&
                white[1] == 255 && white[2] == 255)
                continue;
            uint16_t share = 0;
            if (!crisp_share(black, white, share) || share == 0)
                continue;
            pixels.push_back(FamilyRingPixel{origin_x + column, origin_y + row, share});
        }
    }
    return pixels;
}

/// Draws the ring on the smooth form and keeps each pixel's opacity.
///
/// The sheet starts clear. The painter stores the ring's colour and the
/// coverage as the pixel's opacity, which is the share a later blend uses.
///
/// @param window_x the control's left column
/// @param window_y the control's top row
/// @param point_width the control's width, in points
/// @param point_height the control's height, in points
/// @param percent the scale, in percent
/// @return the ring's pixels
[[nodiscard]] std::vector<FamilyRingPixel> smooth_ring_pixels(
    int32_t window_x, int32_t window_y, int32_t point_width, int32_t point_height, int32_t percent
) {
    const int32_t inset = kit::compact_metrics.focus_inset;
    const kit::Rect control{inset, inset, point_width, point_height};
    const kit::Rect sheet_points{0, 0, point_width + 2 * inset, point_height + 2 * inset};
    const kit::Rect sheet_px = kit::to_canvas(sheet_points, percent);
    const kit::Rect control_px = kit::to_canvas(control, percent);
    if (sheet_px.width <= 0 || sheet_px.height <= 0 || sheet_px.width > k_ring_limit ||
        sheet_px.height > k_ring_limit)
        return {};
    paint::Canvas picture = paint::make_canvas(sheet_px.width, sheet_px.height);
    paint::Painter painter(picture);
    kit::Canvas canvas{};
    canvas.smooth.painter = &painter;
    canvas.smooth.scale_percent = percent;
    kit::draw_focus_ring(canvas, control, true);
    std::vector<FamilyRingPixel> pixels;
    const int32_t origin_x = window_x - control_px.x;
    const int32_t origin_y = window_y - control_px.y;
    const std::size_t count =
        static_cast<std::size_t>(sheet_px.width) * static_cast<std::size_t>(sheet_px.height);
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        const uint8_t opacity = picture.rgba[pixel * 4U + 3U];
        if (opacity == 0)
            continue;
        const int32_t column =
            static_cast<int32_t>(pixel % static_cast<std::size_t>(sheet_px.width));
        const int32_t row = static_cast<int32_t>(pixel / static_cast<std::size_t>(sheet_px.width));
        pixels.push_back(FamilyRingPixel{origin_x + column, origin_y + row, opacity});
    }
    return pixels;
}

} // namespace

void blend_family_coverage(
    uint8_t& red, uint8_t& green, uint8_t& blue, uint16_t coverage, bool crisp
) noexcept {
    const uint8_t ink_red = kit::colour::focus.r;
    const uint8_t ink_green = kit::colour::focus.g;
    const uint8_t ink_blue = kit::colour::focus.b;
    if (crisp) {
        const uint32_t share = std::min<uint32_t>(coverage, k_crisp_opaque);
        if (share == 0)
            return;
        red = crisp_channel(red, ink_red, share);
        green = crisp_channel(green, ink_green, share);
        blue = crisp_channel(blue, ink_blue, share);
        return;
    }
    const uint32_t opacity = std::min<uint32_t>(coverage, 255U);
    if (opacity == 0)
        return;
    if (opacity == 255U) {
        red = ink_red;
        green = ink_green;
        blue = ink_blue;
        return;
    }
    const uint32_t left = 255U - opacity;
    const auto mix = [opacity, left](uint8_t ground, uint8_t ink) {
        return static_cast<uint8_t>(
            (uint32_t{ink} * opacity + uint32_t{ground} * left + 127U) / 255U
        );
    };
    red = mix(red, ink_red);
    green = mix(green, ink_green);
    blue = mix(blue, ink_blue);
}

bool control_window_rect(
    const FamilyControlBox& box,
    const FamilyWindowMap& map,
    int32_t& left,
    int32_t& top,
    int32_t& width,
    int32_t& height
) noexcept {
    if (box.window_pixels) {
        left = box.x;
        top = box.y;
        width = box.width;
        height = box.height;
        return width > 0 && height > 0;
    }
    if (box.width <= 0 || box.height <= 0)
        return false;
    const float density = map.density > 0.0F ? map.density : 1.0F;
    const float origin_x = map.identity ? 0.0F : map.origin_x;
    const float origin_y = map.identity ? 0.0F : map.origin_y;
    const float scale_x = map.identity ? 1.0F : map.scale_x;
    const float scale_y = map.identity ? 1.0F : map.scale_y;
    if (scale_x == 0.0F || scale_y == 0.0F)
        return false;
    const auto pixel = [density](float coordinate) {
        return static_cast<int32_t>(
            std::lround(static_cast<double>(coordinate) * static_cast<double>(density))
        );
    };
    const float window_left = origin_x + static_cast<float>(box.x) * scale_x;
    const float window_top = origin_y + static_cast<float>(box.y) * scale_y;
    const float window_right = origin_x + static_cast<float>(box.x + box.width) * scale_x;
    const float window_bottom = origin_y + static_cast<float>(box.y + box.height) * scale_y;
    left = pixel(window_left);
    top = pixel(window_top);
    width = pixel(window_right) - left;
    height = pixel(window_bottom) - top;
    return width > 0 && height > 0;
}

std::vector<FamilyRingPixel> family_ring_pixels(
    int32_t window_x,
    int32_t window_y,
    int32_t window_width,
    int32_t window_height,
    int32_t scale_percent
) {
    if (window_width <= 0 || window_height <= 0)
        return {};
    int32_t percent = scale_percent > 0 ? scale_percent : 100;
    if (percent < 100)
        percent = 100;
    if (percent % 100 == 0) {
        const int32_t scale = percent / 100;
        const int32_t point_width = std::max(window_width / scale, int32_t{1});
        const int32_t point_height = std::max(window_height / scale, int32_t{1});
        return crisp_ring_pixels(window_x, window_y, point_width, point_height, scale);
    }
    const int32_t point_width = std::max(
        static_cast<int32_t>(
            std::lround(static_cast<double>(window_width) * 100.0 / static_cast<double>(percent))
        ),
        int32_t{1}
    );
    const int32_t point_height = std::max(
        static_cast<int32_t>(
            std::lround(static_cast<double>(window_height) * 100.0 / static_cast<double>(percent))
        ),
        int32_t{1}
    );
    return smooth_ring_pixels(window_x, window_y, point_width, point_height, percent);
}

} // namespace oa::app
