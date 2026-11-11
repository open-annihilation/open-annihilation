// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match's layout for the window: the 3.1c layout, or with touch
// controls on a phone the full-bleed battlefield with the HUD's pieces in
// placed regions; the safe area; where the battlefield overlays go; the
// regions' drawing, and the 3.1c pictures the pad's build ring shows
// (docs/touch-controls.md).
#include "oa/app/runtime.hpp"
#include "touch_state.hpp"
#include "oa/app/scaled_world.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <variant>

namespace oa::app {

namespace layout = oa::ui::display_layout;
namespace touch_hud = oa::ui::touch_hud;

namespace {

/// The source rectangle of the minimap's well: the side column's top square.
constexpr layout::Rect kMinimapSource{0, 0, layout::kSourceLeft, layout::kSourceLeft};
/// The source rectangle of the resource readouts: the top bar.
constexpr layout::Rect kResourcesSource{
    layout::kSourceLeft, 0, layout::kSourceBattlefieldWidth, layout::kSourceTop
};

/// The largest scale, in points per source pixel, a panel sheet grows to.
constexpr double kSheetMaxScale = 1.5;

/// The order page's standing order toggles, in the order the MORE sheet shows them.
constexpr std::array<std::string_view, 4> kStandingToggles{"FIREORD", "MOVEORD", "ONOFF", "CLOAK"};

/// The order page's command words in the order a button's name is tried, as the order panel
/// tries them: UNLOAD before LOAD, which it holds.
constexpr std::array<std::string_view, 11> kOrderWords{
    "MOVE",
    "STOP",
    "ATTACK",
    "BLAST",
    "DEFEND",
    "REPAIR",
    "PATROL",
    "RECLAIM",
    "CAPTURE",
    "UNLOAD",
    "LOAD",
};

/// The orders in the order the phone's order rail and sheets list them.
constexpr std::array<std::string_view, 11> kOrderPriority{
    "MOVE",
    "ATTACK",
    "PATROL",
    "DEFEND",
    "STOP",
    "REPAIR",
    "RECLAIM",
    "CAPTURE",
    "LOAD",
    "UNLOAD",
    "BLAST",
};

/// Returns whether a button's name is one of the standing order toggles (whose names may hold
/// a command word: MOVEORD holds MOVE).
///
/// @param name the button's name
/// @return whether it names FIREORD, MOVEORD, ONOFF or CLOAK
bool standing_toggle(std::string_view name) {
    return std::any_of(kStandingToggles.begin(), kStandingToggles.end(), [&](auto toggle) {
        return name.find(toggle) != std::string_view::npos;
    });
}

/// Returns the command word a button's name holds, as the order panel reads it.
///
/// @param name the button's name
/// @return the first command word the name holds; empty for none or for a standing toggle
std::string_view order_word(std::string_view name) {
    if (standing_toggle(name))
        return {};
    for (const auto word : kOrderWords)
        if (name.find(word) != std::string_view::npos)
            return word;
    return {};
}

/// Returns whether a gadget of the loaded HUD page shows: active, with a size.
///
/// @param gadget the gadget
/// @return whether it is drawn and can be pressed
bool gadget_shown(const oa::ui::gui_layout::Gadget& gadget) {
    return gadget.common.active != 0 && gadget.common.width > 0 && gadget.common.height > 0;
}

/// Returns whether a gadget is a button.
///
/// @param gadget the gadget
/// @return whether its type is button
bool is_button(const oa::ui::gui_layout::Gadget& gadget) {
    return gadget.common.type == oa::ui::gui_layout::GadgetType::button;
}

/// Returns whether a rectangle has no area.
///
/// @param rect the rectangle
/// @return whether its width or height is not positive
bool empty(const layout::Rect& rect) {
    return rect.width <= 0 || rect.height <= 0;
}

/// Returns a rectangle clipped to a surface of a size.
///
/// @param rect the rectangle
/// @param width the surface's width
/// @param height the surface's height
/// @return the part of the rectangle on the surface; empty when none is
layout::Rect clipped(const layout::Rect& rect, int width, int height) {
    const int left = std::max(rect.x, 0);
    const int top = std::max(rect.y, 0);
    const int right = std::min(rect.x + rect.width, width);
    const int bottom = std::min(rect.y + rect.height, height);
    layout::Rect part{};
    if (right <= left || bottom <= top)
        return part;
    part.x = left;
    part.y = top;
    part.width = right - left;
    part.height = bottom - top;
    return part;
}

/// The phone regions' places used while the touch frame was not laid out for a phone (it holds
/// no controls), from the phone layout's values in points: the minimap at the top of the left
/// thumb column, the resource strip from the column to the pause and menu buttons, and the panel
/// sheet over the safe area less a margin.
struct FallbackRects {
    layout::Rect minimap{};
    layout::Rect resources{};
    layout::Rect panel_sheet{};
};

/// Returns the phone regions' places for a layout while the touch frame has no controls.
///
/// @param laid_out the phone layout
/// @param left_handed whether the left-handed layout mirrors them
/// @param control_scale the touch layer's points in window points (the Control size)
/// @return the places, in canvas pixels
FallbackRects
fallback_rects(const layout::MatchLayout& laid_out, bool left_handed, double control_scale) {
    const double density =
        (laid_out.px_per_point > 0.0 ? laid_out.px_per_point : 1.0) * control_scale;
    const auto points = [density](double value) {
        return static_cast<int>(std::lround(value * density));
    };
    const auto& safe = laid_out.safe;
    const int safe_right = laid_out.width - safe.right;
    const int safe_bottom = laid_out.height - safe.bottom;
    FallbackRects rects;
    rects.minimap = {safe.left + points(8), safe.top + points(8), points(104), points(104)};
    // PAUSE and MENU, 44 pt each with a 4 pt gap, 8 pt inside the safe right edge.
    const int pause_left = safe_right - points(8) - points(44 + 4 + 44);
    const int strip_left = rects.minimap.x + rects.minimap.width + points(12);
    rects.resources = {
        strip_left,
        safe.top + points(8),
        std::max(0, pause_left - points(12) - strip_left),
        points(28)
    };
    rects.panel_sheet = {
        safe.left + points(8),
        safe.top + points(8),
        std::max(0, safe_right - safe.left - points(16)),
        std::max(0, safe_bottom - safe.top - points(16))
    };
    if (left_handed) {
        const auto mirror = [&laid_out](layout::Rect& rect) {
            rect.x = laid_out.width - rect.x - rect.width;
        };
        mirror(rects.minimap);
        mirror(rects.resources);
        mirror(rects.panel_sheet);
    }
    return rects;
}

/// Returns a region showing a source rectangle in a canvas rectangle.
///
/// @param source the HUD layer's rectangle
/// @param canvas the canvas rectangle
/// @param role what the region shows
/// @param gadget the gadget's index, -1 for none
/// @return the region
layout::PlacedRegion placed(
    const layout::Rect& source, const layout::Rect& canvas, layout::RegionRole role, int gadget
) {
    layout::PlacedRegion region{};
    region.source = source;
    region.canvas = canvas;
    region.role = role;
    region.gadget = static_cast<int16_t>(gadget);
    return region;
}

} // namespace

layout::MatchLayout Runtime::make_window_match_layout(
    int width, int height, int window_width, int window_height, layout::Insets safe
) const {
    const double px_per_point =
        window_width > 0 ? static_cast<double>(width) / static_cast<double>(window_width) : 1.0;
    const auto to_canvas = [px_per_point](int points) {
        return static_cast<int>(std::lround(static_cast<double>(points) * px_per_point));
    };
    layout::Insets canvas_safe{};
    canvas_safe.left = to_canvas(safe.left);
    canvas_safe.top = to_canvas(safe.top);
    canvas_safe.right = to_canvas(safe.right);
    canvas_safe.bottom = to_canvas(safe.bottom);
    layout::MatchLayout laid_out{};
    if (touch_controls_active()) {
        // The class comes from the window's size in the touch layer's points, the Control size
        // making them larger; the 3.1c chrome keeps the window's points.
        const float scale = std::max(touch_control_scale(), 0.001F);
        const int width_points = window_width > 0 ? window_width : width;
        const int height_points = window_height > 0 ? window_height : height;
        if (touch_hud::classify_device(
                static_cast<int>(static_cast<float>(width_points) / scale),
                static_cast<int>(static_cast<float>(height_points) / scale)
            ) == touch_hud::DeviceClass::phone)
            return layout::make_phone_layout(width, height, px_per_point, canvas_safe);
        // On a canvas of device pixels the chrome keeps its size in points.
        laid_out =
            px_per_point > 1.0
                ? layout::make_match_layout(width, height, layout::kMaxChromeScale * px_per_point)
                : layout::make_match_layout(width, height);
    } else {
        laid_out = layout::make_match_layout(width, height, match_chrome_most_scale());
    }
    laid_out.px_per_point = px_per_point;
    laid_out.safe = canvas_safe;
    return laid_out;
}

layout::Insets Runtime::window_safe_insets() const {
    if (const auto* state = touch_state_if_made(); state != nullptr && state->safe_override)
        return *state->safe_override;
    if (sdl_.window == nullptr)
        return {};
    SDL_Rect area{};
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSafeArea(sdl_.window, &area) || area.w <= 0 || area.h <= 0 ||
        !SDL_GetWindowSize(sdl_.window, &window_width, &window_height))
        return {};
    layout::Insets insets{};
    insets.left = std::max(0, area.x);
    insets.top = std::max(0, area.y);
    insets.right = std::max(0, window_width - area.x - area.w);
    insets.bottom = std::max(0, window_height - area.y - area.h);
    return insets;
}

layout::Rect Runtime::overlay_area() const {
    // The touch controls' clear area, or the pad HUD's: the battlefield less what they cover.
    const auto* state = touch_state_if_made();
    if (state != nullptr && (touch_controls_active() || state->hud.pad.hud) && state->frame_ready &&
        state->frame.clear.width > 0 && state->frame.clear.height > 0)
        return state->frame.clear;
    layout::Rect area{};
    area.x = match_layout_.battlefield_x();
    area.y = match_layout_.battlefield_y();
    area.width = match_layout_.battlefield_width();
    area.height = match_layout_.battlefield_height();
    return area;
}

Runtime::SheetGadgets Runtime::drawer_sheet_gadgets() const {
    // A sheet's gadgets fill its cells one for one.
    static_assert(
        std::tuple_size_v<decltype(SheetGadgets::toggles)> == touch_hud::max_more_cells &&
        std::tuple_size_v<decltype(SheetGadgets::buttons)> == touch_hud::max_drawer_cells &&
        touch_hud::max_drawer_cells == touch_hud::max_more_cells
    );
    SheetGadgets sheet{};
    // The in-game menu's panels are no build or orders page.
    if (!match_hud_ || match_hud_->layout.gadgets.empty() || pause_menu_shown())
        return sheet;
    const auto& gadgets = match_hud_->layout.gadgets;
    const auto add = [&sheet](std::size_t index) {
        if (sheet.button_count < sheet.buttons.size())
            sheet.buttons[sheet.button_count++] = static_cast<int16_t>(index);
    };
    const auto* state = touch_state_if_made();
    const bool orders_tab =
        state != nullptr && state->hud.drawer_tab == touch_hud::DrawerTab::orders;
    if (!orders_tab) {
        // The build page's tiles, in the page's order.
        constexpr uint8_t build_tile =
            oa::ui::hud::kCommonUnitButton | oa::ui::hud::kCommonWeaponButton;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (is_button(gadgets[index]) && gadget_shown(gadgets[index]) &&
                (static_cast<uint8_t>(gadgets[index].common.common_attributes) & build_tile) != 0)
                add(index);
        return sheet;
    }
    // The orders page's toggles the selection can set, then its order buttons, as the page
    // shows them.
    for (const auto toggle : kStandingToggles)
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (is_button(gadgets[index]) && gadget_shown(gadgets[index]) &&
                std::string_view(gadgets[index].common.name).find(toggle) !=
                    std::string_view::npos) {
                if (gadget_command_available(gadgets[index]))
                    add(index);
                break;
            }
    for (const auto word : kOrderPriority)
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (is_button(gadgets[index]) && gadget_shown(gadgets[index]) &&
                order_word(gadgets[index].common.name) == word &&
                gadget_command_available(gadgets[index])) {
                add(index);
                break;
            }
    return sheet;
}

Runtime::SheetGadgets Runtime::more_sheet_gadgets() const {
    SheetGadgets sheet{};
    if (!match_hud_ || match_hud_->layout.gadgets.empty() || pause_menu_shown())
        return sheet;
    const auto& gadgets = match_hud_->layout.gadgets;
    // The standing order toggles the loaded page has and the selection can set (the panel
    // shows the others greyed; the sheet leaves them out).
    for (const auto toggle : kStandingToggles)
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (is_button(gadgets[index]) && gadget_shown(gadgets[index]) &&
                std::string_view(gadgets[index].common.name).find(toggle) !=
                    std::string_view::npos) {
                if (gadget_command_available(gadgets[index]) &&
                    sheet.toggle_count < sheet.toggles.size())
                    sheet.toggles[sheet.toggle_count++] = static_cast<int16_t>(index);
                break;
            }
    // The orders the rail shows (every slot but MORE) stay off the sheet.
    const auto* state = touch_state_if_made();
    const auto on_rail = [state](std::string_view word) {
        if (state == nullptr)
            return false;
        const auto count = std::min<std::size_t>(state->hud.rail_count, state->hud.rail.size());
        for (std::size_t slot = 0; slot < count; ++slot) {
            const auto& rail = state->hud.rail[slot];
            if (!rail.more && touch_hud::order_name(rail.order) == word)
                return true;
        }
        return false;
    };
    for (const auto word : kOrderPriority) {
        if (on_rail(word))
            continue;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (is_button(gadgets[index]) && gadget_shown(gadgets[index]) &&
                order_word(gadgets[index].common.name) == word &&
                gadget_command_available(gadgets[index])) {
                if (sheet.button_count < sheet.buttons.size())
                    sheet.buttons[sheet.button_count++] = static_cast<int16_t>(index);
                break;
            }
    }
    return sheet;
}

touch_hud::Frame PhoneHudAccess::touch_frame(Runtime& runtime, touch_hud::HudState& hud) {
    const auto& laid_out = runtime.match_layout_;
    const auto* state = runtime.touch_state_if_made();
    hud = state != nullptr ? state->hud : touch_hud::HudState{};
    const bool left_handed = runtime.engine_settings().touch_left_handed;
    // The touch layer's points: the window's times the Control size.
    const float px_per_point =
        static_cast<float>(laid_out.px_per_point) * runtime.touch_control_scale();
    // The dispatcher's frame, while it was laid out for this canvas, safe area and hand.
    if (state != nullptr && state->frame_ready) {
        const auto& seen = state->viewport;
        if (seen.width == laid_out.width && seen.height == laid_out.height &&
            seen.device == touch_hud::DeviceClass::phone &&
            std::abs(seen.px_per_point - px_per_point) < 1.0e-4F &&
            seen.safe.left == laid_out.safe.left && seen.safe.top == laid_out.safe.top &&
            seen.safe.right == laid_out.safe.right && seen.safe.bottom == laid_out.safe.bottom &&
            seen.left_handed == left_handed)
            return state->frame;
    }
    touch_hud::Viewport viewport{};
    viewport.width = laid_out.width;
    viewport.height = laid_out.height;
    viewport.px_per_point = px_per_point;
    viewport.safe = laid_out.safe;
    viewport.device = touch_hud::DeviceClass::phone;
    viewport.left_handed = left_handed;
    viewport.chrome = laid_out;
    viewport.chrome.placed_count = 0;
    hud.in_match = true;
    // The cells the open sheet holds are the gadgets it shows.
    if (hud.sheet == touch_hud::Sheet::drawer) {
        hud.drawer_cells_wanted = runtime.drawer_sheet_gadgets().button_count;
    } else if (hud.sheet == touch_hud::Sheet::more) {
        const auto more = runtime.more_sheet_gadgets();
        hud.more_toggle_count = more.toggle_count;
        hud.more_button_count = more.button_count;
    }
    return touch_hud::lay_out(viewport, hud);
}

layout::Rect PhoneHudAccess::sheet_area(
    const Runtime& runtime, const touch_hud::Frame& frame, int source_width, int source_height
) {
    // A frame with no controls was not laid out for a phone.
    const auto area =
        frame.control_count > 0
            ? frame.panel_sheet
            : fallback_rects(runtime.match_layout_, false, runtime.touch_control_scale())
                  .panel_sheet;
    // A small panel grows to at most kSheetMaxScale points per source pixel, so it leaves the
    // battlefield in view.
    const double density =
        runtime.match_layout_.px_per_point > 0.0 ? runtime.match_layout_.px_per_point : 1.0;
    return layout::fit_inside(area, source_width, source_height, kSheetMaxScale * density);
}

void Runtime::refresh_placed_hud_regions() {
    if (!match_layout_.phone)
        return;
    match_layout_.placed_count = 0;
    match_layout_.chrome_scale = 0.0;
    auto& phone = touch_state().phone;
    phone.unit_info_source = {};
    touch_hud::HudState hud{};
    const auto frame = PhoneHudAccess::touch_frame(*this, hud);
    const auto fallback =
        fallback_rects(match_layout_, engine_settings().touch_left_handed, touch_control_scale());
    const auto add = [this](
                         const layout::Rect& source,
                         const layout::Rect& canvas,
                         layout::RegionRole role,
                         int gadget
                     ) {
        if (!empty(source) && !empty(canvas))
            std::ignore = match_layout_.add_placed(placed(source, canvas, role, gadget));
    };
    // The minimap's well and the resource readouts, where the frame puts them (the minimap
    // has none while the drawer covers the column); a frame with no controls was not laid out
    // for a phone.
    const bool laid_out = frame.control_count > 0;
    add(
        kMinimapSource, laid_out ? frame.minimap : fallback.minimap, layout::RegionRole::minimap, -1
    );
    // A replay shows no resources under every player or the whole map.
    if (!replay_top_bar_hidden())
        add(kResourcesSource,
            laid_out ? frame.resources : fallback.resources,
            layout::RegionRole::readout,
            -1);
    // An open sheet's gadgets, each fitted whole into its cell.
    const auto sheet = hud.sheet;
    const auto add_gadget = [&](int16_t index, const layout::Rect& cell) {
        if (!match_hud_ || index < 0 ||
            static_cast<std::size_t>(index) >= match_hud_->layout.gadgets.size())
            return;
        const auto& common = match_hud_->layout.gadgets[static_cast<std::size_t>(index)].common;
        const layout::Rect source{common.x, common.y, common.width, common.height};
        add(source,
            layout::fit_inside(cell, common.width, common.height),
            layout::RegionRole::gadget,
            index);
    };
    if (sheet == touch_hud::Sheet::drawer) {
        const auto gadgets = drawer_sheet_gadgets();
        const auto count = std::min<std::size_t>(
            {gadgets.button_count, frame.drawer_cell_count, frame.drawer_cells.size()}
        );
        for (std::size_t cell = 0; cell < count; ++cell)
            add_gadget(gadgets.buttons[cell], frame.drawer_cells[cell]);
    } else if (sheet == touch_hud::Sheet::more) {
        // The toggles fill the first cells, the order buttons the cells after them.
        const auto gadgets = more_sheet_gadgets();
        const std::size_t cells =
            std::min<std::size_t>(frame.more_cell_count, frame.more_cells.size());
        const std::size_t toggle_cells = std::min<std::size_t>(hud.more_toggle_count, cells);
        for (std::size_t cell = 0; cell < std::min<std::size_t>(gadgets.toggle_count, toggle_cells);
             ++cell)
            add_gadget(gadgets.toggles[cell], frame.more_cells[cell]);
        for (std::size_t button = 0; button < gadgets.button_count && toggle_cells + button < cells;
             ++button)
            add_gadget(gadgets.buttons[button], frame.more_cells[toggle_cells + button]);
    }
    // The in-game menu and the panels it opens show whole as one sheet over everything; with
    // none, an open unit info panel does.
    const int hud_width =
        match_hud_cpu_.width != 0 ? static_cast<int>(match_hud_cpu_.width) : kCanvasWidth;
    const int hud_height =
        match_hud_cpu_.height != 0 ? static_cast<int>(match_hud_cpu_.height) : kCanvasHeight;
    if (pause_menu_shown() && match_hud_ && !match_hud_->layout.gadgets.empty()) {
        const auto& root = match_hud_->layout.gadgets.front().common;
        const auto source =
            clipped({root.x, root.y, root.width, root.height}, hud_width, hud_height);
        if (!empty(source))
            add(source,
                PhoneHudAccess::sheet_area(*this, frame, source.width, source.height),
                layout::RegionRole::sheet,
                -1);
    } else if (unit_info_panel_ && !unit_info_panel_->frame.rgb.empty()) {
        const auto& root = unit_info_panel_->root;
        const auto source = clipped(
            {root.x, root.y, root.width, root.height},
            std::min(hud_width, static_cast<int>(unit_info_panel_->frame.width)),
            std::min(hud_height, static_cast<int>(unit_info_panel_->frame.height))
        );
        if (!empty(source)) {
            add(source,
                PhoneHudAccess::sheet_area(*this, frame, source.width, source.height),
                layout::RegionRole::sheet,
                -1);
            phone.unit_info_source = source;
        }
    }
    match_layout_.chrome_scale = layout::largest_region_scale(match_layout_);
}

std::array<RingPicture, touch_hud::build_ring_slot_count>
PhoneHudAccess::build_ring_pictures(const Runtime& runtime, const touch_hud::BuildRing& ring) {
    std::array<RingPicture, touch_hud::build_ring_slot_count> pictures{};
    constexpr std::size_t bytes_per_pixel = 3;
    const auto& hud = runtime.match_hud_cpu_;
    if (!runtime.match_hud_ || hud.rgb.empty() ||
        hud.rgb.size() < static_cast<std::size_t>(hud.width) * hud.height * bytes_per_pixel)
        return pictures;
    const auto& gadgets = runtime.match_hud_->layout.gadgets;
    const int hud_width = static_cast<int>(runtime.match_hud_cpu_.width);
    const int hud_height = static_cast<int>(runtime.match_hud_cpu_.height);
    for (std::size_t slot = 0; slot < pictures.size(); ++slot) {
        const auto& wedge = ring.wedges[slot];
        if (wedge.gadget < 0 || static_cast<std::size_t>(wedge.gadget) >= gadgets.size())
            continue;
        // The gadget's own pixels, as the drawer's placed region shows them: a gadget that lies
        // partly off the HUD layer shows none.
        const auto& common = gadgets[static_cast<std::size_t>(wedge.gadget)].common;
        const layout::Rect whole{common.x, common.y, common.width, common.height};
        const auto source = clipped(whole, hud_width, hud_height);
        if (empty(source) || source.width != whole.width || source.height != whole.height)
            continue;
        pictures[slot].source = source;
        pictures[slot].canvas = layout::fit_inside(wedge.picture, source.width, source.height);
    }
    return pictures;
}

uint64_t PhoneHudAccess::build_ring_picture_signature(
    const Runtime& runtime, const touch_hud::BuildRing& ring
) {
    // FNV-1a over each picture's place and pixels.
    constexpr uint64_t offset = 14695981039346656037ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    constexpr std::size_t bytes_per_pixel = 3;
    uint64_t hash = offset;
    bool any = false;
    const auto& hud = runtime.match_hud_cpu_;
    for (const auto& picture : build_ring_pictures(runtime, ring)) {
        if (empty(picture.source))
            continue;
        any = true;
        for (const int value :
             {picture.source.x, picture.source.y, picture.source.width, picture.source.height}) {
            hash ^= static_cast<uint32_t>(value);
            hash *= prime;
        }
        for (int row = picture.source.y; row < picture.source.y + picture.source.height; ++row) {
            const uint8_t* pixel = hud.rgb.data() + (static_cast<std::size_t>(row) * hud.width +
                                                     static_cast<std::size_t>(picture.source.x)) *
                                                        bytes_per_pixel;
            const std::size_t count =
                static_cast<std::size_t>(picture.source.width) * bytes_per_pixel;
            for (std::size_t at = 0; at < count; ++at) {
                hash ^= pixel[at];
                hash *= prime;
            }
        }
    }
    return any ? hash : 0U;
}

void PhoneHudAccess::prepare_frame(Runtime& runtime) {
    if (!runtime.match_layout_.phone)
        return;
    runtime.refresh_placed_hud_regions();
    // The unit info panel is drawn apart from the HUD layer; its pixels go where its region
    // shows them, a part of the source battlefield the phone layout shows nowhere else.
    const auto& source = runtime.touch_state().phone.unit_info_source;
    if (empty(source) || !runtime.unit_info_panel_ || runtime.match_hud_cpu_.rgb.empty())
        return;
    runtime.blit_rect(
        runtime.match_hud_cpu_,
        runtime.unit_info_panel_->frame,
        source.x,
        source.y,
        source.x,
        source.y,
        source.width,
        source.height
    );
}

void PhoneHudAccess::blit_regions(Runtime& runtime, oa::ui::frontend_renderer::Surface& frame) {
    const auto& laid_out = runtime.match_layout_;
    if (!layout::placed_mode(laid_out) || runtime.match_hud_cpu_.rgb.empty())
        return;
    const std::size_t count =
        std::min<std::size_t>(laid_out.placed_count, layout::kMaxPlacedRegions);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& region = laid_out.placed[index];
        runtime.scale_blit(
            frame,
            runtime.match_hud_cpu_,
            region.canvas.x,
            region.canvas.y,
            region.canvas.width,
            region.canvas.height,
            region.source.x,
            region.source.y,
            region.source.width,
            region.source.height
        );
    }
}

bool Runtime::placed_hud_covers(float x, float y) const {
    const auto canvas_x = static_cast<int>(std::floor(x));
    const auto canvas_y = static_cast<int>(std::floor(y));
    if (layout::hud_covers(match_layout_, canvas_x, canvas_y))
        return true;
    const auto* state = touch_state_if_made();
    if (!touch_controls_active() && (state == nullptr || !state->hud.pad.hud))
        return false;
    return state != nullptr && state->frame_ready &&
           touch_hud::covers(state->frame, layout::Point{canvas_x, canvas_y});
}

void Runtime::compose_placed_hud_regions(renderer::Surface& frame) {
    if (!layout::placed_mode(match_layout_) || match_hud_cpu_.rgb.empty() ||
        frame.width != static_cast<uint32_t>(match_layout_.width) ||
        frame.height != static_cast<uint32_t>(match_layout_.height))
        return;
    // Drawn over a frame whose colours are already corrected: each region's
    // pixels are corrected as they land, the region's whole rectangle
    // rewritten from the HUD layer first.
    const std::size_t count =
        std::min<std::size_t>(match_layout_.placed_count, layout::kMaxPlacedRegions);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& region = match_layout_.placed[index];
        const auto source = clipped(
            region.source,
            static_cast<int>(match_hud_cpu_.width),
            static_cast<int>(match_hud_cpu_.height)
        );
        if (empty(source) || source.width != region.source.width ||
            source.height != region.source.height)
            continue;
        scale_blit(
            frame,
            match_hud_cpu_,
            region.canvas.x,
            region.canvas.y,
            region.canvas.width,
            region.canvas.height,
            region.source.x,
            region.source.y,
            region.source.width,
            region.source.height
        );
        const auto drawn =
            clipped(region.canvas, static_cast<int>(frame.width), static_cast<int>(frame.height));
        if (gamma_identity_ || empty(drawn))
            continue;
        for (int row = drawn.y; row < drawn.y + drawn.height; ++row)
            apply_gamma_rgb(
                frame.rgb.data() + (static_cast<std::size_t>(row) * frame.width +
                                    static_cast<std::size_t>(drawn.x)) *
                                       3U,
                static_cast<std::size_t>(drawn.width),
                3
            );
    }
}

void Runtime::present_placed_hud_regions() {
    if (!layout::placed_mode(match_layout_) || match_layout_.placed_count == 0 ||
        match_hud_tex_ == nullptr || match_hud_cpu_.rgb.empty() || sdl_.renderer == nullptr)
        return;
    // Basic and Full draw them by the chrome's filter, as they draw the strips.
    if (accelerated_presentation()) {
        draw_accelerated_hud_strips();
        return;
    }
    const std::size_t count =
        std::min<std::size_t>(match_layout_.placed_count, layout::kMaxPlacedRegions);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& region = match_layout_.placed[index];
        if (empty(region.source) || empty(region.canvas))
            continue;
        const SDL_FRect source{
            static_cast<float>(region.source.x),
            static_cast<float>(region.source.y),
            static_cast<float>(region.source.width),
            static_cast<float>(region.source.height)
        };
        const SDL_FRect destination{
            static_cast<float>(region.canvas.x),
            static_cast<float>(region.canvas.y),
            static_cast<float>(region.canvas.width),
            static_cast<float>(region.canvas.height)
        };
        if (!SDL_RenderTexture(sdl_.renderer, match_hud_tex_, &source, &destination))
            throw_present_error("SDL_RenderTexture");
    }
}

} // namespace oa::app
