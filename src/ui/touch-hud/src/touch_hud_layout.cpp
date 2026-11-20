// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The tablet and phone layouts, the slim pad HUD, the phone rail's
// capacity, the clear area, the rings' controls, the group ring's place and
// the left-handed mirror (touch_hud.hpp). Every size is in points, turned
// into canvas pixels with Viewport::px_per_point; every control lies inside
// the safe area.
#include "oa/ui/touch_hud.hpp"

#include "touch_hud_rects.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace oa::ui::touch_hud {

namespace {

// Sizes shared by both layouts, in points.
constexpr float edge_points = 8.0f;          ///< controls keep this far from an edge
constexpr float banner_width = 420.0f;       ///< the armed-order banner
constexpr float banner_height = 36.0f;       ///< the armed-order banner
constexpr float banner_cancel_width = 44.0f; ///< the banner's ✕ at its right end
constexpr float place_cancel_width = 140.0f; ///< ✕ CANCEL
constexpr float place_height = 48.0f;        ///< the placement bar
constexpr float place_gap = 12.0f;           ///< between the bar and a control it moves beside
constexpr float sheet_padding = 8.0f;        ///< inside a sheet's panel
constexpr float menu_row_height = 44.0f;     ///< one menu item
constexpr float menu_row_gap = 4.0f;         ///< between menu items
constexpr float menu_column_width = 160.0f;  ///< one column of menu items
constexpr int min_menu_columns = 2;          ///< SELECT ▾ uses at least two columns
constexpr int max_menu_columns = 4;          ///< and more only when two do not fit the height
constexpr float tip_width = 360.0f;          ///< the help bubble
constexpr float tip_height = 56.0f;          ///< the help bubble: two lines
constexpr float tip_offset = 16.0f;          ///< between the bubble and the point it names
constexpr float min_chip_width = 44.0f;      ///< group chips shrink to this before any is left out
constexpr int max_placement_moves = 4;       ///< tries the placement bar makes to clear controls

// Tablet sizes, in points.
constexpr float tablet_inset = 12.0f;           ///< the thumb column from the battlefield's left
constexpr float tablet_gap = 6.0f;              ///< between tablet controls
constexpr float thumb_width = 76.0f;            ///< the thumb column
constexpr float clear_height = 52.0f;           ///< CLEAR
constexpr float add_height = 52.0f;             ///< ADD
constexpr float queue_height = 62.0f;           ///< QUEUE
constexpr float times_five_height = 44.0f;      ///< x5
constexpr float force_height = 44.0f;           ///< FORCE, at the column's top with a pad
constexpr float group_bar_inset = 12.0f;        ///< the group bar from the thumb column
constexpr float tablet_chip_width = 70.0f;      ///< a stored group's chip
constexpr float tablet_chip_height = 52.0f;     ///< the group bar
constexpr float store_width = 70.0f;            ///< STORE
constexpr float select_width = 100.0f;          ///< SELECT ▾
constexpr float rail_width = 60.0f;             ///< the right rail
constexpr float rail_button_height = 52.0f;     ///< a right rail button
constexpr float min_rail_button_height = 44.0f; ///< rail buttons shrink to this on short windows
constexpr float menu_strip_min_width = 60.0f;   ///< the top bar's blank strip that takes MENU
constexpr float menu_strip_min_height = 36.0f;  ///< and its height below the safe top
constexpr float menu_max_width = 80.0f;         ///< MENU in the strip
constexpr float menu_strip_inset = 4.0f;        ///< MENU from the top bar's right end
constexpr float menu_corner_height = 44.0f;     ///< MENU on the battlefield's corner

// Phone sizes, in points.
constexpr float minimap_side = 104.0f;           ///< the minimap and the left column's width
constexpr float build_height = 44.0f;            ///< BUILD
constexpr float phone_cell = 50.0f;              ///< QUEUE, ADD, CLEAR, SELECT
constexpr float zoom_height = 44.0f;             ///< zoom − and +
constexpr float phone_gap = 4.0f;                ///< between phone controls in a group
constexpr float phone_group_gap = 8.0f;          ///< between groups of the left column
constexpr float phone_button = 44.0f;            ///< PAUSE and MENU
constexpr float rail_slot_width = 64.0f;         ///< an order rail slot
constexpr float rail_slot_height = 44.0f;        ///< an order rail slot
constexpr float strip_inset = 12.0f;             ///< the strip, pill and chips from the column
constexpr float strip_height = 28.0f;            ///< the resource strip
constexpr float pill_gap = 4.0f;                 ///< between the strip and the status pill
constexpr float pill_height = 28.0f;             ///< the status pill
constexpr float pill_max_width = 420.0f;         ///< the status pill and the phone's banner
constexpr float phone_chip_width = 52.0f;        ///< a stored group's chip and +
constexpr float phone_chip_height = 44.0f;       ///< a stored group's chip and +
constexpr float drawer_width = 276.0f;           ///< 3 cells of 84, 2 gaps of 4, 16 of padding
constexpr float drawer_cell = 84.0f;             ///< one build tile
constexpr int drawer_columns = 3;                ///< cells across
constexpr float drawer_header = 44.0f;           ///< ✕, the title, PREV and NEXT
constexpr float drawer_tab_width = 84.0f;        ///< BUILD and ORDERS
constexpr float drawer_tab_height = 36.0f;       ///< the tab row: BUILD, ORDERS, x5, page dots
constexpr float drawer_times_five_width = 44.0f; ///< the drawer's x5
constexpr float more_width = 420.0f;             ///< the MORE sheet
constexpr float more_toggle_width = 200.0f;      ///< an order-page toggle cell, 2 across
constexpr float more_toggle_height = 40.0f;      ///< an order-page toggle cell
constexpr int more_toggle_columns = 2;           ///< toggle cells across
constexpr float more_button_width = 132.0f;      ///< an order button cell, 3 across
constexpr float more_button_height = 44.0f;      ///< an order button cell
constexpr int more_button_columns = 3;           ///< button cells across
constexpr float more_last_row = 48.0f;           ///< INFO and SELF-DESTRUCT · HOLD
constexpr float more_info_width = 132.0f;        ///< INFO
constexpr float more_section_gap = 8.0f;         ///< between the toggles, buttons and last row

// Pad HUD sizes, in points.
constexpr float pad_pill_height = 28.0f;     ///< the pad's status pill
constexpr float pad_pill_max_width = 420.0f; ///< the pad's status pill
constexpr float pad_row_gap = 4.0f;          ///< between the pill and the chips under it
constexpr float pad_chip_width = 88.0f;      ///< QUEUE, ADD and FORCE with their buttons
constexpr float pad_chip_height = 30.0f;     ///< QUEUE, ADD and FORCE with their buttons
constexpr float pad_chip_gap = 6.0f;         ///< between QUEUE, ADD and FORCE
constexpr float pad_group_width = 52.0f;     ///< a stored group's chip
constexpr float pad_group_height = 40.0f;    ///< a stored group's chip
constexpr float pad_group_gap = 4.0f;        ///< between group chips
constexpr float pad_inset = 12.0f;           ///< the group chips from the battlefield's sides

// Group ring sizes, in points.
constexpr float group_ring_inner = 28.0f; ///< the ring's hub
constexpr float group_ring_outer = 84.0f; ///< the ring's outer edge
constexpr float group_wedge_side = 32.0f; ///< a wedge's rectangle, round its number

/// How the left-handed layout moves a control.
enum class Layer : uint8_t {
    base,      ///< mirrored on its own
    fixed,     ///< stays: MENU in the 3.1c top bar's strip
    banner,    ///< moves with the banner, keeping its place in it
    placement, ///< moves with the placement bar, keeping its place in it
    sheet,     ///< moves with the open sheet, keeping its place in it
    radial,    ///< stays: the radial is laid out around the held point
};

/// Returns whether controls of `upper` cover those of `lower` (a sheet over the battlefield's
/// controls, the radial over everything).
///
/// @param upper the layer that may cover
/// @param lower the layer that may be covered
/// @return whether upper lies above lower
bool covers_layer(Layer upper, Layer lower) noexcept {
    const auto rank = [](Layer layer) {
        switch (layer) {
        case Layer::base:
        case Layer::fixed:
            return 0;
        case Layer::banner:
            return 1;
        case Layer::placement:
            return 2;
        case Layer::sheet:
            return 3;
        case Layer::radial:
            return 4;
        }
        return 0;
    };
    return rank(upper) > rank(lower);
}

/// The frame being laid out, with each control's layer.
struct Builder {
    Viewport viewport{};                      ///< the canvas
    float px_per_point{1.0f};                 ///< canvas pixels per point, > 0
    Frame frame{};                            ///< what is laid out so far
    std::array<Layer, max_controls> layers{}; ///< by control, as frame.controls

    /// Returns points as whole canvas pixels.
    ///
    /// @param points the length in points
    /// @return canvas pixels, rounded
    [[nodiscard]] int px(float points) const noexcept {
        return static_cast<int>(std::lround(points * px_per_point));
    }

    /// Returns the offset that centres a length in a span, rounded down to a whole point so
    /// every position scales with px_per_point exactly.
    ///
    /// @param span canvas pixels
    /// @param length canvas pixels
    /// @return canvas pixels from the span's start
    [[nodiscard]] int centred(int span, int length) const noexcept {
        const float free_points = static_cast<float>(span - length) / px_per_point;
        return px(std::floor(free_points / 2.0f));
    }

    /// Appends a control; past max_controls it is dropped.
    ///
    /// @param control the control
    /// @param index as ControlRect::index
    /// @param rect canvas pixels
    /// @param layer how the left-handed layout moves it
    void add(Control control, uint8_t index, Rect rect, Layer layer) noexcept {
        if (frame.control_count >= max_controls)
            return;
        frame.controls[frame.control_count] = {control, index, rect};
        layers[frame.control_count] = layer;
        ++frame.control_count;
    }

    /// Returns whether a rectangle overlaps any control laid out so far.
    ///
    /// @param rect canvas pixels
    /// @return whether it overlaps one
    [[nodiscard]] bool overlaps_any(const Rect& rect) const noexcept {
        for (uint8_t index = 0; index < frame.control_count; ++index) {
            if (rects::intersects(frame.controls[index].rect, rect))
                return true;
        }
        return false;
    }

    /// Returns the rectangle of the first control of a kind, or an empty one.
    ///
    /// @param control the control
    /// @return its rectangle
    [[nodiscard]] Rect rect_of(Control control) const noexcept {
        for (uint8_t index = 0; index < frame.control_count; ++index) {
            if (frame.controls[index].control == control)
                return frame.controls[index].rect;
        }
        return {};
    }
};

/// Returns the bottom edge of a rectangle.
///
/// @param rect the rectangle
/// @return y + height
int bottom_of(const Rect& rect) noexcept {
    return rect.y + rect.height;
}

/// Returns the right edge of a rectangle.
///
/// @param rect the rectangle
/// @return x + width
int right_of(const Rect& rect) noexcept {
    return rect.x + rect.width;
}

/// Returns a rectangle from its edges, empty when they cross.
///
/// @param left canvas pixels
/// @param top canvas pixels
/// @param right canvas pixels
/// @param bottom canvas pixels
/// @return the rectangle
Rect from_edges(int left, int top, int right, int bottom) noexcept {
    return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

/// Returns a value kept between two limits, or the lower limit when they cross.
///
/// @param value the value
/// @param low the lowest value kept
/// @param high the highest value kept
/// @return the value clamped
int clamp_low_first(int value, int low, int high) noexcept {
    return std::max(low, std::min(value, high));
}

/// Mirrors a rectangle left to right about a span.
///
/// @param rect canvas pixels
/// @param left the span's left edge
/// @param right the span's right edge
/// @return the mirrored rectangle
Rect mirror_in(const Rect& rect, int left, int right) noexcept {
    if (rects::empty(rect))
        return rect;
    Rect result = rect;
    result.x = left + right - rect.x - rect.width;
    return result;
}

/// The phone layout's fixed geometry, which the frame and the radial's area share.
struct PhoneGeometry {
    int column_x{};     ///< the left column's left edge
    int column_right{}; ///< the left column's right edge
    int top{};          ///< the safe top
    int bottom{};       ///< the safe bottom
    int left{};         ///< the safe left
    int right{};        ///< the safe right
    Rect pause{};       ///< PAUSE
    Rect menu{};        ///< MENU
    int rail_x{};       ///< the rail's left edge
    int rail_top{};     ///< the first slot's top
    int strip_bottom{}; ///< the resource strip's bottom
    int pill_bottom{};  ///< the status pill's bottom (the banner's when shown)
    int chips_y{};      ///< the group chips' top
};

/// Returns the phone layout's fixed geometry, right-handed.
///
/// @param builder the units
/// @param banner_shown whether the banner replaces the status pill
/// @return the geometry
PhoneGeometry phone_geometry(const Builder& builder, bool banner_shown) noexcept {
    const Rect safe = rects::safe_area(builder.viewport);
    PhoneGeometry geometry{};
    geometry.left = safe.x;
    geometry.top = safe.y;
    geometry.right = right_of(safe);
    geometry.bottom = bottom_of(safe);
    geometry.column_x = safe.x + builder.px(edge_points);
    geometry.column_right = geometry.column_x + builder.px(minimap_side);
    geometry.menu = {
        geometry.right - builder.px(edge_points) - builder.px(phone_button),
        safe.y + builder.px(edge_points),
        builder.px(phone_button),
        builder.px(phone_button)
    };
    geometry.pause = {
        geometry.menu.x - builder.px(phone_gap) - builder.px(phone_button),
        geometry.menu.y,
        builder.px(phone_button),
        builder.px(phone_button)
    };
    geometry.rail_x = geometry.right - builder.px(edge_points) - builder.px(rail_slot_width);
    geometry.rail_top = bottom_of(geometry.pause) + builder.px(edge_points);
    geometry.strip_bottom = safe.y + builder.px(edge_points) + builder.px(strip_height);
    geometry.pill_bottom = geometry.strip_bottom + builder.px(pill_gap) +
                           builder.px(banner_shown ? banner_height : pill_height);
    geometry.chips_y = geometry.bottom - builder.px(edge_points) - builder.px(phone_chip_height);
    return geometry;
}

/// Returns a builder for a viewport, with its units.
///
/// @param viewport the canvas
/// @return the builder, its frame empty
Builder make_builder(const Viewport& viewport) noexcept {
    Builder builder{};
    builder.viewport = viewport;
    builder.px_per_point = viewport.px_per_point > 0.0f ? viewport.px_per_point : 1.0f;
    return builder;
}

/// Returns one column's width. A non-positive width is the standard column.
///
/// @param builder the units
/// @param column_width the item width, canvas pixels; not positive for the standard column
/// @return the item width, canvas pixels
int menu_item_width(const Builder& builder, int column_width) noexcept {
    if (column_width > 0)
        return column_width;
    return builder.px(menu_column_width);
}

/// Returns the size of a menu panel.
///
/// @param builder the units
/// @param items how many items it lists
/// @param columns how many columns they fill
/// @param column_width one item's width, canvas pixels; not positive for the standard column
/// @return the panel's size (x and y 0)
Rect menu_panel_size(
    const Builder& builder, int items, int columns, int column_width = -1
) noexcept {
    const int rows = (items + columns - 1) / columns;
    const int item_width = menu_item_width(builder, column_width);
    const int width = 2 * builder.px(sheet_padding) + columns * item_width +
                      (columns - 1) * builder.px(menu_row_gap);
    const int height = 2 * builder.px(sheet_padding) + rows * builder.px(menu_row_height) +
                       std::max(0, rows - 1) * builder.px(menu_row_gap);
    return {0, 0, width, height};
}

/// Returns the fewest columns, from two, whose menu panel fits a height.
///
/// @param builder the units
/// @param items how many items it lists
/// @param height_available canvas pixels
/// @return the columns to use
int menu_columns_for(const Builder& builder, int items, int height_available) noexcept {
    for (int columns = min_menu_columns; columns <= max_menu_columns; ++columns) {
        if (menu_panel_size(builder, items, columns).height <= height_available)
            return columns;
    }
    return max_menu_columns;
}

/// Lays out a menu sheet's items in its panel, filling each column top to bottom.
///
/// @param[in,out] builder receives the items as menu_item controls
/// @param panel the sheet's panel, canvas pixels
/// @param items how many items it lists
/// @param columns how many columns they fill
/// @param column_width one item's width, canvas pixels; not positive for the standard column
void add_menu_items(
    Builder& builder, const Rect& panel, int items, int columns, int column_width = -1
) noexcept {
    const int rows = (items + columns - 1) / columns;
    const int item_width = menu_item_width(builder, column_width);
    for (int item = 0; item < items; ++item) {
        const int column = item / rows;
        const int row = item % rows;
        const Rect rect{
            panel.x + builder.px(sheet_padding) + column * (item_width + builder.px(menu_row_gap)),
            panel.y + builder.px(sheet_padding) +
                row * (builder.px(menu_row_height) + builder.px(menu_row_gap)),
            item_width,
            builder.px(menu_row_height)
        };
        builder.add(Control::menu_item, static_cast<uint8_t>(item), rect, Layer::sheet);
    }
}

/// Returns how many items a menu sheet lists.
///
/// @param sheet the open sheet
/// @return its items; 0 for a sheet that is not a menu
int menu_item_count(Sheet sheet) noexcept {
    switch (sheet) {
    case Sheet::select_menu:
        return static_cast<int>(select_item_count);
    case Sheet::speed:
        return 2;
    case Sheet::phone_menu:
        return 5;
    case Sheet::none:
    case Sheet::more:
    case Sheet::drawer:
        return 0;
    }
    return 0;
}

/// Returns the fewest columns, from one, whose menu panel fits a height.
///
/// @param builder the units
/// @param items how many items it lists
/// @param height_available canvas pixels
/// @return the columns to use, from 1 to max_menu_columns
int menu_columns_fitting(const Builder& builder, int items, int height_available) noexcept {
    for (int columns = 1; columns <= max_menu_columns; ++columns) {
        if (menu_panel_size(builder, items, columns).height <= height_available)
            return columns;
    }
    return max_menu_columns;
}

/// Lays out a menu sheet left of the rail. Speed stays one column. The phone
/// menu uses as many columns as it needs, from two, when one column is taller
/// than the room, the same rule as SELECT ▾. When that sheet would cover
/// `clear_of`, it moves into the span from there to the rail: the fewest
/// columns whose height fits, and items no wider than one column, so it stays
/// to the right of that edge. Pass 0 to leave the sheet where the rail puts it.
///
/// @param[in,out] builder receives the panel and its items
/// @param sheet the open sheet (speed or phone_menu)
/// @param rail_x the rail's left edge
/// @param top the panel's top, before clamping
/// @param low the highest top kept
/// @param high_bottom the lowest bottom kept
/// @param left_limit the leftmost edge kept, so the items stay in the safe area
/// @param clear_of the sheet starts at or to the right of this edge; 0 skips the reflow
void add_rail_menu(
    Builder& builder,
    Sheet sheet,
    int rail_x,
    int top,
    int low,
    int high_bottom,
    int left_limit,
    int clear_of = 0
) noexcept {
    const int items = menu_item_count(sheet);
    const int room = high_bottom - low;
    int columns = 1;
    if (sheet == Sheet::phone_menu && menu_panel_size(builder, items, 1).height > room)
        columns = menu_columns_for(builder, items, room);
    int item_width = -1;
    Rect panel = menu_panel_size(builder, items, columns);
    panel.x = rail_x - builder.px(edge_points) - panel.width;
    if (panel.x < left_limit)
        panel.x = left_limit;
    const int edge = builder.px(edge_points);
    const int span_left = clear_of + edge;
    const int span_right = rail_x - edge;
    if (clear_of > 0 && panel.x < clear_of && span_right > span_left) {
        columns = menu_columns_fitting(builder, items, room);
        const int gap = builder.px(menu_row_gap);
        const int gaps = std::max(0, columns - 1) * gap;
        const int inner = span_right - span_left - 2 * builder.px(sheet_padding) - gaps;
        const int full = builder.px(menu_column_width);
        const int floor_width = builder.px(menu_row_height);
        int width = columns > 0 ? inner / columns : full;
        if (width > full)
            width = full;
        if (width < floor_width)
            width = floor_width;
        item_width = width;
        panel = menu_panel_size(builder, items, columns, item_width);
        panel.x = span_right - panel.width;
        if (panel.x < span_left)
            panel.x = span_left;
    }
    panel.y = clamp_low_first(top, low, high_bottom - panel.height);
    builder.frame.sheet = panel;
    add_menu_items(builder, panel, items, columns, item_width);
}

/// Lays out the banner and its ✕.
///
/// @param[in,out] builder receives the banner and banner_cancel
/// @param banner the banner, canvas pixels
void add_banner(Builder& builder, const Rect& banner) noexcept {
    builder.frame.banner = banner;
    const Rect cancel{
        right_of(banner) - builder.px(banner_cancel_width),
        banner.y,
        builder.px(banner_cancel_width),
        banner.height
    };
    builder.add(Control::banner_cancel, 0, cancel, Layer::banner);
}

/// Lays out the placement bar (✕ CANCEL; a hold or a double tap places the building) centred
/// in a span on a bottom line. When
/// it would cover a control it moves right of what it covers, if that fits, else above it,
/// and tries again from there.
///
/// @param[in,out] builder receives the bar and its control
/// @param span_left the span's left edge
/// @param span_right the span's right edge
/// @param beside_limit the right edge the bar may reach when it moves right
/// @param bottom_line the bar's bottom
void add_placement(
    Builder& builder, int span_left, int span_right, int beside_limit, int bottom_line
) noexcept {
    const int width = builder.px(place_cancel_width);
    const int height = builder.px(place_height);
    const int centred_x = span_left + builder.centred(span_right - span_left, width);
    Rect bar{centred_x, bottom_line - height, width, height};
    for (int move = 0; move < max_placement_moves && builder.overlaps_any(bar); ++move) {
        int covered_right = bar.x;
        int covered_top = bar.y;
        for (uint8_t index = 0; index < builder.frame.control_count; ++index) {
            const Rect& rect = builder.frame.controls[index].rect;
            if (rects::intersects(rect, bar)) {
                covered_right = std::max(covered_right, right_of(rect));
                covered_top = std::min(covered_top, rect.y);
            }
        }
        Rect beside = bar;
        beside.x = covered_right + builder.px(place_gap);
        if (right_of(beside) <= beside_limit && !builder.overlaps_any(beside)) {
            bar = beside;
            break;
        }
        bar.x = centred_x;
        bar.y = covered_top - builder.px(edge_points) - height;
    }
    builder.frame.placement_bar = bar;
    builder.add(
        Control::place_cancel,
        0,
        {bar.x, bar.y, builder.px(place_cancel_width), height},
        Layer::placement
    );
}

/// Returns the group chips that fit a row and their width.
///
/// @param builder the units
/// @param state the stored groups
/// @param row_left where the chips start
/// @param row_right where the row ends
/// @param chip_width the chips' width before shrinking, canvas pixels
/// @param gap between chips, canvas pixels
/// @param after_chips the width of what follows the chips (STORE, SELECT), gaps included
/// @param[out] width the chips' width
/// @return how many of the stored groups get a chip, lowest numbers first
int fitting_chips(
    const Builder& builder,
    const HudState& state,
    int row_left,
    int row_right,
    int chip_width,
    int gap,
    int after_chips,
    int& width
) noexcept {
    int stored = 0;
    for (std::size_t group = 1; group < state.group_counts.size(); ++group) {
        if (state.group_counts[group] != 0)
            ++stored;
    }
    width = chip_width;
    if (stored == 0)
        return 0;
    const int room = row_right - row_left - after_chips;
    if (stored * (chip_width + gap) > room)
        width = std::max(builder.px(min_chip_width), room / stored - gap);
    return std::clamp(room / (width + gap), 0, stored);
}

/// Lays out the stored groups' chips in a row.
///
/// @param[in,out] builder receives the chips
/// @param state the stored groups
/// @param x the first chip's left edge
/// @param y the chips' top
/// @param width a chip's width
/// @param height a chip's height
/// @param gap between chips
/// @param shown how many chips fit
/// @return the left edge after the last chip and its gap
int add_chips(
    Builder& builder, const HudState& state, int x, int y, int width, int height, int gap, int shown
) noexcept {
    int placed = 0;
    for (std::size_t group = 1; group < state.group_counts.size() && placed < shown; ++group) {
        if (state.group_counts[group] == 0)
            continue;
        builder.add(
            Control::group_chip, static_cast<uint8_t>(group), {x, y, width, height}, Layer::base
        );
        x += width + gap;
        ++placed;
    }
    return x;
}

/// Lays out the tip bubble near its anchor, inside the safe area.
///
/// @param[in,out] builder receives Frame::tip
/// @param state the tip
void add_tip(Builder& builder, const HudState& state) noexcept {
    if (state.tip.until_ms == 0)
        return;
    const Rect safe = rects::safe_area(builder.viewport);
    const int width = std::min(builder.px(tip_width), safe.width - 2 * builder.px(edge_points));
    const int height = builder.px(tip_height);
    int y = state.tip.anchor.y - builder.px(tip_offset) - height;
    if (y < safe.y + builder.px(edge_points))
        y = state.tip.anchor.y + builder.px(tip_offset);
    const int x = state.tip.anchor.x - width / 2;
    builder.frame.tip = {
        clamp_low_first(
            x, safe.x + builder.px(edge_points), right_of(safe) - builder.px(edge_points) - width
        ),
        clamp_low_first(
            y, safe.y + builder.px(edge_points), bottom_of(safe) - builder.px(edge_points) - height
        ),
        width,
        height
    };
}

/// Lays out the open radial's wedges and hub.
///
/// @param[in,out] builder receives radial_item and radial_hub controls
/// @param state the open radial
void add_radial(Builder& builder, const HudState& state) noexcept {
    if (!state.radial.has_value())
        return;
    const Radial& radial = *state.radial;
    for (const RadialWedge& wedge : radial.wedges)
        builder.add(
            Control::radial_item, static_cast<uint8_t>(wedge.item), wedge.hit, Layer::radial
        );
    const Rect hub{
        radial.centre.x - radial.inner_radius,
        radial.centre.y - radial.inner_radius,
        2 * radial.inner_radius,
        2 * radial.inner_radius
    };
    builder.add(Control::radial_hub, 0, hub, Layer::radial);
}

/// Lays out the tablet: the 3.1c HUD at the chrome, with the touch controls beside it.
///
/// @param[in,out] builder receives the frame
/// @param state what the controls show
/// @param[out] span_left the left edge the left-handed layout mirrors about
/// @param[out] span_right the right edge the left-handed layout mirrors about
void lay_out_tablet(
    Builder& builder, const HudState& state, int& span_left, int& span_right
) noexcept {
    const Viewport& viewport = builder.viewport;
    const MatchLayout& chrome = viewport.chrome;
    const Rect safe = rects::safe_area(viewport);
    const int field_left = std::max(chrome.left, safe.x);
    const int field_top = std::max(chrome.top, safe.y);
    const int field_right = right_of(safe);
    const int bottom_line =
        std::min(chrome.bottom_bar_y(), bottom_of(safe)) - builder.px(edge_points);
    span_left = field_left;
    span_right = field_right;

    // Thumb column, bottom up: CLEAR, ADD, QUEUE, and x5 while a build page shows.
    const int column_x = field_left + builder.px(tablet_inset);
    const int column_width = builder.px(thumb_width);
    const Rect clear{
        column_x, bottom_line - builder.px(clear_height), column_width, builder.px(clear_height)
    };
    const Rect add{
        column_x,
        clear.y - builder.px(tablet_gap) - builder.px(add_height),
        column_width,
        builder.px(add_height)
    };
    const Rect queue{
        column_x,
        add.y - builder.px(tablet_gap) - builder.px(queue_height),
        column_width,
        builder.px(queue_height)
    };
    int column_top = queue.y;
    if (state.build_page_loaded) {
        const Rect times_five{
            column_x,
            queue.y - builder.px(tablet_gap) - builder.px(times_five_height),
            column_width,
            builder.px(times_five_height)
        };
        builder.add(Control::times_five, 0, times_five, Layer::base);
        column_top = times_five.y;
    }
    // FORCE tops the column while the pad in use has grips, when it fits under the banner's row.
    if (state.pad.force_shown) {
        const Rect force{
            column_x,
            column_top - builder.px(tablet_gap) - builder.px(force_height),
            column_width,
            builder.px(force_height)
        };
        const int highest = field_top + 2 * builder.px(edge_points) + builder.px(banner_height);
        if (force.y >= highest) {
            builder.add(Control::force, 0, force, Layer::base);
            column_top = force.y;
        }
    }
    builder.add(Control::queue, 0, queue, Layer::base);
    builder.add(Control::add, 0, add, Layer::base);
    builder.add(Control::clear, 0, clear, Layer::base);

    // MENU in the top bar's blank strip, else on the battlefield's top-right corner.
    const int rail_x = field_right - builder.px(edge_points) - builder.px(rail_width);
    const int rail_right = rail_x + builder.px(rail_width);
    const bool menu_in_strip = field_right - chrome.hud_width >= builder.px(menu_strip_min_width) &&
                               chrome.top - safe.y >= builder.px(menu_strip_min_height);
    Rect menu{};
    int rail_top = field_top + builder.px(edge_points);
    if (menu_in_strip) {
        const int menu_x = std::max(
            chrome.hud_width + builder.px(menu_strip_inset), rail_right - builder.px(menu_max_width)
        );
        menu = {menu_x, safe.y, rail_right - menu_x, chrome.top - safe.y};
        builder.add(Control::menu, 0, menu, Layer::fixed);
    } else {
        menu = {
            rail_x,
            field_top + builder.px(edge_points),
            builder.px(rail_width),
            builder.px(menu_corner_height)
        };
        builder.add(Control::menu, 0, menu, Layer::base);
        rail_top = bottom_of(menu) + builder.px(tablet_gap);
    }

    // Right rail: PAUSE, SPEED (not for a watcher), CHAT (shared games), CENTRE, FOLLOW, NEXT,
    // INFO. On a short window the buttons shrink to 44 pt to end above the bottom line; a
    // button that still does not fit is left out.
    const int rail_buttons = 5 + (state.watching ? 0 : 1) + (state.shared_game ? 1 : 0);
    const int rail_gap = builder.px(tablet_gap);
    const int rail_room = bottom_line - rail_top - (rail_buttons - 1) * rail_gap;
    const int rail_height = std::max(
        builder.px(min_rail_button_height),
        std::min(builder.px(rail_button_height), rail_room / rail_buttons)
    );
    int rail_y = rail_top;
    const auto add_rail = [&](Control control) {
        if (rail_y + rail_height <= bottom_line)
            builder.add(
                control, 0, {rail_x, rail_y, builder.px(rail_width), rail_height}, Layer::base
            );
        rail_y += rail_height + rail_gap;
    };
    add_rail(Control::pause);
    if (!state.watching)
        add_rail(Control::speed);
    if (state.shared_game)
        add_rail(Control::chat);
    add_rail(Control::centre);
    add_rail(Control::follow);
    add_rail(Control::next_unit);
    add_rail(Control::info);

    // Group bar on the column's bottom line: stored chips, STORE, SELECT ▾.
    const int bar_y = bottom_line - builder.px(tablet_chip_height);
    const int bar_x = right_of(clear) + builder.px(group_bar_inset);
    const int gap = builder.px(tablet_gap);
    const int after_chips = builder.px(store_width) + gap + builder.px(select_width);
    int chip_width = 0;
    const int shown = fitting_chips(
        builder,
        state,
        bar_x,
        rail_x - builder.px(edge_points),
        builder.px(tablet_chip_width),
        gap,
        after_chips,
        chip_width
    );
    int x = add_chips(
        builder, state, bar_x, bar_y, chip_width, builder.px(tablet_chip_height), gap, shown
    );
    builder.add(
        Control::group_store,
        0,
        {x, bar_y, builder.px(store_width), builder.px(tablet_chip_height)},
        Layer::base
    );
    x += builder.px(store_width) + gap;
    const Rect select{x, bar_y, builder.px(select_width), builder.px(tablet_chip_height)};
    builder.add(Control::select_menu, 0, select, Layer::base);

    // Banner, centred on the battlefield under the top bar.
    Rect banner{};
    if (state.banner.shown) {
        const int width = builder.px(banner_width);
        banner = {
            field_left + builder.centred(field_right - field_left, width),
            field_top + builder.px(edge_points),
            width,
            builder.px(banner_height)
        };
        // On a narrow battlefield it moves left of the rail (and MENU on the corner), and
        // narrows when it must.
        if (builder.overlaps_any(banner)) {
            const int right = rail_x - builder.px(edge_points);
            banner.x = std::max(field_left + builder.px(edge_points), right - width);
            banner.width = std::min(width, right - banner.x);
        }
        add_banner(builder, banner);
    }
    if (state.placement.active)
        add_placement(
            builder, field_left, field_right, rail_x - builder.px(edge_points), bottom_line
        );

    // Clear area: the battlefield left of the rail, above the column and the group bar.
    int clear_top = menu_in_strip ? field_top : bottom_of(menu) + builder.px(edge_points);
    if (state.banner.shown)
        clear_top = std::max(clear_top, bottom_of(banner) + builder.px(edge_points));
    int clear_bottom = std::min(column_top, bar_y) - builder.px(edge_points);
    if (state.placement.active)
        clear_bottom =
            std::min(clear_bottom, builder.frame.placement_bar.y - builder.px(edge_points));
    builder.frame.clear =
        from_edges(field_left, clear_top, rail_x - builder.px(edge_points), clear_bottom);

    // Sheets.
    switch (state.sheet) {
    case Sheet::select_menu: {
        const int items = menu_item_count(Sheet::select_menu);
        const int top_limit = field_top + builder.px(edge_points);
        const int columns =
            menu_columns_for(builder, items, select.y - builder.px(edge_points) - top_limit);
        Rect panel = menu_panel_size(builder, items, columns);
        panel.x = clamp_low_first(
            select.x,
            field_left + builder.px(edge_points),
            rail_x - builder.px(edge_points) - panel.width
        );
        panel.y = std::max(top_limit, select.y - builder.px(edge_points) - panel.height);
        builder.frame.sheet = panel;
        add_menu_items(builder, panel, items, columns);
        break;
    }
    case Sheet::speed: {
        Rect anchor = builder.rect_of(Control::speed);
        if (rects::empty(anchor))
            anchor = builder.rect_of(Control::pause);
        const int height = menu_panel_size(builder, menu_item_count(Sheet::speed), 1).height;
        add_rail_menu(
            builder,
            Sheet::speed,
            rail_x,
            anchor.y + anchor.height / 2 - height / 2,
            field_top + builder.px(edge_points),
            bottom_line,
            field_left
        );
        break;
    }
    case Sheet::phone_menu:
        add_rail_menu(
            builder,
            Sheet::phone_menu,
            rail_x,
            menu_in_strip ? field_top + builder.px(edge_points)
                          : bottom_of(menu) + builder.px(edge_points),
            field_top + builder.px(edge_points),
            bottom_line,
            field_left
        );
        break;
    case Sheet::none:
    case Sheet::more:
    case Sheet::drawer:
        // The drawer and MORE are the phone's: the tablet keeps the 3.1c panel's pages.
        break;
    }
}

/// Lays out the phone's build drawer over the left side.
///
/// @param[in,out] builder receives the drawer's panel, controls and cells
/// @param state the drawer's pages and cells wanted
/// @param geometry the phone's geometry
void add_drawer(Builder& builder, const HudState& state, const PhoneGeometry& geometry) noexcept {
    const Rect panel{
        geometry.left, geometry.top, builder.px(drawer_width), geometry.bottom - geometry.top
    };
    builder.frame.sheet = panel;
    const int x0 = panel.x + builder.px(sheet_padding);
    const int y0 = panel.y + builder.px(sheet_padding);
    const int button = builder.px(drawer_header);
    builder.add(Control::drawer_close, 0, {x0, y0, button, button}, Layer::sheet);
    if (state.drawer_pages > 1) {
        const Rect next{right_of(panel) - builder.px(sheet_padding) - button, y0, button, button};
        builder.add(
            Control::drawer_prev,
            0,
            {next.x - builder.px(phone_gap) - button, y0, button, button},
            Layer::sheet
        );
        builder.add(Control::drawer_next, 0, next, Layer::sheet);
    }
    const int tab_y = y0 + button + builder.px(phone_gap);
    const int tab_height = builder.px(drawer_tab_height);
    const int step = builder.px(drawer_tab_width) + builder.px(phone_gap);
    builder.add(
        Control::drawer_build_tab,
        0,
        {x0, tab_y, builder.px(drawer_tab_width), tab_height},
        Layer::sheet
    );
    builder.add(
        Control::drawer_orders_tab,
        0,
        {x0 + step, tab_y, builder.px(drawer_tab_width), tab_height},
        Layer::sheet
    );
    builder.add(
        Control::times_five,
        0,
        {x0 + 2 * step, tab_y, builder.px(drawer_times_five_width), tab_height},
        Layer::sheet
    );

    const int cell = builder.px(drawer_cell);
    const int cell_step = cell + builder.px(phone_gap);
    const int grid_y = tab_y + tab_height + builder.px(sheet_padding);
    const int grid_bottom = geometry.bottom - builder.px(sheet_padding);
    builder.frame.drawer_grid = from_edges(
        x0, grid_y, x0 + drawer_columns * cell_step - builder.px(phone_gap), grid_bottom
    );
    const int rows = std::max(0, (grid_bottom - grid_y + builder.px(phone_gap)) / cell_step);
    const int wanted = std::min<int>(state.drawer_cells_wanted, static_cast<int>(max_drawer_cells));
    const int cells = std::min(wanted, rows * drawer_columns);
    for (int index = 0; index < cells; ++index) {
        builder.frame.drawer_cells[static_cast<std::size_t>(index)] = {
            x0 + (index % drawer_columns) * cell_step,
            grid_y + (index / drawer_columns) * cell_step,
            cell,
            cell
        };
    }
    builder.frame.drawer_cell_count = static_cast<uint8_t>(cells);
}

/// Lays out the phone's MORE sheet left of the rail: the order-page cells, then INFO and
/// SELF-DESTRUCT · HOLD.
///
/// @param[in,out] builder receives the sheet's panel, cells and items
/// @param state the cells wanted
/// @param geometry the phone's geometry
void add_more(Builder& builder, const HudState& state, const PhoneGeometry& geometry) noexcept {
    const int padding = builder.px(sheet_padding);
    const int gap = builder.px(phone_gap);
    const int section = builder.px(more_section_gap);
    const int room_top = geometry.pill_bottom + builder.px(edge_points);
    const int room_bottom = geometry.bottom - builder.px(edge_points);
    const int max_height = std::min(
        room_bottom - room_top, geometry.bottom - geometry.top - 2 * builder.px(edge_points)
    );
    int toggles = std::min<int>(state.more_toggle_count, static_cast<int>(max_more_cells));
    int buttons =
        std::min<int>(state.more_button_count, static_cast<int>(max_more_cells) - toggles);
    const auto block_height = [&](int count, int columns, float cell_height) {
        const int rows = (count + columns - 1) / columns;
        return rows == 0 ? 0 : rows * builder.px(cell_height) + (rows - 1) * gap;
    };
    const auto sheet_height = [&](int toggle_count, int button_count) {
        const int toggle_block =
            block_height(toggle_count, more_toggle_columns, more_toggle_height);
        const int button_block =
            block_height(button_count, more_button_columns, more_button_height);
        int height = 2 * padding + toggle_block + button_block + builder.px(more_last_row);
        if (toggle_block > 0 && button_block > 0)
            height += section;
        if (toggle_block + button_block > 0)
            height += section;
        return height;
    };
    // Cells that do not fit the height are left out from the end.
    while (sheet_height(toggles, buttons) > max_height && toggles + buttons > 0) {
        if (buttons > 0)
            --buttons;
        else
            --toggles;
    }
    const int width = builder.px(more_width);
    const int height = sheet_height(toggles, buttons);
    const Rect panel{
        geometry.rail_x - builder.px(edge_points) - width, room_bottom - height, width, height
    };
    builder.frame.sheet = panel;
    const int x0 = panel.x + padding;
    int y = panel.y + padding;
    std::size_t cell = 0;
    for (int index = 0; index < toggles; ++index, ++cell) {
        builder.frame.more_cells[cell] = {
            x0 + (index % more_toggle_columns) * (builder.px(more_toggle_width) + gap),
            y + (index / more_toggle_columns) * (builder.px(more_toggle_height) + gap),
            builder.px(more_toggle_width),
            builder.px(more_toggle_height)
        };
    }
    const int toggle_block = block_height(toggles, more_toggle_columns, more_toggle_height);
    if (toggle_block > 0)
        y += toggle_block + (buttons > 0 ? section : 0);
    for (int index = 0; index < buttons; ++index, ++cell) {
        builder.frame.more_cells[cell] = {
            x0 + (index % more_button_columns) * (builder.px(more_button_width) + gap),
            y + (index / more_button_columns) * (builder.px(more_button_height) + gap),
            builder.px(more_button_width),
            builder.px(more_button_height)
        };
    }
    const int button_block = block_height(buttons, more_button_columns, more_button_height);
    y += button_block;
    builder.frame.more_cell_count = static_cast<uint8_t>(cell);
    builder.frame.more_grid = from_edges(x0, panel.y + padding, right_of(panel) - padding, y);
    if (toggle_block + button_block > 0)
        y += section;
    const int last_row = builder.px(more_last_row);
    const Rect info{x0, y, builder.px(more_info_width), last_row};
    builder.add(Control::more_item, static_cast<uint8_t>(MoreItem::info), info, Layer::sheet);
    builder.add(
        Control::more_item,
        static_cast<uint8_t>(MoreItem::self_destruct),
        from_edges(right_of(info) + gap, y, right_of(panel) - padding, y + last_row),
        Layer::sheet
    );
}

/// Lays out the phone: a full-bleed battlefield with the compact controls inside the safe area.
///
/// @param[in,out] builder receives the frame
/// @param state what the controls show
/// @param[out] span_left the left edge the left-handed layout mirrors about
/// @param[out] span_right the right edge the left-handed layout mirrors about
void lay_out_phone(
    Builder& builder, const HudState& state, int& span_left, int& span_right
) noexcept {
    const PhoneGeometry geometry = phone_geometry(builder, state.banner.shown);
    const PhoneGeometry closed = phone_geometry(builder, false);
    span_left = geometry.left;
    span_right = geometry.right;
    const bool drawer_open = state.sheet == Sheet::drawer;
    const int x = geometry.column_x;
    const int wide = builder.px(minimap_side);
    const int cell = builder.px(phone_cell);
    const int second = x + cell + builder.px(phone_gap);

    // Left column: minimap, BUILD, QUEUE ADD, CLEAR SELECT, zoom − +.
    const Rect minimap{x, geometry.top + builder.px(edge_points), wide, wide};
    const Rect build{
        x, bottom_of(minimap) + builder.px(phone_group_gap), wide, builder.px(build_height)
    };
    const int latch_y = bottom_of(build) + builder.px(phone_group_gap);
    const int clear_y = latch_y + cell + builder.px(phone_gap);
    const int zoom_y = clear_y + cell + builder.px(phone_group_gap);
    builder.frame.minimap = drawer_open ? Rect{} : minimap;
    builder.add(Control::build_drawer, 0, build, Layer::base);
    builder.add(Control::queue, 0, {x, latch_y, cell, cell}, Layer::base);
    builder.add(Control::add, 0, {second, latch_y, cell, cell}, Layer::base);
    builder.add(Control::clear, 0, {x, clear_y, cell, cell}, Layer::base);
    const Rect select{second, clear_y, cell, cell};
    builder.add(Control::select_menu, 0, select, Layer::base);
    builder.add(Control::zoom_out, 0, {x, zoom_y, cell, builder.px(zoom_height)}, Layer::base);
    builder.add(Control::zoom_in, 0, {second, zoom_y, cell, builder.px(zoom_height)}, Layer::base);

    // PAUSE and MENU, then the rail under them with MORE last.
    builder.add(Control::pause, 0, geometry.pause, Layer::base);
    builder.add(Control::menu, 0, geometry.menu, Layer::base);
    const int slots = std::min<int>(state.rail_count, rail_capacity(builder.viewport));
    for (int slot = 0; slot < slots; ++slot) {
        const Rect rect{
            geometry.rail_x,
            geometry.rail_top + slot * (builder.px(rail_slot_height) + builder.px(phone_gap)),
            builder.px(rail_slot_width),
            builder.px(rail_slot_height)
        };
        builder.add(
            slot + 1 == slots ? Control::more : Control::order_slot,
            static_cast<uint8_t>(slot),
            rect,
            Layer::base
        );
    }

    // Resource strip, status pill (or banner) and chips start right of the column, or right of
    // the drawer while it is open.
    const Rect drawer{
        geometry.left, geometry.top, builder.px(drawer_width), geometry.bottom - geometry.top
    };
    const int row_left =
        (drawer_open ? right_of(drawer) : geometry.column_right) + builder.px(strip_inset);
    const Rect strip = from_edges(
        row_left,
        geometry.top + builder.px(edge_points),
        geometry.pause.x - builder.px(strip_inset),
        geometry.top + builder.px(edge_points) + builder.px(strip_height)
    );
    builder.frame.resources = strip;
    const int pill_width = std::min(builder.px(pill_max_width), strip.width);
    const Rect pill{
        strip.x + builder.centred(strip.width, pill_width),
        bottom_of(strip) + builder.px(pill_gap),
        pill_width,
        builder.px(pill_height)
    };
    if (state.banner.shown)
        add_banner(builder, {pill.x, pill.y, pill_width, builder.px(banner_height)});
    else
        builder.frame.status = pill;

    const int chip_gap = builder.px(phone_gap);
    int chip_width = 0;
    const int shown = fitting_chips(
        builder,
        state,
        row_left,
        geometry.rail_x - builder.px(strip_inset),
        builder.px(phone_chip_width),
        chip_gap,
        builder.px(phone_chip_width),
        chip_width
    );
    const int store_x = add_chips(
        builder,
        state,
        row_left,
        geometry.chips_y,
        chip_width,
        builder.px(phone_chip_height),
        chip_gap,
        shown
    );
    builder.add(
        Control::group_store,
        0,
        {store_x, geometry.chips_y, builder.px(phone_chip_width), builder.px(phone_chip_height)},
        Layer::base
    );

    const int middle_left = closed.column_right + builder.px(strip_inset);
    const int middle_right = geometry.rail_x - builder.px(strip_inset);
    if (state.placement.active)
        add_placement(
            builder,
            middle_left,
            middle_right,
            middle_right,
            geometry.bottom - builder.px(edge_points)
        );

    // Clear area: between the column and the rail, under the pill, above the chips. Open
    // sheets do not shrink it.
    int clear_bottom = closed.chips_y - builder.px(edge_points);
    if (state.placement.active)
        clear_bottom =
            std::min(clear_bottom, builder.frame.placement_bar.y - builder.px(edge_points));
    builder.frame.clear = from_edges(
        middle_left, geometry.pill_bottom + builder.px(edge_points), middle_right, clear_bottom
    );

    // A 3.1c panel (the in-game menu, unit info) is fitted into the safe area less the edge.
    builder.frame.panel_sheet = from_edges(
        geometry.left + builder.px(edge_points),
        geometry.top + builder.px(edge_points),
        geometry.right - builder.px(edge_points),
        geometry.bottom - builder.px(edge_points)
    );

    const int room_top = geometry.pill_bottom + builder.px(edge_points);
    const int room_bottom = geometry.bottom - builder.px(edge_points);
    switch (state.sheet) {
    case Sheet::drawer:
        add_drawer(builder, state, geometry);
        break;
    case Sheet::more:
        add_more(builder, state, geometry);
        break;
    case Sheet::select_menu: {
        const int items = menu_item_count(Sheet::select_menu);
        const int columns = menu_columns_for(builder, items, room_bottom - room_top);
        Rect panel = menu_panel_size(builder, items, columns);
        panel.x = geometry.column_right + builder.px(edge_points);
        panel.y = clamp_low_first(
            select.y + select.height / 2 - panel.height / 2, room_top, room_bottom - panel.height
        );
        builder.frame.sheet = panel;
        add_menu_items(builder, panel, items, columns);
        break;
    }
    case Sheet::speed:
        add_rail_menu(
            builder,
            Sheet::speed,
            geometry.rail_x,
            geometry.rail_top,
            geometry.rail_top,
            room_bottom,
            geometry.left
        );
        break;
    case Sheet::phone_menu:
        add_rail_menu(
            builder,
            Sheet::phone_menu,
            geometry.rail_x,
            geometry.rail_top,
            geometry.rail_top,
            room_bottom,
            geometry.left,
            geometry.column_right
        );
        break;
    case Sheet::none:
        break;
    }
}

/// Lays out a menu sheet centred on a span: SELECT ▾ in as many columns as its height needs,
/// the speed and phone menus in one.
///
/// @param[in,out] builder receives the panel and its items
/// @param sheet the open sheet
/// @param left the span's left edge
/// @param top the span's top
/// @param right the span's right edge
/// @param bottom the span's bottom
void add_centred_menu(
    Builder& builder, Sheet sheet, int left, int top, int right, int bottom
) noexcept {
    const int items = menu_item_count(sheet);
    if (items == 0)
        return;
    const int columns =
        sheet == Sheet::select_menu ? menu_columns_for(builder, items, bottom - top) : 1;
    Rect panel = menu_panel_size(builder, items, columns);
    panel.x = left + builder.centred(right - left, panel.width);
    panel.y = clamp_low_first(
        top + builder.centred(bottom - top, panel.height), top, bottom - panel.height
    );
    builder.frame.sheet = panel;
    add_menu_items(builder, panel, items, columns);
}

/// Lays out the slim pad HUD over the 3.1c screen's battlefield: the status pill at its top, the
/// QUEUE, ADD and FORCE chips under it, the stored groups' chips near its bottom, and an open
/// menu sheet centred on it.
///
/// @param[in,out] builder receives the frame
/// @param state what the controls show
/// @param[out] span_left the left edge the left-handed layout mirrors about
/// @param[out] span_right the right edge the left-handed layout mirrors about
void lay_out_pad(
    Builder& builder, const HudState& state, int& span_left, int& span_right
) noexcept {
    const Viewport& viewport = builder.viewport;
    const MatchLayout& chrome = viewport.chrome;
    const Rect safe = rects::safe_area(viewport);
    const int field_left = std::max(chrome.left, safe.x);
    const int field_top = std::max(chrome.top, safe.y);
    const int field_right = right_of(safe);
    const int bottom_line =
        std::min(chrome.bottom_bar_y(), bottom_of(safe)) - builder.px(edge_points);
    span_left = field_left;
    span_right = field_right;
    const int field_width = field_right - field_left;

    // The status pill, centred at the battlefield's top.
    const int top = field_top + builder.px(edge_points);
    const int pill_width =
        std::min(builder.px(pad_pill_max_width), field_width - 2 * builder.px(edge_points));
    const Rect pill{
        field_left + builder.centred(field_width, pill_width),
        top,
        pill_width,
        builder.px(pad_pill_height)
    };
    builder.frame.status = pill;

    // QUEUE, ADD and (with grips) FORCE, centred under it.
    const int chips = state.pad.force_shown ? 3 : 2;
    const int chip_width = builder.px(pad_chip_width);
    const int chip_gap = builder.px(pad_chip_gap);
    const int row_width = chips * chip_width + (chips - 1) * chip_gap;
    const int chip_y = bottom_of(pill) + builder.px(pad_row_gap);
    int x = field_left + builder.centred(field_width, row_width);
    for (const Control control : {Control::queue, Control::add, Control::force}) {
        if (control == Control::force && !state.pad.force_shown)
            continue;
        builder.add(control, 0, {x, chip_y, chip_width, builder.px(pad_chip_height)}, Layer::base);
        x += chip_width + chip_gap;
    }
    const int band_bottom = chip_y + builder.px(pad_chip_height);

    // The stored groups' chips with their sizes, from the battlefield's left near its bottom.
    const int group_y = bottom_line - builder.px(pad_group_height);
    int group_width = 0;
    const int shown = fitting_chips(
        builder,
        state,
        field_left + builder.px(pad_inset),
        field_right - builder.px(pad_inset),
        builder.px(pad_group_width),
        builder.px(pad_group_gap),
        0,
        group_width
    );
    std::ignore = add_chips(
        builder,
        state,
        field_left + builder.px(pad_inset),
        group_y,
        group_width,
        builder.px(pad_group_height),
        builder.px(pad_group_gap),
        shown
    );

    // Clear area: the battlefield under the chips and above the group chips.
    const int clear_bottom = shown > 0 ? group_y - builder.px(edge_points) : bottom_line;
    builder.frame.clear =
        from_edges(field_left, band_bottom + builder.px(edge_points), field_right, clear_bottom);
    add_centred_menu(
        builder,
        state.sheet,
        field_left,
        band_bottom + builder.px(edge_points),
        field_right,
        bottom_line
    );
}

/// Lays out the open build ring's wedges.
///
/// @param[in,out] builder receives a build_wedge control for every wedge that holds something
/// @param state the open build ring
void add_build_ring(Builder& builder, const HudState& state) noexcept {
    if (!state.build_ring.has_value())
        return;
    for (std::size_t slot = 0; slot < build_ring_slot_count; ++slot) {
        const BuildWedge& wedge = state.build_ring->wedges[slot];
        if (wedge.kind != BuildWedgeKind::empty)
            builder.add(Control::build_wedge, static_cast<uint8_t>(slot), wedge.hit, Layer::radial);
    }
}

/// Lays out the group ring's nine wedges, a rectangle round each number midway across the ring.
///
/// @param[in,out] builder receives a group_wedge control for each group
/// @param state the group ring
void add_group_ring(Builder& builder, const HudState& state) noexcept {
    if (!state.pad.group_ring.has_value())
        return;
    const GroupRing& ring = *state.pad.group_ring;
    if (ring.outer_radius <= ring.inner_radius)
        return;
    constexpr double pi = 3.14159265358979323846;
    const double middle = static_cast<double>(ring.inner_radius + ring.outer_radius) / 2.0;
    const int side = builder.px(group_wedge_side);
    for (std::size_t slot = 0; slot < group_ring_slot_count; ++slot) {
        const double angle =
            2.0 * pi * static_cast<double>(slot) / static_cast<double>(group_ring_slot_count);
        const int x = ring.centre.x + static_cast<int>(std::lround(middle * std::sin(angle)));
        const int y = ring.centre.y - static_cast<int>(std::lround(middle * std::cos(angle)));
        builder.add(
            Control::group_wedge,
            static_cast<uint8_t>(slot + 1),
            {x - side / 2, y - side / 2, side, side},
            Layer::radial
        );
    }
}

/// Leaves out every control a higher layer covers (a sheet's panel, the radial's wedges and
/// hub): they are hidden under it and cannot be touched while it is open.
///
/// @param[in,out] builder the frame to filter
void leave_out_covered(Builder& builder) noexcept {
    Frame& frame = builder.frame;
    std::array<bool, max_controls> keep{};
    for (uint8_t index = 0; index < frame.control_count; ++index) {
        keep[index] = true;
        const Layer layer = builder.layers[index];
        const Rect& rect = frame.controls[index].rect;
        if (covers_layer(Layer::sheet, layer) && rects::intersects(frame.sheet, rect))
            keep[index] = false;
        for (uint8_t other = 0; other < frame.control_count && keep[index]; ++other) {
            const Layer upper = builder.layers[other];
            if (upper == Layer::radial && covers_layer(upper, layer) &&
                rects::intersects(frame.controls[other].rect, rect))
                keep[index] = false;
        }
    }
    // A control outside the safe area cannot be reached; leave it out too.
    const Rect safe = rects::safe_area(builder.viewport);
    uint8_t kept = 0;
    for (uint8_t index = 0; index < frame.control_count; ++index) {
        if (!keep[index] || !rects::inside(frame.controls[index].rect, safe))
            continue;
        frame.controls[kept] = frame.controls[index];
        builder.layers[kept] = builder.layers[index];
        ++kept;
    }
    for (uint8_t index = kept; index < frame.control_count; ++index)
        frame.controls[index] = {};
    frame.control_count = kept;
}

/// Mirrors the frame left to right about a span for the left-handed layout.
///
/// Each control of the battlefield's layout is mirrored on its own; a banner, the placement
/// bar and an open sheet move as a whole, keeping their insides in reading order; the radial
/// and the tip stay at the points they name.
///
/// @param[in,out] builder the frame to mirror
/// @param left the span's left edge
/// @param right the span's right edge
void mirror_frame(Builder& builder, int left, int right) noexcept {
    Frame& frame = builder.frame;
    const auto shift = [&](const Rect& whole) { return mirror_in(whole, left, right).x - whole.x; };
    const int banner_dx = shift(frame.banner);
    const int placement_dx = shift(frame.placement_bar);
    const int sheet_dx = shift(frame.sheet);
    for (uint8_t index = 0; index < frame.control_count; ++index) {
        Rect& rect = frame.controls[index].rect;
        switch (builder.layers[index]) {
        case Layer::base:
            rect = mirror_in(rect, left, right);
            break;
        case Layer::banner:
            rect.x += banner_dx;
            break;
        case Layer::placement:
            rect.x += placement_dx;
            break;
        case Layer::sheet:
            rect.x += sheet_dx;
            break;
        case Layer::fixed:
        case Layer::radial:
            break;
        }
    }
    for (Rect* region :
         {&frame.minimap,
          &frame.resources,
          &frame.status,
          &frame.banner,
          &frame.sheet,
          &frame.placement_bar,
          &frame.panel_sheet,
          &frame.clear})
        *region = mirror_in(*region, left, right);
    for (Rect* region : {&frame.drawer_grid, &frame.more_grid}) {
        if (!rects::empty(*region))
            region->x += sheet_dx;
    }
    for (uint8_t index = 0; index < frame.drawer_cell_count; ++index)
        frame.drawer_cells[index].x += sheet_dx;
    for (uint8_t index = 0; index < frame.more_cell_count; ++index)
        frame.more_cells[index].x += sheet_dx;
}

} // namespace

namespace rects {

Rect radial_area(const Viewport& viewport) noexcept {
    const Builder builder = make_builder(viewport);
    const Rect safe = safe_area(viewport);
    if (viewport.device == DeviceClass::tablet) {
        const MatchLayout& chrome = viewport.chrome;
        return from_edges(
            std::max(chrome.left, safe.x),
            std::max(chrome.top, safe.y),
            right_of(safe),
            std::min(chrome.bottom_bar_y(), bottom_of(safe))
        );
    }
    const PhoneGeometry geometry = phone_geometry(builder, false);
    const Rect area = from_edges(
        geometry.column_right + builder.px(strip_inset),
        geometry.strip_bottom + builder.px(edge_points),
        geometry.rail_x - builder.px(strip_inset),
        geometry.bottom
    );
    return viewport.left_handed ? mirror_in(area, geometry.left, geometry.right) : area;
}

} // namespace rects

uint8_t rail_capacity(const Viewport& viewport) noexcept {
    if (viewport.device != DeviceClass::phone)
        return 0;
    const Builder builder = make_builder(viewport);
    const PhoneGeometry geometry = phone_geometry(builder, false);
    const int room = geometry.bottom - builder.px(edge_points) - geometry.rail_top;
    const int step = builder.px(rail_slot_height) + builder.px(phone_gap);
    if (room < builder.px(rail_slot_height) || step <= 0)
        return 0;
    const int slots = (room + builder.px(phone_gap)) / step;
    return static_cast<uint8_t>(std::clamp(slots, 0, static_cast<int>(max_rail_slots) - 1));
}

Frame lay_out(const Viewport& viewport, const HudState& state) noexcept {
    Builder builder = make_builder(viewport);
    if (!state.in_match || viewport.width <= 0 || viewport.height <= 0)
        return builder.frame;
    int span_left = 0;
    int span_right = viewport.width;
    if (state.pad.hud)
        lay_out_pad(builder, state, span_left, span_right);
    else if (viewport.device == DeviceClass::phone)
        lay_out_phone(builder, state, span_left, span_right);
    else
        lay_out_tablet(builder, state, span_left, span_right);
    // The rings are laid out in canvas pixels as shown, so the leave-out runs on the mirrored
    // frame.
    add_radial(builder, state);
    add_build_ring(builder, state);
    add_group_ring(builder, state);
    if (viewport.left_handed)
        mirror_frame(builder, span_left, span_right);
    leave_out_covered(builder);
    add_tip(builder, state);
    return builder.frame;
}

GroupRing make_group_ring(const Viewport& viewport) noexcept {
    const Builder builder = make_builder(viewport);
    GroupRing ring{};
    ring.inner_radius = builder.px(group_ring_inner);
    ring.outer_radius = builder.px(group_ring_outer);
    const Rect safe = rects::safe_area(viewport);
    Point centre{};
    int mirror_left = 0;
    int mirror_right = 0;
    if (viewport.device == DeviceClass::phone) {
        // Right of the left column, above the group chips.
        const PhoneGeometry geometry = phone_geometry(builder, false);
        centre = {
            geometry.column_right + builder.px(strip_inset) + ring.outer_radius,
            geometry.chips_y - builder.px(edge_points) - ring.outer_radius
        };
        mirror_left = geometry.left;
        mirror_right = geometry.right;
    } else {
        // Beside the thumb column's place, above the group bar's row: clear of the touch
        // controls when they show, and of the pad HUD's group chips.
        const MatchLayout& chrome = viewport.chrome;
        const int field_left = std::max(chrome.left, safe.x);
        const int bottom_line =
            std::min(chrome.bottom_bar_y(), bottom_of(safe)) - builder.px(edge_points);
        centre = {
            field_left + builder.px(tablet_inset + thumb_width + group_bar_inset) +
                ring.outer_radius,
            bottom_line - builder.px(tablet_chip_height) - builder.px(edge_points) -
                ring.outer_radius
        };
        mirror_left = field_left;
        mirror_right = right_of(safe);
    }
    if (viewport.left_handed)
        centre.x = mirror_left + mirror_right - centre.x;
    // The ring stays inside the battlefield's part of the safe area.
    Rect area = rects::radial_area(viewport);
    if (area.width < 2 * ring.outer_radius || area.height < 2 * ring.outer_radius)
        area = safe;
    const auto keep = [](int value, int low, int high) {
        return low > high ? (low + high) / 2 : std::clamp(value, low, high);
    };
    ring.centre = {
        keep(centre.x, area.x + ring.outer_radius, right_of(area) - ring.outer_radius),
        keep(centre.y, area.y + ring.outer_radius, bottom_of(area) - ring.outer_radius)
    };
    return ring;
}

Rect mirrored(const Rect& rect, const Viewport& viewport) noexcept {
    Rect result = rect;
    result.x = viewport.width - rect.x - rect.width;
    return result;
}

} // namespace oa::ui::touch_hud
