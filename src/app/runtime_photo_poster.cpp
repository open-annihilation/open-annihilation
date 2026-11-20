// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A poster: the view, an area the player marks, or the whole map, at one
// map pixel a picture pixel. Off and Basic draw it in strips on the
// processor. Full classic draws it in tiles. The turned view draws none.
// The draw puts the window back, and it does not write the Game block:
// sight, the clock and the message lines stay as the player left them.
#include "oa/app/runtime.hpp"

#include "full_presentation.hpp"
#include "photo_state.hpp"

#include "oa/app/middle_pan.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/present/photo/photo_poster.hpp"
#include "oa/present/world_renderer.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/photo_panel/words.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace photo = oa::present::photo;
namespace words = oa::ui::photo_panel::words;

constexpr std::string_view kLimit = "{limit}";
constexpr std::string_view kKey = "{key}";
/// A mark is at least this many map pixels on a side.
constexpr int32_t kMarkLeast = 64;

/// Puts `limit` in place of `{limit}`.
///
/// @param pattern the sentence
/// @param limit the pixel cap
/// @return the sentence
std::string fill_limit(std::string_view pattern, int32_t limit) {
    std::string text(pattern);
    const std::size_t at = text.find(kLimit);
    if (at == std::string::npos)
        return text;
    text.replace(at, kLimit.size(), std::to_string(limit));
    return text;
}

/// Puts `key` in place of `{key}`.
///
/// @param pattern the sentence
/// @param key the chord
/// @return the sentence
std::string fill_key(std::string_view pattern, std::string_view key) {
    std::string text(pattern);
    const std::size_t at = text.find(kKey);
    if (at == std::string::npos)
        return text;
    text.replace(at, kKey.size(), key);
    return text;
}

/// Widens a mark to 64 map pixels on a side around its centre. The
/// rectangle stays on the map. A drag marks only what the view shows, so
/// the floor is not the view: that would turn every drag into a mark the
/// view's size.
///
/// @param[in,out] area the mark
/// @param map_w the map's pixels across
/// @param map_h the map's pixels down
void floor_mark(photo::PosterArea& area, int32_t map_w, int32_t map_h) {
    int32_t width = std::max(area.width, kMarkLeast);
    int32_t height = std::max(area.height, kMarkLeast);
    if (map_w > 0)
        width = std::min(width, map_w);
    if (map_h > 0)
        height = std::min(height, map_h);
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    const int32_t centre_x = area.x + area.width / 2;
    const int32_t centre_z = area.z + area.height / 2;
    int32_t x = centre_x - width / 2;
    int32_t z = centre_z - height / 2;
    if (map_w > 0) {
        if (x < 0)
            x = 0;
        if (x > map_w - width)
            x = std::max(map_w - width, int32_t{0});
    }
    if (map_h > 0) {
        if (z < 0)
            z = 0;
        if (z > map_h - height)
            z = std::max(map_h - height, int32_t{0});
    }
    area.x = x;
    area.z = z;
    area.width = width;
    area.height = height;
}

/// Map pixels a draw shows at a zoom. A zoom that is not a view returns 1.
///
/// @param draw the draw's pixels on this axis
/// @param zoom output pixels a map pixel
/// @return the map pixels, at least 1
int32_t visible_map(int32_t draw, float zoom) {
    if (!(zoom > 0.0F) || !std::isfinite(zoom) || draw <= 0)
        return 1;
    const double pixels = static_cast<double>(draw) / static_cast<double>(zoom);
    if (!(pixels > 0.0) || pixels > static_cast<double>(std::numeric_limits<int32_t>::max()))
        return 1;
    const long rounded = std::lround(pixels);
    if (rounded < 1 || rounded > std::numeric_limits<int32_t>::max())
        return 1;
    return static_cast<int32_t>(rounded);
}

/// The three extents, in panel order.
///
/// @param plan the plan
/// @param index 0, 1 or 2
/// @return the offer
const photo::PosterOffer& extent_at(const photo::PosterPlan& plan, int index) {
    if (index == static_cast<int>(photo::PosterExtent::marked))
        return plan.marked;
    if (index == static_cast<int>(photo::PosterExtent::map))
        return plan.map;
    return plan.view;
}

/// The rectangle from one corner to another, at least one pixel.
///
/// @param from_x the first column
/// @param from_z the first row
/// @param to_x the other column
/// @param to_z the other row
/// @param[out] area the rectangle
void mark_between(
    int32_t from_x, int32_t from_z, int32_t to_x, int32_t to_z, photo::PosterArea& area
) {
    const int32_t mark_left = std::min(from_x, to_x);
    const int32_t mark_top = std::min(from_z, to_z);
    const int32_t mark_right = std::max(from_x, to_x);
    const int32_t mark_bottom = std::max(from_z, to_z);
    area.x = mark_left;
    area.z = mark_top;
    area.width = mark_right - mark_left + 1;
    area.height = mark_bottom - mark_top + 1;
}

/// Copies kept rows of a strip into the poster.
///
/// @param picture the poster
/// @param poster_width the poster's width
/// @param strip the strip
/// @param drawn the strip's draw
/// @return false when the draw is not the strip
bool blit_strip(
    std::vector<uint8_t>& picture,
    int32_t poster_width,
    const photo::PosterStrip& strip,
    const std::vector<uint8_t>& drawn
) {
    const int32_t draw_height = strip.height + strip.skip_rows;
    if (strip.width <= 0 || strip.height <= 0 || draw_height <= 0 || poster_width <= 0)
        return false;
    if (strip.dest_x > poster_width - strip.width)
        return false;
    const std::size_t draw_bytes =
        static_cast<std::size_t>(strip.width) * static_cast<std::size_t>(draw_height) * 3U;
    if (drawn.size() != draw_bytes)
        return false;
    for (int32_t row = 0; row < strip.height; ++row) {
        const std::size_t from = (static_cast<std::size_t>(row + strip.skip_rows) *
                                  static_cast<std::size_t>(strip.width)) *
                                 3U;
        const std::size_t to =
            (static_cast<std::size_t>(strip.dest_y + row) * static_cast<std::size_t>(poster_width) +
             static_cast<std::size_t>(strip.dest_x)) *
            3U;
        if (to + static_cast<std::size_t>(strip.width) * 3U > picture.size())
            return false;
        std::memcpy(
            picture.data() + to, drawn.data() + from, static_cast<std::size_t>(strip.width) * 3U
        );
    }
    return true;
}

} // namespace

photo::PosterPlan Runtime::photo_poster_plan(const photo::Machine& machine) {
    photo::PosterAsk ask;
    if (match_ != nullptr) {
        const float zoom = match_zoom();
        ask.view_width = visible_map(match_layout_.battlefield_width(), zoom);
        ask.view_height = visible_map(match_layout_.battlefield_height(), zoom);
        ask.map_width = match_->state().game.map_pixel_width;
        ask.map_height = match_->state().game.map_pixel_height;
    }
    ask.camera_x = match_camera_x_;
    ask.camera_z = match_camera_z_;
    if (photo_ != nullptr) {
        ask.marked.x = photo_->poster_mark_x;
        ask.marked.z = photo_->poster_mark_z;
        ask.marked.width = photo_->poster_mark_w;
        ask.marked.height = photo_->poster_mark_h;
        ask.frozen = photo_frozen();
        ask.held = photo_picture_held();
    }
    return photo::plan_poster(ask, machine);
}

void Runtime::set_photo_poster(bool on) {
    if (photo_ == nullptr)
        return;
    if (!on) {
        photo_->poster = false;
        photo_->poster_marking = false;
        return;
    }
    const photo::Machine machine = photo_capture_machine();
    const photo::PosterPlan plan = photo_poster_plan(machine);
    if (plan.button != photo::SizeLock::open)
        return;
    const int current = std::clamp(static_cast<int>(photo_->poster_extent), 0, 2);
    if (extent_at(plan, current).offered) {
        photo_->poster = true;
        return;
    }
    for (int index = 0; index < 3; ++index) {
        if (!extent_at(plan, index).offered)
            continue;
        photo_->poster_extent = static_cast<uint8_t>(index);
        photo_->poster = true;
        return;
    }
}

void Runtime::set_photo_poster_extent(int extent) {
    if (photo_ == nullptr || extent < 0 || extent > 2)
        return;
    const photo::Machine machine = photo_capture_machine();
    const photo::PosterPlan plan = photo_poster_plan(machine);
    if (plan.button != photo::SizeLock::open || !extent_at(plan, extent).offered)
        return;
    photo_->poster_extent = static_cast<uint8_t>(extent);
    photo_->poster = true;
}

void Runtime::set_photo_poster_rect(int32_t x, int32_t z, int32_t width, int32_t height) {
    if (photo_ == nullptr)
        return;
    photo_->poster_mark_x = x;
    photo_->poster_mark_z = z;
    photo_->poster_mark_w = width;
    photo_->poster_mark_h = height;
    set_photo_poster_extent(static_cast<int>(photo::PosterExtent::marked));
}

void Runtime::photo_mark_area() {
    if (photo_ == nullptr || match_ == nullptr || photo_picture_held())
        return;
    photo_->poster_marking = false;
    const float zoom = match_zoom();
    const int32_t view_w = visible_map(match_layout_.battlefield_width(), zoom);
    const int32_t view_h = visible_map(match_layout_.battlefield_height(), zoom);
    photo::PosterArea area;
    area.x = match_camera_x_;
    area.z = match_camera_z_;
    area.width = view_w;
    area.height = view_h;
    floor_mark(area, match_->state().game.map_pixel_width, match_->state().game.map_pixel_height);
    photo_->poster_mark_x = area.x;
    photo_->poster_mark_z = area.z;
    photo_->poster_mark_w = area.width;
    photo_->poster_mark_h = area.height;
    set_photo_poster_extent(static_cast<int>(photo::PosterExtent::marked));
}

void Runtime::photo_poster_grow(int across, int down, int32_t step) {
    if (!photo_poster_takes_drag() || match_ == nullptr || (across == 0 && down == 0))
        return;
    const float zoom = match_zoom();
    const int32_t view_w = visible_map(match_layout_.battlefield_width(), zoom);
    const int32_t view_h = visible_map(match_layout_.battlefield_height(), zoom);
    int32_t grow = step;
    if (grow < 1)
        grow = std::max(kMarkLeast, std::max(view_w, view_h) / 8);
    photo::PosterArea area;
    area.x = photo_->poster_mark_x;
    area.z = photo_->poster_mark_z;
    area.width = photo_->poster_mark_w;
    area.height = photo_->poster_mark_h;
    if (area.width < 1 || area.height < 1) {
        area.x = match_camera_x_;
        area.z = match_camera_z_;
        area.width = view_w;
        area.height = view_h;
    }
    // Only the edge asked for moves, and it stops at the map's edge: Left
    // at the map's left edge leaves the mark as it is.
    const int64_t map_w = match_->state().game.map_pixel_width;
    const int64_t map_h = match_->state().game.map_pixel_height;
    int64_t left = area.x;
    int64_t top = area.z;
    int64_t right = left + area.width;
    int64_t bottom = top + area.height;
    if (across < 0)
        left -= grow;
    else if (across > 0)
        right += grow;
    if (down < 0)
        top -= grow;
    else if (down > 0)
        bottom += grow;
    if (map_w > 0) {
        left = std::max(left, int64_t{0});
        right = std::min(right, map_w);
    }
    if (map_h > 0) {
        top = std::max(top, int64_t{0});
        bottom = std::min(bottom, map_h);
    }
    const int64_t most = std::numeric_limits<int32_t>::max();
    area.x = static_cast<int32_t>(std::clamp(left, -most, most));
    area.z = static_cast<int32_t>(std::clamp(top, -most, most));
    area.width = static_cast<int32_t>(std::clamp(right - left, int64_t{1}, most));
    area.height = static_cast<int32_t>(std::clamp(bottom - top, int64_t{1}, most));
    floor_mark(area, match_->state().game.map_pixel_width, match_->state().game.map_pixel_height);
    photo_->poster_mark_x = area.x;
    photo_->poster_mark_z = area.z;
    photo_->poster_mark_w = area.width;
    photo_->poster_mark_h = area.height;
}

bool Runtime::photo_poster_takes_drag() const {
    return photo_ != nullptr && photo_->poster &&
           photo_->poster_extent == static_cast<uint8_t>(photo::PosterExtent::marked) &&
           !photo_picture_held();
}

void Runtime::photo_poster_drag_begin(float x, float y) {
    if (photo_ == nullptr)
        return;
    const std::optional<std::array<int32_t, 2>> map = poster_map_at(x, y);
    if (!map)
        return;
    photo_->poster_marking = true;
    photo_->poster_mark_moved = false;
    photo_->poster_press_x = x;
    photo_->poster_press_y = y;
    photo_->poster_anchor_x = (*map)[0];
    photo_->poster_anchor_z = (*map)[1];
    photo_->poster_saved_x = photo_->poster_mark_x;
    photo_->poster_saved_z = photo_->poster_mark_z;
    photo_->poster_saved_w = photo_->poster_mark_w;
    photo_->poster_saved_h = photo_->poster_mark_h;
}

void Runtime::photo_poster_drag_move(float x, float y) {
    if (photo_ == nullptr || !photo_->poster_marking || match_ == nullptr)
        return;
    const float dx = x - photo_->poster_press_x;
    const float dy = y - photo_->poster_press_y;
    const float slop = middle_pan::kClickSlop;
    if (!photo_->poster_mark_moved && dx * dx + dy * dy <= slop * slop)
        return;
    const std::optional<std::array<int32_t, 2>> map = poster_map_at(x, y);
    if (!map)
        return;
    photo_->poster_mark_moved = true;
    photo::PosterArea area;
    mark_between(photo_->poster_anchor_x, photo_->poster_anchor_z, (*map)[0], (*map)[1], area);
    floor_mark(area, match_->state().game.map_pixel_width, match_->state().game.map_pixel_height);
    photo_->poster_mark_x = area.x;
    photo_->poster_mark_z = area.z;
    photo_->poster_mark_w = area.width;
    photo_->poster_mark_h = area.height;
}

void Runtime::photo_poster_drag_end() {
    if (photo_ == nullptr)
        return;
    if (photo_->poster_marking && !photo_->poster_mark_moved) {
        photo_->poster_mark_x = photo_->poster_saved_x;
        photo_->poster_mark_z = photo_->poster_saved_z;
        photo_->poster_mark_w = photo_->poster_saved_w;
        photo_->poster_mark_h = photo_->poster_saved_h;
    }
    photo_->poster_marking = false;
    photo_->poster_mark_moved = false;
}

std::string Runtime::photo_poster_reason(photo::SizeLock lock, int32_t limit) {
    if (lock == photo::SizeLock::open)
        return {};
    if (lock == photo::SizeLock::shared)
        return std::string(oa::data::languages::interface_text(words::shared_game));
    if (lock == photo::SizeLock::held)
        return std::string(oa::data::languages::interface_text(words::held_picture));
    if (lock == photo::SizeLock::above) {
        std::string chord = bound_chord_text("camera.classic-view");
        if (chord.empty())
            chord = "Alt+R";
        return fill_key(oa::data::languages::interface_text(words::poster_above), chord);
    }
    if (lock == photo::SizeLock::moving)
        return std::string(oa::data::languages::interface_text(words::freeze_the_picture));
    if (lock == photo::SizeLock::memory)
        return std::string(oa::data::languages::interface_text(words::not_enough_memory));
    if (lock == photo::SizeLock::edge)
        return fill_limit(oa::data::languages::interface_text(words::photo_edge), limit);
    return {};
}

bool Runtime::capture_poster(std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
    rgb.clear();
    width = 0;
    height = 0;
    if (photo_ == nullptr || match_ == nullptr || !selected_tnt_)
        return false;
    photo_->poster_recorded = false;
    // A poster is the map from above. The turned view, and a shared match,
    // are not offered. The processor draws strips; Full draws tiles. Neither
    // path enters the cheat's poster scene.
    const photo::Machine machine = photo_capture_machine();
    const photo::PosterPlan plan = photo_poster_plan(machine);
    if (plan.button != photo::SizeLock::open)
        return false;
    const int extent = std::clamp(static_cast<int>(photo_->poster_extent), 0, 2);
    const photo::PosterOffer& chosen = extent_at(plan, extent);
    if (!chosen.offered || chosen.area.width <= 0 || chosen.area.height <= 0)
        return false;
    if (chosen.area.width > photo::kProcessorEdge || chosen.area.height > photo::kProcessorEdge)
        return false;
    const uint64_t pixels =
        static_cast<uint64_t>(chosen.area.width) * static_cast<uint64_t>(chosen.area.height);
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U)
        return false;

    if (machine.kind == photo::CaptureKind::processor) {
        const std::vector<photo::PosterStrip> strips =
            photo::poster_strips(chosen.area, plan.view.area.width, plan.view.area.height);
        if (strips.empty())
            return false;
        std::vector<uint8_t> picture(static_cast<std::size_t>(pixels) * 3U, 0);
        bool drew = false;
        try {
            drew = restoring_photo_window([&] {
                for (const photo::PosterStrip& strip : strips) {
                    const int32_t draw_height = strip.height + strip.skip_rows;
                    if (strip.width <= 0 || draw_height <= 0 ||
                        strip.width > photo::kProcessorEdge || draw_height > photo::kProcessorEdge)
                        return false;
                    match_camera_x_ = strip.camera_x;
                    match_camera_z_ = strip.camera_z;
                    exact_view_.held = false;
                    std::vector<uint8_t> drawn;
                    if (!draw_processor_photo(drawn, strip.width, draw_height, 1.0))
                        return false;
                    if (!blit_strip(picture, chosen.area.width, strip, drawn))
                        return false;
                }
                return true;
            });
        } catch (const std::exception&) {
            return false;
        }
        if (!drew)
            return false;
        rgb = std::move(picture);
    } else if (machine.kind == photo::CaptureKind::full) {
        std::vector<uint8_t> picture;
        bool drew = false;
        try {
            drew = restoring_photo_window([&] {
                match_camera_x_ = chosen.area.x;
                match_camera_z_ = chosen.area.z;
                match_zoom_ = 1.0F;
                match_zoom_target_ = 1.0F;
                exact_view_.held = false;
                match_layout_ = oa::ui::display_layout::make_battlefield_layout(
                    chosen.area.width, chosen.area.height
                );
                return draw_full_rgb(picture, chosen.area.width, chosen.area.height, 1.0F, machine);
            });
        } catch (const std::exception&) {
            return false;
        }
        if (!drew)
            return false;
        rgb = std::move(picture);
        photo_->sized_by_full = true;
        photo_->sized_supersample = full_ != nullptr ? full_->supersample : uint32_t{1};
    } else
        return false;

    if (rgb.size() != static_cast<std::size_t>(pixels) * 3U)
        return false;
    width = static_cast<uint32_t>(chosen.area.width);
    height = static_cast<uint32_t>(chosen.area.height);
    photo_->poster_recorded = true;
    photo_->poster_recorded_x = chosen.area.x;
    photo_->poster_recorded_z = chosen.area.z;
    return true;
}

std::optional<std::array<int32_t, 2>> Runtime::poster_map_at(float x, float y) const {
    if (match_ == nullptr)
        return std::nullopt;
    const int32_t field_left = match_layout_.left;
    const int32_t field_top = match_layout_.top;
    const int32_t field_right = field_left + match_layout_.battlefield_width() - 1;
    const int32_t field_bottom = field_top + match_layout_.battlefield_height() - 1;
    if (field_right < field_left || field_bottom < field_top)
        return std::nullopt;
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    const oa::present::world_renderer::ScreenPoint pointer{
        std::clamp(static_cast<int32_t>(std::lround(x)), field_left, field_right),
        std::clamp(static_cast<int32_t>(std::lround(y)), field_top, field_bottom)
    };
    const auto map = map_pixel_drawn_at(viewport, pointer);
    if (!map)
        return std::nullopt;
    return std::array<int32_t, 2>{static_cast<int32_t>(map->x), static_cast<int32_t>(map->y)};
}

std::optional<std::array<int32_t, 4>> Runtime::poster_mark_window() const {
    if (photo_ == nullptr || photo_->poster_mark_w <= 1 || photo_->poster_mark_h <= 1)
        return std::nullopt;
    auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    const auto offset = view_offset();
    const auto shift = view_shift();
    if (shift[0] != 0.0 || shift[1] != 0.0) {
        const auto scale = static_cast<double>(viewport.scale > 0.0F ? viewport.scale : 1.0F);
        const double across = offset.x - shift[0] / scale;
        const double down = offset.y - shift[1] / scale;
        viewport.source_x += static_cast<int32_t>(std::floor(across));
        viewport.source_y += static_cast<int32_t>(std::floor(down));
    }
    const oa::present::world_renderer::ScreenPoint corner =
        oa::present::world_renderer::map_pixel_to_screen(
            viewport, {photo_->poster_mark_x, photo_->poster_mark_z}
        );
    const int32_t span_w =
        oa::present::world_renderer::screen_span(viewport, photo_->poster_mark_w);
    const int32_t span_h =
        oa::present::world_renderer::screen_span(viewport, photo_->poster_mark_h);
    return std::array<int32_t, 4>{corner.x, corner.y, span_w, span_h};
}

} // namespace oa::app
