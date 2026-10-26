// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Match chrome, side HUD, resource readout, fog and minimap.
#include "oa/app/runtime.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/ui/hud/game_clock.hpp"
#include "oa/ui/hud/kill_board.hpp"
#include "oa/ui/hud/status_panel.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/visibility_state.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/hud/build_page_fit.hpp"
#include "oa/ui/hud/health_bar.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/hud/resource_bar.hpp"
#include "oa/ui/hud/unit_panel.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;

/// MatchOverlay::draw_progress's whole, in thousandths.
constexpr int32_t kOverlayProgressWhole = 1000;
/// MatchOverlay::draw_progress's busy bar.
constexpr int32_t kOverlayProgressBusy = -1;
/// A busy bar's stripes: one every this many points, this many points wide,
/// the accent laid over the well at this share, in 256ths (two fifths), as
/// the Game files screen's busy bar.
constexpr int32_t kBusyStripeSpacing = 12;
constexpr int32_t kBusyStripeWidth = 5;
constexpr uint32_t kBusyStripeOpacity = 102;

/// Divides, rounding towards negative infinity.
///
/// @param value the dividend
/// @param divisor the divisor, positive
/// @return the quotient, rounded down
int divide_down(int value, int divisor) noexcept {
    const int quotient = value / divisor;
    return value % divisor != 0 && value < 0 ? quotient - 1 : quotient;
}

/// Divides, rounding towards positive infinity.
///
/// @param value the dividend
/// @param divisor the divisor, positive
/// @return the quotient, rounded up
int divide_up(int value, int divisor) noexcept {
    return -divide_down(-value, divisor);
}

/// Returns the OA UI kit's crisp canvas over a match overlay's painter: its
/// point (0, 0) at a pixel of the painter, `scale` pixels to a point, in the
/// game's fonts, clipped to the battlefield. The battlefield is taken in
/// whole points, rounded in, so nothing lands outside it; where no whole
/// point of it is left, the canvas draws nothing.
///
/// @param target the painter's surface
/// @param fonts the kit's fonts; null draws no text
/// @param at the painter pixel of the canvas's point (0, 0)
/// @param scale painter pixels to a point, at least 1
/// @param field the battlefield, in the painter's pixels
/// @return the canvas
kit::Canvas overlay_kit_canvas(
    renderer::Surface& target,
    const kit::Fonts* fonts,
    oa::ui::display_layout::Point at,
    int scale,
    const oa::ui::display_layout::Rect& field
) {
    kit::Canvas canvas;
    const int left = divide_up(field.x - at.x, scale);
    const int top = divide_up(field.y - at.y, scale);
    const int right = divide_down(field.x + field.width - at.x, scale);
    const int bottom = divide_down(field.y + field.height - at.y, scale);
    if (right <= left || bottom <= top)
        return canvas;
    canvas.surface = &target;
    canvas.placement.x = at.x;
    canvas.placement.y = at.y;
    canvas.placement.scale = scale;
    canvas.placement.clip = {left, top, right - left, bottom - top};
    canvas.fonts = fonts;
    return canvas;
}

/// Draws a busy progress bar: the kit's bar with nothing done, striped
/// across its well in the accent.
///
/// @param canvas where it is drawn
/// @param bar the bar, in points
void draw_busy_progress(const kit::Canvas& canvas, const kit::Rect& bar) {
    kit::draw_progress(canvas, bar, 0, 0);
    const kit::Rect well{bar.x + 1, bar.y + 1, bar.width - 2, bar.height - 2};
    if (well.width <= 0 || well.height <= 0)
        return;
    const kit::Canvas inside = kit::clipped(canvas, well);
    for (int32_t stripe = 0; stripe < well.width; stripe += kBusyStripeSpacing)
        kit::blend_rect(
            inside,
            {well.x + stripe, well.y, kBusyStripeWidth, well.height},
            kit::colour::accent,
            kBusyStripeOpacity
        );
}

/// Gives the rows a line's letters rise above its baseline.
///
/// @param layers the line
/// @return the rows from the first a letter covers down to the baseline; 0
///         for a line with no letter above its baseline
int letter_rows_above_baseline(const oa::present::TextLayers& layers) {
    const auto width = static_cast<std::size_t>(std::max(layers.width, 0));
    for (int32_t row = 0; row < layers.baseline && row < layers.height; ++row)
        for (std::size_t column = 0; column < width; ++column)
            if (const auto at = static_cast<std::size_t>(row) * width + column;
                at < layers.fill.size() && layers.fill[at] != 0)
                return layers.baseline - row;
    return 0;
}

} // namespace

int Runtime::builder_gui_page_count() const {
    if (!match_ || selected_match_unit_ == 0)
        return 0;
    const auto& world = match_->state();
    const auto* unit = oa::world_unit_at(&world, selected_match_unit_);
    if (unit == nullptr || unit->type_index == 0 || unit->type_index >= world.unit_def_count)
        return 0;
    // UnitDef.gui_page_count holds the first missing page index (from the
    // unit definitions), raised to the highest download MENU; pages are
    // 1..count-1.
    const auto counted = world.unit_defs[unit->type_index].gui_page_count;
    return counted > 1 ? static_cast<int>(counted) - 1 : (counted == 1 ? 1 : 0);
}

bool Runtime::is_build_page_nav(std::string_view name) const {
    const auto action = match_hud_action(name);
    return action == "PREV" || action == "NEXT" || action == "PREVIOUS";
}

bool Runtime::build_page_nav_shown() const {
    return builder_gui_page_count() > 1;
}

void Runtime::show_match_build_page(int page) {
    namespace hud = oa::ui::hud;
    // The page another unit showed is none to step from.
    const int shown = match_build_page_unit_ == selected_match_unit_ ? match_build_page_ : 0;
    // The build pages cycle as the game's page flags do; a type with one
    // page behaves as if it had a second, missing one.
    const auto page_count = static_cast<uint8_t>(std::max(2, builder_gui_page_count() + 1));
    const auto current = static_cast<uint32_t>(std::clamp(shown, 0, 7));
    auto flags = hud::kUnitFlagBuildMenu | (current << hud::kUnitBuildPageShift);
    if (page == shown + 1)
        flags = hud::build_menu_forward(flags, page_count, false);
    else if (page == shown - 1)
        flags = hud::build_menu_back(flags, page_count, false);
    else
        flags = hud::build_menu_select(flags, page_count, static_cast<uint32_t>(std::max(page, 0)));
    open_match_build_page(static_cast<int>(hud::build_page(flags)));
}

void Runtime::open_match_build_page(int page) {
    namespace hud = oa::ui::hud;
    auto* unit = match_ && selected_match_unit_ != 0
                     ? oa::world_unit_at(&match_->state(), selected_match_unit_)
                     : nullptr;
    const auto table = unit != nullptr ? order_panel_table() : hud::UnitTable{};
    const auto* def = unit != nullptr ? hud::unit_def(table, *unit) : nullptr;
    if (def == nullptr) {
        status_ = "Select a builder to open the build menu";
        return;
    }
    if (unit->build_remaining != 0.0F) {
        show_match_orders_page();
        return;
    }
    page = std::max(1, page);
    char name[64];
    hud::format_build_page_name(name, sizeof name, *def, static_cast<uint32_t>(page));
    auto& game = match_->state().game;
    auto state = hud::order_panel_load(game);
    summarize_order_panel(state);
    state.unit_id = 0;
    // A page that does not load leaves the panel as it was.
    const auto previous_page = std::exchange(match_build_page_, page);
    const auto previous_page_unit = std::exchange(match_build_page_unit_, unit->id);
    const auto prefix = match_side_name_prefix();
    hud::load_build_page(
        state, *unit, *def, name, page, prefix.c_str(), order_panel_controls(), order_panel_loader()
    );
    if (state.unit_id == unit->id) {
        hud::order_panel_store(game, state);
        status_ = "Build page " + std::to_string(page);
        render_match_surface();
    } else {
        match_build_page_ = previous_page;
        match_build_page_unit_ = previous_page_unit;
        show_unsupported("Build GUI " + std::string(name) + " is not available.");
    }
}

void Runtime::show_match_page_by_key(int page) {
    auto* unit = match_panel_unit();
    if (unit == nullptr || page < 0)
        return;
    const auto& world = match_->state();
    if (unit->type_index == 0 || unit->type_index >= world.unit_def_count)
        return;
    const auto page_count = world.unit_defs[unit->type_index].gui_page_count;
    if (page >= static_cast<int>(page_count))
        return;
    // The unit keeps the page picked, or the order page for 0.
    unit->flags =
        oa::ui::hud::build_menu_select(unit->flags, page_count, static_cast<uint32_t>(page));
    apply_match_hud_for_selection();
    play_match_interface_sound("nextbuildmenu");
}

oa::Unit* Runtime::match_panel_unit() {
    if (!match_ || selected_match_unit_ == 0)
        return nullptr;
    int selected = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit_index != 0 && slot.unit != nullptr &&
            slot.owner_index == match_local_player_ &&
            (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0)
            ++selected;
    return selected > 1 ? nullptr : oa::world_unit_at(&match_->state(), selected_match_unit_);
}

int Runtime::match_panel_page() {
    const auto* unit = match_panel_unit();
    if (unit == nullptr || (unit->flags & oa::ui::hud::kUnitFlagBuildMenu) == 0)
        return 0;
    const auto& world = match_->state();
    if (unit->type_index == 0 || unit->type_index >= world.unit_def_count ||
        world.unit_defs[unit->type_index].gui_page_count == 0)
        return 0;
    return static_cast<int>(oa::ui::hud::build_page(unit->flags));
}

void Runtime::press_match_panel_page(oa::ui::hud::BuildPanelClick press, bool cycle) {
    namespace hud = oa::ui::hud;
    auto* unit = match_panel_unit();
    if (unit == nullptr)
        return;
    const auto& world = match_->state();
    if (unit->type_index == 0 || unit->type_index >= world.unit_def_count)
        return;
    switch (press) {
    case hud::BuildPanelClick::orders:
        unit->flags &= ~hud::kUnitFlagBuildMenu;
        apply_match_hud_for_selection();
        return;
    case hud::BuildPanelClick::build:
        unit->flags |= hud::kUnitFlagBuildMenu;
        apply_match_hud_for_selection();
        return;
    case hud::BuildPanelClick::page_back:
    case hud::BuildPanelClick::page_forward:
        break;
    default:
        return;
    }
    const bool forward = press == hud::BuildPanelClick::page_forward;
    // The unit's page turns among its type's pages, and with `cycle` the
    // order page takes its turn between the last page and the first.
    const auto page_count = world.unit_defs[unit->type_index].gui_page_count;
    unit->flags = forward ? hud::build_menu_forward(unit->flags, page_count, cycle)
                          : hud::build_menu_back(unit->flags, page_count, cycle);
    const auto page = match_panel_page();
    if (page == 0)
        show_match_orders_page();
    else
        open_match_build_page(page);
}

void Runtime::load_match_chrome() {
    match_chrome_ = {};
    match_hud_.reset();
    try {
        const auto path = match_side_tile();
        match_chrome_ = oa::ui::decoded::require(oa::decode_pcx(assets_.read(path).bytes), path);
    } catch (const std::exception& error) {
        std::cerr << "match chrome tile unavailable: " << error.what() << '\n';
    }
    show_match_orders_page();
}

void Runtime::scale_blit(
    renderer::Surface& destination,
    const renderer::Surface& source,
    int dx,
    int dy,
    int dw,
    int dh,
    int sx,
    int sy,
    int sw,
    int sh
) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    if (dw == sw && dh == sh) {
        blit_rect(destination, source, dx, dy, sx, sy, dw, dh);
        return;
    }
    const int dest_w = static_cast<int>(destination.width);
    const int dest_h = static_cast<int>(destination.height);
    const int src_w = static_cast<int>(source.width);
    const int src_h = static_cast<int>(source.height);
    const int y0 = std::max(0, dy);
    const int y1 = std::min(dest_h, dy + dh);
    const int x0 = std::max(0, dx);
    const int x1 = std::min(dest_w, dx + dw);
    if (y0 >= y1 || x0 >= x1)
        return;
    const int x_count = x1 - x0;
    scale_src_x_.resize(static_cast<std::size_t>(x_count));
    for (int i = 0; i < x_count; ++i) {
        const int src_x = sx + (x0 - dx + i) * sw / dw;
        scale_src_x_[static_cast<std::size_t>(i)] = (src_x < 0 || src_x >= src_w) ? -1 : src_x;
    }
    for (int dest_y = y0; dest_y < y1; ++dest_y) {
        const int src_y = sy + (dest_y - dy) * sh / dh;
        if (src_y < 0 || src_y >= src_h)
            continue;
        auto* dest_row =
            destination.rgb.data() +
            (static_cast<std::size_t>(dest_y) * destination.width + static_cast<std::size_t>(x0)) *
                3U;
        const auto* src_row =
            source.rgb.data() + (static_cast<std::size_t>(src_y) * source.width) * 3U;
        for (int i = 0; i < x_count; ++i) {
            const int src_x = scale_src_x_[static_cast<std::size_t>(i)];
            if (src_x >= 0) {
                const auto si = static_cast<std::size_t>(src_x) * 3U;
                dest_row[0] = src_row[si];
                dest_row[1] = src_row[si + 1];
                dest_row[2] = src_row[si + 2];
            }
            dest_row += 3;
        }
    }
}

void Runtime::blit_rect(
    renderer::Surface& destination,
    const renderer::Surface& source,
    int destination_x,
    int destination_y,
    int source_x,
    int source_y,
    int width,
    int height
) {
    const int dest_w = static_cast<int>(destination.width);
    const int dest_h = static_cast<int>(destination.height);
    const int src_w = static_cast<int>(source.width);
    const int src_h = static_cast<int>(source.height);
    int x_off = 0, y_off = 0;
    if (destination_x < 0) {
        x_off = -destination_x;
        destination_x = 0;
    }
    if (destination_y < 0) {
        y_off = -destination_y;
        destination_y = 0;
    }
    source_x += x_off;
    source_y += y_off;
    width -= x_off;
    height -= y_off;
    if (source_x < 0) {
        destination_x -= source_x;
        width += source_x;
        source_x = 0;
    }
    if (source_y < 0) {
        destination_y -= source_y;
        height += source_y;
        source_y = 0;
    }
    width = std::min(width, dest_w - destination_x);
    width = std::min(width, src_w - source_x);
    height = std::min(height, dest_h - destination_y);
    height = std::min(height, src_h - source_y);
    if (width <= 0 || height <= 0)
        return;
    const auto bytes = static_cast<std::size_t>(width) * 3U;
    for (int y = 0; y < height; ++y) {
        auto* dest_row = destination.rgb.data() +
                         (static_cast<std::size_t>(destination_y + y) * destination.width +
                          static_cast<std::size_t>(destination_x)) *
                             3U;
        const auto* src_row =
            source.rgb.data() + (static_cast<std::size_t>(source_y + y) * source.width +
                                 static_cast<std::size_t>(source_x)) *
                                    3U;
        std::memcpy(dest_row, src_row, bytes);
    }
}

void Runtime::draw_match_label(
    int x, int y, std::string_view text, uint8_t palette_index, int scale
) {
    draw_match_text(match_label_font(), x, y, text, palette_index, scale);
}

void Runtime::draw_match_text(
    const oa::formats::fnt::Font* font,
    int x,
    int y,
    std::string_view text,
    uint8_t palette_index,
    int scale
) {
    if (font == nullptr)
        return;
    const auto& palette = match_hud_ && match_hud_->background.palette
                              ? *match_hud_->background.palette
                              : match_palette_;
    const auto pal = static_cast<std::size_t>(palette_index) * 4U;
    if (pal + 2 >= palette.size())
        return;
    paint_text(*font, x, y, text, {palette[pal], palette[pal + 1], palette[pal + 2]}, scale);
}

void Runtime::paint_text(
    const oa::formats::fnt::Font& font,
    int x,
    int y,
    std::string_view text,
    std::array<uint8_t, 3> color,
    int scale,
    bool allow_background
) {
    if (text.empty() || scale < 1)
        return;
    if (!renderer::needs_text_runs(text, true)) {
        paint_font_text(font, x, y, text, color, scale);
        return;
    }
    // The modern fonts sit on the font's baseline, drawn at the scale's size
    // and the size the text takes here.
    const int32_t font_baseline = renderer::fnt_font_baseline(font);
    const auto face = renderer::fnt_font_face(font);
    const auto runs = renderer::split_game_text(text, renderer::fnt_font_characters(font), true);
    std::vector<std::optional<oa::present::TextLayers>> laid(runs.size());
    for (std::size_t index = 0; index < runs.size(); ++index)
        if (runs[index].modern)
            laid[index] = oa::present::modern_text(
                runs[index].text, face, scale, painted_text_size(runs[index]), allow_background
            );
    const int drop = panel_text_drop(font, y, text, scale);
    int pen = x;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto& run = runs[index];
        if (const auto& layers = laid[index]) {
            const int baseline =
                y + drop + painted_baseline(font_baseline, painted_text_size(run)) * scale;
            pen += paint_modern_text(*layers, pen, baseline, color);
            continue;
        }
        // A run the modern fonts cannot draw shows in the font, as the code
        // page holds it.
        const std::string bytes =
            run.modern ? oa::present::encode_game_text(run.text, false) : run.text;
        paint_font_text(font, pen, y + drop, bytes, color, scale);
        pen += static_cast<int>(oa::formats::fnt::measure_text(font, bytes)) * scale;
    }
}

int Runtime::panel_text_drop(
    const oa::formats::fnt::Font& font, int y, std::string_view text, int scale
) {
    if (text_place_ != TextPlace::panel || !panel_top_row_ ||
        !renderer::needs_text_runs(text, true))
        return 0;
    // A line whose letters would rise above the row, as ideographs beside
    // the game's fonts may, is lowered until they do not.
    const int32_t font_baseline = renderer::fnt_font_baseline(font);
    const auto face = renderer::fnt_font_face(font);
    int drop = 0;
    for (const auto& run :
         renderer::split_game_text(text, renderer::fnt_font_characters(font), true)) {
        if (!run.modern)
            continue;
        const int32_t size = painted_text_size(run);
        if (const auto layers = oa::present::modern_text(run.text, face, scale, size, false))
            drop = std::max(
                drop,
                *panel_top_row_ - (y + painted_baseline(font_baseline, size) * scale -
                                   letter_rows_above_baseline(*layers))
            );
    }
    return drop;
}

int Runtime::match_text_width(
    const oa::formats::fnt::Font& font, std::string_view text, int scale
) const {
    scale = std::max(scale, 1);
    if (!renderer::needs_text_runs(text, true))
        return static_cast<int>(oa::formats::fnt::measure_text(font, text)) * scale;
    int width = 0;
    for (const auto& run :
         renderer::split_game_text(text, renderer::fnt_font_characters(font), true)) {
        if (run.modern)
            if (const auto layers = oa::present::modern_text(
                    run.text, renderer::fnt_font_face(font), scale, painted_text_size(run)
                )) {
                width += layers->advance;
                continue;
            }
        const std::string bytes =
            run.modern ? oa::present::encode_game_text(run.text, false) : run.text;
        width += static_cast<int>(oa::formats::fnt::measure_text(font, bytes)) * scale;
    }
    return width;
}

Runtime::TextMargins
Runtime::match_text_margins(const oa::formats::fnt::Font& font, char character) const {
    const std::string_view text(&character, 1);
    // Columns from the pen to the first painted one and to the last, in a
    // coverage of the given width and height.
    const auto margins = [](std::span<const uint8_t> painted,
                            int width,
                            int height,
                            int pen,
                            int advance) -> TextMargins {
        int first = width;
        int last = -1;
        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column)
                if (painted
                        [static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(column)] != 0) {
                    first = std::min(first, column);
                    last = std::max(last, column);
                }
        if (last < 0)
            return {};
        return {first - pen, pen + advance - (last + 1)};
    };
    if (renderer::needs_text_runs(text, true)) {
        const auto runs =
            renderer::split_game_text(text, renderer::fnt_font_characters(font), true);
        if (runs.size() == 1 && runs.front().modern) {
            const auto layers = oa::present::modern_text(
                runs.front().text, renderer::fnt_font_face(font), 1, painted_text_size(runs.front())
            );
            if (!layers || layers->width <= 0 || layers->height <= 0)
                return {};
            // Every layer paints: the letters, their outline and their shadow.
            std::vector<uint8_t> painted(layers->fill);
            for (const auto* layer : {&layers->outline, &layers->shadow})
                for (std::size_t index = 0; index < std::min(painted.size(), layer->size());
                     ++index)
                    painted[index] = static_cast<uint8_t>(painted[index] | (*layer)[index]);
            return margins(painted, layers->width, layers->height, layers->pen, layers->advance);
        }
    }
    const auto& glyph = font.glyphs[static_cast<unsigned char>(character)];
    if (!glyph || glyph->width == 0 ||
        glyph->coverage.size() != static_cast<std::size_t>(glyph->width) * glyph->height)
        return {};
    return margins(glyph->coverage, glyph->width, glyph->height, glyph->origin_x, glyph->width);
}

void Runtime::paint_font_text(
    const oa::formats::fnt::Font& font,
    int x,
    int y,
    std::string_view text,
    std::array<uint8_t, 3> color,
    int scale
) {
    if (text.empty() || scale < 1)
        return;
    auto& dest = paint_target();
    // The glyphs are drawn into a buffer with room above the pen row for
    // the font's lift, and two rows of margin around them.
    const int lift = oa::formats::fnt::row_lift(font);
    const int pen_row = 2 + std::max(0, lift);
    const auto text_w = static_cast<int>(oa::formats::fnt::measure_text(font, text)) + 4;
    const auto text_h = static_cast<int>(oa::formats::fnt::line_height(font)) + 4 + std::abs(lift);
    if (text_w <= 0 || text_h <= 0)
        return;
    thread_local std::vector<uint8_t> indices;
    thread_local std::vector<uint8_t> coverage;
    const auto pixel_count = static_cast<std::size_t>(text_w) * static_cast<std::size_t>(text_h);
    indices.assign(pixel_count, 0);
    coverage.assign(pixel_count, 0);
    const oa::formats::fnt::IndexedSurface target{
        static_cast<uint32_t>(text_w),
        static_cast<uint32_t>(text_h),
        static_cast<std::size_t>(text_w),
        indices,
        coverage
    };
    // The text is measured before it is drawn; where the pen stops is not
    // needed.
    std::ignore = oa::formats::fnt::raster_text(target, font, text, 0, pen_row);
    for (int row = 0; row < text_h * scale; ++row) {
        const int py = y + row - pen_row * scale;
        if (py < 0 || py >= static_cast<int>(dest.height))
            continue;
        auto* out = dest.rgb.data() + (static_cast<std::size_t>(py) * dest.width) * 3U;
        const auto* cover = coverage.data() + static_cast<std::size_t>(row / scale) * text_w;
        for (int column = 0; column < text_w * scale; ++column) {
            if (cover[column / scale] == 0)
                continue;
            const int px = x + column;
            if (px < 0 || px >= static_cast<int>(dest.width))
                continue;
            auto* pixel = out + static_cast<std::size_t>(px) * 3U;
            pixel[0] = color[0];
            pixel[1] = color[1];
            pixel[2] = color[2];
        }
    }
}

void Runtime::load_side_hud() {
    side_hud_ = {};
    try {
        // The SIDEDATA the side table was read from.
        const auto data = assets_.read(side_data_path()).bytes;
        const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
        oa::ui::hud::SideLayout layout;
        // The viewed player's own SIDEn section.
        if (!oa::ui::hud::parse_side_layout(text, static_cast<int32_t>(match_view_side()), layout))
            return;
        const auto rect = [](const oa::ui::hud::Rect& r) {
            return HudRect{r.x, r.y, r.width, r.height};
        };
        side_hud_.metal_color = layout.metal_color;
        side_hud_.energy_color = layout.energy_color;
        side_hud_.metal_bar = rect(layout.metal_bar);
        side_hud_.energy_bar = rect(layout.energy_bar);
        side_hud_.metal_num_x = layout.metal_num_x;
        side_hud_.metal_num_y = layout.metal_num_y;
        side_hud_.metal_max_x = layout.metal_max_x;
        side_hud_.metal_max_y = layout.metal_max_y;
        side_hud_.metal_zero_x = layout.metal_zero_x;
        side_hud_.metal_zero_y = layout.metal_zero_y;
        side_hud_.metal_produced_x = layout.metal_produced_x;
        side_hud_.metal_produced_y = layout.metal_produced_y;
        side_hud_.metal_consumed_x = layout.metal_consumed_x;
        side_hud_.metal_consumed_y = layout.metal_consumed_y;
        side_hud_.energy_num_x = layout.energy_num_x;
        side_hud_.energy_num_y = layout.energy_num_y;
        side_hud_.energy_max_x = layout.energy_max_x;
        side_hud_.energy_max_y = layout.energy_max_y;
        side_hud_.energy_zero_x = layout.energy_zero_x;
        side_hud_.energy_zero_y = layout.energy_zero_y;
        side_hud_.energy_produced_x = layout.energy_produced_x;
        side_hud_.energy_produced_y = layout.energy_produced_y;
        side_hud_.energy_consumed_x = layout.energy_consumed_x;
        side_hud_.energy_consumed_y = layout.energy_consumed_y;
        side_hud_.unit_name = rect(layout.unit_name);
        side_hud_.damage_bar = rect(layout.damage_bar);
        side_hud_.unit_metal_make = rect(layout.unit_metal_make);
        side_hud_.unit_metal_use = rect(layout.unit_metal_use);
        side_hud_.unit_energy_make = rect(layout.unit_energy_make);
        side_hud_.unit_energy_use = rect(layout.unit_energy_use);
        side_hud_.logo = rect(layout.logo);
        side_hud_.logo2 = rect(layout.logo2);
        side_hud_.mission_text = rect(layout.mission_text);
        side_hud_.unit_name2 = rect(layout.unit_name2);
        side_hud_.damage_bar2 = rect(layout.damage_bar2);
        side_hud_.name = rect(layout.name);
        side_hud_.description = rect(layout.description);
    } catch (const std::exception& error) {
        std::cerr << "sidedata HUD unavailable: " << error.what() << '\n';
    }
}

void Runtime::paint_on(PaintLayer layer) {
    if (layer == PaintLayer::hud) {
        overlay_target_ = &match_hud_cpu_;
        hud_source_space_ = true;
        paint_origin_ = {};
        return;
    }
    overlay_target_ = &match_world_cpu_;
    hud_source_space_ = false;
    paint_origin_ = {match_layout_.left, match_layout_.top};
}

renderer::Surface& Runtime::paint_target() {
    return overlay_target_ ? *overlay_target_ : surface_;
}

oa::ui::display_layout::Point Runtime::canvas_paint(int x, int y) const {
    return {x - paint_origin_.x, y - paint_origin_.y};
}

int Runtime::hud_text_scale() const {
    return std::max(1, static_cast<int>(std::lround(match_layout_.scale)));
}

const oa::formats::fnt::Font* Runtime::match_label_font() const {
    return match_small_font_ ? &*match_small_font_ : (match_hud_ ? &match_hud_->font : nullptr);
}

void Runtime::fill_hud_rect(int x, int y, int width, int height, uint8_t palette_index) {
    const auto pal = static_cast<std::size_t>(palette_index) * 4U;
    if (pal + 2 >= match_palette_.size() || width <= 0 || height <= 0)
        return;
    const std::array<uint8_t, 3> color{
        match_palette_[pal], match_palette_[pal + 1], match_palette_[pal + 2]
    };
    auto& dest = paint_target();
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < width; ++column) {
            const int px = x + column, py = y + row;
            if (px < 0 || py < 0 || px >= static_cast<int>(dest.width) ||
                py >= static_cast<int>(dest.height))
                continue;
            const auto di =
                (static_cast<std::size_t>(py) * dest.width + static_cast<std::size_t>(px)) * 3U;
            dest.rgb[di] = color[0];
            dest.rgb[di + 1] = color[1];
            dest.rgb[di + 2] = color[2];
        }
}

const oa::formats::fnt::Font* Runtime::overlay_font(OverlayFont font) {
    return font == OverlayFont::message_log ? &message_font() : match_label_font();
}

void Runtime::draw_extension_overlay() {
    if (extension_.draw_match_overlay == nullptr || !match_)
        return;
    // Its lines step by the game fonts' heights.
    const PanelText panel(*this);
    MatchOverlay overlay{};
    overlay.painter = this;
    overlay.game = &match_->state().game;
    // Over the battlefield, clear of the touch controls while they are on.
    const auto area = overlay_area();
    const auto corner = canvas_paint(area.x, area.y);
    overlay.left = corner.x;
    overlay.top = corner.y;
    overlay.bottom = canvas_paint(area.x, area.y + area.height).y;
    overlay.scale = hud_text_scale();
    // A font's height is the low byte of its header's first word, which the
    // game steps from one line to the next by; the conversion keeps that byte.
    overlay.font_height = [](void* painter, OverlayFont font) -> uint8_t {
        const auto* face = static_cast<Runtime*>(painter)->overlay_font(font);
        return face != nullptr ? static_cast<uint8_t>(face->nominal_height) : 0;
    };
    overlay.draw_text =
        [](void* painter, OverlayFont font, int x, int y, const char* text, uint8_t palette_index) {
            auto& runtime = *static_cast<Runtime*>(painter);
            runtime.draw_match_text(
                runtime.overlay_font(font),
                x,
                y,
                text != nullptr ? text : "",
                palette_index,
                runtime.hud_text_scale()
            );
        };
    overlay.fill_rect =
        [](void* painter, int x, int y, int width, int height, uint8_t palette_index) {
            static_cast<Runtime*>(painter)->fill_hud_rect(x, y, width, height, palette_index);
        };
    // The OA UI kit's components, crisp at the HUD's scale in the game fonts,
    // on the same painter, each clipped to the battlefield.
    static constexpr auto kit_canvas_at = [](Runtime& runtime, int x, int y) {
        const auto area = runtime.overlay_area();
        const auto corner = runtime.canvas_paint(area.x, area.y);
        return overlay_kit_canvas(
            runtime.paint_target(),
            runtime.engine_settings_fonts(),
            {x, y},
            runtime.hud_text_scale(),
            {corner.x, corner.y, area.width, area.height}
        );
    };
    overlay.kit_text_width = [](void* painter, kit::FontRole font, const char* text) -> int {
        auto& runtime = *static_cast<Runtime*>(painter);
        const kit::Fonts* fonts = runtime.engine_settings_fonts();
        if (fonts == nullptr || text == nullptr)
            return 0;
        return kit::text_width(*fonts, font, text) * runtime.hud_text_scale();
    };
    overlay.draw_kit_text = [](void* painter, kit::FontRole font, int x, int y, const char* text) {
        if (text == nullptr)
            return;
        const kit::Canvas canvas = kit_canvas_at(*static_cast<Runtime*>(painter), x, y);
        if (canvas.fonts == nullptr)
            return;
        const int32_t line = font == kit::FontRole::regular ? kit::compact_metrics.regular_line
                                                            : kit::compact_metrics.small_line;
        kit::box_text(
            canvas,
            font,
            text,
            {0, 0, kit::text_width(*canvas.fonts, font, text), line},
            kit::Align::left,
            kit::colour::text
        );
    };
    overlay.draw_chip = [](void* painter, int x, int y, const kit::ChipLook* chip) {
        if (chip == nullptr)
            return;
        const kit::Canvas canvas = kit_canvas_at(*static_cast<Runtime*>(painter), x, y);
        const int32_t width = canvas.fonts != nullptr ? kit::chip_width(*canvas.fonts, *chip)
                                                      : 2 * kit::compact_metrics.chip_padding;
        kit::draw_chip(canvas, {0, 0, width, kit::compact_metrics.chip_height}, *chip);
    };
    overlay.draw_progress =
        [](void* painter, int x, int y, int width, int height, int32_t thousandths) {
            auto& runtime = *static_cast<Runtime*>(painter);
            const int scale = runtime.hud_text_scale();
            const kit::Rect bar{0, 0, width / scale, height / scale};
            if (bar.width <= 0 || bar.height <= 0)
                return;
            const kit::Canvas canvas = kit_canvas_at(runtime, x, y);
            if (thousandths == kOverlayProgressBusy) {
                draw_busy_progress(canvas, bar);
                return;
            }
            kit::draw_progress(
                canvas,
                bar,
                std::clamp(thousandths, int32_t{0}, kOverlayProgressWhole),
                kOverlayProgressWhole
            );
        };
    overlay.draw_hover_card = [](void* painter, int x, int y, const kit::HoverCardLook* card) {
        if (card == nullptr)
            return;
        const kit::Canvas canvas = kit_canvas_at(*static_cast<Runtime*>(painter), x, y);
        if (canvas.fonts == nullptr)
            return;
        const kit::Point size = kit::hover_card_size(*canvas.fonts, *card);
        kit::draw_hover_card(canvas, {0, 0, size.x, size.y}, *card);
    };
    call_hook_or_report<&Extension::draw_match_overlay>(
        extension_, hook_error_report(), *this, overlay
    );
}

oa::ui::display_layout::Point Runtime::hud_canvas(int x, int y) const {
    if (hud_source_space_)
        return {x, y};
    const auto canvas = oa::ui::display_layout::source_to_canvas(match_layout_, x, y);
    return canvas_paint(canvas.x, canvas.y);
}

void Runtime::draw_hud_label(int x, int y, std::string_view text, uint8_t palette_index) {
    const auto point = hud_canvas(x, y);
    draw_match_label(point.x, point.y, text, palette_index);
}

void Runtime::draw_match_label_right(int x, int y, std::string_view text, uint8_t palette_index) {
    const oa::formats::fnt::Font* font = match_label_font();
    if (font == nullptr)
        return;
    // Measured in the side's font; a side that names none gives no width.
    const auto width = match_side_names_font_ ? match_text_width(*font, text, 1) : 0;
    const auto point = hud_canvas(x, y);
    draw_match_label(point.x - width, point.y, text, palette_index);
}

void Runtime::draw_hud_label_centered(int x, int y, std::string_view text, uint8_t palette_index) {
    const oa::formats::fnt::Font* font = match_label_font();
    if (font == nullptr)
        return;
    // Measured in the side's font; a side that names none gives no width.
    const auto width = match_side_names_font_ ? match_text_width(*font, text, 1) : 0;
    draw_hud_label(x - width / 2, y, text, palette_index);
}

void Runtime::fill_source_rect(int x, int y, int width, int height, uint8_t palette_index) {
    if (hud_source_space_) {
        fill_hud_rect(x, y, std::max(1, width), std::max(1, height), palette_index);
        return;
    }
    const auto rect = oa::ui::display_layout::source_rect_to_canvas(
        match_layout_, x, y, std::max(0, width), std::max(0, height)
    );
    const auto origin = canvas_paint(rect.x, rect.y);
    fill_hud_rect(
        origin.x, origin.y, std::max(1, rect.width), std::max(1, rect.height), palette_index
    );
}

void Runtime::draw_player_mark(const oa::Player& player, const HudRect& at) {
    if (at.width <= 0 || at.height <= 0)
        return;
    const auto logo = player_logo_frame(player);
    if (!logo || logo->width == 0 || logo->height == 0)
        return;
    const auto width = std::max(1, at.width);
    const auto height = std::max(1, at.height);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const auto source =
                static_cast<std::size_t>(row * static_cast<int>(logo->height) / height) *
                    logo->width +
                static_cast<std::size_t>(column * static_cast<int>(logo->width) / width);
            if (source < logo->coverage.size() && logo->coverage[source] != 0)
                fill_source_rect(at.x + column, at.y + row, 1, 1, logo->pixels[source]);
        }
    }
}

void Runtime::draw_sidedata_bar(
    const HudRect& bar, float shown, float capacity, float threshold, float store, uint8_t color
) {
    if (bar.width <= 0 || bar.height <= 0)
        return;
    const auto columns = oa::ui::hud::trough_columns(
        shown, capacity, threshold, store, bar.x, bar.x + bar.width - 1
    );
    if (columns.fill)
        fill_source_rect(bar.x, bar.y, columns.fill_right - bar.x + 1, bar.height, color);
    if (columns.marker)
        fill_source_rect(
            columns.marker_left,
            bar.y,
            3,
            bar.height,
            oa::ui::hud::readout_color(match_->state().game, oa::ui::hud::kReadoutConsumedColor)
        );
}

void Runtime::draw_resource_readout() {
    const auto viewer = match_view_player();
    if (viewer >= OA_PLAYER_COUNT)
        return;
    auto& game = match_->state().game;
    auto& player = game.players[viewer];
    auto& readout = game.resource_readout;
    // The readout eases as often a second at any frame rate (take_readout_eases).
    for (uint32_t ease = take_readout_eases(); ease != 0; --ease)
        oa::ui::hud::update_resource_readout(readout, player, game.tick);
    // PANELTOP already contains the METAL/ENERGY chrome; SIDEDATA.TDF names the
    // colour block, the fill troughs, the stored and capacity numbers, and
    // the produced and consumed readouts.
    draw_player_mark(player, side_hud_.logo);
    const auto text_color = oa::ui::hud::readout_color(game, oa::ui::hud::kReadoutTextColor);
    const auto whole = [](float value) { return std::to_string(static_cast<int>(value)); };
    const auto label = [this](int x, int y, const oa::ui::hud::RateText& rate) {
        draw_hud_label(x, y, rate.text, rate.color);
    };
    draw_sidedata_bar(
        side_hud_.energy_bar,
        readout.energy,
        player.energy_storage,
        player.energy_share_threshold,
        player.energy,
        side_hud_.energy_color
    );
    draw_hud_label(
        side_hud_.energy_num_x, side_hud_.energy_num_y, whole(readout.energy), text_color
    );
    draw_hud_label(side_hud_.energy_zero_x, side_hud_.energy_zero_y, "0", text_color);
    draw_match_label_right(
        side_hud_.energy_max_x, side_hud_.energy_max_y, whole(player.energy_storage), text_color
    );
    label(
        side_hud_.energy_produced_x,
        side_hud_.energy_produced_y,
        oa::ui::hud::format_energy_rate(game, readout.energy_produced, true)
    );
    label(
        side_hud_.energy_consumed_x,
        side_hud_.energy_consumed_y,
        oa::ui::hud::format_energy_rate(game, readout.energy_requested, false)
    );
    draw_sidedata_bar(
        side_hud_.metal_bar,
        readout.metal,
        player.metal_storage,
        player.metal_share_threshold,
        player.metal,
        side_hud_.metal_color
    );
    draw_hud_label(side_hud_.metal_num_x, side_hud_.metal_num_y, whole(readout.metal), text_color);
    draw_hud_label(side_hud_.metal_zero_x, side_hud_.metal_zero_y, "0", text_color);
    draw_match_label_right(
        side_hud_.metal_max_x, side_hud_.metal_max_y, whole(player.metal_storage), text_color
    );
    label(
        side_hud_.metal_produced_x,
        side_hud_.metal_produced_y,
        oa::ui::hud::format_metal_rate(game, readout.metal_produced, true)
    );
    label(
        side_hud_.metal_consumed_x,
        side_hud_.metal_consumed_y,
        oa::ui::hud::format_metal_rate(game, readout.metal_requested, false)
    );
}

void Runtime::draw_unit_rates(const oa::Unit& unit, int lowered) {
    const auto rate = [this, lowered](const HudRect& at, float amount, bool metal, bool produced) {
        const auto text =
            oa::ui::hud::format_unit_rate(match_->state().game, amount, metal, produced);
        const auto point = hud_canvas(at.x, at.y);
        draw_match_label(point.x, point.y + lowered, text.text, text.color);
    };
    rate(side_hud_.unit_energy_make, unit.economy.energy.last_produced, false, true);
    rate(side_hud_.unit_energy_use, unit.economy.energy.last_requested, false, false);
    rate(side_hud_.unit_metal_make, unit.economy.metal.last_produced, true, true);
    rate(side_hud_.unit_metal_use, unit.economy.metal.last_requested, true, false);
}

namespace {

namespace hud = oa::ui::hud;

// The unit panel's view of a unit's order queues: the primary queue's
// records, and the secondary queue's with the stockpile build's progress
// (its third word, which saved orders carry).
struct PanelQueues {
    oa::sim::match_runtime::Match* match{};
    std::vector<hud::OrderOverlay> primary{};
    std::vector<hud::OrderOverlay> secondary{};
};

// Bit of an order's flags that puts it on the secondary queue.
constexpr uint8_t kSecondaryQueueOrder = 0x04;
// Records of a primary queue read before the head: only the head is shown.
constexpr std::size_t kPanelQueueHead = 1;

/// Returns a unit's head order (primary) or its secondary queue as the panel
/// reads them, rebuilt into the PanelQueues `user` holds.
hud::OrderOverlay* panel_orders(void* user, const oa::Unit& unit, bool secondary) {
    auto& queues = *static_cast<PanelQueues*>(user);
    auto& world = queues.match->state();
    auto& nodes = secondary ? queues.secondary : queues.primary;
    nodes.clear();
    if (!secondary) {
        std::array<oa::sim::match_runtime::Match::OrderRecordView, kPanelQueueHead> records{};
        const auto count =
            queues.match->queue_records(unit.id, false, records.data(), records.size());
        for (std::size_t index = 0; index < count; ++index) {
            auto& node = nodes.emplace_back();
            node.mission = records[index].kind;
            node.unit = oa::world_unit_at(&world, unit.id);
            node.target = records[index].target != 0
                              ? oa::world_unit_at(&world, records[index].target)
                              : nullptr;
            node.parameter = static_cast<uint32_t>(records[index].parameter_1);
            node.state = records[index].flags;
        }
    } else {
        struct Walk {
            std::vector<hud::OrderOverlay>* nodes{};
            oa::World* world{};
            uint16_t unit{};
        } walk{&nodes, &world, unit.id};

        queues.match->visit_saved_orders(
            unit.id,
            [](void* context,
               const oa::data::persist::SavedOrder* order,
               const oa::data::persist::SavedGoal*) {
                auto& walk = *static_cast<Walk*>(context);
                if (order == nullptr || (order->flags & kSecondaryQueueOrder) == 0)
                    return;
                auto& node = walk.nodes->emplace_back();
                node.mission = order->kind;
                node.unit = oa::world_unit_at(walk.world, walk.unit);
                node.target = order->target_id != 0
                                  ? oa::world_unit_at(walk.world, order->target_id)
                                  : nullptr;
                node.parameter = static_cast<uint32_t>(order->parameter_1);
                node.progress = order->parameter_3;
                node.state = order->flags;
            },
            &walk
        );
    }
    for (std::size_t index = 0; index < nodes.size(); ++index)
        nodes[index].next = index + 1 < nodes.size() ? &nodes[index + 1] : nullptr;
    return nodes.empty() ? nullptr : nodes.data();
}

} // namespace

const char* Runtime::hovered_gadget_name() const {
    if (screen_ != Screen::match || !hovered_ || !match_hud_ ||
        *hovered_ >= match_hud_->layout.gadgets.size())
        return nullptr;
    return match_hud_->layout.gadgets[*hovered_].common.name.c_str();
}

void Runtime::draw_unit_panel() {
    if (!match_)
        return;
    auto& world = match_->state();
    const bool debug_keys =
        (world.game.outcome_flags & oa::ui::console::outcome_flag::debug_keys) != 0;
    constexpr uint8_t text_color = 255;
    const auto text_at = [this](const HudRect& at, std::string_view text) {
        draw_hud_label(at.x, at.y, text, text_color);
    };
    const auto meter = [this, &world](const HudRect& at, int32_t value, int32_t maximum) {
        if (at.width <= 0 || at.height <= 0)
            return;
        oa::present::world_renderer::overlay_meter_bar(
            source_overlay_raster(),
            nullptr,
            value,
            maximum,
            {at.x, at.y, at.x + at.width - 1, at.y + at.height - 1},
            world.game.ui_colors,
            0
        );
    };
    // A gadget under the pointer: a build button shows its unit's cost line
    // and description, any other gadget nothing.
    if (const char* button = hovered_gadget_name()) {
        char line[hud::kPanelLineBytes];
        std::string_view description;
        if (hud::build_button_readout(world, button, line, sizeof line, &description)) {
            text_at(side_hud_.name, line);
            if (!description.empty())
                text_at(side_hud_.description, description);
        }
        return;
    }
    // The panel's words in the player's language; each is copied or drawn
    // before the next is looked up.
    const hud::Localize localize = [](void* context, const char* text) -> const char* {
        auto& self = *static_cast<Runtime*>(context);
        self.unit_panel_word_ = self.translate_ui(text);
        return self.unit_panel_word_.c_str();
    };
    const auto cursor = world.game.cursor_unit_id;
    if (cursor == 0) {
        char line[hud::kPanelLineBytes];
        if (hud::feature_readout(world, debug_keys, localize, this, line, sizeof line))
            text_at(side_hud_.name, line);
        return;
    }
    PanelQueues queues{match_.get()};
    hud::OverlayContext overlay{};
    overlay.world = &world;
    overlay.missions = hud::kMissionOverlays;
    overlay.sink.user = &queues;
    overlay.sink.orders = &panel_orders;
    hud::UnitPanelHooks hooks{};
    hooks.context = this;
    hooks.can_see = [](void* context, const oa::Player& viewer, const oa::Unit& unit) {
        auto& self = *static_cast<Runtime*>(context);
        try {
            return self.match_->unit_shown(viewer.index, unit.id);
        } catch (const std::exception&) {
            return false;
        }
    };
    hooks.localize = localize;
    const auto& ui = ui_rules();
    hooks.allied_units_shown = ui.allied_unit_display.enabled;
    hooks.viewer_allies_every_player =
        match_->slotless_viewer() == oa::sim::match_runtime::SlotlessViewer::ally_of_every_player;
    // The profile's status words, where they differ from 3.1c's.
    const auto texts = view_rules::profile_texts(mod_profile());
    hooks.nanolathing_status = texts.nanolathing_status;
    hooks.paralyzed_status = texts.paralyzed_status;
    if (ui.veterancy_label.enabled)
        hooks.veterancy_level = [](void* context, const oa::Unit& unit) -> uint32_t {
            const auto& self = *static_cast<Runtime*>(context);
            const auto& own =
                self.match_->rules_view().unit_type(unit.type_index).veterancy_thresholds;
            if (own.has_value())
                return hud::veterancy_label_level(
                    {own->items.data(), own->count}, unit.veteran_level
                );
            // A type without its own thresholds takes the profile's default list.
            const auto& defaults = self.match_->rules().veterancy.model.default_thresholds;
            std::array<uint16_t, 32> thresholds{};
            const auto count = std::min<std::size_t>(defaults.count, thresholds.size());
            for (std::size_t index = 0; index < count; ++index)
                thresholds[index] = static_cast<uint16_t>(defaults.items[index]);
            return hud::veterancy_label_level({thresholds.data(), count}, unit.veteran_level);
        };
    const auto panel =
        hud::unit_panel_snapshot(world, cursor, debug_keys, match_session_kind(), overlay, hooks);
    if (panel.unit == 0)
        return;
    // The status line stands one row under the bottom bar's top: its text
    // keeps within the bar.
    const PanelText in_bar(*this, hud_canvas(0, oa::ui::display_layout::kSourceBottomBarY).y);
    draw_hud_label_centered(side_hud_.unit_name.x, side_hud_.unit_name.y, panel.name, text_color);
    if (panel.unidentified)
        return;
    const auto* unit = oa::world_unit_at(&world, panel.unit);
    const auto* def = unit != nullptr ? oa::world_unit_def_of(&world, unit) : nullptr;
    if (unit == nullptr || def == nullptr)
        return;
    if (panel.show_damage)
        meter(side_hud_.damage_bar, unit->health, static_cast<int32_t>(def->max_damage));
    if (panel.logo_player < OA_PLAYER_COUNT)
        draw_player_mark(world.game.players[panel.logo_player], side_hud_.logo2);
    // The status line, lowered to keep within the bar, takes the rate
    // figures under it down as far and one outline further, so that its
    // outline and shadow fall clear of their letters.
    int lowered = 0;
    if (const auto* font = match_label_font(); font != nullptr && panel.mission_text[0] != '\0')
        if (const int drop = panel_text_drop(
                *font,
                hud_canvas(side_hud_.mission_text.x, side_hud_.mission_text.y).y,
                panel.mission_text,
                1
            );
            drop > 0)
            lowered = drop + oa::present::text_border(1, oa::present::game_font_text_size);
    if (panel.show_rates)
        draw_unit_rates(*unit, lowered);
    if (panel.show_kills)
        draw_hud_label(
            side_hud_.damage_bar.x,
            side_hud_.damage_bar.y + side_hud_.damage_bar.height - 1 + 2,
            panel.kills,
            hud::readout_color(world.game, hud::kReadoutTextColor)
        );
    if (panel.mission_text[0] != '\0')
        draw_hud_label_centered(
            side_hud_.mission_text.x, side_hud_.mission_text.y, panel.mission_text, text_color
        );
    if (panel.second == hud::PanelSecondUnit::stockpile) {
        draw_hud_label_centered(
            side_hud_.unit_name2.x,
            side_hud_.unit_name2.y,
            hud::panel_stockpile_label(localize, this),
            text_color
        );
        meter(side_hud_.damage_bar2, panel.stockpile_percent, hud::kStockpileBarFull);
    } else if (panel.second == hud::PanelSecondUnit::target) {
        const auto* target = oa::world_unit_at(&world, panel.second_unit);
        const auto* target_def =
            target != nullptr ? oa::world_unit_def_of(&world, target) : nullptr;
        if (target_def != nullptr) {
            draw_hud_label_centered(
                side_hud_.unit_name2.x,
                side_hud_.unit_name2.y,
                oa::data::languages::unit_display_name(*target_def),
                text_color
            );
            if (panel.second_damage)
                meter(
                    side_hud_.damage_bar2,
                    target->health,
                    static_cast<int32_t>(target_def->max_damage)
                );
        }
    }
}

oa::present::world_renderer::OverlayRaster Runtime::source_overlay_raster() {
    oa::present::world_renderer::OverlayRaster raster;
    raster.user = this;
    raster.rect_outline = [](void* user, oa::Surface*, const oa::Rect32& r, uint8_t color) {
        auto& self = *static_cast<Runtime*>(user);
        if (r.x2 < r.x1 || r.y2 < r.y1)
            return;
        const int width = r.x2 - r.x1 + 1;
        const int height = r.y2 - r.y1 + 1;
        self.fill_source_rect(r.x1, r.y1, width, 1, color);
        self.fill_source_rect(r.x1, r.y2, width, 1, color);
        self.fill_source_rect(r.x1, r.y1, 1, height, color);
        self.fill_source_rect(r.x2, r.y1, 1, height, color);
    };
    raster.fill_rect = [](void* user, oa::Surface*, const oa::Rect32& r, uint8_t color) {
        if (r.x2 < r.x1 || r.y2 < r.y1)
            return;
        static_cast<Runtime*>(user)->fill_source_rect(
            r.x1, r.y1, r.x2 - r.x1 + 1, r.y2 - r.y1 + 1, color
        );
    };
    raster.text = [](void* user, oa::Surface*, const char* text, int32_t x, int32_t y) {
        auto& self = *static_cast<Runtime*>(user);
        self.draw_hud_label(x, y, text, self.ui_colors_[kUiColorText]);
    };
    return raster;
}

void Runtime::absorb_radar_exploration() {
    if (!match_ || !match_mapping_on())
        return;
    const auto& sight = match_->sight();
    const auto cells = static_cast<std::size_t>(std::max(0, sight.width)) *
                       static_cast<std::size_t>(std::max(0, sight.height));
    if (cells == 0)
        return;
    if (radar_explored_.size() != cells)
        radar_explored_.assign(cells, 0);
    const uint8_t viewer = match_view_player();
    const auto bit = static_cast<uint16_t>(1u << (viewer & 0x1fu));
    // A viewer outside the player table has no live coverage.
    const std::span<const uint16_t> coverage =
        viewer < OA_PLAYER_COUNT ? match_->player_coverage(viewer) : std::span<const uint16_t>{};
    for (std::size_t i = 0; i < cells; ++i) {
        if (radar_explored_[i] != 0)
            continue;
        const bool mapped = i < sight.player_bits.size() && (sight.player_bits[i] & bit) != 0;
        const bool live =
            i < coverage.size() && oa::sim::visibility_state::count_in_sight(
                                       coverage[i], oa::sim::visibility_state::SightReading::player
                                   );
        if (mapped || live)
            radar_explored_[i] = 1;
    }
}

void Runtime::blit_match_minimap() {
    auto& radar = radar_state_;
    if (!match_ || radar.built_for != &match_->state() || radar.well == nullptr)
        return;
    auto& game = match_->state().game;
    const int pic_w = game.radar_width;
    const int pic_h = game.radar_height;
    if (pic_w <= 0 || pic_h <= 0)
        return;
    run_radar_ticks();
    oa::present::world_renderer::radar_draw(game, radar.surfaces, *radar.well);
    const int off_x = game.radar_offset_x;
    const int off_y = game.radar_offset_y;
    renderer::Surface mini;
    mini.width = static_cast<uint32_t>(pic_w);
    mini.height = static_cast<uint32_t>(pic_h);
    mini.rgb.assign(static_cast<std::size_t>(pic_w) * static_cast<std::size_t>(pic_h) * 3U, 0);
    const auto* well = radar.well;
    for (int y = 0; y < pic_h; ++y) {
        const uint8_t* row = well->pixels + (off_y + y) * well->pitch + off_x;
        for (int x = 0; x < pic_w; ++x) {
            const auto pal = static_cast<std::size_t>(row[x]) * 4U;
            const auto di = (static_cast<std::size_t>(y) * static_cast<std::size_t>(pic_w) +
                             static_cast<std::size_t>(x)) *
                            3U;
            mini.rgb[di] = match_palette_[pal];
            mini.rgb[di + 1] = match_palette_[pal + 1];
            mini.rgb[di + 2] = match_palette_[pal + 2];
        }
    }
    const auto origin = hud_canvas(off_x, off_y);
    const auto extent = hud_canvas(off_x + pic_w, off_y + pic_h);
    const int dest_w = std::max(1, extent.x - origin.x);
    const int dest_h = std::max(1, extent.y - origin.y);
    scale_blit(paint_target(), mini, origin.x, origin.y, dest_w, dest_h, 0, 0, pic_w, pic_h);
    // Radar clicks arrive in canvas pixels, wherever the picture is painted;
    // on the phone layout only while a placed region shows the minimap (the
    // build drawer covers it while open).
    auto hit =
        oa::ui::display_layout::source_rect_to_canvas(match_layout_, off_x, off_y, pic_w, pic_h);
    if (oa::ui::display_layout::placed_mode(match_layout_)) {
        const auto* region = oa::ui::display_layout::source_region_at(match_layout_, off_x, off_y);
        if (region == nullptr || region->role != oa::ui::display_layout::RegionRole::minimap)
            hit = {};
    }
    radar_picture_ = {hit.x, hit.y, hit.width, hit.height};
    radar_map_w_ = game.map_pixel_width;
    radar_map_h_ = game.map_pixel_height;
}

void Runtime::draw_status_panel() {
    namespace hud = oa::ui::hud;
    if (!match_)
        return;
    const bool held = (action_key_held("game.status-strip") ||
                       action_latched("game.status-strip") || space_held_by_check_) &&
                      !chat_composing_;
    auto& game = match_->state().game;
    // The strip sounds "Panel" as it leaves an end and "Options" as it
    // reaches one, as the kills board does.
    const auto at_end = [](int32_t offset) {
        return offset == 0 || offset == -hud::kStatusPanelRise;
    };
    const int32_t before = game.status_panel_offset;
    const auto now_ms = static_cast<uint32_t>(SDL_GetTicks());
    if (hud::status_panel_step(game, status_panel_next_step_ms_, now_ms, held)) {
        if (at_end(before))
            play_match_interface_sound("Panel");
        if (at_end(game.status_panel_offset))
            play_match_interface_sound("Options");
    }
    // The console's clock gives way to the strip, which shows the game time
    // in its place: hidden from the frame Space is held, it fades back in
    // once the strip is down and the kills board, drawn before it, has
    // stopped sliding.
    const bool board_still = kill_board_.slide == 0 || kill_board_.slide == hud::kBoardWidth;
    console_clock_opacity_ = hud::step_clock_fade(
        console_clock_fade_, held, game.status_panel_offset == 0 && board_still, now_ms
    );
    if (game.status_panel_offset == 0)
        return;
    if (!status_lightbar_loaded_) {
        status_lightbar_loaded_ = true;
        try {
            oa::formats::gaf::Archive archive;
            append_gaf_file(archive, "anims/commongui.gaf");
            const auto* sequence = gaf_sequence(archive, "LIGHTBAR");
            const auto index = static_cast<std::size_t>(hud::kStatusPanelLightbarFrame);
            if (sequence != nullptr && sequence->frames.size() > index) {
                auto rendered = oa::formats::gaf::render_normal(sequence->frames[index]);
                if (rendered.ok()) {
                    // The status panel reset zeros the frame origin.
                    rendered.frame->origin_x = 0;
                    rendered.frame->origin_y = 0;
                    status_lightbar_ = std::move(*rendered.frame);
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "status strip LIGHTBAR unavailable: " << error.what() << '\n';
        }
    }
    // The strip rises over the bottom of the overlays' area (the
    // battlefield, or the part of it the touch controls leave clear) from
    // its left edge, at the text's scale, less while it would be wider than
    // the area. Its top row lies the offset above the area's last row, and
    // only the rows down to that last row show, as the game's view cuts the
    // strip off: the bottom bar keeps its pixels.
    const auto area = overlay_area();
    int scale = hud_text_scale();
    if (status_lightbar_)
        while (scale > 1 && static_cast<int>(status_lightbar_->width) * scale > area.width)
            --scale;
    const int area_bottom = area.y + area.height;
    const int area_right = area.x + area.width;
    const int rows_shown = 1 - game.status_panel_offset;
    const auto row_top = [&](int row) { return area_bottom - (rows_shown - row) * scale; };
    if (status_lightbar_) {
        const auto& frame = *status_lightbar_;
        const int rows = std::min(rows_shown, static_cast<int>(frame.height));
        for (int row = 0; row < rows; ++row)
            for (std::size_t column = 0; column < frame.width; ++column) {
                const int x = area.x + static_cast<int>(column) * scale;
                if (x >= area_right)
                    break;
                const auto at = static_cast<std::size_t>(row) * frame.width + column;
                if (at >= frame.coverage.size() || frame.coverage[at] == 0)
                    continue;
                const auto corner = canvas_paint(x, row_top(row));
                fill_hud_rect(
                    corner.x, corner.y, std::min(scale, area_right - x), scale, frame.pixels[at]
                );
            }
    }
    const int rows_below_pen = rows_shown - hud::kStatusPanelTextDrop;
    if (rows_below_pen <= 0)
        return;
    const auto translate = [](void* context, const char* text) -> const char* {
        auto& runtime = *static_cast<Runtime*>(context);
        runtime.status_label_ = runtime.translate_ui(text);
        return runtime.status_label_.c_str();
    };
    hud::StatusPanelText text{};
    hud::format_status_panel(game, translate, this, text);
    const auto pen = [&](int x) {
        return canvas_paint(area.x + x * scale, row_top(hud::kStatusPanelTextDrop));
    };
    // The readouts are gadget text in the GUI's second font, cut off at the
    // area's last row as the strip is.
    ensure_gui_font();
    const std::pair<int, const char*> readouts[] = {
        {hud::kStatusPanelTimeX, text.time},
        {hud::kStatusPanelUnitsX, text.units},
        {hud::kStatusPanelSpeedX, text.speed}
    };
    if (!gui_label_font_.sequences.empty()) {
        for (const auto& [x, line] : readouts)
            overlay_gui_text(gui_label_font_, pen(x), line, rows_below_pen, true, scale);
        return;
    }
    const oa::formats::fnt::Font* font = match_label_font();
    if (font == nullptr || static_cast<int>(oa::formats::fnt::line_height(*font)) > rows_below_pen)
        return;
    for (const auto& [x, line] : readouts) {
        const auto at = pen(x);
        draw_match_label(at.x, at.y, line, hud::kPaletteWhite, scale);
    }
}

void Runtime::draw_key_latch_chips() {
    key_latch_chips_drawn_.clear();
    if (screen_ != Screen::match || match_ == nullptr)
        return;
    std::vector<KeyLatchChip> chips;
    gather_key_latch_chips(chips);
    if (chips.empty())
        return;

    const auto area = overlay_area();
    const auto corner = canvas_paint(area.x, area.y);
    const int scale = std::max(1, hud_text_scale());
    const kit::Canvas canvas = overlay_kit_canvas(
        paint_target(),
        engine_settings_fonts(),
        corner,
        scale,
        {corner.x, corner.y, area.width, area.height}
    );
    const int32_t gap = 4;
    const int32_t chip_height = kit::compact_metrics.chip_height;
    const int32_t top = canvas.placement.clip.y + 6;
    const int32_t span = canvas.placement.clip.width;

    struct Placed {
        std::string text;
        bool filled;
        int32_t width;
    };

    const auto width_of = [&](const std::string& text, bool filled) {
        kit::ChipLook look;
        look.text = text;
        look.state = filled ? kit::ChipState::playing : kit::ChipState::installed;
        if (canvas.fonts == nullptr)
            return 2 * kit::compact_metrics.chip_padding;
        return kit::chip_width(*canvas.fonts, look);
    };
    if (span <= 0 || canvas.surface == nullptr) {
        for (const KeyLatchChip& chip : chips)
            key_latch_chips_drawn_.push_back(chip.text);
        return;
    }

    std::vector<Placed> row;
    int32_t used_width = 0;
    int dropped = 0;
    for (const KeyLatchChip& chip : chips) {
        const int32_t width = width_of(chip.text, chip.filled);
        const int32_t next = used_width + (row.empty() ? 0 : gap) + width;
        if (!row.empty() && next > span) {
            ++dropped;
            continue;
        }
        used_width = next;
        row.push_back(Placed{chip.text, chip.filled, width});
    }
    if (dropped > 0) {
        const auto more_width = [&](int count) {
            return width_of("+" + std::to_string(count), true);
        };
        int extra = dropped;
        while (!row.empty()) {
            const int32_t width = more_width(extra);
            const int32_t with_more = used_width + gap + width;
            if (with_more <= span)
                break;
            used_width -= row.back().width;
            if (row.size() > 1)
                used_width -= gap;
            row.pop_back();
            ++extra;
        }
        row.push_back(Placed{"+" + std::to_string(extra), true, more_width(extra)});
    }

    int32_t total = 0;
    for (std::size_t index = 0; index < row.size(); ++index)
        total += row[index].width + (index == 0 ? int32_t{0} : gap);
    int32_t origin = canvas.placement.clip.x;
    if (total < span)
        origin += (span - total) / 2;
    for (const Placed& item : row) {
        key_latch_chips_drawn_.push_back(item.text);
        kit::ChipLook look;
        look.text = item.text;
        look.state = item.filled ? kit::ChipState::playing : kit::ChipState::installed;
        kit::draw_chip(canvas, {origin, top, item.width, chip_height}, look);
        origin += item.width + gap;
    }
}

} // namespace oa::app
