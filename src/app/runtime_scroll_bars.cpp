// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The scroll bars of the frontend screen's panel and of the match HUD's
// panel: bound as a panel's first draw binds them, drawn over the panel,
// driven by the pointer and kept in step with the lists they scroll.
#include "oa/app/runtime.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/formats/fnt.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

namespace oa::app {

namespace {

namespace input = oa::ui::gui_input;

/// Returns a layout's gadget index by name, or nothing.
std::optional<std::size_t>
gadget_index(const oa::ui::gui_layout::Layout& layout, std::string_view name) {
    for (std::size_t index = 1; index < layout.gadgets.size(); ++index)
        if (layout.gadgets[index].common.name == name)
            return index;
    return std::nullopt;
}

/// Tells whether an SDL pointer button presses a scroll bar: the left or the
/// right, as 3.1c takes either.
bool scroll_button(uint8_t button) {
    return button == SDL_BUTTON_LEFT || button == SDL_BUTTON_RIGHT;
}

} // namespace

void Runtime::bind_frontend_scrolls(std::string_view layout, std::string_view sprites) {
    frontend_gray_table_.clear();
    frontend_scrolls_own_art_ = renderer::gaf_named_after(layout, sprites);
    const auto art = renderer::scroll_art(
        frontend_scrolls_own_art_ ? &resources_.sprites : nullptr, resources_.shared_sprites
    );
    frontend_scrolls_ = renderer::bind_layout_scrolls(
        resources_.layout, art, oa::formats::fnt::line_height(resources_.font)
    );
    frontend_scrolls_layout_ = resources_.layout.gadgets.data();
    frontend_scrolls_count_ = resources_.layout.gadgets.size();
}

bool Runtime::first_draw_after_setup(Screen screen) {
    return screen == Screen::new_campaign || screen == Screen::any_mission;
}

void Runtime::bind_set_up_frontend_scrolls(std::string_view layout, std::string_view sprites) {
    bind_frontend_scrolls(layout, sprites);
    if (!first_draw_after_setup(screen_))
        return;
    fill_frontend_list("Campaign", campaign_labels_.size());
    select_frontend_list_row("Campaign", selected_campaign_index_);
    fill_frontend_list("Missions", campaign_mission_labels_.size());
    select_frontend_list_row("Missions", selected_mission_index_);
}

void Runtime::bind_hud_scrolls(std::size_t first) {
    if (!match_hud_)
        return;
    auto& layout = match_hud_->layout;
    const bool keep = hud_scrolls() != nullptr && first > 1;
    if (!keep) {
        hud_gray_table_.clear();
        hud_own_art_ = {};
        append_gaf_file(
            hud_own_art_, "anims/" + oa::data::defs::file_stem(match_hud_panel_) + ".GAF"
        );
        hud_scrolls_ = {};
    }
    auto bound = renderer::bind_layout_scrolls(
        layout,
        renderer::scroll_art(&hud_own_art_, match_hud_->sprites),
        oa::formats::fnt::line_height(match_hud_->font),
        first
    );
    if (!keep) {
        hud_scrolls_ = std::move(bound);
    } else {
        hud_scrolls_.bars.insert(hud_scrolls_.bars.end(), bound.bars.begin(), bound.bars.end());
        hud_scrolls_.lists.insert(hud_scrolls_.lists.end(), bound.lists.begin(), bound.lists.end());
    }
    hud_scrolls_layout_ = layout.gadgets.data();
    hud_scrolls_count_ = layout.gadgets.size();
}

renderer::LayoutScrolls* Runtime::frontend_scrolls() {
    if (frontend_scrolls_layout_ == nullptr ||
        frontend_scrolls_layout_ != resources_.layout.gadgets.data() ||
        frontend_scrolls_count_ != resources_.layout.gadgets.size())
        return nullptr;
    return &frontend_scrolls_;
}

renderer::LayoutScrolls* Runtime::hud_scrolls() {
    if (!match_hud_ || hud_scrolls_layout_ == nullptr ||
        hud_scrolls_layout_ != match_hud_->layout.gadgets.data() ||
        hud_scrolls_count_ != match_hud_->layout.gadgets.size())
        return nullptr;
    return &hud_scrolls_;
}

oa::ui::display_layout::Point Runtime::frontend_panel_point(float x, float y) const {
    if (screen_ == Screen::map_selection && !resources_.layout.gadgets.empty()) {
        const auto& modal_root = resources_.layout.gadgets.front().common;
        x -= static_cast<float>((kCanvasWidth - static_cast<int>(modal_root.width)) / 2);
        y -= static_cast<float>((kCanvasHeight - static_cast<int>(modal_root.height)) / 2);
    }
    const auto origin = panel_origin();
    return {static_cast<int>(x) - origin.x, static_cast<int>(y) - origin.y};
}

oa::ui::display_layout::Point Runtime::hud_source_point(float x, float y) const {
    const auto canvas_x = static_cast<int>(x);
    const auto canvas_y = static_cast<int>(y);
    // On the phone layout every piece of the HUD, panels and dialogs
    // included, shows through its placed region.
    if (oa::ui::display_layout::placed_mode(match_layout_))
        return oa::ui::display_layout::canvas_to_source(match_layout_, canvas_x, canvas_y);
    const auto rows = preferences_panel_rows();
    if (rows.width > 0 && rows.height > 0 && canvas_x >= match_layout_.left &&
        match_layout_.scale > 0.0) {
        // The sub-panel keeps the bars' scale down to its bottom, from the
        // side column's edge.
        const auto unscaled = [this](int value) {
            return static_cast<int>(std::lround(static_cast<double>(value) / match_layout_.scale));
        };
        const oa::ui::display_layout::Point point{
            match_layout_.column_narrowed()
                ? kBattlefieldLeft + unscaled(canvas_x - match_layout_.left)
                : unscaled(canvas_x),
            unscaled(canvas_y)
        };
        if (point.x >= rows.x && point.x < rows.x + rows.width && point.y >= rows.y &&
            point.y < rows.y + rows.height)
            return point;
    }
    if (const auto area = placed_panel_area()) {
        // Each canvas pixel maps to the source pixel the panel's draw shows
        // there, rounding down, so a point left of or above it stays off it.
        const auto& root = match_hud_->layout.gadgets.front().common;
        const auto through = [](int canvas, int start, int length, int source, int source_length) {
            return source + static_cast<int>(std::floor(
                                static_cast<double>(canvas - start) * source_length / length
                            ));
        };
        return {
            through(canvas_x, area->x, area->width, root.x, root.width),
            through(canvas_y, area->y, area->height, root.y, root.height)
        };
    }
    // The source pixel a strip of the side column draws at a canvas pixel,
    // as the strip's blit samples it.
    const auto through = [](int canvas, int start, int length, int source, int source_length) {
        return source +
               static_cast<int>(static_cast<int64_t>(canvas - start) * source_length / length);
    };
    const auto strips = match_hud_strips();
    // A page taller than the side column is drawn scaled down under the
    // rows above its panel: a point on it maps to the page as authored, and
    // one in the blank strip beside it to no gadget.
    if (const auto& page = strips[3]; page.w > 0 && page.h > 0 && canvas_x < match_layout_.left &&
                                      canvas_y >= page.y && canvas_y < page.y + page.h) {
        if (canvas_x < page.x || canvas_x >= page.x + page.w)
            return {-1, -1};
        return {
            through(canvas_x, page.x, page.w, page.source_x, page.source_w),
            through(canvas_y, page.y, page.h, page.source_y, page.source_h)
        };
    }
    // A narrowed side column: the source pixel it draws there.
    if (const auto& column = strips[0];
        match_layout_.column_narrowed() && column.w > 0 && column.h > 0 && canvas_x >= 0 &&
        canvas_x < column.x + column.w && canvas_y >= 0 && canvas_y < column.y + column.h)
        return {
            through(canvas_x, column.x, column.w, column.source_x, column.source_w),
            through(canvas_y, column.y, column.h, column.source_y, column.source_h)
        };
    return oa::ui::display_layout::canvas_to_source(match_layout_, canvas_x, canvas_y);
}

oa::ui::display_layout::Point Runtime::hud_gadget_centre(std::size_t index) const {
    const auto& common = match_hud_->layout.gadgets.at(index).common;
    const int x = common.x + common.width / 2;
    const int y = common.y + common.height / 2;
    // On a page drawn scaled down, or in a narrowed side column, the middle
    // of the canvas pixels that show the centre's source pixel.
    const auto through = [](int source, int start, int length, int canvas, int canvas_length) {
        return canvas + static_cast<int>(
                            (static_cast<int64_t>(source - start) * 2 + 1) * canvas_length /
                            (static_cast<int64_t>(length) * 2)
                        );
    };
    const auto strips = match_hud_strips();
    const auto shows = [x, y](const HudStrip& strip) {
        return strip.w > 0 && strip.h > 0 && x >= strip.source_x &&
               x < strip.source_x + strip.source_w && y >= strip.source_y &&
               y < strip.source_y + strip.source_h;
    };
    for (const auto* strip : {&strips[3], match_layout_.column_narrowed() ? &strips[0] : nullptr})
        if (strip != nullptr && shows(*strip))
            return {
                through(x, strip->source_x, strip->source_w, strip->x, strip->w),
                through(y, strip->source_y, strip->source_h, strip->y, strip->h)
            };
    return oa::ui::display_layout::source_to_canvas(match_layout_, x, y);
}

bool Runtime::route_scroll_pointer(const SDL_Event& event, float x, float y) {
    const bool over_match = screen_ == Screen::match;
    if (over_match && (!match_paused_ || match_finished_))
        return false;
    auto* scrolls = over_match ? hud_scrolls() : frontend_scrolls();
    if (scrolls == nullptr || scrolls->bars.empty())
        return false;
    auto& layout = over_match ? match_hud_->layout : resources_.layout;
    const auto point = over_match ? hud_source_point(x, y) : frontend_panel_point(x, y);
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        // Whether a bar is held does not matter: motion always reaches the
        // screen's own handler as well.
        std::ignore = renderer::move_layout_scrolls(*scrolls, point.x, point.y);
        return false;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && scroll_button(event.button.button)) {
        renderer::refresh_layout_scrolls(*scrolls, layout);
        const auto pressed =
            renderer::press_layout_scrolls(*scrolls, layout, point.x, point.y, frontend_tick());
        if (!pressed.taken)
            return false;
        selected_ = -1;
        if (pressed.changed >= 0)
            scroll_bar_changed(over_match, pressed.changed);
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && scroll_button(event.button.button) &&
        scrolls->hold.bar != input::kNoScrollBar) {
        renderer::refresh_layout_scrolls(*scrolls, layout);
        const auto released = renderer::release_layout_scrolls(*scrolls, layout, point.x, point.y);
        if (released.changed >= 0)
            scroll_bar_changed(over_match, released.changed);
        selected_ = -1;
        return true;
    }
    return false;
}

void Runtime::tick_scroll_bars() {
    const bool over_match = screen_ == Screen::match;
    auto* scrolls = over_match ? hud_scrolls() : frontend_scrolls();
    if (scrolls == nullptr || scrolls->hold.bar == input::kNoScrollBar)
        return;
    auto& layout = over_match ? match_hud_->layout : resources_.layout;
    renderer::refresh_layout_scrolls(*scrolls, layout);
    const auto changed = renderer::tick_layout_scrolls(*scrolls, layout, frontend_tick());
    if (changed >= 0)
        scroll_bar_changed(over_match, changed);
}

void Runtime::scroll_bar_changed(bool over_hud, int32_t gadget) {
    if (gadget < 0)
        return;
    const auto index = static_cast<std::size_t>(gadget);
    if (over_hud) {
        if (team_panel_open())
            share_bar_moved(index);
        else if (match_preferences_open())
            preferences_bar_moved(index);
        return;
    }
    switch (screen_) {
    case Screen::options:
    case Screen::sound:
    case Screen::visuals:
    case Screen::speeds:
    case Screen::music:
        options_bar_moved(index);
        break;
    default:
        // A list's bar has moved the list; the frame shows it.
        break;
    }
}

void Runtime::fill_frontend_list(std::string_view name, std::size_t count) {
    auto* scrolls = frontend_scrolls();
    if (scrolls == nullptr)
        return;
    if (const auto index = gadget_index(resources_.layout, name))
        renderer::fill_layout_list(
            *scrolls,
            resources_.layout,
            *index,
            static_cast<int32_t>(std::min<std::size_t>(count, INT16_MAX))
        );
}

void Runtime::select_frontend_list_row(std::string_view name, std::size_t row) {
    auto* scrolls = frontend_scrolls();
    if (scrolls == nullptr)
        return;
    if (const auto index = gadget_index(resources_.layout, name))
        renderer::select_layout_list_row(
            *scrolls,
            resources_.layout,
            *index,
            static_cast<int32_t>(std::min<std::size_t>(row, INT16_MAX))
        );
}

std::optional<std::size_t> Runtime::step_frontend_list_row(std::string_view name, bool forward) {
    auto* scrolls = frontend_scrolls();
    if (scrolls == nullptr)
        return std::nullopt;
    const auto index = gadget_index(resources_.layout, name);
    if (!index)
        return std::nullopt;
    // Whether the selection moved on the page does not matter: the row
    // selected afterwards is read back from the list below.
    std::ignore = renderer::step_layout_list_row(*scrolls, resources_.layout, *index, forward);
    const auto* list = renderer::find_layout_list(*scrolls, *index);
    if (list == nullptr || list->list.selection < 0)
        return std::nullopt;
    return static_cast<std::size_t>(list->list.selection);
}

std::optional<std::size_t> Runtime::frontend_list_first(std::string_view name) {
    auto* scrolls = frontend_scrolls();
    if (scrolls == nullptr)
        return std::nullopt;
    const auto index = gadget_index(resources_.layout, name);
    const auto* list = index ? renderer::find_layout_list(*scrolls, *index) : nullptr;
    if (list == nullptr)
        return std::nullopt;
    return static_cast<std::size_t>(std::max<int16_t>(0, list->list.first));
}

std::optional<std::size_t> Runtime::frontend_list_row_at(std::string_view name, float canvas_y) {
    auto* scrolls = frontend_scrolls();
    if (scrolls == nullptr)
        return std::nullopt;
    const auto index = gadget_index(resources_.layout, name);
    const auto* list = index ? renderer::find_layout_list(*scrolls, *index) : nullptr;
    if (list == nullptr)
        return std::nullopt;
    int32_t row = 0;
    const auto point = frontend_panel_point(0.0F, canvas_y);
    if (input::scroll_list_press(list->list, scrolls->line_height, point.y, row) ==
        input::ScrollListPress::missed)
        return std::nullopt;
    return static_cast<std::size_t>(row);
}

} // namespace oa::app
