// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Painting the Game files screen (game_files_paint.hpp): the background, then
// each of the kit's parts in order, by its role, with the touch controls'
// painter. Boxes, panels and buttons have small rounded corners and hairline
// outlines in the settings dialog's colours, text is antialiased in the
// bundled fonts, and the screen's marks are drawn as strokes on a 24-unit
// square, but for the OA mark: the Open Annihilation icon, which the settings
// dialog's own drawing draws.
#include "game_files_paint.hpp"

#include "oa/app/window_icon.hpp"
#include "oa/formats/png.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

namespace oa::app {

namespace {

namespace paint = oa::ui::paint;
namespace gf = oa::ui::game_files;
namespace kit = oa::ui::kit;
namespace text_font = oa::platform::text_font;
namespace renderer = oa::ui::frontend_renderer;
using paint::Area;
using paint::Box;
using paint::Painter;
using paint::Rgba;
using paint::Spot;

/// The header bar's fill, a little darker than the screen behind it.
constexpr gf::Colour header_colour{0x14, 0x16, 0x12};
/// A banner's fill and outline: the panel warmed towards amber.
constexpr gf::Colour banner_fill_colour{0x21, 0x1f, 0x16};
constexpr gf::Colour banner_outline_colour{0x5a, 0x4a, 0x26};
/// A progress bar's track, darker than the background.
constexpr gf::Colour track_colour{0x0b, 0x0c, 0x09};
/// Corners of buttons, cards, rows and banners, in points.
constexpr float corner_points = 2.0F;
/// Corners of sheets, in points.
constexpr float sheet_corner_points = 4.0F;
/// Hairline outlines, in points.
constexpr float hairline_points = 1.0F;
/// A sheet's outline, in points.
constexpr float sheet_outline_points = 2.0F;
/// The bar at a banner's left edge, in points.
constexpr float banner_bar_points = 3.0F;
/// A banner's own mark when the item names one, in points, and its gap from the bar.
constexpr float banner_mark_points = 16.0F;
constexpr float banner_mark_gap_points = 12.0F;
/// The soft shadow round a sheet, in points.
constexpr float sheet_shadow_points = 8.0F;
/// How dark a backdrop makes what lies under it, 0 to 1.
constexpr float backdrop_opacity = 0.58F;
/// The light laid over a held control, 0 to 1.
constexpr float pressed_light = 0.12F;
/// How a disabled button is drawn: every part at this opacity, 0 to 1.
constexpr float disabled_opacity = 0.40F;
/// The focus ring: its width and its gap outside the item, in points, and its corners.
constexpr float focus_ring_points = 2.0F;
constexpr float focus_gap_points = 3.0F;
constexpr float focus_corner_points = 4.0F;
/// A busy bar's stripes: their spacing and width in points, and their opacity.
constexpr float busy_stripe_spacing_points = 12.0F;
constexpr float busy_stripe_width_points = 5.0F;
constexpr float busy_stripe_opacity = 0.40F;
/// An icon's mark, as a share of its box's shorter side.
constexpr float icon_mark_share = 0.60F;
/// The gap between a button's mark and its label, as a share of the label's size.
constexpr float button_mark_gap_em = 0.5F;
/// The smallest button the layout makes, in points, from which the density is
/// estimated when the caller does not give it.
constexpr float smallest_button_points = gf::min_button_points;
/// The OA mark without the icon: one canvas pixel of a block for every this many pixels of
/// its square's side, the settings dialog header's icon place, so the mark keeps the dialog's
/// proportions at every size.
constexpr int oa_mark_block_side = 20;

/// Returns a rectangle of whole pixels as one with fractions.
///
/// @param rect the rectangle
/// @return the same area
[[nodiscard]] Area area_of(const gf::Rect& rect) noexcept {
    return Area{
        static_cast<float>(rect.x),
        static_cast<float>(rect.y),
        static_cast<float>(rect.width),
        static_cast<float>(rect.height)
    };
}

/// Returns a rectangle as the painter's box.
///
/// @param rect the rectangle
/// @return the same pixels
[[nodiscard]] Box box_of(const gf::Rect& rect) noexcept {
    return Box{rect.x, rect.y, rect.width, rect.height};
}

/// Grows an area by a margin on every side.
///
/// @param area the area
/// @param margin pixels; negative shrinks it
/// @return the grown area
[[nodiscard]] Area grown(Area area, float margin) noexcept {
    return Area{
        area.x - margin, area.y - margin, area.width + 2.0F * margin, area.height + 2.0F * margin
    };
}

/// Returns the style the screens draw a line in: antialiased, at a size, in a
/// family. A bold line is DejaVu Sans Bold in the sans family and Barlow
/// SemiBold, the modern look's labels' weight, in the interface family.
///
/// @param pixel_size pixels per em; clamped to the sizes the fonts draw
/// @param bold whether the line is bold
/// @param family the family
/// @return the style
[[nodiscard]] text_font::Style
line_style(int pixel_size, bool bold, text_font::Family family) noexcept {
    text_font::Style style{};
    style.pixel_size = std::clamp(pixel_size, 1, text_font::max_pixel_size);
    const text_font::Weight heavy =
        family == text_font::Family::sans ? text_font::Weight::bold : text_font::Weight::semibold;
    style.weight = bold ? heavy : text_font::Weight::regular;
    style.rendering = text_font::Rendering::antialiased;
    style.family = family;
    return style;
}

/// Draws a line in the screens' style (line_style).
///
/// @param fonts the bundled fonts
/// @param text UTF-8 text
/// @param pixel_size pixels per em
/// @param bold whether the line is bold
/// @param family the family
/// @return the line; not drawn for text the fonts refuse
[[nodiscard]] paint::TextLine draw_screen_line(
    text_font::FontStack& fonts,
    std::string_view text,
    int pixel_size,
    bool bold,
    text_font::Family family
) {
    paint::TextLine line{};
    if (auto coverage = fonts.draw(text, line_style(pixel_size, bold, family))) {
        line.coverage = std::move(*coverage);
        line.drawn = true;
    }
    return line;
}

/// Returns how far a line in the screens' style moves the pen.
///
/// @param fonts the bundled fonts
/// @param text UTF-8 text
/// @param pixel_size pixels per em
/// @param bold whether the line is bold
/// @param family the family
/// @return pixels; 0 for text the fonts refuse
[[nodiscard]] int screen_text_width(
    text_font::FontStack& fonts,
    std::string_view text,
    int pixel_size,
    bool bold,
    text_font::Family family
) {
    if (text.empty())
        return 0;
    const auto placed = fonts.layout(text, line_style(pixel_size, bold, family));
    if (!placed || placed->empty())
        return 0;
    return placed->back().pen + placed->back().advance;
}

/// Returns the rows a line in the screens' style takes.
///
/// @param fonts the bundled fonts
/// @param pixel_size pixels per em
/// @param bold whether the line is bold
/// @param family the family
/// @return the rows; zeros for a size the fonts refuse
[[nodiscard]] text_font::LineMetrics screen_line_metrics(
    text_font::FontStack& fonts, int pixel_size, bool bold, text_font::Family family
) {
    return fonts.metrics(line_style(pixel_size, bold, family)).value_or(text_font::LineMetrics{});
}

/// What paints one layout: the painter, the fonts and the density.
class LayoutPainter {
  public:

    /// Starts painting a canvas.
    ///
    /// @param canvas the canvas
    /// @param fonts the bundled fonts; null paints no text
    /// @param px_per_point canvas pixels per point
    /// @param icon the Open Annihilation icon; empty draws the OA mark
    /// @param family the family the text is drawn in
    LayoutPainter(
        paint::Canvas& canvas,
        text_font::FontStack* fonts,
        float px_per_point,
        const renderer::RgbaPicture& icon,
        text_font::Family family
    )
        : painter_(canvas), fonts_(fonts), scale_(std::max(px_per_point, 0.25F)), icon_(icon),
          family_(family) {}

    /// Paints the background and every part, in order.
    ///
    /// @param parts the parts
    void paint(const kit::DisplayList& parts) {
        // A new paint: text that crosses other text is judged within this
        // frame, while records from earlier frames of the same screen stay.
        kit::text_fit_begin_paint();
        auto& canvas = painter_.canvas();
        const Box whole{0, 0, canvas.width, canvas.height};
        painter_.reset_clip();
        painter_.clear_box(whole);
        painter_.fill_rect(whole, game_files_rgba(gf::background_colour));
        for (const kit::Item& item : parts.items) {
            set_item_clip(item);
            paint_item(item);
            if (item.state.focused)
                paint_focus(item);
        }
        painter_.reset_clip();
    }

  private:

    /// How a block of lines sits across its box.
    enum class Align : uint8_t {
        left,   ///< at the box's left edge
        centre, ///< centred across it
    };

    /// Returns a length in points as pixels.
    ///
    /// @param points the length
    /// @return pixels
    [[nodiscard]] float px(float points) const noexcept { return points * scale_; }

    /// Returns a length in points as whole pixels, at least one.
    ///
    /// @param points the length
    /// @return pixels
    [[nodiscard]] int whole_px(float points) const noexcept {
        return std::max(1, static_cast<int>(std::lround(px(points))));
    }

    /// Limits painting to an item's clip, when it has one.
    ///
    /// @param item the item
    void set_item_clip(const kit::Item& item) noexcept {
        if (item.clip.width > 0 && item.clip.height > 0)
            painter_.set_clip(box_of(item.clip));
        else
            painter_.reset_clip();
    }

    /// Lays the held look over an item's box.
    ///
    /// @param item the item
    /// @param area its box
    void paint_pressed(const kit::Item& item, Area area) {
        if (item.state.pressed && !item.state.disabled)
            painter_.fill_rounded_rect(
                area,
                px(corner_points),
                Rgba{255, 255, 255, static_cast<uint8_t>(std::lround(255.0F * pressed_light))}
            );
    }

    /// Paints one part by its role. A role the screen does not lay out draws nothing.
    ///
    /// @param item the part
    void paint_item(const kit::Item& item) {
        const Area area = area_of(item.rect);
        switch (item.role) {
        case kit::Role::header_bar:
            painter_.fill_rect(box_of(item.rect), game_files_rgba(header_colour));
            painter_.fill_rect(
                Box{item.rect.x,
                    item.rect.y + item.rect.height - whole_px(hairline_points),
                    item.rect.width,
                    whole_px(hairline_points)},
                game_files_rgba(gf::line_colour)
            );
            paint_text(item, Align::left);
            return;
        case kit::Role::footer_band:
            // The folder chooser's footer. The Game files screen lays out no band of this role.
            painter_.fill_rect(box_of(item.rect), game_files_rgba(header_colour));
            painter_.fill_rect(
                Box{item.rect.x, item.rect.y, item.rect.width, whole_px(hairline_points)},
                game_files_rgba(gf::line_colour)
            );
            return;
        case kit::Role::badge:
            paint_badge(area);
            return;
        case kit::Role::header_text:
        case kit::Role::version:
        case kit::Role::title:
        case kit::Role::text:
            if (item.glyph != gf::Glyph::none && item.lines.empty()) {
                paint_mark(item.glyph, area, game_files_rgba(item.colour));
                return;
            }
            paint_text(item, Align::left);
            return;
        case kit::Role::panel:
            painter_.fill_rounded_rect(area, px(corner_points), game_files_rgba(gf::panel_colour));
            painter_.outline_rounded_rect(
                area, px(corner_points), px(hairline_points), game_files_rgba(gf::line_colour)
            );
            paint_pressed(item, area);
            paint_text(item, Align::left);
            return;
        case kit::Role::icon:
            paint_icon(item, area);
            return;
        case kit::Role::banner:
            paint_banner(item, area);
            return;
        case kit::Role::row:
            painter_.fill_rect(box_of(item.rect), game_files_rgba(gf::panel_colour));
            paint_pressed(item, area);
            paint_text(item, Align::left);
            return;
        case kit::Role::divider:
            painter_.fill_rect(box_of(item.rect), game_files_rgba(gf::line_colour));
            return;
        case kit::Role::toggle:
            paint_switch(item, area);
            return;
        case kit::Role::button_main:
        case kit::Role::button:
        case kit::Role::button_danger:
            paint_button(item, area);
            return;
        case kit::Role::progress_track:
            painter_.fill_rect(box_of(item.rect), game_files_rgba(track_colour));
            painter_.outline_rect(
                box_of(item.rect), whole_px(hairline_points), game_files_rgba(gf::line_colour)
            );
            return;
        case kit::Role::progress_fill:
            // The layout sizes the fill's box from its fraction.
            if (item.rect.width > 0 && item.rect.height > 0)
                painter_.fill_rect(box_of(item.rect), game_files_rgba(gf::green_colour));
            return;
        case kit::Role::progress_busy:
            paint_busy(item, area);
            return;
        case kit::Role::backdrop:
            painter_.fill_rect(
                box_of(item.rect),
                Rgba{0, 0, 0, static_cast<uint8_t>(std::lround(255.0F * backdrop_opacity))}
            );
            return;
        case kit::Role::sheet:
            paint_sheet(item, area);
            return;
        default:
            return;
        }
    }

    /// Returns the rows one line takes at a size: its ascent and descent.
    ///
    /// @param pixel_size the size
    /// @param bold the weight
    /// @return the metrics; zeros without fonts
    [[nodiscard]] text_font::LineMetrics metrics(int pixel_size, bool bold) const {
        if (fonts_ == nullptr || pixel_size <= 0)
            return {};
        return screen_line_metrics(*fonts_, pixel_size, bold, family_);
    }

    /// Paints an item's lines in a colour, top to bottom from the top of a box, one line
    /// height apart, or centred down the box (a button's label).
    ///
    /// @param item the item
    /// @param colour the text's colour
    /// @param align across the box
    /// @param box where the lines go
    /// @param centred_down centre the block of lines down the box
    void paint_lines(
        const kit::Item& item, Rgba colour, Align align, const gf::Rect& box, bool centred_down
    ) {
        if (fonts_ == nullptr || item.lines.empty() || item.pixel_size <= 0)
            return;
        const auto rows = metrics(item.pixel_size, item.bold);
        const int line_height = rows.ascent + rows.descent;
        if (line_height <= 0)
            return;
        const int total = line_height * static_cast<int>(item.lines.size());
        const int top = centred_down ? box.y + (box.height - total) / 2 : box.y;
        int baseline = top + rows.ascent;
        for (const std::string& text : item.lines) {
            const auto line = draw_screen_line(*fonts_, text, item.pixel_size, item.bold, family_);
            int pen = box.x;
            if (align == Align::centre)
                pen = box.x + (box.width - line.coverage.advance) / 2;
            // One line's own box, so stacked lines of one item are not taken
            // to cross each other. The width is the item's.
            kit::note_text_fit(
                text,
                line.coverage.advance,
                box.x,
                baseline - rows.ascent,
                box.width,
                line_height,
                pen
            );
            paint::paint_line(painter_, line, pen, baseline, colour);
            baseline += line_height;
        }
    }

    /// Paints an item's lines in its own colour from the top of its box.
    ///
    /// @param item the item
    /// @param align across the box
    void paint_text(const kit::Item& item, Align align) {
        paint_lines(item, game_files_rgba(item.colour), align, item.rect, false);
    }

    /// Paints a mark: the OA mark as the icon, every other one as strokes.
    ///
    /// @param glyph the mark
    /// @param area where it goes
    /// @param colour the strokes' colour
    void paint_mark(gf::Glyph glyph, Area area, Rgba colour) {
        if (glyph == gf::Glyph::oa)
            paint_game_files_oa_mark(painter_, area, icon_);
        else
            paint_game_files_glyph(painter_, glyph, area, colour);
    }

    /// Paints the OA badge: the Open Annihilation icon filling its box, or the OA mark.
    ///
    /// @param area its box
    void paint_badge(Area area) { paint_game_files_oa_mark(painter_, area, icon_); }

    /// Paints a mark at its share of the box, framed when the item asks for it.
    ///
    /// @param item the icon item
    /// @param area its box
    void paint_icon(const kit::Item& item, Area area) {
        if (item.state.on)
            painter_.outline_rounded_rect(
                area, px(corner_points), px(hairline_points), game_files_rgba(gf::line_colour)
            );
        const float side = std::min(area.width, area.height) * icon_mark_share;
        paint_mark(
            item.glyph,
            Area{
                area.x + (area.width - side) * 0.5F,
                area.y + (area.height - side) * 0.5F,
                side,
                side
            },
            game_files_rgba(item.colour)
        );
    }

    /// Paints a banner: its warm fill and outline, the bar of its colour at its left edge
    /// and, when the item names one, its mark.
    ///
    /// @param item the banner
    /// @param area its box
    void paint_banner(const kit::Item& item, Area area) {
        const Rgba accent = game_files_rgba(item.colour);
        painter_.fill_rect(box_of(item.rect), game_files_rgba(banner_fill_colour));
        painter_.outline_rect(
            box_of(item.rect), whole_px(hairline_points), game_files_rgba(banner_outline_colour)
        );
        painter_.fill_rect(
            Box{item.rect.x, item.rect.y, whole_px(banner_bar_points), item.rect.height}, accent
        );
        if (item.glyph != gf::Glyph::none) {
            const float side = std::min(px(banner_mark_points), area.height);
            paint_mark(
                item.glyph,
                Area{
                    area.x + px(banner_bar_points) + px(banner_mark_gap_points),
                    area.y + (area.height - side) * 0.5F,
                    side,
                    side
                },
                accent
            );
        }
    }

    /// Paints a sheet: a soft shadow, its fill and its outline.
    ///
    /// @param item the sheet
    /// @param area its box
    void paint_sheet(const kit::Item& item, Area area) {
        const float shadow = px(sheet_shadow_points);
        constexpr int shadow_steps = 4;
        constexpr uint8_t shadow_step_alpha = 22;
        for (int step = shadow_steps; step >= 1; --step) {
            const float reach =
                shadow * static_cast<float>(step) / static_cast<float>(shadow_steps);
            painter_.fill_rounded_rect(
                grown(area, reach),
                px(sheet_corner_points) + reach,
                Rgba{0, 0, 0, shadow_step_alpha}
            );
        }
        painter_.fill_rounded_rect(
            area, px(sheet_corner_points), game_files_rgba(gf::background_colour)
        );
        painter_.outline_rounded_rect(
            area,
            px(sheet_corner_points),
            px(sheet_outline_points),
            game_files_rgba(gf::line_colour)
        );
        paint_text(item, Align::left);
    }

    /// Paints a button: its look, its mark and its label, centred.
    ///
    /// @param item the button
    /// @param area its box
    void paint_button(const kit::Item& item, Area area) {
        const float corner = px(corner_points);
        const float hairline = px(hairline_points);
        const float opacity = item.state.disabled ? disabled_opacity : 1.0F;
        Rgba label = game_files_rgba(item.colour);
        if (item.role == kit::Role::button_main) {
            painter_.fill_rounded_rect(
                area, corner, paint::with_opacity(game_files_rgba(gf::green_colour), opacity)
            );
            label = game_files_rgba(gf::background_colour);
            if (item.state.disabled)
                label = game_files_rgba(gf::text_colour);
        } else if (item.role == kit::Role::button_danger) {
            painter_.outline_rounded_rect(
                area,
                corner,
                hairline,
                paint::with_opacity(game_files_rgba(gf::red_colour), opacity)
            );
            label = game_files_rgba(gf::red_colour);
        } else {
            painter_.outline_rounded_rect(
                area,
                corner,
                hairline,
                paint::with_opacity(game_files_rgba(gf::line_colour), opacity)
            );
        }
        paint_pressed(item, area);
        paint_label(item, paint::with_opacity(label, opacity));
    }

    /// Paints a button's label, with its mark before it, the pair centred in the box.
    ///
    /// @param item the button
    /// @param colour the label's colour
    void paint_label(const kit::Item& item, Rgba colour) {
        if (item.glyph == gf::Glyph::none || item.pixel_size <= 0) {
            paint_lines(item, colour, Align::centre, item.rect, true);
            return;
        }
        const int mark = item.glyph_size > 0 ? item.glyph_size : item.pixel_size;
        const int gap =
            static_cast<int>(std::lround(static_cast<float>(item.pixel_size) * button_mark_gap_em));
        int widest = 0;
        if (fonts_ != nullptr)
            for (const std::string& text : item.lines)
                widest = std::max(
                    widest, screen_text_width(*fonts_, text, item.pixel_size, item.bold, family_)
                );
        const int group = mark + (widest > 0 ? gap + widest : 0);
        const int left = item.rect.x + (item.rect.width - group) / 2;
        paint_mark(
            item.glyph,
            Area{
                static_cast<float>(left),
                static_cast<float>(item.rect.y) +
                    static_cast<float>(item.rect.height - mark) * 0.5F,
                static_cast<float>(mark),
                static_cast<float>(mark)
            },
            colour
        );
        if (widest > 0) {
            gf::Rect text_box = item.rect;
            text_box.x = left + mark + gap;
            text_box.width = widest;
            paint_lines(item, colour, Align::left, text_box, true);
        }
    }

    /// Paints an OFF/ON switch: two halves, the one in effect filled.
    ///
    /// @param item the switch
    /// @param area its box
    void paint_switch(const kit::Item& item, Area area) {
        const int half = item.rect.width / 2;
        const gf::Rect off_box{item.rect.x, item.rect.y, half, item.rect.height};
        const gf::Rect on_box{
            item.rect.x + half, item.rect.y, item.rect.width - half, item.rect.height
        };
        const float opacity = item.state.disabled ? disabled_opacity : 1.0F;
        const gf::Rect& lit = item.state.on ? on_box : off_box;
        const Rgba lit_fill =
            item.state.on ? game_files_rgba(gf::green_colour) : game_files_rgba(gf::line_colour);
        painter_.fill_rect(box_of(lit), paint::with_opacity(lit_fill, opacity));
        painter_.outline_rect(
            box_of(item.rect),
            whole_px(hairline_points),
            paint::with_opacity(game_files_rgba(gf::line_colour), opacity)
        );
        paint_pressed(item, area);
        if (item.lines.size() < 2 || fonts_ == nullptr)
            return;
        const Rgba lit_text = paint::with_opacity(
            item.state.on ? game_files_rgba(gf::background_colour)
                          : game_files_rgba(gf::text_colour),
            opacity
        );
        const Rgba unlit_text = paint::with_opacity(game_files_rgba(gf::dim_colour), opacity);
        kit::Item label = item;
        label.lines = {item.lines[0]};
        paint_lines(label, item.state.on ? unlit_text : lit_text, Align::centre, off_box, true);
        label.lines = {item.lines[1]};
        paint_lines(label, item.state.on ? lit_text : unlit_text, Align::centre, on_box, true);
    }

    /// Paints a busy bar: the track with green stripes at 45 degrees.
    ///
    /// @param item the bar
    /// @param area its box
    void paint_busy(const kit::Item& item, Area area) {
        painter_.fill_rect(box_of(item.rect), game_files_rgba(track_colour));
        const Box kept = painter_.clip();
        painter_.set_clip(paint::intersect(kept, box_of(item.rect)));
        const float spacing = std::max(px(busy_stripe_spacing_points), 4.0F);
        const Rgba stripe =
            paint::with_opacity(game_files_rgba(gf::green_colour), busy_stripe_opacity);
        for (float x = area.x - area.height; x < area.x + area.width + area.height; x += spacing)
            painter_.stroke_line(
                Spot{x, area.y + area.height},
                Spot{x + area.height, area.y},
                px(busy_stripe_width_points),
                stripe
            );
        painter_.set_clip(kept);
        painter_.outline_rect(
            box_of(item.rect), whole_px(hairline_points), game_files_rgba(gf::line_colour)
        );
    }

    /// Paints the focus ring outside an item.
    ///
    /// @param item the focused item
    void paint_focus(const kit::Item& item) {
        painter_.reset_clip();
        const Area ring = grown(area_of(item.rect), px(focus_gap_points) + px(focus_ring_points));
        painter_.outline_rounded_rect(
            ring, px(focus_corner_points), px(focus_ring_points), game_files_rgba(gf::green_colour)
        );
    }

    Painter painter_;                   ///< paints the canvas
    text_font::FontStack* fonts_{};     ///< the bundled fonts; null paints no text
    float scale_{1.0F};                 ///< canvas pixels per point
    const renderer::RgbaPicture& icon_; ///< the Open Annihilation icon; empty: the OA mark
    text_font::Family family_{};        ///< the family the text is drawn in
};

/// Measures a line's width in the bundled fonts in a family (TextMeasureHooks::width).
template <text_font::Family family>
int measure_width(void* context, std::string_view text, int pixel_size, bool bold) {
    if (context == nullptr || text.empty() || pixel_size <= 0)
        return 0;
    return screen_text_width(
        *static_cast<text_font::FontStack*>(context), text, pixel_size, bold, family
    );
}

/// Measures a line's height in the bundled fonts in a family (TextMeasureHooks::line_height).
template <text_font::Family family>
int measure_line_height(void* context, int pixel_size, bool bold) {
    if (context == nullptr || pixel_size <= 0)
        return 0;
    const auto rows =
        screen_line_metrics(*static_cast<text_font::FontStack*>(context), pixel_size, bold, family);
    return rows.ascent + rows.descent;
}

/// Estimates the density a layout was made at from its smallest button, which is
/// min_button_points high.
///
/// @param layout the layout
/// @return canvas pixels per point; 1 without buttons
[[nodiscard]] float estimated_density(const gf::Layout& layout) noexcept {
    int smallest = std::numeric_limits<int>::max();
    for (const kit::Item& item : layout.list.items)
        if ((item.role == kit::Role::button || item.role == kit::Role::button_main ||
             item.role == kit::Role::button_danger) &&
            item.rect.height > 0)
            smallest = std::min(smallest, item.rect.height);
    if (smallest == std::numeric_limits<int>::max())
        return 1.0F;
    return std::max(1.0F, std::floor(static_cast<float>(smallest) / smallest_button_points));
}

} // namespace

Rgba game_files_rgba(gf::Colour colour) noexcept {
    return Rgba{colour.r, colour.g, colour.b, colour.a};
}

gf::TextMeasureHooks
game_files_measure(text_font::FontStack* fonts, text_font::Family family) noexcept {
    gf::TextMeasureHooks measure;
    measure.context = fonts;
    if (fonts == nullptr)
        return measure;
    switch (family) {
    case text_font::Family::interface:
    case text_font::Family::interface_condensed:
        measure.width = measure_width<text_font::Family::interface>;
        measure.line_height = measure_line_height<text_font::Family::interface>;
        break;
    case text_font::Family::sans:
        measure.width = measure_width<text_font::Family::sans>;
        measure.line_height = measure_line_height<text_font::Family::sans>;
        break;
    }
    return measure;
}

const renderer::RgbaPicture& game_files_icon() {
    static const WindowIcon icon = [] {
        WindowIcon decoded;
        std::string error;
        if (!decode_window_icon(window_icon_png(), decoded, error))
            return WindowIcon{};
        return visible_part(decoded);
    }();
    static const renderer::RgbaPicture picture{icon.width, icon.height, icon.pixels};
    return picture;
}

void paint_game_files(
    paint::Canvas& canvas, const gf::Layout& layout, text_font::FontStack* fonts
) {
    paint_game_files(canvas, layout, fonts, estimated_density(layout), game_files_icon());
}

void paint_game_files(
    paint::Canvas& canvas, const gf::Layout& layout, text_font::FontStack* fonts, float px_per_point
) {
    paint_game_files(canvas, layout, fonts, px_per_point, game_files_icon());
}

void paint_game_files(
    paint::Canvas& canvas,
    const gf::Layout& layout,
    text_font::FontStack* fonts,
    float px_per_point,
    const renderer::RgbaPicture& icon,
    text_font::Family family
) {
    paint_game_files(canvas, layout.list, fonts, px_per_point, icon, family);
}

void paint_game_files(
    paint::Canvas& canvas,
    const kit::DisplayList& parts,
    text_font::FontStack* fonts,
    float px_per_point,
    const renderer::RgbaPicture& icon,
    text_font::Family family
) {
    if (canvas.width <= 0 || canvas.height <= 0)
        return;
    // The modern look's labels are Barlow; a condensed family is for
    // titles in capitals, which the screens do not draw.
    const text_font::Family drawn =
        family == text_font::Family::sans ? family : text_font::Family::interface;
    LayoutPainter(canvas, fonts, px_per_point, icon, drawn).paint(parts);
}

void paint_game_files_oa_mark(Painter& painter, Area area, const renderer::RgbaPicture& icon) {
    const int side = static_cast<int>(std::lround(std::min(area.width, area.height)));
    if (side <= 0)
        return;
    const Box square{
        static_cast<int>(std::lround(area.x + (area.width - static_cast<float>(side)) * 0.5F)),
        static_cast<int>(std::lround(area.y + (area.height - static_cast<float>(side)) * 0.5F)),
        side,
        side
    };
    const Box drawn = paint::intersect(painter.clip(), square);
    if (drawn.width <= 0 || drawn.height <= 0)
        return;
    // The settings dialog draws on RGB pixels: the canvas's own under the square go in, and
    // the pixels the mark changed come back.
    auto& canvas = painter.canvas();
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(drawn.width);
    surface.height = static_cast<uint32_t>(drawn.height);
    surface.rgb.resize(static_cast<std::size_t>(drawn.width) * drawn.height * 3U);
    const auto canvas_pixel = [&canvas](int x, int y) {
        return &canvas.rgba[(static_cast<std::size_t>(y) * canvas.width + x) * 4U];
    };
    for (int y = 0; y < drawn.height; ++y)
        for (int x = 0; x < drawn.width; ++x) {
            const uint8_t* from = canvas_pixel(drawn.x + x, drawn.y + y);
            std::copy_n(
                from, 3, &surface.rgb[(static_cast<std::size_t>(y) * drawn.width + x) * 3U]
            );
        }
    const std::vector<uint8_t> under = surface.rgb;
    if (renderer::picture_drawable(icon)) {
        oa::ui::engine_settings::draw_oa_mark(
            surface, {square.x - drawn.x, square.y - drawn.y, 1}, side, icon
        );
    } else {
        const int block = std::max(1, side / oa_mark_block_side);
        const int source_side = side / block;
        const int inset = (side - source_side * block) / 2;
        oa::ui::engine_settings::draw_oa_mark(
            surface,
            {square.x - drawn.x + inset, square.y - drawn.y + inset, block},
            source_side,
            renderer::RgbaPicture{}
        );
    }
    for (int y = 0; y < drawn.height; ++y)
        for (int x = 0; x < drawn.width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * drawn.width + x) * 3U;
            if (std::equal(&surface.rgb[at], &surface.rgb[at] + 3, &under[at]))
                continue;
            uint8_t* to = canvas_pixel(drawn.x + x, drawn.y + y);
            std::copy_n(&surface.rgb[at], 3, to);
            to[3] = 255;
        }
}

void paint_game_files_glyph(Painter& painter, gf::Glyph glyph, Area area, Rgba colour) {
    const float size = std::min(area.width, area.height);
    if (glyph == gf::Glyph::none || size <= 0.0F)
        return;
    // Marks are drawn on a 24-unit square centred in the area.
    const float unit = size / 24.0F;
    const float ox = area.x + (area.width - size) * 0.5F;
    const float oy = area.y + (area.height - size) * 0.5F;
    const auto at = [&](float x, float y) { return Spot{ox + x * unit, oy + y * unit}; };
    const float w = std::max(1.6F * unit, 1.1F);
    const auto line = [&](float x0, float y0, float x1, float y1) {
        painter.stroke_line(at(x0, y0), at(x1, y1), w, colour);
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
    constexpr float pi = std::numbers::pi_v<float>;
    switch (glyph) {
    case gf::Glyph::none:
        return;
    case gf::Glyph::folder:
        path(
            {{3.0F, 6.0F},
             {9.5F, 6.0F},
             {11.5F, 8.5F},
             {21.0F, 8.5F},
             {21.0F, 19.0F},
             {3.0F, 19.0F}},
            true
        );
        line(3.0F, 11.0F, 21.0F, 11.0F);
        return;
    case gf::Glyph::device:
        // A screen on its stand.
        path({{3.0F, 4.5F}, {21.0F, 4.5F}, {21.0F, 16.0F}, {3.0F, 16.0F}}, true);
        line(12.0F, 16.0F, 12.0F, 19.5F);
        line(7.5F, 19.5F, 16.5F, 19.5F);
        return;
    case gf::Glyph::disk:
        // A diskette: its body with the cut corner, the shutter and the label.
        path({{4.0F, 4.0F}, {17.0F, 4.0F}, {20.0F, 7.0F}, {20.0F, 20.0F}, {4.0F, 20.0F}}, true);
        path({{8.0F, 4.0F}, {8.0F, 9.0F}, {15.0F, 9.0F}, {15.0F, 4.0F}}, false);
        path({{7.5F, 20.0F}, {7.5F, 14.0F}, {16.5F, 14.0F}, {16.5F, 20.0F}}, false);
        return;
    case gf::Glyph::clock:
        painter.outline_circle(at(12.0F, 12.0F), 8.5F * unit, w, colour);
        path({{12.0F, 7.0F}, {12.0F, 12.0F}, {15.5F, 14.0F}}, false);
        return;
    case gf::Glyph::warning:
        path({{12.0F, 3.5F}, {21.5F, 20.0F}, {2.5F, 20.0F}}, true);
        line(12.0F, 9.0F, 12.0F, 14.0F);
        painter.fill_circle(at(12.0F, 17.0F), 1.3F * unit, colour);
        return;
    case gf::Glyph::check:
        path({{4.5F, 12.5F}, {9.5F, 17.5F}, {19.5F, 6.5F}}, false);
        return;
    case gf::Glyph::cross:
        line(6.0F, 6.0F, 18.0F, 18.0F);
        line(18.0F, 6.0F, 6.0F, 18.0F);
        return;
    case gf::Glyph::dash:
        line(6.0F, 12.0F, 18.0F, 12.0F);
        return;
    case gf::Glyph::info:
        painter.outline_circle(at(12.0F, 12.0F), 8.5F * unit, w, colour);
        painter.fill_circle(at(12.0F, 7.8F), 1.3F * unit, colour);
        line(12.0F, 11.0F, 12.0F, 16.5F);
        return;
    case gf::Glyph::stop:
        painter.outline_circle(at(12.0F, 12.0F), 8.5F * unit, w, colour);
        painter.fill_rounded_rect(
            Area{ox + 8.5F * unit, oy + 8.5F * unit, 7.0F * unit, 7.0F * unit}, unit, colour
        );
        return;
    case gf::Glyph::play: {
        const std::array<Spot, 3> corners{at(7.0F, 4.5F), at(19.0F, 12.0F), at(7.0F, 19.5F)};
        for (std::size_t i = 0; i < corners.size(); ++i)
            painter.stroke_line(corners[i], corners[(i + 1) % corners.size()], w, colour);
        return;
    }
    case gf::Glyph::trash:
        line(4.0F, 6.5F, 20.0F, 6.5F);
        path({{9.5F, 6.5F}, {9.5F, 4.0F}, {14.5F, 4.0F}, {14.5F, 6.5F}}, false);
        path({{6.0F, 6.5F}, {7.0F, 20.0F}, {17.0F, 20.0F}, {18.0F, 6.5F}}, false);
        line(10.0F, 10.0F, 10.0F, 16.5F);
        line(14.0F, 10.0F, 14.0F, 16.5F);
        return;
    case gf::Glyph::refresh:
        // Most of a circle, with the arrowhead at its open end.
        painter.stroke_arc(at(12.0F, 12.0F), 8.0F * unit, pi * 0.15F, pi * 0.78F, w, colour);
        path({{18.6F, 3.8F}, {19.4F, 8.6F}, {14.6F, 8.9F}}, false);
        return;
    case gf::Glyph::plus:
        line(12.0F, 4.5F, 12.0F, 19.5F);
        line(4.5F, 12.0F, 19.5F, 12.0F);
        return;
    case gf::Glyph::file:
        path({{6.0F, 3.5F}, {14.0F, 3.5F}, {18.5F, 8.0F}, {18.5F, 20.5F}, {6.0F, 20.5F}}, true);
        path({{14.0F, 3.5F}, {14.0F, 8.0F}, {18.5F, 8.0F}}, false);
        return;
    case gf::Glyph::oa:
        // The OA mark the settings dialog shows without the icon.
        paint_game_files_oa_mark(painter, area, renderer::RgbaPicture{});
        return;
    case gf::Glyph::magnifier:
        painter.outline_circle(at(10.0F, 10.0F), 5.5F * unit, w, colour);
        line(14.2F, 14.2F, 20.0F, 20.0F);
        return;
    }
}

bool write_game_files_png(
    const std::filesystem::path& file, const paint::Canvas& canvas, std::string* error
) {
    if (canvas.width <= 0 || canvas.height <= 0 ||
        canvas.rgba.size() !=
            static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height) * 4U) {
        if (error != nullptr)
            *error = "the screen's canvas is empty";
        return false;
    }
    // Every pixel laid over the screen's background, so the picture holds no alpha.
    const gf::Colour back = gf::background_colour;
    std::vector<uint8_t> rows(
        static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height) * 3U
    );
    for (std::size_t pixel = 0, count = rows.size() / 3U; pixel < count; ++pixel) {
        const uint8_t* source = &canvas.rgba[pixel * 4U];
        const uint32_t alpha = source[3];
        const auto over = [alpha](uint8_t colour, uint8_t under) {
            return static_cast<uint8_t>(
                (uint32_t{colour} * alpha + uint32_t{under} * (255U - alpha) + 127U) / 255U
            );
        };
        rows[pixel * 3U + 0U] = over(source[0], back.r);
        rows[pixel * 3U + 1U] = over(source[1], back.g);
        rows[pixel * 3U + 2U] = over(source[2], back.b);
    }
    oa::formats::png::Header header{};
    header.width = static_cast<uint32_t>(canvas.width);
    header.height = static_cast<uint32_t>(canvas.height);
    header.bit_depth = 8;
    header.color_type = oa::formats::png::ColorType::rgb;
    std::vector<uint8_t> bytes;
    if (!oa::formats::png::write(oa::formats::png::Image{header, {}, rows}, &bytes)) {
        if (error != nullptr)
            *error = "the screen's picture could not be encoded";
        return false;
    }
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    output.close();
    if (!output) {
        if (error != nullptr)
            *error = "the picture " + oa::platform::path_to_utf8(file) + " could not be written";
        return false;
    }
    return true;
}

} // namespace oa::app
