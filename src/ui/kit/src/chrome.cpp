// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The window's chrome, drawn through the canvas's primitives. The arithmetic
// is the settings dialog's, moved here unchanged.

#include "oa/ui/kit/chrome.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace oa::ui::kit {

namespace {

/// The columns between the header's note and its version.
constexpr int32_t kVersionGap = 8;

/// The selected nav entry's marker, within the entry.
constexpr int32_t kMarkerOffset = 5;
constexpr int32_t kMarkerWidth = 2;
constexpr int32_t kMarkerHeight = 8;
/// An entry's text column within the entry, and the columns kept clear of its right.
constexpr int32_t kTextOffset = 12;
constexpr int32_t kTextMargin = 4;

bool drawable(const Canvas& canvas) noexcept {
    return can_draw(canvas);
}

/// A smooth line's width, in points, with the header's tracking. The smooth
/// form draws the modern face even when the game's font has glyphs, and
/// that face is wider than the game font the columns were kept for.
///
/// @param canvas a smooth canvas
/// @param text the line
/// @param tracking extra points after each character but the last
/// @return the width; 0 when the modern fonts are not open
int32_t smooth_header_width(const Canvas& canvas, std::string_view text, int32_t tracking) {
    if (text.empty() || canvas.smooth.fonts == nullptr)
        return 0;
    // The modern look measures in points at every scale, a tracked line in
    // its tracked type.
    if (canvas.fonts != nullptr && modern_look(*canvas.fonts))
        return tracked_width(*canvas.fonts, FontRole::regular, text, tracking);
    if (tracking == 0)
        return measure_text(canvas, FontRole::regular, text);
    int32_t width = 0;
    bool any = false;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t bytes = character_bytes(text.substr(at));
        if (bytes == 0)
            break;
        if (any)
            width += tracking;
        width += measure_text(canvas, FontRole::regular, text.substr(at, bytes));
        any = true;
        at += bytes;
    }
    return width;
}

/// The title's right column: the columns kept for it, or, when the modern
/// fonts stand in for the game's font wholly, the wider of that and the
/// width they draw. A smooth canvas is measured apart from this, in
/// draw_header, because the game font still has glyphs there.
int32_t title_right(const Canvas& canvas, const Rect& title, const HeaderLook& look) {
    const bool modern_only =
        canvas.fonts != nullptr && !std::any_of(
                                       canvas.fonts->regular.font.glyphs.begin(),
                                       canvas.fonts->regular.font.glyphs.end(),
                                       [](const auto& glyph) { return glyph.has_value(); }
                                   );
    if (!modern_only)
        return title.x + title.width;
    return std::max(
        title.x + title.width,
        title.x +
            modern_tracked_width(
                *canvas.fonts, FontRole::regular, look.title, look.tracking, canvas.placement.scale
            )
    );
}

void draw_line(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Align align,
    Colour ink,
    int32_t tracking
) {
    // A crisp canvas draws with the game's fonts. A smooth one draws with
    // its own, so a header still has its title when those are the only fonts.
    if (!drawable(canvas) || text.empty())
        return;
    if (canvas.smooth.painter == nullptr && canvas.fonts == nullptr)
        return;
    box_text(canvas, role, text, box, align, ink, tracking);
}

} // namespace

void draw_window_face(const Canvas& canvas, const Rect& whole) {
    if (!drawable(canvas))
        return;
    fill_rect(canvas, whole, colour::panel);
}

void draw_window_edge(const Canvas& canvas, const Rect& whole) {
    if (!drawable(canvas))
        return;
    bevel_rect(canvas, whole, colour::edge_light, colour::edge_dark);
}

void draw_header(const Canvas& canvas, const HeaderLook& look) {
    if (!drawable(canvas))
        return;
    const int32_t edge = compact_metrics.edge;
    const int32_t header_top = edge;
    const int32_t header_height = look.height > 0 ? look.height : compact_metrics.header_height;
    const int32_t inner = look.width - 2 * edge;
    fill_rect(canvas, {edge, header_top, inner, header_height}, colour::band);
    rule_rect(canvas, {edge, header_top + header_height, inner, 1}, colour::rule);
    // A band of its own height keeps the mark in its middle.
    const Rect mark{
        compact_metrics.padding,
        look.height > 0 ? header_top + (header_height - compact_metrics.mark_side) / 2
                        : compact_metrics.mark_top,
        compact_metrics.mark_side,
        compact_metrics.mark_side,
    };
    if (look.mark)
        draw_header_mark(canvas, mark);
    // On the smooth form the modern face is wider than the game font the
    // columns were kept for, and in the modern look Barlow Condensed at a
    // larger class's title size is too. Measure that face, or the second
    // word starts inside the title ("Open AnnihilationLibrary"). A crisp
    // canvas in the classic look keeps the columns it was given.
    const bool smooth = canvas.smooth.painter != nullptr;
    const bool measured = smooth || (canvas.fonts != nullptr && modern_look(*canvas.fonts));
    const auto drawn_width = [&canvas, &look](std::string_view text) {
        if (canvas.fonts != nullptr && modern_look(*canvas.fonts))
            return tracked_width(*canvas.fonts, FontRole::regular, text, look.tracking);
        return smooth_header_width(canvas, text, look.tracking);
    };
    const int32_t title_span =
        measured ? std::max(look.title_width, drawn_width(look.title)) : look.title_width;
    const int32_t second_span =
        measured ? std::max(look.second_width, drawn_width(look.second)) : look.second_width;
    const int32_t title_left =
        look.mark ? mark.x + mark.width + compact_metrics.header_gap : compact_metrics.padding;
    const Rect title{
        title_left,
        header_top,
        title_span,
        header_height,
    };
    draw_line(
        canvas, FontRole::regular, look.title, title, Align::left, colour::text, look.tracking
    );
    const int32_t drawn_right = measured ? title.x + title.width : title_right(canvas, title, look);
    // With no mark and no title the second word takes the title's place.
    const bool second_first = !look.mark && look.title.empty();
    const Rect second{
        second_first ? title_left : drawn_right + compact_metrics.header_gap,
        header_top,
        second_span,
        header_height,
    };
    draw_line(
        canvas, FontRole::regular, look.second, second, Align::left, colour::hint, look.tracking
    );
    const int32_t content_right = look.width - edge - compact_metrics.padding - look.right_inset;
    const Rect version{
        content_right - look.version_width, header_top, look.version_width, header_height
    };
    draw_line(canvas, FontRole::small, look.version, version, Align::right, colour::quiet, 0);
    if (look.note.empty() || canvas.fonts == nullptr)
        return;
    const int32_t version_left =
        version.x + version.width - measure_text(canvas, FontRole::small, look.version);
    const int32_t note_left = second.x + second.width + compact_metrics.header_gap;
    const Rect note{
        note_left,
        header_top,
        version_left - kVersionGap - note_left,
        header_height,
    };
    draw_line(canvas, FontRole::small, look.note, note, Align::right, colour::lock, 0);
}

void draw_footer_band(const Canvas& canvas, int32_t width, int32_t rule_row) {
    if (!drawable(canvas))
        return;
    const int32_t inner = width - 2 * compact_metrics.edge;
    rule_rect(canvas, {compact_metrics.edge, rule_row, inner, 1}, colour::rule);
    fill_rect(
        canvas,
        {compact_metrics.edge, rule_row + 1, inner, compact_metrics.footer_height},
        colour::band
    );
}

void draw_nav(const Canvas& canvas, const NavLook& look) {
    if (!drawable(canvas))
        return;
    if (look.area.width > 0 && look.area.height > 0) {
        fill_rect(canvas, look.area, colour::list);
        rule_rect(canvas, {look.rule_column, look.area.y, 1, look.area.height}, colour::rule);
    }
    if (look.divider.has_value())
        rule_rect(canvas, *look.divider, colour::rule);
    for (const NavEntry& entry : look.entries) {
        Colour ink = colour::list_text;
        const int32_t radius = control_radius_for(entry.rect.height);
        if (entry.selected) {
            fill_corner_rect(canvas, entry.rect, radius, colour::list_selected);
            fill_rect(
                canvas,
                {entry.rect.x + kMarkerOffset,
                 entry.rect.y + (entry.rect.height - kMarkerHeight) / 2,
                 kMarkerWidth,
                 kMarkerHeight},
                colour::accent
            );
            ink = colour::text;
        } else if (entry.hovered) {
            fill_corner_rect(canvas, entry.rect, radius, colour::hover);
            ink = colour::text;
        }
        Rect caption{
            entry.rect.x + kTextOffset,
            entry.rect.y,
            entry.rect.width - kTextOffset - kTextMargin,
            entry.rect.height,
        };
        if (entry.tag.has_value()) {
            const int32_t side = std::max(compact_metrics.regular_line - 4, int32_t{0});
            const Rect icon{
                caption.x,
                entry.rect.y + (entry.rect.height - side) / 2,
                side,
                side,
            };
            draw_tag_icon(canvas, icon, *entry.tag, ink);
            const int32_t shift = side + compact_metrics.list_row_gap;
            caption.x += shift;
            caption.width = std::max(caption.width - shift, int32_t{0});
        }
        draw_line(canvas, FontRole::regular, entry.caption, caption, Align::left, ink, 0);
        if (entry.focused)
            draw_focus_ring(canvas, entry.rect, false);
    }
}

void add_nav(
    DisplayList& list,
    const NavLook& look,
    std::span<const ControlId> ids,
    std::span<const std::string> names
) {
    for (std::size_t index = 0; index < look.entries.size(); ++index) {
        const NavEntry& entry = look.entries[index];
        Item item;
        item.role = Role::nav;
        item.rect = entry.rect;
        item.text = entry.caption;
        item.state.hovered = entry.hovered;
        item.state.focused = entry.focused;
        item.state.selected = entry.selected;
        // The first item draws the whole list. A later item keeps its control,
        // and an empty look, so its caption is not drawn again over the first.
        if (index == 0)
            item.look = look;
        else
            item.look = NavLook{};
        list.items.push_back(std::move(item));

        Control control;
        control.id = index < ids.size() ? ids[index] : no_control;
        control.rect = entry.rect;
        control.enabled = true;
        control.name = index < names.size() ? names[index] : std::string{};
        control.kind = ControlKind::tab;
        control.steps = false;
        control.checked = entry.selected;
        control.text = entry.caption;
        list.controls.push_back(std::move(control));
    }
}

void draw_heading(const Canvas& canvas, const Rect& rect, const std::string& text) {
    draw_line(
        canvas,
        FontRole::small,
        text,
        rect,
        Align::left,
        colour::quiet,
        compact_metrics.heading_tracking
    );
}

void draw_row_frame(const Canvas& canvas, const RowFrame& row) {
    if (!drawable(canvas))
        return;
    rule_rect(canvas, {row.left, row.top, row.width, 1}, colour::rule);
    draw_line(canvas, FontRole::regular, row.label_text, row.label, Align::left, colour::text, 0);
    const std::size_t lines = std::min(row.hints.size(), row.hint_texts.size());
    for (std::size_t line = 0; line < lines; ++line) {
        const bool notice = line < row.hint_notices.size() && row.hint_notices[line];
        Canvas target = canvas;
        if (line < row.hint_clips.size() && row.hint_clips[line].width > 0 &&
            row.hint_clips[line].height > 0)
            target = clipped(canvas, row.hint_clips[line]);
        draw_line(
            target,
            FontRole::small,
            row.hint_texts[line],
            row.hints[line],
            Align::left,
            notice ? colour::lock : colour::hint,
            0
        );
    }
}

void draw_locked_fade(const Canvas& canvas, const Rect& rect) {
    if (!drawable(canvas))
        return;
    blend_rect(canvas, rect, colour::panel, locked_fade);
}

void draw_rule(const Canvas& canvas, int32_t left, int32_t row, int32_t width) {
    if (!drawable(canvas))
        return;
    rule_rect(canvas, {left, row, width, 1}, colour::rule);
}

} // namespace oa::ui::kit
